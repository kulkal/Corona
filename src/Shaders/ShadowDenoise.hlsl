#include "Common.hlsl"

Texture2D ShadowTex : register(t0);
Texture2D DepthTex : register(t1);
Texture2D GeoNormalTex : register(t2);

RWTexture2D<float4> OutShadow : register(u0);

cbuffer ShadowDenoiseCB : register(b0)
{
    float4 ProjectionParams;
    float2 RTSize;
    float DepthSigma;
    float NormalSigma;
};

[numthreads(8, 8, 1)]
void ShadowDenoiseCS(uint3 DTid : SV_DispatchThreadID)
{
    uint width = (uint)RTSize.x;
    uint height = (uint)RTSize.y;
    if (DTid.x >= width || DTid.y >= height)
        return;

    int2 centerPos = int2(DTid.xy);
    float centerShadow = ShadowTex[centerPos].x;
    float centerDepth = DepthTex[centerPos].x;
    float centerLinearDepth = GetLinearDepthOpenGL(centerDepth, ProjectionParams.z, ProjectionParams.w);
    float3 centerNormal = normalize(GeoNormalTex[centerPos].xyz);

    float sum = centerShadow;
    float sumW = 1.0f;

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            if (x == 0 && y == 0)
                continue;

            int2 samplePos = clamp(centerPos + int2(x, y), int2(0, 0), int2((int)width - 1, (int)height - 1));
            float sampleShadow = ShadowTex[samplePos].x;
            float sampleDepth = DepthTex[samplePos].x;
            float sampleLinearDepth = GetLinearDepthOpenGL(sampleDepth, ProjectionParams.z, ProjectionParams.w);
            float3 sampleNormal = normalize(GeoNormalTex[samplePos].xyz);

            float spatialW = exp(-0.5f * float(x * x + y * y));
            float depthDelta = abs(centerLinearDepth - sampleLinearDepth) / max(centerLinearDepth, 1e-3f);
            float depthW = exp(-depthDelta * DepthSigma);
            float normalW = pow(saturate(dot(centerNormal, sampleNormal)), NormalSigma);
            float weight = spatialW * depthW * normalW;

            sum += sampleShadow * weight;
            sumW += weight;
        }
    }

    float filtered = sum / max(sumW, 1e-4f);
    OutShadow[DTid.xy] = float4(filtered.xxx, 1.0f);
}
