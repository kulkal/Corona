#include "Common.hlsl"
 
//[[vk::binding(0, VULKAN_SRV_SPACE)]] Texture2D AlbedoTex : register(t0);

//[[vk::binding(0, VULKAN_SAMPLER_SPACE)]] SamplerState sampleWrap : register(s0);

[[vk::binding(0, VULKAN_UNIFORM_SPACE)]] cbuffer SimpleDrawCB : register(b0)
{
    float4x4 ViewProjectionMatrix;
    float4x4 WorldMatrix;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;     
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput result;
	float4 worldPos = mul(float4(input.position, 1.0f), WorldMatrix);
    result.position = mul(worldPos, ViewProjectionMatrix);
	result.normal = normalize(mul(float4(input.normal, 0), WorldMatrix));
    result.tangent = normalize(mul(float4(input.tangent, 0), WorldMatrix));
    result.uv = input.uv;
	
    return result;
}



// float3 CalcPerPixelNormal(float2 vTexcoord, float3 vVertNormal, float3 vVertTangent)
// {
//     vVertNormal = normalize(vVertNormal);
//     vVertTangent = normalize(vVertTangent);

//     float3 vVertBinormal = normalize(cross(vVertTangent, vVertNormal));
//     float3x3 TBN = (float3x3(vVertTangent, vVertBinormal, vVertNormal));

// 	// Compute per-pixel normal.
//     float3 vBumpNormal = (float3) NormalTex.Sample(sampleWrap, vTexcoord);

//     vBumpNormal = 2.0f * vBumpNormal - 1.0f;

//     return mul(vBumpNormal, TBN);
//     //return vVertNormal;
// }

struct PS_OUTPUT
{
    float4 Color : SV_Target0;
};


PS_OUTPUT PSMain(PSInput input) : SV_TARGET
{
    //float4 Albedo = AlbedoTex.Sample(sampleWrap, input.uv);

    //if(Albedo.w < 0.1)
        //discard;

    // float3 WorldNormal = CalcPerPixelNormal(input.uv, input.normal, input.tangent);
	
    PS_OUTPUT output;
    //output.Color.xyz = Albedo.xyz;
    output.Color.xyz = float3(1, 0, 0);


    return output;
}