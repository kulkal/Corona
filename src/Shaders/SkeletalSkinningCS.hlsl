// SkeletalSkinningCS.hlsl — 3D character skeletal skinning compute shader.
//
// Separate from SpineSkinningCS.hlsl (2D Spine path). Output buffer layout
// matches the standard vertex layout (POSITION/TEXCOORD/NORMAL/TANGENT) so
// the GBuffer / shadow PSOs can read it through their normal IA input.

#include "ShaderResourceBindings.hlsli"

// Must match SkinInputVertex in Corona.Skeletal.cpp (80 B).
struct SkinInputVertex
{
    float3 BindPosition;
    float  Pad0;
    float3 BindNormal;
    float  Pad1;
    float3 BindTangent;
    float  Pad2;
    float2 UV;
    uint   BoneIndicesPacked;   // 4 x uint8 (only the low 2 are currently set)
    float  PadEnd;
    float4 BoneWeights;
};

// mat3x4 row-major (translation in .w of each row).
struct SkinBone
{
    float4 Row0;
    float4 Row1;
    float4 Row2;
};

// Must match StandardVertex (48 B) on the CPU side.
struct SkinnedVertex
{
    float4 Position;
    float2 UV;
    float2 _Pad;        // align Normal to 16 B boundary inside the structured layout
    float3 Normal;
    float  _PadN;
    float3 Tangent;
    float  _PadT;
};

StructuredBuffer<SkinInputVertex> Inputs : register(t0);
StructuredBuffer<SkinBone>        Bones  : register(t1);
RWByteAddressBuffer               Output : register(u0);

// Phase A: unified-buffer batched dispatch. One Dispatch covers every
// character. `Inputs` holds a single shared copy of the bind-pose vertices
// (size = VertsPerChar) because the test characters share one mesh.
// `Bones` is a flat array sized CharCount * BoneCount; the slice for
// character c starts at index c*BoneCount. `Output` is sized
// CharCount * VertsPerChar * sizeof(StandardVertex); the slice for
// character c starts at vertex c*VertsPerChar.
cbuffer SkeletalSkinningConstant : register(b0)
{
    uint TotalVertexCount;   // CharCount * VertsPerChar
    uint VertsPerChar;
    uint BoneCount;
    uint _Pad;
};

// Apply a mat3x4 to a homogeneous point (w=1).
float3 TransformPoint(SkinBone bone, float3 p)
{
    float4 ph = float4(p, 1.0f);
    return float3(dot(bone.Row0, ph), dot(bone.Row1, ph), dot(bone.Row2, ph));
}

// Apply only the 3x3 rotation/scale portion of a mat3x4 to a direction.
float3 TransformDirection(SkinBone bone, float3 d)
{
    return float3(
        bone.Row0.x * d.x + bone.Row0.y * d.y + bone.Row0.z * d.z,
        bone.Row1.x * d.x + bone.Row1.y * d.y + bone.Row1.z * d.z,
        bone.Row2.x * d.x + bone.Row2.y * d.y + bone.Row2.z * d.z);
}

[numthreads(64, 1, 1)]
void SkinMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint globalVertex = dispatchThreadID.x;
    if (globalVertex >= TotalVertexCount)
        return;

    const uint charIndex = globalVertex / VertsPerChar;
    const uint localVertex = globalVertex - charIndex * VertsPerChar;
    const uint boneBase = charIndex * BoneCount;

    SkinInputVertex v = Inputs[localVertex];

    const uint i0 = (v.BoneIndicesPacked >>  0) & 0xFFu;
    const uint i1 = (v.BoneIndicesPacked >>  8) & 0xFFu;
    const uint i2 = (v.BoneIndicesPacked >> 16) & 0xFFu;
    const uint i3 = (v.BoneIndicesPacked >> 24) & 0xFFu;

    const SkinBone b0 = Bones[boneBase + i0];
    const SkinBone b1 = Bones[boneBase + i1];
    const SkinBone b2 = Bones[boneBase + i2];
    const SkinBone b3 = Bones[boneBase + i3];

    const float w0 = v.BoneWeights.x;
    const float w1 = v.BoneWeights.y;
    const float w2 = v.BoneWeights.z;
    const float w3 = v.BoneWeights.w;

    float3 P0 = TransformPoint(b0, v.BindPosition);
    float3 P1 = TransformPoint(b1, v.BindPosition);
    float3 P2 = TransformPoint(b2, v.BindPosition);
    float3 P3 = TransformPoint(b3, v.BindPosition);
    float3 P  = P0 * w0 + P1 * w1 + P2 * w2 + P3 * w3;

    float3 N0 = TransformDirection(b0, v.BindNormal);
    float3 N1 = TransformDirection(b1, v.BindNormal);
    float3 N2 = TransformDirection(b2, v.BindNormal);
    float3 N3 = TransformDirection(b3, v.BindNormal);
    float3 N  = normalize(N0 * w0 + N1 * w1 + N2 * w2 + N3 * w3);

    float3 T0 = TransformDirection(b0, v.BindTangent);
    float3 T1 = TransformDirection(b1, v.BindTangent);
    float3 T2 = TransformDirection(b2, v.BindTangent);
    float3 T3 = TransformDirection(b3, v.BindTangent);
    float3 T  = T0 * w0 + T1 * w1 + T2 * w2 + T3 * w3;
    // Re-orthogonalize tangent against the skinned normal.
    T = normalize(T - N * dot(N, T));

    // Write to standard vertex layout: float4 pos @ 0, float2 uv @ 16,
    // float3 nrm @ 24, float3 tan @ 36. Stride 48 B. Address uses
    // globalVertex so each character writes into its own slice of the
    // unified output VB.
    const uint addr = globalVertex * 48u;
    Output.Store4(addr +  0u, asuint(float4(P, 1.0f)));
    Output.Store2(addr + 16u, asuint(v.UV));
    Output.Store3(addr + 24u, asuint(N));
    Output.Store3(addr + 36u, asuint(T));
}
