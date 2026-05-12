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
#include "D3D12Helpers.h"
#include "Win32Application.h"
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
#include <cstring>
#include <dxgidebug.h>
#include "assimp/include/Importer.hpp"
#include "assimp/include/scene.h"
#include "assimp/include/postprocess.h"
//#pragma comment(lib, "assimp\\lib\\assimp.lib")
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

void AppendCpuRuntimeTrace(const std::wstring& line);


using namespace glm;
using namespace DirectX;

namespace
{
	constexpr double kCameraPathDumpFps = 30.0;
	constexpr double kCameraPathRecordMinIntervalSeconds = 1.0 / 120.0;

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

	std::wstring FormatHex32(uint32_t value)
	{
		std::wstringstream stream;
		stream << std::hex << std::uppercase << value;
		return stream.str();
	}

	std::wstring NormalizePathForCompare(const std::wstring& value)
	{
		if (value.empty())
			return {};

		std::error_code ec;
		const std::filesystem::path weakPath = std::filesystem::weakly_canonical(std::filesystem::path(value), ec);
		if (!ec)
			return weakPath.wstring();

		return std::filesystem::path(value).lexically_normal().wstring();
	}

	std::wstring NormalizePathKey(const std::wstring& value)
	{
		std::wstring normalized = NormalizePathForCompare(value);
		for (wchar_t& ch : normalized)
			ch = towlower(ch);
		return normalized;
	}

	bool IsPathInsideDirectory(const std::wstring& path, const std::wstring& directory)
	{
		std::wstring pathKey = NormalizePathKey(path);
		std::wstring directoryKey = NormalizePathKey(directory);
		if (pathKey.empty() || directoryKey.empty())
			return false;

		if (directoryKey.back() != L'\\' && directoryKey.back() != L'/')
			directoryKey.push_back(std::filesystem::path::preferred_separator);
		return pathKey.rfind(directoryKey, 0) == 0;
	}

	bool WriteProceduralDungeonBrickBmp(const std::filesystem::path& filePath)
	{
		constexpr uint32_t kWidth = 128;
		constexpr uint32_t kHeight = 128;
		constexpr uint32_t kBytesPerPixel = 4;
		constexpr uint32_t kHeaderSize = 14 + 40;
		constexpr uint32_t kPixelDataSize = kWidth * kHeight * kBytesPerPixel;
		constexpr uint32_t kFileSize = kHeaderSize + kPixelDataSize;

		std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
		if (!file)
			return false;

		auto writeU16 = [&file](uint16_t value)
		{
			const char bytes[2] =
			{
				static_cast<char>(value & 0xffu),
				static_cast<char>((value >> 8) & 0xffu),
			};
			file.write(bytes, sizeof(bytes));
		};
		auto writeU32 = [&file](uint32_t value)
		{
			const char bytes[4] =
			{
				static_cast<char>(value & 0xffu),
				static_cast<char>((value >> 8) & 0xffu),
				static_cast<char>((value >> 16) & 0xffu),
				static_cast<char>((value >> 24) & 0xffu),
			};
			file.write(bytes, sizeof(bytes));
		};
		auto writeI32 = [&writeU32](int32_t value)
		{
			writeU32(static_cast<uint32_t>(value));
		};
		auto clampByte = [](int value) -> uint8_t
		{
			return static_cast<uint8_t>(std::clamp(value, 0, 255));
		};

		file.write("BM", 2);
		writeU32(kFileSize);
		writeU16(0);
		writeU16(0);
		writeU32(kHeaderSize);
		writeU32(40);
		writeI32(static_cast<int32_t>(kWidth));
		writeI32(static_cast<int32_t>(kHeight));
		writeU16(1);
		writeU16(32);
		writeU32(0);
		writeU32(kPixelDataSize);
		writeI32(2835);
		writeI32(2835);
		writeU32(0);
		writeU32(0);

		for (int y = static_cast<int>(kHeight) - 1; y >= 0; --y)
		{
			const int brickRow = y / 16;
			const int rowLocal = y % 16;
			const int stagger = (brickRow & 1) ? 16 : 0;
			for (uint32_t x = 0; x < kWidth; ++x)
			{
				const int shiftedX = (static_cast<int>(x) + stagger) % 32;
				const bool bMortar = rowLocal < 2 || shiftedX < 2;
				uint8_t r = 76;
				uint8_t g = 70;
				uint8_t b = 62;
				if (!bMortar)
				{
					const int brickColumn = (static_cast<int>(x) + stagger) / 32;
					const uint32_t hash =
						static_cast<uint32_t>(brickRow * 73856093) ^
						static_cast<uint32_t>(brickColumn * 19349663);
					const int variation = static_cast<int>((hash >> 4) & 31u) - 15;
					const int edgeShade = (rowLocal > 13 || shiftedX > 29) ? -10 : 0;
					const int surfaceNoise = static_cast<int>(((hash + x * 13u + static_cast<uint32_t>(y) * 7u) >> 3) & 7u) - 3;
					r = clampByte(132 + variation + edgeShade + surfaceNoise);
					g = clampByte(64 + variation / 3 + edgeShade + surfaceNoise);
					b = clampByte(42 + variation / 4 + edgeShade + surfaceNoise);
				}

				const char pixel[4] =
				{
					static_cast<char>(b),
					static_cast<char>(g),
					static_cast<char>(r),
					static_cast<char>(255),
				};
				file.write(pixel, sizeof(pixel));
			}
		}

		return file.good();
	}

	std::wstring TrimLeadingWhitespace(std::wstring value)
	{
		while (!value.empty() && iswspace(value.front()))
			value.erase(value.begin());
		return value;
	}

	void ForceOpaqueAlpha(const Image* image)
	{
		if (!image || !image->pixels)
			return;

		if (image->format != DXGI_FORMAT_B8G8R8A8_UNORM &&
			image->format != DXGI_FORMAT_R8G8B8A8_UNORM &&
			image->format != DXGI_FORMAT_B8G8R8X8_UNORM)
			return;

		for (size_t y = 0; y < image->height; ++y)
		{
			uint8_t* row = image->pixels + y * image->rowPitch;
			for (size_t x = 0; x < image->width; ++x)
				row[x * 4 + 3] = 0xff;
		}
	}

	bool SaveCapturedTextureHDR(const ScratchImage& captured, const std::wstring& filePath, std::wstring* errorMessage)
	{
		const Image* image = captured.GetImage(0, 0, 0);
		if (!image)
		{
			if (errorMessage)
				*errorMessage = L"missing captured image";
			return false;
		}

		ScratchImage converted;
		HRESULT hr = Convert(*image, DXGI_FORMAT_R32G32B32A32_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"hdr convert failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr));
			return false;
		}

		const Image* convertedImage = converted.GetImage(0, 0, 0);
		if (!convertedImage)
		{
			if (errorMessage)
				*errorMessage = L"missing converted hdr image";
			return false;
		}

		hr = SaveToHDRFile(*convertedImage, filePath.c_str());
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"hdr save failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr));
			return false;
		}

		return true;
	}

	const char* GetRawFloatFormatName(DXGI_FORMAT format)
	{
		switch (format)
		{
		case DXGI_FORMAT_R32_FLOAT:
			return "R32_FLOAT";
		case DXGI_FORMAT_R32G32_FLOAT:
			return "R32G32_FLOAT";
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			return "R32G32B32A32_FLOAT";
		default:
			return "UNKNOWN";
		}
	}

	bool SaveCapturedTextureRawFloat(
		const ScratchImage& captured,
		const std::wstring& filePath,
		DXGI_FORMAT targetFormat,
		uint32_t channelCount,
		std::wstring* errorMessage)
	{
		const Image* image = captured.GetImage(0, 0, 0);
		if (!image)
		{
			if (errorMessage)
				*errorMessage = L"missing captured image";
			return false;
		}

		if (channelCount == 0 || channelCount > 4)
		{
			if (errorMessage)
				*errorMessage = L"invalid raw channel count";
			return false;
		}

		const bool bUseSourceImage =
			(targetFormat == DXGI_FORMAT_R32_FLOAT &&
				(image->format == DXGI_FORMAT_R32_FLOAT || image->format == DXGI_FORMAT_D32_FLOAT)) ||
			(targetFormat == DXGI_FORMAT_R32G32_FLOAT && image->format == DXGI_FORMAT_R32G32_FLOAT) ||
			(targetFormat == DXGI_FORMAT_R32G32B32A32_FLOAT && image->format == DXGI_FORMAT_R32G32B32A32_FLOAT);

		ScratchImage converted;
		if (!bUseSourceImage)
		{
			HRESULT hr = Convert(*image, targetFormat, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
			if (FAILED(hr))
			{
				if (errorMessage)
					*errorMessage = L"raw float convert failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr));
				return false;
			}
		}

		const Image* convertedImage = bUseSourceImage ? image : converted.GetImage(0, 0, 0);
		if (!convertedImage)
		{
			if (errorMessage)
				*errorMessage = L"missing converted raw image";
			return false;
		}

		const size_t bytesPerPixel = sizeof(float) * static_cast<size_t>(channelCount);
		const size_t rowBytes = convertedImage->width * bytesPerPixel;
		if (convertedImage->rowPitch < rowBytes)
		{
			if (errorMessage)
				*errorMessage = L"raw row pitch is smaller than expected";
			return false;
		}

		std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
		if (!file.is_open())
		{
			if (errorMessage)
				*errorMessage = L"failed to open raw output";
			return false;
		}

		file << "CORONA_RAW_FLOAT 1\n";
		file << "width " << convertedImage->width << "\n";
		file << "height " << convertedImage->height << "\n";
		file << "channels " << channelCount << "\n";
		file << "format " << GetRawFloatFormatName(targetFormat) << "\n";
		file << "endianness little\n";
		file << "data\n";

		for (size_t y = 0; y < convertedImage->height; ++y)
		{
			const uint8_t* row = convertedImage->pixels + y * convertedImage->rowPitch;
			file.write(reinterpret_cast<const char*>(row), static_cast<std::streamsize>(rowBytes));
			if (!file.good())
			{
				if (errorMessage)
					*errorMessage = L"raw write failed";
				return false;
			}
		}

		return true;
	}

	bool SaveCapturedTexturePNG(const ScratchImage& captured, const std::wstring& filePath, std::wstring* errorMessage)
	{
		const Image* image = captured.GetImage(0, 0, 0);
		if (!image)
		{
			if (errorMessage)
				*errorMessage = L"missing captured image";
			return false;
		}

		const bool bCanSaveWithoutConvert =
			image->format == DXGI_FORMAT_R8G8B8A8_UNORM ||
			image->format == DXGI_FORMAT_B8G8R8A8_UNORM;
		if (bCanSaveWithoutConvert)
		{
			ScratchImage pngImage;
			HRESULT hr = pngImage.InitializeFromImage(*image);
			const Image* pngOutputImage = SUCCEEDED(hr) ? pngImage.GetImage(0, 0, 0) : nullptr;
			if (!pngOutputImage)
				hr = E_FAIL;
			if (SUCCEEDED(hr))
			{
				ForceOpaqueAlpha(pngOutputImage);
				hr = SaveToWICFile(*pngOutputImage, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, filePath.c_str());
			}
			if (FAILED(hr))
			{
				if (errorMessage)
					*errorMessage = L"png direct save failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr));
				return false;
			}
			return true;
		}

		ScratchImage converted;
		HRESULT hr = Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
		if (FAILED(hr))
		{
			if (errorMessage)
			{
				*errorMessage =
					L"png convert failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)) +
					L", srcFormat=" + std::to_wstring(static_cast<unsigned int>(image->format)) +
					L", size=" + std::to_wstring(image->width) + L"x" + std::to_wstring(image->height);
			}
			return false;
		}

		const Image* convertedImage = converted.GetImage(0, 0, 0);
		if (!convertedImage)
		{
			if (errorMessage)
				*errorMessage = L"missing converted png image";
			return false;
		}

		ForceOpaqueAlpha(convertedImage);
		hr = SaveToWICFile(*convertedImage, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, filePath.c_str());
		if (FAILED(hr))
		{
			if (errorMessage)
				*errorMessage = L"png save failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr));
			return false;
		}
		return true;
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
		const glm::mat4x4& prevViewMat,
		const glm::mat4x4& invViewMat,
		const glm::vec2& currentJitter,
		float nearPlane,
		float farPlane,
		float fov,
		float aspectRatio,
		bool bCameraMotionIncluded,
		bool bResetNeeded)
	{
		sl::Constants consts{};
		const glm::mat4x4 currentProj = unjitteredProjMat;
		const glm::mat4x4 currentInvProj = glm::inverse(currentProj);
		const glm::mat4x4 currentViewToPrevView = prevViewMat * invViewMat;
		const glm::mat4x4 clipToPrevClip = currentProj * currentViewToPrevView * currentInvProj;
		const glm::mat4x4 prevClipToClip = glm::inverse(clipToPrevClip);

		consts.cameraViewToClip = ToSLMatrix(currentProj);
		consts.clipToCameraView = ToSLMatrix(currentInvProj);
		consts.clipToLensClip = ToSLMatrix(glm::mat4x4(1.0f));
		consts.clipToPrevClip = ToSLMatrix(clipToPrevClip);
		consts.prevClipToClip = ToSLMatrix(prevClipToClip);
		// Our projection jitter matrix applies half-pixel offsets in clip space,
		// so convert the stored sequence sample to the actual pixel jitter used.
		const glm::vec2 appliedPixelJitter = currentJitter * 0.5f;
		consts.jitterOffset = sl::float2(appliedPixelJitter.x, appliedPixelJitter.y);
		// Keep the sign convention that matches the current Streamline/NGX path.
		// Corona stores uvCurrent - uvPrevious; with this integration the RR
		// history lookup is correct only when the scale is negated here.
		consts.mvecScale = sl::float2(-1.0f, -1.0f);
		consts.cameraPinholeOffset = sl::float2(0.0f, 0.0f);

		glm::vec3 cameraPos = glm::vec3(invViewMat[3]);
		glm::vec3 cameraRight = glm::normalize(glm::vec3(invViewMat[0]));
		glm::vec3 cameraUp = glm::normalize(glm::vec3(invViewMat[1]));
		glm::vec3 cameraFwd = -glm::normalize(glm::vec3(invViewMat[2]));
		consts.cameraPos = sl::float3(cameraPos.x, cameraPos.y, cameraPos.z);
		consts.cameraRight = sl::float3(cameraRight.x, cameraRight.y, cameraRight.z);
		consts.cameraUp = sl::float3(cameraUp.x, cameraUp.y, cameraUp.z);
		consts.cameraFwd = sl::float3(cameraFwd.x, cameraFwd.y, cameraFwd.z);
		consts.cameraNear = nearPlane;
		consts.cameraFar = farPlane;
		consts.cameraFOV = fov;
		consts.cameraAspectRatio = aspectRatio;
		consts.depthInverted = sl::Boolean::eFalse;
		consts.motionVectorsInvalidValue = 0.0f;
		consts.cameraMotionIncluded = bCameraMotionIncluded ? sl::Boolean::eTrue : sl::Boolean::eFalse;
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

	constexpr std::array<const char*, 17> kGpuPassNames = {
		"Frame Total",
		"GBuffer",
		"RT Shadow",
		"RT AO",
		"RT Sky",
		"RT Reflection",
		"RT Diffuse GI",
		"Screen Probe GI",
		"Temporal Denoise",
		"Lighting",
		"DLSS RR",
		"DLSS SR",
		"Temporal AA",
		"Path Tracing",
		"Tone Map",
		"Debug",
		"ImGui",
	};

	const std::array<UINT64, 17> kGpuPassPixColors = {
		PIX_COLOR(210, 210, 210),
		PIX_COLOR(86, 156, 214),
		PIX_COLOR(214, 86, 86),
		PIX_COLOR(214, 145, 86),
		PIX_COLOR(156, 214, 86),
		PIX_COLOR(214, 86, 189),
		PIX_COLOR(86, 214, 169),
		PIX_COLOR(86, 214, 214),
		PIX_COLOR(181, 140, 255),
		PIX_COLOR(245, 214, 86),
		PIX_COLOR(86, 145, 245),
		PIX_COLOR(86, 189, 245),
		PIX_COLOR(245, 145, 86),
		PIX_COLOR(189, 86, 245),
		PIX_COLOR(245, 245, 245),
		PIX_COLOR(145, 145, 145),
		PIX_COLOR(86, 245, 145),
	};

	void BeginGpuPassMarker(IRenderBackend* backend, UINT passIndex, const char* markerName)
	{
		if (!backend ||
			backend->GetAPI() != ERenderBackendAPI::D3D12 ||
			passIndex >= kGpuPassPixColors.size() ||
			!markerName)
		{
			return;
		}

		ID3D12GraphicsCommandList* commandList = backend->GetGraphicsCommandList();
		if (!commandList)
			return;

		PIXBeginEvent(commandList, kGpuPassPixColors[passIndex], "%s", markerName);
	}

	void EndGpuPassMarker(IRenderBackend* backend)
	{
		if (!backend || backend->GetAPI() != ERenderBackendAPI::D3D12)
			return;

		ID3D12GraphicsCommandList* commandList = backend->GetGraphicsCommandList();
		if (!commandList)
			return;

		PIXEndEvent(commandList);
	}

	constexpr std::array<const char*, 5> kCpuUpdatePhaseNames = {
		"Camera / Physics",
		"Input",
		"Luau Scripts",
		"Camera Path",
		"Render Sync",
	};

	constexpr std::array<const char*, 5> kCpuUpdatePhaseLogColumnNames = {
		"camera_physics",
		"input",
		"luau_scripts",
		"camera_path",
		"render_sync",
	};

	const std::filesystem::path kFramePerfLogPath =
		RuntimePaths::LogFile(L"fps_perf.log");

	double ElapsedMilliseconds(
		const std::chrono::steady_clock::time_point& begin,
		const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - begin).count();
	}

	std::wstring FormatMilliseconds(double milliseconds)
	{
		std::wostringstream stream;
		stream << std::fixed << std::setprecision(3) << milliseconds;
		return stream.str();
	}

	constexpr char kCoronaMeshMagic[8] = { 'C', 'R', 'N', 'M', 'E', 'S', 'H', '\0' };
	constexpr uint32_t kCoronaMeshVersion = 1;
	constexpr uint32_t kCoronaMeshMaterialHasAlpha = 1u << 0;
	constexpr uint32_t kCoronaMeshMaxStringBytes = 64u * 1024u;
	constexpr uint32_t kCoronaMeshMaxMaterials = 4096u;
	constexpr uint32_t kCoronaMeshMaxMeshes = 65536u;

	struct CoronaMeshFileHeader
	{
		char Magic[8];
		uint32_t Version = 0;
		uint32_t HeaderSize = 0;
		uint32_t MaterialCount = 0;
		uint32_t MeshCount = 0;
		uint32_t Flags = 0;
		float BoundsMin[3] = {};
		float BoundsMax[3] = {};
	};

	struct CoronaMeshDiskVertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
		glm::vec3 Tangent;
	};
	static_assert(sizeof(CoronaMeshDiskVertex) == 44, "Corona mesh binary vertex layout must match shader vertex stride.");

	struct CoronaMeshDiskMaterial
	{
		uint32_t Flags = 0;
		std::string Diffuse;
		std::string Normal;
		std::string Roughness;
		std::string Metallic;
	};

	template<typename T>
	bool ReadBinaryValue(std::ifstream& file, T& value)
	{
		file.read(reinterpret_cast<char*>(&value), sizeof(T));
		return static_cast<bool>(file);
	}

	bool ReadBinaryBytes(std::ifstream& file, void* data, size_t byteCount)
	{
		if (byteCount == 0)
			return true;
		file.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(byteCount));
		return static_cast<bool>(file);
	}

	bool ReadBinaryString(std::ifstream& file, std::string& value)
	{
		uint32_t length = 0;
		if (!ReadBinaryValue(file, length) || length > kCoronaMeshMaxStringBytes)
			return false;

		value.clear();
		value.resize(length);
		return ReadBinaryBytes(file, value.data(), value.size());
	}

	std::filesystem::path GetCoronaMeshCachePath(const std::wstring& sourceFileName)
	{
		std::filesystem::path cachePath(sourceFileName);
		cachePath.replace_extension(L".cmesh");
		return cachePath;
	}

	bool IsCoronaMeshCacheUsable(const std::filesystem::path& cachePath, const std::filesystem::path& sourcePath)
	{
		std::error_code errorCode;
		if (!std::filesystem::exists(cachePath, errorCode))
			return false;

		if (!std::filesystem::exists(sourcePath, errorCode))
			return true;

		const auto cacheTime = std::filesystem::last_write_time(cachePath, errorCode);
		if (errorCode)
			return true;
		const auto sourceTime = std::filesystem::last_write_time(sourcePath, errorCode);
		if (errorCode)
			return true;

		return cacheTime >= sourceTime;
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

	const wchar_t* GetAntiAliasingModeName(Corona::EAntiAliasingMode mode)
	{
		switch (mode)
		{
		case Corona::EAntiAliasingMode::OFF:
			return L"off";
		case Corona::EAntiAliasingMode::TAA:
			return L"taa";
		case Corona::EAntiAliasingMode::DLSS_SR:
			return L"dlss-sr";
		case Corona::EAntiAliasingMode::DLSS_RR:
			return L"dlss-rr";
		default:
			return L"unknown";
		}
	}

	const char* GetRayNoiseModeName(Corona::ERayNoiseMode mode)
	{
		switch (mode)
		{
		case Corona::ERayNoiseMode::BLUE_NOISE:
			return "Blue Noise";
		case Corona::ERayNoiseMode::R2_LOW_DISCREPANCY:
			return "R2 Low Discrepancy";
		case Corona::ERayNoiseMode::STABLE_HASH:
			return "Stable Hash";
		default:
			return "Unknown";
		}
	}

	const wchar_t* GetRayNoiseModeNameW(Corona::ERayNoiseMode mode)
	{
		switch (mode)
		{
		case Corona::ERayNoiseMode::BLUE_NOISE:
			return L"blue";
		case Corona::ERayNoiseMode::R2_LOW_DISCREPANCY:
			return L"r2";
		case Corona::ERayNoiseMode::STABLE_HASH:
			return L"stable";
		default:
			return L"unknown";
		}
	}

	const char* GetDiffuseGIModeName(Corona::EDiffuseGIMode mode)
	{
		switch (mode)
		{
		case Corona::EDiffuseGIMode::SIMPLE_RAYTRACE:
			return "Simple Raytrace";
		case Corona::EDiffuseGIMode::SPATIAL_HASH:
			return "Spatial Hash";
		case Corona::EDiffuseGIMode::SCREEN_PROBE:
			return "Screen Probe";
		default:
			return "Unknown";
		}
	}

	const wchar_t* GetDiffuseGIModeNameW(Corona::EDiffuseGIMode mode)
	{
		switch (mode)
		{
		case Corona::EDiffuseGIMode::SIMPLE_RAYTRACE:
			return L"simple";
		case Corona::EDiffuseGIMode::SPATIAL_HASH:
			return L"spatial-hash";
		case Corona::EDiffuseGIMode::SCREEN_PROBE:
			return L"screen-probe";
		default:
			return L"unknown";
		}
	}
}

struct Corona::AsyncImageDumpJob
{
	ScratchImage Image;
	std::wstring FilePath;
	bool bHDR = false;
};

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
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
	m_width(width),
	m_height(height),
	m_aspectRatio(static_cast<float>(width) / static_cast<float>(height)),
	m_title(std::move(name))
{
	m_assetsPath = RuntimePaths::SourceDirectory().wstring();

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
	StopGameThread();
	ShutdownLuauScripting();
	ShutdownCpuPhysics();
}

std::wstring Corona::GetAssetFullPath(LPCWSTR assetName) const
{
	return (std::filesystem::path(m_assetsPath) / assetName).wstring();
}

void Corona::SetCustomWindowText(LPCWSTR text)
{
	std::wstring windowText = m_title + L": " + text;
	SetWindowTextW(Win32Application::GetHwnd(), windowText.c_str());
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
				"avg_update_ms,avg_begin_frame_ms,avg_record_ms,avg_execute_ms,avg_end_frame_ms";
			for (UINT phaseIndex = 0; phaseIndex < CpuUpdatePhaseCount; ++phaseIndex)
			{
				logFile << ",avg_update_" << kCpuUpdatePhaseLogColumnNames[phaseIndex] << "_ms";
			}
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
	FramePerfLogAccumCpuUpdateMs += CpuUpdateLastTimeMs;
	for (UINT phaseIndex = 0; phaseIndex < CpuUpdatePhaseCount; ++phaseIndex)
	{
		FramePerfLogAccumCpuUpdatePhaseMs[phaseIndex] += CpuUpdatePhaseLastTimeMs[phaseIndex];
	}
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
			<< "," << (FramePerfLogAccumCpuUpdateMs / sampleFrameCount)
			<< "," << (FramePerfLogAccumBeginFrameMs / sampleFrameCount)
			<< "," << (FramePerfLogAccumRecordMs / sampleFrameCount)
			<< "," << (FramePerfLogAccumExecuteMs / sampleFrameCount)
			<< "," << (FramePerfLogAccumEndFrameMs / sampleFrameCount);
		for (UINT phaseIndex = 0; phaseIndex < CpuUpdatePhaseCount; ++phaseIndex)
		{
			logFile << "," << (FramePerfLogAccumCpuUpdatePhaseMs[phaseIndex] / sampleFrameCount);
		}
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
	FramePerfLogAccumCpuUpdateMs = 0.0;
	FramePerfLogAccumCpuUpdatePhaseMs.fill(0.0);
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
	const char* markerName = (pass == EGpuPass::Frame) ? "Frame" : GetGpuPassName(pass);
	BeginGpuPassMarker(renderBackend.get(), passIndex, markerName);

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
	EndGpuPassMarker(renderBackend.get());

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

const char* Corona::GetCpuUpdatePhaseName(ECpuUpdatePhase phase) const
{
	return kCpuUpdatePhaseNames[static_cast<size_t>(phase)];
}

void Corona::AddCpuUpdatePhaseTiming(
	ECpuUpdatePhase phase,
	const CpuClock::time_point& begin,
	const CpuClock::time_point& end)
{
	const UINT phaseIndex = static_cast<UINT>(phase);
	CpuUpdatePhaseLastTimeMs[phaseIndex] += static_cast<float>(ElapsedMilliseconds(begin, end));
}

void Corona::FinishCpuUpdateTiming(
	const CpuClock::time_point& begin,
	const CpuClock::time_point& end)
{
	CpuUpdateLastTimeMs = static_cast<float>(ElapsedMilliseconds(begin, end));
	CpuUpdateHistoryMs.push_back(CpuUpdateLastTimeMs);
	for (UINT phaseIndex = 0; phaseIndex < CpuUpdatePhaseCount; ++phaseIndex)
	{
		CpuUpdatePhaseHistoryMs[phaseIndex].push_back(CpuUpdatePhaseLastTimeMs[phaseIndex]);
	}
	TrimCpuUpdateTimingHistory();
}

void Corona::TrimCpuUpdateTimingHistory()
{
	auto trimAndAverage = [this](std::deque<float>& history) -> float
	{
		while (history.size() > GpuTimingAverageFrameCount)
		{
			history.pop_front();
		}

		float sumMs = 0.0f;
		for (float sampleMs : history)
		{
			sumMs += sampleMs;
		}
		return history.empty() ? 0.0f : sumMs / static_cast<float>(history.size());
	};

	CpuUpdateAverageTimeMs = trimAndAverage(CpuUpdateHistoryMs);
	for (UINT phaseIndex = 0; phaseIndex < CpuUpdatePhaseCount; ++phaseIndex)
	{
		CpuUpdatePhaseAverageTimeMs[phaseIndex] = trimAndAverage(CpuUpdatePhaseHistoryMs[phaseIndex]);
	}
}

#if WITH_STREAMLINE
void Corona::InitStreamline()
{
	if (bStreamlineInitialized)
		return;

	sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
	static const std::wstring streamlineLogDirectoryString = RuntimePaths::LogDirectory().wstring();
	std::filesystem::create_directories(std::filesystem::path(streamlineLogDirectoryString));
	sl::Preferences pref{};
	pref.showConsole = true;
	pref.logLevel = sl::LogLevel::eDefault;
	pref.pathToLogsAndData = streamlineLogDirectoryString.c_str();
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
	const glm::vec2 streamlineJitter =
		RenderingMode == ERenderingMode::PATHTRACING ? glm::vec2(0.0f) : CurrentJitter;
	const bool bStreamlineCameraMotionIncluded =
		!(RenderingMode == ERenderingMode::PATHTRACING && IsPathTracingDLSSRREnabled());
	sl::Constants consts = BuildStreamlineConstants(
		UnjitteredProjMat,
		PrevViewMat,
		InvViewMat,
		streamlineJitter,
		Near,
		Far,
		Fov,
		static_cast<float>(m_width) / static_cast<float>(m_height),
		bStreamlineCameraMotionIncluded,
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

	Texture* inputColor = (bDLSSRROutputValidThisFrame && DLSSRRBuffer) ? DLSSRRBuffer.get() : LightingBuffer.get();
	Texture* outputTarget = ColorBuffers[ColorBufferWriteIndex].get();
	if (!inputColor || !outputTarget)
		return false;
	renderBackend->TransitionTexture(outputTarget, EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	sl::ViewportHandle vp(0);
	sl::Extent renderExtent{ 0, 0, GetRenderWidth(), GetRenderHeight() };
	sl::Extent outputExtent{ 0, 0, m_width, m_height };
	sl::Resource colorRes(sl::ResourceType::eTex2d, inputColor->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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

	Texture* inputColor = IsPathTracingDLSSRREnabled() ? PathTracingAccumBuffer[PathTracingWriteIndex].get() : LightingBuffer.get();
	Texture* outputTarget = DLSSRRBuffer.get();
	const bool bUseRRSpecularMotionVectors =
		(IsPathTracingDLSSRREnabled() && bEnablePathTracingRRSpecularMotionVectors) ||
		(RenderingMode == ERenderingMode::HYBRID && bEnableHybridRRSpecularMotionVectors);
	const bool bUseRRSpecularHitDistance =
		(IsPathTracingDLSSRREnabled() && bEnablePathTracingRRSpecularHitDistance) ||
		(RenderingMode == ERenderingMode::HYBRID && bEnableHybridRRSpecularHitDistance);
	if (!inputColor ||
		!outputTarget ||
		!UnjitteredDepthBuffers[ColorBufferWriteIndex] ||
		!VelocityBuffer ||
		!NormalBuffers[ColorBufferWriteIndex] ||
		!RoughnessMetalicBuffer ||
		!AlbedoBuffer ||
		!SpecularAlbedoBuffer ||
		(bUseRRSpecularHitDistance && !PathTracingSpecularHitDistanceBuffer) ||
		(bUseRRSpecularMotionVectors && !PathTracingSpecularMotionVectorBuffer))
		return false;
	renderBackend->TransitionTexture(outputTarget, EResourceState::ShaderRead, EResourceState::UnorderedAccess);

	sl::ViewportHandle vp(0);
	sl::Extent renderExtent{ 0, 0, GetRenderWidth(), GetRenderHeight() };
	sl::Extent outputExtent{ 0, 0, GetRenderWidth(), GetRenderHeight() };
	sl::Resource colorRes(sl::ResourceType::eTex2d, inputColor->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource depthRes(sl::ResourceType::eTex2d, UnjitteredDepthBuffers[ColorBufferWriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource motionRes(sl::ResourceType::eTex2d, VelocityBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource normalRes(sl::ResourceType::eTex2d, NormalBuffers[ColorBufferWriteIndex]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource roughnessRes(sl::ResourceType::eTex2d, RoughnessMetalicBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource albedoRes(sl::ResourceType::eTex2d, AlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource specularAlbedoRes(sl::ResourceType::eTex2d, SpecularAlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource specularHitDistanceRes(sl::ResourceType::eTex2d, PathTracingSpecularHitDistanceBuffer ? PathTracingSpecularHitDistanceBuffer->resource.Get() : nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource specularMotionVectorRes(sl::ResourceType::eTex2d, PathTracingSpecularMotionVectorBuffer ? PathTracingSpecularMotionVectorBuffer->resource.Get() : nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	sl::Resource outputRes(sl::ResourceType::eTex2d, outputTarget->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	sl::ResourceTag colorTag(&colorRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag depthTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag motionTag(&motionRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag normalTag(&normalRes, sl::kBufferTypeNormals, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag roughnessTag(&roughnessRes, sl::kBufferTypeRoughness, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag albedoTag(&albedoRes, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag specularAlbedoTag(&specularAlbedoRes, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag specularHitDistanceTag(&specularHitDistanceRes, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag specularMotionVectorTag(&specularMotionVectorRes, sl::kBufferTypeSpecularMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
	sl::ResourceTag outputTag(&outputRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent);
	std::vector<sl::ResourceTag> tags = {
		colorTag,
		depthTag,
		motionTag,
		normalTag,
		roughnessTag,
		albedoTag,
		specularAlbedoTag,
	};
	if (bUseRRSpecularMotionVectors)
	{
		tags.push_back(specularMotionVectorTag);
	}
	if (bUseRRSpecularHitDistance)
	{
		tags.push_back(specularHitDistanceTag);
	}
	tags.push_back(outputTag);
	slSetTagForFrame(*StreamlineFrameToken, vp, tags.data(), static_cast<uint32_t>(tags.size()), renderBackend->GetGraphicsCommandList());

	std::vector<const sl::BaseStructure*> inputs = {
		static_cast<const sl::BaseStructure*>(&vp),
		static_cast<const sl::BaseStructure*>(&depthTag),
		static_cast<const sl::BaseStructure*>(&normalTag),
		static_cast<const sl::BaseStructure*>(&roughnessTag),
		static_cast<const sl::BaseStructure*>(&albedoTag),
		static_cast<const sl::BaseStructure*>(&specularAlbedoTag),
		static_cast<const sl::BaseStructure*>(&motionTag),
	};
	if (bUseRRSpecularMotionVectors)
		inputs.push_back(static_cast<const sl::BaseStructure*>(&specularMotionVectorTag));
	if (bUseRRSpecularHitDistance)
		inputs.push_back(static_cast<const sl::BaseStructure*>(&specularHitDistanceTag));
	const sl::Result evalResult = slEvaluateFeature(sl::kFeatureDLSS_RR, *StreamlineFrameToken, inputs.data(), static_cast<uint32_t>(inputs.size()), renderBackend->GetGraphicsCommandList());
	bDLSSResetNeeded = false;

	renderBackend->TransitionTexture(outputTarget, EResourceState::UnorderedAccess, EResourceState::ShaderRead);
	if (evalResult == sl::Result::eOk)
	{
		bDLSSRROutputValidThisFrame = true;
		return true;
	}

	bDLSSRROutputValidThisFrame = false;
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
	ClearTextureUAV(SpecularGIMoments[0].get(), clear2);
	ClearTextureUAV(SpecularGIMoments[1].get(), clear2);
	ClearTextureUAV(DiffuseGIRawAux.get(), clear4);
	ClearTextureUAV(DiffuseGIRaw.get(), clear4);
	ClearTextureUAV(DiffuseGIHashCachedAux.get(), clear4);
	ClearTextureUAV(DiffuseGIHashCached.get(), clear4);
	ClearTextureUAV(ScreenProbeGIResolved.get(), clear4);
	ClearTextureUAV(ScreenProbeGIProbeDebug.get(), clear4);
	ClearTextureUAV(ScreenProbeGIRadiance[0].get(), clear4);
	ClearTextureUAV(ScreenProbeGIRadiance[1].get(), clear4);
	for (UINT historyIndex = 0; historyIndex < 2; ++historyIndex)
	{
		for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
			ClearTextureUAV(ScreenProbeGISH[historyIndex][coefficientIndex].get(), clear4);
	}
	ClearTextureUAV(ScreenProbeGIMetadata[0].get(), clear4);
	ClearTextureUAV(ScreenProbeGIMetadata[1].get(), clear4);
	ClearTextureUAV(ScreenProbeGIHistory[0].get(), clear4);
	ClearTextureUAV(ScreenProbeGIHistory[1].get(), clear4);
	ClearTextureUAV(DiffuseGITemporalAux[0].get(), clear4);
	ClearTextureUAV(DiffuseGITemporalAux[1].get(), clear4);
	ClearTextureUAV(DiffuseGITemporal[0].get(), clear4);
	ClearTextureUAV(DiffuseGITemporal[1].get(), clear4);
}

Corona::EAntiAliasingMode Corona::NormalizeAntiAliasingMode(ERenderingMode renderingMode, EAntiAliasingMode requestedMode) const
{
	if (static_cast<int>(requestedMode) < 0 || requestedMode >= EAntiAliasingMode::COUNT)
		requestedMode = EAntiAliasingMode::OFF;

	const bool bD3D12Backend =
		renderBackend &&
		renderBackend->GetAPI() == ERenderBackendAPI::D3D12;

#if WITH_STREAMLINE
	if (!bD3D12Backend && IsDLSSMode(requestedMode))
		return renderingMode == ERenderingMode::PATHTRACING ? EAntiAliasingMode::OFF : EAntiAliasingMode::TAA;
#else
	if (IsDLSSMode(requestedMode))
		return renderingMode == ERenderingMode::PATHTRACING ? EAntiAliasingMode::OFF : EAntiAliasingMode::TAA;
#endif

	if (renderingMode == ERenderingMode::PATHTRACING)
	{
		if (requestedMode == EAntiAliasingMode::DLSS_RR ||
			requestedMode == EAntiAliasingMode::DLSS_SR)
		{
#if WITH_STREAMLINE
			return (bDLSSRRAvailable && bEnablePathTracingDLSSRR) ? EAntiAliasingMode::DLSS_RR : EAntiAliasingMode::OFF;
#else
			return EAntiAliasingMode::OFF;
#endif
		}

		return requestedMode == EAntiAliasingMode::TAA ? EAntiAliasingMode::OFF : requestedMode;
	}

#if WITH_STREAMLINE
	if (requestedMode == EAntiAliasingMode::DLSS_RR)
	{
		if (bDLSSRRAvailable && bDLSSAvailable)
			return EAntiAliasingMode::DLSS_RR;
		return bDLSSAvailable ? EAntiAliasingMode::DLSS_SR : EAntiAliasingMode::TAA;
	}
	if (requestedMode == EAntiAliasingMode::DLSS_SR && !bDLSSAvailable)
		return EAntiAliasingMode::TAA;
#endif

	return requestedMode;
}

void Corona::ApplyRenderingAndAAMode(ERenderingMode requestedRenderingMode, EAntiAliasingMode requestedAAMode)
{
	if (requestedRenderingMode != ERenderingMode::HYBRID &&
		requestedRenderingMode != ERenderingMode::PATHTRACING)
		requestedRenderingMode = ERenderingMode::HYBRID;

	const ERenderingMode previousRenderingMode = RenderingMode;
	const EAntiAliasingMode previousAAMode = AntiAliasingMode;
	const EAntiAliasingMode normalizedAAMode = NormalizeAntiAliasingMode(requestedRenderingMode, requestedAAMode);

	if (previousRenderingMode == requestedRenderingMode && previousAAMode == normalizedAAMode)
		return;

	RenderingMode = requestedRenderingMode;
	AntiAliasingMode = normalizedAAMode;
	if (RenderWorld.bHasFrameSourceState)
	{
		RenderWorld.FrameSourceState.RenderingMode = RenderingMode;
		RenderWorld.FrameSourceState.AntiAliasingMode = AntiAliasingMode;
	}
	{
		std::lock_guard<std::mutex> pendingDeltaLock(RenderFrameDeltaMutex);
		for (RenderFrameDelta& pendingDelta : PendingRenderFrameDeltas)
		{
			if (!pendingDelta.bHasFrameSourceState)
				continue;
			pendingDelta.FrameSourceState.RenderingMode = RenderingMode;
			pendingDelta.FrameSourceState.AntiAliasingMode = AntiAliasingMode;
		}
	}

	const bool bRenderingModeChanged = previousRenderingMode != RenderingMode;
	if (bRenderingModeChanged)
		MarkRayTracingSceneDirty();

	const bool bForceResourceReload =
		bRenderingModeChanged ||
		IsDLSSMode(previousAAMode) ||
		IsDLSSMode(AntiAliasingMode);
	ResetAllAccumulationState(bForceResourceReload);

	AppendCpuRuntimeTrace(
		L"[ApplyRenderingAndAAMode] renderMode " + std::to_wstring(static_cast<int>(previousRenderingMode)) +
		L"->" + std::to_wstring(static_cast<int>(RenderingMode)) +
		L", aa " + std::wstring(GetAntiAliasingModeName(previousAAMode)) +
		L"->" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)) +
		L", forceReload=" + std::to_wstring(bForceResourceReload ? 1 : 0));
}

bool Corona::RenderResolutionResourcesMatchCurrentState() const
{
	const UINT displayWidth = m_width;
	const UINT displayHeight = m_height;
	const UINT renderWidth = GetRenderWidth();
	const UINT renderHeight = GetRenderHeight();
	const auto textureMatches = [](const std::shared_ptr<Texture>& texture, UINT width, UINT height)
	{
		return texture &&
			texture->textureDesc.Width == width &&
			texture->textureDesc.Height == height;
	};

	if (!textureMatches(ColorBuffers[0], displayWidth, displayHeight) ||
		!textureMatches(ColorBuffers[1], displayWidth, displayHeight) ||
		!textureMatches(PathTracingAccumBuffer[0], displayWidth, displayHeight) ||
		!textureMatches(PathTracingAccumBuffer[1], displayWidth, displayHeight))
		return false;

	if (!textureMatches(LightingBuffer, renderWidth, renderHeight) ||
		!textureMatches(DirectLightingBuffer, renderWidth, renderHeight) ||
		!textureMatches(DLSSRRBuffer, renderWidth, renderHeight) ||
		!textureMatches(AlbedoBuffer, renderWidth, renderHeight) ||
		!textureMatches(SpecularAlbedoBuffer, renderWidth, renderHeight) ||
		!textureMatches(VelocityBuffer, renderWidth, renderHeight) ||
		!textureMatches(RoughnessMetalicBuffer, renderWidth, renderHeight) ||
		!textureMatches(UnjitteredDepthBuffers[0], renderWidth, renderHeight) ||
		!textureMatches(UnjitteredDepthBuffers[1], renderWidth, renderHeight) ||
		!textureMatches(PathTracingSpecularHitDistanceBuffer, renderWidth, renderHeight) ||
		!textureMatches(PathTracingSpecularMotionVectorBuffer, renderWidth, renderHeight))
		return false;

	return true;
}

void Corona::ResetAllAccumulationState(bool forceUpscaleReload)
{
	bPendingUpscaleRefresh = true;
	bForceUpscaleReload = bForceUpscaleReload || forceUpscaleReload;
	DLSSTransitionFramesRemaining = bForceUpscaleReload ? 2u : 0u;
	FrameCounter = 0;
	PathTracingAccumulatedFrames = 0;
	IndirectAccumulatedFrames = 0;
	ResolvedColorBufferIndex = ColorBufferWriteIndex;
	PrevJitter = glm::vec2(0.0f);
	CurrentJitter = glm::vec2(0.0f);
	bTemporalAAHistoryValid = false;
	bTemporalDenoiserHistoryValid = false;
	bScreenProbeGIAtlasHistoryValid = false;
	bScreenProbeGIHistoryValid = false;
	bScreenProbeLightingBootstrapPending = false;
	bSpatialHashGIHistoryValid = false;
	ScreenProbeGIAtlasWriteIndex = 0;
	ScreenProbeGIHistoryWriteIndex = 0;
	SpatialHashGIWriteIndex = 0;
	bPendingTemporalHistoryClear = true;
	bResetTemporalStateNextUpdate = true;
	bUseLightingBufferFallbackForToneMap = true;
	bDLSSRROutputValidThisFrame = false;
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

void Corona::RecreateRenderResolutionResources()
{
	if (!renderBackend)
		return;

	const UINT DisplayWidth = m_width;
	const UINT DisplayHeight = m_height;
	const UINT RenderWidthLocal = GetRenderWidth();
	const UINT RenderHeightLocal = GetRenderHeight();
	const ETextureFormat HybridFloat4UAVFormat =
		(renderBackend && renderBackend->GetAPI() == ERenderBackendAPI::Vulkan)
		? ETextureFormat::RGBA32Float
		: ETextureFormat::RGBA16Float;

	auto releaseTexture = [&](std::shared_ptr<Texture>& texture)
	{
		if (dx12_rhi && texture)
			dx12_rhi->ForgetDynamicTexture(texture.get());
		texture.reset();
	};

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

	const bool bRecreateDisplaySizedBuffers =
		!ColorBuffers[0] ||
		ColorBuffers[0]->textureDesc.Width != DisplayWidth ||
		ColorBuffers[0]->textureDesc.Height != DisplayHeight;
	if (bRecreateDisplaySizedBuffers)
	{
		releaseTexture(ColorBuffers[0]);
		releaseTexture(ColorBuffers[1]);
		releaseTexture(PathTracingAccumBuffer[0]);
		releaseTexture(PathTracingAccumBuffer[1]);
	}

	releaseTexture(LightingBuffer);
	releaseTexture(DirectLightingBuffer);
	releaseTexture(DLSSRRBuffer);
	releaseTexture(NormalBuffers[0]);
	releaseTexture(NormalBuffers[1]);
	releaseTexture(GeomNormalBuffers[0]);
	releaseTexture(GeomNormalBuffers[1]);
	releaseTexture(ShadowBuffer);
	releaseTexture(AmbientOcclusionBuffer);
	releaseTexture(SkyLightingBuffer);
	releaseTexture(SpecularGIRaw);
	releaseTexture(SpecularGITemporal[0]);
	releaseTexture(SpecularGITemporal[1]);
	releaseTexture(SpecularGIMoments[0]);
	releaseTexture(SpecularGIMoments[1]);
	releaseTexture(DiffuseGIRawAux);
	releaseTexture(DiffuseGIRaw);
	releaseTexture(DiffuseGIHashCachedAux);
	releaseTexture(DiffuseGIHashCached);
	releaseTexture(ScreenProbeGIResolved);
	releaseTexture(ScreenProbeGIProbeDebug);
	releaseTexture(ScreenProbeGIRadiance[0]);
	releaseTexture(ScreenProbeGIRadiance[1]);
	for (UINT historyIndex = 0; historyIndex < 2; ++historyIndex)
	{
		for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
			releaseTexture(ScreenProbeGISH[historyIndex][coefficientIndex]);
	}
	releaseTexture(ScreenProbeGIMetadata[0]);
	releaseTexture(ScreenProbeGIMetadata[1]);
	releaseTexture(ScreenProbeGIHistory[0]);
	releaseTexture(ScreenProbeGIHistory[1]);
	releaseTexture(DiffuseGITemporalAux[0]);
	releaseTexture(DiffuseGITemporalAux[1]);
	releaseTexture(DiffuseGITemporal[0]);
	releaseTexture(DiffuseGITemporal[1]);
	releaseTexture(AlbedoBuffer);
	releaseTexture(SpecularAlbedoBuffer);
	releaseTexture(VelocityBuffer);
	releaseTexture(RoughnessMetalicBuffer);
	releaseTexture(PathTracingSpecularHitDistanceBuffer);
	releaseTexture(PathTracingSpecularMotionVectorBuffer);
	releaseTexture(DepthBuffer);
	releaseTexture(UnjitteredDepthBuffers[0]);
	releaseTexture(UnjitteredDepthBuffers[1]);

	if (bRecreateDisplaySizedBuffers)
	{
		ColorBuffers[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1);
		ColorBuffers[0]->MakeRTV();
		NAME_D3D12_OBJECT(ColorBuffers[0]->resource);

		ColorBuffers[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1);
		ColorBuffers[1]->MakeRTV();
		NAME_D3D12_OBJECT(ColorBuffers[1]->resource);

		PathTracingAccumBuffer[0] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1, glm::vec4(0.0f));
		NAME_D3D12_OBJECT(PathTracingAccumBuffer[0]->resource);

		PathTracingAccumBuffer[1] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1, glm::vec4(0.0f));
		NAME_D3D12_OBJECT(PathTracingAccumBuffer[1]->resource);
	}

	LightingBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	LightingBuffer->MakeRTV();
	NAME_D3D12_OBJECT(LightingBuffer->resource);

	DirectLightingBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	DirectLightingBuffer->MakeRTV();
	NAME_D3D12_OBJECT(DirectLightingBuffer->resource);

	DLSSRRBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(DLSSRRBuffer->resource);

	NormalBuffers[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	NormalBuffers[0]->MakeRTV();
	NAME_D3D12_OBJECT(NormalBuffers[0]->resource);

	NormalBuffers[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	NormalBuffers[1]->MakeRTV();
	NAME_D3D12_OBJECT(NormalBuffers[1]->resource);

	GeomNormalBuffers[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	GeomNormalBuffers[0]->MakeRTV();
	NAME_D3D12_OBJECT(GeomNormalBuffers[0]->resource);

	GeomNormalBuffers[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	GeomNormalBuffers[1]->MakeRTV();
	NAME_D3D12_OBJECT(GeomNormalBuffers[1]->resource);

	ShadowBuffer = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(ShadowBuffer->resource);

	AmbientOcclusionBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(1.0f));
	NAME_D3D12_OBJECT(AmbientOcclusionBuffer->resource);

	SkyLightingBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	NAME_D3D12_OBJECT(SkyLightingBuffer->resource);

	SpecularGIRaw = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(SpecularGIRaw->resource);

	SpecularGITemporal[0] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(SpecularGITemporal[0]->resource);

	SpecularGITemporal[1] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(SpecularGITemporal[1]->resource);

	SpecularGIMoments[0] = createTexture2D(ETextureFormat::RG16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(SpecularGIMoments[0]->resource);

	SpecularGIMoments[1] = createTexture2D(ETextureFormat::RG16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(SpecularGIMoments[1]->resource);

	DiffuseGIRawAux = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(DiffuseGIRawAux->resource);

	DiffuseGIRaw = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(DiffuseGIRaw->resource);

	DiffuseGIHashCachedAux = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(DiffuseGIHashCachedAux->resource);

	DiffuseGIHashCached = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(DiffuseGIHashCached->resource);

	ScreenProbeGIResolved = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(ScreenProbeGIResolved->resource);

	ScreenProbeGIProbeDebug = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(ScreenProbeGIProbeDebug->resource);

	const int ScreenProbeAtlasWidth = std::max(1, (static_cast<int>(RenderWidthLocal) + 3) / 4);
	const int ScreenProbeAtlasHeight = std::max(1, (static_cast<int>(RenderHeightLocal) + 3) / 4);

	ScreenProbeGIRadiance[0] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, ScreenProbeAtlasWidth, ScreenProbeAtlasHeight, 1);
	NAME_D3D12_OBJECT(ScreenProbeGIRadiance[0]->resource);

	ScreenProbeGIRadiance[1] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, ScreenProbeAtlasWidth, ScreenProbeAtlasHeight, 1);
	NAME_D3D12_OBJECT(ScreenProbeGIRadiance[1]->resource);

	for (UINT historyIndex = 0; historyIndex < 2; ++historyIndex)
	{
		for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		{
			ScreenProbeGISH[historyIndex][coefficientIndex] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, ScreenProbeAtlasWidth, ScreenProbeAtlasHeight, 1);
			NAME_D3D12_OBJECT(ScreenProbeGISH[historyIndex][coefficientIndex]->resource);
		}
	}

	ScreenProbeGIMetadata[0] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, ScreenProbeAtlasWidth, ScreenProbeAtlasHeight, 1);
	NAME_D3D12_OBJECT(ScreenProbeGIMetadata[0]->resource);

	ScreenProbeGIMetadata[1] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, ScreenProbeAtlasWidth, ScreenProbeAtlasHeight, 1);
	NAME_D3D12_OBJECT(ScreenProbeGIMetadata[1]->resource);

	ScreenProbeGIHistory[0] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(ScreenProbeGIHistory[0]->resource);

	ScreenProbeGIHistory[1] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(ScreenProbeGIHistory[1]->resource);

	DiffuseGITemporalAux[0] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(DiffuseGITemporalAux[0]->resource);

	DiffuseGITemporalAux[1] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(DiffuseGITemporalAux[1]->resource);

	DiffuseGITemporal[0] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(DiffuseGITemporal[0]->resource);

	DiffuseGITemporal[1] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(DiffuseGITemporal[1]->resource);

	AlbedoBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	AlbedoBuffer->MakeRTV();
	NAME_D3D12_OBJECT(AlbedoBuffer->resource);

	SpecularAlbedoBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	SpecularAlbedoBuffer->MakeRTV();
	NAME_D3D12_OBJECT(SpecularAlbedoBuffer->resource);

	VelocityBuffer = createTexture2D(ETextureFormat::RG16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f));
	VelocityBuffer->MakeRTV();
	NAME_D3D12_OBJECT(VelocityBuffer->resource);

	RoughnessMetalicBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.001f, 0.0f, 0.0f, 0.0f));
	RoughnessMetalicBuffer->MakeRTV();
	NAME_D3D12_OBJECT(RoughnessMetalicBuffer->resource);

	PathTracingSpecularHitDistanceBuffer = createTexture2D(ETextureFormat::R32Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(Far));
	NAME_D3D12_OBJECT(PathTracingSpecularHitDistanceBuffer->resource);

	PathTracingSpecularMotionVectorBuffer = createTexture2D(ETextureFormat::RG16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f));
	NAME_D3D12_OBJECT(PathTracingSpecularMotionVectorBuffer->resource);

	DepthBuffer = createTexture2D(ETextureFormat::D32Float, TextureUsage_DepthStencil, RenderWidthLocal, RenderHeightLocal, 1);
	DepthBuffer->MakeDSV();
	NAME_D3D12_OBJECT(DepthBuffer->resource);

	UnjitteredDepthBuffers[0] = createTexture2D(ETextureFormat::R32Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	UnjitteredDepthBuffers[0]->MakeRTV();
	NAME_D3D12_OBJECT(UnjitteredDepthBuffers[0]->resource);

	UnjitteredDepthBuffers[1] = createTexture2D(ETextureFormat::R32Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	UnjitteredDepthBuffers[1]->MakeRTV();
	NAME_D3D12_OBJECT(UnjitteredDepthBuffers[1]->resource);

	AppendCpuRuntimeTrace(
		L"[RecreateRenderResolutionResources] render=" + std::to_wstring(RenderWidthLocal) +
		L"x" + std::to_wstring(RenderHeightLocal) +
		L", display=" + std::to_wstring(DisplayWidth) +
		L"x" + std::to_wstring(DisplayHeight) +
		L", displaySized=" + std::to_wstring(bRecreateDisplaySizedBuffers ? 1 : 0));
}

void Corona::ReloadRenderResolutionAssets()
{
	if (!renderBackend)
		return;

	const auto reloadStart = CpuClock::now();
	renderBackend->WaitForGpu();
#if WITH_STREAMLINE
	if (bStreamlineInitialized && (bDLSSAvailable || bDLSSRRAvailable))
	{
		slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle(0));
		slFreeResources(sl::kFeatureDLSS_RR, sl::ViewportHandle(0));
	}
#endif
	RecreateRenderResolutionResources();
	ColorBufferWriteIndex = 0;
	ResolvedColorBufferIndex = 0;
	GIBufferWriteIndex = 0;
	DLSSTransitionFramesRemaining = 2u;
	FrameCounter = 0;
	PathTracingAccumulatedFrames = 0;
	IndirectAccumulatedFrames = 0;
	PrevJitter = glm::vec2(0.0f);
	CurrentJitter = glm::vec2(0.0f);
	bTemporalAAHistoryValid = false;
	bTemporalDenoiserHistoryValid = false;
	bScreenProbeGIAtlasHistoryValid = false;
	bScreenProbeGIHistoryValid = false;
	bScreenProbeLightingBootstrapPending = false;
	bSpatialHashGIHistoryValid = false;
	ScreenProbeGIAtlasWriteIndex = 0;
	ScreenProbeGIHistoryWriteIndex = 0;
	SpatialHashGIWriteIndex = 0;
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
	PrevIndirectSkyLightingStrength = SkyLightingStrength;
	PrevIndirectDiffuseGISkyLightingEnabled = !(bEnableSkyLighting && bEnableRayTracedSkyLighting);
	PrevIndirectPrefilteredEnvRoughnessThreshold = PrefilteredEnvRoughnessThreshold;
	PrevIndirectPrefilteredEnvRoughnessFade = PrefilteredEnvRoughnessFade;
	PrevIndirectPrefilteredEnvSpecularEnabled = bEnablePrefilteredEnvSpecular;
	bUseLightingBufferFallbackForToneMap = true;
	bDLSSRROutputValidThisFrame = false;
#if WITH_STREAMLINE
	bDLSSResetNeeded = true;
#endif
	AppendCpuRuntimeTrace(
		L"[ReloadRenderResolutionAssets] lightweight elapsedMs=" +
		std::to_wstring(ElapsedMilliseconds(reloadStart, CpuClock::now())));
}

void Corona::RefreshUpscaleSettings(bool reloadAssets)
{
	UINT desiredRenderWidth = m_width;
	UINT desiredRenderHeight = m_height;
	auto ApplyDLSSJitterPhaseSettings = [&](UINT32 basePhaseCount)
	{
		DLSSJitterPhaseCountAuto = std::clamp(basePhaseCount, 1u, 512u);
		if (DLSSJitterPhaseCountOverride > 0)
		{
			DLSSJitterPhaseCount = std::clamp(DLSSJitterPhaseCountOverride, 1u, 512u);
			return;
		}

		const float scaledPhaseCount = static_cast<float>(DLSSJitterPhaseCountAuto) * std::max(0.25f, DLSSJitterPhaseScale);
		DLSSJitterPhaseCount = std::clamp(static_cast<UINT32>(std::max(1.0f, ceilf(scaledPhaseCount))), 1u, 512u);
	};

#if WITH_STREAMLINE
	if (RenderingMode == ERenderingMode::HYBRID && bDLSSAvailable && AntiAliasingMode == EAntiAliasingMode::DLSS_SR)
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

		ApplyDLSSJitterPhaseSettings(static_cast<UINT32>(std::max(1.0f, ceilf(8.0f * static_cast<float>(m_width) / static_cast<float>(desiredRenderWidth)))));
		bDLSSResetNeeded = true;
	}
	else if (RenderingMode == ERenderingMode::HYBRID && bDLSSRRAvailable && AntiAliasingMode == EAntiAliasingMode::DLSS_RR)
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

		ApplyDLSSJitterPhaseSettings(static_cast<UINT32>(std::max(1.0f, ceilf(8.0f * static_cast<float>(m_width) / static_cast<float>(desiredRenderWidth)))));
		bDLSSResetNeeded = true;
	}
	else if (RenderingMode == ERenderingMode::PATHTRACING && IsPathTracingDLSSRREnabled())
	{
		desiredRenderWidth = m_width;
		desiredRenderHeight = m_height;
		ApplyDLSSJitterPhaseSettings(1u);
		bDLSSResetNeeded = true;
	}
#endif

	const bool bResolutionChanged = desiredRenderWidth != RenderWidth || desiredRenderHeight != RenderHeight;
	RenderWidth = desiredRenderWidth;
	RenderHeight = desiredRenderHeight;
	AppendCpuRuntimeTrace(
		L"[RefreshUpscaleSettings] render=" + std::to_wstring(RenderWidth) +
		L"x" + std::to_wstring(RenderHeight) +
		L", effective=" + std::to_wstring(GetRenderWidth()) +
		L"x" + std::to_wstring(GetRenderHeight()) +
		L", mode=" + std::to_wstring(static_cast<int>(RenderingMode)) +
		L", aa=" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)) +
		L", dlssJitter=" + std::to_wstring(DLSSJitterPhaseCount) +
		L", dlssJitterAuto=" + std::to_wstring(DLSSJitterPhaseCountAuto) +
		L", dlssJitterScale=" + std::to_wstring(DLSSJitterPhaseScale) +
		L", dlssJitterOverride=" + std::to_wstring(DLSSJitterPhaseCountOverride));
	if (reloadAssets && (bResolutionChanged || bForceUpscaleReload))
		ReloadRenderResolutionAssets();
	bForceUpscaleReload = false;
}

Texture* Corona::GetCurrentResolveSource() const
{
	if (RenderingMode == ERenderingMode::PATHTRACING)
	{
		if (IsPathTracingDLSSRREnabled())
		{
			if (bDLSSRROutputValidThisFrame && DLSSRRBuffer)
				return DLSSRRBuffer.get();
		}
		return PathTracingAccumBuffer[PathTracingWriteIndex].get();
	}

	if (bUseLightingBufferFallbackForToneMap && LightingBuffer)
	{
		if (bDLSSRROutputValidThisFrame && DLSSRRBuffer)
			return DLSSRRBuffer.get();
		return LightingBuffer.get();
	}

	return ColorBuffers[ResolvedColorBufferIndex].get();
}

void Corona::ParseCommandLineArgs(WCHAR* argv[], int argc)
{
	auto AppendStartupTrace = [](const std::wstring& line)
	{
		const std::filesystem::path tracePath = RuntimePaths::LogFile(L"vulkan_runtime_trace.log");
		std::filesystem::create_directories(tracePath.parent_path());
		std::wofstream traceFile(tracePath, std::ios::app);
		if (traceFile.is_open())
			traceFile << line << L"\n";
	};
	AppendStartupTrace(L"[ParseCommandLineArgs] begin");
	for (int i = 1; i < argc; ++i)
	{
		if (_wcsnicmp(argv[i], L"-warp", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/warp", wcslen(argv[i])) == 0)
		{
			m_useWarpDevice = true;
			m_title += L" (WARP)";
		}
	}

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
	if (argc <= 1)
	{
		bCommandLineRenderBackendOverrideSet = true;
		CommandLineRenderBackendAPI = ERenderBackendAPI::D3D12;
		bCommandLineRenderModeOverrideSet = true;
		CommandLineRenderingMode = ERenderingMode::HYBRID;
		bCommandLineAAOverrideSet = true;
		CommandLineSelectedAAMode = EAntiAliasingMode::DLSS_RR;
		bCommandLineAutoDumpOverrideSet = true;
		bCommandLineAutoDumpEnabled = false;
		bStartupSponzaFlyMode = true;
		bEnableStartupLuauScript = false;
		AppendStartupTrace(L"[ParseCommandLineArgs] no args: default dx12, hybrid, dlss-rr, sponza, user-mode");
	}

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
		if (arg == L"--sponza-fly" || arg == L"--sponza" || arg == L"--scene-sponza" || arg == L"--fly-camera")
		{
			bStartupSponzaFlyMode = true;
			bEnableStartupLuauScript = false;
			bCommandLineAutoDumpOverrideSet = true;
			bCommandLineAutoDumpEnabled = false;
			continue;
		}
		if (arg == L"--startup-scripts" || arg == L"--game-character" || arg == L"--dungeon-character")
		{
			bStartupSponzaFlyMode = false;
			bEnableStartupLuauScript = true;
			continue;
		}
		if (arg == L"--no-startup-scripts" || arg == L"--disable-startup-scripts")
		{
			bEnableStartupLuauScript = false;
			continue;
		}
		if (arg == L"--pt-dlss-rr" || arg == L"--pathtracing-dlss-rr" || arg == L"--pt-rr-gbuffer")
		{
			bEnablePathTracingDLSSRR = true;
			bCommandLineAAOverrideSet = true;
			CommandLineSelectedAAMode = EAntiAliasingMode::DLSS_RR;
			continue;
		}
		if (arg == L"--no-pt-dlss-rr" || arg == L"--disable-pt-dlss-rr" || arg == L"--no-pt-rr-gbuffer")
		{
			bEnablePathTracingDLSSRR = false;
			continue;
		}
		if (arg == L"--pt-rr-specular-mv" || arg == L"--enable-pt-rr-specular-mv")
		{
			bEnablePathTracingRRSpecularMotionVectors = true;
			bEnablePathTracingRRSpecularHitDistance = false;
			continue;
		}
		if (arg == L"--no-pt-rr-specular-mv" || arg == L"--disable-pt-rr-specular-mv")
		{
			bEnablePathTracingRRSpecularMotionVectors = false;
			continue;
		}
		if (arg == L"--pt-rr-specular-hit-distance" || arg == L"--pt-rr-specular-hitdist")
		{
			bEnablePathTracingRRSpecularHitDistance = true;
			bEnablePathTracingRRSpecularMotionVectors = false;
			continue;
		}
		if (arg == L"--no-pt-rr-specular-hit-distance" || arg == L"--no-pt-rr-specular-hitdist")
		{
			bEnablePathTracingRRSpecularHitDistance = false;
			continue;
		}
		if (arg == L"--pt-rr-stable-primary-rays" || arg == L"--pt-rr-primary-ray-stability")
		{
			bEnablePathTracingRRPrimaryRayStabilization = true;
			continue;
		}
		if (arg == L"--no-pt-rr-stable-primary-rays" || arg == L"--no-pt-rr-primary-ray-stability")
		{
			bEnablePathTracingRRPrimaryRayStabilization = false;
			continue;
		}
		if (arg == L"--hybrid-rr-specular-mv" || arg == L"--enable-hybrid-rr-specular-mv")
		{
			bEnableHybridRRSpecularMotionVectors = true;
			continue;
		}
		if (arg == L"--no-hybrid-rr-specular-mv" || arg == L"--disable-hybrid-rr-specular-mv")
		{
			bEnableHybridRRSpecularMotionVectors = false;
			continue;
		}
		if (arg == L"--hybrid-rr-specular-hit-distance" || arg == L"--hybrid-rr-specular-hitdist")
		{
			bEnableHybridRRSpecularHitDistance = true;
			continue;
		}
		if (arg == L"--no-hybrid-rr-specular-hit-distance" || arg == L"--no-hybrid-rr-specular-hitdist")
		{
			bEnableHybridRRSpecularHitDistance = false;
			continue;
		}
		if (arg == L"--hybrid-rr-specular-guide-ray")
		{
			bEnableHybridRRSpecularGuideRay = true;
			continue;
		}
		if (arg == L"--no-hybrid-rr-specular-guide-ray")
		{
			bEnableHybridRRSpecularGuideRay = false;
			continue;
		}
		std::wstring hybridSpecularMVScaleValue = ParseValueArg(arg, L"--hybrid-rr-specular-mv-scale", L"-hybrid-rr-specular-mv-scale", i);
		if (!hybridSpecularMVScaleValue.empty())
		{
			try
			{
				HybridRRSpecularMotionVectorScale = std::clamp(std::stof(hybridSpecularMVScaleValue), -2.0f, 2.0f);
				bEnableHybridRRSpecularMotionVectors = fabsf(HybridRRSpecularMotionVectorScale) > 1.0e-4f;
			}
			catch (...)
			{
			}
			continue;
		}
		std::wstring specularMVScaleValue = ParseValueArg(arg, L"--pt-rr-specular-mv-scale", L"-pt-rr-specular-mv-scale", i);
		if (!specularMVScaleValue.empty())
		{
			try
			{
				PathTracingRRSpecularMotionVectorScale = std::clamp(std::stof(specularMVScaleValue), -2.0f, 2.0f);
				bEnablePathTracingRRSpecularMotionVectors = fabsf(PathTracingRRSpecularMotionVectorScale) > 1.0e-4f;
			}
			catch (...)
			{
			}
			continue;
		}
		std::wstring ptSppValue = ParseValueArg(arg, L"--pt-spp", L"-pt-spp", i);
		if (ptSppValue.empty())
			ptSppValue = ParseValueArg(arg, L"--spp", L"-spp", i);
		if (!ptSppValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(ptSppValue);
				PathTracingViewParam.SamplesPerPixel = static_cast<UINT32>(std::clamp<unsigned long>(value, 1ul, 16ul));
			}
			catch (...)
			{
			}
			continue;
		}
		if (arg == L"--rtao")
		{
			bEnableRTAO = true;
			continue;
		}
		if (arg == L"--no-rtao" || arg == L"--disable-rtao")
		{
			bEnableRTAO = false;
			continue;
		}
		if (arg == L"--diffuse-gi" || arg == L"--enable-diffuse-gi")
		{
			bEnableDiffuseGI = true;
			continue;
		}
		if (arg == L"--no-diffuse-gi" || arg == L"--disable-diffuse-gi")
		{
			bEnableDiffuseGI = false;
			continue;
		}
		if (arg == L"--rt-diffuse-gi-ser" || arg == L"--enable-rt-diffuse-gi-ser" || arg == L"--diffuse-gi-ser")
		{
			bEnableRTDiffuseGISER = true;
			continue;
		}
		if (arg == L"--no-rt-diffuse-gi-ser" || arg == L"--disable-rt-diffuse-gi-ser" || arg == L"--no-diffuse-gi-ser")
		{
			bEnableRTDiffuseGISER = false;
			continue;
		}
		if (arg == L"--rt-reflection-ser" || arg == L"--enable-rt-reflection-ser" || arg == L"--reflection-ser")
		{
			bEnableRTReflectionSER = true;
			continue;
		}
		if (arg == L"--no-rt-reflection-ser" || arg == L"--disable-rt-reflection-ser" || arg == L"--no-reflection-ser")
		{
			bEnableRTReflectionSER = false;
			continue;
		}
		if (arg == L"--specular-gi" || arg == L"--enable-specular-gi" || arg == L"--indirect-specular" || arg == L"--enable-indirect-specular")
		{
			bEnableSpecularGI = true;
			continue;
		}
		if (arg == L"--no-specular-gi" || arg == L"--disable-specular-gi" || arg == L"--no-indirect-specular" || arg == L"--disable-indirect-specular")
		{
			bEnableSpecularGI = false;
			continue;
		}
		if (arg == L"--direct-diffuse" || arg == L"--enable-direct-diffuse")
		{
			bEnableDirectDiffuse = true;
			continue;
		}
		if (arg == L"--no-direct-diffuse" || arg == L"--disable-direct-diffuse")
		{
			bEnableDirectDiffuse = false;
			continue;
		}
		if (arg == L"--direct-specular" || arg == L"--enable-direct-specular")
		{
			bEnableDirectSpecular = true;
			continue;
		}
		if (arg == L"--no-direct-specular" || arg == L"--disable-direct-specular")
		{
			bEnableDirectSpecular = false;
			continue;
		}
		if (arg == L"--indirect-specular-only" || arg == L"--specular-gi-only")
		{
			bEnableDiffuseGI = false;
			bEnableSpecularGI = true;
			bEnableDirectDiffuse = false;
			bEnableDirectSpecular = false;
			continue;
		}
		if (arg == L"--sky-lighting" || arg == L"--enable-sky-lighting")
		{
			bEnableSkyLighting = true;
			continue;
		}
		if (arg == L"--no-sky-lighting" || arg == L"--disable-sky-lighting")
		{
			bEnableSkyLighting = false;
			continue;
		}
		std::wstring rtaoRadiusValue = ParseValueArg(arg, L"--rtao-radius", L"-rtao-radius", i);
		if (!rtaoRadiusValue.empty())
		{
			try
			{
				RTAOViewParam.Radius = std::clamp(std::stof(rtaoRadiusValue), 2.0f, 256.0f);
			}
			catch (...)
			{
			}
			continue;
		}
		std::wstring rtaoSamplesValue = ParseValueArg(arg, L"--rtao-samples", L"-rtao-samples", i);
		if (!rtaoSamplesValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(rtaoSamplesValue);
				RTAOViewParam.SampleCount = static_cast<UINT32>(std::clamp<unsigned long>(value, 1ul, 16ul));
			}
			catch (...)
			{
			}
			continue;
		}
		std::wstring rtaoPowerValue = ParseValueArg(arg, L"--rtao-power", L"-rtao-power", i);
		if (!rtaoPowerValue.empty())
		{
			try
			{
				RTAOViewParam.Power = std::clamp(std::stof(rtaoPowerValue), 0.25f, 4.0f);
			}
			catch (...)
			{
			}
			continue;
		}
		std::wstring rtaoBiasValue = ParseValueArg(arg, L"--rtao-bias", L"-rtao-bias", i);
		if (!rtaoBiasValue.empty())
		{
			try
			{
				RTAOViewParam.NormalBias = std::clamp(std::stof(rtaoBiasValue), 0.01f, 2.0f);
			}
			catch (...)
			{
			}
			continue;
		}
		std::wstring surfaceBounceStrengthValue = ParseValueArg(arg, L"--surface-bounce-strength", L"-surface-bounce-strength", i);
		if (surfaceBounceStrengthValue.empty())
			surfaceBounceStrengthValue = ParseValueArg(arg, L"--diffuse-gi-strength", L"-diffuse-gi-strength", i);
		if (!surfaceBounceStrengthValue.empty())
		{
			try
			{
				SurfaceBounceStrength = std::clamp(std::stof(surfaceBounceStrengthValue), 0.0f, 1.0f);
			}
			catch (...)
			{
			}
			continue;
		}
		std::wstring skyLightingStrengthValue = ParseValueArg(arg, L"--sky-lighting-strength", L"-sky-lighting-strength", i);
		if (!skyLightingStrengthValue.empty())
		{
			try
			{
				SkyLightingStrength = std::clamp(std::stof(skyLightingStrengthValue), 0.0f, 1.0f);
			}
			catch (...)
			{
			}
			continue;
		}
		if (arg == L"--camera-path-dump" || arg == L"--dump-camera-path")
		{
			bCommandLineCameraPathDump = true;
			bCommandLineAutoDumpOverrideSet = true;
			bCommandLineAutoDumpEnabled = false;
			continue;
		}
		if (arg == L"--camera-path-diagnostics" || arg == L"--pt-rr-camera-path-diagnostics")
		{
			bCommandLineCameraPathDiagnostics = true;
			continue;
		}
		if (arg == L"--camera-path" || arg == L"-camera-path")
		{
			bCommandLineLoadLatestCameraPath = true;
			if (i + 1 < argc)
			{
				std::wstring rawCameraPath = argv[++i];
				std::wstring loweredCameraPath = ToLower(rawCameraPath);
				if (loweredCameraPath != L"latest")
					CommandLineCameraPathFile = rawCameraPath;
			}
			continue;
		}
		std::wstring cameraPathValue = ParseValueArg(arg, L"--camera-path", L"-camera-path", i);
		if (!cameraPathValue.empty())
		{
			bCommandLineLoadLatestCameraPath = true;
			if (cameraPathValue != L"latest")
				CommandLineCameraPathFile = cameraPathValue;
			continue;
		}

		std::wstring dumpModeValue = ParseValueArg(arg, L"--dump-mode", L"-dump-mode", i);
		if (!dumpModeValue.empty())
		{
			if (dumpModeValue == L"diffuse-gi" || dumpModeValue == L"diffuse_gi" || dumpModeValue == L"diffusegi" || dumpModeValue == L"gi")
			{
				bCommandLineAutoDumpOverrideSet = true;
				bCommandLineAutoDumpEnabled = true;
				bCommandLineDiffuseGIAutoDumpMode = true;
			}
			else if (dumpModeValue == L"readme" || dumpModeValue == L"readme-screenshots" || dumpModeValue == L"readme_screenshots")
			{
				bCommandLineAutoDumpOverrideSet = true;
				bCommandLineAutoDumpEnabled = true;
				bCommandLineReadmeScreenshotDumpMode = true;
			}
			else if (dumpModeValue == L"pathtracing" || dumpModeValue == L"path-tracing" || dumpModeValue == L"path_tracing" || dumpModeValue == L"pt")
			{
				bCommandLineAutoDumpOverrideSet = true;
				bCommandLineAutoDumpEnabled = true;
				bCommandLinePathTracingScreenshotDumpMode = true;
			}
			else if (dumpModeValue == L"lighting-compare" || dumpModeValue == L"lighting_compare" || dumpModeValue == L"gi-compare" || dumpModeValue == L"gi_compare" || dumpModeValue == L"indirect-compare" || dumpModeValue == L"indirect_compare")
			{
				bCommandLineAutoDumpOverrideSet = true;
				bCommandLineAutoDumpEnabled = true;
				bCommandLineLightingCompareDumpMode = true;
			}
			else if (dumpModeValue == L"aa-switch" || dumpModeValue == L"aa_switch" || dumpModeValue == L"aaswitch" || dumpModeValue == L"aa-modes" || dumpModeValue == L"aa_modes")
			{
				bCommandLineAutoDumpOverrideSet = true;
				bCommandLineAutoDumpEnabled = true;
				bCommandLineAASwitchDumpMode = true;
			}
			else if (dumpModeValue == L"specular-sequence" || dumpModeValue == L"specular_sequence" || dumpModeValue == L"specular-noise" || dumpModeValue == L"specular_noise")
			{
				bCommandLineAutoDumpOverrideSet = true;
				bCommandLineAutoDumpEnabled = true;
				bCommandLineSpecularSequenceDumpMode = true;
			}
			continue;
		}
		if (arg == L"--readme-dump" || arg == L"--readme-screenshots")
		{
			bCommandLineAutoDumpOverrideSet = true;
			bCommandLineAutoDumpEnabled = true;
			bCommandLineReadmeScreenshotDumpMode = true;
			continue;
		}
		if (arg == L"--path-tracing-dump" || arg == L"--pathtracing-dump" || arg == L"--pt-dump")
		{
			bCommandLineAutoDumpOverrideSet = true;
			bCommandLineAutoDumpEnabled = true;
			bCommandLinePathTracingScreenshotDumpMode = true;
			continue;
		}
		if (arg == L"--lighting-compare-dump" || arg == L"--gi-compare-dump" || arg == L"--indirect-compare-dump")
		{
			bCommandLineAutoDumpOverrideSet = true;
			bCommandLineAutoDumpEnabled = true;
			bCommandLineLightingCompareDumpMode = true;
			continue;
		}
		if (arg == L"--aa-switch-dump" || arg == L"--aa-modes-dump")
		{
			bCommandLineAutoDumpOverrideSet = true;
			bCommandLineAutoDumpEnabled = true;
			bCommandLineAASwitchDumpMode = true;
			continue;
		}
		if (arg == L"--specular-sequence-dump" || arg == L"--specular-noise-dump")
		{
			bCommandLineAutoDumpOverrideSet = true;
			bCommandLineAutoDumpEnabled = true;
			bCommandLineSpecularSequenceDumpMode = true;
			continue;
		}

		std::wstring giModeValue = ParseValueArg(arg, L"--gi-mode", L"-gi", i);
		if (!giModeValue.empty())
		{
			if (giModeValue == L"screen-probe" || giModeValue == L"screen_probe" || giModeValue == L"screenprobe" || giModeValue == L"probe" || giModeValue == L"probes")
				DiffuseGIMode = EDiffuseGIMode::SCREEN_PROBE;
			else if (giModeValue == L"spatial-hash" || giModeValue == L"spatial_hash" || giModeValue == L"spatialhash" || giModeValue == L"hash" || giModeValue == L"sharc")
				DiffuseGIMode = EDiffuseGIMode::SPATIAL_HASH;
			else
				DiffuseGIMode = EDiffuseGIMode::SIMPLE_RAYTRACE;
			continue;
		}

		std::wstring spatialHashCellValue = ParseValueArg(arg, L"--spatial-hash-cell", L"-spatial-hash-cell", i);
		if (!spatialHashCellValue.empty())
		{
			try
			{
				SpatialHashGICB.CellSize = std::clamp(std::stof(spatialHashCellValue), 4.0f, 256.0f);
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring spatialHashBlendValue = ParseValueArg(arg, L"--spatial-hash-raw-blend", L"-spatial-hash-raw-blend", i);
		if (spatialHashBlendValue.empty())
			spatialHashBlendValue = ParseValueArg(arg, L"--spatial-hash-smoothing", L"-spatial-hash-smoothing", i);
		if (!spatialHashBlendValue.empty())
		{
			try
			{
				SpatialHashGICB.SmoothingStrength = std::clamp(std::stof(spatialHashBlendValue), 0.0f, 1.0f);
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring spatialHashInterpValue = ParseValueArg(arg, L"--spatial-hash-interp", L"-spatial-hash-interp", i);
		if (spatialHashInterpValue.empty())
			spatialHashInterpValue = ParseValueArg(arg, L"--spatial-hash-interpolation", L"-spatial-hash-interpolation", i);
		if (!spatialHashInterpValue.empty())
		{
			try
			{
				SpatialHashGICB.InterpolationStrength = std::clamp(std::stof(spatialHashInterpValue), 0.0f, 1.0f);
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring spatialHashRaysValue = ParseValueArg(arg, L"--spatial-hash-rays", L"-spatial-hash-rays", i);
		if (!spatialHashRaysValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(spatialHashRaysValue);
				RTSpatialHashGIViewParam.RaysPerCell = static_cast<UINT32>(std::clamp<unsigned long>(value, 1ul, 8ul));
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring spatialHashBouncesValue = ParseValueArg(arg, L"--spatial-hash-bounces", L"-spatial-hash-bounces", i);
		if (spatialHashBouncesValue.empty())
			spatialHashBouncesValue = ParseValueArg(arg, L"--spatial-hash-depth", L"-spatial-hash-depth", i);
		if (!spatialHashBouncesValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(spatialHashBouncesValue);
				RTSpatialHashGIViewParam.MaxBounces = static_cast<UINT32>(std::clamp<unsigned long>(value, 1ul, 8ul));
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring diffuseGIDumpFramesValue = ParseValueArg(arg, L"--diffuse-gi-dump-frames", L"-diffuse-gi-dump-frames", i);
		if (diffuseGIDumpFramesValue.empty())
			diffuseGIDumpFramesValue = ParseValueArg(arg, L"--gi-dump-frames", L"-gi-dump-frames", i);
		if (!diffuseGIDumpFramesValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(diffuseGIDumpFramesValue);
				DiffuseGIAutoDumpFrameCount = static_cast<UINT32>(std::clamp<unsigned long>(value, 4ul, 512ul));
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring autoDumpFramesValue = ParseValueArg(arg, L"--auto-dump-frames", L"-auto-dump-frames", i);
		if (autoDumpFramesValue.empty())
			autoDumpFramesValue = ParseValueArg(arg, L"--readme-dump-frames", L"-readme-dump-frames", i);
		if (autoDumpFramesValue.empty())
			autoDumpFramesValue = ParseValueArg(arg, L"--dump-frames", L"-dump-frames", i);
		if (!autoDumpFramesValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(autoDumpFramesValue);
				AutoAADumpFrameCountOverride = static_cast<UINT32>(std::clamp<unsigned long>(value, 1ul, 100000ul));
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring exitAfterFramesValue = ParseValueArg(arg, L"--exit-after-frames", L"-exit-after-frames", i);
		if (exitAfterFramesValue.empty())
			exitAfterFramesValue = ParseValueArg(arg, L"--quit-after-frames", L"-quit-after-frames", i);
		if (!exitAfterFramesValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(exitAfterFramesValue);
				CommandLineExitAfterFrames = static_cast<UINT32>(std::clamp<unsigned long>(value, 1ul, 100000ul));
			}
			catch (...)
			{
			}
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

		std::wstring noiseValue = ParseValueArg(arg, L"--ray-noise", L"-noise", i);
		if (noiseValue.empty())
			noiseValue = ParseValueArg(arg, L"--noise", L"-n", i);
		if (!noiseValue.empty())
		{
			if (noiseValue == L"blue" || noiseValue == L"blue-noise" || noiseValue == L"blue_noise")
				RayNoiseMode = ERayNoiseMode::BLUE_NOISE;
			else if (noiseValue == L"stable" || noiseValue == L"stable-hash" || noiseValue == L"stable_hash" || noiseValue == L"hash")
				RayNoiseMode = ERayNoiseMode::STABLE_HASH;
			else
				RayNoiseMode = ERayNoiseMode::R2_LOW_DISCREPANCY;
			continue;
		}

		std::wstring jitterScaleValue = ParseValueArg(arg, L"--dlss-jitter-scale", L"-dlss-jitter-scale", i);
		if (!jitterScaleValue.empty())
		{
			try
			{
				const float value = std::stof(jitterScaleValue);
				DLSSJitterPhaseScale = std::clamp(value, 0.25f, 16.0f);
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring jitterPhaseValue = ParseValueArg(arg, L"--dlss-jitter-phases", L"-dlss-jitter-phases", i);
		if (!jitterPhaseValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(jitterPhaseValue);
				DLSSJitterPhaseCountOverride = static_cast<UINT32>(std::clamp<unsigned long>(value, 0ul, 512ul));
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring screenProbeSpacingValue = ParseValueArg(arg, L"--screen-probe-spacing", L"-screen-probe-spacing", i);
		if (!screenProbeSpacingValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(screenProbeSpacingValue);
				ScreenProbeGICB.ProbeSpacing = static_cast<UINT32>(std::clamp<unsigned long>(value, 4ul, 64ul));
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring screenProbeRadiusValue = ParseValueArg(arg, L"--screen-probe-radius", L"-screen-probe-radius", i);
		if (!screenProbeRadiusValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(screenProbeRadiusValue);
				ScreenProbeGICB.GatherRadius = static_cast<UINT32>(std::clamp<unsigned long>(value, 1ul, 3ul));
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring screenProbeRawBlendValue = ParseValueArg(arg, L"--screen-probe-raw-blend", L"-screen-probe-raw-blend", i);
		if (!screenProbeRawBlendValue.empty())
		{
			try
			{
				ScreenProbeGICB.RawBlend = std::clamp(std::stof(screenProbeRawBlendValue), 0.0f, 1.0f);
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring screenProbeTemporalAlphaValue = ParseValueArg(arg, L"--screen-probe-temporal-alpha", L"-screen-probe-temporal-alpha", i);
		if (!screenProbeTemporalAlphaValue.empty())
		{
			try
			{
				ScreenProbeGICB.TemporalAlpha = std::clamp(std::stof(screenProbeTemporalAlphaValue), 0.02f, 1.0f);
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring screenProbeRaysValue = ParseValueArg(arg, L"--screen-probe-rays", L"-screen-probe-rays", i);
		if (!screenProbeRaysValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(screenProbeRaysValue);
				RTScreenProbeGIViewParam.RaysPerProbe = static_cast<UINT32>(std::clamp<unsigned long>(value, 1ul, 4ul));
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring screenProbeSHValue = ParseValueArg(arg, L"--screen-probe-sh", L"-screen-probe-sh", i);
		if (!screenProbeSHValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(screenProbeSHValue);
				ScreenProbeGICB.SHCoefficientCount = value <= 4ul ? 4u : 9u;
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring screenProbeEdgeDepthValue = ParseValueArg(arg, L"--screen-probe-edge-depth", L"-screen-probe-edge-depth", i);
		if (!screenProbeEdgeDepthValue.empty())
		{
			try
			{
				ScreenProbeGICB.EdgeDepthWeight = std::clamp(std::stof(screenProbeEdgeDepthValue), 8.0f, 192.0f);
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring screenProbeEdgeNormalValue = ParseValueArg(arg, L"--screen-probe-edge-normal", L"-screen-probe-edge-normal", i);
		if (!screenProbeEdgeNormalValue.empty())
		{
			try
			{
				ScreenProbeGICB.EdgeNormalWeight = std::clamp(std::stof(screenProbeEdgeNormalValue), 1.0f, 96.0f);
			}
			catch (...)
			{
			}
			continue;
		}

		std::wstring screenProbeEdgeSamplesValue = ParseValueArg(arg, L"--screen-probe-edge-samples", L"-screen-probe-edge-samples", i);
		if (!screenProbeEdgeSamplesValue.empty())
		{
			try
			{
				const unsigned long value = std::stoul(screenProbeEdgeSamplesValue);
				ScreenProbeGICB.EdgeSampleCount = static_cast<UINT32>(std::clamp<unsigned long>(value, 1ul, 4ul));
			}
			catch (...)
			{
			}
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
		L", aaOverride=" + std::to_wstring(bCommandLineAAOverrideSet ? 1 : 0) +
		L", aa=" + std::wstring(GetAntiAliasingModeName(CommandLineSelectedAAMode)) +
		L", rayNoise=" + std::wstring(GetRayNoiseModeNameW(RayNoiseMode)) +
		L", diffuseGI=" + std::wstring(GetDiffuseGIModeNameW(DiffuseGIMode)) +
		L", diffuseGIEnabled=" + std::to_wstring(bEnableDiffuseGI ? 1 : 0) +
		L", specularGIEnabled=" + std::to_wstring(bEnableSpecularGI ? 1 : 0) +
		L", directDiffuse=" + std::to_wstring(bEnableDirectDiffuse ? 1 : 0) +
		L", directSpecular=" + std::to_wstring(bEnableDirectSpecular ? 1 : 0) +
		L", skyLighting=" + std::to_wstring(bEnableSkyLighting ? 1 : 0) +
		L", surfaceBounceStrength=" + std::to_wstring(SurfaceBounceStrength) +
		L", skyLightingStrength=" + std::to_wstring(SkyLightingStrength) +
		L", diffuseGIDump=" + std::to_wstring(bCommandLineDiffuseGIAutoDumpMode ? 1 : 0) +
		L", readmeDump=" + std::to_wstring(bCommandLineReadmeScreenshotDumpMode ? 1 : 0) +
		L", pathTracingDump=" + std::to_wstring(bCommandLinePathTracingScreenshotDumpMode ? 1 : 0) +
		L", lightingCompareDump=" + std::to_wstring(bCommandLineLightingCompareDumpMode ? 1 : 0) +
		L", specularSequenceDump=" + std::to_wstring(bCommandLineSpecularSequenceDumpMode ? 1 : 0) +
		L", autoDumpFrames=" + std::to_wstring(AutoAADumpFrameCountOverride) +
		L", exitAfterFrames=" + std::to_wstring(CommandLineExitAfterFrames) +
		L", cameraPathDump=" + std::to_wstring(bCommandLineCameraPathDump ? 1 : 0) +
		L", cameraPathDiagnostics=" + std::to_wstring(bCommandLineCameraPathDiagnostics ? 1 : 0) +
		L", ptDLSSRR=" + std::to_wstring(bEnablePathTracingDLSSRR ? 1 : 0) +
		L", ptSPP=" + std::to_wstring(PathTracingViewParam.SamplesPerPixel) +
		L", ptRRSpecularMV=" + std::to_wstring(bEnablePathTracingRRSpecularMotionVectors ? 1 : 0) +
		L", ptRRSpecularMVScale=" + std::to_wstring(PathTracingRRSpecularMotionVectorScale) +
		L", ptRRSpecularHitDistance=" + std::to_wstring(bEnablePathTracingRRSpecularHitDistance ? 1 : 0) +
		L", ptRRPrimaryRayStability=" + std::to_wstring(bEnablePathTracingRRPrimaryRayStabilization ? 1 : 0) +
		L", hybridRRSpecularMV=" + std::to_wstring(bEnableHybridRRSpecularMotionVectors ? 1 : 0) +
		L", hybridRRSpecularMVScale=" + std::to_wstring(HybridRRSpecularMotionVectorScale) +
		L", hybridRRSpecularHitDistance=" + std::to_wstring(bEnableHybridRRSpecularHitDistance ? 1 : 0) +
		L", hybridRRSpecularGuideRay=" + std::to_wstring(bEnableHybridRRSpecularGuideRay ? 1 : 0) +
		L", sponzaFly=" + std::to_wstring(bStartupSponzaFlyMode ? 1 : 0) +
		L", startupScripts=" + std::to_wstring(bEnableStartupLuauScript ? 1 : 0) +
		L", dlssJitterScale=" + std::to_wstring(DLSSJitterPhaseScale) +
		L", dlssJitterOverride=" + std::to_wstring(DLSSJitterPhaseCountOverride) +
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
	bDiffuseGIAutoDumpMode = bCommandLineDiffuseGIAutoDumpMode;
	if (bDiffuseGIAutoDumpMode)
		bAutoAADumpEnabled = true;
	bReadmeScreenshotDumpMode = bCommandLineReadmeScreenshotDumpMode;
	if (bReadmeScreenshotDumpMode)
		bAutoAADumpEnabled = true;
	bPathTracingScreenshotDumpMode = bCommandLinePathTracingScreenshotDumpMode;
	if (bPathTracingScreenshotDumpMode)
		bAutoAADumpEnabled = true;
	bLightingCompareDumpMode = bCommandLineLightingCompareDumpMode;
	if (bLightingCompareDumpMode)
		bAutoAADumpEnabled = true;
	bAASwitchDumpMode = bCommandLineAASwitchDumpMode;
	if (bAASwitchDumpMode)
		bAutoAADumpEnabled = true;
	bSpecularSequenceDumpMode = bCommandLineSpecularSequenceDumpMode;
	if (bSpecularSequenceDumpMode)
		bAutoAADumpEnabled = true;

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
		else if (aaMode == L"rr" || aaMode == L"dlssrr" || aaMode == L"dlss-rr" || aaMode == L"dlss_rr" || aaMode == L"dlss rr")
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
	AppendCpuRuntimeTrace(
		L"[PromptStartupModeSelection] final backend=" + std::to_wstring(static_cast<int>(StartupRenderBackendAPI)) +
		L", renderMode=" + std::to_wstring(static_cast<int>(StartupRenderingMode)) +
		L", aa=" + std::wstring(GetAntiAliasingModeName(StartupSelectedAAMode)) +
		L", dlssAvailable=" + std::to_wstring(bDLSSAvailable ? 1 : 0) +
		L", dlssRRAvailable=" + std::to_wstring(bDLSSRRAvailable ? 1 : 0) +
		L", autoDump=" + std::to_wstring(bAutoAADumpEnabled ? 1 : 0));
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

	if (bPathTracingScreenshotDumpMode)
	{
		std::filesystem::path dumpDir = RuntimePaths::DumpDirectory() / L"path_tracing_screenshots";
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
		{
			std::wofstream clearLog(logPath, std::ios::trunc);
		}

		StartupRenderingMode = ERenderingMode::PATHTRACING;
		RenderingMode = StartupRenderingMode;
		AntiAliasingMode = EAntiAliasingMode::OFF;
		PathTracingViewParam.SamplesPerPixel = 1;
		PathTracingViewParam.MaxBounces = 4;
		ResetAllAccumulationState(false);
		const UINT32 frameCount = std::max(1000u, AutoAADumpFrameCountOverride);
		AppendAutoAADumpLog(L"[path_tracing] begin frames=" + std::to_wstring(frameCount) + L", spp=" + std::to_wstring(frameCount) + L", aa=off");
		return;
	}

	if (bReadmeScreenshotDumpMode)
	{
		std::filesystem::path dumpDir = RuntimePaths::DumpDirectory() / L"readme_screenshots";
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
		{
			std::wofstream clearLog(logPath, std::ios::trunc);
		}

		StartupRenderingMode = ERenderingMode::HYBRID;
		RenderingMode = StartupRenderingMode;
		AntiAliasingMode = StartupSelectedAAMode;
		bEnableDiffuseGI = true;
		ResetAllAccumulationState(false);
		const UINT32 frameCount = AutoAADumpFrameCountOverride > 0u ? AutoAADumpFrameCountOverride : 1000u;
		AppendAutoAADumpLog(L"[readme_hybrid] begin frames=" + std::to_wstring(frameCount) + L", aa=" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)));
		return;
	}

	if (bDiffuseGIAutoDumpMode)
	{
		std::filesystem::path dumpDir = RuntimePaths::DumpDirectory() / L"diffuse_gi";
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
		{
			std::wofstream clearLog(logPath, std::ios::trunc);
		}

		StartupRenderingMode = ERenderingMode::HYBRID;
		RenderingMode = StartupRenderingMode;
		AntiAliasingMode = EAntiAliasingMode::OFF;
		bEnableDiffuseGI = true;
		DiffuseGIMode = EDiffuseGIMode::SIMPLE_RAYTRACE;
		ResetAllAccumulationState(false);
		AppendAutoAADumpLog(L"[diffuse_gi_simple] begin frames=" + std::to_wstring(DiffuseGIAutoDumpFrameCount) + L", aa=off");
		return;
	}

	if (bLightingCompareDumpMode)
	{
		std::filesystem::path dumpDir = RuntimePaths::DumpDirectory() / L"lighting_compare";
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
		{
			std::wofstream clearLog(logPath, std::ios::trunc);
		}

		StartupRenderingMode = ERenderingMode::HYBRID;
		RenderingMode = StartupRenderingMode;
		AntiAliasingMode = StartupSelectedAAMode;
		bEnableDiffuseGI = true;
		bEnableSkyLighting = true;
		ResetAllAccumulationState(IsDLSSMode(AntiAliasingMode));
		const UINT32 frameCount = AutoAADumpFrameCountOverride > 0u ? AutoAADumpFrameCountOverride : 120u;
		AppendAutoAADumpLog(L"[hybrid_full] begin frames=" + std::to_wstring(frameCount) + L", aa=" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)));
		return;
	}

	if (bAASwitchDumpMode)
	{
		std::filesystem::path dumpDir = RuntimePaths::DumpDirectory() / L"aa_switch";
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
		{
			std::wofstream clearLog(logPath, std::ios::trunc);
		}

		StartupRenderingMode = ERenderingMode::HYBRID;
		RenderingMode = StartupRenderingMode;
		AntiAliasingMode = StartupSelectedAAMode;
		bEnableDiffuseGI = true;
		ResetAllAccumulationState(IsDLSSMode(AntiAliasingMode));
		const UINT32 frameCount = AutoAADumpFrameCountOverride > 0u ? AutoAADumpFrameCountOverride : 60u;
		AppendAutoAADumpLog(L"[aa_switch_0_start] begin frames=" + std::to_wstring(frameCount) + L", aa=" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)));
		return;
	}

	if (bSpecularSequenceDumpMode)
	{
		std::filesystem::path dumpDir = RuntimePaths::DumpDirectory() / L"specular_sequence";
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
		{
			std::wofstream clearLog(logPath, std::ios::trunc);
		}

		StartupRenderingMode = ERenderingMode::HYBRID;
		RenderingMode = StartupRenderingMode;
		AntiAliasingMode = StartupSelectedAAMode;
		bEnableDiffuseGI = true;
		bEnableSpecularGI = true;
		ResetAllAccumulationState(IsDLSSMode(AntiAliasingMode));
		const UINT32 captureFrameCount = AutoAADumpFrameCountOverride > 0u ? AutoAADumpFrameCountOverride : 16u;
		AppendAutoAADumpLog(
			L"[specular_sequence] warmup=64, captureFrames=" + std::to_wstring(captureFrameCount) +
			L", aa=" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)) +
			L", gi=" + std::wstring(GetDiffuseGIModeNameW(DiffuseGIMode)));
		return;
	}

	if (StartupRenderingMode == ERenderingMode::HYBRID)
	{
		const uint32_t maxSupportedHybridStage = renderBackend ? renderBackend->GetMaxSupportedHybridStage() : 7u;
		std::filesystem::path dumpDir = RuntimePaths::DumpDirectory() / L"hybrid_pass_stages";
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
		{
			std::wofstream clearLog(logPath, std::ios::trunc);
		}

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

	std::filesystem::path dumpDir = RuntimePaths::DumpDirectory() / L"aa_modes";
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
	{
		std::wofstream clearLog(logPath, std::ios::trunc);
	}

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
	case 2: return L"stage_02_shadow_raw";
	case 3: return L"stage_03_reflection";
	case 4: return L"stage_04_gi_raw";
	case 5: return L"stage_05_gi_temporal";
	case 6: return L"stage_06_gi_resolved";
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

bool Corona::StartAsyncImageDumpWorkers()
{
	if (bAsyncImageDumpWorkersStarted)
		return true;

	try
	{
		const UINT32 hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
		const UINT32 workerCount = std::clamp(hardwareThreads / 2u, 1u, 4u);
		bAsyncImageDumpStop = false;
		bAsyncImageDumpWorkersStarted = true;
		AsyncImageDumpWorkers.reserve(workerCount);
		for (UINT32 workerIndex = 0; workerIndex < workerCount; ++workerIndex)
			AsyncImageDumpWorkers.emplace_back(&Corona::AsyncImageDumpWorkerMain, this);
		AppendCpuRuntimeTrace(L"[AsyncImageDump] started workers=" + std::to_wstring(workerCount));
	}
	catch (const std::exception& e)
	{
		bAsyncImageDumpWorkersStarted = false;
		AsyncImageDumpWorkers.clear();
		AppendCpuRuntimeTrace(L"[AsyncImageDump] failed to start workers: " + AnsiToWString(e.what()));
		return false;
	}

	return true;
}

bool Corona::EnqueueAsyncImageDump(ScratchImage&& captured, const std::wstring& filePath, bool bHDR)
{
	if (!StartAsyncImageDumpWorkers())
		return false;

	constexpr size_t kMaxQueuedAndActiveJobs = 32;
	{
		std::unique_lock<std::mutex> lock(AsyncImageDumpMutex);
		AsyncImageDumpCV.wait(lock, [&]()
		{
			return bAsyncImageDumpStop ||
				(AsyncImageDumpQueue.size() + AsyncImageDumpActiveJobs) < kMaxQueuedAndActiveJobs;
		});
		if (bAsyncImageDumpStop)
			return false;

		std::unique_ptr<AsyncImageDumpJob> job = std::make_unique<AsyncImageDumpJob>();
		job->Image = std::move(captured);
		job->FilePath = filePath;
		job->bHDR = bHDR;
		AsyncImageDumpQueue.push_back(std::move(job));
	}

	AsyncImageDumpCV.notify_one();
	return true;
}

void Corona::AsyncImageDumpWorkerMain()
{
	const HRESULT coInitResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool bShouldCoUninitialize = SUCCEEDED(coInitResult);

	for (;;)
	{
		std::unique_ptr<AsyncImageDumpJob> job;
		{
			std::unique_lock<std::mutex> lock(AsyncImageDumpMutex);
			AsyncImageDumpCV.wait(lock, [&]()
			{
				return bAsyncImageDumpStop || !AsyncImageDumpQueue.empty();
			});

			if (AsyncImageDumpQueue.empty())
			{
				if (bAsyncImageDumpStop)
					break;
				continue;
			}

			job = std::move(AsyncImageDumpQueue.front());
			AsyncImageDumpQueue.pop_front();
			++AsyncImageDumpActiveJobs;
		}

		std::wstring errorMessage;
		const bool bSaved = job->bHDR ?
			SaveCapturedTextureHDR(job->Image, job->FilePath, &errorMessage) :
			SaveCapturedTexturePNG(job->Image, job->FilePath, &errorMessage);
		if (!bSaved)
		{
			AppendCpuRuntimeTrace(
				L"[AsyncImageDump] save failed path=" + job->FilePath +
				(errorMessage.empty() ? std::wstring() : (L", " + errorMessage)));
		}

		{
			std::lock_guard<std::mutex> lock(AsyncImageDumpMutex);
			if (AsyncImageDumpActiveJobs > 0)
				--AsyncImageDumpActiveJobs;
		}
		AsyncImageDumpCV.notify_all();
	}

	if (bShouldCoUninitialize)
		CoUninitialize();
}

void Corona::WaitForAsyncImageDumps()
{
	if (!bAsyncImageDumpWorkersStarted)
		return;

	std::unique_lock<std::mutex> lock(AsyncImageDumpMutex);
	AsyncImageDumpCV.wait(lock, [&]()
	{
		return AsyncImageDumpQueue.empty() && AsyncImageDumpActiveJobs == 0;
	});
}

void Corona::StopAsyncImageDumpWorkers()
{
	if (!bAsyncImageDumpWorkersStarted)
		return;

	WaitForAsyncImageDumps();
	{
		std::lock_guard<std::mutex> lock(AsyncImageDumpMutex);
		bAsyncImageDumpStop = true;
	}
	AsyncImageDumpCV.notify_all();

	for (std::thread& worker : AsyncImageDumpWorkers)
	{
		if (worker.joinable())
			worker.join();
	}

	AsyncImageDumpWorkers.clear();
	bAsyncImageDumpWorkersStarted = false;
	bAsyncImageDumpStop = false;
	AppendCpuRuntimeTrace(L"[AsyncImageDump] stopped");
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

	if (EnqueueAsyncImageDump(std::move(captured), filePath, true))
		return true;

	std::wstring errorMessage;
	const bool bSaved = SaveCapturedTextureHDR(captured, filePath, &errorMessage);
	if (!bSaved && !errorMessage.empty())
		AppendAutoAADumpLog(L"[capture] hdr " + errorMessage);
	return bSaved;
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

	if (EnqueueAsyncImageDump(std::move(captured), filePath, false))
		return true;

	std::wstring errorMessage;
	const bool bSaved = SaveCapturedTexturePNG(captured, filePath, &errorMessage);
	if (!bSaved && !errorMessage.empty())
		AppendAutoAADumpLog(L"[capture] png " + errorMessage);
	return bSaved;
}

bool Corona::DumpTextureRawFloat(
	Texture* source,
	const std::wstring& filePath,
	D3D12_RESOURCE_STATES beforeState,
	DXGI_FORMAT targetFormat,
	uint32_t channelCount)
{
	if (!source || !renderBackend)
		return false;

	ScratchImage captured;
	HRESULT hr = renderBackend->CaptureTexture(source, captured, beforeState);
	if (FAILED(hr))
	{
		AppendAutoAADumpLog(L"[capture] raw failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
		return false;
	}

	std::wstring errorMessage;
	const bool bSaved = SaveCapturedTextureRawFloat(captured, filePath, targetFormat, channelCount, &errorMessage);
	if (!bSaved && !errorMessage.empty())
		AppendAutoAADumpLog(L"[capture] raw " + errorMessage);
	return bSaved;
}

void Corona::AdvanceAutoAADump(Texture* backbuffer)
{
	if (!bAutoAADumpEnabled || !bAutoAADumpInitialized || bAutoAADumpCompleted)
		return;

	if (bSpecularSequenceDumpMode)
	{
		constexpr UINT32 kWarmupFrameCount = 64u;
		const UINT32 captureFrameCount = AutoAADumpFrameCountOverride > 0u ? AutoAADumpFrameCountOverride : 16u;
		if (RenderingMode != ERenderingMode::HYBRID || !bEnableDiffuseGI || !bEnableSpecularGI)
		{
			RenderingMode = ERenderingMode::HYBRID;
			AntiAliasingMode = StartupSelectedAAMode;
			bEnableDiffuseGI = true;
			bEnableSpecularGI = true;
			ResetAllAccumulationState(IsDLSSMode(AntiAliasingMode));
			AppendAutoAADumpLog(
				L"[specular_sequence] reset for hybrid capture, aa=" +
				std::wstring(GetAntiAliasingModeName(AntiAliasingMode)));
			return;
		}

		if (AutoAADumpFramesInPhase < kWarmupFrameCount)
		{
			++AutoAADumpFramesInPhase;
			return;
		}

		const UINT32 captureIndex = AutoAADumpFramesInPhase - kWarmupFrameCount;
		if (captureIndex < captureFrameCount)
		{
			std::wstringstream frameName;
			frameName << GetAntiAliasingModeName(AntiAliasingMode)
				<< L"_frame_"
				<< std::setfill(L'0') << std::setw(4) << captureIndex;
			const std::wstring base = AutoAADumpDir + L"\\" + frameName.str();

			auto dumpResource = [&](const wchar_t* suffix, Texture* texture, D3D12_RESOURCE_STATES state)
			{
				if (!texture)
					return false;

				const bool pngResult = DumpTexturePNG(texture, base + L"_" + suffix + L".png", state);
				AppendAutoAADumpLog(
					std::wstring(L"[specular_sequence] frame=") + std::to_wstring(captureIndex) +
					L", " + suffix + L" png=" + (pngResult ? L"ok" : L"fail"));
				return pngResult;
			};

			dumpResource(L"specular_raw", SpecularGIRaw.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"specular_temporal", SpecularGITemporal[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"specular_moments", SpecularGIMoments[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"specular_moments_prev", SpecularGIMoments[1 - GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"gbuffer_world_normal", NormalBuffers[ColorBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"gbuffer_geo_normal", GeomNormalBuffers[ColorBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"gbuffer_velocity", VelocityBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"gbuffer_depth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"gbuffer_rm", RoughnessMetalicBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"rr_specular_hit_distance", PathTracingSpecularHitDistanceBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"rr_specular_motion", PathTracingSpecularMotionVectorBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"rtao", AmbientOcclusionBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dumpResource(L"sky_lighting", SkyLightingBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (DirectLightingBuffer)
				dumpResource(L"direct_lighting", DirectLightingBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (LightingBuffer)
				dumpResource(L"lighting", LightingBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (Texture* resolveTarget = GetCurrentResolveSource())
				dumpResource(L"resolve", resolveTarget, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (backbuffer)
				dumpResource(L"screen", backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);

			AppendAutoAADumpLog(
				L"[specular_sequence] frame=" + std::to_wstring(captureIndex) +
				L", frameCounter=" + std::to_wstring(FrameCounter) +
				L", accumulated=" + std::to_wstring(IndirectAccumulatedFrames) +
				L", aa=" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)) +
				L", render=" + std::to_wstring(GetRenderWidth()) + L"x" + std::to_wstring(GetRenderHeight()) +
				L", display=" + std::to_wstring(m_width) + L"x" + std::to_wstring(m_height) +
				L", taaHistory=" + (bTemporalAAHistoryValid ? L"1" : L"0") +
				L", denoiserHistory=" + (bTemporalDenoiserHistoryValid ? L"1" : L"0"));
		}

		++AutoAADumpFramesInPhase;
		if (AutoAADumpFramesInPhase < kWarmupFrameCount + captureFrameCount)
			return;

		bAutoAADumpCompleted = true;
		if (HWND hwnd = Win32Application::GetHwnd())
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		PostQuitMessage(0);
		return;
	}

	if (bAASwitchDumpMode)
	{
		constexpr UINT32 kNumAASwitchPhases = 4u;
		const UINT32 targetFrameCount = AutoAADumpFrameCountOverride > 0u ? AutoAADumpFrameCountOverride : 60u;
		const std::array<EAntiAliasingMode, kNumAASwitchPhases> requestedModes = {
			StartupSelectedAAMode,
			EAntiAliasingMode::TAA,
			EAntiAliasingMode::OFF,
			StartupSelectedAAMode
		};
		const std::array<const wchar_t*, kNumAASwitchPhases> phaseNames = {
			L"aa_switch_0_start",
			L"aa_switch_1_taa",
			L"aa_switch_2_off",
			L"aa_switch_3_start"
		};
		auto normalizeAAMode = [&](EAntiAliasingMode mode) -> EAntiAliasingMode
		{
			const bool bD3D12Backend =
				renderBackend &&
				renderBackend->GetAPI() == ERenderBackendAPI::D3D12;
#if WITH_STREAMLINE
			if (!bD3D12Backend && IsDLSSMode(mode))
				return EAntiAliasingMode::TAA;
			if (mode == EAntiAliasingMode::DLSS_RR && !bDLSSRRAvailable)
				return bDLSSAvailable ? EAntiAliasingMode::DLSS_SR : EAntiAliasingMode::TAA;
			if (mode == EAntiAliasingMode::DLSS_SR && !bDLSSAvailable)
				return EAntiAliasingMode::TAA;
#else
			if (IsDLSSMode(mode))
				return EAntiAliasingMode::TAA;
#endif
			return mode;
		};

		if (AutoAADumpPhase >= kNumAASwitchPhases)
			return;

		const EAntiAliasingMode targetAAMode = normalizeAAMode(requestedModes[AutoAADumpPhase]);
		const wchar_t* currentPhaseName = phaseNames[AutoAADumpPhase];
		if (RenderingMode != ERenderingMode::HYBRID || AntiAliasingMode != targetAAMode || !bEnableDiffuseGI)
		{
			const EAntiAliasingMode previousMode = AntiAliasingMode;
			RenderingMode = ERenderingMode::HYBRID;
			AntiAliasingMode = targetAAMode;
			bEnableDiffuseGI = true;
			ResetAllAccumulationState(IsDLSSMode(previousMode) || IsDLSSMode(targetAAMode));
			AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] begin frames=" + std::to_wstring(targetFrameCount) + L", aa=" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)));
			return;
		}

		if (AutoAADumpFramesInPhase == targetFrameCount - 1)
		{
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

			Texture* resolveTarget = GetCurrentResolveSource();
			dumpResource(L"resolve", resolveTarget, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
			if (LightingBuffer)
				dumpResource(L"lighting", LightingBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
			if (RenderingMode == ERenderingMode::HYBRID && ColorBuffers[ResolvedColorBufferIndex])
				dumpResource(L"colorbuffer", ColorBuffers[ResolvedColorBufferIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
			if (renderBackend)
			{
				renderBackend->RequestWindowCapture(base + L"_screen_preview.png");
				AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] screen capture=requested");
			}
			AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName +
				L"] aa=" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)) +
				L", render=" + std::to_wstring(GetRenderWidth()) + L"x" + std::to_wstring(GetRenderHeight()) +
				L", display=" + std::to_wstring(m_width) + L"x" + std::to_wstring(m_height) +
				L", fallback=" + (bUseLightingBufferFallbackForToneMap ? L"1" : L"0") +
				L", taaHistory=" + (bTemporalAAHistoryValid ? L"1" : L"0") +
				L", resolvedIndex=" + std::to_wstring(ResolvedColorBufferIndex));
		}

		++AutoAADumpFramesInPhase;
		if (AutoAADumpFramesInPhase < targetFrameCount)
			return;

		AutoAADumpFramesInPhase = 0;
		++AutoAADumpPhase;
		if (AutoAADumpPhase < kNumAASwitchPhases)
		{
			const EAntiAliasingMode previousMode = AntiAliasingMode;
			RenderingMode = ERenderingMode::HYBRID;
			AntiAliasingMode = normalizeAAMode(requestedModes[AutoAADumpPhase]);
			bEnableDiffuseGI = true;
			ResetAllAccumulationState(IsDLSSMode(previousMode) || IsDLSSMode(AntiAliasingMode));
			AppendAutoAADumpLog(std::wstring(L"[") + phaseNames[AutoAADumpPhase] + L"] begin frames=" + std::to_wstring(targetFrameCount) + L", aa=" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)));
			return;
		}

		bAutoAADumpCompleted = true;
		if (HWND hwnd = Win32Application::GetHwnd())
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		PostQuitMessage(0);
		return;
	}

	if (bPathTracingScreenshotDumpMode)
	{
		const UINT32 targetFrameCount = std::max(1000u, AutoAADumpFrameCountOverride);
		if (RenderingMode != ERenderingMode::PATHTRACING || AntiAliasingMode != EAntiAliasingMode::OFF)
		{
			RenderingMode = ERenderingMode::PATHTRACING;
			AntiAliasingMode = EAntiAliasingMode::OFF;
			PathTracingViewParam.SamplesPerPixel = 1;
			PathTracingViewParam.MaxBounces = 4;
			ResetAllAccumulationState(false);
			AppendAutoAADumpLog(L"[path_tracing] begin frames=" + std::to_wstring(targetFrameCount) + L", spp=" + std::to_wstring(targetFrameCount) + L", aa=off");
			return;
		}

		if (AutoAADumpFramesInPhase == targetFrameCount - 1)
		{
			Texture* resolveTarget = GetCurrentResolveSource();
			const std::wstring base = AutoAADumpDir + L"\\path_tracing";
			const bool hdrOk = DumpTextureHDR(resolveTarget, base + L".hdr", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			const bool resolvePngOk = DumpTexturePNG(resolveTarget, base + L"_resolve_preview.png", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			AppendAutoAADumpLog(
				L"[path_tracing] frameCounter=" + std::to_wstring(FrameCounter) +
				L", spp=" + std::to_wstring(targetFrameCount) +
				L", maxBounces=" + std::to_wstring(PathTracingViewParam.MaxBounces));
			AppendAutoAADumpLog(std::wstring(L"[path_tracing] resolve hdr=") + (hdrOk ? L"ok" : L"fail") + L", resolve png=" + (resolvePngOk ? L"ok" : L"fail"));
			if (LightingBuffer)
			{
				const bool lightingHdrOk = DumpTextureHDR(LightingBuffer.get(), base + L"_lighting.hdr", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				const bool lightingPngOk = DumpTexturePNG(LightingBuffer.get(), base + L"_lighting_preview.png", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				AppendAutoAADumpLog(std::wstring(L"[path_tracing] lighting hdr=") + (lightingHdrOk ? L"ok" : L"fail") + L", lighting png=" + (lightingPngOk ? L"ok" : L"fail"));
			}
			if (backbuffer)
			{
				const bool screenPngOk = DumpTexturePNG(backbuffer, base + L"_screen_preview.png", D3D12_RESOURCE_STATE_RENDER_TARGET);
				AppendAutoAADumpLog(std::wstring(L"[path_tracing] screen png=") + (screenPngOk ? L"ok" : L"fail"));
			}
		}

		++AutoAADumpFramesInPhase;
		if (AutoAADumpFramesInPhase < targetFrameCount)
			return;

		bAutoAADumpCompleted = true;
		if (HWND hwnd = Win32Application::GetHwnd())
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		PostQuitMessage(0);
		return;
	}

	if (bDiffuseGIAutoDumpMode)
	{
		const UINT32 kDiffuseGIDumpFrames = std::max(1u, DiffuseGIAutoDumpFrameCount);
		constexpr UINT32 kNumDiffuseGIPhases = 3;
		if (AutoAADumpPhase >= kNumDiffuseGIPhases)
			return;

		const bool bSpatialHashPhase = AutoAADumpPhase == 1;
		const bool bScreenProbePhase = AutoAADumpPhase == 2;
		const EDiffuseGIMode targetMode =
			bScreenProbePhase ? EDiffuseGIMode::SCREEN_PROBE :
			(bSpatialHashPhase ? EDiffuseGIMode::SPATIAL_HASH : EDiffuseGIMode::SIMPLE_RAYTRACE);
		const wchar_t* currentPhaseName =
			bScreenProbePhase ? L"diffuse_gi_screen_probe" :
			(bSpatialHashPhase ? L"diffuse_gi_spatial_hash" : L"diffuse_gi_simple");

		if (RenderingMode != ERenderingMode::HYBRID || AntiAliasingMode != EAntiAliasingMode::OFF || DiffuseGIMode != targetMode || !bEnableDiffuseGI)
		{
			RenderingMode = ERenderingMode::HYBRID;
			AntiAliasingMode = EAntiAliasingMode::OFF;
			DiffuseGIMode = targetMode;
			bEnableDiffuseGI = true;
			ResetAllAccumulationState(false);
			AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] begin frames=" + std::to_wstring(kDiffuseGIDumpFrames) + L", aa=off");
			return;
		}

		if (AutoAADumpFramesInPhase == kDiffuseGIDumpFrames - 1)
		{
			const std::wstring base = AutoAADumpDir + L"\\" + currentPhaseName;
			const bool bCanCaptureTexture =
				renderBackend &&
				renderBackend->GetAPI() == ERenderBackendAPI::D3D12;

			auto dumpResource = [&](const wchar_t* suffix, Texture* texture, bool dumpHdr)
			{
				if (!texture || !bCanCaptureTexture)
					return;

				const std::wstring fileBase = base + L"_" + suffix;
				bool hdrOk = true;
				if (dumpHdr)
					hdrOk = DumpTextureHDR(texture, fileBase + L".hdr", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				const bool pngOk = DumpTexturePNG(texture, fileBase + L"_preview.png", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] " + suffix +
					L" hdr=" + (dumpHdr ? (hdrOk ? L"ok" : L"fail") : L"skip") +
					L", png=" + (pngOk ? L"ok" : L"fail"));
			};

			AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName +
				L"] frameCounter=" + std::to_wstring(FrameCounter) +
				L", accumulated=" + std::to_wstring(IndirectAccumulatedFrames) +
				L", mode=" + std::wstring(GetDiffuseGIModeNameW(DiffuseGIMode)));
			dumpResource(L"gi_diffuse_raw", DiffuseGIRaw.get(), true);
			dumpResource(L"gi_diffuse_hash_cache", DiffuseGIHashCached.get(), true);
			dumpResource(L"screen_probe_atlas", ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex].get(), true);
			dumpResource(L"screen_probe_sh", ScreenProbeGISH[ScreenProbeGIAtlasWriteIndex][0].get(), false);
			dumpResource(L"screen_probe_meta", ScreenProbeGIMetadata[ScreenProbeGIAtlasWriteIndex].get(), false);
			dumpResource(L"screen_probe_gi", ScreenProbeGIResolved.get(), true);
			dumpResource(L"screen_probe_probes", ScreenProbeGIProbeDebug.get(), true);
			dumpResource(L"gi_diffuse_temporal", DiffuseGITemporal[GIBufferWriteIndex].get(), true);
			if (!bCanCaptureTexture)
				AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] texture capture skipped for non-DX12 backend");
		}

		++AutoAADumpFramesInPhase;
		if (AutoAADumpFramesInPhase < kDiffuseGIDumpFrames)
			return;

		AutoAADumpFramesInPhase = 0;
		++AutoAADumpPhase;
		if (AutoAADumpPhase < kNumDiffuseGIPhases)
		{
			DiffuseGIMode = AutoAADumpPhase == 1 ? EDiffuseGIMode::SPATIAL_HASH : EDiffuseGIMode::SCREEN_PROBE;
			ResetAllAccumulationState(false);
			AppendAutoAADumpLog(
				std::wstring(AutoAADumpPhase == 1 ? L"[diffuse_gi_spatial_hash]" : L"[diffuse_gi_screen_probe]") +
				L" begin frames=" + std::to_wstring(kDiffuseGIDumpFrames) +
				L", aa=off");
			return;
		}

		bAutoAADumpCompleted = true;
		if (HWND hwnd = Win32Application::GetHwnd())
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		PostQuitMessage(0);
		return;
	}

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
				dumpResource(L"gbuffer_geo_normal", GeomNormalBuffers[ColorBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				dumpResource(L"gbuffer_velocity", VelocityBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				dumpResource(L"gbuffer_depth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				dumpResource(L"gbuffer_rm", RoughnessMetalicBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);

				if (AutoAADumpPhase >= 1)
					dumpResource(L"shadow_raw", ShadowBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
				if (AutoAADumpPhase >= 3)
					dumpResource(L"specular_raw", SpecularGIRaw.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
				if (AutoAADumpPhase >= 4)
				{
					dumpResource(L"gi_diffuse_raw", DiffuseGIRaw.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"gi_diffuse_raw_aux", DiffuseGIRawAux.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"screen_probe_atlas", ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"screen_probe_sh", ScreenProbeGISH[ScreenProbeGIAtlasWriteIndex][0].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
					dumpResource(L"screen_probe_meta", ScreenProbeGIMetadata[ScreenProbeGIAtlasWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
					dumpResource(L"screen_probe_gi", ScreenProbeGIResolved.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"screen_probe_probes", ScreenProbeGIProbeDebug.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
				}
				if (AutoAADumpPhase >= 5)
				{
					dumpResource(L"gi_diffuse_temporal", DiffuseGITemporal[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"gi_diffuse_temporal_aux", DiffuseGITemporalAux[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"gi_specular_temporal", SpecularGITemporal[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
				}
				if (AutoAADumpPhase >= 6)
				{
					dumpResource(L"gi_diffuse_resolved", ((DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE && ScreenProbeGIResolved) ? ScreenProbeGIResolved.get() : DiffuseGITemporal[GIBufferWriteIndex].get()), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
					dumpResource(L"gi_specular_resolved", SpecularGITemporal[GIBufferWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
				}
				if (bLightingStage)
				{
					dumpResource(L"rtao", AmbientOcclusionBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
					dumpResource(L"sky_lighting", SkyLightingBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
					dumpResource(L"direct_lighting", DirectLightingBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
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

	if (!bReadmeScreenshotDumpMode && !bLightingCompareDumpMode && StartupSelectedAAMode != EAntiAliasingMode::DLSS_RR)
	{
		bAutoAADumpCompleted = true;
		return;
	}

	const UINT32 kHybridDumpFrames =
		AutoAADumpFrameCountOverride > 0u ? AutoAADumpFrameCountOverride :
		(bReadmeScreenshotDumpMode ? 1000u : (bLightingCompareDumpMode ? 120u : 60u));
	const UINT32 kPathTracingDumpFrames =
		AutoAADumpFrameCountOverride > 0u ? AutoAADumpFrameCountOverride :
		(bReadmeScreenshotDumpMode ? 1000u : (bLightingCompareDumpMode ? 512u : 180u));
	const UINT32 kNumDumpPhases = bLightingCompareDumpMode ? 5u : (bReadmeScreenshotDumpMode ? 2u : 3u);

	if (AutoAADumpPhase >= kNumDumpPhases)
		return;

	const bool bLightingHybridFullPhase = bLightingCompareDumpMode && AutoAADumpPhase == 0;
	const bool bLightingHybridNoSkyPhase = bLightingCompareDumpMode && AutoAADumpPhase == 1;
	const bool bLightingHybridNoDiffusePhase = bLightingCompareDumpMode && AutoAADumpPhase == 2;
	const bool bLightingHybridDirectOnlyPhase = bLightingCompareDumpMode && AutoAADumpPhase == 3;
	const bool bLightingPathTracingPhase = bLightingCompareDumpMode && AutoAADumpPhase == 4;
	const bool bHybridOnPhase = !bLightingCompareDumpMode && AutoAADumpPhase == 0;
	const bool bHybridOffPhase = !bLightingCompareDumpMode && !bReadmeScreenshotDumpMode && AutoAADumpPhase == 1;
	const bool bPathTracingPhase = bLightingCompareDumpMode ? bLightingPathTracingPhase : (bReadmeScreenshotDumpMode ? AutoAADumpPhase == 1 : AutoAADumpPhase == 2);
	const bool bHybridPhase = !bPathTracingPhase;
	const UINT32 targetFrameCount = bHybridPhase ? kHybridDumpFrames : kPathTracingDumpFrames;
	const ERenderingMode targetRenderingMode = bHybridPhase ? ERenderingMode::HYBRID : ERenderingMode::PATHTRACING;
	const EAntiAliasingMode targetAAMode = bHybridPhase ? StartupSelectedAAMode : EAntiAliasingMode::OFF;
	const bool targetEnableDiffuseGI = bLightingCompareDumpMode ? !(bLightingHybridNoDiffusePhase || bLightingHybridDirectOnlyPhase) : !bHybridOffPhase;
	const bool targetEnableSkyLighting = bLightingCompareDumpMode ? !(bLightingHybridNoSkyPhase || bLightingHybridDirectOnlyPhase) : bEnableSkyLighting;
	const wchar_t* currentPhaseName =
		bLightingCompareDumpMode ?
			(bLightingHybridFullPhase ? L"hybrid_full" :
			(bLightingHybridNoSkyPhase ? L"hybrid_no_sky" :
			(bLightingHybridNoDiffusePhase ? L"hybrid_no_diffuse" :
			(bLightingHybridDirectOnlyPhase ? L"hybrid_direct_only" : L"path_tracing")))) :
		(bReadmeScreenshotDumpMode ?
			(bPathTracingPhase ? L"readme_path_tracing" : L"readme_hybrid") :
			(bHybridOnPhase ? L"hybrid_diffuse_on" :
			(bHybridOffPhase ? L"hybrid_diffuse_off" : L"path_tracing")));

	if (RenderingMode != targetRenderingMode || AntiAliasingMode != targetAAMode || bEnableDiffuseGI != targetEnableDiffuseGI || (bLightingCompareDumpMode && bEnableSkyLighting != targetEnableSkyLighting))
	{
		RenderingMode = targetRenderingMode;
		AntiAliasingMode = targetAAMode;
		bEnableDiffuseGI = targetEnableDiffuseGI;
		if (bLightingCompareDumpMode)
			bEnableSkyLighting = targetEnableSkyLighting;
		ResetAllAccumulationState(bHybridPhase);
		PathTracingViewParam.SamplesPerPixel = 1;
		PathTracingViewParam.MaxBounces = 4;
		AppendAutoAADumpLog(std::wstring(L"[") + currentPhaseName + L"] begin frames=" + std::to_wstring(targetFrameCount));
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
			L", skyLighting=" + (bEnableSkyLighting ? L"1" : L"0") +
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
			dumpResource(L"rtao", AmbientOcclusionBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
			dumpResource(L"sky_lighting", SkyLightingBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
			dumpResource(L"direct_lighting", DirectLightingBuffer.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
			dumpResource(L"gi_diffuse_raw", DiffuseGIRaw.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
			dumpResource(L"screen_probe_atlas", ScreenProbeGIRadiance[ScreenProbeGIAtlasWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
			dumpResource(L"screen_probe_sh", ScreenProbeGISH[ScreenProbeGIAtlasWriteIndex][0].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
			dumpResource(L"screen_probe_meta", ScreenProbeGIMetadata[ScreenProbeGIAtlasWriteIndex].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
			dumpResource(L"screen_probe_gi", ScreenProbeGIResolved.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
			dumpResource(L"screen_probe_probes", ScreenProbeGIProbeDebug.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
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
		WaitForAsyncImageDumps();
		const bool bNextLightingHybridFullPhase = bLightingCompareDumpMode && AutoAADumpPhase == 0;
		const bool bNextLightingHybridNoSkyPhase = bLightingCompareDumpMode && AutoAADumpPhase == 1;
		const bool bNextLightingHybridNoDiffusePhase = bLightingCompareDumpMode && AutoAADumpPhase == 2;
		const bool bNextLightingHybridDirectOnlyPhase = bLightingCompareDumpMode && AutoAADumpPhase == 3;
		const bool bNextLightingPathTracingPhase = bLightingCompareDumpMode && AutoAADumpPhase == 4;
		const bool bNextHybridPhase = bLightingCompareDumpMode ? !bNextLightingPathTracingPhase : (bReadmeScreenshotDumpMode ? AutoAADumpPhase == 0 : AutoAADumpPhase < 2);
		RenderingMode = bNextHybridPhase ? ERenderingMode::HYBRID : ERenderingMode::PATHTRACING;
		AntiAliasingMode = (RenderingMode == ERenderingMode::HYBRID) ? StartupSelectedAAMode : EAntiAliasingMode::OFF;
		bEnableDiffuseGI = bLightingCompareDumpMode ? !(bNextLightingHybridNoDiffusePhase || bNextLightingHybridDirectOnlyPhase) : (bReadmeScreenshotDumpMode ? true : (AutoAADumpPhase != 1));
		if (bLightingCompareDumpMode)
			bEnableSkyLighting = !(bNextLightingHybridNoSkyPhase || bNextLightingHybridDirectOnlyPhase);
		ResetAllAccumulationState(RenderingMode == ERenderingMode::HYBRID);
		const bool bNextHybridOnPhase = !bLightingCompareDumpMode && AutoAADumpPhase == 0;
		const bool bNextHybridOffPhase = !bLightingCompareDumpMode && !bReadmeScreenshotDumpMode && AutoAADumpPhase == 1;
		const bool bNextPathTracingPhase = bLightingCompareDumpMode ? bNextLightingPathTracingPhase : (bReadmeScreenshotDumpMode ? AutoAADumpPhase == 1 : AutoAADumpPhase == 2);
		const UINT32 nextFrameCount = RenderingMode == ERenderingMode::HYBRID ? kHybridDumpFrames : kPathTracingDumpFrames;
		const wchar_t* nextPhaseName =
			bLightingCompareDumpMode ?
				(bNextLightingHybridFullPhase ? L"hybrid_full" :
				(bNextLightingHybridNoSkyPhase ? L"hybrid_no_sky" :
				(bNextLightingHybridNoDiffusePhase ? L"hybrid_no_diffuse" :
				(bNextLightingHybridDirectOnlyPhase ? L"hybrid_direct_only" : L"path_tracing")))) :
			(bReadmeScreenshotDumpMode ?
				(bNextPathTracingPhase ? L"readme_path_tracing" : L"readme_hybrid") :
				(bNextHybridOnPhase ? L"hybrid_diffuse_on" :
				(bNextHybridOffPhase ? L"hybrid_diffuse_off" : L"path_tracing")));
		AppendAutoAADumpLog(std::wstring(L"[") + nextPhaseName + L"] begin frames=" + std::to_wstring(nextFrameCount));
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

	std::filesystem::path screenshotDir = RuntimePaths::DumpDirectory() / L"screenshots";
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
	return (RuntimePaths::DumpDirectory() / L"camera_paths").wstring();
}

std::wstring Corona::GetCameraPathDumpDirectory() const
{
	return (RuntimePaths::DumpDirectory() / L"camera_path_frames").wstring();
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

	const std::wstring pathFile = AllocateUniqueCameraPathFilePath(nullptr);
	if (!pathFile.empty() && SaveCameraPath(pathFile))
		return;

	LastCameraPathStatus = L"Camera path recording ended, but failed to allocate a unique path file.";
}

std::wstring Corona::AllocateUniqueCameraPathFilePath(const wchar_t* tag) const
{
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
			<< std::setw(2) << localTime.wSecond;
		if (tag && tag[0] != L'\0')
			filename << L"_" << tag;
		filename << L"_" << std::setw(4) << attempt
			<< L".coronapath";

		const std::filesystem::path candidate = pathDir / filename.str();
		if (!std::filesystem::exists(candidate))
			return candidate.wstring();
	}

	return {};
}

bool Corona::WriteCameraPathFile(const std::wstring& filePath, bool bUpdateActivePath)
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

	if (bUpdateActivePath)
	{
		ActiveCameraPathFile = filePath;
		bCameraPathListDirty = true;
	}
	return true;
}

bool Corona::SaveCameraPath(const std::wstring& filePath)
{
	if (!WriteCameraPathFile(filePath, true))
		return false;

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
	const std::wstring activePath = NormalizePathKey(ActiveCameraPathFile);
	for (size_t entryIndex = 0; entryIndex < CameraPathEntries.size(); ++entryIndex)
	{
		if (NormalizePathKey(CameraPathEntries[entryIndex].FilePath) == activePath)
		{
			SelectedCameraPathIndex = static_cast<int>(entryIndex);
			break;
		}
	}
	LastCameraPathStatus =
		L"Loaded camera path: " + filePath +
		L" (" + std::to_wstring(static_cast<unsigned long long>(CameraPathKeyframes.size())) + L" keyframes)";
	return true;
}

void Corona::RefreshCameraPathList()
{
	const std::wstring previousSelection =
		SelectedCameraPathIndex >= 0 && SelectedCameraPathIndex < static_cast<int>(CameraPathEntries.size()) ?
		CameraPathEntries[SelectedCameraPathIndex].FilePath :
		ActiveCameraPathFile;

	std::vector<std::pair<std::filesystem::file_time_type, CameraPathListEntry>> entries;
	auto addCameraPathEntry = [&](const std::filesystem::path& filePath, const std::wstring& displayName, std::filesystem::file_time_type writeTime)
	{
		std::error_code fileEc;
		if (!std::filesystem::is_regular_file(filePath, fileEc) || filePath.extension() != L".coronapath")
			return;

		const std::wstring normalizedPath = NormalizePathKey(filePath.wstring());
		for (const auto& existing : entries)
		{
			if (NormalizePathKey(existing.second.FilePath) == normalizedPath)
				return;
		}

		CameraPathListEntry listEntry;
		listEntry.DisplayName = displayName.empty() ? filePath.filename().wstring() : displayName;
		listEntry.FilePath = filePath.wstring();
		entries.push_back({ writeTime, listEntry });
	};

	auto getWriteTime = [](const std::filesystem::path& filePath)
	{
		std::error_code timeEc;
		const auto writeTime = std::filesystem::last_write_time(filePath, timeEc);
		return timeEc ? std::filesystem::file_time_type{} : writeTime;
	};

	std::filesystem::path pathDir(GetCameraPathDirectory());
	std::error_code ec;
	std::filesystem::create_directories(pathDir, ec);
	if (std::filesystem::exists(pathDir, ec))
	{
		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(pathDir, ec))
		{
			if (ec || !entry.is_regular_file())
				continue;
			addCameraPathEntry(entry.path(), entry.path().filename().wstring(), getWriteTime(entry.path()));
		}
	}

	std::filesystem::path dumpRoot(GetCameraPathDumpDirectory());
	std::filesystem::path latestDumpDir;
	std::filesystem::file_time_type latestDumpTime{};
	if (std::filesystem::exists(dumpRoot, ec))
	{
		for (const std::filesystem::directory_entry& dumpEntry : std::filesystem::directory_iterator(dumpRoot, ec))
		{
			if (ec || !dumpEntry.is_directory())
				continue;

			const std::filesystem::path dumpDir = dumpEntry.path();
			const std::filesystem::file_time_type dumpTime = getWriteTime(dumpDir);
			if (latestDumpDir.empty() || dumpTime > latestDumpTime)
			{
				latestDumpDir = dumpDir;
				latestDumpTime = dumpTime;
			}

			bool bAddedReferencedPath = false;
			const std::filesystem::path infoPath = dumpDir / L"dump_info.txt";
			std::wifstream infoFile(infoPath);
			if (infoFile.is_open())
			{
				std::wstring token;
				while (infoFile >> token)
				{
					if (token == L"camera_path")
					{
						std::wstring referencedPath;
						std::getline(infoFile, referencedPath);
						referencedPath = TrimLeadingWhitespace(referencedPath);
						if (!referencedPath.empty())
						{
							std::filesystem::path cameraPath(referencedPath);
							if (std::filesystem::exists(cameraPath))
							{
								addCameraPathEntry(
									cameraPath,
									cameraPath.filename().wstring(),
									getWriteTime(cameraPath));
								bAddedReferencedPath = true;
							}
						}
						break;
					}

					std::wstring ignoredLine;
					std::getline(infoFile, ignoredLine);
				}
			}

			if (!bAddedReferencedPath)
			{
				const std::filesystem::path snapshotPath = dumpDir / L"camera_path.coronapath";
				if (std::filesystem::exists(snapshotPath))
				{
					addCameraPathEntry(
						snapshotPath,
						dumpDir.filename().wstring() + L" / camera_path.coronapath",
						getWriteTime(snapshotPath));
				}
			}
		}
	}

	std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs)
	{
		return lhs.first > rhs.first;
	});

	CameraPathEntries.clear();
	CameraPathEntries.reserve(entries.size());
	for (const auto& entry : entries)
		CameraPathEntries.push_back(entry.second);

	SelectedCameraPathIndex = CameraPathEntries.empty() ? -1 : 0;
	const std::wstring normalizedPreviousSelection = NormalizePathKey(previousSelection);
	const std::wstring normalizedActivePath = NormalizePathKey(ActiveCameraPathFile);
	for (size_t entryIndex = 0; entryIndex < CameraPathEntries.size(); ++entryIndex)
	{
		const std::wstring normalizedEntryPath = NormalizePathKey(CameraPathEntries[entryIndex].FilePath);
		if ((!normalizedPreviousSelection.empty() && normalizedEntryPath == normalizedPreviousSelection) ||
			(normalizedPreviousSelection.empty() && !normalizedActivePath.empty() && normalizedEntryPath == normalizedActivePath))
		{
			SelectedCameraPathIndex = static_cast<int>(entryIndex);
			break;
		}
	}

	if (LastCameraPathDumpDir.empty() && !latestDumpDir.empty())
		LastCameraPathDumpDir = latestDumpDir.wstring();

	bCameraPathListDirty = false;
}

bool Corona::EnsureCameraPathSavedForDump(const std::filesystem::path& dumpDir)
{
	const std::wstring pathDir = GetCameraPathDirectory();
	const bool bActivePathVisibleInUi =
		!ActiveCameraPathFile.empty() &&
		std::filesystem::exists(std::filesystem::path(ActiveCameraPathFile)) &&
		IsPathInsideDirectory(ActiveCameraPathFile, pathDir);

	if (!bActivePathVisibleInUi)
	{
		const std::wstring dumpPathFile = AllocateUniqueCameraPathFilePath(L"dump");
		if (dumpPathFile.empty() || !SaveCameraPath(dumpPathFile))
		{
			AppendCpuRuntimeTrace(L"[CameraPath] failed to persist camera path before dump.");
			return false;
		}
	}

	const std::filesystem::path snapshotPath = dumpDir / L"camera_path.coronapath";
	if (!WriteCameraPathFile(snapshotPath.wstring(), false))
		AppendCpuRuntimeTrace(L"[CameraPath] failed to write dump camera_path.coronapath snapshot: " + snapshotPath.wstring());

	bCameraPathListDirty = true;
	return true;
}

bool Corona::LoadSelectedCameraPath()
{
	if (bCameraPathListDirty)
		RefreshCameraPathList();

	if (SelectedCameraPathIndex < 0 || SelectedCameraPathIndex >= static_cast<int>(CameraPathEntries.size()))
	{
		LastCameraPathStatus = L"No saved camera path is selected.";
		return false;
	}

	return LoadCameraPath(CameraPathEntries[SelectedCameraPathIndex].FilePath);
}

bool Corona::LoadLatestCameraPath()
{
	RefreshCameraPathList();
	if (!CameraPathEntries.empty())
	{
		SelectedCameraPathIndex = 0;
		return LoadSelectedCameraPath();
	}

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
	bCameraPathDumpExitWhenComplete = false;
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

	EnsureCameraPathSavedForDump(std::filesystem::path(LastCameraPathDumpDir));

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
		infoFile << L"render_size " << m_width << L" " << m_height << L"\n";
		infoFile << L"near_far " << Near << L" " << Far << L"\n";
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
	bCameraPathDumpExitWhenComplete = false;
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

void Corona::DumpCameraPathDiagnosticFrame()
{
	if (!bCommandLineCameraPathDiagnostics || LastCameraPathDumpDir.empty())
		return;

	const D3D12_RESOURCE_STATES shaderReadState =
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	std::filesystem::path diagnosticDir = std::filesystem::path(LastCameraPathDumpDir) / L"diagnostics";
	std::error_code ec;
	std::filesystem::create_directories(diagnosticDir, ec);
	if (ec)
		return;

	std::wstringstream framePrefix;
	framePrefix << L"frame_" << std::setfill(L'0') << std::setw(6) << CameraPathDumpFrameIndex;
	const std::filesystem::path basePath = diagnosticDir / framePrefix.str();

	auto dumpTexture = [&](const wchar_t* suffix, Texture* texture, bool bDumpHdr, DXGI_FORMAT rawFormat = DXGI_FORMAT_UNKNOWN, uint32_t rawChannelCount = 0)
	{
		if (!texture)
			return;

		const std::wstring fileBase = (basePath.wstring() + L"_" + suffix);
		DumpTexturePNG(texture, fileBase + L"_preview.png", shaderReadState);
		if (bDumpHdr)
			DumpTextureHDR(texture, fileBase + L".hdr", shaderReadState);
		if (rawFormat != DXGI_FORMAT_UNKNOWN && rawChannelCount > 0)
			DumpTextureRawFloat(texture, fileBase + L".rawf", shaderReadState, rawFormat, rawChannelCount);
	};

	dumpTexture(L"pt_input", PathTracingAccumBuffer[PathTracingWriteIndex].get(), true);
	dumpTexture(L"rr_output", DLSSRRBuffer.get(), true);
	dumpTexture(L"depth", UnjitteredDepthBuffers[ColorBufferWriteIndex].get(), true, DXGI_FORMAT_R32_FLOAT, 1);
	dumpTexture(L"motion", VelocityBuffer.get(), true, DXGI_FORMAT_R32G32_FLOAT, 2);
	dumpTexture(L"specular_hit_distance", PathTracingSpecularHitDistanceBuffer.get(), true, DXGI_FORMAT_R32_FLOAT, 1);
	dumpTexture(L"specular_motion", PathTracingSpecularMotionVectorBuffer.get(), true, DXGI_FORMAT_R32G32_FLOAT, 2);
	dumpTexture(L"roughness_metallic", RoughnessMetalicBuffer.get(), true);
	dumpTexture(L"specular_albedo", SpecularAlbedoBuffer.get(), true);
	dumpTexture(L"albedo", AlbedoBuffer.get(), false);
	dumpTexture(L"normal", NormalBuffers[ColorBufferWriteIndex].get(), false);
	dumpTexture(L"geom_normal", GeomNormalBuffers[ColorBufferWriteIndex].get(), false);
	dumpTexture(L"specular_raw", SpecularGIRaw.get(), true);
	dumpTexture(L"specular_temporal", SpecularGITemporal[GIBufferWriteIndex].get(), true);
	dumpTexture(L"specular_temporal_prev", SpecularGITemporal[1 - GIBufferWriteIndex].get(), true);
	dumpTexture(L"specular_moments", SpecularGIMoments[GIBufferWriteIndex].get(), true);
	dumpTexture(L"diffuse_raw", DiffuseGIRaw.get(), true);
	dumpTexture(L"diffuse_raw_aux", DiffuseGIRawAux.get(), true);
	dumpTexture(L"diffuse_hash_cached", DiffuseGIHashCached.get(), true);
	dumpTexture(L"diffuse_temporal", DiffuseGITemporal[GIBufferWriteIndex].get(), true);
	dumpTexture(L"diffuse_temporal_prev", DiffuseGITemporal[1 - GIBufferWriteIndex].get(), true);
	if (ScreenProbeGIResolved)
		dumpTexture(L"screen_probe_gi", ScreenProbeGIResolved.get(), true);
	if (SkyLightingBuffer)
		dumpTexture(L"sky_lighting", SkyLightingBuffer.get(), true);
	if (DirectLightingBuffer)
		dumpTexture(L"direct_lighting", DirectLightingBuffer.get(), true);
	if (LightingBuffer)
		dumpTexture(L"lighting", LightingBuffer.get(), true);
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

	DumpCameraPathDiagnosticFrame();
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
		if (bCameraPathDumpExitWhenComplete)
			PostQuitMessage(1);
	}
	else if (CameraPathDumpFrameIndex >= CameraPathDumpFrameCount)
	{
		bCameraPathDumping = false;
		bCameraPathPlaying = false;
		LastCameraPathStatus =
			L"Camera path playback and frame dump complete: " + LastCameraPathDumpDir +
			L" (" + std::to_wstring(CameraPathDumpFrameCount) + L" frames)";
		if (bCameraPathDumpExitWhenComplete)
			PostQuitMessage(0);
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
		(RuntimePaths::RootDirectory() / L"tools\\convert_frame_sequence.py").lexically_normal();
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
	return RuntimePaths::ConfigFile(L"camera_state.cfg").wstring();
}

bool Corona::LoadCameraState()
{
	std::ifstream file{ std::filesystem::path(GetCameraStatePath()) };
	if (!file.is_open())
	{
		const std::filesystem::path legacyPath = std::filesystem::path(GetAssetFullPath(L"camera_state.cfg"));
		file.open(legacyPath);
	}
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
	const std::filesystem::path cameraStatePath = std::filesystem::path(GetCameraStatePath());
	std::filesystem::create_directories(cameraStatePath.parent_path());
	std::ofstream file{ cameraStatePath, std::ios::trunc };
	if (!file.is_open())
		return;

	file << std::fixed << std::setprecision(9);
	file << "version 2\n";
	file << "position " << m_camera.m_position.x << ' ' << m_camera.m_position.y << ' ' << m_camera.m_position.z << '\n';
	file << "rotation " << m_camera.m_yaw << ' ' << m_camera.m_pitch << '\n';
	file << "light_direction " << LightDir.x << ' ' << LightDir.y << ' ' << LightDir.z << '\n';
	file << "light_intensity " << LightIntensity << '\n';
}

std::wstring Corona::GetSceneStatePath()
{
	return RuntimePaths::ConfigFile(L"scene_state.cfg").wstring();
}

bool Corona::LoadSceneState()
{
	std::ifstream file{ std::filesystem::path(GetSceneStatePath()) };
	if (!file.is_open())
		return false;

	std::string versionTag;
	int version = 0;
	if (!(file >> versionTag >> version))
		return false;
	if (versionTag != "version" || version != 1)
		return false;

	PersistentScriptControls.clear();
	PointLights.clear();
	NextPointLightId = 1;

	UINT32 maxLoadedPointLightId = 0;
	std::string token;
	while (file >> token)
	{
		if (token == "control_number")
		{
			std::string name;
			float value = 0.0f;
			if (!(file >> name >> value))
				break;

			PersistentScriptControlValue controlValue;
			controlValue.Type = PersistentScriptControlType::Number;
			controlValue.Number = value;
			PersistentScriptControls[name] = controlValue;
		}
		else if (token == "control_bool")
		{
			std::string name;
			int value = 0;
			if (!(file >> name >> value))
				break;

			PersistentScriptControlValue controlValue;
			controlValue.Type = PersistentScriptControlType::Bool;
			controlValue.Bool = value != 0;
			PersistentScriptControls[name] = controlValue;
		}
		else if (token == "control_vec3")
		{
			std::string name;
			glm::vec3 value(0.0f);
			if (!(file >> name >> value.x >> value.y >> value.z))
				break;

			PersistentScriptControlValue controlValue;
			controlValue.Type = PersistentScriptControlType::Vec3;
			controlValue.Vec3 = value;
			PersistentScriptControls[name] = controlValue;
		}
		else if (token == "next_point_light_id")
		{
			file >> NextPointLightId;
		}
		else if (token == "point_light_count")
		{
			size_t ignoredCount = 0;
			file >> ignoredCount;
		}
		else if (token == "point_light")
		{
			PointLightState pointLight;
			int enabled = 1;
			if (!(file >>
				pointLight.Id >>
				enabled >>
				pointLight.Position.x >>
				pointLight.Position.y >>
				pointLight.Position.z >>
				pointLight.Radius >>
				pointLight.Intensity >>
				pointLight.Color.x >>
				pointLight.Color.y >>
				pointLight.Color.z))
			{
				break;
			}

			if (pointLight.Id == 0)
				pointLight.Id = ++maxLoadedPointLightId;
			pointLight.bEnabled = enabled != 0;
			pointLight.Radius = std::clamp(pointLight.Radius, 1.0f, 100000.0f);
			pointLight.Intensity = std::max(0.0f, pointLight.Intensity);
			pointLight.Color = glm::max(pointLight.Color, glm::vec3(0.0f));
			PointLights.push_back(pointLight);
			maxLoadedPointLightId = std::max(maxLoadedPointLightId, pointLight.Id);
		}
		else
		{
			std::string ignoredLine;
			std::getline(file, ignoredLine);
		}
	}

	NextPointLightId = std::max(NextPointLightId, maxLoadedPointLightId + 1);
	MarkAllPointLightsForRenderSync();
	bPersistentSceneStateDirty = false;
	return true;
}

void Corona::SaveSceneState()
{
	const std::filesystem::path sceneStatePath = std::filesystem::path(GetSceneStatePath());
	std::filesystem::create_directories(sceneStatePath.parent_path());
	std::ofstream file{ sceneStatePath, std::ios::trunc };
	if (!file.is_open())
		return;

	auto isSafeControlName = [](const std::string& name)
	{
		if (name.empty())
			return false;
		for (char c : name)
		{
			if (static_cast<unsigned char>(c) <= 0x20)
				return false;
		}
		return true;
	};

	file << std::fixed << std::setprecision(9);
	file << "version 1\n";
	for (const auto& [name, value] : PersistentScriptControls)
	{
		if (!isSafeControlName(name))
			continue;

		switch (value.Type)
		{
		case PersistentScriptControlType::Number:
			file << "control_number " << name << ' ' << value.Number << '\n';
			break;
		case PersistentScriptControlType::Bool:
			file << "control_bool " << name << ' ' << (value.Bool ? 1 : 0) << '\n';
			break;
		case PersistentScriptControlType::Vec3:
			file << "control_vec3 " << name << ' ' << value.Vec3.x << ' ' << value.Vec3.y << ' ' << value.Vec3.z << '\n';
			break;
		default:
			break;
		}
	}

	file << "next_point_light_id " << NextPointLightId << '\n';
	file << "point_light_count " << PointLights.size() << '\n';
	for (const PointLightState& pointLight : PointLights)
	{
		file <<
			"point_light " <<
			pointLight.Id << ' ' <<
			(pointLight.bEnabled ? 1 : 0) << ' ' <<
			pointLight.Position.x << ' ' <<
			pointLight.Position.y << ' ' <<
			pointLight.Position.z << ' ' <<
			pointLight.Radius << ' ' <<
			pointLight.Intensity << ' ' <<
			pointLight.Color.x << ' ' <<
			pointLight.Color.y << ' ' <<
			pointLight.Color.z << '\n';
	}

	bPersistentSceneStateDirty = false;
}

void Corona::InitRenderSyncChannels()
{
	if (bRenderSyncChannelsInitialized)
		return;

	RenderSyncChannels.push_back({ "frame_source", &Corona::CollectFrameSourceRenderSync, &Corona::ApplyFrameSourceRenderSync });
	RenderSyncChannels.push_back({ "scene_objects", &Corona::CollectSceneObjectRenderSync, &Corona::ApplySceneObjectRenderSync });
	RenderSyncChannels.push_back({ "point_lights", &Corona::CollectPointLightRenderSync, &Corona::ApplyPointLightRenderSync });
	bRenderSyncChannelsInitialized = true;
}

void Corona::MarkSceneObjectRenderDirty(SceneObjectHandle handle, UINT32 dirtyBits)
{
	if (handle == InvalidSceneObjectHandle || dirtyBits == 0)
		return;

	const auto it = std::find_if(SceneObjects.begin(), SceneObjects.end(), [handle](const SceneObject& object)
	{
		return object.Handle == handle;
	});
	if (it == SceneObjects.end())
		return;

	const bool bWasClean = it->RenderDirtyBits == 0;
	it->RenderDirtyBits |= dirtyBits;
	if (bWasClean)
		DirtySceneObjectHandles.push_back(handle);
}

void Corona::MarkSceneObjectRenderRemoved(SceneObjectHandle handle)
{
	if (handle == InvalidSceneObjectHandle)
		return;

	RemovedSceneObjectHandles.push_back(handle);
}

void Corona::MarkAllSceneObjectsForRenderSync()
{
	bSceneObjectFullSyncPending = true;
	for (SceneObject& object : SceneObjects)
		object.RenderDirtyBits = 0;
	DirtySceneObjectHandles.clear();
	RemovedSceneObjectHandles.clear();
}

void Corona::MarkPointLightRenderDirty(UINT32 id, UINT32 dirtyBits)
{
	if (id == 0 || dirtyBits == 0)
		return;

	const auto it = std::find_if(PointLights.begin(), PointLights.end(), [id](const PointLightState& pointLight)
	{
		return pointLight.Id == id;
	});
	if (it == PointLights.end())
		return;

	const bool bWasClean = it->RenderDirtyBits == 0;
	it->RenderDirtyBits |= dirtyBits;
	if (bWasClean)
		DirtyPointLightIds.push_back(id);
}

void Corona::MarkPointLightRenderRemoved(UINT32 id)
{
	if (id == 0)
		return;

	RemovedPointLightIds.push_back(id);
}

void Corona::MarkAllPointLightsForRenderSync()
{
	bPointLightFullSyncPending = true;
	for (PointLightState& pointLight : PointLights)
		pointLight.RenderDirtyBits = 0;
	DirtyPointLightIds.clear();
	RemovedPointLightIds.clear();
}

Corona::RenderFrameSourceState Corona::CaptureRenderFrameSourceState() const
{
	auto normalizeOrFallback = [](const glm::vec3& value, const glm::vec3& fallback)
	{
		const float length = glm::length(value);
		return length > 0.0001f ? value / length : fallback;
	};

	RenderFrameSourceState state;
	state.CameraPosition = m_camera.m_position;
	state.CameraLookDirection = normalizeOrFallback(m_camera.m_lookDirection, glm::vec3(0.0f, 0.0f, 1.0f));
	state.CameraUpDirection = normalizeOrFallback(m_camera.m_upDirection, glm::vec3(0.0f, 1.0f, 0.0f));
	state.Fov = Fov;
	state.NearPlane = Near;
	state.FarPlane = Far;
	state.AspectRatio = m_aspectRatio;
	state.TotalSeconds = static_cast<float>(m_timer.GetTotalSeconds());
	state.RenderingMode = RenderingMode;
	state.AntiAliasingMode = AntiAliasingMode;
	state.DLSSQualityMode = DLSSQualityMode;
	state.RayNoiseMode = RayNoiseMode;
	state.DiffuseGIMode = DiffuseGIMode;
	state.bEnableDiffuseGI = bEnableDiffuseGI;
	state.bEnableRTDiffuseGISER = bEnableRTDiffuseGISER;
	state.bEnableRTReflectionSER = bEnableRTReflectionSER;
	state.bEnableSpecularGI = bEnableSpecularGI;
	state.bEnableDirectDiffuse = bEnableDirectDiffuse;
	state.bEnableDirectSpecular = bEnableDirectSpecular;
	state.bEnableRTAO = bEnableRTAO;
	state.bEnableSkyLighting = bEnableSkyLighting;
	state.bEnableRayTracedSkyLighting = bEnableRayTracedSkyLighting;
	state.RTAOIndirectStrength = RTAOIndirectStrength;
	state.RTAOIndirectFloor = RTAOIndirectFloor;
	state.SurfaceBounceStrength = SurfaceBounceStrength;
	state.SurfaceBounceSaturation = SurfaceBounceSaturation;
	state.SkyLightingStrength = SkyLightingStrength;
	state.JitterScale = JitterScale;
	state.TAASampleCount = TAASampleCount;
	state.DLSSJitterPhaseScale = DLSSJitterPhaseScale;
	state.DLSSJitterPhaseCountOverride = DLSSJitterPhaseCountOverride;
	state.LightDir = normalizeOrFallback(LightDir, glm::vec3(0.0f, 1.0f, 0.0f));
	state.LightIntensity = LightIntensity;
	state.SkyColorTop = SkyColorTop;
	state.SkyColorBottom = SkyColorBottom;
	state.SkyIntensity = SkyIntensity;
	state.bEnablePrefilteredEnvSpecular = bEnablePrefilteredEnvSpecular;
	state.PrefilteredEnvRoughnessThreshold = PrefilteredEnvRoughnessThreshold;
	state.PrefilteredEnvRoughnessFade = PrefilteredEnvRoughnessFade;
	state.ShadowLightRadius = RTShadowViewParam.ShadowLightRadius;
	state.ShadowSampleCount = RTShadowViewParam.ShadowSampleCount;
	state.RTAORadius = RTAOViewParam.Radius;
	state.RTAOPower = RTAOViewParam.Power;
	state.RTAONormalBias = RTAOViewParam.NormalBias;
	state.RTAOSampleCount = RTAOViewParam.SampleCount;
	state.SkyLightingRayLength = RTSkyLightingViewParam.RayLength;
	state.SkyLightingNormalBias = RTSkyLightingViewParam.NormalBias;
	state.SkyLightingSampleCount = RTSkyLightingViewParam.SampleCount;
	state.SkyLightingUpBias = RTSkyLightingViewParam.SkyUpBias;
	state.SkyLightingDirectionPower = RTSkyLightingViewParam.SkyDirectionPower;
	state.SkyLightingMinWorldY = RTSkyLightingViewParam.SkyMinWorldY;
	state.SkyLightingMaxSampleAttempts = RTSkyLightingViewParam.SkyMaxSampleAttempts;
	state.ScreenProbeSpacing = ScreenProbeGICB.ProbeSpacing;
	state.ScreenProbeGatherRadius = ScreenProbeGICB.GatherRadius;
	state.ScreenProbeRaysPerProbe = RTScreenProbeGIViewParam.RaysPerProbe;
	state.ScreenProbeSHCoefficientCount = ScreenProbeGICB.SHCoefficientCount;
	state.ScreenProbeRawBlend = ScreenProbeGICB.RawBlend;
	state.ScreenProbeMinResolveWeight = ScreenProbeGICB.MinResolveWeight;
	state.ScreenProbeDepthWeight = ScreenProbeGICB.ProbeDepthWeight;
	state.ScreenProbeNormalWeight = ScreenProbeGICB.ProbeNormalWeight;
	state.ScreenProbeResolveDepthWeight = ScreenProbeGICB.ResolveDepthWeight;
	state.ScreenProbeResolveNormalWeight = ScreenProbeGICB.ResolveNormalWeight;
	state.ScreenProbeTemporalAlpha = ScreenProbeGICB.TemporalAlpha;
	state.ScreenProbeHistoryDepthWeight = ScreenProbeGICB.HistoryDepthWeight;
	state.ScreenProbeHistoryNormalWeight = ScreenProbeGICB.HistoryNormalWeight;
	state.ScreenProbeEdgeDepthWeight = ScreenProbeGICB.EdgeDepthWeight;
	state.ScreenProbeEdgeNormalWeight = ScreenProbeGICB.EdgeNormalWeight;
	state.ScreenProbeEdgeSampleCount = ScreenProbeGICB.EdgeSampleCount;
	state.SpatialHashCellSize = SpatialHashGICB.CellSize;
	state.SpatialHashTemporalAlpha = SpatialHashGICB.TemporalAlpha;
	state.SpatialHashSmoothingStrength = SpatialHashGICB.SmoothingStrength;
	state.SpatialHashInterpolationStrength = SpatialHashGICB.InterpolationStrength;
	state.SpatialHashRaysPerCell = RTSpatialHashGIViewParam.RaysPerCell;
	state.SpatialHashMaxBounces = RTSpatialHashGIViewParam.MaxBounces;
	state.PathTracingDirectLightSampleCount = PathTracingViewParam.DirectLightSampleCount;
	state.PathTracingMaxBounces = PathTracingViewParam.MaxBounces;
	state.PathTracingSamplesPerPixel = PathTracingViewParam.SamplesPerPixel;
	state.PathTracingDebugMode = PathTracingViewParam.DebugMode;
	return state;
}

void Corona::ApplyRenderFrameSourceState(const RenderFrameSourceState& state)
{
	const ERenderingMode previousRenderingMode = RenderingMode;
	RenderingMode = state.RenderingMode;
	AntiAliasingMode = state.AntiAliasingMode;
	DLSSQualityMode = state.DLSSQualityMode;
	RayNoiseMode = state.RayNoiseMode;
	DiffuseGIMode = state.DiffuseGIMode;
	bEnableDiffuseGI = state.bEnableDiffuseGI;
	bEnableRTDiffuseGISER = state.bEnableRTDiffuseGISER;
	bEnableRTReflectionSER = state.bEnableRTReflectionSER;
	bEnableSpecularGI = state.bEnableSpecularGI;
	bEnableDirectDiffuse = state.bEnableDirectDiffuse;
	bEnableDirectSpecular = state.bEnableDirectSpecular;
	bEnableRTAO = state.bEnableRTAO;
	bEnableSkyLighting = state.bEnableSkyLighting;
	bEnableRayTracedSkyLighting = state.bEnableRayTracedSkyLighting;
	RTAOIndirectStrength = state.RTAOIndirectStrength;
	RTAOIndirectFloor = state.RTAOIndirectFloor;
	SurfaceBounceStrength = state.SurfaceBounceStrength;
	SurfaceBounceSaturation = state.SurfaceBounceSaturation;
	SkyLightingStrength = state.SkyLightingStrength;
	JitterScale = state.JitterScale;
	TAASampleCount = state.TAASampleCount;
	DLSSJitterPhaseScale = state.DLSSJitterPhaseScale;
	DLSSJitterPhaseCountOverride = state.DLSSJitterPhaseCountOverride;
	Fov = state.Fov;
	Near = state.NearPlane;
	Far = state.FarPlane;
	m_aspectRatio = state.AspectRatio;
	LightDir = glm::length(state.LightDir) > 0.0001f ? glm::normalize(state.LightDir) : glm::vec3(0.0f, 1.0f, 0.0f);
	LightIntensity = state.LightIntensity;
	SkyColorTop = state.SkyColorTop;
	SkyColorBottom = state.SkyColorBottom;
	SkyIntensity = state.SkyIntensity;
	bEnablePrefilteredEnvSpecular = state.bEnablePrefilteredEnvSpecular;
	PrefilteredEnvRoughnessThreshold = state.PrefilteredEnvRoughnessThreshold;
	PrefilteredEnvRoughnessFade = state.PrefilteredEnvRoughnessFade;
	RTShadowViewParam.ShadowLightRadius = state.ShadowLightRadius;
	RTShadowViewParam.ShadowSampleCount = state.ShadowSampleCount;
	RTAOViewParam.Radius = state.RTAORadius;
	RTAOViewParam.Power = state.RTAOPower;
	RTAOViewParam.NormalBias = state.RTAONormalBias;
	RTAOViewParam.SampleCount = state.RTAOSampleCount;
	RTSkyLightingViewParam.RayLength = state.SkyLightingRayLength;
	RTSkyLightingViewParam.NormalBias = state.SkyLightingNormalBias;
	RTSkyLightingViewParam.SampleCount = state.SkyLightingSampleCount;
	RTSkyLightingViewParam.SkyUpBias = state.SkyLightingUpBias;
	RTSkyLightingViewParam.SkyDirectionPower = state.SkyLightingDirectionPower;
	RTSkyLightingViewParam.SkyMinWorldY = state.SkyLightingMinWorldY;
	RTSkyLightingViewParam.SkyMaxSampleAttempts = state.SkyLightingMaxSampleAttempts;
	ScreenProbeGICB.ProbeSpacing = state.ScreenProbeSpacing;
	ScreenProbeGICB.GatherRadius = state.ScreenProbeGatherRadius;
	RTScreenProbeGIViewParam.RaysPerProbe = state.ScreenProbeRaysPerProbe;
	ScreenProbeGICB.SHCoefficientCount = state.ScreenProbeSHCoefficientCount;
	ScreenProbeGICB.RawBlend = state.ScreenProbeRawBlend;
	ScreenProbeGICB.MinResolveWeight = state.ScreenProbeMinResolveWeight;
	ScreenProbeGICB.ProbeDepthWeight = state.ScreenProbeDepthWeight;
	ScreenProbeGICB.ProbeNormalWeight = state.ScreenProbeNormalWeight;
	ScreenProbeGICB.ResolveDepthWeight = state.ScreenProbeResolveDepthWeight;
	ScreenProbeGICB.ResolveNormalWeight = state.ScreenProbeResolveNormalWeight;
	ScreenProbeGICB.TemporalAlpha = state.ScreenProbeTemporalAlpha;
	ScreenProbeGICB.HistoryDepthWeight = state.ScreenProbeHistoryDepthWeight;
	ScreenProbeGICB.HistoryNormalWeight = state.ScreenProbeHistoryNormalWeight;
	ScreenProbeGICB.EdgeDepthWeight = state.ScreenProbeEdgeDepthWeight;
	ScreenProbeGICB.EdgeNormalWeight = state.ScreenProbeEdgeNormalWeight;
	ScreenProbeGICB.EdgeSampleCount = state.ScreenProbeEdgeSampleCount;
	SpatialHashGICB.CellSize = state.SpatialHashCellSize;
	SpatialHashGICB.TemporalAlpha = state.SpatialHashTemporalAlpha;
	SpatialHashGICB.SmoothingStrength = state.SpatialHashSmoothingStrength;
	SpatialHashGICB.InterpolationStrength = state.SpatialHashInterpolationStrength;
	RTSpatialHashGIViewParam.RaysPerCell = state.SpatialHashRaysPerCell;
	RTSpatialHashGIViewParam.MaxBounces = state.SpatialHashMaxBounces;
	PathTracingViewParam.DirectLightSampleCount = state.PathTracingDirectLightSampleCount;
	PathTracingViewParam.MaxBounces = state.PathTracingMaxBounces;
	PathTracingViewParam.SamplesPerPixel = state.PathTracingSamplesPerPixel;
	PathTracingViewParam.DebugMode = state.PathTracingDebugMode;
	if (RenderingMode != previousRenderingMode)
		MarkRayTracingSceneDirty();
}

void Corona::CollectFrameSourceRenderSync(RenderFrameDelta& delta)
{
	if (!bSplitGameRenderThreads)
		return;

	delta.bHasFrameSourceState = true;
	delta.FrameSourceState = CaptureRenderFrameSourceState();
}

void Corona::ApplyFrameSourceRenderSync(const RenderFrameDelta& delta)
{
	if (!delta.bHasFrameSourceState)
		return;

	const RenderFrameSourceState& newState = delta.FrameSourceState;
	const RenderFrameSourceState& oldState = RenderWorld.FrameSourceState;
	auto floatChanged = [](float a, float b, float epsilon = 0.0001f)
	{
		return std::abs(a - b) > epsilon;
	};

	const bool bHadFrameSourceState = RenderWorld.bHasFrameSourceState;
	const bool bModeOrAAModeChanged =
		bHadFrameSourceState &&
		(oldState.RenderingMode != newState.RenderingMode ||
		 oldState.AntiAliasingMode != newState.AntiAliasingMode ||
		 oldState.DLSSQualityMode != newState.DLSSQualityMode);
	const bool bIndirectSettingsChanged =
		bHadFrameSourceState &&
		(oldState.DiffuseGIMode != newState.DiffuseGIMode ||
		 oldState.RayNoiseMode != newState.RayNoiseMode ||
		 oldState.bEnableDiffuseGI != newState.bEnableDiffuseGI ||
		 oldState.bEnableRTDiffuseGISER != newState.bEnableRTDiffuseGISER ||
		 oldState.bEnableRTReflectionSER != newState.bEnableRTReflectionSER ||
		 oldState.bEnableSpecularGI != newState.bEnableSpecularGI ||
		 oldState.bEnableRTAO != newState.bEnableRTAO ||
		 oldState.bEnableSkyLighting != newState.bEnableSkyLighting ||
		 oldState.bEnableRayTracedSkyLighting != newState.bEnableRayTracedSkyLighting ||
		 oldState.bEnablePrefilteredEnvSpecular != newState.bEnablePrefilteredEnvSpecular ||
		 oldState.ShadowSampleCount != newState.ShadowSampleCount ||
		 oldState.RTAOSampleCount != newState.RTAOSampleCount ||
		 oldState.SkyLightingSampleCount != newState.SkyLightingSampleCount ||
		 oldState.ScreenProbeSpacing != newState.ScreenProbeSpacing ||
		 oldState.ScreenProbeGatherRadius != newState.ScreenProbeGatherRadius ||
		 oldState.ScreenProbeRaysPerProbe != newState.ScreenProbeRaysPerProbe ||
		 oldState.ScreenProbeSHCoefficientCount != newState.ScreenProbeSHCoefficientCount ||
		 oldState.ScreenProbeEdgeSampleCount != newState.ScreenProbeEdgeSampleCount ||
		 oldState.SpatialHashRaysPerCell != newState.SpatialHashRaysPerCell ||
		 oldState.SpatialHashMaxBounces != newState.SpatialHashMaxBounces ||
		 floatChanged(oldState.RTAOIndirectStrength, newState.RTAOIndirectStrength) ||
		 floatChanged(oldState.RTAOIndirectFloor, newState.RTAOIndirectFloor) ||
		 floatChanged(oldState.SurfaceBounceStrength, newState.SurfaceBounceStrength) ||
		 floatChanged(oldState.SurfaceBounceSaturation, newState.SurfaceBounceSaturation) ||
		 floatChanged(oldState.SkyLightingStrength, newState.SkyLightingStrength) ||
		 floatChanged(oldState.SkyIntensity, newState.SkyIntensity) ||
		 floatChanged(oldState.PrefilteredEnvRoughnessThreshold, newState.PrefilteredEnvRoughnessThreshold) ||
		 floatChanged(oldState.PrefilteredEnvRoughnessFade, newState.PrefilteredEnvRoughnessFade) ||
		 floatChanged(oldState.ShadowLightRadius, newState.ShadowLightRadius) ||
		 floatChanged(oldState.RTAORadius, newState.RTAORadius) ||
		 floatChanged(oldState.RTAOPower, newState.RTAOPower) ||
		 floatChanged(oldState.RTAONormalBias, newState.RTAONormalBias) ||
		 floatChanged(oldState.SkyLightingRayLength, newState.SkyLightingRayLength) ||
		 floatChanged(oldState.SkyLightingNormalBias, newState.SkyLightingNormalBias) ||
		 floatChanged(oldState.SkyLightingUpBias, newState.SkyLightingUpBias) ||
		 floatChanged(oldState.SkyLightingDirectionPower, newState.SkyLightingDirectionPower) ||
		 floatChanged(oldState.SkyLightingMinWorldY, newState.SkyLightingMinWorldY) ||
		 floatChanged(oldState.ScreenProbeRawBlend, newState.ScreenProbeRawBlend) ||
		 floatChanged(oldState.ScreenProbeResolveDepthWeight, newState.ScreenProbeResolveDepthWeight) ||
		 floatChanged(oldState.ScreenProbeResolveNormalWeight, newState.ScreenProbeResolveNormalWeight) ||
		 floatChanged(oldState.ScreenProbeEdgeDepthWeight, newState.ScreenProbeEdgeDepthWeight) ||
		 floatChanged(oldState.ScreenProbeEdgeNormalWeight, newState.ScreenProbeEdgeNormalWeight) ||
		 floatChanged(oldState.SpatialHashCellSize, newState.SpatialHashCellSize) ||
		 floatChanged(oldState.SpatialHashTemporalAlpha, newState.SpatialHashTemporalAlpha) ||
		 floatChanged(oldState.SpatialHashSmoothingStrength, newState.SpatialHashSmoothingStrength) ||
		 floatChanged(oldState.SpatialHashInterpolationStrength, newState.SpatialHashInterpolationStrength));

	if (bModeOrAAModeChanged)
	{
		const bool bForceResourceReload =
			oldState.RenderingMode != newState.RenderingMode ||
			oldState.DLSSQualityMode != newState.DLSSQualityMode ||
			IsDLSSMode(oldState.AntiAliasingMode) ||
			IsDLSSMode(newState.AntiAliasingMode);
		ResetAllAccumulationState(bForceResourceReload);
		if (oldState.RenderingMode != newState.RenderingMode)
			MarkRayTracingSceneDirty();
		AppendCpuRuntimeTrace(
			L"[ApplyFrameSourceRenderSync] mode/aa changed renderMode " +
			std::to_wstring(static_cast<int>(oldState.RenderingMode)) +
			L"->" + std::to_wstring(static_cast<int>(newState.RenderingMode)) +
			L", aa " + std::wstring(GetAntiAliasingModeName(oldState.AntiAliasingMode)) +
			L"->" + std::wstring(GetAntiAliasingModeName(newState.AntiAliasingMode)) +
			L", forceReload=" + std::to_wstring(bForceResourceReload ? 1 : 0));
	}

	RenderWorld.FrameSourceState = delta.FrameSourceState;
	RenderWorld.bHasFrameSourceState = true;

	if (bIndirectSettingsChanged)
	{
		FrameCounter = 0;
		PathTracingAccumulatedFrames = 0;
		IndirectAccumulatedFrames = 0;
		bTemporalDenoiserHistoryValid = false;
		bScreenProbeGIAtlasHistoryValid = false;
		bScreenProbeGIHistoryValid = false;
		bSpatialHashGIHistoryValid = false;
		bPendingTemporalHistoryClear = true;
		bResetTemporalStateNextUpdate = true;
		bUseLightingBufferFallbackForToneMap = true;
		bDLSSRROutputValidThisFrame = false;
		PrevPathTracingViewMat = glm::mat4x4(0.0f);
		PrevPathTracingLightDir = glm::vec3(0.0f);
		PrevPathTracingLightIntensity = 0.0f;
#if WITH_STREAMLINE
		bDLSSResetNeeded = true;
#endif
	}
}

void Corona::CollectSceneObjectRenderSync(RenderFrameDelta& delta)
{
	if (bSceneObjectFullSyncPending)
	{
		delta.bFullSceneObjectSync = true;
		delta.SceneObjectDeltas.reserve(delta.SceneObjectDeltas.size() + SceneObjects.size());
		for (SceneObject& object : SceneObjects)
		{
			RenderSceneObjectDelta objectDelta;
			objectDelta.Op = ERenderDeltaOp::Upsert;
			objectDelta.DirtyBits = kSceneObjectDirtyAll;
			objectDelta.Object = object;
			objectDelta.Object.RenderDirtyBits = 0;
			objectDelta.Handle = object.Handle;
			delta.SceneObjectDeltas.push_back(std::move(objectDelta));
			object.RenderDirtyBits = 0;
		}
		bSceneObjectFullSyncPending = false;
		DirtySceneObjectHandles.clear();
		RemovedSceneObjectHandles.clear();
		return;
	}

	for (SceneObjectHandle handle : RemovedSceneObjectHandles)
	{
		RenderSceneObjectDelta objectDelta;
		objectDelta.Op = ERenderDeltaOp::Remove;
		objectDelta.Handle = handle;
		delta.SceneObjectDeltas.push_back(std::move(objectDelta));
	}
	RemovedSceneObjectHandles.clear();

	for (SceneObjectHandle handle : DirtySceneObjectHandles)
	{
		const auto it = std::find_if(SceneObjects.begin(), SceneObjects.end(), [handle](const SceneObject& object)
		{
			return object.Handle == handle;
		});
		if (it == SceneObjects.end() || it->RenderDirtyBits == 0)
			continue;

		RenderSceneObjectDelta objectDelta;
		objectDelta.Op = ERenderDeltaOp::Upsert;
		objectDelta.DirtyBits = it->RenderDirtyBits;
		objectDelta.Object = *it;
		objectDelta.Object.RenderDirtyBits = 0;
		objectDelta.Handle = it->Handle;
		delta.SceneObjectDeltas.push_back(std::move(objectDelta));
		it->RenderDirtyBits = 0;
	}
	DirtySceneObjectHandles.clear();
}

void Corona::ApplySceneObjectRenderSync(const RenderFrameDelta& delta)
{
	if (delta.bFullSceneObjectSync)
	{
		RenderWorld.SceneObjects.clear();
		bRayTracingSceneDirty = true;
		bRayTracingTransformDirty = false;
		PrevPathTracingViewMat = glm::mat4x4(0.0f);
	}

	for (const RenderSceneObjectDelta& objectDelta : delta.SceneObjectDeltas)
	{
		const SceneObjectHandle handle =
			objectDelta.Handle != InvalidSceneObjectHandle ?
			objectDelta.Handle :
			objectDelta.Object.Handle;

		if (objectDelta.Op == ERenderDeltaOp::Remove)
		{
			const auto it = std::find_if(RenderWorld.SceneObjects.begin(), RenderWorld.SceneObjects.end(), [handle](const SceneObject& object)
			{
				return object.Handle == handle;
			});
			if (it != RenderWorld.SceneObjects.end())
			{
				RenderWorld.SceneObjects.erase(it);
				MarkRayTracingSceneDirty();
			}
			continue;
		}

		SceneObject object = objectDelta.Object;
		object.RenderDirtyBits = 0;
		auto it = std::find_if(RenderWorld.SceneObjects.begin(), RenderWorld.SceneObjects.end(), [handle](const SceneObject& candidate)
		{
			return candidate.Handle == handle;
		});

		if (it == RenderWorld.SceneObjects.end())
		{
			RenderWorld.SceneObjects.push_back(std::move(object));
			MarkRayTracingSceneDirty();
			continue;
		}

		const bool bRayTracingSceneRelevant =
			(objectDelta.DirtyBits & (kSceneObjectDirtyScene | kSceneObjectDirtyVisibility | kSceneObjectDirtyRayTracing)) != 0;
		const bool bRayTracingTransformRelevant =
			(objectDelta.DirtyBits & kSceneObjectDirtyTransform) != 0;

		*it = std::move(object);
		if (bRayTracingSceneRelevant)
			MarkRayTracingSceneDirty();
		else if (bRayTracingTransformRelevant && ShouldIncludeSceneObjectInRayTracingAS(*it))
			MarkRayTracingTransformsDirty();
	}
}

void Corona::CollectPointLightRenderSync(RenderFrameDelta& delta)
{
	if (bPointLightFullSyncPending)
	{
		delta.bFullPointLightSync = true;
		delta.PointLightDeltas.reserve(delta.PointLightDeltas.size() + PointLights.size());
		for (PointLightState& pointLight : PointLights)
		{
			RenderPointLightDelta lightDelta;
			lightDelta.Op = ERenderDeltaOp::Upsert;
			lightDelta.DirtyBits = kPointLightDirtyAll;
			lightDelta.Light = pointLight;
			lightDelta.Light.RenderDirtyBits = 0;
			lightDelta.Id = pointLight.Id;
			delta.PointLightDeltas.push_back(std::move(lightDelta));
			pointLight.RenderDirtyBits = 0;
		}
		bPointLightFullSyncPending = false;
		DirtyPointLightIds.clear();
		RemovedPointLightIds.clear();
		return;
	}

	for (UINT32 id : RemovedPointLightIds)
	{
		RenderPointLightDelta lightDelta;
		lightDelta.Op = ERenderDeltaOp::Remove;
		lightDelta.Id = id;
		delta.PointLightDeltas.push_back(std::move(lightDelta));
	}
	RemovedPointLightIds.clear();

	for (UINT32 id : DirtyPointLightIds)
	{
		const auto it = std::find_if(PointLights.begin(), PointLights.end(), [id](const PointLightState& pointLight)
		{
			return pointLight.Id == id;
		});
		if (it == PointLights.end() || it->RenderDirtyBits == 0)
			continue;

		RenderPointLightDelta lightDelta;
		lightDelta.Op = ERenderDeltaOp::Upsert;
		lightDelta.DirtyBits = it->RenderDirtyBits;
		lightDelta.Light = *it;
		lightDelta.Light.RenderDirtyBits = 0;
		lightDelta.Id = it->Id;
		delta.PointLightDeltas.push_back(std::move(lightDelta));
		it->RenderDirtyBits = 0;
	}
	DirtyPointLightIds.clear();
}

void Corona::ApplyPointLightRenderSync(const RenderFrameDelta& delta)
{
	if (delta.bFullPointLightSync)
		RenderWorld.PointLights.clear();

	bool bChanged = delta.bFullPointLightSync;
	for (const RenderPointLightDelta& lightDelta : delta.PointLightDeltas)
	{
		const UINT32 id = lightDelta.Id != 0 ? lightDelta.Id : lightDelta.Light.Id;
		if (lightDelta.Op == ERenderDeltaOp::Remove)
		{
			const auto it = std::find_if(RenderWorld.PointLights.begin(), RenderWorld.PointLights.end(), [id](const PointLightState& pointLight)
			{
				return pointLight.Id == id;
			});
			if (it != RenderWorld.PointLights.end())
			{
				RenderWorld.PointLights.erase(it);
				bChanged = true;
			}
			continue;
		}

		PointLightState light = lightDelta.Light;
		light.RenderDirtyBits = 0;
		auto it = std::find_if(RenderWorld.PointLights.begin(), RenderWorld.PointLights.end(), [id](const PointLightState& pointLight)
		{
			return pointLight.Id == id;
		});
		if (it == RenderWorld.PointLights.end())
			RenderWorld.PointLights.push_back(std::move(light));
		else
			*it = std::move(light);
		bChanged = true;
	}

	if (bChanged)
	{
		FrameCounter = 0;
		IndirectAccumulatedFrames = 0;
		bTemporalDenoiserHistoryValid = false;
		bScreenProbeGIAtlasHistoryValid = false;
		bScreenProbeGIHistoryValid = false;
		bSpatialHashGIHistoryValid = false;
		bPendingTemporalHistoryClear = true;
		bResetTemporalStateNextUpdate = true;
		PrevPathTracingViewMat = glm::mat4x4(0.0f);
		PrevPathTracingLightDir = glm::vec3(0.0f);
		PrevPathTracingLightIntensity = 0.0f;
	}
}

void Corona::CollectRenderFrameDeltas()
{
	InitRenderSyncChannels();

	RenderFrameDelta delta;
	delta.FrameId = NextRenderFrameDeltaId++;
	for (const RenderSyncChannel& channel : RenderSyncChannels)
	{
		if (channel.Collect)
			(this->*channel.Collect)(delta);
	}

	if (delta.SceneObjectDeltas.empty() &&
		delta.PointLightDeltas.empty() &&
		!delta.bHasFrameSourceState &&
		!delta.bFullSceneObjectSync &&
		!delta.bFullPointLightSync)
	{
		return;
	}

	PublishRenderFrameDelta(std::move(delta));
}

void Corona::PublishRenderFrameDelta(RenderFrameDelta&& delta)
{
	std::lock_guard<std::mutex> lock(RenderFrameDeltaMutex);
	PendingRenderFrameDeltas.push_back(std::move(delta));
}

void Corona::ApplyPendingRenderFrameDeltas()
{
	InitRenderSyncChannels();

	std::deque<RenderFrameDelta> pendingDeltas;
	{
		std::lock_guard<std::mutex> lock(RenderFrameDeltaMutex);
		pendingDeltas.swap(PendingRenderFrameDeltas);
	}

	for (const RenderFrameDelta& delta : pendingDeltas)
	{
		for (const RenderSyncChannel& channel : RenderSyncChannels)
		{
			if (channel.Apply)
				(this->*channel.Apply)(delta);
		}
	}
}

void Corona::ApplyRenderPointLightsToFrameParams()
{
	PathTracingViewParam.PointLightCount = 0;
	for (const PointLightState& pointLight : RenderWorld.PointLights)
	{
		if (!pointLight.bEnabled || PathTracingViewParam.PointLightCount >= MaxPointLights)
			continue;

		const UINT32 pointLightIndex = PathTracingViewParam.PointLightCount++;
		PathTracingViewParam.PointLights[pointLightIndex].PositionAndRadius =
			glm::vec4(pointLight.Position, std::max(pointLight.Radius, 0.01f));
		PathTracingViewParam.PointLights[pointLightIndex].ColorAndIntensity =
			glm::vec4(glm::max(pointLight.Color, glm::vec3(0.0f)), std::max(pointLight.Intensity, 0.0f));
	}
}


void Corona::PumpStartupWindowMessages()
{
	MSG msg = {};
	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
		{
			PostQuitMessage(static_cast<int>(msg.wParam));
			break;
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

void Corona::DrawStartupLoadingScreen()
{
	if (!bStartupLoadingScreenActive || !renderBackend || !bImguiInitialized)
		return;

	try
	{
		renderBackend->BeginFrame();

		Texture* backbuffer = renderBackend->GetCurrentWindowRenderTarget();
		if (renderBackend->GetAPI() == ERenderBackendAPI::D3D12 && !backbuffer)
		{
			renderBackend->EndFrame();
			return;
		}

		const float clearColor[4] = { 0.025f, 0.032f, 0.045f, 1.0f };
		if (renderBackend->GetAPI() == ERenderBackendAPI::D3D12)
			renderBackend->PrepareWindowRenderTarget(backbuffer);
		renderBackend->ClearRenderTarget(backbuffer, clearColor);
		renderBackend->SetRenderTarget(backbuffer);
		renderBackend->SetViewportAndScissor(m_width, m_height);

		renderBackend->NewImGuiFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		ImGuiIO& io = ImGui::GetIO();
		ImDrawList* drawList = ImGui::GetBackgroundDrawList();
		const ImVec2 displaySize = io.DisplaySize;
		const float width = std::max(displaySize.x, 1.0f);
		const float height = std::max(displaySize.y, 1.0f);
		const ImVec2 center(width * 0.5f, height * 0.5f - 50.0f);
		const float progress = std::clamp(StartupLoadingProgress, 0.0f, 1.0f);

		drawList->AddRectFilled(ImVec2(0.0f, 0.0f), displaySize, IM_COL32(6, 9, 13, 255));
		drawList->AddCircle(center, 58.0f, IM_COL32(25, 53, 78, 210), 96, 13.0f);
		drawList->AddCircle(center, 49.0f, IM_COL32(79, 197, 255, 245), 96, 4.0f);
		drawList->PathArcTo(center, 38.0f, 0.15f, 4.95f, 72);
		drawList->PathStroke(IM_COL32(226, 246, 255, 235), 0, 5.0f);

		ImFont* font = ImGui::GetFont();
		auto addCenteredText = [&](const char* text, float fontSize, float y, ImU32 color)
		{
			const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
			drawList->AddText(font, fontSize, ImVec2(width * 0.5f - textSize.x * 0.5f, y), color, text);
		};

		addCenteredText("C", 46.0f, center.y - 29.0f, IM_COL32(229, 247, 255, 255));
		addCenteredText("CORONA", 34.0f, center.y + 84.0f, IM_COL32(238, 246, 250, 255));

		const std::string status = WideToUtf8(StartupLoadingStatus.empty() ? L"Starting renderer" : StartupLoadingStatus);
		addCenteredText(status.c_str(), 18.0f, center.y + 130.0f, IM_COL32(160, 180, 191, 255));

		const float barWidth = std::min(width - 120.0f, 560.0f);
		const float barHeight = 10.0f;
		const ImVec2 barMin(width * 0.5f - barWidth * 0.5f, center.y + 174.0f);
		const ImVec2 barMax(barMin.x + barWidth, barMin.y + barHeight);
		drawList->AddRectFilled(barMin, barMax, IM_COL32(27, 37, 48, 255), 5.0f);
		drawList->AddRectFilled(barMin, ImVec2(barMin.x + barWidth * progress, barMax.y), IM_COL32(75, 196, 255, 255), 5.0f);
		drawList->AddRect(barMin, barMax, IM_COL32(74, 95, 110, 255), 5.0f);

		const std::string percentText = std::to_string(static_cast<int>(std::round(progress * 100.0f))) + "%";
		addCenteredText(percentText.c_str(), 14.0f, barMax.y + 16.0f, IM_COL32(123, 146, 158, 255));

		ImGui::Render();
		renderBackend->RenderImGuiDrawData(ImGui::GetDrawData());
		renderBackend->FinalizeWindowRenderTarget(backbuffer);
		renderBackend->ExecuteCurrentCommandList();
		renderBackend->EndFrame();
	}
	catch (const std::exception& e)
	{
		AppendCpuRuntimeTrace(L"[StartupLoading] render failed: " + AnsiToWString(e.what()));
	}
}

void Corona::UpdateStartupLoadingProgress(float progress, const std::wstring& status)
{
	const auto now = CpuClock::now();
	const float previousProgress = StartupLoadingProgress;
	if (!bStartupLoadingTimingStarted)
	{
		bStartupLoadingTimingStarted = true;
		StartupLoadingTimingStart = now;
		StartupLoadingTimingLast = now;
		StartupLoadingTimingLastStatus = status;
		AppendCpuRuntimeTrace(
			L"[StartupTiming] begin progress=" + std::to_wstring(static_cast<int>(std::round(progress * 100.0f))) +
			L"% status=\"" + status + L"\"");
	}
	else
	{
		const double stepMs = ElapsedMilliseconds(StartupLoadingTimingLast, now);
		const double totalMs = ElapsedMilliseconds(StartupLoadingTimingStart, now);
		AppendCpuRuntimeTrace(
			L"[StartupTiming] step=\"" + StartupLoadingTimingLastStatus +
			L"\", stepMs=" + FormatMilliseconds(stepMs) +
			L", totalMs=" + FormatMilliseconds(totalMs) +
			L", progress=" + std::to_wstring(static_cast<int>(std::round(previousProgress * 100.0f))) +
			L"->" + std::to_wstring(static_cast<int>(std::round(progress * 100.0f))) +
			L"% next=\"" + status + L"\"");
		StartupLoadingTimingLast = now;
		StartupLoadingTimingLastStatus = status;
	}

	bStartupLoadingScreenActive = true;
	StartupLoadingProgress = std::clamp(progress, StartupLoadingProgress, 1.0f);
	StartupLoadingStatus = status;
	PumpStartupWindowMessages();
	DrawStartupLoadingScreen();

	if (status == L"Ready")
	{
		AppendCpuRuntimeTrace(
			L"[StartupTiming] complete totalMs=" +
			FormatMilliseconds(ElapsedMilliseconds(StartupLoadingTimingStart, CpuClock::now())));
	}
}

void Corona::OnInit()
{
	//_CrtSetBreakAlloc(4207117);

	CoInitialize(NULL);
	AppendCpuRuntimeTrace(L"[OnInit] begin");
	UpdateStartupLoadingProgress(0.02f, L"Starting Corona");

	g_TS.Initialize(8);
	AppendCpuRuntimeTrace(L"[OnInit] after g_TS.Initialize");
	UpdateStartupLoadingProgress(0.04f, L"Initializing task system");

	m_camera.Init({ 458, 781, 185 });
	m_camera.SetMoveSpeed(200);
	LoadCameraState();
	LoadSceneState();
	AppendCpuRuntimeTrace(L"[OnInit] after camera init");
	UpdateStartupLoadingProgress(0.06f, L"Restoring camera and scene state");

	RenderWidth = m_width;
	RenderHeight = m_height;
	AppendCpuRuntimeTrace(L"[OnInit] after render size init");
	UpdateStartupLoadingProgress(0.08f, L"Preparing render size");
#if WITH_STREAMLINE
	const bool bStartupRequestsVulkan =
		bCommandLineRenderBackendOverrideSet &&
		CommandLineRenderBackendAPI == ERenderBackendAPI::Vulkan;
	if (!bStartupRequestsVulkan)
	{
		UpdateStartupLoadingProgress(0.10f, L"Initializing Streamline");
		InitStreamline();
		AppendCpuRuntimeTrace(L"[OnInit] after InitStreamline");
	}
	else
	{
		AppendCpuRuntimeTrace(L"[OnInit] skip InitStreamline for Vulkan startup");
	}
#endif
	UpdateStartupLoadingProgress(0.12f, L"Creating render backend");
	LoadPipeline();
	AppendCpuRuntimeTrace(L"[OnInit] after LoadPipeline");
	UpdateStartupLoadingProgress(0.16f, L"Choosing startup rendering mode");
	PromptStartupModeSelection();
	AppendCpuRuntimeTrace(
		L"[OnInit] after PromptStartupModeSelection backend=" + std::to_wstring(static_cast<int>(StartupRenderBackendAPI)) +
		L", renderMode=" + std::to_wstring(static_cast<int>(StartupRenderingMode)) +
		L", aa=" + std::wstring(GetAntiAliasingModeName(AntiAliasingMode)) +
		L", dlssAvailable=" + std::to_wstring(bDLSSAvailable ? 1 : 0) +
		L", dlssRRAvailable=" + std::to_wstring(bDLSSRRAvailable ? 1 : 0) +
		L", autoDump=" + std::to_wstring(bAutoAADumpEnabled ? 1 : 0));
	const bool bStartupLoadingVulkanHybrid =
		renderBackend &&
		renderBackend->GetAPI() == ERenderBackendAPI::Vulkan &&
		StartupRenderingMode == ERenderingMode::HYBRID;
	const bool bStartupLoadingCanUseImgui =
		!bCommandLineDisableImgui &&
		!bAutoAADumpEnabled &&
		(!bStartupLoadingVulkanHybrid || renderBackend->GetMaxSupportedHybridStage() >= 7u);
	if (bStartupLoadingCanUseImgui && !bImguiInitialized)
	{
		InitImgui();
		bImguiInitialized = true;
	}
	UpdateStartupLoadingProgress(0.18f, L"Render backend ready");
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
			AppendCpuRuntimeTrace(L"[OnInit] applying default hybrid camera");
			ApplyHybridDefaultCamera();
		}
	}
	AppendCpuRuntimeTrace(L"[OnInit] before RefreshUpscaleSettings");
	UpdateStartupLoadingProgress(0.18f, L"Configuring resolution and upscaling");
	RefreshUpscaleSettings(false);
	AppendCpuRuntimeTrace(L"[OnInit] after RefreshUpscaleSettings");
	AppendCpuRuntimeTrace(L"[OnInit] before LoadAssets");
	UpdateStartupLoadingProgress(0.20f, L"Preparing renderer assets");
	LoadAssets();
	AppendCpuRuntimeTrace(L"[OnInit] after LoadAssets");
	if (bStartupSponzaFlyMode)
	{
		ApplySponzaFlyCamera();
		AppendCpuRuntimeTrace(L"[OnInit] after ApplySponzaFlyCamera");
	}
	UpdateStartupLoadingProgress(0.92f, L"Initializing CPU physics");
	InitCpuPhysics();
	UpdateStartupLoadingProgress(0.94f, L"Initializing Luau scripting");
	InitLuauScripting();
	UpdateStartupLoadingProgress(0.95f, L"Running startup scripts");
	RunStartupLuauScript();
	AppendCpuRuntimeTrace(L"[OnInit] after RunStartupLuauScript");
	bPendingTemporalHistoryClear = true;
	UpdateStartupLoadingProgress(0.98f, L"Preparing first frame");
	InitializeAutoAADump();
	AppendCpuRuntimeTrace(
		L"[OnInit] after InitializeAutoAADump initialized=" + std::to_wstring(bAutoAADumpInitialized ? 1 : 0) +
		L", completed=" + std::to_wstring(bAutoAADumpCompleted ? 1 : 0) +
		L", dir=" + AutoAADumpDir);
	if (!bAutoAADumpEnabled && (bCommandLineLoadLatestCameraPath || !CommandLineCameraPathFile.empty() || bCommandLineCameraPathDump))
	{
		bool bLoadedCameraPath = false;
		if (!CommandLineCameraPathFile.empty())
			bLoadedCameraPath = LoadCameraPath(CommandLineCameraPathFile);
		else
			bLoadedCameraPath = LoadLatestCameraPath();

		AppendCpuRuntimeTrace(
			L"[OnInit] command line camera path loaded=" + std::to_wstring(bLoadedCameraPath ? 1 : 0) +
			L", dump=" + std::to_wstring(bCommandLineCameraPathDump ? 1 : 0) +
			L", status=" + LastCameraPathStatus);

		if (bLoadedCameraPath && bCommandLineCameraPathDump)
		{
			StartCameraPathDump();
			bCameraPathDumpExitWhenComplete = bCameraPathDumping;
			AppendCpuRuntimeTrace(
				L"[OnInit] command line camera path dump started=" + std::to_wstring(bCameraPathDumping ? 1 : 0) +
				L", dir=" + LastCameraPathDumpDir);
		}
		else if (!bLoadedCameraPath && bCommandLineCameraPathDump)
		{
			PostQuitMessage(1);
		}
	}
	UpdateStartupLoadingProgress(1.0f, L"Ready");
	bStartupLoadingScreenActive = false;
}

void AppendCpuRuntimeTrace(const std::wstring& line)
{
	const std::filesystem::path tracePath = RuntimePaths::LogFile(L"cpu_runtime_trace.log");
	std::filesystem::create_directories(tracePath.parent_path());
	std::wofstream traceFile(tracePath, std::ios::app);
	if (traceFile.is_open())
	{
		traceFile << line << L"\n";
	}
}

void Corona::LoadPipeline()
{
	if (bCommandLineRenderBackendOverrideSet && CommandLineRenderBackendAPI == ERenderBackendAPI::Vulkan)
	{
		AppendCpuRuntimeTrace(L"[LoadPipeline] begin Vulkan path");
		renderBackend = CreateRenderBackend(ERenderBackendAPI::Vulkan, nullptr);
		dx12_rhi = nullptr;
		if (!renderBackend)
		{
			throw std::runtime_error("Failed to create Vulkan render backend.");
		}
		AppendCpuRuntimeTrace(
			L"[LoadPipeline] after CreateRenderBackend Vulkan api=" + std::to_wstring(static_cast<int>(renderBackend->GetAPI())) +
			L", name=" + std::wstring(renderBackend->GetBackendName(), renderBackend->GetBackendName() + std::strlen(renderBackend->GetBackendName())));

		renderBackend->CreateSwapChainForWindow(
			nullptr,
			Win32Application::GetHwnd(),
			m_width,
			m_height,
			DXGI_FORMAT_R8G8B8A8_UNORM);
		AppendCpuRuntimeTrace(L"[LoadPipeline] after CreateSwapChainForWindow Vulkan");
		return;
	}

	UINT dxgiFactoryFlags = 0;

	const bool bEnableD3D12DebugLayer =
#if defined(_DEBUG)
		true;
#else
		_wgetenv(L"CORONA_D3D12_DEBUG") != nullptr;
#endif

	if (bEnableD3D12DebugLayer)
	{
		// Enable the debug layer (requires the Graphics Tools "optional feature").
		// NOTE: Enabling the debug layer after device creation will invalidate the active device.
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;

			ComPtr<ID3D12Debug1> spDebugController1;
			if (SUCCEEDED(debugController->QueryInterface(IID_PPV_ARGS(&spDebugController1))))
			{
				//spDebugController1->SetEnableGPUBasedValidation(true);
			}
		}
	}

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

		D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {};
		shaderModel.HighestShaderModel = D3D_SHADER_MODEL_6_9;
		HRESULT shaderModelHr = m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
		bD3D12ShaderModel69Supported =
			SUCCEEDED(shaderModelHr) &&
			shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_9;
		AppendCpuRuntimeTrace(
			L"[D3D12Caps] shaderModelHr=0x" + FormatHex32(static_cast<uint32_t>(shaderModelHr)) +
			L", highestShaderModel=0x" + FormatHex32(static_cast<uint32_t>(shaderModel.HighestShaderModel)) +
			L", shaderModel69Supported=" + std::to_wstring(bD3D12ShaderModel69Supported ? 1 : 0));

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

glm::mat4x4 Corona::BuildCenteredSceneTransform(
	const shared_ptr<Scene>& scene,
	float targetExtent,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees) const
{
	if (!scene || !scene->bHasBounds)
		return glm::mat4x4(1.0f);

	const glm::vec3 boundsSize = scene->BoundsMax - scene->BoundsMin;
	const float maxExtent = std::max(std::max(boundsSize.x, boundsSize.y), boundsSize.z);
	const float scale = maxExtent > 1.0e-4f ? targetExtent / maxExtent : 1.0f;
	const glm::vec3 center = (scene->BoundsMin + scene->BoundsMax) * 0.5f;
	const glm::mat4x4 rotation =
		glm::rotate(glm::radians(rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f)) *
		glm::rotate(glm::radians(rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
		glm::rotate(glm::radians(rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));

	return
		glm::translate(position) *
		rotation *
		glm::scale(glm::vec3(scale)) *
		glm::translate(glm::vec3(-center.x, -scene->BoundsMin.y, -center.z));
}

glm::mat4x4 Corona::BuildScaledSceneTransform(
	const shared_ptr<Scene>& scene,
	const glm::vec3& scale,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees) const
{
	if (!scene || !scene->bHasBounds)
		return glm::mat4x4(1.0f);

	const glm::vec3 safeScale = glm::max(scale, glm::vec3(0.001f));
	const glm::vec3 center = (scene->BoundsMin + scene->BoundsMax) * 0.5f;
	const glm::mat4x4 rotation =
		glm::rotate(glm::radians(rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f)) *
		glm::rotate(glm::radians(rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
		glm::rotate(glm::radians(rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));

	return
		glm::translate(position) *
		rotation *
		glm::scale(safeScale) *
		glm::translate(glm::vec3(-center.x, -scene->BoundsMin.y, -center.z));
}

Corona::SceneObjectHandle Corona::AddCenteredSceneObject(
	const shared_ptr<Scene>& scene,
	float targetExtent,
	float roughness,
	float metallic,
	bool bOverrideRoughnessMetallic,
	const glm::vec3& position,
	const glm::vec3& rotationDegrees)
{
	if (!scene)
		return InvalidSceneObjectHandle;

	SceneObjectDesc desc;
	desc.ScenePtr = scene;
	desc.Transform = BuildCenteredSceneTransform(scene, targetExtent, position, rotationDegrees);
	desc.Roughness = roughness;
	desc.Metallic = metallic;
	desc.bOverrideRoughnessMetallic = bOverrideRoughnessMetallic;
	return AddSceneObject(desc);
}

shared_ptr<Scene> Corona::CreateMirrorCubeScene()
{
	if (!renderBackend)
		return nullptr;

	struct Vertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
		glm::vec3 Tangent;
	};

	struct CubeFace
	{
		glm::vec3 Normal;
		glm::vec3 Tangent;
		glm::vec3 Positions[4];
	};

	constexpr float kHalfExtent = 0.5f;
	const CubeFace faces[] =
	{
		{ glm::vec3( 0.0f,  0.0f,  1.0f), glm::vec3( 1.0f,  0.0f,  0.0f), { glm::vec3(-kHalfExtent, -kHalfExtent,  kHalfExtent), glm::vec3( kHalfExtent, -kHalfExtent,  kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent,  kHalfExtent), glm::vec3(-kHalfExtent,  kHalfExtent,  kHalfExtent) } },
		{ glm::vec3( 0.0f,  0.0f, -1.0f), glm::vec3(-1.0f,  0.0f,  0.0f), { glm::vec3( kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3(-kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3(-kHalfExtent,  kHalfExtent, -kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent, -kHalfExtent) } },
		{ glm::vec3( 1.0f,  0.0f,  0.0f), glm::vec3( 0.0f,  0.0f, -1.0f), { glm::vec3( kHalfExtent, -kHalfExtent,  kHalfExtent), glm::vec3( kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent, -kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent,  kHalfExtent) } },
		{ glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3( 0.0f,  0.0f,  1.0f), { glm::vec3(-kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3(-kHalfExtent, -kHalfExtent,  kHalfExtent), glm::vec3(-kHalfExtent,  kHalfExtent,  kHalfExtent), glm::vec3(-kHalfExtent,  kHalfExtent, -kHalfExtent) } },
		{ glm::vec3( 0.0f,  1.0f,  0.0f), glm::vec3( 1.0f,  0.0f,  0.0f), { glm::vec3(-kHalfExtent,  kHalfExtent,  kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent,  kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent, -kHalfExtent), glm::vec3(-kHalfExtent,  kHalfExtent, -kHalfExtent) } },
		{ glm::vec3( 0.0f, -1.0f,  0.0f), glm::vec3( 1.0f,  0.0f,  0.0f), { glm::vec3(-kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3( kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3( kHalfExtent, -kHalfExtent,  kHalfExtent), glm::vec3(-kHalfExtent, -kHalfExtent,  kHalfExtent) } },
	};

	std::vector<Vertex> vertices;
	vertices.reserve(24);
	std::vector<UINT32> indices;
	indices.reserve(36);
	const glm::vec2 uvs[] =
	{
		glm::vec2(0.0f, 1.0f),
		glm::vec2(1.0f, 1.0f),
		glm::vec2(1.0f, 0.0f),
		glm::vec2(0.0f, 0.0f),
	};

	for (const CubeFace& face : faces)
	{
		const UINT32 baseVertex = static_cast<UINT32>(vertices.size());
		for (UINT32 vertexIndex = 0; vertexIndex < 4; ++vertexIndex)
		{
			Vertex vertex = {};
			vertex.Position = face.Positions[vertexIndex];
			vertex.Normal = face.Normal;
			vertex.UV = uvs[vertexIndex];
			vertex.Tangent = face.Tangent;
			vertices.push_back(vertex);
		}

		indices.push_back(baseVertex + 0);
		indices.push_back(baseVertex + 1);
		indices.push_back(baseVertex + 2);
		indices.push_back(baseVertex + 0);
		indices.push_back(baseVertex + 2);
		indices.push_back(baseVertex + 3);
	}

	shared_ptr<Material> material = std::make_shared<Material>();
	material->Diffuse = DefaultWhiteTex;
	material->Normal = DefaultNormalTex;
	material->Roughness = DefaultBlackTex;
	material->Metallic = DefaultWhiteTex;

	Mesh* mesh = new Mesh;
	mesh->Owner = renderBackend.get();
	mesh->transform = glm::mat4x4(1.0f);
	mesh->NumVertices = static_cast<UINT>(vertices.size());
	mesh->NumIndices = static_cast<UINT>(indices.size());
	mesh->VertexStride = sizeof(Vertex);
	mesh->IndexFormat = DXGI_FORMAT_R32_UINT;
	mesh->Mat = material;
	mesh->Vb = renderBackend->CreateVertexBuffer(sizeof(Vertex) * vertices.size(), sizeof(Vertex), vertices.data());
	mesh->Ib = renderBackend->CreateIndexBuffer(mesh->IndexFormat, sizeof(UINT32) * indices.size(), indices.data());
	mesh->CpuPositions.reserve(vertices.size());
	for (const Vertex& vertex : vertices)
		mesh->CpuPositions.push_back(vertex.Position);
	mesh->CpuIndices = indices;

	Mesh::DrawCall drawCall = {};
	drawCall.IndexStart = 0;
	drawCall.IndexCount = mesh->NumIndices;
	drawCall.VertexBase = 0;
	drawCall.VertexCount = mesh->NumVertices;
	drawCall.mat = material;
	mesh->Draws.push_back(drawCall);

	shared_ptr<Scene> scene = std::make_shared<Scene>();
	scene->Materials.push_back(material);
	scene->meshes.push_back(shared_ptr<Mesh>(mesh));
	scene->bHasBounds = true;
	scene->BoundsMin = glm::vec3(-kHalfExtent, -kHalfExtent, -kHalfExtent);
	scene->BoundsMax = glm::vec3( kHalfExtent,  kHalfExtent,  kHalfExtent);

	return scene;
}

shared_ptr<Texture> Corona::GetProceduralDungeonBrickDiffuseTexture()
{
	if (ProceduralDungeonBrickDiffuseTex)
		return ProceduralDungeonBrickDiffuseTex;
	if (!renderBackend)
		return DefaultWhiteTex;

	const std::filesystem::path texturePath = GetAssetFullPath(L"assets\\procedural\\dungeon_brick.bmp");
	std::error_code ec;
	std::filesystem::create_directories(texturePath.parent_path(), ec);
	if (ec)
	{
		AppendCpuRuntimeTrace(L"[ProceduralTexture] failed to create directory: " + texturePath.parent_path().wstring());
		return DefaultWhiteTex;
	}

	if (!WriteProceduralDungeonBrickBmp(texturePath))
	{
		AppendCpuRuntimeTrace(L"[ProceduralTexture] failed to write dungeon brick texture: " + texturePath.wstring());
		return DefaultWhiteTex;
	}

	ProceduralDungeonBrickDiffuseTex = renderBackend->CreateTextureFromFile(texturePath.wstring(), false);
	if (!ProceduralDungeonBrickDiffuseTex)
	{
		AppendCpuRuntimeTrace(L"[ProceduralTexture] failed to load dungeon brick texture: " + texturePath.wstring());
		return DefaultWhiteTex;
	}

	AppendCpuRuntimeTrace(L"[ProceduralTexture] generated dungeon brick texture: " + texturePath.wstring());
	return ProceduralDungeonBrickDiffuseTex;
}

shared_ptr<Scene> Corona::CreateProceduralBoxScene(const glm::vec3& baseColor, bool bUseBrickTexture, float uvRepeat)
{
	if (!renderBackend)
		return nullptr;

	struct Vertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
		glm::vec3 Tangent;
	};

	struct CubeFace
	{
		glm::vec3 Normal;
		glm::vec3 Tangent;
		glm::vec3 Positions[4];
	};

	constexpr float kHalfExtent = 0.5f;
	const CubeFace faces[] =
	{
		{ glm::vec3( 0.0f,  0.0f,  1.0f), glm::vec3( 1.0f,  0.0f,  0.0f), { glm::vec3(-kHalfExtent, -kHalfExtent,  kHalfExtent), glm::vec3( kHalfExtent, -kHalfExtent,  kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent,  kHalfExtent), glm::vec3(-kHalfExtent,  kHalfExtent,  kHalfExtent) } },
		{ glm::vec3( 0.0f,  0.0f, -1.0f), glm::vec3(-1.0f,  0.0f,  0.0f), { glm::vec3( kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3(-kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3(-kHalfExtent,  kHalfExtent, -kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent, -kHalfExtent) } },
		{ glm::vec3( 1.0f,  0.0f,  0.0f), glm::vec3( 0.0f,  0.0f, -1.0f), { glm::vec3( kHalfExtent, -kHalfExtent,  kHalfExtent), glm::vec3( kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent, -kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent,  kHalfExtent) } },
		{ glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3( 0.0f,  0.0f,  1.0f), { glm::vec3(-kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3(-kHalfExtent, -kHalfExtent,  kHalfExtent), glm::vec3(-kHalfExtent,  kHalfExtent,  kHalfExtent), glm::vec3(-kHalfExtent,  kHalfExtent, -kHalfExtent) } },
		{ glm::vec3( 0.0f,  1.0f,  0.0f), glm::vec3( 1.0f,  0.0f,  0.0f), { glm::vec3(-kHalfExtent,  kHalfExtent,  kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent,  kHalfExtent), glm::vec3( kHalfExtent,  kHalfExtent, -kHalfExtent), glm::vec3(-kHalfExtent,  kHalfExtent, -kHalfExtent) } },
		{ glm::vec3( 0.0f, -1.0f,  0.0f), glm::vec3( 1.0f,  0.0f,  0.0f), { glm::vec3(-kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3( kHalfExtent, -kHalfExtent, -kHalfExtent), glm::vec3( kHalfExtent, -kHalfExtent,  kHalfExtent), glm::vec3(-kHalfExtent, -kHalfExtent,  kHalfExtent) } },
	};

	std::vector<Vertex> vertices;
	vertices.reserve(24);
	std::vector<UINT32> indices;
	indices.reserve(36);
	const float safeUvRepeat = std::clamp(uvRepeat, 1.0f, 64.0f);
	const glm::vec2 uvs[] =
	{
		glm::vec2(0.0f, safeUvRepeat),
		glm::vec2(safeUvRepeat, safeUvRepeat),
		glm::vec2(safeUvRepeat, 0.0f),
		glm::vec2(0.0f, 0.0f),
	};

	for (const CubeFace& face : faces)
	{
		const UINT32 baseVertex = static_cast<UINT32>(vertices.size());
		for (UINT32 vertexIndex = 0; vertexIndex < 4; ++vertexIndex)
		{
			Vertex vertex = {};
			vertex.Position = face.Positions[vertexIndex];
			vertex.Normal = face.Normal;
			vertex.UV = uvs[vertexIndex];
			vertex.Tangent = face.Tangent;
			vertices.push_back(vertex);
		}

		indices.push_back(baseVertex + 0);
		indices.push_back(baseVertex + 1);
		indices.push_back(baseVertex + 2);
		indices.push_back(baseVertex + 0);
		indices.push_back(baseVertex + 2);
		indices.push_back(baseVertex + 3);
	}

	shared_ptr<Material> material = std::make_shared<Material>();
	material->BaseColorFactor = glm::vec4(glm::clamp(baseColor, glm::vec3(0.0f), glm::vec3(1.0f)), 1.0f);
	material->Diffuse = bUseBrickTexture ? GetProceduralDungeonBrickDiffuseTexture() : DefaultWhiteTex;
	material->Normal = DefaultNormalTex;
	material->Roughness = DefaultRougnessTex;
	material->Metallic = DefaultBlackTex;

	Mesh* mesh = new Mesh;
	mesh->Owner = renderBackend.get();
	mesh->transform = glm::mat4x4(1.0f);
	mesh->NumVertices = static_cast<UINT>(vertices.size());
	mesh->NumIndices = static_cast<UINT>(indices.size());
	mesh->VertexStride = sizeof(Vertex);
	mesh->IndexFormat = DXGI_FORMAT_R32_UINT;
	mesh->Mat = material;
	mesh->Vb = renderBackend->CreateVertexBuffer(sizeof(Vertex) * vertices.size(), sizeof(Vertex), vertices.data());
	mesh->Ib = renderBackend->CreateIndexBuffer(mesh->IndexFormat, sizeof(UINT32) * indices.size(), indices.data());
	mesh->CpuPositions.reserve(vertices.size());
	for (const Vertex& vertex : vertices)
		mesh->CpuPositions.push_back(vertex.Position);
	mesh->CpuIndices = indices;

	Mesh::DrawCall drawCall = {};
	drawCall.IndexStart = 0;
	drawCall.IndexCount = mesh->NumIndices;
	drawCall.VertexBase = 0;
	drawCall.VertexCount = mesh->NumVertices;
	drawCall.mat = material;
	mesh->Draws.push_back(drawCall);

	shared_ptr<Scene> scene = std::make_shared<Scene>();
	scene->Materials.push_back(material);
	scene->meshes.push_back(shared_ptr<Mesh>(mesh));
	scene->bHasBounds = true;
	scene->BoundsMin = glm::vec3(-kHalfExtent, -kHalfExtent, -kHalfExtent);
	scene->BoundsMax = glm::vec3( kHalfExtent,  kHalfExtent,  kHalfExtent);

	return scene;
}

shared_ptr<Scene> Corona::CreateProceduralBlockCharacterScene(UINT32 seed)
{
	if (!renderBackend)
		return nullptr;

	struct Vertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
		glm::vec3 Tangent;
	};

	struct CubeFace
	{
		glm::vec3 Normal;
		glm::vec3 Tangent;
		glm::vec3 Positions[4];
	};

	auto scene = std::make_shared<Scene>();

	UINT32 randomState = seed ? seed : 1u;
	auto random01 = [&randomState]() -> float
	{
		randomState = randomState * 1664525u + 1013904223u;
		return static_cast<float>((randomState >> 8) & 0xFFFFu) / 65535.0f;
	};
	auto randomRange = [&random01](float minValue, float maxValue) -> float
	{
		return minValue + (maxValue - minValue) * random01();
	};
	auto randomInt = [&random01](UINT32 count) -> UINT32
	{
		if (count == 0)
			return 0;
		return static_cast<UINT32>(random01() * static_cast<float>(count)) % count;
	};
	auto randomSign = [&random01]() -> float
	{
		return random01() < 0.5f ? -1.0f : 1.0f;
	};
	auto jitterColor = [&random01](const glm::vec3& color, float amount) -> glm::vec3
	{
		const glm::vec3 delta(
			random01() * 2.0f - 1.0f,
			random01() * 2.0f - 1.0f,
			random01() * 2.0f - 1.0f);
		return glm::clamp(color + delta * amount, glm::vec3(0.02f), glm::vec3(1.0f));
	};
	auto makeMaterial = [&scene, this](const glm::vec3& baseColor) -> shared_ptr<Material>
	{
		shared_ptr<Material> material = std::make_shared<Material>();
		material->BaseColorFactor = glm::vec4(baseColor, 1.0f);
		material->Diffuse = DefaultWhiteTex;
		material->Normal = DefaultNormalTex;
		material->Roughness = DefaultRougnessTex;
		material->Metallic = DefaultBlackTex;
		scene->Materials.push_back(material);
		return material;
	};

	const glm::vec3 skinPalette[] =
	{
		glm::vec3(0.86f, 0.58f, 0.42f),
		glm::vec3(0.78f, 0.48f, 0.33f),
		glm::vec3(0.63f, 0.38f, 0.28f),
		glm::vec3(0.91f, 0.69f, 0.53f),
	};
	const glm::vec3 coatPalette[] =
	{
		glm::vec3(0.10f, 0.42f, 0.50f),
		glm::vec3(0.45f, 0.16f, 0.45f),
		glm::vec3(0.18f, 0.42f, 0.22f),
		glm::vec3(0.62f, 0.28f, 0.13f),
		glm::vec3(0.26f, 0.28f, 0.34f),
	};
	const glm::vec3 pantsPalette[] =
	{
		glm::vec3(0.09f, 0.14f, 0.30f),
		glm::vec3(0.12f, 0.18f, 0.16f),
		glm::vec3(0.24f, 0.20f, 0.15f),
		glm::vec3(0.12f, 0.12f, 0.14f),
	};
	const glm::vec3 hairPalette[] =
	{
		glm::vec3(0.18f, 0.075f, 0.035f),
		glm::vec3(0.05f, 0.045f, 0.040f),
		glm::vec3(0.58f, 0.43f, 0.24f),
		glm::vec3(0.34f, 0.18f, 0.07f),
	};
	const glm::vec3 accentPalette[] =
	{
		glm::vec3(0.83f, 0.19f, 0.16f),
		glm::vec3(0.95f, 0.64f, 0.20f),
		glm::vec3(0.20f, 0.58f, 0.85f),
		glm::vec3(0.62f, 0.78f, 0.26f),
		glm::vec3(0.84f, 0.34f, 0.66f),
	};
	const glm::vec3 skinBase = skinPalette[randomInt(4u)];
	const glm::vec3 coatBase = coatPalette[randomInt(5u)];
	const glm::vec3 accentBase = accentPalette[randomInt(5u)];

	const shared_ptr<Material> skin = makeMaterial(jitterColor(skinBase, 0.035f));
	const shared_ptr<Material> skinShadow = makeMaterial(jitterColor(skinBase * 0.72f, 0.025f));
	const shared_ptr<Material> coat = makeMaterial(jitterColor(coatBase, 0.04f));
	const shared_ptr<Material> coatLight = makeMaterial(jitterColor(glm::min(coatBase * 1.28f + glm::vec3(0.035f), glm::vec3(1.0f)), 0.035f));
	const shared_ptr<Material> pants = makeMaterial(jitterColor(pantsPalette[randomInt(4u)], 0.025f));
	const shared_ptr<Material> boots = makeMaterial(jitterColor(glm::vec3(0.035f, 0.035f, 0.055f), 0.015f));
	const shared_ptr<Material> hair = makeMaterial(jitterColor(hairPalette[randomInt(4u)], 0.025f));
	const shared_ptr<Material> scarf = makeMaterial(jitterColor(accentBase, 0.035f));
	const shared_ptr<Material> eye = makeMaterial(glm::vec3(0.035f, 0.045f, 0.060f));
	const shared_ptr<Material> eyeHighlight = makeMaterial(glm::vec3(0.82f, 0.92f, 1.0f));
	const shared_ptr<Material> trim = makeMaterial(jitterColor(glm::vec3(0.94f, 0.72f, 0.38f), 0.025f));
	const shared_ptr<Material> accessory = makeMaterial(jitterColor(glm::min(accentBase * 0.62f + glm::vec3(0.02f), glm::vec3(1.0f)), 0.02f));

	std::vector<Vertex> vertices;
	std::vector<UINT32> indices;
	vertices.reserve(512);
	indices.reserve(768);

	glm::vec3 boundsMin(0.0f);
	glm::vec3 boundsMax(0.0f);
	bool bHasBounds = false;
	auto includeBounds = [&boundsMin, &boundsMax, &bHasBounds](const glm::vec3& p)
	{
		if (!bHasBounds)
		{
			boundsMin = p;
			boundsMax = p;
			bHasBounds = true;
			return;
		}
		boundsMin = glm::min(boundsMin, p);
		boundsMax = glm::max(boundsMax, p);
	};

	Mesh* mesh = new Mesh;
	mesh->Owner = renderBackend.get();
	mesh->transform = glm::mat4x4(1.0f);
	mesh->IndexFormat = DXGI_FORMAT_R32_UINT;
	mesh->VertexStride = sizeof(Vertex);
	mesh->Mat = skin;

	auto appendBox = [&](
		const glm::vec3& center,
		const glm::vec3& size,
		const shared_ptr<Material>& material)
	{
		const glm::vec3 halfSize = size * 0.5f;
		const glm::vec3 mn = center - halfSize;
		const glm::vec3 mx = center + halfSize;
		const CubeFace faces[] =
		{
			{ glm::vec3( 0.0f,  0.0f,  1.0f), glm::vec3( 1.0f,  0.0f,  0.0f), { glm::vec3(mn.x, mn.y, mx.z), glm::vec3(mx.x, mn.y, mx.z), glm::vec3(mx.x, mx.y, mx.z), glm::vec3(mn.x, mx.y, mx.z) } },
			{ glm::vec3( 0.0f,  0.0f, -1.0f), glm::vec3(-1.0f,  0.0f,  0.0f), { glm::vec3(mx.x, mn.y, mn.z), glm::vec3(mn.x, mn.y, mn.z), glm::vec3(mn.x, mx.y, mn.z), glm::vec3(mx.x, mx.y, mn.z) } },
			{ glm::vec3( 1.0f,  0.0f,  0.0f), glm::vec3( 0.0f,  0.0f, -1.0f), { glm::vec3(mx.x, mn.y, mx.z), glm::vec3(mx.x, mn.y, mn.z), glm::vec3(mx.x, mx.y, mn.z), glm::vec3(mx.x, mx.y, mx.z) } },
			{ glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3( 0.0f,  0.0f,  1.0f), { glm::vec3(mn.x, mn.y, mn.z), glm::vec3(mn.x, mn.y, mx.z), glm::vec3(mn.x, mx.y, mx.z), glm::vec3(mn.x, mx.y, mn.z) } },
			{ glm::vec3( 0.0f,  1.0f,  0.0f), glm::vec3( 1.0f,  0.0f,  0.0f), { glm::vec3(mn.x, mx.y, mx.z), glm::vec3(mx.x, mx.y, mx.z), glm::vec3(mx.x, mx.y, mn.z), glm::vec3(mn.x, mx.y, mn.z) } },
			{ glm::vec3( 0.0f, -1.0f,  0.0f), glm::vec3( 1.0f,  0.0f,  0.0f), { glm::vec3(mn.x, mn.y, mn.z), glm::vec3(mx.x, mn.y, mn.z), glm::vec3(mx.x, mn.y, mx.z), glm::vec3(mn.x, mn.y, mx.z) } },
		};
		const glm::vec2 uvs[] =
		{
			glm::vec2(0.0f, 1.0f),
			glm::vec2(1.0f, 1.0f),
			glm::vec2(1.0f, 0.0f),
			glm::vec2(0.0f, 0.0f),
		};

		const UINT32 indexStart = static_cast<UINT32>(indices.size());
		for (const CubeFace& face : faces)
		{
			const UINT32 baseVertex = static_cast<UINT32>(vertices.size());
			for (UINT32 vertexIndex = 0; vertexIndex < 4; ++vertexIndex)
			{
				Vertex vertex = {};
				vertex.Position = face.Positions[vertexIndex];
				vertex.Normal = face.Normal;
				vertex.UV = uvs[vertexIndex];
				vertex.Tangent = face.Tangent;
				vertices.push_back(vertex);
				includeBounds(vertex.Position);
			}

			indices.push_back(baseVertex + 0);
			indices.push_back(baseVertex + 1);
			indices.push_back(baseVertex + 2);
			indices.push_back(baseVertex + 0);
			indices.push_back(baseVertex + 2);
			indices.push_back(baseVertex + 3);
		}

		Mesh::DrawCall drawCall = {};
		drawCall.IndexStart = indexStart;
		drawCall.IndexCount = static_cast<UINT>(indices.size()) - indexStart;
		drawCall.VertexBase = 0;
		drawCall.VertexCount = static_cast<UINT>(vertices.size());
		drawCall.mat = material;
		mesh->Draws.push_back(drawCall);
	};

	const float bootHeight = randomRange(0.16f, 0.30f);
	const float legHeight = randomRange(0.78f, 1.18f);
	const float legWidth = randomRange(0.22f, 0.36f);
	const float legDepth = randomRange(0.26f, 0.42f);
	const float legGap = randomRange(0.10f, 0.24f);
	const float legOffsetX = legGap * 0.5f + legWidth * 0.5f;
	const float legCenterY = bootHeight * 0.62f + legHeight * 0.5f;
	const float hipY = bootHeight * 0.62f + legHeight;
	const float bootWidth = legWidth + randomRange(0.04f, 0.16f);
	const float bootDepth = legDepth + randomRange(0.08f, 0.24f);
	const float bootForward = randomRange(0.02f, 0.09f);

	appendBox(glm::vec3(-legOffsetX, legCenterY, 0.00f), glm::vec3(legWidth, legHeight, legDepth), pants);
	appendBox(glm::vec3( legOffsetX, legCenterY, 0.00f), glm::vec3(legWidth, legHeight, legDepth), pants);
	appendBox(glm::vec3(-legOffsetX, bootHeight * 0.5f, bootForward), glm::vec3(bootWidth, bootHeight, bootDepth), boots);
	appendBox(glm::vec3( legOffsetX, bootHeight * 0.5f, bootForward), glm::vec3(bootWidth, bootHeight, bootDepth), boots);

	const float torsoHeight = randomRange(0.78f, 1.10f);
	const float torsoWidth = randomRange(0.68f, 1.00f);
	const float torsoDepth = randomRange(0.34f, 0.54f);
	const float torsoCenterY = hipY + torsoHeight * 0.48f;
	const float torsoTopY = hipY + torsoHeight;
	const float panelWidth = torsoWidth * randomRange(0.38f, 0.72f);
	const float panelHeight = torsoHeight * randomRange(0.55f, 0.82f);
	const float beltHeight = randomRange(0.07f, 0.14f);

	appendBox(glm::vec3(0.00f, torsoCenterY, 0.00f), glm::vec3(torsoWidth, torsoHeight, torsoDepth), coat);
	appendBox(glm::vec3(0.00f, hipY + torsoHeight * 0.52f, torsoDepth * 0.5f + 0.022f), glm::vec3(panelWidth, panelHeight, 0.044f), coatLight);
	appendBox(glm::vec3(0.00f, hipY + beltHeight * 0.8f, torsoDepth * 0.5f + 0.026f), glm::vec3(torsoWidth * 1.04f, beltHeight, 0.052f), trim);

	const float armWidth = randomRange(0.20f, 0.34f);
	const float armHeight = randomRange(0.66f, 1.04f);
	const float armDepth = randomRange(0.24f, 0.40f);
	const float armOutset = randomRange(0.02f, 0.12f);
	const float armPoseOffset = randomRange(-0.10f, 0.16f);
	const float leftArmY = torsoTopY - armHeight * 0.50f + armPoseOffset;
	const float rightArmY = torsoTopY - armHeight * 0.50f - armPoseOffset * 0.45f;
	const float armX = torsoWidth * 0.5f + armWidth * 0.5f + armOutset;
	const float handHeight = randomRange(0.18f, 0.30f);
	const float handDepth = armDepth + randomRange(-0.02f, 0.04f);

	appendBox(glm::vec3(-armX, leftArmY, randomRange(-0.03f, 0.04f)), glm::vec3(armWidth, armHeight, armDepth), coat);
	appendBox(glm::vec3( armX, rightArmY, randomRange(-0.03f, 0.04f)), glm::vec3(armWidth, armHeight, armDepth), coat);
	appendBox(glm::vec3(-armX, leftArmY - armHeight * 0.5f - handHeight * 0.26f, 0.02f), glm::vec3(armWidth * 1.03f, handHeight, handDepth), skin);
	appendBox(glm::vec3( armX, rightArmY - armHeight * 0.5f - handHeight * 0.26f, 0.02f), glm::vec3(armWidth * 1.03f, handHeight, handDepth), skin);

	const float neckHeight = randomRange(0.06f, 0.16f);
	const float headWidth = randomRange(0.62f, 0.90f);
	const float headHeight = randomRange(0.62f, 0.92f);
	const float headDepth = randomRange(0.58f, 0.84f);
	const float headCenterZ = randomRange(-0.025f, 0.035f);
	const float headCenterY = torsoTopY + neckHeight + headHeight * 0.5f;
	const float headTopY = headCenterY + headHeight * 0.5f;

	appendBox(glm::vec3(0.00f, torsoTopY + neckHeight * 0.52f, 0.00f), glm::vec3(headWidth * 0.28f, neckHeight * 1.2f, headDepth * 0.42f), skinShadow);
	appendBox(glm::vec3(0.00f, headCenterY, headCenterZ), glm::vec3(headWidth, headHeight, headDepth), skin);

	const float scarfHeight = randomRange(0.10f, 0.18f);
	const float scarfY = torsoTopY + neckHeight * 0.35f;
	appendBox(glm::vec3(0.00f, scarfY, torsoDepth * 0.5f + 0.03f), glm::vec3(torsoWidth * 1.08f, scarfHeight, 0.08f), scarf);
	const float scarfTailSide = randomSign();
	const float scarfTailHeight = randomRange(0.28f, 0.58f);
	appendBox(
		glm::vec3(scarfTailSide * randomRange(0.10f, 0.26f), scarfY - scarfTailHeight * 0.44f, torsoDepth * 0.5f + 0.052f),
		glm::vec3(randomRange(0.11f, 0.22f), scarfTailHeight, 0.06f),
		scarf);

	const UINT32 hairStyle = randomInt(5u);
	if (hairStyle == 0)
	{
		const float capHeight = randomRange(0.12f, 0.24f);
		appendBox(glm::vec3(0.00f, headTopY + capHeight * 0.42f, headCenterZ), glm::vec3(headWidth * 1.04f, capHeight, headDepth * 1.02f), hair);
		appendBox(glm::vec3(0.00f, headCenterY + headHeight * 0.18f, headCenterZ + headDepth * 0.52f), glm::vec3(headWidth * 0.74f, headHeight * 0.18f, 0.10f), hair);
	}
	else if (hairStyle == 1)
	{
		appendBox(glm::vec3(0.00f, headTopY + 0.06f, headCenterZ), glm::vec3(headWidth * 1.08f, 0.16f, headDepth * 0.96f), hair);
		appendBox(glm::vec3(-headWidth * 0.54f, headCenterY - headHeight * 0.02f, headCenterZ), glm::vec3(0.12f, headHeight * 0.72f, headDepth * 0.92f), hair);
		appendBox(glm::vec3( headWidth * 0.54f, headCenterY - headHeight * 0.02f, headCenterZ), glm::vec3(0.12f, headHeight * 0.72f, headDepth * 0.92f), hair);
	}
	else if (hairStyle == 2)
	{
		appendBox(glm::vec3(0.00f, headTopY + 0.15f, headCenterZ), glm::vec3(headWidth * 0.34f, 0.34f, headDepth * 0.72f), hair);
		appendBox(glm::vec3(0.00f, headCenterY + headHeight * 0.26f, headCenterZ + headDepth * 0.48f), glm::vec3(headWidth * 0.84f, 0.16f, 0.09f), hair);
	}
	else if (hairStyle == 3)
	{
		appendBox(glm::vec3(0.00f, headTopY + 0.05f, headCenterZ - headDepth * 0.04f), glm::vec3(headWidth * 0.92f, 0.14f, headDepth * 1.06f), hair);
		appendBox(glm::vec3(0.00f, headCenterY - headHeight * 0.03f, headCenterZ - headDepth * 0.52f), glm::vec3(headWidth * 0.86f, headHeight * 0.52f, 0.13f), hair);
	}
	else
	{
		const float side = randomSign();
		appendBox(glm::vec3(0.00f, headTopY + 0.06f, headCenterZ), glm::vec3(headWidth * 0.98f, 0.15f, headDepth * 0.92f), hair);
		appendBox(glm::vec3(side * headWidth * 0.20f, headCenterY + headHeight * 0.16f, headCenterZ + headDepth * 0.51f), glm::vec3(headWidth * 0.72f, 0.22f, 0.10f), hair);
	}

	const float eyeY = headCenterY + headHeight * randomRange(0.06f, 0.18f);
	const float eyeZ = headCenterZ + headDepth * 0.5f + 0.018f;
	const float eyeSpacing = headWidth * randomRange(0.19f, 0.28f);
	const float eyeWidth = randomRange(0.09f, 0.15f);
	const float eyeHeight = randomRange(0.055f, 0.095f);
	appendBox(glm::vec3(-eyeSpacing, eyeY, eyeZ), glm::vec3(eyeWidth, eyeHeight, 0.030f), eye);
	appendBox(glm::vec3( eyeSpacing, eyeY, eyeZ), glm::vec3(eyeWidth, eyeHeight, 0.030f), eye);
	appendBox(glm::vec3(-eyeSpacing + eyeWidth * 0.22f, eyeY + eyeHeight * 0.22f, eyeZ + 0.017f), glm::vec3(eyeWidth * 0.30f, eyeHeight * 0.30f, 0.018f), eyeHighlight);
	appendBox(glm::vec3( eyeSpacing + eyeWidth * 0.22f, eyeY + eyeHeight * 0.22f, eyeZ + 0.017f), glm::vec3(eyeWidth * 0.30f, eyeHeight * 0.30f, 0.018f), eyeHighlight);
	appendBox(glm::vec3(0.00f, headCenterY - headHeight * 0.04f, eyeZ + 0.002f), glm::vec3(randomRange(0.09f, 0.16f), randomRange(0.035f, 0.07f), 0.028f), skinShadow);
	appendBox(glm::vec3(0.00f, headCenterY - headHeight * 0.22f, eyeZ + 0.004f), glm::vec3(randomRange(0.16f, 0.28f), randomRange(0.028f, 0.052f), 0.028f), scarf);

	const UINT32 accessoryStyle = randomInt(5u);
	if (accessoryStyle == 0)
	{
		const float brimY = headTopY + randomRange(0.02f, 0.08f);
		appendBox(glm::vec3(0.00f, brimY, headCenterZ + headDepth * 0.48f), glm::vec3(headWidth * 0.96f, 0.08f, randomRange(0.16f, 0.28f)), accessory);
		appendBox(glm::vec3(0.00f, brimY + 0.11f, headCenterZ), glm::vec3(headWidth * 0.78f, 0.22f, headDepth * 0.76f), accessory);
	}
	else if (accessoryStyle == 1)
	{
		appendBox(glm::vec3(-eyeSpacing, eyeY, eyeZ + 0.026f), glm::vec3(eyeWidth * 1.45f, eyeHeight * 1.20f, 0.020f), accessory);
		appendBox(glm::vec3( eyeSpacing, eyeY, eyeZ + 0.026f), glm::vec3(eyeWidth * 1.45f, eyeHeight * 1.20f, 0.020f), accessory);
		appendBox(glm::vec3(0.00f, eyeY, eyeZ + 0.030f), glm::vec3(eyeSpacing * 1.18f, 0.028f, 0.018f), accessory);
	}
	else if (accessoryStyle == 2)
	{
		appendBox(glm::vec3(0.00f, torsoCenterY + torsoHeight * 0.03f, -torsoDepth * 0.5f - 0.11f), glm::vec3(torsoWidth * 0.72f, torsoHeight * 0.76f, 0.22f), accessory);
	}
	else if (accessoryStyle == 3)
	{
		appendBox(glm::vec3(-torsoWidth * 0.30f, torsoTopY - 0.12f, torsoDepth * 0.5f + 0.04f), glm::vec3(torsoWidth * 0.24f, 0.08f, 0.08f), trim);
		appendBox(glm::vec3( torsoWidth * 0.30f, torsoTopY - 0.12f, torsoDepth * 0.5f + 0.04f), glm::vec3(torsoWidth * 0.24f, 0.08f, 0.08f), trim);
	}
	else
	{
		appendBox(glm::vec3(randomSign() * torsoWidth * 0.22f, torsoCenterY + torsoHeight * 0.18f, torsoDepth * 0.5f + 0.052f), glm::vec3(0.11f, 0.11f, 0.026f), trim);
	}

	mesh->NumVertices = static_cast<UINT>(vertices.size());
	mesh->NumIndices = static_cast<UINT>(indices.size());
	mesh->Vb = renderBackend->CreateVertexBuffer(
		static_cast<uint32_t>(sizeof(Vertex) * vertices.size()),
		sizeof(Vertex),
		vertices.data());
	mesh->Ib = renderBackend->CreateIndexBuffer(
		mesh->IndexFormat,
		static_cast<uint32_t>(sizeof(UINT32) * indices.size()),
		indices.data());
	mesh->CpuPositions.reserve(vertices.size());
	for (const Vertex& vertex : vertices)
		mesh->CpuPositions.push_back(vertex.Position);
	mesh->CpuIndices = indices;

	scene->meshes.push_back(shared_ptr<Mesh>(mesh));
	scene->bHasBounds = bHasBounds;
	scene->BoundsMin = boundsMin;
	scene->BoundsMax = boundsMax;

	return scene;
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
	const bool bSupportsScreenProbeGI = !bVulkanHybridStartup || maxSupportedHybridStage >= 4u;
	const bool bSupportsTemporalDenoise = !bVulkanHybridStartup || maxSupportedHybridStage >= 5u;
	const bool bSupportsFullHybridPresentation = !bVulkanHybridStartup || maxSupportedHybridStage >= 7u;
	const bool bAllowBlueNoiseInit = !bVulkanHybridStartup || maxSupportedHybridStage >= 1u;
	const bool bAllowImguiInit =
		!bCommandLineDisableImgui &&
		(!bVulkanHybridStartup ||
			(bSupportsFullHybridPresentation && !bAutoAADumpEnabled));
	if (bVulkanHybridBootstrap)
		AppendCpuRuntimeTrace(L"[LoadAssets] begin Vulkan hybrid bootstrap");
	else if (bVulkanHybridStartup)
		AppendCpuRuntimeTrace(L"[LoadAssets] begin Vulkan hybrid stage-aware init stage=" + std::to_wstring(maxSupportedHybridStage));
	UpdateStartupLoadingProgress(0.22f, L"Preparing render resources");

	if (bAllowBlueNoiseInit && !bBlueNoiseInitialized)
	{
		UpdateStartupLoadingProgress(0.23f, L"Loading blue-noise texture");
		InitBlueNoiseTexture();
		bBlueNoiseInitialized = true;
	}
	if (bAllowImguiInit && !bImguiInitialized)
	{
		UpdateStartupLoadingProgress(0.24f, L"Initializing UI renderer");
		InitImgui();
		bImguiInitialized = true;
	}
	if (!bAllowImguiInit)
	{
		bShowImgui = false;
		bImguiInitialized = false;
	}

	AppendCpuRuntimeTrace(L"[LoadAssets] before InitGBufferPass");
	UpdateStartupLoadingProgress(0.26f, L"Compiling G-buffer pass");
	InitGBufferPass();
	AppendCpuRuntimeTrace(L"[LoadAssets] after InitGBufferPass");
	if (bSupportsFullHybridPresentation)
	{
		AppendCpuRuntimeTrace(L"[LoadAssets] before full hybrid presentation pass init");
		UpdateStartupLoadingProgress(0.30f, L"Compiling presentation passes");
		InitLightingPass();
		InitToneMapPass();
		if (!bVulkanPathTracingStartup)
			InitTemporalAAPass();
		if (!bVulkanBackend)
		{
			InitDebugPass();
			InitBloomPass();
		}
		AppendCpuRuntimeTrace(L"[LoadAssets] after full hybrid presentation pass init");
	}
	if (bSupportsTemporalDenoise)
	{
		AppendCpuRuntimeTrace(L"[LoadAssets] before InitTemporalDenoisingPass");
		UpdateStartupLoadingProgress(0.37f, L"Compiling temporal denoiser");
		InitTemporalDenoisingPass();
		AppendCpuRuntimeTrace(L"[LoadAssets] after InitTemporalDenoisingPass");
	}
	if (bSupportsScreenProbeGI)
	{
		AppendCpuRuntimeTrace(L"[LoadAssets] before InitScreenProbeGIPass");
		UpdateStartupLoadingProgress(0.39f, L"Compiling screen-probe GI");
		InitScreenProbeGIPass();
		AppendCpuRuntimeTrace(L"[LoadAssets] after InitScreenProbeGIPass");
	}
	if (bSupportsTemporalDenoise)
	{
		AppendCpuRuntimeTrace(L"[LoadAssets] before InitSpatialHashGIPass");
		UpdateStartupLoadingProgress(0.41f, L"Compiling spatial hash GI");
		InitSpatialHashGIPass();
		AppendCpuRuntimeTrace(L"[LoadAssets] after InitSpatialHashGIPass");
	}
	AppendCpuRuntimeTrace(L"[LoadAssets] before InitGpuTimingResources");
	UpdateStartupLoadingProgress(0.45f, L"Preparing GPU timing resources");
	InitGpuTimingResources();
	AppendCpuRuntimeTrace(L"[LoadAssets] after InitGpuTimingResources");

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
		UpdateStartupLoadingProgress(0.48f, L"Allocating color buffers");
		ColorBuffers[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1);
		ColorBuffers[0]->MakeRTV();

		NAME_D3D12_OBJECT(ColorBuffers[0]->resource);

		ColorBuffers[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1);
		ColorBuffers[1]->MakeRTV();

		NAME_D3D12_OBJECT(ColorBuffers[1]->resource);
	}
	AppendCpuRuntimeTrace(L"[LoadAssets] after color buffers");

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
	UpdateStartupLoadingProgress(0.52f, L"Allocating path tracing buffers");
	PathTracingAccumBuffer[0] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1, glm::vec4(0.0f));

	NAME_D3D12_OBJECT(PathTracingAccumBuffer[0]->resource);

	PathTracingAccumBuffer[1] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, DisplayWidth, DisplayHeight, 1, glm::vec4(0.0f));

	NAME_D3D12_OBJECT(PathTracingAccumBuffer[1]->resource);
	AppendCpuRuntimeTrace(L"[LoadAssets] after path tracing buffers");

	// lighting result
	UpdateStartupLoadingProgress(0.55f, L"Allocating lighting buffers");
	LightingBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	LightingBuffer->MakeRTV();

	NAME_D3D12_OBJECT(LightingBuffer->resource);

	DirectLightingBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	DirectLightingBuffer->MakeRTV();

	NAME_D3D12_OBJECT(DirectLightingBuffer->resource);

	DLSSRRBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	NAME_D3D12_OBJECT(DLSSRRBuffer->resource);
	AppendCpuRuntimeTrace(L"[LoadAssets] after lighting buffer");

	// world normal
	UpdateStartupLoadingProgress(0.58f, L"Allocating shadow and normal buffers");
	NormalBuffers[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	NormalBuffers[0]->MakeRTV();

	NAME_D3D12_OBJECT(NormalBuffers[0]->resource);

	NormalBuffers[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	NormalBuffers[1]->MakeRTV();

	NAME_D3D12_OBJECT(NormalBuffers[1]->resource);

	// geometry world normal
	GeomNormalBuffers[0] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	GeomNormalBuffers[0]->MakeRTV();

	NAME_D3D12_OBJECT(GeomNormalBuffers[0]->resource);

	GeomNormalBuffers[1] = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, -0.1f, 0.0f, 0.0f));
	GeomNormalBuffers[1]->MakeRTV();

	NAME_D3D12_OBJECT(GeomNormalBuffers[1]->resource);

	// shadow result
	ShadowBuffer = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(ShadowBuffer->resource);

	AmbientOcclusionBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(1.0f));

	NAME_D3D12_OBJECT(AmbientOcclusionBuffer->resource);

	SkyLightingBuffer = createTexture2D(ETextureFormat::RGBA16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

	NAME_D3D12_OBJECT(SkyLightingBuffer->resource);
	AppendCpuRuntimeTrace(L"[LoadAssets] after shadow buffers");

	// refleciton result
	UpdateStartupLoadingProgress(0.62f, L"Allocating GI buffers");
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

	DiffuseGIHashCachedAux = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGIHashCachedAux->resource);

	DiffuseGIHashCached = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(DiffuseGIHashCached->resource);

	SpatialHashGIActiveFlags = createBuffer(SpatialHashGIEntryCount, sizeof(UINT32), true);
	SpatialHashGIActiveCellSlots = createBuffer(SpatialHashGIActiveCellCapacity, sizeof(UINT32), true);
	SpatialHashGIActiveCounter = createBuffer(4u, sizeof(UINT32), true);
	SpatialHashGICellPosition = createBuffer(SpatialHashGIEntryCount, sizeof(float) * 4u, true);
	SpatialHashGICellNormal = createBuffer(SpatialHashGIEntryCount, sizeof(float) * 4u, true);
	SpatialHashGICellScore = createBuffer(SpatialHashGIEntryCount, sizeof(UINT32), true);
	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		SpatialHashGITraceSH[coefficientIndex] = createBuffer(SpatialHashGIActiveCellCapacity, sizeof(float) * 4u, true);
	SpatialHashGIResolvedKeys[0] = createBuffer(SpatialHashGIEntryCount, sizeof(UINT32), true);
	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
		SpatialHashGIResolvedSH[0][coefficientIndex] = createBuffer(SpatialHashGIEntryCount, sizeof(float) * 4u, true);
	if (SpatialHashGIActiveFlags) SpatialHashGIActiveFlags->MakeStructuredBufferSRV();
	if (SpatialHashGIActiveCellSlots) SpatialHashGIActiveCellSlots->MakeStructuredBufferSRV();
	if (SpatialHashGIActiveCounter) SpatialHashGIActiveCounter->MakeStructuredBufferSRV();
	if (SpatialHashGICellPosition) SpatialHashGICellPosition->MakeStructuredBufferSRV();
	if (SpatialHashGICellNormal) SpatialHashGICellNormal->MakeStructuredBufferSRV();
	if (SpatialHashGICellScore) SpatialHashGICellScore->MakeStructuredBufferSRV();
	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
	{
		if (SpatialHashGITraceSH[coefficientIndex]) SpatialHashGITraceSH[coefficientIndex]->MakeStructuredBufferSRV();
	}
	if (SpatialHashGIResolvedKeys[0]) SpatialHashGIResolvedKeys[0]->MakeStructuredBufferSRV();
	for (UINT coefficientIndex = 0; coefficientIndex < SpatialHashGISHCoefficientCount; ++coefficientIndex)
	{
		if (SpatialHashGIResolvedSH[0][coefficientIndex]) SpatialHashGIResolvedSH[0][coefficientIndex]->MakeStructuredBufferSRV();
	}

	ScreenProbeGIResolved = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(ScreenProbeGIResolved->resource);

	ScreenProbeGIProbeDebug = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(ScreenProbeGIProbeDebug->resource);

	const int ScreenProbeAtlasWidth = std::max(1, (static_cast<int>(RenderWidthLocal) + 3) / 4);
	const int ScreenProbeAtlasHeight = std::max(1, (static_cast<int>(RenderHeightLocal) + 3) / 4);

	ScreenProbeGIRadiance[0] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, ScreenProbeAtlasWidth, ScreenProbeAtlasHeight, 1);

	NAME_D3D12_OBJECT(ScreenProbeGIRadiance[0]->resource);

	ScreenProbeGIRadiance[1] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, ScreenProbeAtlasWidth, ScreenProbeAtlasHeight, 1);

	NAME_D3D12_OBJECT(ScreenProbeGIRadiance[1]->resource);

	for (UINT historyIndex = 0; historyIndex < 2; ++historyIndex)
	{
		for (UINT coefficientIndex = 0; coefficientIndex < ScreenProbeSHCoefficientCount; ++coefficientIndex)
		{
			ScreenProbeGISH[historyIndex][coefficientIndex] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, ScreenProbeAtlasWidth, ScreenProbeAtlasHeight, 1);

			NAME_D3D12_OBJECT(ScreenProbeGISH[historyIndex][coefficientIndex]->resource);
		}
	}

	ScreenProbeGIMetadata[0] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, ScreenProbeAtlasWidth, ScreenProbeAtlasHeight, 1);

	NAME_D3D12_OBJECT(ScreenProbeGIMetadata[0]->resource);

	ScreenProbeGIMetadata[1] = createTexture2D(ETextureFormat::RGBA32Float, TextureUsage_UnorderedAccess, ScreenProbeAtlasWidth, ScreenProbeAtlasHeight, 1);

	NAME_D3D12_OBJECT(ScreenProbeGIMetadata[1]->resource);

	ScreenProbeGIHistory[0] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(ScreenProbeGIHistory[0]->resource);

	ScreenProbeGIHistory[1] = createTexture2D(HybridFloat4UAVFormat, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);

	NAME_D3D12_OBJECT(ScreenProbeGIHistory[1]->resource);

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
	AppendCpuRuntimeTrace(L"[LoadAssets] after temporal GI buffers");

	// albedo
	UpdateStartupLoadingProgress(0.66f, L"Allocating G-buffer textures");
	AlbedoBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	AlbedoBuffer->MakeRTV();

	NAME_D3D12_OBJECT(AlbedoBuffer->resource);

	SpecularAlbedoBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	SpecularAlbedoBuffer->MakeRTV();

	NAME_D3D12_OBJECT(SpecularAlbedoBuffer->resource);

	// velocity
	VelocityBuffer = createTexture2D(ETextureFormat::RG16Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f));
	VelocityBuffer->MakeRTV();
	NAME_D3D12_OBJECT(VelocityBuffer->resource);

	// pbr material
	RoughnessMetalicBuffer = createTexture2D(ETextureFormat::RGBA8Unorm, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.001f, 0.0f, 0.0f, 0.0f));
	RoughnessMetalicBuffer->MakeRTV();

	NAME_D3D12_OBJECT(RoughnessMetalicBuffer->resource);

	PathTracingSpecularHitDistanceBuffer = createTexture2D(ETextureFormat::R32Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(Far));
	NAME_D3D12_OBJECT(PathTracingSpecularHitDistanceBuffer->resource);

	PathTracingSpecularMotionVectorBuffer = createTexture2D(ETextureFormat::RG16Float, TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1, glm::vec4(0.0f));
	NAME_D3D12_OBJECT(PathTracingSpecularMotionVectorBuffer->resource);

	// depth
	DepthBuffer = createTexture2D(ETextureFormat::D32Float, TextureUsage_DepthStencil, RenderWidthLocal, RenderHeightLocal, 1);
	DepthBuffer->MakeDSV();
	NAME_D3D12_OBJECT(DepthBuffer->resource);

	UnjitteredDepthBuffers[0] = createTexture2D(ETextureFormat::R32Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	UnjitteredDepthBuffers[0]->MakeRTV();
	NAME_D3D12_OBJECT(UnjitteredDepthBuffers[0]->resource);

	UnjitteredDepthBuffers[1] = createTexture2D(ETextureFormat::R32Float, TextureUsage_RenderTarget | TextureUsage_UnorderedAccess, RenderWidthLocal, RenderHeightLocal, 1);
	UnjitteredDepthBuffers[1]->MakeRTV();
	NAME_D3D12_OBJECT(UnjitteredDepthBuffers[1]->resource);
	AppendCpuRuntimeTrace(L"[LoadAssets] after gbuffer textures");

	UpdateStartupLoadingProgress(0.70f, L"Loading default textures");
	if (!DefaultWhiteTex) DefaultWhiteTex = renderBackend->CreateTextureFromFile(GetAssetFullPath(L"assets\\default\\default_white.png"), false);
	if (!DefaultBlackTex) DefaultBlackTex = renderBackend->CreateTextureFromFile(GetAssetFullPath(L"assets\\default\\default_black.png"), false);
	if (!DefaultNormalTex) DefaultNormalTex = renderBackend->CreateTextureFromFile(GetAssetFullPath(L"assets\\default\\default_normal.png"), true);
	if (!DefaultRougnessTex) DefaultRougnessTex = renderBackend->CreateTextureFromFile(GetAssetFullPath(L"assets\\default\\default_roughness.png"), true);
	if (bVulkanHybridStartup)
		AppendCpuRuntimeTrace(L"[LoadAssets] after default textures");

	UpdateStartupLoadingProgress(0.72f, L"Loading Sponza scene");
	if (!Sponza) Sponza = LoadModel(WideToUtf8(GetAssetFullPath(L"assets\\Sponza\\Sponza.fbx")));
	if (Sponza && SponzaObject == InvalidSceneObjectHandle)
	{
		SceneObjectDesc desc;
		desc.ScenePtr = Sponza;
		desc.Transform = glm::mat4x4(1.0f);
		desc.Roughness = 1.0f;
		desc.Metallic = 0.0f;
		desc.bOverrideRoughnessMetallic = false;
		SponzaObject = AddSceneObject(desc);
	}
	if (bVulkanHybridStartup)
		AppendCpuRuntimeTrace(L"[LoadAssets] after sponza load");

	const bool bLoadStandaloneDemoObjects = !bEnableStartupLuauScript && !bStartupSponzaFlyMode;
	if (bLoadStandaloneDemoObjects && !Buddha)
	{
		Buddha = LoadModel(WideToUtf8(GetAssetFullPath(L"assets\\buddha\\buddha.obj")));
	}
	if (bLoadStandaloneDemoObjects && Buddha && BuddhaObject == InvalidSceneObjectHandle)
	{
		BuddhaObject = AddCenteredSceneObject(
			Buddha,
			260.0f,
			0.65f,
			0.0f,
			false,
			BuddhaCenterPosition,
			BuddhaCenterRotationDegrees);
	}

	if (bLoadStandaloneDemoObjects && !ShaderBall)
	{
		ShaderBall = LoadModel(WideToUtf8(GetAssetFullPath(L"assets\\shaderBall\\shaderBall.fbx")));
	}
	if (bLoadStandaloneDemoObjects && ShaderBall && ShaderBallObject == InvalidSceneObjectHandle)
	{
		ShaderBallObject = AddCenteredSceneObject(
			ShaderBall,
			220.0f,
			0.15f,
			1.0f,
			false,
			ShaderBallCenterPosition,
			ShaderBallCenterRotationDegrees);
	}

	if (bLoadStandaloneDemoObjects && !Pistol)
	{
		Pistol = LoadModel(WideToUtf8(GetAssetFullPath(L"assets\\pistol\\pistol.obj")));
	}
	if (bLoadStandaloneDemoObjects && Pistol && PistolObject == InvalidSceneObjectHandle)
	{
		PistolObject = AddCenteredSceneObject(
			Pistol,
			280.0f,
			0.55f,
			0.0f,
			false,
			PistolCenterPosition,
			PistolCenterRotationDegrees);
	}

	if (!bStartupSponzaFlyMode && !MirrorCube)
	{
		UpdateStartupLoadingProgress(0.80f, L"Creating mirror cube");
		MirrorCube = CreateMirrorCubeScene();
	}
	if (!bStartupSponzaFlyMode && MirrorCube && MirrorCubeObject == InvalidSceneObjectHandle)
	{
		MirrorCubeObject = AddCenteredSceneObject(
			MirrorCube,
			90.0f,
			0.0f,
			1.0f,
			true,
			MirrorCubeCenterPosition,
			MirrorCubeCenterRotationDegrees);
	}

	// Describe and create a sampler.
	if (!samplerWrap)
	{
		UpdateStartupLoadingProgress(0.82f, L"Creating samplers");
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
		AppendCpuRuntimeTrace(L"[LoadAssets] after samplers");
	if (bVulkanHybridBootstrap)
	{
		AppendCpuRuntimeTrace(L"[LoadAssets] Vulkan hybrid bootstrap assets ready");
		return;
	}
	if (bSupportsHybridRaytracing)
	{
		AppendCpuRuntimeTrace(L"[LoadAssets] before InitRaytracingData");
		UpdateStartupLoadingProgress(0.86f, L"Building ray tracing scene data");
		InitRaytracingData();
		AppendCpuRuntimeTrace(L"[LoadAssets] after InitRaytracingData");
		AppendCpuRuntimeTrace(L"[LoadAssets] before InitRTPSO");
		UpdateStartupLoadingProgress(0.89f, L"Compiling ray tracing pipelines");
		InitRTPSO();
		AppendCpuRuntimeTrace(L"[LoadAssets] after InitRTPSO");
		if (StartupRenderingMode == ERenderingMode::PATHTRACING)
		{
			AppendCpuRuntimeTrace(L"[LoadAssets] before InitPathTracingPass");
			UpdateStartupLoadingProgress(0.91f, L"Compiling path tracing pipeline");
			InitPathTracingPass();
			AppendCpuRuntimeTrace(L"[LoadAssets] after InitPathTracingPass");
		}
	}
	UpdateStartupLoadingProgress(0.92f, L"Renderer assets ready");
	AppendCpuRuntimeTrace(L"[LoadAssets] return");

}



shared_ptr<Scene> Corona::LoadBinaryMeshModel(const std::wstring& binaryFileName, const std::wstring& sourceFileName)
{
	if (bStartupLoadingScreenActive)
		UpdateStartupLoadingProgress(
			std::max(StartupLoadingProgress, 0.72f),
			L"Loading mesh cache: " + GetFileName(binaryFileName.c_str()));

	const auto loadStart = CpuClock::now();
	std::ifstream file(binaryFileName, std::ios::binary);
	if (!file)
	{
		AppendCpuRuntimeTrace(L"[LoadBinaryMeshModel] open failed: " + binaryFileName);
		return nullptr;
	}

	CoronaMeshFileHeader header = {};
	if (!ReadBinaryValue(file, header) ||
		memcmp(header.Magic, kCoronaMeshMagic, sizeof(header.Magic)) != 0 ||
		header.Version != kCoronaMeshVersion ||
		header.HeaderSize != sizeof(CoronaMeshFileHeader) ||
		header.MaterialCount > kCoronaMeshMaxMaterials ||
		header.MeshCount > kCoronaMeshMaxMeshes)
	{
		AppendCpuRuntimeTrace(L"[LoadBinaryMeshModel] invalid header: " + binaryFileName);
		return nullptr;
	}

	const std::wstring textureDir = GetDirectoryFromFilePath(sourceFileName.empty() ? binaryFileName.c_str() : sourceFileName.c_str());
	std::map<std::wstring, shared_ptr<Texture>> textureCache;
	auto loadTextureOrDefault = [&](const std::string& storedPath, bool nonSRGB, const shared_ptr<Texture>& fallback) -> shared_ptr<Texture>
	{
		if (storedPath.empty())
			return fallback;

		std::wstring textureName = GetFileName(Utf8ToWide(storedPath).c_str());
		if (textureName.empty())
			return fallback;

		try
		{
			const std::wstring texturePath = textureDir + textureName;
			const std::wstring textureCacheKey = (nonSRGB ? L"linear|" : L"srgb|") + texturePath;
			auto cachedTexture = textureCache.find(textureCacheKey);
			if (cachedTexture != textureCache.end())
				return cachedTexture->second ? cachedTexture->second : fallback;

			shared_ptr<Texture> texture = renderBackend->CreateTextureFromFile(texturePath, nonSRGB);
			if (texture)
				textureCache[textureCacheKey] = texture;
			return texture ? texture : fallback;
		}
		catch (const std::exception& e)
		{
			AppendCpuRuntimeTrace(L"[LoadBinaryMeshModel] texture fallback: " + Utf8ToWide(storedPath) + L" error=" + AnsiToWString(e.what()));
			return fallback;
		}
	};

	auto scene = std::make_shared<Scene>();
	scene->bHasBounds = true;
	scene->BoundsMin = glm::vec3(header.BoundsMin[0], header.BoundsMin[1], header.BoundsMin[2]);
	scene->BoundsMax = glm::vec3(header.BoundsMax[0], header.BoundsMax[1], header.BoundsMax[2]);
	scene->Materials.reserve(header.MaterialCount);

	for (uint32_t materialIndex = 0; materialIndex < header.MaterialCount; ++materialIndex)
	{
		CoronaMeshDiskMaterial diskMaterial = {};
		if (!ReadBinaryValue(file, diskMaterial.Flags) ||
			!ReadBinaryString(file, diskMaterial.Diffuse) ||
			!ReadBinaryString(file, diskMaterial.Normal) ||
			!ReadBinaryString(file, diskMaterial.Roughness) ||
			!ReadBinaryString(file, diskMaterial.Metallic))
		{
			AppendCpuRuntimeTrace(L"[LoadBinaryMeshModel] material read failed: " + binaryFileName);
			return nullptr;
		}

		auto material = std::make_shared<Material>();
		material->bHasAlpha = (diskMaterial.Flags & kCoronaMeshMaterialHasAlpha) != 0;
		material->Diffuse = loadTextureOrDefault(diskMaterial.Diffuse, false, DefaultWhiteTex);
		material->Normal = loadTextureOrDefault(diskMaterial.Normal, true, DefaultNormalTex);
		material->Roughness = loadTextureOrDefault(diskMaterial.Roughness, true, DefaultRougnessTex);
		material->Metallic = loadTextureOrDefault(diskMaterial.Metallic, true, DefaultBlackTex);
		if (!material->Diffuse) material->Diffuse = DefaultWhiteTex;
		if (!material->Normal) material->Normal = DefaultNormalTex;
		if (!material->Roughness) material->Roughness = DefaultRougnessTex;
		if (!material->Metallic) material->Metallic = DefaultBlackTex;
		scene->Materials.push_back(material);
	}

	if (scene->Materials.empty())
	{
		auto material = std::make_shared<Material>();
		material->Diffuse = DefaultWhiteTex;
		material->Normal = DefaultNormalTex;
		material->Roughness = DefaultRougnessTex;
		material->Metallic = DefaultBlackTex;
		scene->Materials.push_back(material);
	}

	uint64_t totalVertices = 0;
	uint64_t totalIndices = 0;
	scene->meshes.reserve(header.MeshCount);
	for (uint32_t meshIndex = 0; meshIndex < header.MeshCount; ++meshIndex)
	{
		uint32_t materialIndex = 0;
		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
		uint32_t reserved = 0;
		if (!ReadBinaryValue(file, materialIndex) ||
			!ReadBinaryValue(file, vertexCount) ||
			!ReadBinaryValue(file, indexCount) ||
			!ReadBinaryValue(file, reserved) ||
			vertexCount == 0 ||
			indexCount == 0 ||
			(indexCount % 3) != 0)
		{
			AppendCpuRuntimeTrace(L"[LoadBinaryMeshModel] mesh header read failed: " + binaryFileName);
			return nullptr;
		}

		std::vector<CoronaMeshDiskVertex> vertices(vertexCount);
		std::vector<UINT32> indices(indexCount);
		if (!ReadBinaryBytes(file, vertices.data(), vertices.size() * sizeof(CoronaMeshDiskVertex)) ||
			!ReadBinaryBytes(file, indices.data(), indices.size() * sizeof(UINT32)))
		{
			AppendCpuRuntimeTrace(L"[LoadBinaryMeshModel] mesh data read failed: " + binaryFileName);
			return nullptr;
		}

		auto mesh = std::make_shared<Mesh>();
		mesh->Owner = renderBackend.get();
		mesh->transform = glm::mat4x4(1.0f);
		mesh->NumVertices = vertexCount;
		mesh->NumIndices = indexCount;
		mesh->VertexStride = sizeof(CoronaMeshDiskVertex);
		mesh->IndexFormat = DXGI_FORMAT_R32_UINT;
		mesh->Vb = renderBackend->CreateVertexBuffer(static_cast<UINT>(vertices.size() * sizeof(CoronaMeshDiskVertex)), sizeof(CoronaMeshDiskVertex), vertices.data());
		mesh->Ib = renderBackend->CreateIndexBuffer(mesh->IndexFormat, static_cast<UINT>(indices.size() * sizeof(UINT32)), indices.data());
		mesh->CpuPositions.reserve(vertices.size());
		for (const CoronaMeshDiskVertex& vertex : vertices)
			mesh->CpuPositions.push_back(vertex.Position);
		mesh->CpuIndices = indices;

		const uint32_t safeMaterialIndex = materialIndex < scene->Materials.size() ? materialIndex : 0;
		Mesh::DrawCall dc = {};
		dc.IndexCount = indexCount;
		dc.IndexStart = 0;
		dc.VertexBase = 0;
		dc.VertexCount = vertexCount;
		dc.mat = scene->Materials[safeMaterialIndex];
		if (dc.mat->bHasAlpha)
			mesh->bTransparent = true;

		mesh->Draws.push_back(dc);
		scene->meshes.push_back(mesh);
		totalVertices += vertexCount;
		totalIndices += indexCount;
	}

	AppendCpuRuntimeTrace(
		L"[LoadBinaryMeshModel] loaded: " + binaryFileName +
		L", meshes=" + std::to_wstring(header.MeshCount) +
		L", vertices=" + std::to_wstring(totalVertices) +
		L", triangles=" + std::to_wstring(totalIndices / 3) +
		L", elapsedMs=" + std::to_wstring(ElapsedMilliseconds(loadStart, CpuClock::now())));

	return scene;
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

	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
	wstring wide = converter.from_bytes(fileName);
	if (bStartupLoadingScreenActive)
		UpdateStartupLoadingProgress(
			std::max(StartupLoadingProgress, 0.72f),
			L"Loading model: " + GetFileName(wide.c_str()));
	const std::filesystem::path binaryMeshPath = GetCoronaMeshCachePath(wide);
	if (IsCoronaMeshCacheUsable(binaryMeshPath, std::filesystem::path(wide)))
	{
		if (shared_ptr<Scene> binaryScene = LoadBinaryMeshModel(binaryMeshPath.wstring(), wide))
			return binaryScene;
		AppendCpuRuntimeTrace(L"[LoadModel] binary cache rejected, falling back to Assimp: " + binaryMeshPath.wstring());
	}

	wstring dir = GetDirectoryFromFilePath(wide.c_str());
	//wstring dir = L"Sponza/";

	const auto loadStart = CpuClock::now();
	Scene* scene = new Scene;

	Assimp::Importer importer;
	const aiScene* assimpScene = importer.ReadFile(fileName, 0);

	UINT flags = aiProcess_CalcTangentSpace |
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_MakeLeftHanded |
		aiProcess_RemoveRedundantMaterials |
		aiProcess_FlipUVs |
		aiProcess_FlipWindingOrder;

		flags |= aiProcess_PreTransformVertices /*| aiProcess_OptimizeMeshes*/;

	assimpScene = importer.ApplyPostProcessing(flags);
	if (!assimpScene)
	{
		AppendCpuRuntimeTrace(L"[LoadModel] failed: " + wide + L" error=" + AnsiToWString(importer.GetErrorString()));
		return nullptr;
	}

	std::map<std::wstring, shared_ptr<Texture>> textureCache;
	auto loadTextureOrDefault = [&](const wstring& texturePath, bool nonSRGB, const shared_ptr<Texture>& fallback) -> shared_ptr<Texture>
	{
		if (texturePath.empty())
			return fallback;

		try
		{
			const std::wstring textureCacheKey = (nonSRGB ? L"linear|" : L"srgb|") + texturePath;
			auto cachedTexture = textureCache.find(textureCacheKey);
			if (cachedTexture != textureCache.end())
				return cachedTexture->second ? cachedTexture->second : fallback;

			shared_ptr<Texture> texture = renderBackend->CreateTextureFromFile(texturePath, nonSRGB);
			if (texture)
				textureCache[textureCacheKey] = texture;
			return texture ? texture : fallback;
		}
		catch (const std::exception& e)
		{
			AppendCpuRuntimeTrace(L"[LoadModel] texture fallback: " + texturePath + L" error=" + AnsiToWString(e.what()));
			return fallback;
		}
	};

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
			mat->Diffuse = loadTextureOrDefault(dir + wDiffuseTex, false, DefaultWhiteTex);
		}

		if (!mat->Diffuse)
			mat->Diffuse = DefaultWhiteTex;

		if (aiMat.GetTexture(aiTextureType_NORMALS, 0, &normalMapPath) == aiReturn_SUCCESS
			|| aiMat.GetTexture(aiTextureType_HEIGHT, 0, &normalMapPath) == aiReturn_SUCCESS)
			wNormalTex = GetFileName(AnsiToWString(normalMapPath.C_Str()).c_str());

		if (wNormalTex.length() != 0)
		{
			mat->Normal = loadTextureOrDefault(dir + wNormalTex, true, DefaultNormalTex);
		}

		if (!mat->Normal)
			mat->Normal = DefaultNormalTex;


		// aiTextureType_HEIGHT is normal in sponza
		// aiTextureType_AMBIENT is metallic in sponza

		if (aiMat.GetTexture(aiTextureType_AMBIENT, 0, &metallicMapPath) == aiReturn_SUCCESS)
			wMetallicTex = GetFileName(AnsiToWString(metallicMapPath.C_Str()).c_str());
		if (wMetallicTex.length() != 0)
		{
			mat->Metallic = loadTextureOrDefault(dir + wMetallicTex, true, DefaultBlackTex);
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
				mat->Roughness = loadTextureOrDefault(dir + wRoughnessTex, true, DefaultRougnessTex);
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

	for (UINT i = 0; i < numMeshes; ++i)
	{
		aiMesh* asMesh = assimpScene->mMeshes[i];

		Mesh* mesh = new Mesh;
		mesh->Owner = renderBackend.get();
		mesh->transform = glm::mat4x4(1.0f);

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
				const glm::vec3 position(asMesh->mVertices[i].x, asMesh->mVertices[i].y, asMesh->mVertices[i].z);
				vertices[i].Position = position;
				if (!scene->bHasBounds)
				{
					scene->BoundsMin = position;
					scene->BoundsMax = position;
					scene->bHasBounds = true;
				}
				else
				{
					scene->BoundsMin.x = std::min(scene->BoundsMin.x, position.x);
					scene->BoundsMin.y = std::min(scene->BoundsMin.y, position.y);
					scene->BoundsMin.z = std::min(scene->BoundsMin.z, position.z);
					scene->BoundsMax.x = std::max(scene->BoundsMax.x, position.x);
					scene->BoundsMax.y = std::max(scene->BoundsMax.y, position.y);
					scene->BoundsMax.z = std::max(scene->BoundsMax.z, position.z);
				}
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
		mesh->CpuPositions.reserve(vertices.size());
		for (const Vertex& vertex : vertices)
			mesh->CpuPositions.push_back(vertex.Position);
		mesh->CpuIndices = indices;


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
	AppendCpuRuntimeTrace(
		L"[LoadModel] Assimp loaded: " + wide +
		L", meshes=" + std::to_wstring(numMeshes) +
		L", materials=" + std::to_wstring(numMaterials) +
		L", elapsedMs=" + std::to_wstring(ElapsedMilliseconds(loadStart, CpuClock::now())) +
		L", cacheHint=" + binaryMeshPath.wstring());

	return scenePtr;
}

void Corona::InitImgui()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	std::filesystem::create_directories(RuntimePaths::ConfigDirectory());
	std::filesystem::create_directories(RuntimePaths::LogDirectory());
	m_imguiIniPath = WideToUtf8(RuntimePaths::ConfigFile(L"imgui.ini").wstring());
	m_imguiLogPath = WideToUtf8(RuntimePaths::LogFile(L"imgui.log").wstring());
	io.IniFilename = m_imguiIniPath.c_str();
	io.LogFilename = m_imguiLogPath.c_str();

	ImGui_ImplWin32_Init(Win32Application::GetHwnd());
	renderBackend->InitializeImGuiBackend(Win32Application::GetHwnd(), DXGI_FORMAT_R8G8B8A8_UNORM);
}

void Corona::InitBlueNoiseTexture()
{
	const std::filesystem::path path = GetAssetFullPath(L"assets\\bluenoise\\64_64_64\\HDR_RGBA.raw");
	ifstream file(path, ios::in | ios::binary);
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

void Corona::ApplySponzaFlyCamera()
{
	const glm::vec3 position(458.0f, 781.0f, 185.0f);
	const float yaw = 4.4f;
	const float pitch = -0.40f;

	m_camera.m_initialPosition = position;
	m_camera.m_position = position;
	m_camera.m_yaw = yaw;
	m_camera.m_pitch = pitch;
	m_camera.m_upDirection = glm::vec3(0.0f, 1.0f, 0.0f);
	m_camera.m_keysPressed = {};
	m_camera.m_mouseButtonDown = false;
	m_camera.SetMoveSpeed(520.0f);

	const float r = cosf(m_camera.m_pitch);
	m_camera.m_lookDirection.x = r * sinf(m_camera.m_yaw);
	m_camera.m_lookDirection.y = sinf(m_camera.m_pitch);
	m_camera.m_lookDirection.z = r * cosf(m_camera.m_yaw);

	bScriptCameraControlEnabled = false;
	if (SponzaObject != InvalidSceneObjectHandle)
		SetSceneObjectVisibility(SponzaObject, true);

	FrameCounter = 0;
	PathTracingAccumulatedFrames = 0;
	PrevPathTracingViewMat = glm::mat4x4(0.0f);
	PrevPathTracingLightDir = glm::vec3(0.0f);
	PrevPathTracingLightIntensity = 0.0f;
	bTemporalAAHistoryValid = false;
	bTemporalDenoiserHistoryValid = false;
	bResetTemporalStateNextUpdate = true;
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
	std::lock_guard<std::mutex> stateLock(GameRenderStateMutex);
	const auto updateStart = CpuClock::now();
	CpuUpdatePhaseLastTimeMs.fill(0.0f);

	auto phaseStart = CpuClock::now();
	m_timer.Tick(NULL);
	const float elapsedSeconds = static_cast<float>(m_timer.GetElapsedSeconds());

	if (m_frameCounter == 100)
	{
		// Update window text with FPS value.
		wchar_t fps[64];
		swprintf_s(fps, L"%ufps", m_timer.GetFramesPerSecond());
		if (!bSplitGameRenderThreads)
			SetCustomWindowText(fps);
		m_frameCounter = 0;
	}

	m_frameCounter++;

	const glm::vec3 cameraPositionBeforeNativeUpdate = m_camera.m_position;
	const bool bCameraPathDrivesCamera = bCameraPathPlaying || bCameraPathDumping;
	if (!bScriptCameraControlEnabled && !bCameraPathDrivesCamera)
	{
		m_camera.SetTurnSpeed(m_turnSpeed);
		m_camera.Update(elapsedSeconds);
		m_camera.m_position = ResolveCameraPhysicsMovement(cameraPositionBeforeNativeUpdate, m_camera.m_position);
	}
	AddCpuUpdatePhaseTiming(ECpuUpdatePhase::CameraPhysics, phaseStart, CpuClock::now());

	phaseStart = CpuClock::now();
	PollScriptMouseState();
	PollScriptGamepadState();
	AddCpuUpdatePhaseTiming(ECpuUpdatePhase::Input, phaseStart, CpuClock::now());

	phaseStart = CpuClock::now();
	UpdateLuauScripting(elapsedSeconds);
	AddCpuUpdatePhaseTiming(ECpuUpdatePhase::LuauScripts, phaseStart, CpuClock::now());

	phaseStart = CpuClock::now();
	ClearScriptInputFrameState();
	AddCpuUpdatePhaseTiming(ECpuUpdatePhase::Input, phaseStart, CpuClock::now());

	phaseStart = CpuClock::now();
	UpdateCameraPathState();
	AddCpuUpdatePhaseTiming(ECpuUpdatePhase::CameraPath, phaseStart, CpuClock::now());

	phaseStart = CpuClock::now();
	CollectRenderFrameDeltas();
	AddCpuUpdatePhaseTiming(ECpuUpdatePhase::RenderSync, phaseStart, CpuClock::now());

	FinishCpuUpdateTiming(updateStart, CpuClock::now());
}

void Corona::ProcessRenderThreadRequests()
{
	if (bPendingUpscaleRefresh)
	{
		RefreshUpscaleSettings(true);
		bPendingUpscaleRefresh = false;
	}
	if (!RenderResolutionResourcesMatchCurrentState())
	{
		AppendCpuRuntimeTrace(
			L"[ProcessRenderThreadRequests] render resource size mismatch, forcing reload render=" +
			std::to_wstring(GetRenderWidth()) + L"x" + std::to_wstring(GetRenderHeight()) +
			L", display=" + std::to_wstring(m_width) + L"x" + std::to_wstring(m_height));
		bForceUpscaleReload = true;
		RefreshUpscaleSettings(true);
	}

	if (bRecompileShaders)
	{
		RecompileShaders();
		bRecompileShaders = false;
	}
}

void Corona::BuildRenderFrameDerivedState(const RenderFrameSourceState* sourceState)
{
	if (sourceState)
		ApplyRenderFrameSourceState(*sourceState);

	const float effectiveNear = Near;
	const float effectiveFar = Far;

	if (sourceState)
	{
		auto normalizeOrFallback = [](const glm::vec3& value, const glm::vec3& fallback)
		{
			const float length = glm::length(value);
			return length > 0.0001f ? value / length : fallback;
		};

		const glm::vec3 cameraLook = normalizeOrFallback(sourceState->CameraLookDirection, glm::vec3(0.0f, 0.0f, 1.0f));
		glm::vec3 cameraUp = normalizeOrFallback(sourceState->CameraUpDirection, glm::vec3(0.0f, 1.0f, 0.0f));
		if (glm::length(glm::cross(cameraLook, cameraUp)) < 0.0001f)
			cameraUp = std::abs(cameraLook.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

		ViewMat = glm::lookAtRH(sourceState->CameraPosition, sourceState->CameraPosition + cameraLook, cameraUp);
		ProjMat = m_camera.GetProjectionMatrix(Fov, m_aspectRatio, effectiveNear, effectiveFar);
	}
	else
	{
		ViewMat = m_camera.GetViewMatrix();
		ProjMat = m_camera.GetProjectionMatrix(Fov, m_aspectRatio, effectiveNear, effectiveFar);
	}
	UnjitteredProjMat = ProjMat;
	UnjitteredViewProjMat = ProjMat * ViewMat;

	InvViewMat = glm::inverse(ViewMat);
	InvProjMat = glm::inverse(ProjMat);
	UnjitteredInvProjMat = glm::inverse(UnjitteredProjMat);
	FrameProjectionParams.x = effectiveFar / (effectiveFar - effectiveNear);
	FrameProjectionParams.y = effectiveNear / (effectiveNear - effectiveFar);
	FrameProjectionParams.z = effectiveNear;
	FrameProjectionParams.w = effectiveFar;

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
		bScreenProbeGIAtlasHistoryValid = false;
		bScreenProbeGIHistoryValid = false;
		bSpatialHashGIHistoryValid = false;
		bResetTemporalStateNextUpdate = false;
	}

	InvViewProjMat = glm::inverse(ViewProjMat);
	RenderFrameShaderTime = sourceState ? sourceState->TotalSeconds : static_cast<float>(m_timer.GetTotalSeconds());
	RenderFrameShaderTime *= 0.01f;
	RenderFrameNormalizedLightDir = glm::length(LightDir) > 0.0001f ? glm::normalize(LightDir) : glm::vec3(0.0f, 1.0f, 0.0f);
	const float lightDirT = 0.5f * (RenderFrameNormalizedLightDir.y + 1.0f);
	RenderFrameLightColor = glm::mix(SkyColorBottom, SkyColorTop, lightDirT);
	RenderFrameRayNoiseMode = static_cast<UINT32>(RayNoiseMode);
	const bool bDiffuseGIUsesSkyLightingStrength = bEnableSkyLighting && !bEnableRayTracedSkyLighting;
	const bool bDiffuseGIIncludesSkyLighting = !(bEnableSkyLighting && bEnableRayTracedSkyLighting);
	RenderFrameDiffuseGISkyLightingEnabled = bDiffuseGIIncludesSkyLighting ? 1u : 0u;
	RenderFrameDiffuseGISkyIntensity = bDiffuseGIUsesSkyLightingStrength ? SkyIntensity * std::clamp(SkyLightingStrength, 0.0f, 1.0f) : SkyIntensity;
	RenderFrameIndex = FrameCounter;

	const bool indirectLightDirChanged = glm::length(RenderFrameNormalizedLightDir - PrevIndirectAccumLightDir) > 0.0001f;
	const bool indirectLightIntensityChanged = abs(LightIntensity - PrevIndirectAccumLightIntensity) > 0.0001f;
	const bool indirectSkyChanged =
		glm::length(SkyColorTop - PrevIndirectSkyColorTop) > 0.0001f ||
		glm::length(SkyColorBottom - PrevIndirectSkyColorBottom) > 0.0001f ||
		abs(SkyIntensity - PrevIndirectSkyIntensity) > 0.0001f;
	const bool indirectDiffuseGISkyLightingChanged =
		bDiffuseGIIncludesSkyLighting != PrevIndirectDiffuseGISkyLightingEnabled;
	const bool indirectSkyLightingStrengthChanged =
		bDiffuseGIUsesSkyLightingStrength &&
		abs(SkyLightingStrength - PrevIndirectSkyLightingStrength) > 0.0001f;
	const bool indirectPrefilteredEnvChanged =
		abs(PrefilteredEnvRoughnessThreshold - PrevIndirectPrefilteredEnvRoughnessThreshold) > 0.0001f ||
		abs(PrefilteredEnvRoughnessFade - PrevIndirectPrefilteredEnvRoughnessFade) > 0.0001f ||
		bEnablePrefilteredEnvSpecular != PrevIndirectPrefilteredEnvSpecularEnabled;
	const bool indirectLightingChanged =
		indirectLightDirChanged ||
		indirectLightIntensityChanged ||
		indirectSkyChanged ||
		indirectDiffuseGISkyLightingChanged ||
		indirectSkyLightingStrengthChanged ||
		indirectPrefilteredEnvChanged;

	if (indirectLightingChanged)
	{
		if (DiffuseGIMode == EDiffuseGIMode::SPATIAL_HASH)
		{
		}
		else
		{
			IndirectAccumulatedFrames = 0;
			bTemporalDenoiserHistoryValid = false;
			bSpatialHashGIHistoryValid = false;
			if (DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE && (bScreenProbeGIAtlasHistoryValid || bScreenProbeGIHistoryValid))
				bScreenProbeLightingBootstrapPending = true;
			else
			{
				bScreenProbeGIAtlasHistoryValid = false;
				bScreenProbeGIHistoryValid = false;
				bScreenProbeLightingBootstrapPending = false;
			}
		}
		PrevIndirectAccumLightDir = RenderFrameNormalizedLightDir;
		PrevIndirectAccumLightIntensity = LightIntensity;
		PrevIndirectSkyColorTop = SkyColorTop;
		PrevIndirectSkyColorBottom = SkyColorBottom;
		PrevIndirectSkyIntensity = SkyIntensity;
		PrevIndirectSkyLightingStrength = SkyLightingStrength;
		PrevIndirectDiffuseGISkyLightingEnabled = bDiffuseGIIncludesSkyLighting;
		PrevIndirectPrefilteredEnvRoughnessThreshold = PrefilteredEnvRoughnessThreshold;
		PrevIndirectPrefilteredEnvRoughnessFade = PrefilteredEnvRoughnessFade;
		PrevIndirectPrefilteredEnvSpecularEnabled = bEnablePrefilteredEnvSpecular;
	}
	PrevIndirectAccumViewMat = ViewMat;

	if (PathTracingViewParam.DebugMode == 0)
	{
		FrameCounter++;
	}
}

// Render the scene.
void Corona::OnRender()
{
	std::unique_lock<std::mutex> stateLock(GameRenderStateMutex);
	ProcessRenderThreadRequests();
	ApplyPendingRenderFrameDeltas();
	if (bSplitGameRenderThreads && RenderWorld.bHasFrameSourceState)
		BuildRenderFrameDerivedState(&RenderWorld.FrameSourceState);
	else if (!bSplitGameRenderThreads)
		BuildRenderFrameDerivedState(nullptr);

	BeginFramePerfLogging();
	double beginFrameMs = 0.0;
	double executeMs = 0.0;
	double endFrameMs = 0.0;

	FlushSceneObjectChanges();

	const auto beginFrameStart = CpuClock::now();
	renderBackend->BeginFrame();
	beginFrameMs = ElapsedMilliseconds(beginFrameStart, CpuClock::now());
	UpdateGpuTimingReadback();
	BeginGpuTimingFrame();
#if WITH_STREAMLINE
	StreamlineFrameToken = nullptr;
	bStreamlineConstantsSetThisFrame = false;
#endif
	bDLSSRROutputValidThisFrame = false;
	bRTAOOutputValidThisFrame = false;
	bSkyLightingOutputValidThisFrame = false;
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
			AppendCpuRuntimeTrace(
				std::wstring(L"[OnRender] hybrid stage limited by backend to ") +
				GetHybridStageAutoDumpPhaseName(hybridStage));
		}
		const bool bRunShadow = hybridStage >= 1;
		const bool bRunReflection = hybridStage >= 3;
		const bool bRunGI = hybridStage >= 4;
		const bool bRunTemporalDenoise = hybridStage >= 5;
		const bool bRunLighting = hybridStage >= 7;
		const bool bVulkanHybridBackend =
			renderBackend &&
			renderBackend->GetAPI() == ERenderBackendAPI::Vulkan;

		// Hybrid rendering: Rasterization GBuffer + Raytracing
		BeginGpuPassTiming(EGpuPass::GBuffer);
		GBufferPass();
		EndGpuPassTiming(EGpuPass::GBuffer);

		if (bRunShadow)
		{
			BeginGpuPassTiming(EGpuPass::RaytraceShadow);
			RaytraceShadowPass();
			EndGpuPassTiming(EGpuPass::RaytraceShadow);
		}

		if (bRunLighting && bEnableRTAO)
		{
			BeginGpuPassTiming(EGpuPass::RaytraceAO);
			RaytraceAOPass();
			EndGpuPassTiming(EGpuPass::RaytraceAO);
		}

		if (bRunLighting && bEnableSkyLighting && bEnableRayTracedSkyLighting)
		{
			BeginGpuPassTiming(EGpuPass::RaytraceSkyLighting);
			RaytraceSkyLightingPass();
			EndGpuPassTiming(EGpuPass::RaytraceSkyLighting);
		}

		if (bRunReflection)
		{
			BeginGpuPassTiming(EGpuPass::RaytraceReflection);
			RaytraceReflectionPass();
			EndGpuPassTiming(EGpuPass::RaytraceReflection);
		}

		if (bRunGI)
		{
			BeginGpuPassTiming(EGpuPass::RaytraceGI);
			if (DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE)
			{
				ScreenProbeRaytraceGIPass();
			}
			else if (DiffuseGIMode == EDiffuseGIMode::SPATIAL_HASH)
			{
				SpatialHashGIPass();
			}
			else
			{
				RaytraceGIPass();
			}
			EndGpuPassTiming(EGpuPass::RaytraceGI);
		}

		// Simple GI denoising: edge-aware temporal accumulation.
		if (bRunTemporalDenoise)
		{
			BeginGpuPassTiming(EGpuPass::TemporalDenoise);
			TemporalDenoisingPass();
			EndGpuPassTiming(EGpuPass::TemporalDenoise);
		}
		if (bRunGI && DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE)
		{
			BeginGpuPassTiming(EGpuPass::ScreenProbeGI);
			ScreenProbeGIPass();
			EndGpuPassTiming(EGpuPass::ScreenProbeGI);
		}
		// DLSS RR now receives the temporally accumulated GI/specular signal directly.

		if (bRunLighting)
		{
			BeginGpuPassTiming(EGpuPass::Lighting);
			LightingPass();
			EndGpuPassTiming(EGpuPass::Lighting);
		}

		// BloomPass(); // Disabled for hybrid mode

#if WITH_STREAMLINE
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
		else
#endif
		if (bRunLighting)
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

#if WITH_STREAMLINE
		if (IsPathTracingDLSSRREnabled() && PathTracingViewParam.DebugMode == 0)
		{
			BeginGpuPassTiming(EGpuPass::DLSSRR);
			const bool bRRPassed = DLSSRRPass();
			EndGpuPassTiming(EGpuPass::DLSSRR);
			if (!bRRPassed)
			{
				bDLSSRROutputValidThisFrame = false;
				bUseLightingBufferFallbackForToneMap = true;
			}
		}
#endif
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
		AppendCpuRuntimeTrace(L"[OnRender] request stage dump capture");
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
			case 2: previewTexture = ShadowBuffer.get(); break;
			case 3: previewTexture = SpecularGIRaw.get(); break;
			case 4:
				previewTexture =
					(DiffuseGIMode == EDiffuseGIMode::SPATIAL_HASH && DiffuseGIHashCached) ? DiffuseGIHashCached.get() :
					((DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE && ScreenProbeGIResolved) ? ScreenProbeGIResolved.get() : DiffuseGIRaw.get());
				break;
			case 5: previewTexture = (DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE && ScreenProbeGIResolved) ? ScreenProbeGIResolved.get() : DiffuseGITemporal[GIBufferWriteIndex].get(); break;
			case 6: previewTexture = (DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE && ScreenProbeGIResolved) ? ScreenProbeGIResolved.get() : DiffuseGITemporal[GIBufferWriteIndex].get(); break;
			default: previewTexture = nullptr; break;
			}
			vkBackend->PreviewTextureOnWindow(previewTexture ? previewTexture : AlbedoBuffer.get());
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

		char fpsText[64];
		sprintf(fpsText, "FPS : %u fps", m_timer.GetFramesPerSecond());

		const ImVec2 fpsTextPos(10.0f, 8.0f);
		ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
		foregroundDrawList->AddText(ImVec2(fpsTextPos.x + 1.0f, fpsTextPos.y + 1.0f), IM_COL32(0, 0, 0, 180), fpsText);
		foregroundDrawList->AddText(fpsTextPos, IM_COL32(255, 255, 255, 235), fpsText);

		char cullingText[160];
		sprintf_s(
			cullingText,
			"Rendered %llu / %llu objects  |  Frustum %llu  Occlusion %llu",
			static_cast<unsigned long long>(GBufferLastVisibleObjectCount),
			static_cast<unsigned long long>(GBufferLastTotalObjectCount),
			static_cast<unsigned long long>(GBufferLastFrustumCulledObjectCount),
			static_cast<unsigned long long>(GBufferLastOcclusionCulledObjectCount));
		const ImVec2 cullingTextPos(10.0f, 26.0f);
		foregroundDrawList->AddText(ImVec2(cullingTextPos.x + 1.0f, cullingTextPos.y + 1.0f), IM_COL32(0, 0, 0, 180), cullingText);
		foregroundDrawList->AddText(cullingTextPos, IM_COL32(190, 235, 255, 235), cullingText);

		const bool bUseLuauImguiControls = bEnableStartupLuauScript && ScriptState && !ScriptState->Scripts.empty();
		if (bUseLuauImguiControls)
		{
			ImGui::Begin("Hi, Let's traceray!");
			RenderQueuedLuauUi();
			DrawLuauImGui();
			ImGui::End();
		}
		if (!bUseLuauImguiControls)
		{
			char debugText[64];

		ImGui::Begin("Hi, Let's traceray!");
		if (renderBackend)
		{
			ImGui::Text("Render Backend: %s", renderBackend->GetBackendName());
		}
		{
			static ImGuiComboFlags renderingModeComboFlags = 0;
			const char* renderingModeItems[] = {
				"HYBRID (Raster + RT)",
				"PATH TRACING",
			};
			int renderingModeIndex = static_cast<int>(RenderingMode);
			renderingModeIndex = std::clamp(renderingModeIndex, 0, static_cast<int>(IM_ARRAYSIZE(renderingModeItems)) - 1);
			const char* renderingModeCurrent = renderingModeItems[renderingModeIndex];
			if (ImGui::BeginCombo("Rendering Mode", renderingModeCurrent, renderingModeComboFlags))
			{
				for (int n = 0; n < IM_ARRAYSIZE(renderingModeItems); n++)
				{
					bool isSelected = (renderingModeIndex == n);
					if (ImGui::Selectable(renderingModeItems[n], isSelected))
					{
						ApplyRenderingAndAAMode(static_cast<ERenderingMode>(n), AntiAliasingMode);
					}
					if (isSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
		}
		ImGui::Text("Arrow keys : rotate camera imGui\nWASD keys : move camera imGui\nI : show/hide imGui\nB : show/hide buffer visualization\nT : cycle anti-aliasing mode");
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
		{
			static ImGuiComboFlags debugVisualizationComboFlags = 0;
			const char* debugVisualizationItems[] = {
				"SHADOW",
				"WORLD_NORMAL",
				"GEO_NORMAL",
				"DEPTH",
				"RAW_DIFFUSE_GI",
				"RAW_DIFFUSE_GI_AUX",
				"SCREEN_PROBE_DIFFUSE_GI",
				"SCREEN_PROBE_PROBES",
				"SCREEN_PROBE_HISTORY_LENGTH",
				"SCREEN_PROBE_ATLAS_HISTORY_LENGTH",
				"TEMPORAL_FILTERED_DIFFUSE_GI",
				"RESOLVED_DIFFUSE_GI",
				"FINAL_DIFFUSE_GI",
				"ALBEDO",
				"VELOCITY",
				"ROUGNESS_METALLIC",
				"SPECULAR_RAW",
				"TEMPORAL_FILTERED_SPECULAR",
				"BLOOM",
				"SPEC_HISTORY_LENGTH",
				"RTAO",
				"NO_FULLSCREEN",
			};
			int debugVisualizationIndex = std::clamp(
				static_cast<int>(FullscreenDebugBuffer),
				0,
				static_cast<int>(IM_ARRAYSIZE(debugVisualizationItems)) - 1);
			const char* debugVisualizationCurrent = debugVisualizationItems[debugVisualizationIndex];
			if (ImGui::BeginCombo("Visualize Full Screen", debugVisualizationCurrent, debugVisualizationComboFlags))
			{
				for (int n = 0; n < IM_ARRAYSIZE(debugVisualizationItems); n++)
				{
					bool isSelected = (debugVisualizationIndex == n);
					if (ImGui::Selectable(debugVisualizationItems[n], isSelected))
					{
						FullscreenDebugBuffer = (EDebugVisualization)n;
					}
					if (isSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
		}
		ImGui::Separator();

		glm::vec4 test = glm::vec4(0, -0, 0, 1) * glm::transpose(UnjitteredViewProjMat);
		test.x /= test.w;
		test.y /= test.w;
		//test.z /= test.w;
		sprintf(debugText, "test : %f %f %f", test.x, test.y, test.z);
		ImGui::Text(debugText);

		if (ImGui::CollapsingHeader("Capture & Profiling", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::Button("GPU Pass Timings"))
			{
				bShowGpuTimingWindow = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Recompile all shaders"))
				bRecompileShaders = true;

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
		}

		if (ImGui::CollapsingHeader("Camera Path Capture"))
		{
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

		if (bCameraPathListDirty)
			RefreshCameraPathList();

		const bool bCameraPathSelectionBusy = bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping;
		if (bCameraPathSelectionBusy)
			ImGui::BeginDisabled();
		if (ImGui::Button("Refresh Paths"))
		{
			RefreshCameraPathList();
			LastCameraPathStatus = L"Found " + std::to_wstring(static_cast<unsigned long long>(CameraPathEntries.size())) + L" saved camera path(s).";
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(360.0f);
		const std::wstring selectedCameraPathName =
			SelectedCameraPathIndex >= 0 && SelectedCameraPathIndex < static_cast<int>(CameraPathEntries.size()) ?
			CameraPathEntries[SelectedCameraPathIndex].DisplayName :
			L"No saved paths";
		const std::string selectedCameraPathNameUtf8 = WideToUtf8(selectedCameraPathName);
		if (ImGui::BeginCombo("Saved Paths", selectedCameraPathNameUtf8.c_str()))
		{
			for (int entryIndex = 0; entryIndex < static_cast<int>(CameraPathEntries.size()); ++entryIndex)
			{
				const bool bSelected = entryIndex == SelectedCameraPathIndex;
				const std::string entryName = WideToUtf8(CameraPathEntries[entryIndex].DisplayName);
				if (ImGui::Selectable(entryName.c_str(), bSelected))
					SelectedCameraPathIndex = entryIndex;
				if (bSelected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (bCameraPathSelectionBusy)
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
		if (SelectedCameraPathIndex < 0 || bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
			ImGui::BeginDisabled();
		if (ImGui::Button("Load Selected Path"))
		{
			LoadSelectedCameraPath();
		}
		if (SelectedCameraPathIndex < 0 || bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
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

		if (SelectedCameraPathIndex < 0 || bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
			ImGui::BeginDisabled();
		if (ImGui::Button("Play Selected Path"))
		{
			if (LoadSelectedCameraPath())
				StartCameraPathPlayback();
		}
		if (SelectedCameraPathIndex < 0 || bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
			ImGui::EndDisabled();
		ImGui::SameLine();
		if (SelectedCameraPathIndex < 0 || bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
			ImGui::BeginDisabled();
		if (ImGui::Button("Dump Selected Path"))
		{
			if (LoadSelectedCameraPath())
				StartCameraPathDump();
		}
		if (SelectedCameraPathIndex < 0 || bCameraPathRecording || bCameraPathPlaying || bCameraPathDumping)
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
				TrimCpuUpdateTimingHistory();
			}
			ImGui::Separator();
			ImGui::Text(
				"CPU Update: %.3f ms (avg %uF %.3f ms)",
				CpuUpdateLastTimeMs,
				static_cast<unsigned>(CpuUpdateHistoryMs.size()),
				CpuUpdateAverageTimeMs);
			for (UINT phaseIndex = 0; phaseIndex < CpuUpdatePhaseCount; ++phaseIndex)
			{
				ImGui::Text(
					"  %s: %.3f ms (avg %uF %.3f ms)",
					GetCpuUpdatePhaseName(static_cast<ECpuUpdatePhase>(phaseIndex)),
					CpuUpdatePhaseLastTimeMs[phaseIndex],
					static_cast<unsigned>(CpuUpdatePhaseHistoryMs[phaseIndex].size()),
					CpuUpdatePhaseAverageTimeMs[phaseIndex]);
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

		if (ImGui::CollapsingHeader("Scene Objects", ImGuiTreeNodeFlags_DefaultOpen))
		{
		auto drawCenterObjectToggle = [&](
			const char* label,
			const wchar_t* assetPath,
			shared_ptr<Scene>& scene,
			SceneObjectHandle& handle,
			float targetExtent,
			float roughness,
			float metallic,
			bool bOverrideRoughnessMetallic,
			glm::vec3* position,
			glm::vec3* rotationDegrees)
		{
			ImGui::PushID(label);

			auto applyControlledTransform = [&]()
			{
				if (scene && handle != InvalidSceneObjectHandle && position && rotationDegrees)
				{
					SetSceneObjectTransform(
						handle,
						BuildCenteredSceneTransform(scene, targetExtent, *position, *rotationDegrees));
				}
			};

			const bool bInScene = handle != InvalidSceneObjectHandle;
			const std::string buttonLabel = std::string(bInScene ? "Remove " : "Add ") + label;
			if (ImGui::Button(buttonLabel.c_str(), ImVec2(150.0f, 0.0f)))
			{
				if (bInScene)
				{
					RemoveSceneObject(handle);
				}
				else
				{
					if (!scene && assetPath)
						scene = LoadModel(WideToUtf8(GetAssetFullPath(assetPath)));

					const glm::vec3 objectPosition = position ? *position : glm::vec3(0.0f);
					const glm::vec3 objectRotationDegrees = rotationDegrees ? *rotationDegrees : glm::vec3(0.0f);
					handle = AddCenteredSceneObject(
						scene,
						targetExtent,
						roughness,
						metallic,
						bOverrideRoughnessMetallic,
						objectPosition,
						objectRotationDegrees);
				}
			}

			ImGui::SameLine();
			if (!scene)
			{
				ImGui::TextDisabled("not loaded");
			}
			else
			{
				ImGui::Text("%s", handle != InvalidSceneObjectHandle ? "in scene" : "hidden");
			}

			if (position && rotationDegrees)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(180.0f);
				if (ImGui::DragFloat3("Position", &position->x, 1.0f, -1000.0f, 1000.0f, "%.1f"))
					applyControlledTransform();

				ImGui::SameLine();
				ImGui::SetNextItemWidth(180.0f);
				if (ImGui::DragFloat3("Rotation", &rotationDegrees->x, 1.0f, -180.0f, 180.0f, "%.1f deg"))
					applyControlledTransform();
			}

			ImGui::PopID();
		};
		if (!bEnableStartupLuauScript)
		{
			drawCenterObjectToggle(
				"Buddha",
				L"assets\\buddha\\buddha.obj",
				Buddha,
				BuddhaObject,
				260.0f,
				0.65f,
				0.0f,
				false,
				&BuddhaCenterPosition,
				&BuddhaCenterRotationDegrees);
			drawCenterObjectToggle(
				"ShaderBall",
				L"assets\\shaderBall\\shaderBall.fbx",
				ShaderBall,
				ShaderBallObject,
				220.0f,
				0.15f,
				1.0f,
				false,
				&ShaderBallCenterPosition,
				&ShaderBallCenterRotationDegrees);
			drawCenterObjectToggle(
				"Pistol",
				L"assets\\pistol\\pistol.obj",
				Pistol,
				PistolObject,
				280.0f,
				0.55f,
				0.0f,
				false,
				&PistolCenterPosition,
				&PistolCenterRotationDegrees);
		}
		drawCenterObjectToggle(
			"Mirror Cube",
			nullptr,
			MirrorCube,
			MirrorCubeObject,
			90.0f,
			0.0f,
			1.0f,
			true,
			&MirrorCubeCenterPosition,
			&MirrorCubeCenterRotationDegrees);
		RenderQueuedLuauUi();
		DrawLuauImGui();
		}

		if (ImGui::CollapsingHeader("Camera & Anti-Aliasing", ImGuiTreeNodeFlags_DefaultOpen))
		{
		ImGui::SliderFloat("Camera turn speed", &m_turnSpeed, 0.0f, glm::half_pi<float>()*2);
		{
			static const char* AAModes[] = { "Off", "TAA", "DLSS SR", "DLSS RR" };
			int AAModeIndex = static_cast<int>(AntiAliasingMode);
			if (ImGui::Combo("Anti-Aliasing", &AAModeIndex, AAModes, IM_ARRAYSIZE(AAModes)))
			{
				ApplyRenderingAndAAMode(RenderingMode, static_cast<EAntiAliasingMode>(AAModeIndex));
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
				if (ImGui::SliderFloat("DLSS Jitter Phase Scale", &DLSSJitterPhaseScale, 0.25f, 16.0f, "%.2f"))
				{
					ResetAllAccumulationState(false);
				}
				int DLSSJitterPhaseOverrideUI = static_cast<int>(DLSSJitterPhaseCountOverride);
				if (ImGui::SliderInt("DLSS Jitter Phase Override", &DLSSJitterPhaseOverrideUI, 0, 512))
				{
					DLSSJitterPhaseCountOverride = static_cast<UINT32>(DLSSJitterPhaseOverrideUI);
					ResetAllAccumulationState(false);
				}
				if (RenderingMode == ERenderingMode::HYBRID && AntiAliasingMode == EAntiAliasingMode::DLSS_RR)
				{
					if (ImGui::Checkbox("Hybrid RR specular motion vectors", &bEnableHybridRRSpecularMotionVectors))
						ResetAllAccumulationState(false);
					if (ImGui::Checkbox("Hybrid RR specular hit distance", &bEnableHybridRRSpecularHitDistance))
						ResetAllAccumulationState(false);
					if (ImGui::Checkbox("Hybrid RR specular guide ray", &bEnableHybridRRSpecularGuideRay))
						ResetAllAccumulationState(false);
					if (ImGui::SliderFloat("Hybrid RR specular MV scale", &HybridRRSpecularMotionVectorScale, -2.0f, 2.0f, "%.2f"))
						ResetAllAccumulationState(false);
				}
				ImGui::Text("DLSS SR Available: %s", bDLSSAvailable ? "Yes" : "No");
				ImGui::Text("DLSS RR Available: %s", bDLSSRRAvailable ? "Yes" : "No");
				ImGui::Text("Render Resolution: %u x %u", RenderWidth, RenderHeight);
				if (IsDLSSUpscaleEnabled())
					ImGui::Text("DLSS Jitter Phases: %u (auto base %u)", DLSSJitterPhaseCount, DLSSJitterPhaseCountAuto);
			}
			else
			{
				ImGui::Text("DLSS SR Available: No");
				ImGui::Text("DLSS RR Available: No");
			}
#endif
		}
		}
		if (ImGui::CollapsingHeader("Debug Toggles"))
		{
			ImGui::Checkbox("Draw Histogram", &bDrawHistogram);
		}

		// Lighting control options (both Hybrid and Path Tracing)
		if (RenderingMode == ERenderingMode::HYBRID || RenderingMode == ERenderingMode::PATHTRACING)
		{
			if (ImGui::CollapsingHeader("Lighting & GI", ImGuiTreeNodeFlags_DefaultOpen))
			{
			bool bLightingChanged = false;

			if (ImGui::TreeNodeEx("Direct Lighting", ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (ImGui::Checkbox("Enable Direct Diffuse", &bEnableDirectDiffuse)) bLightingChanged = true;
				if (ImGui::Checkbox("Enable Direct Specular", &bEnableDirectSpecular)) bLightingChanged = true;
				if (ImGui::Checkbox("Enable Indirect Specular (GI)", &bEnableSpecularGI)) bLightingChanged = true;
				const bool bRTReflectionSERAvailable =
					renderBackend &&
					renderBackend->GetAPI() == ERenderBackendAPI::D3D12 &&
					bD3D12ShaderModel69Supported;
				if (!bRTReflectionSERAvailable)
					ImGui::BeginDisabled();
				if (ImGui::Checkbox("RT Reflection SER", &bEnableRTReflectionSER))
					bLightingChanged = true;
				if (!bRTReflectionSERAvailable)
					ImGui::EndDisabled();
				if (!bRTReflectionSERAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("Requires DX12 Shader Model 6.9.");
				if (ImGui::SliderFloat("Sun Angular Radius", &RTShadowViewParam.ShadowLightRadius, 0.0f, 0.03f, "%.4f rad"))
					bLightingChanged = true;
				if (RenderingMode == ERenderingMode::HYBRID)
				{
					int shadowSamples = static_cast<int>(RTShadowViewParam.ShadowSampleCount);
					if (ImGui::SliderInt("Hybrid Shadow Samples", &shadowSamples, 1, 16))
					{
						RTShadowViewParam.ShadowSampleCount = static_cast<UINT32>(shadowSamples);
						bLightingChanged = true;
					}
				}
				else if (RenderingMode == ERenderingMode::PATHTRACING)
				{
					int directLightSamples = static_cast<int>(PathTracingViewParam.DirectLightSampleCount);
					if (ImGui::SliderInt("Path Tracing Sun Samples", &directLightSamples, 1, 8))
					{
						PathTracingViewParam.DirectLightSampleCount = static_cast<UINT32>(directLightSamples);
						bLightingChanged = true;
					}
				}
				ImGui::TreePop();
			}

			if (RenderingMode == ERenderingMode::HYBRID)
			{
				if (ImGui::TreeNodeEx("RTAO"))
				{
					if (ImGui::Checkbox("Enable RTAO", &bEnableRTAO))
						bLightingChanged = true;
					if (bEnableRTAO)
					{
						int rtaoSamples = static_cast<int>(RTAOViewParam.SampleCount);
						if (ImGui::SliderInt("RTAO Samples", &rtaoSamples, 1, 16))
						{
							RTAOViewParam.SampleCount = static_cast<UINT32>(rtaoSamples);
							bLightingChanged = true;
						}
						if (ImGui::SliderFloat("RTAO Radius", &RTAOViewParam.Radius, 2.0f, 256.0f, "%.1f"))
							bLightingChanged = true;
						if (ImGui::SliderFloat("RTAO Power", &RTAOViewParam.Power, 0.25f, 4.0f, "%.2f"))
							bLightingChanged = true;
						if (ImGui::SliderFloat("RTAO Normal Bias", &RTAOViewParam.NormalBias, 0.01f, 2.0f, "%.2f"))
							bLightingChanged = true;
					}
					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Sky Lighting", ImGuiTreeNodeFlags_DefaultOpen))
				{
					if (ImGui::Checkbox("Enable Sky Lighting", &bEnableSkyLighting))
						bLightingChanged = true;
					if (bEnableSkyLighting)
					{
						if (ImGui::SliderFloat("Sky Lighting Strength", &SkyLightingStrength, 0.0f, 1.0f, "%.2f"))
							bLightingChanged = true;
						if (ImGui::Checkbox("Ray Traced Sky Pass", &bEnableRayTracedSkyLighting))
							bLightingChanged = true;
						if (bEnableRayTracedSkyLighting)
						{
							int skySamples = static_cast<int>(RTSkyLightingViewParam.SampleCount);
							if (ImGui::SliderInt("Sky Lighting Samples", &skySamples, 1, 32))
							{
								RTSkyLightingViewParam.SampleCount = static_cast<UINT32>(skySamples);
								bLightingChanged = true;
							}
							if (ImGui::SliderFloat("Sky Lighting Ray Length", &RTSkyLightingViewParam.RayLength, 16.0f, 10000.0f, "%.1f"))
								bLightingChanged = true;
							if (ImGui::SliderFloat("Sky Lighting Normal Bias", &RTSkyLightingViewParam.NormalBias, 0.01f, 4.0f, "%.2f"))
								bLightingChanged = true;
							if (ImGui::SliderFloat("Sky Lighting Up Bias", &RTSkyLightingViewParam.SkyUpBias, 0.0f, 1.0f, "%.2f"))
								bLightingChanged = true;
							if (ImGui::SliderFloat("Sky Lighting Direction Power", &RTSkyLightingViewParam.SkyDirectionPower, 0.25f, 8.0f, "%.2f"))
								bLightingChanged = true;
							if (ImGui::SliderFloat("Sky Lighting Min World Y", &RTSkyLightingViewParam.SkyMinWorldY, -0.25f, 0.75f, "%.2f"))
								bLightingChanged = true;
						}
						else
						{
							ImGui::TextDisabled("RT sky pass is skipped; Diffuse GI adds sky miss lighting.");
						}
					}
					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Surface Bounce"))
				{
					if (ImGui::SliderFloat("Surface Bounce Strength", &SurfaceBounceStrength, 0.0f, 1.0f, "%.2f"))
						bLightingChanged = true;
					if (ImGui::SliderFloat("Surface Bounce Saturation", &SurfaceBounceSaturation, 0.0f, 1.0f, "%.2f"))
						bLightingChanged = true;
					ImGui::TreePop();
				}
			}

			if (ImGui::TreeNodeEx("Diffuse GI", ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (ImGui::Checkbox("Enable Diffuse GI", &bEnableDiffuseGI)) bLightingChanged = true;
				const bool bRTDiffuseGISERAvailable =
					renderBackend &&
					renderBackend->GetAPI() == ERenderBackendAPI::D3D12 &&
					bD3D12ShaderModel69Supported;
				if (!bRTDiffuseGISERAvailable)
					ImGui::BeginDisabled();
				if (ImGui::Checkbox("RT Diffuse GI SER", &bEnableRTDiffuseGISER))
					bLightingChanged = true;
				if (!bRTDiffuseGISERAvailable)
					ImGui::EndDisabled();
				if (!bRTDiffuseGISERAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("Requires DX12 Shader Model 6.9.");

				static const char* DiffuseGIModes[] = { "Simple Raytrace", "Spatial Hash", "Screen Probe" };
				int DiffuseGIModeIndex = static_cast<int>(DiffuseGIMode);
				const bool bDiffuseGIMethodAvailable = RenderingMode == ERenderingMode::HYBRID;
				if (!bDiffuseGIMethodAvailable)
				{
					ImGui::BeginDisabled();
				}
				ImGui::SetNextItemWidth(220.0f);
				if (ImGui::Combo("Method##DiffuseGI", &DiffuseGIModeIndex, DiffuseGIModes, IM_ARRAYSIZE(DiffuseGIModes)))
				{
					DiffuseGIMode = static_cast<EDiffuseGIMode>(DiffuseGIModeIndex);
					bLightingChanged = true;
				}
				if (!bDiffuseGIMethodAvailable)
				{
					ImGui::EndDisabled();
					ImGui::TextDisabled("Diffuse GI method selection is used by Hybrid rendering.");
				}
				else if (DiffuseGIMode == EDiffuseGIMode::SCREEN_PROBE)
				{
					int probeSpacing = static_cast<int>(ScreenProbeGICB.ProbeSpacing);
					if (ImGui::SliderInt("Screen Probe Spacing", &probeSpacing, 4, 64))
					{
						ScreenProbeGICB.ProbeSpacing = static_cast<UINT32>(probeSpacing);
						bLightingChanged = true;
					}
					int gatherRadius = static_cast<int>(ScreenProbeGICB.GatherRadius);
					if (ImGui::SliderInt("Screen Probe Gather Radius", &gatherRadius, 1, 3))
					{
						ScreenProbeGICB.GatherRadius = static_cast<UINT32>(gatherRadius);
						bLightingChanged = true;
					}
					int raysPerProbe = static_cast<int>(RTScreenProbeGIViewParam.RaysPerProbe);
					if (ImGui::SliderInt("Screen Probe Rays / Probe", &raysPerProbe, 1, 4))
					{
						RTScreenProbeGIViewParam.RaysPerProbe = static_cast<UINT32>(raysPerProbe);
						bLightingChanged = true;
					}
					static const char* ScreenProbeSHModes[] = { "4 coeffs (L0 + L1)", "9 coeffs (L0 + L1 + L2)" };
					int screenProbeSHMode = ScreenProbeGICB.SHCoefficientCount <= 4u ? 0 : 1;
					if (ImGui::Combo("Screen Probe SH Coefficients", &screenProbeSHMode, ScreenProbeSHModes, IM_ARRAYSIZE(ScreenProbeSHModes)))
					{
						ScreenProbeGICB.SHCoefficientCount = screenProbeSHMode == 0 ? 4u : 9u;
						bLightingChanged = true;
					}
					if (ImGui::SliderFloat("Screen Probe Raw Blend", &ScreenProbeGICB.RawBlend, 0.0f, 0.35f, "%.3f"))
						bLightingChanged = true;
					ImGui::TextDisabled("Screen Probe history converges with a running 1/N radiance average.");
					if (ImGui::SliderFloat("Screen Probe Resolve Depth", &ScreenProbeGICB.ResolveDepthWeight, 1.0f, 96.0f, "%.1f"))
						bLightingChanged = true;
					if (ImGui::SliderFloat("Screen Probe Resolve Normal", &ScreenProbeGICB.ResolveNormalWeight, 1.0f, 96.0f, "%.1f"))
						bLightingChanged = true;
					if (ImGui::SliderFloat("Screen Probe Edge Depth", &ScreenProbeGICB.EdgeDepthWeight, 8.0f, 192.0f, "%.1f"))
						bLightingChanged = true;
					if (ImGui::SliderFloat("Screen Probe Edge Normal", &ScreenProbeGICB.EdgeNormalWeight, 1.0f, 96.0f, "%.1f"))
						bLightingChanged = true;
					int edgeSamples = static_cast<int>(ScreenProbeGICB.EdgeSampleCount);
					if (ImGui::SliderInt("Screen Probe Edge Samples", &edgeSamples, 1, 4))
					{
						ScreenProbeGICB.EdgeSampleCount = static_cast<UINT32>(edgeSamples);
						bLightingChanged = true;
					}
				}
				else if (DiffuseGIMode == EDiffuseGIMode::SPATIAL_HASH)
				{
					if (ImGui::SliderFloat("Spatial Hash Cell Size", &SpatialHashGICB.CellSize, 4.0f, 256.0f))
						bLightingChanged = true;
					int raysPerCell = static_cast<int>(RTSpatialHashGIViewParam.RaysPerCell);
					if (ImGui::SliderInt("Spatial Hash Rays / Cell", &raysPerCell, 1, 8))
					{
						RTSpatialHashGIViewParam.RaysPerCell = static_cast<UINT32>(raysPerCell);
						bLightingChanged = true;
					}
					int maxBounces = static_cast<int>(RTSpatialHashGIViewParam.MaxBounces);
					if (ImGui::SliderInt("Spatial Hash Max Bounces", &maxBounces, 1, 8))
					{
						RTSpatialHashGIViewParam.MaxBounces = static_cast<UINT32>(maxBounces);
						bLightingChanged = true;
					}
					if (ImGui::SliderFloat("Spatial Hash Interpolation", &SpatialHashGICB.InterpolationStrength, 0.0f, 1.0f))
						bLightingChanged = true;
					if (ImGui::SliderFloat("Spatial Hash Smoothing", &SpatialHashGICB.SmoothingStrength, 0.0f, 1.0f))
						bLightingChanged = true;
					if (ImGui::SliderFloat("Spatial Hash Temporal Alpha", &SpatialHashGICB.TemporalAlpha, 0.02f, 1.0f))
						bLightingChanged = true;
				}
				ImGui::TreePop();
			}

			if (ImGui::TreeNodeEx("Ray Sampling"))
			{
				static const char* RayNoiseModes[] = { "Blue Noise", "R2 Low Discrepancy", "Stable Hash" };
				int RayNoiseModeIndex = static_cast<int>(RayNoiseMode);
				if (ImGui::Combo("RT Noise", &RayNoiseModeIndex, RayNoiseModes, IM_ARRAYSIZE(RayNoiseModes)))
				{
					RayNoiseMode = static_cast<ERayNoiseMode>(RayNoiseModeIndex);
					bLightingChanged = true;
				}
				ImGui::TextDisabled("R2 usually converges more calmly with DLSS RR; Blue Noise is kept for comparison.");
				ImGui::TreePop();
			}

			// Lighting toggles only invalidate shading history; they do not require
			// DLSS/RR resource reallocation or render-resolution changes.
			if (bLightingChanged)
			{
				ResetAllAccumulationState(false);
			}
			}
		}

		// Path Tracing settings (only show when in path tracing mode)
		if (RenderingMode == ERenderingMode::PATHTRACING)
		{
			ImGui::Separator();
		ImGui::Text("Path Tracing Settings");
		ImGui::SliderInt("Max Bounces", (int*)&PathTracingViewParam.MaxBounces, 1, 8);
		ImGui::SliderInt("Samples Per Pixel", (int*)&PathTracingViewParam.SamplesPerPixel, 1, 16);
#if WITH_STREAMLINE
		if (ImGui::Checkbox("Primary-hit GBuffer for DLSS RR", &bEnablePathTracingDLSSRR))
		{
			ApplyRenderingAndAAMode(RenderingMode, AntiAliasingMode);
		}
		ImGui::Checkbox("Stabilize moving primary rays for RR", &bEnablePathTracingRRPrimaryRayStabilization);
#endif
	ImGui::Text("Accumulated Frames: %u", PathTracingAccumulatedFrames);
	ImGui::Text("Dispatch SPP: %u", PathTracingLastDispatchSamplesPerPixel);
if (ImGui::Button("Reset Accumulation"))
{
	FrameCounter = 0;
	PathTracingAccumulatedFrames = 0;
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
			PathTracingAccumulatedFrames = 0;
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
		ImGui::Text("Sky Settings");
		ImGui::ColorEdit3("Sky Color Top", &SkyColorTop.x);
		ImGui::ColorEdit3("Sky Color Bottom", &SkyColorBottom.x);
		ImGui::SliderFloat("Sky Intensity", &SkyIntensity, 0.0f, 10.0f);
		if (ImGui::Checkbox("Prefiltered Env Specular", &bEnablePrefilteredEnvSpecular))
			ResetAllAccumulationState(false);
		if (ImGui::SliderFloat("Prefiltered Env Roughness Threshold", &PrefilteredEnvRoughnessThreshold, 0.02f, 1.0f))
			ResetAllAccumulationState(false);
		if (ImGui::SliderFloat("Prefiltered Env Roughness Fade", &PrefilteredEnvRoughnessFade, 0.0f, 0.5f))
			ResetAllAccumulationState(false);

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
		}

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
				ImGui::Text("Row1: SPEC_HISTORY_LENGTH | RTAO | RESOLVED_DIFFUSE_GI | FINAL_DIFFUSE_GI");
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

	const bool bAllowGameUpdateDuringSubmit =
		bSplitGameRenderThreads &&
		!bAutoAADumpEnabled &&
		!bCameraPathRecording &&
		!bCameraPathPlaying &&
		!bCameraPathDumping &&
		!bFinalScreenshotRequested &&
		!bFinalScreenshotCaptureInFlight;
	if (bAllowGameUpdateDuringSubmit)
	{
		PrevViewProjMat = ViewProjMat;
		PrevViewMat = ViewMat;
		PrevUnjitteredViewProjMat = UnjitteredViewProjMat;
		stateLock.unlock();
	}

	const auto executeStart = CpuClock::now();
	renderBackend->ExecuteCurrentCommandList();
	executeMs = ElapsedMilliseconds(executeStart, CpuClock::now());

	const auto endFrameStart = CpuClock::now();
	renderBackend->EndFrame();
	endFrameMs = ElapsedMilliseconds(endFrameStart, CpuClock::now());

	if (!stateLock.owns_lock())
		stateLock.lock();

	FinishFramePerfLogging(beginFrameMs, executeMs, endFrameMs);

	ConsumeCameraPathDumpCaptureResult();
	ConsumeFinalBackbufferScreenshotResult();

	if ((bVulkanStagePreview || bVulkanHybridAutoDump || bAASwitchDumpMode) && bAutoAADumpEnabled && bAutoAADumpInitialized && !AutoAADumpDir.empty())
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

	if (!bAllowGameUpdateDuringSubmit)
	{
		PrevViewProjMat = ViewProjMat;
		PrevViewMat = ViewMat;
		PrevUnjitteredViewProjMat = UnjitteredViewProjMat;
	}

	if (CommandLineExitAfterFrames > 0 && FrameCounter >= CommandLineExitAfterFrames)
	{
		AppendCpuRuntimeTrace(
			L"[OnRender] exit-after-frames reached frame=" + std::to_wstring(FrameCounter) +
			L", target=" + std::to_wstring(CommandLineExitAfterFrames));
		PostQuitMessage(0);
		CommandLineExitAfterFrames = 0;
	}
}

void Corona::StartGameThread()
{
	if (!bSplitGameRenderThreads)
		return;

	{
		std::lock_guard<std::mutex> lock(GameThreadMutex);
		if (bGameThreadStarted)
			return;

		bGameThreadStopRequested = false;
		bGameFrameReady = false;
		bGameThreadStarted = true;
	}

	GameThread = std::thread(&Corona::GameThreadMain, this);
	AppendCpuRuntimeTrace(L"[Threading] game thread started");
}

void Corona::StopGameThread()
{
	{
		std::lock_guard<std::mutex> lock(GameThreadMutex);
		if (!bGameThreadStarted)
			return;

		bGameThreadStopRequested = true;
	}
	GameThreadCv.notify_all();

	if (GameThread.joinable())
		GameThread.join();

	{
		std::lock_guard<std::mutex> lock(GameThreadMutex);
		bGameThreadStarted = false;
		bGameThreadStopRequested = false;
		bGameFrameReady = false;
	}
	AppendCpuRuntimeTrace(L"[Threading] game thread stopped");
}

void Corona::RenderThreadTick()
{
	if (!bSplitGameRenderThreads)
	{
		OnUpdate();
		OnRender();
		return;
	}

	{
		std::unique_lock<std::mutex> lock(GameThreadMutex);
		if (!bGameThreadStarted)
		{
			lock.unlock();
			OnUpdate();
			OnRender();
			return;
		}

		GameThreadCv.wait(lock, [this]()
		{
			return bGameThreadStopRequested || bGameFrameReady;
		});

		if (bGameThreadStopRequested)
			return;

		bGameFrameReady = false;
	}
	GameThreadCv.notify_all();

	OnRender();
}

void Corona::GameThreadMain()
{
	for (;;)
	{
		{
			std::unique_lock<std::mutex> lock(GameThreadMutex);
			GameThreadCv.wait(lock, [this]()
			{
				return bGameThreadStopRequested || !bGameFrameReady;
			});

			if (bGameThreadStopRequested)
				break;
		}

		OnUpdate();

		{
			std::lock_guard<std::mutex> lock(GameThreadMutex);
			if (bGameThreadStopRequested)
				break;
			bGameFrameReady = true;
		}
		GameThreadCv.notify_all();
	}
}

void Corona::OnDestroy()
{
	AppendCpuRuntimeTrace(L"[OnDestroy] begin");
	StopGameThread();
	SaveCameraState();
	SaveSceneState();
	StopAsyncImageDumpWorkers();
	if (!renderBackend)
	{
		CoUninitialize();
		AppendCpuRuntimeTrace(L"[OnDestroy] end no renderBackend");
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
	AppendCpuRuntimeTrace(L"[OnDestroy] end");
}

void Corona::OnKeyDown(UINT8 key)
{
	std::lock_guard<std::mutex> stateLock(GameRenderStateMutex);
	RecordScriptKeyDown(key);

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
		for (int i = 1; i <= static_cast<int>(EAntiAliasingMode::COUNT); ++i)
		{
			const EAntiAliasingMode RequestedMode = static_cast<EAntiAliasingMode>(
				(static_cast<int>(PreviousMode) + i) % static_cast<int>(EAntiAliasingMode::COUNT));
			const EAntiAliasingMode NormalizedMode = NormalizeAntiAliasingMode(RenderingMode, RequestedMode);
			if (NormalizedMode != PreviousMode)
			{
				ApplyRenderingAndAAMode(RenderingMode, NormalizedMode);
				break;
			}
		}
		break;
	}
	case 'C':
		ClampMode++;
		ClampMode = ClampMode % 3;
		break;
	case 'R':
		bRecompileShaders = true;
		break;
	case 'I':
		bShowImgui = !bShowImgui;
		break;
	case VK_F5:
		ReloadLuauScripting();
		break;
	default:
		break;
	}

	if (!bScriptCameraControlEnabled)
		m_camera.OnKeyDown(key);
}

void Corona::OnKeyUp(UINT8 key)
{
	std::lock_guard<std::mutex> stateLock(GameRenderStateMutex);
	RecordScriptKeyUp(key);

	if (!bScriptCameraControlEnabled)
		m_camera.OnKeyUp(key);
}

void Corona::OnRButtonDown(int x, int y)
{
	std::lock_guard<std::mutex> stateLock(GameRenderStateMutex);
	RecordScriptRButtonDown(x, y);

	if (!bScriptCameraControlEnabled)
		m_camera.OnMouseDown(x, y);
}

void Corona::OnRButtonUp()
{
	std::lock_guard<std::mutex> stateLock(GameRenderStateMutex);
	RecordScriptRButtonUp();

	if (!bScriptCameraControlEnabled)
		m_camera.OnMouseUp();
}

void Corona::OnMouseMove(int x, int y)
{
	std::lock_guard<std::mutex> stateLock(GameRenderStateMutex);
	RecordScriptMouseMove(x, y);

	if (!bScriptCameraControlEnabled)
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

// Helper function to add scene meshes to BLAS vector
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
	InitTemporalDenoisingPass();
	InitSpatialHashGIPass();
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
