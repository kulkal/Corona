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
#include "rtxgi/ddgi/Irradiance.hlsl"

Texture2D SrcTex: register(t0);
Texture2D SrcTexSH: register(t1);
Texture2D SrcTexNormal: register(t2);
Texture2D DDGIProbeIrradianceSRV: register(t3);
Texture2D DDGIProbeDistanceSRV: register(t4);
Texture2D DepthTex: register(t5);

RWTexture2D<uint> DDGIProbeStates : register(u0);
RWTexture2D<float4> DDGIProbeOffsets : register(u1);


SamplerState sampleWrap : register(s0);
SamplerState TrilinearSampler : register(s1);

cbuffer DebugPassCB : register(b0)
{
    float4x4 InvViewMatrix;
    float4x4 InvProjMatrix;
    float4 Scale;
    float4 Offset;
    float4 ProjectionParams;   
    float4 CameraPosition;
    float2 RTSize;
    float GIBufferScale;
    uint DebugMode;
};

ConstantBuffer<DDGIVolumeDescGPU> DDGIVolume    : register(b1);


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

    pos.xy = pos.xy * Scale.xy + Offset.xy;
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

        float3 WorldNormal = SrcTexNormal[PixelPos].xyz;

        float3 IndirectDiffuse = project_SH_irradiance(sh_indirect, WorldNormal);
        SrcColor = float4(IndirectDiffuse, 0);
        // SrcColor = float4(sh_indirect.shY);
    }
    else if(DebugMode == 7)
    {
        float2 ScreenPosition = input.uv * 2 -1;
        ScreenPosition.y = -ScreenPosition.y;

        float DeviceDepth = DepthTex[PixelPos].x;//.SampleLevel(sampleWrap, input.uv, 0).x;
        float3 ViewPosition = GetViewPosition(DeviceDepth, ScreenPosition, InvProjMatrix);
        float3 WorldPos = mul(float4(ViewPosition, 1), InvViewMatrix).xyz;
        float3 WorldNormal = SrcTexNormal[PixelPos].xyz;
        float3 cameraDirection = normalize(ViewPosition - CameraPosition.xyz);
        float3 surfaceBias = DDGIGetSurfaceBias(WorldNormal, cameraDirection, DDGIVolume);

        DDGIVolumeResources resources;
        resources.probeIrradianceSRV = DDGIProbeIrradianceSRV;
        resources.probeDistanceSRV = DDGIProbeDistanceSRV;
        resources.trilinearSampler = TrilinearSampler;
#if RTXGI_DDGI_PROBE_RELOCATION
        resources.probeOffsets = DDGIProbeOffsets;
#endif
#if RTXGI_DDGI_PROBE_STATE_CLASSIFIER
        resources.probeStates = DDGIProbeStates;
#endif

        float3 irradiance = 0.f;

        irradiance = DDGIGetVolumeIrradiance(
            WorldPos.xyz,
            surfaceBias,
            WorldNormal,
            DDGIVolume,
            resources);

        if(DeviceDepth == 1)
            SrcColor = float4(0, 0, 0, 0);
        else
            SrcColor = float4(clamp(irradiance, 0, 1), 0);
    }
    else if(DebugMode == 6) // DEPTH
    {
        float DeviceDepth = SrcTex.Sample(sampleWrap, input.uv).x;
        float LinearDepth = GetLinearDepthOpenGL(DeviceDepth, ProjectionParams.z, ProjectionParams.w) ;
        SrcColor = float4(LinearDepth, 0, 0, 0)/ProjectionParams.w;
    }

    // return SrcColor;
    return float4(LinearTosRGB(SrcColor.xyz), 0);
}