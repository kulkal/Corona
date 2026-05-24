// Corona.Skeletal.cpp — 3D skeletal skinning entry points.
//
// Fully separate from Corona.Spine.* and SpineSkinningCS.hlsl. A mesh is
// never both bSpineMesh and bSkeletalSkinned. See:
//   docs/gpu_skinning_3d_repurpose_plan.md
//   docs/skeletal_skinning_implementation_progress.md
//
// Phase 1: stub implementations only. Later phases will add procedural
// box-character generation, compute-shader dispatch, animation update,
// and benchmarks.

#include "Corona.h"

void Corona::InitSkeletalSkinningPSO()
{
	// Phase 3 fills this in.
}

void Corona::DispatchSkeletalSkinningForRenderWorld()
{
	// Phase 4 fills this in.
}

void Corona::SpawnSkeletalTestCharacters()
{
	// Phase 2 fills this in.
}

void Corona::UpdateSkeletalTestCharacters(float /*timeSeconds*/)
{
	// Phase 6 fills this in.
}

void Corona::DumpSkeletalFrameStatsToTrace()
{
	// Phase 6 fills this in.
}
