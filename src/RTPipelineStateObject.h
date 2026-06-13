#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "RHIBinding.h"

class Texture;
class Buffer;
class Sampler;
class RTAS;

struct RtDispatchRaysIndirectTemplate
{
	uint64_t RayGenerationStartAddress = 0;
	uint64_t RayGenerationSizeInBytes = 0;
	uint64_t MissStartAddress = 0;
	uint64_t MissSizeInBytes = 0;
	uint64_t MissStrideInBytes = 0;
	uint64_t HitGroupStartAddress = 0;
	uint64_t HitGroupSizeInBytes = 0;
	uint64_t HitGroupStrideInBytes = 0;
	uint64_t CallableStartAddress = 0;
	uint64_t CallableSizeInBytes = 0;
	uint64_t CallableStrideInBytes = 0;
	uint32_t Width = 0;
	uint32_t Height = 0;
	uint32_t Depth = 1;
};

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
	virtual void BindUAV(const std::string& shader, const RHIBindingDesc& binding)
	{
		BindUAV(shader, binding.Name, binding.RegisterIndex);
	}
	virtual void BindSRV(const std::string& shader, const RHIBindingDesc& binding)
	{
		BindSRV(shader, binding.Name, binding.RegisterIndex);
	}
	virtual void BindSampler(const std::string& shader, const RHIBindingDesc& binding)
	{
		BindSampler(shader, binding.Name, binding.RegisterIndex);
	}
	virtual void BindCBV(const std::string& shader, const RHIBindingDesc& binding)
	{
		BindCBV(shader, binding.Name, binding.RegisterIndex, binding.SizeInBytes, binding.NumInstances);
	}
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
	virtual bool SetBindlessTextureTable(const std::string& shader, const std::string& bindingName)
	{
		(void)shader;
		(void)bindingName;
		return false;
	}
	virtual bool SetBindlessBufferTable(const std::string& shader, const std::string& bindingName)
	{
		(void)shader;
		(void)bindingName;
		return false;
	}
	virtual void SetAccelerationStructure(const std::string& shader, const std::string& bindingName, const std::shared_ptr<RTAS>& rtas, int instanceIndex = -1) = 0;
	virtual void SetSampler(const std::string& shader, const std::string& bindingName, Sampler* sampler, int instanceIndex = -1) = 0;
	virtual void SetCBVValue(const std::string& shader, const std::string& bindingName, void* pData, int instanceIndex = -1) = 0;
	virtual void ResetHitProgram(uint32_t instanceIndex) = 0;
	virtual void StartHitProgram(const std::string& hitGroup, uint32_t instanceIndex) = 0;
	virtual bool InitRS(const std::string& shaderFile) = 0;
	virtual void Apply(uint32_t width, uint32_t height) = 0;
	virtual bool GetDispatchRaysIndirectTemplate(uint32_t width, uint32_t height, RtDispatchRaysIndirectTemplate& outTemplate) const = 0;
	virtual bool ApplyIndirect(Buffer* indirectArgumentBuffer, uint64_t byteOffset) = 0;
};
