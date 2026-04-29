#include "Common.hlsl"

RWStructuredBuffer<float4> TraceSH0 : register(u0);
RWStructuredBuffer<float4> TraceSH1 : register(u1);
RWStructuredBuffer<float4> TraceSH2 : register(u2);
RWStructuredBuffer<float4> TraceSH3 : register(u3);

RaytracingAccelerationStructure gRtScene : register(t0);
StructuredBuffer<uint> CellKeys : register(t1);
StructuredBuffer<float4> CellPosition : register(t2);
StructuredBuffer<float4> CellNormal : register(t3);
Texture3D BlueNoiseTex : register(t4);
ByteAddressBuffer vertices : register(t5);
ByteAddressBuffer indices : register(t6);
Texture2D AlbedoTex : register(t7);
ByteAddressBuffer InstanceProperty : register(t8);

SamplerState sampleWrap : register(s0);

cbuffer ViewParameter : register(b0)
{
    float4 LightDirAndIntensity;
    uint HashEntryCount;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    uint NoiseMode;
    uint RaysPerCell;
    uint MaxBounces;
    float ViewSpreadAngle;
    float RayBias;
    float CellSize;
    float3 SkyColorTop;
    float SkyIntensity;
    float3 SkyColorBottom;
    float _padding;
    float3 LightColor;
    float _padding2;
};

static const float INV_PI = 1.0f / PI;
static const float MAX_HIT_DIST = 10000.0f;

#define SPATIAL_HASH_GI_RAY_FLAGS (RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES)

struct RayPayload
{
    float3 position;
    float3 color;
    float3 normal;
    float spreadAngle;
    float coneWidth;
    bool bHit;
};

struct ShadowRayPayload
{
    bool bHit;
};

struct SH4RGB
{
    float3 c0;
    float3 c1;
    float3 c2;
    float3 c3;
};

float3 SanitizeFloat3(float3 value)
{
    if (any(isnan(value)) || any(isinf(value)))
        return 0.0f.xxx;
    return value;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    value = SanitizeFloat3(value);
    float lenSq = dot(value, value);
    if (lenSq < 1e-8f)
        return fallback;
    return value * rsqrt(lenSq);
}

SH4RGB InitSH4RGB()
{
    SH4RGB sh;
    sh.c0 = 0.0f.xxx;
    sh.c1 = 0.0f.xxx;
    sh.c2 = 0.0f.xxx;
    sh.c3 = 0.0f.xxx;
    return sh;
}

void AccumulateSH4RGB(inout SH4RGB accum, SH4RGB value, float scale)
{
    accum.c0 += value.c0 * scale;
    accum.c1 += value.c1 * scale;
    accum.c2 += value.c2 * scale;
    accum.c3 += value.c3 * scale;
}

SH4RGB ScaleSH4RGB(SH4RGB sh, float scale)
{
    sh.c0 *= scale;
    sh.c1 *= scale;
    sh.c2 *= scale;
    sh.c3 *= scale;
    return sh;
}

SH4RGB ProjectRadianceToSH4RGB(float3 radiance, float3 direction, float sampleWeight)
{
    direction = SafeNormalize(direction, float3(0.0f, 1.0f, 0.0f));
    radiance = max(SanitizeFloat3(radiance), 0.0f.xxx) * sampleWeight;

    float x = direction.x;
    float y = direction.y;
    float z = direction.z;

    SH4RGB sh;
    sh.c0 = radiance * 0.282095f;
    sh.c1 = radiance * (0.488603f * y);
    sh.c2 = radiance * (0.488603f * z);
    sh.c3 = radiance * (0.488603f * x);
    return sh;
}

float3x3 BuildTBN(float3 normal)
{
    static const float3 rvec1 = float3(0.847100675f, 0.207911700f, 0.489073813f);
    static const float3 rvec2 = float3(-0.639436305f, -0.390731126f, 0.662155867f);
    float3 rvec = dot(rvec1, normal) > 0.95f ? rvec2 : rvec1;
    float3 b1 = normalize(rvec - normal * dot(rvec, normal));
    float3 b2 = cross(normal, b1);
    return float3x3(b1, b2, normal);
}

float3 EvaluateSkyColor(float3 direction)
{
    float t = 0.5f * (direction.y + 1.0f);
    return lerp(SkyColorBottom, SkyColorTop, t) * SkyIntensity;
}

bool IsDirectLightVisible(float3 worldPos, float3 normal)
{
    float3 lightDir = normalize(LightDirAndIntensity.xyz);

    RayDesc shadowRay;
    shadowRay.Origin = worldPos + normal * RayBias;
    shadowRay.Direction = lightDir;
    shadowRay.TMin = 0.0f;
    shadowRay.TMax = MAX_HIT_DIST;

    ShadowRayPayload shadowPayload;
    shadowPayload.bHit = true;
    TraceRay(
        gRtScene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
            RAY_FLAG_FORCE_OPAQUE |
            RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        0xFF,
        0,
        0,
        1,
        shadowRay,
        shadowPayload);

    return !shadowPayload.bHit;
}

float3 EvaluateDirectSurfaceRadiance(float3 worldPos, float3 normal, float3 albedo)
{
    if (!IsDirectLightVisible(worldPos, normal))
        return 0.0f.xxx;

    float3 lightDir = normalize(LightDirAndIntensity.xyz);
    float nDotL = saturate(dot(normal, lightDir));
    return nDotL * LightDirAndIntensity.w * LightColor * max(albedo, 0.0f.xxx) * INV_PI;
}

float3 TraceDiffusePath(float3 origin, float3 direction, uint2 noiseCoord, uint sampleIndex)
{
    float3 radiance = 0.0f.xxx;
    float3 throughput = 1.0f.xxx;
    float3 rayOrigin = origin;
    float3 rayDirection = SafeNormalize(direction, float3(0.0f, 1.0f, 0.0f));
    uint bounceCount = clamp(MaxBounces, 1u, 8u);

    [loop]
    for (uint bounceIndex = 0u; bounceIndex < 8u; ++bounceIndex)
    {
        if (bounceIndex >= bounceCount)
            break;

        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = rayDirection;
        ray.TMin = 0.0f;
        ray.TMax = MAX_HIT_DIST;

        RayPayload payload;
        payload.position = 0.0f.xxx;
        payload.color = 0.0f.xxx;
        payload.normal = float3(0.0f, 1.0f, 0.0f);
        payload.spreadAngle = ViewSpreadAngle * max(CellSize, 1.0f);
        payload.coneWidth = 0.0f;
        payload.bHit = false;

        TraceRay(
            gRtScene,
            SPATIAL_HASH_GI_RAY_FLAGS,
            0xFF,
            0,
            0,
            0,
            ray,
            payload);

        if (!payload.bHit)
        {
            radiance += throughput * payload.color;
            break;
        }

        float3 hitNormal = SafeNormalize(payload.normal, float3(0.0f, 1.0f, 0.0f));
        float3 hitAlbedo = max(SanitizeFloat3(payload.color), 0.0f.xxx);
        radiance += throughput * EvaluateDirectSurfaceRadiance(payload.position, hitNormal, hitAlbedo);

        if (bounceIndex + 1u >= bounceCount)
            break;

        throughput *= hitAlbedo;
        float maxThroughput = max(max(throughput.x, throughput.y), throughput.z);
        if (maxThroughput < 1e-3f)
            break;

        uint2 bounceNoiseCoord = noiseCoord + uint2(37u * (bounceIndex + 1u), 53u * (sampleIndex + 1u));
        float2 randomUV = LoadRayNoise2(
            BlueNoiseTex,
            bounceNoiseCoord,
            FrameCounter + 19u * (bounceIndex + 1u) + 7u * sampleIndex,
            BlueNoiseOffsetStride,
            NoiseMode);

        float3 nextDirLocal = SampleHemisphereCosine(randomUV.x, randomUV.y);
        rayDirection = SafeNormalize(mul(nextDirLocal, BuildTBN(hitNormal)), hitNormal);
        rayOrigin = payload.position + hitNormal * RayBias;
    }

    return max(SanitizeFloat3(radiance), 0.0f.xxx);
}

[shader("raygeneration")]
void rayGen()
{
    uint slot = DispatchRaysIndex().x;
    if (slot >= HashEntryCount)
        return;

    uint key = CellKeys[slot];
    if (key == 0u)
    {
        TraceSH0[slot] = 0.0f.xxxx;
        TraceSH1[slot] = 0.0f.xxxx;
        TraceSH2[slot] = 0.0f.xxxx;
        TraceSH3[slot] = 0.0f.xxxx;
        return;
    }

    float4 cellPosition = CellPosition[slot];
    float4 cellNormal = CellNormal[slot];
    if (cellPosition.w <= 0.0f || cellNormal.w <= 0.0f)
    {
        TraceSH0[slot] = 0.0f.xxxx;
        TraceSH1[slot] = 0.0f.xxxx;
        TraceSH2[slot] = 0.0f.xxxx;
        TraceSH3[slot] = 0.0f.xxxx;
        return;
    }

    float3 worldNormal = SafeNormalize(cellNormal.xyz, float3(0.0f, 1.0f, 0.0f));
    float3 worldPos = cellPosition.xyz;
    float3 origin = worldPos + worldNormal * RayBias;
    uint rayCount = clamp(RaysPerCell, 1u, 8u);
    uint2 baseNoiseCoord = uint2(slot & 1023u, slot >> 10u);

    SH4RGB sh = InitSH4RGB();
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < 8u; ++sampleIndex)
    {
        if (sampleIndex >= rayCount)
            break;

        uint2 noiseCoord = baseNoiseCoord + uint2(sampleIndex * 17u, sampleIndex * 31u);
        float2 randomUV = LoadRayNoise2(
            BlueNoiseTex,
            noiseCoord,
            FrameCounter + sampleIndex * 13u,
            BlueNoiseOffsetStride,
            NoiseMode);

        float3 sampleDirLocal = SampleUniformHemisphere(randomUV.x, randomUV.y);
        float3 sampleDirWorld = SafeNormalize(mul(sampleDirLocal, BuildTBN(worldNormal)), worldNormal);
        float samplePdf = 1.0f / (2.0f * PI);
        float invPdf = rcp(max(samplePdf, 1e-4f));
        float3 sampleRadiance = TraceDiffusePath(origin, sampleDirWorld, noiseCoord, sampleIndex);
        AccumulateSH4RGB(sh, ProjectRadianceToSH4RGB(sampleRadiance, sampleDirWorld, invPdf), 1.0f);
    }

    sh = ScaleSH4RGB(sh, rcp(float(rayCount)));
    TraceSH0[slot] = float4(sh.c0, float(rayCount));
    TraceSH1[slot] = float4(sh.c1, 0.0f);
    TraceSH2[slot] = float4(sh.c2, 0.0f);
    TraceSH3[slot] = float4(sh.c3, 0.0f);
}

[shader("miss")]
void miss(inout RayPayload payload)
{
    payload.position = 0.0f.xxx;
    payload.color = EvaluateSkyColor(WorldRayDirection());
    payload.normal = float3(0.0f, 1.0f, 0.0f);
    payload.bHit = false;
}

[shader("closesthit")]
void chs(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();
    Vertex vertex = GetSurfaceVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);

    payload.position = vertex.position;
    payload.normal = SafeNormalize(vertex.normal, float3(0.0f, 1.0f, 0.0f));

    uint w, h;
    AlbedoTex.GetDimensions(w, h);
    float halfLog2NumTexPixels = 0.5f * log2(w * h);
    vertex.textureLODConstant += halfLog2NumTexPixels;
    float hitT = RayTCurrent();
    float rayConeWidth = payload.spreadAngle * hitT + payload.coneWidth;
    float mipLevel = computeTextureLOD(1.0f, rayConeWidth, vertex.textureLODConstant);

    payload.color = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, mipLevel).xyz;
    payload.bHit = true;
}

[shader("miss")]
void missShadow(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}
