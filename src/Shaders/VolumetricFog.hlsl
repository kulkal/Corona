// Froxel volumetric fog.
//
// Three compute passes over a camera-frustum-aligned 3D grid ("froxels"):
//   1. VolumetricFogInjectCS    — per-froxel participating-media lighting
//      (sun w/ HG phase + optional inline-RT shadow ray, point lights via
//      the world-space point light grid, constant ambient), temporally
//      reprojected against the previous frame's scatter volume.
//      Output: rgb = in-scattered radiance * sigma_s  [per meter]
//              a   = extinction sigma_t               [per meter]
//   2. VolumetricFogIntegrateCS — front-to-back integration along Z.
//      Output per slice: rgb = accumulated in-scatter, a = transmittance.
//   3. VolumetricFogCompositeCS — full-res composite into the scene color
//      buffer with a single trilinear fetch at the opaque depth.
//
// The sun shadow variant is compiled through VolumetricFogRayQuery.hlsl
// (defines VOLUMETRIC_FOG_RAYQUERY, built with cs_6_5 via InitCSWithInlineRT).

#include "Common.hlsl"

#define POINT_LIGHT_GRID_CB_REGISTER b1
#define POINT_LIGHT_GRID_COUNTS_REGISTER t4
#define POINT_LIGHT_GRID_INDICES_REGISTER t5
#include "PointLightGrid.hlsli"

struct PointLightParam
{
    float4 PositionAndRadius;
    float4 ColorAndIntensity;
    float4 DirectionAndType;   // xyz = spotlight direction, w = 0 point / 1 spot
    float4 SpotConeAndFlags;   // x innerCos, y outerCos, z invCosDelta, w castShadow
};

RWTexture3D<float4> ScatterVolume : register(u0);
RWTexture3D<float4> IntegratedVolume : register(u1);
RWTexture2D<float4> SceneColorTex : register(u2);

#ifdef VOLUMETRIC_FOG_RAYQUERY
RaytracingAccelerationStructure gRtScene : register(t0);
RaytracingAccelerationStructure gRtDynamicScene : register(t1);
#endif
Texture3D<float4> HistoryVolume : register(t2);
StructuredBuffer<PointLightParam> PointLightBuffer : register(t3);
Texture3D<float4> ScatterVolumeRead : register(t6);
Texture2D<float> DepthTex : register(t7);
Texture3D<float4> IntegratedVolumeRead : register(t8);

SamplerState VolumeSampler : register(s0);

cbuffer FogCB : register(b0)
{
    float4x4 InvViewMatrix;
    float4x4 InvProjMatrix;
    float4x4 PrevViewProjMatrix;
    float4 PrevCameraPosition;      // xyz = previous frame camera position
    float4 FogColorAndDensity;      // rgb = scattering albedo, a = extinction density
    float4 HeightParams;            // x = height, y = falloff, z = start distance, w = max distance
    float4 LightingParams;          // x = anisotropy, y = ambient strength, z = directional strength, w = max opacity
    float4 GridParams;              // x = grid X, y = grid Y, z = grid Z, w = grid pixel size
    float4 LightDirAndIntensity;    // xyz = direction to sun, w = intensity
    float4 SunColorAndShadow;       // rgb = sun color, w > 0.5 = trace sun shadow rays
    float4 TemporalParams;          // x = frame index, y = history blend (0 = off), z = history valid
    float4 PointLightScatterParams; // x = enabled, y = strength, z = max lights per froxel, w = has dynamic RT scene
    float2 RTSize;
    float2 Padding;
};

static const uint VOLUMETRIC_FOG_SHADOW_RAY_MASK = 0x02u;

float3 CameraWorldPosition()
{
    return mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
}

float3 ReconstructViewRay(float2 uv)
{
    float2 screenPosition = uv * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;

    float4 viewFarH = mul(float4(screenPosition, 1.0f, 1.0f), InvProjMatrix);
    float3 viewFar = viewFarH.xyz / max(abs(viewFarH.w), 1.0e-6f);
    return normalize(viewFar);
}

float3 ReconstructWorldRay(float2 uv)
{
    float3 viewRay = ReconstructViewRay(uv);
    return normalize(mul(float4(viewRay, 0.0f), InvViewMatrix).xyz);
}

float ReconstructViewDistance(uint2 pixel, float deviceDepth)
{
    if (deviceDepth >= 0.999999f)
        return HeightParams.w;

    float2 uv = (float2(pixel) + 0.5f) / max(RTSize, float2(1.0f, 1.0f));
    float2 screenPosition = uv * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;

    float3 viewPosition = GetViewPosition(deviceDepth, screenPosition, InvProjMatrix);
    return length(viewPosition);
}

// The froxel grid covers gridXY * pixelSize screen pixels, which can slightly
// exceed the render target; this maps a screen uv to the matching volume uv.
float2 ScreenUvToVolumeUv(float2 screenUv)
{
    float2 gridCoverage = max(GridParams.xy * GridParams.w, float2(1.0f, 1.0f));
    return screenUv * (RTSize / gridCoverage);
}

// --- Z slice distribution: quadratic in t between start and max distance ---

float SliceBoundaryT(uint z)
{
    float gridZ = max(GridParams.z, 1.0f);
    return float(z) / gridZ;
}

float SliceDistanceFromT(float t)
{
    float startDistance = max(HeightParams.z, 0.0f);
    float maxDistance = max(HeightParams.w, startDistance + 1.0f);
    float curvedT = t * t;
    return lerp(startDistance, maxDistance, curvedT);
}

float SliceCenterDistance(uint z)
{
    float gridZ = max(GridParams.z, 1.0f);
    return SliceDistanceFromT((float(z) + 0.5f) / gridZ);
}

float SliceStepLength(uint z)
{
    float d0 = SliceDistanceFromT(SliceBoundaryT(z));
    float d1 = SliceDistanceFromT(SliceBoundaryT(z + 1));
    return max(d1 - d0, 0.0f);
}

// Inverse of SliceDistanceFromT: volume W texture coordinate for a distance.
float DistanceToVolumeW(float viewDistance)
{
    float startDistance = max(HeightParams.z, 0.0f);
    float maxDistance = max(HeightParams.w, startDistance + 1.0f);
    return sqrt(saturate((viewDistance - startDistance) / (maxDistance - startDistance)));
}

float PhaseHenyeyGreenstein(float cosTheta, float g)
{
    g = clamp(g, -0.9f, 0.9f);
    float g2 = g * g;
    float denom = max(1.0f + g2 - 2.0f * g * cosTheta, 1.0e-4f);
    return (1.0f - g2) / (4.0f * 3.14159265f * denom * sqrt(denom));
}

float HeightDensity(float worldY)
{
    float height = HeightParams.x;
    float falloff = max(HeightParams.y, 0.0f);
    return exp(-max(worldY - height, 0.0f) * falloff);
}

// Interleaved gradient noise, animated by frame index (temporal jitter that
// the reprojection blend integrates away).
float InterleavedGradientNoise(float2 pixel, float frameIndex)
{
    pixel += frameIndex * 5.588238f;
    return frac(52.9829189f * frac(0.06711056f * pixel.x + 0.00583715f * pixel.y));
}

float EvaluatePointLightDistanceAttenuation(float distanceSq, float radius)
{
    radius = max(radius, 0.01f);
    const float invRadiusSq = rcp(radius * radius);
    const float normalizedDistSq = saturate(distanceSq * invRadiusSq);
    float rangeAttenuation = saturate(1.0f - normalizedDistSq * normalizedDistSq);
    rangeAttenuation *= rangeAttenuation;
    const float inverseSquareAttenuation = rcp(max(1.0f, distanceSq * 0.0001f));
    return rangeAttenuation * inverseSquareAttenuation;
}

float EvaluateSpotAttenuation(PointLightParam light, float3 surfaceToLightDir)
{
    if (light.DirectionAndType.w < 0.5f)
        return 1.0f;

    float3 spotDir = normalize(light.DirectionAndType.xyz + float3(0.0f, 1.0e-6f, 0.0f));
    float cosTheta = dot(spotDir, -surfaceToLightDir);
    float cone = saturate((cosTheta - light.SpotConeAndFlags.y) * light.SpotConeAndFlags.z);
    return cone * cone;
}

#ifdef VOLUMETRIC_FOG_RAYQUERY
float TraceSunVisibility(float3 worldPosition, float3 dirToSun, float viewDistance)
{
    RayDesc ray;
    ray.Origin = worldPosition;
    ray.Direction = dirToSun;
    // Froxel centers sit in free space, so only a small push-off is needed;
    // clamp the distance scaling hard or far froxels skip real occluders and
    // leak sunlight around distant walls.
    ray.TMin = clamp(viewDistance * 1.0e-4f, 0.05f, 5.0f);
    ray.TMax = 1.0e7f;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
             RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    q.TraceRayInline(gRtScene, RAY_FLAG_NONE, VOLUMETRIC_FOG_SHADOW_RAY_MASK, ray);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_NOTHING)
        return 0.0f;

    if (PointLightScatterParams.w > 0.5f)
    {
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
                 RAY_FLAG_CULL_BACK_FACING_TRIANGLES> qDynamic;
        qDynamic.TraceRayInline(gRtDynamicScene, RAY_FLAG_NONE, VOLUMETRIC_FOG_SHADOW_RAY_MASK, ray);
        qDynamic.Proceed();
        if (qDynamic.CommittedStatus() != COMMITTED_NOTHING)
            return 0.0f;
    }
    return 1.0f;
}
#endif

float3 EvaluatePointLightScattering(float3 worldPosition, float3 worldRay, float anisotropy)
{
    if (PointLightScatterParams.x < 0.5f || !PointLightGridIsEnabled())
        return 0.0f.xxx;

    uint cellIndex = 0u;
    uint candidateCount = PointLightGridSelectCandidateCount(worldPosition, cellIndex);
    uint maxLights = (uint)max(PointLightScatterParams.z, 0.0f);
    candidateCount = min(candidateCount, maxLights);

    float3 scattering = 0.0f.xxx;
    [loop]
    for (uint candidate = 0u; candidate < candidateCount; ++candidate)
    {
        uint lightIndex = PointLightGridLoadLightIndex(cellIndex, candidate);
        if (lightIndex >= PointLightGridGetPointLightCount())
            continue;

        PointLightParam light = PointLightBuffer[lightIndex];
        float3 toLight = light.PositionAndRadius.xyz - worldPosition;
        float distanceSq = max(dot(toLight, toLight), 1.0e-4f);
        float attenuation = EvaluatePointLightDistanceAttenuation(distanceSq, light.PositionAndRadius.w);
        if (attenuation <= 0.0f)
            continue;

        float3 dirToLight = toLight * rsqrt(distanceSq);
        attenuation *= EvaluateSpotAttenuation(light, dirToLight);
        if (attenuation <= 0.0f)
            continue;

        float phase = PhaseHenyeyGreenstein(dot(dirToLight, worldRay), anisotropy);
        scattering += light.ColorAndIntensity.rgb * light.ColorAndIntensity.a * attenuation * phase;
    }

    return scattering * max(PointLightScatterParams.y, 0.0f);
}

[numthreads(4, 4, 4)]
void VolumetricFogInjectCS(uint3 froxel : SV_DispatchThreadID)
{
    uint gridX = (uint)GridParams.x;
    uint gridY = (uint)GridParams.y;
    uint gridZ = (uint)GridParams.z;
    if (froxel.x >= gridX || froxel.y >= gridY || froxel.z >= gridZ)
        return;

    float frameIndex = TemporalParams.x;
    float3 jitter = float3(
        InterleavedGradientNoise(float2(froxel.xy), frameIndex),
        InterleavedGradientNoise(float2(froxel.xy) + float2(37.0f, 17.0f), frameIndex),
        InterleavedGradientNoise(float2(froxel.xy) + float2(59.0f, 83.0f), frameIndex + float(froxel.z) * 0.618034f)) - 0.5f;
    bool bTemporal = TemporalParams.y > 0.0f;
    if (!bTemporal)
        jitter = 0.0f.xxx;

    // Grid cells map to screen pixels of size GridParams.w; uv can exceed 1
    // for the padding cells on the right/bottom edge, which is fine for ray
    // reconstruction.
    float2 uv = (float2(froxel.xy) + 0.5f + jitter.xy) * GridParams.w / max(RTSize, float2(1.0f, 1.0f));
    float3 worldRay = ReconstructWorldRay(uv);
    float3 cameraPosition = CameraWorldPosition();

    float gridZF = max(GridParams.z, 1.0f);
    float viewDistance = SliceDistanceFromT((float(froxel.z) + 0.5f + jitter.z) / gridZF);
    float3 worldPosition = cameraPosition + worldRay * viewDistance;

    float density = max(FogColorAndDensity.a, 0.0f) * HeightDensity(worldPosition.y);

    // In-scattered radiance (before albedo).
    float3 lightDir = normalize(LightDirAndIntensity.xyz + float3(0.0f, 1.0e-6f, 0.0f));
    float phase = PhaseHenyeyGreenstein(dot(lightDir, worldRay), LightingParams.x);
    float sunVisibility = 1.0f;
#ifdef VOLUMETRIC_FOG_RAYQUERY
    if (SunColorAndShadow.w > 0.5f && density > 0.0f)
        sunVisibility = TraceSunVisibility(worldPosition, lightDir, viewDistance);
#endif
    float3 radiance =
        LightingParams.y.xxx +
        SunColorAndShadow.rgb * (max(LightingParams.z, 0.0f) * max(LightDirAndIntensity.w, 0.0f) * phase * sunVisibility);
    if (density > 0.0f)
        radiance += EvaluatePointLightScattering(worldPosition, worldRay, LightingParams.x);

    float3 scatter = max(FogColorAndDensity.rgb, 0.0f.xxx) * radiance * density;
    float4 result = float4(scatter, density);

    if (bTemporal && TemporalParams.z > 0.5f)
    {
        // Reproject the unjittered froxel center into the previous frame's
        // volume and blend against the history scatter volume.
        float2 centerUv = (float2(froxel.xy) + 0.5f) * GridParams.w / max(RTSize, float2(1.0f, 1.0f));
        float3 centerRay = ReconstructWorldRay(centerUv);
        float3 centerWorld = cameraPosition + centerRay * SliceCenterDistance(froxel.z);

        float4 prevClip = mul(float4(centerWorld, 1.0f), PrevViewProjMatrix);
        if (prevClip.w > 1.0e-4f)
        {
            float2 prevNdc = prevClip.xy / prevClip.w;
            float2 prevScreenUv = float2(prevNdc.x * 0.5f + 0.5f, -prevNdc.y * 0.5f + 0.5f);
            float prevDistance = length(centerWorld - PrevCameraPosition.xyz);
            float3 prevVolumeUv = float3(ScreenUvToVolumeUv(prevScreenUv), DistanceToVolumeW(prevDistance));
            if (all(prevVolumeUv >= 0.0f.xxx) && all(prevVolumeUv <= 1.0f.xxx))
            {
                float4 history = max(HistoryVolume.SampleLevel(VolumeSampler, prevVolumeUv, 0.0f), 0.0f.xxxx);
                result = lerp(result, history, saturate(TemporalParams.y));
            }
        }
    }

    ScatterVolume[froxel] = result;
}

[numthreads(8, 8, 1)]
void VolumetricFogIntegrateCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint gridX = (uint)GridParams.x;
    uint gridY = (uint)GridParams.y;
    uint gridZ = (uint)GridParams.z;
    uint2 cell = dispatchThreadId.xy;
    if (cell.x >= gridX || cell.y >= gridY)
        return;

    float3 accumScatter = 0.0f.xxx;
    float transmittance = 1.0f;

    [loop]
    for (uint z = 0; z < gridZ; ++z)
    {
        float4 s = ScatterVolumeRead.Load(int4(cell.x, cell.y, z, 0));
        float sigma = max(s.a, 0.0f);
        float stepLength = SliceStepLength(z);
        float stepTransmittance = exp(-sigma * stepLength);
        // Analytic integration of constant in-scatter across the slice
        // (energy-conserving for large extinction * step products).
        float3 sliceScatter = (sigma > 1.0e-6f)
            ? s.rgb * ((1.0f - stepTransmittance) / sigma)
            : s.rgb * stepLength;
        accumScatter += transmittance * sliceScatter;
        transmittance *= stepTransmittance;

        IntegratedVolume[uint3(cell, z)] = float4(accumScatter, transmittance);
    }
}

[numthreads(8, 8, 1)]
void VolumetricFogCompositeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)RTSize.x || pixel.y >= (uint)RTSize.y)
        return;

    float viewDistance = ReconstructViewDistance(pixel, DepthTex.Load(int3(pixel, 0)).x);
    if (viewDistance <= max(HeightParams.z, 0.0f))
        return;

    float2 uv = (float2(pixel) + 0.5f) / max(RTSize, float2(1.0f, 1.0f));
    // Integrated texel z holds the integral up to the *end* of slice z, i.e.
    // boundary t = (z+1)/gridZ, while its texel center sits at (z+0.5)/gridZ.
    // Shift by half a slice so the fetch matches the opaque depth instead of
    // integrating half a slice past it (visible as light bleed at range).
    float gridZF = max(GridParams.z, 1.0f);
    float volumeW = max(DistanceToVolumeW(viewDistance) - 0.5f / gridZF, 0.0f);
    float3 volumeUv = float3(ScreenUvToVolumeUv(uv), volumeW);
    float4 fog = IntegratedVolumeRead.SampleLevel(VolumeSampler, volumeUv, 0.0f);
    float3 inScatter = max(fog.rgb, 0.0f.xxx);
    float transmittance = saturate(fog.a);

    // Clamp total fog opacity while keeping the in-scatter/transmittance ratio.
    float maxOpacity = saturate(LightingParams.w);
    float effectiveTransmittance = max(transmittance, 1.0f - maxOpacity);
    float scatterScale = (1.0f - effectiveTransmittance) / max(1.0f - transmittance, 1.0e-4f);

    float4 src = SceneColorTex[pixel];
    src.rgb = src.rgb * effectiveTransmittance + inScatter * scatterScale;
    SceneColorTex[pixel] = src;
}
