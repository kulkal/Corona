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
// Texture2D IndirectDiffuseTex : register(t5);
Texture2D GIResultSHTex : register(t5);
Texture2D GIResultColorTex : register(t6);





SamplerState sampleWrap : register(s0);


cbuffer LightingParam : register(b0)
{
    float4 LightDir;
    float2 RTSize;
    float TAABlendFactor;
    float pad;
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


float4 PSMain(PSInput input) : SV_TARGET
{
    float LowFreqWeight = 0.25f;
    float HiFreqWeight = 0.85f;
    float3 clrMin = 99999999.0f;
    float3 clrMax = -99999999.0f;
    float totalWeight = 0.0f;

    input.uv.y = 1 - input.uv.y;
    float2 PixelPos = input.uv * RTSize;


    float3 Albedo = AlbedoTex[PixelPos];//.Sample(sampleWrap, input.uv);
    float3 WorldNormal = NormalTex[PixelPos];//.Sample(sampleWrap, input.uv);
    float3 Shadow = ShadowTex[PixelPos];//.Sample(sampleWrap, input.uv);

    float2 Velocity = VelocityTex[PixelPos];//.Sample(sampleWrap, input.uv).xy;

	
    float3 DiffuseLighting = dot(LightDir.xyz, WorldNormal)* Albedo * Shadow;

    SH sh_indirect;
    sh_indirect.shY = GIResultSHTex[PixelPos];
    sh_indirect.CoCg = GIResultColorTex[PixelPos].xy;

    float3 IndirectDiffuse = project_SH_irradiance(sh_indirect, WorldNormal) * Albedo;

    // float3 IndirectDiffuse = IndirectDiffuseTex[PixelPos] * Albedo;


    return float4(DiffuseLighting + IndirectDiffuse, 1);
}