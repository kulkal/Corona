#include "Common.hlsl"

RWTexture2D<float4> ReflectionResult : register(u0);
RWTexture2D<float> SpecularHitDistanceResult : register(u1);
RWTexture2D<float2> SpecularMotionVectorResult : register(u2);

RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D GeoNormalTex : register(t2);
Texture2D RougnessMetallicTex : register(t6);
Texture3D RayNoiseBlueNoiseSource : register(t7);
Texture2D WorldNormalTex : register(t8);
ByteAddressBuffer vertices : register(t3);
ByteAddressBuffer indices : register(t4);
Texture2D AlbedoTex : register(t5);
ByteAddressBuffer InstanceProperty : register(t9);

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
    float2 RandomOffset;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    float ViewSpreadAngle;
    uint NoiseMode;
    float2 NoisePadding;
    float3 SkyColorTop;
    float SkyIntensity;
    float3 SkyColorBottom;
    float _padding;
    float3 LightColor;
    float PrefilteredEnvRoughnessThreshold;
    float PrefilteredEnvRoughnessFade;
    uint bEnablePrefilteredEnvSpecular;
    float SpecularMotionVectorScale;
    uint bWriteRRSpecularMotionVectors;
    uint bWriteRRSpecularHitDistance;
    uint bUseRRSpecularGuideRay;
};

SamplerState sampleWrap : register(s0);

float SpecSanitizeFloat(float value, float fallback)
{
    return value;
}

float3 SpecSanitizeFloat3(float3 value, float3 fallback)
{
    return value;
}

float4 SpecSanitizeFloat4(float4 value, float4 fallback)
{
    return value;
}

float3 SpecSafeNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return lenSq > 1e-12f ? value * rsqrt(lenSq) : fallback;
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

struct RT_REFLECTION_RAY_PAYLOAD RayPayload
{
    float3 position RT_REFLECTION_PAYLOAD_RW;
    float3 color RT_REFLECTION_PAYLOAD_RW;
    float3 normal RT_REFLECTION_PAYLOAD_RW;
    float spreadAngle RT_REFLECTION_PAYLOAD_RW;
    float coneWidth RT_REFLECTION_PAYLOAD_RW;
    float hitDist RT_REFLECTION_PAYLOAD_RW;
    bool bHit RT_REFLECTION_PAYLOAD_RW;
};

struct RT_REFLECTION_SHADOW_RAY_PAYLOAD ShadowRayPayload
{
    bool bHit RT_REFLECTION_SHADOW_PAYLOAD_RW;
};

/*
    Params.x = Far / (Far - Near);
    Params.y = Near / (Near - Far);
    Params.z = Far;
*/

float3 offset_ray(float3 p, float3 n)
{
    return p + n * (1.0f / 256.0f);
}

// Returns quaternion of rotation from stc to dst
float4 getOrientation(float3 src, float3 dst)
{
    // If the rotation is larger than pi/2 then do it from the other side.
    // 1. rotate by pi around (1,0,0)
    // 2. find shortest rotation from there
    // 3. return the quaternion which does the full rotation
    float tmp = dot(src, dst);
    bool flip = tmp < 0;
    [flatten] if (flip)
    {
        src = float3(src.x, -src.y, -src.z);
    }
    float3 v = cross(src, dst);
    float4 q;
    // TODO: This can be made somewhat faster with rsqrt and rcp, but then also need to normalize
    q.w = sqrt((1 + abs(tmp)) / 2);
    q.xyz = v / (2 * q.w);
    [flatten] if (flip)
    {
        q = float4(q.w, q.z, -q.y, -q.x);
    }
    return q;
}

// Transform a vector v with a quaternion q
// v doesn't need to be normalized
float3 orientVector(float4 q, float3 v)
{
    return v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

float4 inverseOrientation(float4 q)
{
    return float4(-q.xyz, q.w);
}

float3 ImportanceSampleGGX_VNDF(float2 u, float roughness, float3 V, float3x3 TBN, float3 N)
{
    roughness = clamp(SpecSanitizeFloat(roughness, 0.5f), 0.02f, 1.0f);
    float alpha = square(roughness);

    // float3 Ve = -float3(dot(V, TBN[0]), dot(V, TBN[1]), dot(V, TBN[2]));
    float3 Ve = SpecSafeNormalize(mul(V, transpose(TBN)), float3(0.0f, 0.0f, 1.0f));

    float3 Vh = SpecSafeNormalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z), float3(0.0f, 0.0f, 1.0f));
    
    float lensq = square(Vh.x) + square(Vh.y);
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);

    float r = sqrt(u.x);
    float phi = 2.0 * PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(saturate(1.0 - square(t1))) + s * t2;

    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - square(t1) - square(t2))) * Vh;

    // Tangent space H
    float3 Ne = float3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z));

    return SpecSafeNormalize(mul(Ne, TBN), N);
}

float3x3 buildTBN(float3 normal) {

    // TODO: Maybe try approach from here (Building an Orthonormal Basis, Revisited): 
    // https://graphics.pixar.com/library/OrthonormalB/paper.pdf

    normal = SpecSafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));

    // Pick random vector for generating orthonormal basis
    static const float3 rvec1 = float3(0.847100675f, 0.207911700f, 0.489073813f);
    static const float3 rvec2 = float3(-0.639436305f, -0.390731126f, 0.662155867f);
    float3 rvec;

    if (dot(rvec1, normal) > 0.95f)
        rvec = rvec2;
    else
        rvec = rvec1;

    // Construct TBN matrix to orient sampling hemisphere along the surface normal
    float3 b1 = SpecSafeNormalize(rvec - normal * dot(rvec, normal), float3(1.0f, 0.0f, 0.0f));
    float3 b2 = SpecSafeNormalize(cross(normal, b1), float3(0.0f, 0.0f, 1.0f));
    float3x3 tbn = float3x3(b1, b2, normal);

    return tbn;
}

float3 Reinhard(in float3 color)
{
    return color;
    float Luminance = RGBToLuminance(color);
    const float max_white = 2.0;
    color = color*(1.0f + color/(max_white*max_white));
    return color/(1 + color);
}

float3 SampleSkyEnvironment(float3 rayDir)
{
    rayDir = SpecSafeNormalize(rayDir, float3(0.0f, 1.0f, 0.0f));
    float t = 0.5f * (rayDir.y + 1.0f);
    float skyIntensity = max(SpecSanitizeFloat(SkyIntensity, 0.0f), 0.0f);
    float3 skyColor = lerp(SkyColorBottom, SkyColorTop, saturate(t));
    return max(SpecSanitizeFloat3(skyColor * skyIntensity, 0.0f.xxx), 0.0f.xxx);
}

float3 SamplePrefilteredSkyEnvironment(float3 reflectionDir, float roughness)
{
    reflectionDir = SpecSafeNormalize(reflectionDir, float3(0.0f, 1.0f, 0.0f));
    roughness = saturate(roughness);

    float3 helperUp = abs(reflectionDir.y) < 0.98f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = SpecSafeNormalize(cross(helperUp, reflectionDir), float3(1.0f, 0.0f, 0.0f));
    float3 bitangent = SpecSafeNormalize(cross(reflectionDir, tangent), float3(0.0f, 0.0f, 1.0f));
    float coneWidth = lerp(0.05f, 1.35f, roughness * roughness);
    float verticalWeight = saturate(roughness * roughness * 0.75f);

    float3 radiance = SampleSkyEnvironment(reflectionDir) * 4.0f;
    radiance += SampleSkyEnvironment(reflectionDir + tangent * coneWidth);
    radiance += SampleSkyEnvironment(reflectionDir - tangent * coneWidth);
    radiance += SampleSkyEnvironment(reflectionDir + bitangent * coneWidth);
    radiance += SampleSkyEnvironment(reflectionDir - bitangent * coneWidth);
    radiance += SampleSkyEnvironment(float3(0.0f, 1.0f, 0.0f)) * verticalWeight;
    radiance += SampleSkyEnvironment(float3(0.0f, -1.0f, 0.0f)) * verticalWeight;

    return radiance / (8.0f + 2.0f * verticalWeight);
}

static const float MAX_HIT_DIST = 10000;

#define RT_REFLECTION_SURFACE_RAY_FLAGS (RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES)

void TraceReflectionSurfaceRay(RayDesc ray, inout RayPayload payload)
{
#if RT_REFLECTION_USE_SER
    dx::HitObject hit = dx::HitObject::TraceRay(
        gRtScene,
        RT_REFLECTION_SURFACE_RAY_FLAGS,
        0xFF,
        0,
        0,
        0,
        ray,
        payload);
    dx::MaybeReorderThread(hit, hit.GetInstanceID(), RT_REFLECTION_SER_MATERIAL_HINT_BITS);
    dx::HitObject::Invoke(hit, payload);
#else
    TraceRay(
        gRtScene,
        RT_REFLECTION_SURFACE_RAY_FLAGS,
        0xFF,
        0,
        0,
        0,
        ray,
        payload);
#endif
}

bool ProjectToScreenUVChecked(float3 worldPos, float4x4 viewProj, out float2 uv)
{
    float4 clip = mul(float4(worldPos, 1.0f), viewProj);
    if (abs(clip.w) <= 1.0e-6f)
    {
        uv = float2(0.0f, 0.0f);
        return false;
    }

    float3 ndc = clip.xyz * rcp(clip.w);
    uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(isnan(uv)) || any(isinf(uv)) || any(isnan(ndc)) || any(isinf(ndc)))
        return false;

    return ndc.z >= 0.0f && ndc.z <= 1.0f &&
           all(uv >= float2(-0.25f, -0.25f)) &&
           all(uv <= float2(1.25f, 1.25f));
}

void WriteRRSpecularGuides(uint2 pixel, uint2 renderSize, bool primarySurfaceValid, float3 primaryWorldPos, float3 primaryGeomNormal, float3 mirrorDir, float roughness, float metallic, RayPayload reflectionPayload)
{
    float specularHitDistance = ProjectionParams.w;
    float2 specularMotionVector = float2(0.0f, 0.0f);

    if (!primarySurfaceValid || (bWriteRRSpecularHitDistance == 0 && bWriteRRSpecularMotionVectors == 0))
    {
        SpecularHitDistanceResult[pixel] = specularHitDistance;
        SpecularMotionVectorResult[pixel] = specularMotionVector;
        return;
    }

    RayPayload guidePayload = reflectionPayload;
    bool guideHit = reflectionPayload.bHit;

    float smoothGuide = saturate((0.38f - roughness) / 0.18f);
    float metalGuide = saturate(metallic) * saturate((0.55f - roughness) / 0.25f);
    float specularEnergy = lerp(0.04f, 1.0f, saturate(metallic));
    float specularGuideWeight = specularEnergy * max(smoothGuide, metalGuide);

    if (bUseRRSpecularGuideRay != 0 && specularGuideWeight > 0.025f && dot(mirrorDir, primaryGeomNormal) > 1.0e-4f)
    {
        RayDesc guideRay;
        guideRay.Origin = SpecSanitizeFloat3(primaryWorldPos + primaryGeomNormal * 0.5f, primaryWorldPos);
        guideRay.Direction = SpecSafeNormalize(mirrorDir, primaryGeomNormal);
        guideRay.TMin = 0.01f;
        guideRay.TMax = min(ProjectionParams.w, MAX_HIT_DIST);

        guidePayload.position = guideRay.Origin + guideRay.Direction * guideRay.TMax;
        guidePayload.color = 0.0f.xxx;
        guidePayload.normal = primaryGeomNormal;
        guidePayload.coneWidth = 0.0f;
        guidePayload.spreadAngle = 0.0f;
        guidePayload.hitDist = ProjectionParams.w;
        guidePayload.bHit = false;
        TraceReflectionSurfaceRay(guideRay, guidePayload);
        guideHit = guidePayload.bHit;
    }

    if (guideHit)
    {
        specularHitDistance = clamp(guidePayload.hitDist, 0.0f, ProjectionParams.w);
        if (bWriteRRSpecularMotionVectors != 0)
        {
            float2 specCurrentUV;
            float2 specPrevUV;
            if (ProjectToScreenUVChecked(guidePayload.position, UnjitteredViewProjMatrix, specCurrentUV) &&
                ProjectToScreenUVChecked(guidePayload.position, PrevUnjitteredViewProjMatrix, specPrevUV))
            {
                float2 candidateMotionVector = (specPrevUV - specCurrentUV) * float2(renderSize) * SpecularMotionVectorScale;
                if (all(abs(candidateMotionVector) <= float2(renderSize)))
                    specularMotionVector = candidateMotionVector;
            }
        }
    }

    SpecularHitDistanceResult[pixel] = (bWriteRRSpecularHitDistance != 0) ? specularHitDistance : ProjectionParams.w;
    SpecularMotionVectorResult[pixel] = (bWriteRRSpecularMotionVectors != 0) ? specularMotionVector : float2(0.0f, 0.0f);
}

[shader("raygeneration")]
void rayGen
()
{
    uint3 launchIndex = DispatchRaysIndex();
    uint3 launchDim = DispatchRaysDimensions();

    float2 crd = float2(launchIndex.xy);
	//crd.y *= -1;
    float2 dims = float2(launchDim.xy);

    float2 dim = ((crd / dims) * 2.f - 1.f);
    dim *= tan(0.8 / 2);
    float aspectRatio = dims.x / dims.y;


	float2 UV = crd / dims;
	float DeviceDepth = DepthTex.SampleLevel(sampleWrap, UV, 0).x;
    bool primarySurfaceValid = DeviceDepth < 0.999999f;

	float3 WorldNormal = SpecSafeNormalize(WorldNormalTex.SampleLevel(sampleWrap, UV, 0).xyz, float3(0.0f, 1.0f, 0.0f));
  
    float3 GeoNormal = SpecSafeNormalize(GeoNormalTex.SampleLevel(sampleWrap, UV, 0).xyz, WorldNormal);

    float LinearDepth = GetLinearDepthOpenGL(DeviceDepth, ProjectionParams.z, ProjectionParams.w) ;

	float2 ScreenPosition = crd.xy;
	ScreenPosition.x /= dims.x;
	ScreenPosition.y /= dims.y;
	ScreenPosition.xy = ScreenPosition.xy * 2 - 1;
	ScreenPosition.y = -ScreenPosition.y;

	// float3 ViewPosition = GetViewPosition(LinearDepth, ScreenPosition, ProjMatrix._11, ProjMatrix._22);
    float3 ViewPosition = GetViewPosition(DeviceDepth, ScreenPosition, InvProjMatrix);

	float3 WorldPos = SpecSanitizeFloat3(mul(float4(ViewPosition, 1), InvViewMatrix).xyz, 0.0f.xxx);

    float4 material = SpecSanitizeFloat4(RougnessMetallicTex.SampleLevel(sampleWrap, UV, 0), float4(0.65f, 0.0f, 0.0f, 0.0f));
    float Rougness = clamp(material.x, 0.02f, 1.0f);
    float Metallic = saturate(material.y);
    float3 viewRay = SpecSafeNormalize(float3(dim.x * aspectRatio, -dim.y, -1), float3(0.0f, 0.0f, -1.0f));
    float3 V = SpecSafeNormalize(mul(float4(viewRay, 0.0f), InvViewMatrix).xyz, -WorldNormal);
    float3 MirrorL = SpecSafeNormalize(reflect(V, WorldNormal), WorldNormal);

    const bool enablePrefilteredEnvSpecular = bEnablePrefilteredEnvSpecular != 0;
    float envThreshold = saturate(SpecSanitizeFloat(PrefilteredEnvRoughnessThreshold, 0.65f));
    float envFade = enablePrefilteredEnvSpecular ? max(SpecSanitizeFloat(PrefilteredEnvRoughnessFade, 0.0f), 0.0f) : 0.0f;
    float envBlendStart = max(envThreshold - envFade, 0.0f);
    float prefilteredEnvBlend = enablePrefilteredEnvSpecular ? (envFade <= 1e-4f
        ? (Rougness >= envThreshold ? 1.0f : 0.0f)
        : saturate((Rougness - envBlendStart) / max(envThreshold - envBlendStart, 1e-4f))) : 0.0f;
    prefilteredEnvBlend = prefilteredEnvBlend * prefilteredEnvBlend * (3.0f - 2.0f * prefilteredEnvBlend);
    prefilteredEnvBlend = enablePrefilteredEnvSpecular && Rougness >= envThreshold ? 1.0f : prefilteredEnvBlend;
    float3 prefilteredEnvRadiance = SamplePrefilteredSkyEnvironment(MirrorL, Rougness);
    bool useDeterministicPrefilteredEnvRay = prefilteredEnvBlend >= 0.999f;
    float3 L = MirrorL;
    if (!useDeterministicPrefilteredEnvRay)
    {
        float2 RandomUV = GenerateRaySample2D(RayNoiseBlueNoiseSource, launchIndex.xy, FrameCounter, BlueNoiseOffsetStride, NoiseMode);
        float3x3 TBN = buildTBN(WorldNormal);
        float3 H = ImportanceSampleGGX_VNDF(RandomUV, Rougness, -V, TBN, WorldNormal);
        L = SpecSafeNormalize(reflect(V, H), reflect(V, WorldNormal));
    }

    float LightIntensity = max(SpecSanitizeFloat(LightDirAndIntensity.w, 0.0f), 0.0f);


	RayDesc ray;
	ray.Origin = SpecSanitizeFloat3(WorldPos + GeoNormal * 0.5f, WorldPos);
	ray.Direction = SpecSafeNormalize(L, WorldNormal);

	ray.TMin = 0;
	ray.TMax = MAX_HIT_DIST;

	RayPayload payload;
    payload.position = ray.Origin + ray.Direction * MAX_HIT_DIST;
    payload.color = 0.0f.xxx;
    payload.normal = WorldNormal;
    payload.coneWidth = 0;
    payload.spreadAngle = max(SpecSanitizeFloat(ViewSpreadAngle, 0.0f), 0.0f);
    payload.hitDist = MAX_HIT_DIST;
    payload.bHit = false;
    TraceReflectionSurfaceRay(ray, payload);

    float3 tracedRadiance = 0.0f.xxx;
    if(payload.bHit == false)
    {
        // hit sky - payload.color already includes SkyIntensity from miss shader
        tracedRadiance = max(SpecSanitizeFloat3(payload.color, 0.0f.xxx), 0.0f.xxx);
    }
    else
    {
        float3 LightDir = SpecSafeNormalize(LightDirAndIntensity.xyz, float3(0.0f, 1.0f, 0.0f));
        RayDesc shadowRay;
        payload.normal = SpecSafeNormalize(payload.normal, WorldNormal);
        shadowRay.Origin = SpecSanitizeFloat3(payload.position + payload.normal * 0.5f, payload.position);
        shadowRay.Direction = LightDir;

        shadowRay.TMin = 0;
        shadowRay.TMax = MAX_HIT_DIST;

        ShadowRayPayload shadowPayload;
        shadowPayload.bHit = true;
        uint RayIndex = 0;
        TraceRay(
            gRtScene,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
                RAY_FLAG_FORCE_OPAQUE |
                RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
            0xFF,
            RayIndex,
            0,
            1,
            shadowRay,
            shadowPayload);

        float3 Irradiance = 0.0f.xxx;
        float3 Albedo = max(SpecSanitizeFloat3(payload.color, 1.0f.xxx), 0.0f.xxx);
        if(shadowPayload.bHit == false)
        {
            // miss - apply light color
            Irradiance = max(0.0f, dot(LightDir.xyz, payload.normal)) * LightIntensity * max(SpecSanitizeFloat3(LightColor, 1.0f.xxx), 0.0f.xxx) * Albedo;
        }
        else
        {
            // shadowed
        }
            
        tracedRadiance = max(Irradiance, 0.0f.xxx);
    }

    // Prefiltered environment should replace only visible sky misses. Geometry hits
    // still provide local occlusion/direct lighting, otherwise rough indoor surfaces
    // become flooded by unoccluded sky radiance.
    float3 finalRadiance = payload.bHit ? tracedRadiance : lerp(tracedRadiance, prefilteredEnvRadiance, prefilteredEnvBlend);
    ReflectionResult[launchIndex.xy] = SpecSanitizeFloat4(float4(Reinhard(max(finalRadiance, 0.0f.xxx)), 1), float4(0.0f, 0.0f, 0.0f, 1.0f));
    WriteRRSpecularGuides(launchIndex.xy, launchDim.xy, primarySurfaceValid, WorldPos, GeoNormal, MirrorL, Rougness, Metallic, payload);

    float reflectionDistance = MAX_HIT_DIST;
    if (payload.bHit)
    {
        float d = -dot(WorldNormal, WorldPos);
        float distP2Plane = PointPlaneDist(float4(WorldNormal, d), payload.position);
        reflectionDistance = abs(distP2Plane);
    }
    ReflectionResult[launchIndex.xy].w = SpecSanitizeFloat(reflectionDistance, MAX_HIT_DIST);
}



[shader("miss")]
void miss(inout RayPayload payload)
{
    // Sky color - gradient based on ray direction (same as path tracing)
    float3 rayDir = SpecSafeNormalize(WorldRayDirection(), float3(0.0f, 1.0f, 0.0f));
    
    payload.position = float3(0, 0, 0);
    payload.color = SampleSkyEnvironment(rayDir);
    payload.normal = float3(0, 0, -1);
    payload.bHit = false;
    payload.hitDist = MAX_HIT_DIST;
}

[shader("closesthit")]
void chs(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();
    Vertex vertex = GetSurfaceVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);

    float hitT = max(SpecSanitizeFloat(RayTCurrent(), MAX_HIT_DIST), 0.0f);
    payload.position = SpecSanitizeFloat3(vertex.position, WorldRayOrigin() + WorldRayDirection() * hitT);
    payload.normal = SpecSafeNormalize(vertex.normal, -WorldRayDirection());

    uint w, h;
    AlbedoTex.GetDimensions(w, h);
    float halfLog2NumTexPixels = 0.5 * log2(max(float(w) * float(h), 1.0f));

    vertex.textureLODConstant += halfLog2NumTexPixels;
    float rayConeWidth = payload.spreadAngle * hitT + payload.coneWidth;

    float NoV = 1;//dot(V, vertex.normal);
    float mipLevel = computeTextureLOD(NoV, rayConeWidth, vertex.textureLODConstant);
    payload.color = max(SpecSanitizeFloat3(AlbedoTex.SampleLevel(sampleWrap, vertex.uv, mipLevel).xyz, 1.0f.xxx), 0.0f.xxx);
    payload.bHit = true;
    payload.hitDist = hitT;
}

[shader("miss")]
void missShadow(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}
