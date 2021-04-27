// https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ToneMapping.hlsl

#include "Common.hlsl"
Texture2D SrcTex: register(t0);
StructuredBuffer<float> Exposure : register( t1 );

SamplerState sampleWrap : register(s0);
cbuffer ToneMapCB : register(b0)
{
    float4 Scale;
    float4 Offset;
    uint ToneMapMode;
    float WhitePoint_Hejl;
    float ShoulderStrength;
    float LinearStrength;
    float LinearAngle;
    float ToeStrength;
    float WhitePoint_Hable;
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

    float4 pos = input.position;

    pos.xy = pos.xy * Scale + Offset.xy;
    result.position = pos;

    result.uv = input.uv;

    return result;
}

float3 LinearTosRGB(in float3 color)
{
    float3 x = color * 12.92f;
    float3 y = 1.055f * pow(saturate(color), 1.0f / 2.4f) - 0.055f;

    float3 clr = color;
    clr.r = color.r < 0.0031308f ? x.r : y.r;
    clr.g = color.g < 0.0031308f ? x.g : y.g;
    clr.b = color.b < 0.0031308f ? x.b : y.b;

    return clr;
}

float3 SRGBToLinear(in float3 color)
{
    float3 x = color / 12.92f;
    float3 y = pow(max((color + 0.055f) / 1.055f, 0.0f), 2.4f);

    float3 clr = color;
    clr.r = color.r <= 0.04045f ? x.r : y.r;
    clr.g = color.g <= 0.04045f ? x.g : y.g;
    clr.b = color.b <= 0.04045f ? x.b : y.b;

    return clr;
}

float3 ToneMapFilmicALU(in float3 color)
{
    color = max(0, color - 0.004f);
    color = (color * (6.2f * color + 0.5f)) / (color * (6.2f * color + 1.7f)+ 0.06f);
    return color;
}


float3 Reinhard(in float3 color)
{
    float Luminance = RGBToLuminance(color);
    return color/(1 + Luminance);
}


float3 HableFunction(in float3 x) {
    const float A = ShoulderStrength;
    const float B = LinearStrength;
    const float C = LinearAngle;
    const float D = ToeStrength;

    // Not exposed as settings
    const float E = 0.01f;
    const float F = 0.3f;

    return ((x * (A * x + C * B)+ D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 ToneMap_Hable(in float3 color) {
    float3 numerator = HableFunction(color);
    float3 denominator = HableFunction(WhitePoint_Hable);

    return LinearTosRGB(numerator / denominator);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    input.uv.y = 1 - input.uv.y;
    float4 SrcColor = SrcTex.Sample(sampleWrap, input.uv) * Exposure[0];
    float3 ToneMapped;
    if(ToneMapMode == 0) 
        ToneMapped = LinearTosRGB(SrcColor);
    else if(ToneMapMode == 1)
        ToneMapped = Reinhard(SrcColor);
    else if(ToneMapMode == 2)
        ToneMapped = ToneMapFilmicALU(SrcColor);
    else if(ToneMapMode == 3)
        ToneMapped = ToneMap_Hable(SrcColor);
	// return SrcColor;
    return float4(ToneMapped, 0);
}