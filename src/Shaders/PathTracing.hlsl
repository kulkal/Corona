#include "Common.hlsl"
#include "GGX.hlsli"

RWTexture2D<float4> OutputColor : register(u0);
RWTexture2D<float4> OutAlbedo : register(u1);
RWTexture2D<float4> OutSpecularAlbedo : register(u2);
RWTexture2D<float4> OutNormal : register(u3);
RWTexture2D<float4> OutGeomNormal : register(u4);
RWTexture2D<float2> OutVelocity : register(u5);
RWTexture2D<float4> OutRoughnessMetallic : register(u6);
RWTexture2D<float> OutDepth : register(u7);
RWTexture2D<float> OutSpecularHitDistance : register(u8);
RWTexture2D<float2> OutSpecularMotionVector : register(u9);

RaytracingAccelerationStructure gRtScene : register(t0);
ByteAddressBuffer vertices : register(t1);
ByteAddressBuffer indices : register(t2);
ByteAddressBuffer InstanceProperty : register(t3);
Texture2D AlbedoTex : register(t5);
Texture2D NormalTex : register(t6);
Texture2D RoughnessTex : register(t7);
Texture2D MetallicTex : register(t8);

#define MAX_POINT_LIGHTS 8

struct PointLightParam
{
    float4 PositionAndRadius;
    float4 ColorAndIntensity;
};

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
    float DirectLightAngularRadius;
    uint DirectLightSampleCount;
    float2 _directLightPadding;
    float2 RandomOffset;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    uint MaxBounces;
    uint SamplesPerPixel;
    float ViewSpreadAngle;
    uint DebugMode; // 0=None, 1=Albedo, 2=Normal, 3=Roughness, 4=Metallic, 5=WorldPos, 6=Barycentric, 7=UV, 8=InstanceID, 9=TriangleIndex
    float3 SkyColorTop;
    float SkyIntensity;
    float3 SkyColorBottom;
    float _padding;
    float3 LightColor;
    float _padding2;
    uint bEnableDiffuseGI;
    uint bEnableSpecularGI;
    uint bEnableDirectDiffuse;
    uint bEnableDirectSpecular;
    uint bEnableRTAO;
    uint bWritePrimaryGBuffer;
    float SpecularMotionVectorScale;
    uint bStabilizePrimaryRaySamples;
    uint _rtaoPadding;
    PointLightParam PointLights[MAX_POINT_LIGHTS];
    uint PointLightCount;
    float3 PointLightPadding;
};

SamplerState sampleWrap : register(s0);

static const float INV_PI = 1.0 / PI;
static const float PATH_TRACING_RAY_BIAS = 0.5f;

struct PathTracingPayload
{
    float3 radiance;
    float3 throughput;
    float3 origin;
    float3 direction;
    uint depth;
    uint seed;
    bool done;
    uint guideRay;
    uint hit;
    float hitDistance;
    
    // Debug visualization data (only for primary hit)
    float3 debugAlbedo;
    float3 debugNormal;
    float3 debugGeomNormal;
    float3 debugWorldPos;
    float debugRoughness;
    float debugMetallic;
    float3 debugBarycentric;
    float2 debugUV;
    uint debugInstanceID;
    uint debugTriangleIndex;
};

struct ShadowRayPayload
{
    bool bHit;
};

// Random number generation using PCG
uint pcg_hash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float random_float(inout uint seed)
{
    seed = pcg_hash(seed);
    return float(seed) / 4294967296.0;
}

uint init_path_seed(uint2 pixel, uint frameIndex, uint sampleIndex)
{
    uint seed = pixel.x * 0x9E3779B9u;
    seed ^= pixel.y * 0xBB67AE85u;
    seed ^= frameIndex * 0x3C6EF372u;
    seed ^= sampleIndex * 0xA54FF53Au;
    seed = pcg_hash(seed ^ 0x510E527Fu);
    seed ^= pcg_hash(seed + pixel.x + 0x1F83D9ABu);
    seed ^= pcg_hash(seed + pixel.y + 0x5BE0CD19u);
    return seed | 1u;
}

float3 clamp_firefly(float3 radiance)
{
    const float kMaxRadiance = 48.0f;
    float maxChannel = max(radiance.x, max(radiance.y, radiance.z));
    if (maxChannel > kMaxRadiance)
        radiance *= kMaxRadiance / maxChannel;
    return radiance;
}

PathTracingPayload MakePathTracingPayload(float3 rayOrigin, float3 rayDirection, uint depth, uint seed, float3 throughput, uint guideRay)
{
    PathTracingPayload payload;
    payload.radiance = float3(0, 0, 0);
    payload.throughput = throughput;
    payload.origin = rayOrigin;
    payload.direction = rayDirection;
    payload.depth = depth;
    payload.seed = seed;
    payload.done = false;
    payload.guideRay = guideRay;
    payload.hit = 0u;
    payload.hitDistance = ProjectionParams.w;
    payload.debugAlbedo = float3(0, 0, 0);
    payload.debugNormal = float3(0, 0, 0);
    payload.debugGeomNormal = float3(0, 0, 0);
    payload.debugWorldPos = float3(0, 0, 0);
    payload.debugRoughness = 0;
    payload.debugMetallic = 0;
    payload.debugBarycentric = float3(0, 0, 0);
    payload.debugUV = float2(0, 0);
    payload.debugInstanceID = 0;
    payload.debugTriangleIndex = 0;
    return payload;
}

float3 random_in_unit_sphere(inout uint seed)
{
    float z = random_float(seed) * 2.0 - 1.0;
    float a = random_float(seed) * 2.0 * PI;
    float r = sqrt(1.0 - z * z);
    float x = r * cos(a);
    float y = r * sin(a);
    return float3(x, y, z);
}

float3 random_cosine_direction(inout uint seed)
{
    float r1 = random_float(seed);
    float r2 = random_float(seed);
    float z = sqrt(1.0 - r2);
    
    float phi = 2.0 * PI * r1;
    float x = cos(phi) * sqrt(r2);
    float y = sin(phi) * sqrt(r2);
    
    return float3(x, y, z);
}

// Schlick approximation for Fresnel
float schlick_fresnel(float cosine, float ref_idx)
{
    float r0 = (1.0 - ref_idx) / (1.0 + ref_idx);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(1.0 - cosine, 5.0);
}

// Sample GGX distribution
float3 sample_ggx(float3 N, float roughness, inout uint seed)
{
    N = GGXSafeNormalize(N, float3(0.0f, 1.0f, 0.0f));
    float a = roughness * roughness;
    
    float r1 = random_float(seed);
    float r2 = random_float(seed);
    
    float phi = 2.0 * PI * r1;
    float cosTheta = sqrt((1.0 - r2) / (1.0 + (a * a - 1.0) * r2));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    
    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;
    
    float3x3 TBN = construct_ONB_frisvad(N);
    return GGXSafeNormalize(mul(H, TBN), N);
}

float ComputePathTracingTextureMipLevel(uint instanceID, Vertex vertex, float3 viewDir, float hitDistance)
{
    uint textureWidth = 1;
    uint textureHeight = 1;
    AlbedoTex.GetDimensions(textureWidth, textureHeight);

    float halfLog2NumTexPixels = 0.5f * log2(max(float(textureWidth) * float(textureHeight), 1.0f));
    float triangleLodConstant = vertex.textureLODConstant + halfLog2NumTexPixels;
    float3 normal = GGXSafeNormalize(vertex.normal, -viewDir);
    float3 V = GGXSafeNormalize(-viewDir, normal);
    float NoV = max(abs(dot(normal, V)), 1.0e-4f);
    float rayConeWidth = max(ViewSpreadAngle * max(hitDistance, 0.0f), 1.0e-6f);
    return computeTextureLOD(NoV, rayConeWidth, triangleLodConstant);
}

float3 ApplyPathTracingNormalMap(float3 vertexNormal, float3 vertexTangent, float2 uv, uint instanceID, float mipLevel)
{
    float3 N = GGXSafeNormalize(vertexNormal, float3(0.0f, 1.0f, 0.0f));
    float tangentLenSq = dot(vertexTangent, vertexTangent);
    if (any(isnan(vertexTangent)) || any(isinf(vertexTangent)) || tangentLenSq < 1e-8f)
        return N;

    float3 T = GGXSafeNormalize(vertexTangent, N);
    T = T - dot(T, N) * N;
    T = GGXSafeNormalize(T, float3(0.0f, 0.0f, 0.0f));
    if (dot(T, T) < 1e-8f)
        return N;

    float3 B = cross(N, T);
    B = GGXSafeNormalize(B, float3(0.0f, 0.0f, 0.0f));
    if (dot(B, B) < 1e-8f)
        return N;

    float3 normalMap = NormalTex.SampleLevel(sampleWrap, uv, mipLevel).xyz;
    if (any(isnan(normalMap)) || any(isinf(normalMap)))
        return N;
    normalMap = normalMap * 2.0f - 1.0f;

    return GGXSafeNormalize(mul(normalMap, float3x3(T, B, N)), N);
}

float ProjectToDeviceDepth(float3 worldPos, float4x4 viewProj)
{
    float4 clip = mul(float4(worldPos, 1.0f), viewProj);
    float invW = abs(clip.w) > 1.0e-6f ? rcp(clip.w) : 0.0f;
    float deviceDepth = clip.z * invW;
    return (isnan(deviceDepth) || isinf(deviceDepth)) ? 1.0f : deviceDepth;
}

float2 ProjectToScreenUV(float3 worldPos, float4x4 viewProj)
{
    float4 clip = mul(float4(worldPos, 1.0f), viewProj);
    float invW = abs(clip.w) > 1.0e-6f ? rcp(clip.w) : 0.0f;
    float2 uv = (clip.xy * invW) * float2(0.5f, -0.5f) + 0.5f;
    return (any(isnan(uv)) || any(isinf(uv))) ? float2(0.0f, 0.0f) : uv;
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

void WritePrimaryHitGBuffer(uint2 pixel, PathTracingPayload payload, bool bHit, float specularHitDistance, float2 specularMotionVector)
{
    if (!bHit)
    {
        OutAlbedo[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        OutSpecularAlbedo[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        OutNormal[pixel] = float4(0.0f, -0.1f, 0.0f, 0.0f);
        OutGeomNormal[pixel] = float4(0.0f, -0.1f, 0.0f, 0.0f);
        OutVelocity[pixel] = float2(0.0f, 0.0f);
        OutRoughnessMetallic[pixel] = float4(0.001f, 0.0f, 0.0f, 0.0f);
        OutDepth[pixel] = 1.0f;
        OutSpecularHitDistance[pixel] = ProjectionParams.w;
        OutSpecularMotionVector[pixel] = float2(0.0f, 0.0f);
        return;
    }

    float3 albedo = saturate(payload.debugAlbedo);
    float roughness = clamp(payload.debugRoughness, 0.01f, 1.0f);
    float metallic = saturate(payload.debugMetallic);
    float3 normal = GGXSafeNormalize(payload.debugNormal, float3(0.0f, 1.0f, 0.0f));
    float3 geomNormal = GGXSafeNormalize(payload.debugGeomNormal, normal);
    float3 surfaceToView = GGXSafeNormalize(-payload.direction, normal);

    OutAlbedo[pixel] = float4(albedo, 1.0f);
    OutSpecularAlbedo[pixel] = float4(ComputeDLSSRRSpecularAlbedo(albedo, metallic, roughness, normal, surfaceToView), 1.0f);
    OutNormal[pixel] = float4(normal, 0.0f);
    OutGeomNormal[pixel] = float4(geomNormal, 0.0f);
    // PT+RR asks Streamline to rebuild camera motion from depth and clipToPrevClip.
    OutVelocity[pixel] = float2(0.0f, 0.0f);
    OutRoughnessMetallic[pixel] = float4(roughness, metallic, 0.0f, 0.0f);
    OutDepth[pixel] = ProjectToDeviceDepth(payload.debugWorldPos, UnjitteredViewProjMatrix);
    OutSpecularHitDistance[pixel] = clamp(specularHitDistance, 0.0f, ProjectionParams.w);
    OutSpecularMotionVector[pixel] = specularMotionVector;
}

[shader("raygeneration")]
void PathTracingRayGen()
{
    uint3 launchIndex = DispatchRaysIndex();
    uint3 launchDim = DispatchRaysDimensions();
    
    float2 pixelCenter = float2(launchIndex.xy) + float2(0.5, 0.5);
    float2 inUV = pixelCenter / float2(launchDim.xy);
    
    uint frameCounter = (DebugMode > 0) ? 0 : BlueNoiseOffsetStride;
    uint sampleCount = (DebugMode > 0) ? 1u : clamp(SamplesPerPixel, 1u, 16u);
    float3 radianceSum = float3(0, 0, 0);

    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        uint seed = init_path_seed(launchIndex.xy, frameCounter, sampleIndex);
        float2 clipXY = inUV * 2.0f - 1.0f;

        // Keep the RR guiding GBuffer deterministic. During camera motion, lock
        // all primary samples to the same pixel center so color/depth/normal
        // describe the same primary surface for RR reprojection.
        bool bCenterPrimaryRay =
            (bWritePrimaryGBuffer != 0) &&
            (sampleIndex == 0 || bStabilizePrimaryRaySamples != 0);
        if (DebugMode == 0 && !bCenterPrimaryRay)
        {
            float2 jitter = float2(random_float(seed), random_float(seed)) - 0.5;
            clipXY += (2.0f * jitter) / float2(launchDim.xy);
        }

        clipXY.y = -clipXY.y;
        float4 viewFarH = mul(float4(clipXY, 1.0f, 1.0f), InvProjMatrix);
        float invViewFarW = abs(viewFarH.w) > 1.0e-6f ? rcp(viewFarH.w) : 1.0f;
        float3 viewRayDir = viewFarH.xyz * invViewFarW;
        if (any(isnan(viewRayDir)) || any(isinf(viewRayDir)) || dot(viewRayDir, viewRayDir) < 1.0e-8f)
            viewRayDir = float3(0.0f, 0.0f, -1.0f);
        else
            viewRayDir = normalize(viewRayDir);
        float primaryRayTScale = rcp(max(-viewRayDir.z, 1.0e-4f));
        float primaryRayTMin = max(0.0001f, ProjectionParams.z * primaryRayTScale);
        float primaryRayTMax = max(primaryRayTMin + 0.0001f, ProjectionParams.w * primaryRayTScale);
        float3 rayDir = normalize(mul(float4(viewRayDir, 0), InvViewMatrix).xyz);

        float3 rayOrigin = mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
        float3 radiance = float3(0, 0, 0);
        float3 throughput = float3(1, 1, 1);

        for (uint bounce = 0; bounce < MaxBounces; bounce++)
        {
            RayDesc ray;
            ray.Origin = rayOrigin;
            ray.Direction = rayDir;
            ray.TMin = bounce == 0 ? primaryRayTMin : 0.0001f;
            ray.TMax = bounce == 0 ? primaryRayTMax : 100000.0f;
            
            PathTracingPayload payload = MakePathTracingPayload(rayOrigin, rayDir, bounce, seed, throughput, 0u);
            
            TraceRay(gRtScene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
            
            seed = payload.seed;

            if (bWritePrimaryGBuffer != 0 && sampleIndex == 0 && bounce == 0)
            {
                float specularHitDistance = ProjectionParams.w;
                float2 specularMotionVector = float2(0.0f, 0.0f);
                bool primaryHit = payload.hit != 0u;
                if (primaryHit)
                {
                    float roughness = clamp(payload.debugRoughness, 0.02f, 1.0f);
                    float3 specularAlbedo = lerp(0.04f.xxx, saturate(payload.debugAlbedo), saturate(payload.debugMetallic));
                    float specularEnergy = max(specularAlbedo.x, max(specularAlbedo.y, specularAlbedo.z));
                    float smoothGuide = saturate((0.38f - roughness) / 0.18f);
                    float metalGuide = saturate(payload.debugMetallic) * saturate((0.55f - roughness) / 0.25f);
                    float specularGuideWeight = specularEnergy * max(smoothGuide, metalGuide);

                    if (specularGuideWeight > 0.025f)
                    {
                        float3 guideNormal = GGXSafeNormalize(payload.debugGeomNormal, float3(0.0f, 1.0f, 0.0f));
                        float3 guideDir = GGXSafeNormalize(reflect(ray.Direction, guideNormal), guideNormal);

                        if (dot(guideDir, guideNormal) > 1.0e-4f)
                        {
                            RayDesc guideRay;
                            guideRay.Origin = payload.debugWorldPos + guideNormal * PATH_TRACING_RAY_BIAS;
                            guideRay.Direction = guideDir;
                            guideRay.TMin = 0.01f;
                            guideRay.TMax = ProjectionParams.w;

                            PathTracingPayload guidePayload = MakePathTracingPayload(guideRay.Origin, guideRay.Direction, 0u, seed, float3(0, 0, 0), 1u);
                            guidePayload.done = true;
                            TraceRay(gRtScene,
                                     RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                                     RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
                                     0xFF, 0, 0, 0, guideRay, guidePayload);
                            if (guidePayload.hit != 0u)
                            {
                                specularHitDistance = guidePayload.hitDistance;
                                float2 specCurrentUV;
                                float2 specPrevUV;
                                if (ProjectToScreenUVChecked(guidePayload.debugWorldPos, UnjitteredViewProjMatrix, specCurrentUV) &&
                                    ProjectToScreenUVChecked(guidePayload.debugWorldPos, PrevUnjitteredViewProjMatrix, specPrevUV))
                                {
                                    float2 candidateMotionVector = (specPrevUV - specCurrentUV) * float2(launchDim.xy) * SpecularMotionVectorScale;
                                    if (all(abs(candidateMotionVector) <= float2(launchDim.xy)))
                                        specularMotionVector = candidateMotionVector;
                                }
                            }
                        }
                    }
                }
                WritePrimaryHitGBuffer(launchIndex.xy, payload, primaryHit, specularHitDistance, specularMotionVector);
            }
            
            if (DebugMode > 0 && bounce == 0)
            {
                if (payload.hit != 0u)
                {
                    if (DebugMode == 1)
                        radiance = payload.debugAlbedo;
                    else if (DebugMode == 2)
                        radiance = payload.debugNormal * 0.5 + 0.5;
                    else if (DebugMode == 3)
                        radiance = float3(payload.debugRoughness, payload.debugRoughness, payload.debugRoughness);
                    else if (DebugMode == 4)
                        radiance = float3(payload.debugMetallic, payload.debugMetallic, payload.debugMetallic);
                    else if (DebugMode == 5)
                    {
                        radiance = frac(payload.debugWorldPos * 0.1);
                    }
                    else if (DebugMode == 6)
                    {
                        radiance = payload.debugBarycentric;
                    }
                    else if (DebugMode == 7)
                    {
                        radiance = float3(payload.debugUV, 0);
                    }
                    else if (DebugMode == 8)
                    {
                        uint id = payload.debugInstanceID % 8;
                        float3 colors[8] = {
                            float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), float3(1, 1, 0),
                            float3(1, 0, 1), float3(0, 1, 1), float3(1, 1, 1), float3(0.5, 0.5, 0.5)
                        };
                        radiance = colors[id];
                    }
                    else if (DebugMode == 9)
                    {
                        float t = frac(float(payload.debugTriangleIndex) * 0.01);
                        radiance = float3(t, t, t);
                    }
                }
                else
                {
                    radiance = float3(0.5, 0.5, 0.5);
                }
                
                break;
            }
            
            radiance += payload.radiance * throughput;
            
            if (payload.done)
            {
                break;
            }
            
            rayOrigin = payload.origin;
            rayDir = payload.direction;
            throughput = payload.throughput;
            
            if (any(isnan(throughput)) || any(isinf(throughput)) || all(throughput <= 0.0))
            {
                break;
            }
            
            if (bounce > 2)
            {
                float p = max(throughput.x, max(throughput.y, throughput.z));
                
                if (p < 0.001)
                    break;
                    
                if (random_float(seed) > p)
                    break;
                    
                throughput /= max(p, 0.001);
            }
        }
        
        if (any(isnan(radiance)) || any(isinf(radiance)))
        {
            radiance = float3(0, 0, 0);
        }
        if (DebugMode == 0)
        {
            radiance = clamp_firefly(max(radiance, 0.0f.xxx));
        }
        radianceSum += radiance;
    }
    
    float3 radiance = radianceSum / float(sampleCount);
    
    // Validate radiance (check for NaN/Inf)
    if (any(isnan(radiance)) || any(isinf(radiance)))
    {
        radiance = float3(0, 0, 0);
    }
    
    // Accumulate with previous frames
    float3 finalColor;
    
    // In debug mode, don't accumulate - just show current frame
    if (FrameCounter == 0 || DebugMode > 0)
    {
        // First frame or debug mode - use only current radiance
        finalColor = radiance;
    }
    else
    {
        // Progressive accumulation: (prev * N + current) / (N + 1)
        float3 prevColor = OutputColor[launchIndex.xy].xyz;
        
        // Validate previous color
        if (any(isnan(prevColor)) || any(isinf(prevColor)))
        {
            prevColor = float3(0, 0, 0);
        }
        
        float n = float(FrameCounter);
        finalColor = (prevColor * n + radiance) / (n + 1.0);
    }
    
    // Final validation
    if (any(isnan(finalColor)) || any(isinf(finalColor)))
    {
        finalColor = float3(0, 0, 0);
    }
    
    OutputColor[launchIndex.xy] = float4(finalColor, 1.0);
}

[shader("closesthit")]
void PathTracingClosestHit(inout PathTracingPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    if (payload.guideRay != 0)
    {
        payload.hitDistance = RayTCurrent();
        payload.debugWorldPos = payload.origin + payload.direction * payload.hitDistance;
        payload.hit = 1u;
        payload.done = false;
        return;
    }

    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, 
                                  attribs.barycentrics.x, 
                                  attribs.barycentrics.y);
    
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();
    Vertex vertex = GetVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);
    float hitDistance = RayTCurrent();
    float textureMipLevel = ComputePathTracingTextureMipLevel(instanceID, vertex, payload.direction, hitDistance);
    payload.hit = 1u;
    payload.hitDistance = hitDistance;
    
    // Get material properties
    float3 albedo = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, textureMipLevel).xyz;
    float roughness = clamp(RoughnessTex.SampleLevel(sampleWrap, vertex.uv, textureMipLevel).x, 0.02f, 1.0f);
    float metallic = saturate(MetallicTex.SampleLevel(sampleWrap, vertex.uv, textureMipLevel).x);
    ApplyInstanceRoughnessMetallic(instanceID, InstanceProperty, roughness, metallic);
    
    // Store debug information for primary hit (depth == 0)
    if (payload.depth == 0)
    {
        payload.debugAlbedo = albedo;
        payload.debugRoughness = roughness;
        payload.debugMetallic = metallic;
        payload.debugWorldPos = vertex.position;
        // Store barycentric coordinates directly (R=v0 weight, G=v1 weight, B=v2 weight)
        payload.debugBarycentric = barycentrics;
        payload.debugUV = vertex.uv;
        payload.debugInstanceID = instanceID;
        payload.debugTriangleIndex = triangleIndex;
    }
    
    // Sample normal map when a valid tangent basis exists. Models without UVs
    // such as buddha.obj have no tangents, so fall back to the geometric normal.
    float3 N = ApplyPathTracingNormalMap(vertex.normal, vertex.tangent, vertex.uv, instanceID, textureMipLevel);
    float3 V = GGXSafeNormalize(-payload.direction, N);
    float3 geomNormal = GGXSafeNormalize(vertex.normal, N);
    // Path tracing hits both sides of imported meshes. Keep the shading normal
    // on the visible side so inverted/two-sided OBJ normals do not trap paths.
    if (dot(N, V) < 0.0f)
        N = -N;
    if (dot(geomNormal, V) < 0.0f)
        geomNormal = -geomNormal;
    
    // Store final normal for debug visualization
    if (payload.depth == 0)
    {
        payload.debugNormal = N;
        payload.debugGeomNormal = geomNormal;
    }
    
    // Offset ray origin to avoid self-intersection
    // Use larger offset for path tracing to prevent shadow acne
    float3 hitPos = vertex.position + N * PATH_TRACING_RAY_BIAS;
    
    // Direct lighting - sample the sun as a small spherical cap.
    float3 baseLightDir = GGXSafeNormalize(LightDirAndIntensity.xyz, float3(0.0f, 1.0f, 0.0f));
    float lightIntensity = LightDirAndIntensity.w;
    
    float3 directLight = float3(0, 0, 0);
    float3 directF0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    const uint kMaxDirectLightSamples = 8;
    uint directLightSampleCount = min(max(DirectLightSampleCount, 1u), kMaxDirectLightSamples);

    [loop]
    for (uint lightSampleIndex = 0u; lightSampleIndex < kMaxDirectLightSamples; ++lightSampleIndex)
    {
        if (lightSampleIndex >= directLightSampleCount)
            break;

        float2 lightUV = float2(random_float(payload.seed), random_float(payload.seed));
        float3 lightDir = SampleDirectionalLightSphereCap(baseLightDir, DirectLightAngularRadius, lightUV);

        // Shadow ray
        RayDesc shadowRay;
        shadowRay.Origin = hitPos;
        shadowRay.Direction = lightDir;
        shadowRay.TMin = 0.01;
        shadowRay.TMax = 100000;

        ShadowRayPayload shadowPayload;
        shadowPayload.bHit = true;
        TraceRay(gRtScene,
                 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
                 RAY_FLAG_FORCE_OPAQUE |
                 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
                 0xFF, 0, 0, 1, shadowRay, shadowPayload);

        if (!shadowPayload.bHit)
        {
            float NdotL = max(0, dot(N, lightDir));

            if (NdotL > 0)
            {
                // Match the hybrid lighting pass: direct diffuse is evaluated as
                // irradiance * albedo, without the Lambertian 1/pi normalization.
                float3 diffuse = bEnableDirectDiffuse ? (albedo * (1.0 - metallic)) : float3(0, 0, 0);

                // Match the raster lighting pass: direct specular uses the shared GGX BRDF.
                float3 specular = bEnableDirectSpecular ? EvaluateGGXSpecularBRDF(N, V, lightDir, roughness, directF0) : float3(0, 0, 0);

                // Combine diffuse and specular with light color
                directLight += (diffuse + specular) * NdotL * lightIntensity * LightColor;
            }
        }
    }

    directLight /= float(directLightSampleCount);

    uint activePointLightCount = min(PointLightCount, MAX_POINT_LIGHTS);
    [loop]
    for (uint pointLightIndex = 0u; pointLightIndex < MAX_POINT_LIGHTS; ++pointLightIndex)
    {
        if (pointLightIndex >= activePointLightCount)
            break;

        float3 pointPosition = PointLights[pointLightIndex].PositionAndRadius.xyz;
        float pointRadius = max(PointLights[pointLightIndex].PositionAndRadius.w, 0.01f);
        float3 pointColor = max(PointLights[pointLightIndex].ColorAndIntensity.xyz, 0.0f.xxx);
        float pointIntensity = max(PointLights[pointLightIndex].ColorAndIntensity.w, 0.0f);

        float3 toLight = pointPosition - hitPos;
        float distanceSq = max(dot(toLight, toLight), 1.0e-4f);
        float distanceToLight = sqrt(distanceSq);
        float3 pointLightDir = toLight / distanceToLight;
        float rangeAttenuation = saturate(1.0f - distanceToLight / pointRadius);
        rangeAttenuation *= rangeAttenuation;
        float inverseSquareAttenuation = 1.0f / max(1.0f, distanceSq * 0.0001f);
        float attenuation = rangeAttenuation * inverseSquareAttenuation;
        float pointNdotL = max(0.0f, dot(N, pointLightDir));

        if (pointNdotL <= 0.0f || attenuation <= 0.0f || pointIntensity <= 0.0f)
            continue;

        RayDesc pointShadowRay;
        pointShadowRay.Origin = hitPos;
        pointShadowRay.Direction = pointLightDir;
        pointShadowRay.TMin = 0.01f;
        pointShadowRay.TMax = max(distanceToLight - 0.02f, 0.01f);

        ShadowRayPayload pointShadowPayload;
        pointShadowPayload.bHit = true;
        TraceRay(gRtScene,
                 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
                 RAY_FLAG_FORCE_OPAQUE |
                 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
                 0xFF, 0, 0, 1, pointShadowRay, pointShadowPayload);

        if (!pointShadowPayload.bHit)
        {
            float3 pointRadiance = pointColor * pointIntensity * attenuation;
            float3 pointDiffuse = bEnableDirectDiffuse ? (albedo * (1.0 - metallic)) : float3(0, 0, 0);
            float3 pointSpecular = bEnableDirectSpecular ? EvaluateGGXSpecularBRDF(N, V, pointLightDir, roughness, directF0) : float3(0, 0, 0);
            directLight += (pointDiffuse + pointSpecular) * pointNdotL * pointRadiance;
        }
    }
    
    // Set direct lighting contribution
    payload.radiance = directLight;
    
    // Indirect lighting - sample BRDF
    float3 newDir;
    
    // Calculate Fresnel for view direction
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float NdotV = max(0, dot(N, V));
    float3 F = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);
    
    // Choose between diffuse and specular based on Fresnel, then normalize the
    // branch probabilities so the chosen lobe is properly weighted.
    float specularWeight = (F.x + F.y + F.z) / 3.0;
    specularWeight *= (1.0 - roughness * 0.5);
    specularWeight = (bEnableSpecularGI != 0) ? saturate(specularWeight) : 0.0f;
    float diffuseWeight = (bEnableDiffuseGI != 0) ? saturate(1.0 - specularWeight) : 0.0f;
    float totalWeight = specularWeight + diffuseWeight;
    if (totalWeight <= 1e-4f)
    {
        payload.throughput = float3(0, 0, 0);
        payload.done = true;
        return;
    }
    specularWeight /= totalWeight;
    diffuseWeight /= totalWeight;
    
    float r = random_float(payload.seed);
    
    if (r < specularWeight)
    {
        // Specular bounce
        float3 H = sample_ggx(N, roughness, payload.seed);
        newDir = GGXSafeNormalize(reflect(-V, H), N);
        
        // Make sure we're above the surface
        if (dot(newDir, N) < 0)
            newDir = GGXSafeNormalize(reflect(newDir, N), N);
        
        // Compensate for the stochastic lobe selection probability. This is not
        // full MIS yet, but it removes a major dark bias from the path tracer.
        payload.throughput *= F / max(specularWeight, 1e-4);
    }
    else
    {
        // Diffuse bounce - cosine weighted sampling
        float3 localDir = random_cosine_direction(payload.seed);
        float3x3 TBN = construct_ONB_frisvad(N);
        newDir = GGXSafeNormalize(mul(localDir, TBN), N);
        
        // Lambertian BRDF under cosine-weighted sampling reduces to albedo, but
        // we still need to divide by the branch probability.
        payload.throughput *= (albedo * (1.0 - metallic)) / max(diffuseWeight, 1e-4);
    }
    
    payload.origin = hitPos;
    payload.direction = newDir;
    payload.done = false;
}

[shader("miss")]
void PathTracingMiss(inout PathTracingPayload payload)
{
    if (payload.guideRay != 0)
    {
        payload.hitDistance = ProjectionParams.w;
        payload.done = true;
        return;
    }

    // Sky color - gradient based on view direction
    float t = 0.5 * (payload.direction.y + 1.0);
    float3 skyColor = lerp(SkyColorBottom, SkyColorTop, t);
    
    payload.radiance = skyColor * SkyIntensity;
    payload.done = true;
}

[shader("miss")]
void ShadowMiss(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}

[shader("anyhit")]
void PathTracingAnyHit(inout PathTracingPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, 
                                  attribs.barycentrics.x, 
                                  attribs.barycentrics.y);
    
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();

    if (!IsAlphaTestedInstance(instanceID, InstanceProperty))
        return;

    Vertex vertex = GetVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);
    float textureMipLevel = ComputePathTracingTextureMipLevel(instanceID, vertex, payload.direction, RayTCurrent());
    
    // Sample albedo alpha channel
    float alpha = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, textureMipLevel).w;
    
    // If alpha is too low, ignore this hit and continue ray traversal
    if (alpha < 0.1)
    {
        IgnoreHit();
    }
}
