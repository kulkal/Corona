//*********************************************************
// Desktop GBuffer pass.
//
// All VS entries + shared resource bindings live in GBufferCommon.hlsli.
// This file holds only the desktop-specific pixel shader (7 render
// targets, bumpmap normal reconstruction, DLSS RR specular albedo) and
// the cluster-VS gate.
//*********************************************************

#define GBUFFER_HAS_CLUSTER
#include "GBufferCommon.hlsli"

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
}

struct PS_OUTPUT
{
    float4 Albedo          : SV_Target0;
    float4 SpecularAlbedo  : SV_Target1;
    float4 Normal          : SV_Target2;
    float4 GeomNormal      : SV_Target3;
    float2 Velocity        : SV_Target4;
    float4 Material        : SV_Target5;
    float  UnjitteredDepth : SV_Target6;
};

PS_OUTPUT PSMain(PSInput input)
{
    float2 prevPositionSS = (input.prevPosition.xy / input.prevPosition.w) * float2(0.5, -0.5) + 0.5;
    prevPositionSS *= RTSize.xy;

    float prevDepth = input.prevPosition.z / input.prevPosition.w * 0.5 + 0.5;

    float2 positionSS = (input.unjitteredPosition.xy / input.unjitteredPosition.w) * float2(0.5, -0.5) + 0.5;
    positionSS *= RTSize.xy;

    float curDepth = input.unjitteredPosition.z / input.unjitteredPosition.w * 0.5 + 0.5;
    float2 velocity;
    velocity.xy = positionSS - prevPositionSS;

    velocity.xy /= RTSize.xy;

    float4 Albedo    = AlbedoTex.Sample(sampleWrap, input.uv) * BaseColorFactor;
    float  Roughness = RoughnessTex.Sample(sampleWrap, input.uv).x;
    float  Metallic  = MetallicTex.Sample(sampleWrap, input.uv).x;

    if (Albedo.w < 0.1)
        discard;

    float3 WorldNormal = CalcPerPixelNormal(input.uv, input.normal, input.tangent);
    float3 GeomNormal  = CommonSafeNormalize(input.normal, float3(0.0f, 1.0f, 0.0f));
    if (bTwoSidedLighting != 0)
    {
        float3 surfaceToViewForNormal = CommonSafeNormalize(-ViewDir.xyz, WorldNormal);
        if (dot(WorldNormal, surfaceToViewForNormal) < 0.0f)
        {
            WorldNormal = -WorldNormal;
            GeomNormal  = -GeomNormal;
        }
    }

    PS_OUTPUT output;
    output.Albedo.xyz     = Albedo.xyz;
    output.Normal.xyz     = WorldNormal;
    output.GeomNormal.xyz = GeomNormal;
    output.Velocity.xy    = velocity;
    output.UnjitteredDepth = input.unjitteredPosition.z / input.unjitteredPosition.w;

    if (bOverrideRougnessMetallic)
    {
        output.Material.x = clamp(RougnessMetalic.x, 0.02f, 1.0f);
        output.Material.y = saturate(RougnessMetalic.y);
    }
    else
    {
        output.Material.x = clamp(Roughness * RougnessMetalic.x, 0.02f, 1.0f);
        output.Material.y = saturate(Metallic  * RougnessMetalic.y);
    }
    output.Material.z = bUnlitMaterial != 0 ? 1.0f : 0.0f;
    // Material.w = surface-kind flag. 1.0 = grass blade (back-lit SSS in
    // LightingPS), 0.0 = standard opaque. Spine sprites use the unlit
    // path so they don't need a separate flag.
    output.Material.w = bGrassMesh != 0 ? 1.0f : 0.0f;

    float3 surfaceToView = CommonSafeNormalize(-ViewDir.xyz, WorldNormal);
    output.SpecularAlbedo.xyz = ComputeDLSSRRSpecularAlbedo(Albedo.xyz, output.Material.y, output.Material.x, WorldNormal, surfaceToView);
    output.SpecularAlbedo.w = 1.0f;

    return output;
}
