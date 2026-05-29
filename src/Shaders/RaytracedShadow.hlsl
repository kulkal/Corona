#include "Common.hlsl"


RWTexture2D<float4> ShadowResult : register(u0);
RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
ByteAddressBuffer vertices : register(t3);
ByteAddressBuffer indices : register(t4);
Texture2D AlbedoTex : register(t5);
ByteAddressBuffer InstanceProperty : register(t6);
Texture2D GeoNormalTex : register(t7);
Texture3D RayNoiseBlueNoiseSource : register(t8);


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
    uint ShadowedPointLightCount; // 0..3 (Option A) or 0..8 (ReSTIR)
    uint _padding2;
    // First 3 entries are used by Option A; ReSTIR iterates up to 8.
    float4 ShadowedPointLights[8];
    // RIS weights for ReSTIR. x = candidate weight (luma * intensity).
    // y/z/w unused for now (room for distance hints, history flags).
    float4 ShadowedPointLightWeights[8];
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
        for (uint candIdx = 0; candIdx < 8u; ++candIdx)
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

        if (chosenIdx != 0xFFFFFFFFu)
        {
            float pointVis = 1.0f;
            COMPUTE_POINT_LIGHT_VIS(pointVis,
                ShadowedPointLights[chosenIdx].xyz,
                max(ShadowedPointLights[chosenIdx].w, 0.01f));
            // Unbiased estimator weight: weightSum / targetPdf. The
            // LightingPS consumer multiplies the chosen light's evaluated
            // BRDF by this factor so the result matches the all-lights
            // average in expectation.
            float ratio = weightSum / max(chosenWeight, 1.0e-6f);
            outShadow.g = (float)chosenIdx;
            outShadow.b = ratio;
            outShadow.a = pointVis;
        }
        else
        {
            // No candidates — encode sentinel index so LightingPS skips.
            outShadow.g = 255.0f;
            outShadow.b = 0.0f;
            outShadow.a = 1.0f;
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
