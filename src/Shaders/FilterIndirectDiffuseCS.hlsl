#include "Common.hlsl"

Texture2D DepthTex : register(t0);
Texture2D GeoNormalTex : register(t1);
Texture2D InGIResultSHTex : register(t2);
Texture2D InGIResultColorTex : register(t3);


RWTexture2D<float4> OutGIResultSH : register(u0);
RWTexture2D<float4> OutGIResultColor: register(u1);



cbuffer FilterIndirectDiffuseConstant : register(b0)
{
	float4 ProjectionParams;
    uint Iteration;
    float IndirectDiffuseWeightFactorDepth;
    float IndirectDiffuseWeightFactorNormal;
};

static const float wavelet_factor = 0.5;
static const float wavelet_kernel[2][2] = {
	{ 1.0, wavelet_factor  },
	{ wavelet_factor, wavelet_factor * wavelet_factor }
};


[numthreads(32, 32, 1)]
void FilterIndirectDiffuse( uint3 DTid : SV_DispatchThreadID )
{
	int StepSize =  int(1u << (Iteration - 1));

	float2 CenterPos = DTid.xy;

	float CenterDepth = DepthTex[CenterPos.xy];
	float CenterZ = GetLinearDepthOpenGL(CenterDepth, ProjectionParams.x, ProjectionParams.y) ;

	float3 CenterNormal = GeoNormalTex[CenterPos.xy];

	const int r = 1;
	float3 SumColor = float3(0, 0, 0);
	float SumW = 0;

	SH SumSH = init_SH();

	for(int yy = -r; yy <= r; yy++) 
	{
		for(int xx = -r; xx <= r; xx++) 
		{
			float2 SamplePos = CenterPos + float2(xx, yy) * StepSize;

			SH SampleSH;
			SampleSH.shY = InGIResultSHTex[SamplePos];
			SampleSH.CoCg = InGIResultColorTex[SamplePos].xy;

			float W = 1;

			float3 SampleDepth = DepthTex[SamplePos.xy];
			float SampleZ = GetLinearDepthOpenGL(SampleDepth, ProjectionParams.x, ProjectionParams.y) ;

			float DistZ = abs(CenterZ - SampleZ) * IndirectDiffuseWeightFactorDepth;
			W *= exp(-DistZ/StepSize) ;
			float3 Normal = GeoNormalTex[SamplePos.xy];
			W *= wavelet_kernel[abs(xx)][abs(yy)];

			float GNdotGN = max(0.0, dot(CenterNormal, Normal));
			W *= pow(GNdotGN, IndirectDiffuseWeightFactorNormal);

			SumW += W;
			accumulate_SH(SumSH, SampleSH, W);
		}
	}

    scale_SH(SumSH, 1.0/SumW);

    OutGIResultSH[DTid.xy] = SumSH.shY;
    OutGIResultColor[DTid.xy] = float4(SumSH.CoCg, 0, 0);

}