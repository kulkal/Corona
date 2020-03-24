#include "Common.hlsl"

RWTexture2D<float4> ReflectionResult : register(u0);

RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D GeoNormalTex : register(t2);
ByteAddressBuffer vertices : register(t3);
ByteAddressBuffer indices : register(t4);
Texture2D AlbedoTex : register(t5);
Texture2D MetallicRougnessTex : register(t6);
Texture3D BlueNoiseTex : register(t7);
Texture2D WorldNormalTex : register(t8);

cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4 ProjectionParams;
    float4 LightDir;
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
    float3 color;
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

float3 ImportanceSampleGGX_VNDF(float2 u, float roughness, float3 V, float3x3 TBN)
{
    float alpha = square(roughness);

    // float3 Ve = -float3(dot(V, TBN[0]), dot(V, TBN[1]), dot(V, TBN[2]));
    float3 Ve = normalize(mul(V, transpose(TBN)));

    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    
    float lensq = square(Vh.x) + square(Vh.y);
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);

    float r = sqrt(u.x);
    float phi = 2.0 * PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - square(t1)) + s * t2;

    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - square(t1) - square(t2))) * Vh;

    // Tangent space H
    float3 Ne = float3(alpha * Nh.x, max(0.0, Nh.z), alpha * Nh.y);

    // World space H
    // float WNe = float3(dot(Ne, T), dot(Ne, B), dot(Ne, N));
    return normalize(mul(Ne, TBN));
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
    float3x3 TBN = construct_ONB_frisvad(WorldNormal);

    float Rougness = 0;//MetallicRougnessTex.SampleLevel(sampleWrap, UV, 0).y;
    float3 N = WorldNormal;
    float3 V = mul(normalize(float3(d.x * aspectRatio, -d.y, -1)), InvViewMatrix);
    float3 H = ImportanceSampleGGX_VNDF(RandomUV, 0, V, TBN);
    float3 L = reflect(V, H);

    float NoV = max(0, -dot(N, V));
    float NoL = max(0, dot(N, L));
    float NoH = max(0, dot(N, H));
    float VoH = max(0, -dot(V, H));

    if (NoL > 0)
    {
    	RayDesc ray;
    	ray.Origin = WorldPos + GeoNormal * 0.5; //    mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
    	ray.Direction = L;//reflect(V, N);

    	ray.TMin = 0;
    	ray.TMax = 100000;

    	RayPayload payload;
    	TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, ray, payload);

    	ReflectionResult[launchIndex.xy] = float4(payload.color, 1);
        ReflectionResult[launchIndex.xy] = float4(0, 0, 1, 1);

    }
    else
    {
        N = -WorldNormal;

        TBN = construct_ONB_frisvad(N);
        float3 H = ImportanceSampleGGX_VNDF(RandomUV, Rougness, V, TBN);
        L = reflect(V, H);     
     
        RayDesc ray;
        ray.Origin = WorldPos + GeoNormal * 0.5; //    mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
        ray.Direction = L;//reflect(V, N);

        ray.TMin = 0;
        ray.TMax = 100000;

        RayPayload payload;
        TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, ray, payload);

        ReflectionResult[launchIndex.xy] = float4(payload.color, 1);
        ReflectionResult[launchIndex.xy] = float4(1, 0, 0, 1);

    }

        ReflectionResult[launchIndex.xy] = float4(abs(H), 1);

}



[shader("miss")]
void miss(inout RayPayload payload)
{
    payload.color = float3(0.0, 0.0, 0.1);
    payload.distance = 0;
}



struct Vertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float3 tangent;
};

uint3 Load3x16BitIndices(uint offsetBytes)
{
    uint3 index;

    // ByteAdressBuffer loads must be aligned at a 4 byte boundary.
    // Since we need to read three 16 bit indices: { 0, 1, 2 } 
    // aligned at a 4 byte boundary as: { 0 1 } { 2 0 } { 1 2 } { 0 1 } ...
    // we will load 8 bytes (~ 4 indices { a b | c d }) to handle two possible index triplet layouts,
    // based on first index's offsetBytes being aligned at the 4 byte boundary or not:
    //  Aligned:     { 0 1 | 2 - }
    //  Not aligned: { - 0 | 1 2 }
    const uint dwordAlignedOffset = offsetBytes & ~3;    
    const uint2 four16BitIndices = indices.Load2(dwordAlignedOffset);
 
    // Aligned: { 0 1 | 2 - } => retrieve first three 16bit indices
    if (dwordAlignedOffset == offsetBytes)
    {
        index.x = four16BitIndices.x & 0xffff;
        index.y = (four16BitIndices.x >> 16) & 0xffff;
        index.z = four16BitIndices.y & 0xffff;
    }
    else // Not aligned: { - 0 | 1 2 } => retrieve last three 16bit indices
    {
        index.x = (four16BitIndices.x >> 16) & 0xffff;
        index.y = four16BitIndices.y & 0xffff;
        index.z = (four16BitIndices.y >> 16) & 0xffff;
    }

    return index;
}

uint3 GetIndices(uint triangleIndex)
{
    uint baseIndex = (triangleIndex * 3 * 2) ;
    uint3 index;

    // ByteAdressBuffer loads must be aligned at a 4 byte boundary.
    // Since we need to read three 16 bit indices: { 0, 1, 2 } 
    // aligned at a 4 byte boundary as: { 0 1 } { 2 0 } { 1 2 } { 0 1 } ...
    // we will load 8 bytes (~ 4 indices { a b | c d }) to handle two possible index triplet layouts,
    // based on first index's offsetBytes being aligned at the 4 byte boundary or not:
    //  Aligned:     { 0 1 | 2 - }
    //  Not aligned: { - 0 | 1 2 }
    const uint dwordAlignedOffset = baseIndex & ~3;    
    const uint2 four16BitIndices = indices.Load2(dwordAlignedOffset);
 
    // Aligned: { 0 1 | 2 - } => retrieve first three 16bit indices
    if (dwordAlignedOffset == baseIndex)
    {
        index.x = four16BitIndices.x & 0xffff;
        index.y = (four16BitIndices.x >> 16) & 0xffff;
        index.z = four16BitIndices.y & 0xffff;
    }
    else // Not aligned: { - 0 | 1 2 } => retrieve last three 16bit indices
    {
        index.x = (four16BitIndices.x >> 16) & 0xffff;
        index.y = four16BitIndices.y & 0xffff;
        index.z = (four16BitIndices.y >> 16) & 0xffff;
    }

    return index;
}

Vertex GetVertexAttributes(uint triangleIndex, float3 barycentrics)
{
    uint3 index = GetIndices(triangleIndex);
    Vertex v;
    v.position = float3(0, 0, 0);
    v.uv = float2(0, 0);

    for (uint i = 0; i < 3; i++)
    {
        int address = (index[i] * 11) * 4;
        v.position += asfloat(vertices.Load3(address)) * barycentrics[i];
        address += (3 * 8);
        v.uv += asfloat(vertices.Load2(address)) * barycentrics[i];
    }

    return v;
}



[shader("closesthit")]
void chs(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    Vertex vertex = GetVertexAttributes(triangleIndex, barycentrics);

    payload.color = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, 0).xyz;
    payload.distance = RayTCurrent();
}
