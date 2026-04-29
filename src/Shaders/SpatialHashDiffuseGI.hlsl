#include "Common.hlsl"

Texture2D DepthTex : register(t0);
Texture2D WorldNormalTex : register(t1);
Texture2D GeoNormalTex : register(t2);

StructuredBuffer<uint> UpdateKeysIn : register(t5);
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

RWStructuredBuffer<uint> UpdateKeysOut : register(u0);
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
};

struct SH4RGB
{
    float3 c0;
    float3 c1;
    float3 c2;
    float3 c3;
};

static const float MAX_SPATIAL_HASH_HISTORY_SAMPLES = 4096.0f;

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

float3 LoadSurfaceNormal(uint2 pixelPos)
{
    float3 worldNormal = SafeNormalize(WorldNormalTex[pixelPos].xyz, float3(0.0f, 1.0f, 0.0f));
    return SafeNormalize(GeoNormalTex[pixelPos].xyz, worldNormal);
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

uint HashCellKeyFromCell(int3 cell, float3 normal)
{
    uint3 normalBin = uint3(saturate(normal * 0.5f + 0.5f) * 7.0f + 0.5f);
    uint normalBits = (normalBin.x & 7u) | ((normalBin.y & 7u) << 3u) | ((normalBin.z & 7u) << 6u);

    uint h = uint(cell.x) * 73856093u;
    h ^= uint(cell.y) * 19349663u;
    h ^= uint(cell.z) * 83492791u;
    h ^= normalBits * 2654435761u;
    h = HashUInt(h);
    return h == 0u ? 1u : h;
}

uint HashCellKey(float3 worldPos, float3 normal)
{
    return HashCellKeyFromCell(GetSpatialHashCell(worldPos), normal);
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
        InterlockedCompareExchange(UpdateKeysOut[candidate], 0u, key, oldValue);
        if (oldValue == 0u || oldValue == key)
        {
            slot = candidate;
            inserted = oldValue == 0u;
            return true;
        }
    }

    slot = 0u;
    inserted = false;
    return false;
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

bool LoadCachedSHForCell(int3 cell, float3 normal, out SH4RGB sh, out float historyFrames)
{
    uint slot = 0u;
    uint key = HashCellKeyFromCell(cell, normal);
    if (FindSlotForRead(key, slot))
    {
        sh = LoadResolvedSH(slot, historyFrames);
        return historyFrames > 0.0f && SHAbsEnergy(sh) > 1e-7f;
    }

    sh = InitSH4RGB();
    historyFrames = 0.0f;
    return false;
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
                if (LoadCachedSHForCell(baseCell + int3(x, y, z), normal, cachedSH, cachedFrames))
                {
                    weightedSH = AddSH(weightedSH, ScaleSH(cachedSH, weight));
                    weightedFrames += cachedFrames * weight;
                    validWeight += weight;
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
    if (LoadCachedSHForCell(baseCell, normal, baseSH, baseFrames))
    {
        float interpolation = saturate(InterpolationStrength);
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
                if (LoadCachedSHForCell(cell, normal, cachedSH, cachedFrames))
                {
                    float confidence = saturate(cachedFrames / 8.0f);
                    float finalWeight = weight * lerp(0.35f, 1.0f, confidence);
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
    if (entryIndex >= HashEntryCount)
        return;

    UpdateKeysOut[entryIndex] = 0u;
    CellPositionOut[entryIndex] = 0.0f.xxxx;
    CellNormalOut[entryIndex] = 0.0f.xxxx;
    CellScoreOut[entryIndex] = 0xffffffffu;
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

    float3 normal = LoadSurfaceNormal(pixelPos);
    float3 worldPos = ReconstructWorldPosition(pixelPos);
    uint key = HashCellKey(worldPos, normal);

    uint slot = 0u;
    bool inserted = false;
    if (!FindSlotForWrite(key, slot, inserted))
        return;

    float3 cellCenter = (float3(GetSpatialHashCell(worldPos)) + 0.5f.xxx) * max(CellSize, 1e-3f);
    float normalizedDist = saturate(length((worldPos - cellCenter) / max(CellSize, 1e-3f)) * 1.1547005f);
    uint score = min(uint(normalizedDist * 16777215.0f), 16777215u);
    score = (score << 8u) | (HashUInt(pixelPos.x * 1973u + pixelPos.y * 9277u + FrameIndex * 26699u) & 255u);

    uint oldScore = 0xffffffffu;
    InterlockedMin(CellScoreOut[slot], score, oldScore);
    if (score <= oldScore)
    {
        CellPositionOut[slot] = float4(worldPos, 1.0f);
        CellNormalOut[slot] = float4(normal, 1.0f);
    }
}

[numthreads(256, 1, 1)]
void SpatialHashResolve(uint3 DTid : SV_DispatchThreadID)
{
    uint entryIndex = DTid.x;
    if (entryIndex >= HashEntryCount)
        return;

    uint updateKey = UpdateKeysIn[entryIndex];
    if (updateKey != 0u)
    {
        SH4RGB currentSH = LoadTraceSH(entryIndex);
        float currentSamples = LoadTraceSampleCount(entryIndex);
        SH4RGB resolvedSH = currentSH;
        float resolvedFrames = currentSamples;

        uint previousSlot = 0u;
        if (HistoryValid != 0u && FindPrevSlotForRead(updateKey, previousSlot))
        {
            SH4RGB previousSH = LoadPrevResolvedSH(previousSlot);
            float adaptiveDecay = ComputeAdaptiveHistorySampleDecay(previousSH, currentSH, HistorySampleDecay);
            float previousFrames = clamp(SanitizeFloat4(PrevResolvedSH0[previousSlot]).w * adaptiveDecay, 0.0f, MAX_SPATIAL_HASH_HISTORY_SAMPLES);
            if (previousFrames > 0.0f)
            {
                float acceptedFrames = min(previousFrames + currentSamples, MAX_SPATIAL_HASH_HISTORY_SAMPLES);
                float alpha = saturate(currentSamples / max(acceptedFrames, 1.0f));
                resolvedSH = LerpSH(previousSH, currentSH, alpha);
                resolvedFrames = acceptedFrames;
            }
        }

        ResolvedKeysOut[entryIndex] = updateKey;
        StoreResolvedSH(entryIndex, resolvedSH, resolvedFrames);
        return;
    }

    if (HistoryValid != 0u)
    {
        uint prevKey = PrevResolvedKeys[entryIndex];
        float previousFrames = SanitizeFloat4(PrevResolvedSH0[entryIndex]).w;
        if (prevKey != 0u && previousFrames > 1.0f)
        {
            ResolvedKeysOut[entryIndex] = prevKey;
            StoreResolvedSH(entryIndex, LoadPrevResolvedSH(entryIndex), max(previousFrames - 1.0f, 0.0f));
        }
    }
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

    float3 normal = LoadSurfaceNormal(pixelPos);
    float3 worldPos = ReconstructWorldPosition(pixelPos);
    SH4RGB cachedSH = InitSH4RGB();
    float historyFrames = 0.0f;
    if (LoadSmoothedSH(worldPos, normal, cachedSH, historyFrames))
    {
        float3 radiance = EvaluateSHDiffuse(cachedSH, normal);
        OutGIHashColor[pixelPos] = float4(radiance, historyFrames);
        OutGIHashSH[pixelPos] = float4(cachedSH.c0, historyFrames);
    }
    else
    {
        OutGIHashColor[pixelPos] = 0.0f.xxxx;
        OutGIHashSH[pixelPos] = 0.0f.xxxx;
    }
}
