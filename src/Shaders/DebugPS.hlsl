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
Texture2D DepthMaskTex: register(t3);

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


float3 LinearTosRGB(in float3 color)
{
    float3 x = color * 12.92f;
    float3 y = 1.055f * pow(saturate(color), 1.0f / 2.4f) - 0.055f;

    float3 clr = color;
    clr.r = color.r < 0.0031308f ? x.r : y.r;
    clr.g = color.g < 0.0031308f ? x.g : y.g;
    clr.b = color.b < 0.0031308f ? x.b : y.b;

    return clr;
}

float3 DepthDebugRamp(float t)
{
    t = saturate(t);
    float3 c0 = float3(0.05f, 0.10f, 0.35f);
    float3 c1 = float3(0.00f, 0.55f, 0.85f);
    float3 c2 = float3(0.15f, 0.85f, 0.25f);
    float3 c3 = float3(0.95f, 0.85f, 0.10f);
    float3 c4 = float3(1.00f, 0.25f, 0.08f);
    if (t < 0.25f)
        return lerp(c0, c1, t * 4.0f);
    if (t < 0.50f)
        return lerp(c1, c2, (t - 0.25f) * 4.0f);
    if (t < 0.75f)
        return lerp(c2, c3, (t - 0.50f) * 4.0f);
    return lerp(c3, c4, (t - 0.75f) * 4.0f);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // input.uv.y *= -1;

    input.uv.y = 1 - input.uv.y;
    uint2 PixelPos = uint2(input.uv * RTSize);
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
        SrcColor = float4(SrcTex[PixelPos].xyz, 0);
    }
    else if(DebugMode == 6) // DEPTH
    {
        float DeviceDepth = SrcTex.Sample(sampleWrap, input.uv).x;
        float LinearDepth = GetLinearDepthOpenGL(DeviceDepth, ProjectionParams.z, ProjectionParams.w) ;
        SrcColor = float4(LinearDepth, 0, 0, 0)/ProjectionParams.w;
    }
    else if(DebugMode == 7) // HISTORY_LENGTH
    {
        float historyFrames = max(SrcTex.Sample(sampleWrap, input.uv).w, 0.0f);
        float v = saturate(log2(historyFrames + 1.0f) / 16.0f);
        SrcColor = float4(v, v, v, 1.0f);
    }
    else if(DebugMode == 8) // RAW_COPY_DEPTH_MASKED
    {
        float deviceDepth = DepthMaskTex[PixelPos].x;
        SrcColor = deviceDepth >= 0.999999f ? 0.0f.xxxx : SrcTex.Sample(sampleWrap, input.uv);
    }
    else if(DebugMode == 9) // DEPTH_QUANTIZED
    {
        float deviceDepth = SrcTex.Sample(sampleWrap, input.uv).x;
        if (deviceDepth >= 0.999999f)
        {
            SrcColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
        }
        else
        {
            float nearZ = max(ProjectionParams.z, 0.001f);
            float farZ = max(ProjectionParams.w, nearZ + 1.0f);
            float linearDepth = max(GetLinearDepthOpenGL(deviceDepth, nearZ, farZ), nearZ);
            float logDepth = saturate(log2(1.0f + linearDepth / nearZ) / log2(1.0f + farZ / nearZ));
            float quantized = floor(logDepth * 64.0f) / 63.0f;
            float bandLine = step(frac(logDepth * 64.0f), 0.08f);
            float3 color = DepthDebugRamp(quantized);
            color = lerp(color, float3(1.0f, 1.0f, 1.0f), bandLine * 0.35f);
            SrcColor = float4(color, 1.0f);
        }
    }

    // return SrcColor;
    return float4(LinearTosRGB(SrcColor.xyz), 0);
}
