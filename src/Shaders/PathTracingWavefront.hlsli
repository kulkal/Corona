#ifndef CORONA_PATH_TRACING_WAVEFRONT_HLSLI
#define CORONA_PATH_TRACING_WAVEFRONT_HLSLI

// One live path for the wavefront path tracing experiment.
// Must stay 64 bytes to match PathTracingCompactionStateStrideBytes.
struct PathTracingWavefrontState
{
    float3 Origin;
    uint Seed;
    float3 Direction;
    uint PixelIndex;
    float3 Throughput;
    uint Bounce;
    float3 Radiance;
    uint Flags;
};

static const uint PATH_TRACING_WAVEFRONT_FLAG_ACTIVE = 1u << 0;
static const uint PATH_TRACING_WAVEFRONT_FLAG_PRIMARY_HIT = 1u << 1;
static const uint PATH_TRACING_WAVEFRONT_FLAG_WRITE_RR_GBUFFER = 1u << 2;

struct PathTracingWavefrontCounters
{
    uint CurrentCount;
    uint NextCount;
    uint CompletedCount;
    uint Padding;
};

#endif

