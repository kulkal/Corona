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

CBUFFER_BINDING_BEGIN(ShadowMapConstantBuffer, 0)
{
    float4x4 LightViewProjectionMatrix;
    float4x4 WorldMatrix;
    float4 BaseColorFactor;
    uint SpineVertexBase;
    uint3 Padding;
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
    float2 uv : TEXCOORD0;
};

PSInput BuildShadowVertex(float3 position, float2 uv)
{
    PSInput result;
    float4 worldPosition = mul(float4(position, 1.0f), WorldMatrix);
    result.position = mul(worldPosition, LightViewProjectionMatrix);
    result.uv = uv;
    return result;
}

PSInput VSMain(VSInput input)
{
    return BuildShadowVertex(input.position, input.uv);
}

PSInput SpineVSMain(uint vertexId : SV_VertexID)
{
    SpineSkinnedVertex input = SpineVertices[SpineVertexBase + vertexId];
    return BuildShadowVertex(input.position, input.uv);
}

void PSMain(PSInput input)
{
}
