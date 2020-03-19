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

Texture2D SrcTex: register(t0);
SamplerState sampleWrap : register(s0);
cbuffer DebugPassCB : register(b0)
{
    float4 Scale;
    float4 Offset;
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
    input.uv.y *= -1;
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


    return SrcColor;
}