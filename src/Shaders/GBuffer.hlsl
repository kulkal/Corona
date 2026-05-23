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

#include "ShaderResourceBindings.hlsli"
#include "Common.hlsl"

TEXTURE2D_BINDING(AlbedoTex, 0);
TEXTURE2D_BINDING(NormalTex, 1);
TEXTURE2D_BINDING(RoughnessTex, 2);
TEXTURE2D_BINDING(MetallicTex, 3);

SAMPLER_BINDING(sampleWrap, 0);

CBUFFER_BINDING_BEGIN(GBufferConstantBuffer, 0)
{
    float4x4 ViewProjectionMatrix;
    float4x4 PrevViewProjectionMatrix;  
    float4x4 WorldMatrix;
    float4x4 UnjitteredViewProjMat;
    float4x4 PrevUnjitteredViewProjMat;
    float4 ViewDir;
    float4 BaseColorFactor;
    float2 RTSize;
    float2 RougnessMetalic;
    uint bOverrideRougnessMetallic;
    uint bTwoSidedLighting;
    uint bUnlitMaterial;
    uint SpineVertexBase;
} CBUFFER_BINDING_END;

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 tangent : TANGENT;
};

struct SpineSkinnedVertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float3 tangent;
};

StructuredBuffer<SpineSkinnedVertex> SpineVertices : register(t4);

struct PSInput
{
    float4 position : SV_POSITION;
    float4 prevPosition : PREVPOSITION;
    float4 unjitteredPosition : UnjitteredPOSITION;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;     
    float3 tangent : TANGENT;
};

PSInput BuildGBufferVertex(float3 position, float3 normal, float2 uv, float3 tangent)
{
    PSInput result;
	float4 worldPos = mul(float4(position, 1.0f), WorldMatrix);
    result.position = mul(worldPos, ViewProjectionMatrix);

    result.unjitteredPosition = mul(worldPos, UnjitteredViewProjMat);

    result.prevPosition = mul(worldPos, PrevUnjitteredViewProjMat);

	result.normal = normalize(mul(float4(normal, 0), WorldMatrix));
    result.tangent = normalize(mul(float4(tangent, 0), WorldMatrix));
    result.uv = uv;
	
    return result;
}

PSInput VSMain(VSInput input)
{
    return BuildGBufferVertex(input.position, input.normal, input.uv, input.tangent);
}

PSInput SpineVSMain(uint vertexId : SV_VertexID)
{
    SpineSkinnedVertex input = SpineVertices[SpineVertexBase + vertexId];
    return BuildGBufferVertex(input.position, input.normal, input.uv, input.tangent);
}



float3 CalcPerPixelNormal(float2 vTexcoord, float3 vVertNormal, float3 vVertTangent)
{
    float normalLengthSq = dot(vVertNormal, vVertNormal);
    if (normalLengthSq < 1e-8f)
        return float3(0.0f, 1.0f, 0.0f);

    vVertNormal *= rsqrt(normalLengthSq);

    float tangentLengthSq = dot(vVertTangent, vVertTangent);
    if (tangentLengthSq < 1e-8f)
        return vVertNormal;

    vVertTangent *= rsqrt(tangentLengthSq);

    float3 vVertBinormal = cross(vVertTangent, vVertNormal);
    float binormalLengthSq = dot(vVertBinormal, vVertBinormal);
    if (binormalLengthSq < 1e-8f)
        return vVertNormal;
    vVertBinormal *= rsqrt(binormalLengthSq);

    float3x3 TBN = (float3x3(vVertTangent, vVertBinormal, vVertNormal));

    // Compute per-pixel normal. Reconstruct Z from XY so BC5 normal maps work.
    float3 normalSample = (float3) NormalTex.Sample(sampleWrap, vTexcoord);
    float2 normalXY = 2.0f * normalSample.xy - 1.0f;
    float3 vBumpNormal = float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY))));

    float3 worldNormal = mul(vBumpNormal, TBN);
    float worldNormalLengthSq = dot(worldNormal, worldNormal);
    if (worldNormalLengthSq < 1e-8f)
        return vVertNormal;

    return worldNormal * rsqrt(worldNormalLengthSq);
    //return vVertNormal;
}

struct PS_OUTPUT
{
    float4 Albedo : SV_Target0;
    float4 SpecularAlbedo : SV_Target1;
    float4 Normal : SV_Target2;
    float4 GeomNormal : SV_Target3;
    float2 Velocity : SV_Target4;
    float4 Material : SV_Target5;
    float UnjitteredDepth : SV_Target6;
};


PS_OUTPUT PSMain(PSInput input)
{
    float2 prevPositionSS = (input.prevPosition.xy/input.prevPosition.w) * float2(0.5, -0.5) + 0.5;
    prevPositionSS *= RTSize.xy;

    float prevDepth = input.prevPosition.z/input.prevPosition.w * 0.5 + 0.5;

    float2 positionSS = (input.unjitteredPosition.xy/input.unjitteredPosition.w) * float2(0.5, -0.5) + 0.5;
    positionSS *= RTSize.xy;
    
    float curDepth = input.unjitteredPosition.z/input.unjitteredPosition.w * 0.5 + 0.5;
    float2 velocity;
    velocity.xy = positionSS - prevPositionSS;
    // velocity.z = curDepth - prevDepth;


    velocity.xy /= RTSize.xy;

    float4 Albedo = AlbedoTex.Sample(sampleWrap, input.uv) * BaseColorFactor;
    float Roughness = RoughnessTex.Sample(sampleWrap, input.uv).x;
    float Metallic = MetallicTex.Sample(sampleWrap, input.uv).x;

    if(Albedo.w < 0.1)
        discard;

    float3 WorldNormal = CalcPerPixelNormal(input.uv, input.normal, input.tangent);
    float3 GeomNormal = CommonSafeNormalize(input.normal, float3(0.0f, 1.0f, 0.0f));
    if (bTwoSidedLighting != 0)
    {
        float3 surfaceToViewForNormal = CommonSafeNormalize(-ViewDir.xyz, WorldNormal);
        if (dot(WorldNormal, surfaceToViewForNormal) < 0.0f)
        {
            WorldNormal = -WorldNormal;
            GeomNormal = -GeomNormal;
        }
    }
	
    PS_OUTPUT output;
    output.Albedo.xyz = Albedo.xyz;
    output.Normal.xyz = WorldNormal;
    output.GeomNormal.xyz = GeomNormal;
    output.Velocity.xy = velocity;
    output.UnjitteredDepth = input.unjitteredPosition.z/input.unjitteredPosition.w;

    if(bOverrideRougnessMetallic)
    {
        output.Material.x = clamp(RougnessMetalic.x, 0.02f, 1.0f);
        output.Material.y = saturate(RougnessMetalic.y);
    }
    else
    {
        output.Material.x = clamp(Roughness * RougnessMetalic.x, 0.02f, 1.0f);
        output.Material.y = saturate(Metallic * RougnessMetalic.y);
    }
    output.Material.z = bUnlitMaterial != 0 ? 1.0f : 0.0f;
    output.Material.w = 0.0f;

    float3 surfaceToView = CommonSafeNormalize(-ViewDir.xyz, WorldNormal);
    output.SpecularAlbedo.xyz = ComputeDLSSRRSpecularAlbedo(Albedo.xyz, output.Material.y, output.Material.x, WorldNormal, surfaceToView);
    output.SpecularAlbedo.w = 1.0f;

    return output;
}
