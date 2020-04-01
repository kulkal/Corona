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


    // float3 alpha = square(roughness);

    // // Transform incoming direction to local space, with geometry normal pointing to [0, 0, 1]
    // float4 q = getOrientation(float3(0, 0, 1), N);
    // float3 Ve = orientVector(inverseOrientation(q), V);

    // // Section 3.2: transforming the view direction to the hemisphere configuration
    // float3 Vh = normalize(float3(alpha.x * Ve.x, alpha.y * Ve.y, Ve.z));

    // // Section 4.1: orthonormal basis (with special case if cross product is zero)
    // float3 T1 = (Vh.z < 0.9999) ? normalize(cross(float3(0, 0, 1), Vh)) : float3(1, 0, 0);
    // float3 T2 = cross(Vh, T1);

    // // Section 4.2: parameterization of the projected area
    // float r = sqrt(u.x);
    // float phi = 2*PI * u.y;
    // float t1 = r * cos(phi);
    // float t2 = r * sin(phi);
    // float s = 0.5 * (1.0 + Vh.z);
    // t2 = lerp(sqrt(max(mad(-t1, t1, 1), 0)), t2, s);

    // // Section 4.3: reprojection onto hemisphere
    // float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, mad(-t1, t1, mad(-t2, t2, 1))))*Vh;

    // // Section 3.4: transforming the normal back to the ellipsoid configuration
    // float3 Ne = normalize(float3(alpha.x * Nh.x, alpha.y * Nh.y, max(0.0, Nh.z)));

    // float3 H = orientVector(q, Ne);

    // return H;
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
	ray.Direction = L;// reflect(V, N);

	ray.TMin = 0;
	ray.TMax = 100000;

	RayPayload payload;
	TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, ray, payload);

    if(payload.distance == 0)
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

        RayPayload shadowPayload;
        TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, shadowRay, shadowPayload);

        float3 Irradiance = 0..xxx;
        float3 Albedo = payload.color;
        if(shadowPayload.distance == 0)
        {
            // miss
            Irradiance = dot(LightDir.xyz, payload.normal) * LightIntensity  * Albedo;
        }
        else
        {
            // shadowed
        }
            // LightDir = float(0, 0, -1);
            // Irradiance = dot(LightDir.xyz, payload.normal) * 1  * 1;

        ReflectionResult[launchIndex.xy] = float4(Irradiance, 1);
        // ReflectionResult[launchIndex.xy] = float4(payload.color, 1);

    }

        // ReflectionResult[launchIndex.xy] = float4(payload.color, 1);

}



[shader("miss")]
void miss(inout RayPayload payload)
{
    payload.position = float3(0, 0, 0);
    payload.color = float3(0.0, 0.2, 0.4);
    payload.normal = float3(0, 0, -1);
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

    // for (uint i = 0; i < 3; i++)
    // {
    //     int address = (index[i] * 11) * 4;
    //     v.position += asfloat(vertices.Load3(address)) * barycentrics[i];
    //     address += (3 * 8);
    //     v.uv += asfloat(vertices.Load2(address)) * barycentrics[i];
    // }

    float3 p0 = asfloat(vertices.Load3((index[0] * 11) * 4));
    float3 p1 = asfloat(vertices.Load3((index[1] * 11) * 4));
    float3 p2 = asfloat(vertices.Load3((index[2] * 11) * 4));

    float4x4 WorldMatrix = {
        asfloat(InstanceProperty.Load4(InstanceID()*4*16)), 
        asfloat(InstanceProperty.Load4(InstanceID()*4*16 + 16)), 
        asfloat(InstanceProperty.Load4(InstanceID()*4*16 + 16*2)),
        asfloat(InstanceProperty.Load4(InstanceID()*4*16 + 16*3)),
    };


    v.position += p0 * barycentrics[0];
    v.position += p1 * barycentrics[1];
    v.position += p2 * barycentrics[2];

    v.position = mul(float4(v.position, 1), WorldMatrix).xyz;

    float2 uv0 = asfloat(vertices.Load2((index[0] * 11) * 4) + 3*8);
    float2 uv1 = asfloat(vertices.Load2((index[1] * 11) * 4) + 3*8);
    float2 uv2 = asfloat(vertices.Load2((index[2] * 11) * 4) + 3*8);

    v.uv += uv0 * barycentrics[0];
    v.uv += uv1 * barycentrics[1];
    v.uv += uv2 * barycentrics[2];



    float3 e1 = p1 - p0;
    float3 e2 = p2 - p0;
    v.normal = normalize(cross(e1, e2));

    v.normal = mul(float4(v.normal, 0), WorldMatrix).xyz;
    return v;
}



[shader("closesthit")]
void chs(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint triangleIndex = PrimitiveIndex();
    Vertex vertex = GetVertexAttributes(triangleIndex, barycentrics);

    payload.position = vertex.position;
    payload.normal = vertex.normal;
    payload.color =  AlbedoTex.SampleLevel(sampleWrap, vertex.uv, 0).xyz;

    payload.distance = RayTCurrent();
}
