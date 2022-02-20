#pragma once

#include <vulkan/vulkan.h>
#include <Windows.h>
#include <vector>
#include <memory>
#include <string>
#include <optional>
#include <map>
#include <glm/glm.hpp>

#include "AbstractGfxLayer.h"

struct TextureCreateInfo
{
	VkFormat format;
	uint32_t width;
	uint32_t height;
	uint32_t depth = 1;
	uint32_t mipLevels = 1;
	uint32_t arrayLayers = 1;
};

class VKTexture : public GfxTexture
{
public:
	TextureCreateInfo textureInfo;

	VkImage image;
	VkDeviceMemory mem;
	VkImageView view;

	VkFramebuffer framebuffer = VK_NULL_HANDLE;

	bool Create(TextureCreateInfo* info);
	bool CreateFromImage(VkImage img, TextureCreateInfo* info);

	VKTexture() {}
	virtual ~VKTexture();
};

struct UniformBufferCreateInfo
{
	uint32_t size;
};

class VKUniformBuffer
{
public:
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDescriptorBufferInfo buffer_info;

	uint8_t* pData = nullptr;

	bool Create(UniformBufferCreateInfo* info);
	VKUniformBuffer() {}
	virtual ~VKUniformBuffer();
};

struct VertexBufferInfo
{
	uint32_t size;
	void* pData;
};

class VKVertexBuffer
{
public:
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDescriptorBufferInfo buffer_info;

	bool Create(VertexBufferInfo* info);

	VKVertexBuffer(){}
	virtual ~VKVertexBuffer();
};


struct VKShaderCreateInfo
{
	std::wstring dir;
	std::wstring file;
	std::wstring entryPoint;
	std::wstring target;

	std::vector<std::string> defines;
};

struct VKVertexElement
{
	VkFormat format;
	int offset;
};

struct VKVertexInputLayoutInfo
{
	uint32_t vertexStride;
	std::vector<VKVertexElement> vertexElementVec;
};

struct PSOCreateInfo
{
	VkSampleCountFlagBits samples;

	VkFormat depthFormat;
	std::vector<VkFormat> colorFormatVec;

	/*std::vector<VkImageView> colorViewVec;
	VkImageView depthView;*/

	VKShaderCreateInfo* vsInfo;
	VKShaderCreateInfo* psInfo;

	VKVertexInputLayoutInfo* vertexInputLayoutInfo;

	VkPrimitiveTopology	topology;

};
class VKPipelineStateObject : public GfxPipelineStateObject
{
public:
	
	struct BindingData
	{
		std::string name;
		uint32_t BindingIndex;
		VKUniformBuffer* uniformBuffer;
		// sampler
		// texture
		// rw texture
	};
	std::map<std::string, BindingData> uniformBinding;
	std::map<std::string, BindingData> samplerBinding;
	std::map<std::string, BindingData> textureBinding;


	VkPipelineLayout pipeline_layout;
	std::vector<VkDescriptorSetLayout> desc_layout;
	std::vector<VkDescriptorSet> desc_set;

	VkRenderPass render_pass;

	VkVertexInputBindingDescription vi_binding;
	std::vector<VkVertexInputAttributeDescription> vi_attribs;

	VkPipeline pipeline;

	std::map<VKTexture*, VkFramebuffer> frameBuffers;
	VkFramebuffer currentFBO;
	// max 32 descriptor sets per pipeline.
	// strategy 1
	// descriptor set for resource type(uniform buffer, texture, image)


	VKPipelineStateObject(){}
	virtual ~VKPipelineStateObject();

	void BindUniform(std::string name, int index);
	void BindSampler(std::string name, int index);
	void BindTexture(std::string name, int index);

	void SetUniformBuffer(std::string name, VKUniformBuffer* buffer);

	void SetRendertargets(std::vector<VKTexture*> colorTargets, VKTexture* depthTarget);

	void Apply();

	bool Create(PSOCreateInfo* info);
};

class VulkanImpl
{
public:
	VkPhysicalDeviceProperties physical_device_props = {};

	VkInstance Instance;
	std::vector<VkPhysicalDevice> vecGPU;
	VkDevice Device;
	VkQueue GraphicsQueue;
	VkSurfaceKHR Surface;
	VkSwapchainKHR swap_chain;
	VkCommandPool cmd_pool;
	VkCommandBuffer cmd;


	VkPhysicalDeviceMemoryProperties memory_properties;

	uint32_t swapchainImageCount;
	std::vector<VkImage> swapchainImages;

	// pso
	VkDescriptorPool desc_pool;


	// validation layer
	PFN_vkCreateDebugReportCallbackEXT dbgCreateDebugReportCallback;
	PFN_vkDestroyDebugReportCallbackEXT dbgDestroyDebugReportCallback;
	PFN_vkDebugReportMessageEXT dbgBreakCallback;
	VkDebugReportCallbackEXT debug_report_callback;
	std::vector<VkDebugReportCallbackEXT> debug_report_callbacks;

	//
	glm::mat4 MVP;

	std::shared_ptr<VKTexture> depthTexture;
	std::vector<std::shared_ptr<VKTexture>> frameBufferTextures;
	std::vector<VkFramebuffer> frameBuffers;

	

	std::shared_ptr<VKUniformBuffer> uniformBuffer;

	std::shared_ptr< VKPipelineStateObject> pipeline;



	VkPipelineShaderStageCreateInfo shaderStages[2];

public:
	bool Init(HINSTANCE hInstance, HWND hWnd, UINT DisplayWidth, UINT DisplayHeight);

	void GetFrameBuffers(std::vector<std::shared_ptr<VKTexture>>& vkFrameFuffers);

	VulkanImpl();
	~VulkanImpl();
};

