// GrassProcedural.hlsl — vertex-pulling procedural grass.
//
// Coexists with the legacy VB-baked grass path. Instead of uploading
// 100M+ vertices to GPU memory, every blade's geometry is synthesized in
// the VS from SV_InstanceID + SV_VertexID. Pros: zero VB cost, blade-count
// / segments / seed changes are CB writes. Cons: heavier VS ALU per vert.
//
// Draw call: DrawIndexedInstanced(bladeSegments * 6, bladeCount, 0, 0, 0)
// against a shared sequential IB (0,1,2,…) — backend has no plain
// DrawInstanced. PSO uses an empty IA layout; this entry reads no VB.

// Skip the legacy IA-based VS entry points from GBufferCommon — we define
// our own VSMain below that synthesizes geometry from SV_VertexID +
// SV_InstanceID alone.
#define GBUFFER_OMIT_ENTRIES
#include "GBufferCommon.hlsli"

// PG_* fields live in GBufferConstantBuffer (b0) alongside everything else,
// so no separate CB binding is needed.

// Linear heightfield: width × depth floats, row-major (z * width + x).
// Bound only when an ActiveTerrain is present; PG_TerrainWidth > 0 gates use.
StructuredBuffer<float> TerrainHeights : register(t4);

// Sample the terrain heightfield at the given world XZ. Falls back to
// PG_BaseY when no heightfield is bound (PG_TerrainWidth == 0).
float SampleTerrainHeight(float2 worldXZ)
{
    if (PG_TerrainWidth == 0u || PG_TerrainDepth == 0u || PG_TerrainScaleXZ <= 0.0f)
        return PG_BaseY;
    // Terrain is centered around world origin in TerrainMeshBuilder, so
    // map worldXZ ∈ [-halfArea, halfArea] back to sample index.
    const float halfW = float(PG_TerrainWidth - 1u) * 0.5f;
    const float halfD = float(PG_TerrainDepth - 1u) * 0.5f;
    const float fx = worldXZ.x / PG_TerrainScaleXZ + halfW;
    const float fz = worldXZ.y / PG_TerrainScaleXZ + halfD;
    const int ix = clamp(int(fx), 0, int(PG_TerrainWidth) - 1);
    const int iz = clamp(int(fz), 0, int(PG_TerrainDepth) - 1);
    return TerrainHeights[iz * int(PG_TerrainWidth) + ix];
}

// Tiny integer hash → float in [0,1). xorshift-style — fast, decent
// distribution for placement / yaw / height jitter.
float HashFloat(uint x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return float(x & 0x00FFFFFFu) / 16777216.0f;
}

// Decode a flat cellIdx (0..) into (dx, dz) offsets ordered by Chebyshev
// rings around the center: ring 0 = 1 cell (center), ring r = 8r cells.
// This guarantees that even if the per-frame instance count is truncated
// (e.g. density × cells exceeds a perf cap), the cells nearest the player
// are filled FIRST so the player is never standing in a bare patch.
void RingDecodeCell(uint cellIdx, out int dx, out int dz, out uint ring)
{
    if (cellIdx == 0u) { dx = 0; dz = 0; ring = 0u; return; }
    // ring r: cumulative cells through ring r = (2r+1)². So
    // cellIdx ∈ [(2r-1)², (2r+1)²-1] iff ring = r (for r ≥ 1).
    ring = uint(ceil((sqrt(float(cellIdx + 1u)) - 1.0f) * 0.5f));
    const uint innerCount = (2u * ring - 1u) * (2u * ring - 1u);
    const uint inRingIdx = cellIdx - innerCount;
    const uint w = 2u * ring; // perimeter quarter-length
    const int  r = int(ring);
    if (inRingIdx <= w)
    {
        // Top edge (dz = -r), dx sweeps -r → +r
        dx = int(inRingIdx) - r;
        dz = -r;
    }
    else if (inRingIdx <= 2u * w)
    {
        // Right edge (dx = +r), dz sweeps -r+1 → +r-1
        dx = r;
        dz = int(inRingIdx - w) - r;
    }
    else if (inRingIdx <= 3u * w)
    {
        // Bottom edge (dz = +r), dx sweeps +r → -r
        dx = r - int(inRingIdx - 2u * w);
        dz = r;
    }
    else
    {
        // Left edge (dx = -r), dz sweeps +r-1 → -r+1
        dx = -r;
        dz = r - int(inRingIdx - 3u * w);
    }
}

// Adaptive spatial grid around the player. Grid side = 2*halfExtent+1 cells,
// where halfExtent grows with PG_RenderDistance. Each cell has exactly
// PG_BladesPerCell blades, so density is independent of render distance.
// outBand = chebyshev distance from center cell (drives segment LOD).
float3 SynthesizeBladeOrigin(uint instanceId, out float yaw, out float heightJitter, out uint outBand, out uint outHalfExtent)
{
    const float kCellSize = 30.0f;
    const uint  halfExtent = max(1u, uint(PG_RenderDistance / kCellSize));
    outHalfExtent = halfExtent;
    const uint gridSide = halfExtent * 2u + 1u;
    const uint bpc      = max(1u, PG_BladesPerCell);
    const uint cellIdx  = instanceId / bpc;
    const uint intraIdx = instanceId % bpc;

    // Re-seed per (cell, intraIdx) so each blade has a stable hash that
    // doesn't depend on the global instance offset.
    const uint seed = (cellIdx * 2654435761u) ^ (intraIdx * 0x9E3779B1u) ^ PG_Seed;
    const uint sx = seed * 0x9E3779B1u;
    const uint sz = seed * 0xC2B2AE35u + 1u;
    const uint sy = seed * 0x27D4EB2Fu + 2u;
    const uint sh = seed * 0x85EBCA6Bu + 3u;

    const float rx = HashFloat(sx) * 2.0f - 1.0f;
    const float rz = HashFloat(sz) * 2.0f - 1.0f;
    yaw = HashFloat(sy) * 6.2831853f;
    heightJitter = 0.85f + HashFloat(sh) * 0.30f;

    const float2 playerCellF = floor(GrassBendOrigin.xz / kCellSize);
    int cellDX, cellDZ;
    uint ring;
    RingDecodeCell(cellIdx, cellDX, cellDZ, ring);
    // Skip cells that fall outside the grid radius (when cellIdx exceeds
    // the grid's outermost ring). Output degenerate position so the blade
    // never rasterizes.
    if (uint(max(abs(cellDX), abs(cellDZ))) > halfExtent)
    {
        outBand = halfExtent + 1u;
        return float3(1e9f, 1e9f, 1e9f);
    }
    outBand = ring;
    const float2 cellOrigin = (playerCellF + float2(cellDX, cellDZ)) * kCellSize;
    const float2 bladeXZ = cellOrigin + float2(rx * 0.5f + 0.5f, rz * 0.5f + 0.5f) * kCellSize;
    const float bladeY = SampleTerrainHeight(bladeXZ);
    return float3(bladeXZ.x, bladeY, bladeXZ.y);
}

PSInput VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    PSInput o = (PSInput)0;

    float yaw, heightJitter;
    uint band, halfExtent;
    const float3 bladeOrigin = SynthesizeBladeOrigin(iid, yaw, heightJitter, band, halfExtent);

    // Out-of-grid cell (cellIdx exceeded the largest ring inside halfExtent) —
    // emit a degenerate triangle so the rasterizer drops it cheaply.
    if (band > halfExtent)
    {
        o.position = float4(2.0f, 2.0f, 2.0f, 1.0f);
        return o;
    }

    // Per-blade LOD scaled to grid radius. Inner third = full segments,
    // middle third = half, outer third = quarter.
    uint effSegments = PG_BladeSegments;
    const uint b1 = max(1u, halfExtent / 3u);
    const uint b2 = max(1u, (halfExtent * 2u) / 3u);
    if (band >= b2)      effSegments = max(1u, PG_BladeSegments / 4u);
    else if (band >= b1) effSegments = max(1u, PG_BladeSegments / 2u);

    // PG_BladeSegments*6 verts dispatched per instance; emit degenerate
    // (off-screen) positions for verts beyond this blade's LOD count so
    // the rasterizer drops them with no fragment work.
    const uint effVertCount = effSegments * 6u;
    if (vid >= effVertCount)
    {
        o.position = float4(2.0f, 2.0f, 2.0f, 1.0f);
        return o;
    }

    const uint localId = vid;
    const uint segment = localId / 6u;
    const uint subId   = localId % 6u;
    // tri1: (0,0)(0,1)(1,0)   tri2: (0,1)(1,1)(1,0)
    const uint rowPlus = (subId == 2u || subId == 4u || subId == 5u) ? 1u : 0u;
    const uint colIdx  = (subId == 1u || subId == 3u || subId == 4u) ? 1u : 0u;
    const uint row = segment + rowPlus;

    const float thisHeight = PG_BladeHeight * heightJitter;
    const float bladeWidth = PG_BladeHeight * PG_BladeWidthScale;

    const float t = float(row) / float(effSegments);
    // Width: lerp base (1.0) → tip (PG_BladeTipWidthScale ∈ [0, 1]) along
    // the blade height. tip=0 yields a sharp point; tip=1 a flat-top blade.
    // Quadratic ease for a more natural taper than pure linear.
    const float widthScale = lerp(1.0f, PG_BladeTipWidthScale, t * t);
    const float halfW = (bladeWidth * 0.5f) * widthScale * (colIdx == 0u ? -1.0f : 1.0f);

    const float c = cos(yaw);
    const float s = sin(yaw);
    float3 objPos;
    objPos.x = bladeOrigin.x + halfW * c;
    objPos.y = bladeOrigin.y + thisHeight * t;
    objPos.z = bladeOrigin.z + halfW * s;

    // Wind + bend, mirroring the VB path's ApplyVertexDeformations layer.
    const float time           = MeshDeformParams.x;
    const float maxBladeHeight = GrassBendParams.y;
    float3 currObjPos    = objPos;
    float3 prevObjPosVec = objPos;
    float windDamp     = 1.0f;
    float windDampPrev = 1.0f;
    if (GrassBendOrigin.w > 0.0f)
    {
        const float bendRadius = GrassBendParams.x;
        const float falloff    = ComputeGrassBendFalloff01(currObjPos,    GrassBendOrigin.xyz, WorldMatrix, bendRadius);
        windDamp     = saturate(1.0f - 0.9f * falloff);
        windDampPrev = windDamp;
    }
    const float prevDt = 1.0f / 60.0f;
    currObjPos    = ApplyWindSway(currObjPos,    time,         maxBladeHeight, t, windDamp);
    prevObjPosVec = ApplyWindSway(prevObjPosVec, time - prevDt, maxBladeHeight, t, windDampPrev);
    if (GrassBendOrigin.w > 0.0f)
    {
        const float3 worldOrigin  = GrassBendOrigin.xyz;
        const float  bendStrength = GrassBendOrigin.w;
        const float  bendRadius   = GrassBendParams.x;
        currObjPos    = ApplyGrassBend(currObjPos,    worldOrigin, WorldMatrix, bendStrength, bendRadius, t);
        prevObjPosVec = ApplyGrassBend(prevObjPosVec, worldOrigin, WorldMatrix, bendStrength, bendRadius, t);
    }

    const float4 worldPos  = mul(float4(currObjPos,    1.0f), WorldMatrix);
    const float4 prevWorld = mul(float4(prevObjPosVec, 1.0f), WorldMatrix);
    o.position           = mul(worldPos,  ViewProjectionMatrix);
    o.unjitteredPosition = mul(worldPos,  UnjitteredViewProjMat);
    o.prevPosition       = mul(prevWorld, PrevViewProjectionMatrix);

    const float3 normal  = normalize(float3(-s, 0.2f, c));
    const float3 tangent = normalize(float3( c, 0.0f, s));
    o.normal    = mul(float4(normal,  0.0f), WorldMatrix).xyz;
    o.tangent   = mul(float4(tangent, 0.0f), WorldMatrix).xyz;
    o.uv = float2(float(colIdx), t);
    return o;
}

struct PS_OUTPUT
{
    float4 Albedo          : SV_Target0;
    float4 SpecularAlbedo  : SV_Target1;
    float4 Normal          : SV_Target2;
    float4 GeomNormal      : SV_Target3;
    float2 Velocity        : SV_Target4;
    float4 Material        : SV_Target5;
    float  UnjitteredDepth : SV_Target6;
};

PS_OUTPUT PSMain(PSInput input)
{
    PS_OUTPUT o = (PS_OUTPUT)0;
    o.Albedo         = float4(0.18f, 0.48f, 0.22f, 1.0f) * BaseColorFactor;
    o.SpecularAlbedo = float4(0.04f, 0.04f, 0.04f, 0.0f);
    const float3 N   = normalize(input.normal);
    o.Normal         = float4(N * 0.5f + 0.5f, 1.0f);
    o.GeomNormal     = o.Normal;

    const float2 currSS = (input.unjitteredPosition.xy / input.unjitteredPosition.w) * float2(0.5f, -0.5f) + 0.5f;
    const float2 prevSS = (input.prevPosition.xy       / input.prevPosition.w)       * float2(0.5f, -0.5f) + 0.5f;
    o.Velocity       = (currSS - prevSS) * RTSize.xy;
    o.Material       = float4(0.85f, 0.0f, 0.0f, 0.0f);
    o.UnjitteredDepth = input.unjitteredPosition.z / input.unjitteredPosition.w;
    return o;
}
