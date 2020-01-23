#include "Common.hlsl"

Texture2D DepthTex : register(t0);
Texture2D GeoNormalTex : register(t1);
Texture2D InGIResultSHTex : register(t2);
Texture2D InGIResultColorTex : register(t3);
Texture2D InGIResultSHTexPrev : register(t4);
Texture2D InGIResultColorTexPrev : register(t5);
Texture2D VelocityTex : register(t6);



RWTexture2D<float4> OutGIResultSH : register(u0);
RWTexture2D<float4> OutGIResultColor: register(u1);
RWTexture2D<float4> OutGIResultSHDS : register(u2);
RWTexture2D<float4> OutGIResultColorDS: register(u3);



cbuffer TemporalFilterConstant : register(b0)
{
	float4 ProjectionParams;
	float2 RTSize;
};


[numthreads(32, 32, 1)]
void TemporalFilter( uint3 DTid : SV_DispatchThreadID )
{
	float2 PixelPos = DTid.xy;
	SH CurrentSH = init_SH();
	CurrentSH.shY = InGIResultSHTex[PixelPos];
	CurrentSH.CoCg = InGIResultColorTex[PixelPos].xy;

	SH PrevSH = init_SH();
	uint2 PrevPos;

    float2 Velocity = VelocityTex[PixelPos];
    PrevPos = PixelPos - Velocity  * RTSize;

	PrevSH.shY = InGIResultSHTexPrev[PrevPos];
	PrevSH.CoCg = InGIResultColorTexPrev[PrevPos].xy;


	SH BlendedSH = init_SH();
	float W = 0.1;
	BlendedSH.shY = max(CurrentSH.shY * W + PrevSH.shY * (1-W), float4(0, 0, 0, 0));
    BlendedSH.CoCg = max(CurrentSH.CoCg * W + PrevSH.CoCg * (1-W), float2(0, 0));

	OutGIResultSH[PixelPos] = BlendedSH.shY;
    OutGIResultColor[PixelPos] = float4(BlendedSH.CoCg, 0, 0);

    OutGIResultSHDS[PixelPos] = BlendedSH.shY;
    OutGIResultColorDS[PixelPos] = float4(BlendedSH.CoCg, 0, 0);


	// OutGIResultSHDS[PixelPos] = CurrentSH.shY;
 //    OutGIResultColorDS[PixelPos] = float4(CurrentSH.CoCg, 0, 0);
}