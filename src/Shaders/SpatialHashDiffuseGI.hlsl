#include "Common.hlsl"

Texture2D DepthTex : register(t0);
Texture2D WorldNormalTex : register(t1);
Texture2D GeoNormalTex : register(t2);

StructuredBuffer<uint> ActiveCellSlotsIn : register(t5);
StructuredBuffer<float4> CellPositionIn : register(t6);
StructuredBuffer<float4> CellNormalIn : register(t7);
StructuredBuffer<float4> TraceSH0In : register(t8);
StructuredBuffer<float4> TraceSH1In : register(t9);
StructuredBuffer<float4> TraceSH2In : register(t10);
StructuredBuffer<float4> TraceSH3In : register(t11);
StructuredBuffer<uint> PrevResolvedKeys : register(t12);
StructuredBuffer<float4> PrevResolvedSH0 : register(t13);
StructuredBuffer<float4> PrevResolvedSH1 : register(t14);
StructuredBuffer<float4> PrevResolvedSH2 : register(t15);
StructuredBuffer<float4> PrevResolvedSH3 : register(t16);
StructuredBuffer<uint> ResolvedKeysIn : register(t17);
StructuredBuffer<float4> ResolvedSH0In : register(t18);
StructuredBuffer<float4> ResolvedSH1In : register(t19);
StructuredBuffer<float4> ResolvedSH2In : register(t20);
StructuredBuffer<float4> ResolvedSH3In : register(t21);
StructuredBuffer<uint> ActiveCounterIn : register(t22);
// Disocclusion detection inputs for SpatialHashQuery: velocity +
// previous-frame depth/normal. Used to fully reset history to the
// ambient fallback when the current pixel didn't exist on the
// previous frame (camera-pan disocclusion) so cached neighbour
// cells from other surfaces can't leak in.
Texture2D VelocityTex : register(t23);
Texture2D PrevDepthTex : register(t24);
Texture2D PrevNormalTex : register(t25);

RWStructuredBuffer<uint> ActiveFlagsOut : register(u0);
RWStructuredBuffer<float4> CellPositionOut : register(u1);
RWStructuredBuffer<float4> CellNormalOut : register(u2);
RWStructuredBuffer<uint> CellScoreOut : register(u3);
RWStructuredBuffer<uint> ResolvedKeysOut : register(u4);
RWStructuredBuffer<float4> ResolvedSH0Out : register(u5);
RWStructuredBuffer<float4> ResolvedSH1Out : register(u6);
RWStructuredBuffer<float4> ResolvedSH2Out : register(u7);
RWStructuredBuffer<float4> ResolvedSH3Out : register(u8);
RWTexture2D<float4> OutGIHashColor : register(u9);
RWTexture2D<float4> OutGIHashSH : register(u10);
RWStructuredBuffer<uint> ActiveCellSlotsOut : register(u11);
RWStructuredBuffer<uint> ActiveCounterOut : register(u12);
RWStructuredBuffer<uint> CellLightMaskOut : register(u14);

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
    uint _padding4;
    uint _padding5;
    PointLightParam PointLights[RT_DIFFUSE_GI_MAX_POINT_LIGHTS];
    uint PointLightCount;
    float3 PointLightPadding;
    float4 DebugDiffuseGIOverride;
};

struct SH4RGB
{
    float3 c0;
    float3 c1;
    float3 c2;
    float3 c3;
};

static const float MAX_SPATIAL_HASH_HISTORY_SAMPLES = 4096.0f;
static const float SPATIAL_HASH_DIFFUSE_SCALE = 1.0f / PI;
static const uint SPATIAL_HASH_ACTIVE_INIT = 0xffffffffu;
// Cells unseen for this many frames are aged out by the clear pass so the
// table can't saturate over a long session (saturation starves newly-visible
// cells and makes GI progressively vanish). ~17 s @ 60 fps keeps off-screen
// cells alive well past typical look-away, then reclaims them. Tunable.
#ifndef SPATIAL_HASH_MAX_CELL_AGE_FRAMES
#define SPATIAL_HASH_MAX_CELL_AGE_FRAMES 1024u
#endif
static const float SPATIAL_HASH_MIN_SURFACE_NORMAL_DOT = 0.72f;
static const float SPATIAL_HASH_FULL_SURFACE_NORMAL_DOT = 0.92f;
static const float SPATIAL_HASH_PLANE_REJECT_CELL_SCALE = 0.45f;
static const float SPATIAL_HASH_PLANE_SOFT_CELL_SCALE = 0.25f;

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
        if (dot(normal, cellToLightDir) <= 0.0f)
            continue;

        if (light.DirectionAndType.w >= 0.5f)
        {
            float3 spotDir = SafeNormalize(light.DirectionAndType.xyz, float3(0.0f, 1.0f, 0.0f));
            float cosTheta = dot(spotDir, -cellToLightDir);
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

SH4RGB InitSH4RGB()
{
    SH4RGB sh;
    sh.c0 = 0.0f.xxx;
    sh.c1 = 0.0f.xxx;
    sh.c2 = 0.0f.xxx;
    sh.c3 = 0.0f.xxx;
    return sh;
}

SH4RGB SanitizeSH(SH4RGB sh)
{
    sh.c0 = SanitizeFloat3(sh.c0);
    sh.c1 = SanitizeFloat3(sh.c1);
    sh.c2 = SanitizeFloat3(sh.c2);
    sh.c3 = SanitizeFloat3(sh.c3);
    return sh;
}

SH4RGB AddSH(SH4RGB a, SH4RGB b)
{
    a.c0 += b.c0;
    a.c1 += b.c1;
    a.c2 += b.c2;
    a.c3 += b.c3;
    return a;
}

SH4RGB ScaleSH(SH4RGB sh, float scale)
{
    sh.c0 *= scale;
    sh.c1 *= scale;
    sh.c2 *= scale;
    sh.c3 *= scale;
    return sh;
}

SH4RGB LerpSH(SH4RGB a, SH4RGB b, float t)
{
    SH4RGB sh;
    sh.c0 = lerp(a.c0, b.c0, t);
    sh.c1 = lerp(a.c1, b.c1, t);
    sh.c2 = lerp(a.c2, b.c2, t);
    sh.c3 = lerp(a.c3, b.c3, t);
    return sh;
}

float SHAbsEnergy(SH4RGB sh)
{
    return dot(abs(sh.c0), 1.0f.xxx) +
        dot(abs(sh.c1), 1.0f.xxx) +
        dot(abs(sh.c2), 1.0f.xxx) +
        dot(abs(sh.c3), 1.0f.xxx);
}

float SHDeltaEnergy(SH4RGB a, SH4RGB b)
{
    return dot(abs(a.c0 - b.c0), 1.0f.xxx) +
        dot(abs(a.c1 - b.c1), 1.0f.xxx) +
        dot(abs(a.c2 - b.c2), 1.0f.xxx) +
        dot(abs(a.c3 - b.c3), 1.0f.xxx);
}

float Luminance(float3 value)
{
    return dot(max(value, 0.0f.xxx), float3(0.2126f, 0.7152f, 0.0722f));
}

float ComputeAdaptiveHistorySampleDecay(SH4RGB previousSH, SH4RGB currentSH, float requestedDecay)
{
    requestedDecay = saturate(requestedDecay);
    if (requestedDecay >= 0.999f)
        return 1.0f;

    float previousLuma = Luminance(abs(previousSH.c0));
    float currentLuma = Luminance(abs(currentSH.c0));
    float lumaMax = max(previousLuma, currentLuma);
    float lumaDelta = abs(currentLuma - previousLuma);
    float coeffDelta = SHDeltaEnergy(previousSH, currentSH);
    float coeffEnergy = max(SHAbsEnergy(previousSH), SHAbsEnergy(currentSH));

    // Do not drop confidence globally on a light edit. Dark or statistically
    // unchanged cells should keep integrating; only changed cells adapt fast.
    float absoluteChange = max(lumaDelta, coeffDelta * 0.0833333f);
    float absoluteGate = smoothstep(0.0025f, 0.035f, absoluteChange);
    float relativeGate = smoothstep(0.25f, 1.25f, absoluteChange / max(max(lumaMax, coeffEnergy * 0.0833333f), 0.025f));
    float changeWeight = saturate(absoluteGate * relativeGate);

    return lerp(1.0f, requestedDecay, changeWeight);
}

float3 EvaluateSHDiffuse(SH4RGB sh, float3 normal)
{
    normal = SafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));

    float x = normal.x;
    float y = normal.y;
    float z = normal.z;
    float b0 = 0.282095f;
    float b1 = 0.488603f * y;
    float b2 = 0.488603f * z;
    float b3 = 0.488603f * x;

    float3 irradiance =
        PI * sh.c0 * b0 +
        (2.0f * PI / 3.0f) * (sh.c1 * b1 + sh.c2 * b2 + sh.c3 * b3);

    return max(SanitizeFloat3(irradiance), 0.0f.xxx);
}

SH4RGB LoadTraceSH(uint entryIndex)
{
    SH4RGB sh;
    sh.c0 = SanitizeFloat3(TraceSH0In[entryIndex].xyz);
    sh.c1 = SanitizeFloat3(TraceSH1In[entryIndex].xyz);
    sh.c2 = SanitizeFloat3(TraceSH2In[entryIndex].xyz);
    sh.c3 = SanitizeFloat3(TraceSH3In[entryIndex].xyz);
    return sh;
}

float LoadTraceSampleCount(uint entryIndex)
{
    return max(SanitizeFloat4(TraceSH0In[entryIndex]).w, 1.0f);
}

SH4RGB LoadPrevResolvedSH(uint entryIndex)
{
    SH4RGB sh;
    sh.c0 = SanitizeFloat3(PrevResolvedSH0[entryIndex].xyz);
    sh.c1 = SanitizeFloat3(PrevResolvedSH1[entryIndex].xyz);
    sh.c2 = SanitizeFloat3(PrevResolvedSH2[entryIndex].xyz);
    sh.c3 = SanitizeFloat3(PrevResolvedSH3[entryIndex].xyz);
    return sh;
}

SH4RGB LoadResolvedSH(uint entryIndex, out float historyFrames)
{
    float4 sh0 = SanitizeFloat4(ResolvedSH0In[entryIndex]);
    SH4RGB sh;
    sh.c0 = sh0.xyz;
    sh.c1 = SanitizeFloat3(ResolvedSH1In[entryIndex].xyz);
    sh.c2 = SanitizeFloat3(ResolvedSH2In[entryIndex].xyz);
    sh.c3 = SanitizeFloat3(ResolvedSH3In[entryIndex].xyz);
    historyFrames = max(sh0.w, 0.0f);
    return sh;
}

void StoreResolvedSH(uint entryIndex, SH4RGB sh, float historyFrames)
{
    sh = SanitizeSH(sh);
    ResolvedSH0Out[entryIndex] = float4(sh.c0, max(historyFrames, 0.0f));
    ResolvedSH1Out[entryIndex] = float4(sh.c1, 0.0f);
    ResolvedSH2Out[entryIndex] = float4(sh.c2, 0.0f);
    ResolvedSH3Out[entryIndex] = float4(sh.c3, 0.0f);
}

float3 LoadPixelNormal(uint2 pixelPos)
{
    float3 geoNormal = SafeNormalize(GeoNormalTex[pixelPos].xyz, float3(0.0f, 1.0f, 0.0f));
    return SafeNormalize(WorldNormalTex[pixelPos].xyz, geoNormal);
}

float3 LoadCacheNormal(uint2 pixelPos)
{
    float3 pixelNormal = LoadPixelNormal(pixelPos);
    return SafeNormalize(GeoNormalTex[pixelPos].xyz, pixelNormal);
}

float3 OrientNormalTowardView(float3 normal, float3 worldPos)
{
    float3 cameraPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
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

int3 GetSpatialHashCell(float3 worldPos)
{
    float safeCellSize = max(CellSize, 1e-3f);
    return int3(floor(worldPos / safeCellSize));
}

uint EncodeNormalBits(float3 normal)
{
    normal = SafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    uint3 normalBin = uint3(saturate(normal * 0.5f + 0.5f) * 7.0f + 0.5f);
    return (normalBin.x & 7u) | ((normalBin.y & 7u) << 3u) | ((normalBin.z & 7u) << 6u);
}

int ComputePlaneBin(float3 worldPos, float3 normal)
{
    float safeCellSize = max(CellSize, 1e-3f);
    normal = SafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    // Half-cell plane bins let one xyz cell keep separate representatives
    // for parallel or near-parallel surfaces, while the lookup still checks
    // +/- one bin so cache hits do not disappear at quantization boundaries.
    return int(floor((dot(worldPos, normal) / safeCellSize) * 2.0f + 0.5f));
}

// One SH per spatial cell: key depends on the cell coordinate ONLY (no normal
// or plane bin). Directionality is recovered by evaluating the cell's
// full-sphere SH with each surface's own normal at query time, which removes
// the per-bin comb on curved/slanted surfaces. MUST stay identical to the cell
// (trace) shader's copy so insert and lookup agree. normal/planeBin params are
// kept for call-site compatibility and ignored.
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

    // Probe window is full of other cells. Rather than fail (which left a
    // newly-visible cell black for many seconds until its window aged out),
    // evict the LEAST-recently-seen slot in the window (LRU) so the new cell is
    // allocated THIS frame. Key-REPLACEMENT (the slot stays non-zero) keeps the
    // linear-probe chain intact: reads for the evicted key correctly miss, and
    // reads for other keys still probe past this slot. We zero the victim's
    // ResolvedSH0 (history lives in .w) so the resolve treats it as fresh and
    // does not blend the evicted cell's radiance into the new one.
    // Inlined frame stamp (GetActiveFrameStamp is defined later in the file).
    uint curStamp = (FrameIndex & 0x7fffffffu) + 1u;
    if (curStamp == SPATIAL_HASH_ACTIVE_INIT)
        curStamp = 1u;
    uint victim = 0u;
    uint victimKey = 0u;
    uint oldestAge = 0u;
    [loop]
    for (uint p = 0u; p < 16u; ++p)
    {
        if (p >= probeCount)
            break;
        uint candidate = (startSlot + p) & HashEntryMask;
        uint stamp = ActiveFlagsOut[candidate];
        // Skip slots being set up (mid-init / not yet stamped) and slots already
        // marked active this frame — evicting those would thrash live cells.
        if (stamp == SPATIAL_HASH_ACTIVE_INIT || stamp == 0u || stamp == curStamp)
            continue;
        uint a = curStamp - stamp; // larger = older / least-recently-seen
        if (a >= oldestAge)
        {
            oldestAge = a;
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
            ResolvedSH0Out[victim] = 0.0f.xxxx; // history = 0 => resolve replaces
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

bool FindSlotForRead(uint key, out uint slot)
{
    uint startSlot = HashInitialSlot(key);
    uint probeCount = clamp(MaxProbeSteps, 1u, 16u);

    [loop]
    for (uint probeIndex = 0u; probeIndex < 16u; ++probeIndex)
    {
        if (probeIndex >= probeCount)
            break;

        uint candidate = (startSlot + probeIndex) & HashEntryMask;
        uint storedKey = ResolvedKeysIn[candidate];
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

bool FindPrevSlotForRead(uint key, out uint slot)
{
    uint startSlot = HashInitialSlot(key);
    uint probeCount = clamp(MaxProbeSteps, 1u, 16u);

    [loop]
    for (uint probeIndex = 0u; probeIndex < 16u; ++probeIndex)
    {
        if (probeIndex >= probeCount)
            break;

        uint candidate = (startSlot + probeIndex) & HashEntryMask;
        uint storedKey = PrevResolvedKeys[candidate];
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

float ComputeSurfaceLobeWeight(uint slot, float3 queryWorldPos, float3 queryNormal)
{
    float4 storedPosition = SanitizeFloat4(CellPositionIn[slot]);
    if (storedPosition.w <= 0.0f)
        return 0.0f;

    // One full-sphere SH probe per cell serves every surface orientation, so we
    // no longer reject by normal or plane (that per-bin matching was the comb
    // source). Weight purely by distance from the probe's representative surface
    // point: a far surface that happens to share the hash cell contributes less
    // than the near one, and the trilinear/gather blend stays smooth.
    float safeCellSize = max(CellSize, 1e-3f);
    float dist = length(queryWorldPos - storedPosition.xyz);
    return saturate(1.0f - dist / (safeCellSize * 1.5f));
}

bool LoadCachedSHForCell(int3 cell, float3 normal, float3 queryWorldPos,
                          out SH4RGB sh, out float historyFrames, out float bilateralWeight)
{
    bilateralWeight = 0.0f;
    sh = InitSH4RGB();
    historyFrames = 0.0f;

    // One SH per spatial cell -> a single lookup (no plane-bin / normal-bin
    // search). The cell's full-sphere SH is evaluated with the surface normal by
    // the caller, so directionality is preserved without per-bin cells, and the
    // comb is gone.
    uint slot = 0u;
    uint key = HashCellKeyFromCell(cell, normal, 0);
    if (!FindSlotForRead(key, slot))
        return false;

    float surfaceWeight = ComputeSurfaceLobeWeight(slot, queryWorldPos, normal);
    if (surfaceWeight <= 1e-4f)
        return false;

    float candidateHistoryFrames = 0.0f;
    SH4RGB candidateSH = LoadResolvedSH(slot, candidateHistoryFrames);
    if (candidateHistoryFrames <= 0.0f || SHAbsEnergy(candidateSH) <= 1e-7f)
        return false;

    sh = candidateSH;
    historyFrames = candidateHistoryFrames;
    bilateralWeight = surfaceWeight;
    return true;
}

bool LoadInterpolatedSH(float3 worldPos, float3 normal, out SH4RGB outSH, out float outHistoryFrames)
{
    float safeCellSize = max(CellSize, 1e-3f);
    float3 gridPos = worldPos / safeCellSize;
    float3 baseCellFloat = floor(gridPos);
    int3 baseCell = int3(baseCellFloat);
    float3 cellFrac = saturate(gridPos - baseCellFloat);

    SH4RGB weightedSH = InitSH4RGB();
    float weightedFrames = 0.0f;
    float validWeight = 0.0f;

    [unroll]
    for (uint z = 0u; z < 2u; ++z)
    {
        float wz = z == 0u ? (1.0f - cellFrac.z) : cellFrac.z;
        [unroll]
        for (uint y = 0u; y < 2u; ++y)
        {
            float wy = y == 0u ? (1.0f - cellFrac.y) : cellFrac.y;
            [unroll]
            for (uint x = 0u; x < 2u; ++x)
            {
                float wx = x == 0u ? (1.0f - cellFrac.x) : cellFrac.x;
                float weight = wx * wy * wz;
                if (weight <= 0.0f)
                    continue;

                SH4RGB cachedSH = InitSH4RGB();
                float cachedFrames = 0.0f;
                float bw = 0.0f;
                if (LoadCachedSHForCell(baseCell + int3(x, y, z), normal, worldPos, cachedSH, cachedFrames, bw))
                {
                    float bilateralWeightedWeight = weight * bw;
                    weightedSH = AddSH(weightedSH, ScaleSH(cachedSH, bilateralWeightedWeight));
                    weightedFrames += cachedFrames * bilateralWeightedWeight;
                    validWeight += bilateralWeightedWeight;
                }
            }
        }
    }

    if (validWeight <= 1e-5f)
    {
        outSH = InitSH4RGB();
        outHistoryFrames = 0.0f;
        return false;
    }

    float invWeight = rcp(validWeight);
    SH4RGB interpolatedSH = ScaleSH(weightedSH, invWeight);
    float interpolatedFrames = weightedFrames * invWeight;

    SH4RGB baseSH = InitSH4RGB();
    float baseFrames = 0.0f;
    float baseBw = 0.0f;
    if (LoadCachedSHForCell(baseCell, normal, worldPos, baseSH, baseFrames, baseBw))
    {
        float interpolation = saturate(InterpolationStrength) * saturate(baseBw);
        interpolatedSH = LerpSH(baseSH, interpolatedSH, interpolation);
        interpolatedFrames = lerp(baseFrames, interpolatedFrames, interpolation);
    }

    outSH = interpolatedSH;
    outHistoryFrames = interpolatedFrames * saturate(validWeight);
    return true;
}

bool LoadSmoothedSH(float3 worldPos, float3 normal, out SH4RGB outSH, out float outHistoryFrames)
{
    SH4RGB baseSH = InitSH4RGB();
    float baseFrames = 0.0f;
    bool hasBase = LoadInterpolatedSH(worldPos, normal, baseSH, baseFrames);

    float smooth = saturate(SmoothingStrength);
    if (smooth <= 1e-4f)
    {
        outSH = baseSH;
        outHistoryFrames = baseFrames;
        return hasBase;
    }

    float safeCellSize = max(CellSize, 1e-3f);
    float3 gridPos = worldPos / safeCellSize;
    int3 baseCell = int3(floor(gridPos));

    SH4RGB weightedSH = InitSH4RGB();
    float weightedFrames = 0.0f;
    float weightSum = 0.0f;

    [unroll]
    for (int z = -1; z <= 1; ++z)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                int3 cell = baseCell + int3(x, y, z);
                float3 cellCenter = float3(cell) + 0.5f.xxx;
                float3 delta = gridPos - cellCenter;
                float distSq = dot(delta, delta);
                float weight = exp(-distSq * 1.35f);
                if (weight <= 1e-4f)
                    continue;

                SH4RGB cachedSH = InitSH4RGB();
                float cachedFrames = 0.0f;
                float bw = 0.0f;
                if (LoadCachedSHForCell(cell, normal, worldPos, cachedSH, cachedFrames, bw))
                {
                    float confidence = saturate(cachedFrames / 8.0f);
                    float finalWeight = weight * lerp(0.35f, 1.0f, confidence) * bw;
                    weightedSH = AddSH(weightedSH, ScaleSH(cachedSH, finalWeight));
                    weightedFrames += cachedFrames * finalWeight;
                    weightSum += finalWeight;
                }
            }
        }
    }

    if (weightSum <= 1e-5f)
    {
        outSH = baseSH;
        outHistoryFrames = baseFrames;
        return hasBase;
    }

    SH4RGB neighborhoodSH = ScaleSH(weightedSH, rcp(weightSum));
    float neighborhoodFrames = weightedFrames * rcp(weightSum);
    if (hasBase)
    {
        outSH = LerpSH(baseSH, neighborhoodSH, smooth);
        outHistoryFrames = lerp(baseFrames, neighborhoodFrames, smooth);
    }
    else
    {
        outSH = neighborhoodSH;
        outHistoryFrames = neighborhoodFrames;
    }
    return true;
}

[numthreads(256, 1, 1)]
void SpatialHashClear(uint3 DTid : SV_DispatchThreadID)
{
    uint entryIndex = DTid.x;
    if (entryIndex == 0u)
        ActiveCounterOut[0] = 0u;

    if (entryIndex >= HashEntryCount)
        return;

    if (HistoryValid == 0u)
    {
        ActiveFlagsOut[entryIndex] = 0u;
        CellScoreOut[entryIndex] = 0xffffffffu;
        CellPositionOut[entryIndex] = 0.0f.xxxx;
        CellNormalOut[entryIndex] = 0.0f.xxxx;
        CellLightMaskOut[entryIndex] = 0u;
        ResolvedKeysOut[entryIndex] = 0u;
        ResolvedSH0Out[entryIndex] = 0.0f.xxxx;
        ResolvedSH1Out[entryIndex] = 0.0f.xxxx;
        ResolvedSH2Out[entryIndex] = 0.0f.xxxx;
        ResolvedSH3Out[entryIndex] = 0.0f.xxxx;
        return;
    }

    // Steady state: age out long-unseen cells so the hash can't saturate (the
    // cause of GI progressively vanishing after exploring a while). Off-screen
    // cells survive up to SPATIAL_HASH_MAX_CELL_AGE_FRAMES. To keep linear-probe
    // chains intact (reads stop at the first empty slot), only evict a slot that
    // is the TAIL of its cluster — i.e. the next slot is already empty — so
    // freeing it never orphans a key that probed past it. Each thread owns its
    // own entry, so this is race-free.
    uint key = ResolvedKeysOut[entryIndex];
    if (key == 0u)
        return;
    uint lastSeen = ActiveFlagsOut[entryIndex];
    if (lastSeen == 0u || lastSeen == SPATIAL_HASH_ACTIVE_INIT)
        return;
    uint curStamp = (FrameIndex & 0x7fffffffu) + 1u;
    uint age = curStamp - lastSeen;
    if (age <= SPATIAL_HASH_MAX_CELL_AGE_FRAMES)
        return;
    if (ResolvedKeysOut[(entryIndex + 1u) & HashEntryMask] != 0u)
        return; // not the tail of a probe cluster — evicting would break reads

    ActiveFlagsOut[entryIndex] = 0u;
    CellScoreOut[entryIndex] = 0xffffffffu;
    CellPositionOut[entryIndex] = 0.0f.xxxx;
    CellNormalOut[entryIndex] = 0.0f.xxxx;
    CellLightMaskOut[entryIndex] = 0u;
    ResolvedKeysOut[entryIndex] = 0u;
    ResolvedSH0Out[entryIndex] = 0.0f.xxxx;
    ResolvedSH1Out[entryIndex] = 0.0f.xxxx;
    ResolvedSH2Out[entryIndex] = 0.0f.xxxx;
    ResolvedSH3Out[entryIndex] = 0.0f.xxxx;
}

[numthreads(8, 8, 1)]
void SpatialHashUpdate(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixelPos = DTid.xy;
    uint2 textureSize = uint2(RTSize);
    if (pixelPos.x >= textureSize.x || pixelPos.y >= textureSize.y)
        return;

    float deviceDepth = DepthTex[pixelPos].x;
    if (deviceDepth >= 0.99999f)
        return;

    float3 worldPos = ReconstructWorldPosition(pixelPos);
    float3 normal = OrientNormalTowardView(LoadCacheNormal(pixelPos), worldPos);
    uint key = HashCellKey(worldPos, normal);

    uint slot = 0u;
    bool inserted = false;
    if (!FindSlotForWrite(key, slot, inserted))
        return;
    MarkActiveSlot(slot);

    // Pick the cell's representative surface point as the one CLOSEST TO THE
    // CAMERA (smallest view distance wins the InterlockedMin). That surface
    // faces the camera/open side, so the trace's open-space origin offset (along
    // the stored normal) lands in free space rather than buried in geometry, and
    // the probe captures distant incoming radiance without excessive occlusion.
    float3 cameraPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
    float camDist = length(worldPos - cameraPos);
    // Normalize against the far plane so the quantization spans the view range.
    float normalizedDist = saturate(camDist / max(ProjectionParams.w, 1.0f));
    uint score = min(uint(normalizedDist * 16777215.0f), 16777215u);
    score = (score << 8u) | (HashUInt(pixelPos.x * 1973u + pixelPos.y * 9277u + FrameIndex * 26699u) & 255u);

    uint oldScore = 0xffffffffu;
    InterlockedMin(CellScoreOut[slot], score, oldScore);
    if (score <= oldScore)
    {
        CellPositionOut[slot] = float4(worldPos, 1.0f);
        CellNormalOut[slot] = float4(normal, 1.0f);
        CellLightMaskOut[slot] = ComputeCellLightMask(worldPos, normal);
    }
}

[numthreads(256, 1, 1)]
void SpatialHashResolve(uint3 DTid : SV_DispatchThreadID)
{
    uint traceIndex = DTid.x;
    uint activeCount = min(ActiveCounterIn[0], ActiveCellCapacity);
    uint traceCount = min(activeCount, TraceCellBudget);
    if (traceIndex >= traceCount)
        return;

    uint activeIndex = traceIndex;
    if (activeCount > traceCount)
    {
        uint offset = (FrameIndex * traceCount) % activeCount;
        activeIndex = (traceIndex + offset) % activeCount;
    }

    uint cacheSlot = ActiveCellSlotsIn[activeIndex];
    if (cacheSlot >= HashEntryCount || ResolvedKeysOut[cacheSlot] == 0u)
        return;

    SH4RGB currentSH = LoadTraceSH(traceIndex);
    float currentSamples = LoadTraceSampleCount(traceIndex);
    SH4RGB resolvedSH = currentSH;
    float resolvedFrames = currentSamples;

    SH4RGB previousSH;
    float4 previousSH0 = SanitizeFloat4(ResolvedSH0Out[cacheSlot]);
    previousSH.c0 = previousSH0.xyz;
    previousSH.c1 = SanitizeFloat3(ResolvedSH1Out[cacheSlot].xyz);
    previousSH.c2 = SanitizeFloat3(ResolvedSH2Out[cacheSlot].xyz);
    previousSH.c3 = SanitizeFloat3(ResolvedSH3Out[cacheSlot].xyz);

    float adaptiveDecay = ComputeAdaptiveHistorySampleDecay(previousSH, currentSH, HistorySampleDecay);
    float previousFrames = clamp(previousSH0.w * adaptiveDecay, 0.0f, MAX_SPATIAL_HASH_HISTORY_SAMPLES);
    if (previousFrames > 0.0f)
    {
        float acceptedFrames = min(previousFrames + currentSamples, MAX_SPATIAL_HASH_HISTORY_SAMPLES);
        float alpha = saturate(currentSamples / max(acceptedFrames, 1.0f));
        // Phase R-disocclusion-B: accelerated convergence for cells
        // with very low history. For the first ~4 frames after a
        // cell is allocated, weight the new sample much more
        // aggressively so newly-exposed surfaces light up in a
        // handful of frames rather than the legacy ~12-frame ramp.
        // Past `kNewCellRampFrames` the standard sample-count-based
        // alpha takes over and the temporal smoothing pattern is
        // unchanged.
        const float kNewCellRampFrames = 4.0f;
        if (previousFrames < kNewCellRampFrames)
        {
            float boost = 1.0f - saturate(previousFrames / kNewCellRampFrames);
            alpha = saturate(alpha + boost * (0.6f - alpha));
        }
        resolvedSH = LerpSH(previousSH, currentSH, alpha);
        resolvedFrames = acceptedFrames;
    }

    StoreResolvedSH(cacheSlot, resolvedSH, resolvedFrames);
}

[numthreads(8, 8, 1)]
void SpatialHashQuery(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixelPos = DTid.xy;
    uint2 textureSize = uint2(RTSize);
    if (pixelPos.x >= textureSize.x || pixelPos.y >= textureSize.y)
        return;

    float deviceDepth = DepthTex[pixelPos].x;
    if (deviceDepth >= 0.99999f)
    {
        OutGIHashColor[pixelPos] = 0.0f.xxxx;
        OutGIHashSH[pixelPos] = 0.0f.xxxx;
        return;
    }

    if (DebugDiffuseGIOverride.w > 0.5f)
    {
        OutGIHashColor[pixelPos] = float4(max(DebugDiffuseGIOverride.rgb, 0.0f.xxx), 1.0f);
        OutGIHashSH[pixelPos] = float4(max(DebugDiffuseGIOverride.rgb, 0.0f.xxx), 1.0f);
        return;
    }

    float3 worldPos = ReconstructWorldPosition(pixelPos);
    float3 pixelNormal = OrientNormalTowardView(LoadPixelNormal(pixelPos), worldPos);
    float3 cacheNormal = OrientNormalTowardView(LoadCacheNormal(pixelPos), worldPos);
    SH4RGB cachedSH = InitSH4RGB();
    float historyFrames = 0.0f;
    const bool bHasCache = LoadSmoothedSH(worldPos, cacheNormal, cachedSH, historyFrames);
    if (bHasCache)
    {
        float3 radiance = EvaluateSHDiffuse(cachedSH, pixelNormal) * SPATIAL_HASH_DIFFUSE_SCALE;
        OutGIHashColor[pixelPos] = float4(radiance, historyFrames);
        OutGIHashSH[pixelPos] = float4(cachedSH.c0, historyFrames);
    }
    else
    {
        OutGIHashColor[pixelPos] = float4(0.0f.xxx, 0.0f);
        OutGIHashSH[pixelPos] = float4(0.0f.xxx, 0.0f);
    }
}

// =====================================================================
// SpatialHashScreenResolve — per-pixel screen-space resolve layer.
//
// The SpatialHashQuery pass above writes raw cell-evaluated radiance
// per pixel (DiffuseGIHashCached). World-space cell caches leak across
// surfaces on camera motion because their gather neighbourhood spans
// adjacent cells regardless of underlying geometry, and they have no
// per-pixel disocclusion concept.
//
// This pass mirrors ScreenProbeGI's per-pixel resolve: a bilateral
// gather over the per-pixel cache output (depth + normal weighted),
// followed by motion-reprojected temporal accumulation with a
// depth+normal disocclusion gate. Output goes to a separate buffer
// that LightingPS reads. Prev frame's filtered output is snapshotted
// at end-of-pass for next-frame temporal reuse.
// =====================================================================
RWTexture2D<float4> OutDiffuseGIFiltered : register(u13);
Texture2D InDiffuseGIFilteredPrev        : register(t26);

[numthreads(8, 8, 1)]
void SpatialHashScreenResolve(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixelPos = DTid.xy;
    uint2 textureSize = uint2(RTSize);
    if (pixelPos.x >= textureSize.x || pixelPos.y >= textureSize.y)
        return;

    float deviceDepth = DepthTex[pixelPos].x;
    if (deviceDepth >= 0.99999f)
    {
        OutDiffuseGIFiltered[pixelPos] = 0.0f.xxxx;
        return;
    }

    if (DebugDiffuseGIOverride.w > 0.5f)
    {
        OutDiffuseGIFiltered[pixelPos] = float4(max(DebugDiffuseGIOverride.rgb, 0.0f.xxx), 1.0f);
        return;
    }

    // Use GEOmetric normal (bump-free) for the bilateral edge stop.
    // The per-pixel WorldNormal includes normal-map perturbations
    // that can deflect 15-25° between adjacent pixels on the same
    // physical surface, which would falsely trip the hard 0.95
    // dot cutoff and leave only the centre pixel — that was the
    // source of the residual 1-pixel-thick edge ghosts.
    float3 pixelNormal = LoadCacheNormal(pixelPos);
    float curLinear = max(GetLinearDepthOpenGL(deviceDepth, ProjectionParams.z, ProjectionParams.w), 1e-3f);

    // --- Bilateral spatial gather on the per-pixel cell output ---
    // 9×9 footprint with hard depth+normal edge stops on the
    // geometric normal. Larger radius averages more same-surface
    // samples so per-frame cell-update variance gets smoothed
    // within a single frame.
    const int kRadius = 4;
    float3 sumRgb = 0.0f.xxx;
    float sumFrames = 0.0f;
    float sumWeight = 0.0f;
    [loop]
    for (int dy = -kRadius; dy <= kRadius; ++dy)
    {
        [loop]
        for (int dx = -kRadius; dx <= kRadius; ++dx)
        {
            int2 nPx = int2(pixelPos) + int2(dx, dy);
            if (nPx.x < 0 || nPx.y < 0 || nPx.x >= int(textureSize.x) || nPx.y >= int(textureSize.y))
                continue;

            float nDepth = DepthTex[nPx].x;
            if (nDepth >= 0.99999f)
                continue;
            float3 nNormal = LoadCacheNormal(nPx);
            float nLinear = max(GetLinearDepthOpenGL(nDepth, ProjectionParams.z, ProjectionParams.w), 1e-3f);

            float depthDelta = abs(curLinear - nLinear) / max(curLinear, 1e-3f);
            float normalDot = saturate(dot(pixelNormal, nNormal));
            // Hard edge stop: ANY neighbour with > 3% depth delta or
            // < 0.95 normal dot (~18°) is considered a different
            // surface and dropped from the gather entirely. Earlier
            // soft falloff still let very-bright sun-lit column
            // pixels leak measurably into adjacent wall queries
            // because the dynamic range is huge (1000:1) — a small
            // soft weight × huge intensity is still visible.
            if (depthDelta > 0.03f || normalDot < 0.95f)
                continue;
            float spatial = exp(-(dx * dx + dy * dy) * 0.18f);
            float weight = spatial;
            if (weight <= 1e-4f)
                continue;
            float4 sample = OutGIHashColor[nPx];
            sumRgb    += sample.xyz * weight;
            sumFrames += sample.w  * weight;
            sumWeight += weight;
        }
    }
    float3 filteredRgb = sumWeight > 1e-5f ? (sumRgb / sumWeight) : 0.0f.xxx;
    float filteredFrames = sumWeight > 1e-5f ? (sumFrames / sumWeight) : 0.0f;

    // --- Per-pixel temporal accumulation with disocclusion gate ---
    float2 uvNow = (float2(pixelPos) + 0.5f) / float2(textureSize);
    float2 velocity = VelocityTex[pixelPos].xy;
    float2 prevUV = uvNow - velocity;
    bool bHistoryValid = false;
    float3 prevRgb = 0.0f.xxx;
    float prevFrames = 0.0f;
    if (prevUV.x >= 0.0f && prevUV.x <= 1.0f && prevUV.y >= 0.0f && prevUV.y <= 1.0f)
    {
        int2 prevPx = clamp(int2(prevUV * float2(textureSize)), int2(0,0), int2(textureSize) - 1);
        float prevDeviceDepth = PrevDepthTex[prevPx].x;
        if (prevDeviceDepth < 0.99999f)
        {
            // PrevNormalTex is the WORLD normal buffer — for the
            // disocclusion compare we want the geometric component
            // only, but we don't have a prev GeoNormal channel.
            // Treat the world normal as an approximation; the
            // bilateral gather above uses geo normal directly.
            float3 prevNormal = SafeNormalize(PrevNormalTex[prevPx].xyz, pixelNormal);
            float prevLinear = max(GetLinearDepthOpenGL(prevDeviceDepth, ProjectionParams.z, ProjectionParams.w), 1e-3f);
            float depthDelta = abs(curLinear - prevLinear) / max(curLinear, 1e-3f);
            float normalDot = saturate(dot(pixelNormal, prevNormal));
            // Very strict disocclusion gate — sub-pixel motion-vector
            // truncation can land prevPx on a neighbour pixel from a
            // DIFFERENT surface at silhouette edges. Tighten depth to
            // 2 % and normal dot to 0.97 (~14°) so column / wall
            // edge transitions are reliably rejected even when the
            // prev pixel is only one texel away from the correct
            // position. Loose threshold here was the residual
            // 1-pixel-thick edge ghost source.
            if (depthDelta < 0.02f && normalDot > 0.97f)
            {
                float4 prev = InDiffuseGIFilteredPrev[prevPx];
                prevRgb = max(SanitizeFloat3(prev.xyz), 0.0f.xxx);
                // Smaller frame cap = faster forgetfulness. With 32
                // a wall pixel whose prev value got column-tinted
                // by last frame's bilateral spread keeps that
                // contamination for ~33 frames; at 8 it washes out
                // in a few frames so the user-perceived trail-
                // edge ghost decays quickly under panning.
                prevFrames = clamp(prev.w, 0.0f, 4.0f);
                bHistoryValid = true;
            }
        }
    }

    float3 outRgb;
    float outFrames;
    if (bHistoryValid)
    {
        float acceptedFrames = min(prevFrames + 1.0f, 4.0f);
        float alpha = 1.0f / max(acceptedFrames, 1.0f);
        outRgb = lerp(prevRgb, filteredRgb, alpha);
        outFrames = acceptedFrames;
    }
    else
    {
        outRgb = filteredRgb;
        outFrames = 1.0f;
    }

    OutDiffuseGIFiltered[pixelPos] = float4(outRgb, outFrames);
}
