//*********************************************************
//
// Spine runtime bridge for Luau-driven 2D skeleton sampling.
//
//*********************************************************

#include "stdafx.h"
#include "Corona.h"
#include "PlatformSystem.h"
#include "RenderResources.h"

#include <spine/Atlas.h>
#include <spine/Attachment.h>
#include <spine/Animation.h>
#include <spine/MeshAttachment.h>
#include <spine/RegionAttachment.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonData.h>
#include <spine/SkeletonJson.h>
#include <spine/Slot.h>
#include <spine/WeightedMeshAttachment.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

void AppendCpuRuntimeTrace(const std::wstring& line);

extern "C" void _spAtlasPage_createTexture(spAtlasPage* self, const char* path)
{
	(void)path;
	if (self)
		self->rendererObject = nullptr;
}

extern "C" void _spAtlasPage_disposeTexture(spAtlasPage* self)
{
	if (self)
		self->rendererObject = nullptr;
}

extern "C" char* _readFile(const char* path, int* length);

extern "C" char* _spUtil_readFile(const char* path, int* length)
{
	return _readFile(path, length);
}

namespace
{
	struct SpineSampleVertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
		glm::vec3 Tangent;
	};

	struct SpineSkinInputVertex
	{
		glm::vec2 UV;
		float LocalZ = 0.0f;
		UINT32 InfluenceOffset = 0;
		UINT32 InfluenceCount = 0;
		UINT32 Padding = 0;
	};

	struct SpineSkinInfluence
	{
		glm::vec2 LocalPosition = glm::vec2(0.0f);
		UINT32 BoneIndex = 0;
		float Weight = 1.0f;
	};

	struct SpineSkinBone
	{
		glm::vec4 X = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec4 Y = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
	};

	struct SpineSampleMesh
	{
		struct DrawRange
		{
			UINT32 IndexStart = 0;
			UINT32 IndexCount = 0;
			UINT32 VertexBase = 0;
			UINT32 VertexCount = 0;
		};

		std::vector<SpineSampleVertex> Vertices;
		std::vector<UINT32> Indices;
		std::vector<DrawRange> Draws;
		std::vector<SpineSkinInputVertex> SkinVertices;
		std::vector<SpineSkinInfluence> SkinInfluences;
		std::vector<SpineSkinBone> SkinBones;
		glm::vec3 BoundsMin = glm::vec3(std::numeric_limits<float>::max());
		glm::vec3 BoundsMax = glm::vec3(std::numeric_limits<float>::lowest());
		UINT32 CpuFallbackVertexCount = 0;
		UINT32 GpuPlaceholderVertexCount = 0;
		int RegionAttachmentCount = 0;
		int MeshAttachmentCount = 0;
		int WeightedMeshAttachmentCount = 0;
	};

	struct SpineAtlasDeleter
	{
		void operator()(spAtlas* atlas) const
		{
			if (atlas)
				spAtlas_dispose(atlas);
		}
	};

	struct SpineSkeletonJsonDeleter
	{
		void operator()(spSkeletonJson* json) const
		{
			if (json)
				spSkeletonJson_dispose(json);
		}
	};

	struct SpineSkeletonDataDeleter
	{
		void operator()(spSkeletonData* data) const
		{
			if (data)
				spSkeletonData_dispose(data);
		}
	};

	struct SpineSkeletonDeleter
	{
		void operator()(spSkeleton* skeleton) const
		{
			if (skeleton)
				spSkeleton_dispose(skeleton);
		}
	};

	struct SpineRuntimeAsset
	{
		std::filesystem::path SkeletonPath;
		std::filesystem::path AtlasPath;
		std::filesystem::path DiffusePath;
		std::unique_ptr<spAtlas, SpineAtlasDeleter> Atlas;
		std::unique_ptr<spSkeletonData, SpineSkeletonDataDeleter> SkeletonData;
		std::shared_ptr<Texture> DiffuseTexture;
		bool bDiffuseFileExists = false;
		bool bDiffuseLoaded = false;
	};

	std::unordered_map<std::wstring, std::unique_ptr<SpineRuntimeAsset>> GSpineRuntimeAssetCache;
	void* GSpineRuntimeAssetCacheBackend = nullptr;

	constexpr int kRegionQuadTriangles[] = { 0, 1, 2, 2, 3, 0 };
	constexpr float kSpineAttachmentLocalDepthStep = 0.02f;

	std::string ToUtf8Path(const std::filesystem::path& path)
	{
		return PlatformWideToUtf8(path.wstring());
	}

	std::wstring WidenAscii(const char* text)
	{
		if (!text)
			return std::wstring();
		return std::wstring(text, text + std::strlen(text));
	}

	std::wstring NormalizePathForKey(const std::filesystem::path& path)
	{
		std::error_code ec;
		std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
		if (ec)
			normalized = std::filesystem::absolute(path, ec).lexically_normal();
		if (normalized.empty())
			normalized = path.lexically_normal();
		return normalized.wstring();
	}

	const char* SelectAnimationName(spSkeletonData* skeletonData, const std::string& requestedAnimation)
	{
		if (!skeletonData || skeletonData->animationsCount <= 0)
			return nullptr;

		if (!requestedAnimation.empty() && spSkeletonData_findAnimation(skeletonData, requestedAnimation.c_str()))
			return requestedAnimation.c_str();

		if (spSkeletonData_findAnimation(skeletonData, "idle"))
			return "idle";

		return skeletonData->animations[0] ? skeletonData->animations[0]->name : nullptr;
	}

	void AddSpineFallbackVertex(SpineSampleMesh& outMesh, const glm::vec2& position, const glm::vec2& uv, float localZ, float sourceScale)
	{
		SpineSampleVertex vertex = {};
		vertex.Position = glm::vec3(position.x * sourceScale, position.y * sourceScale, localZ * sourceScale);
		vertex.Normal = glm::vec3(0.0f, 0.0f, 1.0f);
		vertex.UV = uv;
		vertex.Tangent = glm::vec3(1.0f, 0.0f, 0.0f);
		outMesh.Vertices.push_back(vertex);

		outMesh.BoundsMin = glm::min(outMesh.BoundsMin, vertex.Position);
		outMesh.BoundsMax = glm::max(outMesh.BoundsMax, vertex.Position);
	}

	template <typename IndexType>
	void AppendAttachmentIndices(
		SpineSampleMesh& outMesh,
		UINT32 baseVertex,
		const IndexType* triangles,
		int triangleIndexCount)
	{
		if (!triangles || triangleIndexCount <= 0)
			return;

		// Indices are stored *local* to the attachment's vertex range.
		// The renderer passes drawcall.VertexBase = baseVertex to
		// vkCmdDrawIndexed / DrawIndexedInstanced, which adds it to each
		// fetched index — so adding baseVertex here too would double-offset
		// and read the wrong vertices (manifests as zig-zag / flying-head
		// garbage on multi-attachment Spine meshes).
		//
		// Only emit a single winding per triangle. The GBuffer pipeline runs
		// with backface culling disabled, so two-sided visibility is already
		// covered; emitting the reversed winding too would put two coplanar
		// triangles at identical depth and trigger flickering z-fights
		// (visible as e.g. alternating-eye disappearance on Spine faces).
		(void)baseVertex;
		for (int i = 0; i + 2 < triangleIndexCount; i += 3)
		{
			const UINT32 a = static_cast<UINT32>(triangles[i]);
			const UINT32 b = static_cast<UINT32>(triangles[i + 1]);
			const UINT32 c = static_cast<UINT32>(triangles[i + 2]);
			outMesh.Indices.push_back(a);
			outMesh.Indices.push_back(b);
			outMesh.Indices.push_back(c);
		}
	}

	UINT32 FindBoneIndex(spSkeleton* skeleton, const spBone* bone)
	{
		if (!skeleton || !bone)
			return 0;

		for (int boneIndex = 0; boneIndex < skeleton->bonesCount; ++boneIndex)
		{
			if (skeleton->bones[boneIndex] == bone)
				return static_cast<UINT32>(boneIndex);
		}
		return 0;
	}

	void BuildSkinBones(SpineSampleMesh& outMesh, spSkeleton* skeleton)
	{
		outMesh.SkinBones.clear();
		if (!skeleton)
			return;

		outMesh.SkinBones.reserve(static_cast<size_t>(std::max(0, skeleton->bonesCount)));
		const float skeletonX = skeleton->x;
		const float skeletonY = skeleton->y;
		for (int boneIndex = 0; boneIndex < skeleton->bonesCount; ++boneIndex)
		{
			const spBone* bone = skeleton->bones[boneIndex];
			SpineSkinBone skinBone = {};
			if (bone)
			{
				skinBone.X = glm::vec4(bone->a, bone->b, bone->worldX + skeletonX, 0.0f);
				skinBone.Y = glm::vec4(bone->c, bone->d, bone->worldY + skeletonY, 0.0f);
			}
			outMesh.SkinBones.push_back(skinBone);
		}
	}

	void AddSpineSkinVertex(
		SpineSampleMesh& outMesh,
		float localX,
		float localY,
		float u,
		float v,
		float localZ,
		UINT32 boneIndex,
		float weight = 1.0f)
	{
		const UINT32 influenceOffset = static_cast<UINT32>(outMesh.SkinInfluences.size());
		outMesh.SkinInfluences.push_back({ glm::vec2(localX, localY), boneIndex, weight });
		outMesh.SkinVertices.push_back({ glm::vec2(u, v), localZ, influenceOffset, 1u, 0u });
	}

	void AddSpineSkinVertexFromInfluenceRange(
		SpineSampleMesh& outMesh,
		float u,
		float v,
		float localZ,
		UINT32 influenceOffset,
		UINT32 influenceCount)
	{
		if (influenceCount == 0)
			return;
		outMesh.SkinVertices.push_back({ glm::vec2(u, v), localZ, influenceOffset, influenceCount, 0u });
	}

	void AppendSkinFallbackVertices(
		SpineSampleMesh& outMesh,
		UINT32 skinVertexStart,
		float sourceScale)
	{
		const UINT32 skinVertexEnd = static_cast<UINT32>(outMesh.SkinVertices.size());
		for (UINT32 skinVertexIndex = skinVertexStart; skinVertexIndex < skinVertexEnd; ++skinVertexIndex)
		{
			const SpineSkinInputVertex& skinVertex = outMesh.SkinVertices[skinVertexIndex];
			glm::vec2 worldPosition(0.0f);
			for (UINT32 influenceIndex = 0; influenceIndex < skinVertex.InfluenceCount; ++influenceIndex)
			{
				const UINT32 sourceInfluenceIndex = skinVertex.InfluenceOffset + influenceIndex;
				if (sourceInfluenceIndex >= outMesh.SkinInfluences.size())
					continue;

				const SpineSkinInfluence& influence = outMesh.SkinInfluences[sourceInfluenceIndex];
				if (influence.BoneIndex >= outMesh.SkinBones.size())
					continue;

				const SpineSkinBone& bone = outMesh.SkinBones[influence.BoneIndex];
				const glm::vec2 localPosition = influence.LocalPosition;
				const glm::vec2 transformedPosition(
					localPosition.x * bone.X.x + localPosition.y * bone.X.y + bone.X.z,
					localPosition.x * bone.Y.x + localPosition.y * bone.Y.y + bone.Y.z);
				worldPosition += transformedPosition * influence.Weight;
			}

			AddSpineFallbackVertex(outMesh, worldPosition, skinVertex.UV, skinVertex.LocalZ, sourceScale);
			++outMesh.CpuFallbackVertexCount;
		}
	}

	void AppendSkinPlaceholderVertices(
		SpineSampleMesh& outMesh,
		UINT32 skinVertexStart,
		float sourceScale)
	{
		const UINT32 skinVertexEnd = static_cast<UINT32>(outMesh.SkinVertices.size());
		for (UINT32 skinVertexIndex = skinVertexStart; skinVertexIndex < skinVertexEnd; ++skinVertexIndex)
		{
			const SpineSkinInputVertex& skinVertex = outMesh.SkinVertices[skinVertexIndex];
			glm::vec2 approximateLocalPosition(0.0f);
			float accumulatedWeight = 0.0f;
			for (UINT32 influenceIndex = 0; influenceIndex < skinVertex.InfluenceCount; ++influenceIndex)
			{
				const UINT32 sourceInfluenceIndex = skinVertex.InfluenceOffset + influenceIndex;
				if (sourceInfluenceIndex >= outMesh.SkinInfluences.size())
					continue;

				const SpineSkinInfluence& influence = outMesh.SkinInfluences[sourceInfluenceIndex];
				approximateLocalPosition += influence.LocalPosition * influence.Weight;
				accumulatedWeight += influence.Weight;
			}
			if (accumulatedWeight > 1.0e-5f && std::abs(accumulatedWeight - 1.0f) > 1.0e-4f)
				approximateLocalPosition /= accumulatedWeight;

			SpineSampleVertex vertex = {};
			vertex.Position = glm::vec3(
				approximateLocalPosition.x * sourceScale,
				approximateLocalPosition.y * sourceScale,
				skinVertex.LocalZ * sourceScale);
			vertex.Normal = glm::vec3(0.0f, 0.0f, 1.0f);
			vertex.UV = skinVertex.UV;
			vertex.Tangent = glm::vec3(1.0f, 0.0f, 0.0f);
			outMesh.Vertices.push_back(vertex);
			outMesh.BoundsMin = glm::min(outMesh.BoundsMin, vertex.Position);
			outMesh.BoundsMax = glm::max(outMesh.BoundsMax, vertex.Position);
			++outMesh.GpuPlaceholderVertexCount;
		}
	}

	void AppendRegionSkinVertices(
		SpineSampleMesh& outMesh,
		spSkeleton* skeleton,
		spSlot* slot,
		const spRegionAttachment* attachment,
		float localZ)
	{
		if (!slot || !slot->bone || !attachment)
			return;

		const UINT32 boneIndex = FindBoneIndex(skeleton, slot->bone);
		for (int i = 0; i + 1 < 8; i += 2)
			AddSpineSkinVertex(outMesh, attachment->offset[i], attachment->offset[i + 1], attachment->uvs[i], attachment->uvs[i + 1], localZ, boneIndex);
	}

	void AppendMeshSkinVertices(
		SpineSampleMesh& outMesh,
		spSkeleton* skeleton,
		spSlot* slot,
		const spMeshAttachment* attachment,
		float localZ)
	{
		if (!slot || !slot->bone || !attachment || !attachment->uvs)
			return;

		const UINT32 boneIndex = FindBoneIndex(skeleton, slot->bone);
		const float* vertices = attachment->vertices;
		if (slot->attachmentVerticesCount == attachment->verticesCount && slot->attachmentVertices)
			vertices = slot->attachmentVertices;
		if (!vertices)
			return;

		for (int i = 0; i + 1 < attachment->verticesCount; i += 2)
			AddSpineSkinVertex(outMesh, vertices[i], vertices[i + 1], attachment->uvs[i], attachment->uvs[i + 1], localZ, boneIndex);
	}

	void AppendWeightedMeshSkinVertices(
		SpineSampleMesh& outMesh,
		spSlot* slot,
		const spWeightedMeshAttachment* attachment,
		float localZ)
	{
		if (!slot || !attachment || !attachment->bones || !attachment->weights || !attachment->uvs)
			return;

		const float* ffd = slot->attachmentVerticesCount > 0 ? slot->attachmentVertices : nullptr;
		int boneCursor = 0;
		int weightCursor = 0;
		int ffdCursor = 0;
		int uvCursor = 0;
		while (boneCursor < attachment->bonesCount && uvCursor + 1 < attachment->uvsCount)
		{
			const int influenceCount = attachment->bones[boneCursor++];
			const UINT32 influenceOffset = static_cast<UINT32>(outMesh.SkinInfluences.size());
			for (int influenceIndex = 0; influenceIndex < influenceCount; ++influenceIndex)
			{
				if (boneCursor >= attachment->bonesCount || weightCursor + 2 >= attachment->weightsCount)
					break;

				const UINT32 boneIndex = static_cast<UINT32>(std::max(0, attachment->bones[boneCursor++]));
				float localX = attachment->weights[weightCursor];
				float localY = attachment->weights[weightCursor + 1];
				const float weight = attachment->weights[weightCursor + 2];
				weightCursor += 3;
				if (ffd)
				{
					localX += ffd[ffdCursor];
					localY += ffd[ffdCursor + 1];
					ffdCursor += 2;
				}
				outMesh.SkinInfluences.push_back({ glm::vec2(localX, localY), boneIndex, weight });
			}

			const UINT32 appendedCount = static_cast<UINT32>(outMesh.SkinInfluences.size()) - influenceOffset;
			AddSpineSkinVertexFromInfluenceRange(
				outMesh,
				attachment->uvs[uvCursor],
				attachment->uvs[uvCursor + 1],
				localZ,
				influenceOffset,
				appendedCount);
			uvCursor += 2;
		}
	}

	void AppendSpineDrawRange(SpineSampleMesh& outMesh, UINT32 baseVertex, UINT32 indexStart)
	{
		const UINT32 indexCount = static_cast<UINT32>(outMesh.Indices.size()) - indexStart;
		const UINT32 vertexCount = static_cast<UINT32>(outMesh.Vertices.size()) - baseVertex;
		if (indexCount == 0 || vertexCount == 0)
			return;

		outMesh.Draws.push_back({ indexStart, indexCount, baseVertex, vertexCount });
	}

	SpineSampleMesh BuildSpineSampleMesh(spSkeleton* skeleton, float sourceScale, bool bBuildCpuFallbackVertices)
	{
		SpineSampleMesh mesh;
		BuildSkinBones(mesh, skeleton);

		// Spine drawOrder is already the intended back-to-front painter order.
		// Keep that order explicitly and give each attachment a tiny depth layer
		// for stable tests against world depth without making sprites look thick.
		const int slotCount = skeleton ? skeleton->slotsCount : 0;
		for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
		{
			spSlot* slot = skeleton->drawOrder ? skeleton->drawOrder[slotIndex] : skeleton->slots[slotIndex];
			if (!slot || !slot->attachment)
				continue;

			const float localZ = -static_cast<float>(slotIndex) * kSpineAttachmentLocalDepthStep;
			switch (slot->attachment->type)
			{
			case SP_ATTACHMENT_REGION:
			{
				spRegionAttachment* attachment = reinterpret_cast<spRegionAttachment*>(slot->attachment);
				const UINT32 baseVertex = static_cast<UINT32>(mesh.Vertices.size());
				const UINT32 indexStart = static_cast<UINT32>(mesh.Indices.size());
				const UINT32 skinVertexStart = static_cast<UINT32>(mesh.SkinVertices.size());
				AppendRegionSkinVertices(mesh, skeleton, slot, attachment, localZ);
				if (mesh.SkinVertices.size() > skinVertexStart)
				{
					if (bBuildCpuFallbackVertices)
						AppendSkinFallbackVertices(mesh, skinVertexStart, sourceScale);
					else
						AppendSkinPlaceholderVertices(mesh, skinVertexStart, sourceScale);
					AppendAttachmentIndices(mesh, baseVertex, kRegionQuadTriangles, 6);
					AppendSpineDrawRange(mesh, baseVertex, indexStart);
				}
				++mesh.RegionAttachmentCount;
				break;
			}
			case SP_ATTACHMENT_MESH:
			{
				spMeshAttachment* attachment = reinterpret_cast<spMeshAttachment*>(slot->attachment);
				const UINT32 baseVertex = static_cast<UINT32>(mesh.Vertices.size());
				const UINT32 indexStart = static_cast<UINT32>(mesh.Indices.size());
				const UINT32 skinVertexStart = static_cast<UINT32>(mesh.SkinVertices.size());
				AppendMeshSkinVertices(mesh, skeleton, slot, attachment, localZ);
				if (mesh.SkinVertices.size() > skinVertexStart)
				{
					if (bBuildCpuFallbackVertices)
						AppendSkinFallbackVertices(mesh, skinVertexStart, sourceScale);
					else
						AppendSkinPlaceholderVertices(mesh, skinVertexStart, sourceScale);
					AppendAttachmentIndices(mesh, baseVertex, attachment->triangles, attachment->trianglesCount);
					AppendSpineDrawRange(mesh, baseVertex, indexStart);
				}
				++mesh.MeshAttachmentCount;
				break;
			}
			case SP_ATTACHMENT_WEIGHTED_MESH:
			{
				spWeightedMeshAttachment* attachment = reinterpret_cast<spWeightedMeshAttachment*>(slot->attachment);
				const UINT32 baseVertex = static_cast<UINT32>(mesh.Vertices.size());
				const UINT32 indexStart = static_cast<UINT32>(mesh.Indices.size());
				const UINT32 skinVertexStart = static_cast<UINT32>(mesh.SkinVertices.size());
				AppendWeightedMeshSkinVertices(mesh, slot, attachment, localZ);
				if (mesh.SkinVertices.size() > skinVertexStart)
				{
					if (bBuildCpuFallbackVertices)
						AppendSkinFallbackVertices(mesh, skinVertexStart, sourceScale);
					else
						AppendSkinPlaceholderVertices(mesh, skinVertexStart, sourceScale);
					AppendAttachmentIndices(mesh, baseVertex, attachment->triangles, attachment->trianglesCount);
					AppendSpineDrawRange(mesh, baseVertex, indexStart);
				}
				++mesh.WeightedMeshAttachmentCount;
				break;
			}
			default:
				break;
			}
		}

		if (mesh.Vertices.empty())
		{
			mesh.BoundsMin = glm::vec3(0.0f);
			mesh.BoundsMax = glm::vec3(0.0f);
		}

		return mesh;
	}
}

Corona::ScriptSceneHandle Corona::CreateSpineSceneForScript(
	const std::wstring& assetPath,
	const std::string& animationName,
	float timeSeconds,
	float sourceScale)
{
	if (!renderBackend || assetPath.empty())
		return InvalidScriptSceneHandle;

	std::filesystem::path skeletonPath(assetPath);
	if (skeletonPath.is_relative())
		skeletonPath = GetAssetFullPath(assetPath.c_str());

	const std::wstring normalizedSkeletonPath = NormalizePathForKey(skeletonPath);
	skeletonPath = normalizedSkeletonPath;
	std::filesystem::path atlasPath = skeletonPath;
	atlasPath.replace_extension(L".atlas");

	const int sampleFrame120 = std::max(0, static_cast<int>(std::round(std::max(0.0f, timeSeconds) * 120.0f)));
	const float sampleTimeSeconds = static_cast<float>(sampleFrame120) / 120.0f;
	const float safeSourceScale = std::clamp(sourceScale, 0.0001f, 100.0f);
	const int quantizedScale = static_cast<int>(std::round(safeSourceScale * 100000.0f));
	const std::wstring key =
		L"spine://" + normalizedSkeletonPath +
		L"|anim=" + std::wstring(animationName.begin(), animationName.end()) +
		L"|frame120=" + std::to_wstring(sampleFrame120) +
		L"|scale=" + std::to_wstring(quantizedScale);

	const auto cachedIt = ScriptSceneByPath.find(key);
	if (cachedIt != ScriptSceneByPath.end())
		return cachedIt->second;

	if (GSpineRuntimeAssetCacheBackend != renderBackend.get())
	{
		GSpineRuntimeAssetCache.clear();
		GSpineRuntimeAssetCacheBackend = renderBackend.get();
	}

	if (!std::filesystem::exists(skeletonPath) || !std::filesystem::exists(atlasPath))
	{
		AppendCpuRuntimeTrace(L"[SpineComponent] missing skeleton or atlas: " + skeletonPath.wstring());
		return InvalidScriptSceneHandle;
	}

	SpineRuntimeAsset* runtimeAsset = nullptr;
	auto runtimeAssetIt = GSpineRuntimeAssetCache.find(normalizedSkeletonPath);
	if (runtimeAssetIt != GSpineRuntimeAssetCache.end())
	{
		runtimeAsset = runtimeAssetIt->second.get();
	}
	else
	{
		auto newRuntimeAsset = std::make_unique<SpineRuntimeAsset>();
		newRuntimeAsset->SkeletonPath = skeletonPath;
		newRuntimeAsset->AtlasPath = atlasPath;

		const std::string atlasPathUtf8 = ToUtf8Path(atlasPath);
		const std::string skeletonPathUtf8 = ToUtf8Path(skeletonPath);

		newRuntimeAsset->Atlas.reset(spAtlas_createFromFile(atlasPathUtf8.c_str(), nullptr));
		if (!newRuntimeAsset->Atlas)
		{
			AppendCpuRuntimeTrace(L"[SpineComponent] failed to load atlas: " + atlasPath.wstring());
			return InvalidScriptSceneHandle;
		}

		std::unique_ptr<spSkeletonJson, SpineSkeletonJsonDeleter> json(spSkeletonJson_create(newRuntimeAsset->Atlas.get()));
		if (!json)
		{
			AppendCpuRuntimeTrace(L"[SpineComponent] failed to create skeleton json reader");
			return InvalidScriptSceneHandle;
		}
		json->scale = 1.0f;

		newRuntimeAsset->SkeletonData.reset(spSkeletonJson_readSkeletonDataFile(json.get(), skeletonPathUtf8.c_str()));
		if (!newRuntimeAsset->SkeletonData)
		{
			const char* error = json->error ? json->error : "unknown";
			AppendCpuRuntimeTrace(L"[SpineComponent] failed to read skeleton: " + skeletonPath.wstring() + L" error=" + WidenAscii(error));
			return InvalidScriptSceneHandle;
		}

		newRuntimeAsset->DiffusePath = atlasPath.parent_path();
		if (newRuntimeAsset->Atlas->pages && newRuntimeAsset->Atlas->pages->name)
			newRuntimeAsset->DiffusePath /= std::filesystem::path(newRuntimeAsset->Atlas->pages->name);
		else
			newRuntimeAsset->DiffusePath = std::filesystem::path(skeletonPath).replace_extension(L".png");

		newRuntimeAsset->bDiffuseFileExists = std::filesystem::exists(newRuntimeAsset->DiffusePath);
		newRuntimeAsset->DiffuseTexture = renderBackend->CreateTextureFromFile(newRuntimeAsset->DiffusePath.wstring(), false);
		newRuntimeAsset->bDiffuseLoaded = newRuntimeAsset->DiffuseTexture != nullptr;
		if (!newRuntimeAsset->DiffuseTexture)
			newRuntimeAsset->DiffuseTexture = DefaultWhiteTex;

		runtimeAsset = newRuntimeAsset.get();
		GSpineRuntimeAssetCache[normalizedSkeletonPath] = std::move(newRuntimeAsset);
		AppendCpuRuntimeTrace(
			L"[SpineComponent] cached runtime asset path=" + normalizedSkeletonPath +
			L" atlas=" + atlasPath.wstring() +
			L" diffuse=" + runtimeAsset->DiffusePath.wstring() +
			L" diffuse_exists=" + std::to_wstring(runtimeAsset->bDiffuseFileExists ? 1 : 0) +
			L" diffuse_loaded=" + std::to_wstring(runtimeAsset->bDiffuseLoaded ? 1 : 0));
	}

	if (!runtimeAsset || !runtimeAsset->SkeletonData)
		return InvalidScriptSceneHandle;

	std::unique_ptr<spSkeleton, SpineSkeletonDeleter> skeleton(spSkeleton_create(runtimeAsset->SkeletonData.get()));
	if (!skeleton)
	{
		AppendCpuRuntimeTrace(L"[SpineComponent] failed to create skeleton instance");
		return InvalidScriptSceneHandle;
	}

	const char* selectedAnimation = SelectAnimationName(runtimeAsset->SkeletonData.get(), animationName);
	spSkeleton_setToSetupPose(skeleton.get());
	if (selectedAnimation)
	{
		spAnimation* animation = spSkeletonData_findAnimation(runtimeAsset->SkeletonData.get(), selectedAnimation);
		if (animation)
			spAnimation_apply(animation, skeleton.get(), 0.0f, sampleTimeSeconds, 1, nullptr, nullptr);
	}
	spSkeleton_updateWorldTransform(skeleton.get());

#if CORONA_PLATFORM_MOBILE
	const bool bRequestGpuSpineSkinning = false;
#else
	const bool bRequestGpuSpineSkinning = bEnableGpuSpineSkinning;
#endif
	SpineSampleMesh sampleMesh = BuildSpineSampleMesh(skeleton.get(), safeSourceScale, !bRequestGpuSpineSkinning);
	if (sampleMesh.Vertices.empty() || sampleMesh.Indices.empty())
	{
		AppendCpuRuntimeTrace(L"[SpineComponent] skeleton produced no drawable mesh: " + skeletonPath.wstring());
		return InvalidScriptSceneHandle;
	}

	auto material = std::make_shared<Material>();
	material->bHasAlpha = true;
	// Spine renders unlit. Give sprite textures a small post-tonemap headroom
	// boost so character art does not read darker than the source on DX12.
	material->BaseColorFactor = glm::vec4(1.22f, 1.22f, 1.22f, 1.0f);
	material->Diffuse = runtimeAsset->DiffuseTexture ? runtimeAsset->DiffuseTexture : DefaultWhiteTex;
	material->Normal = DefaultNormalTex;
	material->Roughness = DefaultRougnessTex;
	material->Metallic = DefaultBlackTex;

	Mesh* mesh = new Mesh;
	mesh->Owner = renderBackend.get();
	mesh->transform = glm::mat4x4(1.0f);
	mesh->bTransparent = true;
	mesh->bSpineMesh = true;
	mesh->NumVertices = static_cast<UINT32>(sampleMesh.Vertices.size());
	mesh->NumIndices = static_cast<UINT32>(sampleMesh.Indices.size());
	mesh->VertexStride = sizeof(SpineSampleVertex);
	mesh->IndexFormat = EIndexFormat::U32;
	mesh->Mat = material;
	mesh->Textures.push_back(material->Diffuse);
	mesh->Vb = renderBackend->CreateVertexBuffer(
		static_cast<UINT32>(sizeof(SpineSampleVertex) * sampleMesh.Vertices.size()),
		sizeof(SpineSampleVertex),
		sampleMesh.Vertices.data());
	mesh->Ib = renderBackend->CreateIndexBuffer(
		mesh->IndexFormat,
		static_cast<UINT32>(sizeof(UINT32) * sampleMesh.Indices.size()),
		sampleMesh.Indices.data());
	// Adreno Vulkan stalls hard when running the Spine compute skinning
	// pipeline. On mobile we skip the GPU skinning infrastructure entirely
	// and draw the CPU-skinned mesh->Vb through the standard vertex-
	// attribute path. Desktop keeps the compute path for performance.
#if !CORONA_PLATFORM_MOBILE
	if (bEnableGpuSpineSkinning &&
		sampleMesh.SkinVertices.size() == sampleMesh.Vertices.size() &&
		!sampleMesh.SkinVertices.empty() &&
		!sampleMesh.SkinInfluences.empty() &&
		!sampleMesh.SkinBones.empty())
	{
		mesh->GpuSpineInputVertices = renderBackend->CreateBuffer({
			static_cast<uint32_t>(sampleMesh.SkinVertices.size()),
			static_cast<uint32_t>(sizeof(SpineSkinInputVertex)),
			EInitialResourceState::ShaderRead,
			true,
			sampleMesh.SkinVertices.data(),
			EBufferShape::Structured });
		mesh->GpuSpineInfluences = renderBackend->CreateBuffer({
			static_cast<uint32_t>(sampleMesh.SkinInfluences.size()),
			static_cast<uint32_t>(sizeof(SpineSkinInfluence)),
			EInitialResourceState::ShaderRead,
			true,
			sampleMesh.SkinInfluences.data(),
			EBufferShape::Structured });
		mesh->GpuSpineBones = renderBackend->CreateBuffer({
			static_cast<uint32_t>(sampleMesh.SkinBones.size()),
			static_cast<uint32_t>(sizeof(SpineSkinBone)),
			EInitialResourceState::ShaderRead,
			true,
			sampleMesh.SkinBones.data(),
			EBufferShape::Structured });
		mesh->GpuSpineSkinnedVertices = renderBackend->CreateBuffer({
			static_cast<uint32_t>(sampleMesh.Vertices.size()),
			static_cast<uint32_t>(sizeof(SpineSampleVertex)),
			EInitialResourceState::ShaderRead,
			true,
			sampleMesh.Vertices.data(),
			EBufferShape::Structured });
		mesh->bGpuSpineSkinned =
			mesh->GpuSpineInputVertices &&
			mesh->GpuSpineInfluences &&
			mesh->GpuSpineBones &&
			mesh->GpuSpineSkinnedVertices;
		mesh->GpuSpineSkinningVertexCount = mesh->bGpuSpineSkinned ? mesh->NumVertices : 0;
		mesh->GpuSpineSkinningSourceScale = safeSourceScale;
	}
#endif
	mesh->CpuPositions.reserve(sampleMesh.Vertices.size());
	for (const SpineSampleVertex& vertex : sampleMesh.Vertices)
		mesh->CpuPositions.push_back(vertex.Position);
	mesh->CpuIndices = sampleMesh.Indices;

	if (!sampleMesh.Draws.empty())
	{
		mesh->Draws.reserve(sampleMesh.Draws.size());
		for (const SpineSampleMesh::DrawRange& drawRange : sampleMesh.Draws)
		{
			Mesh::DrawCall drawCall = {};
			drawCall.mat = material;
			drawCall.IndexStart = drawRange.IndexStart;
			drawCall.IndexCount = drawRange.IndexCount;
			drawCall.VertexBase = drawRange.VertexBase;
			drawCall.VertexCount = drawRange.VertexCount;
			mesh->Draws.push_back(drawCall);
		}
	}
	else
	{
		Mesh::DrawCall drawCall = {};
		drawCall.mat = material;
		drawCall.IndexStart = 0;
		drawCall.IndexCount = mesh->NumIndices;
		drawCall.VertexBase = 0;
		drawCall.VertexCount = mesh->NumVertices;
		mesh->Draws.push_back(drawCall);
	}

	auto scene = std::make_shared<Scene>();
	scene->Materials.push_back(material);
	scene->meshes.push_back(std::shared_ptr<Mesh>(mesh));
	scene->bHasBounds = true;
	scene->BoundsMin = sampleMesh.BoundsMin;
	scene->BoundsMax = sampleMesh.BoundsMax;

	ScriptSceneHandle handle = NextScriptSceneHandle++;
	if (handle == InvalidScriptSceneHandle)
		handle = NextScriptSceneHandle++;

	ScriptScenes[handle] = { scene, key, EPhysicsCollisionShape::TriangleMesh, glm::vec3(0.5f) };
	ScriptSceneByPath[key] = handle;

	const char* version = runtimeAsset->SkeletonData->version ? runtimeAsset->SkeletonData->version : "";
	AppendCpuRuntimeTrace(
		L"[SpineComponent] scene handle=" + std::to_wstring(handle) +
		L" path=" + normalizedSkeletonPath +
		L" animation=" + WidenAscii(selectedAnimation) +
		L" time=" + std::to_wstring(sampleTimeSeconds) +
		L" version=" + WidenAscii(version) +
		L" vertices=" + std::to_wstring(mesh->NumVertices) +
		L" triangles=" + std::to_wstring(mesh->NumIndices / 3) +
		L" draws=" + std::to_wstring(mesh->Draws.size()) +
		L" region=" + std::to_wstring(sampleMesh.RegionAttachmentCount) +
		L" mesh=" + std::to_wstring(sampleMesh.MeshAttachmentCount) +
		L" weightedmesh=" + std::to_wstring(sampleMesh.WeightedMeshAttachmentCount) +
		L" gpu_skin=" + std::to_wstring(mesh->bGpuSpineSkinned ? 1 : 0) +
		L" cpu_fallback_skin=" + std::to_wstring(sampleMesh.CpuFallbackVertexCount > 0 ? 1 : 0) +
		L" cpu_fallback_vertices=" + std::to_wstring(sampleMesh.CpuFallbackVertexCount) +
		L" gpu_placeholder_vertices=" + std::to_wstring(sampleMesh.GpuPlaceholderVertexCount) +
		L" skin_influences=" + std::to_wstring(sampleMesh.SkinInfluences.size()) +
		L" bones=" + std::to_wstring(sampleMesh.SkinBones.size()) +
		L" diffuse=" + runtimeAsset->DiffusePath.wstring() +
		L" diffuse_exists=" + std::to_wstring(runtimeAsset->bDiffuseFileExists ? 1 : 0) +
		L" diffuse_loaded=" + std::to_wstring(runtimeAsset->bDiffuseLoaded ? 1 : 0));

	return handle;
}
