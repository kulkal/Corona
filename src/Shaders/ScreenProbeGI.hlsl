#include "Common.hlsl"

Texture2D DepthTex : register(t0);
Texture2D WorldNormalTex : register(t1);
Texture2D GeoNormalTex : register(t2);
Texture2D ScreenProbeRadianceTex : register(t3);
Texture2D ScreenProbeMetaTex : register(t4);
Texture2D PrevScreenProbeGITex : register(t5);
Texture2D VelocityTex : register(t6);
Texture2D PrevDepthTex : register(t7);
Texture2D PrevNormalTex : register(t8);
Texture2D ScreenProbeSH0Tex : register(t9);
Texture2D ScreenProbeSH1Tex : register(t10);
Texture2D ScreenProbeSH2Tex : register(t11);
Texture2D ScreenProbeSH3Tex : register(t12);
Texture2D ScreenProbeSH4Tex : register(t13);
Texture2D ScreenProbeSH5Tex : register(t14);
Texture2D ScreenProbeSH6Tex : register(t15);
Texture2D ScreenProbeSH7Tex : register(t16);
Texture2D ScreenProbeSH8Tex : register(t17);

RWTexture2D<float4> OutScreenProbeGI : register(u0);
RWTexture2D<float4> OutScreenProbeDebug : register(u1);
RWTexture2D<float4> OutScreenProbeHistory : register(u2);

SamplerState BilinearClamp : register(s0);

cbuffer ScreenProbeGIConstant : register(b0)
{
    float4 ProjectionParams;
    float2 RTSize;
    float2 ProbeGridSize;
    uint ProbeSpacing;
    uint GatherRadius;
    float ProbeDepthWeight;
    float ProbeNormalWeight;
    float ResolveDepthWeight;
    float ResolveNormalWeight;
    float RawBlend;
    float MinResolveWeight;
    uint FrameIndex;
    uint Padding;
    float TemporalAlpha;
    float HistoryDepthWeight;
    float HistoryNormalWeight;
    uint HistoryValid;
    float EdgeDepthWeight;
    float EdgeNormalWeight;
    uint EdgeSampleCount;
    uint SHCoefficientCount;
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

struct SH3RGB
{
    float3 c0;
    float3 c1;
    float3 c2;
    float3 c3;
    float3 c4;
    float3 c5;
    float3 c6;
    float3 c7;
    float3 c8;
};

static const float SH3_DIFFUSE_SCALE = 1.0f;
static const float MAX_SCREEN_PROBE_HISTORY_FRAMES = 65504.0f;
static const float MIN_SCREEN_PROBE_HISTORY_CONFIDENCE = 0.25f;

float3 SafeNormalize(float3 value, float3 fallback)
{
    value = SanitizeFloat3(value);
    float lenSq = dot(value, value);
    if (lenSq < 1e-8f)
        return fallback;
    return value * rsqrt(lenSq);
}

float LoadLinearDepth(uint2 pos)
{
    float deviceDepth = DepthTex[pos].x;
    return GetLinearDepthOpenGL(deviceDepth, ProjectionParams.z, ProjectionParams.w);
}

float3 LoadGeoNormal(uint2 pos)
{
    return SafeNormalize(GeoNormalTex[pos].xyz, SafeNormalize(WorldNormalTex[pos].xyz, float3(0.0f, 1.0f, 0.0f)));
}

float3 LoadPixelNormal(uint2 pos)
{
    return SafeNormalize(WorldNormalTex[pos].xyz, LoadGeoNormal(pos));
}

uint2 ClampPixelCoord(float2 pixelCenter)
{
    float2 maxPixel = max(RTSize - 1.0f.xx, 0.0f.xx);
    return uint2(clamp(floor(pixelCenter), 0.0f.xx, maxPixel));
}

float GeometryCompatibility(float depthA, float3 normalA, float depthB, float3 normalB, float depthWeight, float normalWeight)
{
    float depthDelta = abs(depthA - depthB) / max(min(depthA, depthB), 1e-3f);
    float depthCompatibility = exp(-depthDelta * depthWeight);
    float normalCompatibility = pow(saturate(dot(normalA, normalB)), normalWeight);
    return depthCompatibility * normalCompatibility;
}

float ComputeEdgeStopWeight(uint2 pixelPos, float currentDepth, float3 currentGeoNormal, float2 probePixel, float probeDepth, float3 probeNormal)
{
    float2 pixelCenter = float2(pixelPos) + 0.5f.xx;
    float2 probeCenter = clamp(probePixel, 0.5f.xx, RTSize - 0.5f.xx);
    float2 probeDelta = probeCenter - pixelCenter;
    if (dot(probeDelta, probeDelta) < 1.0f)
        return 1.0f;

    // UE5/Lumen-style edge awareness: do not gather a probe if the screen-space
    // segment to that probe crosses a depth/normal discontinuity.
    uint sampleCount = clamp(EdgeSampleCount, 1u, 4u);
    float edgeWeight = 1.0f;
    [loop]
    for (uint sampleIndex = 0; sampleIndex < 4u; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
            continue;

        float t = (float(sampleIndex) + 1.0f) / (float(sampleCount) + 1.0f);
        uint2 samplePos = ClampPixelCoord(pixelCenter + probeDelta * t);
        float sampleDepth = LoadLinearDepth(samplePos);
        float3 sampleNormal = LoadGeoNormal(samplePos);

        float segmentWeight = GeometryCompatibility(
            sampleDepth,
            sampleNormal,
            currentDepth,
            currentGeoNormal,
            EdgeDepthWeight,
            EdgeNormalWeight);

        edgeWeight = min(edgeWeight, segmentWeight);
        currentDepth = sampleDepth;
        currentGeoNormal = sampleNormal;
    }

    edgeWeight = min(edgeWeight, GeometryCompatibility(
        probeDepth,
        probeNormal,
        currentDepth,
        currentGeoNormal,
        EdgeDepthWeight,
        EdgeNormalWeight));

    return saturate(edgeWeight);
}

float4 LoadProbe(uint2 probeCoord)
{
    probeCoord = min(probeCoord, uint2(ProbeGridSize) - 1u);
    return float4(max(SanitizeFloat3(ScreenProbeRadianceTex[probeCoord].xyz), 0.0f.xxx), 1.0f);
}

SH3RGB LoadProbeSH(uint2 probeCoord)
{
    probeCoord = min(probeCoord, uint2(ProbeGridSize) - 1u);
    SH3RGB sh;
    sh.c0 = SanitizeFloat3(ScreenProbeSH0Tex[probeCoord].xyz);
    sh.c1 = SanitizeFloat3(ScreenProbeSH1Tex[probeCoord].xyz);
    sh.c2 = SanitizeFloat3(ScreenProbeSH2Tex[probeCoord].xyz);
    sh.c3 = SanitizeFloat3(ScreenProbeSH3Tex[probeCoord].xyz);
    sh.c4 = 0.0f.xxx;
    sh.c5 = 0.0f.xxx;
    sh.c6 = 0.0f.xxx;
    sh.c7 = 0.0f.xxx;
    sh.c8 = 0.0f.xxx;
    if (SHCoefficientCount > 4u)
    {
        sh.c4 = SanitizeFloat3(ScreenProbeSH4Tex[probeCoord].xyz);
        sh.c5 = SanitizeFloat3(ScreenProbeSH5Tex[probeCoord].xyz);
        sh.c6 = SanitizeFloat3(ScreenProbeSH6Tex[probeCoord].xyz);
        sh.c7 = SanitizeFloat3(ScreenProbeSH7Tex[probeCoord].xyz);
        sh.c8 = SanitizeFloat3(ScreenProbeSH8Tex[probeCoord].xyz);
    }
    return sh;
}

float4 LoadProbeMeta(uint2 probeCoord)
{
    probeCoord = min(probeCoord, uint2(ProbeGridSize) - 1u);
    return ScreenProbeMetaTex[probeCoord];
}

float3 EvaluateProbeSH(uint2 probeCoord, float3 shadingNormal)
{
    float3 probeRadiance = LoadProbe(probeCoord).xyz;
    SH3RGB sh = LoadProbeSH(probeCoord);
    shadingNormal = SafeNormalize(shadingNormal, float3(0.0f, 1.0f, 0.0f));

    float x = shadingNormal.x;
    float y = shadingNormal.y;
    float z = shadingNormal.z;

    float b0 = 0.282095f;
    float b1 = 0.488603f * y;
    float b2 = 0.488603f * z;
    float b3 = 0.488603f * x;
    // UE's SH3 path keeps L0/L1/L2. Apply the Lambertian convolution
    // constants per band to evaluate diffuse irradiance from radiance SH.
    float3 shRadiance =
        PI * sh.c0 * b0 +
        (2.0f * PI / 3.0f) * (sh.c1 * b1 + sh.c2 * b2 + sh.c3 * b3);
    if (SHCoefficientCount > 4u)
    {
        float b4 = 1.092548f * x * y;
        float b5 = 1.092548f * y * z;
        float b6 = 0.315392f * (3.0f * z * z - 1.0f);
        float b7 = 1.092548f * x * z;
        float b8 = 0.546274f * (x * x - y * y);
        shRadiance += (PI / 4.0f) * (sh.c4 * b4 + sh.c5 * b5 + sh.c6 * b6 + sh.c7 * b7 + sh.c8 * b8);
    }
    shRadiance = max(shRadiance * SH3_DIFFUSE_SCALE, 0.0f.xxx);

    float shEnergy =
        dot(abs(sh.c0), 1.0f.xxx) +
        dot(abs(sh.c1), 1.0f.xxx) +
        dot(abs(sh.c2), 1.0f.xxx) +
        dot(abs(sh.c3), 1.0f.xxx);
    if (SHCoefficientCount > 4u)
    {
        shEnergy +=
            dot(abs(sh.c4), 1.0f.xxx) +
            dot(abs(sh.c5), 1.0f.xxx) +
            dot(abs(sh.c6), 1.0f.xxx) +
            dot(abs(sh.c7), 1.0f.xxx) +
            dot(abs(sh.c8), 1.0f.xxx);
    }
    return shEnergy > 1e-6f ? shRadiance : probeRadiance;
}

float3 ResolveFromScreenProbes(uint2 pixelPos, out float resolveWeight, out float3 nearestProbeRadiance)
{
    uint2 probeGridSize = max(uint2(ProbeGridSize), uint2(1u, 1u));
    uint spacing = max(ProbeSpacing, 1u);
    float2 probeSpace = (float2(pixelPos) + 0.5f.xx) / float(spacing) - 0.5f.xx;
    int2 baseProbe = int2(floor(probeSpace));

    float currentDepth = LoadLinearDepth(pixelPos);
    float3 currentPixelNormal = LoadPixelNormal(pixelPos);
    float3 currentGeoNormal = LoadGeoNormal(pixelPos);
    uint radius = clamp(GatherRadius, 1u, 3u);

    float3 sumRadiance = 0.0f.xxx;
    float sumWeight = 0.0f;
    float nearestWeight = -1.0f;
    float fallbackWeight = -1.0f;
    float3 fallbackProbeRadiance = 0.0f.xxx;
    nearestProbeRadiance = 0.0f.xxx;

    [loop]
    for (int gy = -3; gy <= 3; ++gy)
    {
        [loop]
        for (int gx = -3; gx <= 3; ++gx)
        {
            if (abs(gx) > int(radius) || abs(gy) > int(radius))
                continue;

            int2 probeCoordI = baseProbe + int2(gx, gy);
            if (probeCoordI.x < 0 || probeCoordI.y < 0 || probeCoordI.x >= int(probeGridSize.x) || probeCoordI.y >= int(probeGridSize.y))
                continue;

            uint2 probeCoord = uint2(probeCoordI);
            float2 probePixel = min(float2(probeCoord) * float(spacing) + float(spacing) * 0.5f.xx, RTSize - 0.5f.xx);
            float2 pixelDelta = (float2(pixelPos) + 0.5f.xx - probePixel) / max(float(spacing), 1.0f);
            float spatialWeight = exp(-dot(pixelDelta, pixelDelta) * 0.75f);

            float4 probeMeta = LoadProbeMeta(probeCoord);
            float3 probeNormal = SafeNormalize(probeMeta.xyz, currentGeoNormal);
            float probeDepth = probeMeta.w;
            if (probeDepth <= 0.0f)
                continue;

            float3 probeRadiance = EvaluateProbeSH(probeCoord, currentPixelNormal);

            float depthDelta = abs(currentDepth - probeDepth) / max(currentDepth, 1e-3f);
            float endpointGeometryWeight = exp(-depthDelta * ResolveDepthWeight);
            endpointGeometryWeight *= pow(saturate(dot(currentGeoNormal, probeNormal)), ResolveNormalWeight);

            float endpointWeight = spatialWeight * endpointGeometryWeight;
            if (endpointWeight > fallbackWeight)
            {
                fallbackWeight = endpointWeight;
                fallbackProbeRadiance = probeRadiance;
            }

            float edgeWeight = ComputeEdgeStopWeight(pixelPos, currentDepth, currentGeoNormal, probePixel, probeDepth, probeNormal);
            float weight = endpointWeight * edgeWeight;
            sumRadiance += probeRadiance * weight;
            sumWeight += weight;

            if (weight > nearestWeight)
            {
                nearestWeight = weight;
                nearestProbeRadiance = probeRadiance;
            }
        }
    }

    if (nearestWeight <= 1e-7f)
        nearestProbeRadiance = fallbackProbeRadiance;

    resolveWeight = sumWeight;
    return sumRadiance / max(sumWeight, 1e-4f);
}

float3 ApplyTemporalHistory(uint2 pixelPos, float3 currentRadiance, out float currentHistoryFrames)
{
    currentHistoryFrames = 1.0f;
    if (HistoryValid == 0)
        return currentRadiance;

    float2 velocity = VelocityTex[pixelPos].xy;
    float2 prevUV = (float2(pixelPos) + 0.5f.xx - velocity * RTSize) / RTSize;
    if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
        return currentRadiance;

    float currentDepth = DepthTex[pixelPos].x;
    float prevDepth = PrevDepthTex.SampleLevel(BilinearClamp, prevUV, 0).x;
    float currentLinearDepth = GetLinearDepthOpenGL(currentDepth, ProjectionParams.z, ProjectionParams.w);
    float prevLinearDepth = GetLinearDepthOpenGL(prevDepth, ProjectionParams.z, ProjectionParams.w);

    float3 currentNormal = LoadPixelNormal(pixelPos);
    float3 prevNormal = SafeNormalize(PrevNormalTex.SampleLevel(BilinearClamp, prevUV, 0).xyz, currentNormal);

    float depthDelta = abs(currentLinearDepth - prevLinearDepth) / max(currentLinearDepth, 1e-3f);
    float normalDot = saturate(dot(currentNormal, prevNormal));
    float depthWeight = exp(-depthDelta * HistoryDepthWeight);
    float normalWeight = pow(normalDot, min(HistoryNormalWeight, 4.0f));
    float historyWeight = saturate(depthWeight * normalWeight);
    if (depthDelta < 1e-4f && normalDot > 0.999f)
        historyWeight = 1.0f;
    if (historyWeight < MIN_SCREEN_PROBE_HISTORY_CONFIDENCE)
        return currentRadiance;

    float4 prevRadianceAndFrames = PrevScreenProbeGITex.SampleLevel(BilinearClamp, prevUV, 0);
    float3 prevRadiance = max(SanitizeFloat3(prevRadianceAndFrames.xyz), 0.0f.xxx);
    float prevHistoryFrames = clamp(prevRadianceAndFrames.w, 0.0f, MAX_SCREEN_PROBE_HISTORY_FRAMES);
    float acceptedHistoryFrames = min(prevHistoryFrames + 1.0f, MAX_SCREEN_PROBE_HISTORY_FRAMES);
    float alpha = rcp(max(acceptedHistoryFrames, 1.0f));
    currentHistoryFrames = acceptedHistoryFrames;
    return lerp(prevRadiance, currentRadiance, alpha);
}

[numthreads(8, 8, 1)]
void ScreenProbeGI(uint3 DTid : SV_DispatchThreadID)
{
    uint2 textureSize = uint2(RTSize);
    if (DTid.x >= textureSize.x || DTid.y >= textureSize.y)
        return;

    uint2 pixelPos = DTid.xy;
    float resolveWeight = 0.0f;
    float3 nearestProbeRadiance = 0.0f.xxx;
    float3 screenProbeRadiance = ResolveFromScreenProbes(pixelPos, resolveWeight, nearestProbeRadiance);
    float3 stableRadiance = resolveWeight < MinResolveWeight ? nearestProbeRadiance : screenProbeRadiance;
    stableRadiance = lerp(stableRadiance, nearestProbeRadiance, saturate(RawBlend));
    float historyFrames = 1.0f;
    stableRadiance = ApplyTemporalHistory(pixelPos, stableRadiance, historyFrames);

    OutScreenProbeGI[pixelPos] = float4(max(stableRadiance, 0.0f.xxx), historyFrames);
    OutScreenProbeDebug[pixelPos] = float4(max(nearestProbeRadiance, 0.0f.xxx), 1.0f);
    OutScreenProbeHistory[pixelPos] = float4(max(stableRadiance, 0.0f.xxx), historyFrames);
}
