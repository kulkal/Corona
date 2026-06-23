struct GBufferObjectDrawRange
{
    uint FirstDraw;
    uint DrawCount;
    uint OpaqueDrawCount;
    uint AlphaDrawCount;
};

struct GBufferCachedDrawInfo
{
    uint DrawRecordIndex;
    uint IndexCount;
    uint Flags;
    uint Pad;
};

struct GBufferObjectBounds
{
    float4 BoundsMin;
    float4 BoundsMax;
};

struct DrawIndirectArguments
{
    uint VertexCountPerInstance;
    uint InstanceCount;
    uint StartVertexLocation;
    uint StartInstanceLocation;
};

cbuffer GBufferCullCB : register(b0)
{
    uint CandidateCount;
    uint ObjectRangeCount;
    uint DrawInfoCount;
    uint MaxOutputDraws;
    float4 FrustumPlanes[6];
    float4x4 ViewProjectionMatrix;
    uint EnableFrustumCull;
    uint EnableHiZOcclusion;
    uint DepthPyramidWidth;
    uint DepthPyramidHeight;
    uint DepthPyramidMipCount;
    uint DepthPyramidMaxMip;
    float HiZDepthBias;
    uint Pad0;
    uint4 DepthPyramidMipOffsets[4];
};

StructuredBuffer<GBufferObjectDrawRange> GBufferObjectRanges : register(t0);
StructuredBuffer<GBufferCachedDrawInfo> GBufferCachedDraws : register(t1);
StructuredBuffer<uint> GBufferCandidateObjectIndices : register(t2);
StructuredBuffer<GBufferObjectBounds> GBufferObjectBoundsBuffer : register(t3);
StructuredBuffer<float> GBufferDepthPyramid : register(t4);

RWStructuredBuffer<DrawIndirectArguments> GBufferOpaqueArgs : register(u0);
RWStructuredBuffer<DrawIndirectArguments> GBufferAlphaArgs : register(u1);
RWStructuredBuffer<uint> GBufferCullCounters : register(u2);

[numthreads(1, 1, 1)]
void ClearGBufferIndirectCountersCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x == 0)
    {
        GBufferCullCounters[0] = 0;
        GBufferCullCounters[1] = 0;
    }
}

bool IsAabbInFrustum(float3 boundsMin, float3 boundsMax)
{
    const float3 center = (boundsMin + boundsMax) * 0.5f;
    const float3 extents = max((boundsMax - boundsMin) * 0.5f, 0.0f.xxx);
    [unroll]
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex)
    {
        const float4 plane = FrustumPlanes[planeIndex];
        const float3 normal = plane.xyz;
        const float distance = dot(normal, center) + plane.w;
        const float radius = dot(abs(normal), extents);
        if (distance + radius < 0.0f)
            return false;
    }
    return true;
}

uint GetDepthPyramidMipOffset(uint mip)
{
    const uint4 offsets = DepthPyramidMipOffsets[mip >> 2u];
    return offsets[mip & 3u];
}

uint2 GetDepthPyramidMipSize(uint mip)
{
    const uint divisor = 1u << min(mip, 30u);
    return uint2(max(1u, (DepthPyramidWidth + divisor - 1u) / divisor),
                 max(1u, (DepthPyramidHeight + divisor - 1u) / divisor));
}

float LoadDepthPyramid(uint mip, uint2 pixel)
{
    const uint2 mipSize = GetDepthPyramidMipSize(mip);
    pixel = min(pixel, mipSize - 1u);
    return GBufferDepthPyramid[GetDepthPyramidMipOffset(mip) + pixel.y * mipSize.x + pixel.x];
}

bool IsAabbOccludedByHiZ(float3 boundsMin, float3 boundsMax)
{
    if (EnableHiZOcclusion == 0u ||
        DepthPyramidWidth == 0u ||
        DepthPyramidHeight == 0u ||
        DepthPyramidMipCount == 0u)
    {
        return false;
    }

    const float3 corners[8] =
    {
        float3(boundsMin.x, boundsMin.y, boundsMin.z),
        float3(boundsMax.x, boundsMin.y, boundsMin.z),
        float3(boundsMin.x, boundsMax.y, boundsMin.z),
        float3(boundsMax.x, boundsMax.y, boundsMin.z),
        float3(boundsMin.x, boundsMin.y, boundsMax.z),
        float3(boundsMax.x, boundsMin.y, boundsMax.z),
        float3(boundsMin.x, boundsMax.y, boundsMax.z),
        float3(boundsMax.x, boundsMax.y, boundsMax.z)
    };

    float2 minNdc = float2(1.0f, 1.0f);
    float2 maxNdc = float2(-1.0f, -1.0f);
    float nearestDepth = 1.0f;
    bool anyProjected = false;

    [unroll]
    for (uint cornerIndex = 0; cornerIndex < 8u; ++cornerIndex)
    {
        const float4 clip = mul(float4(corners[cornerIndex], 1.0f), ViewProjectionMatrix);
        if (clip.w <= 0.0001f)
            return false;

        const float3 ndc = clip.xyz / clip.w;
        minNdc = min(minNdc, ndc.xy);
        maxNdc = max(maxNdc, ndc.xy);
        nearestDepth = min(nearestDepth, ndc.z);
        anyProjected = true;
    }

    if (!anyProjected || nearestDepth <= 0.0f || nearestDepth >= 1.0f)
        return false;

    minNdc = clamp(minNdc, -1.0f.xx, 1.0f.xx);
    maxNdc = clamp(maxNdc, -1.0f.xx, 1.0f.xx);
    if (maxNdc.x <= minNdc.x || maxNdc.y <= minNdc.y)
        return false;

    float2 minPixelF = float2((minNdc.x * 0.5f + 0.5f) * DepthPyramidWidth,
                              (1.0f - (maxNdc.y * 0.5f + 0.5f)) * DepthPyramidHeight);
    float2 maxPixelF = float2((maxNdc.x * 0.5f + 0.5f) * DepthPyramidWidth,
                              (1.0f - (minNdc.y * 0.5f + 0.5f)) * DepthPyramidHeight);

    minPixelF = clamp(minPixelF - 2.0f.xx, 0.0f.xx, float2(DepthPyramidWidth - 1u, DepthPyramidHeight - 1u));
    maxPixelF = clamp(maxPixelF + 2.0f.xx, 0.0f.xx, float2(DepthPyramidWidth - 1u, DepthPyramidHeight - 1u));

    const float2 rectSizeF = max(maxPixelF - minPixelF, 1.0f.xx);
    if (rectSizeF.x > DepthPyramidWidth * 0.75f || rectSizeF.y > DepthPyramidHeight * 0.75f)
        return false;

    uint mip = 0u;
    uint rectExtent = (uint)ceil(max(rectSizeF.x, rectSizeF.y));
    while (rectExtent > 2u && mip + 1u < DepthPyramidMipCount && mip < DepthPyramidMaxMip)
    {
        rectExtent = (rectExtent + 1u) >> 1u;
        ++mip;
    }

    const uint2 mipSize = GetDepthPyramidMipSize(mip);
    uint2 minPixel = (uint2)floor(minPixelF / exp2((float)mip));
    uint2 maxPixel = (uint2)ceil(maxPixelF / exp2((float)mip));
    minPixel = min(minPixel, mipSize - 1u);
    maxPixel = min(maxPixel, mipSize - 1u);

    const float objectDepth = saturate(nearestDepth - HiZDepthBias);
    const uint2 centerPixel = (minPixel + maxPixel) >> 1u;
    const float d0 = LoadDepthPyramid(mip, minPixel);
    const float d1 = LoadDepthPyramid(mip, uint2(maxPixel.x, minPixel.y));
    const float d2 = LoadDepthPyramid(mip, uint2(minPixel.x, maxPixel.y));
    const float d3 = LoadDepthPyramid(mip, maxPixel);
    const float d4 = LoadDepthPyramid(mip, centerPixel);
    const float maxSampleDepth = max(max(max(d0, d1), max(d2, d3)), d4);

    return maxSampleDepth < objectDepth;
}

[numthreads(64, 1, 1)]
void BuildGBufferIndirectArgsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint candidateIndex = dispatchThreadId.x;
    if (candidateIndex >= CandidateCount)
        return;

    const uint objectIndex = GBufferCandidateObjectIndices[candidateIndex];
    if (objectIndex == 0xffffffffu)
        return;
    if (objectIndex >= ObjectRangeCount)
        return;

    if (EnableFrustumCull != 0)
    {
        const GBufferObjectBounds objectBounds = GBufferObjectBoundsBuffer[objectIndex];
        if (objectBounds.BoundsMin.w != 0.0f &&
            !IsAabbInFrustum(objectBounds.BoundsMin.xyz, objectBounds.BoundsMax.xyz))
        {
            return;
        }
    }

    if (EnableHiZOcclusion != 0)
    {
        const GBufferObjectBounds objectBounds = GBufferObjectBoundsBuffer[objectIndex];
        if (objectBounds.BoundsMin.w != 0.0f &&
            IsAabbOccludedByHiZ(objectBounds.BoundsMin.xyz, objectBounds.BoundsMax.xyz))
        {
            return;
        }
    }

    const GBufferObjectDrawRange range = GBufferObjectRanges[objectIndex];
    for (uint drawOffset = 0; drawOffset < range.DrawCount; ++drawOffset)
    {
        const uint cachedDrawIndex = range.FirstDraw + drawOffset;
        if (cachedDrawIndex >= DrawInfoCount)
            break;

        const GBufferCachedDrawInfo drawInfo = GBufferCachedDraws[cachedDrawIndex];
        if (drawInfo.IndexCount == 0)
            continue;

        DrawIndirectArguments args;
        args.VertexCountPerInstance = drawInfo.IndexCount;
        args.InstanceCount = 1;
        args.StartVertexLocation = 0;
        args.StartInstanceLocation = drawInfo.DrawRecordIndex;

        uint dstIndex = 0;
        if ((drawInfo.Flags & 1u) != 0)
        {
            InterlockedAdd(GBufferCullCounters[1], 1, dstIndex);
            if (dstIndex < MaxOutputDraws)
                GBufferAlphaArgs[dstIndex] = args;
        }
        else
        {
            InterlockedAdd(GBufferCullCounters[0], 1, dstIndex);
            if (dstIndex < MaxOutputDraws)
                GBufferOpaqueArgs[dstIndex] = args;
        }
    }
}
