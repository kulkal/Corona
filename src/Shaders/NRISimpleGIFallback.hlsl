#include "Common.hlsl"

RWTexture2D<float4> GIResultColor : register(u0);

Texture2D<float> DepthTex : register(t0);
Texture2D<float4> WorldNormalTex : register(t1);

#define RT_DIFFUSE_GI_MAX_POINT_LIGHTS 16

struct PointLightParam
{
    float4 PositionAndRadius;
    float4 ColorAndIntensity;
    float4 DirectionAndType;
    float4 SpotConeAndFlags;
};

cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4x4 InvProjMatrix;
    float4 ProjectionParams;
    float4 LightDirAndIntensity;
    float2 RandomOffset;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    float ViewSpreadAngle;
    uint NoiseMode;
    uint GISamplesPerPixel;
    uint _paddingAfterGISamples;
    float3 LightColor;
    float _padding;
    PointLightParam PointLights[RT_DIFFUSE_GI_MAX_POINT_LIGHTS];
    uint PointLightCount;
    float3 PointLightPadding;
};

static const float INV_PI = 1.0f / PI;

float3 EvaluatePointLightFill(float3 normal)
{
    float fill = 0.0f;
    uint activeCount = min(PointLightCount, (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS);

    [loop]
    for (uint lightIndex = 0u; lightIndex < RT_DIFFUSE_GI_MAX_POINT_LIGHTS; ++lightIndex)
    {
        if (lightIndex >= activeCount)
            break;

        PointLightParam light = PointLights[lightIndex];
        float3 pointLightDir = CommonSafeNormalize(light.PositionAndRadius.xyz, float3(0.0f, 1.0f, 0.0f));
        float lightIntensity = min(max(CommonSanitizeFloat(light.ColorAndIntensity.w, 0.0f), 0.0f), 2.0f);
        float wrap = saturate(dot(normal, pointLightDir) * 0.5f + 0.5f);
        fill += lightIntensity * wrap;
    }

    return fill.xxx * (0.0006f * INV_PI);
}

[numthreads(8, 8, 1)]
void NRISimpleGIFallback(uint3 DTid : SV_DispatchThreadID)
{
    uint width = 0;
    uint height = 0;
    GIResultColor.GetDimensions(width, height);

    uint2 px = DTid.xy;
    if (px.x >= width || px.y >= height)
        return;

    float deviceDepth = DepthTex[px];
    if (deviceDepth >= 0.99999f)
    {
        GIResultColor[px] = 0.0f.xxxx;
        return;
    }

    float3 worldNormal = CommonSafeNormalize(WorldNormalTex[px].xyz, float3(0.0f, 1.0f, 0.0f));
    float3 lightDir = CommonSafeNormalize(LightDirAndIntensity.xyz, float3(0.0f, 1.0f, 0.0f));

    float directionalFill = saturate(dot(worldNormal, lightDir) * 0.5f + 0.5f);
    float3 irradiance =
        0.018f.xxx +
        directionalFill.xxx * 0.032f;
    irradiance += EvaluatePointLightFill(worldNormal);

    irradiance = min(max(CommonSanitizeFloat3(irradiance, 0.0f.xxx), 0.0f.xxx), 0.12f.xxx);

    GIResultColor[px] = float4(irradiance, 1.0f);
}
