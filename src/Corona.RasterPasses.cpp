//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "stdafx.h"
#include "Corona.h"
#include "ParticleSystem.h"
#include "TerrainComponent.h"
#include "VulkanBackend.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <unordered_map>

#include "glm/gtc/matrix_transform.hpp"

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	using GBufferProfileClock = std::chrono::steady_clock;
	constexpr float kRenderWorldCullingCellSize = 4096.0f;
	constexpr uint32_t kParallelCullingIndexBuildThreshold = 1024u;
	constexpr uint32_t kParallelCellFrustumCullThreshold = 512u;
	constexpr uint32_t kParallelFrustumCullThreshold = 2048u;
	// Batched objects keep their last occlusion result for a while. Refreshing
	// them requires pulling objects out of the bindless batch for real draws,
	// so keep that trickle tiny and favor stable, approximate culling.
	constexpr uint64_t kMaxGBufferOcclusionSkipFrames = 120u;
	constexpr uint64_t kBatchedGBufferOcclusionRefreshFrames = 240u;
	constexpr uint32_t kBatchedGBufferOcclusionRefreshQueryBudget = 64u;

	enum class EFrustumAabbRelation : uint8_t
	{
		Outside,
		Intersect,
		Inside,
	};

	using FrustumPlaneArray = std::array<glm::vec4, 6>;

	FrustumPlaneArray BuildFrustumPlanes(const glm::mat4x4& viewProj)
	{
		auto row = [](const glm::mat4x4& matrix, int index)
		{
			return glm::vec4(matrix[0][index], matrix[1][index], matrix[2][index], matrix[3][index]);
		};
		const glm::vec4 row0 = row(viewProj, 0);
		const glm::vec4 row1 = row(viewProj, 1);
		const glm::vec4 row2 = row(viewProj, 2);
		const glm::vec4 row3 = row(viewProj, 3);
		return
		{
			row3 + row0,
			row3 - row0,
			row3 + row1,
			row3 - row1,
			row3 + row2,
			row3 - row2,
		};
	}

	EFrustumAabbRelation ClassifyAabbAgainstFrustumPlanes(
		const FrustumPlaneArray& planes,
		const glm::vec3& boundsMin,
		const glm::vec3& boundsMax)
	{
		const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
		const glm::vec3 extents = glm::max((boundsMax - boundsMin) * 0.5f, glm::vec3(0.0f));
		bool bFullyInside = true;
		for (const glm::vec4& plane : planes)
		{
			const glm::vec3 normal(plane.x, plane.y, plane.z);
			const float distance = glm::dot(normal, center) + plane.w;
			const float radius = glm::dot(glm::abs(normal), extents);
			if (distance + radius < 0.0f)
				return EFrustumAabbRelation::Outside;
			if (distance - radius < 0.0f)
				bFullyInside = false;
		}
		return bFullyInside ? EFrustumAabbRelation::Inside : EFrustumAabbRelation::Intersect;
	}

	uint64_t PackCullingCellCoord(int32_t cellX, int32_t cellZ)
	{
		return (static_cast<uint64_t>(static_cast<uint32_t>(cellX)) << 32u) |
			static_cast<uint32_t>(cellZ);
	}

	bool IsGBufferObjectBatchProfileEnabled()
	{
		static const bool enabled = []
		{
			const char* nriValue = std::getenv("CORONA_NRI_RECORD_PROFILE");
			const char* gbufferValue = std::getenv("CORONA_GBUFFER_BATCH_PROFILE");
			const bool nriEnabled = nriValue && nriValue[0] != '\0' && nriValue[0] != '0';
			const bool gbufferEnabled = gbufferValue && gbufferValue[0] != '\0' && gbufferValue[0] != '0';
			return nriEnabled || gbufferEnabled;
		}();
		return enabled;
	}

	bool IsGBufferCullingProfileEnabled()
	{
		static const bool enabled = []
		{
			const char* nriValue = std::getenv("CORONA_NRI_RECORD_PROFILE");
			const char* cullValue = std::getenv("CORONA_GBUFFER_CULL_PROFILE");
			const bool nriEnabled = nriValue && nriValue[0] != '\0' && nriValue[0] != '0';
			const bool cullEnabled = cullValue && cullValue[0] != '\0' && cullValue[0] != '0';
			return nriEnabled || cullEnabled;
		}();
		return enabled;
	}

	bool IsGBufferPassProfileEnabled()
	{
		static const bool enabled = []
		{
			auto isEnabled = [](const char* value)
			{
				return value && value[0] != '\0' && value[0] != '0';
			};
			return
				isEnabled(std::getenv("CORONA_NRI_RECORD_PROFILE")) ||
				isEnabled(std::getenv("CORONA_GBUFFER_PASS_PROFILE")) ||
				isEnabled(std::getenv("CORONA_GBUFFER_BATCH_PROFILE")) ||
				isEnabled(std::getenv("CORONA_GBUFFER_CULL_PROFILE"));
		}();
		return enabled;
	}

	int GetGBufferCullingModeOverride()
	{
		const char* value = std::getenv("CORONA_GBUFFER_CULL_MODE");
		if (!value || value[0] == '\0')
			return -1;
		if (value[0] == '0' || value[0] == 'c' || value[0] == 'C')
			return 0;
		if (value[0] == '1' || value[0] == 'g' || value[0] == 'G')
			return 1;
		return -1;
	}

	double GBufferProfileElapsedMs(GBufferProfileClock::time_point begin)
	{
		return std::chrono::duration<double, std::milli>(GBufferProfileClock::now() - begin).count();
	}

	void GBufferProfileAdd(bool enabled, double& totalMs, uint64_t& count, GBufferProfileClock::time_point begin)
	{
		if (!enabled)
			return;
		totalMs += GBufferProfileElapsedMs(begin);
		++count;
	}

	struct GBufferProfileScope
	{
		bool Enabled = false;
		GBufferProfileClock::time_point Begin{};
		double& TotalMs;
		uint64_t& Count;

		GBufferProfileScope(bool enabled, double& totalMs, uint64_t& count)
			: Enabled(enabled)
			, Begin(enabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{})
			, TotalMs(totalMs)
			, Count(count)
		{
		}

		~GBufferProfileScope()
		{
			if (Enabled)
			{
				TotalMs += GBufferProfileElapsedMs(Begin);
				++Count;
			}
		}
	};

	struct GBufferObjectBatchProfile
	{
		uint64_t LastFrame = UINT64_MAX;
		uint32_t Frames = 0;
		uint64_t Calls = 0;
		uint64_t Objects = 0;
		uint64_t DrawRecords = 0;
		uint64_t Materials = 0;
		uint64_t Geometries = 0;
		uint64_t IndirectArgs = 0;
		double TotalMs = 0.0;
		double BuildMs = 0.0;
		double AllocMs = 0.0;
		double MaterialAllocMs = 0.0;
		double GeometryAllocMs = 0.0;
		double DrawRecordAllocMs = 0.0;
		double IndirectAllocMs = 0.0;
		double BindMs = 0.0;
		double DrawMs = 0.0;
		uint64_t TotalCount = 0;
		uint64_t BuildCount = 0;
		uint64_t AllocCount = 0;
		uint64_t MaterialAllocCount = 0;
		uint64_t GeometryAllocCount = 0;
		uint64_t DrawRecordAllocCount = 0;
		uint64_t IndirectAllocCount = 0;
		uint64_t BindCount = 0;
		uint64_t DrawCount = 0;

		void Reset()
		{
			*this = GBufferObjectBatchProfile{};
		}

		void FlushIfReady()
		{
			if (Frames < 120)
				return;
			const double frames = static_cast<double>(std::max(1u, Frames));
			auto avgMs = [frames](double totalMs) { return totalMs / frames; };
			auto perFrame = [frames](uint64_t count) { return static_cast<double>(count) / frames; };
			auto metric = [&](std::wstringstream& ss, const wchar_t* name, double ms, uint64_t count)
			{
				ss << L", " << name << L"=" << std::fixed << std::setprecision(3) << avgMs(ms)
					<< L"ms/" << std::setprecision(1) << perFrame(count) << L"c";
			};

			std::wstringstream ss;
			ss << L"[GBufferObjectBatchProfile] frames=" << Frames
				<< L", calls/frame=" << std::fixed << std::setprecision(1) << perFrame(Calls)
				<< L", objects/frame=" << perFrame(Objects)
				<< L", drawRecords/frame=" << perFrame(DrawRecords)
				<< L", materials/frame=" << perFrame(Materials)
				<< L", geometries/frame=" << perFrame(Geometries)
				<< L", indirectArgs/frame=" << perFrame(IndirectArgs);
			metric(ss, L"total", TotalMs, TotalCount);
			metric(ss, L"build", BuildMs, BuildCount);
			metric(ss, L"alloc", AllocMs, AllocCount);
			metric(ss, L"matAlloc", MaterialAllocMs, MaterialAllocCount);
			metric(ss, L"geoAlloc", GeometryAllocMs, GeometryAllocCount);
			metric(ss, L"drawRecAlloc", DrawRecordAllocMs, DrawRecordAllocCount);
			metric(ss, L"indirectAlloc", IndirectAllocMs, IndirectAllocCount);
			metric(ss, L"bind", BindMs, BindCount);
			metric(ss, L"draw", DrawMs, DrawCount);
			AppendCpuRuntimeTrace(ss.str());
			Reset();
		}

		void BeginFrame(uint64_t frame)
		{
			if (LastFrame == frame)
				return;
			FlushIfReady();
			LastFrame = frame;
			++Frames;
		}
	};

	GBufferObjectBatchProfile& GetGBufferObjectBatchProfile()
	{
		static GBufferObjectBatchProfile profile;
		return profile;
	}

	struct GBufferCullingProfile
	{
		uint64_t LastFrame = UINT64_MAX;
		uint32_t Frames = 0;
		uint64_t Objects = 0;
		uint64_t BoundsTests = 0;
		uint64_t FrustumTests = 0;
		uint64_t CandidateScans = 0;
		uint64_t ObjectScans = 0;
		double TotalMs = 0.0;
		double CandidateScanMs = 0.0;
		double ObjectScanMs = 0.0;
		double BoundsMs = 0.0;
		double FrustumMs = 0.0;
		uint64_t TotalCount = 0;
		uint64_t CandidateScanCount = 0;
		uint64_t ObjectScanCount = 0;
		uint64_t BoundsCount = 0;
		uint64_t FrustumCount = 0;

		void Reset()
		{
			*this = GBufferCullingProfile{};
		}

		void FlushIfReady()
		{
			if (Frames < 120)
				return;
			const double frames = static_cast<double>(std::max(1u, Frames));
			auto avgMs = [frames](double totalMs) { return totalMs / frames; };
			auto perFrame = [frames](uint64_t count) { return static_cast<double>(count) / frames; };
			auto metric = [&](std::wstringstream& ss, const wchar_t* name, double ms, uint64_t count)
			{
				ss << L", " << name << L"=" << std::fixed << std::setprecision(3) << avgMs(ms)
					<< L"ms/" << std::setprecision(1) << perFrame(count) << L"c";
			};

			std::wstringstream ss;
			ss << L"[GBufferCullingProfile] frames=" << Frames
				<< L", objects/frame=" << std::fixed << std::setprecision(1) << perFrame(Objects)
				<< L", bounds/frame=" << perFrame(BoundsTests)
				<< L", frustum/frame=" << perFrame(FrustumTests)
				<< L", candidateScans/frame=" << perFrame(CandidateScans)
				<< L", objectScans/frame=" << perFrame(ObjectScans);
			metric(ss, L"total", TotalMs, TotalCount);
			metric(ss, L"candidateScan", CandidateScanMs, CandidateScanCount);
			metric(ss, L"objectScan", ObjectScanMs, ObjectScanCount);
			metric(ss, L"bounds", BoundsMs, BoundsCount);
			metric(ss, L"frustum", FrustumMs, FrustumCount);
			AppendCpuRuntimeTrace(ss.str());
			Reset();
		}

		void BeginFrame(uint64_t frame)
		{
			if (LastFrame == frame)
				return;
			FlushIfReady();
			LastFrame = frame;
			++Frames;
		}
	};

	GBufferCullingProfile& GetGBufferCullingProfile()
	{
		static GBufferCullingProfile profile;
		return profile;
	}

	struct GBufferPassProfile
	{
		uint64_t LastFrame = UINT64_MAX;
		uint32_t Frames = 0;
		uint64_t TotalObjects = 0;
		uint64_t VisibleObjects = 0;
		uint64_t FrustumCulledObjects = 0;
		uint64_t OcclusionCulledObjects = 0;
		uint64_t SpatialCandidateObjects = 0;
		uint64_t BindlessDraws = 0;
		uint64_t StaticDraws = 0;
		uint64_t Queries = 0;
		double TotalMs = 0.0;
		double PreTransitionMs = 0.0;
		double ClearMs = 0.0;
		double SkinningMs = 0.0;
		double TargetSetupMs = 0.0;
		double PrepareCullMs = 0.0;
		double GatherVisibleMs = 0.0;
		double ClusterDrawMs = 0.0;
		double ClassifyMs = 0.0;
		double DrawSubmitMs = 0.0;
		double FinishCullMs = 0.0;
		double PostTransitionMs = 0.0;
		uint64_t TotalCount = 0;
		uint64_t PreTransitionCount = 0;
		uint64_t ClearCount = 0;
		uint64_t SkinningCount = 0;
		uint64_t TargetSetupCount = 0;
		uint64_t PrepareCullCount = 0;
		uint64_t GatherVisibleCount = 0;
		uint64_t ClusterDrawCount = 0;
		uint64_t ClassifyCount = 0;
		uint64_t DrawSubmitCount = 0;
		uint64_t FinishCullCount = 0;
		uint64_t PostTransitionCount = 0;

		void Reset()
		{
			*this = GBufferPassProfile{};
		}

		void FlushIfReady()
		{
			if (Frames < 120)
				return;
			const double frames = static_cast<double>(std::max(1u, Frames));
			auto avgMs = [frames](double totalMs) { return totalMs / frames; };
			auto perFrame = [frames](uint64_t count) { return static_cast<double>(count) / frames; };
			auto metric = [&](std::wstringstream& ss, const wchar_t* name, double ms, uint64_t count)
			{
				ss << L", " << name << L"=" << std::fixed << std::setprecision(3) << avgMs(ms)
					<< L"ms/" << std::setprecision(1) << perFrame(count) << L"c";
			};

			std::wstringstream ss;
			ss << L"[GBufferPassProfile] frames=" << Frames
				<< L", visible/frame=" << std::fixed << std::setprecision(1) << perFrame(VisibleObjects)
				<< L"/" << perFrame(TotalObjects)
				<< L", frustum/frame=" << perFrame(FrustumCulledObjects)
				<< L", occlusion/frame=" << perFrame(OcclusionCulledObjects)
				<< L", spatialCandidates/frame=" << perFrame(SpatialCandidateObjects)
				<< L", bindlessDraws/frame=" << perFrame(BindlessDraws)
				<< L", staticDraws/frame=" << perFrame(StaticDraws)
				<< L", queries/frame=" << perFrame(Queries);
			metric(ss, L"total", TotalMs, TotalCount);
			metric(ss, L"preTransition", PreTransitionMs, PreTransitionCount);
			metric(ss, L"clear", ClearMs, ClearCount);
			metric(ss, L"skinning", SkinningMs, SkinningCount);
			metric(ss, L"targetSetup", TargetSetupMs, TargetSetupCount);
			metric(ss, L"prepareCull", PrepareCullMs, PrepareCullCount);
			metric(ss, L"gatherVisible", GatherVisibleMs, GatherVisibleCount);
			metric(ss, L"clusterDraw", ClusterDrawMs, ClusterDrawCount);
			metric(ss, L"classify", ClassifyMs, ClassifyCount);
			metric(ss, L"drawSubmit", DrawSubmitMs, DrawSubmitCount);
			metric(ss, L"finishCull", FinishCullMs, FinishCullCount);
			metric(ss, L"postTransition", PostTransitionMs, PostTransitionCount);
			AppendCpuRuntimeTrace(ss.str());
			Reset();
		}

		void BeginFrame(uint64_t frame)
		{
			if (LastFrame == frame)
				return;
			FlushIfReady();
			LastFrame = frame;
			++Frames;
		}
	};

	GBufferPassProfile& GetGBufferPassProfile()
	{
		static GBufferPassProfile profile;
		return profile;
	}

	constexpr uint32_t kGBufferBindlessTextureRegisterSpace = 10;
	constexpr uint32_t kGBufferMaterialRecordRegister = 13;
	constexpr uint32_t kGBufferDrawRecordRegister = 15;
	constexpr uint32_t kGBufferLegacyStaticVertexStride = 44;

	struct GBufferMaterialRecord
	{
		UINT32 AlbedoTextureIndex = RHI_INVALID_BINDLESS_INDEX;
		UINT32 NormalTextureIndex = RHI_INVALID_BINDLESS_INDEX;
		UINT32 RoughnessTextureIndex = RHI_INVALID_BINDLESS_INDEX;
		UINT32 MetallicTextureIndex = RHI_INVALID_BINDLESS_INDEX;
	};

	struct GBufferMaterialKey
	{
		Texture* Albedo = nullptr;
		Texture* Normal = nullptr;
		Texture* Roughness = nullptr;
		Texture* Metallic = nullptr;

		bool operator==(const GBufferMaterialKey& other) const
		{
			return Albedo == other.Albedo &&
				Normal == other.Normal &&
				Roughness == other.Roughness &&
				Metallic == other.Metallic;
		}
	};

	struct GBufferMaterialTable
	{
		std::vector<GBufferMaterialKey> Keys;
		std::vector<GBufferMaterialRecord> Records;
		std::shared_ptr<Buffer> Buffer;
	};

	struct GBufferGeometryRecord
	{
		UINT32 VertexBufferIndex = RHI_INVALID_BINDLESS_INDEX;
		UINT32 IndexBufferIndex = RHI_INVALID_BINDLESS_INDEX;
		UINT32 VertexStride = 0;
		UINT32 IndexStride = 0;
	};

	struct GBufferDrawRecord
	{
		glm::vec4 WorldMatrixRow0 = glm::vec4(0.0f);
		glm::vec4 WorldMatrixRow1 = glm::vec4(0.0f);
		glm::vec4 WorldMatrixRow2 = glm::vec4(0.0f);
		glm::vec4 WorldMatrixRow3 = glm::vec4(0.0f);
		glm::vec4 BaseColorFactor = glm::vec4(1.0f);
		glm::vec2 RougnessMetalic = glm::vec2(1.0f, 0.0f);
		UINT32 bOverrideRougnessMetallic = 0;
		UINT32 bTwoSidedLighting = 0;
		UINT32 bUnlitMaterial = 0;
		UINT32 bGrassMesh = 0;
		UINT32 bTerrainMesh = 0;
		UINT32 bExcludeFromDeformSphere = 0;
		UINT32 GBufferMaterialIndex = 0;
		UINT32 GBufferGeometryIndex = 0;
		UINT32 GBufferIndexStart = 0;
		INT32 GBufferVertexBase = 0;
	};
	static_assert(sizeof(GBufferDrawRecord) == 128, "GBufferDrawRecord must match GBufferCommon.hlsli.");

	struct GBufferGeometryKey
	{
		VertexBuffer* Vertex = nullptr;
		IndexBuffer* Index = nullptr;
		UINT32 VertexStride = 0;
		UINT32 IndexStride = 0;

		bool operator==(const GBufferGeometryKey& other) const
		{
			return Vertex == other.Vertex &&
				Index == other.Index &&
				VertexStride == other.VertexStride &&
				IndexStride == other.IndexStride;
		}
	};

	struct GBufferStaticBatchKey
	{
		const Scene* ScenePtr = nullptr;
		float Roughness = 1.0f;
		float Metallic = 0.0f;
		bool OverrideRoughnessMetallic = false;

		bool operator==(const GBufferStaticBatchKey& rhs) const
		{
			return ScenePtr == rhs.ScenePtr &&
				Roughness == rhs.Roughness &&
				Metallic == rhs.Metallic &&
				OverrideRoughnessMetallic == rhs.OverrideRoughnessMetallic;
		}
	};

	struct GBufferStaticBatchKeyHash
	{
		size_t operator()(const GBufferStaticBatchKey& key) const
		{
			size_t seed = std::hash<const Scene*>{}(key.ScenePtr);
			auto combine = [&seed](size_t value)
			{
				seed ^= value + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
			};
			combine(std::hash<float>{}(key.Roughness));
			combine(std::hash<float>{}(key.Metallic));
			combine(std::hash<bool>{}(key.OverrideRoughnessMetallic));
			return seed;
		}
	};

	struct CachedGBufferStaticObjectDrawRange
	{
		uint32_t FirstDraw = 0;
		uint32_t DrawCount = 0;
		uint32_t OpaqueDrawCount = 0;
		uint32_t AlphaDrawCount = 0;
	};
	static_assert(sizeof(CachedGBufferStaticObjectDrawRange) == 16, "CachedGBufferStaticObjectDrawRange must match GBufferCullCS.hlsl.");

	struct CachedGBufferStaticDrawInfo
	{
		uint32_t DrawRecordIndex = 0;
		uint32_t IndexCount = 0;
		bool bTransparent = false;
	};

	struct GBufferGpuCachedDrawInfo
	{
		uint32_t DrawRecordIndex = 0;
		uint32_t IndexCount = 0;
		uint32_t Flags = 0;
		uint32_t Pad = 0;
	};
	static_assert(sizeof(GBufferGpuCachedDrawInfo) == 16, "GBufferGpuCachedDrawInfo must match GBufferCullCS.hlsl.");

	struct GBufferGpuCullConstant
	{
		uint32_t CandidateCount = 0;
		uint32_t ObjectRangeCount = 0;
		uint32_t DrawInfoCount = 0;
		uint32_t MaxOutputDraws = 0;
	};
	static_assert(sizeof(GBufferGpuCullConstant) == 16, "GBufferGpuCullConstant must match GBufferCullCS.hlsl.");

	struct CachedGBufferStaticDrawTable
	{
		IRenderBackend* Backend = nullptr;
		uint64_t Generation = UINT64_MAX;
		uint64_t DefaultsHash = 0;
		uint32_t ObjectCount = 0;
		uint32_t MaterialCount = 0;
		uint32_t GeometryCount = 0;
		uint32_t DrawRecordCount = 0;
		std::shared_ptr<Buffer> MaterialBuffer;
		std::shared_ptr<Buffer> GeometryBuffer;
		std::shared_ptr<Buffer> DrawRecordBuffer;
		std::shared_ptr<Buffer> GpuObjectRangeBuffer;
		std::shared_ptr<Buffer> GpuDrawInfoBuffer;
		std::shared_ptr<Buffer> GpuOpaqueIndirectArgsBuffer;
		std::shared_ptr<Buffer> GpuAlphaIndirectArgsBuffer;
		std::shared_ptr<Buffer> GpuIndirectCountBuffer;
		EResourceState GpuOpaqueIndirectArgsState = EResourceState::ShaderRead;
		EResourceState GpuAlphaIndirectArgsState = EResourceState::ShaderRead;
		EResourceState GpuIndirectCountState = EResourceState::ShaderRead;
		uint64_t GpuCullBufferCreateFrame = UINT64_MAX;
		std::vector<CachedGBufferStaticObjectDrawRange> ObjectRanges;
		std::vector<CachedGBufferStaticDrawInfo> Draws;

		void Reset()
		{
			Backend = nullptr;
			Generation = UINT64_MAX;
			DefaultsHash = 0;
			ObjectCount = 0;
			MaterialCount = 0;
			GeometryCount = 0;
			DrawRecordCount = 0;
			MaterialBuffer.reset();
			GeometryBuffer.reset();
			DrawRecordBuffer.reset();
			GpuObjectRangeBuffer.reset();
			GpuDrawInfoBuffer.reset();
			GpuOpaqueIndirectArgsBuffer.reset();
			GpuAlphaIndirectArgsBuffer.reset();
			GpuIndirectCountBuffer.reset();
			GpuOpaqueIndirectArgsState = EResourceState::ShaderRead;
			GpuAlphaIndirectArgsState = EResourceState::ShaderRead;
			GpuIndirectCountState = EResourceState::ShaderRead;
			GpuCullBufferCreateFrame = UINT64_MAX;
			ObjectRanges.clear();
			Draws.clear();
		}
	};

	CachedGBufferStaticDrawTable& GetCachedGBufferStaticDrawTable(const Corona* owner)
	{
		static std::unordered_map<const Corona*, CachedGBufferStaticDrawTable> caches;
		return caches[owner];
	}

	void ResetCachedGBufferStaticDrawTable(const Corona* owner)
	{
		GetCachedGBufferStaticDrawTable(owner).Reset();
	}

	bool BuildGBufferIndirectDrawArguments(
		uint32_t drawRecordIndex,
		uint32_t indexCount,
		DrawIndirectArguments& outArgs,
		std::wstring* failureReason = nullptr)
	{
		outArgs = {};
		if (indexCount == 0)
			return true;
		(void)failureReason;

		outArgs.VertexCountPerInstance = indexCount;
		outArgs.InstanceCount = 1;
		outArgs.StartVertexLocation = 0;
		outArgs.StartInstanceLocation = drawRecordIndex;
		return true;
	}

	struct GBufferGeometryTable
	{
		std::vector<GBufferGeometryKey> Keys;
		std::vector<GBufferGeometryRecord> Records;
		std::shared_ptr<Buffer> Buffer;
	};

	void HashCombineGBufferMaterial(uint64_t& seed, uint64_t value)
	{
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
	}

	struct GBufferSceneTopologyCounts
	{
		uint64_t Hash = 1469598103934665603ull;
		uint32_t MeshCount = 0;
		uint32_t DrawCount = 0;
	};

	GBufferSceneTopologyCounts CountGBufferSceneTopology(const Scene* scene)
	{
		GBufferSceneTopologyCounts counts{};
		if (!scene)
			return counts;
		counts.MeshCount = static_cast<uint32_t>(scene->meshes.size());
		HashCombineGBufferMaterial(counts.Hash, counts.MeshCount);
		for (const std::shared_ptr<Mesh>& mesh : scene->meshes)
		{
			HashCombineGBufferMaterial(counts.Hash, reinterpret_cast<uintptr_t>(mesh.get()));
			if (!mesh)
				continue;
			const uint32_t meshDrawCount = static_cast<uint32_t>(mesh->Draws.size());
			counts.DrawCount += meshDrawCount;
			HashCombineGBufferMaterial(counts.Hash, meshDrawCount);
			HashCombineGBufferMaterial(counts.Hash, reinterpret_cast<uintptr_t>(mesh->Draws.data()));
			HashCombineGBufferMaterial(counts.Hash, reinterpret_cast<uintptr_t>(mesh->Vb.get()));
			HashCombineGBufferMaterial(counts.Hash, reinterpret_cast<uintptr_t>(mesh->Ib.get()));
			HashCombineGBufferMaterial(counts.Hash, mesh->VertexStride);
			HashCombineGBufferMaterial(counts.Hash, static_cast<uint32_t>(mesh->IndexFormat));
		}
		return counts;
	}

	bool SupportsGBufferBindlessMaterials(IRenderBackend* backend)
	{
		if (!backend)
			return false;
		const RenderBackendCapabilities capabilities = backend->GetCapabilities();
		return capabilities.SupportsBindlessTextures && capabilities.SupportsRuntimeDescriptorArrays;
	}

	bool SupportsGBufferBindlessGeometry(IRenderBackend* backend)
	{
		if (!backend)
			return false;
		static const bool bDisableGBufferBindlessGeometry =
			std::getenv("CORONA_DISABLE_GBUFFER_BINDLESS_GEOMETRY") != nullptr;
		static bool bLoggedDisableGBufferBindlessGeometry = false;
		if (bDisableGBufferBindlessGeometry)
		{
			if (!bLoggedDisableGBufferBindlessGeometry)
			{
				AppendCpuRuntimeTrace(L"[GBufferBindless] bindless geometry disabled by CORONA_DISABLE_GBUFFER_BINDLESS_GEOMETRY");
				bLoggedDisableGBufferBindlessGeometry = true;
			}
			return false;
		}
		const RenderBackendCapabilities capabilities = backend->GetCapabilities();
		return capabilities.SupportsBindlessBuffers && capabilities.SupportsRuntimeDescriptorArrays;
	}

	bool ShouldDisableNriGBufferObjectBatch(IRenderBackend* backend)
	{
		if (!backend || backend->GetAPI() != ERenderBackendAPI::NRI)
			return false;
		static const bool bDisableNriGBufferObjectBatch =
			std::getenv("CORONA_NRI_DISABLE_GBUFFER_OBJECT_BATCH") != nullptr;
		static bool bLoggedDisableNriGBufferObjectBatch = false;
		if (bDisableNriGBufferObjectBatch && !bLoggedDisableNriGBufferObjectBatch)
		{
			AppendCpuRuntimeTrace(L"[GBufferObjectBatch] NRI object batch disabled by CORONA_NRI_DISABLE_GBUFFER_OBJECT_BATCH");
			bLoggedDisableNriGBufferObjectBatch = true;
		}
		return bDisableNriGBufferObjectBatch;
	}

	bool IsGBufferStaticBindlessGeometryEligible(const Mesh& mesh, std::wstring* reason = nullptr)
	{
		auto fail = [&](const wchar_t* message) -> bool
		{
			if (reason)
				*reason = message;
			return false;
		};

		if (!mesh.Vb || !mesh.Ib)
			return fail(L"missing VB/IB");
		if (mesh.bProceduralGrass || mesh.bGpuSpineSkinned || mesh.bSpineMesh || mesh.bSkeletalSkinned)
			return fail(L"non-static mesh path");
		if (mesh.VertexStride != kGBufferLegacyStaticVertexStride)
			return fail(L"unsupported vertex stride");

		const uint32_t vertexCount =
			(mesh.Vb && mesh.Vb->numVertices > 0) ?
			static_cast<uint32_t>(mesh.Vb->numVertices) :
			mesh.NumVertices;
		const uint32_t indexCount =
			(mesh.Ib && mesh.Ib->numIndices > 0) ?
			static_cast<uint32_t>(mesh.Ib->numIndices) :
			mesh.NumIndices;
		if (vertexCount == 0 || indexCount == 0)
			return fail(L"empty VB/IB");

		for (const Mesh::DrawCall& drawcall : mesh.Draws)
		{
			if (drawcall.IndexCount == 0)
				continue;
			if (drawcall.IndexStart > indexCount ||
				drawcall.IndexCount > indexCount - drawcall.IndexStart)
			{
				return fail(L"index range outside IB");
			}
			if (drawcall.VertexBase > vertexCount)
				return fail(L"vertex base outside VB");
			if (drawcall.VertexCount > 0 &&
				(drawcall.VertexCount > vertexCount ||
				 drawcall.VertexBase > vertexCount - drawcall.VertexCount))
			{
				return fail(L"vertex range outside VB");
			}
		}
		return true;
	}

	bool IsGBufferSceneStaticBindlessGeometryEligible(const Scene* scene, std::wstring* reason = nullptr)
	{
		if (!scene)
		{
			if (reason)
				*reason = L"missing scene";
			return false;
		}
		for (const std::shared_ptr<Mesh>& mesh : scene->meshes)
		{
			if (!mesh)
				continue;
			if (!IsGBufferStaticBindlessGeometryEligible(*mesh, reason))
				return false;
		}
		return true;
	}

	GBufferMaterialKey ResolveGBufferMaterialKey(
		Material* material,
		Texture* defaultWhite,
		Texture* defaultNormal,
		Texture* defaultRoughness,
		Texture* defaultBlack)
	{
		GBufferMaterialKey key{};
		key.Albedo = (material && material->Diffuse) ? material->Diffuse.get() : defaultWhite;
		key.Normal = (material && material->Normal) ? material->Normal.get() : defaultNormal;
		key.Roughness = (material && material->Roughness) ? material->Roughness.get() : defaultRoughness;
		key.Metallic = (material && material->Metallic) ? material->Metallic.get() : defaultBlack;
		return key;
	}

	UINT32 RegisterGBufferBindlessTexture(IRenderBackend* backend, Texture* texture)
	{
		if (!backend || !texture)
			return RHI_INVALID_BINDLESS_INDEX;

		RHITextureHandle handle = backend->RegisterBindlessTexture(texture);
		if (!handle.IsValid())
			handle = backend->GetBindlessTextureHandle(texture);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	}

	GBufferMaterialRecord MakeGBufferMaterialRecord(IRenderBackend* backend, const GBufferMaterialKey& key)
	{
		GBufferMaterialRecord record{};
		record.AlbedoTextureIndex = RegisterGBufferBindlessTexture(backend, key.Albedo);
		record.NormalTextureIndex = RegisterGBufferBindlessTexture(backend, key.Normal);
		record.RoughnessTextureIndex = RegisterGBufferBindlessTexture(backend, key.Roughness);
		record.MetallicTextureIndex = RegisterGBufferBindlessTexture(backend, key.Metallic);
		return record;
	}

	bool IsValidGBufferMaterialRecord(const GBufferMaterialRecord& record)
	{
		return record.AlbedoTextureIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.NormalTextureIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.RoughnessTextureIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.MetallicTextureIndex != RHI_INVALID_BINDLESS_INDEX;
	}

	uint64_t ComputeGBufferMaterialDefaultsHash(
		Texture* defaultWhite,
		Texture* defaultNormal,
		Texture* defaultRoughness,
		Texture* defaultBlack)
	{
		uint64_t hash = 1469598103934665603ull;
		HashCombineGBufferMaterial(hash, reinterpret_cast<uintptr_t>(defaultWhite));
		HashCombineGBufferMaterial(hash, reinterpret_cast<uintptr_t>(defaultNormal));
		HashCombineGBufferMaterial(hash, reinterpret_cast<uintptr_t>(defaultRoughness));
		HashCombineGBufferMaterial(hash, reinterpret_cast<uintptr_t>(defaultBlack));
		return hash;
	}

	void ResetGBufferSceneResourceBindGroup(Scene* scene)
	{
		if (!scene)
			return;
		scene->CachedGBufferResourceBindGroups.clear();
	}

	std::shared_ptr<GraphicsBindGroupHandle> BindGBufferSceneResourceBindGroup(
		IRenderBackend* backend,
		GraphicsPipelineHandle* pipeline,
		Scene* scene,
		Sampler* sampler,
		Buffer* materialBuffer,
		Buffer* geometryBuffer,
		Buffer* drawRecordBuffer,
		bool bAllowCache = true)
	{
		if (!backend || !pipeline || !scene || !sampler || !materialBuffer || !drawRecordBuffer)
			return nullptr;

		if (bAllowCache)
		{
			for (const Scene::GBufferResourceBindGroupCacheEntry& cached : scene->CachedGBufferResourceBindGroups)
			{
				if (cached.BindGroup &&
					cached.Pipeline == pipeline &&
					cached.Sampler == sampler &&
					cached.MaterialBuffer == materialBuffer &&
					cached.GeometryBuffer == geometryBuffer &&
					cached.DrawRecordBuffer == drawRecordBuffer)
				{
					backend->BindGraphicsBindGroup(
						pipeline,
						kGraphicsBindGroupSlot_Material,
						cached.BindGroup);
					return cached.BindGroup;
				}
			}
		}

		GraphicsBindGroupDesc desc{};
		desc.Pipeline = pipeline;
		desc.Slot = kGraphicsBindGroupSlot_Material;
		desc.Entries.reserve(4);
		desc.Entries.push_back(GraphicsBindGroupEntry::BufferSRV("GBufferMaterials", materialBuffer));
		if (geometryBuffer)
			desc.Entries.push_back(GraphicsBindGroupEntry::BufferSRV("GBufferGeometries", geometryBuffer));
		desc.Entries.push_back(GraphicsBindGroupEntry::BufferSRV("GBufferDrawRecords", drawRecordBuffer));
		desc.Entries.push_back(GraphicsBindGroupEntry::SamplerBinding("samplerWrap", sampler));

		std::shared_ptr<GraphicsBindGroupHandle> bindGroup = backend->CreateGraphicsBindGroup(desc);
		backend->BindGraphicsBindGroup(pipeline, kGraphicsBindGroupSlot_Material, bindGroup);

		if (!bAllowCache)
			return bindGroup;

		Scene::GBufferResourceBindGroupCacheEntry cacheEntry;
		cacheEntry.Pipeline = pipeline;
		cacheEntry.Sampler = sampler;
		cacheEntry.MaterialBuffer = materialBuffer;
		cacheEntry.GeometryBuffer = geometryBuffer;
		cacheEntry.DrawRecordBuffer = drawRecordBuffer;
		cacheEntry.BindGroup = bindGroup;
		scene->CachedGBufferResourceBindGroups.push_back(std::move(cacheEntry));
		return bindGroup;
	}

	uint32_t GetGBufferIndexStride(EIndexFormat format)
	{
		return format == EIndexFormat::U16 ? 2u : 4u;
	}

	UINT32 RegisterGBufferBindlessVertexBuffer(IRenderBackend* backend, VertexBuffer* buffer)
	{
		if (!backend || !buffer)
			return RHI_INVALID_BINDLESS_INDEX;

		RHIBufferHandle handle = backend->RegisterBindlessVertexBuffer(buffer);
		if (!handle.IsValid())
			handle = backend->GetBindlessVertexBufferHandle(buffer);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	}

	UINT32 RegisterGBufferBindlessIndexBuffer(IRenderBackend* backend, IndexBuffer* buffer)
	{
		if (!backend || !buffer)
			return RHI_INVALID_BINDLESS_INDEX;

		RHIBufferHandle handle = backend->RegisterBindlessIndexBuffer(buffer);
		if (!handle.IsValid())
			handle = backend->GetBindlessIndexBufferHandle(buffer);
		return handle.IsValid() ? handle.Index : RHI_INVALID_BINDLESS_INDEX;
	}

	bool IsValidGBufferGeometryRecord(const GBufferGeometryRecord& record)
	{
		return record.VertexBufferIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.IndexBufferIndex != RHI_INVALID_BINDLESS_INDEX &&
			record.VertexStride > 0 &&
			(record.IndexStride == 2u || record.IndexStride == 4u);
	}

	GBufferGeometryRecord MakeGBufferGeometryRecord(IRenderBackend* backend, const GBufferGeometryKey& key)
	{
		GBufferGeometryRecord record{};
		record.VertexBufferIndex = RegisterGBufferBindlessVertexBuffer(backend, key.Vertex);
		record.IndexBufferIndex = RegisterGBufferBindlessIndexBuffer(backend, key.Index);
		record.VertexStride = key.VertexStride;
		record.IndexStride = key.IndexStride;
		return record;
	}

	GBufferMaterialTable BuildGBufferMaterialTable(
		IRenderBackend* backend,
		Scene* scene,
		Texture* defaultWhite,
		Texture* defaultNormal,
		Texture* defaultRoughness,
		Texture* defaultBlack)
	{
		GBufferMaterialTable table{};
		if (!SupportsGBufferBindlessMaterials(backend) || !scene)
			return table;

		const uint64_t defaultsHash = ComputeGBufferMaterialDefaultsHash(
			defaultWhite,
			defaultNormal,
			defaultRoughness,
			defaultBlack);
		const GBufferSceneTopologyCounts topologyCounts = CountGBufferSceneTopology(scene);

		auto findOrAddUniqueKey = [&](const GBufferMaterialKey& key) -> uint32_t
		{
			for (uint32_t i = 0; i < table.Keys.size(); ++i)
			{
				if (table.Keys[i] == key)
					return i;
			}
			table.Keys.push_back(key);
			return static_cast<uint32_t>(table.Keys.size() - 1);
		};

		for (std::shared_ptr<Mesh>& mesh : scene->meshes)
		{
			if (!mesh)
				continue;
			for (Mesh::DrawCall& drawcall : mesh->Draws)
			{
				Material* material = drawcall.mat ? drawcall.mat.get() : mesh->Mat.get();
				const GBufferMaterialKey key = ResolveGBufferMaterialKey(
					material,
					defaultWhite,
					defaultNormal,
					defaultRoughness,
					defaultBlack);
				drawcall.GBufferMaterialIndex = findOrAddUniqueKey(key);
			}
		}

		if (table.Keys.empty())
		{
			const GBufferMaterialKey defaultKey = ResolveGBufferMaterialKey(
				nullptr,
				defaultWhite,
				defaultNormal,
				defaultRoughness,
				defaultBlack);
			findOrAddUniqueKey(defaultKey);
		}

		uint64_t materialHash = 1469598103934665603ull;
		HashCombineGBufferMaterial(materialHash, static_cast<uint64_t>(table.Keys.size()));
		for (const GBufferMaterialKey& key : table.Keys)
		{
			HashCombineGBufferMaterial(materialHash, reinterpret_cast<uintptr_t>(key.Albedo));
			HashCombineGBufferMaterial(materialHash, reinterpret_cast<uintptr_t>(key.Normal));
			HashCombineGBufferMaterial(materialHash, reinterpret_cast<uintptr_t>(key.Roughness));
			HashCombineGBufferMaterial(materialHash, reinterpret_cast<uintptr_t>(key.Metallic));
		}

		if (scene->GBufferMaterialRecordBuffer &&
			scene->GBufferMaterialRecordHash == materialHash &&
			scene->GBufferMaterialRecordBackend == backend &&
			scene->GBufferMaterialDefaultsHash == defaultsHash &&
			scene->GBufferMaterialRecordTopologyHash == topologyCounts.Hash &&
			scene->GBufferMaterialRecordMeshCount == topologyCounts.MeshCount &&
			scene->GBufferMaterialRecordDrawCount == topologyCounts.DrawCount)
		{
			table.Buffer = scene->GBufferMaterialRecordBuffer;
			scene->bGBufferMaterialRecordCacheValid = true;
			return table;
		}

		bool allTexturesRegistered = true;
		table.Records.reserve(table.Keys.size());
		for (const GBufferMaterialKey& key : table.Keys)
		{
			GBufferMaterialRecord record = MakeGBufferMaterialRecord(backend, key);
			allTexturesRegistered = allTexturesRegistered && IsValidGBufferMaterialRecord(record);
			table.Records.push_back(record);
		}

		if (!allTexturesRegistered)
		{
			static bool bLoggedFailure = false;
			if (!bLoggedFailure)
			{
				AppendCpuRuntimeTrace(L"[GBufferMaterial] bindless texture registration failed");
				bLoggedFailure = true;
			}
			scene->GBufferMaterialRecordHash = 0;
			scene->GBufferMaterialDefaultsHash = 0;
			scene->GBufferMaterialRecordTopologyHash = 0;
			scene->GBufferMaterialRecordMeshCount = 0;
			scene->GBufferMaterialRecordDrawCount = 0;
			scene->GBufferMaterialRecordBackend = nullptr;
			scene->bGBufferMaterialRecordCacheValid = false;
			scene->GBufferMaterialRecordBuffer.reset();
			ResetGBufferSceneResourceBindGroup(scene);
			table.Keys.clear();
			table.Records.clear();
			return table;
		}

		BufferCreateDesc desc = {};
		desc.NumElements = static_cast<uint32_t>(table.Records.size());
		desc.ElementSize = sizeof(GBufferMaterialRecord);
		desc.InitialState = EInitialResourceState::ShaderRead;
		desc.InitialData = table.Records.data();
		desc.Shape = EBufferShape::Structured;
		desc.Access = EBufferAccess::GpuOnly;
		desc.AllocationPolicy = EBufferAllocationPolicy::Suballocated;

		table.Buffer = backend->CreateBuffer(desc);
		if (!table.Buffer)
		{
			static bool bLoggedBufferFailure = false;
			if (!bLoggedBufferFailure)
			{
				AppendCpuRuntimeTrace(L"[GBufferMaterial] failed to allocate material record buffer");
				bLoggedBufferFailure = true;
			}
			scene->GBufferMaterialRecordHash = 0;
			scene->GBufferMaterialDefaultsHash = 0;
			scene->GBufferMaterialRecordTopologyHash = 0;
			scene->GBufferMaterialRecordMeshCount = 0;
			scene->GBufferMaterialRecordDrawCount = 0;
			scene->GBufferMaterialRecordBackend = nullptr;
			scene->bGBufferMaterialRecordCacheValid = false;
			scene->GBufferMaterialRecordBuffer.reset();
			ResetGBufferSceneResourceBindGroup(scene);
			table.Keys.clear();
			table.Records.clear();
			return table;
		}
		scene->GBufferMaterialRecordHash = materialHash;
		scene->GBufferMaterialDefaultsHash = defaultsHash;
		scene->GBufferMaterialRecordTopologyHash = topologyCounts.Hash;
		scene->GBufferMaterialRecordMeshCount = topologyCounts.MeshCount;
		scene->GBufferMaterialRecordDrawCount = topologyCounts.DrawCount;
		scene->GBufferMaterialRecordBackend = backend;
		scene->bGBufferMaterialRecordCacheValid = true;
		scene->GBufferMaterialRecordBuffer = table.Buffer;
		ResetGBufferSceneResourceBindGroup(scene);
		return table;
	}

	GBufferGeometryTable BuildGBufferGeometryTable(IRenderBackend* backend, Scene* scene)
	{
		GBufferGeometryTable table{};
		if (!SupportsGBufferBindlessGeometry(backend) || !scene)
			return table;

		const GBufferSceneTopologyCounts topologyCounts = CountGBufferSceneTopology(scene);
		if (scene->bGBufferGeometryRecordCacheValid &&
			scene->GBufferGeometryRecordBuffer &&
			scene->GBufferGeometryRecordBackend == backend &&
			scene->GBufferGeometryRecordTopologyHash == topologyCounts.Hash &&
			scene->GBufferGeometryRecordMeshCount == topologyCounts.MeshCount &&
			scene->GBufferGeometryRecordDrawCount == topologyCounts.DrawCount)
		{
			table.Buffer = scene->GBufferGeometryRecordBuffer;
			return table;
		}

		auto findOrAddUniqueKey = [&](const GBufferGeometryKey& key) -> uint32_t
		{
			if (!key.Vertex || !key.Index || key.VertexStride == 0 || key.IndexStride == 0)
				return 0;
			for (uint32_t i = 0; i < table.Keys.size(); ++i)
			{
				if (table.Keys[i] == key)
					return i;
			}
			table.Keys.push_back(key);
			return static_cast<uint32_t>(table.Keys.size() - 1);
		};

		for (std::shared_ptr<Mesh>& mesh : scene->meshes)
		{
			if (!mesh)
				continue;
			if (mesh->Vb && mesh->Ib)
			{
				mesh->GBufferGeometryIndex = findOrAddUniqueKey({
					mesh->Vb.get(),
					mesh->Ib.get(),
					mesh->VertexStride,
					GetGBufferIndexStride(mesh->IndexFormat)
				});
			}
		}

		if (table.Keys.empty())
			return table;

		uint64_t geometryHash = 1469598103934665603ull;
		HashCombineGBufferMaterial(geometryHash, static_cast<uint64_t>(table.Keys.size()));
		for (const GBufferGeometryKey& key : table.Keys)
		{
			HashCombineGBufferMaterial(geometryHash, reinterpret_cast<uintptr_t>(key.Vertex));
			HashCombineGBufferMaterial(geometryHash, reinterpret_cast<uintptr_t>(key.Index));
			HashCombineGBufferMaterial(geometryHash, key.VertexStride);
			HashCombineGBufferMaterial(geometryHash, key.IndexStride);
		}

		if (scene->GBufferGeometryRecordBuffer &&
			scene->GBufferGeometryRecordHash == geometryHash &&
			scene->GBufferGeometryRecordBackend == backend &&
			scene->GBufferGeometryRecordTopologyHash == topologyCounts.Hash &&
			scene->GBufferGeometryRecordMeshCount == topologyCounts.MeshCount &&
			scene->GBufferGeometryRecordDrawCount == topologyCounts.DrawCount)
		{
			table.Buffer = scene->GBufferGeometryRecordBuffer;
			scene->bGBufferGeometryRecordCacheValid = true;
			return table;
		}

		bool allBuffersRegistered = true;
		table.Records.reserve(table.Keys.size());
		for (const GBufferGeometryKey& key : table.Keys)
		{
			GBufferGeometryRecord record = MakeGBufferGeometryRecord(backend, key);
			allBuffersRegistered = allBuffersRegistered && IsValidGBufferGeometryRecord(record);
			table.Records.push_back(record);
		}

		if (!allBuffersRegistered)
		{
			static bool bLoggedFailure = false;
			if (!bLoggedFailure)
			{
				AppendCpuRuntimeTrace(L"[GBufferGeometry] bindless VB/IB registration failed");
				bLoggedFailure = true;
			}
			scene->GBufferGeometryRecordHash = 0;
			scene->GBufferGeometryRecordTopologyHash = 0;
			scene->GBufferGeometryRecordMeshCount = 0;
			scene->GBufferGeometryRecordDrawCount = 0;
			scene->GBufferGeometryRecordBackend = nullptr;
			scene->bGBufferGeometryRecordCacheValid = false;
			scene->GBufferGeometryRecordBuffer.reset();
			ResetGBufferSceneResourceBindGroup(scene);
			table.Keys.clear();
			table.Records.clear();
			return table;
		}

		BufferCreateDesc desc = {};
		desc.NumElements = static_cast<uint32_t>(table.Records.size());
		desc.ElementSize = sizeof(GBufferGeometryRecord);
		desc.InitialState = EInitialResourceState::ShaderRead;
		desc.InitialData = table.Records.data();
		desc.Shape = EBufferShape::Structured;
		desc.Access = EBufferAccess::GpuOnly;
		desc.AllocationPolicy = EBufferAllocationPolicy::Suballocated;

		table.Buffer = backend->CreateBuffer(desc);
		if (!table.Buffer)
		{
			static bool bLoggedBufferFailure = false;
			if (!bLoggedBufferFailure)
			{
				AppendCpuRuntimeTrace(L"[GBufferGeometry] failed to allocate geometry record buffer");
				bLoggedBufferFailure = true;
			}
			scene->GBufferGeometryRecordHash = 0;
			scene->GBufferGeometryRecordTopologyHash = 0;
			scene->GBufferGeometryRecordMeshCount = 0;
			scene->GBufferGeometryRecordDrawCount = 0;
			scene->GBufferGeometryRecordBackend = nullptr;
			scene->bGBufferGeometryRecordCacheValid = false;
			scene->GBufferGeometryRecordBuffer.reset();
			ResetGBufferSceneResourceBindGroup(scene);
			table.Keys.clear();
			table.Records.clear();
			return table;
		}

		scene->GBufferGeometryRecordHash = geometryHash;
		scene->GBufferGeometryRecordTopologyHash = topologyCounts.Hash;
		scene->GBufferGeometryRecordMeshCount = topologyCounts.MeshCount;
		scene->GBufferGeometryRecordDrawCount = topologyCounts.DrawCount;
		scene->GBufferGeometryRecordBackend = backend;
		scene->bGBufferGeometryRecordCacheValid = true;
		scene->GBufferGeometryRecordBuffer = table.Buffer;
		ResetGBufferSceneResourceBindGroup(scene);
		return table;
	}

}

void Corona::InitBloomPass()
{
	auto createPSO = [&](const wchar_t* shaderFile, const std::string& entryPoint,
		std::initializer_list<RHIBindingDesc> bindings)
		-> std::shared_ptr<ComputePipelineStateObject>
	{
		auto pso = renderBackend->CreateComputePipelineStateObject();
		if (!pso) return nullptr;
		for (const RHIBindingDesc& binding : bindings)
		{
			switch (binding.DescriptorKind)
			{
			case RHIDescriptorKind::SRV:
			case RHIDescriptorKind::AccelerationStructure:
				pso->BindSRV(binding);
				break;
			case RHIDescriptorKind::UAV:
				pso->BindUAV(binding);
				break;
			case RHIDescriptorKind::CBV:
				pso->BindCBV(binding);
				break;
			case RHIDescriptorKind::Sampler:
				pso->BindSampler(binding);
				break;
			}
		}
		if (!pso->InitCS(GetAssetFullPath(shaderFile), entryPoint))
			return nullptr;
		return pso;
	};
	const RHIShaderStageMask computeStage = ToRHIShaderStageMask(RHIShaderStage::Compute);

	if (auto pso = createPSO(L"Shaders\\BloomBlur.hlsl", "BloomExtract",
		{
			MakeRHITextureSRV("SrcTex", 0, computeStage),
			MakeRHIBufferSRV("Exposure", 1, computeStage),
			MakeRHITextureUAV("DstTex", 0, computeStage),
			MakeRHITextureUAV("LumaResult", 1, computeStage),
			MakeRHISampler("samplerWrap", 0, computeStage),
			MakeRHICBV("BloomCB", 0, sizeof(BloomCB), computeStage),
		}))
		BloomExtractPSO = pso;

	if (auto pso = createPSO(L"Shaders\\BloomBlur.hlsl", "BloomBlur",
		{
			MakeRHITextureSRV("SrcTex", 0, computeStage),
			MakeRHITextureUAV("DstTex", 0, computeStage),
			MakeRHISampler("samplerWrap", 0, computeStage),
			MakeRHICBV("BloomCB", 0, sizeof(BloomCB), computeStage),
		}))
		BloomBlurPSO = pso;

	if (auto pso = createPSO(L"Shaders\\Histogram.hlsl", "GenerateHistogram",
		{
			MakeRHITextureSRV("LumaTex", 0, computeStage),
			MakeRHIBufferUAV("Histogram", 0, computeStage, RHIBufferViewKind::Raw),
		}))
		HistogramPSO = pso;

	if (auto pso = createPSO(L"Shaders\\DrawHistogram.hlsl", "DrawHistogram",
		{
			MakeRHIBufferSRV("Histogram", 0, computeStage, RHIBufferViewKind::Raw),
			MakeRHIBufferSRV("Exposure", 1, computeStage),
			MakeRHITextureUAV("ColorBuffer", 0, computeStage),
		}))
		DrawHistogramPSO = pso;

	if (auto pso = createPSO(L"Shaders\\Histogram.hlsl", "ClearHistogram",
		{
			MakeRHIBufferUAV("Histogram", 0, computeStage, RHIBufferViewKind::Raw),
		}))
		ClearHistogramPSO = pso;

	if (auto pso = createPSO(L"Shaders\\AdaptExposureCS.hlsl", "AdaptExposure",
		{
			MakeRHIBufferSRV("Histogram", 0, computeStage, RHIBufferViewKind::Raw),
			MakeRHIBufferUAV("Exposure", 0, computeStage),
			MakeRHICBV("AdaptExposureCB", 0, sizeof(AdaptExposureCB), computeStage),
		}))
		AdapteExposurePSO = pso;

	BloomBlurPingPong[0] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });
	NAME_D3D12_OBJECT(BloomBlurPingPong[0]->resource);

	BloomBlurPingPong[1] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });
	NAME_D3D12_OBJECT(BloomBlurPingPong[1]->resource);

	LumaBuffer = renderBackend->CreateTexture2D({ ETextureFormat::R8Uint, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });
	NAME_D3D12_OBJECT(LumaBuffer->resource);

	Histogram = renderBackend->CreateBuffer({ 256u, sizeof(UINT32), EInitialResourceState::ShaderRead, true, nullptr, EBufferShape::ByteAddress });
	NAME_D3D12_OBJECT(Histogram->resource);

	__declspec(align(16)) float initExposure[] =
	{
		Exposure,
		1.0f / Exposure,
		0.01,
		Exposure,
		0.0f,
		kInitialMinLog,
		kInitialMaxLog,
		kInitialMaxLog - kInitialMinLog,
		1.0f / (kInitialMaxLog - kInitialMinLog)
	};

	ExposureData = renderBackend->CreateBuffer({ 8u, sizeof(float), EInitialResourceState::ShaderRead, true, initExposure, EBufferShape::Structured });
	NAME_D3D12_OBJECT(ExposureData->resource);
}

void Corona::InitGBufferPass()
{
	static bool bAppliedGBufferCullModeEnv = false;
	if (!bAppliedGBufferCullModeEnv)
	{
		bAppliedGBufferCullModeEnv = true;
		const int modeOverride = GetGBufferCullingModeOverride();
		if (modeOverride == 0 || modeOverride == 1)
		{
			GBufferObjectCullingMode = static_cast<EGBufferObjectCullingMode>(modeOverride);
			AppendCpuRuntimeTrace(L"[InitGBufferPass] CORONA_GBUFFER_CULL_MODE=" + std::to_wstring(modeOverride));
		}
	}

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(CORONA_PLATFORM_MOBILE ? L"Shaders\\GBufferMobile.hlsl" : L"Shaders\\GBuffer.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	// Default mesh layout (44 B legacy "Vertex"): POSITION float3 @ 0,
	// NORMAL float3 @ 12, UV float2 @ 24, TANGENT float3 @ 32. This is
	// what CreateMeshFromVertices and the platformer scene meshes upload.
	// Skeletal meshes use a different 48 B StandardVertex layout; their
	// pipelines override this stride + offsets explicitly. Vulkan reads
	// the stride from the PSO, so getting the default wrong slides every
	// non-skeletal vertex 4 B on mobile.
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0 },
		{ "NORMAL",   0, EVertexAttributeFormat::Float3, 12 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 24 },
		{ "TANGENT",  0, EVertexAttributeFormat::Float3, 32 },
	};
	desc.VertexStride = 44;
	if (CORONA_PLATFORM_MOBILE)
	{
		desc.ColorFormats = {
			ETextureFormat::RGBA8Unorm,
			ETextureFormat::RGBA8Unorm,
			ETextureFormat::RG16Float,
			ETextureFormat::RGBA8Unorm,
		};
	}
	else
	{
		desc.ColorFormats = {
			ETextureFormat::RGBA8Unorm,
			ETextureFormat::RGBA8Unorm,
			ETextureFormat::RGBA16Float,
			ETextureFormat::RGBA16Float,
			ETextureFormat::RG16Float,
			ETextureFormat::RGBA8Unorm,
			ETextureFormat::R32Float,
		};
	}
	desc.DepthFormat = ETextureFormat::D32Float;
	desc.bDepthEnable = true;
	desc.bCullBackFaces = false;
	desc.ConstantBufferSize = sizeof(GBufferConstantBuffer);
	desc.ConstantBufferBinding = 0;
	const RHIShaderStageMask gbufferVertexStage = ToRHIShaderStageMask(RHIShaderStage::Vertex);
	const RHIShaderStageMask gbufferPixelStage = ToRHIShaderStageMask(RHIShaderStage::Pixel);
	const RHIShaderStageMask gbufferGraphicsStages = RHIShaderStage::Vertex | RHIShaderStage::Pixel;
	{
		GBufferDrawRecord dummyDrawRecord{};
		BufferCreateDesc dummyDrawRecordDesc{};
		dummyDrawRecordDesc.NumElements = 1;
		dummyDrawRecordDesc.ElementSize = sizeof(GBufferDrawRecord);
		dummyDrawRecordDesc.InitialState = EInitialResourceState::ShaderRead;
		dummyDrawRecordDesc.InitialData = &dummyDrawRecord;
		dummyDrawRecordDesc.Shape = EBufferShape::Structured;
		dummyDrawRecordDesc.Access = EBufferAccess::GpuOnly;
		dummyDrawRecordDesc.AllocationPolicy = EBufferAllocationPolicy::Suballocated;
		GBufferDummyDrawRecordBuffer = renderBackend->CreateBuffer(dummyDrawRecordDesc);
		if (!GBufferDummyDrawRecordBuffer)
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create dummy GBufferDrawRecords buffer");
	}
	auto setBaseGBufferLayout = [&](GraphicsPipelineDesc& pipelineDesc)
	{
		pipelineDesc.PipelineLayout.Bindings = {
			MakeRHICBV("__CB0", 0, sizeof(GBufferConstantBuffer), gbufferGraphicsStages),
			MakeRHIBindlessTextureSRV("MaterialTextures", 0, kGBufferBindlessTextureRegisterSpace, gbufferPixelStage),
			MakeRHIBufferSRV("GBufferMaterials", kGBufferMaterialRecordRegister, gbufferPixelStage),
			MakeRHIBufferSRV("GBufferDrawRecords", kGBufferDrawRecordRegister, gbufferGraphicsStages),
			MakeRHISampler("samplerWrap", 0, gbufferPixelStage),
		};
	};
	auto appendGBufferSRV = [&](GraphicsPipelineDesc& pipelineDesc, const char* name, uint32_t slot)
	{
		pipelineDesc.PipelineLayout.Bindings.push_back(MakeRHIBufferSRV(name, slot, gbufferVertexStage));
	};
	auto appendGBufferBindlessGeometryLayout = [&](GraphicsPipelineDesc& pipelineDesc)
	{
		pipelineDesc.PipelineLayout.Bindings.push_back(MakeRHIBindlessBufferSRV(
			"GeometryBuffers",
			0,
			12,
			gbufferVertexStage,
			RHIBufferViewKind::Raw));
		pipelineDesc.PipelineLayout.Bindings.push_back(MakeRHIBufferSRV(
			"GBufferGeometries",
			14,
			gbufferVertexStage,
			RHIBufferViewKind::Structured));
	};
	setBaseGBufferLayout(desc);

	GBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
	{
		GraphicsPipelineDesc bindlessGeometryDesc = desc;
		bindlessGeometryDesc.VertexEntryPoint = "VSMainBindless";
		bindlessGeometryDesc.VertexElements.clear();
		bindlessGeometryDesc.VertexStride = 0;
		appendGBufferBindlessGeometryLayout(bindlessGeometryDesc);
		try
		{
			GBufferBindlessGeometryGraphicsPipeline = renderBackend->CreateGraphicsPipeline(bindlessGeometryDesc);
		}
		catch (const std::exception&)
		{
			AppendCpuRuntimeTrace(L"[InitGBufferPass] Bindless static geometry pipeline create exception");
		}
		if (!GBufferBindlessGeometryGraphicsPipeline)
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Bindless Static GBuffer pipeline");
	}
	{
		const RenderBackendCapabilities backendCapabilities = renderBackend ?
			renderBackend->GetCapabilities() :
			RenderBackendCapabilities{};
		if (backendCapabilities.SupportsDrawIndirect &&
			backendCapabilities.SupportsDrawIndirectFirstInstance)
		{
			GraphicsPipelineDesc bindlessIndirectDesc = desc;
			bindlessIndirectDesc.VertexEntryPoint = "VSMainBindlessIndirect";
			bindlessIndirectDesc.VertexElements.clear();
			bindlessIndirectDesc.VertexStride = 0;
			appendGBufferBindlessGeometryLayout(bindlessIndirectDesc);
			try
			{
				GBufferBindlessIndirectGraphicsPipeline = renderBackend->CreateGraphicsPipeline(bindlessIndirectDesc);
			}
			catch (const std::exception&)
			{
				AppendCpuRuntimeTrace(L"[InitGBufferPass] Bindless indirect GBuffer pipeline create exception");
			}
			if (!GBufferBindlessIndirectGraphicsPipeline)
				AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Bindless Indirect GBuffer pipeline");

			// Opaque variant: identical pipeline that runs PSMainOpaque
			// (early-Z, no discard) so fully-opaque draws reject occluded
			// pixels before shading. Optional — if it fails we fall back to
			// the single alpha-test pipeline for all draws.
			GraphicsPipelineDesc bindlessIndirectOpaqueDesc = bindlessIndirectDesc;
			bindlessIndirectOpaqueDesc.PixelEntryPoint = "PSMainOpaque";
			try
			{
				GBufferBindlessIndirectOpaqueGraphicsPipeline = renderBackend->CreateGraphicsPipeline(bindlessIndirectOpaqueDesc);
			}
			catch (const std::exception&)
			{
				AppendCpuRuntimeTrace(L"[InitGBufferPass] Bindless indirect opaque GBuffer pipeline create exception");
			}
			if (!GBufferBindlessIndirectOpaqueGraphicsPipeline)
				AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Bindless Indirect opaque GBuffer pipeline (opaque early-Z split disabled)");
		}
		else
		{
			AppendCpuRuntimeTrace(L"[InitGBufferPass] skipping Bindless Indirect GBuffer pipeline: backend lacks draw-indirect first-instance support");
		}
	}
	{
		const RenderBackendCapabilities backendCapabilities = renderBackend ?
			renderBackend->GetCapabilities() :
			RenderBackendCapabilities{};
		if (backendCapabilities.SupportsDrawIndirectCount)
		{
			const RHIShaderStageMask computeStage = ToRHIShaderStageMask(RHIShaderStage::Compute);
			auto createGBufferCullPSO = [&](const std::string& entryPoint, std::initializer_list<RHIBindingDesc> bindings)
				-> std::shared_ptr<ComputePipelineStateObject>
			{
				auto pso = renderBackend->CreateComputePipelineStateObject();
				if (!pso)
					return nullptr;
				for (const RHIBindingDesc& binding : bindings)
				{
					switch (binding.DescriptorKind)
					{
					case RHIDescriptorKind::SRV:
						pso->BindSRV(binding);
						break;
					case RHIDescriptorKind::UAV:
						pso->BindUAV(binding);
						break;
					case RHIDescriptorKind::CBV:
						pso->BindCBV(binding);
						break;
					case RHIDescriptorKind::Sampler:
					case RHIDescriptorKind::AccelerationStructure:
						break;
					}
				}
				if (!pso->InitCS(GetAssetFullPath(L"Shaders\\GBufferCullCS.hlsl"), entryPoint))
					return nullptr;
				return pso;
			};

			GBufferGpuCullClearPSO = createGBufferCullPSO(
				"ClearGBufferIndirectCountersCS",
				{
					MakeRHIBufferUAV("GBufferCullCounters", 2, computeStage),
				});
			GBufferGpuCullBuildPSO = createGBufferCullPSO(
				"BuildGBufferIndirectArgsCS",
				{
					MakeRHIBufferSRV("GBufferObjectRanges", 0, computeStage),
					MakeRHIBufferSRV("GBufferCachedDraws", 1, computeStage),
					MakeRHIBufferSRV("GBufferCandidateObjectIndices", 2, computeStage),
					MakeRHIBufferUAV("GBufferOpaqueArgs", 0, computeStage),
					MakeRHIBufferUAV("GBufferAlphaArgs", 1, computeStage),
					MakeRHIBufferUAV("GBufferCullCounters", 2, computeStage),
					MakeRHICBV("GBufferCullCB", 0, sizeof(GBufferGpuCullConstant), computeStage),
				});
			if (!GBufferGpuCullClearPSO || !GBufferGpuCullBuildPSO)
				AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create GPU GBuffer culling compute PSOs");
		}
	}

	if (!CORONA_PLATFORM_MOBILE)
	{
		GraphicsPipelineDesc staticInstancedDesc = desc;
		staticInstancedDesc.VertexEntryPoint = "StaticInstancedVSMain";
		appendGBufferSRV(staticInstancedDesc, "StaticInstanceTransforms", 12);
		try
		{
			StaticInstancedGBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(staticInstancedDesc);
		}
		catch (const std::exception& ex)
		{
			AppendCpuRuntimeTrace(L"[InitGBufferPass] StaticInstanced pipeline create exception");
			(void)ex;
		}
		if (!StaticInstancedGBufferGraphicsPipeline)
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Static Instanced GBuffer pipeline");

		GraphicsPipelineDesc staticInstancedBindlessDesc = desc;
		staticInstancedBindlessDesc.VertexEntryPoint = "StaticInstancedVSMainBindless";
		staticInstancedBindlessDesc.VertexElements.clear();
		staticInstancedBindlessDesc.VertexStride = 0;
		appendGBufferBindlessGeometryLayout(staticInstancedBindlessDesc);
		appendGBufferSRV(staticInstancedBindlessDesc, "StaticInstanceTransforms", 12);
		try
		{
			StaticInstancedBindlessGBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(staticInstancedBindlessDesc);
		}
		catch (const std::exception&)
		{
			AppendCpuRuntimeTrace(L"[InitGBufferPass] StaticInstanced bindless pipeline create exception");
		}
		if (!StaticInstancedBindlessGBufferGraphicsPipeline)
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Static Instanced Bindless GBuffer pipeline");
	}

	// Procedural grass PSO — no IA stream; the VS reads SV_VertexID +
	// SV_InstanceID and the per-mesh params (PG_*) from b0. Sharing the
	// GBuffer CB lets DrawScene's existing CB fill code populate the
	// world/view matrices in one place.
	{
		GraphicsPipelineDesc pgDesc = desc;
		pgDesc.ShaderPath = GetAssetFullPath(L"Shaders\\GrassProcedural.hlsl");
		pgDesc.VertexEntryPoint = "VSMain";
		pgDesc.PixelEntryPoint  = "PSMain";
		pgDesc.VertexElements.clear();
		pgDesc.VertexStride = 0;
		// TerrainHeights at t4 (matches binding slot used by SpineVertices
		// in legacy GBuffer; safe to reuse since procedural has no Spine).
		pgDesc.PipelineLayout.Bindings = {
			MakeRHICBV("__CB0", 0, sizeof(GBufferConstantBuffer), gbufferGraphicsStages),
			MakeRHIBufferSRV("TerrainHeights", 4, gbufferVertexStage),
		};
		ProceduralGrassGraphicsPipeline = renderBackend->CreateGraphicsPipeline(pgDesc);
		if (!ProceduralGrassGraphicsPipeline)
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create ProceduralGrass pipeline");

		// Shared sequential IB (0,1,2,…) — sized for the maximum supported
		// blade tessellation (32 segments × 6 = 192 indices). Created once,
		// reused by every procedural-grass draw.
		if (!ProceduralGrassSequentialIb)
		{
			constexpr uint32_t kMaxSegments = 32u;
			constexpr uint32_t kMaxIndices  = kMaxSegments * 6u;
			std::vector<uint32_t> indices(kMaxIndices);
			for (uint32_t i = 0; i < kMaxIndices; ++i)
				indices[i] = i;
			ProceduralGrassSequentialIb = renderBackend->CreateIndexBuffer(
				EIndexFormat::U32,
				static_cast<uint32_t>(indices.size() * sizeof(uint32_t)),
				indices.data());
		}
	}

	// CPU Spine path uses `mesh->Vb` filled with SpineSampleVertex (44 B:
	// POSITION @ 0, NORMAL @ 12, UV @ 24, TANGENT @ 32). The base desc
	// declares the 48 B StandardVertex layout for Skeletal — that stride
	// would slip every Spine vertex 4 B on Vulkan (PSO stride wins).
	// Override here so the CPU-skinned VB is fetched correctly.
	GraphicsPipelineDesc cpuSpineDesc = desc;
	cpuSpineDesc.bDepthWriteEnable = false;
	cpuSpineDesc.VertexStride = 44;
	cpuSpineDesc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0 },
		{ "NORMAL",   0, EVertexAttributeFormat::Float3, 12 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 24 },
		{ "TANGENT",  0, EVertexAttributeFormat::Float3, 32 },
	};
	CpuSpineGBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(cpuSpineDesc);
	if (!CpuSpineGBufferGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create CPU Spine GBuffer pipeline");

	GraphicsPipelineDesc spineDesc = desc;
	spineDesc.VertexEntryPoint = "SpineVSMain";
	spineDesc.VertexElements.clear();
	spineDesc.VertexStride = 0;
	spineDesc.bDepthWriteEnable = false;
	appendGBufferSRV(spineDesc, "SpineVertices", 4);
	SpineGBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(spineDesc);
	if (!SpineGBufferGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Spine GBuffer pipeline");

	// Spine VS-inline PSO: same vertex-fetch-via-SBV layout as
	// SpineGBufferGraphicsPipeline but the VS skins from
	// SpineVsInlineInputVertices/Influences/Bones instead of reading
	// pre-skinned vertices. Desktop and mobile both expose this entry
	// (GBuffer.hlsl + GBufferMobile.hlsl) -- on mobile this replaces the
	// compute pre-pass entirely. try/catch so missing-entry failures
	// degrade gracefully.
	{
		GraphicsPipelineDesc spineVsInlineDesc = spineDesc;
		spineVsInlineDesc.VertexEntryPoint = "SpineVsInlineVSMain";
		setBaseGBufferLayout(spineVsInlineDesc);
		appendGBufferSRV(spineVsInlineDesc, "SpineVsInlineInputVertices", 9);
		appendGBufferSRV(spineVsInlineDesc, "SpineVsInlineInfluences", 10);
		appendGBufferSRV(spineVsInlineDesc, "SpineVsInlineBones", 11);
		try
		{
			SpineVsInlineGBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(spineVsInlineDesc);
		}
		catch (const std::exception& ex)
		{
			AppendCpuRuntimeTrace(L"[InitGBufferPass] SpineVsInline pipeline create exception");
			(void)ex;
		}
		if (!SpineVsInlineGBufferGraphicsPipeline)
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Spine VS-inline GBuffer pipeline");
	}

	// Phase 11 (revised): Skeletal GBuffer PSO. Same IA layout as the
	// standard GBufferGraphicsPipeline, but the VS variant re-skins the
	// bind-pose vertex with the previous frame's bone palette so velocity
	// reflects per-vertex skinning motion, not just camera/world delta.
	// Two SBVs:
	//   t5 = SkeletalInputs    (bind position + bone indices/weights)
	//   t6 = SkeletalPrevBones (previous frame's mat3x4 per bone)
	GraphicsPipelineDesc skeletalDesc = desc;
	skeletalDesc.VertexEntryPoint = "SkeletalVSMain";
	appendGBufferSRV(skeletalDesc, "SkeletalInputs", 5);
	appendGBufferSRV(skeletalDesc, "SkeletalPrevBones", 6);
	// Skeletal output / bind-pose VBs use the StandardVertex layout
	// (POSITION float4 @ 0, TEXCOORD @ 16, NORMAL @ 24, TANGENT @ 36,
	// stride 48), not the standard 44-byte sponza layout. Vulkan reads
	// stride from the PSO binding description, so we have to override
	// here or every vertex slips by 4 bytes. Note: Vulkan assigns
	// VkVertexInputAttributeDescription location by index in this
	// vector — keep the order matching VSInput in GBuffer.hlsl
	// (POSITION / NORMAL / TEXCOORD0 / TANGENT).
	skeletalDesc.VertexStride = 48;
	skeletalDesc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0 },
		{ "NORMAL",   0, EVertexAttributeFormat::Float3, 24 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 },
		{ "TANGENT",  0, EVertexAttributeFormat::Float3, 36 },
	};
	try
	{
		SkeletalGBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(skeletalDesc);
	}
	catch (const std::exception& ex)
	{
		AppendCpuRuntimeTrace(L"[InitGBufferPass] SkeletalGBuffer pipeline create exception");
		(void)ex;
	}
	if (!SkeletalGBufferGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Skeletal GBuffer pipeline");

	// Path C: VS inline skinning PSO. Same IA layout, different VS entry,
	// adds one SBV for the current-frame bone palette.
	GraphicsPipelineDesc vsInlineDesc = desc;
	vsInlineDesc.VertexEntryPoint = "SkeletalVsInlineVSMain";
	appendGBufferSRV(vsInlineDesc, "SkeletalInputs", 5);
	appendGBufferSRV(vsInlineDesc, "SkeletalPrevBones", 6);
	appendGBufferSRV(vsInlineDesc, "SkeletalCurrBones", 7);
	// Same StandardVertex IA layout as skeletalDesc (48 B stride).
	vsInlineDesc.VertexStride = 48;
	vsInlineDesc.VertexElements = skeletalDesc.VertexElements;
	try
	{
		SkeletalVsInlineGraphicsPipeline = renderBackend->CreateGraphicsPipeline(vsInlineDesc);
	}
	catch (const std::exception& ex)
	{
		AppendCpuRuntimeTrace(L"[InitGBufferPass] SkeletalVsInline pipeline create exception");
		(void)ex;
	}
	if (!SkeletalVsInlineGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Skeletal VS-inline GBuffer pipeline");

	// Phase D (desktop only): instanced cluster PSO. One DrawIndexedInstanced
	// draws every skeletal character; the VS reads its world matrix from
	// SkeletalInstanceTransforms[SV_InstanceID]. Mobile shaders
	// (GBufferMobile.hlsl) deliberately do not contain this entry — the
	// Adreno SPIR-V compiler couldn't handle the SV_InstanceID +
	// SkeletalInstanceTransforms variant. We try/catch so missing-entry
	// failures on mobile leave the PSO null and the caller falls back.
	if (!CORONA_PLATFORM_MOBILE)
	{
		GraphicsPipelineDesc vsClusterDesc = desc;
		vsClusterDesc.VertexEntryPoint = "SkeletalVsInlineClusterVSMain";
		appendGBufferSRV(vsClusterDesc, "SkeletalInputs", 5);
		appendGBufferSRV(vsClusterDesc, "SkeletalPrevBones", 6);
		appendGBufferSRV(vsClusterDesc, "SkeletalCurrBones", 7);
		appendGBufferSRV(vsClusterDesc, "SkeletalInstanceTransforms", 8);
		vsClusterDesc.VertexStride = 48;
		vsClusterDesc.VertexElements = skeletalDesc.VertexElements;
		try
		{
			SkeletalVsInlineClusterGraphicsPipeline = renderBackend->CreateGraphicsPipeline(vsClusterDesc);
		}
		catch (const std::exception& ex)
		{
			AppendCpuRuntimeTrace(L"[InitGBufferPass] SkeletalVsInlineCluster pipeline create exception");
			(void)ex;
		}
		if (!SkeletalVsInlineClusterGraphicsPipeline)
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Skeletal VS-inline cluster GBuffer pipeline");
	}

	auto spineSkinningPSO = renderBackend->CreateComputePipelineStateObject();
	if (spineSkinningPSO)
	{
		const RHIShaderStageMask computeStage = ToRHIShaderStageMask(RHIShaderStage::Compute);
		spineSkinningPSO->BindSRV(MakeRHIBufferSRV("InputVertices", 0, computeStage));
		spineSkinningPSO->BindSRV(MakeRHIBufferSRV("Influences", 1, computeStage));
		spineSkinningPSO->BindSRV(MakeRHIBufferSRV("Bones", 2, computeStage));
		spineSkinningPSO->BindUAV(MakeRHIBufferUAV("OutputVertices", 0, computeStage));
		spineSkinningPSO->BindCBV(MakeRHICBV("SpineSkinningConstant", 0, sizeof(SpineSkinningConstant), computeStage));
		if (spineSkinningPSO->InitCS(GetAssetFullPath(L"Shaders\\SpineSkinningCS.hlsl"), "SkinMain"))
			SpineSkinningPSO = spineSkinningPSO;
		else
			AppendCpuRuntimeTrace(L"[InitGBufferPass] failed to create Spine skinning compute PSO");
	}

	// 3D skeletal skinning PSO (separate path from Spine).
	InitSkeletalSkinningPSO();
}

void Corona::InitToneMapPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },    // Bottom left.
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },    // Top left.
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },    // Bottom right.
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }        // Top right.
	};

	const UINT vertexBufferSize = sizeof(quadVertices);
	const UINT vertexBufferStride = sizeof(PostVertex);

	FullScreenVB = renderBackend->CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices);

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexStride = vertexBufferStride;
	desc.ColorFormats = { ETextureFormat::RGBA8Unorm };
	desc.bDepthEnable = false;
	desc.bCullBackFaces = false;
	desc.bTriangleStrip = true;
	desc.ConstantBufferSize = sizeof(ToneMapCB);
	desc.ConstantBufferBinding = 0;
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float4, 0 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 }
	};
	const RHIShaderStageMask toneMapPixelStage = ToRHIShaderStageMask(RHIShaderStage::Pixel);
	const RHIShaderStageMask toneMapGraphicsStages = RHIShaderStage::Vertex | RHIShaderStage::Pixel;
	desc.PipelineLayout.Bindings = {
		MakeRHICBV("__CB0", 0, sizeof(ToneMapCB), toneMapGraphicsStages),
		MakeRHITextureSRV("SrcTex", 0, toneMapPixelStage),
		MakeRHISampler("sampleWrap", 0, toneMapPixelStage),
	};

	ToneMapGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
}

void Corona::InitParticlePass()
{
	if (!renderBackend)
		return;
	// Pipeline only needs render-target *formats*, not the live textures, so
	// this init order is safe even before LightingBuffer is allocated. The
	// runtime ParticlePass() guards against a missing LightingBuffer.

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\Particle.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexStride = sizeof(Particles::QuadVertex);
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0  },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 12 },
		{ "COLOR",    0, EVertexAttributeFormat::Float4, 20 },
	};
	// Single-RT pass: draws into the post-light HDR LightingBuffer
	// (RGBA16Float). Depth-tested against the GBuffer DepthBuffer but never
	// writes to it. Additive blend by default for M1 (sparks); M3 adds an
	// alpha-blend variant for smoke.
	desc.ColorFormats = { ETextureFormat::RGBA16Float };
	desc.DepthFormat = ETextureFormat::D32Float;
	desc.bDepthEnable = true;
	desc.bDepthWriteEnable = false;
	desc.bCullBackFaces = false;
	desc.BlendMode = EBlendMode::Additive;
	desc.ConstantBufferSize = sizeof(ParticleCB);
	desc.ConstantBufferBinding = 0;
	const RHIShaderStageMask particlePixelStage = ToRHIShaderStageMask(RHIShaderStage::Pixel);
	const RHIShaderStageMask particleGraphicsStages = RHIShaderStage::Vertex | RHIShaderStage::Pixel;
	desc.PipelineLayout.Bindings = {
		MakeRHICBV("__CB0", 0, sizeof(ParticleCB), particleGraphicsStages),
		MakeRHITextureSRV("ParticleTex", 0, particlePixelStage),
		MakeRHISampler("samplerClamp", 0, particlePixelStage),
	};

	ParticleGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
	if (!ParticleGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitParticlePass] pipeline create failed");
	else
		AppendCpuRuntimeTrace(L"[InitParticlePass] pipeline created");
}

void Corona::UpdateParticleSystems(float dt)
{
	Terrain::Component* terrain = ActiveTerrain.get();
	for (auto& sys : ActiveParticleSystems)
	{
		if (!sys)
			continue;
		sys->SetTerrainCollider(terrain);
		sys->Tick(dt);
	}
}

uint32_t Corona::ParticleBurstForScript(float x, float y, float z, uint32_t count)
{
	if (ActiveParticleSystems.empty() || count == 0)
		return 0;
	auto& sys = ActiveParticleSystems.front();
	if (!sys)
		return 0;
	return sys->SpawnBurst(glm::vec3(x, y, z), count);
}

void Corona::UseNativeCameraForScript(bool enabled)
{
	if (enabled)
	{
		// Seed the SimpleCamera with the active script camera's pose so
		// the free-fly takeover doesn't jump to a stale location. Pull
		// position straight from the entity transform, the look direction
		// from the CameraComponent.
		if (auto activeCam = EntityWorld.GetActiveCameraEntity(); activeCam.IsValid())
		{
			const auto* trans = EntityWorld.GetTransform(activeCam);
			const auto* cam = EntityWorld.GetCamera(activeCam);
			if (trans && cam)
			{
				m_camera.m_position = trans->GetPosition();
				const glm::vec3 look = cam->LookDirection;
				m_camera.m_lookDirection = look;
				m_camera.m_yaw = std::atan2(look.x, look.z);
				m_camera.m_pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
				m_camera.m_upDirection = cam->UpDirection;
			}
		}
		EntityWorld.ClearActiveCamera();
		bScriptCameraControlEnabled = false;
	}
	else
	{
		// Re-enable script-driven camera updates. The Luau script is
		// responsible for re-activating its CameraComponent on the next
		// tick (corona.CameraComponent.set with active=true).
		bScriptCameraControlEnabled = true;
	}
}

void Corona::ParticlePass()
{
	if (!renderBackend || !ParticleGraphicsPipeline || ActiveParticleSystems.empty())
		return;
	if (!LightingBuffer || !DepthBuffer)
		return;

	renderBackend->EmitGpuCrashMarker("ParticlePass");

	// LightingPass ends by transitioning LightingBuffer → ShaderRead. Bring
	// it back to RenderTarget for the additive sprite draws, then restore
	// the state the downstream TemporalAA/ToneMap path expects.
	renderBackend->TransitionTexture(LightingBuffer.get(),
		EResourceState::ShaderRead, EResourceState::RenderTarget);

	Texture* color = LightingBuffer.get();
	renderBackend->SetRenderTargets(&color, 1, DepthBuffer.get());
	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());

	ParticleCB cb{};
	cb.ViewProj = glm::transpose(ViewProjMat);

	// DX12 SetGraphicsRootSignature (inside BindGraphicsPipeline) invalidates
	// root-table writes that came before it — bind first, then descriptors.
	renderBackend->BindGraphicsPipeline(ParticleGraphicsPipeline.get());

	uint32_t totalActive = 0;
	for (auto& sys : ActiveParticleSystems)
	{
		if (!sys)
			continue;
		sys->UploadQuadVertices(renderBackend.get(), ViewMat);
		const uint32_t indexCount = sys->GetIndexCount();
		if (indexCount == 0)
			continue;
		auto& tex = sys->GetTexture();
		auto& vb  = sys->GetVb();
		auto& ib  = sys->GetIb();
		if (!tex || !vb || !ib)
			continue;

		CreateAndBindGraphicsBindGroup(renderBackend.get(), ParticleGraphicsPipeline.get(),
			{
				GraphicsBindGroupEntry::SamplerBinding("samplerClamp", samplerBilinearWrap.get()),
				GraphicsBindGroupEntry::TextureSRV("ParticleTex", tex.get()),
				GraphicsBindGroupEntry::Constant(0, &cb, sizeof(cb)),
			});
		renderBackend->BindMeshBuffers(vb.get(), ib.get());
		renderBackend->DrawIndexed(indexCount, 0, 0);
		totalActive += sys->GetActiveCount();
	}

	renderBackend->TransitionTexture(LightingBuffer.get(),
		EResourceState::RenderTarget, EResourceState::ShaderRead);

	static uint32_t s_logCounter = 0;
	if ((++s_logCounter % 120u) == 0u)
	{
		AppendCpuRuntimeTrace(
			L"[Particles] active=" + std::to_wstring(totalActive) +
			L" systems=" + std::to_wstring(ActiveParticleSystems.size()));
	}
}

void Corona::InitDebugPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },    // Bottom left.
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },    // Top left.
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },    // Bottom right.
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }        // Top right.
	};

	const UINT vertexBufferSize = sizeof(quadVertices);
	const UINT vertexBufferStride = sizeof(PostVertex);

	FullScreenVB = renderBackend->CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices);

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\DebugPS.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexStride = vertexBufferStride;
	desc.ColorFormats = { ETextureFormat::RGBA8Unorm };
	desc.bDepthEnable = false;
	desc.bCullBackFaces = false;
	desc.bTriangleStrip = true;
	desc.ConstantBufferSize = sizeof(DebugPassCB);
	desc.ConstantBufferBinding = 0;
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float4, 0 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 }
	};
	const RHIShaderStageMask debugPixelStage = ToRHIShaderStageMask(RHIShaderStage::Pixel);
	const RHIShaderStageMask debugGraphicsStages = RHIShaderStage::Vertex | RHIShaderStage::Pixel;
	desc.PipelineLayout.Bindings = {
		MakeRHICBV("__CB0", 0, sizeof(DebugPassCB), debugGraphicsStages),
		MakeRHITextureSRV("SrcTex", 0, debugPixelStage),
		MakeRHITextureSRV("SrcTexSH", 1, debugPixelStage),
		MakeRHITextureSRV("SrcTexNormal", 2, debugPixelStage),
		MakeRHISampler("samplerWrap", 0, debugPixelStage),
	};

	BufferVisualizeGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
}

void Corona::InitLightingPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }
	};

	FullScreenVB = renderBackend->CreateVertexBuffer(sizeof(quadVertices), sizeof(PostVertex), &quadVertices);

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\LightingPS.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexStride = sizeof(PostVertex);
	desc.ColorFormats = { ETextureFormat::RGBA16Float };
	desc.bDepthEnable = false;
	desc.bCullBackFaces = false;
	desc.bTriangleStrip = true;
	desc.ConstantBufferSize = sizeof(LightingParam);
	desc.ConstantBufferBinding = 0;
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float4, 0 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 }
	};
	const RHIShaderStageMask lightingPixelStage = ToRHIShaderStageMask(RHIShaderStage::Pixel);
	const RHIShaderStageMask lightingGraphicsStages = RHIShaderStage::Vertex | RHIShaderStage::Pixel;
	desc.PipelineLayout.Bindings = {
		MakeRHICBV("__CB0", 0, sizeof(LightingParam), lightingGraphicsStages),
		MakeRHITextureSRV("AlbedoTex", 0, lightingPixelStage),
		MakeRHITextureSRV("NormalTex", 1, lightingPixelStage),
		MakeRHITextureSRV("ShadowTex", 2, lightingPixelStage),
		MakeRHITextureSRV("VelocityTex", 3, lightingPixelStage),
		MakeRHITextureSRV("DepthTex", 4, lightingPixelStage),
		MakeRHITextureSRV("GIResultSHTex", 5, lightingPixelStage),
		MakeRHITextureSRV("GIResultColorTex", 6, lightingPixelStage),
		MakeRHITextureSRV("SpecularGITex", 7, lightingPixelStage),
		MakeRHITextureSRV("RoughnessMetalicTex", 8, lightingPixelStage),
		MakeRHITextureSRV("AmbientOcclusionTex", 14, lightingPixelStage),
		MakeRHISampler("sampleWrap", 0, lightingPixelStage),
	};

	LightingGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
}

void Corona::InitMobileShadowMapPass()
{
	if (!renderBackend)
		return;

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\MobileShadowMap.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	// Default mesh layout (44 B legacy "Vertex"). Skeletal-skinned meshes
	// upload a different 48 B StandardVertex layout; the Skeletal shadow
	// PSO below overrides stride + offsets to match.
	desc.VertexStride = 44;
	desc.ColorFormats.clear();
	desc.DepthFormat = ETextureFormat::D32Float;
	desc.bDepthEnable = true;
	desc.bCullBackFaces = false;
	desc.bDepthBiasEnable = true;
	desc.DepthBiasConstantFactor = 1.25f;
	desc.DepthBiasClamp = 0.0f;
	desc.DepthBiasSlopeFactor = 2.0f;
	desc.ConstantBufferSize = sizeof(ShadowMapConstantBuffer);
	desc.ConstantBufferBinding = 0;
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0 },
		{ "NORMAL",   0, EVertexAttributeFormat::Float3, 12 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 24 },
		{ "TANGENT",  0, EVertexAttributeFormat::Float3, 32 }
	};
	const RHIShaderStageMask shadowVertexStage = ToRHIShaderStageMask(RHIShaderStage::Vertex);
	const RHIShaderStageMask shadowGraphicsStages = RHIShaderStage::Vertex | RHIShaderStage::Pixel;
	desc.PipelineLayout.Bindings = {
		MakeRHICBV("__CB0", 0, sizeof(ShadowMapConstantBuffer), shadowGraphicsStages),
	};

	MobileShadowMapGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
	if (!MobileShadowMapGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitMobileShadowMapPass] failed to create mobile shadow map pipeline");

	GraphicsPipelineDesc skeletalShadowDesc = desc;
	skeletalShadowDesc.VertexStride = 48;
	skeletalShadowDesc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float3, 0 },
		{ "NORMAL",   0, EVertexAttributeFormat::Float3, 24 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 },
		{ "TANGENT",  0, EVertexAttributeFormat::Float3, 36 },
	};
	SkeletalMobileShadowMapGraphicsPipeline = renderBackend->CreateGraphicsPipeline(skeletalShadowDesc);
	if (!SkeletalMobileShadowMapGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitMobileShadowMapPass] failed to create Skeletal mobile shadow map pipeline");

	GraphicsPipelineDesc spineDesc = desc;
	spineDesc.VertexEntryPoint = "SpineVSMain";
	spineDesc.VertexElements.clear();
	spineDesc.VertexStride = 0;
	spineDesc.PipelineLayout.Bindings.push_back(MakeRHIBufferSRV("SpineVertices", 4, shadowVertexStage));
	SpineMobileShadowMapGraphicsPipeline = renderBackend->CreateGraphicsPipeline(spineDesc);
	if (!SpineMobileShadowMapGraphicsPipeline)
		AppendCpuRuntimeTrace(L"[InitMobileShadowMapPass] failed to create Spine mobile shadow map pipeline");
}

void Corona::InitTemporalAAPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }
	};

	FullScreenVB = renderBackend->CreateVertexBuffer(sizeof(quadVertices), sizeof(PostVertex), &quadVertices);

	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\TemporalAA.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexStride = sizeof(PostVertex);
	desc.ColorFormats = { ETextureFormat::RGBA16Float };
	desc.bDepthEnable = false;
	desc.bCullBackFaces = false;
	desc.bTriangleStrip = true;
	desc.ConstantBufferSize = sizeof(TemporalAAParam);
	desc.ConstantBufferBinding = 0;
	desc.VertexElements = {
		{ "POSITION", 0, EVertexAttributeFormat::Float4, 0 },
		{ "TEXCOORD", 0, EVertexAttributeFormat::Float2, 16 }
	};
	const RHIShaderStageMask taaPixelStage = ToRHIShaderStageMask(RHIShaderStage::Pixel);
	const RHIShaderStageMask taaGraphicsStages = RHIShaderStage::Vertex | RHIShaderStage::Pixel;
	desc.PipelineLayout.Bindings = {
		MakeRHICBV("__CB0", 0, sizeof(TemporalAAParam), taaGraphicsStages),
		MakeRHITextureSRV("CurrentColorTex", 0, taaPixelStage),
		MakeRHITextureSRV("PrevColorTex", 1, taaPixelStage),
		MakeRHITextureSRV("VelocityTex", 2, taaPixelStage),
		MakeRHITextureSRV("DepthTex", 3, taaPixelStage),
		MakeRHITextureSRV("BloomTex", 4, taaPixelStage),
		MakeRHIBufferSRV("Exposure", 5, taaPixelStage),
		MakeRHISampler("sampleWrap", 0, taaPixelStage),
	};

	TemporalAAGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
}

void Corona::ToneMapPass()
{
	renderBackend->EmitGpuCrashMarker("ToneMapPass");

	if (!ToneMapGraphicsPipeline)
		return;

	Texture* ResolveTarget = GetCurrentResolveSource();
	if (!ResolveTarget)
		return;

	ToneMapCB.Offset = glm::vec4(0, 0, 0, 0);
	ToneMapCB.Scale = glm::vec4(1, 1, 0, 0);
	ToneMapCB.ToneMapMode = ToneMapMode;

	// DX12 invalidates root-table bindings when SetGraphicsRootSignature
	// is called inside BindGraphicsPipeline. Bind the pipeline first so
	// subsequent texture/sampler/CB writes land on the right root sig.
	renderBackend->BindGraphicsPipeline(ToneMapGraphicsPipeline.get());
	CreateAndBindGraphicsBindGroup(renderBackend.get(), ToneMapGraphicsPipeline.get(),
		{
			GraphicsBindGroupEntry::TextureSRV("SrcTex", ResolveTarget),
			GraphicsBindGroupEntry::SamplerBinding("sampleWrap", samplerWrap.get()),
			GraphicsBindGroupEntry::Constant(0, &ToneMapCB, sizeof(ToneMapCB)),
		});
	Texture* backbuffer = renderBackend->GetCurrentWindowRenderTarget();
	const UINT outputWidth = (backbuffer && backbuffer->Width > 0) ? backbuffer->Width : m_width;
	const UINT outputHeight = (backbuffer && backbuffer->Height > 0) ? backbuffer->Height : m_height;
	renderBackend->SetViewportAndScissor(outputWidth, outputHeight);
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());
}

void Corona::DebugPass()
{
	if (!renderBackend || !BufferVisualizeGraphicsPipeline)
	{
		bDebugDraw = false;
		return;
	}

	renderBackend->EmitGpuCrashMarker("DebugPass");

	// DX12 SetGraphicsRootSignature (inside BindGraphicsPipeline) invalidates root
	// descriptor tables. Each visualize call must bind the pipeline BEFORE writing
	// textures + CB; the same goes for the once-only sampler bind below.
	auto visualize = [&](const DebugPassCB& cb, Texture* tex) {
		if (!tex) return;
		renderBackend->BindGraphicsPipeline(BufferVisualizeGraphicsPipeline.get());
		CreateAndBindGraphicsBindGroup(renderBackend.get(), BufferVisualizeGraphicsPipeline.get(),
			{
				GraphicsBindGroupEntry::SamplerBinding("samplerWrap", samplerWrap.get()),
				GraphicsBindGroupEntry::TextureSRV("SrcTex", tex),
				GraphicsBindGroupEntry::Constant(0, &cb, sizeof(cb)),
			});
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	};
	auto visualizeMulti = [&](const DebugPassCB& cb, Texture* tex, Texture* texSH, Texture* texNormal) {
		if (!tex || !texSH || !texNormal) return;
		renderBackend->BindGraphicsPipeline(BufferVisualizeGraphicsPipeline.get());
		CreateAndBindGraphicsBindGroup(renderBackend.get(), BufferVisualizeGraphicsPipeline.get(),
			{
				GraphicsBindGroupEntry::SamplerBinding("samplerWrap", samplerWrap.get()),
				GraphicsBindGroupEntry::TextureSRV("SrcTex", tex),
				GraphicsBindGroupEntry::TextureSRV("SrcTexSH", texSH),
				GraphicsBindGroupEntry::TextureSRV("SrcTexNormal", texNormal),
				GraphicsBindGroupEntry::Constant(0, &cb, sizeof(cb)),
			});
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	};

	auto getLightingDiffuseSource = [&]() -> Texture*
	{
		if (DiffuseGIMode == EDiffuseGIMode::SPATIAL_HASH)
		{
			if (DiffuseGIHashCached)
				return DiffuseGIHashCached.get();
			if (DiffuseGIHashFiltered)
				return DiffuseGIHashFiltered.get();
		}
		if (DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE && ScreenProbeGIResolved)
			return ScreenProbeGIResolved.get();
		return DiffuseGITemporal[GIBufferWriteIndex].get();
	};

	auto getLightingDiffuseAuxSource = [&]() -> Texture*
	{
		if (DiffuseGIMode == EDiffuseGIMode::SPATIAL_HASH && DiffuseGIHashCachedAux)
			return DiffuseGIHashCachedAux.get();
		return DiffuseGITemporalAux[GIBufferWriteIndex].get();
	};



	std::vector<std::function<void(EDebugVisualization eFS)>> functions;
	functions.push_back([&](EDebugVisualization eFS){
		//raytraced shadow
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SHADOW)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		visualize(cb, ShadowBuffer.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// ray traced ambient occlusion
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RTAO)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		if (!AmbientOcclusionBuffer)
			return;
		cb.DebugMode = CHANNEL_X;
		visualize(cb, AmbientOcclusionBuffer.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// world normal
		DebugPassCB cb;

		if (eFS ==  EDebugVisualization::WORLD_NORMAL)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		visualize(cb, NormalBuffers[ColorBufferWriteIndex].get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// geom world normal
		DebugPassCB cb;
		if (eFS == EDebugVisualization::GEO_NORMAL)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		visualize(cb, GeomNormalBuffers[ColorBufferWriteIndex].get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// Blooom buffer
		DebugPassCB cb;
		if (eFS == EDebugVisualization::BLOOM)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		visualize(cb, BloomBlurPingPong[0].get());
	});


	functions.push_back([&](EDebugVisualization eFS) {
		// depth
		DebugPassCB cb;

		if (eFS == EDebugVisualization::DEPTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.ProjectionParams.z = Near;
		cb.ProjectionParams.w = Far;
		cb.DebugMode = DEPTH;
		visualize(cb, UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// raw diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RAW_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			// Grid-thumbnail slot removed — only the "Final Diffuse GI"
			// thumbnail is kept in the overview. Remaining diffuse-GI
			// debug buffers are still selectable via the fullscreen
			// dropdown above.
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, DiffuseGIRaw.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// raw diffuse gi aux
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RAW_DIFFUSE_GI_AUX)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, DiffuseGIRawAux.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// screen probe diffuse gi resolve
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIResolved)
			return;
		cb.DebugMode = RAW_COPY;
		visualize(cb, ScreenProbeGIResolved.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// nearest screen probe radiance debug
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_PROBES)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIProbeDebug)
			return;
		cb.DebugMode = RAW_COPY;
		visualize(cb, ScreenProbeGIProbeDebug.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// full-resolution screen-probe resolve history length
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_HISTORY_LENGTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIHistory[ScreenProbeGIHistoryWriteIndex])
			return;
		cb.DebugMode = HISTORY_LENGTH;
		visualize(cb, ScreenProbeGIHistory[ScreenProbeGIHistoryWriteIndex].get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// probe-atlas radiance history length
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SCREEN_PROBE_ATLAS_HISTORY_LENGTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		if (!ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex])
			return;
		cb.DebugMode = HISTORY_LENGTH;
		visualize(cb, ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex].get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// temporal filtered diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, DiffuseGITemporal[GIBufferWriteIndex].get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// resolved diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RESOLVED_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, getLightingDiffuseSource());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// final diffuse gi
		DebugPassCB cb;

		cb.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
		cb.GIBufferScale = GIBufferScale;
		if (eFS == EDebugVisualization::FINAL_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualizeMulti(cb, getLightingDiffuseSource(), getLightingDiffuseAuxSource(), NormalBuffers[ColorBufferWriteIndex].get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// albedo
		DebugPassCB cb;

		if (eFS == EDebugVisualization::ALBEDO)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else  if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, AlbedoBuffer.get());
	});
	
	functions.push_back([&](EDebugVisualization eFS) {
		// velocity
		DebugPassCB cb;

		if (eFS == EDebugVisualization::VELOCITY)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, VelocityBuffer.get());
	});
	
	functions.push_back([&](EDebugVisualization eFS) {
		// material
		DebugPassCB cb;
		if (eFS == EDebugVisualization::ROUGNESS_METALLIC)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, RoughnessMetalicBuffer.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// specular raw
		DebugPassCB cb;


		if (eFS == EDebugVisualization::SPECULAR_RAW)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, SpecularGIRaw.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// temporal filtered specular
		DebugPassCB cb;


		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_SPECULAR)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			// Grid-thumbnail slot freed — TemporalDenoisingPass no
			// longer feeds LightingPS for specular (ReSTIR in the
			// reflection raygen handles temporal). SpecularGITemporal
			// is now a stale buffer; thumbnail kept only for the
			// fullscreen dropdown for now.
			return;
		}

		cb.DebugMode = RAW_COPY;
		visualize(cb, SpecularGITemporal[GIBufferWriteIndex].get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// history length
		DebugPassCB cb;


		if (eFS == EDebugVisualization::SPEC_HISTORY_LENGTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else
		{
			// Grid-thumbnail slot freed — same reason as the
			// "temporal filtered specular" slot above.
			return;
		}

		cb.DebugMode = CHANNEL_W;
		visualize(cb, SpecularGITemporal[GIBufferWriteIndex].get());
	});

	EDebugVisualization FullScreenVisualize = EDebugVisualization::SPECULAR_RAW;

	for (auto& f : functions)
	{
		f(FullscreenDebugBuffer);
	}
}

void Corona::LightingPass()
{
	renderBackend->EmitGpuCrashMarker("LightingPass");

	renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	glm::mat4x4 InvViewMat = glm::inverse(ViewMat);
	
	// Calculate light color from sky gradient (same as raytracing modes)
	glm::vec3 normalizedLightDir = glm::normalize(LightDir);
	float lightDirT = 0.5f * (normalizedLightDir.y + 1.0f);
	glm::vec3 lightColor = glm::mix(SkyColorBottom, SkyColorTop, lightDirT);
	
	LightingParam Param;
	Param.ViewMatrix = glm::transpose(ViewMat);
	Param.InvViewMatrix = glm::transpose(InvViewMat);
	Param.InvProjMatrix = glm::transpose(InvProjMat);
	Param.ShadowViewProjectionMatrix = glm::transpose(MobileShadowViewProjMat);
	Param.LightDir = glm::vec4(normalizedLightDir, LightIntensity);
	
	Param.RTSize.x = GetRenderWidth();
	Param.RTSize.y = GetRenderHeight();

	if (IsTemporalAAEnabled())
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	const bool bMobileHybridDirectOnly =
		CORONA_PLATFORM_MOBILE &&
		RenderingMode == ERenderingMode::HYBRID;
	const uint32_t backendMaxSupportedHybridStage =
		(renderBackend && RenderingMode == ERenderingMode::HYBRID) ?
		renderBackend->GetMaxSupportedHybridStage() : 7u;
	const bool bPartialHybridDiffuseGIBringup =
		backendMaxSupportedHybridStage >= 4u &&
		backendMaxSupportedHybridStage < 7u;
	const bool bBackendSupportsSpecularGI = backendMaxSupportedHybridStage >= 3u;
	const bool bBackendSupportsDiffuseGI = backendMaxSupportedHybridStage >= 4u;
	const bool bBackendSupportsRTAO = backendMaxSupportedHybridStage >= 2u;
	Param.GIBufferScale = GIBufferScale;
	Param.LightColor = lightColor;
	Param.bEnableDiffuseGI = (!bMobileHybridDirectOnly && bBackendSupportsDiffuseGI && bEnableDiffuseGI) ? 1 : 0;
	Param.bEnableSpecularGI = (!bMobileHybridDirectOnly && bBackendSupportsSpecularGI && bEnableSpecularGI) ? 1 : 0;
	Param.bEnableDirectDiffuse = bEnableDirectDiffuse ? 1 : 0;
	Param.bEnableDirectSpecular = bEnableDirectSpecular ? 1 : 0;
	Param.bEnableRTAO = (!bMobileHybridDirectOnly && bBackendSupportsRTAO && bEnableRTAO && bRTAOOutputValidThisFrame && AmbientOcclusionBuffer) ? 1 : 0;
	Param.RTAOIndirectStrength = RTAOIndirectStrength;
	Param.RTAOIndirectFloor = RTAOIndirectFloor;
	Param.RTAODirectContactStrength = std::clamp(RTAODirectContactStrength, 0.0f, 1.0f);
	Param.SurfaceBounceStrength = std::clamp(SurfaceBounceStrength, 0.0f, 1.0f);
	Param.SurfaceBounceSaturation = std::clamp(SurfaceBounceSaturation, 0.0f, 1.0f);
	Param.LightingOutputMode = bMobileHybridDirectOnly ? 2u : EditorLightingViewMode;
	const bool bUseMobileShadowMap =
		bMobileHybridDirectOnly &&
		bMobileShadowMapValidThisFrame &&
		ShadowBuffer;
	const bool bDirectionalShadowAvailable =
		bUseMobileShadowMap ||
		(!bMobileHybridDirectOnly && bShadowOutputValidThisFrame && ShadowBuffer);
	Param.bEnableDirectionalShadow = bDirectionalShadowAvailable ? 1u : 0u;
	Param.bUseShadowMap = bUseMobileShadowMap ? 1u : 0u;
	Param.ShadowMode = (bEnableReSTIRDirectShadow && bDirectionalShadowAvailable) ? 1u : 0u;
	static bool bLoggedMissingDesktopShadowOutput = false;
	if (!bMobileHybridDirectOnly &&
		ShadowBuffer &&
		!bShadowOutputValidThisFrame &&
		!bLoggedMissingDesktopShadowOutput)
	{
		AppendCpuRuntimeTrace(L"[LightingPass] desktop shadow buffer bound before a valid shadow output; preserving previous-frame shadow visibility");
		bLoggedMissingDesktopShadowOutput = true;
	}
	if (bMobileHybridDirectOnly)
	{
		Param.AmbientSkyColorAndStrength = glm::vec4(glm::max(SkyColorTop, glm::vec3(0.0f)), 0.18f);
		Param.AmbientGroundColorAndStrength = glm::vec4(glm::max(SkyColorBottom, glm::vec3(0.0f)), 0.075f);
	}
	else
	{
		Param.AmbientSkyColorAndStrength = glm::vec4(glm::max(SkyColorTop, glm::vec3(0.0f)), 0.0f);
		Param.AmbientGroundColorAndStrength = glm::vec4(glm::max(SkyColorBottom, glm::vec3(0.0f)), 0.0f);
	}
	Param.PointLightCount = 0;
	std::vector<const PointLightState*> pointLightCandidates;
	// ReSTIR: use the shared stable-Id-ordered table so the chosen light index the
	// shadow pass writes indexes the SAME light here (and matches the GI light mask).
	// Option A keeps the full score-sorted candidate set (its channel map below sorts
	// by distance and relies on that order).
	if (bEnableReSTIRDirectShadow)
		BuildReSTIRSharedPointLights(pointLightCandidates);
	else
		BuildPointLightRenderCandidates(pointLightCandidates);
	const UINT32 maxLightingPointLights = bEnableReSTIRDirectShadow ? MaxDiffuseGIPointLights : MaxPointLights;
	if (bEnableReSTIRDirectShadow && bReSTIRMaskLightCacheValid && !ReSTIRMaskLightCache.empty())
	{
		// Shade from the exact cached table the GI mask + ReSTIR shadow pass share, so
		// the light index the shadow ray chose addresses the same light here (parity
		// holds across the persistent mask even while the camera moves).
		for (const PointLightParam& p : ReSTIRMaskLightCache)
		{
			if (Param.PointLightCount >= maxLightingPointLights)
				break;
			Param.PointLights[Param.PointLightCount++] = p;
		}
	}
	else
	{
		for (const PointLightState* pointLightPtr : pointLightCandidates)
		{
			if (!pointLightPtr || Param.PointLightCount >= maxLightingPointLights)
				continue;

			const PointLightState& pointLight = *pointLightPtr;
			const UINT32 pointLightIndex = Param.PointLightCount++;
			Param.PointLights[pointLightIndex] = BuildPointLightParam(pointLight);
		}
	}

	{
		static size_t sLastLoggedTotal = static_cast<size_t>(-1);
		static size_t sLastLoggedCandidate = static_cast<size_t>(-1);
		static UINT32 sLastLoggedSubmitted = 0xFFFFFFFFu;
		static UINT32 sLastLoggedShadowMode = 0xFFFFFFFFu;
		const size_t totalPointLights = RenderWorld.PointLights.size();
		const size_t candidatePointLights = pointLightCandidates.size();
		if (totalPointLights != sLastLoggedTotal ||
			candidatePointLights != sLastLoggedCandidate ||
			Param.PointLightCount != sLastLoggedSubmitted ||
			Param.ShadowMode != sLastLoggedShadowMode)
		{
			sLastLoggedTotal = totalPointLights;
			sLastLoggedCandidate = candidatePointLights;
			sLastLoggedSubmitted = Param.PointLightCount;
			sLastLoggedShadowMode = Param.ShadowMode;
			AppendCpuRuntimeTrace(
				L"[LightingPass][PointLights] total=" + std::to_wstring(totalPointLights) +
				L", candidates=" + std::to_wstring(candidatePointLights) +
				L", submitted=" + std::to_wstring(Param.PointLightCount) +
				L", max=" + std::to_wstring(maxLightingPointLights) +
				L", shadowMode=" + std::to_wstring(Param.ShadowMode) +
				L", shadowValid=" + std::to_wstring(bDirectionalShadowAvailable ? 1 : 0));
		}
	}

	// Initialize shadow channel map to "no shadow" sentinel for every
	// slot. Option A fills the entries for its top-3 in-frustum closest
	// lights below; ReSTIR leaves the map unused.
	for (auto& slot : Param.ShadowChannelMap)
		slot = glm::uvec4(0xFFFFFFFFu);

	if (!bEnableReSTIRDirectShadow && Param.PointLightCount > 0)
	{
		// Mirror the RaytraceShadowPass candidate-selection (in-frustum
		// closest 3). The shadow pass writes ShadowResult.g/b/a in the
		// order it picked, so the channel index here matches the C++
		// candidate order. Use the same sort criterion (distance² to
		// camera) so the two stay in lockstep frame to frame.
		struct LocalCandidate { uint32_t LightingPSIndex; float DistSq; };
		std::vector<LocalCandidate> cands;
		cands.reserve(Param.PointLightCount);
		const glm::vec3 camPos = RenderFrameCameraPosition;
		for (uint32_t i = 0; i < Param.PointLightCount; ++i)
		{
			if (i >= pointLightCandidates.size() ||
				!pointLightCandidates[i] ||
				!pointLightCandidates[i]->bCastShadow)
			{
				continue;
			}
			const glm::vec3 pos = glm::vec3(Param.PointLights[i].PositionAndRadius);
			const glm::vec3 toLight = pos - camPos;
			cands.push_back({ i, glm::dot(toLight, toLight) });
		}
		std::sort(cands.begin(), cands.end(),
			[](const LocalCandidate& a, const LocalCandidate& b) { return a.DistSq < b.DistSq; });
		const uint32_t pick = std::min<uint32_t>(static_cast<uint32_t>(cands.size()), 3u);
		for (uint32_t k = 0; k < pick; ++k)
		{
			const uint32_t lpsIdx = cands[k].LightingPSIndex;
			Param.ShadowChannelMap[lpsIdx >> 2u][lpsIdx & 3u] = k;
		}
	}

	glm::normalize(Param.LightDir);
	const bool bDiffuseGIEnabledThisFrame = Param.bEnableDiffuseGI != 0;
	const bool bSpecularGIEnabledThisFrame = Param.bEnableSpecularGI != 0;
	const wchar_t* lightingDiffuseSource = L"black";
	Texture* lightingDiffuseAuxTex = DefaultBlackTex.get();
	// Diffuse GI source for LightingPS — same logic as specular below:
	// the second-stage screen-space TemporalDenoisingPass reprojects
	// with the surface motion vector, which smears prev-frame indirect
	// onto disoccluded pixels during camera panning (the indirect
	// bounce doesn't follow the surface). The SpatialHashGI cache is
	// world-space, so its cell-level temporal already covers the
	// "stable across frames" axis correctly. Prefer the direct
	// per-pixel query result (DiffuseGIHashCached) when SpatialHash
	// is active; DiffuseGIHashFiltered remains a diagnostic/polish
	// buffer, not the authoritative lighting input.
	Texture* lightingDiffuseTex = DefaultBlackTex.get();
	if (!bMobileHybridDirectOnly &&
		bDiffuseGIEnabledThisFrame &&
		bPartialHybridDiffuseGIBringup &&
		!IsDLSSRREnabled())
	{
		// Partial hybrid NON-RR path: feed the screen-space TemporalDenoisingPass
		// output (DiffuseGITemporal — fed by the spatial-hash query, see
		// bUseSpatialHashDiffuseInput) so the composite is denoised without DLSS-RR.
		// Under DLSS-RR this branch is skipped so the flow falls through to the
		// SPATIAL_HASH branch below and RR owns denoising from the raw cached query
		// (RR denoises best from un-temporally-filtered input).
		if (bEnableTemporalDenoisingPass &&
			backendMaxSupportedHybridStage >= 5u &&
			DiffuseGITemporal[GIBufferWriteIndex])
		{
			lightingDiffuseTex = DiffuseGITemporal[GIBufferWriteIndex].get();
			lightingDiffuseSource = L"temporal_partial_hybrid";
			if (DiffuseGITemporalAux[GIBufferWriteIndex])
				lightingDiffuseAuxTex = DiffuseGITemporalAux[GIBufferWriteIndex].get();
		}
		else if (DiffuseGIHashCached)
		{
			lightingDiffuseTex = DiffuseGIHashCached.get();
			lightingDiffuseSource = L"spatial_hash_cached_partial_hybrid_temporal_disabled";
			if (DiffuseGIHashCachedAux)
				lightingDiffuseAuxTex = DiffuseGIHashCachedAux.get();
		}
		else if (DiffuseGIRaw)
		{
			lightingDiffuseTex = DiffuseGIRaw.get();
			lightingDiffuseSource = L"raw_partial_hybrid";
			if (DiffuseGIRawAux)
				lightingDiffuseAuxTex = DiffuseGIRawAux.get();
		}
	}
	else if (!bMobileHybridDirectOnly &&
		bDiffuseGIEnabledThisFrame &&
		DiffuseGIMode == EDiffuseGIMode::SPATIAL_HASH &&
		DiffuseGIHashCached)
	{
		lightingDiffuseTex = DiffuseGIHashCached.get();
		lightingDiffuseSource = L"spatial_hash_cached";
		if (DiffuseGIHashCachedAux)
			lightingDiffuseAuxTex = DiffuseGIHashCachedAux.get();
	}
	else if (!bMobileHybridDirectOnly && bDiffuseGIEnabledThisFrame)
	{
		// Under DLSS Ray Reconstruction, feed the RAW (un-reprojected) diffuse GI and
		// let RR own the temporal/disocclusion pass — exactly like the specular path
		// below. The screen-space TemporalDenoisingPass reprojects with the surface
		// motion vector, which smears prev-frame indirect onto disoccluded pixels
		// (ghosting/trailing under camera motion); RR reconstructs motion and
		// disocclusion natively from its guide buffers, so that pre-temporal stage only
		// adds ghosting here. Non-RR paths (TAA/off) keep the temporal stage they rely on.
		if (IsDLSSRREnabled() && bFeedRawGIToRR && DiffuseGIRaw)
		{
			if (bEnableSimpleGISpatialFilter && DiffuseGISpatialFiltered)
			{
				// Spatially pre-filtered (no temporal reproject) — RR-idiomatic: calms
				// the 1spp variance (less dolly flicker) without any ghosting.
				lightingDiffuseTex = DiffuseGISpatialFiltered.get();
				lightingDiffuseSource = L"spatial_filtered_for_rr";
				if (DiffuseGISpatialFilteredAux)
					lightingDiffuseAuxTex = DiffuseGISpatialFilteredAux.get();
			}
			else
			{
				lightingDiffuseTex = DiffuseGIRaw.get();
				lightingDiffuseSource = L"raw_for_rr";
				if (DiffuseGIRawAux)
					lightingDiffuseAuxTex = DiffuseGIRawAux.get();
			}
		}
		else if (IsDLSSRREnabled() &&
			bEnableTemporalDenoisingPass &&
			bEnableGIDisocclusionFilter &&
			DiffuseGISpatialFiltered)
		{
			// Temporally accumulated, then variance-guided disocclusion-cleaned for RR.
			lightingDiffuseTex = DiffuseGISpatialFiltered.get();
			lightingDiffuseSource = L"temporal_disoccl_filtered";
			if (DiffuseGISpatialFilteredAux)
				lightingDiffuseAuxTex = DiffuseGISpatialFilteredAux.get();
		}
		else if (bEnableTemporalDenoisingPass && DiffuseGITemporal[GIBufferWriteIndex])
		{
			lightingDiffuseTex = DiffuseGITemporal[GIBufferWriteIndex].get();
			lightingDiffuseSource = L"temporal";
			if (DiffuseGITemporalAux[GIBufferWriteIndex])
				lightingDiffuseAuxTex = DiffuseGITemporalAux[GIBufferWriteIndex].get();
		}
		else if (DiffuseGIRaw)
		{
			lightingDiffuseTex = DiffuseGIRaw.get();
			lightingDiffuseSource = L"raw_temporal_disabled";
			if (DiffuseGIRawAux)
				lightingDiffuseAuxTex = DiffuseGIRawAux.get();
		}
	}
	if (!bMobileHybridDirectOnly &&
		bDiffuseGIEnabledThisFrame &&
		DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE &&
		ScreenProbeGIResolved)
	{
		lightingDiffuseTex = ScreenProbeGIResolved.get();
		lightingDiffuseSource = L"screen_probe";
	}
	// Keep specular GI raw for the final lighting input. DLSS-RR should see the
	// native specular signal and own reconstruction.
	const wchar_t* lightingSpecularSource = L"black";
	Texture* lightingSpecularTex = DefaultBlackTex.get();
	if (!bMobileHybridDirectOnly && bSpecularGIEnabledThisFrame &&
		bPartialHybridDiffuseGIBringup && backendMaxSupportedHybridStage >= 5u &&
		!IsDLSSRREnabled() &&
		SpecularGITemporal[GIBufferWriteIndex])
	{
		// Partial hybrid NON-RR: consume the screen-space TemporalDenoising denoised
		// specular instead of the raw 1-spp reflections. Under DLSS-RR fall through
		// to raw so RR owns specular denoising.
		lightingSpecularTex = SpecularGITemporal[GIBufferWriteIndex].get();
		lightingSpecularSource = L"temporal_partial_hybrid";
	}
	else if (!bMobileHybridDirectOnly && bSpecularGIEnabledThisFrame && SpecularGIRaw)
	{
		lightingSpecularTex = SpecularGIRaw.get();
		lightingSpecularSource = L"raw";
	}
	{
		static UINT32 sLastDiffuseEnabled = 0xFFFFFFFFu;
		static UINT32 sLastSpecularEnabled = 0xFFFFFFFFu;
		static UINT32 sLastLightingOutputMode = 0xFFFFFFFFu;
		static int sLastDiffuseMode = -1;
		static int sLastAA = -1;
		static uintptr_t sLastDiffuseTex = 0;
		static uintptr_t sLastSpecularTex = 0;
		static float sLastSurfaceBounceStrength = -1.0f;
		static float sLastSurfaceBounceSaturation = -1.0f;
		const uintptr_t diffuseTexId = reinterpret_cast<uintptr_t>(lightingDiffuseTex);
		const uintptr_t specularTexId = reinterpret_cast<uintptr_t>(lightingSpecularTex);
		const int diffuseMode = static_cast<int>(DiffuseGIMode);
		const int aaMode = static_cast<int>(AntiAliasingMode);
		if (sLastDiffuseEnabled != Param.bEnableDiffuseGI ||
			sLastSpecularEnabled != Param.bEnableSpecularGI ||
			sLastLightingOutputMode != Param.LightingOutputMode ||
			sLastDiffuseMode != diffuseMode ||
			sLastAA != aaMode ||
			sLastDiffuseTex != diffuseTexId ||
			sLastSpecularTex != specularTexId ||
			std::abs(sLastSurfaceBounceStrength - Param.SurfaceBounceStrength) > 0.0001f ||
			std::abs(sLastSurfaceBounceSaturation - Param.SurfaceBounceSaturation) > 0.0001f)
		{
			sLastDiffuseEnabled = Param.bEnableDiffuseGI;
			sLastSpecularEnabled = Param.bEnableSpecularGI;
			sLastLightingOutputMode = Param.LightingOutputMode;
			sLastDiffuseMode = diffuseMode;
			sLastAA = aaMode;
			sLastDiffuseTex = diffuseTexId;
			sLastSpecularTex = specularTexId;
			sLastSurfaceBounceStrength = Param.SurfaceBounceStrength;
			sLastSurfaceBounceSaturation = Param.SurfaceBounceSaturation;
			AppendCpuRuntimeTrace(
				L"[LightingPass][GI] diffuseEnable=" + std::to_wstring(Param.bEnableDiffuseGI) +
				L", diffuseSource=" + std::wstring(lightingDiffuseSource) +
				L", specularEnable=" + std::to_wstring(Param.bEnableSpecularGI) +
				L", specularSource=" + std::wstring(lightingSpecularSource) +
				L", surfaceBounceStrength=" + std::to_wstring(Param.SurfaceBounceStrength) +
				L", surfaceBounceSaturation=" + std::to_wstring(Param.SurfaceBounceSaturation) +
				L", outputMode=" + std::to_wstring(Param.LightingOutputMode) +
				L", diffuseMode=" + std::to_wstring(diffuseMode) +
				L", aa=" + std::to_wstring(aaMode) +
				L", render=" + std::to_wstring(Param.RTSize.x) +
				L"x" + std::to_wstring(Param.RTSize.y));
		}
	}
	Texture* shadowTex =
		bDirectionalShadowAvailable ?
		ShadowBuffer.get() :
		DefaultWhiteTex.get();
	Texture* ambientOcclusionTex =
		(!bMobileHybridDirectOnly && AmbientOcclusionBuffer) ?
		AmbientOcclusionBuffer.get() :
		DefaultWhiteTex.get();

	{
		static UINT32 sLastRTAOEnable = 0xFFFFFFFFu;
		static UINT32 sLastRTAORequested = 0xFFFFFFFFu;
		static UINT32 sLastRTAOValid = 0xFFFFFFFFu;
		static float sLastRTAOContactStrength = -1.0f;
		static float sLastRTAOAOStrength = -1.0f;
		static float sLastRTAOAOFloor = -1.0f;
		static float sLastRTAORadius = -1.0f;
		static float sLastRTAOPower = -1.0f;
		static float sLastRTAONormalBias = -1.0f;
		static UINT32 sLastRTAOSamples = 0xFFFFFFFFu;
		const UINT32 rtaoRequested = bEnableRTAO ? 1u : 0u;
		const UINT32 rtaoValid = bRTAOOutputValidThisFrame ? 1u : 0u;
		if (sLastRTAOEnable != Param.bEnableRTAO ||
			sLastRTAORequested != rtaoRequested ||
			sLastRTAOValid != rtaoValid ||
			std::abs(sLastRTAOContactStrength - Param.RTAODirectContactStrength) > 0.0001f ||
			std::abs(sLastRTAOAOStrength - Param.RTAOIndirectStrength) > 0.0001f ||
			std::abs(sLastRTAOAOFloor - Param.RTAOIndirectFloor) > 0.0001f ||
			std::abs(sLastRTAORadius - RTAOViewParam.Radius) > 0.0001f ||
			std::abs(sLastRTAOPower - RTAOViewParam.Power) > 0.0001f ||
			std::abs(sLastRTAONormalBias - RTAOViewParam.NormalBias) > 0.0001f ||
			sLastRTAOSamples != RTAOViewParam.SampleCount)
		{
			sLastRTAOEnable = Param.bEnableRTAO;
			sLastRTAORequested = rtaoRequested;
			sLastRTAOValid = rtaoValid;
			sLastRTAOContactStrength = Param.RTAODirectContactStrength;
			sLastRTAOAOStrength = Param.RTAOIndirectStrength;
			sLastRTAOAOFloor = Param.RTAOIndirectFloor;
			sLastRTAORadius = RTAOViewParam.Radius;
			sLastRTAOPower = RTAOViewParam.Power;
			sLastRTAONormalBias = RTAOViewParam.NormalBias;
			sLastRTAOSamples = RTAOViewParam.SampleCount;
			AppendCpuRuntimeTrace(
				L"[LightingPass][RTAO] enable=" + std::to_wstring(Param.bEnableRTAO) +
				L", requested=" + std::to_wstring(rtaoRequested) +
				L", valid=" + std::to_wstring(rtaoValid) +
				L", contactStrength=" + std::to_wstring(Param.RTAODirectContactStrength) +
				L", aoStrength=" + std::to_wstring(Param.RTAOIndirectStrength) +
				L", aoFloor=" + std::to_wstring(Param.RTAOIndirectFloor) +
				L", radius=" + std::to_wstring(RTAOViewParam.Radius) +
				L", power=" + std::to_wstring(RTAOViewParam.Power) +
				L", normalBias=" + std::to_wstring(RTAOViewParam.NormalBias) +
				L", samples=" + std::to_wstring(RTAOViewParam.SampleCount));
		}
	}

	if (!LightingGraphicsPipeline)
		return;

	// DX12 SetGraphicsRootSignature (called inside BindGraphicsPipeline) invalidates
	// previously-written root descriptor tables — bind the pipeline first, then write
	// textures/samplers/CB. Same order as InitGBufferPass + DrawScene.
	Texture* lightingTarget = LightingBuffer.get();
	renderBackend->SetRenderTargets(&lightingTarget, 1, nullptr);
	renderBackend->BindGraphicsPipeline(LightingGraphicsPipeline.get());
	auto bindLightingGroup = [&]()
	{
		CreateAndBindGraphicsBindGroup(renderBackend.get(), LightingGraphicsPipeline.get(),
			{
				GraphicsBindGroupEntry::TextureSRV("AlbedoTex", AlbedoBuffer.get()),
				GraphicsBindGroupEntry::TextureSRV("NormalTex", NormalBuffers[ColorBufferWriteIndex].get()),
				GraphicsBindGroupEntry::TextureSRV("ShadowTex", shadowTex),
				GraphicsBindGroupEntry::TextureSRV("VelocityTex", VelocityBuffer.get()),
				GraphicsBindGroupEntry::TextureSRV("DepthTex", DepthBuffer.get()),
				GraphicsBindGroupEntry::TextureSRV("GIResultSHTex", lightingDiffuseAuxTex),
				GraphicsBindGroupEntry::TextureSRV("GIResultColorTex", lightingDiffuseTex),
				GraphicsBindGroupEntry::TextureSRV("SpecularGITex", lightingSpecularTex),
				GraphicsBindGroupEntry::TextureSRV("RoughnessMetalicTex", RoughnessMetalicBuffer.get()),
				GraphicsBindGroupEntry::TextureSRV("AmbientOcclusionTex", ambientOcclusionTex),
				GraphicsBindGroupEntry::SamplerBinding("sampleWrap", samplerWrap.get()),
				GraphicsBindGroupEntry::Constant(0, &Param, sizeof(Param)),
			});
	};
	bindLightingGroup();
	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);

	if (bAutoAADumpEnabled && DirectLightingBuffer)
	{
		Param.LightingOutputMode = 1;
		renderBackend->TransitionTexture(DirectLightingBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
		Texture* directLightingTarget = DirectLightingBuffer.get();
		renderBackend->SetRenderTargets(&directLightingTarget, 1, nullptr);
		// Re-bind pipeline before re-writing CB — same DX12 root-table invalidation rule.
		renderBackend->BindGraphicsPipeline(LightingGraphicsPipeline.get());
		bindLightingGroup();
		renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
		renderBackend->TransitionTexture(DirectLightingBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	}
	renderBackend->BindGraphicsBindGroup(LightingGraphicsPipeline.get(), kGraphicsBindGroupSlot_All, nullptr);
}

void Corona::TemporalAAPass()
{
	renderBackend->EmitGpuCrashMarker("TemporalAAPass");

	UINT PrevColorBufferIndex = 1 - ColorBufferWriteIndex;
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex].get();

	if (!TemporalAAGraphicsPipeline || !ResolveTarget || !LightingBuffer || !ExposureData)
	{
		bUseLightingBufferFallbackForToneMap = true;
		bTemporalAAHistoryValid = false;
		return;
	}

	Texture* PrevColorBuffer = ColorBuffers[PrevColorBufferIndex].get();
	Texture* BloomTexture = BloomBlurPingPong[0] ? BloomBlurPingPong[0].get() : DefaultBlackTex.get();
	if (!PrevColorBuffer || !BloomTexture)
	{
		bUseLightingBufferFallbackForToneMap = true;
		bTemporalAAHistoryValid = false;
		return;
	}

	TemporalAAParam Param;
	Param.RTSize.x = GetRenderWidth();
	Param.RTSize.y = GetRenderHeight();
	Param.TAABlendFactor = IsTemporalAAEnabled() ? 0.1f : 1.0f;
	Param.ClampMode = ClampMode;
	Param.BloomStrength = BloomBlurPingPong[0] ? BloomStrength : 0.0f;
	Param.HistoryValid = bTemporalAAHistoryValid ? 1u : 0u;
	Param.CurrentJitter = IsJitterEnabled() ? (CurrentJitter * 0.5f) : glm::vec2(0.0f);

	// DX12 root-table invalidation: bind pipeline before per-pipeline writes.
	renderBackend->TransitionTexture(ResolveTarget, EResourceState::ShaderRead, EResourceState::RenderTarget);
	Texture* temporalTarget = ResolveTarget;
	renderBackend->SetRenderTargets(&temporalTarget, 1, nullptr);
	renderBackend->BindGraphicsPipeline(TemporalAAGraphicsPipeline.get());
	CreateAndBindGraphicsBindGroup(renderBackend.get(), TemporalAAGraphicsPipeline.get(),
		{
			GraphicsBindGroupEntry::TextureSRV("CurrentColorTex", LightingBuffer.get()),
			GraphicsBindGroupEntry::TextureSRV("PrevColorTex", PrevColorBuffer),
			GraphicsBindGroupEntry::TextureSRV("VelocityTex", VelocityBuffer.get()),
			GraphicsBindGroupEntry::TextureSRV("DepthTex", DepthBuffer.get()),
			GraphicsBindGroupEntry::TextureSRV("BloomTex", BloomTexture),
			GraphicsBindGroupEntry::BufferSRV("Exposure", ExposureData.get()),
			GraphicsBindGroupEntry::SamplerBinding("sampleWrap", samplerBilinearWrap ? samplerBilinearWrap.get() : samplerWrap.get()),
			GraphicsBindGroupEntry::Constant(0, &Param, sizeof(Param)),
		});
	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	renderBackend->TransitionTexture(ResolveTarget, EResourceState::RenderTarget, EResourceState::ShaderRead);

	if (bDrawHistogram && DrawHistogramPSO)
	{
		renderBackend->TransitionTexture(ResolveTarget, EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		DrawHistogramPSO->SetBufferSRV("Histogram", Histogram.get());
		DrawHistogramPSO->SetBufferSRV("Exposure", ExposureData.get());
		DrawHistogramPSO->SetTextureUAV("ColorBuffer", ResolveTarget);
		DrawHistogramPSO->Apply();
		renderBackend->Dispatch(1, 32, 1);
		renderBackend->TransitionTexture(ResolveTarget, EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	}

	bTemporalAAHistoryValid = IsTemporalAAEnabled();
	bUseLightingBufferFallbackForToneMap = false;
	ResolvedColorBufferIndex = ColorBufferWriteIndex;
}

void Corona::BloomPass()
{
	renderBackend->EmitGpuCrashMarker("BloomPass");

	BloomCB.RTSize.x = BloomBufferWidth;
	BloomCB.RTSize.y = BloomBufferHeight;

	float sigma_pixels = BloomSigma * m_height;

	float effective_sigma = sigma_pixels * 0.25f;
	effective_sigma = glm::min(effective_sigma, 100.f);
	effective_sigma = glm::max(effective_sigma, 1.f);
	BloomCB.NumSamples = glm::round(effective_sigma*4.f);
	BloomCB.WeightScale = -1.f / (2.0 * effective_sigma * effective_sigma);
	BloomCB.NormalizationScale = 1.f / (sqrtf(2 * glm::pi<float>()) * effective_sigma);;
	//BloomCB.Exposure = Exposure;
	/*BloomCB.MinLog = kInitialMinLog;
	BloomCB.RcpLogRange = 1.0f / (kInitialMaxLog - kInitialMinLog);*/

	// extraction pass
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(LumaBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	BloomExtractPSO->SetTextureSRV("SrcTex", LightingBuffer.get());
	BloomExtractPSO->SetBufferSRV("Exposure", ExposureData.get());
	BloomExtractPSO->SetTextureUAV("DstTex", BloomBlurPingPong[0].get());
	BloomExtractPSO->SetTextureUAV("LumaResult", LumaBuffer.get());
	BloomExtractPSO->SetSampler("samplerWrap", samplerWrap.get());
	BloomExtractPSO->SetCBVValue("BloomCB", &BloomCB);
	BloomExtractPSO->Apply();

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(LumaBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// horizontal pass
	renderBackend->TransitionTexture(BloomBlurPingPong[1].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	BloomBlurPSO->SetTextureSRV("SrcTex", BloomBlurPingPong[0].get());
	BloomBlurPSO->SetTextureUAV("DstTex", BloomBlurPingPong[1].get());
	BloomBlurPSO->SetSampler("samplerWrap", samplerWrap.get());
	BloomCB.BlurDirection = glm::vec2(1, 0);
	BloomBlurPSO->SetCBVValue("BloomCB", &BloomCB);
	BloomBlurPSO->Apply();

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);
	renderBackend->TransitionTexture(BloomBlurPingPong[1].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// vertical pass
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	BloomBlurPSO->SetTextureSRV("SrcTex", BloomBlurPingPong[1].get());
	BloomBlurPSO->SetTextureUAV("DstTex", BloomBlurPingPong[0].get());
	BloomBlurPSO->SetSampler("samplerWrap", samplerWrap.get());
	BloomCB.BlurDirection = glm::vec2(0, 1);
	BloomBlurPSO->SetCBVValue("BloomCB", &BloomCB);
	BloomBlurPSO->Apply();

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// histogram pass
	renderBackend->TransitionBuffer(Histogram.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	ClearHistogramPSO->SetBufferUAV("Histogram", Histogram.get());
	ClearHistogramPSO->Apply();
	renderBackend->Dispatch(1, 1, 1);

	HistogramPSO->SetTextureSRV("LumaTex", LumaBuffer.get());
	HistogramPSO->SetBufferUAV("Histogram", Histogram.get());
	HistogramPSO->Apply();
	renderBackend->Dispatch(BloomBufferWidth / 16, 1, 1);

	renderBackend->TransitionBuffer(Histogram.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// adapte exposure pass
	renderBackend->TransitionBuffer(ExposureData.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	AdapteExposurePSO->SetBufferSRV("Histogram", Histogram.get());
	AdapteExposurePSO->SetBufferUAV("Exposure", ExposureData.get());
	AdaptExposureCB.PixelCount = BloomBufferWidth * BloomBufferHeight;
	AdapteExposurePSO->SetCBVValue("AdaptExposureCB", &AdaptExposureCB);
	AdapteExposurePSO->Apply();

	renderBackend->Dispatch(1, 1, 1);
	renderBackend->TransitionBuffer(ExposureData.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}

void Corona::DispatchSpineSkinningForMesh(Mesh* mesh)
{
	// Spine VS-inline mode bypasses the compute pre-pass; the VS reads the
	// input SBVs directly each frame. Mark the mesh "dispatched" so the
	// downstream readiness checks treat it as ready to draw.
	if (bSpineUseVsInlineSkinning && mesh && mesh->bGpuSpineSkinned)
	{
		mesh->bGpuSpineSkinningDispatched = true;
		return;
	}

	if (!renderBackend ||
		!bEnableGpuSpineSkinning ||
		!SpineSkinningPSO ||
		!mesh ||
		!mesh->bGpuSpineSkinned ||
		mesh->bGpuSpineSkinningDispatched ||
		mesh->GpuSpineSkinningVertexCount == 0 ||
		!mesh->GpuSpineInputVertices ||
		!mesh->GpuSpineInfluences ||
		!mesh->GpuSpineBones ||
		!mesh->GpuSpineSkinnedVertices)
	{
		return;
	}


	SpineSkinningConstant constants = {};
	constants.VertexCount = mesh->GpuSpineSkinningVertexCount;
	constants.SourceScale = mesh->GpuSpineSkinningSourceScale;

	renderBackend->TransitionBuffer(mesh->GpuSpineSkinnedVertices.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	SpineSkinningPSO->SetBufferSRV("InputVertices", mesh->GpuSpineInputVertices.get());
	SpineSkinningPSO->SetBufferSRV("Influences", mesh->GpuSpineInfluences.get());
	SpineSkinningPSO->SetBufferSRV("Bones", mesh->GpuSpineBones.get());
	SpineSkinningPSO->SetBufferUAV("OutputVertices", mesh->GpuSpineSkinnedVertices.get());
	SpineSkinningPSO->SetCBVValue("SpineSkinningConstant", &constants);
	SpineSkinningPSO->Apply();
	renderBackend->Dispatch((mesh->GpuSpineSkinningVertexCount + 63u) / 64u, 1, 1);
	renderBackend->TransitionBuffer(mesh->GpuSpineSkinnedVertices.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	mesh->bGpuSpineSkinningDispatched = true;
}

void Corona::DispatchSpineSkinningForScene(const shared_ptr<Scene>& scene)
{
	if (!scene)
		return;

	for (const auto& mesh : scene->meshes)
		DispatchSpineSkinningForMesh(mesh.get());
}

void Corona::DispatchSpineSkinningForRenderWorld()
{
	if (!renderBackend || !bEnableGpuSpineSkinning)
		return;

	UploadLiveSpineTransientBonesForRender();
	if (!SpineSkinningPSO && !bSpineUseVsInlineSkinning)
		return;

	static thread_local std::unordered_set<const Scene*> s_spineSkinningVisitedScenes;
	s_spineSkinningVisitedScenes.clear();
	s_spineSkinningVisitedScenes.reserve(256);
	for (const SceneObject& object : RenderWorld.SceneObjects)
	{
		if (!object.bVisible || !object.ScenePtr)
			continue;
		const Scene* sceneKey = object.ScenePtr.get();
		if (!s_spineSkinningVisitedScenes.insert(sceneKey).second)
			continue;
		DispatchSpineSkinningForScene(object.ScenePtr);
	}
}

bool Corona::IsSceneEligibleForStaticGBufferInstancing(const std::shared_ptr<Scene>& scene) const
{
	if (!scene || !StaticInstancedGBufferGraphicsPipeline || scene->meshes.empty())
		return false;

	for (const std::shared_ptr<Mesh>& mesh : scene->meshes)
	{
		if (!mesh || !mesh->Vb || !mesh->Ib || mesh->Draws.empty())
			return false;
		if (mesh->bProceduralGrass || mesh->bGpuSpineSkinned || mesh->bSpineMesh || mesh->bSkeletalSkinned)
			return false;
		if (mesh->bTerrainMesh || mesh->bGrassMesh)
			return false;
		for (const Mesh::DrawCall& drawcall : mesh->Draws)
		{
			if (!drawcall.mat || drawcall.IndexCount == 0)
				return false;
		}
	}
	return true;
}

bool Corona::DrawStaticInstancedScene(
	const std::shared_ptr<Scene>& scene,
	const std::vector<const SceneObject*>& objects,
	float roughness,
	float metallic,
	bool overrideRoughnessMetallic)
{
	if (!scene || objects.empty() || !StaticInstancedGBufferGraphicsPipeline)
		return false;

	const uint32_t instanceCount = static_cast<uint32_t>(objects.size());
	StaticGBufferInstanceTransformScratch.resize(instanceCount);

	GBufferGeometryTable geometryTable = BuildGBufferGeometryTable(renderBackend.get(), scene.get());
	bool bUseBindlessGeometry =
		StaticInstancedBindlessGBufferGraphicsPipeline &&
		geometryTable.Buffer &&
		IsGBufferSceneStaticBindlessGeometryEligible(scene.get());
	GraphicsPipelineHandle* pso = bUseBindlessGeometry ?
		StaticInstancedBindlessGBufferGraphicsPipeline.get() :
		StaticInstancedGBufferGraphicsPipeline.get();

	auto packWorld = [](const glm::mat4x4& world, StaticGBufferInstanceXform& dst)
	{
		const glm::mat4x4 t = glm::transpose(world);
		dst.r0[0] = t[0][0]; dst.r0[1] = t[0][1]; dst.r0[2] = t[0][2]; dst.r0[3] = t[0][3];
		dst.r1[0] = t[1][0]; dst.r1[1] = t[1][1]; dst.r1[2] = t[1][2]; dst.r1[3] = t[1][3];
		dst.r2[0] = t[2][0]; dst.r2[1] = t[2][1]; dst.r2[2] = t[2][2]; dst.r2[3] = t[2][3];
	};

	renderBackend->BindGraphicsPipeline(pso);

	for (uint32_t i = 0; i < instanceCount; ++i)
		packWorld(objects[i]->Transform, StaticGBufferInstanceTransformScratch[i]);

	std::shared_ptr<Buffer> instanceBuffer = renderBackend->AllocateTransientUploadStructuredBuffer(
		instanceCount,
		static_cast<uint32_t>(sizeof(StaticGBufferInstanceXform)),
		StaticGBufferInstanceTransformScratch.data());
	if (!instanceBuffer)
		return false;

	GBufferMaterialTable materialTable = BuildGBufferMaterialTable(
		renderBackend.get(),
		scene.get(),
		DefaultWhiteTex.get(),
		DefaultNormalTex.get(),
		DefaultRougnessTex.get(),
		DefaultBlackTex.get());
	if (!materialTable.Buffer)
		return false;

	BindGBufferSceneResourceBindGroup(
		renderBackend.get(),
		pso,
		scene.get(),
		samplerWrap.get(),
		materialTable.Buffer.get(),
		bUseBindlessGeometry ? geometryTable.Buffer.get() : nullptr,
		GBufferDummyDrawRecordBuffer.get());

	for (const std::shared_ptr<Mesh>& mesh : scene->meshes)
	{
		if (!mesh || !mesh->Vb || !mesh->Ib)
			return false;

		if (!bUseBindlessGeometry)
			renderBackend->BindMeshBuffers(mesh->Vb.get(), mesh->Ib.get());

		for (const Mesh::DrawCall& drawcall : mesh->Draws)
		{
			GBufferConstantBuffer objCB = {};
			objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
			objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);
			objCB.WorldMatrix = glm::transpose(mesh->transform);
			objCB.UnjitteredViewProjMat = glm::transpose(UnjitteredViewProjMat);
			objCB.PrevUnjitteredViewProjMat = glm::transpose(PrevUnjitteredViewProjMat);
			objCB.ViewDir.x = RenderFrameCameraLookDirection.x;
			objCB.ViewDir.y = RenderFrameCameraLookDirection.y;
			objCB.ViewDir.z = RenderFrameCameraLookDirection.z;
			objCB.ViewDir.w = 0.0f;
			objCB.BaseColorFactor = drawcall.mat ? drawcall.mat->BaseColorFactor : glm::vec4(1.0f);
			objCB.RTSize.x = GetRenderWidth();
			objCB.RTSize.y = GetRenderHeight();
			objCB.RougnessMetalic.x = roughness;
			objCB.RougnessMetalic.y = metallic;
			objCB.bOverrideRougnessMetallic = overrideRoughnessMetallic ? 1u : 0u;
			objCB.bTwoSidedLighting = 0u;
			objCB.bUnlitMaterial = 0u;
			objCB.SpineVertexBase = 0u;
			objCB.SkeletalCharIndex = 0u;
			objCB.SkeletalVertsPerChar = 0u;
			objCB.SkeletalBoneCount = 0u;
			objCB.MeshDeformParams = glm::vec4(RenderFrameWindTime, 0.0f, 0.0f, 0.0f);
			objCB.GrassBendOrigin = RenderFrameGrassBendOrigin;
			objCB.GrassBendParams = RenderFrameGrassBendParams;
			objCB.bGrassMesh = mesh->bGrassMesh ? 1u : 0u;
			objCB.bTerrainMesh = mesh->bTerrainMesh ? 1u : 0u;
			objCB.bExcludeFromDeformSphere = mesh->bExcludeFromDeformSphere ? 1u : 0u;
			objCB.WindParams = RenderFrameWindParams;
			objCB.WindTuning = RenderFrameWindTuning;
			objCB.TerrainDeformSphere = RenderFrameTerrainDeformSphere;
			if (bUseBindlessGeometry)
			{
				objCB.GBufferGeometryIndex = mesh->GBufferGeometryIndex;
				objCB.GBufferIndexStart = drawcall.IndexStart;
				objCB.GBufferVertexBase = drawcall.VertexBase;
				objCB.bGBufferBindlessGeometry = 1u;
			}
			objCB.GBufferMaterialIndex = drawcall.GBufferMaterialIndex;
			CreateAndBindGraphicsBindGroup(renderBackend.get(), pso, kGraphicsBindGroupSlot_Draw,
				{
					GraphicsBindGroupEntry::BufferSRV("StaticInstanceTransforms", instanceBuffer.get()),
					GraphicsBindGroupEntry::Constant(0, &objCB, sizeof(objCB)),
				});

			if (bUseBindlessGeometry)
			{
				renderBackend->DrawInstanced(drawcall.IndexCount, instanceCount, 0, 0);
			}
			else
			{
				renderBackend->DrawIndexedInstanced(
					drawcall.IndexCount,
					instanceCount,
					drawcall.IndexStart,
					drawcall.VertexBase,
					0);
			}
			++GBufferLastStaticInstancedDrawCount;
		}
	}

	++GBufferLastStaticInstancedBatchCount;
	GBufferLastStaticInstancedObjectCount += instanceCount;
	return true;
}

bool Corona::DrawStaticObjectBindlessBatch(const std::vector<uint32_t>& objectIndices)
{
	auto failPrerequisite = [](const wchar_t* reason) -> bool
	{
		const std::wstring reasonText = reason ? reason : L"unknown";
		static std::vector<std::wstring> loggedReasons;
		bool bAlreadyLogged = false;
		for (const std::wstring& loggedReason : loggedReasons)
		{
			if (loggedReason == reasonText)
			{
				bAlreadyLogged = true;
				break;
			}
		}
		if (!bAlreadyLogged)
		{
			AppendCpuRuntimeTrace(
				std::wstring(L"[GBufferObjectBatch] prerequisite failed: ") +
				reasonText);
			loggedReasons.push_back(reasonText);
		}
		return false;
	};

	if (objectIndices.empty())
		return false;
	if (!renderBackend)
		return failPrerequisite(L"missing render backend");
	if (!GBufferBindlessIndirectGraphicsPipeline)
		return failPrerequisite(L"missing bindless indirect pipeline");
	if (!samplerWrap)
		return failPrerequisite(L"missing sampler");
	if (!DefaultWhiteTex || !DefaultNormalTex || !DefaultRougnessTex || !DefaultBlackTex)
		return failPrerequisite(L"missing default material texture");

	if (!SupportsGBufferBindlessMaterials(renderBackend.get()) ||
		!SupportsGBufferBindlessGeometry(renderBackend.get()))
	{
		return failPrerequisite(L"backend lacks bindless material or geometry support");
	}
	const RenderBackendCapabilities backendCapabilities = renderBackend->GetCapabilities();
	if (!backendCapabilities.SupportsDrawIndirect ||
		!backendCapabilities.SupportsDrawIndirectFirstInstance)
	{
		return failPrerequisite(L"backend lacks draw indirect first-instance support");
	}
	if (backendCapabilities.RequiresStartupLoadingScreenGBufferFallback && bStartupLoadingScreenActive)
		return failPrerequisite(L"startup loading screen active");

	const bool profile = IsGBufferObjectBatchProfileEnabled();
	GBufferObjectBatchProfile& batchProfile = GetGBufferObjectBatchProfile();
	if (profile)
		batchProfile.BeginFrame(static_cast<uint64_t>(FrameCounter));
	GBufferProfileScope totalScope(profile, batchProfile.TotalMs, batchProfile.TotalCount);
	const auto buildStart = profile ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};

	const uint64_t defaultsHash = ComputeGBufferMaterialDefaultsHash(
		DefaultWhiteTex.get(),
		DefaultNormalTex.get(),
		DefaultRougnessTex.get(),
		DefaultBlackTex.get());
	CachedGBufferStaticDrawTable& staticDrawTable = GetCachedGBufferStaticDrawTable(this);

	auto buildStaticDrawTable = [&]() -> bool
	{
		const auto cacheBuildStart = GBufferProfileClock::now();
		AppendCpuRuntimeTrace(
			L"[GBufferObjectBatchCache] build begin objects=" +
			std::to_wstring(RenderWorld.SceneObjects.size()) +
			L", generation=" + std::to_wstring(RenderWorld.SceneObjectCullingIndexGeneration));
		staticDrawTable.Reset();
		staticDrawTable.Backend = renderBackend.get();
		staticDrawTable.Generation = RenderWorld.SceneObjectCullingIndexGeneration;
		staticDrawTable.DefaultsHash = defaultsHash;
		staticDrawTable.ObjectCount = static_cast<uint32_t>(RenderWorld.SceneObjects.size());
		staticDrawTable.ObjectRanges.assign(RenderWorld.SceneObjects.size(), {});

		std::vector<GBufferMaterialKey> materialKeys;
		std::vector<GBufferMaterialRecord> materialRecords;
		std::vector<GBufferGeometryKey> geometryKeys;
		std::vector<GBufferGeometryRecord> geometryRecords;
		std::vector<GBufferDrawRecord> drawRecords;
		materialKeys.reserve(128);
		materialRecords.reserve(128);
		geometryKeys.reserve(256);
		geometryRecords.reserve(256);
		drawRecords.reserve(RenderWorld.SceneObjects.size());
		staticDrawTable.Draws.reserve(RenderWorld.SceneObjects.size());

		auto findOrAddMaterial = [&](const GBufferMaterialKey& key, uint32_t& outIndex) -> bool
		{
			for (size_t i = 0; i < materialKeys.size(); ++i)
			{
				if (materialKeys[i] == key)
				{
					outIndex = static_cast<uint32_t>(i);
					return true;
				}
			}
			if (materialRecords.size() >= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
				return false;
			GBufferMaterialRecord record = MakeGBufferMaterialRecord(renderBackend.get(), key);
			if (!IsValidGBufferMaterialRecord(record))
				return false;
			materialKeys.push_back(key);
			materialRecords.push_back(record);
			outIndex = static_cast<uint32_t>(materialRecords.size() - 1);
			return true;
		};

		auto findOrAddGeometry = [&](const GBufferGeometryKey& key, uint32_t& outIndex) -> bool
		{
			for (size_t i = 0; i < geometryKeys.size(); ++i)
			{
				if (geometryKeys[i] == key)
				{
					outIndex = static_cast<uint32_t>(i);
					return true;
				}
			}
			if (geometryRecords.size() >= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
				return false;
			GBufferGeometryRecord record = MakeGBufferGeometryRecord(renderBackend.get(), key);
			if (!IsValidGBufferGeometryRecord(record))
				return false;
			geometryKeys.push_back(key);
			geometryRecords.push_back(record);
			outIndex = static_cast<uint32_t>(geometryRecords.size() - 1);
			return true;
		};

		for (uint32_t objectIndex = 0; objectIndex < static_cast<uint32_t>(RenderWorld.SceneObjects.size()); ++objectIndex)
		{
			const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
			CachedGBufferStaticObjectDrawRange& range = staticDrawTable.ObjectRanges[objectIndex];
			range.FirstDraw = static_cast<uint32_t>(staticDrawTable.Draws.size());
			if (!object.ScenePtr)
				continue;

			for (const std::shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
			{
				if (!mesh || !mesh->Vb || !mesh->Ib || !IsGBufferStaticBindlessGeometryEligible(*mesh))
					continue;

				uint32_t geometryIndex = 0;
				const GBufferGeometryKey geometryKey{
					mesh->Vb.get(),
					mesh->Ib.get(),
					mesh->VertexStride,
					GetGBufferIndexStride(mesh->IndexFormat)
				};
				if (!findOrAddGeometry(geometryKey, geometryIndex))
					return false;

				const glm::mat4x4 worldMatrix = glm::transpose(object.Transform * mesh->transform);
				for (const Mesh::DrawCall& drawcall : mesh->Draws)
				{
					if (drawcall.IndexCount == 0)
						continue;
					if (drawRecords.size() >= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
						return false;

					Material* material = drawcall.mat ? drawcall.mat.get() : mesh->Mat.get();
					const GBufferMaterialKey materialKey = ResolveGBufferMaterialKey(
						material,
						DefaultWhiteTex.get(),
						DefaultNormalTex.get(),
						DefaultRougnessTex.get(),
						DefaultBlackTex.get());
					uint32_t materialIndex = 0;
					if (!findOrAddMaterial(materialKey, materialIndex))
						return false;

					GBufferDrawRecord drawRecord{};
					drawRecord.WorldMatrixRow0 = worldMatrix[0];
					drawRecord.WorldMatrixRow1 = worldMatrix[1];
					drawRecord.WorldMatrixRow2 = worldMatrix[2];
					drawRecord.WorldMatrixRow3 = worldMatrix[3];
					drawRecord.BaseColorFactor = drawcall.mat ? drawcall.mat->BaseColorFactor : glm::vec4(1.0f);
					drawRecord.RougnessMetalic = glm::vec2(object.Roughness, object.Metallic);
					drawRecord.bOverrideRougnessMetallic = object.bOverrideRoughnessMetallic ? 1u : 0u;
					drawRecord.bTwoSidedLighting = mesh->bGrassMesh ? 1u : 0u;
					drawRecord.bUnlitMaterial = 0u;
					drawRecord.bGrassMesh = mesh->bGrassMesh ? 1u : 0u;
					drawRecord.bTerrainMesh = mesh->bTerrainMesh ? 1u : 0u;
					drawRecord.bExcludeFromDeformSphere = mesh->bExcludeFromDeformSphere ? 1u : 0u;
					drawRecord.GBufferMaterialIndex = materialIndex;
					drawRecord.GBufferGeometryIndex = geometryIndex;
					drawRecord.GBufferIndexStart = drawcall.IndexStart;
					drawRecord.GBufferVertexBase = drawcall.VertexBase;

					const bool bTransparentDraw = mesh->bTransparent;
					staticDrawTable.Draws.push_back({
						static_cast<uint32_t>(drawRecords.size()),
						drawcall.IndexCount,
						bTransparentDraw
					});
					if (bTransparentDraw)
						++range.AlphaDrawCount;
					else
						++range.OpaqueDrawCount;
					drawRecords.push_back(drawRecord);
				}
			}
			range.DrawCount = static_cast<uint32_t>(staticDrawTable.Draws.size()) - range.FirstDraw;
		}

		if (drawRecords.empty() || materialRecords.empty() || geometryRecords.empty())
		{
			staticDrawTable.Reset();
			return true;
		}

		std::vector<GBufferGpuCachedDrawInfo> gpuDrawInfos;
		gpuDrawInfos.reserve(staticDrawTable.Draws.size());
		for (const CachedGBufferStaticDrawInfo& drawInfo : staticDrawTable.Draws)
		{
			GBufferGpuCachedDrawInfo gpuDrawInfo{};
			gpuDrawInfo.DrawRecordIndex = drawInfo.DrawRecordIndex;
			gpuDrawInfo.IndexCount = drawInfo.IndexCount;
			gpuDrawInfo.Flags = drawInfo.bTransparent ? 1u : 0u;
			gpuDrawInfos.push_back(gpuDrawInfo);
		}

		auto createStaticStructuredBuffer = [&](uint32_t numElements, uint32_t elementSize, const void* initialData)
			-> std::shared_ptr<Buffer>
		{
			BufferCreateDesc desc = {};
			desc.NumElements = numElements;
			desc.ElementSize = elementSize;
			desc.InitialState = EInitialResourceState::ShaderRead;
			desc.InitialData = const_cast<void*>(initialData);
			desc.Shape = EBufferShape::Structured;
			desc.Access = EBufferAccess::GpuOnly;
			desc.AllocationPolicy = EBufferAllocationPolicy::Suballocated;
			return renderBackend->CreateBuffer(desc);
		};
		auto createGpuWriteStructuredBuffer = [&](uint32_t numElements, uint32_t elementSize)
			-> std::shared_ptr<Buffer>
		{
			BufferCreateDesc desc = {};
			desc.NumElements = std::max(1u, numElements);
			desc.ElementSize = elementSize;
			desc.InitialState = EInitialResourceState::ShaderRead;
			desc.bAllowUnorderedAccess = true;
			desc.Shape = EBufferShape::Structured;
			desc.Access = EBufferAccess::GpuOnly;
			desc.AllocationPolicy = EBufferAllocationPolicy::Dedicated;
			return renderBackend->CreateBuffer(desc);
		};

		staticDrawTable.MaterialBuffer = createStaticStructuredBuffer(
			static_cast<uint32_t>(materialRecords.size()),
			static_cast<uint32_t>(sizeof(GBufferMaterialRecord)),
			materialRecords.data());
		staticDrawTable.GeometryBuffer = createStaticStructuredBuffer(
			static_cast<uint32_t>(geometryRecords.size()),
			static_cast<uint32_t>(sizeof(GBufferGeometryRecord)),
			geometryRecords.data());
		staticDrawTable.DrawRecordBuffer = createStaticStructuredBuffer(
			static_cast<uint32_t>(drawRecords.size()),
			static_cast<uint32_t>(sizeof(GBufferDrawRecord)),
			drawRecords.data());
		staticDrawTable.GpuObjectRangeBuffer = createStaticStructuredBuffer(
			static_cast<uint32_t>(staticDrawTable.ObjectRanges.size()),
			static_cast<uint32_t>(sizeof(CachedGBufferStaticObjectDrawRange)),
			staticDrawTable.ObjectRanges.data());
		staticDrawTable.GpuDrawInfoBuffer = createStaticStructuredBuffer(
			static_cast<uint32_t>(gpuDrawInfos.size()),
			static_cast<uint32_t>(sizeof(GBufferGpuCachedDrawInfo)),
			gpuDrawInfos.data());
		staticDrawTable.GpuOpaqueIndirectArgsBuffer = createGpuWriteStructuredBuffer(
			static_cast<uint32_t>(drawRecords.size()),
			static_cast<uint32_t>(sizeof(DrawIndirectArguments)));
		staticDrawTable.GpuAlphaIndirectArgsBuffer = createGpuWriteStructuredBuffer(
			static_cast<uint32_t>(drawRecords.size()),
			static_cast<uint32_t>(sizeof(DrawIndirectArguments)));
		staticDrawTable.GpuIndirectCountBuffer = createGpuWriteStructuredBuffer(
			2u,
			static_cast<uint32_t>(sizeof(uint32_t)));
		staticDrawTable.GpuOpaqueIndirectArgsState = EResourceState::ShaderRead;
		staticDrawTable.GpuAlphaIndirectArgsState = EResourceState::ShaderRead;
		staticDrawTable.GpuIndirectCountState = EResourceState::ShaderRead;
		staticDrawTable.GpuCullBufferCreateFrame = FrameCounter;
		if (!staticDrawTable.MaterialBuffer || !staticDrawTable.GeometryBuffer || !staticDrawTable.DrawRecordBuffer)
		{
			staticDrawTable.Reset();
			return false;
		}
		if (!staticDrawTable.GpuObjectRangeBuffer ||
			!staticDrawTable.GpuDrawInfoBuffer ||
			!staticDrawTable.GpuOpaqueIndirectArgsBuffer ||
			!staticDrawTable.GpuAlphaIndirectArgsBuffer ||
			!staticDrawTable.GpuIndirectCountBuffer)
		{
			AppendCpuRuntimeTrace(L"[GBufferObjectBatchCache] GPU cull buffers unavailable; CPU indirect mode remains available");
			staticDrawTable.GpuObjectRangeBuffer.reset();
			staticDrawTable.GpuDrawInfoBuffer.reset();
			staticDrawTable.GpuOpaqueIndirectArgsBuffer.reset();
			staticDrawTable.GpuAlphaIndirectArgsBuffer.reset();
			staticDrawTable.GpuIndirectCountBuffer.reset();
		}

		staticDrawTable.MaterialCount = static_cast<uint32_t>(materialRecords.size());
		staticDrawTable.GeometryCount = static_cast<uint32_t>(geometryRecords.size());
		staticDrawTable.DrawRecordCount = static_cast<uint32_t>(drawRecords.size());
		AppendCpuRuntimeTrace(
			L"[GBufferObjectBatchCache] build complete ms=" +
			std::to_wstring(GBufferProfileElapsedMs(cacheBuildStart)) +
			L", materials=" + std::to_wstring(staticDrawTable.MaterialCount) +
			L", geometries=" + std::to_wstring(staticDrawTable.GeometryCount) +
			L", records=" + std::to_wstring(staticDrawTable.DrawRecordCount) +
			L", drawInfos=" + std::to_wstring(staticDrawTable.Draws.size()));
		return true;
	};

	const bool bStaticDrawTableValid =
		staticDrawTable.Backend == renderBackend.get() &&
		staticDrawTable.Generation == RenderWorld.SceneObjectCullingIndexGeneration &&
		staticDrawTable.DefaultsHash == defaultsHash &&
		staticDrawTable.ObjectCount == static_cast<uint32_t>(RenderWorld.SceneObjects.size()) &&
		staticDrawTable.MaterialBuffer &&
		staticDrawTable.GeometryBuffer &&
		staticDrawTable.DrawRecordBuffer;
	if (!bStaticDrawTableValid && !buildStaticDrawTable())
		return failPrerequisite(L"static draw table build failed");

	if (!staticDrawTable.MaterialBuffer || !staticDrawTable.GeometryBuffer || !staticDrawTable.DrawRecordBuffer)
		return true;

	GBufferConstantBuffer objCB = {};
	objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
	objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);
	objCB.WorldMatrix = glm::mat4x4(1.0f);
	objCB.UnjitteredViewProjMat = glm::transpose(UnjitteredViewProjMat);
	objCB.PrevUnjitteredViewProjMat = glm::transpose(PrevUnjitteredViewProjMat);
	objCB.ViewDir.x = RenderFrameCameraLookDirection.x;
	objCB.ViewDir.y = RenderFrameCameraLookDirection.y;
	objCB.ViewDir.z = RenderFrameCameraLookDirection.z;
	objCB.ViewDir.w = 0.0f;
	objCB.BaseColorFactor = glm::vec4(1.0f);
	objCB.RTSize.x = GetRenderWidth();
	objCB.RTSize.y = GetRenderHeight();
	objCB.RougnessMetalic = glm::vec2(1.0f, 0.0f);
	objCB.bOverrideRougnessMetallic = 0u;
	objCB.MeshDeformParams = glm::vec4(RenderFrameWindTime, 0.0f, 0.0f, 0.0f);
	objCB.GrassBendOrigin = RenderFrameGrassBendOrigin;
	objCB.GrassBendParams = RenderFrameGrassBendParams;
	objCB.WindParams = RenderFrameWindParams;
	objCB.WindTuning = RenderFrameWindTuning;
	objCB.TerrainDeformSphere = RenderFrameTerrainDeformSphere;
	objCB.bGBufferBindlessGeometry = 1u;

	static bool bLoggedFirstStaticObjectCpuBatch = false;
	static bool bLoggedFirstStaticObjectGpuBatch = false;

	if (objectIndices.front() >= RenderWorld.SceneObjects.size() ||
		!RenderWorld.SceneObjects[objectIndices.front()].ScenePtr)
	{
		return failPrerequisite(L"missing first object scene");
	}
	auto* gbufferScene = RenderWorld.SceneObjects[objectIndices.front()].ScenePtr.get();

	auto bindGBufferBatchResources = [&](GraphicsPipelineHandle* batchPso) -> bool
	{
		if (!batchPso)
			return true;
		const auto bindStart = profile ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
		renderBackend->BindGraphicsPipeline(batchPso);
		if (!BindGBufferSceneResourceBindGroup(
			renderBackend.get(),
			batchPso,
			gbufferScene,
			samplerWrap.get(),
			staticDrawTable.MaterialBuffer.get(),
			staticDrawTable.GeometryBuffer.get(),
			staticDrawTable.DrawRecordBuffer.get(),
			true))
		{
			return failPrerequisite(L"resource bind group creation failed");
		}
		std::vector<GraphicsBindGroupEntry> drawBindEntries;
		drawBindEntries.reserve(1);
		drawBindEntries.push_back(GraphicsBindGroupEntry::Constant(0, &objCB, sizeof(objCB)));
		if (!CreateAndBindGraphicsBindGroup(renderBackend.get(), batchPso, kGraphicsBindGroupSlot_Draw, drawBindEntries))
			return failPrerequisite(L"draw bind group creation failed");
		GBufferProfileAdd(profile, batchProfile.BindMs, batchProfile.BindCount, bindStart);
		return true;
	};

	auto submitGBufferBatch = [&](GraphicsPipelineHandle* batchPso, Buffer* indirectArgsBuffer, uint64_t byteOffset, uint32_t drawCount) -> bool
	{
		if (!batchPso || drawCount == 0)
			return true;
		if (!bindGBufferBatchResources(batchPso))
			return false;
		const auto drawStart = profile ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
		const bool drawOk = renderBackend->DrawIndirect(indirectArgsBuffer, byteOffset, drawCount);
		GBufferProfileAdd(profile, batchProfile.DrawMs, batchProfile.DrawCount, drawStart);
		return drawOk;
	};

	auto submitGBufferBatchCount = [&](GraphicsPipelineHandle* batchPso, Buffer* indirectArgsBuffer, Buffer* countBuffer, uint64_t countByteOffset, uint32_t maxDrawCount) -> bool
	{
		if (!batchPso || maxDrawCount == 0)
			return true;
		if (!bindGBufferBatchResources(batchPso))
			return false;
		const auto drawStart = profile ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
		const bool drawOk = renderBackend->DrawIndirectCount(indirectArgsBuffer, 0, countBuffer, countByteOffset, maxDrawCount);
		GBufferProfileAdd(profile, batchProfile.DrawMs, batchProfile.DrawCount, drawStart);
		return drawOk;
	};

	auto transitionTrackedBuffer = [&](Buffer* buffer, EResourceState& currentState, EResourceState nextState)
	{
		if (!buffer || currentState == nextState)
			return;
		renderBackend->TransitionBuffer(buffer, currentState, nextState);
		currentState = nextState;
	};

	auto trySubmitGpuGeneratedIndirect = [&](bool& outGpuWorkStarted) -> bool
	{
		outGpuWorkStarted = false;
		if (GBufferObjectCullingMode != EGBufferObjectCullingMode::GpuIndirect)
			return false;
		const RenderBackendCapabilities gpuCapabilities = renderBackend->GetCapabilities();
		if (!gpuCapabilities.SupportsDrawIndirectCount ||
			!GBufferGpuCullClearPSO ||
			!GBufferGpuCullBuildPSO ||
			!staticDrawTable.GpuObjectRangeBuffer ||
			!staticDrawTable.GpuDrawInfoBuffer ||
			!staticDrawTable.GpuOpaqueIndirectArgsBuffer ||
			!staticDrawTable.GpuAlphaIndirectArgsBuffer ||
			!staticDrawTable.GpuIndirectCountBuffer)
		{
			return false;
		}
		if (staticDrawTable.GpuCullBufferCreateFrame == FrameCounter)
			return false;

		uint32_t candidateOpaqueMaxDraws = 0;
		uint32_t candidateAlphaMaxDraws = 0;
		for (uint32_t objectIndex : objectIndices)
		{
			if (objectIndex >= staticDrawTable.ObjectRanges.size())
				return false;
			const CachedGBufferStaticObjectDrawRange& range = staticDrawTable.ObjectRanges[objectIndex];
			if (range.OpaqueDrawCount > std::numeric_limits<uint32_t>::max() - candidateOpaqueMaxDraws ||
				range.AlphaDrawCount > std::numeric_limits<uint32_t>::max() - candidateAlphaMaxDraws)
			{
				return false;
			}
			candidateOpaqueMaxDraws += range.OpaqueDrawCount;
			candidateAlphaMaxDraws += range.AlphaDrawCount;
		}
		if (candidateAlphaMaxDraws > std::numeric_limits<uint32_t>::max() - candidateOpaqueMaxDraws)
			return false;
		const uint32_t candidateMaxDraws = candidateOpaqueMaxDraws + candidateAlphaMaxDraws;
		if (candidateMaxDraws == 0)
			return true;

		const auto allocStart = profile ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
		std::shared_ptr<Buffer> candidateObjectBuffer = renderBackend->AllocateTransientUploadStructuredBuffer(
			static_cast<uint32_t>(objectIndices.size()),
			static_cast<uint32_t>(sizeof(uint32_t)),
			objectIndices.data());
		GBufferProfileAdd(profile, batchProfile.IndirectAllocMs, batchProfile.IndirectAllocCount, allocStart);
		GBufferProfileAdd(profile, batchProfile.AllocMs, batchProfile.AllocCount, allocStart);
		if (!candidateObjectBuffer)
			return false;

		transitionTrackedBuffer(
			staticDrawTable.GpuOpaqueIndirectArgsBuffer.get(),
			staticDrawTable.GpuOpaqueIndirectArgsState,
			EResourceState::UnorderedAccess);
		transitionTrackedBuffer(
			staticDrawTable.GpuAlphaIndirectArgsBuffer.get(),
			staticDrawTable.GpuAlphaIndirectArgsState,
			EResourceState::UnorderedAccess);
		transitionTrackedBuffer(
			staticDrawTable.GpuIndirectCountBuffer.get(),
			staticDrawTable.GpuIndirectCountState,
			EResourceState::UnorderedAccess);

		outGpuWorkStarted = true;
		GBufferGpuCullClearPSO->SetBufferUAV("GBufferCullCounters", staticDrawTable.GpuIndirectCountBuffer.get());
		GBufferGpuCullClearPSO->Apply();
		renderBackend->Dispatch(1, 1, 1);
		renderBackend->UAVBarrier(staticDrawTable.GpuIndirectCountBuffer.get());

		GBufferGpuCullConstant cullCB{};
		cullCB.CandidateCount = static_cast<uint32_t>(objectIndices.size());
		cullCB.ObjectRangeCount = static_cast<uint32_t>(staticDrawTable.ObjectRanges.size());
		cullCB.DrawInfoCount = static_cast<uint32_t>(staticDrawTable.Draws.size());
		cullCB.MaxOutputDraws = std::max(candidateOpaqueMaxDraws, candidateAlphaMaxDraws);
		GBufferGpuCullBuildPSO->SetBufferSRV("GBufferObjectRanges", staticDrawTable.GpuObjectRangeBuffer.get());
		GBufferGpuCullBuildPSO->SetBufferSRV("GBufferCachedDraws", staticDrawTable.GpuDrawInfoBuffer.get());
		GBufferGpuCullBuildPSO->SetBufferSRV("GBufferCandidateObjectIndices", candidateObjectBuffer.get());
		GBufferGpuCullBuildPSO->SetBufferUAV("GBufferOpaqueArgs", staticDrawTable.GpuOpaqueIndirectArgsBuffer.get());
		GBufferGpuCullBuildPSO->SetBufferUAV("GBufferAlphaArgs", staticDrawTable.GpuAlphaIndirectArgsBuffer.get());
		GBufferGpuCullBuildPSO->SetBufferUAV("GBufferCullCounters", staticDrawTable.GpuIndirectCountBuffer.get());
		GBufferGpuCullBuildPSO->SetCBVValue("GBufferCullCB", &cullCB);
		GBufferGpuCullBuildPSO->Apply();
		const uint32_t dispatchGroups = (cullCB.CandidateCount + 63u) / 64u;
		renderBackend->Dispatch(std::max(1u, dispatchGroups), 1, 1);
		renderBackend->UAVBarrier(staticDrawTable.GpuOpaqueIndirectArgsBuffer.get());
		renderBackend->UAVBarrier(staticDrawTable.GpuAlphaIndirectArgsBuffer.get());
		renderBackend->UAVBarrier(staticDrawTable.GpuIndirectCountBuffer.get());

		transitionTrackedBuffer(
			staticDrawTable.GpuOpaqueIndirectArgsBuffer.get(),
			staticDrawTable.GpuOpaqueIndirectArgsState,
			EResourceState::IndirectArgument);
		transitionTrackedBuffer(
			staticDrawTable.GpuAlphaIndirectArgsBuffer.get(),
			staticDrawTable.GpuAlphaIndirectArgsState,
			EResourceState::IndirectArgument);
		transitionTrackedBuffer(
			staticDrawTable.GpuIndirectCountBuffer.get(),
			staticDrawTable.GpuIndirectCountState,
			EResourceState::IndirectArgument);
		GBufferProfileAdd(profile, batchProfile.BuildMs, batchProfile.BuildCount, buildStart);

		GraphicsPipelineHandle* opaquePso = GBufferBindlessIndirectOpaqueGraphicsPipeline.get();
		GraphicsPipelineHandle* opaqueDrawPso = opaquePso ? opaquePso : GBufferBindlessIndirectGraphicsPipeline.get();
		const bool bOpaqueOk = submitGBufferBatchCount(
			opaqueDrawPso,
			staticDrawTable.GpuOpaqueIndirectArgsBuffer.get(),
			staticDrawTable.GpuIndirectCountBuffer.get(),
			0,
			candidateOpaqueMaxDraws);
		const bool bAlphaOk = submitGBufferBatchCount(
			GBufferBindlessIndirectGraphicsPipeline.get(),
			staticDrawTable.GpuAlphaIndirectArgsBuffer.get(),
			staticDrawTable.GpuIndirectCountBuffer.get(),
			sizeof(uint32_t),
			candidateAlphaMaxDraws);
		if (!bOpaqueOk || !bAlphaOk)
			return false;

		++GBufferLastBindlessObjectBatchCount;
		GBufferLastBindlessObjectCount += static_cast<uint64_t>(objectIndices.size());
		GBufferLastBindlessObjectDrawCount += static_cast<uint64_t>(candidateMaxDraws);
		if (profile)
		{
			++batchProfile.Calls;
			batchProfile.Objects += static_cast<uint64_t>(objectIndices.size());
			batchProfile.DrawRecords += static_cast<uint64_t>(candidateMaxDraws);
			batchProfile.Materials += static_cast<uint64_t>(staticDrawTable.MaterialCount);
			batchProfile.Geometries += static_cast<uint64_t>(staticDrawTable.GeometryCount);
			batchProfile.IndirectArgs += static_cast<uint64_t>(candidateMaxDraws);
		}
		if (!bLoggedFirstStaticObjectGpuBatch || (FrameCounter % 120u) == 0u)
		{
			AppendCpuRuntimeTrace(
				L"[GBufferObjectBatch] submitted mode=gpu objects=" + std::to_wstring(objectIndices.size()) +
				L", cachedRecords=" + std::to_wstring(staticDrawTable.DrawRecordCount) +
				L", candidateDraws=" + std::to_wstring(candidateMaxDraws) +
				L", opaqueMax=" + std::to_wstring(candidateOpaqueMaxDraws) +
				L", alphaMax=" + std::to_wstring(candidateAlphaMaxDraws) +
				L", materials=" + std::to_wstring(staticDrawTable.MaterialCount) +
				L", geometries=" + std::to_wstring(staticDrawTable.GeometryCount));
			bLoggedFirstStaticObjectGpuBatch = true;
		}
		return true;
	};

	bool bGpuWorkStarted = false;
	if (trySubmitGpuGeneratedIndirect(bGpuWorkStarted))
		return true;
	if (bGpuWorkStarted)
		return failPrerequisite(L"GPU generated indirect draw submission failed");

	static thread_local std::vector<DrawIndirectArguments> s_indirectArgs;
	std::vector<DrawIndirectArguments>& indirectArgs = s_indirectArgs;
	indirectArgs.clear();
	// Opaque/alpha-test split (early-Z): opaque draws batch here and run with
	// PSMainOpaque ([earlydepthstencil]); alpha-tested draws stay on the
	// discard PSO. Concatenated into indirectArgs as [opaque | alpha] below.
	static thread_local std::vector<DrawIndirectArguments> s_opaqueIndirectArgs;
	static thread_local std::vector<DrawIndirectArguments> s_alphaIndirectArgs;
	std::vector<DrawIndirectArguments>& opaqueIndirectArgs = s_opaqueIndirectArgs;
	std::vector<DrawIndirectArguments>& alphaIndirectArgs = s_alphaIndirectArgs;
	opaqueIndirectArgs.clear();
	alphaIndirectArgs.clear();
	indirectArgs.reserve(objectIndices.size() * 2u);
	opaqueIndirectArgs.reserve(objectIndices.size() * 2u);
	alphaIndirectArgs.reserve(objectIndices.size() * 2u);

	for (uint32_t objectIndex : objectIndices)
	{
		if (objectIndex >= RenderWorld.SceneObjects.size())
			return failPrerequisite(L"object index out of range");
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		if (!object.ScenePtr)
			return failPrerequisite(L"missing object scene");
		if (objectIndex >= staticDrawTable.ObjectRanges.size())
			return failPrerequisite(L"object draw range missing");
		const CachedGBufferStaticObjectDrawRange& range = staticDrawTable.ObjectRanges[objectIndex];
		for (uint32_t drawOffset = 0; drawOffset < range.DrawCount; ++drawOffset)
		{
			const uint32_t cachedDrawIndex = range.FirstDraw + drawOffset;
			if (cachedDrawIndex >= staticDrawTable.Draws.size())
				return failPrerequisite(L"cached draw index overflow");
			const CachedGBufferStaticDrawInfo& drawInfo = staticDrawTable.Draws[cachedDrawIndex];
			DrawIndirectArguments arg{};
			std::wstring failureReason;
			if (!BuildGBufferIndirectDrawArguments(
				drawInfo.DrawRecordIndex,
				drawInfo.IndexCount,
				arg,
				&failureReason))
			{
				return failPrerequisite(failureReason.c_str());
			}
			// Route by opacity: opaque -> early-Z PSO batch, alpha-tested ->
			// discard PSO batch. mesh->bTransparent matches the RT BVH opaque
			// classification; conservative (any-alpha mesh -> discard path).
			if (drawInfo.bTransparent)
				alphaIndirectArgs.push_back(arg);
			else
				opaqueIndirectArgs.push_back(arg);
		}
	}

	// Concatenate as [opaque | alpha]; opaque count is the DrawIndirect split.
	const uint32_t gbufferOpaqueDrawCount = static_cast<uint32_t>(opaqueIndirectArgs.size());
	indirectArgs.clear();
	indirectArgs.reserve(opaqueIndirectArgs.size() + alphaIndirectArgs.size());
	indirectArgs.insert(indirectArgs.end(), opaqueIndirectArgs.begin(), opaqueIndirectArgs.end());
	indirectArgs.insert(indirectArgs.end(), alphaIndirectArgs.begin(), alphaIndirectArgs.end());
	GBufferProfileAdd(profile, batchProfile.BuildMs, batchProfile.BuildCount, buildStart);

	if (indirectArgs.empty())
		return true;

	// Static records are persistent device-local buffers. Per frame we only
	// stream the visible indirect argument list.
	const auto allocStart = profile ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	const auto indirectAllocStart = profile ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	std::shared_ptr<Buffer> indirectArgsBuffer = renderBackend->AllocateTransientUploadStructuredBuffer(
		static_cast<uint32_t>(indirectArgs.size()),
		static_cast<uint32_t>(sizeof(DrawIndirectArguments)),
		indirectArgs.data());
	GBufferProfileAdd(profile, batchProfile.IndirectAllocMs, batchProfile.IndirectAllocCount, indirectAllocStart);
	GBufferProfileAdd(profile, batchProfile.AllocMs, batchProfile.AllocCount, allocStart);
	if (!indirectArgsBuffer)
		return failPrerequisite(L"transient upload allocation failed");

	const uint32_t gbufferTotalDrawCount = static_cast<uint32_t>(indirectArgs.size());
	const uint32_t gbufferAlphaDrawCount = gbufferTotalDrawCount - gbufferOpaqueDrawCount;
	const uint64_t gbufferArgStride = static_cast<uint64_t>(sizeof(DrawIndirectArguments));

	bool bDrawSubmitted = false;
	GraphicsPipelineHandle* opaquePso = GBufferBindlessIndirectOpaqueGraphicsPipeline.get();
	if (opaquePso)
	{
		// Opaque batch first (fills depth so early-Z rejects occluded opaque
		// pixels), then the alpha-tested batch on the discard PSO.
		const bool bOpaqueOk = submitGBufferBatch(opaquePso, indirectArgsBuffer.get(), 0, gbufferOpaqueDrawCount);
		const bool bAlphaOk = submitGBufferBatch(
			GBufferBindlessIndirectGraphicsPipeline.get(),
			indirectArgsBuffer.get(),
			static_cast<uint64_t>(gbufferOpaqueDrawCount) * gbufferArgStride,
			gbufferAlphaDrawCount);
		bDrawSubmitted = bOpaqueOk && bAlphaOk;
	}
	else
	{
		// Opaque early-Z PSO unavailable: original single discard pass.
		bDrawSubmitted = submitGBufferBatch(
			GBufferBindlessIndirectGraphicsPipeline.get(), indirectArgsBuffer.get(), 0, gbufferTotalDrawCount);
	}
	if (!bDrawSubmitted)
		return failPrerequisite(L"draw submission failed");

	++GBufferLastBindlessObjectBatchCount;
	GBufferLastBindlessObjectCount += static_cast<uint64_t>(objectIndices.size());
	GBufferLastBindlessObjectDrawCount += static_cast<uint64_t>(indirectArgs.size());
	if (profile)
	{
		++batchProfile.Calls;
		batchProfile.Objects += static_cast<uint64_t>(objectIndices.size());
		batchProfile.DrawRecords += static_cast<uint64_t>(indirectArgs.size());
		batchProfile.Materials += static_cast<uint64_t>(staticDrawTable.MaterialCount);
		batchProfile.Geometries += static_cast<uint64_t>(staticDrawTable.GeometryCount);
		batchProfile.IndirectArgs += static_cast<uint64_t>(indirectArgs.size());
	}

	if (!bLoggedFirstStaticObjectCpuBatch || (FrameCounter % 120u) == 0u)
	{
		AppendCpuRuntimeTrace(
			L"[GBufferObjectBatch] submitted mode=cpu objects=" + std::to_wstring(objectIndices.size()) +
			L", visibleRecords=" + std::to_wstring(indirectArgs.size()) +
			L", cachedRecords=" + std::to_wstring(staticDrawTable.DrawRecordCount) +
			L", indirectDraws=" + std::to_wstring(indirectArgs.size()) +
			L", materials=" + std::to_wstring(staticDrawTable.MaterialCount) +
			L", geometries=" + std::to_wstring(staticDrawTable.GeometryCount));
		bLoggedFirstStaticObjectCpuBatch = true;
	}
	return true;
}

void Corona::ResetGBufferStaticDrawCache()
{
	ResetCachedGBufferStaticDrawTable(this);
}

void Corona::DrawScene(shared_ptr<Scene> scene, const glm::mat4x4& instanceTransform, float Roughness, float Metalic, bool bOverrideRoughnessMetallic)
{
	if (!scene)
		return;

	GBufferMaterialTable materialTable = BuildGBufferMaterialTable(
		renderBackend.get(),
		scene.get(),
		DefaultWhiteTex.get(),
		DefaultNormalTex.get(),
		DefaultRougnessTex.get(),
		DefaultBlackTex.get());
	GBufferGeometryTable geometryTable = BuildGBufferGeometryTable(renderBackend.get(), scene.get());
	bool bUseSceneBindlessGeometry =
		GBufferBindlessGeometryGraphicsPipeline &&
		geometryTable.Buffer;
	const RenderBackendCapabilities backendCapabilities =
		renderBackend ? renderBackend->GetCapabilities() : RenderBackendCapabilities{};
	const bool bBackendSupportsBindlessIndirect =
		renderBackend &&
		backendCapabilities.SupportsDrawIndirect &&
		backendCapabilities.SupportsDrawIndirectFirstInstance;
	const bool bSceneStaticBindlessIndirectCandidate =
		bUseSceneBindlessGeometry &&
		IsGBufferSceneStaticBindlessGeometryEligible(scene.get());
	const bool bSceneRequiresBindlessIndirect =
		bSceneStaticBindlessIndirectCandidate;

	auto tryDrawSceneBindlessIndirect = [&]() -> bool
	{
		auto failPrerequisite = [](const wchar_t* reason) -> bool
		{
			const std::wstring reasonText = reason ? reason : L"unknown";
			static std::vector<std::wstring> loggedReasons;
			bool bAlreadyLogged = false;
			for (const std::wstring& loggedReason : loggedReasons)
			{
				if (loggedReason == reasonText)
				{
					bAlreadyLogged = true;
					break;
				}
			}
			if (!bAlreadyLogged)
			{
				AppendCpuRuntimeTrace(
					std::wstring(L"[GBufferIndirect] prerequisite failed: ") +
					reasonText);
				loggedReasons.push_back(reasonText);
			}
			return false;
		};

		if (!bSceneStaticBindlessIndirectCandidate)
			return false;
		if (!bBackendSupportsBindlessIndirect)
			return failPrerequisite(L"backend lacks draw indirect first-instance support");
		if (!GBufferBindlessIndirectGraphicsPipeline)
			return failPrerequisite(L"missing bindless indirect pipeline");
		if (!materialTable.Buffer)
			return failPrerequisite(L"missing material table");
		if (!geometryTable.Buffer)
			return failPrerequisite(L"missing geometry table");
		if (!bUseSceneBindlessGeometry)
			return failPrerequisite(L"missing bindless geometry pipeline");
		if (!samplerWrap)
			return failPrerequisite(L"missing sampler");
		if (backendCapabilities.RequiresStartupLoadingScreenGBufferFallback && bStartupLoadingScreenActive)
			return failPrerequisite(L"startup loading screen active");

		uint32_t drawCount = 0;
		for (const std::shared_ptr<Mesh>& mesh : scene->meshes)
		{
			if (!mesh)
				continue;
			if (!mesh->Vb || !mesh->Ib)
				return failPrerequisite(L"mesh missing geometry buffers");
			if (!IsGBufferStaticBindlessGeometryEligible(*mesh))
			{
				return failPrerequisite(L"mesh is not static bindless eligible");
			}
			for (const Mesh::DrawCall& drawcall : mesh->Draws)
			{
				if (drawcall.IndexCount == 0)
					continue;
				if (drawCount == std::numeric_limits<uint32_t>::max())
					return failPrerequisite(L"draw count overflow");
				++drawCount;
			}
		}

		if (drawCount == 0)
			return true;

		std::vector<GBufferDrawRecord> drawRecords;
		std::vector<DrawIndirectArguments> indirectArgs;
		drawRecords.reserve(drawCount);
		indirectArgs.reserve(drawCount);

		for (const std::shared_ptr<Mesh>& mesh : scene->meshes)
		{
			if (!mesh)
				continue;

			const glm::mat4x4 worldMatrix = glm::transpose(instanceTransform * mesh->transform);
			for (const Mesh::DrawCall& drawcall : mesh->Draws)
			{
				if (drawcall.IndexCount == 0)
					continue;

				GBufferDrawRecord drawRecord{};
				drawRecord.WorldMatrixRow0 = worldMatrix[0];
				drawRecord.WorldMatrixRow1 = worldMatrix[1];
				drawRecord.WorldMatrixRow2 = worldMatrix[2];
				drawRecord.WorldMatrixRow3 = worldMatrix[3];
				drawRecord.BaseColorFactor = drawcall.mat ? drawcall.mat->BaseColorFactor : glm::vec4(1.0f);
				drawRecord.RougnessMetalic = glm::vec2(Roughness, Metalic);
				drawRecord.bOverrideRougnessMetallic = bOverrideRoughnessMetallic ? 1u : 0u;
				drawRecord.bTwoSidedLighting = mesh->bGrassMesh ? 1u : 0u;
				drawRecord.bUnlitMaterial = 0u;
				drawRecord.bGrassMesh = mesh->bGrassMesh ? 1u : 0u;
				drawRecord.bTerrainMesh = mesh->bTerrainMesh ? 1u : 0u;
				drawRecord.bExcludeFromDeformSphere = mesh->bExcludeFromDeformSphere ? 1u : 0u;
				drawRecord.GBufferMaterialIndex = drawcall.GBufferMaterialIndex;
				drawRecord.GBufferGeometryIndex = mesh->GBufferGeometryIndex;
				drawRecord.GBufferIndexStart = drawcall.IndexStart;
				drawRecord.GBufferVertexBase = drawcall.VertexBase;

				DrawIndirectArguments arg{};
				std::wstring failureReason;
				if (!BuildGBufferIndirectDrawArguments(
					static_cast<uint32_t>(drawRecords.size()),
					drawcall.IndexCount,
					arg,
					&failureReason))
				{
					return failPrerequisite(failureReason.c_str());
				}

				drawRecords.push_back(drawRecord);
				indirectArgs.push_back(arg);
			}
		}

		std::shared_ptr<Buffer> drawRecordBuffer = renderBackend->AllocateTransientUploadStructuredBuffer(
			static_cast<uint32_t>(drawRecords.size()),
			static_cast<uint32_t>(sizeof(GBufferDrawRecord)),
			drawRecords.data());
		std::shared_ptr<Buffer> indirectArgsBuffer = renderBackend->AllocateTransientUploadStructuredBuffer(
			static_cast<uint32_t>(indirectArgs.size()),
			static_cast<uint32_t>(sizeof(DrawIndirectArguments)),
			indirectArgs.data());
		if (!drawRecordBuffer || !indirectArgsBuffer)
			return failPrerequisite(L"transient upload allocation failed");

		GraphicsPipelineHandle* pso = GBufferBindlessIndirectGraphicsPipeline.get();
		renderBackend->BindGraphicsPipeline(pso);
		if (!BindGBufferSceneResourceBindGroup(
			renderBackend.get(),
			pso,
			scene.get(),
			samplerWrap.get(),
			materialTable.Buffer.get(),
			geometryTable.Buffer.get(),
			drawRecordBuffer.get(),
			false))
		{
			return failPrerequisite(L"resource bind group creation failed");
		}

		GBufferConstantBuffer objCB = {};
		objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
		objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);
		objCB.WorldMatrix = glm::mat4x4(1.0f);
		objCB.UnjitteredViewProjMat = glm::transpose(UnjitteredViewProjMat);
		objCB.PrevUnjitteredViewProjMat = glm::transpose(PrevUnjitteredViewProjMat);
		objCB.ViewDir.x = RenderFrameCameraLookDirection.x;
		objCB.ViewDir.y = RenderFrameCameraLookDirection.y;
		objCB.ViewDir.z = RenderFrameCameraLookDirection.z;
		objCB.ViewDir.w = 0.0f;
		objCB.BaseColorFactor = glm::vec4(1.0f);
		objCB.RTSize.x = GetRenderWidth();
		objCB.RTSize.y = GetRenderHeight();
		objCB.RougnessMetalic.x = Roughness;
		objCB.RougnessMetalic.y = Metalic;
		objCB.bOverrideRougnessMetallic = bOverrideRoughnessMetallic ? 1u : 0u;
		objCB.MeshDeformParams = glm::vec4(RenderFrameWindTime, 0.0f, 0.0f, 0.0f);
		objCB.GrassBendOrigin = RenderFrameGrassBendOrigin;
		objCB.GrassBendParams = RenderFrameGrassBendParams;
		objCB.WindParams = RenderFrameWindParams;
		objCB.WindTuning = RenderFrameWindTuning;
		objCB.TerrainDeformSphere = RenderFrameTerrainDeformSphere;
		objCB.bGBufferBindlessGeometry = 1u;

		std::vector<GraphicsBindGroupEntry> drawBindEntries;
		drawBindEntries.reserve(1);
		drawBindEntries.push_back(GraphicsBindGroupEntry::Constant(0, &objCB, sizeof(objCB)));
		if (!CreateAndBindGraphicsBindGroup(renderBackend.get(), pso, kGraphicsBindGroupSlot_Draw, drawBindEntries))
			return failPrerequisite(L"draw bind group creation failed");

		const bool bDrawSubmitted = renderBackend->DrawIndirect(
			indirectArgsBuffer.get(),
			0,
			static_cast<uint32_t>(indirectArgs.size()));
		if (bDrawSubmitted)
		{
			static bool bLoggedBindlessIndirect = false;
			if (!bLoggedBindlessIndirect)
			{
				AppendCpuRuntimeTrace(L"[GBufferIndirect] submitted bindless indirect drawCount=" + std::to_wstring(indirectArgs.size()));
				bLoggedBindlessIndirect = true;
			}
		}
		else
		{
			return failPrerequisite(L"draw submission failed");
		}
		return bDrawSubmitted;
	};

	if (tryDrawSceneBindlessIndirect())
		return;
	if (bSceneRequiresBindlessIndirect)
	{
		static bool bLoggedBindlessIndirectRequiredFailure = false;
		if (!bLoggedBindlessIndirectRequiredFailure)
		{
			AppendCpuRuntimeTrace(L"[GBufferIndirect] required static bindless indirect draw failed; no fallback draw submitted");
			bLoggedBindlessIndirectRequiredFailure = true;
		}
		return;
	}

	for (auto& mesh : scene->meshes)
	{
		if (!mesh)
			continue;

		// Spine VS-inline path: skinning math runs in the VS, reading
		// directly from the input SBVs each frame instead of a
		// pre-skinned vertex buffer. No compute pre-pass needed, so we
		// don't gate on bGpuSpineSkinningDispatched. Works on both
		// desktop and mobile (mobile keeps compute disabled separately).
		const bool bUseSpineVsInline =
			bSpineUseVsInlineSkinning &&
			mesh->bGpuSpineSkinned &&
			mesh->GpuSpineInputVertices &&
			mesh->GpuSpineInfluences &&
			mesh->GpuSpineBones &&
			SpineVsInlineGBufferGraphicsPipeline;
		const bool bUseSpineVertexFetch =
			!bUseSpineVsInline &&
			mesh->bGpuSpineSkinned &&
			mesh->bGpuSpineSkinningDispatched &&
			mesh->GpuSpineSkinnedVertices &&
			SpineGBufferGraphicsPipeline;
		const bool bUseCpuSpinePipeline =
			!bUseSpineVsInline &&
			!bUseSpineVertexFetch &&
			mesh->bSpineMesh &&
			CpuSpineGBufferGraphicsPipeline;
		// Phase 11 / Phase A / Path C skeletal paths. Each picks a different
		// PSO + IA source VB but all share the same SkeletalInputs +
		// SkeletalPrevBones bindings.
		//   Path C (VS inline): IA reads bind-pose, VS does skinning inline,
		//                        binds SkeletalCurrBones too.
		//   CPU "Spine-style":  IA reads SkeletalUnifiedCpuSkinnedVb (CPU
		//                        skinned this frame), VS reads PrevBones for
		//                        motion vectors only.
		//   GPU compute:        IA reads the compute-output VB, same VS as CPU.
		const bool bSkeletalReady =
			mesh->bSkeletalSkinned &&
			mesh->bSkeletalSkinningDispatched &&
			mesh->SkeletalInputVertices &&
			mesh->SkeletalPrevBoneMatrices;
		const bool bUseSkeletalVsInline =
			bSkeletalReady &&
			bSkeletalUseVsInlineSkinning &&
			SkeletalUnifiedBoneMatrices &&
			SkeletalUnifiedBindVb &&
			SkeletalVsInlineGraphicsPipeline;
		const bool bUseSkeletalSkinned =
			!bUseSkeletalVsInline &&
			bSkeletalReady &&
			mesh->SkeletalOutputVb &&
			SkeletalGBufferGraphicsPipeline;
		const bool bUseProceduralGrass = mesh->bProceduralGrass && ProceduralGrassGraphicsPipeline;
		const bool bUseStaticBindlessGeometry =
			bUseSceneBindlessGeometry &&
			!bUseProceduralGrass &&
			!bUseSkeletalVsInline &&
			!bUseSkeletalSkinned &&
			!bUseSpineVsInline &&
			!bUseSpineVertexFetch &&
			!bUseCpuSpinePipeline &&
			mesh->Vb &&
			mesh->Ib &&
			IsGBufferStaticBindlessGeometryEligible(*mesh);
		GraphicsPipelineHandle* activeGBufferPipeline =
			bUseProceduralGrass ? ProceduralGrassGraphicsPipeline.get() :
			(bUseSkeletalVsInline ? SkeletalVsInlineGraphicsPipeline.get() :
			(bUseSkeletalSkinned ? SkeletalGBufferGraphicsPipeline.get() :
			(bUseSpineVsInline ? SpineVsInlineGBufferGraphicsPipeline.get() :
			(bUseSpineVertexFetch ? SpineGBufferGraphicsPipeline.get() :
			(bUseCpuSpinePipeline ? CpuSpineGBufferGraphicsPipeline.get() :
			(bUseStaticBindlessGeometry ? GBufferBindlessGeometryGraphicsPipeline.get() : GBufferGraphicsPipeline.get()))))));
		renderBackend->BindGraphicsPipeline(activeGBufferPipeline);
		if (!bUseProceduralGrass && materialTable.Buffer)
		{
			BindGBufferSceneResourceBindGroup(
				renderBackend.get(),
				activeGBufferPipeline,
				scene.get(),
				samplerWrap.get(),
				materialTable.Buffer.get(),
				bUseStaticBindlessGeometry ? geometryTable.Buffer.get() : nullptr,
				GBufferDummyDrawRecordBuffer.get());
		}
		std::vector<GraphicsBindGroupEntry> meshBindEntries;
		meshBindEntries.reserve(8);
		if (bUseSpineVertexFetch)
			meshBindEntries.push_back(GraphicsBindGroupEntry::BufferSRV("SpineVertices", mesh->GpuSpineSkinnedVertices.get()));
		if (bUseSpineVsInline)
		{
			meshBindEntries.push_back(GraphicsBindGroupEntry::BufferSRV("SpineVsInlineInputVertices", mesh->GpuSpineInputVertices.get()));
			meshBindEntries.push_back(GraphicsBindGroupEntry::BufferSRV("SpineVsInlineInfluences", mesh->GpuSpineInfluences.get()));
			meshBindEntries.push_back(GraphicsBindGroupEntry::BufferSRV("SpineVsInlineBones", mesh->GpuSpineBones.get()));
		}
		if (bUseSkeletalSkinned || bUseSkeletalVsInline)
		{
			meshBindEntries.push_back(GraphicsBindGroupEntry::BufferSRV("SkeletalInputs", mesh->SkeletalInputVertices.get()));
			meshBindEntries.push_back(GraphicsBindGroupEntry::BufferSRV("SkeletalPrevBones", mesh->SkeletalPrevBoneMatrices.get()));
		}
		if (bUseSkeletalVsInline)
		{
			meshBindEntries.push_back(GraphicsBindGroupEntry::BufferSRV("SkeletalCurrBones", SkeletalUnifiedBoneMatrices.get()));
		}

		// 3D skeletal skinning: swap the bind-pose VB for the compute-skinned
		// output VB. Layout matches the standard IA so the GBuffer PSO is
		// unchanged. Path C (VS inline) reads bind-pose; CPU mode reads the
		// per-frame UPLOAD VB the CPU skinner produced; default uses the
		// compute-output VB.
		VertexBuffer* drawVb = mesh->Vb.get();
		if (bUseSkeletalVsInline)
		{
			drawVb = SkeletalUnifiedBindVb.get();
		}
		else if (bUseSkeletalSkinned)
		{
			drawVb = (bSkeletalUseCpuSkinning && SkeletalUnifiedCpuSkinnedVb)
				? SkeletalUnifiedCpuSkinnedVb.get()
				: mesh->SkeletalOutputVb.get();
		}
		// Procedural grass has no VB/IB — the VS synthesizes geometry. The
		// existing IA bindings are harmless if we skip BindMeshBuffers.
		if (!bUseProceduralGrass)
		{
			if (!bUseStaticBindlessGeometry)
				renderBackend->BindMeshBuffers(drawVb, mesh->Ib.get());
		}

		for (int i = 0; i < mesh->Draws.size(); i++)
		{
			Mesh::DrawCall& drawcall = mesh->Draws[i];
			GBufferConstantBuffer objCB = {};
			int sizea = sizeof(GBufferConstantBuffer);

			objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
			objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);

			//glm::mat4 m; // Identity matrix
			objCB.WorldMatrix = glm::transpose(instanceTransform * mesh->transform);

			objCB.UnjitteredViewProjMat = glm::transpose(UnjitteredViewProjMat);
			objCB.PrevUnjitteredViewProjMat = glm::transpose(PrevUnjitteredViewProjMat);
			objCB.ViewDir.x = RenderFrameCameraLookDirection.x;
			objCB.ViewDir.y = RenderFrameCameraLookDirection.y;
			objCB.ViewDir.z = RenderFrameCameraLookDirection.z;
			objCB.ViewDir.w = 0.0f;
			objCB.BaseColorFactor = drawcall.mat ? drawcall.mat->BaseColorFactor : glm::vec4(1.0f);

			objCB.RTSize.x = GetRenderWidth();
			objCB.RTSize.y = GetRenderHeight();

			objCB.RougnessMetalic.x = Roughness;
			objCB.RougnessMetalic.y = Metalic;

			objCB.bOverrideRougnessMetallic = bOverrideRoughnessMetallic ? 1 : 0;
			// Spine meshes always render unlit + two-sided regardless of
			// whether they go through the compute-skinning vertex-fetch
			// pipeline, the VS-inline path, or the CPU-skinned VBO path.
			const bool bSpineUnlit = bUseSpineVertexFetch || bUseSpineVsInline || mesh->bSpineMesh;
			// Grass blades are thin and translucent; force two-sided so
			// back-of-blade pixels get a normal pointing at the camera and
			// the LightingPS SSS / back-light branch can fire.
			objCB.bTwoSidedLighting = (bSpineUnlit || mesh->bGrassMesh) ? 1u : 0u;
			objCB.bUnlitMaterial = bSpineUnlit ? 1u : 0u;
			objCB.SpineVertexBase = bUseSpineVertexFetch ? drawcall.VertexBase : 0u;
			objCB.SpineSourceScale = (bUseSpineVsInline && mesh->GpuSpineSkinningSourceScale > 0.0f)
				? mesh->GpuSpineSkinningSourceScale : 1.0f;
			const bool bAnySkeletalPath = bUseSkeletalSkinned || bUseSkeletalVsInline;
			objCB.SkeletalCharIndex = bAnySkeletalPath ? mesh->SkeletalCharIndex : 0u;
			objCB.SkeletalVertsPerChar = bAnySkeletalPath ? SkeletalUnifiedVertsPerChar : 0u;
			objCB.SkeletalBoneCount = bAnySkeletalPath ? SkeletalUnifiedBoneCount : 0u;
			// Layer 2 deformation: shared across all draws via render-
			// frame snapshot. Grass bend only fires on meshes flagged
			// bGrassMesh, which DrawScene sets here per draw.
			objCB.MeshDeformParams = glm::vec4(RenderFrameWindTime, 0.0f, 0.0f, 0.0f);
			objCB.GrassBendOrigin = RenderFrameGrassBendOrigin;
			objCB.GrassBendParams = RenderFrameGrassBendParams;
			objCB.bGrassMesh = mesh->bGrassMesh ? 1u : 0u;
			objCB.bTerrainMesh = mesh->bTerrainMesh ? 1u : 0u;
			objCB.bExcludeFromDeformSphere = mesh->bExcludeFromDeformSphere ? 1u : 0u;
			objCB.WindParams = RenderFrameWindParams;
			objCB.WindTuning = RenderFrameWindTuning;
			objCB.TerrainDeformSphere = RenderFrameTerrainDeformSphere;
			if (bUseStaticBindlessGeometry)
			{
				objCB.GBufferGeometryIndex = mesh->GBufferGeometryIndex;
				objCB.GBufferIndexStart = drawcall.IndexStart;
				objCB.GBufferVertexBase = drawcall.VertexBase;
				objCB.bGBufferBindlessGeometry = 1u;
			}
			if (bUseProceduralGrass)
			{
				objCB.PG_BladeCount    = mesh->Procedural.BladeCount;
				objCB.PG_BladeSegments = mesh->Procedural.BladeSegments;
				objCB.PG_BladeHeight   = (std::max)(0.05f, GrassProceduralBladeHeight);
				objCB.PG_HalfAreaXZ    = mesh->Procedural.HalfAreaXZ;
				objCB.PG_Seed          = mesh->Procedural.Seed;
				objCB.PG_BaseY = 0.5f * (mesh->Procedural.HeightMin + mesh->Procedural.HeightMax);
				if (ActiveTerrain && ActiveTerrain->GetHeightBuffer())
				{
					const auto& th = ActiveTerrain->GetData().Header;
					objCB.PG_TerrainWidth   = th.Width;
					objCB.PG_TerrainDepth   = th.Depth;
					objCB.PG_TerrainScaleXZ = th.WorldScaleXZ;
				}
				objCB.PG_RenderDistance   = (std::max)(15.0f, GrassRenderDistance);
				objCB.PG_BladesPerCell    = (std::max)(1, GrassProceduralBladesPerCell);
				objCB.PG_BladeWidthScale  = (std::max)(0.001f, GrassProceduralBladeWidthScale);
				objCB.PG_BladeTipWidthScale = std::clamp(GrassProceduralBladeTipWidthScale, 0.0f, 1.0f);
			}

			objCB.GBufferMaterialIndex = drawcall.GBufferMaterialIndex;

			std::vector<GraphicsBindGroupEntry> drawBindEntries = meshBindEntries;
			drawBindEntries.reserve(meshBindEntries.size() + 8);
			drawBindEntries.push_back(GraphicsBindGroupEntry::Constant(0, &objCB, sizeof(objCB)));
			if (bUseProceduralGrass)
			{
				if (ActiveTerrain && ActiveTerrain->GetHeightBuffer())
					drawBindEntries.push_back(GraphicsBindGroupEntry::BufferSRV("TerrainHeights", ActiveTerrain->GetHeightBuffer().get()));
			}
			else
			{
				if (!materialTable.Buffer)
					continue;
			}
			CreateAndBindGraphicsBindGroup(renderBackend.get(), activeGBufferPipeline, kGraphicsBindGroupSlot_Draw, drawBindEntries);

			static bool bLoggedFirstGBufferDraw = false;
			if (!bLoggedFirstGBufferDraw && !mesh->CpuPositions.empty() && !mesh->CpuIndices.empty() && drawcall.IndexCount >= 3)
			{
				const glm::mat4 world = instanceTransform * mesh->transform;
				auto matrixFinite = [](const glm::mat4& matrix)
				{
					for (int column = 0; column < 4; ++column)
					{
						for (int row = 0; row < 4; ++row)
						{
							if (!std::isfinite(matrix[column][row]))
								return false;
						}
					}
					return true;
				};
				std::wstring logLine =
					L"[GBufferFirstDraw] stride=" + std::to_wstring(mesh->VertexStride) +
					L", vertices=" + std::to_wstring(mesh->NumVertices) +
					L", indices=" + std::to_wstring(mesh->NumIndices) +
					L", indexStart=" + std::to_wstring(drawcall.IndexStart) +
					L", indexCount=" + std::to_wstring(drawcall.IndexCount) +
					L", vertexBase=" + std::to_wstring(drawcall.VertexBase) +
					L", worldFinite=" + std::to_wstring(matrixFinite(world) ? 1 : 0) +
					L", viewProjFinite=" + std::to_wstring(matrixFinite(ViewProjMat) ? 1 : 0) +
					L", viewFinite=" + std::to_wstring(matrixFinite(ViewMat) ? 1 : 0) +
					L", projFinite=" + std::to_wstring(matrixFinite(ProjMat) ? 1 : 0) +
					L", world00=" + std::to_wstring(world[0][0]) +
					L", world30=" + std::to_wstring(world[3][0]) +
					L", world31=" + std::to_wstring(world[3][1]) +
					L", world32=" + std::to_wstring(world[3][2]) +
					L", vp00=" + std::to_wstring(ViewProjMat[0][0]);
				for (uint32_t cornerIndex = 0; cornerIndex < 3; ++cornerIndex)
				{
					const uint32_t indexOffset = drawcall.IndexStart + cornerIndex;
					if (indexOffset >= mesh->CpuIndices.size())
						break;
					const int32_t vertexIndex = static_cast<int32_t>(mesh->CpuIndices[indexOffset]) + drawcall.VertexBase;
					if (vertexIndex < 0 || static_cast<size_t>(vertexIndex) >= mesh->CpuPositions.size())
						break;
					const glm::vec4 local = glm::vec4(mesh->CpuPositions[vertexIndex], 1.0f);
					const glm::vec4 worldPos = world * local;
					const glm::vec4 clip = ViewProjMat * worldPos;
					const glm::vec3 ndc =
						std::abs(clip.w) > 1e-6f ?
						glm::vec3(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w) :
						glm::vec3(0.0f);
					logLine +=
						L", v" + std::to_wstring(cornerIndex) +
						L"Idx=" + std::to_wstring(vertexIndex) +
						L", local=(" + std::to_wstring(local.x) +
						L"," + std::to_wstring(local.y) +
						L"," + std::to_wstring(local.z) +
						L"), world=(" + std::to_wstring(worldPos.x) +
						L"," + std::to_wstring(worldPos.y) +
						L"," + std::to_wstring(worldPos.z) +
						L"," + std::to_wstring(worldPos.w) +
						L")" +
						L", clip=(" + std::to_wstring(clip.x) +
						L"," + std::to_wstring(clip.y) +
						L"," + std::to_wstring(clip.z) +
						L"," + std::to_wstring(clip.w) +
						L"), ndc=(" + std::to_wstring(ndc.x) +
						L"," + std::to_wstring(ndc.y) +
						L"," + std::to_wstring(ndc.z) + L")";
				}
				AppendCpuRuntimeTrace(logLine);
				bLoggedFirstGBufferDraw = true;
			}

			if (bUseProceduralGrass)
			{
				const uint32_t vertsPerBlade = mesh->Procedural.BladeSegments * 6u;
				if (ProceduralGrassSequentialIb)
				{
					// Instance count = cells_in_grid × bladesPerCell, clamped
					// to the recipe's BladeCount as a hard upper bound.
					constexpr float kCellSize = 30.0f;
					const uint32_t halfExt = (std::max)(1u,
						static_cast<uint32_t>(objCB.PG_RenderDistance / kCellSize));
					const uint32_t gridSide = halfExt * 2u + 1u;
					const uint32_t cells = gridSide * gridSide;
					const uint64_t want = static_cast<uint64_t>(cells) * objCB.PG_BladesPerCell;
					const uint32_t instanceCount = static_cast<uint32_t>(
						(std::min<uint64_t>)(want, mesh->Procedural.BladeCount));
					renderBackend->BindMeshBuffers(nullptr, ProceduralGrassSequentialIb.get());
					renderBackend->DrawIndexedInstanced(
						vertsPerBlade,
						instanceCount,
						0, 0, 0);
				}
			}
			else
			{
				if (bUseStaticBindlessGeometry)
				{
					renderBackend->DrawInstanced(drawcall.IndexCount, 1, 0, 0);
				}
				else
				{
					renderBackend->DrawIndexed(
						drawcall.IndexCount,
						drawcall.IndexStart,
						bUseSpineVertexFetch ? 0 : drawcall.VertexBase);
				}
			}
		}
	}
}

void Corona::DrawSceneShadowMap(shared_ptr<Scene> scene, const glm::mat4x4& instanceTransform)
{
	if (!scene || !MobileShadowMapGraphicsPipeline)
		return;

	for (auto& mesh : scene->meshes)
	{
		if (!mesh)
			continue;

		const bool bUseSpineVertexFetch =
			mesh->bGpuSpineSkinned &&
			mesh->bGpuSpineSkinningDispatched &&
			mesh->GpuSpineSkinnedVertices &&
			SpineMobileShadowMapGraphicsPipeline;
		// Skeletal meshes upload StandardVertex (48 B). Their skinned shadow
		// VBs share the same layout. The base shadow PSO uses the 44 B
		// platformer Vertex layout, so route Skeletal to its own PSO.
		const bool bUseSkeletalShadow =
			!bUseSpineVertexFetch &&
			mesh->bSkeletalSkinned &&
			SkeletalMobileShadowMapGraphicsPipeline;
		GraphicsPipelineHandle* activeShadowPipeline =
			bUseSpineVertexFetch ? SpineMobileShadowMapGraphicsPipeline.get() :
			(bUseSkeletalShadow ? SkeletalMobileShadowMapGraphicsPipeline.get() :
			MobileShadowMapGraphicsPipeline.get());
		renderBackend->BindGraphicsPipeline(activeShadowPipeline);
		std::vector<GraphicsBindGroupEntry> shadowMeshBindEntries;
		shadowMeshBindEntries.reserve(2);
		if (bUseSpineVertexFetch)
			shadowMeshBindEntries.push_back(GraphicsBindGroupEntry::BufferSRV("SpineVertices", mesh->GpuSpineSkinnedVertices.get()));

		// Skeletal meshes share a single-copy bind-pose VB; using
		// drawcall.VertexBase against that buffer reads past the end for
		// every char after #0. Route to the per-frame skinned VB instead
		// (CPU-skinned, GPU-compute output, or bind-pose for VS inline).
		VertexBuffer* shadowVb = mesh->Vb.get();
		if (mesh->bSkeletalSkinned && mesh->bSkeletalSkinningDispatched)
		{
			if (bSkeletalUseVsInlineSkinning && SkeletalUnifiedBindVb)
			{
				// VS inline doesn't skin shadows — fall back to bind pose
				// but use BaseVertexLocation = 0 since the shared VB only
				// holds one char's worth of data. Skinned shadow is lost
				// in this mode; acceptable for the benchmark.
				shadowVb = SkeletalUnifiedBindVb.get();
			}
			else if (bSkeletalUseCpuSkinning && SkeletalUnifiedCpuSkinnedVb)
			{
				shadowVb = SkeletalUnifiedCpuSkinnedVb.get();
			}
			else if (mesh->SkeletalOutputVb)
			{
				shadowVb = mesh->SkeletalOutputVb.get();
			}
		}
		renderBackend->BindMeshBuffers(shadowVb, mesh->Ib.get());

		for (int i = 0; i < mesh->Draws.size(); i++)
		{
			Mesh::DrawCall& drawcall = mesh->Draws[i];
			ShadowMapConstantBuffer objCB = {};
			objCB.LightViewProjectionMatrix = glm::transpose(MobileShadowViewProjMat);
			objCB.WorldMatrix = glm::transpose(instanceTransform * mesh->transform);
			objCB.BaseColorFactor = glm::vec4(1.0f);
			objCB.SpineVertexBase = bUseSpineVertexFetch ? drawcall.VertexBase : 0u;

			std::vector<GraphicsBindGroupEntry> shadowDrawBindEntries = shadowMeshBindEntries;
			shadowDrawBindEntries.push_back(GraphicsBindGroupEntry::Constant(0, &objCB, sizeof(objCB)));
			CreateAndBindGraphicsBindGroup(renderBackend.get(), activeShadowPipeline, kGraphicsBindGroupSlot_Draw, shadowDrawBindEntries);

			renderBackend->DrawIndexed(
				drawcall.IndexCount,
				drawcall.IndexStart,
				bUseSpineVertexFetch ? 0 : drawcall.VertexBase);
		}
	}
}

bool Corona::BuildMobileShadowViewProjection(glm::mat4x4& lightViewProj)
{
	auto isFinite3 = [](const glm::vec3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	};

	auto makeAabbCorners = [](const glm::vec3& boundsMin, const glm::vec3& boundsMax)
	{
		return std::array<glm::vec3, 8>
		{
			glm::vec3(boundsMin.x, boundsMin.y, boundsMin.z),
			glm::vec3(boundsMax.x, boundsMin.y, boundsMin.z),
			glm::vec3(boundsMin.x, boundsMax.y, boundsMin.z),
			glm::vec3(boundsMax.x, boundsMax.y, boundsMin.z),
			glm::vec3(boundsMin.x, boundsMin.y, boundsMax.z),
			glm::vec3(boundsMax.x, boundsMin.y, boundsMax.z),
			glm::vec3(boundsMin.x, boundsMax.y, boundsMax.z),
			glm::vec3(boundsMax.x, boundsMax.y, boundsMax.z),
		};
	};

	auto expandBounds = [](glm::vec3& boundsMin, glm::vec3& boundsMax, const glm::vec3& point)
	{
		boundsMin = glm::min(boundsMin, point);
		boundsMax = glm::max(boundsMax, point);
	};

	struct MobileShadowObjectBounds
	{
		uint32_t ObjectIndex = 0;
		glm::vec3 BoundsMin = glm::vec3(0.0f);
		glm::vec3 BoundsMax = glm::vec3(0.0f);
		glm::vec3 Extents = glm::vec3(0.0f);
		glm::vec3 Center = glm::vec3(0.0f);
		float Radius = 0.0f;
		float CameraDistance = 0.0f;
		bool bCameraVisible = false;
		bool bCameraReceiver = false;
		bool bPlayerCharacter = false;
	};

	MobileShadowCasterObjectIndices.clear();
	MobileShadowLastTotalObjectCount = 0;
	MobileShadowLastCandidateObjectCount = 0;
	MobileShadowLastReceiverObjectCount = 0;
	MobileShadowLastCasterObjectCount = 0;
	MobileShadowLastGuaranteedCasterCount = 0;
	MobileShadowLastCulledObjectCount = 0;
	MobileShadowLastDistanceCulledObjectCount = 0;

	std::vector<MobileShadowObjectBounds> boundedObjects;
	boundedObjects.reserve(RenderWorld.SceneObjects.size());

	const glm::vec3 cameraPosition =
		isFinite3(m_camera.m_position) ?
		m_camera.m_position :
		glm::vec3(InvViewMat[3]);
	glm::vec3 cameraForward =
		isFinite3(m_camera.m_lookDirection) && glm::length(m_camera.m_lookDirection) > 0.0001f ?
		glm::normalize(m_camera.m_lookDirection) :
		glm::vec3(0.0f, 0.0f, 1.0f);
	const glm::vec3 focusReceiverCenter = cameraPosition + cameraForward * MobileShadowFocusDistance;
	const glm::vec3 focusReceiverHalfExtent(
		MobileShadowFocusRadius,
		MobileShadowFocusRadius * 0.60f,
		MobileShadowFocusRadius);
	glm::vec3 receiverMin(std::numeric_limits<float>::max());
	glm::vec3 receiverMax(-std::numeric_limits<float>::max());
	bool bHasReceiverBounds = false;

	for (uint32_t objectIndex = 0; objectIndex < static_cast<uint32_t>(RenderWorld.SceneObjects.size()); ++objectIndex)
	{
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		if (!object.bVisible || !object.ScenePtr || !object.bRayTracing)
			continue;

		++MobileShadowLastTotalObjectCount;

		glm::vec3 boundsMin(0.0f);
		glm::vec3 boundsMax(0.0f);
		glm::vec3 boundsCenter(0.0f);
		float boundsRadius = 0.0f;
		if (!GetSceneObjectWorldBounds(object, boundsMin, boundsMax, boundsCenter, boundsRadius))
			continue;
		if (!isFinite3(boundsMin) || !isFinite3(boundsMax))
			continue;

		MobileShadowObjectBounds objectBounds = {};
		objectBounds.ObjectIndex = objectIndex;
		objectBounds.BoundsMin = boundsMin;
		objectBounds.BoundsMax = boundsMax;
		objectBounds.Extents = glm::max(boundsMax - boundsMin, glm::vec3(0.0f));
		objectBounds.Center = boundsCenter;
		objectBounds.Radius = boundsRadius;
		objectBounds.CameraDistance = std::max(0.0f, glm::length(boundsCenter - cameraPosition) - boundsRadius);
		objectBounds.bCameraVisible = IsWorldAabbInViewFrustum(boundsMin, boundsMax);
		if (const std::string* entityName = EntityWorld.GetName(object.EntityHandle))
			objectBounds.bPlayerCharacter = *entityName == "DungeonCharacter";
		boundedObjects.push_back(objectBounds);
	}

	const MobileShadowObjectBounds* playerBounds = nullptr;
	for (const MobileShadowObjectBounds& objectBounds : boundedObjects)
	{
		if (objectBounds.bPlayerCharacter)
		{
			playerBounds = &objectBounds;
			break;
		}
	}

	const glm::vec3 shadowFocusCenter = playerBounds ? playerBounds->Center : focusReceiverCenter;
	receiverMin = shadowFocusCenter - focusReceiverHalfExtent;
	receiverMax = shadowFocusCenter + focusReceiverHalfExtent;
	bHasReceiverBounds = true;
	if (playerBounds)
	{
		receiverMin = glm::min(receiverMin, playerBounds->BoundsMin);
		receiverMax = glm::max(receiverMax, playerBounds->BoundsMax);
	}

	if (!bHasReceiverBounds)
		return false;

	glm::vec3 lightDir = RenderFrameNormalizedLightDir;
	if (!isFinite3(lightDir) || glm::length(lightDir) < 0.0001f)
		lightDir = LightDir;
	if (!isFinite3(lightDir) || glm::length(lightDir) < 0.0001f)
		lightDir = glm::vec3(0.3f, 0.8f, 0.4f);
	lightDir = glm::normalize(lightDir);

	const glm::vec3 receiverCenter = (receiverMin + receiverMax) * 0.5f;
	const float receiverRadius = std::max(glm::length(receiverMax - receiverMin) * 0.5f, 8.0f);
	const glm::vec3 eye = receiverCenter + lightDir * (receiverRadius + 32.0f);
	glm::vec3 up = std::abs(glm::dot(lightDir, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f ?
		glm::vec3(0.0f, 0.0f, 1.0f) :
		glm::vec3(0.0f, 1.0f, 0.0f);

	const glm::mat4x4 lightView = glm::lookAtRH(eye, receiverCenter, up);

	struct LightSpaceBounds
	{
		glm::vec3 Min = glm::vec3(std::numeric_limits<float>::max());
		glm::vec3 Max = glm::vec3(-std::numeric_limits<float>::max());
	};

	auto computeLightSpaceBounds = [&](const glm::vec3& boundsMin, const glm::vec3& boundsMax)
	{
		LightSpaceBounds lightBounds;
		for (const glm::vec3& corner : makeAabbCorners(boundsMin, boundsMax))
		{
			const glm::vec3 lightSpaceCorner = glm::vec3(lightView * glm::vec4(corner, 1.0f));
			expandBounds(lightBounds.Min, lightBounds.Max, lightSpaceCorner);
		}
		return lightBounds;
	};

	glm::vec3 receiverLightMin(std::numeric_limits<float>::max());
	glm::vec3 receiverLightMax(-std::numeric_limits<float>::max());
	const LightSpaceBounds focusLightBounds = computeLightSpaceBounds(receiverMin, receiverMax);
	expandBounds(receiverLightMin, receiverLightMax, focusLightBounds.Min);
	expandBounds(receiverLightMin, receiverLightMax, focusLightBounds.Max);

	const float xyPadding = std::clamp(receiverRadius * 0.03f, 4.0f, 64.0f);
	const float zPadding = std::clamp(receiverRadius * 0.05f, 6.0f, 128.0f);
	glm::vec3 projectionLightMin = receiverLightMin;
	glm::vec3 projectionLightMax = receiverLightMax;

	float shadowMinZ = receiverLightMin.z;
	float shadowMaxZ = receiverLightMax.z;

	struct NearbyCasterCandidate
	{
		const MobileShadowObjectBounds* Bounds = nullptr;
		float DistanceSq = 0.0f;
	};

	std::vector<const MobileShadowObjectBounds*> selectedCasterBounds;
	selectedCasterBounds.reserve(static_cast<size_t>(MobileShadowNearbyCasterCount + 1));
	std::vector<NearbyCasterCandidate> nearbyCandidates;
	nearbyCandidates.reserve(boundedObjects.size());
	for (const MobileShadowObjectBounds& objectBounds : boundedObjects)
	{
		if (objectBounds.bPlayerCharacter)
		{
			selectedCasterBounds.push_back(&objectBounds);
			++MobileShadowLastGuaranteedCasterCount;
			continue;
		}

		if (!objectBounds.bCameraVisible || objectBounds.Extents.y < MobileShadowMinCasterHeight)
			continue;

		const glm::vec3 delta = objectBounds.Center - shadowFocusCenter;
		nearbyCandidates.push_back({ &objectBounds, glm::dot(delta, delta) });
	}
	MobileShadowLastCandidateObjectCount =
		static_cast<uint64_t>(nearbyCandidates.size() + selectedCasterBounds.size());
	std::sort(nearbyCandidates.begin(), nearbyCandidates.end(), [](const NearbyCasterCandidate& lhs, const NearbyCasterCandidate& rhs)
	{
		if (lhs.DistanceSq != rhs.DistanceSq)
			return lhs.DistanceSq < rhs.DistanceSq;
		const float lhsRadius = lhs.Bounds ? lhs.Bounds->Radius : 0.0f;
		const float rhsRadius = rhs.Bounds ? rhs.Bounds->Radius : 0.0f;
		return lhsRadius < rhsRadius;
	});
	for (const NearbyCasterCandidate& candidate : nearbyCandidates)
	{
		if (selectedCasterBounds.size() >= static_cast<size_t>(MobileShadowNearbyCasterCount + (playerBounds ? 1u : 0u)))
			break;
		if (candidate.Bounds)
			selectedCasterBounds.push_back(candidate.Bounds);
	}

	for (const MobileShadowObjectBounds* objectBounds : selectedCasterBounds)
	{
		if (!objectBounds)
			continue;

		MobileShadowCasterObjectIndices.push_back(objectBounds->ObjectIndex);
		const LightSpaceBounds casterLightBounds = computeLightSpaceBounds(objectBounds->BoundsMin, objectBounds->BoundsMax);
		expandBounds(projectionLightMin, projectionLightMax, casterLightBounds.Min);
		expandBounds(projectionLightMin, projectionLightMax, casterLightBounds.Max);
		shadowMinZ = std::min(shadowMinZ, casterLightBounds.Min.z);
		shadowMaxZ = std::max(shadowMaxZ, casterLightBounds.Max.z);
		if (objectBounds->bCameraVisible)
		{
			++MobileShadowLastReceiverObjectCount;
			receiverMin = glm::min(receiverMin, objectBounds->BoundsMin);
			receiverMax = glm::max(receiverMax, objectBounds->BoundsMax);
		}
	}

	MobileShadowLastCasterObjectCount = MobileShadowCasterObjectIndices.size();
	MobileShadowLastCulledObjectCount =
		MobileShadowLastTotalObjectCount > MobileShadowLastCasterObjectCount ?
		MobileShadowLastTotalObjectCount - MobileShadowLastCasterObjectCount :
		0;
	if (MobileShadowCasterObjectIndices.empty())
		return false;

	float left = projectionLightMin.x - xyPadding;
	float right = projectionLightMax.x + xyPadding;
	float bottom = projectionLightMin.y - xyPadding;
	float top = projectionLightMax.y + xyPadding;
	const float minExtent = 8.0f;
	if (right - left < minExtent)
	{
		const float center = (left + right) * 0.5f;
		left = center - minExtent * 0.5f;
		right = center + minExtent * 0.5f;
	}
	if (top - bottom < minExtent)
	{
		const float center = (bottom + top) * 0.5f;
		bottom = center - minExtent * 0.5f;
		top = center + minExtent * 0.5f;
	}

	auto snapBoundsToShadowTexels = [](float& minValue, float& maxValue)
	{
		const float extent = maxValue - minValue;
		if (extent <= 0.0f)
			return;

		const float texelSize = extent / static_cast<float>(MobileShadowMapResolution);
		if (texelSize <= 1.0e-5f)
			return;

		const float center = (minValue + maxValue) * 0.5f;
		const float snappedCenter = std::floor((center / texelSize) + 0.5f) * texelSize;
		minValue = snappedCenter - extent * 0.5f;
		maxValue = snappedCenter + extent * 0.5f;
	};
	snapBoundsToShadowTexels(left, right);
	snapBoundsToShadowTexels(bottom, top);

	const float nearPlane = std::max(0.1f, -shadowMaxZ - zPadding);
	const float farPlane = std::max(nearPlane + 1.0f, -shadowMinZ + zPadding);
	const glm::mat4x4 lightProjection = glm::orthoRH(left, right, bottom, top, nearPlane, farPlane);
	lightViewProj = lightProjection * lightView;
	return true;
}

void Corona::MobileShadowMapPass()
{
	bMobileShadowMapValidThisFrame = false;
	if (!renderBackend ||
		RenderingMode != ERenderingMode::HYBRID ||
		!MobileShadowMapGraphicsPipeline ||
		!ShadowBuffer)
	{
		return;
	}

	glm::mat4x4 lightViewProj(1.0f);
	if (!BuildMobileShadowViewProjection(lightViewProj))
		return;

	MobileShadowViewProjMat = lightViewProj;
	renderBackend->EmitGpuCrashMarker("MobileShadowMapPass");
	DispatchSpineSkinningForRenderWorld();
	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::ShaderRead, EResourceState::DepthWrite);
	renderBackend->ClearDepth(ShadowBuffer.get(), 1.0f);
	renderBackend->SetViewportAndScissor(MobileShadowMapResolution, MobileShadowMapResolution);
	renderBackend->SetRenderTargets(nullptr, 0, ShadowBuffer.get());
	renderBackend->BindGraphicsPipeline(MobileShadowMapGraphicsPipeline.get());

	for (uint32_t objectIndex : MobileShadowCasterObjectIndices)
	{
		if (objectIndex >= RenderWorld.SceneObjects.size())
			continue;
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		if (!object.bVisible || !object.ScenePtr)
			continue;
		DrawSceneShadowMap(object.ScenePtr, object.Transform);
	}

	if ((FrameCounter % 120u) == 0u &&
		(MobileShadowLastTotalObjectCount != 0 ||
		 MobileShadowLastCasterObjectCount != 0 ||
		 MobileShadowLastCulledObjectCount != 0))
	{
		AppendCpuRuntimeTrace(
			L"[MobileShadowCulling] casters=" + std::to_wstring(MobileShadowLastCasterObjectCount) +
			L"/" + std::to_wstring(MobileShadowLastTotalObjectCount) +
			L", candidates=" + std::to_wstring(MobileShadowLastCandidateObjectCount) +
			L", receivers=" + std::to_wstring(MobileShadowLastReceiverObjectCount) +
			L", guaranteed=" + std::to_wstring(MobileShadowLastGuaranteedCasterCount) +
			L", culled=" + std::to_wstring(MobileShadowLastCulledObjectCount) +
			L", distance=" + std::to_wstring(MobileShadowLastDistanceCulledObjectCount));
	}

	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::DepthWrite, EResourceState::ShaderRead);
	bMobileShadowMapValidThisFrame = true;
	bShadowOutputValidThisFrame = true;
}

bool Corona::GetSceneObjectWorldBounds(
	const SceneObject& object,
	glm::vec3& boundsMin,
	glm::vec3& boundsMax,
	glm::vec3& center,
	float& radius) const
{
	if (!object.ScenePtr || !object.ScenePtr->bHasBounds)
		return false;

	if (object.bWorldBoundsCacheValid)
	{
		boundsMin = object.CachedWorldBoundsMin;
		boundsMax = object.CachedWorldBoundsMax;
		center = object.CachedWorldBoundsCenter;
		radius = object.CachedWorldBoundsRadius;
		return radius > 0.001f;
	}

	const glm::vec3 localMin = object.ScenePtr->BoundsMin;
	const glm::vec3 localMax = object.ScenePtr->BoundsMax;
	const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
	const glm::vec3 localExtents = glm::max((localMax - localMin) * 0.5f, glm::vec3(0.0f));
	center = glm::vec3(object.Transform * glm::vec4(localCenter, 1.0f));
	const glm::vec3 extents(
		std::abs(object.Transform[0][0]) * localExtents.x + std::abs(object.Transform[1][0]) * localExtents.y + std::abs(object.Transform[2][0]) * localExtents.z,
		std::abs(object.Transform[0][1]) * localExtents.x + std::abs(object.Transform[1][1]) * localExtents.y + std::abs(object.Transform[2][1]) * localExtents.z,
		std::abs(object.Transform[0][2]) * localExtents.x + std::abs(object.Transform[1][2]) * localExtents.y + std::abs(object.Transform[2][2]) * localExtents.z);
	boundsMin = center - extents;
	boundsMax = center + extents;
	radius = glm::length(extents);
	object.CachedWorldBoundsMin = boundsMin;
	object.CachedWorldBoundsMax = boundsMax;
	object.CachedWorldBoundsCenter = center;
	object.CachedWorldBoundsRadius = radius;
	object.bWorldBoundsCacheValid = radius > 0.001f;
	return radius > 0.001f;
}

bool Corona::IsWorldAabbInViewFrustum(const glm::vec3& boundsMin, const glm::vec3& boundsMax) const
{
	const FrustumPlaneArray planes = BuildFrustumPlanes(UnjitteredViewProjMat);
	return ClassifyAabbAgainstFrustumPlanes(planes, boundsMin, boundsMax) != EFrustumAabbRelation::Outside;
}

void Corona::MarkRenderWorldCullingIndexDirty()
{
	RenderWorld.bSceneObjectCullingIndexDirty = true;
}

void Corona::RebuildRenderWorldCullingIndex()
{
	const bool bGBufferCullProfileEnabled = IsGBufferCullingProfileEnabled();
	GBufferCullingProfile& gbufferCullProfile = GetGBufferCullingProfile();
	const auto boundsProfileBegin =
		bGBufferCullProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};

	RenderWorld.SceneObjectCullingData.clear();
	RenderWorld.SceneObjectCullingData.resize(RenderWorld.SceneObjects.size());
	RenderWorld.SceneObjectCullingCells.clear();
	RenderWorld.UnboundedSceneObjectIndices.clear();
	RenderWorld.SceneObjectCullingCellLookup.clear();
	RenderWorld.SceneObjectCullingObjectCount = 0;
	RenderWorld.SceneObjectCullingCells.reserve(std::max<size_t>(16, RenderWorld.SceneObjects.size() / 64));
	RenderWorld.UnboundedSceneObjectIndices.reserve(64);
	RenderWorld.SceneObjectCullingCellLookup.reserve(std::max<size_t>(16, RenderWorld.SceneObjects.size() / 64));

	auto isFinite3 = [](const glm::vec3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	};

	const uint32_t objectCount = static_cast<uint32_t>(RenderWorld.SceneObjects.size());
	auto buildObjectCullingRecord = [&](uint32_t objectIndex)
	{
		const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
		RenderWorldMirror::SceneObjectCullingRecord& objectData = RenderWorld.SceneObjectCullingData[objectIndex];
		objectData.Visible = object.bVisible && object.ScenePtr != nullptr;
		if (!objectData.Visible)
			return;

		glm::vec3 boundsMin(0.0f);
		glm::vec3 boundsMax(0.0f);
		glm::vec3 boundsCenter(0.0f);
		float boundsRadius = 0.0f;
		objectData.HasBounds = GetSceneObjectWorldBounds(object, boundsMin, boundsMax, boundsCenter, boundsRadius) &&
			isFinite3(boundsMin) &&
			isFinite3(boundsMax) &&
			isFinite3(boundsCenter) &&
			std::isfinite(boundsRadius);
		if (!objectData.HasBounds)
			return;

		objectData.BoundsMin = boundsMin;
		objectData.BoundsMax = boundsMax;
		objectData.BoundsCenter = boundsCenter;
		objectData.BoundsRadius = boundsRadius;
	};

	const uint32_t taskThreadCount = std::max<uint32_t>(1u, g_TS.GetNumTaskThreads());
	if (objectCount >= kParallelCullingIndexBuildThreshold && taskThreadCount > 1u)
	{
		enki::TaskSet buildRecordsTask(objectCount, [&](enki::TaskSetPartition range, uint32_t)
		{
			for (uint32_t objectIndex = range.start; objectIndex < range.end; ++objectIndex)
				buildObjectCullingRecord(objectIndex);
		});
		buildRecordsTask.m_MinRange = 256u;
		g_TS.AddTaskSetToPipe(&buildRecordsTask);
		g_TS.WaitforTask(&buildRecordsTask);
	}
	else
	{
		for (uint32_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
			buildObjectCullingRecord(objectIndex);
	}

	for (uint32_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
	{
		const RenderWorldMirror::SceneObjectCullingRecord& objectData = RenderWorld.SceneObjectCullingData[objectIndex];
		if (!objectData.Visible)
			continue;
		++RenderWorld.SceneObjectCullingObjectCount;
		if (!objectData.HasBounds)
		{
			RenderWorld.UnboundedSceneObjectIndices.push_back(objectIndex);
			continue;
		}

		const int32_t cellX = static_cast<int32_t>(std::floor(objectData.BoundsCenter.x / kRenderWorldCullingCellSize));
		const int32_t cellZ = static_cast<int32_t>(std::floor(objectData.BoundsCenter.z / kRenderWorldCullingCellSize));
		const uint64_t cellKey = PackCullingCellCoord(cellX, cellZ);
		auto cellIt = RenderWorld.SceneObjectCullingCellLookup.find(cellKey);
		if (cellIt == RenderWorld.SceneObjectCullingCellLookup.end())
		{
			const uint32_t newCellIndex = static_cast<uint32_t>(RenderWorld.SceneObjectCullingCells.size());
			cellIt = RenderWorld.SceneObjectCullingCellLookup.emplace(cellKey, newCellIndex).first;
			RenderWorld.SceneObjectCullingCells.push_back({});
		}

		RenderWorldMirror::SceneObjectCullingCell& cell = RenderWorld.SceneObjectCullingCells[cellIt->second];
		cell.BoundsMin = glm::min(cell.BoundsMin, objectData.BoundsMin);
		cell.BoundsMax = glm::max(cell.BoundsMax, objectData.BoundsMax);
		cell.ObjectIndices.push_back(objectIndex);
	}

	RenderWorld.bSceneObjectCullingIndexDirty = false;
	++RenderWorld.SceneObjectCullingIndexGeneration;
	if (bGBufferCullProfileEnabled)
	{
		gbufferCullProfile.BoundsTests += static_cast<uint64_t>(RenderWorld.SceneObjects.size());
		GBufferProfileAdd(true, gbufferCullProfile.BoundsMs, gbufferCullProfile.BoundsCount, boundsProfileBegin);
	}
}

const std::vector<uint32_t>& Corona::GatherGBufferVisibleObjectIndices()
{
	const bool bGBufferCullProfileEnabled = IsGBufferCullingProfileEnabled();
	GBufferCullingProfile& gbufferCullProfile = GetGBufferCullingProfile();
	if (bGBufferCullProfileEnabled)
		gbufferCullProfile.BeginFrame(static_cast<uint64_t>(FrameCounter));
	const auto totalProfileBegin =
		bGBufferCullProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};

	if (RenderWorld.bSceneObjectCullingIndexDirty ||
		RenderWorld.SceneObjectCullingData.size() != RenderWorld.SceneObjects.size())
	{
		RebuildRenderWorldCullingIndex();
	}

	GBufferVisibleObjectIndices.clear();
	GBufferSpatialFrustumCandidateIndices.clear();
	GBufferLastSpatialCellCount = RenderWorld.SceneObjectCullingCells.size();
	GBufferLastSpatialVisibleCellCount = 0;
	GBufferLastSpatialPartialCellCount = 0;
	GBufferLastSpatialCandidateObjectCount = 0;
	GBufferLastSpatialVisibleObjectCount = 0;

	const uint64_t totalCullableObjectCount = RenderWorld.SceneObjectCullingObjectCount;
	GBufferLastTotalObjectCount = totalCullableObjectCount;

	GBufferVisibleObjectIndices.reserve(static_cast<size_t>(std::min<uint64_t>(totalCullableObjectCount, RenderWorld.SceneObjects.size())));
	for (uint32_t objectIndex : RenderWorld.UnboundedSceneObjectIndices)
		GBufferVisibleObjectIndices.push_back(objectIndex);

	const FrustumPlaneArray planes = BuildFrustumPlanes(UnjitteredViewProjMat);
	const uint32_t taskThreadCount = std::max<uint32_t>(1u, g_TS.GetNumTaskThreads());
	const auto candidateScanProfileBegin =
		bGBufferCullProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	const uint32_t cellCount = static_cast<uint32_t>(RenderWorld.SceneObjectCullingCells.size());
	if (cellCount >= kParallelCellFrustumCullThreshold && taskThreadCount > 1u)
	{
		GBufferSpatialThreadVisibleIndices.resize(taskThreadCount);
		GBufferSpatialThreadCandidateIndices.resize(taskThreadCount);
		const size_t perThreadObjectReserve =
			static_cast<size_t>(std::max<uint64_t>(64u, totalCullableObjectCount / taskThreadCount));
		for (uint32_t threadIndex = 0; threadIndex < taskThreadCount; ++threadIndex)
		{
			GBufferSpatialThreadVisibleIndices[threadIndex].clear();
			GBufferSpatialThreadVisibleIndices[threadIndex].reserve(perThreadObjectReserve);
			GBufferSpatialThreadCandidateIndices[threadIndex].clear();
			GBufferSpatialThreadCandidateIndices[threadIndex].reserve(perThreadObjectReserve / 2u + 64u);
		}
		std::vector<uint32_t> visibleCellCounts(taskThreadCount, 0u);
		std::vector<uint32_t> partialCellCounts(taskThreadCount, 0u);

		enki::TaskSet cellCullTask(cellCount, [&](enki::TaskSetPartition range, uint32_t threadnum)
		{
			const uint32_t threadIndex = std::min<uint32_t>(threadnum, taskThreadCount - 1u);
			std::vector<uint32_t>& threadVisible = GBufferSpatialThreadVisibleIndices[threadIndex];
			std::vector<uint32_t>& threadCandidates = GBufferSpatialThreadCandidateIndices[threadIndex];
			uint32_t localVisibleCells = 0;
			uint32_t localPartialCells = 0;
			for (uint32_t cellIndex = range.start; cellIndex < range.end; ++cellIndex)
			{
				const RenderWorldMirror::SceneObjectCullingCell& cell = RenderWorld.SceneObjectCullingCells[cellIndex];
				if (cell.ObjectIndices.empty())
					continue;

				const EFrustumAabbRelation relation =
					ClassifyAabbAgainstFrustumPlanes(planes, cell.BoundsMin, cell.BoundsMax);
				if (relation == EFrustumAabbRelation::Outside)
					continue;

				++localVisibleCells;
				if (relation == EFrustumAabbRelation::Inside)
				{
					threadVisible.insert(
						threadVisible.end(),
						cell.ObjectIndices.begin(),
						cell.ObjectIndices.end());
					continue;
				}

				++localPartialCells;
				threadCandidates.insert(
					threadCandidates.end(),
					cell.ObjectIndices.begin(),
					cell.ObjectIndices.end());
			}
			visibleCellCounts[threadIndex] += localVisibleCells;
			partialCellCounts[threadIndex] += localPartialCells;
		});
		cellCullTask.m_MinRange = 64u;
		g_TS.AddTaskSetToPipe(&cellCullTask);
		g_TS.WaitforTask(&cellCullTask);

		for (uint32_t threadIndex = 0; threadIndex < taskThreadCount; ++threadIndex)
		{
			GBufferLastSpatialVisibleCellCount += visibleCellCounts[threadIndex];
			GBufferLastSpatialPartialCellCount += partialCellCounts[threadIndex];
			GBufferVisibleObjectIndices.insert(
				GBufferVisibleObjectIndices.end(),
				GBufferSpatialThreadVisibleIndices[threadIndex].begin(),
				GBufferSpatialThreadVisibleIndices[threadIndex].end());
			GBufferSpatialFrustumCandidateIndices.insert(
				GBufferSpatialFrustumCandidateIndices.end(),
				GBufferSpatialThreadCandidateIndices[threadIndex].begin(),
				GBufferSpatialThreadCandidateIndices[threadIndex].end());
		}
	}
	else
	{
		for (const RenderWorldMirror::SceneObjectCullingCell& cell : RenderWorld.SceneObjectCullingCells)
		{
			if (cell.ObjectIndices.empty())
				continue;

			const EFrustumAabbRelation relation =
				ClassifyAabbAgainstFrustumPlanes(planes, cell.BoundsMin, cell.BoundsMax);
			if (relation == EFrustumAabbRelation::Outside)
				continue;

			++GBufferLastSpatialVisibleCellCount;
			if (relation == EFrustumAabbRelation::Inside)
			{
				GBufferVisibleObjectIndices.insert(
					GBufferVisibleObjectIndices.end(),
					cell.ObjectIndices.begin(),
					cell.ObjectIndices.end());
				continue;
			}

			++GBufferLastSpatialPartialCellCount;
			GBufferSpatialFrustumCandidateIndices.insert(
				GBufferSpatialFrustumCandidateIndices.end(),
				cell.ObjectIndices.begin(),
				cell.ObjectIndices.end());
		}
	}
	if (bGBufferCullProfileEnabled)
	{
		gbufferCullProfile.CandidateScans += static_cast<uint64_t>(RenderWorld.SceneObjectCullingCells.size());
		GBufferProfileAdd(true, gbufferCullProfile.CandidateScanMs, gbufferCullProfile.CandidateScanCount, candidateScanProfileBegin);
	}

	const auto objectScanProfileBegin =
		bGBufferCullProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	GBufferLastSpatialCandidateObjectCount = GBufferSpatialFrustumCandidateIndices.size();
	const uint32_t candidateCount = static_cast<uint32_t>(GBufferSpatialFrustumCandidateIndices.size());
	if (candidateCount >= kParallelFrustumCullThreshold && taskThreadCount > 1u)
	{
		GBufferSpatialThreadVisibleIndices.resize(taskThreadCount);
		for (std::vector<uint32_t>& threadIndices : GBufferSpatialThreadVisibleIndices)
		{
			threadIndices.clear();
			threadIndices.reserve((candidateCount / taskThreadCount) + 64u);
		}

		enki::TaskSet frustumCullTask(candidateCount, [&](enki::TaskSetPartition range, uint32_t threadnum)
		{
			std::vector<uint32_t>& threadVisible =
				GBufferSpatialThreadVisibleIndices[std::min<uint32_t>(threadnum, taskThreadCount - 1u)];
			for (uint32_t candidateIndex = range.start; candidateIndex < range.end; ++candidateIndex)
			{
				const uint32_t objectIndex = GBufferSpatialFrustumCandidateIndices[candidateIndex];
				if (objectIndex >= RenderWorld.SceneObjectCullingData.size())
					continue;
				const RenderWorldMirror::SceneObjectCullingRecord& objectData =
					RenderWorld.SceneObjectCullingData[objectIndex];
				if (!objectData.Visible || !objectData.HasBounds)
					continue;
				if (ClassifyAabbAgainstFrustumPlanes(planes, objectData.BoundsMin, objectData.BoundsMax) != EFrustumAabbRelation::Outside)
					threadVisible.push_back(objectIndex);
			}
		});
		frustumCullTask.m_MinRange = 256u;
		g_TS.AddTaskSetToPipe(&frustumCullTask);
		g_TS.WaitforTask(&frustumCullTask);

		for (const std::vector<uint32_t>& threadVisible : GBufferSpatialThreadVisibleIndices)
		{
			GBufferVisibleObjectIndices.insert(
				GBufferVisibleObjectIndices.end(),
				threadVisible.begin(),
				threadVisible.end());
		}
	}
	else
	{
		for (uint32_t objectIndex : GBufferSpatialFrustumCandidateIndices)
		{
			if (objectIndex >= RenderWorld.SceneObjectCullingData.size())
				continue;
			const RenderWorldMirror::SceneObjectCullingRecord& objectData =
				RenderWorld.SceneObjectCullingData[objectIndex];
			if (!objectData.Visible || !objectData.HasBounds)
				continue;
			if (ClassifyAabbAgainstFrustumPlanes(planes, objectData.BoundsMin, objectData.BoundsMax) != EFrustumAabbRelation::Outside)
				GBufferVisibleObjectIndices.push_back(objectIndex);
		}
	}
	GBufferLastSpatialVisibleObjectCount = GBufferVisibleObjectIndices.size();
	GBufferLastFrustumCulledObjectCount =
		totalCullableObjectCount > GBufferVisibleObjectIndices.size() ?
		totalCullableObjectCount - GBufferVisibleObjectIndices.size() :
		0;

	if (bGBufferCullProfileEnabled)
	{
		gbufferCullProfile.Objects += totalCullableObjectCount;
		gbufferCullProfile.ObjectScans += GBufferSpatialFrustumCandidateIndices.size();
		gbufferCullProfile.FrustumTests +=
			static_cast<uint64_t>(RenderWorld.SceneObjectCullingCells.size()) +
			static_cast<uint64_t>(GBufferSpatialFrustumCandidateIndices.size());
		GBufferProfileAdd(true, gbufferCullProfile.ObjectScanMs, gbufferCullProfile.ObjectScanCount, objectScanProfileBegin);
		GBufferProfileAdd(true, gbufferCullProfile.TotalMs, gbufferCullProfile.TotalCount, totalProfileBegin);
	}
	return GBufferVisibleObjectIndices;
}

void Corona::PrepareGBufferCulling(uint32_t sceneObjectCount)
{
	GBufferLastTotalObjectCount = 0;
	GBufferLastVisibleObjectCount = 0;
	GBufferLastFrustumCulledObjectCount = 0;
	GBufferLastOcclusionCulledObjectCount = 0;
	GBufferLastStaticInstancedBatchCount = 0;
	GBufferLastStaticInstancedObjectCount = 0;
	GBufferLastStaticInstancedDrawCount = 0;
	GBufferLastBindlessObjectBatchCount = 0;
	GBufferLastBindlessObjectCount = 0;
	GBufferLastBindlessObjectDrawCount = 0;
	GBufferOcclusionQueryCount = 0;
	bGBufferOcclusionQueriesActive = false;

	if (IsVulkanCaptureSafeActive())
		return;

	if (!renderBackend || !renderBackend->GetCapabilities().SupportsGBufferOcclusionQueries || sceneObjectCount == 0)
		return;

	uint32_t capacityPerFrame = 256u;
	while (capacityPerFrame < sceneObjectCount + 32u)
		capacityPerFrame *= 2u;

	if (capacityPerFrame != GBufferOcclusionQueryCapacityPerFrame)
	{
		GBufferOcclusionQueryCapacityPerFrame = capacityPerFrame;
		SceneObjectCullingStates.assign(
			std::max<size_t>(1u, static_cast<size_t>(NextSceneObjectHandle)),
			SceneObjectCullingState{});
		renderBackend->InitializeOcclusionQueries(GBufferOcclusionQueryCapacityPerFrame * std::max<uint32_t>(1u, renderBackend->GetFrameCount()));
	}
	else if (SceneObjectCullingStates.size() <= static_cast<size_t>(NextSceneObjectHandle))
	{
		SceneObjectCullingStates.resize(static_cast<size_t>(NextSceneObjectHandle));
	}

	GBufferOcclusionFrameIndex = renderBackend->GetCurrentFrameIndex();
	bGBufferOcclusionQueriesActive = GBufferOcclusionQueryCapacityPerFrame > 0;
}

bool Corona::ShouldDrawSceneObjectInGBuffer(const SceneObject& object, const glm::vec3& boundsCenter, float boundsRadius)
{
	if (!bGBufferOcclusionQueriesActive || object.Handle == InvalidSceneObjectHandle)
		return true;

	if (SceneObjectCullingStates.size() <= static_cast<size_t>(object.Handle))
		SceneObjectCullingStates.resize(static_cast<size_t>(object.Handle) + 1u);
	SceneObjectCullingState& state = SceneObjectCullingStates[object.Handle];
	const float movementThreshold = std::max(4.0f, boundsRadius * 0.05f);
	const glm::vec3 boundsDelta = boundsCenter - state.LastBoundsCenter;
	const bool boundsChanged =
		!state.HasBounds ||
		glm::dot(boundsDelta, boundsDelta) > movementThreshold * movementThreshold ||
		std::abs(boundsRadius - state.LastBoundsRadius) > movementThreshold;
	if (boundsChanged)
	{
		state.LastVisible = true;
		state.HasPendingOcclusionQuery = false;
		state.LastTestFrame = 0;
		state.LastBoundsCenter = boundsCenter;
		state.LastBoundsRadius = boundsRadius;
		state.HasBounds = true;
	}

	if (state.HasPendingOcclusionQuery && state.LastQueryFrameIndex == GBufferOcclusionFrameIndex)
	{
		state.LastVisible = renderBackend->ReadOcclusionQueryValue(state.LastQueryIndex) != 0;
		state.HasPendingOcclusionQuery = false;
	}

	const uint64_t framesSinceTest =
		FrameCounter >= state.LastTestFrame ?
		static_cast<uint64_t>(FrameCounter) - state.LastTestFrame :
		kMaxGBufferOcclusionSkipFrames + 1u;
	if (!state.LastVisible && framesSinceTest <= kMaxGBufferOcclusionSkipFrames)
	{
		++GBufferLastOcclusionCulledObjectCount;
		return false;
	}

	return true;
}

uint32_t Corona::BeginGBufferOcclusionQuery(SceneObjectHandle handle)
{
	if (IsVulkanCaptureSafeActive())
		return std::numeric_limits<uint32_t>::max();

	if (!bGBufferOcclusionQueriesActive || handle == InvalidSceneObjectHandle || GBufferOcclusionQueryCount >= GBufferOcclusionQueryCapacityPerFrame)
		return std::numeric_limits<uint32_t>::max();

	const uint32_t queryIndex = GBufferOcclusionFrameIndex * GBufferOcclusionQueryCapacityPerFrame + GBufferOcclusionQueryCount;
	++GBufferOcclusionQueryCount;
	renderBackend->BeginOcclusionQuery(queryIndex);
	return queryIndex;
}

void Corona::EndGBufferOcclusionQuery(SceneObjectHandle handle, uint32_t queryIndex)
{
	if (queryIndex == std::numeric_limits<uint32_t>::max() || handle == InvalidSceneObjectHandle)
		return;

	renderBackend->EndOcclusionQuery(queryIndex);
	if (SceneObjectCullingStates.size() <= static_cast<size_t>(handle))
		SceneObjectCullingStates.resize(static_cast<size_t>(handle) + 1u);
	SceneObjectCullingState& state = SceneObjectCullingStates[handle];
	state.HasPendingOcclusionQuery = true;
	state.LastQueryIndex = queryIndex;
	state.LastQueryFrameIndex = GBufferOcclusionFrameIndex;
	state.LastTestFrame = FrameCounter;
}

void Corona::FinishGBufferCulling()
{
	if (IsVulkanCaptureSafeActive())
		return;

	if (!bGBufferOcclusionQueriesActive || GBufferOcclusionQueryCount == 0)
		return;

	const uint32_t firstQuery = GBufferOcclusionFrameIndex * GBufferOcclusionQueryCapacityPerFrame;
	renderBackend->ResolveOcclusionQueryRange(firstQuery, GBufferOcclusionQueryCount);
}

void Corona::GBufferPass()
{
	const bool bGBufferPassProfileEnabled = IsGBufferPassProfileEnabled();
	GBufferPassProfile& gbufferPassProfile = GetGBufferPassProfile();
	if (bGBufferPassProfileEnabled)
		gbufferPassProfile.BeginFrame(static_cast<uint64_t>(FrameCounter));
	GBufferProfileScope gbufferPassTotalScope(
		bGBufferPassProfileEnabled,
		gbufferPassProfile.TotalMs,
		gbufferPassProfile.TotalCount);
	const auto preTransitionProfileBegin =
		bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};

	ColorBufferWriteIndex = 1 - ColorBufferWriteIndex;
	//DepthBufferWriteIndex = 1 - DepthBufferWriteIndex;
	renderBackend->EmitGpuCrashMarker("GBufferPass");
	const bool bMobileDirectGBuffer =
		CORONA_PLATFORM_MOBILE &&
		RenderingMode == ERenderingMode::HYBRID;

	renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::ShaderRead, EResourceState::DepthWrite);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	GBufferProfileAdd(
		bGBufferPassProfileEnabled,
		gbufferPassProfile.PreTransitionMs,
		gbufferPassProfile.PreTransitionCount,
		preTransitionProfileBegin);

	const auto clearProfileBegin =
		bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	renderBackend->ClearRenderTarget(AlbedoBuffer.get(), clearColor);
	if (!bMobileDirectGBuffer)
		renderBackend->ClearRenderTarget(SpecularAlbedoBuffer.get(), clearColor);
	const float normalClearColor[] =
	{
		bMobileDirectGBuffer ? 0.5f : 0.0f,
		bMobileDirectGBuffer ? 0.45f : -0.1f,
		bMobileDirectGBuffer ? 0.5f : 0.0f,
		0.0f
	};
	renderBackend->ClearRenderTarget(NormalBuffers[ColorBufferWriteIndex].get(), normalClearColor);
	if (!bMobileDirectGBuffer)
		renderBackend->ClearRenderTarget(GeomNormalBuffers[ColorBufferWriteIndex].get(), normalClearColor);
	const float velocityClearColor[] = { 0.0f, 0.0f};
	renderBackend->ClearRenderTarget(VelocityBuffer.get(), velocityClearColor);
	const float roughnessClearColor[] = { 0.001f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearRenderTarget(RoughnessMetalicBuffer.get(), roughnessClearColor);

	renderBackend->ClearDepth(DepthBuffer.get(), 1.0f);
	if (!bMobileDirectGBuffer)
	{
		const float ujitteredDepthClearColor[] = { 1.0f, 1.0f, 1.0f, 1.0f};
		renderBackend->ClearRenderTarget(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), ujitteredDepthClearColor);
	}
	GBufferProfileAdd(
		bGBufferPassProfileEnabled,
		gbufferPassProfile.ClearMs,
		gbufferPassProfile.ClearCount,
		clearProfileBegin);

	const auto skinningProfileBegin =
		bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	renderBackend->BindDefaultDescriptorHeaps();
	DispatchSpineSkinningForRenderWorld();
	BeginGpuPassTiming(EGpuPass::SkeletalSkinning);
	DispatchSkeletalSkinningForRenderWorld();
	EndGpuPassTiming(EGpuPass::SkeletalSkinning);
	GBufferProfileAdd(
		bGBufferPassProfileEnabled,
		gbufferPassProfile.SkinningMs,
		gbufferPassProfile.SkinningCount,
		skinningProfileBegin);

	const auto targetSetupProfileBegin =
		bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
	if (bMobileDirectGBuffer)
	{
		Texture* renderTargets[] = {
			AlbedoBuffer.get(),
			NormalBuffers[ColorBufferWriteIndex].get(),
			VelocityBuffer.get(),
			RoughnessMetalicBuffer.get()
		};
		renderBackend->SetRenderTargets(renderTargets, static_cast<uint32_t>(std::size(renderTargets)), DepthBuffer.get());
	}
	else
	{
		Texture* renderTargets[] = {
			AlbedoBuffer.get(),
			SpecularAlbedoBuffer.get(),
			NormalBuffers[ColorBufferWriteIndex].get(),
			GeomNormalBuffers[ColorBufferWriteIndex].get(),
			VelocityBuffer.get(),
			RoughnessMetalicBuffer.get(),
			UnjitteredDepthBuffers[ColorBufferWriteIndex].get()
		};
		renderBackend->SetRenderTargets(renderTargets, static_cast<uint32_t>(std::size(renderTargets)), DepthBuffer.get());
	}

	renderBackend->BindGraphicsPipeline(GBufferGraphicsPipeline.get());
	GBufferProfileAdd(
		bGBufferPassProfileEnabled,
		gbufferPassProfile.TargetSetupMs,
		gbufferPassProfile.TargetSetupCount,
		targetSetupProfileBegin);

	const auto prepareCullProfileBegin =
		bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	PrepareGBufferCulling(static_cast<uint32_t>(RenderWorld.SceneObjects.size()));
	GBufferProfileAdd(
		bGBufferPassProfileEnabled,
		gbufferPassProfile.PrepareCullMs,
		gbufferPassProfile.PrepareCullCount,
		prepareCullProfileBegin);
	const auto gatherVisibleProfileBegin =
		bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	const std::vector<uint32_t>& visibleObjectIndices = GatherGBufferVisibleObjectIndices();
	GBufferProfileAdd(
		bGBufferPassProfileEnabled,
		gbufferPassProfile.GatherVisibleMs,
		gbufferPassProfile.GatherVisibleCount,
		gatherVisibleProfileBegin);

	// Phase D (desktop only): if the cluster PSO is live AND we're in the
	// VS-inline skinning mode, draw all skeletal characters with a single
	// DrawIndexedInstanced (SV_InstanceID picks the per-char world matrix
	// from SkeletalInstanceTransforms). Mobile leaves
	// SkeletalVsInlineClusterGraphicsPipeline null (Adreno couldn't
	// compile the SV_InstanceID + SBV variant) and falls back to the
	// per-mesh DrawScene path below.
	const bool bClusterDrawActive =
		bSkeletalUseVsInlineSkinning &&
		SkeletalVsInlineClusterGraphicsPipeline &&
		SkeletalUnifiedCharCount > 0;
	const auto clusterDrawProfileBegin =
		bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	if (bClusterDrawActive)
	{
		UpdateSkeletalUnifiedInstanceTransforms();
		if (!DrawSkeletalVsInlineClusterDesktop())
		{
			// Fall back to per-mesh path this frame if the draw bailed.
		}
		renderBackend->BindGraphicsPipeline(GBufferGraphicsPipeline.get());
	}
	GBufferProfileAdd(
		bGBufferPassProfileEnabled,
		gbufferPassProfile.ClusterDrawMs,
		gbufferPassProfile.ClusterDrawCount,
		clusterDrawProfileBegin);

	static thread_local std::unordered_map<const Scene*, bool> s_skeletalUnifiedSceneCache;
	std::unordered_map<const Scene*, bool>& skeletalUnifiedSceneCache = s_skeletalUnifiedSceneCache;
	skeletalUnifiedSceneCache.clear();
	skeletalUnifiedSceneCache.reserve(64);
	auto isSkeletalUnifiedObject = [bClusterDrawActive, &skeletalUnifiedSceneCache](const std::shared_ptr<Scene>& scene)
	{
		if (!bClusterDrawActive || !scene)
			return false;
		const Scene* key = scene.get();
		auto it = skeletalUnifiedSceneCache.find(key);
		if (it != skeletalUnifiedSceneCache.end())
			return it->second;
		bool eligible = false;
		for (const auto& mesh : scene->meshes)
		{
			if (mesh && mesh->bSkeletalSkinned)
			{
				eligible = true;
				break;
			}
		}
		skeletalUnifiedSceneCache[key] = eligible;
		return eligible;
	};

	if (!bMultiThreadRendering)
	{
		const auto classifyProfileBegin =
			bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
		static thread_local std::unordered_map<const Scene*, bool> s_spineMeshSceneCache;
		std::unordered_map<const Scene*, bool>& spineMeshSceneCache = s_spineMeshSceneCache;
		spineMeshSceneCache.clear();
		spineMeshSceneCache.reserve(128);
		auto sceneUsesSpineMesh = [&spineMeshSceneCache](const std::shared_ptr<Scene>& scene)
		{
			if (!scene)
				return false;
			const Scene* key = scene.get();
			auto it = spineMeshSceneCache.find(key);
			if (it != spineMeshSceneCache.end())
				return it->second;
			bool usesSpine = false;
			for (const auto& mesh : scene->meshes)
			{
				if (mesh && mesh->bSpineMesh)
				{
					usesSpine = true;
					break;
				}
			}
			spineMeshSceneCache[key] = usesSpine;
			return usesSpine;
		};

		struct StaticBatchObjectEntry
		{
			const SceneObject* Object = nullptr;
			bool HasBounds = false;
			glm::vec3 BoundsCenter = glm::vec3(0.0f);
			float BoundsRadius = 0.0f;
		};
		struct StaticBatch
		{
			std::shared_ptr<Scene> ScenePtr;
			float Roughness = 1.0f;
			float Metallic = 0.0f;
			bool OverrideRoughnessMetallic = false;
			std::vector<StaticBatchObjectEntry> Objects;
		};
		struct VisibleGBufferObjectEntry
		{
			uint32_t ObjectIndex = std::numeric_limits<uint32_t>::max();
			const SceneObject* Object = nullptr;
			GBufferStaticBatchKey StaticBatchKey;
			bool SpineObject = false;
			bool SkeletalUnifiedObject = false;
			bool TerrainScene = false;
			bool ProceduralGrassScene = false;
			bool LegacyGrassScene = false;
			bool StaticInstancingEligible = false;
			bool StaticObjectBatchEligible = false;
			bool HasBounds = false;
			glm::vec3 BoundsCenter = glm::vec3(0.0f);
			float BoundsRadius = 0.0f;
		};
		constexpr size_t kMinStaticGBufferInstanceCount = 32;
		auto makeStaticBatchKey = [](const SceneObject& object)
		{
			GBufferStaticBatchKey key;
			key.ScenePtr = object.ScenePtr.get();
			key.Roughness = object.Roughness;
			key.Metallic = object.Metallic;
			key.OverrideRoughnessMetallic = object.bOverrideRoughnessMetallic;
			return key;
		};
		auto markObjectDrawnWithoutOcclusionQuery =
			[this](const SceneObject& object, bool hasBounds, const glm::vec3& boundsCenter, float boundsRadius)
		{
			if (!bGBufferOcclusionQueriesActive || object.Handle == InvalidSceneObjectHandle)
				return;
			if (static_cast<size_t>(object.Handle) >= SceneObjectCullingStates.size())
				return;
			SceneObjectCullingState& state = SceneObjectCullingStates[object.Handle];
			if (hasBounds)
			{
				state.LastBoundsCenter = boundsCenter;
				state.LastBoundsRadius = boundsRadius;
				state.HasBounds = true;
			}
		};
		auto shouldRefreshBatchedOcclusionQuery =
			[this](const SceneObject& object)
		{
			if (!bGBufferOcclusionQueriesActive || object.Handle == InvalidSceneObjectHandle)
				return false;
			const uint32_t batchedQueryBudget =
				std::min(GBufferOcclusionQueryCapacityPerFrame, kBatchedGBufferOcclusionRefreshQueryBudget);
			if (GBufferOcclusionQueryCount >= batchedQueryBudget)
				return false;

			if (static_cast<size_t>(object.Handle) >= SceneObjectCullingStates.size())
				return true;

			const SceneObjectCullingState& state = SceneObjectCullingStates[object.Handle];
			if (state.HasPendingOcclusionQuery)
				return false;
			if (state.LastTestFrame == 0)
				return true;

			const uint64_t framesSinceTest =
				FrameCounter >= state.LastTestFrame ?
				static_cast<uint64_t>(FrameCounter) - state.LastTestFrame :
				kBatchedGBufferOcclusionRefreshFrames + 1u;
			if (!state.LastVisible)
				return framesSinceTest > kMaxGBufferOcclusionSkipFrames;
			return framesSinceTest >= kBatchedGBufferOcclusionRefreshFrames;
		};
		static thread_local std::unordered_map<const Scene*, bool> s_staticInstancingEligibilityCache;
		std::unordered_map<const Scene*, bool>& staticInstancingEligibilityCache = s_staticInstancingEligibilityCache;
		staticInstancingEligibilityCache.clear();
		staticInstancingEligibilityCache.reserve(128);
		auto isStaticInstancingEligibleCached = [this, &staticInstancingEligibilityCache](const std::shared_ptr<Scene>& scene)
		{
			if (!scene)
				return false;
			const Scene* key = scene.get();
			auto it = staticInstancingEligibilityCache.find(key);
			if (it != staticInstancingEligibilityCache.end())
				return it->second;
			const bool eligible = IsSceneEligibleForStaticGBufferInstancing(scene);
			staticInstancingEligibilityCache[key] = eligible;
			return eligible;
		};
		const RenderBackendCapabilities gbufferCapabilities =
			renderBackend ? renderBackend->GetCapabilities() : RenderBackendCapabilities{};
		const bool bStaticObjectBatchSupported =
			!ShouldDisableNriGBufferObjectBatch(renderBackend.get()) &&
			SupportsGBufferBindlessMaterials(renderBackend.get()) &&
			SupportsGBufferBindlessGeometry(renderBackend.get()) &&
			gbufferCapabilities.SupportsDrawIndirect &&
			gbufferCapabilities.SupportsDrawIndirectFirstInstance;
		static thread_local std::unordered_map<const Scene*, bool> s_staticObjectBatchEligibilityCache;
		std::unordered_map<const Scene*, bool>& staticObjectBatchEligibilityCache = s_staticObjectBatchEligibilityCache;
		staticObjectBatchEligibilityCache.clear();
		staticObjectBatchEligibilityCache.reserve(128);
		auto isStaticObjectBatchEligibleCached = [bStaticObjectBatchSupported, &staticObjectBatchEligibilityCache](const std::shared_ptr<Scene>& scene)
		{
			if (!bStaticObjectBatchSupported)
				return false;
			if (!scene)
				return false;
			const Scene* key = scene.get();
			auto it = staticObjectBatchEligibilityCache.find(key);
			if (it != staticObjectBatchEligibilityCache.end())
				return it->second;

			bool eligible = !scene->meshes.empty();
			bool hasDrawableRange = false;
			if (eligible)
			{
				for (const std::shared_ptr<Mesh>& mesh : scene->meshes)
				{
					if (!mesh || !mesh->Vb || !mesh->Ib || mesh->Draws.empty() ||
						mesh->bTerrainMesh || mesh->bGrassMesh ||
						!IsGBufferStaticBindlessGeometryEligible(*mesh))
					{
						eligible = false;
						break;
					}
					for (const Mesh::DrawCall& drawcall : mesh->Draws)
					{
						if (drawcall.IndexCount != 0)
							hasDrawableRange = true;
					}
				}
			}
			eligible = eligible && hasDrawableRange;
			staticObjectBatchEligibilityCache[key] = eligible;
			return eligible;
		};
		static thread_local std::unordered_map<const Scene*, bool> s_proceduralGrassSceneCache;
		std::unordered_map<const Scene*, bool>& proceduralGrassSceneCache = s_proceduralGrassSceneCache;
		proceduralGrassSceneCache.clear();
		proceduralGrassSceneCache.reserve(128);
		auto sceneHasProceduralGrass = [&proceduralGrassSceneCache](const std::shared_ptr<Scene>& scene)
		{
			if (!scene)
				return false;
			const Scene* key = scene.get();
			auto it = proceduralGrassSceneCache.find(key);
			if (it != proceduralGrassSceneCache.end())
				return it->second;
			bool hasProceduralGrass = false;
			for (const std::shared_ptr<Mesh>& sceneMesh : scene->meshes)
			{
				if (sceneMesh && sceneMesh->bProceduralGrass)
				{
					hasProceduralGrass = true;
					break;
				}
			}
			proceduralGrassSceneCache[key] = hasProceduralGrass;
			return hasProceduralGrass;
		};
		const std::shared_ptr<Scene> activeTerrainScene =
			ActiveTerrain ? ActiveTerrain->GetScene() : std::shared_ptr<Scene>{};
		const std::shared_ptr<Scene> activeGrassScene = ActiveGrassScene.lock();
		static thread_local std::unordered_map<GBufferStaticBatchKey, uint32_t, GBufferStaticBatchKeyHash> s_staticInstancingCandidateCounts;
		std::unordered_map<GBufferStaticBatchKey, uint32_t, GBufferStaticBatchKeyHash>& staticInstancingCandidateCounts =
			s_staticInstancingCandidateCounts;
		staticInstancingCandidateCounts.clear();
		staticInstancingCandidateCounts.reserve(std::min<size_t>(visibleObjectIndices.size(), 1024u));
		static thread_local std::vector<VisibleGBufferObjectEntry> s_visibleGBufferEntries;
		std::vector<VisibleGBufferObjectEntry>& visibleGBufferEntries = s_visibleGBufferEntries;
		visibleGBufferEntries.clear();
		visibleGBufferEntries.reserve(visibleObjectIndices.size());
		static thread_local std::vector<uint32_t> s_prebatchedBindlessObjectIndices;
		std::vector<uint32_t>& prebatchedBindlessObjectIndices = s_prebatchedBindlessObjectIndices;
		prebatchedBindlessObjectIndices.clear();
		prebatchedBindlessObjectIndices.reserve(visibleObjectIndices.size());
		const uint32_t prebatchedRefreshBudget =
			std::min(GBufferOcclusionQueryCapacityPerFrame, kBatchedGBufferOcclusionRefreshQueryBudget);
		uint32_t prebatchedRefreshCandidates = 0;
		bool bHasSpineObjects = false;
		for (uint32_t objectIndex : visibleObjectIndices)
		{
			if (objectIndex >= RenderWorld.SceneObjects.size())
				continue;
			const SceneObject& object = RenderWorld.SceneObjects[objectIndex];
			if (!object.bVisible || !object.ScenePtr)
				continue;

			VisibleGBufferObjectEntry entry{};
			entry.ObjectIndex = objectIndex;
			entry.Object = &object;
			entry.SpineObject = sceneUsesSpineMesh(object.ScenePtr);
			entry.SkeletalUnifiedObject = isSkeletalUnifiedObject(object.ScenePtr);
			entry.TerrainScene = activeTerrainScene && object.ScenePtr == activeTerrainScene;
			entry.ProceduralGrassScene = sceneHasProceduralGrass(object.ScenePtr);
			entry.LegacyGrassScene =
				!entry.ProceduralGrassScene && activeGrassScene && object.ScenePtr == activeGrassScene;
			entry.StaticObjectBatchEligible =
				!entry.SpineObject &&
				!entry.SkeletalUnifiedObject &&
				!entry.TerrainScene &&
				!entry.ProceduralGrassScene &&
				!entry.LegacyGrassScene &&
				isStaticObjectBatchEligibleCached(object.ScenePtr);
			const bool bOriginallyStaticObjectBatchEligible = entry.StaticObjectBatchEligible;
			if (objectIndex < RenderWorld.SceneObjectCullingData.size())
			{
				const RenderWorldMirror::SceneObjectCullingRecord& objectData =
					RenderWorld.SceneObjectCullingData[objectIndex];
				entry.HasBounds = objectData.HasBounds;
				entry.BoundsCenter = objectData.BoundsCenter;
				entry.BoundsRadius = objectData.BoundsRadius;
			}

			if (entry.StaticObjectBatchEligible)
			{
				if (entry.HasBounds)
				{
					if (!ShouldDrawSceneObjectInGBuffer(object, entry.BoundsCenter, entry.BoundsRadius))
						continue;
					const bool bRefreshBatchedObject =
						prebatchedRefreshCandidates < prebatchedRefreshBudget &&
						shouldRefreshBatchedOcclusionQuery(object);
					if (bRefreshBatchedObject)
					{
						++prebatchedRefreshCandidates;
						entry.StaticObjectBatchEligible = false;
					}
					else
					{
						prebatchedBindlessObjectIndices.push_back(objectIndex);
						continue;
					}
				}
				else
				{
					prebatchedBindlessObjectIndices.push_back(objectIndex);
					continue;
				}
			}

			entry.StaticInstancingEligible =
				!entry.SpineObject &&
				!entry.SkeletalUnifiedObject &&
				!entry.TerrainScene &&
				!entry.ProceduralGrassScene &&
				!entry.LegacyGrassScene &&
				!bOriginallyStaticObjectBatchEligible &&
				isStaticInstancingEligibleCached(object.ScenePtr);
			if (entry.StaticInstancingEligible)
				entry.StaticBatchKey = makeStaticBatchKey(object);

			if (entry.SpineObject)
				bHasSpineObjects = true;
			if (entry.StaticInstancingEligible)
				++staticInstancingCandidateCounts[entry.StaticBatchKey];
			visibleGBufferEntries.push_back(entry);
		}
		GBufferProfileAdd(
			bGBufferPassProfileEnabled,
			gbufferPassProfile.ClassifyMs,
			gbufferPassProfile.ClassifyCount,
			classifyProfileBegin);

		const auto drawSubmitProfileBegin =
			bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
		const int drawSpinePassCount = bHasSpineObjects ? 2 : 1;
		for (int drawSpinePass = 0; drawSpinePass < drawSpinePassCount; ++drawSpinePass)
		{
			static thread_local std::unordered_map<GBufferStaticBatchKey, StaticBatch, GBufferStaticBatchKeyHash> s_staticBatches;
			std::unordered_map<GBufferStaticBatchKey, StaticBatch, GBufferStaticBatchKeyHash>& staticBatches = s_staticBatches;
			staticBatches.clear();
			staticBatches.reserve(staticInstancingCandidateCounts.size());
			static thread_local std::vector<const SceneObject*> s_staticBatchObjectPtrs;
			std::vector<const SceneObject*>& staticBatchObjectPtrs = s_staticBatchObjectPtrs;
			staticBatchObjectPtrs.clear();
			for (const VisibleGBufferObjectEntry& entry : visibleGBufferEntries)
			{
				if (!entry.Object)
					continue;
				const SceneObject& object = *entry.Object;
				const uint32_t objectIndex = entry.ObjectIndex;
				if ((drawSpinePass == 0 && entry.SpineObject) || (drawSpinePass == 1 && !entry.SpineObject))
					continue;

				if (entry.SkeletalUnifiedObject)
					continue;

				const bool bTerrainScene = entry.TerrainScene;
				// Procedural-grass scenes route through the vertex-pulling PSO
				// regardless of whether they're the active terrain-anchored
				// grass (script-spawned) or a toolbox-spawned grass entity, so
				// detect it from the mesh flag rather than the active-scene
				// pointer. Legacy VB-backed grass keeps the original Grass
				// bucket; procedural lands in its own bucket so the overlay
				// can show their costs separately.
				const bool bProceduralGrassScene = entry.ProceduralGrassScene;
				const bool bLegacyGrassScene = entry.LegacyGrassScene;
				uint32_t staticCandidateCount = 0u;
				if (entry.StaticInstancingEligible)
				{
					const auto staticCandidateCountIt = staticInstancingCandidateCounts.find(entry.StaticBatchKey);
					staticCandidateCount =
						staticCandidateCountIt != staticInstancingCandidateCounts.end() ? staticCandidateCountIt->second : 0u;
				}
				bool bStaticInstancingCandidate =
					drawSpinePass == 0 &&
					entry.StaticInstancingEligible &&
					staticCandidateCount >= kMinStaticGBufferInstanceCount;
				bool bDrawsWithoutOcclusionQuery =
					bStaticInstancingCandidate;

				const glm::vec3 boundsCenter = entry.BoundsCenter;
				const float boundsRadius = entry.BoundsRadius;
				const bool bHasBounds = entry.HasBounds;
				if (bHasBounds)
				{
					if (!ShouldDrawSceneObjectInGBuffer(object, boundsCenter, boundsRadius))
						continue;
					if (bDrawsWithoutOcclusionQuery &&
						shouldRefreshBatchedOcclusionQuery(object))
					{
						bStaticInstancingCandidate = false;
						bDrawsWithoutOcclusionQuery = false;
					}
				}

				if (bStaticInstancingCandidate)
				{
					StaticBatch& batch = staticBatches[entry.StaticBatchKey];
					if (!batch.ScenePtr)
					{
						batch.ScenePtr = object.ScenePtr;
						batch.Roughness = object.Roughness;
						batch.Metallic = object.Metallic;
						batch.OverrideRoughnessMetallic = object.bOverrideRoughnessMetallic;
					}
					batch.Objects.push_back({ &object, bHasBounds, boundsCenter, boundsRadius });
					continue;
				}

				const uint32_t occlusionQueryIndex = BeginGBufferOcclusionQuery(object.Handle);
				if (bTerrainScene)
					BeginGpuPassTiming(EGpuPass::Terrain);
				else if (bProceduralGrassScene)
					BeginGpuPassTiming(EGpuPass::ProceduralGrass);
				else if (bLegacyGrassScene)
					BeginGpuPassTiming(EGpuPass::Grass);
				DrawScene(
					object.ScenePtr,
					object.Transform,
					object.Roughness,
					object.Metallic,
					object.bOverrideRoughnessMetallic);
				if (bTerrainScene)
					EndGpuPassTiming(EGpuPass::Terrain);
				else if (bProceduralGrassScene)
					EndGpuPassTiming(EGpuPass::ProceduralGrass);
				else if (bLegacyGrassScene)
					EndGpuPassTiming(EGpuPass::Grass);
				EndGBufferOcclusionQuery(object.Handle, occlusionQueryIndex);
				++GBufferLastVisibleObjectCount;
			}

			if (drawSpinePass == 0 && !prebatchedBindlessObjectIndices.empty())
			{
				if (DrawStaticObjectBindlessBatch(prebatchedBindlessObjectIndices))
				{
					GBufferLastVisibleObjectCount += prebatchedBindlessObjectIndices.size();
				}
				else
				{
					static bool bLoggedBindlessObjectBatchFailure = false;
					if (!bLoggedBindlessObjectBatchFailure)
					{
						AppendCpuRuntimeTrace(L"[GBufferObjectBatch] required bindless indirect batch failed; no fallback draw submitted");
						bLoggedBindlessObjectBatchFailure = true;
					}
				}
			}

			for (auto& batchPair : staticBatches)
			{
				StaticBatch& batch = batchPair.second;
				if (!batch.ScenePtr || batch.Objects.empty())
					continue;
				staticBatchObjectPtrs.clear();
				staticBatchObjectPtrs.reserve(batch.Objects.size());
				for (const StaticBatchObjectEntry& entry : batch.Objects)
				{
					if (entry.Object)
						staticBatchObjectPtrs.push_back(entry.Object);
				}
				if (staticBatchObjectPtrs.size() >= kMinStaticGBufferInstanceCount &&
					DrawStaticInstancedScene(
						batch.ScenePtr,
						staticBatchObjectPtrs,
						batch.Roughness,
						batch.Metallic,
						batch.OverrideRoughnessMetallic))
				{
					for (const StaticBatchObjectEntry& entry : batch.Objects)
					{
						if (!entry.Object)
							continue;
						markObjectDrawnWithoutOcclusionQuery(
							*entry.Object,
							entry.HasBounds,
							entry.BoundsCenter,
							entry.BoundsRadius);
						++GBufferLastVisibleObjectCount;
					}
					continue;
				}
				for (const StaticBatchObjectEntry& entry : batch.Objects)
				{
					const SceneObject* object = entry.Object;
					if (!object)
						continue;
					const uint32_t occlusionQueryIndex = BeginGBufferOcclusionQuery(object->Handle);
					DrawScene(
						object->ScenePtr,
						object->Transform,
						object->Roughness,
						object->Metallic,
						object->bOverrideRoughnessMetallic);
					EndGBufferOcclusionQuery(object->Handle, occlusionQueryIndex);
					++GBufferLastVisibleObjectCount;
				}
			}
		}
		GBufferProfileAdd(
			bGBufferPassProfileEnabled,
			gbufferPassProfile.DrawSubmitMs,
			gbufferPassProfile.DrawSubmitCount,
			drawSubmitProfileBegin);
	}
	else
	{
		//UINT RemainDraw = mesh->Draws.size();
		//UINT NumDrawThread = mesh->Draws.size() / (NumThread);
		//UINT StartIndex = 0;

		//vector<ThreadDescriptorHeapPool> vecDHPool;
		//vecDHPool.resize(NumThread);

		//vector<ParallelDrawTaskSet> vecTask;
		//vecTask.resize(NumThread);

		//for (int i = 0; i < NumThread; i++)
		//{
		//	UINT ThisDraw = NumDrawThread;
		//	
		//	if (i == NumThread - 1)
		//		ThisDraw = RemainDraw;

		//	ThreadDescriptorHeapPool& DHPool = vecDHPool[i];
		//	DHPool.AllocPool(RS_Mesh->GetGraphicsBindingDHSize()*ThisDraw);

		//	// draw
		//	ParallelDrawTaskSet& task = vecTask[i];
		//	task.app = this;
		//	task.StartIndex = StartIndex;
		//	task.ThisDraw = ThisDraw;
		//	task.ThreadIndex = i;
		//	task.DHPool = &DHPool;

		//	g_TS.AddTaskSetToPipe(&task);

		//	RemainDraw -= ThisDraw;
		//	StartIndex += ThisDraw;
		//}

		//g_TS.WaitforAll();
	}

	const auto finishCullProfileBegin =
		bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	FinishGBufferCulling();
	GBufferProfileAdd(
		bGBufferPassProfileEnabled,
		gbufferPassProfile.FinishCullMs,
		gbufferPassProfile.FinishCullCount,
		finishCullProfileBegin);

	if (bGBufferPassProfileEnabled)
	{
		gbufferPassProfile.TotalObjects += GBufferLastTotalObjectCount;
		gbufferPassProfile.VisibleObjects += GBufferLastVisibleObjectCount;
		gbufferPassProfile.FrustumCulledObjects += GBufferLastFrustumCulledObjectCount;
		gbufferPassProfile.OcclusionCulledObjects += GBufferLastOcclusionCulledObjectCount;
		gbufferPassProfile.SpatialCandidateObjects += GBufferLastSpatialCandidateObjectCount;
		gbufferPassProfile.BindlessDraws += GBufferLastBindlessObjectDrawCount;
		gbufferPassProfile.StaticDraws += GBufferLastStaticInstancedDrawCount;
		gbufferPassProfile.Queries += GBufferOcclusionQueryCount;
	}

	if ((FrameCounter % 120u) == 0u &&
		(GBufferLastTotalObjectCount != 0 ||
		 GBufferLastVisibleObjectCount != 0 ||
		 GBufferLastFrustumCulledObjectCount != 0 ||
		 GBufferLastOcclusionCulledObjectCount != 0))
	{
		AppendCpuRuntimeTrace(
			L"[GBufferCulling] rendered=" + std::to_wstring(GBufferLastVisibleObjectCount) +
			L"/" + std::to_wstring(GBufferLastTotalObjectCount) +
			L", frustum=" + std::to_wstring(GBufferLastFrustumCulledObjectCount) +
			L", occlusion=" + std::to_wstring(GBufferLastOcclusionCulledObjectCount) +
			L", queries=" + std::to_wstring(GBufferOcclusionQueryCount) +
			L", staticBatches=" + std::to_wstring(GBufferLastStaticInstancedBatchCount) +
			L", staticObjects=" + std::to_wstring(GBufferLastStaticInstancedObjectCount) +
			L", staticDraws=" + std::to_wstring(GBufferLastStaticInstancedDrawCount) +
			L", bindlessBatches=" + std::to_wstring(GBufferLastBindlessObjectBatchCount) +
			L", bindlessObjects=" + std::to_wstring(GBufferLastBindlessObjectCount) +
			L", bindlessDraws=" + std::to_wstring(GBufferLastBindlessObjectDrawCount) +
			L", cells=" + std::to_wstring(GBufferLastSpatialVisibleCellCount) +
			L"/" + std::to_wstring(GBufferLastSpatialCellCount) +
			L", partialCells=" + std::to_wstring(GBufferLastSpatialPartialCellCount) +
			L", spatialCandidates=" + std::to_wstring(GBufferLastSpatialCandidateObjectCount) +
			L", spatialVisible=" + std::to_wstring(GBufferLastSpatialVisibleObjectCount) +
			L", active=" + std::to_wstring(bGBufferOcclusionQueriesActive ? 1 : 0));
	}

	const auto postTransitionProfileBegin =
		bGBufferPassProfileEnabled ? GBufferProfileClock::now() : GBufferProfileClock::time_point{};
	renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);


	renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::DepthWrite, EResourceState::ShaderRead);
	if (!bMobileDirectGBuffer)
		renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	GBufferProfileAdd(
		bGBufferPassProfileEnabled,
		gbufferPassProfile.PostTransitionMs,
		gbufferPassProfile.PostTransitionCount,
		postTransitionProfileBegin);
}
