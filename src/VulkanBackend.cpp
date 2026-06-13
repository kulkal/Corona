#include "stdafx.h"
#include "CoronaImageIO.h"
#include "PlatformSystem.h"
#include "VulkanBackend.h"
#include "RHIBuildConfig.h"
#if CORONA_HAS_D3D12
#include "DX12Backend.h"
#endif
#include "Utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

#if CORONA_HAS_DIRECTXTEX
#include "DirectXTex.h"
#endif

#if CORONA_HAS_VULKAN
#include <vulkan/vulkan.h>
#include "imgui_impl_vulkan.h"
#endif

namespace
{
	constexpr uint32_t kVulkanTextureBindingBase = 0;
	constexpr uint32_t kVulkanUavBindingBase = 32;
	constexpr uint32_t kVulkanSamplerBindingBase = 64;
	constexpr uint32_t kVulkanConstantBufferBindingBase = 128;

	const char* GetVulkanPlatformSurfaceExtension()
	{
#if CORONA_PLATFORM_IS_WINDOWS
		return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif CORONA_PLATFORM_IS_ANDROID
		return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#elif CORONA_PLATFORM_IS_MACOS || CORONA_PLATFORM_IS_IOS
		return VK_EXT_METAL_SURFACE_EXTENSION_NAME;
#else
		return nullptr;
#endif
	}
	constexpr uint32_t kVulkanGraphicsDescriptorSetsPerPool = 8192;
	constexpr uint32_t kVulkanGraphicsDescriptorPoolLimitPerFrame = 64;
	constexpr uint32_t kVulkanComputeDescriptorSetsPerPool = 256;
	constexpr VkDeviceSize kVulkanTransientUniformBytesPerFrame = 16ull * 1024ull * 1024ull;
	constexpr uint32_t kVulkanBindlessTextureDescriptorSet = 10;
	constexpr uint32_t kVulkanBindlessBufferDescriptorSet = 12;
	constexpr uint32_t kMaxRequestedVulkanBindlessSlots = 65536;

	VkDeviceSize AlignVkDeviceSize(VkDeviceSize value, VkDeviceSize alignment)
	{
		if (alignment <= 1)
			return value;
		return ((value + alignment - 1) / alignment) * alignment;
	}

	uint32_t ToVulkanTextureBinding(uint32_t registerIndex)
	{
		return kVulkanTextureBindingBase + registerIndex;
	}

	uint32_t ToVulkanUavBinding(uint32_t registerIndex)
	{
		return kVulkanUavBindingBase + registerIndex;
	}

	uint32_t ToVulkanSamplerBinding(uint32_t registerIndex)
	{
		return kVulkanSamplerBindingBase + registerIndex;
	}

	uint32_t ToVulkanConstantBufferBinding(uint32_t registerIndex)
	{
		return kVulkanConstantBufferBindingBase + registerIndex;
	}

	RHIBindingDesc MakeLegacyVulkanRHIBindingDesc(
		const std::string& name,
		RHIDescriptorKind descriptorKind,
		RHIResourceKind resourceKind,
		uint32_t baseRegister,
		uint32_t descriptorCount,
		uint32_t sizeInBytes = 0)
	{
		RHIBindingDesc binding{};
		binding.Name = name;
		binding.DescriptorKind = descriptorKind;
		binding.ResourceKind = resourceKind;
		binding.Access = descriptorKind == RHIDescriptorKind::UAV ? RHIDescriptorAccess::ReadWrite : RHIDescriptorAccess::ReadOnly;
		binding.RegisterIndex = baseRegister;
		binding.DescriptorCount = descriptorCount;
		binding.SizeInBytes = sizeInBytes;
		return binding;
	}

	VkDescriptorType ToVulkanDescriptorType(const RHIBindingDesc& binding, VkDescriptorType fallback)
	{
		switch (binding.DescriptorKind)
		{
		case RHIDescriptorKind::CBV:
			return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		case RHIDescriptorKind::Sampler:
			return VK_DESCRIPTOR_TYPE_SAMPLER;
		case RHIDescriptorKind::AccelerationStructure:
			return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		case RHIDescriptorKind::UAV:
			if (binding.ResourceKind == RHIResourceKind::Buffer)
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			if (binding.ResourceKind == RHIResourceKind::Texture)
				return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			return fallback;
		case RHIDescriptorKind::SRV:
			if (binding.ResourceKind == RHIResourceKind::AccelerationStructure)
				return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
			if (binding.ResourceKind == RHIResourceKind::Buffer)
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			if (binding.ResourceKind == RHIResourceKind::Texture)
				return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			return fallback;
		default:
			return fallback;
		}
	}

	VkShaderStageFlags ToVulkanShaderStageFlags(RHIShaderStageMask stages, VkShaderStageFlags fallback)
	{
		VkShaderStageFlags flags = 0;
		if (stages & ToRHIShaderStageMask(RHIShaderStage::Vertex))
			flags |= VK_SHADER_STAGE_VERTEX_BIT;
		if (stages & ToRHIShaderStageMask(RHIShaderStage::Pixel))
			flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
		if (stages & ToRHIShaderStageMask(RHIShaderStage::Compute))
			flags |= VK_SHADER_STAGE_COMPUTE_BIT;
		if (stages & ToRHIShaderStageMask(RHIShaderStage::RayGeneration))
			flags |= VK_SHADER_STAGE_RAYGEN_BIT_KHR;
		if (stages & ToRHIShaderStageMask(RHIShaderStage::Miss))
			flags |= VK_SHADER_STAGE_MISS_BIT_KHR;
		if (stages & ToRHIShaderStageMask(RHIShaderStage::ClosestHit))
			flags |= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
		if (stages & ToRHIShaderStageMask(RHIShaderStage::AnyHit))
			flags |= VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
		if (stages & ToRHIShaderStageMask(RHIShaderStage::Intersection))
			flags |= VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
		if (stages & ToRHIShaderStageMask(RHIShaderStage::Callable))
			flags |= VK_SHADER_STAGE_CALLABLE_BIT_KHR;
		return flags != 0 ? flags : fallback;
	}

	uint32_t ToVulkanDescriptorSet(const RHIBindingDesc& binding)
	{
		return binding.RegisterSpace;
	}

	uint32_t ToVulkanDescriptorBinding(const RHIBindingDesc& binding, uint32_t fallbackBinding)
	{
		switch (binding.DescriptorKind)
		{
		case RHIDescriptorKind::UAV:
			return ToVulkanUavBinding(binding.RegisterIndex);
		case RHIDescriptorKind::CBV:
			return ToVulkanConstantBufferBinding(binding.RegisterIndex);
		case RHIDescriptorKind::Sampler:
			return ToVulkanSamplerBinding(binding.RegisterIndex);
		case RHIDescriptorKind::SRV:
		case RHIDescriptorKind::AccelerationStructure:
			return ToVulkanTextureBinding(binding.RegisterIndex);
		default:
			return fallbackBinding;
		}
	}

	VulkanRTPipelineStateObject::BindingDesc MakeRTBindingDesc(
		const std::string& shader,
		const std::string& name,
		uint32_t baseRegister,
		uint32_t descriptorSet,
		uint32_t descriptorBinding,
		uint32_t dataSize = 0)
	{
		VulkanRTPipelineStateObject::BindingDesc binding{};
		binding.Shader = shader;
		binding.Name = name;
		binding.DescriptorSet = descriptorSet;
		binding.BaseRegister = baseRegister;
		binding.DescriptorBinding = descriptorBinding;
		binding.DataSize = dataSize;
		return binding;
	}

	VulkanComputePipelineStateObject::BindingDesc MakeComputeBindingDesc(
		const std::string& name,
		uint32_t baseRegister,
		uint32_t descriptorBinding,
		uint32_t descriptorCount,
		uint32_t dataSize,
		VkDescriptorType descriptorType)
	{
		VulkanComputePipelineStateObject::BindingDesc binding{};
		binding.Name = name;
		binding.BaseRegister = baseRegister;
		binding.DescriptorBinding = descriptorBinding;
		binding.DescriptorCount = descriptorCount;
		binding.DataSize = dataSize;
		binding.DescriptorType = descriptorType;
		return binding;
	}

	std::string BuildNotImplementedMessage(const char* functionName)
	{
		return std::string("Vulkan backend stub: ") + functionName + " is not implemented yet.";
	}

#if CORONA_HAS_VULKAN
	std::filesystem::path ResolveVulkanSpirvPath(const std::wstring& fileName);

	std::string TrimAscii(std::string value)
	{
		auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
		while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
			value.erase(value.begin());
		while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
			value.pop_back();
		return value;
	}

	std::optional<VkDescriptorType> InferRayTracingDescriptorTypeFromDeclaration(const std::string& line)
	{
		if (line.find("RaytracingAccelerationStructure") != std::string::npos)
			return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		if (line.find("RWTexture") != std::string::npos)
			return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		if (line.find("Texture") != std::string::npos)
			return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		if (line.find("ByteAddressBuffer") != std::string::npos || line.find("StructuredBuffer") != std::string::npos)
			return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		if (line.find("SamplerState") != std::string::npos)
			return VK_DESCRIPTOR_TYPE_SAMPLER;
		return std::nullopt;
	}

	std::string ExtractHlslResourceNameFromDeclaration(const std::string& line)
	{
		size_t nameEnd = line.find(':');
		if (nameEnd == std::string::npos)
			nameEnd = line.find(';');
		if (nameEnd == std::string::npos)
			return std::string();

		const std::string declarationPrefix = TrimAscii(line.substr(0, nameEnd));
		const size_t nameStart = declarationPrefix.find_last_of(" \t");
		if (nameStart == std::string::npos)
			return std::string();

		std::string resourceName = TrimAscii(declarationPrefix.substr(nameStart + 1));
		if (resourceName.size() >= 2 && resourceName.substr(resourceName.size() - 2) == "[]")
			resourceName.resize(resourceName.size() - 2);
		return resourceName;
	}

	std::unordered_map<std::string, VkDescriptorType> ParseHlslDescriptorTypes(const std::filesystem::path& shaderPath)
	{
		std::unordered_map<std::string, VkDescriptorType> descriptorTypesByName;
		std::ifstream shaderStream(shaderPath);
		if (!shaderStream.is_open())
			return descriptorTypesByName;

		std::string line;
		while (std::getline(shaderStream, line))
		{
			const std::string trimmed = TrimAscii(line);
			if (trimmed.empty() || trimmed.rfind("//", 0) == 0)
				continue;

			if (trimmed.rfind("cbuffer ", 0) == 0)
			{
				const size_t begin = std::string("cbuffer ").size();
				size_t end = trimmed.find(' ', begin);
				if (end == std::string::npos)
					end = trimmed.find(':', begin);
				if (end != std::string::npos)
					descriptorTypesByName[trimmed.substr(begin, end - begin)] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				continue;
			}

			const std::optional<VkDescriptorType> inferredType = InferRayTracingDescriptorTypeFromDeclaration(trimmed);
			if (!inferredType.has_value())
				continue;

			const std::string resourceName = ExtractHlslResourceNameFromDeclaration(trimmed);
			if (!resourceName.empty())
				descriptorTypesByName[resourceName] = *inferredType;
		}
		return descriptorTypesByName;
	}

	std::filesystem::path ResolveVulkanComputeSpirvPath(const std::wstring& shaderStem, const std::string& entryPoint)
	{
		const std::wstring entryPointWide(entryPoint.begin(), entryPoint.end());
		const std::filesystem::path entrySpecificPath = ResolveVulkanSpirvPath(shaderStem + L"_" + entryPointWide + L"Vulkan.comp.spv");
		if (std::filesystem::exists(entrySpecificPath))
			return entrySpecificPath;
		return ResolveVulkanSpirvPath(shaderStem + L"Vulkan.comp.spv");
	}

	std::filesystem::path ResolveVulkanGraphicsVertexSpirvPath(const std::wstring& shaderStem, const std::string& entryPoint)
	{
		const std::wstring entryPointWide(entryPoint.begin(), entryPoint.end());
		const std::filesystem::path entrySpecificPath = ResolveVulkanSpirvPath(shaderStem + L"_" + entryPointWide + L"Vulkan.vert.spv");
		if (std::filesystem::exists(entrySpecificPath))
			return entrySpecificPath;
		return ResolveVulkanSpirvPath(shaderStem + L"Vulkan.vert.spv");
	}
#endif

	std::filesystem::path NormalizeShaderPath(std::string shaderPath)
	{
		std::replace(shaderPath.begin(), shaderPath.end(), '\\', '/');
		return std::filesystem::path(shaderPath);
	}

	std::filesystem::path NormalizeShaderPath(std::wstring shaderPath)
	{
		std::replace(shaderPath.begin(), shaderPath.end(), L'\\', L'/');
		return std::filesystem::path(shaderPath);
	}

	std::wstring Utf8ToWide(const std::string& text)
	{
		return PlatformUtf8ToWide(text);
	}

	bool HasExtensionI(const std::wstring& filePath, const wchar_t* extension)
	{
		const std::wstring actual = std::filesystem::path(filePath).extension().wstring();
		const std::wstring expected = extension ? std::wstring(extension) : std::wstring();
		if (actual.size() != expected.size())
			return false;
		for (size_t i = 0; i < actual.size(); ++i)
		{
			if (std::towlower(actual[i]) != std::towlower(expected[i]))
				return false;
		}
		return true;
	}

	bool WriteRGBA8PNG(const std::wstring& outputPath, const std::vector<uint8_t>& rgbaPixels, uint32_t width, uint32_t height, std::wstring* errorMessage)
	{
		if (width == 0 || height == 0 || rgbaPixels.size() < static_cast<size_t>(width) * height * 4u)
		{
			if (errorMessage)
				*errorMessage = L"Vulkan PNG output has invalid dimensions or pixel data.";
			return false;
		}

		CapturedImage image;
		image.Format = ETextureFormat::RGBA8Unorm;
		image.Width = width;
		image.Height = height;
		image.RowPitch = width * 4u;
		image.Pixels = rgbaPixels;
		for (size_t i = 3; i < image.Pixels.size(); i += 4)
			image.Pixels[i] = 0xff;
		return CoronaImageIO::SavePNG(image, outputPath, errorMessage);
	}

	bool LoadRGBA8TextureFromFile(const std::wstring& filePath, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight, std::wstring* errorMessage)
	{
#if CORONA_HAS_DIRECTXTEX
		auto copyRGBA8Image = [&](const DirectX::Image& image)
		{
			if (!image.pixels || image.width == 0 || image.height == 0)
			{
				if (errorMessage) *errorMessage = L"DDS texture has invalid image data.";
				return false;
			}

			outWidth = static_cast<uint32_t>(image.width);
			outHeight = static_cast<uint32_t>(image.height);
			const size_t dstRowPitch = static_cast<size_t>(outWidth) * 4u;
			if (image.rowPitch < dstRowPitch)
			{
				if (errorMessage) *errorMessage = L"DDS texture row pitch is smaller than expected.";
				return false;
			}
			outPixels.resize(dstRowPitch * static_cast<size_t>(outHeight));
			for (uint32_t row = 0; row < outHeight; ++row)
			{
				const uint8_t* srcRow = image.pixels + static_cast<size_t>(row) * image.rowPitch;
				uint8_t* dstRow = outPixels.data() + static_cast<size_t>(row) * dstRowPitch;
				std::memcpy(dstRow, srcRow, dstRowPitch);
			}
			return true;
		};

		if (HasExtensionI(filePath, L".dds"))
		{
			DirectX::TexMetadata metadata{};
			DirectX::ScratchImage sourceImage;
			HRESULT hr = DirectX::LoadFromDDSFile(filePath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, sourceImage);
			if (FAILED(hr))
			{
				if (errorMessage) *errorMessage = L"Failed to load DDS texture file.";
				return false;
			}

			const DirectX::Image* baseImage = sourceImage.GetImage(0, 0, 0);
			if (!baseImage)
			{
				if (errorMessage) *errorMessage = L"DDS texture has no base image.";
				return false;
			}

			DirectX::ScratchImage convertedImage;
			const DirectX::Image* rgbaImage = baseImage;
			if (DirectX::IsCompressed(baseImage->format))
			{
				hr = DirectX::Decompress(*baseImage, DXGI_FORMAT_R8G8B8A8_UNORM, convertedImage);
				if (FAILED(hr))
				{
					if (errorMessage) *errorMessage = L"Failed to decompress DDS texture.";
					return false;
				}
				rgbaImage = convertedImage.GetImage(0, 0, 0);
			}
			else if (baseImage->format != DXGI_FORMAT_R8G8B8A8_UNORM)
			{
				hr = DirectX::Convert(
					*baseImage,
					DXGI_FORMAT_R8G8B8A8_UNORM,
					DirectX::TEX_FILTER_DEFAULT,
					DirectX::TEX_THRESHOLD_DEFAULT,
					convertedImage);
				if (FAILED(hr))
				{
					if (errorMessage) *errorMessage = L"Failed to convert DDS texture to RGBA8.";
					return false;
				}
				rgbaImage = convertedImage.GetImage(0, 0, 0);
			}

			return rgbaImage && copyRGBA8Image(*rgbaImage);
		}
#else
		if (HasExtensionI(filePath, L".dds"))
		{
			if (errorMessage) *errorMessage = L"DDS not supported in this build (CORONA_HAS_DIRECTXTEX=0).";
			return false;
		}
#endif // CORONA_HAS_DIRECTXTEX

		CapturedImage image;
		if (!CoronaImageIO::Load(filePath, image, false, errorMessage))
			return false;
		if (image.Width == 0 || image.Height == 0 || image.Pixels.empty())
		{
			if (errorMessage) *errorMessage = L"Texture has invalid dimensions or pixels.";
			return false;
		}
		if (image.Format != ETextureFormat::RGBA8Unorm && image.Format != ETextureFormat::BGRA8Unorm)
		{
			if (errorMessage) *errorMessage = L"Texture format is not RGBA8-compatible for Vulkan upload.";
			return false;
		}

		outWidth = image.Width;
		outHeight = image.Height;
		const size_t dstRowPitch = static_cast<size_t>(outWidth) * 4u;
		if (image.RowPitch < dstRowPitch)
		{
			if (errorMessage) *errorMessage = L"Texture row pitch is smaller than expected.";
			return false;
		}
		outPixels.resize(dstRowPitch * static_cast<size_t>(outHeight));
		for (uint32_t row = 0; row < outHeight; ++row)
		{
			const uint8_t* srcRow = image.Pixels.data() + static_cast<size_t>(row) * image.RowPitch;
			uint8_t* dstRow = outPixels.data() + static_cast<size_t>(row) * dstRowPitch;
			if (image.Format == ETextureFormat::RGBA8Unorm)
			{
				std::memcpy(dstRow, srcRow, dstRowPitch);
			}
			else
			{
				for (uint32_t x = 0; x < outWidth; ++x)
				{
					dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
					dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
					dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
					dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
				}
			}
		}

		return true;
	}

	std::vector<uint32_t> LoadSpirvFile(const std::filesystem::path& filePath)
	{
		std::ifstream file(filePath, std::ios::binary | std::ios::ate);
		if (!file.is_open())
			throw std::runtime_error("Failed to open SPIR-V shader file: " + filePath.string());

		const std::streamsize fileSize = file.tellg();
		if (fileSize <= 0 || (fileSize % 4) != 0)
			throw std::runtime_error("Invalid SPIR-V shader file size.");

		file.seekg(0, std::ios::beg);
		std::vector<uint32_t> spirv(static_cast<size_t>(fileSize) / sizeof(uint32_t));
		if (!file.read(reinterpret_cast<char*>(spirv.data()), fileSize))
			throw std::runtime_error("Failed to read SPIR-V shader file.");

		return spirv;
	}

#ifndef CORONA_VULKAN_SHADER_BINARY_DIR
#define CORONA_VULKAN_SHADER_BINARY_DIR L""
#endif

	std::filesystem::path ResolveVulkanSpirvPath(const std::wstring& fileName)
	{
		std::vector<std::filesystem::path> candidates;

		const std::filesystem::path configuredDir(CORONA_VULKAN_SHADER_BINARY_DIR);
		if (!configuredDir.empty())
			candidates.push_back(configuredDir / fileName);

		const std::filesystem::path workingDir = std::filesystem::current_path();
		candidates.push_back(workingDir / L"VulkanShaders" / fileName);
		candidates.push_back(workingDir / L"Shaders" / fileName);

		for (const std::filesystem::path& candidate : candidates)
		{
			if (std::filesystem::exists(candidate))
				return candidate;
		}
		return candidates.empty() ? std::filesystem::path(fileName) : candidates.front();
	}

#if CORONA_HAS_VULKAN
	void AppendVulkanValidationLog(const std::string& line)
	{
		const std::filesystem::path logPath = RuntimePaths::LogFile(L"vulkan_validation.log");
		std::filesystem::create_directories(logPath.parent_path());
		std::ofstream logFile(logPath, std::ios::app);
		if (logFile.is_open())
		{
			logFile << line << "\n";
		}
#if CORONA_PLATFORM_IS_WINDOWS
		OutputDebugStringA((line + "\n").c_str());
#endif
	}

	void AppendVulkanRuntimeTraceBackend(const std::wstring& line)
	{
		constexpr bool kVerboseVulkanRuntimeTrace = false;
		if (!kVerboseVulkanRuntimeTrace)
		{
			constexpr const wchar_t* kHighFrequencyPrefixes[] =
			{
				L"[VulkanRTPipelineStateObject::Apply]",
				L"[VulkanBackend::BeginFrame]",
				L"[VulkanBackend::EndFrame]",
				L"[VulkanBackend::ExecuteCurrentCommandList]",
				L"[VulkanBackend::PreviewTextureOnWindow]"
			};
			for (const wchar_t* prefix : kHighFrequencyPrefixes)
			{
				if (line.rfind(prefix, 0) == 0)
					return;
			}
		}

		const std::filesystem::path tracePath = RuntimePaths::LogFile(L"vulkan_runtime_trace.log");
		std::filesystem::create_directories(tracePath.parent_path());
		std::ofstream traceFile(tracePath, std::ios::app);
		if (traceFile.is_open())
			traceFile << PlatformWideToUtf8(line) << "\n";
	}

	bool IsVulkanValidationEnabled()
	{
		const auto value = GetPlatformEnvironmentVariable(L"CORONA_VULKAN_VALIDATION");
		if (!value || value->empty())
			return false;

		std::wstring normalized = *value;
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch)
		{
			return static_cast<wchar_t>(std::towlower(ch));
		});
		return normalized == L"1" || normalized == L"true" || normalized == L"on" || normalized == L"yes";
	}

	bool HasInstanceLayer(const char* layerName)
	{
		uint32_t layerCount = 0;
		vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
		if (layerCount == 0)
			return false;

		std::vector<VkLayerProperties> layers(layerCount);
		vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
		for (const VkLayerProperties& layer : layers)
		{
			if (strcmp(layer.layerName, layerName) == 0)
				return true;
		}

		return false;
	}

	bool HasInstanceExtension(const char* extensionName)
	{
		uint32_t extensionCount = 0;
		vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
		if (extensionCount == 0)
			return false;

		std::vector<VkExtensionProperties> extensions(extensionCount);
		vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
		for (const VkExtensionProperties& extension : extensions)
		{
			if (strcmp(extension.extensionName, extensionName) == 0)
				return true;
		}

		return false;
	}

	VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
		VkDebugUtilsMessageTypeFlagsEXT messageTypes,
		const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
		void* userData)
	{
		(void)userData;

		std::ostringstream oss;
		oss << "[validation]"
			<< " severity=0x" << std::hex << static_cast<uint32_t>(messageSeverity)
			<< " type=0x" << std::hex << static_cast<uint32_t>(messageTypes)
			<< " message=" << (callbackData && callbackData->pMessage ? callbackData->pMessage : "<null>");
		AppendVulkanValidationLog(oss.str());
		return VK_FALSE;
	}

	VkResult CreateDebugUtilsMessengerEXT(
		VkInstance instance,
		const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
		const VkAllocationCallbacks* allocator,
		VkDebugUtilsMessengerEXT* messenger)
	{
		auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
		if (!func)
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		return func(instance, createInfo, allocator, messenger);
	}

	void DestroyDebugUtilsMessengerEXT(
		VkInstance instance,
		VkDebugUtilsMessengerEXT messenger,
		const VkAllocationCallbacks* allocator)
	{
		auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
		if (func)
			func(instance, messenger, allocator);
	}

	struct VulkanTriangleContext
	{
		VkInstance Instance = VK_NULL_HANDLE;
		VkDevice Device = VK_NULL_HANDLE;
		VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
		VkQueue GraphicsQueue = VK_NULL_HANDLE;
		uint32_t GraphicsQueueFamilyIndex = UINT32_MAX;
		VkCommandPool CommandPool = VK_NULL_HANDLE;
		VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
		VkRenderPass RenderPass = VK_NULL_HANDLE;
		VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
		VkPipeline Pipeline = VK_NULL_HANDLE;
		VkImage ColorImage = VK_NULL_HANDLE;
		VkDeviceMemory ColorImageMemory = VK_NULL_HANDLE;
		VkImageView ColorImageView = VK_NULL_HANDLE;
		VkFramebuffer Framebuffer = VK_NULL_HANDLE;
		VkBuffer ReadbackBuffer = VK_NULL_HANDLE;
		VkDeviceMemory ReadbackBufferMemory = VK_NULL_HANDLE;
		VkShaderModule VertexShaderModule = VK_NULL_HANDLE;
		VkShaderModule FragmentShaderModule = VK_NULL_HANDLE;
	};

	uint32_t FindMemoryTypeIndex(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags requiredProperties)
	{
		VkPhysicalDeviceMemoryProperties memoryProperties{};
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

		for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
		{
			const bool bTypeSupported = (typeBits & (1u << i)) != 0;
			const bool bPropertyMatch = (memoryProperties.memoryTypes[i].propertyFlags & requiredProperties) == requiredProperties;
			if (bTypeSupported && bPropertyMatch)
				return i;
		}

		throw std::runtime_error("Failed to find a compatible Vulkan memory type.");
	}

	bool IsSharedRtGeometryBinding(const std::string& name)
	{
		return name == "vertices" || name == "indices" || name == "InstanceProperty";
	}

	void DestroyTriangleContext(VulkanTriangleContext& context)
	{
		if (context.Device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(context.Device);
		}

		if (context.ReadbackBuffer != VK_NULL_HANDLE)
			vkDestroyBuffer(context.Device, context.ReadbackBuffer, nullptr);
		if (context.ReadbackBufferMemory != VK_NULL_HANDLE)
			vkFreeMemory(context.Device, context.ReadbackBufferMemory, nullptr);
		if (context.Framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(context.Device, context.Framebuffer, nullptr);
		if (context.ColorImageView != VK_NULL_HANDLE)
			vkDestroyImageView(context.Device, context.ColorImageView, nullptr);
		if (context.ColorImage != VK_NULL_HANDLE)
			vkDestroyImage(context.Device, context.ColorImage, nullptr);
		if (context.ColorImageMemory != VK_NULL_HANDLE)
			vkFreeMemory(context.Device, context.ColorImageMemory, nullptr);
		if (context.Pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(context.Device, context.Pipeline, nullptr);
		if (context.PipelineLayout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(context.Device, context.PipelineLayout, nullptr);
		if (context.RenderPass != VK_NULL_HANDLE)
			vkDestroyRenderPass(context.Device, context.RenderPass, nullptr);
		if (context.VertexShaderModule != VK_NULL_HANDLE)
			vkDestroyShaderModule(context.Device, context.VertexShaderModule, nullptr);
		if (context.FragmentShaderModule != VK_NULL_HANDLE)
			vkDestroyShaderModule(context.Device, context.FragmentShaderModule, nullptr);
		if (context.CommandPool != VK_NULL_HANDLE)
			vkDestroyCommandPool(context.Device, context.CommandPool, nullptr);
		if (context.Device != VK_NULL_HANDLE)
			vkDestroyDevice(context.Device, nullptr);
		if (context.Instance != VK_NULL_HANDLE)
			vkDestroyInstance(context.Instance, nullptr);

		context = {};
	}

	static const std::array<uint32_t, 375> kTriangleVertexShaderSpirv = {
		119734787, 65536, 524299, 58, 0, 131089, 1, 393227, 1, 1280527431, 1685353262, 808793134, 0, 196622, 0, 1, 524303, 0, 4, 1852399981, 0, 13, 27, 42, 196611, 2, 450, 262149, 4, 1852399981, 0, 393221, 11, 1348430951, 1700164197, 2019914866, 0, 393222, 11, 0, 1348430951, 1953067887, 7237481, 458758, 11, 1, 1348430951, 1953393007, 1702521171, 0, 458758, 11, 2, 1130327143, 1148217708, 1635021673, 6644590, 458758, 11, 3, 1130327143, 1147956341, 1635021673, 6644590, 196613, 13, 0, 393221, 27, 1449094247, 1702130277, 1684949368, 30821, 327685, 30, 1701080681, 1818386808, 101, 327685, 42, 1131705711, 1919904879, 0, 327685, 54, 1701080681, 1818386808, 101, 196679, 11, 2, 327752, 11, 0, 11, 0, 327752, 11, 1, 11, 1, 327752, 11, 2, 11, 3, 327752, 11, 3, 11, 4, 262215, 27, 11, 42, 262215, 42, 30, 0, 131091, 2, 196641, 3, 2, 196630, 6, 32, 262167, 7, 6, 4, 262165, 8, 32, 0, 262187, 8, 9, 1, 262172, 10, 6, 9, 393246, 11, 7, 6, 10, 10, 262176, 12, 3, 11, 262203, 12, 13, 3, 262165, 14, 32, 1, 262187, 14, 15, 0, 262167, 16, 6, 2, 262187, 8, 17, 3, 262172, 18, 16, 17, 262187, 6, 19, 0, 262187, 6, 20, 3207803699, 327724, 16, 21, 19, 20, 262187, 6, 22, 1060320051, 327724, 16, 23, 22, 22, 327724, 16, 24, 20, 22, 393260, 18, 25, 21, 23, 24, 262176, 26, 1, 14, 262203, 26, 27, 1, 262176, 29, 7, 18, 262176, 31, 7, 16, 262187, 6, 34, 1065353216, 262176, 38, 3, 7, 262167, 40, 6, 3, 262176, 41, 3, 40, 262203, 41, 42, 3, 262172, 43, 40, 17, 262187, 6, 44, 1041865114, 262187, 6, 45, 1036831949, 393260, 40, 46, 34, 44, 45, 262187, 6, 47, 1048576000, 393260, 40, 48, 44, 34, 47, 262187, 6, 49, 1057803469, 393260, 40, 50, 45, 49, 34, 393260, 43, 51, 46, 48, 50, 262176, 53, 7, 43, 262176, 55, 7, 40, 327734, 2, 4, 0, 3, 131320, 5, 262203, 29, 30, 7, 262203, 53, 54, 7, 262205, 14, 28, 27, 196670, 30, 25, 327745, 31, 32, 30, 28, 262205, 16, 33, 32, 327761, 6, 35, 33, 0, 327761, 6, 36, 33, 1, 458832, 7, 37, 35, 36, 19, 34, 327745, 38, 39, 13, 15, 196670, 39, 37, 262205, 14, 52, 27, 196670, 54, 51, 327745, 55, 56, 54, 52, 262205, 40, 57, 56, 196670, 42, 57, 65789, 65592
	};

	static const std::array<uint32_t, 124> kTriangleFragmentShaderSpirv = {
		119734787, 65536, 524299, 19, 0, 131089, 1, 393227, 1, 1280527431, 1685353262, 808793134, 0, 196622, 0, 1, 458767, 4, 4, 1852399981, 0, 9, 12, 196624, 4, 7, 196611, 2, 450, 262149, 4, 1852399981, 0, 327685, 9, 1131705711, 1919904879, 0, 262149, 12, 1866690153, 7499628, 262215, 9, 30, 0, 262215, 12, 30, 0, 131091, 2, 196641, 3, 2, 196630, 6, 32, 262167, 7, 6, 4, 262176, 8, 3, 7, 262203, 8, 9, 3, 262167, 10, 6, 3, 262176, 11, 1, 10, 262203, 11, 12, 1, 262187, 6, 14, 1065353216, 327734, 2, 4, 0, 3, 131320, 5, 262205, 10, 13, 12, 327761, 6, 15, 13, 0, 327761, 6, 16, 13, 1, 327761, 6, 17, 13, 2, 458832, 7, 18, 15, 16, 17, 14, 196670, 9, 18, 65789, 65592
	};

	VkSurfaceFormatKHR ChooseSwapchainSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
	{
		for (const VkSurfaceFormatKHR& format : formats)
		{
			if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
				return format;
		}

		return formats.front();
	}

	VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes)
	{
#if CORONA_PLATFORM_IS_ANDROID
		for (VkPresentModeKHR presentMode : presentModes)
		{
			if (presentMode == VK_PRESENT_MODE_FIFO_KHR)
				return presentMode;
		}
#endif
		for (VkPresentModeKHR presentMode : presentModes)
		{
			if (presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
				return presentMode;
		}

		for (VkPresentModeKHR presentMode : presentModes)
		{
			if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
				return presentMode;
		}

		return VK_PRESENT_MODE_FIFO_KHR;
	}

	const wchar_t* GetPresentModeName(VkPresentModeKHR presentMode)
	{
		switch (presentMode)
		{
		case VK_PRESENT_MODE_IMMEDIATE_KHR: return L"Immediate";
		case VK_PRESENT_MODE_MAILBOX_KHR: return L"Mailbox";
		case VK_PRESENT_MODE_FIFO_KHR: return L"Fifo";
		case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return L"FifoRelaxed";
		default: return L"Unknown";
		}
	}

	VkExtent2D ChooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height)
	{
		if (capabilities.currentExtent.width != UINT32_MAX)
			return capabilities.currentExtent;

		VkExtent2D extent = {};
		extent.width = (std::max)(capabilities.minImageExtent.width, (std::min)(capabilities.maxImageExtent.width, width));
		extent.height = (std::max)(capabilities.minImageExtent.height, (std::min)(capabilities.maxImageExtent.height, height));
		return extent;
	}

	VkSurfaceTransformFlagBitsKHR ChooseSwapchainPreTransform(const VkSurfaceCapabilitiesKHR& capabilities)
	{
#if CORONA_PLATFORM_IS_ANDROID
		if ((capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0)
			return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
#endif
		return capabilities.currentTransform;
	}

	VkFormat ToVkFormat(ETextureFormat format)
	{
		switch (format)
		{
		case ETextureFormat::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case ETextureFormat::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
		case ETextureFormat::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
		case ETextureFormat::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
		case ETextureFormat::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
		case ETextureFormat::D32Float: return VK_FORMAT_D32_SFLOAT;
		case ETextureFormat::R32Float: return VK_FORMAT_R32_SFLOAT;
		case ETextureFormat::R8Uint: return VK_FORMAT_R8_UINT;
		}
		return VK_FORMAT_R8G8B8A8_UNORM;
	}

	ETextureFormat FromVkFormat(VkFormat format)
	{
		switch (format)
		{
		case VK_FORMAT_R16G16B16A16_SFLOAT: return ETextureFormat::RGBA16Float;
		case VK_FORMAT_R32G32B32A32_SFLOAT: return ETextureFormat::RGBA32Float;
		case VK_FORMAT_R16G16_SFLOAT: return ETextureFormat::RG16Float;
		case VK_FORMAT_R8G8B8A8_UNORM: return ETextureFormat::RGBA8Unorm;
		case VK_FORMAT_B8G8R8A8_UNORM: return ETextureFormat::BGRA8Unorm;
		case VK_FORMAT_D32_SFLOAT: return ETextureFormat::D32Float;
		case VK_FORMAT_R32_SFLOAT: return ETextureFormat::R32Float;
		case VK_FORMAT_R8_UINT: return ETextureFormat::R8Uint;
		default: return ETextureFormat::RGBA8Unorm;
		}
	}

	uint32_t GetTextureFormatBytesPerPixel(ETextureFormat format)
	{
		switch (format)
		{
		case ETextureFormat::RGBA32Float: return 16;
		case ETextureFormat::RGBA16Float: return 8;
		case ETextureFormat::RG16Float: return 4;
		case ETextureFormat::R32Float:
		case ETextureFormat::D32Float:
		case ETextureFormat::RGBA8Unorm:
		case ETextureFormat::BGRA8Unorm:
			return 4;
		case ETextureFormat::R8Uint: return 1;
		default: return 4;
		}
	}

	std::filesystem::path ResolveVulkanGraphicsFragmentSpirvPath(const std::wstring& shaderStem, const std::string& entryPoint)
	{
		const std::wstring entryPointWide(entryPoint.begin(), entryPoint.end());
		const std::filesystem::path entrySpecificPath = ResolveVulkanSpirvPath(shaderStem + L"_" + entryPointWide + L"Vulkan.frag.spv");
		if (std::filesystem::exists(entrySpecificPath))
			return entrySpecificPath;
		return ResolveVulkanSpirvPath(shaderStem + L"Vulkan.frag.spv");
	}

	VkFormat ToVkVertexFormat(EVertexAttributeFormat format)
	{
		switch (format)
		{
		case EVertexAttributeFormat::Float2: return VK_FORMAT_R32G32_SFLOAT;
		case EVertexAttributeFormat::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
		case EVertexAttributeFormat::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
		}
		return VK_FORMAT_R32G32B32A32_SFLOAT;
	}

	VkImageAspectFlags GetImageAspectFlags(ETextureFormat format)
	{
		return format == ETextureFormat::D32Float ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	}

	VkImageUsageFlags GetImageUsageFlags(ETextureUsageFlags usage)
	{
		VkImageUsageFlags flags = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if (HasTextureUsage(usage, TextureUsage_RenderTarget))
			flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if (HasTextureUsage(usage, TextureUsage_UnorderedAccess))
			flags |= VK_IMAGE_USAGE_STORAGE_BIT;
		if (HasTextureUsage(usage, TextureUsage_DepthStencil))
			flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		return flags;
	}

	VkFilter ToVkFilter(ESamplerFilter filter)
	{
		return filter == ESamplerFilter::Linear || filter == ESamplerFilter::Anisotropic ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	}

	VkSamplerAddressMode ToVkAddressMode(ESamplerAddressMode mode)
	{
		return mode == ESamplerAddressMode::Clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}

	VkImageLayout ToVkImageLayout(EResourceState state)
	{
		switch (state)
		{
		case EResourceState::ShaderRead: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		case EResourceState::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		case EResourceState::DepthWrite: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		case EResourceState::CopyDest: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		case EResourceState::CopySource: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		case EResourceState::UnorderedAccess: return VK_IMAGE_LAYOUT_GENERAL;
		case EResourceState::IndirectArgument: return VK_IMAGE_LAYOUT_GENERAL;
		case EResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		default: return VK_IMAGE_LAYOUT_GENERAL;
		}
	}

	VkAccessFlags ToVkBufferAccessMask(EResourceState state)
	{
		switch (state)
		{
		case EResourceState::ShaderRead:
			return VK_ACCESS_SHADER_READ_BIT;
		case EResourceState::UnorderedAccess:
			return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		case EResourceState::CopyDest:
			return VK_ACCESS_TRANSFER_WRITE_BIT;
		case EResourceState::CopySource:
			return VK_ACCESS_TRANSFER_READ_BIT;
		case EResourceState::VertexBuffer:
			return VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		case EResourceState::IndirectArgument:
			return VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
		default:
			return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		}
	}

	void TransitionImageLayout(
		VkCommandBuffer commandBuffer,
		VkImage image,
		VkImageAspectFlags aspectMask,
		VkImageLayout oldLayout,
		VkImageLayout newLayout)
	{
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = aspectMask;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

		if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		{
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		{
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		{
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		}

		vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	void TransitionImageLayoutGeneric(
		VkCommandBuffer commandBuffer,
		VkImage image,
		VkImageAspectFlags aspectMask,
		VkImageLayout oldLayout,
		VkImageLayout newLayout)
	{
		if (oldLayout == newLayout)
			return;

		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = aspectMask;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		auto inferAccess = [](VkImageLayout layout, bool bDst) -> VkAccessFlags
		{
			switch (layout)
			{
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return bDst ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_READ_BIT;
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return bDst ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return bDst ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT) : (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return bDst ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return bDst ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT;
			case VK_IMAGE_LAYOUT_GENERAL: return bDst ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
			case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return 0;
			default: return 0;
			}
		};

		barrier.srcAccessMask = inferAccess(oldLayout, false);
		barrier.dstAccessMask = inferAccess(newLayout, true);

		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier);
	}
#endif
}

#if CORONA_HAS_VULKAN
void VulkanRTAS::Release()
{
	if (!Owner || Owner->Device == VK_NULL_HANDLE)
		return;

	if (AccelerationStructure != VK_NULL_HANDLE && Owner->vkDestroyAccelerationStructureKHRFn)
		Owner->vkDestroyAccelerationStructureKHRFn(Owner->Device, AccelerationStructure, nullptr);
	if (ScratchBuffer != VK_NULL_HANDLE)
		vkDestroyBuffer(Owner->Device, ScratchBuffer, nullptr);
	if (ScratchMemory != VK_NULL_HANDLE)
		vkFreeMemory(Owner->Device, ScratchMemory, nullptr);
	if (ResultBuffer != VK_NULL_HANDLE)
		vkDestroyBuffer(Owner->Device, ResultBuffer, nullptr);
	if (ResultMemory != VK_NULL_HANDLE)
		vkFreeMemory(Owner->Device, ResultMemory, nullptr);
	if (InstanceBuffer != VK_NULL_HANDLE)
		vkDestroyBuffer(Owner->Device, InstanceBuffer, nullptr);
	if (InstanceMemory != VK_NULL_HANDLE)
		vkFreeMemory(Owner->Device, InstanceMemory, nullptr);

	AccelerationStructure = VK_NULL_HANDLE;
	ScratchBuffer = VK_NULL_HANDLE;
	ScratchMemory = VK_NULL_HANDLE;
	ResultBuffer = VK_NULL_HANDLE;
	ResultMemory = VK_NULL_HANDLE;
	InstanceBuffer = VK_NULL_HANDLE;
	InstanceMemory = VK_NULL_HANDLE;
	DeviceAddress = 0;
	PrimitiveCount = 0;
	NumInstances = 0;
	bAllowUpdate = false;
	bIsTopLevel = false;
	Owner = nullptr;
}

VulkanRTAS::~VulkanRTAS()
{
	Release();
}

void VulkanGraphicsPipelineHandle::Release()
{
	if (!Owner || Owner->Device == VK_NULL_HANDLE)
		return;

	if (UniformBufferMapped)
	{
		vkUnmapMemory(Owner->Device, UniformBufferMemory);
		UniformBufferMapped = nullptr;
	}
	if (UniformBuffer != VK_NULL_HANDLE)
		vkDestroyBuffer(Owner->Device, UniformBuffer, nullptr);
	if (UniformBufferMemory != VK_NULL_HANDLE)
		vkFreeMemory(Owner->Device, UniformBufferMemory, nullptr);
	if (DescriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(Owner->Device, DescriptorPool, nullptr);
	if (DescriptorSetLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(Owner->Device, DescriptorSetLayout, nullptr);
	if (Pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(Owner->Device, Pipeline, nullptr);
	if (CompatibleRenderPass != VK_NULL_HANDLE)
		vkDestroyRenderPass(Owner->Device, CompatibleRenderPass, nullptr);
	if (Layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(Owner->Device, Layout, nullptr);
	if (VertexShaderModule != VK_NULL_HANDLE)
		vkDestroyShaderModule(Owner->Device, VertexShaderModule, nullptr);
	if (FragmentShaderModule != VK_NULL_HANDLE)
		vkDestroyShaderModule(Owner->Device, FragmentShaderModule, nullptr);

	UniformBuffer = VK_NULL_HANDLE;
	UniformBufferMemory = VK_NULL_HANDLE;
	UniformBufferMapped = nullptr;
	DescriptorPool = VK_NULL_HANDLE;
	DescriptorSetLayout = VK_NULL_HANDLE;
	Pipeline = VK_NULL_HANDLE;
	CompatibleRenderPass = VK_NULL_HANDLE;
	Layout = VK_NULL_HANDLE;
	VertexShaderModule = VK_NULL_HANDLE;
	FragmentShaderModule = VK_NULL_HANDLE;
	for (auto& bindGroup : BoundBindGroups)
		bindGroup.reset();
	Owner = nullptr;
}

VulkanGraphicsPipelineHandle::~VulkanGraphicsPipelineHandle()
{
	Release();
}

void VulkanRTPipelineStateObject::Release()
{
	if (!Owner || Owner->Device == VK_NULL_HANDLE)
		return;

	ReleaseTempUniformBuffers();
	if (Pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(Owner->Device, Pipeline, nullptr);
	if (ShaderModule != VK_NULL_HANDLE)
		vkDestroyShaderModule(Owner->Device, ShaderModule, nullptr);
	if (ShaderBindingTableBuffer != VK_NULL_HANDLE)
		vkDestroyBuffer(Owner->Device, ShaderBindingTableBuffer, nullptr);
	if (ShaderBindingTableMemory != VK_NULL_HANDLE)
		vkFreeMemory(Owner->Device, ShaderBindingTableMemory, nullptr);
	if (DescriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(Owner->Device, DescriptorPool, nullptr);
	for (size_t layoutIndex = 0; layoutIndex < DescriptorSetLayouts.size(); ++layoutIndex)
	{
		if (layoutIndex < DescriptorSetLayoutOwned.size() &&
			DescriptorSetLayoutOwned[layoutIndex] &&
			DescriptorSetLayouts[layoutIndex] != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(Owner->Device, DescriptorSetLayouts[layoutIndex], nullptr);
		}
	}
	if (DescriptorSetLayouts.empty() && DescriptorSetLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(Owner->Device, DescriptorSetLayout, nullptr);
	if (PipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(Owner->Device, PipelineLayout, nullptr);

	ShaderStages.clear();
	ShaderGroups.clear();
	Pipeline = VK_NULL_HANDLE;
	ShaderModule = VK_NULL_HANDLE;
	DescriptorSet = VK_NULL_HANDLE;
	ShaderBindingTableBuffer = VK_NULL_HANDLE;
	ShaderBindingTableMemory = VK_NULL_HANDLE;
	RaygenRegion = {};
	MissRegion = {};
	HitRegion = {};
	CallableRegion = {};
	GlobalBindingValues.clear();
	HitProgramBindingValues.clear();
	DescriptorPool = VK_NULL_HANDLE;
	DescriptorSetLayout = VK_NULL_HANDLE;
	DescriptorSetLayouts.clear();
	DescriptorSetLayoutOwned.clear();
	LocalDescriptorSetNumbers.clear();
	ActiveDescriptorSets.clear();
	PipelineLayout = VK_NULL_HANDLE;
	bShaderTableOpen = false;
	bShaderTableFinalized = false;
	Owner = nullptr;
}

VulkanRTPipelineStateObject::~VulkanRTPipelineStateObject()
{
	Release();
}

void VulkanRTPipelineStateObject::ReleaseTempUniformBuffers()
{
	if (!Owner || Owner->Device == VK_NULL_HANDLE)
		return;
	for (VkBuffer buffer : TempUniformBuffers)
	{
		if (buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Owner->Device, buffer, nullptr);
	}
	for (VkDeviceMemory memory : TempUniformMemories)
	{
		if (memory != VK_NULL_HANDLE)
			vkFreeMemory(Owner->Device, memory, nullptr);
	}
	TempUniformBuffers.clear();
	TempUniformMemories.clear();
}

void VulkanRTPipelineStateObject::SetNumInstances(uint32_t numInstances)
{
	NumInstances = numInstances;
}

void VulkanRTPipelineStateObject::Configure(uint32_t maxRecursion, uint32_t maxPayloadSizeInBytes, uint32_t maxAttributeSizeInBytes)
{
	MaxRecursion = maxRecursion;
	MaxPayloadSizeInBytes = maxPayloadSizeInBytes;
	MaxAttributeSizeInBytes = maxAttributeSizeInBytes;
}

void VulkanRTPipelineStateObject::AddHitGroup(const std::string& name, const std::string& chs, const std::string& ahs)
{
	HitGroups.push_back({ name, chs, ahs });
}

void VulkanRTPipelineStateObject::AddShader(const std::string& shader, ShaderType shaderType)
{
	Shaders.push_back({ shader, shaderType });
}

void VulkanRTPipelineStateObject::BindUAV(const std::string& shader, const std::string& name, uint32_t baseRegister)
{
	UAVBindings.push_back(MakeRTBindingDesc(shader, name, baseRegister, 0, ToVulkanUavBinding(baseRegister)));
	UAVBindings.back().Schema = MakeLegacyVulkanRHIBindingDesc(name, RHIDescriptorKind::UAV, RHIResourceKind::Unknown, baseRegister, 1);
}

void VulkanRTPipelineStateObject::BindUAV(const std::string& shader, const RHIBindingDesc& binding)
{
	BindUAV(shader, binding.Name, binding.RegisterIndex);
	UAVBindings.back().Schema = binding;
	UAVBindings.back().DescriptorSet = ToVulkanDescriptorSet(binding);
	UAVBindings.back().DescriptorBinding = ToVulkanDescriptorBinding(binding, UAVBindings.back().DescriptorBinding);
}

void VulkanRTPipelineStateObject::BindSRV(const std::string& shader, const std::string& name, uint32_t baseRegister)
{
	SRVBindings.push_back(MakeRTBindingDesc(shader, name, baseRegister, 0, ToVulkanTextureBinding(baseRegister)));
	SRVBindings.back().Schema = MakeLegacyVulkanRHIBindingDesc(name, RHIDescriptorKind::SRV, RHIResourceKind::Unknown, baseRegister, 1);
}

void VulkanRTPipelineStateObject::BindSRV(const std::string& shader, const RHIBindingDesc& binding)
{
	BindSRV(shader, binding.Name, binding.RegisterIndex);
	SRVBindings.back().Schema = binding;
	SRVBindings.back().DescriptorSet = ToVulkanDescriptorSet(binding);
	SRVBindings.back().DescriptorBinding = ToVulkanDescriptorBinding(binding, SRVBindings.back().DescriptorBinding);
}

void VulkanRTPipelineStateObject::BindSampler(const std::string& shader, const std::string& name, uint32_t baseRegister)
{
	SamplerBindings.push_back(MakeRTBindingDesc(shader, name, baseRegister, 0, ToVulkanSamplerBinding(baseRegister)));
	SamplerBindings.back().Schema = MakeLegacyVulkanRHIBindingDesc(name, RHIDescriptorKind::Sampler, RHIResourceKind::Sampler, baseRegister, 1);
}

void VulkanRTPipelineStateObject::BindSampler(const std::string& shader, const RHIBindingDesc& binding)
{
	BindSampler(shader, binding.Name, binding.RegisterIndex);
	SamplerBindings.back().Schema = binding;
	SamplerBindings.back().DescriptorSet = ToVulkanDescriptorSet(binding);
	SamplerBindings.back().DescriptorBinding = ToVulkanDescriptorBinding(binding, SamplerBindings.back().DescriptorBinding);
}

void VulkanRTPipelineStateObject::BindCBV(const std::string& shader, const std::string& name, uint32_t baseRegister, uint32_t size, uint32_t numInstance)
{
	(void)numInstance;
	CBVBindings.push_back(MakeRTBindingDesc(shader, name, baseRegister, 0, ToVulkanConstantBufferBinding(baseRegister), size));
	CBVBindings.back().Schema = MakeLegacyVulkanRHIBindingDesc(name, RHIDescriptorKind::CBV, RHIResourceKind::ConstantBuffer, baseRegister, 1, size);
}

void VulkanRTPipelineStateObject::BindCBV(const std::string& shader, const RHIBindingDesc& binding)
{
	BindCBV(shader, binding.Name, binding.RegisterIndex, binding.SizeInBytes, binding.NumInstances);
	CBVBindings.back().Schema = binding;
	CBVBindings.back().DescriptorSet = ToVulkanDescriptorSet(binding);
	CBVBindings.back().DescriptorBinding = ToVulkanDescriptorBinding(binding, CBVBindings.back().DescriptorBinding);
}

void VulkanRTPipelineStateObject::SetShaderDefine(const std::string& name, const std::string& value)
{
	if (name.empty())
		return;
	auto it = std::find_if(ShaderDefines.begin(), ShaderDefines.end(), [&](const auto& entry)
	{
		return entry.first == name;
	});
	if (it != ShaderDefines.end())
		it->second = value;
	else
		ShaderDefines.emplace_back(name, value);
}

void VulkanRTPipelineStateObject::SetShaderLibraryTarget(const std::string& target)
{
	ShaderLibraryTarget = target;
}

void VulkanRTPipelineStateObject::BeginShaderTable()
{
	bShaderTableOpen = true;
	bShaderTableFinalized = false;
}

void VulkanRTPipelineStateObject::EndShaderTable()
{
	bShaderTableOpen = false;
	bShaderTableFinalized =
		Pipeline != VK_NULL_HANDLE &&
		ShaderBindingTableBuffer != VK_NULL_HANDLE &&
		RaygenRegion.deviceAddress != 0 &&
		RaygenRegion.size != 0;
}
void VulkanRTPipelineStateObject::SetTextureUAV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex)
{
	(void)shader;
	(void)instanceIndex;
	GlobalBindingValues[bindingName].TextureValue = texture;
}
void VulkanRTPipelineStateObject::SetBufferUAV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int instanceIndex)
{
	(void)shader;
	(void)instanceIndex;
	GlobalBindingValues[bindingName].BufferValue = buffer;
}
void VulkanRTPipelineStateObject::SetTextureSRV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex)
{
	(void)shader;
	(void)instanceIndex;
	GlobalBindingValues[bindingName].TextureValue = texture;
}
void VulkanRTPipelineStateObject::SetBufferSRV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int instanceIndex)
{
	(void)shader;
	(void)instanceIndex;
	GlobalBindingValues[bindingName].BufferValue = buffer;
}
bool VulkanRTPipelineStateObject::SetBindlessTextureTable(const std::string& shader, const std::string& bindingName)
{
	(void)shader;
	auto bindingIt = std::find_if(SRVBindings.begin(), SRVBindings.end(), [&](const BindingDesc& binding)
	{
		return binding.Name == bindingName && binding.Schema.Bindless && binding.Schema.ResourceKind == RHIResourceKind::Texture;
	});
	return Owner &&
		Owner->bBindlessTextureTableReady &&
		Owner->BindlessTextureDescriptorSet != VK_NULL_HANDLE &&
		bindingIt != SRVBindings.end();
}

bool VulkanRTPipelineStateObject::SetBindlessBufferTable(const std::string& shader, const std::string& bindingName)
{
	(void)shader;
	auto bindingIt = std::find_if(SRVBindings.begin(), SRVBindings.end(), [&](const BindingDesc& binding)
	{
		return binding.Name == bindingName && binding.Schema.Bindless && binding.Schema.ResourceKind == RHIResourceKind::Buffer;
	});
	return Owner &&
		Owner->bBindlessBufferTableReady &&
		Owner->BindlessBufferDescriptorSet != VK_NULL_HANDLE &&
		bindingIt != SRVBindings.end();
}
void VulkanRTPipelineStateObject::SetAccelerationStructure(const std::string& shader, const std::string& bindingName, const std::shared_ptr<RTAS>& rtas, int instanceIndex)
{
	(void)shader;
	(void)instanceIndex;
	GlobalBindingValues[bindingName].RTASValue = rtas;
}
void VulkanRTPipelineStateObject::SetSampler(const std::string& shader, const std::string& bindingName, Sampler* sampler, int instanceIndex)
{
	(void)shader;
	(void)instanceIndex;
	GlobalBindingValues[bindingName].SamplerValue = sampler;
}
void VulkanRTPipelineStateObject::SetCBVValue(const std::string& shader, const std::string& bindingName, void* pData, int instanceIndex)
{
	(void)shader;
	(void)instanceIndex;
	ResourceBindingValue& binding = GlobalBindingValues[bindingName];
	const auto bindingIt = std::find_if(CBVBindings.begin(), CBVBindings.end(), [&](const BindingDesc& desc)
	{
		return desc.Name == bindingName;
	});
	if (bindingIt == CBVBindings.end() || !pData)
		return;
	uint32_t alignedSize = bindingIt->DataSize;
	if (alignedSize == 0)
		alignedSize = 256;
	alignedSize = (alignedSize + 255u) & ~255u;
	binding.ConstantData.resize(alignedSize);
	std::memset(binding.ConstantData.data(), 0, binding.ConstantData.size());
	std::memcpy(binding.ConstantData.data(), pData, std::min<size_t>(binding.ConstantData.size(), bindingIt->DataSize));
}
void VulkanRTPipelineStateObject::ResetHitProgram(uint32_t instanceIndex)
{
	HitProgramBindingValues[instanceIndex].clear();
}
void VulkanRTPipelineStateObject::StartHitProgram(const std::string& hitGroup, uint32_t instanceIndex)
{
	(void)hitGroup;
	HitProgramBindingValues[instanceIndex].clear();
}

bool VulkanRTPipelineStateObject::InitRS(const std::string& shaderFile)
{
	ShaderFile = shaderFile;
	if (!Owner || Owner->Device == VK_NULL_HANDLE)
		return false;

	VulkanBackend* backendOwner = Owner;
	Release();
	Owner = backendOwner;

	std::filesystem::path shaderPath = NormalizeShaderPath(shaderFile);
	if (shaderPath.is_relative())
		shaderPath = RuntimePaths::SourceDirectory() / shaderPath;
	std::ifstream shaderStream(shaderPath);
	if (!shaderStream.is_open())
	{
		Owner->ErrorString += "Failed to open Vulkan RT shader source: " + shaderPath.string() + "\n";
		return false;
	}

	std::unordered_map<std::string, VkDescriptorType> descriptorTypesByName;
	std::string line;
	while (std::getline(shaderStream, line))
	{
		const std::string trimmed = TrimAscii(line);
		if (trimmed.empty() || trimmed.rfind("//", 0) == 0)
			continue;

		if (trimmed.rfind("cbuffer ", 0) == 0)
		{
			const size_t begin = std::string("cbuffer ").size();
			size_t end = trimmed.find(' ', begin);
			if (end == std::string::npos)
				end = trimmed.find(':', begin);
			if (end != std::string::npos)
				descriptorTypesByName[trimmed.substr(begin, end - begin)] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			continue;
		}

		const std::optional<VkDescriptorType> inferredType = InferRayTracingDescriptorTypeFromDeclaration(trimmed);
		if (!inferredType.has_value())
			continue;

		const std::string resourceName = ExtractHlslResourceNameFromDeclaration(trimmed);
		if (!resourceName.empty())
			descriptorTypesByName[resourceName] = *inferredType;
	}

	struct RTDescriptorSetBuildData
	{
		std::vector<VkDescriptorSetLayoutBinding> Bindings;
		VkDescriptorSetLayout BackendLayout = VK_NULL_HANDLE;
		bool bUsesBackendLayout = false;
	};

	std::map<uint32_t, RTDescriptorSetBuildData> descriptorSetBuildData;
	std::vector<VkDescriptorPoolSize> poolSizes;
	std::unordered_set<uint64_t> seenBindings;
	uint32_t layoutBindingCount = 0;
	bool bDescriptorLayoutBuildFailed = false;
	auto resolveDescriptorType = [&](const BindingDesc& binding, std::optional<VkDescriptorType> fallbackType) -> std::optional<VkDescriptorType>
	{
		const auto it = descriptorTypesByName.find(binding.Name);
		if (it != descriptorTypesByName.end())
			return it->second;
		const VkDescriptorType schemaType = ToVulkanDescriptorType(binding.Schema, VK_DESCRIPTOR_TYPE_MAX_ENUM);
		if (schemaType != VK_DESCRIPTOR_TYPE_MAX_ENUM)
			return schemaType;
		if (fallbackType.has_value())
			return fallbackType;
		if (binding.Name == "gRtScene")
			return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		if (binding.Name == "vertices" || binding.Name == "indices" || binding.Name == "InstanceProperty")
			return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		if (binding.Name.find("Tex") != std::string::npos || binding.Name.find("Texture") != std::string::npos)
			return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		return std::nullopt;
	};
	auto addPoolSize = [&](VkDescriptorType type, uint32_t descriptorCount)
	{
		if (descriptorCount == 0)
			return;
		for (auto& poolSize : poolSizes)
		{
			if (poolSize.type == type)
			{
				poolSize.descriptorCount += descriptorCount;
				return;
			}
		}
		VkDescriptorPoolSize poolSize{};
		poolSize.type = type;
		poolSize.descriptorCount = descriptorCount;
		poolSizes.push_back(poolSize);
	};

	auto appendBindings = [&](const std::vector<BindingDesc>& bindings, std::optional<VkDescriptorType> fallbackType = std::nullopt)
	{
		for (const BindingDesc& binding : bindings)
		{
			const std::optional<VkDescriptorType> resolvedType = resolveDescriptorType(binding, fallbackType);
			if (!resolvedType.has_value())
				continue;

			if (binding.Schema.Bindless)
			{
				RTDescriptorSetBuildData& setData = descriptorSetBuildData[binding.DescriptorSet];
				if (binding.Schema.ResourceKind == RHIResourceKind::Texture)
				{
					if (!Owner->bBindlessTextureTableReady || Owner->BindlessTextureDescriptorSetLayout == VK_NULL_HANDLE)
					{
						Owner->ErrorString += "Vulkan bindless texture descriptor table is not initialized.\n";
						bDescriptorLayoutBuildFailed = true;
						continue;
					}
					setData.BackendLayout = Owner->BindlessTextureDescriptorSetLayout;
					setData.bUsesBackendLayout = true;
				}
				else if (binding.Schema.ResourceKind == RHIResourceKind::Buffer)
				{
					if (!Owner->bBindlessBufferTableReady || Owner->BindlessBufferDescriptorSetLayout == VK_NULL_HANDLE)
					{
						Owner->ErrorString += "Vulkan bindless buffer descriptor table is not initialized.\n";
						bDescriptorLayoutBuildFailed = true;
						continue;
					}
					setData.BackendLayout = Owner->BindlessBufferDescriptorSetLayout;
					setData.bUsesBackendLayout = true;
				}
				continue;
			}

			const uint32_t descriptorCount = (binding.Shader == "global" || IsSharedRtGeometryBinding(binding.Name)) ? 1u : std::max(NumInstances, 1u);
			const uint64_t bindingKey = (static_cast<uint64_t>(binding.DescriptorSet) << 32ull) | binding.DescriptorBinding;
			RTDescriptorSetBuildData& setData = descriptorSetBuildData[binding.DescriptorSet];
			if (seenBindings.find(bindingKey) != seenBindings.end())
			{
				for (auto& existingBinding : setData.Bindings)
				{
					if (existingBinding.binding == binding.DescriptorBinding)
					{
						if (descriptorCount > existingBinding.descriptorCount)
						{
							addPoolSize(existingBinding.descriptorType, descriptorCount - existingBinding.descriptorCount);
							existingBinding.descriptorCount = descriptorCount;
						}
						break;
					}
				}
				continue;
			}

			VkDescriptorSetLayoutBinding layoutBinding{};
			layoutBinding.binding = binding.DescriptorBinding;
			layoutBinding.descriptorType = *resolvedType;
			layoutBinding.descriptorCount = descriptorCount;
			layoutBinding.stageFlags =
				VK_SHADER_STAGE_RAYGEN_BIT_KHR |
				VK_SHADER_STAGE_MISS_BIT_KHR |
				VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
				VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
			setData.Bindings.push_back(layoutBinding);
			seenBindings.insert(bindingKey);
			++layoutBindingCount;
			addPoolSize(*resolvedType, descriptorCount);
		}
	};

	appendBindings(UAVBindings, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	appendBindings(SRVBindings);
	appendBindings(SamplerBindings, VK_DESCRIPTOR_TYPE_SAMPLER);
	appendBindings(CBVBindings, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	if (bDescriptorLayoutBuildFailed)
		return false;

	const std::wstring shaderStem = shaderPath.stem().wstring();
	const std::filesystem::path spirvPath = ResolveVulkanSpirvPath(shaderStem + L"Vulkan.rt.spv");
	if (!std::filesystem::exists(spirvPath))
	{
		Owner->ErrorString += "Missing Vulkan RT SPIR-V module: " + spirvPath.string() + "\n";
		return false;
	}

	const std::vector<uint32_t> shaderSpirv = LoadSpirvFile(spirvPath);
	VkShaderModuleCreateInfo shaderModuleInfo{};
	shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderModuleInfo.codeSize = shaderSpirv.size() * sizeof(uint32_t);
	shaderModuleInfo.pCode = shaderSpirv.data();
	if (vkCreateShaderModule(Owner->Device, &shaderModuleInfo, nullptr, &ShaderModule) != VK_SUCCESS)
	{
		Owner->ErrorString += "Failed to create Vulkan RT shader module.\n";
		return false;
	}

	if (!descriptorSetBuildData.empty())
	{
		uint32_t maxDescriptorSet = 0;
		for (const auto& entry : descriptorSetBuildData)
			maxDescriptorSet = std::max(maxDescriptorSet, entry.first);
		if (maxDescriptorSet > 0 && Owner->EmptyDescriptorSetLayout == VK_NULL_HANDLE)
		{
			Owner->ErrorString += "Vulkan RT pipeline requires an empty descriptor set layout for sparse set indices.\n";
			return false;
		}

		DescriptorSetLayouts.assign(static_cast<size_t>(maxDescriptorSet) + 1, Owner->EmptyDescriptorSetLayout);
		DescriptorSetLayoutOwned.assign(DescriptorSetLayouts.size(), false);
		ActiveDescriptorSets.assign(DescriptorSetLayouts.size(), VK_NULL_HANDLE);

		for (auto& entry : descriptorSetBuildData)
		{
			const uint32_t setIndex = entry.first;
			RTDescriptorSetBuildData& setData = entry.second;
			if (setData.bUsesBackendLayout)
			{
				DescriptorSetLayouts[setIndex] = setData.BackendLayout;
				continue;
			}

			if (setData.Bindings.empty())
				continue;

			std::sort(setData.Bindings.begin(), setData.Bindings.end(), [](const VkDescriptorSetLayoutBinding& a, const VkDescriptorSetLayoutBinding& b)
			{
				return a.binding < b.binding;
			});

			VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
			descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			descriptorSetLayoutInfo.bindingCount = static_cast<uint32_t>(setData.Bindings.size());
			descriptorSetLayoutInfo.pBindings = setData.Bindings.data();
			if (vkCreateDescriptorSetLayout(Owner->Device, &descriptorSetLayoutInfo, nullptr, &DescriptorSetLayouts[setIndex]) != VK_SUCCESS)
			{
				Owner->ErrorString += "Failed to create Vulkan RT descriptor set layout.\n";
				return false;
			}
			DescriptorSetLayoutOwned[setIndex] = true;
			LocalDescriptorSetNumbers.push_back(setIndex);
		}

		if (!poolSizes.empty() && !LocalDescriptorSetNumbers.empty())
		{
			const uint32_t descriptorSetCount = std::max<uint32_t>(Owner->GetFrameCount(), 3u);
			VkDescriptorPoolCreateInfo descriptorPoolInfo{};
			descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			descriptorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
			descriptorPoolInfo.maxSets = descriptorSetCount * static_cast<uint32_t>(LocalDescriptorSetNumbers.size());
			for (VkDescriptorPoolSize& poolSize : poolSizes)
				poolSize.descriptorCount *= descriptorSetCount;
			descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
			descriptorPoolInfo.pPoolSizes = poolSizes.data();
			if (vkCreateDescriptorPool(Owner->Device, &descriptorPoolInfo, nullptr, &DescriptorPool) != VK_SUCCESS)
			{
				Owner->ErrorString += "Failed to create Vulkan RT descriptor pool.\n";
				return false;
			}
		}

		if (!DescriptorSetLayouts.empty())
			DescriptorSetLayout = DescriptorSetLayouts[0];
	}

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(DescriptorSetLayouts.size());
	pipelineLayoutInfo.pSetLayouts = DescriptorSetLayouts.empty() ? nullptr : DescriptorSetLayouts.data();
	if (vkCreatePipelineLayout(Owner->Device, &pipelineLayoutInfo, nullptr, &PipelineLayout) != VK_SUCCESS)
	{
		Owner->ErrorString += "Failed to create Vulkan RT pipeline layout.\n";
		return false;
	}

	ShaderStages.clear();
	ShaderGroups.clear();
	std::unordered_map<std::string, uint32_t> stageIndexByName;
	for (size_t shaderIndex = 0; shaderIndex < Shaders.size(); ++shaderIndex)
	{
		const ShaderDesc& shader = Shaders[shaderIndex];
		ShaderStageData stageData{};
		stageData.EntryPoint = shader.Name;
		stageData.CreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stageData.CreateInfo.module = ShaderModule;
		stageData.CreateInfo.pName = stageData.EntryPoint.c_str();
		switch (shader.Type)
		{
		case RTPipelineStateObject::RAYGEN:
			stageData.CreateInfo.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
			break;
		case RTPipelineStateObject::MISS:
			stageData.CreateInfo.stage = VK_SHADER_STAGE_MISS_BIT_KHR;
			break;
		case RTPipelineStateObject::HIT:
			stageData.CreateInfo.stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
			break;
		case RTPipelineStateObject::ANYHIT:
			stageData.CreateInfo.stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
			break;
		default:
			continue;
		}
		stageIndexByName[shader.Name] = static_cast<uint32_t>(ShaderStages.size());
		ShaderStages.push_back(stageData);
	}

	for (uint32_t shaderIndex = 0; shaderIndex < static_cast<uint32_t>(Shaders.size()); ++shaderIndex)
	{
		const ShaderDesc& shader = Shaders[shaderIndex];
		if (shader.Type != RTPipelineStateObject::RAYGEN && shader.Type != RTPipelineStateObject::MISS)
			continue;
		const auto stageIt = stageIndexByName.find(shader.Name);
		if (stageIt == stageIndexByName.end())
			continue;

		VkRayTracingShaderGroupCreateInfoKHR groupInfo{};
		groupInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
		groupInfo.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
		groupInfo.generalShader = stageIt->second;
		groupInfo.closestHitShader = VK_SHADER_UNUSED_KHR;
		groupInfo.anyHitShader = VK_SHADER_UNUSED_KHR;
		groupInfo.intersectionShader = VK_SHADER_UNUSED_KHR;
		ShaderGroups.push_back(groupInfo);
	}

	for (const HitGroupDesc& hitGroup : HitGroups)
	{
		VkRayTracingShaderGroupCreateInfoKHR groupInfo{};
		groupInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
		groupInfo.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
		groupInfo.generalShader = VK_SHADER_UNUSED_KHR;
		groupInfo.closestHitShader = VK_SHADER_UNUSED_KHR;
		groupInfo.anyHitShader = VK_SHADER_UNUSED_KHR;
		groupInfo.intersectionShader = VK_SHADER_UNUSED_KHR;

		for (uint32_t shaderIndex = 0; shaderIndex < static_cast<uint32_t>(Shaders.size()); ++shaderIndex)
		{
			const ShaderDesc& shader = Shaders[shaderIndex];
			if (!hitGroup.ClosestHitShader.empty() && shader.Name == hitGroup.ClosestHitShader)
			{
				const auto stageIt = stageIndexByName.find(shader.Name);
				if (stageIt != stageIndexByName.end())
					groupInfo.closestHitShader = stageIt->second;
			}
			if (!hitGroup.AnyHitShader.empty() && shader.Name == hitGroup.AnyHitShader)
			{
				const auto stageIt = stageIndexByName.find(shader.Name);
				if (stageIt != stageIndexByName.end())
					groupInfo.anyHitShader = stageIt->second;
			}
		}

		ShaderGroups.push_back(groupInfo);
	}

	if (!ShaderStages.empty() && !ShaderGroups.empty() && PipelineLayout != VK_NULL_HANDLE && Owner->vkCreateRayTracingPipelinesKHRFn)
	{
		VkRayTracingPipelineCreateInfoKHR pipelineCreateInfo{};
		pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
		pipelineCreateInfo.stageCount = static_cast<uint32_t>(ShaderStages.size());
		std::vector<VkPipelineShaderStageCreateInfo> pipelineStages;
		pipelineStages.reserve(ShaderStages.size());
		for (const ShaderStageData& stage : ShaderStages)
		{
			VkPipelineShaderStageCreateInfo stageCreateInfo = stage.CreateInfo;
			stageCreateInfo.pName = stage.EntryPoint.c_str();
			pipelineStages.push_back(stageCreateInfo);
		}
		pipelineCreateInfo.pStages = pipelineStages.data();
		pipelineCreateInfo.groupCount = static_cast<uint32_t>(ShaderGroups.size());
		pipelineCreateInfo.pGroups = ShaderGroups.data();
		pipelineCreateInfo.maxPipelineRayRecursionDepth = MaxRecursion;
		pipelineCreateInfo.layout = PipelineLayout;
		if (Owner->vkCreateRayTracingPipelinesKHRFn(Owner->Device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &Pipeline) != VK_SUCCESS)
		{
			Owner->ErrorString += "Failed to create Vulkan RT pipeline.\n";
			return false;
		}

		VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties{};
		rtProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
		VkPhysicalDeviceProperties2 properties2{};
		properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		properties2.pNext = &rtProperties;
		vkGetPhysicalDeviceProperties2(Owner->PhysicalDevice, &properties2);

		const uint32_t groupCount = static_cast<uint32_t>(ShaderGroups.size());
		const uint32_t handleSize = rtProperties.shaderGroupHandleSize;
		const uint32_t handleAlignment = rtProperties.shaderGroupHandleAlignment;
		const uint32_t baseAlignment = rtProperties.shaderGroupBaseAlignment;
		const uint32_t alignedHandleSize = (handleSize + handleAlignment - 1) & ~(handleAlignment - 1);

		PipelineStackSize = 0;
		if (Owner->vkGetRayTracingShaderGroupStackSizeKHRFn && Owner->vkCmdSetRayTracingPipelineStackSizeKHRFn)
		{
			VkDeviceSize raygenStackSize = 0;
			VkDeviceSize missStackSize = 0;
			VkDeviceSize hitStackSize = 0;
			VkDeviceSize callableStackSize = 0;
			for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex)
			{
				const VkRayTracingShaderGroupCreateInfoKHR& group = ShaderGroups[groupIndex];
				if (group.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR)
				{
					const VkPipelineShaderStageCreateInfo& stage = pipelineStages[group.generalShader];
					const VkDeviceSize stackSize = Owner->vkGetRayTracingShaderGroupStackSizeKHRFn(
						Owner->Device,
						Pipeline,
						groupIndex,
						VK_SHADER_GROUP_SHADER_GENERAL_KHR);
					if (stage.stage == VK_SHADER_STAGE_RAYGEN_BIT_KHR)
						raygenStackSize = std::max(raygenStackSize, stackSize);
					else if (stage.stage == VK_SHADER_STAGE_MISS_BIT_KHR)
						missStackSize = std::max(missStackSize, stackSize);
					else if (stage.stage == VK_SHADER_STAGE_CALLABLE_BIT_KHR)
						callableStackSize = std::max(callableStackSize, stackSize);
				}
				else
				{
					VkDeviceSize groupHitStackSize = 0;
					if (group.closestHitShader != VK_SHADER_UNUSED_KHR)
					{
						groupHitStackSize += Owner->vkGetRayTracingShaderGroupStackSizeKHRFn(
							Owner->Device,
							Pipeline,
							groupIndex,
							VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR);
					}
					if (group.anyHitShader != VK_SHADER_UNUSED_KHR)
					{
						groupHitStackSize += Owner->vkGetRayTracingShaderGroupStackSizeKHRFn(
							Owner->Device,
							Pipeline,
							groupIndex,
							VK_SHADER_GROUP_SHADER_ANY_HIT_KHR);
					}
					if (group.intersectionShader != VK_SHADER_UNUSED_KHR)
					{
						groupHitStackSize += Owner->vkGetRayTracingShaderGroupStackSizeKHRFn(
							Owner->Device,
							Pipeline,
							groupIndex,
							VK_SHADER_GROUP_SHADER_INTERSECTION_KHR);
					}
					hitStackSize = std::max(hitStackSize, groupHitStackSize);
				}
			}

			const VkDeviceSize continuationStackSize = std::max(missStackSize, hitStackSize);
			if (continuationStackSize > 0)
				PipelineStackSize = raygenStackSize + static_cast<VkDeviceSize>(std::max(MaxRecursion, 1u)) * continuationStackSize + 2 * callableStackSize;
			AppendVulkanRuntimeTraceBackend(
				L"[VulkanRTPipelineStateObject::InitRS] stackSize=" + std::to_wstring(PipelineStackSize) +
				L" raygen=" + std::to_wstring(raygenStackSize) +
				L" miss=" + std::to_wstring(missStackSize) +
				L" hit=" + std::to_wstring(hitStackSize));
		}

		uint32_t raygenCount = 0;
		uint32_t missCount = 0;
		uint32_t hitCount = 0;
		for (const VkRayTracingShaderGroupCreateInfoKHR& group : ShaderGroups)
		{
			if (group.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR)
			{
				const VkPipelineShaderStageCreateInfo& stage = pipelineStages[group.generalShader];
				if (stage.stage == VK_SHADER_STAGE_RAYGEN_BIT_KHR)
					++raygenCount;
				else if (stage.stage == VK_SHADER_STAGE_MISS_BIT_KHR)
					++missCount;
			}
			else
			{
				++hitCount;
			}
		}

		const uint32_t raygenRegionSize = std::max(1u, raygenCount) * alignedHandleSize;
		const uint32_t missRegionSize = std::max(1u, missCount) * alignedHandleSize;
		const uint32_t hitRegionSize = std::max(1u, hitCount) * alignedHandleSize;
		const uint32_t raygenOffset = 0;
		const uint32_t missOffset = (raygenRegionSize + baseAlignment - 1) & ~(baseAlignment - 1);
		const uint32_t hitOffset = (missOffset + missRegionSize + baseAlignment - 1) & ~(baseAlignment - 1);
		const uint32_t sbtSize = hitOffset + hitRegionSize;

		std::vector<uint8_t> shaderGroupHandles(static_cast<size_t>(groupCount) * handleSize);
		if (Owner->vkGetRayTracingShaderGroupHandlesKHRFn(Owner->Device, Pipeline, 0, groupCount, shaderGroupHandles.size(), shaderGroupHandles.data()) != VK_SUCCESS)
		{
			Owner->ErrorString += "Failed to fetch Vulkan RT shader group handles.\n";
			return false;
		}

		if (!Owner->CreateBufferWithMemory(
			sbtSize,
			VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			true,
			ShaderBindingTableBuffer,
			ShaderBindingTableMemory))
		{
			Owner->ErrorString += "Failed to create Vulkan RT shader binding table buffer.\n";
			return false;
		}

		void* mappedData = nullptr;
		if (vkMapMemory(Owner->Device, ShaderBindingTableMemory, 0, sbtSize, 0, &mappedData) != VK_SUCCESS)
		{
			Owner->ErrorString += "Failed to map Vulkan RT shader binding table memory.\n";
			return false;
		}

		std::vector<uint8_t> sbtData(sbtSize, 0);
		uint32_t raygenWriteIndex = 0;
		uint32_t missWriteIndex = 0;
		uint32_t hitWriteIndex = 0;
		for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex)
		{
			const VkRayTracingShaderGroupCreateInfoKHR& group = ShaderGroups[groupIndex];
			const uint8_t* handleSrc = shaderGroupHandles.data() + static_cast<size_t>(groupIndex) * handleSize;
			if (group.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR)
			{
				const VkPipelineShaderStageCreateInfo& stage = pipelineStages[group.generalShader];
				if (stage.stage == VK_SHADER_STAGE_RAYGEN_BIT_KHR)
				{
					std::memcpy(sbtData.data() + raygenOffset + raygenWriteIndex * alignedHandleSize, handleSrc, handleSize);
					++raygenWriteIndex;
				}
				else if (stage.stage == VK_SHADER_STAGE_MISS_BIT_KHR)
				{
					std::memcpy(sbtData.data() + missOffset + missWriteIndex * alignedHandleSize, handleSrc, handleSize);
					++missWriteIndex;
				}
			}
			else
			{
				std::memcpy(sbtData.data() + hitOffset + hitWriteIndex * alignedHandleSize, handleSrc, handleSize);
				++hitWriteIndex;
			}
		}
		std::memcpy(mappedData, sbtData.data(), sbtData.size());
		vkUnmapMemory(Owner->Device, ShaderBindingTableMemory);

		const VkDeviceAddress sbtAddress = Owner->GetBufferDeviceAddress(ShaderBindingTableBuffer);
		RaygenRegion.deviceAddress = sbtAddress + raygenOffset;
		RaygenRegion.stride = alignedHandleSize;
		RaygenRegion.size = raygenRegionSize;
		MissRegion.deviceAddress = sbtAddress + missOffset;
		MissRegion.stride = alignedHandleSize;
		MissRegion.size = missRegionSize;
		HitRegion.deviceAddress = sbtAddress + hitOffset;
		HitRegion.stride = alignedHandleSize;
		HitRegion.size = hitRegionSize;
		CallableRegion = {};
	}

	std::ostringstream ss;
	ss << "Vulkan RT layout created from " << shaderFile
		<< ", bindings=" << layoutBindingCount
		<< ", shaders=" << Shaders.size()
		<< ", stages=" << ShaderStages.size()
		<< ", groups=" << ShaderGroups.size()
		<< ", hitGroups=" << HitGroups.size()
		<< ", pipeline=" << (Pipeline != VK_NULL_HANDLE ? 1 : 0)
		<< ". Vulkan RT trace path initialized.\n";
	Owner->ErrorString += ss.str();
	return Pipeline != VK_NULL_HANDLE;
}

void VulkanRTPipelineStateObject::Apply(uint32_t width, uint32_t height)
{
	if (!Owner || Owner->ActiveCommandBuffer == VK_NULL_HANDLE || Pipeline == VK_NULL_HANDLE || PipelineLayout == VK_NULL_HANDLE)
		return;
	AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] begin shader=" + Utf8ToWide(ShaderFile));

	if (ActiveDescriptorSets.size() != DescriptorSetLayouts.size())
		ActiveDescriptorSets.assign(DescriptorSetLayouts.size(), VK_NULL_HANDLE);
	else
		std::fill(ActiveDescriptorSets.begin(), ActiveDescriptorSets.end(), VK_NULL_HANDLE);

	if (kVulkanBindlessTextureDescriptorSet < ActiveDescriptorSets.size() &&
		DescriptorSetLayouts[kVulkanBindlessTextureDescriptorSet] == Owner->BindlessTextureDescriptorSetLayout)
	{
		ActiveDescriptorSets[kVulkanBindlessTextureDescriptorSet] = Owner->BindlessTextureDescriptorSet;
	}
	if (kVulkanBindlessBufferDescriptorSet < ActiveDescriptorSets.size() &&
		DescriptorSetLayouts[kVulkanBindlessBufferDescriptorSet] == Owner->BindlessBufferDescriptorSetLayout)
	{
		ActiveDescriptorSets[kVulkanBindlessBufferDescriptorSet] = Owner->BindlessBufferDescriptorSet;
	}

	for (uint32_t setIndex : LocalDescriptorSetNumbers)
	{
		if (setIndex >= DescriptorSetLayouts.size() ||
			DescriptorSetLayouts[setIndex] == VK_NULL_HANDLE ||
			DescriptorPool == VK_NULL_HANDLE)
		{
			return;
		}

		VkDescriptorSet activeDescriptorSet = VK_NULL_HANDLE;
		VkDescriptorSetAllocateInfo descriptorSetAllocateInfo{};
		descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		descriptorSetAllocateInfo.descriptorPool = DescriptorPool;
		descriptorSetAllocateInfo.descriptorSetCount = 1;
		descriptorSetAllocateInfo.pSetLayouts = &DescriptorSetLayouts[setIndex];
		if (vkAllocateDescriptorSets(Owner->Device, &descriptorSetAllocateInfo, &activeDescriptorSet) != VK_SUCCESS)
			return;
		Owner->TrackFrameDescriptorSet(DescriptorPool, activeDescriptorSet);
		ActiveDescriptorSets[setIndex] = activeDescriptorSet;
	}
	DescriptorSet = !ActiveDescriptorSets.empty() ? ActiveDescriptorSets[0] : VK_NULL_HANDLE;

	std::vector<VkWriteDescriptorSet> writes;
	std::vector<VkDescriptorImageInfo> imageInfos;
	std::vector<VkDescriptorBufferInfo> bufferInfos;
	std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelInfos;
	writes.reserve(256);
	imageInfos.reserve(512);
	bufferInfos.reserve(512);
	accelInfos.reserve(64);

	auto getDescriptorSetForBinding = [&](const BindingDesc& binding) -> VkDescriptorSet
	{
		if (binding.DescriptorSet >= ActiveDescriptorSets.size())
			return VK_NULL_HANDLE;
		return ActiveDescriptorSets[binding.DescriptorSet];
	};

	auto appendImageWrite = [&](const BindingDesc& binding, VkDescriptorType type, VkImageView imageView, VkImageLayout imageLayout, VkSampler sampler)
	{
		VkDescriptorSet dstSet = getDescriptorSetForBinding(binding);
		if (dstSet == VK_NULL_HANDLE)
			return;

		VkDescriptorImageInfo& imageInfo = imageInfos.emplace_back();
		imageInfo.imageView = imageView;
		imageInfo.imageLayout = imageLayout;
		imageInfo.sampler = sampler;

		VkWriteDescriptorSet& write = writes.emplace_back();
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = dstSet;
		write.dstBinding = binding.DescriptorBinding;
		write.descriptorCount = 1;
		write.descriptorType = type;
		write.pImageInfo = &imageInfo;
	};

	auto appendBufferWrite = [&](const BindingDesc& binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range)
	{
		VkDescriptorSet dstSet = getDescriptorSetForBinding(binding);
		if (dstSet == VK_NULL_HANDLE)
			return;

		VkDescriptorBufferInfo& bufferInfo = bufferInfos.emplace_back();
		bufferInfo.buffer = buffer;
		bufferInfo.offset = offset;
		bufferInfo.range = range;

		VkWriteDescriptorSet& write = writes.emplace_back();
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = dstSet;
		write.dstBinding = binding.DescriptorBinding;
		write.descriptorCount = 1;
		write.descriptorType = type;
		write.pBufferInfo = &bufferInfo;
	};

	for (const BindingDesc& binding : UAVBindings)
	{
		if (binding.Shader != "global")
			continue;
		auto valueIt = GlobalBindingValues.find(binding.Name);
		if (valueIt == GlobalBindingValues.end())
			continue;
		if (valueIt->second.TextureValue)
		{
			auto textureIt = Owner->TextureAllocations.find(valueIt->second.TextureValue);
			if (textureIt != Owner->TextureAllocations.end())
				appendImageWrite(binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, textureIt->second.ImageView, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
		}
		else if (valueIt->second.BufferValue)
		{
			auto bufferIt = Owner->BufferAllocations.find(valueIt->second.BufferValue);
			if (bufferIt != Owner->BufferAllocations.end())
				appendBufferWrite(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferIt->second.Buffer, bufferIt->second.Offset, bufferIt->second.SizeInBytes);
		}
	}

	for (const BindingDesc& binding : SRVBindings)
	{
		if (binding.Shader != "global")
			continue;
		auto valueIt = GlobalBindingValues.find(binding.Name);
		if (valueIt == GlobalBindingValues.end())
			continue;

		auto textureIt = valueIt->second.TextureValue ? Owner->TextureAllocations.find(valueIt->second.TextureValue) : Owner->TextureAllocations.end();
		auto bufferIt = valueIt->second.BufferValue ? Owner->BufferAllocations.find(valueIt->second.BufferValue) : Owner->BufferAllocations.end();
		auto rtas = valueIt->second.RTASValue ? dynamic_cast<VulkanRTAS*>(valueIt->second.RTASValue.get()) : nullptr;

		if (textureIt != Owner->TextureAllocations.end())
		{
			appendImageWrite(binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureIt->second.ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_NULL_HANDLE);
		}
		else if (bufferIt != Owner->BufferAllocations.end())
		{
			appendBufferWrite(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferIt->second.Buffer, bufferIt->second.Offset, bufferIt->second.SizeInBytes);
		}
		else if (rtas && rtas->AccelerationStructure != VK_NULL_HANDLE)
		{
			VkDescriptorSet dstSet = getDescriptorSetForBinding(binding);
			if (dstSet == VK_NULL_HANDLE)
				continue;
			VkWriteDescriptorSetAccelerationStructureKHR& accelInfo = accelInfos.emplace_back();
			accelInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
			accelInfo.accelerationStructureCount = 1;
			accelInfo.pAccelerationStructures = &rtas->AccelerationStructure;

			VkWriteDescriptorSet& write = writes.emplace_back();
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.pNext = &accelInfo;
			write.dstSet = dstSet;
			write.dstBinding = binding.DescriptorBinding;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		}
	}

	for (const BindingDesc& binding : SamplerBindings)
	{
		if (binding.Shader != "global")
			continue;
		auto valueIt = GlobalBindingValues.find(binding.Name);
		if (valueIt == GlobalBindingValues.end() || !valueIt->second.SamplerValue)
			continue;
		auto samplerIt = Owner->SamplerAllocations.find(valueIt->second.SamplerValue);
		if (samplerIt == Owner->SamplerAllocations.end())
			continue;
		appendImageWrite(binding, VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, samplerIt->second.SamplerHandle);
	}

	for (const BindingDesc& binding : CBVBindings)
	{
		if (binding.Shader != "global")
			continue;
		auto valueIt = GlobalBindingValues.find(binding.Name);
		if (valueIt == GlobalBindingValues.end() || valueIt->second.ConstantData.empty())
			continue;

		VkBuffer uniformBuffer = VK_NULL_HANDLE;
		VkDeviceSize uniformOffset = 0;
		VkDeviceSize uniformRange = 0;
		if (!Owner->UploadTransientUniformData(
			valueIt->second.ConstantData.data(),
			valueIt->second.ConstantData.size(),
			uniformBuffer,
			uniformOffset,
			uniformRange,
			TempUniformBuffers,
			TempUniformMemories))
		{
			continue;
		}

		appendBufferWrite(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniformBuffer, uniformOffset, uniformRange);
	}

	std::unordered_map<std::string, uint32_t> hitBindingSlotByName;
	uint32_t nextHitBindingSlot = 0;
	for (const BindingDesc& binding : SRVBindings)
	{
		if (binding.Shader == "global")
			continue;
		if (hitBindingSlotByName.find(binding.Name) == hitBindingSlotByName.end())
			hitBindingSlotByName[binding.Name] = nextHitBindingSlot++;
	}

	for (const BindingDesc& binding : SRVBindings)
	{
		if (binding.Shader == "global")
			continue;
		const auto slotIt = hitBindingSlotByName.find(binding.Name);
		if (slotIt == hitBindingSlotByName.end())
			continue;
		const uint32_t slotIndex = slotIt->second;

		const size_t writeStartImage = imageInfos.size();
		const size_t writeStartBuffer = bufferInfos.size();
		const size_t writeStartAccel = accelInfos.size();
		uint32_t descriptorCount = 0;
		VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
		const uint32_t hitDescriptorInstanceCount = IsSharedRtGeometryBinding(binding.Name) ? 1u : NumInstances;

		for (uint32_t instanceIndex = 0; instanceIndex < hitDescriptorInstanceCount; ++instanceIndex)
		{
			auto hitIt = HitProgramBindingValues.find(instanceIndex);
			if (hitIt == HitProgramBindingValues.end() || hitIt->second.size() <= slotIndex)
				continue;
			const ResourceBindingValue& value = hitIt->second[slotIndex];

			if (value.TextureValue)
			{
				auto textureIt = Owner->TextureAllocations.find(value.TextureValue);
				if (textureIt == Owner->TextureAllocations.end())
					continue;
				descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				VkDescriptorImageInfo& imageInfo = imageInfos.emplace_back();
				imageInfo.imageView = textureIt->second.ImageView;
				imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				imageInfo.sampler = VK_NULL_HANDLE;
				++descriptorCount;
			}
			else if (value.BufferValue)
			{
				auto bufferIt = Owner->BufferAllocations.find(value.BufferValue);
				if (bufferIt == Owner->BufferAllocations.end())
					continue;
				descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				VkDescriptorBufferInfo& bufferInfo = bufferInfos.emplace_back();
				bufferInfo.buffer = bufferIt->second.Buffer;
				bufferInfo.offset = 0;
				bufferInfo.range = bufferIt->second.SizeInBytes;
				++descriptorCount;
			}
			else if (value.VertexBufferValue)
			{
				auto bufferIt = Owner->VertexBufferAllocations.find(value.VertexBufferValue);
				if (bufferIt == Owner->VertexBufferAllocations.end())
					continue;
				descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				VkDescriptorBufferInfo& bufferInfo = bufferInfos.emplace_back();
				bufferInfo.buffer = bufferIt->second.Buffer;
				bufferInfo.offset = 0;
				bufferInfo.range = bufferIt->second.SizeInBytes;
				++descriptorCount;
			}
			else if (value.IndexBufferValue)
			{
				auto bufferIt = Owner->IndexBufferAllocations.find(value.IndexBufferValue);
				if (bufferIt == Owner->IndexBufferAllocations.end())
					continue;
				descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				VkDescriptorBufferInfo& bufferInfo = bufferInfos.emplace_back();
				bufferInfo.buffer = bufferIt->second.Buffer;
				bufferInfo.offset = 0;
				bufferInfo.range = bufferIt->second.SizeInBytes;
				++descriptorCount;
			}
			else if (value.RTASValue)
			{
				VulkanRTAS* rtas = dynamic_cast<VulkanRTAS*>(value.RTASValue.get());
				if (!rtas || rtas->AccelerationStructure == VK_NULL_HANDLE)
					continue;
				VkDescriptorSet dstSet = getDescriptorSetForBinding(binding);
				if (dstSet == VK_NULL_HANDLE)
					continue;
				descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
				VkWriteDescriptorSetAccelerationStructureKHR& accelInfo = accelInfos.emplace_back();
				accelInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
				accelInfo.accelerationStructureCount = 1;
				accelInfo.pAccelerationStructures = &rtas->AccelerationStructure;
				VkWriteDescriptorSet& write = writes.emplace_back();
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.pNext = &accelInfo;
				write.dstSet = dstSet;
				write.dstBinding = binding.DescriptorBinding;
				write.descriptorCount = 1;
				write.descriptorType = descriptorType;
				++descriptorCount;
			}
		}

		if (descriptorCount == 0)
		{
			imageInfos.resize(writeStartImage);
			bufferInfos.resize(writeStartBuffer);
			accelInfos.resize(writeStartAccel);
			continue;
		}

		if (descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		{
			VkDescriptorSet dstSet = getDescriptorSetForBinding(binding);
			if (dstSet == VK_NULL_HANDLE)
				continue;
			VkWriteDescriptorSet& write = writes.emplace_back();
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.dstSet = dstSet;
			write.dstBinding = binding.DescriptorBinding;
			write.descriptorCount = descriptorCount;
			write.descriptorType = descriptorType;
			if (descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
				write.pImageInfo = imageInfos.data() + writeStartImage;
			else
				write.pBufferInfo = bufferInfos.data() + writeStartBuffer;
		}
	}

	if (!writes.empty())
	{
		AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] before vkUpdateDescriptorSets writes=" + std::to_wstring(writes.size()));
		vkUpdateDescriptorSets(Owner->Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] after vkUpdateDescriptorSets");
	}

	AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] before vkCmdBindPipeline");
	vkCmdBindPipeline(Owner->ActiveCommandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, Pipeline);
	AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] before vkCmdBindDescriptorSets");
	for (uint32_t setIndex = 0; setIndex < static_cast<uint32_t>(ActiveDescriptorSets.size()); ++setIndex)
	{
		VkDescriptorSet descriptorSet = ActiveDescriptorSets[setIndex];
		if (descriptorSet == VK_NULL_HANDLE)
			continue;
		vkCmdBindDescriptorSets(
			Owner->ActiveCommandBuffer,
			VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
			PipelineLayout,
			setIndex,
			1,
			&descriptorSet,
			0,
			nullptr);
	}
	if (Owner->vkCmdSetRayTracingPipelineStackSizeKHRFn && PipelineStackSize > 0)
	{
		Owner->vkCmdSetRayTracingPipelineStackSizeKHRFn(Owner->ActiveCommandBuffer, PipelineStackSize);
	}
	if (width == 0 || height == 0)
	{
		AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] bound without direct trace");
		return;
	}
	AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] before vkCmdTraceRaysKHR");
	Owner->vkCmdTraceRaysKHRFn(Owner->ActiveCommandBuffer, &RaygenRegion, &MissRegion, &HitRegion, &CallableRegion, width, height, 1);
	AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] after vkCmdTraceRaysKHR");
}

bool VulkanRTPipelineStateObject::GetDispatchRaysIndirectTemplate(uint32_t width, uint32_t height, RtDispatchRaysIndirectTemplate& outTemplate) const
{
	outTemplate = {};
	if (!Owner ||
		!Owner->bRayTracingIndirectEnabled ||
		Pipeline == VK_NULL_HANDLE ||
		RaygenRegion.deviceAddress == 0 ||
		RaygenRegion.size == 0)
	{
		return false;
	}

	outTemplate.RayGenerationStartAddress = RaygenRegion.deviceAddress;
	outTemplate.RayGenerationSizeInBytes = RaygenRegion.size;
	outTemplate.MissStartAddress = MissRegion.deviceAddress;
	outTemplate.MissSizeInBytes = MissRegion.size;
	outTemplate.MissStrideInBytes = MissRegion.stride;
	outTemplate.HitGroupStartAddress = HitRegion.deviceAddress;
	outTemplate.HitGroupSizeInBytes = HitRegion.size;
	outTemplate.HitGroupStrideInBytes = HitRegion.stride;
	outTemplate.CallableStartAddress = CallableRegion.deviceAddress;
	outTemplate.CallableSizeInBytes = CallableRegion.size;
	outTemplate.CallableStrideInBytes = CallableRegion.stride;
	outTemplate.Width = width;
	outTemplate.Height = height;
	outTemplate.Depth = 1;
	return true;
}

bool VulkanRTPipelineStateObject::ApplyIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset)
{
	if (!Owner ||
		Owner->ActiveCommandBuffer == VK_NULL_HANDLE ||
		!Owner->bRayTracingIndirectEnabled ||
		!Owner->vkCmdTraceRaysIndirectKHRFn ||
		!indirectArgumentBuffer)
	{
		return false;
	}

	auto bufferIt = Owner->BufferAllocations.find(indirectArgumentBuffer);
	if (bufferIt == Owner->BufferAllocations.end() || bufferIt->second.Buffer == VK_NULL_HANDLE)
		return false;

	const VkDeviceAddress bufferAddress = Owner->GetBufferDeviceAddress(bufferIt->second.Buffer);
	if (bufferAddress == 0)
		return false;

	Apply(0, 0);
	constexpr VkDeviceSize kD3D12DispatchRaysDimensionsOffset = 88;
	const VkDeviceAddress indirectAddress =
		bufferAddress +
		bufferIt->second.Offset +
		static_cast<VkDeviceSize>(byteOffset) +
		kD3D12DispatchRaysDimensionsOffset;
	Owner->vkCmdTraceRaysIndirectKHRFn(
		Owner->ActiveCommandBuffer,
		&RaygenRegion,
		&MissRegion,
		&HitRegion,
		&CallableRegion,
		indirectAddress);
	return true;
}
#endif

#if CORONA_HAS_VULKAN
void VulkanComputePipelineStateObject::ReleaseTempUniformBuffers()
{
	if (!Owner || Owner->Device == VK_NULL_HANDLE)
		return;
	for (VkBuffer buffer : TempUniformBuffers)
	{
		if (buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Owner->Device, buffer, nullptr);
	}
	for (VkDeviceMemory memory : TempUniformMemories)
	{
		if (memory != VK_NULL_HANDLE)
			vkFreeMemory(Owner->Device, memory, nullptr);
	}
	TempUniformBuffers.clear();
	TempUniformMemories.clear();
}

void VulkanComputePipelineStateObject::Release()
{
	if (!Owner || Owner->Device == VK_NULL_HANDLE)
		return;

	ReleaseTempUniformBuffers();
	if (Pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(Owner->Device, Pipeline, nullptr);
	if (ShaderModule != VK_NULL_HANDLE)
		vkDestroyShaderModule(Owner->Device, ShaderModule, nullptr);
	if (DescriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(Owner->Device, DescriptorPool, nullptr);
	if (DescriptorSetLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(Owner->Device, DescriptorSetLayout, nullptr);
	if (PipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(Owner->Device, PipelineLayout, nullptr);

	BindingValues.clear();
	SRVBindings.clear();
	UAVBindings.clear();
	SamplerBindings.clear();
	CBVBindings.clear();
	ShaderModule = VK_NULL_HANDLE;
	DescriptorPool = VK_NULL_HANDLE;
	DescriptorSet = VK_NULL_HANDLE;
	DescriptorSetLayout = VK_NULL_HANDLE;
	PipelineLayout = VK_NULL_HANDLE;
	Pipeline = VK_NULL_HANDLE;
	Owner = nullptr;
}

VulkanComputePipelineStateObject::~VulkanComputePipelineStateObject()
{
	Release();
}

void VulkanComputePipelineStateObject::BindSRV(const std::string& name, uint32_t baseRegister, uint32_t numDescriptors)
{
	SRVBindings.push_back(MakeComputeBindingDesc(name, baseRegister, ToVulkanTextureBinding(baseRegister), numDescriptors, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE));
	SRVBindings.back().Schema = MakeLegacyVulkanRHIBindingDesc(name, RHIDescriptorKind::SRV, RHIResourceKind::Unknown, baseRegister, numDescriptors);
}

void VulkanComputePipelineStateObject::BindSRV(const RHIBindingDesc& binding)
{
	BindSRV(binding.Name, binding.RegisterIndex, RHILegacyDescriptorCount(binding));
	SRVBindings.back().Schema = binding;
	SRVBindings.back().DescriptorType = ToVulkanDescriptorType(binding, SRVBindings.back().DescriptorType);
}

void VulkanComputePipelineStateObject::BindUAV(const std::string& name, uint32_t baseRegister)
{
	UAVBindings.push_back(MakeComputeBindingDesc(name, baseRegister, ToVulkanUavBinding(baseRegister), 1, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));
	UAVBindings.back().Schema = MakeLegacyVulkanRHIBindingDesc(name, RHIDescriptorKind::UAV, RHIResourceKind::Unknown, baseRegister, 1);
}

void VulkanComputePipelineStateObject::BindUAV(const RHIBindingDesc& binding)
{
	BindUAV(binding.Name, binding.RegisterIndex);
	UAVBindings.back().Schema = binding;
	UAVBindings.back().DescriptorType = ToVulkanDescriptorType(binding, UAVBindings.back().DescriptorType);
}

void VulkanComputePipelineStateObject::BindCBV(const std::string& name, uint32_t baseRegister, uint32_t size)
{
	CBVBindings.push_back(MakeComputeBindingDesc(name, baseRegister, ToVulkanConstantBufferBinding(baseRegister), 1, size, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
	CBVBindings.back().Schema = MakeLegacyVulkanRHIBindingDesc(name, RHIDescriptorKind::CBV, RHIResourceKind::ConstantBuffer, baseRegister, 1, size);
}

void VulkanComputePipelineStateObject::BindCBV(const RHIBindingDesc& binding)
{
	BindCBV(binding.Name, binding.RegisterIndex, binding.SizeInBytes);
	CBVBindings.back().Schema = binding;
	CBVBindings.back().DescriptorType = ToVulkanDescriptorType(binding, CBVBindings.back().DescriptorType);
}

void VulkanComputePipelineStateObject::BindSampler(const std::string& name, uint32_t baseRegister)
{
	SamplerBindings.push_back(MakeComputeBindingDesc(name, baseRegister, ToVulkanSamplerBinding(baseRegister), 1, 0, VK_DESCRIPTOR_TYPE_SAMPLER));
	SamplerBindings.back().Schema = MakeLegacyVulkanRHIBindingDesc(name, RHIDescriptorKind::Sampler, RHIResourceKind::Sampler, baseRegister, 1);
}

void VulkanComputePipelineStateObject::BindSampler(const RHIBindingDesc& binding)
{
	BindSampler(binding.Name, binding.RegisterIndex);
	SamplerBindings.back().Schema = binding;
	SamplerBindings.back().DescriptorType = ToVulkanDescriptorType(binding, SamplerBindings.back().DescriptorType);
}

bool VulkanComputePipelineStateObject::InitCS(const std::wstring& shaderFile, const std::string& entryPoint)
{
	ShaderFile = shaderFile;
	EntryPoint = entryPoint;
	if (!Owner || Owner->Device == VK_NULL_HANDLE)
		return false;

	std::filesystem::path shaderPath = NormalizeShaderPath(shaderFile);
	const std::wstring shaderStem = shaderPath.stem().wstring();
	if (shaderPath.is_relative())
		shaderPath = RuntimePaths::SourceDirectory() / shaderPath;
	const std::unordered_map<std::string, VkDescriptorType> descriptorTypesByName = ParseHlslDescriptorTypes(shaderPath);
	auto applyDescriptorInference = [&](std::vector<BindingDesc>& bindings)
	{
		for (BindingDesc& binding : bindings)
		{
			const auto descriptorTypeIt = descriptorTypesByName.find(binding.Name);
			if (descriptorTypeIt != descriptorTypesByName.end())
				binding.DescriptorType = descriptorTypeIt->second;
		}
	};
	applyDescriptorInference(SRVBindings);
	applyDescriptorInference(UAVBindings);
	applyDescriptorInference(SamplerBindings);
	applyDescriptorInference(CBVBindings);

	const std::filesystem::path spirvPath = ResolveVulkanComputeSpirvPath(shaderStem, entryPoint);
	if (!std::filesystem::exists(spirvPath))
	{
		Owner->ErrorString += "Missing Vulkan compute SPIR-V module: " + spirvPath.string() + "\n";
		return false;
	}

	const std::vector<uint32_t> shaderSpirv = LoadSpirvFile(spirvPath);
	VkShaderModuleCreateInfo shaderModuleInfo{};
	shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderModuleInfo.codeSize = shaderSpirv.size() * sizeof(uint32_t);
	shaderModuleInfo.pCode = shaderSpirv.data();
	if (vkCreateShaderModule(Owner->Device, &shaderModuleInfo, nullptr, &ShaderModule) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan compute shader module.");

	std::vector<VkDescriptorSetLayoutBinding> descriptorBindings;
	std::vector<VkDescriptorPoolSize> poolSizes;
	auto appendPoolSize = [&](VkDescriptorType type, uint32_t count)
	{
		for (VkDescriptorPoolSize& poolSize : poolSizes)
		{
			if (poolSize.type == type)
			{
				poolSize.descriptorCount += count;
				return;
			}
		}
		VkDescriptorPoolSize poolSize{};
		poolSize.type = type;
		poolSize.descriptorCount = count;
		poolSizes.push_back(poolSize);
	};
	auto appendBinding = [&](const BindingDesc& binding, VkShaderStageFlags stageFlags)
	{
		VkDescriptorSetLayoutBinding descriptorBinding{};
		descriptorBinding.binding = binding.DescriptorBinding;
		descriptorBinding.descriptorType = binding.DescriptorType;
		descriptorBinding.descriptorCount = binding.DescriptorCount;
		descriptorBinding.stageFlags = stageFlags;
		descriptorBindings.push_back(descriptorBinding);
		appendPoolSize(binding.DescriptorType, binding.DescriptorCount * kVulkanComputeDescriptorSetsPerPool);
	};

	for (const BindingDesc& binding : SRVBindings)
		appendBinding(binding, VK_SHADER_STAGE_COMPUTE_BIT);
	for (const BindingDesc& binding : UAVBindings)
		appendBinding(binding, VK_SHADER_STAGE_COMPUTE_BIT);
	for (const BindingDesc& binding : SamplerBindings)
		appendBinding(binding, VK_SHADER_STAGE_COMPUTE_BIT);
	for (const BindingDesc& binding : CBVBindings)
		appendBinding(binding, VK_SHADER_STAGE_COMPUTE_BIT);

	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
	descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptorSetLayoutInfo.bindingCount = static_cast<uint32_t>(descriptorBindings.size());
	descriptorSetLayoutInfo.pBindings = descriptorBindings.empty() ? nullptr : descriptorBindings.data();
	if (vkCreateDescriptorSetLayout(Owner->Device, &descriptorSetLayoutInfo, nullptr, &DescriptorSetLayout) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan compute descriptor set layout.");

	VkDescriptorPoolCreateInfo descriptorPoolInfo{};
	descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	descriptorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	descriptorPoolInfo.pPoolSizes = poolSizes.empty() ? nullptr : poolSizes.data();
	descriptorPoolInfo.maxSets = kVulkanComputeDescriptorSetsPerPool;
	if (vkCreateDescriptorPool(Owner->Device, &descriptorPoolInfo, nullptr, &DescriptorPool) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan compute descriptor pool.");

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &DescriptorSetLayout;
	if (vkCreatePipelineLayout(Owner->Device, &pipelineLayoutInfo, nullptr, &PipelineLayout) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan compute pipeline layout.");

	VkPipelineShaderStageCreateInfo shaderStageInfo{};
	shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	shaderStageInfo.module = ShaderModule;
	shaderStageInfo.pName = EntryPoint.c_str();

	VkComputePipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = shaderStageInfo;
	pipelineInfo.layout = PipelineLayout;
	if (vkCreateComputePipelines(Owner->Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &Pipeline) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan compute pipeline.");

	return Pipeline != VK_NULL_HANDLE;
}

void VulkanComputePipelineStateObject::Apply()
{
	if (!Owner || Owner->ActiveCommandBuffer == VK_NULL_HANDLE || Pipeline == VK_NULL_HANDLE || DescriptorSetLayout == VK_NULL_HANDLE)
		return;

	VkDescriptorSet activeDescriptorSet = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo descriptorSetAllocInfo{};
	descriptorSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorSetAllocInfo.descriptorPool = DescriptorPool;
	descriptorSetAllocInfo.descriptorSetCount = 1;
	descriptorSetAllocInfo.pSetLayouts = &DescriptorSetLayout;
	if (vkAllocateDescriptorSets(Owner->Device, &descriptorSetAllocInfo, &activeDescriptorSet) != VK_SUCCESS)
		return;
	Owner->TrackFrameDescriptorSet(DescriptorPool, activeDescriptorSet);
	DescriptorSet = activeDescriptorSet;

	std::vector<VkWriteDescriptorSet> writes;
	std::vector<VkDescriptorImageInfo> imageInfos;
	std::vector<VkDescriptorBufferInfo> bufferInfos;
	writes.reserve(SRVBindings.size() + UAVBindings.size() + SamplerBindings.size() + CBVBindings.size());
	imageInfos.reserve(SRVBindings.size() + UAVBindings.size() + SamplerBindings.size());
	bufferInfos.reserve(SRVBindings.size() + UAVBindings.size() + CBVBindings.size());

	auto appendImageWrite = [&](uint32_t binding, VkDescriptorType type, VkImageView imageView, VkImageLayout imageLayout, VkSampler samplerHandle)
	{
		VkDescriptorImageInfo& imageInfo = imageInfos.emplace_back();
		imageInfo.imageView = imageView;
		imageInfo.imageLayout = imageLayout;
		imageInfo.sampler = samplerHandle;

		VkWriteDescriptorSet& write = writes.emplace_back();
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = activeDescriptorSet;
		write.dstBinding = binding;
		write.descriptorCount = 1;
		write.descriptorType = type;
		write.pImageInfo = &imageInfo;
	};

	auto appendBufferWrite = [&](uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
	{
		VkDescriptorBufferInfo& bufferInfo = bufferInfos.emplace_back();
		bufferInfo.buffer = buffer;
		bufferInfo.offset = offset;
		bufferInfo.range = size;

		VkWriteDescriptorSet& write = writes.emplace_back();
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = activeDescriptorSet;
		write.dstBinding = binding;
		write.descriptorCount = 1;
		write.descriptorType = type;
		write.pBufferInfo = &bufferInfo;
	};

	for (const BindingDesc& binding : SRVBindings)
	{
		auto valueIt = BindingValues.find(binding.Name);
		if (valueIt == BindingValues.end())
			continue;
		if (valueIt->second.TextureValue)
		{
			auto textureIt = Owner->TextureAllocations.find(valueIt->second.TextureValue);
			if (textureIt != Owner->TextureAllocations.end())
				appendImageWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureIt->second.ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_NULL_HANDLE);
		}
		else if (valueIt->second.BufferValue)
		{
			auto bufferIt = Owner->BufferAllocations.find(valueIt->second.BufferValue);
			if (bufferIt != Owner->BufferAllocations.end())
				appendBufferWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferIt->second.Buffer, bufferIt->second.Offset, bufferIt->second.SizeInBytes);
		}
		else if (valueIt->second.VertexBufferValue)
		{
			auto bufferIt = Owner->VertexBufferAllocations.find(valueIt->second.VertexBufferValue);
			if (bufferIt != Owner->VertexBufferAllocations.end())
				appendBufferWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferIt->second.Buffer, bufferIt->second.Offset, bufferIt->second.SizeInBytes);
		}
	}

	for (const BindingDesc& binding : UAVBindings)
	{
		auto valueIt = BindingValues.find(binding.Name);
		if (valueIt == BindingValues.end())
			continue;
		if (valueIt->second.TextureValue)
		{
			auto textureIt = Owner->TextureAllocations.find(valueIt->second.TextureValue);
			if (textureIt != Owner->TextureAllocations.end())
				appendImageWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, textureIt->second.ImageView, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
		}
		else if (valueIt->second.BufferValue)
		{
			auto bufferIt = Owner->BufferAllocations.find(valueIt->second.BufferValue);
			if (bufferIt != Owner->BufferAllocations.end())
				appendBufferWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferIt->second.Buffer, bufferIt->second.Offset, bufferIt->second.SizeInBytes);
		}
		else if (valueIt->second.VertexBufferValue)
		{
			auto bufferIt = Owner->VertexBufferAllocations.find(valueIt->second.VertexBufferValue);
			if (bufferIt != Owner->VertexBufferAllocations.end())
				appendBufferWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferIt->second.Buffer, bufferIt->second.Offset, bufferIt->second.SizeInBytes);
		}
	}

	for (const BindingDesc& binding : SamplerBindings)
	{
		auto valueIt = BindingValues.find(binding.Name);
		if (valueIt == BindingValues.end() || !valueIt->second.SamplerValue)
			continue;
		auto samplerIt = Owner->SamplerAllocations.find(valueIt->second.SamplerValue);
		if (samplerIt != Owner->SamplerAllocations.end())
			appendImageWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, samplerIt->second.SamplerHandle);
	}

	for (const BindingDesc& binding : CBVBindings)
	{
		auto valueIt = BindingValues.find(binding.Name);
		if (valueIt == BindingValues.end() || valueIt->second.ConstantData.empty())
			continue;

		VkBuffer uniformBuffer = VK_NULL_HANDLE;
		VkDeviceSize uniformOffset = 0;
		VkDeviceSize uniformRange = 0;
		if (!Owner->UploadTransientUniformData(
			valueIt->second.ConstantData.data(),
			valueIt->second.ConstantData.size(),
			uniformBuffer,
			uniformOffset,
			uniformRange,
			TempUniformBuffers,
			TempUniformMemories))
		{
			continue;
		}

		appendBufferWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniformBuffer, uniformOffset, uniformRange);
	}

	if (!writes.empty())
		vkUpdateDescriptorSets(Owner->Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

	vkCmdBindPipeline(Owner->ActiveCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, Pipeline);
	vkCmdBindDescriptorSets(Owner->ActiveCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout, 0, 1, &activeDescriptorSet, 0, nullptr);
}

void VulkanComputePipelineStateObject::SetTextureSRV(const std::string& name, Texture* texture)
{
	BindingValues[name].TextureValue = texture;
	BindingValues[name].BufferValue = nullptr;
	BindingValues[name].VertexBufferValue = nullptr;
}

void VulkanComputePipelineStateObject::SetTextureUAV(const std::string& name, Texture* texture)
{
	BindingValues[name].TextureValue = texture;
	BindingValues[name].BufferValue = nullptr;
	BindingValues[name].VertexBufferValue = nullptr;
}

void VulkanComputePipelineStateObject::SetBufferSRV(const std::string& name, Buffer* buffer)
{
	BindingValues[name].BufferValue = buffer;
	BindingValues[name].TextureValue = nullptr;
	BindingValues[name].VertexBufferValue = nullptr;
}

void VulkanComputePipelineStateObject::SetBufferUAV(const std::string& name, Buffer* buffer)
{
	BindingValues[name].BufferValue = buffer;
	BindingValues[name].TextureValue = nullptr;
	BindingValues[name].VertexBufferValue = nullptr;
}

void VulkanComputePipelineStateObject::SetVertexBufferUAV(const std::string& name, VertexBuffer* vertexBuffer)
{
	BindingValues[name].VertexBufferValue = vertexBuffer;
	BindingValues[name].TextureValue = nullptr;
	BindingValues[name].BufferValue = nullptr;
}

void VulkanComputePipelineStateObject::SetSampler(const std::string& name, Sampler* sampler)
{
	BindingValues[name].SamplerValue = sampler;
}

void VulkanComputePipelineStateObject::SetCBVValue(const std::string& name, void* pData)
{
	const auto bindingIt = std::find_if(CBVBindings.begin(), CBVBindings.end(), [&](const BindingDesc& desc)
	{
		return desc.Name == name;
	});
	if (bindingIt == CBVBindings.end() || !pData)
		return;

	uint32_t alignedSize = bindingIt->DataSize;
	if (alignedSize == 0)
		alignedSize = 256;
	alignedSize = (alignedSize + 255u) & ~255u;

	auto& binding = BindingValues[name];
	binding.ConstantData.resize(alignedSize);
	std::memset(binding.ConstantData.data(), 0, binding.ConstantData.size());
	std::memcpy(binding.ConstantData.data(), pData, (std::min)(binding.ConstantData.size(), static_cast<size_t>(bindingIt->DataSize)));
}
#endif

VulkanBackend::~VulkanBackend()
{
	DestroyWindowContext();
	if (TrackedResourceOwner)
		TrackedResourceOwner->Backend = nullptr;
}

[[noreturn]] void VulkanBackend::ThrowNotImplemented(const char* functionName) const
{
	throw std::runtime_error(BuildNotImplementedMessage(functionName));
}

#if CORONA_HAS_VULKAN
std::weak_ptr<VulkanBackend::VulkanTrackedResourceOwner> VulkanBackend::GetTrackedResourceOwner()
{
	if (!TrackedResourceOwner)
		TrackedResourceOwner = std::make_shared<VulkanTrackedResourceOwner>();
	TrackedResourceOwner->Backend = this;
	return TrackedResourceOwner;
}

std::shared_ptr<Texture> VulkanBackend::CreateTrackedTextureHandle()
{
	const std::weak_ptr<VulkanTrackedResourceOwner> owner = GetTrackedResourceOwner();
	return std::shared_ptr<Texture>(new Texture(), [owner](Texture* texture)
	{
		if (std::shared_ptr<VulkanTrackedResourceOwner> lockedOwner = owner.lock())
		{
			if (lockedOwner->Backend)
				lockedOwner->Backend->ReleaseTextureAllocation(texture);
		}
		delete texture;
	});
}

std::shared_ptr<Buffer> VulkanBackend::CreateTrackedBufferHandle()
{
	const std::weak_ptr<VulkanTrackedResourceOwner> owner = GetTrackedResourceOwner();
	return std::shared_ptr<Buffer>(new Buffer(), [owner](Buffer* buffer)
	{
		if (std::shared_ptr<VulkanTrackedResourceOwner> lockedOwner = owner.lock())
		{
			if (lockedOwner->Backend)
				lockedOwner->Backend->ReleaseBufferAllocation(buffer);
		}
		delete buffer;
	});
}

std::shared_ptr<VertexBuffer> VulkanBackend::CreateTrackedVertexBufferHandle()
{
	const std::weak_ptr<VulkanTrackedResourceOwner> owner = GetTrackedResourceOwner();
	return std::shared_ptr<VertexBuffer>(new VertexBuffer(), [owner](VertexBuffer* vertexBuffer)
	{
		if (std::shared_ptr<VulkanTrackedResourceOwner> lockedOwner = owner.lock())
		{
			if (lockedOwner->Backend)
				lockedOwner->Backend->ReleaseVertexBufferAllocation(vertexBuffer);
		}
		delete vertexBuffer;
	});
}

std::shared_ptr<IndexBuffer> VulkanBackend::CreateTrackedIndexBufferHandle()
{
	const std::weak_ptr<VulkanTrackedResourceOwner> owner = GetTrackedResourceOwner();
	return std::shared_ptr<IndexBuffer>(new IndexBuffer(), [owner](IndexBuffer* indexBuffer)
	{
		if (std::shared_ptr<VulkanTrackedResourceOwner> lockedOwner = owner.lock())
		{
			if (lockedOwner->Backend)
				lockedOwner->Backend->ReleaseIndexBufferAllocation(indexBuffer);
		}
		delete indexBuffer;
	});
}

std::shared_ptr<Sampler> VulkanBackend::CreateTrackedSamplerHandle()
{
	const std::weak_ptr<VulkanTrackedResourceOwner> owner = GetTrackedResourceOwner();
	return std::shared_ptr<Sampler>(new Sampler(), [owner](Sampler* sampler)
	{
		if (std::shared_ptr<VulkanTrackedResourceOwner> lockedOwner = owner.lock())
		{
			if (lockedOwner->Backend)
				lockedOwner->Backend->ReleaseSamplerAllocation(sampler);
		}
		delete sampler;
	});
}

void VulkanBackend::ReleaseTextureAllocation(Texture* texture)
{
	if (!texture)
		return;
	UnregisterBindlessTexture(texture);

	auto it = TextureAllocations.find(texture);
	if (it == TextureAllocations.end())
		return;

	if (Device != VK_NULL_HANDLE)
	{
		if (it->second.bOwnsImageView && it->second.ImageView != VK_NULL_HANDLE)
			vkDestroyImageView(Device, it->second.ImageView, nullptr);
		if (it->second.bOwnsImage && it->second.Image != VK_NULL_HANDLE)
			vkDestroyImage(Device, it->second.Image, nullptr);
		if (it->second.bOwnsMemory && it->second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, it->second.Memory, nullptr);
	}
	TextureAllocations.erase(it);
}

void VulkanBackend::ReleaseBufferAllocation(Buffer* buffer)
{
	if (!buffer)
		return;
	UnregisterBindlessBuffer(buffer);

	auto it = BufferAllocations.find(buffer);
	if (it == BufferAllocations.end())
		return;

	const VulkanBufferAllocation allocation = it->second;
	if (allocation.PersistentPoolBlock)
	{
		ReleasePersistentStructuredBufferRange(allocation);
	}
	else if (!allocation.PoolBlock && Device != VK_NULL_HANDLE)
	{
		if (allocation.Buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, allocation.Buffer, nullptr);
		if (allocation.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, allocation.Memory, nullptr);
	}
	BufferAllocations.erase(it);
}

void VulkanBackend::ReleaseVertexBufferAllocation(VertexBuffer* vertexBuffer)
{
	if (!vertexBuffer)
		return;
	UnregisterBindlessVertexBuffer(vertexBuffer);

	auto it = VertexBufferAllocations.find(vertexBuffer);
	if (it == VertexBufferAllocations.end())
		return;

	if (!it->second.PoolBlock && Device != VK_NULL_HANDLE)
	{
		if (it->second.Buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, it->second.Buffer, nullptr);
		if (it->second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, it->second.Memory, nullptr);
	}
	VertexBufferAllocations.erase(it);
}

void VulkanBackend::ReleaseIndexBufferAllocation(IndexBuffer* indexBuffer)
{
	if (!indexBuffer)
		return;
	UnregisterBindlessIndexBuffer(indexBuffer);

	auto it = IndexBufferAllocations.find(indexBuffer);
	if (it == IndexBufferAllocations.end())
		return;

	if (!it->second.PoolBlock && Device != VK_NULL_HANDLE)
	{
		if (it->second.Buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, it->second.Buffer, nullptr);
		if (it->second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, it->second.Memory, nullptr);
	}
	IndexBufferAllocations.erase(it);
}

void VulkanBackend::ReleaseSamplerAllocation(Sampler* sampler)
{
	if (!sampler)
		return;

	auto it = SamplerAllocations.find(sampler);
	if (it == SamplerAllocations.end())
		return;

	if (Device != VK_NULL_HANDLE && it->second.SamplerHandle != VK_NULL_HANDLE)
		vkDestroySampler(Device, it->second.SamplerHandle, nullptr);
	SamplerAllocations.erase(it);
}

bool VulkanBackend::CreateBufferWithMemory(
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags properties,
	bool bEnableDeviceAddress,
	VkBuffer& outBuffer,
	VkDeviceMemory& outMemory)
{
	outBuffer = VK_NULL_HANDLE;
	outMemory = VK_NULL_HANDLE;

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(Device, &bufferInfo, nullptr, &outBuffer) != VK_SUCCESS)
		return false;

	VkMemoryRequirements memoryRequirements{};
	vkGetBufferMemoryRequirements(Device, outBuffer, &memoryRequirements);

	VkMemoryAllocateFlagsInfo allocateFlagsInfo{};
	allocateFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	allocateFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

	VkMemoryAllocateInfo allocateInfo{};
	allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocateInfo.allocationSize = memoryRequirements.size;
	allocateInfo.memoryTypeIndex = FindMemoryTypeIndex(PhysicalDevice, memoryRequirements.memoryTypeBits, properties);
	if (bEnableDeviceAddress)
		allocateInfo.pNext = &allocateFlagsInfo;

	if (vkAllocateMemory(Device, &allocateInfo, nullptr, &outMemory) != VK_SUCCESS)
	{
		vkDestroyBuffer(Device, outBuffer, nullptr);
		outBuffer = VK_NULL_HANDLE;
		return false;
	}

	if (vkBindBufferMemory(Device, outBuffer, outMemory, 0) != VK_SUCCESS)
	{
		vkFreeMemory(Device, outMemory, nullptr);
		vkDestroyBuffer(Device, outBuffer, nullptr);
		outBuffer = VK_NULL_HANDLE;
		outMemory = VK_NULL_HANDLE;
		return false;
	}

	return true;
}

bool VulkanBackend::CreateDeviceLocalBufferWithUpload(
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	bool bEnableDeviceAddress,
	const void* srcData,
	VkBuffer& outBuffer,
	VkDeviceMemory& outMemory)
{
	outBuffer = VK_NULL_HANDLE;
	outMemory = VK_NULL_HANDLE;
	if (size == 0)
		return false;

	if (!CreateBufferWithMemory(
		size,
		usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		bEnableDeviceAddress,
		outBuffer,
		outMemory))
	{
		return false;
	}

	if (!srcData)
		return true;

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	auto cleanupDestination = [&]()
	{
		if (outBuffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, outBuffer, nullptr);
		if (outMemory != VK_NULL_HANDLE)
			vkFreeMemory(Device, outMemory, nullptr);
		outBuffer = VK_NULL_HANDLE;
		outMemory = VK_NULL_HANDLE;
	};
	auto cleanupStaging = [&]()
	{
		if (stagingBuffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, stagingBuffer, nullptr);
		if (stagingMemory != VK_NULL_HANDLE)
			vkFreeMemory(Device, stagingMemory, nullptr);
		stagingBuffer = VK_NULL_HANDLE;
		stagingMemory = VK_NULL_HANDLE;
	};

	if (!CreateBufferWithMemory(
		size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		false,
		stagingBuffer,
		stagingMemory))
	{
		cleanupDestination();
		return false;
	}

	void* mappedData = nullptr;
	if (vkMapMemory(Device, stagingMemory, 0, size, 0, &mappedData) != VK_SUCCESS)
	{
		cleanupStaging();
		cleanupDestination();
		return false;
	}
	std::memcpy(mappedData, srcData, static_cast<size_t>(size));
	vkUnmapMemory(Device, stagingMemory);

	VkCommandBufferAllocateInfo commandBufferAllocInfo{};
	commandBufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocInfo.commandPool = CommandPool;
	commandBufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocInfo.commandBufferCount = 1;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(Device, &commandBufferAllocInfo, &commandBuffer) != VK_SUCCESS)
	{
		cleanupStaging();
		cleanupDestination();
		return false;
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		cleanupStaging();
		cleanupDestination();
		return false;
	}

	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, stagingBuffer, outBuffer, 1, &copyRegion);

	if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		cleanupStaging();
		cleanupDestination();
		return false;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		cleanupStaging();
		cleanupDestination();
		return false;
	}
	vkQueueWaitIdle(GraphicsQueue);

	vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
	cleanupStaging();
	return true;
}

VkDeviceAddress VulkanBackend::GetBufferDeviceAddress(VkBuffer buffer) const
{
	if (!bRayTracingEnabled || vkGetBufferDeviceAddressKHRFn == nullptr || buffer == VK_NULL_HANDLE)
		return 0;

	VkBufferDeviceAddressInfo addressInfo{};
	addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	addressInfo.buffer = buffer;
	return vkGetBufferDeviceAddressKHRFn(Device, &addressInfo);
}

bool VulkanBackend::InitializeTransientUniformBuffer(VkDeviceSize bytesPerFrame)
{
	if (Device == VK_NULL_HANDLE || bytesPerFrame == 0)
		return false;
	if (TransientUniformBuffer != VK_NULL_HANDLE)
		return true;

	TransientUniformBytesPerFrame = AlignVkDeviceSize(bytesPerFrame, UniformBufferAlignment);
	TransientUniformFrameCount = std::max<uint32_t>(GetFrameCount(), 3);
	const VkDeviceSize totalSize = TransientUniformBytesPerFrame * TransientUniformFrameCount;

	if (!CreateBufferWithMemory(
		totalSize,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		false,
		TransientUniformBuffer,
		TransientUniformMemory))
	{
		TransientUniformBuffer = VK_NULL_HANDLE;
		TransientUniformMemory = VK_NULL_HANDLE;
		return false;
	}

	if (vkMapMemory(Device, TransientUniformMemory, 0, totalSize, 0, &TransientUniformMapped) != VK_SUCCESS)
	{
		DestroyTransientUniformBuffer();
		return false;
	}

	TransientUniformFrameOffset = 0;
	AppendVulkanRuntimeTraceBackend(
		L"[VulkanBackend] transient uniform ring initialized bytesPerFrame=" +
		std::to_wstring(TransientUniformBytesPerFrame) +
		L", frameCount=" +
		std::to_wstring(TransientUniformFrameCount));
	return true;
}

void VulkanBackend::DestroyTransientUniformBuffer()
{
	if (Device == VK_NULL_HANDLE)
	{
		TransientUniformBuffer = VK_NULL_HANDLE;
		TransientUniformMemory = VK_NULL_HANDLE;
		TransientUniformMapped = nullptr;
		return;
	}

	if (TransientUniformMapped)
		vkUnmapMemory(Device, TransientUniformMemory);
	if (TransientUniformBuffer != VK_NULL_HANDLE)
		vkDestroyBuffer(Device, TransientUniformBuffer, nullptr);
	if (TransientUniformMemory != VK_NULL_HANDLE)
		vkFreeMemory(Device, TransientUniformMemory, nullptr);

	TransientUniformBuffer = VK_NULL_HANDLE;
	TransientUniformMemory = VK_NULL_HANDLE;
	TransientUniformMapped = nullptr;
	TransientUniformBytesPerFrame = 0;
	TransientUniformFrameOffset = 0;
	TransientUniformFrameCount = 0;
}

bool VulkanBackend::AllocateTransientUniform(VkDeviceSize size, VkBuffer& outBuffer, VkDeviceSize& outOffset, void** outMappedData)
{
	outBuffer = VK_NULL_HANDLE;
	outOffset = 0;
	if (outMappedData)
		*outMappedData = nullptr;
	if (size == 0)
		return false;
	if (MaxUniformBufferRange > 0 && size > MaxUniformBufferRange)
		return false;
	if (!InitializeTransientUniformBuffer(kVulkanTransientUniformBytesPerFrame))
		return false;

	const VkDeviceSize alignedSize = AlignVkDeviceSize(size, UniformBufferAlignment);
	if (TransientUniformFrameOffset + alignedSize > TransientUniformBytesPerFrame)
	{
		++TransientUniformOverflowCount;
		AppendVulkanRuntimeTraceBackend(
			L"[VulkanBackend] transient uniform ring overflow count=" +
			std::to_wstring(TransientUniformOverflowCount) +
			L", requested=" +
			std::to_wstring(size));
		return false;
	}

	const VkDeviceSize frameBase =
		static_cast<VkDeviceSize>(CurrentFrameIndex % std::max<uint32_t>(TransientUniformFrameCount, 1)) *
		TransientUniformBytesPerFrame;
	outBuffer = TransientUniformBuffer;
	outOffset = frameBase + TransientUniformFrameOffset;
	if (outMappedData)
		*outMappedData = static_cast<uint8_t*>(TransientUniformMapped) + outOffset;
	TransientUniformFrameOffset += alignedSize;
	return true;
}

bool VulkanBackend::UploadTransientUniformData(
	const void* data,
	VkDeviceSize size,
	VkBuffer& outBuffer,
	VkDeviceSize& outOffset,
	VkDeviceSize& outRange,
	std::vector<VkBuffer>& fallbackBuffers,
	std::vector<VkDeviceMemory>& fallbackMemories)
{
	outBuffer = VK_NULL_HANDLE;
	outOffset = 0;
	outRange = size;
	if (!data || size == 0)
		return false;

	void* mappedData = nullptr;
	if (AllocateTransientUniform(size, outBuffer, outOffset, &mappedData))
	{
		std::memcpy(mappedData, data, static_cast<size_t>(size));
		return true;
	}

	VkDeviceMemory uniformMemory = VK_NULL_HANDLE;
	if (!CreateBufferWithMemory(
		size,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		false,
		outBuffer,
		uniformMemory))
	{
		return false;
	}

	if (vkMapMemory(Device, uniformMemory, 0, size, 0, &mappedData) != VK_SUCCESS)
	{
		vkDestroyBuffer(Device, outBuffer, nullptr);
		vkFreeMemory(Device, uniformMemory, nullptr);
		outBuffer = VK_NULL_HANDLE;
		return false;
	}
	std::memcpy(mappedData, data, static_cast<size_t>(size));
	vkUnmapMemory(Device, uniformMemory);
	fallbackBuffers.push_back(outBuffer);
	fallbackMemories.push_back(uniformMemory);
	return true;
}

void VulkanBackend::DestroyFrameContexts()
{
	if (Device == VK_NULL_HANDLE)
	{
		FrameContexts.clear();
		ActiveFrameContextIndex = 0;
		NextFrameContextIndex = 0;
		ActiveCommandBuffer = VK_NULL_HANDLE;
		return;
	}

	for (VulkanFrameContext& frame : FrameContexts)
	{
		for (const auto& descriptorSet : frame.DescriptorSetsToFree)
		{
			if (descriptorSet.first != VK_NULL_HANDLE && descriptorSet.second != VK_NULL_HANDLE)
				vkFreeDescriptorSets(Device, descriptorSet.first, 1, &descriptorSet.second);
		}
		frame.DescriptorSetsToFree.clear();

		for (VkDescriptorPool descriptorPool : frame.GraphicsDescriptorPools)
		{
			if (descriptorPool != VK_NULL_HANDLE)
				vkDestroyDescriptorPool(Device, descriptorPool, nullptr);
		}
		frame.GraphicsDescriptorPools.clear();
		frame.ActiveGraphicsDescriptorPoolIndex = 0;

		for (VkFramebuffer framebuffer : frame.FramebuffersToDestroy)
		{
			if (framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(Device, framebuffer, nullptr);
		}
		frame.FramebuffersToDestroy.clear();

		if (frame.CommandBuffer != VK_NULL_HANDLE && CommandPool != VK_NULL_HANDLE)
			vkFreeCommandBuffers(Device, CommandPool, 1, &frame.CommandBuffer);
		if (frame.ImageAvailableSemaphore != VK_NULL_HANDLE)
			vkDestroySemaphore(Device, frame.ImageAvailableSemaphore, nullptr);
		if (frame.RenderFinishedSemaphore != VK_NULL_HANDLE)
			vkDestroySemaphore(Device, frame.RenderFinishedSemaphore, nullptr);
		if (frame.InFlightFence != VK_NULL_HANDLE)
			vkDestroyFence(Device, frame.InFlightFence, nullptr);
	}

	FrameContexts.clear();
	ActiveFrameContextIndex = 0;
	NextFrameContextIndex = 0;
	ActiveCommandBuffer = VK_NULL_HANDLE;
}

void VulkanBackend::TrackFrameDescriptorSet(VkDescriptorPool descriptorPool, VkDescriptorSet descriptorSet)
{
	if (descriptorPool == VK_NULL_HANDLE ||
		descriptorSet == VK_NULL_HANDLE ||
		ActiveFrameContextIndex >= FrameContexts.size())
	{
		return;
	}

	FrameContexts[ActiveFrameContextIndex].DescriptorSetsToFree.emplace_back(descriptorPool, descriptorSet);
}

VkDescriptorPool VulkanBackend::CreateGraphicsTransientDescriptorPool()
{
	if (Device == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	const uint32_t sampledImageDescriptors = kVulkanGraphicsDescriptorSetsPerPool * 8;
	const uint32_t storageBufferDescriptors = kVulkanGraphicsDescriptorSetsPerPool * 4;
	const uint32_t samplerDescriptors = kVulkanGraphicsDescriptorSetsPerPool * 8;

	VkDescriptorPoolSize poolSizes[] =
	{
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kVulkanGraphicsDescriptorSetsPerPool },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampledImageDescriptors },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kVulkanGraphicsDescriptorSetsPerPool },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storageBufferDescriptors },
		{ VK_DESCRIPTOR_TYPE_SAMPLER, samplerDescriptors },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampledImageDescriptors },
	};

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = kVulkanGraphicsDescriptorSetsPerPool;
	poolInfo.poolSizeCount = static_cast<uint32_t>(sizeof(poolSizes) / sizeof(poolSizes[0]));
	poolInfo.pPoolSizes = poolSizes;

	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
	if (vkCreateDescriptorPool(Device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return descriptorPool;
}

VkDescriptorSet VulkanBackend::AllocateGraphicsDescriptorSet(VkDescriptorSetLayout descriptorSetLayout)
{
	if (Device == VK_NULL_HANDLE ||
		descriptorSetLayout == VK_NULL_HANDLE ||
		ActiveFrameContextIndex >= FrameContexts.size())
	{
		return VK_NULL_HANDLE;
	}

	VulkanFrameContext& frame = FrameContexts[ActiveFrameContextIndex];
	while (frame.ActiveGraphicsDescriptorPoolIndex < kVulkanGraphicsDescriptorPoolLimitPerFrame)
	{
		if (frame.ActiveGraphicsDescriptorPoolIndex >= frame.GraphicsDescriptorPools.size())
		{
			VkDescriptorPool newPool = CreateGraphicsTransientDescriptorPool();
			if (newPool == VK_NULL_HANDLE)
			{
				static bool bLoggedCreateFailure = false;
				if (!bLoggedCreateFailure)
				{
					AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::AllocateGraphicsDescriptorSet] failed to create graphics descriptor pool page");
					bLoggedCreateFailure = true;
				}
				return VK_NULL_HANDLE;
			}
			frame.GraphicsDescriptorPools.push_back(newPool);
			if (frame.GraphicsDescriptorPools.size() > 1 &&
				(frame.GraphicsDescriptorPools.size() == 2 || (frame.GraphicsDescriptorPools.size() % 8) == 0))
			{
				AppendVulkanRuntimeTraceBackend(
					L"[VulkanBackend::AllocateGraphicsDescriptorSet] graphics descriptor pool pages=" +
					std::to_wstring(frame.GraphicsDescriptorPools.size()));
			}
		}

		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
		VkDescriptorSetAllocateInfo descriptorSetAllocInfo{};
		descriptorSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		descriptorSetAllocInfo.descriptorPool = frame.GraphicsDescriptorPools[frame.ActiveGraphicsDescriptorPoolIndex];
		descriptorSetAllocInfo.descriptorSetCount = 1;
		descriptorSetAllocInfo.pSetLayouts = &descriptorSetLayout;

		const VkResult allocateResult = vkAllocateDescriptorSets(Device, &descriptorSetAllocInfo, &descriptorSet);
		if (allocateResult == VK_SUCCESS)
			return descriptorSet;

		if (allocateResult == VK_ERROR_OUT_OF_POOL_MEMORY || allocateResult == VK_ERROR_FRAGMENTED_POOL)
		{
			++frame.ActiveGraphicsDescriptorPoolIndex;
			continue;
		}

		static bool bLoggedAllocateFailure = false;
		if (!bLoggedAllocateFailure)
		{
			AppendVulkanRuntimeTraceBackend(
				L"[VulkanBackend::AllocateGraphicsDescriptorSet] vkAllocateDescriptorSets failed result=" +
				std::to_wstring(static_cast<int>(allocateResult)));
			bLoggedAllocateFailure = true;
		}
		return VK_NULL_HANDLE;
	}

	static bool bLoggedPoolLimit = false;
	if (!bLoggedPoolLimit)
	{
		AppendVulkanRuntimeTraceBackend(
			L"[VulkanBackend::AllocateGraphicsDescriptorSet] graphics descriptor pool page limit reached, pages=" +
			std::to_wstring(kVulkanGraphicsDescriptorPoolLimitPerFrame) +
			L", setsPerPage=" +
			std::to_wstring(kVulkanGraphicsDescriptorSetsPerPool));
		bLoggedPoolLimit = true;
	}
	return VK_NULL_HANDLE;
}

bool VulkanBackend::InitializeBindlessDescriptorTables()
{
	if (Device == VK_NULL_HANDLE || !bDescriptorIndexingEnabled)
		return false;

	DestroyBindlessDescriptorTables();

	VkDescriptorSetLayoutCreateInfo emptyLayoutInfo{};
	emptyLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	if (vkCreateDescriptorSetLayout(Device, &emptyLayoutInfo, nullptr, &EmptyDescriptorSetLayout) != VK_SUCCESS)
		return false;

	auto createRuntimeArraySet = [&](
		VkDescriptorType descriptorType,
		uint32_t descriptorCount,
		VkShaderStageFlags stageFlags,
		VkDescriptorSetLayout& outLayout,
		VkDescriptorPool& outPool,
		VkDescriptorSet& outSet) -> bool
	{
		if (descriptorCount == 0)
			return false;

		VkDescriptorSetLayoutBinding layoutBinding{};
		layoutBinding.binding = 0;
		layoutBinding.descriptorType = descriptorType;
		layoutBinding.descriptorCount = descriptorCount;
		layoutBinding.stageFlags = stageFlags;

		const VkDescriptorBindingFlags bindingFlags =
			VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
			VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
		VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
		bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
		bindingFlagsInfo.bindingCount = 1;
		bindingFlagsInfo.pBindingFlags = &bindingFlags;

		VkDescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.pNext = &bindingFlagsInfo;
		layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
		layoutInfo.bindingCount = 1;
		layoutInfo.pBindings = &layoutBinding;
		if (vkCreateDescriptorSetLayout(Device, &layoutInfo, nullptr, &outLayout) != VK_SUCCESS)
			return false;

		VkDescriptorPoolSize poolSize{};
		poolSize.type = descriptorType;
		poolSize.descriptorCount = descriptorCount;

		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
		poolInfo.maxSets = 1;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes = &poolSize;
		if (vkCreateDescriptorPool(Device, &poolInfo, nullptr, &outPool) != VK_SUCCESS)
			return false;

		VkDescriptorSetAllocateInfo allocateInfo{};
		allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocateInfo.descriptorPool = outPool;
		allocateInfo.descriptorSetCount = 1;
		allocateInfo.pSetLayouts = &outLayout;
		if (vkAllocateDescriptorSets(Device, &allocateInfo, &outSet) != VK_SUCCESS)
			return false;

		return true;
	};

	const VkShaderStageFlags rtStageFlags =
		VK_SHADER_STAGE_RAYGEN_BIT_KHR |
		VK_SHADER_STAGE_MISS_BIT_KHR |
		VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
		VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

	bBindlessTextureTableReady = createRuntimeArraySet(
		VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		MaxVulkanBindlessTextureSlots,
		rtStageFlags,
		BindlessTextureDescriptorSetLayout,
		BindlessTextureDescriptorPool,
		BindlessTextureDescriptorSet);

	bBindlessBufferTableReady = createRuntimeArraySet(
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		MaxVulkanBindlessBufferSlots,
		rtStageFlags,
		BindlessBufferDescriptorSetLayout,
		BindlessBufferDescriptorPool,
		BindlessBufferDescriptorSet);

	AppendVulkanRuntimeTraceBackend(
		L"[VulkanBackend::InitializeBindlessDescriptorTables] textures=" +
		std::to_wstring(bBindlessTextureTableReady ? MaxVulkanBindlessTextureSlots : 0) +
		L", buffers=" +
		std::to_wstring(bBindlessBufferTableReady ? MaxVulkanBindlessBufferSlots : 0));
	return bBindlessTextureTableReady && bBindlessBufferTableReady;
}

void VulkanBackend::DestroyBindlessDescriptorTables()
{
	if (Device != VK_NULL_HANDLE)
	{
		if (BindlessTextureDescriptorPool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(Device, BindlessTextureDescriptorPool, nullptr);
		if (BindlessTextureDescriptorSetLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(Device, BindlessTextureDescriptorSetLayout, nullptr);
		if (BindlessBufferDescriptorPool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(Device, BindlessBufferDescriptorPool, nullptr);
		if (BindlessBufferDescriptorSetLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(Device, BindlessBufferDescriptorSetLayout, nullptr);
		if (EmptyDescriptorSetLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(Device, EmptyDescriptorSetLayout, nullptr);
	}

	BindlessTextureDescriptorPool = VK_NULL_HANDLE;
	BindlessTextureDescriptorSetLayout = VK_NULL_HANDLE;
	BindlessTextureDescriptorSet = VK_NULL_HANDLE;
	BindlessBufferDescriptorPool = VK_NULL_HANDLE;
	BindlessBufferDescriptorSetLayout = VK_NULL_HANDLE;
	BindlessBufferDescriptorSet = VK_NULL_HANDLE;
	EmptyDescriptorSetLayout = VK_NULL_HANDLE;
	bBindlessTextureTableReady = false;
	bBindlessBufferTableReady = false;

	{
		std::lock_guard<std::mutex> lock(BindlessTextureMutex);
		for (VulkanBindlessTextureSlot& slot : BindlessTextureSlots)
		{
			if (slot.TexturePtr)
				slot.TexturePtr->BindlessHandle = {};
			slot = {};
		}
		BindlessTextureSlots.clear();
		BindlessTextureFreeList.clear();
	}

	{
		std::lock_guard<std::mutex> lock(BindlessBufferMutex);
		for (VulkanBindlessBufferSlot& slot : BindlessBufferSlots)
		{
			if (slot.BufferPtr)
			{
				if (slot.ResourceType == 1)
					const_cast<Buffer*>(static_cast<const Buffer*>(slot.BufferPtr))->BindlessHandle = {};
				else if (slot.ResourceType == 2)
					const_cast<VertexBuffer*>(static_cast<const VertexBuffer*>(slot.BufferPtr))->BindlessHandle = {};
				else if (slot.ResourceType == 3)
					const_cast<IndexBuffer*>(static_cast<const IndexBuffer*>(slot.BufferPtr))->BindlessHandle = {};
			}
			slot = {};
		}
		BindlessBufferSlots.clear();
		BindlessBufferFreeList.clear();
	}
}
#endif

void VulkanBackend::BeginFrame()
{
#if !CORONA_HAS_VULKAN
	ThrowNotImplemented(__FUNCTION__);
#else
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::BeginFrame] begin");
	if (Swapchain == VK_NULL_HANDLE)
		throw std::runtime_error("Vulkan BeginFrame requires a swapchain.");
	if (FrameContexts.empty())
		throw std::runtime_error("Vulkan BeginFrame requires frame contexts.");
	if (bFrameActive)
		return;

	const uint32_t frameContextIndex = NextFrameContextIndex % static_cast<uint32_t>(FrameContexts.size());
	VulkanFrameContext& frame = FrameContexts[frameContextIndex];

	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::BeginFrame] before vkWaitForFences");
	const VkResult waitResult = vkWaitForFences(Device, 1, &frame.InFlightFence, VK_TRUE, UINT64_MAX);
	if (waitResult != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to wait for Vulkan in-flight fence.");
	}
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::BeginFrame] after vkWaitForFences");

	RetirePersistentStructuredBufferFrees(frameContextIndex);
	ResetTransientUploadStructuredFrame(frameContextIndex);

	for (const auto& descriptorSet : frame.DescriptorSetsToFree)
	{
		if (descriptorSet.first != VK_NULL_HANDLE && descriptorSet.second != VK_NULL_HANDLE)
			vkFreeDescriptorSets(Device, descriptorSet.first, 1, &descriptorSet.second);
	}
	frame.DescriptorSetsToFree.clear();

	for (VkDescriptorPool descriptorPool : frame.GraphicsDescriptorPools)
	{
		if (descriptorPool != VK_NULL_HANDLE)
		{
			const VkResult resetResult = vkResetDescriptorPool(Device, descriptorPool, 0);
			if (resetResult != VK_SUCCESS)
			{
				static bool bLoggedDescriptorPoolResetFailure = false;
				if (!bLoggedDescriptorPoolResetFailure)
				{
					AppendVulkanRuntimeTraceBackend(
						L"[VulkanBackend::BeginFrame] vkResetDescriptorPool failed result=" +
						std::to_wstring(static_cast<int>(resetResult)));
					bLoggedDescriptorPoolResetFailure = true;
				}
			}
		}
	}
	frame.ActiveGraphicsDescriptorPoolIndex = 0;

	for (VkFramebuffer framebuffer : frame.FramebuffersToDestroy)
	{
		if (framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(Device, framebuffer, nullptr);
	}
	frame.FramebuffersToDestroy.clear();

	uint32_t imageIndex = 0;
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::BeginFrame] before vkAcquireNextImageKHR");
	const VkResult acquireResult = vkAcquireNextImageKHR(Device, Swapchain, UINT64_MAX, frame.ImageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::BeginFrame] after vkAcquireNextImageKHR");
	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
	{
		RecreateSwapchain(SwapchainExtent.width, SwapchainExtent.height);
		return;
	}
	if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
		throw std::runtime_error("Failed to acquire Vulkan swapchain image.");

	if (vkResetFences(Device, 1, &frame.InFlightFence) != VK_SUCCESS)
		throw std::runtime_error("Failed to reset Vulkan in-flight fence.");
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::BeginFrame] after vkResetFences");

	CurrentFrameIndex = frameContextIndex;
	ActiveFrameContextIndex = frameContextIndex;
	ActiveSwapchainImageIndex = imageIndex;
	ActiveCommandBuffer = frame.CommandBuffer;
	bTimestampQueriesResetForCurrentFrame = false;
	bOcclusionQueriesResetForCurrentFrame = false;
	TransientUniformFrameOffset = 0;
	vkResetCommandBuffer(ActiveCommandBuffer, 0);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	if (vkBeginCommandBuffer(ActiveCommandBuffer, &beginInfo) != VK_SUCCESS)
		throw std::runtime_error("Failed to begin Vulkan command buffer.");

	if (OcclusionQueryPool != VK_NULL_HANDLE && OcclusionQueryCount > 0)
	{
		vkCmdResetQueryPool(ActiveCommandBuffer, OcclusionQueryPool, 0, OcclusionQueryCount);
		bOcclusionQueriesResetForCurrentFrame = true;
	}

	VkImageMemoryBarrier toColorAttachment{};
	toColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toColorAttachment.srcAccessMask = 0;
	toColorAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toColorAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toColorAttachment.image = SwapchainImages[imageIndex];
	toColorAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toColorAttachment.subresourceRange.levelCount = 1;
	toColorAttachment.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(ActiveCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toColorAttachment);
	if (ActiveSwapchainImageIndex < SwapchainWrappedTextures.size() && SwapchainWrappedTextures[ActiveSwapchainImageIndex])
	{
		auto wrappedIt = TextureAllocations.find(SwapchainWrappedTextures[ActiveSwapchainImageIndex].get());
		if (wrappedIt != TextureAllocations.end())
			wrappedIt->second.CurrentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	bFrameActive = true;
	bRenderPassActive = false;
	bViewportBound = false;
	BoundVertexBuffer = VK_NULL_HANDLE;
	BoundIndexBuffer = VK_NULL_HANDLE;
	PendingViewportWidth = 0;
	PendingViewportHeight = 0;
	NextFrameContextIndex = (frameContextIndex + 1) % static_cast<uint32_t>(FrameContexts.size());
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::BeginFrame] end");
#endif
}
#if CORONA_HAS_VULKAN
void VulkanBackend::LoadRayTracingFunctionPointers()
{
	vkCreateAccelerationStructureKHRFn = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(Device, "vkCreateAccelerationStructureKHR"));
	vkDestroyAccelerationStructureKHRFn = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(Device, "vkDestroyAccelerationStructureKHR"));
	vkGetAccelerationStructureBuildSizesKHRFn = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(Device, "vkGetAccelerationStructureBuildSizesKHR"));
	vkCmdBuildAccelerationStructuresKHRFn = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(Device, "vkCmdBuildAccelerationStructuresKHR"));
	vkGetAccelerationStructureDeviceAddressKHRFn = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(Device, "vkGetAccelerationStructureDeviceAddressKHR"));
	vkCreateRayTracingPipelinesKHRFn = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(vkGetDeviceProcAddr(Device, "vkCreateRayTracingPipelinesKHR"));
	vkGetRayTracingShaderGroupHandlesKHRFn = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(vkGetDeviceProcAddr(Device, "vkGetRayTracingShaderGroupHandlesKHR"));
	vkGetRayTracingShaderGroupStackSizeKHRFn = reinterpret_cast<PFN_vkGetRayTracingShaderGroupStackSizeKHR>(vkGetDeviceProcAddr(Device, "vkGetRayTracingShaderGroupStackSizeKHR"));
	vkCmdSetRayTracingPipelineStackSizeKHRFn = reinterpret_cast<PFN_vkCmdSetRayTracingPipelineStackSizeKHR>(vkGetDeviceProcAddr(Device, "vkCmdSetRayTracingPipelineStackSizeKHR"));
	vkCmdTraceRaysKHRFn = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkGetDeviceProcAddr(Device, "vkCmdTraceRaysKHR"));
	vkCmdTraceRaysIndirectKHRFn = reinterpret_cast<PFN_vkCmdTraceRaysIndirectKHR>(vkGetDeviceProcAddr(Device, "vkCmdTraceRaysIndirectKHR"));
	vkGetBufferDeviceAddressKHRFn = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(vkGetDeviceProcAddr(Device, "vkGetBufferDeviceAddressKHR"));
	bRayTracingIndirectEnabled = bRayTracingIndirectEnabled && vkCmdTraceRaysIndirectKHRFn != nullptr;

	bRayTracingEnabled =
		vkCreateAccelerationStructureKHRFn != nullptr &&
		vkDestroyAccelerationStructureKHRFn != nullptr &&
		vkGetAccelerationStructureBuildSizesKHRFn != nullptr &&
		vkCmdBuildAccelerationStructuresKHRFn != nullptr &&
		vkGetAccelerationStructureDeviceAddressKHRFn != nullptr &&
		vkCreateRayTracingPipelinesKHRFn != nullptr &&
		vkGetRayTracingShaderGroupHandlesKHRFn != nullptr &&
		vkGetRayTracingShaderGroupStackSizeKHRFn != nullptr &&
		vkCmdSetRayTracingPipelineStackSizeKHRFn != nullptr &&
		vkCmdTraceRaysKHRFn != nullptr &&
		vkGetBufferDeviceAddressKHRFn != nullptr;
}
#endif
void VulkanBackend::EndFrame()
{
#if !CORONA_HAS_VULKAN
	ThrowNotImplemented(__FUNCTION__);
#else
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::EndFrame] begin");
	if (!bFrameActive)
		return;
	if (ActiveFrameContextIndex >= FrameContexts.size())
		throw std::runtime_error("Vulkan EndFrame has no active frame context.");

	VulkanFrameContext& frame = FrameContexts[ActiveFrameContextIndex];

	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &frame.ImageAvailableSemaphore;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &ActiveCommandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &frame.RenderFinishedSemaphore;
	if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, frame.InFlightFence) != VK_SUCCESS)
		throw std::runtime_error("Failed to submit Vulkan frame.");

	if (!PendingCapturePath.empty())
	{
		if (vkWaitForFences(Device, 1, &frame.InFlightFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
			throw std::runtime_error("Failed to wait for Vulkan frame before capture.");
		LastCapturePath = PendingCapturePath;
		PendingCapturePath.clear();
		bLastCaptureSucceeded = CaptureCurrentSwapchainImageToPNG(ActiveSwapchainImageIndex, LastCapturePath, &LastCaptureError);
		bLastCaptureResultValid = true;
	}

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &frame.RenderFinishedSemaphore;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &Swapchain;
	presentInfo.pImageIndices = &ActiveSwapchainImageIndex;
	const VkResult presentResult = vkQueuePresentKHR(GraphicsQueue, &presentInfo);
	const bool bRecreateSwapchainAfterPresent =
		presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
		presentResult == VK_SUBOPTIMAL_KHR;
	if (presentResult == VK_SUBOPTIMAL_KHR)
	{
		static bool bLoggedSuboptimalPresent = false;
		if (!bLoggedSuboptimalPresent)
		{
			AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::EndFrame] present returned SUBOPTIMAL; recreating swapchain after frame cleanup.");
			bLoggedSuboptimalPresent = true;
		}
	}
	else if (presentResult != VK_SUCCESS && presentResult != VK_ERROR_OUT_OF_DATE_KHR)
	{
		throw std::runtime_error("Failed to present Vulkan frame.");
	}

	bFrameActive = false;
	bRenderPassActive = false;
	bViewportBound = false;
	ActiveGraphicsRenderPass = VK_NULL_HANDLE;
	ActiveColorAttachmentCount = 0;
	ActiveCommandBuffer = VK_NULL_HANDLE;
	frame.FramebuffersToDestroy.insert(
		frame.FramebuffersToDestroy.end(),
		TransientOffscreenFramebuffers.begin(),
		TransientOffscreenFramebuffers.end());
	TransientOffscreenFramebuffers.clear();
	ActiveOffscreenFramebuffer = VK_NULL_HANDLE;
	PendingOffscreenColorTargets.clear();
	PendingOffscreenDepthTarget = nullptr;
	PendingTextureClearColors.clear();
	PendingDepthClearValues.clear();
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::EndFrame] end");
	if (bRecreateSwapchainAfterPresent)
		RecreateSwapchain(SwapchainExtent.width, SwapchainExtent.height);
#endif
}
void VulkanBackend::WaitForGpu()
{
#if CORONA_HAS_VULKAN
	if (Device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(Device);
#endif
}
void VulkanBackend::EmitGpuCrashMarker(const char* markerName)
{
#if CORONA_HAS_VULKAN
	if (!markerName || !vkCmdInsertDebugUtilsLabelEXTFn || ActiveCommandBuffer == VK_NULL_HANDLE)
		return;

	VkDebugUtilsLabelEXT label{};
	label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
	label.pLabelName = markerName;
	label.color[0] = 0.20f;
	label.color[1] = 0.60f;
	label.color[2] = 1.00f;
	label.color[3] = 1.00f;
	vkCmdInsertDebugUtilsLabelEXTFn(ActiveCommandBuffer, &label);
#else
	(void)markerName;
#endif
}
const std::string& VulkanBackend::GetErrorString() const { return ErrorString; }
void VulkanBackend::ClearErrorString() { ErrorString.clear(); }
uint64_t VulkanBackend::GetTimestampFrequency() const
{
#if CORONA_HAS_VULKAN
	if (TimestampValidBits == 0 || TimestampPeriodNs <= 0.0f)
		return 0;
	return static_cast<uint64_t>(1000000000.0 / static_cast<double>(TimestampPeriodNs));
#else
	return 0;
#endif
}
uint32_t VulkanBackend::GetFrameCount() const
{
#if CORONA_HAS_VULKAN
	if (!FrameContexts.empty())
		return static_cast<uint32_t>(FrameContexts.size());
	return static_cast<uint32_t>(std::max<size_t>(SwapchainImages.size(), 1));
#else
	return 1;
#endif
}
uint32_t VulkanBackend::GetCurrentFrameIndex() const { return CurrentFrameIndex; }
bool VulkanBackend::SupportsRayTracing() const
{
#if CORONA_HAS_VULKAN
	return bRayTracingEnabled;
#else
	return false;
#endif
}
DX12Backend* VulkanBackend::AsDX12Backend() { return nullptr; }

bool VulkanBackend::GetStreamlineTextureResource(Texture* texture, EResourceState state, StreamlineTextureResourceDesc& outDesc) const
{
#if !CORONA_HAS_VULKAN
	(void)texture;
	(void)state;
	(void)outDesc;
	return false;
#else
	if (!texture)
		return false;
	auto textureIt = TextureAllocations.find(texture);
	if (textureIt == TextureAllocations.end() ||
		textureIt->second.Image == VK_NULL_HANDLE ||
		textureIt->second.ImageView == VK_NULL_HANDLE)
	{
		return false;
	}

	const VulkanTextureAllocation& allocation = textureIt->second;
	outDesc = {};
	outDesc.Native = reinterpret_cast<void*>(allocation.Image);
	outDesc.Memory = reinterpret_cast<void*>(allocation.Memory);
	outDesc.View = reinterpret_cast<void*>(allocation.ImageView);
	outDesc.State = static_cast<uint32_t>(
		allocation.CurrentLayout != VK_IMAGE_LAYOUT_UNDEFINED
			? allocation.CurrentLayout
			: ToVkImageLayout(state));
	outDesc.Width = allocation.Width;
	outDesc.Height = allocation.Height;
	outDesc.NativeFormat = static_cast<uint32_t>(allocation.Format);
	outDesc.MipLevels = texture->MipLevels;
	outDesc.ArrayLayers = 1;
	outDesc.Flags = 0;
	outDesc.Usage = static_cast<uint32_t>(GetImageUsageFlags(allocation.Usage));
	return outDesc.Native != nullptr;
#endif
}

void* VulkanBackend::GetStreamlineCommandBuffer()
{
#if !CORONA_HAS_VULKAN
	return nullptr;
#else
	return reinterpret_cast<void*>(ActiveCommandBuffer);
#endif
}

bool VulkanBackend::GetStreamlineVulkanDeviceInfo(StreamlineVulkanDeviceInfo& outInfo) const
{
#if !CORONA_HAS_VULKAN
	(void)outInfo;
	return false;
#else
	if (Device == VK_NULL_HANDLE ||
		Instance == VK_NULL_HANDLE ||
		PhysicalDevice == VK_NULL_HANDLE ||
		GraphicsQueueFamilyIndex == UINT32_MAX)
	{
		return false;
	}

	outInfo = {};
	outInfo.Device = reinterpret_cast<void*>(Device);
	outInfo.Instance = reinterpret_cast<void*>(Instance);
	outInfo.PhysicalDevice = reinterpret_cast<void*>(PhysicalDevice);
	VkPhysicalDeviceIDProperties idProperties{};
	idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
	VkPhysicalDeviceProperties2 properties2{};
	properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	properties2.pNext = &idProperties;
	vkGetPhysicalDeviceProperties2(PhysicalDevice, &properties2);
	if (idProperties.deviceLUIDValid == VK_TRUE)
	{
		static_assert(VK_LUID_SIZE <= std::tuple_size<decltype(outInfo.DeviceLUID)>::value);
		std::memcpy(outInfo.DeviceLUID.data(), idProperties.deviceLUID, VK_LUID_SIZE);
		outInfo.DeviceLUIDSizeInBytes = VK_LUID_SIZE;
	}
	outInfo.ComputeQueueIndex = 0;
	outInfo.ComputeQueueFamily = GraphicsQueueFamilyIndex;
	outInfo.GraphicsQueueIndex = 0;
	outInfo.GraphicsQueueFamily = GraphicsQueueFamilyIndex;
	outInfo.OpticalFlowQueueIndex = 0;
	outInfo.OpticalFlowQueueFamily = GraphicsQueueFamilyIndex;
	return true;
#endif
}

std::shared_ptr<Texture> VulkanBackend::CreateTexture2D(const TextureCreateDesc& desc)
{
#if !CORONA_HAS_VULKAN
	(void)desc;
	ThrowNotImplemented(__FUNCTION__);
#else
	auto texture = CreateTrackedTextureHandle();
	texture->Width = static_cast<uint32_t>(desc.Width);
	texture->Height = static_cast<uint32_t>(desc.Height);
	texture->MipLevels = static_cast<uint32_t>((std::max)(desc.MipLevels, 1));
	texture->Format = desc.Format;
	texture->Usage = desc.Usage;

	VulkanTextureAllocation allocation{};
	allocation.Format = ToVkFormat(desc.Format);
	allocation.Width = static_cast<uint32_t>(desc.Width);
	allocation.Height = static_cast<uint32_t>(desc.Height);
	allocation.Depth = 1;
	allocation.Usage = desc.Usage;

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = allocation.Format;
	imageInfo.extent.width = allocation.Width;
	imageInfo.extent.height = allocation.Height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = static_cast<uint32_t>((std::max)(desc.MipLevels, 1));
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = GetImageUsageFlags(desc.Usage);
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(Device, &imageInfo, nullptr, &allocation.Image) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan texture image.");

	VkMemoryRequirements memoryRequirements{};
	vkGetImageMemoryRequirements(Device, allocation.Image, &memoryRequirements);

	VkMemoryAllocateInfo allocateInfo{};
	allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocateInfo.allocationSize = memoryRequirements.size;
	allocateInfo.memoryTypeIndex = FindMemoryTypeIndex(
		PhysicalDevice,
		memoryRequirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(Device, &allocateInfo, nullptr, &allocation.Memory) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate Vulkan texture memory.");

	if (vkBindImageMemory(Device, allocation.Image, allocation.Memory, 0) != VK_SUCCESS)
		throw std::runtime_error("Failed to bind Vulkan texture memory.");

	VkImageViewCreateInfo imageViewInfo{};
	imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	imageViewInfo.image = allocation.Image;
	imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	imageViewInfo.format = allocation.Format;
	imageViewInfo.subresourceRange.aspectMask = GetImageAspectFlags(desc.Format);
	imageViewInfo.subresourceRange.levelCount = imageInfo.mipLevels;
	imageViewInfo.subresourceRange.layerCount = 1;
	if (vkCreateImageView(Device, &imageViewInfo, nullptr, &allocation.ImageView) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan texture view.");
	allocation.CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	if (desc.InitialState == EInitialResourceState::ShaderRead ||
		desc.InitialState == EInitialResourceState::GenericRead)
	{
		VkCommandBufferAllocateInfo commandBufferAllocInfo{};
		commandBufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandBufferAllocInfo.commandPool = CommandPool;
		commandBufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandBufferAllocInfo.commandBufferCount = 1;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(Device, &commandBufferAllocInfo, &commandBuffer) != VK_SUCCESS)
			throw std::runtime_error("Failed to allocate Vulkan texture initialization command buffer.");

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(commandBuffer, &beginInfo);

		TransitionImageLayout(
			commandBuffer,
			allocation.Image,
			GetImageAspectFlags(desc.Format),
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vkEndCommandBuffer(commandBuffer);

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
			throw std::runtime_error("Failed to submit Vulkan texture initialization.");
		vkQueueWaitIdle(GraphicsQueue);
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);

		allocation.CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	TextureAllocations[texture.get()] = allocation;
	return texture;
#endif
}
std::shared_ptr<Buffer> VulkanBackend::CreateBuffer(const BufferCreateDesc& desc)
{
#if !CORONA_HAS_VULKAN
	(void)desc;
	ThrowNotImplemented(__FUNCTION__);
#else
	if (desc.NumElements == 0 || desc.ElementSize == 0)
		return nullptr;

	if (desc.AllocationPolicy == EBufferAllocationPolicy::Suballocated &&
		desc.Lifetime == EBufferLifetime::Persistent &&
		desc.Shape == EBufferShape::Structured &&
		desc.Access == EBufferAccess::GpuOnly &&
		!desc.bAllowUnorderedAccess &&
		desc.InitialData)
	{
		return CreateSuballocatedStructuredBuffer(desc);
	}

	auto buffer = CreateTrackedBufferHandle();
	buffer->NumElements = desc.NumElements;
	buffer->ElementSize = desc.ElementSize;
	buffer->Type = Buffer::UNKNOWN;

	VulkanBufferAllocation allocation{};
	allocation.Stride = desc.ElementSize;
	allocation.SizeInBytes =
		static_cast<uint32_t>(static_cast<uint64_t>(desc.NumElements) * static_cast<uint64_t>(desc.ElementSize));

	VkBufferUsageFlags bufferUsage =
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
	if (bRayTracingEnabled)
	{
		bufferUsage |=
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	}

	const bool bUseDeviceLocalMemory =
		desc.bAllowUnorderedAccess ||
		desc.Access == EBufferAccess::GpuOnly;
	bool bCreatedBuffer = false;
	if (bUseDeviceLocalMemory)
	{
		bCreatedBuffer = CreateDeviceLocalBufferWithUpload(
			allocation.SizeInBytes,
			bufferUsage,
			bRayTracingEnabled,
			desc.InitialData,
			allocation.Buffer,
			allocation.Memory);
	}
	else
	{
		VkMemoryPropertyFlags memoryProperties =
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		bCreatedBuffer = CreateBufferWithMemory(
			allocation.SizeInBytes,
			bufferUsage,
			memoryProperties,
			bRayTracingEnabled,
			allocation.Buffer,
			allocation.Memory);
		if (bCreatedBuffer && desc.InitialData)
		{
			void* mappedData = nullptr;
			if (vkMapMemory(Device, allocation.Memory, 0, allocation.SizeInBytes, 0, &mappedData) != VK_SUCCESS)
			{
				vkDestroyBuffer(Device, allocation.Buffer, nullptr);
				vkFreeMemory(Device, allocation.Memory, nullptr);
				throw std::runtime_error("Failed to map Vulkan upload buffer.");
			}
			std::memcpy(mappedData, desc.InitialData, allocation.SizeInBytes);
			vkUnmapMemory(Device, allocation.Memory);
		}
	}
	if (!bCreatedBuffer)
	{
		throw std::runtime_error("Failed to create Vulkan buffer.");
	}

	BufferAllocations[buffer.get()] = allocation;
	return buffer;
#endif
}
std::shared_ptr<Sampler> VulkanBackend::CreateSampler(const SamplerCreateDesc& desc)
{
#if !CORONA_HAS_VULKAN
	(void)desc;
	ThrowNotImplemented(__FUNCTION__);
#else
	auto sampler = CreateTrackedSamplerHandle();

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = ToVkFilter(desc.Filter);
	samplerInfo.minFilter = ToVkFilter(desc.Filter);
	samplerInfo.addressModeU = ToVkAddressMode(desc.AddressU);
	samplerInfo.addressModeV = ToVkAddressMode(desc.AddressV);
	samplerInfo.addressModeW = ToVkAddressMode(desc.AddressW);
	samplerInfo.mipLodBias = desc.MipLODBias;
	const bool bUseAnisotropy = desc.Filter == ESamplerFilter::Anisotropic && bSamplerAnisotropySupported;
	samplerInfo.anisotropyEnable = bUseAnisotropy ? VK_TRUE : VK_FALSE;
	samplerInfo.maxAnisotropy = bUseAnisotropy ? static_cast<float>((std::max)(desc.MaxAnisotropy, 1u)) : 1.0f;
	samplerInfo.minLod = desc.MinLOD;
	samplerInfo.maxLod = desc.MaxLOD;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	if (vkCreateSampler(Device, &samplerInfo, nullptr, &SamplerAllocations[sampler.get()].SamplerHandle) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan sampler.");

	return sampler;
#endif
}
std::shared_ptr<Texture> VulkanBackend::CreateTextureFromFile(const std::wstring& fileName, bool nonSRGB)
{
#if !CORONA_HAS_VULKAN
	(void)fileName;
	(void)nonSRGB;
	ThrowNotImplemented(__FUNCTION__);
#else
	(void)nonSRGB;
	std::vector<uint8_t> pixels;
	uint32_t width = 0;
	uint32_t height = 0;
	std::wstring errorMessage;
	if (!LoadRGBA8TextureFromFile(fileName, pixels, width, height, &errorMessage))
	{
		std::string message = "Failed to load Vulkan texture file: " + std::filesystem::path(fileName).string();
		if (!errorMessage.empty())
			message += " (" + PlatformWideToUtf8(errorMessage) + ")";
		throw std::runtime_error(message);
	}

	TextureCreateDesc desc{};
	desc.Format = ETextureFormat::RGBA8Unorm;
	desc.Usage = TextureUsage_None;
	desc.InitialState = EInitialResourceState::CopyDest;
	desc.Width = static_cast<int>(width);
	desc.Height = static_cast<int>(height);
	desc.MipLevels = 1;
	auto texture = CreateTexture2D(desc);

	auto textureIt = TextureAllocations.find(texture.get());
	if (textureIt == TextureAllocations.end())
		throw std::runtime_error("Vulkan texture allocation missing.");

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = pixels.size();
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(Device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan staging buffer.");

	VkMemoryRequirements memoryRequirements{};
	vkGetBufferMemoryRequirements(Device, stagingBuffer, &memoryRequirements);
	VkMemoryAllocateInfo allocateInfo{};
	allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocateInfo.allocationSize = memoryRequirements.size;
	allocateInfo.memoryTypeIndex = FindMemoryTypeIndex(
		PhysicalDevice,
		memoryRequirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (vkAllocateMemory(Device, &allocateInfo, nullptr, &stagingMemory) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate Vulkan staging memory.");
	if (vkBindBufferMemory(Device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS)
		throw std::runtime_error("Failed to bind Vulkan staging memory.");

	void* mappedData = nullptr;
	if (vkMapMemory(Device, stagingMemory, 0, pixels.size(), 0, &mappedData) != VK_SUCCESS)
		throw std::runtime_error("Failed to map Vulkan staging memory.");
	std::memcpy(mappedData, pixels.data(), pixels.size());
	vkUnmapMemory(Device, stagingMemory);

	VkCommandBufferAllocateInfo commandBufferAllocInfo{};
	commandBufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocInfo.commandPool = CommandPool;
	commandBufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocInfo.commandBufferCount = 1;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(Device, &commandBufferAllocInfo, &commandBuffer) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate Vulkan upload command buffer.");

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	TransitionImageLayout(commandBuffer, textureIt->second.Image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkBufferImageCopy copyRegion{};
	copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegion.imageSubresource.layerCount = 1;
	copyRegion.imageExtent.width = width;
	copyRegion.imageExtent.height = height;
	copyRegion.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, textureIt->second.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

	TransitionImageLayout(commandBuffer, textureIt->second.Image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	textureIt->second.CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
		throw std::runtime_error("Failed to submit Vulkan texture upload.");
	vkQueueWaitIdle(GraphicsQueue);

	vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
	vkDestroyBuffer(Device, stagingBuffer, nullptr);
	vkFreeMemory(Device, stagingMemory, nullptr);

	return texture;
#endif
}

RHITextureHandle VulkanBackend::RegisterBindlessTexture(Texture* texture)
{
#if !CORONA_HAS_VULKAN
	(void)texture;
	return {};
#else
	if (!texture || !bBindlessTextureTableReady || BindlessTextureDescriptorSet == VK_NULL_HANDLE)
		return {};
	if (TextureAllocations.find(texture) == TextureAllocations.end())
		return {};

	{
		std::lock_guard<std::mutex> lock(BindlessTextureMutex);
		const RHITextureHandle existing = texture->BindlessHandle;
		if (existing.IsValid() &&
			existing.Index < BindlessTextureSlots.size() &&
			BindlessTextureSlots[existing.Index].Occupied &&
			BindlessTextureSlots[existing.Index].Generation == existing.Generation &&
			BindlessTextureSlots[existing.Index].TexturePtr == texture)
		{
			return existing;
		}

		uint32_t slotIndex = RHI_INVALID_BINDLESS_INDEX;
		if (!BindlessTextureFreeList.empty())
		{
			slotIndex = BindlessTextureFreeList.back();
			BindlessTextureFreeList.pop_back();
		}
		else
		{
			if (BindlessTextureSlots.size() >= MaxVulkanBindlessTextureSlots)
				return {};
			slotIndex = static_cast<uint32_t>(BindlessTextureSlots.size());
			BindlessTextureSlots.emplace_back();
		}

		VulkanBindlessTextureSlot& slot = BindlessTextureSlots[slotIndex];
		if (slot.Generation == 0)
			slot.Generation = 1;
		slot.TexturePtr = texture;
		slot.Occupied = true;
		texture->BindlessHandle = { slotIndex, slot.Generation };
	}

	if (!UpdateBindlessTexture(texture))
	{
		UnregisterBindlessTexture(texture);
		return {};
	}
	return texture->BindlessHandle;
#endif
}

bool VulkanBackend::UpdateBindlessTexture(Texture* texture)
{
#if !CORONA_HAS_VULKAN
	(void)texture;
	return false;
#else
	if (!texture || !bBindlessTextureTableReady || BindlessTextureDescriptorSet == VK_NULL_HANDLE)
		return false;

	auto textureIt = TextureAllocations.find(texture);
	if (textureIt == TextureAllocations.end() || textureIt->second.ImageView == VK_NULL_HANDLE)
		return false;

	std::lock_guard<std::mutex> lock(BindlessTextureMutex);
	const RHITextureHandle handle = texture->BindlessHandle;
	if (!handle.IsValid() ||
		handle.Index >= BindlessTextureSlots.size() ||
		!BindlessTextureSlots[handle.Index].Occupied ||
		BindlessTextureSlots[handle.Index].Generation != handle.Generation ||
		BindlessTextureSlots[handle.Index].TexturePtr != texture)
	{
		return false;
	}

	VkDescriptorImageInfo imageInfo{};
	imageInfo.imageView = textureIt->second.ImageView;
	imageInfo.imageLayout = textureIt->second.CurrentLayout == VK_IMAGE_LAYOUT_GENERAL
		? VK_IMAGE_LAYOUT_GENERAL
		: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = BindlessTextureDescriptorSet;
	write.dstBinding = 0;
	write.dstArrayElement = handle.Index;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	write.pImageInfo = &imageInfo;
	vkUpdateDescriptorSets(Device, 1, &write, 0, nullptr);
	return true;
#endif
}

void VulkanBackend::UnregisterBindlessTexture(Texture* texture)
{
#if !CORONA_HAS_VULKAN
	(void)texture;
#else
	if (!texture)
		return;

	std::lock_guard<std::mutex> lock(BindlessTextureMutex);
	const RHITextureHandle handle = texture->BindlessHandle;
	if (handle.IsValid() && handle.Index < BindlessTextureSlots.size())
	{
		VulkanBindlessTextureSlot& slot = BindlessTextureSlots[handle.Index];
		if (slot.Occupied &&
			slot.Generation == handle.Generation &&
			slot.TexturePtr == texture)
		{
			slot.TexturePtr = nullptr;
			slot.Occupied = false;
			++slot.Generation;
			if (slot.Generation == 0)
				slot.Generation = 1;
			BindlessTextureFreeList.push_back(handle.Index);
		}
	}
	texture->BindlessHandle = {};
#endif
}

RHITextureHandle VulkanBackend::GetBindlessTextureHandle(const Texture* texture) const
{
#if !CORONA_HAS_VULKAN
	(void)texture;
	return {};
#else
	if (!texture)
		return {};

	std::lock_guard<std::mutex> lock(BindlessTextureMutex);
	const RHITextureHandle handle = texture->BindlessHandle;
	if (!handle.IsValid() ||
		handle.Index >= BindlessTextureSlots.size() ||
		!BindlessTextureSlots[handle.Index].Occupied ||
		BindlessTextureSlots[handle.Index].Generation != handle.Generation ||
		BindlessTextureSlots[handle.Index].TexturePtr != texture)
	{
		return {};
	}
	return handle;
#endif
}

namespace
{
#if CORONA_HAS_VULKAN
	bool WriteVulkanBindlessBufferDescriptor(
		VkDevice device,
		VkDescriptorSet descriptorSet,
		uint32_t arrayIndex,
		VkBuffer buffer,
		VkDeviceSize offset,
		VkDeviceSize sizeInBytes)
	{
		if (device == VK_NULL_HANDLE ||
			descriptorSet == VK_NULL_HANDLE ||
			buffer == VK_NULL_HANDLE ||
			sizeInBytes == 0)
		{
			return false;
		}

		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = buffer;
		bufferInfo.offset = offset;
		bufferInfo.range = sizeInBytes;

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = descriptorSet;
		write.dstBinding = 0;
		write.dstArrayElement = arrayIndex;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		write.pBufferInfo = &bufferInfo;
		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
		return true;
	}
#endif
}

RHIBufferHandle VulkanBackend::RegisterBindlessBuffer(Buffer* buffer)
{
#if !CORONA_HAS_VULKAN
	(void)buffer;
	return {};
#else
	if (!buffer || !bBindlessBufferTableReady || BindlessBufferDescriptorSet == VK_NULL_HANDLE)
		return {};
	auto allocationIt = BufferAllocations.find(buffer);
	if (allocationIt == BufferAllocations.end())
		return {};

	{
		std::lock_guard<std::mutex> lock(BindlessBufferMutex);
		const RHIBufferHandle existing = buffer->BindlessHandle;
		if (existing.IsValid() &&
			existing.Index < BindlessBufferSlots.size() &&
			BindlessBufferSlots[existing.Index].Occupied &&
			BindlessBufferSlots[existing.Index].Generation == existing.Generation &&
			BindlessBufferSlots[existing.Index].BufferPtr == buffer)
		{
			return existing;
		}

		uint32_t slotIndex = RHI_INVALID_BINDLESS_INDEX;
		if (!BindlessBufferFreeList.empty())
		{
			slotIndex = BindlessBufferFreeList.back();
			BindlessBufferFreeList.pop_back();
		}
		else
		{
			if (BindlessBufferSlots.size() >= MaxVulkanBindlessBufferSlots)
				return {};
			slotIndex = static_cast<uint32_t>(BindlessBufferSlots.size());
			BindlessBufferSlots.emplace_back();
		}

		VulkanBindlessBufferSlot& slot = BindlessBufferSlots[slotIndex];
		if (slot.Generation == 0)
			slot.Generation = 1;
		slot.BufferPtr = buffer;
		slot.ResourceType = 1;
		slot.Occupied = true;
		buffer->BindlessHandle = { slotIndex, slot.Generation };
	}

	if (!WriteVulkanBindlessBufferDescriptor(
		Device,
		BindlessBufferDescriptorSet,
		buffer->BindlessHandle.Index,
		allocationIt->second.Buffer,
		allocationIt->second.Offset,
		allocationIt->second.SizeInBytes))
	{
		UnregisterBindlessBuffer(buffer);
		return {};
	}
	return buffer->BindlessHandle;
#endif
}

RHIBufferHandle VulkanBackend::RegisterBindlessVertexBuffer(VertexBuffer* buffer)
{
#if !CORONA_HAS_VULKAN
	(void)buffer;
	return {};
#else
	if (!buffer || !bBindlessBufferTableReady || BindlessBufferDescriptorSet == VK_NULL_HANDLE)
		return {};
	auto allocationIt = VertexBufferAllocations.find(buffer);
	if (allocationIt == VertexBufferAllocations.end())
		return {};

	{
		std::lock_guard<std::mutex> lock(BindlessBufferMutex);
		const RHIBufferHandle existing = buffer->BindlessHandle;
		if (existing.IsValid() &&
			existing.Index < BindlessBufferSlots.size() &&
			BindlessBufferSlots[existing.Index].Occupied &&
			BindlessBufferSlots[existing.Index].Generation == existing.Generation &&
			BindlessBufferSlots[existing.Index].BufferPtr == buffer)
		{
			return existing;
		}

		uint32_t slotIndex = RHI_INVALID_BINDLESS_INDEX;
		if (!BindlessBufferFreeList.empty())
		{
			slotIndex = BindlessBufferFreeList.back();
			BindlessBufferFreeList.pop_back();
		}
		else
		{
			if (BindlessBufferSlots.size() >= MaxVulkanBindlessBufferSlots)
				return {};
			slotIndex = static_cast<uint32_t>(BindlessBufferSlots.size());
			BindlessBufferSlots.emplace_back();
		}

		VulkanBindlessBufferSlot& slot = BindlessBufferSlots[slotIndex];
		if (slot.Generation == 0)
			slot.Generation = 1;
		slot.BufferPtr = buffer;
		slot.ResourceType = 2;
		slot.Occupied = true;
		buffer->BindlessHandle = { slotIndex, slot.Generation };
	}

	if (!WriteVulkanBindlessBufferDescriptor(
		Device,
		BindlessBufferDescriptorSet,
		buffer->BindlessHandle.Index,
		allocationIt->second.Buffer,
		allocationIt->second.Offset,
		allocationIt->second.SizeInBytes))
	{
		UnregisterBindlessVertexBuffer(buffer);
		return {};
	}
	return buffer->BindlessHandle;
#endif
}

RHIBufferHandle VulkanBackend::RegisterBindlessIndexBuffer(IndexBuffer* buffer)
{
#if !CORONA_HAS_VULKAN
	(void)buffer;
	return {};
#else
	if (!buffer || !bBindlessBufferTableReady || BindlessBufferDescriptorSet == VK_NULL_HANDLE)
		return {};
	auto allocationIt = IndexBufferAllocations.find(buffer);
	if (allocationIt == IndexBufferAllocations.end())
		return {};

	{
		std::lock_guard<std::mutex> lock(BindlessBufferMutex);
		const RHIBufferHandle existing = buffer->BindlessHandle;
		if (existing.IsValid() &&
			existing.Index < BindlessBufferSlots.size() &&
			BindlessBufferSlots[existing.Index].Occupied &&
			BindlessBufferSlots[existing.Index].Generation == existing.Generation &&
			BindlessBufferSlots[existing.Index].BufferPtr == buffer)
		{
			return existing;
		}

		uint32_t slotIndex = RHI_INVALID_BINDLESS_INDEX;
		if (!BindlessBufferFreeList.empty())
		{
			slotIndex = BindlessBufferFreeList.back();
			BindlessBufferFreeList.pop_back();
		}
		else
		{
			if (BindlessBufferSlots.size() >= MaxVulkanBindlessBufferSlots)
				return {};
			slotIndex = static_cast<uint32_t>(BindlessBufferSlots.size());
			BindlessBufferSlots.emplace_back();
		}

		VulkanBindlessBufferSlot& slot = BindlessBufferSlots[slotIndex];
		if (slot.Generation == 0)
			slot.Generation = 1;
		slot.BufferPtr = buffer;
		slot.ResourceType = 3;
		slot.Occupied = true;
		buffer->BindlessHandle = { slotIndex, slot.Generation };
	}

	if (!WriteVulkanBindlessBufferDescriptor(
		Device,
		BindlessBufferDescriptorSet,
		buffer->BindlessHandle.Index,
		allocationIt->second.Buffer,
		allocationIt->second.Offset,
		allocationIt->second.SizeInBytes))
	{
		UnregisterBindlessIndexBuffer(buffer);
		return {};
	}
	return buffer->BindlessHandle;
#endif
}

void VulkanBackend::UnregisterBindlessBuffer(Buffer* buffer)
{
#if !CORONA_HAS_VULKAN
	(void)buffer;
#else
	if (!buffer)
		return;
	std::lock_guard<std::mutex> lock(BindlessBufferMutex);
	const RHIBufferHandle handle = buffer->BindlessHandle;
	if (handle.IsValid() && handle.Index < BindlessBufferSlots.size())
	{
		VulkanBindlessBufferSlot& slot = BindlessBufferSlots[handle.Index];
		if (slot.Occupied && slot.Generation == handle.Generation && slot.BufferPtr == buffer)
		{
			slot.BufferPtr = nullptr;
			slot.ResourceType = 0;
			slot.Occupied = false;
			++slot.Generation;
			if (slot.Generation == 0)
				slot.Generation = 1;
			BindlessBufferFreeList.push_back(handle.Index);
		}
	}
	buffer->BindlessHandle = {};
#endif
}

void VulkanBackend::UnregisterBindlessVertexBuffer(VertexBuffer* buffer)
{
#if !CORONA_HAS_VULKAN
	(void)buffer;
#else
	if (!buffer)
		return;
	std::lock_guard<std::mutex> lock(BindlessBufferMutex);
	const RHIBufferHandle handle = buffer->BindlessHandle;
	if (handle.IsValid() && handle.Index < BindlessBufferSlots.size())
	{
		VulkanBindlessBufferSlot& slot = BindlessBufferSlots[handle.Index];
		if (slot.Occupied && slot.Generation == handle.Generation && slot.BufferPtr == buffer)
		{
			slot.BufferPtr = nullptr;
			slot.ResourceType = 0;
			slot.Occupied = false;
			++slot.Generation;
			if (slot.Generation == 0)
				slot.Generation = 1;
			BindlessBufferFreeList.push_back(handle.Index);
		}
	}
	buffer->BindlessHandle = {};
#endif
}

void VulkanBackend::UnregisterBindlessIndexBuffer(IndexBuffer* buffer)
{
#if !CORONA_HAS_VULKAN
	(void)buffer;
#else
	if (!buffer)
		return;
	std::lock_guard<std::mutex> lock(BindlessBufferMutex);
	const RHIBufferHandle handle = buffer->BindlessHandle;
	if (handle.IsValid() && handle.Index < BindlessBufferSlots.size())
	{
		VulkanBindlessBufferSlot& slot = BindlessBufferSlots[handle.Index];
		if (slot.Occupied && slot.Generation == handle.Generation && slot.BufferPtr == buffer)
		{
			slot.BufferPtr = nullptr;
			slot.ResourceType = 0;
			slot.Occupied = false;
			++slot.Generation;
			if (slot.Generation == 0)
				slot.Generation = 1;
			BindlessBufferFreeList.push_back(handle.Index);
		}
	}
	buffer->BindlessHandle = {};
#endif
}

RHIBufferHandle VulkanBackend::GetBindlessBufferHandle(const Buffer* buffer) const
{
#if !CORONA_HAS_VULKAN
	(void)buffer;
	return {};
#else
	if (!buffer)
		return {};
	std::lock_guard<std::mutex> lock(BindlessBufferMutex);
	const RHIBufferHandle handle = buffer->BindlessHandle;
	if (!handle.IsValid() ||
		handle.Index >= BindlessBufferSlots.size() ||
		!BindlessBufferSlots[handle.Index].Occupied ||
		BindlessBufferSlots[handle.Index].Generation != handle.Generation ||
		BindlessBufferSlots[handle.Index].BufferPtr != buffer)
	{
		return {};
	}
	return handle;
#endif
}

RHIBufferHandle VulkanBackend::GetBindlessVertexBufferHandle(const VertexBuffer* buffer) const
{
#if !CORONA_HAS_VULKAN
	(void)buffer;
	return {};
#else
	if (!buffer)
		return {};
	std::lock_guard<std::mutex> lock(BindlessBufferMutex);
	const RHIBufferHandle handle = buffer->BindlessHandle;
	if (!handle.IsValid() ||
		handle.Index >= BindlessBufferSlots.size() ||
		!BindlessBufferSlots[handle.Index].Occupied ||
		BindlessBufferSlots[handle.Index].Generation != handle.Generation ||
		BindlessBufferSlots[handle.Index].BufferPtr != buffer)
	{
		return {};
	}
	return handle;
#endif
}

RHIBufferHandle VulkanBackend::GetBindlessIndexBufferHandle(const IndexBuffer* buffer) const
{
#if !CORONA_HAS_VULKAN
	(void)buffer;
	return {};
#else
	if (!buffer)
		return {};
	std::lock_guard<std::mutex> lock(BindlessBufferMutex);
	const RHIBufferHandle handle = buffer->BindlessHandle;
	if (!handle.IsValid() ||
		handle.Index >= BindlessBufferSlots.size() ||
		!BindlessBufferSlots[handle.Index].Occupied ||
		BindlessBufferSlots[handle.Index].Generation != handle.Generation ||
		BindlessBufferSlots[handle.Index].BufferPtr != buffer)
	{
		return {};
	}
	return handle;
#endif
}

std::shared_ptr<Texture> VulkanBackend::CreateTexture3D(ETextureFormat format, ETextureUsageFlags usage, EInitialResourceState initialState, int width, int height, int depth, int mipLevels)
{
#if !CORONA_HAS_VULKAN
	(void)format; (void)usage; (void)initialState; (void)width; (void)height; (void)depth; (void)mipLevels;
	ThrowNotImplemented(__FUNCTION__);
#else
	auto texture = CreateTrackedTextureHandle();
	VulkanTextureAllocation allocation{};
	allocation.Format = ToVkFormat(format);
	allocation.Width = static_cast<uint32_t>(width);
	allocation.Height = static_cast<uint32_t>(height);
	allocation.Depth = static_cast<uint32_t>(depth);
	allocation.Usage = usage;

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_3D;
	imageInfo.format = allocation.Format;
	imageInfo.extent.width = allocation.Width;
	imageInfo.extent.height = allocation.Height;
	imageInfo.extent.depth = allocation.Depth;
	imageInfo.mipLevels = static_cast<uint32_t>((std::max)(mipLevels, 1));
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = GetImageUsageFlags(usage) | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(Device, &imageInfo, nullptr, &allocation.Image) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan 3D texture image.");

	VkMemoryRequirements memoryRequirements{};
	vkGetImageMemoryRequirements(Device, allocation.Image, &memoryRequirements);

	VkMemoryAllocateInfo allocateInfo{};
	allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocateInfo.allocationSize = memoryRequirements.size;
	allocateInfo.memoryTypeIndex = FindMemoryTypeIndex(
		PhysicalDevice,
		memoryRequirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(Device, &allocateInfo, nullptr, &allocation.Memory) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate Vulkan 3D texture memory.");

	if (vkBindImageMemory(Device, allocation.Image, allocation.Memory, 0) != VK_SUCCESS)
		throw std::runtime_error("Failed to bind Vulkan 3D texture memory.");

	VkImageViewCreateInfo imageViewInfo{};
	imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	imageViewInfo.image = allocation.Image;
	imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
	imageViewInfo.format = allocation.Format;
	imageViewInfo.subresourceRange.aspectMask = GetImageAspectFlags(format);
	imageViewInfo.subresourceRange.levelCount = imageInfo.mipLevels;
	imageViewInfo.subresourceRange.layerCount = 1;
	if (vkCreateImageView(Device, &imageViewInfo, nullptr, &allocation.ImageView) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan 3D texture view.");

	(void)initialState;
	allocation.CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	TextureAllocations[texture.get()] = allocation;
	return texture;
#endif
}

void VulkanBackend::UploadTexture3D(Texture* texture, const void* data, uint64_t rowPitch, uint64_t slicePitch)
{
#if !CORONA_HAS_VULKAN
	(void)texture; (void)data; (void)rowPitch; (void)slicePitch; ThrowNotImplemented(__FUNCTION__);
#else
	if (!texture || !data || rowPitch == 0 || slicePitch == 0)
		return;

	auto textureIt = TextureAllocations.find(texture);
	if (textureIt == TextureAllocations.end())
		return;

	VulkanTextureAllocation& allocation = textureIt->second;
	const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(slicePitch * allocation.Depth);

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	if (!CreateBufferWithMemory(
		uploadSize,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		false,
		stagingBuffer,
		stagingMemory))
	{
		throw std::runtime_error("Failed to create Vulkan 3D texture upload buffer.");
	}

	void* mappedData = nullptr;
	if (vkMapMemory(Device, stagingMemory, 0, uploadSize, 0, &mappedData) != VK_SUCCESS)
	{
		vkDestroyBuffer(Device, stagingBuffer, nullptr);
		vkFreeMemory(Device, stagingMemory, nullptr);
		throw std::runtime_error("Failed to map Vulkan 3D texture upload buffer.");
	}
	std::memcpy(mappedData, data, static_cast<size_t>(uploadSize));
	vkUnmapMemory(Device, stagingMemory);

	VkCommandBufferAllocateInfo commandBufferAllocInfo{};
	commandBufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocInfo.commandPool = CommandPool;
	commandBufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocInfo.commandBufferCount = 1;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(Device, &commandBufferAllocInfo, &commandBuffer) != VK_SUCCESS)
	{
		vkDestroyBuffer(Device, stagingBuffer, nullptr);
		vkFreeMemory(Device, stagingMemory, nullptr);
		throw std::runtime_error("Failed to allocate Vulkan 3D texture upload command buffer.");
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	TransitionImageLayoutGeneric(
		commandBuffer,
		allocation.Image,
		VK_IMAGE_ASPECT_COLOR_BIT,
		allocation.CurrentLayout,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	allocation.CurrentLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

	VkBufferImageCopy copyRegion{};
	copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegion.imageSubresource.layerCount = 1;
	copyRegion.imageExtent.width = allocation.Width;
	copyRegion.imageExtent.height = allocation.Height;
	copyRegion.imageExtent.depth = allocation.Depth;
	vkCmdCopyBufferToImage(
		commandBuffer,
		stagingBuffer,
		allocation.Image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&copyRegion);

	TransitionImageLayoutGeneric(
		commandBuffer,
		allocation.Image,
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	allocation.CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		vkDestroyBuffer(Device, stagingBuffer, nullptr);
		vkFreeMemory(Device, stagingMemory, nullptr);
		throw std::runtime_error("Failed to record Vulkan 3D texture upload command buffer.");
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		vkDestroyBuffer(Device, stagingBuffer, nullptr);
		vkFreeMemory(Device, stagingMemory, nullptr);
		throw std::runtime_error("Failed to submit Vulkan 3D texture upload.");
	}
	vkQueueWaitIdle(GraphicsQueue);

	vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
	vkDestroyBuffer(Device, stagingBuffer, nullptr);
	vkFreeMemory(Device, stagingMemory, nullptr);
#endif
}
std::shared_ptr<VertexBuffer> VulkanBackend::CreateVertexBuffer(uint32_t size, uint32_t stride, void* srcData)
{
#if !CORONA_HAS_VULKAN
	(void)size; (void)stride; (void)srcData; ThrowNotImplemented(__FUNCTION__);
#else
	auto vertexBuffer = CreateTrackedVertexBufferHandle();
	vertexBuffer->numVertices = stride > 0 ? static_cast<int>(size / stride) : 0;

	VulkanBufferAllocation allocation{};
	allocation.Stride = stride;
	allocation.SizeInBytes = size;

	VkBufferUsageFlags bufferUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (bRayTracingEnabled)
	{
		bufferUsage |=
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	}
	if (!CreateDeviceLocalBufferWithUpload(
		size,
		bufferUsage,
		bRayTracingEnabled,
		srcData,
		allocation.Buffer,
		allocation.Memory))
	{
		throw std::runtime_error("Failed to create Vulkan vertex buffer.");
	}

	VertexBufferAllocations[vertexBuffer.get()] = allocation;
	return vertexBuffer;
#endif
}
std::shared_ptr<IndexBuffer> VulkanBackend::CreateIndexBuffer(EIndexFormat format, uint32_t size, void* srcData)
{
#if !CORONA_HAS_VULKAN
	(void)format; (void)size; (void)srcData; ThrowNotImplemented(__FUNCTION__);
#else
	auto indexBuffer = CreateTrackedIndexBufferHandle();
	indexBuffer->numIndices = format == EIndexFormat::U16 ? static_cast<int>(size / 2) : static_cast<int>(size / 4);

	VulkanBufferAllocation allocation{};
	allocation.Stride = format == EIndexFormat::U16 ? 2u : 4u;
	allocation.SizeInBytes = size;

	VkBufferUsageFlags bufferUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (bRayTracingEnabled)
	{
		bufferUsage |=
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	}
	if (!CreateDeviceLocalBufferWithUpload(
		size,
		bufferUsage,
		bRayTracingEnabled,
		srcData,
		allocation.Buffer,
		allocation.Memory))
	{
		throw std::runtime_error("Failed to create Vulkan index buffer.");
	}

	IndexBufferAllocations[indexBuffer.get()] = allocation;
	return indexBuffer;
#endif
}

// Phase 3.5 (Vulkan) — HOST_VISIBLE direct-write upload pool. Sub-allocates
// VB/IB ranges from a large persistent VkBuffer, eliminating the staged
// copy path's per-call vkQueueSubmit + vkQueueWaitIdle. On Adreno's
// unified-memory architecture, HOST_VISIBLE | HOST_COHERENT reads from
// the GPU are essentially free, so the trade-off is strictly favourable
// for many small per-frame buffers like Spine sprite geometry.
//
// Lifetime: the pool keeps `ActiveUploadBlock` as a bump-target. Each
// VulkanBufferAllocation that came from the pool holds a shared_ptr to
// its block via `PoolBlock`. When the block fills, a new block is
// committed; the old one stays alive automatically while any sub-alloc
// still references it. The block destructor unmaps, frees memory, and
// destroys the VkBuffer.
VulkanBackend::VulkanUploadHeapBlock::~VulkanUploadHeapBlock()
{
#if CORONA_HAS_VULKAN
	if (OwningDevice == VK_NULL_HANDLE)
		return;
	if (MappedBase != nullptr && Memory != VK_NULL_HANDLE)
		vkUnmapMemory(OwningDevice, Memory);
	if (Buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(OwningDevice, Buffer, nullptr);
	if (Memory != VK_NULL_HANDLE)
		vkFreeMemory(OwningDevice, Memory, nullptr);
	OwningDevice = VK_NULL_HANDLE;
	Buffer = VK_NULL_HANDLE;
	Memory = VK_NULL_HANDLE;
	MappedBase = nullptr;
#endif
}

VulkanBackend::VulkanPersistentBufferBlock::~VulkanPersistentBufferBlock()
{
#if CORONA_HAS_VULKAN
	if (OwningDevice == VK_NULL_HANDLE)
		return;
	if (Buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(OwningDevice, Buffer, nullptr);
	if (Memory != VK_NULL_HANDLE)
		vkFreeMemory(OwningDevice, Memory, nullptr);
	OwningDevice = VK_NULL_HANDLE;
	Buffer = VK_NULL_HANDLE;
	Memory = VK_NULL_HANDLE;
	Capacity = 0;
	Cursor = 0;
#endif
}

void VulkanBackend::AddPersistentStructuredBufferFreeRange(
	const std::shared_ptr<VulkanPersistentBufferBlock>& block,
	VkDeviceSize offset,
	VkDeviceSize size)
{
#if CORONA_HAS_VULKAN
	if (!block || size == 0)
		return;

	VulkanPersistentBufferBlock::FreeRange newRange{ offset, size };
	auto& ranges = block->FreeRanges;
	auto insertIt = ranges.begin();
	while (insertIt != ranges.end() && insertIt->Offset < newRange.Offset)
		++insertIt;
	insertIt = ranges.insert(insertIt, newRange);

	if (insertIt != ranges.begin())
	{
		auto prevIt = std::prev(insertIt);
		const VkDeviceSize prevEnd = prevIt->Offset + prevIt->Size;
		if (prevEnd >= insertIt->Offset)
		{
			prevIt->Size = std::max(prevEnd, insertIt->Offset + insertIt->Size) - prevIt->Offset;
			insertIt = ranges.erase(insertIt);
			insertIt = prevIt;
		}
	}

	auto nextIt = std::next(insertIt);
	while (nextIt != ranges.end())
	{
		const VkDeviceSize rangeEnd = insertIt->Offset + insertIt->Size;
		if (rangeEnd < nextIt->Offset)
			break;
		insertIt->Size = std::max(rangeEnd, nextIt->Offset + nextIt->Size) - insertIt->Offset;
		nextIt = ranges.erase(nextIt);
	}
#else
	(void)block; (void)offset; (void)size;
#endif
}

void VulkanBackend::ReleasePersistentStructuredBufferRange(const VulkanBufferAllocation& allocation)
{
#if CORONA_HAS_VULKAN
	if (!allocation.PersistentPoolBlock || allocation.SizeInBytes == 0)
		return;

	if (FrameContexts.empty())
	{
		AddPersistentStructuredBufferFreeRange(
			allocation.PersistentPoolBlock,
			allocation.Offset,
			static_cast<VkDeviceSize>(allocation.SizeInBytes));
		return;
	}

	if (PendingPersistentStructuredBufferFrees.size() < FrameContexts.size())
		PendingPersistentStructuredBufferFrees.resize(FrameContexts.size());

	const uint32_t frameIndex =
		CurrentFrameIndex < PendingPersistentStructuredBufferFrees.size()
			? CurrentFrameIndex
			: 0u;
	PendingPersistentStructuredBufferFrees[frameIndex].push_back({
		allocation.PersistentPoolBlock,
		allocation.Offset,
		static_cast<VkDeviceSize>(allocation.SizeInBytes)
	});
#else
	(void)allocation;
#endif
}

void VulkanBackend::RetirePersistentStructuredBufferFrees(uint32_t frameIndex)
{
#if CORONA_HAS_VULKAN
	if (frameIndex >= PendingPersistentStructuredBufferFrees.size())
		return;

	std::vector<PendingPersistentStructuredBufferFree>& pending =
		PendingPersistentStructuredBufferFrees[frameIndex];
	for (const PendingPersistentStructuredBufferFree& freeRange : pending)
	{
		AddPersistentStructuredBufferFreeRange(freeRange.Block, freeRange.Offset, freeRange.Size);
	}
	pending.clear();
#else
	(void)frameIndex;
#endif
}

bool VulkanBackend::AllocatePersistentStructuredBufferRange(
	VkDeviceSize size,
	VkDeviceSize alignment,
	const void* srcData,
	VulkanBufferAllocation& outAllocation)
{
	outAllocation = {};
#if !CORONA_HAS_VULKAN
	(void)size; (void)alignment; (void)srcData;
	return false;
#else
	if (size == 0 || !srcData)
		return false;

	const VkDeviceSize align = std::max<VkDeviceSize>(alignment > 0 ? alignment : 4, StorageBufferAlignment);
	auto allocateFromBlock = [&](const std::shared_ptr<VulkanPersistentBufferBlock>& block) -> bool
	{
		if (!block || block->Buffer == VK_NULL_HANDLE)
			return false;

		for (size_t rangeIndex = 0; rangeIndex < block->FreeRanges.size(); ++rangeIndex)
		{
			VulkanPersistentBufferBlock::FreeRange range = block->FreeRanges[rangeIndex];
			const VkDeviceSize alignedOffset = AlignVkDeviceSize(range.Offset, align);
			const VkDeviceSize rangeEnd = range.Offset + range.Size;
			if (alignedOffset > rangeEnd || alignedOffset + size > rangeEnd)
				continue;

			auto insertIt = block->FreeRanges.erase(block->FreeRanges.begin() + rangeIndex);
			if (alignedOffset > range.Offset)
			{
				insertIt = block->FreeRanges.insert(insertIt, { range.Offset, alignedOffset - range.Offset });
				++insertIt;
			}
			const VkDeviceSize allocEnd = alignedOffset + size;
			if (allocEnd < rangeEnd)
			{
				block->FreeRanges.insert(insertIt, { allocEnd, rangeEnd - allocEnd });
			}

			outAllocation.Buffer = block->Buffer;
			outAllocation.Memory = block->Memory;
			outAllocation.Offset = alignedOffset;
			outAllocation.SizeInBytes = static_cast<uint32_t>(size);
			outAllocation.PersistentPoolBlock = block;
			++PersistentStructuredBufferAllocationCount;
			PersistentStructuredBufferBytesIssued += size;
			return true;
		}

		const VkDeviceSize alignedCursor = AlignVkDeviceSize(block->Cursor, align);
		if (alignedCursor + size > block->Capacity)
			return false;
		outAllocation.Buffer = block->Buffer;
		outAllocation.Memory = block->Memory;
		outAllocation.Offset = alignedCursor;
		outAllocation.SizeInBytes = static_cast<uint32_t>(size);
		outAllocation.PersistentPoolBlock = block;
		block->Cursor = alignedCursor + size;
		++PersistentStructuredBufferAllocationCount;
		PersistentStructuredBufferBytesIssued += size;
		return true;
	};

	for (const std::shared_ptr<VulkanPersistentBufferBlock>& block : PersistentStructuredBufferBlocks)
	{
		if (allocateFromBlock(block))
			goto upload;
	}

	{
		const VkDeviceSize requestedSize = size + align;
		const VkDeviceSize blockSize = std::max<VkDeviceSize>(PersistentStructuredBufferBlockDefaultSize, requestedSize);
		auto block = std::make_shared<VulkanPersistentBufferBlock>();
		block->OwningDevice = Device;
		block->Capacity = blockSize;

		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = blockSize;
		bufferInfo.usage =
			VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		if (bRayTracingEnabled)
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		}
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(Device, &bufferInfo, nullptr, &block->Buffer) != VK_SUCCESS)
			return false;

		VkMemoryRequirements memReq{};
		vkGetBufferMemoryRequirements(Device, block->Buffer, &memReq);

		uint32_t memoryTypeIndex = UINT32_MAX;
		VkPhysicalDeviceMemoryProperties memProps{};
		vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &memProps);
		for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
		{
			if (!(memReq.memoryTypeBits & (1u << i)))
				continue;
			if (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			{
				memoryTypeIndex = i;
				break;
			}
		}
		if (memoryTypeIndex == UINT32_MAX)
		{
			vkDestroyBuffer(Device, block->Buffer, nullptr);
			block->Buffer = VK_NULL_HANDLE;
			return false;
		}

		VkMemoryAllocateFlagsInfo allocFlags{};
		allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
		if (bRayTracingEnabled)
			allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

		VkMemoryAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.pNext = bRayTracingEnabled ? &allocFlags : nullptr;
		allocInfo.allocationSize = memReq.size;
		allocInfo.memoryTypeIndex = memoryTypeIndex;
		if (vkAllocateMemory(Device, &allocInfo, nullptr, &block->Memory) != VK_SUCCESS)
		{
			vkDestroyBuffer(Device, block->Buffer, nullptr);
			block->Buffer = VK_NULL_HANDLE;
			return false;
		}
		if (vkBindBufferMemory(Device, block->Buffer, block->Memory, 0) != VK_SUCCESS)
		{
			vkFreeMemory(Device, block->Memory, nullptr);
			vkDestroyBuffer(Device, block->Buffer, nullptr);
			block->Memory = VK_NULL_HANDLE;
			block->Buffer = VK_NULL_HANDLE;
			return false;
		}

		PersistentStructuredBufferBlocks.push_back(block);
		++PersistentStructuredBufferBlockCount;
		PersistentStructuredBufferBytesReserved += blockSize;
		if (!allocateFromBlock(block))
			return false;
	}

upload:
	if (outAllocation.Buffer == VK_NULL_HANDLE || outAllocation.SizeInBytes == 0)
		return false;

	auto releaseReservedRange = [&]()
	{
		if (outAllocation.PersistentPoolBlock && outAllocation.SizeInBytes != 0)
		{
			AddPersistentStructuredBufferFreeRange(
				outAllocation.PersistentPoolBlock,
				outAllocation.Offset,
				static_cast<VkDeviceSize>(outAllocation.SizeInBytes));
		}
		outAllocation = {};
	};

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	auto cleanupStaging = [&]()
	{
		if (stagingBuffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, stagingBuffer, nullptr);
		if (stagingMemory != VK_NULL_HANDLE)
			vkFreeMemory(Device, stagingMemory, nullptr);
	};

	if (!CreateBufferWithMemory(
		size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		false,
		stagingBuffer,
		stagingMemory))
	{
		releaseReservedRange();
		return false;
	}

	void* mappedData = nullptr;
	if (vkMapMemory(Device, stagingMemory, 0, size, 0, &mappedData) != VK_SUCCESS)
	{
		cleanupStaging();
		releaseReservedRange();
		return false;
	}
	std::memcpy(mappedData, srcData, static_cast<size_t>(size));
	vkUnmapMemory(Device, stagingMemory);

	VkCommandBufferAllocateInfo commandBufferAllocInfo{};
	commandBufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocInfo.commandPool = CommandPool;
	commandBufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocInfo.commandBufferCount = 1;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(Device, &commandBufferAllocInfo, &commandBuffer) != VK_SUCCESS)
	{
		cleanupStaging();
		releaseReservedRange();
		return false;
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		cleanupStaging();
		releaseReservedRange();
		return false;
	}

	VkBufferCopy copyRegion{};
	copyRegion.srcOffset = 0;
	copyRegion.dstOffset = outAllocation.Offset;
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, stagingBuffer, outAllocation.Buffer, 1, &copyRegion);

	VkBufferMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = outAllocation.Buffer;
	barrier.offset = outAllocation.Offset;
	barrier.size = size;
	vkCmdPipelineBarrier(
		commandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0,
		0, nullptr,
		1, &barrier,
		0, nullptr);

	if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		cleanupStaging();
		releaseReservedRange();
		return false;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		cleanupStaging();
		releaseReservedRange();
		return false;
	}
	vkQueueWaitIdle(GraphicsQueue);
	vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
	cleanupStaging();
	return true;
#endif
}

std::shared_ptr<Buffer> VulkanBackend::CreateSuballocatedStructuredBuffer(const BufferCreateDesc& desc)
{
#if !CORONA_HAS_VULKAN
	(void)desc;
	return nullptr;
#else
	if (desc.NumElements == 0 || desc.ElementSize == 0 || !desc.InitialData)
		return nullptr;

	const uint64_t sizeInBytes64 =
		static_cast<uint64_t>(desc.NumElements) * static_cast<uint64_t>(desc.ElementSize);
	if (sizeInBytes64 > UINT32_MAX)
		return nullptr;

	VulkanBufferAllocation allocation{};
	if (!AllocatePersistentStructuredBufferRange(
		static_cast<VkDeviceSize>(sizeInBytes64),
		static_cast<VkDeviceSize>(desc.ElementSize),
		desc.InitialData,
		allocation))
	{
		return nullptr;
	}
	allocation.Stride = desc.ElementSize;

	auto buffer = CreateTrackedBufferHandle();
	buffer->NumElements = desc.NumElements;
	buffer->ElementSize = desc.ElementSize;
	buffer->Type = Buffer::STRUCTURED;
	BufferAllocations[buffer.get()] = allocation;
	return buffer;
#endif
}

bool VulkanBackend::AllocateUploadBufferRange(
	VkDeviceSize size,
	VkDeviceSize alignment,
	const void* srcData,
	VulkanBufferAllocation& outAllocation)
{
	outAllocation = {};
#if !CORONA_HAS_VULKAN
	(void)size; (void)alignment; (void)srcData;
	return false;
#else
	if (size == 0)
		return false;
	if (alignment == 0)
		alignment = 4;

	auto tryAllocateFromActive = [&]() -> bool
	{
		if (!ActiveUploadBlock)
			return false;
		VulkanUploadHeapBlock& block = *ActiveUploadBlock;
		const VkDeviceSize alignedCursor = AlignVkDeviceSize(block.Cursor, alignment);
		if (alignedCursor + size > block.Capacity)
			return false;
		outAllocation.Buffer = block.Buffer;
		outAllocation.Memory = block.Memory;
		outAllocation.Offset = alignedCursor;
		outAllocation.SizeInBytes = static_cast<uint32_t>(size);
		outAllocation.PoolBlock = ActiveUploadBlock;
		if (srcData)
			std::memcpy(block.MappedBase + alignedCursor, srcData, static_cast<size_t>(size));
		block.Cursor = alignedCursor + size;
		++UploadAllocationCount;
		UploadBytesIssued += size;
		return true;
	};

	if (tryAllocateFromActive())
		return true;

	// Need a new block. Size to at least this request + alignment padding.
	const VkDeviceSize requestedSize = size + alignment;
	const VkDeviceSize blockSize = std::max<VkDeviceSize>(UploadBlockDefaultSize, requestedSize);

	auto newBlock = std::make_shared<VulkanUploadHeapBlock>();
	newBlock->OwningDevice = Device;
	newBlock->Capacity = blockSize;

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = blockSize;
	// The block backs VBs, IBs, AND structured buffers (live spine bones,
	// skeletal instance transforms) — all three usage bits are required
	// or Adreno silently reads garbage when an UPLOAD-pool allocation is
	// bound as VK_DESCRIPTOR_TYPE_STORAGE_BUFFER.
	bufferInfo.usage =
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(Device, &bufferInfo, nullptr, &newBlock->Buffer) != VK_SUCCESS)
		return false;

	VkMemoryRequirements memReq{};
	vkGetBufferMemoryRequirements(Device, newBlock->Buffer, &memReq);

	// HOST_VISIBLE | HOST_COHERENT — readable by the GPU without explicit
	// flush, writable by the CPU via persistent mapping. Skip
	// HOST_CACHED so writes go straight to GPU-visible memory.
	uint32_t memoryTypeIndex = UINT32_MAX;
	VkPhysicalDeviceMemoryProperties memProps{};
	vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &memProps);
	const VkMemoryPropertyFlags wantProps =
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
	{
		if (!(memReq.memoryTypeBits & (1u << i)))
			continue;
		if ((memProps.memoryTypes[i].propertyFlags & wantProps) == wantProps)
		{
			memoryTypeIndex = i;
			break;
		}
	}
	if (memoryTypeIndex == UINT32_MAX)
	{
		vkDestroyBuffer(Device, newBlock->Buffer, nullptr);
		newBlock->Buffer = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = memoryTypeIndex;
	if (vkAllocateMemory(Device, &allocInfo, nullptr, &newBlock->Memory) != VK_SUCCESS)
	{
		vkDestroyBuffer(Device, newBlock->Buffer, nullptr);
		newBlock->Buffer = VK_NULL_HANDLE;
		return false;
	}
	if (vkBindBufferMemory(Device, newBlock->Buffer, newBlock->Memory, 0) != VK_SUCCESS)
	{
		vkFreeMemory(Device, newBlock->Memory, nullptr);
		vkDestroyBuffer(Device, newBlock->Buffer, nullptr);
		newBlock->Memory = VK_NULL_HANDLE;
		newBlock->Buffer = VK_NULL_HANDLE;
		return false;
	}

	void* mapped = nullptr;
	if (vkMapMemory(Device, newBlock->Memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
	{
		vkFreeMemory(Device, newBlock->Memory, nullptr);
		vkDestroyBuffer(Device, newBlock->Buffer, nullptr);
		newBlock->Memory = VK_NULL_HANDLE;
		newBlock->Buffer = VK_NULL_HANDLE;
		return false;
	}
	newBlock->MappedBase = static_cast<uint8_t*>(mapped);
	newBlock->Cursor = 0;

	ActiveUploadBlock = std::move(newBlock);
	++UploadBlockCount;
	UploadBytesReserved += blockSize;

	return tryAllocateFromActive();
#endif
}

#if CORONA_HAS_VULKAN
void VulkanBackend::ResetTransientUploadStructuredFrame(uint32_t frameIndex)
{
	const size_t frameCount = std::max<size_t>(FrameContexts.size(), 1u);
	if (TransientUploadStructuredFrames.size() < frameCount)
		TransientUploadStructuredFrames.resize(frameCount);
	if (frameIndex >= TransientUploadStructuredFrames.size())
		return;

	VulkanTransientUploadStructuredFrame& frame = TransientUploadStructuredFrames[frameIndex];
	frame.KeepAlive.clear();
	for (const std::shared_ptr<VulkanUploadHeapBlock>& block : frame.Blocks)
	{
		if (block)
			block->Cursor = 0;
	}
}

bool VulkanBackend::AllocateTransientUploadStructuredRange(
	uint32_t frameIndex,
	VkDeviceSize size,
	VkDeviceSize alignment,
	const void* srcData,
	VulkanBufferAllocation& outAllocation)
{
	outAllocation = {};
	if (size == 0)
		return false;

	const size_t frameCount = std::max<size_t>(FrameContexts.size(), 1u);
	if (TransientUploadStructuredFrames.size() < frameCount)
		TransientUploadStructuredFrames.resize(frameCount);
	if (frameIndex >= TransientUploadStructuredFrames.size())
		frameIndex = 0;

	VulkanTransientUploadStructuredFrame& frame = TransientUploadStructuredFrames[frameIndex];
	const VkDeviceSize align = std::max<VkDeviceSize>(alignment > 0 ? alignment : 4, StorageBufferAlignment);

	auto allocateFromBlock = [&](const std::shared_ptr<VulkanUploadHeapBlock>& block) -> bool
	{
		if (!block || block->Buffer == VK_NULL_HANDLE || !block->MappedBase)
			return false;
		const VkDeviceSize alignedCursor = AlignVkDeviceSize(block->Cursor, align);
		if (alignedCursor + size > block->Capacity)
			return false;
		outAllocation.Buffer = block->Buffer;
		outAllocation.Memory = block->Memory;
		outAllocation.Offset = alignedCursor;
		outAllocation.SizeInBytes = static_cast<uint32_t>(size);
		outAllocation.PoolBlock = block;
		if (srcData)
			std::memcpy(block->MappedBase + alignedCursor, srcData, static_cast<size_t>(size));
		block->Cursor = alignedCursor + size;
		++TransientUploadStructuredAllocationCount;
		TransientUploadStructuredBytesIssued += size;
		return true;
	};

	for (const std::shared_ptr<VulkanUploadHeapBlock>& block : frame.Blocks)
	{
		if (allocateFromBlock(block))
			return true;
	}

	const VkDeviceSize requestedSize = size + align;
	const VkDeviceSize blockSize = std::max<VkDeviceSize>(TransientUploadStructuredBlockDefaultSize, requestedSize);
	auto block = std::make_shared<VulkanUploadHeapBlock>();
	block->OwningDevice = Device;
	block->Capacity = blockSize;

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = blockSize;
	bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(Device, &bufferInfo, nullptr, &block->Buffer) != VK_SUCCESS)
		return false;

	VkMemoryRequirements memReq{};
	vkGetBufferMemoryRequirements(Device, block->Buffer, &memReq);

	uint32_t memoryTypeIndex = UINT32_MAX;
	VkPhysicalDeviceMemoryProperties memProps{};
	vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &memProps);
	const VkMemoryPropertyFlags wantProps =
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
	{
		if (!(memReq.memoryTypeBits & (1u << i)))
			continue;
		if ((memProps.memoryTypes[i].propertyFlags & wantProps) == wantProps)
		{
			memoryTypeIndex = i;
			break;
		}
	}
	if (memoryTypeIndex == UINT32_MAX)
	{
		vkDestroyBuffer(Device, block->Buffer, nullptr);
		block->Buffer = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = memoryTypeIndex;
	if (vkAllocateMemory(Device, &allocInfo, nullptr, &block->Memory) != VK_SUCCESS)
	{
		vkDestroyBuffer(Device, block->Buffer, nullptr);
		block->Buffer = VK_NULL_HANDLE;
		return false;
	}
	if (vkBindBufferMemory(Device, block->Buffer, block->Memory, 0) != VK_SUCCESS)
	{
		vkFreeMemory(Device, block->Memory, nullptr);
		vkDestroyBuffer(Device, block->Buffer, nullptr);
		block->Memory = VK_NULL_HANDLE;
		block->Buffer = VK_NULL_HANDLE;
		return false;
	}

	void* mapped = nullptr;
	if (vkMapMemory(Device, block->Memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
	{
		vkFreeMemory(Device, block->Memory, nullptr);
		vkDestroyBuffer(Device, block->Buffer, nullptr);
		block->Memory = VK_NULL_HANDLE;
		block->Buffer = VK_NULL_HANDLE;
		return false;
	}
	block->MappedBase = static_cast<uint8_t*>(mapped);
	block->Cursor = 0;
	frame.Blocks.push_back(block);
	++TransientUploadStructuredBlockCount;
	TransientUploadStructuredBytesReserved += blockSize;

	return allocateFromBlock(block);
}
#endif

std::shared_ptr<VertexBuffer> VulkanBackend::CreateRWVertexBuffer(uint32_t size, uint32_t stride)
{
	if (size == 0)
		return nullptr;
#if !CORONA_HAS_VULKAN
	(void)stride;
	return nullptr;
#else
	VulkanBufferAllocation allocation{};
	allocation.Stride = stride;
	allocation.SizeInBytes = size;

	VkBufferUsageFlags bufferUsage =
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	if (bRayTracingEnabled)
	{
		bufferUsage |=
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	}
	if (!CreateBufferWithMemory(
		size,
		bufferUsage,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		bRayTracingEnabled,
		allocation.Buffer,
		allocation.Memory))
	{
		return nullptr;
	}

	auto vb = CreateTrackedVertexBufferHandle();
	vb->numVertices = stride > 0 ? static_cast<int>(size / stride) : 0;
	VertexBufferAllocations[vb.get()] = allocation;
	return vb;
#endif
}

std::shared_ptr<Buffer> VulkanBackend::CreateUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize)
{
	if (numElements == 0 || elementSize == 0)
		return nullptr;
#if !CORONA_HAS_VULKAN
	(void)elementSize;
	return nullptr;
#else
	// Dedicated VkBuffer + VkMemory per upload structured buffer so the
	// descriptor binding has offset=0 (avoids the pool sub-allocation
	// offset alignment + usage-flag traps that hit Adreno). The cost is
	// a few extra allocator round-trips at scene-creation time; runtime
	// updates still go through a persistent host-coherent mapping.
	const uint32_t size = numElements * elementSize;
	VkBuffer vkBuf = VK_NULL_HANDLE;
	VkDeviceMemory vkMem = VK_NULL_HANDLE;
	const VkBufferUsageFlags usage =
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (!CreateBufferWithMemory(
		size,
		usage,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		false,
		vkBuf,
		vkMem))
	{
		return nullptr;
	}
	// Persistent map so UpdateUploadStructuredBuffer can memcpy directly.
	void* mapped = nullptr;
	if (vkMapMemory(Device, vkMem, 0, size, 0, &mapped) != VK_SUCCESS)
	{
		vkDestroyBuffer(Device, vkBuf, nullptr);
		vkFreeMemory(Device, vkMem, nullptr);
		return nullptr;
	}
	// Store as a fake "PoolBlock" so the persistent mapping survives in
	// UpdateUploadStructuredBuffer without inventing a new field. The
	// block is dedicated so Offset stays 0 in BufferAllocations.
	auto block = std::make_shared<VulkanUploadHeapBlock>();
	block->OwningDevice = Device;
	block->Buffer = vkBuf;
	block->Memory = vkMem;
	block->MappedBase = static_cast<uint8_t*>(mapped);
	block->Capacity = size;
	block->Cursor = size;

	VulkanBufferAllocation allocation{};
	allocation.Buffer = vkBuf;
	allocation.Memory = vkMem;
	allocation.Stride = elementSize;
	allocation.SizeInBytes = size;
	allocation.Offset = 0;
	allocation.PoolBlock = block;

	auto buf = CreateTrackedBufferHandle();
	BufferAllocations[buf.get()] = allocation;
	return buf;
#endif
}

void VulkanBackend::UpdateUploadStructuredBuffer(Buffer* buffer, const void* srcData, uint32_t sizeInBytes)
{
#if !CORONA_HAS_VULKAN
	(void)buffer; (void)srcData; (void)sizeInBytes;
#else
	if (!buffer || !srcData || sizeInBytes == 0)
		return;
	auto it = BufferAllocations.find(buffer);
	if (it == BufferAllocations.end())
		return;
	auto& alloc = it->second;
	if (!alloc.PoolBlock || !alloc.PoolBlock->MappedBase)
		return;
	const uint32_t copyBytes = std::min<uint32_t>(sizeInBytes, alloc.SizeInBytes);
	std::memcpy(alloc.PoolBlock->MappedBase + alloc.Offset, srcData, copyBytes);
#endif
}

std::shared_ptr<Buffer> VulkanBackend::AllocateTransientUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize, const void* srcData)
{
	if (numElements == 0 || elementSize == 0)
		return nullptr;
#if !CORONA_HAS_VULKAN
	(void)srcData;
	return nullptr;
#else
	const uint64_t sizeInBytes64 = static_cast<uint64_t>(numElements) * static_cast<uint64_t>(elementSize);
	if (sizeInBytes64 > UINT32_MAX)
		return nullptr;

	VulkanBufferAllocation allocation{};
	if (!AllocateTransientUploadStructuredRange(
		CurrentFrameIndex,
		static_cast<VkDeviceSize>(sizeInBytes64),
		static_cast<VkDeviceSize>(elementSize),
		srcData,
		allocation))
	{
		return nullptr;
	}
	allocation.Stride = elementSize;

	auto buf = CreateTrackedBufferHandle();
	buf->Type = Buffer::STRUCTURED;
	buf->NumElements = numElements;
	buf->ElementSize = elementSize;
	BufferAllocations[buf.get()] = allocation;

	const size_t frameCount = std::max<size_t>(FrameContexts.size(), 1u);
	if (TransientUploadStructuredFrames.size() < frameCount)
		TransientUploadStructuredFrames.resize(frameCount);
	const uint32_t frameIndex = CurrentFrameIndex < TransientUploadStructuredFrames.size() ? CurrentFrameIndex : 0u;
	TransientUploadStructuredFrames[frameIndex].KeepAlive.push_back(buf);
	return buf;
#endif
}

std::shared_ptr<VertexBuffer> VulkanBackend::CreateUploadVertexBuffer(uint32_t size, uint32_t stride, const void* srcData)
{
	if (size == 0)
		return nullptr;
#if !CORONA_HAS_VULKAN
	(void)stride; (void)srcData;
	return nullptr;
#else
	VulkanBufferAllocation allocation{};
	const VkDeviceSize align = stride > 0 ? stride : 4;
	if (!AllocateUploadBufferRange(size, align, srcData, allocation))
		return nullptr;

	allocation.Stride = stride;

	auto vb = CreateTrackedVertexBufferHandle();
	vb->numVertices = stride > 0 ? static_cast<int>(size / stride) : 0;
	if (allocation.PoolBlock && allocation.PoolBlock->MappedBase)
	{
		vb->MappedCpu = allocation.PoolBlock->MappedBase + allocation.Offset;
		vb->MappedCapacityBytes = size;
	}
	VertexBufferAllocations[vb.get()] = allocation;
	return vb;
#endif
}

std::shared_ptr<IndexBuffer> VulkanBackend::CreateUploadIndexBuffer(EIndexFormat format, uint32_t size, const void* srcData)
{
	if (size == 0)
		return nullptr;
#if !CORONA_HAS_VULKAN
	(void)format; (void)srcData;
	return nullptr;
#else
	VulkanBufferAllocation allocation{};
	const VkDeviceSize align = format == EIndexFormat::U16 ? 2u : 4u;
	if (!AllocateUploadBufferRange(size, align, srcData, allocation))
		return nullptr;

	allocation.Stride = format == EIndexFormat::U16 ? 2u : 4u;

	auto ib = CreateTrackedIndexBufferHandle();
	ib->numIndices = format == EIndexFormat::U16 ? static_cast<int>(size / 2) : static_cast<int>(size / 4);
	if (allocation.PoolBlock && allocation.PoolBlock->MappedBase)
	{
		ib->MappedCpu = allocation.PoolBlock->MappedBase + allocation.Offset;
		ib->MappedCapacityBytes = size;
	}
	IndexBufferAllocations[ib.get()] = allocation;
	return ib;
#endif
}

void VulkanBackend::UpdateUploadVertexBuffer(VertexBuffer* buffer, const void* srcData, uint32_t sizeInBytes)
{
	if (!buffer || !buffer->MappedCpu || !srcData || sizeInBytes == 0)
		return;
	if (sizeInBytes > buffer->MappedCapacityBytes)
		sizeInBytes = buffer->MappedCapacityBytes;
	memcpy(buffer->MappedCpu, srcData, sizeInBytes);
}

void VulkanBackend::UpdateUploadIndexBuffer(IndexBuffer* buffer, const void* srcData, uint32_t sizeInBytes)
{
	if (!buffer || !buffer->MappedCpu || !srcData || sizeInBytes == 0)
		return;
	if (sizeInBytes > buffer->MappedCapacityBytes)
		sizeInBytes = buffer->MappedCapacityBytes;
	memcpy(buffer->MappedCpu, srcData, sizeInBytes);
}

std::shared_ptr<RTAS> VulkanBackend::CreateBLASForSkeletalMesh(Mesh* mesh)
{
#if !CORONA_HAS_VULKAN
	(void)mesh;
	return nullptr;
#else
	if (!mesh ||
		!mesh->bSkeletalSkinned ||
		!mesh->SkeletalOutputVb ||
		!mesh->Ib ||
		mesh->VertexStride == 0 ||
		mesh->SkeletalVertexCount == 0 ||
		mesh->Ib->numIndices < 3 ||
		!bRayTracingEnabled)
	{
		return nullptr;
	}

	auto vbIt = VertexBufferAllocations.find(mesh->SkeletalOutputVb.get());
	auto ibIt = IndexBufferAllocations.find(mesh->Ib.get());
	if (vbIt == VertexBufferAllocations.end() || ibIt == IndexBufferAllocations.end())
		return nullptr;

	const VkDeviceAddress vertexBaseAddress = GetBufferDeviceAddress(vbIt->second.Buffer);
	const VkDeviceAddress indexBaseAddress = GetBufferDeviceAddress(ibIt->second.Buffer);
	if (vertexBaseAddress == 0 || indexBaseAddress == 0)
		return nullptr;

	const VkDeviceSize vertexByteOffset =
		static_cast<VkDeviceSize>(mesh->SkeletalCharIndex) *
		static_cast<VkDeviceSize>(mesh->SkeletalVertexCount) *
		static_cast<VkDeviceSize>(mesh->VertexStride);

	auto rtas = std::make_shared<VulkanRTAS>();
	rtas->Owner = this;
	rtas->MeshPtr = mesh;
	rtas->PrimitiveCount = static_cast<uint32_t>(mesh->Ib->numIndices / 3);
	rtas->bAllowUpdate = true;
	rtas->bIsTopLevel = false;

	VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
	triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	triangles.vertexData.deviceAddress = vertexBaseAddress + vbIt->second.Offset + vertexByteOffset;
	triangles.vertexStride = mesh->VertexStride;
	triangles.maxVertex = mesh->SkeletalVertexCount;
	triangles.indexType = mesh->IndexFormat == EIndexFormat::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
	triangles.indexData.deviceAddress = indexBaseAddress + ibIt->second.Offset;

	VkAccelerationStructureGeometryKHR geometry{};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = mesh->bTransparent ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.triangles = triangles;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags =
		VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
		VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	const uint32_t primitiveCount = rtas->PrimitiveCount;
	VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
	sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHRFn(
		Device,
		VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		&buildInfo,
		&primitiveCount,
		&sizeInfo);
	if (sizeInfo.accelerationStructureSize == 0)
		return nullptr;

	if (!CreateBufferWithMemory(
		sizeInfo.accelerationStructureSize,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		true,
		rtas->ResultBuffer,
		rtas->ResultMemory))
	{
		return nullptr;
	}

	VkAccelerationStructureCreateInfoKHR asCreateInfo{};
	asCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	asCreateInfo.buffer = rtas->ResultBuffer;
	asCreateInfo.size = sizeInfo.accelerationStructureSize;
	asCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if (vkCreateAccelerationStructureKHRFn(Device, &asCreateInfo, nullptr, &rtas->AccelerationStructure) != VK_SUCCESS)
		return nullptr;

	const VkDeviceSize scratchSize = std::max(sizeInfo.buildScratchSize, sizeInfo.updateScratchSize);
	if (!CreateBufferWithMemory(
		scratchSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		true,
		rtas->ScratchBuffer,
		rtas->ScratchMemory))
	{
		return nullptr;
	}

	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.dstAccelerationStructure = rtas->AccelerationStructure;
	buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(rtas->ScratchBuffer);

	VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo{};
	buildRangeInfo.primitiveCount = primitiveCount;
	const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos[] = { &buildRangeInfo };

	auto recordBuild = [&](VkCommandBuffer commandBuffer)
	{
		vkCmdBuildAccelerationStructuresKHRFn(commandBuffer, 1, &buildInfo, buildRangeInfos);
		VkMemoryBarrier memoryBarrier{};
		memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memoryBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		memoryBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			0,
			1, &memoryBarrier,
			0, nullptr,
			0, nullptr);
	};

	if (ActiveCommandBuffer != VK_NULL_HANDLE && !bRenderPassActive)
	{
		recordBuild(ActiveCommandBuffer);
	}
	else
	{
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = CommandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(Device, &allocInfo, &commandBuffer) != VK_SUCCESS)
			return nullptr;

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(commandBuffer, &beginInfo);
		recordBuild(commandBuffer);
		vkEndCommandBuffer(commandBuffer);

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
		{
			vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
			return nullptr;
		}
		vkQueueWaitIdle(GraphicsQueue);
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
	}

	VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
	addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	addressInfo.accelerationStructure = rtas->AccelerationStructure;
	rtas->DeviceAddress = vkGetAccelerationStructureDeviceAddressKHRFn(Device, &addressInfo);
	if (rtas->DeviceAddress == 0)
		return nullptr;

	RayTracingAccelerationStructures.push_back(rtas);
	return rtas;
#endif
}

void VulkanBackend::RefitBLAS(RTAS* rtas, Mesh* mesh)
{
#if !CORONA_HAS_VULKAN
	(void)rtas;
	(void)mesh;
#else
	VulkanRTAS* as = dynamic_cast<VulkanRTAS*>(rtas);
	if (!as ||
		!as->bAllowUpdate ||
		as->bIsTopLevel ||
		as->AccelerationStructure == VK_NULL_HANDLE ||
		as->ScratchBuffer == VK_NULL_HANDLE ||
		!mesh ||
		!mesh->SkeletalOutputVb ||
		!mesh->Ib ||
		mesh->VertexStride == 0 ||
		mesh->SkeletalVertexCount == 0 ||
		mesh->Ib->numIndices < 3)
	{
		return;
	}

	auto vbIt = VertexBufferAllocations.find(mesh->SkeletalOutputVb.get());
	auto ibIt = IndexBufferAllocations.find(mesh->Ib.get());
	if (vbIt == VertexBufferAllocations.end() || ibIt == IndexBufferAllocations.end())
		return;

	const VkDeviceAddress vertexBaseAddress = GetBufferDeviceAddress(vbIt->second.Buffer);
	const VkDeviceAddress indexBaseAddress = GetBufferDeviceAddress(ibIt->second.Buffer);
	if (vertexBaseAddress == 0 || indexBaseAddress == 0)
		return;

	const VkDeviceSize vertexByteOffset =
		static_cast<VkDeviceSize>(mesh->SkeletalCharIndex) *
		static_cast<VkDeviceSize>(mesh->SkeletalVertexCount) *
		static_cast<VkDeviceSize>(mesh->VertexStride);

	VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
	triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	triangles.vertexData.deviceAddress = vertexBaseAddress + vbIt->second.Offset + vertexByteOffset;
	triangles.vertexStride = mesh->VertexStride;
	triangles.maxVertex = mesh->SkeletalVertexCount;
	triangles.indexType = mesh->IndexFormat == EIndexFormat::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
	triangles.indexData.deviceAddress = indexBaseAddress + ibIt->second.Offset;

	VkAccelerationStructureGeometryKHR geometry{};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = mesh->bTransparent ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.triangles = triangles;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags =
		VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
		VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
	buildInfo.srcAccelerationStructure = as->AccelerationStructure;
	buildInfo.dstAccelerationStructure = as->AccelerationStructure;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;
	buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(as->ScratchBuffer);

	uint32_t primitiveCount = as->PrimitiveCount;
	if (primitiveCount == 0)
		primitiveCount = static_cast<uint32_t>(mesh->Ib->numIndices / 3);
	VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo{};
	buildRangeInfo.primitiveCount = primitiveCount;
	const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos[] = { &buildRangeInfo };

	auto recordUpdate = [&](VkCommandBuffer commandBuffer)
	{
		vkCmdBuildAccelerationStructuresKHRFn(commandBuffer, 1, &buildInfo, buildRangeInfos);
		VkMemoryBarrier memoryBarrier{};
		memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memoryBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		memoryBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			0,
			1, &memoryBarrier,
			0, nullptr,
			0, nullptr);
	};

	if (ActiveCommandBuffer != VK_NULL_HANDLE && !bRenderPassActive)
	{
		recordUpdate(ActiveCommandBuffer);
		return;
	}

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = CommandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(Device, &allocInfo, &commandBuffer) != VK_SUCCESS)
		return;

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);
	recordUpdate(commandBuffer);
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS)
		vkQueueWaitIdle(GraphicsQueue);
	vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
#endif
}

std::shared_ptr<RTAS> VulkanBackend::CreateBLASForMesh(Mesh* mesh)
{
	if (!mesh)
		throw std::runtime_error("Vulkan CreateBLASForMesh requires a mesh.");
	if (!bRayTracingEnabled)
		throw std::runtime_error("Vulkan ray tracing is not enabled on this device.");

	auto vbIt = VertexBufferAllocations.find(mesh->Vb.get());
	auto ibIt = IndexBufferAllocations.find(mesh->Ib.get());
	if (vbIt == VertexBufferAllocations.end() || ibIt == IndexBufferAllocations.end())
		throw std::runtime_error("Vulkan BLAS creation requires uploaded vertex and index buffers.");

	auto rtas = std::make_shared<VulkanRTAS>();
	rtas->Owner = this;
	rtas->MeshPtr = mesh;
	rtas->bIsTopLevel = false;

	VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
	triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	triangles.vertexData.deviceAddress = GetBufferDeviceAddress(vbIt->second.Buffer);
	triangles.vertexStride = mesh->VertexStride;
	triangles.maxVertex = static_cast<uint32_t>(mesh->Vb->numVertices);
	triangles.indexType = mesh->IndexFormat == EIndexFormat::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
	triangles.indexData.deviceAddress = GetBufferDeviceAddress(ibIt->second.Buffer);

	VkAccelerationStructureGeometryKHR geometry{};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	// Keep any-hit enabled only for alpha-tested meshes; opaque geometry can skip it during traversal.
	geometry.flags = mesh->bTransparent ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.triangles = triangles;

	const uint32_t primitiveCount = static_cast<uint32_t>(mesh->Ib->numIndices / 3);
	rtas->PrimitiveCount = primitiveCount;
	VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
	sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHRFn(
		Device,
		VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		&buildInfo,
		&primitiveCount,
		&sizeInfo);

	if (!CreateBufferWithMemory(
		sizeInfo.accelerationStructureSize,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		true,
		rtas->ResultBuffer,
		rtas->ResultMemory))
	{
		throw std::runtime_error("Failed to create Vulkan BLAS result buffer.");
	}

	VkAccelerationStructureCreateInfoKHR asCreateInfo{};
	asCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	asCreateInfo.buffer = rtas->ResultBuffer;
	asCreateInfo.size = sizeInfo.accelerationStructureSize;
	asCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if (vkCreateAccelerationStructureKHRFn(Device, &asCreateInfo, nullptr, &rtas->AccelerationStructure) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan BLAS.");

	if (!CreateBufferWithMemory(
		sizeInfo.buildScratchSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		true,
		rtas->ScratchBuffer,
		rtas->ScratchMemory))
	{
		throw std::runtime_error("Failed to create Vulkan BLAS scratch buffer.");
	}

	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.dstAccelerationStructure = rtas->AccelerationStructure;
	buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(rtas->ScratchBuffer);

	VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo{};
	buildRangeInfo.primitiveCount = primitiveCount;
	const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos[] = { &buildRangeInfo };

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = CommandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(Device, &allocInfo, &commandBuffer) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate Vulkan BLAS command buffer.");

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);
	vkCmdBuildAccelerationStructuresKHRFn(commandBuffer, 1, &buildInfo, buildRangeInfos);

	VkMemoryBarrier memoryBarrier{};
	memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memoryBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	memoryBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier(
		commandBuffer,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		0,
		1, &memoryBarrier,
		0, nullptr,
		0, nullptr);
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
		throw std::runtime_error("Failed to submit Vulkan BLAS build.");
	vkQueueWaitIdle(GraphicsQueue);
	vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);

	VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
	addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	addressInfo.accelerationStructure = rtas->AccelerationStructure;
	rtas->DeviceAddress = vkGetAccelerationStructureDeviceAddressKHRFn(Device, &addressInfo);
	RayTracingAccelerationStructures.push_back(rtas);
	return rtas;
}

bool VulkanBackend::WriteVulkanTLASInstanceDescs(VulkanRTAS* rtas, const std::vector<RTInstanceDesc>& instancesDesc)
{
#if !CORONA_HAS_VULKAN
	(void)rtas;
	(void)instancesDesc;
	return false;
#else
	if (!rtas || rtas->InstanceMemory == VK_NULL_HANDLE || instancesDesc.size() > static_cast<size_t>(UINT32_MAX))
		return false;

	const uint32_t instanceCount = static_cast<uint32_t>(instancesDesc.size());
	const VkDeviceSize instanceBufferSize = sizeof(VkAccelerationStructureInstanceKHR) * static_cast<VkDeviceSize>(instanceCount);
	if (instanceBufferSize == 0)
		return false;

	std::vector<VkAccelerationStructureInstanceKHR> instances;
	instances.reserve(instanceCount);
	for (uint32_t i = 0; i < instanceCount; ++i)
	{
		VulkanRTAS* blas = dynamic_cast<VulkanRTAS*>(instancesDesc[i].BottomLevelAS.get());
		if (!blas || blas->DeviceAddress == 0)
			return false;

		const glm::mat4x4 mat = glm::transpose(instancesDesc[i].Transform);
		VkAccelerationStructureInstanceKHR instance{};
		instance.instanceCustomIndex = i;
		instance.mask = 0xFF;
		instance.instanceShaderBindingTableRecordOffset = 0;
		instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		instance.accelerationStructureReference = blas->DeviceAddress;
		instance.transform.matrix[0][0] = mat[0][0];
		instance.transform.matrix[0][1] = mat[0][1];
		instance.transform.matrix[0][2] = mat[0][2];
		instance.transform.matrix[0][3] = mat[0][3];
		instance.transform.matrix[1][0] = mat[1][0];
		instance.transform.matrix[1][1] = mat[1][1];
		instance.transform.matrix[1][2] = mat[1][2];
		instance.transform.matrix[1][3] = mat[1][3];
		instance.transform.matrix[2][0] = mat[2][0];
		instance.transform.matrix[2][1] = mat[2][1];
		instance.transform.matrix[2][2] = mat[2][2];
		instance.transform.matrix[2][3] = mat[2][3];
		instances.push_back(instance);
	}

	void* mappedData = nullptr;
	if (vkMapMemory(Device, rtas->InstanceMemory, 0, instanceBufferSize, 0, &mappedData) != VK_SUCCESS)
		return false;
	std::memcpy(mappedData, instances.data(), static_cast<size_t>(instanceBufferSize));
	vkUnmapMemory(Device, rtas->InstanceMemory);
	return true;
#endif
}

std::shared_ptr<RTAS> VulkanBackend::CreateTLAS(const std::vector<RTInstanceDesc>& instancesDesc)
{
	if (!bRayTracingEnabled)
		throw std::runtime_error("Vulkan ray tracing is not enabled on this device.");

	auto rtas = std::make_shared<VulkanRTAS>();
	rtas->Owner = this;
	rtas->bIsTopLevel = true;
	rtas->bAllowUpdate = true;
	rtas->NumInstances = static_cast<uint32_t>(instancesDesc.size());

	std::vector<VkAccelerationStructureInstanceKHR> instances;
	instances.reserve(instancesDesc.size());
	for (size_t i = 0; i < instancesDesc.size(); ++i)
	{
		VulkanRTAS* blas = dynamic_cast<VulkanRTAS*>(instancesDesc[i].BottomLevelAS.get());
		if (!blas || !blas->MeshPtr || blas->DeviceAddress == 0)
			throw std::runtime_error("Vulkan TLAS creation requires valid Vulkan BLAS instances.");

		const glm::mat4x4 mat = glm::transpose(instancesDesc[i].Transform);
		VkAccelerationStructureInstanceKHR instance{};
		instance.instanceCustomIndex = static_cast<uint32_t>(i);
		instance.mask = 0xFF;
		// Vulkan currently stores per-instance hit resources in descriptor arrays indexed by
		// InstanceCustomIndexKHR. The hit SBT itself only has one record per hit group, so
		// per-instance SBT offsets would point past the hit table and produce undefined hits.
		instance.instanceShaderBindingTableRecordOffset = 0;
		instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		instance.accelerationStructureReference = blas->DeviceAddress;
		instance.transform.matrix[0][0] = mat[0][0];
		instance.transform.matrix[0][1] = mat[0][1];
		instance.transform.matrix[0][2] = mat[0][2];
		instance.transform.matrix[0][3] = mat[0][3];
		instance.transform.matrix[1][0] = mat[1][0];
		instance.transform.matrix[1][1] = mat[1][1];
		instance.transform.matrix[1][2] = mat[1][2];
		instance.transform.matrix[1][3] = mat[1][3];
		instance.transform.matrix[2][0] = mat[2][0];
		instance.transform.matrix[2][1] = mat[2][1];
		instance.transform.matrix[2][2] = mat[2][2];
		instance.transform.matrix[2][3] = mat[2][3];
		instances.push_back(instance);
	}

	if (!instances.empty())
	{
		const VkDeviceSize instanceBufferSize = sizeof(VkAccelerationStructureInstanceKHR) * instances.size();
		if (!CreateBufferWithMemory(
			instanceBufferSize,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			true,
			rtas->InstanceBuffer,
			rtas->InstanceMemory))
		{
			throw std::runtime_error("Failed to create Vulkan TLAS instance buffer.");
		}

		void* mappedData = nullptr;
		if (vkMapMemory(Device, rtas->InstanceMemory, 0, instanceBufferSize, 0, &mappedData) != VK_SUCCESS)
			throw std::runtime_error("Failed to map Vulkan TLAS instance buffer.");
		std::memcpy(mappedData, instances.data(), static_cast<size_t>(instanceBufferSize));
		vkUnmapMemory(Device, rtas->InstanceMemory);
	}

	VkAccelerationStructureGeometryInstancesDataKHR instancesData{};
	instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	instancesData.arrayOfPointers = VK_FALSE;
	instancesData.data.deviceAddress = GetBufferDeviceAddress(rtas->InstanceBuffer);

	VkAccelerationStructureGeometryKHR geometry{};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.geometry.instances = instancesData;

	const uint32_t primitiveCount = static_cast<uint32_t>(instances.size());
	rtas->PrimitiveCount = primitiveCount;
	VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	buildInfo.flags =
		VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
		VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
	sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHRFn(
		Device,
		VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		&buildInfo,
		&primitiveCount,
		&sizeInfo);

	if (!CreateBufferWithMemory(
		sizeInfo.accelerationStructureSize,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		true,
		rtas->ResultBuffer,
		rtas->ResultMemory))
	{
		throw std::runtime_error("Failed to create Vulkan TLAS result buffer.");
	}

	VkAccelerationStructureCreateInfoKHR asCreateInfo{};
	asCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	asCreateInfo.buffer = rtas->ResultBuffer;
	asCreateInfo.size = sizeInfo.accelerationStructureSize;
	asCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	if (vkCreateAccelerationStructureKHRFn(Device, &asCreateInfo, nullptr, &rtas->AccelerationStructure) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan TLAS.");

	const VkDeviceSize scratchSize = std::max(sizeInfo.buildScratchSize, sizeInfo.updateScratchSize);
	if (!CreateBufferWithMemory(
		scratchSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		true,
		rtas->ScratchBuffer,
		rtas->ScratchMemory))
	{
		throw std::runtime_error("Failed to create Vulkan TLAS scratch buffer.");
	}

	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.dstAccelerationStructure = rtas->AccelerationStructure;
	buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(rtas->ScratchBuffer);

	VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo{};
	buildRangeInfo.primitiveCount = primitiveCount;
	const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos[] = { &buildRangeInfo };

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = CommandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(Device, &allocInfo, &commandBuffer) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate Vulkan TLAS command buffer.");

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);
	vkCmdBuildAccelerationStructuresKHRFn(commandBuffer, 1, &buildInfo, buildRangeInfos);

	VkMemoryBarrier memoryBarrier{};
	memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memoryBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	memoryBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier(
		commandBuffer,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		0,
		1, &memoryBarrier,
		0, nullptr,
		0, nullptr);
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
		throw std::runtime_error("Failed to submit Vulkan TLAS build.");
	vkQueueWaitIdle(GraphicsQueue);
	vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);

	VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
	addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	addressInfo.accelerationStructure = rtas->AccelerationStructure;
	rtas->DeviceAddress = vkGetAccelerationStructureDeviceAddressKHRFn(Device, &addressInfo);
	RayTracingAccelerationStructures.push_back(rtas);
	return rtas;
}

bool VulkanBackend::UpdateTLAS(const std::shared_ptr<RTAS>& topLevelAS, const std::vector<RTInstanceDesc>& instances)
{
#if !CORONA_HAS_VULKAN
	(void)topLevelAS;
	(void)instances;
	return false;
#else
	VulkanRTAS* as = dynamic_cast<VulkanRTAS*>(topLevelAS.get());
	if (!as ||
		!as->bIsTopLevel ||
		!as->bAllowUpdate ||
		as->AccelerationStructure == VK_NULL_HANDLE ||
		as->ScratchBuffer == VK_NULL_HANDLE ||
		as->InstanceBuffer == VK_NULL_HANDLE ||
		as->NumInstances == 0 ||
		instances.empty() ||
		instances.size() != static_cast<size_t>(as->NumInstances))
	{
		return false;
	}
	if (!WriteVulkanTLASInstanceDescs(as, instances))
		return false;

	VkAccelerationStructureGeometryInstancesDataKHR instancesData{};
	instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	instancesData.arrayOfPointers = VK_FALSE;
	instancesData.data.deviceAddress = GetBufferDeviceAddress(as->InstanceBuffer);
	if (instancesData.data.deviceAddress == 0)
		return false;

	VkAccelerationStructureGeometryKHR geometry{};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.geometry.instances = instancesData;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	buildInfo.flags =
		VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
		VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
	buildInfo.srcAccelerationStructure = as->AccelerationStructure;
	buildInfo.dstAccelerationStructure = as->AccelerationStructure;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;
	buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(as->ScratchBuffer);
	if (buildInfo.scratchData.deviceAddress == 0)
		return false;

	VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo{};
	buildRangeInfo.primitiveCount = as->NumInstances;
	const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos[] = { &buildRangeInfo };

	auto recordUpdate = [&](VkCommandBuffer commandBuffer)
	{
		vkCmdBuildAccelerationStructuresKHRFn(commandBuffer, 1, &buildInfo, buildRangeInfos);
		VkMemoryBarrier memoryBarrier{};
		memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memoryBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		memoryBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			0,
			1, &memoryBarrier,
			0, nullptr,
			0, nullptr);
	};

	if (ActiveCommandBuffer != VK_NULL_HANDLE && !bRenderPassActive)
	{
		recordUpdate(ActiveCommandBuffer);
		return true;
	}

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = CommandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(Device, &allocInfo, &commandBuffer) != VK_SUCCESS)
		return false;

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		return false;
	}
	recordUpdate(commandBuffer);
	if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		return false;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	const bool bSubmitted = vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS;
	if (bSubmitted)
		vkQueueWaitIdle(GraphicsQueue);
	vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
	return bSubmitted;
#endif
}

std::shared_ptr<RTPipelineStateObject> VulkanBackend::CreateRTPipelineStateObject()
{
#if !CORONA_HAS_VULKAN
	ThrowNotImplemented(__FUNCTION__);
#else
	if (!SupportsRayTracing())
		return nullptr;
	auto pso = std::make_shared<VulkanRTPipelineStateObject>();
	pso->Owner = this;
	RayTracingPipelines.push_back(pso);
	return pso;
#endif
}

std::shared_ptr<ComputePipelineStateObject> VulkanBackend::CreateComputePipelineStateObject()
{
#if !CORONA_HAS_VULKAN
	ThrowNotImplemented(__FUNCTION__);
#else
	auto pso = std::make_shared<VulkanComputePipelineStateObject>();
	pso->Owner = this;
	ComputePipelines.push_back(pso);
	return pso;
#endif
}
ShaderBytecode VulkanBackend::CreateShader(const std::wstring& fileName, const std::string& entryPoint, const std::string& target)
{
#if !CORONA_HAS_VULKAN
	(void)fileName;
	(void)entryPoint;
	(void)target;
	return {};
#else
	try
	{
		std::filesystem::path spirvPath;
		const std::filesystem::path inputPath = NormalizeShaderPath(fileName);
		if (inputPath.extension() == L".spv")
		{
			spirvPath = inputPath;
		}
		else
		{
			const std::wstring stem = inputPath.stem().wstring();
			if (target.rfind("vs_", 0) == 0)
				spirvPath = ResolveVulkanGraphicsVertexSpirvPath(stem, entryPoint);
			else if (target.rfind("ps_", 0) == 0)
				spirvPath = ResolveVulkanGraphicsFragmentSpirvPath(stem, entryPoint);
			else if (target.rfind("cs_", 0) == 0)
				spirvPath = ResolveVulkanComputeSpirvPath(stem, entryPoint);
			else if (target.rfind("lib_", 0) == 0)
				spirvPath = ResolveVulkanSpirvPath(stem + L"Vulkan.rt.spv");
			else
				return {};
		}

		if (!std::filesystem::exists(spirvPath))
			return {};

		const std::vector<uint32_t> spirv = LoadSpirvFile(spirvPath);
		ShaderBytecode result;
		result.Data.resize(spirv.size() * sizeof(uint32_t));
		std::memcpy(result.Data.data(), spirv.data(), result.Data.size());
		return result;
	}
	catch (...)
	{
		return {};
	}
#endif
}
void VulkanBackend::ResetDynamicResources()
{
#if CORONA_HAS_VULKAN
	if (Device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(Device);
	for (const std::shared_ptr<VulkanRTPipelineStateObject>& pipeline : RayTracingPipelines)
	{
		if (pipeline)
			pipeline->ReleaseTempUniformBuffers();
	}
	for (const std::shared_ptr<VulkanComputePipelineStateObject>& pipeline : ComputePipelines)
	{
		if (pipeline)
			pipeline->ReleaseTempUniformBuffers();
	}
	TransientUniformFrameOffset = 0;
	PendingTextureClearColors.clear();
	PendingDepthClearValues.clear();
#endif
}
void VulkanBackend::CreateSwapChainForWindow(WindowHandle window, uint32_t width, uint32_t height, ETextureFormat format)
{
	(void)format;
#if !CORONA_HAS_VULKAN
	(void)window;
	(void)width;
	(void)height;
	ThrowNotImplemented(__FUNCTION__);
#else
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] begin");
	DestroyWindowContext();

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Corona Vulkan";
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "Corona";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_1;

	std::error_code resetValidationLogError;
	std::filesystem::remove(RuntimePaths::LogFile(L"vulkan_validation.log"), resetValidationLogError);
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after reset validation log");

	const char* platformSurfaceExtension = GetVulkanPlatformSurfaceExtension();
	if (!platformSurfaceExtension)
		throw std::runtime_error("Vulkan window surfaces are not implemented for this Corona platform.");
	std::vector<const char*> instanceExtensions = { VK_KHR_SURFACE_EXTENSION_NAME, platformSurfaceExtension };
	std::vector<const char*> instanceLayers;
	VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo{};
	bValidationLayersEnabled = false;

	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] before validation layer query");
	const bool bEnableValidation = IsVulkanValidationEnabled();
	const bool bHasDebugUtils = HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	if (bHasDebugUtils)
		instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	if (bEnableValidation && HasInstanceLayer("VK_LAYER_KHRONOS_validation") && bHasDebugUtils)
	{
		instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
		bValidationLayersEnabled = true;

		debugMessengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		debugMessengerInfo.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debugMessengerInfo.messageType =
			VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debugMessengerInfo.pfnUserCallback = VulkanDebugCallback;
	}
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after validation layer query");

	VkInstanceCreateInfo instanceInfo{};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &appInfo;
	instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
	instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
	instanceInfo.enabledLayerCount = static_cast<uint32_t>(instanceLayers.size());
	instanceInfo.ppEnabledLayerNames = instanceLayers.empty() ? nullptr : instanceLayers.data();
	instanceInfo.pNext = bValidationLayersEnabled ? &debugMessengerInfo : nullptr;
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] before vkCreateInstance");
	if (vkCreateInstance(&instanceInfo, nullptr, &Instance) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan instance for window rendering.");
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after vkCreateInstance");

	if (bValidationLayersEnabled)
	{
		const VkResult debugResult = CreateDebugUtilsMessengerEXT(Instance, &debugMessengerInfo, nullptr, &DebugMessenger);
		if (debugResult != VK_SUCCESS)
		{
			AppendVulkanValidationLog("Failed to create Vulkan debug messenger.");
			DebugMessenger = VK_NULL_HANDLE;
		}
		else
		{
			AppendVulkanValidationLog("Vulkan validation layers enabled.");
		}
	}

#if CORONA_PLATFORM_IS_WINDOWS
	HWND hwnd = static_cast<HWND>(window.PlatformHandle);
	VkWin32SurfaceCreateInfoKHR surfaceInfo{};
	surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	surfaceInfo.hinstance = GetModuleHandleW(nullptr);
	surfaceInfo.hwnd = hwnd;
	if (vkCreateWin32SurfaceKHR(Instance, &surfaceInfo, nullptr, &Surface) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan Win32 surface.");
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after vkCreateWin32SurfaceKHR");
#elif CORONA_PLATFORM_IS_ANDROID
	ANativeWindow* nativeWindow = static_cast<ANativeWindow*>(window.PlatformHandle);
	VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
	surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
	surfaceInfo.window = nativeWindow;
	if (vkCreateAndroidSurfaceKHR(Instance, &surfaceInfo, nullptr, &Surface) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan Android surface.");
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after vkCreateAndroidSurfaceKHR");
#else
	(void)window;
	throw std::runtime_error("Vulkan surface creation is not implemented for this Corona platform.");
#endif

	uint32_t physicalDeviceCount = 0;
	vkEnumeratePhysicalDevices(Instance, &physicalDeviceCount, nullptr);
	if (physicalDeviceCount == 0)
		throw std::runtime_error("No Vulkan physical devices were found.");

	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	vkEnumeratePhysicalDevices(Instance, &physicalDeviceCount, physicalDevices.data());
	for (VkPhysicalDevice physicalDevice : physicalDevices)
	{
		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

		for (uint32_t queueFamilyIndex = 0; queueFamilyIndex < queueFamilyCount; ++queueFamilyIndex)
		{
			VkBool32 supportsPresent = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, Surface, &supportsPresent);
			if ((queueFamilies[queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && supportsPresent == VK_TRUE)
			{
				PhysicalDevice = physicalDevice;
				GraphicsQueueFamilyIndex = queueFamilyIndex;
				PresentQueueFamilyIndex = queueFamilyIndex;
				TimestampValidBits = queueFamilies[queueFamilyIndex].timestampValidBits;
				break;
			}
		}

		if (PhysicalDevice != VK_NULL_HANDLE)
			break;
	}

	if (PhysicalDevice == VK_NULL_HANDLE)
		throw std::runtime_error("Failed to find a Vulkan device with graphics and present support.");
	VkPhysicalDeviceProperties physicalDeviceProperties{};
	vkGetPhysicalDeviceProperties(PhysicalDevice, &physicalDeviceProperties);
	TimestampPeriodNs = physicalDeviceProperties.limits.timestampPeriod;
	UniformBufferAlignment = std::max<VkDeviceSize>(256, physicalDeviceProperties.limits.minUniformBufferOffsetAlignment);
	StorageBufferAlignment = std::max<VkDeviceSize>(4, physicalDeviceProperties.limits.minStorageBufferOffsetAlignment);
	MaxUniformBufferRange = physicalDeviceProperties.limits.maxUniformBufferRange;
	bDescriptorIndexingEnabled = false;
	bBindlessTextureTableReady = false;
	bBindlessBufferTableReady = false;
	MaxVulkanBindlessTextureSlots = 0;
	MaxVulkanBindlessBufferSlots = 0;
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after select physical device");
	AppendVulkanRuntimeTraceBackend(
		L"[VulkanBackend::CreateSwapChainForWindow] physical device=" +
		Utf8ToWide(std::string(physicalDeviceProperties.deviceName)) +
		L", maxColorAttachments=" +
		std::to_wstring(physicalDeviceProperties.limits.maxColorAttachments) +
		L", maxFragmentOutputAttachments=" +
		std::to_wstring(physicalDeviceProperties.limits.maxFragmentOutputAttachments) +
		L", maxFragmentCombinedOutputResources=" +
		std::to_wstring(physicalDeviceProperties.limits.maxFragmentCombinedOutputResources));
	AppendVulkanRuntimeTraceBackend(
		L"[VulkanBackend::CreateSwapChainForWindow] timestamp valid bits=" +
		std::to_wstring(TimestampValidBits) +
		L", period_ns=" +
		std::to_wstring(TimestampPeriodNs));

	uint32_t deviceExtensionCount = 0;
	vkEnumerateDeviceExtensionProperties(PhysicalDevice, nullptr, &deviceExtensionCount, nullptr);
	std::vector<VkExtensionProperties> deviceExtensionsAvailable(deviceExtensionCount);
	vkEnumerateDeviceExtensionProperties(PhysicalDevice, nullptr, &deviceExtensionCount, deviceExtensionsAvailable.data());

	auto hasDeviceExtension = [&](const char* extensionName) -> bool
	{
		for (const VkExtensionProperties& extension : deviceExtensionsAvailable)
		{
			if (std::strcmp(extension.extensionName, extensionName) == 0)
				return true;
		}
		return false;
	};

	const bool bHasBufferDeviceAddressExtension = hasDeviceExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
	const bool bHasDeferredHostOperationsExtension = hasDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
	const bool bHasAccelerationStructureExtension = hasDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
	const bool bHasRayTracingPipelineExtension = hasDeviceExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
	const bool bHasRayQueryExtension = hasDeviceExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
	const bool bHasDescriptorIndexingExtension = hasDeviceExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
	bRayTracingExtensionSupport =
		bHasBufferDeviceAddressExtension &&
		bHasDeferredHostOperationsExtension &&
		bHasAccelerationStructureExtension &&
		bHasRayTracingPipelineExtension &&
		bHasRayQueryExtension &&
		bHasDescriptorIndexingExtension;

	VkPhysicalDeviceFeatures physicalDeviceFeatures{};
	vkGetPhysicalDeviceFeatures(PhysicalDevice, &physicalDeviceFeatures);
	bSamplerAnisotropySupported = physicalDeviceFeatures.samplerAnisotropy == VK_TRUE;
	bool bRayTracingIndirectFeatureSupport = false;
	VkPhysicalDeviceDescriptorIndexingProperties descriptorIndexingProperties{};
	descriptorIndexingProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
	VkPhysicalDeviceProperties2 descriptorProperties2{};
	descriptorProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	descriptorProperties2.pNext = &descriptorIndexingProperties;
	vkGetPhysicalDeviceProperties2(PhysicalDevice, &descriptorProperties2);

	if (bRayTracingExtensionSupport)
	{
		VkPhysicalDeviceFeatures2 features2{};
		features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{};
		bufferDeviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
		VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
		accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
		VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures{};
		rayTracingPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
		VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
		rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
		VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};
		descriptorIndexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;

		features2.pNext = &bufferDeviceAddressFeatures;
		bufferDeviceAddressFeatures.pNext = &accelerationStructureFeatures;
		accelerationStructureFeatures.pNext = &rayTracingPipelineFeatures;
		rayTracingPipelineFeatures.pNext = &rayQueryFeatures;
		rayQueryFeatures.pNext = &descriptorIndexingFeatures;

		vkGetPhysicalDeviceFeatures2(PhysicalDevice, &features2);

		bRayTracingFeatureSupport =
			bufferDeviceAddressFeatures.bufferDeviceAddress == VK_TRUE &&
			accelerationStructureFeatures.accelerationStructure == VK_TRUE &&
			rayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE &&
			rayQueryFeatures.rayQuery == VK_TRUE &&
			descriptorIndexingFeatures.runtimeDescriptorArray == VK_TRUE &&
			descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
			descriptorIndexingFeatures.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE &&
			descriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
			descriptorIndexingFeatures.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE &&
			descriptorIndexingFeatures.descriptorBindingPartiallyBound == VK_TRUE;
		if (bRayTracingFeatureSupport)
		{
			const uint32_t sampledImageLimit = descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSampledImages != 0
				? descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSampledImages
				: physicalDeviceProperties.limits.maxDescriptorSetSampledImages;
			const uint32_t storageBufferLimit = descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindStorageBuffers != 0
				? descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindStorageBuffers
				: physicalDeviceProperties.limits.maxDescriptorSetStorageBuffers;
			MaxVulkanBindlessTextureSlots = std::min(kMaxRequestedVulkanBindlessSlots, sampledImageLimit);
			MaxVulkanBindlessBufferSlots = std::min(kMaxRequestedVulkanBindlessSlots, storageBufferLimit);
			bRayTracingIndirectFeatureSupport = rayTracingPipelineFeatures.rayTracingPipelineTraceRaysIndirect == VK_TRUE;
			bDescriptorIndexingEnabled =
				MaxVulkanBindlessTextureSlots > 0 &&
				MaxVulkanBindlessBufferSlots > 0;
		}
	}

	AppendVulkanRuntimeTraceBackend(
		L"[VulkanBackend::CreateSwapChainForWindow] ray tracing support ext=" + std::to_wstring(bRayTracingExtensionSupport ? 1 : 0) +
		L", feat=" + std::to_wstring(bRayTracingFeatureSupport ? 1 : 0) +
		L", descriptorIndexing=" + std::to_wstring(bDescriptorIndexingEnabled ? 1 : 0) +
		L", maxBindlessTextures=" + std::to_wstring(MaxVulkanBindlessTextureSlots) +
		L", maxBindlessBuffers=" + std::to_wstring(MaxVulkanBindlessBufferSlots));

	const bool bCanEnableRayTracing = bRayTracingExtensionSupport && bRayTracingFeatureSupport && bDescriptorIndexingEnabled;

	const float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueCreateInfo{};
	queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueCreateInfo.queueFamilyIndex = GraphicsQueueFamilyIndex;
	queueCreateInfo.queueCount = 1;
	queueCreateInfo.pQueuePriorities = &queuePriority;

	std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{};
	VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
	VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures{};
	VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
	VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};
	void* deviceFeatureChain = nullptr;
	if (bCanEnableRayTracing)
	{
		deviceExtensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
		deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
		deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
		deviceExtensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
		deviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
		deviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);

		bufferDeviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
		bufferDeviceAddressFeatures.bufferDeviceAddress = VK_TRUE;
		accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
		accelerationStructureFeatures.accelerationStructure = VK_TRUE;
		rayTracingPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
		rayTracingPipelineFeatures.rayTracingPipeline = VK_TRUE;
		rayTracingPipelineFeatures.rayTracingPipelineTraceRaysIndirect = bRayTracingIndirectFeatureSupport ? VK_TRUE : VK_FALSE;
		bRayTracingIndirectEnabled = bRayTracingIndirectFeatureSupport;
		rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
		rayQueryFeatures.rayQuery = VK_TRUE;
		descriptorIndexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
		descriptorIndexingFeatures.runtimeDescriptorArray = VK_TRUE;
		descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		descriptorIndexingFeatures.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
		descriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
		descriptorIndexingFeatures.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
		descriptorIndexingFeatures.descriptorBindingPartiallyBound = VK_TRUE;

		bufferDeviceAddressFeatures.pNext = &accelerationStructureFeatures;
		accelerationStructureFeatures.pNext = &rayTracingPipelineFeatures;
		rayTracingPipelineFeatures.pNext = &rayQueryFeatures;
		rayQueryFeatures.pNext = &descriptorIndexingFeatures;
		deviceFeatureChain = &bufferDeviceAddressFeatures;
	}

	VkDeviceCreateInfo deviceCreateInfo{};
	deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceCreateInfo.queueCreateInfoCount = 1;
	deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
	deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
	deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();
	deviceCreateInfo.pEnabledFeatures = &physicalDeviceFeatures;
	deviceCreateInfo.pNext = deviceFeatureChain;
	if (vkCreateDevice(PhysicalDevice, &deviceCreateInfo, nullptr, &Device) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan device for window rendering.");
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after vkCreateDevice");
	vkCmdInsertDebugUtilsLabelEXTFn = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
		vkGetDeviceProcAddr(Device, "vkCmdInsertDebugUtilsLabelEXT"));
	vkCmdBeginDebugUtilsLabelEXTFn = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
		vkGetDeviceProcAddr(Device, "vkCmdBeginDebugUtilsLabelEXT"));
	vkCmdEndDebugUtilsLabelEXTFn = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
		vkGetDeviceProcAddr(Device, "vkCmdEndDebugUtilsLabelEXT"));

	if (bCanEnableRayTracing)
		LoadRayTracingFunctionPointers();
	AppendVulkanRuntimeTraceBackend(
		L"[VulkanBackend::CreateSwapChainForWindow] ray tracing enabled=" + std::to_wstring(bRayTracingEnabled ? 1 : 0));
	if (bRayTracingEnabled && bDescriptorIndexingEnabled)
	{
		if (!InitializeBindlessDescriptorTables())
		{
			AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] disabling ray tracing because bindless descriptor tables failed");
			DestroyBindlessDescriptorTables();
			bRayTracingEnabled = false;
		}
	}

	vkGetDeviceQueue(Device, GraphicsQueueFamilyIndex, 0, &GraphicsQueue);
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after vkGetDeviceQueue");

	VkCommandPoolCreateInfo commandPoolCreateInfo{};
	commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolCreateInfo.queueFamilyIndex = GraphicsQueueFamilyIndex;
	commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	if (vkCreateCommandPool(Device, &commandPoolCreateInfo, nullptr, &CommandPool) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan command pool for window rendering.");
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after vkCreateCommandPool");

	RecreateSwapchain(width, height);
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after RecreateSwapchain");
#endif
}
std::shared_ptr<Texture> VulkanBackend::GetSwapChainTexture(uint32_t bufferIndex)
{
#if !CORONA_HAS_VULKAN
	(void)bufferIndex;
	return nullptr;
#else
	if (bufferIndex >= SwapchainImages.size() || bufferIndex >= SwapchainImageViews.size())
		return nullptr;
	if (SwapchainWrappedTextures.size() <= bufferIndex)
		SwapchainWrappedTextures.resize(bufferIndex + 1);
	if (SwapchainWrappedTextures[bufferIndex])
		return SwapchainWrappedTextures[bufferIndex];

	auto texture = CreateTrackedTextureHandle();
	texture->Width = SwapchainExtent.width;
	texture->Height = SwapchainExtent.height;
	texture->MipLevels = 1;
	texture->Format = FromVkFormat(SwapchainFormat);
	texture->Usage = TextureUsage_RenderTarget;

	VulkanTextureAllocation allocation{};
	allocation.Image = SwapchainImages[bufferIndex];
	allocation.ImageView = SwapchainImageViews[bufferIndex];
	allocation.Memory = VK_NULL_HANDLE;
	allocation.Format = SwapchainFormat;
	allocation.Width = SwapchainExtent.width;
	allocation.Height = SwapchainExtent.height;
	allocation.Depth = 1;
	allocation.Usage = TextureUsage_RenderTarget;
	allocation.CurrentLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	allocation.bOwnsImage = false;
	allocation.bOwnsMemory = false;
	allocation.bOwnsImageView = false;
	TextureAllocations[texture.get()] = allocation;
	SwapchainWrappedTextures[bufferIndex] = texture;
	return texture;
#endif
}
bool VulkanBackend::CaptureTexture(Texture* source, CapturedImage& captured, EResourceState beforeState)
{
#if !CORONA_HAS_VULKAN
	(void)source;
	(void)captured;
	(void)beforeState;
	return false;
#else
	captured = {};
	if (!source || Device == VK_NULL_HANDLE || CommandPool == VK_NULL_HANDLE || bFrameActive)
		return false;

	auto textureIt = TextureAllocations.find(source);
	if (textureIt == TextureAllocations.end())
		return false;

	VulkanTextureAllocation& allocation = textureIt->second;
	if (allocation.Image == VK_NULL_HANDLE || allocation.Width == 0 || allocation.Height == 0)
		return false;

	const ETextureFormat captureFormat = source->Format;
	const uint32_t bytesPerPixel = GetTextureFormatBytesPerPixel(captureFormat);
	const VkDeviceSize rowPitch = static_cast<VkDeviceSize>(allocation.Width) * bytesPerPixel;
	const VkDeviceSize readbackSize = rowPitch * allocation.Height;
	if (readbackSize == 0)
		return false;

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	if (!CreateBufferWithMemory(
		readbackSize,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		false,
		stagingBuffer,
		stagingMemory))
	{
		return false;
	}

	auto cleanup = [&]()
	{
		if (stagingBuffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, stagingBuffer, nullptr);
		if (stagingMemory != VK_NULL_HANDLE)
			vkFreeMemory(Device, stagingMemory, nullptr);
	};

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = CommandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(Device, &allocInfo, &commandBuffer) != VK_SUCCESS)
	{
		cleanup();
		return false;
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		cleanup();
		return false;
	}

	const VkImageAspectFlags aspectMask = HasTextureUsage(allocation.Usage, TextureUsage_DepthStencil)
		? VK_IMAGE_ASPECT_DEPTH_BIT
		: VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageLayout originalLayout =
		allocation.CurrentLayout != VK_IMAGE_LAYOUT_UNDEFINED
			? allocation.CurrentLayout
			: ToVkImageLayout(beforeState);
	TransitionImageLayoutGeneric(
		commandBuffer,
		allocation.Image,
		aspectMask,
		originalLayout,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	VkBufferImageCopy copyRegion{};
	copyRegion.imageSubresource.aspectMask = aspectMask;
	copyRegion.imageSubresource.layerCount = 1;
	copyRegion.imageExtent = { allocation.Width, allocation.Height, 1 };
	vkCmdCopyImageToBuffer(
		commandBuffer,
		allocation.Image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		stagingBuffer,
		1,
		&copyRegion);

	TransitionImageLayoutGeneric(
		commandBuffer,
		allocation.Image,
		aspectMask,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		originalLayout);

	if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		cleanup();
		return false;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if (vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);
		cleanup();
		return false;
	}
	vkQueueWaitIdle(GraphicsQueue);
	vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);

	void* mappedData = nullptr;
	if (vkMapMemory(Device, stagingMemory, 0, readbackSize, 0, &mappedData) != VK_SUCCESS || !mappedData)
	{
		cleanup();
		return false;
	}
	captured.Format = captureFormat;
	captured.Width = allocation.Width;
	captured.Height = allocation.Height;
	captured.RowPitch = static_cast<uint32_t>(rowPitch);
	captured.Pixels.resize(static_cast<size_t>(readbackSize));
	std::memcpy(captured.Pixels.data(), mappedData, captured.Pixels.size());
	vkUnmapMemory(Device, stagingMemory);
	cleanup();
	return true;
#endif
}
void VulkanBackend::InitializeImGuiBackend(WindowHandle window, ETextureFormat rtvFormat)
{
	(void)window;
	(void)rtvFormat;
#if !CORONA_HAS_VULKAN
	ThrowNotImplemented(__FUNCTION__);
#else
	if (bImGuiBackendInitialized)
		return;
	if (Device == VK_NULL_HANDLE || RenderPass == VK_NULL_HANDLE || SwapchainImages.empty())
		return;

	VkDescriptorPoolSize poolSizes[] =
	{
		{ VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
	};

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	const uint32_t poolSizeCount = static_cast<uint32_t>(sizeof(poolSizes) / sizeof(poolSizes[0]));
	poolInfo.maxSets = 1000 * poolSizeCount;
	poolInfo.poolSizeCount = poolSizeCount;
	poolInfo.pPoolSizes = poolSizes;
	if (vkCreateDescriptorPool(Device, &poolInfo, nullptr, &ImGuiDescriptorPool) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan ImGui descriptor pool.");

	ImGui_ImplVulkan_InitInfo initInfo{};
	initInfo.ApiVersion = VK_API_VERSION_1_1;
	initInfo.Instance = Instance;
	initInfo.PhysicalDevice = PhysicalDevice;
	initInfo.Device = Device;
	initInfo.QueueFamily = GraphicsQueueFamilyIndex;
	initInfo.Queue = GraphicsQueue;
	initInfo.PipelineCache = VK_NULL_HANDLE;
	initInfo.DescriptorPool = ImGuiDescriptorPool;
	initInfo.MinImageCount = 2;
	initInfo.ImageCount = static_cast<uint32_t>(SwapchainImages.size());
	initInfo.PipelineInfoMain.RenderPass = RenderPass;
	initInfo.PipelineInfoMain.Subpass = 0;
	initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	initInfo.Allocator = nullptr;
	initInfo.CheckVkResultFn = nullptr;
	initInfo.MinAllocationSize = 1024 * 1024;
	if (!ImGui_ImplVulkan_Init(&initInfo))
		throw std::runtime_error("Failed to initialize Vulkan ImGui backend.");
	bImGuiBackendInitialized = true;
#endif
}
void VulkanBackend::NewImGuiFrame()
{
#if CORONA_HAS_VULKAN
	if (bImGuiBackendInitialized)
		ImGui_ImplVulkan_NewFrame();
#endif
}
void VulkanBackend::RenderImGuiDrawData(ImDrawData* drawData)
{
#if !CORONA_HAS_VULKAN
	(void)drawData; ThrowNotImplemented(__FUNCTION__);
#else
	if (!bImGuiBackendInitialized || !drawData || !bFrameActive || ActiveCommandBuffer == VK_NULL_HANDLE)
		return;

	if (!bRenderPassActive)
		SetRenderTarget(nullptr);
	if (!bRenderPassActive)
		return;

	ImGui_ImplVulkan_RenderDrawData(drawData, ActiveCommandBuffer);
#endif
}
void VulkanBackend::ShutdownImGuiBackend()
{
#if CORONA_HAS_VULKAN
	if (!bImGuiBackendInitialized)
		return;
	if (Device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(Device);
	ImGui_ImplVulkan_Shutdown();
	if (Device != VK_NULL_HANDLE && ImGuiDescriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(Device, ImGuiDescriptorPool, nullptr);
	ImGuiDescriptorPool = VK_NULL_HANDLE;
	bImGuiBackendInitialized = false;
#endif
}
void VulkanBackend::InitializeGpuTimestampQueries(uint32_t queryCount)
{
#if !CORONA_HAS_VULKAN
	(void)queryCount;
#else
	if (Device == VK_NULL_HANDLE || queryCount == 0 || TimestampValidBits == 0)
		return;

	if (TimestampQueryPool != VK_NULL_HANDLE && TimestampQueryCount == queryCount)
		return;

	ShutdownGpuTimestampQueries();

	VkQueryPoolCreateInfo queryPoolInfo{};
	queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
	queryPoolInfo.queryCount = queryCount;
	if (vkCreateQueryPool(Device, &queryPoolInfo, nullptr, &TimestampQueryPool) != VK_SUCCESS)
	{
		TimestampQueryPool = VK_NULL_HANDLE;
		TimestampQueryCount = 0;
		TimestampQueriesPerFrame = 0;
		return;
	}

	TimestampQueryCount = queryCount;
	const uint32_t frameCount = std::max<uint32_t>(GetFrameCount(), 1);
	TimestampQueriesPerFrame = queryCount / frameCount;
	bTimestampQueriesResetForCurrentFrame = false;
#endif
}

void VulkanBackend::ShutdownGpuTimestampQueries()
{
#if CORONA_HAS_VULKAN
	if (Device != VK_NULL_HANDLE && TimestampQueryPool != VK_NULL_HANDLE)
		vkDestroyQueryPool(Device, TimestampQueryPool, nullptr);
	TimestampQueryPool = VK_NULL_HANDLE;
	TimestampQueryCount = 0;
	TimestampQueriesPerFrame = 0;
	bTimestampQueriesResetForCurrentFrame = false;
#endif
}

void VulkanBackend::WriteGpuTimestamp(uint32_t queryIndex)
{
#if !CORONA_HAS_VULKAN
	(void)queryIndex;
#else
	if (TimestampQueryPool == VK_NULL_HANDLE ||
		ActiveCommandBuffer == VK_NULL_HANDLE ||
		queryIndex >= TimestampQueryCount)
	{
		return;
	}

	if (!bTimestampQueriesResetForCurrentFrame)
	{
		const uint32_t firstQuery =
			TimestampQueriesPerFrame > 0
				? CurrentFrameIndex * TimestampQueriesPerFrame
				: 0;
		const uint32_t queryCount =
			TimestampQueriesPerFrame > 0
				? std::min(TimestampQueriesPerFrame, TimestampQueryCount - firstQuery)
				: TimestampQueryCount;
		if (queryCount > 0 && firstQuery < TimestampQueryCount)
			vkCmdResetQueryPool(ActiveCommandBuffer, TimestampQueryPool, firstQuery, queryCount);
		bTimestampQueriesResetForCurrentFrame = true;
	}

	const VkPipelineStageFlagBits stage =
		(queryIndex & 1u) == 0u
			? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
			: VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	vkCmdWriteTimestamp(ActiveCommandBuffer, stage, TimestampQueryPool, queryIndex);
#endif
}

void VulkanBackend::ResolveGpuTimestampRange(uint32_t startQueryIndex, uint32_t queryCount)
{
	(void)startQueryIndex;
	(void)queryCount;
	// Vulkan timestamp query results are read directly from the query pool.
}

uint64_t VulkanBackend::ReadGpuTimestampValue(uint32_t queryIndex) const
{
#if !CORONA_HAS_VULKAN
	(void)queryIndex;
	return 0;
#else
	if (Device == VK_NULL_HANDLE ||
		TimestampQueryPool == VK_NULL_HANDLE ||
		queryIndex >= TimestampQueryCount)
	{
		return 0;
	}

	uint64_t result = 0;
	const VkResult queryResult = vkGetQueryPoolResults(
		Device,
		TimestampQueryPool,
		queryIndex,
		1,
		sizeof(result),
		&result,
		sizeof(result),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
	if (queryResult != VK_SUCCESS)
		return 0;

	if (TimestampValidBits > 0 && TimestampValidBits < 64)
	{
		const uint64_t validMask = (uint64_t{ 1 } << TimestampValidBits) - 1u;
		result &= validMask;
	}
	return result;
#endif
}

void VulkanBackend::InitializeOcclusionQueries(uint32_t queryCount)
{
#if CORONA_HAS_VULKAN
	if (Device == VK_NULL_HANDLE)
		return;
	if (OcclusionQueryPool != VK_NULL_HANDLE && OcclusionQueryCount == queryCount)
		return;

	ShutdownOcclusionQueries();
	if (queryCount == 0)
		return;

	VkQueryPoolCreateInfo queryPoolInfo{};
	queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	queryPoolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
	queryPoolInfo.queryCount = queryCount;
	if (vkCreateQueryPool(Device, &queryPoolInfo, nullptr, &OcclusionQueryPool) != VK_SUCCESS)
	{
		OcclusionQueryPool = VK_NULL_HANDLE;
		OcclusionQueryCount = 0;
		return;
	}
	OcclusionQueryCount = queryCount;
	bOcclusionQueriesResetForCurrentFrame = false;
#else
	(void)queryCount;
#endif
}

void VulkanBackend::ShutdownOcclusionQueries()
{
#if CORONA_HAS_VULKAN
	if (Device != VK_NULL_HANDLE && OcclusionQueryPool != VK_NULL_HANDLE)
		vkDestroyQueryPool(Device, OcclusionQueryPool, nullptr);
	OcclusionQueryPool = VK_NULL_HANDLE;
	OcclusionQueryCount = 0;
	bOcclusionQueriesResetForCurrentFrame = false;
#endif
}

void VulkanBackend::BeginOcclusionQuery(uint32_t queryIndex)
{
#if CORONA_HAS_VULKAN
	if (OcclusionQueryPool == VK_NULL_HANDLE ||
		ActiveCommandBuffer == VK_NULL_HANDLE ||
		queryIndex >= OcclusionQueryCount)
	{
		return;
	}
	vkCmdBeginQuery(ActiveCommandBuffer, OcclusionQueryPool, queryIndex, 0);
#else
	(void)queryIndex;
#endif
}

void VulkanBackend::EndOcclusionQuery(uint32_t queryIndex)
{
#if CORONA_HAS_VULKAN
	if (OcclusionQueryPool == VK_NULL_HANDLE ||
		ActiveCommandBuffer == VK_NULL_HANDLE ||
		queryIndex >= OcclusionQueryCount)
	{
		return;
	}
	vkCmdEndQuery(ActiveCommandBuffer, OcclusionQueryPool, queryIndex);
#else
	(void)queryIndex;
#endif
}

void VulkanBackend::ResolveOcclusionQueryRange(uint32_t startQueryIndex, uint32_t queryCount)
{
#if CORONA_HAS_VULKAN
	(void)startQueryIndex;
	(void)queryCount;
	// Vulkan occlusion query results are read directly from the query pool.
#else
	(void)startQueryIndex;
	(void)queryCount;
#endif
}

uint64_t VulkanBackend::ReadOcclusionQueryValue(uint32_t queryIndex) const
{
#if CORONA_HAS_VULKAN
	if (Device == VK_NULL_HANDLE ||
		OcclusionQueryPool == VK_NULL_HANDLE ||
		queryIndex >= OcclusionQueryCount)
	{
		return 1;
	}

	uint64_t result = 1;
	const VkResult queryResult = vkGetQueryPoolResults(
		Device,
		OcclusionQueryPool,
		queryIndex,
		1,
		sizeof(result),
		&result,
		sizeof(result),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
	if (queryResult != VK_SUCCESS)
		return 1;
	return result;
#else
	(void)queryIndex;
	return 1;
#endif
}

void VulkanBackend::SetRenderTarget(Texture* colorTarget, Texture* depthTarget)
{
#if !CORONA_HAS_VULKAN
	(void)colorTarget; (void)depthTarget; ThrowNotImplemented(__FUNCTION__);
#else
	if (colorTarget || depthTarget)
	{
		bool bColorIsActiveSwapchain = false;
		if (colorTarget && ActiveSwapchainImageIndex < SwapchainImages.size())
		{
			auto colorIt = TextureAllocations.find(colorTarget);
			bColorIsActiveSwapchain =
				colorIt != TextureAllocations.end() &&
				colorIt->second.Image == SwapchainImages[ActiveSwapchainImageIndex];
		}

		if (!bColorIsActiveSwapchain || depthTarget)
		{
			Texture* colorTargets[] = { colorTarget };
			SetRenderTargets(colorTarget ? colorTargets : nullptr, colorTarget ? 1u : 0u, depthTarget);
			return;
		}
	}

	if (!bFrameActive || bRenderPassActive)
		return;

	VkClearValue clearValue{};
	clearValue.color.float32[0] = PendingClearColor[0];
	clearValue.color.float32[1] = PendingClearColor[1];
	clearValue.color.float32[2] = PendingClearColor[2];
	clearValue.color.float32[3] = PendingClearColor[3];

	VkRenderPassBeginInfo renderPassBeginInfo{};
	renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassBeginInfo.renderPass = RenderPass;
	renderPassBeginInfo.framebuffer = SwapchainFramebuffers[ActiveSwapchainImageIndex];
	renderPassBeginInfo.renderArea.extent = SwapchainExtent;
	renderPassBeginInfo.clearValueCount = 1;
	renderPassBeginInfo.pClearValues = &clearValue;
	vkCmdBeginRenderPass(ActiveCommandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
	bRenderPassActive = true;
	ActiveGraphicsRenderPass = RenderPass;
	ActiveColorAttachmentCount = 1;
#endif
}
void VulkanBackend::SetRenderTargets(Texture* const* colorTargets, uint32_t colorTargetCount, Texture* depthTarget)
{
#if !CORONA_HAS_VULKAN
	(void)colorTargets; (void)colorTargetCount; (void)depthTarget; ThrowNotImplemented(__FUNCTION__);
#else
	PendingOffscreenColorTargets.clear();
	for (uint32_t i = 0; i < colorTargetCount; ++i)
	{
		if (colorTargets[i])
			PendingOffscreenColorTargets.push_back(colorTargets[i]);
	}
	PendingOffscreenDepthTarget = depthTarget;
#endif
}
void VulkanBackend::ClearRenderTarget(Texture* colorTarget, const float clearColor[4])
{
#if !CORONA_HAS_VULKAN
	(void)colorTarget; (void)clearColor; ThrowNotImplemented(__FUNCTION__);
#else
	(void)colorTarget;
	if (clearColor)
	{
		PendingClearColor[0] = clearColor[0];
		PendingClearColor[1] = clearColor[1];
		PendingClearColor[2] = clearColor[2];
		PendingClearColor[3] = clearColor[3];
		if (colorTarget)
		{
			PendingTextureClearColors[colorTarget] = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
		}
	}
#endif
}
void VulkanBackend::ClearDepth(Texture* depthTarget, float depthValue)
{
#if !CORONA_HAS_VULKAN
	(void)depthTarget; (void)depthValue; ThrowNotImplemented(__FUNCTION__);
#else
	if (depthTarget)
		PendingDepthClearValues[depthTarget] = depthValue;
#endif
}
void VulkanBackend::BindDefaultDescriptorHeaps()
{
#if !CORONA_HAS_VULKAN
	ThrowNotImplemented(__FUNCTION__);
#endif
}
void VulkanBackend::SetViewportAndScissor(uint32_t width, uint32_t height)
{
#if !CORONA_HAS_VULKAN
	(void)width; (void)height; ThrowNotImplemented(__FUNCTION__);
#else
	PendingViewportWidth = width;
	PendingViewportHeight = height;
	if (!bRenderPassActive)
		return;

	VkViewport viewport{};
	viewport.width = static_cast<float>(width);
	viewport.y = static_cast<float>(height);
	viewport.height = -static_cast<float>(height);
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(ActiveCommandBuffer, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.extent = { width, height };
	vkCmdSetScissor(ActiveCommandBuffer, 0, 1, &scissor);
	bViewportBound = true;
#endif
}
#if CORONA_HAS_VULKAN
void VulkanBackend::BindGraphicsPipelineForDraw(VulkanGraphicsPipelineHandle* pipeline)
{
	if (!pipeline || pipeline->Pipeline == VK_NULL_HANDLE)
		return;

	vkCmdBindPipeline(ActiveCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Pipeline);
	if (pipeline->DescriptorSetLayout == VK_NULL_HANDLE)
		return;

	VkDescriptorSet descriptorSet = AllocateGraphicsDescriptorSet(pipeline->DescriptorSetLayout);
	if (descriptorSet == VK_NULL_HANDLE)
		return;

	std::array<const VulkanGraphicsBindGroupHandle*, kMaxGraphicsBindGroupSlots> bindGroups{};
	size_t bindGroupTextureCount = 0;
	size_t bindGroupBufferCount = 0;
	size_t bindGroupSamplerCount = 0;
	const VulkanGraphicsBindGroupHandle* constantBindGroup = nullptr;
	for (uint32_t slot = 0; slot < kMaxGraphicsBindGroupSlots; ++slot)
	{
		const VulkanGraphicsBindGroupHandle* bindGroup =
			(pipeline->BoundBindGroups[slot] && pipeline->BoundBindGroups[slot]->Pipeline == pipeline) ?
			pipeline->BoundBindGroups[slot].get() :
			nullptr;
		bindGroups[slot] = bindGroup;
		if (!bindGroup)
			continue;
		bindGroupTextureCount += bindGroup->Textures.size();
		bindGroupBufferCount += bindGroup->Buffers.size();
		bindGroupSamplerCount += bindGroup->Samplers.size();
		if (bindGroup->bHasConstantData)
			constantBindGroup = bindGroup;
	}

	std::vector<VkDescriptorImageInfo> imageInfos;
	std::vector<VkDescriptorBufferInfo> bufferInfos;
	std::vector<VkWriteDescriptorSet> descriptorWrites;
	imageInfos.reserve(bindGroupTextureCount + bindGroupSamplerCount);
	bufferInfos.reserve(
		(pipeline->Desc.ConstantBufferSize > 0 ? 1 : 0) +
		bindGroupBufferCount);
	descriptorWrites.reserve(
		(pipeline->Desc.ConstantBufferSize > 0 ? 1 : 0) +
		bindGroupTextureCount +
		bindGroupBufferCount +
		bindGroupSamplerCount);

	if (pipeline->Desc.ConstantBufferSize > 0)
	{
		if (!constantBindGroup || constantBindGroup->ConstantData.empty())
			return;

		VkBuffer uniformBuffer = VK_NULL_HANDLE;
		VkDeviceSize uniformOffset = 0;
		void* mappedData = nullptr;
		if (!AllocateTransientUniform(pipeline->Desc.ConstantBufferSize, uniformBuffer, uniformOffset, &mappedData))
			return;
		const size_t copySize = (std::min)(constantBindGroup->ConstantData.size(), static_cast<size_t>(pipeline->Desc.ConstantBufferSize));
		std::memcpy(mappedData, constantBindGroup->ConstantData.data(), copySize);

		VkDescriptorBufferInfo& bufferInfo = bufferInfos.emplace_back();
		bufferInfo.buffer = uniformBuffer;
		bufferInfo.offset = uniformOffset;
		bufferInfo.range = pipeline->Desc.ConstantBufferSize;

		VkWriteDescriptorSet& writeDescriptor = descriptorWrites.emplace_back();
		writeDescriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptor.dstSet = descriptorSet;
		writeDescriptor.dstBinding = constantBindGroup->ConstantDataBinding;
		writeDescriptor.descriptorCount = 1;
		writeDescriptor.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writeDescriptor.pBufferInfo = &bufferInfo;
	}

	auto writeTextureDescriptor = [&](uint32_t descriptorBinding, Texture* texture)
	{
		if (!texture)
			return;
		auto textureIt = TextureAllocations.find(texture);
		if (textureIt == TextureAllocations.end())
			return;

		VkDescriptorImageInfo& imageInfo = imageInfos.emplace_back();
		imageInfo.sampler = VK_NULL_HANDLE;
		imageInfo.imageView = textureIt->second.ImageView;
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkWriteDescriptorSet& writeDescriptor = descriptorWrites.emplace_back();
		writeDescriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptor.dstSet = descriptorSet;
		writeDescriptor.dstBinding = descriptorBinding;
		writeDescriptor.descriptorCount = 1;
		writeDescriptor.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writeDescriptor.pImageInfo = &imageInfo;
	};

	auto writeBufferDescriptor = [&](uint32_t descriptorBinding, Buffer* buffer, VertexBuffer* vertexBuffer)
	{
		VkBuffer descriptorBuffer = VK_NULL_HANDLE;
		VkDeviceSize descriptorOffset = 0;
		VkDeviceSize descriptorRange = 0;
		if (buffer)
		{
			auto bufferIt = BufferAllocations.find(buffer);
			if (bufferIt == BufferAllocations.end())
				return;
			descriptorBuffer = bufferIt->second.Buffer;
			descriptorOffset = bufferIt->second.Offset;
			descriptorRange = bufferIt->second.SizeInBytes;
		}
		else if (vertexBuffer)
		{
			auto vertexBufferIt = VertexBufferAllocations.find(vertexBuffer);
			if (vertexBufferIt == VertexBufferAllocations.end())
				return;
			descriptorBuffer = vertexBufferIt->second.Buffer;
			descriptorOffset = vertexBufferIt->second.Offset;
			descriptorRange = vertexBufferIt->second.SizeInBytes;
		}
		else
		{
			return;
		}

		VkDescriptorBufferInfo& bufferInfo = bufferInfos.emplace_back();
		bufferInfo.buffer = descriptorBuffer;
		bufferInfo.offset = descriptorOffset;
		bufferInfo.range = descriptorRange;

		VkWriteDescriptorSet& writeDescriptor = descriptorWrites.emplace_back();
		writeDescriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptor.dstSet = descriptorSet;
		writeDescriptor.dstBinding = descriptorBinding;
		writeDescriptor.descriptorCount = 1;
		writeDescriptor.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writeDescriptor.pBufferInfo = &bufferInfo;
	};

	auto writeSamplerDescriptor = [&](uint32_t descriptorBinding, Sampler* sampler)
	{
		if (!sampler)
			return;
		auto samplerIt = SamplerAllocations.find(sampler);
		if (samplerIt == SamplerAllocations.end())
			return;

		VkDescriptorImageInfo& imageInfo = imageInfos.emplace_back();
		imageInfo.sampler = samplerIt->second.SamplerHandle;
		imageInfo.imageView = VK_NULL_HANDLE;
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkWriteDescriptorSet& writeDescriptor = descriptorWrites.emplace_back();
		writeDescriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptor.dstSet = descriptorSet;
		writeDescriptor.dstBinding = descriptorBinding;
		writeDescriptor.descriptorCount = 1;
		writeDescriptor.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		writeDescriptor.pImageInfo = &imageInfo;
	};

	for (const VulkanGraphicsBindGroupHandle* bindGroup : bindGroups)
	{
		if (!bindGroup)
			continue;
		for (const auto& binding : bindGroup->Textures)
			writeTextureDescriptor(binding.Binding, binding.TextureValue);
		for (const auto& binding : bindGroup->Buffers)
			writeBufferDescriptor(binding.Binding, binding.BufferValue, binding.VertexBufferValue);
		for (const auto& binding : bindGroup->Samplers)
			writeSamplerDescriptor(binding.Binding, binding.SamplerValue);
	}

	if (!descriptorWrites.empty())
		vkUpdateDescriptorSets(Device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);

	vkCmdBindDescriptorSets(
		ActiveCommandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeline->Layout,
		0,
		1,
		&descriptorSet,
		0,
		nullptr);
}
#endif
void VulkanBackend::DrawFullscreenQuad(VertexBuffer* vertexBuffer)
{
#if !CORONA_HAS_VULKAN
	(void)vertexBuffer; ThrowNotImplemented(__FUNCTION__);
#else
	if (!bRenderPassActive || !vertexBuffer)
		return;

	auto vbIt = VertexBufferAllocations.find(vertexBuffer);
	if (vbIt == VertexBufferAllocations.end())
		return;

	const VkDeviceSize offsets[] = { 0 };
	VkBuffer vertexBufferHandle = vbIt->second.Buffer;
	vkCmdBindVertexBuffers(ActiveCommandBuffer, 0, 1, &vertexBufferHandle, offsets);

	if (auto* pipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(BoundGraphicsPipeline))
	{
		BindGraphicsPipelineForDraw(pipeline);
	}
	else
	{
		return;
	}

	if (!bViewportBound)
		SetViewportAndScissor(
			PendingViewportWidth > 0 ? PendingViewportWidth : SwapchainExtent.width,
			PendingViewportHeight > 0 ? PendingViewportHeight : SwapchainExtent.height);

	vkCmdDraw(ActiveCommandBuffer, 4, 1, 0, 0);
#endif
}
#if CORONA_HAS_VULKAN
VkPipeline VulkanBackend::CreateTestTrianglePipeline(VkRenderPass compatibleRenderPass, uint32_t colorAttachmentCount)
{
	if (compatibleRenderPass == VK_NULL_HANDLE ||
		PipelineLayout == VK_NULL_HANDLE ||
		VertexShaderModule == VK_NULL_HANDLE ||
		FragmentShaderModule == VK_NULL_HANDLE)
	{
		return VK_NULL_HANDLE;
	}

	VkPipelineShaderStageCreateInfo shaderStages[2]{};
	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = VertexShaderModule;
	shaderStages[0].pName = "main";
	shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = FragmentShaderModule;
	shaderStages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.cullMode = VK_CULL_MODE_NONE;
	rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rasterizer.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_FALSE;
	depthStencil.depthWriteEnable = VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

	std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(
		std::max<uint32_t>(1u, colorAttachmentCount));
	for (auto& colorBlendAttachment : colorBlendAttachments)
	{
		colorBlendAttachment.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	}

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
	colorBlending.pAttachments = colorBlendAttachments.data();

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = PipelineLayout;
	pipelineInfo.renderPass = compatibleRenderPass;
	pipelineInfo.subpass = 0;

	VkPipeline pipeline = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan test triangle pipeline.");
	return pipeline;
}
#endif
void VulkanBackend::DrawWindowTestTriangle()
{
#if !CORONA_HAS_VULKAN
	ThrowNotImplemented(__FUNCTION__);
#else
	if (!bFrameActive || ActiveCommandBuffer == VK_NULL_HANDLE || GraphicsPipeline == VK_NULL_HANDLE)
		return;

	if (!bRenderPassActive)
		SetRenderTarget(nullptr);
	if (!bRenderPassActive)
		return;

	vkCmdBindPipeline(ActiveCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, GraphicsPipeline);
	if (!bViewportBound)
		SetViewportAndScissor(SwapchainExtent.width, SwapchainExtent.height);
	vkCmdDraw(ActiveCommandBuffer, 3, 1, 0, 0);

	static bool bLoggedWindowTestTriangle = false;
	if (!bLoggedWindowTestTriangle)
	{
		AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DrawWindowTestTriangle] submitted forced swapchain triangle");
		bLoggedWindowTestTriangle = true;
	}
#endif
}
void VulkanBackend::DrawActiveRenderPassTestTriangle()
{
#if !CORONA_HAS_VULKAN
	ThrowNotImplemented(__FUNCTION__);
#else
	if (!bFrameActive ||
		!bRenderPassActive ||
		ActiveCommandBuffer == VK_NULL_HANDLE ||
		ActiveGraphicsRenderPass == VK_NULL_HANDLE)
	{
		return;
	}

	VkPipeline pipeline = VK_NULL_HANDLE;
	if (ActiveGraphicsRenderPass == RenderPass && ActiveColorAttachmentCount == 1)
	{
		pipeline = GraphicsPipeline;
	}
	else
	{
		auto pipelineIt = TestTrianglePipelines.find(ActiveGraphicsRenderPass);
		if (pipelineIt == TestTrianglePipelines.end())
		{
			pipeline = CreateTestTrianglePipeline(ActiveGraphicsRenderPass, ActiveColorAttachmentCount);
			TestTrianglePipelines[ActiveGraphicsRenderPass] = pipeline;
		}
		else
		{
			pipeline = pipelineIt->second;
		}
	}

	if (pipeline == VK_NULL_HANDLE)
		return;

	vkCmdBindPipeline(ActiveCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	if (!bViewportBound)
		SetViewportAndScissor(
			PendingViewportWidth > 0 ? PendingViewportWidth : SwapchainExtent.width,
			PendingViewportHeight > 0 ? PendingViewportHeight : SwapchainExtent.height);
	vkCmdDraw(ActiveCommandBuffer, 3, 1, 0, 0);

	static bool bLoggedActiveRenderPassTestTriangle = false;
	if (!bLoggedActiveRenderPassTestTriangle)
	{
		AppendVulkanRuntimeTraceBackend(
			L"[VulkanBackend::DrawActiveRenderPassTestTriangle] submitted forced active render pass triangle, attachments=" +
			std::to_wstring(ActiveColorAttachmentCount));
		bLoggedActiveRenderPassTestTriangle = true;
	}
#endif
}
void VulkanBackend::BindMeshBuffers(VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer)
{
#if !CORONA_HAS_VULKAN
	(void)vertexBuffer; (void)indexBuffer; ThrowNotImplemented(__FUNCTION__);
#else
	if (!bRenderPassActive || !indexBuffer)
		return;
	// VB may be null for vertex-pulling PSOs (procedural grass). IB must
	// always be valid since DrawIndexedInstanced needs it.
	auto vbIt = vertexBuffer ? VertexBufferAllocations.find(vertexBuffer)
	                         : VertexBufferAllocations.end();
	auto ibIt = IndexBufferAllocations.find(indexBuffer);
	if (ibIt == IndexBufferAllocations.end() ||
	    (vertexBuffer && vbIt == VertexBufferAllocations.end()))
	{
		static bool bLoggedMissingMeshBuffer = false;
		if (!bLoggedMissingMeshBuffer)
		{
			AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::BindMeshBuffers] missing allocation");
			bLoggedMissingMeshBuffer = true;
		}
		return;
	}

	// Validate VB stride vs the currently-bound PSO. Vulkan reads stride
	// from the PSO (VkVertexInputBindingDescription::stride), not from
	// the VB binding — so a mismatch here means every vertex slips by
	// the delta with no validation-layer error. Always check (a single
	// integer compare is below noise next to the map lookups we already
	// did); assert in debug, log once in release.
	auto* pipeline = vertexBuffer ? dynamic_cast<VulkanGraphicsPipelineHandle*>(BoundGraphicsPipeline) : nullptr;
	if (pipeline)
	{
		const uint32_t psoStride = pipeline->Desc.VertexStride;
		const uint32_t vbStride = vbIt->second.Stride;
		// PSO with stride 0 fetches vertices via SBV / SV_VertexID and
		// doesn't consume the IA — that's the Spine / cluster-draw
		// pattern. VB without recorded stride (legacy paths) is also
		// skipped. Anything else with mismatched stride is a bug.
		if (psoStride != 0 && vbStride != 0 && psoStride != vbStride)
		{
			static std::atomic<bool> bLoggedStrideMismatch{ false };
			bool expected = false;
			if (bLoggedStrideMismatch.compare_exchange_strong(expected, true))
			{
				AppendVulkanRuntimeTraceBackend(
					L"[VulkanBackend::BindMeshBuffers] STRIDE MISMATCH — PSO=" +
					std::to_wstring(psoStride) +
					L" VB=" + std::to_wstring(vbStride) +
					L" path=" + pipeline->Desc.ShaderPath +
					L" — every vertex will read " +
					std::to_wstring(int32_t(psoStride) - int32_t(vbStride)) +
					L" B past its slot. Vulkan stride is taken from PSO.");
			}
#if defined(_DEBUG) || defined(DEBUG)
			assert(false && "Vulkan PSO/VB stride mismatch — see runtime trace");
#endif
		}
	}

	BoundIndexBuffer = ibIt->second.Buffer;
	vkCmdBindIndexBuffer(ActiveCommandBuffer, BoundIndexBuffer, ibIt->second.Offset, ibIt->second.Stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
	if (vertexBuffer)
	{
		const VkDeviceSize vbOffsets[] = { vbIt->second.Offset };
		BoundVertexBuffer = vbIt->second.Buffer;
		vkCmdBindVertexBuffers(ActiveCommandBuffer, 0, 1, &BoundVertexBuffer, vbOffsets);
	}
	static bool bLoggedFirstMeshBufferBind = false;
	if (!bLoggedFirstMeshBufferBind)
	{
		AppendVulkanRuntimeTraceBackend(
			L"[VulkanBackend::BindMeshBuffers] first bind vbSize=" +
			std::to_wstring(vbIt->second.SizeInBytes) +
			L", vbStride=" +
			std::to_wstring(vbIt->second.Stride) +
			L", ibSize=" +
			std::to_wstring(ibIt->second.SizeInBytes) +
			L", ibStride=" +
			std::to_wstring(ibIt->second.Stride));
		bLoggedFirstMeshBufferBind = true;
	}
#endif
}
void VulkanBackend::DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation)
{
#if !CORONA_HAS_VULKAN
	(void)indexCount; (void)startIndexLocation; (void)baseVertexLocation; ThrowNotImplemented(__FUNCTION__);
#else
	if (!bRenderPassActive)
		return;
	auto* pipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(BoundGraphicsPipeline);
	if (pipeline)
	{
		BindGraphicsPipelineForDraw(pipeline);
	}
	else
	{
		return;
	}
	if (!bViewportBound)
		SetViewportAndScissor(
			PendingViewportWidth > 0 ? PendingViewportWidth : SwapchainExtent.width,
			PendingViewportHeight > 0 ? PendingViewportHeight : SwapchainExtent.height);
	static bool bLoggedFirstDrawIndexed = false;
	if (!bLoggedFirstDrawIndexed)
	{
		AppendVulkanRuntimeTraceBackend(
			L"[VulkanBackend::DrawIndexed] first draw indexCount=" +
			std::to_wstring(indexCount) +
			L", startIndex=" +
			std::to_wstring(startIndexLocation) +
			L", baseVertex=" +
			std::to_wstring(baseVertexLocation) +
			L", vbBound=" +
			std::to_wstring(BoundVertexBuffer != VK_NULL_HANDLE ? 1 : 0) +
			L", ibBound=" +
			std::to_wstring(BoundIndexBuffer != VK_NULL_HANDLE ? 1 : 0) +
			L", activeAttachments=" +
			std::to_wstring(ActiveColorAttachmentCount));
		bLoggedFirstDrawIndexed = true;
	}
	vkCmdDrawIndexed(ActiveCommandBuffer, indexCount, 1, startIndexLocation, baseVertexLocation, 0);
#endif
}
void VulkanBackend::DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation)
{
#if !CORONA_HAS_VULKAN
	(void)indexCountPerInstance; (void)instanceCount; (void)startIndexLocation; (void)baseVertexLocation; (void)startInstanceLocation; ThrowNotImplemented(__FUNCTION__);
#else
	if (!bRenderPassActive)
		return;
	auto* pipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(BoundGraphicsPipeline);
	if (pipeline)
		BindGraphicsPipelineForDraw(pipeline);
	else
		return;
	if (!bViewportBound)
		SetViewportAndScissor(
			PendingViewportWidth > 0 ? PendingViewportWidth : SwapchainExtent.width,
			PendingViewportHeight > 0 ? PendingViewportHeight : SwapchainExtent.height);
	vkCmdDrawIndexed(ActiveCommandBuffer, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
#endif
}
void VulkanBackend::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
#if !CORONA_HAS_VULKAN
	(void)groupCountX; (void)groupCountY; (void)groupCountZ; ThrowNotImplemented(__FUNCTION__);
#else
	if (!bFrameActive || ActiveCommandBuffer == VK_NULL_HANDLE)
		return;
	if (bRenderPassActive)
	{
		vkCmdEndRenderPass(ActiveCommandBuffer);
		bRenderPassActive = false;
	}
	vkCmdDispatch(ActiveCommandBuffer, groupCountX, groupCountY, groupCountZ);
#endif
}
void VulkanBackend::ClearTextureUAVFloat(Texture* texture, const float clearColor[4])
{
#if !CORONA_HAS_VULKAN
	(void)texture; (void)clearColor; ThrowNotImplemented(__FUNCTION__);
#else
	if (!bFrameActive || ActiveCommandBuffer == VK_NULL_HANDLE || !texture || !clearColor)
		return;

	auto textureIt = TextureAllocations.find(texture);
	if (textureIt == TextureAllocations.end())
		return;

	if (bRenderPassActive)
	{
		vkCmdEndRenderPass(ActiveCommandBuffer);
		bRenderPassActive = false;
	}

	VulkanTextureAllocation& allocation = textureIt->second;
	if (allocation.CurrentLayout != VK_IMAGE_LAYOUT_GENERAL)
	{
		TransitionImageLayoutGeneric(
			ActiveCommandBuffer,
			allocation.Image,
			VK_IMAGE_ASPECT_COLOR_BIT,
			allocation.CurrentLayout,
			VK_IMAGE_LAYOUT_GENERAL);
		allocation.CurrentLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkClearColorValue clearValue{};
	clearValue.float32[0] = clearColor[0];
	clearValue.float32[1] = clearColor[1];
	clearValue.float32[2] = clearColor[2];
	clearValue.float32[3] = clearColor[3];

	VkImageSubresourceRange range{};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;

	vkCmdClearColorImage(
		ActiveCommandBuffer,
		allocation.Image,
		VK_IMAGE_LAYOUT_GENERAL,
		&clearValue,
		1,
		&range);

	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = allocation.Image;
	barrier.subresourceRange = range;
	vkCmdPipelineBarrier(
		ActiveCommandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrier);
#endif
}
void VulkanBackend::ExecuteCurrentCommandList()
{
#if !CORONA_HAS_VULKAN
	ThrowNotImplemented(__FUNCTION__);
#else
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::ExecuteCurrentCommandList] begin");
	if (!bFrameActive)
		return;
	if (bRenderPassActive)
	{
		vkCmdEndRenderPass(ActiveCommandBuffer);
		bRenderPassActive = false;
	}

	VkImageLayout currentSwapchainLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VulkanTextureAllocation* wrappedSwapchainAllocation = nullptr;
	if (ActiveSwapchainImageIndex < SwapchainWrappedTextures.size() && SwapchainWrappedTextures[ActiveSwapchainImageIndex])
	{
		auto wrappedIt = TextureAllocations.find(SwapchainWrappedTextures[ActiveSwapchainImageIndex].get());
		if (wrappedIt != TextureAllocations.end())
		{
			wrappedSwapchainAllocation = &wrappedIt->second;
			currentSwapchainLayout = wrappedIt->second.CurrentLayout;
		}
	}
	if (currentSwapchainLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
	{
		VkImageMemoryBarrier toPresent{};
		toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		toPresent.dstAccessMask = 0;
		toPresent.oldLayout = currentSwapchainLayout;
		toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresent.image = SwapchainImages[ActiveSwapchainImageIndex];
		toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		toPresent.subresourceRange.levelCount = 1;
		toPresent.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(ActiveCommandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);
		if (wrappedSwapchainAllocation)
			wrappedSwapchainAllocation->CurrentLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}

	if (vkEndCommandBuffer(ActiveCommandBuffer) != VK_SUCCESS)
		throw std::runtime_error("Failed to end Vulkan command buffer.");
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::ExecuteCurrentCommandList] end");
#endif
}
void VulkanBackend::BeginGpuMarker(uint64_t color, const char* label)
{
#if CORONA_HAS_VULKAN
	if (!label || ActiveCommandBuffer == VK_NULL_HANDLE)
		return;
	if (!vkCmdBeginDebugUtilsLabelEXTFn)
	{
		if (vkCmdInsertDebugUtilsLabelEXTFn)
		{
			VkDebugUtilsLabelEXT instantLabel{};
			instantLabel.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
			instantLabel.pLabelName = label;
			instantLabel.color[0] = 0.20f;
			instantLabel.color[1] = 0.60f;
			instantLabel.color[2] = 1.00f;
			instantLabel.color[3] = 1.00f;
			vkCmdInsertDebugUtilsLabelEXTFn(ActiveCommandBuffer, &instantLabel);
		}
		return;
	}

	VkDebugUtilsLabelEXT debugLabel{};
	debugLabel.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
	debugLabel.pLabelName = label;
	debugLabel.color[0] = static_cast<float>((color >> 16) & 0xffu) / 255.0f;
	debugLabel.color[1] = static_cast<float>((color >> 8) & 0xffu) / 255.0f;
	debugLabel.color[2] = static_cast<float>(color & 0xffu) / 255.0f;
	debugLabel.color[3] = static_cast<float>((color >> 24) & 0xffu) / 255.0f;
	vkCmdBeginDebugUtilsLabelEXTFn(ActiveCommandBuffer, &debugLabel);
#else
	(void)color;
	(void)label;
#endif
}
void VulkanBackend::EndGpuMarker()
{
#if CORONA_HAS_VULKAN
	if (ActiveCommandBuffer != VK_NULL_HANDLE && vkCmdEndDebugUtilsLabelEXTFn)
		vkCmdEndDebugUtilsLabelEXTFn(ActiveCommandBuffer);
#endif
}
void VulkanBackend::TransitionTexture(Texture* texture, EResourceState stateBefore, EResourceState stateAfter)
{
#if !CORONA_HAS_VULKAN
	(void)texture; (void)stateBefore; (void)stateAfter; ThrowNotImplemented(__FUNCTION__);
#else
	(void)stateBefore;
	if (!texture)
		return;
	auto it = TextureAllocations.find(texture);
	if (it == TextureAllocations.end())
		return;

	VkImageLayout newLayout = ToVkImageLayout(stateAfter);
	const bool bTransitioningAwayFromAttachment =
		it->second.CurrentLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
		it->second.CurrentLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	if (bRenderPassActive && bTransitioningAwayFromAttachment && it->second.CurrentLayout != newLayout)
	{
		vkCmdEndRenderPass(ActiveCommandBuffer);
		bRenderPassActive = false;
	}
	if (it->second.CurrentLayout == newLayout)
		return;
	if (ActiveCommandBuffer == VK_NULL_HANDLE)
	{
		it->second.CurrentLayout = newLayout;
		return;
	}

	TransitionImageLayoutGeneric(
		ActiveCommandBuffer,
		it->second.Image,
		(it->second.Usage & TextureUsage_DepthStencil) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
		it->second.CurrentLayout,
		newLayout);
	it->second.CurrentLayout = newLayout;
#endif
}
void VulkanBackend::TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter)
{
#if !CORONA_HAS_VULKAN
	(void)buffer; (void)stateBefore; (void)stateAfter; ThrowNotImplemented(__FUNCTION__);
#else
	if (!buffer || ActiveCommandBuffer == VK_NULL_HANDLE)
		return;

	auto it = BufferAllocations.find(buffer);
	if (it == BufferAllocations.end() || it->second.Buffer == VK_NULL_HANDLE || it->second.SizeInBytes == 0)
		return;

	VkBufferMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = ToVkBufferAccessMask(stateBefore);
	barrier.dstAccessMask = ToVkBufferAccessMask(stateAfter);
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = it->second.Buffer;
	barrier.offset = 0;
	barrier.size = it->second.SizeInBytes;

	vkCmdPipelineBarrier(
		ActiveCommandBuffer,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0,
		0, nullptr,
		1, &barrier,
		0, nullptr);
#endif
}
void VulkanBackend::TransitionVertexBuffer(VertexBuffer* vertexBuffer, EResourceState stateBefore, EResourceState stateAfter)
{
#if !CORONA_HAS_VULKAN
	(void)vertexBuffer;
	(void)stateBefore;
	(void)stateAfter;
#else
	if (!vertexBuffer || ActiveCommandBuffer == VK_NULL_HANDLE)
		return;

	auto it = VertexBufferAllocations.find(vertexBuffer);
	if (it == VertexBufferAllocations.end() || it->second.Buffer == VK_NULL_HANDLE || it->second.SizeInBytes == 0)
		return;

	VkBufferMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = ToVkBufferAccessMask(stateBefore);
	barrier.dstAccessMask = ToVkBufferAccessMask(stateAfter);
	if (stateAfter == EResourceState::VertexBuffer)
		barrier.dstAccessMask |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = it->second.Buffer;
	barrier.offset = it->second.Offset;
	barrier.size = it->second.SizeInBytes;

	vkCmdPipelineBarrier(
		ActiveCommandBuffer,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0,
		0, nullptr,
		1, &barrier,
		0, nullptr);
#endif
}

void VulkanBackend::UAVBarrier(Buffer* buffer)
{
#if !CORONA_HAS_VULKAN
	(void)buffer; ThrowNotImplemented(__FUNCTION__);
#else
	(void)buffer;
	if (ActiveCommandBuffer == VK_NULL_HANDLE)
		return;

	VkMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

	vkCmdPipelineBarrier(
		ActiveCommandBuffer,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0,
		1, &barrier,
		0, nullptr,
		0, nullptr);
#endif
}

Texture* VulkanBackend::GetCurrentWindowRenderTarget()
{
#if !CORONA_HAS_VULKAN
	return nullptr;
#else
	if (SwapchainImages.empty())
		return nullptr;
	const uint32_t imageIndex = bFrameActive
		? ActiveSwapchainImageIndex
		: (CurrentFrameIndex % static_cast<uint32_t>(SwapchainImages.size()));
	std::shared_ptr<Texture> texture = GetSwapChainTexture(imageIndex);
	if (!texture)
		return nullptr;
	if (bFrameActive)
	{
		auto wrappedIt = TextureAllocations.find(texture.get());
		if (wrappedIt != TextureAllocations.end())
			wrappedIt->second.CurrentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	return texture.get();
#endif
}

void VulkanBackend::PrepareWindowRenderTarget(Texture* renderTarget)
{
#if CORONA_HAS_VULKAN
	if (renderTarget)
		TransitionTexture(renderTarget, EResourceState::Present, EResourceState::RenderTarget);
#else
	(void)renderTarget;
#endif
}

void VulkanBackend::FinalizeWindowRenderTarget(Texture* renderTarget)
{
#if CORONA_HAS_VULKAN
	if (renderTarget)
		TransitionTexture(renderTarget, EResourceState::RenderTarget, EResourceState::Present);
#else
	(void)renderTarget;
#endif
}

void VulkanBackend::DestroyWindowContext()
{
#if CORONA_HAS_VULKAN
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] begin");
	const bool bAlreadyEmpty =
		Device == VK_NULL_HANDLE &&
		Instance == VK_NULL_HANDLE &&
		Surface == VK_NULL_HANDLE &&
		Swapchain == VK_NULL_HANDLE &&
		CommandPool == VK_NULL_HANDLE &&
		RenderPass == VK_NULL_HANDLE &&
		SwapchainFramebuffers.empty() &&
		TransientOffscreenFramebuffers.empty() &&
		ActiveOffscreenFramebuffer == VK_NULL_HANDLE &&
		SwapchainWrappedTextures.empty() &&
		SwapchainImageViews.empty() &&
		VertexBufferAllocations.empty() &&
		IndexBufferAllocations.empty() &&
		BufferAllocations.empty() &&
		TextureAllocations.empty() &&
		SamplerAllocations.empty() &&
		OcclusionQueryPool == VK_NULL_HANDLE &&
		EmptyDescriptorSetLayout == VK_NULL_HANDLE &&
		BindlessTextureDescriptorPool == VK_NULL_HANDLE &&
		BindlessTextureDescriptorSetLayout == VK_NULL_HANDLE &&
		BindlessBufferDescriptorPool == VK_NULL_HANDLE &&
		BindlessBufferDescriptorSetLayout == VK_NULL_HANDLE &&
		RayTracingAccelerationStructures.empty();
	if (bAlreadyEmpty)
	{
		AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] already empty");
		return;
	}
	if (Device != VK_NULL_HANDLE)
	{
		AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] before vkDeviceWaitIdle");
		vkDeviceWaitIdle(Device);
		AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after vkDeviceWaitIdle");
	}

	for (VkFramebuffer framebuffer : SwapchainFramebuffers)
	{
		if (framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(Device, framebuffer, nullptr);
	}
	SwapchainFramebuffers.clear();
	for (VkFramebuffer framebuffer : TransientOffscreenFramebuffers)
	{
		if (framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(Device, framebuffer, nullptr);
	}
	TransientOffscreenFramebuffers.clear();
	ActiveOffscreenFramebuffer = VK_NULL_HANDLE;
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after framebuffer cleanup");

	SwapchainWrappedTextures.clear();
	for (VkImageView imageView : SwapchainImageViews)
	{
		if (imageView != VK_NULL_HANDLE)
			vkDestroyImageView(Device, imageView, nullptr);
	}
	SwapchainImageViews.clear();
	SwapchainImages.clear();
	DestroyFrameContexts();
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after swapchain image cleanup");

	for (auto& entry : TestTrianglePipelines)
	{
		if (entry.second != VK_NULL_HANDLE)
			vkDestroyPipeline(Device, entry.second, nullptr);
	}
	TestTrianglePipelines.clear();
	if (GraphicsPipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(Device, GraphicsPipeline, nullptr);
	if (PipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(Device, PipelineLayout, nullptr);
	if (VertexShaderModule != VK_NULL_HANDLE)
		vkDestroyShaderModule(Device, VertexShaderModule, nullptr);
	if (FragmentShaderModule != VK_NULL_HANDLE)
		vkDestroyShaderModule(Device, FragmentShaderModule, nullptr);
	GraphicsPipeline = VK_NULL_HANDLE;
	PipelineLayout = VK_NULL_HANDLE;
	VertexShaderModule = VK_NULL_HANDLE;
	FragmentShaderModule = VK_NULL_HANDLE;

	if (RenderPass != VK_NULL_HANDLE)
		vkDestroyRenderPass(Device, RenderPass, nullptr);
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after window render pass cleanup");
	for (const std::shared_ptr<VulkanGraphicsPipelineHandle>& pipeline : GraphicsPipelines)
	{
		if (pipeline)
			pipeline->Release();
	}
	for (const std::shared_ptr<VulkanRTPipelineStateObject>& pipeline : RayTracingPipelines)
	{
		if (pipeline)
			pipeline->Release();
	}
	for (const std::shared_ptr<VulkanComputePipelineStateObject>& pipeline : ComputePipelines)
	{
		if (pipeline)
			pipeline->Release();
	}
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after pipeline handle cleanup");
	DestroyBindlessDescriptorTables();
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after bindless descriptor cleanup");
	for (const std::shared_ptr<VulkanRTAS>& rtas : RayTracingAccelerationStructures)
	{
		if (rtas)
			rtas->Release();
	}
	RayTracingAccelerationStructures.clear();
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after rtas cleanup");
	ShutdownGpuTimestampQueries();
	ShutdownOcclusionQueries();
	DestroyTransientUniformBuffer();
	for (auto& entry : VertexBufferAllocations)
	{
		// Pool-backed allocations share the block's VkBuffer/VkDeviceMemory;
		// the block's destructor frees them when its shared_ptr refcount
		// hits zero. Skip explicit cleanup here for those entries.
		if (entry.second.PoolBlock)
			continue;
		if (entry.second.Buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, entry.second.Buffer, nullptr);
		if (entry.second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, entry.second.Memory, nullptr);
	}
	for (auto& entry : IndexBufferAllocations)
	{
		if (entry.second.PoolBlock)
			continue;
		if (entry.second.Buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, entry.second.Buffer, nullptr);
		if (entry.second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, entry.second.Memory, nullptr);
	}
	// Drop the active block reference. Any pool-backed VB/IB still alive
	// keeps its block alive via VulkanBufferAllocation::PoolBlock.
	ActiveUploadBlock.reset();
	for (auto& entry : BufferAllocations)
	{
		if (entry.second.PoolBlock || entry.second.PersistentPoolBlock)
			continue;
		if (entry.second.Buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, entry.second.Buffer, nullptr);
		if (entry.second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, entry.second.Memory, nullptr);
	}
	for (auto& entry : TextureAllocations)
	{
		if (entry.second.bOwnsImageView && entry.second.ImageView != VK_NULL_HANDLE)
			vkDestroyImageView(Device, entry.second.ImageView, nullptr);
		if (entry.second.bOwnsImage && entry.second.Image != VK_NULL_HANDLE)
			vkDestroyImage(Device, entry.second.Image, nullptr);
		if (entry.second.bOwnsMemory && entry.second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, entry.second.Memory, nullptr);
	}
	for (auto& entry : SamplerAllocations)
	{
		if (entry.second.SamplerHandle != VK_NULL_HANDLE)
			vkDestroySampler(Device, entry.second.SamplerHandle, nullptr);
	}
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after resource allocation cleanup");

	// Empty every container that holds RAII Vulkan resources BEFORE
	// vkDestroyDevice runs. Some elements (notably VulkanUploadHeapBlock,
	// kept alive by VulkanBufferAllocation::PoolBlock shared_ptrs) cache
	// the device handle and call vkUnmapMemory/vkDestroyBuffer/vkFreeMemory
	// in their destructor. If we cleared these containers after destroying
	// the device, the cached handle would be dangling and the cleanup
	// would access-violate.
	VertexBufferAllocations.clear();
	IndexBufferAllocations.clear();
	BufferAllocations.clear();
	PendingPersistentStructuredBufferFrees.clear();
	TransientUploadStructuredFrames.clear();
	PersistentStructuredBufferBlocks.clear();
	TextureAllocations.clear();
	SamplerAllocations.clear();
	RayTracingPipelines.clear();
	ComputePipelines.clear();
	GraphicsPipelines.clear();
	TestTrianglePipelines.clear();
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after allocation container drop");

	if (Swapchain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(Device, Swapchain, nullptr);
	if (CommandPool != VK_NULL_HANDLE)
		vkDestroyCommandPool(Device, CommandPool, nullptr);
	if (Device != VK_NULL_HANDLE)
		vkDestroyDevice(Device, nullptr);
	if (Surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(Instance, Surface, nullptr);
	if (DebugMessenger != VK_NULL_HANDLE)
		DestroyDebugUtilsMessengerEXT(Instance, DebugMessenger, nullptr);
	if (Instance != VK_NULL_HANDLE)
		vkDestroyInstance(Instance, nullptr);
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after instance cleanup");

	Instance = VK_NULL_HANDLE;
	DebugMessenger = VK_NULL_HANDLE;
	PhysicalDevice = VK_NULL_HANDLE;
	Device = VK_NULL_HANDLE;
	Surface = VK_NULL_HANDLE;
	Swapchain = VK_NULL_HANDLE;
	GraphicsQueue = VK_NULL_HANDLE;
	GraphicsQueueFamilyIndex = UINT32_MAX;
	PresentQueueFamilyIndex = UINT32_MAX;
	CommandPool = VK_NULL_HANDLE;
	RenderPass = VK_NULL_HANDLE;
	PipelineLayout = VK_NULL_HANDLE;
	GraphicsPipeline = VK_NULL_HANDLE;
	VertexShaderModule = VK_NULL_HANDLE;
	FragmentShaderModule = VK_NULL_HANDLE;
	SwapchainFormat = VK_FORMAT_UNDEFINED;
	SwapchainExtent = {};
	PendingCapturePath.clear();
	LastCapturePath.clear();
	LastCaptureError.clear();
	bLastCaptureResultValid = false;
	bLastCaptureSucceeded = false;
	// VB/IB/Buffer/Texture/Sampler/Pipeline containers were already
	// drained above (before vkDestroyDevice) to keep RAII destructors
	// from touching a dead device. Only the no-RAII bookkeeping bits
	// remain for reset here.
	RayTracingAccelerationStructures.clear();
	CurrentFrameIndex = 0;
	ActiveFrameContextIndex = 0;
	NextFrameContextIndex = 0;
	bValidationLayersEnabled = false;
	bDescriptorIndexingEnabled = false;
	bRayTracingIndirectEnabled = false;
	MaxVulkanBindlessTextureSlots = 0;
	MaxVulkanBindlessBufferSlots = 0;
	vkCmdTraceRaysIndirectKHRFn = nullptr;
	vkCmdInsertDebugUtilsLabelEXTFn = nullptr;
	vkCmdBeginDebugUtilsLabelEXTFn = nullptr;
	vkCmdEndDebugUtilsLabelEXTFn = nullptr;
	TimestampValidBits = 0;
	TimestampPeriodNs = 0.0f;
	OcclusionQueryCount = 0;
	bOcclusionQueriesResetForCurrentFrame = false;
	UniformBufferAlignment = 256;
	MaxUniformBufferRange = 0;
	TransientUniformOverflowCount = 0;
#endif
}

void VulkanBackend::CreateWindowTrianglePipeline(VkFormat swapchainFormat)
{
#if !CORONA_HAS_VULKAN
	(void)swapchainFormat;
#else
	VkAttachmentDescription colorAttachment{};
	colorAttachment.format = swapchainFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorAttachmentRef{};
	colorAttachmentRef.attachment = 0;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachmentRef;

	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = 1;
	renderPassInfo.pAttachments = &colorAttachment;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 1;
	renderPassInfo.pDependencies = &dependency;
	if (vkCreateRenderPass(Device, &renderPassInfo, nullptr, &RenderPass) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan window render pass.");

	VkShaderModuleCreateInfo shaderModuleInfo{};
	shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderModuleInfo.codeSize = sizeof(kTriangleVertexShaderSpirv);
	shaderModuleInfo.pCode = kTriangleVertexShaderSpirv.data();
	if (vkCreateShaderModule(Device, &shaderModuleInfo, nullptr, &VertexShaderModule) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan window test triangle vertex shader.");
	shaderModuleInfo.codeSize = sizeof(kTriangleFragmentShaderSpirv);
	shaderModuleInfo.pCode = kTriangleFragmentShaderSpirv.data();
	if (vkCreateShaderModule(Device, &shaderModuleInfo, nullptr, &FragmentShaderModule) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan window test triangle fragment shader.");

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	if (vkCreatePipelineLayout(Device, &pipelineLayoutInfo, nullptr, &PipelineLayout) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan window test triangle pipeline layout.");

	VkPipelineShaderStageCreateInfo shaderStages[2]{};
	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = VertexShaderModule;
	shaderStages[0].pName = "main";
	shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = FragmentShaderModule;
	shaderStages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.cullMode = VK_CULL_MODE_NONE;
	rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rasterizer.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = PipelineLayout;
	pipelineInfo.renderPass = RenderPass;
	pipelineInfo.subpass = 0;
	if (vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &GraphicsPipeline) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan window test triangle pipeline.");
#endif
}

void VulkanBackend::RecreateSwapchain(uint32_t width, uint32_t height)
{
#if !CORONA_HAS_VULKAN
	(void)width;
	(void)height;
	ThrowNotImplemented(__FUNCTION__);
#else
	if (Device == VK_NULL_HANDLE || Surface == VK_NULL_HANDLE)
		throw std::runtime_error("Vulkan swapchain recreation requires a valid device and surface.");

	if (Swapchain != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(Device);
		DestroyFrameContexts();
		for (VkFramebuffer framebuffer : SwapchainFramebuffers)
		{
			if (framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(Device, framebuffer, nullptr);
		}
		SwapchainFramebuffers.clear();
		SwapchainWrappedTextures.clear();
		for (VkImageView imageView : SwapchainImageViews)
		{
			if (imageView != VK_NULL_HANDLE)
				vkDestroyImageView(Device, imageView, nullptr);
		}
		SwapchainImageViews.clear();
		SwapchainImages.clear();
		for (auto& entry : TestTrianglePipelines)
		{
			if (entry.second != VK_NULL_HANDLE)
				vkDestroyPipeline(Device, entry.second, nullptr);
		}
		TestTrianglePipelines.clear();
		if (GraphicsPipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(Device, GraphicsPipeline, nullptr);
		if (PipelineLayout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(Device, PipelineLayout, nullptr);
		if (RenderPass != VK_NULL_HANDLE)
			vkDestroyRenderPass(Device, RenderPass, nullptr);
		if (VertexShaderModule != VK_NULL_HANDLE)
			vkDestroyShaderModule(Device, VertexShaderModule, nullptr);
		if (FragmentShaderModule != VK_NULL_HANDLE)
			vkDestroyShaderModule(Device, FragmentShaderModule, nullptr);
		vkDestroySwapchainKHR(Device, Swapchain, nullptr);
		Swapchain = VK_NULL_HANDLE;
		GraphicsPipeline = VK_NULL_HANDLE;
		PipelineLayout = VK_NULL_HANDLE;
		RenderPass = VK_NULL_HANDLE;
		VertexShaderModule = VK_NULL_HANDLE;
		FragmentShaderModule = VK_NULL_HANDLE;
	}

	VkSurfaceCapabilitiesKHR surfaceCapabilities{};
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(PhysicalDevice, Surface, &surfaceCapabilities);

	uint32_t surfaceFormatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface, &surfaceFormatCount, nullptr);
	std::vector<VkSurfaceFormatKHR> surfaceFormats(surfaceFormatCount);
	vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface, &surfaceFormatCount, surfaceFormats.data());
	VkSurfaceFormatKHR surfaceFormat = ChooseSwapchainSurfaceFormat(surfaceFormats);

	uint32_t presentModeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(PhysicalDevice, Surface, &presentModeCount, nullptr);
	std::vector<VkPresentModeKHR> presentModes(presentModeCount);
	vkGetPhysicalDeviceSurfacePresentModesKHR(PhysicalDevice, Surface, &presentModeCount, presentModes.data());

	VkExtent2D extent = ChooseSwapchainExtent(surfaceCapabilities, width, height);
	const VkPresentModeKHR selectedPresentMode = ChoosePresentMode(presentModes);
	uint32_t imageCount = surfaceCapabilities.minImageCount + 1;
	if (surfaceCapabilities.maxImageCount > 0 && imageCount > surfaceCapabilities.maxImageCount)
		imageCount = surfaceCapabilities.maxImageCount;

	VkSwapchainCreateInfoKHR swapchainInfo{};
	swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchainInfo.surface = Surface;
	swapchainInfo.minImageCount = imageCount;
	swapchainInfo.imageFormat = surfaceFormat.format;
	swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
	swapchainInfo.imageExtent = extent;
	swapchainInfo.imageArrayLayers = 1;
	swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchainInfo.preTransform = ChooseSwapchainPreTransform(surfaceCapabilities);
	swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapchainInfo.presentMode = selectedPresentMode;
	swapchainInfo.clipped = VK_TRUE;
	swapchainInfo.oldSwapchain = VK_NULL_HANDLE;
	std::wstring supportedPresentModes;
	for (VkPresentModeKHR mode : presentModes)
	{
		if (!supportedPresentModes.empty())
			supportedPresentModes += L"|";
		supportedPresentModes += GetPresentModeName(mode);
	}
	AppendVulkanRuntimeTraceBackend(
		L"[VulkanBackend::RecreateSwapchain] extent=" +
		std::to_wstring(extent.width) + L"x" + std::to_wstring(extent.height) +
		L", requestedImages=" + std::to_wstring(imageCount) +
		L", presentMode=" + GetPresentModeName(selectedPresentMode) +
		L", supportedPresentModes=" + supportedPresentModes);
	if (vkCreateSwapchainKHR(Device, &swapchainInfo, nullptr, &Swapchain) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan swapchain.");

	SwapchainFormat = surfaceFormat.format;
	SwapchainExtent = extent;

	uint32_t actualImageCount = 0;
	vkGetSwapchainImagesKHR(Device, Swapchain, &actualImageCount, nullptr);
	SwapchainImages.resize(actualImageCount);
	vkGetSwapchainImagesKHR(Device, Swapchain, &actualImageCount, SwapchainImages.data());
	SwapchainImageViews.resize(actualImageCount);
	SwapchainFramebuffers.resize(actualImageCount);

	for (uint32_t imageIndex = 0; imageIndex < actualImageCount; ++imageIndex)
	{
		VkImageViewCreateInfo imageViewInfo{};
		imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		imageViewInfo.image = SwapchainImages[imageIndex];
		imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageViewInfo.format = SwapchainFormat;
		imageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageViewInfo.subresourceRange.levelCount = 1;
		imageViewInfo.subresourceRange.layerCount = 1;
		if (vkCreateImageView(Device, &imageViewInfo, nullptr, &SwapchainImageViews[imageIndex]) != VK_SUCCESS)
			throw std::runtime_error("Failed to create Vulkan swapchain image view.");
	}

	CreateWindowTrianglePipeline(SwapchainFormat);

	for (uint32_t imageIndex = 0; imageIndex < actualImageCount; ++imageIndex)
	{
		VkImageView attachments[] = { SwapchainImageViews[imageIndex] };
		VkFramebufferCreateInfo framebufferInfo{};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = RenderPass;
		framebufferInfo.attachmentCount = 1;
		framebufferInfo.pAttachments = attachments;
		framebufferInfo.width = SwapchainExtent.width;
		framebufferInfo.height = SwapchainExtent.height;
		framebufferInfo.layers = 1;
		if (vkCreateFramebuffer(Device, &framebufferInfo, nullptr, &SwapchainFramebuffers[imageIndex]) != VK_SUCCESS)
			throw std::runtime_error("Failed to create Vulkan framebuffer.");
	}

	FrameContexts.resize(actualImageCount);
	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (VulkanFrameContext& frame : FrameContexts)
	{
		if (vkCreateSemaphore(Device, &semaphoreInfo, nullptr, &frame.ImageAvailableSemaphore) != VK_SUCCESS ||
			vkCreateSemaphore(Device, &semaphoreInfo, nullptr, &frame.RenderFinishedSemaphore) != VK_SUCCESS ||
			vkCreateFence(Device, &fenceInfo, nullptr, &frame.InFlightFence) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan frame synchronization objects.");
		}
	}

	std::vector<VkCommandBuffer> allocatedCommandBuffers(actualImageCount);
	VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
	commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocateInfo.commandPool = CommandPool;
	commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocateInfo.commandBufferCount = actualImageCount;
	if (vkAllocateCommandBuffers(Device, &commandBufferAllocateInfo, allocatedCommandBuffers.data()) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate Vulkan command buffers.");
	for (uint32_t frameIndex = 0; frameIndex < actualImageCount; ++frameIndex)
		FrameContexts[frameIndex].CommandBuffer = allocatedCommandBuffers[frameIndex];
	ActiveFrameContextIndex = 0;
	NextFrameContextIndex = 0;
#endif
}

bool VulkanBackend::CaptureCurrentSwapchainImageToPNG(uint32_t imageIndex, const std::wstring& outputPath, std::wstring* errorMessage)
{
#if !CORONA_HAS_VULKAN
	(void)imageIndex;
	(void)outputPath;
	(void)errorMessage;
	return false;
#else
	if (imageIndex >= SwapchainImages.size())
		return false;

	VkDeviceSize readbackSize = static_cast<VkDeviceSize>(SwapchainExtent.width) * static_cast<VkDeviceSize>(SwapchainExtent.height) * 4;
	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = readbackSize;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(Device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
		return false;

	VkMemoryRequirements memoryRequirements{};
	vkGetBufferMemoryRequirements(Device, stagingBuffer, &memoryRequirements);

	VkMemoryAllocateInfo allocateInfo{};
	allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocateInfo.allocationSize = memoryRequirements.size;
	allocateInfo.memoryTypeIndex = FindMemoryTypeIndex(
		PhysicalDevice,
		memoryRequirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (vkAllocateMemory(Device, &allocateInfo, nullptr, &stagingMemory) != VK_SUCCESS)
	{
		vkDestroyBuffer(Device, stagingBuffer, nullptr);
		return false;
	}

	vkBindBufferMemory(Device, stagingBuffer, stagingMemory, 0);

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo commandBufferAllocInfo{};
	commandBufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocInfo.commandPool = CommandPool;
	commandBufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocInfo.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(Device, &commandBufferAllocInfo, &commandBuffer) != VK_SUCCESS)
	{
		vkFreeMemory(Device, stagingMemory, nullptr);
		vkDestroyBuffer(Device, stagingBuffer, nullptr);
		return false;
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	VkImageMemoryBarrier toTransfer{};
	toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toTransfer.srcAccessMask = 0;
	toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.image = SwapchainImages[imageIndex];
	toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toTransfer.subresourceRange.levelCount = 1;
	toTransfer.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

	VkBufferImageCopy copyRegion{};
	copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegion.imageSubresource.layerCount = 1;
	copyRegion.imageExtent = { SwapchainExtent.width, SwapchainExtent.height, 1 };
	vkCmdCopyImageToBuffer(commandBuffer, SwapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &copyRegion);

	VkImageMemoryBarrier backToPresent{};
	backToPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	backToPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	backToPresent.dstAccessMask = 0;
	backToPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	backToPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	backToPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	backToPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	backToPresent.image = SwapchainImages[imageIndex];
	backToPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	backToPresent.subresourceRange.levelCount = 1;
	backToPresent.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &backToPresent);

	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	vkQueueSubmit(GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(GraphicsQueue);
	vkFreeCommandBuffers(Device, CommandPool, 1, &commandBuffer);

	void* mappedData = nullptr;
	vkMapMemory(Device, stagingMemory, 0, readbackSize, 0, &mappedData);
	std::vector<uint8_t> rgbaPixels(readbackSize);
	std::memcpy(rgbaPixels.data(), mappedData, rgbaPixels.size());
	vkUnmapMemory(Device, stagingMemory);

	vkFreeMemory(Device, stagingMemory, nullptr);
	vkDestroyBuffer(Device, stagingBuffer, nullptr);

	std::vector<uint8_t> flipped(rgbaPixels.size());
	const size_t rowPitch = static_cast<size_t>(SwapchainExtent.width) * 4;
	for (uint32_t y = 0; y < SwapchainExtent.height; ++y)
	{
		const size_t srcOffset = static_cast<size_t>(SwapchainExtent.height - 1 - y) * rowPitch;
		const size_t dstOffset = static_cast<size_t>(y) * rowPitch;
		std::memcpy(flipped.data() + dstOffset, rgbaPixels.data() + srcOffset, rowPitch);
	}

	return WriteRGBA8PNG(outputPath, flipped, SwapchainExtent.width, SwapchainExtent.height, errorMessage);
#endif
}

void VulkanBackend::RequestWindowCapture(const std::wstring& outputPath)
{
	PendingCapturePath = outputPath;
	bLastCaptureResultValid = false;
	LastCapturePath.clear();
	LastCaptureError.clear();
}

bool VulkanBackend::ConsumeWindowCaptureResult(std::wstring* outputPath, bool* success, std::wstring* errorMessage)
{
	if (!bLastCaptureResultValid)
		return false;

	if (outputPath)
		*outputPath = LastCapturePath;
	if (success)
		*success = bLastCaptureSucceeded;
	if (errorMessage)
		*errorMessage = LastCaptureError;

	bLastCaptureResultValid = false;
	return true;
}

std::shared_ptr<GraphicsPipelineHandle> VulkanBackend::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
#if !CORONA_HAS_VULKAN
	(void)desc;
	ThrowNotImplemented(__FUNCTION__);
#else
	auto handle = std::make_shared<VulkanGraphicsPipelineHandle>();
	handle->Owner = this;
	handle->Desc = desc;
	GraphicsPipelines.push_back(handle);
	auto getGraphicsDescriptorBinding = [](const RHIBindingDesc& binding)
	{
		switch (binding.DescriptorKind)
		{
		case RHIDescriptorKind::UAV:
			return ToVulkanUavBinding(binding.RegisterIndex);
		case RHIDescriptorKind::Sampler:
			return ToVulkanSamplerBinding(binding.RegisterIndex);
		case RHIDescriptorKind::CBV:
			return ToVulkanConstantBufferBinding(binding.RegisterIndex);
		case RHIDescriptorKind::SRV:
		case RHIDescriptorKind::AccelerationStructure:
		default:
			return ToVulkanTextureBinding(binding.RegisterIndex);
		}
	};
	if (!desc.PipelineLayout.Bindings.empty())
	{
		for (const RHIBindingDesc& binding : desc.PipelineLayout.Bindings)
		{
			const uint32_t descriptorBinding = getGraphicsDescriptorBinding(binding);
			switch (binding.DescriptorKind)
			{
			case RHIDescriptorKind::SRV:
				if (binding.ResourceKind == RHIResourceKind::Buffer)
					handle->BufferBindingSlots[binding.Name] = descriptorBinding;
				else
					handle->TextureBindingSlots[binding.Name] = descriptorBinding;
				break;
			case RHIDescriptorKind::UAV:
				if (binding.ResourceKind == RHIResourceKind::Buffer)
					handle->BufferBindingSlots[binding.Name] = descriptorBinding;
				else
					handle->TextureBindingSlots[binding.Name] = descriptorBinding;
				break;
			case RHIDescriptorKind::Sampler:
				handle->SamplerBindingSlots[binding.Name] = descriptorBinding;
				break;
			case RHIDescriptorKind::CBV:
				if (!handle->bHasConstantBufferDescriptorBinding)
				{
					handle->bHasConstantBufferDescriptorBinding = true;
					handle->ConstantBufferDescriptorBinding = descriptorBinding;
				}
				break;
			case RHIDescriptorKind::AccelerationStructure:
				handle->TextureBindingSlots[binding.Name] = descriptorBinding;
				break;
			}
		}
	}
	else
	{
		for (const auto& binding : desc.TextureBindings)
			handle->TextureBindingSlots[binding.Name] = ToVulkanTextureBinding(binding.Slot);
		for (const auto& binding : desc.BufferBindings)
			handle->BufferBindingSlots[binding.Name] = ToVulkanTextureBinding(binding.Slot);
		for (const auto& binding : desc.SamplerBindings)
			handle->SamplerBindingSlots[binding.Name] = ToVulkanSamplerBinding(binding.Slot);
	}
	if (desc.ConstantBufferSize > 0 && !handle->bHasConstantBufferDescriptorBinding)
	{
		handle->bHasConstantBufferDescriptorBinding = true;
		handle->ConstantBufferDescriptorBinding = ToVulkanConstantBufferBinding(desc.ConstantBufferBinding);
	}

	const std::wstring stem = NormalizeShaderPath(desc.ShaderPath).stem().wstring();
	const std::vector<uint32_t> vertexSpirv = LoadSpirvFile(ResolveVulkanGraphicsVertexSpirvPath(stem, desc.VertexEntryPoint));
	const std::vector<uint32_t> fragmentSpirv = LoadSpirvFile(ResolveVulkanSpirvPath(stem + L"Vulkan.frag.spv"));

	VkShaderModuleCreateInfo shaderInfo{};
	shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = vertexSpirv.size() * sizeof(uint32_t);
	shaderInfo.pCode = vertexSpirv.data();
	if (vkCreateShaderModule(Device, &shaderInfo, nullptr, &handle->VertexShaderModule) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan graphics vertex shader module.");
	shaderInfo.codeSize = fragmentSpirv.size() * sizeof(uint32_t);
	shaderInfo.pCode = fragmentSpirv.data();
	if (vkCreateShaderModule(Device, &shaderInfo, nullptr, &handle->FragmentShaderModule) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan graphics fragment shader module.");

	std::vector<VkDescriptorSetLayoutBinding> descriptorBindings;
	auto appendDescriptorLayoutBinding = [&](uint32_t descriptorBindingIndex, VkDescriptorType descriptorType, uint32_t descriptorCount, VkShaderStageFlags stageFlags)
	{
		VkDescriptorSetLayoutBinding descriptorBinding{};
		descriptorBinding.binding = descriptorBindingIndex;
		descriptorBinding.descriptorType = descriptorType;
		descriptorBinding.descriptorCount = descriptorCount;
		descriptorBinding.stageFlags = stageFlags;
		descriptorBindings.push_back(descriptorBinding);
	};

	if (!desc.PipelineLayout.Bindings.empty())
	{
		for (const RHIBindingDesc& binding : desc.PipelineLayout.Bindings)
		{
			const VkDescriptorType descriptorType = ToVulkanDescriptorType(binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
			const uint32_t descriptorCount = RHILegacyDescriptorCount(binding);
			const RHIShaderStageMask graphicsStages =
				binding.Stages & ToRHIShaderStageMask(RHIShaderStage::AllGraphics);
			const VkShaderStageFlags stageFlags = ToVulkanShaderStageFlags(
				graphicsStages,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			appendDescriptorLayoutBinding(getGraphicsDescriptorBinding(binding), descriptorType, descriptorCount, stageFlags);
		}
		const bool hasTypedConstantBuffer = std::any_of(
			desc.PipelineLayout.Bindings.begin(),
			desc.PipelineLayout.Bindings.end(),
			[](const RHIBindingDesc& binding)
			{
				return binding.DescriptorKind == RHIDescriptorKind::CBV;
			});
		if (desc.ConstantBufferSize > 0 && !hasTypedConstantBuffer)
		{
			appendDescriptorLayoutBinding(
				handle->ConstantBufferDescriptorBinding,
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				1,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
		}
	}
	else
	{
		if (desc.ConstantBufferSize > 0)
		{
			appendDescriptorLayoutBinding(
				ToVulkanConstantBufferBinding(desc.ConstantBufferBinding),
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				1,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
		}
		for (const auto& binding : desc.TextureBindings)
		{
			appendDescriptorLayoutBinding(
				ToVulkanTextureBinding(binding.Slot),
				VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				1,
				VK_SHADER_STAGE_FRAGMENT_BIT);
		}
		for (const auto& binding : desc.BufferBindings)
		{
			appendDescriptorLayoutBinding(
				ToVulkanTextureBinding(binding.Slot),
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				1,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
		}
		for (const auto& binding : desc.SamplerBindings)
		{
			appendDescriptorLayoutBinding(
				ToVulkanSamplerBinding(binding.Slot),
				VK_DESCRIPTOR_TYPE_SAMPLER,
				1,
				VK_SHADER_STAGE_FRAGMENT_BIT);
		}
	}

	if (!descriptorBindings.empty())
	{
		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
		descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		descriptorSetLayoutInfo.bindingCount = static_cast<uint32_t>(descriptorBindings.size());
		descriptorSetLayoutInfo.pBindings = descriptorBindings.data();
		if (vkCreateDescriptorSetLayout(Device, &descriptorSetLayoutInfo, nullptr, &handle->DescriptorSetLayout) != VK_SUCCESS)
			throw std::runtime_error("Failed to create Vulkan graphics descriptor set layout.");
	}

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	if (handle->DescriptorSetLayout != VK_NULL_HANDLE)
	{
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &handle->DescriptorSetLayout;
	}
	if (vkCreatePipelineLayout(Device, &pipelineLayoutInfo, nullptr, &handle->Layout) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan graphics pipeline layout.");

	VkPipelineShaderStageCreateInfo shaderStages[2]{};
	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = handle->VertexShaderModule;
	shaderStages[0].pName = desc.VertexEntryPoint.c_str();
	shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = handle->FragmentShaderModule;
	shaderStages[1].pName = desc.PixelEntryPoint.c_str();

	VkVertexInputBindingDescription bindingDescription{};
	bindingDescription.stride = desc.VertexStride;
	bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
	for (uint32_t i = 0; i < desc.VertexElements.size(); ++i)
	{
		VkVertexInputAttributeDescription attr{};
		attr.location = i;
		attr.binding = 0;
		attr.format = ToVkVertexFormat(desc.VertexElements[i].Format);
		attr.offset = desc.VertexElements[i].Offset;
		attributeDescriptions.push_back(attr);
	}

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
	vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = desc.bTriangleStrip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.cullMode = desc.bCullBackFaces ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
	rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rasterizer.lineWidth = 1.0f;
	rasterizer.depthBiasEnable = desc.bDepthBiasEnable ? VK_TRUE : VK_FALSE;
	rasterizer.depthBiasConstantFactor = desc.DepthBiasConstantFactor;
	rasterizer.depthBiasClamp = desc.DepthBiasClamp;
	rasterizer.depthBiasSlopeFactor = desc.DepthBiasSlopeFactor;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = desc.bDepthEnable ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = (desc.bDepthEnable && desc.bDepthWriteEnable) ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = desc.bDepthEnable ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS;

	std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
	const uint32_t colorAttachmentCount = static_cast<uint32_t>(desc.ColorFormats.size());
	if (PhysicalDevice != VK_NULL_HANDLE)
	{
		VkPhysicalDeviceProperties physicalDeviceProperties{};
		vkGetPhysicalDeviceProperties(PhysicalDevice, &physicalDeviceProperties);
		if (colorAttachmentCount > physicalDeviceProperties.limits.maxColorAttachments)
		{
			AppendVulkanRuntimeTraceBackend(
				L"[VulkanBackend::CreateGraphicsPipeline] warning colorAttachmentCount=" +
				std::to_wstring(colorAttachmentCount) +
				L" exceeds maxColorAttachments=" +
				std::to_wstring(physicalDeviceProperties.limits.maxColorAttachments));
		}
	}
	colorBlendAttachments.resize(colorAttachmentCount);
	for (auto& colorBlendAttachment : colorBlendAttachments)
	{
		colorBlendAttachment.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	}
	if (desc.BlendMode != EBlendMode::Opaque && colorAttachmentCount > 0)
	{
		// Mirror DX12: only RT 0 blends; remaining RTs masked off so a
		// translucent PSO can coexist with the opaque GBuffer pass and not
		// scribble into Normal/Velocity/Roughness attachments.
		auto& rt0 = colorBlendAttachments[0];
		rt0.blendEnable = VK_TRUE;
		rt0.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		rt0.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		rt0.alphaBlendOp = VK_BLEND_OP_ADD;
		rt0.colorBlendOp = VK_BLEND_OP_ADD;
		if (desc.BlendMode == EBlendMode::Additive)
		{
			rt0.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			rt0.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		}
		else // AlphaBlend
		{
			rt0.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			rt0.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		}
		for (uint32_t i = 1; i < colorAttachmentCount; ++i)
			colorBlendAttachments[i].colorWriteMask = 0;
	}

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.attachmentCount = colorAttachmentCount;
	colorBlending.pAttachments = colorAttachmentCount > 0 ? colorBlendAttachments.data() : nullptr;

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = handle->Layout;
	const bool bUseSwapchainRenderPass =
		Swapchain != VK_NULL_HANDLE &&
		desc.ColorFormats.size() == 1 &&
		!desc.bDepthEnable &&
		desc.ColorFormats[0] == ETextureFormat::RGBA8Unorm;
	if (!bUseSwapchainRenderPass)
	{
		std::vector<VkAttachmentDescription> attachments;
		attachments.reserve(desc.ColorFormats.size() + (desc.bDepthEnable ? 1 : 0));
		for (ETextureFormat colorFormat : desc.ColorFormats)
		{
			VkAttachmentDescription attachment{};
			attachment.format = ToVkFormat(colorFormat);
			attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachments.push_back(attachment);
		}
		std::vector<VkAttachmentReference> colorReferences;
		colorReferences.reserve(desc.ColorFormats.size());
		for (uint32_t i = 0; i < desc.ColorFormats.size(); ++i)
		{
			VkAttachmentReference reference{};
			reference.attachment = i;
			reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			colorReferences.push_back(reference);
		}
		VkAttachmentReference depthReference{};
		if (desc.bDepthEnable)
		{
			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = ToVkFormat(desc.DepthFormat.value_or(ETextureFormat::D32Float));
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			attachments.push_back(depthAttachment);

			depthReference.attachment = static_cast<uint32_t>(attachments.size() - 1);
			depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		}

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
		subpass.pColorAttachments = colorReferences.empty() ? nullptr : colorReferences.data();
		if (desc.bDepthEnable)
			subpass.pDepthStencilAttachment = &depthReference;

		VkRenderPassCreateInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		if (vkCreateRenderPass(Device, &renderPassInfo, nullptr, &handle->CompatibleRenderPass) != VK_SUCCESS)
			throw std::runtime_error("Failed to create Vulkan graphics compatible render pass.");
	}
	pipelineInfo.renderPass = bUseSwapchainRenderPass ? RenderPass : handle->CompatibleRenderPass;
	pipelineInfo.subpass = 0;
	if (vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &handle->Pipeline) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan graphics pipeline.");

	return handle;
#endif
}

std::shared_ptr<GraphicsBindGroupHandle> VulkanBackend::CreateGraphicsBindGroup(const GraphicsBindGroupDesc& desc)
{
#if !CORONA_HAS_VULKAN
	(void)desc;
	ThrowNotImplemented(__FUNCTION__);
#else
	auto* vkPipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(desc.Pipeline);
	if (!vkPipeline || vkPipeline->Pipeline == VK_NULL_HANDLE || desc.Slot >= kMaxGraphicsBindGroupSlots)
		return nullptr;

	auto handle = std::make_shared<VulkanGraphicsBindGroupHandle>();
	handle->Pipeline = vkPipeline;
	handle->Slot = desc.Slot;
	handle->Textures.reserve(desc.Entries.size());
	handle->Buffers.reserve(desc.Entries.size());
	handle->Samplers.reserve(desc.Entries.size());

	for (const GraphicsBindGroupEntry& entry : desc.Entries)
	{
		switch (entry.Type)
		{
		case EGraphicsBindGroupEntryType::TextureSRV:
		{
			if (!entry.TextureValue)
				break;
			const auto bindingIt = vkPipeline->TextureBindingSlots.find(entry.BindingName);
			if (bindingIt != vkPipeline->TextureBindingSlots.end())
				handle->Textures.push_back({ bindingIt->second, entry.TextureValue });
			break;
		}
		case EGraphicsBindGroupEntryType::BufferSRV:
		{
			if (!entry.BufferValue)
				break;
			const auto bindingIt = vkPipeline->BufferBindingSlots.find(entry.BindingName);
			if (bindingIt != vkPipeline->BufferBindingSlots.end())
				handle->Buffers.push_back({ bindingIt->second, entry.BufferValue, nullptr });
			break;
		}
		case EGraphicsBindGroupEntryType::VertexBufferSRV:
		{
			if (!entry.VertexBufferValue)
				break;
			const auto bindingIt = vkPipeline->BufferBindingSlots.find(entry.BindingName);
			if (bindingIt != vkPipeline->BufferBindingSlots.end())
				handle->Buffers.push_back({ bindingIt->second, nullptr, entry.VertexBufferValue });
			break;
		}
		case EGraphicsBindGroupEntryType::Sampler:
		{
			if (!entry.SamplerValue)
				break;
			const auto bindingIt = vkPipeline->SamplerBindingSlots.find(entry.BindingName);
			if (bindingIt != vkPipeline->SamplerBindingSlots.end())
				handle->Samplers.push_back({ bindingIt->second, entry.SamplerValue });
			break;
		}
		case EGraphicsBindGroupEntryType::ConstantData:
		{
			if (!entry.ConstantData || entry.ConstantDataSize == 0 || entry.Slot != 0 || vkPipeline->Desc.ConstantBufferSize == 0)
				break;
			handle->bHasConstantData = true;
			handle->ConstantDataBinding = vkPipeline->ConstantBufferDescriptorBinding;
			handle->ConstantDataSize = vkPipeline->Desc.ConstantBufferSize;
			handle->ConstantData.assign(vkPipeline->Desc.ConstantBufferSize, 0);
			const uint32_t copySize = (std::min)(entry.ConstantDataSize, vkPipeline->Desc.ConstantBufferSize);
			std::memcpy(handle->ConstantData.data(), entry.ConstantData, copySize);
			break;
		}
		}
	}

	return handle;
#endif
}

void VulkanBackend::BindGraphicsPipeline(GraphicsPipelineHandle* pipeline)
{
	BoundGraphicsPipeline = pipeline;
#if CORONA_HAS_VULKAN
	if (!bRenderPassActive)
	{
		auto* vkPipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(pipeline);
		if (vkPipeline &&
			vkPipeline->CompatibleRenderPass != VK_NULL_HANDLE &&
			(!PendingOffscreenColorTargets.empty() || PendingOffscreenDepthTarget) &&
			bFrameActive)
		{
			std::vector<VkImageView> attachments;
			attachments.reserve(PendingOffscreenColorTargets.size() + (PendingOffscreenDepthTarget ? 1 : 0));
			std::vector<VkClearValue> clearValues;
			clearValues.reserve(PendingOffscreenColorTargets.size() + (PendingOffscreenDepthTarget ? 1 : 0));

			uint32_t width = 0;
			uint32_t height = 0;
			for (Texture* target : PendingOffscreenColorTargets)
			{
				auto it = TextureAllocations.find(target);
				if (it == TextureAllocations.end())
					return;
				attachments.push_back(it->second.ImageView);
				if (width == 0)
				{
					width = it->second.Width;
					height = it->second.Height;
				}
				VkClearValue clearValue{};
				const auto clearIt = PendingTextureClearColors.find(target);
				if (clearIt != PendingTextureClearColors.end())
				{
					clearValue.color.float32[0] = clearIt->second[0];
					clearValue.color.float32[1] = clearIt->second[1];
					clearValue.color.float32[2] = clearIt->second[2];
					clearValue.color.float32[3] = clearIt->second[3];
				}
				clearValues.push_back(clearValue);
			}

			if (PendingOffscreenDepthTarget)
			{
				auto depthIt = TextureAllocations.find(PendingOffscreenDepthTarget);
				if (depthIt == TextureAllocations.end())
					return;
				if (width == 0)
				{
					width = depthIt->second.Width;
					height = depthIt->second.Height;
				}
				attachments.push_back(depthIt->second.ImageView);
				VkClearValue clearValue{};
				const auto clearIt = PendingDepthClearValues.find(PendingOffscreenDepthTarget);
				clearValue.depthStencil.depth = clearIt != PendingDepthClearValues.end() ? clearIt->second : 1.0f;
				clearValues.push_back(clearValue);
			}

			if (ActiveOffscreenFramebuffer != VK_NULL_HANDLE)
				ActiveOffscreenFramebuffer = VK_NULL_HANDLE;

			VkFramebufferCreateInfo framebufferInfo{};
			framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferInfo.renderPass = vkPipeline->CompatibleRenderPass;
			framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			framebufferInfo.pAttachments = attachments.data();
			framebufferInfo.width = width;
			framebufferInfo.height = height;
			framebufferInfo.layers = 1;
			if (vkCreateFramebuffer(Device, &framebufferInfo, nullptr, &ActiveOffscreenFramebuffer) != VK_SUCCESS)
				throw std::runtime_error("Failed to create Vulkan offscreen framebuffer.");
			TransientOffscreenFramebuffers.push_back(ActiveOffscreenFramebuffer);

			VkRenderPassBeginInfo renderPassBeginInfo{};
			renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			renderPassBeginInfo.renderPass = vkPipeline->CompatibleRenderPass;
			renderPassBeginInfo.framebuffer = ActiveOffscreenFramebuffer;
			renderPassBeginInfo.renderArea.extent = { width, height };
			renderPassBeginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
			renderPassBeginInfo.pClearValues = clearValues.data();
			vkCmdBeginRenderPass(ActiveCommandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
			bRenderPassActive = true;
			ActiveGraphicsRenderPass = vkPipeline->CompatibleRenderPass;
			ActiveColorAttachmentCount = static_cast<uint32_t>(PendingOffscreenColorTargets.size());
			if (PendingViewportWidth > 0 && PendingViewportHeight > 0)
				SetViewportAndScissor(PendingViewportWidth, PendingViewportHeight);
		}
	}
#endif
}

void VulkanBackend::BindGraphicsBindGroup(GraphicsPipelineHandle* pipeline, uint32_t slot, const std::shared_ptr<GraphicsBindGroupHandle>& bindGroup)
{
#if CORONA_HAS_VULKAN
	auto* vkPipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(pipeline);
	if (!vkPipeline || slot >= kMaxGraphicsBindGroupSlots)
		return;
	if (!bindGroup)
	{
		vkPipeline->BoundBindGroups[slot].reset();
		return;
	}
	auto vkBindGroup = std::dynamic_pointer_cast<VulkanGraphicsBindGroupHandle>(bindGroup);
	if (!vkBindGroup || vkBindGroup->Pipeline != vkPipeline || vkBindGroup->Slot != slot)
		return;
	vkPipeline->BoundBindGroups[slot] = vkBindGroup;
#else
	(void)pipeline;
	(void)slot;
	(void)bindGroup;
#endif
}

void VulkanBackend::PreviewTextureOnWindow(Texture* texture)
{
#if !CORONA_HAS_VULKAN
	(void)texture;
	ThrowNotImplemented(__FUNCTION__);
#else
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::PreviewTextureOnWindow] begin");
	if (!bFrameActive || ActiveCommandBuffer == VK_NULL_HANDLE || texture == nullptr)
		return;

	auto textureIt = TextureAllocations.find(texture);
	if (textureIt == TextureAllocations.end())
		return;

	if (bRenderPassActive)
	{
		vkCmdEndRenderPass(ActiveCommandBuffer);
		bRenderPassActive = false;
	}

	VulkanTextureAllocation& sourceAllocation = textureIt->second;
	TransitionImageLayoutGeneric(
		ActiveCommandBuffer,
		sourceAllocation.Image,
		(sourceAllocation.Usage & TextureUsage_DepthStencil) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
		sourceAllocation.CurrentLayout,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	sourceAllocation.CurrentLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	VkImageMemoryBarrier toTransferDst{};
	toTransferDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toTransferDst.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toTransferDst.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransferDst.image = SwapchainImages[ActiveSwapchainImageIndex];
	toTransferDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toTransferDst.subresourceRange.levelCount = 1;
	toTransferDst.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(
		ActiveCommandBuffer,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &toTransferDst);

	VkImageBlit blitRegion{};
	blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.srcSubresource.layerCount = 1;
	blitRegion.srcOffsets[1] = {
		static_cast<int32_t>(sourceAllocation.Width),
		static_cast<int32_t>(sourceAllocation.Height),
		1
	};
	blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.dstSubresource.layerCount = 1;
	blitRegion.dstOffsets[1] = {
		static_cast<int32_t>(SwapchainExtent.width),
		static_cast<int32_t>(SwapchainExtent.height),
		1
	};
	vkCmdBlitImage(
		ActiveCommandBuffer,
		sourceAllocation.Image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		SwapchainImages[ActiveSwapchainImageIndex],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&blitRegion,
		VK_FILTER_LINEAR);

	VkImageMemoryBarrier backToColorAttachment{};
	backToColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	backToColorAttachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	backToColorAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	backToColorAttachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	backToColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	backToColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	backToColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	backToColorAttachment.image = SwapchainImages[ActiveSwapchainImageIndex];
	backToColorAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	backToColorAttachment.subresourceRange.levelCount = 1;
	backToColorAttachment.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(
		ActiveCommandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &backToColorAttachment);

	TransitionImageLayoutGeneric(
		ActiveCommandBuffer,
		sourceAllocation.Image,
		(sourceAllocation.Usage & TextureUsage_DepthStencil) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
		sourceAllocation.CurrentLayout,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	sourceAllocation.CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::PreviewTextureOnWindow] end");
#endif
}
