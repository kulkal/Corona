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
	struct SceneConstantBuffer
	{
		XMFLOAT4X4 mvp;		// Model-view-projection (MVP) matrix.
		FLOAT padding[48];
	};

	struct ObjConstantBuffer
	{
		XMFLOAT4X4 WorldMatrix;		// Model-view-projection (MVP) matrix.
		FLOAT padding[48];
	};

	struct RTViewParamCB
	{
		XMFLOAT4X4 ViewMatrix;		// Model-view-projection (MVP) matrix.
		XMFLOAT4X4 InvViewMatrix;		// Model-view-projection (MVP) matrix.
		//float padding[];
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
	bool bMultiThreadRendering = false;
	//static const UINT FrameCount = 3
	
	UINT m_frameCounter;
	;
	
	static const bool UseBundles = false;

	// Pipeline objects.
	CD3DX12_VIEWPORT m_viewport;
	CD3DX12_RECT m_scissorRect;
	ComPtr<IDXGISwapChain3> m_swapChain;
	ComPtr<ID3D12Device5> m_device;
	
	shared_ptr<Texture> ColorBuffer;
	shared_ptr<Texture> AlbedoBuffer;
	shared_ptr<Texture> NormalBuffer;

	shared_ptr<Texture> ShadowBuffer;






	unique_ptr<PipelineStateObject> RS;

	unique_ptr<PipelineStateObject> RS_Mesh;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDescMesh;
	shared_ptr<Shader> vsMesh;
	shared_ptr<Shader> psMesh;



	unique_ptr<PipelineStateObject> RS_Compute;

	shared_ptr<Texture> ComputeOuputTexture;



	//unique_ptr<Texture> SceneColorBuffer;



	std::shared_ptr<Sampler> samplerWrap;


	unique_ptr<Mesh> SquintRoom;

	StepTimer m_timer;

	SimpleCamera m_camera;
	std::vector<std::shared_ptr<Texture>> framebuffers;

	// Frame resources.

	unique_ptr<GlobalConstantBuffer> ViewParameter;

	XMFLOAT4X4 m_modelMatrices;

	glm::vec3 LightDir = glm::vec3(1, 1, -1);


	std::unique_ptr<DumRHI_DX12> dx12_rhi;									


	XMFLOAT4X4 mvp;


	// raytracing as
	shared_ptr<RTAS> BLAS;

	shared_ptr<RTAS> TLAS;
	


	ComPtr<ID3D12Resource> mpShaderTable;
	uint32_t mShaderTableEntrySize = 0;

	D3D12_GPU_DESCRIPTOR_HANDLE RTASGPUHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE RTASCPUHandle;

	unique_ptr<GlobalConstantBuffer> ViewMatrixCB;

	unique_ptr<RTPipelineStateObject> PSO_RT;
	RTViewParamCB RTViewParam;


	// full screen copy pass
	unique_ptr<PipelineStateObject> RS_Copy;
	shared_ptr<VertexBuffer> FullScreenVB;

	// lighting pass
	unique_ptr<PipelineStateObject> RS_Lighting;
	struct LightingParam
	{
		glm::vec4 LightDir;
		FLOAT padding[60];

	};


public:

	void InitRaytracing();
	
	void LoadPipeline();
	void LoadAssets();

	void LoadMesh();

	void InitComputeRS();

	void InitDrawMeshRS();

	void InitDrawMeshPass();
	void DrawMeshPass();

	void RaytracePass();

	void RecordDraw(UINT StartIndex, UINT NumDraw, UINT CLIndex, ThreadDescriptorHeapPool* DHPool);


	void ComputePass();

	void InitCopyPass();
	void InitLightingPass();

	void CopyPass();

	void LightingPass();

};
