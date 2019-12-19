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


    // float3 CurrentColor = DiffuseLighting;
    // float2 PrevPixelPos = PixelPos - Velocity * RTSize;
    // float3 PrevColor = PrevColorTex[PrevPixelPos].xyz;
    // // PrevColor = clamp(PrevColor, clrMin, clrMax);

    // float Depth = DepthTex[PixelPos];
    // float PrevDepth = DepthTex[PrevPixelPos];

    // float BlendFactor = TAABlendFactor;


  


    //     // return float4(CurrentColor, 1);
    
    // float3 weightA = saturate(1.0f - BlendFactor);
    // float3 weightB = saturate(BlendFactor);

    // float3 temporalWeight = saturate(abs(clrMax - clrMin) / CurrentColor);
    // weightB = saturate(lerp(LowFreqWeight, HiFreqWeight, temporalWeight));
    // weightA = 1.0f - weightB;


    //  // if( PrevDepth < Depth || PrevPixelPos.x > RTSize.x || PrevPixelPos.y > RTSize.y || PrevPixelPos.x < 0 || PrevPixelPos.y < 0)
    //  if(  PrevPixelPos.x > RTSize.x || PrevPixelPos.y > RTSize.y || PrevPixelPos.x < 0 || PrevPixelPos.y < 0 || TAABlendFactor > 0.999f)
    //  {
    //     weightA = 1.0f;
    //     weightB = 0.0f;
    //     // return float4(1, 0, 0, 0);
    //  }

    // float4 Color = float4((CurrentColor * weightA + PrevColor * weightB) / (weightA + weightB), 1);
    

    return float4(DiffuseLighting, 1);
}