#include "Common.hlsl"

Texture2D InputTex : register(t0);
Texture2D DepthTex : register(t1);
Texture2D GeoNormalTex : register(t2);

RWTexture2D<float4> OutputTex : register(u0);


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
	for(int yy = -r; yy <= r; yy++) 
	{
		for(int xx = -r; xx <= r; xx++) 
		{
			float2 SamplePos = CenterPos + float2(xx, yy) * StepSize;
			float3 Sample = InputTex[SamplePos];

			float W = 1;

			float3 SampleDepth = DepthTex[SamplePos.xy];
			float SampleZ = GetLinearDepthOpenGL(SampleDepth, ProjectionParams.x, ProjectionParams.y) ;

			float DistZ = abs(CenterZ - SampleZ) * IndirectDiffuseWeightFactorDepth;
			//exp(-dist_z / float(step_size * GRAD_DWN));
			W *= exp(-DistZ/StepSize) ;
			float3 Normal = GeoNormalTex[SamplePos.xy];
			W *= wavelet_kernel[abs(xx)][abs(yy)];

			float GNdotGN = max(0.0, dot(CenterNormal, Normal));
			W *= pow(GNdotGN, IndirectDiffuseWeightFactorNormal);

			SumW += W;
			SumColor += Sample*W;
		}
	}
    OutputTex[DTid.xy] = float4(SumColor/SumW, 1);
}