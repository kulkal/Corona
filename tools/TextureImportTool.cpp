#include <DirectXTex.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
	std::wstring ToLower(std::wstring value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
		return value;
	}

	std::wstring HrToString(HRESULT hr)
	{
		wchar_t buffer[32] = {};
		swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
		return buffer;
	}

	DXGI_FORMAT ParseFormat(const std::wstring& value)
	{
		const std::wstring lower = ToLower(value);
		if (lower == L"bc7")
			return DXGI_FORMAT_BC7_UNORM;
		if (lower == L"bc5")
			return DXGI_FORMAT_BC5_UNORM;
		if (lower == L"bc4")
			return DXGI_FORMAT_BC4_UNORM;
		if (lower == L"rgba8")
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		return DXGI_FORMAT_UNKNOWN;
	}

	int ParseChannel(const std::wstring& value)
	{
		const std::wstring lower = ToLower(value);
		if (lower == L"r" || lower == L"red" || lower == L"0")
			return 0;
		if (lower == L"g" || lower == L"green" || lower == L"1")
			return 1;
		if (lower == L"b" || lower == L"blue" || lower == L"2")
			return 2;
		if (lower == L"a" || lower == L"alpha" || lower == L"3")
			return 3;
		return -1;
	}

	bool IsBlockCompressed(DXGI_FORMAT format)
	{
		return DirectX::IsCompressed(format);
	}

	HRESULT LoadTextureFile(const std::wstring& inputPath, DirectX::ScratchImage& image)
	{
		const std::wstring extension = ToLower(std::filesystem::path(inputPath).extension().wstring());
		if (extension == L".dds")
			return DirectX::LoadFromDDSFile(inputPath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
		if (extension == L".tga")
			return DirectX::LoadFromTGAFile(inputPath.c_str(), nullptr, image);
		return DirectX::LoadFromWICFile(inputPath.c_str(), DirectX::WIC_FLAGS_FORCE_RGB, nullptr, image);
	}

	HRESULT ExtractChannelImage(const DirectX::ScratchImage& rgbaImage, int channel, DirectX::ScratchImage& channelImage)
	{
		const DirectX::TexMetadata metadata = rgbaImage.GetMetadata();
		const DirectX::Image* sourceImage = rgbaImage.GetImage(0, 0, 0);
		if (!sourceImage || sourceImage->format != DXGI_FORMAT_R8G8B8A8_UNORM || channel < 0 || channel > 3)
			return E_INVALIDARG;

		HRESULT hr = channelImage.Initialize2D(DXGI_FORMAT_R8_UNORM, metadata.width, metadata.height, 1, 1);
		if (FAILED(hr))
			return hr;

		const DirectX::Image* targetImage = channelImage.GetImage(0, 0, 0);
		if (!targetImage)
			return E_FAIL;

		for (size_t y = 0; y < metadata.height; ++y)
		{
			const uint8_t* sourceRow = sourceImage->pixels + y * sourceImage->rowPitch;
			uint8_t* targetRow = targetImage->pixels + y * targetImage->rowPitch;
			for (size_t x = 0; x < metadata.width; ++x)
				targetRow[x] = sourceRow[x * 4 + static_cast<size_t>(channel)];
		}
		return S_OK;
	}

	bool ConvertTexture(const std::wstring& inputPath, const std::wstring& outputPath, DXGI_FORMAT outputFormat, bool force, int extractChannel)
	{
		std::error_code ec;
		if (!std::filesystem::exists(inputPath, ec))
		{
			std::wcerr << L"input not found: " << inputPath << L"\n";
			return false;
		}

		if (!force && std::filesystem::exists(outputPath, ec))
		{
			const auto outputTime = std::filesystem::last_write_time(outputPath, ec);
			if (!ec)
			{
				const auto inputTime = std::filesystem::last_write_time(inputPath, ec);
				if (!ec && outputTime >= inputTime)
				{
					std::wcout << L"up to date: " << outputPath << L"\n";
					return true;
				}
			}
		}

		DirectX::ScratchImage source;
		HRESULT hr = LoadTextureFile(inputPath, source);
		if (FAILED(hr))
		{
			std::wcerr << L"load failed: " << inputPath << L" hr=" << HrToString(hr) << L"\n";
			return false;
		}

		const DirectX::TexMetadata sourceMetadata = source.GetMetadata();
		DirectX::ScratchImage working;
		if (DirectX::IsCompressed(sourceMetadata.format))
		{
			hr = DirectX::Decompress(source.GetImages(), source.GetImageCount(), sourceMetadata, DXGI_FORMAT_R8G8B8A8_UNORM, working);
			if (FAILED(hr))
			{
				std::wcerr << L"decompress failed: " << inputPath << L" hr=" << HrToString(hr) << L"\n";
				return false;
			}
		}
		else
		{
			hr = DirectX::Convert(source.GetImages(), source.GetImageCount(), sourceMetadata, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, working);
			if (FAILED(hr))
			{
				const DirectX::Image* sourceImage = source.GetImage(0, 0, 0);
				hr = sourceImage ? working.InitializeFromImage(*sourceImage) : E_INVALIDARG;
				if (FAILED(hr))
				{
					std::wcerr << L"convert failed: " << inputPath << L" hr=" << HrToString(hr) << L"\n";
					return false;
				}
			}
		}

		const DirectX::ScratchImage* mipSource = &working;
		DirectX::ScratchImage channelImage;
		if (extractChannel >= 0)
		{
			hr = ExtractChannelImage(working, extractChannel, channelImage);
			if (FAILED(hr))
			{
				std::wcerr << L"channel extraction failed: " << inputPath << L" hr=" << HrToString(hr) << L"\n";
				return false;
			}
			mipSource = &channelImage;
		}

		DirectX::ScratchImage mipChain;
		hr = DirectX::GenerateMipMaps(mipSource->GetImages(), mipSource->GetImageCount(), mipSource->GetMetadata(), DirectX::TEX_FILTER_DEFAULT, 0, mipChain);
		if (FAILED(hr))
		{
			std::wcerr << L"mip generation failed: " << inputPath << L" hr=" << HrToString(hr) << L"\n";
			return false;
		}

		DirectX::ScratchImage output;
		if (IsBlockCompressed(outputFormat))
		{
			DWORD compressFlags = DirectX::TEX_COMPRESS_PARALLEL;
			if (outputFormat == DXGI_FORMAT_BC7_UNORM)
				compressFlags |= DirectX::TEX_COMPRESS_BC7_QUICK;
			hr = DirectX::Compress(mipChain.GetImages(), mipChain.GetImageCount(), mipChain.GetMetadata(), outputFormat, compressFlags, DirectX::TEX_THRESHOLD_DEFAULT, output);
			if (FAILED(hr))
			{
				std::wcerr << L"compression failed: " << inputPath << L" hr=" << HrToString(hr) << L"\n";
				return false;
			}
		}
		else
		{
			hr = DirectX::Convert(mipChain.GetImages(), mipChain.GetImageCount(), mipChain.GetMetadata(), outputFormat, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, output);
			if (FAILED(hr))
			{
				std::wcerr << L"output conversion failed: " << inputPath << L" hr=" << HrToString(hr) << L"\n";
				return false;
			}
		}

		std::filesystem::create_directories(std::filesystem::path(outputPath).parent_path(), ec);
		if (std::filesystem::exists(outputPath, ec))
			std::filesystem::remove(outputPath, ec);
		hr = DirectX::SaveToDDSFile(output.GetImages(), output.GetImageCount(), output.GetMetadata(), DirectX::DDS_FLAGS_NONE, outputPath.c_str());
		if (FAILED(hr))
		{
			std::wcerr << L"save failed: " << outputPath << L" hr=" << HrToString(hr) << L"\n";
			return false;
		}

		std::wcout << L"wrote " << outputPath << L" format=" << outputFormat << L" mips=" << output.GetMetadata().mipLevels << L"\n";
		return true;
	}

	void PrintUsage()
	{
		std::wcout << L"Usage: CoronaTextureImport.exe --input <path> --output <path> --format <bc7|bc5|bc4|rgba8> [--extract-channel <r|g|b|a>] [--force]\n";
	}
}

int wmain(int argc, wchar_t** argv)
{
	const HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool coInitialized = SUCCEEDED(coInitHr);

	std::wstring inputPath;
	std::wstring outputPath;
	std::wstring formatName = L"bc7";
	int extractChannel = -1;
	bool force = false;

	for (int i = 1; i < argc; ++i)
	{
		const std::wstring arg = argv[i];
		if (arg == L"--input" && i + 1 < argc)
			inputPath = argv[++i];
		else if (arg == L"--output" && i + 1 < argc)
			outputPath = argv[++i];
		else if (arg == L"--format" && i + 1 < argc)
			formatName = argv[++i];
		else if (arg == L"--extract-channel" && i + 1 < argc)
		{
			extractChannel = ParseChannel(argv[++i]);
			if (extractChannel < 0)
			{
				std::wcerr << L"invalid channel: " << argv[i] << L"\n";
				PrintUsage();
				return 2;
			}
		}
		else if (arg == L"--force")
			force = true;
		else if (arg == L"--help" || arg == L"-h")
		{
			PrintUsage();
			return 0;
		}
		else
		{
			std::wcerr << L"unknown argument: " << arg << L"\n";
			PrintUsage();
			return 2;
		}
	}

	const DXGI_FORMAT outputFormat = ParseFormat(formatName);
	if (inputPath.empty() || outputPath.empty() || outputFormat == DXGI_FORMAT_UNKNOWN)
	{
		PrintUsage();
		return 2;
	}

	const bool succeeded = ConvertTexture(inputPath, outputPath, outputFormat, force, extractChannel);
	if (coInitialized)
		CoUninitialize();
	return succeeded ? 0 : 1;
}
