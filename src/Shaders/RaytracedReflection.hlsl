#include "Common.hlsl"

RWTexture2D<float4> ReflectionResult : register(u0);

RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D GeoNormalTex : register(t2);
ByteAddressBuffer vertices : register(t3);
ByteAddressBuffer indices : register(t4);
Texture2D AlbedoTex : register(t5);
Texture2D RougnessMetallicTex : register(t6);
Texture3D BlueNoiseTex : register(t7);
Texture2D WorldNormalTex : register(t8);
ByteAddressBuffer InstanceProperty : register(t9);

cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4 ProjectionParams;
    float4 LightDirAndIntensity;
    float2 RandomOffset;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    float ViewSpreadAngle;
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

/*
    Params.x = Far / (Far - Near);
    Params.y = Near / (Near - Far);
    Params.z = Far;
*/

float3 offset_ray(float3 p, float3 n)
{
    float origin = 1.0f / 32.0f;
    float float_scale = 1.0f / 65536.0f;
    float int_scale = 256.0f;
	
    int3 of_i = int3(int_scale * n.x, int_scale * n.y, int_scale * n.z);

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
    float alpha = square(roughness);

    // float3 Ve = -float3(dot(V, TBN[0]), dot(V, TBN[1]), dot(V, TBN[2]));
    float3 Ve = normalize(mul(V, transpose(TBN)));

    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    
    float lensq = square(Vh.x) + square(Vh.y);
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);

    float r = sqrt(u.x);
    float phi = 2.0 * PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - square(t1)) + s * t2;

    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - square(t1) - square(t2))) * Vh;

    // Tangent space H
    float3 Ne = float3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z));

    return normalize(mul(Ne, TBN));
}

float3x3 buildTBN(float3 normal) {

    // TODO: Maybe try approach from here (Building an Orthonormal Basis, Revisited): 
    // https://graphics.pixar.com/library/OrthonormalB/paper.pdf

    // Pick random vector for generating orthonormal basis
    static const float3 rvec1 = float3(0.847100675f, 0.207911700f, 0.489073813f);
    static const float3 rvec2 = float3(-0.639436305f, -0.390731126f, 0.662155867f);
    float3 rvec;

    if (dot(rvec1, normal) > 0.95f)
        rvec = rvec2;
    else
        rvec = rvec1;

    // Construct TBN matrix to orient sampling hemisphere along the surface normal
    float3 b1 = normalize(rvec - normal * dot(rvec, normal));
    float3 b2 = cross(normal, b1);
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

	float3 WorldNormal = normalize(WorldNormalTex.SampleLevel(sampleWrap, UV, 0).xyz);
  
    float3 GeoNormal = normalize(GeoNormalTex.SampleLevel(sampleWrap, UV, 0).xyz);

    float LinearDepth = GetLinearDepthOpenGL(DeviceDepth, ProjectionParams.z, ProjectionParams.w) ;

	float2 ScreenPosition = crd.xy;
	ScreenPosition.x /= dims.x;
	ScreenPosition.y /= dims.y;
	ScreenPosition.xy = ScreenPosition.xy * 2 - 1;
	ScreenPosition.y = -ScreenPosition.y;

	float3 ViewPosition = GetViewPosition(LinearDepth, ScreenPosition, ProjMatrix._11, ProjMatrix._22);
	float3 WorldPos = mul(float4(ViewPosition, 1), InvViewMatrix).xyz;

    float2 RandomUV = LoadBlueNoise2(BlueNoiseTex, launchIndex, FrameCounter, BlueNoiseOffsetStride);
    float3x3 TBN = buildTBN(WorldNormal);

    float Rougness = RougnessMetallicTex.SampleLevel(sampleWrap, UV, 0).x;
    float3 N = WorldNormal;
    float3 V = mul(normalize(float3(d.x * aspectRatio, -d.y, -1)), InvViewMatrix);
    float3 H = ImportanceSampleGGX_VNDF(RandomUV, Rougness, -V, TBN, WorldNormal);
    float3 L = reflect(V, H);

    float NoV = max(0, -dot(N, V));
    float NoL = max(0, dot(N, L));
    float NoH = max(0, dot(N, H));
    float VoH = max(0, -dot(V, H));
    float LoH = max(0, -dot(L, H));

    float3 LightIntensity = LightDirAndIntensity.w;


	RayDesc ray;
	ray.Origin = WorldPos + GeoNormal * 0.5; //    mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
	ray.Direction = L;

	ray.TMin = 0;
	ray.TMax = 100000;

	RayPayload payload;
    payload.coneWidth = 0;
    payload.spreadAngle = ViewSpreadAngle; 
    TraceRay(gRtScene, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, ray, payload);

    if(payload.bHit == false)
    {
        // hit sky
        float3 Radiance = payload.color * LightIntensity;
        ReflectionResult[launchIndex.xy] = float4(Radiance, 1);
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
        uint RayIndex = 0;
        TraceRay(gRtScene, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES  /*rayFlags*/, 0xFF, RayIndex /* ray index*/, 0, 1, shadowRay, shadowPayload);

        float3 Irradiance = 0..xxx;
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
            

        ReflectionResult[launchIndex.xy] = float4(Irradiance, 1);
    }
}



[shader("miss")]
void miss(inout RayPayload payload)
{
    payload.position = float3(0, 0, 0);
    payload.color = float3(0.0, 0.2, 0.4);
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

    uint w, h;
    AlbedoTex.GetDimensions(w, h);
    float halfLog2NumTexPixels = 0.5 * log2(w * h);

    vertex.textureLODConstant += halfLog2NumTexPixels;
    float hitT = RayTCurrent();
    float rayConeWidth = payload.spreadAngle * hitT + payload.coneWidth;

    float NoV = 1;//dot(V, vertex.normal);
    float mipLevel = computeTextureLOD(NoV, rayConeWidth, vertex.textureLODConstant);
    payload.color =  AlbedoTex.SampleLevel(sampleWrap, vertex.uv, mipLevel).xyz;
    payload.bHit = true;
}

[shader("miss")]
void missShadow(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}
