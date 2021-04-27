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


Texture2D AlbedoTex : register(t0);
Texture2D NormalTex : register(t1);
Texture2D ShadowTex : register(t2);
Texture2D VelocityTex : register(t3);
Texture2D DepthTex : register(t4);
Texture2D GIResultSHTex : register(t5);
Texture2D GIResultColorTex : register(t6);
Texture2D SpecularGITex : register(t7);
Texture2D RoughnessMetalicTex : register(t8);
Texture2D DiffuseGITex: register(t9);




Texture2D DDGIProbeIrradianceSRV: register(t10);
Texture2D DDGIProbeDistanceSRV: register(t11);

RWTexture2D<uint> DDGIProbeStates : register(u0);
RWTexture2D<float4> DDGIProbeOffsets : register(u1);





SamplerState sampleWrap : register(s0);
SamplerState TrilinearSampler : register(s1);


cbuffer LightingParam : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 InvProjMatrix;
    float4 LightDirAndIntensity;
    float4 CameraPosition;
    float2 RTSize;
    float GIBufferScale;
    uint DiffuseGIMode;
	uint UseNRD;
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


    float3 Albedo = AlbedoTex[PixelPos];
    float3 WorldNormal = NormalTex[PixelPos];
    float3 Shadow = ShadowTex[PixelPos];

    float2 Velocity = VelocityTex[PixelPos];

    float3 LightDir = LightDirAndIntensity.xyz;
    float LightIntensity = LightDirAndIntensity.w;
	
    float3 DiffuseLighting = dot(LightDir.xyz, WorldNormal) * LightIntensity * Albedo * Shadow;

    float2 ScreenUV = input.uv;
    SH sh_indirect;
    sh_indirect.shY = GIResultSHTex[PixelPos/GIBufferScale];
    sh_indirect.CoCg = GIResultColorTex[PixelPos/GIBufferScale].xy;

    float3 IndirectDiffuse = 0..xxx;// = project_SH_irradiance(sh_indirect, WorldNormal) * Albedo;

    if(DiffuseGIMode == 0)
	{
		if (UseNRD)
			IndirectDiffuse = DiffuseGITex[PixelPos] * Albedo;
		else
			IndirectDiffuse = project_SH_irradiance(sh_indirect, WorldNormal) * Albedo;
	}
    else
    {
			float2 ScreenPosition = input.uv * 2 - 1;
			ScreenPosition.y = -ScreenPosition.y;

			float DeviceDepth = DepthTex[PixelPos].x; 
			float3 ViewPosition = GetViewPosition(DeviceDepth, ScreenPosition, InvProjMatrix);
			float3 WorldPos = mul(float4(ViewPosition, 1), InvViewMatrix).xyz;
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

			if (DeviceDepth == 1)
				IndirectDiffuse = float4(0, 0, 0, 0);
			else
				IndirectDiffuse = float4(irradiance, 0) * Albedo;
		}

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

		if (UseNRD)
			IndirectSpecular = SpecularGITex[PixelPos].xyz * SpecularColor;
		else
			IndirectSpecular = SpecularGITex[PixelPos].xyz * SpecularColor;


		float3 DirectSpecular = SpecularColor * GGX(V, normalize(LightDir), WorldNormal, Rougness, 0.0) * LightIntensity * Shadow;

		DiffuseLighting = max(DiffuseLighting, 0);

		DirectSpecular = max(DirectSpecular + IndirectSpecular, 0);

		return float4(DiffuseLighting * (1 - Specular) + DirectSpecular + IndirectDiffuse * (1 - Specular) + IndirectSpecular, 1);
	}