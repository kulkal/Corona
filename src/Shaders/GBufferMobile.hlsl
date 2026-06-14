//*********************************************************
// Mobile GBuffer pass.
//
// All VS entries + shared resource bindings live in GBufferCommon.hlsli.
// This file holds only the mobile-specific pixel shader (4 render
// targets, packed normal encoding, no bumpmap, no DLSS RR specular)
// and intentionally does NOT define GBUFFER_HAS_CLUSTER — the cluster
// VS + SkeletalInstanceTransforms SBV stay out of the SPIR-V build
// for Adreno.
//*********************************************************

#include "GBufferCommon.hlsli"

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
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
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

    uint drawRecordIndex = input.drawRecordIndex;
    float2 roughnessMetallicFactor = GetGBufferRoughnessMetallicFactor(drawRecordIndex);
    GBufferMaterialRecord material = GetGBufferMaterialRecord(drawRecordIndex);
    float4 Albedo = SampleGBufferAlbedo(material, input.uv) * GetGBufferBaseColorFactor(drawRecordIndex);

    if (Albedo.w < 0.1f)
        discard;

    float3 WorldNormal = SafeNormalizeMobile(input.normal, float3(0.0f, 1.0f, 0.0f));
    if (GetGBufferTwoSidedLighting(drawRecordIndex) != 0)
    {
        float3 surfaceToView = SafeNormalizeMobile(-ViewDir.xyz, WorldNormal);
        if (dot(WorldNormal, surfaceToView) < 0.0f)
            WorldNormal = -WorldNormal;
    }

    PS_OUTPUT output;
    output.Albedo   = float4(Albedo.xyz, 1.0f);
    output.Normal   = float4(WorldNormal * 0.5f + 0.5f, 1.0f);
    output.Velocity = velocity;

    if (GetGBufferOverrideRoughnessMetallic(drawRecordIndex) != 0)
    {
        output.Material.x = clamp(roughnessMetallicFactor.x, 0.02f, 1.0f);
        output.Material.y = saturate(roughnessMetallicFactor.y);
    }
    else
    {
        output.Material.x = clamp(roughnessMetallicFactor.x, 0.02f, 1.0f);
        output.Material.y = 0.0f;
    }
    output.Material.z = GetGBufferUnlitMaterial(drawRecordIndex) != 0 ? 1.0f : 0.0f;
    output.Material.w = 0.0f;

    return output;
}
