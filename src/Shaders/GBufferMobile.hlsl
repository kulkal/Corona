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
    uint SpineVertexBase;
    uint SkeletalCharIndex;
    uint SkeletalVertsPerChar;
    uint SkeletalBoneCount;
    uint _SkeletalPad;
} CBUFFER_BINDING_END;

struct SkinInputVertex_t
{
    float3 BindPosition;
    float  Pad0;
    float3 BindNormal;
    float  Pad1;
    float3 BindTangent;
    float  Pad2;
    float2 UV;
    uint   BoneIndicesPacked;
    float  PadEnd;
    float4 BoneWeights;
};
struct SkinBone_t
{
    float4 Row0;
    float4 Row1;
    float4 Row2;
};

StructuredBuffer<SkinInputVertex_t> SkeletalInputs    : register(t5);
StructuredBuffer<SkinBone_t>        SkeletalPrevBones : register(t6);
StructuredBuffer<SkinBone_t>        SkeletalCurrBones : register(t7);

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

// Skeletal motion-vector path. IA reads either the compute-skinned VB
// or the CPU-skinned VB; the VS re-skins from the bind pose with the
// previous-frame palette to derive a per-vertex prev clip position.
PSInput SkeletalVSMain(VSInput input, uint vertexId : SV_VertexID)
{
    PSInput result;
    float4 worldPos = mul(float4(input.position, 1.0f), WorldMatrix);
    result.position = mul(worldPos, ViewProjectionMatrix);
    result.unjitteredPosition = mul(worldPos, UnjitteredViewProjMat);

    const uint localVertex = vertexId - SkeletalCharIndex * SkeletalVertsPerChar;
    const uint boneBase = SkeletalCharIndex * SkeletalBoneCount;

    SkinInputVertex_t v = SkeletalInputs[localVertex];
    uint i0 = (v.BoneIndicesPacked >>  0) & 0xFFu;
    uint i1 = (v.BoneIndicesPacked >>  8) & 0xFFu;
    uint i2 = (v.BoneIndicesPacked >> 16) & 0xFFu;
    uint i3 = (v.BoneIndicesPacked >> 24) & 0xFFu;
    SkinBone_t b0 = SkeletalPrevBones[boneBase + i0];
    SkinBone_t b1 = SkeletalPrevBones[boneBase + i1];
    SkinBone_t b2 = SkeletalPrevBones[boneBase + i2];
    SkinBone_t b3 = SkeletalPrevBones[boneBase + i3];
    float4 bp = float4(v.BindPosition, 1.0f);
    float3 P0 = float3(dot(b0.Row0, bp), dot(b0.Row1, bp), dot(b0.Row2, bp));
    float3 P1 = float3(dot(b1.Row0, bp), dot(b1.Row1, bp), dot(b1.Row2, bp));
    float3 P2 = float3(dot(b2.Row0, bp), dot(b2.Row1, bp), dot(b2.Row2, bp));
    float3 P3 = float3(dot(b3.Row0, bp), dot(b3.Row1, bp), dot(b3.Row2, bp));
    float3 prevObjPos = P0*v.BoneWeights.x + P1*v.BoneWeights.y + P2*v.BoneWeights.z + P3*v.BoneWeights.w;
    float4 prevWorldPos = mul(float4(prevObjPos, 1.0f), WorldMatrix);
    result.prevPosition = mul(prevWorldPos, PrevUnjitteredViewProjMat);

    result.normal = normalize(mul(float4(input.normal, 0), WorldMatrix));
    result.tangent = normalize(mul(float4(input.tangent, 0), WorldMatrix));
    result.uv = input.uv;
    return result;
}

// Path C — VS inline skinning. No compute pre-pass, no skinned VB; the
// VS pulls bind-pose vertex from IA + bone palettes from SBVs and
// computes the skinned position/normal/tangent itself.
PSInput SkeletalVsInlineVSMain(VSInput input, uint vertexId : SV_VertexID)
{
    PSInput result;
    const uint localVertex = vertexId - SkeletalCharIndex * SkeletalVertsPerChar;
    const uint boneBase = SkeletalCharIndex * SkeletalBoneCount;
    const float4x4 worldMatrix = WorldMatrix;

    SkinInputVertex_t v = SkeletalInputs[localVertex];
    uint i0 = (v.BoneIndicesPacked >>  0) & 0xFFu;
    uint i1 = (v.BoneIndicesPacked >>  8) & 0xFFu;
    uint i2 = (v.BoneIndicesPacked >> 16) & 0xFFu;
    uint i3 = (v.BoneIndicesPacked >> 24) & 0xFFu;
    float w0 = v.BoneWeights.x;
    float w1 = v.BoneWeights.y;
    float w2 = v.BoneWeights.z;
    float w3 = v.BoneWeights.w;
    float4 bp = float4(v.BindPosition, 1.0f);

    // Curr-frame skin → object space.
    SkinBone_t c0 = SkeletalCurrBones[boneBase + i0];
    SkinBone_t c1 = SkeletalCurrBones[boneBase + i1];
    SkinBone_t c2 = SkeletalCurrBones[boneBase + i2];
    SkinBone_t c3 = SkeletalCurrBones[boneBase + i3];
    float3 CP0 = float3(dot(c0.Row0, bp), dot(c0.Row1, bp), dot(c0.Row2, bp));
    float3 CP1 = float3(dot(c1.Row0, bp), dot(c1.Row1, bp), dot(c1.Row2, bp));
    float3 CP2 = float3(dot(c2.Row0, bp), dot(c2.Row1, bp), dot(c2.Row2, bp));
    float3 CP3 = float3(dot(c3.Row0, bp), dot(c3.Row1, bp), dot(c3.Row2, bp));
    float3 currObjPos = CP0*w0 + CP1*w1 + CP2*w2 + CP3*w3;

    float3 N0 = float3(dot(c0.Row0.xyz, input.normal), dot(c0.Row1.xyz, input.normal), dot(c0.Row2.xyz, input.normal));
    float3 N1 = float3(dot(c1.Row0.xyz, input.normal), dot(c1.Row1.xyz, input.normal), dot(c1.Row2.xyz, input.normal));
    float3 N2 = float3(dot(c2.Row0.xyz, input.normal), dot(c2.Row1.xyz, input.normal), dot(c2.Row2.xyz, input.normal));
    float3 N3 = float3(dot(c3.Row0.xyz, input.normal), dot(c3.Row1.xyz, input.normal), dot(c3.Row2.xyz, input.normal));
    float3 currObjNormal = normalize(N0*w0 + N1*w1 + N2*w2 + N3*w3);

    float3 T0 = float3(dot(c0.Row0.xyz, input.tangent), dot(c0.Row1.xyz, input.tangent), dot(c0.Row2.xyz, input.tangent));
    float3 T1 = float3(dot(c1.Row0.xyz, input.tangent), dot(c1.Row1.xyz, input.tangent), dot(c1.Row2.xyz, input.tangent));
    float3 T2 = float3(dot(c2.Row0.xyz, input.tangent), dot(c2.Row1.xyz, input.tangent), dot(c2.Row2.xyz, input.tangent));
    float3 T3 = float3(dot(c3.Row0.xyz, input.tangent), dot(c3.Row1.xyz, input.tangent), dot(c3.Row2.xyz, input.tangent));
    float3 currObjTangent = T0*w0 + T1*w1 + T2*w2 + T3*w3;
    currObjTangent = normalize(currObjTangent - currObjNormal * dot(currObjNormal, currObjTangent));

    float4 worldPos = mul(float4(currObjPos, 1.0f), worldMatrix);
    result.position = mul(worldPos, ViewProjectionMatrix);
    result.unjitteredPosition = mul(worldPos, UnjitteredViewProjMat);

    SkinBone_t p0 = SkeletalPrevBones[boneBase + i0];
    SkinBone_t p1 = SkeletalPrevBones[boneBase + i1];
    SkinBone_t p2 = SkeletalPrevBones[boneBase + i2];
    SkinBone_t p3 = SkeletalPrevBones[boneBase + i3];
    float3 PP0 = float3(dot(p0.Row0, bp), dot(p0.Row1, bp), dot(p0.Row2, bp));
    float3 PP1 = float3(dot(p1.Row0, bp), dot(p1.Row1, bp), dot(p1.Row2, bp));
    float3 PP2 = float3(dot(p2.Row0, bp), dot(p2.Row1, bp), dot(p2.Row2, bp));
    float3 PP3 = float3(dot(p3.Row0, bp), dot(p3.Row1, bp), dot(p3.Row2, bp));
    float3 prevObjPos = PP0*w0 + PP1*w1 + PP2*w2 + PP3*w3;
    float4 prevWorldPos = mul(float4(prevObjPos, 1.0f), worldMatrix);
    result.prevPosition = mul(prevWorldPos, PrevUnjitteredViewProjMat);

    result.normal = normalize(mul(float4(currObjNormal, 0), worldMatrix));
    result.tangent = normalize(mul(float4(currObjTangent, 0), worldMatrix));
    result.uv = v.UV;
    return result;
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
