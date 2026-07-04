#include "Common.hlsl"

struct RoadDecalData
{
    float4 CenterHalfLength;
    float4 AxisXHalfWidth;
    float4 AxisZHeight;
    float4 BaseColorRoughness;
    float4 Params;
    float4 BoundsMin;
    float4 BoundsMax;
    float4 Polygon01;
    float4 Polygon23;
    float4 Polygon45;
    float4 Polygon67;
    float4 Polygon89;
    float4 PolygonAB;
    float4 PolygonCD;
    float4 PolygonEF;
};

cbuffer RoadDecalCullCB : register(b0)
{
    uint DecalCount;
    uint TileCountX;
    uint TileCountY;
    uint MaxDecalsPerTile;
    float2 RTSize;
    float2 CullPadding;
    float4 FrustumPlanes[6];
    float4x4 ViewProjectionMatrix;
    uint EnableFrustumCull;
    uint EnableHiZOcclusion;
    uint DepthPyramidWidth;
    uint DepthPyramidHeight;
    uint DepthPyramidMipCount;
    uint DepthPyramidMaxMip;
    float HiZDepthBias;
    uint CullPad0;
    uint4 DepthPyramidMipOffsets[4];
};

cbuffer RoadDecalApplyCB : register(b1)
{
    float4x4 InvViewMatrix;
    float4x4 InvProjMatrix;
    float2 ApplyRTSize;
    uint ApplyTileCountX;
    uint ApplyMaxDecalsPerTile;
};

StructuredBuffer<RoadDecalData> RoadDecals : register(t0);
StructuredBuffer<float> RoadDecalDepthPyramid : register(t1);

Texture2D<float> DepthTex : register(t5);
Texture2D<float4> NormalTex : register(t6);
StructuredBuffer<RoadDecalData> ApplyRoadDecals : register(t7);
StructuredBuffer<uint> ApplyTileCounts : register(t8);
StructuredBuffer<uint> ApplyTileIndices : register(t9);

RWStructuredBuffer<uint> RoadDecalTileCounts : register(u0);
RWStructuredBuffer<uint> RoadDecalTileIndices : register(u1);

RWTexture2D<float4> AlbedoTex : register(u2);
RWTexture2D<float4> RoughnessMetallicTex : register(u3);

static const uint RoadDecalTileSize = 16u;

float GetRoadSurfaceLayerTolerance(float halfWidth, float heightTolerance)
{
    const float footprintTolerance = min(6.0f, max(3.0f, halfWidth * 0.035f));
    return min(heightTolerance, footprintTolerance);
}

uint GetTileCount()
{
    return TileCountX * TileCountY;
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
    return RoadDecalDepthPyramid[GetDepthPyramidMipOffset(mip) + pixel.y * mipSize.x + pixel.x];
}

bool IsAabbInFrustum(float3 boundsMin, float3 boundsMax)
{
    const float3 center = (boundsMin + boundsMax) * 0.5f;
    const float3 extents = max((boundsMax - boundsMin) * 0.5f, 0.0f.xxx);
    [unroll]
    for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
    {
        const float4 plane = FrustumPlanes[planeIndex];
        const float distance = dot(plane.xyz, center) + plane.w;
        const float radius = dot(abs(plane.xyz), extents);
        if (distance + radius < 0.0f)
            return false;
    }
    return true;
}

bool ProjectAabbToScreenRect(float3 boundsMin, float3 boundsMax, out uint2 minPixel, out uint2 maxPixel, out float nearestDepth)
{
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

    float2 minNdc = 1.0f.xx;
    float2 maxNdc = -1.0f.xx;
    nearestDepth = 1.0f;
    bool anyBehindNear = false;

    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        const float4 clip = mul(float4(corners[i], 1.0f), ViewProjectionMatrix);
        if (clip.w <= 0.0001f)
        {
            anyBehindNear = true;
            continue;
        }

        const float3 ndc = clip.xyz / clip.w;
        minNdc = min(minNdc, ndc.xy);
        maxNdc = max(maxNdc, ndc.xy);
        nearestDepth = min(nearestDepth, ndc.z);
    }

    if (anyBehindNear)
    {
        minPixel = uint2(0u, 0u);
        maxPixel = uint2((uint)max(RTSize.x, 1.0f) - 1u, (uint)max(RTSize.y, 1.0f) - 1u);
        nearestDepth = 0.0f;
        return true;
    }

    if (maxNdc.x < -1.0f || minNdc.x > 1.0f || maxNdc.y < -1.0f || minNdc.y > 1.0f)
        return false;
    if (nearestDepth <= 0.0f || nearestDepth >= 1.0f)
        return false;

    minNdc = clamp(minNdc, -1.0f.xx, 1.0f.xx);
    maxNdc = clamp(maxNdc, -1.0f.xx, 1.0f.xx);

    float2 minPixelF = float2((minNdc.x * 0.5f + 0.5f) * RTSize.x,
                              (1.0f - (maxNdc.y * 0.5f + 0.5f)) * RTSize.y);
    float2 maxPixelF = float2((maxNdc.x * 0.5f + 0.5f) * RTSize.x,
                              (1.0f - (minNdc.y * 0.5f + 0.5f)) * RTSize.y);

    minPixelF = clamp(floor(minPixelF) - 2.0f.xx, 0.0f.xx, RTSize - 1.0f.xx);
    maxPixelF = clamp(ceil(maxPixelF) + 2.0f.xx, 0.0f.xx, RTSize - 1.0f.xx);
    if (maxPixelF.x < minPixelF.x || maxPixelF.y < minPixelF.y)
        return false;

    minPixel = (uint2)minPixelF;
    maxPixel = (uint2)maxPixelF;
    return true;
}

bool IsRectOccludedByHiZ(uint2 minPixelFullRes, uint2 maxPixelFullRes, float nearestDepth)
{
    if (EnableHiZOcclusion == 0u ||
        DepthPyramidWidth == 0u ||
        DepthPyramidHeight == 0u ||
        DepthPyramidMipCount == 0u ||
        nearestDepth <= 0.0f)
    {
        return false;
    }

    float2 minPixelF = (float2)minPixelFullRes * 0.5f;
    float2 maxPixelF = (float2)maxPixelFullRes * 0.5f;
    minPixelF = clamp(minPixelF - 1.0f.xx, 0.0f.xx, float2(DepthPyramidWidth - 1u, DepthPyramidHeight - 1u));
    maxPixelF = clamp(maxPixelF + 1.0f.xx, 0.0f.xx, float2(DepthPyramidWidth - 1u, DepthPyramidHeight - 1u));

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
    const float divisor = exp2((float)mip);
    uint2 minPixel = (uint2)floor(minPixelF / divisor);
    uint2 maxPixel = (uint2)ceil(maxPixelF / divisor);
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
void ClearRoadDecalTilesCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint tileIndex = dispatchThreadId.x;
    if (tileIndex < GetTileCount())
        RoadDecalTileCounts[tileIndex] = 0u;
}

[numthreads(64, 1, 1)]
void BuildRoadDecalTilesCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint decalIndex = dispatchThreadId.x;
    if (decalIndex >= DecalCount)
        return;

    const RoadDecalData decal = RoadDecals[decalIndex];
    const float3 boundsMin = decal.BoundsMin.xyz;
    const float3 boundsMax = decal.BoundsMax.xyz;
    if (EnableFrustumCull != 0u && !IsAabbInFrustum(boundsMin, boundsMax))
        return;

    uint2 minPixel = uint2(0u, 0u);
    uint2 maxPixel = uint2(0u, 0u);
    float nearestDepth = 1.0f;
    if (!ProjectAabbToScreenRect(boundsMin, boundsMax, minPixel, maxPixel, nearestDepth))
        return;
    if (IsRectOccludedByHiZ(minPixel, maxPixel, nearestDepth))
        return;

    const uint2 minTile = minPixel / RoadDecalTileSize;
    const uint2 maxTile = min(maxPixel / RoadDecalTileSize, uint2(TileCountX - 1u, TileCountY - 1u));
    for (uint tileY = minTile.y; tileY <= maxTile.y; ++tileY)
    {
        for (uint tileX = minTile.x; tileX <= maxTile.x; ++tileX)
        {
            const uint tileIndex = tileY * TileCountX + tileX;
            uint slot = 0u;
            InterlockedAdd(RoadDecalTileCounts[tileIndex], 1u, slot);
            if (slot < MaxDecalsPerTile)
                RoadDecalTileIndices[tileIndex * MaxDecalsPerTile + slot] = decalIndex;
        }
    }
}

float3 ReconstructWorldPosition(uint2 pixel, float deviceDepth)
{
    float2 uv = (float2(pixel) + 0.5f) / max(ApplyRTSize, 1.0f.xx);
    float2 screenPosition = uv * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;
    float3 viewPosition = GetViewPosition(deviceDepth, screenPosition, InvProjMatrix);
    return mul(float4(viewPosition, 1.0f), InvViewMatrix).xyz;
}

float Hash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float Cross2D(float2 a, float2 b)
{
    return a.x * b.y - a.y * b.x;
}

float2 LoadRoadDecalPolygonVertex(RoadDecalData decal, uint index)
{
    if (index == 0u)
        return decal.Polygon01.xy;
    if (index == 1u)
        return decal.Polygon01.zw;
    if (index == 2u)
        return decal.Polygon23.xy;
    if (index == 3u)
        return decal.Polygon23.zw;
    if (index == 4u)
        return decal.Polygon45.xy;
    if (index == 5u)
        return decal.Polygon45.zw;
    if (index == 6u)
        return decal.Polygon67.xy;
    if (index == 7u)
        return decal.Polygon67.zw;
    if (index == 8u)
        return decal.Polygon89.xy;
    if (index == 9u)
        return decal.Polygon89.zw;
    if (index == 10u)
        return decal.PolygonAB.xy;
    if (index == 11u)
        return decal.PolygonAB.zw;
    if (index == 12u)
        return decal.PolygonCD.xy;
    if (index == 13u)
        return decal.PolygonCD.zw;
    if (index == 14u)
        return decal.PolygonEF.xy;
    return decal.PolygonEF.zw;
}

bool EvaluateRoadDecalPolygonFootprint(RoadDecalData decal, float2 pointXZ, uint featherEdgeMask, out float edgeFade)
{
    bool hasPositive = false;
    bool hasNegative = false;
    bool hasAnyEdge = false;
    bool hasFeatherEdge = false;
    float minEdgeDistance = 1.0e20f;

    [unroll]
    for (uint i = 0u; i < 16u; ++i)
    {
        const float2 a = LoadRoadDecalPolygonVertex(decal, i);
        const float2 b = LoadRoadDecalPolygonVertex(decal, (i + 1u) & 15u);
        const float2 edge = b - a;
        const float edgeLength = length(edge);
        if (edgeLength < 0.5f)
            continue;

        const float signedArea = Cross2D(edge, pointXZ - a);
        hasPositive = hasPositive || signedArea > 0.01f;
        hasNegative = hasNegative || signedArea < -0.01f;
        if (hasPositive && hasNegative)
        {
            edgeFade = 0.0f;
            return false;
        }

        if ((featherEdgeMask & (1u << i)) != 0u)
        {
            minEdgeDistance = min(minEdgeDistance, abs(signedArea) / edgeLength);
            hasFeatherEdge = true;
        }
        hasAnyEdge = true;
    }

    if (!hasAnyEdge)
    {
        edgeFade = 0.0f;
        return false;
    }

    if (!hasFeatherEdge)
    {
        edgeFade = 1.0f;
        return true;
    }

    const float edgeFadeWidth = max(decal.AxisXHalfWidth.w * 0.08f, 8.0f);
    const float seamOverlapBias = max(decal.AxisXHalfWidth.w * 0.035f, 4.0f);
    edgeFade = saturate((minEdgeDistance + seamOverlapBias) / edgeFadeWidth);
    return true;
}

float3 EvaluateRoadColor(RoadDecalData decal, float localX, float localZ)
{
    const float halfLength = max(decal.CenterHalfLength.w, 1.0f);
    const float halfWidth = max(decal.AxisXHalfWidth.w, 1.0f);
    const float2 uv = float2((localX + halfLength) / max(halfLength * 2.0f, 1.0f),
                             (localZ + halfWidth) / max(halfWidth * 2.0f, 1.0f));
    const float2 repeat = max(decal.Params.xy, 0.01f.xx);
    const float fineNoise = Hash21(floor(uv * repeat * 128.0f));
    const float coarseNoise = Hash21(floor(uv * repeat * 23.0f));
    float asphalt = lerp(0.72f, 1.08f, fineNoise) * lerp(0.88f, 1.08f, coarseNoise);
    float3 color = decal.BaseColorRoughness.rgb * asphalt;

    const float dashPeriod = 220.0f;
    const float dashDuty = 0.48f;
    const float stripeHalfWidth = max(halfWidth * 0.018f, 2.5f);
    const bool centerStripe = abs(localZ) <= stripeHalfWidth;
    const bool dash = frac((localX + halfLength) / dashPeriod) < dashDuty;
    const bool drawCenterStripe = (decal.BoundsMin.w > 0.5f && decal.BoundsMin.w < 1.5f) || decal.BoundsMin.w > 2.5f;
    if (drawCenterStripe && centerStripe && dash)
        color = lerp(color, float3(0.78f, 0.74f, 0.36f), 0.88f);
    return saturate(color);
}

[numthreads(8, 8, 1)]
void ApplyRoadDecalsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)ApplyRTSize.x || pixel.y >= (uint)ApplyRTSize.y)
        return;

    const float deviceDepth = DepthTex.Load(int3(pixel, 0)).x;
    if (deviceDepth >= 0.999999f)
        return;

    const uint2 tile = pixel / RoadDecalTileSize;
    const uint tileIndex = tile.y * ApplyTileCountX + tile.x;
    const uint count = min(ApplyTileCounts[tileIndex], ApplyMaxDecalsPerTile);
    if (count == 0u)
        return;

    const float3 worldPosition = ReconstructWorldPosition(pixel, deviceDepth);
    const float3 worldNormal = CommonSafeNormalize(NormalTex.Load(int3(pixel, 0)).xyz, float3(0.0f, 1.0f, 0.0f));

    float4 albedo = AlbedoTex[pixel];
    float4 roughnessMetallic = RoughnessMetallicTex[pixel];
    bool modified = false;

    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        const uint decalIndex = ApplyTileIndices[tileIndex * ApplyMaxDecalsPerTile + i];
        const RoadDecalData decal = ApplyRoadDecals[decalIndex];
        const float3 center = decal.CenterHalfLength.xyz;
        const float halfLength = decal.CenterHalfLength.w;
        const float3 axisX = decal.AxisXHalfWidth.xyz;
        const float halfWidth = decal.AxisXHalfWidth.w;
        const float3 axisZ = decal.AxisZHeight.xyz;
        const float height = decal.AxisZHeight.w;
        const float heightTolerance = decal.Params.z;
        const float normalThreshold = decal.Params.w;
        const float roadSurfaceLayerTolerance = GetRoadSurfaceLayerTolerance(halfWidth, heightTolerance);

        if (dot(worldNormal, float3(0.0f, 1.0f, 0.0f)) < normalThreshold)
            continue;
        if (abs(worldPosition.y - height) > roadSurfaceLayerTolerance)
            continue;

        const float3 rel = worldPosition - center;
        const float localX = dot(rel, axisX);
        const float localZ = dot(rel, axisZ);
        const float decalMode = decal.BoundsMin.w;
        const bool polygonFootprint = abs(decalMode) > 1.5f;
        const bool fillOnlyMissingRoad = decalMode < -0.5f && !polygonFootprint;
        const float capRadius = fillOnlyMissingRoad ? 0.0f : halfWidth * 0.55f;
        const float effectiveHalfLength = halfLength + capRadius;
        float edgeFade = 0.0f;
        if (polygonFootprint)
        {
            const bool roadSegmentPolygon = decalMode > 2.5f;
            const uint featherEdgeMask = roadSegmentPolygon ? ((1u << 0u) | (1u << 2u)) : 0u;
            if (!EvaluateRoadDecalPolygonFootprint(decal, worldPosition.xz, featherEdgeMask, edgeFade))
                continue;
            edgeFade = roadSegmentPolygon ? saturate(0.72f + edgeFade * 0.28f) : 1.0f;
        }
        else if (fillOnlyMissingRoad)
        {
            const float2 normalizedPatch = float2(
                localX / max(halfLength, 1.0f),
                localZ / max(halfWidth, 1.0f));
            const float radial = length(normalizedPatch);
            if (radial > 1.0f)
                continue;

            const float3 approximateRoadBase = decal.BaseColorRoughness.rgb;
            const float currentRoadDistance = length(albedo.rgb - approximateRoadBase);
            const float currentLuma = dot(albedo.rgb, float3(0.2126f, 0.7152f, 0.0722f));
            if (currentRoadDistance < 0.22f && currentLuma < 0.38f)
                continue;

            edgeFade = saturate((1.0f - radial) * 5.0f);
        }
        else
        {
            if (abs(localX) > effectiveHalfLength || abs(localZ) > halfWidth)
                continue;
            const float edgeFadeX = saturate((effectiveHalfLength - abs(localX)) / max(halfWidth * 0.08f, 8.0f));
            const float edgeFadeZ = saturate((halfWidth - abs(localZ)) / max(halfWidth * 0.08f, 8.0f));
            edgeFade = min(edgeFadeX, edgeFadeZ);
        }

        const float alpha = saturate(edgeFade * decal.BaseColorRoughness.a);
        const float3 roadColor = EvaluateRoadColor(decal, localX, localZ);
        albedo.rgb = lerp(albedo.rgb, roadColor, alpha);
        roughnessMetallic.x = lerp(roughnessMetallic.x, saturate(decal.BoundsMax.w), alpha);
        roughnessMetallic.y = lerp(roughnessMetallic.y, 0.0f, alpha);
        modified = true;
    }

    if (modified)
    {
        AlbedoTex[pixel] = albedo;
        RoughnessMetallicTex[pixel] = roughnessMetallic;
    }
}
