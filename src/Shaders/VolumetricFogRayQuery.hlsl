// Sun-shadowed variant of the froxel fog inject pass. Compiled with
// cs_6_5 via InitCSWithInlineRT so VolumetricFogInjectCS can trace
// inline shadow rays toward the sun. See VolumetricFog.hlsl.
#define VOLUMETRIC_FOG_RAYQUERY 1
#include "VolumetricFog.hlsl"
