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

#pragma once
#define GLM_FORCE_CTOR_INIT
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "glm/glm.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/transform.hpp"
#include "glm/mat4x4.hpp"
#include "glm/fwd.hpp"
#include "glm/gtc/quaternion.hpp"

#include "StepTimer.h"
#include "SimpleCamera.h"
#include "RHIBuildConfig.h"
#include "RenderBackend.h"
#include "RenderResources.h"
#if CORONA_HAS_D3D12
#include "DX12Backend.h"
#endif
#include "EntityComponentSystem.h"
#include "enkiTS/TaskScheduler.h"
#if CORONA_HAS_PIX
#define PROFILE_BUILD 1
#include "pix3.h"
#endif
#ifndef WITH_STREAMLINE
#define WITH_STREAMLINE 0
#endif
#if WITH_STREAMLINE
#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss.h"
#include "sl_dlss_d.h"
#endif

#if CORONA_PLATFORM_IS_WINDOWS
using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().
using Microsoft::WRL::ComPtr;
#endif
using namespace std;

struct lua_State;
struct lua_Callbacks;
struct PlatformTouchState;

enum class ERawFloatDumpFormat
{
	Unknown,
	R32Float,
	R32G32Float,
	R32G32B32A32Float,
};

class Corona
{
public:
	enum class ERenderingMode
	{
		HYBRID,		// Rasterization GBuffer + Raytracing
		PATHTRACING	// Full path tracing
	};

	enum class EDebugVisualization
	{
		SHADOW,
		WORLD_NORMAL,
		GEO_NORMAL,
		DEPTH,
		RAW_DIFFUSE_GI,
		RAW_DIFFUSE_GI_AUX,
		SCREEN_PROBE_DIFFUSE_GI,
		SCREEN_PROBE_PROBES,
		SCREEN_PROBE_HISTORY_LENGTH,
		SCREEN_PROBE_ATLAS_HISTORY_LENGTH,
		TEMPORAL_FILTERED_DIFFUSE_GI,
		RESOLVED_DIFFUSE_GI,
		FINAL_DIFFUSE_GI,
		ALBEDO,
		VELOCITY,
		ROUGNESS_METALLIC,
		SPECULAR_RAW,
		TEMPORAL_FILTERED_SPECULAR,
		BLOOM,
		SPEC_HISTORY_LENGTH,
		RTAO,
		NO_FULLSCREEN,
	};

	ERenderingMode RenderingMode = ERenderingMode::HYBRID;
	EDebugVisualization FullscreenDebugBuffer = EDebugVisualization::NO_FULLSCREEN;
private:
	enum class EGpuPass : UINT32
	{
		Frame = 0,
		GBuffer,
		RaytraceShadow,
		RaytraceAO,
		RaytraceSkyLighting,
		RaytraceReflection,
		RaytraceGI,
		ScreenProbeGI,
		TemporalDenoise,
		Lighting,
		DLSSRR,
		DLSSSR,
		TemporalAA,
		PathTracing,
		ToneMap,
		Debug,
		ImGui,
		Count
	};

	enum class ECpuUpdatePhase : UINT32
	{
		CameraPhysics = 0,
		Input,
		LuauScripts,
		CameraPath,
		RenderSync,
		Count
	};

	enum class ERenderCommandPhase : UINT32
	{
		SceneFlush = 0,
		GpuTimingReadback,
		FrameSetup,
		RenderPasses,
		BackbufferToneMap,
		CaptureUi,
		GpuTimingResolve,
		PresentTransition,
		Count
	};

	enum class ESceneFlushPhase : UINT32
	{
		UpdateGatherInstances = 0,
		UpdateGpuWait,
		UpdateTlas,
		UpdateInstanceProperties,
		RebuildGpuWait,
		RebuildGatherInstances,
		RebuildTlas,
		RebuildInstanceProperties,
		RebuildPipelineState,
		Count
	};

	static constexpr UINT32 GpuPassCount = static_cast<UINT32>(EGpuPass::Count);
	static constexpr UINT32 CpuUpdatePhaseCount = static_cast<UINT32>(ECpuUpdatePhase::Count);
	static constexpr UINT32 RenderCommandPhaseCount = static_cast<UINT32>(ERenderCommandPhase::Count);
	static constexpr UINT32 SceneFlushPhaseCount = static_cast<UINT32>(ESceneFlushPhase::Count);
	static constexpr UINT32 GpuQueriesPerPass = 2;
	static constexpr UINT32 MobileShadowMapResolution = 512;
	static constexpr UINT32 MobileShadowNearbyCasterCount = 10;
	static constexpr float MobileShadowFocusDistance = 320.0f;
	static constexpr float MobileShadowFocusRadius = 420.0f;
	static constexpr float MobileShadowMinCasterHeight = 24.0f;
	using CpuClock = std::chrono::steady_clock;

	shared_ptr<Texture> DepthBuffer;
	shared_ptr<Texture> UnjitteredDepthBuffers[2];

	UINT ColorBufferWriteIndex = 0;
	UINT ResolvedColorBufferIndex = 0;
	shared_ptr<Texture> ColorBuffers[2];
	shared_ptr<Texture> LightingBuffer;
	shared_ptr<Texture> DirectLightingBuffer;
	shared_ptr<Texture> DLSSRRBuffer;
	shared_ptr<Texture> AlbedoBuffer;
	shared_ptr<Texture> SpecularAlbedoBuffer;
	shared_ptr<Texture> NormalBuffers[2];
	shared_ptr<Texture> GeomNormalBuffers[2];
	shared_ptr<Texture> VelocityBuffer;
	shared_ptr<Texture> RoughnessMetalicBuffer;
	shared_ptr<Texture> PathTracingSpecularHitDistanceBuffer;
	shared_ptr<Texture> PathTracingSpecularMotionVectorBuffer;
	shared_ptr<Texture> ShadowBuffer;
	shared_ptr<Texture> AmbientOcclusionBuffer;
	shared_ptr<Texture> SkyLightingBuffer;
	glm::mat4x4 MobileShadowViewProjMat = glm::mat4x4(1.0f);
	bool bMobileShadowMapValidThisFrame = false;
	std::vector<uint32_t> MobileShadowCasterObjectIndices;

	shared_ptr<Texture> SpecularGIRaw;

	shared_ptr<Texture> SpecularGITemporal[2];

	shared_ptr<Texture> SpecularGIMoments[2];





	UINT GIBufferScale = 1;
	UINT GIBufferWriteIndex = 0;
	shared_ptr<Texture> DiffuseGITemporalAux[2];
	shared_ptr<Texture> DiffuseGITemporal[2];
	shared_ptr<Texture> DiffuseGIRawAux;
	shared_ptr<Texture> DiffuseGIRaw;
	shared_ptr<Texture> DiffuseGIHashCached;
	shared_ptr<Texture> DiffuseGIHashCachedAux;
	shared_ptr<Texture> ScreenProbeGIResolved;
	shared_ptr<Texture> ScreenProbeGIProbeDebug;
	shared_ptr<Texture> ScreenProbeGIRadiance[2];
	static constexpr UINT ScreenProbeSHCoefficientCount = 9;
	shared_ptr<Texture> ScreenProbeGISH[2][ScreenProbeSHCoefficientCount];
	shared_ptr<Texture> ScreenProbeGIMetadata[2];
	shared_ptr<Texture> ScreenProbeGIHistory[2];
	UINT ScreenProbeGIAtlasWriteIndex = 0;
	UINT ScreenProbeGIHistoryWriteIndex = 0;

	shared_ptr<Texture> BloomBlurPingPong[2];
	shared_ptr<Texture> LumaBuffer;
	std::shared_ptr<Buffer> Histogram;
	std::shared_ptr<Buffer> ExposureData;



	std::vector<std::shared_ptr<Texture>> framebuffers;
	
	// mesh draw pass
	struct GBufferConstantBuffer
	{
		glm::mat4x4 ViewProjectionMatrix;
		glm::mat4x4 PrevViewProjectionMatrix;
		glm::mat4x4 WorldMatrix;
		glm::mat4x4 UnjitteredViewProjMat;
		glm::mat4x4 PrevUnjitteredViewProjMat;
		glm::vec4 ViewDir;
		glm::vec4 BaseColorFactor = glm::vec4(1.0f);
		glm::vec2 RTSize;
		glm::vec2 RougnessMetalic;
		UINT32 bOverrideRougnessMetallic;
		UINT32 bTwoSidedLighting;
		UINT32 bUnlitMaterial;
		UINT32 SpineVertexBase = 0;
	};

	std::shared_ptr<GraphicsPipelineHandle> GBufferGraphicsPipeline;
	std::shared_ptr<GraphicsPipelineHandle> CpuSpineGBufferGraphicsPipeline;
	std::shared_ptr<GraphicsPipelineHandle> SpineGBufferGraphicsPipeline;
	struct SpineSkinningConstant
	{
		UINT32 VertexCount = 0;
		float SourceScale = 1.0f;
		glm::vec2 Padding = glm::vec2(0.0f);
	};
	shared_ptr<ComputePipelineStateObject> SpineSkinningPSO;

	// 3D skeletal skinning (Corona.Skeletal.*). Fully separate from the Spine
	// compute path above. The PSO consumes SkinInputVertex SBV + SkinBone SBV
	// and writes a standard-layout vertex buffer that the existing GBuffer /
	// shadow PSOs read through their normal IA input layout.
	struct SkeletalSkinningConstant
	{
		UINT32 VertexCount = 0;
		UINT32 BoneBase = 0;
		UINT32 Pad0 = 0;
		UINT32 Pad1 = 0;
	};
	shared_ptr<ComputePipelineStateObject> SkeletalSkinningPSO;

	struct SkeletalFrameStats
	{
		UINT32 CharactersAnimated = 0;
		UINT32 VerticesSkinned = 0;
		UINT32 BonesUploaded = 0;
		UINT32 DispatchCount = 0;
		UINT32 TransitionCount = 0;
		UINT32 BlasUpdates = 0;
		float AnimationEvalMs = 0.0f;
		float BoneUploadMs = 0.0f;
		float DispatchMs = 0.0f;
		float BlasMs = 0.0f;
	};
	SkeletalFrameStats SkeletalStats;

	// Spine sprite/skinning profiling counters (Phase 1 of the platformer
	// Spine optimization plan). All values accumulate across script-driven
	// Spine evaluations between DumpSpineFrameStatsToTrace() calls.
	struct SpineFrameStats
	{
		// Counters
		UINT32 InstancesEvaluated = 0;        // CreateSpineSceneForScript invocations
		UINT32 ScriptSceneCacheHits = 0;      // ScriptSceneByPath returned an existing scene
		UINT32 ClipCacheHits = 0;             // SpineClipFrameCache served pre-built CPU geometry
		UINT32 ClipCacheMisses = 0;           // BuildSpineSampleMesh actually ran
		UINT32 ClipCacheEvictions = 0;        // LRU evictions from SpineClipFrameCache
		UINT32 ClipCacheEntries = 0;          // Snapshot of current cache size at last report
		UINT64 ClipCacheBytes = 0;            // Snapshot of current cache bytes at last report
		UINT32 SkeletonInstancesBuilt = 0;    // spSkeleton_create calls (miss-path only)
		UINT32 SlotsProcessed = 0;            // Spine slots iterated by mesh build
		UINT32 VerticesGenerated = 0;         // CPU-skinned vertices written to a cache entry
		UINT32 IndicesGenerated = 0;
		UINT32 DrawRangesBuilt = 0;
		UINT32 RuntimeGpuBufferCreations = 0; // VB+IB created from a Spine animation update
		UINT32 RuntimeGpuUploadStalls = 0;    // immediate uploads that can stall the queue
		UINT32 PrewarmCallsHit = 0;
		UINT32 PrewarmCallsMiss = 0;
		// Frame-time breakdown (cumulative milliseconds since last reset)
		double AtlasLoadMs = 0.0;
		double SkeletonReadMs = 0.0;
		double AnimationEvaluationMs = 0.0;
		double SkinningMs = 0.0;
		double MeshBuildMs = 0.0;
		double GpuUploadMs = 0.0;
		void Reset();
	};
	SpineFrameStats SpineStats;
	SpineFrameStats SpineStatsLastReport;
	UINT32 SpineStatsCallsSinceReport = 0;
	UINT32 SpineStatsReportIntervalCalls = 64;
	void ResetSpineFrameStats();
	void DumpSpineFrameStatsToTrace(const wchar_t* reasonTag);

	// temporal denoising
	struct TemporalFilterConstant
	{
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::mat4x4 PrevUnjitteredViewProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4 TemporalValidParams = glm::vec4(28, 0, 0, 0);
		glm::vec2 RTSize;
		UINT32 FrameIndex;
		float BayerRotScale = 0.1;
		float SpecularBlurRadius = 4;
		float Point2PlaneDistScale = 10.0f;
		float AccumulationAlpha = 1.0f;
		UINT32 HistoryValid = 0;
		glm::vec2 JitterOffset = glm::vec2(0.0f);
		float SpecularAccumulationAlpha = 1.0f;
		float SpecularVarianceClipGamma = 1.75f;
	};

	TemporalFilterConstant TemporalFilterCB;

	shared_ptr<ComputePipelineStateObject> TemporalDenoisingFilterPSO;

	// Screen-probe diffuse GI resolve
	struct ScreenProbeGIConstant
	{
		glm::vec4 ProjectionParams;
		glm::vec2 RTSize;
		glm::vec2 ProbeGridSize;
		UINT32 ProbeSpacing = 8;
		UINT32 GatherRadius = 3;
		float ProbeDepthWeight = 8.0f;
		float ProbeNormalWeight = 8.0f;
		float ResolveDepthWeight = 24.0f;
		float ResolveNormalWeight = 16.0f;
		float RawBlend = 0.02f;
		float MinResolveWeight = 0.02f;
		UINT32 FrameIndex = 0;
		UINT32 Padding = 0;
		float TemporalAlpha = 0.06f;
		float HistoryDepthWeight = 32.0f;
		float HistoryNormalWeight = 32.0f;
		UINT32 HistoryValid = 0;
		float EdgeDepthWeight = 32.0f;
		float EdgeNormalWeight = 16.0f;
		UINT32 EdgeSampleCount = 3;
		UINT32 SHCoefficientCount = 4;
	};

	ScreenProbeGIConstant ScreenProbeGICB;
	shared_ptr<ComputePipelineStateObject> ScreenProbeGIPSO;

	// SHaRC-style spatial hash diffuse GI cache
	static constexpr UINT32 SpatialHashGIEntryCount = 1u << 21;
	static constexpr UINT32 SpatialHashGIActiveCellCapacity = 1u << 20;
	static constexpr UINT32 SpatialHashGITraceCellBudget = SpatialHashGIActiveCellCapacity;
	static constexpr UINT32 SpatialHashGISHCoefficientCount = 4u;
	struct SpatialHashGIConstant
	{
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec2 RTSize;
		float CellSize = 48.0f;
		float HistorySampleDecay = 1.0f;
		UINT32 HashEntryCount = SpatialHashGIEntryCount;
		UINT32 HashEntryMask = SpatialHashGIEntryCount - 1u;
		UINT32 FrameIndex = 0;
		UINT32 HistoryValid = 0;
		float TemporalAlpha = 0.08f;
		float SmoothingStrength = 0.65f;
		UINT32 MaxProbeSteps = 8;
		float InterpolationStrength = 1.0f;
		UINT32 ActiveCellCapacity = SpatialHashGIActiveCellCapacity;
		UINT32 TraceCellBudget = SpatialHashGITraceCellBudget;
		UINT32 Padding1 = 0;
		UINT32 Padding2 = 0;
	};

	SpatialHashGIConstant SpatialHashGICB;
	shared_ptr<ComputePipelineStateObject> SpatialHashGIClearPSO;
	shared_ptr<ComputePipelineStateObject> SpatialHashGIUpdatePSO;
	shared_ptr<ComputePipelineStateObject> SpatialHashGIResolvePSO;
	shared_ptr<ComputePipelineStateObject> SpatialHashGIQueryPSO;
	std::shared_ptr<Buffer> SpatialHashGIActiveFlags;
	std::shared_ptr<Buffer> SpatialHashGIActiveCellSlots;
	std::shared_ptr<Buffer> SpatialHashGIActiveCounter;
	std::shared_ptr<Buffer> SpatialHashGICellPosition;
	std::shared_ptr<Buffer> SpatialHashGICellNormal;
	std::shared_ptr<Buffer> SpatialHashGICellScore;
	std::shared_ptr<Buffer> SpatialHashGITraceSH[SpatialHashGISHCoefficientCount];
	std::shared_ptr<Buffer> SpatialHashGIResolvedKeys[2];
	std::shared_ptr<Buffer> SpatialHashGIResolvedSH[2][SpatialHashGISHCoefficientCount];
	UINT32 SpatialHashGIWriteIndex = 0;
	
	// RT shadow
	struct RTShadowViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4 LightDir;
		float ShadowLightRadius = 0.001f;
		UINT32 ShadowSampleCount = 1;
		UINT32 FrameCounter = 0;
		UINT32 BlueNoiseOffsetStride = 1;
		UINT32 NoiseMode = 1;
		UINT32 _padding0 = 0;
		UINT32 _padding1 = 0;
		UINT32 _padding2 = 0;
	};

	RTShadowViewParamCB RTShadowViewParam;
	
	shared_ptr<RTPipelineStateObject> PSO_RT_SHADOW;
	bool bShadowOutputValidThisFrame = false;

	// RT ambient occlusion. This is intentionally short-range contact AO; diffuse
	// GI remains responsible for broad, low-frequency lighting.
	struct RTAOViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec2 RTSize;
		float Radius = 96.0f;
		float Power = 1.10f;
		UINT32 SampleCount = 1;
		UINT32 FrameCounter = 0;
		UINT32 NoiseMode = 1;
		UINT32 BlueNoiseOffsetStride = 1;
		float NormalBias = 0.35f;
		glm::vec3 _padding = glm::vec3(0.0f);
	};

	RTAOViewParamCB RTAOViewParam;
	shared_ptr<RTPipelineStateObject> PSO_RT_AO;
	bool bRTAOOutputValidThisFrame = false;

	struct RTSkyLightingViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec2 RTSize;
		float RayLength = 10000.0f;
		float NormalBias = 0.5f;
		glm::vec3 SkyColorTop = glm::vec3(1.0f);
		float SkyIntensity = 3.0f;
		glm::vec3 SkyColorBottom = glm::vec3(0.8f);
		UINT32 SampleCount = 32;
		UINT32 FrameCounter = 0;
		UINT32 NoiseMode = 1;
		UINT32 BlueNoiseOffsetStride = 1;
		float SkyUpBias = 0.65f;
		float SkyDirectionPower = 2.25f;
		float SkyMinWorldY = 0.02f;
		UINT32 SkyMaxSampleAttempts = 4;
		UINT32 _padding = 0;
	};

	RTSkyLightingViewParamCB RTSkyLightingViewParam;
	shared_ptr<RTPipelineStateObject> PSO_RT_SKY_LIGHTING;
	bool bSkyLightingOutputValidThisFrame = false;

	// RT reflection
	struct RTReflectionViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::mat4x4 UnjitteredViewProjMatrix;
		glm::mat4x4 PrevUnjitteredViewProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4	LightDir;
		glm::vec2 RandomOffset;
		UINT32 FrameCounter;
		UINT32 BlueNoiseOffsetStride = 1;
		float ViewSpreadAngle;
		UINT32 NoiseMode = 1;
		glm::vec2 _noisePadding;
		glm::vec3 SkyColorTop;
		float SkyIntensity;
		glm::vec3 SkyColorBottom;
		float _padding;
		glm::vec3 LightColor;
		float PrefilteredEnvRoughnessThreshold = 0.65f;
		float PrefilteredEnvRoughnessFade = 0.10f;
		UINT32 bEnablePrefilteredEnvSpecular = 0;
		float SpecularMotionVectorScale = 1.0f;
		UINT32 bWriteRRSpecularMotionVectors = 0;
		UINT32 bWriteRRSpecularHitDistance = 0;
		UINT32 bUseRRSpecularGuideRay = 1;
	};

	RTReflectionViewParamCB RTReflectionViewParam;
	
	shared_ptr<RTPipelineStateObject> PSO_RT_REFLECTION;
	shared_ptr<RTPipelineStateObject> PSO_RT_REFLECTION_SER;
	bool bRTReflectionSERInitFailed = false;

	// RT GI
	struct RTGIViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4 LightDir;
		glm::vec2 RandomOffset;
		UINT32 FrameCounter;
		UINT32 BlueNoiseOffsetStride = 1;
		float ViewSpreadAngle;
		UINT32 NoiseMode = 1;
		UINT32 bIncludeSkyLighting = 0;
		float _noisePadding = 0.0f;
		glm::vec3 SkyColorTop;
		float SkyIntensity;
		glm::vec3 SkyColorBottom;
		float _padding;
		glm::vec3 LightColor;
		float _padding2;
	};

	RTGIViewParamCB RTGIViewParam;
	shared_ptr<RTPipelineStateObject> PSO_RT_GI;
	shared_ptr<RTPipelineStateObject> PSO_RT_GI_SER;
	bool bRTDiffuseGISimpleSERInitFailed = false;

	struct RTScreenProbeGIViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4 LightDir;
		glm::vec2 RandomOffset;
		glm::vec2 RTSize;
		glm::vec2 ProbeGridSize;
		UINT32 FrameCounter = 0;
		UINT32 BlueNoiseOffsetStride = 1;
		UINT32 NoiseMode = 1;
		UINT32 ProbeSpacing = 8;
		UINT32 RaysPerProbe = 4;
		UINT32 HistoryValid = 0;
		float ViewSpreadAngle = 0.0f;
		float TemporalAlpha = 0.06f;
		float HistoryDepthWeight = 32.0f;
		float HistoryNormalWeight = 32.0f;
		UINT32 bIncludeSkyLighting = 0;
		glm::vec3 SkyColorTop;
		float SkyIntensity = 3.0f;
		glm::vec3 SkyColorBottom;
		float _padding = 0.0f;
		glm::vec3 LightColor;
		float _padding2 = 0.0f;
		UINT32 LightingBootstrap = 0;
		UINT32 BootstrapRays = 100;
		UINT32 SHCoefficientCount = 4;
		UINT32 _padding3 = 0;
	};

	RTScreenProbeGIViewParamCB RTScreenProbeGIViewParam;
	shared_ptr<RTPipelineStateObject> PSO_RT_SCREEN_PROBE_GI;
	shared_ptr<RTPipelineStateObject> PSO_RT_SCREEN_PROBE_GI_SER;
	bool bRTDiffuseGIScreenProbeSERInitFailed = false;

	struct RTSpatialHashGIViewParamCB
	{
		glm::vec4 LightDir;
		UINT32 HashEntryCount = SpatialHashGITraceCellBudget;
		UINT32 FrameCounter = 0;
		UINT32 BlueNoiseOffsetStride = 1;
		UINT32 NoiseMode = 1;
		UINT32 RaysPerCell = 2;
		UINT32 MaxBounces = 2;
		float ViewSpreadAngle = 0.0f;
		float RayBias = 0.5f;
		float CellSize = 48.0f;
		glm::vec3 SkyColorTop;
		float SkyIntensity = 3.0f;
		glm::vec3 SkyColorBottom;
		float _padding = 0.0f;
		glm::vec3 LightColor;
		float _padding2 = 0.0f;
		UINT32 ActiveCellCapacity = SpatialHashGIActiveCellCapacity;
		UINT32 bIncludeSkyLighting = 0;
		UINT32 _padding4 = 0;
		UINT32 _padding5 = 0;
	};

	RTSpatialHashGIViewParamCB RTSpatialHashGIViewParam;
	shared_ptr<RTPipelineStateObject> PSO_RT_SPATIAL_HASH_GI;
	shared_ptr<RTPipelineStateObject> PSO_RT_SPATIAL_HASH_GI_SER;
	bool bRTDiffuseGISpatialHashSERInitFailed = false;

	static constexpr UINT32 MaxPointLights = 8;

	struct PointLightParam
	{
		glm::vec4 PositionAndRadius = glm::vec4(0.0f);
		glm::vec4 ColorAndIntensity = glm::vec4(1.0f);
	};
	
	// Path Tracing
	enum class EPathTracingDebugMode
	{
		NONE = 0,
		ALBEDO = 1,
		NORMAL = 2,
		ROUGHNESS = 3,
		METALLIC = 4,
		WORLD_POSITION = 5,
		BARYCENTRIC = 6
	};
	
	struct PathTracingViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::mat4x4 UnjitteredViewProjMatrix;
		glm::mat4x4 PrevUnjitteredViewProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4 LightDirAndIntensity;
		float DirectLightAngularRadius = 0.001f;
		UINT32 DirectLightSampleCount = 1;
		glm::vec2 _directLightPadding = glm::vec2(0.0f);
		glm::vec2 RandomOffset;
		UINT32 FrameCounter;
		UINT32 BlueNoiseOffsetStride = 1;
		UINT32 MaxBounces = 4;
		UINT32 SamplesPerPixel = 1;
		float ViewSpreadAngle;
		UINT32 DebugMode = 0; // EPathTracingDebugMode
		glm::vec3 SkyColorTop;
		float SkyIntensity;
		glm::vec3 SkyColorBottom;
		float _padding;
		glm::vec3 LightColor;
		float _padding2;
		UINT32 bEnableDiffuseGI;
		UINT32 bEnableSpecularGI;
		UINT32 bEnableDirectDiffuse;
		UINT32 bEnableDirectSpecular;
		UINT32 bEnableRTAO;
		UINT32 bWritePrimaryGBuffer = 0;
		float SpecularMotionVectorScale = 1.0f;
		UINT32 bStabilizePrimaryRaySamples = 0;
		UINT32 _rtaoPadding = 0;
		PointLightParam PointLights[MaxPointLights];
		UINT32 PointLightCount = 0;
		glm::vec3 PointLightPadding = glm::vec3(0.0f);
	};

	PathTracingViewParamCB PathTracingViewParam;
	shared_ptr<RTPipelineStateObject> PSO_PATH_TRACING;
	shared_ptr<Texture> PathTracingAccumBuffer[2];
	UINT PathTracingWriteIndex = 0;
	UINT32 PathTracingAccumulatedFrames = 0;
	UINT32 PathTracingLastDispatchSamplesPerPixel = 1;
	float PathTracingRRSpecularMotionVectorScale = 1.0f;
	bool bEnablePathTracingRRSpecularMotionVectors = true;
	bool bEnablePathTracingRRSpecularHitDistance = false;
	bool bEnablePathTracingRRPrimaryRayStabilization = true;
	float HybridRRSpecularMotionVectorScale = 1.0f;
	bool bEnableHybridRRSpecularMotionVectors = true;
	bool bEnableHybridRRSpecularHitDistance = true;
	bool bEnableHybridRRSpecularGuideRay = true;
	glm::mat4x4 PrevPathTracingViewMat = glm::mat4x4(0.0f);
	glm::vec3 PrevPathTracingLightDir;
	float PrevPathTracingLightIntensity = 0.0f;
	glm::vec3 PrevSkyColorTop = glm::vec3(0.0f);
	glm::vec3 PrevSkyColorBottom = glm::vec3(0.0f);
	float PrevSkyIntensity = 0.0f;
	UINT32 IndirectAccumulatedFrames = 0;
	glm::mat4x4 PrevIndirectAccumViewMat = glm::mat4x4(0.0f);
	glm::vec3 PrevIndirectAccumLightDir = glm::vec3(0.0f);
	float PrevIndirectAccumLightIntensity = 0.0f;
	glm::vec3 PrevIndirectSkyColorTop = glm::vec3(0.0f);
	glm::vec3 PrevIndirectSkyColorBottom = glm::vec3(0.0f);
	float PrevIndirectSkyIntensity = 0.0f;
	float PrevIndirectSkyLightingStrength = 0.0f;
	bool PrevIndirectDiffuseGISkyLightingEnabled = false;
	float PrevIndirectPrefilteredEnvRoughnessThreshold = 0.0f;
	float PrevIndirectPrefilteredEnvRoughnessFade = 0.0f;
	bool PrevIndirectPrefilteredEnvSpecularEnabled = false;

	// full screen copy pass
	enum EToneMapMode
	{
		LINEAR_TO_SRGB,
		REINHARD,
		FILMIC_ALU,
		FILMIC_HABLE,
	};
	struct ToneMapCB
	{
		glm::vec4 Scale;
		glm::vec4 Offset;
		UINT32 ToneMapMode = 0;
		float WhitePoint_Hejl = 1.0f;
		float ShoulderStrength = 4.0f;
		float LinearStrength = 5.0f;
		float LinearAngle = 0.12f;
		float ToeStrength = 13.0f;
		float WhitePoint_Hable = 6.0f;
	};
	ToneMapCB ToneMapCB;


	UINT32 ToneMapMode = FILMIC_HABLE;
#if CORONA_HAS_D3D12
	shared_ptr<PipelineStateObject> ToneMapPSO;
#endif
	std::shared_ptr<GraphicsPipelineHandle> ToneMapGraphicsPipeline;

	// debug pass
	enum EDebugMode
	{
		RAW_COPY = 0,
		CHANNEL_X = 1,
		CHANNEL_Y = 2,
		CHANNEL_Z = 3,
		CHANNEL_W = 4,
		SH_LIGHTING = 5,
		DEPTH = 6,
		HISTORY_LENGTH = 7,
		COUNT = 8,
	};
	struct DebugPassCB
	{
		glm::vec4 Scale;
		glm::vec4 Offset;
		glm::vec4 ProjectionParams;
		glm::vec2 RTSize;
		float GIBufferScale;
		UINT32 DebugMode;
	};

#if CORONA_HAS_D3D12
	shared_ptr<PipelineStateObject> BufferVisualizePSO;
#endif

	// lighting pass
	
	struct LightingParam
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::mat4x4 ShadowViewProjectionMatrix;
		glm::vec4 LightDir;
		glm::vec2 RTSize;
		float TAABlendFactor;
		float GIBufferScale;
		glm::vec3 LightColor;
		float _padding;
		UINT32 bEnableDiffuseGI;
		UINT32 bEnableSpecularGI;
		UINT32 bEnableDirectDiffuse;
		UINT32 bEnableDirectSpecular;
		UINT32 bEnableRTAO;
		UINT32 bEnableSkyLighting;
		float RTAOIndirectStrength;
		float RTAOIndirectFloor;
		float SurfaceBounceStrength;
		float SurfaceBounceSaturation;
		float SkyLightingStrength;
		UINT32 LightingOutputMode = 0;
		UINT32 bEnableDirectionalShadow = 1;
		UINT32 bUseShadowMap = 0;
		UINT32 bEnableSimpleSkyLighting = 0;
		UINT32 LightingPadding1 = 0;
		glm::vec4 AmbientSkyColorAndStrength = glm::vec4(0.0f);
		glm::vec4 AmbientGroundColorAndStrength = glm::vec4(0.0f);
		PointLightParam PointLights[MaxPointLights];
		UINT32 PointLightCount = 0;
		glm::vec3 PointLightPadding = glm::vec3(0.0f);
	};
	
#if CORONA_HAS_D3D12
	shared_ptr<PipelineStateObject> LightingPSO;
#endif
	std::shared_ptr<GraphicsPipelineHandle> LightingGraphicsPipeline;

	struct ShadowMapConstantBuffer
	{
		glm::mat4x4 LightViewProjectionMatrix;
		glm::mat4x4 WorldMatrix;
		glm::vec4 BaseColorFactor = glm::vec4(1.0f);
		UINT32 SpineVertexBase = 0;
		UINT32 Padding[3] = {};
	};
	std::shared_ptr<GraphicsPipelineHandle> MobileShadowMapGraphicsPipeline;
	std::shared_ptr<GraphicsPipelineHandle> SpineMobileShadowMapGraphicsPipeline;

	// temporalAA
	struct TemporalAAParam
	{
		glm::vec2 RTSize;
		float TAABlendFactor;
		UINT32 ClampMode;
		float BloomStrength;
		UINT32 HistoryValid;
		glm::vec2 CurrentJitter;
	};

public:
	enum class EAntiAliasingMode
	{
		OFF = 0,
		TAA,
		DLSS_SR,
		DLSS_RR,
		COUNT
	};

	enum class EDLSSQualityMode
	{
		QUALITY = 0,
		BALANCED,
		PERFORMANCE,
		ULTRA_PERFORMANCE,
		COUNT
	};

	enum class ERayNoiseMode
	{
		BLUE_NOISE = 0,
		R2_LOW_DISCREPANCY,
		STABLE_HASH,
		COUNT
	};

	enum class EDiffuseGIMode
	{
		SIMPLE_RAYTRACE = 0,
		SPATIAL_HASH,
		SCREEN_PROBE,
		COUNT
	};

	using SceneObjectHandle = uint32_t;
	static constexpr SceneObjectHandle InvalidSceneObjectHandle = 0;
	using ScriptSceneHandle = uint32_t;
	static constexpr ScriptSceneHandle InvalidScriptSceneHandle = 0;

	struct NativeEntityScriptCallbacks
	{
		std::function<void(Corona&, CoronaECS::Entity, float)> Update;
		std::function<void(Corona&, CoronaECS::Entity)> Shutdown;
		std::function<void(Corona&, CoronaECS::Entity)> Ui;
		std::function<void(Corona&, CoronaECS::Entity)> ImGui;
	};

	struct ScriptFunctionProfileStat
	{
		std::wstring SourceName;
		std::string CallbackName;
		bool bNative = false;
		uint64_t CallCount = 0;
		double TotalMs = 0.0;
		double LastMs = 0.0;
		double MinMs = 0.0;
		double MaxMs = 0.0;
	};
	struct ScriptFunctionSampleStat
	{
		std::wstring SourceName;
		std::string FunctionName;
		int LineDefined = -1;
		uint64_t InclusiveSamples = 0;
		uint64_t SelfSamples = 0;
		double InclusiveMs = 0.0;
		double SelfMs = 0.0;
	};

	enum class EPhysicsCollisionShape : UINT8
	{
		TriangleMesh,
		Box,
	};

	struct SceneObjectDesc
	{
		CoronaECS::Entity EntityHandle;
		shared_ptr<Scene> ScenePtr;
		glm::mat4x4 Transform = glm::mat4x4(1.0f);
		float Roughness = 1.0f;
		float Metallic = 0.0f;
		bool bOverrideRoughnessMetallic = false;
		bool bVisible = true;
		bool bRayTracing = true;
		bool bPhysicsQuery = true;
		EPhysicsCollisionShape PhysicsCollisionShape = EPhysicsCollisionShape::TriangleMesh;
		glm::vec3 PhysicsBoxHalfExtent = glm::vec3(0.5f);
	};

	struct CpuPhysicsRaycastHit
	{
		SceneObjectHandle ObjectHandle = InvalidSceneObjectHandle;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		float Distance = 0.0f;
	};

private:

	EAntiAliasingMode AntiAliasingMode = EAntiAliasingMode::DLSS_RR;
	EDLSSQualityMode DLSSQualityMode = EDLSSQualityMode::QUALITY;
	ERayNoiseMode RayNoiseMode = ERayNoiseMode::R2_LOW_DISCREPANCY;
	EDiffuseGIMode DiffuseGIMode = EDiffuseGIMode::SPATIAL_HASH;
	bool bEnableDiffuseGI = true;
	bool bEnableRTDiffuseGISER = false;
	bool bEnableRTReflectionSER = false;
	bool bD3D12ShaderModel69Supported = false;
	bool bEnableSpecularGI = true;
	bool bEnableDirectDiffuse = true;
	bool bEnableDirectSpecular = true;
	bool bEnableRTAO = true;
	bool bEnableSkyLighting = false;
	bool bEnableRayTracedSkyLighting = true;
	float RTAOIndirectStrength = 0.25f;
	float RTAOIndirectFloor = 0.55f;
	float SurfaceBounceStrength = 0.35f;
	float SurfaceBounceSaturation = 0.45f;
	float SkyLightingStrength = 0.35f;

	UINT32 ClampMode = 2;

	float JitterScale = 0.6;
	UINT32 TAASampleCount = 32;
	float DLSSJitterPhaseScale = 4.0f;
	UINT32 DLSSJitterPhaseCountAuto = 32;
	UINT32 DLSSJitterPhaseCount = 32;
	UINT32 DLSSJitterPhaseCountOverride = 0;
	bool bEnablePathTracingDLSSRR = true;
	UINT RenderWidth = 0;
	UINT RenderHeight = 0;
	bool bPendingUpscaleRefresh = false;
	bool bForceUpscaleReload = false;
	bool bResetTemporalStateNextUpdate = false;
	UINT32 DLSSTransitionFramesRemaining = 0;
	bool bUseLightingBufferFallbackForToneMap = false;
	bool bDLSSRROutputValidThisFrame = false;
	bool bAutoAADumpEnabled = true;
	bool bAutoAADumpInitialized = false;
	bool bAutoAADumpCompleted = false;
	bool bHybridStageAutoDumpMode = false;
	bool bDiffuseGIAutoDumpMode = false;
	bool bReadmeScreenshotDumpMode = false;
	bool bPathTracingScreenshotDumpMode = false;
	bool bLightingCompareDumpMode = false;
	bool bMobileGBufferDumpMode = false;
	bool bAASwitchDumpMode = false;
	bool bSpecularSequenceDumpMode = false;
	bool bLoggedHybridStageLimit = false;
	bool bMobileGBufferDumpCompleted = false;
	bool bStartupModeConfigured = false;
	UINT32 AutoAADumpPhase = 0;
	UINT32 AutoAADumpFramesInPhase = 0;
	UINT32 AutoAADumpFrameCountOverride = 0;
	UINT32 DiffuseGIAutoDumpFrameCount = 96;
	UINT32 MobileGBufferDumpPhase = 0;
	std::wstring AutoAADumpDir;
	std::wstring MobileGBufferDumpDir;
	EAntiAliasingMode StartupSelectedAAMode = EAntiAliasingMode::DLSS_RR;
	ERenderingMode StartupRenderingMode = ERenderingMode::HYBRID;
	ERenderBackendAPI StartupRenderBackendAPI = ERenderBackendAPI::D3D12;
	bool bCommandLineAutoDumpOverrideSet = false;
	bool bCommandLineAutoDumpEnabled = false;
	bool bCommandLineAAOverrideSet = false;
	EAntiAliasingMode CommandLineSelectedAAMode = EAntiAliasingMode::DLSS_RR;
	bool bCommandLineRenderModeOverrideSet = false;
	ERenderingMode CommandLineRenderingMode = ERenderingMode::HYBRID;
	bool bCommandLineRenderBackendOverrideSet = false;
	ERenderBackendAPI CommandLineRenderBackendAPI = ERenderBackendAPI::D3D12;
	bool bCommandLineDisableImgui = false;
	bool bCommandLineDisableStreamline = false;
	bool bEnableGpuSpineSkinning = true;
	bool bCommandLineSpineSkinningOverrideSet = false;
	bool bCommandLineSpineGpuSkinningEnabled = true;
	bool bCommandLinePlatformerSpineBenchmark = false;
	UINT32 CommandLinePlatformerSpineBenchmarkCount = 50;
	bool bCommandLineSpawnSkeletalTest = false;
	UINT32 CommandLineSkeletalTestCount = 1;
	bool bCommandLineBvhViewerOverrideSet = false;
	bool bCommandLineBvhViewerEnabled = false;
	bool bCommandLineNvFrapsBvhLiveTlas = false;
	bool bStartupSponzaFlyMode = false;
	bool bCommandLineDiffuseGIAutoDumpMode = false;
	bool bCommandLineReadmeScreenshotDumpMode = false;
	bool bCommandLinePathTracingScreenshotDumpMode = false;
	bool bCommandLineLightingCompareDumpMode = false;
	bool bCommandLineMobileGBufferDumpMode = false;
	bool bCommandLineAASwitchDumpMode = false;
	bool bCommandLineSpecularSequenceDumpMode = false;
	bool bCommandLineCameraPathDump = false;
	bool bCommandLineCameraPathDiagnostics = false;
	bool bCommandLineLoadLatestCameraPath = false;
	UINT32 CommandLineExitAfterFrames = 0;
	bool bCommandLineExitAfterFramesTriggered = false;
	std::wstring CommandLineCameraPathFile;

#if CORONA_HAS_D3D12
	shared_ptr<PipelineStateObject> TemporalAAPSO;
#endif
	std::shared_ptr<GraphicsPipelineHandle> TemporalAAGraphicsPipeline;
	bool bTemporalAAHistoryValid = false;
	bool bTemporalDenoiserHistoryValid = false;
	bool bScreenProbeGIAtlasHistoryValid = false;
	bool bScreenProbeGIHistoryValid = false;
	bool bScreenProbeLightingBootstrapPending = false;
	bool bSpatialHashGIHistoryValid = false;
	bool bPendingTemporalHistoryClear = false;
	bool bImguiInitialized = false;
	bool bBlueNoiseInitialized = false;
#if WITH_STREAMLINE
	bool bStreamlineInitialized = false;
	bool bDLSSAvailable = false;
	bool bDLSSRRAvailable = false;
	bool bDLSSResetNeeded = false;
	sl::FrameToken* StreamlineFrameToken = nullptr;
	uint32_t StreamlineFrameIndex = 0;
	bool bStreamlineConstantsSetThisFrame = false;
#else
	bool bDLSSAvailable = false;
	bool bDLSSRRAvailable = false;
#endif


	// bloom blur
	struct BloomCB
	{
		glm::vec2 BlurDirection;
		glm::vec2 RTSize;
		UINT32 NumSamples;
		float WeightScale;
		float NormalizationScale;
		float BloomThreshHold = 1.0;
		//float Exposure;
		//float MinLog;
		//float RcpLogRange;
	};

	const float kInitialMinLog = -12.0f;
	const float kInitialMaxLog = 4.0f;

	BloomCB BloomCB;
	float BloomSigma = 0.037;
	float Exposure = 1;
	float BloomStrength = 1.0;

	UINT BloomBufferWidth = 640;
	UINT  BloomBufferHeight = 384;

#if CORONA_HAS_D3D12
	shared_ptr<PipelineStateObject> BloomBlurPSO;

	shared_ptr<PipelineStateObject> BloomExtractPSO;

	shared_ptr<PipelineStateObject> HistogramPSO;

	shared_ptr<PipelineStateObject> ClearHistogramPSO;
#endif

	bool bDrawHistogram = false;
#if CORONA_HAS_D3D12
	shared_ptr<PipelineStateObject> DrawHistogramPSO;

	struct AdaptExposureCB
	{
		float TargetLuminance = 0.008;
		float AdaptationRate = 0.05;
		float MinExposure = 1.0f / 64.0f;
		float MaxExposure = 8;
		UINT32 PixelCount;
	};

	/*AdaptExposureCB.TargetLuminance = 0.08;
AdaptExposureCB.AdaptationRate = 0.05;
AdaptExposureCB.MinExposure = 1.0f / 64.0f;
AdaptExposureCB.MaxExposure = 64.0f;*/
	AdaptExposureCB AdaptExposureCB;

	shared_ptr<PipelineStateObject> AdapteExposurePSO;
#endif // CORONA_HAS_D3D12 (DX12-only post-process PSOs)

	shared_ptr<VertexBuffer> FullScreenVB;

	// blue noise texture
	shared_ptr<Texture> BlueNoiseTex;
	shared_ptr<Texture> DefaultWhiteTex;
	shared_ptr<Texture> DefaultBlackTex;
	shared_ptr<Texture> DefaultNormalTex;
	shared_ptr<Texture> DefaultRougnessTex;
	shared_ptr<Texture> ProceduralDungeonBrickDiffuseTex;
	std::map<std::wstring, shared_ptr<Texture>> ProceduralBoxDiffuseTextures;

	// global wrap sampler
	std::shared_ptr<Sampler> samplerWrap;
	std::shared_ptr<Sampler> samplerBilinearWrap;


	// mesh
	shared_ptr<Mesh> mesh;

	shared_ptr<Scene> Sponza;
	SceneObjectHandle SponzaObject = InvalidSceneObjectHandle;

	shared_ptr<Scene> Buddha;
	SceneObjectHandle BuddhaObject = InvalidSceneObjectHandle;
	glm::vec3 BuddhaCenterPosition = glm::vec3(273.0f, -12.0f, -6.0f);
	glm::vec3 BuddhaCenterRotationDegrees = glm::vec3(0.0f, -12.0f, 0.0f);

	shared_ptr<Scene> ShaderBall;
	SceneObjectHandle ShaderBallObject = InvalidSceneObjectHandle;
	glm::vec3 ShaderBallCenterPosition = glm::vec3(0.0f, -4.0f, -72.0f);
	glm::vec3 ShaderBallCenterRotationDegrees = glm::vec3(0.0f, -120.0f, 0.0f);

	shared_ptr<Scene> Pistol;
	SceneObjectHandle PistolObject = InvalidSceneObjectHandle;
	glm::vec3 PistolCenterPosition = glm::vec3(30.0f, 62.0f, 116.0f);
	glm::vec3 PistolCenterRotationDegrees = glm::vec3(0.0f, 141.0f, 0.0f);

	shared_ptr<Scene> MirrorCube;
	SceneObjectHandle MirrorCubeObject = InvalidSceneObjectHandle;
	glm::vec3 MirrorCubeCenterPosition = glm::vec3(0.0f, 0.0f, 0.0f);
	glm::vec3 MirrorCubeCenterRotationDegrees = glm::vec3(0.0f, 0.0f, 0.0f);

	struct SceneObject
	{
		SceneObjectHandle Handle = InvalidSceneObjectHandle;
		CoronaECS::Entity EntityHandle;
		shared_ptr<Scene> ScenePtr;
		glm::mat4x4 Transform = glm::mat4x4(1.0f);
		float Roughness = 1.0f;
		float Metallic = 0.0f;
		bool bOverrideRoughnessMetallic = false;
		bool bVisible = true;
		bool bRayTracing = true;
		bool bPhysicsQuery = true;
		EPhysicsCollisionShape PhysicsCollisionShape = EPhysicsCollisionShape::TriangleMesh;
		glm::vec3 PhysicsBoxHalfExtent = glm::vec3(0.5f);
		UINT32 RenderDirtyBits = 0;
	};
	vector<SceneObject> SceneObjects;
	SceneObjectHandle NextSceneObjectHandle = 1;
	CoronaECS::EntityComponentSystem EntityWorld;
	CoronaECS::Entity WorldEntity;
	CoronaECS::Entity LevelEntity;
	CoronaECS::Entity MainCameraEntity;
	bool bRayTracingSceneDirty = false;
	bool bRayTracingTransformDirty = false;

	struct ScriptSceneEntry
	{
		shared_ptr<Scene> ScenePtr;
		std::wstring Path;
		EPhysicsCollisionShape PhysicsCollisionShape = EPhysicsCollisionShape::TriangleMesh;
		glm::vec3 PhysicsBoxHalfExtent = glm::vec3(0.5f);
	};
	struct ScriptObjectState
	{
		ScriptSceneHandle SceneHandle = InvalidScriptSceneHandle;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 RotationDegrees = glm::vec3(0.0f);
		float TargetExtent = 1.0f;
		glm::vec3 Scale = glm::vec3(1.0f);
		bool bUseScale = false;
	};
	struct LuauScriptState
	{
		lua_State* L = nullptr;
	};
	std::unique_ptr<LuauScriptState> ScriptState;
	enum class ScriptUiCommandType
	{
		Separator,
		Text,
		SameLine,
		BeginWindow,
		EndWindow,
		Button,
		SliderFloat,
		SliderInt,
		Combo,
		Checkbox,
		OverlayText,
		OverlayLine,
		OverlayRect,
		OverlayRectFilled,
		OverlayButton,
		OverlayProgressBar,
		Gizmo3D,
		WorldAxis,
		WorldText,
		WorldProgressBar,
		WorldHealthBar,
	};
	struct ScriptUiCommand
	{
		ScriptUiCommandType Type = ScriptUiCommandType::Text;
		bool bGameUi = false;
		std::string Id;
		std::string Label;
		float FloatValue = 0.0f;
		float MinValue = 0.0f;
		float MaxValue = 0.0f;
		int IntValue = 0;
		int MinIntValue = 0;
		int MaxIntValue = 0;
		bool BoolValue = false;
		glm::vec3 Vec3Value = glm::vec3(0.0f);
		glm::vec4 ColorValue = glm::vec4(1.0f);
		glm::vec4 SecondaryColorValue = glm::vec4(0.0f);
		glm::vec4 TertiaryColorValue = glm::vec4(1.0f);
		std::vector<std::string> Items;
	};
	enum class PersistentScriptControlType
	{
		Number,
		Bool,
		Vec3,
	};
	struct PersistentScriptControlValue
	{
		PersistentScriptControlType Type = PersistentScriptControlType::Number;
		float Number = 0.0f;
		bool Bool = false;
		glm::vec3 Vec3 = glm::vec3(0.0f);
	};
	std::mutex ScriptUiMutex;
	std::vector<ScriptUiCommand> ScriptUiBuildCommands;
	std::vector<ScriptUiCommand> ScriptUiRenderCommands;
	std::map<std::string, bool> ScriptUiClickedResults;
	std::map<std::string, float> ScriptUiFloatResults;
	std::map<std::string, int> ScriptUiIntResults;
	std::map<std::string, bool> ScriptUiBoolResults;
	std::map<std::string, glm::vec3> ScriptUiVec3Results;
	std::map<std::string, PersistentScriptControlValue> PersistentScriptControls;
	bool bPersistentSceneStateDirty = false;
	std::map<ScriptSceneHandle, ScriptSceneEntry> ScriptScenes;
	std::map<std::wstring, ScriptSceneHandle> ScriptSceneByPath;
	std::map<SceneObjectHandle, ScriptObjectState> ScriptObjects;
	ScriptSceneHandle NextScriptSceneHandle = 1;
	std::map<std::string, NativeEntityScriptCallbacks> NativeEntityScripts;
	std::mutex ScriptProfileMutex;
	std::map<std::string, ScriptFunctionProfileStat> ScriptProfileStats;
	std::map<std::string, ScriptFunctionProfileStat> ScriptProfileCurrentFrameStats;
	std::vector<ScriptFunctionProfileStat> ScriptProfileLastFrameStats;
	double ScriptProfileCurrentFrameTotalMs = 0.0;
	double ScriptProfileLastFrameTotalMs = 0.0;
	std::map<std::string, ScriptFunctionSampleStat> ScriptProfileSamples;
	std::wstring ScriptProfileLastDumpStatus;
	std::atomic<bool> bScriptProfileSamplerStop = false;
	std::atomic<int> ScriptProfileActiveDepth = 0;
	std::atomic<uint64_t> ScriptProfileSamplerTicksNs = 0;
	std::atomic<uint64_t> ScriptProfileSamplerRequests = 0;
	std::atomic<lua_Callbacks*> ScriptProfileLuaCallbacks = nullptr;
	std::thread ScriptProfileSamplerThread;
	uint64_t ScriptProfileLastSampleTickNs = 0;
	int ScriptProfileSampleHz = 1000;
	bool bEnableStartupLuauScript = true;
	bool bCommandLineDungeonCharacterMode = false;
	std::wstring StartupLuauMode = L"platformer";
	bool bScriptCameraControlEnabled = false;
	bool bLuauImGuiFrameActive = false;
	bool bScriptGameUiHidden = false;
	bool bCurrentScriptUiIsGame = false;
	std::array<bool, 256> ScriptKeyDown = {};
	std::array<bool, 256> ScriptKeyPressed = {};
	std::array<bool, 256> ScriptKeyReleased = {};
	bool bScriptRightMouseDown = false;
	bool bScriptRightMousePressed = false;
	bool bScriptRightMouseReleased = false;
	int ScriptMouseX = 0;
	int ScriptMouseY = 0;
	int ScriptMouseDeltaX = 0;
	int ScriptMouseDeltaY = 0;
	bool bScriptMousePositionInitialized = false;
	bool bScriptGamepadConnected = false;
	float ScriptGamepadLeftX = 0.0f;
	float ScriptGamepadLeftY = 0.0f;
	float ScriptGamepadRightX = 0.0f;
	float ScriptGamepadRightY = 0.0f;
	float ScriptGamepadLeftTrigger = 0.0f;
	float ScriptGamepadRightTrigger = 0.0f;
	uint16_t ScriptGamepadButtonsDown = 0;
	uint16_t ScriptGamepadButtonsPressed = 0;
	uint16_t ScriptGamepadButtonsReleased = 0;
	struct CpuPhysicsState;
	struct CpuPhysicsStateDeleter
	{
		void operator()(CpuPhysicsState* state) const;
	};
	std::unique_ptr<CpuPhysicsState, CpuPhysicsStateDeleter> CpuPhysics;
	bool bCpuPhysicsSceneDirty = true;

	// time & camera
	StepTimer m_timer;

	float m_turnSpeed = glm::half_pi<float>();

	SimpleCamera m_camera;
	bool bMobileVirtualMoveForward = false;
	bool bMobileVirtualMoveBackward = false;
	bool bMobileVirtualMoveLeft = false;
	bool bMobileVirtualMoveRight = false;
	bool bMobileVirtualJoystickActive = false;
	glm::vec2 MobileVirtualMoveAxis = glm::vec2(0.0f);
	glm::vec2 MobileVirtualJoystickCenter = glm::vec2(0.0f);
	glm::vec2 MobileVirtualJoystickDrag = glm::vec2(0.0f);
	float MobileVirtualJoystickRadius = 1.0f;
	bool bMobileVirtualAttackDown = false;
	bool bMobileVirtualAttackPressed = false;
	bool bMobileVirtualAttackReleased = false;
	glm::vec2 MobileVirtualAttackCenter = glm::vec2(0.0f);
	bool bMobileVirtualAttack2Down = false;
	bool bMobileVirtualAttack2Pressed = false;
	bool bMobileVirtualAttack2Released = false;
	glm::vec2 MobileVirtualAttack2Center = glm::vec2(0.0f);
	bool bMobileVirtualAttack3Down = false;
	bool bMobileVirtualAttack3Pressed = false;
	bool bMobileVirtualAttack3Released = false;
	glm::vec2 MobileVirtualAttack3Center = glm::vec2(0.0f);
	float MobileVirtualAttackRadius = 1.0f;
	float MobileTouchLookSensitivityScale = 1.15f;
	struct CameraPathKeyframe
	{
		double TimeSeconds = 0.0;
		glm::vec3 Position = glm::vec3(0.0f);
		float Yaw = 0.0f;
		float Pitch = 0.0f;
		float Fov = 0.8f;
		glm::vec3 DirectionalLightDir = glm::vec3(0.0f, 1.0f, 0.0f);
		float DirectionalLightIntensity = 0.4f;
	};
	struct CameraPathListEntry
	{
		std::wstring DisplayName;
		std::wstring FilePath;
	};
	std::vector<CameraPathKeyframe> CameraPathKeyframes;
	std::vector<CameraPathListEntry> CameraPathEntries;
	int SelectedCameraPathIndex = -1;
	bool bCameraPathRecording = false;
	bool bCameraPathPlaying = false;
	bool bCameraPathDumping = false;
	bool bCameraPathDumpExitWhenComplete = false;
	bool bCameraPathDumpCaptureInFlight = false;
	bool bCameraPathListDirty = true;
	double CameraPathRecordingStartSeconds = 0.0;
	double CameraPathPlaybackStartSeconds = 0.0;
	UINT32 CameraPathDumpFrameIndex = 0;
	UINT32 CameraPathDumpFrameCount = 0;
	std::wstring ActiveCameraPathFile;
	std::wstring LastCameraPathDumpDir;
	std::wstring LastCameraPathStatus;
	std::wstring LastCameraPathVideoCommand;

	// misc
	glm::vec3 LightDir = glm::normalize(glm::vec3(0.901, 0.88, 0.176));
	float LightIntensity = 0.4;
	struct PointLightState
	{
		UINT32 Id = 0;
		CoronaECS::Entity EntityHandle;
		bool bEnabled = true;
		glm::vec3 Position = glm::vec3(0.0f);
		float Radius = 320.0f;
		glm::vec3 Color = glm::vec3(1.0f);
		float Intensity = 10.0f;
		UINT32 RenderDirtyBits = 0;
	};
	std::vector<PointLightState> PointLights;
	UINT32 NextPointLightId = 1;
	CoronaECS::Entity MainDirectionalLightEntity;

	enum class ERenderDeltaOp : UINT8
	{
		Upsert,
		Remove,
	};

	static constexpr UINT32 kSceneObjectDirtyScene = 1u << 0;
	static constexpr UINT32 kSceneObjectDirtyTransform = 1u << 1;
	static constexpr UINT32 kSceneObjectDirtyMaterial = 1u << 2;
	static constexpr UINT32 kSceneObjectDirtyVisibility = 1u << 3;
	static constexpr UINT32 kSceneObjectDirtyRayTracing = 1u << 4;
	static constexpr UINT32 kSceneObjectDirtyAll =
		kSceneObjectDirtyScene |
		kSceneObjectDirtyTransform |
		kSceneObjectDirtyMaterial |
		kSceneObjectDirtyVisibility |
		kSceneObjectDirtyRayTracing;

	static constexpr UINT32 kPointLightDirtyEnabled = 1u << 0;
	static constexpr UINT32 kPointLightDirtyTransform = 1u << 1;
	static constexpr UINT32 kPointLightDirtyShape = 1u << 2;
	static constexpr UINT32 kPointLightDirtyEmission = 1u << 3;
	static constexpr UINT32 kPointLightDirtyAll =
		kPointLightDirtyEnabled |
		kPointLightDirtyTransform |
		kPointLightDirtyShape |
		kPointLightDirtyEmission;

	struct RenderSceneObjectDelta
	{
		ERenderDeltaOp Op = ERenderDeltaOp::Upsert;
		UINT32 DirtyBits = 0;
		SceneObject Object;
		SceneObjectHandle Handle = InvalidSceneObjectHandle;
	};

	struct RenderPointLightDelta
	{
		ERenderDeltaOp Op = ERenderDeltaOp::Upsert;
		UINT32 DirtyBits = 0;
		PointLightState Light;
		UINT32 Id = 0;
	};

	struct RenderFrameSourceState
	{
		glm::vec3 CameraPosition = glm::vec3(0.0f);
		glm::vec3 CameraLookDirection = glm::vec3(0.0f, 0.0f, 1.0f);
		glm::vec3 CameraUpDirection = glm::vec3(0.0f, 1.0f, 0.0f);
		float Fov = 0.8f;
		float NearPlane = 10.0f;
		float FarPlane = 20000.0f;
		float AspectRatio = 1.0f;
		float TotalSeconds = 0.0f;
		ERenderingMode RenderingMode = ERenderingMode::HYBRID;
		EAntiAliasingMode AntiAliasingMode = EAntiAliasingMode::TAA;
		EDLSSQualityMode DLSSQualityMode = EDLSSQualityMode::QUALITY;
		ERayNoiseMode RayNoiseMode = ERayNoiseMode::R2_LOW_DISCREPANCY;
		EDiffuseGIMode DiffuseGIMode = EDiffuseGIMode::SPATIAL_HASH;
		bool bEnableDiffuseGI = true;
		bool bEnableRTDiffuseGISER = false;
		bool bEnableRTReflectionSER = false;
		bool bEnableSpecularGI = true;
		bool bEnableDirectDiffuse = true;
		bool bEnableDirectSpecular = true;
		bool bEnableRTAO = true;
		bool bEnableSkyLighting = false;
		bool bEnableRayTracedSkyLighting = true;
		float RTAOIndirectStrength = 0.25f;
		float RTAOIndirectFloor = 0.55f;
		float SurfaceBounceStrength = 0.35f;
		float SurfaceBounceSaturation = 0.45f;
		float SkyLightingStrength = 0.35f;
		float JitterScale = 0.6f;
		UINT32 TAASampleCount = 32;
		float DLSSJitterPhaseScale = 4.0f;
		UINT32 DLSSJitterPhaseCountOverride = 0;
		glm::vec3 LightDir = glm::vec3(0.0f, 1.0f, 0.0f);
		float LightIntensity = 0.4f;
		glm::vec3 SkyColorTop = glm::vec3(1.0f);
		glm::vec3 SkyColorBottom = glm::vec3(0.8f);
		float SkyIntensity = 3.0f;
		bool bEnablePrefilteredEnvSpecular = false;
		float PrefilteredEnvRoughnessThreshold = 0.65f;
		float PrefilteredEnvRoughnessFade = 0.10f;
		float ShadowLightRadius = 0.001f;
		UINT32 ShadowSampleCount = 1;
		float RTAORadius = 96.0f;
		float RTAOPower = 1.10f;
		float RTAONormalBias = 0.35f;
		UINT32 RTAOSampleCount = 1;
		float SkyLightingRayLength = 10000.0f;
		float SkyLightingNormalBias = 0.5f;
		UINT32 SkyLightingSampleCount = 32;
		float SkyLightingUpBias = 0.65f;
		float SkyLightingDirectionPower = 2.25f;
		float SkyLightingMinWorldY = 0.02f;
		UINT32 SkyLightingMaxSampleAttempts = 4;
		UINT32 ScreenProbeSpacing = 8;
		UINT32 ScreenProbeGatherRadius = 3;
		UINT32 ScreenProbeRaysPerProbe = 4;
		UINT32 ScreenProbeSHCoefficientCount = 4;
		float ScreenProbeRawBlend = 0.02f;
		float ScreenProbeMinResolveWeight = 0.02f;
		float ScreenProbeDepthWeight = 8.0f;
		float ScreenProbeNormalWeight = 8.0f;
		float ScreenProbeResolveDepthWeight = 24.0f;
		float ScreenProbeResolveNormalWeight = 16.0f;
		float ScreenProbeTemporalAlpha = 0.06f;
		float ScreenProbeHistoryDepthWeight = 32.0f;
		float ScreenProbeHistoryNormalWeight = 32.0f;
		float ScreenProbeEdgeDepthWeight = 32.0f;
		float ScreenProbeEdgeNormalWeight = 16.0f;
		UINT32 ScreenProbeEdgeSampleCount = 3;
		float SpatialHashCellSize = 48.0f;
		float SpatialHashTemporalAlpha = 0.08f;
		float SpatialHashSmoothingStrength = 0.65f;
		float SpatialHashInterpolationStrength = 1.0f;
		UINT32 SpatialHashRaysPerCell = 2;
		UINT32 SpatialHashMaxBounces = 2;
		UINT32 PathTracingDirectLightSampleCount = 1;
		UINT32 PathTracingMaxBounces = 4;
		UINT32 PathTracingSamplesPerPixel = 1;
		UINT32 PathTracingDebugMode = 0;
	};

	struct RenderFrameDelta
	{
		uint64_t FrameId = 0;
		bool bHasFrameSourceState = false;
		RenderFrameSourceState FrameSourceState;
		bool bFullSceneObjectSync = false;
		bool bFullPointLightSync = false;
		std::vector<RenderSceneObjectDelta> SceneObjectDeltas;
		std::vector<RenderPointLightDelta> PointLightDeltas;
	};

	struct RenderWorldMirror
	{
		std::vector<SceneObject> SceneObjects;
		std::vector<PointLightState> PointLights;
		bool bHasFrameSourceState = false;
		RenderFrameSourceState FrameSourceState;
	};

	using RenderSyncCollectFn = void (Corona::*)(RenderFrameDelta&);
	using RenderSyncApplyFn = void (Corona::*)(const RenderFrameDelta&);
	struct RenderSyncChannel
	{
		const char* Name = nullptr;
		RenderSyncCollectFn Collect = nullptr;
		RenderSyncApplyFn Apply = nullptr;
	};

	RenderWorldMirror RenderWorld;
	struct SceneObjectCullingState
	{
		bool LastVisible = true;
		bool HasPendingOcclusionQuery = false;
		uint32_t LastQueryIndex = 0;
		uint32_t LastQueryFrameIndex = 0;
		uint64_t LastTestFrame = 0;
		glm::vec3 LastBoundsCenter = glm::vec3(0.0f);
		float LastBoundsRadius = 0.0f;
		bool HasBounds = false;
	};
	std::map<SceneObjectHandle, SceneObjectCullingState> SceneObjectCullingStates;
	uint32_t GBufferOcclusionQueryCapacityPerFrame = 0;
	uint32_t GBufferOcclusionFrameIndex = 0;
	uint32_t GBufferOcclusionQueryCount = 0;
	bool bGBufferOcclusionQueriesActive = false;
	uint64_t GBufferLastTotalObjectCount = 0;
	uint64_t GBufferLastVisibleObjectCount = 0;
	uint64_t GBufferLastFrustumCulledObjectCount = 0;
	uint64_t GBufferLastOcclusionCulledObjectCount = 0;
	uint64_t MobileShadowLastTotalObjectCount = 0;
	uint64_t MobileShadowLastCandidateObjectCount = 0;
	uint64_t MobileShadowLastReceiverObjectCount = 0;
	uint64_t MobileShadowLastCasterObjectCount = 0;
	uint64_t MobileShadowLastGuaranteedCasterCount = 0;
	uint64_t MobileShadowLastCulledObjectCount = 0;
	uint64_t MobileShadowLastDistanceCulledObjectCount = 0;
	bool bShowCullingTextOverlay = false;
	std::vector<RenderSyncChannel> RenderSyncChannels;
	bool bRenderSyncChannelsInitialized = false;
	bool bSceneObjectFullSyncPending = false;
	bool bPointLightFullSyncPending = false;
	std::vector<SceneObjectHandle> DirtySceneObjectHandles;
	std::vector<SceneObjectHandle> RemovedSceneObjectHandles;
	std::vector<UINT32> DirtyPointLightIds;
	std::vector<UINT32> RemovedPointLightIds;
	std::mutex RenderFrameDeltaMutex;
	std::deque<RenderFrameDelta> PendingRenderFrameDeltas;
	uint64_t NextRenderFrameDeltaId = 1;
	
	// Sky colors for path tracing
	glm::vec3 SkyColorTop = glm::vec3(1.0f, 1.0f, 1.0f);
	glm::vec3 SkyColorBottom = glm::vec3(0.8f, 0.8f, 0.8f);
	float SkyIntensity = 3.0f;
	bool bEnablePrefilteredEnvSpecular = false;
	float PrefilteredEnvRoughnessThreshold = 0.65f;
	float PrefilteredEnvRoughnessFade = 0.10f;
	
	float Near = 10.0f;
	float Far = 20000.0f;
	float Fov = 0.8f;

	glm::mat4x4 ViewMat;
	glm::mat4x4 ProjMat;
	glm::mat4x4 UnjitteredProjMat;
	glm::mat4x4 InvViewMat;
	glm::mat4x4 InvProjMat;
	glm::mat4x4 UnjitteredInvProjMat;
	glm::mat4x4 ViewProjMat;
	glm::mat4x4 InvViewProjMat;
	glm::mat4x4 PrevViewMat;
	glm::mat4x4 PrevViewProjMat;
	glm::vec4 FrameProjectionParams = glm::vec4(0.0f);
	glm::vec3 RenderFrameCameraLookDirection = glm::vec3(0.0f, 0.0f, 1.0f);
	glm::vec3 RenderFrameNormalizedLightDir = glm::vec3(0.0f, 1.0f, 0.0f);
	glm::vec3 RenderFrameLightColor = glm::vec3(1.0f);
	float RenderFrameShaderTime = 0.0f;
	float RenderFrameDiffuseGISkyIntensity = 3.0f;
	UINT32 RenderFrameRayNoiseMode = 0;
	UINT32 RenderFrameDiffuseGISkyLightingEnabled = 0;
	UINT32 RenderFrameIndex = 0;
	glm::vec2 JitterOffset;
	glm::vec2 PrevJitter;
	glm::vec2 CurrentJitter;

	glm::mat4x4 UnjitteredViewProjMat;
	glm::mat4x4 PrevUnjitteredViewProjMat;


	// raytracing resources

	struct InstanceProperty
	{
		glm::mat4x4 WorldMatrix;
		UINT32 VertexOffset = 0;
		UINT32 IndexOffset = 0;
		UINT32 Flags = 0;
		UINT32 bOverrideRoughnessMetallic = 0;
		glm::vec2 RoughnessMetallic = glm::vec2(1.0f, 0.0f);
		glm::vec2 Padding = glm::vec2(0.0f);
	};

	std::shared_ptr<Buffer> InstancePropertyBuffer;
	shared_ptr<RTAS> TLAS;
	std::vector<std::shared_ptr<Buffer>> InstancePropertyFrameBuffers;
	std::vector<shared_ptr<RTAS>> TLASFrameResources;
	std::vector<UINT32> TLASFrameInstanceCounts;
	std::map<Mesh*, std::shared_ptr<RTAS>> RayTracingBLASCache;
	std::vector<RTInstanceDesc> RayTracingInstances;
	
	// ...
	bool bMultiThreadRendering = false;

	bool bDebugDraw = false;


	UINT m_frameCounter = 0;

	UINT FrameCounter = 0;

	// Pipeline objects.
#if CORONA_HAS_D3D12
	ComPtr<ID3D12Device5> m_device;
#endif
	std::unique_ptr<IRenderBackend> renderBackend;
	DX12Backend* dx12_rhi = nullptr;

	enki::TaskScheduler g_TS;

	bool bRecompileShaders = false;
	bool bShowImgui = true;
	bool bShowGpuTimingWindow = false;
	bool bFinalScreenshotRequested = false;
	bool bFinalScreenshotCaptureInFlight = false;
	UINT32 FinalScreenshotCounter = 0;
	std::wstring PendingFinalScreenshotPath;
	std::wstring LastFinalScreenshotStatus;
	struct AsyncImageDumpJob;
	std::deque<std::unique_ptr<AsyncImageDumpJob>> AsyncImageDumpQueue;
	std::vector<std::thread> AsyncImageDumpWorkers;
	std::mutex AsyncImageDumpMutex;
	std::condition_variable AsyncImageDumpCV;
	UINT32 AsyncImageDumpActiveJobs = 0;
	bool bAsyncImageDumpStop = false;
	bool bAsyncImageDumpWorkersStarted = false;
	bool bGpuTimingResourcesInitialized = false;
	UINT32 GpuTimingAverageFrameCount = 30;
	UINT64 GpuTimestampFrequency = 0;
	std::array<std::array<uint8_t, GpuPassCount>, 3> GpuPassActiveMaskPerFrame = {};
	std::array<float, GpuPassCount> GpuPassLastTimeMs = {};
	std::array<float, GpuPassCount> GpuPassAverageTimeMs = {};
	std::array<std::deque<float>, GpuPassCount> GpuPassHistoryMs = {};
	float CpuUpdateLastTimeMs = 0.0f;
	float CpuUpdateAverageTimeMs = 0.0f;
	std::deque<float> CpuUpdateHistoryMs;
	std::array<float, CpuUpdatePhaseCount> CpuUpdatePhaseLastTimeMs = {};
	std::array<float, CpuUpdatePhaseCount> CpuUpdatePhaseAverageTimeMs = {};
	std::array<std::deque<float>, CpuUpdatePhaseCount> CpuUpdatePhaseHistoryMs = {};
	std::array<float, RenderCommandPhaseCount> RenderCommandPhaseLastTimeMs = {};
	std::array<float, RenderCommandPhaseCount> RenderCommandPhaseCompletedLastTimeMs = {};
	std::array<float, RenderCommandPhaseCount> RenderCommandPhaseAverageTimeMs = {};
	std::array<std::deque<float>, RenderCommandPhaseCount> RenderCommandPhaseHistoryMs = {};
	std::array<float, SceneFlushPhaseCount> SceneFlushPhaseLastTimeMs = {};
	std::array<float, SceneFlushPhaseCount> SceneFlushPhaseCompletedLastTimeMs = {};
	std::array<float, SceneFlushPhaseCount> SceneFlushPhaseAverageTimeMs = {};
	std::array<std::deque<float>, SceneFlushPhaseCount> SceneFlushPhaseHistoryMs = {};
	bool bFramePerfLogInitialized = false;
	UINT64 FramePerfLogTotalFrameCount = 0;
	UINT32 FramePerfLogSampleFrameCount = 0;
	double FramePerfLogAccumFrameMs = 0.0;
	double FramePerfLogMinFrameMs = 1.0e30;
	double FramePerfLogMaxFrameMs = 0.0;
	double FramePerfLogAccumCpuUpdateMs = 0.0;
	std::array<double, CpuUpdatePhaseCount> FramePerfLogAccumCpuUpdatePhaseMs = {};
	std::array<double, RenderCommandPhaseCount> FramePerfLogAccumRenderCommandPhaseMs = {};
	std::array<double, SceneFlushPhaseCount> FramePerfLogAccumSceneFlushPhaseMs = {};
	double FramePerfLogAccumBeginFrameMs = 0.0;
	double FramePerfLogAccumRecordMs = 0.0;
	double FramePerfLogAccumExecuteMs = 0.0;
	double FramePerfLogAccumEndFrameMs = 0.0;
	double FramePerfLogAccumRenderWaitMs = 0.0;
	float FramePerfLastFrameMs = 0.0f;
	float FramePerfAverageFrameMs = 0.0f;
	float FramePerfLastBeginFrameMs = 0.0f;
	float FramePerfAverageBeginFrameMs = 0.0f;
	float FramePerfLastRecordMs = 0.0f;
	float FramePerfAverageRecordMs = 0.0f;
	float FramePerfLastExecuteMs = 0.0f;
	float FramePerfAverageExecuteMs = 0.0f;
	float FramePerfLastEndFrameMs = 0.0f;
	float FramePerfAverageEndFrameMs = 0.0f;
	float FramePerfLastRenderWaitMs = 0.0f;
	float FramePerfAverageRenderWaitMs = 0.0f;
	std::array<float, GpuPassCount> CpuPassLastTimeMs = {};
	std::array<double, GpuPassCount> CpuPassAccumTimeMs = {};
	std::array<uint8_t, GpuPassCount> CpuPassActiveMask = {};
	std::array<CpuClock::time_point, GpuPassCount> CpuPassStartTimes = {};
	CpuClock::time_point FramePerfLogFrameStart = {};
	CpuClock::time_point FramePerfLogLastFlush = {};
	void RecompileShaders();
	void BeginFramePerfLogging();
	void FinishFramePerfLogging(double beginFrameMs, double executeMs, double endFrameMs, double renderWaitMs);
	void InitGpuTimingResources();
	void BeginGpuTimingFrame();
	void ResolveGpuTimingFrame();
	void UpdateGpuTimingReadback();
	void BeginGpuPassTiming(EGpuPass pass);
	void EndGpuPassTiming(EGpuPass pass);
	const char* GetGpuPassName(EGpuPass pass) const;
	const char* GetCpuUpdatePhaseName(ECpuUpdatePhase phase) const;
	const char* GetRenderCommandPhaseName(ERenderCommandPhase phase) const;
	const char* GetSceneFlushPhaseName(ESceneFlushPhase phase) const;
	void AddRenderCommandPhaseTiming(ERenderCommandPhase phase, const CpuClock::time_point& begin, const CpuClock::time_point& end);
	void AddSceneFlushPhaseTiming(ESceneFlushPhase phase, const CpuClock::time_point& begin, const CpuClock::time_point& end);
	void AddCpuUpdatePhaseTiming(ECpuUpdatePhase phase, const CpuClock::time_point& begin, const CpuClock::time_point& end);
	void FinishCpuUpdateTiming(const CpuClock::time_point& begin, const CpuClock::time_point& end);
	void TrimCpuUpdateTimingHistory();
	std::wstring BuildFinalScreenshotPath();
	void RequestFinalBackbufferScreenshot();
	void ConsumeFinalBackbufferScreenshotResult();
	std::wstring GetCameraPathDirectory() const;
	std::wstring GetCameraPathDumpDirectory() const;
	CameraPathKeyframe CaptureCurrentCameraPathKeyframe(double timeSeconds) const;
	CameraPathKeyframe SampleCameraPath(double timeSeconds) const;
	void ApplyCameraPathKeyframe(const CameraPathKeyframe& keyframe);
	void StartCameraPathRecording();
	void EndCameraPathRecording();
	std::wstring AllocateUniqueCameraPathFilePath(const wchar_t* tag) const;
	bool WriteCameraPathFile(const std::wstring& filePath, bool bUpdateActivePath);
	bool SaveCameraPath(const std::wstring& filePath);
	bool LoadCameraPath(const std::wstring& filePath);
	void RefreshCameraPathList();
	bool EnsureCameraPathSavedForDump(const std::filesystem::path& dumpDir);
	bool LoadSelectedCameraPath();
	bool LoadLatestCameraPath();
	void StartCameraPathPlayback();
	void StopCameraPathPlayback();
	void StartCameraPathDump();
	void StopCameraPathDump();
	void UpdateCameraPathState();
	std::wstring BuildCameraPathFrameDumpPath() const;
	void DumpCameraPathDiagnosticFrame();
	void RequestCameraPathDumpFrameCapture();
	void ConsumeCameraPathDumpCaptureResult();
	void LaunchCameraPathVideoEncode();
	double GetCameraPathDurationSeconds() const;

	struct RTSceneHitProgramDesc
	{
		RTSceneHitProgramDesc()
			: HitGroup("HitGroup")
			, bBindSceneGeometry(true)
			, bBindDiffuseTexture(true)
			, bBindInstanceProperty(true)
			, bBindInstancePropertyBeforeDiffuse(false)
		{
		}

		const char* HitGroup;
		bool bBindSceneGeometry;
		bool bBindDiffuseTexture;
		bool bBindInstanceProperty;
		bool bBindInstancePropertyBeforeDiffuse;
	};

	class RTPassBuilder
	{
	public:
		using HitProgramBinder = std::function<void(RTPipelineStateObject& pso, const RTSceneHitProgramDesc& desc, Mesh& mesh, uint32_t instanceIndex)>;

		RTPassBuilder(Corona& owner, const shared_ptr<RTPipelineStateObject>& pso);

		bool IsValid() const;
		RTPassBuilder& BeginScene();
		RTPassBuilder& SetTextureUAV(const char* shader, const char* bindingName, Texture* texture);
		RTPassBuilder& SetBufferUAV(const char* shader, const char* bindingName, Buffer* buffer);
		RTPassBuilder& SetTextureSRV(const char* shader, const char* bindingName, Texture* texture);
		RTPassBuilder& SetBufferSRV(const char* shader, const char* bindingName, Buffer* buffer);
		RTPassBuilder& SetAccelerationStructure(const char* shader, const char* bindingName, const shared_ptr<RTAS>& rtas);
		RTPassBuilder& SetSampler(const char* shader, const char* bindingName, Sampler* sampler);
		RTPassBuilder& SetCBVValue(const char* shader, const char* bindingName, void* data);
		uint32_t BindSceneHitPrograms(const RTSceneHitProgramDesc& desc = RTSceneHitProgramDesc(), const HitProgramBinder& customBinder = HitProgramBinder());
		void Dispatch(uint32_t width, uint32_t height);

		Texture* GetDiffuseTexture(const Mesh& mesh) const;
		Texture* GetNormalTexture(const Mesh& mesh) const;
		Texture* GetRoughnessTexture(const Mesh& mesh) const;
		Texture* GetMetallicTexture(const Mesh& mesh) const;

	private:
		Material* GetPrimaryMaterial(const Mesh& mesh) const;

		Corona& Owner;
		shared_ptr<RTPipelineStateObject> PSO;
		bool bBegan = false;
	};
	
	// Raytracing helper functions
	glm::mat4x4 BuildCenteredSceneTransform(
		const shared_ptr<Scene>& scene,
		float targetExtent,
		const glm::vec3& position = glm::vec3(0.0f),
		const glm::vec3& rotationDegrees = glm::vec3(0.0f)) const;
	glm::mat4x4 BuildScaledSceneTransform(
		const shared_ptr<Scene>& scene,
		const glm::vec3& scale,
		const glm::vec3& position = glm::vec3(0.0f),
		const glm::vec3& rotationDegrees = glm::vec3(0.0f)) const;
	SceneObjectHandle AddCenteredSceneObject(
		const shared_ptr<Scene>& scene,
		float targetExtent,
		float roughness,
		float metallic,
		bool bOverrideRoughnessMetallic,
		const glm::vec3& position = glm::vec3(0.0f),
		const glm::vec3& rotationDegrees = glm::vec3(0.0f));
	shared_ptr<Scene> CreateMirrorCubeScene();
	shared_ptr<Texture> GetProceduralDungeonBrickDiffuseTexture();
	shared_ptr<Texture> GetProceduralBoxDiffuseTexture(const std::wstring& textureKind);
	shared_ptr<Scene> CreateProceduralBoxScene(const glm::vec3& baseColor, bool bUseBrickTexture = false, float uvRepeat = 1.0f, const std::wstring& textureKind = std::wstring(), float uvRepeatY = -1.0f, bool bFrontOnly = false);
	bool ShouldIncludeSceneObjectInRayTracingAS(const SceneObject& object) const;
	void MarkRayTracingSceneDirty();
	void MarkRayTracingTransformsDirty();
	void FlushSceneObjectChanges();
	UINT32 GetRayTracingFrameResourceIndex() const;
	void EnsureRayTracingFrameResourceSlots();
	void ActivateCurrentRayTracingFrameResources();
	bool IsCurrentRayTracingFrameResourceReady() const;
	void UpdateRayTracingInstanceTransforms();
	void UpdateInstancePropertyBuffer();
	void RebuildAccelerationStructures();
	void InitCpuPhysics();
	void ShutdownCpuPhysics();
	void MarkCpuPhysicsSceneDirty();
	void RebuildCpuPhysicsScene();
	bool CpuPhysicsRaycast(
		const glm::vec3& origin,
		const glm::vec3& direction,
		float maxDistance,
		CpuPhysicsRaycastHit& hit);
	bool CpuPhysicsSphereSweep(
		const glm::vec3& origin,
		float radius,
		const glm::vec3& direction,
		float maxDistance,
		CpuPhysicsRaycastHit& hit);
	glm::vec3 ResolveCameraPhysicsMovement(
		const glm::vec3& startPosition,
		const glm::vec3& desiredPosition);
	void InitializeWorldEntity();
	void InitializeLevelEntity();
	CoronaECS::Entity CreateSceneObjectEntity(const SceneObject& object);
	void UpdateSceneObjectEntity(const SceneObject& object);
	void InitializeMainCameraEntity();
	void UpdateMainCameraEntityFromSimpleCamera();
	void InitializeMainDirectionalLightEntity();
	void UpdateMainDirectionalLightEntityFromState();
	void ApplyDirectionalLightEntityToState();
	CoronaECS::Entity CreatePointLightEntity(PointLightState& pointLight);
	void UpdatePointLightEntity(const PointLightState& pointLight);
	void DestroyPointLightEntity(PointLightState& pointLight);
	PointLightState* FindPointLightByEntity(CoronaECS::Entity entity);
	const PointLightState* FindPointLightByEntity(CoronaECS::Entity entity) const;
	void InitRaytracingShadowPass();
	void InitRaytracingReflectionPass();
	shared_ptr<RTPipelineStateObject> CreateRaytracingReflectionPSO(bool bUseSER);
	bool InitRaytracingReflectionSERPass();
	void InitRaytracingSimpleGIPass();
	shared_ptr<RTPipelineStateObject> CreateRaytracingSimpleGIPSO(bool bUseSER);
	bool InitRaytracingSimpleGISERPass();
	void InitRaytracingScreenProbePass();
	shared_ptr<RTPipelineStateObject> CreateRaytracingScreenProbeGIPSO(bool bUseSER);
	bool InitRaytracingScreenProbeGISERPass();
	void InitRaytracingSpatialHashPass();
	shared_ptr<RTPipelineStateObject> CreateRaytracingSpatialHashGIPSO(bool bUseSER);
	bool InitRaytracingSpatialHashGISERPass();
	
public:

	void InitRaytracingData();
	CoronaECS::Entity CreateEntity(const std::string& name = std::string());
	CoronaECS::Entity GetSceneObjectEntity(SceneObjectHandle handle) const;
	SceneObjectHandle GetEntitySceneObject(CoronaECS::Entity entity) const;
	CoronaECS::EntityComponentSystem& GetEntityWorld();
	const CoronaECS::EntityComponentSystem& GetEntityWorld() const;
	SceneObjectHandle AddSceneObject(const SceneObjectDesc& desc);
	SceneObjectHandle AddSceneInstance(const shared_ptr<Scene>& scene, const glm::mat4x4& transform);
	bool RemoveSceneObject(SceneObjectHandle handle);
	bool SetSceneObjectTransform(SceneObjectHandle handle, const glm::mat4x4& transform);
	bool SetSceneObjectVisibility(SceneObjectHandle handle, bool visible);
	bool SetSceneObjectRayTracingEnabled(SceneObjectHandle handle, bool enabled);
	bool CpuPhysicsRaycastForScript(
		const glm::vec3& origin,
		const glm::vec3& direction,
		float maxDistance,
		CpuPhysicsRaycastHit& hit);
	ScriptSceneHandle CreateProceduralBlockCharacterSceneForScript(UINT32 seed);
	ScriptSceneHandle CreateProceduralBoxSceneForScript(const glm::vec3& baseColor, bool bUseBrickTexture = false, float uvRepeat = 1.0f, const std::wstring& textureKind = std::wstring(), float uvRepeatY = -1.0f, bool bFrontOnly = false);
	ScriptSceneHandle CreateSpineSceneForScript(const std::wstring& assetPath, const std::string& animationName, float timeSeconds, float sourceScale = 1.0f);
	// Phase 2: SpineClipFrameCache management. PrewarmSpineClipFrameForScript
	// fills the CPU-side cache without creating any GPU vertex/index buffers
	// (matches the doc's "nonblocking miss path" requirement). The cache is
	// keyed by quantized (asset + animation + sample-frame + scale), and the
	// other helpers report and bound its memory footprint.
	bool PrewarmSpineClipFrameForScript(const std::wstring& assetPath, const std::string& animationName, float timeSeconds, float sourceScale = 1.0f);
	size_t GetSpineClipFrameCacheMemoryBytes() const;
	size_t GetSpineClipFrameCacheEntryCount() const;
	size_t GetSpineClipFrameCacheMemoryBudgetBytes() const;
	void SetSpineClipFrameCacheMemoryBudgetBytes(size_t budgetBytes);
	void ResetSpineClipFrameCache();
	float GetScriptSceneHeightForScript(ScriptSceneHandle sceneHandle) const;
	bool SetSpinePoseForScript(
		CoronaECS::Entity entity,
		ScriptSceneHandle sceneHandle,
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		float targetHeight,
		float roughness,
		float metallic,
		bool bMirrorX,
		bool bUseWorldScale = false,
		bool bRayTracing = false);
	ScriptSceneHandle LoadSceneForScript(const std::wstring& assetPath);
	SceneObjectHandle SpawnSceneObjectForScript(
		ScriptSceneHandle sceneHandle,
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		float targetExtent,
		const glm::vec3& scale,
		bool bUseScale,
		float roughness,
		float metallic,
		bool bOverrideRoughnessMetallic,
		bool bVisible,
		bool bRayTracing,
		bool bPhysicsQuery);
	CoronaECS::Entity SpawnEntityForScript(
		ScriptSceneHandle sceneHandle,
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		float targetExtent,
		const glm::vec3& scale,
		bool bUseScale,
		float roughness,
		float metallic,
		bool bOverrideRoughnessMetallic,
		bool bVisible,
		bool bRayTracing,
		bool bPhysicsQuery);
	bool SetSceneObjectTransformForScript(
		SceneObjectHandle handle,
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		float targetExtent,
		const glm::vec3& scale,
		bool bUseScale);
	bool GetSceneObjectTransformForScript(
		SceneObjectHandle handle,
		glm::vec3& position,
		glm::vec3& rotationDegrees,
		float& targetExtent,
		glm::vec3& scale,
		bool& bUseScale) const;
	bool AddMeshComponentForScript(
		CoronaECS::Entity entity,
		ScriptSceneHandle sceneHandle,
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		float targetExtent,
		const glm::vec3& scale,
		bool bUseScale,
		float roughness,
		float metallic,
		bool bOverrideRoughnessMetallic,
		bool bVisible,
		bool bRayTracing,
		bool bPhysicsQuery);
	bool SetEntityMeshTransformForScript(
		CoronaECS::Entity entity,
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		float targetExtent,
		const glm::vec3& scale,
		bool bUseScale);
	bool SetEntityMeshComponentForScript(
		CoronaECS::Entity entity,
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		float targetExtent,
		const glm::vec3& scale,
		bool bUseScale,
		float roughness,
		float metallic,
		bool bOverrideRoughnessMetallic,
		bool bVisible,
		bool bRayTracing,
		bool bPhysicsQuery);
	bool GetEntityMeshForScript(
		CoronaECS::Entity entity,
		CoronaECS::MeshComponent& component) const;
	CoronaECS::Entity GetWorldEntityForScript();
	CoronaECS::Entity GetLevelEntityForScript();
	CoronaECS::Entity GetMainCameraEntityForScript() const;
	CoronaECS::Entity GetMainDirectionalLightEntityForScript();
	CoronaECS::Entity SpawnPointLightEntityForScript(
		const glm::vec3& position,
		float radius,
		const glm::vec3& color,
		float intensity,
		bool bEnabled);
	bool SetEntityTransformForScript(
		CoronaECS::Entity entity,
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		const glm::vec3& scale);
	bool GetEntityTransformForScript(
		CoronaECS::Entity entity,
		glm::vec3& position) const;
	bool SetEntityPhysicsForScript(
		CoronaECS::Entity entity,
		bool bQueryEnabled,
		CoronaECS::PhysicsCollisionShape collisionShape,
		const glm::vec3& boxHalfExtent);
	bool GetEntityPhysicsForScript(
		CoronaECS::Entity entity,
		CoronaECS::PhysicsComponent& component) const;
	bool SetEntityLightForScript(
		CoronaECS::Entity entity,
		const CoronaECS::LightComponent& component,
		bool bPersistSceneState = true);
	bool GetEntityLightForScript(
		CoronaECS::Entity entity,
		CoronaECS::LightComponent& component) const;
	bool SetEntityCameraComponentForScript(
		CoronaECS::Entity entity,
		const CoronaECS::CameraComponent& component);
	bool GetEntityCameraComponentForScript(
		CoronaECS::Entity entity,
		CoronaECS::CameraComponent& component) const;
	bool SetEntityVisibilityForScript(CoronaECS::Entity entity, bool visible);
	bool SetEntityRayTracingForScript(CoronaECS::Entity entity, bool enabled);
	bool DestroyEntityForScript(CoronaECS::Entity entity);
	bool AttachEntityScriptForScript(
		CoronaECS::Entity entity,
		int updateRef,
		int shutdownRef,
		int imguiRef,
		int uiRef,
		const std::wstring& sourceName,
		bool bPassEntityToCallbacks = true);
	bool AttachEntityScriptFileForScript(
		CoronaECS::Entity entity,
		const std::wstring& scriptPath);
	void RegisterNativeEntityScript(
		const std::string& name,
		NativeEntityScriptCallbacks callbacks);
	bool HasNativeEntityScript(const std::string& name) const;
	bool AttachNativeEntityScriptForScript(
		CoronaECS::Entity entity,
		const std::string& nativeScriptName);
	bool DetachEntityScriptForScript(CoronaECS::Entity entity);
	void RecordScriptFunctionProfile(
		const std::wstring& sourceName,
		const std::string& callbackName,
		double elapsedMs,
		bool bNative);
	void PublishScriptProfileFrame();
	void RecordLuauScriptProfileSample(lua_State* L, int gcState);
	void BeginLuauScriptProfileExecution();
	void EndLuauScriptProfileExecution();
	void ResetScriptProfileStats();
	bool DumpScriptProfileStats();
	void PushScriptProfileStatsForScript(lua_State* L);
	void PushScriptProfileSamplesForScript(lua_State* L);
	bool SetDefaultWorldVisibleForScript(bool visible);
	bool SetScriptCameraControlForScript(bool enabled);
	bool SetCameraForScript(
		const glm::vec3& position,
		const glm::vec3& lookAt,
		const glm::vec3& upDirection);
	void GetCameraForScript(
		glm::vec3& position,
		glm::vec3& forward,
		glm::vec3& right,
		glm::vec3& up,
		float& yawDegrees,
		float& pitchDegrees) const;
	void RecordScriptKeyDown(UINT8 key);
	void RecordScriptKeyUp(UINT8 key);
	void RecordScriptRButtonDown(int x, int y);
	void RecordScriptRButtonUp();
	void RecordScriptMouseMove(int x, int y);
	void PollScriptMouseState();
	void PollScriptGamepadState();
	bool IsScriptKeyDownForScript(UINT8 key) const;
	bool WasScriptKeyPressedForScript(UINT8 key) const;
	bool WasScriptKeyReleasedForScript(UINT8 key) const;
	void GetScriptMouseForScript(
		int& x,
		int& y,
		int& deltaX,
		int& deltaY,
		bool& rightDown,
		bool& rightPressed,
		bool& rightReleased) const;
	void GetScriptGamepadForScript(
		bool& connected,
		float& leftX,
		float& leftY,
		float& rightX,
		float& rightY,
		float& leftTrigger,
		float& rightTrigger,
		uint16_t& buttonsDown,
		uint16_t& buttonsPressed,
		uint16_t& buttonsReleased) const;
	void ClearScriptInputFrameState();
	void InitLuauScripting();
	void RunStartupLuauScript(bool bShowLoadingProgress = true);
	bool LoadLuauScriptFile(const std::filesystem::path& scriptPath);
	void CallLuauShutdownCallbacks();
	void CallEntityScriptShutdownCallbacks();
	void ReloadLuauScripting();
	void UpdateLuauScripting(float dt);
	void UpdateEntityScripts(float dt);
	void BuildLuauUi();
	void BuildEntityScriptUi();
	void RenderQueuedLuauUi(bool bRenderToolUi = true, bool bRenderGameUi = true);
	void DrawLuauImGui();
	void DrawEntityScriptImGui();
	void ShutdownLuauScripting();
	void StartLuauScriptProfileSampler(lua_State* L);
	void StopLuauScriptProfileSampler();
	void ScriptProfileSamplerLoop();
	void DestroyEntityScriptComponent(CoronaECS::Entity entity);
	bool IsLuauImGuiFrameActive() const;
	void QueueScriptUiCommandForScript(ScriptUiCommand command);
	void QueueScriptUiSeparatorForScript();
	void QueueScriptUiTextForScript(const std::string& text);
	void QueueScriptUiSameLineForScript();
	void QueueScriptUiBeginWindowForScript(const std::string& title);
	void QueueScriptUiEndWindowForScript();
	void QueueScriptUiOverlayTextForScript(const std::string& text, float x, float y);
	void QueueScriptUiOverlayLineForScript(
		float x0,
		float y0,
		float x1,
		float y1,
		const glm::vec4& color,
		float thickness);
	void QueueScriptUiOverlayRectForScript(
		float x,
		float y,
		float width,
		float height,
		const glm::vec4& color,
		float thickness,
		float rounding);
	void QueueScriptUiOverlayRectFilledForScript(
		float x,
		float y,
		float width,
		float height,
		const glm::vec4& color,
		float rounding);
	bool QueueScriptUiOverlayButtonForScript(
		const std::string& id,
		const std::string& label,
		float x,
		float y,
		float width,
		float height,
		const glm::vec4& fillColor,
		const glm::vec4& hoverColor,
		const glm::vec4& borderColor,
		float rounding);
	void QueueScriptUiOverlayProgressBarForScript(
		const std::string& id,
		float x,
		float y,
		float width,
		float height,
		float fraction,
		const std::string& label,
		const glm::vec4& fillColor,
		const glm::vec4& backgroundColor,
		const glm::vec4& borderColor);
	void QueueScriptUiWorldAxisForScript(
		const std::string& id,
		const glm::vec3& position,
		float length,
		float thickness);
	void QueueScriptUiWorldTextForScript(
		const std::string& id,
		const glm::vec3& position,
		const std::string& text,
		float xOffset,
		float yOffset,
		const glm::vec4& color);
	void QueueScriptUiWorldProgressBarForScript(
		const std::string& id,
		const glm::vec3& position,
		float fraction,
		const std::string& label,
		float width,
		float height,
		const glm::vec4& fillColor,
		const glm::vec4& backgroundColor,
		const glm::vec4& borderColor);
	void QueueScriptUiWorldHealthBarForScript(
		const std::string& id,
		const glm::vec3& position,
		float fraction,
		const std::string& label,
		float width,
		float height);
	glm::vec3 QueueScriptUiGizmo3DForScript(
		const std::string& id,
		const std::string& label,
		const glm::vec3& value,
		float size,
		int mode);
	bool QueueScriptUiButtonForScript(const std::string& id, const std::string& label);
	float QueueScriptUiSliderFloatForScript(
		const std::string& id,
		const std::string& label,
		float value,
		float minValue,
		float maxValue);
	int QueueScriptUiSliderIntForScript(
		const std::string& id,
		const std::string& label,
		int value,
		int minValue,
		int maxValue);
	int QueueScriptUiComboForScript(
		const std::string& id,
		const std::string& label,
		int selectedIndex,
		const std::vector<std::string>& items);
	bool QueueScriptUiCheckboxForScript(const std::string& id, const std::string& label, bool value);
	void PushLuauUiStateForScript(lua_State* L, const std::string& mode = std::string());
	bool SetLuauUiValueForScript(const std::string& name, lua_State* L, int valueIndex);
	bool RunLuauUiCommandForScript(const std::string& name, lua_State* L, int argIndex);
	void PushPersistentScriptControlForScript(lua_State* L, const std::string& name, int defaultIndex);
	bool SetPersistentScriptControlForScript(const std::string& name, lua_State* L, int valueIndex);
	

	void LoadPipeline();

	void LoadAssets();

	shared_ptr<Scene> LoadModel(string fileName);
	shared_ptr<Scene> CreateProceduralBlockCharacterScene(UINT32 seed);
	shared_ptr<Scene> LoadBinaryMeshModel(const std::wstring& binaryFileName, const std::wstring& sourceFileName);

	void InitRTPSO();

	void InitTemporalDenoisingPass();
	void InitScreenProbeGIPass();
	void InitSpatialHashGIPass();

	void InitGBufferPass();

	void InitToneMapPass();

	void InitDebugPass();

	void InitLightingPass();
	void InitMobileShadowMapPass();

	void InitTemporalAAPass();

	void InitBloomPass();

	void InitGenMipSpecularGIPass();

	void InitImgui();
	void DrawMobileVirtualControls();
	void DrawMobilePerformanceOverlay();
	void UpdateMobileVirtualMoveFromTouch(const PlatformTouchState& touchState);
	bool LoadCameraState();
	void SaveCameraState();
	std::wstring GetCameraStatePath();
	bool LoadSceneState();
	void SaveSceneState();
	std::wstring GetSceneStatePath();

	void InitBlueNoiseTexture();

	void DrawScene(shared_ptr<Scene> scene, const glm::mat4x4& instanceTransform, float Roughness, float Metalic, bool bOverrideRoughnessMetallic);
	void DrawSceneShadowMap(shared_ptr<Scene> scene, const glm::mat4x4& instanceTransform);
	void DispatchSpineSkinningForMesh(Mesh* mesh);
	void DispatchSpineSkinningForScene(const shared_ptr<Scene>& scene);
	void DispatchSpineSkinningForRenderWorld();

	// 3D skeletal skinning entry points (implemented in Corona.Skeletal.cpp).
	// Spawn helpers create procedural box characters for Sponza-mode testing.
	void InitSkeletalSkinningPSO();
	void DispatchSkeletalSkinningForRenderWorld();
	void SpawnSkeletalTestCharacters();
	void UpdateSkeletalTestCharacters(float timeSeconds);
	void DumpSkeletalFrameStatsToTrace();
	bool BuildMobileShadowViewProjection(glm::mat4x4& lightViewProj);
	bool GetSceneObjectWorldBounds(const SceneObject& object, glm::vec3& boundsMin, glm::vec3& boundsMax, glm::vec3& center, float& radius) const;
	bool IsWorldAabbInViewFrustum(const glm::vec3& boundsMin, const glm::vec3& boundsMax) const;
	void PrepareGBufferCulling(uint32_t sceneObjectCount);
	bool ShouldDrawSceneObjectInGBuffer(const SceneObject& object, const glm::vec3& boundsCenter, float boundsRadius);
	uint32_t BeginGBufferOcclusionQuery(SceneObjectHandle handle);
	void EndGBufferOcclusionQuery(SceneObjectHandle handle, uint32_t queryIndex);
	void FinishGBufferCulling();

	void GBufferPass();
	void MobileShadowMapPass();

	void RaytraceShadowPass();

	void InitRaytracingAOPass();
	void RaytraceAOPass();
	void InitRaytracingSkyLightingPass();
	void RaytraceSkyLightingPass();

	void RaytraceReflectionPass();

	void RaytraceGIPass();
	void SpatialHashGIPass();
	void ScreenProbeRaytraceGIPass();
	void ScreenProbeGIPass();

	void TemporalDenoisingPass();

	void BloomPass();

	void InitPathTracingPass();

	void PathTracingPass();
	void ApplyHybridDefaultCamera();
	void ApplySponzaFlyCamera();
	void EnsureWindowFramebuffers();

	void ToneMapPass();

	void DebugPass();

	void LightingPass();

	void TemporalAAPass();

	bool IsTemporalAAEnabled() const { return AntiAliasingMode == EAntiAliasingMode::TAA; }
	bool IsDLSSSREnabled() const { return AntiAliasingMode == EAntiAliasingMode::DLSS_SR && bDLSSAvailable; }
	bool IsDLSSRREnabled() const { return AntiAliasingMode == EAntiAliasingMode::DLSS_RR && bDLSSRRAvailable; }
	bool IsPathTracingDLSSRREnabled() const { return RenderingMode == ERenderingMode::PATHTRACING && bEnablePathTracingDLSSRR && IsDLSSRREnabled(); }
	bool IsDLSSUpscaleEnabled() const { return RenderingMode == ERenderingMode::HYBRID && (IsDLSSSREnabled() || IsDLSSRREnabled()); }
	bool IsJitterEnabled() const { return IsTemporalAAEnabled() || IsDLSSUpscaleEnabled(); }
	UINT GetRenderWidth() const
	{
#if CORONA_PLATFORM_MOBILE
		return RenderWidth > 0 ? RenderWidth : m_width;
#else
		return IsDLSSUpscaleEnabled() ? RenderWidth : m_width;
#endif
	}
	UINT GetRenderHeight() const
	{
#if CORONA_PLATFORM_MOBILE
		return RenderHeight > 0 ? RenderHeight : m_height;
#else
		return IsDLSSUpscaleEnabled() ? RenderHeight : m_height;
#endif
	}
	UINT GetWidth() const { return m_width; }
	UINT GetHeight() const { return m_height; }
	const WCHAR* GetTitle() const { return m_title.c_str(); }
	double GetTargetFrameRateLimitHz() const;

	void GenMipSpecularGIPass();
	EAntiAliasingMode NormalizeAntiAliasingMode(ERenderingMode renderingMode, EAntiAliasingMode requestedMode) const;
	void ApplyRenderingAndAAMode(ERenderingMode requestedRenderingMode, EAntiAliasingMode requestedAAMode);
	bool RenderResolutionResourcesMatchCurrentState() const;
	void ResetAllAccumulationState(bool forceUpscaleReload);
	void ResetTemporalHistoryBuffers();
	void RecreateRenderResolutionResources();
	void ReloadRenderResolutionAssets();
	void RefreshUpscaleSettings(bool reloadAssets);
	Texture* GetCurrentResolveSource() const;
	void PromptStartupModeSelection();
	void InitializeAutoAADump();
	bool AdvanceMobileGBufferDump(Texture* backbuffer);
	void AdvanceAutoAADump(Texture* backbuffer);
	void AppendAutoAADumpLog(const std::wstring& line);
	bool IsHybridStageAutoDumpPhase() const;
	const wchar_t* GetHybridStageAutoDumpPhaseName(uint32_t phase) const;
	bool DumpTextureHDR(Texture* source, const std::wstring& filePath, EResourceState beforeState);
	bool DumpTexturePNG(Texture* source, const std::wstring& filePath, EResourceState beforeState);
	bool DumpTextureRawFloat(Texture* source, const std::wstring& filePath, EResourceState beforeState, ERawFloatDumpFormat targetFormat, uint32_t channelCount);
	bool StartAsyncImageDumpWorkers();
	void AsyncImageDumpWorkerMain();
	void WaitForAsyncImageDumps();
	void StopAsyncImageDumpWorkers();
	bool EnqueueAsyncImageDump(CapturedImage&& captured, const std::wstring& filePath, bool bHDR);
	void ProcessRenderThreadRequests();
#if WITH_STREAMLINE
	void InitStreamline();
	void ShutdownStreamline();
	bool BeginStreamlineFrame();
	bool EnsureStreamlineConstants();
	bool DLSSPass();
	bool DLSSRRPass();
#endif

	void OnInit();
	void UpdateStartupLoadingProgress(float progress, const std::wstring& status);
	void DrawStartupLoadingScreen();
	bool IsStartupLoadingScreenActive() const { return bStartupLoadingScreenActive; }

	void OnUpdate();
	void UpdateMobileTouchCameraInput(float elapsedSeconds);

	void OnRender();

	void StartGameThread();
	void StopGameThread();
	void RenderThreadTick();
	void GameThreadMain();

	void OnDestroy();

	void ParseCommandLineArgs(_In_reads_(argc) WCHAR* argv[], int argc);

	void OnKeyDown(UINT8 key);

	void OnKeyUp(UINT8 key);
	
	void OnRButtonDown(int x, int y);
	virtual void OnRButtonUp();
	virtual void OnMouseMove(int x, int y);

	Corona(UINT width, UINT height, std::wstring name);

	~Corona();

private:
	std::wstring GetAssetFullPath(LPCWSTR assetName) const;
	void SetCustomWindowText(LPCWSTR text);
	void PumpStartupWindowMessages();
	void InitRenderSyncChannels();
	void MarkSceneObjectRenderDirty(SceneObjectHandle handle, UINT32 dirtyBits);
	void MarkSceneObjectRenderRemoved(SceneObjectHandle handle);
	void MarkAllSceneObjectsForRenderSync();
	void MarkPointLightRenderDirty(UINT32 id, UINT32 dirtyBits);
	void MarkPointLightRenderRemoved(UINT32 id);
	void MarkAllPointLightsForRenderSync();
	void CollectRenderFrameDeltas();
	void PublishRenderFrameDelta(RenderFrameDelta&& delta);
	void ApplyPendingRenderFrameDeltas();
	RenderFrameSourceState CaptureRenderFrameSourceState() const;
	void ApplyRenderFrameSourceState(const RenderFrameSourceState& state);
	void CollectFrameSourceRenderSync(RenderFrameDelta& delta);
	void ApplyFrameSourceRenderSync(const RenderFrameDelta& delta);
	void CollectSceneObjectRenderSync(RenderFrameDelta& delta);
	void ApplySceneObjectRenderSync(const RenderFrameDelta& delta);
	void CollectPointLightRenderSync(RenderFrameDelta& delta);
	void ApplyPointLightRenderSync(const RenderFrameDelta& delta);
	void BuildRenderFrameDerivedState(const RenderFrameSourceState* sourceState);
	void ApplyRenderPointLightsToFrameParams();

	UINT m_width = 0;
	UINT m_height = 0;
	float m_aspectRatio = 1.0f;
	bool m_useWarpDevice = false;
	std::wstring m_assetsPath;
	std::wstring m_title;
	std::string m_imguiIniPath;
	std::string m_imguiLogPath;
	float StartupLoadingProgress = 0.0f;
	std::wstring StartupLoadingStatus;
	CpuClock::time_point StartupLoadingTimingStart = {};
	CpuClock::time_point StartupLoadingTimingLast = {};
	std::wstring StartupLoadingTimingLastStatus;
	bool bStartupLoadingTimingStarted = false;
	bool bStartupLoadingScreenActive = false;
	std::mutex GameRenderStateMutex;
	std::mutex GameThreadMutex;
	std::condition_variable GameThreadCv;
	std::thread GameThread;
	bool bSplitGameRenderThreads = true;
	bool bGameThreadStarted = false;
	bool bGameThreadStopRequested = false;
	bool bGameFrameReady = false;
};
