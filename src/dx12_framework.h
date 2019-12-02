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
	struct ObjConstantBuffer
	{
		glm::mat4x4 ViewProjectionMatrix;
		glm::mat4x4 WorldMatrix;
		glm::vec4 ViewDir;
	};

	struct RTShadowViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4	LightDir;
	};

	struct RTReflectionViewParamCB
	{
		glm::mat4x4 ViewMatrix;
		glm::mat4x4 InvViewMatrix;
		glm::mat4x4 ProjMatrix;
		glm::vec4 ProjectionParams;
		glm::vec4	LightDir;
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
	
	// Pipeline objects.
	CD3DX12_VIEWPORT m_viewport;
	CD3DX12_RECT m_scissorRect;
	ComPtr<IDXGISwapChain3> m_swapChain;
	ComPtr<ID3D12Device5> m_device;
	
	shared_ptr<Texture> ColorBuffer;
	shared_ptr<Texture> AlbedoBuffer;
	shared_ptr<Texture> NormalBuffer;
	shared_ptr<Texture> GeomNormalBuffer;
	shared_ptr<Texture> ShadowBuffer;
	shared_ptr<Texture> ReflectionBuffer;


	shared_ptr<Texture> DepthBuffer;

	shared_ptr<Texture> FakeDepthBuffer;


	std::vector<std::shared_ptr<Texture>> framebuffers;


	unique_ptr<PipelineStateObject> RS_Mesh;


	// test compute pass
	unique_ptr<PipelineStateObject> RS_Compute;
	shared_ptr<Texture> ComputeOuputTexture;

	// global wrap sampler
	std::shared_ptr<Sampler> samplerWrap;

	// dx sample mesh
	shared_ptr<Mesh> mesh;

	// fbx mesh
	vector<shared_ptr<Mesh>> meshes;
	vector<shared_ptr<Material>> Materials;

	StepTimer m_timer;
	SimpleCamera m_camera;


	glm::vec3 LightDir = glm::normalize(glm::vec3(0.5, 1, 0.2));
	float Near = 1.0f;
	float Far = 10000.0f;


	// raytracing
	shared_ptr<RTAS> TLAS;
	vector<shared_ptr<RTAS>> vecBLAS;
	unique_ptr<RTPipelineStateObject> PSO_RT_SHADOW;
	RTShadowViewParamCB RTShadowViewParam;

	unique_ptr<RTPipelineStateObject> PSO_RT_REFLECTION;
	RTReflectionViewParamCB RTReflectionViewParam;



	// full screen copy pass
	unique_ptr<PipelineStateObject> RS_Copy;
	shared_ptr<VertexBuffer> FullScreenVB;

	// lighting pass
	unique_ptr<PipelineStateObject> RS_Lighting;
	struct LightingParam
	{
		glm::vec4 LightDir;
	};


public:

	void InitRaytracing();
	
	void LoadPipeline();
	void LoadAssets();

	void LoadMesh();

	void LoadFbx();

	void InitComputeRS();

	void InitDrawMeshRS();

	void InitDrawMeshPass();
	void DrawMeshPass();

	void RaytraceShadowPass();

	void RaytraceReflectionPass();

	void RecordDraw(UINT StartIndex, UINT NumDraw, UINT CLIndex, ThreadDescriptorHeapPool* DHPool);


	void ComputePass();

	void InitCopyPass();

	void InitLightingPass();

	void CopyPass();

	void DebugPass();

	void LightingPass();

};
