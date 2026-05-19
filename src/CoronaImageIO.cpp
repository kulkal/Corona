#include "CoronaImageIO.h"
#include "RHIBuildConfig.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include "external/stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb/stb_image_write.h"

#if CORONA_HAS_DIRECTXTEX
// DirectXTex is the only loader that handles block-compressed DDS textures used
// by the bundled Sponza assets. Mobile/Linux ports either convert those assets
// to PNG/JPG ahead of time or supply a portable DDS reader.
#include "DirectXTex.h"
#endif

namespace
{
	// stb_image's stdio bypass: read whole file into a buffer first so we can
	// support wchar paths on Windows without depending on stb's narrow fopen.
	bool ReadFileToBuffer(const std::wstring& filePath, std::vector<uint8_t>& out)
	{
#ifdef _WIN32
		FILE* f = nullptr;
		if (_wfopen_s(&f, filePath.c_str(), L"rb") != 0 || !f)
			return false;
#else
		// Best-effort UTF-8 conversion for portable builds.
		std::string narrow;
		narrow.reserve(filePath.size());
		for (wchar_t wc : filePath)
			narrow.push_back(static_cast<char>(wc));
		FILE* f = std::fopen(narrow.c_str(), "rb");
		if (!f)
			return false;
#endif
		std::fseek(f, 0, SEEK_END);
		const long size = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		if (size <= 0)
		{
			std::fclose(f);
			return false;
		}
		out.resize(static_cast<size_t>(size));
		const size_t read = std::fread(out.data(), 1, out.size(), f);
		std::fclose(f);
		return read == out.size();
	}

	bool WriteFileFromBuffer(const std::wstring& filePath, const void* data, size_t size)
	{
#ifdef _WIN32
		FILE* f = nullptr;
		if (_wfopen_s(&f, filePath.c_str(), L"wb") != 0 || !f)
			return false;
#else
		std::string narrow;
		narrow.reserve(filePath.size());
		for (wchar_t wc : filePath)
			narrow.push_back(static_cast<char>(wc));
		FILE* f = std::fopen(narrow.c_str(), "wb");
		if (!f)
			return false;
#endif
		const size_t written = std::fwrite(data, 1, size, f);
		std::fclose(f);
		return written == size;
	}

	struct StbWriteContext
	{
		std::vector<uint8_t> Bytes;
	};

	void StbWriteFunc(void* user, void* data, int size)
	{
		auto* ctx = static_cast<StbWriteContext*>(user);
		const auto* bytes = static_cast<const uint8_t*>(data);
		ctx->Bytes.insert(ctx->Bytes.end(), bytes, bytes + size);
	}

	bool EndsWithI(const std::wstring& path, const wchar_t* suffix)
	{
		const size_t suffixLen = std::wcslen(suffix);
		if (path.size() < suffixLen)
			return false;
		for (size_t i = 0; i < suffixLen; ++i)
		{
			const wchar_t pc = path[path.size() - suffixLen + i];
			const wchar_t sc = suffix[i];
			const wchar_t pcLower = (pc >= L'A' && pc <= L'Z') ? static_cast<wchar_t>(pc + 32) : pc;
			const wchar_t scLower = (sc >= L'A' && sc <= L'Z') ? static_cast<wchar_t>(sc + 32) : sc;
			if (pcLower != scLower)
				return false;
		}
		return true;
	}

	float ClampToUnit(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

	uint16_t Float32ToHalfBits(float f)
	{
		uint32_t bits;
		std::memcpy(&bits, &f, sizeof(bits));
		const uint32_t sign = (bits >> 31) & 0x1;
		int32_t expVal = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
		uint32_t mant = bits & 0x7FFFFFu;
		uint16_t out;
		if (expVal <= 0)
		{
			out = static_cast<uint16_t>(sign << 15);
		}
		else if (expVal >= 31)
		{
			out = static_cast<uint16_t>((sign << 15) | (0x1Fu << 10));
		}
		else
		{
			out = static_cast<uint16_t>((sign << 15) | (static_cast<uint32_t>(expVal) << 10) | (mant >> 13));
		}
		return out;
	}

	float HalfBitsToFloat32(uint16_t h)
	{
		const uint32_t sign = (h >> 15) & 0x1u;
		const uint32_t expVal = (h >> 10) & 0x1Fu;
		const uint32_t mant = h & 0x3FFu;
		uint32_t bits;
		if (expVal == 0)
		{
			bits = sign << 31;
		}
		else if (expVal == 31)
		{
			bits = (sign << 31) | (0xFFu << 23);
		}
		else
		{
			bits = (sign << 31) | ((expVal - 15 + 127) << 23) | (mant << 13);
		}
		float f;
		std::memcpy(&f, &bits, sizeof(f));
		return f;
	}

	// Convert any supported CapturedImage into a tightly packed RGBA8 buffer
	// (sRGB unchanged; HDR inputs are clamped to 0..1). Used by PNG save.
	bool ConvertToRGBA8(const CapturedImage& src, std::vector<uint8_t>& out)
	{
		const size_t pixelCount = static_cast<size_t>(src.Width) * src.Height;
		out.resize(pixelCount * 4);

		switch (src.Format)
		{
		case ETextureFormat::RGBA8Unorm:
			for (uint32_t y = 0; y < src.Height; ++y)
				std::memcpy(out.data() + y * src.Width * 4,
					src.Pixels.data() + y * src.RowPitch,
					src.Width * 4);
			return true;
		case ETextureFormat::BGRA8Unorm:
			for (uint32_t y = 0; y < src.Height; ++y)
			{
				const uint8_t* row = src.Pixels.data() + y * src.RowPitch;
				uint8_t* dst = out.data() + y * src.Width * 4;
				for (uint32_t x = 0; x < src.Width; ++x)
				{
					dst[x * 4 + 0] = row[x * 4 + 2];
					dst[x * 4 + 1] = row[x * 4 + 1];
					dst[x * 4 + 2] = row[x * 4 + 0];
					dst[x * 4 + 3] = row[x * 4 + 3];
				}
			}
			return true;
		case ETextureFormat::RGBA16Float:
			for (uint32_t y = 0; y < src.Height; ++y)
			{
				const uint8_t* row = src.Pixels.data() + y * src.RowPitch;
				uint8_t* dst = out.data() + y * src.Width * 4;
				for (uint32_t x = 0; x < src.Width; ++x)
				{
					const uint16_t* halfPix = reinterpret_cast<const uint16_t*>(row + x * 8);
					for (int c = 0; c < 4; ++c)
					{
						const float v = ClampToUnit(HalfBitsToFloat32(halfPix[c]));
						dst[x * 4 + c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
					}
				}
			}
			return true;
		case ETextureFormat::RGBA32Float:
			for (uint32_t y = 0; y < src.Height; ++y)
			{
				const uint8_t* row = src.Pixels.data() + y * src.RowPitch;
				uint8_t* dst = out.data() + y * src.Width * 4;
				for (uint32_t x = 0; x < src.Width; ++x)
				{
					const float* floats = reinterpret_cast<const float*>(row + x * 16);
					for (int c = 0; c < 4; ++c)
					{
						const float v = ClampToUnit(floats[c]);
						dst[x * 4 + c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
					}
				}
			}
			return true;
		default:
			return false;
		}
	}

	bool ConvertToRGBA32Float(const CapturedImage& src, std::vector<float>& out)
	{
		const size_t pixelCount = static_cast<size_t>(src.Width) * src.Height;
		out.resize(pixelCount * 4);

		switch (src.Format)
		{
		case ETextureFormat::RGBA32Float:
			for (uint32_t y = 0; y < src.Height; ++y)
				std::memcpy(out.data() + y * src.Width * 4,
					src.Pixels.data() + y * src.RowPitch,
					src.Width * 16);
			return true;
		case ETextureFormat::RGBA16Float:
			for (uint32_t y = 0; y < src.Height; ++y)
			{
				const uint8_t* row = src.Pixels.data() + y * src.RowPitch;
				float* dst = out.data() + y * src.Width * 4;
				for (uint32_t x = 0; x < src.Width; ++x)
				{
					const uint16_t* halfPix = reinterpret_cast<const uint16_t*>(row + x * 8);
					for (int c = 0; c < 4; ++c)
						dst[x * 4 + c] = HalfBitsToFloat32(halfPix[c]);
				}
			}
			return true;
		case ETextureFormat::RGBA8Unorm:
		case ETextureFormat::BGRA8Unorm:
			for (uint32_t y = 0; y < src.Height; ++y)
			{
				const uint8_t* row = src.Pixels.data() + y * src.RowPitch;
				float* dst = out.data() + y * src.Width * 4;
				const bool bSwapRB = src.Format == ETextureFormat::BGRA8Unorm;
				for (uint32_t x = 0; x < src.Width; ++x)
				{
					const uint8_t* p = row + x * 4;
					const float fr = (bSwapRB ? p[2] : p[0]) / 255.0f;
					const float fg = p[1] / 255.0f;
					const float fb = (bSwapRB ? p[0] : p[2]) / 255.0f;
					const float fa = p[3] / 255.0f;
					dst[x * 4 + 0] = fr;
					dst[x * 4 + 1] = fg;
					dst[x * 4 + 2] = fb;
					dst[x * 4 + 3] = fa;
				}
			}
			return true;
		default:
			return false;
		}
	}
}

namespace CoronaImageIO
{
	bool Load(const std::wstring& filePath, CapturedImage& out, bool bForceSRGB, std::wstring* errorMessage)
	{
		(void)bForceSRGB; // sRGB decoding is decided at view binding time, not at load.

		std::vector<uint8_t> fileBytes;
		if (!ReadFileToBuffer(filePath, fileBytes))
		{
			if (errorMessage) *errorMessage = L"failed to open image file";
			return false;
		}

		if (EndsWithI(filePath, L".dds"))
		{
#if CORONA_HAS_DIRECTXTEX
			DirectX::ScratchImage scratch;
			DirectX::TexMetadata metadata{};
			HRESULT hr = DirectX::LoadFromDDSMemory(fileBytes.data(), fileBytes.size(), DirectX::DDS_FLAGS_NONE, &metadata, scratch);
			if (FAILED(hr))
			{
				if (errorMessage) *errorMessage = L"DDS load failed";
				return false;
			}
			const DirectX::Image* image = scratch.GetImage(0, 0, 0);
			if (!image)
			{
				if (errorMessage) *errorMessage = L"DDS image had no first mip";
				return false;
			}
			// DDS files can be compressed (BC*). We only forward already-uncompressed
			// pixel data; compressed DDS is uploaded as-is by the DX12 path and not
			// surfaced through CoronaImageIO.
			if (DirectX::IsCompressed(image->format))
			{
				if (errorMessage) *errorMessage = L"compressed DDS not supported by CoronaImageIO";
				return false;
			}
			out.Width = static_cast<uint32_t>(image->width);
			out.Height = static_cast<uint32_t>(image->height);
			out.RowPitch = static_cast<uint32_t>(image->rowPitch);
			out.Pixels.assign(image->pixels, image->pixels + image->slicePitch);
			// Map the DXGI format down to ETextureFormat where possible. The
			// renderer's existing DX12 path consumes DDS directly so this branch
			// is only for non-DDS callers that happen to point at a .dds.
			switch (image->format)
			{
			case DXGI_FORMAT_R8G8B8A8_UNORM:        out.Format = ETextureFormat::RGBA8Unorm; break;
			case DXGI_FORMAT_B8G8R8A8_UNORM:        out.Format = ETextureFormat::BGRA8Unorm; break;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:    out.Format = ETextureFormat::RGBA16Float; break;
			case DXGI_FORMAT_R32G32B32A32_FLOAT:    out.Format = ETextureFormat::RGBA32Float; break;
			default:
				if (errorMessage) *errorMessage = L"DDS format not mapped to ETextureFormat";
				return false;
			}
			return true;
#else
			if (errorMessage) *errorMessage = L"DDS not supported in this build (CORONA_HAS_DIRECTXTEX=0)";
			return false;
#endif
		}

		if (EndsWithI(filePath, L".hdr"))
		{
			int w = 0, h = 0, n = 0;
			float* pixels = stbi_loadf_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &w, &h, &n, 4);
			if (!pixels)
			{
				if (errorMessage) *errorMessage = L"HDR decode failed";
				return false;
			}
			out.Format = ETextureFormat::RGBA32Float;
			out.Width = static_cast<uint32_t>(w);
			out.Height = static_cast<uint32_t>(h);
			out.RowPitch = static_cast<uint32_t>(w) * 16u;
			out.Pixels.assign(reinterpret_cast<uint8_t*>(pixels), reinterpret_cast<uint8_t*>(pixels) + static_cast<size_t>(w) * h * 16);
			stbi_image_free(pixels);
			return true;
		}

		int w = 0, h = 0, n = 0;
		stbi_uc* pixels = stbi_load_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &w, &h, &n, 4);
		if (!pixels)
		{
			if (errorMessage) *errorMessage = L"image decode failed";
			return false;
		}
		out.Format = ETextureFormat::RGBA8Unorm;
		out.Width = static_cast<uint32_t>(w);
		out.Height = static_cast<uint32_t>(h);
		out.RowPitch = static_cast<uint32_t>(w) * 4u;
		out.Pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
		stbi_image_free(pixels);
		return true;
	}

	bool SavePNG(const CapturedImage& image, const std::wstring& filePath, std::wstring* errorMessage)
	{
		if (image.IsEmpty())
		{
			if (errorMessage) *errorMessage = L"empty image";
			return false;
		}
		std::vector<uint8_t> rgba;
		if (!ConvertToRGBA8(image, rgba))
		{
			if (errorMessage) *errorMessage = L"PNG: unsupported source format";
			return false;
		}
		StbWriteContext ctx;
		const int stride = static_cast<int>(image.Width) * 4;
		const int ok = stbi_write_png_to_func(&StbWriteFunc, &ctx,
			static_cast<int>(image.Width), static_cast<int>(image.Height), 4,
			rgba.data(), stride);
		if (!ok)
		{
			if (errorMessage) *errorMessage = L"PNG encode failed";
			return false;
		}
		std::filesystem::create_directories(std::filesystem::path(filePath).parent_path());
		if (!WriteFileFromBuffer(filePath, ctx.Bytes.data(), ctx.Bytes.size()))
		{
			if (errorMessage) *errorMessage = L"PNG file write failed";
			return false;
		}
		return true;
	}

	bool SaveHDR(const CapturedImage& image, const std::wstring& filePath, std::wstring* errorMessage)
	{
		if (image.IsEmpty())
		{
			if (errorMessage) *errorMessage = L"empty image";
			return false;
		}
		std::vector<float> rgba32;
		if (!ConvertToRGBA32Float(image, rgba32))
		{
			if (errorMessage) *errorMessage = L"HDR: unsupported source format";
			return false;
		}
		StbWriteContext ctx;
		const int ok = stbi_write_hdr_to_func(&StbWriteFunc, &ctx,
			static_cast<int>(image.Width), static_cast<int>(image.Height), 4,
			rgba32.data());
		if (!ok)
		{
			if (errorMessage) *errorMessage = L"HDR encode failed";
			return false;
		}
		std::filesystem::create_directories(std::filesystem::path(filePath).parent_path());
		if (!WriteFileFromBuffer(filePath, ctx.Bytes.data(), ctx.Bytes.size()))
		{
			if (errorMessage) *errorMessage = L"HDR file write failed";
			return false;
		}
		return true;
	}
}
