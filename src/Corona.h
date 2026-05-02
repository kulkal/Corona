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
#include "RenderBackend.h"
#include "SimpleDX12.h"
#include "enkiTS/TaskScheduler.h"
#define PROFILE_BUILD 1
#include "pix3.h"
#ifndef WITH_STREAMLINE
#define WITH_STREAMLINE 0
#endif
#if WITH_STREAMLINE
#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss.h"
#include "sl_dlss_d.h"
#endif
using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().
using Microsoft::WRL::ComPtr;
using namespace std;

struct lua_State;

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
		SPATIAL_FILTERED_DIFFUSE_GI,
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
		ShadowDenoise,
		RaytraceAO,
		RaytraceSkyLighting,
		RaytraceReflection,
		RaytraceGI,
		ScreenProbeGI,
		TemporalDenoise,
		SpatialDenoise,
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

	static constexpr UINT32 GpuPassCount = static_cast<UINT32>(EGpuPass::Count);
	static constexpr UINT32 GpuQueriesPerPass = 2;
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
	shared_ptr<Texture> ShadowBuffer;
	shared_ptr<Texture> ShadowDenoisedBuffer;
	shared_ptr<Texture> AmbientOcclusionBuffer;
	shared_ptr<Texture> SkyLightingRawBuffer;
	shared_ptr<Texture> SkyLightingBuffer;

	shared_ptr<Texture> SpecularGIRaw;

	shared_ptr<Texture> SpecularGITemporal[2];
	shared_ptr<Texture> SpecularGISpatial[2];

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

	shared_ptr<Texture> DiffuseGISpatialAux[2];
	shared_ptr<Texture> DiffuseGISpatial[2];

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
		glm::vec2 RTSize;
		glm::vec2 RougnessMetalic;
		UINT32 bOverrideRougnessMetallic;
		UINT32 Padding[3] = {};
	};

	std::shared_ptr<GraphicsPipelineHandle> GBufferGraphicsPipeline;

	// spatial denoising
	struct SpatialFilterConstant
	{
		glm::vec4 ProjectionParams;
		UINT32 Iteration;
		UINT32 GIBufferScale;
		UINT32 AccumulatedFrames = 0;
		UINT32 Padding = 0;
		float IndirectDiffuseWeightFactorDepth = 0.5f;
		float IndirectDiffuseWeightFactorNormal = 1.0f;
		float IndirectSpecularWeightFactorDepth = 0.5f;
		float IndirectSpecularWeightFactorNormal = 2.0f;
		float IndirectSpecularLuminanceWeight = 1.5f;
		float IndirectSpecularEnergyPreservation = 0.85f;
	};

	SpatialFilterConstant SpatialFilterCB;

	shared_ptr<ComputePipelineStateObject> SpatialDenoisingFilterPSO;



	// temporal denoising
	struct TemporalFilterConstant
	{
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 InvProjMatrix;
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
		UINT32 ShadowSampleCount = 8;
		glm::vec2 _padding;
		glm::vec4 pad;
	};

	RTShadowViewParamCB RTShadowViewParam;
	
	shared_ptr<RTPipelineStateObject> PSO_RT_SHADOW;

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
		UINT32 SampleCount = 16;
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

	struct SkyLightingDenoiseCB
	{
		glm::vec4 ProjectionParams;
		glm::vec2 RTSize;
		float DepthSigma = 48.0f;
		float NormalSigma = 48.0f;
		float VisibilitySigma = 8.0f;
		UINT32 Radius = 5;
		glm::vec2 _padding = glm::vec2(0.0f);
	};
	SkyLightingDenoiseCB SkyLightingDenoiseParam;
	shared_ptr<ComputePipelineStateObject> SkyLightingDenoisePSO;

	struct ShadowDenoiseCB
	{
		glm::vec4 ProjectionParams;
		glm::vec2 RTSize;
		float DepthSigma = 32.0f;
		float NormalSigma = 64.0f;
	};
	ShadowDenoiseCB ShadowDenoiseParam;
	shared_ptr<ComputePipelineStateObject> ShadowDenoisePSO;


	// RT reflection
	struct RTReflectionViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::mat4x4 InvProjMatrix;
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
		glm::vec2 _prefilteredEnvPadding = glm::vec2(0.0f);
	};

	RTReflectionViewParamCB RTReflectionViewParam;
	
	shared_ptr<RTPipelineStateObject> PSO_RT_REFLECTION;

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
		glm::vec2 _noisePadding;
		glm::vec3 SkyColorTop;
		float SkyIntensity;
		glm::vec3 SkyColorBottom;
		float _padding;
		glm::vec3 LightColor;
		float _padding2;
	};

	RTGIViewParamCB RTGIViewParam;
	shared_ptr<RTPipelineStateObject> PSO_RT_GI;

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
		UINT32 Padding = 0;
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
		UINT32 _padding3 = 0;
		UINT32 _padding4 = 0;
		UINT32 _padding5 = 0;
	};

	RTSpatialHashGIViewParamCB RTSpatialHashGIViewParam;
	shared_ptr<RTPipelineStateObject> PSO_RT_SPATIAL_HASH_GI;
	
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
		UINT32 _rtaoPadding[3] = {};
	};

	PathTracingViewParamCB PathTracingViewParam;
	shared_ptr<RTPipelineStateObject> PSO_PATH_TRACING;
	shared_ptr<Texture> PathTracingAccumBuffer[2];
	UINT PathTracingWriteIndex = 0;
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
	shared_ptr<PipelineStateObject> ToneMapPSO;
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

	shared_ptr<PipelineStateObject> BufferVisualizePSO;

	// lighting pass
	
	struct LightingParam
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
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
	};
	
	shared_ptr<PipelineStateObject> LightingPSO;
	std::shared_ptr<GraphicsPipelineHandle> LightingGraphicsPipeline;

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

	struct SceneObjectDesc
	{
		shared_ptr<Scene> ScenePtr;
		glm::mat4x4 Transform = glm::mat4x4(1.0f);
		float Roughness = 1.0f;
		float Metallic = 0.0f;
		bool bOverrideRoughnessMetallic = false;
		bool bVisible = true;
		bool bRayTracing = true;
		bool bPhysicsQuery = true;
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
	bool bEnableSpecularGI = true;
	bool bEnableDirectDiffuse = true;
	bool bEnableDirectSpecular = true;
	bool bEnableRTAO = true;
	bool bEnableSkyLighting = true;
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
	bool bAASwitchDumpMode = false;
	bool bSpecularSequenceDumpMode = false;
	bool bLoggedHybridStageLimit = false;
	bool bStartupModeConfigured = false;
	UINT32 AutoAADumpPhase = 0;
	UINT32 AutoAADumpFramesInPhase = 0;
	UINT32 AutoAADumpFrameCountOverride = 0;
	UINT32 DiffuseGIAutoDumpFrameCount = 96;
	std::wstring AutoAADumpDir;
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
	bool bCommandLineDiffuseGIAutoDumpMode = false;
	bool bCommandLineReadmeScreenshotDumpMode = false;
	bool bCommandLinePathTracingScreenshotDumpMode = false;
	bool bCommandLineLightingCompareDumpMode = false;
	bool bCommandLineAASwitchDumpMode = false;
	bool bCommandLineSpecularSequenceDumpMode = false;
	bool bCommandLineCameraPathDump = false;
	bool bCommandLineLoadLatestCameraPath = false;
	std::wstring CommandLineCameraPathFile;

	shared_ptr<PipelineStateObject> TemporalAAPSO;
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

	shared_ptr<PipelineStateObject> BloomBlurPSO;

	shared_ptr<PipelineStateObject> BloomExtractPSO;

	shared_ptr<PipelineStateObject> HistogramPSO;

	shared_ptr<PipelineStateObject> ClearHistogramPSO;

	bool bDrawHistogram = false;
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

	shared_ptr<VertexBuffer> FullScreenVB;

	// blue noise texture
	shared_ptr<Texture> BlueNoiseTex;
	shared_ptr<Texture> DefaultWhiteTex;
	shared_ptr<Texture> DefaultBlackTex;
	shared_ptr<Texture> DefaultNormalTex;
	shared_ptr<Texture> DefaultRougnessTex;

	// global wrap sampler
	std::shared_ptr<Sampler> samplerWrap;
	std::shared_ptr<Sampler> samplerBilinearWrap;


	// mesh
	shared_ptr<Mesh> mesh;

	float SponzaRoughnessMultiplier = 1;
	shared_ptr<Scene> Sponza;
	SceneObjectHandle SponzaObject = InvalidSceneObjectHandle;

	shared_ptr<Scene> Buddha;
	SceneObjectHandle BuddhaObject = InvalidSceneObjectHandle;
	glm::vec3 BuddhaCenterPosition = glm::vec3(273.0f, -12.0f, -6.0f);
	glm::vec3 BuddhaCenterRotationDegrees = glm::vec3(0.0f, -12.0f, 0.0f);

	float ShaderBallRoughnessMultiplier = 0.15;
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
		shared_ptr<Scene> ScenePtr;
		glm::mat4x4 Transform = glm::mat4x4(1.0f);
		float Roughness = 1.0f;
		float Metallic = 0.0f;
		bool bOverrideRoughnessMetallic = false;
		bool bVisible = true;
		bool bRayTracing = true;
		bool bPhysicsQuery = true;
	};
	vector<SceneObject> SceneObjects;
	SceneObjectHandle NextSceneObjectHandle = 1;
	bool bRayTracingSceneDirty = false;
	bool bRayTracingTransformDirty = false;

	struct ScriptSceneEntry
	{
		shared_ptr<Scene> ScenePtr;
		std::wstring Path;
	};
	struct ScriptObjectState
	{
		ScriptSceneHandle SceneHandle = InvalidScriptSceneHandle;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 RotationDegrees = glm::vec3(0.0f);
		float TargetExtent = 1.0f;
	};
	struct LuauScriptState
	{
		lua_State* L = nullptr;
		int UpdateRef = 0;
	};
	std::unique_ptr<LuauScriptState> ScriptState;
	std::map<ScriptSceneHandle, ScriptSceneEntry> ScriptScenes;
	std::map<std::wstring, ScriptSceneHandle> ScriptSceneByPath;
	std::map<SceneObjectHandle, ScriptObjectState> ScriptObjects;
	ScriptSceneHandle NextScriptSceneHandle = 1;
	bool bEnableStartupLuauScript = true;
	bool bScriptCameraControlEnabled = false;
	std::array<bool, 256> ScriptKeyDown = {};
	std::array<bool, 256> ScriptKeyPressed = {};
	std::array<bool, 256> ScriptKeyReleased = {};
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
	glm::mat4x4 ViewProjMat;
	glm::mat4x4 InvViewProjMat;
	glm::mat4x4 PrevViewMat;
	glm::mat4x4 PrevViewProjMat;
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
		UINT32 Padding = 0;
	};

	std::shared_ptr<Buffer> InstancePropertyBuffer;
	shared_ptr<RTAS> TLAS;
	std::map<Mesh*, std::shared_ptr<RTAS>> RayTracingBLASCache;
	std::vector<RTInstanceDesc> RayTracingInstances;
	
	// ...
	bool bMultiThreadRendering = false;

	bool bDebugDraw = false;


	UINT m_frameCounter = 0;

	UINT FrameCounter = 0;

	// Pipeline objects.
	CD3DX12_VIEWPORT m_viewport;
	CD3DX12_RECT m_scissorRect;
	ComPtr<ID3D12Device5> m_device;
	std::unique_ptr<IRenderBackend> renderBackend;
	SimpleDX12* dx12_rhi = nullptr;

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
	bool bFramePerfLogInitialized = false;
	UINT64 FramePerfLogTotalFrameCount = 0;
	UINT32 FramePerfLogSampleFrameCount = 0;
	double FramePerfLogAccumFrameMs = 0.0;
	double FramePerfLogMinFrameMs = 1.0e30;
	double FramePerfLogMaxFrameMs = 0.0;
	double FramePerfLogAccumBeginFrameMs = 0.0;
	double FramePerfLogAccumRecordMs = 0.0;
	double FramePerfLogAccumExecuteMs = 0.0;
	double FramePerfLogAccumEndFrameMs = 0.0;
	std::array<float, GpuPassCount> CpuPassLastTimeMs = {};
	std::array<double, GpuPassCount> CpuPassAccumTimeMs = {};
	std::array<uint8_t, GpuPassCount> CpuPassActiveMask = {};
	std::array<CpuClock::time_point, GpuPassCount> CpuPassStartTimes = {};
	CpuClock::time_point FramePerfLogFrameStart = {};
	CpuClock::time_point FramePerfLogLastFlush = {};
	void RecompileShaders();
	void BeginFramePerfLogging();
	void FinishFramePerfLogging(double beginFrameMs, double executeMs, double endFrameMs);
	void InitGpuTimingResources();
	void BeginGpuTimingFrame();
	void ResolveGpuTimingFrame();
	void UpdateGpuTimingReadback();
	void BeginGpuPassTiming(EGpuPass pass);
	void EndGpuPassTiming(EGpuPass pass);
	const char* GetGpuPassName(EGpuPass pass) const;
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
	void RequestCameraPathDumpFrameCapture();
	void ConsumeCameraPathDumpCaptureResult();
	void LaunchCameraPathVideoEncode();
	double GetCameraPathDurationSeconds() const;

	struct RTSceneHitProgramDesc
	{
		const char* HitGroup = "HitGroup";
		bool bBindSceneGeometry = true;
		bool bBindDiffuseTexture = true;
		bool bBindInstanceProperty = true;
		bool bBindInstancePropertyBeforeDiffuse = false;
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
	SceneObjectHandle AddCenteredSceneObject(
		const shared_ptr<Scene>& scene,
		float targetExtent,
		float roughness,
		float metallic,
		bool bOverrideRoughnessMetallic,
		const glm::vec3& position = glm::vec3(0.0f),
		const glm::vec3& rotationDegrees = glm::vec3(0.0f));
	shared_ptr<Scene> CreateMirrorCubeScene();
	void MarkRayTracingSceneDirty();
	void MarkRayTracingTransformsDirty();
	void FlushSceneObjectChanges();
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
	void InitRaytracingShadowPass();
	void InitRaytracingReflectionPass();
	void InitRaytracingSimpleGIPass();
	void InitRaytracingScreenProbePass();
	void InitRaytracingSpatialHashPass();
	
public:

	void InitRaytracingData();
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
	ScriptSceneHandle LoadSceneForScript(const std::wstring& assetPath);
	SceneObjectHandle SpawnSceneObjectForScript(
		ScriptSceneHandle sceneHandle,
		const glm::vec3& position,
		const glm::vec3& rotationDegrees,
		float targetExtent,
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
		float targetExtent);
	bool GetSceneObjectTransformForScript(
		SceneObjectHandle handle,
		glm::vec3& position,
		glm::vec3& rotationDegrees,
		float& targetExtent) const;
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
	bool IsScriptKeyDownForScript(UINT8 key) const;
	bool WasScriptKeyPressedForScript(UINT8 key) const;
	bool WasScriptKeyReleasedForScript(UINT8 key) const;
	void ClearScriptInputFrameState();
	void InitLuauScripting();
	void RunStartupLuauScript();
	void ReloadLuauScripting();
	void UpdateLuauScripting(float dt);
	void ShutdownLuauScripting();
	

	void LoadPipeline();

	void LoadAssets();

	shared_ptr<Scene> LoadModel(string fileName);
	shared_ptr<Scene> LoadBinaryMeshModel(const std::wstring& binaryFileName, const std::wstring& sourceFileName);

	void InitRTPSO();

	void InitSpatialDenoisingPass();

	void InitTemporalDenoisingPass();
	void InitScreenProbeGIPass();
	void InitSpatialHashGIPass();

	void InitGBufferPass();

	void InitToneMapPass();

	void InitDebugPass();

	void InitLightingPass();

	void InitShadowDenoisePass();
	void InitSkyLightingDenoisePass();

	void InitTemporalAAPass();

	void InitBloomPass();

	void InitGenMipSpecularGIPass();

	void InitImgui();
	bool LoadCameraState();
	void SaveCameraState();
	std::wstring GetCameraStatePath();

	void InitBlueNoiseTexture();

	void DrawScene(shared_ptr<Scene> scene, const glm::mat4x4& instanceTransform, float Roughness, float Metalic, bool bOverrideRoughnessMetallic);

	void GBufferPass();

	void RaytraceShadowPass();

	void ShadowDenoisePass();
	void InitRaytracingAOPass();
	void RaytraceAOPass();
	void InitRaytracingSkyLightingPass();
	void RaytraceSkyLightingPass();
	void SkyLightingDenoisePass();

	void RaytraceReflectionPass();

	void RaytraceGIPass();
	void SpatialHashGIPass();
	void ScreenProbeRaytraceGIPass();
	void ScreenProbeGIPass();

	void SpatialDenoisingPass();


	void TemporalDenoisingPass();

	void BloomPass();

	void InitPathTracingPass();

	void PathTracingPass();
	void ApplyHybridDefaultCamera();
	void EnsureWindowFramebuffers();

	void ToneMapPass();

	void DebugPass();

	void LightingPass();

	void TemporalAAPass();

	bool IsTemporalAAEnabled() const { return AntiAliasingMode == EAntiAliasingMode::TAA; }
	bool IsDLSSSREnabled() const { return AntiAliasingMode == EAntiAliasingMode::DLSS_SR && bDLSSAvailable; }
	bool IsDLSSRREnabled() const { return AntiAliasingMode == EAntiAliasingMode::DLSS_RR && bDLSSRRAvailable; }
	bool IsDLSSUpscaleEnabled() const { return IsDLSSSREnabled() || IsDLSSRREnabled(); }
	bool IsJitterEnabled() const { return IsTemporalAAEnabled() || IsDLSSUpscaleEnabled(); }
	UINT GetRenderWidth() const { return IsDLSSUpscaleEnabled() && RenderingMode == ERenderingMode::HYBRID ? RenderWidth : m_width; }
	UINT GetRenderHeight() const { return IsDLSSUpscaleEnabled() && RenderingMode == ERenderingMode::HYBRID ? RenderHeight : m_height; }
	UINT GetWidth() const { return m_width; }
	UINT GetHeight() const { return m_height; }
	const WCHAR* GetTitle() const { return m_title.c_str(); }

	void GenMipSpecularGIPass();
	void ResetAllAccumulationState(bool forceUpscaleReload);
	void ResetTemporalHistoryBuffers();
	void RecreateRenderResolutionResources();
	void ReloadRenderResolutionAssets();
	void RefreshUpscaleSettings(bool reloadAssets);
	Texture* GetCurrentResolveSource() const;
	void PromptStartupModeSelection();
	void InitializeAutoAADump();
	void AdvanceAutoAADump(Texture* backbuffer);
	void AppendAutoAADumpLog(const std::wstring& line);
	bool IsHybridStageAutoDumpPhase() const;
	const wchar_t* GetHybridStageAutoDumpPhaseName(uint32_t phase) const;
	bool DumpTextureHDR(Texture* source, const std::wstring& filePath, D3D12_RESOURCE_STATES beforeState);
	bool DumpTexturePNG(Texture* source, const std::wstring& filePath, D3D12_RESOURCE_STATES beforeState);
	bool StartAsyncImageDumpWorkers();
	void AsyncImageDumpWorkerMain();
	void WaitForAsyncImageDumps();
	void StopAsyncImageDumpWorkers();
	bool EnqueueAsyncImageDump(DirectX::ScratchImage&& captured, const std::wstring& filePath, bool bHDR);
#if WITH_STREAMLINE
	void InitStreamline();
	void ShutdownStreamline();
	bool BeginStreamlineFrame();
	bool EnsureStreamlineConstants();
	bool DLSSPass();
	bool DLSSRRPass();
#endif

	void OnInit();

	void OnUpdate();

	void OnRender();

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

	UINT m_width = 0;
	UINT m_height = 0;
	float m_aspectRatio = 1.0f;
	bool m_useWarpDevice = false;
	std::wstring m_assetsPath;
	std::wstring m_title;
	std::string m_imguiIniPath;
	std::string m_imguiLogPath;
};
