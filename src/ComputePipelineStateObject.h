#pragma once

#include <cstdint>
#include <memory>
#include <string>

class Texture;
class Buffer;
class VertexBuffer;
class Sampler;
class RTAS;

class ComputePipelineStateObject
{
public:
	virtual ~ComputePipelineStateObject() = default;

	virtual void BindSRV(const std::string& name, uint32_t baseRegister, uint32_t numDescriptors) = 0;
	virtual void BindUAV(const std::string& name, uint32_t baseRegister) = 0;
	virtual void BindCBV(const std::string& name, uint32_t baseRegister, uint32_t size) = 0;
	virtual void BindSampler(const std::string& name, uint32_t baseRegister) = 0;

	virtual bool InitCS(const std::wstring& shaderFile, const std::string& entryPoint) = 0;
	// Inline RT variant — compiles with cs_6_5 so RayQuery is available
	// in the shader. Use for compute passes that need to trace fresh
	// visibility rays (e.g. ReSTIR Phase 3 spatial reuse) without
	// authoring a full RT pipeline. Default implementation falls back
	// to InitCS so non-DX12 backends remain buildable.
	virtual bool InitCSWithInlineRT(const std::wstring& shaderFile, const std::string& entryPoint)
	{
		return InitCS(shaderFile, entryPoint);
	}
	virtual void Apply() = 0;

	virtual void SetTextureSRV(const std::string& name, Texture* texture) = 0;
	virtual void SetTextureUAV(const std::string& name, Texture* texture) = 0;
	virtual void SetBufferSRV(const std::string& name, Buffer* buffer) = 0;
	virtual void SetBufferUAV(const std::string& name, Buffer* buffer) = 0;
	virtual void SetVertexBufferUAV(const std::string& name, VertexBuffer* vertexBuffer) = 0;
	virtual void SetSampler(const std::string& name, Sampler* sampler) = 0;
	virtual void SetCBVValue(const std::string& name, void* pData) = 0;
	// TLAS handle bound as an SRV at the requested t-register. HLSL
	// declares `RaytracingAccelerationStructure as : register(tN)` and
	// uses RayQuery to trace from compute. Default no-op for backends
	// that don't yet implement inline RT.
	virtual void SetAccelerationStructure(const std::string& /*name*/, const std::shared_ptr<RTAS>& /*rtas*/) {}
};
