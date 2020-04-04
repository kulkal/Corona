#include "Common.hlsl"

RWTexture2D<float4> GIResultSH : register(u0);
RWTexture2D<float4> GIResultColor : register(u1);


RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
ByteAddressBuffer vertices : register(t3);
ByteAddressBuffer indices : register(t4);
Texture2D AlbedoTex : register(t5);
ByteAddressBuffer InstanceProperty : register(t6);
Texture3D BlueNoiseTex : register(t7);


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

float random(float2 co){
    return frac(sin(dot(co.xy ,float2(12.9898,78.233))) * 43758.5453);
}

float madFrac(float a, float b) {
    return (a*b) - floor(a*b);
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
  

    float LinearDepth = GetLinearDepthOpenGL(DeviceDepth, ProjectionParams.z, ProjectionParams.w) ;
	
    float2 ScreenPosition = crd.xy;
	ScreenPosition.x /= dims.x;
	ScreenPosition.y /= dims.y;
	ScreenPosition.xy = ScreenPosition.xy * 2 - 1;
	ScreenPosition.y = -ScreenPosition.y;

	float3 ViewPosition = GetViewPosition(LinearDepth, ScreenPosition, ProjMatrix._11, ProjMatrix._22);
	float3 WorldPos = mul(float4(ViewPosition, 1), InvViewMatrix).xyz;

  
    float rand_u = random(crd + RandomOffset);
    float rand_v = random(crd + RandomOffset + float2(100, 100));

    float2 RandomUV = LoadBlueNoise2(BlueNoiseTex, launchIndex, FrameCounter, BlueNoiseOffsetStride);

    // RandomUV = float2(rand_u, rand_v);
    // float3 sampleDirLocal = SampleUniformHemisphere(rand_u, rand_v);
    float3 sampleDirLocal = SampleHemisphereCosine(RandomUV.x, RandomUV.y);


    float3x3 tbn = buildTBN(WorldNormal);
    float3 sampleDirWorld = mul(sampleDirLocal, tbn);

    // https://computergraphics.stackexchange.com/questions/4664/does-cosine-weighted-hemisphere-sampling-still-require-ndotl-when-calculating-co
    // https://computergraphics.stackexchange.com/questions/8578/how-to-set-equivalent-pdfs-for-cosine-weighted-and-uniform-sampled-hemispheres
    float cosTerm = 1;//dot(float3(0, 0, 1), sampleDirLocal)*2;

    float3 LightIntensity = LightDirAndIntensity.w;

    float3 ViewDir = mul(normalize(float3(d.x * aspectRatio, -d.y, -1)), InvViewMatrix);

	RayDesc ray;
	ray.Origin = WorldPos + WorldNormal * 0.5; //    mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
	ray.Direction = normalize(sampleDirWorld);//reflect(ViewDir, WorldNormal);

	ray.TMin = 0;
	ray.TMax = 100000;

	RayPayload payload;
	TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, ray, payload);
    if(payload.distance == 0)
    {
        // hit sky
        float3 Radiance = payload.color * LightIntensity;
        float3 Irradiance = Radiance * cosTerm;

        SH sh_indirect = init_SH();
        sh_indirect = irradiance_to_SH(Irradiance, sampleDirWorld);

        GIResultSH[launchIndex.xy] = sh_indirect.shY;
        GIResultColor[launchIndex.xy] = float4(sh_indirect.CoCg, 0, 0);
        // gOutput[launchIndex.xy] = float4(DiffuseLighting.xyz, 1);   
    }
    else
    {
        float3 LightDir = LightDirAndIntensity.xyz;
        RayDesc shadowRay;
        shadowRay.Origin = payload.position + payload.normal *0.5;
        shadowRay.Direction = LightDir;

        shadowRay.TMin = 0;
        shadowRay.TMax = 100000;

        RayPayload shadowPayload;
        TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, shadowRay, shadowPayload);

        float3 Albedo = payload.color;
        SH sh_indirect = init_SH();
        if(shadowPayload.distance == 0)
        {
            // miss
            float3 Irradiance = dot(LightDir.xyz, payload.normal) * LightIntensity  * Albedo;
            sh_indirect = irradiance_to_SH(Irradiance, sampleDirWorld);
        }
        else
        {
            // shadowed
        }

        // gOutput[launchIndex.xy] = float4(DiffuseLighting.xyz, 1);   

        GIResultSH[launchIndex.xy] = sh_indirect.shY;
        GIResultColor[launchIndex.xy] = float4(sh_indirect.CoCg, 0, 0);
    }


}



[shader("miss")]
void miss(inout RayPayload payload)
{
    payload.position = float3(0, 0, 0);
    payload.color = float3(0.0, 0.2, 0.4);
    payload.normal = float3(0, 0, -1);
    payload.distance = 0;
}

[shader("closesthit")]
void chs(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    // Vertex vertex = GetVertexAttributes(triangleIndex, barycentrics);
    Vertex vertex = GetVertexAttributes(InstanceID(), vertices, indices, InstanceProperty, triangleIndex, barycentrics);

    payload.position = vertex.position;
    payload.normal = vertex.normal;
    payload.color = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, 0).xyz;

    payload.distance = RayTCurrent();
}


[shader("closesthit")]
void chsShadow(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    // float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    // uint triangleIndex = PrimitiveIndex();
    // Vertex vertex = GetVertexAttributes(triangleIndex, barycentrics);

    // payload.position = vertex.position;
    // payload.normal = vertex.normal;
    // payload.color =  AlbedoTex.SampleLevel(sampleWrap, vertex.uv, 0).xyz;

    // payload.distance = RayTCurrent();
}


[shader("anyhit")]
void anyhitShadow(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.distance = RayTCurrent();;

    AcceptHitAndEndSearch();
}

[shader("miss")]
void missShadow(inout RayPayload payload)
{
    payload.position = float3(0, 0, 0);
    payload.color = float3(0.0, 0.2, 0.4);
    payload.normal = float3(0, 0, -1);
    payload.distance = 0;
}

