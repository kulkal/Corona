//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "stdafx.h"
#include "Corona.h"

#include <algorithm>
#include <limits>

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	static constexpr UINT32 PathTracingCompactionStateStrideBytes = 64u;
	static constexpr UINT32 PathTracingCompactionCounterCount = 16u;
	static constexpr UINT32 PathTracingCompactionIndirectArgsBytes = 128u;

	void SplitUint64(UINT64 value, UINT32& lo, UINT32& hi)
	{
		lo = static_cast<UINT32>(value & 0xffffffffull);
		hi = static_cast<UINT32>(value >> 32u);
	}
}

void Corona::InitPathTracingCompactionPass()
{
	auto seedPso = renderBackend->CreateComputePipelineStateObject();
	auto indirectArgsPso = renderBackend->CreateComputePipelineStateObject();
	auto resolvePso = renderBackend->CreateComputePipelineStateObject();
	auto tracePso = renderBackend->CreateRTPipelineStateObject();
	if (!seedPso || !indirectArgsPso || !resolvePso || !tracePso)
		return;

	const RHIShaderStageMask computeStage = ToRHIShaderStageMask(RHIShaderStage::Compute);
	seedPso->BindUAV(MakeRHIBufferUAV("StateOut", 0, computeStage));
	seedPso->BindUAV(MakeRHIBufferUAV("ActiveListOut", 1, computeStage));
	seedPso->BindUAV(MakeRHIBufferUAV("Counters", 2, computeStage));
	seedPso->BindUAV(MakeRHIBufferUAV("PathRadiance", 3, computeStage));
	seedPso->BindCBV(MakeRHICBV("ViewParameter", 0, sizeof(PathTracingViewParamCB), computeStage));
	seedPso->BindCBV(MakeRHICBV("PathCompaction", 1, sizeof(PathTracingCompactionParamCB), computeStage));

	indirectArgsPso->BindUAV(MakeRHIBufferUAV("Counters", 2, computeStage));
	indirectArgsPso->BindUAV(MakeRHIBufferUAV("IndirectArgs", 5, computeStage, RHIBufferViewKind::Raw));
	indirectArgsPso->BindCBV(MakeRHICBV("PathCompactionIndirect", 2, sizeof(PathTracingCompactionIndirectParamCB), computeStage));

	resolvePso->BindUAV(MakeRHIBufferUAV("PathRadiance", 3, computeStage));
	resolvePso->BindUAV(MakeRHITextureUAV("OutputColor", 4, computeStage));
	resolvePso->BindCBV(MakeRHICBV("ViewParameter", 0, sizeof(PathTracingViewParamCB), computeStage));
	resolvePso->BindCBV(MakeRHICBV("PathCompaction", 1, sizeof(PathTracingCompactionParamCB), computeStage));

	tracePso->SetNumInstances(static_cast<uint32_t>(RayTracingInstances.size()));
	tracePso->AddHitGroup("HitGroup", "PathTracingClosestHit", "PathTracingAnyHit");
	tracePso->AddShader("PathTracingCompactionRayGen", RTPipelineStateObject::RAYGEN);
	const RHIShaderStageMask rayGenStage = ToRHIShaderStageMask(RHIShaderStage::RayGeneration);
	const RHIShaderStageMask closestHitStage = ToRHIShaderStageMask(RHIShaderStage::ClosestHit);
	const RHIShaderStageMask anyHitStage = ToRHIShaderStageMask(RHIShaderStage::AnyHit);
	BindRTBindlessMaterialSchema(*tracePso, closestHitStage | anyHitStage);
	BindRTBindlessGeometrySchema(*tracePso, closestHitStage | anyHitStage);
	tracePso->BindUAV("global", MakeRHITextureUAV("OutAlbedo", 1, rayGenStage));
	tracePso->BindUAV("global", MakeRHITextureUAV("OutSpecularAlbedo", 2, rayGenStage));
	tracePso->BindUAV("global", MakeRHITextureUAV("OutNormal", 3, rayGenStage));
	tracePso->BindUAV("global", MakeRHITextureUAV("OutGeomNormal", 4, rayGenStage));
	tracePso->BindUAV("global", MakeRHITextureUAV("OutVelocity", 5, rayGenStage));
	tracePso->BindUAV("global", MakeRHITextureUAV("OutRoughnessMetallic", 6, rayGenStage));
	tracePso->BindUAV("global", MakeRHITextureUAV("OutDepth", 7, rayGenStage));
	tracePso->BindUAV("global", MakeRHITextureUAV("OutSpecularHitDistance", 8, rayGenStage));
	tracePso->BindUAV("global", MakeRHITextureUAV("OutSpecularMotionVector", 9, rayGenStage));
	tracePso->BindUAV("global", MakeRHIBufferUAV("PathCompactionStateIn", 10, rayGenStage));
	tracePso->BindUAV("global", MakeRHIBufferUAV("PathCompactionStateOut", 11, rayGenStage));
	tracePso->BindUAV("global", MakeRHIBufferUAV("PathCompactionActiveListIn", 12, rayGenStage));
	tracePso->BindUAV("global", MakeRHIBufferUAV("PathCompactionActiveListOut", 13, rayGenStage));
	tracePso->BindUAV("global", MakeRHIBufferUAV("PathCompactionCounters", 14, rayGenStage));
	tracePso->BindUAV("global", MakeRHIBufferUAV("PathCompactionRadiance", 15, rayGenStage));
	tracePso->BindSRV("global", MakeRHIAccelerationStructureSRV("gRtScene", 0, rayGenStage));
	tracePso->BindSRV("global", MakeRHIBufferSRV("PointLightBuffer", 4, rayGenStage));
	tracePso->BindCBV("global", MakeRHICBV("ViewParameter", 0, sizeof(PathTracingViewParam), rayGenStage));
	tracePso->BindCBV("global", MakeRHICBV("PathCompaction", 1, sizeof(PathTracingCompactionParamCB), rayGenStage));
	tracePso->BindSampler("global", MakeRHISampler("sampleWrap", 0, rayGenStage | closestHitStage | anyHitStage));

	tracePso->AddShader("PathTracingMiss", RTPipelineStateObject::MISS);
	tracePso->AddShader("ShadowMiss", RTPipelineStateObject::MISS);

	tracePso->AddShader("PathTracingClosestHit", RTPipelineStateObject::HIT);

	tracePso->AddShader("PathTracingAnyHit", RTPipelineStateObject::ANYHIT);
	tracePso->Configure(8, 256, sizeof(float) * 2);

	const std::wstring computeShader = GetAssetFullPath(L"Shaders\\PathTracingCompactionCompute.hlsl");
	const bool bSeedOk = seedPso->InitCS(computeShader, "PathTracingCompactionSeedCS");
	const bool bIndirectArgsOk = indirectArgsPso->InitCS(computeShader, "PathTracingCompactionIndirectArgsCS");
	const bool bResolveOk = resolvePso->InitCS(computeShader, "PathTracingCompactionResolveCS");
	const bool bTraceOk = tracePso->InitRS("Shaders\\PathTracing.hlsl");
	if (!bSeedOk || !bIndirectArgsOk || !bResolveOk || !bTraceOk)
	{
		AppendCpuRuntimeTrace(L"[PathTracingCompaction] PSO init failed; using mega-kernel fallback");
		return;
	}

	PSO_PATH_TRACING_COMPACTION_SEED = seedPso;
	PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS = indirectArgsPso;
	PSO_PATH_TRACING_COMPACTION_RESOLVE = resolvePso;
	PSO_PATH_TRACING_COMPACTION_TRACE = tracePso;
	AppendCpuRuntimeTrace(L"[PathTracingCompaction] seed/indirect/trace/resolve PSOs ready");
}

bool Corona::EnsurePathTracingCompactionResources(UINT32 width, UINT32 height)
{
	if (width == 0 || height == 0 || !renderBackend)
		return false;

	const UINT64 requestedCapacity64 = static_cast<UINT64>(width) * static_cast<UINT64>(height);
	if (requestedCapacity64 > std::numeric_limits<UINT32>::max())
		return false;

	const UINT32 requestedCapacity = static_cast<UINT32>(requestedCapacity64);
	if (requestedCapacity == PathTracingCompactionCapacity &&
		PathTracingCompactionState[0] &&
		PathTracingCompactionState[1] &&
		PathTracingCompactionActiveList[0] &&
		PathTracingCompactionActiveList[1] &&
		PathTracingCompactionCounter &&
		PathTracingCompactionRadiance &&
		PathTracingCompactionIndirectArgs)
	{
		return true;
	}

	PathTracingCompactionCapacity = 0;
	PathTracingCompactionState[0].reset();
	PathTracingCompactionState[1].reset();
	PathTracingCompactionActiveList[0].reset();
	PathTracingCompactionActiveList[1].reset();
	PathTracingCompactionCounter.reset();
	PathTracingCompactionRadiance.reset();
	PathTracingCompactionIndirectArgs.reset();

	BufferCreateDesc stateDesc = {};
	stateDesc.NumElements = requestedCapacity;
	stateDesc.ElementSize = PathTracingCompactionStateStrideBytes;
	stateDesc.Shape = EBufferShape::Structured;
	stateDesc.bAllowUnorderedAccess = true;
	stateDesc.InitialState = EInitialResourceState::ShaderRead;

	BufferCreateDesc listDesc = {};
	listDesc.NumElements = requestedCapacity;
	listDesc.ElementSize = sizeof(UINT32);
	listDesc.Shape = EBufferShape::Structured;
	listDesc.bAllowUnorderedAccess = true;
	listDesc.InitialState = EInitialResourceState::ShaderRead;

	BufferCreateDesc counterDesc = {};
	counterDesc.NumElements = PathTracingCompactionCounterCount;
	counterDesc.ElementSize = sizeof(UINT32);
	counterDesc.Shape = EBufferShape::Structured;
	counterDesc.bAllowUnorderedAccess = true;
	counterDesc.InitialState = EInitialResourceState::ShaderRead;

	BufferCreateDesc radianceDesc = {};
	radianceDesc.NumElements = requestedCapacity;
	radianceDesc.ElementSize = sizeof(float) * 4u;
	radianceDesc.Shape = EBufferShape::Structured;
	radianceDesc.bAllowUnorderedAccess = true;
	radianceDesc.InitialState = EInitialResourceState::ShaderRead;

	BufferCreateDesc indirectArgsDesc = {};
	indirectArgsDesc.NumElements = PathTracingCompactionIndirectArgsBytes / sizeof(UINT32);
	indirectArgsDesc.ElementSize = sizeof(UINT32);
	indirectArgsDesc.Shape = EBufferShape::ByteAddress;
	indirectArgsDesc.bAllowUnorderedAccess = true;
	indirectArgsDesc.InitialState = EInitialResourceState::ShaderRead;

	PathTracingCompactionState[0] = renderBackend->CreateBuffer(stateDesc);
	PathTracingCompactionState[1] = renderBackend->CreateBuffer(stateDesc);
	PathTracingCompactionActiveList[0] = renderBackend->CreateBuffer(listDesc);
	PathTracingCompactionActiveList[1] = renderBackend->CreateBuffer(listDesc);
	PathTracingCompactionCounter = renderBackend->CreateBuffer(counterDesc);
	PathTracingCompactionRadiance = renderBackend->CreateBuffer(radianceDesc);
	PathTracingCompactionIndirectArgs = renderBackend->CreateBuffer(indirectArgsDesc);

	if (!PathTracingCompactionState[0] ||
		!PathTracingCompactionState[1] ||
		!PathTracingCompactionActiveList[0] ||
		!PathTracingCompactionActiveList[1] ||
		!PathTracingCompactionCounter ||
		!PathTracingCompactionRadiance ||
		!PathTracingCompactionIndirectArgs)
	{
		PathTracingCompactionState[0].reset();
		PathTracingCompactionState[1].reset();
		PathTracingCompactionActiveList[0].reset();
		PathTracingCompactionActiveList[1].reset();
		PathTracingCompactionCounter.reset();
		PathTracingCompactionRadiance.reset();
		PathTracingCompactionIndirectArgs.reset();
		return false;
	}

	PathTracingCompactionCapacity = requestedCapacity;
	bPathTracingCompactionResourcesNeedDescriptorRefresh = true;
	AppendCpuRuntimeTrace(
		L"[PathTracingCompaction] allocated 1spp wavefront buffers, capacity=" +
		std::to_wstring(PathTracingCompactionCapacity));
	return true;
}

bool Corona::PathTracingCompactionPass(Texture* outputColor, const PathTracingViewParamCB& dispatchViewParam, bool bWritePrimaryGBuffer)
{
	if (!TLAS || !outputColor)
		return false;

	if (!EnsureRTMaterialRecordBuffer())
		return false;

	if (dispatchViewParam.DebugMode != 0 || dispatchViewParam.SamplesPerPixel != 1u)
	{
		if (!bPathTracingCompactionFallbackLogged)
		{
			AppendCpuRuntimeTrace(L"[PathTracingCompaction] supports non-debug 1spp frames only; using mega-kernel fallback");
			bPathTracingCompactionFallbackLogged = true;
		}
		return false;
	}

	if (!EnsurePathTracingCompactionResources(m_width, m_height))
	{
		if (!bPathTracingCompactionFallbackLogged)
		{
			AppendCpuRuntimeTrace(L"[PathTracingCompaction] resource allocation failed; using mega-kernel fallback");
			bPathTracingCompactionFallbackLogged = true;
		}
		return false;
	}

	if (bPathTracingCompactionResourcesNeedDescriptorRefresh)
	{
		bPathTracingCompactionResourcesNeedDescriptorRefresh = false;
		if (!bPathTracingCompactionFallbackLogged)
		{
			AppendCpuRuntimeTrace(L"[PathTracingCompaction] descriptors will refresh next frame; using mega-kernel fallback once");
			bPathTracingCompactionFallbackLogged = true;
		}
		return false;
	}

	if (!PSO_PATH_TRACING_COMPACTION_TRACE ||
		!PSO_PATH_TRACING_COMPACTION_SEED ||
		!PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS ||
		!PSO_PATH_TRACING_COMPACTION_RESOLVE)
	{
		InitPathTracingCompactionPass();
	}

	if (!PSO_PATH_TRACING_COMPACTION_TRACE ||
		!PSO_PATH_TRACING_COMPACTION_SEED ||
		!PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS ||
		!PSO_PATH_TRACING_COMPACTION_RESOLVE)
	{
		if (!bPathTracingCompactionFallbackLogged)
		{
			AppendCpuRuntimeTrace(L"[PathTracingCompaction] PSO unavailable; using mega-kernel fallback");
			bPathTracingCompactionFallbackLogged = true;
		}
		return false;
	}

	bPathTracingCompactionFallbackLogged = false;
	if (!bPathTracingCompactionDispatchLogged)
	{
		AppendCpuRuntimeTrace(
			L"[PathTracingCompaction] dispatching indirect seed/trace/resolve, capacity=" +
			std::to_wstring(PathTracingCompactionCapacity) +
			L", maxBounces=" + std::to_wstring(dispatchViewParam.MaxBounces) +
			L", rrGBuffer=" + std::to_wstring(bWritePrimaryGBuffer ? 1 : 0));
		bPathTracingCompactionDispatchLogged = true;
	}

	auto transitionCompactionBuffers = [this](EResourceState before, EResourceState after)
	{
		renderBackend->TransitionBuffer(PathTracingCompactionState[0].get(), before, after);
		renderBackend->TransitionBuffer(PathTracingCompactionState[1].get(), before, after);
		renderBackend->TransitionBuffer(PathTracingCompactionActiveList[0].get(), before, after);
		renderBackend->TransitionBuffer(PathTracingCompactionActiveList[1].get(), before, after);
		renderBackend->TransitionBuffer(PathTracingCompactionCounter.get(), before, after);
		renderBackend->TransitionBuffer(PathTracingCompactionRadiance.get(), before, after);
	};

	auto barrierCompactionBuffers = [this]()
	{
		renderBackend->UAVBarrier(PathTracingCompactionState[0].get());
		renderBackend->UAVBarrier(PathTracingCompactionState[1].get());
		renderBackend->UAVBarrier(PathTracingCompactionActiveList[0].get());
		renderBackend->UAVBarrier(PathTracingCompactionActiveList[1].get());
		renderBackend->UAVBarrier(PathTracingCompactionCounter.get());
		renderBackend->UAVBarrier(PathTracingCompactionRadiance.get());
	};

	transitionCompactionBuffers(EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionBuffer(PathTracingCompactionIndirectArgs.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	PathTracingCompactionParamCB compactionParam = {};
	compactionParam.RenderWidth = m_width;
	compactionParam.RenderHeight = m_height;
	compactionParam.Capacity = PathTracingCompactionCapacity;
	compactionParam.BounceIndex = 0;

	renderBackend->BeginGpuMarker(0xff66ccffu, "PathTracingCompactionSeed");
	PSO_PATH_TRACING_COMPACTION_SEED->SetBufferUAV("StateOut", PathTracingCompactionState[0].get());
	PSO_PATH_TRACING_COMPACTION_SEED->SetBufferUAV("ActiveListOut", PathTracingCompactionActiveList[0].get());
	PSO_PATH_TRACING_COMPACTION_SEED->SetBufferUAV("Counters", PathTracingCompactionCounter.get());
	PSO_PATH_TRACING_COMPACTION_SEED->SetBufferUAV("PathRadiance", PathTracingCompactionRadiance.get());
	PSO_PATH_TRACING_COMPACTION_SEED->SetCBVValue("ViewParameter", const_cast<PathTracingViewParamCB*>(&dispatchViewParam));
	PSO_PATH_TRACING_COMPACTION_SEED->SetCBVValue("PathCompaction", &compactionParam);
	PSO_PATH_TRACING_COMPACTION_SEED->Apply();
	renderBackend->Dispatch((m_width + 7u) / 8u, (m_height + 7u) / 8u, 1u);
	renderBackend->EndGpuMarker();
	barrierCompactionBuffers();

	auto makeIndirectParam = [this](const RtDispatchRaysIndirectTemplate& dispatchTemplate, UINT32 bounceIndex)
	{
		PathTracingCompactionIndirectParamCB param = {};
		SplitUint64(dispatchTemplate.RayGenerationStartAddress, param.RayGenStartLo, param.RayGenStartHi);
		SplitUint64(dispatchTemplate.RayGenerationSizeInBytes, param.RayGenSizeLo, param.RayGenSizeHi);
		SplitUint64(dispatchTemplate.MissStartAddress, param.MissStartLo, param.MissStartHi);
		SplitUint64(dispatchTemplate.MissSizeInBytes, param.MissSizeLo, param.MissSizeHi);
		SplitUint64(dispatchTemplate.MissStrideInBytes, param.MissStrideLo, param.MissStrideHi);
		SplitUint64(dispatchTemplate.HitGroupStartAddress, param.HitStartLo, param.HitStartHi);
		SplitUint64(dispatchTemplate.HitGroupSizeInBytes, param.HitSizeLo, param.HitSizeHi);
		SplitUint64(dispatchTemplate.HitGroupStrideInBytes, param.HitStrideLo, param.HitStrideHi);
		SplitUint64(dispatchTemplate.CallableStartAddress, param.CallableStartLo, param.CallableStartHi);
		SplitUint64(dispatchTemplate.CallableSizeInBytes, param.CallableSizeLo, param.CallableSizeHi);
		SplitUint64(dispatchTemplate.CallableStrideInBytes, param.CallableStrideLo, param.CallableStrideHi);
		param.MaxDispatchWidth = PathTracingCompactionCapacity;
		param.CounterIndex = std::min(bounceIndex, PathTracingCompactionCounterCount - 1u);
		param.DispatchHeight = 1u;
		param.DispatchDepth = 1u;
		return param;
	};

	const UINT32 bounceCount = std::min(std::max(dispatchViewParam.MaxBounces, 1u), PathTracingCompactionCounterCount - 1u);
	for (UINT32 bounceIndex = 0; bounceIndex < bounceCount; ++bounceIndex)
	{
		const UINT32 readIndex = bounceIndex & 1u;
		const UINT32 writeIndex = 1u - readIndex;
		compactionParam.BounceIndex = bounceIndex;

		RTPassBuilder pass(*this, PSO_PATH_TRACING_COMPACTION_TRACE);
		pass.BeginScene()
			.SetTextureUAV("global", "OutAlbedo", AlbedoBuffer.get())
			.SetTextureUAV("global", "OutSpecularAlbedo", SpecularAlbedoBuffer.get())
			.SetTextureUAV("global", "OutNormal", NormalBuffers[ColorBufferWriteIndex].get())
			.SetTextureUAV("global", "OutGeomNormal", GeomNormalBuffers[ColorBufferWriteIndex].get())
			.SetTextureUAV("global", "OutVelocity", VelocityBuffer.get())
			.SetTextureUAV("global", "OutRoughnessMetallic", RoughnessMetalicBuffer.get())
			.SetTextureUAV("global", "OutDepth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get())
			.SetTextureUAV("global", "OutSpecularHitDistance", PathTracingSpecularHitDistanceBuffer.get())
			.SetTextureUAV("global", "OutSpecularMotionVector", PathTracingSpecularMotionVectorBuffer.get())
			.SetBufferUAV("global", "PathCompactionStateIn", PathTracingCompactionState[readIndex].get())
			.SetBufferUAV("global", "PathCompactionStateOut", PathTracingCompactionState[writeIndex].get())
			.SetBufferUAV("global", "PathCompactionActiveListIn", PathTracingCompactionActiveList[readIndex].get())
			.SetBufferUAV("global", "PathCompactionActiveListOut", PathTracingCompactionActiveList[writeIndex].get())
			.SetBufferUAV("global", "PathCompactionCounters", PathTracingCompactionCounter.get())
			.SetBufferUAV("global", "PathCompactionRadiance", PathTracingCompactionRadiance.get())
			.SetAccelerationStructure("global", "gRtScene", TLAS)
			.SetBufferSRV("global", "PointLightBuffer", PathTracingPointLightBuffer.get())
			.SetCBVValue("global", "ViewParameter", const_cast<PathTracingViewParamCB*>(&dispatchViewParam))
			.SetCBVValue("global", "PathCompaction", &compactionParam)
			.SetSampler("global", "sampleWrap", samplerWrap.get());
		pass.SetBindlessTextureTable("global", "MaterialTextures")
			.SetBufferSRV("global", "RtMaterials", RTMaterialRecordBuffer.get());

		RTSceneHitProgramDesc hitProgramDesc;
		pass.BindSceneHitPrograms(hitProgramDesc);

		RtDispatchRaysIndirectTemplate dispatchTemplate = {};
		const bool bCanDispatchIndirect =
			pass.GetDispatchRaysIndirectTemplate(PathTracingCompactionCapacity, 1u, dispatchTemplate) &&
			PathTracingCompactionIndirectArgs;

		bool bTraceDispatched = false;
		if (bCanDispatchIndirect)
		{
			PathTracingCompactionIndirectParamCB indirectParam = makeIndirectParam(dispatchTemplate, bounceIndex);
			renderBackend->BeginGpuMarker(0xff44aaffu, "PathTracingCompactionIndirectArgs");
			PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS->SetBufferUAV("Counters", PathTracingCompactionCounter.get());
			PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS->SetBufferUAV("IndirectArgs", PathTracingCompactionIndirectArgs.get());
			PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS->SetCBVValue("PathCompactionIndirect", &indirectParam);
			PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS->Apply();
			renderBackend->Dispatch(1u, 1u, 1u);
			renderBackend->EndGpuMarker();
			renderBackend->UAVBarrier(PathTracingCompactionIndirectArgs.get());

			renderBackend->TransitionBuffer(PathTracingCompactionIndirectArgs.get(), EResourceState::UnorderedAccess, EResourceState::IndirectArgument);
			renderBackend->BeginGpuMarker(0xff66cc99u, "PathTracingCompactionTrace");
			bTraceDispatched = pass.DispatchIndirect(PathTracingCompactionIndirectArgs.get(), 0);
			renderBackend->EndGpuMarker();
			renderBackend->TransitionBuffer(PathTracingCompactionIndirectArgs.get(), EResourceState::IndirectArgument, EResourceState::UnorderedAccess);
		}

		if (!bTraceDispatched)
		{
			renderBackend->BeginGpuMarker(0xff66cc99u, "PathTracingCompactionTrace");
			pass.Dispatch(m_width, m_height);
			renderBackend->EndGpuMarker();
		}
		barrierCompactionBuffers();
	}

	compactionParam.BounceIndex = bounceCount;
	renderBackend->BeginGpuMarker(0xffffcc66u, "PathTracingCompactionResolve");
	PSO_PATH_TRACING_COMPACTION_RESOLVE->SetBufferUAV("PathRadiance", PathTracingCompactionRadiance.get());
	PSO_PATH_TRACING_COMPACTION_RESOLVE->SetTextureUAV("OutputColor", outputColor);
	PSO_PATH_TRACING_COMPACTION_RESOLVE->SetCBVValue("ViewParameter", const_cast<PathTracingViewParamCB*>(&dispatchViewParam));
	PSO_PATH_TRACING_COMPACTION_RESOLVE->SetCBVValue("PathCompaction", &compactionParam);
	PSO_PATH_TRACING_COMPACTION_RESOLVE->Apply();
	renderBackend->Dispatch((m_width + 7u) / 8u, (m_height + 7u) / 8u, 1u);
	renderBackend->EndGpuMarker();
	renderBackend->UAVBarrier(PathTracingCompactionRadiance.get());

	renderBackend->TransitionBuffer(PathTracingCompactionIndirectArgs.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	transitionCompactionBuffers(EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	(void)bWritePrimaryGBuffer;
	return true;
}
