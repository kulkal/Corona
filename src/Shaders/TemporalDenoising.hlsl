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




RWTexture2D<float4> OutGIResultSH : register(u0);
RWTexture2D<float4> OutGIResultColor: register(u1);
RWTexture2D<float4> OutGIResultSHDS : register(u2);
RWTexture2D<float4> OutGIResultColorDS: register(u3);
RWTexture2D<float4> OutSpecularGI: register(u4);



cbuffer TemporalFilterConstant : register(b0)
{
	float4 ProjectionParams;
	float2 RTSize;
};

#define GROUPSIZE 15
groupshared float4 g_SH[GROUPSIZE][GROUPSIZE]; 
groupshared float2 g_CoCg[GROUPSIZE][GROUPSIZE];
groupshared float3 g_Normal[GROUPSIZE][GROUPSIZE]; 
groupshared float g_Depth[GROUPSIZE][GROUPSIZE]; 

static const float2 off[4] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };


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
	SH PrevSH = init_SH();
	float2 PrevPos;

    float2 Velocity = VelocityTex[PixelPos];
    PrevPos = PixelPos - Velocity  * RTSize;

	float4 CurrentSpecular = InSpecularGITex[PixelPos];
	float4 PrevSpecular = InSpecularGITexPrev[PrevPos];


	SH CurrentSH = init_SH();
	CurrentSH.shY = InGIResultSHTex[PixelPos];
	CurrentSH.CoCg = InGIResultColorTex[PixelPos].xy;

	bool isValidHistory = false;
	float temporal_sum_w_spec = 0.0f;
	float2 pos_ld = floor(PrevPos - float2(0.5, 0.5));
	float2 subpix = frac(PrevPos - float2(0.5, 0.5) - pos_ld);
	
	PrevSH.shY = InGIResultSHTexPrev[PrevPos];
	PrevSH.CoCg = InGIResultColorTexPrev[PrevPos].xy;

 	
	//Bilinear/bilateral filter
	float w[4] = {
		(1.0 - subpix.x) * (1.0 - subpix.y),
		(subpix.x      ) * (1.0 - subpix.y),
		(1.0 - subpix.x) * (subpix.y      ),
		(subpix.x      ) * (subpix.y      )
	};

	// bool bt = false;
	// [unroll]
	for(int i = 0; i < 4; i++) 
	{
		float2 p = float2(pos_ld) + off[i];

		if(p.x < 0 || p.x >= RTSize.x || p.y < 0 || p.y >= RTSize.y)
			continue;

		float PrevDepth = DepthTex[p];
		float3 PrevNormal = NormalTex[p];

		float dist_depth = abs(CurDepth - PrevDepth);
		float dot_normals = dot(CurNormal, PrevNormal);

		if(CurDepth < 0)
		{
			// Reduce the filter sensitivity to depth for secondary surfaces,
			// because reflection/refraction motion vectors are often inaccurate.
			dist_depth *= 0.25;
		}

		if(dist_depth < 2.0 && dot_normals > 0.5) 
		{
			float w_diff = w[i];
			float w_spec = w_diff * pow(max(dot_normals, 0), 128);
			temporal_sum_w_spec += w_spec;
			// bt = true;
		}
	}

	// this code have application hang infinitely with gpu validation enabled.
	if(temporal_sum_w_spec > 0.000001)
	{
		isValidHistory = true;
	}

	// if(bt == true)
		// isValidHistory = true;

		// isValidHistory = true;

	float4 BlendedSpecular = 0..xxxx;
	SH BlendedSH = init_SH();
	float W = 0.1;
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
	
	// if(pos_ld.x < 0 || pos_ld.x >= RTSize.x || pos_ld.y < 0 || pos_ld.y >= RTSize.y)
	// 	BlendedSpecular = CurrentSpecular;
	// else
	//     BlendedSpecular = max(CurrentSpecular * W + PrevSpecular * (1-W), float4(0, 0, 0, 0));


	
	// BlendedSH.shY = CurrentSH.shY;
	// BlendedSH.CoCg = CurrentSH.CoCg;

	OutGIResultSH[PixelPos] = BlendedSH.shY;
    OutGIResultColor[PixelPos] = float4(BlendedSH.CoCg, 0, 0);

    OutSpecularGI[PixelPos] = BlendedSpecular;

    g_SH[GroupPos.y][GroupPos.x] = BlendedSH.shY;
    g_CoCg[GroupPos.y][GroupPos.x] = BlendedSH.CoCg;
   


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
	float CenterZ = GetLinearDepthOpenGL(CenterDepth, ProjectionParams.x, ProjectionParams.y) ;
	
	// float depth_width = s_depth_width[lowres_local_id.y][lowres_local_id.x];

	SH CenterSH;
	CenterSH.shY = g_SH[CenterHiResPos.y][CenterHiResPos.x];
	CenterSH.CoCg = g_CoCg[CenterHiResPos.y][CenterHiResPos.x];

	float sum_w = 1;
	SH SumSH = CenterSH;

	for(int yy = -1; yy <= 1; yy++)
	{
		for(int xx = -1; xx <= 1; xx++)
		{
			if(yy == 0 && xx == 0)
				continue;

			float3 SampleNormal = g_Normal[CenterHiResPos.y + yy][CenterHiResPos.x + xx].xyz;
			float SampleDepth = g_Depth[CenterHiResPos.y + yy][CenterHiResPos.x + xx];
			float SampleZ = GetLinearDepthOpenGL(SampleDepth, ProjectionParams.x, ProjectionParams.y) ;

			float w = 1.0f;
			float DistZ = abs(SampleZ - CenterZ) * 0.2;
			w *= exp(-DistZ/1) ;

			w *= pow(max(dot(SampleNormal, CenterNormal), 0), 8);

			SH SampleSH;
			SampleSH.shY = g_SH[CenterHiResPos.y + yy][CenterHiResPos.x + xx];
			SampleSH.CoCg = g_CoCg[CenterHiResPos.y + yy][CenterHiResPos.x + xx];

			accumulate_SH(SumSH, SampleSH, w);
			sum_w += w;
		}
	}

	float inv_w = 1.0 / sum_w;
	SumSH.shY  *= inv_w;
	SumSH.CoCg *= inv_w;

    uint2 LowResPos = GId * (GROUPSIZE / DOWNSAMPLE_SIZE) + LowResGroupPos;
	OutGIResultSHDS[LowResPos] = SumSH.shY;
    OutGIResultColorDS[LowResPos] = float4(SumSH.CoCg, 0, 0);
}