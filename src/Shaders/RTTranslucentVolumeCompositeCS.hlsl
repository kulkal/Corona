Texture2D<float4> VolumeTex : register(t0);
Texture2D<float4> CoverageTex : register(t1);
RWTexture2D<float4> LightingTex : register(u0);

cbuffer RTTranslucentVolumeCompositeCB : register(b0)
{
    float4 FullSizeAndInvSize;
    uint4 LowResolutionSize;
};

float4 SampleLowResolutionVolume(float2 uv)
{
    int2 lowSize = int2(max(LowResolutionSize.xy, uint2(1u, 1u)));
    float2 samplePosition = uv * float2(lowSize) - 0.5f;
    int2 p0 = int2(floor(samplePosition));
    float2 f = frac(samplePosition);
    p0 = clamp(p0, int2(0, 0), lowSize - 1);
    int2 p1 = min(p0 + 1, lowSize - 1);

    float4 c00 = VolumeTex.Load(int3(p0, 0));
    float4 c10 = VolumeTex.Load(int3(int2(p1.x, p0.y), 0));
    float4 c01 = VolumeTex.Load(int3(int2(p0.x, p1.y), 0));
    float4 c11 = VolumeTex.Load(int3(p1, 0));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(FullSizeAndInvSize.xy)))
        return;

    // The full-resolution refracted guide is also an exact glass coverage
    // mask, preventing low-resolution volume samples from bleeding outside
    // the visible glass silhouette.
    if (CoverageTex.Load(int3(pixel, 0)).w <= 0.0001f)
        return;

    float2 uv = (float2(pixel) + 0.5f) * FullSizeAndInvSize.zw;
    float4 volume = SampleLowResolutionVolume(uv);
    float4 lighting = LightingTex[pixel];
    lighting.rgb = lighting.rgb * saturate(volume.a) + max(volume.rgb, 0.0f.xxx);
    LightingTex[pixel] = lighting;
}
