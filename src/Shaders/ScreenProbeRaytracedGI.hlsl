#include "Common.hlsl"
#include "BindlessResources.hlsli"

RWTexture2D<float4> ProbeRadiance : register(u0);
RWTexture2D<float4> ProbeMeta : register(u1);
RWTexture2D<float4> ProbeSH0 : register(u2);
RWTexture2D<float4> ProbeSH1 : register(u3);
RWTexture2D<float4> ProbeSH2 : register(u4);
RWTexture2D<float4> ProbeSH3 : register(u5);
RWTexture2D<float4> ProbeSH4 : register(u6);
RWTexture2D<float4> ProbeSH5 : register(u7);
RWTexture2D<float4> ProbeSH6 : register(u8);
RWTexture2D<float4> ProbeSH7 : register(u9);
RWTexture2D<float4> ProbeSH8 : register(u10);

RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
Texture3D RayNoiseBlueNoiseSource : register(t7);
Texture2D PrevProbeRadianceTex : register(t8);
Texture2D PrevProbeMetaTex : register(t9);
Texture2D VelocityTex : register(t10);
Texture2D PrevDepthTex : register(t11);
Texture2D PrevNormalTex : register(t12);
Texture2D PrevProbeSH0Tex : register(t13);
Texture2D PrevProbeSH1Tex : register(t14);
Texture2D PrevProbeSH2Tex : register(t15);
Texture2D PrevProbeSH3Tex : register(t16);
Texture2D PrevProbeSH4Tex : register(t17);
Texture2D PrevProbeSH5Tex : register(t18);
Texture2D PrevProbeSH6Tex : register(t19);
Texture2D PrevProbeSH7Tex : register(t20);
Texture2D PrevProbeSH8Tex : register(t21);
Texture2D GeoNormalTex : register(t22);

SamplerState sampleWrap : register(s0);
SamplerState historyClamp : register(s1);

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
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4x4 InvProjMatrix;
    float4 ProjectionParams;
    float4 LightDirAndIntensity;
    float2 RandomOffset;
    float2 RTSize;
    float2 ProbeGridSize;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    uint NoiseMode;
    uint ProbeSpacing;
    uint RaysPerProbe;
    uint HistoryValid;
    float ViewSpreadAngle;
    float TemporalAlpha;
    float HistoryDepthWeight;
    float HistoryNormalWeight;
    float3 LightColor;
    float _padding;
    uint LightingBootstrap;
    uint BootstrapRays;
    uint SHCoefficientCount;
    uint _padding3;
    PointLightParam PointLights[RT_DIFFUSE_GI_MAX_POINT_LIGHTS];
    uint PointLightCount;
    float3 PointLightPadding;
};

static const float INV_PI = 1.0f / PI;
static const float MAX_HIT_DIST = 10000.0f;
static const float MAX_PROBE_HISTORY_FRAMES = 65504.0f;
static const float MIN_PROBE_HISTORY_CONFIDENCE = 0.25f;
static const int ADJACENT_ATLAS_FALLBACK_LATERAL_RADIUS = 2;
static const int ADJACENT_ATLAS_FALLBACK_MAX_INWARD_STEPS = 12;
static const float ADJACENT_ATLAS_FALLBACK_MIN_HISTORY_FRAMES = 12.0f;
static const float ADJACENT_ATLAS_FALLBACK_SEED_FRAMES = 8.0f;
static const float PROBE_ANCHOR_DEPTH_WEIGHT = 64.0f;
static const float PROBE_ANCHOR_NORMAL_WEIGHT = 32.0f;
static const float PROBE_ANCHOR_PREV_WEIGHT = 16.0f;
static const float PROBE_ANCHOR_SUPPORT_WEIGHT = 4.0f;

#define SCREEN_PROBE_RAY_FLAGS (RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES)

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
        SCREEN_PROBE_RAY_FLAGS,
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
        SCREEN_PROBE_RAY_FLAGS,
        0xFF,
        0,
        0,
        0,
        ray,
        payload);
#endif
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

struct ProbeAnchor
{
    uint2 pixelPos;
    float2 pixelCenter;
    float deviceDepth;
    float linearDepth;
    float3 worldNormal;
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

SH3RGB InitSH3RGB()
{
    SH3RGB sh;
    sh.c0 = 0.0f.xxx;
    sh.c1 = 0.0f.xxx;
    sh.c2 = 0.0f.xxx;
    sh.c3 = 0.0f.xxx;
    sh.c4 = 0.0f.xxx;
    sh.c5 = 0.0f.xxx;
    sh.c6 = 0.0f.xxx;
    sh.c7 = 0.0f.xxx;
    sh.c8 = 0.0f.xxx;
    return sh;
}

void AccumulateSH3RGB(inout SH3RGB accum, SH3RGB value, float scale)
{
    accum.c0 += value.c0 * scale;
    accum.c1 += value.c1 * scale;
    accum.c2 += value.c2 * scale;
    accum.c3 += value.c3 * scale;
    if (SHCoefficientCount > 4u)
    {
        accum.c4 += value.c4 * scale;
        accum.c5 += value.c5 * scale;
        accum.c6 += value.c6 * scale;
        accum.c7 += value.c7 * scale;
        accum.c8 += value.c8 * scale;
    }
}

SH3RGB ScaleSH3RGB(SH3RGB sh, float scale)
{
    sh.c0 *= scale;
    sh.c1 *= scale;
    sh.c2 *= scale;
    sh.c3 *= scale;
    if (SHCoefficientCount > 4u)
    {
        sh.c4 *= scale;
        sh.c5 *= scale;
        sh.c6 *= scale;
        sh.c7 *= scale;
        sh.c8 *= scale;
    }
    return sh;
}

SH3RGB LerpSH3RGB(SH3RGB a, SH3RGB b, float t)
{
    SH3RGB result;
    result.c0 = lerp(a.c0, b.c0, t);
    result.c1 = lerp(a.c1, b.c1, t);
    result.c2 = lerp(a.c2, b.c2, t);
    result.c3 = lerp(a.c3, b.c3, t);
    result.c4 = 0.0f.xxx;
    result.c5 = 0.0f.xxx;
    result.c6 = 0.0f.xxx;
    result.c7 = 0.0f.xxx;
    result.c8 = 0.0f.xxx;
    if (SHCoefficientCount > 4u)
    {
        result.c4 = lerp(a.c4, b.c4, t);
        result.c5 = lerp(a.c5, b.c5, t);
        result.c6 = lerp(a.c6, b.c6, t);
        result.c7 = lerp(a.c7, b.c7, t);
        result.c8 = lerp(a.c8, b.c8, t);
    }
    return result;
}

SH3RGB ProjectRadianceToSH3RGB(float3 radiance, float3 direction, float sampleWeight)
{
    direction = SanitizeFloat3(direction);
    float dirLenSq = dot(direction, direction);
    direction = dirLenSq < 1e-8f ? float3(0.0f, 1.0f, 0.0f) : direction * rsqrt(dirLenSq);
    radiance = max(SanitizeFloat3(radiance), 0.0f.xxx) * sampleWeight;

    float x = direction.x;
    float y = direction.y;
    float z = direction.z;

    SH3RGB sh;
    sh.c0 = radiance * 0.282095f;
    sh.c1 = radiance * (0.488603f * y);
    sh.c2 = radiance * (0.488603f * z);
    sh.c3 = radiance * (0.488603f * x);
    sh.c4 = 0.0f.xxx;
    sh.c5 = 0.0f.xxx;
    sh.c6 = 0.0f.xxx;
    sh.c7 = 0.0f.xxx;
    sh.c8 = 0.0f.xxx;
    if (SHCoefficientCount > 4u)
    {
        sh.c4 = radiance * (1.092548f * x * y);
        sh.c5 = radiance * (1.092548f * y * z);
        sh.c6 = radiance * (0.315392f * (3.0f * z * z - 1.0f));
        sh.c7 = radiance * (1.092548f * x * z);
        sh.c8 = radiance * (0.546274f * (x * x - y * y));
    }
    return sh;
}

SH3RGB LoadPrevProbeSH(uint2 probeCoord)
{
    SH3RGB sh;
    sh.c0 = SanitizeFloat3(PrevProbeSH0Tex[probeCoord].xyz);
    sh.c1 = SanitizeFloat3(PrevProbeSH1Tex[probeCoord].xyz);
    sh.c2 = SanitizeFloat3(PrevProbeSH2Tex[probeCoord].xyz);
    sh.c3 = SanitizeFloat3(PrevProbeSH3Tex[probeCoord].xyz);
    sh.c4 = 0.0f.xxx;
    sh.c5 = 0.0f.xxx;
    sh.c6 = 0.0f.xxx;
    sh.c7 = 0.0f.xxx;
    sh.c8 = 0.0f.xxx;
    if (SHCoefficientCount > 4u)
    {
        sh.c4 = SanitizeFloat3(PrevProbeSH4Tex[probeCoord].xyz);
        sh.c5 = SanitizeFloat3(PrevProbeSH5Tex[probeCoord].xyz);
        sh.c6 = SanitizeFloat3(PrevProbeSH6Tex[probeCoord].xyz);
        sh.c7 = SanitizeFloat3(PrevProbeSH7Tex[probeCoord].xyz);
        sh.c8 = SanitizeFloat3(PrevProbeSH8Tex[probeCoord].xyz);
    }
    return sh;
}

void StoreProbeSH(uint2 probeCoord, SH3RGB sh)
{
    ProbeSH0[probeCoord] = float4(sh.c0, 0.0f);
    ProbeSH1[probeCoord] = float4(sh.c1, 0.0f);
    ProbeSH2[probeCoord] = float4(sh.c2, 0.0f);
    ProbeSH3[probeCoord] = float4(sh.c3, 0.0f);
    if (SHCoefficientCount > 4u)
    {
        ProbeSH4[probeCoord] = float4(sh.c4, 0.0f);
        ProbeSH5[probeCoord] = float4(sh.c5, 0.0f);
        ProbeSH6[probeCoord] = float4(sh.c6, 0.0f);
        ProbeSH7[probeCoord] = float4(sh.c7, 0.0f);
        ProbeSH8[probeCoord] = float4(sh.c8, 0.0f);
    }
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    value = SanitizeFloat3(value);
    float lenSq = dot(value, value);
    if (lenSq < 1e-8f)
        return fallback;
    return value * rsqrt(lenSq);
}

uint2 ClampProbePixelCoord(int2 pixelPos)
{
    int2 maxPixel = int2(max(RTSize - 1.0f.xx, 0.0f.xx));
    return uint2(clamp(pixelPos, int2(0, 0), maxPixel));
}

float ProbeSurfaceCompatibility(float depthA, float3 normalA, float depthB, float3 normalB, float depthWeight, float normalWeight)
{
    float depthDelta = abs(depthA - depthB) / max(min(depthA, depthB), 1e-3f);
    float depthCompatibility = exp(-depthDelta * depthWeight);
    float normalCompatibility = pow(saturate(dot(normalA, normalB)), normalWeight);
    return saturate(depthCompatibility * normalCompatibility);
}

ProbeAnchor LoadProbeAnchor(uint2 pixelPos)
{
    ProbeAnchor anchor;
    anchor.pixelPos = pixelPos;
    anchor.pixelCenter = float2(pixelPos) + 0.5f.xx;
    anchor.deviceDepth = DepthTex[pixelPos].x;
    anchor.linearDepth = GetLinearDepthOpenGL(anchor.deviceDepth, ProjectionParams.z, ProjectionParams.w);
    anchor.worldNormal = SafeNormalize(GeoNormalTex[pixelPos].xyz, SafeNormalize(WorldNormalTex[pixelPos].xyz, float3(0.0f, 1.0f, 0.0f)));
    return anchor;
}

float ComputeProbeAnchorLocalStability(ProbeAnchor anchor)
{
    float stability = 0.0f;
    float weightSum = 0.0f;

    [loop]
    for (int oy = -2; oy <= 2; ++oy)
    {
        [loop]
        for (int ox = -2; ox <= 2; ++ox)
        {
            if (ox == 0 && oy == 0)
                continue;

            float2 offset = float2(ox, oy);
            float spatialWeight = exp(-dot(offset, offset) * 0.18f);
            uint2 samplePos = ClampProbePixelCoord(int2(anchor.pixelPos) + int2(ox, oy));
            ProbeAnchor sampleAnchor = LoadProbeAnchor(samplePos);
            stability += spatialWeight * ProbeSurfaceCompatibility(
                anchor.linearDepth,
                anchor.worldNormal,
                sampleAnchor.linearDepth,
                sampleAnchor.worldNormal,
                PROBE_ANCHOR_DEPTH_WEIGHT,
                PROBE_ANCHOR_NORMAL_WEIGHT);
            weightSum += spatialWeight;
        }
    }

    return stability / max(weightSum, 1e-4f);
}

ProbeAnchor SelectStableProbeAnchor(uint2 centerPixelPos, uint2 probeCoord)
{
    ProbeAnchor fallbackAnchor = LoadProbeAnchor(centerPixelPos);

    float4 prevProbeMeta = HistoryValid != 0 ? PrevProbeMetaTex[probeCoord] : 0.0f.xxxx;
    const bool prevAnchorValid = HistoryValid != 0 && prevProbeMeta.w > 0.0f;
    float prevDepth = prevProbeMeta.w;
    float3 prevNormal = SafeNormalize(prevProbeMeta.xyz, fallbackAnchor.worldNormal);

    ProbeAnchor bestAnchor = fallbackAnchor;
    float bestScore = -1e20f;

    [loop]
    for (int oy = -2; oy <= 2; ++oy)
    {
        [loop]
        for (int ox = -2; ox <= 2; ++ox)
        {
            uint2 candidatePos = ClampProbePixelCoord(int2(centerPixelPos) + int2(ox, oy));
            ProbeAnchor candidate = LoadProbeAnchor(candidatePos);

            float2 offset = float2(ox, oy);
            float centerPreference = exp(-dot(offset, offset) * 0.18f);
            float localStability = ComputeProbeAnchorLocalStability(candidate);
            float score = localStability * PROBE_ANCHOR_SUPPORT_WEIGHT + centerPreference * 0.15f;

            if (prevAnchorValid)
            {
                float prevCompatibility = ProbeSurfaceCompatibility(
                    candidate.linearDepth,
                    candidate.worldNormal,
                    prevDepth,
                    prevNormal,
                    HistoryDepthWeight,
                    HistoryNormalWeight);
                score += prevCompatibility * PROBE_ANCHOR_PREV_WEIGHT;
            }

            if (score > bestScore)
            {
                bestScore = score;
                bestAnchor = candidate;
            }
        }
    }

    return bestAnchor;
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

bool IsPointLightVisible(float3 worldPos, float3 normal, float3 lightDir, float lightDistance, PointLightParam light)
{
    if (light.SpotConeAndFlags.w <= 0.5f)
        return true;

    RayDesc shadowRay;
    shadowRay.Origin = worldPos + normal * 0.5f;
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

float3 EvaluatePointLightBounce(float3 worldPos, float3 normal, float3 albedo)
{
    float3 radiance = 0.0f.xxx;
    uint activeCount = min(PointLightCount, (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS);
    [loop]
    for (uint lightIndex = 0u; lightIndex < RT_DIFFUSE_GI_MAX_POINT_LIGHTS; ++lightIndex)
    {
        if (lightIndex >= activeCount)
            break;

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

        float3 lightColor = max(SanitizeFloat3(light.ColorAndIntensity.xyz), 0.0f.xxx);
        float lightIntensity = max(CommonSanitizeFloat(light.ColorAndIntensity.w, 0.0f), 0.0f);
        radiance += nDotL * lightColor * lightIntensity * attenuation * max(albedo, 0.0f.xxx) * INV_PI;
    }
    return radiance;
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

float3 ReconstructWorldPosition(float2 pixelCenter, float deviceDepth)
{
    float2 screenPosition = pixelCenter / RTSize;
    screenPosition = screenPosition * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;
    float3 viewPosition = GetViewPosition(deviceDepth, screenPosition, InvProjMatrix);
    return mul(float4(viewPosition, 1.0f), InvViewMatrix).xyz;
}

float3 TraceDiffuseProbeRay(float3 worldPos, float3 worldNormal, uint2 probeCoord, uint sampleIndex, out float3 sampleDirWorld, out float sampleCosTheta, out float samplePdf)
{
    uint2 noiseCoord = probeCoord * uint2(17u, 29u) + uint2(sampleIndex * 11u, sampleIndex * 7u);
    float2 randomUV = GenerateRaySample2D(RayNoiseBlueNoiseSource, noiseCoord, FrameCounter + sampleIndex * 13u, BlueNoiseOffsetStride, NoiseMode);

    // SH represents incoming radiance over directions, so use a uniform
    // hemisphere estimator and apply 1/pdf when projecting into SH.
    float3 sampleDirLocal = SampleUniformHemisphere(randomUV.x, randomUV.y);
    sampleDirWorld = normalize(mul(sampleDirLocal, BuildTBN(worldNormal)));
    sampleCosTheta = saturate(sampleDirLocal.z);
    samplePdf = 1.0f / (2.0f * PI);

    RayDesc ray;
    ray.Origin = worldPos + worldNormal * 0.5f;
    ray.Direction = sampleDirWorld;
    ray.TMin = 0.0f;
    ray.TMax = MAX_HIT_DIST;

    RayPayload payload;
    payload.position = 0.0f.xxx;
    payload.color = 0.0f.xxx;
    payload.normal = worldNormal;
    payload.spreadAngle = ViewSpreadAngle * float(max(ProbeSpacing, 1u));
    payload.coneWidth = 0.0f;
    payload.bHit = false;

    TraceDiffuseGIRay(ray, payload);

    if (!payload.bHit)
        return 0.0f.xxx;

    float3 lightDir = normalize(LightDirAndIntensity.xyz);
    RayDesc shadowRay;
    shadowRay.Origin = payload.position + payload.normal * 0.5f;
    shadowRay.Direction = lightDir;
    shadowRay.TMin = 0.0f;
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

    float3 radiance = 0.0f.xxx;
    if (!shadowPayload.bHit)
    {
        float nDotL = saturate(dot(lightDir, payload.normal));
        radiance += nDotL * LightDirAndIntensity.w * LightColor * payload.color * INV_PI;
    }
    radiance += EvaluatePointLightBounce(payload.position, payload.normal, payload.color);
    return radiance;
}

bool ApplyProbeTemporalHistory(uint2 probeCoord, float2 probePixelCenter, float currentLinearDepth, float3 currentNormal, inout float3 currentRadiance, inout SH3RGB currentSH, inout float currentHistoryFrames)
{
    if (HistoryValid == 0)
        return false;

    float2 currentUV = probePixelCenter / RTSize;
    float2 velocity = VelocityTex.SampleLevel(historyClamp, currentUV, 0).xy;
    float2 prevUV = currentUV - velocity;
    if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
        return false;

    float prevDeviceDepth = PrevDepthTex.SampleLevel(historyClamp, prevUV, 0).x;
    float prevLinearDepth = GetLinearDepthOpenGL(prevDeviceDepth, ProjectionParams.z, ProjectionParams.w);
    float3 prevNormal = SafeNormalize(PrevNormalTex.SampleLevel(historyClamp, prevUV, 0).xyz, currentNormal);

    // Reproject into the previous probe lattice continuously instead of snapping
    // to the nearest probe. This is especially important for camera rotation.
    float screenDepthDelta = abs(currentLinearDepth - prevLinearDepth) / max(currentLinearDepth, 1e-3f);
    float screenDepthWeight = exp(-screenDepthDelta * HistoryDepthWeight);
    float screenNormalWeight = pow(saturate(dot(currentNormal, prevNormal)), min(HistoryNormalWeight, 4.0f));
    float screenHistoryWeight = saturate(screenDepthWeight * screenNormalWeight);

    float spacing = float(max(ProbeSpacing, 1u));
    float2 prevPixelCenter = prevUV * RTSize;
    float2 prevProbeCoordF = (prevPixelCenter - (spacing * 0.5f + 0.5f)) / spacing;
    int2 prevProbeBase = int2(floor(prevProbeCoordF));
    float2 prevProbeFrac = saturate(prevProbeCoordF - floor(prevProbeCoordF));
    uint2 probeGridSize = max(uint2(ProbeGridSize), uint2(1u, 1u));

    float3 prevRadianceSum = 0.0f.xxx;
    SH3RGB prevSHSum = InitSH3RGB();
    float prevHistoryFramesSum = 0.0f;
    float prevWeightSum = 0.0f;
    float bestProbeHistoryWeight = 0.0f;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            int2 probeCoordI = prevProbeBase + int2(x, y);
            if (probeCoordI.x < 0 || probeCoordI.y < 0 || probeCoordI.x >= int(probeGridSize.x) || probeCoordI.y >= int(probeGridSize.y))
                continue;

            float2 bilinear = float2(x == 0 ? 1.0f - prevProbeFrac.x : prevProbeFrac.x, y == 0 ? 1.0f - prevProbeFrac.y : prevProbeFrac.y);
            float bilinearWeight = bilinear.x * bilinear.y;
            if (bilinearWeight <= 0.0f)
                continue;

            uint2 prevProbeCoord = uint2(probeCoordI);
            float4 prevProbeMeta = PrevProbeMetaTex[prevProbeCoord];
            float3 prevProbeNormal = SafeNormalize(prevProbeMeta.xyz, prevNormal);
            float prevProbeDepth = prevProbeMeta.w;
            if (prevProbeDepth <= 0.0f)
                continue;

            float probeDepthDelta = abs(currentLinearDepth - prevProbeDepth) / max(currentLinearDepth, 1e-3f);
            float probeNormalDot = saturate(dot(currentNormal, prevProbeNormal));
            float probeDepthWeight = exp(-probeDepthDelta * HistoryDepthWeight);
            float probeNormalWeight = pow(probeNormalDot, HistoryNormalWeight);
            float probeHistoryWeight = saturate(probeDepthWeight * probeNormalWeight);
            if (probeDepthDelta < 1e-4f && probeNormalDot > 0.999f)
                probeHistoryWeight = 1.0f;
            float historySampleWeight = bilinearWeight * probeHistoryWeight;

            float4 prevProbeRadiance = PrevProbeRadianceTex[prevProbeCoord];
            prevRadianceSum += max(SanitizeFloat3(prevProbeRadiance.xyz), 0.0f.xxx) * historySampleWeight;
            AccumulateSH3RGB(prevSHSum, LoadPrevProbeSH(prevProbeCoord), historySampleWeight);
            prevHistoryFramesSum += clamp(prevProbeRadiance.w, 0.0f, MAX_PROBE_HISTORY_FRAMES) * historySampleWeight;
            prevWeightSum += historySampleWeight;
            bestProbeHistoryWeight = max(bestProbeHistoryWeight, probeHistoryWeight);
        }
    }

    if (prevWeightSum <= 1e-4f)
        return false;

    if (screenDepthDelta < 1e-4f && saturate(dot(currentNormal, prevNormal)) > 0.999f)
        screenHistoryWeight = 1.0f;

    float historyWeight = saturate(screenHistoryWeight * bestProbeHistoryWeight);
    if (historyWeight < MIN_PROBE_HISTORY_CONFIDENCE)
        return false;

    float3 prevRadiance = prevRadianceSum / max(prevWeightSum, 1e-4f);
    SH3RGB prevSH = ScaleSH3RGB(prevSHSum, rcp(max(prevWeightSum, 1e-4f)));
    float prevHistoryFrames = prevHistoryFramesSum / max(prevWeightSum, 1e-4f);
    float acceptedHistoryFrames = min(prevHistoryFrames + 1.0f, MAX_PROBE_HISTORY_FRAMES);
    float alpha = rcp(max(acceptedHistoryFrames, 1.0f));
    currentRadiance = lerp(prevRadiance, currentRadiance, alpha);
    currentSH = LerpSH3RGB(prevSH, currentSH, alpha);
    currentHistoryFrames = acceptedHistoryFrames;
    return true;
}

bool ApplyAdjacentAtlasFallback(float2 probePixelCenter, float3 currentNormal, inout float3 currentRadiance, inout SH3RGB currentSH, inout float currentHistoryFrames)
{
    if (HistoryValid == 0)
        return false;

    float2 currentUV = probePixelCenter / RTSize;
    float2 velocity = VelocityTex.SampleLevel(historyClamp, currentUV, 0).xy;
    float2 prevUV = currentUV - velocity;
    if (all(prevUV >= 0.0f.xx) && all(prevUV <= 1.0f.xx))
        return false;

    uint2 probeGridSize = max(uint2(ProbeGridSize), uint2(1u, 1u));
    float spacing = float(max(ProbeSpacing, 1u));
    float2 edgeUV = clamp(prevUV, 0.5f.xx / RTSize, 1.0f.xx - 0.5f.xx / RTSize);
    float2 edgePixelCenter = edgeUV * RTSize;
    float2 edgeProbeCoordF = (edgePixelCenter - (spacing * 0.5f + 0.5f)) / spacing;
    float2 offscreenPixelDelta = (prevUV - edgeUV) * RTSize;
    float offscreenPixelDistance = length(offscreenPixelDelta);
    float2 inwardProbeDir = offscreenPixelDistance > 1e-3f ? -offscreenPixelDelta / offscreenPixelDistance : 0.0f.xx;
    uint inwardStepCount = min(uint(ceil(offscreenPixelDistance / spacing)) + 1u, uint(ADJACENT_ATLAS_FALLBACK_MAX_INWARD_STEPS));

    uint2 bestProbeCoord = 0u.xx;
    float bestScore = -1.0f;
    float bestHistoryFrames = 0.0f;

    [loop]
    for (uint stepIndex = 0u; stepIndex <= uint(ADJACENT_ATLAS_FALLBACK_MAX_INWARD_STEPS); ++stepIndex)
    {
        if (stepIndex > inwardStepCount)
            continue;

        float2 stepCenter = edgeProbeCoordF + inwardProbeDir * float(stepIndex);
        int2 stepBase = int2(floor(stepCenter + 0.5f.xx));

        [loop]
        for (int y = -ADJACENT_ATLAS_FALLBACK_LATERAL_RADIUS; y <= ADJACENT_ATLAS_FALLBACK_LATERAL_RADIUS; ++y)
        {
            [loop]
            for (int x = -ADJACENT_ATLAS_FALLBACK_LATERAL_RADIUS; x <= ADJACENT_ATLAS_FALLBACK_LATERAL_RADIUS; ++x)
            {
                int2 sampleCoordI = stepBase + int2(x, y);
                if (sampleCoordI.x < 0 || sampleCoordI.y < 0 || sampleCoordI.x >= int(probeGridSize.x) || sampleCoordI.y >= int(probeGridSize.y))
                    continue;

                uint2 sampleCoord = uint2(sampleCoordI);
                float4 prevProbeRadiance = PrevProbeRadianceTex[sampleCoord];
                float prevHistoryFrames = clamp(prevProbeRadiance.w, 0.0f, MAX_PROBE_HISTORY_FRAMES);
                if (prevHistoryFrames < ADJACENT_ATLAS_FALLBACK_MIN_HISTORY_FRAMES)
                    continue;

                float4 prevProbeMeta = PrevProbeMetaTex[sampleCoord];
                if (prevProbeMeta.w <= 0.0f)
                    continue;

                float3 prevNormal = SafeNormalize(prevProbeMeta.xyz, currentNormal);
                float normalWeight = pow(saturate(dot(currentNormal, prevNormal)), 2.0f);
                if (normalWeight <= 1e-4f)
                    continue;

                float2 sampleProbeDelta = float2(sampleCoordI) - stepCenter;
                float distanceWeight = exp(-dot(sampleProbeDelta, sampleProbeDelta) * 0.35f);
                float inwardWeight = exp(-float(stepIndex) * 0.12f);
                float historyWeight = saturate(prevHistoryFrames / 64.0f);
                float score = normalWeight * distanceWeight * inwardWeight * historyWeight;
                if (score > bestScore)
                {
                    bestScore = score;
                    bestProbeCoord = sampleCoord;
                    bestHistoryFrames = prevHistoryFrames;
                }
            }
        }
    }

    if (bestScore <= 1e-4f)
        return false;

    float3 fallbackRadiance = max(SanitizeFloat3(PrevProbeRadianceTex[bestProbeCoord].xyz), 0.0f.xxx);
    SH3RGB fallbackSH = LoadPrevProbeSH(bestProbeCoord);
    float seedFrames = min(min(bestHistoryFrames, ADJACENT_ATLAS_FALLBACK_SEED_FRAMES), MAX_PROBE_HISTORY_FRAMES - 1.0f);
    float acceptedHistoryFrames = seedFrames + 1.0f;
    float alpha = rcp(max(acceptedHistoryFrames, 1.0f));

    currentRadiance = lerp(fallbackRadiance, currentRadiance, alpha);
    currentSH = LerpSH3RGB(fallbackSH, currentSH, alpha);
    currentHistoryFrames = acceptedHistoryFrames;
    return true;
}

[shader("raygeneration")]
void rayGen()
{
    uint2 probeCoord = DispatchRaysIndex().xy;
    uint2 probeGridSize = uint2(ProbeGridSize);
    if (probeCoord.x >= probeGridSize.x || probeCoord.y >= probeGridSize.y)
        return;

    uint spacing = max(ProbeSpacing, 1u);
    uint2 centerPixelPos = min(probeCoord * spacing + spacing / 2u, uint2(RTSize) - 1u);
    ProbeAnchor anchor = SelectStableProbeAnchor(centerPixelPos, probeCoord);
    uint2 pixelPos = anchor.pixelPos;
    float2 pixelCenter = anchor.pixelCenter;
    float deviceDepth = anchor.deviceDepth;
    float linearDepth = anchor.linearDepth;
    float3 worldNormal = anchor.worldNormal;
    float3 worldPos = ReconstructWorldPosition(pixelCenter, deviceDepth);

    float3 radiance = 0.0f.xxx;
    SH3RGB sh = InitSH3RGB();
    bool lightingBootstrap = LightingBootstrap != 0u;

    uint rayCount = lightingBootstrap ? clamp(BootstrapRays, 1u, 128u) : clamp(RaysPerProbe, 1u, 4u);
    [loop]
    for (uint sampleIndex = 0; sampleIndex < rayCount; ++sampleIndex)
    {
        float3 sampleDirWorld = worldNormal;
        float sampleCosTheta = 1.0f;
        float samplePdf = 1.0f;
        float3 sampleRadiance = TraceDiffuseProbeRay(worldPos, worldNormal, probeCoord, sampleIndex, sampleDirWorld, sampleCosTheta, samplePdf);
        float invPdf = rcp(max(samplePdf, 1e-4f));
        radiance += sampleRadiance * sampleCosTheta * invPdf;
        AccumulateSH3RGB(sh, ProjectRadianceToSH3RGB(sampleRadiance, sampleDirWorld, invPdf), 1.0f);
    }
    radiance /= float(rayCount);
    sh = ScaleSH3RGB(sh, rcp(float(rayCount)));

    float historyFrames = lightingBootstrap ? float(rayCount) : 1.0f;
    if (!lightingBootstrap)
    {
        bool temporalHistoryApplied = ApplyProbeTemporalHistory(probeCoord, pixelCenter, linearDepth, worldNormal, radiance, sh, historyFrames);
        if (!temporalHistoryApplied)
            ApplyAdjacentAtlasFallback(pixelCenter, worldNormal, radiance, sh, historyFrames);
    }

    ProbeRadiance[probeCoord] = float4(max(radiance, 0.0f.xxx), historyFrames);
    ProbeMeta[probeCoord] = float4(worldNormal, linearDepth);
    StoreProbeSH(probeCoord, sh);
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

    float3 baseColor = MaterialTextures[NonUniformResourceIndex(material.AlbedoTextureIndex)].SampleLevel(sampleWrap, vertex.uv, mipLevel).xyz * material.BaseColorFactor.xyz;
    payload.color = max(CommonSanitizeFloat3(baseColor, 1.0f.xxx), 0.0f.xxx);

    payload.bHit = true;
}

[shader("miss")]
void missShadow(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}
