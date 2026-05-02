#include "Common.hlsl"


RWTexture2D<float4> ShadowResult : register(u0);
RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
ByteAddressBuffer vertices : register(t3);
ByteAddressBuffer indices : register(t4);
Texture2D AlbedoTex : register(t5);
ByteAddressBuffer InstanceProperty : register(t6);
Texture2D GeoNormalTex : register(t7);


cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4x4 InvProjMatrix;
    float4 ProjectionParams;
    float4 LightDir;
    float ShadowLightRadius;
    uint ShadowSampleCount;
    float2 _padding;
    float4 pad;
};
SamplerState sampleWrap : register(s0);


float3 linearToSrgb(float3 c)
{
    // Based on http://chilliant.blogspot.com/2012/08/srgb-approximations-for-hlsl.html
    float3 sq1 = sqrt(c);
    float3 sq2 = sqrt(sq1);
    float3 sq3 = sqrt(sq2);
    float3 srgb = 0.662002687 * sq1 + 0.684122060 * sq2 - 0.323583601 * sq3 - 0.0225411470 * c;
    return srgb;
}

struct RayPayload
{
    uint bHit;
    float3 _padding;
};

static const uint RT_SHADOW_RAY_FLAGS =
    RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
    RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
    RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

float random(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float3 offset_ray(float3 p, float3 n)
{
    return p + n * (1.0f / 256.0f);
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
        ShadowResult[pixelPos] = float4(1.0f.xxx, 1.0f);
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

    float3 traceNormal = CommonSafeNormalize(geoNormal + worldNormal * 0.25f, geoNormal);
    float3 baseLightDir = CommonSafeNormalize(LightDir.xyz, float3(0.0f, 1.0f, 0.0f));
    float visibility = 0.0f;
    const uint kMaxShadowSamples = 16;
    uint sampleCount = min(max(ShadowSampleCount, 1), kMaxShadowSamples);
    float normalBias = 0.5f;

    [loop]
    for (uint sampleIndex = 0; sampleIndex < kMaxShadowSamples; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
            break;

        float2 randUV = float2(
            random(float2(pixelPos) + float2(sampleIndex * 13.17f, 17.31f)),
            random(float2(pixelPos) + float2(sampleIndex * 29.73f, 47.77f)));
        float3 rayDir = SampleDirectionalLightSphereCap(baseLightDir, ShadowLightRadius, randUV);
        float3 rayBiasNormal = dot(traceNormal, rayDir) < 0.0f ? -traceNormal : traceNormal;

        RayDesc ray;
        ray.Origin = worldPos + rayBiasNormal * normalBias;
        ray.Direction = rayDir;
        ray.TMin = max(0.05f, normalBias * 0.25f);
        ray.TMax = 100000;

        RayPayload payload;
        payload.bHit = 1u;
        payload._padding = 0.0f.xxx;
        TraceRay(gRtScene,
            RT_SHADOW_RAY_FLAGS,
            0xFF, 0, 0, 0, ray, payload);

        visibility += payload.bHit == 0u ? 1.0f : 0.0f;
    }

    visibility /= sampleCount;
    ShadowResult[pixelPos] = float4(visibility.xxx, 1.0);

}

[shader("miss")]
void miss(inout RayPayload payload)
{
    // payload.opacity = 0.0;
    payload.bHit = 0u;
    payload._padding = 0.0f.xxx;
}

[shader("anyhit")]
void anyhit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();

    if (!IsAlphaTestedInstance(instanceID, InstanceProperty))
    {
        AcceptHitAndEndSearch();
        return;
    }

    Vertex vertex = GetVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);
    float opacity = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, 5).w;

        // payload.bHit = false;

    if(opacity > 0.10)
    {
        AcceptHitAndEndSearch();
        return;
    }
    
    IgnoreHit();
}
