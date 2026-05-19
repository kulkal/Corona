#pragma once

// Cross-platform image load/save facade. Goes through stb_image / stb_image_write
// for PNG/JPG/TGA/HDR formats. DDS (block-compressed) routes to DirectXTex when
// CORONA_HAS_DIRECTXTEX is enabled, and is reported as unsupported otherwise.
//
// The renderer's texture I/O paths consume `CapturedImage` (from RenderBackend.h)
// as the canonical raw-pixel container so the load side and the capture side
// share a single in-memory representation.

#include <cstdint>
#include <string>
#include "RenderBackend.h"

namespace CoronaImageIO
{
	// Loads any supported file into a CapturedImage. The returned image always
	// has 4 channels per pixel; backends can decide to discard the alpha. The
	// `bForceSRGB` hint enables sRGB-aware decoding when supported by the
	// underlying loader (currently a no-op since stb_image returns raw bytes
	// and the GPU view decides on sRGB).
	//
	// Returns false on error; `errorMessage` (if non-null) carries a human
	// readable description.
	bool Load(const std::wstring& filePath, CapturedImage& out, bool bForceSRGB, std::wstring* errorMessage = nullptr);

	// 32-bit RGBA8 PNG save. `image.Format` is interpreted; only RGBA8Unorm /
	// BGRA8Unorm are written without conversion. Floating point inputs are
	// tone-clamped to 0..1 then converted to 8-bit before write.
	bool SavePNG(const CapturedImage& image, const std::wstring& filePath, std::wstring* errorMessage = nullptr);

	// 32-bit float-per-channel Radiance HDR save. Floating point inputs are
	// passed through; integer inputs are promoted to float in 0..1 range.
	bool SaveHDR(const CapturedImage& image, const std::wstring& filePath, std::wstring* errorMessage = nullptr);
}
