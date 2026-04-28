#include "stdafx.h"
#include "VulkanBackend.h"
#include "SimpleDX12.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include <wincodec.h>

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

#include "external/assimp/include/Importer.hpp"
#include "external/assimp/include/scene.h"
#include "external/assimp/include/postprocess.h"

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
	constexpr uint32_t kVulkanGraphicsDescriptorSetsPerPool = 4096;
	constexpr uint32_t kVulkanComputeDescriptorSetsPerPool = 256;
	constexpr VkDeviceSize kVulkanTransientUniformBytesPerFrame = 16ull * 1024ull * 1024ull;

	VkDeviceSize AlignVkDeviceSize(VkDeviceSize value, VkDeviceSize alignment)
	{
		if (alignment <= 1)
			return value;
		return (value + alignment - 1) & ~(alignment - 1);
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

	VulkanRTPipelineStateObject::BindingDesc MakeRTBindingDesc(const std::string& shader, const std::string& name, uint32_t baseRegister, uint32_t descriptorBinding, uint32_t dataSize = 0)
	{
		VulkanRTPipelineStateObject::BindingDesc binding{};
		binding.Shader = shader;
		binding.Name = name;
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
#endif

	std::wstring Utf8ToWide(const std::string& text)
	{
		if (text.empty())
			return std::wstring();

		const int requiredChars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
		if (requiredChars <= 0)
			return std::wstring(text.begin(), text.end());

		std::wstring result(requiredChars, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), requiredChars);
		return result;
	}

	bool WriteRGBA8PNG(const std::wstring& outputPath, const std::vector<uint8_t>& rgbaPixels, uint32_t width, uint32_t height, std::wstring* errorMessage)
	{
		Microsoft::WRL::ComPtr<IWICImagingFactory> imagingFactory;
		HRESULT hr = CoCreateInstance(
			CLSID_WICImagingFactory,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&imagingFactory));
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to create WIC imaging factory for Vulkan PNG output.";
			return false;
		}

		Microsoft::WRL::ComPtr<IWICStream> stream;
		hr = imagingFactory->CreateStream(&stream);
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to create WIC stream for Vulkan PNG output.";
			return false;
		}

		hr = stream->InitializeFromFilename(outputPath.c_str(), GENERIC_WRITE);
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to open Vulkan PNG output file.";
			return false;
		}

		Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
		hr = imagingFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to create WIC PNG encoder.";
			return false;
		}

		hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to initialize WIC PNG encoder.";
			return false;
		}

		Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
		Microsoft::WRL::ComPtr<IPropertyBag2> propertyBag;
		hr = encoder->CreateNewFrame(&frame, &propertyBag);
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to create WIC PNG frame.";
			return false;
		}

		hr = frame->Initialize(propertyBag.Get());
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to initialize WIC PNG frame.";
			return false;
		}

		hr = frame->SetSize(width, height);
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to set WIC PNG frame size.";
			return false;
		}

		WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
		hr = frame->SetPixelFormat(&pixelFormat);
		if (FAILED(hr) || pixelFormat != GUID_WICPixelFormat32bppBGRA)
		{
			if (errorMessage)
				*errorMessage = L"Failed to configure WIC PNG pixel format.";
			return false;
		}

		std::vector<uint8_t> bgraPixels(rgbaPixels.size());
		for (size_t i = 0; i < rgbaPixels.size(); i += 4)
		{
			bgraPixels[i + 0] = rgbaPixels[i + 2];
			bgraPixels[i + 1] = rgbaPixels[i + 1];
			bgraPixels[i + 2] = rgbaPixels[i + 0];
			bgraPixels[i + 3] = rgbaPixels[i + 3];
		}

		const UINT stride = width * 4;
		hr = frame->WritePixels(height, stride, static_cast<UINT>(bgraPixels.size()), bgraPixels.data());
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to write Vulkan PNG pixels.";
			return false;
		}

		hr = frame->Commit();
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to commit Vulkan PNG frame.";
			return false;
		}

		hr = encoder->Commit();
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"Failed to finalize Vulkan PNG output.";
			return false;
		}

		return true;
	}

	bool LoadRGBA8TextureFromFile(const std::wstring& filePath, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight, std::wstring* errorMessage)
	{
		Microsoft::WRL::ComPtr<IWICImagingFactory> imagingFactory;
		HRESULT hr = CoCreateInstance(
			CLSID_WICImagingFactory,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&imagingFactory));
		if (FAILED(hr))
		{
			if (errorMessage) *errorMessage = L"Failed to create WIC factory.";
			return false;
		}

		Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
		hr = imagingFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
		if (FAILED(hr))
		{
			if (errorMessage) *errorMessage = L"Failed to open texture file.";
			return false;
		}

		Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
		hr = decoder->GetFrame(0, &frame);
		if (FAILED(hr))
		{
			if (errorMessage) *errorMessage = L"Failed to read texture frame.";
			return false;
		}

		UINT width = 0;
		UINT height = 0;
		frame->GetSize(&width, &height);
		if (width == 0 || height == 0)
		{
			if (errorMessage) *errorMessage = L"Texture has invalid dimensions.";
			return false;
		}

		Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
		hr = imagingFactory->CreateFormatConverter(&converter);
		if (FAILED(hr))
		{
			if (errorMessage) *errorMessage = L"Failed to create WIC format converter.";
			return false;
		}

		hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
		if (FAILED(hr))
		{
			if (errorMessage) *errorMessage = L"Failed to convert texture to RGBA8.";
			return false;
		}

		outWidth = width;
		outHeight = height;
		outPixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
		const UINT stride = width * 4;
		hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(outPixels.size()), outPixels.data());
		if (FAILED(hr))
		{
			if (errorMessage) *errorMessage = L"Failed to read texture pixels.";
			return false;
		}

		return true;
	}

	std::vector<uint32_t> LoadSpirvFile(const std::wstring& filePath)
	{
		std::ifstream file(std::filesystem::path(filePath), std::ios::binary | std::ios::ate);
		if (!file.is_open())
			throw std::runtime_error("Failed to open SPIR-V shader file.");

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
	const std::filesystem::path kVulkanValidationLogPath = std::filesystem::path(L"C:\\dev\\Corona\\dumps\\vulkan_validation.log");
	const std::filesystem::path kVulkanRuntimeTracePath = std::filesystem::path(L"C:\\dev\\Corona\\dumps\\vulkan_runtime_trace.log");

	void AppendVulkanValidationLog(const std::string& line)
	{
		std::filesystem::create_directories(kVulkanValidationLogPath.parent_path());
		std::ofstream logFile(kVulkanValidationLogPath, std::ios::app);
		if (logFile.is_open())
		{
			logFile << line << "\n";
		}
		OutputDebugStringA((line + "\n").c_str());
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

		std::filesystem::create_directories(kVulkanRuntimeTracePath.parent_path());
		std::wofstream traceFile(kVulkanRuntimeTracePath, std::ios::app);
		if (traceFile.is_open())
			traceFile << line << L"\n";
	}

	bool IsVulkanValidationEnabled()
	{
		wchar_t value[32] = {};
		const DWORD length = GetEnvironmentVariableW(L"CORONA_VULKAN_VALIDATION", value, static_cast<DWORD>(_countof(value)));
		if (length == 0 || length >= _countof(value))
			return false;

		std::wstring normalized(value, length);
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

	VkExtent2D ChooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height)
	{
		if (capabilities.currentExtent.width != UINT32_MAX)
			return capabilities.currentExtent;

		VkExtent2D extent = {};
		extent.width = (std::max)(capabilities.minImageExtent.width, (std::min)(capabilities.maxImageExtent.width, width));
		extent.height = (std::max)(capabilities.minImageExtent.height, (std::min)(capabilities.maxImageExtent.height, height));
		return extent;
	}

	VkFormat ToVkFormat(ETextureFormat format)
	{
		switch (format)
		{
		case ETextureFormat::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case ETextureFormat::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
		case ETextureFormat::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
		case ETextureFormat::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
		case ETextureFormat::D32Float: return VK_FORMAT_D32_SFLOAT;
		case ETextureFormat::R32Float: return VK_FORMAT_R32_SFLOAT;
		case ETextureFormat::R8Uint: return VK_FORMAT_R8_UINT;
		default: return VK_FORMAT_R8G8B8A8_UNORM;
		}
	}

	VkFormat ToVkFormat(DXGI_FORMAT format)
	{
		switch (format)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
		case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
		case DXGI_FORMAT_D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
		default: return VK_FORMAT_R8G8B8A8_UNORM;
		}
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
		case EResourceState::UnorderedAccess: return VK_IMAGE_LAYOUT_GENERAL;
		case EResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		default: return VK_IMAGE_LAYOUT_GENERAL;
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
			case VK_IMAGE_LAYOUT_GENERAL: return bDst ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
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
	ConstantData.clear();
	DescriptorPool = VK_NULL_HANDLE;
	DescriptorSetLayout = VK_NULL_HANDLE;
	Pipeline = VK_NULL_HANDLE;
	CompatibleRenderPass = VK_NULL_HANDLE;
	Layout = VK_NULL_HANDLE;
	VertexShaderModule = VK_NULL_HANDLE;
	FragmentShaderModule = VK_NULL_HANDLE;
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
	if (DescriptorSetLayout != VK_NULL_HANDLE)
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
	PipelineLayout = VK_NULL_HANDLE;
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
	UAVBindings.push_back(MakeRTBindingDesc(shader, name, baseRegister, ToVulkanUavBinding(baseRegister)));
}

void VulkanRTPipelineStateObject::BindSRV(const std::string& shader, const std::string& name, uint32_t baseRegister)
{
	SRVBindings.push_back(MakeRTBindingDesc(shader, name, baseRegister, ToVulkanTextureBinding(baseRegister)));
}

void VulkanRTPipelineStateObject::BindSampler(const std::string& shader, const std::string& name, uint32_t baseRegister)
{
	SamplerBindings.push_back(MakeRTBindingDesc(shader, name, baseRegister, ToVulkanSamplerBinding(baseRegister)));
}

void VulkanRTPipelineStateObject::BindCBV(const std::string& shader, const std::string& name, uint32_t baseRegister, uint32_t size, uint32_t numInstance)
{
	(void)numInstance;
	CBVBindings.push_back(MakeRTBindingDesc(shader, name, baseRegister, ToVulkanConstantBufferBinding(baseRegister), size));
}

void VulkanRTPipelineStateObject::BeginShaderTable() {}
void VulkanRTPipelineStateObject::EndShaderTable() {}
void VulkanRTPipelineStateObject::SetTextureUAV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex)
{
	(void)shader;
	(void)instanceIndex;
	GlobalBindingValues[bindingName].TextureValue = texture;
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
void VulkanRTPipelineStateObject::AddTextureSRVToHitProgram(const std::string& hitGroup, Texture* texture, uint32_t instanceIndex)
{
	(void)hitGroup;
	ResourceBindingValue value{};
	value.TextureValue = texture;
	HitProgramBindingValues[instanceIndex].push_back(value);
}
void VulkanRTPipelineStateObject::AddBufferSRVToHitProgram(const std::string& hitGroup, Buffer* buffer, uint32_t instanceIndex)
{
	(void)hitGroup;
	ResourceBindingValue value{};
	value.BufferValue = buffer;
	HitProgramBindingValues[instanceIndex].push_back(value);
}
void VulkanRTPipelineStateObject::AddVertexBufferSRVToHitProgram(const std::string& hitGroup, VertexBuffer* buffer, uint32_t instanceIndex)
{
	(void)hitGroup;
	ResourceBindingValue value{};
	value.VertexBufferValue = buffer;
	HitProgramBindingValues[instanceIndex].push_back(value);
}
void VulkanRTPipelineStateObject::AddIndexBufferSRVToHitProgram(const std::string& hitGroup, IndexBuffer* buffer, uint32_t instanceIndex)
{
	(void)hitGroup;
	ResourceBindingValue value{};
	value.IndexBufferValue = buffer;
	HitProgramBindingValues[instanceIndex].push_back(value);
}

bool VulkanRTPipelineStateObject::InitRS(const std::string& shaderFile)
{
	ShaderFile = shaderFile;
	if (!Owner || Owner->Device == VK_NULL_HANDLE)
		return false;

	VulkanBackend* backendOwner = Owner;
	Release();
	Owner = backendOwner;

	const std::filesystem::path shaderPath = std::filesystem::path(std::filesystem::current_path()) / shaderFile;
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

		size_t nameEnd = trimmed.find(':');
		if (nameEnd == std::string::npos)
			nameEnd = trimmed.find(';');
		if (nameEnd == std::string::npos)
			continue;

		size_t nameStart = trimmed.rfind(' ', nameEnd);
		if (nameStart == std::string::npos)
			continue;

		const std::string resourceName = TrimAscii(trimmed.substr(nameStart + 1, nameEnd - nameStart - 1));
		if (!resourceName.empty())
		{
			std::string normalizedResourceName = resourceName;
			if (normalizedResourceName.size() >= 2 && normalizedResourceName.substr(normalizedResourceName.size() - 2) == "[]")
				normalizedResourceName.resize(normalizedResourceName.size() - 2);
			descriptorTypesByName[normalizedResourceName] = *inferredType;
		}
	}

	std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
	std::vector<VkDescriptorPoolSize> poolSizes;
	std::unordered_set<uint32_t> seenBindings;
	auto resolveDescriptorType = [&](const BindingDesc& binding, std::optional<VkDescriptorType> fallbackType) -> std::optional<VkDescriptorType>
	{
		const auto it = descriptorTypesByName.find(binding.Name);
		if (it != descriptorTypesByName.end())
			return it->second;
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
	auto addPoolSize = [&](VkDescriptorType type)
	{
		for (auto& poolSize : poolSizes)
		{
			if (poolSize.type == type)
			{
				++poolSize.descriptorCount;
				return;
			}
		}
		VkDescriptorPoolSize poolSize{};
		poolSize.type = type;
		poolSize.descriptorCount = 1;
		poolSizes.push_back(poolSize);
	};

	auto appendBindings = [&](const std::vector<BindingDesc>& bindings, std::optional<VkDescriptorType> fallbackType = std::nullopt)
	{
		for (const BindingDesc& binding : bindings)
		{
			const std::optional<VkDescriptorType> resolvedType = resolveDescriptorType(binding, fallbackType);
			if (!resolvedType.has_value())
				continue;

			const uint32_t descriptorCount = (binding.Shader == "global" || IsSharedRtGeometryBinding(binding.Name)) ? 1u : std::max(NumInstances, 1u);
			if (seenBindings.find(binding.DescriptorBinding) != seenBindings.end())
			{
				for (auto& existingBinding : layoutBindings)
				{
					if (existingBinding.binding == binding.DescriptorBinding)
					{
						existingBinding.descriptorCount = std::max(existingBinding.descriptorCount, descriptorCount);
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
			layoutBindings.push_back(layoutBinding);
			seenBindings.insert(binding.DescriptorBinding);
			for (uint32_t count = 0; count < descriptorCount; ++count)
				addPoolSize(*resolvedType);
		}
	};

	appendBindings(UAVBindings, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	appendBindings(SRVBindings);
	appendBindings(SamplerBindings, VK_DESCRIPTOR_TYPE_SAMPLER);
	appendBindings(CBVBindings, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

	const std::wstring shaderStem = shaderPath.stem().wstring();
	const std::filesystem::path spirvPath = ResolveVulkanSpirvPath(shaderStem + L"Vulkan.rt.spv");
	if (!std::filesystem::exists(spirvPath))
	{
		Owner->ErrorString += "Missing Vulkan RT SPIR-V module: " + spirvPath.string() + "\n";
		return false;
	}

	const std::vector<uint32_t> shaderSpirv = LoadSpirvFile(spirvPath.wstring());
	VkShaderModuleCreateInfo shaderModuleInfo{};
	shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderModuleInfo.codeSize = shaderSpirv.size() * sizeof(uint32_t);
	shaderModuleInfo.pCode = shaderSpirv.data();
	if (vkCreateShaderModule(Owner->Device, &shaderModuleInfo, nullptr, &ShaderModule) != VK_SUCCESS)
	{
		Owner->ErrorString += "Failed to create Vulkan RT shader module.\n";
		return false;
	}

	if (!layoutBindings.empty())
	{
		std::sort(layoutBindings.begin(), layoutBindings.end(), [](const VkDescriptorSetLayoutBinding& a, const VkDescriptorSetLayoutBinding& b)
		{
			return a.binding < b.binding;
		});

		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
		descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		descriptorSetLayoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
		descriptorSetLayoutInfo.pBindings = layoutBindings.data();
		if (vkCreateDescriptorSetLayout(Owner->Device, &descriptorSetLayoutInfo, nullptr, &DescriptorSetLayout) != VK_SUCCESS)
		{
			Owner->ErrorString += "Failed to create Vulkan RT descriptor set layout.\n";
			return false;
		}

		if (!poolSizes.empty())
		{
			const uint32_t descriptorSetCount = std::max<uint32_t>(Owner->GetFrameCount(), 3u);
			VkDescriptorPoolCreateInfo descriptorPoolInfo{};
			descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			descriptorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
			descriptorPoolInfo.maxSets = descriptorSetCount;
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

		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &DescriptorSetLayout;
		if (vkCreatePipelineLayout(Owner->Device, &pipelineLayoutInfo, nullptr, &PipelineLayout) != VK_SUCCESS)
		{
			Owner->ErrorString += "Failed to create Vulkan RT pipeline layout.\n";
			return false;
		}
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
		<< ", bindings=" << layoutBindings.size()
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
	if (!Owner || Owner->ActiveCommandBuffer == VK_NULL_HANDLE || Pipeline == VK_NULL_HANDLE || DescriptorSetLayout == VK_NULL_HANDLE || DescriptorPool == VK_NULL_HANDLE)
		return;
	AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] begin shader=" + Utf8ToWide(ShaderFile));

	VkDescriptorSet activeDescriptorSet = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo descriptorSetAllocateInfo{};
	descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorSetAllocateInfo.descriptorPool = DescriptorPool;
	descriptorSetAllocateInfo.descriptorSetCount = 1;
	descriptorSetAllocateInfo.pSetLayouts = &DescriptorSetLayout;
	if (vkAllocateDescriptorSets(Owner->Device, &descriptorSetAllocateInfo, &activeDescriptorSet) != VK_SUCCESS)
		return;
	Owner->TrackFrameDescriptorSet(DescriptorPool, activeDescriptorSet);
	DescriptorSet = activeDescriptorSet;

	std::vector<VkWriteDescriptorSet> writes;
	std::vector<VkDescriptorImageInfo> imageInfos;
	std::vector<VkDescriptorBufferInfo> bufferInfos;
	std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelInfos;
	writes.reserve(256);
	imageInfos.reserve(512);
	bufferInfos.reserve(512);
	accelInfos.reserve(64);

	auto appendImageWrite = [&](uint32_t binding, VkDescriptorType type, VkImageView imageView, VkImageLayout imageLayout, VkSampler sampler)
	{
		VkDescriptorImageInfo& imageInfo = imageInfos.emplace_back();
		imageInfo.imageView = imageView;
		imageInfo.imageLayout = imageLayout;
		imageInfo.sampler = sampler;

		VkWriteDescriptorSet& write = writes.emplace_back();
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = DescriptorSet;
		write.dstBinding = binding;
		write.descriptorCount = 1;
		write.descriptorType = type;
		write.pImageInfo = &imageInfo;
	};

	auto appendBufferWrite = [&](uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range)
	{
		VkDescriptorBufferInfo& bufferInfo = bufferInfos.emplace_back();
		bufferInfo.buffer = buffer;
		bufferInfo.offset = offset;
		bufferInfo.range = range;

		VkWriteDescriptorSet& write = writes.emplace_back();
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = DescriptorSet;
		write.dstBinding = binding;
		write.descriptorCount = 1;
		write.descriptorType = type;
		write.pBufferInfo = &bufferInfo;
	};

	for (const BindingDesc& binding : UAVBindings)
	{
		if (binding.Shader != "global")
			continue;
		auto valueIt = GlobalBindingValues.find(binding.Name);
		if (valueIt == GlobalBindingValues.end() || !valueIt->second.TextureValue)
			continue;
		auto textureIt = Owner->TextureAllocations.find(valueIt->second.TextureValue);
		if (textureIt == Owner->TextureAllocations.end())
			continue;
		appendImageWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, textureIt->second.ImageView, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
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
			appendImageWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureIt->second.ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_NULL_HANDLE);
		}
		else if (bufferIt != Owner->BufferAllocations.end())
		{
			appendBufferWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferIt->second.Buffer, 0, bufferIt->second.SizeInBytes);
		}
		else if (rtas && rtas->AccelerationStructure != VK_NULL_HANDLE)
		{
			VkWriteDescriptorSetAccelerationStructureKHR& accelInfo = accelInfos.emplace_back();
			accelInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
			accelInfo.accelerationStructureCount = 1;
			accelInfo.pAccelerationStructures = &rtas->AccelerationStructure;

			VkWriteDescriptorSet& write = writes.emplace_back();
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.pNext = &accelInfo;
			write.dstSet = DescriptorSet;
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
		appendImageWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, samplerIt->second.SamplerHandle);
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

		appendBufferWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniformBuffer, uniformOffset, uniformRange);
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
				descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
				VkWriteDescriptorSetAccelerationStructureKHR& accelInfo = accelInfos.emplace_back();
				accelInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
				accelInfo.accelerationStructureCount = 1;
				accelInfo.pAccelerationStructures = &rtas->AccelerationStructure;
				VkWriteDescriptorSet& write = writes.emplace_back();
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.pNext = &accelInfo;
				write.dstSet = DescriptorSet;
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
			VkWriteDescriptorSet& write = writes.emplace_back();
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.dstSet = DescriptorSet;
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
	vkCmdBindDescriptorSets(Owner->ActiveCommandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, PipelineLayout, 0, 1, &DescriptorSet, 0, nullptr);
	if (Owner->vkCmdSetRayTracingPipelineStackSizeKHRFn && PipelineStackSize > 0)
	{
		Owner->vkCmdSetRayTracingPipelineStackSizeKHRFn(Owner->ActiveCommandBuffer, PipelineStackSize);
	}
	AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] before vkCmdTraceRaysKHR");
	Owner->vkCmdTraceRaysKHRFn(Owner->ActiveCommandBuffer, &RaygenRegion, &MissRegion, &HitRegion, &CallableRegion, width, height, 1);
	AppendVulkanRuntimeTraceBackend(L"[VulkanRTPipelineStateObject::Apply] after vkCmdTraceRaysKHR");
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
}

void VulkanComputePipelineStateObject::BindUAV(const std::string& name, uint32_t baseRegister)
{
	UAVBindings.push_back(MakeComputeBindingDesc(name, baseRegister, ToVulkanUavBinding(baseRegister), 1, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));
}

void VulkanComputePipelineStateObject::BindCBV(const std::string& name, uint32_t baseRegister, uint32_t size)
{
	CBVBindings.push_back(MakeComputeBindingDesc(name, baseRegister, ToVulkanConstantBufferBinding(baseRegister), 1, size, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
}

void VulkanComputePipelineStateObject::BindSampler(const std::string& name, uint32_t baseRegister)
{
	SamplerBindings.push_back(MakeComputeBindingDesc(name, baseRegister, ToVulkanSamplerBinding(baseRegister), 1, 0, VK_DESCRIPTOR_TYPE_SAMPLER));
}

bool VulkanComputePipelineStateObject::InitCS(const std::wstring& shaderFile, const std::string& entryPoint)
{
	ShaderFile = shaderFile;
	EntryPoint = entryPoint;
	if (!Owner || Owner->Device == VK_NULL_HANDLE)
		return false;

	const std::wstring shaderStem = std::filesystem::path(shaderFile).stem().wstring();
	const std::filesystem::path spirvPath = ResolveVulkanSpirvPath(shaderStem + L"Vulkan.comp.spv");
	if (!std::filesystem::exists(spirvPath))
	{
		Owner->ErrorString += "Missing Vulkan compute SPIR-V module: " + spirvPath.string() + "\n";
		return false;
	}

	const std::vector<uint32_t> shaderSpirv = LoadSpirvFile(spirvPath.wstring());
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
	bufferInfos.reserve(SRVBindings.size() + CBVBindings.size());

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
				appendBufferWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferIt->second.Buffer, 0, bufferIt->second.SizeInBytes);
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
				appendBufferWrite(binding.DescriptorBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferIt->second.Buffer, 0, bufferIt->second.SizeInBytes);
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
}

void VulkanComputePipelineStateObject::SetTextureUAV(const std::string& name, Texture* texture)
{
	BindingValues[name].TextureValue = texture;
	BindingValues[name].BufferValue = nullptr;
}

void VulkanComputePipelineStateObject::SetBufferSRV(const std::string& name, Buffer* buffer)
{
	BindingValues[name].BufferValue = buffer;
	BindingValues[name].TextureValue = nullptr;
}

void VulkanComputePipelineStateObject::SetBufferUAV(const std::string& name, Buffer* buffer)
{
	BindingValues[name].BufferValue = buffer;
	BindingValues[name].TextureValue = nullptr;
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
}

[[noreturn]] void VulkanBackend::ThrowNotImplemented(const char* functionName) const
{
	throw std::runtime_error(BuildNotImplementedMessage(functionName));
}

#if CORONA_HAS_VULKAN
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

	for (const auto& descriptorSet : frame.DescriptorSetsToFree)
	{
		if (descriptorSet.first != VK_NULL_HANDLE && descriptorSet.second != VK_NULL_HANDLE)
			vkFreeDescriptorSets(Device, descriptorSet.first, 1, &descriptorSet.second);
	}
	frame.DescriptorSetsToFree.clear();

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
	TransientUniformFrameOffset = 0;
	vkResetCommandBuffer(ActiveCommandBuffer, 0);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	if (vkBeginCommandBuffer(ActiveCommandBuffer, &beginInfo) != VK_SUCCESS)
		throw std::runtime_error("Failed to begin Vulkan command buffer.");

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
	vkGetBufferDeviceAddressKHRFn = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(vkGetDeviceProcAddr(Device, "vkGetBufferDeviceAddressKHR"));

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
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
	{
		RecreateSwapchain(SwapchainExtent.width, SwapchainExtent.height);
	}
	else if (presentResult != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to present Vulkan frame.");
	}

	bFrameActive = false;
	bRenderPassActive = false;
	bViewportBound = false;
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
#endif
}
void VulkanBackend::WaitForGpu()
{
#if CORONA_HAS_VULKAN
	if (Device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(Device);
#endif
}
void VulkanBackend::EmitGpuCrashMarker(const char* markerName) { (void)markerName; }
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
SimpleDX12* VulkanBackend::AsSimpleDX12() { return nullptr; }
std::shared_ptr<Texture> VulkanBackend::CreateTexture2D(const TextureCreateDesc& desc)
{
#if !CORONA_HAS_VULKAN
	(void)desc;
	ThrowNotImplemented(__FUNCTION__);
#else
	auto texture = std::make_shared<Texture>();

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
	Buffer* buffer = new Buffer();
	buffer->NumElements = desc.NumElements;
	buffer->ElementSize = desc.ElementSize;
	buffer->Type = Buffer::UNKNOWN;

	VulkanBufferAllocation allocation{};
	allocation.Stride = desc.ElementSize;
	allocation.SizeInBytes = desc.NumElements * desc.ElementSize;

	VkBufferUsageFlags bufferUsage =
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if (bRayTracingEnabled)
	{
		bufferUsage |=
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	}
	const bool bUseDeviceLocalUpload = desc.InitialData != nullptr && allocation.SizeInBytes > 0;
	const bool bCreatedBuffer = bUseDeviceLocalUpload
		? CreateDeviceLocalBufferWithUpload(
			allocation.SizeInBytes,
			bufferUsage,
			bRayTracingEnabled,
			desc.InitialData,
			allocation.Buffer,
			allocation.Memory)
		: CreateBufferWithMemory(
			allocation.SizeInBytes,
			bufferUsage,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			bRayTracingEnabled,
			allocation.Buffer,
			allocation.Memory);
	if (!bCreatedBuffer)
	{
		throw std::runtime_error("Failed to create Vulkan buffer.");
	}

	BufferAllocations[buffer] = allocation;
	return std::shared_ptr<Buffer>(buffer);
#endif
}
std::shared_ptr<Sampler> VulkanBackend::CreateSampler(const SamplerCreateDesc& desc)
{
#if !CORONA_HAS_VULKAN
	(void)desc;
	ThrowNotImplemented(__FUNCTION__);
#else
	auto sampler = std::make_shared<Sampler>();

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
		throw std::runtime_error("Failed to load Vulkan texture file.");

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
std::shared_ptr<Texture> VulkanBackend::WrapNativeTexture(const Microsoft::WRL::ComPtr<ID3D12Resource>& resource) { (void)resource; ThrowNotImplemented(__FUNCTION__); }
std::shared_ptr<Texture> VulkanBackend::CreateTexture3D(ETextureFormat format, ETextureUsageFlags usage, EInitialResourceState initialState, int width, int height, int depth, int mipLevels)
{
#if !CORONA_HAS_VULKAN
	(void)format; (void)usage; (void)initialState; (void)width; (void)height; (void)depth; (void)mipLevels;
	ThrowNotImplemented(__FUNCTION__);
#else
	auto texture = std::make_shared<Texture>();
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
	VertexBuffer* vertexBuffer = new VertexBuffer();
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

	VertexBufferAllocations[vertexBuffer] = allocation;
	return std::shared_ptr<VertexBuffer>(vertexBuffer);
#endif
}
std::shared_ptr<IndexBuffer> VulkanBackend::CreateIndexBuffer(DXGI_FORMAT format, uint32_t size, void* srcData)
{
#if !CORONA_HAS_VULKAN
	(void)format; (void)size; (void)srcData; ThrowNotImplemented(__FUNCTION__);
#else
	IndexBuffer* indexBuffer = new IndexBuffer();
	indexBuffer->numIndices = format == DXGI_FORMAT_R16_UINT ? static_cast<int>(size / 2) : static_cast<int>(size / 4);

	VulkanBufferAllocation allocation{};
	allocation.Stride = format == DXGI_FORMAT_R16_UINT ? 2u : 4u;
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

	IndexBufferAllocations[indexBuffer] = allocation;
	return std::shared_ptr<IndexBuffer>(indexBuffer);
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

	VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
	triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	triangles.vertexData.deviceAddress = GetBufferDeviceAddress(vbIt->second.Buffer);
	triangles.vertexStride = mesh->VertexStride;
	triangles.maxVertex = static_cast<uint32_t>(mesh->Vb->numVertices);
	triangles.indexType = mesh->IndexFormat == DXGI_FORMAT_R16_UINT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
	triangles.indexData.deviceAddress = GetBufferDeviceAddress(ibIt->second.Buffer);

	VkAccelerationStructureGeometryKHR geometry{};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	// Keep any-hit enabled only for alpha-tested meshes; opaque geometry can skip it during traversal.
	geometry.flags = mesh->bTransparent ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.triangles = triangles;

	const uint32_t primitiveCount = static_cast<uint32_t>(mesh->Ib->numIndices / 3);
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
std::shared_ptr<RTAS> VulkanBackend::CreateTLAS(std::vector<std::shared_ptr<RTAS>>& bottomLevelAS)
{
	if (!bRayTracingEnabled)
		throw std::runtime_error("Vulkan ray tracing is not enabled on this device.");

	auto rtas = std::make_shared<VulkanRTAS>();
	rtas->Owner = this;

	std::vector<VkAccelerationStructureInstanceKHR> instances;
	instances.reserve(bottomLevelAS.size());
	for (size_t i = 0; i < bottomLevelAS.size(); ++i)
	{
		VulkanRTAS* blas = dynamic_cast<VulkanRTAS*>(bottomLevelAS[i].get());
		if (!blas || !blas->MeshPtr || blas->DeviceAddress == 0)
			throw std::runtime_error("Vulkan TLAS creation requires valid Vulkan BLAS instances.");

		const glm::mat4x4 mat = glm::transpose(blas->MeshPtr->transform);
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
	VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
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
		throw std::runtime_error("Failed to create Vulkan TLAS result buffer.");
	}

	VkAccelerationStructureCreateInfoKHR asCreateInfo{};
	asCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	asCreateInfo.buffer = rtas->ResultBuffer;
	asCreateInfo.size = sizeInfo.accelerationStructureSize;
	asCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	if (vkCreateAccelerationStructureKHRFn(Device, &asCreateInfo, nullptr, &rtas->AccelerationStructure) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan TLAS.");

	if (!CreateBufferWithMemory(
		sizeInfo.buildScratchSize,
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
std::shared_ptr<RTPipelineStateObject> VulkanBackend::CreateRTPipelineStateObject()
{
#if !CORONA_HAS_VULKAN
	ThrowNotImplemented(__FUNCTION__);
#else
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
Microsoft::WRL::ComPtr<ID3DBlob> VulkanBackend::CreateShader(const std::wstring& fileName, const std::string& entryPoint, const std::string& target) { (void)fileName; (void)entryPoint; (void)target; ThrowNotImplemented(__FUNCTION__); }
void VulkanBackend::ResetDynamicResources() {}
void VulkanBackend::CreateSwapChainForWindow(IDXGIFactory4* factory, HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format)
{
	(void)factory;
	(void)format;
#if !CORONA_HAS_VULKAN
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
	std::filesystem::remove(kVulkanValidationLogPath, resetValidationLogError);
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after reset validation log");

	std::vector<const char*> instanceExtensions = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
	std::vector<const char*> instanceLayers;
	VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo{};
	bValidationLayersEnabled = false;

	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] before validation layer query");
	const bool bEnableValidation = IsVulkanValidationEnabled();
	if (bEnableValidation && HasInstanceLayer("VK_LAYER_KHRONOS_validation") && HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
	{
		instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
		instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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

	VkWin32SurfaceCreateInfoKHR surfaceInfo{};
	surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	surfaceInfo.hinstance = GetModuleHandleW(nullptr);
	surfaceInfo.hwnd = hwnd;
	if (vkCreateWin32SurfaceKHR(Instance, &surfaceInfo, nullptr, &Surface) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan Win32 surface.");
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after vkCreateWin32SurfaceKHR");

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
	MaxUniformBufferRange = physicalDeviceProperties.limits.maxUniformBufferRange;
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::CreateSwapChainForWindow] after select physical device");
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
			descriptorIndexingFeatures.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE;
	}

	AppendVulkanRuntimeTraceBackend(
		L"[VulkanBackend::CreateSwapChainForWindow] ray tracing support ext=" + std::to_wstring(bRayTracingExtensionSupport ? 1 : 0) +
		L", feat=" + std::to_wstring(bRayTracingFeatureSupport ? 1 : 0));

	const bool bCanEnableRayTracing = bRayTracingExtensionSupport && bRayTracingFeatureSupport;

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
		rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
		rayQueryFeatures.rayQuery = VK_TRUE;
		descriptorIndexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
		descriptorIndexingFeatures.runtimeDescriptorArray = VK_TRUE;
		descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		descriptorIndexingFeatures.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;

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

	if (bCanEnableRayTracing)
		LoadRayTracingFunctionPointers();
	AppendVulkanRuntimeTraceBackend(
		L"[VulkanBackend::CreateSwapChainForWindow] ray tracing enabled=" + std::to_wstring(bRayTracingEnabled ? 1 : 0));

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
Microsoft::WRL::ComPtr<ID3D12Resource> VulkanBackend::GetSwapChainBuffer(uint32_t bufferIndex) { (void)bufferIndex; ThrowNotImplemented(__FUNCTION__); }
HRESULT VulkanBackend::CaptureTexture(Texture* source, DirectX::ScratchImage& captured, D3D12_RESOURCE_STATES beforeState) { (void)source; (void)captured; (void)beforeState; ThrowNotImplemented(__FUNCTION__); }
void VulkanBackend::InitializeImGuiBackend(HWND hwnd, DXGI_FORMAT rtvFormat)
{
	(void)hwnd;
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
void VulkanBackend::SetRenderTarget(Texture* colorTarget, Texture* depthTarget)
{
#if !CORONA_HAS_VULKAN
	(void)colorTarget; (void)depthTarget; ThrowNotImplemented(__FUNCTION__);
#else
	(void)colorTarget;
	(void)depthTarget;
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
	if (pipeline->DescriptorSetLayout == VK_NULL_HANDLE || pipeline->DescriptorPool == VK_NULL_HANDLE)
		return;

	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo descriptorSetAllocInfo{};
	descriptorSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorSetAllocInfo.descriptorPool = pipeline->DescriptorPool;
	descriptorSetAllocInfo.descriptorSetCount = 1;
	descriptorSetAllocInfo.pSetLayouts = &pipeline->DescriptorSetLayout;
	if (vkAllocateDescriptorSets(Device, &descriptorSetAllocInfo, &descriptorSet) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate Vulkan graphics descriptor set.");
	TrackFrameDescriptorSet(pipeline->DescriptorPool, descriptorSet);

	std::vector<VkDescriptorImageInfo> imageInfos;
	std::vector<VkDescriptorBufferInfo> bufferInfos;
	std::vector<VkWriteDescriptorSet> descriptorWrites;
	imageInfos.reserve(pipeline->TextureBindingSlots.size() + pipeline->SamplerBindingSlots.size());
	bufferInfos.reserve((pipeline->Desc.ConstantBufferSize > 0 ? 1 : 0) + pipeline->BufferBindingSlots.size());
	descriptorWrites.reserve(
		(pipeline->Desc.ConstantBufferSize > 0 ? 1 : 0) +
		pipeline->TextureBindingSlots.size() +
		pipeline->BufferBindingSlots.size() +
		pipeline->SamplerBindingSlots.size());

	if (pipeline->Desc.ConstantBufferSize > 0)
	{
		if (pipeline->ConstantData.empty())
			return;

		VkBuffer uniformBuffer = VK_NULL_HANDLE;
		VkDeviceSize uniformOffset = 0;
		void* mappedData = nullptr;
		if (!AllocateTransientUniform(pipeline->Desc.ConstantBufferSize, uniformBuffer, uniformOffset, &mappedData))
			return;
		std::memcpy(mappedData, pipeline->ConstantData.data(), pipeline->ConstantData.size());

		VkDescriptorBufferInfo& bufferInfo = bufferInfos.emplace_back();
		bufferInfo.buffer = uniformBuffer;
		bufferInfo.offset = uniformOffset;
		bufferInfo.range = pipeline->Desc.ConstantBufferSize;

		VkWriteDescriptorSet& writeDescriptor = descriptorWrites.emplace_back();
		writeDescriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptor.dstSet = descriptorSet;
		writeDescriptor.dstBinding = ToVulkanConstantBufferBinding(pipeline->Desc.ConstantBufferBinding);
		writeDescriptor.descriptorCount = 1;
		writeDescriptor.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writeDescriptor.pBufferInfo = &bufferInfo;
	}

	for (const auto& bindingPair : pipeline->TextureBindingSlots)
	{
		auto textureBindingIt = pipeline->BoundTextures.find(bindingPair.first);
		if (textureBindingIt == pipeline->BoundTextures.end() || textureBindingIt->second == nullptr)
			continue;
		auto textureIt = TextureAllocations.find(textureBindingIt->second);
		if (textureIt == TextureAllocations.end())
			continue;

		VkDescriptorImageInfo& imageInfo = imageInfos.emplace_back();
		imageInfo.sampler = VK_NULL_HANDLE;
		imageInfo.imageView = textureIt->second.ImageView;
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkWriteDescriptorSet& writeDescriptor = descriptorWrites.emplace_back();
		writeDescriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptor.dstSet = descriptorSet;
		writeDescriptor.dstBinding = bindingPair.second;
		writeDescriptor.descriptorCount = 1;
		writeDescriptor.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writeDescriptor.pImageInfo = &imageInfo;
	}

	for (const auto& bindingPair : pipeline->BufferBindingSlots)
	{
		auto bufferBindingIt = pipeline->BoundBuffers.find(bindingPair.first);
		if (bufferBindingIt == pipeline->BoundBuffers.end() || bufferBindingIt->second == nullptr)
			continue;
		auto bufferIt = BufferAllocations.find(bufferBindingIt->second);
		if (bufferIt == BufferAllocations.end())
			continue;

		VkDescriptorBufferInfo& bufferInfo = bufferInfos.emplace_back();
		bufferInfo.buffer = bufferIt->second.Buffer;
		bufferInfo.offset = 0;
		bufferInfo.range = bufferIt->second.SizeInBytes;

		VkWriteDescriptorSet& writeDescriptor = descriptorWrites.emplace_back();
		writeDescriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptor.dstSet = descriptorSet;
		writeDescriptor.dstBinding = bindingPair.second;
		writeDescriptor.descriptorCount = 1;
		writeDescriptor.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writeDescriptor.pBufferInfo = &bufferInfo;
	}

	for (const auto& bindingPair : pipeline->SamplerBindingSlots)
	{
		auto samplerBindingIt = pipeline->BoundSamplers.find(bindingPair.first);
		if (samplerBindingIt == pipeline->BoundSamplers.end() || samplerBindingIt->second == nullptr)
			continue;
		auto samplerIt = SamplerAllocations.find(samplerBindingIt->second);
		if (samplerIt == SamplerAllocations.end())
			continue;

		VkDescriptorImageInfo& imageInfo = imageInfos.emplace_back();
		imageInfo.sampler = samplerIt->second.SamplerHandle;
		imageInfo.imageView = VK_NULL_HANDLE;
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkWriteDescriptorSet& writeDescriptor = descriptorWrites.emplace_back();
		writeDescriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptor.dstSet = descriptorSet;
		writeDescriptor.dstBinding = bindingPair.second;
		writeDescriptor.descriptorCount = 1;
		writeDescriptor.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		writeDescriptor.pImageInfo = &imageInfo;
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
void VulkanBackend::BindMeshBuffers(VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer)
{
#if !CORONA_HAS_VULKAN
	(void)vertexBuffer; (void)indexBuffer; ThrowNotImplemented(__FUNCTION__);
#else
	if (!bRenderPassActive || !vertexBuffer || !indexBuffer)
		return;
	auto vbIt = VertexBufferAllocations.find(vertexBuffer);
	auto ibIt = IndexBufferAllocations.find(indexBuffer);
	if (vbIt == VertexBufferAllocations.end() || ibIt == IndexBufferAllocations.end())
		return;

	const VkDeviceSize offsets[] = { 0 };
	BoundVertexBuffer = vbIt->second.Buffer;
	BoundIndexBuffer = ibIt->second.Buffer;
	vkCmdBindVertexBuffers(ActiveCommandBuffer, 0, 1, &BoundVertexBuffer, offsets);
	vkCmdBindIndexBuffer(ActiveCommandBuffer, BoundIndexBuffer, 0, ibIt->second.Stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
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
	vkCmdDrawIndexed(ActiveCommandBuffer, indexCount, 1, startIndexLocation, baseVertexLocation, 0);
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

	VkImageMemoryBarrier toPresent{};
	toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toPresent.dstAccessMask = 0;
	toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toPresent.image = SwapchainImages[ActiveSwapchainImageIndex];
	toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toPresent.subresourceRange.levelCount = 1;
	toPresent.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(ActiveCommandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);

	if (vkEndCommandBuffer(ActiveCommandBuffer) != VK_SUCCESS)
		throw std::runtime_error("Failed to end Vulkan command buffer.");
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::ExecuteCurrentCommandList] end");
#endif
}
ID3D12GraphicsCommandList* VulkanBackend::GetGraphicsCommandList() { return nullptr; }
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
void VulkanBackend::TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter) { (void)buffer; (void)stateBefore; (void)stateAfter; ThrowNotImplemented(__FUNCTION__); }
Texture* VulkanBackend::GetCurrentWindowRenderTarget() { return nullptr; }
void VulkanBackend::PrepareWindowRenderTarget(Texture* renderTarget) { (void)renderTarget; }
void VulkanBackend::FinalizeWindowRenderTarget(Texture* renderTarget) { (void)renderTarget; }

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
		SwapchainImageViews.empty() &&
		VertexBufferAllocations.empty() &&
		IndexBufferAllocations.empty() &&
		BufferAllocations.empty() &&
		TextureAllocations.empty() &&
		SamplerAllocations.empty() &&
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

	for (VkImageView imageView : SwapchainImageViews)
	{
		if (imageView != VK_NULL_HANDLE)
			vkDestroyImageView(Device, imageView, nullptr);
	}
	SwapchainImageViews.clear();
	SwapchainImages.clear();
	DestroyFrameContexts();
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after swapchain image cleanup");

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
	for (const std::shared_ptr<VulkanRTAS>& rtas : RayTracingAccelerationStructures)
	{
		if (rtas)
			rtas->Release();
	}
	RayTracingAccelerationStructures.clear();
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after rtas cleanup");
	ShutdownGpuTimestampQueries();
	DestroyTransientUniformBuffer();
	for (auto& entry : VertexBufferAllocations)
	{
		if (entry.second.Buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, entry.second.Buffer, nullptr);
		if (entry.second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, entry.second.Memory, nullptr);
	}
	for (auto& entry : IndexBufferAllocations)
	{
		if (entry.second.Buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, entry.second.Buffer, nullptr);
		if (entry.second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, entry.second.Memory, nullptr);
	}
	for (auto& entry : BufferAllocations)
	{
		if (entry.second.Buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(Device, entry.second.Buffer, nullptr);
		if (entry.second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, entry.second.Memory, nullptr);
	}
	for (auto& entry : TextureAllocations)
	{
		if (entry.second.ImageView != VK_NULL_HANDLE)
			vkDestroyImageView(Device, entry.second.ImageView, nullptr);
		if (entry.second.Image != VK_NULL_HANDLE)
			vkDestroyImage(Device, entry.second.Image, nullptr);
		if (entry.second.Memory != VK_NULL_HANDLE)
			vkFreeMemory(Device, entry.second.Memory, nullptr);
	}
	for (auto& entry : SamplerAllocations)
	{
		if (entry.second.SamplerHandle != VK_NULL_HANDLE)
			vkDestroySampler(Device, entry.second.SamplerHandle, nullptr);
	}
	AppendVulkanRuntimeTraceBackend(L"[VulkanBackend::DestroyWindowContext] after resource allocation cleanup");
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
	SwapchainFormat = VK_FORMAT_UNDEFINED;
	SwapchainExtent = {};
	PendingCapturePath.clear();
	LastCapturePath.clear();
	LastCaptureError.clear();
	bLastCaptureResultValid = false;
	bLastCaptureSucceeded = false;
	VertexBufferAllocations.clear();
	IndexBufferAllocations.clear();
	BufferAllocations.clear();
	TextureAllocations.clear();
	SamplerAllocations.clear();
	RayTracingPipelines.clear();
	ComputePipelines.clear();
	GraphicsPipelines.clear();
	RayTracingAccelerationStructures.clear();
	CurrentFrameIndex = 0;
	ActiveFrameContextIndex = 0;
	NextFrameContextIndex = 0;
	bValidationLayersEnabled = false;
	TimestampValidBits = 0;
	TimestampPeriodNs = 0.0f;
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
		for (VkImageView imageView : SwapchainImageViews)
		{
			if (imageView != VK_NULL_HANDLE)
				vkDestroyImageView(Device, imageView, nullptr);
		}
		SwapchainImageViews.clear();
		SwapchainImages.clear();
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
	swapchainInfo.preTransform = surfaceCapabilities.currentTransform;
	swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapchainInfo.presentMode = ChoosePresentMode(presentModes);
	swapchainInfo.clipped = VK_TRUE;
	swapchainInfo.oldSwapchain = VK_NULL_HANDLE;
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
	for (const auto& binding : desc.TextureBindings)
		handle->TextureBindingSlots[binding.Name] = ToVulkanTextureBinding(binding.Slot);
	for (const auto& binding : desc.BufferBindings)
		handle->BufferBindingSlots[binding.Name] = ToVulkanTextureBinding(binding.Slot);
	for (const auto& binding : desc.SamplerBindings)
		handle->SamplerBindingSlots[binding.Name] = ToVulkanSamplerBinding(binding.Slot);

	const std::wstring stem = std::filesystem::path(desc.ShaderPath).stem().wstring();
	const std::vector<uint32_t> vertexSpirv = LoadSpirvFile(ResolveVulkanSpirvPath(stem + L"Vulkan.vert.spv").wstring());
	const std::vector<uint32_t> fragmentSpirv = LoadSpirvFile(ResolveVulkanSpirvPath(stem + L"Vulkan.frag.spv").wstring());

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
	std::vector<VkDescriptorPoolSize> descriptorPoolSizes;
	auto addPoolSize = [&](VkDescriptorType type)
	{
		for (auto& poolSize : descriptorPoolSizes)
		{
			if (poolSize.type == type)
			{
				++poolSize.descriptorCount;
				return;
			}
		}
		VkDescriptorPoolSize poolSize{};
		poolSize.type = type;
		poolSize.descriptorCount = 1;
		descriptorPoolSizes.push_back(poolSize);
	};

	if (desc.ConstantBufferSize > 0)
	{
		VkDescriptorSetLayoutBinding descriptorBinding{};
		descriptorBinding.binding = ToVulkanConstantBufferBinding(desc.ConstantBufferBinding);
		descriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptorBinding.descriptorCount = 1;
		descriptorBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		descriptorBindings.push_back(descriptorBinding);
		addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	}
	for (const auto& binding : desc.TextureBindings)
	{
		VkDescriptorSetLayoutBinding descriptorBinding{};
		descriptorBinding.binding = ToVulkanTextureBinding(binding.Slot);
		descriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptorBinding.descriptorCount = 1;
		descriptorBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		descriptorBindings.push_back(descriptorBinding);
		addPoolSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
	}
	for (const auto& binding : desc.BufferBindings)
	{
		VkDescriptorSetLayoutBinding descriptorBinding{};
		descriptorBinding.binding = ToVulkanTextureBinding(binding.Slot);
		descriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptorBinding.descriptorCount = 1;
		descriptorBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		descriptorBindings.push_back(descriptorBinding);
		addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	}
	for (const auto& binding : desc.SamplerBindings)
	{
		VkDescriptorSetLayoutBinding descriptorBinding{};
		descriptorBinding.binding = ToVulkanSamplerBinding(binding.Slot);
		descriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		descriptorBinding.descriptorCount = 1;
		descriptorBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		descriptorBindings.push_back(descriptorBinding);
		addPoolSize(VK_DESCRIPTOR_TYPE_SAMPLER);
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

	if (desc.ConstantBufferSize > 0)
		handle->ConstantData.resize(desc.ConstantBufferSize);

	if (!descriptorPoolSizes.empty())
	{
		VkDescriptorPoolCreateInfo descriptorPoolInfo{};
		descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		descriptorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		for (VkDescriptorPoolSize& poolSize : descriptorPoolSizes)
			poolSize.descriptorCount *= kVulkanGraphicsDescriptorSetsPerPool;
		descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(descriptorPoolSizes.size());
		descriptorPoolInfo.pPoolSizes = descriptorPoolSizes.data();
		descriptorPoolInfo.maxSets = kVulkanGraphicsDescriptorSetsPerPool;
		if (vkCreateDescriptorPool(Device, &descriptorPoolInfo, nullptr, &handle->DescriptorPool) != VK_SUCCESS)
			throw std::runtime_error("Failed to create Vulkan graphics descriptor pool.");
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
		attr.format =
			desc.VertexElements[i].Format == DXGI_FORMAT_R32G32B32_FLOAT ? VK_FORMAT_R32G32B32_SFLOAT :
			desc.VertexElements[i].Format == DXGI_FORMAT_R32G32_FLOAT ? VK_FORMAT_R32G32_SFLOAT :
			VK_FORMAT_R32G32B32A32_SFLOAT;
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

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = desc.bDepthEnable ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = desc.bDepthEnable ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = desc.bDepthEnable ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS;

	std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
	const uint32_t colorAttachmentCount = static_cast<uint32_t>(desc.ColorFormats.empty() ? 1 : desc.ColorFormats.size());
	colorBlendAttachments.resize(colorAttachmentCount);
	for (auto& colorBlendAttachment : colorBlendAttachments)
	{
		colorBlendAttachment.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	}

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.attachmentCount = colorAttachmentCount;
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
	pipelineInfo.layout = handle->Layout;
	const bool bUseSwapchainRenderPass =
		Swapchain != VK_NULL_HANDLE &&
		desc.ColorFormats.size() == 1 &&
		!desc.bDepthEnable &&
		desc.ColorFormats[0] == DXGI_FORMAT_R8G8B8A8_UNORM;
	if (!bUseSwapchainRenderPass)
	{
		std::vector<VkAttachmentDescription> attachments;
		attachments.reserve(desc.ColorFormats.size() + (desc.bDepthEnable ? 1 : 0));
		for (DXGI_FORMAT colorFormat : desc.ColorFormats)
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
			depthAttachment.format = ToVkFormat(desc.DepthFormat);
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
		subpass.pColorAttachments = colorReferences.data();
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

void VulkanBackend::BindGraphicsPipeline(GraphicsPipelineHandle* pipeline)
{
	BoundGraphicsPipeline = pipeline;
#if CORONA_HAS_VULKAN
	if (!bRenderPassActive)
	{
		auto* vkPipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(pipeline);
		if (vkPipeline && vkPipeline->CompatibleRenderPass != VK_NULL_HANDLE && !PendingOffscreenColorTargets.empty() && bFrameActive)
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
			if (PendingViewportWidth > 0 && PendingViewportHeight > 0)
				SetViewportAndScissor(PendingViewportWidth, PendingViewportHeight);
		}
	}
#endif
}

void VulkanBackend::SetGraphicsPipelineConstantData(GraphicsPipelineHandle* pipeline, uint32_t slot, const void* data, uint32_t size)
{
#if CORONA_HAS_VULKAN
	auto* vkPipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(pipeline);
	if (!vkPipeline || !data || size == 0 || slot != 0 || vkPipeline->Desc.ConstantBufferSize == 0)
		return;
	vkPipeline->ConstantData.assign(vkPipeline->Desc.ConstantBufferSize, 0);
	const uint32_t copySize = (std::min)(size, vkPipeline->Desc.ConstantBufferSize);
	std::memcpy(vkPipeline->ConstantData.data(), data, copySize);
#else
	(void)pipeline; (void)slot; (void)data; (void)size;
#endif
}

void VulkanBackend::BindGraphicsPipelineTexture(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Texture* texture)
{
#if CORONA_HAS_VULKAN
	auto* vkPipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(pipeline);
	if (!vkPipeline || !texture)
		return;
	vkPipeline->BoundTextures[bindingName] = texture;
#else
	(void)pipeline;
	(void)bindingName;
	(void)texture;
#endif
}

void VulkanBackend::BindGraphicsPipelineBuffer(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Buffer* buffer)
{
#if CORONA_HAS_VULKAN
	auto* vkPipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(pipeline);
	if (!vkPipeline || !buffer)
		return;
	vkPipeline->BoundBuffers[bindingName] = buffer;
#else
	(void)pipeline;
	(void)bindingName;
	(void)buffer;
#endif
}

void VulkanBackend::BindGraphicsPipelineSampler(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Sampler* sampler)
{
#if CORONA_HAS_VULKAN
	auto* vkPipeline = dynamic_cast<VulkanGraphicsPipelineHandle*>(pipeline);
	if (!vkPipeline || !sampler)
		return;
	vkPipeline->BoundSamplers[bindingName] = sampler;
#else
	(void)pipeline;
	(void)bindingName;
	(void)sampler;
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
