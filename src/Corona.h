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

#include "glm/glm.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/transform.hpp"
#include "glm/mat4x4.hpp"
#include "glm/fwd.hpp"
#include "glm/gtc/quaternion.hpp"

#include "DXSample.h"
#include "StepTimer.h"
#include "SimpleCamera.h"
#include "AbstractGfxLayer.h"
#include "enkiTS/TaskScheduler.h""
#define PROFILE_BUILD 1
#include "pix3.h"

#define USE_DLSS 0
#define USE_NRD 0
#define RTXGI 1
#define USE_IMGUI 0
#define USE_GIZMO 0
#define USE_AFTERMATH 0

#define VULKAN_RENDERER 1

#if USE_DLSS
#include "nvsdk_ngx.h"
#include <nvsdk_ngx_helpers.h>

#define DLSS_APP_ID 100689411
#endif //USE_DLSS

#if RTXGI
#include <rtxgi/Types.h>
#include <rtxgi/ddgi/DDGIVolume.h>
#endif // RTXGI

#if USE_NRD
constexpr uint32_t BUFFERED_FRAME_MAX_NUM = 2;
#include "NRD.h"
#include "NRI.h"
#include "NRDIntegration.h"

#include "NRIDeviceCreation.h"
#include "NRIWrapperD3D12.h"
#endif // USE_NRD





// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().
//using Microsoft::WRL::ComPtr;
using namespace std;

#if USE_NRD
struct NRIInterface
	: public nri::CoreInterface
	, public nri::WrapperD3D12Interface
	//, public nri::SwapChainInterface
	//, public nri::RayTracingInterface
{};
#endif // USE_NRD



class Corona : public DXSample
{
	enum class EDebugVisualization
	{
		SHADOW,
		WORLD_NORMAL,
		GEO_NORMAL,
		DEPTH,
		RAW_SH,
		RAW_CoCg,
		TEMPORAL_FILTERED_SH,
		SPATIAL_FILTERED_SH,
		FINAL_INDIRECT_DIFFUSE,
		RTXGI_RESULT,
		ALBEDO,
		VELOCITY,
		ROUGNESS_METALLIC,
		SPECULAR_RAW,
		TEMPORAL_FILTERED_SPECULAR,
		BLOOM,
		SPEC_HISTORY_LENGTH,		
		NO_FULLSCREEN,
	};

	EDebugVisualization FullscreenDebugBuffer = EDebugVisualization::FINAL_INDIRECT_DIFFUSE;// EDebugVisualization::NO_FULLSCREEN;
private:

	UINT32 RenderWidth;
	UINT32 RenderHeight;
	UINT32 DisplayWidth;
	UINT32 DisplayHeight;



	shared_ptr<GfxTexture> DepthBuffer;
	shared_ptr<GfxTexture> UnjitteredDepthBuffers[2];

	UINT ColorBufferWriteIndex = 0;
	shared_ptr<GfxTexture> ColorBuffers[2];
	shared_ptr<GfxTexture> LightingBuffer;
	shared_ptr<GfxTexture> LightingWithBloomBuffer;

	shared_ptr<GfxTexture> AlbedoBuffer;
	shared_ptr<GfxTexture> NormalBuffers[2];
	shared_ptr<GfxTexture> GeomNormalBuffer;
	shared_ptr<GfxTexture> VelocityBuffer;
	shared_ptr<GfxTexture> PixelVelocityBuffer;

	shared_ptr<GfxTexture> RoughnessMetalicBuffer;
	shared_ptr<GfxTexture> ShadowBuffer;

	shared_ptr<GfxTexture> SpeculaGIBufferRaw;

	shared_ptr<GfxTexture> SpeculaGIBufferTemporal[2];

	shared_ptr<GfxTexture> SpeculaGIMoments[2];

	// made seperated output buffers for NRD, i will refactor after NRD works.
	shared_ptr<GfxTexture> SpecularGI_NRD;
	shared_ptr<GfxTexture> DiffuseGI_NRD;
	shared_ptr<GfxTexture> NormalRoughness_NRD;
	shared_ptr<GfxTexture> LinearDepth_NRD;




	UINT GIBufferScale = 3;
	UINT GIBufferWriteIndex = 0;
	shared_ptr<GfxTexture> DiffuseGISHTemporal[2];
	shared_ptr<GfxTexture> DiffuseGICoCgTemporal[2];
	shared_ptr<GfxTexture> DiffuseGISHRaw;
	shared_ptr<GfxTexture> DiffuseGICoCgRaw;

	shared_ptr<GfxTexture> DiffuseGISHSpatial[2];
	shared_ptr<GfxTexture> DiffuseGICoCgSpatial[2];

	shared_ptr<GfxTexture> BloomBlurPingPong[2];
	shared_ptr<GfxTexture> LumaBuffer;
	std::shared_ptr<GfxBuffer> Histogram;
	std::shared_ptr<GfxBuffer> ExposureData;



	std::vector<std::shared_ptr<GfxTexture>> framebuffers;
	
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

	shared_ptr<GfxPipelineStateObject> GBufferPassPSO;

	// spatial denoising
	struct SpatialFilterConstant
	{
		glm::vec4 ProjectionParams;
		UINT32 Iteration;
		UINT32 GIBufferScale;
		float IndirectDiffuseWeightFactorDepth = 0.5f;
		float IndirectDiffuseWeightFactorNormal = 1.0f;
	};

	SpatialFilterConstant SpatialFilterCB;

	shared_ptr<GfxPipelineStateObject> SpatialDenoisingFilterPSO;



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
	};

	TemporalFilterConstant TemporalFilterCB;

	shared_ptr<GfxPipelineStateObject> TemporalDenoisingFilterPSO;
	
	// RT shadow
	struct RTShadowViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4	LightDir;
		glm::vec4	pad[2];
	};

	RTShadowViewParamCB RTShadowViewParam;
	
	shared_ptr<GfxRTPipelineStateObject> PSO_RT_SHADOW;


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
		UINT32 BlueNoiseOffsetStride = 1.0f;
		float ViewSpreadAngle;
	};

	RTReflectionViewParamCB RTReflectionViewParam;
	
	shared_ptr<GfxRTPipelineStateObject> PSO_RT_REFLECTION;

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
		UINT32 BlueNoiseOffsetStride = 1.0f;
		float ViewSpreadAngle;
		UINT32 bPackNRD;
	};

	RTGIViewParamCB RTGIViewParam;
	shared_ptr<GfxRTPipelineStateObject> PSO_RT_GI;
	

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
	shared_ptr<GfxPipelineStateObject> ToneMapPSO;

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
		RTXGI_LIGHTING = 7,
		COUNT = 8,
	};
	struct DebugPassCB
	{
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::vec4 Scale;
		glm::vec4 Offset;
		glm::vec4 ProjectionParams;
		glm::vec4 CameraPosition;
		glm::vec2 RTSize;
		float GIBufferScale;
		UINT32 DebugMode;
	};

	shared_ptr<GfxPipelineStateObject> BufferVisualizePSO;

	// lighting pass
	
	enum EDiffuseGIMode
	{
		PATH_TRACING,
#if USE_RTXGI
		RTXGI
#endif
	};
	EDiffuseGIMode DiffuseGIMethod = PATH_TRACING;

	struct LightingParam
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 InvProjMatrix;
		glm::vec4 LightDir;
		glm::vec4 CameraPosition;
		glm::vec2 RTSize;
		float GIBufferScale;
		UINT32 DiffuseGIMode;
		UINT32 UseNRD;
	};
	
	shared_ptr<GfxPipelineStateObject> LightingPSO;

	// temporalAA
	struct TemporalAAParam
	{
		glm::vec2 RTSize;
		float TAABlendFactor;
		UINT32 ClampMode;
		//float Exposure;
	};
	
	bool bEnableTAA = true;

	enum EAntialiasingMethod
	{
		TEMPORAL_AA,
		DLSS,
		NO_AA,
	};
#if USE_DLSS
	EAntialiasingMethod AAMethod = DLSS;
#else
	EAntialiasingMethod AAMethod = TEMPORAL_AA;
#endif
	EAntialiasingMethod PrevAAMethod = TEMPORAL_AA;

	UINT32 ClampMode = 2;

	float JitterScale = 1;

	shared_ptr<GfxPipelineStateObject> TemporalAAPSO;


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

	shared_ptr<GfxPipelineStateObject> BloomBlurPSO;

	shared_ptr<GfxPipelineStateObject> BloomExtractPSO;

	shared_ptr<GfxPipelineStateObject> HistogramPSO;

	shared_ptr<GfxPipelineStateObject> ClearHistogramPSO;

	bool bDrawHistogram = false;
	shared_ptr<GfxPipelineStateObject> DrawHistogramPSO;

	struct AdaptExposureCB
	{
		float TargetLuminance = 0.03;
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

	shared_ptr<GfxPipelineStateObject> AdapteExposurePSO;

	struct AddBloomCB
	{
		glm::vec4 Scale;
		glm::vec4 Offset;
		float BloomStrength;
	};
	AddBloomCB AddBloomCB;

	shared_ptr<GfxPipelineStateObject> AddBloomPSO;


	shared_ptr<GfxPipelineStateObject> ResolvePixelVelocityPSO;

	// imgui font texture
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleImguiFontTex;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleImguiFontTex;

	shared_ptr<GfxVertexBuffer> FullScreenVB;

	// blue noise texture
	shared_ptr<GfxTexture> BlueNoiseTex;
	shared_ptr<GfxTexture> DefaultWhiteTex;
	shared_ptr<GfxTexture> DefaultBlackTex;
	shared_ptr<GfxTexture> DefaultNormalTex;
	shared_ptr<GfxTexture> DefaultRougnessTex;

	// global wrap sampler
	std::shared_ptr<GfxSampler> samplerAnisoWrap;
	std::shared_ptr<GfxSampler> samplerBilinearWrap;
	std::shared_ptr<GfxSampler> samplerTrilinearClamp;

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
	float LightIntensity = 1;
	float Near = 10.0f;
	float Far = 20000.0f;
	float Fov = 0.8f;

	glm::mat4x4 ViewMat;
	glm::mat4x4 ProjMat;
	glm::mat4x4 InvViewMat;
	glm::mat4x4 InvProjMat;
	glm::mat4x4 ViewProjMat;
	glm::mat4x4 InvViewProjMat;
	
	glm::mat4x4 PrevViewMat;
	glm::mat4x4 PrevProjMat;

	glm::mat4x4 PrevInvViewMat;


	glm::mat4x4 PrevViewProjMat;


	glm::vec2 JitterOffset;
	glm::vec2 PrevJitter;

	glm::mat4x4 UnjitteredViewProjMat;
	glm::mat4x4 PrevUnjitteredViewProjMat;


	// raytracing resources

	struct InstanceProperty
	{
		glm::mat4x4 WorldMatrix;
	};

	std::shared_ptr<GfxBuffer> InstancePropertyBuffer;
	shared_ptr<GfxRTAS> TLAS;
	vector<shared_ptr<GfxRTAS>> vecBLAS;
	
	// ...
	bool bMultiThreadRendering = false;

	bool bDebugDraw = false;


	UINT m_frameCounter = 0;

	UINT FrameCounter = 0;

	// Pipeline objects.
	Rect m_scissorRect;
	

	enki::TaskScheduler g_TS;

	bool bRecompileShaders = false;
	bool bShowImgui = true;
	void RecompileShaders();

#if USE_DLSS
	bool m_ngxInitialized = false;
	bool m_bDlssAvailable = false;

	NVSDK_NGX_Parameter* m_ngxParameters = nullptr;
	NVSDK_NGX_Handle* m_dlssFeature = nullptr;

	bool bResetDLSS = false;

#endif

#if USE_RTXGI
	shared_ptr<rtxgi::DDGIVolume> volume;
	rtxgi::DDGIVolumeDesc volumeDesc = {};
	rtxgi::DDGIVolumeResources volumeResources = {};
	rtxgi::float3 volumeTranslation = { 0, 800, 0 };

	std::shared_ptr<Buffer> VolumeCB;

	shared_ptr<Texture> probeRTRadiance;
	shared_ptr<Texture> probeIrradiance;
	shared_ptr<Texture> probeDistance;
	shared_ptr<Texture> probeOffsets;
	shared_ptr<Texture> probeStates;

	D3D12_CPU_DESCRIPTOR_HANDLE volumeDescriptorTableCPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE volumeDescriptorTableGPUHandle;

	struct LightInfoCB
	{
		glm::vec4 LightDirAndIntensity;

	};
	shared_ptr<RTPipelineStateObject> PSO_RT_PROBE;

	bool bDrawIrradiance = false;
	float IrradianceScale = 1;
	bool bDrawDistance = false;
	float DistanceScale = 1;

	float probeHysteresis = 0.996f;
	float normalBias= 0.1;
	float viewBias = 0.1;
#endif

# if USE_NRD
	// NRD
	bool bNRDDenoising = false;
	Nrd m_NRD;

	NRIInterface m_NRI = {};
	nri::Device* m_NRIDevice = nullptr;
	//shared_ptr<nri::CommandQueue> m_NRICommandQueue = nullptr;
	
	//std::array<nri::CommandBuffer*, 3> m_NRIcommandBuffers;

	bool m_NRDInit = false;
	bool m_NRIInit = false;

	struct ResolveNRDParam
	{
		glm::mat4x4 InvProjMatrix;
		float Near;
		float Far;
	};
	shared_ptr<PipelineStateObject> ResolveNormalRoughnessPSO;
#endif

	struct SimpleDrawCB
	{
		glm::mat4x4 ViewProjectionMatrix;
		glm::mat4x4 WorldMatrix;
	};

	shared_ptr<GfxPipelineStateObject> SimpleDrawPSO;


	struct PostVertex
	{
		glm::vec4 position;
		glm::vec2 uv;
	};

	struct MeshVertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
		glm::vec3 Tangent;
	};
public:

	void InitRaytracingData();

	void InitDLSS();

	void InitNRD();

#if USE_RTXGI
	void InitRTXGI();
#endif

	void ShutDownDLSS();
	

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

	void InitTemporalAAPass();

	void InitBloomPass();

	void InitResolvePixelVelocityPass();

	void InitImgui();

	void InitBlueNoiseTexture();

	void InitSimpleDraw();

	void DrawScene(shared_ptr<Scene> scene, float Roughness, float Metalic, bool bOverrideRoughnessMetallic);

	void GBufferPass();

	void RaytraceShadowPass();

	void RaytraceReflectionPass();

	void RaytraceGIPass();

	void SpatialDenoisingPass();


	void TemporalDenoisingPass();

	void NRDPass();


	void BloomPass();



	void ToneMapPass();

	void DebugPass();

	void LightingPass();

	void TemporalAAPass();

#if USE_DLSS
	void DLSSPass();
#endif

	void ResolvePixelVelocityPass();

	void RTXGIPass();

	void SimpleDrawPass();

	// DXSample functions
	virtual void OnInit();

	virtual void OnUpdate();

	virtual void OnRender();

	virtual void OnDestroy();

	virtual void OnKeyDown(UINT8 key);

	virtual void OnKeyUp(UINT8 key);

	virtual void OnSizeChanged(UINT width, UINT height, bool minimized);


	Corona(UINT width, UINT height, UINT renderWidth, UINT renderHeight, std::wstring name);

	virtual ~Corona();

	
};
