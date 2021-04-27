#include "Common.hlsl"

Texture2D Normal : register(t0);
Texture2D RoughnessMetallic : register(t1);
Texture2D Depth : register(t2);

RWTexture2D<float4> NormalRoughness : register(u0);
RWTexture2D<float> LinearDepth: register(u1);

cbuffer ResolveNRDParam : register(b0)
{
    float4x4 InvProjMatrix;
	float Near;
	float Far;
};

[numthreads(32, 32, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
	float3 normal = Normal[DTid.xy].xyz;
	float roughness = RoughnessMetallic[DTid.xy].x;
	NormalRoughness[DTid.xy] = float4(normal, roughness);
	
	float DeviceDepth = Depth[DTid.xy].x;
	if (DeviceDepth == 1) 
		DeviceDepth = 0;
	float3 ViewPosition = GetViewPosition(DeviceDepth, float2(0, 0), InvProjMatrix);
	
	LinearDepth[DTid.xy].x = ViewPosition.z / (Far - Near);
}