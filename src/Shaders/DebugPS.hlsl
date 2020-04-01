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

Texture2D SrcTex: register(t0);
Texture2D SrcTexSH: register(t1);
Texture2D SrcTexNormal: register(t2);

SamplerState sampleWrap : register(s0);
cbuffer DebugPassCB : register(b0)
{
    float4 Scale;
    float4 Offset;
    float4 ProjectionParams;   
    float2 RTSize;
    float GIBufferScale;
    uint DebugMode;
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

    float4 pos = input.position;

    pos.xy = pos.xy * Scale + Offset.xy;
    result.position = pos;

    result.uv = input.uv;

    return result;
}


float4 PSMain(PSInput input) : SV_TARGET
{
    // input.uv.y *= -1;

    input.uv.y = 1 - input.uv.y;
    float2 PixelPos = input.uv * RTSize;
	float4 SrcColor;
	
    if(DebugMode == 0)
        SrcColor = SrcTex.Sample(sampleWrap, input.uv);
    else if(DebugMode == 1)
    {
        float v = SrcTex.Sample(sampleWrap, input.uv).x;
        SrcColor = float4(v, v, v, v);
    }
    else if(DebugMode == 2)
    {
        float v = SrcTex.Sample(sampleWrap, input.uv).y;
        SrcColor = float4(v, v, v, v);
    }
    else if(DebugMode == 3)
    {
        float v = SrcTex.Sample(sampleWrap, input.uv).z;
        SrcColor = float4(v, v, v, v);
    }
    else if(DebugMode == 4)
    {
        float v = SrcTex.Sample(sampleWrap, input.uv).w;
        SrcColor = float4(v, v, v, v);
    }
    else if(DebugMode == 5)
    {
        SH sh_indirect;
        sh_indirect.shY = SrcTexSH[PixelPos/GIBufferScale];
        sh_indirect.CoCg = SrcTex[PixelPos/GIBufferScale].xy;

        float3 WorldNormal = SrcTexNormal[PixelPos];

        float3 IndirectDiffuse = project_SH_irradiance(sh_indirect, WorldNormal);
        SrcColor = float4(IndirectDiffuse, 0);
    }
    else if(DebugMode == 6) // DEPTH
    {
        float DeviceDepth = SrcTex.Sample(sampleWrap, input.uv).x;
        float LinearDepth = GetLinearDepthOpenGL(DeviceDepth, ProjectionParams.z, ProjectionParams.w) ;
        SrcColor = float4(LinearDepth, 0, 0, 0)/ProjectionParams.w;
    }

    return SrcColor;
}