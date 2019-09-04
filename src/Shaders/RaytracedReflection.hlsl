/***************************************************************************
# Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
***************************************************************************/

RWTexture2D<float4> gOutput : register(u0);

RaytracingAccelerationStructure gRtScene : register(t0);
Texture2D DepthTex : register(t1);
Texture2D WorldNormalTex : register(t2);
ByteAddressBuffer vertices : register(t3);
ByteAddressBuffer indices : register(t4);
Texture2D AlbedoTex : register(t5);

cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4 ProjectionParams;
    float4 LightDir;
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
float GetLinearDepth(float DeviceDepth, float ParamX, float ParamY, float ParamZ)
{
    // return Near/(Near-Far)/(DeviceDepth -Far/(Far-Near))*Far
    return ParamY / (DeviceDepth - ParamX) * ParamZ;
}

float3 GetViewPosition(float LinearDepth, float2 ScreenPosition, float Proj11, float Proj22)
{
    float2 screenSpaceRay = float2(ScreenPosition.x / Proj11,
                                   ScreenPosition.y / Proj22);
    
    float3 ViewPosition;
    ViewPosition.z = LinearDepth;
    // Solve the two projection equations
    ViewPosition.xy = screenSpaceRay.xy * ViewPosition.z;
    ViewPosition.z *= -1;
    return ViewPosition;
}

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


	float2 UV = crd / dims;
	float DeviceDepth = DepthTex.SampleLevel(sampleWrap, UV, 0).x;

	float3 WorldNormal = normalize(WorldNormalTex.SampleLevel(sampleWrap, UV, 0).xyz);
  

	float LinearDepth = GetLinearDepth(DeviceDepth, ProjectionParams.x, ProjectionParams.y, ProjectionParams.z) ;

	float2 ScreenPosition = crd.xy;
	ScreenPosition.x /= dims.x;
	ScreenPosition.y /= dims.y;
	ScreenPosition.xy = ScreenPosition.xy * 2 - 1;
	ScreenPosition.y = -ScreenPosition.y;

	float3 ViewPosition = GetViewPosition(LinearDepth, ScreenPosition, ProjMatrix._11, ProjMatrix._22);
	float3 WorldPos = mul(float4(ViewPosition, 1), InvViewMatrix).xyz;

  

    float3 ViewDir = mul(normalize(float3(d.x * aspectRatio, -d.y, -1)), InvViewMatrix);

	RayDesc ray;
	ray.Origin = WorldPos + WorldNormal * 0.5; //    mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
	ray.Direction = reflect(ViewDir, WorldNormal);

	ray.TMin = 0;
	ray.TMax = 100000;

	RayPayload payload;
	TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, 0, 0, ray, payload);

	gOutput[launchIndex.xy] = float4(payload.color, 1);
}



[shader("miss")]
void miss(inout RayPayload payload)
{
    //payload.color = float3(0.4, 0.6, 0.2);
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
