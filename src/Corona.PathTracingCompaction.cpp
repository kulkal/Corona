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
#include "RenderGraph.h"

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

	PathTracingCompactionParamCB compactionParam = {};
	compactionParam.RenderWidth = m_width;
	compactionParam.RenderHeight = m_height;
	compactionParam.Capacity = PathTracingCompactionCapacity;
	compactionParam.BounceIndex = 0;

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

	RenderGraph rg(renderBackend.get());
	RGTextureRef outputColorTarget = rg.ImportTexture("PathTracingCompaction.OutputColor", outputColor, EResourceState::ShaderRead);
	RGTextureRef outAlbedo = bWritePrimaryGBuffer && AlbedoBuffer
		? rg.ImportTexture("PathTracingCompaction.OutAlbedo", AlbedoBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outSpecularAlbedo = bWritePrimaryGBuffer && SpecularAlbedoBuffer
		? rg.ImportTexture("PathTracingCompaction.OutSpecularAlbedo", SpecularAlbedoBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outNormal = bWritePrimaryGBuffer && NormalBuffers[ColorBufferWriteIndex]
		? rg.ImportTexture("PathTracingCompaction.OutNormal", NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outGeomNormal = bWritePrimaryGBuffer && GeomNormalBuffers[ColorBufferWriteIndex]
		? rg.ImportTexture("PathTracingCompaction.OutGeomNormal", GeomNormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outVelocity = bWritePrimaryGBuffer && VelocityBuffer
		? rg.ImportTexture("PathTracingCompaction.OutVelocity", VelocityBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outRoughnessMetallic = bWritePrimaryGBuffer && RoughnessMetalicBuffer
		? rg.ImportTexture("PathTracingCompaction.OutRoughnessMetallic", RoughnessMetalicBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outDepth = bWritePrimaryGBuffer && UnjitteredDepthBuffers[ColorBufferWriteIndex]
		? rg.ImportTexture("PathTracingCompaction.OutDepth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outSpecularHitDistance = bWritePrimaryGBuffer && PathTracingSpecularHitDistanceBuffer
		? rg.ImportTexture("PathTracingCompaction.OutSpecularHitDistance", PathTracingSpecularHitDistanceBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGTextureRef outSpecularMotionVector = bWritePrimaryGBuffer && PathTracingSpecularMotionVectorBuffer
		? rg.ImportTexture("PathTracingCompaction.OutSpecularMotionVector", PathTracingSpecularMotionVectorBuffer.get(), EResourceState::ShaderRead)
		: RGTextureRef{};
	RGBufferRef state[2] =
	{
		rg.ImportBuffer("PathTracingCompaction.State0", PathTracingCompactionState[0].get(), EResourceState::ShaderRead),
		rg.ImportBuffer("PathTracingCompaction.State1", PathTracingCompactionState[1].get(), EResourceState::ShaderRead),
	};
	RGBufferRef activeList[2] =
	{
		rg.ImportBuffer("PathTracingCompaction.ActiveList0", PathTracingCompactionActiveList[0].get(), EResourceState::ShaderRead),
		rg.ImportBuffer("PathTracingCompaction.ActiveList1", PathTracingCompactionActiveList[1].get(), EResourceState::ShaderRead),
	};
	RGBufferRef counter = rg.ImportBuffer("PathTracingCompaction.Counter", PathTracingCompactionCounter.get(), EResourceState::ShaderRead);
	RGBufferRef radiance = rg.ImportBuffer("PathTracingCompaction.Radiance", PathTracingCompactionRadiance.get(), EResourceState::ShaderRead);
	RGBufferRef indirectArgs = rg.ImportBuffer("PathTracingCompaction.IndirectArgs", PathTracingCompactionIndirectArgs.get(), EResourceState::ShaderRead);
	RGBufferRef pointLightBuffer = rg.ImportBuffer("PathTracingCompaction.PointLightBuffer", PathTracingPointLightBuffer.get(), EResourceState::ShaderRead);
	RGBufferRef rtMaterials = rg.ImportBuffer("PathTracingCompaction.RtMaterials", RTMaterialRecordBuffer.get(), EResourceState::ShaderRead);

	rg.ExportTexture(outputColorTarget, EResourceState::ShaderRead);
	if (outAlbedo.IsValid())
		rg.ExportTexture(outAlbedo, EResourceState::ShaderRead);
	if (outSpecularAlbedo.IsValid())
		rg.ExportTexture(outSpecularAlbedo, EResourceState::ShaderRead);
	if (outNormal.IsValid())
		rg.ExportTexture(outNormal, EResourceState::ShaderRead);
	if (outGeomNormal.IsValid())
		rg.ExportTexture(outGeomNormal, EResourceState::ShaderRead);
	if (outVelocity.IsValid())
		rg.ExportTexture(outVelocity, EResourceState::ShaderRead);
	if (outRoughnessMetallic.IsValid())
		rg.ExportTexture(outRoughnessMetallic, EResourceState::ShaderRead);
	if (outDepth.IsValid())
		rg.ExportTexture(outDepth, EResourceState::ShaderRead);
	if (outSpecularHitDistance.IsValid())
		rg.ExportTexture(outSpecularHitDistance, EResourceState::ShaderRead);
	if (outSpecularMotionVector.IsValid())
		rg.ExportTexture(outSpecularMotionVector, EResourceState::ShaderRead);
	for (uint32_t index = 0; index < 2; ++index)
	{
		rg.ExportBuffer(state[index], EResourceState::ShaderRead);
		rg.ExportBuffer(activeList[index], EResourceState::ShaderRead);
	}
	rg.ExportBuffer(counter, EResourceState::ShaderRead);
	rg.ExportBuffer(radiance, EResourceState::ShaderRead);
	rg.ExportBuffer(indirectArgs, EResourceState::ShaderRead);

	const PathTracingViewParamCB viewParam = dispatchViewParam;
	const PathTracingCompactionParamCB seedParam = compactionParam;
	rg.AddPass(
		"PathTracingCompactionSeed",
		ERGPassFlags::Compute,
		[&](RGPassBuilder& builder)
		{
			builder.WriteBuffer(state[0], EResourceState::UnorderedAccess)
				.WriteBuffer(activeList[0], EResourceState::UnorderedAccess)
				.ReadWriteBuffer(counter, EResourceState::UnorderedAccess)
				.ReadWriteBuffer(radiance, EResourceState::UnorderedAccess);
		},
		[&, seedParam, viewParam](RGContext& ctx)
		{
			PathTracingCompactionParamCB passCompactionParam = seedParam;
			PathTracingViewParamCB passViewParam = viewParam;
			PSO_PATH_TRACING_COMPACTION_SEED->SetBufferUAV("StateOut", ctx.GetBuffer(state[0]));
			PSO_PATH_TRACING_COMPACTION_SEED->SetBufferUAV("ActiveListOut", ctx.GetBuffer(activeList[0]));
			PSO_PATH_TRACING_COMPACTION_SEED->SetBufferUAV("Counters", ctx.GetBuffer(counter));
			PSO_PATH_TRACING_COMPACTION_SEED->SetBufferUAV("PathRadiance", ctx.GetBuffer(radiance));
			PSO_PATH_TRACING_COMPACTION_SEED->SetCBVValue("ViewParameter", &passViewParam);
			PSO_PATH_TRACING_COMPACTION_SEED->SetCBVValue("PathCompaction", &passCompactionParam);
			PSO_PATH_TRACING_COMPACTION_SEED->Apply();
			renderBackend->Dispatch((m_width + 7u) / 8u, (m_height + 7u) / 8u, 1u);
		});

	const UINT32 bounceCount = std::min(std::max(dispatchViewParam.MaxBounces, 1u), PathTracingCompactionCounterCount - 1u);
	for (UINT32 bounceIndex = 0; bounceIndex < bounceCount; ++bounceIndex)
	{
		const UINT32 readIndex = bounceIndex & 1u;
		const UINT32 writeIndex = 1u - readIndex;
		compactionParam.BounceIndex = bounceIndex;

		const PathTracingCompactionParamCB traceParam = compactionParam;
		rg.AddPass(
			"PathTracingCompactionTrace",
			ERGPassFlags::RayTracing,
			[&, readIndex, writeIndex](RGPassBuilder& builder)
			{
				builder.ReadWriteBuffer(state[readIndex], EResourceState::UnorderedAccess)
					.ReadWriteBuffer(state[writeIndex], EResourceState::UnorderedAccess)
					.ReadWriteBuffer(activeList[readIndex], EResourceState::UnorderedAccess)
					.ReadWriteBuffer(activeList[writeIndex], EResourceState::UnorderedAccess)
					.ReadWriteBuffer(counter, EResourceState::UnorderedAccess)
					.ReadWriteBuffer(radiance, EResourceState::UnorderedAccess)
					.ReadWriteBuffer(indirectArgs, EResourceState::UnorderedAccess)
					.ReadBuffer(pointLightBuffer, EResourceState::ShaderRead)
					.ReadBuffer(rtMaterials, EResourceState::ShaderRead);
				if (outAlbedo.IsValid())
					builder.ReadWriteTexture(outAlbedo, EResourceState::UnorderedAccess);
				if (outSpecularAlbedo.IsValid())
					builder.ReadWriteTexture(outSpecularAlbedo, EResourceState::UnorderedAccess);
				if (outNormal.IsValid())
					builder.ReadWriteTexture(outNormal, EResourceState::UnorderedAccess);
				if (outGeomNormal.IsValid())
					builder.ReadWriteTexture(outGeomNormal, EResourceState::UnorderedAccess);
				if (outVelocity.IsValid())
					builder.ReadWriteTexture(outVelocity, EResourceState::UnorderedAccess);
				if (outRoughnessMetallic.IsValid())
					builder.ReadWriteTexture(outRoughnessMetallic, EResourceState::UnorderedAccess);
				if (outDepth.IsValid())
					builder.ReadWriteTexture(outDepth, EResourceState::UnorderedAccess);
				if (outSpecularHitDistance.IsValid())
					builder.ReadWriteTexture(outSpecularHitDistance, EResourceState::UnorderedAccess);
				if (outSpecularMotionVector.IsValid())
					builder.ReadWriteTexture(outSpecularMotionVector, EResourceState::UnorderedAccess);
			},
			[&, readIndex, writeIndex, bounceIndex, traceParam, viewParam](RGContext& ctx)
			{
				PathTracingCompactionParamCB passCompactionParam = traceParam;
				PathTracingViewParamCB passViewParam = viewParam;
				RTPassBuilder pass(*this, PSO_PATH_TRACING_COMPACTION_TRACE, ERtProfilePass::PathTracingCompaction);
				pass.BeginScene()
					.SetTextureUAV("global", "OutAlbedo", outAlbedo.IsValid() ? ctx.GetTexture(outAlbedo) : nullptr)
					.SetTextureUAV("global", "OutSpecularAlbedo", outSpecularAlbedo.IsValid() ? ctx.GetTexture(outSpecularAlbedo) : nullptr)
					.SetTextureUAV("global", "OutNormal", outNormal.IsValid() ? ctx.GetTexture(outNormal) : nullptr)
					.SetTextureUAV("global", "OutGeomNormal", outGeomNormal.IsValid() ? ctx.GetTexture(outGeomNormal) : nullptr)
					.SetTextureUAV("global", "OutVelocity", outVelocity.IsValid() ? ctx.GetTexture(outVelocity) : nullptr)
					.SetTextureUAV("global", "OutRoughnessMetallic", outRoughnessMetallic.IsValid() ? ctx.GetTexture(outRoughnessMetallic) : nullptr)
					.SetTextureUAV("global", "OutDepth", outDepth.IsValid() ? ctx.GetTexture(outDepth) : nullptr)
					.SetTextureUAV("global", "OutSpecularHitDistance", outSpecularHitDistance.IsValid() ? ctx.GetTexture(outSpecularHitDistance) : nullptr)
					.SetTextureUAV("global", "OutSpecularMotionVector", outSpecularMotionVector.IsValid() ? ctx.GetTexture(outSpecularMotionVector) : nullptr)
					.SetBufferUAV("global", "PathCompactionStateIn", ctx.GetBuffer(state[readIndex]))
					.SetBufferUAV("global", "PathCompactionStateOut", ctx.GetBuffer(state[writeIndex]))
					.SetBufferUAV("global", "PathCompactionActiveListIn", ctx.GetBuffer(activeList[readIndex]))
					.SetBufferUAV("global", "PathCompactionActiveListOut", ctx.GetBuffer(activeList[writeIndex]))
					.SetBufferUAV("global", "PathCompactionCounters", ctx.GetBuffer(counter))
					.SetBufferUAV("global", "PathCompactionRadiance", ctx.GetBuffer(radiance))
					.SetAccelerationStructure("global", "gRtScene", TLAS)
					.SetBufferSRV("global", "PointLightBuffer", ctx.GetBuffer(pointLightBuffer))
					.SetCBVValue("global", "ViewParameter", &passViewParam)
					.SetCBVValue("global", "PathCompaction", &passCompactionParam)
					.SetSampler("global", "sampleWrap", samplerWrap.get());
				pass.SetBindlessTextureTable("global", "MaterialTextures")
					.SetBufferSRV("global", "RtMaterials", ctx.GetBuffer(rtMaterials));

				RTSceneHitProgramDesc hitProgramDesc;
				pass.BindSceneHitPrograms(hitProgramDesc);

				RtDispatchRaysIndirectTemplate dispatchTemplate = {};
				const bool bCanDispatchIndirect =
					pass.GetDispatchRaysIndirectTemplate(PathTracingCompactionCapacity, 1u, dispatchTemplate) &&
					ctx.GetBuffer(indirectArgs);

				bool bTraceDispatched = false;
				if (bCanDispatchIndirect)
				{
					PathTracingCompactionIndirectParamCB indirectParam = makeIndirectParam(dispatchTemplate, bounceIndex);
					PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS->SetBufferUAV("Counters", ctx.GetBuffer(counter));
					PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS->SetBufferUAV("IndirectArgs", ctx.GetBuffer(indirectArgs));
					PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS->SetCBVValue("PathCompactionIndirect", &indirectParam);
					PSO_PATH_TRACING_COMPACTION_INDIRECT_ARGS->Apply();
					renderBackend->Dispatch(1u, 1u, 1u);
					renderBackend->UAVBarrier(ctx.GetBuffer(indirectArgs));

					renderBackend->TransitionBuffer(ctx.GetBuffer(indirectArgs), EResourceState::UnorderedAccess, EResourceState::IndirectArgument);
					bTraceDispatched = pass.DispatchIndirect(ctx.GetBuffer(indirectArgs), 0);
					renderBackend->TransitionBuffer(ctx.GetBuffer(indirectArgs), EResourceState::IndirectArgument, EResourceState::UnorderedAccess);
				}

				if (!bTraceDispatched)
					pass.Dispatch(m_width, m_height);
			});
	}

	compactionParam.BounceIndex = bounceCount;
	const PathTracingCompactionParamCB resolveParam = compactionParam;
	rg.AddPass(
		"PathTracingCompactionResolve",
		ERGPassFlags::Compute,
		[&](RGPassBuilder& builder)
		{
			builder.ReadWriteBuffer(radiance, EResourceState::UnorderedAccess)
				.ReadWriteTexture(outputColorTarget, EResourceState::UnorderedAccess);
		},
		[&, resolveParam, viewParam](RGContext& ctx)
		{
			PathTracingCompactionParamCB passCompactionParam = resolveParam;
			PathTracingViewParamCB passViewParam = viewParam;
			PSO_PATH_TRACING_COMPACTION_RESOLVE->SetBufferUAV("PathRadiance", ctx.GetBuffer(radiance));
			PSO_PATH_TRACING_COMPACTION_RESOLVE->SetTextureUAV("OutputColor", ctx.GetTexture(outputColorTarget));
			PSO_PATH_TRACING_COMPACTION_RESOLVE->SetCBVValue("ViewParameter", &passViewParam);
			PSO_PATH_TRACING_COMPACTION_RESOLVE->SetCBVValue("PathCompaction", &passCompactionParam);
			PSO_PATH_TRACING_COMPACTION_RESOLVE->Apply();
			renderBackend->Dispatch((m_width + 7u) / 8u, (m_height + 7u) / 8u, 1u);
		});

	if (!rg.Execute())
		return false;

	(void)bWritePrimaryGBuffer;
	return true;
}
