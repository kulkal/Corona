#include "Common.hlsl"
#include "PathTracingWavefront.hlsli"

RWStructuredBuffer<PathTracingWavefrontState> StateOut : register(u0);
RWStructuredBuffer<uint> ActiveListOut : register(u1);
RWStructuredBuffer<uint> Counters : register(u2);
RWStructuredBuffer<float4> PathRadiance : register(u3);
RWTexture2D<float4> OutputColor : register(u4);
RWByteAddressBuffer IndirectArgs : register(u5);

cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4x4 InvProjMatrix;
    float4x4 UnjitteredViewProjMatrix;
    float4x4 PrevUnjitteredViewProjMatrix;
    float4 ProjectionParams;
    float4 LightDirAndIntensity;
    float DirectLightAngularRadius;
    uint DirectLightSampleCount;
    uint bDirectLightCastShadow;
    uint PointLightSampleCount;
    float2 RandomOffset;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    uint MaxBounces;
    uint SamplesPerPixel;
    float ViewSpreadAngle;
    uint DebugMode;
    float3 SkyColorTop;
    float SkyIntensity;
    float3 SkyColorBottom;
    float _padding;
    float3 LightColor;
    float _padding2;
    uint bEnableDiffuseGI;
    uint bEnableSpecularGI;
    uint bEnableDirectDiffuse;
    uint bEnableDirectSpecular;
    uint bEnableRTAO;
    uint bWritePrimaryGBuffer;
    float SpecularMotionVectorScale;
    uint bStabilizePrimaryRaySamples;
    uint _rtaoPadding;
    uint _pointLightPadding0;
    uint PointLightCount;
    float3 PointLightPadding;
};

cbuffer PathCompaction : register(b1)
{
    uint PathCompactionRenderWidth;
    uint PathCompactionRenderHeight;
    uint PathCompactionCapacity;
    uint PathCompactionBounceIndex;
};

cbuffer PathCompactionIndirect : register(b2)
{
    uint PathCompactionRayGenStartLo;
    uint PathCompactionRayGenStartHi;
    uint PathCompactionRayGenSizeLo;
    uint PathCompactionRayGenSizeHi;
    uint PathCompactionMissStartLo;
    uint PathCompactionMissStartHi;
    uint PathCompactionMissSizeLo;
    uint PathCompactionMissSizeHi;
    uint PathCompactionMissStrideLo;
    uint PathCompactionMissStrideHi;
    uint PathCompactionHitStartLo;
    uint PathCompactionHitStartHi;
    uint PathCompactionHitSizeLo;
    uint PathCompactionHitSizeHi;
    uint PathCompactionHitStrideLo;
    uint PathCompactionHitStrideHi;
    uint PathCompactionCallableStartLo;
    uint PathCompactionCallableStartHi;
    uint PathCompactionCallableSizeLo;
    uint PathCompactionCallableSizeHi;
    uint PathCompactionCallableStrideLo;
    uint PathCompactionCallableStrideHi;
    uint PathCompactionMaxDispatchWidth;
    uint PathCompactionCounterIndex;
    uint PathCompactionDispatchHeight;
    uint PathCompactionDispatchDepth;
    uint PathCompactionIndirectPadding0;
    uint PathCompactionIndirectPadding1;
};

uint pcg_hash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float random_float(inout uint seed)
{
    seed = pcg_hash(seed);
    return float(seed) / 4294967296.0f;
}

uint init_path_seed(uint2 pixel, uint frameIndex, uint sampleIndex)
{
    uint seed = pixel.x * 0x9E3779B9u;
    seed ^= pixel.y * 0xBB67AE85u;
    seed ^= frameIndex * 0x3C6EF372u;
    seed ^= sampleIndex * 0xA54FF53Au;
    seed = pcg_hash(seed ^ 0x510E527Fu);
    seed ^= pcg_hash(seed + pixel.x + 0x1F83D9ABu);
    seed ^= pcg_hash(seed + pixel.y + 0x5BE0CD19u);
    return seed | 1u;
}

float3 clamp_firefly(float3 radiance)
{
    const float kMaxRadiance = 48.0f;
    float maxChannel = max(radiance.x, max(radiance.y, radiance.z));
    if (maxChannel > kMaxRadiance)
        radiance *= kMaxRadiance / maxChannel;
    return radiance;
}

void store_u64(uint byteOffset, uint lo, uint hi)
{
    IndirectArgs.Store2(byteOffset, uint2(lo, hi));
}

[numthreads(1, 1, 1)]
void PathTracingCompactionIndirectArgsCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint activeCount = 0u;
    if (PathCompactionCounterIndex < 16u)
        activeCount = Counters[PathCompactionCounterIndex];

    uint dispatchWidth = min(activeCount, PathCompactionMaxDispatchWidth);
    dispatchWidth = max(dispatchWidth, 1u);
    uint dispatchHeight = max(PathCompactionDispatchHeight, 1u);
    uint dispatchDepth = max(PathCompactionDispatchDepth, 1u);

    // D3D12_DISPATCH_RAYS_DESC:
    // 0: RayGenerationShaderRecord, 16: MissShaderTable, 40: HitGroupTable,
    // 64: CallableShaderTable, 88: Width/Height/Depth.
    store_u64(0u, PathCompactionRayGenStartLo, PathCompactionRayGenStartHi);
    store_u64(8u, PathCompactionRayGenSizeLo, PathCompactionRayGenSizeHi);
    store_u64(16u, PathCompactionMissStartLo, PathCompactionMissStartHi);
    store_u64(24u, PathCompactionMissSizeLo, PathCompactionMissSizeHi);
    store_u64(32u, PathCompactionMissStrideLo, PathCompactionMissStrideHi);
    store_u64(40u, PathCompactionHitStartLo, PathCompactionHitStartHi);
    store_u64(48u, PathCompactionHitSizeLo, PathCompactionHitSizeHi);
    store_u64(56u, PathCompactionHitStrideLo, PathCompactionHitStrideHi);
    store_u64(64u, PathCompactionCallableStartLo, PathCompactionCallableStartHi);
    store_u64(72u, PathCompactionCallableSizeLo, PathCompactionCallableSizeHi);
    store_u64(80u, PathCompactionCallableStrideLo, PathCompactionCallableStrideHi);
    IndirectArgs.Store(88u, dispatchWidth);
    IndirectArgs.Store(92u, dispatchHeight);
    IndirectArgs.Store(96u, dispatchDepth);
    IndirectArgs.Store(100u, 0u);
}

[numthreads(8, 8, 1)]
void PathTracingCompactionSeedCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x == 0u && dispatchThreadID.y == 0u)
    {
        [unroll]
        for (uint counterIndex = 0u; counterIndex < 16u; ++counterIndex)
            Counters[counterIndex] = 0u;
        Counters[0] = PathCompactionCapacity;
    }

    if (dispatchThreadID.x >= PathCompactionRenderWidth ||
        dispatchThreadID.y >= PathCompactionRenderHeight)
    {
        return;
    }

    uint2 pixel = dispatchThreadID.xy;
    uint pathId = pixel.x + pixel.y * PathCompactionRenderWidth;
    if (pathId >= PathCompactionCapacity)
        return;

    float2 pixelCenter = float2(pixel) + float2(0.5f, 0.5f);
    float2 inUV = pixelCenter / float2(PathCompactionRenderWidth, PathCompactionRenderHeight);

    uint frameCounter = (DebugMode > 0u) ? 0u : BlueNoiseOffsetStride;
    uint seed = init_path_seed(pixel, frameCounter, 0u);
    float2 clipXY = inUV * 2.0f - 1.0f;
    bool bCenterPrimaryRay = bWritePrimaryGBuffer != 0u;
    if (DebugMode == 0u && !bCenterPrimaryRay)
    {
        float2 jitter = float2(random_float(seed), random_float(seed)) - 0.5f;
        clipXY += (2.0f * jitter) / float2(PathCompactionRenderWidth, PathCompactionRenderHeight);
    }

    clipXY.y = -clipXY.y;
    float4 viewFarH = mul(float4(clipXY, 1.0f, 1.0f), InvProjMatrix);
    float invViewFarW = abs(viewFarH.w) > 1.0e-6f ? rcp(viewFarH.w) : 1.0f;
    float3 viewRayDir = viewFarH.xyz * invViewFarW;
    if (any(isnan(viewRayDir)) || any(isinf(viewRayDir)) || dot(viewRayDir, viewRayDir) < 1.0e-8f)
        viewRayDir = float3(0.0f, 0.0f, -1.0f);
    else
        viewRayDir = normalize(viewRayDir);

    PathTracingWavefrontState state;
    state.Origin = mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
    state.Seed = seed;
    state.Direction = normalize(mul(float4(viewRayDir, 0), InvViewMatrix).xyz);
    state.PixelIndex = pathId;
    state.Throughput = float3(1, 1, 1);
    state.Bounce = 0u;
    state.Radiance = float3(0, 0, 0);
    state.Flags = PATH_TRACING_WAVEFRONT_FLAG_ACTIVE;

    StateOut[pathId] = state;
    ActiveListOut[pathId] = pathId;
    PathRadiance[pathId] = float4(0, 0, 0, 1);
}

[numthreads(8, 8, 1)]
void PathTracingCompactionResolveCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= PathCompactionRenderWidth ||
        dispatchThreadID.y >= PathCompactionRenderHeight)
    {
        return;
    }

    uint2 pixel = dispatchThreadID.xy;
    uint pathId = pixel.x + pixel.y * PathCompactionRenderWidth;
    if (pathId >= PathCompactionCapacity)
        return;

    float3 radiance = PathRadiance[pathId].xyz;
    if (any(isnan(radiance)) || any(isinf(radiance)))
        radiance = float3(0, 0, 0);
    radiance = clamp_firefly(max(radiance, 0.0f.xxx));

    float3 finalColor;
    if (FrameCounter == 0u || DebugMode > 0u)
    {
        finalColor = radiance;
    }
    else
    {
        float3 prevColor = OutputColor[pixel].xyz;
        if (any(isnan(prevColor)) || any(isinf(prevColor)))
            prevColor = float3(0, 0, 0);
        float n = float(FrameCounter);
        finalColor = (prevColor * n + radiance) / (n + 1.0f);
    }

    if (any(isnan(finalColor)) || any(isinf(finalColor)))
        finalColor = float3(0, 0, 0);

    OutputColor[pixel] = float4(finalColor, 1.0f);
}
