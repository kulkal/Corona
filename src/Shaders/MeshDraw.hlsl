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
    // float4x4 UnjitteredViewProjectionMatrix;
    // float4x4 InvViewProjectionMatrix;
    float4x4 PrevViewProjectionMatrix;  
    float4x4 WorldMatrix;
    float4 ViewDir;
    float2 RTSize;
    float2 JitterOffset;
    float4 pad[2];
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
    // float4 prevPosition : PREVPOSITION;
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

	// float4 positionSS = result.position / result.position.w;
 //    positionSS.xy = (positionSS.xy*float2(0.5, -0.5) + 0.5) * RTSize;

    //result.prevPosition = mul(worldPos, PrevViewProjectionMatrix);
    // result.prevPosition /= result.prevPosition.w;
 //    float2 prevPositionSS = (prevPosition.xy*float2(0.5, -0.5) + 0.5) * RTSize;

    // result.currentPosition = mul(worldPos, ViewProjectionMatrix);
    // result.currentPosition /= result.prevPosition.w;
    

 //    result.velocity.xy = positionSS.xy - prevPositionSS.xy;

    // float4 unjitteredPosition = mul(worldPos, UnjitteredViewProjectionMatrix);
    // unjitteredPosition /= unjitteredPosition.w;
    // result.velocity.zw = float2(unjitteredPosition.z, 0);



	result.normal = mul(input.normal, WorldMatrix);
    result.tangent = mul(input.tangent, WorldMatrix);
    result.uv = input.uv;
	
    return result;
}



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
    // float2 Velocity : SV_Target3;
};


PS_OUTPUT PSMain(PSInput input) : SV_TARGET
{
 //    float4 positionNCD = float4((input.position.xy / RTSize) * 2.0f - 1.0f, 1.0f, 1.0f);
	// positionNCD.y *= -1.0f;
    
	// float4 positionWS = mul(positionNCD, InvViewProjectionMatrix);
 //    positionWS /= positionWS.w;

 //    float4 prevPositionNCD = mul(float4(positionWS.xyz, 1.0f), PrevViewProjectionMatrix);
 //    prevPositionNCD /= prevPositionNCD.w;

 //    float2 prevPositionSS = (prevPositionNCD.xy * float2(0.5f, -0.5f) + 0.5f) * RTSize;
    // float2 prevPositionSS = (input.prevPosition.xy/input.prevPosition.w) * float2(0.5, -0.5) + 0.5;
    // prevPositionSS *= RTSize.xy;
    // float2 velocity = input.position.xy - prevPositionSS;
    // velocity -= JitterOffset;
    // velocity /= RTSize.xy;


    float3 Albedo = diffuseMap.Sample(sampleWrap, input.uv).rgb;

    float3 WorldNormal = CalcPerPixelNormal(input.uv, input.normal, input.tangent);
	
    PS_OUTPUT output;
    output.Albedo.xyz = Albedo;
	//output.Albedo.w = input.velocity.z;// unjittered depth
    output.Normal.xyz = WorldNormal;
    output.GeomNormal.xyz = input.normal;
    //output.Velocity = velocity.xy;

    return output;
}