#include "Common.hlsl"

RWTexture2D<float4> AmbientOcclusionResult : register(u0);

RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
Texture3D BlueNoiseTex : register(t3);
ByteAddressBuffer vertices : register(t4);
ByteAddressBuffer indices : register(t5);
Texture2D AlbedoTex : register(t6);
ByteAddressBuffer InstanceProperty : register(t7);
Texture2D GeoNormalTex : register(t8);

cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4x4 InvProjMatrix;
    float4 ProjectionParams;
    float2 RTSize;
    float Radius;
    float Power;
    uint SampleCount;
    uint FrameCounter;
    uint NoiseMode;
    uint BlueNoiseOffsetStride;
    float NormalBias;
    float3 _padding;
};

SamplerState sampleWrap : register(s0);

struct AOPayload
{
    uint bHit;
    float HitT;
    float2 _padding;
};

static const uint RT_AO_RAY_FLAGS = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

float3x3 BuildAOTBN(float3 normal)
{
    normal = CommonSafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));

    const float3 rvec1 = float3(0.847100675f, 0.207911700f, 0.489073813f);
    const float3 rvec2 = float3(-0.639436305f, -0.390731126f, 0.662155867f);
    float3 rvec = dot(rvec1, normal) > 0.95f ? rvec2 : rvec1;
    float3 tangent = CommonSafeNormalize(rvec - normal * dot(rvec, normal), float3(1.0f, 0.0f, 0.0f));
    float3 bitangent = CommonSafeNormalize(cross(normal, tangent), float3(0.0f, 0.0f, 1.0f));

    return float3x3(tangent, bitangent, normal);
}

float2 SampleAOSequence(uint2 pixelPos, uint sampleIndex)
{
    float2 baseNoise = LoadRayNoise2(BlueNoiseTex, pixelPos, FrameCounter + sampleIndex * 17u, BlueNoiseOffsetStride, NoiseMode);
    uint seed = pixelPos.x * 1973u + pixelPos.y * 9277u + FrameCounter * 26699u + sampleIndex * 104729u;
    float2 hashed = float2(HashToUnitFloat(seed), HashToUnitFloat(seed ^ 0x9E3779B9u));
    return frac(baseNoise + hashed);
}

[shader("raygeneration")]
void rayGen()
{
    uint3 launchIndex = DispatchRaysIndex();
    uint3 launchDim = DispatchRaysDimensions();
    uint2 pixelPos = launchIndex.xy;

    float2 launchSize = float2(max(launchDim.x, 1u), max(launchDim.y, 1u));
    float2 uv = (float2(pixelPos) + float2(0.5f, 0.5f)) / launchSize;
    float deviceDepth = DepthTex.SampleLevel(sampleWrap, uv, 0).x;
    if (deviceDepth >= 0.999999f)
    {
        AmbientOcclusionResult[pixelPos] = float4(1.0f.xxx, 1.0f);
        return;
    }

    float2 screenPosition = uv * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;
    float3 viewPosition = GetViewPosition(deviceDepth, screenPosition, InvProjMatrix);
    float3 worldPos = mul(float4(viewPosition, 1.0f), InvViewMatrix).xyz;

    float3 geoNormal = CommonSafeNormalize(GeoNormalTex.SampleLevel(sampleWrap, uv, 0).xyz, float3(0.0f, 1.0f, 0.0f));
    float3 worldNormal = CommonSafeNormalize(WorldNormalTex.SampleLevel(sampleWrap, uv, 0).xyz, geoNormal);
    float3 cameraWorld = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
    float3 surfaceToCamera = CommonSafeNormalize(cameraWorld - worldPos, -worldNormal);
    if (dot(geoNormal, surfaceToCamera) < 0.0f)
        geoNormal = -geoNormal;
    if (dot(worldNormal, geoNormal) < 0.0f)
        worldNormal = -worldNormal;

    const uint kMaxSamples = 16u;
    uint sampleCount = min(max(SampleCount, 1u), kMaxSamples);
    float rayRadius = clamp(Radius, 0.01f, 256.0f);
    float normalBias = clamp(NormalBias, 0.001f, 2.0f);
    float3 traceNormal = CommonSafeNormalize(geoNormal + worldNormal * 0.25f, geoNormal);
    float3 rayOrigin = worldPos + traceNormal * normalBias;
    float3x3 tbn = BuildAOTBN(traceNormal);

    float occluded = 0.0f;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < kMaxSamples; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
            break;

        float2 randomUV = SampleAOSequence(pixelPos, sampleIndex);
        float3 localDir = SampleHemisphereCosine(randomUV.x, randomUV.y);
        float3 rayDir = CommonSafeNormalize(mul(localDir, tbn), worldNormal);

        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = rayDir;
        ray.TMin = max(0.05f, normalBias * 0.25f);
        ray.TMax = rayRadius;

        AOPayload payload;
        payload.bHit = 0u;
        payload.HitT = rayRadius;
        payload._padding = 0.0f.xx;
        TraceRay(
            gRtScene,
            RT_AO_RAY_FLAGS,
            0xFF,
            0,
            0,
            0,
            ray,
            payload);

        if (payload.bHit != 0u)
        {
            float hitVisibility = saturate(payload.HitT / rayRadius);
            float contactWeight = 1.0f - hitVisibility;
            occluded += contactWeight * contactWeight;
        }
    }

    float ao = 1.0f - occluded / float(sampleCount);
    ao = pow(saturate(ao), clamp(Power, 0.25f, 4.0f));
    AmbientOcclusionResult[pixelPos] = float4(ao.xxx, 1.0f);
}

[shader("miss")]
void miss(inout AOPayload payload)
{
    payload.bHit = 0u;
    payload.HitT = Radius;
    payload._padding = 0.0f.xx;
}

[shader("closesthit")]
void closesthit(inout AOPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.bHit = 1u;
    payload.HitT = RayTCurrent();
}

[shader("anyhit")]
void anyhit(inout AOPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();

    if (!IsAlphaTestedInstance(instanceID, InstanceProperty))
    {
        return;
    }

    Vertex vertex = GetVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);

    float opacity = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, 5).w;
    if (opacity > 0.10f)
    {
        return;
    }

    IgnoreHit();
}
