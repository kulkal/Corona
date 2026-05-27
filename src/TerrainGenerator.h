#pragma once

// Procedural heightmap generator for TerrainComponent.
// Pure CPU, no RHI dependency. Produces a height grid via FBM + ridged
// noise + domain warping over an OpenSimplex2-style 2D gradient noise.

#include <cstdint>
#include <vector>

namespace Terrain
{
	struct GenerateParams
	{
		uint32_t Width = 1024;          // samples (X)
		uint32_t Depth = 1024;          // samples (Z)
		float    WorldScaleXZ = 1.0f;   // world units per sample (m/sample)
		float    HeightMin = 0.0f;      // output Y range bottom
		float    HeightMax = 200.0f;    // output Y range top
		uint32_t Seed = 1;

		// FBM (smooth rolling terrain)
		float    NoiseFrequency = 1.0f / 256.0f;  // cycles per sample
		int      Octaves = 5;
		float    Lacunarity = 2.0f;
		float    Gain = 0.5f;

		// Ridged (mountain spines, mixed in)
		float    RidgeWeight = 0.4f;    // 0 = pure FBM, 1 = pure ridged

		// Domain warping (break up grid alignment)
		float    WarpAmplitude = 30.0f; // sample-space displacement
		float    WarpFrequency = 1.0f / 512.0f;
	};

	struct GenerateStats
	{
		float MinHeight = 0.0f;
		float MaxHeight = 0.0f;
		float MeanHeight = 0.0f;
	};

	// Fill `outHeights` with `Width*Depth` float heights in [HeightMin, HeightMax].
	// Single-threaded; ~50ms for 1024×1024 on modern desktop.
	GenerateStats GenerateHeights(const GenerateParams& params, std::vector<float>& outHeights);

	// Low-level building blocks (exposed for unit tests / reuse).
	float Simplex2D(float x, float y, uint32_t seed);
	float FBM2D(float x, float y, int octaves, float lacunarity, float gain, uint32_t seed);
	float Ridged2D(float x, float y, int octaves, float lacunarity, float gain, uint32_t seed);
}
