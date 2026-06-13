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

Texture2D MaterialTextures[] : register(t0, space10);
StructuredBuffer<RTMaterialRecord> RtMaterials : register(t0, space11);

bool IsValidBindlessTextureIndex(uint index)
{
	return index != CORONA_INVALID_BINDLESS_INDEX;
}

#endif
