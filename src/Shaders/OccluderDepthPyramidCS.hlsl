Texture2D<float> SourceDepth : register(t0);
RWStructuredBuffer<float> DepthPyramid : register(u0);

cbuffer OccluderDepthPyramidCB : register(b0)
{
    uint Mode;
    uint SrcOffset;
    uint DstOffset;
    uint SrcWidth;
    uint SrcHeight;
    uint DstWidth;
    uint DstHeight;
    uint Pad0;
};

float LoadSourceDepthClamped(uint2 pixel)
{
    pixel.x = min(pixel.x, SrcWidth - 1u);
    pixel.y = min(pixel.y, SrcHeight - 1u);
    return SourceDepth.Load(int3(pixel, 0));
}

float LoadPyramidDepthClamped(uint2 pixel)
{
    pixel.x = min(pixel.x, SrcWidth - 1u);
    pixel.y = min(pixel.y, SrcHeight - 1u);
    return DepthPyramid[SrcOffset + pixel.y * SrcWidth + pixel.x];
}

[numthreads(8, 8, 1)]
void BuildOccluderDepthPyramidCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DstWidth || dispatchThreadId.y >= DstHeight)
        return;

    const uint2 dstPixel = dispatchThreadId.xy;
    const uint2 srcPixel = dstPixel * 2u;

    float maxDepth = 0.0f;
    if (Mode == 0u)
    {
        maxDepth = max(max(LoadSourceDepthClamped(srcPixel + uint2(0u, 0u)),
                           LoadSourceDepthClamped(srcPixel + uint2(1u, 0u))),
                       max(LoadSourceDepthClamped(srcPixel + uint2(0u, 1u)),
                           LoadSourceDepthClamped(srcPixel + uint2(1u, 1u))));
    }
    else
    {
        maxDepth = max(max(LoadPyramidDepthClamped(srcPixel + uint2(0u, 0u)),
                           LoadPyramidDepthClamped(srcPixel + uint2(1u, 0u))),
                       max(LoadPyramidDepthClamped(srcPixel + uint2(0u, 1u)),
                           LoadPyramidDepthClamped(srcPixel + uint2(1u, 1u))));
    }

    DepthPyramid[DstOffset + dstPixel.y * DstWidth + dstPixel.x] = maxDepth;
}
