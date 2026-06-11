#include "Common.hlsl"

RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
ByteAddressBuffer vertices : register(t2);
ByteAddressBuffer indices : register(t3);
ByteAddressBuffer InstanceProperty : register(t4);

RWStructuredBuffer<uint> ActiveFlagsOut : register(u0);
RWStructuredBuffer<float4> CellPositionOut : register(u1);
RWStructuredBuffer<float4> CellNormalOut : register(u2);
RWStructuredBuffer<uint> CellScoreOut : register(u3);
RWStructuredBuffer<uint> ResolvedKeysOut : register(u4);
RWStructuredBuffer<float4> ResolvedSH0Out : register(u5);
RWStructuredBuffer<uint> ActiveCellSlotsOut : register(u6);
RWStructuredBuffer<uint> ActiveCounterOut : register(u7);
RWStructuredBuffer<uint> CellLightMaskOut : register(u8);

#define RT_DIFFUSE_GI_MAX_POINT_LIGHTS 16

struct PointLightParam
{
    float4 PositionAndRadius;
    float4 ColorAndIntensity;
    float4 DirectionAndType;
    float4 SpotConeAndFlags;
};

cbuffer SpatialHashGIConstant : register(b0)
{
    float4x4 InvViewMatrix;
    float4x4 InvProjMatrix;
    float4 ProjectionParams;
    float2 RTSize;
    float CellSize;
    float HistorySampleDecay;
    uint HashEntryCount;
    uint HashEntryMask;
    uint FrameIndex;
    uint HistoryValid;
    float TemporalAlpha;
    float SmoothingStrength;
    uint MaxProbeSteps;
    float InterpolationStrength;
    uint ActiveCellCapacity;
    uint TraceCellBudget;
    uint GIMode;
    uint OctCellCapacity;
    PointLightParam PointLights[RT_DIFFUSE_GI_MAX_POINT_LIGHTS];
    uint PointLightCount;
    float OctNearConvergenceBias;
    float EvictDistanceWeight;
    float LightingChangedFlag;
    float4 DebugDiffuseGIOverride;
    float4 SpatialHashLevelParams;
    float4 SpatialHashSkyAmbient;
};

cbuffer PrimaryDeepSeedConstant : register(b1)
{
    uint DeepSeedPixelStride;
    uint DeepSeedFrameIndex;
    uint2 DeepSeedPadding;
};

static const uint SPATIAL_HASH_ACTIVE_INIT = 0xffffffffu;
#ifndef SPATIAL_HASH_MAX_CELL_AGE_FRAMES
#define SPATIAL_HASH_MAX_CELL_AGE_FRAMES 1024u
#endif

struct DeepSeedPayload
{
    float3 position;
    float3 normal;
    uint bHit;
    uint _padding;
};

float3 SanitizeFloat3(float3 value)
{
    if (any(isnan(value)) || any(isinf(value)))
        return 0.0f.xxx;
    return value;
}

float4 SanitizeFloat4(float4 value)
{
    if (any(isnan(value)) || any(isinf(value)))
        return 0.0f.xxxx;
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

float3 GetCameraPosition()
{
    return mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
}

float3 OrientNormalTowardView(float3 normal, float3 worldPos)
{
    float3 cameraPos = GetCameraPosition();
    float3 viewDir = SafeNormalize(cameraPos - worldPos, normal);
    normal = SafeNormalize(normal, viewDir);
    return dot(normal, viewDir) < 0.0f ? -normal : normal;
}

float3 ReconstructWorldPosition(uint2 pixelPos)
{
    float deviceDepth = DepthTex[pixelPos].x;
    float2 screenPosition = (float2(pixelPos) + 0.5f.xx) / RTSize;
    screenPosition = screenPosition * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;
    float3 viewPosition = GetViewPosition(deviceDepth, screenPosition, InvProjMatrix);
    return mul(float4(viewPosition, 1.0f), InvViewMatrix).xyz;
}

float SpatialHashLeveledCellSize(float3 worldPos, out uint level)
{
    float cs = max(CellSize, 1e-3f);
    level = 0u;
    if (SpatialHashLevelParams.x < 0.5f)
        return cs;

    float3 cameraPos = GetCameraPosition();
    float dist = length(worldPos - cameraPos);
    float fl = floor(max(log2(max(dist / max(SpatialHashLevelParams.y, 1e-3f), 1.0f)), 0.0f));
    level = (uint)fl;
    return cs * exp2(fl);
}

int3 SpatialHashLevelOffset(uint level)
{
    return int3(level, level, level) * int3(1737, 9277, 4513);
}

int3 GetSpatialHashCell(float3 worldPos)
{
    uint level;
    float cs = SpatialHashLeveledCellSize(worldPos, level);
    return int3(floor(worldPos / cs)) + SpatialHashLevelOffset(level);
}

int ComputePlaneBin(float3 worldPos, float3 normal)
{
    uint level;
    float cs = SpatialHashLeveledCellSize(worldPos, level);
    normal = SafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    return int(floor((dot(worldPos, normal) / cs) * 2.0f + 0.5f));
}

uint HashCellKeyFromCell(int3 cell, float3 normal, int planeBin)
{
    uint h = uint(cell.x) * 73856093u;
    h ^= uint(cell.y) * 19349663u;
    h ^= uint(cell.z) * 83492791u;
    h = HashUInt(h);
    return h == 0u ? 1u : h;
}

uint HashCellKey(float3 worldPos, float3 normal)
{
    return HashCellKeyFromCell(GetSpatialHashCell(worldPos), normal, ComputePlaneBin(worldPos, normal));
}

uint HashInitialSlot(uint key)
{
    return HashUInt(key ^ 0x9e3779b9u) & HashEntryMask;
}

float ComputeLightLuma(float3 color)
{
    return dot(max(color, 0.0f.xxx), float3(0.2126f, 0.7152f, 0.0722f));
}

uint ComputeCellLightMask(float3 worldPos, float3 normal)
{
    uint mask = 0u;
    uint activeCount = min(PointLightCount, (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS);
    float cellRadius = max(CellSize, 1e-3f) * 1.7320508f;
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
        if (lightDistance > range + cellRadius)
            continue;

        float3 cellToLightDir = toLight / lightDistance;
        if (light.DirectionAndType.w >= 0.5f)
        {
            float3 spotDir = SafeNormalize(light.DirectionAndType.xyz, float3(0.0f, 1.0f, 0.0f));
            float cosTheta = dot(spotDir, -cellToLightDir);
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

bool FindSlotForWrite(uint key, out uint slot, out bool inserted)
{
    uint startSlot = HashInitialSlot(key);
    uint probeCount = clamp(MaxProbeSteps, 1u, 16u);

    [loop]
    for (uint probeIndex = 0u; probeIndex < 16u; ++probeIndex)
    {
        if (probeIndex >= probeCount)
            break;

        uint candidate = (startSlot + probeIndex) & HashEntryMask;
        uint oldValue = 0u;
        InterlockedCompareExchange(ResolvedKeysOut[candidate], 0u, key, oldValue);
        if (oldValue == 0u || oldValue == key)
        {
            slot = candidate;
            inserted = oldValue == 0u;
            return true;
        }
    }

    uint curStamp = (FrameIndex & 0x7fffffffu) + 1u;
    if (curStamp == SPATIAL_HASH_ACTIVE_INIT)
        curStamp = 1u;

    float3 evCamPos = GetCameraPosition();
    float evFar = max(ProjectionParams.w, 1.0f);
    float evDistW = saturate(EvictDistanceWeight);
    uint victim = 0u;
    uint victimKey = 0u;
    float bestEvictScore = -1.0f;
    [loop]
    for (uint p = 0u; p < 16u; ++p)
    {
        if (p >= probeCount)
            break;
        uint candidate = (startSlot + p) & HashEntryMask;
        uint stamp = ActiveFlagsOut[candidate];
        if (stamp == SPATIAL_HASH_ACTIVE_INIT || stamp == 0u || stamp == curStamp)
            continue;

        float ageNorm = saturate(float(curStamp - stamp) / float(SPATIAL_HASH_MAX_CELL_AGE_FRAMES));
        float3 cpos = SanitizeFloat4(CellPositionOut[candidate]).xyz;
        float distNorm = saturate(length(cpos - evCamPos) / evFar);
        float evictScore = distNorm * evDistW + ageNorm * (1.0f - evDistW);
        if (evictScore >= bestEvictScore)
        {
            bestEvictScore = evictScore;
            victim = candidate;
            victimKey = ResolvedKeysOut[candidate];
        }
    }

    if (victimKey != 0u)
    {
        uint prevKey = 0u;
        InterlockedCompareExchange(ResolvedKeysOut[victim], victimKey, key, prevKey);
        if (prevKey == victimKey || prevKey == key)
        {
            ResolvedSH0Out[victim] = 0.0f.xxxx;
            slot = victim;
            inserted = true;
            return true;
        }
    }

    slot = 0u;
    inserted = false;
    return false;
}

uint GetActiveFrameStamp()
{
    uint frameStamp = (FrameIndex & 0x7fffffffu) + 1u;
    return frameStamp == SPATIAL_HASH_ACTIVE_INIT ? 1u : frameStamp;
}

void MarkActiveSlot(uint slot)
{
    uint frameStamp = GetActiveFrameStamp();

    [loop]
    for (uint attempt = 0u; attempt < 8u; ++attempt)
    {
        uint observed = 0u;
        InterlockedCompareExchange(ActiveFlagsOut[slot], frameStamp, frameStamp, observed);
        if (observed == frameStamp)
            return;

        if (observed == SPATIAL_HASH_ACTIVE_INIT)
            continue;

        uint previous = 0u;
        InterlockedCompareExchange(ActiveFlagsOut[slot], observed, SPATIAL_HASH_ACTIVE_INIT, previous);
        if (previous != observed)
            continue;

        CellScoreOut[slot] = 0xffffffffu;
        ActiveFlagsOut[slot] = frameStamp;

        uint activeIndex = 0u;
        InterlockedAdd(ActiveCounterOut[0], 1u, activeIndex);
        if (activeIndex < ActiveCellCapacity)
            ActiveCellSlotsOut[activeIndex] = slot;
        return;
    }
}

bool ShouldTracePixel(uint2 pixelPos)
{
    uint stride = clamp(DeepSeedPixelStride, 1u, 16u);
    if (stride <= 1u)
        return true;

    uint sampleCount = stride * stride;
    uint seed = pixelPos.x * 1973u + pixelPos.y * 9277u + DeepSeedFrameIndex * 26699u;
    return (HashUInt(seed) % sampleCount) == 0u;
}

void InsertDeepSeedCell(float3 worldPos, float3 normal, uint2 pixelPos)
{
    normal = OrientNormalTowardView(normal, worldPos);
    uint key = HashCellKey(worldPos, normal);

    uint slot = 0u;
    bool inserted = false;
    if (!FindSlotForWrite(key, slot, inserted))
        return;

    MarkActiveSlot(slot);

    float normalizedDist;
    if (GIMode == 1u)
    {
        float camDist = length(worldPos - GetCameraPosition());
        normalizedDist = saturate(camDist / max(ProjectionParams.w, 1.0f));
    }
    else
    {
        uint lvl;
        float cs = SpatialHashLeveledCellSize(worldPos, lvl);
        float3 cellCenter = (floor(worldPos / cs) + 0.5f.xxx) * cs;
        normalizedDist = saturate(length((worldPos - cellCenter) / cs) * 1.1547005f);
    }

    uint score = min(uint(normalizedDist * 16777215.0f), 16777215u);
    score = (score << 8u) | (HashUInt(pixelPos.x * 3011u + pixelPos.y * 1741u + FrameIndex * 4357u) & 255u);

    uint oldScore = 0xffffffffu;
    InterlockedMin(CellScoreOut[slot], score, oldScore);
    if (score <= oldScore)
    {
        CellPositionOut[slot] = float4(worldPos, 1.0f);
        CellNormalOut[slot] = float4(normal, 1.0f);
        CellLightMaskOut[slot] = ComputeCellLightMask(worldPos, normal);
    }
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixelPos = DispatchRaysIndex().xy;
    uint2 textureSize = uint2(RTSize);
    if (pixelPos.x >= textureSize.x || pixelPos.y >= textureSize.y)
        return;
    if (!ShouldTracePixel(pixelPos))
        return;

    float deviceDepth = DepthTex[pixelPos].x;
    if (deviceDepth >= 0.99999f)
        return;

    float3 firstHitPos = ReconstructWorldPosition(pixelPos);
    float3 cameraPos = GetCameraPosition();
    float3 rayDir = SafeNormalize(firstHitPos - cameraPos, float3(0.0f, 0.0f, 1.0f));
    float firstHitDistance = length(firstHitPos - cameraPos);
    if (firstHitDistance <= 1.0e-3f)
        return;

    float rayBias = clamp(CellSize * 0.02f, 0.05f, 1.0f);
    RayDesc ray;
    ray.Origin = cameraPos;
    ray.Direction = rayDir;
    ray.TMin = firstHitDistance + rayBias;
    ray.TMax = max(ProjectionParams.w, firstHitDistance + CellSize * 8.0f);

    DeepSeedPayload payload;
    payload.position = 0.0f.xxx;
    payload.normal = float3(0.0f, 1.0f, 0.0f);
    payload.bHit = 0u;
    payload._padding = 0u;

    TraceRay(
        gRtScene,
        RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        0xFF,
        0,
        0,
        0,
        ray,
        payload);

    if (payload.bHit == 0u)
        return;

    InsertDeepSeedCell(payload.position, payload.normal, pixelPos);
}

[shader("closesthit")]
void chs(inout DeepSeedPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(
        1.0f - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y);
    Vertex vertex = GetSurfaceVertexAttributes(
        InstanceID(),
        vertices,
        indices,
        InstanceProperty,
        PrimitiveIndex(),
        barycentrics);

    payload.position = CommonSanitizeFloat3(vertex.position, WorldRayOrigin() + WorldRayDirection() * RayTCurrent());
    float3 hitNormal = SafeNormalize(vertex.normal, -WorldRayDirection());
    if (dot(hitNormal, -WorldRayDirection()) < 0.0f)
        hitNormal = -hitNormal;
    payload.normal = hitNormal;
    payload.bHit = 1u;
}

[shader("miss")]
void miss(inout DeepSeedPayload payload)
{
    payload.bHit = 0u;
}
