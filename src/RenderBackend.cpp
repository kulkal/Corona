#include "stdafx.h"
#include "RenderBackend.h"
#include "VulkanBackend.h"

std::unique_ptr<IRenderBackend> CreateRenderBackend(ERenderBackendAPI api)
{
	switch (api)
	{
	case ERenderBackendAPI::Vulkan:
		return std::make_unique<VulkanBackend>();
	case ERenderBackendAPI::D3D12:
		// DX12 requires a pre-created device; bootstrap constructs DX12Backend directly.
		return nullptr;
	default:
		return nullptr;
	}
}
