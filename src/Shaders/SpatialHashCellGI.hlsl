#include "Common.hlsl"

RWStructuredBuffer<float4> TraceSH0 : register(u0);
RWStructuredBuffer<float4> TraceSH1 : register(u1);
RWStructuredBuffer<float4> TraceSH2 : register(u2);
RWStructuredBuffer<float4> TraceSH3 : register(u3);

RaytracingAccelerationStructure gRtScene : register(t0);
StructuredBuffer<uint> CellKeys : register(t1);
StructuredBuffer<float4> CellPosition : register(t2);
StructuredBuffer<float4> CellNormal : register(t3);
Texture3D RayNoiseBlueNoiseSource : register(t4);
ByteAddressBuffer vertices : register(t5);
ByteAddressBuffer indices : register(t6);
Texture2D AlbedoTex : register(t7);
ByteAddressBuffer InstanceProperty : register(t8);
StructuredBuffer<uint> ActiveCellSlots : register(t9);
StructuredBuffer<uint> ActiveCounter : register(t10);
StructuredBuffer<uint> CellLightMask : register(t11);

SamplerState sampleWrap : register(s0);

// Must match Corona::MaxPointLights in Corona.h.
#define MAX_POINT_LIGHTS 128
#define RT_DIFFUSE_GI_MAX_POINT_LIGHTS 16

struct PointLightParam
{
    float4 PositionAndRadius;
    float4 ColorAndIntensity;
    float4 DirectionAndType;
    float4 SpotConeAndFlags;
};

cbuffer ViewParameter : register(b0)
{
    float4 LightDirAndIntensity;
    uint HashEntryCount;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    uint NoiseMode;
    uint RaysPerCell;
    uint MaxBounces;
    float ViewSpreadAngle;
    float RayBias;
    float CellSize;
    float3 SkyColorTop;
    float SkyIntensity;
    float3 SkyColorBottom;
    float _padding;
    float3 LightColor;
    float _padding2;
    uint ActiveCellCapacity;
    uint bIncludeSkyLighting;
    uint HashEntryMask;
    uint MaxProbeSteps;
    uint GIMode;            // 0 = SH4 trace, 1 = octahedral per-ray trace, 2 = HL2 basis
    uint OctCellCapacity;   // probe slots backed by octahedral atlas storage
    uint OctRaysPerCell;    // rays per probe per frame (octahedral mode)
    PointLightParam PointLights[RT_DIFFUSE_GI_MAX_POINT_LIGHTS];
    uint PointLightCount;
    float3 PointLightPadding;
    float4 CameraPosition; // world-space camera (oct origin camera-bias)
    float4 SpatialHashLevelParams; // x=enable, y=base distance (SHaRC cell levels)
};

// SHaRC distance-based cell sizing: far cells are exponentially larger so far
// vistas need few cells (bounded working set). Disabled -> base CellSize. MUST
// match the diffuse shader's copy so insert/lookup/light-mask keys agree.
float SpatialHashLeveledCellSize(float3 worldPos, out uint level)
{
    float cs = max(CellSize, 1e-3f);
    level = 0u;
    if (SpatialHashLevelParams.x < 0.5f)
        return cs;
    float dist = length(worldPos - CameraPosition.xyz);
    float fl = floor(max(log2(max(dist / max(SpatialHashLevelParams.y, 1e-3f), 1.0f)), 0.0f));
    level = (uint)fl;
    return cs * exp2(fl);
}

// Per-level integer offset so cells at different levels never share a key.
int3 SpatialHashLevelOffset(uint level)
{
    return int3(level, level, level) * int3(1737, 9277, 4513);
}

// Per-ray output for octahedral DDGI (GIMode==1): rgb radiance + hit distance.
// Indexed by probeSlot * OctRaysPerCell + rayIndex. Consumed by the octahedral
// blend pass in SpatialHashDiffuseGI.hlsl.
RWStructuredBuffer<float4> OctRayData : register(u4);

// Single camera-anchored ambient SH4 (4 float4: c0..c3, c0.w = history count).
// One extra dispatched thread traces a full-sphere SH from the camera each frame;
// the query uses it as the fallback for uncached cells (adapts to indoor/outdoor,
// unlike a fixed sky colour). Shared by SH and oct modes.
RWStructuredBuffer<float4> CameraProbeSHOut : register(u5);

static const float INV_PI = 1.0f / PI;
static const float MAX_HIT_DIST = 10000.0f;
static const float SPATIAL_HASH_MIN_SURFACE_NORMAL_DOT = 0.72f;
static const float SPATIAL_HASH_FULL_SURFACE_NORMAL_DOT = 0.92f;
static const float SPATIAL_HASH_PLANE_REJECT_CELL_SCALE = 0.45f;
static const float SPATIAL_HASH_PLANE_SOFT_CELL_SCALE = 0.25f;

#define SPATIAL_HASH_GI_RAY_FLAGS (RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES)

struct RT_DIFFUSE_GI_RAY_PAYLOAD RayPayload
{
    float3 position RT_DIFFUSE_GI_PAYLOAD_RW;
    float3 color RT_DIFFUSE_GI_PAYLOAD_RW;
    float3 normal RT_DIFFUSE_GI_PAYLOAD_RW;
    float spreadAngle RT_DIFFUSE_GI_PAYLOAD_RW;
    float coneWidth RT_DIFFUSE_GI_PAYLOAD_RW;
    bool bHit RT_DIFFUSE_GI_PAYLOAD_RW;
};

struct RT_DIFFUSE_GI_RAY_PAYLOAD ShadowRayPayload
{
    bool bHit RT_DIFFUSE_GI_SHADOW_PAYLOAD_RW;
};

void TraceDiffuseGIRay(RayDesc ray, inout RayPayload payload)
{
#if RT_DIFFUSE_GI_USE_SER
    dx::HitObject hit = dx::HitObject::TraceRay(
        gRtScene,
        SPATIAL_HASH_GI_RAY_FLAGS,
        0xFF,
        0,
        0,
        0,
        ray,
        payload);
    dx::MaybeReorderThread(hit, hit.GetInstanceID(), RT_DIFFUSE_GI_SER_MATERIAL_HINT_BITS);
    dx::HitObject::Invoke(hit, payload);
#else
    TraceRay(
        gRtScene,
        SPATIAL_HASH_GI_RAY_FLAGS,
        0xFF,
        0,
        0,
        0,
        ray,
        payload);
#endif
}

struct SH4RGB
{
    float3 c0;
    float3 c1;
    float3 c2;
    float3 c3;
};

static const float3 HL2_BASIS0 = float3(0.81649658f, 0.0f, 0.57735027f);
static const float3 HL2_BASIS1 = float3(-0.40824829f, 0.70710678f, 0.57735027f);
static const float3 HL2_BASIS2 = float3(-0.40824829f, -0.70710678f, 0.57735027f);

float3 SanitizeFloat3(float3 value)
{
    if (any(isnan(value)) || any(isinf(value)))
        return 0.0f.xxx;
    return value;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    value = SanitizeFloat3(value);
    float lenSq = dot(value, value);
    if (lenSq < 1e-8f)
        return fallback;
    return value * rsqrt(lenSq);
}

SH4RGB InitSH4RGB()
{
    SH4RGB sh;
    sh.c0 = 0.0f.xxx;
    sh.c1 = 0.0f.xxx;
    sh.c2 = 0.0f.xxx;
    sh.c3 = 0.0f.xxx;
    return sh;
}

void AccumulateSH4RGB(inout SH4RGB accum, SH4RGB value, float scale)
{
    accum.c0 += value.c0 * scale;
    accum.c1 += value.c1 * scale;
    accum.c2 += value.c2 * scale;
    accum.c3 += value.c3 * scale;
}

SH4RGB ScaleSH4RGB(SH4RGB sh, float scale)
{
    sh.c0 *= scale;
    sh.c1 *= scale;
    sh.c2 *= scale;
    sh.c3 *= scale;
    return sh;
}

SH4RGB ProjectRadianceToSH4RGB(float3 radiance, float3 direction, float sampleWeight)
{
    direction = SafeNormalize(direction, float3(0.0f, 1.0f, 0.0f));
    radiance = max(SanitizeFloat3(radiance), 0.0f.xxx) * sampleWeight;

    float x = direction.x;
    float y = direction.y;
    float z = direction.z;

    SH4RGB sh;
    sh.c0 = radiance * 0.282095f;
    sh.c1 = radiance * (0.488603f * y);
    sh.c2 = radiance * (0.488603f * z);
    sh.c3 = radiance * (0.488603f * x);
    return sh;
}

float3 ComputeHL2BasisWeights(float3 localDir)
{
    localDir = SafeNormalize(localDir, float3(0.0f, 0.0f, 1.0f));
    return float3(
        max(0.0f, dot(localDir, HL2_BASIS0)),
        max(0.0f, dot(localDir, HL2_BASIS1)),
        max(0.0f, dot(localDir, HL2_BASIS2)));
}

void AccumulateHL2RGB(inout SH4RGB accum, inout float3 weightSum, float3 radiance, float3 localDir)
{
    radiance = max(SanitizeFloat3(radiance), 0.0f.xxx);
    float3 w = ComputeHL2BasisWeights(localDir);
    accum.c0 += radiance * w.x;
    accum.c1 += radiance * w.y;
    accum.c2 += radiance * w.z;
    weightSum += w;
}

SH4RGB NormalizeHL2RGB(SH4RGB accum, float3 weightSum, float3 fallbackRadiance)
{
    fallbackRadiance = max(SanitizeFloat3(fallbackRadiance), 0.0f.xxx);
    accum.c0 = weightSum.x > 1e-4f ? accum.c0 / weightSum.x : fallbackRadiance;
    accum.c1 = weightSum.y > 1e-4f ? accum.c1 / weightSum.y : fallbackRadiance;
    accum.c2 = weightSum.z > 1e-4f ? accum.c2 / weightSum.z : fallbackRadiance;
    accum.c3 = 0.0f.xxx;
    return accum;
}

float3x3 BuildTBN(float3 normal)
{
    static const float3 rvec1 = float3(0.847100675f, 0.207911700f, 0.489073813f);
    static const float3 rvec2 = float3(-0.639436305f, -0.390731126f, 0.662155867f);
    float3 rvec = dot(rvec1, normal) > 0.95f ? rvec2 : rvec1;
    float3 b1 = normalize(rvec - normal * dot(rvec, normal));
    float3 b2 = cross(normal, b1);
    return float3x3(b1, b2, normal);
}

float ComputeLightLuma(float3 color)
{
    return dot(max(color, 0.0f.xxx), float3(0.2126f, 0.7152f, 0.0722f));
}

uint ComputeLocalLightMask(float3 worldPos, float3 normal)
{
    uint mask = 0u;
    uint activeCount = min(PointLightCount, (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS);
    normal = SafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));

    [loop]
    for (uint lightIndex = 0u; lightIndex < RT_DIFFUSE_GI_MAX_POINT_LIGHTS; ++lightIndex)
    {
        if (lightIndex >= activeCount)
            break;

        PointLightParam light = PointLights[lightIndex];
        float3 toLight = light.PositionAndRadius.xyz - worldPos;
        float distanceSq = max(dot(toLight, toLight), 1.0e-4f);
        float lightDistance = sqrt(distanceSq);
        float range = max(light.PositionAndRadius.w, 0.01f);
        if (lightDistance > range)
            continue;

        float3 lightDir = toLight / lightDistance;
        if (dot(normal, lightDir) <= 0.0f)
            continue;

        if (light.DirectionAndType.w >= 0.5f)
        {
            float3 spotDir = SafeNormalize(light.DirectionAndType.xyz, float3(0.0f, 1.0f, 0.0f));
            float cosTheta = dot(spotDir, -lightDir);
            if (cosTheta < light.SpotConeAndFlags.y - 0.05f)
                continue;
        }

        float rangeAttenuation = saturate(1.0f - lightDistance / range);
        rangeAttenuation *= rangeAttenuation;
        float lightEnergy = ComputeLightLuma(light.ColorAndIntensity.xyz) * max(light.ColorAndIntensity.w, 0.0f) * rangeAttenuation;
        if (lightEnergy <= 1.0e-5f)
            continue;

        mask |= (1u << lightIndex);
    }

    return mask;
}

int3 GetSpatialHashCell(float3 worldPos)
{
    uint level;
    float cs = SpatialHashLeveledCellSize(worldPos, level);
    return int3(floor(worldPos / cs)) + SpatialHashLevelOffset(level);
}

uint EncodeNormalBits(float3 normal)
{
    normal = SafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    uint3 normalBin = uint3(saturate(normal * 0.5f + 0.5f) * 7.0f + 0.5f);
    return (normalBin.x & 7u) | ((normalBin.y & 7u) << 3u) | ((normalBin.z & 7u) << 6u);
}

int ComputePlaneBin(float3 worldPos, float3 normal)
{
    uint level;
    float cs = SpatialHashLeveledCellSize(worldPos, level);
    normal = SafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    return int(floor((dot(worldPos, normal) / cs) * 2.0f + 0.5f));
}

// Cell key. GIMode==1 (octahedral DDGI): one probe per cell -> key depends on
// the cell coordinate ONLY; directionality comes from the octahedral map. Else
// (SH4/HL2 normal-bin modes): split the cell by 3-axis normal bin + plane bin so
// each surface orientation gets its own hemisphere cache (faster convergence).
// MUST stay identical to the diffuse shader's copy so insert and lookup agree.
uint HashCellKeyFromCell(int3 cell, float3 normal, int planeBin)
{
    uint h = uint(cell.x) * 73856093u;
    h ^= uint(cell.y) * 19349663u;
    h ^= uint(cell.z) * 83492791u;
    if (GIMode != 1u)
    {
        h ^= EncodeNormalBits(normal) * 2654435761u;
        h ^= uint(planeBin) * 1597334677u;
    }
    h = HashUInt(h);
    return h == 0u ? 1u : h;
}

uint HashInitialSlot(uint key)
{
    return HashUInt(key ^ 0x9e3779b9u) & HashEntryMask;
}

bool FindSpatialHashSlot(uint key, out uint slot)
{
    uint startSlot = HashInitialSlot(key);
    uint probeCount = clamp(MaxProbeSteps, 1u, 16u);

    [loop]
    for (uint probeIndex = 0u; probeIndex < 16u; ++probeIndex)
    {
        if (probeIndex >= probeCount)
            break;

        uint candidate = (startSlot + probeIndex) & HashEntryMask;
        uint storedKey = CellKeys[candidate];
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

float ComputeSurfaceHashMatchWeight(uint slot, float3 worldPos, float3 normal)
{
    float4 storedPosition = CellPosition[slot];
    float4 storedNormal4 = CellNormal[slot];
    if (storedPosition.w <= 0.0f || storedNormal4.w <= 0.0f)
        return 0.0f;

    normal = SafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    float3 storedNormal = SafeNormalize(storedNormal4.xyz, normal);
    float normalDot = saturate(dot(normal, storedNormal));
    if (normalDot < SPATIAL_HASH_MIN_SURFACE_NORMAL_DOT)
        return 0.0f;

    float safeCellSize = max(CellSize, 1e-3f);
    float planeDelta = abs(dot(worldPos - storedPosition.xyz, storedNormal));
    float planeReject = safeCellSize * SPATIAL_HASH_PLANE_REJECT_CELL_SCALE;
    if (planeDelta > planeReject)
        return 0.0f;

    float normalWeight = smoothstep(
        SPATIAL_HASH_MIN_SURFACE_NORMAL_DOT,
        SPATIAL_HASH_FULL_SURFACE_NORMAL_DOT,
        normalDot);
    float planeWeight = 1.0f - smoothstep(
        safeCellSize * SPATIAL_HASH_PLANE_SOFT_CELL_SCALE,
        planeReject,
        planeDelta);
    return saturate(normalWeight * planeWeight);
}

uint LoadSurfaceLightMask(float3 worldPos, float3 normal)
{
    int3 cell = GetSpatialHashCell(worldPos);
    int basePlaneBin = ComputePlaneBin(worldPos, normal);
    uint bestMask = 0u;
    float bestScore = 0.0f;

    [unroll]
    for (int planeOffset = -1; planeOffset <= 1; ++planeOffset)
    {
        uint slot = 0u;
        uint key = HashCellKeyFromCell(cell, normal, basePlaneBin + planeOffset);
        if (!FindSpatialHashSlot(key, slot))
            continue;

        float score = ComputeSurfaceHashMatchWeight(slot, worldPos, normal);
        if (score > bestScore)
        {
            bestScore = score;
            bestMask = CellLightMask[slot];
        }
    }

    if (bestScore > 0.0f)
        return bestMask;

    return ComputeLocalLightMask(worldPos, normal);
}

float3 EvaluateSkyColor(float3 direction)
{
    float t = 0.5f * (direction.y + 1.0f);
    return max(lerp(SkyColorBottom, SkyColorTop, t) * SkyIntensity, 0.0f.xxx);
}

float3 EvaluateSkyDiffuseBounce(float3 normal)
{
    float3 averageSky = 0.5f * (SkyColorTop + SkyColorBottom);
    float3 skyGradient = 0.5f * (SkyColorTop - SkyColorBottom);
    return max((averageSky + (2.0f / 3.0f) * skyGradient * normal.y) * SkyIntensity, 0.0f.xxx);
}

float EvaluateSpotAttenuation(PointLightParam light, float3 surfaceToLightDir)
{
    if (light.DirectionAndType.w < 0.5f)
        return 1.0f;

    float3 spotDir = SafeNormalize(light.DirectionAndType.xyz, float3(0.0f, 1.0f, 0.0f));
    float cosTheta = dot(spotDir, -surfaceToLightDir);
    float cone = saturate((cosTheta - light.SpotConeAndFlags.y) * light.SpotConeAndFlags.z);
    return cone * cone;
}

bool IsDirectLightVisible(float3 worldPos, float3 normal)
{
    float3 lightDir = normalize(LightDirAndIntensity.xyz);
    if (dot(normal, lightDir) <= 0.0f)
        return false;

    RayDesc shadowRay;
    shadowRay.Origin = worldPos + normal * RayBias;
    shadowRay.Direction = lightDir;
    shadowRay.TMin = 0.001f;
    shadowRay.TMax = MAX_HIT_DIST;

    ShadowRayPayload shadowPayload;
    shadowPayload.bHit = true;
    TraceRay(
        gRtScene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
            RAY_FLAG_FORCE_OPAQUE |
            RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        0xFF,
        0,
        0,
        1,
        shadowRay,
        shadowPayload);

    return !shadowPayload.bHit;
}

bool IsPointLightVisible(float3 worldPos, float3 normal, float3 lightDir, float lightDistance, PointLightParam light)
{
    if (light.SpotConeAndFlags.w <= 0.5f)
        return true;

    RayDesc shadowRay;
    shadowRay.Origin = worldPos + normal * RayBias;
    shadowRay.Direction = lightDir;
    shadowRay.TMin = 0.001f;
    shadowRay.TMax = max(lightDistance - 0.05f, 0.001f);

    ShadowRayPayload shadowPayload;
    shadowPayload.bHit = true;
    TraceRay(
        gRtScene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
            RAY_FLAG_FORCE_OPAQUE |
            RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        0xFF,
        0,
        0,
        1,
        shadowRay,
        shadowPayload);

    return !shadowPayload.bHit;
}

float3 EvaluatePointLightBounce(float3 worldPos, float3 normal, float3 albedo, uint lightMask)
{
    float3 radiance = 0.0f.xxx;
    uint activeCount = min(PointLightCount, (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS);
    [loop]
    for (uint lightIndex = 0u; lightIndex < RT_DIFFUSE_GI_MAX_POINT_LIGHTS; ++lightIndex)
    {
        if (lightIndex >= activeCount)
            break;
        if ((lightMask & (1u << lightIndex)) == 0u)
            continue;

        PointLightParam light = PointLights[lightIndex];
        float3 toLight = light.PositionAndRadius.xyz - worldPos;
        float distanceSq = max(dot(toLight, toLight), 1.0e-4f);
        float lightDistance = sqrt(distanceSq);
        float3 lightDir = toLight / lightDistance;
        float range = max(light.PositionAndRadius.w, 0.01f);
        float rangeAttenuation = saturate(1.0f - lightDistance / range);
        rangeAttenuation *= rangeAttenuation;
        float inverseSquareAttenuation = 1.0f / max(1.0f, distanceSq * 0.0001f);
        float attenuation = rangeAttenuation * inverseSquareAttenuation * EvaluateSpotAttenuation(light, lightDir);
        float nDotL = saturate(dot(normal, lightDir));
        if (attenuation <= 0.0f || nDotL <= 0.0f || !IsPointLightVisible(worldPos, normal, lightDir, lightDistance, light))
            continue;

        float3 lightColor = max(CommonSanitizeFloat3(light.ColorAndIntensity.xyz, 0.0f.xxx), 0.0f.xxx);
        float lightIntensity = max(CommonSanitizeFloat(light.ColorAndIntensity.w, 0.0f), 0.0f);
        radiance += nDotL * lightColor * lightIntensity * attenuation * max(albedo, 0.0f.xxx) * INV_PI;
    }
    return radiance;
}

float3 EvaluateDirectSurfaceRadiance(float3 worldPos, float3 normal, float3 albedo, uint lightMask)
{
    float3 radiance = EvaluatePointLightBounce(worldPos, normal, albedo, lightMask);
    float3 lightDir = normalize(LightDirAndIntensity.xyz);
    float nDotL = saturate(dot(normal, lightDir));
    if (nDotL <= 0.0f)
        return radiance;
    if (!IsDirectLightVisible(worldPos, normal))
        return radiance;

    radiance += nDotL * LightDirAndIntensity.w * LightColor * max(albedo, 0.0f.xxx) * INV_PI;
    return radiance;
}

float3 TraceDiffusePath(float3 origin, float3 direction, uint2 noiseCoord, uint sampleIndex, out float firstHitDistance)
{
    float3 radiance = 0.0f.xxx;
    float3 throughput = 1.0f.xxx;
    float3 rayOrigin = origin;
    float3 rayDirection = SafeNormalize(direction, float3(0.0f, 1.0f, 0.0f));
    uint bounceCount = clamp(MaxBounces, 1u, 8u);
    firstHitDistance = MAX_HIT_DIST;

    [loop]
    for (uint bounceIndex = 0u; bounceIndex < 8u; ++bounceIndex)
    {
        if (bounceIndex >= bounceCount)
            break;

        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = rayDirection;
        ray.TMin = 0.0f;
        ray.TMax = MAX_HIT_DIST;

        RayPayload payload;
        payload.position = 0.0f.xxx;
        payload.color = 0.0f.xxx;
        payload.normal = float3(0.0f, 1.0f, 0.0f);
        payload.spreadAngle = ViewSpreadAngle * max(CellSize, 1.0f);
        payload.coneWidth = 0.0f;
        payload.bHit = false;

        TraceDiffuseGIRay(ray, payload);

        if (!payload.bHit)
        {
            if (bIncludeSkyLighting != 0u)
                radiance += throughput * EvaluateSkyColor(rayDirection);
            break;
        }

        if (bounceIndex == 0u)
            firstHitDistance = length(payload.position - origin);

        float3 hitNormal = SafeNormalize(payload.normal, float3(0.0f, 1.0f, 0.0f));
        float3 hitAlbedo = max(SanitizeFloat3(payload.color), 0.0f.xxx);
        uint hitLightMask = LoadSurfaceLightMask(payload.position, hitNormal);
        radiance += throughput * EvaluateDirectSurfaceRadiance(payload.position, hitNormal, hitAlbedo, hitLightMask);

        if (bounceIndex + 1u >= bounceCount)
            break;

        throughput *= hitAlbedo;
        float maxThroughput = max(max(throughput.x, throughput.y), throughput.z);
        if (maxThroughput < 1e-3f)
            break;

        uint2 bounceNoiseCoord = noiseCoord + uint2(37u * (bounceIndex + 1u), 53u * (sampleIndex + 1u));
        float2 randomUV = GenerateRaySample2D(
            RayNoiseBlueNoiseSource,
            bounceNoiseCoord,
            FrameCounter + 19u * (bounceIndex + 1u) + 7u * sampleIndex,
            BlueNoiseOffsetStride,
            NoiseMode);

        float3 nextDirLocal = SampleHemisphereCosine(randomUV.x, randomUV.y);
        rayDirection = SafeNormalize(mul(nextDirLocal, BuildTBN(hitNormal)), hitNormal);
        rayOrigin = payload.position + hitNormal * RayBias;
    }

    return max(SanitizeFloat3(radiance), 0.0f.xxx);
}

void TraceCameraAmbientProbe(); // defined after RayGenOctahedral

// Octahedral DDGI trace (GIMode==1): one dispatched thread per (probe, ray).
// Each thread traces a single full-sphere path from the probe's open-space
// origin and records (radiance, hitDistance) to OctRayData. A later compute
// pass convolves these rays into the per-probe octahedral irradiance/depth map.
void RayGenOctahedral()
{
    uint globalRay = DispatchRaysIndex().x;
    // Extra trailing thread used to trace the camera ambient probe. Disabled for
    // the oct miss-fallback test; query now relies on view-ray hash lookup first.
    if (globalRay == OctCellCapacity * OctRaysPerCell)
    {
        return;
    }
    uint raysPerProbe = max(OctRaysPerCell, 1u);
    uint probeIndex = globalRay / raysPerProbe;
    uint rayIndex = globalRay % raysPerProbe;

    uint activeCount = min(ActiveCounter[0], ActiveCellCapacity);
    uint octProbeCount = min(activeCount, OctCellCapacity);
    if (probeIndex >= octProbeCount)
        return;

    uint slot = ActiveCellSlots[probeIndex];
    uint key = CellKeys[slot];
    float4 cellPosition = CellPosition[slot];
    float4 cellNormal = CellNormal[slot];
    if (key == 0u || cellPosition.w <= 0.0f || cellNormal.w <= 0.0f)
    {
        OctRayData[globalRay] = float4(0.0f, 0.0f, 0.0f, MAX_HIT_DIST);
        return;
    }

    float3 worldNormal = SafeNormalize(cellNormal.xyz, float3(0.0f, 1.0f, 0.0f));
    float3 worldPos = cellPosition.xyz;
#ifndef SPATIAL_HASH_PROBE_OFFSET
#define SPATIAL_HASH_PROBE_OFFSET 0.5f
#endif
    // Probe origin: lift slightly off the surface along its normal, then push
    // toward the CAMERA. The representative surface is the cell's camera-closest
    // hit, so the segment from it toward the camera is unobstructed (visible) —
    // biasing along it keeps the sample center in open space rather than buried
    // in the opposite wall/geometry, without needing a relocation ray.
    float3 toCamera = CameraPosition.xyz - worldPos;
    float camLen = length(toCamera);
    float3 viewDir = (camLen > 1e-4f) ? (toCamera / camLen) : worldNormal;
    float camPush = min(CellSize * SPATIAL_HASH_PROBE_OFFSET, camLen * 0.5f);
    float3 origin = worldPos + worldNormal * RayBias + viewDir * camPush;

    float4 rotation = PerFrameRotationQuaternion(FrameCounter);
    float3 sampleDir = RotateVectorByQuaternion(SphericalFibonacciDir(rayIndex, raysPerProbe), rotation);
    sampleDir = SafeNormalize(sampleDir, worldNormal);

    uint noiseSeed = slot ^ (rayIndex * 1664525u);
    uint2 noiseCoord = uint2(noiseSeed & 1023u, noiseSeed >> 10u);

    float firstHitDistance = MAX_HIT_DIST;
    float3 radiance = TraceDiffusePath(origin, sampleDir, noiseCoord, rayIndex, firstHitDistance);
    radiance = max(SanitizeFloat3(radiance), 0.0f.xxx);

    OctRayData[globalRay] = float4(radiance, firstHitDistance);
}

// One camera-anchored ambient probe: trace a full-sphere SH from the camera and
// temporally blend it. The query evaluates this SH with the pixel normal as the
// fallback for uncached cells — it tracks the local irradiance (indoor/outdoor),
// matching the surrounding GI level far better than a fixed sky colour.
void TraceCameraAmbientProbe()
{
    const uint kCameraProbeRays = 32u;
    float3 origin = CameraPosition.xyz;
    float4 rotation = PerFrameRotationQuaternion(FrameCounter);
    SH4RGB sh = InitSH4RGB();
    [loop]
    for (uint r = 0u; r < kCameraProbeRays; ++r)
    {
        float3 dir = RotateVectorByQuaternion(SphericalFibonacciDir(r, kCameraProbeRays), rotation);
        uint2 noiseCoord = uint2((r * 37u) & 1023u, (r * 53u) & 1023u);
        float ignoredDist;
        float3 rad = TraceDiffusePath(origin, dir, noiseCoord, r, ignoredDist);
        AccumulateSH4RGB(sh, ProjectRadianceToSH4RGB(rad, dir, 4.0f * PI), 1.0f);
    }
    sh = ScaleSH4RGB(sh, rcp(float(kCameraProbeRays)));

    float4 prev0 = CameraProbeSHOut[0];
    float prevFrames = max(prev0.w, 0.0f);
    float accepted = min(prevFrames + 1.0f, 64.0f);
    float alpha = saturate(1.0f / max(accepted, 1.0f));
    CameraProbeSHOut[0] = float4(lerp(prev0.xyz, sh.c0, alpha), accepted);
    CameraProbeSHOut[1] = float4(lerp(CameraProbeSHOut[1].xyz, sh.c1, alpha), 0.0f);
    CameraProbeSHOut[2] = float4(lerp(CameraProbeSHOut[2].xyz, sh.c2, alpha), 0.0f);
    CameraProbeSHOut[3] = float4(lerp(CameraProbeSHOut[3].xyz, sh.c3, alpha), 0.0f);
}

[shader("raygeneration")]
void rayGen()
{
    if (GIMode == 1u)
    {
        RayGenOctahedral();
        return;
    }

    uint traceIndex = DispatchRaysIndex().x;
    // Extra trailing thread used to trace the camera ambient probe. Disabled for
    // the oct miss-fallback test; SH mode keeps its normal cache path unchanged.
    if (traceIndex == HashEntryCount)
    {
        return;
    }
    uint activeCount = min(ActiveCounter[0], ActiveCellCapacity);
    uint traceCount = min(activeCount, HashEntryCount);
    if (traceIndex >= traceCount)
        return;

    uint activeIndex = traceIndex;
    if (activeCount > traceCount)
    {
        uint offset = (FrameCounter * traceCount) % activeCount;
        activeIndex = (traceIndex + offset) % activeCount;
    }

    uint slot = ActiveCellSlots[activeIndex];
    uint key = CellKeys[slot];
    if (key == 0u)
    {
        TraceSH0[traceIndex] = 0.0f.xxxx;
        TraceSH1[traceIndex] = 0.0f.xxxx;
        TraceSH2[traceIndex] = 0.0f.xxxx;
        TraceSH3[traceIndex] = 0.0f.xxxx;
        return;
    }

    float4 cellPosition = CellPosition[slot];
    float4 cellNormal = CellNormal[slot];
    if (cellPosition.w <= 0.0f || cellNormal.w <= 0.0f)
    {
        TraceSH0[traceIndex] = 0.0f.xxxx;
        TraceSH1[traceIndex] = 0.0f.xxxx;
        TraceSH2[traceIndex] = 0.0f.xxxx;
        TraceSH3[traceIndex] = 0.0f.xxxx;
        return;
    }

    float3 worldNormal = SafeNormalize(cellNormal.xyz, float3(0.0f, 1.0f, 0.0f));
    float3 worldPos = cellPosition.xyz;
    // SH4/HL2 normal-bin modes: the cell is split per normal bin, so each slot's
    // stored surface faces one way. Trace a uniform hemisphere around that normal
    // from just off the surface (faster convergence than a full sphere).
    float3 origin = worldPos + worldNormal * RayBias;
    uint rayCount = clamp(RaysPerCell, 1u, 8u);
    uint noiseSeed = slot ^ (traceIndex * 1664525u);
    uint2 baseNoiseCoord = uint2(noiseSeed & 1023u, noiseSeed >> 10u);

    SH4RGB sh = InitSH4RGB();
    SH4RGB hl2 = InitSH4RGB();
    float3 hl2WeightSum = 0.0f.xxx;
    float3 hl2MeanRadiance = 0.0f.xxx;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < 8u; ++sampleIndex)
    {
        if (sampleIndex >= rayCount)
            break;

        uint2 noiseCoord = baseNoiseCoord + uint2(sampleIndex * 17u, sampleIndex * 31u);
        float2 randomUV = GenerateRaySample2D(
            RayNoiseBlueNoiseSource,
            noiseCoord,
            FrameCounter + sampleIndex * 13u,
            BlueNoiseOffsetStride,
            NoiseMode);

        // Hemisphere gather around the cell's stored normal (normal-bin SH).
        float3 sampleDirLocal = SampleUniformHemisphere(randomUV.x, randomUV.y);
        float3 sampleDirWorld = SafeNormalize(mul(sampleDirLocal, BuildTBN(worldNormal)), worldNormal);
        float samplePdf = 1.0f / (2.0f * PI);
        float invPdf = rcp(max(samplePdf, 1e-4f));
        float ignoredHitDistance;
        float3 sampleRadiance = TraceDiffusePath(origin, sampleDirWorld, noiseCoord, sampleIndex, ignoredHitDistance);
        if (GIMode == 2u)
        {
            AccumulateHL2RGB(hl2, hl2WeightSum, sampleRadiance, sampleDirLocal);
            hl2MeanRadiance += max(SanitizeFloat3(sampleRadiance), 0.0f.xxx);
        }
        else
            AccumulateSH4RGB(sh, ProjectRadianceToSH4RGB(sampleRadiance, sampleDirWorld, invPdf), 1.0f);
    }

    if (GIMode == 2u)
    {
        hl2 = NormalizeHL2RGB(hl2, hl2WeightSum, hl2MeanRadiance * rcp(float(rayCount)));
        TraceSH0[traceIndex] = float4(hl2.c0, float(rayCount));
        TraceSH1[traceIndex] = float4(hl2.c1, 0.0f);
        TraceSH2[traceIndex] = float4(hl2.c2, 0.0f);
        TraceSH3[traceIndex] = 0.0f.xxxx;
    }
    else
    {
        sh = ScaleSH4RGB(sh, rcp(float(rayCount)));
        TraceSH0[traceIndex] = float4(sh.c0, float(rayCount));
        TraceSH1[traceIndex] = float4(sh.c1, 0.0f);
        TraceSH2[traceIndex] = float4(sh.c2, 0.0f);
        TraceSH3[traceIndex] = float4(sh.c3, 0.0f);
    }
}

[shader("miss")]
void miss(inout RayPayload payload)
{
    payload.position = 0.0f.xxx;
    payload.color = 0.0f.xxx;
    payload.normal = float3(0.0f, 1.0f, 0.0f);
    payload.bHit = false;
}

[shader("closesthit")]
void chs(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();
    Vertex vertex = GetSurfaceVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);

    payload.position = CommonSanitizeFloat3(vertex.position, WorldRayOrigin() + WorldRayDirection() * RayTCurrent());
    float3 hitNormal = SafeNormalize(vertex.normal, -WorldRayDirection());
    if (dot(hitNormal, -WorldRayDirection()) < 0.0f)
        hitNormal = -hitNormal;
    payload.normal = hitNormal;

    uint w, h;
    AlbedoTex.GetDimensions(w, h);
    float halfLog2NumTexPixels = 0.5f * log2(w * h);
    vertex.textureLODConstant += halfLog2NumTexPixels;
    float hitT = RayTCurrent();
    float rayConeWidth = payload.spreadAngle * hitT + payload.coneWidth;
    float mipLevel = computeTextureLOD(1.0f, rayConeWidth, vertex.textureLODConstant);

    payload.color = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, mipLevel).xyz;
    payload.bHit = true;
}

[shader("miss")]
void missShadow(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}
