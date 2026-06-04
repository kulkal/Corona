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

// Octahedral DDGI (GIMode==1). RayData (per-frame per-ray radiance+dist) is
// consumed by the blend kernel; OctIrradianceOut is the persistent resolved
// atlas (in-place temporal blend); OctIrradianceIn is the same atlas as SRV for
// the query. Indexed by (hashSlot & (OctCellCapacity-1)) * texels + texelIndex.
StructuredBuffer<float4> OctRayDataIn : register(t27);
StructuredBuffer<float4> OctIrradianceIn : register(t28);
StructuredBuffer<float2> OctDepthIn : register(t29);
StructuredBuffer<uint> OctCellKeyIn : register(t30);
// Camera-anchored ambient SH4 (c0..c3), traced from the camera in the RT pass.
// Used as the fallback for uncached cells (tracks local indoor/outdoor ambient).
StructuredBuffer<float4> CameraProbeSHIn : register(t31);
RWStructuredBuffer<float4> OctIrradianceOut : register(u15);
RWStructuredBuffer<float2> OctDepthOut : register(u16);
RWStructuredBuffer<float4> OctReservoirRayOut : register(u18);
RWStructuredBuffer<float4> OctReservoirRadianceOut : register(u19);
// Per-oct-slot ownership tag, packed (ownerHash16 << 16 | frameStamp16). Because
// octIndex = hashSlot & (OctCellCapacity-1) aliases unrelated cells in large
// scenes, only ONE cell may own an oct slot at a time; colliding cells skip it
// and fall back to neighbour probes (no corrupt bright/dark dots). Ownership is
// self-cleaning: the owner re-stamps every frame; if its stamp goes stale (it
// stopped being visible / was evicted) a colliding visible cell takes over.
RWStructuredBuffer<uint> OctCellKeyOut : register(u17);
#define OCT_OWNER_STALE_FRAMES 60u
// Query rejects a probe whose owner stamp wasn't refreshed within this many
// frames (its oct slot isn't being blended -> frozen/stale -> fall back to
// neighbours). Small so stuck cells clear quickly.
#define OCT_QUERY_STALE_FRAMES 4u

// 16-bit non-zero ownership hash of a cell key (0 is reserved for "free").
uint OctOwnerHash(uint key)
{
    uint h = HashUInt(key) & 0xffffu;
    return h == 0u ? 1u : h;
}

// Must match Corona::SpatialHashGIOct* constants.
#define OCT_IRRADIANCE_RES 8u
#define OCT_RAYS_PER_CELL 64u
#define OCT_IRRADIANCE_TEXELS (OCT_IRRADIANCE_RES * OCT_IRRADIANCE_RES)
#define OCT_DEPTH_RES 8u
#define OCT_DEPTH_TEXELS (OCT_DEPTH_RES * OCT_DEPTH_RES)
static const float OCT_MAX_HISTORY_FRAMES = 256.0f;
static const float OCT_NEW_CELL_RAMP_FRAMES = 6.0f;
// Cosine exponent sharpening the per-texel depth weighting (DDGI uses a high
// power so the visibility map is sharper than the irradiance map).
static const float OCT_DEPTH_SHARPNESS = 12.0f;
// Hit distances are clamped to this many cells so a single missed ray
// (distance ~= MAX_HIT_DIST) can't blow up a wall texel's mean distance.
static const float OCT_DEPTH_MAX_CELLS = 8.0f;
// Per-ray luminance soft-knee for firefly suppression in the octahedral blend.
// Values below the knee pass through; brighter rays are compressed toward it.
static const float OCT_FIREFLY_LUMA_KNEE = 4.0f;
// Low-light oct texels can have high relative variance: most rays see little
// energy, then a rare path finds a lit opening. Preserve more history only for
// those noisy dark lobes so nearby stable probes still adapt quickly.
static const float OCT_LOW_LIGHT_NOISE_LUMA = 0.35f;
static const float OCT_LOW_LIGHT_VARIANCE_START = 0.35f;
static const float OCT_LOW_LIGHT_VARIANCE_END = 2.50f;
static const float OCT_LOW_LIGHT_ALPHA_SCALE = 0.35f;
// ReSTIR-lite reused path sample per oct probe. This keeps rare useful diffuse
// paths alive for several frames and injects them as a small virtual ray in the
// octahedral convolution.
static const float OCT_RESERVOIR_MAX_AGE = 24.0f;
static const float OCT_RESERVOIR_VIRTUAL_WEIGHT = 3.0f;
static const float OCT_RESERVOIR_MIN_TARGET = 1e-4f;

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
    uint GIMode;           // 0 = SH4, 1 = octahedral DDGI
    uint OctCellCapacity;  // octahedral atlas slot count (power of two)
    PointLightParam PointLights[RT_DIFFUSE_GI_MAX_POINT_LIGHTS];
    uint PointLightCount;
    // Repurposed former float3 padding (layout unchanged):
    float OctNearConvergenceBias; // 0 = uniform, 1 = strong near-camera priority
    float EvictDistanceWeight;    // LRU victim: 0 = age only, 1 = camera distance only
    float _spatialHashPad;
    float4 DebugDiffuseGIOverride;
    float4 SpatialHashLevelParams; // x=enable, y=base distance (SHaRC cell levels)
    float4 SpatialHashSkyAmbient;  // rgb = sky-ambient fill for uncached cells (E/pi)
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
#ifndef SPATIAL_HASH_VIEW_FALLBACK_STEPS
#define SPATIAL_HASH_VIEW_FALLBACK_STEPS 6u
#endif
static const float SPATIAL_HASH_VIEW_FALLBACK_START_CELL_SCALE = 0.20f;
static const float SPATIAL_HASH_VIEW_FALLBACK_STEP_CELL_SCALE = 0.35f;
static const float SPATIAL_HASH_VIEW_FALLBACK_MAX_CELL_SCALE = 2.25f;

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

// Fallback ambient for uncached cells. The previous camera-anchored SH fallback
// is disabled here so missing oct cells are exposed to the view-ray lookup first
// and only fall back to the fixed sky ambient.
float3 EvaluateUncachedAmbient(float3 normal)
{
    return max(SpatialHashSkyAmbient.rgb, 0.0f.xxx);
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

// SHaRC distance-based cell sizing (must match SpatialHashCellGI.hlsl's copy so
// insert/query/light-mask keys agree). Disabled -> base CellSize, offset 0, so
// behaviour is identical to the fixed-grid path when the toggle is off.
float SpatialHashLeveledCellSize(float3 worldPos, out uint level)
{
    float cs = max(CellSize, 1e-3f);
    level = 0u;
    if (SpatialHashLevelParams.x < 0.5f)
        return cs;
    float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
    float dist = length(worldPos - camPos);
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
    // Half-cell plane bins let one xyz cell keep separate representatives
    // for parallel or near-parallel surfaces, while the lookup still checks
    // +/- one bin so cache hits do not disappear at quantization boundaries.
    return int(floor((dot(worldPos, normal) / cs) * 2.0f + 0.5f));
}

uint HashCellKeyFromBits(int3 cell, uint normalBits, int planeBin)
{
    uint h = uint(cell.x) * 73856093u;
    h ^= uint(cell.y) * 19349663u;
    h ^= uint(cell.z) * 83492791u;
    h ^= normalBits * 2654435761u;
    h ^= uint(planeBin) * 1597334677u;
    h = HashUInt(h);
    return h == 0u ? 1u : h;
}

// Cell key. GIMode==1 (octahedral DDGI): one probe per cell -> key depends on
// the cell coordinate ONLY. Else (SH4 normal-bin mode): split the cell by 3-axis
// normal bin + plane bin. MUST stay identical to the cell (trace) shader's copy
// so insert and lookup agree.
uint HashCellKeyFromCell(int3 cell, float3 normal, int planeBin)
{
    if (GIMode == 1u)
    {
        uint h = uint(cell.x) * 73856093u;
        h ^= uint(cell.y) * 19349663u;
        h ^= uint(cell.z) * 83492791u;
        h = HashUInt(h);
        return h == 0u ? 1u : h;
    }
    return HashCellKeyFromBits(cell, EncodeNormalBits(normal), planeBin);
}

// Normal-bin blending (curved/slanted surfaces, SH4 mode). EncodeNormalBits
// quantizes the normal to 8 levels/axis, so a smoothly-curving surface crosses
// bin boundaries and adjacent pixels hash to different cells -> visible
// comb/banding. When the query normal sits near a bin boundary, return the
// adjacent bin's encoded bits and a blend weight so the query mixes both cells'
// SH and dissolves the seam. Only the axis nearest a boundary is considered.
#ifndef SPATIAL_HASH_NORMAL_BIN_BLEND
#define SPATIAL_HASH_NORMAL_BIN_BLEND 1
#endif
#ifndef SPATIAL_HASH_NORMAL_BLEND_MIN_FRAC
#define SPATIAL_HASH_NORMAL_BLEND_MIN_FRAC 0.28f
#endif

bool ComputeNormalBinNeighbor(float3 normal, out uint neighborBits, out float blendWeight)
{
    neighborBits = 0u;
    blendWeight = 0.0f;
    normal = SafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    float3 e = saturate(normal * 0.5f + 0.5f) * 7.0f; // encoded float, matches EncodeNormalBits
    int3 bin = int3(e + 0.5f);
    float3 fr = e - float3(bin);                       // (-0.5, 0.5)
    float3 af = abs(fr);
    int axis = (af.x >= af.y && af.x >= af.z) ? 0 : (af.y >= af.z ? 1 : 2);
    float fa = (axis == 0) ? fr.x : ((axis == 1) ? fr.y : fr.z);
    float aaf = abs(fa);
    if (aaf < SPATIAL_HASH_NORMAL_BLEND_MIN_FRAC)
        return false; // well inside the bin — no seam, skip the extra lookup
    int cur = (axis == 0) ? bin.x : ((axis == 1) ? bin.y : bin.z);
    int nb = clamp(cur + (fa > 0.0f ? 1 : -1), 0, 7);
    if (nb == cur)
        return false; // at the hemisphere edge, no neighbour bin
    int3 nbin = bin;
    if (axis == 0) nbin.x = nb; else if (axis == 1) nbin.y = nb; else nbin.z = nb;
    neighborBits = (uint(nbin.x) & 7u) | ((uint(nbin.y) & 7u) << 3u) | ((uint(nbin.z) & 7u) << 6u);
    blendWeight = saturate((aaf - SPATIAL_HASH_NORMAL_BLEND_MIN_FRAC) /
                           max(0.5f - SPATIAL_HASH_NORMAL_BLEND_MIN_FRAC, 1e-3f)) * 0.5f;
    return true;
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
    // Victim = highest eviction score in the window. Score combines camera
    // distance and staleness: evictScore = distNorm*EvictDistanceWeight +
    // ageNorm*(1-EvictDistanceWeight). With EvictDistanceWeight high, a FAR cell
    // (even recently seen) is evicted before a NEAR cell (even long-unseen) —
    // near cells are the most valuable to keep cached.
    float3 evCamPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
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
        // Skip slots being set up (mid-init / not yet stamped) and slots already
        // marked active this frame — evicting those would thrash live cells.
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
    float4 storedNormal4 = SanitizeFloat4(CellNormalIn[slot]);
    if (storedPosition.w <= 0.0f || storedNormal4.w <= 0.0f)
        return 0.0f;

    queryNormal = SafeNormalize(queryNormal, float3(0.0f, 1.0f, 0.0f));
    float3 storedNormal = SafeNormalize(storedNormal4.xyz, queryNormal);
    float normalDot = saturate(dot(queryNormal, storedNormal));
    if (normalDot < SPATIAL_HASH_MIN_SURFACE_NORMAL_DOT)
        return 0.0f;

    uint hashLevel;
    float safeCellSize = SpatialHashLeveledCellSize(queryWorldPos, hashLevel);
    float planeDelta = abs(dot(queryWorldPos - storedPosition.xyz, storedNormal));
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

bool LoadCachedSHForCell(int3 cell, float3 normal, float3 queryWorldPos,
                          out SH4RGB sh, out float historyFrames, out float bilateralWeight)
{
    bilateralWeight = 0.0f;
    sh = InitSH4RGB();
    historyFrames = 0.0f;

    int basePlaneBin = ComputePlaneBin(queryWorldPos, normal);
    float bestScore = 0.0f;
    SH4RGB bestSH = InitSH4RGB();
    float bestHistoryFrames = 0.0f;
    float bestWeight = 0.0f;

    [unroll]
    for (int planeOffset = -1; planeOffset <= 1; ++planeOffset)
    {
        uint slot = 0u;
        uint key = HashCellKeyFromCell(cell, normal, basePlaneBin + planeOffset);
        if (!FindSlotForRead(key, slot))
            continue;

        float surfaceWeight = ComputeSurfaceLobeWeight(slot, queryWorldPos, normal);
        if (surfaceWeight <= 1e-4f)
            continue;

        float candidateHistoryFrames = 0.0f;
        SH4RGB candidateSH = LoadResolvedSH(slot, candidateHistoryFrames);
        if (candidateHistoryFrames <= 0.0f || SHAbsEnergy(candidateSH) <= 1e-7f)
            continue;

        float candidateScore = surfaceWeight * lerp(0.25f, 1.0f, saturate(candidateHistoryFrames / 8.0f));
        if (candidateScore > bestScore)
        {
            bestScore = candidateScore;
            bestSH = candidateSH;
            bestHistoryFrames = candidateHistoryFrames;
            bestWeight = surfaceWeight;
        }
    }

    if (bestScore <= 1e-5f)
        return false;

#if SPATIAL_HASH_NORMAL_BIN_BLEND
    // Crossfade with the adjacent normal bin when the query normal is near a bin
    // boundary, dissolving the comb seam on curved/slanted surfaces.
    uint neighborBits;
    float nblend;
    if (ComputeNormalBinNeighbor(normal, neighborBits, nblend))
    {
        uint nslot = 0u;
        uint nkey = HashCellKeyFromBits(cell, neighborBits, basePlaneBin);
        if (FindSlotForRead(nkey, nslot))
        {
            float nw = ComputeSurfaceLobeWeight(nslot, queryWorldPos, normal);
            if (nw > 1e-4f)
            {
                float nhist = 0.0f;
                SH4RGB nsh = LoadResolvedSH(nslot, nhist);
                if (nhist > 0.0f && SHAbsEnergy(nsh) > 1e-7f)
                {
                    float t = saturate(nblend);
                    bestSH = LerpSH(bestSH, nsh, t);
                    bestHistoryFrames = lerp(bestHistoryFrames, nhist, t);
                    bestWeight = max(bestWeight, nw);
                }
            }
        }
    }
#endif

    sh = bestSH;
    historyFrames = bestHistoryFrames;
    bilateralWeight = bestWeight;
    return true;
}

bool LoadInterpolatedSH(float3 worldPos, float3 normal, out SH4RGB outSH, out float outHistoryFrames)
{
    uint hashLevel;
    float safeCellSize = SpatialHashLeveledCellSize(worldPos, hashLevel);
    int3 levelOffset = SpatialHashLevelOffset(hashLevel);
    float3 gridPos = worldPos / safeCellSize;
    float3 baseCellFloat = floor(gridPos);
    int3 baseCell = int3(baseCellFloat) + levelOffset;
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

    uint hashLevel;
    float safeCellSize = SpatialHashLeveledCellSize(worldPos, hashLevel);
    int3 levelOffset = SpatialHashLevelOffset(hashLevel);
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
                int3 rawCell = baseCell + int3(x, y, z);
                float3 cellCenter = float3(rawCell) + 0.5f.xxx;
                float3 delta = gridPos - cellCenter;
                float distSq = dot(delta, delta);
                float weight = exp(-distSq * 1.35f);
                if (weight <= 1e-4f)
                    continue;

                int3 cell = rawCell + levelOffset; // key uses level-offset coords
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

    // Steady state: NO unconditional time-based eviction. Cells persist (their
    // cached GI stays valid for re-use) as long as there is room. Reclamation is
    // on-demand only: when a newly-visible cell's probe window is full,
    // FindSlotForWrite evicts the best victim (least useful = far + least-recently
    // seen, weighted by EvictDistanceWeight). This keeps off-screen GI cached
    // until space pressure actually forces a swap, per design.
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

    // Pick the cell's representative surface point. Octahedral (GIMode==1): the
    // surface CLOSEST TO THE CAMERA, so the open-space probe origin lands in free
    // space. SH4 normal-bin: the surface nearest the cell centre (the slot is
    // already orientation-split, so a centred representative is best).
    float normalizedDist;
    if (GIMode == 1u)
    {
        float3 cameraPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
        float camDist = length(worldPos - cameraPos);
        normalizedDist = saturate(camDist / max(ProjectionParams.w, 1.0f));
    }
    else
    {
        // Use the RAW (non-level-offset) leveled grid for the cell centre.
        uint lvl;
        float cs = SpatialHashLeveledCellSize(worldPos, lvl);
        float3 cellCenter = (floor(worldPos / cs) + 0.5f.xxx) * cs;
        normalizedDist = saturate(length((worldPos - cellCenter) / cs) * 1.1547005f);
    }
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

// ---- Octahedral DDGI (GIMode==1) -------------------------------------------
float4 LoadOctTexel(uint octIndex, int2 xy)
{
    xy = clamp(xy, int2(0, 0), int2((int)OCT_IRRADIANCE_RES - 1, (int)OCT_IRRADIANCE_RES - 1));
    uint idx = octIndex * OCT_IRRADIANCE_TEXELS + (uint)xy.y * OCT_IRRADIANCE_RES + (uint)xy.x;
    return SanitizeFloat4(OctIrradianceIn[idx]);
}

// Bilinear sample of the probe's octahedral irradiance map in a direction.
// Stage A has no octahedral border, so we clamp at the edges (minor seam at the
// octahedral fold — addressed in Stage C with a proper 1px border).
float4 SampleOctIrradiance(uint octIndex, float3 dir)
{
    float2 uv = DirectionToOctahedralUV(SafeNormalize(dir, float3(0.0f, 1.0f, 0.0f)));
    float2 t = uv * (float)OCT_IRRADIANCE_RES - 0.5f;
    float2 base = floor(t);
    float2 f = t - base;
    int2 b = int2(base);
    float4 c00 = LoadOctTexel(octIndex, b + int2(0, 0));
    float4 c10 = LoadOctTexel(octIndex, b + int2(1, 0));
    float4 c01 = LoadOctTexel(octIndex, b + int2(0, 1));
    float4 c11 = LoadOctTexel(octIndex, b + int2(1, 1));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

float2 LoadOctDepthTexel(uint octIndex, int2 xy)
{
    xy = clamp(xy, int2(0, 0), int2((int)OCT_DEPTH_RES - 1, (int)OCT_DEPTH_RES - 1));
    uint idx = octIndex * OCT_DEPTH_TEXELS + (uint)xy.y * OCT_DEPTH_RES + (uint)xy.x;
    return OctDepthIn[idx];
}

// Bilinear sample of the probe's octahedral depth map (mean, mean^2) in a dir.
float2 SampleOctDepth(uint octIndex, float3 dir)
{
    float2 uv = DirectionToOctahedralUV(SafeNormalize(dir, float3(0.0f, 1.0f, 0.0f)));
    float2 t = uv * (float)OCT_DEPTH_RES - 0.5f;
    float2 base = floor(t);
    float2 f = t - base;
    int2 b = int2(base);
    float2 c00 = LoadOctDepthTexel(octIndex, b + int2(0, 0));
    float2 c10 = LoadOctDepthTexel(octIndex, b + int2(1, 0));
    float2 c01 = LoadOctDepthTexel(octIndex, b + int2(0, 1));
    float2 c11 = LoadOctDepthTexel(octIndex, b + int2(1, 1));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

// DDGI Chebyshev visibility: probability the query point is visible from the
// probe, from the probe's stored mean/mean^2 hit distance in the probe->query
// direction. Returns 1 when the probe has no depth data yet (mean<=0) so GI is
// not suppressed before the depth map converges. This is what kills the corner
// light-leak: a probe occluded from the query (query beyond the mean occluder
// distance) gets a low weight and is dropped from the blend.
float OctChebyshevWeight(uint octIndex, float3 probePos, float3 queryPos)
{
    float3 probeToQuery = queryPos - probePos;
    float distToQuery = length(probeToQuery);
    if (distToQuery <= 1e-4f)
        return 1.0f;
    float2 moments = SampleOctDepth(octIndex, probeToQuery / distToQuery);
    float meanDist = moments.x;
    if (meanDist <= 0.0f)
        return 1.0f;                 // no depth data yet
    if (distToQuery <= meanDist)
        return 1.0f;                 // in front of nearest occluder -> visible
    float variance = max(moments.y - meanDist * meanDist, 1e-5f);
    float d = distToQuery - meanDist;
    float chebyshev = variance / (variance + d * d);
    chebyshev = chebyshev * chebyshev * chebyshev; // sharpen (DDGI)
    return saturate(chebyshev);
}

// Position/plane bilateral weight for an octahedral probe. The probe is
// omnidirectional (the octahedral map covers all directions), so unlike the SH
// normal-bin path we must NOT reject by stored normal. Instead reject probes
// whose representative surface point sits well off the query surface's tangent
// plane — i.e. the perpendicular wall/floor across a corner. Acts as a coarse
// prior before the depth map converges; Chebyshev does the sharp cut after.
float OctProbeSurfaceWeight(uint slot, float3 queryPos, float3 queryNormal)
{
    float4 storedPosition = SanitizeFloat4(CellPositionIn[slot]);
    if (storedPosition.w <= 0.0f)
        return 0.0f;
    uint hashLevel;
    float safeCellSize = SpatialHashLeveledCellSize(queryPos, hashLevel);
    queryNormal = SafeNormalize(queryNormal, float3(0.0f, 1.0f, 0.0f));
    float planeDelta = abs(dot(queryPos - storedPosition.xyz, queryNormal));
    return saturate(1.0f - smoothstep(safeCellSize * 0.25f, safeCellSize * 0.85f, planeDelta));
}

// 8-cell trilinear interpolation of octahedral irradiance (DDGI-style). The
// index position selects which hash cells to try; the query position remains the
// actual shaded surface used by surface/visibility checks. Keeping these
// separate lets a miss probe a little toward the camera without pretending the
// shaded point itself moved.
float3 SampleOctIrradianceIndexed(float3 indexPos, float3 queryPos, float3 evalNormal, float3 cacheNormal, uint hashLevel)
{
    float safeCellSize = max(CellSize, 1e-3f) * exp2((float)hashLevel);
    int3 levelOffset = SpatialHashLevelOffset(hashLevel);
    float3 gridPos = indexPos / safeCellSize;
    float3 baseF = floor(gridPos);
    int3 baseCell = int3(baseF) + levelOffset;
    float3 frac3 = saturate(gridPos - baseF);

    float3 sumRadiance = 0.0f.xxx;
    float sumWeight = 0.0f;
    // Fallback accumulation ignoring Chebyshev/surface rejection. Used only when
    // EVERY probe got rejected (sumWeight ~= 0), so an over-occluded pixel falls
    // back to plain trilinear instead of going fully black (the scattered dark
    // cells). Normal pixels still use the visibility-weighted result.
    float3 fallbackRadiance = 0.0f.xxx;
    float fallbackWeight = 0.0f;
    [unroll]
    for (uint z = 0u; z < 2u; ++z)
    {
        float wz = (z == 0u) ? (1.0f - frac3.z) : frac3.z;
        [unroll]
        for (uint y = 0u; y < 2u; ++y)
        {
            float wy = (y == 0u) ? (1.0f - frac3.y) : frac3.y;
            [unroll]
            for (uint x = 0u; x < 2u; ++x)
            {
                float wx = (x == 0u) ? (1.0f - frac3.x) : frac3.x;
                float trilinear = wx * wy * wz;
                if (trilinear <= 1e-4f)
                    continue;

                int3 cell = baseCell + int3(x, y, z);
                uint slot = 0u;
                uint key = HashCellKeyFromCell(cell, cacheNormal, 0);
                if (!FindSlotForRead(key, slot))
                    continue;

                uint octIndex = slot & (OctCellCapacity - 1u);
                // Ownership + freshness: use the probe only if its oct slot is
                // owned by THIS cell AND its owner stamp was refreshed within the
                // last few frames. A stale stamp means the slot isn't being blended
                // (insert failed under window pressure / beyond the oct cap / a
                // colliding owner) so its data is FROZEN, possibly from an earlier
                // aliasing collision — skip it and fall back to fresh neighbours
                // instead of showing the stuck corrupted value.
                uint octPacked = OctCellKeyIn[octIndex];
                if (OctOwnerHash(key) != (octPacked >> 16u))
                    continue;
                uint octAge = ((FrameIndex & 0xffffu) - (octPacked & 0xffffu)) & 0xffffu;
                if (octAge > OCT_QUERY_STALE_FRAMES)
                    continue;

                float4 probe = SampleOctIrradiance(octIndex, evalNormal);
                if (probe.w <= 0.0f)
                    continue;

                fallbackRadiance += probe.xyz * trilinear;
                fallbackWeight += trilinear;

                float surfaceWeight = OctProbeSurfaceWeight(slot, queryPos, cacheNormal);
                if (surfaceWeight <= 1e-4f)
                    continue;

                // Chebyshev visibility (DDGI): drop probes occluded from the
                // query point. Bias the query a little off the surface so a probe
                // doesn't self-occlude its own surface.
                float3 probePos = SanitizeFloat4(CellPositionIn[slot]).xyz;
                float3 biasedQuery = queryPos + cacheNormal * (safeCellSize * 0.1f);
                float visibility = OctChebyshevWeight(octIndex, probePos, biasedQuery);

                float w = trilinear * surfaceWeight * visibility;
                if (w <= 1e-5f)
                    continue;
                sumRadiance += probe.xyz * w;
                sumWeight += w;
            }
        }
    }

    if (sumWeight <= 1e-4f)
    {
        // All probes rejected by visibility — fall back to plain trilinear so the
        // pixel shows the available (leaky) GI rather than a black cell.
        if (fallbackWeight > 1e-4f)
            return fallbackRadiance / fallbackWeight;
        return float3(-1.0f, -1.0f, -1.0f); // sentinel: no valid probe at all
    }
    return sumRadiance / sumWeight;
}

float3 SampleOctIrradianceInterpolated(float3 worldPos, float3 evalNormal, float3 cacheNormal, uint hashLevel)
{
    return SampleOctIrradianceIndexed(worldPos, worldPos, evalNormal, cacheNormal, hashLevel);
}

float3 SampleOctIrradianceViewRayFallback(float3 worldPos, float3 evalNormal, float3 cacheNormal)
{
    float3 cameraPos = GetCameraPosition();
    float3 toCamera = cameraPos - worldPos;
    float viewLen = length(toCamera);
    if (viewLen <= 1e-3f)
        return float3(-1.0f, -1.0f, -1.0f);

    float3 viewDir = toCamera / viewLen;
    uint baseLevel;
    float baseCellSize = SpatialHashLeveledCellSize(worldPos, baseLevel);
    float maxTravel = min(viewLen * 0.95f, baseCellSize * SPATIAL_HASH_VIEW_FALLBACK_MAX_CELL_SCALE);

    [loop]
    for (uint stepIndex = 0u; stepIndex < SPATIAL_HASH_VIEW_FALLBACK_STEPS; ++stepIndex)
    {
        float travel = baseCellSize *
            (SPATIAL_HASH_VIEW_FALLBACK_START_CELL_SCALE + SPATIAL_HASH_VIEW_FALLBACK_STEP_CELL_SCALE * (float)stepIndex);
        if (travel > maxTravel)
            break;

        float3 indexPos = worldPos + viewDir * travel;
        uint indexLevel;
        SpatialHashLeveledCellSize(indexPos, indexLevel);

        float3 radiance = SampleOctIrradianceIndexed(indexPos, worldPos, evalNormal, cacheNormal, indexLevel);
        if (radiance.x >= 0.0f)
            return radiance;

        if (SpatialHashLevelParams.x >= 0.5f)
        {
            radiance = SampleOctIrradianceIndexed(indexPos, worldPos, evalNormal, cacheNormal, indexLevel + 1u);
            if (radiance.x >= 0.0f)
                return radiance;
            if (indexLevel > 0u)
            {
                radiance = SampleOctIrradianceIndexed(indexPos, worldPos, evalNormal, cacheNormal, indexLevel - 1u);
                if (radiance.x >= 0.0f)
                    return radiance;
            }
        }
    }

    return float3(-1.0f, -1.0f, -1.0f);
}

float3 SampleOctIrradianceNeighborhoodFallbackAtLevel(float3 worldPos, float3 evalNormal, float3 cacheNormal, uint hashLevel)
{
    float safeCellSize = max(CellSize, 1e-3f) * exp2((float)hashLevel);
    int3 levelOffset = SpatialHashLevelOffset(hashLevel);
    float3 gridPos = worldPos / safeCellSize;
    int3 baseCell = int3(floor(gridPos));

    float3 sumRadiance = 0.0f.xxx;
    float sumWeight = 0.0f;
    float3 fallbackRadiance = 0.0f.xxx;
    float fallbackWeight = 0.0f;

    [unroll]
    for (int z = -1; z <= 1; ++z)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                int3 rawCell = baseCell + int3(x, y, z);
                float3 cellCenter = (float3(rawCell) + 0.5f.xxx) * safeCellSize;
                float3 delta = (worldPos - cellCenter) / safeCellSize;
                float spatialWeight = exp(-dot(delta, delta) * 0.95f);
                if (spatialWeight <= 1e-4f)
                    continue;

                int3 cell = rawCell + levelOffset;
                uint slot = 0u;
                uint key = HashCellKeyFromCell(cell, cacheNormal, 0);
                if (!FindSlotForRead(key, slot))
                    continue;

                uint octIndex = slot & (OctCellCapacity - 1u);
                uint octPacked = OctCellKeyIn[octIndex];
                if (OctOwnerHash(key) != (octPacked >> 16u))
                    continue;
                uint octAge = ((FrameIndex & 0xffffu) - (octPacked & 0xffffu)) & 0xffffu;
                if (octAge > OCT_QUERY_STALE_FRAMES)
                    continue;

                float4 probe = SampleOctIrradiance(octIndex, evalNormal);
                if (probe.w <= 0.0f)
                    continue;

                fallbackRadiance += probe.xyz * spatialWeight;
                fallbackWeight += spatialWeight;

                float surfaceWeight = OctProbeSurfaceWeight(slot, worldPos, cacheNormal);
                if (surfaceWeight <= 1e-4f)
                    continue;

                float3 probePos = SanitizeFloat4(CellPositionIn[slot]).xyz;
                float3 biasedQuery = worldPos + cacheNormal * (safeCellSize * 0.1f);
                float visibility = OctChebyshevWeight(octIndex, probePos, biasedQuery);
                float weight = spatialWeight * surfaceWeight * visibility;
                if (weight <= 1e-5f)
                    continue;

                sumRadiance += probe.xyz * weight;
                sumWeight += weight;
            }
        }
    }

    if (sumWeight > 1e-4f)
        return sumRadiance / sumWeight;
    if (fallbackWeight > 1e-4f)
        return fallbackRadiance / fallbackWeight;
    return float3(-1.0f, -1.0f, -1.0f);
}

float3 SampleOctIrradianceNeighborhoodFallback(float3 worldPos, float3 evalNormal, float3 cacheNormal)
{
    uint primaryLevel;
    SpatialHashLeveledCellSize(worldPos, primaryLevel);

    float3 radiance = SampleOctIrradianceNeighborhoodFallbackAtLevel(worldPos, evalNormal, cacheNormal, primaryLevel);
    if (radiance.x >= 0.0f)
        return radiance;

    if (SpatialHashLevelParams.x >= 0.5f)
    {
        radiance = SampleOctIrradianceNeighborhoodFallbackAtLevel(worldPos, evalNormal, cacheNormal, primaryLevel + 1u);
        if (radiance.x >= 0.0f)
            return radiance;
        if (primaryLevel > 0u)
        {
            radiance = SampleOctIrradianceNeighborhoodFallbackAtLevel(worldPos, evalNormal, cacheNormal, primaryLevel - 1u);
            if (radiance.x >= 0.0f)
                return radiance;
        }
    }

    return float3(-1.0f, -1.0f, -1.0f);
}

// One thread per (probe, irradiance texel). Reconstructs the trace's ray
// directions, convolves the per-ray radiance with this texel's cosine lobe to
// form the directional irradiance E/pi, and temporally blends it into the
// persistent atlas. Atlas is indexed by hashSlot & (OctCellCapacity-1) so the
// history follows a cell (stable key->slot) rather than the transient probe
// order; collisions only occur when active cells approach OctCellCapacity.
groupshared uint gOctOwned;  // 1 if this group's cell owns its oct slot this frame
groupshared uint gOctFresh;  // 1 if it just took the slot over (reset history)
groupshared float4 gOctReservoirRay;      // xyz = direction, w = age
groupshared float4 gOctReservoirRadiance; // xyz = radiance, w = target luma

[numthreads(64, 1, 1)]
void SpatialHashOctBlend(uint3 DTid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    uint gid = DTid.x;
    uint probeIndex = gid / OCT_IRRADIANCE_TEXELS;
    uint texelIndex = gid % OCT_IRRADIANCE_TEXELS;

    // Group-uniform validity (one group == one probe == 64 texels), so every
    // thread reaches the ownership barrier together.
    uint activeCount = min(ActiveCounterIn[0], ActiveCellCapacity);
    uint octProbeCount = min(activeCount, OctCellCapacity);
    bool valid = (probeIndex < octProbeCount);
    uint slot = valid ? ActiveCellSlotsIn[probeIndex] : 0u;
    uint cellKey = valid ? ResolvedKeysIn[slot] : 0u;
    valid = valid && (slot < HashEntryCount) && (cellKey != 0u);
    uint octIndex = slot & (OctCellCapacity - 1u);

    // Claim oct-slot ownership (leader thread), broadcast to the group. Only the
    // owning cell writes this slot; colliding cells skip it (the query then falls
    // back to neighbour probes). Self-cleaning via a 16-bit frame stamp.
    if (groupIndex == 0u)
    {
        gOctOwned = 0u;
        gOctFresh = 0u;
        if (valid)
        {
            uint myHash = OctOwnerHash(cellKey);
            uint curStamp = FrameIndex & 0xffffu;
            uint packed = OctCellKeyOut[octIndex];
            uint ownerHash = packed >> 16u;
            uint ownerStamp = packed & 0xffffu;
            uint ageStamp = (curStamp - ownerStamp) & 0xffffu;
            bool takeable = (ownerHash == 0u) || (ownerHash == myHash) || (ageStamp > OCT_OWNER_STALE_FRAMES);
            if (takeable)
            {
                uint desired = (myHash << 16u) | curStamp;
                uint prev = 0u;
                InterlockedCompareExchange(OctCellKeyOut[octIndex], packed, desired, prev);
                if (prev == packed)
                {
                    gOctOwned = 1u;
                    gOctFresh = (ownerHash != myHash) ? 1u : 0u;
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gOctOwned == 0u)
        return; // a colliding cell owns this oct slot this frame - skip writing
    bool octFresh = (gOctFresh != 0u);
    float4 rotation = PerFrameRotationQuaternion(FrameIndex);

    if (groupIndex == 0u)
    {
        gOctReservoirRay = 0.0f.xxxx;
        gOctReservoirRadiance = 0.0f.xxxx;

        float3 currentDir = 0.0f.xxx;
        float3 currentRadiance = 0.0f.xxx;
        float currentWeightSum = 0.0f;
        uint reservoirSeed = HashUInt(cellKey ^ (FrameIndex * 747796405u) ^ (octIndex * 2891336453u));

        [loop]
        for (uint r = 0u; r < OCT_RAYS_PER_CELL; ++r)
        {
            float3 rayDir = RotateVectorByQuaternion(SphericalFibonacciDir(r, OCT_RAYS_PER_CELL), rotation);
            float3 rad = SanitizeFloat3(OctRayDataIn[probeIndex * OCT_RAYS_PER_CELL + r].xyz);
            float radLuma = Luminance(rad);
            if (radLuma > OCT_FIREFLY_LUMA_KNEE)
            {
                float clampedLuma = radLuma / (1.0f + (radLuma - OCT_FIREFLY_LUMA_KNEE) / OCT_FIREFLY_LUMA_KNEE);
                rad *= clampedLuma / radLuma;
                radLuma = clampedLuma;
            }
            if (radLuma <= OCT_RESERVOIR_MIN_TARGET)
                continue;

            currentWeightSum += radLuma;
            float u = HashToUnitFloat(reservoirSeed ^ (r * 0x9e3779b9u));
            if (u * currentWeightSum <= radLuma)
            {
                currentDir = rayDir;
                currentRadiance = rad;
            }
        }

        float4 prevRay = octFresh ? 0.0f.xxxx : SanitizeFloat4(OctReservoirRayOut[octIndex]);
        float4 prevRadiance = octFresh ? 0.0f.xxxx : SanitizeFloat4(OctReservoirRadianceOut[octIndex]);
        float prevAge = min(max(prevRay.w, 0.0f) + 1.0f, OCT_RESERVOIR_MAX_AGE);
        float prevTarget = max(prevRadiance.w, 0.0f);
        bool prevValid = prevAge > 1.0f && prevAge < OCT_RESERVOIR_MAX_AGE && prevTarget > OCT_RESERVOIR_MIN_TARGET;
        bool currentValid = currentWeightSum > OCT_RESERVOIR_MIN_TARGET;

        float prevRetention = prevValid ? (1.0f - saturate((prevAge - 1.0f) / OCT_RESERVOIR_MAX_AGE)) : 0.0f;
        float prevCombineTarget = prevTarget * prevRetention;
        bool useCurrent = currentValid;
        if (prevValid && currentValid)
        {
            float u = HashToUnitFloat(reservoirSeed ^ 0xa511e9b3u);
            useCurrent = (u * (prevCombineTarget + currentWeightSum) <= currentWeightSum);
        }
        else if (prevValid)
        {
            useCurrent = false;
        }

        if (useCurrent)
        {
            gOctReservoirRay = float4(SafeNormalize(currentDir, float3(0.0f, 1.0f, 0.0f)), 1.0f);
            gOctReservoirRadiance = float4(currentRadiance, max(currentWeightSum, OCT_RESERVOIR_MIN_TARGET));
        }
        else if (prevValid)
        {
            gOctReservoirRay = float4(SafeNormalize(prevRay.xyz, float3(0.0f, 1.0f, 0.0f)), prevAge);
            gOctReservoirRadiance = float4(max(prevRadiance.xyz, 0.0f.xxx), prevTarget);
        }

        OctReservoirRayOut[octIndex] = gOctReservoirRay;
        OctReservoirRadianceOut[octIndex] = gOctReservoirRadiance;
    }
    GroupMemoryBarrierWithGroupSync();

    uint tx = texelIndex % OCT_IRRADIANCE_RES;
    uint ty = texelIndex / OCT_IRRADIANCE_RES;
    float2 texelUV = (float2(tx, ty) + 0.5f) / (float)OCT_IRRADIANCE_RES;
    float3 texelDir = OctahedralUVToDirection(texelUV);

    float3 sumRadiance = 0.0f.xxx;
    float sumLuma = 0.0f;
    float sumLuma2 = 0.0f;
    float sumWeight = 0.0f;
    [loop]
    for (uint r = 0u; r < OCT_RAYS_PER_CELL; ++r)
    {
        float3 rayDir = RotateVectorByQuaternion(SphericalFibonacciDir(r, OCT_RAYS_PER_CELL), rotation);
        float w = max(0.0f, dot(texelDir, rayDir));
        if (w <= 0.0f)
            continue;
        float3 rad = SanitizeFloat3(OctRayDataIn[probeIndex * OCT_RAYS_PER_CELL + r].xyz);
        // Firefly soft-clamp: octahedral stores radiance per direction, so a
        // single ray hitting a very bright spot (direct light / emissive) makes
        // ONE texel pop as a bright dot (SH spreads it across low-order bands, so
        // SH doesn't show this). Soft-knee compress each ray's luminance above a
        // knee, preserving normal GI (< knee) while taming outliers.
        float radLuma = Luminance(rad);
        if (radLuma > OCT_FIREFLY_LUMA_KNEE)
        {
            float clampedLuma = radLuma / (1.0f + (radLuma - OCT_FIREFLY_LUMA_KNEE) / OCT_FIREFLY_LUMA_KNEE);
            rad *= clampedLuma / radLuma;
            radLuma = clampedLuma;
        }
        sumRadiance += rad * w;
        sumLuma += radLuma * w;
        sumLuma2 += radLuma * radLuma * w;
        sumWeight += w;
    }
    float reservoirAge = max(gOctReservoirRay.w, 0.0f);
    float reservoirTarget = max(gOctReservoirRadiance.w, 0.0f);
    if (reservoirAge > 1.0f && reservoirTarget > OCT_RESERVOIR_MIN_TARGET)
    {
        float3 reservoirDir = SafeNormalize(gOctReservoirRay.xyz, float3(0.0f, 1.0f, 0.0f));
        float reservoirWeight = max(0.0f, dot(texelDir, reservoirDir));
        if (reservoirWeight > 0.0f)
        {
            float ageGate = saturate((reservoirAge - 1.0f) / 4.0f);
            float decayGate = 1.0f - saturate((reservoirAge - 1.0f) / OCT_RESERVOIR_MAX_AGE);
            reservoirWeight *= OCT_RESERVOIR_VIRTUAL_WEIGHT * ageGate * decayGate;
            float3 reservoirRadiance = max(SanitizeFloat3(gOctReservoirRadiance.xyz), 0.0f.xxx);
            float reservoirLuma = Luminance(reservoirRadiance);
            sumRadiance += reservoirRadiance * reservoirWeight;
            sumLuma += reservoirLuma * reservoirWeight;
            sumLuma2 += reservoirLuma * reservoirLuma * reservoirWeight;
            sumWeight += reservoirWeight;
        }
    }
    // Cosine-weighted mean radiance over the hemisphere == E/pi, matching the SH
    // path's EvaluateSHDiffuse * (1/pi) output so the A/B brightness is equal.
    float3 newIrradiance = sumWeight > 1e-4f ? (sumRadiance / sumWeight) : 0.0f.xxx;
    float meanLuma = sumWeight > 1e-4f ? (sumLuma / sumWeight) : 0.0f;
    float lumaVariance = sumWeight > 1e-4f ? max(sumLuma2 / sumWeight - meanLuma * meanLuma, 0.0f) : 0.0f;
    float relativeVariance = lumaVariance / max(meanLuma * meanLuma, 1e-4f);
    float lowLightGate = 1.0f - smoothstep(OCT_LOW_LIGHT_NOISE_LUMA * 0.35f, OCT_LOW_LIGHT_NOISE_LUMA, meanLuma);
    float noisyLowLight = lowLightGate * smoothstep(OCT_LOW_LIGHT_VARIANCE_START, OCT_LOW_LIGHT_VARIANCE_END, relativeVariance);

    uint outIdx = octIndex * OCT_IRRADIANCE_TEXELS + texelIndex;
    float4 prev = SanitizeFloat4(OctIrradianceOut[outIdx]);
    // Fresh take-over of a (possibly stale) oct slot -> ignore inherited history.
    float prevFrames = octFresh ? 0.0f : max(prev.w, 0.0f);

    // Near-camera cells adapt faster: cap their accumulated history shorter so
    // they reach a stable value in fewer frames. Strength is OctNearConvergenceBias
    // (0 = uniform/long cap everywhere, 1 = strong near priority). Far cells keep
    // the long cap for low-noise steady state.
    float3 probePos = SanitizeFloat4(CellPositionIn[slot]).xyz;
    float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
    float proximity = saturate(1.0f - length(probePos - camPos) / max(ProjectionParams.w * 0.3f, 1.0f));
    float maxFrames = lerp(OCT_MAX_HISTORY_FRAMES, 48.0f, proximity * saturate(OctNearConvergenceBias));
    maxFrames = lerp(maxFrames, OCT_MAX_HISTORY_FRAMES, noisyLowLight);

    float3 blended;
    float frames;
    if (prevFrames <= 0.0f)
    {
        blended = newIrradiance;
        frames = 1.0f;
    }
    else
    {
        float accepted = min(prevFrames + 1.0f, maxFrames);
        float alpha = saturate(1.0f / max(accepted, 1.0f));
        if (prevFrames >= OCT_NEW_CELL_RAMP_FRAMES)
            alpha *= lerp(1.0f, OCT_LOW_LIGHT_ALPHA_SCALE, noisyLowLight);
        if (prevFrames < OCT_NEW_CELL_RAMP_FRAMES)
        {
            float boost = 1.0f - saturate(prevFrames / OCT_NEW_CELL_RAMP_FRAMES);
            alpha = saturate(alpha + boost * (0.6f - alpha));
        }
        // Adaptive recovery: a probe locked DARK (stale history from slot reuse,
        // or an early bad gather) barely updates at alpha=1/maxFrames. If the new
        // sample is brighter, weight it more so the dark cell recovers in a few
        // frames. GATED to dark probes (darkGate) so a normal probe getting a
        // transient bright sample does NOT spike — that gating prevents the
        // adaptive term from amplifying fireflies into bright dots.
        float prevLuma = Luminance(prev.xyz);
        float newLuma = Luminance(newIrradiance);
        float brighten = saturate((newLuma - prevLuma) / max(newLuma, 1e-3f));
        float darkGate = saturate(1.0f - prevLuma / 0.15f); // 1 when history is dark
        float recoveryScale = lerp(1.0f, OCT_LOW_LIGHT_ALPHA_SCALE, noisyLowLight);
        alpha = max(alpha, brighten * darkGate * 0.5f * recoveryScale);
        blended = lerp(prev.xyz, newIrradiance, alpha);
        frames = accepted;
    }
    OctIrradianceOut[outIdx] = float4(max(blended, 0.0f.xxx), frames);
}

// One thread per (probe, depth texel). Builds the octahedral visibility map:
// for this texel's direction, the sharpened-cosine-weighted mean and mean^2 of
// the per-ray hit distances. Consumed by OctChebyshevWeight at query time to
// kill light leak. Texels that received no rays this frame keep their previous
// value (no decay toward zero).
[numthreads(64, 1, 1)]
void SpatialHashOctDepthBlend(uint3 DTid : SV_DispatchThreadID)
{
    uint gid = DTid.x;
    uint probeIndex = gid / OCT_DEPTH_TEXELS;
    uint texelIndex = gid % OCT_DEPTH_TEXELS;

    uint activeCount = min(ActiveCounterIn[0], ActiveCellCapacity);
    uint octProbeCount = min(activeCount, OctCellCapacity);
    if (probeIndex >= octProbeCount)
        return;

    uint slot = ActiveCellSlotsIn[probeIndex];
    if (slot >= HashEntryCount || ResolvedKeysIn[slot] == 0u)
        return;
    uint octIndex = slot & (OctCellCapacity - 1u);
    // Only the owning cell (set by the irradiance blend earlier this frame) writes
    // this oct slot's depth; colliding cells skip so they don't corrupt it.
    if (OctOwnerHash(ResolvedKeysIn[slot]) != (OctCellKeyOut[octIndex] >> 16u))
        return;

    uint tx = texelIndex % OCT_DEPTH_RES;
    uint ty = texelIndex / OCT_DEPTH_RES;
    float2 texelUV = (float2(tx, ty) + 0.5f) / (float)OCT_DEPTH_RES;
    float3 texelDir = OctahedralUVToDirection(texelUV);

    float maxDist = max(CellSize, 1e-3f) * OCT_DEPTH_MAX_CELLS;
    float4 rotation = PerFrameRotationQuaternion(FrameIndex);
    float sumDist = 0.0f;
    float sumDist2 = 0.0f;
    float sumWeight = 0.0f;
    [loop]
    for (uint r = 0u; r < OCT_RAYS_PER_CELL; ++r)
    {
        float3 rayDir = RotateVectorByQuaternion(SphericalFibonacciDir(r, OCT_RAYS_PER_CELL), rotation);
        float c = max(0.0f, dot(texelDir, rayDir));
        if (c <= 0.0f)
            continue;
        float w = pow(c, OCT_DEPTH_SHARPNESS);
        float dist = min(OctRayDataIn[probeIndex * OCT_RAYS_PER_CELL + r].w, maxDist);
        sumDist += dist * w;
        sumDist2 += dist * dist * w;
        sumWeight += w;
    }
    if (sumWeight <= 1e-4f)
        return; // no rays landed in this texel's lobe this frame — keep history

    float2 newMoments = float2(sumDist / sumWeight, sumDist2 / sumWeight);
    uint outIdx = octIndex * OCT_DEPTH_TEXELS + texelIndex;
    float2 prev = OctDepthOut[outIdx];
    float2 blended = (prev.x <= 0.0f) ? newMoments : lerp(prev, newMoments, 0.1f);
    OctDepthOut[outIdx] = blended;
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

    // Octahedral DDGI query: look up the cell's probe (stable key->slot->octIndex)
    // and bilinearly sample its octahedral irradiance map in the pixel normal
    // direction. Output is E/pi, matching the SH path's scale.
    if (GIMode == 1u)
    {
        // Stochastic lookup dither: jitter the query position within the tangent
        // plane by a fraction of a cell, varying per pixel AND per frame. A pixel
        // sitting on a deterministically-unpopulated cell (corner where a colliding
        // cell starves the oct slot, or a level/cell seam) otherwise stays black
        // while the camera is still; jitter makes it sample slightly different
        // neighbour cells each frame and the screen-resolve temporal averages them,
        // filling the gap. The cache (insert) is NOT jittered, so accumulation
        // stays stable.
        uint primaryLevel;
        float octQueryCellSize = SpatialHashLeveledCellSize(worldPos, primaryLevel);
        uint jitterSeed = pixelPos.x * 1973u + pixelPos.y * 9277u + FrameIndex * 26699u;
        float2 jitter2 = float2(HashToUnitFloat(jitterSeed), HashToUnitFloat(jitterSeed ^ 0xb5297a4du)) - 0.5f;
        float3 jt = SafeNormalize(cross(cacheNormal, abs(cacheNormal.y) < 0.9f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f)), float3(1.0f, 0.0f, 0.0f));
        float3 jb = cross(cacheNormal, jt);
        float3 octQueryPos = worldPos + (jt * jitter2.x + jb * jitter2.y) * (octQueryCellSize * 0.5f);
        SpatialHashLeveledCellSize(octQueryPos, primaryLevel);
        float3 octRadiance = SampleOctIrradianceInterpolated(octQueryPos, pixelNormal, cacheNormal, primaryLevel);
        // At a level boundary the just-crossed primary-level cells may not be
        // populated yet (transient black). Fall back to the coarser level, which
        // far regions keep populated, then the finer level — bridging the gap.
        if (octRadiance.x < 0.0f && SpatialHashLevelParams.x >= 0.5f)
            octRadiance = SampleOctIrradianceInterpolated(octQueryPos, pixelNormal, cacheNormal, primaryLevel + 1u);
        if (octRadiance.x < 0.0f && SpatialHashLevelParams.x >= 0.5f && primaryLevel > 0u)
            octRadiance = SampleOctIrradianceInterpolated(octQueryPos, pixelNormal, cacheNormal, primaryLevel - 1u);
        if (octRadiance.x < 0.0f)
            octRadiance = SampleOctIrradianceViewRayFallback(worldPos, pixelNormal, cacheNormal);
        if (octRadiance.x < 0.0f)
            octRadiance = SampleOctIrradianceNeighborhoodFallback(worldPos, pixelNormal, cacheNormal);
        if (octRadiance.x >= 0.0f)
        {
            octRadiance = max(SanitizeFloat3(octRadiance), 0.0f.xxx);
            OutGIHashColor[pixelPos] = float4(octRadiance, 1.0f);
            OutGIHashSH[pixelPos] = float4(octRadiance, 1.0f);
        }
        else
        {
            // No cached GI for this cell (uncached / starved oct slot) — fill with
            // the camera ambient probe instead of black. frames=1 so the
            // screen-resolve uses it.
            float3 ambientFill = max(EvaluateUncachedAmbient(pixelNormal), 0.0f.xxx);
            OutGIHashColor[pixelPos] = float4(ambientFill, 1.0f);
            OutGIHashSH[pixelPos] = float4(ambientFill, 1.0f);
        }
        return;
    }

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
        // Uncached cell — camera-ambient fill instead of black.
        float3 ambientFill = max(EvaluateUncachedAmbient(pixelNormal), 0.0f.xxx);
        OutGIHashColor[pixelPos] = float4(ambientFill, 1.0f);
        OutGIHashSH[pixelPos] = float4(ambientFill, 1.0f);
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
            // Skip no-data (sentinel/black) samples — frames<=0 means the cell
            // query found nothing. Including them would darken the gather and
            // spread the black; excluding them lets a black centre pixel be filled
            // from its valid same-surface neighbours.
            if (sample.w <= 0.0f)
                continue;
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
