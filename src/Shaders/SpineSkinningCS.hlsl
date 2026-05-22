#include "ShaderResourceBindings.hlsli"

struct SpineSkinInputVertex
{
    float2 uv;
    float localZ;
    uint influenceOffset;
    uint influenceCount;
    uint padding;
};

struct SpineSkinInfluence
{
    float2 localPosition;
    uint boneIndex;
    float weight;
};

struct SpineSkinBone
{
    float4 xformX;
    float4 xformY;
};

struct SpineSkinnedVertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float3 tangent;
};

StructuredBuffer<SpineSkinInputVertex> InputVertices : register(t0);
StructuredBuffer<SpineSkinInfluence> Influences : register(t1);
StructuredBuffer<SpineSkinBone> Bones : register(t2);
RWStructuredBuffer<SpineSkinnedVertex> OutputVertices : register(u0);

cbuffer SpineSkinningConstant : register(b0)
{
    uint VertexCount;
    float SourceScale;
    float2 Padding;
};

[numthreads(64, 1, 1)]
void SkinMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint vertexIndex = dispatchThreadID.x;
    if (vertexIndex >= VertexCount)
        return;

    SpineSkinInputVertex input = InputVertices[vertexIndex];
    float2 worldPosition = float2(0.0f, 0.0f);

    [loop]
    for (uint influenceIndex = 0; influenceIndex < input.influenceCount; ++influenceIndex)
    {
        SpineSkinInfluence influence = Influences[input.influenceOffset + influenceIndex];
        SpineSkinBone bone = Bones[influence.boneIndex];
        const float2 localPosition = influence.localPosition;
        const float2 skinnedPosition = float2(
            localPosition.x * bone.xformX.x + localPosition.y * bone.xformX.y + bone.xformX.z,
            localPosition.x * bone.xformY.x + localPosition.y * bone.xformY.y + bone.xformY.z);
        worldPosition += skinnedPosition * influence.weight;
    }

    SpineSkinnedVertex output;
    output.position = float3(worldPosition * SourceScale, input.localZ * SourceScale);
    output.normal = float3(0.0f, 0.0f, 1.0f);
    output.uv = input.uv;
    output.tangent = float3(1.0f, 0.0f, 0.0f);
    OutputVertices[vertexIndex] = output;
}
