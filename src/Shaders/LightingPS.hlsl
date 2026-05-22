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
Texture2D SkyLightingTex : register(t15);








SamplerState sampleWrap : register(s0);

#define MAX_POINT_LIGHTS 8

struct PointLightParam
{
    float4 PositionAndRadius;
    float4 ColorAndIntensity;
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
    uint bEnableSkyLighting;
    float RTAOIndirectStrength;
    float RTAOIndirectFloor;
    float SurfaceBounceStrength;
    float SurfaceBounceSaturation;
    float SkyLightingStrength;
    uint LightingOutputMode;
    uint bEnableDirectionalShadow;
    uint bUseShadowMap;
    uint bEnableSimpleSkyLighting;
    uint LightingPadding1;
    float4 AmbientSkyColorAndStrength;
    float4 AmbientGroundColorAndStrength;
    PointLightParam PointLights[MAX_POINT_LIGHTS];
    uint PointLightCount;
    float3 PointLightPadding;
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
        return saturate(SanitizeFloat3(ShadowTex[uint2(screenUV * RTSize)].xyz));

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
    if ((!bMobileDirectOnly && bEnableSimpleSkyLighting == 0) || deviceDepth >= 0.999999f)
        return 0.0f.xxx;

    const float skyStrength = bMobileDirectOnly ? saturate(AmbientSkyColorAndStrength.w) : saturate(SkyLightingStrength);
    const float groundStrength = bMobileDirectOnly ? saturate(AmbientGroundColorAndStrength.w) : saturate(SkyLightingStrength * 0.35f);
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


    bool bDirectOutput = LightingOutputMode != 0;
    bool bMobileDirectOnly = LightingOutputMode == 2;
    float3 Albedo = SanitizeFloat3(AlbedoTex[PixelPos].xyz);
    float3 NormalSample = SanitizeFloat3(NormalTex[PixelPos].xyz);
    float3 WorldNormal = SafeNormalize(bMobileDirectOnly ? (NormalSample * 2.0f - 1.0f) : NormalSample, float3(0.0f, 1.0f, 0.0f));
    float DeviceDepth = DepthTex[PixelPos].x;
    float3 DirectVisibility = EvaluateDirectionalVisibility(screenUV, DeviceDepth, WorldNormal);

    float2 Velocity = VelocityTex[PixelPos];

    float3 LightDir = normalize(LightDirAndIntensity.xyz);
    float LightIntensity = LightDirAndIntensity.w;
    float NdotL = saturate(dot(LightDir, WorldNormal));
    float4 RoughnessMetallic = SanitizeFloat4(RoughnessMetalicTex[PixelPos]);
    float UnlitMaterial = saturate(RoughnessMetallic.z);
    if (UnlitMaterial > 0.5f && DeviceDepth < 0.999999f)
        return float4(Albedo, 1.0f);

    float Roughness = clamp(RoughnessMetallic.x, 0.02f, 1.0f);
    float Metallic = saturate(RoughnessMetallic.y);
    float3 F0 = lerp(0.04f.xxx, Albedo.xyz, Metallic);
	
    float3 DirectionalDiffuse = bEnableDirectDiffuse ? (NdotL * LightIntensity * LightColor * Albedo * (1.0f - Metallic) * DirectVisibility) : float3(0, 0, 0);

    float3 V = ComputeSurfaceToViewDirection(screenUV);
    float NdotV = saturate(dot(WorldNormal, V));

    float3 SpecularColor = FresnelSchlick(NdotV, F0);
    float3 IndirectDiffuse = 0.0f.xxx;
    float3 IndirectSpecular = 0.0f.xxx;

    if (!bDirectOutput)
    {
        float AmbientOcclusion = bEnableRTAO != 0 ? saturate(SanitizeFloat3(AmbientOcclusionTex[PixelPos].xyz).x) : 1.0f;
        float ContactAO = lerp(1.0f, max(AmbientOcclusion, saturate(RTAOIndirectFloor)), saturate(RTAOIndirectStrength));
        float3 SkyDiffuse = (bEnableSkyLighting != 0) ? SanitizeFloat3(SkyLightingTex[PixelPos].xyz) * Albedo * (1.0f - Metallic) * saturate(SkyLightingStrength) : float3(0, 0, 0);
        float3 SurfaceBounceGI = max(SanitizeFloat3(GIResultColorTex[PixelPos / GIBufferScale].xyz), 0.0f.xxx);
        float surfaceBounceLuma = dot(SurfaceBounceGI, float3(0.2126f, 0.7152f, 0.0722f));
        SurfaceBounceGI = lerp(surfaceBounceLuma.xxx, SurfaceBounceGI, saturate(SurfaceBounceSaturation));
        float3 SurfaceBounceDiffuse = SurfaceBounceGI * Albedo * (1.0f - Metallic) * ContactAO * saturate(SurfaceBounceStrength);
        IndirectDiffuse = (bEnableDiffuseGI ? SurfaceBounceDiffuse : float3(0, 0, 0)) + SkyDiffuse;
        IndirectSpecular = bEnableSpecularGI ? SanitizeFloat3(SpecularGITex[PixelPos].xyz * SpecularColor) : float3(0, 0, 0);
    }


    float3 DirectionalSpecular = bEnableDirectSpecular ? (EvaluateGGXSpecularBRDF(WorldNormal, V, LightDir, Roughness, F0) * NdotL * LightIntensity * LightColor * DirectVisibility) : float3(0, 0, 0);

    float3 PointDiffuse = 0.0f.xxx;
    float3 PointSpecular = 0.0f.xxx;
    if (PointLightCount > 0 && DeviceDepth < 0.999999f)
    {
        float3 WorldPosition = ReconstructWorldPosition(screenUV, DeviceDepth);
        uint activePointLightCount = min(PointLightCount, MAX_POINT_LIGHTS);
        [loop]
        for (uint lightIndex = 0; lightIndex < activePointLightCount; ++lightIndex)
        {
            float3 pointPosition = PointLights[lightIndex].PositionAndRadius.xyz;
            float pointRadius = max(PointLights[lightIndex].PositionAndRadius.w, 0.01f);
            float3 pointColor = max(PointLights[lightIndex].ColorAndIntensity.xyz, 0.0f.xxx);
            float pointIntensity = max(PointLights[lightIndex].ColorAndIntensity.w, 0.0f);

            float3 toLight = pointPosition - WorldPosition;
            float distanceSq = max(dot(toLight, toLight), 1.0e-4f);
            float distanceToLight = sqrt(distanceSq);
            float3 pointLightDir = toLight / distanceToLight;
            float rangeAttenuation = saturate(1.0f - distanceToLight / pointRadius);
            rangeAttenuation *= rangeAttenuation;
            float inverseSquareAttenuation = 1.0f / max(1.0f, distanceSq * 0.0001f);
            float attenuation = rangeAttenuation * inverseSquareAttenuation;
            float pointNdotL = saturate(dot(pointLightDir, WorldNormal));
            float3 pointRadiance = pointColor * pointIntensity * attenuation;

            if (bEnableDirectDiffuse)
                PointDiffuse += pointNdotL * pointRadiance * Albedo * (1.0f - Metallic);
            if (bEnableDirectSpecular)
                PointSpecular += EvaluateGGXSpecularBRDF(WorldNormal, V, pointLightDir, Roughness, F0) * pointNdotL * pointRadiance;
        }
    }

    float3 SimpleSkyAmbient = (bMobileDirectOnly || bEnableSimpleSkyLighting != 0) ? EvaluateSimpleSkyAmbient(WorldNormal, Albedo, Metallic, DeviceDepth) : 0.0f.xxx;
    float3 DiffuseLighting = max(DirectionalDiffuse + PointDiffuse + SimpleSkyAmbient, 0);
    float3 DirectSpecular = max(DirectionalSpecular + PointSpecular, 0);

    float3 DirectLighting = max(DiffuseLighting + DirectSpecular, 0);
    if (bMobileDirectOnly && DeviceDepth >= 0.999999f)
    {
        float vertical = saturate(1.0f - screenUV.y);
        float3 dungeonBackground = lerp(float3(0.010f, 0.011f, 0.013f), float3(0.045f, 0.050f, 0.055f), vertical);
        return float4(dungeonBackground, 1);
    }
    if (bDirectOutput)
        return float4(SanitizeFloat3(DirectLighting), 1);

    float3 TotalSpecular = max(DirectSpecular + IndirectSpecular, 0);

    float3 FinalColor = DiffuseLighting + TotalSpecular + IndirectDiffuse;
    return float4(SanitizeFloat3(FinalColor), 1);
}
