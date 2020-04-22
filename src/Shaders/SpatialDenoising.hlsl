#include "Common.hlsl"

Texture2D DepthTex : register(t0);
Texture2D GeoNormalTex : register(t1);
Texture2D InGIResultSHTex : register(t2);
Texture2D InGIResultColorTex : register(t3);
Texture2D InSpecualrGITex : register(t4);




RWTexture2D<float4> OutGIResultSH : register(u0);
RWTexture2D<float4> OutGIResultColor: register(u1);
RWTexture2D<float4> OutSpecularGI: register(u2);



cbuffer SpatialFilterConstant : register(b0)
{
	float4 ProjectionParams;
    uint Iteration;
    uint GIBufferScale;
    float IndirectDiffuseWeightFactorDepth;
    float IndirectDiffuseWeightFactorNormal;
};

static const float wavelet_factor = 0.5;
static const float wavelet_kernel[2][2] = {
	{ 1.0, wavelet_factor  },
	{ wavelet_factor, wavelet_factor * wavelet_factor }
};

void DeFlicker(Texture2D GIResultSHTex, Texture2D GIResultColorTex, Texture2D SpecularGITex, uint2 Pos, inout SH result, inout float3 SpecularResult )
{
	int StepSize =  int(1u << (Iteration - 1));

	uint2 CenterPos = Pos;

	SH CenterSH = init_SH();
	CenterSH.shY = GIResultSHTex[CenterPos];
	CenterSH.CoCg = GIResultColorTex[CenterPos].xy;

	float3 CenterSpecular = SpecularGITex[CenterPos].xyz;

	float3 SumColor = float3(0, 0, 0);

	float3 SumSpecular = 0..xxx;

	float SumW = 0;


	SH SumSH = init_SH();

	
	const int r = 1;

	for(int yy = -r; yy <= r; yy++) 
	{
		for(int xx = -r; xx <= r; xx++) 
		{
			if(xx == 0 && yy == 0)
				continue;

			uint2 SamplePos = CenterPos + float2(xx, yy);

			SumSH.shY += GIResultSHTex[SamplePos];
			SumSH.CoCg += GIResultColorTex[SamplePos].xy;

			SumSpecular += SpecularGITex[SamplePos].xyz;
		}
	}

	const float num_pixels = pow(r * 2 + 1, 2) - 1;

	float max_lum = SumSH.shY.w * 1.0 / num_pixels;
	if(CenterSH.shY.w > max_lum)
	{
		float ratio = max_lum / CenterSH.shY.w;
		CenterSH.shY *= ratio;
		CenterSH.CoCg *= ratio;
	}

	result = CenterSH;

	float max_spec = RGBToLuminance(SumSpecular) * 1.0 / num_pixels;
	float center_spec_luma = RGBToLuminance(CenterSpecular);
	if(center_spec_luma > max_lum)
	{
		float ratio = max_lum / center_spec_luma;

		CenterSpecular *= ratio; 	
	}
	SpecularResult = CenterSpecular;
}

void Filter(Texture2D GIResultSHTex,Texture2D GIResultColorTex, Texture2D SpecularGITex, uint2 Pos, inout SH result, inout float3 SpecularResult)
{
	int StepSize =  int(1u << (Iteration - 1));

	uint2 CenterPos = Pos;
	uint2 CenterPosHiRes = CenterPos * DOWNSAMPLE_SIZE + uint2(1, 1);

	float CenterDepth = DepthTex[CenterPosHiRes.xy];
	float CenterZ = GetLinearDepthOpenGL(CenterDepth, ProjectionParams.z, ProjectionParams.w) ;

	float3 CenterNormal = GeoNormalTex[CenterPosHiRes.xy];

	const int r = 1;
	float3 SumColor = float3(0, 0, 0);
	float3 SumSpecular = 0..xxx;

	float SumW = 1.0f;

	SH SumSH = init_SH();

	SumSH.shY = GIResultSHTex[CenterPos];
	SumSH.CoCg = GIResultColorTex[CenterPos].xy;
	for(int yy = -r; yy <= r; yy++) 
	{
		for(int xx = -r; xx <= r; xx++) 
		{
			if(xx == 0 && yy == 0)
				continue;
			uint2 SamplePos = CenterPos + float2(xx, yy) * StepSize;
			uint2 SamplePosHiRes = SamplePos * DOWNSAMPLE_SIZE + uint2(1,1);

			SH SampleSH;
			SampleSH.shY = GIResultSHTex[SamplePos];
			SampleSH.CoCg = GIResultColorTex[SamplePos].xy;

			float3 SampleSpecular = SpecularGITex[SamplePos].xyz;
			float W = 1;

			float3 SampleDepth = DepthTex[SamplePosHiRes.xy];
			float SampleZ = GetLinearDepthOpenGL(SampleDepth, ProjectionParams.z, ProjectionParams.w) ;

			float DistZ = abs(CenterZ - SampleZ) * IndirectDiffuseWeightFactorDepth;
			W *= exp(-DistZ/float(StepSize*DOWNSAMPLE_SIZE)) ;
			float3 Normal = GeoNormalTex[SamplePosHiRes.xy];
			W *= wavelet_kernel[abs(xx)][abs(yy)];

			float GNdotGN = max(0.0, dot(CenterNormal, Normal));
			W *= pow(GNdotGN, IndirectDiffuseWeightFactorNormal);

			SumW += W;
			accumulate_SH(SumSH, SampleSH, W);
			SumSpecular += SampleSpecular;
		}
	}

    scale_SH(SumSH, 1.0/SumW);

    result = SumSH;

    SumSpecular /= SumW;
    SpecularResult = SumSpecular;
}

[numthreads(32, 32, 1)]
void SpatialFilter( uint3 DTid : SV_DispatchThreadID )
{
    SH ResultSH;
    float3 SpecularReuslt = 0..xxxx;
    if(Iteration == 0)
		DeFlicker(InGIResultSHTex, InGIResultColorTex, InSpecualrGITex, DTid.xy, ResultSH, SpecularReuslt);
	else
		Filter(InGIResultSHTex, InGIResultColorTex, InSpecualrGITex, DTid.xy, ResultSH, SpecularReuslt);


    OutGIResultSH[DTid.xy] = ResultSH.shY;
    OutGIResultColor[DTid.xy] = float4(ResultSH.CoCg, 0, 0);

    OutSpecularGI[DTid.xy] = float4(SpecularReuslt, 0);
}
