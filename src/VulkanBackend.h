#pragma once

#include "RHIBuildConfig.h"
#include "RenderBackend.h"
#include "RenderResources.h"

#if CORONA_HAS_VULKAN
#if CORONA_PLATFORM_IS_WINDOWS
#  ifndef VK_USE_PLATFORM_WIN32_KHR
#    define VK_USE_PLATFORM_WIN32_KHR 1
#  endif
#elif CORONA_PLATFORM_IS_ANDROID
#  ifndef VK_USE_PLATFORM_ANDROID_KHR
#    define VK_USE_PLATFORM_ANDROID_KHR 1
#  endif
#elif CORONA_PLATFORM_IS_MACOS || CORONA_PLATFORM_IS_IOS
#  ifndef VK_USE_PLATFORM_METAL_EXT
#    define VK_USE_PLATFORM_METAL_EXT 1
#  endif
#endif
#include <array>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vulkan/vulkan.h>
#endif

class VulkanBackend;

#if CORONA_HAS_VULKAN
struct VulkanGraphicsPipelineHandle;

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
	uint32_t PrimitiveCount = 0;
	uint32_t NumInstances = 0;
	bool bAllowUpdate = false;
	bool bIsTopLevel = false;

	void Release();
	~VulkanRTAS() override;
};

struct VulkanGraphicsBindGroupHandle : GraphicsBindGroupHandle
{
	struct TextureBinding
	{
		uint32_t Binding = 0;
		Texture* TextureValue = nullptr;
	};

	struct BufferBinding
	{
		uint32_t Binding = 0;
		Buffer* BufferValue = nullptr;
		VertexBuffer* VertexBufferValue = nullptr;
	};

	struct SamplerBinding
	{
		uint32_t Binding = 0;
		Sampler* SamplerValue = nullptr;
	};

	VulkanGraphicsPipelineHandle* Pipeline = nullptr;
	std::vector<TextureBinding> Textures;
	std::vector<BufferBinding> Buffers;
	std::vector<SamplerBinding> Samplers;
	std::vector<uint8_t> ConstantData;
	bool bHasConstantData = false;
	uint32_t ConstantDataBinding = 0;
	uint32_t ConstantDataSize = 0;
	uint32_t Slot = 0;
};

struct VulkanGraphicsPipelineHandle : GraphicsPipelineHandle
{
	GraphicsPipelineDesc Desc;
	VkPipelineLayout Layout = VK_NULL_HANDLE;
	VkPipeline Pipeline = VK_NULL_HANDLE;
	VkRenderPass CompatibleRenderPass = VK_NULL_HANDLE;
	VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;
	std::vector<VkDescriptorSetLayout> DescriptorSetLayouts;
	VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
	VkBuffer UniformBuffer = VK_NULL_HANDLE;
	VkDeviceMemory UniformBufferMemory = VK_NULL_HANDLE;
	void* UniformBufferMapped = nullptr;
	VkShaderModule VertexShaderModule = VK_NULL_HANDLE;
	VkShaderModule FragmentShaderModule = VK_NULL_HANDLE;
	VulkanBackend* Owner = nullptr;
	std::unordered_map<std::string, uint32_t> TextureBindingSlots;
	std::unordered_map<std::string, uint32_t> BufferBindingSlots;
	std::unordered_map<std::string, uint32_t> SamplerBindingSlots;
	std::array<std::shared_ptr<VulkanGraphicsBindGroupHandle>, kMaxGraphicsBindGroupSlots> BoundBindGroups;
	bool bHasConstantBufferDescriptorBinding = false;
	uint32_t ConstantBufferDescriptorBinding = 0;
	bool bUsesBindlessTextureTable = false;
	bool bUsesBindlessBufferTable = false;

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
		uint32_t DescriptorSet = 0;
		uint32_t BaseRegister = 0;
		uint32_t DescriptorBinding = 0;
		uint32_t DataSize = 0;
		RHIBindingDesc Schema;
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
	std::vector<std::pair<std::string, std::string>> ShaderDefines;
	std::string ShaderLibraryTarget;
	bool bShaderTableOpen = false;
	bool bShaderTableFinalized = false;

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
	std::vector<VkDescriptorSetLayout> DescriptorSetLayouts;
	std::vector<bool> DescriptorSetLayoutOwned;
	std::vector<uint32_t> LocalDescriptorSetNumbers;
	std::vector<VkDescriptorSet> ActiveDescriptorSets;
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
	void BindUAV(const std::string& shader, const RHIBindingDesc& binding) override;
	void BindSRV(const std::string& shader, const RHIBindingDesc& binding) override;
	void BindSampler(const std::string& shader, const RHIBindingDesc& binding) override;
	void BindCBV(const std::string& shader, const RHIBindingDesc& binding) override;
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
	bool SetBindlessTextureTable(const std::string& shader, const std::string& bindingName) override;
	bool SetBindlessBufferTable(const std::string& shader, const std::string& bindingName) override;
	void SetAccelerationStructure(const std::string& shader, const std::string& bindingName, const std::shared_ptr<RTAS>& rtas, int instanceIndex = -1) override;
	void SetSampler(const std::string& shader, const std::string& bindingName, Sampler* sampler, int instanceIndex = -1) override;
	void SetCBVValue(const std::string& shader, const std::string& bindingName, void* pData, int instanceIndex = -1) override;
	void ResetHitProgram(uint32_t instanceIndex) override;
	void StartHitProgram(const std::string& hitGroup, uint32_t instanceIndex) override;
	bool InitRS(const std::string& shaderFile) override;
	void Apply(uint32_t width, uint32_t height) override;
	bool GetDispatchRaysIndirectTemplate(uint32_t width, uint32_t height, RtDispatchRaysIndirectTemplate& outTemplate) const override;
	bool ApplyIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset) override;
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
		RHIBindingDesc Schema;
	};

	struct ResourceBindingValue
	{
		Texture* TextureValue = nullptr;
		Buffer* BufferValue = nullptr;
		VertexBuffer* VertexBufferValue = nullptr;
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

	void BindSRV(const RHIBindingDesc& binding) override;
	void BindUAV(const RHIBindingDesc& binding) override;
	void BindCBV(const RHIBindingDesc& binding) override;
	void BindSampler(const RHIBindingDesc& binding) override;
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
	void SetVertexBufferUAV(const std::string& name, VertexBuffer* vertexBuffer) override;
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
	RenderBackendCapabilities GetCapabilities() const override
	{
		RenderBackendCapabilities capabilities{};
		capabilities.SupportsTypedBindingSchema = true;
#if CORONA_HAS_VULKAN
		capabilities.SupportsBindlessTextures = bBindlessTextureTableReady;
		capabilities.SupportsBindlessBuffers = bBindlessBufferTableReady;
		capabilities.SupportsRuntimeDescriptorArrays = bDescriptorIndexingEnabled;
		capabilities.SupportsPartiallyBoundDescriptors = bDescriptorIndexingEnabled;
		capabilities.SupportsUpdateAfterBind = bDescriptorIndexingEnabled;
		capabilities.SupportsDrawIndexedIndirect = bDrawIndexedIndirectEnabled;
		capabilities.SupportsDrawIndirect = bDrawIndexedIndirectEnabled;
		capabilities.SupportsMultiDrawIndirect = bMultiDrawIndirectEnabled;
		capabilities.SupportsDrawIndirectFirstInstance = bDrawIndirectFirstInstanceEnabled;
		capabilities.MaxBindlessTextureCount = MaxVulkanBindlessTextureSlots;
		capabilities.MaxBindlessBufferCount = MaxVulkanBindlessBufferSlots;
#endif
		return capabilities;
	}
	RenderBackendAllocatorStats GetAllocatorStats() const override;
	bool GetStreamlineTextureResource(Texture* texture, EResourceState state, StreamlineTextureResourceDesc& outDesc) const override;
	void* GetStreamlineCommandBuffer() override;
	bool GetStreamlineVulkanDeviceInfo(StreamlineVulkanDeviceInfo& outInfo) const override;
	uint32_t GetMaxSupportedHybridStage() const override { return SupportsRayTracing() ? 7u : 0u; }
	bool SupportsRayTracing() const override;
	bool SupportsShaderExecutionReordering() const override { return false; }

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
	RHITextureHandle RegisterBindlessTexture(Texture* texture) override;
	bool UpdateBindlessTexture(Texture* texture) override;
	void UnregisterBindlessTexture(Texture* texture) override;
	RHITextureHandle GetBindlessTextureHandle(const Texture* texture) const override;
	RHIBufferHandle RegisterBindlessBuffer(Buffer* buffer) override;
	RHIBufferHandle RegisterBindlessVertexBuffer(VertexBuffer* buffer) override;
	RHIBufferHandle RegisterBindlessIndexBuffer(IndexBuffer* buffer) override;
	void UnregisterBindlessBuffer(Buffer* buffer) override;
	void UnregisterBindlessVertexBuffer(VertexBuffer* buffer) override;
	void UnregisterBindlessIndexBuffer(IndexBuffer* buffer) override;
	RHIBufferHandle GetBindlessBufferHandle(const Buffer* buffer) const override;
	RHIBufferHandle GetBindlessVertexBufferHandle(const VertexBuffer* buffer) const override;
	RHIBufferHandle GetBindlessIndexBufferHandle(const IndexBuffer* buffer) const override;
	std::shared_ptr<Texture> CreateTexture3D(ETextureFormat format, ETextureUsageFlags usage, EInitialResourceState initialState, int width, int height, int depth, int mipLevels) override;
	void UploadTexture3D(Texture* texture, const void* data, uint64_t rowPitch, uint64_t slicePitch) override;
	std::shared_ptr<VertexBuffer> CreateVertexBuffer(uint32_t size, uint32_t stride, void* srcData) override;
	std::shared_ptr<IndexBuffer> CreateIndexBuffer(EIndexFormat format, uint32_t size, void* srcData) override;
	std::shared_ptr<VertexBuffer> CreateUploadVertexBuffer(uint32_t size, uint32_t stride, const void* srcData) override;
	std::shared_ptr<IndexBuffer> CreateUploadIndexBuffer(EIndexFormat format, uint32_t size, const void* srcData) override;
	void UpdateUploadVertexBuffer(VertexBuffer* buffer, const void* srcData, uint32_t sizeInBytes) override;
	void UpdateUploadIndexBuffer(IndexBuffer* buffer, const void* srcData, uint32_t sizeInBytes) override;
	std::shared_ptr<VertexBuffer> CreateRWVertexBuffer(uint32_t size, uint32_t stride) override;
	std::shared_ptr<Buffer> CreateUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize) override;
	void UpdateUploadStructuredBuffer(Buffer* buffer, const void* srcData, uint32_t sizeInBytes) override;
	std::shared_ptr<Buffer> AllocateTransientUploadStructuredBuffer(uint32_t numElements, uint32_t elementSize, const void* srcData) override;
	std::shared_ptr<RTAS> CreateBLASForMesh(Mesh* mesh) override;
	std::shared_ptr<RTAS> CreateBLASForSkeletalMesh(Mesh* mesh) override;
	void RefitBLAS(RTAS* rtas, Mesh* mesh) override;
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
	void DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation) override;
	void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) override;
	void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) override;
	bool DrawIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset, uint32_t drawCount) override;
	bool DrawIndexedIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset, uint32_t drawCount) override;
	void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
	void ClearTextureUAVFloat(Texture* texture, const float clearColor[4]) override;
	void ExecuteCurrentCommandList() override;
	void BeginGpuMarker(uint64_t color, const char* label) override;
	void EndGpuMarker() override;
	void TransitionTexture(Texture* texture, EResourceState stateBefore, EResourceState stateAfter) override;
	void TransitionBuffer(Buffer* buffer, EResourceState stateBefore, EResourceState stateAfter) override;
	void TransitionVertexBuffer(VertexBuffer* vertexBuffer, EResourceState stateBefore, EResourceState stateAfter) override;
	void UAVBarrier(Buffer* buffer) override;
	Texture* GetCurrentWindowRenderTarget() override;
	void PrepareWindowRenderTarget(Texture* renderTarget) override;
	void FinalizeWindowRenderTarget(Texture* renderTarget) override;
	void RequestWindowCapture(const std::wstring& outputPath) override;
	bool ConsumeWindowCaptureResult(std::wstring* outputPath, bool* success, std::wstring* errorMessage) override;
	std::shared_ptr<GraphicsPipelineHandle> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
	std::shared_ptr<GraphicsBindGroupHandle> CreateGraphicsBindGroup(const GraphicsBindGroupDesc& desc) override;
	void BindGraphicsPipeline(GraphicsPipelineHandle* pipeline) override;
	void BindGraphicsBindGroup(GraphicsPipelineHandle* pipeline, uint32_t slot, const std::shared_ptr<GraphicsBindGroupHandle>& bindGroup) override;
	void PreviewTextureOnWindow(Texture* texture);
	void DrawWindowTestTriangle();
	void DrawActiveRenderPassTestTriangle();

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
	void InitializePipelineCache();
	void SaveAndDestroyPipelineCache();
	bool InitializeBindlessDescriptorTables();
	void DestroyBindlessDescriptorTables();
	void BindGraphicsPipelineForDraw(VulkanGraphicsPipelineHandle* pipeline);
	void DestroyWindowContext();
	void DestroyFrameContexts();
	void TrackFrameDescriptorSet(VkDescriptorPool descriptorPool, VkDescriptorSet descriptorSet);
	VkDescriptorPool CreateGraphicsTransientDescriptorPool();
	VkDescriptorSet AllocateGraphicsDescriptorSet(VkDescriptorSetLayout descriptorSetLayout);
	bool WriteVulkanTLASInstanceDescs(VulkanRTAS* rtas, const std::vector<RTInstanceDesc>& instances);
	VkPipeline CreateTestTrianglePipeline(VkRenderPass compatibleRenderPass, uint32_t colorAttachmentCount);
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
	// Phase 3.5 (Vulkan): large HOST_VISIBLE block sub-allocated by
	// CreateUploadVertexBuffer / CreateUploadIndexBuffer. One persistent
	// mapping, bump-allocate cursor, combined VERTEX+INDEX buffer usage so
	// the same backing VkBuffer serves both APIs.
	struct VulkanUploadHeapBlock
	{
		VkDevice OwningDevice = VK_NULL_HANDLE;
		VkBuffer Buffer = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		uint8_t* MappedBase = nullptr;
		VkDeviceSize Capacity = 0;
		VkDeviceSize Cursor = 0;
		~VulkanUploadHeapBlock();
	};

	struct VulkanPersistentBufferBlock;

	struct VulkanBufferAllocation
	{
		VkBuffer Buffer = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		uint32_t Stride = 0;
		uint32_t SizeInBytes = 0;
		VkDeviceSize Offset = 0;
		// Phase 3.5 (Vulkan): non-null only when this allocation came from
		// the HOST_VISIBLE upload pool. Holding a shared_ptr keeps the
		// pool block alive until every sub-allocation that referenced it
		// has been released — Buffer/Memory above are non-owning views
		// into that block in this case.
		std::shared_ptr<VulkanUploadHeapBlock> PoolBlock;
		std::shared_ptr<VulkanPersistentBufferBlock> PersistentPoolBlock;
	};

	struct VulkanPersistentBufferBlock
	{
		VkDevice OwningDevice = VK_NULL_HANDLE;
		VkBuffer Buffer = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkDeviceSize Capacity = 0;
		VkDeviceSize Cursor = 0;
		struct FreeRange
		{
			VkDeviceSize Offset = 0;
			VkDeviceSize Size = 0;
		};
		std::vector<FreeRange> FreeRanges;
		~VulkanPersistentBufferBlock();
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
		bool bOwnsImage = true;
		bool bOwnsMemory = true;
		bool bOwnsImageView = true;
	};

	struct VulkanSamplerAllocation
	{
		VkSampler SamplerHandle = VK_NULL_HANDLE;
	};

	std::shared_ptr<Texture> CreateTrackedTextureHandle();
	std::shared_ptr<Buffer> CreateTrackedBufferHandle();
	std::shared_ptr<VertexBuffer> CreateTrackedVertexBufferHandle();
	std::shared_ptr<IndexBuffer> CreateTrackedIndexBufferHandle();
	std::shared_ptr<Sampler> CreateTrackedSamplerHandle();
	struct VulkanTrackedResourceOwner
	{
		VulkanBackend* Backend = nullptr;
	};
	std::weak_ptr<VulkanTrackedResourceOwner> GetTrackedResourceOwner();
	void ReleaseTextureAllocation(Texture* texture);
	void ReleaseBufferAllocation(Buffer* buffer);
	void ReleaseVertexBufferAllocation(VertexBuffer* vertexBuffer);
	void ReleaseIndexBufferAllocation(IndexBuffer* indexBuffer);
	void ReleaseSamplerAllocation(Sampler* sampler);

	struct VulkanFrameContext
	{
		VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
		VkSemaphore ImageAvailableSemaphore = VK_NULL_HANDLE;
		VkSemaphore RenderFinishedSemaphore = VK_NULL_HANDLE;
		VkFence InFlightFence = VK_NULL_HANDLE;
		struct DeferredBufferDestroy
		{
			VkBuffer Buffer = VK_NULL_HANDLE;
			VkDeviceMemory Memory = VK_NULL_HANDLE;
		};
		std::vector<std::pair<VkDescriptorPool, VkDescriptorSet>> DescriptorSetsToFree;
		std::vector<VkDescriptorPool> GraphicsDescriptorPools;
		uint32_t ActiveGraphicsDescriptorPoolIndex = 0;
		std::vector<VkFramebuffer> FramebuffersToDestroy;
		std::vector<DeferredBufferDestroy> BuffersToDestroy;
	};

	VkInstance Instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
	VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
	VkDevice Device = VK_NULL_HANDLE;
	VkSurfaceKHR Surface = VK_NULL_HANDLE;
	VkPipelineCache PipelineCache = VK_NULL_HANDLE;
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
	std::vector<std::shared_ptr<Texture>> SwapchainWrappedTextures;
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
	VkQueryPool OcclusionQueryPool = VK_NULL_HANDLE;
	VkRenderPass ActiveGraphicsRenderPass = VK_NULL_HANDLE;
	uint32_t ActiveColorAttachmentCount = 0;
	std::unordered_map<VkRenderPass, VkPipeline> TestTrianglePipelines;
	uint32_t TimestampQueryCount = 0;
	uint32_t OcclusionQueryCount = 0;
	uint32_t TimestampQueriesPerFrame = 0;
	uint32_t TimestampValidBits = 0;
	float TimestampPeriodNs = 0.0f;
	bool bTimestampQueriesResetForCurrentFrame = false;
	bool bOcclusionQueriesResetForCurrentFrame = false;
	VkBuffer TransientUniformBuffer = VK_NULL_HANDLE;
	VkDeviceMemory TransientUniformMemory = VK_NULL_HANDLE;
	void* TransientUniformMapped = nullptr;
	VkDeviceSize TransientUniformBytesPerFrame = 0;
	VkDeviceSize TransientUniformFrameOffset = 0;
	uint32_t TransientUniformFrameCount = 0;
	VkDeviceSize UniformBufferAlignment = 256;
	VkDeviceSize StorageBufferAlignment = 16;
	VkDeviceSize MaxUniformBufferRange = 0;

	// Phase 3.5 (Vulkan) — UPLOAD-heap-style sub-allocator that mirrors the
	// DX12 path. Returns a sub-range of a persistent HOST_VISIBLE block
	// suitable for raster VB/IB binding; the block stays alive via the
	// returned allocation's PoolBlock shared_ptr.
	bool AllocateUploadBufferRange(
		VkDeviceSize size,
		VkDeviceSize alignment,
		const void* srcData,
		VulkanBufferAllocation& outAllocation);

	struct VulkanTransientUploadStructuredFrame
	{
		std::vector<std::shared_ptr<VulkanUploadHeapBlock>> Blocks;
		std::vector<std::shared_ptr<Buffer>> KeepAlive;
	};
	std::vector<VulkanTransientUploadStructuredFrame> TransientUploadStructuredFrames;
	VkDeviceSize TransientUploadStructuredBlockDefaultSize = 4ull * 1024ull * 1024ull;
	uint32_t TransientUploadStructuredBlockCount = 0;
	uint32_t TransientUploadStructuredAllocationCount = 0;
	uint64_t TransientUploadStructuredBytesIssued = 0;
	uint64_t TransientUploadStructuredBytesReserved = 0;
	void ResetTransientUploadStructuredFrame(uint32_t frameIndex);
	bool AllocateTransientUploadStructuredRange(
		uint32_t frameIndex,
		VkDeviceSize size,
		VkDeviceSize alignment,
		const void* srcData,
		VulkanBufferAllocation& outAllocation);

	std::vector<std::shared_ptr<VulkanPersistentBufferBlock>> PersistentStructuredBufferBlocks;
	struct PendingPersistentStructuredBufferFree
	{
		std::shared_ptr<VulkanPersistentBufferBlock> Block;
		VkDeviceSize Offset = 0;
		VkDeviceSize Size = 0;
	};
	std::vector<std::vector<PendingPersistentStructuredBufferFree>> PendingPersistentStructuredBufferFrees;
	VkDeviceSize PersistentStructuredBufferBlockDefaultSize = 16ull * 1024ull * 1024ull;
	uint32_t PersistentStructuredBufferBlockCount = 0;
	uint32_t PersistentStructuredBufferAllocationCount = 0;
	uint64_t PersistentStructuredBufferBytesIssued = 0;
	uint64_t PersistentStructuredBufferBytesReserved = 0;
	struct PendingPersistentStructuredBufferUpload
	{
		VkFence Fence = VK_NULL_HANDLE;
		VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
		VkBuffer StagingBuffer = VK_NULL_HANDLE;
		VkDeviceMemory StagingMemory = VK_NULL_HANDLE;
		VkDeviceSize Bytes = 0;
	};
	std::vector<PendingPersistentStructuredBufferUpload> PendingPersistentStructuredBufferUploads;
	uint64_t PendingPersistentStructuredBufferUploadBytes = 0;
	static constexpr uint64_t kMaxInFlightPersistentStructuredBufferUploadBytes = 128ull * 1024ull * 1024ull;
	bool AllocatePersistentStructuredBufferRange(
		VkDeviceSize size,
		VkDeviceSize alignment,
		const void* srcData,
		VulkanBufferAllocation& outAllocation);
	void AddPersistentStructuredBufferFreeRange(
		const std::shared_ptr<VulkanPersistentBufferBlock>& block,
		VkDeviceSize offset,
		VkDeviceSize size);
	void ReleasePersistentStructuredBufferRange(const VulkanBufferAllocation& allocation);
	void RetirePersistentStructuredBufferFrees(uint32_t frameIndex);
	void RetirePersistentStructuredBufferUploads(bool waitForAll = false);
	std::shared_ptr<Buffer> CreateSuballocatedStructuredBuffer(const BufferCreateDesc& desc);

	// Phase 3.5 (Vulkan) upload pool state. ActiveUploadBlock is the
	// current bump-target; retired blocks stay alive via outstanding
	// VulkanBufferAllocation::PoolBlock shared_ptrs.
	std::shared_ptr<VulkanUploadHeapBlock> ActiveUploadBlock;
	VkDeviceSize UploadBlockDefaultSize = 16ull * 1024ull * 1024ull;
	uint32_t UploadBlockCount = 0;
	uint32_t UploadAllocationCount = 0;
	uint64_t UploadBytesIssued = 0;
	uint64_t UploadBytesReserved = 0;
	uint64_t TransientUniformOverflowCount = 0;
	std::unordered_map<Buffer*, VulkanBufferAllocation> BufferAllocations;
	std::unordered_map<VertexBuffer*, VulkanBufferAllocation> VertexBufferAllocations;
	std::unordered_map<IndexBuffer*, VulkanBufferAllocation> IndexBufferAllocations;
	std::unordered_map<Texture*, VulkanTextureAllocation> TextureAllocations;
	std::unordered_map<Sampler*, VulkanSamplerAllocation> SamplerAllocations;
	std::shared_ptr<VulkanTrackedResourceOwner> TrackedResourceOwner;
	std::vector<std::shared_ptr<VulkanGraphicsPipelineHandle>> GraphicsPipelines;
	std::vector<std::shared_ptr<VulkanRTPipelineStateObject>> RayTracingPipelines;
	std::vector<std::shared_ptr<VulkanComputePipelineStateObject>> ComputePipelines;
	std::vector<std::shared_ptr<VulkanRTAS>> RayTracingAccelerationStructures;
	bool bValidationLayersEnabled = false;
	bool bSamplerAnisotropySupported = false;
	bool bDrawIndexedIndirectEnabled = true;
	bool bMultiDrawIndirectEnabled = false;
	bool bDrawIndirectFirstInstanceEnabled = false;
	bool bRayTracingExtensionSupport = false;
	bool bRayTracingFeatureSupport = false;
	bool bRayTracingEnabled = false;
	bool bRayTracingIndirectEnabled = false;
	bool bDescriptorIndexingEnabled = false;
	bool bBindlessTextureTableReady = false;
	bool bBindlessBufferTableReady = false;
	uint32_t MaxVulkanBindlessTextureSlots = 0;
	uint32_t MaxVulkanBindlessBufferSlots = 0;
	VkDescriptorSetLayout EmptyDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout BindlessTextureDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool BindlessTextureDescriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet BindlessTextureDescriptorSet = VK_NULL_HANDLE;
	VkDescriptorSetLayout BindlessBufferDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool BindlessBufferDescriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet BindlessBufferDescriptorSet = VK_NULL_HANDLE;
	struct VulkanBindlessTextureSlot
	{
		Texture* TexturePtr = nullptr;
		uint32_t Generation = 0;
		bool Occupied = false;
	};
	struct VulkanBindlessBufferSlot
	{
		const void* BufferPtr = nullptr;
		uint8_t ResourceType = 0;
		uint32_t Generation = 0;
		bool Occupied = false;
	};
	std::vector<VulkanBindlessTextureSlot> BindlessTextureSlots;
	std::vector<uint32_t> BindlessTextureFreeList;
	mutable std::mutex BindlessTextureMutex;
	std::vector<VulkanBindlessBufferSlot> BindlessBufferSlots;
	std::vector<uint32_t> BindlessBufferFreeList;
	mutable std::mutex BindlessBufferMutex;
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
	PFN_vkCmdTraceRaysIndirectKHR vkCmdTraceRaysIndirectKHRFn = nullptr;
	PFN_vkGetBufferDeviceAddressKHR vkGetBufferDeviceAddressKHRFn = nullptr;
	PFN_vkCmdInsertDebugUtilsLabelEXT vkCmdInsertDebugUtilsLabelEXTFn = nullptr;
	PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginDebugUtilsLabelEXTFn = nullptr;
	PFN_vkCmdEndDebugUtilsLabelEXT vkCmdEndDebugUtilsLabelEXTFn = nullptr;
#endif
};
