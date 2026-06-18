#include "Common.hlsl"
#include "BindlessResources.hlsli"

RWStructuredBuffer<float4> TraceSH0 : register(u0);
RWStructuredBuffer<float4> TraceSH1 : register(u1);
RWStructuredBuffer<float4> TraceSH2 : register(u2);
RWStructuredBuffer<float4> TraceSH3 : register(u3);

RaytracingAccelerationStructure gRtScene : register(t0);
StructuredBuffer<uint> CellKeys : register(t1);
StructuredBuffer<float4> CellPosition : register(t2);
StructuredBuffer<float4> CellNormal : register(t3);
Texture3D RayNoiseBlueNoiseSource : register(t4);
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
    float3 LightColor;
    float _padding;
    uint ActiveCellCapacity;
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
// Read-only view of the resolved octahedral irradiance atlas (previous frame),
// used only to read per-probe convergence (texel 0's .w = accumulated frames) so
// converged cells can be traced at a reduced rate. Bound to the same buffer the
// blend writes; at trace time it is in ShaderRead state.
StructuredBuffer<float4> OctIrradianceConverge : register(t12);

// Convergence-adaptive trace scheduling (P3a). A spatial-hash diffuse-irradiance
// cell barely changes once converged, so re-tracing it every frame is wasted
// work. Young cells (frames < FULL) trace every frame; converged cells trace once
// per REFRESH_PERIOD, phase-staggered by octIndex so the cost spreads across
// frames. Trace and blend evaluate this identically (same atlas frames + frame
// index), so they agree on which cells produce rays each frame.
#define OCT_IRRADIANCE_TEXELS 64u
// Full-rate until ~1.6s @60fps so cells trace every frame through the 1-2s
// convergence window (no convergence-speed regression), then throttle. Near
// cells cap their history below this so they stay full-rate (quality-critical).
#define OCT_BUDGET_FULL_FRAMES 96.0f
#define OCT_BUDGET_REFRESH_PERIOD 4u
bool OctShouldTraceThisFrame(float frames, uint octIndex, uint frameIndex)
{
    if (frames < OCT_BUDGET_FULL_FRAMES)
        return true;
    return (frameIndex % OCT_BUDGET_REFRESH_PERIOD) == (octIndex % OCT_BUDGET_REFRESH_PERIOD);
}

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
    uint cellLevel;
    float cellRadius = max(SpatialHashLeveledCellSize(worldPos, cellLevel), 1e-3f) * 1.7320508f;
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
            float coneRelax = saturate(cellRadius / max(lightDistance, 1e-3f));
            if (cosTheta < light.SpotConeAndFlags.y - coneRelax - 0.05f)
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

// Cell-only oct probe importance ray (must match the blend's copy in
// SpatialHashDiffuseGI.hlsl). Light mostly arrives from the cell normal's
// hemisphere, so the first `cosineCount` rays are cosine-weighted around `n`
// (variance cut where it's queried), the rest uniform-sphere (keeps the
// opposite-facing texels of a multi-orientation cell sampled). Deterministic:
// the blend reconstructs the identical direction + its generating density and
// convolves with weight/density, so the octahedral map stays unbiased.
#define OCT_IMPORTANCE_COSINE_FRAC 0.75f
float3 OctImportanceDir(uint r, uint rayCount, uint cosineCount, float3 n, float4 frameRot, uint frame, out float density)
{
    if (r < cosineCount)
    {
        float u = (float(r) + 0.5f) / float(max(cosineCount, 1u));
        float z = sqrt(saturate(1.0f - u));        // cos(theta); cosine pdf = z/PI
        float rr = sqrt(saturate(u));
        float phi = float(r) * 2.39996323f + float(frame & 1023u) * 0.0613592f; // golden + per-frame
        float3 local = float3(rr * cos(phi), rr * sin(phi), z);
        density = max(z, 0.05f) * (1.0f / PI);      // capped to avoid horizon spikes
        return SafeNormalize(mul(local, BuildTBN(n)), n);
    }
    float3 d = RotateVectorByQuaternion(SphericalFibonacciDir(r - cosineCount, max(rayCount - cosineCount, 1u)), frameRot);
    density = 1.0f / (4.0f * PI);
    return SafeNormalize(d, n);
}

bool MapOctTraceProbeIndex(uint compactProbeIndex, uint activeCount, out uint activeIndex)
{
    uint octSourceCount = min(activeCount, OctCellCapacity);
    uint traceProbeCount = min(octSourceCount, max(HashEntryCount, 1u));
    if (compactProbeIndex >= traceProbeCount)
    {
        activeIndex = 0u;
        return false;
    }

    activeIndex = compactProbeIndex;
    if (octSourceCount > traceProbeCount)
    {
        uint offset = (FrameCounter * traceProbeCount) % octSourceCount;
        activeIndex = (compactProbeIndex + offset) % octSourceCount;
    }
    return true;
}

// Octahedral DDGI trace (GIMode==1): one dispatched thread per compacted
// (probe-window, ray). The active-cell list is already compacted by the update
// pass; this maps a bounded per-frame window over that list so dispatch width is
// tied to useful refresh work instead of the full atlas capacity.
// Each thread traces a single path from the probe's open-space origin and records
// (radiance, hitDistance) to OctRayData. A later compute pass convolves these
// rays into the per-probe octahedral irradiance/depth map.
void RayGenOctahedral()
{
    uint globalRay = DispatchRaysIndex().x;
    uint raysPerProbe = max(OctRaysPerCell, 1u);
    uint probeIndex = globalRay / raysPerProbe;
    uint rayIndex = globalRay % raysPerProbe;

    uint activeCount = min(ActiveCounter[0], ActiveCellCapacity);
    uint activeIndex;
    if (!MapOctTraceProbeIndex(probeIndex, activeCount, activeIndex))
        return;

    uint slot = ActiveCellSlots[activeIndex];
    uint key = CellKeys[slot];
    float4 cellPosition = CellPosition[slot];
    float4 cellNormal = CellNormal[slot];
    if (key == 0u || cellPosition.w <= 0.0f || cellNormal.w <= 0.0f)
    {
        OctRayData[globalRay] = float4(0.0f, 0.0f, 0.0f, MAX_HIT_DIST);
        return;
    }

    // Convergence-adaptive scheduling: a converged cell (high accumulated frames)
    // that is not due for a refresh this frame skips all of its ray work. The
    // blend evaluates the same predicate and keeps the atlas untouched for skipped
    // cells (its ownership stamp is still refreshed so the query stays fresh).
    uint octIndex = slot & (OctCellCapacity - 1u);
    // PointLightPadding.x (P3b) > 0.5 = lighting changed -> force a full-rate trace
    // so converged cells re-trace and re-converge to the new lighting.
    float convergeFrames = OctIrradianceConverge[octIndex * OCT_IRRADIANCE_TEXELS].w;
    if (PointLightPadding.x <= 0.5f && !OctShouldTraceThisFrame(convergeFrames, octIndex, FrameCounter))
        return;

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
    uint cosineCount = (uint)(float(raysPerProbe) * OCT_IMPORTANCE_COSINE_FRAC);
    float ignoredDensity;
    float3 sampleDir = OctImportanceDir(rayIndex, raysPerProbe, cosineCount, worldNormal, rotation, FrameCounter, ignoredDensity);

    uint noiseSeed = slot ^ (rayIndex * 1664525u);
    uint2 noiseCoord = uint2(noiseSeed & 1023u, noiseSeed >> 10u);

    float firstHitDistance = MAX_HIT_DIST;
    float3 radiance = TraceDiffusePath(origin, sampleDir, noiseCoord, rayIndex, firstHitDistance);
    radiance = max(SanitizeFloat3(radiance), 0.0f.xxx);

    OctRayData[globalRay] = float4(radiance, firstHitDistance);
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
    // SH4 (GIMode 0) is now a cell-only full-sphere probe (one SH4 per spatial
    // cell, serves all orientations) — bias the origin toward the camera so the
    // omnidirectional sample center sits in open space (matches oct). HL2 (GIMode
    // 2) stays a normal-binned hemisphere probe (its tangent basis needs a normal).
    float3 origin;
    if (GIMode == 2u)
    {
        origin = worldPos + worldNormal * RayBias;
    }
    else
    {
        float3 toCamera = CameraPosition.xyz - worldPos;
        float camLen = length(toCamera);
        float3 viewDir = (camLen > 1e-4f) ? (toCamera / camLen) : worldNormal;
        float camPush = min(CellSize * 0.5f, camLen * 0.5f);
        origin = worldPos + worldNormal * RayBias + viewDir * camPush;
    }
    // SH4 is full-sphere cell-only: half the rays land below the surface and it has
    // far fewer cells (cell-only key) -> perf headroom spent on extra rays to cut
    // the full-sphere variance (low-frequency blotch). RaysPerCell x 4, min 8, cap 32.
    uint rayCount = clamp(RaysPerCell * 4u, 8u, 32u);
    uint noiseSeed = slot ^ (traceIndex * 1664525u);
    uint2 baseNoiseCoord = uint2(noiseSeed & 1023u, noiseSeed >> 10u);

    SH4RGB sh = InitSH4RGB();
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < 32u; ++sampleIndex)
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

        // SH4 cell-only full-sphere, importance-sampled toward the stored normal
        // hemisphere (where light mostly arrives) to cut variance. MIS mixture:
        // most rays cosine-weighted around the normal, the rest uniform-sphere (so
        // a cell shared by surfaces facing other ways still samples the lower
        // hemisphere). The estimator divides by the MIXTURE pdf -> unbiased full-sphere.
        float3 sampleDirWorld;
        const float kCosineFraction = 0.85f;
        bool useCosine = (float(sampleIndex) + 0.5f) < (kCosineFraction * float(rayCount));
        if (useCosine)
        {
            float r = sqrt(saturate(randomUV.x));
            float phi = 2.0f * PI * randomUV.y;
            float3 local = float3(r * cos(phi), r * sin(phi), sqrt(saturate(1.0f - randomUV.x)));
            sampleDirWorld = SafeNormalize(mul(local, BuildTBN(worldNormal)), worldNormal);
        }
        else
        {
            float z = 1.0f - 2.0f * randomUV.x;
            float rr = sqrt(saturate(1.0f - z * z));
            float phi = 2.0f * PI * randomUV.y;
            sampleDirWorld = float3(rr * cos(phi), rr * sin(phi), z);
        }
        float cosTerm = max(dot(sampleDirWorld, worldNormal), 0.0f);
        float pdf = kCosineFraction * (cosTerm * (1.0f / PI)) +
                    (1.0f - kCosineFraction) * (1.0f / (4.0f * PI));
        // Cap the per-sample weight: grazing/lower-hemisphere dirs have tiny pdf ->
        // huge invPdf -> fireflies/blotch. Bound it (small bias, big variance cut).
        float invPdf = min(1.0f / max(pdf, 1e-4f), 8.0f * PI);

        float ignoredHitDistance;
        float3 sampleRadiance = TraceDiffusePath(origin, sampleDirWorld, noiseCoord, sampleIndex, ignoredHitDistance);
        // Firefly soft-clamp: a single very bright ray, amplified by invPdf, would
        // blotch the low-order SH. Soft-knee compress luminance above a knee.
        float3 rad = max(SanitizeFloat3(sampleRadiance), 0.0f.xxx);
        float luma = dot(rad, float3(0.2126f, 0.7152f, 0.0722f));
        const float kKnee = 6.0f;
        if (luma > kKnee)
            rad *= (luma / (1.0f + (luma - kKnee) / kKnee)) / luma;
        AccumulateSH4RGB(sh, ProjectRadianceToSH4RGB(rad, sampleDirWorld, invPdf), 1.0f);
    }

    sh = ScaleSH4RGB(sh, rcp(float(rayCount)));
    TraceSH0[traceIndex] = float4(sh.c0, float(rayCount));
    TraceSH1[traceIndex] = float4(sh.c1, 0.0f);
    TraceSH2[traceIndex] = float4(sh.c2, 0.0f);
    TraceSH3[traceIndex] = float4(sh.c3, 0.0f);
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
    Vertex vertex = CORONA_GET_SURFACE_VERTEX_ATTRIBUTES(instanceID, triangleIndex, barycentrics);

    payload.position = CommonSanitizeFloat3(vertex.position, WorldRayOrigin() + WorldRayDirection() * RayTCurrent());
    float3 hitNormal = SafeNormalize(vertex.normal, -WorldRayDirection());
    if (dot(hitNormal, -WorldRayDirection()) < 0.0f)
        hitNormal = -hitNormal;
    payload.normal = hitNormal;

    RTMaterialRecord material = RtMaterials[instanceID];
    uint w, h;
    MaterialTextures[NonUniformResourceIndex(material.AlbedoTextureIndex)].GetDimensions(w, h);

    float halfLog2NumTexPixels = 0.5f * log2(w * h);
    vertex.textureLODConstant += halfLog2NumTexPixels;
    float hitT = RayTCurrent();
    float rayConeWidth = payload.spreadAngle * hitT + payload.coneWidth;
    float mipLevel = computeTextureLOD(1.0f, rayConeWidth, vertex.textureLODConstant);

    payload.color = MaterialTextures[NonUniformResourceIndex(material.AlbedoTextureIndex)].SampleLevel(sampleWrap, vertex.uv, mipLevel).xyz * material.BaseColorFactor.xyz;

    payload.bHit = true;
}

[shader("miss")]
void missShadow(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}
