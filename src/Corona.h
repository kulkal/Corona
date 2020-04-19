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
#include "SimpleDX12.h"
#include "enkiTS/TaskScheduler.h""
#define PROFILE_BUILD 1
#include "pix3.h"
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
		SH_LIGHTING,
		ALBEDO,
		VELOCITY,
		ROUGNESS_METALLIC,
		SPECULAR_RAW,
		TEMPORAL_FILTERED_SPECULAR,
		TEMPORAL_FILTERED_SPECULAR_2X2,
		TEMPORAL_FILTERED_SPECULAR_4X4,
		TEMPORAL_FILTERED_SPECULAR_8X8,
		TEMPORAL_FILTERED_SPECULAR_16X16,
		BLOOM,
		NO_FULLSCREEN,
	};

	EDebugVisualization FullscreenDebugBuffer = EDebugVisualization::NO_FULLSCREEN;
private:
	shared_ptr<Texture> DepthBuffer;

	UINT ColorBufferWriteIndex = 0;
	shared_ptr<Texture> ColorBuffer0;
	shared_ptr<Texture> ColorBuffer1;
	std::array<Texture*, 2> ColorBuffers;
	shared_ptr<Texture> LightingBuffer;
	shared_ptr<Texture> AlbedoBuffer;
	shared_ptr<Texture> NormalBuffer;
	shared_ptr<Texture> GeomNormalBuffer;
	shared_ptr<Texture> VelocityBuffer;
	shared_ptr<Texture> RoughnessMetalicBuffer;
	shared_ptr<Texture> ShadowBuffer;

	shared_ptr<Texture> SpeculaGIBufferRaw;

	shared_ptr<Texture> SpeculaGIBufferTemporal[2];
	shared_ptr<Texture> SpeculaGIBufferSpatial[2];

	shared_ptr<Texture> SpeculaGIBufferMip[4];





	UINT GIBufferScale = 3;
	UINT GIBufferWriteIndex = 0;
	shared_ptr<Texture> DiffuseGISHTemporal[2];
	shared_ptr<Texture> DiffuseGICoCgTemporal[2];
	shared_ptr<Texture> DiffuseGISHRaw;
	shared_ptr<Texture> DiffuseGICoCgRaw;

	shared_ptr<Texture> DiffuseGISHSpatial[2];
	shared_ptr<Texture> DiffuseGICoCgSpatial[2];

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
		float IndirectDiffuseWeightFactorDepth = 0.5f;
		float IndirectDiffuseWeightFactorNormal = 1.0f;
	};

	SpatialFilterConstant SpatialFilterCB;

	shared_ptr<PipelineStateObject> SpatialDenoisingFilterPSO;

	// gen specular gi mip
	struct GenMipCB
	{
		glm::vec2 TexelSize;
		UINT32 NumMipLevels;
	};

	GenMipCB GenMipCB;
	shared_ptr<PipelineStateObject> GenMipPSO;


	// temporal denoising
	struct TemporalFilterConstant
	{
		glm::vec4 ProjectionParams;
		glm::vec4 TemporalValidParams = glm::vec4(15, 0, 0, 0);
		glm::vec2 RTSize;
	};

	TemporalFilterConstant TemporalFilterCB;

	shared_ptr<PipelineStateObject> TemporalDenoisingFilterPSO;
	
	// RT shadow
	struct RTShadowViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4	LightDir;
		glm::vec4	pad[2];
	};

	RTShadowViewParamCB RTShadowViewParam;
	
	shared_ptr<RTPipelineStateObject> PSO_RT_SHADOW;


	// RT reflection
	struct RTReflectionViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4	LightDir;
		glm::vec2 RandomOffset;
		UINT32 FrameCounter;
		UINT32 BlueNoiseOffsetStride = 1.0f;
		float ViewSpreadAngle;
	};

	RTReflectionViewParamCB RTReflectionViewParam;
	
	shared_ptr<RTPipelineStateObject> PSO_RT_REFLECTION;

	// RT GI
	struct RTGIViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4 LightDir;
		glm::vec2 RandomOffset;
		UINT32 FrameCounter;
		UINT32 BlueNoiseOffsetStride = 1.0f;
		float ViewSpreadAngle;
	};

	RTGIViewParamCB RTGIViewParam;
	shared_ptr<RTPipelineStateObject> PSO_RT_GI;
	

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


	UINT32 ToneMapMode = FILMIC_ALU;
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
	};
	
	shared_ptr<PipelineStateObject> LightingPSO;

	// temporalAA
	struct TemporalAAParam
	{
		glm::vec2 RTSize;
		float TAABlendFactor;
		UINT32 ClampMode;
		//float Exposure;
		float BloomStrength;
	};
	
	bool bEnableTAA = true;

	UINT32 ClampMode = 2;

	float JitterScale = 0.85;

	shared_ptr<PipelineStateObject> TemporalAAPSO;


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
	float LightIntensity = 0.169;
	float Near = 1.0f;
	float Far = 40000.0f;
	float Fov = 0.8f;

	glm::mat4x4 ViewMat;
	glm::mat4x4 ProjMat;
	glm::mat4x4 ViewProjMat;
	glm::mat4x4 InvViewProjMat;
	glm::mat4x4 PrevViewMat;
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
	void RecompileShaders();
public:

	void InitRaytracingData();
	

	void LoadPipeline();

	void LoadAssets();

	shared_ptr<Scene> LoadModel(string fileName);

	void InitRTPSO();

	void InitSpatialDenoisingPass();

	void InitTemporalDenoisingPass();

	void InitDrawMeshRS();

	void InitCopyPass();

	void InitDebugPass();

	void InitLightingPass();

	void InitTemporalAAPass();

	void InitBloomPass();

	void InitGenMipSpecularGIPass();

	void InitImgui();

	void InitBlueNoiseTexture();

	void DrawScene(shared_ptr<Scene> scene, float Roughness, float Metalic, bool bOverrideRoughnessMetallic);

	void GBufferPass();

	void RaytraceShadowPass();

	void RaytraceReflectionPass();

	void RaytraceGIPass();

	void SpatialDenoisingPass();


	void TemporalDenoisingPass();

	void BloomPass();



	void CopyPass();

	void DebugPass();

	void LightingPass();

	void TemporalAAPass();

	void GenMipSpecularGIPass();

	// DXSample functions
	virtual void OnInit();

	virtual void OnUpdate();

	virtual void OnRender();

	virtual void OnDestroy();

	virtual void OnKeyDown(UINT8 key);

	virtual void OnKeyUp(UINT8 key);

	Corona(UINT width, UINT height, std::wstring name);

	virtual ~Corona();

	
};
