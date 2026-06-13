#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

constexpr uint32_t RHI_BINDLESS_ARRAY = std::numeric_limits<uint32_t>::max();
constexpr uint32_t RHI_INVALID_BINDLESS_INDEX = std::numeric_limits<uint32_t>::max();

struct RHIBindlessHandle
{
	uint32_t Index = RHI_INVALID_BINDLESS_INDEX;
	uint32_t Generation = 0;

	bool IsValid() const
	{
		return Index != RHI_INVALID_BINDLESS_INDEX && Generation != 0;
	}
};

using RHITextureHandle = RHIBindlessHandle;
using RHIBufferHandle = RHIBindlessHandle;
using RHISamplerHandle = RHIBindlessHandle;

enum class RHIDescriptorKind : uint8_t
{
	SRV,
	UAV,
	CBV,
	Sampler,
	AccelerationStructure,
};

enum class RHIResourceKind : uint8_t
{
	Unknown,
	Texture,
	Buffer,
	ConstantBuffer,
	Sampler,
	AccelerationStructure,
};

enum class RHITextureDimension : uint8_t
{
	Unknown,
	Tex1D,
	Tex2D,
	Tex2DArray,
	Tex3D,
	Cube,
	CubeArray,
};

enum class RHIBufferViewKind : uint8_t
{
	None,
	Raw,
	Structured,
	Typed,
};

enum class RHIDescriptorAccess : uint8_t
{
	ReadOnly,
	ReadWrite,
};

enum class RHIShaderStage : uint32_t
{
	None = 0,
	Vertex = 1u << 0,
	Pixel = 1u << 1,
	Compute = 1u << 2,
	RayGeneration = 1u << 3,
	Miss = 1u << 4,
	ClosestHit = 1u << 5,
	AnyHit = 1u << 6,
	Intersection = 1u << 7,
	Callable = 1u << 8,
	AllGraphics = (1u << 0) | (1u << 1),
	AllRayTracing = (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8),
	All = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8),
};

using RHIShaderStageMask = uint32_t;

constexpr RHIShaderStageMask ToRHIShaderStageMask(RHIShaderStage stage)
{
	return static_cast<RHIShaderStageMask>(stage);
}

constexpr RHIShaderStageMask operator|(RHIShaderStage lhs, RHIShaderStage rhs)
{
	return ToRHIShaderStageMask(lhs) | ToRHIShaderStageMask(rhs);
}

constexpr RHIShaderStageMask operator|(RHIShaderStageMask lhs, RHIShaderStage rhs)
{
	return lhs | ToRHIShaderStageMask(rhs);
}

struct RHIBindingDesc
{
	std::string Name;
	RHIDescriptorKind DescriptorKind = RHIDescriptorKind::SRV;
	RHIResourceKind ResourceKind = RHIResourceKind::Unknown;
	RHITextureDimension TextureDimension = RHITextureDimension::Unknown;
	RHIBufferViewKind BufferView = RHIBufferViewKind::None;
	RHIDescriptorAccess Access = RHIDescriptorAccess::ReadOnly;
	uint32_t RegisterIndex = 0;
	uint32_t RegisterSpace = 0;
	uint32_t DescriptorCount = 1;
	RHIShaderStageMask Stages = ToRHIShaderStageMask(RHIShaderStage::All);
	uint32_t SizeInBytes = 0;
	uint32_t NumInstances = 1;
	bool RuntimeArray = false;
	bool PartiallyBound = false;
	bool UpdateAfterBind = false;
	bool Bindless = false;
};

struct RHIPipelineLayoutDesc
{
	std::vector<RHIBindingDesc> Bindings;
	uint32_t PushConstantBytes = 0;
};

inline uint32_t RHILegacyDescriptorCount(const RHIBindingDesc& binding)
{
	if (binding.RuntimeArray || binding.DescriptorCount == RHI_BINDLESS_ARRAY)
		return 1;
	return binding.DescriptorCount;
}

inline RHIBindingDesc MakeRHITextureSRV(
	const std::string& name,
	uint32_t registerIndex,
	RHIShaderStageMask stages,
	uint32_t descriptorCount = 1,
	uint32_t registerSpace = 0,
	RHITextureDimension dimension = RHITextureDimension::Tex2D)
{
	RHIBindingDesc binding{};
	binding.Name = name;
	binding.DescriptorKind = RHIDescriptorKind::SRV;
	binding.ResourceKind = RHIResourceKind::Texture;
	binding.TextureDimension = dimension;
	binding.RegisterIndex = registerIndex;
	binding.RegisterSpace = registerSpace;
	binding.DescriptorCount = descriptorCount;
	binding.Stages = stages;
	return binding;
}

inline RHIBindingDesc MakeRHIBindlessTextureSRV(
	const std::string& name,
	uint32_t registerIndex,
	uint32_t registerSpace,
	RHIShaderStageMask stages,
	RHITextureDimension dimension = RHITextureDimension::Tex2D)
{
	RHIBindingDesc binding = MakeRHITextureSRV(name, registerIndex, stages, RHI_BINDLESS_ARRAY, registerSpace, dimension);
	binding.RuntimeArray = true;
	binding.PartiallyBound = true;
	binding.UpdateAfterBind = true;
	binding.Bindless = true;
	return binding;
}

inline RHIBindingDesc MakeRHITextureUAV(
	const std::string& name,
	uint32_t registerIndex,
	RHIShaderStageMask stages,
	uint32_t descriptorCount = 1,
	uint32_t registerSpace = 0,
	RHITextureDimension dimension = RHITextureDimension::Tex2D)
{
	RHIBindingDesc binding = MakeRHITextureSRV(name, registerIndex, stages, descriptorCount, registerSpace, dimension);
	binding.DescriptorKind = RHIDescriptorKind::UAV;
	binding.Access = RHIDescriptorAccess::ReadWrite;
	return binding;
}

inline RHIBindingDesc MakeRHIBufferSRV(
	const std::string& name,
	uint32_t registerIndex,
	RHIShaderStageMask stages,
	RHIBufferViewKind bufferView = RHIBufferViewKind::Structured,
	uint32_t descriptorCount = 1,
	uint32_t registerSpace = 0)
{
	RHIBindingDesc binding{};
	binding.Name = name;
	binding.DescriptorKind = RHIDescriptorKind::SRV;
	binding.ResourceKind = RHIResourceKind::Buffer;
	binding.BufferView = bufferView;
	binding.RegisterIndex = registerIndex;
	binding.RegisterSpace = registerSpace;
	binding.DescriptorCount = descriptorCount;
	binding.Stages = stages;
	return binding;
}

inline RHIBindingDesc MakeRHIBindlessBufferSRV(
	const std::string& name,
	uint32_t registerIndex,
	uint32_t registerSpace,
	RHIShaderStageMask stages,
	RHIBufferViewKind bufferView = RHIBufferViewKind::Raw)
{
	RHIBindingDesc binding = MakeRHIBufferSRV(name, registerIndex, stages, bufferView, RHI_BINDLESS_ARRAY, registerSpace);
	binding.RuntimeArray = true;
	binding.PartiallyBound = true;
	binding.UpdateAfterBind = true;
	binding.Bindless = true;
	return binding;
}

inline RHIBindingDesc MakeRHIBufferUAV(
	const std::string& name,
	uint32_t registerIndex,
	RHIShaderStageMask stages,
	RHIBufferViewKind bufferView = RHIBufferViewKind::Structured,
	uint32_t descriptorCount = 1,
	uint32_t registerSpace = 0)
{
	RHIBindingDesc binding = MakeRHIBufferSRV(name, registerIndex, stages, bufferView, descriptorCount, registerSpace);
	binding.DescriptorKind = RHIDescriptorKind::UAV;
	binding.Access = RHIDescriptorAccess::ReadWrite;
	return binding;
}

inline RHIBindingDesc MakeRHIAccelerationStructureSRV(
	const std::string& name,
	uint32_t registerIndex,
	RHIShaderStageMask stages,
	uint32_t registerSpace = 0)
{
	RHIBindingDesc binding{};
	binding.Name = name;
	binding.DescriptorKind = RHIDescriptorKind::AccelerationStructure;
	binding.ResourceKind = RHIResourceKind::AccelerationStructure;
	binding.RegisterIndex = registerIndex;
	binding.RegisterSpace = registerSpace;
	binding.DescriptorCount = 1;
	binding.Stages = stages;
	return binding;
}

inline RHIBindingDesc MakeRHICBV(
	const std::string& name,
	uint32_t registerIndex,
	uint32_t sizeInBytes,
	RHIShaderStageMask stages,
	uint32_t registerSpace = 0,
	uint32_t numInstances = 1)
{
	RHIBindingDesc binding{};
	binding.Name = name;
	binding.DescriptorKind = RHIDescriptorKind::CBV;
	binding.ResourceKind = RHIResourceKind::ConstantBuffer;
	binding.RegisterIndex = registerIndex;
	binding.RegisterSpace = registerSpace;
	binding.DescriptorCount = 1;
	binding.SizeInBytes = sizeInBytes;
	binding.NumInstances = numInstances;
	binding.Stages = stages;
	return binding;
}

inline RHIBindingDesc MakeRHISampler(
	const std::string& name,
	uint32_t registerIndex,
	RHIShaderStageMask stages,
	uint32_t registerSpace = 0)
{
	RHIBindingDesc binding{};
	binding.Name = name;
	binding.DescriptorKind = RHIDescriptorKind::Sampler;
	binding.ResourceKind = RHIResourceKind::Sampler;
	binding.RegisterIndex = registerIndex;
	binding.RegisterSpace = registerSpace;
	binding.DescriptorCount = 1;
	binding.Stages = stages;
	return binding;
}
