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
    // Phase A: per-draw locator into the unified skeletal buffers.
    uint SkeletalCharIndex;
    uint SkeletalVertsPerChar;
    uint SkeletalBoneCount;
    uint _SkeletalPad;
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

// Phase 11 (revised): skeletal motion-vector path. The skeletal GBuffer
// PSO is the only thing that references these. Layout must match the
// SkinInputVertex / SkinBone structs in Corona.Skeletal.cpp and
// SkeletalSkinningCS.hlsl.
struct SkinInputVertex_t
{
    float3 BindPosition;
    float  Pad0;
    float3 BindNormal;
    float  Pad1;
    float3 BindTangent;
    float  Pad2;
    float2 UV;
    uint   BoneIndicesPacked;   // 4 x uint8
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
// Path C (VS inline skinning): current-frame bone palette. Phase B's
// SkeletalInstanceTransforms SBV was dropped — Adreno's HLSL→SPIR-V
// path refused to compile the VS with that binding present, so we
// fall back to per-mesh draws that read WorldMatrix from the CB. The
// VS still uses SkeletalCharIndex (from CB) to index Bones / PrevBones.
StructuredBuffer<SkinBone_t>        SkeletalCurrBones : register(t7);

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

// Phase 11 (revised) — GBuffer VS variant for skeletal-skinned meshes.
// Curr-frame position comes from the standard IA layout (already filled
// by the compute skinning pass this frame). Prev-frame position is
// derived by re-skinning the bind-pose vertex with the *previous* frame's
// bone palette, which sits in SkeletalPrevBones. This keeps the BLAS
// source VB stable (no ping-pong) and stays accurate per-vertex.
PSInput SkeletalVSMain(VSInput input, uint vertexId : SV_VertexID)
{
    PSInput result;

    // Per-mesh draw path: CB.WorldMatrix is the world transform,
    // CB.SkeletalCharIndex picks the char's slice of the unified
    // SkeletalInputs / SkeletalPrevBones SBVs.
    const uint charIndex = SkeletalCharIndex;
    const float4x4 worldMatrix = WorldMatrix;

    float4 worldPos = mul(float4(input.position, 1.0f), worldMatrix);
    result.position = mul(worldPos, ViewProjectionMatrix);
    result.unjitteredPosition = mul(worldPos, UnjitteredViewProjMat);

    // BaseVertexLocation in the per-mesh draw offsets SV_VertexID;
    // subtract the char base to land on the local slot inside the
    // single shared bind-pose Inputs SBV.
    const uint localVertex = vertexId - charIndex * SkeletalVertsPerChar;
    const uint boneBase = charIndex * SkeletalBoneCount;

    // Re-skin from bind position using prev bones for this character's slice.
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
    float3 prevObjPos =
        P0 * v.BoneWeights.x +
        P1 * v.BoneWeights.y +
        P2 * v.BoneWeights.z +
        P3 * v.BoneWeights.w;
    float4 prevWorldPos = mul(float4(prevObjPos, 1.0f), worldMatrix);
    result.prevPosition = mul(prevWorldPos, PrevUnjitteredViewProjMat);

    result.normal = normalize(mul(float4(input.normal, 0), worldMatrix));
    result.tangent = normalize(mul(float4(input.tangent, 0), worldMatrix));
    result.uv = input.uv;
    return result;
}

// Path C — pure VS skinning. There is no compute pre-pass, no skinned
// VB. IA is bound to the static bind-pose VB; the VS pulls the bone
// indices/weights from SkeletalInputs and the per-char palette from
// SkeletalCurrBones (curr-frame) and SkeletalPrevBones (prev-frame for
// motion vectors). All draws use the instanced path (SV_InstanceID =
// charIdx, no SkeletalCharIndex sentinel needed because this VS is
// only ever bound by the instanced draw).
PSInput SkeletalVsInlineVSMain(VSInput input, uint vertexId : SV_VertexID)
{
    PSInput result;

    const uint charIndex = SkeletalCharIndex;
    const uint boneBase = charIndex * SkeletalBoneCount;
    const float4x4 worldMatrix = WorldMatrix;

    const uint localVertex = vertexId - charIndex * SkeletalVertsPerChar;
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
    float3 currObjPos = CP0 * w0 + CP1 * w1 + CP2 * w2 + CP3 * w3;

    // Curr-frame normal + tangent: only the 3x3 portion of each bone.
    float3 N0 = float3(c0.Row0.x * input.normal.x + c0.Row0.y * input.normal.y + c0.Row0.z * input.normal.z,
                       c0.Row1.x * input.normal.x + c0.Row1.y * input.normal.y + c0.Row1.z * input.normal.z,
                       c0.Row2.x * input.normal.x + c0.Row2.y * input.normal.y + c0.Row2.z * input.normal.z);
    float3 N1 = float3(c1.Row0.x * input.normal.x + c1.Row0.y * input.normal.y + c1.Row0.z * input.normal.z,
                       c1.Row1.x * input.normal.x + c1.Row1.y * input.normal.y + c1.Row1.z * input.normal.z,
                       c1.Row2.x * input.normal.x + c1.Row2.y * input.normal.y + c1.Row2.z * input.normal.z);
    float3 N2 = float3(c2.Row0.x * input.normal.x + c2.Row0.y * input.normal.y + c2.Row0.z * input.normal.z,
                       c2.Row1.x * input.normal.x + c2.Row1.y * input.normal.y + c2.Row1.z * input.normal.z,
                       c2.Row2.x * input.normal.x + c2.Row2.y * input.normal.y + c2.Row2.z * input.normal.z);
    float3 N3 = float3(c3.Row0.x * input.normal.x + c3.Row0.y * input.normal.y + c3.Row0.z * input.normal.z,
                       c3.Row1.x * input.normal.x + c3.Row1.y * input.normal.y + c3.Row1.z * input.normal.z,
                       c3.Row2.x * input.normal.x + c3.Row2.y * input.normal.y + c3.Row2.z * input.normal.z);
    float3 currObjNormal = normalize(N0 * w0 + N1 * w1 + N2 * w2 + N3 * w3);

    float3 T0 = float3(c0.Row0.x * input.tangent.x + c0.Row0.y * input.tangent.y + c0.Row0.z * input.tangent.z,
                       c0.Row1.x * input.tangent.x + c0.Row1.y * input.tangent.y + c0.Row1.z * input.tangent.z,
                       c0.Row2.x * input.tangent.x + c0.Row2.y * input.tangent.y + c0.Row2.z * input.tangent.z);
    float3 T1 = float3(c1.Row0.x * input.tangent.x + c1.Row0.y * input.tangent.y + c1.Row0.z * input.tangent.z,
                       c1.Row1.x * input.tangent.x + c1.Row1.y * input.tangent.y + c1.Row1.z * input.tangent.z,
                       c1.Row2.x * input.tangent.x + c1.Row2.y * input.tangent.y + c1.Row2.z * input.tangent.z);
    float3 T2 = float3(c2.Row0.x * input.tangent.x + c2.Row0.y * input.tangent.y + c2.Row0.z * input.tangent.z,
                       c2.Row1.x * input.tangent.x + c2.Row1.y * input.tangent.y + c2.Row1.z * input.tangent.z,
                       c2.Row2.x * input.tangent.x + c2.Row2.y * input.tangent.y + c2.Row2.z * input.tangent.z);
    float3 T3 = float3(c3.Row0.x * input.tangent.x + c3.Row0.y * input.tangent.y + c3.Row0.z * input.tangent.z,
                       c3.Row1.x * input.tangent.x + c3.Row1.y * input.tangent.y + c3.Row1.z * input.tangent.z,
                       c3.Row2.x * input.tangent.x + c3.Row2.y * input.tangent.y + c3.Row2.z * input.tangent.z);
    float3 currObjTangent = T0 * w0 + T1 * w1 + T2 * w2 + T3 * w3;
    currObjTangent = normalize(currObjTangent - currObjNormal * dot(currObjNormal, currObjTangent));

    float4 worldPos = mul(float4(currObjPos, 1.0f), worldMatrix);
    result.position = mul(worldPos, ViewProjectionMatrix);
    result.unjitteredPosition = mul(worldPos, UnjitteredViewProjMat);

    // Prev-frame skin (motion vector). Same math with the previous-frame
    // bone palette. Note: VS inline cannot use a per-instance prev-frame
    // world transform — uses the current world matrix (animated joints
    // dominate the motion vector for stationary characters).
    SkinBone_t p0 = SkeletalPrevBones[boneBase + i0];
    SkinBone_t p1 = SkeletalPrevBones[boneBase + i1];
    SkinBone_t p2 = SkeletalPrevBones[boneBase + i2];
    SkinBone_t p3 = SkeletalPrevBones[boneBase + i3];
    float3 PP0 = float3(dot(p0.Row0, bp), dot(p0.Row1, bp), dot(p0.Row2, bp));
    float3 PP1 = float3(dot(p1.Row0, bp), dot(p1.Row1, bp), dot(p1.Row2, bp));
    float3 PP2 = float3(dot(p2.Row0, bp), dot(p2.Row1, bp), dot(p2.Row2, bp));
    float3 PP3 = float3(dot(p3.Row0, bp), dot(p3.Row1, bp), dot(p3.Row2, bp));
    float3 prevObjPos = PP0 * w0 + PP1 * w1 + PP2 * w2 + PP3 * w3;
    float4 prevWorldPos = mul(float4(prevObjPos, 1.0f), worldMatrix);
    result.prevPosition = mul(prevWorldPos, PrevUnjitteredViewProjMat);

    result.normal = normalize(mul(float4(currObjNormal, 0), worldMatrix));
    result.tangent = normalize(mul(float4(currObjTangent, 0), worldMatrix));
    result.uv = v.UV;
    return result;
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
