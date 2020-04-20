#include "Common.hlsl"


RWTexture2D<float4> DstTex : register(u0);
RWTexture2D<uint> LumaResult : register(u1);

Texture2D SrcTex: register(t0);
StructuredBuffer<float> Exposure : register( t1 );

SamplerState sampleWrap : register(s0);

cbuffer BloomCB : register(b0)
{
    float2 BlurDirection;
    float2 RTSize;
    uint NumSamples;
   	float WeightScale;
   	float NormalizationScale;
   	float BloomThreshHold;
   	// float Exposure;
   	// float MinLog;
   	// float RcpLogRange;
};



[numthreads(32, 32, 1)]
void BloomExtract( uint3 DTid : SV_DispatchThreadID)
{
    float2 PixelPos = DTid.xy;

 	float2 uv = (DTid.xy + 0.5) / RTSize;
    float2 offset = 1.0/RTSize * 0.25;

    // Use 4 bilinear samples to guarantee we don't undersample when downsizing by more than 2x
    float3 color1 = SrcTex.SampleLevel( sampleWrap, uv + float2(-offset.x, -offset.y), 0 );
    float3 color2 = SrcTex.SampleLevel( sampleWrap, uv + float2( offset.x, -offset.y), 0 );
    float3 color3 = SrcTex.SampleLevel( sampleWrap, uv + float2(-offset.x,  offset.y), 0 );
    float3 color4 = SrcTex.SampleLevel( sampleWrap, uv + float2( offset.x,  offset.y), 0 );

    float luma1 = RGBToLuminance(color1);
    float luma2 = RGBToLuminance(color2);
    float luma3 = RGBToLuminance(color3);
    float luma4 = RGBToLuminance(color4);

    const float kSmallEpsilon = 0.0001;

    float ScaledThreshold = BloomThreshHold * 1 / Exposure[1];    // BloomThreshold / Exposure

    // We perform a brightness filter pass, where lone bright pixels will contribute less.
    color1 *= max(kSmallEpsilon, luma1 - ScaledThreshold) / (luma1 + kSmallEpsilon);
    color2 *= max(kSmallEpsilon, luma2 - ScaledThreshold) / (luma2 + kSmallEpsilon);
    color3 *= max(kSmallEpsilon, luma3 - ScaledThreshold) / (luma3 + kSmallEpsilon);
    color4 *= max(kSmallEpsilon, luma4 - ScaledThreshold) / (luma4 + kSmallEpsilon);

    // The shimmer filter helps remove stray bright pixels from the bloom buffer by inversely weighting
    // them by their luminance.  The overall effect is to shrink bright pixel regions around the border.
    // Lone pixels are likely to dissolve completely.  This effect can be tuned by adjusting the shimmer
    // filter inverse strength.  The bigger it is, the less a pixel's luminance will matter.
    const float kShimmerFilterInverseStrength = 1.0f;
    float weight1 = 1.0f / (luma1 + kShimmerFilterInverseStrength);
    float weight2 = 1.0f / (luma2 + kShimmerFilterInverseStrength);
    float weight3 = 1.0f / (luma3 + kShimmerFilterInverseStrength);
    float weight4 = 1.0f / (luma4 + kShimmerFilterInverseStrength);
    float weightSum = weight1 + weight2 + weight3 + weight4;


    float3 Result = (color1 * weight1 + color2 * weight2 + color3 * weight3 + color4 * weight4) / weightSum;;

    DstTex[DTid.xy] = float4(Result, 0);

	float luma = (luma1 + luma2 + luma3 + luma4) * 0.25;
	
	if (luma == 0.0)
    {
        LumaResult[DTid.xy] = 0;
    }
    else
    {
    	const float MinLog = Exposure[4];
        const float RcpLogRange = Exposure[7];
        float logLuma = saturate((log2(luma) - MinLog) * RcpLogRange);    // Rescale to [0.0, 1.0]
        LumaResult[DTid.xy] = logLuma * 254.0 + 1.0;                    // Rescale to [1, 255]
    }
}

[numthreads(32, 32, 1)]
void BloomBlur( uint3 DTid : SV_DispatchThreadID)
{
    float2 PixelPos = DTid.xy;
    int2 bloom_extent = int2(RTSize.x, RTSize.y);
    float2 bloom_sample_extent = float2(bloom_extent) - float2(0.5, 0.5);
	
	float3 Result = SrcTex[PixelPos].xyz;
	float x;
    for(x = 1;x < NumSamples;x += 2)
    {
		float w1 = exp(pow(x, 2) * WeightScale);
		float w2 = exp(pow(x + 1.0, 2) * WeightScale);

		float w12 = w1 + w2;
		float p = w2 / w12;
		float2 offset = BlurDirection * (x + p);
		float2 pos1 = clamp(PixelPos + float2(0.5, 0.5) + offset, float2(0.0, 0.0), bloom_sample_extent);
		float2 pos2 = clamp(PixelPos + float2(0.5, 0.5) - offset, float2(0.0, 0.0), bloom_sample_extent);

		float2 uv1 = pos1/RTSize;
		float2 uv2 = pos2/RTSize;

		float3 Sample;
		Sample = SrcTex.SampleLevel(sampleWrap, uv1, 0).xyz;
		Result += Sample * w12;

		Sample = SrcTex.SampleLevel(sampleWrap, uv2, 0).xyz;
		Result += Sample * w12;
    }

    Result *= NormalizationScale;
    DstTex[DTid.xy] = float4(Result, 1);
}