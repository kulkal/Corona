//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
Texture2D CurrentColorTex : register(t0);
Texture2D PrevColorTex : register(t1);
Texture2D VelocityTex : register(t2);
Texture2D DepthTex : register(t3);
Texture2D BloomTex : register(t4);
StructuredBuffer<float> Exposure : register( t5 );


SamplerState sampleWrap : register(s0);


cbuffer TemporalAAParam : register(b0)
{
    float2 RTSize;
    float TAABlendFactor;
    uint ClampMode;
    float BloomStrength;
    uint HistoryValid;
    float2 CurrentJitter;
};

struct VSInput
{
    float4 position : POSITION;
    float2 uv : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

PSInput VSMain(
    VSInput input)
{
    PSInput result;

    result.position = input.position;
    result.uv = input.uv;

    return result;
}

static const float Pi = 3.14159265359;
static const float CubicB = 0.33f;
static const float CubicC = 0.33f;
static const float SkyDepthThreshold = 0.999999f;
// All filtering functions assume that 'x' is normalized to [0, 1], where 1 == FilteRadius
float FilterBox(in float x)
{
    return x <= 1.0f;
}

static float FilterTriangle(in float x)
{
    return saturate(1.0f - x);
}

static float FilterGaussian(in float x)
{
    float GaussianSigma = 0.5f;

    const float sigma = GaussianSigma;
    const float g = 1.0f / sqrt(2.0f * 3.14159f * sigma * sigma);
    return (g * exp(-(x * x) / (2 * sigma * sigma)));
}

 float FilterCubic(in float x, in float B, in float C)
{
    float y = 0.0f;
    float x2 = x * x;
    float x3 = x * x * x;
    if(x < 1)
        y = (12 - 9 * B - 6 * C) * x3 + (-18 + 12 * B + 6 * C) * x2 + (6 - 2 * B);
    else if (x <= 2)
        y = (-B - 6 * C) * x3 + (6 * B + 30 * C) * x2 + (-12 * B - 48 * C) * x + (8 * B + 24 * C);

    return y / 6.0f;
}

float FilterSinc(in float x, in float filterRadius)
{
    float s;

    x *= filterRadius * 2.0f;

    if(x < 0.001f)
        s = 1.0f;
    else
        s = sin(x * Pi) / (x * Pi);

    return s;
}

float FilterBlackmanHarris(in float x)
{
    x = 1.0f - x;

    const float a0 = 0.35875f;
    const float a1 = 0.48829f;
    const float a2 = 0.14128f;
    const float a3 = 0.01168f;
    return saturate(a0 - a1 * cos(Pi * x) + a2 * cos(2 * Pi * x) - a3 * cos(3 * Pi * x));
}

float FilterSmoothstep(in float x)
{
    return 1.0f - smoothstep(0.0f, 1.0f, x);
}

float Filter(in float x, in int filterType, in float filterRadius, in bool rescaleCubic)
{
    // Cubic filters naturually work in a [-2, 2] domain. For the resolve case we
    // want to rescale the filter so that it works in [-1, 1] instead
    float cubicX = rescaleCubic ? x * 2.0f : x;

    if(filterType == 0)
        return FilterBox(x);
    else if(filterType == 1)
        return FilterTriangle(x);
    else if(filterType == 2)
        return FilterGaussian(x);
    else if(filterType == 3)
        return FilterBlackmanHarris(x);
    else if(filterType == 4)
        return FilterSmoothstep(x);
    else if(filterType == 5)
        return FilterCubic(cubicX, 1.0, 0.0f);
    else if(filterType == 6)
        return FilterCubic(cubicX, 0, 0.5f);
    else if(filterType == 7)
        return FilterCubic(cubicX, 1 / 3.0f, 1 / 3.0f);
    else if(filterType == 8)
        return FilterCubic(cubicX, CubicB, CubicC);
    else if(filterType == 9)
        return FilterSinc(x, filterRadius);
    else
        return 1.0f;
}

float3 ClipAABB(float3 aabbMin, float3 aabbMax, float3 prevSample, float3 avg)
{
    #if 1
        // note: only clips towards aabb center (but fast!)
        float3 p_clip = 0.5 * (aabbMax + aabbMin);
        float3 e_clip = 0.5 * (aabbMax - aabbMin);

        float3 v_clip = prevSample - p_clip;
        float3 v_unit = v_clip.xyz / e_clip;
        float3 a_unit = abs(v_unit);
        float ma_unit = max(a_unit.x, max(a_unit.y, a_unit.z));

        if (ma_unit > 1.0)
            return p_clip + v_clip / ma_unit;
        else
            return prevSample;// point inside aabb
    #else
        float3 r = prevSample - avg;
        float3 rmax = aabbMax - avg.xyz;
        float3 rmin = aabbMin - avg.xyz;

        const float eps = 0.000001f;

        if (r.x > rmax.x + eps)
            r *= (rmax.x / r.x);
        if (r.y > rmax.y + eps)
            r *= (rmax.y / r.y);
        if (r.z > rmax.z + eps)
            r *= (rmax.z / r.z);

        if (r.x < rmin.x - eps)
            r *= (rmin.x / r.x);
        if (r.y < rmin.y - eps)
            r *= (rmin.y / r.y);
        if (r.z < rmin.z - eps)
            r *= (rmin.z / r.z);

        return avg + r;
    #endif
}

#define VARIANCE_CLIP 0
#define RGB_CLAMP 1

float4 PSMain(PSInput input) : SV_TARGET
{
    const float MinHistoryWeight = 0.35f;
    float3 clrMin = 99999999.0f;
    float3 clrMax = -99999999.0f;
    float3 m1 = 0.0f;
    float3 m2 = 0.0f;
    float mWeight = 0.0f;
    float3 currentFilteredColor = 0.0f;
    float currentFilterWeight = 0.0f;

    input.uv.y = 1 - input.uv.y;
    uint2 PixelPos = uint2(input.position.xy);
    float2 PixelCenter = float2(PixelPos) + 0.5f;
    float2 CurrentSampleCenter = PixelCenter + CurrentJitter;
    float CenterDepth = DepthTex[PixelPos].x;
    bool bCenterSky = CenterDepth >= SkyDepthThreshold;


    const int SampleRadius_ = 1;

    const float filterRadius = 2.0f / 2.0f;

	const float ResolveFilterDiameter = 2.0f;

	// neighborhood clamping of Playdead Inside.
    for(int y = -SampleRadius_; y <= SampleRadius_; ++y)
    {
        for(int x = -SampleRadius_; x <= SampleRadius_; ++x)
        {
            float2 sampleOffset = float2(x, y);
            float2 samplePos = CurrentSampleCenter + sampleOffset;
            samplePos = clamp(samplePos, 0.5f.xx, RTSize - 0.5f.xx);
            float2 sampleUV = samplePos / RTSize;
            uint2 samplePixel = uint2(samplePos);
            bool bSampleSky = DepthTex[samplePixel].x >= SkyDepthThreshold;
            if (bSampleSky != bCenterSky)
                continue;

            float2 sampleDist = abs(sampleOffset) / (ResolveFilterDiameter / 2.0f);

            float3 sample = CurrentColorTex.SampleLevel(sampleWrap, sampleUV, 0).xyz * Exposure[0];
            float sampleWeight = FilterBlackmanHarris(sampleDist.x) * FilterBlackmanHarris(sampleDist.y);
          
            clrMin = min(clrMin, sample);
            clrMax = max(clrMax, sample);

            m1 += sample;
            m2 += sample * sample;
            mWeight += 1.0f;
            currentFilteredColor += sample * sampleWeight;
            currentFilterWeight += sampleWeight;
        }
    }

    if (currentFilterWeight <= 1.0e-5f || mWeight <= 0.0f)
    {
        float3 centerSample = CurrentColorTex[PixelPos].xyz * Exposure[0];
        clrMin = centerSample;
        clrMax = centerSample;
        m1 = centerSample;
        m2 = centerSample * centerSample;
        mWeight = 1.0f;
        currentFilteredColor = centerSample;
        currentFilterWeight = 1.0f;
    }

    float2 Velocity = VelocityTex[PixelPos];
    float3 Bloom = BloomTex.SampleLevel(sampleWrap, input.uv, 0);
    float3 CurrentColor = currentFilteredColor / max(currentFilterWeight, 1e-5f);
    CurrentColor += Bloom * BloomStrength;

    if (bCenterSky)
        return float4(CurrentColor, 1);

    float2 PrevPixelPos = PixelCenter - Velocity * RTSize;
    float2 PrevUV = (PrevPixelPos + 0.0f) / RTSize;

    if (HistoryValid == 0 || any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
        return float4(CurrentColor, 1);

    float3 PrevColor = PrevColorTex.SampleLevel( sampleWrap, PrevUV, 0);


    if(ClampMode == 0)
        PrevColor = clamp(PrevColor, clrMin, clrMax);
    else if(ClampMode == 1)
    	PrevColor = ClipAABB(clrMin, clrMax, PrevColor, m1 / mWeight);
    else if(ClampMode == 2)
    {
        const float VarianceClipGamma = 1.50f;

        float3 mu = m1 / mWeight;
        float3 sigma = sqrt(abs(m2 / mWeight - mu * mu));
        float3 minc = mu - VarianceClipGamma * sigma;
        float3 maxc = mu + VarianceClipGamma * sigma;
        PrevColor = ClipAABB(minc, maxc, PrevColor, mu);
    }   

    float3 neighborhoodExtent = abs(clrMax - clrMin);
    float3 normalizedExtent = neighborhoodExtent / max(abs(CurrentColor), 1e-3.xxx);
    float instability = saturate(max(normalizedExtent.x, max(normalizedExtent.y, normalizedExtent.z)) * 0.35f);

    float historyWeight = saturate(1.0f - TAABlendFactor);
    historyWeight *= lerp(1.0f, 0.6f, instability);
    historyWeight = max(historyWeight, MinHistoryWeight);

    float currentWeight = 1.0f - historyWeight;
    float3 resolvedColor = CurrentColor * currentWeight + PrevColor * historyWeight;

    return float4(resolvedColor, 1);
}
