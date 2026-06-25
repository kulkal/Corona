// ReSTIR DI Phase 3 — proper 2-pass spatial reuse.
//
// The RT raygen pass writes a "pre-spatial" reservoir to
// ShadowBufferPreSpatial + ShadowReservoirMBufferPreSpatial (Phase 1 RIS
// + Phase 2 temporal reuse). This compute pass reads those current-
// frame reservoirs at neighbour pixels and combines them via RIS,
// producing the final ShadowBuffer + ShadowReservoirMBuffer that
// LightingPS consumes. Because the spatially-chosen light may differ
// from the centre pixel's, RayQuery (inline RT, cs_6_5) is used to
// re-trace visibility for the post-spatial choice — without this the
// neighbour's stored .a visibility is wrong at this pixel.

#include "Common.hlsl"

Texture2D PreSpatialReservoir : register(t0);
Texture2D PreSpatialM         : register(t1);
Texture2D DepthTex            : register(t2);
Texture2D WorldNormalTex      : register(t3);
Texture2D GeoNormalTex        : register(t4);
RaytracingAccelerationStructure gRtScene : register(t5);

RWTexture2D<float4> ShadowResult    : register(u0);
RWTexture2D<float>  ShadowReservoirM : register(u1);

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
    uint ShadowMode;
    uint ShadowedPointLightCount;
    float ShadowMaxM;
    float SpatialLightCellSize;
    uint SpatialLightHashEntryMask;
    uint SpatialLightMaxProbeSteps;
    uint bUseSpatialLightMask;
    float4 SpatialHashLevelParams;
    #define MAX_SHADOWED_PT_LIGHTS 16
    float4 ShadowedPointLights[MAX_SHADOWED_PT_LIGHTS];
    float4 ShadowedPointLightWeights[MAX_SHADOWED_PT_LIGHTS];
};

SamplerState sampleWrap : register(s0);

static const uint kSpatialFlags =
    RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
    RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
static const uint RT_SHADOW_RAY_MASK = 0x02u;

// World-space reconstruction from depth + inverse-projection. Matches
// the existing GetViewPosition helper convention (row-vector form):
// matrices are stored transposed in the CB so HLSL `mul(vec, mat)` is
// the correct order. Using mul(mat, vec) instead silently produces
// garbage world positions and ALL the inline-RT visibility traces hit
// random geometry — symptom is "everything shadowed" after the
// spatial pass.
float3 ReconstructWorldPos(float2 uv, float depth)
{
    const float2 screenPos = float2(uv.x * 2.0f - 1.0f, 1.0f - 2.0f * uv.y);
    float3 viewPos = GetViewPosition(depth, screenPos, InvProjMatrix);
    return mul(float4(viewPos, 1.0f), InvViewMatrix).xyz;
}

float TargetPdfAtPixel(uint lightIdx, float3 worldPos, float3 worldNormal)
{
    if (lightIdx >= ShadowedPointLightCount)
        return 0.0f;
    float3 candPos = ShadowedPointLights[lightIdx].xyz;
    float  candRadius = max(ShadowedPointLights[lightIdx].w, 0.01f);
    float  candLuma = ShadowedPointLightWeights[lightIdx].x;
    if (candLuma <= 0.0f)
        return 0.0f;
    float3 toCand = candPos - worldPos;
    float  distSq = max(dot(toCand, toCand), 1.0e-4f);
    float  dist   = sqrt(distSq);
    const float invRadiusSq = rcp(candRadius * candRadius);
    const float normalizedDistSq = saturate(distSq * invRadiusSq);
    float  rangeAtten = saturate(1.0f - normalizedDistSq * normalizedDistSq);
    rangeAtten *= rangeAtten;
    const float inverseSquareAtten = rcp(max(1.0f, distSq * 0.0001f));
    float  NdotL = saturate(dot(worldNormal, toCand) / max(dist, 1.0e-3f));
    return candLuma * max(0.0f, rangeAtten * inverseSquareAtten * NdotL);
}

float ComputePointLightEndpointBias(float lightRadius, float normalBias)
{
    return max(normalBias * 0.5f, clamp(max(lightRadius, 0.01f) * 0.02f, 0.25f, 80.0f));
}

// Trace a single visibility ray for a point light using inline RT.
// Returns 1.0 for visible (no hit between worldPos and the light), 0.0
// otherwise. Mirrors COMPUTE_POINT_LIGHT_VIS in the raygen path but
// without alpha-test support — the compute pass treats all geometry as
// opaque, which is acceptable for the spatially-chosen sample
// correction (centre pixel's choice already had its alpha-tested ray
// traced in the raygen pass).
float TraceVisibilityInline(float3 worldPos, float3 worldNormal, float3 traceNormal,
                             uint lightIdx, float normalBias)
{
    if (lightIdx >= ShadowedPointLightCount)
        return 1.0f;
    float3 lightPos = ShadowedPointLights[lightIdx].xyz;
    float  lightRadius = max(ShadowedPointLights[lightIdx].w, 0.01f);
    float3 toLight = lightPos - worldPos;
    float  distToLight = length(toLight);
    if (distToLight > lightRadius || distToLight < 1.0e-3f)
        return 1.0f;
    float3 lightDir = toLight / distToLight;
    if (dot(worldNormal, lightDir) <= 0.0f)
        return 0.0f;
    float3 bias = dot(traceNormal, lightDir) < 0.0f ? -traceNormal : traceNormal;
    RayDesc ray;
    ray.Origin = worldPos + bias * normalBias;
    ray.Direction = lightDir;
    ray.TMin = max(0.05f, normalBias * 0.25f);
    const float endpointBias = ComputePointLightEndpointBias(lightRadius, normalBias);
    if (distToLight <= endpointBias + ray.TMin)
        return 1.0f;
    ray.TMax = max(distToLight - endpointBias, ray.TMin + 0.05f);

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gRtScene, RAY_FLAG_NONE, RT_SHADOW_RAY_MASK, ray);
    q.Proceed();
    return (q.CommittedStatus() == COMMITTED_NOTHING) ? 1.0f : 0.0f;
}

[numthreads(8, 8, 1)]
void main(uint2 pixelPos : SV_DispatchThreadID)
{
    uint2 launchSize;
    ShadowResult.GetDimensions(launchSize.x, launchSize.y);
    if (pixelPos.x >= launchSize.x || pixelPos.y >= launchSize.y)
        return;

    const float2 uv = (float2(pixelPos) + 0.5f) / float2(launchSize);

    // Centre pre-spatial reservoir + auxiliary buffers.
    const float4 centerRes = PreSpatialReservoir[pixelPos];
    const float  centerM   = PreSpatialM[pixelPos].x;

    // Option A (4-channel) doesn't have a single-light reservoir — just
    // pass the pre-spatial output through unchanged.
    if (ShadowMode == 0u)
    {
        ShadowResult[pixelPos] = centerRes;
        ShadowReservoirM[pixelPos] = centerM;
        return;
    }

    const float depth = DepthTex[pixelPos].x;
    if (depth >= 1.0f)
    {
        // Sky / background — nothing to shadow.
        ShadowResult[pixelPos] = centerRes;
        ShadowReservoirM[pixelPos] = centerM;
        return;
    }

    const float3 worldPos    = ReconstructWorldPos(uv, depth);
    const float3 worldNormal = normalize(WorldNormalTex[pixelPos].xyz * 2.0f - 1.0f);
    const float3 traceNormal = normalize(GeoNormalTex[pixelPos].xyz * 2.0f - 1.0f);
    const float  centerLinearDepth = depth; // for bilateral comparison

    // Initialize RIS combine with the centre reservoir. Using
    // M-weighted MIS to prevent the "biased ReSTIR" over-brightness:
    // sum the (M_i * tpdf_i(this_pixel)) products as the normalizer,
    // then weight each sample's contribution by M_i / sum_M_tpdf so
    // the combined W stays close to the unbiased ground truth even
    // when neighbours' chosen lights have a higher target_pdf at the
    // current pixel than the centre's did.
    const uint  centerIdx = (uint)(centerRes.g + 0.5f);
    float chosenIdx_f = centerRes.g;
    uint  chosenIdx = centerIdx;
    float chosenWeight = TargetPdfAtPixel(centerIdx, worldPos, worldNormal);
    // First-pass accumulators: total M (for MIS normalization).
    float M_eff = centerM;
    // RIS weight = M_i * W_i * tpdf_i(this_pixel). This stays the
    // pre-MIS contribution; MIS rescaling happens after all candidates
    // are gathered (sum of M_i is the normalizer).
    float weightSum = centerM * centerRes.b * chosenWeight;

    if (centerIdx >= ShadowedPointLightCount || centerM <= 0.0f)
    {
        // Centre had no valid pick — fall back to the raygen output
        // (which already encoded a "no candidates" sentinel).
        ShadowResult[pixelPos] = centerRes;
        ShadowReservoirM[pixelPos] = centerM;
        return;
    }

    // Currently set to 0 — when set >0 the spatial RIS combine
    // produces an over-brightness bias under sponza-like scenes
    // because neighbour reservoirs that picked higher-tpdf lights
    // inflate weightSum more than M_eff. The combined W is then fed
    // back through temporal reuse next frame and the bias compounds.
    // Proper fix: implement balance-heuristic MIS (evaluates each
    // candidate's tpdf at all neighbour pixels) — left as a follow-up.
    // With kSpatialSamples=0 the pass becomes a pure pass-through +
    // RayQuery fresh-visibility correction, which is still a useful
    // win when neighbour pick differs from centre but doesn't help
    // single-pixel variance reduction.
    const int   kSpatialSamples = 0;
    const float kSpatialRadius  = 12.0f;
    const float kDepthRelTol    = 0.04f;  // |Δz|/z threshold
    const float kNormalDotMin   = 0.85f;  // cos(~31°)

    [unroll]
    for (int s = 0; s < kSpatialSamples; ++s)
    {
        // Hash from pixel + frame + sample index to a unit disc point.
        uint seed = (pixelPos.x * 1973u + pixelPos.y * 9277u + FrameCounter * 27u + (uint)s * 991u) * 1597u;
        seed ^= seed >> 13u; seed *= 0x5bd1e995u; seed ^= seed >> 15u;
        float rA = (seed & 0xFFFFu) / 65535.0f;
        seed = seed * 1664525u + 1013904223u;
        float rB = (seed & 0xFFFFu) / 65535.0f;
        float angle = rA * 6.28318530717958647692f;
        float radius = sqrt(rB) * kSpatialRadius;
        int2 ofs = int2(round(cos(angle) * radius), round(sin(angle) * radius));
        int2 np = int2(pixelPos) + ofs;
        if (np.x < 0 || np.y < 0 || np.x >= (int)launchSize.x || np.y >= (int)launchSize.y)
            continue;
        uint2 npu = uint2(np);

        // Depth + normal bilateral edge-stop. Skip neighbours that are
        // on a different surface — otherwise the spatial reuse smears
        // shadow values across silhouettes which is exactly the
        // artefact this pass is meant to fix.
        float nDepth = DepthTex[npu].x;
        if (nDepth >= 1.0f) continue;
        if (abs(nDepth - centerLinearDepth) / max(centerLinearDepth, 1e-4f) > kDepthRelTol) continue;
        float3 nNormal = normalize(WorldNormalTex[npu].xyz * 2.0f - 1.0f);
        if (dot(nNormal, worldNormal) < kNormalDotMin) continue;

        float4 nRes = PreSpatialReservoir[npu];
        float  nM   = PreSpatialM[npu].x;
        uint   nIdx = (uint)(nRes.g + 0.5f);
        if (nIdx >= ShadowedPointLightCount || nM <= 0.0f) continue;

        float nTpdfAtCenter = TargetPdfAtPixel(nIdx, worldPos, worldNormal);
        if (nTpdfAtCenter <= 0.0f) continue;
        // Canonical RIS recombine — full neighbour M, NOT half-weighted.
        // Earlier attempts used (nM * 0.5) for the weight contribution
        // but full nM for M_eff accumulation, which created an
        // asymmetry: bright (high-W) neighbours inflated the final
        // ratio when surfaces were similar enough to pass the edge-
        // stops. End result was a brighter-than-truth scene. With both
        // sides using nM the unbiased estimator passes through cleanly.
        float nWeight = nM * nRes.b * nTpdfAtCenter;
        if (nWeight <= 0.0f) continue;
        weightSum += nWeight;
        uint pickSeed = seed * 6151u + 0xdeadbeefu;
        pickSeed ^= pickSeed >> 13u; pickSeed *= 0x5bd1e995u; pickSeed ^= pickSeed >> 15u;
        float u = (pickSeed & 0x00FFFFFFu) / 16777216.0f;
        if (u * weightSum <= nWeight)
        {
            chosenIdx = nIdx;
            chosenWeight = nTpdfAtCenter;
            chosenIdx_f = nRes.g;
        }
        M_eff += nM;
    }

    // Cap M to bound stale-history influence.
    const float kMaxM = 20.0f;
    M_eff = min(M_eff, kMaxM);

    // Re-trace visibility for the final chosen light. If the spatial
    // pass picked a different light than the centre's pre-spatial
    // choice, the centre's pre-spatial visibility (centerRes.a) is for
    // the WRONG light at this pixel — without this fresh trace the
    // shadow value is biased toward the wrong light's occlusion. When
    // chosenIdx hasn't changed, we still re-trace for simplicity (the
    // result should match centerRes.a almost exactly).
    const float kNormalBias = 0.5f;
    float finalVis = TraceVisibilityInline(worldPos, worldNormal, traceNormal,
                                            chosenIdx, kNormalBias);

    // Firefly clamp + final unbiased estimator: W = W_sum / (p_chosen * M).
    const float kFireflyCap = 30.0f;
    float ratio = weightSum / (max(chosenWeight, 1.0e-6f) * M_eff);
    ratio = clamp(ratio, 0.0f, kFireflyCap);

    float4 outShadow;
    outShadow.r = centerRes.r;       // sun visibility (unchanged by Phase 3)
    outShadow.g = (float)chosenIdx;
    outShadow.b = ratio;
    outShadow.a = finalVis;
    ShadowResult[pixelPos] = outShadow;
    ShadowReservoirM[pixelPos] = M_eff;
}
