#include "stdafx.h"
#include "RenderBackend.h"
#include "SimpleDX12.h"
#include "VulkanBackend.h"

std::unique_ptr<IRenderBackend> CreateRenderBackend(ERenderBackendAPI api, const Microsoft::WRL::ComPtr<ID3D12Device5>& device)
{
	switch (api)
	{
	case ERenderBackendAPI::D3D12:
		return std::make_unique<SimpleDX12>(device);
	case ERenderBackendAPI::Vulkan:
		return std::make_unique<VulkanBackend>();
	default:
		return nullptr;
	}
}
