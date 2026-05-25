// Corona.Skeletal.cpp — 3D skeletal skinning entry points.
//
// Fully separate from Corona.Spine.* and SpineSkinningCS.hlsl. A mesh is
// never both bSpineMesh and bSkeletalSkinned. See:
//   docs/gpu_skinning_3d_repurpose_plan.md
//   docs/skeletal_skinning_implementation_progress.md

#include "stdafx.h"
#include "Corona.h"
#include "RenderResources.h"
#include "ComputePipelineStateObject.h"

#include <cmath>
#include <cstring>

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	// Vertex layout written into mesh->Vb and into the compute output buffer.
	// Must match the standard IA layout used by GBuffer.hlsl / GBufferMobile.hlsl
	// (POSITION at offset 0 vec4, TEXCOORD at 16 vec2, NORMAL at 24 vec3,
	// TANGENT at 36 vec3). Stride 48 B.
	struct StandardVertex
	{
		glm::vec4 Position;
		glm::vec2 UV;
		glm::vec3 Normal;
		glm::vec3 Tangent;
	};
	static_assert(sizeof(StandardVertex) == 48, "StandardVertex must match standard IA layout stride");

	// Per-vertex skinning input data fed into the compute shader. SoA-friendly
	// 48 B layout; 4-influence fixed weights.
	struct SkinInputVertex
	{
		glm::vec3 BindPosition;
		float Pad0 = 0.0f;
		glm::vec3 BindNormal;
		float Pad1 = 0.0f;
		glm::vec3 BindTangent;
		float Pad2 = 0.0f;
		glm::vec2 UV;
		UINT32 BoneIndicesPacked = 0;  // 4 x uint8
		float PadEnd = 0.0f;
		glm::vec4 BoneWeights = glm::vec4(0.0f);
	};
	static_assert(sizeof(SkinInputVertex) == 80, "SkinInputVertex layout drift");

	// Bone segment definition for procedural skeleton building. Each segment
	// is a line in bind-pose object space; vertices snap to nearest segments
	// for weight painting.
	struct BoneSegment
	{
		const char* Name;
		int ParentIndex;          // -1 for root
		glm::vec3 HeadLocalOffset;// translation from parent in bind pose
		glm::vec3 TailDirection;  // unit-ish direction (length 1) from head
		float Length;             // along TailDirection
	};

	// Bone names / indices for the procedural humanoid. 17 bones total.
	enum BoxBoneIndex : int
	{
		BONE_ROOT = 0,
		BONE_HIPS,
		BONE_SPINE,
		BONE_NECK,
		BONE_HEAD,
		BONE_SHOULDER_L,
		BONE_UPPER_ARM_L,
		BONE_LOWER_ARM_L,
		BONE_SHOULDER_R,
		BONE_UPPER_ARM_R,
		BONE_LOWER_ARM_R,
		BONE_HIP_L,
		BONE_UPPER_LEG_L,
		BONE_LOWER_LEG_L,
		BONE_HIP_R,
		BONE_UPPER_LEG_R,
		BONE_LOWER_LEG_R,
		BONE_COUNT
	};

	// Body proportions, in object-space units (roughly meters, "1.8m tall").
	constexpr float kTorsoWidth  = 0.50f;
	constexpr float kTorsoDepth  = 0.28f;
	constexpr float kTorsoHeight = 0.70f;
	constexpr float kHeadSize    = 0.26f;
	constexpr float kNeckHeight  = 0.10f;
	constexpr float kArmThick    = 0.13f;
	constexpr float kUpperArmLen = 0.32f;
	constexpr float kLowerArmLen = 0.30f;
	constexpr float kLegThick    = 0.17f;
	constexpr float kUpperLegLen = 0.42f;
	constexpr float kLowerLegLen = 0.40f;
	constexpr float kHipHeight   = 0.95f; // ground to hips
	constexpr float kShoulderY   = kHipHeight + kTorsoHeight;
	constexpr float kShoulderXOff = kTorsoWidth * 0.5f + kArmThick * 0.5f - 0.02f;
	constexpr float kHipXOff      = kTorsoWidth * 0.25f;

	void BuildSkeleton(std::vector<BoneSegment>& outBones)
	{
		outBones.resize(BONE_COUNT);
		auto& B = outBones;
		B[BONE_ROOT]         = { "root",       -1,             glm::vec3(0,        0,           0), glm::vec3(0, 1, 0), 0.0f };
		B[BONE_HIPS]         = { "hips",       BONE_ROOT,      glm::vec3(0,        kHipHeight,  0), glm::vec3(0, 1, 0), 0.05f };
		B[BONE_SPINE]        = { "spine",      BONE_HIPS,      glm::vec3(0,        0.05f,       0), glm::vec3(0, 1, 0), kTorsoHeight - 0.10f };
		B[BONE_NECK]         = { "neck",       BONE_SPINE,     glm::vec3(0,        kTorsoHeight - 0.10f, 0), glm::vec3(0, 1, 0), kNeckHeight };
		B[BONE_HEAD]         = { "head",       BONE_NECK,      glm::vec3(0,        kNeckHeight, 0), glm::vec3(0, 1, 0), kHeadSize };

		B[BONE_SHOULDER_L]   = { "shoulderL",  BONE_SPINE,     glm::vec3( kShoulderXOff, kTorsoHeight - 0.18f, 0), glm::vec3(1, 0, 0), 0.05f };
		B[BONE_UPPER_ARM_L]  = { "upperArmL",  BONE_SHOULDER_L, glm::vec3(0.05f, 0, 0),                          glm::vec3(1, 0, 0), kUpperArmLen };
		B[BONE_LOWER_ARM_L]  = { "lowerArmL",  BONE_UPPER_ARM_L, glm::vec3(kUpperArmLen, 0, 0),                  glm::vec3(1, 0, 0), kLowerArmLen };

		B[BONE_SHOULDER_R]   = { "shoulderR",  BONE_SPINE,     glm::vec3(-kShoulderXOff, kTorsoHeight - 0.18f, 0), glm::vec3(-1, 0, 0), 0.05f };
		B[BONE_UPPER_ARM_R]  = { "upperArmR",  BONE_SHOULDER_R, glm::vec3(-0.05f, 0, 0),                          glm::vec3(-1, 0, 0), kUpperArmLen };
		B[BONE_LOWER_ARM_R]  = { "lowerArmR",  BONE_UPPER_ARM_R, glm::vec3(-kUpperArmLen, 0, 0),                  glm::vec3(-1, 0, 0), kLowerArmLen };

		B[BONE_HIP_L]        = { "hipL",       BONE_HIPS,      glm::vec3( kHipXOff,  0, 0),         glm::vec3(0, -1, 0), 0.02f };
		B[BONE_UPPER_LEG_L]  = { "upperLegL",  BONE_HIP_L,     glm::vec3(0, -0.02f, 0),             glm::vec3(0, -1, 0), kUpperLegLen };
		B[BONE_LOWER_LEG_L]  = { "lowerLegL",  BONE_UPPER_LEG_L, glm::vec3(0, -kUpperLegLen, 0),    glm::vec3(0, -1, 0), kLowerLegLen };

		B[BONE_HIP_R]        = { "hipR",       BONE_HIPS,      glm::vec3(-kHipXOff,  0, 0),         glm::vec3(0, -1, 0), 0.02f };
		B[BONE_UPPER_LEG_R]  = { "upperLegR",  BONE_HIP_R,     glm::vec3(0, -0.02f, 0),             glm::vec3(0, -1, 0), kUpperLegLen };
		B[BONE_LOWER_LEG_R]  = { "lowerLegR",  BONE_UPPER_LEG_R, glm::vec3(0, -kUpperLegLen, 0),    glm::vec3(0, -1, 0), kLowerLegLen };
	}

	// Compute world-space head/tail position for each bone in bind pose by
	// walking the parent chain. Returns parallel arrays sized BONE_COUNT.
	void ComputeBindPoseBoneEndpoints(
		const std::vector<BoneSegment>& bones,
		std::vector<glm::vec3>& outHeads,
		std::vector<glm::vec3>& outTails)
	{
		const int n = static_cast<int>(bones.size());
		outHeads.resize(n);
		outTails.resize(n);
		for (int i = 0; i < n; ++i)
		{
			const glm::vec3 parentHead = (bones[i].ParentIndex >= 0) ? outHeads[bones[i].ParentIndex] : glm::vec3(0.0f);
			outHeads[i] = parentHead + bones[i].HeadLocalOffset;
			outTails[i] = outHeads[i] + bones[i].TailDirection * bones[i].Length;
		}
	}

	// Box part description used to assemble the character. Each box is drawn
	// in object space and shaped along an axis between two bone endpoints
	// for natural skinning alignment.
	struct BoxPart
	{
		glm::vec3 Center;   // box center in object space
		glm::vec3 SizeXYZ;  // full extents along x/y/z
		int       PrimaryBone;   // bone index this part is primarily attached to
		int       SecondaryBone; // for joint blending (lower-arm child gets parent
		                         // upper-arm as secondary, etc.). -1 if none.
		int       SubdivAxis;    // subdivision along the long axis (use Y by
		                         // default for limbs aligned with bones)
		int       SubdivCross;   // subdivision on the cross section
	};

	// Box face direction. Used by AppendBox to emit per-face quads with normals.
	struct CubeFace
	{
		glm::vec3 Normal;
		glm::vec3 Tangent;
		glm::vec3 UAxis;
		glm::vec3 VAxis;
		// faceOrigin = center + Normal * halfThickness;
		float NormalSign; // +1 or -1 along the box axis this face occupies
	};

	// Append a single subdivided box into the running vertex/index/input
	// arrays. Subdivision is uniform across all faces; subdiv = (face_main,
	// face_cross). For a typical limb box the long edge gets more divisions.
	void AppendBox(
		std::vector<StandardVertex>& outVerts,
		std::vector<UINT32>&         outIndices,
		std::vector<SkinInputVertex>& outSkin,
		const BoxPart& part,
		const std::vector<glm::vec3>& boneHeads,
		const std::vector<glm::vec3>& boneTails)
	{
		// Subdivision per axis. We just use (SubdivAxis+1) by (SubdivCross+1)
		// vertices per face regardless of face orientation; the user picks
		// subdivAxis to be the count along the bone's long axis.
		const int Sa = std::max(1, part.SubdivAxis);
		const int Sc = std::max(1, part.SubdivCross);

		const glm::vec3 H = part.SizeXYZ * 0.5f;

		// Six face descriptors with local U/V axes the subdivision walks.
		const CubeFace faces[6] =
		{
			{ glm::vec3( 0,  0,  1), glm::vec3( 1,  0,  0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), +1.0f }, // +Z
			{ glm::vec3( 0,  0, -1), glm::vec3(-1,  0,  0), glm::vec3(-1,0, 0), glm::vec3(0, 1, 0), -1.0f }, // -Z
			{ glm::vec3( 1,  0,  0), glm::vec3( 0,  0, -1), glm::vec3(0, 0,-1), glm::vec3(0, 1, 0), +1.0f }, // +X
			{ glm::vec3(-1,  0,  0), glm::vec3( 0,  0,  1), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0), -1.0f }, // -X
			{ glm::vec3( 0,  1,  0), glm::vec3( 1,  0,  0), glm::vec3(1, 0, 0), glm::vec3(0, 0,-1), +1.0f }, // +Y
			{ glm::vec3( 0, -1,  0), glm::vec3( 1,  0,  0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), -1.0f }, // -Y
		};

		// For each face: produce (Sa+1)*(Sc+1) vertices, then 2*Sa*Sc triangles.
		for (const CubeFace& f : faces)
		{
			const UINT32 base = static_cast<UINT32>(outVerts.size());

			// Compute the face's half-extent contributions. The face is offset
			// from box center by H projected onto the normal axis.
			const glm::vec3 faceCenter = part.Center + f.Normal * glm::dot(H, glm::abs(f.Normal));
			// The U/V axes scale by the box's extent along those local axes.
			const glm::vec3 faceUAxis = f.UAxis * glm::dot(H, glm::abs(f.UAxis));
			const glm::vec3 faceVAxis = f.VAxis * glm::dot(H, glm::abs(f.VAxis));

			for (int j = 0; j <= Sa; ++j)
			{
				const float v = (Sa > 0) ? (static_cast<float>(j) / static_cast<float>(Sa)) : 0.5f;
				for (int i = 0; i <= Sc; ++i)
				{
					const float u = (Sc > 0) ? (static_cast<float>(i) / static_cast<float>(Sc)) : 0.5f;
					const glm::vec3 pos = faceCenter
						+ faceUAxis * (2.0f * u - 1.0f)
						+ faceVAxis * (2.0f * v - 1.0f);
					StandardVertex sv = {};
					sv.Position = glm::vec4(pos, 1.0f);
					sv.UV = glm::vec2(u, v);
					sv.Normal = f.Normal;
					sv.Tangent = f.Tangent;
					outVerts.push_back(sv);

					// Skinning input mirrors the bind pose. Per-vertex weights
					// painted below by distance to bone segments.
					SkinInputVertex siv = {};
					siv.BindPosition = pos;
					siv.BindNormal = f.Normal;
					siv.BindTangent = f.Tangent;
					siv.UV = sv.UV;
					outSkin.push_back(siv);
				}
			}

			// Two triangles per cell.
			const int rowStride = Sc + 1;
			for (int j = 0; j < Sa; ++j)
			{
				for (int i = 0; i < Sc; ++i)
				{
					const UINT32 a = base + static_cast<UINT32>(j * rowStride + i);
					const UINT32 b = base + static_cast<UINT32>(j * rowStride + i + 1);
					const UINT32 c = base + static_cast<UINT32>((j + 1) * rowStride + i + 1);
					const UINT32 d = base + static_cast<UINT32>((j + 1) * rowStride + i);
					outIndices.push_back(a);
					outIndices.push_back(b);
					outIndices.push_back(c);
					outIndices.push_back(a);
					outIndices.push_back(c);
					outIndices.push_back(d);
				}
			}
		}

		(void)boneHeads;
		(void)boneTails;
	}

	float DistancePointToSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
	{
		const glm::vec3 ab = b - a;
		const float lenSq = glm::dot(ab, ab);
		if (lenSq < 1e-8f)
			return glm::length(p - a);
		const float t = glm::clamp(glm::dot(p - a, ab) / lenSq, 0.0f, 1.0f);
		const glm::vec3 proj = a + ab * t;
		return glm::length(p - proj);
	}

	// Paint per-vertex bone weights by inverse distance to bone segments.
	// Picks top 2 nearest bones; weights normalized.
	void PaintWeights(
		std::vector<SkinInputVertex>& skin,
		const std::vector<glm::vec3>& boneHeads,
		const std::vector<glm::vec3>& boneTails,
		const std::vector<float>& boneInfluenceRadius)
	{
		const int boneCount = static_cast<int>(boneHeads.size());
		for (SkinInputVertex& v : skin)
		{
			// Track the 2 nearest bones.
			int bestIdx[2] = { 0, 0 };
			float bestDist[2] = { 1e30f, 1e30f };
			for (int i = 1; i < boneCount; ++i) // skip BONE_ROOT (i=0)
			{
				const float d = DistancePointToSegment(v.BindPosition, boneHeads[i], boneTails[i]);
				const float radius = boneInfluenceRadius[i];
				const float scaled = d / std::max(radius, 1e-3f);
				if (scaled < bestDist[0])
				{
					bestDist[1] = bestDist[0]; bestIdx[1] = bestIdx[0];
					bestDist[0] = scaled;      bestIdx[0] = i;
				}
				else if (scaled < bestDist[1])
				{
					bestDist[1] = scaled;
					bestIdx[1] = i;
				}
			}
			// Convert distances to weights: 1/(eps+d^2). Smoother than 1/d.
			const float w0 = 1.0f / (1e-3f + bestDist[0] * bestDist[0]);
			const float w1 = 1.0f / (1e-3f + bestDist[1] * bestDist[1]);
			const float sum = w0 + w1;
			const float nw0 = w0 / sum;
			const float nw1 = w1 / sum;
			v.BoneIndicesPacked =
				static_cast<UINT32>(bestIdx[0] & 0xFF) |
				(static_cast<UINT32>(bestIdx[1] & 0xFF) << 8);
			v.BoneWeights = glm::vec4(nw0, nw1, 0.0f, 0.0f);
		}
	}

	void BuildBoxParts(std::vector<BoxPart>& outParts)
	{
		// Standing T-pose body, head at +Y, feet at Y=0.
		outParts.clear();

		// Torso (centered on spine between hips and shoulders).
		outParts.push_back(BoxPart{
			glm::vec3(0.0f, kHipHeight + kTorsoHeight * 0.5f, 0.0f),
			glm::vec3(kTorsoWidth, kTorsoHeight, kTorsoDepth),
			BONE_SPINE, BONE_HIPS, 16, 4 });

		// Head.
		outParts.push_back(BoxPart{
			glm::vec3(0.0f, kShoulderY + kNeckHeight + kHeadSize * 0.5f, 0.0f),
			glm::vec3(kHeadSize, kHeadSize, kHeadSize),
			BONE_HEAD, BONE_NECK, 8, 4 });

		// Upper arms (lie along +/- X from shoulder).
		outParts.push_back(BoxPart{
			glm::vec3( kShoulderXOff + kUpperArmLen * 0.5f + 0.05f, kShoulderY - 0.18f, 0.0f),
			glm::vec3(kUpperArmLen, kArmThick, kArmThick),
			BONE_UPPER_ARM_L, BONE_SHOULDER_L, 14, 4 });
		outParts.push_back(BoxPart{
			glm::vec3(-kShoulderXOff - kUpperArmLen * 0.5f - 0.05f, kShoulderY - 0.18f, 0.0f),
			glm::vec3(kUpperArmLen, kArmThick, kArmThick),
			BONE_UPPER_ARM_R, BONE_SHOULDER_R, 14, 4 });

		// Lower arms.
		outParts.push_back(BoxPart{
			glm::vec3( kShoulderXOff + kUpperArmLen + kLowerArmLen * 0.5f + 0.05f, kShoulderY - 0.18f, 0.0f),
			glm::vec3(kLowerArmLen, kArmThick * 0.9f, kArmThick * 0.9f),
			BONE_LOWER_ARM_L, BONE_UPPER_ARM_L, 14, 4 });
		outParts.push_back(BoxPart{
			glm::vec3(-kShoulderXOff - kUpperArmLen - kLowerArmLen * 0.5f - 0.05f, kShoulderY - 0.18f, 0.0f),
			glm::vec3(kLowerArmLen, kArmThick * 0.9f, kArmThick * 0.9f),
			BONE_LOWER_ARM_R, BONE_UPPER_ARM_R, 14, 4 });

		// Upper legs (down along -Y from hips).
		outParts.push_back(BoxPart{
			glm::vec3( kHipXOff, kHipHeight - 0.02f - kUpperLegLen * 0.5f, 0.0f),
			glm::vec3(kLegThick, kUpperLegLen, kLegThick),
			BONE_UPPER_LEG_L, BONE_HIP_L, 16, 4 });
		outParts.push_back(BoxPart{
			glm::vec3(-kHipXOff, kHipHeight - 0.02f - kUpperLegLen * 0.5f, 0.0f),
			glm::vec3(kLegThick, kUpperLegLen, kLegThick),
			BONE_UPPER_LEG_R, BONE_HIP_R, 16, 4 });

		// Lower legs.
		outParts.push_back(BoxPart{
			glm::vec3( kHipXOff, kHipHeight - 0.02f - kUpperLegLen - kLowerLegLen * 0.5f, 0.0f),
			glm::vec3(kLegThick * 0.9f, kLowerLegLen, kLegThick * 0.9f),
			BONE_LOWER_LEG_L, BONE_UPPER_LEG_L, 16, 4 });
		outParts.push_back(BoxPart{
			glm::vec3(-kHipXOff, kHipHeight - 0.02f - kUpperLegLen - kLowerLegLen * 0.5f, 0.0f),
			glm::vec3(kLegThick * 0.9f, kLowerLegLen, kLegThick * 0.9f),
			BONE_LOWER_LEG_R, BONE_UPPER_LEG_R, 16, 4 });
	}

	// Per-bone effective influence radius used to normalize distance->weight
	// painting. Larger bones (torso) get larger radii; thin limbs are tight.
	std::vector<float> BuildBoneInfluenceRadii()
	{
		std::vector<float> radii(BONE_COUNT, 0.2f);
		radii[BONE_HIPS]         = 0.30f;
		radii[BONE_SPINE]        = 0.35f;
		radii[BONE_NECK]         = 0.15f;
		radii[BONE_HEAD]         = 0.20f;
		radii[BONE_SHOULDER_L]   = 0.15f;
		radii[BONE_SHOULDER_R]   = 0.15f;
		radii[BONE_UPPER_ARM_L]  = 0.14f;
		radii[BONE_UPPER_ARM_R]  = 0.14f;
		radii[BONE_LOWER_ARM_L]  = 0.13f;
		radii[BONE_LOWER_ARM_R]  = 0.13f;
		radii[BONE_HIP_L]        = 0.18f;
		radii[BONE_HIP_R]        = 0.18f;
		radii[BONE_UPPER_LEG_L]  = 0.18f;
		radii[BONE_UPPER_LEG_R]  = 0.18f;
		radii[BONE_LOWER_LEG_L]  = 0.16f;
		radii[BONE_LOWER_LEG_R]  = 0.16f;
		return radii;
	}
}

void Corona::InitSkeletalSkinningPSO()
{
	if (!renderBackend)
		return;

	std::shared_ptr<ComputePipelineStateObject> pso = renderBackend->CreateComputePipelineStateObject();
	if (!pso)
	{
		AppendCpuRuntimeTrace(L"[SkeletalSkinningPSO] CreateComputePipelineStateObject failed");
		return;
	}
	pso->BindSRV("Inputs", 0, 1);
	pso->BindSRV("Bones", 1, 1);
	pso->BindUAV("Output", 0);
	pso->BindCBV("Constants", 0, sizeof(SkeletalSkinningConstant));
	if (pso->InitCS(GetAssetFullPath(L"Shaders\\SkeletalSkinningCS.hlsl"), "SkinMain"))
	{
		SkeletalSkinningPSO = pso;
		AppendCpuRuntimeTrace(L"[SkeletalSkinningPSO] InitCS succeeded");
	}
	else
	{
		AppendCpuRuntimeTrace(L"[SkeletalSkinningPSO] InitCS failed");
	}
}

void Corona::DispatchSkeletalSkinningForRenderWorld()
{
	if (!renderBackend || !SkeletalSkinningPSO)
		return;

	// Gather visible skeletal meshes (single pass).
	std::vector<Mesh*> skinnedMeshes;
	skinnedMeshes.reserve(64);
	for (const SceneObject& object : SceneObjects)
	{
		if (!object.bVisible || !object.ScenePtr)
			continue;
		for (const std::shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
		{
			if (mesh && mesh->bSkeletalSkinned &&
				mesh->SkeletalInputVertices &&
				mesh->SkeletalBoneMatrices &&
				mesh->SkeletalOutputVb &&
				mesh->SkeletalVertexCount > 0)
			{
				skinnedMeshes.push_back(mesh.get());
			}
		}
	}
	if (skinnedMeshes.empty())
		return;

	SkeletalStats.CharactersAnimated = static_cast<UINT32>(skinnedMeshes.size());
	SkeletalStats.DispatchCount = 0;
	SkeletalStats.TransitionCount = 0;
	SkeletalStats.VerticesSkinned = 0;
	SkeletalStats.BonesUploaded = 0;
	SkeletalStats.BlasUpdates = 0;

	// Phase 11: ping-pong the output VB so the skeletal GBuffer VS can read
	// the previous frame's skinned positions for accurate per-vertex motion
	// vectors. After swap: SkeletalOutputVb = the buffer that compute will
	// overwrite (was 2-frames-old); SkeletalOutputVbPrev = last frame's
	// output that the VS samples this frame.
	for (Mesh* mesh : skinnedMeshes)
	{
		if (mesh->SkeletalOutputVbPrev)
			std::swap(mesh->SkeletalOutputVb, mesh->SkeletalOutputVbPrev);
	}

	// Batch transitions: SR -> UA for all outputs.
	for (Mesh* mesh : skinnedMeshes)
	{
		renderBackend->TransitionVertexBuffer(mesh->SkeletalOutputVb.get(),
			EResourceState::VertexBuffer, EResourceState::UnorderedAccess);
		++SkeletalStats.TransitionCount;
	}

	SkeletalSkinningPSO->Apply();
	for (Mesh* mesh : skinnedMeshes)
	{
		SkeletalSkinningConstant constants = {};
		constants.VertexCount = mesh->SkeletalVertexCount;
		constants.BoneBase = 0;

		SkeletalSkinningPSO->SetBufferSRV("Inputs", mesh->SkeletalInputVertices.get());
		SkeletalSkinningPSO->SetBufferSRV("Bones", mesh->SkeletalBoneMatrices.get());
		SkeletalSkinningPSO->SetVertexBufferUAV("Output", mesh->SkeletalOutputVb.get());
		SkeletalSkinningPSO->SetCBVValue("Constants", &constants);
		renderBackend->Dispatch((mesh->SkeletalVertexCount + 63u) / 64u, 1, 1);

		mesh->bSkeletalSkinningDispatched = true;
		++SkeletalStats.DispatchCount;
		SkeletalStats.VerticesSkinned += mesh->SkeletalVertexCount;
		SkeletalStats.BonesUploaded += mesh->SkeletalBoneCount;
	}

	// Batch transitions: UA -> VertexBuffer for IA read.
	for (Mesh* mesh : skinnedMeshes)
	{
		renderBackend->TransitionVertexBuffer(mesh->SkeletalOutputVb.get(),
			EResourceState::UnorderedAccess, EResourceState::VertexBuffer);
		++SkeletalStats.TransitionCount;
	}

	// Phase 10: refit BLAS for each skinned mesh so RT passes (reflection,
	// GI, shadow if RT) see the current skinned geometry. The combined
	// VertexBuffer | NON_PIXEL_SHADER_RESOURCE state set by the UA->VB
	// transition above is compatible with BLAS build inputs, so no extra
	// state transitions are needed.
	for (Mesh* mesh : skinnedMeshes)
	{
		if (mesh->SkeletalBlas)
		{
			renderBackend->RefitBLAS(mesh->SkeletalBlas.get(), mesh);
			++SkeletalStats.BlasUpdates;
		}
	}
}

void Corona::SpawnSkeletalTestCharacters()
{
	if (!renderBackend)
		return;

	const UINT32 desiredCount = std::max<UINT32>(1u, CommandLineSkeletalTestCount);

	// Build the shared procedural mesh once. Phase 2 emits per-character
	// meshes (own VB) so the GBuffer path treats each as an independent
	// SceneObject. Optimisation phase will consolidate input SBV.
	std::vector<BoneSegment> bones;
	BuildSkeleton(bones);
	std::vector<glm::vec3> boneHeads, boneTails;
	ComputeBindPoseBoneEndpoints(bones, boneHeads, boneTails);

	std::vector<BoxPart> parts;
	BuildBoxParts(parts);

	std::vector<StandardVertex> vertices;
	std::vector<UINT32> indices;
	std::vector<SkinInputVertex> skin;
	vertices.reserve(4096);
	indices.reserve(8192);
	skin.reserve(4096);

	for (const BoxPart& part : parts)
		AppendBox(vertices, indices, skin, part, boneHeads, boneTails);

	const std::vector<float> radii = BuildBoneInfluenceRadii();
	PaintWeights(skin, boneHeads, boneTails, radii);

	const UINT32 vertexCount = static_cast<UINT32>(vertices.size());
	const UINT32 indexCount = static_cast<UINT32>(indices.size());

	AppendCpuRuntimeTrace(
		L"[SkeletalSpawn] procedural box character vertices=" + std::to_wstring(vertexCount) +
		L", indices=" + std::to_wstring(indexCount) +
		L", parts=" + std::to_wstring(parts.size()) +
		L", bones=" + std::to_wstring(BONE_COUNT) +
		L", spawning=" + std::to_wstring(desiredCount));

	// Shared material — default white with a default normal so lighting is
	// readable on the box surfaces.
	shared_ptr<Material> material = std::make_shared<Material>();
	material->Diffuse = DefaultWhiteTex;
	material->Normal = DefaultNormalTex;
	material->Roughness = DefaultBlackTex;   // shiny enough to read normals
	material->Metallic = DefaultBlackTex;

	// Sponza is in centimeter-ish units (Buddha at extent 260, camera at
	// (458, 781, 185) looking towards (-0.876, -0.389, -0.283)). Place
	// characters in front of the camera at a visible distance. Box character
	// is ~1.8 units tall in object space; scale by ~140 so each character
	// is ~250 units tall (roughly Buddha-statue size) for easy verification.
	const float characterScale = 140.0f;
	const float spacing = 150.0f;
	// Aim point ~500 units along the look direction from the camera, then
	// snap to the Sponza floor (y ≈ -12).
	const glm::vec3 cameraPos(458.0f, 781.0f, 185.0f);
	const glm::vec3 lookDir(-0.876484f, -0.389418f, -0.283072f);
	const glm::vec3 spawnAim = cameraPos + lookDir * 600.0f;
	const glm::vec3 spawnOrigin(spawnAim.x, -12.0f, spawnAim.z);
	const int gridSide = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(desiredCount))));
	const float gridOffset = -0.5f * (gridSide - 1) * spacing;

	for (UINT32 instance = 0; instance < desiredCount; ++instance)
	{
		const int gx = static_cast<int>(instance) % gridSide;
		const int gz = static_cast<int>(instance) / gridSide;
		const float wx = spawnOrigin.x + gridOffset + gx * spacing;
		const float wz = spawnOrigin.z + gridOffset + gz * spacing;
		const glm::mat4x4 instanceTransform =
			glm::translate(glm::mat4x4(1.0f), glm::vec3(wx, spawnOrigin.y, wz)) *
			glm::scale(glm::mat4x4(1.0f), glm::vec3(characterScale));

		std::shared_ptr<Scene> scene = std::make_shared<Scene>();
		scene->Materials.push_back(material);

		auto mesh = std::make_shared<Mesh>();
		mesh->Owner = renderBackend.get();
		mesh->transform = glm::mat4x4(1.0f);
		mesh->NumVertices = vertexCount;
		mesh->NumIndices = indexCount;
		mesh->VertexStride = sizeof(StandardVertex);
		mesh->IndexFormat = EIndexFormat::U32;
		mesh->Mat = material;
		// In Phase 2 we use the bind-pose vertices as the only vertex buffer
		// so the GBuffer path can render the character before compute
		// skinning exists. Phase 5 will swap to SkeletalOutputVb produced by
		// the compute shader.
		mesh->Vb = renderBackend->CreateVertexBuffer(
			static_cast<UINT32>(sizeof(StandardVertex) * vertices.size()),
			sizeof(StandardVertex), vertices.data());
		mesh->Ib = renderBackend->CreateIndexBuffer(
			mesh->IndexFormat,
			static_cast<UINT32>(sizeof(UINT32) * indices.size()),
			indices.data());
		mesh->CpuPositions.reserve(vertices.size());
		for (const StandardVertex& v : vertices)
			mesh->CpuPositions.emplace_back(glm::vec3(v.Position));
		mesh->CpuIndices = indices;

		Mesh::DrawCall drawCall = {};
		drawCall.mat = material;
		drawCall.IndexStart = 0;
		drawCall.IndexCount = indexCount;
		drawCall.VertexBase = 0;
		drawCall.VertexCount = vertexCount;
		mesh->Draws.push_back(drawCall);

		// 3D skeletal fields. SkeletalInputVertices SBV created here; the
		// bone matrix / output VB are populated in later phases.
		mesh->bSkeletalSkinned = true;
		mesh->SkeletalVertexCount = vertexCount;
		mesh->SkeletalBoneCount = static_cast<UINT32>(BONE_COUNT);
		BufferCreateDesc inputDesc = {};
		inputDesc.NumElements = static_cast<UINT32>(skin.size());
		inputDesc.ElementSize = sizeof(SkinInputVertex);
		inputDesc.InitialState = EInitialResourceState::ShaderRead;
		// UAV flag picks the DEFAULT-heap path which is required when
		// CreateBuffer is asked to upload InitialData on the GPU.
		inputDesc.bAllowUnorderedAccess = true;
		inputDesc.InitialData = skin.data();
		inputDesc.Shape = EBufferShape::Structured;
		mesh->SkeletalInputVertices = renderBackend->CreateBuffer(inputDesc);

		// Bone matrix SBV — one mat3x4 (48 B) per bone, persistent-mapped on
		// UPLOAD heap so per-frame UpdateSkeletalTestCharacters can refresh
		// via a single memcpy (no CreateCommittedResource, no staging copy,
		// no WaitGPU per character).
		struct InitialBoneMatrix { float r0[4]; float r1[4]; float r2[4]; };
		static_assert(sizeof(InitialBoneMatrix) == 48, "SkinBone size drift");
		std::vector<InitialBoneMatrix> identityBones(BONE_COUNT);
		for (auto& m : identityBones)
		{
			m.r0[0] = 1.0f; m.r0[1] = 0.0f; m.r0[2] = 0.0f; m.r0[3] = 0.0f;
			m.r1[0] = 0.0f; m.r1[1] = 1.0f; m.r1[2] = 0.0f; m.r1[3] = 0.0f;
			m.r2[0] = 0.0f; m.r2[1] = 0.0f; m.r2[2] = 1.0f; m.r2[3] = 0.0f;
		}
		mesh->SkeletalBoneMatrices = renderBackend->CreateUploadStructuredBuffer(
			static_cast<UINT32>(identityBones.size()), sizeof(InitialBoneMatrix));
		if (mesh->SkeletalBoneMatrices)
		{
			renderBackend->UpdateUploadStructuredBuffer(
				mesh->SkeletalBoneMatrices.get(),
				identityBones.data(),
				static_cast<UINT32>(identityBones.size() * sizeof(InitialBoneMatrix)));
		}

		// RW vertex buffer — compute writes here, GBuffer IA reads it.
		mesh->SkeletalOutputVb = renderBackend->CreateRWVertexBuffer(
			static_cast<UINT32>(sizeof(StandardVertex) * vertices.size()),
			sizeof(StandardVertex));
		// Phase 11: companion ping-pong VB that holds the previous frame's
		// skinning output. Swapped with SkeletalOutputVb at the start of
		// each dispatch so the compute always writes the new frame's output.
		// First-frame contents are uninitialized — motion vector reads will
		// produce garbage for the very first frame and then track correctly.
		mesh->SkeletalOutputVbPrev = renderBackend->CreateRWVertexBuffer(
			static_cast<UINT32>(sizeof(StandardVertex) * vertices.size()),
			sizeof(StandardVertex));

		scene->meshes.push_back(mesh);
		scene->bHasBounds = true;
		scene->BoundsMin = glm::vec3(-kTorsoWidth, 0.0f, -kTorsoDepth);
		scene->BoundsMax = glm::vec3( kTorsoWidth, kShoulderY + kNeckHeight + kHeadSize, kTorsoDepth);

		SceneObjectDesc desc;
		desc.ScenePtr = scene;
		desc.Transform = instanceTransform;
		desc.Roughness = 0.6f;
		desc.Metallic = 0.0f;
		desc.bOverrideRoughnessMetallic = true;
		desc.bRayTracing = true;
		desc.bPhysicsQuery = false;
		(void)AddSceneObject(desc);
	}

	AppendCpuRuntimeTrace(
		L"[SkeletalSpawn] done count=" + std::to_wstring(desiredCount));
}

namespace
{
	// One mat4 per bone; multiply parent * local along the chain to get
	// world. Final skinning matrix = world * inverse(bind_world).
	struct AnimatedBone
	{
		glm::mat4 LocalTransform = glm::mat4(1.0f);
		glm::mat4 WorldTransform = glm::mat4(1.0f);
	};

	// Build a per-frame skinning palette (mat3x4 each) for the procedural
	// box-character skeleton.
	void ComputeSkeletalPalette(float timeSeconds, std::vector<glm::mat4>& outPalette, int instanceIndex)
	{
		std::vector<BoneSegment> bones;
		BuildSkeleton(bones);
		std::vector<glm::vec3> boneHeads, boneTails;
		ComputeBindPoseBoneEndpoints(bones, boneHeads, boneTails);

		// Per-instance phase offset so a crowd doesn't move in lockstep.
		const float phase = static_cast<float>(instanceIndex) * 0.37f;
		const float t = timeSeconds + phase;

		std::vector<AnimatedBone> anim(bones.size());
		for (size_t i = 0; i < bones.size(); ++i)
		{
			// Start from bind-pose local: translate(HeadLocalOffset).
			anim[i].LocalTransform = glm::translate(glm::mat4(1.0f), bones[i].HeadLocalOffset);
		}
		// Apply per-bone procedural rotations around their head joint.
		auto rotateAroundHead = [&](int boneIdx, const glm::vec3& axis, float angle)
		{
			if (boneIdx < 0 || boneIdx >= static_cast<int>(anim.size()))
				return;
			// translate(head) * rotate(axis, angle) instead of just translate(head).
			anim[boneIdx].LocalTransform =
				glm::translate(glm::mat4(1.0f), bones[boneIdx].HeadLocalOffset) *
				glm::rotate(glm::mat4(1.0f), angle, axis);
		};

		rotateAroundHead(BONE_UPPER_ARM_R, glm::vec3(0, 0, 1),  std::sin(t * 2.0f)        * 0.9f);
		rotateAroundHead(BONE_LOWER_ARM_R, glm::vec3(0, 0, 1),  std::sin(t * 2.0f + 1.0f) * 0.7f);
		rotateAroundHead(BONE_UPPER_ARM_L, glm::vec3(0, 0, 1),  std::sin(t * 2.0f + 3.14f) * 0.9f);
		rotateAroundHead(BONE_LOWER_ARM_L, glm::vec3(0, 0, 1),  std::sin(t * 2.0f + 4.14f) * 0.7f);
		rotateAroundHead(BONE_UPPER_LEG_L, glm::vec3(1, 0, 0),  std::sin(t * 1.5f)        * 0.4f);
		rotateAroundHead(BONE_LOWER_LEG_L, glm::vec3(1, 0, 0),  std::max(0.0f, std::sin(t * 1.5f + 0.5f) * 0.4f));
		rotateAroundHead(BONE_UPPER_LEG_R, glm::vec3(1, 0, 0),  std::sin(t * 1.5f + 3.14f) * 0.4f);
		rotateAroundHead(BONE_LOWER_LEG_R, glm::vec3(1, 0, 0),  std::max(0.0f, std::sin(t * 1.5f + 3.64f) * 0.4f));
		rotateAroundHead(BONE_SPINE,       glm::vec3(0, 1, 0),  std::sin(t * 1.0f)        * 0.15f);
		rotateAroundHead(BONE_HEAD,        glm::vec3(0, 1, 0),  std::sin(t * 0.8f)        * 0.25f);

		// Parent -> child accumulation.
		anim[BONE_ROOT].WorldTransform = anim[BONE_ROOT].LocalTransform;
		for (size_t i = 1; i < bones.size(); ++i)
		{
			const int p = bones[i].ParentIndex;
			anim[i].WorldTransform = anim[p].WorldTransform * anim[i].LocalTransform;
		}

		// Final skinning = world * inverse(bind_world). Bind pose is pure
		// translation by boneHeads[i] so the inverse is translate(-head).
		outPalette.resize(bones.size());
		for (size_t i = 0; i < bones.size(); ++i)
		{
			const glm::mat4 bindWorldInv = glm::translate(glm::mat4(1.0f), -boneHeads[i]);
			outPalette[i] = anim[i].WorldTransform * bindWorldInv;
		}
	}
}

void Corona::UpdateSkeletalTestCharacters(float timeSeconds)
{
	if (!renderBackend)
		return;

	int instanceIndex = 0;
	std::vector<glm::mat4> palette;
	struct SkinBoneRow { float r0[4]; float r1[4]; float r2[4]; };
	std::vector<SkinBoneRow> packed;

	for (SceneObject& object : SceneObjects)
	{
		if (!object.ScenePtr)
			continue;
		for (const std::shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
		{
			if (!mesh || !mesh->bSkeletalSkinned)
				continue;

			ComputeSkeletalPalette(timeSeconds, palette, instanceIndex);
			packed.resize(palette.size());
			for (size_t i = 0; i < palette.size(); ++i)
			{
				// glm::mat4 is column-major. We want row-major mat3x4 so that
				// the shader's Row0/Row1/Row2 are the top 3 rows of the
				// transform. transpose() converts to row-major; we then take
				// rows 0..2 (each is 4 floats).
				const glm::mat4 t = glm::transpose(palette[i]);
				packed[i].r0[0] = t[0][0]; packed[i].r0[1] = t[0][1]; packed[i].r0[2] = t[0][2]; packed[i].r0[3] = t[0][3];
				packed[i].r1[0] = t[1][0]; packed[i].r1[1] = t[1][1]; packed[i].r1[2] = t[1][2]; packed[i].r1[3] = t[1][3];
				packed[i].r2[0] = t[2][0]; packed[i].r2[1] = t[2][1]; packed[i].r2[2] = t[2][2]; packed[i].r2[3] = t[2][3];
			}

			// Phase 8: refresh the persistent-mapped UPLOAD-heap buffer
			// created at spawn time with a single memcpy. No new
			// CreateCommittedResource, no command list, no WaitGPU.
			if (mesh->SkeletalBoneMatrices)
			{
				renderBackend->UpdateUploadStructuredBuffer(
					mesh->SkeletalBoneMatrices.get(),
					packed.data(),
					static_cast<UINT32>(packed.size() * sizeof(SkinBoneRow)));
			}

			++instanceIndex;
		}
	}
}

void Corona::DumpSkeletalFrameStatsToTrace()
{
	AppendCpuRuntimeTrace(
		L"[SkeletalStats]"
		L" chars=" + std::to_wstring(SkeletalStats.CharactersAnimated) +
		L" verts=" + std::to_wstring(SkeletalStats.VerticesSkinned) +
		L" bones=" + std::to_wstring(SkeletalStats.BonesUploaded) +
		L" dispatch=" + std::to_wstring(SkeletalStats.DispatchCount) +
		L" transitions=" + std::to_wstring(SkeletalStats.TransitionCount) +
		L" blas=" + std::to_wstring(SkeletalStats.BlasUpdates));
}
