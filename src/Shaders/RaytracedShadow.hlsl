#include "Common.hlsl"


RWTexture2D<float4> ShadowResult : register(u0);
RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
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
    bool bHit;
};

float random(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float2 SampleDisk(float2 u)
{
    float r = sqrt(u.x);
    float phi = 2.0 * PI * u.y;
    return float2(cos(phi), sin(phi)) * r;
}

float3x3 BuildBasis(float3 dir)
{
    float3 up = abs(dir.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, dir));
    float3 bitangent = cross(dir, tangent);
    return float3x3(tangent, bitangent, dir);
}

float3 offset_ray(float3 p, float3 n)
{
    float origin = 1.0f / 32.0f;
    float float_scale = 1.0f / 65536.0f;
    float int_scale = 256.0f;
	
    int3 of_i = int3(int_scale * n.x, int_scale * n.y, int_scale * n.z);

}

[shader("raygeneration")]
void rayGen()
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
  

    float LinearDepth = GetLinearDepthOpenGL(DeviceDepth, ProjectionParams.z, ProjectionParams.w) ;



	float2 ScreenPosition = crd.xy;
	ScreenPosition.x /= dims.x;
	ScreenPosition.y /= dims.y;
	ScreenPosition.xy = ScreenPosition.xy * 2 - 1;
	ScreenPosition.y = -ScreenPosition.y;

	// float3 ViewPosition = GetViewPosition(LinearDepth, ScreenPosition, ProjMatrix._11, ProjMatrix._22);
    float3 ViewPosition = GetViewPosition(DeviceDepth, ScreenPosition, InvProjMatrix);

	float3 WorldPos = mul(float4(ViewPosition, 1), InvViewMatrix).xyz;




    float3 baseLightDir = normalize(LightDir.xyz);
    float3x3 lightBasis = BuildBasis(baseLightDir);
    float visibility = 0.0f;
    const uint kMaxShadowSamples = 16;
    uint sampleCount = min(max(ShadowSampleCount, 1), kMaxShadowSamples);

    [loop]
    for (uint sampleIndex = 0; sampleIndex < kMaxShadowSamples; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
            break;

        float2 randUV = float2(
            random(crd + float2(sampleIndex * 13.17, 17.31)),
            random(crd + float2(sampleIndex * 29.73, 47.77)));
        float2 disk = SampleDisk(randUV) * ShadowLightRadius;
        float3 rayDir = normalize(baseLightDir + lightBasis[0] * disk.x + lightBasis[1] * disk.y);

        RayDesc ray;
        ray.Origin = WorldPos + WorldNormal * 0.5;
        ray.Direction = rayDir;
        ray.TMin = 0.01;
        ray.TMax = 100000;

        RayPayload payload;
        payload.bHit = true;
        TraceRay(gRtScene,
            RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
            0xFF, 0, 0, 0, ray, payload);

        visibility += payload.bHit == false ? 1.0 : 0.0;
    }

    visibility /= sampleCount;
    ShadowResult[launchIndex.xy] = float4(visibility.xxx, 1.0);

}

[shader("miss")]
void miss(inout RayPayload payload)
{
    // payload.opacity = 0.0;
    payload.bHit = false;
}

[shader("anyhit")]
void anyhit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    Vertex vertex = GetVertexAttributes(InstanceID(), vertices, indices, InstanceProperty, triangleIndex, barycentrics);

    float opacity = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, 5).w;

        // payload.bHit = false;

    if(opacity > 0.10)
    {
        payload.bHit = true;
        AcceptHitAndEndSearch();
    }
    
    IgnoreHit();
}
