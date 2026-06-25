#ifndef POINT_LIGHT_GRID_HLSLI
#define POINT_LIGHT_GRID_HLSLI

#ifndef POINT_LIGHT_GRID_CB_REGISTER
#define POINT_LIGHT_GRID_CB_REGISTER b1
#endif

#ifndef POINT_LIGHT_GRID_COUNTS_REGISTER
#define POINT_LIGHT_GRID_COUNTS_REGISTER t5
#endif

#ifndef POINT_LIGHT_GRID_INDICES_REGISTER
#define POINT_LIGHT_GRID_INDICES_REGISTER t6
#endif

cbuffer PointLightGridCB : register(POINT_LIGHT_GRID_CB_REGISTER)
{
    float4 PointLightGridMinAndEnabled;       // xyz = world-space min, w = enabled
    float4 PointLightGridInvExtentAndRes;     // xyz = reciprocal world extent, w = resolution
    uint4 PointLightGridCountsParam;          // x = point light count, y = resolution, z = max lights/cell
};

StructuredBuffer<uint> PointLightGridCounts : register(POINT_LIGHT_GRID_COUNTS_REGISTER);
StructuredBuffer<uint> PointLightGridIndices : register(POINT_LIGHT_GRID_INDICES_REGISTER);

bool PointLightGridIsEnabled()
{
    return PointLightGridMinAndEnabled.w > 0.5f &&
        PointLightGridCountsParam.x > 0u &&
        PointLightGridCountsParam.y > 0u &&
        PointLightGridCountsParam.z > 0u;
}

uint PointLightGridGetPointLightCount()
{
    return PointLightGridCountsParam.x;
}

uint PointLightGridGetMaxCount()
{
    return PointLightGridCountsParam.z;
}

bool PointLightGridProjectCell(float3 worldPosition, uint axis, out uint cellIndex, out uint count)
{
    const uint resolution = max(PointLightGridCountsParam.y, 1u);
    const float resolutionF = max(PointLightGridInvExtentAndRes.w, 1.0f);
    const float3 rel3 = (worldPosition - PointLightGridMinAndEnabled.xyz) * PointLightGridInvExtentAndRes.xyz;

    float2 rel = rel3.yz;
    if (axis == 1u)
        rel = rel3.xz;
    else if (axis == 2u)
        rel = rel3.xy;

    if (rel.x < 0.0f || rel.y < 0.0f || rel.x > 1.0f || rel.y > 1.0f)
    {
        cellIndex = 0u;
        count = 0u;
        return false;
    }

    uint2 cell = min((uint2)floor(saturate(rel) * resolutionF), uint2(resolution - 1u, resolution - 1u));
    cellIndex = axis * resolution * resolution + cell.y * resolution + cell.x;
    count = min(PointLightGridCounts[cellIndex], PointLightGridCountsParam.z);
    return true;
}

uint PointLightGridSelectCandidateCount(float3 worldPosition, out uint bestCellIndex)
{
    bestCellIndex = 0u;
    if (!PointLightGridIsEnabled())
        return 0u;

    uint bestCount = 0xffffffffu;
    bool foundCell = false;
    [unroll]
    for (uint axis = 0u; axis < 3u; ++axis)
    {
        uint cellIndex = 0u;
        uint count = 0u;
        if (!PointLightGridProjectCell(worldPosition, axis, cellIndex, count))
            continue;

        if (!foundCell || count < bestCount)
        {
            foundCell = true;
            bestCellIndex = cellIndex;
            bestCount = count;
        }
    }

    return foundCell ? bestCount : 0u;
}

uint PointLightGridLoadLightIndex(uint cellIndex, uint candidateIndex)
{
    return PointLightGridIndices[cellIndex * PointLightGridCountsParam.z + candidateIndex];
}

bool PointLightGridCellContainsLightIndex(uint cellIndex, uint candidateCount, uint lightIndex)
{
    const uint count = min(candidateCount, PointLightGridCountsParam.z);
    [loop]
    for (uint candidateIndex = 0u; candidateIndex < 128u; ++candidateIndex)
    {
        if (candidateIndex >= count)
            break;
        if (PointLightGridLoadLightIndex(cellIndex, candidateIndex) == lightIndex)
            return true;
    }
    return false;
}

#endif
