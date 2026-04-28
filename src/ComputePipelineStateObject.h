#pragma once

#include <cstdint>
#include <memory>
#include <string>

class Texture;
class Buffer;
class Sampler;

class ComputePipelineStateObject
{
public:
	virtual ~ComputePipelineStateObject() = default;

	virtual void BindSRV(const std::string& name, uint32_t baseRegister, uint32_t numDescriptors) = 0;
	virtual void BindUAV(const std::string& name, uint32_t baseRegister) = 0;
	virtual void BindCBV(const std::string& name, uint32_t baseRegister, uint32_t size) = 0;
	virtual void BindSampler(const std::string& name, uint32_t baseRegister) = 0;

	virtual bool InitCS(const std::wstring& shaderFile, const std::string& entryPoint) = 0;
	virtual void Apply() = 0;

	virtual void SetTextureSRV(const std::string& name, Texture* texture) = 0;
	virtual void SetTextureUAV(const std::string& name, Texture* texture) = 0;
	virtual void SetBufferSRV(const std::string& name, Buffer* buffer) = 0;
	virtual void SetBufferUAV(const std::string& name, Buffer* buffer) = 0;
	virtual void SetSampler(const std::string& name, Sampler* sampler) = 0;
	virtual void SetCBVValue(const std::string& name, void* pData) = 0;
};
