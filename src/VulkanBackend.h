#pragma once

#include "RenderBackend.h"

#if CORONA_HAS_VULKAN
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <array>
#include <unordered_map>
#include <utility>
#include <vulkan/vulkan.h>
#endif

class VulkanBackend;

#if CORONA_HAS_VULKAN
struct VulkanRTAS : RTAS
{
	VulkanBackend* Owner = nullptr;
	VkAccelerationStructureKHR AccelerationStructure = VK_NULL_HANDLE;
	VkBuffer ScratchBuffer = VK_NULL_HANDLE;
	VkDeviceMemory ScratchMemory = VK_NULL_HANDLE;
	VkBuffer ResultBuffer = VK_NULL_HANDLE;
	VkDeviceMemory ResultMemory = VK_NULL_HANDLE;
	VkBuffer InstanceBuffer = VK_NULL_HANDLE;
	VkDeviceMemory InstanceMemory = VK_NULL_HANDLE;
	VkDeviceAddress DeviceAddress = 0;

	void Release();
	~VulkanRTAS() override;
};

struct VulkanGraphicsPipelineHandle : GraphicsPipelineHandle
{
	GraphicsPipelineDesc Desc;
	VkPipelineLayout Layout = VK_NULL_HANDLE;
	VkPipeline Pipeline = VK_NULL_HANDLE;
	VkRenderPass CompatibleRenderPass = VK_NULL_HANDLE;
	VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
	VkBuffer UniformBuffer = VK_NULL_HANDLE;
	VkDeviceMemory UniformBufferMemory = VK_NULL_HANDLE;
	void* UniformBufferMapped = nullptr;
	std::vector<uint8_t> ConstantData;
	VkShaderModule VertexShaderModule = VK_NULL_HANDLE;
	VkShaderModule FragmentShaderModule = VK_NULL_HANDLE;
	VulkanBackend* Owner = nullptr;
	std::unordered_map<std::string, uint32_t> TextureBindingSlots;
	std::unordered_map<std::string, uint32_t> BufferBindingSlots;
	std::unordered_map<std::string, uint32_t> SamplerBindingSlots;
	std::unordered_map<std::string, Texture*> BoundTextures;
	std::unordered_map<std::string, Buffer*> BoundBuffers;
	std::unordered_map<std::string, Sampler*> BoundSamplers;

	void Release();
	~VulkanGraphicsPipelineHandle() override;
};

struct VulkanRTPipelineStateObject : RTPipelineStateObject
{
	VulkanBackend* Owner = nullptr;

	struct BindingDesc
	{
		std::string Shader;
		std::string Name;
		uint32_t BaseRegister = 0;
		uint32_t DescriptorBinding = 0;
		uint32_t DataSize = 0;
	};

	struct ShaderDesc
	{
		std::string Name;
		ShaderType Type = GLOBAL;
	};

	struct HitGroupDesc
	{
		std::string Name;
		std::string ClosestHitShader;
		std::string AnyHitShader;
	};

	struct ResourceBindingValue
	{
		Texture* TextureValue = nullptr;
		Buffer* BufferValue = nullptr;
		VertexBuffer* VertexBufferValue = nullptr;
		IndexBuffer* IndexBufferValue = nullptr;
		Sampler* SamplerValue = nullptr;
		std::shared_ptr<RTAS> RTASValue;
		std::vector<uint8_t> ConstantData;
	};

	uint32_t NumInstances = 0;
	uint32_t MaxRecursion = 1;
	uint32_t MaxPayloadSizeInBytes = 0;
	uint32_t MaxAttributeSizeInBytes = 0;
	std::string ShaderFile;

	std::vector<ShaderDesc> Shaders;
	std::vector<HitGroupDesc> HitGroups;
	std::vector<BindingDesc> UAVBindings;
	std::vector<BindingDesc> SRVBindings;
	std::vector<BindingDesc> SamplerBindings;
	std::vector<BindingDesc> CBVBindings;
	VkShaderModule ShaderModule = VK_NULL_HANDLE;
	struct ShaderStageData
	{
		VkPipelineShaderStageCreateInfo CreateInfo{};
		std::string EntryPoint;
	};
	std::vector<ShaderStageData> ShaderStages;
	std::vector<VkRayTracingShaderGroupCreateInfoKHR> ShaderGroups;
	VkPipeline Pipeline = VK_NULL_HANDLE;
	VkDescriptorSet DescriptorSet = VK_NULL_HANDLE;
	VkBuffer ShaderBindingTableBuffer = VK_NULL_HANDLE;
	VkDeviceMemory ShaderBindingTableMemory = VK_NULL_HANDLE;
	VkStridedDeviceAddressRegionKHR RaygenRegion{};
	VkStridedDeviceAddressRegionKHR MissRegion{};
	VkStridedDeviceAddressRegionKHR HitRegion{};
	VkStridedDeviceAddressRegionKHR CallableRegion{};
	VkDeviceSize PipelineStackSize = 0;
	VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
	VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
	std::unordered_map<std::string, ResourceBindingValue> GlobalBindingValues;
	std::unordered_map<uint32_t, std::vector<ResourceBindingValue>> HitProgramBindingValues;
	std::vector<VkBuffer> TempUniformBuffers;
	std::vector<VkDeviceMemory> TempUniformMemories;

	void Release();
	~VulkanRTPipelineStateObject() override;
	void ReleaseTempUniformBuffers();

	void SetNumInstances(uint32_t numInstances) override;
	void Configure(uint32_t maxRecursion, uint32_t maxPayloadSizeInBytes, uint32_t maxAttributeSizeInBytes) override;
	void AddHitGroup(const std::string& name, const std::string& chs, const std::string& ahs) override;
	void AddShader(const std::string& shader, ShaderType shaderType) override;
	void BindUAV(const std::string& shader, const std::string& name, uint32_t baseRegister) override;
	void BindSRV(const std::string& shader, const std::string& name, uint32_t baseRegister) override;
	void BindSampler(const std::string& shader, const std::string& name, uint32_t baseRegister) override;
	void BindCBV(const std::string& shader, const std::string& name, uint32_t baseRegister, uint32_t size, uint32_t numInstance) override;
	void SetShaderDefine(const std::string& name, const std::string& value) override;
	void SetShaderLibraryTarget(const std::string& target) override;
	void BeginShaderTable() override;
	void EndShaderTable() override;
	void SetTextureUAV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex = -1) override;
	void SetBufferUAV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int instanceIndex = -1) override;
	void SetTextureSRV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex = -1) override;
	void SetBufferSRV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int instanceIndex = -1) override;
	void SetAccelerationStructure(const std::string& shader, const std::string& bindingName, const std::shared_ptr<RTAS>& rtas, int instanceIndex = -1) override;
	void SetSampler(const std::string& shader, const std::string& bindingName, Sampler* sampler, int instanceIndex = -1) override;
	void SetCBVValue(const std::string& shader, const std::string& bindingName, void* pData, int instanceIndex = -1) override;
	void ResetHitProgram(uint32_t instanceIndex) override;
	void StartHitProgram(const std::string& hitGroup, uint32_t instanceIndex) override;
	void AddTextureSRVToHitProgram(const std::string& hitGroup, Texture* texture, uint32_t instanceIndex) override;
	void AddBufferSRVToHitProgram(const std::string& hitGroup, Buffer* buffer, uint32_t instanceIndex) override;
	void AddSceneGeometrySRVsToHitProgram(const std::string& hitGroup, VertexBuffer* sceneVertexBuffer, IndexBuffer* sceneIndexBuffer, uint32_t instanceIndex) override;
	bool InitRS(const std::string& shaderFile) override;
	void Apply(uint32_t width, uint32_t height) override;
};

struct VulkanComputePipelineStateObject : ComputePipelineStateObject
{
	VulkanBackend* Owner = nullptr;

	struct BindingDesc
	{
		std::string Name;
		uint32_t BaseRegister = 0;
		uint32_t DescriptorBinding = 0;
		uint32_t DescriptorCount = 1;
		uint32_t DataSize = 0;
		VkDescriptorType DescriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
	};

	struct ResourceBindingValue
	{
		Texture* TextureValue = nullptr;
		Buffer* BufferValue = nullptr;
		Sampler* SamplerValue = nullptr;
		std::vector<uint8_t> ConstantData;
	};

	std::wstring ShaderFile;
	std::string EntryPoint;
	std::vector<BindingDesc> SRVBindings;
	std::vector<BindingDesc> UAVBindings;
	std::vector<BindingDesc> SamplerBindings;
	std::vector<BindingDesc> CBVBindings;
	std::unordered_map<std::string, ResourceBindingValue> BindingValues;
	std::vector<VkBuffer> TempUniformBuffers;
	std::vector<VkDeviceMemory> TempUniformMemories;

	VkShaderModule ShaderModule = VK_NULL_HANDLE;
	VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet DescriptorSet = VK_NULL_HANDLE;
	VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
	VkPipeline Pipeline = VK_NULL_HANDLE;

	void Release();
	~VulkanComputePipelineStateObject() override;
	void ReleaseTempUniformBuffers();

	void BindSRV(const std::string& name, uint32_t baseRegister, uint32_t numDescriptors) override;
	void BindUAV(const std::string& name, uint32_t baseRegister) override;
	void BindCBV(const std::string& name, uint32_t baseRegister, uint32_t size) override;
	void BindSampler(const std::string& name, uint32_t baseRegister) override;
	bool InitCS(const std::wstring& shaderFile, const std::string& entryPoint) override;
	void Apply() override;
	void SetTextureSRV(const std::string& name, Texture* texture) override;
	void SetTextureUAV(const std::string& name, Texture* texture) override;
	void SetBufferSRV(const std::string& name, Buffer* buffer) override;
	void SetBufferUAV(const std::string& name, Buffer* buffer) override;
	void SetSampler(const std::string& name, Sampler* sampler) override;
	void SetCBVValue(const std::string& name, void* pData) override;
};
#else
struct VulkanGraphicsPipelineHandle;
struct VulkanRTPipelineStateObject;
struct VulkanComputePipelineStateObject;
#endif

class VulkanBackend : public IRenderBackend
{
	friend struct VulkanRTAS;
	friend struct VulkanGraphicsPipelineHandle;
	friend struct VulkanRTPipelineStateObject;
	friend struct VulkanComputePipelineStateObject;

public:
	VulkanBackend() = default;
	~VulkanBackend() override;

	ERenderBackendAPI GetAPI() const override { return ERenderBackendAPI::Vulkan; }
	const char* GetBackendName() const override { return "Vulkan"; }
	uint32_t GetMaxSupportedHybridStage() const override { return 7u; }

	void BeginFrame() override;
	void EndFrame() override;
	void WaitForGpu() override;
	void EmitGpuCrashMarker(const char* markerName) override;
	const std::string& GetErrorString() const override;
	void ClearErrorString() override;
	uint64_t GetTimestampFrequency() const override;
	uint32_t GetFrameCount() const override;
	uint32_t GetCurrentFrameIndex() const override;
	DX12Backend* AsDX12Backend() override;
	std::shared_ptr<Texture> CreateTexture2D(const TextureCreateDesc& desc) override;
	std::shared_ptr<Buffer> CreateBuffer(const BufferCreateDesc& desc) override;
	std::shared_ptr<Sampler> CreateSampler(const SamplerCreateDesc& desc) override;
	std::shared_ptr<Texture> CreateTextureFromFile(const std::wstring& fileName, bool nonSRGB) override;
	std::shared_ptr<Texture> CreateTexture3D(ETextureFormat format, ETextureUsageFlags usage, EInitialResourceState initialState, int width, int height, int depth, int mipLevels) override;
	void UploadTexture3D(Texture* texture, const void* data, uint64_t rowPitch, uint64_t slicePitch) override;
	std::shared_ptr<VertexBuffer> CreateVertexBuffer(uint32_t size, uint32_t stride, void* srcData) override;
	std::shared_ptr<IndexBuffer> CreateIndexBuffer(EIndexFormat format, uint32_t size, void* srcData) override;
	std::shared_ptr<RTAS> CreateBLASForMesh(Mesh* mesh) override;
	std::shared_ptr<RTAS> CreateTLAS(const std::vector<RTInstanceDesc>& instances) override;
	bool UpdateTLAS(const std::shared_ptr<RTAS>& topLevelAS, const std::vector<RTInstanceDesc>& instances) override;
	std::shared_ptr<RTPipelineStateObject> CreateRTPipelineStateObject() override;
	std::shared_ptr<ComputePipelineStateObject> CreateComputePipelineStateObject() override;
	ShaderBytecode CreateShader(const std::wstring& fileName, const std::string& entryPoint, const std::string& target) override;
	void ResetDynamicResources() override;
	void CreateSwapChainForWindow(WindowHandle window, uint32_t width, uint32_t height, ETextureFormat format) override;
	std::shared_ptr<Texture> GetSwapChainTexture(uint32_t bufferIndex) override;
	bool CaptureTexture(Texture* source, CapturedImage& captured, EResourceState beforeState) override;
	void InitializeImGuiBackend(WindowHandle window, ETextureFormat rtvFormat) override;
	void NewImGuiFrame() override;
	void RenderImGuiDrawData(ImDrawData* drawData) override;
	void ShutdownImGuiBackend() override;
	void InitializeGpuTimestampQueries(uint32_t queryCount) override;
	void ShutdownGpuTimestampQueries() override;
	void WriteGpuTimestamp(uint32_t queryIndex) override;
	void ResolveGpuTimestampRange(uint32_t startQueryIndex, uint32_t queryCount) override;
	uint64_t ReadGpuTimestampValue(uint32_t queryIndex) const override;
	void InitializeOcclusionQueries(uint32_t queryCount) override;
	void ShutdownOcclusionQueries() override;
	void BeginOcclusionQuery(uint32_t queryIndex) override;
	void EndOcclusionQuery(uint32_t queryIndex) override;
	void ResolveOcclusionQueryRange(uint32_t startQueryIndex, uint32_t queryCount) override;
	uint64_t ReadOcclusionQueryValue(uint32_t queryIndex) const override;
	void SetRenderTarget(Texture* colorTarget, Texture* depthTarget = nullptr) override;
	void SetRenderTargets(Texture* const* colorTargets, uint32_t colorTargetCount, Texture* depthTarget = nullptr) override;
	void ClearRenderTarget(Texture* colorTarget, const float clearColor[4]) override;
	void ClearDepth(Texture* depthTarget, float depthValue) override;
	void BindDefaultDescriptorHeaps() override;
	void SetViewportAndScissor(uint32_t width, uint32_t height) override;
	void DrawFullscreenQuad(VertexBuffer* vertexBuffer) override;
	void BindMeshBuffers(VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer) override;
	void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) override;
	void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
	void ClearTextureUAVFloat(Texture* texture, const float clearColor[4]) override;
	void ExecuteCurrentCommandList() override;
	void BeginGpuMarker(uint64_t color, const char* label) override;
	void EndGpuMarker() override;
	void TransitionTexture(Texture* texture, EResourceState stateBefore, EResourceState stateAfter) override;
	void TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter) override;
	Texture* GetCurrentWindowRenderTarget() override;
	void PrepareWindowRenderTarget(Texture* renderTarget) override;
	void FinalizeWindowRenderTarget(Texture* renderTarget) override;
	void RequestWindowCapture(const std::wstring& outputPath) override;
	bool ConsumeWindowCaptureResult(std::wstring* outputPath, bool* success, std::wstring* errorMessage) override;
	std::shared_ptr<GraphicsPipelineHandle> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
	void BindGraphicsPipeline(GraphicsPipelineHandle* pipeline) override;
	void SetGraphicsPipelineConstantData(GraphicsPipelineHandle* pipeline, uint32_t slot, const void* data, uint32_t size) override;
	void BindGraphicsPipelineTexture(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Texture* texture) override;
	void BindGraphicsPipelineBuffer(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Buffer* buffer) override;
	void BindGraphicsPipelineSampler(GraphicsPipelineHandle* pipeline, const std::string& bindingName, Sampler* sampler) override;
	void PreviewTextureOnWindow(Texture* texture);

private:
	[[noreturn]] void ThrowNotImplemented(const char* functionName) const;
#if CORONA_HAS_VULKAN
	bool CreateBufferWithMemory(
		VkDeviceSize size,
		VkBufferUsageFlags usage,
		VkMemoryPropertyFlags properties,
		bool bEnableDeviceAddress,
		VkBuffer& outBuffer,
		VkDeviceMemory& outMemory);
	bool CreateDeviceLocalBufferWithUpload(
		VkDeviceSize size,
		VkBufferUsageFlags usage,
		bool bEnableDeviceAddress,
		const void* srcData,
		VkBuffer& outBuffer,
		VkDeviceMemory& outMemory);
	VkDeviceAddress GetBufferDeviceAddress(VkBuffer buffer) const;
	bool InitializeTransientUniformBuffer(VkDeviceSize bytesPerFrame);
	void DestroyTransientUniformBuffer();
	bool AllocateTransientUniform(VkDeviceSize size, VkBuffer& outBuffer, VkDeviceSize& outOffset, void** outMappedData);
	bool UploadTransientUniformData(
		const void* data,
		VkDeviceSize size,
		VkBuffer& outBuffer,
		VkDeviceSize& outOffset,
		VkDeviceSize& outRange,
		std::vector<VkBuffer>& fallbackBuffers,
		std::vector<VkDeviceMemory>& fallbackMemories);
	void LoadRayTracingFunctionPointers();
	void BindGraphicsPipelineForDraw(VulkanGraphicsPipelineHandle* pipeline);
	void DestroyWindowContext();
	void DestroyFrameContexts();
	void TrackFrameDescriptorSet(VkDescriptorPool descriptorPool, VkDescriptorSet descriptorSet);
	void CreateWindowTrianglePipeline(VkFormat swapchainFormat);
	void RecreateSwapchain(uint32_t width, uint32_t height);
	bool CaptureCurrentSwapchainImageToPNG(uint32_t imageIndex, const std::wstring& outputPath, std::wstring* errorMessage);
#else
	void DestroyWindowContext();
#endif

private:
	std::string ErrorString;
	uint32_t CurrentFrameIndex = 0;

#if CORONA_HAS_VULKAN
	struct VulkanBufferAllocation
	{
		VkBuffer Buffer = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		uint32_t Stride = 0;
		uint32_t SizeInBytes = 0;
	};

	struct VulkanTextureAllocation
	{
		VkImage Image = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkImageView ImageView = VK_NULL_HANDLE;
		VkFormat Format = VK_FORMAT_UNDEFINED;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t Depth = 1;
		ETextureUsageFlags Usage = TextureUsage_None;
		VkImageLayout CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	struct VulkanSamplerAllocation
	{
		VkSampler SamplerHandle = VK_NULL_HANDLE;
	};

	struct VulkanFrameContext
	{
		VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
		VkSemaphore ImageAvailableSemaphore = VK_NULL_HANDLE;
		VkSemaphore RenderFinishedSemaphore = VK_NULL_HANDLE;
		VkFence InFlightFence = VK_NULL_HANDLE;
		std::vector<std::pair<VkDescriptorPool, VkDescriptorSet>> DescriptorSetsToFree;
		std::vector<VkFramebuffer> FramebuffersToDestroy;
	};

	VkInstance Instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
	VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
	VkDevice Device = VK_NULL_HANDLE;
	VkSurfaceKHR Surface = VK_NULL_HANDLE;
	VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
	VkQueue GraphicsQueue = VK_NULL_HANDLE;
	uint32_t GraphicsQueueFamilyIndex = UINT32_MAX;
	uint32_t PresentQueueFamilyIndex = UINT32_MAX;
	VkCommandPool CommandPool = VK_NULL_HANDLE;
	VkRenderPass RenderPass = VK_NULL_HANDLE;
	VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
	VkPipeline GraphicsPipeline = VK_NULL_HANDLE;
	VkShaderModule VertexShaderModule = VK_NULL_HANDLE;
	VkShaderModule FragmentShaderModule = VK_NULL_HANDLE;
	VkFormat SwapchainFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D SwapchainExtent = {};
	std::vector<VkImage> SwapchainImages;
	std::vector<VkImageView> SwapchainImageViews;
	std::vector<VkFramebuffer> SwapchainFramebuffers;
	std::vector<VulkanFrameContext> FrameContexts;
	uint32_t ActiveFrameContextIndex = 0;
	uint32_t NextFrameContextIndex = 0;
	VkDescriptorPool ImGuiDescriptorPool = VK_NULL_HANDLE;
	bool bImGuiBackendInitialized = false;
	std::wstring PendingCapturePath;
	std::wstring LastCapturePath;
	std::wstring LastCaptureError;
	bool bLastCaptureResultValid = false;
	bool bLastCaptureSucceeded = false;
	std::vector<Texture*> PendingOffscreenColorTargets;
	Texture* PendingOffscreenDepthTarget = nullptr;
	VkFramebuffer ActiveOffscreenFramebuffer = VK_NULL_HANDLE;
	std::vector<VkFramebuffer> TransientOffscreenFramebuffers;
	std::unordered_map<Texture*, std::array<float, 4>> PendingTextureClearColors;
	std::unordered_map<Texture*, float> PendingDepthClearValues;
	bool bFrameActive = false;
	bool bRenderPassActive = false;
	bool bViewportBound = false;
	float PendingClearColor[4] = { 0.04f, 0.05f, 0.08f, 1.0f };
	VkCommandBuffer ActiveCommandBuffer = VK_NULL_HANDLE;
	uint32_t ActiveSwapchainImageIndex = 0;
	VkBuffer BoundVertexBuffer = VK_NULL_HANDLE;
	VkBuffer BoundIndexBuffer = VK_NULL_HANDLE;
	GraphicsPipelineHandle* BoundGraphicsPipeline = nullptr;
	uint32_t PendingViewportWidth = 0;
	uint32_t PendingViewportHeight = 0;
	VkQueryPool TimestampQueryPool = VK_NULL_HANDLE;
	uint32_t TimestampQueryCount = 0;
	uint32_t TimestampQueriesPerFrame = 0;
	uint32_t TimestampValidBits = 0;
	float TimestampPeriodNs = 0.0f;
	bool bTimestampQueriesResetForCurrentFrame = false;
	VkBuffer TransientUniformBuffer = VK_NULL_HANDLE;
	VkDeviceMemory TransientUniformMemory = VK_NULL_HANDLE;
	void* TransientUniformMapped = nullptr;
	VkDeviceSize TransientUniformBytesPerFrame = 0;
	VkDeviceSize TransientUniformFrameOffset = 0;
	uint32_t TransientUniformFrameCount = 0;
	VkDeviceSize UniformBufferAlignment = 256;
	VkDeviceSize MaxUniformBufferRange = 0;
	uint64_t TransientUniformOverflowCount = 0;
	std::unordered_map<Buffer*, VulkanBufferAllocation> BufferAllocations;
	std::unordered_map<VertexBuffer*, VulkanBufferAllocation> VertexBufferAllocations;
	std::unordered_map<IndexBuffer*, VulkanBufferAllocation> IndexBufferAllocations;
	std::unordered_map<Texture*, VulkanTextureAllocation> TextureAllocations;
	std::unordered_map<Sampler*, VulkanSamplerAllocation> SamplerAllocations;
	std::vector<std::shared_ptr<VulkanGraphicsPipelineHandle>> GraphicsPipelines;
	std::vector<std::shared_ptr<VulkanRTPipelineStateObject>> RayTracingPipelines;
	std::vector<std::shared_ptr<VulkanComputePipelineStateObject>> ComputePipelines;
	std::vector<std::shared_ptr<VulkanRTAS>> RayTracingAccelerationStructures;
	bool bValidationLayersEnabled = false;
	bool bSamplerAnisotropySupported = false;
	bool bRayTracingExtensionSupport = false;
	bool bRayTracingFeatureSupport = false;
	bool bRayTracingEnabled = false;
	PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHRFn = nullptr;
	PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHRFn = nullptr;
	PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHRFn = nullptr;
	PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHRFn = nullptr;
	PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHRFn = nullptr;
	PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHRFn = nullptr;
	PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHRFn = nullptr;
	PFN_vkGetRayTracingShaderGroupStackSizeKHR vkGetRayTracingShaderGroupStackSizeKHRFn = nullptr;
	PFN_vkCmdSetRayTracingPipelineStackSizeKHR vkCmdSetRayTracingPipelineStackSizeKHRFn = nullptr;
	PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHRFn = nullptr;
	PFN_vkGetBufferDeviceAddressKHR vkGetBufferDeviceAddressKHRFn = nullptr;
	PFN_vkCmdInsertDebugUtilsLabelEXT vkCmdInsertDebugUtilsLabelEXTFn = nullptr;
#endif
};
