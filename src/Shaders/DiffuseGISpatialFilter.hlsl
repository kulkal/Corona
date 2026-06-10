#include "Common.hlsl"

// Variance-guided, edge-aware spatial filter for the SIMPLE_RAYTRACE diffuse GI.
//
// Primary use (default): runs AFTER the screen-space temporal accumulation, just
// before the DLSS-RR feed. DLSS-RR runs in COMBINED mode (one lit-color input, no
// separate diffuse layer / diffuse hit-distance guide), so on freshly-disoccluded
// pixels — which have no temporal history for RR either — it must denoise purely
// spatially, and the dark-scene 1spp diffuse GI noise there exceeds what its combined
// spatial pass removes. Those pixels are exactly the ones the temporal pass could not
// accumulate, so they show high local variance while well-accumulated pixels are
// smooth. We therefore blur ONLY where local variance is high (disoccluded / noisy),
// edge-stopped by depth + geometric normal, and pass accumulated pixels through almost
// untouched (detail preserved). This HELPS RR (cleaner input where it is weakest)
// rather than fighting it (a global pre-blur would erase detail RR relies on).

Texture2D<float4> InGIColor   : register(t0);
Texture2D<float4> InGIAux     : register(t1); // SH/aux (directional) — filtered with the same weights
Texture2D<float>  DepthTex    : register(t2);
Texture2D<float4> NormalTex   : register(t3); // geometric world normal
RWTexture2D<float4> OutGIColor : register(u0);
RWTexture2D<float4> OutGIAux   : register(u1);

cbuffer SpatialFilterConstant : register(b0)
{
    float2 RTSize;
    float2 ProjectionParams;   // z,w for linear depth
    int    Radius;             // filter half-width in pixels (stride 1)
    float  DepthSigma;         // relative depth falloff
    float  NormalPower;        // normal edge-stop sharpness
    float  _pad;
};

float LinearizeDepth(float d)
{
    return GetLinearDepthOpenGL(d, ProjectionParams.x, ProjectionParams.y);
}

[numthreads(8, 8, 1)]
void DiffuseGISpatialFilter(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    uint2 size = uint2(RTSize);
    if (px.x >= size.x || px.y >= size.y)
        return;

    float4 centerColor = InGIColor[px];
    float4 centerAux   = InGIAux[px];

    float centerDeviceDepth = DepthTex[px].x;
    // Background / sky — pass through unfiltered.
    if (centerDeviceDepth >= 0.99999f)
    {
        OutGIColor[px] = centerColor;
        OutGIAux[px]   = centerAux;
        return;
    }

    float  centerLinear = max(LinearizeDepth(centerDeviceDepth), 1e-3f);
    float3 centerNormal = normalize(NormalTex[px].xyz);

    float4 sumColor = 0.0f.xxxx;
    float4 sumAux   = 0.0f.xxxx;
    float  sumW     = 0.0f;

    // Luminance moments over the (edge-stopped) neighborhood -> local noise estimate.
    float  sumLum   = 0.0f;
    float  sumLumSq = 0.0f;
    float  sumLumW  = 0.0f;

    int r = clamp(Radius, 1, 8);
    [loop]
    for (int dy = -r; dy <= r; ++dy)
    {
        [loop]
        for (int dx = -r; dx <= r; ++dx)
        {
            int2 sp = int2(px) + int2(dx, dy);
            if (sp.x < 0 || sp.y < 0 || sp.x >= (int)size.x || sp.y >= (int)size.y)
                continue;

            float sDepth = DepthTex[sp].x;
            if (sDepth >= 0.99999f)
                continue;

            float sLinear = max(LinearizeDepth(sDepth), 1e-3f);
            float3 sNormal = normalize(NormalTex[sp].xyz);

            // Edge-stopping weights: spatial gaussian * depth bilateral * normal.
            float spatial = exp(-float(dx * dx + dy * dy) / max(2.0f * float(r * r) * 0.35f, 1e-3f));
            float depthDelta = abs(centerLinear - sLinear) / (centerLinear * max(DepthSigma, 1e-3f));
            float depthW = exp(-depthDelta * depthDelta);
            float nd = saturate(dot(centerNormal, sNormal));
            float normalW = pow(nd, max(NormalPower, 1.0f));

            float w = spatial * depthW * normalW;
            if (w <= 1e-5f)
                continue;

            float4 sColor = InGIColor[sp];
            sumColor += sColor * w;
            sumAux   += InGIAux[sp] * w;
            sumW     += w;

            float lum = max(dot(sColor.xyz, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
            sumLum   += lum * w;
            sumLumSq += lum * lum * w;
            sumLumW  += w;
        }
    }

    if (sumW <= 1e-5f)
    {
        OutGIColor[px] = centerColor;
        OutGIAux[px]   = centerAux;
        return;
    }

    float4 filteredColor = sumColor / sumW;
    float4 filteredAux   = sumAux   / sumW;

    // Relative local std-dev of luminance: high on noisy/disoccluded pixels (no temporal
    // history -> raw 1spp), low on well-accumulated pixels. Drives how much we filter.
    float meanLum = sumLum / max(sumLumW, 1e-5f);
    float varLum  = max(sumLumSq / max(sumLumW, 1e-5f) - meanLum * meanLum, 0.0f);
    float stdLum  = sqrt(varLum);
    float relStd  = stdLum / max(meanLum, 1e-3f);

    // Below kLow: treat as accumulated -> keep original (preserve detail).
    // Above kHigh: treat as fully noisy -> use the filtered estimate.
    const float kLow  = 0.25f;
    const float kHigh = 0.90f;
    float strength = saturate((relStd - kLow) / (kHigh - kLow));

    OutGIColor[px] = lerp(centerColor, filteredColor, strength);
    OutGIAux[px]   = lerp(centerAux,   filteredAux,   strength);
}
