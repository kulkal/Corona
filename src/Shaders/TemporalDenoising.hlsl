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
RWTexture2D<float2> OutMoments: register(u5);

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
	float AccumulationAlpha;
	uint HistoryValid;
	float2 Padding;
};

#define GROUPSIZE 15
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

float4 SanitizeFloat4(float4 value)
{
	if (any(isnan(value)) || any(isinf(value)))
		return 0.0f.xxxx;

	return value;
}

[numthreads(15, 15, 1)]
void TemporalFilter( uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint GTIndex : SV_GroupIndex, uint3 GId : SV_GroupID)
{
    uint2 TextureSize = uint2(RTSize);
    uint2 PixelPos = DTid.xy;
	bool IsInBounds = PixelPos.x < TextureSize.x && PixelPos.y < TextureSize.y;
	uint2 SafePixelPos = min(PixelPos, TextureSize - 1);
	float accumulationAlpha = saturate(AccumulationAlpha);

    float4 CurrentSpecular = SanitizeFloat4(InSpecularGITex[SafePixelPos]);
    float4 CurrentDiffuse = SanitizeFloat4(InGIResultColorTex[SafePixelPos]);
    CurrentDiffuse.w = 1.0f;

    float4 BlendedSpecular = CurrentSpecular;
    float4 BlendedDiffuse = CurrentDiffuse;

    if (HistoryValid != 0)
    {
        float2 velocity = VelocityTex[SafePixelPos].xy;
        float2 prevUV = (float2(SafePixelPos) + 0.5f - velocity * RTSize) / RTSize;

        bool validHistory = all(prevUV >= 0.0.xx) && all(prevUV <= 1.0.xx);
        if (validHistory)
        {
            float currentDepth = DepthTex[SafePixelPos].x;
            float prevDepth = PrevDepthTex.SampleLevel(BilinearClamp, prevUV, 0).x;

            float currentLinearDepth = GetLinearDepthOpenGL(currentDepth, ProjectionParams.z, ProjectionParams.w);
            float prevLinearDepth = GetLinearDepthOpenGL(prevDepth, ProjectionParams.z, ProjectionParams.w);

            float3 currentNormal = normalize(NormalTex[SafePixelPos].xyz);
            float3 prevNormal = normalize(PrevNormalTex.SampleLevel(BilinearClamp, prevUV, 0).xyz);

            float depthDelta = abs(currentLinearDepth - prevLinearDepth) / max(currentLinearDepth, 1e-3f);
            float depthWeight = exp(-depthDelta * 32.0f);
            float normalWeight = pow(saturate(dot(currentNormal, prevNormal)), 32.0f);
            float historyWeight = saturate(depthWeight * normalWeight);

            float4 PrevSpecular = SanitizeFloat4(InSpecularGITexPrev.SampleLevel(BilinearClamp, prevUV, 0));
            float4 PrevDiffuse = SanitizeFloat4(InGIResultColorTexPrev.SampleLevel(BilinearClamp, prevUV, 0));
            PrevDiffuse.w = 1.0f;

            float diffuseAlpha = lerp(1.0f, accumulationAlpha, historyWeight);
            float specularAlpha = lerp(1.0f, saturate(accumulationAlpha * 1.5f), historyWeight);

            BlendedDiffuse = lerp(PrevDiffuse, CurrentDiffuse, diffuseAlpha);
            BlendedDiffuse.w = 1.0f;
            BlendedSpecular = lerp(PrevSpecular, CurrentSpecular, specularAlpha);
            BlendedSpecular.w = lerp(PrevSpecular.w + 1.0f, 1.0f, 1.0f - historyWeight);
        }
        else
        {
            BlendedSpecular.w = 1.0f;
        }
    }
    else
    {
        BlendedSpecular.w = 1.0f;
    }

	if(IsInBounds)
	{
		OutGIResultSH[PixelPos] = 0.0f.xxxx;
		OutGIResultColor[PixelPos] = BlendedDiffuse;
		OutGIResultSHDS[PixelPos] = 0.0f.xxxx;
		OutGIResultColorDS[PixelPos] = BlendedDiffuse;
		OutSpecularGI[PixelPos] = BlendedSpecular;
	}

}
