#include "stdafx.h"
#include "RenderGraph.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_set>

namespace
{
	double ElapsedMs(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - begin).count();
	}

	uint64_t MakeResourceKey(uint8_t kind, uint32_t index)
	{
		return (static_cast<uint64_t>(kind) << 32) | index;
	}

	bool IsUnorderedAccess(EResourceState state)
	{
		return state == EResourceState::UnorderedAccess;
	}
}

RGContext::RGContext(IRenderBackend* backend, RenderGraph* graph)
	: Backend(backend)
	, Graph(graph)
{
}

Texture* RGContext::GetTexture(RGTextureRef ref) const
{
	return Graph ? Graph->GetTexture(ref) : nullptr;
}

Buffer* RGContext::GetBuffer(RGBufferRef ref) const
{
	return Graph ? Graph->GetBuffer(ref) : nullptr;
}

RGPassBuilder::RGPassBuilder(RenderGraph* graph, uint32_t passIndex)
	: Graph(graph)
	, PassIndex(passIndex)
{
}

RGPassBuilder& RGPassBuilder::ReadTexture(RGTextureRef texture, EResourceState state)
{
	if (Graph)
		Graph->AddTextureAccess(PassIndex, texture, state, ERGAccessMode::Read);
	return *this;
}

RGPassBuilder& RGPassBuilder::WriteTexture(RGTextureRef texture, EResourceState state)
{
	if (Graph)
		Graph->AddTextureAccess(PassIndex, texture, state, ERGAccessMode::Write);
	return *this;
}

RGPassBuilder& RGPassBuilder::ReadWriteTexture(RGTextureRef texture, EResourceState state)
{
	if (Graph)
		Graph->AddTextureAccess(PassIndex, texture, state, ERGAccessMode::ReadWrite);
	return *this;
}

RGPassBuilder& RGPassBuilder::ReadBuffer(RGBufferRef buffer, EResourceState state)
{
	if (Graph)
		Graph->AddBufferAccess(PassIndex, buffer, state, ERGAccessMode::Read);
	return *this;
}

RGPassBuilder& RGPassBuilder::WriteBuffer(RGBufferRef buffer, EResourceState state)
{
	if (Graph)
		Graph->AddBufferAccess(PassIndex, buffer, state, ERGAccessMode::Write);
	return *this;
}

RGPassBuilder& RGPassBuilder::ReadWriteBuffer(RGBufferRef buffer, EResourceState state)
{
	if (Graph)
		Graph->AddBufferAccess(PassIndex, buffer, state, ERGAccessMode::ReadWrite);
	return *this;
}

RenderGraph::RenderGraph(IRenderBackend* backend)
	: Backend(backend)
{
	Textures.reserve(16);
	Buffers.reserve(8);
	Passes.reserve(4);
	TransitionPlan.reserve(32);
	Diagnostics.reserve(4);
}

void RenderGraph::Reset()
{
	Textures.clear();
	Buffers.clear();
	Passes.clear();
	TransitionPlan.clear();
	Diagnostics.clear();
	Stats = {};
	bCompiled = false;
}

RGTextureRef RenderGraph::ImportTexture(const char* name, Texture* texture, EResourceState initialState)
{
	TextureResource resource{};
	resource.Name = name ? name : "<unnamed texture>";
	resource.Lifetime = ERGResourceLifetime::Imported;
	resource.Imported = texture;
	resource.InitialState = initialState;
	resource.FinalState = initialState;
	Textures.push_back(resource);
	bCompiled = false;
	return { static_cast<uint32_t>(Textures.size() - 1) };
}

RGBufferRef RenderGraph::ImportBuffer(const char* name, Buffer* buffer, EResourceState initialState)
{
	BufferResource resource{};
	resource.Name = name ? name : "<unnamed buffer>";
	resource.Lifetime = ERGResourceLifetime::Imported;
	resource.Imported = buffer;
	resource.InitialState = initialState;
	resource.FinalState = initialState;
	Buffers.push_back(resource);
	bCompiled = false;
	return { static_cast<uint32_t>(Buffers.size() - 1) };
}

RGTextureRef RenderGraph::CreateTexture(const char* name, const TextureCreateDesc& desc)
{
	TextureResource resource{};
	resource.Name = name ? name : "<unnamed texture>";
	resource.Lifetime = ERGResourceLifetime::Transient;
	resource.Desc = desc;
	resource.InitialState = ToResourceState(desc.InitialState);
	resource.FinalState = resource.InitialState;
	Textures.push_back(resource);
	bCompiled = false;
	return { static_cast<uint32_t>(Textures.size() - 1) };
}

RGBufferRef RenderGraph::CreateBuffer(const char* name, const BufferCreateDesc& desc)
{
	BufferResource resource{};
	resource.Name = name ? name : "<unnamed buffer>";
	resource.Lifetime = ERGResourceLifetime::Transient;
	resource.Desc = desc;
	resource.InitialState = ToResourceState(desc.InitialState);
	resource.FinalState = resource.InitialState;
	Buffers.push_back(resource);
	bCompiled = false;
	return { static_cast<uint32_t>(Buffers.size() - 1) };
}

void RenderGraph::ExportTexture(RGTextureRef texture, EResourceState finalState)
{
	if (!texture.IsValid() || texture.Index >= Textures.size())
	{
		Diagnostics.push_back("RenderGraph::ExportTexture received an invalid texture handle.");
		return;
	}

	Textures[texture.Index].bExported = true;
	Textures[texture.Index].FinalState = finalState;
	bCompiled = false;
}

void RenderGraph::ExportBuffer(RGBufferRef buffer, EResourceState finalState)
{
	if (!buffer.IsValid() || buffer.Index >= Buffers.size())
	{
		Diagnostics.push_back("RenderGraph::ExportBuffer received an invalid buffer handle.");
		return;
	}

	Buffers[buffer.Index].bExported = true;
	Buffers[buffer.Index].FinalState = finalState;
	bCompiled = false;
}

void RenderGraph::AddPass(const char* name, ERGPassFlags flags, const SetupFunc& setup, const ExecuteFunc& execute)
{
	Pass pass{};
	pass.Name = name ? name : "<unnamed pass>";
	pass.Flags = flags;
	pass.Execute = execute;
	pass.Accesses.reserve(16);
	Passes.push_back(std::move(pass));

	const uint32_t passIndex = static_cast<uint32_t>(Passes.size() - 1);
	if (setup)
	{
		RGPassBuilder builder(this, passIndex);
		setup(builder);
	}

	bCompiled = false;
}

bool RenderGraph::Compile()
{
	const auto compileBegin = std::chrono::steady_clock::now();
	TransitionPlan.clear();
	Diagnostics.clear();
	Stats = {};
	Stats.PassCount = static_cast<uint32_t>(Passes.size());

	for (Pass& pass : Passes)
		pass.bCulled = false;

	if (!ValidateGraph())
	{
		Stats.CompileMs = ElapsedMs(compileBegin, std::chrono::steady_clock::now());
		return false;
	}

	CullPasses();
	if (!AllocateTransientResources())
	{
		Stats.CompileMs = ElapsedMs(compileBegin, std::chrono::steady_clock::now());
		return false;
	}

	BuildTransitionPlan();

	for (const Pass& pass : Passes)
	{
		if (pass.bCulled)
			++Stats.CulledPassCount;
		else
			++Stats.ExecutedPassCount;
	}

	for (const PlannedTransition& transition : TransitionPlan)
	{
		if (transition.bUavBarrier)
			++Stats.UavBarrierCount;
		else if (transition.Kind == ResourceKind::Texture)
			++Stats.TextureTransitionCount;
		else
			++Stats.BufferTransitionCount;
	}

	Stats.CompileMs = ElapsedMs(compileBegin, std::chrono::steady_clock::now());
	bCompiled = true;
	return true;
}

bool RenderGraph::Execute()
{
	if (!bCompiled && !Compile())
		return false;
	if (!Backend)
		return false;

	const auto executeBegin = std::chrono::steady_clock::now();
	RGContext context(Backend, this);
	for (uint32_t passIndex = 0; passIndex < Passes.size(); ++passIndex)
	{
		Pass& pass = Passes[passIndex];
		if (pass.bCulled)
			continue;

		EmitTransitionsForPass(passIndex);
		Backend->EmitGpuCrashMarker(pass.Name.c_str());
		if (pass.Execute)
			pass.Execute(context);
	}

	EmitFinalTransitions();
	Stats.ExecuteMs = ElapsedMs(executeBegin, std::chrono::steady_clock::now());
	return true;
}

Texture* RenderGraph::GetTexture(RGTextureRef ref) const
{
	if (!ref.IsValid() || ref.Index >= Textures.size())
		return nullptr;

	const TextureResource& resource = Textures[ref.Index];
	return resource.Imported ? resource.Imported : resource.Owned.get();
}

Buffer* RenderGraph::GetBuffer(RGBufferRef ref) const
{
	if (!ref.IsValid() || ref.Index >= Buffers.size())
		return nullptr;

	const BufferResource& resource = Buffers[ref.Index];
	return resource.Imported ? resource.Imported : resource.Owned.get();
}

std::string RenderGraph::Dump() const
{
	std::ostringstream out;
	out << "RenderGraph: passes=" << Passes.size()
		<< ", executed=" << Stats.ExecutedPassCount
		<< ", culled=" << Stats.CulledPassCount
		<< ", textureTransitions=" << Stats.TextureTransitionCount
		<< ", bufferTransitions=" << Stats.BufferTransitionCount
		<< ", uavBarriers=" << Stats.UavBarrierCount << "\n";

	for (uint32_t passIndex = 0; passIndex < Passes.size(); ++passIndex)
	{
		const Pass& pass = Passes[passIndex];
		out << "[" << passIndex << "] " << pass.Name;
		if (pass.bCulled)
			out << " (culled)";
		out << "\n";

		for (const ResourceAccess& access : pass.Accesses)
		{
			const char* resourceName = "<invalid>";
			if (access.Kind == ResourceKind::Texture && access.ResourceIndex < Textures.size())
				resourceName = Textures[access.ResourceIndex].Name.c_str();
			else if (access.Kind == ResourceKind::Buffer && access.ResourceIndex < Buffers.size())
				resourceName = Buffers[access.ResourceIndex].Name.c_str();

			out << "  " << ToString(access.Mode)
				<< " " << ToString(access.Kind)
				<< " " << resourceName
				<< " as " << ToString(access.State) << "\n";
		}
	}

	for (const PlannedTransition& transition : TransitionPlan)
	{
		const bool bFinalTransition = transition.PassIndex == RG_INVALID_INDEX;
		const char* resourceName = "<invalid>";
		if (transition.Kind == ResourceKind::Texture && transition.ResourceIndex < Textures.size())
			resourceName = Textures[transition.ResourceIndex].Name.c_str();
		else if (transition.Kind == ResourceKind::Buffer && transition.ResourceIndex < Buffers.size())
			resourceName = Buffers[transition.ResourceIndex].Name.c_str();

		out << "  transition before ";
		if (bFinalTransition)
			out << "<final>";
		else if (transition.PassIndex < Passes.size())
			out << Passes[transition.PassIndex].Name;
		else
			out << "<invalid>";

		out << ": " << resourceName;
		if (transition.bUavBarrier)
			out << " UAV barrier";
		else
			out << " " << ToString(transition.Before) << " -> " << ToString(transition.After);
		out << "\n";
	}

	return out.str();
}

void RenderGraph::AddTextureAccess(uint32_t passIndex, RGTextureRef texture, EResourceState state, ERGAccessMode mode)
{
	if (passIndex >= Passes.size() || !texture.IsValid() || texture.Index >= Textures.size())
	{
		Diagnostics.push_back("RenderGraph texture access declaration used an invalid handle.");
		return;
	}

	Passes[passIndex].Accesses.push_back({ ResourceKind::Texture, texture.Index, state, mode });
	bCompiled = false;
}

void RenderGraph::AddBufferAccess(uint32_t passIndex, RGBufferRef buffer, EResourceState state, ERGAccessMode mode)
{
	if (passIndex >= Passes.size() || !buffer.IsValid() || buffer.Index >= Buffers.size())
	{
		Diagnostics.push_back("RenderGraph buffer access declaration used an invalid handle.");
		return;
	}

	Passes[passIndex].Accesses.push_back({ ResourceKind::Buffer, buffer.Index, state, mode });
	bCompiled = false;
}

bool RenderGraph::ValidateGraph()
{
	if (!Backend)
		Diagnostics.push_back("RenderGraph has no IRenderBackend.");

	for (uint32_t index = 0; index < Textures.size(); ++index)
	{
		const TextureResource& resource = Textures[index];
		if (resource.Lifetime != ERGResourceLifetime::Transient && !resource.Imported)
			Diagnostics.push_back("Imported texture '" + resource.Name + "' has no Texture pointer.");
	}

	for (uint32_t index = 0; index < Buffers.size(); ++index)
	{
		const BufferResource& resource = Buffers[index];
		if (resource.Lifetime != ERGResourceLifetime::Transient && !resource.Imported)
			Diagnostics.push_back("Imported buffer '" + resource.Name + "' has no Buffer pointer.");
	}

	std::vector<bool> transientTextureWritten(Textures.size(), false);
	std::vector<bool> transientBufferWritten(Buffers.size(), false);

	for (uint32_t passIndex = 0; passIndex < Passes.size(); ++passIndex)
	{
		const Pass& pass = Passes[passIndex];
		if (!pass.Execute)
			Diagnostics.push_back("Pass '" + pass.Name + "' has no execute function.");

		std::vector<uint64_t> resourcesInPass;
		resourcesInPass.reserve(pass.Accesses.size());
		for (const ResourceAccess& access : pass.Accesses)
		{
			if (access.Kind == ResourceKind::Texture)
			{
				if (access.ResourceIndex >= Textures.size())
				{
					Diagnostics.push_back("Pass '" + pass.Name + "' references an invalid texture.");
					continue;
				}

				const TextureResource& resource = Textures[access.ResourceIndex];
				if (resource.Lifetime == ERGResourceLifetime::Transient &&
					IsReadAccess(access.Mode) &&
					!transientTextureWritten[access.ResourceIndex])
				{
					Diagnostics.push_back("Pass '" + pass.Name + "' reads transient texture '" + resource.Name + "' before any write.");
				}
				if (IsWriteAccess(access.Mode))
					transientTextureWritten[access.ResourceIndex] = true;
			}
			else
			{
				if (access.ResourceIndex >= Buffers.size())
				{
					Diagnostics.push_back("Pass '" + pass.Name + "' references an invalid buffer.");
					continue;
				}

				const BufferResource& resource = Buffers[access.ResourceIndex];
				if (resource.Lifetime == ERGResourceLifetime::Transient &&
					IsReadAccess(access.Mode) &&
					!transientBufferWritten[access.ResourceIndex])
				{
					Diagnostics.push_back("Pass '" + pass.Name + "' reads transient buffer '" + resource.Name + "' before any write.");
				}
				if (IsWriteAccess(access.Mode))
					transientBufferWritten[access.ResourceIndex] = true;
			}

			const uint64_t key = MakeResourceKey(static_cast<uint8_t>(access.Kind), access.ResourceIndex);
			if (std::find(resourcesInPass.begin(), resourcesInPass.end(), key) != resourcesInPass.end())
				Diagnostics.push_back("Pass '" + pass.Name + "' declares resource access more than once; split or use ReadWrite access.");
			else
				resourcesInPass.push_back(key);
		}
	}

	return Diagnostics.empty();
}

void RenderGraph::CullPasses()
{
	std::vector<bool> liveTextures(Textures.size(), false);
	std::vector<bool> liveBuffers(Buffers.size(), false);

	for (uint32_t index = 0; index < Textures.size(); ++index)
	{
		const TextureResource& resource = Textures[index];
		liveTextures[index] =
			resource.bExported ||
			resource.Lifetime == ERGResourceLifetime::Imported ||
			resource.Lifetime == ERGResourceLifetime::External;
	}

	for (uint32_t index = 0; index < Buffers.size(); ++index)
	{
		const BufferResource& resource = Buffers[index];
		liveBuffers[index] =
			resource.bExported ||
			resource.Lifetime == ERGResourceLifetime::Imported ||
			resource.Lifetime == ERGResourceLifetime::External;
	}

	for (int32_t passIndex = static_cast<int32_t>(Passes.size()) - 1; passIndex >= 0; --passIndex)
	{
		Pass& pass = Passes[passIndex];
		bool bKeep = HasRGPassFlag(pass.Flags, ERGPassFlags::NeverCull);

		for (const ResourceAccess& access : pass.Accesses)
		{
			if (!IsWriteAccess(access.Mode))
				continue;

			if (access.Kind == ResourceKind::Texture && access.ResourceIndex < liveTextures.size() && liveTextures[access.ResourceIndex])
				bKeep = true;
			if (access.Kind == ResourceKind::Buffer && access.ResourceIndex < liveBuffers.size() && liveBuffers[access.ResourceIndex])
				bKeep = true;
		}

		pass.bCulled = !bKeep;
		if (!bKeep)
			continue;

		for (const ResourceAccess& access : pass.Accesses)
		{
			if (!IsReadAccess(access.Mode))
				continue;

			if (access.Kind == ResourceKind::Texture && access.ResourceIndex < liveTextures.size())
				liveTextures[access.ResourceIndex] = true;
			if (access.Kind == ResourceKind::Buffer && access.ResourceIndex < liveBuffers.size())
				liveBuffers[access.ResourceIndex] = true;
		}
	}
}

bool RenderGraph::AllocateTransientResources()
{
	if (!Backend)
		return false;

	for (TextureResource& resource : Textures)
	{
		if (resource.Lifetime != ERGResourceLifetime::Transient || resource.Owned)
			continue;

		resource.Owned = Backend->CreateTexture2D(resource.Desc);
		if (!resource.Owned)
		{
			Diagnostics.push_back("Failed to allocate transient texture '" + resource.Name + "'.");
			return false;
		}
	}

	for (BufferResource& resource : Buffers)
	{
		if (resource.Lifetime != ERGResourceLifetime::Transient || resource.Owned)
			continue;

		resource.Owned = Backend->CreateBuffer(resource.Desc);
		if (!resource.Owned)
		{
			Diagnostics.push_back("Failed to allocate transient buffer '" + resource.Name + "'.");
			return false;
		}
	}

	return true;
}

void RenderGraph::BuildTransitionPlan()
{
	struct StateTracker
	{
		EResourceState State = EResourceState::ShaderRead;
		bool bLastWriteWasUav = false;
	};

	std::vector<StateTracker> textureStates(Textures.size());
	std::vector<StateTracker> bufferStates(Buffers.size());
	for (uint32_t index = 0; index < Textures.size(); ++index)
		textureStates[index].State = Textures[index].InitialState;
	for (uint32_t index = 0; index < Buffers.size(); ++index)
		bufferStates[index].State = Buffers[index].InitialState;

	for (uint32_t passIndex = 0; passIndex < Passes.size(); ++passIndex)
	{
		const Pass& pass = Passes[passIndex];
		if (pass.bCulled)
			continue;

		for (const ResourceAccess& access : pass.Accesses)
		{
			StateTracker* tracker = nullptr;
			if (access.Kind == ResourceKind::Texture && access.ResourceIndex < textureStates.size())
				tracker = &textureStates[access.ResourceIndex];
			else if (access.Kind == ResourceKind::Buffer && access.ResourceIndex < bufferStates.size())
				tracker = &bufferStates[access.ResourceIndex];

			if (!tracker)
				continue;

			if (tracker->State != access.State)
			{
				TransitionPlan.push_back({
					passIndex,
					access.Kind,
					access.ResourceIndex,
					tracker->State,
					access.State,
					false,
				});
				tracker->State = access.State;
				tracker->bLastWriteWasUav = false;
			}
			else if (IsUnorderedAccess(access.State) && tracker->bLastWriteWasUav)
			{
				TransitionPlan.push_back({
					passIndex,
					access.Kind,
					access.ResourceIndex,
					tracker->State,
					access.State,
					true,
				});
			}

			tracker->bLastWriteWasUav = IsUnorderedAccess(access.State) && IsWriteAccess(access.Mode);
		}
	}

	for (uint32_t index = 0; index < Textures.size(); ++index)
	{
		const TextureResource& resource = Textures[index];
		if (!resource.bExported)
			continue;

		if (textureStates[index].State != resource.FinalState)
		{
			TransitionPlan.push_back({
				RG_INVALID_INDEX,
				ResourceKind::Texture,
				index,
				textureStates[index].State,
				resource.FinalState,
				false,
			});
			textureStates[index].State = resource.FinalState;
		}
	}

	for (uint32_t index = 0; index < Buffers.size(); ++index)
	{
		const BufferResource& resource = Buffers[index];
		if (!resource.bExported)
			continue;

		if (bufferStates[index].State != resource.FinalState)
		{
			TransitionPlan.push_back({
				RG_INVALID_INDEX,
				ResourceKind::Buffer,
				index,
				bufferStates[index].State,
				resource.FinalState,
				false,
			});
			bufferStates[index].State = resource.FinalState;
		}
	}
}

void RenderGraph::EmitTransitionsForPass(uint32_t passIndex)
{
	for (const PlannedTransition& transition : TransitionPlan)
	{
		if (transition.PassIndex != passIndex)
			continue;

		if (transition.Kind == ResourceKind::Texture)
		{
			Texture* texture = GetTexture({ transition.ResourceIndex });
			if (transition.bUavBarrier)
				Backend->UAVBarrier(texture);
			else
				Backend->TransitionTexture(texture, transition.Before, transition.After);
		}
		else
		{
			Buffer* buffer = GetBuffer({ transition.ResourceIndex });
			if (transition.bUavBarrier)
				Backend->UAVBarrier(buffer);
			else
				Backend->TransitionBuffer(buffer, transition.Before, transition.After);
		}
	}
}

void RenderGraph::EmitFinalTransitions()
{
	for (const PlannedTransition& transition : TransitionPlan)
	{
		if (transition.PassIndex != RG_INVALID_INDEX)
			continue;

		if (transition.Kind == ResourceKind::Texture)
			Backend->TransitionTexture(GetTexture({ transition.ResourceIndex }), transition.Before, transition.After);
		else
			Backend->TransitionBuffer(GetBuffer({ transition.ResourceIndex }), transition.Before, transition.After);
	}
}

const char* RenderGraph::ToString(EResourceState state)
{
	switch (state)
	{
	case EResourceState::ShaderRead: return "ShaderRead";
	case EResourceState::RenderTarget: return "RenderTarget";
	case EResourceState::UnorderedAccess: return "UnorderedAccess";
	case EResourceState::Present: return "Present";
	case EResourceState::DepthWrite: return "DepthWrite";
	case EResourceState::CopyDest: return "CopyDest";
	case EResourceState::CopySource: return "CopySource";
	case EResourceState::VertexBuffer: return "VertexBuffer";
	case EResourceState::IndirectArgument: return "IndirectArgument";
	default: return "<unknown>";
	}
}

const char* RenderGraph::ToString(ERGAccessMode mode)
{
	switch (mode)
	{
	case ERGAccessMode::Read: return "read";
	case ERGAccessMode::Write: return "write";
	case ERGAccessMode::ReadWrite: return "readwrite";
	default: return "<unknown>";
	}
}

const char* RenderGraph::ToString(ResourceKind kind)
{
	switch (kind)
	{
	case ResourceKind::Texture: return "texture";
	case ResourceKind::Buffer: return "buffer";
	default: return "<unknown>";
	}
}

EResourceState RenderGraph::ToResourceState(EInitialResourceState state)
{
	switch (state)
	{
	case EInitialResourceState::ShaderRead:
	case EInitialResourceState::GenericRead:
		return EResourceState::ShaderRead;
	case EInitialResourceState::CopyDest:
		return EResourceState::CopyDest;
	default:
		return EResourceState::ShaderRead;
	}
}

bool RenderGraph::IsWriteAccess(ERGAccessMode mode)
{
	return mode == ERGAccessMode::Write || mode == ERGAccessMode::ReadWrite;
}

bool RenderGraph::IsReadAccess(ERGAccessMode mode)
{
	return mode == ERGAccessMode::Read || mode == ERGAccessMode::ReadWrite;
}
