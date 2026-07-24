#include "Common.hlsl"
#include "BindlessResources.hlsli"

RWTexture2D<float4> GIResultSH : register(u0);
RWTexture2D<float4> GIResultColor : register(u1);


RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
Texture3D RayNoiseBlueNoiseSource : register(t7);
Texture2D TranslucentGuideUVTex : register(t8);

// Must match Corona::MaxPointLights in Corona.h.
#define MAX_POINT_LIGHTS 128
#define RT_DIFFUSE_GI_MAX_POINT_LIGHTS 16

struct PointLightParam
{
    float4 PositionAndRadius;
    float4 ColorAndIntensity;
    float4 DirectionAndType;
    float4 SpotConeAndFlags;
};

StructuredBuffer<PointLightParam> PointLightBuffer : register(t4);
#include "PointLightGrid.hlsli"

cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4x4 InvProjMatrix;
    float4 ProjectionParams;
    float4 LightDirAndIntensity;
    float2 RandomOffset;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    float ViewSpreadAngle;
    uint NoiseMode;
    uint GISamplesPerPixel;
    uint TranslucentGuideFlags;
    float3 LightColor;
    float _padding;
    PointLightParam PointLights[RT_DIFFUSE_GI_MAX_POINT_LIGHTS];
    uint PointLightCount;
    float3 PointLightPadding;
    uint CheckerboardMode;
    uint CheckerboardPhase;
    uint CheckerboardLobeParity;
    float CheckerboardOutputScale;
};

SamplerState sampleWrap : register(s0);

static const uint TRANSLUCENT_GUIDE_FLAG_USE = 1u;
static const uint TRANSLUCENT_GUIDE_FLAG_OFFSET = 2u;

float2 ResolveGIReceiverProjectionUv(uint2 pixelPos, float2 dstUv)
{
    if ((TranslucentGuideFlags & TRANSLUCENT_GUIDE_FLAG_USE) == 0u)
        return dstUv;

    float4 guide = TranslucentGuideUVTex.Load(int3(pixelPos, 0));
    if (guide.w <= 0.0001f)
        return dstUv;

    const bool guideStoresOffset = (TranslucentGuideFlags & TRANSLUCENT_GUIDE_FLAG_OFFSET) != 0u;
    return saturate(guideStoresOffset ? (dstUv + guide.xy) : guide.xy);
}

static const float INV_PI = 1.0 / PI;
static const float MAX_HIT_DIST = 10000;

#define RT_GI_SURFACE_RAY_FLAGS (RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES)

uint RTGIHashCheckerboardTile(uint2 tile)
{
    uint h = tile.x * 0x8da6b343u;
    h ^= tile.y * 0xd8163841u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h;
}

uint2 RTGIGetCheckerboardTargetOffset(uint2 pixel, uint2 renderSize)
{
    (void)renderSize;
    uint2 basePixel = pixel - (pixel & uint2(1u, 1u));
    uint2 tile = basePixel / 2u;
    uint temporalSeed = CheckerboardPhase / 4u;
    uint tileHash = RTGIHashCheckerboardTile(tile ^ uint2(temporalSeed * 0x9e3779b9u, temporalSeed * 0x7f4a7c15u));
    uint lobePhaseBase = (CheckerboardPhase & 3u) + (tileHash & 3u);
    uint lobePhase = (lobePhaseBase + (CheckerboardLobeParity & 1u) * 2u) & 3u;
    uint2 offset = uint2(lobePhase & 1u, (lobePhase >> 1u) & 1u);
    return offset;
}

uint2 RTGIGetCheckerboardSourcePixel(uint2 pixel, uint2 renderSize)
{
    uint2 basePixel = pixel - (pixel & uint2(1u, 1u));
    return basePixel + RTGIGetCheckerboardTargetOffset(pixel, renderSize);
}

bool RTGIIsCheckerboardRepresentative(uint2 pixel, uint2 renderSize)
{
    uint2 basePixel = pixel - (pixel & uint2(1u, 1u));
    return all((pixel - basePixel) == RTGIGetCheckerboardTargetOffset(pixel, renderSize));
}

bool RTGIShouldTracePixel(uint2 pixel, uint2 renderSize)
{
    if (CheckerboardMode != 3u)
        return true;
    return RTGIIsCheckerboardRepresentative(pixel, renderSize);
}

void RTGIStoreCheckerboard(uint2 pixel, uint2 renderSize, float4 shValue, float4 colorValue)
{
    (void)renderSize;
    float scale = CheckerboardMode == 3u ? CheckerboardOutputScale : 1.0f;
    GIResultSH[pixel] = shValue * scale;
    GIResultColor[pixel] = float4(colorValue.rgb * scale, colorValue.a);
}

void RTGIClearCheckerboardPixel(uint2 pixel)
{
    GIResultSH[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    GIResultColor[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}

float3 linearToSrgb(float3 c)
{
    // Based on http://chilliant.blogspot.com/2012/08/srgb-approximations-for-hlsl.html
    float3 sq1 = sqrt(c);
    float3 sq2 = sqrt(sq1);
    float3 sq3 = sqrt(sq2);
    float3 srgb = 0.662002687 * sq1 + 0.684122060 * sq2 - 0.323583601 * sq3 - 0.0225411470 * c;
    return srgb;
}

struct RT_DIFFUSE_GI_RAY_PAYLOAD RayPayload
{
    float3 position RT_DIFFUSE_GI_PAYLOAD_RW;
    float3 color RT_DIFFUSE_GI_PAYLOAD_RW;
    float3 normal RT_DIFFUSE_GI_PAYLOAD_RW;
    bool bHit RT_DIFFUSE_GI_PAYLOAD_RW;
    // spreadAngle/coneWidth removed from payload: spreadAngle is uniform
    // (ViewSpreadAngle) and coneWidth starts at 0, so chs derives the ray-cone
    // width directly from ViewSpreadAngle. Keeps 2 dwords out of per-ray payload.
};


struct RT_DIFFUSE_GI_RAY_PAYLOAD ShadowRayPayload
{
    bool bHit RT_DIFFUSE_GI_SHADOW_PAYLOAD_RW;
};

static const uint RT_SHADOW_RAY_MASK = 0x02u;

bool TraceDiffuseGIShadowOccluded(RayDesc shadowRay)
{
#if RT_DIFFUSE_GI_USE_RAYQUERY_SHADOWS
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
             RAY_FLAG_FORCE_OPAQUE |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gRtScene, RAY_FLAG_NONE, RT_SHADOW_RAY_MASK, shadowRay);
    q.Proceed();
    return q.CommittedStatus() != COMMITTED_NOTHING;
#else
    ShadowRayPayload shadowPayload;
    shadowPayload.bHit = true;
    TraceRay(
        gRtScene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
            RAY_FLAG_FORCE_OPAQUE |
            RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        RT_SHADOW_RAY_MASK,
        0,
        0,
        1,
        shadowRay,
        shadowPayload);
    return shadowPayload.bHit;
#endif
}

float EvaluateSpotAttenuation(PointLightParam light, float3 surfaceToLightDir)
{
    if (light.DirectionAndType.w < 0.5f)
        return 1.0f;

    float3 spotDir = CommonSafeNormalize(light.DirectionAndType.xyz, float3(0.0f, 1.0f, 0.0f));
    float cosTheta = dot(spotDir, -surfaceToLightDir);
    float cone = saturate((cosTheta - light.SpotConeAndFlags.y) * light.SpotConeAndFlags.z);
    return cone * cone;
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

float ComputePointLightEndpointBias(float lightRadius)
{
    return clamp(max(lightRadius, 0.01f) * 0.02f, 0.25f, 80.0f);
}

float3 offset_ray(float3 p, float3 n);

bool LoadPointLightForGI(uint lightIndex, bool usePointLightGrid, out PointLightParam light)
{
    light.PositionAndRadius = 0.0f.xxxx;
    light.ColorAndIntensity = 0.0f.xxxx;
    light.DirectionAndType = 0.0f.xxxx;
    light.SpotConeAndFlags = 0.0f.xxxx;

    if (usePointLightGrid)
    {
        if (lightIndex >= PointLightGridGetPointLightCount() || lightIndex >= (uint)MAX_POINT_LIGHTS)
            return false;
        light = PointLightBuffer[lightIndex];
    }
    else
    {
        if (lightIndex >= PointLightCount || lightIndex >= (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS)
            return false;
        light = PointLights[lightIndex];
    }

    return max(light.ColorAndIntensity.w, 0.0f) > 0.0f &&
        max(light.PositionAndRadius.w, 0.0f) > 0.0f;
}

bool IsPointLightVisible(float3 worldPos, float3 normal, float3 lightDir, float lightDistance, PointLightParam light)
{
    if (light.SpotConeAndFlags.w <= 0.5f)
        return true;

    float3 biasNormal = dot(normal, lightDir) < 0.0f ? -normal : normal;
    RayDesc shadowRay;
    shadowRay.Origin = offset_ray(worldPos + biasNormal * 0.5f, biasNormal);
    shadowRay.Direction = lightDir;
    shadowRay.TMin = 0.5f;
    const float endpointBias = ComputePointLightEndpointBias(light.PositionAndRadius.w);
    if (lightDistance <= endpointBias + shadowRay.TMin)
        return true;
    shadowRay.TMax = max(lightDistance - endpointBias, shadowRay.TMin + 0.05f);

    return !TraceDiffuseGIShadowOccluded(shadowRay);
}

uint HashPointLightSample(uint2 pixel, uint frameIndex, uint sampleIndex)
{
    uint h = pixel.x * 1973u;
    h ^= pixel.y * 9277u;
    h ^= frameIndex * 26699u;
    h ^= sampleIndex * 911u;
    h ^= h >> 16;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return h;
}

float EvaluatePointLightSelectionScore(float3 worldPos, float3 normal, PointLightParam light)
{
    float3 toLight = light.PositionAndRadius.xyz - worldPos;
    float distanceSq = max(dot(toLight, toLight), 1.0e-4f);
    float lightDistance = sqrt(distanceSq);
    float3 lightDir = toLight / lightDistance;
    float range = max(light.PositionAndRadius.w, 0.01f);
    float attenuation = EvaluatePointLightDistanceAttenuation(distanceSq, range) * EvaluateSpotAttenuation(light, lightDir);
    float nDotL = saturate(dot(normal, lightDir));
    float luma = dot(max(light.ColorAndIntensity.xyz, 0.0f.xxx), float3(0.2126f, 0.7152f, 0.0722f));
    return max(0.0f, luma * max(light.ColorAndIntensity.w, 0.0f) * attenuation * nDotL);
}

float3 EvaluateSinglePointLightBounce(float3 worldPos, float3 normal, float3 albedo, uint lightIndex, bool usePointLightGrid)
{
    PointLightParam light;
    if (!LoadPointLightForGI(lightIndex, usePointLightGrid, light))
        return 0.0f.xxx;

    float3 toLight = light.PositionAndRadius.xyz - worldPos;
    float distanceSq = max(dot(toLight, toLight), 1.0e-4f);
    float lightDistance = sqrt(distanceSq);
    float3 lightDir = toLight / lightDistance;
    float range = max(light.PositionAndRadius.w, 0.01f);
    float attenuation = EvaluatePointLightDistanceAttenuation(distanceSq, range) * EvaluateSpotAttenuation(light, lightDir);
    float nDotL = saturate(dot(normal, lightDir));
    if (attenuation <= 0.0f || nDotL <= 0.0f || !IsPointLightVisible(worldPos, normal, lightDir, lightDistance, light))
        return 0.0f.xxx;

    float3 lightColor = max(CommonSanitizeFloat3(light.ColorAndIntensity.xyz, 0.0f.xxx), 0.0f.xxx);
    float lightIntensity = max(CommonSanitizeFloat(light.ColorAndIntensity.w, 0.0f), 0.0f);
    return nDotL * lightColor * lightIntensity * attenuation * max(albedo, 0.0f.xxx) * INV_PI;
}

float3 EvaluatePointLightBounce(float3 worldPos, float3 normal, float3 albedo, uint2 pixel)
{
    float3 radiance = 0.0f.xxx;
    const bool usePointLightGrid = PointLightGridIsEnabled();
    const uint activeCount = usePointLightGrid ?
        min(PointLightGridGetPointLightCount(), (uint)MAX_POINT_LIGHTS) :
        min(PointLightCount, (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS);
    if (activeCount == 0u)
        return radiance;

    uint gridCellIndex = 0u;
    const uint candidateCount = usePointLightGrid ?
        min(PointLightGridSelectCandidateCount(worldPos, gridCellIndex), PointLightGridGetMaxCount()) :
        activeCount;
    if (candidateCount == 0u)
        return radiance;

    uint sampleCount = clamp(GISamplesPerPixel, 1u, (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS);
    if (sampleCount >= candidateCount)
    {
        [loop]
        for (uint candidateIndex = 0u; candidateIndex < MAX_POINT_LIGHTS; ++candidateIndex)
        {
            if (candidateIndex >= candidateCount)
                break;
            const uint lightIndex = usePointLightGrid ?
                PointLightGridLoadLightIndex(gridCellIndex, candidateIndex) :
                candidateIndex;
            if (lightIndex >= activeCount)
                continue;
            radiance += EvaluateSinglePointLightBounce(worldPos, normal, albedo, lightIndex, usePointLightGrid);
        }
        return radiance;
    }

    float topScore[RT_DIFFUSE_GI_MAX_POINT_LIGHTS];
    uint topIndex[RT_DIFFUSE_GI_MAX_POINT_LIGHTS];
    [unroll]
    for (uint slot = 0u; slot < (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS; ++slot)
    {
        topScore[slot] = 0.0f;
        topIndex[slot] = 0xFFFFFFFFu;
    }

    [loop]
    for (uint candidateIndex = 0u; candidateIndex < MAX_POINT_LIGHTS; ++candidateIndex)
    {
        if (candidateIndex >= candidateCount)
            break;
        const uint lightIndex = usePointLightGrid ?
            PointLightGridLoadLightIndex(gridCellIndex, candidateIndex) :
            candidateIndex;
        if (lightIndex >= activeCount)
            continue;

        PointLightParam light;
        if (!LoadPointLightForGI(lightIndex, usePointLightGrid, light))
            continue;

        const float score = EvaluatePointLightSelectionScore(worldPos, normal, light);
        if (score <= 0.0f)
            continue;

        [unroll]
        for (uint slot = 0u; slot < (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS; ++slot)
        {
            if (slot >= sampleCount)
                break;
            if (score <= topScore[slot])
                continue;

            [unroll]
            for (uint shift = (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS - 1u; shift > 0u; --shift)
            {
                if (shift >= sampleCount || shift <= slot)
                    continue;
                topScore[shift] = topScore[shift - 1u];
                topIndex[shift] = topIndex[shift - 1u];
            }

            topScore[slot] = score;
            topIndex[slot] = lightIndex;
            break;
        }
    }

    [unroll]
    for (uint slot = 0u; slot < (uint)RT_DIFFUSE_GI_MAX_POINT_LIGHTS; ++slot)
    {
        if (slot >= sampleCount)
            break;
        if (topIndex[slot] == 0xFFFFFFFFu)
            continue;
        radiance += EvaluateSinglePointLightBounce(worldPos, normal, albedo, topIndex[slot], usePointLightGrid);
    }
    return radiance;
}

void TraceDiffuseGIRay(RayDesc ray, inout RayPayload payload)
{
#if RT_DIFFUSE_GI_USE_SER
    dx::HitObject hit = dx::HitObject::TraceRay(
        gRtScene,
        RT_GI_SURFACE_RAY_FLAGS,
        0xFF,
        0,
        0,
        0,
        ray,
        payload);
    dx::MaybeReorderThread(hit, hit.GetInstanceID(), RT_DIFFUSE_GI_SER_MATERIAL_HINT_BITS);
    dx::HitObject::Invoke(hit, payload);
#else
    TraceRay(
        gRtScene,
        RT_GI_SURFACE_RAY_FLAGS,
        0xFF,
        0,
        0,
        0,
        ray,
        payload);
#endif
}

float3 offset_ray(float3 p, float3 n)
{
    const float origin = 1.0f / 32.0f;
    const float floatScale = 1.0f / 65536.0f;
    const float intScale = 256.0f;

    int3 ofi = int3(n * intScale);
    int3 piInt = asint(p);
    piInt.x += p.x < 0.0f ? -ofi.x : ofi.x;
    piInt.y += p.y < 0.0f ? -ofi.y : ofi.y;
    piInt.z += p.z < 0.0f ? -ofi.z : ofi.z;

    float3 pi = asfloat(piInt);
    return float3(
        abs(p.x) < origin ? p.x + floatScale * n.x : pi.x,
        abs(p.y) < origin ? p.y + floatScale * n.y : pi.y,
        abs(p.z) < origin ? p.z + floatScale * n.z : pi.z);
}

float random(float2 co){
    return frac(sin(dot(co.xy ,float2(12.9898,78.233))) * 43758.5453);
}

float madFrac(float a, float b) {
    return (a*b) - floor(a*b);
}




float3x3 buildTBN(float3 normal) {

    // TODO: Maybe try approach from here (Building an Orthonormal Basis, Revisited): 
    // https://graphics.pixar.com/library/OrthonormalB/paper.pdf

    normal = CommonSafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));

    // Pick random vector for generating orthonormal basis
    static const float3 rvec1 = float3(0.847100675f, 0.207911700f, 0.489073813f);
    static const float3 rvec2 = float3(-0.639436305f, -0.390731126f, 0.662155867f);
    float3 rvec;

    if (dot(rvec1, normal) > 0.95f)
        rvec = rvec2;
    else
        rvec = rvec1;

    // Construct TBN matrix to orient sampling hemisphere along the surface normal
    float3 b1 = CommonSafeNormalize(rvec - normal * dot(rvec, normal), float3(1.0f, 0.0f, 0.0f));
    float3 b2 = CommonSafeNormalize(cross(normal, b1), float3(0.0f, 0.0f, 1.0f));
    float3x3 tbn = float3x3(b1, b2, normal);

    return tbn;
}

[shader("raygeneration")]
void rayGen
()
{
    uint3 launchIndex = DispatchRaysIndex();
    uint3 launchDim = DispatchRaysDimensions();

    if (!RTGIShouldTracePixel(launchIndex.xy, launchDim.xy))
    {
        RTGIClearCheckerboardPixel(launchIndex.xy);
        return;
    }


    float2 crd = float2(launchIndex.xy);
	//crd.y *= -1;
    float2 dims = float2(launchDim.xy);

	float2 UV = crd / dims;
    float2 receiverProjectionUv = ResolveGIReceiverProjectionUv(launchIndex.xy, UV);
    float2 d = (receiverProjectionUv * 2.f - 1.f);
    d *= tan(0.8 / 2);
    float aspectRatio = dims.x / dims.y;


	float DeviceDepth = DepthTex.SampleLevel(sampleWrap, UV, 0).x;
    if (DeviceDepth >= 0.999999f)
    {
        RTGIStoreCheckerboard(launchIndex.xy, launchDim.xy, float4(0.0f, 0.0f, 0.0f, 0.0f), float4(0.0f, 0.0f, 0.0f, 0.0f));
        return;
    }

	float3 WorldNormal = CommonSafeNormalize(WorldNormalTex.SampleLevel(sampleWrap, UV, 0).xyz, float3(0.0f, 1.0f, 0.0f));
  

    float LinearDepth = GetLinearDepthOpenGL(DeviceDepth, ProjectionParams.z, ProjectionParams.w) ;
	
    float2 ScreenPosition = receiverProjectionUv * 2.0f - 1.0f;
	ScreenPosition.y = -ScreenPosition.y;

	// float3 ViewPosition = GetViewPosition(LinearDepth, ScreenPosition, ProjMatrix._11, ProjMatrix._22);
    float3 ViewPosition = GetViewPosition(DeviceDepth, ScreenPosition, InvProjMatrix);
    // 
	float3 WorldPos = mul(float4(ViewPosition, 1), InvViewMatrix).xyz;

  
    float2 RandomUV = GenerateRaySample2D(RayNoiseBlueNoiseSource, launchIndex.xy, FrameCounter, BlueNoiseOffsetStride, NoiseMode);

    float LightIntensity = max(CommonSanitizeFloat(LightDirAndIntensity.w, 0.0f), 0.0f);

    float3 viewRay = CommonSafeNormalize(float3(d.x * aspectRatio, -d.y, -1.0f), float3(0.0f, 0.0f, -1.0f));
    float3 ViewDir = CommonSafeNormalize(mul(float4(viewRay, 0.0f), InvViewMatrix).xyz, -WorldNormal);
    if (dot(WorldNormal, -ViewDir) < 0.0f)
        WorldNormal = -WorldNormal;

    float3 sampleDirLocal = SampleHemisphereCosine(RandomUV.x, RandomUV.y);
    float3x3 tbn = buildTBN(WorldNormal);
    float3 sampleDirWorld = CommonSafeNormalize(mul(sampleDirLocal, tbn), WorldNormal);

	RayDesc ray;
	// Self-intersection guard. A fixed 0.5u push-off does not clear the surface on deep /
	// grazing geometry (tall building walls — the depth-reconstructed WorldPos error grows
	// with view distance), so grazing hemisphere rays re-hit the origin surface and return
	// a dark self-hit that toggles with the per-frame sample direction. Under DLSS-RR's
	// raw diffuse feed that read as large flickering regions. Scaling the push-off and TMin
	// with view depth keeps the origin clear of the surface at any distance.
	float surfEps = max(0.5f, abs(LinearDepth) * 0.02f);
	ray.Origin = offset_ray(WorldPos + WorldNormal * surfEps, WorldNormal);
	ray.Direction = sampleDirWorld;//reflect(ViewDir, WorldNormal);

	ray.TMin = max(0.01f, surfEps * 0.5f);
    ray.TMax = MAX_HIT_DIST;

	RayPayload payload;
    payload.position = 0.0f.xxx;
    payload.color = 0.0f.xxx;
    payload.normal = WorldNormal;
    payload.bHit = false;
    TraceDiffuseGIRay(ray, payload);
    if(payload.bHit == false)
    {
        float3 Irradiance = 0.0f.xxx;

        // Sanitize before it reaches DLSS-RR: NaN/Inf or extreme fireflies in the raw GI
        // feed pollute the RR input color and make RR collapse the whole frame to black.
        {
            Irradiance = max(CommonSanitizeFloat3(Irradiance, 0.0f.xxx), 0.0f.xxx);
            float giLuma = dot(Irradiance, float3(0.2126f, 0.7152f, 0.0722f));
            const float kGIMaxLuma = 16.0f;
            if (giLuma > kGIMaxLuma)
                Irradiance *= kGIMaxLuma / giLuma;
        }

        SH sh_indirect = init_SH();
        sh_indirect = irradiance_to_SH(Irradiance, sampleDirWorld);

        RTGIStoreCheckerboard(launchIndex.xy, launchDim.xy, sh_indirect.shY, float4(Irradiance, 1.0f));
    }
    else
    {
        float3 LightDir = CommonSafeNormalize(LightDirAndIntensity.xyz, float3(0.0f, 1.0f, 0.0f));
        payload.normal = CommonSafeNormalize(payload.normal, WorldNormal);
        float3 Albedo = max(CommonSanitizeFloat3(payload.color, 1.0f.xxx), 0.0f.xxx);
        SH sh_indirect = init_SH();
        float3 Irradiance = 0.0f.xxx;
        float NdotL = saturate(dot(LightDir, payload.normal));
        bool shadowHit = true;
        if (LightIntensity > 0.0f && NdotL > 0.0f)
        {
            float3 shadowBiasNormal = dot(payload.normal, LightDir) < 0.0f ? -payload.normal : payload.normal;
            RayDesc shadowRay;
            shadowRay.Origin = offset_ray(payload.position + shadowBiasNormal * 0.5f, shadowBiasNormal);
            shadowRay.Direction = LightDir;
            shadowRay.TMin = 0.5f;
            shadowRay.TMax = MAX_HIT_DIST;
            shadowHit = TraceDiffuseGIShadowOccluded(shadowRay);
        }
        if(shadowHit == false)
        {
            // miss - apply light color
            Irradiance += NdotL * LightIntensity * max(CommonSanitizeFloat3(LightColor, 1.0f.xxx), 0.0f.xxx) * Albedo * INV_PI;
        }
        Irradiance += EvaluatePointLightBounce(payload.position, payload.normal, Albedo, launchIndex.xy);

        // Sanitize before it reaches DLSS-RR: a point light close to a GI hit makes the
        // 1/d^2 term blow up (Inf / huge firefly), which pollutes the RR input color and
        // makes RR collapse the whole frame to black. NaN passes firefly luma compares
        // (always false), so an explicit sanitize is required, then a luminance cap.
        {
            Irradiance = max(CommonSanitizeFloat3(Irradiance, 0.0f.xxx), 0.0f.xxx);
            float giLuma = dot(Irradiance, float3(0.2126f, 0.7152f, 0.0722f));
            const float kGIMaxLuma = 16.0f;
            if (giLuma > kGIMaxLuma)
                Irradiance *= kGIMaxLuma / giLuma;
        }

        sh_indirect = irradiance_to_SH(Irradiance, sampleDirWorld);

        RTGIStoreCheckerboard(launchIndex.xy, launchDim.xy, sh_indirect.shY, float4(Irradiance, 1.0f));
    }


}

[shader("miss")]
void miss(inout RayPayload payload)
{
    payload.position = float3(0, 0, 0);
    payload.color = 0.0f.xxx;
    payload.normal = float3(0, 0, -1);
    payload.bHit = false;
}

[shader("closesthit")]
void chs(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();
    Vertex vertex = CORONA_GET_SURFACE_VERTEX_ATTRIBUTES(instanceID, triangleIndex, barycentrics);

    payload.position = CommonSanitizeFloat3(vertex.position, WorldRayOrigin() + WorldRayDirection() * RayTCurrent());
    float3 hitNormal = CommonSafeNormalize(vertex.normal, -WorldRayDirection());
    if (dot(hitNormal, -WorldRayDirection()) < 0.0f)
        hitNormal = -hitNormal;
    payload.normal = hitNormal;

    RTMaterialRecord material = RtMaterials[instanceID];
    vertex.textureLODConstant += material.AlbedoLodConstant;
    float hitT = RayTCurrent();
    float rayConeWidth = ViewSpreadAngle * hitT;

    float NoV = 1;//dot(V, vertex.normal);
    float mipLevel = computeTextureLOD(NoV, rayConeWidth, vertex.textureLODConstant);

    float3 baseColor = MaterialTextures[NonUniformResourceIndex(material.AlbedoTextureIndex)].SampleLevel(sampleWrap, vertex.uv, mipLevel).xyz * material.BaseColorFactor.xyz;
    payload.color = max(CommonSanitizeFloat3(baseColor, 1.0f.xxx), 0.0f.xxx);


    payload.bHit = true;
}

[shader("miss")]
void missShadow(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}
