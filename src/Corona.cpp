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
#include "VulkanBackend.h"
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

namespace
{
	constexpr double kCameraPathDumpFps = 30.0;
	constexpr double kCameraPathRecordMinIntervalSeconds = 1.0 / 120.0;

	void AppendVulkanRuntimeTrace(const std::wstring& line);

	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty())
			return {};

		const int valueLength = static_cast<int>(value.size());
		const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, value.data(), valueLength, nullptr, 0, nullptr, nullptr);
		if (requiredSize <= 0)
			return {};

		std::string result(static_cast<size_t>(requiredSize), '\0');
		WideCharToMultiByte(CP_UTF8, 0, value.data(), valueLength, result.data(), requiredSize, nullptr, nullptr);
		return result;
	}

	std::wstring Utf8ToWide(const std::string& value)
	{
		if (value.empty())
			return {};

		const int valueLength = static_cast<int>(value.size());
		const int requiredSize = MultiByteToWideChar(CP_UTF8, 0, value.data(), valueLength, nullptr, 0);
		if (requiredSize <= 0)
			return std::wstring(value.begin(), value.end());

		std::wstring result(static_cast<size_t>(requiredSize), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value.data(), valueLength, result.data(), requiredSize);
		return result;
	}

	std::wstring ReadTextFilePreview(const std::filesystem::path& filePath, size_t maxChars)
	{
		std::ifstream file(filePath, std::ios::binary);
		if (!file.is_open())
			return {};

		std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (text.size() > maxChars)
			text = text.substr(0, maxChars) + "\n...";
		return Utf8ToWide(text);
	}

	uint32_t GetHybridStageAutoDumpFrameCount(uint32_t phase, bool bVulkanHybridDump, bool bLimitedHybridStageDump)
	{
		if (!bVulkanHybridDump)
			return bLimitedHybridStageDump ? 1u : 8u;
		if (phase >= 7u)
			return 16u;
		if (phase >= 5u)
			return 8u;
		return 1u;
	}

	bool ShouldUseTAAForHybridStageAutoDump(uint32_t phase, bool bVulkanHybridDump)
	{
		return bVulkanHybridDump && phase >= 7u;
	}

}

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

	const std::filesystem::path kFramePerfLogPath =
		std::filesystem::path(L"C:\\dev\\Corona\\dumps\\fps_perf.log");

	double ElapsedMilliseconds(
		const std::chrono::steady_clock::time_point& begin,
		const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - begin).count();
	}

	const char* GetRenderingModeName(Corona::ERenderingMode mode)
	{
		switch (mode)
		{
		case Corona::ERenderingMode::HYBRID:
			return "HYBRID";
		case Corona::ERenderingMode::PATHTRACING:
			return "PATHTRACING";
		default:
			return "UNKNOWN";
		}
	}
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

void Corona::BeginFramePerfLogging()
{
	const auto now = CpuClock::now();
	if (!bFramePerfLogInitialized)
	{
		bFramePerfLogInitialized = true;
		FramePerfLogLastFlush = now;
		std::error_code createDirectoryError;
		std::filesystem::create_directories(kFramePerfLogPath.parent_path(), createDirectoryError);

		std::ofstream logFile(kFramePerfLogPath, std::ios::trunc);
		if (logFile.is_open())
		{
			logFile << "# Corona frame performance log. One row is an approximately one-second sample.\n";
			logFile << "sample,total_frames,backend,mode,timer_fps,avg_fps,avg_frame_ms,min_frame_ms,max_frame_ms,"
				"avg_begin_frame_ms,avg_record_ms,avg_execute_ms,avg_end_frame_ms";
			for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
			{
				logFile << ",cpu_" << GetGpuPassName(static_cast<EGpuPass>(passIndex)) << "_ms";
			}
			for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
			{
				logFile << ",gpu_" << GetGpuPassName(static_cast<EGpuPass>(passIndex)) << "_ms";
			}
			logFile << "\n";
		}
	}

	FramePerfLogFrameStart = now;
	CpuPassLastTimeMs.fill(0.0f);
	CpuPassActiveMask.fill(0);
}

void Corona::FinishFramePerfLogging(double beginFrameMs, double executeMs, double endFrameMs)
{
	if (!bFramePerfLogInitialized)
		return;

	const auto now = CpuClock::now();
	const double frameMs = ElapsedMilliseconds(FramePerfLogFrameStart, now);
	const double recordMs = std::max(0.0, frameMs - beginFrameMs - executeMs - endFrameMs);

	++FramePerfLogTotalFrameCount;
	++FramePerfLogSampleFrameCount;
	FramePerfLogAccumFrameMs += frameMs;
	FramePerfLogMinFrameMs = std::min(FramePerfLogMinFrameMs, frameMs);
	FramePerfLogMaxFrameMs = std::max(FramePerfLogMaxFrameMs, frameMs);
	FramePerfLogAccumBeginFrameMs += beginFrameMs;
	FramePerfLogAccumRecordMs += recordMs;
	FramePerfLogAccumExecuteMs += executeMs;
	FramePerfLogAccumEndFrameMs += endFrameMs;
	for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
	{
		// Divide by sample frame count when flushing so inactive passes naturally show as 0 cost.
		CpuPassAccumTimeMs[passIndex] += CpuPassLastTimeMs[passIndex];
	}

	const double elapsedSinceFlushSeconds =
		std::chrono::duration<double>(now - FramePerfLogLastFlush).count();
	if (elapsedSinceFlushSeconds < 1.0 && FramePerfLogSampleFrameCount < 120)
		return;

	const double sampleFrameCount = static_cast<double>(FramePerfLogSampleFrameCount);
	const double avgFrameMs = FramePerfLogAccumFrameMs / sampleFrameCount;
	const double avgFps = FramePerfLogAccumFrameMs > 0.0
		? (sampleFrameCount * 1000.0 / FramePerfLogAccumFrameMs)
		: 0.0;

	std::ofstream logFile(kFramePerfLogPath, std::ios::app);
	if (logFile.is_open())
	{
		logFile << std::fixed << std::setprecision(3)
			<< (FramePerfLogTotalFrameCount / std::max<UINT32>(1, FramePerfLogSampleFrameCount))
			<< "," << FramePerfLogTotalFrameCount
			<< "," << (renderBackend ? renderBackend->GetBackendName() : "None")
			<< "," << GetRenderingModeName(RenderingMode)
			<< "," << m_timer.GetFramesPerSecond()
			<< "," << avgFps
			<< "," << avgFrameMs
			<< "," << FramePerfLogMinFrameMs
			<< "," << FramePerfLogMaxFrameMs
			<< "," << (FramePerfLogAccumBeginFrameMs / sampleFrameCount)
			<< "," << (FramePerfLogAccumRecordMs / sampleFrameCount)
			<< "," << (FramePerfLogAccumExecuteMs / sampleFrameCount)
			<< "," << (FramePerfLogAccumEndFrameMs / sampleFrameCount);
		for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
		{
			logFile << "," << (CpuPassAccumTimeMs[passIndex] / sampleFrameCount);
		}
		for (UINT passIndex = 0; passIndex < GpuPassCount; ++passIndex)
		{
			logFile << "," << GpuPassAverageTimeMs[passIndex];
		}
		logFile << "\n";
	}

	FramePerfLogLastFlush = now;
	FramePerfLogSampleFrameCount = 0;
	FramePerfLogAccumFrameMs = 0.0;
	FramePerfLogMinFrameMs = 1.0e30;
	FramePerfLogMaxFrameMs = 0.0;
	FramePerfLogAccumBeginFrameMs = 0.0;
	FramePerfLogAccumRecordMs = 0.0;
	FramePerfLogAccumExecuteMs = 0.0;
	FramePerfLogAccumEndFrameMs = 0.0;
	CpuPassAccumTimeMs.fill(0.0);
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
	const UINT passIndex = static_cast<UINT>(pass);
	CpuPassActiveMask[passIndex] = 1;
	CpuPassStartTimes[passIndex] = CpuClock::now();

	if (!bGpuTimingResourcesInitialized)
		return;

	const UINT frameIndex = renderBackend->GetCurrentFrameIndex();
	GpuPassActiveMaskPerFrame[frameIndex][passIndex] = 1;

	const UINT queryIndex = frameIndex * GpuPassCount * GpuQueriesPerPass + passIndex * GpuQueriesPerPass;
	renderBackend->WriteGpuTimestamp(queryIndex);
}

void Corona::EndGpuPassTiming(EGpuPass pass)
{
	const UINT passIndex = static_cast<UINT>(pass);
	if (CpuPassActiveMask[passIndex])
	{
		CpuPassLastTimeMs[passIndex] =
			static_cast<float>(ElapsedMilliseconds(CpuPassStartTimes[passIndex], CpuClock::now()));
	}

	if (!bGpuTimingResourcesInitialized)
		return;

	const UINT frameIndex = renderBackend->GetCurrentFrameIndex();
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
	if (!renderBackend)
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
	auto AppendStartupTrace = [](const std::wstring& line)
	{
		const std::filesystem::path tracePath = std::filesystem::path(L"C:\\dev\\Corona\\dumps\\vulkan_runtime_trace.log");
		std::filesystem::create_directories(tracePath.parent_path());
		std::wofstream traceFile(tracePath, std::ios::app);
		if (traceFile.is_open())
			traceFile << line << L"\n";
	};
	AppendStartupTrace(L"[ParseCommandLineArgs] begin");

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
		if (arg == L"--no-imgui" || arg == L"--disable-imgui")
		{
			bCommandLineDisableImgui = true;
			bShowImgui = false;
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

		std::wstring backendValue = ParseValueArg(arg, L"--backend", L"-backend", i);
		if (!backendValue.empty())
		{
			bCommandLineRenderBackendOverrideSet = true;
			CommandLineRenderBackendAPI = (backendValue == L"vulkan" || backendValue == L"vk") ? ERenderBackendAPI::Vulkan : ERenderBackendAPI::D3D12;
			continue;
		}
	}
	AppendStartupTrace(
		L"[ParseCommandLineArgs] end backendOverride=" + std::to_wstring(bCommandLineRenderBackendOverrideSet ? 1 : 0) +
		L", backend=" + std::to_wstring(static_cast<int>(CommandLineRenderBackendAPI)) +
		L", renderOverride=" + std::to_wstring(bCommandLineRenderModeOverrideSet ? 1 : 0) +
		L", renderMode=" + std::to_wstring(static_cast<int>(CommandLineRenderingMode)) +
		L", autoDumpOverride=" + std::to_wstring(bCommandLineAutoDumpOverrideSet ? 1 : 0) +
		L", autoDump=" + std::to_wstring(bCommandLineAutoDumpEnabled ? 1 : 0) +
		L", noImgui=" + std::to_wstring(bCommandLineDisableImgui ? 1 : 0));
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
	StartupRenderBackendAPI = ERenderBackendAPI::D3D12;
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
	if (bCommandLineRenderBackendOverrideSet)
	{
		StartupRenderBackendAPI = CommandLineRenderBackendAPI;
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

	if (StartupRenderBackendAPI == ERenderBackendAPI::Vulkan && IsDLSSMode(StartupSelectedAAMode))
	{
		StartupSelectedAAMode = EAntiAliasingMode::TAA;
	}

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

	if (StartupRenderingMode == ERenderingMode::HYBRID)
	{
		const uint32_t maxSupportedHybridStage = renderBackend ? renderBackend->GetMaxSupportedHybridStage() : 7u;
		std::filesystem::path dumpDir = std::filesystem::path(L"C:\\dev\\Corona\\dumps\\hybrid_pass_stages");
		std::filesystem::create_directories(dumpDir);
		this->AutoAADumpDir = dumpDir.wstring();
		this->bAutoAADumpInitialized = true;
		this->bAutoAADumpCompleted = false;
		this->bHybridStageAutoDumpMode = true;
		this->AutoAADumpPhase = 0;
		this->AutoAADumpFramesInPhase = 0;

		std::filesystem::path logPath = std::filesystem::path(AutoAADumpDir) / L"dump_log.txt";
		std::error_code ec;
		std::filesystem::remove(logPath, ec);

		StartupRenderingMode = ERenderingMode::HYBRID;
		RenderingMode = StartupRenderingMode;
		AntiAliasingMode = EAntiAliasingMode::OFF;
		ResetAllAccumulationState(false);
		const bool bVulkanHybridDump =
			renderBackend &&
			renderBackend->GetAPI() == ERenderBackendAPI::Vulkan;
		const bool bLimitedHybridStageDump = maxSupportedHybridStage < 7u;
		if (maxSupportedHybridStage < 7u)
		{
			const wchar_t* backendName =
				!renderBackend ? L"unknown" :
				(renderBackend->GetAPI() == ERenderBackendAPI::Vulkan ? L"Vulkan" : L"D3D12");
			AppendAutoAADumpLog(
				std::wstring(L"[hybrid] stage capture limited by backend ") +
				std::wstring(backendName) +
				std::wstring(L" to stage_") + (maxSupportedHybridStage < 10 ? std::wstring(1, wchar_t(L'0' + maxSupportedHybridStage)) : std::to_wstring(maxSupportedHybridStage)));
		}
		AppendAutoAADumpLog(
			std::wstring(L"[") + GetHybridStageAutoDumpPhaseName(0) +
			L"] begin frames=" + std::to_wstring(GetHybridStageAutoDumpFrameCount(0u, bVulkanHybridDump, bLimitedHybridStageDump)) +
			L", aa=off");
		return;
	}

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
	this->bHybridStageAutoDumpMode = false;
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

bool Corona::IsHybridStageAutoDumpPhase() const
{
	return bHybridStageAutoDumpMode && bAutoAADumpEnabled && bAutoAADumpInitialized && !bAutoAADumpCompleted;
}

const wchar_t* Corona::GetHybridStageAutoDumpPhaseName(uint32_t phase) const
{
	switch (phase)
	{
	case 0: return L"stage_00_gbuffer_only";
	case 1: return L"stage_01_shadow_raw";
	case 2: return L"stage_02_shadow_denoise";
	case 3: return L"stage_03_reflection";
	case 4: return L"stage_04_gi_raw";
	case 5: return L"stage_05_gi_temporal";
	case 6: return L"stage_06_gi_spatial";
	case 7: return L"stage_07_lighting_tonemap";
	default: return L"stage_unknown";
	}
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

	if (IsHybridStageAutoDumpPhase())
	{
		const uint32_t maxSupportedHybridStage = renderBackend ? renderBackend->GetMaxSupportedHybridStage() : 7u;
		const bool bVulkanHybridDump =
			renderBackend &&
			renderBackend->GetAPI() == ERenderBackendAPI::Vulkan;
		const bool bLimitedHybridStageDump = maxSupportedHybridStage < 7u;
		const UINT32 kHybridStageDumpFrames = GetHybridStageAutoDumpFrameCount(AutoAADumpPhase, bVulkanHybridDump, bLimitedHybridStageDump);
		const UINT32 kNumHybridStages = std::min<uint32_t>(8u, maxSupportedHybridStage + 1u);
		if (AutoAADumpPhase >= kNumHybridStages)
			return;

		const wchar_t* currentPhaseName = GetHybridStageAutoDumpPhaseName(AutoAADumpPhase);
		const bool bLightingStage = AutoAADumpPhase >= 7;

		if (AutoAADumpFramesInPhase == kHybridStageDumpFrames - 1)
		{
			const std::wstring base = AutoAADumpDir + L"\\" + currentPhaseName;
			if (bVulkanHybridDump)
			{
				AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] screen png=requested");
			}
			else
			{
			auto dumpResource = [&](const wchar_t* suffix, Texture* texture, D3D12_RESOURCE_STATES state, bool dumpHdr)
			{
				if (!texture)
					return;
				const std::wstring fileBase = base + L"_" + suffix;
				bool hdrOk = true;
				if (dumpHdr)
					hdrOk = DumpTextureHDR(texture, fileBase + L".hdr", state);
				const bool pngOk = DumpTexturePNG(texture, fileBase + L"_preview.png", state);
				AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] " + suffix +
					L" hdr=" + (dumpHdr ? (hdrOk ? L"ok" : L"fail") : L"skip") +
					L", png=" + (pngOk ? L"ok" : L"fail"));
			};

			if (!bLimitedHybridStageDump)
			{
				dumpResource(L"gbuffer_albedo", AlbedoBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				dumpResource(L"gbuffer_world_normal", NormalBuffers[ColorBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				dumpResource(L"gbuffer_geo_normal", GeomNormalBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				dumpResource(L"gbuffer_velocity", VelocityBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				dumpResource(L"gbuffer_depth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				dumpResource(L"gbuffer_rm", RoughnessMetalicBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);

				if (AutoAADumpPhase >= 1)
					dumpResource(L"shadow_raw", ShadowBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				if (AutoAADumpPhase >= 2)
					dumpResource(L"shadow_denoised", ShadowDenoisedBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				if (AutoAADumpPhase >= 3)
					dumpResource(L"specular_raw", SpecularGIRaw.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
				if (AutoAADumpPhase >= 4)
				{
					dumpResource(L"gi_diffuse_raw", DiffuseGIRaw.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"gi_diffuse_raw_aux", DiffuseGIRawAux.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
				}
				if (AutoAADumpPhase >= 5)
				{
					dumpResource(L"gi_diffuse_temporal", DiffuseGITemporal[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"gi_diffuse_temporal_aux", DiffuseGITemporalAux[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"gi_specular_temporal", SpecularGITemporal[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
				}
				if (AutoAADumpPhase >= 6)
				{
					dumpResource(L"gi_diffuse_spatial", DiffuseGISpatial[0].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"gi_diffuse_spatial_aux", DiffuseGISpatialAux[0].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"gi_specular_spatial", SpecularGISpatial[0].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
				}
				if (bLightingStage)
				{
					dumpResource(L"lighting", LightingBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					Texture* resolveTarget = GetCurrentResolveSource();
					dumpResource(L"resolve", resolveTarget, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					if (backbuffer)
					{
						const bool screenPngOk = DumpTexturePNG(backbuffer, base + L"_screen_preview.png", D3D12_RESOURCE_STATE_RENDER_TARGET);
						AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] screen png=" + (screenPngOk ? L"ok" : L"fail"));
					}
				}
			}
			else
			{
				AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] limited backend preview capture requested");
			}
			}
		}

		++AutoAADumpFramesInPhase;
		if (AutoAADumpFramesInPhase < kHybridStageDumpFrames)
			return;

		AutoAADumpFramesInPhase = 0;
		++AutoAADumpPhase;
		if (AutoAADumpPhase < kNumHybridStages)
		{
			const bool bUseTAAForStage = ShouldUseTAAForHybridStageAutoDump(AutoAADumpPhase, bVulkanHybridDump);
			AntiAliasingMode = bUseTAAForStage ? EAntiAliasingMode::TAA : EAntiAliasingMode::OFF;
			ResetAllAccumulationState(false);
			AppendAutoAADumpLog(
				std::wstring(L"[") + GetHybridStageAutoDumpPhaseName(AutoAADumpPhase) +
				L"] begin frames=" + std::to_wstring(GetHybridStageAutoDumpFrameCount(AutoAADumpPhase, bVulkanHybridDump, bLimitedHybridStageDump)) +
				L", aa=" + (bUseTAAForStage ? L"taa" : L"off"));
			return;
		}

		bAutoAADumpCompleted = true;
		if (HWND hwnd = Win32Application::GetHwnd())
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		PostQuitMessage(0);
		return;
	}

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
		if (HWND hwnd = Win32Application::GetHwnd())
		{
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		}
		PostQuitMessage(0);
		return;
	}
}

std::wstring Corona::BuildFinalScreenshotPath()
{
	SYSTEMTIME localTime{};
	GetLocalTime(&localTime);

	std::filesystem::path screenshotDir = std::filesystem::path(L"C:\\dev\\Corona\\dumps\\screenshots");
	std::error_code ec;
	std::filesystem::create_directories(screenshotDir, ec);

	const wchar_t* backendName = L"unknown";
	if (renderBackend)
		backendName = renderBackend->GetAPI() == ERenderBackendAPI::Vulkan ? L"vulkan" : L"dx12";

	const wchar_t* modeName = RenderingMode == ERenderingMode::PATHTRACING ? L"pathtracing" : L"hybrid";

	for (UINT32 attempt = 0; attempt < 10000; ++attempt)
	{
		const UINT32 uniqueIndex = FinalScreenshotCounter++;
		std::wstringstream filename;
		filename << L"final_"
			<< std::setfill(L'0')
			<< std::setw(4) << localTime.wYear
			<< std::setw(2) << localTime.wMonth
			<< std::setw(2) << localTime.wDay
			<< L"_"
			<< std::setw(2) << localTime.wHour
			<< std::setw(2) << localTime.wMinute
			<< std::setw(2) << localTime.wSecond
			<< L"_" << backendName
			<< L"_" << modeName
			<< L"_frame" << std::setw(6) << FrameCounter
			<< L"_" << std::setw(4) << uniqueIndex
			<< L".png";

		std::filesystem::path candidate = screenshotDir / filename.str();
		if (!std::filesystem::exists(candidate))
			return candidate.wstring();
	}

	return (screenshotDir / L"final_capture.png").wstring();
}

void Corona::RequestFinalBackbufferScreenshot()
{
	if (!renderBackend || bFinalScreenshotCaptureInFlight)
		return;

	PendingFinalScreenshotPath = BuildFinalScreenshotPath();
	renderBackend->RequestWindowCapture(PendingFinalScreenshotPath);
	bFinalScreenshotCaptureInFlight = true;
	bFinalScreenshotRequested = false;
	LastFinalScreenshotStatus = L"Capturing final backbuffer without ImGui...";
}

void Corona::ConsumeFinalBackbufferScreenshotResult()
{
	if (!bFinalScreenshotCaptureInFlight || !renderBackend)
		return;

	std::wstring outputPath;
	std::wstring errorMessage;
	bool bCaptureSuccess = false;
	if (!renderBackend->ConsumeWindowCaptureResult(&outputPath, &bCaptureSuccess, &errorMessage))
		return;

	bFinalScreenshotCaptureInFlight = false;
	if (bCaptureSuccess)
	{
		LastFinalScreenshotStatus = L"Saved: " + outputPath;
	}
	else
	{
		LastFinalScreenshotStatus = L"Screenshot failed";
		if (!errorMessage.empty())
			LastFinalScreenshotStatus += L": " + errorMessage;
	}

	PendingFinalScreenshotPath.clear();
}

std::wstring Corona::GetCameraPathDirectory() const
{
	return L"C:\\dev\\Corona\\dumps\\camera_paths";
}

std::wstring Corona::GetCameraPathDumpDirectory() const
{
	return L"C:\\dev\\Corona\\dumps\\camera_path_frames";
}

Corona::CameraPathKeyframe Corona::CaptureCurrentCameraPathKeyframe(double timeSeconds) const
{
	CameraPathKeyframe keyframe;
	keyframe.TimeSeconds = timeSeconds;
	keyframe.Position = m_camera.m_position;
	keyframe.Yaw = m_camera.m_yaw;
	keyframe.Pitch = m_camera.m_pitch;
	keyframe.Fov = Fov;
	const float lightDirLength = glm::length(LightDir);
	keyframe.DirectionalLightDir = lightDirLength > 0.0f ? (LightDir / lightDirLength) : glm::vec3(0.0f, 1.0f, 0.0f);
	keyframe.DirectionalLightIntensity = LightIntensity;
	return keyframe;
}

double Corona::GetCameraPathDurationSeconds() const
{
	if (CameraPathKeyframes.size() < 2)
		return 0.0;
	return std::max(0.0, CameraPathKeyframes.back().TimeSeconds - CameraPathKeyframes.front().TimeSeconds);
}

Corona::CameraPathKeyframe Corona::SampleCameraPath(double timeSeconds) const
{
	if (CameraPathKeyframes.empty())
		return CaptureCurrentCameraPathKeyframe(0.0);

	if (CameraPathKeyframes.size() == 1 || timeSeconds <= CameraPathKeyframes.front().TimeSeconds)
		return CameraPathKeyframes.front();

	if (timeSeconds >= CameraPathKeyframes.back().TimeSeconds)
		return CameraPathKeyframes.back();

	auto upper = std::lower_bound(
		CameraPathKeyframes.begin(),
		CameraPathKeyframes.end(),
		timeSeconds,
		[](const CameraPathKeyframe& keyframe, double value)
		{
			return keyframe.TimeSeconds < value;
		});

	if (upper == CameraPathKeyframes.begin())
		return *upper;

	const CameraPathKeyframe& b = *upper;
	const CameraPathKeyframe& a = *(upper - 1);
	const double span = std::max(1.0e-6, b.TimeSeconds - a.TimeSeconds);
	const float alpha = static_cast<float>((timeSeconds - a.TimeSeconds) / span);

	CameraPathKeyframe result;
	result.TimeSeconds = timeSeconds;
	result.Position = glm::mix(a.Position, b.Position, alpha);
	float yawDelta = b.Yaw - a.Yaw;
	while (yawDelta > glm::pi<float>())
		yawDelta -= glm::two_pi<float>();
	while (yawDelta < -glm::pi<float>())
		yawDelta += glm::two_pi<float>();
	result.Yaw = a.Yaw + yawDelta * alpha;
	result.Pitch = a.Pitch + (b.Pitch - a.Pitch) * alpha;
	result.Fov = a.Fov + (b.Fov - a.Fov) * alpha;
	const glm::vec3 blendedLightDir = glm::mix(a.DirectionalLightDir, b.DirectionalLightDir, alpha);
	const float blendedLightDirLength = glm::length(blendedLightDir);
	result.DirectionalLightDir = blendedLightDirLength > 0.0f ? (blendedLightDir / blendedLightDirLength) : b.DirectionalLightDir;
	result.DirectionalLightIntensity =
		a.DirectionalLightIntensity + (b.DirectionalLightIntensity - a.DirectionalLightIntensity) * alpha;
	return result;
}

void Corona::ApplyCameraPathKeyframe(const CameraPathKeyframe& keyframe)
{
	m_camera.m_position = keyframe.Position;
	m_camera.m_yaw = keyframe.Yaw;
	m_camera.m_pitch = glm::clamp(keyframe.Pitch, -glm::quarter_pi<float>(), glm::quarter_pi<float>());
	m_camera.m_keysPressed = {};
	m_camera.m_mouseButtonDown = false;
	Fov = keyframe.Fov;
	const float lightDirLength = glm::length(keyframe.DirectionalLightDir);
	LightDir = lightDirLength > 0.0f ? (keyframe.DirectionalLightDir / lightDirLength) : LightDir;
	LightIntensity = keyframe.DirectionalLightIntensity;

	const float r = cosf(m_camera.m_pitch);
	m_camera.m_lookDirection.x = r * sinf(m_camera.m_yaw);
	m_camera.m_lookDirection.y = sinf(m_camera.m_pitch);
	m_camera.m_lookDirection.z = r * cosf(m_camera.m_yaw);
}

void Corona::StartCameraPathRecording()
{
	bCameraPathPlaying = false;
	bCameraPathDumping = false;
	bCameraPathDumpCaptureInFlight = false;
	bCameraPathRecording = true;
	CameraPathKeyframes.clear();
	CameraPathRecordingStartSeconds = m_timer.GetTotalSeconds();
	CameraPathKeyframes.push_back(CaptureCurrentCameraPathKeyframe(0.0));
	LastCameraPathStatus = L"Camera path recording started.";
}

void Corona::EndCameraPathRecording()
{
	if (!bCameraPathRecording)
		return;

	const double currentTime = std::max(0.0, m_timer.GetTotalSeconds() - CameraPathRecordingStartSeconds);
	if (CameraPathKeyframes.empty() || currentTime > CameraPathKeyframes.back().TimeSeconds + 1.0e-6)
		CameraPathKeyframes.push_back(CaptureCurrentCameraPathKeyframe(currentTime));

	if (CameraPathKeyframes.size() == 1)
	{
		CameraPathKeyframe duplicate = CameraPathKeyframes.back();
		duplicate.TimeSeconds += 1.0 / kCameraPathDumpFps;
		CameraPathKeyframes.push_back(duplicate);
	}

	bCameraPathRecording = false;

	SYSTEMTIME localTime{};
	GetLocalTime(&localTime);
	std::filesystem::path pathDir(GetCameraPathDirectory());
	std::error_code ec;
	std::filesystem::create_directories(pathDir, ec);

	for (UINT32 attempt = 0; attempt < 10000; ++attempt)
	{
		std::wstringstream filename;
		filename << L"camera_path_"
			<< std::setfill(L'0')
			<< std::setw(4) << localTime.wYear
			<< std::setw(2) << localTime.wMonth
			<< std::setw(2) << localTime.wDay
			<< L"_"
			<< std::setw(2) << localTime.wHour
			<< std::setw(2) << localTime.wMinute
			<< std::setw(2) << localTime.wSecond
			<< L"_" << std::setw(4) << attempt
			<< L".coronapath";

		const std::filesystem::path candidate = pathDir / filename.str();
		if (!std::filesystem::exists(candidate))
		{
			SaveCameraPath(candidate.wstring());
			return;
		}
	}

	LastCameraPathStatus = L"Camera path recording ended, but failed to allocate a unique path file.";
}

bool Corona::SaveCameraPath(const std::wstring& filePath)
{
	if (CameraPathKeyframes.empty())
	{
		LastCameraPathStatus = L"No camera path keyframes to save.";
		return false;
	}

	std::filesystem::create_directories(std::filesystem::path(filePath).parent_path());
	std::ofstream file(std::filesystem::path(filePath), std::ios::trunc);
	if (!file.is_open())
	{
		LastCameraPathStatus = L"Failed to save camera path.";
		return false;
	}

	file << std::fixed << std::setprecision(9);
	file << "corona_camera_path 2\n";
	file << "fps " << kCameraPathDumpFps << "\n";
	file << "keyframes " << CameraPathKeyframes.size() << "\n";
	file << "columns time_seconds pos_x pos_y pos_z yaw pitch fov light_dir_x light_dir_y light_dir_z light_intensity\n";
	for (const CameraPathKeyframe& keyframe : CameraPathKeyframes)
	{
		file << "k "
			<< keyframe.TimeSeconds << ' '
			<< keyframe.Position.x << ' '
			<< keyframe.Position.y << ' '
			<< keyframe.Position.z << ' '
			<< keyframe.Yaw << ' '
			<< keyframe.Pitch << ' '
			<< keyframe.Fov << ' '
			<< keyframe.DirectionalLightDir.x << ' '
			<< keyframe.DirectionalLightDir.y << ' '
			<< keyframe.DirectionalLightDir.z << ' '
			<< keyframe.DirectionalLightIntensity << '\n';
	}

	ActiveCameraPathFile = filePath;
	LastCameraPathStatus =
		L"Saved camera path: " + filePath +
		L" (" + std::to_wstring(static_cast<unsigned long long>(CameraPathKeyframes.size())) + L" keyframes)";
	return true;
}

bool Corona::LoadCameraPath(const std::wstring& filePath)
{
	std::ifstream file{ std::filesystem::path(filePath) };
	if (!file.is_open())
	{
		LastCameraPathStatus = L"Failed to open camera path: " + filePath;
		return false;
	}

	std::vector<CameraPathKeyframe> loadedKeyframes;
	std::string token;
	while (file >> token)
	{
		if (token.empty())
			continue;

		if (token[0] == '#')
		{
			std::string ignoredLine;
			std::getline(file, ignoredLine);
			continue;
		}

		if (token == "corona_camera_path")
		{
			int version = 0;
			file >> version;
			continue;
		}

		if (token == "fps" || token == "keyframes" || token == "columns")
		{
			std::string ignoredLine;
			std::getline(file, ignoredLine);
			continue;
		}

		if (token == "k")
		{
			std::string keyframeLine;
			std::getline(file, keyframeLine);
			std::istringstream keyframeStream(keyframeLine);
			CameraPathKeyframe keyframe;
			keyframe.DirectionalLightDir = glm::length(LightDir) > 0.0f ? glm::normalize(LightDir) : glm::vec3(0.0f, 1.0f, 0.0f);
			keyframe.DirectionalLightIntensity = LightIntensity;
			keyframeStream >> keyframe.TimeSeconds
				>> keyframe.Position.x
				>> keyframe.Position.y
				>> keyframe.Position.z
				>> keyframe.Yaw
				>> keyframe.Pitch
				>> keyframe.Fov;
			if (keyframeStream)
			{
				glm::vec3 loadedLightDir;
				float loadedLightIntensity = keyframe.DirectionalLightIntensity;
				if (keyframeStream >> loadedLightDir.x >> loadedLightDir.y >> loadedLightDir.z >> loadedLightIntensity)
				{
					const float loadedLightDirLength = glm::length(loadedLightDir);
					if (loadedLightDirLength > 0.0f)
						keyframe.DirectionalLightDir = loadedLightDir / loadedLightDirLength;
					keyframe.DirectionalLightIntensity = loadedLightIntensity;
				}
				loadedKeyframes.push_back(keyframe);
			}
		}
		else
		{
			std::string ignoredLine;
			std::getline(file, ignoredLine);
		}
	}

	if (loadedKeyframes.size() < 2)
	{
		LastCameraPathStatus = L"Camera path needs at least two keyframes: " + filePath;
		return false;
	}

	const double firstTime = loadedKeyframes.front().TimeSeconds;
	for (CameraPathKeyframe& keyframe : loadedKeyframes)
		keyframe.TimeSeconds = std::max(0.0, keyframe.TimeSeconds - firstTime);

	CameraPathKeyframes = std::move(loadedKeyframes);
	ActiveCameraPathFile = filePath;
	LastCameraPathStatus =
		L"Loaded camera path: " + filePath +
		L" (" + std::to_wstring(static_cast<unsigned long long>(CameraPathKeyframes.size())) + L" keyframes)";
	return true;
}

bool Corona::LoadLatestCameraPath()
{
	std::filesystem::path pathDir(GetCameraPathDirectory());
	std::error_code ec;
	if (!std::filesystem::exists(pathDir, ec))
	{
		LastCameraPathStatus = L"No camera path directory exists yet.";
		return false;
	}

	bool bFound = false;
	std::filesystem::path latestPath;
	std::filesystem::file_time_type latestTime{};
	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(pathDir, ec))
	{
		if (ec || !entry.is_regular_file())
			continue;
		if (entry.path().extension() != L".coronapath")
			continue;

		std::error_code timeEc;
		const auto writeTime = entry.last_write_time(timeEc);
		if (timeEc)
			continue;

		if (!bFound || writeTime > latestTime)
		{
			bFound = true;
			latestTime = writeTime;
			latestPath = entry.path();
		}
	}

	if (!bFound)
	{
		LastCameraPathStatus = L"No .coronapath files found.";
		return false;
	}

	return LoadCameraPath(latestPath.wstring());
}

void Corona::StartCameraPathPlayback()
{
	if (CameraPathKeyframes.size() < 2)
	{
		LastCameraPathStatus = L"Record or load a camera path before playback.";
		return;
	}

	bCameraPathRecording = false;
	bCameraPathDumping = false;
	bCameraPathDumpCaptureInFlight = false;
	bCameraPathPlaying = true;
	CameraPathPlaybackStartSeconds = m_timer.GetTotalSeconds();
	ApplyCameraPathKeyframe(CameraPathKeyframes.front());
	FrameCounter = 0;
	bResetTemporalStateNextUpdate = true;
	LastCameraPathStatus = L"Camera path playback started.";
}

void Corona::StopCameraPathPlayback()
{
	if (bCameraPathDumping)
	{
		StopCameraPathDump();
		return;
	}

	if (bCameraPathPlaying)
		LastCameraPathStatus = L"Camera path playback stopped.";
	bCameraPathPlaying = false;
}

void Corona::StartCameraPathDump()
{
	if (CameraPathKeyframes.size() < 2)
	{
		LastCameraPathStatus = L"Record or load a camera path before dumping frames.";
		return;
	}

	SYSTEMTIME localTime{};
	GetLocalTime(&localTime);
	std::filesystem::path dumpRoot(GetCameraPathDumpDirectory());
	std::error_code ec;
	std::filesystem::create_directories(dumpRoot, ec);
	LastCameraPathDumpDir.clear();

	for (UINT32 attempt = 0; attempt < 10000; ++attempt)
	{
		std::wstringstream dirName;
		dirName << L"camera_path_"
			<< std::setfill(L'0')
			<< std::setw(4) << localTime.wYear
			<< std::setw(2) << localTime.wMonth
			<< std::setw(2) << localTime.wDay
			<< L"_"
			<< std::setw(2) << localTime.wHour
			<< std::setw(2) << localTime.wMinute
			<< std::setw(2) << localTime.wSecond
			<< L"_" << std::setw(4) << attempt;

		std::filesystem::path candidate = dumpRoot / dirName.str();
		if (!std::filesystem::exists(candidate))
		{
			std::filesystem::create_directories(candidate, ec);
			LastCameraPathDumpDir = candidate.wstring();
			break;
		}
	}

	if (LastCameraPathDumpDir.empty())
	{
		LastCameraPathStatus = L"Failed to allocate a camera path frame dump directory.";
		return;
	}

	const double duration = GetCameraPathDurationSeconds();
	CameraPathDumpFrameCount = std::max<UINT32>(1u, static_cast<UINT32>(std::ceil(duration * kCameraPathDumpFps)) + 1u);
	CameraPathDumpFrameIndex = 0;
	bCameraPathRecording = false;
	bCameraPathPlaying = true;
	bCameraPathDumping = true;
	bCameraPathDumpCaptureInFlight = false;
	CameraPathPlaybackStartSeconds = m_timer.GetTotalSeconds();
	FrameCounter = 0;
	bResetTemporalStateNextUpdate = true;
	ApplyCameraPathKeyframe(CameraPathKeyframes.front());

	std::wofstream infoFile(std::filesystem::path(LastCameraPathDumpDir) / L"dump_info.txt", std::ios::trunc);
	if (infoFile.is_open())
	{
		infoFile << L"fps " << static_cast<int>(kCameraPathDumpFps) << L"\n";
		infoFile << L"frame_count " << CameraPathDumpFrameCount << L"\n";
		infoFile << L"camera_path " << ActiveCameraPathFile << L"\n";
	}

	LastCameraPathStatus =
		L"Playing camera path and dumping 30fps PNG frames to: " + LastCameraPathDumpDir +
		L" (" + std::to_wstring(CameraPathDumpFrameCount) + L" frames)";
}

void Corona::StopCameraPathDump()
{
	if (bCameraPathDumping)
		LastCameraPathStatus = L"Camera path playback and frame dump stopped.";
	bCameraPathDumping = false;
	bCameraPathDumpCaptureInFlight = false;
	bCameraPathPlaying = false;
}

void Corona::UpdateCameraPathState()
{
	if (bCameraPathDumping)
	{
		bCameraPathPlaying = true;
		const double duration = GetCameraPathDurationSeconds();
		const double frameTime = std::min(duration, static_cast<double>(CameraPathDumpFrameIndex) / kCameraPathDumpFps);
		ApplyCameraPathKeyframe(SampleCameraPath(frameTime));
		return;
	}

	if (bCameraPathPlaying)
	{
		const double duration = GetCameraPathDurationSeconds();
		const double playbackTime = m_timer.GetTotalSeconds() - CameraPathPlaybackStartSeconds;
		if (playbackTime >= duration)
		{
			ApplyCameraPathKeyframe(CameraPathKeyframes.back());
			bCameraPathPlaying = false;
			LastCameraPathStatus = L"Camera path playback finished.";
		}
		else
		{
			ApplyCameraPathKeyframe(SampleCameraPath(playbackTime));
		}
		return;
	}

	if (bCameraPathRecording)
	{
		const double sampleTime = std::max(0.0, m_timer.GetTotalSeconds() - CameraPathRecordingStartSeconds);
		if (CameraPathKeyframes.empty() ||
			sampleTime >= CameraPathKeyframes.back().TimeSeconds + kCameraPathRecordMinIntervalSeconds)
		{
			CameraPathKeyframes.push_back(CaptureCurrentCameraPathKeyframe(sampleTime));
		}
	}
}

std::wstring Corona::BuildCameraPathFrameDumpPath() const
{
	std::wstringstream filename;
	filename << L"frame_" << std::setfill(L'0') << std::setw(6) << CameraPathDumpFrameIndex << L".png";
	return (std::filesystem::path(LastCameraPathDumpDir) / filename.str()).wstring();
}

void Corona::RequestCameraPathDumpFrameCapture()
{
	if (!renderBackend || !bCameraPathDumping || bCameraPathDumpCaptureInFlight)
		return;

	if (CameraPathDumpFrameIndex >= CameraPathDumpFrameCount)
	{
		StopCameraPathDump();
		return;
	}

	renderBackend->RequestWindowCapture(BuildCameraPathFrameDumpPath());
	bCameraPathDumpCaptureInFlight = true;
}

void Corona::ConsumeCameraPathDumpCaptureResult()
{
	if (!bCameraPathDumpCaptureInFlight || !renderBackend)
		return;

	std::wstring outputPath;
	std::wstring errorMessage;
	bool bCaptureSuccess = false;
	if (!renderBackend->ConsumeWindowCaptureResult(&outputPath, &bCaptureSuccess, &errorMessage))
		return;

	bCameraPathDumpCaptureInFlight = false;
	++CameraPathDumpFrameIndex;

	if (!bCaptureSuccess)
	{
		LastCameraPathStatus = L"Camera path frame capture failed";
		if (!errorMessage.empty())
			LastCameraPathStatus += L": " + errorMessage;
	}
	else if (CameraPathDumpFrameIndex >= CameraPathDumpFrameCount)
	{
		bCameraPathDumping = false;
		bCameraPathPlaying = false;
		LastCameraPathStatus =
			L"Camera path playback and frame dump complete: " + LastCameraPathDumpDir +
			L" (" + std::to_wstring(CameraPathDumpFrameCount) + L" frames)";
	}
	else
	{
		LastCameraPathStatus =
			L"Dumped frame " + std::to_wstring(CameraPathDumpFrameIndex) +
			L" / " + std::to_wstring(CameraPathDumpFrameCount);
	}
}

void Corona::LaunchCameraPathVideoEncode()
{
	if (LastCameraPathDumpDir.empty())
	{
		LastCameraPathStatus = L"No camera path frame dump is available yet.";
		return;
	}

	const std::filesystem::path scriptPath =
		std::filesystem::path(GetAssetFullPath(L"..\\tools\\convert_frame_sequence.py")).lexically_normal();
	if (!std::filesystem::exists(scriptPath))
	{
		LastCameraPathStatus = L"Video conversion script was not found: " + scriptPath.wstring();
		return;
	}

	const std::filesystem::path outputPath = std::filesystem::path(LastCameraPathDumpDir) / L"camera_path.mp4";
	const std::filesystem::path logPath = std::filesystem::path(LastCameraPathDumpDir) / L"camera_path_encode.log";
	std::wstring displayCommand =
		L"py -3 \"" + scriptPath.wstring() +
		L"\" --input \"" + LastCameraPathDumpDir +
		L"\" --fps 30 --output \"" + outputPath.wstring() + L"\"";
	std::wstring command = displayCommand + L" > \"" + logPath.wstring() + L"\" 2>&1";
	LastCameraPathVideoCommand = displayCommand;
	const int result = _wsystem(command.c_str());
	if (result == 0)
		LastCameraPathStatus = L"Video conversion complete: " + outputPath.wstring();
	else
	{
		const std::wstring logPreview = ReadTextFilePreview(logPath, 700);
		LastCameraPathStatus = L"Video conversion failed. Log: " + logPath.wstring();
		if (!logPreview.empty())
			LastCameraPathStatus += L"\n" + logPreview;
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
	AppendVulkanRuntimeTrace(L"[OnInit] begin");

	g_TS.Initialize(8);
	AppendVulkanRuntimeTrace(L"[OnInit] after g_TS.Initialize");

	m_camera.Init({ 458, 781, 185 });
	m_camera.SetMoveSpeed(200);
	LoadCameraState();
	AppendVulkanRuntimeTrace(L"[OnInit] after camera init");

	RenderWidth = m_width;
	RenderHeight = m_height;
	AppendVulkanRuntimeTrace(L"[OnInit] after render size init");
#if WITH_STREAMLINE
	const bool bStartupRequestsVulkan =
		bCommandLineRenderBackendOverrideSet &&
		CommandLineRenderBackendAPI == ERenderBackendAPI::Vulkan;
	if (!bStartupRequestsVulkan)
	{
		InitStreamline();
		AppendVulkanRuntimeTrace(L"[OnInit] after InitStreamline");
	}
	else
	{
		AppendVulkanRuntimeTrace(L"[OnInit] skip InitStreamline for Vulkan startup");
	}
#endif
	LoadPipeline();
	AppendVulkanRuntimeTrace(L"[OnInit] after LoadPipeline");
	PromptStartupModeSelection();
	AppendVulkanRuntimeTrace(
		L"[OnInit] after PromptStartupModeSelection backend=" + std::to_wstring(static_cast<int>(StartupRenderBackendAPI)) +
		L", renderMode=" + std::to_wstring(static_cast<int>(StartupRenderingMode)) +
		L", autoDump=" + std::to_wstring(bAutoAADumpEnabled ? 1 : 0));
	const bool bVulkanHybridStartup =
		StartupRenderingMode == ERenderingMode::HYBRID &&
		StartupRenderBackendAPI == ERenderBackendAPI::Vulkan;
	if (bVulkanHybridStartup)
	{
		const bool bCameraLooksUninitialized =
			glm::length(m_camera.m_position) < 0.01f &&
			fabsf(m_camera.m_yaw) < 0.01f &&
			fabsf(m_camera.m_pitch) < 0.01f;
		if (bCameraLooksUninitialized)
		{
			AppendVulkanRuntimeTrace(L"[OnInit] applying default hybrid camera");
			ApplyHybridDefaultCamera();
		}
	}
	AppendVulkanRuntimeTrace(L"[OnInit] before RefreshUpscaleSettings");
	RefreshUpscaleSettings(false);
	AppendVulkanRuntimeTrace(L"[OnInit] after RefreshUpscaleSettings");
	AppendVulkanRuntimeTrace(L"[OnInit] before LoadAssets");
	LoadAssets();
	AppendVulkanRuntimeTrace(L"[OnInit] after LoadAssets");
	bPendingTemporalHistoryClear = true;
	InitializeAutoAADump();
	AppendVulkanRuntimeTrace(
		L"[OnInit] after InitializeAutoAADump initialized=" + std::to_wstring(bAutoAADumpInitialized ? 1 : 0) +
		L", completed=" + std::to_wstring(bAutoAADumpCompleted ? 1 : 0) +
		L", dir=" + AutoAADumpDir);
}

namespace
{
	void AppendVulkanRuntimeTrace(const std::wstring& line)
	{
		constexpr bool kVerboseVulkanRuntimeTrace = false;
		if (!kVerboseVulkanRuntimeTrace)
		{
			constexpr const wchar_t* kHighFrequencyPrefixes[] =
			{
				L"[OnRender]",
				L"[GBufferPass]",
				L"[RaytraceReflectionPass]",
				L"[TemporalDenoisingPass]",
				L"[SpatialDenoisingPass]"
			};
			for (const wchar_t* prefix : kHighFrequencyPrefixes)
			{
				if (line.rfind(prefix, 0) == 0)
					return;
			}
		}

		const std::filesystem::path tracePath = std::filesystem::path(L"C:\\dev\\Corona\\dumps\\vulkan_runtime_trace.log");
		std::filesystem::create_directories(tracePath.parent_path());
		std::wofstream traceFile(tracePath, std::ios::app);
		if (traceFile.is_open())
		{
			traceFile << line << L"\n";
		}
	}
}

void Corona::LoadPipeline()
{
	if (bCommandLineRenderBackendOverrideSet && CommandLineRenderBackendAPI == ERenderBackendAPI::Vulkan)
	{
		AppendVulkanRuntimeTrace(L"[LoadPipeline] begin Vulkan path");
		renderBackend = CreateRenderBackend(ERenderBackendAPI::Vulkan, nullptr);
		dx12_rhi = nullptr;
		if (!renderBackend)
		{
			throw std::runtime_error("Failed to create Vulkan render backend.");
		}
		AppendVulkanRuntimeTrace(
			L"[LoadPipeline] after CreateRenderBackend Vulkan api=" + std::to_wstring(static_cast<int>(renderBackend->GetAPI())) +
			L", name=" + std::wstring(renderBackend->GetBackendName(), renderBackend->GetBackendName() + std::strlen(renderBackend->GetBackendName())));

		renderBackend->CreateSwapChainForWindow(
			nullptr,
			Win32Application::GetHwnd(),
			m_width,
			m_height,
			DXGI_FORMAT_R8G8B8A8_UNORM);
		AppendVulkanRuntimeTrace(L"[LoadPipeline] after CreateSwapChainForWindow Vulkan");
		return;
	}

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
	const bool bVulkanBackend =
		renderBackend &&
		renderBackend->GetAPI() == ERenderBackendAPI::Vulkan;
	const bool bVulkanHybridStartup =
		bVulkanBackend &&
		StartupRenderingMode == ERenderingMode::HYBRID;
	const bool bVulkanPathTracingStartup =
		bVulkanBackend &&
		StartupRenderingMode == ERenderingMode::PATHTRACING;
	const uint32_t maxSupportedHybridStage =
		(renderBackend && StartupRenderingMode == ERenderingMode::HYBRID) ?
		renderBackend->GetMaxSupportedHybridStage() : 7u;
	const bool bVulkanHybridBootstrap = bVulkanHybridStartup && maxSupportedHybridStage == 0u;
	const bool bSupportsHybridRaytracing = !bVulkanHybridStartup || maxSupportedHybridStage >= 1u;
	const bool bSupportsShadowDenoise = !bVulkanHybridStartup || maxSupportedHybridStage >= 2u;
	const bool bSupportsTemporalDenoise = !bVulkanHybridStartup || maxSupportedHybridStage >= 5u;
	const bool bSupportsSpatialDenoise = !bVulkanHybridStartup || maxSupportedHybridStage >= 6u;
	const bool bSupportsFullHybridPresentation = !bVulkanHybridStartup || maxSupportedHybridStage >= 7u;
	const bool bAllowBlueNoiseInit = !bVulkanHybridStartup || maxSupportedHybridStage >= 3u;
	const bool bAllowImguiInit =
		!bCommandLineDisableImgui &&
		(!bVulkanHybridStartup ||
			(bSupportsFullHybridPresentation && !bAutoAADumpEnabled));
	if (bVulkanHybridBootstrap)
		AppendVulkanRuntimeTrace(L"[LoadAssets] begin Vulkan hybrid bootstrap");
	else if (bVulkanHybridStartup)
		AppendVulkanRuntimeTrace(L"[LoadAssets] begin Vulkan hybrid stage-aware init stage=" + std::to_wstring(maxSupportedHybridStage));

	if (bAllowBlueNoiseInit && !bBlueNoiseInitialized)
	{
		InitBlueNoiseTexture();
		bBlueNoiseInitialized = true;
	}
	if (bAllowImguiInit && !bImguiInitialized)
	{
		InitImgui();
		bImguiInitialized = true;
	}
	if (!bAllowImguiInit)
	{
		bShowImgui = false;
		bImguiInitialized = false;
	}

	AppendVulkanRuntimeTrace(L"[LoadAssets] before InitGBufferPass");
	InitGBufferPass();
	AppendVulkanRuntimeTrace(L"[LoadAssets] after InitGBufferPass");
	if (bSupportsFullHybridPresentation)
	{
		AppendVulkanRuntimeTrace(L"[LoadAssets] before full hybrid presentation pass init");
		InitLightingPass();
		InitToneMapPass();
		if (!bVulkanPathTracingStartup)
			InitTemporalAAPass();
		if (!bVulkanBackend)
		{
			InitDebugPass();
			InitBloomPass();
		}
		AppendVulkanRuntimeTrace(L"[LoadAssets] after full hybrid presentation pass init");
	}
	if (bSupportsShadowDenoise)
	{
		AppendVulkanRuntimeTrace(L"[LoadAssets] before InitShadowDenoisePass");
		InitShadowDenoisePass();
		AppendVulkanRuntimeTrace(L"[LoadAssets] after InitShadowDenoisePass");
	}
	if (bSupportsTemporalDenoise)
	{
		AppendVulkanRuntimeTrace(L"[LoadAssets] before InitTemporalDenoisingPass");
		InitTemporalDenoisingPass();
		AppendVulkanRuntimeTrace(L"[LoadAssets] after InitTemporalDenoisingPass");
	}
	if (bSupportsTemporalDenoise || bSupportsSpatialDenoise)
	{
		AppendVulkanRuntimeTrace(L"[LoadAssets] before InitSpatialDenoisingPass");
		InitSpatialDenoisingPass();
		AppendVulkanRuntimeTrace(L"[LoadAssets] after InitSpatialDenoisingPass");
	}
	AppendVulkanRuntimeTrace(L"[LoadAssets] before InitGpuTimingResources");
	InitGpuTimingResources();
	AppendVulkanRuntimeTrace(L"[LoadAssets] after InitGpuTimingResources");

	const UINT DisplayWidth = m_width;
	const UINT DisplayHeight = m_height;
	const UINT RenderWidthLocal = GetRenderWidth();
	const UINT RenderHeightLocal = GetRenderHeight();
	const ETextureFormat HybridFloat4UAVFormat =
		(renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
		? ETextureFormat::RGBA32Float
		: ETextureFormat::RGBA16Float;

	if (bSupportsFullHybridPresentation &&
		renderBackend &&
		renderBackend->GetAPI() == ERenderBackendAPI::D3D12 &&
		framebuffers.empty())
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
	AppendVulkanRuntimeTrace(L"[LoadAssets] after color buffers");

	if (bSupportsFullHybridPresentation && bVulkanHybridStartup && !ExposureData)
	{
		float initExposure[] =
		{
			Exposure,
			1.0f / Exposure,
			0.01f,
			Exposure,
			0.0f,
			kInitialMinLog,
			kInitialMaxLog,
			kInitialMaxLog - kInitialMinLog
		};
		ExposureData = createBuffer(8u, sizeof(float), true, initExposure);
	}

	// Path tracing accumulation buffers
	PathTracingAccumBuffer[0] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1, glm::vec4(0.0f));
	
	NAME_D3D12_OBJECT(PathTracingAccumBuffer[0]->resource);

	PathTracingAccumBuffer[1] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1, glm::vec4(0.0f));
	
	NAME_D3D12_OBJECT(PathTracingAccumBuffer[1]->resource);
	AppendVulkanRuntimeTrace(L"[LoadAssets] after path tracing buffers");

	// lighting result
	LightingBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	LightingBuffer->MakeRTV();

	NAME_D3D12_OBJECT(LightingBuffer->resource);
	AppendVulkanRuntimeTrace(L"[LoadAssets] after lighting buffer");

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
	ShadowBuffer = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(ShadowBuffer->resource);

	ShadowDenoisedBuffer = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(ShadowDenoisedBuffer->resource);
	AppendVulkanRuntimeTrace(L"[LoadAssets] after shadow buffers");

	// refleciton result
	SpecularGIRaw = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(SpecularGIRaw->resource);

	SpecularGITemporal[0] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(SpecularGITemporal[0]->resource);

	SpecularGITemporal[1] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(SpecularGITemporal[1]->resource);

	// moments
	SpecularGIMoments[0] = createTexture2D(ETextureFormat::RG16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(SpecularGIMoments[0]->resource);

	SpecularGIMoments[1] = createTexture2D(ETextureFormat::RG16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(SpecularGIMoments[1]->resource);
	// diffuse gi

	DiffuseGIRawAux = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGIRawAux->resource);

	DiffuseGIRaw = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGIRaw->resource);

	// gi result sh
	DiffuseGITemporalAux[0] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGITemporalAux[0]->resource);

	DiffuseGITemporalAux[1] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGITemporalAux[1]->resource);

	// gi result color
	DiffuseGITemporal[0] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGITemporal[0]->resource);

	DiffuseGITemporal[1] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGITemporal[1]->resource);
	AppendVulkanRuntimeTrace(L"[LoadAssets] after temporal GI buffers");

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
	AppendVulkanRuntimeTrace(L"[LoadAssets] after gbuffer textures");

	if (!DefaultWhiteTex) DefaultWhiteTex = renderBackend->CreateTextureFromFile(L"assets/default/default_white.png", false);
	if (!DefaultBlackTex) DefaultBlackTex = renderBackend->CreateTextureFromFile(L"assets/default/default_black.png", false);
	if (!DefaultNormalTex) DefaultNormalTex = renderBackend->CreateTextureFromFile(L"assets/default/default_normal.png", true);
	if (!DefaultRougnessTex) DefaultRougnessTex = renderBackend->CreateTextureFromFile(L"assets/default/default_roughness.png", true);
	if (bVulkanHybridStartup)
		AppendVulkanRuntimeTrace(L"[LoadAssets] after default textures");

	if (!Sponza) Sponza = LoadModel("assets/Sponza/Sponza.fbx");
	if (bVulkanHybridStartup)
		AppendVulkanRuntimeTrace(L"[LoadAssets] after sponza load");

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
	if (bVulkanHybridStartup)
		AppendVulkanRuntimeTrace(L"[LoadAssets] after samplers");
	if (bVulkanHybridBootstrap)
	{
		AppendVulkanRuntimeTrace(L"[LoadAssets] Vulkan hybrid bootstrap assets ready");
		return;
	}
	if (bSupportsHybridRaytracing)
	{
		AppendVulkanRuntimeTrace(L"[LoadAssets] before InitRaytracingData");
		InitRaytracingData();
		AppendVulkanRuntimeTrace(L"[LoadAssets] after InitRaytracingData");
		AppendVulkanRuntimeTrace(L"[LoadAssets] before InitRTPSO");
		InitRTPSO();
		AppendVulkanRuntimeTrace(L"[LoadAssets] after InitRTPSO");
		if (StartupRenderingMode == ERenderingMode::PATHTRACING)
		{
			AppendVulkanRuntimeTrace(L"[LoadAssets] before InitPathTracingPass");
			InitPathTracingPass();
			AppendVulkanRuntimeTrace(L"[LoadAssets] after InitPathTracingPass");
		}
	}

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
	UINT totalNumIndex = 0;
	for (UINT i = 0; i < numMeshes; ++i)
	{
		aiMesh* asMesh = assimpScene->mMeshes[i];

		totalNumVert += asMesh->mNumVertices;
		totalNumIndex += asMesh->mNumFaces * 3;
	}

	vector<Vertex> rtSceneVertices;
	vector<UINT32> rtSceneIndices;
	rtSceneVertices.reserve(totalNumVert);
	rtSceneIndices.reserve(totalNumIndex);

	for (UINT i = 0; i < numMeshes; ++i)
	{
		aiMesh* asMesh = assimpScene->mMeshes[i];

		Mesh* mesh = new Mesh;
		mesh->Owner = renderBackend.get();

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

		mesh->RtVertexOffset = static_cast<UINT>(rtSceneVertices.size());
		mesh->RtIndexOffset = static_cast<UINT>(rtSceneIndices.size());
		rtSceneVertices.insert(rtSceneVertices.end(), vertices.begin(), vertices.end());
		rtSceneIndices.insert(rtSceneIndices.end(), indices.begin(), indices.end());

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

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan && !rtSceneVertices.empty() && !rtSceneIndices.empty())
	{
		scene->RtSceneVertexBuffer = renderBackend->CreateVertexBuffer(
			static_cast<UINT>(sizeof(Vertex) * rtSceneVertices.size()),
			sizeof(Vertex),
			rtSceneVertices.data());
		scene->RtSceneIndexBuffer = renderBackend->CreateIndexBuffer(
			DXGI_FORMAT_R32_UINT,
			static_cast<UINT>(sizeof(UINT32) * rtSceneIndices.size()),
			rtSceneIndices.data());
	}

	shared_ptr<Scene> scenePtr = shared_ptr<Scene>(scene);

	return scenePtr;
}

void Corona::InitSpatialDenoisingPass()
{
	shared_ptr<ComputePipelineStateObject> TEMP_SpatialDenoisingFilterPSO = renderBackend->CreateComputePipelineStateObject();
	if (!TEMP_SpatialDenoisingFilterPSO)
		return;
	TEMP_SpatialDenoisingFilterPSO->BindSRV("DepthTex", 0, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("GeoNormalTex", 1, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("InGIResultSHTex", 2, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("InGIResultColorTex", 3, 1);
	TEMP_SpatialDenoisingFilterPSO->BindSRV("InSpecularGITex", 4, 1);
	
	
	TEMP_SpatialDenoisingFilterPSO->BindUAV("OutGIResultSH", 0);
	TEMP_SpatialDenoisingFilterPSO->BindUAV("OutGIResultColor", 1);
	TEMP_SpatialDenoisingFilterPSO->BindUAV("OutSpecularGI", 2);
	
	
	TEMP_SpatialDenoisingFilterPSO->BindCBV("SpatialFilterConstant", 0, sizeof(SpatialFilterConstant));
	bool bSuccess = TEMP_SpatialDenoisingFilterPSO->InitCS(GetAssetFullPath(L"Shaders\\SpatialDenoising.hlsl"), "SpatialFilter");
	if (bSuccess)
		SpatialDenoisingFilterPSO = TEMP_SpatialDenoisingFilterPSO;

	UINT WidthGI = GetRenderWidth();
	UINT HeightGI = GetRenderHeight();
	const ETextureFormat HybridFloat4UAVFormat =
		(renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
		? ETextureFormat::RGBA32Float
		: ETextureFormat::RGBA16Float;

	DiffuseGISpatialAux[0] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatialAux[0]->resource);

	DiffuseGISpatialAux[1] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatialAux[1]->resource);

	DiffuseGISpatial[0] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatial[0]->resource);

	DiffuseGISpatial[1] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(DiffuseGISpatial[1]->resource);

	SpecularGISpatial[0] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(SpecularGISpatial[0]->resource);

	SpecularGISpatial[1] = renderBackend->CreateTexture2D({ HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, EInitialResourceState::ShaderRead, (int)WidthGI, (int)HeightGI, 1, std::nullopt });

	NAME_D3D12_OBJECT(SpecularGISpatial[1]->resource);
}

void Corona::InitTemporalDenoisingPass()
{
	shared_ptr<ComputePipelineStateObject> TEMP_TemporalDenoisingFilterPSO = renderBackend->CreateComputePipelineStateObject();
	if (!TEMP_TemporalDenoisingFilterPSO)
		return;
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
	bool bSuccess = TEMP_TemporalDenoisingFilterPSO->InitCS(GetAssetFullPath(L"Shaders\\TemporalDenoising.hlsl"), "TemporalFilter");
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
	GraphicsPipelineDesc desc{};
	desc.ShaderPath = GetAssetFullPath(L"Shaders\\GBuffer.hlsl");
	desc.VertexEntryPoint = "VSMain";
	desc.PixelEntryPoint = "PSMain";
	desc.VertexElements = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 12 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 24 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 32 },
	};
	desc.TextureBindings = {
		{ "AlbedoTex", 0 },
		{ "NormalTex", 1 },
		{ "RoughnessTex", 2 },
		{ "MetallicTex", 3 },
	};
	desc.SamplerBindings = {
		{ "samplerWrap", 0 },
	};
	desc.VertexStride = 44;
	desc.ColorFormats = {
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R16G16_FLOAT,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R32_FLOAT,
	};
	desc.DepthFormat = DXGI_FORMAT_D32_FLOAT;
	desc.bDepthEnable = true;
	desc.bCullBackFaces = false;
	desc.ConstantBufferSize = sizeof(GBufferConstantBuffer);
	desc.ConstantBufferBinding = 0;

	GBufferGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
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
		renderBackend->UploadTexture3D(BlueNoiseTex.get(), textureData.pData, textureData.RowPitch, textureData.SlicePitch);

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

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		GraphicsPipelineDesc desc{};
		desc.ShaderPath = GetAssetFullPath(L"Shaders\\ToneMapPS.hlsl");
		desc.VertexEntryPoint = "VSMain";
		desc.PixelEntryPoint = "PSMain";
		desc.VertexStride = vertexBufferStride;
		desc.ColorFormats = { DXGI_FORMAT_R8G8B8A8_UNORM };
		desc.bDepthEnable = false;
		desc.bCullBackFaces = false;
		desc.bTriangleStrip = true;
		desc.ConstantBufferSize = sizeof(ToneMapCB);
		desc.ConstantBufferBinding = 0;
		desc.VertexElements = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 16 }
		};
		desc.TextureBindings = {
			{ "SrcTex", 0 }
		};
		desc.SamplerBindings = {
			{ "sampleWrap", 0 }
		};

		ToneMapGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
		return;
	}
	
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
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		struct PostVertex
		{
			XMFLOAT4 position;
			XMFLOAT2 uv;
		};

		PostVertex quadVertices[] =
		{
			{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
			{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
			{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }
		};

		FullScreenVB = renderBackend->CreateVertexBuffer(sizeof(quadVertices), sizeof(PostVertex), &quadVertices);

		GraphicsPipelineDesc desc{};
		desc.ShaderPath = GetAssetFullPath(L"Shaders\\LightingPS.hlsl");
		desc.VertexEntryPoint = "VSMain";
		desc.PixelEntryPoint = "PSMain";
		desc.VertexStride = sizeof(PostVertex);
		desc.ColorFormats = { DXGI_FORMAT_R16G16B16A16_FLOAT };
		desc.bDepthEnable = false;
		desc.bCullBackFaces = false;
		desc.bTriangleStrip = true;
		desc.ConstantBufferSize = sizeof(LightingParam);
		desc.ConstantBufferBinding = 0;
		desc.VertexElements = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 16 }
		};
		desc.TextureBindings = {
			{ "AlbedoTex", 0 },
			{ "NormalTex", 1 },
			{ "ShadowTex", 2 },
			{ "VelocityTex", 3 },
			{ "DepthTex", 4 },
			{ "GIResultSHTex", 5 },
			{ "GIResultColorTex", 6 },
			{ "SpecularGITex", 7 },
			{ "RoughnessMetalicTex", 8 }
		};
		desc.SamplerBindings = {
			{ "sampleWrap", 0 }
		};

		LightingGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
		return;
	}

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
	shared_ptr<ComputePipelineStateObject> tempPSO = renderBackend->CreateComputePipelineStateObject();
	if (!tempPSO)
		return;
	tempPSO->BindSRV("ShadowTex", 0, 1);
	tempPSO->BindSRV("DepthTex", 1, 1);
	tempPSO->BindSRV("GeoNormalTex", 2, 1);
	tempPSO->BindUAV("OutShadow", 0);
	tempPSO->BindCBV("ShadowDenoiseCB", 0, sizeof(ShadowDenoiseCB));
	if (tempPSO->InitCS(GetAssetFullPath(L"Shaders\\ShadowDenoise.hlsl"), "ShadowDenoiseCS"))
		ShadowDenoisePSO = tempPSO;
}

void Corona::InitTemporalAAPass()
{
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		struct PostVertex
		{
			XMFLOAT4 position;
			XMFLOAT2 uv;
		};

		PostVertex quadVertices[] =
		{
			{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
			{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
			{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }
		};

		FullScreenVB = renderBackend->CreateVertexBuffer(sizeof(quadVertices), sizeof(PostVertex), &quadVertices);

		GraphicsPipelineDesc desc{};
		desc.ShaderPath = GetAssetFullPath(L"Shaders\\TemporalAA.hlsl");
		desc.VertexEntryPoint = "VSMain";
		desc.PixelEntryPoint = "PSMain";
		desc.VertexStride = sizeof(PostVertex);
		desc.ColorFormats = { DXGI_FORMAT_R16G16B16A16_FLOAT };
		desc.bDepthEnable = false;
		desc.bCullBackFaces = false;
		desc.bTriangleStrip = true;
		desc.ConstantBufferSize = sizeof(TemporalAAParam);
		desc.ConstantBufferBinding = 0;
		desc.VertexElements = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 16 }
		};
		desc.TextureBindings = {
			{ "CurrentColorTex", 0 },
			{ "PrevColorTex", 1 },
			{ "VelocityTex", 2 },
			{ "DepthTex", 3 },
			{ "BloomTex", 4 }
		};
		desc.BufferBindings = {
			{ "Exposure", 5 }
		};
		desc.SamplerBindings = {
			{ "sampleWrap", 0 }
		};

		TemporalAAGraphicsPipeline = renderBackend->CreateGraphicsPipeline(desc);
		return;
	}

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



void Corona::EnsureWindowFramebuffers()
{
	if (!renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::D3D12 || !framebuffers.empty())
		return;

	for (UINT i = 0; i < renderBackend->GetFrameCount(); i++)
	{
		ComPtr<ID3D12Resource> rendertarget = renderBackend->GetSwapChainBuffer(i);
		shared_ptr<Texture> rt = renderBackend->WrapNativeTexture(rendertarget);
		rt->MakeRTV();
		framebuffers.push_back(rt);
	}
}

void Corona::ApplyHybridDefaultCamera()
{
	const glm::vec3 position(0.0f, 0.2f, 3.2f);
	const float yaw = glm::pi<float>();
	const float pitch = -0.08f;

	m_camera.m_initialPosition = position;
	m_camera.m_position = position;
	m_camera.m_yaw = yaw;
	m_camera.m_pitch = pitch;
	m_camera.m_keysPressed = {};
	m_camera.m_mouseButtonDown = false;

	const float r = cosf(m_camera.m_pitch);
	m_camera.m_lookDirection.x = r * sinf(m_camera.m_yaw);
	m_camera.m_lookDirection.y = sinf(m_camera.m_pitch);
	m_camera.m_lookDirection.z = r * cosf(m_camera.m_yaw);

	bTemporalAAHistoryValid = false;
	bTemporalDenoiserHistoryValid = false;
	bResetTemporalStateNextUpdate = true;
}

void Corona::ToneMapPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("CopyPass");
#endif
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "CopyPass");
	}

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		if (!ToneMapGraphicsPipeline)
			return;

		Texture* ResolveTarget = nullptr;
		if (RenderingMode == ERenderingMode::PATHTRACING)
			ResolveTarget = PathTracingAccumBuffer[PathTracingWriteIndex].get();
		else if (bUseLightingBufferFallbackForToneMap && LightingBuffer)
			ResolveTarget = LightingBuffer.get();
		else
			ResolveTarget = ColorBuffers[ResolvedColorBufferIndex].get();
		if (!ResolveTarget)
			return;

		ToneMapCB.Offset = glm::vec4(0, 0, 0, 0);
		ToneMapCB.Scale = glm::vec4(1, 1, 0, 0);
		ToneMapCB.ToneMapMode = ToneMapMode;

		renderBackend->BindGraphicsPipelineTexture(ToneMapGraphicsPipeline.get(), "SrcTex", ResolveTarget);
		renderBackend->BindGraphicsPipelineSampler(ToneMapGraphicsPipeline.get(), "sampleWrap", samplerWrap.get());
		renderBackend->SetGraphicsPipelineConstantData(ToneMapGraphicsPipeline.get(), 0, &ToneMapCB, sizeof(ToneMapCB));
		renderBackend->BindGraphicsPipeline(ToneMapGraphicsPipeline.get());
		renderBackend->SetViewportAndScissor(m_width, m_height);
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
		return;
	}


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
	if (!renderBackend ||
		renderBackend->GetAPI() != ERenderBackendAPI::D3D12 ||
		!BufferVisualizePSO)
	{
		bDebugDraw = false;
		return;
	}

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
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "LightingPass");
	}

	renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

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

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		if (!LightingGraphicsPipeline)
			return;

		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "AlbedoTex", AlbedoBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "NormalTex", NormalBuffers[ColorBufferWriteIndex].get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "ShadowTex", ShadowDenoisedBuffer ? ShadowDenoisedBuffer.get() : ShadowBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "VelocityTex", VelocityBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "DepthTex", DepthBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "GIResultSHTex", DiffuseGISpatialAux[0].get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "GIResultColorTex", DiffuseGISpatial[0].get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "SpecularGITex", SpecularGISpatial[0].get());
		renderBackend->BindGraphicsPipelineTexture(LightingGraphicsPipeline.get(), "RoughnessMetalicTex", RoughnessMetalicBuffer.get());
		renderBackend->BindGraphicsPipelineSampler(LightingGraphicsPipeline.get(), "sampleWrap", samplerWrap.get());
		renderBackend->SetGraphicsPipelineConstantData(LightingGraphicsPipeline.get(), 0, &Param, sizeof(Param));

		Texture* lightingTarget = LightingBuffer.get();
		renderBackend->SetRenderTargets(&lightingTarget, 1, nullptr);
		renderBackend->BindGraphicsPipeline(LightingGraphicsPipeline.get());
		renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
		renderBackend->TransitionTexture(LightingBuffer.get(), EResourceState::RenderTarget, EResourceState::ShaderRead);
		return;
	}

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
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalAAPass");
	}

	UINT PrevColorBufferIndex = 1 - ColorBufferWriteIndex;
	Texture* ResolveTarget = ColorBuffers[ColorBufferWriteIndex].get();

	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		if (!TemporalAAGraphicsPipeline || !ResolveTarget || !LightingBuffer || !ExposureData)
		{
			bUseLightingBufferFallbackForToneMap = true;
			bTemporalAAHistoryValid = false;
			return;
		}

		Texture* PrevColorBuffer = ColorBuffers[PrevColorBufferIndex].get();
		Texture* BloomTexture = BloomBlurPingPong[0] ? BloomBlurPingPong[0].get() : DefaultBlackTex.get();
		if (!PrevColorBuffer || !BloomTexture)
		{
			bUseLightingBufferFallbackForToneMap = true;
			bTemporalAAHistoryValid = false;
			return;
		}

		TemporalAAParam Param;
		Param.RTSize.x = GetRenderWidth();
		Param.RTSize.y = GetRenderHeight();
		Param.TAABlendFactor = IsTemporalAAEnabled() ? 0.1f : 1.0f;
		Param.ClampMode = ClampMode;
		Param.BloomStrength = BloomBlurPingPong[0] ? BloomStrength : 0.0f;
		Param.HistoryValid = bTemporalAAHistoryValid ? 1u : 0u;

		renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "CurrentColorTex", LightingBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "PrevColorTex", PrevColorBuffer);
		renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "VelocityTex", VelocityBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "DepthTex", DepthBuffer.get());
		renderBackend->BindGraphicsPipelineTexture(TemporalAAGraphicsPipeline.get(), "BloomTex", BloomTexture);
		renderBackend->BindGraphicsPipelineBuffer(TemporalAAGraphicsPipeline.get(), "Exposure", ExposureData.get());
		renderBackend->BindGraphicsPipelineSampler(TemporalAAGraphicsPipeline.get(), "sampleWrap", samplerBilinearWrap ? samplerBilinearWrap.get() : samplerWrap.get());
		renderBackend->SetGraphicsPipelineConstantData(TemporalAAGraphicsPipeline.get(), 0, &Param, sizeof(Param));

		renderBackend->TransitionTexture(ResolveTarget, EResourceState::ShaderRead, EResourceState::RenderTarget);
		Texture* temporalTarget = ResolveTarget;
		renderBackend->SetRenderTargets(&temporalTarget, 1, nullptr);
		renderBackend->BindGraphicsPipeline(TemporalAAGraphicsPipeline.get());
		renderBackend->SetViewportAndScissor(GetRenderWidth(), GetRenderHeight());
		renderBackend->DrawFullscreenQuad(FullScreenVB.get());
		renderBackend->TransitionTexture(ResolveTarget, EResourceState::RenderTarget, EResourceState::ShaderRead);

		bTemporalAAHistoryValid = IsTemporalAAEnabled();
		bUseLightingBufferFallbackForToneMap = false;
		ResolvedColorBufferIndex = ColorBufferWriteIndex;
		return;
	}

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
	UpdateCameraPathState();

	const float effectiveNear = Near;
	const float effectiveFar = Far;

	ViewMat = m_camera.GetViewMatrix();
	ProjMat = m_camera.GetProjectionMatrix(Fov, m_aspectRatio, effectiveNear, effectiveFar);
	UnjitteredProjMat = ProjMat;
	UnjitteredViewProjMat = ProjMat * ViewMat;

	InvViewMat = glm::inverse(ViewMat);
	InvProjMat = glm::inverse(ProjMat);

	RTShadowViewParam.ViewMatrix = glm::transpose(ViewMat);
	RTShadowViewParam.InvViewMatrix = glm::transpose(InvViewMat);
	RTShadowViewParam.ProjMatrix = glm::transpose(ProjMat);
	RTShadowViewParam.InvProjMatrix = glm::transpose(InvProjMat);
	RTShadowViewParam.ProjectionParams.x = effectiveFar / (effectiveFar - effectiveNear);
	RTShadowViewParam.ProjectionParams.y = effectiveNear / (effectiveNear - effectiveFar);
	RTShadowViewParam.ProjectionParams.z = effectiveNear;
	RTShadowViewParam.ProjectionParams.w = effectiveFar;
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
	RTReflectionViewParam.ProjectionParams.x = effectiveFar / (effectiveFar - effectiveNear);
	RTReflectionViewParam.ProjectionParams.y = effectiveNear / (effectiveNear - effectiveFar);
	RTReflectionViewParam.ProjectionParams.z = effectiveNear;
	RTReflectionViewParam.ProjectionParams.w = effectiveFar;
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
	RTGIViewParam.ProjectionParams.x = effectiveFar / (effectiveFar - effectiveNear);
	RTGIViewParam.ProjectionParams.y = effectiveNear / (effectiveNear - effectiveFar);
	RTGIViewParam.ProjectionParams.z = effectiveNear;
	RTGIViewParam.ProjectionParams.w = effectiveFar;
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
	PathTracingViewParam.ProjectionParams.x = effectiveFar / (effectiveFar - effectiveNear);
	PathTracingViewParam.ProjectionParams.y = effectiveNear / (effectiveNear - effectiveFar);
	PathTracingViewParam.ProjectionParams.z = effectiveNear;
	PathTracingViewParam.ProjectionParams.w = effectiveFar;
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
	
	SpatialFilterCB.ProjectionParams.z = effectiveNear;
	SpatialFilterCB.ProjectionParams.w = effectiveFar;
	SpatialFilterCB.AccumulatedFrames = IndirectAccumulatedFrames;


	TemporalFilterCB.InvViewMatrix = glm::transpose(InvViewMat);
	TemporalFilterCB.InvProjMatrix = glm::transpose(InvProjMat);
	TemporalFilterCB.ProjectionParams.z = effectiveNear;
	TemporalFilterCB.ProjectionParams.w = effectiveFar;
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
	BeginFramePerfLogging();
	double beginFrameMs = 0.0;
	double executeMs = 0.0;
	double endFrameMs = 0.0;

	const auto beginFrameStart = CpuClock::now();
	renderBackend->BeginFrame();
	beginFrameMs = ElapsedMilliseconds(beginFrameStart, CpuClock::now());
	AppendVulkanRuntimeTrace(L"[OnRender] after BeginFrame");
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
		const bool bStageDump = IsHybridStageAutoDumpPhase();
		const uint32_t maxSupportedHybridStage = renderBackend ? renderBackend->GetMaxSupportedHybridStage() : 7u;
		const uint32_t requestedHybridStage = bStageDump ? AutoAADumpPhase : 7u;
		const uint32_t hybridStage = std::min(requestedHybridStage, maxSupportedHybridStage);
		if (!bLoggedHybridStageLimit && requestedHybridStage > hybridStage)
		{
			bLoggedHybridStageLimit = true;
			AppendVulkanRuntimeTrace(
				std::wstring(L"[OnRender] hybrid stage limited by backend to ") +
				GetHybridStageAutoDumpPhaseName(hybridStage));
		}
		const bool bRunShadow = hybridStage >= 1;
		const bool bRunShadowDenoise = hybridStage >= 2;
		const bool bRunReflection = hybridStage >= 3;
		const bool bRunGI = hybridStage >= 4;
		const bool bRunTemporalDenoise = hybridStage >= 5;
		const bool bRunSpatialDenoise = hybridStage >= 6;
		const bool bRunLighting = hybridStage >= 7;
		const bool bVulkanHybridBackend =
			renderBackend &&
			renderBackend->GetAPI() == ERenderBackendAPI::Vulkan;

		// Hybrid rendering: Rasterization GBuffer + Raytracing
		BeginGpuPassTiming(EGpuPass::GBuffer);
		AppendVulkanRuntimeTrace(L"[OnRender] before GBufferPass");
		GBufferPass();
		AppendVulkanRuntimeTrace(L"[OnRender] after GBufferPass");
		EndGpuPassTiming(EGpuPass::GBuffer);

		if (bRunShadow)
		{
			BeginGpuPassTiming(EGpuPass::RaytraceShadow);
			AppendVulkanRuntimeTrace(L"[OnRender] before RaytraceShadowPass");
			RaytraceShadowPass();
			AppendVulkanRuntimeTrace(L"[OnRender] after RaytraceShadowPass");
			EndGpuPassTiming(EGpuPass::RaytraceShadow);
		}

		if (bRunShadowDenoise)
		{
			BeginGpuPassTiming(EGpuPass::ShadowDenoise);
			AppendVulkanRuntimeTrace(L"[OnRender] before ShadowDenoisePass");
			ShadowDenoisePass();
			AppendVulkanRuntimeTrace(L"[OnRender] after ShadowDenoisePass");
			EndGpuPassTiming(EGpuPass::ShadowDenoise);
		}

		if (bRunReflection)
		{
			BeginGpuPassTiming(EGpuPass::RaytraceReflection);
			AppendVulkanRuntimeTrace(L"[OnRender] before RaytraceReflectionPass");
			RaytraceReflectionPass();
			AppendVulkanRuntimeTrace(L"[OnRender] after RaytraceReflectionPass");
			EndGpuPassTiming(EGpuPass::RaytraceReflection);
		}

		if (bRunGI)
		{
			BeginGpuPassTiming(EGpuPass::RaytraceGI);
			AppendVulkanRuntimeTrace(L"[OnRender] before RaytraceGIPass");
			RaytraceGIPass();
			AppendVulkanRuntimeTrace(L"[OnRender] after RaytraceGIPass");
			EndGpuPassTiming(EGpuPass::RaytraceGI);
		}

		// Simple GI denoising: edge-aware temporal accumulation + spatial bilateral.
		if (bRunTemporalDenoise)
		{
			BeginGpuPassTiming(EGpuPass::TemporalDenoise);
			AppendVulkanRuntimeTrace(L"[OnRender] before TemporalDenoisingPass");
			TemporalDenoisingPass();
			AppendVulkanRuntimeTrace(L"[OnRender] after TemporalDenoisingPass");
			EndGpuPassTiming(EGpuPass::TemporalDenoise);
		}
		// GenMipSpecularGIPass();
		if (bRunSpatialDenoise)
		{
			BeginGpuPassTiming(EGpuPass::SpatialDenoise);
			AppendVulkanRuntimeTrace(L"[OnRender] before SpatialDenoisingPass");
			SpatialDenoisingPass();
			AppendVulkanRuntimeTrace(L"[OnRender] after SpatialDenoisingPass");
			EndGpuPassTiming(EGpuPass::SpatialDenoise);
		}

		if (bRunLighting)
		{
			BeginGpuPassTiming(EGpuPass::Lighting);
			LightingPass();
			EndGpuPassTiming(EGpuPass::Lighting);
		}

		// BloomPass(); // Disabled for hybrid mode

		if (bRunLighting && IsDLSSRREnabled())
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
		else if (bRunLighting && IsDLSSSREnabled())
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
		else if (bRunLighting)
		{
			if (bVulkanHybridBackend && !TemporalAAGraphicsPipeline)
			{
				bUseLightingBufferFallbackForToneMap = true;
				bTemporalAAHistoryValid = false;
			}
			else if (!bVulkanHybridBackend && DLSSTransitionFramesRemaining > 0)
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

	
	Texture* backbuffer = nullptr;
	if (renderBackend->GetCurrentFrameIndex() < framebuffers.size())
		backbuffer = framebuffers[renderBackend->GetCurrentFrameIndex()].get();
	if (!backbuffer)
		backbuffer = renderBackend->GetCurrentWindowRenderTarget();
	const bool bVulkanHybridBackend =
		RenderingMode == ERenderingMode::HYBRID &&
		renderBackend &&
		renderBackend->GetAPI() == ERenderBackendAPI::Vulkan;
	const bool bVulkanHybridAutoDump = bVulkanHybridBackend && IsHybridStageAutoDumpPhase();
	const uint32_t vulkanPreviewStage =
		bVulkanHybridBackend
			? (bVulkanHybridAutoDump
				? std::min<uint32_t>(AutoAADumpPhase, renderBackend->GetMaxSupportedHybridStage())
				: renderBackend->GetMaxSupportedHybridStage())
			: 7u;
	const bool bVulkanStagePreview = bVulkanHybridBackend && vulkanPreviewStage < 7u;
	const UINT32 vulkanStageDumpCaptureFrame =
		bVulkanHybridAutoDump
			? GetHybridStageAutoDumpFrameCount(
				AutoAADumpPhase,
				true,
				renderBackend->GetMaxSupportedHybridStage() < 7u) - 1u
			: 7u;
	const bool bRequestVulkanStageDumpCapture =
		bVulkanHybridAutoDump &&
		AutoAADumpFramesInPhase == vulkanStageDumpCaptureFrame;
	if (bRequestVulkanStageDumpCapture)
	{
		AppendVulkanRuntimeTrace(L"[OnRender] request stage dump capture");
		renderBackend->RequestWindowCapture(
			AutoAADumpDir + L"\\" + GetHybridStageAutoDumpPhaseName(AutoAADumpPhase) + L"_screen_preview.png");
	}
	if (bVulkanStagePreview)
	{
		if (auto* vkBackend = dynamic_cast<VulkanBackend*>(renderBackend.get()))
		{
			Texture* previewTexture = nullptr;
			switch (vulkanPreviewStage)
			{
			case 0: previewTexture = AlbedoBuffer.get(); break;
			case 1: previewTexture = ShadowBuffer.get(); break;
			case 2: previewTexture = ShadowDenoisedBuffer.get(); break;
			case 3: previewTexture = SpecularGIRaw.get(); break;
			case 4: previewTexture = DiffuseGIRaw.get(); break;
			case 5: previewTexture = DiffuseGITemporal[GIBufferWriteIndex].get(); break;
			case 6: previewTexture = DiffuseGISpatial[0].get(); break;
			default: previewTexture = nullptr; break;
			}
			AppendVulkanRuntimeTrace(L"[OnRender] before PreviewTextureOnWindow");
			vkBackend->PreviewTextureOnWindow(previewTexture ? previewTexture : AlbedoBuffer.get());
			AppendVulkanRuntimeTrace(L"[OnRender] after PreviewTextureOnWindow");
		}
	}
	else
	{
		renderBackend->TransitionTexture(backbuffer, EResourceState::Present, EResourceState::RenderTarget);
		renderBackend->SetRenderTarget(backbuffer);
	}
	if (!bVulkanStagePreview && RenderingMode == ERenderingMode::HYBRID &&
		IsHybridStageAutoDumpPhase() && AutoAADumpPhase < 7)
	{
		static const float kStageClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		renderBackend->ClearRenderTarget(backbuffer, kStageClearColor);
	}
	else if (!bVulkanStagePreview)
	{
		BeginGpuPassTiming(EGpuPass::ToneMap);
		ToneMapPass();
		EndGpuPassTiming(EGpuPass::ToneMap);
	}
	AdvanceAutoAADump(backbuffer);

	if(bDebugDraw)
	{
		BeginGpuPassTiming(EGpuPass::Debug);
		DebugPass();
		EndGpuPassTiming(EGpuPass::Debug);
	}

	bool bSuppressImguiForCapture = false;
	if (bCameraPathDumping)
	{
		if (!bVulkanStagePreview)
		{
			RequestCameraPathDumpFrameCapture();
			bSuppressImguiForCapture = true;
		}
		else
		{
			StopCameraPathDump();
			LastCameraPathStatus = L"Camera path dumping is not available while a Vulkan stage preview is active.";
		}
	}

	if (!bSuppressImguiForCapture && bFinalScreenshotRequested)
	{
		if (!bVulkanStagePreview)
		{
			RequestFinalBackbufferScreenshot();
			bSuppressImguiForCapture = bFinalScreenshotCaptureInFlight;
		}
		else
		{
			bFinalScreenshotRequested = false;
			LastFinalScreenshotStatus = L"Screenshot is not available while a Vulkan stage preview is active.";
		}
	}

	if (!bSuppressImguiForCapture && bShowImgui && bImguiInitialized)
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

		if (bFinalScreenshotRequested || bFinalScreenshotCaptureInFlight)
			ImGui::BeginDisabled();
		if (ImGui::Button("Capture Final Backbuffer"))
		{
			bFinalScreenshotRequested = true;
			LastFinalScreenshotStatus = L"Screenshot will be captured on the next frame without ImGui.";
		}
		if (bFinalScreenshotRequested || bFinalScreenshotCaptureInFlight)
			ImGui::EndDisabled();
		if (!LastFinalScreenshotStatus.empty())
		{
			const std::string statusText = WideToUtf8(LastFinalScreenshotStatus);
			ImGui::TextWrapped("Screenshot: %s", statusText.c_str());
		}

		ImGui::Separator();
		ImGui::Text("Camera Path Capture");
		const bool bCameraPathBusy = bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping;
		if (bCameraPathBusy)
			ImGui::BeginDisabled();
		if (ImGui::Button("Start Camera Path"))
		{
			StartCameraPathRecording();
		}
		if (bCameraPathBusy)
			ImGui::EndDisabled();
		ImGui::SameLine();
		if (!bCameraPathRecording)
			ImGui::BeginDisabled();
		if (ImGui::Button("End Camera Path"))
		{
			EndCameraPathRecording();
		}
		if (!bCameraPathRecording)
			ImGui::EndDisabled();

		if (CameraPathKeyframes.size() < 2 || bCameraPathRecording || bCameraPathDumping)
			ImGui::BeginDisabled();
		if (ImGui::Button(bCameraPathPlaying ? "Stop Playback" : "Play Camera Path"))
		{
			if (bCameraPathPlaying)
				StopCameraPathPlayback();
			else
				StartCameraPathPlayback();
		}
		if (CameraPathKeyframes.size() < 2 || bCameraPathRecording || bCameraPathDumping)
			ImGui::EndDisabled();
		ImGui::SameLine();
		if (bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
			ImGui::BeginDisabled();
		if (ImGui::Button("Load Latest Path"))
		{
			LoadLatestCameraPath();
		}
		if (bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
			ImGui::EndDisabled();

		if (CameraPathKeyframes.size() < 2 || bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
			ImGui::BeginDisabled();
		if (ImGui::Button("Play + Dump 30fps PNG Sequence"))
		{
			StartCameraPathDump();
		}
		if (CameraPathKeyframes.size() < 2 || bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
			ImGui::EndDisabled();
		ImGui::SameLine();
		if (!bCameraPathDumping)
			ImGui::BeginDisabled();
		if (ImGui::Button("Stop Playback + Dump"))
		{
			StopCameraPathPlayback();
		}
		if (!bCameraPathDumping)
			ImGui::EndDisabled();

		if (LastCameraPathDumpDir.empty() || bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
			ImGui::BeginDisabled();
		if (ImGui::Button("Convert Last Dump To MP4"))
		{
			LaunchCameraPathVideoEncode();
		}
		if (LastCameraPathDumpDir.empty() || bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
			ImGui::EndDisabled();

		ImGui::Text("Path keyframes: %u, duration: %.2fs",
			static_cast<unsigned>(CameraPathKeyframes.size()),
			GetCameraPathDurationSeconds());
		if (bCameraPathDumping)
		{
			ImGui::Text("Dump progress: %u / %u",
				static_cast<unsigned>(CameraPathDumpFrameIndex),
				static_cast<unsigned>(CameraPathDumpFrameCount));
		}
		if (!LastCameraPathStatus.empty())
		{
			const std::string statusText = WideToUtf8(LastCameraPathStatus);
			ImGui::TextWrapped("Camera path: %s", statusText.c_str());
		}
		if (!LastCameraPathVideoCommand.empty())
		{
			const std::string commandText = WideToUtf8(LastCameraPathVideoCommand);
			ImGui::TextWrapped("Video command: %s", commandText.c_str());
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
		const bool bDebugVisualizationAvailable =
			renderBackend &&
			renderBackend->GetAPI() == ERenderBackendAPI::D3D12 &&
			BufferVisualizePSO != nullptr;
		if (bDebugVisualizationAvailable)
		{
			ImGui::Checkbox("Visualize Buffers", &bDebugDraw);
		}
		else
		{
			bDebugDraw = false;
			bool disabledDebugDraw = false;
			ImGui::BeginDisabled();
			ImGui::Checkbox("Visualize Buffers", &disabledDebugDraw);
			ImGui::EndDisabled();
			ImGui::TextDisabled("Visualize Buffers is currently available on the DX12 backend.");
		}
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
				"MESH TEST",
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
		const bool bCameraPathOwnsLightControls = bCameraPathPlaying || bCameraPathDumping;
		if (bCameraPathOwnsLightControls)
			ImGui::BeginDisabled();
		glm::vec3 LD = glm::vec3(LightDir.z, -LightDir.y, -LightDir.x);
		ImGui::gizmo3D("##gizmo1", LD, 200 /* mode */);
		LightDir = glm::vec3(-LD.z, -LD.y, LD.x);
		ImGui::SameLine();
		ImGui::Text("Light Direction");


		ImGui::SliderFloat("Light Brightness", &LightIntensity, 0.0f, 20.0f);
		if (bCameraPathOwnsLightControls)
		{
			ImGui::EndDisabled();
			ImGui::TextDisabled("Camera path playback is controlling the directional light.");
		}

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


	const auto executeStart = CpuClock::now();
	renderBackend->ExecuteCurrentCommandList();
	executeMs = ElapsedMilliseconds(executeStart, CpuClock::now());
	AppendVulkanRuntimeTrace(L"[OnRender] after ExecuteCurrentCommandList");

	const auto endFrameStart = CpuClock::now();
	renderBackend->EndFrame();
	endFrameMs = ElapsedMilliseconds(endFrameStart, CpuClock::now());
	AppendVulkanRuntimeTrace(L"[OnRender] after EndFrame");
	FinishFramePerfLogging(beginFrameMs, executeMs, endFrameMs);

	ConsumeCameraPathDumpCaptureResult();
	ConsumeFinalBackbufferScreenshotResult();

	if ((bVulkanStagePreview || bVulkanHybridAutoDump) && bAutoAADumpEnabled && bAutoAADumpInitialized && !AutoAADumpDir.empty())
	{
		std::wstring outputPath;
		std::wstring errorMessage;
		bool bCaptureSuccess = false;
		if (renderBackend->ConsumeWindowCaptureResult(&outputPath, &bCaptureSuccess, &errorMessage))
		{
			AppendAutoAADumpLog(std::wstring(L"[capture] ") + outputPath + L" = " + (bCaptureSuccess ? L"ok" : L"fail"));
			if (!errorMessage.empty())
				AppendAutoAADumpLog(L"[capture] " + errorMessage);
		}
	}

	PrevViewProjMat = ViewProjMat;
	PrevViewMat = ViewMat;

	PrevUnjitteredViewProjMat = UnjitteredViewProjMat;
}

void Corona::OnDestroy()
{
	SaveCameraState();
	if (!renderBackend)
	{
		CoUninitialize();
		return;
	}
	renderBackend->WaitForGpu();
	renderBackend->ShutdownGpuTimestampQueries();

#if WITH_STREAMLINE
	// Streamline can spend a long time in plugin shutdown after automated captures.
	// The process is exiting immediately, so let the OS tear it down in this path.
	if (!bAutoAADumpEnabled)
		ShutdownStreamline();
#endif
	if (bImguiInitialized)
	{
		renderBackend->ShutdownImGuiBackend();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		bImguiInitialized = false;
	}
	CoUninitialize();
}

void Corona::OnKeyDown(UINT8 key)
{
	switch (key)
	{
	/*case 'M':
		bMultiThreadRendering = !bMultiThreadRendering;
		break;*/
	case 'B':
		if (renderBackend &&
			renderBackend->GetAPI() == ERenderBackendAPI::D3D12 &&
			BufferVisualizePSO)
		{
			bDebugDraw = !bDebugDraw;
		}
		else
		{
			bDebugDraw = false;
		}
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
			GBufferConstantBuffer objCB = {};
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
			objCB.ViewDir.w = 0.0f;

			objCB.RTSize.x = GetRenderWidth();
			objCB.RTSize.y = GetRenderHeight();

			objCB.RougnessMetalic.x = Roughness;
			objCB.RougnessMetalic.y = Metalic;

			objCB.bOverrideRougnessMetallic = bOverrideRoughnessMetallic ? 1 : 0;

			renderBackend->SetGraphicsPipelineConstantData(GBufferGraphicsPipeline.get(), 0, &objCB, sizeof(objCB));

			Texture* AlbedoTex = drawcall.mat->Diffuse ? drawcall.mat->Diffuse.get() : DefaultWhiteTex.get();
			Texture* NormalTex = drawcall.mat->Normal ? drawcall.mat->Normal.get() : DefaultNormalTex.get();
			Texture* RoughnessTex = drawcall.mat->Roughness ? drawcall.mat->Roughness.get() : DefaultRougnessTex.get();
			Texture* MetallicTex = drawcall.mat->Metallic ? drawcall.mat->Metallic.get() : DefaultBlackTex.get();

			renderBackend->BindGraphicsPipelineTexture(GBufferGraphicsPipeline.get(), "AlbedoTex", AlbedoTex);
			renderBackend->BindGraphicsPipelineTexture(GBufferGraphicsPipeline.get(), "NormalTex", NormalTex);
			renderBackend->BindGraphicsPipelineTexture(GBufferGraphicsPipeline.get(), "RoughnessTex", RoughnessTex);
			renderBackend->BindGraphicsPipelineTexture(GBufferGraphicsPipeline.get(), "MetallicTex", MetallicTex);


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
	AppendVulkanRuntimeTrace(L"[GBufferPass] begin");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "GBufferPass");
	}

	renderBackend->TransitionTexture(AlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(SpecularAlbedoBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(NormalBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(GeomNormalBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(VelocityBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	renderBackend->TransitionTexture(RoughnessMetalicBuffer.get(), EResourceState::ShaderRead, EResourceState::RenderTarget);

	renderBackend->TransitionTexture(DepthBuffer.get(), EResourceState::ShaderRead, EResourceState::DepthWrite);
	renderBackend->TransitionTexture(UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), EResourceState::ShaderRead, EResourceState::RenderTarget);
	AppendVulkanRuntimeTrace(L"[GBufferPass] after transitions");

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
	AppendVulkanRuntimeTrace(L"[GBufferPass] after clears");


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
	renderBackend->SetRenderTargets(renderTargets, static_cast<uint32_t>(std::size(renderTargets)), DepthBuffer.get());
	AppendVulkanRuntimeTrace(L"[GBufferPass] after SetRenderTargets");

	renderBackend->BindGraphicsPipeline(GBufferGraphicsPipeline.get());
	renderBackend->BindGraphicsPipelineSampler(GBufferGraphicsPipeline.get(), "samplerWrap", samplerWrap.get());
	AppendVulkanRuntimeTrace(L"[GBufferPass] after BindGraphicsPipeline");

	if (!bMultiThreadRendering)
	{
		AppendVulkanRuntimeTrace(L"[GBufferPass] before DrawScene");
		DrawScene(Sponza, SponzaRoughnessMultiplier, 0, false);
		AppendVulkanRuntimeTrace(L"[GBufferPass] after DrawScene");
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
	AppendVulkanRuntimeTrace(L"[GBufferPass] end");
}

void Corona::SpatialDenoisingPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("SpatialDenoisingPass");
#endif
	if (!SpatialDenoisingFilterPSO)
		return;
	AppendVulkanRuntimeTrace(L"[SpatialDenoisingPass] begin");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "SpatialDenoisingPass");
	}

	UINT WriteIndex = 0;
	UINT ReadIndex = 1;
	for (int i = 0; i < 4; i++)
	{
		WriteIndex = 1 - WriteIndex; // 1
		ReadIndex = 1 - WriteIndex; // 0

		renderBackend->TransitionTexture(DiffuseGISpatialAux[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(DiffuseGISpatial[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
		renderBackend->TransitionTexture(SpecularGISpatial[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

		SpatialDenoisingFilterPSO->SetTextureSRV("DepthTex", DepthBuffer.get());
		SpatialDenoisingFilterPSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffer.get());
		if (i == 0)
		{
			SpatialDenoisingFilterPSO->SetTextureSRV("InGIResultSHTex", DiffuseGITemporalAux[GIBufferWriteIndex].get());
			SpatialDenoisingFilterPSO->SetTextureSRV("InGIResultColorTex", DiffuseGITemporal[GIBufferWriteIndex].get());
			SpatialDenoisingFilterPSO->SetTextureSRV("InSpecularGITex", SpecularGITemporal[GIBufferWriteIndex].get());
		}
		else
		{
			SpatialDenoisingFilterPSO->SetTextureSRV("InGIResultSHTex", DiffuseGISpatialAux[ReadIndex].get());
			SpatialDenoisingFilterPSO->SetTextureSRV("InGIResultColorTex", DiffuseGISpatial[ReadIndex].get());
			SpatialDenoisingFilterPSO->SetTextureSRV("InSpecularGITex", SpecularGISpatial[ReadIndex].get());
		}


		SpatialDenoisingFilterPSO->SetTextureUAV("OutGIResultSH", DiffuseGISpatialAux[WriteIndex].get());
		SpatialDenoisingFilterPSO->SetTextureUAV("OutGIResultColor", DiffuseGISpatial[WriteIndex].get());
		SpatialDenoisingFilterPSO->SetTextureUAV("OutSpecularGI", SpecularGISpatial[WriteIndex].get());

		SpatialFilterCB.Iteration = i;
		SpatialDenoisingFilterPSO->SetCBVValue("SpatialFilterConstant", &SpatialFilterCB);
		SpatialDenoisingFilterPSO->Apply();
		AppendVulkanRuntimeTrace(L"[SpatialDenoisingPass] after Apply iteration=" + std::to_wstring(i));

		UINT WidthGI = GetRenderWidth();
		UINT HeightGI = GetRenderHeight();

		renderBackend->Dispatch((WidthGI + 31) / 32, (HeightGI + 31) / 32, 1);
		AppendVulkanRuntimeTrace(L"[SpatialDenoisingPass] after Dispatch iteration=" + std::to_wstring(i));

		renderBackend->TransitionTexture(DiffuseGISpatialAux[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(DiffuseGISpatial[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
		renderBackend->TransitionTexture(SpecularGISpatial[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	}
	AppendVulkanRuntimeTrace(L"[SpatialDenoisingPass] end");
}

void Corona::TemporalDenoisingPass()
{
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("TemporalDenoisingPass");
#endif
	if (!TemporalDenoisingFilterPSO)
		return;
	AppendVulkanRuntimeTrace(L"[TemporalDenoisingPass] begin");
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "TemporalDenoisingPass");
	}

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
	renderBackend->TransitionTexture(SpecularGIMoments[WriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	TemporalDenoisingFilterPSO->SetTextureSRV("DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("NormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultSHTex", DiffuseGIRawAux.get());
	TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultColorTex", DiffuseGIRaw.get());
	TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultSHTexPrev", DiffuseGITemporalAux[ReadIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("InGIResultColorTexPrev", DiffuseGITemporal[ReadIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("VelocityTex", VelocityBuffer.get());
	TemporalDenoisingFilterPSO->SetTextureSRV("InSpecularGITex", SpecularGIRaw.get());
	TemporalDenoisingFilterPSO->SetTextureSRV("InSpecularGITexPrev", SpecularGITemporal[ReadIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("RougnessMetalicTex", RoughnessMetalicBuffer.get());
	TemporalDenoisingFilterPSO->SetTextureSRV("PrevDepthTex", UnjitteredDepthBuffers[1 - ColorBufferWriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("PrevNormalTex", NormalBuffers[1 - ColorBufferWriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureSRV("PrevMomentsTex", SpecularGIMoments[ReadIndex].get());


	TemporalDenoisingFilterPSO->SetTextureUAV("OutGIResultSH", DiffuseGITemporalAux[WriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutGIResultColor", DiffuseGITemporal[WriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutGIResultSHDS", DiffuseGISpatialAux[0].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutGIResultColorDS", DiffuseGISpatial[0].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutSpecularGI", SpecularGITemporal[WriteIndex].get());
	TemporalDenoisingFilterPSO->SetTextureUAV("OutMoments", SpecularGIMoments[WriteIndex].get());
	//TemporalDenoisingFilterPSO->SetUAV("OutSpecularGIDS", SpecularGISpatial[0]->GpuHandleUAV, renderBackend->GetGraphicsCommandList());

	TemporalDenoisingFilterPSO->SetSampler("BilinearClamp", samplerBilinearWrap.get());
	TemporalFilterCB.HistoryValid = bTemporalDenoiserHistoryValid ? 1u : 0u;

	TemporalDenoisingFilterPSO->SetCBVValue("TemporalFilterConstant", &TemporalFilterCB);
	TemporalDenoisingFilterPSO->Apply();
	AppendVulkanRuntimeTrace(L"[TemporalDenoisingPass] after Apply");

	renderBackend->Dispatch((GetRenderWidth() + 14) / 15, (GetRenderHeight() + 14) / 15, 1);
	AppendVulkanRuntimeTrace(L"[TemporalDenoisingPass] after Dispatch");

	renderBackend->TransitionTexture(DiffuseGISpatialAux[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGISpatial[0].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGITemporalAux[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(DiffuseGITemporal[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(SpecularGITemporal[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	renderBackend->TransitionTexture(SpecularGIMoments[WriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	bTemporalDenoiserHistoryValid = true;
	IndirectAccumulatedFrames = std::min(IndirectAccumulatedFrames + 1u, 1024u);
	AppendVulkanRuntimeTrace(L"[TemporalDenoisingPass] end");
}



// Helper function to add scene meshes to BLAS vector
void AddMeshesToBLAS(vector<shared_ptr<RTAS>>& vecBLAS, shared_ptr<Scene> scene)
{
	for (auto& mesh : scene->meshes)
	{
		shared_ptr<RTAS> blas = mesh->CreateBLAS();
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
	InitPathTracingPass();
	InitSpatialDenoisingPass();
	InitTemporalDenoisingPass();
	InitGBufferPass();
	InitToneMapPass();
	InitLightingPass();
	InitTemporalAAPass();
	if (!renderBackend || renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		InitDebugPass();
		InitBloomPass();
	}
	//InitGenMipSpecularGIPass();
}

void Corona::UpdateInstancePropertyBuffer()
{
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
	{
		std::vector<InstanceProperty> instanceProperties(500u);
		const size_t instanceCount = (std::min)(instanceProperties.size(), vecBLAS.size());
		for (size_t i = 0; i < instanceCount; ++i)
		{
			instanceProperties[i].WorldMatrix = glm::transpose(vecBLAS[i]->MeshPtr->transform);
			instanceProperties[i].VertexOffset = vecBLAS[i]->MeshPtr->RtVertexOffset;
			instanceProperties[i].IndexOffset = vecBLAS[i]->MeshPtr->RtIndexOffset;
		}

		InstancePropertyBuffer = renderBackend->CreateBuffer({
			static_cast<uint32_t>(instanceProperties.size()),
			sizeof(InstanceProperty),
			EInitialResourceState::ShaderRead,
			false,
			instanceProperties.data()
		});
		InstancePropertyBuffer->MakeByteAddressBufferSRV();
		NAME_D3D12_OBJECT(InstancePropertyBuffer->resource);
		return;
	}

	// Map and update instance properties
	uint8_t* pData;
	InstancePropertyBuffer->resource->Map(0, nullptr, (void**)&pData);

	for (auto& m : vecBLAS)
	{
		InstanceProperty instanceProperty = {};
		instanceProperty.WorldMatrix = glm::transpose(m->MeshPtr->transform);
		memcpy(pData, &instanceProperty, sizeof(InstanceProperty));
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

	if (PSO_PATH_TRACING)
		InitPathTracingPass();
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
	const uint32_t maxSupportedHybridStage = renderBackend ? renderBackend->GetMaxSupportedHybridStage() : 7u;
	const bool bInitReflectionRT = !renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::Vulkan || maxSupportedHybridStage >= 3u;
	const bool bInitGIRT = !renderBackend || renderBackend->GetAPI() != ERenderBackendAPI::Vulkan || maxSupportedHybridStage >= 4u;

	// create shadow rtpso
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_SHADOW = renderBackend->CreateRTPipelineStateObject();
		if (!TEMP_PSO_RT_SHADOW)
			return;
		TEMP_PSO_RT_SHADOW->SetNumInstances(static_cast<uint32_t>(vecBLAS.size()));// scene->meshes.size(); // important for cbv allocation & shadertable size.

		// new interface
		TEMP_PSO_RT_SHADOW->AddHitGroup("HitGroup", "", "anyhit");
		TEMP_PSO_RT_SHADOW->AddShader("rayGen", RTPipelineStateObject::RAYGEN);

		TEMP_PSO_RT_SHADOW->BindUAV("global", "ShadowResult", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_SHADOW->BindSRV("global", "WorldNormalTex", 2);

		TEMP_PSO_RT_SHADOW->BindCBV("global", "ViewParameter", 0, sizeof(RTShadowViewParamCB), 1);
		TEMP_PSO_RT_SHADOW->BindSampler("global", "sampleWrap", 0);



		TEMP_PSO_RT_SHADOW->AddShader("miss", RTPipelineStateObject::MISS);
		
		TEMP_PSO_RT_SHADOW->AddShader("anyhit", RTPipelineStateObject::ANYHIT);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "vertices", 3);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "indices", 4);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "AlbedoTex", 5);
		TEMP_PSO_RT_SHADOW->BindSRV("anyhit", "InstanceProperty", 6);
		TEMP_PSO_RT_SHADOW->Configure(1, sizeof(float) * 2, sizeof(float) * 2);

		bool bSuccess = TEMP_PSO_RT_SHADOW->InitRS("Shaders\\RaytracedShadow.hlsl");
		if (bSuccess)
		{
			PSO_RT_SHADOW = TEMP_PSO_RT_SHADOW;
		}
	}

	// create reflection rtpso
	if (bInitReflectionRT)
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_REFLECTION = renderBackend->CreateRTPipelineStateObject();
		if (!TEMP_PSO_RT_REFLECTION)
			return;
		TEMP_PSO_RT_REFLECTION->SetNumInstances(static_cast<uint32_t>(vecBLAS.size()));// scene->meshes.size();

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
		TEMP_PSO_RT_REFLECTION->BindSampler("global", "sampleWrap", 0);

		TEMP_PSO_RT_REFLECTION->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_REFLECTION->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO_RT_REFLECTION->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "AlbedoTex", 5);
		TEMP_PSO_RT_REFLECTION->BindSRV("chs", "InstanceProperty", 9);
		TEMP_PSO_RT_REFLECTION->Configure(1, sizeof(float) * 13, sizeof(float) * 2);

		bool bSuccess = TEMP_PSO_RT_REFLECTION->InitRS("Shaders\\RaytracedReflection.hlsl");

		if (bSuccess)
		{
			PSO_RT_REFLECTION = TEMP_PSO_RT_REFLECTION;
		}
	}
	// gi rtpso
	if (bInitGIRT)
	{
		shared_ptr<RTPipelineStateObject> TEMP_PSO_RT_GI = renderBackend->CreateRTPipelineStateObject();
		if (!TEMP_PSO_RT_GI)
			return;
		TEMP_PSO_RT_GI->SetNumInstances(static_cast<uint32_t>(vecBLAS.size()));// scene->meshes.size();

		TEMP_PSO_RT_GI->AddHitGroup("HitGroup", "chs", "");


		TEMP_PSO_RT_GI->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
		
		TEMP_PSO_RT_GI->BindUAV("global", "GIResultSH", 0);
		TEMP_PSO_RT_GI->BindUAV("global", "GIResultColor", 1);
		TEMP_PSO_RT_GI->BindSRV("global", "gRtScene", 0);
		TEMP_PSO_RT_GI->BindSRV("global", "DepthTex", 1);
		TEMP_PSO_RT_GI->BindSRV("global", "WorldNormalTex", 2);
		TEMP_PSO_RT_GI->BindCBV("global", "ViewParameter", 0, sizeof(RTGIViewParam), 1);
		TEMP_PSO_RT_GI->BindSampler("global", "sampleWrap", 0);
		TEMP_PSO_RT_GI->BindSRV("global", "BlueNoiseTex", 7);

		TEMP_PSO_RT_GI->AddShader("miss", RTPipelineStateObject::MISS);
		TEMP_PSO_RT_GI->AddShader("missShadow", RTPipelineStateObject::MISS);


		TEMP_PSO_RT_GI->AddShader("chs", RTPipelineStateObject::HIT);
		TEMP_PSO_RT_GI->BindSRV("chs", "vertices", 3);
		TEMP_PSO_RT_GI->BindSRV("chs", "indices", 4);
		TEMP_PSO_RT_GI->BindSRV("chs", "AlbedoTex", 5);
		TEMP_PSO_RT_GI->BindSRV("chs", "InstanceProperty", 6);
		TEMP_PSO_RT_GI->Configure(1, sizeof(float) * 12, sizeof(float) * 2);

		bool bSuccess = TEMP_PSO_RT_GI->InitRS("Shaders\\RaytracedGI.hlsl");

		if (bSuccess)
		{
			PSO_RT_GI = TEMP_PSO_RT_GI;
		}
	}
}

void Corona::InitPathTracingPass()
{
	shared_ptr<RTPipelineStateObject> TEMP_PSO_PATH_TRACING = renderBackend->CreateRTPipelineStateObject();
	if (!TEMP_PSO_PATH_TRACING)
		return;
	TEMP_PSO_PATH_TRACING->SetNumInstances(static_cast<uint32_t>(vecBLAS.size()));

	TEMP_PSO_PATH_TRACING->AddHitGroup("HitGroup", "PathTracingClosestHit", "PathTracingAnyHit");

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingRayGen", RTPipelineStateObject::RAYGEN);
	
	TEMP_PSO_PATH_TRACING->BindUAV("global", "OutputColor", 0);
	TEMP_PSO_PATH_TRACING->BindSRV("global", "gRtScene", 0);
	TEMP_PSO_PATH_TRACING->BindCBV("global", "ViewParameter", 0, sizeof(PathTracingViewParam), 1);
	TEMP_PSO_PATH_TRACING->BindSampler("global", "sampleWrap", 0);
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

	TEMP_PSO_PATH_TRACING->AddShader("PathTracingAnyHit", RTPipelineStateObject::ANYHIT);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "vertices", 1);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "indices", 2);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "InstanceProperty", 3);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "AlbedoTex", 5);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "NormalTex", 6);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "RoughnessTex", 7);
	TEMP_PSO_PATH_TRACING->BindSRV("PathTracingAnyHit", "MetallicTex", 8);
	TEMP_PSO_PATH_TRACING->Configure(8, 192, sizeof(float) * 2);

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
	if (!TLAS || !PSO_RT_SHADOW)
		return;
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("RaytraceShadowPass");
#endif
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand()%255, rand() % 255, rand() % 255), "RaytraceShadowPass");
	}

	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	PSO_RT_SHADOW->SetNumInstances(static_cast<uint32_t>(vecBLAS.size()));

	PSO_RT_SHADOW->BeginShaderTable();

	const bool bUseVulkanRtSceneGeometry =
		renderBackend &&
		renderBackend->GetAPI() == ERenderBackendAPI::Vulkan &&
		Sponza &&
		Sponza->RtSceneVertexBuffer &&
		Sponza->RtSceneIndexBuffer;

	int i = 0;
	for (auto&as : vecBLAS)
	{
		Mesh* mesh = as->MeshPtr;
		VertexBuffer* rtVertexBuffer = bUseVulkanRtSceneGeometry ? Sponza->RtSceneVertexBuffer.get() : mesh->Vb.get();
		IndexBuffer* rtIndexBuffer = bUseVulkanRtSceneGeometry ? Sponza->RtSceneIndexBuffer.get() : mesh->Ib.get();
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();

		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		PSO_RT_SHADOW->ResetHitProgram(i);
		PSO_RT_SHADOW->StartHitProgram("HitGroup", i);

		PSO_RT_SHADOW->AddVertexBufferSRVToHitProgram("HitGroup", rtVertexBuffer, i);
		PSO_RT_SHADOW->AddIndexBufferSRVToHitProgram("HitGroup", rtIndexBuffer, i);
		PSO_RT_SHADOW->AddTextureSRVToHitProgram("HitGroup", diffuseTex, i);
		PSO_RT_SHADOW->AddBufferSRVToHitProgram("HitGroup", InstancePropertyBuffer.get(), i);

		i++;
	}

	PSO_RT_SHADOW->SetTextureUAV("global", "ShadowResult", ShadowBuffer.get());
	PSO_RT_SHADOW->SetAccelerationStructure("global", "gRtScene", TLAS);
	PSO_RT_SHADOW->SetTextureSRV("global", "DepthTex", DepthBuffer.get());
	PSO_RT_SHADOW->SetTextureSRV("global", "WorldNormalTex", GeomNormalBuffer.get());
	PSO_RT_SHADOW->SetCBVValue("global", "ViewParameter", &RTShadowViewParam);
		PSO_RT_SHADOW->SetSampler("global", "sampleWrap", samplerWrap.get());


	PSO_RT_SHADOW->EndShaderTable();

	PSO_RT_SHADOW->Apply(GetRenderWidth(), GetRenderHeight());

	renderBackend->TransitionTexture(ShadowBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}

void Corona::ShadowDenoisePass()
{
	if (!ShadowDenoisePSO || !ShadowBuffer || !ShadowDenoisedBuffer)
		return;

	renderBackend->TransitionTexture(ShadowDenoisedBuffer.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	ShadowDenoisePSO->SetTextureSRV("ShadowTex", ShadowBuffer.get());
	ShadowDenoisePSO->SetTextureSRV("DepthTex", DepthBuffer.get());
	ShadowDenoisePSO->SetTextureSRV("GeoNormalTex", GeomNormalBuffer.get());
	ShadowDenoiseParam.ProjectionParams = RTShadowViewParam.ProjectionParams;
	ShadowDenoiseParam.RTSize = glm::vec2(GetRenderWidth(), GetRenderHeight());
	ShadowDenoisePSO->SetCBVValue("ShadowDenoiseCB", &ShadowDenoiseParam);
	ShadowDenoisePSO->SetTextureUAV("OutShadow", ShadowDenoisedBuffer.get());
	ShadowDenoisePSO->Apply();
	renderBackend->Dispatch((GetRenderWidth() + 7) / 8, (GetRenderHeight() + 7) / 8, 1);

	renderBackend->TransitionTexture(ShadowDenoisedBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}

void Corona::RaytraceReflectionPass()
{
	if (!TLAS || !PSO_RT_REFLECTION)
		return;
	AppendVulkanRuntimeTrace(L"[RaytraceReflectionPass] begin");
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("RaytraceReflectionPass");
#endif
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceReflectionPass");
	}

	renderBackend->TransitionTexture(SpecularGIRaw.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	AppendVulkanRuntimeTrace(L"[RaytraceReflectionPass] after transition to UAV");
	const FLOAT clearReflection[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearTextureUAVFloat(SpecularGIRaw.get(), clearReflection);
	AppendVulkanRuntimeTrace(L"[RaytraceReflectionPass] after clear");

	PSO_RT_REFLECTION->SetNumInstances(static_cast<uint32_t>(vecBLAS.size()));
	PSO_RT_REFLECTION->BeginShaderTable();
	AppendVulkanRuntimeTrace(L"[RaytraceReflectionPass] after BeginShaderTable");

	PSO_RT_REFLECTION->SetTextureUAV("global", "ReflectionResult", SpecularGIRaw.get());
	PSO_RT_REFLECTION->SetAccelerationStructure("global", "gRtScene", TLAS);
	PSO_RT_REFLECTION->SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	PSO_RT_REFLECTION->SetTextureSRV("global", "GeoNormalTex", GeomNormalBuffer.get());
	PSO_RT_REFLECTION->SetTextureSRV("global", "RougnessMetallicTex", RoughnessMetalicBuffer.get());
	PSO_RT_REFLECTION->SetTextureSRV("global", "BlueNoiseTex", BlueNoiseTex.get());
	PSO_RT_REFLECTION->SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());

	RTReflectionViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5) / (0.5f * GetRenderHeight());
	PSO_RT_REFLECTION->SetCBVValue("global", "ViewParameter", &RTReflectionViewParam);
	PSO_RT_REFLECTION->SetSampler("global", "sampleWrap", samplerWrap.get());
	AppendVulkanRuntimeTrace(L"[RaytraceReflectionPass] after global bindings");


	const bool bUseVulkanRtSceneGeometry =
		renderBackend &&
		renderBackend->GetAPI() == ERenderBackendAPI::Vulkan &&
		Sponza &&
		Sponza->RtSceneVertexBuffer &&
		Sponza->RtSceneIndexBuffer;

	int i = 0;
	for(auto&as : vecBLAS)
	{
		Mesh* mesh = as->MeshPtr;
		VertexBuffer* rtVertexBuffer = bUseVulkanRtSceneGeometry ? Sponza->RtSceneVertexBuffer.get() : mesh->Vb.get();
		IndexBuffer* rtIndexBuffer = bUseVulkanRtSceneGeometry ? Sponza->RtSceneIndexBuffer.get() : mesh->Ib.get();
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();

		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();
		PSO_RT_REFLECTION->ResetHitProgram(i);

		PSO_RT_REFLECTION->StartHitProgram("HitGroup", i);
		PSO_RT_REFLECTION->AddVertexBufferSRVToHitProgram("HitGroup", rtVertexBuffer, i);
		PSO_RT_REFLECTION->AddIndexBufferSRVToHitProgram("HitGroup", rtIndexBuffer, i);
		PSO_RT_REFLECTION->AddTextureSRVToHitProgram("HitGroup", diffuseTex, i);
		PSO_RT_REFLECTION->AddBufferSRVToHitProgram("HitGroup", InstancePropertyBuffer.get(), i);

		//PSO_RT_REFLECTION->StartHitProgram("ShadowHitGroup", i);
		/*
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", mesh->Vb->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", mesh->Ib->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", diffuseTex->GpuHandleSRV, i);
		PSO_RT_REFLECTION->AddDescriptor2HitProgram("ShadowHitGroup", InstancePropertyBuffer->GpuHandleSRV, i);*/
		i++;
	}
	AppendVulkanRuntimeTrace(L"[RaytraceReflectionPass] after hit bindings count=" + std::to_wstring(i));

	PSO_RT_REFLECTION->EndShaderTable();
	AppendVulkanRuntimeTrace(L"[RaytraceReflectionPass] after EndShaderTable");

	AppendVulkanRuntimeTrace(L"[RaytraceReflectionPass] before Apply");
	PSO_RT_REFLECTION->Apply(GetRenderWidth(), GetRenderHeight());
	AppendVulkanRuntimeTrace(L"[RaytraceReflectionPass] after Apply");

	renderBackend->TransitionTexture(SpecularGIRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	AppendVulkanRuntimeTrace(L"[RaytraceReflectionPass] after transition to SRV");
	//PIXEndEvent();
}

void Corona::RaytraceGIPass()
{
	if (!TLAS || !PSO_RT_GI)
		return;
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("RaytraceGIPass");
#endif
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "RaytraceGIPass");
	}

	renderBackend->TransitionTexture(DiffuseGIRawAux.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	renderBackend->TransitionTexture(DiffuseGIRaw.get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);
	const FLOAT clearGI[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderBackend->ClearTextureUAVFloat(DiffuseGIRawAux.get(), clearGI);
	renderBackend->ClearTextureUAVFloat(DiffuseGIRaw.get(), clearGI);

	PSO_RT_GI->SetNumInstances(static_cast<uint32_t>(vecBLAS.size()));
	PSO_RT_GI->BeginShaderTable();

	PSO_RT_GI->SetTextureUAV("global", "GIResultSH", DiffuseGIRawAux.get());
	PSO_RT_GI->SetTextureUAV("global", "GIResultColor", DiffuseGIRaw.get());
	PSO_RT_GI->SetAccelerationStructure("global", "gRtScene", TLAS);
	PSO_RT_GI->SetTextureSRV("global", "DepthTex", UnjitteredDepthBuffers[ColorBufferWriteIndex].get());
	PSO_RT_GI->SetTextureSRV("global", "WorldNormalTex", NormalBuffers[ColorBufferWriteIndex].get());
	PSO_RT_GI->SetTextureSRV("global", "BlueNoiseTex", BlueNoiseTex.get());
	
	RTGIViewParam.ViewSpreadAngle = glm::tan(Fov * 0.5) / (0.5f * GetRenderHeight());
	PSO_RT_GI->SetCBVValue("global", "ViewParameter", &RTGIViewParam);
	PSO_RT_GI->SetSampler("global", "sampleWrap", samplerWrap.get());

	const bool bUseVulkanRtSceneGeometry =
		renderBackend &&
		renderBackend->GetAPI() == ERenderBackendAPI::Vulkan &&
		Sponza &&
		Sponza->RtSceneVertexBuffer &&
		Sponza->RtSceneIndexBuffer;

	int i = 0;
	for(auto&as : vecBLAS)
	{
		Mesh* mesh = as->MeshPtr;
		VertexBuffer* rtVertexBuffer = bUseVulkanRtSceneGeometry ? Sponza->RtSceneVertexBuffer.get() : mesh->Vb.get();
		IndexBuffer* rtIndexBuffer = bUseVulkanRtSceneGeometry ? Sponza->RtSceneIndexBuffer.get() : mesh->Ib.get();
		
		Texture* diffuseTex = mesh->Draws[0].mat->Diffuse.get();
		if (!diffuseTex)
			diffuseTex = DefaultWhiteTex.get();

		PSO_RT_GI->ResetHitProgram(i);

		PSO_RT_GI->StartHitProgram("HitGroup", i);
		PSO_RT_GI->AddVertexBufferSRVToHitProgram("HitGroup", rtVertexBuffer, i);
		PSO_RT_GI->AddIndexBufferSRVToHitProgram("HitGroup", rtIndexBuffer, i);
		PSO_RT_GI->AddTextureSRVToHitProgram("HitGroup", diffuseTex, i);
		PSO_RT_GI->AddBufferSRVToHitProgram("HitGroup", InstancePropertyBuffer.get(), i);

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
	if (!TLAS || !PathTracingAccumBuffer[PathTracingWriteIndex])
		return;
#if USE_AFTERMATH
	renderBackend->EmitGpuCrashMarker("PathTracingPass");
#endif
	if (renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
	{
		PIXScopedEvent(renderBackend->GetGraphicsCommandList(), PIX_COLOR(rand() % 255, rand() % 255, rand() % 255), "PathTracingPass");
	}

	if (!PSO_PATH_TRACING)
	{
		InitPathTracingPass();
		if (!PSO_PATH_TRACING)
			return;
	}

	// Transition output buffer to UAV
	renderBackend->TransitionTexture(PathTracingAccumBuffer[PathTracingWriteIndex].get(), EResourceState::ShaderRead, EResourceState::UnorderedAccess);

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

	PSO_PATH_TRACING->SetNumInstances(static_cast<uint32_t>(vecBLAS.size()));
	PSO_PATH_TRACING->BeginShaderTable();

	PSO_PATH_TRACING->SetTextureUAV("global", "OutputColor", PathTracingAccumBuffer[PathTracingWriteIndex].get());
	PSO_PATH_TRACING->SetAccelerationStructure("global", "gRtScene", TLAS);
	PSO_PATH_TRACING->SetTextureSRV("global", "BlueNoiseTex", BlueNoiseTex.get());
	
	// PathTracingViewParam is already updated in OnUpdate()
	PSO_PATH_TRACING->SetCBVValue("global", "ViewParameter", &PathTracingViewParam);
	PSO_PATH_TRACING->SetSampler("global", "sampleWrap", samplerWrap.get());

	const bool bUseVulkanRtSceneGeometry =
		renderBackend &&
		renderBackend->GetAPI() == ERenderBackendAPI::Vulkan &&
		Sponza &&
		Sponza->RtSceneVertexBuffer &&
		Sponza->RtSceneIndexBuffer;

	int i = 0;
	for(auto& as : vecBLAS)
	{
		Mesh* mesh = as->MeshPtr;
		VertexBuffer* rtVertexBuffer = bUseVulkanRtSceneGeometry ? Sponza->RtSceneVertexBuffer.get() : mesh->Vb.get();
		IndexBuffer* rtIndexBuffer = bUseVulkanRtSceneGeometry ? Sponza->RtSceneIndexBuffer.get() : mesh->Ib.get();
		
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
		PSO_PATH_TRACING->AddVertexBufferSRVToHitProgram("HitGroup", rtVertexBuffer, i);
		PSO_PATH_TRACING->AddIndexBufferSRVToHitProgram("HitGroup", rtIndexBuffer, i);
		PSO_PATH_TRACING->AddBufferSRVToHitProgram("HitGroup", InstancePropertyBuffer.get(), i);
		PSO_PATH_TRACING->AddTextureSRVToHitProgram("HitGroup", diffuseTex, i);
		PSO_PATH_TRACING->AddTextureSRVToHitProgram("HitGroup", normalTex, i);
		PSO_PATH_TRACING->AddTextureSRVToHitProgram("HitGroup", roughnessTex, i);
		PSO_PATH_TRACING->AddTextureSRVToHitProgram("HitGroup", metallicTex, i);

		i++;
	}

	PSO_PATH_TRACING->EndShaderTable();

	PSO_PATH_TRACING->Apply(m_width, m_height);

	// Transition output buffer back to SRV
	renderBackend->TransitionTexture(PathTracingAccumBuffer[PathTracingWriteIndex].get(), EResourceState::UnorderedAccess, EResourceState::ShaderRead);
}
