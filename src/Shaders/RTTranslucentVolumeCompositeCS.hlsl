Texture2D<float4> BackDepthTex : register(t0);
Texture2D<float4> FrontDataTex : register(t1);
Texture2D<float4> CoverageTex : register(t2);
Texture2D<float4> ShadowTex : register(t3);
RWTexture2D<float4> LightingTex : register(u0);

cbuffer RTTranslucentVolumeCompositeCB : register(b0)
{
    float4 FullSizeAndInvSize;
    uint4 LowResolutionSize;
    // x density, y scattering strength, z anisotropy, w tan(verticalFov / 2)
    float4 VolumeParams;
    float4 ViewLightDirAndIntensity;
    float4 LightColorAndSkyIntensity;
    float4 SkyColorTop;
    float4 SkyColorBottom;
    // x fallback world-space thickness for open raster layers.
    float4 RasterParams;
};

void GetLowResolutionSample(float2 uv, out int2 p0, out int2 p1, out float2 f)
{
    int2 lowSize = int2(max(LowResolutionSize.xy, uint2(1u, 1u)));
    float2 samplePosition = uv * float2(lowSize) - 0.5f;
    p0 = int2(floor(samplePosition));
    f = frac(samplePosition);
    p0 = clamp(p0, int2(0, 0), lowSize - 1);
    p1 = min(p0 + 1, lowSize - 1);
}

float4 SampleLowResolutionBackDepth(float2 uv)
{
    int2 p0;
    int2 p1;
    float2 f;
    GetLowResolutionSample(uv, p0, p1, f);
    float4 c00 = BackDepthTex.Load(int3(p0, 0));
    float4 c10 = BackDepthTex.Load(int3(int2(p1.x, p0.y), 0));
    float4 c01 = BackDepthTex.Load(int3(int2(p0.x, p1.y), 0));
    float4 c11 = BackDepthTex.Load(int3(p1, 0));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

float4 SampleLowResolutionFrontData(float2 uv)
{
    int2 p0;
    int2 p1;
    float2 f;
    GetLowResolutionSample(uv, p0, p1, f);
    float4 c00 = FrontDataTex.Load(int3(p0, 0));
    float4 c10 = FrontDataTex.Load(int3(int2(p1.x, p0.y), 0));
    float4 c01 = FrontDataTex.Load(int3(int2(p0.x, p1.y), 0));
    float4 c11 = FrontDataTex.Load(int3(p1, 0));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint2 fullSize = uint2(FullSizeAndInvSize.xy);
    if (any(pixel >= fullSize))
        return;

    if (CoverageTex.Load(int3(pixel, 0)).w <= 0.0001f)
        return;

    float2 uv = (float2(pixel) + 0.5f) * FullSizeAndInvSize.zw;
    float4 frontData = SampleLowResolutionFrontData(uv);
    if (LowResolutionSize.z != 0u)
    {
        // RT-primary already integrated the volume and stores scattering in RGB
        // and Beer-Lambert transmittance in A.
        float4 lighting = LightingTex[pixel];
        lighting.rgb = lighting.rgb * saturate(frontData.a) + max(frontData.rgb, 0.0f.xxx);
        LightingTex[pixel] = lighting;
        return;
    }

    float backDepth = SampleLowResolutionBackDepth(uv).x;
    float frontDepth = frontData.a;
    if (frontDepth <= 0.0f)
        return;
    if (backDepth <= frontDepth + 1.0e-4f)
        backDepth = frontDepth + max(RasterParams.x, 0.1f);

    float tanHalfFov = max(VolumeParams.w, 1.0e-4f);
    float aspect = FullSizeAndInvSize.x * FullSizeAndInvSize.w;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float3 viewRayDir = normalize(float3(ndc.x * aspect * tanHalfFov, ndc.y * tanHalfFov, -1.0f));
    float segmentLength = (backDepth - frontDepth) / max(abs(viewRayDir.z), 0.05f);
    float density = max(VolumeParams.x, 0.0f);
    float segmentT = exp(-density * segmentLength);

    uint shadowWidth;
    uint shadowHeight;
    ShadowTex.GetDimensions(shadowWidth, shadowHeight);
    uint2 shadowPixel = min(
        uint2(uv * float2(shadowWidth, shadowHeight)),
        uint2(max(shadowWidth, 1u) - 1u, max(shadowHeight, 1u) - 1u));
    float visibility = saturate(ShadowTex.Load(int3(shadowPixel, 0)).x);

    float3 lightDir = normalize(ViewLightDirAndIntensity.xyz);
    float anisotropy = clamp(VolumeParams.z, -0.9f, 0.9f);
    float cosTheta = dot(-viewRayDir, lightDir);
    float phaseDenom = max(1.0f + anisotropy * anisotropy - 2.0f * anisotropy * cosTheta, 1.0e-3f);
    float phase = (1.0f - anisotropy * anisotropy) /
        (4.0f * 3.14159265359f * pow(phaseDenom, 1.5f));

    float3 direct = max(LightColorAndSkyIntensity.rgb, 0.0f.xxx) *
        max(ViewLightDirAndIntensity.w, 0.0f) * visibility * phase;
    float3 ambient = max(lerp(SkyColorBottom.rgb, SkyColorTop.rgb, 0.5f), 0.0f.xxx) *
        max(LightColorAndSkyIntensity.w, 0.0f);
    float3 scattering = max(frontData.rgb, 0.0f.xxx) *
        max(VolumeParams.y, 0.0f) * (1.0f - segmentT) * (direct + ambient);

    float4 lighting = LightingTex[pixel];
    lighting.rgb = lighting.rgb * saturate(segmentT) + max(scattering, 0.0f.xxx);
    LightingTex[pixel] = lighting;
}
