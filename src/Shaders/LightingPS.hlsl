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

Texture2D AlbedoTex : register(t0);
Texture2D NormalTex : register(t1);
Texture2D ShadowTex : register(t2);
Texture2D PrevColorTex : register(t3);
Texture2D VelocityTex : register(t4);




SamplerState sampleWrap : register(s0);


cbuffer LightingParam : register(b0)
{
    float4 LightDir;
    float2 RTSize;
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
    input.uv.y = 1 - input.uv.y;
    float2 PixelPos = input.uv * RTSize;
    float3 Albedo = AlbedoTex[PixelPos];//.Sample(sampleWrap, input.uv);
    float3 WorldNormal = NormalTex[PixelPos];//.Sample(sampleWrap, input.uv);
    float3 Shadow = ShadowTex[PixelPos];//.Sample(sampleWrap, input.uv);

    float2 Velocity = VelocityTex[PixelPos];//.Sample(sampleWrap, input.uv).xy;

	
    float3 DiffuseLighting = dot(LightDir.xyz, WorldNormal)* Albedo * Shadow;


    float4 CurrentColor = float4(DiffuseLighting, 1);
    float2 PrevColorPos = PixelPos - Velocity;
    float4 PrevColor = PrevColorTex[PrevColorPos];

    float4 Color = PrevColor *0.9 + CurrentColor * 0.1;

    return CurrentColor;
}