// GBufferCommon.hlsli — shared VS pipeline for every GBuffer-family mesh.
//
// Three layers (see docs/gbuffer_vs_unification_plan.md):
//   1. Source loaders (LoadVertex_*) per mesh type → VertexObjSpace
//   2. Deformation pipeline (ApplyVertexDeformations) → VertexObjSpace
//   3. Final transform (BuildPSInput) → PSInput
//
// Each VS entry is a 3-line composition of those layers. New vertex
// deformation effects go into Layer 2 and automatically apply to every
// mesh type (static, Spine cached, Spine VS-inline, Skeletal compute,
// Skeletal VS-inline, Skeletal cluster).
//
// Resource bindings live here so a binding-slot conflict between paths
// is impossible by construction.
//
// Included by:
//   GBuffer.hlsl       — desktop GBuffer PS + cluster VS gated by GBUFFER_HAS_CLUSTER
//   GBufferMobile.hlsl — mobile GBuffer PS (no cluster)

#ifndef GBUFFER_COMMON_HLSLI
#define GBUFFER_COMMON_HLSLI

#include "ShaderResourceBindings.hlsli"
#include "Common.hlsl"

// =====================================================================
// Layer 0: Resource bindings + structs
// =====================================================================

TEXTURE2D_BINDING(AlbedoTex,    0);
TEXTURE2D_BINDING(NormalTex,    1);
TEXTURE2D_BINDING(RoughnessTex, 2);
TEXTURE2D_BINDING(MetallicTex,  3);

SAMPLER_BINDING(sampleWrap, 0);

CBUFFER_BINDING_BEGIN(GBufferConstantBuffer, 0)
{
    float4x4 ViewProjectionMatrix;
    float4x4 PrevViewProjectionMatrix;
    float4x4 WorldMatrix;
    float4x4 UnjitteredViewProjMat;
    float4x4 PrevUnjitteredViewProjMat;
    float4   ViewDir;
    float4   BaseColorFactor;
    float2   RTSize;
    float2   RougnessMetalic;
    uint     bOverrideRougnessMetallic;
    uint     bTwoSidedLighting;
    uint     bUnlitMaterial;
    uint     SpineVertexBase;
    // Per-draw locator into the unified skeletal buffers.
    uint     SkeletalCharIndex;
    uint     SkeletalVertsPerChar;
    uint     SkeletalBoneCount;
    // Spine VS-inline source scale (matches SpineSkinningCS::SourceScale).
    float    SpineSourceScale;
    // Layer 2 vertex deformation params. .x = time (seconds), reserved
    // for future per-effect knobs. Currently unused; kept for future
    // global deformations.
    float4   MeshDeformParams;
    // Grass bend origin (player world position). .xyz = world position,
    // .w = bend strength * radius (0 disables grass bend). Used by the
    // grass-blade deformation in ApplyVertexDeformations.
    float4   GrassBendOrigin;
    // Grass bend more params. .x = bend radius (units), .y = max blade
    // height (used to convert object-space Y → "height ratio" so blade
    // tips bend more than roots), .zw = reserved.
    float4   GrassBendParams;
    // Per-mesh flag set by the host when drawing the procedural grass
    // mesh. Non-zero → ApplyVertexDeformations runs the grass bend on
    // this mesh's vertices. Every other mesh leaves it at 0.
    uint     bGrassMesh;
    uint3    _GBufferCBPad;
    // Wind sway. .xyz = wind direction normalized in XZ (Y typically 0),
    // .w = strength (0 disables). Composes additively with grass bend.
    float4   WindParams;
    // Wind tuning. .x = temporal frequency (rad/s on the sin), .y =
    // spatial frequency (rad/world-unit, gives blade-to-blade variation),
    // .zw = reserved.
    float4   WindTuning;
} CBUFFER_BINDING_END;

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float3 tangent  : TANGENT;
};

// Cached compute-fetch Spine (SpineSkinningCS output baked once).
struct SpineSkinnedVertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float3 tangent;
};
StructuredBuffer<SpineSkinnedVertex> SpineVertices : register(t4);

// 3D skeletal: bind pose + per-frame palettes.
struct SkinInputVertex_t
{
    float3 BindPosition;
    float  Pad0;
    float3 BindNormal;
    float  Pad1;
    float3 BindTangent;
    float  Pad2;
    float2 UV;
    uint   BoneIndicesPacked;   // 4 × uint8
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

#ifdef GBUFFER_HAS_CLUSTER
// Desktop-only: per-instance world transform indexed by SV_InstanceID
// for the cluster draw path. Mobile (GBufferMobile.hlsl) doesn't define
// GBUFFER_HAS_CLUSTER, so this binding + the cluster VS entry below
// stay out of the SPIR-V build for Adreno.
StructuredBuffer<SkinBone_t> SkeletalInstanceTransforms : register(t8);

float4x4 BuildWorldMatrixFromMat3x4Cluster(SkinBone_t m)
{
    return float4x4(
        m.Row0.x, m.Row1.x, m.Row2.x, 0.0f,
        m.Row0.y, m.Row1.y, m.Row2.y, 0.0f,
        m.Row0.z, m.Row1.z, m.Row2.z, 0.0f,
        m.Row0.w, m.Row1.w, m.Row2.w, 1.0f);
}
#endif // GBUFFER_HAS_CLUSTER

// Spine VS-inline (live + cached): bind-pose attachment data,
// influence list, bones SBV.
struct SpineSkinInputVertex_t
{
    float2 uv;
    float  localZ;
    uint   influenceOffset;
    uint   influenceCount;
    uint   _pad;
};
struct SpineSkinInfluence_t
{
    float2 localPosition;
    uint   boneIndex;
    float  weight;
};
struct SpineSkinBone_t
{
    float4 xformX;
    float4 xformY;
};
StructuredBuffer<SpineSkinInputVertex_t> SpineVsInlineInputVertices : register(t9);
StructuredBuffer<SpineSkinInfluence_t>   SpineVsInlineInfluences    : register(t10);
StructuredBuffer<SpineSkinBone_t>        SpineVsInlineBones         : register(t11);

struct PSInput
{
    float4 position           : SV_POSITION;
    float4 prevPosition       : PREVPOSITION;
    float4 unjitteredPosition : UnjitteredPOSITION;
    float2 uv                 : TEXCOORD0;
    float3 normal             : NORMAL;
    float3 tangent            : TANGENT;
};

// =====================================================================
// Unified vertex representation (Layer 1 output / Layer 2 in/out)
// =====================================================================

// Every LoadVertex_* function returns this. Layer 2
// (ApplyVertexDeformations) and Layer 3 (BuildPSInput) read it.
struct VertexObjSpace
{
    float3 currObjPos;        // current-frame object-space position
    float3 currObjNormal;
    float3 currObjTangent;
    float3 prevObjPos;        // previous-frame object-space position
                              // (for static meshes: same as currObjPos)
    float2 uv;
    float4x4 worldMatrix;     // current-frame world transform
    float4x4 prevWorldMatrix; // previous-frame world transform
                              // (for now: same as worldMatrix; can diverge
                              //  if per-instance prev transforms get added)
    uint   instanceId;        // per-character / per-instance ID — used by
                              // Layer 2 effects for per-instance phase
                              // (wind sway randomization, etc.)
};

// =====================================================================
// Layer 1: Source loaders
// =====================================================================

VertexObjSpace LoadVertex_Static(VSInput input)
{
    VertexObjSpace v;
    v.currObjPos     = input.position;
    v.currObjNormal  = input.normal;
    v.currObjTangent = input.tangent;
    v.prevObjPos     = input.position; // no per-vertex animation
    v.uv             = input.uv;
    v.worldMatrix    = WorldMatrix;
    v.prevWorldMatrix = WorldMatrix;
    v.instanceId     = 0;
    return v;
}

VertexObjSpace LoadVertex_SpineCached(uint vertexId)
{
    // Cached compute-fetch: SpineVertices was baked once by SpineSkinningCS.
    SpineSkinnedVertex input = SpineVertices[SpineVertexBase + vertexId];
    VertexObjSpace v;
    v.currObjPos     = input.position;
    v.currObjNormal  = input.normal;
    v.currObjTangent = input.tangent;
    v.prevObjPos     = input.position;
    v.uv             = input.uv;
    v.worldMatrix    = WorldMatrix;
    v.prevWorldMatrix = WorldMatrix;
    v.instanceId     = 0;
    return v;
}

VertexObjSpace LoadVertex_SpineInline(uint vertexId)
{
    // VS-inline (cached or live): bind-pose attachment + weighted influence list.
    SpineSkinInputVertex_t v0 = SpineVsInlineInputVertices[vertexId];
    float2 worldPosition = float2(0.0f, 0.0f);
    [loop]
    for (uint i = 0; i < v0.influenceCount; ++i)
    {
        SpineSkinInfluence_t inf = SpineVsInlineInfluences[v0.influenceOffset + i];
        SpineSkinBone_t bone = SpineVsInlineBones[inf.boneIndex];
        const float2 lp = inf.localPosition;
        const float2 skinned = float2(
            lp.x * bone.xformX.x + lp.y * bone.xformX.y + bone.xformX.z,
            lp.x * bone.xformY.x + lp.y * bone.xformY.y + bone.xformY.z);
        worldPosition += skinned * inf.weight;
    }

    VertexObjSpace v;
    v.currObjPos     = float3(worldPosition * SpineSourceScale, v0.localZ * SpineSourceScale);
    v.currObjNormal  = float3(0.0f, 0.0f, 1.0f);
    v.currObjTangent = float3(1.0f, 0.0f, 0.0f);
    v.prevObjPos     = v.currObjPos; // no per-vertex prev bones SBV; motion vec = 0 by skin
    v.uv             = v0.uv;
    v.worldMatrix    = WorldMatrix;
    v.prevWorldMatrix = WorldMatrix;
    v.instanceId     = 0;
    return v;
}

// Helper: 4-bone weighted skin of a position with a palette + per-vertex weights.
float3 SkinPosFourBoneInternal(
    StructuredBuffer<SkinBone_t> palette,
    uint boneBase,
    uint i0, uint i1, uint i2, uint i3,
    float4 weights,
    float4 bp)
{
    SkinBone_t b0 = palette[boneBase + i0];
    SkinBone_t b1 = palette[boneBase + i1];
    SkinBone_t b2 = palette[boneBase + i2];
    SkinBone_t b3 = palette[boneBase + i3];
    float3 P0 = float3(dot(b0.Row0, bp), dot(b0.Row1, bp), dot(b0.Row2, bp));
    float3 P1 = float3(dot(b1.Row0, bp), dot(b1.Row1, bp), dot(b1.Row2, bp));
    float3 P2 = float3(dot(b2.Row0, bp), dot(b2.Row1, bp), dot(b2.Row2, bp));
    float3 P3 = float3(dot(b3.Row0, bp), dot(b3.Row1, bp), dot(b3.Row2, bp));
    return P0 * weights.x + P1 * weights.y + P2 * weights.z + P3 * weights.w;
}

// Helper: 4-bone weighted skin of a 3D direction (normal or tangent).
// Uses only the 3×3 portion of each bone (no translation).
float3 SkinDirFourBoneInternal(
    StructuredBuffer<SkinBone_t> palette,
    uint boneBase,
    uint i0, uint i1, uint i2, uint i3,
    float4 weights,
    float3 dir)
{
    SkinBone_t b0 = palette[boneBase + i0];
    SkinBone_t b1 = palette[boneBase + i1];
    SkinBone_t b2 = palette[boneBase + i2];
    SkinBone_t b3 = palette[boneBase + i3];
    float3 D0 = float3(dot(b0.Row0.xyz, dir), dot(b0.Row1.xyz, dir), dot(b0.Row2.xyz, dir));
    float3 D1 = float3(dot(b1.Row0.xyz, dir), dot(b1.Row1.xyz, dir), dot(b1.Row2.xyz, dir));
    float3 D2 = float3(dot(b2.Row0.xyz, dir), dot(b2.Row1.xyz, dir), dot(b2.Row2.xyz, dir));
    float3 D3 = float3(dot(b3.Row0.xyz, dir), dot(b3.Row1.xyz, dir), dot(b3.Row2.xyz, dir));
    return D0 * weights.x + D1 * weights.y + D2 * weights.z + D3 * weights.w;
}

// Compute path: IA already holds compute-output (currently-skinned) pos;
// only the prev pos is reconstructed from bind pose × prev bones.
VertexObjSpace LoadVertex_SkeletalComp(VSInput input, uint vertexId)
{
    const uint charIndex   = SkeletalCharIndex;
    const uint localVertex = vertexId - charIndex * SkeletalVertsPerChar;
    const uint boneBase    = charIndex * SkeletalBoneCount;

    SkinInputVertex_t sv = SkeletalInputs[localVertex];
    uint i0 = (sv.BoneIndicesPacked >>  0) & 0xFFu;
    uint i1 = (sv.BoneIndicesPacked >>  8) & 0xFFu;
    uint i2 = (sv.BoneIndicesPacked >> 16) & 0xFFu;
    uint i3 = (sv.BoneIndicesPacked >> 24) & 0xFFu;
    float4 bp = float4(sv.BindPosition, 1.0f);

    VertexObjSpace v;
    v.currObjPos     = input.position;
    v.currObjNormal  = input.normal;
    v.currObjTangent = input.tangent;
    v.prevObjPos     = SkinPosFourBoneInternal(SkeletalPrevBones, boneBase, i0, i1, i2, i3, sv.BoneWeights, bp);
    v.uv             = input.uv;
    v.worldMatrix    = WorldMatrix;
    v.prevWorldMatrix = WorldMatrix;
    v.instanceId     = charIndex;
    return v;
}

// VS-inline (per-mesh): skin from bind pose on the GPU using curr palette
// for current frame and prev palette for motion vector.
VertexObjSpace LoadVertex_SkeletalIL(VSInput input, uint vertexId)
{
    const uint charIndex   = SkeletalCharIndex;
    const uint localVertex = vertexId - charIndex * SkeletalVertsPerChar;
    const uint boneBase    = charIndex * SkeletalBoneCount;

    SkinInputVertex_t sv = SkeletalInputs[localVertex];
    uint i0 = (sv.BoneIndicesPacked >>  0) & 0xFFu;
    uint i1 = (sv.BoneIndicesPacked >>  8) & 0xFFu;
    uint i2 = (sv.BoneIndicesPacked >> 16) & 0xFFu;
    uint i3 = (sv.BoneIndicesPacked >> 24) & 0xFFu;
    float4 w  = sv.BoneWeights;
    float4 bp = float4(sv.BindPosition, 1.0f);

    float3 currObjPos = SkinPosFourBoneInternal(SkeletalCurrBones, boneBase, i0, i1, i2, i3, w, bp);
    float3 prevObjPos = SkinPosFourBoneInternal(SkeletalPrevBones, boneBase, i0, i1, i2, i3, w, bp);

    float3 currObjNormal  = normalize(SkinDirFourBoneInternal(SkeletalCurrBones, boneBase, i0, i1, i2, i3, w, input.normal));
    float3 currObjTangent = SkinDirFourBoneInternal(SkeletalCurrBones, boneBase, i0, i1, i2, i3, w, input.tangent);
    currObjTangent = normalize(currObjTangent - currObjNormal * dot(currObjNormal, currObjTangent));

    VertexObjSpace v;
    v.currObjPos     = currObjPos;
    v.currObjNormal  = currObjNormal;
    v.currObjTangent = currObjTangent;
    v.prevObjPos     = prevObjPos;
    v.uv             = sv.UV;
    v.worldMatrix    = WorldMatrix;
    v.prevWorldMatrix = WorldMatrix;
    v.instanceId     = charIndex;
    return v;
}

#ifdef GBUFFER_HAS_CLUSTER
// VS-inline (cluster, desktop-only): same skin as VS-inline per-mesh, but
// world matrix comes from SkeletalInstanceTransforms[SV_InstanceID] and
// charIndex == instanceId (no SkeletalCharIndex CB dependency).
VertexObjSpace LoadVertex_SkeletalCl(VSInput input, uint vertexId, uint instanceId)
{
    const uint charIndex = instanceId;
    const uint boneBase  = charIndex * SkeletalBoneCount;

    SkinInputVertex_t sv = SkeletalInputs[vertexId];
    uint i0 = (sv.BoneIndicesPacked >>  0) & 0xFFu;
    uint i1 = (sv.BoneIndicesPacked >>  8) & 0xFFu;
    uint i2 = (sv.BoneIndicesPacked >> 16) & 0xFFu;
    uint i3 = (sv.BoneIndicesPacked >> 24) & 0xFFu;
    float4 w  = sv.BoneWeights;
    float4 bp = float4(sv.BindPosition, 1.0f);

    float3 currObjPos = SkinPosFourBoneInternal(SkeletalCurrBones, boneBase, i0, i1, i2, i3, w, bp);
    float3 prevObjPos = SkinPosFourBoneInternal(SkeletalPrevBones, boneBase, i0, i1, i2, i3, w, bp);

    float3 currObjNormal  = normalize(SkinDirFourBoneInternal(SkeletalCurrBones, boneBase, i0, i1, i2, i3, w, input.normal));
    float3 currObjTangent = SkinDirFourBoneInternal(SkeletalCurrBones, boneBase, i0, i1, i2, i3, w, input.tangent);
    currObjTangent = normalize(currObjTangent - currObjNormal * dot(currObjNormal, currObjTangent));

    float4x4 worldMatrix = BuildWorldMatrixFromMat3x4Cluster(SkeletalInstanceTransforms[charIndex]);

    VertexObjSpace v;
    v.currObjPos     = currObjPos;
    v.currObjNormal  = currObjNormal;
    v.currObjTangent = currObjTangent;
    v.prevObjPos     = prevObjPos;
    v.uv             = sv.UV;
    v.worldMatrix    = worldMatrix;
    v.prevWorldMatrix = worldMatrix; // prev-frame per-instance world TBD
    v.instanceId     = charIndex;
    return v;
}
#endif // GBUFFER_HAS_CLUSTER

// =====================================================================
// Layer 2: Deformation pipeline
//
// Phase 1: identity. Add per-effect transforms in subsequent commits;
// each effect should transform BOTH currObjPos and prevObjPos so motion
// vectors stay correct.
// =====================================================================

// Global wind sway. Time-based oscillation in the wind direction with a
// per-blade phase shift derived from the blade's base XZ position so
// adjacent blades aren't synced. Strength scales with heightRatio² so
// roots stay put and tips sway most. Active when WindParams.w > 0.
//
// Returns the deformed object-space position. The caller is expected to
// invoke this both for the current frame's time and (time - dt) for the
// previous frame so motion vectors track the sway accurately.
float3 ApplyWindSway(float3 objPos, float time, float maxBladeHeight)
{
    if (WindParams.w <= 0.0f)
        return objPos;

    float heightRatio = saturate(objPos.y / max(maxBladeHeight, 1e-3f));
    if (heightRatio <= 0.0f)
        return objPos;

    const float tempFreq  = WindTuning.x;  // primary temporal frequency (rad/s)
    const float spaceFreq = WindTuning.y;  // primary spatial frequency  (rad/world-unit)

    // Two-octave sway: the primary swing dominates (this is what gives
    // the "every 2 s" cycle), the secondary is a faint per-blade ripple
    // so neighbours aren't perfectly synced. Secondary uses a *spatial*
    // offset only (no extra temporal multiplier) — multiplying time
    // again would make it buzz and undo the slow primary.
    const float basePhase     = time * tempFreq + objPos.x * spaceFreq + objPos.z * spaceFreq * 0.7f;
    const float primarySway   = sin(basePhase);
    const float secondarySway = sin(basePhase + objPos.x * spaceFreq * 0.5f + 1.3f) * 0.15f;

    // Cubic height curve: roots stay nearly still, tips swing wide. This
    // is the key to a natural-looking wind — linear/quadratic still moves
    // the lower portion of the blade visibly.
    const float heightCurve = heightRatio * heightRatio * heightRatio;
    const float swayAmt = (primarySway + secondarySway) * heightCurve * WindParams.w;

    const float2 windXZ = WindParams.xz; // normalized in XZ
    float3 result = objPos;
    result.x += windXZ.x * swayAmt;
    result.z += windXZ.y * swayAmt;
    // Foreshortening: when the tip swings sideways its Y dips because
    // the blade's arc-length is fixed. Scales with heightCurve so the
    // droop concentrates at the tip.
    result.y -= heightCurve * abs(swayAmt) * 0.2f;
    return result;
}

// Grass-blade bend deformation. The grass mesh is generated with each
// blade rooted at Y=0 and growing up to Y≈GrassBendParams.y in
// object-space. As the player approaches, blade tips bend AWAY from the
// player position in the XZ plane. Roots stay put. Bend strength scales
// with (heightRatio² × falloffByDistance).
//
// Activated by `bGrassMesh` (per-mesh flag). Other meshes (the ground,
// the dungeon character) ignore the deformation even when the CB is
// configured.
float3 ApplyGrassBend(float3 objPos, float3 worldOrigin, float4x4 worldMatrix, float bendStrength, float bendRadius, float maxBladeHeight)
{
    // Convert blade vertex to world space so we can compare against the
    // world-space player origin.
    float3 worldPos = mul(float4(objPos, 1.0f), worldMatrix).xyz;
    float2 toBlade  = worldPos.xz - worldOrigin.xz;
    float  distSq   = dot(toBlade, toBlade);
    float  radiusSq = bendRadius * bendRadius;
    if (distSq >= radiusSq || distSq <= 1e-6f)
        return objPos;

    float dist = sqrt(distSq);
    float falloff = 1.0f - dist / bendRadius;          // 1 at player, 0 at radius edge
    falloff = falloff * falloff;                        // softer near edge

    float heightRatio = saturate(objPos.y / max(maxBladeHeight, 1e-3f));
    float bendAmt = heightRatio * heightRatio * falloff * bendStrength;

    // Bend direction: away from the player, in the XZ plane.
    float2 bendDirXZ = toBlade / dist;
    // Object-space ground plane assumes world XZ ≈ object XZ for an
    // identity world matrix (which the grass mesh is built with). Apply
    // the offset directly to currObjPos.xz.
    float3 result = objPos;
    result.x += bendDirXZ.x * bendAmt;
    result.z += bendDirXZ.y * bendAmt;
    // Push the tip down a touch so it really lies over instead of
    // sliding sideways.
    result.y -= heightRatio * heightRatio * falloff * bendStrength * 0.5f;
    return result;
}

VertexObjSpace ApplyVertexDeformations(VertexObjSpace v)
{
    // Grass bend + wind sway compose additively on the same blade mesh.
    // The mesh signals "this is grass" by setting bGrassMesh in the CB
    // for that draw. We deliberately do NOT key off bone-count or
    // similar — the deformation needs to apply only to vertex-animated
    // grass meshes, never to ground / skeletal characters / Spine
    // sprites etc. The host code that submits the grass draw sets the
    // flag; everything else leaves it at 0.
    if (bGrassMesh != 0u)
    {
        const float time           = MeshDeformParams.x;
        const float maxBladeHeight = GrassBendParams.y;

        // Layer 2a — global wind sway (always-on when WindParams.w > 0).
        // Prev frame uses (time - dt) so motion vectors track the sway
        // even when nothing else moves.
        const float prevDt = 1.0f / 60.0f;
        v.currObjPos = ApplyWindSway(v.currObjPos, time,         maxBladeHeight);
        v.prevObjPos = ApplyWindSway(v.prevObjPos, time - prevDt, maxBladeHeight);

        // Layer 2b — player-proximity bend on top of the wind base pose.
        if (GrassBendOrigin.w > 0.0f)
        {
            const float3 worldOrigin  = GrassBendOrigin.xyz;
            const float  bendStrength = GrassBendOrigin.w;
            const float  bendRadius   = GrassBendParams.x;
            v.currObjPos = ApplyGrassBend(v.currObjPos, worldOrigin, v.worldMatrix,    bendStrength, bendRadius, maxBladeHeight);
            v.prevObjPos = ApplyGrassBend(v.prevObjPos, worldOrigin, v.prevWorldMatrix, bendStrength, bendRadius, maxBladeHeight);
        }
    }
    return v;
}

// =====================================================================
// Layer 3: Final transform → PSInput
// =====================================================================

PSInput BuildPSInput(VertexObjSpace v)
{
    PSInput result;
    float4 worldPos = mul(float4(v.currObjPos, 1.0f), v.worldMatrix);
    result.position           = mul(worldPos, ViewProjectionMatrix);
    result.unjitteredPosition = mul(worldPos, UnjitteredViewProjMat);

    float4 prevWorldPos = mul(float4(v.prevObjPos, 1.0f), v.prevWorldMatrix);
    result.prevPosition       = mul(prevWorldPos, PrevUnjitteredViewProjMat);

    result.normal             = normalize(mul(float4(v.currObjNormal,  0), v.worldMatrix));
    result.tangent            = normalize(mul(float4(v.currObjTangent, 0), v.worldMatrix));
    result.uv                 = v.uv;
    return result;
}

// =====================================================================
// Layer 4: VS entry points (thin compositions of Layer 1 → 2 → 3)
// =====================================================================

PSInput VSMain(VSInput input)
{
    VertexObjSpace v = LoadVertex_Static(input);
    v = ApplyVertexDeformations(v);
    return BuildPSInput(v);
}

PSInput SpineVSMain(uint vertexId : SV_VertexID)
{
    VertexObjSpace v = LoadVertex_SpineCached(vertexId);
    v = ApplyVertexDeformations(v);
    return BuildPSInput(v);
}

PSInput SpineVsInlineVSMain(uint vertexId : SV_VertexID)
{
    VertexObjSpace v = LoadVertex_SpineInline(vertexId);
    v = ApplyVertexDeformations(v);
    return BuildPSInput(v);
}

PSInput SkeletalVSMain(VSInput input, uint vertexId : SV_VertexID)
{
    VertexObjSpace v = LoadVertex_SkeletalComp(input, vertexId);
    v = ApplyVertexDeformations(v);
    return BuildPSInput(v);
}

PSInput SkeletalVsInlineVSMain(VSInput input, uint vertexId : SV_VertexID)
{
    VertexObjSpace v = LoadVertex_SkeletalIL(input, vertexId);
    v = ApplyVertexDeformations(v);
    return BuildPSInput(v);
}

#ifdef GBUFFER_HAS_CLUSTER
PSInput SkeletalVsInlineClusterVSMain(VSInput input, uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    VertexObjSpace v = LoadVertex_SkeletalCl(input, vertexId, instanceId);
    v = ApplyVertexDeformations(v);
    return BuildPSInput(v);
}
#endif // GBUFFER_HAS_CLUSTER

#endif // GBUFFER_COMMON_HLSLI
