// https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ToneMapping.hlsl

#include "Common.hlsl"
Texture2D SrcTex: register(t0);
Texture2D BloomTex : register(t1);

SamplerState sampleWrap : register(s0);
cbuffer AddBloomCB : register(b0)
{
    float4 Scale;
    float4 Offset;
    float BloomStrength;
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

float4 PSMain(PSInput input) : SV_TARGET
{
    input.uv.y = 1 - input.uv.y;
    float4 SrcColor = SrcTex.Sample(sampleWrap, input.uv) ;
    float3 Bloom = BloomTex.SampleLevel( sampleWrap, input.uv, 0);
    float3 CurrentColor =  SrcColor  + Bloom * BloomStrength;
    return float4(CurrentColor, 0);
}