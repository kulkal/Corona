#include "Common.hlsl"

Texture2D SkyLightingRawTex : register(t0);
Texture2D DepthTex : register(t1);
Texture2D GeoNormalTex : register(t2);

RWTexture2D<float4> OutSkyLighting : register(u0);

cbuffer SkyLightingDenoiseCB : register(b0)
{
    float4 ProjectionParams;
    float2 RTSize;
    float DepthSigma;
    float NormalSigma;
    float VisibilitySigma;
    uint Radius;
    float2 _padding;
};

[numthreads(8, 8, 1)]
void SkyLightingDenoiseCS(uint3 DTid : SV_DispatchThreadID)
{
    uint width = (uint)RTSize.x;
    uint height = (uint)RTSize.y;
    if (DTid.x >= width || DTid.y >= height)
        return;

    int2 centerPos = int2(DTid.xy);
    float4 centerSky = SkyLightingRawTex[centerPos];
    float centerDepth = DepthTex[centerPos].x;
    if (centerDepth >= 0.999999f)
    {
        OutSkyLighting[DTid.xy] = centerSky;
        return;
    }

    float centerLinearDepth = GetLinearDepthOpenGL(centerDepth, ProjectionParams.z, ProjectionParams.w);
    float3 centerNormal = CommonSafeNormalize(GeoNormalTex[centerPos].xyz, float3(0.0f, 1.0f, 0.0f));

    float3 sumRadiance = max(centerSky.xyz, 0.0f.xxx);
    float sumVisibility = saturate(centerSky.w);
    float sumW = 1.0f;

    int radius = min((int)Radius, 6);
    float spatialSigma = max((float)radius * 0.55f, 1.0f);
    float invTwoSpatialSigmaSq = rcp(2.0f * spatialSigma * spatialSigma);

    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            if (x == 0 && y == 0)
                continue;

            int2 samplePos = clamp(centerPos + int2(x, y), int2(0, 0), int2((int)width - 1, (int)height - 1));
            float sampleDepth = DepthTex[samplePos].x;
            if (sampleDepth >= 0.999999f)
                continue;

            float4 sampleSky = SkyLightingRawTex[samplePos];
            float sampleLinearDepth = GetLinearDepthOpenGL(sampleDepth, ProjectionParams.z, ProjectionParams.w);
            float3 sampleNormal = CommonSafeNormalize(GeoNormalTex[samplePos].xyz, centerNormal);

            float spatialW = exp(-float(x * x + y * y) * invTwoSpatialSigmaSq);
            float depthDelta = abs(centerLinearDepth - sampleLinearDepth) / max(centerLinearDepth, 1e-3f);
            float depthW = exp(-depthDelta * DepthSigma);
            float normalW = pow(saturate(dot(centerNormal, sampleNormal)), NormalSigma);
            float visibilityW = exp(-abs(saturate(centerSky.w) - saturate(sampleSky.w)) * VisibilitySigma);
            float weight = spatialW * depthW * normalW * visibilityW;

            sumRadiance += max(sampleSky.xyz, 0.0f.xxx) * weight;
            sumVisibility += saturate(sampleSky.w) * weight;
            sumW += weight;
        }
    }

    float invSumW = rcp(max(sumW, 1e-4f));
    OutSkyLighting[DTid.xy] = float4(sumRadiance * invSumW, saturate(sumVisibility * invSumW));
}
