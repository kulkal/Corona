#include "Common.hlsl"
#include "BindlessResources.hlsli"

RWTexture2D<float4> SkyLightingResult : register(u0);

RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
Texture3D RayNoiseBlueNoiseSource : register(t3);
Texture2D GeoNormalTex : register(t8);

cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4x4 InvProjMatrix;
    float4 ProjectionParams;
    float2 RTSize;
    float RayLength;
    float NormalBias;
    float3 SkyColorTop;
    float SkyIntensity;
    float3 SkyColorBottom;
    uint SampleCount;
    uint FrameCounter;
    uint NoiseMode;
    uint BlueNoiseOffsetStride;
    float SkyUpBias;
    float SkyDirectionPower;
    float SkyMinWorldY;
    uint SkyMaxSampleAttempts;
    uint _padding;
};

SamplerState sampleWrap : register(s0);

struct SkyPayload
{
    uint bHit;
    float3 _padding;
};

static const uint RT_SKY_RAY_FLAGS =
    RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
    RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
    RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

float3x3 BuildSkyTBN(float3 normal)
{
    normal = CommonSafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));

    const float3 rvec1 = float3(0.847100675f, 0.207911700f, 0.489073813f);
    const float3 rvec2 = float3(-0.639436305f, -0.390731126f, 0.662155867f);
    float3 rvec = dot(rvec1, normal) > 0.95f ? rvec2 : rvec1;
    float3 tangent = CommonSafeNormalize(rvec - normal * dot(rvec, normal), float3(1.0f, 0.0f, 0.0f));
    float3 bitangent = CommonSafeNormalize(cross(normal, tangent), float3(0.0f, 0.0f, 1.0f));

    return float3x3(tangent, bitangent, normal);
}

float3 EvaluateSkyColor(float3 direction)
{
    float t = 0.5f * (CommonSafeNormalize(direction, float3(0.0f, 1.0f, 0.0f)).y + 1.0f);
    return max(lerp(SkyColorBottom, SkyColorTop, t) * SkyIntensity, 0.0f.xxx);
}

float ComputeDiffuseSkySampleWeight(float3 rayDir, float3 traceNormal, float3 guideNormal, float directionPower)
{
    float traceCos = saturate(dot(rayDir, traceNormal));
    float guideCos = saturate(dot(rayDir, guideNormal));
    if (traceCos <= 0.0f || guideCos <= 0.0f)
        return 0.0f;

    float power = clamp(directionPower, 0.25f, 8.0f);
    float oneMinusGuideCosSq = max(1.0f - guideCos * guideCos, 1e-4f);
    float poweredPdfFactor = pow(oneMinusGuideCosSq, rcp(power) - 1.0f);

    // The sky rays are sampled around guideNormal for faster convergence, but
    // the diffuse term is the cosine integral around traceNormal. Convert the
    // guided PDF back to the desired diffuse estimator so up-biased rays do not
    // over-light vertical or recessed surfaces.
    float guidePdfOverInvPi = max(guideCos * poweredPdfFactor / power, 1e-4f);
    return min(traceCos / guidePdfOverInvPi, 4.0f);
}

float2 SampleSkySequence(uint2 pixelPos, uint sampleIndex)
{
    float2 baseNoise = GenerateRaySample2D(RayNoiseBlueNoiseSource, pixelPos, FrameCounter + sampleIndex * 19u, BlueNoiseOffsetStride, NoiseMode);
    uint seed = pixelPos.x * 1973u + pixelPos.y * 9277u + FrameCounter * 26699u + sampleIndex * 1299709u;
    float2 hashed = float2(HashToUnitFloat(seed), HashToUnitFloat(seed ^ 0x85EBCA6Bu));
    return frac(baseNoise + hashed);
}

[shader("raygeneration")]
void rayGen()
{
    uint3 launchIndex = DispatchRaysIndex();
    uint3 launchDim = DispatchRaysDimensions();
    uint2 pixelPos = launchIndex.xy;

    float2 launchSize = float2(max(launchDim.x, 1u), max(launchDim.y, 1u));
    float2 uv = (float2(pixelPos) + 0.5f.xx) / launchSize;
    float deviceDepth = DepthTex.SampleLevel(sampleWrap, uv, 0).x;
    if (deviceDepth >= 0.999999f)
    {
        SkyLightingResult[pixelPos] = float4(0.0f.xxx, 1.0f);
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
    float3 rayOrigin = worldPos + traceNormal * clamp(NormalBias, 0.001f, 4.0f);
    float3 worldUp = float3(0.0f, 1.0f, 0.0f);
    float3 guideNormal = CommonSafeNormalize(lerp(traceNormal, worldUp, saturate(SkyUpBias)), traceNormal);
    float3x3 tbn = BuildSkyTBN(guideNormal);

    const uint kMaxSamples = 32u;
    uint sampleCount = min(max(SampleCount, 1u), kMaxSamples);
    uint maxAttempts = min(max(SkyMaxSampleAttempts, 1u), 8u);
    float rayLength = clamp(RayLength, 1.0f, 100000.0f);
    float minWorldY = clamp(SkyMinWorldY, -0.25f, 0.75f);
    float directionPower = clamp(SkyDirectionPower, 0.25f, 8.0f);
    float3 skyRadiance = 0.0f.xxx;
    float visibleCount = 0.0f;

    [loop]
    for (uint sampleIndex = 0u; sampleIndex < kMaxSamples; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
            break;

        float3 rayDir = guideNormal;
        bool bAcceptedDirection = false;

        [loop]
        for (uint attemptIndex = 0u; attemptIndex < 8u; ++attemptIndex)
        {
            if (attemptIndex >= maxAttempts)
                break;

            float2 randomUV = SampleSkySequence(pixelPos, sampleIndex + attemptIndex * sampleCount);
            float3 localDir = SampleHemisphereCosine(pow(saturate(randomUV.x), directionPower), randomUV.y);
            float3 candidateDir = CommonSafeNormalize(mul(localDir, tbn), guideNormal);
            if (dot(candidateDir, traceNormal) > 0.001f && candidateDir.y >= minWorldY)
            {
                rayDir = candidateDir;
                bAcceptedDirection = true;
                break;
            }
        }

        if (!bAcceptedDirection)
        {
            rayDir = CommonSafeNormalize(traceNormal + worldUp * saturate(SkyUpBias), traceNormal);
            if (dot(rayDir, traceNormal) <= 0.001f || rayDir.y < minWorldY)
                rayDir = traceNormal;
        }

        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = rayDir;
        ray.TMin = 0.05f;
        ray.TMax = rayLength;

        SkyPayload payload;
        payload.bHit = 1u;
        payload._padding = 0.0f.xxx;
        TraceRay(
            gRtScene,
            RT_SKY_RAY_FLAGS,
            0xFF,
            0,
            0,
            0,
            ray,
            payload);

        if (payload.bHit == 0u)
        {
            visibleCount += 1.0f;
            skyRadiance += EvaluateSkyColor(rayDir) * ComputeDiffuseSkySampleWeight(rayDir, traceNormal, guideNormal, directionPower);
        }
    }

    float invSampleCount = rcp(float(sampleCount));
    SkyLightingResult[pixelPos] = float4(skyRadiance * invSampleCount, visibleCount * invSampleCount);
}

[shader("miss")]
void miss(inout SkyPayload payload)
{
    payload.bHit = 0u;
    payload._padding = 0.0f.xxx;
}

[shader("anyhit")]
void anyhit(inout SkyPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();

    if (!IsAlphaTestedInstance(instanceID, CORONA_INSTANCE_PROPERTY))
        return;

    Vertex vertex = CORONA_GET_VERTEX_ATTRIBUTES(instanceID, triangleIndex, barycentrics);
    float opacity = 1.0f;
    RTMaterialRecord material = RtMaterials[instanceID];
    opacity = MaterialTextures[NonUniformResourceIndex(material.AlbedoTextureIndex)].SampleLevel(sampleWrap, vertex.uv, 5).w;

    if (opacity > 0.10f)
        return;

    IgnoreHit();
}
