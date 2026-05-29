#include "Common.hlsl"


RWTexture2D<float4> ShadowResult : register(u0);
// Phase 2b — current-frame per-pixel M (effective sample count). Lives in
// a separate single-channel UAV because ShadowResult's 4 RGBA32F slots
// are spoken for (sun / idx / W / vis). The shader writes M_eff here
// and the host copies it to ShadowReservoirMPrev for next frame.
RWTexture2D<float> ShadowReservoirM : register(u1);
RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
ByteAddressBuffer vertices : register(t3);
ByteAddressBuffer indices : register(t4);
Texture2D AlbedoTex : register(t5);
ByteAddressBuffer InstanceProperty : register(t6);
Texture2D GeoNormalTex : register(t7);
Texture3D RayNoiseBlueNoiseSource : register(t8);
// ReSTIR Phase 2 — previous-frame reservoir cache. .gba carries
// (lightIdx, weight, visibility); .r is the previous frame's sun
// visibility (unused by ReSTIR here). Sample at the motion-reprojected
// pixel to combine with the current frame's RIS choice.
Texture2D ShadowReservoirPrev : register(t9);
Texture2D VelocityTex : register(t10);
Texture2D ShadowReservoirMPrev : register(t11);


cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4x4 InvProjMatrix;
    float4 ProjectionParams;
    float4 LightDir;
    float ShadowLightRadius;
    uint ShadowSampleCount;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    uint NoiseMode;
    // 0 = Option A channel-pack (sun in R, top-3 lights in GBA),
    // 1 = ReSTIR Phase 1 single-light reservoir (sun in R, G=lightIdx,
    //     B=lightWeight, A=visibility). The two modes write different
    //     ShadowBuffer semantics; LightingPS branches on the same flag.
    uint ShadowMode;
    uint ShadowedPointLightCount; // 0..3 (Option A) or 0..MAX_SHADOWED_PT_LIGHTS (ReSTIR)
    uint _padding2;
    // Must match Corona::MaxPointLights in Corona.h. Option A reads only
    // the first 3 entries; ReSTIR iterates all valid entries.
    #define MAX_SHADOWED_PT_LIGHTS 128
    float4 ShadowedPointLights[MAX_SHADOWED_PT_LIGHTS];
    // RIS weights for ReSTIR. x = candidate weight (luma * intensity).
    // y/z/w unused for now (room for distance hints, history flags).
    float4 ShadowedPointLightWeights[MAX_SHADOWED_PT_LIGHTS];
};
SamplerState sampleWrap : register(s0);


float3 linearToSrgb(float3 c)
{
    // Based on http://chilliant.blogspot.com/2012/08/srgb-approximations-for-hlsl.html
    float3 sq1 = sqrt(c);
    float3 sq2 = sqrt(sq1);
    float3 sq3 = sqrt(sq2);
    float3 srgb = 0.662002687 * sq1 + 0.684122060 * sq2 - 0.323583601 * sq3 - 0.0225411470 * c;
    return srgb;
}

struct RayPayload
{
    uint bHit;
    float3 _padding;
};

static const uint RT_SHADOW_RAY_FLAGS =
    RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
    RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
    RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

float3 offset_ray(float3 p, float3 n)
{
    return p + n * (1.0f / 256.0f);
}

[shader("raygeneration")]
void rayGen()
{
    uint3 launchIndex = DispatchRaysIndex();
    uint3 launchDim = DispatchRaysDimensions();
    uint2 pixelPos = launchIndex.xy;

    float2 launchSize = float2(max(launchDim.x, 1u), max(launchDim.y, 1u));
    float2 uv = (float2(pixelPos) + float2(0.5f, 0.5f)) / launchSize;
	float deviceDepth = DepthTex.SampleLevel(sampleWrap, uv, 0).x;
    if (deviceDepth >= 0.999999f)
    {
        ShadowResult[pixelPos] = float4(1.0f.xxx, 1.0f);
        return;
    }

	float2 screenPosition = uv * 2.0f - 1.0f;
	screenPosition.y = -screenPosition.y;
    float3 viewPosition = GetViewPosition(deviceDepth, screenPosition, InvProjMatrix);
	float3 worldPos = mul(float4(viewPosition, 1.0f), InvViewMatrix).xyz;

	float3 geoNormal = CommonSafeNormalize(GeoNormalTex.SampleLevel(sampleWrap, uv, 0).xyz, float3(0.0f, 1.0f, 0.0f));
	float3 worldNormal = CommonSafeNormalize(WorldNormalTex.SampleLevel(sampleWrap, uv, 0).xyz, geoNormal);
    float3 cameraWorld = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
    float3 surfaceToCamera = CommonSafeNormalize(cameraWorld - worldPos, -worldNormal);
    if (dot(geoNormal, surfaceToCamera) < 0.0f)
        geoNormal = -geoNormal;
    if (dot(worldNormal, geoNormal) < 0.0f)
        worldNormal = -worldNormal;

    float3 traceNormal = CommonSafeNormalize(geoNormal + worldNormal * 0.25f, geoNormal);
    float3 baseLightDir = CommonSafeNormalize(LightDir.xyz, float3(0.0f, 1.0f, 0.0f));
    float visibility = 0.0f;
    const uint kMaxShadowSamples = 16;
    uint sampleCount = min(max(ShadowSampleCount, 1), kMaxShadowSamples);
    // Bias scales with distance from camera so far-floor pixels in
    // sponza-scale (~thousands of world units) don't self-shadow against
    // their own geometry. A fixed 0.5 unit bias was fine in tight scenes
    // but flickered hard on the bench-mode ground plane far from camera.
    // Use both a linear-in-distance term and a quadratic term so really
    // distant pixels (10k+ units) get enough headroom to clear the same
    // surface they were sampled from after BVH leaf-level rounding.
    const float distanceToCamera = length(worldPos - cameraWorld);
    float normalBias = max(0.5f,
        distanceToCamera * 0.003f + distanceToCamera * distanceToCamera * 5e-8f);

    [loop]
    for (uint sampleIndex = 0; sampleIndex < kMaxShadowSamples; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
            break;

        uint2 noisePixel = pixelPos + uint2(sampleIndex * 17u, sampleIndex * 31u);
        uint noiseFrame = FrameCounter + sampleIndex * 13u;
        float2 randUV = GenerateRaySample2D(RayNoiseBlueNoiseSource, noisePixel, noiseFrame, BlueNoiseOffsetStride, NoiseMode);
        float3 rayDir = SampleDirectionalLightSphereCap(baseLightDir, ShadowLightRadius, randUV);
        float3 rayBiasNormal = dot(traceNormal, rayDir) < 0.0f ? -traceNormal : traceNormal;

        RayDesc ray;
        ray.Origin = worldPos + rayBiasNormal * normalBias;
        ray.Direction = rayDir;
        ray.TMin = max(0.05f, normalBias * 0.25f);
        ray.TMax = 100000;

        RayPayload payload;
        payload.bHit = 1u;
        payload._padding = 0.0f.xxx;
        TraceRay(gRtScene,
            RT_SHADOW_RAY_FLAGS,
            0xFF, 0, 0, 0, ray, payload);

        visibility += payload.bHit == 0u ? 1.0f : 0.0f;
    }

    visibility /= sampleCount;

    // Helper: cast a single occlusion ray to a world-space point light
    // position. Returns 1.0 if unoccluded, 0.0 if shadowed (skipping rays
    // for back-facing surfaces or out-of-range lights). Shared between
    // Option A's fixed channel pack and ReSTIR's single-light reservoir.
    #define COMPUTE_POINT_LIGHT_VIS(visOut, lightPos, lightRadius)         \
    {                                                                      \
        float3 _toLight = (lightPos) - worldPos;                           \
        float _distToLight = length(_toLight);                             \
        if (_distToLight > (lightRadius) || _distToLight < 1.0e-3f)        \
        {                                                                  \
            visOut = 1.0f; /* out of range — direct attenuation handles */ \
        }                                                                  \
        else                                                               \
        {                                                                  \
            float3 _lightDir = _toLight / _distToLight;                    \
            if (dot(worldNormal, _lightDir) <= 0.0f)                       \
            {                                                              \
                visOut = 0.0f; /* back-facing → NdotL=0 anyway */          \
            }                                                              \
            else                                                           \
            {                                                              \
                float3 _bias = dot(traceNormal, _lightDir) < 0.0f          \
                    ? -traceNormal : traceNormal;                          \
                RayDesc _ray;                                              \
                _ray.Origin = worldPos + _bias * normalBias;               \
                _ray.Direction = _lightDir;                                \
                _ray.TMin = max(0.05f, normalBias * 0.25f);                \
                _ray.TMax = max(_distToLight - max(normalBias * 0.5f, 0.05f), _ray.TMin + 0.05f); \
                RayPayload _p;                                             \
                _p.bHit = 1u;                                              \
                _p._padding = 0.0f.xxx;                                    \
                TraceRay(gRtScene, RT_SHADOW_RAY_FLAGS,                    \
                    0xFF, 0, 0, 0, _ray, _p);                              \
                visOut = (_p.bHit == 0u) ? 1.0f : 0.0f;                    \
            }                                                              \
        }                                                                  \
    }

    float4 outShadow = float4(visibility, 1.0f, 1.0f, 1.0f);

    if (ShadowMode == 0u)
    {
        // -------------- Option A: channel-pack first 3 lights ----------
        [loop]
        for (uint lightIdx = 0; lightIdx < 3u; ++lightIdx)
        {
            if (lightIdx >= ShadowedPointLightCount)
                break;
            float pointVis = 1.0f;
            COMPUTE_POINT_LIGHT_VIS(pointVis,
                ShadowedPointLights[lightIdx].xyz,
                max(ShadowedPointLights[lightIdx].w, 0.01f));
            if (lightIdx == 0u) outShadow.g = pointVis;
            else if (lightIdx == 1u) outShadow.b = pointVis;
            else                     outShadow.a = pointVis;
        }
        // Option A doesn't use the M side buffer — zero it so a later
        // runtime toggle into ReSTIR doesn't pick up stale temporal M.
        ShadowReservoirM[pixelPos] = 0.0f;
    }
    else
    {
        // -------------- ReSTIR Phase 1: per-pixel RIS over all lights --
        // Pick one light per pixel proportional to luma*intensity (target
        // PDF). NO temporal / spatial reuse yet — that's Phase 2/3.
        // Outputs to ShadowBuffer.gba :
        //   G = chosen light index (cast back to uint in LightingPS),
        //   B = candidate weight ratio (W / pdf) for unbiased estimate,
        //   A = visibility of the chosen light.
        uint chosenIdx = 0xFFFFFFFFu;
        float chosenWeight = 0.0f;
        float weightSum = 0.0f;

        [loop]
        for (uint candIdx = 0; candIdx < (uint)MAX_SHADOWED_PT_LIGHTS; ++candIdx)
        {
            if (candIdx >= ShadowedPointLightCount)
                break;
            float3 candPos = ShadowedPointLights[candIdx].xyz;
            float candRadius = max(ShadowedPointLights[candIdx].w, 0.01f);
            float candLuma = ShadowedPointLightWeights[candIdx].x;
            if (candLuma <= 0.0f)
                continue;

            // Per-candidate unshadowed contribution estimate: luma /
            // (distance² + range² damping). Higher means better candidate.
            float3 toCand = candPos - worldPos;
            float distSq = max(dot(toCand, toCand), 1.0e-4f);
            float dist = sqrt(distSq);
            float rangeAtten = saturate(1.0f - dist / candRadius);
            float NdotL = saturate(dot(worldNormal, toCand) / max(dist, 1.0e-3f));
            float targetPdf = candLuma * rangeAtten * rangeAtten * NdotL / max(distSq * 0.0001f, 1.0f);
            if (targetPdf <= 0.0f)
                continue;

            weightSum += targetPdf;

            // Reservoir update: keep with probability targetPdf/weightSum.
            // Use a tiny LCG seeded by pixelPos + FrameCounter + candIdx.
            uint seed = (pixelPos.x * 1973u + pixelPos.y * 9277u + FrameCounter * 26699u + candIdx * 49u) * 6151u;
            seed ^= seed >> 13u;
            seed *= 0x5bd1e995u;
            seed ^= seed >> 15u;
            float u = (seed & 0x00FFFFFFu) / 16777216.0f;
            if (u * weightSum <= targetPdf)
            {
                chosenIdx = candIdx;
                chosenWeight = targetPdf;
            }
        }

        // Track effective sample count M for the unbiased estimator:
        // W = W_sum / (p_chosen * M). Phase 2b stores M in a side buffer
        // (ShadowReservoirM) so temporal reuse can carry it across
        // frames; M_eff grows over many frames up to a cap, dropping
        // variance ~1/M. Cap chosen to bound the influence of stale
        // samples after motion / disocclusion.
        const float kMaxM = 20.0f;
        float M_eff = (chosenIdx == 0xFFFFFFFFu) ? 0.0f : 1.0f;

        // ReSTIR Phase 3 — spatial reuse over a small neighbourhood of
        // the previous frame's reservoirs. Reading prev (not curr) keeps
        // the pass single-dispatch — the spatial samples are one frame
        // stale but that's a cheap cost vs the variance reduction. The
        // network of 3 neighbours + the centre temporal sample below
        // effectively raises M_eff per pixel without an extra compute
        // pass, which is what DLSS RR needs to denoise cleanly.
        const int   kSpatialSamples = 3;
        const float kSpatialRadius  = 8.0f; // pixels
        [unroll]
        for (int sIdx = 0; sIdx < kSpatialSamples; ++sIdx)
        {
            uint sSeed = (pixelPos.x * 5237u + pixelPos.y * 6311u + FrameCounter * 17u + (uint)sIdx * 991u) * 1597u;
            sSeed ^= sSeed >> 13u; sSeed *= 0x5bd1e995u; sSeed ^= sSeed >> 15u;
            float rA = (sSeed & 0xFFFFu) / 65535.0f;
            sSeed = sSeed * 1664525u + 1013904223u;
            float rB = (sSeed & 0xFFFFu) / 65535.0f;
            const float angle = rA * 6.28318530717958647692f;
            const float radius = sqrt(rB) * kSpatialRadius;
            const int2 ofs = int2(round(cos(angle) * radius), round(sin(angle) * radius));
            const float2 spatialUV = uv + (float2(ofs) / launchSize);
            if (spatialUV.x < 0.0f || spatialUV.x > 1.0f || spatialUV.y < 0.0f || spatialUV.y > 1.0f)
                continue;
            const float4 sp = ShadowReservoirPrev.SampleLevel(sampleWrap, spatialUV, 0);
            const uint  spIdx   = (uint)(sp.g + 0.5f);
            const float spRatio = sp.b;
            const float spM     = ShadowReservoirMPrev.SampleLevel(sampleWrap, spatialUV, 0).x;
            if (spIdx >= ShadowedPointLightCount || spRatio <= 0.0f || spM <= 0.0f)
                continue;
            const float3 cPos = ShadowedPointLights[spIdx].xyz;
            const float  cRad = max(ShadowedPointLights[spIdx].w, 0.01f);
            const float  cLum = ShadowedPointLightWeights[spIdx].x;
            const float3 toC = cPos - worldPos;
            const float  dSq = max(dot(toC, toC), 1.0e-4f);
            const float  d   = sqrt(dSq);
            const float  raC = saturate(1.0f - d / cRad);
            const float  NLc = saturate(dot(worldNormal, toC) / max(d, 1.0e-3f));
            const float  tpdfC = cLum * raC * raC * NLc / max(dSq * 0.0001f, 1.0f);
            const float  spW   = spRatio * tpdfC * spM;
            if (spW <= 0.0f) continue;
            weightSum += spW;
            uint sSeed2 = sSeed * 6151u + 0xdeadbeefu;
            sSeed2 ^= sSeed2 >> 13u; sSeed2 *= 0x5bd1e995u; sSeed2 ^= sSeed2 >> 15u;
            const float ru = (sSeed2 & 0x00FFFFFFu) / 16777216.0f;
            if (ru * weightSum <= spW)
            {
                chosenIdx    = spIdx;
                chosenWeight = tpdfC;
            }
            M_eff = min(M_eff + spM * 0.5f, kMaxM); // half-weight neighbours
        }

        // ReSTIR Phase 2 — temporal reuse. Sample the previous frame's
        // reservoir at the motion-reprojected pixel and RIS-combine into
        // the current pixel's reservoir.
        const float2 velocity = VelocityTex.SampleLevel(sampleWrap, uv, 0).xy;
        const float2 prevUV = uv - velocity;
        if (prevUV.x >= 0.0f && prevUV.x <= 1.0f && prevUV.y >= 0.0f && prevUV.y <= 1.0f)
        {
            const float4 prev = ShadowReservoirPrev.SampleLevel(sampleWrap, prevUV, 0);
            const uint prevIdx = (uint)(prev.g + 0.5f);
            const float prevRatio = prev.b;
            const float prevM = ShadowReservoirMPrev.SampleLevel(sampleWrap, prevUV, 0).x;
            if (prevIdx < (uint)MAX_SHADOWED_PT_LIGHTS && prevIdx < ShadowedPointLightCount && prevRatio > 0.0f && prevM > 0.0f)
            {
                const float3 candPos = ShadowedPointLights[prevIdx].xyz;
                const float candRadius = max(ShadowedPointLights[prevIdx].w, 0.01f);
                const float candLuma = ShadowedPointLightWeights[prevIdx].x;
                const float3 toCand = candPos - worldPos;
                const float distSq = max(dot(toCand, toCand), 1.0e-4f);
                const float dist = sqrt(distSq);
                const float rangeAtten = saturate(1.0f - dist / candRadius);
                const float NdotL = saturate(dot(worldNormal, toCand) / max(dist, 1.0e-3f));
                const float targetPdfPrev = candLuma * rangeAtten * rangeAtten * NdotL / max(distSq * 0.0001f, 1.0f);
                // RIS combine: prev sample's W-contribution at this pixel
                // is prev.W_at_curr * prev.M = (prevRatio * targetPdfPrev)
                // weighted by the prev sample count.
                const float prevWeight = prevRatio * targetPdfPrev * prevM;
                if (prevWeight > 0.0f)
                {
                    weightSum += prevWeight;
                    uint seed2 = (pixelPos.x * 31337u + pixelPos.y * 6151u + FrameCounter * 12347u + 0xabcd1234u);
                    seed2 ^= seed2 >> 13u;
                    seed2 *= 0x5bd1e995u;
                    seed2 ^= seed2 >> 15u;
                    const float u2 = (seed2 & 0x00FFFFFFu) / 16777216.0f;
                    if (u2 * weightSum <= prevWeight)
                    {
                        chosenIdx = prevIdx;
                        chosenWeight = targetPdfPrev;
                    }
                    // Accumulate prev's M (capped) so subsequent frames'
                    // variance reduction is proportional to total sample
                    // history. Without the cap, stale samples after fast
                    // motion would over-weight.
                    M_eff = min(M_eff + prevM, kMaxM);
                }
            }
        }

        if (chosenIdx != 0xFFFFFFFFu && M_eff > 0.0f)
        {
            float pointVis = 1.0f;
            COMPUTE_POINT_LIGHT_VIS(pointVis,
                ShadowedPointLights[chosenIdx].xyz,
                max(ShadowedPointLights[chosenIdx].w, 0.01f));
            // Unbiased estimator weight: W = W_sum / (p_chosen * M).
            // Firefly clamp keeps a single low-pdf sample from spiking
            // many frames of accumulated weight into one pixel — that's
            // what the user saw as "camera stops and direct light gets
            // way too bright with revealed noise". The clamp bounds the
            // single-sample contribution; over many frames the bias
            // from clamping is tiny but the variance reduction is
            // dramatic and DLSS RR can finally denoise the result.
            const float kFireflyCap = 30.0f;
            float ratio = weightSum / (max(chosenWeight, 1.0e-6f) * M_eff);
            ratio = clamp(ratio, 0.0f, kFireflyCap);
            outShadow.g = (float)chosenIdx;
            outShadow.b = ratio;
            outShadow.a = pointVis;
            ShadowReservoirM[pixelPos] = M_eff;
        }
        else
        {
            // No candidates — encode sentinel index so LightingPS skips.
            outShadow.g = 255.0f;
            outShadow.b = 0.0f;
            outShadow.a = 1.0f;
            ShadowReservoirM[pixelPos] = 0.0f;
        }
    }

    ShadowResult[pixelPos] = outShadow;

}

[shader("miss")]
void miss(inout RayPayload payload)
{
    // payload.opacity = 0.0;
    payload.bHit = 0u;
    payload._padding = 0.0f.xxx;
}

[shader("anyhit")]
void anyhit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();

    if (!IsAlphaTestedInstance(instanceID, InstanceProperty))
    {
        AcceptHitAndEndSearch();
        return;
    }

    Vertex vertex = GetVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);
    float opacity = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, 5).w;

        // payload.bHit = false;

    if(opacity > 0.10)
    {
        AcceptHitAndEndSearch();
        return;
    }
    
    IgnoreHit();
}
