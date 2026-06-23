RaytracingAccelerationStructure GBufferRtScene : register(t0);

struct GBufferObjectDrawRange
{
    uint FirstDraw;
    uint DrawCount;
    uint OpaqueDrawCount;
    uint AlphaDrawCount;
};

StructuredBuffer<uint> RTInstanceSceneObjectIndices : register(t1);
StructuredBuffer<GBufferObjectDrawRange> GBufferObjectRanges : register(t2);

RWStructuredBuffer<uint> SparseRayCandidateObjectIndices : register(u0);
RWStructuredBuffer<uint> SparseRayObjectHitFlags : register(u1);
RWStructuredBuffer<uint> SparseRayCounters : register(u2);

cbuffer GBufferSparseRayOccluderCB : register(b0)
{
    float4x4 InvViewProjectionMatrix;
    float4 CameraPositionAndTMin;
    uint RayGridWidth;
    uint RayGridHeight;
    uint MaxOccluderObjects;
    uint ObjectCount;
    uint RTInstanceCount;
    uint FrameIndex;
    uint Pad0;
    uint Pad1;
};

static const uint kInvalidObjectIndex = 0xffffffffu;

uint HashSparseRaySample(uint2 p, uint frame)
{
    uint x = p.x * 747796405u ^ p.y * 2891336453u ^ frame * 277803737u;
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    x ^= x >> 16;
    return x;
}

float2 SparseRayJitter(uint2 p, uint frame)
{
    const uint h = HashSparseRaySample(p, frame);
    return float2((h & 0x0000ffffu) * (1.0f / 65536.0f),
                  ((h >> 16u) & 0x0000ffffu) * (1.0f / 65536.0f));
}

[numthreads(64, 1, 1)]
void ClearSparseRayOccluderSelectionCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index < MaxOccluderObjects)
        SparseRayCandidateObjectIndices[index] = kInvalidObjectIndex;
    if (index < ObjectCount)
        SparseRayObjectHitFlags[index] = 0u;
    if (index == 0u)
    {
        SparseRayCounters[0] = 0u;
        SparseRayCounters[1] = 0u;
    }
}

[numthreads(8, 8, 1)]
void SelectSparseRayOccludersCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= RayGridWidth || dispatchThreadId.y >= RayGridHeight)
        return;

    const uint2 samplePixel = dispatchThreadId.xy;
    const float2 jitter = SparseRayJitter(samplePixel, FrameIndex);
    const float2 uv = (float2(samplePixel) + jitter) / max(float2(RayGridWidth, RayGridHeight), float2(1.0f, 1.0f));
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float4 nearWorld = mul(float4(ndc, 0.0f, 1.0f), InvViewProjectionMatrix);
    float4 farWorld = mul(float4(ndc, 1.0f, 1.0f), InvViewProjectionMatrix);
    if (abs(nearWorld.w) < 1.0e-6f || abs(farWorld.w) < 1.0e-6f)
        return;

    nearWorld.xyz /= nearWorld.w;
    farWorld.xyz /= farWorld.w;

    RayDesc ray;
    ray.Origin = CameraPositionAndTMin.xyz;
    ray.Direction = normalize(farWorld.xyz - ray.Origin);
    ray.TMin = max(CameraPositionAndTMin.w, 0.001f);
    ray.TMax = 100000.0f;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
             RAY_FLAG_FORCE_OPAQUE |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(GBufferRtScene, RAY_FLAG_NONE, 0xffu, ray);
    while (q.Proceed())
    {
    }
    if (q.CommittedStatus() == COMMITTED_NOTHING)
        return;

    const uint rtInstanceIndex = q.CommittedInstanceID();
    if (rtInstanceIndex >= RTInstanceCount)
        return;

    const uint objectIndex = RTInstanceSceneObjectIndices[rtInstanceIndex];
    if (objectIndex == kInvalidObjectIndex || objectIndex >= ObjectCount)
        return;

    const GBufferObjectDrawRange range = GBufferObjectRanges[objectIndex];
    if (range.OpaqueDrawCount == 0u)
        return;

    uint previous = 0u;
    InterlockedCompareExchange(SparseRayObjectHitFlags[objectIndex], 0u, 1u, previous);
    if (previous != 0u)
        return;

    uint dstIndex = 0u;
    InterlockedAdd(SparseRayCounters[0], 1u, dstIndex);
    if (dstIndex < MaxOccluderObjects)
    {
        SparseRayCandidateObjectIndices[dstIndex] = objectIndex;
    }
    else
    {
        InterlockedAdd(SparseRayCounters[1], 1u);
    }
}
