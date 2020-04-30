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
Texture2D PrevMomentsTex : register(t12);


RWTexture2D<float4> OutGIResultSH : register(u0);
RWTexture2D<float4> OutGIResultColor: register(u1);
RWTexture2D<float4> OutGIResultSHDS : register(u2);
RWTexture2D<float4> OutGIResultColorDS: register(u3);
RWTexture2D<float4> OutSpecularGI: register(u4);
RWTexture2D<float2> OutMoments: register(u4);

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


static const uint POISSON_SAMPLE_NUM_32 = 32;
static const float2 POISSON_SAMPLES_32[POISSON_SAMPLE_NUM_32] =
{
float2( 0.08914344750840812f, -0.2027165125645366f ),
float2( -0.031254838210002314f, 0.9912623227437531f ),
float2( -0.9964568437817566f, -0.07390083486272249f ),
float2( 0.981919243083431f, 0.13481222555143754f ),
float2( -0.4885452139738782f, -0.859840112028502f ),
float2( 0.6612104643206592f, -0.7173737184190583f ),
float2( -0.7507281568803518f, 0.6225813229479589f ),
float2( 0.596869625129957f, 0.7158779322491781f ),
float2( -0.20179031491534383f, 0.3819461664975282f ),
float2( -0.4618695266408025f, -0.29223438720335576f ),
float2( 0.08073455658976186f, -0.7819072590660048f ),
float2( 0.4507053855068781f, 0.2154671921495113f ),
float2( 0.6412236296974572f, -0.2446829484686105f ),
float2( -0.8341371480526937f, -0.48705381195987907f ),
float2( -0.5536884767374927f, 0.11035488791371083f ),
float2( -0.42228914896175185f, 0.8898791336133093f ),
float2( 0.19500257262746878f, 0.42870015254050525f ),
float2( -0.9070646162199346f, 0.28760274401734853f ),
float2( -0.22501159500075157f, -0.04371424710100678f ),
float2( 0.3340736476755119f, -0.4833473135029284f ),
float2( -0.25545302940574766f, -0.5798035145815387f ),
float2( 0.8302957300483342f, 0.48671992299637945f ),
float2( 0.2036372687768874f, 0.7744221179374544f ),
float2( 0.012189199786729759f, 0.14469405499546542f ),
float2( -0.4964896537118397f, 0.43257476489283847f ),
float2( 0.8689327687123395f, -0.47837457885212686f ),
float2( -0.13058148341375442f, 0.7212742371777681f ),
float2( 0.35315773287482366f, -0.8898675152646364f ),
float2( -0.6935527106056756f, -0.1318961631412847f ),
float2( 0.43600590198894346f, -0.051051712958311866f ),
float2( 0.9457234273621928f, -0.15546033539173398f ),
float2( -0.22423020436783675f, -0.9342829287420676f ),
};


static const uint POISSON_SAMPLE_NUM_64 = 64;
static const float2 POISSON_SAMPLES_64[POISSON_SAMPLE_NUM_64] =
{
float2( -0.542699805405688f, -0.6906232994914508f ),
float2( 0.7229818777150687f, 0.6767239587180666f ),
float2( -0.7640630372972745f, 0.6437852135668126f ),
float2( 0.8265813457988677f, -0.49215469271496204f ),
float2( -0.0649569168880174f, 0.04093397887517135f ),
float2( -0.011259959625007762f, 0.9986607243233767f ),
float2( 0.2142512838549119f, -0.7858672337212956f ),
float2( -0.9562978304629239f, -0.016122189266543165f ),
float2( 0.5440957565881402f, 0.09462335373694758f ),
float2( -0.03938018587722491f, 0.516569715310768f ),
float2( -0.4845596545374887f, -0.17714284087300866f ),
float2( -0.5225976937129615f, 0.2713849119605479f ),
float2( 0.28902219925684897f, -0.2729533622332572f ),
float2( -0.8434478654557043f, -0.4149799149141992f ),
float2( 0.9601148248961222f, 0.24023224759991627f ),
float2( -0.10331045216156008f, -0.4636150588384604f ),
float2( -0.17523649061744007f, -0.9001585059444526f ),
float2( -0.3589258939558971f, 0.7380610581602933f ),
float2( 0.2646304715788124f, 0.334214982922808f ),
float2( 0.34186196190691964f, 0.7729414072729975f ),
float2( -0.9273026608063973f, 0.35057597007436586f ),
float2( 0.836996516571903f, -0.11756463972449667f ),
float2( 0.5245761162866703f, -0.8469800883437837f ),
float2( 0.5494335154934327f, 0.38398789119159266f ),
float2( 0.5469191059895063f, -0.5608878370408178f ),
float2( 0.5861901431237382f, -0.26898967866341267f ),
float2( -0.6768347917561706f, 0.02994255053635442f ),
float2( -0.25956496904501947f, 0.28098376532248837f ),
float2( 0.2791446002945637f, 0.036529317874680854f ),
float2( 0.005362365485203324f, -0.22895711897457705f ),
float2( -0.28090513192055044f, -0.6362686445398181f ),
float2( 0.16027855656278303f, -0.47819721073191906f ),
float2( -0.46866796039852127f, -0.4509375409020569f ),
float2( 0.0940378351441739f, 0.7271009855657213f ),
float2( -0.3988737023463961f, 0.056383407943574985f ),
float2( 0.7837036756174697f, 0.39687308378377534f ),
float2( 0.25606151411561845f, 0.5548947926658006f ),
float2( -0.2388035720745557f, 0.9545526035947465f ),
float2( -0.0285609341277277f, -0.6765751742350687f ),
float2( -0.2450156453058468f, -0.26471424895817336f ),
float2( -0.5843744129586053f, 0.7867749755473687f ),
float2( -0.2830726357940043f, 0.5295041975199964f ),
float2( 0.5480630813667018f, 0.8249111683995971f ),
float2( -0.6348479096544887f, 0.4666948948865301f ),
float2( -0.707902760161206f, -0.2077234791999431f ),
float2( -0.0034893514090893717f, 0.28500709681958075f ),
float2( 0.783310893438842f, 0.1508349067504005f ),
float2( -0.9527134137440653f, -0.21907857746095002f ),
float2( -0.7780952748280195f, -0.6189027138883865f ),
float2( 0.036101099795017355f, -0.9143159343643856f ),
float2( 0.46834207088610885f, -0.10416605464178012f ),
float2( 0.36474545760628907f, -0.4750130392043023f ),
float2( -0.757153198204298f, 0.21897886878311537f ),
float2( -0.40398930704501845f, -0.8577744065764443f ),
float2( -0.1384381406752557f, 0.7485154500750951f ),
float2( 0.19224965196204163f, 0.9271226758982362f ),
float2( -0.44269788835929463f, 0.43341756128621217f ),
float2( 0.13340733416528336f, 0.17195427818321096f ),
float2( 0.9976456856364166f, -0.0011724919559358959f ),
float2( 0.7134533896237952f, -0.6526927274133182f ),
float2( -0.32302314689108574f, -0.1082072235848419f ),
float2( 0.2217134081664831f, -0.9748442275920963f ),
float2( 0.10545644067805462f, -0.07930945289406821f ),
float2( 0.4107027127726185f, -0.7071269300216128f ),
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
	float4 PrevSpecular = 0..xxxx;

	PrevSpecular = InSpecularGITexPrev.SampleLevel(BilinearClamp, PrevUV, 0);

    // get current indirect specular.

	float4 CurrentSpecular = InSpecularGITex[PixelPos];

	float HistoryLength = PrevSpecular.w * 10.0f;

	float3 SpecMin = 99999999.0f;
	float3 SpecMax = -99999999.0f;
	// float SpecularBlurRadius = 1;
	float Point2PlaneDist = clamp(CurrentSpecular.w/Point2PlaneDistScale, 0, 100);
	float Roughness = RougnessMetalicTex[PixelPos].x;
	float SumWSpec = 1;
	float RotAngle = BAYER_SAMPLES[FrameIndex % BAYER_SAMPLE_NUM] * 0.1;
	float CZ = GetLinearDepthOpenGL(CurDepth, ProjectionParams.z, ProjectionParams.w) ;
	
	// to reduce ghosting
	if(PrevSpecular.w < 0.5)
		PrevSpecular.xyz = float3(0, 0, 0);

	// if(PrevSpecular.w < 0.5)
	// {
	// 	float BlurRadius = SpecularBlurRadius * Roughness*4;// * (saturate(hitDist/0.5) * 0.9 + 0.1);

	// 	for(int i=0;i<POISSON_SAMPLE_NUM_32;i++)
	// 	{
	// 		float2 Offset = POISSON_SAMPLES_32[i];
	// 		float2 OffsetRotated;
	// 		OffsetRotated.x = Offset.x * cos(RotAngle) - Offset.y * sin(RotAngle);
	// 		OffsetRotated.y = Offset.x * sin(RotAngle) + Offset.y * cos(RotAngle);

	// 		// OffsetRotated = Offset;
	//  		float2 uv = (DTid.xy + 0.5 + OffsetRotated* BlurRadius) / RTSize;

	// 		float4 SampleSpecular = InSpecularGITex.SampleLevel(BilinearClamp, uv, 0);

	// 		SpecMin = min(SpecMin, SampleSpecular);
	//         SpecMax = max(SpecMax, SampleSpecular);

	// 		float SampleDepth = DepthTex.SampleLevel(BilinearClamp, uv, 0);
	// 		float SampleZ = GetLinearDepthOpenGL(SampleDepth, ProjectionParams.z, ProjectionParams.w) ;
	// 		float SampleW = 1;///16.0; // point to plane weight
	// 		float DistZ = abs(SampleZ - CZ) * 0.2;
	// 		SampleW *= exp(-DistZ/1);

	// 		CurrentSpecular += SampleSpecular * SampleW;
	// 		SumWSpec += SampleW;
	// 	}
	// }
	// else 
	{
		float BlurRadius = SpecularBlurRadius * Roughness *1 ;// * (saturate(hitDist/0.5) * 0.9 + 0.1);

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
	}

	CurrentSpecular /= SumWSpec;

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
		if(dist_depth < 0.001 && dot_normals > 0.5) 
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
		
		isValidHistory = true;
	}

	
	
	float4 BlendedSpecular = 0..xxxx;
	SH BlendedSH = init_SH();
	float W = 0.05;
	if(isValidHistory)
	{
		BlendedSH.shY = max(CurrentSH.shY * W + PrevSH.shY * (1-W), float4(0, 0, 0, 0));
    	BlendedSH.CoCg = max(CurrentSH.CoCg * W + PrevSH.CoCg * (1-W), float2(0, 0));	
	    BlendedSpecular = max(CurrentSpecular * W + PrevSpecular * (1-W), float4(0, 0, 0, 0));
    	BlendedSpecular.w = PrevSpecular.w + 0.1;
	}
	else
	{
		BlendedSH.shY = CurrentSH.shY;
		BlendedSH.CoCg = CurrentSH.CoCg;

		BlendedSpecular = CurrentSpecular;

    	BlendedSpecular.w = 0;

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

}