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
namespace Terrain { class Component; }
namespace Particles { class System; }
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

// LLM-driven motion playback (Corona.MotionClip.*, Corona.MotionPlayback.*,
// Corona.SmplCharacter.*). Forward-declared so Corona.h stays lightweight;
// Corona.cpp + Corona.SmplCharacter.cpp include the real headers.
namespace CoronaMotion { struct MotionClip; class MotionPlayback; }
namespace CoronaSmpl   { struct CharacterResources; }

enum class ERawFloatDumpFormat
{
	Unknown,
	R32Float,
	R32G32Float,
	R32G32B32A32Float,
};

class Corona
{
	friend class CoronaConsole; // accesses LoadModel + AddCenteredSceneObject + camera state
	friend class CoronaSceneInspector; // accesses EntityWorld + render-frame state for the entity list panel
	friend class CoronaAssetExplorer; // accesses LoadModel / AddCenteredSceneObject for asset right-click spawn
	friend class CoronaToolbox; // accesses CreateProceduralBoxSceneForScript + AddMeshComponentForScript
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
		SkeletalSkinning,
		GBuffer,
		Terrain,
		Grass,
		ProceduralGrass,
		Particles,
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
		SkeletalUpdate,
		SkeletalPalette,
		SkeletalPack,
		SkeletalUpload,
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

	enum class ERtRecordPhase : UINT32
	{
		BeginScene = 0,
		BindResources,
		BindHitPrograms,
		EndShaderTable,
		ApplyDispatch,
		Count
	};

	static constexpr UINT32 GpuPassCount = static_cast<UINT32>(EGpuPass::Count);
	static constexpr UINT32 CpuUpdatePhaseCount = static_cast<UINT32>(ECpuUpdatePhase::Count);
	static constexpr UINT32 RenderCommandPhaseCount = static_cast<UINT32>(ERenderCommandPhase::Count);
	static constexpr UINT32 SceneFlushPhaseCount = static_cast<UINT32>(ESceneFlushPhase::Count);
	static constexpr UINT32 RtRecordPhaseCount = static_cast<UINT32>(ERtRecordPhase::Count);
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
	// ReSTIR GI on specular RT path — per-pixel reservoir storage.
	// `*A`: .xyz = chosen sample's hit world position, .w = M
	// (effective sample count). `*B`: .xyz = radiance leaving the
	// hit toward the pixel (LINEAR, pre-Reinhard), .w = W (RIS
	// weight). Prev buffers receive end-of-frame CopyResource so
	// the next frame can RIS-combine via motion reprojection.
	// Sized to match the raytraced reflection output (full render
	// resolution). RGBA32Float used throughout for unbiased combine
	// math — packing to FP16 would lose hit-position precision.
	shared_ptr<Texture> ReflectionReservoirA;
	shared_ptr<Texture> ReflectionReservoirB;
	shared_ptr<Texture> ReflectionReservoirAPrev;
	shared_ptr<Texture> ReflectionReservoirBPrev;
	// SpatialHashGI Option A — screen-space per-pixel resolve layer
	// (bilateral spatial filter + motion-reprojected temporal with
	// depth+normal disocclusion gate). Mirrors what ScreenProbe does
	// downstream of its world cache. `Filtered` = current frame
	// output (LightingPS reads). `FilteredPrev` = end-of-frame
	// snapshot for next frame's temporal blend.
	shared_ptr<Texture> DiffuseGIHashFiltered;
	shared_ptr<Texture> DiffuseGIHashFilteredPrev;
	shared_ptr<Texture> ShadowBuffer;
	// ReSTIR DI Phase 2 — previous-frame reservoir cache. Copied from
	// ShadowBuffer at the end of RaytraceShadowPass so the next frame can
	// reproject and combine. RGBA32Float layout matches ShadowBuffer
	// (.r = sun visibility, .gba = reservoir lightIdx / weight / vis).
	shared_ptr<Texture> ShadowReservoirPrevBuffer;
	// Phase 2b — per-pixel effective sample count M. Lives in a separate
	// single-channel texture because ShadowBuffer's 4 RGBA32F slots are
	// fully spoken for (sun / idx / W / vis). R16Float gives ample range
	// for the M cap (~20). Curr is written by the RT shadow shader; Prev
	// is the CopyResource'd snapshot used by the next frame's combine.
	shared_ptr<Texture> ShadowReservoirMBuffer;
	shared_ptr<Texture> ShadowReservoirMPrevBuffer;
	// Phase 3 proper 2-pass scratch buffers. The RT raygen writes here
	// (Phase 1 RIS + Phase 2 temporal only); the spatial-reuse compute
	// pass reads these and writes the final ShadowBuffer +
	// ShadowReservoirMBuffer that LightingPS consumes. Splitting the
	// pass lets the spatial neighbour sampling see CURRENT-frame
	// reservoirs (not 1-frame stale prev) and re-trace visibility via
	// RayQuery so the spatially-chosen light's shadow is correct at
	// this pixel.
	shared_ptr<Texture> ShadowBufferPreSpatial;
	shared_ptr<Texture> ShadowReservoirMBufferPreSpatial;
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
		// Phase A: unified skeletal buffers. The skeletal GBuffer VS uses
		// SkeletalCharIndex / SkeletalVertsPerChar / SkeletalBoneCount to
		// locate this draw's slice inside the shared SBVs.
		UINT32 SkeletalCharIndex = 0;
		UINT32 SkeletalVertsPerChar = 0;
		UINT32 SkeletalBoneCount = 0;
		// Spine VS-inline skinning (desktop-only): per-mesh source scale.
		float SpineSourceScale = 1.0f;
		// Layer 2 vertex deformation infra. See GBufferCommon.hlsli.
		// MeshDeformParams: .x = time (seconds), .yzw reserved.
		glm::vec4 MeshDeformParams = glm::vec4(0.0f);
		// GrassBendOrigin: .xyz = world position of bend center (player),
		// .w = bend strength (0 disables grass bend entirely).
		glm::vec4 GrassBendOrigin = glm::vec4(0.0f);
		// GrassBendParams: .x = bend radius (units), .y = max blade
		// height, .zw reserved.
		glm::vec4 GrassBendParams = glm::vec4(0.0f);
		// Per-mesh flag — set to 1 when drawing the grass mesh.
		UINT32 bGrassMesh = 0;
		// Per-mesh flag — set to 1 when drawing the terrain mesh. Gates
		// ApplyTerrainDeform inside ApplyVertexDeformations.
		UINT32 bTerrainMesh = 0;
		// Per-mesh opt-out for the global deform sphere (BuildPSInput).
		// Set to 1 on meshes you don't want to be carved by the shockwave
		// — e.g. the player avatar that sits at the sphere center.
		UINT32 bExcludeFromDeformSphere = 0;
		UINT32 _GBufferCBPad = 0;
		// WindParams: .xyz = wind direction normalized in XZ (Y typically 0),
		// .w = strength (0 disables wind sway).
		glm::vec4 WindParams = glm::vec4(0.0f);
		// WindTuning: .x = temporal frequency (rad/s), .y = spatial frequency
		// (rad/world-unit), .zw reserved.
		glm::vec4 WindTuning = glm::vec4(0.0f);
		// TerrainDeformSphere: .xyz = world-space sphere center, .w = radius
		// (0 disables). Lower-hemisphere of the sphere is subtracted from the
		// terrain mesh in VS — purely visual, no physics/collision sync.
		glm::vec4 TerrainDeformSphere = glm::vec4(0.0f);
		// Procedural grass (vertex-pulling path). Only meaningful when the
		// draw was issued via the GrassProceduralGraphicsPipeline PSO; the
		// legacy VB path leaves these zeroed.
		UINT32 PG_BladeCount = 0;
		UINT32 PG_BladeSegments = 0;
		float  PG_BladeHeight = 0.0f;
		float  PG_HalfAreaXZ = 0.0f;
		UINT32 PG_Seed = 0;
		// Phase B: per-blade Y is sampled from TerrainHeights[] when
		// PG_TerrainWidth > 0. PG_BaseY is the fallback used when no
		// heightfield is bound (flat-ground demo).
		float  PG_BaseY = 0.0f;
		UINT32 PG_TerrainWidth  = 0;
		UINT32 PG_TerrainDepth  = 0;
		float  PG_TerrainScaleXZ = 1.0f;
		// Procedural grass live-tunables — picked up by VSMain each frame.
		float  PG_RenderDistance = 100.0f;
		UINT32 PG_BladesPerCell  = 500u;
		float  PG_BladeWidthScale = 0.06f; // base width = bladeHeight * this
		float  PG_BladeTipWidthScale = 0.0f; // tip width as fraction of base (0 = pointy, 1 = flat)
		float  PG_GrassPad0 = 0.0f;
		float  PG_GrassPad1 = 0.0f;
		float  PG_GrassPad2 = 0.0f;
	};

	std::shared_ptr<GraphicsPipelineHandle> GBufferGraphicsPipeline;
	// Desktop static-mesh instancing path. Draws repeated map-spawned
	// SceneObjects that share the same Scene/material override as
	// DrawIndexedInstanced, with per-instance world matrices in t12.
	std::shared_ptr<GraphicsPipelineHandle> StaticInstancedGBufferGraphicsPipeline;
	// Vertex-pulling procedural grass PSO. Empty IA (no VB/IB), reads
	// SV_InstanceID / SV_VertexID, samples GBufferConstantBuffer (b0) for
	// world matrix + wind/bend + PG_* fields.
	std::shared_ptr<GraphicsPipelineHandle> ProceduralGrassGraphicsPipeline;
	// Shared sequential IB (0,1,2,…) used by the procedural grass path —
	// the backend doesn't expose non-indexed instanced draw, so we bind
	// this trivial IB and let SV_VertexID equal the index value.
	std::shared_ptr<IndexBuffer> ProceduralGrassSequentialIb;
	std::shared_ptr<GraphicsPipelineHandle> CpuSpineGBufferGraphicsPipeline;
	std::shared_ptr<GraphicsPipelineHandle> SpineGBufferGraphicsPipeline;
	// Spine VS-inline path: skinning math executes in the vertex shader,
	// bypassing the compute pre-pass + GpuSpineSkinnedVertices SBV.
	// Available on both desktop and mobile (mobile keeps Spine compute
	// disabled; VS-inline is the only GPU path it has).
	std::shared_ptr<GraphicsPipelineHandle> SpineVsInlineGBufferGraphicsPipeline;
	bool bSpineUseVsInlineSkinning = false;
	// Phase 11: GBuffer PSO variant for 3D skeletal-skinned meshes. Same IA
	// layout/state as GBufferGraphicsPipeline but the VS samples a SBV of
	// previous-frame skinned positions to emit accurate per-vertex motion
	// vectors (fixes TAA ghosting on rotating limbs).
	std::shared_ptr<GraphicsPipelineHandle> SkeletalGBufferGraphicsPipeline;
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
		UINT32 TotalVertexCount = 0;  // CharCount * VertsPerChar
		UINT32 VertsPerChar = 0;
		UINT32 BoneCount = 0;
		UINT32 _Pad = 0;
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

	// Keep shader-side MAX_POINT_LIGHTS definitions in lock-step with this.
	static constexpr UINT32 MaxPointLights = 128;
	static constexpr UINT32 MaxPointLightsForShadowCB = 128;
	static_assert(MaxPointLights == MaxPointLightsForShadowCB,
		"MaxPointLights and MaxPointLightsForShadowCB must match; RT lighting CB layouts assume the same cap");

	struct PointLightParam
	{
		glm::vec4 PositionAndRadius = glm::vec4(0.0f);
		glm::vec4 ColorAndIntensity = glm::vec4(1.0f);
		glm::vec4 DirectionAndType = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
		glm::vec4 SpotConeAndFlags = glm::vec4(1.0f, 0.70710677f, 3.4142137f, 1.0f);
	};
	static constexpr UINT32 MaxDiffuseGIPointLights = 16;

	// SHaRC-style spatial hash diffuse GI cache
	static constexpr UINT32 SpatialHashGIEntryCount = 1u << 21;
	static constexpr UINT32 SpatialHashGIActiveCellCapacity = 1u << 20;
	static constexpr UINT32 SpatialHashGITraceCellBudget = SpatialHashGIActiveCellCapacity;
	static constexpr UINT32 SpatialHashGISHCoefficientCount = 4u;
	// --- Octahedral DDGI mode (alternative to SH; selected at runtime via
	// SpatialHashGIConstant::GIMode). The first SpatialHashGIOctCellCapacity
	// active cells get an octahedral irradiance + depth map; typical scenes
	// never exceed this so it acts as a hard memory bound, not a real limit.
	// Storage (per probe): irradiance 8x8 (6x6 interior + 1px border) float4,
	// depth 16x16 (14x14 interior + 1px border) float2. "Balanced" regime:
	// 64K probes x 64 rays/probe, ~0.4GB (trace+resolved x irradiance+depth).
	static constexpr UINT32 SpatialHashGIOctCellCapacity = 1u << 18;     // 256K probes (4x: fewer octIndex collisions)
	static constexpr UINT32 SpatialHashGIOctRaysPerCell = 64u;           // full-sphere rays per probe/frame
	static constexpr UINT32 SpatialHashGIOctIrradianceRes = 8u;          // 8x8 directions
	static constexpr UINT32 SpatialHashGIOctDepthRes = 8u;               // 8x8 (smaller, to afford 4x capacity)
	static constexpr UINT32 SpatialHashGIOctIrradianceTexels = SpatialHashGIOctIrradianceRes * SpatialHashGIOctIrradianceRes;
	static constexpr UINT32 SpatialHashGIOctDepthTexels = SpatialHashGIOctDepthRes * SpatialHashGIOctDepthRes;
	enum class SpatialHashGIStorageMode : UINT32 { SH = 0u, Octahedral = 1u };
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
		// 0 = SH4 (legacy, kept for A/B comparison), 1 = octahedral DDGI.
		UINT32 GIMode = 0;
		// Slot capacity that has octahedral atlas storage backing it.
		UINT32 OctCellCapacity = SpatialHashGIOctCellCapacity;
		PointLightParam PointLights[MaxDiffuseGIPointLights];
		UINT32 PointLightCount = 0;
		// Repurposed former float3 padding (layout unchanged):
		float OctNearConvergenceBias = 0.4f; // 0 = uniform, 1 = strong near priority
		float EvictDistanceWeight = 0.7f;    // LRU victim: 0 = age only, 1 = distance only
		float _spatialHashPad = 0.0f;
		glm::vec4 DebugDiffuseGIOverride = glm::vec4(0.0f);
	};

	SpatialHashGIConstant SpatialHashGICB;
	shared_ptr<ComputePipelineStateObject> SpatialHashGIClearPSO;
	shared_ptr<ComputePipelineStateObject> SpatialHashGIUpdatePSO;
	shared_ptr<ComputePipelineStateObject> SpatialHashGIResolvePSO;
	shared_ptr<ComputePipelineStateObject> SpatialHashGIQueryPSO;
	// Option A — bilateral spatial + temporal disocclusion-gated
	// resolve over the per-pixel SpatialHashQuery output. Mirrors
	// the resolve stage from ScreenProbeGI.
	shared_ptr<ComputePipelineStateObject> SpatialHashGIScreenResolvePSO;
	// Octahedral DDGI blend pass: convolves OctRayData into the irradiance atlas.
	shared_ptr<ComputePipelineStateObject> SpatialHashGIOctBlendPSO;
	// Octahedral DDGI depth blend: convolves hit distances into the depth atlas.
	shared_ptr<ComputePipelineStateObject> SpatialHashGIOctDepthBlendPSO;
	std::shared_ptr<Buffer> SpatialHashGIActiveFlags;
	std::shared_ptr<Buffer> SpatialHashGIActiveCellSlots;
	std::shared_ptr<Buffer> SpatialHashGIActiveCounter;
	std::shared_ptr<Buffer> SpatialHashGICellPosition;
	std::shared_ptr<Buffer> SpatialHashGICellNormal;
	std::shared_ptr<Buffer> SpatialHashGICellScore;
	std::shared_ptr<Buffer> SpatialHashGICellLightMask;
	std::shared_ptr<Buffer> SpatialHashGITraceSH[SpatialHashGISHCoefficientCount];
	std::shared_ptr<Buffer> SpatialHashGIResolvedKeys[2];
	std::shared_ptr<Buffer> SpatialHashGIResolvedSH[2][SpatialHashGISHCoefficientCount];
	// Octahedral DDGI atlases (GIMode==1). Index [0] = trace (this frame's ray
	// accumulation, rebuilt per frame), [1] = resolved (persistent history,
	// temporal hysteresis blend done in-place during resolve). Layout is
	// slot-major: texel t of probe slot s lives at index s*Texels + t.
	//   OctIrradiance: float4 (rgb radiance sum, valid-sample weight in .w)
	//   OctDepth:      float2 (mean hit distance, mean distance^2) for Chebyshev
	std::shared_ptr<Buffer> SpatialHashGIOctIrradiance[2];
	std::shared_ptr<Buffer> SpatialHashGIOctDepth[2];
	// Per-oct-slot ownership tag (packed ownerHash16<<16 | frameStamp16). Only the
	// owning cell writes a given oct slot; colliding cells (octIndex aliasing in
	// large scenes) skip it and fall back to neighbour probes at query time.
	std::shared_ptr<Buffer> SpatialHashGIOctCellKey;
	// Per-ray scratch written by the RT trace (oct mode) and consumed by the
	// octahedral blend pass: float4(radiance.rgb, hit distance). Indexed by
	// probeSlot * OctRaysPerCell + rayIndex. Ray directions are regenerated
	// from rayIndex via spherical-Fibonacci + a per-frame random rotation.
	std::shared_ptr<Buffer> SpatialHashGIOctRayData;
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
		// Mode: 0 = Option A channel-pack (sun in R, top-3 point lights in GBA),
		//       1 = ReSTIR Phase 1 single-light reservoir (sun in R; G=light
		//           index, B=light weight, A=visibility for the RIS-chosen
		//           point light). Phase 1 has no temporal/spatial reuse —
		//           per-pixel single-frame RIS only. Phase 2 adds reuse.
		UINT32 ShadowMode = 0;
		// For Option A: 0..3 (number of channel-packed lights).
		// For ReSTIR : 0..MaxPointLights (number of candidate lights iterated).
		UINT32 ShadowedPointLightCount = 0;
		// ReSTIR DI temporal M cap. Higher = more aggressive history
		// retention (smoother but slower to respond to motion); lower =
		// more current-frame-dominant (responsive but noisier on still).
		// Empirical sweet spot on Sponza: 3.0. Runtime-tunable so the
		// user can sweep without recompile. Read by Phase 2 temporal
		// writeback (`min(M_eff, ShadowMaxM)`).
		float ShadowMaxM = 3.0f;
		// Up to MaxPointLightsForShadowCB candidates. Option A reads only
		// the first 3 (channel-pack hard cap); ReSTIR Phase 1 iterates
		// all valid entries for per-pixel RIS. 128 * 32 B = 4 KB — fits
		// the CB budget comfortably.
		glm::vec4 ShadowedPointLights[MaxPointLightsForShadowCB] = {};
		// Per-light intensity used as RIS candidate weight (color luma *
		// intensity). Only ReSTIR mode reads these; Option A treats every
		// shadowed light as equal-cost.
		glm::vec4 ShadowedPointLightWeights[MaxPointLightsForShadowCB] = {};
	};

	RTShadowViewParamCB RTShadowViewParam;
	
	shared_ptr<RTPipelineStateObject> PSO_RT_SHADOW;
	// Phase 3 spatial-reuse compute PSO. Uses inline RT (RayQuery) so
	// it can trace fresh visibility for the spatially-chosen light.
	// Reads the PreSpatial reservoirs; writes the final ShadowBuffer.
	shared_ptr<ComputePipelineStateObject> PSO_SHADOW_SPATIAL_REUSE;
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
		PointLightParam PointLights[MaxDiffuseGIPointLights];
		UINT32 PointLightCount = 0;
		glm::vec3 PointLightPadding = glm::vec3(0.0f);
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
		PointLightParam PointLights[MaxDiffuseGIPointLights];
		UINT32 PointLightCount = 0;
		glm::vec3 PointLightPadding = glm::vec3(0.0f);
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
		UINT32 HashEntryMask = SpatialHashGIEntryCount - 1u;
		UINT32 MaxProbeSteps = 8;
		// Octahedral DDGI: 0 = SH trace (legacy), 1 = per-ray trace to RayData.
		UINT32 GIMode = 0;
		UINT32 OctCellCapacity = SpatialHashGIOctCellCapacity;
		UINT32 OctRaysPerCell = SpatialHashGIOctRaysPerCell;
		PointLightParam PointLights[MaxDiffuseGIPointLights];
		UINT32 PointLightCount = 0;
		glm::vec3 PointLightPadding = glm::vec3(0.0f);
		// World-space camera position (oct mode biases the probe origin toward the
		// camera/visible side so the sample center isn't buried in geometry).
		glm::vec4 CameraPosition = glm::vec4(0.0f);
	};

	RTSpatialHashGIViewParamCB RTSpatialHashGIViewParam;
	shared_ptr<RTPipelineStateObject> PSO_RT_SPATIAL_HASH_GI;
	shared_ptr<RTPipelineStateObject> PSO_RT_SPATIAL_HASH_GI_SER;
	bool bRTDiffuseGISpatialHashSERInitFailed = false;

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
		UINT32 bDirectLightCastShadow = 1;
		float _directLightPadding = 0.0f;
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
	bool PrevPathTracingDirectionalLightCastShadow = true;
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
	std::shared_ptr<GraphicsPipelineHandle> ToneMapGraphicsPipeline;

	// Particle pass (Phase 1 / M1: single static additive billboard).
	// PSO targets the post-light LightingBuffer with EBlendMode::Additive +
	// depth-test (no depth write). ActiveParticleSystems is populated by the
	// startup-mode demo bootstrap and walked once per frame by ParticlePass.
	std::shared_ptr<GraphicsPipelineHandle> ParticleGraphicsPipeline;
	std::vector<std::shared_ptr<Particles::System>> ActiveParticleSystems;
	struct ParticleCB
	{
		glm::mat4x4 ViewProj;
	};

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

	std::shared_ptr<GraphicsPipelineHandle> BufferVisualizeGraphicsPipeline;

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
		// Mirror RTShadowViewParamCB::ShadowMode (0 = Option A, 1 = ReSTIR
		// Phase 1). LightingPS branches its point-light loop accordingly.
		UINT32 ShadowMode = 0;
		glm::vec4 AmbientSkyColorAndStrength = glm::vec4(0.0f);
		glm::vec4 AmbientGroundColorAndStrength = glm::vec4(0.0f);
		// Option A channel-pack map: global lightIndex -> channel index
		// (0/1/2) or 0xFFFFFFFF when the light isn't shadowed this frame.
		// Packed 4 entries per uvec4 to match HLSL's 16-byte CB array
		// stride without inflating the cbuffer to N*16 bytes.
		glm::uvec4 ShadowChannelMap[MaxPointLights / 4];
		PointLightParam PointLights[MaxPointLights];
		UINT32 PointLightCount = 0;
		// Strength of the RTAO contact term multiplied into DIRECT diffuse light
		// (0 = no contact darkening, 1 = full AO). Adjustable in the Editor Config
		// RTAO Details UI. Repurposes former padding (layout unchanged).
		float RTAODirectContactStrength = 0.5f;
		glm::vec2 PointLightPadding = glm::vec2(0.0f);
	};
	
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
	std::shared_ptr<GraphicsPipelineHandle> SkeletalMobileShadowMapGraphicsPipeline;
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

	EAntiAliasingMode AntiAliasingMode = EAntiAliasingMode::TAA;
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
	// Point-light shadow mode: false = 4-channel pack (sun + first 3 lights,
	// hard-cap), true = ReSTIR Phase 1+2 reservoir (single-light per pixel,
	// scales to MaxPointLights candidates, with previous-frame reproject).
	// ReSTIR is now the default — it handles arbitrary light counts; the
	// 4-channel path stays as a fallback for A/B comparison.
	bool bEnableReSTIRDirectShadow = true;
	// Runtime-tunable temporal M cap for the ReSTIR Phase 2 reservoir.
	// Defaults to the empirical sweet spot tuned on Sponza + DLSS RR.
	// Surfaced via the Sponza demo's imgui panel.
	float ReSTIRShadowMaxM = 3.0f;
	// Phase 3 proper 2-pass spatial reuse compute. Disabled by
	// default — the current implementation over-brightens sponza
	// because the simple M-weighted RIS combine doesn't properly MIS-
	// reweight neighbours whose chosen lights have a higher tpdf at
	// the current pixel than the centre's. Re-enable once balance-
	// heuristic MIS is implemented (or set the kSpatialSamples in the
	// .hlsl back to 4 with a proper MIS pass).
	bool bEnableShadowSpatialReuseCompute = false;
	bool bEnableSkyLighting = false;
	bool bEnableRayTracedSkyLighting = true;
	bool bDebugForceDiffuseGIColor = false;
	glm::vec3 DebugForceDiffuseGIColor = glm::vec3(1.0f, 0.0f, 1.0f);
	UINT32 DiffuseGIPointLightLimit = MaxDiffuseGIPointLights;
	float RTAOIndirectStrength = 1.0f;
	float RTAOIndirectFloor = 0.55f;
	// RTAO contact term multiplied into DIRECT diffuse light (0..1).
	float RTAODirectContactStrength = 0.5f;
	float SurfaceBounceStrength = 1.0f;
	float SurfaceBounceSaturation = 1.0f;
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
	EAntiAliasingMode StartupSelectedAAMode = EAntiAliasingMode::TAA;
	ERenderingMode StartupRenderingMode = ERenderingMode::HYBRID;
	ERenderBackendAPI StartupRenderBackendAPI = ERenderBackendAPI::D3D12;
	bool bCommandLineAutoDumpOverrideSet = false;
	bool bCommandLineAutoDumpEnabled = false;
	bool bCommandLineAAOverrideSet = false;
	EAntiAliasingMode CommandLineSelectedAAMode = EAntiAliasingMode::TAA;
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
	bool bCommandLineSpineBenchmarkLive = false;
	UINT32 CommandLinePlatformerSpineBenchmarkCount = 50;
	bool bCommandLineSpawnSkeletalTest = false;
	UINT32 CommandLineSkeletalTestCount = 1;
	// Standalone skeletal-skinning benchmark mode: no Sponza, no other
	// scene content — just the procedural box characters dropped at the
	// origin so the GPU cost (compute skin + BLAS refit + GBuffer draw)
	// is the only thing being measured.
	bool bCommandLineSkeletalBenchMode = false;
	bool bCommandLineSkeletalTestScreenshot = false;
	UINT32 SkeletalTestScreenshotFrame = 60;
	std::wstring SkeletalTestScreenshotPath;
	bool bSkeletalTestScreenshotDone = false;
	bool bCommandLineBvhViewerOverrideSet = false;
	bool bCommandLineBvhViewerEnabled = false;
	bool bCommandLineNvFrapsBvhLiveTlas = false;
	bool bStartupFreeFlyCamera = false;
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
	std::wstring CommandLineLoadMapFile;
	UINT32 CommandLineScreenshotFrame = 0;
	bool bCommandLineScreenshotTriggered = false;

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

	shared_ptr<ComputePipelineStateObject> BloomBlurPSO;
	shared_ptr<ComputePipelineStateObject> BloomExtractPSO;
	shared_ptr<ComputePipelineStateObject> HistogramPSO;
	shared_ptr<ComputePipelineStateObject> ClearHistogramPSO;

	bool bDrawHistogram = false;
	shared_ptr<ComputePipelineStateObject> DrawHistogramPSO;

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

	shared_ptr<ComputePipelineStateObject> AdapteExposurePSO;

	shared_ptr<VertexBuffer> FullScreenVB;

	// Active terrain instance (Phase 1: 1km×1km procedural). Holds per-chunk
	// AABBs the per-frame culling pass consumes; Mesh + Scene are inside.
	std::unique_ptr<Terrain::Component> ActiveTerrain;


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
	bool bRayTracingBLASCacheResetPending = false;
	bool bRayTracingBLASBuildSuspended = false;

	// Recipe = how the Scene was created, so SaveMap can serialize the
	// procedural params (seed/blade_count/...) or the asset path without
	// duplicating that data per-entity.
	struct SceneRecipe
	{
		enum class Kind : uint8_t
		{
			Asset,           // loaded from disk (OBJ/FBX/glTF) — AssetPath
			Terrain,         // procedural terrain — Seed
			GrassOnTerrain,  // procedural grass on terrain — BladeCount, BladeHeight, Seed
			Grass,           // flat procedural grass field — BladeCount, AreaSize, BladeHeight, Seed
			BlockCharacter,  // procedural block character — Seed
		};
		Kind RecipeKind = Kind::Asset;
		std::wstring AssetPath;
		uint32_t Seed = 0;
		uint32_t BladeCount = 0;
		float BladeHeight = 0.0f;
		float AreaSize = 0.0f;
		// Per-blade tessellation segments (Grass / GrassOnTerrain only).
		// 0 = leave the default (currently 4). Higher = smoother bend.
		uint32_t BladeSegments = 0;
		// Vertex-pulling path (Grass / GrassOnTerrain). When true the mesh
		// is spawned via CreateProceduralGrassOnTerrainSceneInstanced — no
		// VB upload, blade geometry synthesized in the VS.
		bool bProceduralPath = false;
	};
	struct ScriptSceneEntry
	{
		shared_ptr<Scene> ScenePtr;
		std::wstring Path;
		EPhysicsCollisionShape PhysicsCollisionShape = EPhysicsCollisionShape::TriangleMesh;
		glm::vec3 PhysicsBoxHalfExtent = glm::vec3(0.5f);
		SceneRecipe Recipe;
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
	// Render thread sets this when the C++ imgui sun-direction gizmo
	// modifies LightDir; the next ApplyFrameSourceRenderSync skips its
	// LightDir/LightIntensity overwrite so the user's drag isn't reverted
	// by a stale game-thread state capture (split game/render threads).
	bool bRenderThreadOwnsLightDirNextFrame = false;
	// Set by LoadCameraState() on success so ApplySponzaFlyCamera can keep
	// the restored camera position/rotation/light instead of snapping back
	// to the default fly-camera preset — useful for resuming an inspection of
	// the scene at the same vantage point across runs.
	bool bCameraStateRestoredFromDisk = false;
	struct PointLightState
	{
		UINT32 Id = 0;
		CoronaECS::Entity EntityHandle;
		bool bEnabled = true;
		glm::vec3 Position = glm::vec3(0.0f);
		float Radius = 320.0f;
		glm::vec3 Color = glm::vec3(1.0f);
		float Intensity = 10.0f;
		CoronaECS::LightType Type = CoronaECS::LightType::Point;
		bool bCastShadow = true;
		glm::vec3 Direction = glm::vec3(0.0f, 1.0f, 0.0f);
		float InnerConeAngle = 0.0f;
		float OuterConeAngle = 0.785398163f;
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
		float RTAOIndirectStrength = 1.0f;
		float RTAOIndirectFloor = 0.55f;
		float RTAODirectContactStrength = 0.5f;
		float SurfaceBounceStrength = 1.0f;
		float SurfaceBounceSaturation = 1.0f;
		float SkyLightingStrength = 0.35f;
		float JitterScale = 0.6f;
		UINT32 TAASampleCount = 32;
		float DLSSJitterPhaseScale = 4.0f;
		UINT32 DLSSJitterPhaseCountOverride = 0;
		glm::vec3 LightDir = glm::vec3(0.0f, 1.0f, 0.0f);
		float LightIntensity = 0.4f;
		bool bDirectionalLightCastShadow = true;
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
		// 0 = SH4 spatial-hash GI, 1 = octahedral DDGI (A/B comparison toggle).
		UINT32 SpatialHashGIStorageMode = 0;
		float SpatialHashOctNearConvergenceBias = 0.4f;
		float SpatialHashEvictDistanceWeight = 0.7f;
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
	uint64_t GBufferLastStaticInstancedBatchCount = 0;
	uint64_t GBufferLastStaticInstancedObjectCount = 0;
	uint64_t GBufferLastStaticInstancedDrawCount = 0;
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
	bool RenderFrameDirectionalLightCastShadow = true;
	float RenderFrameShaderTime = 0.0f;
	// Unscaled real time (seconds since startup) for shader effects that
	// need true wall-clock rate — currently the wind sway. RT noise paths
	// stay on the 0.01× ShaderTime to preserve their random-seed range.
	float RenderFrameWindTime = 0.0f;
	// Grass bend (Phase 3 vertex deformation effect). Updated each frame
	// by scripts via corona.set_grass_bend_origin(...). The CB filler in
	// DrawScene copies these directly into GBufferConstantBuffer.
	glm::vec4 RenderFrameGrassBendOrigin = glm::vec4(0.0f); // xyz = world pos, w = bend strength (0 disables)
	glm::vec4 RenderFrameGrassBendParams = glm::vec4(120.0f, 30.0f, 0.0f, 0.0f); // x = radius, y = max blade height
	// Wind sway (Phase 3.5 vertex deformation effect). Composes additively
	// with grass bend.
	glm::vec4 RenderFrameWindParams = glm::vec4(0.0f);          // xyz = dir (XZ-normalized), w = strength (0 disables)
	glm::vec4 RenderFrameWindTuning = glm::vec4(2.0f, 0.015f, 0.0f, 0.0f); // x = temporal freq, y = spatial freq
	// Terrain deformation sphere — visual-only sphere subtract applied to
	// the terrain mesh in VS. .xyz = world center, .w = radius (0 disables).
	glm::vec4 RenderFrameTerrainDeformSphere = glm::vec4(0.0f);
	// World-space position of the active camera (script-driven or default
	// free-fly) for the frame being built. Console / scripts that need to
	// place objects in front of the camera read this instead of poking at
	// m_camera.m_position directly (which is stale when a Luau script is
	// driving the camera via CameraComponent.set).
	glm::vec3 RenderFrameCameraPosition = glm::vec3(0.0f);
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
	bool bShowFrameTimingOverlay = false;
	bool bEditorConfigWindowOpen = false;
	bool bEditorLightingGIWindowOpen = false;
	bool bEditorDebugCaptureWindowOpen = false;
	bool bEditorCameraCollisionEnabled = false;
	float EditorCameraMoveSpeed = 1000.0f;
	bool bEditorMapLoadQueued = false;
	bool bEditorMapLoadInProgress = false;
	uint32_t EditorMapLoadEntityIndex = 0;
	uint32_t EditorMapLoadEntityCount = 0;
	std::wstring PendingEditorMapLoadName;
	std::wstring LastEditorMapLoadStatus;
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

	// Cached overlay text rebuilt at 4 Hz instead of every frame so the Lua
	// imgui callback doesn't re-iterate 60+ pass entries × 4 fields each
	// across the C++↔Lua boundary every frame. Empty if the overlay was
	// never requested by a script this run.
	std::string CachedFrameTimingOverlayText;
	double CachedFrameTimingOverlayTimestampSec = -1.0;
	void RebuildFrameTimingOverlayTextIfStale();
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
	std::array<float, RtRecordPhaseCount> RtRecordPhaseLastTimeMs = {};
	std::array<float, RtRecordPhaseCount> RtRecordPhaseCompletedLastTimeMs = {};
	std::array<float, RtRecordPhaseCount> RtRecordPhaseAverageTimeMs = {};
	std::array<std::deque<float>, RtRecordPhaseCount> RtRecordPhaseHistoryMs = {};
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
	std::array<double, RtRecordPhaseCount> FramePerfLogAccumRtRecordPhaseMs = {};
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
	std::array<float, GpuPassCount> CpuPassAverageTimeMs = {};
	std::array<std::deque<float>, GpuPassCount> CpuPassHistoryMs = {};
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
	void AddRtRecordPhaseTiming(ERtRecordPhase phase, const CpuClock::time_point& begin, const CpuClock::time_point& end);
	void AddCpuUpdatePhaseTiming(ECpuUpdatePhase phase, const CpuClock::time_point& begin, const CpuClock::time_point& end);
	void FinishCpuUpdateTiming(const CpuClock::time_point& begin, const CpuClock::time_point& end);
	void TrimCpuUpdateTimingHistory();
	void SetGpuTimingAverageFrameCount(UINT32 frameCount);
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
		uint64_t BuildHitProgramBindingSignature(const RTSceneHitProgramDesc& desc) const;

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
	shared_ptr<Scene> CreateProceduralGrassScene(UINT32 numBlades, float areaSize, float bladeHeight, UINT32 seed, UINT32 bladeSegments = 4u);
	// Procedural UV sphere generator. `rings` = latitude steps, `segments` =
	// longitude steps. Standard 44 B Vertex layout so it shares the default
	// GBuffer PSO.
	shared_ptr<Scene> CreateProceduralSphereScene(float radius, uint32_t rings = 24, uint32_t segments = 32);
	ScriptSceneHandle CreateProceduralSphereSceneForScript(float radius, uint32_t rings = 24, uint32_t segments = 32);
	// Same as CreateProceduralGrassScene but each blade's base Y is sampled
	// from the currently-active TerrainComponent so the blades sit on the
	// terrain surface. Falls back to flat (y=0) when no terrain is active.
	shared_ptr<Scene> CreateProceduralGrassOnTerrainScene(UINT32 numBlades, float bladeHeight, UINT32 seed, UINT32 bladeSegments = 4u);
	// Lightweight twin of CreateProceduralGrassOnTerrainScene: no VB/IB,
	// just metadata; the renderer issues DrawInstanced through the
	// ProceduralGrassGraphicsPipeline PSO which synthesizes blade geometry
	// in the VS.
	shared_ptr<Scene> CreateProceduralGrassOnTerrainSceneInstanced(UINT32 numBlades, float bladeHeight, UINT32 seed, UINT32 bladeSegments = 4u);
	void UpdateGrassCulling(const glm::mat4& viewProj);
	shared_ptr<Scene> CreateProceduralTerrainScene(UINT32 seed);
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
	void UpdateSimpleCameraFromActiveCameraEntity();
	void InitializeMainDirectionalLightEntity();
	void UpdateMainDirectionalLightEntityFromState();
	void ApplyDirectionalLightEntityToState();
	CoronaECS::Entity CreatePointLightEntity(PointLightState& pointLight);
	void UpdatePointLightEntity(const PointLightState& pointLight);
	void DestroyPointLightEntity(PointLightState& pointLight);
	PointLightState* FindPointLightByEntity(CoronaECS::Entity entity);
	const PointLightState* FindPointLightByEntity(CoronaECS::Entity entity) const;
	void InitRaytracingShadowPass();
	void InitShadowSpatialReusePass();
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

	// Accessor for the active terrain (Phase 1, single-instance). Returns
	// nullptr when no terrain has been spawned yet.
	Terrain::Component* GetActiveTerrain() const { return ActiveTerrain.get(); }

	// Grass chunk DrawCall + AABB record. Public so the Lua callbacks +
	// the BuildGrassBlades helper (anonymous namespace) can reference it.
	struct GrassChunkInfo
	{
		uint32_t IndexStart = 0;
		uint32_t IndexCount = 0;
		uint32_t VertexBase = 0;
		uint32_t VertexCount = 0;
		float    CenterX = 0.0f;
		float    CenterZ = 0.0f;
		float    AabbMin[3] = {0,0,0};
		float    AabbMax[3] = {0,0,0};
	};
	// Origin (typically player or camera world position) used by the
	// distance check inside UpdateGrassCulling.
	glm::vec3 GrassRenderOrigin = glm::vec3(0.0f);
	// Distance in world units from GrassRenderOrigin within which a grass
	// chunk is allowed to render. 0 = unlimited (frustum-only cull).
	float GrassRenderDistance = 300.0f;
	// Procedural grass live-tunables, edited via Scene Inspector. Apply to
	// every procedural-path grass mesh — DrawScene reads them into the CB
	// each frame and the host-side draw uses BladesPerCell to compute the
	// per-frame instance count.
	int   GrassProceduralBladesPerCell  = 500;
	float GrassProceduralBladeWidthScale = 0.06f;
	float GrassProceduralBladeTipWidthScale = 0.0f; // 0 = pointy, 1 = flat top
	float GrassProceduralBladeHeight    = 1.6f;
	// Per-bin DrawCall + AABB list for the active grass field. Populated
	// once at spawn, consumed by UpdateGrassCulling each frame.
	std::shared_ptr<Mesh>       ActiveGrassMesh;
	std::shared_ptr<Material>   ActiveGrassMaterial;
	std::vector<GrassChunkInfo> ActiveGrassChunks;
	// Weak ref to the Scene that owns ActiveGrassMesh. Used by the GBuffer
	// draw loop to wrap the grass draw in its own GPU timing pass.
	std::weak_ptr<Scene>        ActiveGrassScene;
	uint32_t LastVisibleGrassChunkCount = 0;

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
	// Procedural grass: a single Mesh containing N upright blades laid out
	// over a square area. Each blade is a 2-segment quad rooted at Y=0
	// with the tip at Y=bladeHeight. Marked `bGrassMesh = true` so the
	// GBuffer VS Layer 2 deformation runs grass bend on it.
	ScriptSceneHandle CreateProceduralGrassSceneForScript(UINT32 numBlades, float areaSize, float bladeHeight, UINT32 seed, UINT32 bladeSegments = 4u);
	ScriptSceneHandle CreateProceduralGrassOnTerrainSceneForScript(UINT32 numBlades, float bladeHeight, UINT32 seed, UINT32 bladeSegments = 4u);
	ScriptSceneHandle CreateProceduralGrassOnTerrainSceneInstancedForScript(UINT32 numBlades, float bladeHeight, UINT32 seed, UINT32 bladeSegments = 4u);

	// Re-spawn the procedural grass mesh on an existing entity using the
	// supplied params. Used by Scene Inspector's "Regenerate" button so the
	// user can re-tune blade_count / blade_height / blade_segments without
	// destroying the entity itself.
	bool RegenerateGrassEntityForScript(
		CoronaECS::Entity entity,
		UINT32 bladeCount,
		float bladeHeight,
		UINT32 seed,
		UINT32 bladeSegments,
		bool bProcedural);
	ScriptSceneHandle CreateProceduralTerrainSceneForScript(UINT32 seed);
	// Script-facing setters for the grass-bend CB inputs. Called from
	// Lua each frame; the GBuffer CB filler reads
	// RenderFrameGrassBendOrigin / RenderFrameGrassBendParams.
	void SetGrassBendOriginForScript(float x, float y, float z, float strength) { RenderFrameGrassBendOrigin = glm::vec4(x, y, z, strength); }
	void SetGrassBendParamsForScript(float radius, float maxBladeHeight)        { RenderFrameGrassBendParams = glm::vec4(radius, maxBladeHeight, 0.0f, 0.0f); }
	// Read-only accessors for the Lua get_* bindings (Corona.Scripting.cpp
	// lives outside the friend list so it can't reach private members
	// directly).
	glm::vec4 GetRenderFrameWindParamsForScript() const  { return RenderFrameWindParams; }
	glm::vec4 GetRenderFrameWindTuningForScript() const  { return RenderFrameWindTuning; }
	glm::vec4 GetRenderFrameGrassBendParamsForScript() const { return RenderFrameGrassBendParams; }
	float     GetGrassRenderDistanceForScript() const    { return GrassRenderDistance; }
	// Inspector helpers: did this entity spawn from a procedural-grass
	// recipe? Used by the Scene Inspector to surface wind / bend / render-
	// distance controls only when a grass mesh is selected.
	bool IsEntityGrassMesh(CoronaECS::Entity entity) const;
	// Fetch the SceneRecipe a mesh entity was spawned from. Returns false
	// for engine-internal or pre-recipe meshes; otherwise fills `outRecipe`
	// with a copy. Used by the inspector to seed its edit fields.
	bool GetEntityMeshRecipeForScript(CoronaECS::Entity entity, SceneRecipe& outRecipe) const;
	// Wind sway: dirX/dirZ should already be XZ-normalized (Lua side does
	// the normalization for clarity). tempFreq/spaceFreq are tuning knobs;
	// pass 0 to keep current values.
	void SetWindParamsForScript(float dirX, float dirZ, float strength, float tempFreq, float spaceFreq)
	{
		// Normalize the XZ direction so the visible sway amplitude only
		// depends on `strength`. Earlier callers passed unnormalized small
		// vectors like (0.11, 0.14) which silently capped the effective
		// strength to ~18% of the slider's value.
		const float dirLen = std::sqrt(dirX * dirX + dirZ * dirZ);
		if (dirLen > 1e-4f)
		{
			dirX /= dirLen;
			dirZ /= dirLen;
		}
		RenderFrameWindParams = glm::vec4(dirX, 0.0f, dirZ, strength);
		if (tempFreq  > 0.0f) RenderFrameWindTuning.x = tempFreq;
		if (spaceFreq > 0.0f) RenderFrameWindTuning.y = spaceFreq;
	}
	// Visual-only terrain sphere subtract. radius=0 disables.
	void SetTerrainDeformSphereForScript(float x, float y, float z, float radius)
	{
		RenderFrameTerrainDeformSphere = glm::vec4(x, y, z, radius);
	}
	ScriptSceneHandle CreateSpineSceneForScript(const std::wstring& assetPath, const std::string& animationName, float timeSeconds, float sourceScale = 1.0f);
	// Live Spine: persistent skeleton + animation state owned per call.
	// CreateLiveSpineForScript builds the skeleton + initial mesh and
	// returns a ScriptSceneHandle that can be passed to set_pose. The
	// underlying VB/IB are allocated once at max-vertex-count and
	// re-uploaded in place by UpdateLiveSpineForScript every frame.
	// This bypasses both the Lua sceneCache and the C++
	// ScriptSceneByPath/clip caches, so it pays the full
	// `spAnimation_apply + spSkeleton_updateWorldTransform +
	// BuildSpineSampleMesh + memcpy` cost on every tick — the canonical
	// "live Spine" model the official runtimes use.
	ScriptSceneHandle CreateLiveSpineForScript(const std::wstring& assetPath, const std::string& animationName, float sourceScale = 1.0f);
	bool UpdateLiveSpineForScript(ScriptSceneHandle handle, float deltaSeconds);
	bool DestroyLiveSpineForScript(ScriptSceneHandle handle);
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
	bool SetEntityExcludeFromDeformSphereForScript(CoronaECS::Entity entity, bool bExclude);
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
	void PushLuauUiStateForScript(lua_State* L, const std::string& mode = std::string(), bool bWantPointLights = true);
	bool SetLuauUiValueForScript(const std::string& name, lua_State* L, int valueIndex);
	bool RunLuauUiCommandForScript(const std::string& name, lua_State* L, int argIndex);
	void PushPersistentScriptControlForScript(lua_State* L, const std::string& name, int defaultIndex);
	bool SetPersistentScriptControlForScript(const std::string& name, lua_State* L, int valueIndex);
	

	void LoadPipeline();

	void LoadAssets();

	shared_ptr<Scene> LoadModel(string fileName);
	shared_ptr<Scene> CreateProceduralBlockCharacterScene(UINT32 seed);

	// Console (~ key) accessors so CoronaConsole can spawn assets in front
	// of the camera the renderer is actually using. RenderFrameCameraPosition
	// reflects whichever camera (script-driven or default) drove the most
	// recent frame — m_camera.m_position is stale when a Luau script uses
	// CameraComponent.set since that path doesn't write back into m_camera.
	glm::vec3 GetCameraPositionForConsole() const { return RenderFrameCameraPosition; }
	glm::vec3 GetCameraLookDirForConsole() const { return RenderFrameCameraLookDirection; }
	Terrain::Component* GetActiveTerrainForConsole() const { return ActiveTerrain.get(); }
	glm::mat4x4 GetUnjitteredViewProjForOverlay() const { return UnjitteredViewProjMat; }
	UINT GetRenderWidthForOverlay() const { return GetRenderWidth(); }
	UINT GetRenderHeightForOverlay() const { return GetRenderHeight(); }

	// Map serialization (Corona.MapFormat.cpp). Writes / reads a Luau-syntax
	// snapshot of the script-spawned entity graph (mesh recipes, transforms,
	// material overrides, lights, cameras) plus global state (wind / grass
	// deform / terrain deform sphere). Used by the console `savemap` /
	// `loadmap` commands and by mode scripts that want to skip the explicit
	// spawn block when a cached map is already on disk.
	bool SaveMapToFile(const std::wstring& name, std::wstring* outError = nullptr);
	bool LoadMapFromFile(const std::wstring& name, std::wstring* outError = nullptr);
	void QueueEditorMapLoad(const std::wstring& name);
	bool IsEditorMapLoadQueued() const { return bEditorMapLoadQueued; }
	const std::wstring& GetLastEditorMapLoadStatus() const { return LastEditorMapLoadStatus; }

	// Serialize a single entity (mesh / light / camera) as a standalone
	// asset to `assets/scene_assets/<assetName>.asset.lua`. Returns false +
	// fills `outError` on failure (entity dead, no serializable component,
	// I/O error). Loading the asset back is a follow-up to Phase 1.
	bool SaveEntityAsAsset(CoronaECS::Entity entity, const std::string& assetName, std::wstring* outError = nullptr);
	void ClearScriptSpawnedScene();
	std::filesystem::path ResolveMapPath(const std::wstring& name) const;
	// Cross-run "last loaded map" persistence. PersistLastEditorMapName writes
	// the name to assets/maps/.last_loaded_map on a successful load_map so the
	// no-arg launch (editor mode) can reopen it via ReadPersistedLastEditorMapName.
	void PersistLastEditorMapName(const std::wstring& name) const;
	std::wstring ReadPersistedLastEditorMapName() const;
	// Last successful save_map / load_map name. Empty if no map has been
	// touched this session. `savemap` (no args) overwrites this; the console
	// reports "no current map" if nothing is loaded yet.
	const std::wstring& GetCurrentMapName() const { return CurrentMapName; }
private:
	std::wstring CurrentMapName;
public:
	shared_ptr<Scene> LoadBinaryMeshModel(const std::wstring& binaryFileName, const std::wstring& sourceFileName);

	// Deletes the cmesh cache file paired with `sourceFilePath` (.cmesh
	// sibling). Next LoadMeshModel of the same source goes through the
	// FBX importer again — useful when the importer config changes or
	// the asset was edited externally. Returns true on success or if
	// no cache exists (which is also "ready for re-import").
	bool InvalidateMeshCacheForSource(const std::wstring& sourceFilePath, std::wstring* outErr = nullptr);

	void InitRTPSO();

	void InitTemporalDenoisingPass();
	void InitScreenProbeGIPass();
	void InitSpatialHashGIPass();

	void InitGBufferPass();

	void InitToneMapPass();

	void InitDebugPass();

	void InitLightingPass();
	void InitParticlePass();
	void InitMobileShadowMapPass();

	void InitTemporalAAPass();

	void InitBloomPass();

	void InitGenMipSpecularGIPass();

	void InitImgui();
	void DrawMobileVirtualControls();
	void DrawMobilePerformanceOverlay();
	void DrawRuntimeFrameOverlay();
	void DrawEditorMainWindowControls();
	void DrawEditorCameraOverlay();
	void UpdateMobileVirtualMoveFromTouch(const PlatformTouchState& touchState);
	bool LoadCameraState();
	void SaveCameraState();
	std::wstring GetCameraStatePath();
	// Editor render-state persistence (anti-aliasing mode). Saved when the UI AA
	// combo changes; loaded at startup and used as the default startup AA mode so
	// the user's choice carries over to the next run.
	bool LoadEditorRenderState();
	void SaveEditorRenderState();
	std::wstring GetEditorRenderStatePath();
	EAntiAliasingMode SavedEditorAAMode = EAntiAliasingMode::TAA;
	bool bHasSavedEditorAAMode = false;
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

	// LLM-driven motion playback path (Corona.SmplCharacter.cpp +
	// Corona.MotionPlayback.h). The console hands a loaded BVH clip to
	// SetMotionClipForPlayback; that lazy-spawns one SMPL 24-bone box
	// character at the camera-spawn point (Step 8 substitutes a terrain-
	// snapped position) and starts the playback driver. The per-frame
	// palette upload runs as the first step inside
	// DispatchSkeletalSkinningForRenderWorld so the existing compute
	// skinning + GBuffer paths see fresh bone matrices each frame.
	bool SpawnSmplMotionCharacter(const glm::vec3& worldOrigin, float uniformScale);
	void UpdateSmplMotionCharacterPalette();
	void SetMotionClipForPlayback(CoronaMotion::MotionClip&& clip,
		const glm::vec3& spawnWorldOrigin, float uniformScale);
	std::unique_ptr<CoronaSmpl::CharacterResources> SmplMotionCharacter;
	SceneObjectHandle                               SmplMotionCharacterHandle = InvalidSceneObjectHandle;
	std::unique_ptr<CoronaMotion::MotionPlayback>   MotionPlayback;
	// Wall-clock baseline for advancing the motion playback time. Refreshed
	// every UpdateSmplMotionCharacterPalette call; clamped to 0.1 s so a
	// debugger pause doesn't teleport the clip half a loop forward.
	std::chrono::steady_clock::time_point           MotionLastTickTime{};
	bool                                            bMotionTickInit = false;
	// Time the previous frame's skeletal palette was sampled at, used by
	// UpdateSkeletalTestCharacters to compute the prev-frame bone matrices
	// it uploads to SkeletalPrevBoneMatrices for the motion-vector VS.
	float SkeletalPrevUpdateTimeSeconds = 0.0f;
	bool bSkeletalPrevUpdateTimeValid = false;
	// Phase A render-thread optimization: every test character points at
	// these shared unified buffers so the compute path can drive all
	// characters with a single Dispatch and the GBuffer pulls each
	// character's slice via BaseVertexLocation + a per-draw char index.
	// Owned by the spawn function; identical pointers are also stored on
	// each Mesh's per-character SkeletalInputVertices / SkeletalBoneMatrices
	// / SkeletalPrevBoneMatrices / SkeletalOutputVb so existing
	// per-mesh code paths keep working.
	std::shared_ptr<Buffer> SkeletalUnifiedInputVertices;
	std::shared_ptr<Buffer> SkeletalUnifiedBoneMatrices;
	std::shared_ptr<Buffer> SkeletalUnifiedPrevBoneMatrices;
	std::shared_ptr<VertexBuffer> SkeletalUnifiedOutputVb;
	std::shared_ptr<VertexBuffer> SkeletalUnifiedBindVb;
	std::shared_ptr<IndexBuffer>  SkeletalUnifiedIb;
	// Phase D (desktop-only cluster draw): per-instance world transform
	// SBV indexed by SV_InstanceID in SkeletalVsInlineClusterVSMain.
	// Stored as mat3x4 (SkinBone_t layout, 48 B per entry) to match the
	// other unified palette buffers. Refreshed each frame from
	// RenderWorld.SceneObjects. Mobile (GBufferMobile.hlsl) doesn't have
	// the cluster shader entry so this SBV isn't bound there.
	std::shared_ptr<Buffer> SkeletalUnifiedInstanceTransforms;
	std::shared_ptr<Material> SkeletalUnifiedMaterial;
	uint32_t SkeletalUnifiedIndexCount = 0;
	uint32_t SkeletalUnifiedCharCount = 0;
	uint32_t SkeletalUnifiedVertsPerChar = 0;
	uint32_t SkeletalUnifiedBoneCount = 0;
	// Phase S (CPU skinning): when bSkeletalUseCpuSkinning is true, the
	// dispatch step skips the compute pre-pass and instead CPU-skins
	// every character into an UPLOAD-heap VB created fresh each frame.
	// `SkeletalUnifiedPaletteCpu` mirrors the packed bone palette that
	// UpdateSkeletalTestCharacters computes so the render thread can
	// read it without re-running the math.
	bool bSkeletalUseCpuSkinning = false;
	// Path C benchmark flag: skip per-character BLAS refit inside the
	// compute path so the comparison reflects skinning cost only.
	// RT visuals freeze at the bind pose while this is on.
	bool bSkeletalSkipBlas = false;
	// Path C: VS inline skinning. Skips both the compute pre-pass and
	// CPU skinning; the GBuffer VS reads bind-pose verts from IA and
	// runs the skinning math inline.
	bool bSkeletalUseVsInlineSkinning = false;
	std::shared_ptr<GraphicsPipelineHandle> SkeletalVsInlineGraphicsPipeline;
	// Phase D: desktop-only cluster-draw variant of the VS inline PSO.
	// One DrawIndexedInstanced(IndexCount, CharCount, ...) replaces N
	// per-mesh draws. Mobile (GBufferMobile.hlsl) lacks the matching
	// shader entry, so the SPIR-V build skips it and this stays null.
	std::shared_ptr<GraphicsPipelineHandle> SkeletalVsInlineClusterGraphicsPipeline;
	bool DrawSkeletalVsInlineClusterDesktop();
	struct SkinBoneRowCpu { float r0[4]; float r1[4]; float r2[4]; };
	std::vector<SkinBoneRowCpu> SkeletalUnifiedPaletteCpu;
	std::vector<SkinBoneRowCpu> SkeletalUnifiedPalettePrevCpu;
	std::shared_ptr<VertexBuffer> SkeletalUnifiedCpuSkinnedVb;
	// Ring buffer of previous-frame CPU-skinned VBs so the D3D12 debug
	// layer doesn't trip "object deleted while still in use" when we
	// re-allocate per frame from the upload pool. Sized for the max
	// number of frames in flight.
	std::array<std::shared_ptr<VertexBuffer>, 4> SkeletalUnifiedCpuSkinnedVbRing;
	uint32_t SkeletalUnifiedCpuSkinnedVbRingIndex = 0;
	// Cached bind-pose data so the CPU skin pass doesn't need to read
	// back from a GPU SBV. Filled at spawn from the same arrays that
	// build SkeletalUnifiedInputVertices.
	struct SkinInputVertexCpu
	{
		float BindPosition[3];
		float BindNormal[3];
		float BindTangent[3];
		float UV[2];
		uint32_t BoneIndicesPacked;
		float BoneWeights[4];
	};
	std::vector<SkinInputVertexCpu> SkeletalUnifiedBindPoseCpu;
	void UpdateSkeletalUnifiedInstanceTransforms();
	void CpuSkinSkeletalCharactersForRenderWorld();
	void DumpSkeletalFrameStatsToTrace();
	struct StaticGBufferInstanceXform { float r0[4]; float r1[4]; float r2[4]; };
	std::vector<StaticGBufferInstanceXform> StaticGBufferInstanceTransformScratch;
	std::vector<std::array<std::shared_ptr<Buffer>, 4>> StaticGBufferInstanceTransformBuffers;
	uint32_t StaticGBufferInstanceTransformDrawIndex = 0;
	Buffer* AcquireStaticGBufferInstanceTransformBuffer(uint32_t instanceCount);
	bool IsSceneEligibleForStaticGBufferInstancing(const std::shared_ptr<Scene>& scene) const;
	bool DrawStaticInstancedScene(
		const std::shared_ptr<Scene>& scene,
		const std::vector<const SceneObject*>& objects,
		float roughness,
		float metallic,
		bool overrideRoughnessMetallic);
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
	void ApplyDefaultFlyCamera();
	void EnsureWindowFramebuffers();

	void ToneMapPass();

	void DebugPass();

	void LightingPass();

	// Forward-translucent particle draw against the post-light HDR buffer.
	// Walks ActiveParticleSystems and uploads + draws each system's quads.
	void ParticlePass();
	void UpdateParticleSystems(float dt);

	// Spawn-burst the first active particle system. Exposed via the Luau
	// binding `corona.particle_burst(x, y, z, count)`. Returns the number
	// actually spawned.
	uint32_t ParticleBurstForScript(float x, float y, float z, uint32_t count);

	// Toggle between the script-owned active camera and SimpleCamera. Called
	// from Lua via `corona.use_native_camera(enabled)`.
	void UseNativeCameraForScript(bool enabled);

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
	void ApplyDiffuseGIMode(EDiffuseGIMode requestedMode);
	bool RenderResolutionResourcesMatchCurrentState() const;
	void ResetAllAccumulationState(bool forceUpscaleReload);
	void ResetTemporalHistoryBuffers();
	void ClearDisabledGIOutputBuffers(bool clearDiffuseGI, bool clearSpecularGI);
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
	void HandleStreamlineFeatureFailure(sl::Feature feature, sl::Result result, const wchar_t* operation);
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
	bool IsEditorStartupMode() const { return bEnableStartupLuauScript && StartupLuauMode == L"editor"; }
	void DrawEditorModeOverlay();

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

	// Quake-style ~/` toggled console with TripoSR dispatcher (Phase 1
	// natural-language asset generation pipeline). Lazy-initialized inside
	// the ImGui pass so headless/mobile builds without ImGui don't pay
	// for it.
	std::unique_ptr<class CoronaConsole> Console;
	// Left-anchored scene inspector — entity list + per-entity component
	// editor. Toggled via a floating "Show Scene" button at the bottom-left.
	std::unique_ptr<class CoronaSceneInspector> SceneInspector;
	// Bin/assets file browser — right-click to spawn models, load maps,
	// delete files. Toggled via a button next to the scene inspector toggle.
	std::unique_ptr<class CoronaAssetExplorer> AssetExplorer;
	// Spawn toolbox — directional/point lights, primitive shapes. Toggled
	// via the third bottom button.
	std::unique_ptr<class CoronaToolbox> Toolbox;

private:
	std::wstring GetAssetFullPath(LPCWSTR assetName) const;
	void SetCustomWindowText(LPCWSTR text);
	void PumpStartupWindowMessages();
	void ProcessPendingEditorMapLoad();
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
	void SyncCurrentLightingSettingsToFrameSourceState();
	void CollectFrameSourceRenderSync(RenderFrameDelta& delta);
	void ApplyFrameSourceRenderSync(const RenderFrameDelta& delta);
	void CollectSceneObjectRenderSync(RenderFrameDelta& delta);
	void ApplySceneObjectRenderSync(const RenderFrameDelta& delta);
	void CollectPointLightRenderSync(RenderFrameDelta& delta);
	void ApplyPointLightRenderSync(const RenderFrameDelta& delta);
	void BuildRenderFrameDerivedState(const RenderFrameSourceState* sourceState);
	void BuildPointLightRenderCandidates(std::vector<const PointLightState*>& outCandidates) const;
	PointLightParam BuildPointLightParam(const PointLightState& pointLight) const;
	void FillPointLightParams(PointLightParam* outPointLights, UINT32& outPointLightCount, UINT32 maxCount) const;
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
	std::wstring StartupLoadingTitle;
	CpuClock::time_point StartupLoadingTimingStart = {};
	CpuClock::time_point StartupLoadingTimingLast = {};
	CpuClock::time_point StartupLoadingLastDrawTime = {};
	std::wstring StartupLoadingTimingLastStatus;
	bool bStartupLoadingTimingStarted = false;
	bool bStartupLoadingScreenActive = false;
	bool bStartupLoadingCompactWindow = false;
	std::mutex GameRenderStateMutex;
	std::mutex GameThreadMutex;
	std::condition_variable GameThreadCv;
	std::thread GameThread;
	bool bSplitGameRenderThreads = true;
	bool bGameThreadStarted = false;
	bool bGameThreadStopRequested = false;
	bool bGameFrameReady = false;
};
