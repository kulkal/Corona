#include "Common.hlsl"
#ifndef RT_SHADOW_INLINE_RAYQUERY
#include "BindlessResources.hlsli"
#endif


RWTexture2D<float4> ShadowResult : register(u0);
// Phase 2b — current-frame per-pixel M (effective sample count). Lives in
// a separate single-channel UAV because ShadowResult's 4 RGBA32F slots
// are spoken for (sun / idx / W / vis). The shader writes M_eff here
// and the host copies it to ShadowReservoirMPrev for next frame.
RWTexture2D<float> ShadowReservoirM : register(u1);
RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
Texture2D GeoNormalTex : register(t7);
Texture3D RayNoiseBlueNoiseSource : register(t8);
// ReSTIR Phase 2 — previous-frame reservoir cache. .gba carries
// (lightIdx, weight, visibility); .r is the previous frame's sun
// visibility (unused by ReSTIR here). Sample at the motion-reprojected
// pixel to combine with the current frame's RIS choice.
Texture2D ShadowReservoirPrev : register(t9);
Texture2D VelocityTex : register(t10);
Texture2D ShadowReservoirMPrev : register(t11);
// Option C disocclusion test inputs — previous frame depth + normal.
// Compared against the current pixel's depth/normal (post motion
// reproject) to reject temporal/spatial samples when the surface
// has changed. Without this gate, the temporal reservoir's chosen
// light keeps haunting the new pixel after motion → shadow ghosting.
Texture2D DepthTexPrev : register(t12);
Texture2D WorldNormalTexPrev : register(t13);
StructuredBuffer<uint> SpatialLightCellKeys : register(t14);
StructuredBuffer<uint> SpatialLightCellMask : register(t15);

struct PointLightParam
{
    float4 PositionAndRadius;
    float4 ColorAndIntensity;
    float4 DirectionAndType;
    float4 SpotConeAndFlags;
};

StructuredBuffer<PointLightParam> PointLightBuffer : register(t16);
#define POINT_LIGHT_GRID_COUNTS_REGISTER t17
#define POINT_LIGHT_GRID_INDICES_REGISTER t18
#include "PointLightGrid.hlsli"


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
    // Runtime-tunable temporal M cap. Replaces the previous shader-side
    // `const float kMaxM = 3.0f`. Set per-frame from the C++
    // `bEnableReSTIRDirectShadow`-mode UI control.
    float ShadowMaxM;
    float SpatialLightCellSize;
    uint SpatialLightHashEntryMask;
    uint SpatialLightMaxProbeSteps;
    uint bUseSpatialLightMask;
    float4 SpatialHashLevelParams;
    // Must match Corona::MaxPointLights in Corona.h. Option A reads only
    // the first 3 entries; ReSTIR iterates all valid entries.
    #define MAX_SHADOWED_PT_LIGHTS 16
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

#ifndef RT_SHADOW_INLINE_RAYQUERY
struct RayPayload
{
    uint bHit;
    float3 _padding;
};
#endif

static const uint RT_SHADOW_RAY_FLAGS =
    RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
    RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
    RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
static const uint RT_SHADOW_RAY_MASK = 0x02u;

bool TraceShadowOccluded(RayDesc ray)
{
#ifdef RT_SHADOW_INLINE_RAYQUERY
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gRtScene, RAY_FLAG_NONE, RT_SHADOW_RAY_MASK, ray);
    q.Proceed();
    return q.CommittedStatus() != COMMITTED_NOTHING;
#else
    RayPayload payload;
    payload.bHit = 1u;
    payload._padding = 0.0f.xxx;
    TraceRay(gRtScene,
        RT_SHADOW_RAY_FLAGS,
        RT_SHADOW_RAY_MASK, 0, 0, 0, ray, payload);
    return payload.bHit != 0u;
#endif
}

uint CandidateMaskForCount(uint count)
{
    count = min(count, (uint)MAX_SHADOWED_PT_LIGHTS);
    return count >= 32u ? 0xffffffffu : ((1u << count) - 1u);
}

float SpatialHashLeveledCellSize(float3 worldPos, out uint level)
{
    float cs = max(SpatialLightCellSize, 1e-3f);
    level = 0u;
    if (SpatialHashLevelParams.x < 0.5f)
        return cs;
    float3 cameraPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
    float dist = length(worldPos - cameraPos);
    float fl = floor(max(log2(max(dist / max(SpatialHashLevelParams.y, 1e-3f), 1.0f)), 0.0f));
    level = (uint)fl;
    return cs * exp2(fl);
}

int3 SpatialHashLevelOffset(uint level)
{
    return int3(level, level, level) * int3(1737, 9277, 4513);
}

uint SpatialLightCellKey(float3 worldPos)
{
    uint level = 0u;
    float cs = SpatialHashLeveledCellSize(worldPos, level);
    int3 cell = int3(floor(worldPos / cs)) + SpatialHashLevelOffset(level);
    uint h = uint(cell.x) * 73856093u;
    h ^= uint(cell.y) * 19349663u;
    h ^= uint(cell.z) * 83492791u;
    h = HashUInt(h);
    return h == 0u ? 1u : h;
}

bool FindSpatialLightSlot(uint key, out uint slot)
{
    uint startSlot = HashUInt(key ^ 0x9e3779b9u) & SpatialLightHashEntryMask;
    uint probeCount = clamp(SpatialLightMaxProbeSteps, 1u, 16u);
    [loop]
    for (uint probeIndex = 0u; probeIndex < 16u; ++probeIndex)
    {
        if (probeIndex >= probeCount)
            break;
        uint candidate = (startSlot + probeIndex) & SpatialLightHashEntryMask;
        uint storedKey = SpatialLightCellKeys[candidate];
        if (storedKey == key)
        {
            slot = candidate;
            return true;
        }
        if (storedKey == 0u)
            break;
    }
    slot = 0u;
    return false;
}

uint LoadSpatialLightMask(float3 worldPos)
{
    uint allMask = CandidateMaskForCount(ShadowedPointLightCount);
    if (bUseSpatialLightMask == 0u)
        return allMask;

    uint slot = 0u;
    if (!FindSpatialLightSlot(SpatialLightCellKey(worldPos), slot))
        return allMask;
    return SpatialLightCellMask[slot] & allMask;
}

bool LoadReSTIRPointLight(uint lightIndex, bool usePointLightGrid, out float3 position, out float radius, out float luma)
{
    position = 0.0f.xxx;
    radius = 0.0f;
    luma = 0.0f;

    if (usePointLightGrid)
    {
        if (lightIndex >= PointLightGridGetPointLightCount())
            return false;
        PointLightParam light = PointLightBuffer[lightIndex];
        if (light.SpotConeAndFlags.w <= 0.5f)
            return false;
        position = light.PositionAndRadius.xyz;
        radius = max(light.PositionAndRadius.w, 0.01f);
        luma =
            max(0.0f, 0.2126f * light.ColorAndIntensity.x + 0.7152f * light.ColorAndIntensity.y + 0.0722f * light.ColorAndIntensity.z) *
            max(0.0f, light.ColorAndIntensity.w);
        return luma > 0.0f;
    }

    if (lightIndex >= ShadowedPointLightCount || lightIndex >= (uint)MAX_SHADOWED_PT_LIGHTS)
        return false;
    position = ShadowedPointLights[lightIndex].xyz;
    radius = max(ShadowedPointLights[lightIndex].w, 0.01f);
    luma = ShadowedPointLightWeights[lightIndex].x;
    return luma > 0.0f;
}

bool LoadDirectPointLightCandidate(
    uint lightIndex,
    bool usePointLightGrid,
    out float3 position,
    out float radius,
    out float luma,
    out bool castsShadow)
{
    position = 0.0f.xxx;
    radius = 0.0f;
    luma = 0.0f;
    castsShadow = true;

    if (usePointLightGrid)
    {
        if (lightIndex >= PointLightGridGetPointLightCount())
            return false;
        PointLightParam light = PointLightBuffer[lightIndex];
        position = light.PositionAndRadius.xyz;
        radius = max(light.PositionAndRadius.w, 0.01f);
        luma =
            max(0.0f, 0.2126f * light.ColorAndIntensity.x + 0.7152f * light.ColorAndIntensity.y + 0.0722f * light.ColorAndIntensity.z) *
            max(0.0f, light.ColorAndIntensity.w);
        castsShadow = light.SpotConeAndFlags.w > 0.5f;
        return luma > 0.0f;
    }

    if (!LoadReSTIRPointLight(lightIndex, false, position, radius, luma))
        return false;
    castsShadow = true;
    return true;
}

bool ReSTIRLightAllowed(
    uint lightIndex,
    bool usePointLightGrid,
    uint gridCellIndex,
    uint gridCandidateCount,
    uint spatialLightMask)
{
    if (usePointLightGrid)
        return PointLightGridCellContainsLightIndex(gridCellIndex, gridCandidateCount, lightIndex);
    return lightIndex < 32u && (spatialLightMask & (1u << lightIndex)) != 0u;
}

float EvaluatePointLightDistanceAttenuation(float distanceSq, float radius)
{
    radius = max(radius, 0.01f);
    const float invRadiusSq = rcp(radius * radius);
    const float normalizedDistSq = saturate(distanceSq * invRadiusSq);
    float rangeAttenuation = saturate(1.0f - normalizedDistSq * normalizedDistSq);
    rangeAttenuation *= rangeAttenuation;
    const float inverseSquareAttenuation = rcp(max(1.0f, distanceSq * 0.0001f));
    return rangeAttenuation * inverseSquareAttenuation;
}

float EvaluateReSTIRPointLightTargetPdf(float luma, float distanceSq, float radius, float nDotL)
{
    const float spatialTerm = max(0.0f, EvaluatePointLightDistanceAttenuation(distanceSq, radius) * saturate(nDotL));
    return max(0.0f, luma) * spatialTerm;
}

float ComputePointLightEndpointBias(float lightRadius, float normalBias)
{
    // Light proxy meshes are filtered from the shadow mask on the host side,
    // so this endpoint guard can stay narrow. A wide guard hides legitimate
    // blockers near the light and shows up as direct-light leaks.
    return max(normalBias * 0.5f, clamp(lightRadius * 0.005f, 0.10f, 20.0f));
}

float ReconstructLinearViewDepth(float2 sampleUv, float deviceDepth)
{
    float2 screenPosition = sampleUv * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;
    return abs(GetViewPosition(deviceDepth, screenPosition, InvProjMatrix).z);
}

bool ReSTIRSurfaceCompatible(float2 sampleUv, float sampleDepth, float3 sampleNormal, float currentLinearDepth, float3 currentNormal)
{
    if (sampleDepth >= 0.999999f)
        return false;

    const float sampleLinearDepth = ReconstructLinearViewDepth(sampleUv, sampleDepth);
    const float depthTolerance = max(2.0f, currentLinearDepth * 0.01f);
    if (abs(sampleLinearDepth - currentLinearDepth) > depthTolerance)
        return false;

    const float3 n = CommonSafeNormalize(sampleNormal, currentNormal);
    return dot(n, currentNormal) > 0.85f;
}

float3 offset_ray(float3 p, float3 n)
{
    return p + n * (1.0f / 256.0f);
}

void ExecuteShadowPass(uint2 pixelPos, uint2 launchDim)
{
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
    const float currentLinearDepth = abs(viewPosition.z);

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
    float visibility = 1.0f;
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

    const bool directionalCastsShadow = LightDir.w > 0.5f;
    if (directionalCastsShadow)
    {
        visibility = 0.0f;
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

            visibility += TraceShadowOccluded(ray) ? 0.0f : 1.0f;
        }

        visibility /= sampleCount;
    }

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
                float _endpointBias = ComputePointLightEndpointBias((lightRadius), normalBias); \
                if (_distToLight <= _endpointBias + _ray.TMin)             \
                {                                                          \
                    visOut = 1.0f;                                         \
                }                                                          \
                else                                                       \
                {                                                          \
                    _ray.TMax = max(_distToLight - _endpointBias, _ray.TMin + 0.05f); \
                    visOut = TraceShadowOccluded(_ray) ? 0.0f : 1.0f;      \
                }                                                          \
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
    {
        // Deterministic finite-light shadows for direct lighting.
        //
        // Direct lighting is too visible to feed as a stochastic 1-light
        // reservoir when DLSS RR is the only denoiser. Match the practical
        // engine-style path instead: pick the strongest local shadow-casting
        // candidates for this pixel and trace those deterministically. The
        // lighting pass still accumulates the local grid list, but only these
        // packed entries modulate finite-light visibility.
        const uint spatialLightMask = LoadSpatialLightMask(worldPos);
        const bool usePointLightGrid = PointLightGridIsEnabled();
        uint gridCellIndex = 0u;
        uint gridCandidateCount = usePointLightGrid ?
            PointLightGridSelectCandidateCount(worldPos, gridCellIndex) :
            0u;
        const uint candCount = usePointLightGrid ?
            min(gridCandidateCount, PointLightGridGetMaxCount()) :
            min(ShadowedPointLightCount, (uint)MAX_SHADOWED_PT_LIGHTS);

        float topScore0 = 0.0f;
        float topScore1 = 0.0f;
        float topScore2 = 0.0f;
        uint topIdx0 = 0xFFFFFFFFu;
        uint topIdx1 = 0xFFFFFFFFu;
        uint topIdx2 = 0xFFFFFFFFu;

        [loop]
        for (uint candidateIndex = 0u; candidateIndex < 128u; ++candidateIndex)
        {
            if (candidateIndex >= candCount)
                break;

            const uint candIdx = usePointLightGrid ?
                PointLightGridLoadLightIndex(gridCellIndex, candidateIndex) :
                candidateIndex;
            if (!usePointLightGrid &&
                !ReSTIRLightAllowed(candIdx, usePointLightGrid, gridCellIndex, gridCandidateCount, spatialLightMask))
                continue;

            float3 candPos = 0.0f.xxx;
            float candRadius = 0.0f;
            float candLuma = 0.0f;
            bool candCastsShadow = false;
            if (!LoadDirectPointLightCandidate(candIdx, usePointLightGrid, candPos, candRadius, candLuma, candCastsShadow) ||
                !candCastsShadow)
                continue;

            float3 toCand = candPos - worldPos;
            float distSq = max(dot(toCand, toCand), 1.0e-4f);
            float dist = sqrt(distSq);
            float nDotL = saturate(dot(worldNormal, toCand) / max(dist, 1.0e-3f));
            float score = EvaluateReSTIRPointLightTargetPdf(candLuma, distSq, candRadius, nDotL);
            if (score <= 0.0f)
                continue;

            if (score > topScore0)
            {
                topScore2 = topScore1; topIdx2 = topIdx1;
                topScore1 = topScore0; topIdx1 = topIdx0;
                topScore0 = score; topIdx0 = candIdx;
            }
            else if (score > topScore1)
            {
                topScore2 = topScore1; topIdx2 = topIdx1;
                topScore1 = score; topIdx1 = candIdx;
            }
            else if (score > topScore2)
            {
                topScore2 = score; topIdx2 = candIdx;
            }
        }

        float packed0 = 0.0f;
        float packed1 = 0.0f;
        float packed2 = 0.0f;
        if (topIdx0 != 0xFFFFFFFFu)
        {
            float3 pos = 0.0f.xxx;
            float radius = 0.0f;
            float luma = 0.0f;
            bool castsShadow = false;
            if (LoadDirectPointLightCandidate(topIdx0, usePointLightGrid, pos, radius, luma, castsShadow) && castsShadow)
            {
                float vis = 1.0f;
                COMPUTE_POINT_LIGHT_VIS(vis, pos, radius);
                packed0 = (vis >= 0.5f ? 1.0f : -1.0f) * ((float)topIdx0 + 1.0f);
            }
        }
        if (topIdx1 != 0xFFFFFFFFu)
        {
            float3 pos = 0.0f.xxx;
            float radius = 0.0f;
            float luma = 0.0f;
            bool castsShadow = false;
            if (LoadDirectPointLightCandidate(topIdx1, usePointLightGrid, pos, radius, luma, castsShadow) && castsShadow)
            {
                float vis = 1.0f;
                COMPUTE_POINT_LIGHT_VIS(vis, pos, radius);
                packed1 = (vis >= 0.5f ? 1.0f : -1.0f) * ((float)topIdx1 + 1.0f);
            }
        }
        if (topIdx2 != 0xFFFFFFFFu)
        {
            float3 pos = 0.0f.xxx;
            float radius = 0.0f;
            float luma = 0.0f;
            bool castsShadow = false;
            if (LoadDirectPointLightCandidate(topIdx2, usePointLightGrid, pos, radius, luma, castsShadow) && castsShadow)
            {
                float vis = 1.0f;
                COMPUTE_POINT_LIGHT_VIS(vis, pos, radius);
                packed2 = (vis >= 0.5f ? 1.0f : -1.0f) * ((float)topIdx2 + 1.0f);
            }
        }

        outShadow.g = packed0;
        outShadow.b = packed1;
        outShadow.a = packed2;
        ShadowReservoirM[pixelPos] = 0.0f;
        ShadowResult[pixelPos] = outShadow;
        return;
    }

#if 0
        // -------------- Stochastic direct-light sample -----------------
        // No temporal/spatial ReSTIR reservoir. Pick one locally relevant
        // finite light uniformly per pixel/frame and let DLSS RR denoise the
        // raw noisy direct-light signal. This avoids stable structured
        // reservoir patterns showing up in direct diffuse when GI is off.
        uint stochasticChosenIdx = 0xFFFFFFFFu;
        bool stochasticChosenCastsShadow = false;
        uint stochasticValidCount = 0u;
        const uint stochasticSpatialLightMask = LoadSpatialLightMask(worldPos);
        const bool stochasticUsePointLightGrid = PointLightGridIsEnabled();
        uint stochasticGridCellIndex = 0u;
        uint stochasticGridCandidateCount = stochasticUsePointLightGrid ?
            PointLightGridSelectCandidateCount(worldPos, stochasticGridCellIndex) :
            0u;

        const uint stochasticCandCount = stochasticUsePointLightGrid ?
            min(stochasticGridCandidateCount, PointLightGridGetMaxCount()) :
            min(ShadowedPointLightCount, (uint)MAX_SHADOWED_PT_LIGHTS);

        [loop]
        for (uint stochasticCandidateIndex = 0u; stochasticCandidateIndex < 128u; ++stochasticCandidateIndex)
        {
            if (stochasticCandidateIndex >= stochasticCandCount)
                break;
            const uint stochasticCandIdx = stochasticUsePointLightGrid ?
                PointLightGridLoadLightIndex(stochasticGridCellIndex, stochasticCandidateIndex) :
                stochasticCandidateIndex;
            if (!stochasticUsePointLightGrid &&
                !ReSTIRLightAllowed(stochasticCandIdx, stochasticUsePointLightGrid, stochasticGridCellIndex, stochasticGridCandidateCount, stochasticSpatialLightMask))
                continue;

            float3 stochasticCandPos = 0.0f.xxx;
            float stochasticCandRadius = 0.0f;
            float stochasticCandLuma = 0.0f;
            bool stochasticCandCastsShadow = true;
            if (!LoadDirectPointLightCandidate(
                stochasticCandIdx,
                stochasticUsePointLightGrid,
                stochasticCandPos,
                stochasticCandRadius,
                stochasticCandLuma,
                stochasticCandCastsShadow))
                continue;

            const float3 stochasticToCand = stochasticCandPos - worldPos;
            const float stochasticDistSq = max(dot(stochasticToCand, stochasticToCand), 1.0e-4f);
            const float stochasticDist = sqrt(stochasticDistSq);
            const float stochasticNdotL = saturate(dot(worldNormal, stochasticToCand) / max(stochasticDist, 1.0e-3f));
            const float stochasticContribution =
                EvaluateReSTIRPointLightTargetPdf(stochasticCandLuma, stochasticDistSq, stochasticCandRadius, stochasticNdotL);
            if (stochasticContribution <= 0.0f)
                continue;

            ++stochasticValidCount;
            uint stochasticSeed =
                (pixelPos.x * 1973u + pixelPos.y * 9277u + FrameCounter * 26699u + stochasticCandIdx * 49u) * 6151u;
            stochasticSeed ^= stochasticSeed >> 13u;
            stochasticSeed *= 0x5bd1e995u;
            stochasticSeed ^= stochasticSeed >> 15u;
            const float stochasticU = (stochasticSeed & 0x00FFFFFFu) / 16777216.0f;
            if (stochasticU < rcp((float)stochasticValidCount))
            {
                stochasticChosenIdx = stochasticCandIdx;
                stochasticChosenCastsShadow = stochasticCandCastsShadow;
            }
        }

        if (stochasticChosenIdx != 0xFFFFFFFFu && stochasticValidCount > 0u)
        {
            float3 stochasticChosenPos = 0.0f.xxx;
            float stochasticChosenRadius = 0.0f;
            float stochasticChosenLuma = 0.0f;
            bool stochasticCastsShadow = false;
            const bool stochasticChosenValid = LoadDirectPointLightCandidate(
                stochasticChosenIdx,
                stochasticUsePointLightGrid,
                stochasticChosenPos,
                stochasticChosenRadius,
                stochasticChosenLuma,
                stochasticCastsShadow);
            float stochasticPointVis = 1.0f;
            if (stochasticChosenValid && stochasticChosenCastsShadow && stochasticCastsShadow)
            {
                COMPUTE_POINT_LIGHT_VIS(stochasticPointVis, stochasticChosenPos, stochasticChosenRadius);
            }
            outShadow.g = stochasticChosenValid ? (float)stochasticChosenIdx : 255.0f;
            outShadow.b = stochasticChosenValid ? (float)stochasticValidCount : 0.0f;
            outShadow.a = stochasticChosenValid ? stochasticPointVis : 1.0f;
        }
        else
        {
            outShadow.g = 255.0f;
            outShadow.b = 0.0f;
            outShadow.a = 1.0f;
        }

        ShadowReservoirM[pixelPos] = 0.0f;
        ShadowResult[pixelPos] = outShadow;
        return;
#endif

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
        const uint spatialLightMask = LoadSpatialLightMask(worldPos);
        const bool usePointLightGrid = PointLightGridIsEnabled();
        uint gridCellIndex = 0u;
        uint gridCandidateCount = usePointLightGrid ?
            PointLightGridSelectCandidateCount(worldPos, gridCellIndex) :
            0u;

        const uint candCount = usePointLightGrid ?
            min(gridCandidateCount, PointLightGridGetMaxCount()) :
            min(ShadowedPointLightCount, (uint)MAX_SHADOWED_PT_LIGHTS);
        [loop]
        for (uint candidateIndex = 0; candidateIndex < 128u; ++candidateIndex)
        {
            if (candidateIndex >= candCount)
                break;
            const uint candIdx = usePointLightGrid ?
                PointLightGridLoadLightIndex(gridCellIndex, candidateIndex) :
                candidateIndex;
            if (!usePointLightGrid &&
                !ReSTIRLightAllowed(candIdx, usePointLightGrid, gridCellIndex, gridCandidateCount, spatialLightMask))
                continue;
            float3 candPos = 0.0f.xxx;
            float candRadius = 0.0f;
            float candLuma = 0.0f;
            if (!LoadReSTIRPointLight(candIdx, usePointLightGrid, candPos, candRadius, candLuma))
                continue;

            // Per-candidate unshadowed contribution estimate: luma /
            // (distance² + range² damping). Higher means better candidate.
            float3 toCand = candPos - worldPos;
            float distSq = max(dot(toCand, toCand), 1.0e-4f);
            float dist = sqrt(distSq);
            float NdotL = saturate(dot(worldNormal, toCand) / max(dist, 1.0e-3f));
            float targetPdf = EvaluateReSTIRPointLightTargetPdf(candLuma, distSq, candRadius, NdotL);
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
        //
        // Runtime-tunable via CB.ShadowMaxM (was a 3.0f compile-time
        // constant). User-facing slider in the Sponza demo panel.
        // Empirical sweet spot on Sponza + DLSS RR: 3.0 (ghost-free,
        // ~√3 ≈ 1.7× variance reduction). Clamped against div-by-zero.
        const float kMaxM = max(ShadowMaxM, 1.0f);
        float M_eff = (chosenIdx == 0xFFFFFFFFu) ? 0.0f : 1.0f;

        // ReSTIR Phase 3 — spatial reuse with disocclusion gate
        // (Option C, 2026-05-30). Each neighbour's prev-frame
        // depth/normal is compared against the current pixel's; if
        // they diverge (different surface, disocclusion, fast motion)
        // the neighbour is rejected. This keeps the variance-
        // reduction benefit of spatial reuse on stationary regions
        // while preventing the ghost streaks under camera motion that
        // pure 1-frame-stale reuse caused.
        const int   kSpatialSamples   = 3;
        const float kSpatialRadius    = 4.0f;  // pixels
        // (No disocclusion / velocity-decay gates here — they were
        // tried 2026-05-30 to remove motion ghosting, but the
        // residual motion noise was traced to TAA/DLSS-RR's
        // pre-existing handling of motion-time noise, not ReSTIR.
        // Gates only cost brightness under motion by falling back to
        // Phase 1-only behaviour; kept disabled to preserve the
        // 4-channel-matching mean luma.)
        [unroll]
        for (int sIdx = 0; sIdx < kSpatialSamples; ++sIdx)
        {
            uint sSeed = (pixelPos.x * 5237u + pixelPos.y * 6311u + (uint)sIdx * 991u) * 1597u;
            sSeed ^= sSeed >> 13u; sSeed *= 0x5bd1e995u; sSeed ^= sSeed >> 15u;
            float rA = (sSeed & 0xFFFFu) / 65535.0f;
            sSeed = sSeed * 1664525u + 1013904223u;
            float rB = (sSeed & 0xFFFFu) / 65535.0f;
            const float angle = rA * 6.28318530717958647692f;
            const float radius = sqrt(rB) * kSpatialRadius;
            const int2 ofs = int2(round(cos(angle) * radius), round(sin(angle) * radius));
            const int2 spatialPx = int2(pixelPos) + ofs;
            if (spatialPx.x < 0 || spatialPx.x >= (int)launchSize.x ||
                spatialPx.y < 0 || spatialPx.y >= (int)launchSize.y)
                continue;
            const float2 spatialUv = (float2(spatialPx) + 0.5f) / launchSize;
            const float spatialDepth = DepthTexPrev.Load(int3(spatialPx, 0)).x;
            const float3 spatialNormal = WorldNormalTexPrev.Load(int3(spatialPx, 0)).xyz;
            if (!ReSTIRSurfaceCompatible(spatialUv, spatialDepth, spatialNormal, currentLinearDepth, worldNormal))
                continue;
            // Integer-coord point sampling. SampleLevel bilinear on a
            // RGBA32F reservoir whose .g channel encodes lightIdx as a
            // float corrupts the index (4-pixel blend → non-integer →
            // wrong light), causing apparent history resets/noise
            // under any sub-pixel motion (TAA jitter included).
            const float4 sp = ShadowReservoirPrev.Load(int3(spatialPx, 0));
            const uint  spIdx   = (uint)(sp.g + 0.5f);
            const float spRatio = sp.b;
            const float spM     = ShadowReservoirMPrev.Load(int3(spatialPx, 0)).x;
            if (spRatio <= 0.0f || spM <= 0.0f ||
                !ReSTIRLightAllowed(spIdx, usePointLightGrid, gridCellIndex, gridCandidateCount, spatialLightMask))
                continue;
            float3 cPos = 0.0f.xxx;
            float cRad = 0.0f;
            float cLum = 0.0f;
            if (!LoadReSTIRPointLight(spIdx, usePointLightGrid, cPos, cRad, cLum))
                continue;
            const float3 toC = cPos - worldPos;
            const float  dSq = max(dot(toC, toC), 1.0e-4f);
            const float  d   = sqrt(dSq);
            const float  NLc = saturate(dot(worldNormal, toC) / max(d, 1.0e-3f));
            const float  tpdfC = EvaluateReSTIRPointLightTargetPdf(cLum, dSq, cRad, NLc);
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
            // CANONICAL RIS combine: M_eff must accumulate the SAME spM
            // factor that was used in the weight contribution (spW above
            // uses full spM). The previous half-weight asymmetry
            // (`spM * 0.5f` here while `spW` used full `spM`) caused
            // weightSum to grow faster than M_eff, which compounded
            // through Phase 2 temporal each frame and saturated the
            // firefly clamp — the visible "ReSTIR is way too bright vs
            // 4-channel" symptom.
            M_eff += spM;
        }

        // ReSTIR Phase 2 — temporal reuse. Sample the previous
        // frame's reservoir at the motion-reprojected pixel and RIS-
        // combine into the current pixel's reservoir.
        const float2 velocity = VelocityTex.SampleLevel(sampleWrap, uv, 0).xy;
        const float2 prevUV = uv - velocity;
        if (prevUV.x >= 0.0f && prevUV.x < 1.0f && prevUV.y >= 0.0f && prevUV.y < 1.0f)
        {
            // Integer-coord point sampling — bilinear on the
            // RGBA32F reservoir corrupts lightIdx (.g) on any
            // sub-pixel reprojection (e.g. TAA/DLSS jitter), which
            // resets the temporal history every frame and produces
            // strong noise under camera motion. See spatial-reuse
            // block above for the same fix.
            const int2 prevPx = int2(prevUV * launchSize);
            const float prevDepth = DepthTexPrev.Load(int3(prevPx, 0)).x;
            const float3 prevNormal = WorldNormalTexPrev.Load(int3(prevPx, 0)).xyz;
            if (ReSTIRSurfaceCompatible((float2(prevPx) + 0.5f) / launchSize, prevDepth, prevNormal, currentLinearDepth, worldNormal))
            {
                const float4 prev = ShadowReservoirPrev.Load(int3(prevPx, 0));
                const uint prevIdx = (uint)(prev.g + 0.5f);
                const float prevRatio = prev.b;
                const float prevM = ShadowReservoirMPrev.Load(int3(prevPx, 0)).x;
                if (prevRatio > 0.0f && prevM > 0.0f &&
                    ReSTIRLightAllowed(prevIdx, usePointLightGrid, gridCellIndex, gridCandidateCount, spatialLightMask))
                {
                    float3 candPos = 0.0f.xxx;
                    float candRadius = 0.0f;
                    float candLuma = 0.0f;
                    if (!LoadReSTIRPointLight(prevIdx, usePointLightGrid, candPos, candRadius, candLuma))
                    {
                        candLuma = 0.0f;
                    }
                const float3 toCand = candPos - worldPos;
                const float distSq = max(dot(toCand, toCand), 1.0e-4f);
                const float dist = sqrt(distSq);
                const float NdotL = saturate(dot(worldNormal, toCand) / max(dist, 1.0e-3f));
                const float targetPdfPrev = EvaluateReSTIRPointLightTargetPdf(candLuma, distSq, candRadius, NdotL);
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
                    // Accumulate prev's M with NO running cap here —
                    // the prev's M is already capped by last frame's
                    // writeback (`min(M_eff, kMaxM)` below). Applying a
                    // running cap during combine while weightSum keeps
                    // growing breaks the W = w_sum/(tpdf·M) ratio and
                    // inflates W per frame. Steady-state W stays
                    // bounded as long as the combine is symmetric and
                    // the writeback cap clips next frame's prev_M.
                    M_eff += prevM;
                }
            }
            }
        }

        if (chosenIdx != 0xFFFFFFFFu && M_eff > 0.0f)
        {
            float3 chosenPos = 0.0f.xxx;
            float chosenRadius = 0.0f;
            float chosenLuma = 0.0f;
            const bool chosenValid = LoadReSTIRPointLight(chosenIdx, usePointLightGrid, chosenPos, chosenRadius, chosenLuma);
            float pointVis = 1.0f;
            if (chosenValid)
            {
                COMPUTE_POINT_LIGHT_VIS(pointVis, chosenPos, chosenRadius);
            }
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
            outShadow.g = chosenValid ? (float)chosenIdx : 255.0f;
            outShadow.b = chosenValid ? ratio : 0.0f;
            outShadow.a = chosenValid ? pointVis : 1.0f;
            // Cap M ON WRITEBACK so next frame's Phase 2/3 lite read
            // back a bounded prev_M. Running cap during the combine
            // (removed above) created the W-inflation bug.
            ShadowReservoirM[pixelPos] = min(M_eff, kMaxM);
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

#ifdef RT_SHADOW_INLINE_RAYQUERY
RT_SHADOW_INLINE_ENTRY_DECL
{
    uint2 launchDim;
    ShadowResult.GetDimensions(launchDim.x, launchDim.y);
    if (pixelPos.x >= launchDim.x || pixelPos.y >= launchDim.y)
        return;
    ExecuteShadowPass(pixelPos, launchDim);
}
#else
[shader("raygeneration")]
void rayGen()
{
    uint3 launchIndex = DispatchRaysIndex();
    uint3 launchDim = DispatchRaysDimensions();
    ExecuteShadowPass(launchIndex.xy, launchDim.xy);
}
#endif

#ifndef RT_SHADOW_INLINE_RAYQUERY
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

    if (!IsAlphaTestedInstance(instanceID, CORONA_INSTANCE_PROPERTY))
    {
        AcceptHitAndEndSearch();
        return;
    }

    Vertex vertex = CORONA_GET_VERTEX_ATTRIBUTES(instanceID, triangleIndex, barycentrics);
    float opacity = 1.0f;
    RTMaterialRecord material = RtMaterials[instanceID];
    opacity = MaterialTextures[NonUniformResourceIndex(material.AlbedoTextureIndex)].SampleLevel(sampleWrap, vertex.uv, 5).w * material.BaseColorFactor.w;


        // payload.bHit = false;

    if(opacity > 0.10)
    {
        AcceptHitAndEndSearch();
        return;
    }
    
    IgnoreHit();
}
#endif
