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

float4 PSMain(PSInput input) : SV_TARGET
{
    float LowFreqWeight = 0.25f;
    float HiFreqWeight = 0.85f;
    float3 clrMin = 99999999.0f;
    float3 clrMax = -99999999.0f;
    float totalWeight = 0.0f;

    input.uv.y = 1 - input.uv.y;
    float2 PixelPos = input.uv * RTSize;


    float3 Albedo = SanitizeFloat3(AlbedoTex[PixelPos].xyz);
    float3 WorldNormal = normalize(SanitizeFloat3(NormalTex[PixelPos].xyz));
    float3 Shadow = saturate(SanitizeFloat3(ShadowTex[PixelPos].xyz));

    float2 Velocity = VelocityTex[PixelPos];

    float3 LightDir = normalize(LightDirAndIntensity.xyz);
    float LightIntensity = LightDirAndIntensity.w;
	
    float3 DiffuseLighting = bEnableDirectDiffuse ? (saturate(dot(LightDir, WorldNormal)) * LightIntensity * LightColor * Albedo * Shadow) : float3(0, 0, 0);

    float3 IndirectDiffuse = bEnableDiffuseGI ? SanitizeFloat3(GIResultColorTex[PixelPos / GIBufferScale].xyz * Albedo) : float3(0, 0, 0);

    float3 V = mul(InvViewMatrix, float3(0, 0, 1));
    float NdotV = clamp(dot(WorldNormal, -V), 0, 1);

    float Rougness = RoughnessMetalicTex[PixelPos].x; 

    float Metalic = RoughnessMetalicTex[PixelPos].y;
    // use 0.05 if is non-metal
    float Specular = lerp(0.05, 1.0, Metalic); 
    Specular = clamp(schlick_ross_fresnel(Specular, Rougness, NdotV), 0, 1);

    // non-metal doesnt have specular color
    float3 SpecularColor = lerp(1..xxxx, Albedo.xyz, Metalic) * Specular;
    float3 IndirectSpecular;


    IndirectSpecular = bEnableSpecularGI ? SanitizeFloat3(SpecularGITex[PixelPos].xyz * SpecularColor) : float3(0, 0, 0);


    float3 DirectSpecular = bEnableDirectSpecular ? (SpecularColor * GGX(V, LightDir, WorldNormal, Rougness, 0.0) * LightIntensity * LightColor * Shadow) : float3(0, 0, 0);

    DiffuseLighting = max(DiffuseLighting , 0);

    float3 TotalSpecular = max(DirectSpecular + IndirectSpecular, 0);

    float3 FinalColor = DiffuseLighting * (1-Specular) + TotalSpecular + IndirectDiffuse * (1-Specular);
    return float4(SanitizeFloat3(FinalColor), 1);
}
