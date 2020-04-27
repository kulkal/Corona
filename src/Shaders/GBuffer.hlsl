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
Texture2D RoughnessTex : register(t2);
Texture2D MetallicTex : register(t3);



SamplerState sampleWrap : register(s0);

cbuffer GBufferConstantBuffer : register(b0)
{
    float4x4 ViewProjectionMatrix;
    float4x4 PrevViewProjectionMatrix;  
    float4x4 WorldMatrix;
    float4x4 UnjitteredViewProjMat;
    float4x4 PrevUnjitteredViewProjMat;
    float4 ViewDir;
    float2 RTSize;
    float2 RougnessMetalic;
    uint bOverrideRougnessMetallic;
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
    float4 prevPosition : PREVPOSITION;
    float4 unjitteredPosition : UnjitteredPOSITION;
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

    result.unjitteredPosition = mul(worldPos, UnjitteredViewProjMat);

    result.prevPosition = mul(worldPos, PrevUnjitteredViewProjMat);

	result.normal = normalize(mul(float4(input.normal, 0), WorldMatrix));
    result.tangent = normalize(mul(float4(input.tangent, 0), WorldMatrix));
    result.uv = input.uv;
	
    return result;
}



float3 CalcPerPixelNormal(float2 vTexcoord, float3 vVertNormal, float3 vVertTangent)
{
    vVertNormal = normalize(vVertNormal);
    vVertTangent = normalize(vVertTangent);

    float3 vVertBinormal = normalize(cross(vVertTangent, vVertNormal));
    float3x3 TBN = (float3x3(vVertTangent, vVertBinormal, vVertNormal));

	// Compute per-pixel normal.
    float3 vBumpNormal = (float3) NormalTex.Sample(sampleWrap, vTexcoord);

    vBumpNormal = 2.0f * vBumpNormal - 1.0f;

    return mul(vBumpNormal, TBN);
    //return vVertNormal;
}

struct PS_OUTPUT
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 GeomNormal : SV_Target2;
    float4 Velocity : SV_Target3;
    float4 Material : SV_Target4;
    float UnjitteredDepth : SV_Target5;
};


PS_OUTPUT PSMain(PSInput input) : SV_TARGET
{
    float2 prevPositionSS = (input.prevPosition.xy/input.prevPosition.w) * float2(0.5, -0.5) + 0.5;
    prevPositionSS *= RTSize.xy;

    float prevDepth = input.prevPosition.z/input.prevPosition.w * 0.5 + 0.5;

    float2 positionSS = (input.unjitteredPosition.xy/input.unjitteredPosition.w) * float2(0.5, -0.5) + 0.5;
    positionSS *= RTSize.xy;
    
    float curDepth = input.unjitteredPosition.z/input.unjitteredPosition.w * 0.5 + 0.5;
    float3 velocity;
    velocity.xy = positionSS - prevPositionSS;
    velocity.z = curDepth - prevDepth;


    velocity.xy /= RTSize.xy;

    float4 Albedo = AlbedoTex.Sample(sampleWrap, input.uv);
    float Roughness = RoughnessTex.Sample(sampleWrap, input.uv).x;
    float Metallic = MetallicTex.Sample(sampleWrap, input.uv).x;

    if(Albedo.w < 0.1)
        discard;

    float3 WorldNormal = CalcPerPixelNormal(input.uv, input.normal, input.tangent);
	
    PS_OUTPUT output;
    output.Albedo.xyz = Albedo.xyz;
    output.Normal.xyz = WorldNormal;
    output.GeomNormal.xyz = input.normal;
    output.Velocity.xyz = velocity;
    output.UnjitteredDepth = input.unjitteredPosition.z/input.unjitteredPosition.w;

    if(bOverrideRougnessMetallic)
    {
        output.Material.x = RougnessMetalic.x;
        output.Material.y = RougnessMetalic.y;
    }
    else
    {
        output.Material.x = max(Roughness, 0.01) * RougnessMetalic.x;
        output.Material.y = max(Metallic, 0.01) * RougnessMetalic.y;
    }

    return output;
}