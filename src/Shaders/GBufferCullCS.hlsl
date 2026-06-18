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
};

StructuredBuffer<GBufferObjectDrawRange> GBufferObjectRanges : register(t0);
StructuredBuffer<GBufferCachedDrawInfo> GBufferCachedDraws : register(t1);
StructuredBuffer<uint> GBufferCandidateObjectIndices : register(t2);

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

[numthreads(64, 1, 1)]
void BuildGBufferIndirectArgsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint candidateIndex = dispatchThreadId.x;
    if (candidateIndex >= CandidateCount)
        return;

    const uint objectIndex = GBufferCandidateObjectIndices[candidateIndex];
    if (objectIndex >= ObjectRangeCount)
        return;

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
