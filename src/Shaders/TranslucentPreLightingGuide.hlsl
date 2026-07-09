cbuffer TranslucentPreLightingMeshGuideCB : register(b0)
{
    float4x4 WorldViewMatrix;
    float4x4 WorldViewProjectionMatrix;
    float4x4 NormalWorldViewMatrix;
    float4 BaseColorFactor;
    float4 RenderTargetParams;
    float4 EffectParams;
    uint4 StochasticParams;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL0;
    float2 uv       : TEXCOORD0;
    float3 tangent  : TANGENT0;
};

struct PSInput
{
    float4 position   : SV_POSITION;
    float3 viewPos    : TEXCOORD0;
    float3 viewNormal : TEXCOORD1;
};


PSInput VSMain(VSInput input)
{
    PSInput result;
    result.position = mul(float4(input.position, 1.0f), WorldViewProjectionMatrix);
    result.viewPos = mul(float4(input.position, 1.0f), WorldViewMatrix).xyz;
    result.viewNormal = normalize(mul(float4(input.normal, 0.0f), NormalWorldViewMatrix).xyz);
    return result;
}

uint HashUint(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float Random01(uint2 pixel, uint frameIndex, uint drawSeed)
{
    uint h = pixel.x * 0x9e3779b9u;
    h ^= pixel.y * 0x85ebca6bu;
    h ^= frameIndex * 0xc2b2ae35u;
    h ^= drawSeed;
    return (HashUint(h) & 0x00ffffffu) * (1.0f / 16777216.0f);
}

float2 SampleConcentricDisk(float2 randomSample)
{
    float2 offset = randomSample * 2.0f - 1.0f;
    if (abs(offset.x) < 1.0e-6f && abs(offset.y) < 1.0e-6f)
        return float2(0.0f, 0.0f);

    float radius;
    float theta;
    if (abs(offset.x) > abs(offset.y))
    {
        radius = offset.x;
        theta = 0.78539816339f * (offset.y / offset.x);
    }
    else
    {
        radius = offset.y;
        theta = 1.57079632679f - 0.78539816339f * (offset.x / offset.y);
    }
    return radius * float2(cos(theta), sin(theta));
}

float4 PSMain(PSInput input) : SV_Target0
{
    float alpha = saturate(BaseColorFactor.a);
    float2 invTargetSize = RenderTargetParams.xy;
    float2 screenUv = saturate(input.position.xy * invTargetSize);
    float3 viewNormal = normalize(input.viewNormal);
    float3 viewDir = normalize(-input.viewPos);
    if (dot(viewNormal, viewDir) < 0.0f)
        viewNormal = -viewNormal;

    float noV = saturate(dot(viewNormal, viewDir));
    float grazing = 1.0f - noV;
    float2 normalOffset = float2(viewNormal.x, -viewNormal.y);

    float refractionPixels = max(EffectParams.x, 0.0f);
    float2 refractionDeltaUv = normalOffset * refractionPixels * invTargetSize * (0.35f + 0.65f * grazing);

    float glassRoughness = saturate(EffectParams.y);
    if (glassRoughness > 1.0e-4f)
    {
        uint2 stochasticPixel = uint2(max(input.position.xy, 0.0f.xx));
        float2 refractionRandom = float2(
            Random01(stochasticPixel, StochasticParams.y, StochasticParams.z ^ 0x68bc21ebu),
            Random01(stochasticPixel, StochasticParams.y, StochasticParams.z ^ 0x02e5be93u));
        float2 roughnessDisk = SampleConcentricDisk(refractionRandom);
        float roughnessRadiusPixels =
            glassRoughness * glassRoughness *
            lerp(12.0f, 48.0f, grazing);

        // AdditiveAlpha multiplies RGB by source alpha. Compensate here so a
        // single glass layer keeps the requested stochastic footprint instead
        // of shrinking it by the visual opacity control.
        refractionDeltaUv +=
            roughnessDisk * roughnessRadiusPixels * invTargetSize /
            max(alpha, 0.05f);
    }

    // PSO uses AdditiveAlpha blending: RGB is multiplied by source alpha and
    // alpha is additively accumulated. Store a composable offset rather than
    // an absolute UV so overlapping translucent meshes do not overwrite each
    // other like opaque layers.
    return float4(refractionDeltaUv, 1.0f, alpha);
}
