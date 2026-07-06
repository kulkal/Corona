cbuffer TranslucentGuideWarpCB : register(b0)
{
    float4 Params; // xy = inverse render size, z = coverage threshold, w = reserved
    uint4 Flags;
};

Texture2D<float>  SrcDepth                 : register(t0);
Texture2D<float2> SrcVelocity              : register(t1);
Texture2D<float4> SrcNormal                : register(t2);
Texture2D<float4> SrcRoughness             : register(t3);
Texture2D<float4> SrcAlbedo                : register(t4);
Texture2D<float4> SrcSpecularAlbedo        : register(t5);
// Absolute mode: xy = composited source UV.
// Offset mode:   xy = accumulated source-UV delta, resolved as current UV + xy.
// z/w carry translucent coverage for visualization/thresholding.
Texture2D<float4> TranslucentDistortionTex : register(t6);

RWTexture2D<float>  OutDepth          : register(u0);
RWTexture2D<float2> OutVelocity       : register(u1);
RWTexture2D<float4> OutNormal         : register(u2);
RWTexture2D<float4> OutRoughness      : register(u3);
RWTexture2D<float4> OutAlbedo         : register(u4);
RWTexture2D<float4> OutSpecularAlbedo : register(u5);

uint2 ClampPixel(int2 pixel, uint2 dims)
{
    return uint2(
        clamp(pixel.x, 0, int(max(dims.x, 1u) - 1u)),
        clamp(pixel.y, 0, int(max(dims.y, 1u) - 1u)));
}

void BilinearFootprint(float2 uv, uint2 dims, out uint2 p00, out uint2 p10, out uint2 p01, out uint2 p11, out float2 f)
{
    float2 pixel = saturate(uv) * float2(dims) - 0.5f.xx;
    float2 basePixel = floor(pixel);
    f = pixel - basePixel;
    int2 baseInt = int2(basePixel);
    p00 = ClampPixel(baseInt, dims);
    p10 = ClampPixel(baseInt + int2(1, 0), dims);
    p01 = ClampPixel(baseInt + int2(0, 1), dims);
    p11 = ClampPixel(baseInt + int2(1, 1), dims);
}

float BilinearFloat(Texture2D<float> tex, float2 uv, uint2 dims)
{
    uint2 p00, p10, p01, p11;
    float2 f;
    BilinearFootprint(uv, dims, p00, p10, p01, p11, f);
    float v00 = tex.Load(int3(p00, 0));
    float v10 = tex.Load(int3(p10, 0));
    float v01 = tex.Load(int3(p01, 0));
    float v11 = tex.Load(int3(p11, 0));
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

float2 BilinearFloat2(Texture2D<float2> tex, float2 uv, uint2 dims)
{
    uint2 p00, p10, p01, p11;
    float2 f;
    BilinearFootprint(uv, dims, p00, p10, p01, p11, f);
    float2 v00 = tex.Load(int3(p00, 0));
    float2 v10 = tex.Load(int3(p10, 0));
    float2 v01 = tex.Load(int3(p01, 0));
    float2 v11 = tex.Load(int3(p11, 0));
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

float4 BilinearFloat4(Texture2D<float4> tex, float2 uv, uint2 dims)
{
    uint2 p00, p10, p01, p11;
    float2 f;
    BilinearFootprint(uv, dims, p00, p10, p01, p11, f);
    float4 v00 = tex.Load(int3(p00, 0));
    float4 v10 = tex.Load(int3(p10, 0));
    float4 v01 = tex.Load(int3(p01, 0));
    float4 v11 = tex.Load(int3(p11, 0));
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return lenSq > 1.0e-8f ? value * rsqrt(lenSq) : fallback;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dims;
    SrcDepth.GetDimensions(dims.x, dims.y);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dims.x || pixel.y >= dims.y)
        return;

    float2 uv = (float2(pixel) + 0.5f.xx) * Params.xy;
    float4 refractionGuide = TranslucentDistortionTex.Load(int3(pixel, 0));
    float coverage = refractionGuide.w;
    float coverageThreshold = max(Params.z, 0.0f);
    bool useWarp = coverage > coverageThreshold;
    bool guideStoresOffset = Flags.z != 0u;
    float2 warpedUv = guideStoresOffset ? (uv + refractionGuide.xy) : refractionGuide.xy;
    float2 sourceUv = useWarp ? saturate(warpedUv) : uv;

    float depth = useWarp ? BilinearFloat(SrcDepth, sourceUv, dims) : SrcDepth.Load(int3(pixel, 0));
    float2 velocity = useWarp ? BilinearFloat2(SrcVelocity, sourceUv, dims) : SrcVelocity.Load(int3(pixel, 0));
    float4 normal = useWarp ? BilinearFloat4(SrcNormal, sourceUv, dims) : SrcNormal.Load(int3(pixel, 0));
    float4 roughness = useWarp ? BilinearFloat4(SrcRoughness, sourceUv, dims) : SrcRoughness.Load(int3(pixel, 0));
    float4 albedo = useWarp ? BilinearFloat4(SrcAlbedo, sourceUv, dims) : SrcAlbedo.Load(int3(pixel, 0));
    float4 specularAlbedo = useWarp ? BilinearFloat4(SrcSpecularAlbedo, sourceUv, dims) : SrcSpecularAlbedo.Load(int3(pixel, 0));

    if (Flags.y == 0u)
        normal.xyz = SafeNormalize(normal.xyz, float3(0.0f, 1.0f, 0.0f));

    OutDepth[pixel] = depth;
    OutVelocity[pixel] = velocity;
    OutNormal[pixel] = normal;
    OutRoughness[pixel] = roughness;
    OutAlbedo[pixel] = albedo;
    OutSpecularAlbedo[pixel] = specularAlbedo;
}
