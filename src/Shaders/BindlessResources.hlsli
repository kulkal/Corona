#ifndef CORONA_BINDLESS_RESOURCES_HLSLI
#define CORONA_BINDLESS_RESOURCES_HLSLI

#define CORONA_INVALID_BINDLESS_INDEX 0xffffffffu

struct RTMaterialRecord
{
	uint AlbedoTextureIndex;
	uint NormalTextureIndex;
	uint RoughnessTextureIndex;
	uint MetallicTextureIndex;
};

#if defined(CORONA_BINDLESS_MATERIALS) && CORONA_BINDLESS_MATERIALS
Texture2D MaterialTextures[] : register(t0, space10);
StructuredBuffer<RTMaterialRecord> RtMaterials : register(t0, space11);
#endif

struct RTGeometryRecord
{
	uint VertexBufferIndex;
	uint IndexBufferIndex;
	uint Padding0;
	uint Padding1;
};

#if defined(CORONA_BINDLESS_GEOMETRY) && CORONA_BINDLESS_GEOMETRY
ByteAddressBuffer GeometryBuffers[] : register(t0, space12);
StructuredBuffer<RTGeometryRecord> RtGeometries : register(t0, space13);
ByteAddressBuffer RtInstanceProperties : register(t0, space14);
#endif

bool IsValidBindlessTextureIndex(uint index)
{
	return index != CORONA_INVALID_BINDLESS_INDEX;
}

#if defined(CORONA_BINDLESS_GEOMETRY) && CORONA_BINDLESS_GEOMETRY
Vertex GetBindlessVertexAttributes(uint instanceID, uint triangleIndex, float3 barycentrics)
{
	RTGeometryRecord geometry = RtGeometries[instanceID];
	return GetVertexAttributes(
		instanceID,
		GeometryBuffers[NonUniformResourceIndex(geometry.VertexBufferIndex)],
		GeometryBuffers[NonUniformResourceIndex(geometry.IndexBufferIndex)],
		RtInstanceProperties,
		triangleIndex,
		barycentrics);
}

Vertex GetBindlessSurfaceVertexAttributes(uint instanceID, uint triangleIndex, float3 barycentrics)
{
	RTGeometryRecord geometry = RtGeometries[instanceID];
	return GetSurfaceVertexAttributes(
		instanceID,
		GeometryBuffers[NonUniformResourceIndex(geometry.VertexBufferIndex)],
		GeometryBuffers[NonUniformResourceIndex(geometry.IndexBufferIndex)],
		RtInstanceProperties,
		triangleIndex,
		barycentrics);
}

#define CORONA_INSTANCE_PROPERTY RtInstanceProperties
#define CORONA_GET_VERTEX_ATTRIBUTES(instanceID, triangleIndex, barycentrics) GetBindlessVertexAttributes(instanceID, triangleIndex, barycentrics)
#define CORONA_GET_SURFACE_VERTEX_ATTRIBUTES(instanceID, triangleIndex, barycentrics) GetBindlessSurfaceVertexAttributes(instanceID, triangleIndex, barycentrics)
#else
#define CORONA_INSTANCE_PROPERTY InstanceProperty
#define CORONA_GET_VERTEX_ATTRIBUTES(instanceID, triangleIndex, barycentrics) GetVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics)
#define CORONA_GET_SURFACE_VERTEX_ATTRIBUTES(instanceID, triangleIndex, barycentrics) GetSurfaceVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics)
#endif

#endif
