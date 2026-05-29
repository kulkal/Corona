// Particle.hlsl — camera-facing 2D sprite particles.
//
// CPU builds 4 verts per particle, already expanded along the view-space
// right/up basis so the quad is camera-aligned. The VS only does the world→
// clip transform; the PS samples a procedural sprite texture and modulates by
// the per-vertex color. PSO uses EBlendMode::Additive so the alpha channel
// scales the additive contribution.

Texture2D    ParticleTex   : register(t0);
SamplerState samplerClamp  : register(s0);

cbuffer ParticleCB : register(b0)
{
    float4x4 ViewProj;
};

struct VSInput
{
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

PSInput VSMain(VSInput input)
{
    PSInput result;
    result.position = mul(float4(input.position, 1.0f), ViewProj);
    result.uv = input.uv;
    result.color = input.color;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 tex = ParticleTex.Sample(samplerClamp, input.uv);
    float4 outColor;
    outColor.rgb = tex.rgb * input.color.rgb;
    outColor.a   = tex.a   * input.color.a;
    return outColor;
}
