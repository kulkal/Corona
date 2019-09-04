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

//Texture2D shadowMap : register(t0);
Texture2D diffuseMap : register(t0);
Texture2D normalMap : register(t1);

SamplerState sampleWrap : register(s0);

//cbuffer ViewParameter : register(b0)
//{
//    float4x4 ViewProjectionMatrix;

//};

cbuffer ObjParameter : register(b0)
{
    float4x4 ViewProjectionMatrix;
    float4x4 WorldMatrix;
    float4 ViewDir;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 tangent : TANGENT;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 worldpos : POSITION;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
};

PSInput VSMain(
    VSInput input)
{
    PSInput result;
	float4 worldPos = mul(float4(input.position, 1.0f), WorldMatrix);

    result.position = mul(worldPos, ViewProjectionMatrix);
	result.worldpos = worldPos;

    float3 CorrectNormal;
 //   if (dot(input.normal, ViewDir.xyz) > 0)
 //       CorrectNormal = input.normal;
	//else
        CorrectNormal = input.normal;

	result.normal = CorrectNormal;
    result.tangent = input.tangent;
    result.uv = input.uv;
	
    return result;
}


//float4 PSMain(PSInput input) : SV_TARGET
//{
//	float4 diffuseColor = diffuseMap.Sample(sampleWrap, input.uv);
//	float3 pixelNormal = CalcPerPixelNormal(input.uv, input.normal, input.tangent);
//	float4 totalLight = ambientColor;

//	for (int i = 0; i < NUM_LIGHTS; i++)
//	{
//		float4 lightPass = CalcLightingColor(lights[i].position, lights[i].direction, lights[i].color, lights[i].falloff, input.worldpos.xyz, pixelNormal);
//		if (sampleShadowMap && i == 0)
//		{
//			lightPass *= CalcUnshadowedAmountPCF2x2(i, input.worldpos);
//		}
//		totalLight += lightPass;
//	}

//	return diffuseColor * saturate(totalLight);
//}

float3 CalcPerPixelNormal(float2 vTexcoord, float3 vVertNormal, float3 vVertTangent)
{
	// Compute tangent frame.
    vVertNormal = normalize(vVertNormal);
    vVertTangent = normalize(vVertTangent);

    float3 vVertBinormal = normalize(cross(vVertTangent, vVertNormal));
    float3x3 mTangentSpaceToWorldSpace = (float3x3(vVertTangent, vVertBinormal, vVertNormal));

	// Compute per-pixel normal.
    float3 vBumpNormal = (float3) normalMap.Sample(sampleWrap, vTexcoord);
    //return vBumpNormal;

    vBumpNormal = 2.0f * vBumpNormal - 1.0f;

    return mul(vBumpNormal, mTangentSpaceToWorldSpace);
    //return vVertNormal;
}


struct PS_OUTPUT
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 GeomNormal : SV_Target2;
};


PS_OUTPUT PSMain(PSInput input) : SV_TARGET
{

    //float3 Albedo = diffuseMap.SampleLevel(sampleWrap, input.uv, 0).rgb;
    float3 Albedo = diffuseMap.Sample(sampleWrap, input.uv).rgb;

    float3 WorldNormal = CalcPerPixelNormal(input.uv, input.normal, input.tangent);
    float3 lightDir = normalize(float3(2, 1, -1)) * 1.2;
    float3 DiffuseLighting = dot(lightDir, WorldNormal);
    //float3 mat = g_txMats[materialConstants.matIndex].Sample(g_sampler, input.uv).rgb;
    //float3 mat = g_txMats[20].Sample(g_sampler, input.uv).rgb;
    //float3 mat = g_txMats[8].Sample(g_sampler, input.uv).rgb;
	
    PS_OUTPUT output;
    output.Albedo.xyz = Albedo;
    output.Normal.xyz = WorldNormal;
    output.GeomNormal.xyz = input.normal;

    return output;
}