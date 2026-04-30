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








SamplerState sampleWrap : register(s0);

cbuffer LightingParam : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
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

float4 PSMain(PSInput input) : SV_TARGET
{
    float LowFreqWeight = 0.25f;
    float HiFreqWeight = 0.85f;
    float3 clrMin = 99999999.0f;
    float3 clrMax = -99999999.0f;
    float totalWeight = 0.0f;

    float2 screenUV = input.uv;
    input.uv.y = 1 - input.uv.y;
    float2 PixelPos = input.uv * RTSize;


    float3 Albedo = SanitizeFloat3(AlbedoTex[PixelPos].xyz);
    float3 WorldNormal = normalize(SanitizeFloat3(NormalTex[PixelPos].xyz));
    float3 Shadow = saturate(SanitizeFloat3(ShadowTex[PixelPos].xyz));

    float2 Velocity = VelocityTex[PixelPos];

    float3 LightDir = normalize(LightDirAndIntensity.xyz);
    float LightIntensity = LightDirAndIntensity.w;
    float NdotL = saturate(dot(LightDir, WorldNormal));
    float4 RoughnessMetallic = SanitizeFloat4(RoughnessMetalicTex[PixelPos]);
    float Roughness = clamp(RoughnessMetallic.x, 0.02f, 1.0f);
    float Metallic = saturate(RoughnessMetallic.y);
    float3 F0 = lerp(0.04f.xxx, Albedo.xyz, Metallic);
	
    float3 DiffuseLighting = bEnableDirectDiffuse ? (NdotL * LightIntensity * LightColor * Albedo * (1.0f - Metallic) * Shadow) : float3(0, 0, 0);

    float3 IndirectDiffuse = bEnableDiffuseGI ? SanitizeFloat3(GIResultColorTex[PixelPos / GIBufferScale].xyz * Albedo * (1.0f - Metallic)) : float3(0, 0, 0);

    float3 V = ComputeSurfaceToViewDirection(screenUV);
    float NdotV = saturate(dot(WorldNormal, V));

    float3 SpecularColor = FresnelSchlick(NdotV, F0);
    float3 IndirectSpecular;


    IndirectSpecular = bEnableSpecularGI ? SanitizeFloat3(SpecularGITex[PixelPos].xyz * SpecularColor) : float3(0, 0, 0);


    float3 DirectSpecular = bEnableDirectSpecular ? (EvaluateGGXSpecularBRDF(WorldNormal, V, LightDir, Roughness, F0) * NdotL * LightIntensity * LightColor * Shadow) : float3(0, 0, 0);

    DiffuseLighting = max(DiffuseLighting , 0);

    float3 TotalSpecular = max(DirectSpecular + IndirectSpecular, 0);

    float3 FinalColor = DiffuseLighting + TotalSpecular + IndirectDiffuse;
    return float4(SanitizeFloat3(FinalColor), 1);
}
