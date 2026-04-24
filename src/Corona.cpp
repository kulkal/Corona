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
#include <dxcapi.use.h>
//#include <dxcapi.h>
#include "Utils.h"
#include <iostream>
#include <algorithm>
#include <array>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <variant>
#include <codecvt>
#include <dxgidebug.h>
#include "assimp/include/Importer.hpp"
#include "assimp/include/scene.h"
#include "assimp/include/postprocess.h"
//#pragma comment(lib, "assimp\\lib\\assimp.lib")
#include "GFSDK_Aftermath/include/GFSDK_Aftermath.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imGuIZMO.h"
#include "DirectXTex.h"
#include <wincodec.h>
#include <filesystem>
#include <fstream>
#if WITH_STREAMLINE
#include "sl_core_api.h"
#include "sl_core_types.h"
#include "sl_dlss.h"
#include "sl_dlss_d.h"
#endif



#ifdef _DEBUG
#define new DEBUG_CLIENTBLOCK
#endif

#define arraysize(a) (sizeof(a)/sizeof(a[0]))
#define align_to(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)

static dxc::DxcDllSupport gDxcDllHelper;


using namespace glm;
using namespace DirectX;

#if WITH_STREAMLINE
namespace
{
	sl::float4x4 ToSLMatrix(const glm::mat4x4& matrix)
	{
		glm::mat4x4 rowMajor = glm::transpose(matrix);
		sl::float4x4 result{};
		for (uint32_t row = 0; row < 4; ++row)
		{
			result.setRow(row, sl::float4(rowMajor[row][0], rowMajor[row][1], rowMajor[row][2], rowMajor[row][3]));
		}
		return result;
	}

	sl::DLSSMode ToSLDLSSMode(Corona::EDLSSQualityMode mode)
	{
		switch (mode)
		{
		case Corona::EDLSSQualityMode::QUALITY:
			return sl::DLSSMode::eMaxQuality;
		case Corona::EDLSSQualityMode::BALANCED:
			return sl::DLSSMode::eBalanced;
		case Corona::EDLSSQualityMode::PERFORMANCE:
			return sl::DLSSMode::eMaxPerformance;
		case Corona::EDLSSQualityMode::ULTRA_PERFORMANCE:
			return sl::DLSSMode::eUltraPerformance;
		default:
			return sl::DLSSMode::eMaxQuality;
		}
	}

	sl::Constants BuildStreamlineConstants(
		const glm::mat4x4& unjitteredProjMat,
		const glm::mat4x4& unjitteredViewProjMat,
		const glm::mat4x4& prevUnjitteredViewProjMat,
		const glm::mat4x4& invViewMat,
		const glm::vec2& currentJitter,
		const glm::vec3& cameraLookDirection,
		float nearPlane,
		float farPlane,
		float fov,
		float aspectRatio,
		bool bResetNeeded)
	{
		sl::Constants consts{};
		const glm::mat4x4 currentProj = unjitteredProjMat;
		const glm::mat4x4 currentInvProj = glm::inverse(currentProj);
		const glm::mat4x4 currentClipToWorld = glm::inverse(unjitteredViewProjMat);
		const glm::mat4x4 prevClipToWorld = glm::inverse(prevUnjitteredViewProjMat);
		const glm::mat4x4 clipToPrevClip = prevUnjitteredViewProjMat * currentClipToWorld;
		const glm::mat4x4 prevClipToClip = unjitteredViewProjMat * prevClipToWorld;

		consts.cameraViewToClip = ToSLMatrix(currentProj);
		consts.clipToCameraView = ToSLMatrix(currentInvProj);
		consts.clipToLensClip = ToSLMatrix(glm::mat4x4(1.0f));
		consts.clipToPrevClip = ToSLMatrix(clipToPrevClip);
		consts.prevClipToClip = ToSLMatrix(prevClipToClip);
		// Our projection jitter matrix applies half-pixel offsets in clip space,
		// so convert the stored sequence sample to the actual pixel jitter used.
		const glm::vec2 appliedPixelJitter = currentJitter * 0.5f;
		consts.jitterOffset = sl::float2(appliedPixelJitter.x, appliedPixelJitter.y);
		// Corona's velocity buffer stores (current - previous) in normalized screen space.
		// Streamline/DLSS expects vectors that reproject current pixels back to the
		// previous frame, so flip the sign at integration time without affecting the
		// engine's internal TAA/denoiser path.
		consts.mvecScale = sl::float2(-1.0f, -1.0f);
		consts.cameraPinholeOffset = sl::float2(0.0f, 0.0f);

		glm::vec3 cameraPos = glm::vec3(invViewMat[3]);
		glm::vec3 cameraRight = glm::normalize(glm::vec3(invViewMat[0]));
		glm::vec3 cameraUp = glm::normalize(glm::vec3(invViewMat[1]));
		glm::vec3 cameraFwd = glm::normalize(cameraLookDirection);
		consts.cameraPos = sl::float3(cameraPos.x, cameraPos.y, cameraPos.z);
		consts.cameraRight = sl::float3(cameraRight.x, cameraRight.y, cameraRight.z);
		consts.cameraUp = sl::float3(cameraUp.x, cameraUp.y, cameraUp.z);
		consts.cameraFwd = sl::float3(cameraFwd.x, cameraFwd.y, cameraFwd.z);
		consts.cameraNear = nearPlane;
		consts.cameraFar = farPlane;
		consts.cameraFOV = fov;
		consts.cameraAspectRatio = aspectRatio;
		consts.depthInverted = sl::Boolean::eFalse;
		consts.cameraMotionIncluded = sl::Boolean::eTrue;
		consts.motionVectors3D = sl::Boolean::eFalse;
		consts.reset = bResetNeeded ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		consts.orthographicProjection = sl::Boolean::eFalse;
		consts.motionVectorsDilated = sl::Boolean::eFalse;
		consts.motionVectorsJittered = sl::Boolean::eFalse;
		return consts;
	}

}
#endif

namespace
{
	bool IsDLSSMode(Corona::EAntiAliasingMode mode)
	{
		return mode == Corona::EAntiAliasingMode::DLSS_SR || mode == Corona::EAntiAliasingMode::DLSS_RR;
	}

	constexpr std::array<const char*, 16> kGpuPassNames = {
		"Frame Total",
		"GBuffer",
		"RT Shadow",
		"Shadow Denoise",
		"RT Reflection",
		"RT Diffuse GI",
		"Temporal Denoise",
		"Spatial Denoise",
		"Lighting",
		"DLSS RR",
		"DLSS SR",
		"Temporal AA",
		"Path Tracing",
		"Tone Map",
		"Debug",
		"ImGui",
	};
}

template<class BlotType>
std::string convertBlobToString(BlotType* pBlob)
{
	std::vector<char> infoLog(pBlob->GetBufferSize() + 1);
	memcpy(infoLog.data(), pBlob->GetBufferPointer(), pBlob->GetBufferSize());
	infoLog[pBlob->GetBufferSize()] = 0;
	return std::string(infoLog.data());
}

ComPtr<ID3DBlob> compileLibrary(const WCHAR* filename, const WCHAR* targetString)
{
	// Initialize the helper
	gDxcDllHelper.Initialize();
	ComPtr<IDxcCompiler> pCompiler;
	ComPtr<IDxcLibrary> pLibrary;
	gDxcDllHelper.CreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler), &pCompiler);
	gDxcDllHelper.CreateInstance(CLSID_DxcLibrary, __uuidof(IDxcLibrary), &pLibrary);

	// Open and read the file
	std::ifstream shaderFile(filename);
	if (shaderFile.good() == false)
	{
		//msgBox("Can't open file " + wstring_2_string(std::wstring(filename)));
		return nullptr;
	}
	std::stringstream strStream;
	strStream << shaderFile.rdbuf();
	std::string shader = strStream.str();

	// Create blob from the string
	ComPtr<IDxcBlobEncoding> pTextBlob;
	pLibrary->CreateBlobWithEncodingFromPinned((LPBYTE)shader.c_str(), (uint32_t)shader.size(), 0, &pTextBlob);

	// Compile
	ComPtr<IDxcOperationResult> pResult;
	pCompiler->Compile(pTextBlob.Get(), filename, L"", targetString, nullptr, 0, nullptr, 0, nullptr, &pResult);

	// Verify the result
	HRESULT resultCode;
	pResult->GetStatus(&resultCode);
	if (FAILED(resultCode))
	{
		ComPtr<IDxcBlobEncoding> pError;
		pResult->GetErrorBuffer(&pError);
		std::string log = convertBlobToString(pError.Get());
		//msgBox("Compiler error:\n" + log);
		return nullptr;
	}

	ID3DBlob* pBlob;
	pResult->GetResult((IDxcBlob**)&pBlob);
	return ComPtr<ID3DBlob>(pBlob);
}

Corona::Corona(UINT width, UINT height, std::wstring name) :
	DXSample(width, height, name),
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height))
{
	int tmpFlag = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);

	// Turn on leak-checking bit.
	tmpFlag |= _CRTDBG_LEAK_CHECK_DF;
	tmpFlag |= _CRTDBG_ALLOC_MEM_DF;
	//tmpFlag |= _CRTDBG_CHECK_ALWAYS_DF;


	// Turn off CRT block checking bit.
	//tmpFlag &= ~_CRTDBG_CHECK_CRT_DF;

	// Set flag to the new value.
	_CrtSetDbgFlag(tmpFlag);
}

Corona::~Corona()
{

}

void Corona::InitGpuTimingResources()
{
	if (bGpuTimingResourcesInitialized)
		return;

	renderBackend->InitializeGpuTimestampQueries(renderBackend->GetFrameCount() * GpuPassCount * GpuQueriesPerPass);
	GpuTimestampFrequency = renderBackend->GetTimestampFrequency();
	bGpuTimingResourcesInitialized = true;
}

void Corona::BeginGpuTimingFrame()
{
	if (!bGpuTimingResourcesInitialized)
		return;

	GpuPassActiveMaskPerFrame[renderBackend->GetCurrentFrameIndex()].fill(0);
}

void Corona::ResolveGpuTimingFrame()
{
	if (!bGpuTimingResourcesInitialized)
		return;

	const UINT frameIndex = renderBackend->GetCurrentFrameIndex();
	const UINT queryFrameBase = frameIndex * GpuPassCount * GpuQueriesPerPass;
	const auto& activeMask = GpuPassActiveMaskPerFrame[frameIndex];

	for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
	{
		if (!activeMask[passIndex])
			continue;

		const UINT queryIndex = queryFrameBase + passIndex * GpuQueriesPerPass;
		renderBackend->ResolveGpuTimestampRange(queryIndex, GpuQueriesPerPass);
	}
}

void Corona::UpdateGpuTimingReadback()
{
	if (!bGpuTimingResourcesInitialized || GpuTimestampFrequency == 0)
		return;

	const UINT frameIndex = renderBackend->GetCurrentFrameIndex();
	const auto& activeMask = GpuPassActiveMaskPerFrame[frameIndex];
	const UINT queryBase = frameIndex * GpuPassCount * GpuQueriesPerPass;

	for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
	{
		if (!activeMask[passIndex])
			continue;

		const UINT64 startTimestamp = renderBackend->ReadGpuTimestampValue(queryBase + passIndex * GpuQueriesPerPass + 0);
		const UINT64 endTimestamp = renderBackend->ReadGpuTimestampValue(queryBase + passIndex * GpuQueriesPerPass + 1);
		if (endTimestamp <= startTimestamp)
			continue;

		const float durationMs = static_cast<float>(double(endTimestamp - startTimestamp) * 1000.0 / double(GpuTimestampFrequency));
		GpuPassLastTimeMs[passIndex] = durationMs;
		auto& history = GpuPassHistoryMs[passIndex];
		history.push_back(durationMs);
		while (history.size() > GpuTimingAverageFrameCount)
		{
			history.pop_front();
		}

		float sumMs = 0.0f;
		for (float sampleMs : history)
		{
			sumMs += sampleMs;
		}
		GpuPassAverageTimeMs[passIndex] = history.empty() ? 0.0f : (sumMs / static_cast<float>(history.size()));
	}
}

void Corona::BeginGpuPassTiming(EGpuPass pass)
{
	if (!bGpuTimingResourcesInitialized)
		return;

	const UINT frameIndex = renderBackend->GetCurrentFrameIndex();
	const UINT passIndex = static_cast<UINT>(pass);
	GpuPassActiveMaskPerFrame[frameIndex][passIndex] = 1;

	const UINT queryIndex = frameIndex * GpuPassCount * GpuQueriesPerPass + passIndex * GpuQueriesPerPass;
	renderBackend->WriteGpuTimestamp(queryIndex);
}

void Corona::EndGpuPassTiming(EGpuPass pass)
{
	if (!bGpuTimingResourcesInitialized)
		return;

	const UINT frameIndex = renderBackend->GetCurrentFrameIndex();
	const UINT passIndex = static_cast<UINT>(pass);
	const UINT queryIndex = frameIndex * GpuPassCount * GpuQueriesPerPass + passIndex * GpuQueriesPerPass + 1;
	renderBackend->WriteGpuTimestamp(queryIndex);
}

const char* Corona::GetGpuPassName(EGpuPass pass) const
{
	return kGpuPassNames[static_cast<size_t>(pass)];
}

#if WITH_STREAMLINE
void Corona::InitStreamline()
{
	if (bStreamlineInitialized)
		return;

	sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
	sl::Preferences pref{};
	pref.showConsole = true;
	pref.logLevel = sl::LogLevel::eDefault;
	pref.pathToLogsAndData = L".";
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = _countof(features);
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
	pref.renderAPI = sl::RenderAPI::eD3D12;
	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	bStreamlineInitialized = slInit(pref) == sl::Result::eOk;
}

void Corona::ShutdownStreamline()
{
	if (!bStreamlineInitialized)
		return;

	slShutdown();
	bStreamlineInitialized = false;
	StreamlineFrameToken = nullptr;
	StreamlineFrameIndex = 0;
	bStreamlineConstantsSetThisFrame = false;
}

bool Corona::BeginStreamlineFrame()
{
	if (!bStreamlineInitialized)
		return false;

	if (StreamlineFrameToken)
		return true;

	uint32_t frameIndex = FrameCounter;
	StreamlineFrameToken = nullptr;
	if (slGetNewFrameToken(StreamlineFrameToken, &frameIndex) != sl::Result::eOk || StreamlineFrameToken == nullptr)
	{
		StreamlineFrameToken = nullptr;
		return false;
	}

	StreamlineFrameIndex = frameIndex;
	bStreamlineConstantsSetThisFrame = false;
	return true;
}

bool Corona::EnsureStreamlineConstants()
{
	if (!BeginStreamlineFrame())
		return false;

	if (bStreamlineConstantsSetThisFrame)
		return true;

	sl::ViewportHandle vp(0);
	sl::Constants consts = BuildStreamlineConstants(
		UnjitteredProjMat,
		UnjitteredViewProjMat,
		PrevUnjitteredViewProjMat,
		InvViewMat,
		CurrentJitter,
		m_camera.m_lookDirection,
		Near,
		Far,
		Fov,
		static_cast<float>(m_width) / static_cast<float>(m_height),
		bDLSSResetNeeded);
	if (slSetConstants(consts, *StreamlineFrameToken, vp) != sl::Result::eOk)
		return false;

	bStreamlineConstantsSetThisFrame = true;
	return true;
}

bool Corona::DLSSPass()
{
	if (!bDLSSAvailable)
		return false;

	if (!EnsureStreamlineConstants())
		return false;

	sl::DLSSOptions opts{};
	opts.mode = ToSLDLSSMode(DLSSQualityMode);
	opts.outputWidth = m_width;
	opts.outputHeight = m_height;
	opts.colorBuffersHDR = sl::Boolean::eTrue;
	opts.useAutoExposure = sl::Boolean::eFalse;
	slDLSSSetOptions(sl::ViewportHandle(0), opts);

	Texture* outputTarget = ColorBuffers[ColorBufferWriteIndex].get();
	renderBackend->TransitionTexture(outputTarget, EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	sl::ViewportHandle vp(0);
	sl::Extent renderExtent{ 0, 0, GetRenderWidth(), GetRenderHeight() };
	sl::Extent outputExtent{ 0, 0, m_width, m_height };
	sl::Resource colorRes(sl::ResourceType::eTex2d, LightingBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource depthRes(sl::ResourceType::eTex2d, UnjitteredDepthBuffers[ColorBufferWriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource motionRes(sl::ResourceType::eTex2d, VelocityBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource outputRes(sl::ResourceType::eTex2d, outputTarget->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	sl::ResourceTag colorTag(&colorRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag depthTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag motionTag(&motionRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag outputTag(&outputRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent);
	sl::ResourceTag tags[] = {
		colorTag,
		depthTag,
		motionTag,
		outputTag,
	};
	slSetTagForFrame(*StreamlineFrameToken, vp, tags, _countof(tags), renderBackend->GetGraphicsCommandList());

	const sl::BaseStructure* inputs[] = {
		static_cast<const sl::BaseStructure*>(&vp),
		static_cast<const sl::BaseStructure*>(&depthTag),
	};
	const sl::Result evalResult = slEvaluateFeature(sl::kFeatureDLSS, *StreamlineFrameToken, inputs, _countof(inputs), renderBackend->GetGraphicsCommandList());
	bDLSSResetNeeded = false;

	renderBackend->TransitionTexture(outputTarget, EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	if (evalResult == sl::Result::eOk)
	{
		ResolvedColorBufferIndex = ColorBufferWriteIndex;
		bUseLightingBufferFallbackForToneMap = false;
		return true;
	}

	bUseLightingBufferFallbackForToneMap = true;
	return false;
}

bool Corona::DLSSRRPass()
{
	if (!IsDLSSRREnabled())
		return false;

	if (!EnsureStreamlineConstants())
		return false;

	sl::DLSSDOptions opts{};
	opts.mode = ToSLDLSSMode(DLSSQualityMode);
	opts.outputWidth = GetRenderWidth();
	opts.outputHeight = GetRenderHeight();
	opts.colorBuffersHDR = sl::Boolean::eTrue;
	opts.preExposure = 1.0f;
	opts.exposureScale = 1.0f;
	opts.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::eUnpacked;
	opts.worldToCameraView = ToSLMatrix(ViewMat);
	opts.cameraViewToWorld = ToSLMatrix(InvViewMat);
	slDLSSDSetOptions(sl::ViewportHandle(0), opts);

	Texture* outputTarget = LightingBuffer.get();
	if (!outputTarget)
		return false;
	renderBackend->TransitionTexture(outputTarget, EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	sl::ViewportHandle vp(0);
	sl::Extent renderExtent{ 0, 0, GetRenderWidth(), GetRenderHeight() };
	sl::Extent outputExtent{ 0, 0, GetRenderWidth(), GetRenderHeight() };
	sl::Resource colorRes(sl::ResourceType::eTex2d, LightingBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource depthRes(sl::ResourceType::eTex2d, UnjitteredDepthBuffers[ColorBufferWriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource motionRes(sl::ResourceType::eTex2d, VelocityBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource normalRes(sl::ResourceType::eTex2d, NormalBuffers[ColorBufferWriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource roughnessRes(sl::ResourceType::eTex2d, RoughnessMetalicBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource albedoRes(sl::ResourceType::eTex2d, AlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource specularAlbedoRes(sl::ResourceType::eTex2d, SpecularAlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource outputRes(sl::ResourceType::eTex2d, outputTarget->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	sl::ResourceTag colorTag(&colorRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag depthTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag motionTag(&motionRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag normalTag(&normalRes, sl::kBufferTypeNormals, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag roughnessTag(&roughnessRes, sl::kBufferTypeRoughness, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag albedoTag(&albedoRes, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag specularAlbedoTag(&specularAlbedoRes, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag outputTag(&outputRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent);
	sl::ResourceTag tags[] = {
		colorTag,
		depthTag,
		motionTag,
		normalTag,
		roughnessTag,
		albedoTag,
		specularAlbedoTag,
		outputTag,
	};
	slSetTagForFrame(*StreamlineFrameToken, vp, tags, _countof(tags), renderBackend->GetGraphicsCommandList());

	const sl::BaseStructure* inputs[] = {
		static_cast<const sl::BaseStructure*>(&vp),
		static_cast<const sl::BaseStructure*>(&depthTag),
		static_cast<const sl::BaseStructure*>(&normalTag),
		static_cast<const sl::BaseStructure*>(&roughnessTag),
		static_cast<const sl::BaseStructure*>(&albedoTag),
		static_cast<const sl::BaseStructure*>(&specularAlbedoTag),
		static_cast<const sl::BaseStructure*>(&motionTag),
	};
	const sl::Result evalResult = slEvaluateFeature(sl::kFeatureDLSS_RR, *StreamlineFrameToken, inputs, _countof(inputs), renderBackend->GetGraphicsCommandList());

	renderBackend->TransitionTexture(outputTarget, EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	if (evalResult == sl::Result::eOk)
	{
		return true;
	}

	bUseLightingBufferFallbackForToneMap = true;
	return false;
}
#endif

void Corona::ResetTemporalHistoryBuffers()
{
	if (!dx12_rhi)
		return;

	const FLOAT clear4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const FLOAT clear2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	auto ClearTextureUAV = [&](Texture* tex, const FLOAT* clearValue)
	{
		if (!tex)
			return;

		renderBackend->TransitionTexture(tex, EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->ClearTextureUAVFloat(tex, clearValue);
		renderBackend->TransitionTexture(tex, EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	};

	ClearTextureUAV(SpecularGIRaw.get(), clear4);
	ClearTextureUAV(SpecularGITemporal[0].get(), clear4);
	ClearTextureUAV(SpecularGITemporal[1].get(), clear4);
	ClearTextureUAV(SpecularGISpatial[0].get(), clear4);
	ClearTextureUAV(SpecularGISpatial[1].get(), clear4);
	ClearTextureUAV(SpecularGIMoments[0].get(), clear2);
	ClearTextureUAV(SpecularGIMoments[1].get(), clear2);
	ClearTextureUAV(DiffuseGIRawAux.get(), clear4);
	ClearTextureUAV(DiffuseGIRaw.get(), clear4);
	ClearTextureUAV(DiffuseGITemporalAux[0].get(), clear4);
	ClearTextureUAV(DiffuseGITemporalAux[1].get(), clear4);
	ClearTextureUAV(DiffuseGITemporal[0].get(), clear4);
	ClearTextureUAV(DiffuseGITemporal[1].get(), clear4);
	ClearTextureUAV(DiffuseGISpatialAux[0].get(), clear4);
	ClearTextureUAV(DiffuseGISpatialAux[1].get(), clear4);
	ClearTextureUAV(DiffuseGISpatial[0].get(), clear4);
	ClearTextureUAV(DiffuseGISpatial[1].get(), clear4);
}

void Corona::ResetAllAccumulationState(bool forceUpscaleReload)
{
	bPendingUpscaleRefresh = true;
	bForceUpscaleReload = forceUpscaleReload;
	DLSSTransitionFramesRemaining = forceUpscaleReload ? 2u : 0u;
	FrameCounter = 0;
	IndirectAccumulatedFrames = 0;
	ResolvedColorBufferIndex = ColorBufferWriteIndex;
	PrevJitter = glm::vec2(0.0f);
	CurrentJitter = glm::vec2(0.0f);
	bTemporalAAHistoryValid = false;
	bTemporalDenoiserHistoryValid = false;
	bPendingTemporalHistoryClear = true;
	bResetTemporalStateNextUpdate = true;
	bUseLightingBufferFallbackForToneMap = true;
	PrevPathTracingViewMat = glm::mat4x4(0.0f);
	PrevPathTracingLightDir = glm::vec3(0.0f);
	PrevPathTracingLightIntensity = 0.0f;
	PrevSkyColorTop = glm::vec3(0.0f);
	PrevSkyColorBottom = glm::vec3(0.0f);
	PrevSkyIntensity = 0.0f;
#if WITH_STREAMLINE
	bDLSSResetNeeded = true;
#endif
}

void Corona::ReloadRenderResolutionAssets()
{
	if (!dx12_rhi)
		return;

	renderBackend->WaitForGpu();
#if WITH_STREAMLINE
	if (bStreamlineInitialized && (bDLSSAvailable || bDLSSRRAvailable))
	{
		slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle(0));
		slFreeResources(sl::kFeatureDLSS_RR, sl::ViewportHandle(0));
	}
#endif
	renderBackend->ResetDynamicResources();
	LoadAssets();
	ColorBufferWriteIndex = 0;
	ResolvedColorBufferIndex = 0;
	GIBufferWriteIndex = 0;
	DLSSTransitionFramesRemaining = 2u;
	FrameCounter = 0;
	IndirectAccumulatedFrames = 0;
	PrevJitter = glm::vec2(0.0f);
	CurrentJitter = glm::vec2(0.0f);
	bTemporalAAHistoryValid = false;
	bTemporalDenoiserHistoryValid = false;
	bPendingTemporalHistoryClear = true;
	bResetTemporalStateNextUpdate = true;
	PrevViewProjMat = ViewProjMat;
	PrevViewMat = ViewMat;
	PrevUnjitteredViewProjMat = UnjitteredViewProjMat;
	PrevIndirectAccumViewMat = ViewMat;
	PrevIndirectAccumLightDir = glm::normalize(LightDir);
	PrevIndirectAccumLightIntensity = LightIntensity;
	PrevIndirectSkyColorTop = SkyColorTop;
	PrevIndirectSkyColorBottom = SkyColorBottom;
	PrevIndirectSkyIntensity = SkyIntensity;
	bUseLightingBufferFallbackForToneMap = true;
#if WITH_STREAMLINE
	bDLSSResetNeeded = true;
#endif
}

void Corona::RefreshUpscaleSettings(bool reloadAssets)
{
	UINT desiredRenderWidth = m_width;
	UINT desiredRenderHeight = m_height;

#if WITH_STREAMLINE
	if (bDLSSAvailable && AntiAliasingMode == EAntiAliasingMode::DLSS_SR)
	{
		sl::DLSSOptions opts{};
		opts.mode = ToSLDLSSMode(DLSSQualityMode);
		opts.outputWidth = m_width;
		opts.outputHeight = m_height;
		opts.colorBuffersHDR = sl::Boolean::eTrue;
		opts.useAutoExposure = sl::Boolean::eFalse;
		slDLSSSetOptions(sl::ViewportHandle(0), opts);

		sl::DLSSOptimalSettings settings{};
		if (slDLSSGetOptimalSettings(opts, settings) == sl::Result::eOk && settings.optimalRenderWidth > 0 && settings.optimalRenderHeight > 0)
		{
			desiredRenderWidth = settings.optimalRenderWidth;
			desiredRenderHeight = settings.optimalRenderHeight;
		}

		DLSSJitterPhaseCount = static_cast<UINT32>(std::max(1.0f, ceilf(8.0f * static_cast<float>(m_width) / static_cast<float>(desiredRenderWidth))));
		bDLSSResetNeeded = true;
	}
	else if (bDLSSRRAvailable && AntiAliasingMode == EAntiAliasingMode::DLSS_RR)
	{
		sl::DLSSDOptions opts{};
		const glm::mat4 identity(1.0f);
		opts.mode = ToSLDLSSMode(DLSSQualityMode);
		opts.outputWidth = m_width;
		opts.outputHeight = m_height;
		opts.colorBuffersHDR = sl::Boolean::eTrue;
		opts.preExposure = 1.0f;
		opts.exposureScale = 1.0f;
		opts.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::eUnpacked;
		opts.worldToCameraView = ToSLMatrix(identity);
		opts.cameraViewToWorld = ToSLMatrix(identity);
		slDLSSDSetOptions(sl::ViewportHandle(0), opts);

		sl::DLSSDOptimalSettings settings{};
		if (slDLSSDGetOptimalSettings(opts, settings) == sl::Result::eOk && settings.optimalRenderWidth > 0 && settings.optimalRenderHeight > 0)
		{
			desiredRenderWidth = settings.optimalRenderWidth;
			desiredRenderHeight = settings.optimalRenderHeight;
		}

		DLSSJitterPhaseCount = static_cast<UINT32>(std::max(1.0f, ceilf(8.0f * static_cast<float>(m_width) / static_cast<float>(desiredRenderWidth))));
		bDLSSResetNeeded = true;
	}
#endif

	const bool bResolutionChanged = desiredRenderWidth != RenderWidth || desiredRenderHeight != RenderHeight;
	RenderWidth = desiredRenderWidth;
	RenderHeight = desiredRenderHeight;
	if (reloadAssets && (bResolutionChanged || bForceUpscaleReload))
		ReloadRenderResolutionAssets();
	bForceUpscaleReload = false;
}

Texture* Corona::GetCurrentResolveSource() const
{
	if (RenderingMode == ERenderingMode::PATHTRACING)
		return PathTracingAccumBuffer[PathTracingWriteIndex].get();

	if (bUseLightingBufferFallbackForToneMap && LightingBuffer)
		return LightingBuffer.get();

	return ColorBuffers[ResolvedColorBufferIndex].get();
}

void Corona::ParseCommandLineArgs(WCHAR* argv[], int argc)
{
	DXSample::ParseCommandLineArgs(argv, argc);

	auto ToLower = [](std::wstring value)
	{
		for (auto& ch : value)
			ch = towlower(ch);
		return value;
	};

	auto ParseValueArg = [&](const std::wstring& current, const wchar_t* longName, const wchar_t* shortName, int& index) -> std::wstring
	{
		if (current == longName || current == shortName)
		{
			if (index + 1 < argc)
				return ToLower(argv[++index]);
			return L"";
		}

		const std::wstring longPrefix = std::wstring(longName) + L"=";
		const std::wstring shortPrefix = std::wstring(shortName) + L"=";
		if (current.rfind(longPrefix, 0) == 0)
			return ToLower(current.substr(longPrefix.size()));
		if (current.rfind(shortPrefix, 0) == 0)
			return ToLower(current.substr(shortPrefix.size()));

		return L"";
	};

	for (int i = 1; i < argc; ++i)
	{
		const std::wstring arg = ToLower(argv[i]);

		if (arg == L"--auto-dump" || arg == L"-dump")
		{
			bCommandLineAutoDumpOverrideSet = true;
			bCommandLineAutoDumpEnabled = true;
			continue;
		}
		if (arg == L"--user-mode" || arg == L"--manual" || arg == L"-user")
		{
			bCommandLineAutoDumpOverrideSet = true;
			bCommandLineAutoDumpEnabled = false;
			continue;
		}

		std::wstring aaValue = ParseValueArg(arg, L"--aa", L"-aa", i);
		if (!aaValue.empty())
		{
			bCommandLineAAOverrideSet = true;
			if (aaValue == L"off")
				CommandLineSelectedAAMode = EAntiAliasingMode::OFF;
			else if (aaValue == L"taa")
				CommandLineSelectedAAMode = EAntiAliasingMode::TAA;
			else if (aaValue == L"dlss" || aaValue == L"dlss-sr" || aaValue == L"dlss_sr" || aaValue == L"dlss sr" || aaValue == L"sr")
				CommandLineSelectedAAMode = EAntiAliasingMode::DLSS_SR;
			else if (aaValue == L"dlss-rr" || aaValue == L"dlss_rr" || aaValue == L"dlss rr" || aaValue == L"rr")
				CommandLineSelectedAAMode = EAntiAliasingMode::DLSS_RR;
			continue;
		}

		std::wstring renderValue = ParseValueArg(arg, L"--render-mode", L"-render", i);
		if (!renderValue.empty())
		{
			bCommandLineRenderModeOverrideSet = true;
			if (renderValue == L"pt" || renderValue == L"pathtracing" || renderValue == L"path-tracing" || renderValue == L"path_tracing" || renderValue == L"path tracing")
				CommandLineRenderingMode = ERenderingMode::PATHTRACING;
			else
				CommandLineRenderingMode = ERenderingMode::HYBRID;
			continue;
		}
	}
}

void Corona::PromptStartupModeSelection()
{
	if (bStartupModeConfigured)
		return;

	wchar_t envValue[32] = {};
	auto ToLower = [](std::wstring value)
	{
		for (auto& ch : value)
			ch = towlower(ch);
		return value;
	};

	const auto SelectDefaultAAMode = [&]() -> EAntiAliasingMode
	{
#if WITH_STREAMLINE
		if (bDLSSRRAvailable)
			return EAntiAliasingMode::DLSS_RR;
		if (bDLSSAvailable)
			return EAntiAliasingMode::DLSS_SR;
#endif
		return EAntiAliasingMode::TAA;
	};

	StartupSelectedAAMode = SelectDefaultAAMode();
	StartupRenderingMode = ERenderingMode::HYBRID;
	RenderingMode = StartupRenderingMode;
	bAutoAADumpEnabled = false;

	if (bCommandLineAutoDumpOverrideSet)
		bAutoAADumpEnabled = bCommandLineAutoDumpEnabled;

	const DWORD dumpEnvLength = GetEnvironmentVariableW(L"CORONA_AUTO_DUMP", envValue, _countof(envValue));
	if (!bCommandLineAutoDumpOverrideSet && dumpEnvLength > 0)
	{
		const std::wstring dumpMode = ToLower(envValue);
		bAutoAADumpEnabled = (dumpMode == L"1" || dumpMode == L"true" || dumpMode == L"yes" || dumpMode == L"dump");
	}

	std::fill(std::begin(envValue), std::end(envValue), 0);
	if (bCommandLineRenderModeOverrideSet)
	{
		StartupRenderingMode = CommandLineRenderingMode;
	}
	const DWORD renderEnvLength = GetEnvironmentVariableW(L"CORONA_START_RENDER_MODE", envValue, _countof(envValue));
	if (!bCommandLineRenderModeOverrideSet && renderEnvLength > 0)
	{
		const std::wstring renderMode = ToLower(envValue);
		if (renderMode == L"pt" || renderMode == L"pathtracing" || renderMode == L"path_tracing" || renderMode == L"path tracing")
		{
			StartupRenderingMode = ERenderingMode::PATHTRACING;
		}
		else
		{
			StartupRenderingMode = ERenderingMode::HYBRID;
		}
	}

	std::fill(std::begin(envValue), std::end(envValue), 0);
	const DWORD aaEnvLength = GetEnvironmentVariableW(L"CORONA_START_AA", envValue, _countof(envValue));
	if (bCommandLineAAOverrideSet)
	{
		StartupSelectedAAMode = CommandLineSelectedAAMode;
	}
	if (!bCommandLineAAOverrideSet && aaEnvLength > 0)
	{
		const std::wstring aaMode = ToLower(envValue);

		if (aaMode == L"off")
		{
			StartupSelectedAAMode = EAntiAliasingMode::OFF;
		}
		else if (aaMode == L"taa")
		{
			StartupSelectedAAMode = EAntiAliasingMode::TAA;
		}
		else if (aaMode == L"dlss" || aaMode == L"dlss_sr" || aaMode == L"dlss sr")
		{
#if WITH_STREAMLINE
			StartupSelectedAAMode = SelectDefaultAAMode();
#else
			StartupSelectedAAMode = EAntiAliasingMode::TAA;
#endif
		}
		else if (aaMode == L"rr" || aaMode == L"dlssrr" || aaMode == L"dlss_rr" || aaMode == L"dlss rr")
		{
#if WITH_STREAMLINE
			if (bDLSSRRAvailable)
				StartupSelectedAAMode = EAntiAliasingMode::DLSS_RR;
			else if (bDLSSAvailable)
				StartupSelectedAAMode = EAntiAliasingMode::DLSS_SR;
			else
				StartupSelectedAAMode = EAntiAliasingMode::TAA;
#else
			StartupSelectedAAMode = EAntiAliasingMode::TAA;
#endif
		}
	}

#if WITH_STREAMLINE
	if (StartupSelectedAAMode == EAntiAliasingMode::DLSS_SR && !bDLSSAvailable)
		StartupSelectedAAMode = EAntiAliasingMode::TAA;
	if (StartupSelectedAAMode == EAntiAliasingMode::DLSS_RR)
	{
		if (bDLSSRRAvailable)
		{
		}
		else if (bDLSSAvailable)
		{
			StartupSelectedAAMode = EAntiAliasingMode::DLSS_SR;
		}
		else
		{
			StartupSelectedAAMode = EAntiAliasingMode::TAA;
		}
	}
#else
	if (StartupSelectedAAMode == EAntiAliasingMode::DLSS_SR || StartupSelectedAAMode == EAntiAliasingMode::DLSS_RR)
		StartupSelectedAAMode = EAntiAliasingMode::TAA;
#endif

	AntiAliasingMode = StartupSelectedAAMode;
	RenderingMode = StartupRenderingMode;
	bStartupModeConfigured = true;
	return;

	const int runModeResult = MessageBoxW(
		Win32Application::GetHwnd(),
		L"Startup mode:\n\nYes = Automatic dump mode\nNo = Manual inspection mode",
		L"Corona Startup Mode",
		MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);

	bAutoAADumpEnabled = (runModeResult == IDYES);

	std::wstring aaMessage =
		L"Initial anti-aliasing mode:\n\n"
		L"Yes = Off\n"
		L"No = TAA\n"
		L"Cancel = DLSS RR";

#if WITH_STREAMLINE
	if (!bDLSSRRAvailable)
	{
		aaMessage += L"\n\nDLSS RR is currently unavailable, so Cancel will fall back to the best available mode.";
	}
#else
	aaMessage += L"\n\nDLSS RR is unavailable in this build, so Cancel will fall back to TAA.";
#endif

	const int aaModeResult = MessageBoxW(
		Win32Application::GetHwnd(),
		aaMessage.c_str(),
		L"Corona Initial AA Mode",
		MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON2);

	if (aaModeResult == IDYES)
		StartupSelectedAAMode = EAntiAliasingMode::OFF;
	else if (aaModeResult == IDNO)
		StartupSelectedAAMode = EAntiAliasingMode::TAA;
	else
	{
#if WITH_STREAMLINE
		StartupSelectedAAMode = SelectDefaultAAMode();
#else
		StartupSelectedAAMode = EAntiAliasingMode::TAA;
#endif
	}

	AntiAliasingMode = StartupSelectedAAMode;
	bStartupModeConfigured = true;
}

void Corona::InitializeAutoAADump()
{
	if (this->bAutoAADumpInitialized || !this->bAutoAADumpEnabled)
		return;

	if (StartupSelectedAAMode != EAntiAliasingMode::DLSS_RR)
	{
		bAutoAADumpEnabled = false;
		return;
	}

	std::filesystem::path dumpDir = std::filesystem::path(L"C:\\dev\\Corona\\dumps\\aa_modes");
	std::filesystem::create_directories(dumpDir);
	this->AutoAADumpDir = dumpDir.wstring();
	this->bAutoAADumpInitialized = true;
	this->bAutoAADumpCompleted = false;
	this->AutoAADumpPhase = 0;
	this->AutoAADumpFramesInPhase = 0;

	std::filesystem::path logPath = std::filesystem::path(AutoAADumpDir) / L"dump_log.txt";
	std::error_code ec;
	std::filesystem::remove(logPath, ec);

	StartupRenderingMode = ERenderingMode::HYBRID;
	RenderingMode = StartupRenderingMode;
	AntiAliasingMode = StartupSelectedAAMode;
	PathTracingViewParam.SamplesPerPixel = 1;
	PathTracingViewParam.MaxBounces = 4;
	ResetAllAccumulationState(false);
}

void Corona::AppendAutoAADumpLog(const std::wstring& line)
{
	if (AutoAADumpDir.empty())
		return;

	std::wofstream logFile(std::filesystem::path(AutoAADumpDir) / L"dump_log.txt", std::ios::app);
	if (!logFile.is_open())
		return;

	logFile << line << L"\n";
}

bool Corona::DumpTextureHDR(Texture* source, const std::wstring& filePath, D3D12_RESOURCE_STATES beforeState)
{
	if (!source || !renderBackend)
		return false;

	ScratchImage captured;
	HRESULT hr = renderBackend->CaptureTexture(source, captured, beforeState);
	if (FAILED(hr))
	{
		AppendAutoAADumpLog(L"[capture] hdr failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
		return false;
	}

	const Image* image = captured.GetImage(0, 0, 0);
	if (!image)
		return false;

	ScratchImage converted;
	hr = Convert(*image, DXGI_FORMAT_R32G32B32A32_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
	if (FAILED(hr))
	{
		AppendAutoAADumpLog(L"[capture] hdr convert failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
		return false;
	}

	const Image* convertedImage = converted.GetImage(0, 0, 0);
	if (!convertedImage)
		return false;

	hr = SaveToHDRFile(*convertedImage, filePath.c_str());
	if (FAILED(hr))
	{
		AppendAutoAADumpLog(L"[capture] hdr save failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
	}
	return SUCCEEDED(hr);
}

bool Corona::DumpTexturePNG(Texture* source, const std::wstring& filePath, D3D12_RESOURCE_STATES beforeState)
{
	if (!source || !renderBackend)
		return false;

	ScratchImage captured;
	HRESULT hr = renderBackend->CaptureTexture(source, captured, beforeState);
	if (FAILED(hr))
	{
		AppendAutoAADumpLog(L"[capture] png failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
		return false;
	}

	const Image* image = captured.GetImage(0, 0, 0);
	if (!image)
		return false;

	const bool bCanSaveWithoutConvert =
		image->format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		image->format == DXGI_FORMAT_B8G8R8A8_UNORM ||
		image->format == DXGI_FORMAT_B8G8R8X8_UNORM;
	if (bCanSaveWithoutConvert)
	{
		hr = SaveToWICFile(*image, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, filePath.c_str());
		if (FAILED(hr))
		{
			AppendAutoAADumpLog(L"[capture] png direct save failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
		}
		return SUCCEEDED(hr);
	}

	ScratchImage converted;
	hr = Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
	if (FAILED(hr))
	{
		AppendAutoAADumpLog(
			L"[capture] png convert srcFormat=" + std::to_wstring(static_cast<unsigned int>(image->format)) +
			L", width=" + std::to_wstring(image->width) +
			L", height=" + std::to_wstring(image->height));
		AppendAutoAADumpLog(L"[capture] png convert failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
		return false;
	}

	const Image* convertedImage = converted.GetImage(0, 0, 0);
	if (!convertedImage)
		return false;

	hr = SaveToWICFile(*convertedImage, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, filePath.c_str());
	if (FAILED(hr))
	{
		AppendAutoAADumpLog(L"[capture] png save failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
	}
	return SUCCEEDED(hr);
}

void Corona::AdvanceAutoAADump(Texture* backbuffer)
{
	if (!bAutoAADumpEnabled || !bAutoAADumpInitialized || bAutoAADumpCompleted)
		return;

	if (StartupSelectedAAMode != EAntiAliasingMode::DLSS_RR)
	{
		bAutoAADumpCompleted = true;
		return;
	}

	constexpr UINT32 kHybridDumpFrames = 60;
	constexpr UINT32 kPathTracingDumpFrames = 180;
	constexpr UINT32 kNumDumpPhases = 3;

	if (AutoAADumpPhase >= kNumDumpPhases)
		return;

	const bool bHybridOnPhase = AutoAADumpPhase == 0;
	const bool bHybridOffPhase = AutoAADumpPhase == 1;
	const bool bPathTracingPhase = AutoAADumpPhase == 2;
	const bool bHybridPhase = bHybridOnPhase || bHybridOffPhase;
	const UINT32 targetFrameCount = bHybridPhase ? kHybridDumpFrames : kPathTracingDumpFrames;
	const ERenderingMode targetRenderingMode = bHybridPhase ? ERenderingMode::HYBRID : ERenderingMode::PATHTRACING;
	const EAntiAliasingMode targetAAMode = bHybridPhase ? StartupSelectedAAMode : EAntiAliasingMode::OFF;
	const bool targetEnableDiffuseGI = !bHybridOffPhase;
	const wchar_t* currentPhaseName =
		bHybridOnPhase ? L"hybrid_diffuse_on" :
		(bHybridOffPhase ? L"hybrid_diffuse_off" : L"path_tracing");

	if (RenderingMode != targetRenderingMode || AntiAliasingMode != targetAAMode || bEnableDiffuseGI != targetEnableDiffuseGI)
	{
		RenderingMode = targetRenderingMode;
		AntiAliasingMode = targetAAMode;
		bEnableDiffuseGI = targetEnableDiffuseGI;
		ResetAllAccumulationState(bHybridPhase);
		PathTracingViewParam.SamplesPerPixel = 1;
		PathTracingViewParam.MaxBounces = 4;
		AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] begin");
		return;
	}

	if (AutoAADumpFramesInPhase == targetFrameCount - 1)
	{
		Texture* resolveTarget = GetCurrentResolveSource();
		Texture* resolvedColorBuffer = (RenderingMode == ERenderingMode::HYBRID && ColorBuffers[ResolvedColorBufferIndex]) ? ColorBuffers[ResolvedColorBufferIndex].get() : nullptr;
		const std::wstring base = AutoAADumpDir + L"\\" + currentPhaseName;
		auto dumpResource = [&](const wchar_t* suffix, Texture* texture, D3D12_RESOURCE_STATES state, bool dumpHdr)
		{
			if (!texture)
				return;

			const std::wstring fileBase = base + L"_" + suffix;
			bool hdrResult = true;
			if (dumpHdr)
				hdrResult = DumpTextureHDR(texture, fileBase + L".hdr", state);
			const bool pngResult = DumpTexturePNG(texture, fileBase + L"_preview.png", state);
			AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] " + suffix +
				L" hdr=" + (dumpHdr ? (hdrResult ? L"ok" : L"fail") : L"skip") +
				L", png=" + (pngResult ? L"ok" : L"fail"));
		};

		const bool hdrOk = DumpTextureHDR(resolveTarget, base + L".hdr", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		const bool resolvePngOk = DumpTexturePNG(resolveTarget, base + L"_resolve_preview.png", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] frameCounter=" + std::to_wstring(FrameCounter) +
			L", taaHistory=" + (bTemporalAAHistoryValid ? L"1" : L"0") +
			L", fallback=" + (bUseLightingBufferFallbackForToneMap ? L"1" : L"0") +
			L", diffuseGI=" + (bEnableDiffuseGI ? L"1" : L"0") +
			L", specularGI=" + (bEnableSpecularGI ? L"1" : L"0") +
			L", resolvedIndex=" + std::to_wstring(ResolvedColorBufferIndex) +
			L", renderingMode=" + std::wstring(RenderingMode == ERenderingMode::PATHTRACING ? L"pt" : L"hybrid"));
		AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] resolve hdr=" + (hdrOk ? L"ok" : L"fail") + L", resolve png=" + (resolvePngOk ? L"ok" : L"fail"));
		if (resolvedColorBuffer)
		{
			const bool resolvedBufferHdrOk = DumpTextureHDR(resolvedColorBuffer, base + L"_colorbuffer.hdr", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			const bool resolvedBufferPngOk = DumpTexturePNG(resolvedColorBuffer, base + L"_colorbuffer_preview.png", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] colorbuffer hdr=" + (resolvedBufferHdrOk ? L"ok" : L"fail") + L", colorbuffer png=" + (resolvedBufferPngOk ? L"ok" : L"fail"));
		}
		if (LightingBuffer)
		{
			const bool lightingHdrOk = DumpTextureHDR(LightingBuffer.get(), base + L"_lighting.hdr", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			const bool lightingPngOk = DumpTexturePNG(LightingBuffer.get(), base + L"_lighting_preview.png", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] lighting hdr=" + (lightingHdrOk ? L"ok" : L"fail") + L", lighting png=" + (lightingPngOk ? L"ok" : L"fail"));
		}

		if (RenderingMode == ERenderingMode::HYBRID)
		{
			dumpResource(L"gbuffer_world_normal", NormalBuffers[ColorBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
			dumpResource(L"gbuffer_velocity", VelocityBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
			dumpResource(L"gbuffer_depth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
			dumpResource(L"gi_diffuse_raw", DiffuseGIRaw.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
			dumpResource(L"gi_diffuse_spatial", DiffuseGISpatial[0].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
			dumpResource(L"gi_diffuse_temporal", DiffuseGITemporal[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
			dumpResource(L"gi_specular_temporal", SpecularGITemporal[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
		}

		if (backbuffer)
		{
			const bool screenPngOk = DumpTexturePNG(backbuffer, base + L"_screen_preview.png", D3D12_RESOURCE_STATE_RENDER_TARGET);
			AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] screen png=" + (screenPngOk ? L"ok" : L"fail"));
		}
	}

	++AutoAADumpFramesInPhase;
	if (AutoAADumpFramesInPhase < targetFrameCount)
		return;

	AutoAADumpFramesInPhase = 0;
	++AutoAADumpPhase;

	if (AutoAADumpPhase < kNumDumpPhases)
	{
		RenderingMode = (AutoAADumpPhase < 2) ? ERenderingMode::HYBRID : ERenderingMode::PATHTRACING;
		AntiAliasingMode = (RenderingMode == ERenderingMode::HYBRID) ? StartupSelectedAAMode : EAntiAliasingMode::OFF;
		bEnableDiffuseGI = (AutoAADumpPhase != 1);
		ResetAllAccumulationState(RenderingMode == ERenderingMode::HYBRID);
		return;
	}

	if (AutoAADumpPhase >= kNumDumpPhases)
	{
		bAutoAADumpCompleted = true;
		PostQuitMessage(0);
		return;
	}
}

std::wstring Corona::GetCameraStatePath()
{
	return GetAssetFullPath(L"camera_state.cfg");
}

bool Corona::LoadCameraState()
{
	std::ifstream file{ std::filesystem::path(GetCameraStatePath()) };
	if (!file.is_open())
		return false;

	std::string versionTag;
	std::string positionTag;
	std::string rotationTag;
	std::string lightDirectionTag;
	std::string lightIntensityTag;
	int version = 0;
	glm::vec3 position(0.0f);
	float yaw = 0.0f;
	float pitch = 0.0f;
	glm::vec3 savedLightDir(0.0f);
	float savedLightIntensity = LightIntensity;

	if (!(file >> versionTag >> version))
		return false;
	if (versionTag != "version" || (version != 1 && version != 2))
		return false;
	if (!(file >> positionTag >> position.x >> position.y >> position.z))
		return false;
	if (positionTag != "position")
		return false;
	if (!(file >> rotationTag >> yaw >> pitch))
		return false;
	if (rotationTag != "rotation")
		return false;

	if (version >= 2)
	{
		if (!(file >> lightDirectionTag >> savedLightDir.x >> savedLightDir.y >> savedLightDir.z))
			return false;
		if (lightDirectionTag != "light_direction")
			return false;
		if (!(file >> lightIntensityTag >> savedLightIntensity))
			return false;
		if (lightIntensityTag != "light_intensity")
			return false;
	}

	m_camera.m_initialPosition = position;
	m_camera.m_position = position;
	m_camera.m_yaw = yaw;
	m_camera.m_pitch = glm::clamp(pitch, -glm::quarter_pi<float>(), glm::quarter_pi<float>());
	m_camera.m_keysPressed = {};
	m_camera.m_mouseButtonDown = false;

	const float r = cosf(m_camera.m_pitch);
	m_camera.m_lookDirection.x = r * sinf(m_camera.m_yaw);
	m_camera.m_lookDirection.y = sinf(m_camera.m_pitch);
	m_camera.m_lookDirection.z = r * cosf(m_camera.m_yaw);

	if (version >= 2)
	{
		const float lightDirLength = glm::length(savedLightDir);
		if (lightDirLength > 0.0f)
			LightDir = savedLightDir / lightDirLength;
		LightIntensity = savedLightIntensity;
	}
	return true;
}

void Corona::SaveCameraState()
{
	std::ofstream file{ std::filesystem::path(GetCameraStatePath()), std::ios::trunc };
	if (!file.is_open())
		return;

	file << std::fixed << std::setprecision(9);
	file << "version 2\n";
	file << "position " << m_camera.m_position.x << ' ' << m_camera.m_position.y << ' ' << m_camera.m_position.z << '\n';
	file << "rotation " << m_camera.m_yaw << ' ' << m_camera.m_pitch << '\n';
	file << "light_direction " << LightDir.x << ' ' << LightDir.y << ' ' << LightDir.z << '\n';
	file << "light_intensity " << LightIntensity << '\n';
}


void Corona::OnInit()
{
	//_CrtSetBreakAlloc(4207117);

	CoInitialize(NULL);

	g_TS.Initialize(8);

	m_camera.Init({ 458, 781, 185 });
	m_camera.SetMoveSpeed(200);
	LoadCameraState();

	RenderWidth = m_width;
	RenderHeight = m_height;
#if WITH_STREAMLINE
	InitStreamline();
#endif
	LoadPipeline();
	PromptStartupModeSelection();
	RefreshUpscaleSettings(false);
	LoadAssets();
	bPendingTemporalHistoryClear = true;
	InitializeAutoAADump();
}

void Corona::LoadPipeline()
{
	UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;

		}

		ComPtr<ID3D12Debug1> spDebugController1;
		debugController->QueryInterface(IID_PPV_ARGS(&spDebugController1));
		//spDebugController1->SetEnableGPUBasedValidation(true);
	}
#endif

	ComPtr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	ComPtr<IDXGIAdapter1> hardwareAdapter;
	//GetHardwareAdapter(factory.Get(), &hardwareAdapter);
	for (uint32_t i = 0; DXGI_ERROR_NOT_FOUND != factory->EnumAdapters1(i, &hardwareAdapter); i++)
	{
		DXGI_ADAPTER_DESC1 desc;
		hardwareAdapter->GetDesc1(&desc);

		// Skip SW adapters
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

		ThrowIfFailed(D3D12CreateDevice(
			hardwareAdapter.Get(),
			D3D_FEATURE_LEVEL_12_1,
			IID_PPV_ARGS(&m_device)
		));

#if WITH_STREAMLINE
		if (bStreamlineInitialized)
			slSetD3DDevice(m_device.Get());
#endif

		D3D12_FEATURE_DATA_D3D12_OPTIONS5 features5;
		HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &features5, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5));
		if (FAILED(hr) || features5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
		{
			//msgBox("Raytracing is not supported on this device. Make sure your GPU supports DXR (such as Nvidia's Volta or Turing RTX) and you're on the latest drivers. The DXR fallback layer is not supported.");
			ThrowIfFailed(hr);
		}


		/*ComPtr<IDXGIAdapter3> pDXGIAdapter3;
		hardwareAdapter->QueryInterface(IID_PPV_ARGS(&pDXGIAdapter3));

		ThrowIfFailed(pDXGIAdapter3->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, 2213100441));


		DXGI_QUERY_VIDEO_MEMORY_INFO LocalVideoMemoryInfo;
		ThrowIfFailed(pDXGIAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &LocalVideoMemoryInfo));*/

		// break on error

		ComPtr<ID3D12InfoQueue> d3dInfoQueue;
		if (SUCCEEDED(m_device->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&d3dInfoQueue)))
		{
			d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
			d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);

			//D3D12_MESSAGE_ID blockedIds[] = {
			//	/*	D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
			//		D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE, */
			//		D3D12_MESSAGE_ID_COPY_DESCRIPTORS_INVALID_RANGES
			//};
			//D3D12_INFO_QUEUE_FILTER filter = {};
			//filter.DenyList.pIDList = blockedIds;
			//filter.DenyList.NumIDs = 1;
			//d3dInfoQueue->AddRetrievalFilterEntries(&filter);
			//d3dInfoQueue->AddStorageFilterEntries(&filter);
		}
		break;
	}

#if WITH_STREAMLINE
	if (bStreamlineInitialized && hardwareAdapter)
	{
		DXGI_ADAPTER_DESC1 desc;
		hardwareAdapter->GetDesc1(&desc);
		sl::AdapterInfo adapterInfo{};
		adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
		adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);
		bDLSSAvailable = slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo) == sl::Result::eOk;
		bDLSSRRAvailable = slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo) == sl::Result::eOk;
	}
#endif

	renderBackend = CreateRenderBackend(ERenderBackendAPI::D3D12, m_device);
	dx12_rhi = renderBackend ? renderBackend->AsSimpleDX12() : nullptr;
	if (!dx12_rhi)
	{
		throw std::runtime_error("Failed to create D3D12 render backend.");
	}

	renderBackend->CreateSwapChainForWindow(
		factory.Get(),
		Win32Application::GetHwnd(),
		m_width,
		m_height,
		DXGI_FORMAT_R8G8B8A8_UNORM);
}

void Corona::LoadAssets()
{
	if (!bBlueNoiseInitialized)
	{
		InitBlueNoiseTexture();
		bBlueNoiseInitialized = true;
	}
	if (!bImguiInitialized)
	{
		InitImgui();
		bImguiInitialized = true;
	}
	InitGBufferPass();
	InitToneMapPass();
	InitDebugPass();
	InitLightingPass();
	InitShadowDenoisePass();
	InitTemporalAAPass();
	InitSpatialDenoisingPass();
	InitTemporalDenoisingPass();
	InitBloomPass();
	//InitGenMipSpecularGIPass();

	InitRTPSO();
	InitPathTracingPass();
	InitGpuTimingResources();

	const UINT DisplayWidth = m_width;
	const UINT DisplayHeight = m_height;
	const UINT RenderWidthLocal = GetRenderWidth();
	const UINT RenderHeightLocal = GetRenderHeight();

	if (framebuffers.empty())
	{
		for (UINT i = 0; i < renderBackend->GetFrameCount(); i++)
		{
			ComPtr<ID3D12Resource> rendertarget = renderBackend->GetSwapChainBuffer(i);
			shared_ptr<Texture> rt = renderBackend->WrapNativeTexture(rendertarget);
			rt->MakeRTV();
			framebuffers.push_back(rt);
		}
	}

	auto createTexture2D = [&](ETextureFormat format, ETextureUsageFlags usage, int width, int height, int mipLevels, std::optional<glm::vec4> clearColor = std::nullopt, EInitialResourceState initialState = EInitialResourceState::ShaderRead)
	{
		TextureCreateDesc desc = {};
		desc.Format = format;
		desc.Usage = usage;
		desc.InitialState = initialState;
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = mipLevels;
		desc.ClearColor = clearColor;
		return renderBackend->CreateTexture2D(desc);
	};

	auto createBuffer = [&](uint32_t numElements, uint32_t elementSize, bool allowUAV, void* initialData = nullptr, EInitialResourceState initialState = EInitialResourceState::ShaderRead)
	{
		BufferCreateDesc desc = {};
		desc.NumElements = numElements;
		desc.ElementSize = elementSize;
		desc.InitialState = initialState;
		desc.bAllowUnorderedAccess = allowUAV;
		desc.InitialData = initialData;
		return renderBackend->CreateBuffer(desc);
	};

	// TAA pingping buffer
	const bool bRecreateColorBuffers =
		!ColorBuffers[0] ||
		ColorBuffers[0]->textureDesc.Width != DisplayWidth ||
		ColorBuffers[0]->textureDesc.Height != DisplayHeight;
	if (bRecreateColorBuffers)
	{
		ColorBuffers[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1);
		ColorBuffers[0]->MakeRTV();

		NAME_D3D12_OBJECT(ColorBuffers[0]->resource);

		ColorBuffers[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1);
		ColorBuffers[1]->MakeRTV();

		NAME_D3D12_OBJECT(ColorBuffers[1]->resource);
	}

	// Path tracing accumulation buffers
	PathTracingAccumBuffer[0] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1, glm::vec4(0.0f));
	
	NAME_D3D12_OBJECT(PathTracingAccumBuffer[0]->resource);

	PathTracingAccumBuffer[1] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1, glm::vec4(0.0f));
	
	NAME_D3D12_OBJECT(PathTracingAccumBuffer[1]->resource);

	// lighting result
	LightingBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	LightingBuffer->MakeRTV();

	NAME_D3D12_OBJECT(LightingBuffer->resource);

	// world normal
	NormalBuffers[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	NormalBuffers[0]->MakeRTV();

	NAME_D3D12_OBJECT(NormalBuffers[0]->resource);

	NormalBuffers[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	NormalBuffers[1]->MakeRTV();

	NAME_D3D12_OBJECT(NormalBuffers[1]->resource);

	// geometry world normal
	GeomNormalBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	GeomNormalBuffer->MakeRTV();

	NAME_D3D12_OBJECT(GeomNormalBuffer->resource);

	// shadow result
	ShadowBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(ShadowBuffer->resource);

	ShadowDenoisedBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(ShadowDenoisedBuffer->resource);

	// refleciton result
	SpecularGIRaw = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(SpecularGIRaw->resource);

	SpecularGITemporal[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(SpecularGITemporal[0]->resource);

	SpecularGITemporal[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(SpecularGITemporal[1]->resource);

	// moments
	SpecularGIMoments[0] = createTexture2D(ETextureFormat::RG16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(SpecularGIMoments[0]->resource);

	SpecularGIMoments[1] = createTexture2D(ETextureFormat::RG16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(SpecularGIMoments[1]->resource);
	// diffuse gi

	DiffuseGIRawAux = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGIRawAux->resource);

	DiffuseGIRaw = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGIRaw->resource);

	// gi result sh
	DiffuseGITemporalAux[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGITemporalAux[0]->resource);

	DiffuseGITemporalAux[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGITemporalAux[1]->resource);

	// gi result color
	DiffuseGITemporal[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGITemporal[0]->resource);

	DiffuseGITemporal[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGITemporal[1]->resource);

	// albedo
	AlbedoBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_RenderTarget, RenderWidthLocal, RenderHeightLocal, 1);
	AlbedoBuffer->MakeRTV();

	NAME_D3D12_OBJECT(AlbedoBuffer->resource);

	SpecularAlbedoBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_RenderTarget, RenderWidthLocal, RenderHeightLocal, 1);
	SpecularAlbedoBuffer->MakeRTV();

	NAME_D3D12_OBJECT(SpecularAlbedoBuffer->resource);

	// velocity
	VelocityBuffer = createTexture2D(ETextureFormat::RG16Float, TextureUsage_RenderTarget, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f));
	VelocityBuffer->MakeRTV();
	NAME_D3D12_OBJECT(VelocityBuffer->resource);

	// pbr material
	RoughnessMetalicBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_RenderTarget, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.001f, 0.0f, 0.0f, 0.0f));
	RoughnessMetalicBuffer->MakeRTV();

	NAME_D3D12_OBJECT(RoughnessMetalicBuffer->resource);

	// depth 
	DepthBuffer = createTexture2D(ETextureFormat::D32Float, TextureUsage_DepthStencil, RenderWidthLocal, RenderHeightLocal, 1);
	DepthBuffer->MakeDSV();
	NAME_D3D12_OBJECT(DepthBuffer->resource);

	UnjitteredDepthBuffers[0] = createTexture2D(ETextureFormat::R32Float, TextureUsage_RenderTarget, RenderWidthLocal, RenderHeightLocal, 1);
	UnjitteredDepthBuffers[0]->MakeRTV();
	NAME_D3D12_OBJECT(UnjitteredDepthBuffers[0]->resource);

	UnjitteredDepthBuffers[1] = createTexture2D(ETextureFormat::R32Float, TextureUsage_RenderTarget, RenderWidthLocal, RenderHeightLocal, 1);
	UnjitteredDepthBuffers[1]->MakeRTV();
	NAME_D3D12_OBJECT(UnjitteredDepthBuffers[1]->resource);

	if (!DefaultWhiteTex) DefaultWhiteTex = renderBackend->CreateTextureFromFile(L"assets/default/default_white.png", false);
	if (!DefaultBlackTex) DefaultBlackTex = renderBackend->CreateTextureFromFile(L"assets/default/default_black.png", false);
	if (!DefaultNormalTex) DefaultNormalTex = renderBackend->CreateTextureFromFile(L"assets/default/default_normal.png", true);
	if (!DefaultRougnessTex) DefaultRougnessTex = renderBackend->CreateTextureFromFile(L"assets/default/default_roughness.png", true);

	if (!Sponza) Sponza = LoadModel("assets/Sponza/Sponza.fbx");

	//  ShaderBall = LoadModel("assets/shaderball/shaderBall.fbx");

	// glm::mat4x4 scaleMat = glm::scale(glm::vec3(2.5, 2.5, 2.5));
	// glm::mat4x4 translatemat = glm::translate(glm::vec3(-150, 20, 0));
	// ShaderBall->SetTransform(scaleMat* translatemat );
	
	//Buddha = LoadModel("buddha/buddha.obj");

	/*glm::mat4x4 buddhaTM = glm::scale(vec3(100, 100, 100));
	Buddha->SetTransform(buddhaTM);*/

	// Describe and create a sampler.
	if (!samplerWrap)
	{
		SamplerCreateDesc samplerDesc = {};
		samplerDesc.Filter = ESamplerFilter::Anisotropic;
		samplerDesc.AddressU = ESamplerAddressMode::Wrap;
		samplerDesc.AddressV = ESamplerAddressMode::Wrap;
		samplerDesc.AddressW = ESamplerAddressMode::Wrap;
		samplerDesc.MinLOD = 0.0f;
		samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
		samplerDesc.MipLODBias = -1.0f;
		samplerDesc.MaxAnisotropy = 1;

		samplerWrap = renderBackend->CreateSampler(samplerDesc);

	}

	if (!samplerBilinearWrap)
	{
		SamplerCreateDesc samplerDesc = {};
		samplerDesc.Filter = ESamplerFilter::Linear;
		samplerDesc.AddressU = ESamplerAddressMode::Clamp;
		samplerDesc.AddressV = ESamplerAddressMode::Clamp;
		samplerDesc.AddressW = ESamplerAddressMode::Clamp;
		samplerDesc.MinLOD = 0.0f;
		samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
		samplerDesc.MipLODBias = -1.0f;
		samplerDesc.MaxAnisotropy = 1;

		samplerBilinearWrap = renderBackend->CreateSampler(samplerDesc);

	}
	InitRaytracingData();

}



shared_ptr<Scene> Corona::LoadModel(string fileName)
{
	map<wstring, wstring> SponzaRoughnessMap = {
	{L"Background_Albedo", L"Background_Roughness"},
	{L"ChainTexture_Albedo", L"ChainTexture_Roughness"},
	{L"Lion_Albedo", L"Lion_Roughness"},
	{L"Sponza_Arch_diffuse", L"Sponza_Arch_roughness"},
	{L"Sponza_Bricks_a_Albedo", L"Sponza_Bricks_a_Roughness"},
	{L"Sponza_Ceiling_diffuse", L"Sponza_Ceiling_roughness"},
	{L"Sponza_Column_a_diffuse", L"Sponza_Column_a_roughness"},
	{L"Sponza_Column_b_diffuse", L"Sponza_Column_b_roughness"},
	{L"Sponza_Column_c_diffuse", L"Sponza_Column_c_roughness"},
	{L"Sponza_Curtain_Blue_diffuse", L"Sponza_Curtain_roughness"},
	{L"Sponza_Curtain_Green_diffuse", L"Sponza_Curtain_roughness"},
	{L"Sponza_Curtain_Red_diffuse", L"Sponza_Curtain_roughness"},
	{L"Sponza_Details_diffuse", L"Sponza_Details_roughness"},
	{L"Sponza_Fabric_Blue_diffuse", L"Sponza_Fabric_roughness"},
	{L"Sponza_Fabric_Green_diffuse", L"Sponza_Fabric_roughness"},
	{L"Sponza_Fabric_Red_diffuse", L"Sponza_Fabric_roughness"},
	{L"Sponza_FlagPole_diffuse", L"Sponza_FlagPole_roughness"},
	{L"Sponza_Floor_diffuse", L"Sponza_Floor_roughness"},
	{L"Sponza_Roof_diffuse", L"Sponza_Roof_roughness"},
	{L"Sponza_Thorn_diffuse", L"Sponza_Thorn_roughness"},
	{L"Vase_diffuse", L"Vase_roughness"},
	{L"VaseHanging_diffuse", L"VaseHanging_roughness"},
	{L"VasePlant_diffuse", L"VasePlant_roughness"},
	{L"VaseRound_diffuse", L"VaseRound_roughness"}
	};

	Scene* scene = new Scene;

	Assimp::Importer importer;
	const aiScene* assimpScene = importer.ReadFile(fileName, 0);
	
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
	wstring wide = converter.from_bytes(fileName);

	wstring dir = GetDirectoryFromFilePath(wide.c_str());
	//wstring dir = L"Sponza/";

	UINT flags = aiProcess_CalcTangentSpace |
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_MakeLeftHanded |
		aiProcess_RemoveRedundantMaterials |
		aiProcess_FlipUVs |
		aiProcess_FlipWindingOrder;

		flags |= aiProcess_PreTransformVertices /*| aiProcess_OptimizeMeshes*/;

	assimpScene = importer.ApplyPostProcessing(flags);

	const int numMaterials = assimpScene->mNumMaterials;
	scene->Materials.reserve(numMaterials);
	for (int i = 0; i < numMaterials; ++i)
	{
		const aiMaterial& aiMat = *assimpScene->mMaterials[i];
		//shared_ptr<Material> mat = shared_ptr<Material>(new Material);
		Material* mat = new Material;
		wstring wDiffuseTex;
		wstring wNormalTex;
		wstring wRoughnessTex;
		wstring wMetallicTex;


		aiString diffuseTexPath;
		aiString normalMapPath;
		aiString rougnessMapPath;
		aiString metallicMapPath;


		if (aiMat.GetTexture(aiTextureType_DIFFUSE, 0, &diffuseTexPath) == aiReturn_SUCCESS)
			wDiffuseTex = GetFileName(AnsiToWString(diffuseTexPath.C_Str()).c_str());
		if (wDiffuseTex.length() != 0)
		{
			mat->Diffuse = renderBackend->CreateTextureFromFile(dir + wDiffuseTex, false);
		}

		if (!mat->Diffuse)
			mat->Diffuse = DefaultWhiteTex;
		
		if (aiMat.GetTexture(aiTextureType_NORMALS, 0, &normalMapPath) == aiReturn_SUCCESS
			|| aiMat.GetTexture(aiTextureType_HEIGHT, 0, &normalMapPath) == aiReturn_SUCCESS)
			wNormalTex = GetFileName(AnsiToWString(normalMapPath.C_Str()).c_str());

		if (wNormalTex.length() != 0)
		{
			mat->Normal = renderBackend->CreateTextureFromFile(dir + wNormalTex, true);
		}

		if (!mat->Normal)
			mat->Normal = DefaultNormalTex;

		
		// aiTextureType_HEIGHT is normal in sponza
		// aiTextureType_AMBIENT is metallic in sponza

		if (aiMat.GetTexture(aiTextureType_AMBIENT, 0, &metallicMapPath) == aiReturn_SUCCESS)
			wMetallicTex = GetFileName(AnsiToWString(metallicMapPath.C_Str()).c_str());
		if (wMetallicTex.length() != 0)
		{
			mat->Metallic = renderBackend->CreateTextureFromFile(dir + wMetallicTex, true);
		}

		if (!mat->Metallic)
			mat->Metallic = DefaultBlackTex;
		
		if (wDiffuseTex.length() != 0)
		{
			wstring wNameStr = wstring(wDiffuseTex.substr(0, wDiffuseTex.length() - 4));
			map<wstring, wstring> ::iterator it = SponzaRoughnessMap.find(wNameStr);
			if (it != SponzaRoughnessMap.end())
			{
				wRoughnessTex = SponzaRoughnessMap[wNameStr] + L".png";
				mat->Roughness = renderBackend->CreateTextureFromFile(dir + wRoughnessTex, true);
			}
		}

		if (!mat->Roughness)
		{
			mat->Roughness = DefaultRougnessTex;
		}

		// HACK!
		if (wDiffuseTex == L"Sponza_Thorn_diffuse.png" || wDiffuseTex == L"VasePlant_diffuse.png" || wDiffuseTex == L"ChainTexture_Albedo.png")
			mat->bHasAlpha = true;

		scene->Materials.push_back(shared_ptr<Material>(mat));
	}

	struct Vertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
		glm::vec3 Tangent;
	};
	const UINT numMeshes = assimpScene->mNumMeshes;

	UINT totalNumVert = 0;
	for (UINT i = 0; i < numMeshes; ++i)
	{
		aiMesh* asMesh = assimpScene->mMeshes[i];

		totalNumVert += asMesh->mNumVertices;
	}

	for (UINT i = 0; i < numMeshes; ++i)
	{
		aiMesh* asMesh = assimpScene->mMeshes[i];

		Mesh* mesh = new Mesh;
		mesh->Owner = dx12_rhi;

		mesh->NumVertices = asMesh->mNumVertices;
		mesh->NumIndices = asMesh->mNumFaces * 3;

		vector<Vertex> vertices;
		vertices.resize(mesh->NumVertices);

		vector<UINT32> indices;
		indices.resize(mesh->NumIndices);
		//if (i > 0) break;

		if (asMesh->HasPositions())
		{
			for (int i = 0; i < mesh->NumVertices; ++i)
			{
				vertices[i].Position.x = asMesh->mVertices[i].x;
				vertices[i].Position.y = asMesh->mVertices[i].y;
				vertices[i].Position.z = asMesh->mVertices[i].z;
			}
		}

		if (asMesh->HasNormals())
		{
			for (int i = 0; i < mesh->NumVertices; ++i)
			{
				vertices[i].Normal.x = asMesh->mNormals[i].x;
				vertices[i].Normal.y = asMesh->mNormals[i].y;
				vertices[i].Normal.z = asMesh->mNormals[i].z;
			}
		}

		if (asMesh->HasTextureCoords(0))
		{
			for (int i = 0; i < mesh->NumVertices; ++i)
			{
				vertices[i].UV.x = asMesh->mTextureCoords[0][i].x;
				vertices[i].UV.y = asMesh->mTextureCoords[0][i].y;
			}
		}

		if (asMesh->HasTangentsAndBitangents())
		{
			for (int i = 0; i < mesh->NumVertices; ++i)
			{
				vertices[i].Tangent.x = asMesh->mTangents[i].x;
				vertices[i].Tangent.y = asMesh->mTangents[i].y;
				vertices[i].Tangent.z = asMesh->mTangents[i].z;
			}
		}

		const UINT numTriangles = asMesh->mNumFaces;
		for (int triIdx = 0; triIdx < numTriangles; ++triIdx)
		{
			indices[triIdx * 3 + 0] = asMesh->mFaces[triIdx].mIndices[0];
			indices[triIdx * 3 + 1] = asMesh->mFaces[triIdx].mIndices[1];
			indices[triIdx * 3 + 2] = asMesh->mFaces[triIdx].mIndices[2];
		}

		mesh->Vb = renderBackend->CreateVertexBuffer(sizeof(Vertex) * mesh->NumVertices, sizeof(Vertex), vertices.data());
		mesh->VertexStride = sizeof(Vertex);
		mesh->IndexFormat = DXGI_FORMAT_R32_UINT;

		mesh->Ib = renderBackend->CreateIndexBuffer(mesh->IndexFormat, sizeof(UINT32)*3*numTriangles, indices.data());


		Mesh::DrawCall dc;
		dc.IndexCount = numTriangles * 3;
		dc.IndexStart = 0;
		dc.VertexBase = 0;
		dc.VertexCount = vertices.size();
		dc.mat = scene->Materials[asMesh->mMaterialIndex];
		if (dc.mat->bHasAlpha) mesh->bTransparent = true;
		
		mesh->Draws.push_back(dc);

		scene->meshes.push_back(shared_ptr<Mesh>(mesh));
	}

	shared_ptr<Scene> scenePtr = shared_ptr<Scene>(scene);

	return scenePtr;
}

void Corona::InitSpatialDenoisingPass()
{
	ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\SpatialDenoising.hlsl"), "SpatialFilter", "cs_5_0");

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

	shared_ptr<PipelineStateObject> TEMP_SpatialDenoisingFilterPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_SpatialDenoisingFilterPSO->Owner = dx12_rhi;
	TEMP_SpatialDenoisingFilterPSO->cs = cs;
	TEMP_SpatialDenoisingFilterPSO->computePSODesc = computePsoDesc;
	TEMP_SpatialDenoisingFilterPSO->BindSRV("DepthTex", 0, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("GeoNormalTex", 1, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("InGIResultSHTex", 2, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("InGIResultColorTex", 3, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("InSpecularGITex", 4, 1);
	
	
	TEMP_SpatialDenoisingFilterPSO->BindUAV("OutGIResultSH", 0);
	TEMP_SpatialDenoisingFilterPSO->BindUAV("OutGIResultColor", 1);
	TEMP_SpatialDenoisingFilterPSO->BindUAV("OutSpecularGI", 2);
	
	
	TEMP_SpatialDenoisingFilterPSO->BindCBV("SpatialFilterConstant", 0, sizeof(SpatialFilterConstant));
	TEMP_SpatialDenoisingFilterPSO->IsCompute = true;
	bool bSuccess = TEMP_SpatialDenoisingFilterPSO->Init();
	if (bSuccess)
		SpatialDenoisingFilterPSO = TEMP_SpatialDenoisingFilterPSO;

	UINT WidthGI = GetRenderWidth();
	UINT HeightGI = GetRenderHeight();

	DiffuseGISpatialAux[0] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatialAux[0]->resource);

	DiffuseGISpatialAux[1] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatialAux[1]->resource);

	DiffuseGISpatial[0] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatial[0]->resource);

	DiffuseGISpatial[1] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatial[1]->resource);

	SpecularGISpatial[0] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(SpecularGISpatial[0]->resource);

	SpecularGISpatial[1] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(SpecularGISpatial[1]->resource);
}

void Corona::InitTemporalDenoisingPass()
{
	ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\TemporalDenoising.hlsl"), "TemporalFilter", "cs_5_0");
	
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	
	shared_ptr<PipelineStateObject> TEMP_TemporalDenoisingFilterPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_TemporalDenoisingFilterPSO->Owner = dx12_rhi;
	TEMP_TemporalDenoisingFilterPSO->cs = cs;
	TEMP_TemporalDenoisingFilterPSO->computePSODesc = computePsoDesc;
	TEMP_TemporalDenoisingFilterPSO->BindSRV("DepthTex", 0, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("NormalTex", 1, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InGIResultSHTex", 2, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InGIResultColorTex", 3, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InGIResultSHTexPrev", 4, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InGIResultColorTexPrev", 5, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("VelocityTex", 6, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InSpecularGITex", 7, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("InSpecularGITexPrev", 8, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("RougnessMetalicTex", 9, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("PrevDepthTex", 10, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("PrevNormalTex", 11, 1);
	TEMP_TemporalDenoisingFilterPSO->BindSRV("PrevMomentsTex", 12, 1);





	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutGIResultSH", 0);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutGIResultColor", 1);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutGIResultSHDS", 2);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutGIResultColorDS", 3);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutSpecularGI", 4);
	TEMP_TemporalDenoisingFilterPSO->BindUAV("OutMoments", 5);

	//TemporalDenoisingFilterPSO->BindUAV("OutSpecularGIDS", 5);

	TEMP_TemporalDenoisingFilterPSO->BindSampler("BilinearClamp", 0);


	TEMP_TemporalDenoisingFilterPSO->BindCBV("TemporalFilterConstant", 0, sizeof(TemporalFilterConstant));
	TEMP_TemporalDenoisingFilterPSO->IsCompute = true;
	bool bSuccess = TEMP_TemporalDenoisingFilterPSO->Init();
	if (bSuccess)
		TemporalDenoisingFilterPSO = TEMP_TemporalDenoisingFilterPSO;
}

void Corona::InitBloomPass()
{
	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\BloomBlur.hlsl"), "BloomExtract", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_BloomExtractPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_BloomExtractPSO->Owner = dx12_rhi;
		TEMP_BloomExtractPSO->cs = cs;
		TEMP_BloomExtractPSO->computePSODesc = computePsoDesc;
		TEMP_BloomExtractPSO->BindSRV("SrcTex", 0, 1);
		TEMP_BloomExtractPSO->BindSRV("Exposure", 1, 1);
		TEMP_BloomExtractPSO->BindUAV("DstTex", 0);
		TEMP_BloomExtractPSO->BindUAV("LumaResult", 1);
		TEMP_BloomExtractPSO->BindSampler("samplerWrap", 0);
		TEMP_BloomExtractPSO->BindCBV("BloomCB", 0, sizeof(BloomCB));
		TEMP_BloomExtractPSO->IsCompute = true;
		bool bSuccess = TEMP_BloomExtractPSO->Init();
		if (bSuccess)
			BloomExtractPSO = TEMP_BloomExtractPSO;
	}
	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\BloomBlur.hlsl"), "BloomBlur", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_BloomBlurPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_BloomBlurPSO->Owner = dx12_rhi;
		TEMP_BloomBlurPSO->cs = cs;
		TEMP_BloomBlurPSO->computePSODesc = computePsoDesc;
		TEMP_BloomBlurPSO->BindSRV("SrcTex", 0, 1);
		TEMP_BloomBlurPSO->BindUAV("DstTex", 0);
		TEMP_BloomBlurPSO->BindSampler("samplerWrap", 0);
		TEMP_BloomBlurPSO->BindCBV("BloomCB", 0, sizeof(BloomCB));
		TEMP_BloomBlurPSO->IsCompute = true;
		bool bSucess = TEMP_BloomBlurPSO->Init();
		if (bSucess)
			BloomBlurPSO = TEMP_BloomBlurPSO;
	}

	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\Histogram.hlsl"), "GenerateHistogram", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_HistogramPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_HistogramPSO->Owner = dx12_rhi;
		TEMP_HistogramPSO->cs = cs;
		TEMP_HistogramPSO->computePSODesc = computePsoDesc;
		TEMP_HistogramPSO->BindSRV("LumaTex", 0, 1);
		TEMP_HistogramPSO->BindUAV("Histogram", 0);
		TEMP_HistogramPSO->IsCompute = true;
		bool bSucess = TEMP_HistogramPSO->Init();
		if (bSucess)
			HistogramPSO = TEMP_HistogramPSO;

	}

	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\DrawHistogram.hlsl"), "DrawHistogram", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_DrawHistogramPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_DrawHistogramPSO->Owner = dx12_rhi;
		TEMP_DrawHistogramPSO->cs = cs;
		TEMP_DrawHistogramPSO->computePSODesc = computePsoDesc;
		TEMP_DrawHistogramPSO->BindSRV("Histogram", 0, 1);
		TEMP_DrawHistogramPSO->BindSRV("Exposure", 1, 1);
		TEMP_DrawHistogramPSO->BindUAV("ColorBuffer", 0);
		TEMP_DrawHistogramPSO->IsCompute = true;
		bool bSuccess = TEMP_DrawHistogramPSO->Init();
		if (bSuccess)
			DrawHistogramPSO = TEMP_DrawHistogramPSO;

	}

	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\Histogram.hlsl"), "ClearHistogram", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_ClearHistogramPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_ClearHistogramPSO->Owner = dx12_rhi;
		TEMP_ClearHistogramPSO->cs = cs;
		TEMP_ClearHistogramPSO->computePSODesc = computePsoDesc;
		TEMP_ClearHistogramPSO->BindUAV("Histogram", 0);
		TEMP_ClearHistogramPSO->IsCompute = true;
		bool bSuccess = TEMP_ClearHistogramPSO->Init();
		if (bSuccess)
			ClearHistogramPSO = TEMP_ClearHistogramPSO;

	}


	{
		ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\AdaptExposureCS.hlsl"), "AdaptExposure", "cs_5_0");
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};

		shared_ptr<PipelineStateObject> TEMP_AdapteExposurePSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
		TEMP_AdapteExposurePSO->Owner = dx12_rhi;
		TEMP_AdapteExposurePSO->cs = cs;
		TEMP_AdapteExposurePSO->computePSODesc = computePsoDesc;
		TEMP_AdapteExposurePSO->BindSRV("Histogram", 0, 1);
		TEMP_AdapteExposurePSO->BindUAV("Exposure", 0);
		TEMP_AdapteExposurePSO->BindUAV("Exposure", 0);
		TEMP_AdapteExposurePSO->BindCBV("AdaptExposureCB", 0, sizeof(AdaptExposureCB));

		TEMP_AdapteExposurePSO->IsCompute = true;
		bool bSucess = TEMP_AdapteExposurePSO->Init();
		if (bSucess)
			AdapteExposurePSO = TEMP_AdapteExposurePSO;

	}

	BloomBlurPingPong[0] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });

	NAME_D3D12_OBJECT(BloomBlurPingPong[0]->resource);

	BloomBlurPingPong[1] = renderBackend->CreateTexture2D({ ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });

	NAME_D3D12_OBJECT(BloomBlurPingPong[1]->resource);


	LumaBuffer = renderBackend->CreateTexture2D({ ETextureFormat::R8Uint, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)BloomBufferWidth, (int)BloomBufferHeight, 1, std::nullopt });

	NAME_D3D12_OBJECT(LumaBuffer->resource);

	Histogram = renderBackend->CreateBuffer({ 256u, sizeof(UINT32), EInitialResourceState::ShaderRead, true, nullptr });
	Histogram->MakeByteAddressBufferSRV();
	NAME_D3D12_OBJECT(Histogram->resource);

	__declspec(align(16)) float initExposure[] =
	{
		Exposure,
		1.0f / Exposure,
		0.01,
		Exposure,
		0.0f,
		kInitialMinLog,
		kInitialMaxLog,
		kInitialMaxLog - kInitialMinLog,
		1.0f / (kInitialMaxLog - kInitialMinLog)
	};

	ExposureData = renderBackend->CreateBuffer({ 8u, sizeof(float), EInitialResourceState::ShaderRead, true, initExposure });
	ExposureData->MakeStructuredBufferSRV();
	NAME_D3D12_OBJECT(ExposureData->resource);

}

void Corona::InitGBufferPass()
{
	ComPtr<ID3DBlob> vs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\GBuffer.hlsl"), "VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\GBuffer.hlsl"), "PSMain", "ps_5_0");

	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
	psoDescMesh.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	//psoDesc.pRootSignature = m_rootSignature.Get();
	/*psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);*/
	psoDescMesh.RasterizerState = rasterizerStateDesc;
	psoDescMesh.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDescMesh.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDescMesh.SampleMask = UINT_MAX;
	psoDescMesh.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescMesh.NumRenderTargets = 7;
	psoDescMesh.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	psoDescMesh.RTVFormats[4] = DXGI_FORMAT_R16G16_FLOAT;
	psoDescMesh.RTVFormats[5] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.RTVFormats[6] = DXGI_FORMAT_R32_FLOAT;

	psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDescMesh.SampleDesc.Count = 1;

	
	shared_ptr<PipelineStateObject> TEMP_GBufferPassPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_GBufferPassPSO->Owner = dx12_rhi;
	TEMP_GBufferPassPSO->ps = ps;
	TEMP_GBufferPassPSO->vs = vs;
	TEMP_GBufferPassPSO->graphicsPSODesc = psoDescMesh;
	TEMP_GBufferPassPSO->BindSRV("AlbedoTex", 0, 1);
	TEMP_GBufferPassPSO->BindSRV("NormalTex", 1, 1);
	TEMP_GBufferPassPSO->BindSRV("RoughnessTex", 2, 1);
	TEMP_GBufferPassPSO->BindSRV("MetallicTex", 3, 1);
	
	TEMP_GBufferPassPSO->BindSampler("samplerWrap", 0);
	TEMP_GBufferPassPSO->BindCBV("GBufferConstantBuffer", 0, sizeof(GBufferConstantBuffer));

	bool bSucess = TEMP_GBufferPassPSO->Init();
	if (bSucess)
		GBufferPassPSO = TEMP_GBufferPassPSO;
}

void Corona::InitImgui()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;

	ImGui_ImplWin32_Init(Win32Application::GetHwnd());
	renderBackend->InitializeImGuiBackend(Win32Application::GetHwnd(), DXGI_FORMAT_R8G8B8A8_UNORM);
}

void Corona::InitBlueNoiseTexture()
{
	string path = "assets/bluenoise/64_64_64/HDR_RGBA.raw";
	ifstream file(path.data(), ios::in | ios::binary);
	if (file.is_open())
	{
		file.seekg(0, file.end);
		int length = file.tellg();
		file.seekg(0, file.beg);

		UINT32 Version;
		file.read(reinterpret_cast<char*>(&Version), sizeof(UINT32));

		UINT32 nChannel;
		file.read(reinterpret_cast<char*>(&nChannel), sizeof(UINT32));

		UINT32 nDimension;
		file.read(reinterpret_cast<char*>(&nDimension), sizeof(UINT32));

		UINT32 Shape[3];
		for(int i=0;i< nDimension;i++)
			file.read(reinterpret_cast<char*>(&Shape[i]), sizeof(UINT32));

		UINT DataSize = sizeof(UINT32) * nChannel * Shape[0] * Shape[1] * Shape[2];
		UINT32* NoiseDataRaw = new UINT32[DataSize];

		file.read(reinterpret_cast<char*>(NoiseDataRaw), DataSize);
		size_t extracted = file.gcount();
		/*UINT32 NoiseData[128];
		file.read(reinterpret_cast<char*>(NoiseData), 128*sizeof(UINT32));*/
		int NumFloat = nChannel * Shape[0] * Shape[1] * Shape[2];

		float* NoiseDataFloat = new float[NumFloat];
		stringstream ss;

		float MaxValue = Shape[0] * Shape[1] * Shape[2];
		for (int i = 0; i < NumFloat; i++)
		{
			if (NoiseDataRaw[i] == 3452816845)
			{
				int a = 0;
			}
			NoiseDataFloat[i] = static_cast<float>(NoiseDataRaw[i]) / MaxValue;

			ss << NoiseDataFloat[i] << " ";
			if(i %(64*4) == 0)
				ss << "\n";
		}

		D3D12_SUBRESOURCE_DATA textureData = {};
		textureData.pData = NoiseDataFloat;
		textureData.RowPitch = Shape[0] * nChannel * sizeof(UINT32);
		textureData.SlicePitch = textureData.RowPitch * Shape[1];

		BlueNoiseTex = renderBackend->CreateTexture3D(ETextureFormat::RGBA32Float, TextureUsage_None, EInitialResourceState::CopyDest, Shape[0], Shape[1], Shape[2], 1);
		BlueNoiseTex->UploadSRCData3D(&textureData);

		delete[] NoiseDataRaw;
		delete[] NoiseDataFloat;
	}
	
	file.close();
}

void Corona::InitToneMapPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },    // Bottom left.
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },    // Top left.
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },    // Bottom right.
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }        // Top right.
	};

	const UINT vertexBufferSize = sizeof(quadVertices);
	const UINT vertexBufferStride = sizeof(PostVertex);

	D3D12_SUBRESOURCE_DATA vertexData = {};
	vertexData.pData = &quadVertices;
	vertexData.RowPitch = vertexBufferSize;
	vertexData.SlicePitch = vertexData.RowPitch;

	FullScreenVB = renderBackend->CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices);
	
	ComPtr<ID3DBlob> vs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl"), "VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl"), "PSMain", "ps_5_0");


	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	psoDesc.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	psoDesc.RasterizerState = rasterizerStateDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	//psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	shared_ptr<PipelineStateObject> TEMP_ToneMapPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_ToneMapPSO->Owner = dx12_rhi;
	TEMP_ToneMapPSO->ps = ps;
	TEMP_ToneMapPSO->vs = vs;
	TEMP_ToneMapPSO->graphicsPSODesc = psoDesc;

	TEMP_ToneMapPSO->BindSRV("SrcTex", 0, 1);
	TEMP_ToneMapPSO->BindSampler("samplerWrap", 0);
	TEMP_ToneMapPSO->BindCBV("ScaleOffsetParams", 0, sizeof(ToneMapCB));

	bool bSuccess = TEMP_ToneMapPSO->Init();
	if (bSuccess)
		ToneMapPSO = TEMP_ToneMapPSO;
}

void Corona::InitDebugPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },    // Bottom left.
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },    // Top left.
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },    // Bottom right.
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }        // Top right.
	};

	const UINT vertexBufferSize = sizeof(quadVertices);
	const UINT vertexBufferStride = sizeof(PostVertex);

	D3D12_SUBRESOURCE_DATA vertexData = {};
	vertexData.pData = &quadVertices;
	vertexData.RowPitch = vertexBufferSize;
	vertexData.SlicePitch = vertexData.RowPitch;

	FullScreenVB = renderBackend->CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices);

	ComPtr<ID3DBlob> vs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\DebugPS.hlsl"), "VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\DebugPS.hlsl"), "PSMain", "ps_5_0");

	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	psoDesc.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	psoDesc.RasterizerState = rasterizerStateDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	//psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	shared_ptr<PipelineStateObject> TEMP_BufferVisualizePSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_BufferVisualizePSO->Owner = dx12_rhi;
	TEMP_BufferVisualizePSO->ps = ps;
	TEMP_BufferVisualizePSO->vs = vs;
	TEMP_BufferVisualizePSO->graphicsPSODesc = psoDesc;

	TEMP_BufferVisualizePSO->BindSRV("SrcTex", 0, 1);
	TEMP_BufferVisualizePSO->BindSRV("SrcTexSH", 1, 1);
	TEMP_BufferVisualizePSO->BindSRV("SrcTexNormal", 2, 1);

	TEMP_BufferVisualizePSO->BindSampler("samplerWrap", 0);
	TEMP_BufferVisualizePSO->BindCBV("DebugPassCB", 0, sizeof(DebugPassCB));

	bool bSuccess = TEMP_BufferVisualizePSO->Init();
	if (bSuccess)
		BufferVisualizePSO = TEMP_BufferVisualizePSO;
}

void Corona::InitLightingPass()
{
	ComPtr<ID3DBlob> vs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\LightingPS.hlsl"), "VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\LightingPS.hlsl"), "PSMain", "ps_5_0");

	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	psoDesc.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	psoDesc.RasterizerState = rasterizerStateDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	//psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	shared_ptr<PipelineStateObject> TEMP_LightingPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_LightingPSO->Owner = dx12_rhi;
	TEMP_LightingPSO->ps = ps;
	TEMP_LightingPSO->vs = vs;
	TEMP_LightingPSO->graphicsPSODesc = psoDesc;
	
	TEMP_LightingPSO->BindSRV("AlbedoTex", 0, 1);
	TEMP_LightingPSO->BindSRV("NormalTex", 1, 1);
	TEMP_LightingPSO->BindSRV("ShadowTex", 2, 1);
	TEMP_LightingPSO->BindSRV("VelocityTex", 3, 1);
	TEMP_LightingPSO->BindSRV("DepthTex", 4, 1);
	TEMP_LightingPSO->BindSRV("GIResultSHTex", 5, 1);
	TEMP_LightingPSO->BindSRV("GIResultColorTex", 6, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITex", 7, 1);
	TEMP_LightingPSO->BindSRV("RoughnessMetalicTex", 8, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITex3x3", 9, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITexMip1", 10, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITexMip2", 11, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITexMip3", 12, 1);
	TEMP_LightingPSO->BindSRV("SpecularGITexMip4", 13, 1);
	
	
	
	TEMP_LightingPSO->BindSampler("samplerWrap", 0);
	TEMP_LightingPSO->BindCBV("LightingParam", 0, sizeof(LightingParam));
	bool bSuccess = TEMP_LightingPSO->Init();
	if (bSuccess)
		LightingPSO = TEMP_LightingPSO;
}

void Corona::InitShadowDenoisePass()
{
	ComPtr<ID3DBlob> cs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\ShadowDenoise.hlsl"), "ShadowDenoiseCS", "cs_5_0");

	shared_ptr<PipelineStateObject> tempPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	tempPSO->Owner = dx12_rhi;
	tempPSO->cs = cs;
	tempPSO->IsCompute = true;
	tempPSO->BindSRV("ShadowTex", 0, 1);
	tempPSO->BindSRV("DepthTex", 1, 1);
	tempPSO->BindSRV("GeoNormalTex", 2, 1);
	tempPSO->BindUAV("OutShadow", 0);
	tempPSO->BindCBV("ShadowDenoiseCB", 0, sizeof(ShadowDenoiseCB));
	if (tempPSO->Init())
		ShadowDenoisePSO = tempPSO;
}

void Corona::InitTemporalAAPass()
{
	ComPtr<ID3DBlob> vs = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\TemporalAA.hlsl"), "VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps = renderBackend->CreateShader(GetAssetFullPath(L"Shaders\\TemporalAA.hlsl"), "PSMain", "ps_5_0");
	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	psoDesc.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	psoDesc.RasterizerState = rasterizerStateDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	//psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	shared_ptr<PipelineStateObject> TEMP_TemporalAAPSO = shared_ptr<PipelineStateObject>(new PipelineStateObject);
	TEMP_TemporalAAPSO->Owner = dx12_rhi;
	TEMP_TemporalAAPSO->ps = ps;
	TEMP_TemporalAAPSO->vs = vs;
	TEMP_TemporalAAPSO->graphicsPSODesc = psoDesc;

	TEMP_TemporalAAPSO->BindSRV("CurrentColorTex", 0, 1);
	TEMP_TemporalAAPSO->BindSRV("PrevColorTex", 1, 1);
	TEMP_TemporalAAPSO->BindSRV("VelocityTex", 2, 1);
	TEMP_TemporalAAPSO->BindSRV("DepthTex", 3, 1);
	TEMP_TemporalAAPSO->BindSRV("BloomTex", 4, 1);
	TEMP_TemporalAAPSO->BindSRV("Exposure", 5, 1);


		TEMP_TemporalAAPSO->BindSampler("samplerWrap", 0);
		TEMP_TemporalAAPSO->BindCBV("LightingParam", 0, sizeof(LightingParam));
	bool bSuccess = TEMP_TemporalAAPSO->Init();
	if (bSuccess)
		TemporalAAPSO = TEMP_TemporalAAPSO;
}



void Corona::ToneMapPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("CopyPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "CopyPass");


	Texture* backbuffer = framebuffers[renderBackend->GetCurrentFrameIndex()].get();
	Texture* ResolveTarget = nullptr;
	
	// Select source texture based on rendering mode
	if (RenderingMode == ERenderingMode::PATHTRACING)
	{
		ResolveTarget = PathTracingAccumBuffer[PathTracingWriteIndex].get();
	}
	else
	{
		if (bUseLightingBufferFallbackForToneMap && LightingBuffer)
			ResolveTarget = LightingBuffer.get();
		else
			ResolveTarget = ColorBuffers[ResolvedColorBufferIndex].get();
	}

	ToneMapPSO->Apply();


	ToneMapPSO->SetSampler("samplerWrap", samplerWrap.get());
	ToneMapPSO->SetSRV("SrcTex", ResolveTarget->GpuHandleSRV);

	ToneMapCB.Offset = glm::vec4(0, 0, 0, 0);
	ToneMapCB.Scale = glm::vec4(1, 1, 0, 0);
	ToneMapCB.ToneMapMode = ToneMapMode;
	ToneMapPSO->SetCBVValue("ScaleOffsetParams", &ToneMapCB);

	ToneMapPSO->Apply();

	renderBackend->SetViewportAndScissor(m_width, m_height);
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());

	
	//PIXEndEvent(renderBackend->GetGraphicsCommandList());
}

void Corona::DebugPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("DebugPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "DebugPass");

	BufferVisualizePSO->Apply();
	BufferVisualizePSO->SetSampler("samplerWrap", samplerWrap.get());

	Texture* backbuffer = framebuffers[renderBackend->GetCurrentFrameIndex()].get();

	renderBackend->DrawFullscreenQuad(FullScreenVB.get());

	

	std::vector<std::function<void(EDebugVisualization eFS)>> functions;
	functions.push_back([&](EDebugVisualization eFS){
		//raytraced shadow
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SHADOW)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", ShadowBuffer->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// world normal
		DebugPassCB cb;

		if (eFS ==  EDebugVisualization::WORLD_NORMAL)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", NormalBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// geom world normal
		DebugPassCB cb;
		if (eFS == EDebugVisualization::GEO_NORMAL)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", GeomNormalBuffer->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// Blooom buffer
		DebugPassCB cb;
		if (eFS == EDebugVisualization::BLOOM)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.25, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}
		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", BloomBlurPingPong[0]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});


	functions.push_back([&](EDebugVisualization eFS) {
		// depth
		DebugPassCB cb;

		if (eFS == EDebugVisualization::DEPTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.ProjectionParams.z = Near;
		cb.ProjectionParams.w = Far;
		cb.DebugMode = DEPTH;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", UnjitteredDepthBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// raw diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RAW_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGIRaw->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// raw diffuse gi aux
		DebugPassCB cb;

		if (eFS == EDebugVisualization::RAW_DIFFUSE_GI_AUX)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGIRawAux->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// temporal filtered diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGITemporal[GIBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// spatial filtered diffuse gi
		DebugPassCB cb;

		if (eFS == EDebugVisualization::SPATIAL_FILTERED_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else  if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.25, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGISpatial[0]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// final diffuse gi
		DebugPassCB cb;

		cb.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
		cb.GIBufferScale = GIBufferScale;
		if (eFS == EDebugVisualization::FINAL_DIFFUSE_GI)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = SH_LIGHTING;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", DiffuseGISpatial[0]->GpuHandleSRV);
		BufferVisualizePSO->SetSRV("SrcTexSH", DiffuseGISpatialAux[0]->GpuHandleSRV);
		BufferVisualizePSO->SetSRV("SrcTexNormal", NormalBuffers[ColorBufferWriteIndex]->GpuHandleSRV);

		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// albedo
		DebugPassCB cb;

		if (eFS == EDebugVisualization::ALBEDO)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else  if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, -0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", AlbedoBuffer->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	
	functions.push_back([&](EDebugVisualization eFS) {
		// velocity
		DebugPassCB cb;

		if (eFS == EDebugVisualization::VELOCITY)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if(eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", VelocityBuffer->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	
	functions.push_back([&](EDebugVisualization eFS) {
		// material
		DebugPassCB cb;
		if (eFS == EDebugVisualization::ROUGNESS_METALLIC)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(0.75, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", RoughnessMetalicBuffer->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});
	functions.push_back([&](EDebugVisualization eFS) {
		// specular raw
		DebugPassCB cb;


		if (eFS == EDebugVisualization::SPECULAR_RAW)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, -0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", SpecularGIRaw->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// temporal filtered specular
		DebugPassCB cb;


		if (eFS == EDebugVisualization::TEMPORAL_FILTERED_SPECULAR)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, 0.25, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = RAW_COPY;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", SpecularGITemporal[GIBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});

	functions.push_back([&](EDebugVisualization eFS) {
		// history length
		DebugPassCB cb;


		if (eFS == EDebugVisualization::SPEC_HISTORY_LENGTH)
		{
			cb.Offset = glm::vec4(0, 0, 0, 0);
			cb.Scale = glm::vec4(1, 1, 0, 0);
		}
		else if (eFS == EDebugVisualization::NO_FULLSCREEN)
		{
			cb.Offset = glm::vec4(-0.75, 0.75, 0, 0);
			cb.Scale = glm::vec4(0.25, 0.25, 0, 0);
		}
		else
		{
			return;
		}

		cb.DebugMode = CHANNEL_W;
		BufferVisualizePSO->SetCBVValue("DebugPassCB", &cb);
		BufferVisualizePSO->SetSRV("SrcTex", SpecularGITemporal[GIBufferWriteIndex]->GpuHandleSRV);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	});

	EDebugVisualization FullScreenVisualize = EDebugVisualization::SPECULAR_RAW;

	for (auto& f : functions)
	{
		f(FullscreenDebugBuffer);
	}
}

void Corona::LightingPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("LightingPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "LightingPass");

	renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	LightingPSO->Apply();

	LightingPSO->SetSampler("samplerWrap", samplerWrap.get());
	LightingPSO->SetSRV("AlbedoTex", AlbedoBuffer->GpuHandleSRV);
	LightingPSO->SetSRV("NormalTex", NormalBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
	LightingPSO->SetSRV("ShadowTex", ShadowDenoisedBuffer ? ShadowDenoisedBuffer->GpuHandleSRV : ShadowBuffer->GpuHandleSRV);

	LightingPSO->SetSRV("VelocityTex", VelocityBuffer->GpuHandleSRV);
	LightingPSO->SetSRV("DepthTex", DepthBuffer->GpuHandleSRV);
	LightingPSO->SetSRV("GIResultSHTex", DiffuseGISpatialAux[0]->GpuHandleSRV);
	LightingPSO->SetSRV("GIResultColorTex", DiffuseGISpatial[0]->GpuHandleSRV);
	LightingPSO->SetSRV("SpecularGITex", SpecularGISpatial[0]->GpuHandleSRV);
	LightingPSO->SetSRV("RoughnessMetalicTex", RoughnessMetalicBuffer->GpuHandleSRV);




	glm::mat4x4 InvViewMat = glm::inverse(ViewMat);
	
	// Calculate light color from sky gradient (same as raytracing modes)
	glm::vec3 normalizedLightDir = glm::normalize(LightDir);
	float lightDirT = 0.5f * (normalizedLightDir.y + 1.0f);
	glm::vec3 lightColor = glm::mix(SkyColorBottom, SkyColorTop, lightDirT);
	
	LightingParam Param;
	Param.ViewMatrix = glm::transpose(ViewMat);
	Param.InvViewMatrix = glm::transpose(InvViewMat);
	Param.LightDir = glm::vec4(normalizedLightDir, LightIntensity);
	
	Param.RTSize.x = GetRenderWidth();
	Param.RTSize.y = GetRenderHeight();

	if (IsTemporalAAEnabled())
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	Param.GIBufferScale = GIBufferScale;
	Param.LightColor = lightColor;
	Param.bEnableDiffuseGI = bEnableDiffuseGI ? 1 : 0;
	Param.bEnableSpecularGI = bEnableSpecularGI ? 1 : 0;
	Param.bEnableDirectDiffuse = bEnableDirectDiffuse ? 1 : 0;
	Param.bEnableDirectSpecular = bEnableDirectSpecular ? 1 : 0;

	glm::normalize(Param.LightDir);
	LightingPSO->SetCBVValue("LightingParam", &Param);


	LightingPSO->Apply();

	renderBackend->SetRenderTarget(LightingBuffer.get());
	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
}

void Corona::TemporalAAPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("TemporalAAPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalAAPass");

	UINT PrevColorBufferIndex = 1 - ColorBufferWriteIndex;
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex].get();

	renderBackend->TransitionTexture(ResolveTarget, EResourceState::ShaderRead, EResourceState::RenderTarget);

	TemporalAAPSO->Apply();

	TemporalAAPSO->SetSampler("samplerWrap", samplerBilinearWrap.get());
	TemporalAAPSO->SetSRV("CurrentColorTex", LightingBuffer->GpuHandleSRV);
	Texture* PrevColorBuffer = ColorBuffers[PrevColorBufferIndex].get();
	TemporalAAPSO->SetSRV("PrevColorTex", PrevColorBuffer->GpuHandleSRV);
	TemporalAAPSO->SetSRV("VelocityTex", VelocityBuffer->GpuHandleSRV);
	TemporalAAPSO->SetSRV("DepthTex", DepthBuffer->GpuHandleSRV);
	TemporalAAPSO->SetSRV("BloomTex", BloomBlurPingPong[0]->GpuHandleSRV);
	TemporalAAPSO->SetSRV("Exposure", ExposureData->GpuHandleSRV);

	TemporalAAParam Param;

	Param.RTSize.x = GetRenderWidth();
	Param.RTSize.y = GetRenderHeight();

	if (IsTemporalAAEnabled())
		Param.TAABlendFactor = 0.1;
	else
		Param.TAABlendFactor = 1.0;

	Param.ClampMode = ClampMode;
	Param.BloomStrength = BloomStrength;
	Param.HistoryValid = bTemporalAAHistoryValid ? 1u : 0u;

	TemporalAAPSO->SetCBVValue("LightingParam", &Param);
	TemporalAAPSO->Apply();

	renderBackend->SetRenderTarget(ResolveTarget);
	renderBackend->SetViewportAndScissor(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height));
	renderBackend->DrawFullscreenQuad(FullScreenVB.get());
	
	renderBackend->TransitionTexture(ResolveTarget, EResourceState::RenderTarget, EResourceState::UnorderedAccess);

	if (bDrawHistogram)
	{
		DrawHistogramPSO->Apply();
		DrawHistogramPSO->SetSRV("Histogram", Histogram->GpuHandleSRV);
		DrawHistogramPSO->SetSRV("Exposure", ExposureData->GpuHandleSRV);
		DrawHistogramPSO->SetUAV("ColorBuffer", ResolveTarget->GpuHandleUAV);
		renderBackend->Dispatch(1, 32, 1);
	}
	renderBackend->TransitionTexture(ResolveTarget, EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	bTemporalAAHistoryValid = IsTemporalAAEnabled();
	bUseLightingBufferFallbackForToneMap = false;
	ResolvedColorBufferIndex = ColorBufferWriteIndex;

}


void Corona::BloomPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("BloomPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "BloomPass");

	BloomCB.RTSize.x = BloomBufferWidth;
	BloomCB.RTSize.y = BloomBufferHeight;

	float sigma_pixels = BloomSigma * m_height;

	float effective_sigma = sigma_pixels * 0.25f;
	effective_sigma = glm::min(effective_sigma, 100.f);
	effective_sigma = glm::max(effective_sigma, 1.f);
	BloomCB.NumSamples = glm::round(effective_sigma*4.f);
	BloomCB.WeightScale = -1.f / (2.0 * effective_sigma * effective_sigma);
	BloomCB.NormalizationScale = 1.f / (sqrtf(2 * glm::pi<float>()) * effective_sigma);;
	//BloomCB.Exposure = Exposure;
	/*BloomCB.MinLog = kInitialMinLog;
	BloomCB.RcpLogRange = 1.0f / (kInitialMaxLog - kInitialMinLog);*/

	// extraction pass
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(LumaBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	BloomExtractPSO->Apply();
	BloomExtractPSO->SetSRV("SrcTex", LightingBuffer->GpuHandleSRV);
	BloomExtractPSO->SetSRV("Exposure", ExposureData->GpuHandleSRV);
	BloomExtractPSO->SetUAV("DstTex", BloomBlurPingPong[0]->GpuHandleUAV);
	BloomExtractPSO->SetUAV("LumaResult", LumaBuffer->GpuHandleUAV);


	BloomExtractPSO->SetSampler("samplerWrap", samplerWrap.get());

	BloomExtractPSO->SetCBVValue("BloomCB", &BloomCB);

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(LumaBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// horizontal pass
	renderBackend->TransitionTexture(BloomBlurPingPong[1].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);


	BloomBlurPSO->Apply();

	BloomBlurPSO->SetSRV("SrcTex", BloomBlurPingPong[0]->GpuHandleSRV);
	BloomBlurPSO->SetUAV("DstTex", BloomBlurPingPong[1]->GpuHandleUAV);


	BloomBlurPSO->SetSampler("samplerWrap", samplerWrap.get());

	BloomCB.BlurDirection = glm::vec2(1, 0);
	BloomBlurPSO->SetCBVValue("BloomCB", &BloomCB);

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);

	renderBackend->TransitionTexture(BloomBlurPingPong[1].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);


	// vertical pass
	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);


	BloomBlurPSO->Apply();

	BloomBlurPSO->SetSRV("SrcTex", BloomBlurPingPong[1]->GpuHandleSRV);
	BloomBlurPSO->SetUAV("DstTex", BloomBlurPingPong[0]->GpuHandleUAV);


	BloomBlurPSO->SetSampler("samplerWrap", samplerWrap.get());

	BloomCB.BlurDirection = glm::vec2(0, 1);
	BloomBlurPSO->SetCBVValue("BloomCB", &BloomCB);

	renderBackend->Dispatch(BloomBufferWidth / 32, BloomBufferHeight / 32, 1);

	renderBackend->TransitionTexture(BloomBlurPingPong[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

	// histogram pass
	renderBackend->TransitionBuffer(Histogram.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	ClearHistogramPSO->Apply();
	ClearHistogramPSO->SetUAV("Histogram", Histogram->GpuHandleUAV);
	renderBackend->Dispatch(1, 1, 1);

	HistogramPSO->Apply();
	HistogramPSO->SetSRV("LumaTex", LumaBuffer->GpuHandleSRV);
	HistogramPSO->SetUAV("Histogram", Histogram->GpuHandleUAV);
	renderBackend->Dispatch(BloomBufferWidth / 16, 1, 1);

	renderBackend->TransitionBuffer(Histogram.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);


	// adapte exposure pass
	renderBackend->TransitionBuffer(ExposureData.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	AdapteExposurePSO->Apply();
	AdapteExposurePSO->SetSRV("Histogram", Histogram->GpuHandleSRV);
	AdapteExposurePSO->SetUAV("Exposure", ExposureData->GpuHandleUAV);


	AdaptExposureCB.PixelCount = BloomBufferWidth * BloomBufferHeight;
	
	AdapteExposurePSO->SetCBVValue("AdaptExposureCB", &AdaptExposureCB);

	renderBackend->Dispatch(1, 1, 1);
	renderBackend->TransitionBuffer(ExposureData.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);

}

static const float OneMinusEpsilon = 0.9999999403953552f;

inline float RadicalInverseBase2(uint32 bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10f; // / 0x100000000
}
inline glm::vec2 Hammersley2D(uint64 sampleIdx, uint64 numSamples)
{
	return glm::vec2(float(sampleIdx) / float(numSamples), RadicalInverseBase2(uint32(sampleIdx)));
}

void Corona::OnUpdate()
{
	if (bPendingUpscaleRefresh)
	{
		RefreshUpscaleSettings(true);
		bPendingUpscaleRefresh = false;
	}

	m_timer.Tick(NULL);

	if (m_frameCounter == 100)
	{
		// Update window text with FPS value.
		wchar_t fps[64];
		swprintf_s(fps, L"%ufps", m_timer.GetFramesPerSecond());
		SetCustomWindowText(fps);
		m_frameCounter = 0;
	}

	m_frameCounter++;

	m_camera.SetTurnSpeed(m_turnSpeed);
	m_camera.Update(static_cast<float>(m_timer.GetElapsedSeconds()));

	ViewMat = m_camera.GetViewMatrix();
	ProjMat = m_camera.GetProjectionMatrix(Fov, m_aspectRatio, Near, Far);
	UnjitteredProjMat = ProjMat;
	UnjitteredViewProjMat = ProjMat * ViewMat;

	InvViewMat = glm::inverse(ViewMat);
	InvProjMat = glm::inverse(ProjMat);

	RTShadowViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTShadowViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTShadowViewParam.ProjMatrix = glm::transpose(ProjMat);
	RTShadowViewParam.InvProjMatrix = glm::transpose(InvProjMat);
	RTShadowViewParam.ProjectionParams.x = Far / (Far - Near);
	RTShadowViewParam.ProjectionParams.y = Near / (Near - Far);
	RTShadowViewParam.ProjectionParams.z = Near;
	RTShadowViewParam.ProjectionParams.w = Far;
	RTShadowViewParam.LightDir = glm::vec4(LightDir, 0);
	RTShadowViewParam.ShadowLightRadius = 0.03f;
	RTShadowViewParam.ShadowSampleCount = 8;

	glm::vec2 Jitter;
	const uint64 ActiveJitterSampleCount = IsDLSSUpscaleEnabled() ? std::max<uint64>(1, DLSSJitterPhaseCount) : std::max<uint64>(1, TAASampleCount);
	uint64 idx = FrameCounter % ActiveJitterSampleCount;
	Jitter = Hammersley2D(idx, ActiveJitterSampleCount) * 2.0f - glm::vec2(1.0f);
	Jitter *= JitterScale;

	const float offsetX = Jitter.x * (1.0f / GetRenderWidth());
	const float offsetY = Jitter.y * (1.0f / GetRenderHeight());

	if (IsJitterEnabled())
		JitterOffset = (Jitter - PrevJitter) * 0.5f;
	else
		JitterOffset = glm::vec2(0, 0);
	
	CurrentJitter = IsJitterEnabled() ? Jitter : glm::vec2(0.0f);
	PrevJitter = CurrentJitter;
	glm::mat4x4 JitterMat = glm::translate(glm::vec3(offsetX, -offsetY, 0));
	

	if (IsJitterEnabled())
		ProjMat = JitterMat * ProjMat;


	ViewProjMat = ProjMat * ViewMat;
	if (bResetTemporalStateNextUpdate)
	{
		PrevViewProjMat = ViewProjMat;
		PrevViewMat = ViewMat;
		PrevUnjitteredViewProjMat = UnjitteredViewProjMat;
		PrevJitter = CurrentJitter;
		JitterOffset = glm::vec2(0.0f);
		bTemporalAAHistoryValid = false;
		bTemporalDenoiserHistoryValid = false;
		bResetTemporalStateNextUpdate = false;
	}

	InvViewProjMat = glm::inverse(ViewProjMat);
	glm::mat4x4 UnjitteredInvProjMat = glm::inverse(UnjitteredProjMat);
	
	float timeElapsed = m_timer.GetTotalSeconds();
	timeElapsed *= 0.01f;
	// Calculate light color from sky gradient based on light direction
	glm::vec3 normalizedLightDir = glm::normalize(LightDir);
	float lightDirT = 0.5f * (normalizedLightDir.y + 1.0f);
	glm::vec3 lightColor = glm::mix(SkyColorBottom, SkyColorTop, lightDirT);

	bool indirectCameraChanged = false;
	for (int i = 0; i < 4 && !indirectCameraChanged; i++)
	{
		for (int j = 0; j < 4 && !indirectCameraChanged; j++)
		{
			if (abs(PrevIndirectAccumViewMat[i][j] - ViewMat[i][j]) > 0.0001f)
				indirectCameraChanged = true;
		}
	}

	const bool indirectLightDirChanged = glm::length(normalizedLightDir - PrevIndirectAccumLightDir) > 0.0001f;
	const bool indirectLightIntensityChanged = abs(LightIntensity - PrevIndirectAccumLightIntensity) > 0.0001f;
	const bool indirectSkyChanged =
		glm::length(SkyColorTop - PrevIndirectSkyColorTop) > 0.0001f ||
		glm::length(SkyColorBottom - PrevIndirectSkyColorBottom) > 0.0001f ||
		abs(SkyIntensity - PrevIndirectSkyIntensity) > 0.0001f;

	if (indirectCameraChanged || indirectLightDirChanged || indirectLightIntensityChanged || indirectSkyChanged)
	{
		IndirectAccumulatedFrames = 0;
		bTemporalDenoiserHistoryValid = false;
		PrevIndirectAccumViewMat = ViewMat;
		PrevIndirectAccumLightDir = normalizedLightDir;
		PrevIndirectAccumLightIntensity = LightIntensity;
		PrevIndirectSkyColorTop = SkyColorTop;
		PrevIndirectSkyColorBottom = SkyColorBottom;
		PrevIndirectSkyIntensity = SkyIntensity;
	}
	
	// reflection view param
	RTReflectionViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTReflectionViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTReflectionViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	RTReflectionViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	RTReflectionViewParam.ProjectionParams.x = Far / (Far - Near);
	RTReflectionViewParam.ProjectionParams.y = Near / (Near - Far);
	RTReflectionViewParam.ProjectionParams.z = Near;
	RTReflectionViewParam.ProjectionParams.w = Far;
	RTReflectionViewParam.LightDir = glm::vec4(normalizedLightDir, LightIntensity);
	RTReflectionViewParam.RandomOffset = glm::vec2(timeElapsed, timeElapsed);
	RTReflectionViewParam.FrameCounter = FrameCounter;
	RTReflectionViewParam.SkyColorTop = SkyColorTop;
	RTReflectionViewParam.SkyColorBottom = SkyColorBottom;
	RTReflectionViewParam.SkyIntensity = SkyIntensity;
	RTReflectionViewParam.LightColor = lightColor;

	// GI view param
	RTGIViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTGIViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTGIViewParam.ProjMatrix = glm::transpose(UnjitteredProjMat);
	RTGIViewParam.InvProjMatrix = glm::transpose(UnjitteredInvProjMat);
	RTGIViewParam.ProjectionParams.x = Far / (Far - Near);
	RTGIViewParam.ProjectionParams.y = Near / (Near - Far);
	RTGIViewParam.ProjectionParams.z = Near;
	RTGIViewParam.ProjectionParams.w = Far;
	RTGIViewParam.LightDir = glm::vec4(normalizedLightDir, LightIntensity);
	RTGIViewParam.RandomOffset = glm::vec2(timeElapsed, timeElapsed);
	RTGIViewParam.FrameCounter = FrameCounter;
	RTGIViewParam.SkyColorTop = SkyColorTop;
	RTGIViewParam.SkyColorBottom = SkyColorBottom;
	RTGIViewParam.SkyIntensity = SkyIntensity;
	RTGIViewParam.LightColor = lightColor;
	
	// Path Tracing view param
	PathTracingViewParam.ViewMatrix = glm::transpose(ViewMat);
	PathTracingViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	PathTracingViewParam.ProjMatrix = glm::transpose(ProjMat);
	PathTracingViewParam.InvProjMatrix = glm::transpose(InvProjMat);
	PathTracingViewParam.ProjectionParams.x = Far / (Far - Near);
	PathTracingViewParam.ProjectionParams.y = Near / (Near - Far);
	PathTracingViewParam.ProjectionParams.z = Near;
	PathTracingViewParam.ProjectionParams.w = Far;
	PathTracingViewParam.LightDirAndIntensity = glm::vec4(normalizedLightDir, LightIntensity);
	PathTracingViewParam.RandomOffset = glm::vec2(timeElapsed, timeElapsed);
	PathTracingViewParam.FrameCounter = FrameCounter;
	PathTracingViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5) / (0.5f * m_height);
	PathTracingViewParam.SkyColorTop = SkyColorTop;
	PathTracingViewParam.SkyColorBottom = SkyColorBottom;
	PathTracingViewParam.SkyIntensity = SkyIntensity;
	PathTracingViewParam.LightColor = lightColor;
	PathTracingViewParam.bEnableDiffuseGI = bEnableDiffuseGI ? 1 : 0;
	PathTracingViewParam.bEnableSpecularGI = bEnableSpecularGI ? 1 : 0;
	PathTracingViewParam.bEnableDirectDiffuse = bEnableDirectDiffuse ? 1 : 0;
	PathTracingViewParam.bEnableDirectSpecular = bEnableDirectSpecular ? 1 : 0;
	
	SpatialFilterCB.ProjectionParams.z = Near;
	SpatialFilterCB.ProjectionParams.w = Far;
	SpatialFilterCB.AccumulatedFrames = IndirectAccumulatedFrames;


	TemporalFilterCB.InvViewMatrix = glm::transpose(InvViewMat);
	TemporalFilterCB.InvProjMatrix = glm::transpose(InvProjMat);
	TemporalFilterCB.ProjectionParams.z = Near;
	TemporalFilterCB.ProjectionParams.w = Far;
	TemporalFilterCB.RTSize.x = GetRenderWidth();
	TemporalFilterCB.RTSize.y = GetRenderHeight();
	TemporalFilterCB.FrameIndex = FrameCounter;
	TemporalFilterCB.AccumulationAlpha = bTemporalDenoiserHistoryValid ? (1.0f / float(std::min(IndirectAccumulatedFrames + 1u, 32u))) : 1.0f;

	// Don't increment frame counter in debug mode (to avoid accumulation noise)
	if (PathTracingViewParam.DebugMode == 0)
	{
		FrameCounter++;
	}

	//ColorBufferWriteIndex = FrameCounter % 2;

	if (bRecompileShaders)
	{
		RecompileShaders();
		bRecompileShaders = false;
	}
}

// Render the scene.
void Corona::OnRender()
{
	renderBackend->BeginFrame();
	UpdateGpuTimingReadback();
	BeginGpuTimingFrame();
#if WITH_STREAMLINE
	StreamlineFrameToken = nullptr;
	bStreamlineConstantsSetThisFrame = false;
#endif
	if (bPendingTemporalHistoryClear)
	{
		ResetTemporalHistoryBuffers();
		bPendingTemporalHistoryClear = false;
	}
	
	// Record all the commands we need to render the scene into the command list.
	BeginGpuPassTiming(EGpuPass::Frame);

	if (RenderingMode == ERenderingMode::HYBRID)
	{
		// Hybrid rendering: Rasterization GBuffer + Raytracing
		BeginGpuPassTiming(EGpuPass::GBuffer);
		GBufferPass();
		EndGpuPassTiming(EGpuPass::GBuffer);

		BeginGpuPassTiming(EGpuPass::RaytraceShadow);
		RaytraceShadowPass();
		EndGpuPassTiming(EGpuPass::RaytraceShadow);

		BeginGpuPassTiming(EGpuPass::ShadowDenoise);
		ShadowDenoisePass();
		EndGpuPassTiming(EGpuPass::ShadowDenoise);

		BeginGpuPassTiming(EGpuPass::RaytraceReflection);
		RaytraceReflectionPass();
		EndGpuPassTiming(EGpuPass::RaytraceReflection);

		BeginGpuPassTiming(EGpuPass::RaytraceGI);
		RaytraceGIPass();
		EndGpuPassTiming(EGpuPass::RaytraceGI);

		// Simple GI denoising: edge-aware temporal accumulation + spatial bilateral.
		BeginGpuPassTiming(EGpuPass::TemporalDenoise);
		TemporalDenoisingPass();
		EndGpuPassTiming(EGpuPass::TemporalDenoise);
		// GenMipSpecularGIPass();
		BeginGpuPassTiming(EGpuPass::SpatialDenoise);
		SpatialDenoisingPass();
		EndGpuPassTiming(EGpuPass::SpatialDenoise);

		BeginGpuPassTiming(EGpuPass::Lighting);
		LightingPass();
		EndGpuPassTiming(EGpuPass::Lighting);

		// BloomPass(); // Disabled for hybrid mode

		if (IsDLSSRREnabled())
		{
			bool bNeedTemporalAA = false;
			BeginGpuPassTiming(EGpuPass::DLSSRR);
			const bool bRRPassed = DLSSRRPass();
			EndGpuPassTiming(EGpuPass::DLSSRR);
			if (bRRPassed)
			{
				BeginGpuPassTiming(EGpuPass::DLSSSR);
				const bool bDLSSPassed = DLSSPass();
				EndGpuPassTiming(EGpuPass::DLSSSR);
				bNeedTemporalAA = !bDLSSPassed;
			}
			else
			{
				bNeedTemporalAA = true;
			}

			if (bNeedTemporalAA)
			{
				BeginGpuPassTiming(EGpuPass::TemporalAA);
				TemporalAAPass();
				EndGpuPassTiming(EGpuPass::TemporalAA);
			}
		}
		else if (IsDLSSSREnabled())
		{
			BeginGpuPassTiming(EGpuPass::DLSSSR);
			const bool bDLSSPassed = DLSSPass();
			EndGpuPassTiming(EGpuPass::DLSSSR);
			if (!bDLSSPassed)
			{
				BeginGpuPassTiming(EGpuPass::TemporalAA);
				TemporalAAPass();
				EndGpuPassTiming(EGpuPass::TemporalAA);
			}
		}
		else
		{
			if (DLSSTransitionFramesRemaining > 0)
			{
				--DLSSTransitionFramesRemaining;
			}
			else
			{
				BeginGpuPassTiming(EGpuPass::TemporalAA);
				TemporalAAPass();
				EndGpuPassTiming(EGpuPass::TemporalAA);
			}
		}
	}
	else if (RenderingMode == ERenderingMode::PATHTRACING)
	{
		// Full path tracing
		BeginGpuPassTiming(EGpuPass::PathTracing);
		PathTracingPass();
		EndGpuPassTiming(EGpuPass::PathTracing);
		
		// Copy path tracing result to color buffer for tonemap
		// (In a complete implementation, you would copy PathTracingAccumBuffer to ColorBuffers)
	}

	
	Texture* backbuffer = framebuffers[renderBackend->GetCurrentFrameIndex()].get();
	renderBackend->TransitionTexture(backbuffer, EResourceState::Present, EResourceState::RenderTarget);
	renderBackend->SetRenderTarget(backbuffer);

	BeginGpuPassTiming(EGpuPass::ToneMap);
	ToneMapPass();
	EndGpuPassTiming(EGpuPass::ToneMap);
	AdvanceAutoAADump(backbuffer);

	if(bDebugDraw)
	{
		BeginGpuPassTiming(EGpuPass::Debug);
		DebugPass();
		EndGpuPassTiming(EGpuPass::Debug);
	}

	if (bShowImgui)
	{

		renderBackend->NewImGuiFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		bool show_demo_window = true;

		//ImGui::ShowDemoWindow(&show_demo_window);

		char fps[64];
		sprintf(fps, "FPS : %u fps", m_timer.GetFramesPerSecond());

		ImGui::Begin("Hi, Let's traceray!");
		ImGui::Text(fps);
		if (renderBackend)
		{
			ImGui::Text("Render Backend: %s", renderBackend->GetBackendName());
		}

		glm::vec4 test = glm::vec4(0, -0, 0, 1) * glm::transpose(UnjitteredViewProjMat);
		test.x /= test.w;
		test.y /= test.w;
		//test.z /= test.w;
		sprintf(fps, "test : %f %f %f", test.x, test.y, test.z);
		ImGui::Text(fps);
		if (ImGui::Button("GPU Pass Timings"))
		{
			bShowGpuTimingWindow = true;
		}

		if (bShowGpuTimingWindow)
		{
			ImGui::Begin("GPU Pass Timings", &bShowGpuTimingWindow);
			ImGui::Text("GPU Pass Timings (ms)");
			int averageFrameCountUI = static_cast<int>(GpuTimingAverageFrameCount);
			if (ImGui::SliderInt("Average Frames", &averageFrameCountUI, 1, 240))
			{
				GpuTimingAverageFrameCount = static_cast<UINT32>(averageFrameCountUI);
				for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
				{
					auto& history = GpuPassHistoryMs[passIndex];
					while (history.size() > GpuTimingAverageFrameCount)
					{
						history.pop_front();
					}

					float sumMs = 0.0f;
					for (float sampleMs : history)
					{
						sumMs += sampleMs;
					}
					GpuPassAverageTimeMs[passIndex] = history.empty() ? 0.0f : (sumMs / static_cast<float>(history.size()));
				}
			}
			ImGui::Separator();
			for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
			{
				if (GpuPassAverageTimeMs[passIndex] <= 0.0f)
					continue;

				ImGui::Text(
					"%s: %.3f ms (avg %uF %.3f ms)",
					GetGpuPassName(static_cast<EGpuPass>(passIndex)),
					GpuPassLastTimeMs[passIndex],
					static_cast<unsigned>(GpuPassHistoryMs[passIndex].size()),
					GpuPassAverageTimeMs[passIndex]);
			}
			ImGui::End();
		}

		if (ImGui::Button("Recompile all shaders"))
			bRecompileShaders = true;
		
		// Example: Add ShaderBall scene at runtime
		// if (ImGui::Button("Add ShaderBall Scene"))
		// {
		// 	AddScene(ShaderBall);
		// }

		ImGui::Text("\nArrow keys : rotate camera imGui\
			\nWASD keys : move camera imGui\
			\nI : show/hide imGui\
			\nB : show/hide buffer visualization\
			\nT : cycle anti-aliasing mode\n\n");

		ImGui::SliderFloat("Camera turn speed", &m_turnSpeed, 0.0f, glm::half_pi<float>()*2);
		{
			static const char* AAModes[] = { "Off", "TAA", "DLSS SR", "DLSS RR" };
			int AAModeIndex = static_cast<int>(AntiAliasingMode);
			if (ImGui::Combo("Anti-Aliasing", &AAModeIndex, AAModes, IM_ARRAYSIZE(AAModes)))
			{
				const EAntiAliasingMode PreviousMode = AntiAliasingMode;
				EAntiAliasingMode RequestedMode = static_cast<EAntiAliasingMode>(AAModeIndex);
#if WITH_STREAMLINE
				if (RequestedMode == EAntiAliasingMode::DLSS_SR && !bDLSSAvailable)
					RequestedMode = EAntiAliasingMode::TAA;
				if (RequestedMode == EAntiAliasingMode::DLSS_RR && !bDLSSRRAvailable)
					RequestedMode = EAntiAliasingMode::TAA;
#else
				if (RequestedMode == EAntiAliasingMode::DLSS_SR || RequestedMode == EAntiAliasingMode::DLSS_RR)
					RequestedMode = EAntiAliasingMode::TAA;
#endif
				AntiAliasingMode = RequestedMode;
				ResetAllAccumulationState(IsDLSSMode(PreviousMode) || IsDLSSMode(RequestedMode));
			}
			if (IsTemporalAAEnabled())
			{
				int TAASampleCountUI = static_cast<int>(TAASampleCount);
				if (ImGui::SliderInt("TAA Jitter Samples", &TAASampleCountUI, 1, 64))
				{
					TAASampleCount = static_cast<UINT32>(TAASampleCountUI);
					FrameCounter = 0;
					PrevJitter = glm::vec2(0.0f);
					bTemporalAAHistoryValid = false;
					bTemporalDenoiserHistoryValid = false;
					bResetTemporalStateNextUpdate = true;
				}
			}
			if (ImGui::SliderFloat("TAA Jitter Scale", &JitterScale, 0.0f, 1.0f))
			{
				FrameCounter = 0;
				PrevJitter = glm::vec2(0.0f);
				bTemporalAAHistoryValid = false;
				bTemporalDenoiserHistoryValid = false;
				bResetTemporalStateNextUpdate = true;
			}
#if WITH_STREAMLINE
			if (bDLSSAvailable || bDLSSRRAvailable)
			{
				static const char* DLSSModes[] = { "Quality", "Balanced", "Performance", "Ultra Performance" };
				int DLSSQualityIndex = static_cast<int>(DLSSQualityMode);
				if (ImGui::Combo("DLSS Quality", &DLSSQualityIndex, DLSSModes, IM_ARRAYSIZE(DLSSModes)))
				{
					DLSSQualityMode = static_cast<EDLSSQualityMode>(DLSSQualityIndex);
					ResetAllAccumulationState(true);
				}
				ImGui::Text("DLSS SR Available: %s", bDLSSAvailable ? "Yes" : "No");
				ImGui::Text("DLSS RR Available: %s", bDLSSRRAvailable ? "Yes" : "No");
				ImGui::Text("Render Resolution: %u x %u", RenderWidth, RenderHeight);
				if (IsDLSSUpscaleEnabled())
					ImGui::Text("DLSS Jitter Phases: %u", DLSSJitterPhaseCount);
			}
			else
			{
				ImGui::Text("DLSS SR Available: No");
				ImGui::Text("DLSS RR Available: No");
			}
#endif
		}
		ImGui::Checkbox("Visualize Buffers", &bDebugDraw);
		ImGui::Checkbox("Draw Histogram", &bDrawHistogram);
		
		// Lighting control options (both Hybrid and Path Tracing)
		if (RenderingMode == ERenderingMode::HYBRID || RenderingMode == ERenderingMode::PATHTRACING)
		{
			ImGui::Separator();
			ImGui::Text("Lighting Control");
			
			bool bLightingChanged = false;
			if (ImGui::Checkbox("Enable Direct Diffuse", &bEnableDirectDiffuse)) bLightingChanged = true;
			if (ImGui::Checkbox("Enable Direct Specular", &bEnableDirectSpecular)) bLightingChanged = true;
			if (ImGui::Checkbox("Enable Indirect Diffuse (GI)", &bEnableDiffuseGI)) bLightingChanged = true;
			if (ImGui::Checkbox("Enable Indirect Specular (GI)", &bEnableSpecularGI)) bLightingChanged = true;
			
			// Lighting toggles only invalidate shading history; they do not require
			// DLSS/RR resource reallocation or render-resolution changes.
			if (bLightingChanged)
			{
				ResetAllAccumulationState(false);
			}
		}

	
		/*
		enum class EDebugVisualization
		{
				SHADOW,
		WORLD_NORMAL,
		GEO_NORMAL,
		DEPTH,
		RAW_DIFFUSE_GI,
		RAW_DIFFUSE_GI_AUX,
		TEMPORAL_FILTERED_DIFFUSE_GI,
		SPATIAL_FILTERED_DIFFUSE_GI,
		FINAL_DIFFUSE_GI,
		ALBEDO,
		VELOCITY,
		ROUGNESS_METALLIC,
		SPECULAR_RAW,
		TEMPORAL_FILTERED_SPECULAR,
		BLOOM,
		SPEC_HISTORY_LENGTH,
		NO_FULLSCREEN,
		};
		*/
		static ImGuiComboFlags flags = 0;
		const char* items[] = { 
			"SHADOW",
			"WORLD_NORMAL",
			"GEO_NORMAL",
			"DEPTH",
			"RAW_DIFFUSE_GI",
			"RAW_DIFFUSE_GI_AUX",
			"TEMPORAL_FILTERED_DIFFUSE_GI",
			"SPATIAL_FILTERED_DIFFUSE_GI",
			"FINAL_DIFFUSE_GI",
			"ALBEDO",
			"VELOCITY",
			"ROUGNESS_METALLIC",
			"SPECULAR_RAW",
			"TEMPORAL_FILTERED_SPECULAR",
			"BLOOM",
			"SPEC_HISTORY_LENGTH",
			"NO_FULLSCREEN",
		};
		static const char* item_current = items[UINT(EDebugVisualization::NO_FULLSCREEN)];
		if (ImGui::BeginCombo("Visualize Full Screen", item_current, flags))
		{
			for (int n = 0; n < IM_ARRAYSIZE(items); n++)
			{
				bool is_selected = (item_current == items[n]);
				if (ImGui::Selectable(items[n], is_selected))\
				{
					item_current = items[n];
					FullscreenDebugBuffer = (EDebugVisualization)n;
				}
				if (is_selected)
				{
					ImGui::SetItemDefaultFocus(); 
				}

			}
			ImGui::EndCombo();
		}

		// Rendering Mode selector
		{
			static ImGuiComboFlags flags = 0;
			const char* items[] = {
				"HYBRID (Raster + RT)",
				"PATH TRACING",
			};
			static const char* item_current = items[UINT(ERenderingMode::HYBRID)];
			if (ImGui::BeginCombo("Rendering Mode", item_current, flags))
			{
				for (int n = 0; n < IM_ARRAYSIZE(items); n++)
				{
					bool is_selected = (item_current == items[n]);
				if (ImGui::Selectable(items[n], is_selected))
				{
					item_current = items[n];
					RenderingMode = (ERenderingMode)n;
					
					// Reset frame counter when switching modes for path tracing accumulation
				if (RenderingMode == ERenderingMode::PATHTRACING)
				{
					FrameCounter = 0;
					PrevPathTracingViewMat = glm::mat4x4(0.0f); // Force camera change detection on first frame
					PrevPathTracingLightDir = glm::vec3(0.0f); // Force light change detection
					PrevPathTracingLightIntensity = 0.0f;
				}
				}
					if (is_selected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
		}

		// Path Tracing settings (only show when in path tracing mode)
		if (RenderingMode == ERenderingMode::PATHTRACING)
		{
			ImGui::Separator();
		ImGui::Text("Path Tracing Settings");
		ImGui::SliderInt("Max Bounces", (int*)&PathTracingViewParam.MaxBounces, 1, 8);
		ImGui::SliderInt("Samples Per Pixel", (int*)&PathTracingViewParam.SamplesPerPixel, 1, 16);
	ImGui::Text("Accumulated Frames: %u", FrameCounter);
if (ImGui::Button("Reset Accumulation"))
{
	FrameCounter = 0;
	PrevPathTracingViewMat = glm::mat4x4(0.0f);
	PrevPathTracingLightDir = glm::vec3(0.0f);
	PrevPathTracingLightIntensity = 0.0f;
}
		
		ImGui::Separator();
		ImGui::Text("Debug Visualization");
		const char* debugModes[] = { "None", "Albedo", "Normal", "Roughness", "Metallic", "World Position", "Barycentric" };
		static int debugMode = 0;
		if (ImGui::Combo("Debug Mode", &debugMode, debugModes, IM_ARRAYSIZE(debugModes)))
		{
			PathTracingViewParam.DebugMode = debugMode;
			FrameCounter = 0; // Reset accumulation when changing debug mode
			PrevPathTracingViewMat = glm::mat4x4(0.0f);
		}
		
		ImGui::Separator();
		}

		{
			static ImGuiComboFlags flags = 0;
			const char* items[] = {
				"LINEAR_TO_SRGB",
				"REINHARD",
				"FILMIC_ALU",
				"FILMIC_HABLE",
			};
			static const char* item_current = items[UINT(EToneMapMode::FILMIC_HABLE)];
			if (ImGui::BeginCombo("Tone Map Operator", item_current, flags))
			{
				for (int n = 0; n < IM_ARRAYSIZE(items); n++)
				{
					bool is_selected = (item_current == items[n]);
					if (ImGui::Selectable(items[n], is_selected))\
					{
						item_current = items[n];
						ToneMapMode = (EToneMapMode)n;
					}
					if (is_selected)
					{
						ImGui::SetItemDefaultFocus();
					}

				}
				ImGui::EndCombo();
			}

		}

		if (ToneMapMode == FILMIC_HABLE)
		{

			ImGui::SliderFloat("WhitePoint_Hejl", &ToneMapCB.WhitePoint_Hejl, 0.1f, 5.0f);

			ImGui::SliderFloat("ShoulderStrength", &ToneMapCB.ShoulderStrength, 0.1f, 10.0f);

			ImGui::SliderFloat("LinearStrength", &ToneMapCB.LinearStrength, 0.1f, 10.0f);

			ImGui::SliderFloat("LinearAngle", &ToneMapCB.LinearAngle, 0.1f, 20.0f);

			ImGui::SliderFloat("ToeStrength", &ToneMapCB.ToeStrength, 0.1f, 20.0f);

			ImGui::SliderFloat("WhitePoint_Hable", &ToneMapCB.WhitePoint_Hable, 0.1f, 20.0f);
		}

		// ImGui::gizmo3D has memory leak.
		glm::vec3 LD = glm::vec3(LightDir.z, -LightDir.y, -LightDir.x);
		ImGui::gizmo3D("##gizmo1", LD, 200 /* mode */);
		LightDir = glm::vec3(-LD.z, -LD.y, LD.x);
		ImGui::SameLine();
		ImGui::Text("Light Direction");


		ImGui::SliderFloat("Light Brightness", &LightIntensity, 0.0f, 20.0f);

		ImGui::Separator();
		ImGui::Text("Sky Settings (Path Tracing)");
		ImGui::ColorEdit3("Sky Color Top", &SkyColorTop.x);
		ImGui::ColorEdit3("Sky Color Bottom", &SkyColorBottom.x);
		ImGui::SliderFloat("Sky Intensity", &SkyIntensity, 0.0f, 10.0f);
		ImGui::Separator();

		ImGui::SliderFloat("SponzaRoughness multiplier", &SponzaRoughnessMultiplier, 0.0f, 1.0f);
		ImGui::SliderFloat("ShaderBallRoughness multiplier", &ShaderBallRoughnessMultiplier, 0.0f, 1.0f);


		ImGui::SliderFloat("IndirectDiffuse Depth Weight Factor", &SpatialFilterCB.IndirectDiffuseWeightFactorDepth, 0.0f, 20.0f);
		ImGui::SliderFloat("IndirectDiffuse Normal Weight Factor", &SpatialFilterCB.IndirectDiffuseWeightFactorNormal, 0.0f, 20.0f);

		ImGui::SliderFloat("TemporalValidParams.x", &TemporalFilterCB.TemporalValidParams.x, 0.0f, 128);

		ImGui::SliderFloat("BloomSigma", &BloomSigma, 0.0f, 2.0f);

		ImGui::SliderFloat("BloomThreshHold", &BloomCB.BloomThreshHold, 0.0f, 2.0f);

		ImGui::SliderFloat("BloomStrength", &BloomStrength, 0.0f, 4.0f);

		ImGui::SliderFloat("TargetLuminance", &AdaptExposureCB.TargetLuminance, 0.001f, 0.990f);

		ImGui::SliderFloat("AdaptationRate", &AdaptExposureCB.AdaptationRate, 0.01f, 1.0f);

		ImGui::SliderFloat("MinExposure", &AdaptExposureCB.MinExposure, -8.0f, 0.0f);
		ImGui::SliderFloat("MaxExposure", &AdaptExposureCB.MaxExposure, 0.0f, 8.0f);

		ImGui::SliderFloat("BayerRotScale", &TemporalFilterCB.BayerRotScale, 0.0f, 1.0f);

		ImGui::SliderFloat("SpecularBlurRadius", &TemporalFilterCB.SpecularBlurRadius, 0.0f, 5.0f);

		ImGui::SliderFloat("Point2PlaneDistScale", &TemporalFilterCB.Point2PlaneDistScale, 0.0f, 1000.0f);

	/*	AdaptExposureCB.TargetLuminance = 0.08;
		AdaptExposureCB.AdaptationRate = 0.05;
		AdaptExposureCB.MinExposure = 1.0f / 64.0f;
		AdaptExposureCB.MaxExposure = 64.0f;
		*/
		if (!renderBackend->GetErrorString().empty())
		{
			if (!ImGui::IsPopupOpen("Msg"))
			{
				ImGui::SetNextWindowSize(ImVec2(1200, 800));
				ImGui::OpenPopup("Msg");
			}

			if (ImGui::BeginPopupModal("Msg"))
			{
				ImGui::TextWrapped(renderBackend->GetErrorString().c_str());
			
				if (ImGui::Button("Compile again", ImVec2(120, 0)))
				{
					bRecompileShaders = true;
					renderBackend->ClearErrorString();
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Close", ImVec2(80, 0)))
				{
					renderBackend->ClearErrorString();
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		
		}
	
		ImGui::End();

		if (bDebugDraw && FullscreenDebugBuffer == EDebugVisualization::NO_FULLSCREEN)
		{
			ImGui::SetNextWindowBgAlpha(0.85f);
			ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
			ImGuiWindowFlags overlayFlags =
				ImGuiWindowFlags_NoDecoration |
				ImGuiWindowFlags_AlwaysAutoResize |
				ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoFocusOnAppearing |
				ImGuiWindowFlags_NoNav;

			if (ImGui::Begin("Buffer Tile Labels", nullptr, overlayFlags))
			{
				ImGui::Text("VisualizeBuffer Tile Labels");
				ImGui::Separator();
				ImGui::Text("Row1: SPEC_HISTORY_LENGTH | (empty) | SPATIAL_FILTERED_DIFFUSE_GI | FINAL_DIFFUSE_GI");
				ImGui::Text("Row2: TEMPORAL_FILTERED_SPECULAR | BLOOM | TEMPORAL_FILTERED_DIFFUSE_GI | ROUGNESS_METALLIC");
				ImGui::Text("Row3: SPECULAR_RAW | GEO_NORMAL | RAW_DIFFUSE_GI + RAW_DIFFUSE_GI_AUX | VELOCITY");
				ImGui::Text("Row4: SHADOW | WORLD_NORMAL | DEPTH | ALBEDO");
			}
			ImGui::End();
		}

		ImGui::Render();
		BeginGpuPassTiming(EGpuPass::ImGui);
		renderBackend->RenderImGuiDrawData(ImGui::GetDrawData());
		EndGpuPassTiming(EGpuPass::ImGui);

	}

	EndGpuPassTiming(EGpuPass::Frame);
	ResolveGpuTimingFrame();

	renderBackend->TransitionTexture(backbuffer, EResourceState::RenderTarget, EResourceState::Present);


	renderBackend->ExecuteCurrentCommandList();

	renderBackend->EndFrame();

	PrevViewProjMat = ViewProjMat;
	PrevViewMat = ViewMat;

	PrevUnjitteredViewProjMat = UnjitteredViewProjMat;
}

void Corona::OnDestroy()
{
	SaveCameraState();
	renderBackend->WaitForGpu();
	renderBackend->ShutdownGpuTimestampQueries();

#if WITH_STREAMLINE
	ShutdownStreamline();
#endif
	if (bImguiInitialized)
	{
		renderBackend->ShutdownImGuiBackend();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		bImguiInitialized = false;
	}
}

void Corona::OnKeyDown(UINT8 key)
{
	switch (key)
	{
	/*case 'M':
		bMultiThreadRendering = !bMultiThreadRendering;
		break;*/
	case 'B':
		bDebugDraw = !bDebugDraw;
		break;
	case 'T':
	{
		const EAntiAliasingMode PreviousMode = AntiAliasingMode;
		AntiAliasingMode = static_cast<EAntiAliasingMode>((static_cast<int>(AntiAliasingMode) + 1) % static_cast<int>(EAntiAliasingMode::COUNT));
#if WITH_STREAMLINE
		if (AntiAliasingMode == EAntiAliasingMode::DLSS_SR && !bDLSSAvailable)
			AntiAliasingMode = EAntiAliasingMode::DLSS_RR;
		if (AntiAliasingMode == EAntiAliasingMode::DLSS_RR && !bDLSSRRAvailable)
			AntiAliasingMode = EAntiAliasingMode::OFF;
#else
		if (AntiAliasingMode == EAntiAliasingMode::DLSS_SR || AntiAliasingMode == EAntiAliasingMode::DLSS_RR)
			AntiAliasingMode = EAntiAliasingMode::OFF;
#endif
		ResetAllAccumulationState(IsDLSSMode(PreviousMode) || IsDLSSMode(AntiAliasingMode));
		break;
	}
	case 'C':
		ClampMode++;
		ClampMode = ClampMode % 3;
		break;
	case 'R':
		RecompileShaders();
		break;
	case 'I':
		bShowImgui = !bShowImgui;
		break;
	default:
		break;
	}

	m_camera.OnKeyDown(key);
}

void Corona::OnKeyUp(UINT8 key)
{
	m_camera.OnKeyUp(key);
}

void Corona::OnRButtonDown(int x, int y)
{
	m_camera.OnMouseDown(x, y);
}

void Corona::OnRButtonUp()
{
	m_camera.OnMouseUp();
}

void Corona::OnMouseMove(int x, int y)
{
	m_camera.OnMouseMove(x, y);
}

struct ParallelDrawTaskSet : enki::ITaskSet
{
	Corona* app;
	UINT StartIndex;
	UINT ThisDraw;
	UINT ThreadIndex;
	//ThreadDescriptorHeapPool* DHPool;

	ParallelDrawTaskSet(){}
	ParallelDrawTaskSet(ParallelDrawTaskSet &&) {}
	ParallelDrawTaskSet(const ParallelDrawTaskSet&) = delete;

	virtual void ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum)
	{
		//app->RecordDraw(StartIndex, ThisDraw, ThreadIndex, const_cast<ThreadDescriptorHeapPool*>(DHPool));
	}
};

void Corona::DrawScene(shared_ptr<Scene> scene, float Roughness, float Metalic, bool bOverrideRoughnessMetallic)
{
	for (auto& mesh : scene->meshes)
	{
		renderBackend->BindMeshBuffers(mesh->Vb.get(), mesh->Ib.get());

		for (int i = 0; i < mesh->Draws.size(); i++)
		{
			Mesh::DrawCall& drawcall = mesh->Draws[i];
			GBufferConstantBuffer objCB;
			int sizea = sizeof(GBufferConstantBuffer);

			objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
			objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);

			//glm::mat4 m; // Identity matrix
			objCB.WorldMatrix = glm::transpose(mesh->transform);

			objCB.UnjitteredViewProjMat = glm::transpose(UnjitteredViewProjMat);
			objCB.PrevUnjitteredViewProjMat = glm::transpose(PrevUnjitteredViewProjMat);
			objCB.ViewDir.x = m_camera.m_lookDirection.x;
			objCB.ViewDir.y = m_camera.m_lookDirection.y;
			objCB.ViewDir.z = m_camera.m_lookDirection.z;

			objCB.RTSize.x = GetRenderWidth();
			objCB.RTSize.y = GetRenderHeight();

			objCB.RougnessMetalic.x = Roughness;
			objCB.RougnessMetalic.y = Metalic;

			objCB.bOverrideRougnessMetallic = bOverrideRoughnessMetallic ? 1 : 0;

			GBufferPassPSO->SetCBVValue("GBufferConstantBuffer", (void*)&objCB);

			Texture* AlbedoTex = drawcall.mat->Diffuse.get();
			if (AlbedoTex)
				GBufferPassPSO->SetSRV("AlbedoTex", AlbedoTex->GpuHandleSRV);

			Texture* NormalTex = drawcall.mat->Normal.get();
			if (NormalTex)
				GBufferPassPSO->SetSRV("NormalTex", NormalTex->GpuHandleSRV);

			Texture* RoughnessTex = drawcall.mat->Roughness.get();
			if (RoughnessTex)
				GBufferPassPSO->SetSRV("RoughnessTex", RoughnessTex->GpuHandleSRV);

			Texture* MetallicTex = drawcall.mat->Metallic.get();
			if (MetallicTex)
				GBufferPassPSO->SetSRV("MetallicTex", MetallicTex->GpuHandleSRV);


			renderBackend->DrawIndexed(drawcall.IndexCount, drawcall.IndexStart, drawcall.VertexBase);
		}
	}
}

void Corona::GBufferPass()
{
	ColorBufferWriteIndex = 1 - ColorBufferWriteIndex;
	//DepthBufferWriteIndex = 1 - DepthBufferWriteIndex;
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("GBufferPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "GBufferPass");

	renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(GeomNormalBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::ShaderRead, EResourceState::DepthWrite);
	renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	renderBackend->ClearRenderTarget(AlbedoBuffer.get(), clearColor);
	renderBackend->ClearRenderTarget(SpecularAlbedoBuffer.get(), clearColor);
	const float normalClearColor[] = { 0.0f, -0.1f, 0.0f, 0.0f };
	renderBackend->ClearRenderTarget(NormalBuffers[ColorBufferWriteIndex].get(), normalClearColor);
	renderBackend->ClearRenderTarget(GeomNormalBuffer.get(), normalClearColor);
	const float velocityClearColor[] = { 0.0f, 0.0f};
	renderBackend->ClearRenderTarget(VelocityBuffer.get(), velocityClearColor);
	const float roughnessClearColor[] = { 0.001f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearRenderTarget(RoughnessMetalicBuffer.get(), roughnessClearColor);

	renderBackend->ClearDepth(DepthBuffer.get(), 1.0f);
	const float ujitteredDepthClearColor[] = { 1.0f, 1.0f, 1.0f, 1.0f};
	renderBackend->ClearRenderTarget(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), ujitteredDepthClearColor);


	renderBackend->BindDefaultDescriptorHeaps();

	renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
	Texture* renderTargets[] = {
		AlbedoBuffer.get(),
		SpecularAlbedoBuffer.get(),
		NormalBuffers[ColorBufferWriteIndex].get(),
		GeomNormalBuffer.get(),
		VelocityBuffer.get(),
		RoughnessMetalicBuffer.get(),
		UnjitteredDepthBuffers[ColorBufferWriteIndex].get()
	};
	renderBackend->SetRenderTargets(renderTargets, GBufferPassPSO->graphicsPSODesc.NumRenderTargets, DepthBuffer.get());

	GBufferPassPSO->Apply();
	
	GBufferPassPSO->SetSampler("samplerWrap", samplerWrap.get());

	if (!bMultiThreadRendering)
	{

		
		DrawScene(Sponza, SponzaRoughnessMultiplier, 0, false);
		//DrawScene(ShaderBall, ShaderBallRoughnessMultiplier, 1, true);
	}
	else
	{
		//UINT RemainDraw = mesh->Draws.size();
		//UINT NumDrawThread = mesh->Draws.size() / (NumThread);
		//UINT StartIndex = 0;

		//vector<ThreadDescriptorHeapPool> vecDHPool;
		//vecDHPool.resize(NumThread);

		//vector<ParallelDrawTaskSet> vecTask;
		//vecTask.resize(NumThread);

		//for (int i = 0; i < NumThread; i++)
		//{
		//	UINT ThisDraw = NumDrawThread;
		//	
		//	if (i == NumThread - 1)
		//		ThisDraw = RemainDraw;

		//	ThreadDescriptorHeapPool& DHPool = vecDHPool[i];
		//	DHPool.AllocPool(RS_Mesh->GetGraphicsBindingDHSize()*ThisDraw);

		//	// draw
		//	ParallelDrawTaskSet& task = vecTask[i];
		//	task.app = this;
		//	task.StartIndex = StartIndex;
		//	task.ThisDraw = ThisDraw;
		//	task.ThreadIndex = i;
		//	task.DHPool = &DHPool;

		//	g_TS.AddTaskSetToPipe(&task);

		//	RemainDraw -= ThisDraw;
		//	StartIndex += ThisDraw;
		//}

		//g_TS.WaitforAll();
	}
	
	renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(GeomNormalBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);


	renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::DepthWrite, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
}

void Corona::SpatialDenoisingPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("SpatialDenoisingPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "SpatialDenoisingPass");

	UINT WriteIndex = 0;
	UINT ReadIndex = 1;
	for (int i = 0; i < 4; i++)
	{
		WriteIndex = 1 - WriteIndex; // 1
		ReadIndex = 1 - WriteIndex; // 0

		renderBackend->TransitionTexture(DiffuseGISpatialAux[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(DiffuseGISpatial[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(SpecularGISpatial[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

		SpatialDenoisingFilterPSO->Apply();

		SpatialDenoisingFilterPSO->SetSRV("DepthTex", DepthBuffer->GpuHandleSRV);
		SpatialDenoisingFilterPSO->SetSRV("GeoNormalTex", GeomNormalBuffer->GpuHandleSRV);
		if (i == 0)
		{
			SpatialDenoisingFilterPSO->SetSRV("InGIResultSHTex", DiffuseGITemporalAux[GIBufferWriteIndex]->GpuHandleSRV);
			SpatialDenoisingFilterPSO->SetSRV("InGIResultColorTex", DiffuseGITemporal[GIBufferWriteIndex]->GpuHandleSRV);
			SpatialDenoisingFilterPSO->SetSRV("InSpecularGITex", SpecularGITemporal[GIBufferWriteIndex]->GpuHandleSRV);
		}
		else
		{
			SpatialDenoisingFilterPSO->SetSRV("InGIResultSHTex", DiffuseGISpatialAux[ReadIndex]->GpuHandleSRV);
			SpatialDenoisingFilterPSO->SetSRV("InGIResultColorTex", DiffuseGISpatial[ReadIndex]->GpuHandleSRV);
			SpatialDenoisingFilterPSO->SetSRV("InSpecularGITex", SpecularGISpatial[ReadIndex]->GpuHandleSRV);
		}


		SpatialDenoisingFilterPSO->SetUAV("OutGIResultSH", DiffuseGISpatialAux[WriteIndex]->GpuHandleUAV);
		SpatialDenoisingFilterPSO->SetUAV("OutGIResultColor", DiffuseGISpatial[WriteIndex]->GpuHandleUAV);
		SpatialDenoisingFilterPSO->SetUAV("OutSpecularGI", SpecularGISpatial[WriteIndex]->GpuHandleUAV);

		SpatialFilterCB.Iteration = i;
		SpatialDenoisingFilterPSO->SetCBVValue("SpatialFilterConstant", &SpatialFilterCB);

		UINT WidthGI = GetRenderWidth();
		UINT HeightGI = GetRenderHeight();

		renderBackend->Dispatch((WidthGI + 31) / 32, (HeightGI + 31) / 32, 1);

		renderBackend->TransitionTexture(DiffuseGISpatialAux[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(DiffuseGISpatial[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(SpecularGISpatial[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	}
}

void Corona::TemporalDenoisingPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("TemporalDenoisingPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalDenoisingPass");

	// GIBufferSH : full scale
	// FilterIndirectDiffusePingPongSH : 3x3 downsample
	GIBufferWriteIndex = 1 - GIBufferWriteIndex;
	UINT WriteIndex = GIBufferWriteIndex;
	UINT ReadIndex = 1 - WriteIndex;

	// first pass
	renderBackend->TransitionTexture(DiffuseGISpatialAux[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGISpatial[0].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGITemporalAux[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGITemporal[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(SpecularGITemporal[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);


	TemporalDenoisingFilterPSO->Apply();

	TemporalDenoisingFilterPSO->SetSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("NormalTex", NormalBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("InGIResultSHTex", DiffuseGIRawAux->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("InGIResultColorTex", DiffuseGIRaw->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("InGIResultSHTexPrev", DiffuseGITemporalAux[ReadIndex]->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("InGIResultColorTexPrev", DiffuseGITemporal[ReadIndex]->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("VelocityTex", VelocityBuffer->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("InSpecularGITex", SpecularGIRaw->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("InSpecularGITexPrev", SpecularGITemporal[ReadIndex]->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("RougnessMetalicTex", RoughnessMetalicBuffer->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("PrevDepthTex", UnjitteredDepthBuffers[1- ColorBufferWriteIndex]->GpuHandleSRV);
	TemporalDenoisingFilterPSO->SetSRV("PrevNormalTex", NormalBuffers[1 - ColorBufferWriteIndex]->GpuHandleSRV);


	TemporalDenoisingFilterPSO->SetUAV("OutGIResultSH", DiffuseGITemporalAux[WriteIndex]->GpuHandleUAV);
	TemporalDenoisingFilterPSO->SetUAV("OutGIResultColor", DiffuseGITemporal[WriteIndex]->GpuHandleUAV);
	TemporalDenoisingFilterPSO->SetUAV("OutGIResultSHDS", DiffuseGISpatialAux[0]->GpuHandleUAV);
	TemporalDenoisingFilterPSO->SetUAV("OutGIResultColorDS", DiffuseGISpatial[0]->GpuHandleUAV);
	TemporalDenoisingFilterPSO->SetUAV("OutSpecularGI", SpecularGITemporal[WriteIndex]->GpuHandleUAV);
	//TemporalDenoisingFilterPSO->SetUAV("OutSpecularGIDS", SpecularGISpatial[0]->GpuHandleUAV, renderBackend->GetGraphicsCommandList());

	TemporalDenoisingFilterPSO->SetSampler("BilinearClamp", samplerBilinearWrap.get());
	TemporalFilterCB.HistoryValid = bTemporalDenoiserHistoryValid ? 1u : 0u;

	TemporalDenoisingFilterPSO->SetCBVValue("TemporalFilterConstant", &TemporalFilterCB);

	renderBackend->Dispatch((GetRenderWidth() + 14) / 15, (GetRenderHeight() + 14) / 15, 1);

	renderBackend->TransitionTexture(DiffuseGISpatialAux[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGISpatial[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGITemporalAux[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGITemporal[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(SpecularGITemporal[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	bTemporalDenoiserHistoryValid = true;
	IndirectAccumulatedFrames = std::min(IndirectAccumulatedFrames + 1u, 1024u);
}



// Helper function to add scene meshes to BLAS vector
void AddMeshesToBLAS(vector<shared_ptr<RTAS>>& vecBLAS, shared_ptr<Scene> scene)
{
	for (auto& mesh : scene->meshes)
	{
		shared_ptr<RTAS> blas = mesh->CreateBLAS();
		blas->mesh = mesh;
		if (blas == nullptr)
		{
			continue;
		}
		vecBLAS.push_back(blas);
	}
}

void Corona::RecompileShaders()
{
	renderBackend->WaitForGpu();
	bTemporalAAHistoryValid = false;
	bTemporalDenoiserHistoryValid = false;
	bPendingTemporalHistoryClear = true;
	IndirectAccumulatedFrames = 0;
	bResetTemporalStateNextUpdate = true;

	InitRTPSO();
	InitSpatialDenoisingPass();
	InitTemporalDenoisingPass();
	InitGBufferPass();
	InitToneMapPass();
	InitDebugPass();
	InitLightingPass();
	InitTemporalAAPass();
	InitBloomPass();
	//InitGenMipSpecularGIPass();
}

void Corona::UpdateInstancePropertyBuffer()
{
	// Map and update instance properties
	uint8_t* pData;
	InstancePropertyBuffer->resource->Map(0, nullptr, (void**)&pData);

	for (auto& m : vecBLAS)
	{
		glm::mat4x4 mat = glm::transpose(m->mesh->transform);
		memcpy(pData, &mat, sizeof(glm::mat4x4));
		pData += sizeof(InstanceProperty);
	}

	InstancePropertyBuffer->resource->Unmap(0, nullptr);
}

void Corona::RebuildAccelerationStructures()
{
	// Wait for GPU to finish using current structures
	renderBackend->WaitForGpu();
	
	// Recreate TLAS with current BLAS list
	TLAS = renderBackend->CreateTLAS(vecBLAS);
	
	// Update instance property buffer
	UpdateInstancePropertyBuffer();
}

void Corona::AddScene(shared_ptr<Scene> scene)
{
	// Add all meshes from scene to BLAS vector
	AddMeshesToBLAS(vecBLAS, scene);
	
	// Rebuild acceleration structures and update buffers
	RebuildAccelerationStructures();
}

void Corona::InitRaytracingData()
{
	UINT NumTotalMesh = Sponza->meshes.size();
	vecBLAS.reserve(NumTotalMesh);

	// Create initial instance property buffer (large enough for many instances)
	InstancePropertyBuffer = renderBackend->CreateBuffer({ 500u, sizeof(InstanceProperty), EInitialResourceState::GenericRead, false, nullptr });
	InstancePropertyBuffer->MakeByteAddressBufferSRV();
	NAME_D3D12_OBJECT(InstancePropertyBuffer->resource);

	// Add initial scene(s)
	AddScene(Sponza);
	//AddScene(ShaderBall);
}

void Corona::InitRTPSO()
{
	// create shadow rtpso
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_SHADOW = shared_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
		TEMP_PSO_RT_SHADOW->Owner = dx12_rhi;

		TEMP_PSO_RT_SHADOW->NumInstance = vecBLAS.size();// scene->meshes.size(); // important for cbv allocation & shadertable size.

		// new interface
		TEMP_PSO_RT_SHADOW->AddHitGroup("HitGroup", "", "anyhit");
		TEMP_PSO_RT_SHADOW->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

		TEMP_PSO_RT_SHADOW->BindUAV("global", "ShadowResult", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "WorldNormalTex", 2);

		TEMP_PSO_RT_SHADOW->BindCBV("global", "ViewParameter", 0, sizeof(RTShadowViewParamCB), 1);
		TEMP_PSO_RT_SHADOW->BindSampler("global", "samplerWrap", 0);



		TEMP_PSO_RT_SHADOW->AddShader("miss", RTPipelineStateObject::MISS);
		
		TEMP_PSO_RT_SHADOW->AddShader("anyhit", RTPipelineStateObject::ANYHIT);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "vertices", 3);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "indices", 4);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "AlbedoTex", 5);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "InstanceProperty", 6);

		TEMP_PSO_RT_SHADOW->MaxRecursion = 1;
		TEMP_PSO_RT_SHADOW->MaxAttributeSizeInBytes = sizeof(float) * 2;
		TEMP_PSO_RT_SHADOW->MaxPayloadSizeInBytes = sizeof(float) * 2;

		bool bSuccess = TEMP_PSO_RT_SHADOW->InitRS("Shaders\\RaytracedShadow.hlsl");
		if (bSuccess)
		{
			PSO_RT_SHADOW = TEMP_PSO_RT_SHADOW;
		}
	}

	// create reflection rtpso
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_REFLECTION = shared_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
		TEMP_PSO_RT_REFLECTION->Owner = dx12_rhi;
		TEMP_PSO_RT_REFLECTION->NumInstance = vecBLAS.size();// scene->meshes.size();

		TEMP_PSO_RT_REFLECTION->AddHitGroup("HitGroup", "chs", "");
		//TEMP_PSO_RT_REFLECTION->AddHitGroup("ShadowHitGroup", "chsShadow", "");


		TEMP_PSO_RT_REFLECTION->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
		
		TEMP_PSO_RT_REFLECTION->BindUAV("global", "ReflectionResult", 0);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "GeoNormalTex", 2);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "RougnessMetallicTex", 6);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "BlueNoiseTex", 7);
		TEMP_PSO_RT_REFLECTION->BindSRV("global", "WorldNormalTex", 8);


		TEMP_PSO_RT_REFLECTION->BindCBV("global", "ViewParameter", 0, sizeof(RTReflectionViewParam), 1);
		TEMP_PSO_RT_REFLECTION->BindSampler("global", "samplerWrap", 0);

		TEMP_PSO_RT_REFLECTION->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_REFLECTION->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO_RT_REFLECTION->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "AlbedoTex", 5);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "InstanceProperty", 9);

		TEMP_PSO_RT_REFLECTION->MaxRecursion = 1;
		TEMP_PSO_RT_REFLECTION->MaxAttributeSizeInBytes = sizeof(float) * 2;
		TEMP_PSO_RT_REFLECTION->MaxPayloadSizeInBytes = sizeof(float) * 13;


		bool bSuccess = TEMP_PSO_RT_REFLECTION->InitRS("Shaders\\RaytracedReflection.hlsl");

		if (bSuccess)
		{
			PSO_RT_REFLECTION = TEMP_PSO_RT_REFLECTION;
		}
	}
	// gi rtpso
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_GI = shared_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
		TEMP_PSO_RT_GI->Owner = dx12_rhi;
		TEMP_PSO_RT_GI->NumInstance = vecBLAS.size();// scene->meshes.size();

		TEMP_PSO_RT_GI->AddHitGroup("HitGroup", "chs", "");


		TEMP_PSO_RT_GI->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
		
		TEMP_PSO_RT_GI->BindUAV("global", "GIResultSH", 0);
		TEMP_PSO_RT_GI->BindUAV("global", "GIResultColor", 1);
		TEMP_PSO_RT_GI->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_GI->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_GI->BindSRV("global", "WorldNormalTex", 2);
		TEMP_PSO_RT_GI->BindCBV("global", "ViewParameter", 0, sizeof(RTGIViewParam), 1);
		TEMP_PSO_RT_GI->BindSampler("global", "samplerWrap", 0);
		TEMP_PSO_RT_GI->BindSRV("global", "BlueNoiseTex", 7);

		TEMP_PSO_RT_GI->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_GI->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO_RT_GI->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_GI->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_GI->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_GI->BindSRV("chs", "AlbedoTex", 5);
		TEMP_PSO_RT_GI->BindSRV("chs", "InstanceProperty", 6);

		TEMP_PSO_RT_GI->MaxRecursion = 1;
		TEMP_PSO_RT_GI->MaxAttributeSizeInBytes = sizeof(float) * 2;

		TEMP_PSO_RT_GI->MaxPayloadSizeInBytes = sizeof(float) * 12;

		bool bSuccess = TEMP_PSO_RT_GI->InitRS("Shaders\\RaytracedGI.hlsl");

		if (bSuccess)
		{
			PSO_RT_GI = TEMP_PSO_RT_GI;
		}
	}
}

void Corona::InitPathTracingPass()
{
	shared_ptr<RTPipelineStateObject> TEMP_PSO_PATH_TRACING = shared_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
	TEMP_PSO_PATH_TRACING->Owner = dx12_rhi;
	TEMP_PSO_PATH_TRACING->NumInstance = vecBLAS.size();

	TEMP_PSO_PATH_TRACING->AddHitGroup("HitGroup", "PathTracingClosestHit", "PathTracingAnyHit");

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingRayGen", RTPipelineStateObject::RAYGEN);
	
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutputColor", 0);
	TEMP_PSO_PATH_TRACING->BindSRV("global", "gRtScene", 0);
	TEMP_PSO_PATH_TRACING->BindCBV("global", "ViewParameter", 0, sizeof(PathTracingViewParam), 1);
	TEMP_PSO_PATH_TRACING->BindSampler("global", "samplerWrap", 0);
	TEMP_PSO_PATH_TRACING->BindSRV("global", "BlueNoiseTex", 4);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingMiss", RTPipelineStateObject::MISS);
	TEMP_PSO_PATH_TRACING->AddShader("ShadowMiss", RTPipelineStateObject::MISS);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingClosestHit", RTPipelineStateObject::HIT);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "vertices", 1);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "indices", 2);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "InstanceProperty", 3);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "AlbedoTex", 5);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "NormalTex", 6);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "RoughnessTex", 7);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingClosestHit", "MetallicTex", 8);

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingAnyHit", RTPipelineStateObject::HIT);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "vertices", 1);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "indices", 2);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "InstanceProperty", 3);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "AlbedoTex", 5);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "NormalTex", 6);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "RoughnessTex", 7);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "MetallicTex", 8);


	TEMP_PSO_PATH_TRACING->MaxRecursion = 8;  // Support multiple bounces
	TEMP_PSO_PATH_TRACING->MaxAttributeSizeInBytes = sizeof(float) * 2;
	TEMP_PSO_PATH_TRACING->MaxPayloadSizeInBytes = 140; // PathTracingPayload: 3*float3 + 3*float3 + 1*float3 + 1*float3 + 1*float3 + 1*float2 + 3*uint + 2*float + 1*bool = 132+ bytes

	bool bSuccess = TEMP_PSO_PATH_TRACING->InitRS("Shaders\\PathTracing.hlsl");

	if (bSuccess)
	{
		PSO_PATH_TRACING = TEMP_PSO_PATH_TRACING;
	}
}

vector<UINT64> ResourceInt64array(ComPtr<ID3D12Resource> resource, int size)
{
	uint8_t* pData;
	HRESULT hr = resource->Map(0, nullptr, (void**)&pData);

	int size64 = size / sizeof(UINT64);
	vector<UINT64> mem;
	for (int i = 0; i < size64; i++)
	{
		UINT64 v = *(UINT64*)(pData + i * sizeof(UINT64));
		mem.push_back(v);
	}

	return mem;
}
void Corona::RaytraceShadowPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("RaytraceShadowPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand()%255, rand() % 255, rand() % 255), "RaytraceShadowPass");

	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	PSO_RT_SHADOW->NumInstance = vecBLAS.size();

	PSO_RT_SHADOW->BeginShaderTable();

	int i = 0;
	for (auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();

		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		PSO_RT_SHADOW->ResetHitProgram(i);
		PSO_RT_SHADOW->StartHitProgram("HitGroup", i);

		PSO_RT_SHADOW->AddDescriptor2HitProgram("HitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_SHADOW->AddDescriptor2HitProgram("HitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_SHADOW->AddDescriptor2HitProgram("HitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_SHADOW->AddDescriptor2HitProgram("HitGroup", InstancePropertyBuffer->GpuHandleSRV, i);

		i++;
	}

	PSO_RT_SHADOW->SetUAV("global", "ShadowResult", ShadowBuffer->GpuHandleUAV);
	PSO_RT_SHADOW->SetSRV("global", "gRtScene", TLAS->GPUHandle);
	PSO_RT_SHADOW->SetSRV("global", "DepthTex", DepthBuffer->GpuHandleSRV);
	PSO_RT_SHADOW->SetSRV("global", "WorldNormalTex", GeomNormalBuffer->GpuHandleSRV);
	PSO_RT_SHADOW->SetCBVValue("global", "ViewParameter", &RTShadowViewParam);
	PSO_RT_SHADOW->SetSampler("global", "samplerWrap", samplerWrap.get());


	PSO_RT_SHADOW->EndShaderTable();

	PSO_RT_SHADOW->Apply(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}

void Corona::ShadowDenoisePass()
{
	if (!ShadowDenoisePSO || !ShadowBuffer || !ShadowDenoisedBuffer)
		return;

	renderBackend->TransitionTexture(ShadowDenoisedBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	ShadowDenoisePSO->Apply();
	ShadowDenoisePSO->SetSRV("ShadowTex", ShadowBuffer->GpuHandleSRV);
	ShadowDenoisePSO->SetSRV("DepthTex", DepthBuffer->GpuHandleSRV);
	ShadowDenoisePSO->SetSRV("GeoNormalTex", GeomNormalBuffer->GpuHandleSRV);
	ShadowDenoiseParam.ProjectionParams = RTShadowViewParam.ProjectionParams;
	ShadowDenoiseParam.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	ShadowDenoisePSO->SetCBVValue("ShadowDenoiseCB", &ShadowDenoiseParam);
	ShadowDenoisePSO->SetUAV("OutShadow", ShadowDenoisedBuffer->GpuHandleUAV);
	renderBackend->Dispatch((GetRenderWidth() + 7) / 8, (GetRenderHeight() + 7) / 8, 1);

	renderBackend->TransitionTexture(ShadowDenoisedBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}

void Corona::RaytraceReflectionPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("RaytraceReflectionPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceReflectionPass");

	renderBackend->TransitionTexture(SpecularGIRaw.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	const FLOAT clearReflection[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearTextureUAVFloat(SpecularGIRaw.get(), clearReflection);

	PSO_RT_REFLECTION->NumInstance = vecBLAS.size();
	PSO_RT_REFLECTION->BeginShaderTable();

	PSO_RT_REFLECTION->SetUAV("global", "ReflectionResult", SpecularGIRaw->GpuHandleUAV);
	PSO_RT_REFLECTION->SetSRV("global", "gRtScene", TLAS->GPUHandle);
	PSO_RT_REFLECTION->SetSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("global", "GeoNormalTex", GeomNormalBuffer->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("global", "RougnessMetallicTex", RoughnessMetalicBuffer->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("global", "BlueNoiseTex", BlueNoiseTex->GpuHandleSRV);
	PSO_RT_REFLECTION->SetSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex]->GpuHandleSRV);

	RTReflectionViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5) / (0.5f * GetRenderHeight());
	PSO_RT_REFLECTION->SetCBVValue("global", "ViewParameter", &RTReflectionViewParam);
	PSO_RT_REFLECTION->SetSampler("global", "samplerWrap", samplerWrap.get());


	int i = 0;
	for(auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();

		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();
		PSO_RT_REFLECTION->ResetHitProgram(i);

		PSO_RT_REFLECTION->StartHitProgram("HitGroup", i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("HitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("HitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("HitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("HitGroup", InstancePropertyBuffer->GpuHandleSRV, i);

		//PSO_RT_REFLECTION->StartHitProgram("ShadowHitGroup", i);
		/*
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", InstancePropertyBuffer->GpuHandleSRV, i);*/
		i++;
	}

	PSO_RT_REFLECTION->EndShaderTable();

	PSO_RT_REFLECTION->Apply(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(SpecularGIRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	//PIXEndEvent();
}

void Corona::RaytraceGIPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("RaytraceGIPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceGIPass");

	renderBackend->TransitionTexture(DiffuseGIRawAux.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGIRaw.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	const FLOAT clearGI[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearTextureUAVFloat(DiffuseGIRawAux.get(), clearGI);
	renderBackend->ClearTextureUAVFloat(DiffuseGIRaw.get(), clearGI);

	PSO_RT_GI->NumInstance = vecBLAS.size();
	PSO_RT_GI->BeginShaderTable();

	PSO_RT_GI->SetUAV("global", "GIResultSH", DiffuseGIRawAux->GpuHandleUAV);
	PSO_RT_GI->SetUAV("global", "GIResultColor", DiffuseGIRaw->GpuHandleUAV);
	PSO_RT_GI->SetSRV("global", "gRtScene", TLAS->GPUHandle);
	PSO_RT_GI->SetSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
	PSO_RT_GI->SetSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex]->GpuHandleSRV);
	PSO_RT_GI->SetSRV("global", "BlueNoiseTex", BlueNoiseTex->GpuHandleSRV);
	
	RTGIViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5) / (0.5f * GetRenderHeight());
	PSO_RT_GI->SetCBVValue("global", "ViewParameter", &RTGIViewParam);
	PSO_RT_GI->SetSampler("global", "samplerWrap", samplerWrap.get());

	int i = 0;
	for(auto&as : vecBLAS)
	{
		auto& mesh = as->mesh;
		
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		PSO_RT_GI->ResetHitProgram(i);

		PSO_RT_GI->StartHitProgram("HitGroup", i);
		PSO_RT_GI->AddDescriptor2HitProgram("HitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("HitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("HitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("HitGroup", InstancePropertyBuffer->GpuHandleSRV, i);

		/*PSO_RT_GI->StartHitProgram("ShadowHitGroup", i);
		PSO_RT_GI->AddDescriptor2HitProgram("ShadowHitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("ShadowHitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("ShadowHitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_GI->AddDescriptor2HitProgram("ShadowHitGroup", InstancePropertyBuffer->GpuHandleSRV, i);*/

		i++;
	}

	PSO_RT_GI->EndShaderTable();


	PSO_RT_GI->Apply(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(DiffuseGIRawAux.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGIRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}

void Corona::PathTracingPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("PathTracingPass");
#endif
	PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "PathTracingPass");

	// Transition output buffer to UAV
	renderBackend->TransitionTexture(PathTracingAccumBuffer[PathTracingWriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	if (!PSO_PATH_TRACING)
	{
		InitPathTracingPass();
	}

	// Check if camera or light changed and reset accumulation
	bool cameraChanged = false;
	for (int i = 0; i < 4 && !cameraChanged; i++)
	{
		for (int j = 0; j < 4 && !cameraChanged; j++)
		{
			if (abs(PrevPathTracingViewMat[i][j] - ViewMat[i][j]) > 0.0001f)
			{
				cameraChanged = true;
			}
		}
	}
	
	// Check if light direction or intensity changed
	glm::vec3 currentLightDir = glm::normalize(LightDir);
	bool lightDirChanged = glm::length(currentLightDir - PrevPathTracingLightDir) > 0.0001f;
	bool lightIntensityChanged = abs(LightIntensity - PrevPathTracingLightIntensity) > 0.0001f;
	
	// Check if sky color changed
	bool skyColorChanged = glm::length(SkyColorTop - PrevSkyColorTop) > 0.0001f ||
	                       glm::length(SkyColorBottom - PrevSkyColorBottom) > 0.0001f ||
	                       abs(SkyIntensity - PrevSkyIntensity) > 0.0001f;
	
	if (cameraChanged || lightDirChanged || lightIntensityChanged || skyColorChanged)
	{
		FrameCounter = 0;
		PrevPathTracingViewMat = ViewMat;
		PrevPathTracingLightDir = currentLightDir;
		PrevPathTracingLightIntensity = LightIntensity;
		PrevSkyColorTop = SkyColorTop;
		PrevSkyColorBottom = SkyColorBottom;
		PrevSkyIntensity = SkyIntensity;
		
		// Note: Buffer will be cleared in shader when FrameCounter == 0
	}

	PSO_PATH_TRACING->NumInstance = vecBLAS.size();
	PSO_PATH_TRACING->BeginShaderTable();

	PSO_PATH_TRACING->SetUAV("global", "OutputColor", PathTracingAccumBuffer[PathTracingWriteIndex]->GpuHandleUAV);
	PSO_PATH_TRACING->SetSRV("global", "gRtScene", TLAS->GPUHandle);
	PSO_PATH_TRACING->SetSRV("global", "BlueNoiseTex", BlueNoiseTex->GpuHandleSRV);
	
	// PathTracingViewParam is already updated in OnUpdate()
	PSO_PATH_TRACING->SetCBVValue("global", "ViewParameter", &PathTracingViewParam);
	PSO_PATH_TRACING->SetSampler("global", "samplerWrap", samplerWrap.get());

	int i = 0;
	for(auto& as : vecBLAS)
	{
		auto& mesh = as->mesh;
		
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();
		
		Texture* normalTex = mesh->Draws[0].mat->Normal.get();
		if (!normalTex)
			normalTex = DefaultNormalTex.get();
		
		Texture* roughnessTex = mesh->Draws[0].mat->Roughness.get();
		if (!roughnessTex)
			roughnessTex = DefaultRougnessTex.get();
		
		Texture* metallicTex = mesh->Draws[0].mat->Metallic.get();
		if (!metallicTex)
			metallicTex = DefaultBlackTex.get();

		PSO_PATH_TRACING->ResetHitProgram(i);

		PSO_PATH_TRACING->StartHitProgram("HitGroup", i);
		PSO_PATH_TRACING->AddDescriptor2HitProgram("HitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_PATH_TRACING->AddDescriptor2HitProgram("HitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_PATH_TRACING->AddDescriptor2HitProgram("HitGroup", InstancePropertyBuffer->GpuHandleSRV, i);
		PSO_PATH_TRACING->AddDescriptor2HitProgram("HitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_PATH_TRACING->AddDescriptor2HitProgram("HitGroup", normalTex->GpuHandleSRV, i);
		PSO_PATH_TRACING->AddDescriptor2HitProgram("HitGroup", roughnessTex->GpuHandleSRV, i);
		PSO_PATH_TRACING->AddDescriptor2HitProgram("HitGroup", metallicTex->GpuHandleSRV, i);

		i++;
	}

	PSO_PATH_TRACING->EndShaderTable();

	PSO_PATH_TRACING->Apply(m_width, m_height);

	// Transition output buffer back to SRV
	renderBackend->TransitionTexture(PathTracingAccumBuffer[PathTracingWriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}
