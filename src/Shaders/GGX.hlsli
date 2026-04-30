#ifndef CORONA_GGX_HLSLI
#define CORONA_GGX_HLSLI

float GGXSquare(float x)
{
    return x * x;
}

float3 GGXSafeNormalize(float3 value, float3 fallback)
{
    if (any(isnan(value)) || any(isinf(value)))
        value = fallback;

    float lenSq = dot(value, value);
    if (lenSq > 1e-12f)
        return value * rsqrt(lenSq);

    if (any(isnan(fallback)) || any(isinf(fallback)))
        fallback = float3(0.0f, 1.0f, 0.0f);

    float fallbackLenSq = dot(fallback, fallback);
    return fallbackLenSq > 1e-12f ? fallback * rsqrt(fallbackLenSq) : fallback;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    cosTheta = saturate(cosTheta);
    F0 = saturate(F0);
    return F0 + (1.0f.xxx - F0) * pow(1.0f - cosTheta, 5.0f);
}

float DistributionGGX(float NoH, float roughness)
{
    NoH = saturate(NoH);
    roughness = clamp(roughness, 0.02f, 1.0f);
    float alpha = GGXSquare(roughness);
    float alpha2 = GGXSquare(alpha);
    float denom = NoH * NoH * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / max(PI * denom * denom, 1e-6f);
}

float GeometrySchlickGGX(float NoX, float roughness)
{
    NoX = saturate(NoX);
    roughness = clamp(roughness, 0.02f, 1.0f);
    float r = roughness + 1.0f;
    float k = (r * r) * 0.125f;
    return NoX / max(NoX * (1.0f - k) + k, 1e-6f);
}

float GeometrySmithGGX(float NoV, float NoL, float roughness)
{
    return GeometrySchlickGGX(NoV, roughness) * GeometrySchlickGGX(NoL, roughness);
}

float3 EvaluateGGXSpecularBRDF(float3 N, float3 V, float3 L, float roughness, float3 F0)
{
    N = GGXSafeNormalize(N, float3(0.0f, 1.0f, 0.0f));
    V = GGXSafeNormalize(V, -N);
    L = GGXSafeNormalize(L, N);
    roughness = clamp(roughness, 0.02f, 1.0f);
    F0 = saturate(F0);

    float NoV = saturate(dot(N, V));
    float NoL = saturate(dot(N, L));
    if (NoV <= 0.0f || NoL <= 0.0f)
        return 0.0f.xxx;

    float3 H = GGXSafeNormalize(V + L, N);
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));

    float D = DistributionGGX(NoH, roughness);
    float G = GeometrySmithGGX(NoV, NoL, roughness);
    float3 F = FresnelSchlick(VoH, F0);

    return (D * G * F) / max(4.0f * NoV * NoL, 1e-6f);
}

#endif
