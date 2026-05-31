#include "Common.hlsl"

RWTexture2D<float4> GIResultSH : register(u0);
RWTexture2D<float4> GIResultColor : register(u1);


RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
Texture3D RayNoiseBlueNoiseSource : register(t7);
ByteAddressBuffer vertices : register(t3);
ByteAddressBuffer indices : register(t4);
Texture2D AlbedoTex : register(t5);
ByteAddressBuffer InstanceProperty : register(t6);


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
    uint bIncludeSkyLighting;
    float NoisePadding;
    float3 SkyColorTop;
    float SkyIntensity;
    float3 SkyColorBottom;
    float _padding;
    float3 LightColor;
    float _padding2;
};

SamplerState sampleWrap : register(s0);

static const float INV_PI = 1.0 / PI;
static const float MAX_HIT_DIST = 10000;

#define RT_GI_SURFACE_RAY_FLAGS (RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES)

float3 EvaluateSkyColor(float3 direction)
{
    float t = 0.5 * (direction.y + 1.0);
    return max(lerp(SkyColorBottom, SkyColorTop, t) * SkyIntensity, 0.0f.xxx);
}

float3 EvaluateSkyDiffuseBounce(float3 normal)
{
    float3 averageSky = 0.5f * (SkyColorTop + SkyColorBottom);
    float3 skyGradient = 0.5f * (SkyColorTop - SkyColorBottom);
    return (averageSky + (2.0f / 3.0f) * skyGradient * normal.y) * SkyIntensity;
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
    float spreadAngle RT_DIFFUSE_GI_PAYLOAD_RW;
    float coneWidth RT_DIFFUSE_GI_PAYLOAD_RW;
    bool bHit RT_DIFFUSE_GI_PAYLOAD_RW;
};


struct RT_DIFFUSE_GI_RAY_PAYLOAD ShadowRayPayload
{
    bool bHit RT_DIFFUSE_GI_SHADOW_PAYLOAD_RW;
};

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
    return p + n * (1.0f / 256.0f);
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


    float2 crd = float2(launchIndex.xy);
	//crd.y *= -1;
    float2 dims = float2(launchDim.xy);

    float2 d = ((crd / dims) * 2.f - 1.f);
    d *= tan(0.8 / 2);
    float aspectRatio = dims.x / dims.y;


	float2 UV = crd / dims;
	float DeviceDepth = DepthTex.SampleLevel(sampleWrap, UV, 0).x;

	float3 WorldNormal = CommonSafeNormalize(WorldNormalTex.SampleLevel(sampleWrap, UV, 0).xyz, float3(0.0f, 1.0f, 0.0f));
  

    float LinearDepth = GetLinearDepthOpenGL(DeviceDepth, ProjectionParams.z, ProjectionParams.w) ;
	
    float2 ScreenPosition = crd.xy;
	ScreenPosition.x /= dims.x;
	ScreenPosition.y /= dims.y;
	ScreenPosition.xy = ScreenPosition.xy * 2 - 1;
	ScreenPosition.y = -ScreenPosition.y;

	// float3 ViewPosition = GetViewPosition(LinearDepth, ScreenPosition, ProjMatrix._11, ProjMatrix._22);
    float3 ViewPosition = GetViewPosition(DeviceDepth, ScreenPosition, InvProjMatrix);
    // 
	float3 WorldPos = mul(float4(ViewPosition, 1), InvViewMatrix).xyz;

  
    float rand_u = random(crd + RandomOffset);
    float rand_v = random(crd + RandomOffset + float2(100, 100));

    float2 RandomUV = GenerateRaySample2D(RayNoiseBlueNoiseSource, launchIndex.xy, FrameCounter, BlueNoiseOffsetStride, NoiseMode);

    float LightIntensity = max(CommonSanitizeFloat(LightDirAndIntensity.w, 0.0f), 0.0f);

    float3 viewRay = CommonSafeNormalize(float3(d.x * aspectRatio, -d.y, -1.0f), float3(0.0f, 0.0f, -1.0f));
    float3 ViewDir = CommonSafeNormalize(mul(float4(viewRay, 0.0f), InvViewMatrix).xyz, -WorldNormal);
    if (dot(WorldNormal, -ViewDir) < 0.0f)
        WorldNormal = -WorldNormal;

    float3 sampleDirLocal = SampleHemisphereCosine(RandomUV.x, RandomUV.y);
    float3x3 tbn = buildTBN(WorldNormal);
    float3 sampleDirWorld = CommonSafeNormalize(mul(sampleDirLocal, tbn), WorldNormal);

    // https://computergraphics.stackexchange.com/questions/4664/does-cosine-weighted-hemisphere-sampling-still-require-ndotl-when-calculating-co
    // https://computergraphics.stackexchange.com/questions/8578/how-to-set-equivalent-pdfs-for-cosine-weighted-and-uniform-sampled-hemispheres
    float cosTerm = 1;//dot(float3(0, 0, 1), sampleDirLocal)*2;

	RayDesc ray;
	ray.Origin = WorldPos + WorldNormal * 0.5; //    mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
	ray.Direction = sampleDirWorld;//reflect(ViewDir, WorldNormal);

	ray.TMin = 0.001f;
    ray.TMax = MAX_HIT_DIST;

	RayPayload payload;
    payload.position = 0.0f.xxx;
    payload.color = 0.0f.xxx;
    payload.normal = WorldNormal;
    payload.coneWidth = 0;
    payload.spreadAngle = ViewSpreadAngle; 
    payload.bHit = false;
    TraceDiffuseGIRay(ray, payload);
    if(payload.bHit == false)
    {
        float3 Irradiance = (bIncludeSkyLighting != 0u) ? max(EvaluateSkyColor(sampleDirWorld), 0.0f.xxx) : 0.0f.xxx;

        SH sh_indirect = init_SH();
        sh_indirect = irradiance_to_SH(Irradiance, sampleDirWorld);

        GIResultSH[launchIndex.xy] = sh_indirect.shY;
        GIResultColor[launchIndex.xy] = float4(Irradiance, 1.0f);
    }
    else
    {
        float3 LightDir = CommonSafeNormalize(LightDirAndIntensity.xyz, float3(0.0f, 1.0f, 0.0f));
        payload.normal = CommonSafeNormalize(payload.normal, WorldNormal);
        RayDesc shadowRay;
        shadowRay.Origin = payload.position + payload.normal * 0.5f;
        shadowRay.Direction = LightDir;

        shadowRay.TMin = 0.001f;
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

        float3 Albedo = max(CommonSanitizeFloat3(payload.color, 1.0f.xxx), 0.0f.xxx);
        SH sh_indirect = init_SH();
        float3 Irradiance = 0.0f.xxx;
        if(shadowPayload.bHit == false)
        {
            // miss - apply light color
            float NdotL = saturate(dot(LightDir, payload.normal));
            Irradiance += NdotL * LightIntensity * max(CommonSanitizeFloat3(LightColor, 1.0f.xxx), 0.0f.xxx) * Albedo * INV_PI;
        }
        sh_indirect = irradiance_to_SH(Irradiance, sampleDirWorld);

        GIResultSH[launchIndex.xy] = sh_indirect.shY;
        GIResultColor[launchIndex.xy] = float4(Irradiance, 1.0f);
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
    Vertex vertex = GetSurfaceVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);

    payload.position = CommonSanitizeFloat3(vertex.position, WorldRayOrigin() + WorldRayDirection() * RayTCurrent());
    float3 hitNormal = CommonSafeNormalize(vertex.normal, -WorldRayDirection());
    if (dot(hitNormal, -WorldRayDirection()) < 0.0f)
        hitNormal = -hitNormal;
    payload.normal = hitNormal;

    uint w, h;
    AlbedoTex.GetDimensions(w, h);
    float halfLog2NumTexPixels = 0.5 * log2(w * h);

    vertex.textureLODConstant += halfLog2NumTexPixels;
    float hitT = RayTCurrent();
    float rayConeWidth = payload.spreadAngle * hitT + payload.coneWidth;

    float NoV = 1;//dot(V, vertex.normal);
    float mipLevel = computeTextureLOD(NoV, rayConeWidth, vertex.textureLODConstant);

    payload.color = max(CommonSanitizeFloat3(AlbedoTex.SampleLevel(sampleWrap, vertex.uv, mipLevel).xyz, 1.0f.xxx), 0.0f.xxx);

    payload.bHit = true;
}

[shader("miss")]
void missShadow(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}

