// SkeletalSkinningCS.hlsl — 3D character skeletal skinning compute shader.
//
// Separate from SpineSkinningCS.hlsl (2D Spine path). Output buffer layout
// matches the standard vertex layout (POSITION/TEXCOORD/NORMAL/TANGENT) so
// the GBuffer / shadow PSOs can read it through their normal IA input.
//
// Phase 1: stub. Phase 3 fills in the actual skinning math.

#include "ShaderResourceBindings.hlsli"

cbuffer SkeletalSkinningConstant : register(b0)
{
    uint VertexCount;
    uint BoneBase;
    uint Pad0;
    uint Pad1;
};

[numthreads(64, 1, 1)]
void SkinMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    // Phase 3 fills this in.
}
