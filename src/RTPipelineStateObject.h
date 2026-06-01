#pragma once

#include <cstdint>
#include <memory>
#include <string>

class Texture;
class Buffer;
class Sampler;
class RTAS;
class VertexBuffer;
class IndexBuffer;

class RTPipelineStateObject
{
public:
	enum ShaderType
	{
		GLOBAL,
		RAYGEN,
		MISS,
		HIT,
		ANYHIT
	};

	virtual ~RTPipelineStateObject() = default;

	virtual void SetNumInstances(uint32_t numInstances) = 0;
	virtual void Configure(uint32_t maxRecursion, uint32_t maxPayloadSizeInBytes, uint32_t maxAttributeSizeInBytes) = 0;
	virtual void AddHitGroup(const std::string& name, const std::string& chs, const std::string& ahs) = 0;
	virtual void AddShader(const std::string& shader, ShaderType shaderType) = 0;
	virtual void BindUAV(const std::string& shader, const std::string& name, uint32_t baseRegister) = 0;
	virtual void BindSRV(const std::string& shader, const std::string& name, uint32_t baseRegister) = 0;
	virtual void BindSampler(const std::string& shader, const std::string& name, uint32_t baseRegister) = 0;
	virtual void BindCBV(const std::string& shader, const std::string& name, uint32_t baseRegister, uint32_t size, uint32_t numInstance) = 0;
	virtual void SetShaderDefine(const std::string& name, const std::string& value) = 0;
	virtual void SetShaderLibraryTarget(const std::string& target) = 0;
	virtual bool IsHitProgramBindingCacheValid(uint32_t numInstances, uint64_t signature) const { return false; }
	virtual void MarkHitProgramBindingCacheDirty() {}
	virtual void MarkHitProgramBindingCacheValid(uint32_t numInstances, uint64_t signature) {}
	virtual void BeginShaderTable() = 0;
	virtual void EndShaderTable() = 0;
	virtual void SetTextureUAV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex = -1) = 0;
	virtual void SetBufferUAV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int instanceIndex = -1) = 0;
	virtual void SetTextureSRV(const std::string& shader, const std::string& bindingName, Texture* texture, int instanceIndex = -1) = 0;
	virtual void SetBufferSRV(const std::string& shader, const std::string& bindingName, Buffer* buffer, int instanceIndex = -1) = 0;
	virtual void SetAccelerationStructure(const std::string& shader, const std::string& bindingName, const std::shared_ptr<RTAS>& rtas, int instanceIndex = -1) = 0;
	virtual void SetSampler(const std::string& shader, const std::string& bindingName, Sampler* sampler, int instanceIndex = -1) = 0;
	virtual void SetCBVValue(const std::string& shader, const std::string& bindingName, void* pData, int instanceIndex = -1) = 0;
	virtual void ResetHitProgram(uint32_t instanceIndex) = 0;
	virtual void StartHitProgram(const std::string& hitGroup, uint32_t instanceIndex) = 0;
	virtual void AddTextureSRVToHitProgram(const std::string& hitGroup, Texture* texture, uint32_t instanceIndex) = 0;
	virtual void AddBufferSRVToHitProgram(const std::string& hitGroup, Buffer* buffer, uint32_t instanceIndex) = 0;
	// Hit shaders use scene-global geometry; InstanceProperty carries the per-instance offsets.
	virtual void AddSceneGeometrySRVsToHitProgram(const std::string& hitGroup, VertexBuffer* sceneVertexBuffer, IndexBuffer* sceneIndexBuffer, uint32_t instanceIndex) = 0;
	virtual bool InitRS(const std::string& shaderFile) = 0;
	virtual void Apply(uint32_t width, uint32_t height) = 0;
};
