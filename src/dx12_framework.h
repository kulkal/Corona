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

#include "glm/glm.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/transform.hpp"
#include "glm/mat4x4.hpp"
#include "glm/fwd.hpp"

#include "DXSample.h"
#include "StepTimer.h"
#include "SimpleCamera.h"
#include "DumRHI_DX12.h"
#include <array>
using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().
using Microsoft::WRL::ComPtr;
using namespace std;


class dx12_framework : public DXSample
{
	struct InstanceProperty
	{
		glm::mat4x4 WorldMatrix;
	};
	struct ObjConstantBuffer
	{
		glm::mat4x4 ViewProjectionMatrix;
		//glm::mat4x4 UnjitteredViewProjectionMatrix;
		//glm::mat4x4 InvViewProjectionMatrix;
		glm::mat4x4 PrevViewProjectionMatrix;
		glm::mat4x4 WorldMatrix;
		glm::vec4 ViewDir;
		glm::vec2 RTSize;
		glm::vec2 JitterOffset;
		glm::vec4 pad[2];
	};

	struct RTShadowViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4	LightDir;
		glm::vec4	pad[2];
	};

	struct RTReflectionViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4	LightDir;
	};

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
	};

	struct CopyScaleOffsetCB
	{
		glm::vec4 Scale;
		glm::vec4 Offset;
	};

public:
	dx12_framework(UINT width, UINT height, std::wstring name);
	virtual ~dx12_framework();

	virtual void OnInit();
	virtual void OnUpdate();
	virtual void OnRender();
	virtual void OnDestroy();
	virtual void OnKeyDown(UINT8 key);
	virtual void OnKeyUp(UINT8 key);

private:
	std::unique_ptr<DumRHI_DX12> dx12_rhi;

	bool bMultiThreadRendering = false;
	bool bDebugDraw = false;
	
	UINT m_frameCounter = 0;

	UINT FrameCounter = 0;
	
	// Pipeline objects.
	CD3DX12_VIEWPORT m_viewport;
	CD3DX12_RECT m_scissorRect;
	ComPtr<IDXGISwapChain3> m_swapChain;
	ComPtr<ID3D12Device5> m_device;
	
	UINT ColorBufferWriteIndex = 0;
	shared_ptr<Texture> ColorBuffer0;
	shared_ptr<Texture> ColorBuffer1;
	
	std::array<Texture*, 2> ColorBuffers;

	shared_ptr<Texture> LightingBuffer;

	shared_ptr<Texture> AlbedoBuffer;
	shared_ptr<Texture> NormalBuffer;
	shared_ptr<Texture> GeomNormalBuffer;
	shared_ptr<Texture> VelocityBuffer;


	shared_ptr<Texture> ShadowBuffer;
	shared_ptr<Texture> ReflectionBuffer;
	//shared_ptr<Texture> GIBuffer;

	shared_ptr<Texture> GIBufferSH;
	shared_ptr<Texture> GIBufferColor;



	shared_ptr<Texture> DepthBuffer;

	shared_ptr<Texture> FakeDepthBuffer;


	std::vector<std::shared_ptr<Texture>> framebuffers;


	unique_ptr<PipelineStateObject> RS_Mesh;

	// denoising
	struct FilterIndirectDiffuseConstant
	{
		glm::vec4 ProjectionParams;
		UINT32 Iteration;
		float IndirectDiffuseWeightFactorDepth = 0.20f;
		float IndirectDiffuseWeightFactorNormal = 9.0f;
	};

	//FLOAT IndirectDiffuseWeightFactor0;
	FilterIndirectDiffuseConstant FilterIndirectDiffuseConstantCB;

	unique_ptr<PipelineStateObject> RS_FilterIndirectDiffuse;
	shared_ptr<Texture> FilterIndirectDiffusePingPong[2];

	shared_ptr<Texture> FilterIndirectDiffusePingPongSH[2];
	shared_ptr<Texture> FilterIndirectDiffusePingPongColor[2];



	// test compute pass
	unique_ptr<PipelineStateObject> RS_Compute;
	shared_ptr<Texture> ComputeOuputTexture;

	// global wrap sampler
	std::shared_ptr<Sampler> samplerWrap;

	// dx sample mesh
	shared_ptr<Mesh> mesh;

	

	shared_ptr<Scene> Sponza;

	shared_ptr<Scene> Buddha;

	shared_ptr<Scene> ShaderBall;


	// fbx mesh
	//vector<shared_ptr<Mesh>> meshes;
	//vector<shared_ptr<Material>> Materials;

	StepTimer m_timer;
	SimpleCamera m_camera;


	glm::vec3 LightDir = glm::normalize(glm::vec3(0.901, 0.88, 0.176));
	float LightIntensity = 3.0f;
	float Near = 1.0f;
	float Far = 20000.0f;

	glm::mat4x4 ViewMat;
	glm::mat4x4 ProjMat;
	glm::mat4x4 ViewProjMat;
	glm::mat4x4 UnjitteredViewProjMat;

	glm::mat4x4 InvViewProjMat;


	glm::mat4x4 PrevViewMat;
	glm::mat4x4 PrevViewProjMat;
	glm::vec2 JitterOffset;
	glm::vec2 PrevJitter;



	// raytracing
	std::shared_ptr<Buffer> InstancePropertyBuffer;
	shared_ptr<RTAS> TLAS;
	vector<shared_ptr<RTAS>> vecBLAS;
	unique_ptr<RTPipelineStateObject> PSO_RT_SHADOW;
	RTShadowViewParamCB RTShadowViewParam;

	unique_ptr<RTPipelineStateObject> PSO_RT_REFLECTION;
	RTReflectionViewParamCB RTReflectionViewParam;


	unique_ptr<RTPipelineStateObject> PSO_RT_GI;
	RTGIViewParamCB RTGIViewParam;



	// full screen copy pass
	unique_ptr<PipelineStateObject> RS_Copy;
	shared_ptr<VertexBuffer> FullScreenVB;

	// lighting pass
	unique_ptr<PipelineStateObject> RS_Lighting;

	// temporalAA
	unique_ptr<PipelineStateObject> RS_TemporalAA;


	struct LightingParam
	{
		glm::vec4 LightDir;
		glm::vec2 RTSize;
		float TAABlendFactor;
		float pad0;
	};

	struct TemporalAAParam
	{
		glm::vec2 RTSize;
		float TAABlendFactor;
		UINT32 ClampMode;
	};
	bool bEnableTAA = true;

	UINT32 ClampMode = 2;
	float JitterScale = 0.85;

	// imgui font texture
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleImguiFontTex;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleImguiFontTex;

	// blue noise texture
	shared_ptr<Texture> BlueNoiseTex;
	shared_ptr<Texture> DefaultWhiteTex;
	shared_ptr<Texture> DefaultNormalTex;

public:

	void InitRaytracing();

	void InitRaytraceShadow();
	
	void LoadPipeline();
	void LoadAssets();

	void LoadMesh();

	shared_ptr<Scene> LoadModel(string fileName);

	void InitComputeRS();

	void InitDenoiserPass();

	void InitDrawMeshRS();

	void InitImgui();

	void InitBlueNoiseTexture();

	void DrawScene(shared_ptr<Scene> scene);

	void DrawMeshPass();

	void RaytraceShadowPass();

	void RaytraceReflectionPass();

	void RaytraceGIPass();


	void RecordDraw(UINT StartIndex, UINT NumDraw, UINT CLIndex, ThreadDescriptorHeapPool* DHPool);


	void FilterIndirectDiffusePass();

	void InitCopyPass();

	void InitLightingPass();

	void InitTemporalAAPass();


	void CopyPass();

	void DebugPass();

	void LightingPass();

	void TemporalAAPass();

};
