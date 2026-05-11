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
RWTexture2D<float4> OutSpecularGI: register(u2);
RWTexture2D<float2> OutMoments: register(u3);

SamplerState BilinearClamp : register(s0);

cbuffer TemporalFilterConstant : register(b0)
{
    float4x4 InvViewMatrix;
    float4x4 InvProjMatrix;
    float4x4 PrevUnjitteredViewProjMatrix;
	float4 ProjectionParams;
	float4 TemporalValidParams;
	float2 RTSize;
	uint FrameIndex;
	float BayerRotScale;
	float SpecularBlurRadius;
	float Point2PlaneDistScale;
	float AccumulationAlpha;
	uint HistoryValid;
	float2 JitterOffset;
	float SpecularAccumulationAlpha;
	float SpecularVarianceClipGamma;
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

float3 SanitizeFloat3(float3 value)
{
	if (any(isnan(value)) || any(isinf(value)))
		return 0.0f.xxx;

	return value;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
	value = SanitizeFloat3(value);
	fallback = SanitizeFloat3(fallback);
	float lenSq = dot(value, value);
	return lenSq > 1e-12f ? value * rsqrt(lenSq) : fallback;
}

float3 ReconstructWorldPosition(uint2 pixelPos, float deviceDepth)
{
    float2 uv = (float2(pixelPos) + 0.5f) / RTSize;
    float2 screenPosition = uv * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;
    float3 viewPosition = GetViewPosition(deviceDepth, screenPosition, InvProjMatrix);
    return SanitizeFloat3(mul(float4(viewPosition, 1.0f), InvViewMatrix).xyz);
}

bool ProjectToScreenUVAndDepth(float3 worldPos, float4x4 viewProj, out float2 uv, out float deviceDepth)
{
    float4 clip = mul(float4(worldPos, 1.0f), viewProj);
    if (abs(clip.w) <= 1.0e-6f)
    {
        uv = 0.0f.xx;
        deviceDepth = 1.0f;
        return false;
    }

    float3 ndc = clip.xyz * rcp(clip.w);
    uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    deviceDepth = ndc.z;
    if (any(isnan(uv)) || any(isinf(uv)) || isnan(deviceDepth) || isinf(deviceDepth))
        return false;

    return deviceDepth >= 0.0f && deviceDepth <= 1.0f;
}

float3 LoadSpecularColor(int2 pos)
{
    float4 value = SanitizeFloat4(InSpecularGITex[pos]);
    return max(value.xyz, 0.0f.xxx);
}

float CurrentFrameGeometryWeight(int2 samplePos, float centerLinearDepth, float3 centerNormal)
{
    float sampleDepth = DepthTex[samplePos].x;
    float sampleLinearDepth = GetLinearDepthOpenGL(sampleDepth, ProjectionParams.z, ProjectionParams.w);
    float3 sampleNormal = SafeNormalize(NormalTex[samplePos].xyz, centerNormal);

    float depthDelta = abs(centerLinearDepth - sampleLinearDepth) / max(centerLinearDepth, 1e-3f);
    float depthWeight = exp(-depthDelta * 64.0f);
    float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), 16.0f);
    return saturate(depthWeight * normalWeight);
}

void ComputeSpecularNeighborhoodStats(uint2 centerPos, uint2 textureSize, float centerLinearDepth, float3 centerNormal, out float3 meanColor, out float3 sigmaColor, out float edgeFactor)
{
    float3 sumColor = 0.0f.xxx;
    float3 sumColorSq = 0.0f.xxx;
    float sampleWeightSum = 0.0f;
    float neighborWeightSum = 0.0f;

    [unroll]
    for (int yy = -1; yy <= 1; ++yy)
    {
        [unroll]
        for (int xx = -1; xx <= 1; ++xx)
        {
            int2 samplePos = int2(centerPos) + int2(xx, yy);
            samplePos = clamp(samplePos, int2(0, 0), int2(textureSize) - 1);
            float geometryWeight = (xx == 0 && yy == 0) ? 1.0f : CurrentFrameGeometryWeight(samplePos, centerLinearDepth, centerNormal);
            float3 color = LoadSpecularColor(samplePos);
            sumColor += color * geometryWeight;
            sumColorSq += color * color * geometryWeight;
            sampleWeightSum += geometryWeight;
            if (xx != 0 || yy != 0)
                neighborWeightSum += geometryWeight;
        }
    }

    meanColor = sumColor / max(sampleWeightSum, 1.0e-4f);
    float3 variance = max(sumColorSq / max(sampleWeightSum, 1.0e-4f) - meanColor * meanColor, 0.0f.xxx);
    sigmaColor = sqrt(variance);
    edgeFactor = saturate(1.0f - neighborWeightSum / 8.0f);
}

bool SelectPrevSurfacePixel(float2 prevUV, uint2 textureSize, float expectedPrevLinearDepth, float3 currentNormal, out int2 bestPos, out float bestWeight)
{
    float2 prevPixel = prevUV * float2(textureSize) - 0.5f;
    int2 basePos = int2(floor(prevPixel));
    int2 maxPos = int2(textureSize) - 1;
    bestPos = clamp(int2(round(prevPixel)), int2(0, 0), maxPos);
    bestWeight = 0.0f;

    [unroll]
    for (int yy = -1; yy <= 2; ++yy)
    {
        [unroll]
        for (int xx = -1; xx <= 2; ++xx)
        {
            int2 samplePos = clamp(basePos + int2(xx, yy), int2(0, 0), maxPos);
            float prevDepth = PrevDepthTex[samplePos].x;
            float prevLinearDepth = GetLinearDepthOpenGL(prevDepth, ProjectionParams.z, ProjectionParams.w);
            float3 prevNormal = SafeNormalize(PrevNormalTex[samplePos].xyz, currentNormal);

            float2 sampleCenter = float2(samplePos) + 0.5f;
            float2 pixelDelta = sampleCenter - prevUV * float2(textureSize);
            float filterWeight = exp(-dot(pixelDelta, pixelDelta) * 1.0f);
            float depthDelta = abs(expectedPrevLinearDepth - prevLinearDepth) / max(expectedPrevLinearDepth, 1e-3f);
            float depthWeight = exp(-depthDelta * 48.0f);
            float normalWeight = pow(saturate(dot(currentNormal, prevNormal)), 16.0f);
            float weight = filterWeight * depthWeight * normalWeight;

            if (weight > bestWeight)
            {
                bestWeight = weight;
                bestPos = samplePos;
            }
        }
    }

    return bestWeight > 1.0e-5f;
}

float2 BlendSpecularMoments(float2 prevMoments, float3 currentSpecular, float alpha)
{
    float luminance = max(RGBToLuminance(currentSpecular), 0.0f);
    float2 currentMoments = float2(luminance, luminance * luminance);
    return lerp(prevMoments, currentMoments, alpha);
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

    float currentDepth = DepthTex[SafePixelPos].x;
    float currentLinearDepth = GetLinearDepthOpenGL(currentDepth, ProjectionParams.z, ProjectionParams.w);
    float3 currentNormal = SafeNormalize(NormalTex[SafePixelPos].xyz, float3(0.0f, 1.0f, 0.0f));
    float roughness = clamp(SanitizeFloat4(RougnessMetalicTex[SafePixelPos]).x, 0.02f, 1.0f);
    float3 specularNeighborhoodMean = 0.0f.xxx;
    float3 specularNeighborhoodSigma = 0.0f.xxx;
    float geometryEdgeFactor = 0.0f;
    ComputeSpecularNeighborhoodStats(SafePixelPos, TextureSize, currentLinearDepth, currentNormal, specularNeighborhoodMean, specularNeighborhoodSigma, geometryEdgeFactor);
    CurrentSpecular.xyz = max(CurrentSpecular.xyz, 0.0f.xxx);

	float4 BlendedSpecular = CurrentSpecular;
	float4 BlendedDiffuse = CurrentDiffuse;
    float2 BlendedMoments = float2(max(RGBToLuminance(CurrentSpecular.xyz), 0.0f), 0.0f);
    BlendedMoments.y = BlendedMoments.x * BlendedMoments.x;

    if (HistoryValid != 0)
    {
        float2 velocity = VelocityTex[SafePixelPos].xy;
        float2 velocityPrevUV = (float2(SafePixelPos) + 0.5f - velocity * RTSize) / RTSize;
        float3 currentWorldPos = ReconstructWorldPosition(SafePixelPos, currentDepth);
        float2 reprojectedPrevUV = velocityPrevUV;
        float expectedPrevDeviceDepth = currentDepth;
        bool reprojectionValid = ProjectToScreenUVAndDepth(
            currentWorldPos,
            PrevUnjitteredViewProjMatrix,
            reprojectedPrevUV,
            expectedPrevDeviceDepth);
        float2 prevUV = reprojectionValid ? reprojectedPrevUV : velocityPrevUV;
        float expectedPrevLinearDepth = GetLinearDepthOpenGL(expectedPrevDeviceDepth, ProjectionParams.z, ProjectionParams.w);

        bool validHistory = all(prevUV >= 0.0.xx) && all(prevUV <= 1.0.xx);
        int2 bestPrevSpecularPos = int2(0, 0);
        float bestPrevSpecularWeight = 0.0f;
        if (validHistory)
        {
            validHistory = SelectPrevSurfacePixel(prevUV, TextureSize, expectedPrevLinearDepth, currentNormal, bestPrevSpecularPos, bestPrevSpecularWeight);
        }

        if (validHistory)
        {
            float prevDepth = PrevDepthTex[bestPrevSpecularPos].x;
            float prevLinearDepth = GetLinearDepthOpenGL(prevDepth, ProjectionParams.z, ProjectionParams.w);

            float3 prevNormal = SafeNormalize(PrevNormalTex[bestPrevSpecularPos].xyz, currentNormal);

            float depthDelta = abs(expectedPrevLinearDepth - prevLinearDepth) / max(expectedPrevLinearDepth, 1e-3f);
            float depthWeight = exp(-depthDelta * 36.0f);
            float normalDot = saturate(dot(currentNormal, prevNormal));
            float diffuseNormalWeight = pow(normalDot, 32.0f);
            float specularNormalPower = lerp(32.0f, 12.0f, roughness);
            float specularNormalWeight = pow(normalDot, specularNormalPower);
            float diffuseHistoryWeight = saturate(depthWeight * diffuseNormalWeight);
            float specularSurfaceWeight = saturate(bestPrevSpecularWeight * 6.0f);
            float specularHistoryWeight = saturate(depthWeight * specularNormalWeight * specularSurfaceWeight);

            float4 PrevSpecular = SanitizeFloat4(InSpecularGITexPrev[bestPrevSpecularPos]);
            float4 PrevDiffuse = SanitizeFloat4(InGIResultColorTexPrev.SampleLevel(BilinearClamp, prevUV, 0));
            float2 PrevMoments = max(PrevMomentsTex[bestPrevSpecularPos].xy, 0.0f.xx);
            PrevDiffuse.w = 1.0f;

            float3 clipRadius = max(specularNeighborhoodSigma * max(SpecularVarianceClipGamma, 0.0f), 1.0e-4f.xxx);
            float3 clipMin = max(specularNeighborhoodMean - clipRadius, 0.0f.xxx);
            float3 clipMax = specularNeighborhoodMean + clipRadius;
            float3 ClippedPrevSpecular = clamp(PrevSpecular.xyz, clipMin, clipMax);
            float currentSpecularSignal = max(RGBToLuminance(CurrentSpecular.xyz), RGBToLuminance(specularNeighborhoodMean));
            float sparseSampleClipWeight = saturate(currentSpecularSignal * 16.0f);
            float specularClipWeight = lerp(1.0f, sparseSampleClipWeight, roughness);
            PrevSpecular.xyz = lerp(max(PrevSpecular.xyz, 0.0f.xxx), ClippedPrevSpecular, specularClipWeight);
            float edgeCurrentClipWeight = geometryEdgeFactor * (1.0f - specularHistoryWeight) * saturate(roughness * 1.25f);
            float3 ClippedCurrentSpecular = clamp(CurrentSpecular.xyz, clipMin, clipMax);
            CurrentSpecular.xyz = lerp(CurrentSpecular.xyz, ClippedCurrentSpecular, edgeCurrentClipWeight);

            float diffuseAlpha = lerp(1.0f, accumulationAlpha, diffuseHistoryWeight);
            float specularAlphaBase = saturate(SpecularAccumulationAlpha * lerp(1.25f, 0.75f, roughness));
            float specularAlpha = lerp(1.0f, specularAlphaBase, specularHistoryWeight);
            specularAlpha *= lerp(1.0f, 0.60f, geometryEdgeFactor * specularHistoryWeight);

            BlendedDiffuse = lerp(PrevDiffuse, CurrentDiffuse, diffuseAlpha);
            BlendedDiffuse.w = 1.0f;
            BlendedSpecular = lerp(PrevSpecular, CurrentSpecular, specularAlpha);
            BlendedSpecular.w = lerp(PrevSpecular.w + 1.0f, 1.0f, 1.0f - specularHistoryWeight);
            BlendedMoments = BlendSpecularMoments(PrevMoments, BlendedSpecular.xyz, specularAlpha);
        }
        else
        {
            float3 clipRadius = max(specularNeighborhoodSigma * max(SpecularVarianceClipGamma, 0.0f), 1.0e-4f.xxx);
            float3 clipMin = max(specularNeighborhoodMean - clipRadius, 0.0f.xxx);
            float3 clipMax = specularNeighborhoodMean + clipRadius;
            float edgeCurrentClipWeight = geometryEdgeFactor * saturate(roughness * 1.25f);
            CurrentSpecular.xyz = lerp(CurrentSpecular.xyz, clamp(CurrentSpecular.xyz, clipMin, clipMax), edgeCurrentClipWeight);
            BlendedSpecular = CurrentSpecular;
            BlendedSpecular.w = 1.0f;
        }
    }
    else
    {
        float3 clipRadius = max(specularNeighborhoodSigma * max(SpecularVarianceClipGamma, 0.0f), 1.0e-4f.xxx);
        float3 clipMin = max(specularNeighborhoodMean - clipRadius, 0.0f.xxx);
        float3 clipMax = specularNeighborhoodMean + clipRadius;
        float edgeCurrentClipWeight = geometryEdgeFactor * saturate(roughness * 1.25f);
        CurrentSpecular.xyz = lerp(CurrentSpecular.xyz, clamp(CurrentSpecular.xyz, clipMin, clipMax), edgeCurrentClipWeight);
        BlendedSpecular = CurrentSpecular;
        BlendedSpecular.w = 1.0f;
    }

	if(IsInBounds)
	{
		BlendedDiffuse = SanitizeFloat4(BlendedDiffuse);
		BlendedDiffuse.xyz = max(BlendedDiffuse.xyz, 0.0f.xxx);
		BlendedDiffuse.w = 1.0f;
		BlendedSpecular = SanitizeFloat4(BlendedSpecular);
		BlendedSpecular.xyz = max(BlendedSpecular.xyz, 0.0f.xxx);
		OutGIResultSH[PixelPos] = 0.0f.xxxx;
		OutGIResultColor[PixelPos] = BlendedDiffuse;
		OutSpecularGI[PixelPos] = BlendedSpecular;
        OutMoments[PixelPos] = max(BlendedMoments, 0.0f.xx);
	}

}
