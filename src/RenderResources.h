#pragma once

// API-neutral renderer resource model.
//
// `Mesh`, `Scene`, `Material`, `DrawCall` are pure data containers that the
// renderer manipulates regardless of the active backend. They reference
// GPU-side resource handles (`Texture`, `IndexBuffer`, `VertexBuffer`, etc.)
// only through forward declarations so this header has no DX12 / Vulkan
// dependency.
//
// The concrete resource classes live in the active backend header
// (e.g. DX12Backend.h defines `Texture` with ComPtr<ID3D12Resource>); any
// inline use of those members must happen in backend-specific translation
// units.

#include <cstdint>
#include <memory>
#include <vector>

#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"

#include "RenderBackend.h"

class IRenderBackend;
class Texture;
class Buffer;
class VertexBuffer;
class IndexBuffer;
class Sampler;
class RTAS;

class Material
{
public:
	bool bHasAlpha = false;
	glm::vec4 BaseColorFactor = glm::vec4(1.0f);

	std::shared_ptr<Texture> Diffuse;
	std::shared_ptr<Texture> Normal;
	std::shared_ptr<Texture> Roughness;
	std::shared_ptr<Texture> Metallic;
};

class Mesh
{
public:
	IRenderBackend* Owner = nullptr;

	struct DrawCall
	{
		std::shared_ptr<Material> mat;
		int32_t DiffuseTextureIndex = -1;
		int32_t NormalTextureIndex = -1;
		int32_t SpecularTextureIndex = -1;
		uint32_t IndexStart = 0;
		uint32_t IndexCount = 0;
		uint32_t VertexBase = 0;
		uint32_t VertexCount = 0;
	};

	bool bTransparent = false;
	glm::mat4x4 transform = glm::mat4x4(1.0f);
	uint32_t NumIndices = 0;
	uint32_t NumVertices = 0;

	uint32_t VertexStride = 0;

	EIndexFormat IndexFormat = EIndexFormat::U32;

	std::shared_ptr<IndexBuffer> Ib;
	std::shared_ptr<VertexBuffer> Vb;
	std::vector<glm::vec3> CpuPositions;
	std::vector<uint32_t> CpuIndices;
	std::vector<std::shared_ptr<Texture>> Textures;

	std::shared_ptr<Material> Mat;

	std::vector<DrawCall> Draws;

	std::shared_ptr<RTAS> CreateBLAS();
};

class Scene
{
public:
	void SetTransform(glm::mat4x4 inTransform);

public:
	std::vector<std::shared_ptr<Mesh>> meshes;
	std::vector<std::shared_ptr<Material>> Materials;
	bool bHasBounds = false;
	glm::vec3 BoundsMin = glm::vec3(0.0f);
	glm::vec3 BoundsMax = glm::vec3(0.0f);
};
