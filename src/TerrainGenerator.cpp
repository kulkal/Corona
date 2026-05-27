#include "TerrainGenerator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Terrain
{
namespace
{
	// 2D simplex noise — classic Ken Perlin formulation, simplified for a single
	// scalar output. Returns roughly [-1, 1].
	//
	// Reference: https://en.wikipedia.org/wiki/Simplex_noise (2D case)
	// Seeded by hashing the integer simplex corner with a per-call seed.

	inline uint32_t HashCorner(int i, int j, uint32_t seed)
	{
		uint32_t h = seed + 0x9E3779B9u;
		h ^= static_cast<uint32_t>(i) * 0x85EBCA77u;
		h = (h << 13) | (h >> 19);
		h ^= static_cast<uint32_t>(j) * 0xC2B2AE3Du;
		h = (h << 17) | (h >> 15);
		h *= 0x27D4EB2Fu;
		h ^= h >> 16;
		return h;
	}

	inline void Gradient2(uint32_t h, float& gx, float& gy)
	{
		// 8 cardinal + diagonal unit vectors; pick via low bits of hash.
		static const float kG[8][2] = {
			{ 1.0f,  0.0f}, {-1.0f,  0.0f}, { 0.0f,  1.0f}, { 0.0f, -1.0f},
			{ 0.70710678f,  0.70710678f}, {-0.70710678f,  0.70710678f},
			{ 0.70710678f, -0.70710678f}, {-0.70710678f, -0.70710678f}
		};
		const uint32_t idx = h & 7u;
		gx = kG[idx][0];
		gy = kG[idx][1];
	}

	constexpr float F2 = 0.36602540378443864676f; // (sqrt(3) - 1) / 2
	constexpr float G2 = 0.21132486540518711775f; // (3 - sqrt(3)) / 6

	inline float Contribution(int i, int j, float x, float y, uint32_t seed)
	{
		float t = 0.5f - x * x - y * y;
		if (t < 0.0f) return 0.0f;
		float gx, gy;
		Gradient2(HashCorner(i, j, seed), gx, gy);
		t *= t;
		return t * t * (gx * x + gy * y);
	}
}

float Simplex2D(float x, float y, uint32_t seed)
{
	const float s = (x + y) * F2;
	const float xs = x + s;
	const float ys = y + s;
	const int i = static_cast<int>(std::floor(xs));
	const int j = static_cast<int>(std::floor(ys));

	const float t = static_cast<float>(i + j) * G2;
	const float x0 = x - (static_cast<float>(i) - t);
	const float y0 = y - (static_cast<float>(j) - t);

	int i1, j1;
	if (x0 > y0) { i1 = 1; j1 = 0; }
	else         { i1 = 0; j1 = 1; }

	const float x1 = x0 - static_cast<float>(i1) + G2;
	const float y1 = y0 - static_cast<float>(j1) + G2;
	const float x2 = x0 - 1.0f + 2.0f * G2;
	const float y2 = y0 - 1.0f + 2.0f * G2;

	const float n0 = Contribution(i, j, x0, y0, seed);
	const float n1 = Contribution(i + i1, j + j1, x1, y1, seed);
	const float n2 = Contribution(i + 1, j + 1, x2, y2, seed);

	// Scale to ~[-1, 1]. 70 is the standard normalization for simplex 2D.
	return 70.0f * (n0 + n1 + n2);
}

float FBM2D(float x, float y, int octaves, float lacunarity, float gain, uint32_t seed)
{
	float sum = 0.0f;
	float amp = 1.0f;
	float norm = 0.0f;
	float freq = 1.0f;
	for (int o = 0; o < octaves; ++o)
	{
		sum += amp * Simplex2D(x * freq, y * freq, seed + static_cast<uint32_t>(o) * 1013u);
		norm += amp;
		amp *= gain;
		freq *= lacunarity;
	}
	return norm > 0.0f ? sum / norm : 0.0f;
}

float Ridged2D(float x, float y, int octaves, float lacunarity, float gain, uint32_t seed)
{
	float sum = 0.0f;
	float amp = 1.0f;
	float norm = 0.0f;
	float freq = 1.0f;
	for (int o = 0; o < octaves; ++o)
	{
		const float s = Simplex2D(x * freq, y * freq, seed + 7919u + static_cast<uint32_t>(o) * 1013u);
		const float r = 1.0f - std::fabs(s); // ridges where noise crosses zero
		sum += amp * r * r;                  // square sharpens ridges
		norm += amp;
		amp *= gain;
		freq *= lacunarity;
	}
	return norm > 0.0f ? sum / norm : 0.0f; // [0, 1]
}

GenerateStats GenerateHeights(const GenerateParams& params, std::vector<float>& outHeights)
{
	const uint32_t W = params.Width;
	const uint32_t D = params.Depth;
	outHeights.assign(static_cast<size_t>(W) * D, 0.0f);

	const float fbmW = std::clamp(1.0f - params.RidgeWeight, 0.0f, 1.0f);
	const float ridW = std::clamp(params.RidgeWeight, 0.0f, 1.0f);
	const float warpSeedOffset = 1.0f;

	float minH = std::numeric_limits<float>::infinity();
	float maxH = -std::numeric_limits<float>::infinity();
	double sumH = 0.0;

	for (uint32_t z = 0; z < D; ++z)
	{
		for (uint32_t x = 0; x < W; ++x)
		{
			// Domain warp: displace sample point by a separate low-freq noise.
			const float wx = static_cast<float>(x);
			const float wz = static_cast<float>(z);
			const float warpX = Simplex2D(wx * params.WarpFrequency, wz * params.WarpFrequency, params.Seed + 13u) * params.WarpAmplitude;
			const float warpZ = Simplex2D(wx * params.WarpFrequency + warpSeedOffset, wz * params.WarpFrequency + warpSeedOffset, params.Seed + 17u) * params.WarpAmplitude;

			const float px = (wx + warpX) * params.NoiseFrequency;
			const float pz = (wz + warpZ) * params.NoiseFrequency;

			const float fbm  = 0.5f * (FBM2D(px, pz, params.Octaves, params.Lacunarity, params.Gain, params.Seed) + 1.0f); // [0, 1]
			const float ridg = Ridged2D(px, pz, params.Octaves, params.Lacunarity, params.Gain, params.Seed);              // [0, 1]
			float h01 = fbmW * fbm + ridW * ridg;

			h01 = std::clamp(h01, 0.0f, 1.0f);
			const float h = params.HeightMin + h01 * (params.HeightMax - params.HeightMin);
			outHeights[static_cast<size_t>(z) * W + x] = h;

			minH = std::min(minH, h);
			maxH = std::max(maxH, h);
			sumH += h;
		}
	}

	GenerateStats stats{};
	stats.MinHeight = minH;
	stats.MaxHeight = maxH;
	stats.MeanHeight = static_cast<float>(sumH / static_cast<double>(W * D));
	return stats;
}

}
