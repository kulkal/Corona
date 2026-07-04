//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "Common.hlsl"
#include "GGX.hlsli"
#define POINT_LIGHT_GRID_COUNTS_REGISTER t15
#define POINT_LIGHT_GRID_INDICES_REGISTER t16
#include "PointLightGrid.hlsli"

Texture2D AlbedoTex : register(t0);
Texture2D NormalTex : register(t1);
Texture2D ShadowTex : register(t2);
Texture2D VelocityTex : register(t3);
Texture2D DepthTex : register(t4);
Texture2D GIResultSHTex : register(t5);
Texture2D GIResultColorTex : register(t6);
Texture2D SpecularGITex : register(t7);
Texture2D RoughnessMetalicTex : register(t8);
Texture2D AmbientOcclusionTex : register(t14);








SamplerState sampleWrap : register(s0);

// Must match Corona::MaxPointLights in Corona.h. Raised from 8 → 128 to
// support ReSTIR DI demos with many lights.
#define MAX_POINT_LIGHTS 128

struct PointLightParam
{
    float4 PositionAndRadius;
    float4 ColorAndIntensity;
    float4 DirectionAndType;   // xyz = spotlight direction, w = 0 point / 1 spot
    float4 SpotConeAndFlags;   // x innerCos, y outerCos, z invCosDelta, w castShadow
};

cbuffer LightingParam : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 InvProjMatrix;
    float4x4 ShadowViewProjectionMatrix;
    float4 LightDirAndIntensity;
    float2 RTSize;
    float TAABlendFactor;
    float GIBufferScale;
    float3 LightColor;
    float _padding;
    uint bEnableDiffuseGI;
    uint bEnableSpecularGI;
    uint bEnableDirectDiffuse;
    uint bEnableDirectSpecular;
    uint bEnableRTAO;
    float RTAOIndirectStrength;
    float RTAOIndirectFloor;
    float SurfaceBounceStrength;
    float SurfaceBounceSaturation;
    uint LightingOutputMode;
    uint bEnableDirectionalShadow;
    uint bUseShadowMap;
    // 0 = Option A channel-pack (ShadowTex.gba = visibility for first 3
    //     enabled point lights),
    // 1 = deterministic per-pixel finite-light shadows. ShadowTex.gba pack
    //     up to three signed light indices: abs(value)-1 is the light index,
    //     sign is hard visibility.
    uint ShadowMode;
    uint3 _paddingAfterShadowMode;
    float4 AmbientSkyColorAndStrength;
    float4 AmbientGroundColorAndStrength;
    // Option A channel-pack map: 4 light->channel entries per uint4.
    // Read with `ShadowChannelMap[i >> 2][i & 3]`. 0xFFFFFFFF = not
    // shadowed (light wasn't in the in-frustum top-3 closest this frame).
    uint4 ShadowChannelMap[MAX_POINT_LIGHTS / 4];
    PointLightParam PointLights[MAX_POINT_LIGHTS];
    uint PointLightCount;
    float RTAODirectContactStrength; // RTAO contact term strength per lighting lobe
    float UnselectedShadowCasterVisibility;
    float PointLightPadding;
};

struct VSInput
{
    float4 position : POSITION;
    float2 uv : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

PSInput VSMain(
    VSInput input)
{
    PSInput result;

    result.position = input.position;
    result.uv = input.uv;

    return result;
}

float3 SanitizeFloat3(float3 value)
{
    if (any(isnan(value)) || any(isinf(value)))
        return 0.0f.xxx;

    return value;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    value = SanitizeFloat3(value);
    float lengthSq = dot(value, value);
    if (lengthSq < 1e-8f)
        return fallback;

    return value * rsqrt(lengthSq);
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

float4 SanitizeFloat4(float4 value)
{
    if (any(isnan(value)) || any(isinf(value)))
        return 0.0f.xxxx;

    return value;
}

float3 ComputeSurfaceToViewDirection(float2 screenUV)
{
    float2 d = screenUV * 2.0f - 1.0f;
    d.y = -d.y;

    float aspectRatio = RTSize.x / max(RTSize.y, 1.0f);
    d *= tan(0.8f * 0.5f);
    d.x *= aspectRatio;

    float3 viewRay = normalize(float3(d.x, d.y, -1.0f));
    float3 worldRay = normalize(mul(float4(viewRay, 0.0f), InvViewMatrix).xyz);
    return -worldRay;
}

float ComputeSpecularOcclusion(float ao, float roughness, float ndotv)
{
    ao = saturate(ao);
    roughness = clamp(roughness, 0.02f, 1.0f);
    ndotv = saturate(ndotv);

    // Lagarde-style specular occlusion. AO is diffuse/contact visibility, so
    // apply it through a roughness/view dependent approximation instead of
    // multiplying the specular lobe by the diffuse AO scalar directly.
    float exponent = exp2(-16.0f * roughness - 1.0f);
    return saturate(pow(max(ndotv + ao, 1.0e-4f), exponent) - 1.0f + ao);
}

float3 ReconstructWorldPosition(float2 screenUV, float deviceDepth)
{
    float2 screenPosition = screenUV * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;
    float3 viewPosition = GetViewPosition(deviceDepth, screenPosition, InvProjMatrix);
    return mul(float4(viewPosition, 1.0f), InvViewMatrix).xyz;
}

float SampleShadowMapVisibility(float2 shadowUV, float receiverDepth, float3 worldNormal)
{
    uint shadowWidth = 1;
    uint shadowHeight = 1;
    ShadowTex.GetDimensions(shadowWidth, shadowHeight);
    float3 lightDir = SafeNormalize(LightDirAndIntensity.xyz, float3(0.0f, 1.0f, 0.0f));
    float normalFacing = saturate(dot(SafeNormalize(worldNormal, float3(0.0f, 1.0f, 0.0f)), lightDir));
    float depthBias = lerp(0.0060f, 0.0018f, normalFacing);
    uint2 shadowPixel = min(uint2(saturate(shadowUV) * float2(shadowWidth, shadowHeight)), uint2(shadowWidth - 1, shadowHeight - 1));
    float blockerDepth = ShadowTex.Load(int3(shadowPixel, 0)).x;
    return (receiverDepth - depthBias <= blockerDepth) ? 1.0f : 0.0f;
}

float3 EvaluateDirectionalVisibility(float2 screenUV, float deviceDepth, float3 worldNormal)
{
    if (bEnableDirectionalShadow == 0 || deviceDepth >= 0.999999f)
        return 1.0f.xxx;

    if (bUseShadowMap == 0)
    {
        // ShadowTex layout: R = directional sun visibility, GBA = up to 3
        // shadowed point lights (channel-packed Option A). Sun reads R
        // only — older code that splat xyz worked only because the entire
        // buffer was sun visibility.
        float sunVis = saturate(SanitizeFloat3(ShadowTex[uint2(screenUV * RTSize)].xyz).x);
        return sunVis.xxx;
    }

    float3 worldPosition = ReconstructWorldPosition(screenUV, deviceDepth);
    worldPosition += SafeNormalize(worldNormal, float3(0.0f, 1.0f, 0.0f)) * 1.75f;
    float4 shadowClip = mul(float4(worldPosition, 1.0f), ShadowViewProjectionMatrix);
    if (abs(shadowClip.w) <= 1.0e-5f)
        return 1.0f.xxx;

    float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    float2 shadowUV = shadowNdc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (shadowUV.x < 0.0f || shadowUV.x > 1.0f || shadowUV.y < 0.0f || shadowUV.y > 1.0f || shadowNdc.z < 0.0f || shadowNdc.z > 1.0f)
        return 1.0f.xxx;

    return SampleShadowMapVisibility(shadowUV, shadowNdc.z, worldNormal).xxx;
}

float3 EvaluateSimpleSkyAmbient(float3 worldNormal, float3 albedo, float metallic, float deviceDepth)
{
    const bool bMobileDirectOnly = LightingOutputMode == 2;
    if (!bMobileDirectOnly || deviceDepth >= 0.999999f)
        return 0.0f.xxx;

    const float skyStrength = saturate(AmbientSkyColorAndStrength.w);
    const float groundStrength = saturate(AmbientGroundColorAndStrength.w);
    if (skyStrength <= 0.0f && groundStrength <= 0.0f)
        return 0.0f.xxx;

    float3 normal = SafeNormalize(worldNormal, float3(0.0f, 1.0f, 0.0f));
    float upWeight = saturate(normal.y * 0.5f + 0.5f);
    float3 skyColor = max(AmbientSkyColorAndStrength.rgb, 0.0f.xxx);
    float3 groundColor = max(AmbientGroundColorAndStrength.rgb, 0.0f.xxx);
    if (dot(skyColor, 1.0f.xxx) <= 1.0e-5f)
        skyColor = float3(0.68f, 0.74f, 0.88f);
    if (dot(groundColor, 1.0f.xxx) <= 1.0e-5f)
        groundColor = float3(0.070f, 0.065f, 0.060f);

    float3 sky = skyColor * skyStrength;
    float3 ground = groundColor * groundStrength;
    float3 ambient = lerp(ground, sky, upWeight);
    return ambient * albedo * (1.0f - metallic);
}

float3 EvaluateSkyBackground(float2 screenUV)
{
    float vertical = saturate(1.0f - screenUV.y);
    float3 skyTop = max(AmbientSkyColorAndStrength.rgb, 0.0f.xxx);
    float3 skyBottom = max(AmbientGroundColorAndStrength.rgb, 0.0f.xxx);
    if (dot(skyTop, 1.0f.xxx) <= 1.0e-5f)
        skyTop = float3(0.58f, 0.84f, 1.10f);
    if (dot(skyBottom, 1.0f.xxx) <= 1.0e-5f)
        skyBottom = float3(0.88f, 0.98f, 1.12f);
    return lerp(skyBottom, skyTop, vertical) * 1.65f;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float LowFreqWeight = 0.25f;
    float HiFreqWeight = 0.85f;
    float3 clrMin = 99999999.0f;
    float3 clrMax = -99999999.0f;
    float totalWeight = 0.0f;

    input.uv.y = 1 - input.uv.y;
    float2 screenUV = input.uv;
    float2 PixelPos = input.uv * RTSize;


    // LightingOutputMode: 0 = full composite, 1 = direct only, 2 = mobile direct
    // only, 3 = indirect only (GI), 4 = lighting only (white albedo for signal
    // inspection). Direct-only returns are gated to 1/2 so 3/4 fall through.
    bool bDirectOutput = (LightingOutputMode == 1u || LightingOutputMode == 2u);
    bool bMobileDirectOnly = LightingOutputMode == 2;
    float3 Albedo = SanitizeFloat3(AlbedoTex[PixelPos].xyz);
    // Lighting-only: replace albedo with white so the output is the raw lighting
    // signal (no texture). Mode 4 keeps the normal-mapped world normal; mode 5
    // additionally drops the normal map (geometric normal below), isolating the
    // lighting signal from both albedo AND normal-map detail.
    if (LightingOutputMode == 4u || LightingOutputMode == 5u)
        Albedo = 1.0f.xxx;
    float3 NormalSample = SanitizeFloat3(NormalTex[PixelPos].xyz);
    float3 WorldNormal = SafeNormalize(bMobileDirectOnly ? (NormalSample * 2.0f - 1.0f) : NormalSample, float3(0.0f, 1.0f, 0.0f));
    float DeviceDepth = DepthTex[PixelPos].x;
    // Mode 5 "Lighting only (geo normal)": derive a per-pixel geometric normal
    // from the depth gradient (cross of screen-space world-position derivatives),
    // discarding normal-map perturbation. The branch is uniform (cbuffer), so the
    // ddx/ddy are well-defined. Garbage at depth silhouettes is acceptable here.
    if (LightingOutputMode == 5u && DeviceDepth < 0.999999f)
    {
        float3 wp = ReconstructWorldPosition(screenUV, DeviceDepth);
        float3 geoNormal = normalize(cross(ddx(wp), ddy(wp)));
        float3 viewDir = ComputeSurfaceToViewDirection(screenUV);
        if (dot(geoNormal, viewDir) < 0.0f)
            geoNormal = -geoNormal;
        WorldNormal = SafeNormalize(geoNormal, WorldNormal);
    }
    float4 RoughnessMetallic = SanitizeFloat4(RoughnessMetalicTex[PixelPos]);
    float UnlitMaterial = saturate(RoughnessMetallic.z);
    // Spine sprites keep depth writes disabled so attachment draw order stays
    // faithful to the source data. Their unlit GBuffer pixels must survive the
    // sky-depth fallback even when no platform wrote depth behind them.
    if (UnlitMaterial > 0.5f)
        return float4(Albedo, 1.0f);
    if (DeviceDepth >= 0.999999f)
        return float4(EvaluateSkyBackground(screenUV), 1.0f);

    float3 DirectVisibility = EvaluateDirectionalVisibility(screenUV, DeviceDepth, WorldNormal);

    float2 Velocity = VelocityTex[PixelPos];

    float3 LightDir = normalize(LightDirAndIntensity.xyz);
    float LightIntensity = LightDirAndIntensity.w;
    float RawNdotL = dot(LightDir, WorldNormal);
    float NdotL = saturate(RawNdotL);
    float Roughness = clamp(RoughnessMetallic.x, 0.02f, 1.0f);
    float Metallic = saturate(RoughnessMetallic.y);
    float3 F0 = lerp(0.04f.xxx, Albedo.xyz, Metallic);

    // Grass / foliage SSS approximation. Material.w == 1 marks grass
    // blades (set in GBuffer.hlsl on bGrassMesh draws). For these surfaces
    // we add a back-light wrap term so blades lit from behind glow with a
    // warmer translucent green instead of going pitch black. The bGrass
    // flag also already forces two-sided normal flipping at GBuffer time
    // so the front-of-pixel always faces the camera — meaning RawNdotL
    // gives the *primary* light angle and the back-light comes from
    // -LightDir. Wrap diffuse softens the terminator on top of that.
    const bool bGrassSurface = RoughnessMetallic.w > 0.5f;
    float WrappedNdotL = NdotL;
    float3 GrassSSS = float3(0.0f, 0.0f, 0.0f);
    if (bGrassSurface)
    {
        // Wrap term — blade still has nonzero diffuse at NdotL = -0.2.
        const float wrap = 0.5f;
        WrappedNdotL = saturate((RawNdotL + wrap) / (1.0f + wrap));
        // Back-light translucency: when the light is on the far side of
        // the blade, the front receives a soft greenish glow scaled by
        // -RawNdotL (only positive when light is behind). A 0.35 scatter
        // strength keeps it subtle but visible at golden hour.
        const float backFactor = saturate(-RawNdotL) * 0.65f;
        GrassSSS = backFactor * LightIntensity * LightColor * Albedo * DirectVisibility;
    }

    float3 DirectionalDiffuse = bEnableDirectDiffuse ?
        (WrappedNdotL * LightIntensity * LightColor * Albedo * (1.0f - Metallic) * DirectVisibility + GrassSSS) :
        float3(0, 0, 0);

    float3 V = ComputeSurfaceToViewDirection(screenUV);
    float NdotV = saturate(dot(WorldNormal, V));

    float3 SpecularColor = FresnelSchlick(NdotV, F0);
    float3 IndirectDiffuse = 0.0f.xxx;
    float3 IndirectSpecular = 0.0f.xxx;

    // RTAO read once (1.0 = unoccluded when RTAO is off/invalid). Diffuse uses
    // the contact AO directly; specular gets a lobe-aware approximation below.
    float AmbientOcclusion = bEnableRTAO != 0 ? saturate(SanitizeFloat3(AmbientOcclusionTex[PixelPos].xyz).x) : 1.0f;

    if (!bDirectOutput)
    {
        float3 SurfaceBounceGI = max(SanitizeFloat3(GIResultColorTex[PixelPos / GIBufferScale].xyz), 0.0f.xxx);
        float surfaceBounceLuma = dot(SurfaceBounceGI, float3(0.2126f, 0.7152f, 0.0722f));
        SurfaceBounceGI = lerp(surfaceBounceLuma.xxx, SurfaceBounceGI, saturate(SurfaceBounceSaturation));
        float3 SurfaceBounceDiffuse = SurfaceBounceGI * Albedo * (1.0f - Metallic) * saturate(SurfaceBounceStrength);
        IndirectDiffuse = bEnableDiffuseGI ? SurfaceBounceDiffuse : float3(0, 0, 0);
        IndirectSpecular = bEnableSpecularGI ? SanitizeFloat3(SpecularGITex[PixelPos].xyz * SpecularColor) : float3(0, 0, 0);
    }


    float3 DirectionalSpecular = bEnableDirectSpecular ? (EvaluateGGXSpecularBRDF(WorldNormal, V, LightDir, Roughness, F0) * NdotL * LightIntensity * LightColor * DirectVisibility) : float3(0, 0, 0);

    float3 PointDiffuse = 0.0f.xxx;
    float3 PointSpecular = 0.0f.xxx;
    if (PointLightCount > 0 && DeviceDepth < 0.999999f)
    {
        float4 shadowSample = bEnableDirectionalShadow != 0 ?
            saturate(SanitizeFloat4(ShadowTex[uint2(screenUV * RTSize)])) :
            float4(1.0f, 1.0f, 1.0f, 1.0f);

        float3 WorldPosition = ReconstructWorldPosition(screenUV, DeviceDepth);
        uint activePointLightCount = min(PointLightCount, MAX_POINT_LIGHTS);

        if (ShadowMode == 1u)
        {
            // Deterministic finite-light shadows. The shadow pass chooses the
            // strongest local shadow-casting lights for this pixel and packs
            // their visibility into gba. This keeps direct lighting free of
            // stochastic low-frequency noise; unselected local lights remain
            // unshadowed, matching a bounded direct-shadow budget.
            uint2 shadowPixel = uint2(screenUV * RTSize);
            float3 packedShadow = ShadowTex[shadowPixel].gba;
            const bool usePointLightGrid = PointLightGridIsEnabled();
            uint gridCellIndex = 0u;
            uint gridCandidateCount = usePointLightGrid ?
                PointLightGridSelectCandidateCount(WorldPosition, gridCellIndex) :
                activePointLightCount;
            uint pointLightLoopCount = usePointLightGrid ?
                min(gridCandidateCount, PointLightGridGetMaxCount()) :
                activePointLightCount;
            [loop]
            for (uint candidateIndex = 0u; candidateIndex < MAX_POINT_LIGHTS; ++candidateIndex)
            {
                if (candidateIndex >= pointLightLoopCount)
                    break;
                const uint lightIndex = usePointLightGrid ?
                    PointLightGridLoadLightIndex(gridCellIndex, candidateIndex) :
                    candidateIndex;
                if (lightIndex >= activePointLightCount)
                    continue;

                float3 pointPosition = PointLights[lightIndex].PositionAndRadius.xyz;
                float pointRadius = max(PointLights[lightIndex].PositionAndRadius.w, 0.01f);
                float3 pointColor = max(PointLights[lightIndex].ColorAndIntensity.xyz, 0.0f.xxx);
                float pointIntensity = max(PointLights[lightIndex].ColorAndIntensity.w, 0.0f);
                if (pointIntensity <= 0.0f || max(pointColor.x, max(pointColor.y, pointColor.z)) <= 0.0f)
                    continue;

                float3 toLight = pointPosition - WorldPosition;
                float distanceSq = max(dot(toLight, toLight), 1.0e-4f);
                float invDistanceToLight = rsqrt(distanceSq);
                float3 pointLightDir = toLight * invDistanceToLight;
                float attenuation = EvaluatePointLightDistanceAttenuation(distanceSq, pointRadius) *
                    EvaluateSpotAttenuation(PointLights[lightIndex], pointLightDir);
                float pointNdotL = saturate(dot(pointLightDir, WorldNormal));
                if (attenuation <= 0.0f || pointNdotL <= 0.0f)
                    continue;
                float3 pointRadiance = pointColor * pointIntensity * attenuation;
                const bool castsShadow = PointLights[lightIndex].SpotConeAndFlags.w > 0.5f;
                // Shadow-casting lights outside the bounded per-pixel shadow
                // set have no visibility ray. Non-RR keeps the conservative
                // dark fallback; RR uses the unshadowed fallback so unknown
                // local lights do not enter the reconstruction as false
                // shadow regions.
                float visibilityEstimate = castsShadow ? saturate(UnselectedShadowCasterVisibility) : 1.0f;
                if (castsShadow)
                {
                    [unroll]
                    for (uint shadowChannel = 0u; shadowChannel < 3u; ++shadowChannel)
                    {
                        const float packed = packedShadow[shadowChannel];
                        if (abs(packed) < 0.5f)
                            continue;
                        const uint packedIndex = (uint)(abs(packed) - 1.0f + 0.5f);
                        if (packedIndex == lightIndex)
                        {
                            visibilityEstimate = packed > 0.0f ? 1.0f : 0.0f;
                            break;
                        }
                    }
                }

                if (bEnableDirectDiffuse)
                    PointDiffuse += pointNdotL * pointRadiance * visibilityEstimate * Albedo * (1.0f - Metallic);
                if (bEnableDirectSpecular)
                    PointSpecular += EvaluateGGXSpecularBRDF(WorldNormal, V, pointLightDir, Roughness, F0) * pointNdotL * pointRadiance * visibilityEstimate;
            }
        }
        else
        {
            // Option A: top-3 in-frustum closest lights get hard shadow
            // from .gba; the C++ side fills ShadowChannelMap so we can
            // look up whether *this* lightIndex made the cut (and which
            // channel got its visibility). Lights that weren't selected
            // stay unshadowed.
            float3 PointVis = float3(shadowSample.g, shadowSample.b, shadowSample.a);
            const bool usePointLightGrid = PointLightGridIsEnabled();
            uint gridCellIndex = 0u;
            uint gridCandidateCount = usePointLightGrid ?
                PointLightGridSelectCandidateCount(WorldPosition, gridCellIndex) :
                activePointLightCount;
            uint pointLightLoopCount = usePointLightGrid ?
                min(gridCandidateCount, PointLightGridGetMaxCount()) :
                activePointLightCount;

            [loop]
            for (uint candidateIndex = 0u; candidateIndex < MAX_POINT_LIGHTS; ++candidateIndex)
            {
                if (candidateIndex >= pointLightLoopCount)
                    break;
                const uint lightIndex = usePointLightGrid ?
                    PointLightGridLoadLightIndex(gridCellIndex, candidateIndex) :
                    candidateIndex;
                if (lightIndex >= activePointLightCount)
                    continue;

                float3 pointPosition = PointLights[lightIndex].PositionAndRadius.xyz;
                float pointRadius = max(PointLights[lightIndex].PositionAndRadius.w, 0.01f);
                float3 pointColor = max(PointLights[lightIndex].ColorAndIntensity.xyz, 0.0f.xxx);
                float pointIntensity = max(PointLights[lightIndex].ColorAndIntensity.w, 0.0f);
                if (pointIntensity <= 0.0f || max(pointColor.x, max(pointColor.y, pointColor.z)) <= 0.0f)
                    continue;

                float3 toLight = pointPosition - WorldPosition;
                float distanceSq = max(dot(toLight, toLight), 1.0e-4f);
                float invDistanceToLight = rsqrt(distanceSq);
                float3 pointLightDir = toLight * invDistanceToLight;
                float attenuation = EvaluatePointLightDistanceAttenuation(distanceSq, pointRadius) *
                    EvaluateSpotAttenuation(PointLights[lightIndex], pointLightDir);
                float pointNdotL = saturate(dot(pointLightDir, WorldNormal));
                if (attenuation <= 0.0f || pointNdotL <= 0.0f)
                    continue;
                uint shadowChan = ShadowChannelMap[lightIndex >> 2u][lightIndex & 3u];
                float pointVisibility = shadowChan < 3u ? PointVis[shadowChan] : 1.0f;
                float3 pointRadiance = pointColor * pointIntensity * attenuation * pointVisibility;

                if (bEnableDirectDiffuse)
                    PointDiffuse += pointNdotL * pointRadiance * Albedo * (1.0f - Metallic);
                if (bEnableDirectSpecular)
                    PointSpecular += EvaluateGGXSpecularBRDF(WorldNormal, V, pointLightDir, Roughness, F0) * pointNdotL * pointRadiance;
            }
        }
    }

    float3 SimpleSkyAmbient = bMobileDirectOnly ? EvaluateSimpleSkyAmbient(WorldNormal, Albedo, Metallic, DeviceDepth) : 0.0f.xxx;
    float3 DiffuseLighting = max(DirectionalDiffuse + PointDiffuse + SimpleSkyAmbient, 0);
    float3 DirectSpecular = max(DirectionalSpecular + PointSpecular, 0);

    const float AOVisibility = lerp(1.0f, max(AmbientOcclusion, saturate(RTAOIndirectFloor)), saturate(RTAOIndirectStrength));
    const float ContactStrength = saturate(RTAODirectContactStrength);
    const float DiffuseContactAO = lerp(1.0f, AOVisibility, ContactStrength);
    const float SpecularContactRaw = ComputeSpecularOcclusion(AOVisibility, Roughness, NdotV);
    const float SpecularContactStrength = ContactStrength * saturate(0.20f + 0.80f * Roughness);
    const float SpecularContactAO = lerp(1.0f, SpecularContactRaw, SpecularContactStrength);

    float3 OccludedDiffuseLighting = DiffuseLighting * DiffuseContactAO;
    float3 OccludedDirectSpecular = DirectSpecular * SpecularContactAO;
    float3 DirectLighting = max(OccludedDiffuseLighting + OccludedDirectSpecular, 0);
    if (bDirectOutput)
        return float4(SanitizeFloat3(DirectLighting), 1);

    float3 OccludedIndirectDiffuse = IndirectDiffuse * DiffuseContactAO;
    float3 OccludedIndirectSpecular = IndirectSpecular * SpecularContactAO;
    float3 TotalSpecular = max(OccludedDirectSpecular + OccludedIndirectSpecular, 0);

    // Indirect-only: just the GI (diffuse + specular indirect), no direct light.
    if (LightingOutputMode == 3u)
        return float4(SanitizeFloat3(max(OccludedIndirectDiffuse + OccludedIndirectSpecular, 0)), 1);

    float3 FinalColor = OccludedDiffuseLighting + TotalSpecular + OccludedIndirectDiffuse;
    return float4(SanitizeFloat3(FinalColor), 1);
}
