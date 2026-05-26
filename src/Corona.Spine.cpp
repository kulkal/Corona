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
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
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

	// Live Spine: per-handle persistent skeleton + animation state. Stores
	// the persistent VB/IB capacities so UpdateLiveSpineForScript can
	// memcpy in place without reallocating each frame.
	struct SpineLiveState
	{
		std::unique_ptr<spSkeleton, SpineSkeletonDeleter> Skeleton;
		SpineRuntimeAsset* RuntimeAsset = nullptr;
		std::string AnimationName;
		spAnimation* Animation = nullptr;
		float ElapsedTime = 0.0f;
		float SourceScale = 1.0f;
		Corona::ScriptSceneHandle SceneHandle = 0;
		std::shared_ptr<Scene> Scene_;
		Mesh* MeshPtr = nullptr;
		uint32_t MaxVertexBytes = 0;
		uint32_t MaxIndexBytes = 0;
	};
	std::unordered_map<Corona::ScriptSceneHandle, std::unique_ptr<SpineLiveState>> GSpineLiveStates;

	constexpr int kRegionQuadTriangles[] = { 0, 1, 2, 2, 3, 0 };
	constexpr float kSpineAttachmentLocalDepthStep = 0.02f;

	using SpineHighResClock = std::chrono::steady_clock;

	double MillisecondsBetween(const SpineHighResClock::time_point& start, const SpineHighResClock::time_point& end)
	{
		const std::chrono::duration<double, std::milli> delta = end - start;
		return delta.count();
	}

	struct SpineClipScopedTimer
	{
		double* Accumulator;
		SpineHighResClock::time_point Start;
		SpineClipScopedTimer(double* accumulator)
			: Accumulator(accumulator), Start(SpineHighResClock::now()) {}
		~SpineClipScopedTimer()
		{
			if (Accumulator)
				*Accumulator += MillisecondsBetween(Start, SpineHighResClock::now());
		}
	};

	std::wstring FormatMillisecondsFixed(double value)
	{
		std::wostringstream stream;
		stream << std::fixed << std::setprecision(3) << value;
		return stream.str();
	}

	// Phase 2: CPU-side cached geometry produced by BuildSpineSampleMesh.
	// Stored on the CPU only — the doc forbids creating per-frame GPU
	// vertex/index buffers during the cache miss path itself.
	struct SpineClipFrameCacheEntry
	{
		std::vector<SpineSampleVertex> Vertices;
		std::vector<UINT32> Indices;
		std::vector<SpineSampleMesh::DrawRange> Draws;
		glm::vec3 BoundsMin = glm::vec3(0.0f);
		glm::vec3 BoundsMax = glm::vec3(0.0f);
		UINT32 RegionAttachmentCount = 0;
		UINT32 MeshAttachmentCount = 0;
		UINT32 WeightedMeshAttachmentCount = 0;
		UINT64 ApproxByteSize = 0;
		UINT64 LastUsedFrame = 0;
	};

	UINT64 EstimateClipEntryByteSize(const SpineSampleMesh& mesh)
	{
		UINT64 bytes = 0;
		bytes += static_cast<UINT64>(mesh.Vertices.size()) * sizeof(SpineSampleVertex);
		bytes += static_cast<UINT64>(mesh.Indices.size()) * sizeof(UINT32);
		bytes += static_cast<UINT64>(mesh.Draws.size()) * sizeof(SpineSampleMesh::DrawRange);
		bytes += sizeof(SpineClipFrameCacheEntry);
		return bytes;
	}

	struct SpineClipFrameCache
	{
		// LRU eviction policy: keep newest-used entries at the front of the
		// list; the back is the eviction candidate. The map points into the
		// list for O(1) move-to-front on lookup.
		std::list<std::wstring> LruOrder;
		std::unordered_map<std::wstring, std::pair<std::shared_ptr<SpineClipFrameCacheEntry>, std::list<std::wstring>::iterator>> Entries;
		UINT64 CurrentBytes = 0;
		UINT64 MemoryBudgetBytes = 64ull * 1024ull * 1024ull; // 64 MiB default; sufficient for ~50 platformer chars
		UINT64 InsertCounter = 0;
		UINT32 EvictionCount = 0;

		std::shared_ptr<SpineClipFrameCacheEntry> Lookup(const std::wstring& key)
		{
			auto it = Entries.find(key);
			if (it == Entries.end())
				return nullptr;
			LruOrder.splice(LruOrder.begin(), LruOrder, it->second.second);
			it->second.second = LruOrder.begin();
			it->second.first->LastUsedFrame = ++InsertCounter;
			return it->second.first;
		}

		void Touch(const std::wstring& key)
		{
			auto it = Entries.find(key);
			if (it == Entries.end())
				return;
			LruOrder.splice(LruOrder.begin(), LruOrder, it->second.second);
			it->second.second = LruOrder.begin();
		}

		std::shared_ptr<SpineClipFrameCacheEntry> Insert(const std::wstring& key, std::shared_ptr<SpineClipFrameCacheEntry> entry)
		{
			auto existing = Entries.find(key);
			if (existing != Entries.end())
			{
				CurrentBytes -= existing->second.first->ApproxByteSize;
				existing->second.first = entry;
				CurrentBytes += entry->ApproxByteSize;
				LruOrder.splice(LruOrder.begin(), LruOrder, existing->second.second);
				existing->second.second = LruOrder.begin();
				return entry;
			}

			LruOrder.push_front(key);
			Entries.emplace(key, std::make_pair(entry, LruOrder.begin()));
			CurrentBytes += entry->ApproxByteSize;
			entry->LastUsedFrame = ++InsertCounter;
			return entry;
		}

		UINT32 EvictUntilUnderBudget()
		{
			UINT32 evicted = 0;
			while (CurrentBytes > MemoryBudgetBytes && !LruOrder.empty())
			{
				const std::wstring victimKey = LruOrder.back();
				auto victim = Entries.find(victimKey);
				if (victim != Entries.end())
				{
					CurrentBytes -= victim->second.first->ApproxByteSize;
					Entries.erase(victim);
				}
				LruOrder.pop_back();
				++evicted;
				++EvictionCount;
			}
			return evicted;
		}

		size_t Size() const { return Entries.size(); }
		UINT64 Bytes() const { return CurrentBytes; }
		UINT64 Budget() const { return MemoryBudgetBytes; }
		void SetBudget(UINT64 bytes)
		{
			MemoryBudgetBytes = bytes;
			EvictUntilUnderBudget();
		}
		void Clear()
		{
			Entries.clear();
			LruOrder.clear();
			CurrentBytes = 0;
			InsertCounter = 0;
		}
	};

	SpineClipFrameCache GSpineClipFrameCache;
	void* GSpineClipFrameCacheBackend = nullptr;

	void EnsureSpineClipCacheBackendMatches(void* backendPtr)
	{
		if (GSpineClipFrameCacheBackend == backendPtr)
			return;
		GSpineClipFrameCache.Clear();
		GSpineClipFrameCacheBackend = backendPtr;
	}

	void PopulateClipEntryFromSampleMesh(SpineClipFrameCacheEntry& entry, const SpineSampleMesh& mesh)
	{
		entry.Vertices = mesh.Vertices;
		entry.Indices = mesh.Indices;
		entry.Draws = mesh.Draws;
		entry.BoundsMin = mesh.BoundsMin;
		entry.BoundsMax = mesh.BoundsMax;
		entry.RegionAttachmentCount = static_cast<UINT32>(std::max(0, mesh.RegionAttachmentCount));
		entry.MeshAttachmentCount = static_cast<UINT32>(std::max(0, mesh.MeshAttachmentCount));
		entry.WeightedMeshAttachmentCount = static_cast<UINT32>(std::max(0, mesh.WeightedMeshAttachmentCount));
		entry.ApproxByteSize = EstimateClipEntryByteSize(mesh);
	}

	std::wstring BuildSpineClipCacheKey(
		const std::wstring& normalizedSkeletonPath,
		const std::string& animationName,
		int sampleFrame120,
		int quantizedScale)
	{
		return
			L"spine://" + normalizedSkeletonPath +
			L"|anim=" + std::wstring(animationName.begin(), animationName.end()) +
			L"|frame120=" + std::to_wstring(sampleFrame120) +
			L"|scale=" + std::to_wstring(quantizedScale);
	}

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

		// Indices are emitted GLOBALLY — each value already includes the
		// attachment's `baseVertex` offset. Paired DrawRanges therefore set
		// `VertexBase = 0`, which is what lets the Phase 4 pass collapse
		// adjacent same-material attachments into a single DrawIndexed
		// without re-binding offsets.
		//
		// Only emit one winding per triangle. The GBuffer pipeline runs
		// with backface culling disabled, so two-sided visibility is already
		// covered; emitting both windings would create coplanar triangles
		// at identical depth and trigger z-fight flicker (e.g. alternating
		// eye disappearance on Spine faces).
		for (int i = 0; i + 2 < triangleIndexCount; i += 3)
		{
			const UINT32 a = static_cast<UINT32>(triangles[i]) + baseVertex;
			const UINT32 b = static_cast<UINT32>(triangles[i + 1]) + baseVertex;
			const UINT32 c = static_cast<UINT32>(triangles[i + 2]) + baseVertex;
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

		// Indices are now globally addressed (see AppendAttachmentIndices),
		// so per-draw VertexBase is 0. We still record `baseVertex` /
		// `vertexCount` as informational metadata for the cache entry.
		outMesh.Draws.push_back({ indexStart, indexCount, baseVertex, vertexCount });
	}

	// Phase 4: collapse a SpineSampleMesh's per-attachment Draws into a
	// minimal set of merged Draws. All attachments of a single Spine
	// character share the same material (atlas+blend), so adjacent draws
	// can fuse — typically yielding exactly one DrawIndexed per character
	// instead of ~22 (one per slot/attachment). Spine drawOrder is
	// preserved automatically because attachments are appended into the
	// index buffer in slot order.
	void CollapseSpineDrawsForBatching(SpineSampleMesh& mesh)
	{
		if (mesh.Draws.empty())
			return;

		std::vector<SpineSampleMesh::DrawRange> merged;
		merged.reserve(mesh.Draws.size());
		SpineSampleMesh::DrawRange current = mesh.Draws.front();
		current.VertexBase = 0; // global indices
		for (size_t i = 1; i < mesh.Draws.size(); ++i)
		{
			const SpineSampleMesh::DrawRange& next = mesh.Draws[i];
			const bool bContiguous = (current.IndexStart + current.IndexCount) == next.IndexStart;
			if (bContiguous)
			{
				// Extend the open range; vertex span widens to include the
				// new attachment's vertices.
				current.IndexCount += next.IndexCount;
				const UINT32 nextVertexEnd = next.VertexBase + next.VertexCount;
				const UINT32 currentVertexEnd = current.VertexBase + current.VertexCount;
				const UINT32 endVertex = std::max(currentVertexEnd, nextVertexEnd);
				current.VertexCount = endVertex - current.VertexBase;
			}
			else
			{
				merged.push_back(current);
				current = next;
				current.VertexBase = 0;
			}
		}
		merged.push_back(current);
		mesh.Draws = std::move(merged);
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

		// Phase 4: collapse per-attachment draws into a minimal set. Spine
		// characters share one atlas across all attachments, so this
		// typically yields one DrawIndexed per character (down from ~22).
		CollapseSpineDrawsForBatching(mesh);

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

	++SpineStats.InstancesEvaluated;
	++SpineStatsCallsSinceReport;

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
	const std::wstring key = BuildSpineClipCacheKey(normalizedSkeletonPath, animationName, sampleFrame120, quantizedScale);

	const auto cachedIt = ScriptSceneByPath.find(key);
	if (cachedIt != ScriptSceneByPath.end())
	{
		++SpineStats.ScriptSceneCacheHits;
		// Promote the matching clip-cache entry too so the LRU reflects real
		// demand. A scene-cache hit conceptually implies a clip hit, but the
		// clip cache might have been evicted independently — that's OK.
		EnsureSpineClipCacheBackendMatches(renderBackend.get());
		GSpineClipFrameCache.Touch(key);
		if (SpineStatsReportIntervalCalls > 0 && SpineStatsCallsSinceReport >= SpineStatsReportIntervalCalls)
			DumpSpineFrameStatsToTrace(L"interval");
		return cachedIt->second;
	}

	if (GSpineRuntimeAssetCacheBackend != renderBackend.get())
	{
		GSpineRuntimeAssetCache.clear();
		GSpineRuntimeAssetCacheBackend = renderBackend.get();
	}
	EnsureSpineClipCacheBackendMatches(renderBackend.get());

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

		{
			SpineClipScopedTimer atlasTimer(&SpineStats.AtlasLoadMs);
			newRuntimeAsset->Atlas.reset(spAtlas_createFromFile(atlasPathUtf8.c_str(), nullptr));
		}
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

		{
			SpineClipScopedTimer skeletonTimer(&SpineStats.SkeletonReadMs);
			newRuntimeAsset->SkeletonData.reset(spSkeletonJson_readSkeletonDataFile(json.get(), skeletonPathUtf8.c_str()));
		}
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

#if CORONA_PLATFORM_MOBILE
	const bool bRequestGpuSpineSkinning = false;
#else
	const bool bRequestGpuSpineSkinning = bEnableGpuSpineSkinning;
#endif

	const char* selectedAnimation = SelectAnimationName(runtimeAsset->SkeletonData.get(), animationName);

	// Phase 2: try the CPU clip cache before re-running animation evaluation
	// and CPU skinning. The cache is only consulted when GPU skinning is off
	// because the GPU path also needs skin/influence/bone buffers, which the
	// CPU cache does not retain.
	std::shared_ptr<SpineClipFrameCacheEntry> clipEntry;
	if (!bRequestGpuSpineSkinning)
		clipEntry = GSpineClipFrameCache.Lookup(key);

	SpineSampleMesh sampleMesh;
	UINT32 slotCountForStats = 0;
	if (clipEntry)
	{
		++SpineStats.ClipCacheHits;
		sampleMesh.Vertices = clipEntry->Vertices;
		sampleMesh.Indices = clipEntry->Indices;
		sampleMesh.Draws = clipEntry->Draws;
		sampleMesh.BoundsMin = clipEntry->BoundsMin;
		sampleMesh.BoundsMax = clipEntry->BoundsMax;
		sampleMesh.RegionAttachmentCount = static_cast<int>(clipEntry->RegionAttachmentCount);
		sampleMesh.MeshAttachmentCount = static_cast<int>(clipEntry->MeshAttachmentCount);
		sampleMesh.WeightedMeshAttachmentCount = static_cast<int>(clipEntry->WeightedMeshAttachmentCount);
	}
	else
	{
		++SpineStats.ClipCacheMisses;
		std::unique_ptr<spSkeleton, SpineSkeletonDeleter> skeleton;
		{
			SpineClipScopedTimer animTimer(&SpineStats.AnimationEvaluationMs);
			skeleton.reset(spSkeleton_create(runtimeAsset->SkeletonData.get()));
			if (!skeleton)
			{
				AppendCpuRuntimeTrace(L"[SpineComponent] failed to create skeleton instance");
				return InvalidScriptSceneHandle;
			}
			++SpineStats.SkeletonInstancesBuilt;
			spSkeleton_setToSetupPose(skeleton.get());
			if (selectedAnimation)
			{
				spAnimation* animation = spSkeletonData_findAnimation(runtimeAsset->SkeletonData.get(), selectedAnimation);
				if (animation)
					spAnimation_apply(animation, skeleton.get(), 0.0f, sampleTimeSeconds, 1, nullptr, nullptr);
			}
			spSkeleton_updateWorldTransform(skeleton.get());
		}

		slotCountForStats = static_cast<UINT32>(std::max(0, skeleton ? skeleton->slotsCount : 0));
		{
			SpineClipScopedTimer meshTimer(&SpineStats.MeshBuildMs);
			sampleMesh = BuildSpineSampleMesh(skeleton.get(), safeSourceScale, !bRequestGpuSpineSkinning);
		}
		SpineStats.VerticesGenerated += static_cast<UINT32>(sampleMesh.Vertices.size());
		SpineStats.IndicesGenerated += static_cast<UINT32>(sampleMesh.Indices.size());
		SpineStats.DrawRangesBuilt += static_cast<UINT32>(sampleMesh.Draws.size());

		if (sampleMesh.Vertices.empty() || sampleMesh.Indices.empty())
		{
			AppendCpuRuntimeTrace(L"[SpineComponent] skeleton produced no drawable mesh: " + skeletonPath.wstring());
			return InvalidScriptSceneHandle;
		}

		// Insert into the clip cache (CPU-only payload), then evict if we
		// exceed the configured memory budget. This satisfies the doc's
		// nonblocking miss path: no CreateVertexBuffer or CreateIndexBuffer
		// is performed inside the cache itself.
		if (!bRequestGpuSpineSkinning)
		{
			auto entry = std::make_shared<SpineClipFrameCacheEntry>();
			PopulateClipEntryFromSampleMesh(*entry, sampleMesh);
			GSpineClipFrameCache.Insert(key, entry);
			const UINT32 evicted = GSpineClipFrameCache.EvictUntilUnderBudget();
			SpineStats.ClipCacheEvictions += evicted;
		}
	}

	SpineStats.SlotsProcessed += slotCountForStats;
	SpineStats.ClipCacheEntries = static_cast<UINT32>(GSpineClipFrameCache.Size());
	SpineStats.ClipCacheBytes = GSpineClipFrameCache.Bytes();
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
	{
		SpineClipScopedTimer uploadTimer(&SpineStats.GpuUploadMs);
		// Phase 3: use the generic upload-heap-only buffer path. This skips
		// the per-call ExecuteCommandList + WaitGPU stall that was
		// dominating the runtime CPU cost in the baseline (~3.66 ms/call
		// from Phase 1 measurements). The CPU-skinning Spine pipeline
		// binds VB/IB through BindMeshBuffers without requiring
		// DEFAULT-heap residency, so a HOST_VISIBLE / UPLOAD-heap buffer
		// suffices.
		mesh->Vb = renderBackend->CreateUploadVertexBuffer(
			static_cast<UINT32>(sizeof(SpineSampleVertex) * sampleMesh.Vertices.size()),
			sizeof(SpineSampleVertex),
			sampleMesh.Vertices.data());
		mesh->Ib = renderBackend->CreateUploadIndexBuffer(
			mesh->IndexFormat,
			static_cast<UINT32>(sizeof(UINT32) * sampleMesh.Indices.size()),
			sampleMesh.Indices.data());
	}
	SpineStats.RuntimeGpuBufferCreations += 2;
	// Upload-heap path no longer issues ExecuteCommandList + WaitGPU on
	// DX12. Mobile/Vulkan still uses the staged path until that backend
	// gets its HOST_VISIBLE shortcut.
#if CORONA_PLATFORM_MOBILE
	++SpineStats.RuntimeGpuUploadStalls;
#endif
	// Adreno Vulkan stalls hard when running the Spine compute *pre-pass*,
	// so mobile only uploads the inputs the VS-inline path needs and
	// leaves GpuSpineSkinnedVertices null. Desktop still creates the
	// compute output buffer for the compute-skinning fast path.
	if (bEnableGpuSpineSkinning &&
		sampleMesh.SkinVertices.size() == sampleMesh.Vertices.size() &&
		!sampleMesh.SkinVertices.empty() &&
		!sampleMesh.SkinInfluences.empty() &&
		!sampleMesh.SkinBones.empty())
	{
		SpineClipScopedTimer uploadTimer(&SpineStats.GpuUploadMs);
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
#if !CORONA_PLATFORM_MOBILE
		mesh->GpuSpineSkinnedVertices = renderBackend->CreateBuffer({
			static_cast<uint32_t>(sampleMesh.Vertices.size()),
			static_cast<uint32_t>(sizeof(SpineSampleVertex)),
			EInitialResourceState::ShaderRead,
			true,
			sampleMesh.Vertices.data(),
			EBufferShape::Structured });
#endif
		mesh->bGpuSpineSkinned =
			mesh->GpuSpineInputVertices &&
			mesh->GpuSpineInfluences &&
			mesh->GpuSpineBones;
#if !CORONA_PLATFORM_MOBILE
		mesh->bGpuSpineSkinned = mesh->bGpuSpineSkinned && mesh->GpuSpineSkinnedVertices;
#endif
		mesh->GpuSpineSkinningVertexCount = mesh->bGpuSpineSkinned ? mesh->NumVertices : 0;
		mesh->GpuSpineSkinningSourceScale = safeSourceScale;
		SpineStats.RuntimeGpuBufferCreations += 4;
	}
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
		L" clip_cache_hit=" + std::to_wstring(clipEntry ? 1 : 0) +
		L" clip_cache_entries=" + std::to_wstring(GSpineClipFrameCache.Size()) +
		L" clip_cache_bytes=" + std::to_wstring(GSpineClipFrameCache.Bytes()) +
		L" diffuse=" + runtimeAsset->DiffusePath.wstring() +
		L" diffuse_exists=" + std::to_wstring(runtimeAsset->bDiffuseFileExists ? 1 : 0) +
		L" diffuse_loaded=" + std::to_wstring(runtimeAsset->bDiffuseLoaded ? 1 : 0));

	if (SpineStatsReportIntervalCalls > 0 && SpineStatsCallsSinceReport >= SpineStatsReportIntervalCalls)
		DumpSpineFrameStatsToTrace(L"interval");

	return handle;
}

namespace
{
	SpineRuntimeAsset* EnsureSpineRuntimeAsset(Corona* corona, const std::filesystem::path& skeletonPath)
	{
		const std::wstring normalizedSkeletonPath = NormalizePathForKey(skeletonPath);
		auto it = GSpineRuntimeAssetCache.find(normalizedSkeletonPath);
		if (it != GSpineRuntimeAssetCache.end())
			return it->second.get();
		// Fall back to the full create path which populates the cache as
		// a side effect.
		(void)corona;
		return nullptr;
	}
}

Corona::ScriptSceneHandle Corona::CreateLiveSpineForScript(
	const std::wstring& assetPath,
	const std::string& animationName,
	float sourceScale)
{
	if (!renderBackend || assetPath.empty())
		return InvalidScriptSceneHandle;

	// Seed the runtime-asset cache by going through the normal scene
	// builder once at time=0 (cheap clip-cache hit on subsequent
	// CreateLiveSpine calls of the same asset). We immediately drop the
	// returned scene handle — only the cached SpineRuntimeAsset is
	// reused. This keeps live-Spine self-contained without duplicating
	// the atlas/skeleton-data loading code.
	ScriptSceneHandle seedHandle = CreateSpineSceneForScript(assetPath, animationName, 0.0f, sourceScale);
	if (seedHandle == InvalidScriptSceneHandle)
		return InvalidScriptSceneHandle;

	std::filesystem::path skeletonPath(assetPath);
	if (skeletonPath.is_relative())
		skeletonPath = GetAssetFullPath(assetPath.c_str());
	const std::wstring normalizedSkeletonPath = NormalizePathForKey(skeletonPath);
	SpineRuntimeAsset* runtimeAsset = nullptr;
	auto runtimeAssetIt = GSpineRuntimeAssetCache.find(normalizedSkeletonPath);
	if (runtimeAssetIt != GSpineRuntimeAssetCache.end())
		runtimeAsset = runtimeAssetIt->second.get();
	if (!runtimeAsset || !runtimeAsset->SkeletonData)
		return InvalidScriptSceneHandle;

	const float safeSourceScale = std::clamp(sourceScale, 0.0001f, 100.0f);

	auto liveState = std::make_unique<SpineLiveState>();
	liveState->RuntimeAsset = runtimeAsset;
	liveState->AnimationName = animationName;
	liveState->SourceScale = safeSourceScale;
	liveState->ElapsedTime = 0.0f;
	liveState->Skeleton.reset(spSkeleton_create(runtimeAsset->SkeletonData.get()));
	if (!liveState->Skeleton)
		return InvalidScriptSceneHandle;
	spSkeleton_setToSetupPose(liveState->Skeleton.get());
	liveState->Animation = !animationName.empty()
		? spSkeletonData_findAnimation(runtimeAsset->SkeletonData.get(), animationName.c_str())
		: nullptr;
	if (liveState->Animation)
		spAnimation_apply(liveState->Animation, liveState->Skeleton.get(), 0.0f, 0.0f, 1, nullptr, nullptr);
	spSkeleton_updateWorldTransform(liveState->Skeleton.get());

	SpineSampleMesh sampleMesh = BuildSpineSampleMesh(liveState->Skeleton.get(), safeSourceScale, true);
	if (sampleMesh.Vertices.empty() || sampleMesh.Indices.empty())
		return InvalidScriptSceneHandle;

	// Capacity headroom: real-time skinning may grow/shrink the active
	// attachment vertex count slightly. 2x the initial pose covers
	// typical Spine walk/idle cycles without resizing.
	const uint32_t vbCapacity = static_cast<uint32_t>(sizeof(SpineSampleVertex) * sampleMesh.Vertices.size()) * 2u;
	const uint32_t ibCapacity = static_cast<uint32_t>(sizeof(UINT32) * sampleMesh.Indices.size()) * 2u;

	auto material = std::make_shared<Material>();
	material->bHasAlpha = true;
	material->BaseColorFactor = glm::vec4(1.22f, 1.22f, 1.22f, 1.0f);
	material->Diffuse = runtimeAsset->DiffuseTexture ? runtimeAsset->DiffuseTexture : DefaultWhiteTex;
	material->Normal = DefaultNormalTex;
	material->Roughness = DefaultRougnessTex;
	material->Metallic = DefaultBlackTex;

	auto* mesh = new Mesh;
	mesh->Owner = renderBackend.get();
	mesh->transform = glm::mat4x4(1.0f);
	mesh->bTransparent = true;
	mesh->bSpineMesh = true;
	mesh->NumVertices = static_cast<UINT>(sampleMesh.Vertices.size());
	mesh->NumIndices = static_cast<UINT>(sampleMesh.Indices.size());
	mesh->VertexStride = sizeof(SpineSampleVertex);
	mesh->IndexFormat = EIndexFormat::U32;
	mesh->Mat = material;
	mesh->Textures.push_back(material->Diffuse);
	mesh->Vb = renderBackend->CreateUploadVertexBuffer(vbCapacity, sizeof(SpineSampleVertex), nullptr);
	mesh->Ib = renderBackend->CreateUploadIndexBuffer(EIndexFormat::U32, ibCapacity, nullptr);
	if (!mesh->Vb || !mesh->Ib)
	{
		delete mesh;
		return InvalidScriptSceneHandle;
	}
	renderBackend->UpdateUploadVertexBuffer(mesh->Vb.get(),
		sampleMesh.Vertices.data(),
		static_cast<uint32_t>(sizeof(SpineSampleVertex) * sampleMesh.Vertices.size()));
	renderBackend->UpdateUploadIndexBuffer(mesh->Ib.get(),
		sampleMesh.Indices.data(),
		static_cast<uint32_t>(sizeof(UINT32) * sampleMesh.Indices.size()));

	Mesh::DrawCall drawCall = {};
	drawCall.mat = material;
	drawCall.IndexStart = 0;
	drawCall.IndexCount = static_cast<UINT>(sampleMesh.Indices.size());
	drawCall.VertexBase = 0;
	drawCall.VertexCount = static_cast<UINT>(sampleMesh.Vertices.size());
	mesh->Draws.push_back(drawCall);

	auto scene = std::make_shared<Scene>();
	scene->Materials.push_back(material);
	scene->meshes.push_back(std::shared_ptr<Mesh>(mesh));
	scene->bHasBounds = true;
	scene->BoundsMin = sampleMesh.BoundsMin;
	scene->BoundsMax = sampleMesh.BoundsMax;

	ScriptSceneHandle handle = NextScriptSceneHandle++;
	if (handle == InvalidScriptSceneHandle)
		handle = NextScriptSceneHandle++;
	const std::wstring key = L"live:" + std::to_wstring(handle);
	ScriptScenes[handle] = { scene, key, EPhysicsCollisionShape::TriangleMesh, glm::vec3(0.5f) };

	liveState->SceneHandle = handle;
	liveState->Scene_ = scene;
	liveState->MeshPtr = mesh;
	liveState->MaxVertexBytes = vbCapacity;
	liveState->MaxIndexBytes = ibCapacity;
	GSpineLiveStates[handle] = std::move(liveState);

	return handle;
}

bool Corona::UpdateLiveSpineForScript(ScriptSceneHandle handle, float deltaSeconds)
{
	auto it = GSpineLiveStates.find(handle);
	if (it == GSpineLiveStates.end() || !it->second)
		return false;
	SpineLiveState& live = *it->second;
	if (!live.Skeleton || !live.MeshPtr || !live.MeshPtr->Vb || !live.MeshPtr->Ib)
		return false;

	live.ElapsedTime += deltaSeconds;
	if (live.Animation)
	{
		const float duration = live.Animation->duration > 0.0f ? live.Animation->duration : 1.0f;
		float t = std::fmod(live.ElapsedTime, duration);
		if (t < 0.0f) t += duration;
		spSkeleton_setToSetupPose(live.Skeleton.get());
		spAnimation_apply(live.Animation, live.Skeleton.get(), 0.0f, t, 1, nullptr, nullptr);
	}
	spSkeleton_updateWorldTransform(live.Skeleton.get());

	SpineSampleMesh sampleMesh = BuildSpineSampleMesh(live.Skeleton.get(), live.SourceScale, true);
	if (sampleMesh.Vertices.empty() || sampleMesh.Indices.empty())
		return false;

	const uint32_t vbBytes = static_cast<uint32_t>(sizeof(SpineSampleVertex) * sampleMesh.Vertices.size());
	const uint32_t ibBytes = static_cast<uint32_t>(sizeof(UINT32) * sampleMesh.Indices.size());
	if (vbBytes > live.MaxVertexBytes || ibBytes > live.MaxIndexBytes)
	{
		// Topology grew beyond reserved capacity; drop this tick rather
		// than crash. The next tick will retry; benchmark animations
		// (walk/idle) are stable in vertex count so this is rare.
		return false;
	}
	renderBackend->UpdateUploadVertexBuffer(live.MeshPtr->Vb.get(), sampleMesh.Vertices.data(), vbBytes);
	renderBackend->UpdateUploadIndexBuffer(live.MeshPtr->Ib.get(), sampleMesh.Indices.data(), ibBytes);
	live.MeshPtr->NumVertices = static_cast<UINT>(sampleMesh.Vertices.size());
	live.MeshPtr->NumIndices = static_cast<UINT>(sampleMesh.Indices.size());
	if (!live.MeshPtr->Draws.empty())
	{
		live.MeshPtr->Draws[0].IndexCount = static_cast<UINT>(sampleMesh.Indices.size());
		live.MeshPtr->Draws[0].VertexCount = static_cast<UINT>(sampleMesh.Vertices.size());
	}
	return true;
}

bool Corona::DestroyLiveSpineForScript(ScriptSceneHandle handle)
{
	auto it = GSpineLiveStates.find(handle);
	if (it == GSpineLiveStates.end())
		return false;
	GSpineLiveStates.erase(it);
	ScriptScenes.erase(handle);
	return true;
}

void Corona::SpineFrameStats::Reset()
{
	*this = SpineFrameStats{};
}

void Corona::ResetSpineFrameStats()
{
	SpineStatsLastReport = SpineStats;
	SpineStats.Reset();
	SpineStatsCallsSinceReport = 0;
}

void Corona::DumpSpineFrameStatsToTrace(const wchar_t* reasonTag)
{
	const std::wstring tag = reasonTag ? std::wstring(reasonTag) : std::wstring(L"manual");
	AppendCpuRuntimeTrace(
		L"[SpineStats] reason=" + tag +
		L" calls=" + std::to_wstring(SpineStats.InstancesEvaluated) +
		L" scene_hits=" + std::to_wstring(SpineStats.ScriptSceneCacheHits) +
		L" clip_hits=" + std::to_wstring(SpineStats.ClipCacheHits) +
		L" clip_misses=" + std::to_wstring(SpineStats.ClipCacheMisses) +
		L" clip_evictions=" + std::to_wstring(SpineStats.ClipCacheEvictions) +
		L" clip_entries=" + std::to_wstring(SpineStats.ClipCacheEntries) +
		L" clip_bytes=" + std::to_wstring(SpineStats.ClipCacheBytes) +
		L" skel_built=" + std::to_wstring(SpineStats.SkeletonInstancesBuilt) +
		L" slots=" + std::to_wstring(SpineStats.SlotsProcessed) +
		L" vertices=" + std::to_wstring(SpineStats.VerticesGenerated) +
		L" indices=" + std::to_wstring(SpineStats.IndicesGenerated) +
		L" draw_ranges=" + std::to_wstring(SpineStats.DrawRangesBuilt) +
		L" gpu_buf_creates=" + std::to_wstring(SpineStats.RuntimeGpuBufferCreations) +
		L" gpu_upload_stalls=" + std::to_wstring(SpineStats.RuntimeGpuUploadStalls) +
		L" prewarm_hits=" + std::to_wstring(SpineStats.PrewarmCallsHit) +
		L" prewarm_misses=" + std::to_wstring(SpineStats.PrewarmCallsMiss) +
		L" atlas_ms=" + FormatMillisecondsFixed(SpineStats.AtlasLoadMs) +
		L" skel_read_ms=" + FormatMillisecondsFixed(SpineStats.SkeletonReadMs) +
		L" anim_eval_ms=" + FormatMillisecondsFixed(SpineStats.AnimationEvaluationMs) +
		L" mesh_build_ms=" + FormatMillisecondsFixed(SpineStats.MeshBuildMs) +
		L" skinning_ms=" + FormatMillisecondsFixed(SpineStats.SkinningMs) +
		L" gpu_upload_ms=" + FormatMillisecondsFixed(SpineStats.GpuUploadMs));
	ResetSpineFrameStats();
}

bool Corona::PrewarmSpineClipFrameForScript(
	const std::wstring& assetPath,
	const std::string& animationName,
	float timeSeconds,
	float sourceScale)
{
	if (!renderBackend || assetPath.empty())
		return false;

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
	const std::wstring key = BuildSpineClipCacheKey(normalizedSkeletonPath, animationName, sampleFrame120, quantizedScale);

	EnsureSpineClipCacheBackendMatches(renderBackend.get());

	if (auto existing = GSpineClipFrameCache.Lookup(key))
	{
		++SpineStats.PrewarmCallsHit;
		return true;
	}
	++SpineStats.PrewarmCallsMiss;

	if (GSpineRuntimeAssetCacheBackend != renderBackend.get())
	{
		GSpineRuntimeAssetCache.clear();
		GSpineRuntimeAssetCacheBackend = renderBackend.get();
	}

	if (!std::filesystem::exists(skeletonPath) || !std::filesystem::exists(atlasPath))
	{
		AppendCpuRuntimeTrace(L"[SpineComponent] prewarm missing skeleton or atlas: " + skeletonPath.wstring());
		return false;
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

		{
			SpineClipScopedTimer atlasTimer(&SpineStats.AtlasLoadMs);
			newRuntimeAsset->Atlas.reset(spAtlas_createFromFile(atlasPathUtf8.c_str(), nullptr));
		}
		if (!newRuntimeAsset->Atlas)
			return false;

		std::unique_ptr<spSkeletonJson, SpineSkeletonJsonDeleter> json(spSkeletonJson_create(newRuntimeAsset->Atlas.get()));
		if (!json)
			return false;
		json->scale = 1.0f;

		{
			SpineClipScopedTimer skelTimer(&SpineStats.SkeletonReadMs);
			newRuntimeAsset->SkeletonData.reset(spSkeletonJson_readSkeletonDataFile(json.get(), skeletonPathUtf8.c_str()));
		}
		if (!newRuntimeAsset->SkeletonData)
			return false;

		newRuntimeAsset->DiffusePath = atlasPath.parent_path();
		if (newRuntimeAsset->Atlas->pages && newRuntimeAsset->Atlas->pages->name)
			newRuntimeAsset->DiffusePath /= std::filesystem::path(newRuntimeAsset->Atlas->pages->name);
		else
			newRuntimeAsset->DiffusePath = std::filesystem::path(skeletonPath).replace_extension(L".png");
		newRuntimeAsset->bDiffuseFileExists = std::filesystem::exists(newRuntimeAsset->DiffusePath);
		// Prewarm intentionally skips texture upload — the caller will load
		// the texture lazily via CreateSpineSceneForScript on first render.

		runtimeAsset = newRuntimeAsset.get();
		GSpineRuntimeAssetCache[normalizedSkeletonPath] = std::move(newRuntimeAsset);
	}

	if (!runtimeAsset || !runtimeAsset->SkeletonData)
		return false;

	const char* selectedAnimation = SelectAnimationName(runtimeAsset->SkeletonData.get(), animationName);

	std::unique_ptr<spSkeleton, SpineSkeletonDeleter> skeleton;
	{
		SpineClipScopedTimer animTimer(&SpineStats.AnimationEvaluationMs);
		skeleton.reset(spSkeleton_create(runtimeAsset->SkeletonData.get()));
		if (!skeleton)
			return false;
		++SpineStats.SkeletonInstancesBuilt;
		spSkeleton_setToSetupPose(skeleton.get());
		if (selectedAnimation)
		{
			spAnimation* animation = spSkeletonData_findAnimation(runtimeAsset->SkeletonData.get(), selectedAnimation);
			if (animation)
				spAnimation_apply(animation, skeleton.get(), 0.0f, sampleTimeSeconds, 1, nullptr, nullptr);
		}
		spSkeleton_updateWorldTransform(skeleton.get());
	}

	SpineSampleMesh sampleMesh;
	{
		SpineClipScopedTimer meshTimer(&SpineStats.MeshBuildMs);
		// Always build the CPU-skinned fallback variant: the cache stores
		// final CPU-resolved positions so a future sprite batcher can stream
		// them directly without re-running the skinning step.
		sampleMesh = BuildSpineSampleMesh(skeleton.get(), safeSourceScale, true);
	}
	SpineStats.VerticesGenerated += static_cast<UINT32>(sampleMesh.Vertices.size());
	SpineStats.IndicesGenerated += static_cast<UINT32>(sampleMesh.Indices.size());
	SpineStats.DrawRangesBuilt += static_cast<UINT32>(sampleMesh.Draws.size());
	SpineStats.SlotsProcessed += static_cast<UINT32>(std::max(0, skeleton ? skeleton->slotsCount : 0));

	if (sampleMesh.Vertices.empty() || sampleMesh.Indices.empty())
		return false;

	auto entry = std::make_shared<SpineClipFrameCacheEntry>();
	PopulateClipEntryFromSampleMesh(*entry, sampleMesh);
	GSpineClipFrameCache.Insert(key, entry);
	SpineStats.ClipCacheEvictions += GSpineClipFrameCache.EvictUntilUnderBudget();
	SpineStats.ClipCacheEntries = static_cast<UINT32>(GSpineClipFrameCache.Size());
	SpineStats.ClipCacheBytes = GSpineClipFrameCache.Bytes();
	return true;
}

size_t Corona::GetSpineClipFrameCacheMemoryBytes() const
{
	return static_cast<size_t>(GSpineClipFrameCache.Bytes());
}

size_t Corona::GetSpineClipFrameCacheEntryCount() const
{
	return GSpineClipFrameCache.Size();
}

size_t Corona::GetSpineClipFrameCacheMemoryBudgetBytes() const
{
	return static_cast<size_t>(GSpineClipFrameCache.Budget());
}

void Corona::SetSpineClipFrameCacheMemoryBudgetBytes(size_t budgetBytes)
{
	GSpineClipFrameCache.SetBudget(static_cast<UINT64>(budgetBytes));
	SpineStats.ClipCacheEntries = static_cast<UINT32>(GSpineClipFrameCache.Size());
	SpineStats.ClipCacheBytes = GSpineClipFrameCache.Bytes();
}

void Corona::ResetSpineClipFrameCache()
{
	GSpineClipFrameCache.Clear();
	SpineStats.ClipCacheEntries = 0;
	SpineStats.ClipCacheBytes = 0;
}
