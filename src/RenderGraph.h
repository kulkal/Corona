#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RenderBackend.h"

class Buffer;
class IRenderBackend;
class Texture;

static constexpr uint32_t RG_INVALID_INDEX = UINT32_MAX;

struct RGTextureRef
{
	uint32_t Index = RG_INVALID_INDEX;
	bool IsValid() const { return Index != RG_INVALID_INDEX; }
};

struct RGBufferRef
{
	uint32_t Index = RG_INVALID_INDEX;
	bool IsValid() const { return Index != RG_INVALID_INDEX; }
};

enum class ERGPassFlags : uint32_t
{
	None = 0,
	Graphics = 1 << 0,
	Compute = 1 << 1,
	RayTracing = 1 << 2,
	Copy = 1 << 3,
	AsyncCompute = 1 << 8,
	NeverCull = 1 << 9,
};

inline ERGPassFlags operator|(ERGPassFlags lhs, ERGPassFlags rhs)
{
	return static_cast<ERGPassFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

inline ERGPassFlags operator&(ERGPassFlags lhs, ERGPassFlags rhs)
{
	return static_cast<ERGPassFlags>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

inline bool HasRGPassFlag(ERGPassFlags flags, ERGPassFlags value)
{
	return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(value)) != 0;
}

enum class ERGAccessMode : uint8_t
{
	Read,
	Write,
	ReadWrite,
};

enum class ERGResourceLifetime : uint8_t
{
	Transient,
	Imported,
	External,
};

struct RGCompileStats
{
	uint32_t PassCount = 0;
	uint32_t ExecutedPassCount = 0;
	uint32_t CulledPassCount = 0;
	uint32_t TextureTransitionCount = 0;
	uint32_t BufferTransitionCount = 0;
	uint32_t UavBarrierCount = 0;
	double CompileMs = 0.0;
	double ExecuteMs = 0.0;
};

class RenderGraph;

class RGContext
{
public:
	RGContext(IRenderBackend* backend, RenderGraph* graph);

	IRenderBackend* GetBackend() const { return Backend; }
	Texture* GetTexture(RGTextureRef ref) const;
	Buffer* GetBuffer(RGBufferRef ref) const;

private:
	IRenderBackend* Backend = nullptr;
	RenderGraph* Graph = nullptr;
};

class RGPassBuilder
{
public:
	RGPassBuilder(RenderGraph* graph, uint32_t passIndex);

	RGPassBuilder& ReadTexture(RGTextureRef texture, EResourceState state = EResourceState::ShaderRead);
	RGPassBuilder& WriteTexture(RGTextureRef texture, EResourceState state);
	RGPassBuilder& ReadWriteTexture(RGTextureRef texture, EResourceState state = EResourceState::UnorderedAccess);

	RGPassBuilder& ReadBuffer(RGBufferRef buffer, EResourceState state = EResourceState::ShaderRead);
	RGPassBuilder& WriteBuffer(RGBufferRef buffer, EResourceState state);
	RGPassBuilder& ReadWriteBuffer(RGBufferRef buffer, EResourceState state = EResourceState::UnorderedAccess);

private:
	RenderGraph* Graph = nullptr;
	uint32_t PassIndex = RG_INVALID_INDEX;
};

class RenderGraph
{
public:
	using ExecuteFunc = std::function<void(RGContext&)>;
	using SetupFunc = std::function<void(RGPassBuilder&)>;

	explicit RenderGraph(IRenderBackend* backend);

	void Reset();

	RGTextureRef ImportTexture(const char* name, Texture* texture, EResourceState initialState);
	RGBufferRef ImportBuffer(const char* name, Buffer* buffer, EResourceState initialState);

	RGTextureRef CreateTexture(const char* name, const TextureCreateDesc& desc);
	RGBufferRef CreateBuffer(const char* name, const BufferCreateDesc& desc);

	void ExportTexture(RGTextureRef texture, EResourceState finalState);
	void ExportBuffer(RGBufferRef buffer, EResourceState finalState);

	void AddPass(const char* name, ERGPassFlags flags, const SetupFunc& setup, const ExecuteFunc& execute);

	bool Compile();
	bool Execute();

	Texture* GetTexture(RGTextureRef ref) const;
	Buffer* GetBuffer(RGBufferRef ref) const;

	const RGCompileStats& GetStats() const { return Stats; }
	const std::vector<std::string>& GetDiagnostics() const { return Diagnostics; }
	std::string Dump() const;

private:
	friend class RGPassBuilder;

	enum class ResourceKind : uint8_t
	{
		Texture,
		Buffer,
	};

	struct ResourceAccess
	{
		ResourceKind Kind = ResourceKind::Texture;
		uint32_t ResourceIndex = RG_INVALID_INDEX;
		EResourceState State = EResourceState::ShaderRead;
		ERGAccessMode Mode = ERGAccessMode::Read;
	};

	struct TextureResource
	{
		std::string Name;
		ERGResourceLifetime Lifetime = ERGResourceLifetime::Transient;
		TextureCreateDesc Desc{};
		Texture* Imported = nullptr;
		std::shared_ptr<Texture> Owned;
		EResourceState InitialState = EResourceState::ShaderRead;
		EResourceState FinalState = EResourceState::ShaderRead;
		bool bExported = false;
	};

	struct BufferResource
	{
		std::string Name;
		ERGResourceLifetime Lifetime = ERGResourceLifetime::Transient;
		BufferCreateDesc Desc{};
		Buffer* Imported = nullptr;
		std::shared_ptr<Buffer> Owned;
		EResourceState InitialState = EResourceState::ShaderRead;
		EResourceState FinalState = EResourceState::ShaderRead;
		bool bExported = false;
	};

	struct Pass
	{
		std::string Name;
		ERGPassFlags Flags = ERGPassFlags::None;
		std::vector<ResourceAccess> Accesses;
		ExecuteFunc Execute;
		bool bCulled = false;
	};

	struct PlannedTransition
	{
		uint32_t PassIndex = RG_INVALID_INDEX;
		ResourceKind Kind = ResourceKind::Texture;
		uint32_t ResourceIndex = RG_INVALID_INDEX;
		EResourceState Before = EResourceState::ShaderRead;
		EResourceState After = EResourceState::ShaderRead;
		bool bUavBarrier = false;
	};

	void AddTextureAccess(uint32_t passIndex, RGTextureRef texture, EResourceState state, ERGAccessMode mode);
	void AddBufferAccess(uint32_t passIndex, RGBufferRef buffer, EResourceState state, ERGAccessMode mode);

	bool ValidateGraph();
	void CullPasses();
	bool AllocateTransientResources();
	void BuildTransitionPlan();
	void EmitTransitionsForPass(uint32_t passIndex);
	void EmitFinalTransitions();

	static const char* ToString(EResourceState state);
	static const char* ToString(ERGAccessMode mode);
	static const char* ToString(ResourceKind kind);
	static EResourceState ToResourceState(EInitialResourceState state);
	static bool IsWriteAccess(ERGAccessMode mode);
	static bool IsReadAccess(ERGAccessMode mode);

	IRenderBackend* Backend = nullptr;
	std::vector<TextureResource> Textures;
	std::vector<BufferResource> Buffers;
	std::vector<Pass> Passes;
	std::vector<PlannedTransition> TransitionPlan;
	std::vector<std::string> Diagnostics;
	RGCompileStats Stats{};
	bool bCompiled = false;
};
