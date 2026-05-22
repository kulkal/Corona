//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
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
    uint Padding;
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
    SpineSkinnedVertex input = SpineVertices[vertexId];
    return BuildGBufferVertex(input.position, input.normal, input.uv, input.tangent);
}

float3 SafeNormalizeMobile(float3 value, float3 fallback)
{
    if (any(isnan(value)) || any(isinf(value)))
        return fallback;

    float lengthSq = dot(value, value);
    if (lengthSq < 1e-8f)
        return fallback;

    return value * rsqrt(lengthSq);
}

struct PS_OUTPUT
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float2 Velocity : SV_Target2;
    float4 Material : SV_Target3;
};

PS_OUTPUT PSMain(PSInput input)
{
    float2 prevPositionSS = (input.prevPosition.xy / input.prevPosition.w) * float2(0.5, -0.5) + 0.5;
    prevPositionSS *= RTSize.xy;

    float2 positionSS = (input.unjitteredPosition.xy / input.unjitteredPosition.w) * float2(0.5, -0.5) + 0.5;
    positionSS *= RTSize.xy;
    float2 velocity = (positionSS - prevPositionSS) / RTSize.xy;

    float4 Albedo = AlbedoTex.Sample(sampleWrap, input.uv) * BaseColorFactor;

    if (Albedo.w < 0.1f)
        discard;

    float3 WorldNormal = SafeNormalizeMobile(input.normal, float3(0.0f, 1.0f, 0.0f));
    if (bTwoSidedLighting != 0)
    {
        float3 surfaceToView = SafeNormalizeMobile(-ViewDir.xyz, WorldNormal);
        if (dot(WorldNormal, surfaceToView) < 0.0f)
            WorldNormal = -WorldNormal;
    }

    PS_OUTPUT output;
    output.Albedo = float4(Albedo.xyz, 1.0f);
    output.Normal = float4(WorldNormal * 0.5f + 0.5f, 1.0f);
    output.Velocity = velocity;

    if (bOverrideRougnessMetallic)
    {
        output.Material.x = clamp(RougnessMetalic.x, 0.02f, 1.0f);
        output.Material.y = saturate(RougnessMetalic.y);
    }
    else
    {
        output.Material.x = clamp(RougnessMetalic.x, 0.02f, 1.0f);
        output.Material.y = 0.0f;
    }
    output.Material.z = bUnlitMaterial != 0 ? 1.0f : 0.0f;
    output.Material.w = 0.0f;

    return output;
}
