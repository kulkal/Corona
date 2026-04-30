#include "Common.hlsl"

Texture2D DepthTex : register(t0);
Texture2D GeoNormalTex : register(t1);
Texture2D InGIResultSHTex : register(t2);
Texture2D InGIResultColorTex : register(t3);
Texture2D InSpecularGITex : register(t4);

RWTexture2D<float4> OutGIResultSH : register(u0);
RWTexture2D<float4> OutGIResultColor: register(u1);
RWTexture2D<float4> OutSpecularGI : register(u2);

cbuffer SpatialFilterConstant : register(b0)
{
	float4 ProjectionParams;
    uint Iteration;
    uint GIBufferScale;
    uint AccumulatedFrames;
    uint Padding;
    float IndirectDiffuseWeightFactorDepth;
    float IndirectDiffuseWeightFactorNormal;
};

static const float wavelet_factor = 0.5;
static const float wavelet_kernel[2][2] = {
	{ 1.0, wavelet_factor  },
	{ wavelet_factor, wavelet_factor * wavelet_factor }
};

float SanitizeFloat(float value, float fallback)
{
    return (isnan(value) || isinf(value)) ? fallback : value;
}

float3 SanitizeFloat3(float3 value)
{
    return (any(isnan(value)) || any(isinf(value))) ? 0.0f.xxx : value;
}

float4 SanitizeFloat4(float4 value)
{
    return (any(isnan(value)) || any(isinf(value))) ? 0.0f.xxxx : value;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    value = SanitizeFloat3(value);
    fallback = SanitizeFloat3(fallback);
    float lenSq = dot(value, value);
    return lenSq > 1e-12f ? value * rsqrt(lenSq) : fallback;
}

float3 LoadDiffuseRadiance(Texture2D GIResultColorTex, int2 pos)
{
    return max(SanitizeFloat3(GIResultColorTex[pos].xyz), 0.0f.xxx);
}

float4 LoadSpecularRadiance(Texture2D SpecularTex, int2 pos)
{
    float4 value = SanitizeFloat4(SpecularTex[pos]);
    value.xyz = max(value.xyz, 0.0f.xxx);
    return value;
}

float Luminance(float3 color)
{
    return SanitizeFloat(dot(SanitizeFloat3(color), float3(0.2126, 0.7152, 0.0722)), 0.0f);
}

void Filter(Texture2D GIResultColorTex, uint2 Pos, inout float3 result)
{
	int StepSize = int(1u << max(int(Iteration) - 1, 0));

	uint Width;
	uint Height;
	DepthTex.GetDimensions(Width, Height);

	int2 CenterPos = int2(Pos);

	float CenterDepth = DepthTex[CenterPos];
	float CenterZ = GetLinearDepthOpenGL(CenterDepth, ProjectionParams.z, ProjectionParams.w);
	float3 CenterNormal = SafeNormalize(GeoNormalTex[CenterPos].xyz, float3(0.0f, 1.0f, 0.0f));

	const int r = 1;
	float3 SumColor = LoadDiffuseRadiance(GIResultColorTex, CenterPos);
	float SumW = 1.0f;

	for (int yy = -r; yy <= r; yy++)
	{
		for (int xx = -r; xx <= r; xx++)
		{
			if (xx == 0 && yy == 0)
				continue;

			int2 SamplePos = CenterPos + int2(xx, yy) * StepSize;
			if (SamplePos.x < 0 || SamplePos.y < 0 || SamplePos.x >= Width || SamplePos.y >= Height)
				continue;

			float W = 1.0f;
			float SampleDepth = DepthTex[SamplePos];
			float SampleZ = GetLinearDepthOpenGL(SampleDepth, ProjectionParams.z, ProjectionParams.w);

			float DistZ = abs(CenterZ - SampleZ) * IndirectDiffuseWeightFactorDepth;
			W *= exp(-DistZ / float(StepSize));

			float3 Normal = SafeNormalize(GeoNormalTex[SamplePos].xyz, CenterNormal);
			W *= wavelet_kernel[abs(xx)][abs(yy)];

			float GNdotGN = max(0.0, dot(CenterNormal, Normal));
			W *= pow(GNdotGN, IndirectDiffuseWeightFactorNormal);

			SumW += W;
			SumColor += LoadDiffuseRadiance(GIResultColorTex, SamplePos) * W;
		}
	}

    result = SumColor / SumW;
}

void FilterSpecular(Texture2D SpecularTex, uint2 Pos, inout float4 result)
{
	int StepSize = int(1u << max(int(Iteration) - 1, 0));

	uint Width;
	uint Height;
	DepthTex.GetDimensions(Width, Height);

	int2 CenterPos = int2(Pos);
	float CenterDepth = DepthTex[CenterPos];
	float CenterZ = GetLinearDepthOpenGL(CenterDepth, ProjectionParams.z, ProjectionParams.w);
	float3 CenterNormal = SafeNormalize(GeoNormalTex[CenterPos].xyz, float3(0.0f, 1.0f, 0.0f));
	float4 CenterSpecular = LoadSpecularRadiance(SpecularTex, CenterPos);

	const int r = 1;
	float4 SumSpecular = CenterSpecular;
	float SumW = 1.0f;

	for (int yy = -r; yy <= r; yy++)
	{
		for (int xx = -r; xx <= r; xx++)
		{
			if (xx == 0 && yy == 0)
				continue;

			int2 SamplePos = CenterPos + int2(xx, yy) * StepSize;
			if (SamplePos.x < 0 || SamplePos.y < 0 || SamplePos.x >= Width || SamplePos.y >= Height)
				continue;

			float SampleDepth = DepthTex[SamplePos];
			float SampleZ = GetLinearDepthOpenGL(SampleDepth, ProjectionParams.z, ProjectionParams.w);
			float3 SampleNormal = SafeNormalize(GeoNormalTex[SamplePos].xyz, CenterNormal);
			float4 SampleSpecular = LoadSpecularRadiance(SpecularTex, SamplePos);

			float depthDelta = abs(CenterZ - SampleZ) * IndirectDiffuseWeightFactorDepth;
			float normalWeight = pow(max(0.0, dot(CenterNormal, SampleNormal)), IndirectDiffuseWeightFactorNormal * 2.0);
			float luminanceDelta = abs(Luminance(CenterSpecular.xyz) - Luminance(SampleSpecular.xyz));

			float W = wavelet_kernel[abs(xx)][abs(yy)];
			W *= exp(-depthDelta / float(StepSize));
			W *= normalWeight;
			W *= exp(-luminanceDelta * 4.0);

			W = max(SanitizeFloat(W, 0.0f), 0.0f);
			SumSpecular += SampleSpecular * W;
			SumW += W;
		}
	}

	result = SanitizeFloat4(SumSpecular / max(SumW, 1e-4f));
	result.xyz = max(result.xyz, 0.0f.xxx);
}

[numthreads(32, 32, 1)]
void SpatialFilter(uint3 DTid : SV_DispatchThreadID)
{
    uint Width;
    uint Height;
    DepthTex.GetDimensions(Width, Height);
    if (DTid.x >= Width || DTid.y >= Height)
        return;

    if (AccumulatedFrames < 2)
    {
        OutGIResultSH[DTid.xy] = 0.0f.xxxx;
        OutGIResultColor[DTid.xy] = float4(LoadDiffuseRadiance(InGIResultColorTex, DTid.xy), 1.0f);
        OutSpecularGI[DTid.xy] = LoadSpecularRadiance(InSpecularGITex, DTid.xy);
        return;
    }

    float3 ResultDiffuse = 0.0f.xxx;
    float4 ResultSpecular = 0.0f.xxxx;
    Filter(InGIResultColorTex, DTid.xy, ResultDiffuse);
    FilterSpecular(InSpecularGITex, DTid.xy, ResultSpecular);

    OutGIResultSH[DTid.xy] = 0.0f.xxxx;
    OutGIResultColor[DTid.xy] = float4(max(SanitizeFloat3(ResultDiffuse), 0.0f.xxx), 1.0f);
    OutSpecularGI[DTid.xy] = SanitizeFloat4(ResultSpecular);
}
