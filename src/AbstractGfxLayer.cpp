#include "AbstractGfxLayer.h"
#include "DX12Impl.h"
#include "VulkanImpl.h"
#include <d3dx12.h>
#include <dxcapi.h>


extern DX12Impl* g_dx12_rhi;
extern VulkanImpl* g_vulkanImpl;

DX12Impl* dx12_ptr;
VulkanImpl* vulkan_ptr;


std::map<FORMAT, VkFormat> formatMap =
{
	{FORMAT_R32_TYPELESS, VK_FORMAT_D32_SFLOAT},
	{FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT},
	{FORMAT_D32_FLOAT, VK_FORMAT_D32_SFLOAT},
	{FORMAT_R8_UINT, VK_FORMAT_R8_UINT},
	{FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
	{FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT},
	{FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
	{FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
	{FORMAT_R32G32_FLOAT, VK_FORMAT_R32G32_SFLOAT},
	{FORMAT_R32G32B32_FLOAT, VK_FORMAT_R32G32B32_SFLOAT},
};

VkFormat getVkFormat(FORMAT f)
{
	VkFormat format;
	std::map<FORMAT, VkFormat>::iterator it;
	if ((it=formatMap.find(f)) != formatMap.end())
		format = it->second;
	else
	{
		assert(false);
	}

	return format;
}

std::map< PRIMITIVE_TOPOLOGY, VkPrimitiveTopology> topologyMap =
{
	{PRIMITIVE_TOPOLOGY_POINTLIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST},
	{PRIMITIVE_TOPOLOGY_LINELIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST},
	{PRIMITIVE_TOPOLOGY_LINESTRIP, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP},
	{PRIMITIVE_TOPOLOGY_TRIANGLELIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
	{PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP},
	{PRIMITIVE_TOPOLOGY_LINELIST_ADJ, VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY},
	{PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY},
	{PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY},
	{PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY}
	// VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
	// VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
};

VkPrimitiveTopology getVKTopology(PRIMITIVE_TOPOLOGY t)
{
	VkPrimitiveTopology topology;
	std::map<PRIMITIVE_TOPOLOGY, VkPrimitiveTopology>::iterator it;
	if ((it = topologyMap.find(t)) != topologyMap.end())
		topology = it->second;
	else
	{
		assert(false);
	}

	return topology;
}

GfxSampler* AbstractGfxLayer::CreateSampler(SAMPLER_DESC& InSamplerDesc)
{
	if (g_dx12_rhi)
	{
		D3D12_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = static_cast<D3D12_FILTER>(InSamplerDesc.Filter);
		samplerDesc.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(InSamplerDesc.AddressU);
		samplerDesc.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(InSamplerDesc.AddressV);
		samplerDesc.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(InSamplerDesc.AddressW);
		samplerDesc.MinLOD = InSamplerDesc.MinLOD;
		samplerDesc.MaxLOD = InSamplerDesc.MaxLOD;
		samplerDesc.MipLODBias = -1.0f;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(D3D12_COMPARISON_FUNC_ALWAYS);

		Sampler* sampler = g_dx12_rhi->CreateSampler(samplerDesc);
		return sampler;
	}
	else
	{
		return nullptr;
	}
}

void AbstractGfxLayer::SetSampler(std::string bindName, GfxCommandList* cl, GfxPipelineStateObject* PSO, GfxSampler* sampler)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		Sampler* dx12Sampler = static_cast<Sampler*>(sampler);
		CommandList* dx12CL = static_cast<CommandList*>(cl);
		dx12PSO->SetSampler(bindName, dx12Sampler, dx12CL->CmdList.Get());
	}
}

GfxTexture* AbstractGfxLayer::CreateTextureFromFile(std::wstring fileName, bool nonSRGB)
{
	if (g_dx12_rhi)
	{
		Texture* texture = g_dx12_rhi->CreateTextureFromFile(fileName, nonSRGB);

		return texture;
	}

	return nullptr;
}

GfxTexture* AbstractGfxLayer::CreateTexture2D(FORMAT format, RESOURCE_FLAGS resFlags, RESOURCE_STATES initResState, int width, int height, int mipLevels, std::optional<glm::vec4> clearColor)
{
	if (g_dx12_rhi)
	{
		Texture* texture = g_dx12_rhi->CreateTexture2D(static_cast<DXGI_FORMAT>(format), static_cast<D3D12_RESOURCE_FLAGS>(resFlags), static_cast<D3D12_RESOURCE_STATES>(initResState), width, height, mipLevels, clearColor);
		return texture;
	}
	else if (g_vulkanImpl)
	{
		VKTexture* vkTexture = new VKTexture;

		TextureCreateInfo info;
		
		info.format = getVkFormat(format);
		info.width = width;
		info.height = height;
		info.depth = 1;
		info.mipLevels = mipLevels;
		info.arrayLayers = 1;

		vkTexture->Create(&info);

		return vkTexture;
	}

	return nullptr;
}

GfxTexture* AbstractGfxLayer::CreateTexture3D(FORMAT format, RESOURCE_FLAGS resFlags, RESOURCE_STATES initResState, int width, int height, int depth, int mipLevels)
{
	if (g_dx12_rhi)
	{
		Texture* texture = g_dx12_rhi->CreateTexture3D(static_cast<DXGI_FORMAT>(format), static_cast<D3D12_RESOURCE_FLAGS>(resFlags), static_cast<D3D12_RESOURCE_STATES>(initResState), width, height, depth, mipLevels);
		return texture;
	}

	return nullptr;
}

GfxVertexBuffer* AbstractGfxLayer::CreateVertexBuffer(UINT Size, UINT Stride, void* SrcData)
{
	if (g_dx12_rhi)
	{
		VertexBuffer* VB = g_dx12_rhi->CreateVertexBuffer(Size, Stride, SrcData);
		return VB;
	}

	return nullptr;
}

GfxIndexBuffer* AbstractGfxLayer::CreateIndexBuffer(FORMAT format, UINT Size, void* SrcData)
{
	if (g_dx12_rhi)
	{
		DXGI_FORMAT dx12Format = static_cast<DXGI_FORMAT>(format);
		IndexBuffer* dx12IB = g_dx12_rhi->CreateIndexBuffer(dx12Format, Size, SrcData);

		return dx12IB;
	}

	return nullptr;
}

GfxBuffer* AbstractGfxLayer::CreateByteAddressBuffer(UINT InNumElements, UINT InElementSize, HEAP_TYPE InType, RESOURCE_STATES initResState, RESOURCE_FLAGS InFlags, void* SrcData)
{
	if (g_dx12_rhi)
	{
		D3D12_HEAP_TYPE dx12HeapType = static_cast<D3D12_HEAP_TYPE>(InType);
		D3D12_RESOURCE_STATES dx12InitResState = static_cast<D3D12_RESOURCE_STATES>(initResState);
		D3D12_RESOURCE_FLAGS dx12Flags = static_cast<D3D12_RESOURCE_FLAGS>(InFlags);

		Buffer* dx12B = g_dx12_rhi->CreateBuffer(InNumElements, InElementSize, dx12HeapType, dx12InitResState, dx12Flags, SrcData);
		dx12B->MakeByteAddressBufferSRV();

		return dx12B;
	}

	return nullptr;
}

GfxRTAS* AbstractGfxLayer::CreateBLAS(GfxMesh* mesh)
{
	if (g_dx12_rhi)
	{
		RTAS* as = g_dx12_rhi->CreateBLAS(mesh);
		return as;
	}

	return nullptr;
}

void AbstractGfxLayer::MapBuffer(GfxBuffer* buffer, void ** pData)
{
	if (g_dx12_rhi)
	{
		Buffer* dx12Buffer = static_cast<Buffer*>(buffer);
		dx12Buffer->resource->Map(0, nullptr, pData);
	}
}

void AbstractGfxLayer::UnmapBuffer(GfxBuffer* buffer)
{
	if (g_dx12_rhi)
	{
		Buffer* dx12Buffer = static_cast<Buffer*>(buffer);
		dx12Buffer->resource->Unmap(0, nullptr);
	}
}


GfxRTAS* AbstractGfxLayer::CreateTLAS(std::vector<std::shared_ptr<GfxRTAS>>& VecBLAS)
{
	if (g_dx12_rhi)
	{
		std::vector <RTAS*> vecBLAS;
		for (auto& blas : VecBLAS)
		{
			RTAS* dx12BLAS = static_cast<RTAS*>(blas.get());
			vecBLAS.push_back(dx12BLAS);
		}
		RTAS* as = g_dx12_rhi->CreateTLAS(vecBLAS);
		return as;
	}
}


void AbstractGfxLayer::SetReadTexture(GfxPipelineStateObject* PSO, std::string name, GfxTexture* texture, GfxCommandList* CL)
{
	if (!texture) return;

	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		Texture* dx12Texture= static_cast<Texture*>(texture);
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		dx12PSO->SetSRV(name, dx12Texture->SRV.GpuHandle, dx12CL->CmdList.Get());
	}
}

void AbstractGfxLayer::SetWriteTexture(GfxPipelineStateObject* PSO, std::string name, GfxTexture* texture, GfxCommandList* CL)
{
	if (!texture) return;

	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		Texture* dx12Texture = static_cast<Texture*>(texture);
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		dx12PSO->SetUAV(name, dx12Texture->SRV.GpuHandle, dx12CL->CmdList.Get());
	}
}

void AbstractGfxLayer::SetReadBuffer(GfxPipelineStateObject* PSO, std::string name, GfxBuffer* buffer, GfxCommandList* CL)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		Buffer* dx12Buffer = static_cast<Buffer*>(buffer);
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		dx12PSO->SetSRV(name, dx12Buffer->SRV.GpuHandle, dx12CL->CmdList.Get());
	}
}

void AbstractGfxLayer::SetWriteBuffer(GfxPipelineStateObject* PSO, std::string name, GfxBuffer* buffer, GfxCommandList* CL)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		Buffer* dx12Buffer = static_cast<Buffer*>(buffer);
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		dx12PSO->SetUAV(name, dx12Buffer->SRV.GpuHandle, dx12CL->CmdList.Get());
	}
}

void AbstractGfxLayer::SetUniformValue(GfxPipelineStateObject* PSO, std::string name, void* pData, GfxCommandList* CL)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		dx12PSO->SetCBVValue(name, pData, dx12CL->CmdList.Get());
	}
}

void AbstractGfxLayer::SetUniformBuffer(GfxPipelineStateObject* PSO, std::string name, GfxBuffer* buffer, int offset, GfxCommandList* CL)
{
	if (!buffer) return;

	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		Buffer* dx12Buffer = static_cast<Buffer*>(buffer);
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		dx12PSO->SetCBVValue(name, dx12Buffer->resource->GetGPUVirtualAddress() + offset, dx12CL->CmdList.Get());
	}
}

void AbstractGfxLayer::SetPSO(GfxPipelineStateObject* PSO, GfxCommandList* CL)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		dx12PSO->Apply(dx12CL->CmdList.Get());
	}

}

void AbstractGfxLayer::DrawInstanced(GfxCommandList* CL, int VertexCountPerInstance, int InstanceCount, int StartVertexLocation, int StartInstanceLocation)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		dx12CL->CmdList->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
	}
}

void AbstractGfxLayer::DrawIndexedInstanced(GfxCommandList* CL, int IndexCountPerInstance, int InstanceCount, int StartIndexLocation, int BaseVertexLocation, int StartInstanceLocation)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		dx12CL->CmdList->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
	}

}

void AbstractGfxLayer::SetVertexBuffer(GfxCommandList* CL, int StartSlot, int NumViews, GfxVertexBuffer* buffer)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		VertexBuffer* dx12Buffer = static_cast<VertexBuffer*>(buffer);

		dx12CL->CmdList->IASetVertexBuffers(0, 1, &dx12Buffer->view);
	}
}

void AbstractGfxLayer::SetIndexBuffer(GfxCommandList* CL, GfxIndexBuffer* buffer)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		IndexBuffer* dx12Buffer = static_cast<IndexBuffer*>(buffer);

		dx12CL->CmdList->IASetIndexBuffer(&dx12Buffer->view);
	}
}

void AbstractGfxLayer::SetPrimitiveTopology(GfxCommandList* CL, PRIMITIVE_TOPOLOGY PrimitiveTopology)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);

		dx12CL->CmdList->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(PrimitiveTopology));
	}
}

void AbstractGfxLayer::SetScissorRects(GfxCommandList* CL, int NumRects, Rect* rects)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);

		std::vector< D3D12_RECT> rectVec;
		for (int i = 0; i < NumRects; i++)
		{
			D3D12_RECT dx12Rect = { rects[i].left, rects[i].top, rects[i].right, rects[i].bottom };
			rectVec.push_back(dx12Rect);
		}

		dx12CL->CmdList->RSSetScissorRects(1, rectVec.data());
	}
}

void AbstractGfxLayer::SetViewports(GfxCommandList* CL, int NumViewPorts, ViewPort* viewPort)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);

		std::vector< D3D12_VIEWPORT> vewPortVec;
		for (int i = 0; i < NumViewPorts; i++)
		{
			D3D12_VIEWPORT dx12ViewPort = { viewPort[i].TopLeftX, viewPort[i].TopLeftY, viewPort[i].Width, viewPort[i].Height, viewPort[i].MinDepth, viewPort[i].MaxDepth };
			vewPortVec.push_back(dx12ViewPort);
		}

		dx12CL->CmdList->RSSetViewports(1, vewPortVec.data());
	}
}

void AbstractGfxLayer::ClearRenderTarget(GfxCommandList* CL, GfxTexture* texture, float ColorRGBA[4], int NumRects, Rect* rects)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		Texture* dx12Texture = static_cast<Texture*>(texture);

		std::vector< D3D12_RECT> rectVec;
		for (int i = 0; i < NumRects; i++)
		{
			D3D12_RECT dx12Rect = { rects[i].left, rects[i].top, rects[i].right, rects[i].bottom };
			rectVec.push_back(dx12Rect);
		}

		dx12CL->CmdList->ClearRenderTargetView(dx12Texture->RTV.CpuHandle, ColorRGBA, NumRects, rectVec.data());
	}
}

void AbstractGfxLayer::ClearDepthStencil(GfxCommandList* CL, GfxTexture* texture, CLEAR_FLAGS ClearFlags, float Depth, unsigned char Stencil, int NumRects, Rect* rects)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		Texture* dx12Texture = static_cast<Texture*>(texture);

		std::vector< D3D12_RECT> rectVec;
		for (int i = 0; i < NumRects; i++)
		{
			D3D12_RECT dx12Rect = { rects[i].left, rects[i].top, rects[i].right, rects[i].bottom };
			rectVec.push_back(dx12Rect);
		}

		dx12CL->CmdList->ClearDepthStencilView(dx12Texture->DSV.CpuHandle, static_cast<D3D12_CLEAR_FLAGS>(ClearFlags), Depth, Stencil, NumRects, rectVec.data());
	}
}

void AbstractGfxLayer::SetRenderTargets(GfxCommandList* CL, GfxPipelineStateObject* PSO, int NumRendertargets, GfxTexture** Rendertargets, GfxTexture* DepthTexture)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		Texture* dx12DepthTexture = static_cast<Texture*>(DepthTexture);

		std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dx12RenderTargets;
		for (int i = 0; i < NumRendertargets; i++)
		{
			Texture* dx12Texture = static_cast<Texture*>(Rendertargets[i]);
			dx12RenderTargets.push_back(dx12Texture->RTV.CpuHandle);
		}
		if (DepthTexture)
			dx12CL->CmdList->OMSetRenderTargets(NumRendertargets, dx12RenderTargets.data(), FALSE, &dx12DepthTexture->DSV.CpuHandle);
		else
			dx12CL->CmdList->OMSetRenderTargets(NumRendertargets, dx12RenderTargets.data(), FALSE, nullptr);
	}
	else if (g_vulkanImpl)
	{
		VKPipelineStateObject* vkPSO = static_cast<VKPipelineStateObject*>(PSO);
		VKTexture* vkDepthTexture = static_cast<VKTexture*>(DepthTexture);
		std::vector<VKTexture*> vkRenderTargets;

		for (int i = 0; i < NumRendertargets; i++)
		{
			VKTexture* vkTexture = static_cast<VKTexture*>(Rendertargets[i]);
			vkRenderTargets.push_back(vkTexture);
		}

		vkPSO->SetRendertargets(vkRenderTargets, vkDepthTexture);
	}
}

void AbstractGfxLayer::Dispatch(GfxCommandList* CL, int ThreadGroupCountX, int ThreadGroupCountY, int ThreadGroupCountZ)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		dx12CL->CmdList->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
	}
}

GfxPipelineStateObject* AbstractGfxLayer::CreatePSO()
{
	if (g_dx12_rhi)
	{
		PipelineStateObject*  dx12PSO = new PipelineStateObject;

		return dx12PSO;
	}
	else if (g_vulkanImpl)
	{
		VKPipelineStateObject* vkPSO = new VKPipelineStateObject;

		return vkPSO;
	}

	return nullptr;
}

GfxRTPipelineStateObject* AbstractGfxLayer::CreateRTPSO()
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = new RTPipelineStateObject;

		return dx12PSO;
	}
	
	return nullptr;
}


bool AbstractGfxLayer::InitPSO(GfxPipelineStateObject* PSO, GRAPHICS_PIPELINE_STATE_DESC* psoDesc)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);

		{
			ComPtr<ID3DBlob> vs;
			vector< DxcDefine> Defines;
			if (psoDesc->vsDesc->Defines.has_value())
			{
				for (ShaderDefine& d : psoDesc->vsDesc->Defines.value())
				{
					Defines.push_back({ d.Name.c_str(), d.Value.c_str() });
				}
				vs = g_dx12_rhi->CreateShaderDXC(psoDesc->vsDesc->Dir, psoDesc->vsDesc->FileName, psoDesc->vsDesc->EntryPoint, psoDesc->vsDesc->Target, Defines);
			}
			else
			{
				vs = g_dx12_rhi->CreateShaderDXC(psoDesc->vsDesc->Dir, psoDesc->vsDesc->FileName, psoDesc->vsDesc->EntryPoint, psoDesc->vsDesc->Target, std::nullopt);
			}
			dx12PSO->vs = vs;
		}
		
		{
			ComPtr<ID3DBlob> ps;
			vector< DxcDefine> Defines;
			if (psoDesc->psDesc->Defines.has_value())
			{
				for (ShaderDefine& d : psoDesc->psDesc->Defines.value())
				{
					Defines.push_back({ d.Name.c_str(), d.Value.c_str() });
				}
				ps = g_dx12_rhi->CreateShaderDXC(psoDesc->vsDesc->Dir,psoDesc->psDesc->FileName, psoDesc->psDesc->EntryPoint, psoDesc->psDesc->Target, Defines);
			}
			else
			{
				ps = g_dx12_rhi->CreateShaderDXC(psoDesc->vsDesc->Dir,psoDesc->psDesc->FileName, psoDesc->psDesc->EntryPoint, psoDesc->psDesc->Target, std::nullopt);
			}
			dx12PSO->ps = ps;

		}

		

		std::vector<D3D12_INPUT_ELEMENT_DESC> StandardVertexDescription;

		for(int i=0;i< psoDesc->InputLayout.NumElements;i++)
		{
			StandardVertexDescription.push_back({ psoDesc->InputLayout.pInputElementDescs[i].SemanticName, psoDesc->InputLayout.pInputElementDescs[i].SemanticIndex, static_cast<DXGI_FORMAT>(psoDesc->InputLayout.pInputElementDescs[i].Format),
			psoDesc->InputLayout.pInputElementDescs[i].InputSlot, psoDesc->InputLayout.pInputElementDescs[i].AlignedByteOffset, static_cast<D3D12_INPUT_CLASSIFICATION>(psoDesc->InputLayout.pInputElementDescs[i].InputSlotClass),
			psoDesc->InputLayout.pInputElementDescs[i].InstanceDataStepRate });
		};

		CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
		rasterizerStateDesc.CullMode = static_cast<D3D12_CULL_MODE>(psoDesc->CullMode);

		D3D12_BLEND_DESC blendState = {
			/*AlphaToCoverageEnable=*/psoDesc->BlendState.AlphaToCoverageEnable,
			/*IndependentBlendEnable*/psoDesc->BlendState.IndependentBlendEnable
		};

		for (int i = 0; i < 8; i++)
		{
			blendState.RenderTarget[i] = {
				/*BlendEnable=*/psoDesc->BlendState.RenderTarget[i].BlendEnable,
				/*LogicOpEnable=*/psoDesc->BlendState.RenderTarget[i].LogicOpEnable,
				/*SrcBlend=*/ static_cast<D3D12_BLEND>(psoDesc->BlendState.RenderTarget[i].SrcBlend),
				/*DestBlend=*/static_cast<D3D12_BLEND>(psoDesc->BlendState.RenderTarget[i].DestBlend),
				/*BlendOp=*/static_cast<D3D12_BLEND_OP>(psoDesc->BlendState.RenderTarget[i].BlendOp),
				/*SrcBlendAlpha=*/static_cast<D3D12_BLEND>(psoDesc->BlendState.RenderTarget[i].SrcBlendAlpha),
				/*DestBlendAlpha=*/static_cast<D3D12_BLEND>(psoDesc->BlendState.RenderTarget[i].DestBlendAlpha),
				/*BlendOpAlpha=*/static_cast<D3D12_BLEND_OP>(psoDesc->BlendState.RenderTarget[i].BlendOpAlpha),
				/*LogicOp=*/static_cast<D3D12_LOGIC_OP>(psoDesc->BlendState.RenderTarget[i].LogicOp),
				/*RenderTargetWriteMask=*/psoDesc->BlendState.RenderTarget[i].RenderTargetWriteMask,
			};
			
		}
		
		D3D12_DEPTH_STENCILOP_DESC frontFaceStencilOp =
		{
			/*StencilFailOp =*/ static_cast<D3D12_STENCIL_OP>(psoDesc->DepthStencilState.FrontFace.StencilDepthFailOp),
			/*StencilDepthFailOp =*/ static_cast<D3D12_STENCIL_OP>(psoDesc->DepthStencilState.FrontFace.StencilDepthFailOp),
			/*StencilPassOp = */static_cast<D3D12_STENCIL_OP>(psoDesc->DepthStencilState.FrontFace.StencilPassOp),
			/*StencilFunc = */static_cast<D3D12_COMPARISON_FUNC>(psoDesc->DepthStencilState.FrontFace.StencilFunc)
		};

		D3D12_DEPTH_STENCILOP_DESC backFaceStencilOp =
		{
			/*StencilFailOp =*/ static_cast<D3D12_STENCIL_OP>(psoDesc->DepthStencilState.BackFace.StencilDepthFailOp),
			/*StencilDepthFailOp =*/ static_cast<D3D12_STENCIL_OP>(psoDesc->DepthStencilState.BackFace.StencilDepthFailOp),
			/*StencilPassOp = */static_cast<D3D12_STENCIL_OP>(psoDesc->DepthStencilState.BackFace.StencilPassOp),
			/*StencilFunc = */static_cast<D3D12_COMPARISON_FUNC>(psoDesc->DepthStencilState.BackFace.StencilFunc)
		};

		D3D12_DEPTH_STENCIL_DESC depthStencilState = {
			/*DepthEnable = */psoDesc->DepthStencilState.DepthEnable,
			/*DepthWriteMask = */static_cast<D3D12_DEPTH_WRITE_MASK>(psoDesc->DepthStencilState.DepthWriteMask),
			/*DepthFunc = */static_cast<D3D12_COMPARISON_FUNC>(psoDesc->DepthStencilState.DepthFunc),
			/*StencilEnable = */psoDesc->DepthStencilState.StencilEnable,
			/*StencilReadMask = */psoDesc->DepthStencilState.StencilReadMask,
			/*StencilWriteMask = */psoDesc->DepthStencilState.StencilWriteMask,
			/*FrontFace = */frontFaceStencilOp,
			/*BackFace = */backFaceStencilOp

		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
		//psoDescMesh.InputLayout.NumElements = static_cast<UINT>(StandardVertexDescription.size());
		//memcpy((void*)psoDescMesh.InputLayout.pInputElementDescs, StandardVertexDescription.data(), StandardVertexDescription.size() * sizeof(D3D12_INPUT_ELEMENT_DESC));
		psoDescMesh.InputLayout = { StandardVertexDescription.data(), static_cast<UINT>(StandardVertexDescription.size())};
		/*for (int i = 0; i < StandardVertexDescription.size(); i++)
		{
			const D3D12_INPUT_ELEMENT_DESC& pDesc = psoDescMesh.InputLayout.pInputElementDescs[i];
			int a = 0;
		}*/

		psoDescMesh.RasterizerState = rasterizerStateDesc;
		psoDescMesh.BlendState = blendState;
		psoDescMesh.DepthStencilState = depthStencilState;
		psoDescMesh.SampleMask = psoDesc->SampleMask;
		// we only support triangle type
		psoDescMesh.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(psoDesc->PrimitiveTopologyType);
		psoDescMesh.NumRenderTargets = psoDesc->NumRenderTargets;
		for (int i = 0; i < psoDesc->NumRenderTargets; i++)
			psoDescMesh.RTVFormats[i] = static_cast<DXGI_FORMAT>(psoDesc->RTVFormats[i]);

		psoDescMesh.DSVFormat = static_cast<DXGI_FORMAT>(psoDesc->DSVFormat);// DXGI_FORMAT_D24_UNORM_S8_UINT;
		psoDescMesh.SampleDesc.Count = psoDesc->MultiSampleCount;
		psoDescMesh.SampleDesc.Quality = psoDesc->MultiSampleQuality;

		dx12PSO->graphicsPSODesc = psoDescMesh;
		bool bSucess = dx12PSO->Init();

		if (!bSucess) delete dx12PSO;

		return bSucess;
	}
	else if (g_vulkanImpl)
	{
		VKPipelineStateObject* vkPSO = static_cast<VKPipelineStateObject*>(PSO);

		VKShaderCreateInfo vsInfo = {
		psoDesc->vsDesc->Dir,//L"C:\\dev\\dx12_wrapper\\src\\Shaders\\",
		psoDesc->vsDesc->FileName,//L"SimpleDraw.hlsl",
		psoDesc->vsDesc->EntryPoint,//L"VSMain",
		psoDesc->vsDesc->Target//L"vs_6_0",
		};

		VKShaderCreateInfo psInfo = {
		psoDesc->psDesc->Dir,//L"C:\\dev\\dx12_wrapper\\src\\Shaders\\",
		psoDesc->psDesc->FileName,//L"SimpleDraw.hlsl",
		psoDesc->psDesc->EntryPoint,/*L"PSMain",*/
		psoDesc->psDesc->Target//L"ps_6_0",
		};

		VKVertexInputLayoutInfo vertLayoutInfo = {
			/*vertexStride*/ psoDesc->InputLayout.VertexStride,
		};

		for (int i = 0; i < psoDesc->InputLayout.NumElements; i++)
		{
			VkFormat format = getVkFormat(psoDesc->InputLayout.pInputElementDescs[i].Format);
			int offset = psoDesc->InputLayout.pInputElementDescs[i].AlignedByteOffset;
			vertLayoutInfo.vertexElementVec.push_back({format, offset});
		};



		PSOCreateInfo psoInfo;
		psoInfo.samples = static_cast<VkSampleCountFlagBits>(1 << (psoDesc->MultiSampleCount-1));
		psoInfo.depthFormat = getVkFormat(psoDesc->DSVFormat);//VK_FORMAT_D32_SFLOAT;

		for (int i = 0; i < psoDesc->NumRenderTargets; i++)
		{
			VkFormat format = getVkFormat(psoDesc->RTVFormats[i]);
			psoInfo.colorFormatVec.push_back(format);
		}

		psoInfo.vsInfo = &vsInfo;
		psoInfo.psInfo = &psInfo;
		psoInfo.vertexInputLayoutInfo = &vertLayoutInfo;
		psoInfo.topology = getVKTopology(psoDesc->PrimitiveTopology);

		vkPSO->Create(&psoInfo);

	}

	return false;
}

bool AbstractGfxLayer::InitPSO(GfxPipelineStateObject* PSO, COMPUTE_PIPELINE_STATE_DESC* psoDesc)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		ComPtr<ID3DBlob> cs;
		vector< DxcDefine> Defines;
		if (psoDesc->csDesc->Defines.has_value())
		{
			for (ShaderDefine& d : psoDesc->csDesc->Defines.value())
			{
				Defines.push_back({ d.Name.c_str(), d.Value.c_str() });
			}
			cs = g_dx12_rhi->CreateShaderDXC(psoDesc->csDesc->Dir, psoDesc->csDesc->FileName, psoDesc->csDesc->EntryPoint, psoDesc->csDesc->Target, Defines);
		}
		else
		{
			cs = g_dx12_rhi->CreateShaderDXC(psoDesc->csDesc->Dir, psoDesc->csDesc->FileName, psoDesc->csDesc->EntryPoint, psoDesc->csDesc->Target, std::nullopt);
		}
		dx12PSO->cs = cs;

		dx12PSO->IsCompute = true;

		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
		dx12PSO->computePSODesc = computePsoDesc;

		bool bSucess = dx12PSO->Init();

		if (!bSucess) delete dx12PSO;

		return bSucess;
	}
}

void AbstractGfxLayer::BindCBV(GfxPipelineStateObject* PSO, string name, int baseRegister, int size)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		dx12PSO->BindCBV(name, baseRegister, size);
	}
	else if (g_vulkanImpl)
	{
		VKPipelineStateObject* vkPSO = static_cast<VKPipelineStateObject*>(PSO);
		vkPSO->BindUniform(name, baseRegister);
	}
}

void AbstractGfxLayer::BindSampler(GfxPipelineStateObject* PSO, std::string name, int baseRegister)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		dx12PSO->BindSampler(name, baseRegister);
	}
	else if (g_vulkanImpl)
	{
		VKPipelineStateObject* vkPSO = static_cast<VKPipelineStateObject*>(PSO);
		vkPSO->BindSampler(name, baseRegister);
	}
}

void AbstractGfxLayer::BindSRV(GfxPipelineStateObject* PSO, std::string name, int baseRegister, int num)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		dx12PSO->BindSRV(name, baseRegister, num);
	}
	else if (g_vulkanImpl)
	{
		VKPipelineStateObject* vkPSO = static_cast<VKPipelineStateObject*>(PSO);
		vkPSO->BindTexture(name, baseRegister);
	}
}

void AbstractGfxLayer::BindUAV(GfxPipelineStateObject* PSO, std::string name, int baseRegister)
{
	if (g_dx12_rhi)
	{
		PipelineStateObject* dx12PSO = static_cast<PipelineStateObject*>(PSO);
		dx12PSO->BindUAV(name, baseRegister);
	}
}

void AbstractGfxLayer::AddHitGroup(GfxRTPipelineStateObject* PSO, std::string name, std::string chs, std::string ahs)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->AddHitGroup(name, chs, ahs);
	}
}

void AbstractGfxLayer::AddShader(GfxRTPipelineStateObject* PSO, std::string shader, RTShaderType shaderType)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->AddShader(shader, static_cast<RTPipelineStateObject::ShaderType>(shaderType));
	}
}

void AbstractGfxLayer::BindUAV(GfxRTPipelineStateObject* PSO, std::string shader, std::string name, int baseRegister)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->BindUAV(shader, name, baseRegister);
	}
}

void AbstractGfxLayer::BindSRV(GfxRTPipelineStateObject* PSO, std::string shader, std::string name, int baseRegister)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->BindSRV(shader, name, baseRegister);
	}
}

void AbstractGfxLayer::BindSampler(GfxRTPipelineStateObject* PSO, std::string shader, std::string name, int baseRegister)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->BindSampler(shader, name, baseRegister);
	}
}

void AbstractGfxLayer::BindCBV(GfxRTPipelineStateObject* PSO, std::string shader, std::string name, int baseRegister, int size)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->BindCBV(shader, name, baseRegister, size);
	}
}

void AbstractGfxLayer::SetUAV(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, GfxTexture* texture, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		Texture* dx12Texture = static_cast<Texture*>(texture);

		dx12PSO->SetUAV(shader, bindingName, dx12Texture->UAV.GpuHandle, instanceIndex);
	}
}

void AbstractGfxLayer::SetSRV(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, GfxTexture* texture, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		Texture* dx12Texture = static_cast<Texture*>(texture);

		dx12PSO->SetSRV(shader, bindingName, dx12Texture->SRV.GpuHandle, instanceIndex);
	}
}

void AbstractGfxLayer::SetSRV(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, GfxRTAS* rtas, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		RTAS* dx12RTAS= static_cast<RTAS*>(rtas);

		dx12PSO->SetSRV(shader, bindingName, dx12RTAS->Descriptor.GpuHandle, instanceIndex);
	}
}

void AbstractGfxLayer::SetSampler(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, GfxSampler* sampler, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		Sampler* dx12Sampler = static_cast<Sampler*>(sampler);

		dx12PSO->SetSampler(shader, bindingName, dx12Sampler, instanceIndex);
	}
}

void AbstractGfxLayer::SetCBVValue(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, void* pData, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->SetCBVValue(shader, bindingName, pData, instanceIndex);
	}
}

void AbstractGfxLayer::SetCBVValue(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, unsigned __int64 GPUAddr, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->SetCBVValue(shader, bindingName, GPUAddr, instanceIndex);
	}
}

void AbstractGfxLayer::BeginShaderTable(GfxRTPipelineStateObject* PSO)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->BeginShaderTable();
	}
}

void AbstractGfxLayer::EndShaderTable(GfxRTPipelineStateObject* PSO, UINT NumInstance)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->EndShaderTable(NumInstance);
	}
}

void AbstractGfxLayer::ResetHitProgram(GfxRTPipelineStateObject* PSO, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->ResetHitProgram(instanceIndex);
	}
}

void AbstractGfxLayer::StartHitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->StartHitProgram(HitGroup, instanceIndex);
	}
}

void AbstractGfxLayer::AddDescriptor2HitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, GfxDescriptor* des, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		Descriptor* dx12Descriptor = static_cast<Descriptor*>(des);

		dx12PSO->AddDescriptor2HitProgram("HitGroup", dx12Descriptor->GpuHandle, instanceIndex);

	}
}

void AbstractGfxLayer::AddSRVDescriptor2HitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, GfxTexture* resource, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		Texture* dx12resource= static_cast<Texture*>(resource);

		dx12PSO->AddDescriptor2HitProgram("HitGroup", dx12resource->SRV.GpuHandle, instanceIndex);

	}
}

void AbstractGfxLayer::AddSRVDescriptor2HitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, GfxVertexBuffer* resource, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		VertexBuffer* dx12resource = static_cast<VertexBuffer*>(resource);

		dx12PSO->AddDescriptor2HitProgram("HitGroup", dx12resource->Descriptor.GpuHandle, instanceIndex);

	}
}

void AbstractGfxLayer::AddSRVDescriptor2HitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, GfxIndexBuffer* resource, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		IndexBuffer* dx12resource = static_cast<IndexBuffer*>(resource);

		dx12PSO->AddDescriptor2HitProgram("HitGroup", dx12resource->Descriptor.GpuHandle, instanceIndex);

	}
}

void AbstractGfxLayer::AddSRVDescriptor2HitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, GfxBuffer* resource, int instanceIndex)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		Buffer* dx12resource = static_cast<Buffer*>(resource);

		dx12PSO->AddDescriptor2HitProgram("HitGroup", dx12resource->SRV.GpuHandle, instanceIndex);

	}
}


GfxDescriptor* GetSRV(GfxTexture* texture)
{
	GfxDescriptor* des = nullptr;
	if (g_dx12_rhi)
	{
		Texture* dx12Texture = static_cast<Texture*>(texture);
		des = static_cast<GfxDescriptor*>(&dx12Texture->SRV);
	}

	return des;
}

GfxDescriptor* GetSRV(GfxBuffer* buffer)
{
	GfxDescriptor* des = nullptr;
	if (g_dx12_rhi)
	{
		Buffer* dx12Buffer = static_cast<Buffer*>(buffer);
		des = static_cast<GfxDescriptor*>(&dx12Buffer->SRV);
	}

	return des;
}

GfxDescriptor* GetSRV(GfxVertexBuffer* buffer)
{
	GfxDescriptor* des = nullptr;
	if (g_dx12_rhi)
	{
		VertexBuffer* dx12Buffer = static_cast<VertexBuffer*>(buffer);
		des = static_cast<GfxDescriptor*>(&dx12Buffer->Descriptor);
	}

	return des;
}

GfxDescriptor* GetSRV(GfxIndexBuffer* buffer)
{
	GfxDescriptor* des = nullptr;
	if (g_dx12_rhi)
	{
		IndexBuffer* dx12Buffer = static_cast<IndexBuffer*>(buffer);
		des = static_cast<GfxDescriptor*>(&dx12Buffer->Descriptor);
	}

	return des;
}

void AbstractGfxLayer::UploadSRCData3D(GfxTexture* texture, SUBRESOURCE_DATA* SrcData)
{
	if (g_dx12_rhi)
	{
		Texture* dx12Texture = static_cast<Texture*>(texture);

		D3D12_SUBRESOURCE_DATA textureData = {
			SrcData->pData,
			SrcData->RowPitch,
			SrcData->SlicePitch
		};
		
		dx12Texture->UploadSRCData3D(&textureData);
	}
}

void AbstractGfxLayer::DispatchRay(GfxRTPipelineStateObject* PSO, int width, int height, GfxCommandList* CL, int NumInstance)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		CommandList* dx12CL = static_cast<CommandList*>(CL);

		dx12PSO->DispatchRay(width, height, dx12CL, NumInstance);
	}
}


bool AbstractGfxLayer::InitRTPSO(GfxRTPipelineStateObject* PSO, RTPSO_DESC* desc)
{
	if (g_dx12_rhi)
	{
		RTPipelineStateObject* dx12PSO = static_cast<RTPipelineStateObject*>(PSO);
		dx12PSO->MaxRecursion = desc->MaxRecursion;
		dx12PSO->MaxAttributeSizeInBytes = desc->MaxAttributeSizeInBytes;
		dx12PSO->MaxPayloadSizeInBytes = desc->MaxPayloadSizeInBytes;


		bool bResult;
		if (desc->Defines.has_value())
		{
			std::vector<DxcDefine> Defines;

			for (ShaderDefine& d : desc->Defines.value())
			{
				Defines.push_back({ d.Name.c_str(), d.Value.c_str() });
			}
			bResult = dx12PSO->InitRS(desc->Dir, desc->ShaderFile, Defines);
		}
		else
			bResult = dx12PSO->InitRS(desc->Dir, desc->ShaderFile);


		return bResult;
	}
}


void AbstractGfxLayer::TransitionResource(GfxCommandList* CL, int NumTransition, ResourceTransition* transitions)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);

		std::vector< D3D12_RESOURCE_BARRIER> barriers;
		for(int i=0;i<NumTransition;i++)
		{
			auto& tr = transitions[i];
			Texture* dx12Texture = static_cast<Texture*>(tr.res.texture);
			Buffer* dx12Buffer = static_cast<Buffer*>(tr.res.buffer);

			D3D12_RESOURCE_BARRIER barrier;
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			if(tr.resType == ResourceTransition::ResType::TEXTURE)
				barrier.Transition.pResource = dx12Texture->resource.Get();
			else if (tr.resType == ResourceTransition::ResType::BUFFER)
				barrier.Transition.pResource = dx12Buffer->resource.Get();

			barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(tr.before);
			barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(tr.after);
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			
			barriers.push_back(barrier);
		}

		dx12CL->CmdList->ResourceBarrier(barriers.size(), barriers.data());
	}
}

void AbstractGfxLayer::GetFrameBuffers(std::vector<std::shared_ptr<GfxTexture>>& FrameFuffers)
{
	if (g_dx12_rhi)
	{
		std::vector<std::shared_ptr<Texture>> dx12FrameFuffers;
		g_dx12_rhi->GetFrameBuffers(dx12FrameFuffers);
		for (auto& fb : dx12FrameFuffers)
			FrameFuffers.push_back(fb);
	}
	else if (g_vulkanImpl)
	{
		std::vector<std::shared_ptr<VKTexture>> vkFrameBuffers;
		g_vulkanImpl->GetFrameBuffers(vkFrameBuffers);
		for (auto& fb : vkFrameBuffers)
			FrameFuffers.push_back(fb);
	}
}

void AbstractGfxLayer::BeginFrame(std::list<GfxTexture*>& DynamicTexture)
{
	if (g_dx12_rhi)
	{
		std::list<Texture*> dx12DynamicTexture;
		for (auto& dt : DynamicTexture)
			dx12DynamicTexture.push_back(static_cast<Texture*>(dt));
		g_dx12_rhi->BeginFrame(dx12DynamicTexture);
	}
}

void AbstractGfxLayer::EndFrame()
{
	if (g_dx12_rhi)
		g_dx12_rhi->EndFrame();
}


void AbstractGfxLayer::OnSizeChanged(std::vector<std::shared_ptr<GfxTexture>>& FrameFuffers, int width, int height, bool minimized)
{
	if (g_dx12_rhi)
	{
		std::vector<Texture*> dx12Framebuffers;
		for (auto& dt : FrameFuffers)
			dx12Framebuffers.push_back(static_cast<Texture*>(dt.get()));

		AbstractGfxLayer::WaitGPUFlush();

		// Release the resources holding references to the swap chain (requirement of
		// IDXGISwapChain::ResizeBuffers) and reset the frame fence values to the
		// current fence value.
		for (UINT n = 0; n < g_dx12_rhi->NumFrame; n++)
		{
			dx12Framebuffers[n]->resource.Reset();
			g_dx12_rhi->FrameFenceValueVec[n] = g_dx12_rhi->FrameFenceValueVec[g_dx12_rhi->CurrentFrameIndex];

			//m_renderTargets[n].Reset();
			//m_fenceValues[n] = m_fenceValues[m_frameIndex];
		}




		// Resize the swap chain to the desired dimensions.
		DXGI_SWAP_CHAIN_DESC desc = {};
		g_dx12_rhi->m_swapChain->GetDesc(&desc);
		ThrowIfFailed(g_dx12_rhi->m_swapChain->ResizeBuffers(g_dx12_rhi->NumFrame, width, height, desc.BufferDesc.Format, desc.Flags));

		BOOL fullscreenState;
		ThrowIfFailed(g_dx12_rhi->m_swapChain->GetFullscreenState(&fullscreenState, nullptr));
		g_dx12_rhi->m_windowedMode = !fullscreenState;

		// Reset the frame index to the current back buffer index.
		g_dx12_rhi->CurrentFrameIndex = g_dx12_rhi->m_swapChain->GetCurrentBackBufferIndex();
	}

}

void AbstractGfxLayer::NameTexture(GfxTexture* texture, std::wstring name)
{
	if (g_dx12_rhi)
	{
		Texture* dx12Texture = static_cast<Texture*>(texture);
		dx12Texture->name = name;
		SetName(dx12Texture->resource.Get(), name.c_str());
	}
}

void AbstractGfxLayer::NameBuffer(GfxBuffer* buffer, std::wstring name)
{
	if (g_dx12_rhi)
	{
		Buffer* dx12Buffer = static_cast<Buffer*>(buffer);
		dx12Buffer->name = name;
		SetName(dx12Buffer->resource.Get(), name.c_str());
	}
}


void AbstractGfxLayer::ExecuteCommandList(GfxCommandList* cmd)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(cmd);
		g_dx12_rhi->CmdQSync->ExecuteCommandList(dx12CL);

	}
}

void AbstractGfxLayer::SetDescriptorHeap(GfxCommandList* CL)
{
	if (g_dx12_rhi)
	{
		CommandList* dx12CL = static_cast<CommandList*>(CL);
		ID3D12DescriptorHeap* ppHeaps[] = { g_dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(), g_dx12_rhi->SamplerDescriptorHeapShaderVisible->DH.Get() };
		dx12CL->CmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	}
}


GfxCommandList* AbstractGfxLayer::GetGlobalCommandList()
{
	if (g_dx12_rhi)
	{
		return g_dx12_rhi->GlobalCmdList;
	}
}

int AbstractGfxLayer::GetCurrentFrameIndex()
{
	if (g_dx12_rhi)
		return g_dx12_rhi->CurrentFrameIndex;
}


void AbstractGfxLayer::WaitGPUFlush()
{
	if (g_dx12_rhi)
		g_dx12_rhi->CmdQSync->WaitGPU();
}


bool AbstractGfxLayer::IsDX12()
{
	if (g_dx12_rhi)
		return true;
	else
		return false;
}

void AbstractGfxLayer::CreateDX12API(HWND hWnd, UINT DisplayWidth, UINT DisplayHeight)
{
	dx12_ptr = new DX12Impl(hWnd, DisplayWidth, DisplayHeight);
}

void AbstractGfxLayer::CreateVulkanAPI(HINSTANCE hInstance, HWND hWnd, UINT DisplayWidth, UINT DisplayHeight)
{
	VulkanImpl* impl = new VulkanImpl;
	bool bSuccess = impl->Init(hInstance, hWnd, DisplayWidth, DisplayHeight);
	if(bSuccess)
		vulkan_ptr = impl;

}

void AbstractGfxLayer::Release()
{
	if (dx12_ptr) delete dx12_ptr;
	if (vulkan_ptr) delete vulkan_ptr;

}
