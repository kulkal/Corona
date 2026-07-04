#include "Common.hlsl"

RWTexture2D<float4> ColorTex : register(u0);
Texture2D<float> DepthTex : register(t0);

cbuffer FogCB : register(b0)
{
    float4x4 InvViewMatrix;
    float4x4 InvProjMatrix;
    float4 FogColorAndDensity; // rgb = fog color, a = density
    float4 HeightParams;       // x = height, y = falloff, z = start distance, w = max opacity
    float2 RTSize;
    float2 Padding;
};

float3 ReconstructWorldPosition(uint2 pixel, float deviceDepth)
{
    float2 uv = (float2(pixel) + 0.5f) / max(RTSize, 1.0f.xx);
    float2 screenPosition = uv * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;

    float3 viewPosition = GetViewPosition(deviceDepth, screenPosition, InvProjMatrix);
    return mul(float4(viewPosition, 1.0f), InvViewMatrix).xyz;
}

[numthreads(8, 8, 1)]
void DepthHeightFogCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)RTSize.x || pixel.y >= (uint)RTSize.y)
        return;

    float deviceDepth = DepthTex.Load(int3(pixel, 0)).x;
    if (deviceDepth >= 0.999999f)
        return;

    float3 worldPosition = ReconstructWorldPosition(pixel, deviceDepth);
    float3 cameraPosition = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
    float viewDistance = length(worldPosition - cameraPosition);

    float density = max(FogColorAndDensity.a, 0.0f);
    float startDistance = max(HeightParams.z, 0.0f);
    float maxOpacity = saturate(HeightParams.w);
    float height = HeightParams.x;
    float heightFalloff = max(HeightParams.y, 0.0f);

    float depthDistance = max(viewDistance - startDistance, 0.0f);
    float heightDensity = exp(-max(worldPosition.y - height, 0.0f) * heightFalloff);
    float fogAmount = saturate(1.0f - exp(-density * depthDistance * heightDensity)) * maxOpacity;

    float4 src = ColorTex[pixel];
    float3 fogColor = max(FogColorAndDensity.rgb, 0.0f.xxx);
    src.rgb = lerp(src.rgb, fogColor, fogAmount);
    ColorTex[pixel] = src;
}
