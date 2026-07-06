#define TRANSLUCENT_GUIDE_MAX_PRIMITIVES 32

cbuffer TranslucentPreLightingGuideCB : register(b0)
{
    float4 RenderTargetParams; // xy = inverse render size
    float4 EffectParams;       // x = refraction pixels, y = global alpha, z = alpha draw count
    uint4 PrimitiveParams;     // x = primitive count, y = frame index
    float4 PrimitiveCenterRadius[TRANSLUCENT_GUIDE_MAX_PRIMITIVES]; // xy = top-left UV center, zw = UV radius
    float4 PrimitiveData[TRANSLUCENT_GUIDE_MAX_PRIMITIVES];         // x = alpha, y = shape, z = seed, w = depth
};

struct VSInput
{
    float4 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput result;
    result.position = input.position;
    result.uv = input.uv;
    return result;
}

float EllipseMask(float2 uv, float2 center, float2 radius)
{
    float2 p = (uv - center) / max(radius, 1.0e-4f.xx);
    float d = length(p);
    return smoothstep(1.0f, 0.82f, d);
}

float BoxMask(float2 uv, float2 center, float2 radius)
{
    float2 p = abs((uv - center) / max(radius, 1.0e-4f.xx));
    float d = max(p.x, p.y);
    return smoothstep(1.0f, 0.92f, d);
}

float RoundedBoxMask(float2 uv, float2 center, float2 radius)
{
    float2 p = abs((uv - center) / max(radius, 1.0e-4f.xx));
    float outside = length(max(p - 0.82f.xx, 0.0f.xx)) * 5.0f;
    float inside = max(p.x, p.y);
    return saturate(smoothstep(1.0f, 0.86f, inside) * smoothstep(1.25f, 0.0f, outside));
}

float4 PSMain(PSInput input) : SV_Target0
{
    float2 uv = saturate(float2(input.uv.x, 1.0f - input.uv.y));
    float2 invTargetSize = RenderTargetParams.xy;
    float refractionPixels = max(EffectParams.x, 0.0f);
    uint primitiveCount = min(PrimitiveParams.x, (uint)TRANSLUCENT_GUIDE_MAX_PRIMITIVES);

    float totalMask = 0.0f;
    float2 distortionDir = 0.0f.xx;
    float alphaWeight = 0.0f;

    [loop]
    for (uint i = 0u; i < primitiveCount; ++i)
    {
        float4 centerRadius = PrimitiveCenterRadius[i];
        float4 data = PrimitiveData[i];
        float2 center = centerRadius.xy;
        float2 radius = max(centerRadius.zw, 1.0e-4f.xx);
        float alpha = saturate(data.x);
        float shapeKind = data.y;

        float ellipse = EllipseMask(uv, center, radius);
        float box = BoxMask(uv, center, radius);
        float roundedBox = RoundedBoxMask(uv, center, radius);
        float m = ellipse;
        if (shapeKind > 1.5f)
            m = roundedBox;
        else if (shapeKind > 0.5f)
            m = box;

        float2 radial = uv - center;
        float radialLen = max(length(radial), 1.0e-4f);
        float2 dir = radial / radialLen;
        float seed = data.z + (float)i * 13.0f + (float)PrimitiveParams.y * 0.017f;
        float phase = dot(uv, float2(31.0f + seed * 0.037f, 19.0f + seed * 0.071f)) * 6.2831853f;
        float2 ripple = float2(sin(phase), cos(phase * 0.73f + seed * 0.01f));
        float weight = m * max(alpha, 0.001f);
        distortionDir += weight * (dir * 0.72f + ripple * 0.12f);
        alphaWeight = max(alphaWeight, weight);
        totalMask = max(totalMask, m);
    }

    if (totalMask <= 0.0001f || refractionPixels <= 0.0001f || alphaWeight <= 0.0001f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    distortionDir /= max(alphaWeight * 2.25f, 1.0f);
    float coverageAlpha = saturate(alphaWeight);
    float2 refractedUv = saturate(uv + distortionDir * refractionPixels * invTargetSize * coverageAlpha);
    return float4(refractedUv, coverageAlpha, 1.0f);
}
