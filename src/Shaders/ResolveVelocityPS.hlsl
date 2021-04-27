#include "Common.hlsl"
Texture2D SrcTex: register(t0);

SamplerState sampleWrap : register(s0);
cbuffer ResolveVelocityCB : register(b0)
{
    float2 RTSize;
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

    result.position = pos;

    result.uv = input.uv;

    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    input.uv.y = 1 - input.uv.y;
    float2 Velocity = -SrcTex.Sample(sampleWrap, input.uv) * RTSize;
    return float4(Velocity, 0, 0);
}