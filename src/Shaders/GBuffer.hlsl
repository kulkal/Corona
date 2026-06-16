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

float3 CalcPerPixelNormal(GBufferMaterialRecord material, float2 vTexcoord, float3 vVertNormal, float3 vVertTangent)
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
    float3 normalSample = SampleGBufferNormal(material, vTexcoord);
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

// Shared GBuffer shading: writes all 7 targets and returns the sampled albedo
// alpha so the caller decides whether to alpha-test. Contains no `discard`, so
// the opaque entry point below can be marked [earlydepthstencil].
PS_OUTPUT GBufferShade(PSInput input, out float outAlbedoAlpha)
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

    uint drawRecordIndex = input.drawRecordIndex;
    float2 roughnessMetallicFactor = GetGBufferRoughnessMetallicFactor(drawRecordIndex);
    GBufferMaterialRecord material = GetGBufferMaterialRecord(drawRecordIndex);
    float4 Albedo    = SampleGBufferAlbedo(material, input.uv) * GetGBufferBaseColorFactor(drawRecordIndex);
    float  Roughness = SampleGBufferRoughness(material, input.uv);
    float  Metallic  = SampleGBufferMetallic(material, input.uv);

    outAlbedoAlpha = Albedo.w;

    float3 WorldNormal = CalcPerPixelNormal(material, input.uv, input.normal, input.tangent);
    float3 GeomNormal  = CommonSafeNormalize(input.normal, float3(0.0f, 1.0f, 0.0f));
    if (GetGBufferTwoSidedLighting(drawRecordIndex) != 0)
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

    if (GetGBufferOverrideRoughnessMetallic(drawRecordIndex) != 0)
    {
        output.Material.x = clamp(roughnessMetallicFactor.x, 0.02f, 1.0f);
        output.Material.y = saturate(roughnessMetallicFactor.y);
    }
    else
    {
        output.Material.x = clamp(Roughness * roughnessMetallicFactor.x, 0.02f, 1.0f);
        output.Material.y = saturate(Metallic  * roughnessMetallicFactor.y);
    }
    output.Material.z = GetGBufferUnlitMaterial(drawRecordIndex) != 0 ? 1.0f : 0.0f;
    // Material.w = surface-kind flag. 1.0 = grass blade (back-lit SSS in
    // LightingPS), 0.0 = standard opaque. Spine sprites use the unlit
    // path so they don't need a separate flag.
    output.Material.w = GetGBufferGrassMesh(drawRecordIndex) != 0 ? 1.0f : 0.0f;

    float3 surfaceToView = CommonSafeNormalize(-ViewDir.xyz, WorldNormal);
    output.SpecularAlbedo.xyz = ComputeDLSSRRSpecularAlbedo(Albedo.xyz, output.Material.y, output.Material.x, WorldNormal, surfaceToView);
    output.SpecularAlbedo.w = 1.0f;

    return output;
}

// Alpha-tested geometry: late-Z so the discard can cut cutout texels.
PS_OUTPUT PSMain(PSInput input)
{
    float albedoAlpha;
    PS_OUTPUT output = GBufferShade(input, albedoAlpha);
    if (albedoAlpha < 0.1)
        discard;
    return output;
}

// Fully-opaque geometry: force early depth test/write so occluded opaque pixels
// are rejected before shading + pixel export, relieving the SM->PROP pixout
// limiter. Only safe because this entry has no discard.
[earlydepthstencil]
PS_OUTPUT PSMainOpaque(PSInput input)
{
    float albedoAlpha;
    return GBufferShade(input, albedoAlpha);
}
