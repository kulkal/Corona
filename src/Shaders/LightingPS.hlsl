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
Texture2D DepthTex : register(t5);




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
    input.uv.y = 1 - input.uv.y;
    float2 PixelPos = input.uv * RTSize;
    float3 Albedo = AlbedoTex[PixelPos];//.Sample(sampleWrap, input.uv);
    float3 WorldNormal = NormalTex[PixelPos];//.Sample(sampleWrap, input.uv);
    float3 Shadow = ShadowTex[PixelPos];//.Sample(sampleWrap, input.uv);

    float2 Velocity = VelocityTex[PixelPos];//.Sample(sampleWrap, input.uv).xy;

	
    float3 DiffuseLighting = dot(LightDir.xyz, WorldNormal)* Albedo * Shadow;


    float4 CurrentColor = float4(DiffuseLighting, 1);
    float2 PrevPixelPos = PixelPos - Velocity * RTSize;
    float4 PrevColor = PrevColorTex[PrevPixelPos];

    float Depth = DepthTex[PixelPos];
    float PrevDepth = DepthTex[PrevPixelPos];

    float BlendFactor = TAABlendFactor;

    if(PrevDepth < Depth || PrevPixelPos.x > RTSize.x || PrevPixelPos.y > RTSize.y || PrevPixelPos.x < 0 || PrevPixelPos.y < 0)
        BlendFactor = 1.0;

    float4 Color = PrevColor *(1-BlendFactor) + CurrentColor * BlendFactor;

    // float depth = DepthTex[PixelPos];
    // if(depth > 0.5)
    //     Color = float4(1, 0, 0, 0);
    // else if(depth > 0)
    //     Color = float4(0, 1, 0, 0);
    // else if(depth > -0.5)
    //     Color = float4(0, 0, 1, 0);

    return Color;
}