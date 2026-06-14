#pragma once

// Procedural 24-bone SMPL skinned character.
//
// Build path mirrors Corona.Skeletal.cpp's SpawnSkeletalTestCharacters in
// miniature: one instance, dedicated GPU buffers (no unified-batch
// sharing), SMPL 24-joint hierarchy, single box per limb segment skinned
// rigidly to one bone. Bone matrices are updated per-frame from a
// CoronaMotion::MotionPlayback pose snapshot before the engine's existing
// skeletal compute-skinning pass runs.
//
// Design doc: docs/design/llm_motion_generation_pipeline.md

#include <array>
#include <cstdint>
#include <memory>

#include "Corona.SmplSkeleton.h"
#include "glm/glm.hpp"

class Buffer;
class VertexBuffer;
class IndexBuffer;
class Mesh;
class Scene;
class Material;

namespace CoronaMotion { class MotionPlayback; }

namespace CoronaSmpl
{
	// Owned GPU + scene resources for a single SMPL character. Construction
	// is done via Corona::SpawnSmplMotionCharacter; per-frame updates go
	// through Corona::UpdateSmplMotionCharacterPalette.
	struct CharacterResources
	{
		std::shared_ptr<Buffer>       InputVertices;
		std::shared_ptr<Buffer>       BoneMatrices;
		std::shared_ptr<Buffer>       PrevBoneMatrices;
		std::shared_ptr<Buffer>       BoneMatricesFallback;
		std::shared_ptr<Buffer>       PrevBoneMatricesFallback;
		std::shared_ptr<VertexBuffer> OutputVb;
		std::shared_ptr<VertexBuffer> BindVb;
		std::shared_ptr<IndexBuffer>  Ib;
		std::shared_ptr<Material>     Mat;
		std::shared_ptr<Scene>        ScenePtr;
		std::shared_ptr<Mesh>         MeshPtr;
		std::uint32_t                 VertexCount = 0;
		std::uint32_t                 IndexCount  = 0;

		// Inverse of the bind-pose world transform at each SMPL joint, in
		// object space (no instance scale baked in). Precomputed once at
		// build time and combined with the per-frame posed transforms to
		// derive the mat3x4 palette uploaded to BoneMatrices.
		std::array<glm::mat4x4, kJointCount> BindWorldInverse;
	};
}
