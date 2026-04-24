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
#include <deque>
#include <vector>

#include "glm/glm.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/transform.hpp"
#include "glm/mat4x4.hpp"
#include "glm/fwd.hpp"
#include "glm/gtc/quaternion.hpp"

#include "DXSample.h"
#include "StepTimer.h"
#include "SimpleCamera.h"
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


class Corona : public DXSample
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
		RaytraceReflection,
		RaytraceGI,
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

	shared_ptr<Texture> DepthBuffer;
	shared_ptr<Texture> UnjitteredDepthBuffers[2];

	UINT ColorBufferWriteIndex = 0;
	UINT ResolvedColorBufferIndex = 0;
	shared_ptr<Texture> ColorBuffers[2];
	shared_ptr<Texture> LightingBuffer;
	shared_ptr<Texture> AlbedoBuffer;
	shared_ptr<Texture> SpecularAlbedoBuffer;
	shared_ptr<Texture> NormalBuffers[2];
	shared_ptr<Texture> GeomNormalBuffer;
	shared_ptr<Texture> VelocityBuffer;
	shared_ptr<Texture> RoughnessMetalicBuffer;
	shared_ptr<Texture> ShadowBuffer;
	shared_ptr<Texture> ShadowDenoisedBuffer;

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
	};

	shared_ptr<PipelineStateObject> GBufferPassPSO;

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
	};

	SpatialFilterConstant SpatialFilterCB;

	shared_ptr<PipelineStateObject> SpatialDenoisingFilterPSO;



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
		glm::vec2 Padding = glm::vec2(0.0f);
	};

	TemporalFilterConstant TemporalFilterCB;

	shared_ptr<PipelineStateObject> TemporalDenoisingFilterPSO;
	
	// RT shadow
	struct RTShadowViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4 LightDir;
		float ShadowLightRadius = 0.03f;
		UINT32 ShadowSampleCount = 8;
		glm::vec2 _padding;
		glm::vec4 pad;
	};

	RTShadowViewParamCB RTShadowViewParam;
	
	shared_ptr<RTPipelineStateObject> PSO_RT_SHADOW;
	struct ShadowDenoiseCB
	{
		glm::vec4 ProjectionParams;
		glm::vec2 RTSize;
		float DepthSigma = 32.0f;
		float NormalSigma = 64.0f;
	};
	ShadowDenoiseCB ShadowDenoiseParam;
	shared_ptr<PipelineStateObject> ShadowDenoisePSO;


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
		glm::vec3 SkyColorTop;
		float SkyIntensity;
		glm::vec3 SkyColorBottom;
		float _padding;
		glm::vec3 LightColor;
		float _padding2;
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
		glm::vec3 SkyColorTop;
		float SkyIntensity;
		glm::vec3 SkyColorBottom;
		float _padding;
		glm::vec3 LightColor;
		float _padding2;
	};

	RTGIViewParamCB RTGIViewParam;
	shared_ptr<RTPipelineStateObject> PSO_RT_GI;
	
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
		COUNT = 7,
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
	};
	
	shared_ptr<PipelineStateObject> LightingPSO;

	// temporalAA
	struct TemporalAAParam
	{
		glm::vec2 RTSize;
		float TAABlendFactor;
		UINT32 ClampMode;
		float BloomStrength;
		UINT32 HistoryValid;
		glm::vec3 _padding;
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

private:

	EAntiAliasingMode AntiAliasingMode = EAntiAliasingMode::DLSS_RR;
	EDLSSQualityMode DLSSQualityMode = EDLSSQualityMode::QUALITY;
	bool bEnableDiffuseGI = true;
	bool bEnableSpecularGI = true;
	bool bEnableDirectDiffuse = true;
	bool bEnableDirectSpecular = true;

	UINT32 ClampMode = 2;

	float JitterScale = 0.6;
	UINT32 TAASampleCount = 32;
	UINT32 DLSSJitterPhaseCount = 32;
	UINT RenderWidth = 0;
	UINT RenderHeight = 0;
	bool bPendingUpscaleRefresh = false;
	bool bForceUpscaleReload = false;
	bool bResetTemporalStateNextUpdate = false;
	UINT32 DLSSTransitionFramesRemaining = 0;
	bool bUseLightingBufferFallbackForToneMap = false;
	bool bAutoAADumpEnabled = true;
	bool bAutoAADumpInitialized = false;
	bool bAutoAADumpCompleted = false;
	bool bStartupModeConfigured = false;
	UINT32 AutoAADumpPhase = 0;
	UINT32 AutoAADumpFramesInPhase = 0;
	std::wstring AutoAADumpDir;
	EAntiAliasingMode StartupSelectedAAMode = EAntiAliasingMode::DLSS_RR;
	ERenderingMode StartupRenderingMode = ERenderingMode::HYBRID;
	bool bCommandLineAutoDumpOverrideSet = false;
	bool bCommandLineAutoDumpEnabled = false;
	bool bCommandLineAAOverrideSet = false;
	EAntiAliasingMode CommandLineSelectedAAMode = EAntiAliasingMode::DLSS_RR;
	bool bCommandLineRenderModeOverrideSet = false;
	ERenderingMode CommandLineRenderingMode = ERenderingMode::HYBRID;

	shared_ptr<PipelineStateObject> TemporalAAPSO;
	bool bTemporalAAHistoryValid = false;
	bool bTemporalDenoiserHistoryValid = false;
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




	// imgui font texture
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleImguiFontTex;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleImguiFontTex;

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

	shared_ptr<Scene> Buddha;

	float ShaderBallRoughnessMultiplier = 0.15;
	shared_ptr<Scene> ShaderBall;

	// time & camera
	StepTimer m_timer;

	float m_turnSpeed = glm::half_pi<float>();

	SimpleCamera m_camera;

	// misc
	glm::vec3 LightDir = glm::normalize(glm::vec3(0.901, 0.88, 0.176));
	float LightIntensity = 0.4;
	
	// Sky colors for path tracing
	glm::vec3 SkyColorTop = glm::vec3(1.0f, 1.0f, 1.0f);
	glm::vec3 SkyColorBottom = glm::vec3(0.8f, 0.8f, 0.8f);
	float SkyIntensity = 3.0f;
	
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
	};

	std::shared_ptr<Buffer> InstancePropertyBuffer;
	shared_ptr<RTAS> TLAS;
	vector<shared_ptr<RTAS>> vecBLAS;
	
	// ...
	bool bMultiThreadRendering = false;

	bool bDebugDraw = false;


	UINT m_frameCounter = 0;

	UINT FrameCounter = 0;

	// Pipeline objects.
	CD3DX12_VIEWPORT m_viewport;
	CD3DX12_RECT m_scissorRect;
	ComPtr<IDXGISwapChain3> m_swapChain;
	ComPtr<ID3D12Device5> m_device;
	std::unique_ptr<SimpleDX12> dx12_rhi;

	enki::TaskScheduler g_TS;

	bool bRecompileShaders = false;
	bool bShowImgui = true;
	bool bShowGpuTimingWindow = false;
	bool bGpuTimingResourcesInitialized = false;
	UINT32 GpuTimingAverageFrameCount = 30;
	UINT64 GpuTimestampFrequency = 0;
	ComPtr<ID3D12QueryHeap> GpuTimestampQueryHeap;
	ComPtr<ID3D12Resource> GpuTimestampReadbackBuffer;
	UINT64* GpuTimestampReadbackMapped = nullptr;
	std::array<std::array<uint8_t, GpuPassCount>, 3> GpuPassActiveMaskPerFrame = {};
	std::array<float, GpuPassCount> GpuPassLastTimeMs = {};
	std::array<float, GpuPassCount> GpuPassAverageTimeMs = {};
	std::array<std::deque<float>, GpuPassCount> GpuPassHistoryMs = {};
	void RecompileShaders();
	void InitGpuTimingResources();
	void BeginGpuTimingFrame();
	void ResolveGpuTimingFrame();
	void UpdateGpuTimingReadback();
	void BeginGpuPassTiming(EGpuPass pass);
	void EndGpuPassTiming(EGpuPass pass);
	const char* GetGpuPassName(EGpuPass pass) const;
	
	// Raytracing helper functions
	void UpdateInstancePropertyBuffer();
	void RebuildAccelerationStructures();
	
public:

	void InitRaytracingData();
	void AddScene(shared_ptr<Scene> scene);
	

	void LoadPipeline();

	void LoadAssets();

	shared_ptr<Scene> LoadModel(string fileName);

	void InitRTPSO();

	void InitSpatialDenoisingPass();

	void InitTemporalDenoisingPass();

	void InitGBufferPass();

	void InitToneMapPass();

	void InitDebugPass();

	void InitLightingPass();

	void InitShadowDenoisePass();

	void InitTemporalAAPass();

	void InitBloomPass();

	void InitGenMipSpecularGIPass();

	void InitImgui();
	bool LoadCameraState();
	void SaveCameraState();
	std::wstring GetCameraStatePath();

	void InitBlueNoiseTexture();

	void DrawScene(shared_ptr<Scene> scene, float Roughness, float Metalic, bool bOverrideRoughnessMetallic);

	void GBufferPass();

	void RaytraceShadowPass();

	void ShadowDenoisePass();

	void RaytraceReflectionPass();

	void RaytraceGIPass();

	void SpatialDenoisingPass();


	void TemporalDenoisingPass();

	void BloomPass();

	void InitPathTracingPass();

	void PathTracingPass();

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

	void GenMipSpecularGIPass();
	void ResetAllAccumulationState(bool forceUpscaleReload);
	void ResetTemporalHistoryBuffers();
	void ReloadRenderResolutionAssets();
	void RefreshUpscaleSettings(bool reloadAssets);
	Texture* GetCurrentResolveSource() const;
	void PromptStartupModeSelection();
	void InitializeAutoAADump();
	void AdvanceAutoAADump(Texture* backbuffer);
	void AppendAutoAADumpLog(const std::wstring& line);
	bool DumpTextureHDR(Texture* source, const std::wstring& filePath, D3D12_RESOURCE_STATES beforeState);
	bool DumpTexturePNG(Texture* source, const std::wstring& filePath, D3D12_RESOURCE_STATES beforeState);
#if WITH_STREAMLINE
	void InitStreamline();
	void ShutdownStreamline();
	bool BeginStreamlineFrame();
	bool EnsureStreamlineConstants();
	bool DLSSPass();
	bool DLSSRRPass();
#endif

	// DXSample functions
	virtual void OnInit();

	virtual void OnUpdate();

	virtual void OnRender();

	virtual void OnDestroy();

	virtual void ParseCommandLineArgs(_In_reads_(argc) WCHAR* argv[], int argc) override;

	virtual void OnKeyDown(UINT8 key);

	virtual void OnKeyUp(UINT8 key);
	
	virtual void OnRButtonDown(int x, int y);
	virtual void OnRButtonUp();
	virtual void OnMouseMove(int x, int y);

	Corona(UINT width, UINT height, std::wstring name);

	virtual ~Corona();

	
};
