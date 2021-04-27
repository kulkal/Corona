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
    bool bHit;
};

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




	RayDesc ray;
	ray.Origin = WorldPos + WorldNormal * 1.0; //    mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;

	ray.Direction = LightDir;

	ray.TMin = 0;
	ray.TMax = 100000;

	RayPayload payload;
    payload.bHit = true;
	TraceRay(gRtScene, 
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES 
       // RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER 
        , 0xFF, 0 /* ray index*/, 0, 0, ray, payload);

	if (payload.bHit == false )
	{
		ShadowResult[launchIndex.xy] = float4(1, 1, 1, 1);
	}
	else
	{
		ShadowResult[launchIndex.xy] = float4(0.0, 0.0, 0.0, 1);
	}

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
