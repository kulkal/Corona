#include "Common.hlsl"
#include "rtxgi/ddgi/ProbeCommon.hlsl"
#include "rtxgi/ddgi/Irradiance.hlsl"

RWTexture2D<float4> DDGIProbeRTRadiance : register(u0);
RWTexture2D<uint> DDGIProbeStates : register(u1);
RWTexture2D<float4> DDGIProbeOffsets : register(u2);

RaytracingAccelerationStructure SceneBVH : register(t0);

Texture2D DDGIProbeIrradianceSRV : register(t1);
Texture2D DDGIProbeDistanceSRV : register(t2);
// Texture2D DDGIProbeStates : register(t3);
// Texture2D DDGIProbeOffsets : register(t4);
Texture3D BlueNoiseTex : register(t5);


ByteAddressBuffer vertices : register(t10);
ByteAddressBuffer indices : register(t11);
ByteAddressBuffer InstanceProperty : register(t12);
Texture2D AlbedoTex : register(t13);


// cbuffer DDGIVolume : register(b0)
// {
//     float3      origin;
//     int         numRaysPerProbe;
//     float3      probeGridSpacing;
//     float       probeMaxRayDistance;
//     int3        probeGridCounts;
//     float       probeDistanceExponent;
//     float       probeHysteresis;
//     float       probeChangeThreshold;
//     float       probeBrightnessThreshold;
//     float       probeVariablePad0;
//     float       probeIrradianceEncodingGamma;
//     float       probeInverseIrradianceEncodingGamma;
//     int         probeNumIrradianceTexels;
//     int         probeNumDistanceTexels;
//     float       normalBias;
//     float       viewBias;
//     float2      probeVariablePad1;
//     float4x4    probeRayRotationTransform;      // 160B

// #if !RTXGI_DDGI_PROBE_RELOCATION && !RTXGI_DDGI_PROBE_STATE_CLASSIFIER
//     float4      padding[6];                     // 160B + 96B = 256B
// #elif !RTXGI_DDGI_PROBE_RELOCATION && RTXGI_DDGI_PROBE_STATE_CLASSIFIER
//     float       probeBackfaceThreshold;         // 164B
//     float3      padding;                        // 176B
//     float4      padding1[5];                    // 176B + 80B = 256B
// #elif RTXGI_DDGI_PROBE_RELOCATION /* && (RTXGI_DDGI_PROBE_STATE_CLASSIFIER || !RTXGI_DDGI_PROBE_STATE_CLASSIFIER) */
//     float       probeBackfaceThreshold;         // 164B
//     float       probeMinFrontfaceDistance;      // 168B
//     float2      padding;                        // 176B
//     float4      padding1[5];                    // 176B + 80B = 256B
// #endif
// };

ConstantBuffer<DDGIVolumeDescGPU> DDGIVolume    : register(b0);

cbuffer LightInfoCB : register(b1)
{
    float4 LightDirAndIntensity;
}


SamplerState TrilinearSampler : register(s0);



struct RayPayload
{
    float3 position;
    float3 color;
    float3 normal;
    bool bHit;
    float hitT;
};


struct ShadowRayPayload
{
    bool bHit;
};



#define RTXGI_DDGI_COMPUTE_IRRADIANCE_RECURSIVE 1

[shader("raygeneration")]
void rayGen
()
{
    float4 result = 0.f;

    uint2 DispatchIndex = DispatchRaysIndex().xy;
    int rayIndex = DispatchIndex.x;                    // index of ray within a probe
    int probeIndex = DispatchIndex.y;                  // index of current probe

#if RTXGI_DDGI_PROBE_STATE_CLASSIFIER
    int2 texelPosition = DDGIGetProbeTexelPosition(probeIndex, DDGIVolume.probeGridCounts);
    int  probeState = DDGIProbeStates[texelPosition];
    if (probeState == PROBE_STATE_INACTIVE)
    {
       return;  // if the probe is inactive, do not shoot rays
    }
#endif

 #if RTXGI_DDGI_PROBE_RELOCATION
     float3 probeWorldPosition = DDGIGetProbeWorldPositionWithOffset(probeIndex, DDGIVolume.origin, DDGIVolume.probeGridCounts, DDGIVolume.probeGridSpacing, DDGIProbeOffsets);
 #else
    float3 probeWorldPosition = DDGIGetProbeWorldPosition(probeIndex, DDGIVolume.origin, DDGIVolume.probeGridCounts, DDGIVolume.probeGridSpacing);
 #endif

    
    probeWorldPosition = DDGIGetProbeWorldPosition(probeIndex, DDGIVolume.origin, DDGIVolume.probeGridCounts, DDGIVolume.probeGridSpacing);


    float3 probeRayDirection = DDGIGetProbeRayDirection(rayIndex, DDGIVolume.numRaysPerProbe, DDGIVolume.probeRayRotationTransform);


   
    float cosTerm = 1;//dot(float3(0, 0, 1), sampleDirLocal)*2;

    float3 LightIntensity = LightDirAndIntensity.w;

    float3 Irradiance = 0..xxxx;
    RayDesc ray;
    ray.Origin = probeWorldPosition;
    ray.Direction = probeRayDirection;

    ray.TMin = 0;
    ray.TMax = 100000;

    RayPayload payload;

    TraceRay(SceneBVH, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, ray, payload);
    if(payload.bHit == false)
    {
        // hit sky
        float3 Radiance = payload.color * LightIntensity;
        // float3 Radiance = float3(1, 0, 0) * LightIntensity;

        Irradiance = Radiance * cosTerm;

        result = float4(Irradiance, 1e27f);
        DDGIProbeRTRadiance[DispatchIndex.xy] = result;
        return;
    }
    else
    {
        float3 LightDir = LightDirAndIntensity.xyz;
        RayDesc shadowRay;
        shadowRay.Origin = payload.position + payload.normal *0.5;
        shadowRay.Direction = LightDir;

        shadowRay.TMin = 0;
        shadowRay.TMax = 100000;

        ShadowRayPayload shadowPayload;
        shadowPayload.bHit = true;
        TraceRay(SceneBVH, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 1, shadowRay, shadowPayload);

        float3 Albedo = payload.color;
        if(shadowPayload.bHit == false)
        {
            // miss
            Irradiance = dot(LightDir.xyz, payload.normal) * LightIntensity  * Albedo;
        }
        else
        {
            // shadowed
        }

        // Irradiance += float3(0.01, 0.01, 0.01);

    }

    float3 RecursiveIrradiance = 0..xxx;
    //
#if RTXGI_DDGI_COMPUTE_IRRADIANCE_RECURSIVE
    float3 surfaceBias = DDGIGetSurfaceBias(payload.normal, ray.Direction, DDGIVolume);

    DDGIVolumeResources resources;
    resources.probeIrradianceSRV = DDGIProbeIrradianceSRV;
    resources.probeDistanceSRV = DDGIProbeDistanceSRV;
    resources.trilinearSampler = TrilinearSampler;
#if RTXGI_DDGI_PROBE_RELOCATION
    resources.probeOffsets = DDGIProbeOffsets;
#endif
#if RTXGI_DDGI_PROBE_STATE_CLASSIFIER
    resources.probeStates = DDGIProbeStates;
#endif
    float volumeBlendWeight = DDGIGetVolumeBlendWeight(payload.position, DDGIVolume);
    if (volumeBlendWeight > 0)
    {
        // Get irradiance from the DDGIVolume
        RecursiveIrradiance = DDGIGetVolumeIrradiance(
            payload.position,
            surfaceBias,
            payload.normal,
            DDGIVolume,
            resources);

        // Attenuate irradiance by the blend weight
        RecursiveIrradiance *= volumeBlendWeight;
    }
#endif

    //
    result = float4(Irradiance + payload.color/PI * RecursiveIrradiance, payload.hitT);

    DDGIProbeRTRadiance[DispatchIndex.xy] = result;

}

[shader("miss")]
void miss(inout RayPayload payload)
{
    payload.position = float3(0, 0, 0);
    payload.color = float3(0.0, 0.2, 0.4);
    payload.color = float3(0.0, 0.0, 0.0);

    payload.normal = float3(0, 0, -1);
    payload.bHit = false;
}

[shader("closesthit")]
void chs(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    Vertex vertex = GetVertexAttributes(InstanceID(), vertices, indices, InstanceProperty, triangleIndex, barycentrics);

    payload.position = vertex.position;
    payload.normal = vertex.normal;
    payload.color = AlbedoTex.SampleLevel(TrilinearSampler, vertex.uv, 0).xyz;

    payload.bHit = true;

    bool isBackFace = dot(payload.normal, WorldRayDirection()) > 0.f;
    if(isBackFace)
        payload.hitT = -RayTCurrent() * 0.2;
    else
        payload.hitT = RayTCurrent();
}

[shader("miss")]
void missShadow(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}

