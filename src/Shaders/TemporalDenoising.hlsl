#include "Common.hlsl"

Texture2D DepthTex : register(t0);
Texture2D NormalTex : register(t1);
Texture2D InGIResultSHTex : register(t2);
Texture2D InGIResultColorTex : register(t3);
Texture2D InGIResultSHTexPrev : register(t4);
Texture2D InGIResultColorTexPrev : register(t5);
Texture2D VelocityTex : register(t6);
Texture2D InSpecularGITex : register(t7);
Texture2D InSpecularGITexPrev : register(t8);
Texture2D RougnessMetalicTex : register(t9);
Texture2D PrevDepthTex : register(t10);
Texture2D PrevNormalTex : register(t11);







RWTexture2D<float4> OutGIResultSH : register(u0);
RWTexture2D<float4> OutGIResultColor: register(u1);
RWTexture2D<float4> OutGIResultSHDS : register(u2);
RWTexture2D<float4> OutGIResultColorDS: register(u3);
RWTexture2D<float4> OutSpecularGI: register(u4);
// RWTexture2D<float4> OutSpecularGIDS: register(u5);

SamplerState BilinearClamp : register(s0);



cbuffer TemporalFilterConstant : register(b0)
{
    float4x4 InvViewMatrix;
    float4x4 InvProjMatrix;
	float4 ProjectionParams;
	float4 TemporalValidParams;
	float2 RTSize;
	uint FrameIndex;
	float BayerRotScale;
	float SpecularBlurRadius;
	float Point2PlaneDistScale;
};

#define GROUPSIZE 15
groupshared float4 g_SH[GROUPSIZE][GROUPSIZE]; 
groupshared float2 g_CoCg[GROUPSIZE][GROUPSIZE];
groupshared float3 g_Normal[GROUPSIZE][GROUPSIZE]; 
groupshared float g_Depth[GROUPSIZE][GROUPSIZE]; 
// groupshared float4 g_Specular[GROUPSIZE][GROUPSIZE]; 


static const float2 off[4] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };

static const uint POISSON_SAMPLE_NUM = 16;
static const float2 POISSON_SAMPLES[POISSON_SAMPLE_NUM] =
{
	float2( 0.25846023600949697f, -0.07369550760351032f ),
	float2( -0.9838570552784007f, -0.05779516564478064f ),
	float2( -0.027743258156343067f, 0.9811291888930508f ),
	float2( -0.27749280859153147f, -0.9558763050496616f ),
	float2( 0.7957500833563568f, 0.601381663828957f ),
	float2( 0.610845296785476f, -0.7463029770721497f ),
	float2( -0.6784295880309313f, 0.660255493042322f ),
	float2( 0.964729322668074f, -0.061480119938628806f ),
	float2( -0.44963058033639447f, -0.357675895070531f ),
	float2( -0.313556193156912f, 0.2145219816729168f ),
	float2( 0.2859315712886788f, 0.43424956054318387f ),
	float2( -0.04534300033675266f, -0.5334074339145939f ),
	float2( -0.6541908929580176f, -0.7536055504687329f ),
	float2( 0.18809498350573928f, -0.9128177827906239f ),
	float2( 0.60895935404913f, 0.15435512892534362f ),
	float2( 0.43638971738806215f, 0.8146738166643487f ),
};


static const uint BAYER_SAMPLE_NUM = 16;
static const float BAYER_SAMPLES[BAYER_SAMPLE_NUM] =
{
0, 8, 2, 10, 
12, 4, 14, 6, 
3, 11, 1, 9, 
15, 7, 13, 5
};

[numthreads(15, 15, 1)]
void TemporalFilter( uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint GTIndex : SV_GroupIndex, uint3 GId : SV_GroupID)
{
	float2 PixelPos = DTid.xy;
	float2 GroupPos = GTid.xy;



  	uint2 LowResGroupPos;
	LowResGroupPos.x = GTIndex % (GROUPSIZE / DOWNSAMPLE_SIZE);
	LowResGroupPos.y = GTIndex / (GROUPSIZE / DOWNSAMPLE_SIZE);
	uint2 CenterHiResPos = LowResGroupPos * DOWNSAMPLE_SIZE + uint2(1, 1);

	float3 CurNormal = g_Normal[GroupPos.y][GroupPos.x] = NormalTex[PixelPos].xyz;
    float CurDepth = g_Depth[GroupPos.y][GroupPos.x] = DepthTex[PixelPos];

	

    // GroupMemoryBarrierWithGroupSync();
	float2 PrevPos;

    float3 Velocity = VelocityTex[PixelPos].xyz;
    PrevPos = PixelPos - Velocity.xy  * RTSize;
    float2 PrevUV = (PrevPos + 0.5) /RTSize;


    // get current indirect specular.

	float4 CurrentSpecular = InSpecularGITex[PixelPos];

	float3 SpecMin = 99999999.0f;
	float3 SpecMax = -99999999.0f;
	// float SpecularBlurRadius = 1;
	float Point2PlaneDist = clamp(CurrentSpecular.w/Point2PlaneDistScale, 0, 100);
	float Roughness = RougnessMetalicTex[PixelPos].x;
	float BlurRadius = SpecularBlurRadius * Roughness;// * (saturate(hitDist/0.5) * 0.9 + 0.1);
	float SumWSpec = 1;
	float RotAngle = BAYER_SAMPLES[FrameIndex % BAYER_SAMPLE_NUM] * 0.1;
	float CZ = GetLinearDepthOpenGL(CurDepth, ProjectionParams.z, ProjectionParams.w) ;
	for(int i=0;i<POISSON_SAMPLE_NUM;i++)
	{
		float2 Offset = POISSON_SAMPLES[i];
		float2 OffsetRotated;
		OffsetRotated.x = Offset.x * cos(RotAngle) - Offset.y * sin(RotAngle);
		OffsetRotated.y = Offset.x * sin(RotAngle) + Offset.y * cos(RotAngle);

		// OffsetRotated = Offset;
 		float2 uv = (DTid.xy + 0.5 + OffsetRotated* BlurRadius) / RTSize;

		float4 SampleSpecular = InSpecularGITex.SampleLevel(BilinearClamp, uv, 0);

		SpecMin = min(SpecMin, SampleSpecular);
        SpecMax = max(SpecMax, SampleSpecular);

		float SampleDepth = DepthTex.SampleLevel(BilinearClamp, uv, 0);
		float SampleZ = GetLinearDepthOpenGL(SampleDepth, ProjectionParams.z, ProjectionParams.w) ;
		float SampleW = 1;///16.0; // point to plane weight
		float DistZ = abs(SampleZ - CZ) * 0.2;
		SampleW *= exp(-DistZ/1);

		CurrentSpecular += SampleSpecular * SampleW;
		SumWSpec += SampleW;
	}
	CurrentSpecular /= SumWSpec;

	float4 PrevSpecular = InSpecularGITexPrev.SampleLevel(BilinearClamp, PrevUV, 0);


	SH CurrentSH = init_SH();
	CurrentSH.shY = InGIResultSHTex[PixelPos];
	CurrentSH.CoCg = InGIResultColorTex[PixelPos].xy;

	bool isValidHistory = false;
	float temporal_sum_w_spec = 0.0f;
	float2 pos_ld = floor(PrevPos - float2(0.5, 0.5));
	float2 subpix = frac(PrevPos - float2(0.5, 0.5) - pos_ld);
	
	SH PrevSH = init_SH();

	PrevSH.shY = InGIResultSHTexPrev[PrevPos];
	PrevSH.CoCg = InGIResultColorTexPrev[PrevPos].xy;

 	
	//Bilinear/bilateral filter
	float w[4] = {
		(1.0 - subpix.x) * (1.0 - subpix.y),
		(subpix.x      ) * (1.0 - subpix.y),
		(1.0 - subpix.x) * (subpix.y      ),
		(subpix.x      ) * (subpix.y      )
	};

	float fwidth_depth = 1.0 / max(0.1, (abs(DepthTex[PixelPos + float2(1, 0)] - DepthTex[PixelPos]) * 2 + abs(DepthTex[PixelPos + float2(0, 1)] - DepthTex[PixelPos])));

	// bool bt = false;
	// [unroll]
	for(int i = 0; i < 4; i++) 
	{
		float2 p = float2(pos_ld) + off[i];

		if(p.x < 0 || p.x >= RTSize.x || p.y < 0 || p.y >= RTSize.y)
			continue;

		float PrevDepth = PrevDepthTex[p];
		float3 PrevNormal = PrevNormalTex[p];

		float dist_depth = abs(CurDepth - PrevDepth ) ;
		// float dist_depth  = abs(GetLinearDepthOpenGL(CurDepth, ProjectionParams.z, ProjectionParams.w) - GetLinearDepthOpenGL(PrevDepth, ProjectionParams.z, ProjectionParams.w));
		float dot_normals = abs(dot(CurNormal, PrevNormal));
		// if(CurDepth < 0)
		// {
		// 	// Reduce the filter sensitivity to depth for secondary surfaces,
		// 	// because reflection/refraction motion vectors are often inaccurate.
		// 	dist_depth *= 0.25;
		// }


		if(dist_depth < 0.001 && dot_normals > 0.5) // dont use normal weight
		// if(dist_depth < 0.001) 
		{
			float w_diff = w[i];
			float w_spec = w_diff * pow(max(dot_normals, 0), TemporalValidParams.x);
			temporal_sum_w_spec += w_spec;
		}
	}

	// this code have application hang infinitely with gpu validation enabled.
	if(temporal_sum_w_spec > 0.000001)
	{
		float inv_w_spec = 1.0 / temporal_sum_w_spec;
		// PrevSH.shY *= inv_w_spec;
		// PrevSH.CoCg *= inv_w_spec;
		// PrevSpecular *= inv_w_spec;
		isValidHistory = true;
	}

	if(abs(CurDepth - PrevDepthTex[PrevPos].x) > 0.001)
		isValidHistory = false;
	
	float4 BlendedSpecular = 0..xxxx;
	SH BlendedSH = init_SH();
	float W = 0.05;
	if(isValidHistory)
	{
		BlendedSH.shY = max(CurrentSH.shY * W + PrevSH.shY * (1-W), float4(0, 0, 0, 0));
    	BlendedSH.CoCg = max(CurrentSH.CoCg * W + PrevSH.CoCg * (1-W), float2(0, 0));	
	    BlendedSpecular = max(CurrentSpecular * W + PrevSpecular * (1-W), float4(0, 0, 0, 0));
	}
	else
	{
		BlendedSH.shY = CurrentSH.shY;
		BlendedSH.CoCg = CurrentSH.CoCg;

		BlendedSpecular = CurrentSpecular;
	}

	OutGIResultSH[PixelPos] = BlendedSH.shY;
    OutGIResultColor[PixelPos] = float4(BlendedSH.CoCg, 0, 0);

    OutSpecularGI[PixelPos] = BlendedSpecular;

    g_SH[GroupPos.y][GroupPos.x] = BlendedSH.shY;
    g_CoCg[GroupPos.y][GroupPos.x] = BlendedSH.CoCg;

    // g_Specular[GroupPos.y][GroupPos.x] = BlendedSpecular;
   
    GroupMemoryBarrierWithGroupSync();

	// gl_LocalInvocationIndex 0 ~225
	// GROUP_SIZE / GRAD_DWN = 15/3 = 5
	// 5 * 15
	// 5x5 == 25 
	// lowres_local_id.x = 0~5
	// lowres_local_id.y 0 ~ 5
	if(LowResGroupPos.y >= (GROUPSIZE / DOWNSAMPLE_SIZE))
		return;

	float3 CenterNormal = g_Normal[CenterHiResPos.y][CenterHiResPos.x];
	float CenterDepth = g_Depth[CenterHiResPos.y][CenterHiResPos.x];
	float CenterZ = GetLinearDepthOpenGL(CenterDepth, ProjectionParams.z, ProjectionParams.w) ;
	
	// float depth_width = s_depth_width[lowres_local_id.y][lowres_local_id.x];

	SH CenterSH;
	CenterSH.shY = g_SH[CenterHiResPos.y][CenterHiResPos.x];
	CenterSH.CoCg = g_CoCg[CenterHiResPos.y][CenterHiResPos.x];

	float sum_w = 1;
	SH SumSH = CenterSH;

	// float3 SumSpecular = 0..xxx;

	for(int yy = -1; yy <= 1; yy++)
	{
		for(int xx = -1; xx <= 1; xx++)
		{
			if(yy == 0 && xx == 0)
				continue;

			float3 SampleNormal = g_Normal[CenterHiResPos.y + yy][CenterHiResPos.x + xx].xyz;
			float SampleDepth = g_Depth[CenterHiResPos.y + yy][CenterHiResPos.x + xx];
			float SampleZ = GetLinearDepthOpenGL(SampleDepth, ProjectionParams.z, ProjectionParams.w) ;

			float w = 1.0f;
			float DistZ = abs(SampleZ - CenterZ) * 0.2;
			w *= exp(-DistZ/1) ;

			w *= pow(max(dot(SampleNormal, CenterNormal), 0), 8	);

			SH SampleSH;
			SampleSH.shY = g_SH[CenterHiResPos.y + yy][CenterHiResPos.x + xx];
			SampleSH.CoCg = g_CoCg[CenterHiResPos.y + yy][CenterHiResPos.x + xx];

			accumulate_SH(SumSH, SampleSH, w);

			// float3 SampleSpecular = g_Specular[CenterHiResPos.y + yy][CenterHiResPos.x + xx];
			// SumSpecular += SampleSpecular;
			sum_w += w;
		}
	}

	float inv_w = 1.0 / sum_w;
	SumSH.shY  *= inv_w;
	SumSH.CoCg *= inv_w;

	// SumSpecular *= inv_w;

    uint2 LowResPos = GId * (GROUPSIZE / DOWNSAMPLE_SIZE) + LowResGroupPos;
	OutGIResultSHDS[LowResPos] = SumSH.shY;
    OutGIResultColorDS[LowResPos] = float4(SumSH.CoCg, 0, 0);

    // OutSpecularGIDS[LowResPos] = float4(SumSpecular, 0);
}