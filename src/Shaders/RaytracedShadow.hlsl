#include "Common.hlsl"


RWTexture2D<float4> ShadowResult : register(u0);
RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
Texture2D AlbedoTex : register(t3);

// Texture2D dummy : register(t3);
// Texture2D dummy2 : register(t3);

cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4 ProjectionParams;
    float4 LightDir;
    float4 pad[2];
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
    //float3 color;
    float distance;
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

#define DEPTH 0
#define SIMPLE 0
#define SHADOW 1

#if SIMPLE 
    RayDesc ray;
    ray.Origin = float3(0, 0, -2);
    ray.Direction = normalize(float3(d.x * aspectRatio, -d.y, 1));

    ray.TMin = 0;
    ray.TMax = 100000;

    RayPayload payload;
    //TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, ray, payload);
    float dd = payload.distance / 1000.0f;
    ShadowResult[launchIndex.xy] = float4(dd, dd, dd, 1);
    float sx = sin(crd.x);
    float sy = sin(crd.y);
    float DeviceDepth = DepthTex.Load(int3(launchIndex.xy, 0), int2(0, 0)).x;
    float3 WorldNormal = WorldNormalTex.Load(int3(launchIndex.xy, 0), int2(0, 0)).xyz;
    ShadowResult[launchIndex.xy] = float4(WorldNormal, 1);
    float LinearDepth = GetLinearDepth(DeviceDepth, ProjectionParams.x, ProjectionParams.y) * ProjectionParams.z /1000.0f;

    ShadowResult[launchIndex.xy] = float4(LinearDepth, LinearDepth, LinearDepth, 1);


#elif DEPTH
	RayDesc ray;
	ray.Origin = mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
	ray.Direction = mul(normalize(float3(d.x * aspectRatio, -d.y, -1)), InvViewMatrix);

	ray.TMin = 0;
	ray.TMax = 100000;

	RayPayload payload;
    TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, ray, payload);
	float dd = payload.distance / 1000.0f;
    ShadowResult[launchIndex.xy] = float4(dd, dd, dd, 1);
    //gOutput[launchIndex.xy] = float4(1, 0, 0, 1);

#elif SHADOW
	float2 UV = crd / dims;
	float DeviceDepth = DepthTex.SampleLevel(sampleWrap, UV, 0).x;

	float3 WorldNormal = normalize(WorldNormalTex.SampleLevel(sampleWrap, UV, 0).xyz);
  

	// float LinearDepth = GetLinearDepth(DeviceDepth, ProjectionParams.x, ProjectionParams.y, ProjectionParams.z) ;
    float LinearDepth = GetLinearDepthOpenGL(DeviceDepth, ProjectionParams.z, ProjectionParams.w) ;



	float2 ScreenPosition = crd.xy;
	ScreenPosition.x /= dims.x;
	ScreenPosition.y /= dims.y;
	ScreenPosition.xy = ScreenPosition.xy * 2 - 1;
	ScreenPosition.y = -ScreenPosition.y;

    // float3 ViewPosition = normalize(float3(d.x * aspectRatio, -d.y, -1)) * LinearDepth;
	float3 ViewPosition = GetViewPosition(LinearDepth, ScreenPosition, ProjMatrix._11, ProjMatrix._22);
	float3 WorldPos = mul(float4(ViewPosition, 1), InvViewMatrix).xyz;




	RayDesc ray;
	ray.Origin = WorldPos + WorldNormal * 0.5; //    mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
    // ray.Origin = WorldPos ; //    mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;

	ray.Direction = LightDir;

	ray.TMin = 0;
	ray.TMax = 100000;

	RayPayload payload;
	TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, ray, payload);

	if (payload.distance == 0)
	{
		ShadowResult[launchIndex.xy] = float4(1, 1, 1, 1);
	}
	else
	{
		ShadowResult[launchIndex.xy] = float4(0.0, 0.0, 0.0, 1);
	}

        // gOutput[launchIndex.xy] = float4(LinearDepth/1000.0, 0.1, 0.1, 1);

#endif
}

[shader("miss")]

    void miss
    (inout
    RayPayload payload)
{
    //payload.color = float3(0.4, 0.6, 0.2);
    payload.distance = 0;
}

[shader("closesthit")]

    void chs
    (inout
    RayPayload payload, in BuiltInTriangleIntersectionAttributes
    attribs)
{
    //float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);

    //const float3 A = float3(1, 0, 0);
    //const float3 B = float3(0, 1, 0);
    //const float3 C = float3(0, 0, 1);

    //payload.color = A * barycentrics.x + B * barycentrics.y + C * barycentrics.z;
    payload.distance = RayTCurrent();
}
