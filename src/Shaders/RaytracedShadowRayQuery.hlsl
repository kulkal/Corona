#define RT_SHADOW_INLINE_RAYQUERY 1
#define RT_SHADOW_INLINE_ENTRY_DECL [numthreads(8, 8, 1)] void main(uint2 pixelPos : SV_DispatchThreadID)
#include "RaytracedShadow.hlsl"
