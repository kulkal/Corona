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

		// Subdivisions tuned so the whole character lands near 2 000 triangles
		// (representative of casual / mid-tier far-LOD mobile characters where
		// CPU skinning is most viable). Each part's triangle count is
		// SubdivAxis * SubdivCross * 12, so the totals here come to roughly
		// 10 parts * ~190 tris ~= 1900 tris.
		constexpr int kBodySa = 8;
		constexpr int kBodySc = 2;
		constexpr int kHeadSa = 4;
		constexpr int kHeadSc = 3;
		constexpr int kLimbSa = 8;
		constexpr int kLimbSc = 2;

		// Torso (centered on spine between hips and shoulders).
		outParts.push_back(BoxPart{
			glm::vec3(0.0f, kHipHeight + kTorsoHeight * 0.5f, 0.0f),
			glm::vec3(kTorsoWidth, kTorsoHeight, kTorsoDepth),
			BONE_SPINE, BONE_HIPS, kBodySa, kBodySc });

		// Head.
		outParts.push_back(BoxPart{
			glm::vec3(0.0f, kShoulderY + kNeckHeight + kHeadSize * 0.5f, 0.0f),
			glm::vec3(kHeadSize, kHeadSize, kHeadSize),
			BONE_HEAD, BONE_NECK, kHeadSa, kHeadSc });

		// Upper arms (lie along +/- X from shoulder).
		outParts.push_back(BoxPart{
			glm::vec3( kShoulderXOff + kUpperArmLen * 0.5f + 0.05f, kShoulderY - 0.18f, 0.0f),
			glm::vec3(kUpperArmLen, kArmThick, kArmThick),
			BONE_UPPER_ARM_L, BONE_SHOULDER_L, kLimbSa, kLimbSc });
		outParts.push_back(BoxPart{
			glm::vec3(-kShoulderXOff - kUpperArmLen * 0.5f - 0.05f, kShoulderY - 0.18f, 0.0f),
			glm::vec3(kUpperArmLen, kArmThick, kArmThick),
			BONE_UPPER_ARM_R, BONE_SHOULDER_R, kLimbSa, kLimbSc });

		// Lower arms.
		outParts.push_back(BoxPart{
			glm::vec3( kShoulderXOff + kUpperArmLen + kLowerArmLen * 0.5f + 0.05f, kShoulderY - 0.18f, 0.0f),
			glm::vec3(kLowerArmLen, kArmThick * 0.9f, kArmThick * 0.9f),
			BONE_LOWER_ARM_L, BONE_UPPER_ARM_L, kLimbSa, kLimbSc });
		outParts.push_back(BoxPart{
			glm::vec3(-kShoulderXOff - kUpperArmLen - kLowerArmLen * 0.5f - 0.05f, kShoulderY - 0.18f, 0.0f),
			glm::vec3(kLowerArmLen, kArmThick * 0.9f, kArmThick * 0.9f),
			BONE_LOWER_ARM_R, BONE_UPPER_ARM_R, kLimbSa, kLimbSc });

		// Upper legs (down along -Y from hips).
		outParts.push_back(BoxPart{
			glm::vec3( kHipXOff, kHipHeight - 0.02f - kUpperLegLen * 0.5f, 0.0f),
			glm::vec3(kLegThick, kUpperLegLen, kLegThick),
			BONE_UPPER_LEG_L, BONE_HIP_L, kLimbSa, kLimbSc });
		outParts.push_back(BoxPart{
			glm::vec3(-kHipXOff, kHipHeight - 0.02f - kUpperLegLen * 0.5f, 0.0f),
			glm::vec3(kLegThick, kUpperLegLen, kLegThick),
			BONE_UPPER_LEG_R, BONE_HIP_R, kLimbSa, kLimbSc });

		// Lower legs.
		outParts.push_back(BoxPart{
			glm::vec3( kHipXOff, kHipHeight - 0.02f - kUpperLegLen - kLowerLegLen * 0.5f, 0.0f),
			glm::vec3(kLegThick * 0.9f, kLowerLegLen, kLegThick * 0.9f),
			BONE_LOWER_LEG_L, BONE_UPPER_LEG_L, kLimbSa, kLimbSc });
		outParts.push_back(BoxPart{
			glm::vec3(-kHipXOff, kHipHeight - 0.02f - kUpperLegLen - kLowerLegLen * 0.5f, 0.0f),
			glm::vec3(kLegThick * 0.9f, kLowerLegLen, kLegThick * 0.9f),
			BONE_LOWER_LEG_R, BONE_UPPER_LEG_R, kLimbSa, kLimbSc });
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

	// LLM-driven SMPL character: refresh its bone palette from the current
	// MotionPlayback pose before the generic skeletal compute dispatch
	// runs. The SMPL mesh has bSkeletalSkinned=true, so it gets picked up
	// by the same gathering loop below without any path-specific glue.
	UpdateSmplMotionCharacterPalette();

	// Path C: VS inline skinning. No compute, no CPU skin pass, no
	// upload VB — the GBuffer VS does all the skinning math itself.
	if (bSkeletalUseVsInlineSkinning)
	{
		SkeletalStats.CharactersAnimated = SkeletalUnifiedCharCount;
		SkeletalStats.DispatchCount = 0;
		SkeletalStats.TransitionCount = 0;
		SkeletalStats.VerticesSkinned = 0;
		SkeletalStats.BonesUploaded = SkeletalUnifiedCharCount * SkeletalUnifiedBoneCount;
		SkeletalStats.BlasUpdates = 0;
		// Mark meshes dispatched so the GBuffer path treats them as
		// "ready" (some code branches still gate on this flag).
		for (SceneObject& object : SceneObjects)
		{
			if (!object.ScenePtr) continue;
			for (const std::shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
				if (mesh && mesh->bSkeletalSkinned) mesh->bSkeletalSkinningDispatched = true;
		}
		return;
	}

	// Phase S: CPU-skinning benchmark path. Skip the GPU compute pre-pass
	// and skin every character on the CPU into a fresh UPLOAD-heap VB
	// the GBuffer draw will read instead of SkeletalUnifiedOutputVb.
	// BLAS refit is also skipped — RT visuals will reflect the bind pose
	// while this mode is active.
	if (bSkeletalUseCpuSkinning)
	{
		CpuSkinSkeletalCharactersForRenderWorld();
		SkeletalStats.CharactersAnimated = SkeletalUnifiedCharCount;
		SkeletalStats.DispatchCount = 0;
		SkeletalStats.TransitionCount = 0;
		SkeletalStats.VerticesSkinned = SkeletalUnifiedCharCount * SkeletalUnifiedVertsPerChar;
		SkeletalStats.BonesUploaded = SkeletalUnifiedCharCount * SkeletalUnifiedBoneCount;
		SkeletalStats.BlasUpdates = 0;
		// Mark every skeletal mesh as "skinning ready" so the GBuffer
		// path picks the skeletal PSO + the CPU-skinned VB instead of
		// falling back to the static bind VB.
		for (SceneObject& object : SceneObjects)
		{
			if (!object.ScenePtr) continue;
			for (const std::shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
				if (mesh && mesh->bSkeletalSkinned) mesh->bSkeletalSkinningDispatched = true;
		}
		return;
	}

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

	// Phase A: every visible skeletal mesh aliases the unified output VB.
	// One transition + one dispatch covers all characters. Verify all
	// meshes share the expected unified output VB; bail to per-mesh path
	// otherwise (e.g. user adds a non-unified skeletal mesh later).
	const bool bAllUnified = SkeletalUnifiedOutputVb && SkeletalUnifiedBoneMatrices &&
		SkeletalUnifiedInputVertices &&
		std::all_of(skinnedMeshes.begin(), skinnedMeshes.end(), [&](Mesh* m) {
			return m->SkeletalOutputVb == SkeletalUnifiedOutputVb &&
				m->SkeletalInputVertices == SkeletalUnifiedInputVertices &&
				m->SkeletalBoneMatrices == SkeletalUnifiedBoneMatrices;
		});

	if (!bAllUnified)
	{
		// Fallback: legacy per-mesh dispatch loop. Kept so future mesh
		// templates that haven't migrated to the unified buffers still
		// render correctly.
		for (Mesh* mesh : skinnedMeshes)
		{
			renderBackend->TransitionVertexBuffer(mesh->SkeletalOutputVb.get(),
				EResourceState::VertexBuffer, EResourceState::UnorderedAccess);
			++SkeletalStats.TransitionCount;

			// Single-character mesh: shader still uses the same compute
			// path but with VertsPerChar = full vertex count, so charIndex
			// resolves to 0 inside the dispatch.
			SkeletalSkinningConstant constants = {};
			constants.TotalVertexCount = mesh->SkeletalVertexCount;
			constants.VertsPerChar = mesh->SkeletalVertexCount;
			constants.BoneCount = mesh->SkeletalBoneCount;
			SkeletalSkinningPSO->SetBufferSRV("Inputs", mesh->SkeletalInputVertices.get());
			SkeletalSkinningPSO->SetBufferSRV("Bones", mesh->SkeletalBoneMatrices.get());
			SkeletalSkinningPSO->SetVertexBufferUAV("Output", mesh->SkeletalOutputVb.get());
			SkeletalSkinningPSO->SetCBVValue("Constants", &constants);
			SkeletalSkinningPSO->Apply();
			renderBackend->Dispatch((mesh->SkeletalVertexCount + 63u) / 64u, 1, 1);

			mesh->bSkeletalSkinningDispatched = true;
			++SkeletalStats.DispatchCount;
			SkeletalStats.VerticesSkinned += mesh->SkeletalVertexCount;
			SkeletalStats.BonesUploaded += mesh->SkeletalBoneCount;

			renderBackend->TransitionVertexBuffer(mesh->SkeletalOutputVb.get(),
				EResourceState::UnorderedAccess, EResourceState::VertexBuffer);
			++SkeletalStats.TransitionCount;
		}
	}
	else
	{
		// Phase A fast path: single dispatch covers every character.
		renderBackend->TransitionVertexBuffer(SkeletalUnifiedOutputVb.get(),
			EResourceState::VertexBuffer, EResourceState::UnorderedAccess);
		++SkeletalStats.TransitionCount;

		const UINT32 totalVertexCount = SkeletalUnifiedCharCount * SkeletalUnifiedVertsPerChar;
		SkeletalSkinningConstant constants = {};
		constants.TotalVertexCount = totalVertexCount;
		constants.VertsPerChar = SkeletalUnifiedVertsPerChar;
		constants.BoneCount = SkeletalUnifiedBoneCount;
		SkeletalSkinningPSO->SetBufferSRV("Inputs", SkeletalUnifiedInputVertices.get());
		SkeletalSkinningPSO->SetBufferSRV("Bones", SkeletalUnifiedBoneMatrices.get());
		SkeletalSkinningPSO->SetVertexBufferUAV("Output", SkeletalUnifiedOutputVb.get());
		SkeletalSkinningPSO->SetCBVValue("Constants", &constants);
		SkeletalSkinningPSO->Apply();
		renderBackend->Dispatch((totalVertexCount + 63u) / 64u, 1, 1);

		for (Mesh* mesh : skinnedMeshes)
		{
			mesh->bSkeletalSkinningDispatched = true;
			SkeletalStats.VerticesSkinned += mesh->SkeletalVertexCount;
			SkeletalStats.BonesUploaded += mesh->SkeletalBoneCount;
		}
		SkeletalStats.DispatchCount = 1;

		renderBackend->TransitionVertexBuffer(SkeletalUnifiedOutputVb.get(),
			EResourceState::UnorderedAccess, EResourceState::VertexBuffer);
		++SkeletalStats.TransitionCount;
	}

	// Phase 10: refit BLAS for each skinned mesh so RT passes (reflection,
	// GI, shadow if RT) see the current skinned geometry. BLAS is still
	// per-character — each one was built with the char's vertex slice in
	// the unified output VB. Benchmark mode can skip the refit loop with
	// --skeletal-skip-blas so the reported skinning cost excludes BLAS.
	if (!bSkeletalSkipBlas)
	{
		for (Mesh* mesh : skinnedMeshes)
		{
			if (mesh->SkeletalBlas)
			{
				renderBackend->RefitBLAS(mesh->SkeletalBlas.get(), mesh);
				++SkeletalStats.BlasUpdates;
			}
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

	// Uniform per-box weights: every vertex on a box rigidly follows the
	// box's PrimaryBone. The smooth distance-based weight blend at joints
	// (previous PaintWeights pass) caused per-face normal blending after
	// compute skinning — vertices on the same face ended up with different
	// bone weights, so the bone-rotated normals diverged across the face
	// and the box character looked oddly smooth-shaded / dirty rather than
	// crisply flat. With rigid weights each box rotates as a unit, so all
	// vertices on a face share the exact same skinned normal → true
	// per-face flat shading. Trade-off: joints show small geometric gaps
	// between boxes, which is the expected look for a box-and-bone test
	// character.
	for (const BoxPart& part : parts)
	{
		const size_t vertStart = skin.size();
		AppendBox(vertices, indices, skin, part, boneHeads, boneTails);
		const size_t vertEnd = skin.size();
		for (size_t i = vertStart; i < vertEnd; ++i)
		{
			skin[i].BoneIndicesPacked = static_cast<UINT32>(part.PrimaryBone & 0xFF);
			skin[i].BoneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
		}
	}

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
	float spacing = 150.0f;
	glm::vec3 spawnOrigin;
	if (bCommandLineSkeletalBenchMode)
	{
		// Standalone benchmark: no Sponza, drop the grid at the world
		// origin so the entire crowd is visible from a single camera
		// vantage point set up by OnInit below. Use wider spacing so
		// individual characters stay countable visually (characters are
		// ~70 units wide after scale).
		spawnOrigin = glm::vec3(0.0f, 0.0f, 0.0f);
		spacing = 250.0f;
	}
	else
	{
		// Aim point ~600 units along the look direction from the sponza-fly
		// startup camera, then snap to the Sponza floor (y ≈ -12).
		const glm::vec3 cameraPos(458.0f, 781.0f, 185.0f);
		const glm::vec3 lookDir(-0.876484f, -0.389418f, -0.283072f);
		const glm::vec3 spawnAim = cameraPos + lookDir * 600.0f;
		spawnOrigin = glm::vec3(spawnAim.x, -12.0f, spawnAim.z);
	}
	const int gridSide = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(desiredCount))));
	const float gridOffset = -0.5f * (gridSide - 1) * spacing;

	// Phase A: build a single unified set of skeletal resources once and
	// share the pointers across every test character. SBV layouts:
	//   SkeletalUnifiedInputVertices: VertsPerChar entries (single shared
	//                                 bind-pose copy)
	//   SkeletalUnifiedBoneMatrices : CharCount * BoneCount entries
	//   SkeletalUnifiedPrevBoneMatrices: same
	//   SkeletalUnifiedOutputVb     : CharCount * VertsPerChar vertices
	//   SkeletalUnifiedBindVb       : VertsPerChar vertices (single shared)
	//   SkeletalUnifiedIb           : indexCount indices (single shared)
	// Each Mesh's per-char SkeletalCharIndex stores its slot.
	SkeletalUnifiedCharCount = desiredCount;
	SkeletalUnifiedVertsPerChar = vertexCount;
	SkeletalUnifiedBoneCount = static_cast<UINT32>(BONE_COUNT);

	{
		BufferCreateDesc inputDesc = {};
		inputDesc.NumElements = static_cast<UINT32>(skin.size());
		inputDesc.ElementSize = sizeof(SkinInputVertex);
		inputDesc.InitialState = EInitialResourceState::ShaderRead;
		inputDesc.bAllowUnorderedAccess = true;
		inputDesc.InitialData = skin.data();
		inputDesc.Shape = EBufferShape::Structured;
		SkeletalUnifiedInputVertices = renderBackend->CreateBuffer(inputDesc);

		// CPU-side mirror of the bind pose for the CPU-skinning benchmark
		// path. Avoids reading back from a GPU SBV at skin time.
		SkeletalUnifiedBindPoseCpu.resize(skin.size());
		for (size_t i = 0; i < skin.size(); ++i)
		{
			SkinInputVertexCpu& dst = SkeletalUnifiedBindPoseCpu[i];
			dst.BindPosition[0] = skin[i].BindPosition.x;
			dst.BindPosition[1] = skin[i].BindPosition.y;
			dst.BindPosition[2] = skin[i].BindPosition.z;
			dst.BindNormal[0] = skin[i].BindNormal.x;
			dst.BindNormal[1] = skin[i].BindNormal.y;
			dst.BindNormal[2] = skin[i].BindNormal.z;
			dst.BindTangent[0] = skin[i].BindTangent.x;
			dst.BindTangent[1] = skin[i].BindTangent.y;
			dst.BindTangent[2] = skin[i].BindTangent.z;
			dst.UV[0] = skin[i].UV.x;
			dst.UV[1] = skin[i].UV.y;
			dst.BoneIndicesPacked = skin[i].BoneIndicesPacked;
			dst.BoneWeights[0] = skin[i].BoneWeights.x;
			dst.BoneWeights[1] = skin[i].BoneWeights.y;
			dst.BoneWeights[2] = skin[i].BoneWeights.z;
			dst.BoneWeights[3] = skin[i].BoneWeights.w;
		}
	}

	struct InitialBoneMatrix { float r0[4]; float r1[4]; float r2[4]; };
	static_assert(sizeof(InitialBoneMatrix) == 48, "SkinBone size drift");
	{
		std::vector<InitialBoneMatrix> identityBones(
			static_cast<size_t>(SkeletalUnifiedCharCount) * BONE_COUNT);
		const InitialBoneMatrix identity = {
			{1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, 1.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f}
		};
		for (auto& m : identityBones) m = identity;
		SkeletalUnifiedBoneMatrices = renderBackend->CreateUploadStructuredBuffer(
			static_cast<UINT32>(identityBones.size()), sizeof(InitialBoneMatrix));
		if (SkeletalUnifiedBoneMatrices)
		{
			renderBackend->UpdateUploadStructuredBuffer(
				SkeletalUnifiedBoneMatrices.get(),
				identityBones.data(),
				static_cast<UINT32>(identityBones.size() * sizeof(InitialBoneMatrix)));
		}
		SkeletalUnifiedPrevBoneMatrices = renderBackend->CreateUploadStructuredBuffer(
			static_cast<UINT32>(identityBones.size()), sizeof(InitialBoneMatrix));
		if (SkeletalUnifiedPrevBoneMatrices)
		{
			renderBackend->UpdateUploadStructuredBuffer(
				SkeletalUnifiedPrevBoneMatrices.get(),
				identityBones.data(),
				static_cast<UINT32>(identityBones.size() * sizeof(InitialBoneMatrix)));
		}
	}

	// Shared bind-pose VB and IB (single copy used by every character).
	SkeletalUnifiedBindVb = renderBackend->CreateVertexBuffer(
		static_cast<UINT32>(sizeof(StandardVertex) * vertices.size()),
		sizeof(StandardVertex), vertices.data());
	SkeletalUnifiedIb = renderBackend->CreateIndexBuffer(
		EIndexFormat::U32,
		static_cast<UINT32>(sizeof(UINT32) * indices.size()),
		indices.data());

	// Unified RW output VB sized for all characters.
	SkeletalUnifiedOutputVb = renderBackend->CreateRWVertexBuffer(
		static_cast<UINT32>(sizeof(StandardVertex) * vertices.size()) * SkeletalUnifiedCharCount,
		sizeof(StandardVertex));

	// Phase D (desktop cluster draw): per-instance world transform SBV
	// indexed by SV_InstanceID in the cluster VS variant. Layout matches
	// SkinBone_t (mat3x4 row, 48 B per entry) — affine-only, last row
	// implicit (0, 0, 0, 1). Mobile doesn't bind this SBV.
	{
		const InitialBoneMatrix identity = {
			{1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, 1.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f}
		};
		std::vector<InitialBoneMatrix> identityTransforms(SkeletalUnifiedCharCount, identity);
		SkeletalUnifiedInstanceTransforms = renderBackend->CreateUploadStructuredBuffer(
			SkeletalUnifiedCharCount,
			static_cast<UINT32>(sizeof(InitialBoneMatrix)));
		if (SkeletalUnifiedInstanceTransforms)
		{
			renderBackend->UpdateUploadStructuredBuffer(
				SkeletalUnifiedInstanceTransforms.get(),
				identityTransforms.data(),
				static_cast<UINT32>(identityTransforms.size() * sizeof(InitialBoneMatrix)));
		}
	}
	SkeletalUnifiedMaterial = material;
	SkeletalUnifiedIndexCount = indexCount;

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

		auto mesh = std::make_shared<Mesh>(renderBackend.get());
		mesh->transform = glm::mat4x4(1.0f);
		mesh->NumVertices = vertexCount;
		mesh->NumIndices = indexCount;
		mesh->VertexStride = sizeof(StandardVertex);
		mesh->IndexFormat = EIndexFormat::U32;
		mesh->Mat = material;
		// All per-char meshes share the unified bind-pose VB / IB. The
		// per-char slice is selected at draw time via VertexBase.
		mesh->Vb = SkeletalUnifiedBindVb;
		mesh->Ib = SkeletalUnifiedIb;
		mesh->CpuPositions.reserve(vertices.size());
		for (const StandardVertex& v : vertices)
			mesh->CpuPositions.emplace_back(glm::vec3(v.Position));
		mesh->CpuIndices = indices;

		Mesh::DrawCall drawCall = {};
		drawCall.mat = material;
		drawCall.IndexStart = 0;
		drawCall.IndexCount = indexCount;
		// Skeletal draw reads from the unified output VB at this slot's
		// slice. Bind-pose fallback works too because the unified bind VB
		// has a single copy and SV_VertexID stays inside [0, VertsPerChar)
		// for that draw — but the GBuffer skeletal VS subtracts the char
		// base anyway, so this BaseVertex value matters only for the IA
		// when reading the unified output VB.
		drawCall.VertexBase = static_cast<int32_t>(instance * vertexCount);
		drawCall.VertexCount = vertexCount;
		mesh->Draws.push_back(drawCall);

		mesh->bSkeletalSkinned = true;
		mesh->SkeletalVertexCount = vertexCount;
		mesh->SkeletalBoneCount = static_cast<UINT32>(BONE_COUNT);
		mesh->SkeletalCharIndex = instance;
		// Per-mesh skeletal pointers all alias to the unified resources.
		// Pre-existing callers that walk mesh->SkeletalInputVertices etc.
		// keep working without knowing about the unification.
		mesh->SkeletalInputVertices = SkeletalUnifiedInputVertices;
		mesh->SkeletalBoneMatrices = SkeletalUnifiedBoneMatrices;
		mesh->SkeletalPrevBoneMatrices = SkeletalUnifiedPrevBoneMatrices;
		mesh->SkeletalOutputVb = SkeletalUnifiedOutputVb;

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

	// Bench mode: add a large flat ground box under the grid so we have
	// a surface for shadows to land on and a reference for whether the
	// characters are actually rendering.
	if (bCommandLineSkeletalBenchMode)
	{
		const float groundHalfXZ = std::max(2000.0f, static_cast<float>(gridSide) * spacing + 1000.0f);
		const float groundHalfY = 5.0f;

		// 8 corner positions of a unit cube spanning [-1, 1] in each axis.
		static const glm::vec3 kCubeCorners[8] = {
			{-1.0f, -1.0f, -1.0f}, { 1.0f, -1.0f, -1.0f}, { 1.0f,  1.0f, -1.0f}, {-1.0f,  1.0f, -1.0f},
			{-1.0f, -1.0f,  1.0f}, { 1.0f, -1.0f,  1.0f}, { 1.0f,  1.0f,  1.0f}, {-1.0f,  1.0f,  1.0f},
		};
		static const int kFaceCorners[6][4] = {
			{0, 1, 2, 3}, // -Z
			{5, 4, 7, 6}, // +Z
			{4, 0, 3, 7}, // -X
			{1, 5, 6, 2}, // +X
			{3, 2, 6, 7}, // +Y (top)
			{4, 5, 1, 0}, // -Y (bottom)
		};
		static const glm::vec3 kFaceNormals[6] = {
			{ 0,  0, -1}, { 0,  0,  1}, {-1,  0,  0}, { 1,  0,  0}, { 0,  1,  0}, { 0, -1,  0},
		};
		static const glm::vec3 kFaceTangents[6] = {
			{ 1,  0,  0}, {-1,  0,  0}, { 0,  0,  1}, { 0,  0, -1}, { 1,  0,  0}, { 1,  0,  0},
		};

		// Use the 44 B legacy "Vertex" layout (POS@0 / NORMAL@12 / UV@24 /
		// TANGENT@32) — same one the base GBufferGraphicsPipeline binds.
		// Skeletal characters use the 48 B StandardVertex layout against
		// their own dedicated PSOs, but the ground is a non-skeletal mesh
		// drawn through the base PSO, so it must match the base stride.
		// Using StandardVertex here silently worked on DX12 (VBV stride
		// wins) but would slip every vertex 4 B on Vulkan.
		struct GroundVertex
		{
			glm::vec3 Position;
			glm::vec3 Normal;
			glm::vec2 UV;
			glm::vec3 Tangent;
		};
		static_assert(sizeof(GroundVertex) == 44, "GroundVertex must match base GBuffer PSO stride");

		std::vector<GroundVertex> gv;
		std::vector<UINT32> gi;
		gv.reserve(24);
		gi.reserve(36);
		for (int f = 0; f < 6; ++f)
		{
			const UINT32 base = static_cast<UINT32>(gv.size());
			for (int c = 0; c < 4; ++c)
			{
				const glm::vec3 corner = kCubeCorners[kFaceCorners[f][c]];
				GroundVertex v = {};
				v.Position = glm::vec3(corner.x * groundHalfXZ, corner.y * groundHalfY, corner.z * groundHalfXZ);
				v.UV = glm::vec2(c == 1 || c == 2 ? 1.0f : 0.0f, c >= 2 ? 1.0f : 0.0f);
				v.Normal = kFaceNormals[f];
				v.Tangent = kFaceTangents[f];
				gv.push_back(v);
			}
			gi.push_back(base + 0); gi.push_back(base + 1); gi.push_back(base + 2);
			gi.push_back(base + 0); gi.push_back(base + 2); gi.push_back(base + 3);
		}

		std::shared_ptr<Material> groundMat = std::make_shared<Material>();
		groundMat->Diffuse = DefaultWhiteTex;
		groundMat->Normal = DefaultNormalTex;
		groundMat->Roughness = DefaultBlackTex;
		groundMat->Metallic = DefaultBlackTex;

		std::shared_ptr<Scene> groundScene = std::make_shared<Scene>();
		groundScene->Materials.push_back(groundMat);

		auto groundMesh = std::make_shared<Mesh>(renderBackend.get());
		groundMesh->transform = glm::mat4x4(1.0f);
		groundMesh->NumVertices = static_cast<UINT32>(gv.size());
		groundMesh->NumIndices = static_cast<UINT32>(gi.size());
		groundMesh->VertexStride = sizeof(GroundVertex);
		groundMesh->IndexFormat = EIndexFormat::U32;
		groundMesh->Mat = groundMat;
		groundMesh->Vb = renderBackend->CreateVertexBuffer(
			static_cast<UINT32>(sizeof(GroundVertex) * gv.size()),
			sizeof(GroundVertex), gv.data());
		groundMesh->Ib = renderBackend->CreateIndexBuffer(
			groundMesh->IndexFormat,
			static_cast<UINT32>(sizeof(UINT32) * gi.size()),
			gi.data());
		groundMesh->CpuPositions.reserve(gv.size());
		for (const GroundVertex& v : gv)
			groundMesh->CpuPositions.emplace_back(v.Position);
		groundMesh->CpuIndices = gi;

		Mesh::DrawCall groundDraw = {};
		groundDraw.mat = groundMat;
		groundDraw.IndexStart = 0;
		groundDraw.IndexCount = static_cast<UINT32>(gi.size());
		groundDraw.VertexBase = 0;
		groundDraw.VertexCount = static_cast<UINT32>(gv.size());
		groundMesh->Draws.push_back(groundDraw);

		groundScene->meshes.push_back(groundMesh);
		groundScene->bHasBounds = true;
		groundScene->BoundsMin = glm::vec3(-groundHalfXZ, -groundHalfY, -groundHalfXZ);
		groundScene->BoundsMax = glm::vec3( groundHalfXZ,  groundHalfY,  groundHalfXZ);

		// Sit slightly below grid floor (Y=0) so characters stand on it.
		SceneObjectDesc groundDesc;
		groundDesc.ScenePtr = groundScene;
		groundDesc.Transform = glm::translate(glm::mat4x4(1.0f), glm::vec3(0.0f, -groundHalfY, 0.0f));
		groundDesc.Roughness = 0.6f;
		groundDesc.Metallic = 0.0f;
		groundDesc.bOverrideRoughnessMetallic = true;
		groundDesc.bRayTracing = true;
		groundDesc.bPhysicsQuery = false;
		(void)AddSceneObject(groundDesc);

		AppendCpuRuntimeTrace(
			L"[SkeletalBench] added ground halfXZ=" + std::to_wstring(groundHalfXZ) +
			L" halfY=" + std::to_wstring(groundHalfY));
	}

	AppendCpuRuntimeTrace(
		L"[SkeletalSpawn] done count=" + std::to_wstring(desiredCount));
}

namespace
{
	// mat3x4 row layout matching the SBV the compute skinning shader binds.
	struct SkinBoneRow { float r0[4]; float r1[4]; float r2[4]; };

	// Skeleton + bind-pose data is identical for every test character and
	// every frame. Build it once on first use and reuse across calls.
	struct SkeletonCache
	{
		std::array<BoneSegment, BONE_COUNT> Bones;
		std::array<glm::vec3, BONE_COUNT> BoneHeads;
		std::array<glm::mat4, BONE_COUNT> BindLocalTranslation;  // translate(HeadLocalOffset)
		bool Initialized = false;
	};
	SkeletonCache& GetSkeletonCache()
	{
		static SkeletonCache cache;
		if (!cache.Initialized)
		{
			std::vector<BoneSegment> bonesVec;
			BuildSkeleton(bonesVec);
			std::vector<glm::vec3> heads, tails;
			ComputeBindPoseBoneEndpoints(bonesVec, heads, tails);
			for (size_t i = 0; i < BONE_COUNT; ++i)
			{
				cache.Bones[i] = bonesVec[i];
				cache.BoneHeads[i] = heads[i];
				cache.BindLocalTranslation[i] =
					glm::translate(glm::mat4(1.0f), bonesVec[i].HeadLocalOffset);
			}
			cache.Initialized = true;
		}
		return cache;
	}

	// Compute the per-bone skinning matrices for one character and pack
	// them directly into the mat3x4 row layout the compute shader expects.
	// No glm::mat4 intermediate palette, no per-call vector allocations,
	// no BuildSkeleton repeat.
	void ComputeSkeletalPalettePacked(float timeSeconds, SkinBoneRow* outPacked, int instanceIndex)
	{
		const SkeletonCache& cache = GetSkeletonCache();

		// Per-instance phase offset so a crowd doesn't move in lockstep.
		const float phase = static_cast<float>(instanceIndex) * 0.37f;
		const float t = timeSeconds + phase;

		std::array<glm::mat4, BONE_COUNT> localT = cache.BindLocalTranslation;

		auto rotateAroundHead = [&](int boneIdx, const glm::vec3& axis, float angle)
		{
			if (boneIdx < 0 || boneIdx >= static_cast<int>(BONE_COUNT))
				return;
			localT[boneIdx] = cache.BindLocalTranslation[boneIdx] *
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

		// Parent -> child world accumulation.
		std::array<glm::mat4, BONE_COUNT> worldT;
		worldT[BONE_ROOT] = localT[BONE_ROOT];
		for (size_t i = 1; i < BONE_COUNT; ++i)
		{
			worldT[i] = worldT[cache.Bones[i].ParentIndex] * localT[i];
		}

		// Final skinning = world * inverse(bind_world). bind_world is pure
		// translation by BoneHeads[i] so M = world with col3 replaced by
		// world * vec4(-BoneHeads[i], 1). Pack directly into the row-major
		// mat3x4 layout the shader binds (rows 0..2 of transposed M).
		for (size_t i = 0; i < BONE_COUNT; ++i)
		{
			const glm::mat4& W = worldT[i];
			const glm::vec3& bh = cache.BoneHeads[i];
			const glm::vec4 c3 = W[3] - W[0] * bh.x - W[1] * bh.y - W[2] * bh.z;
			SkinBoneRow& dst = outPacked[i];
			dst.r0[0] = W[0].x; dst.r0[1] = W[1].x; dst.r0[2] = W[2].x; dst.r0[3] = c3.x;
			dst.r1[0] = W[0].y; dst.r1[1] = W[1].y; dst.r1[2] = W[2].y; dst.r1[3] = c3.y;
			dst.r2[0] = W[0].z; dst.r2[1] = W[1].z; dst.r2[2] = W[2].z; dst.r2[3] = c3.z;
		}
	}
}

void Corona::UpdateSkeletalTestCharacters(float timeSeconds)
{
	if (!renderBackend)
		return;

	// Phase 11 motion vector: compute the palette TWICE per character —
	// once for the current frame's time and once for the previous frame's
	// time — and upload both. The skeletal GBuffer VS re-skins from the
	// bind pose with the previous palette to derive a per-vertex prev
	// clip position. First frame falls back to prev = curr so no motion
	// is reported.
	const float prevTimeSeconds = bSkeletalPrevUpdateTimeValid ? SkeletalPrevUpdateTimeSeconds : timeSeconds;

	// Collect every skeletal mesh that needs an update this frame so the
	// palette compute step (per-character, independent) can run in parallel
	// across all of them. Uploads stay serial because they hit D3D12.
	struct SkinJob
	{
		Mesh* MeshPtr;
		int InstanceIndex;
	};
	std::vector<SkinJob> jobs;
	jobs.reserve(64);
	int instanceIndex = 0;
	for (SceneObject& object : SceneObjects)
	{
		if (!object.ScenePtr)
			continue;
		for (const std::shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
		{
			if (!mesh || !mesh->bSkeletalSkinned)
				continue;
			jobs.push_back({ mesh.get(), instanceIndex++ });
		}
	}

	const size_t jobCount = jobs.size();
	// One mat3x4 palette per bone, two palettes per character (curr + prev),
	// laid out contiguously so workers write disjoint ranges. Indexed by
	// SkeletalCharIndex so the unified buffer slice matches what the
	// compute / GBuffer shader expects.
	const size_t paletteSize = static_cast<size_t>(SkeletalUnifiedCharCount) * BONE_COUNT;
	std::vector<SkinBoneRow> packed(paletteSize);
	std::vector<SkinBoneRow> packedPrev(paletteSize);

	const auto computeStart = CpuClock::now();

	auto computeJob = [&](size_t j)
	{
		const SkinJob& job = jobs[j];
		const size_t slot = static_cast<size_t>(job.MeshPtr->SkeletalCharIndex);
		ComputeSkeletalPalettePacked(timeSeconds, packed.data() + slot * BONE_COUNT, job.InstanceIndex);
		ComputeSkeletalPalettePacked(prevTimeSeconds, packedPrev.data() + slot * BONE_COUNT, job.InstanceIndex);
	};

	// Parallelize palette compute across hardware threads. For very small
	// job counts the dispatch overhead can dwarf the work so fall back to
	// in-line execution.
	const unsigned int hwThreads = std::max(1u, std::thread::hardware_concurrency());
	const size_t kParallelThreshold = 4;
	if (jobCount <= kParallelThreshold || hwThreads <= 1)
	{
		for (size_t j = 0; j < jobCount; ++j)
			computeJob(j);
	}
	else
	{
		const unsigned int workerCount = static_cast<unsigned int>(std::min<size_t>(hwThreads, jobCount));
		std::vector<std::thread> workers;
		workers.reserve(workerCount - 1u);
		std::atomic<size_t> nextJob = 0;
		auto runWorker = [&]()
		{
			for (;;)
			{
				const size_t j = nextJob.fetch_add(1, std::memory_order_relaxed);
				if (j >= jobCount)
					return;
				computeJob(j);
			}
		};
		for (unsigned int w = 1; w < workerCount; ++w)
			workers.emplace_back(runWorker);
		runWorker();
		for (std::thread& t : workers)
			t.join();
	}

	const auto computeEnd = CpuClock::now();
	AddCpuUpdatePhaseTiming(ECpuUpdatePhase::SkeletalPalette, computeStart, computeEnd);
	// Pack is now fused into the compute step; report 0 to keep the slot
	// visible in the overlay for comparison with the pre-fusion baseline.
	AddCpuUpdatePhaseTiming(ECpuUpdatePhase::SkeletalPack, computeEnd, computeEnd);

	// Single contiguous upload covers all characters at once. The unified
	// buffer is persistent-mapped UPLOAD heap, so this is one memcpy.
	if (SkeletalUnifiedBoneMatrices && !packed.empty())
	{
		renderBackend->UpdateUploadStructuredBuffer(
			SkeletalUnifiedBoneMatrices.get(),
			packed.data(),
			static_cast<UINT32>(packed.size() * sizeof(SkinBoneRow)));
	}
	if (SkeletalUnifiedPrevBoneMatrices && !packedPrev.empty())
	{
		renderBackend->UpdateUploadStructuredBuffer(
			SkeletalUnifiedPrevBoneMatrices.get(),
			packedPrev.data(),
			static_cast<UINT32>(packedPrev.size() * sizeof(SkinBoneRow)));
	}

	// Mirror the packed palettes onto the Corona class so the CPU-skinning
	// benchmark path can read them on the render thread without a GPU
	// readback. Same layout as the SBV slots.
	static_assert(sizeof(SkinBoneRow) == sizeof(SkinBoneRowCpu),
		"SkinBoneRow layout must match SkinBoneRowCpu");
	SkeletalUnifiedPaletteCpu.assign(
		reinterpret_cast<const SkinBoneRowCpu*>(packed.data()),
		reinterpret_cast<const SkinBoneRowCpu*>(packed.data() + packed.size()));
	SkeletalUnifiedPalettePrevCpu.assign(
		reinterpret_cast<const SkinBoneRowCpu*>(packedPrev.data()),
		reinterpret_cast<const SkinBoneRowCpu*>(packedPrev.data() + packedPrev.size()));
	AddCpuUpdatePhaseTiming(ECpuUpdatePhase::SkeletalUpload, computeEnd, CpuClock::now());

	SkeletalPrevUpdateTimeSeconds = timeSeconds;
	bSkeletalPrevUpdateTimeValid = true;
}

void Corona::CpuSkinSkeletalCharactersForRenderWorld()
{
	if (!renderBackend ||
		SkeletalUnifiedCharCount == 0 ||
		SkeletalUnifiedVertsPerChar == 0 ||
		SkeletalUnifiedBindPoseCpu.empty() ||
		SkeletalUnifiedPaletteCpu.size() < SkeletalUnifiedCharCount * SkeletalUnifiedBoneCount)
	{
		SkeletalUnifiedCpuSkinnedVb.reset();
		return;
	}

	const uint32_t boneCount = SkeletalUnifiedBoneCount;
	const uint32_t vertsPerChar = SkeletalUnifiedVertsPerChar;
	const uint32_t totalVerts = SkeletalUnifiedCharCount * vertsPerChar;
	std::vector<StandardVertex> skinned(totalVerts);

	auto skinOne = [&](size_t charIdx)
	{
		const SkinBoneRowCpu* charBones = &SkeletalUnifiedPaletteCpu[charIdx * boneCount];
		StandardVertex* dst = &skinned[charIdx * vertsPerChar];

		auto transformPoint = [](const SkinBoneRowCpu& b, float x, float y, float z) -> glm::vec3 {
			return glm::vec3(
				b.r0[0]*x + b.r0[1]*y + b.r0[2]*z + b.r0[3],
				b.r1[0]*x + b.r1[1]*y + b.r1[2]*z + b.r1[3],
				b.r2[0]*x + b.r2[1]*y + b.r2[2]*z + b.r2[3]);
		};
		auto transformDir = [](const SkinBoneRowCpu& b, float x, float y, float z) -> glm::vec3 {
			return glm::vec3(
				b.r0[0]*x + b.r0[1]*y + b.r0[2]*z,
				b.r1[0]*x + b.r1[1]*y + b.r1[2]*z,
				b.r2[0]*x + b.r2[1]*y + b.r2[2]*z);
		};

		for (uint32_t v = 0; v < vertsPerChar; ++v)
		{
			const SkinInputVertexCpu& src = SkeletalUnifiedBindPoseCpu[v];
			const uint32_t i0 = (src.BoneIndicesPacked >>  0) & 0xFFu;
			const uint32_t i1 = (src.BoneIndicesPacked >>  8) & 0xFFu;
			const uint32_t i2 = (src.BoneIndicesPacked >> 16) & 0xFFu;
			const uint32_t i3 = (src.BoneIndicesPacked >> 24) & 0xFFu;
			const float w0 = src.BoneWeights[0];
			const float w1 = src.BoneWeights[1];
			const float w2 = src.BoneWeights[2];
			const float w3 = src.BoneWeights[3];

			const glm::vec3 P0 = transformPoint(charBones[i0], src.BindPosition[0], src.BindPosition[1], src.BindPosition[2]);
			const glm::vec3 P1 = transformPoint(charBones[i1], src.BindPosition[0], src.BindPosition[1], src.BindPosition[2]);
			const glm::vec3 P2 = transformPoint(charBones[i2], src.BindPosition[0], src.BindPosition[1], src.BindPosition[2]);
			const glm::vec3 P3 = transformPoint(charBones[i3], src.BindPosition[0], src.BindPosition[1], src.BindPosition[2]);
			const glm::vec3 P = P0*w0 + P1*w1 + P2*w2 + P3*w3;

			const glm::vec3 N0 = transformDir(charBones[i0], src.BindNormal[0], src.BindNormal[1], src.BindNormal[2]);
			const glm::vec3 N1 = transformDir(charBones[i1], src.BindNormal[0], src.BindNormal[1], src.BindNormal[2]);
			const glm::vec3 N2 = transformDir(charBones[i2], src.BindNormal[0], src.BindNormal[1], src.BindNormal[2]);
			const glm::vec3 N3 = transformDir(charBones[i3], src.BindNormal[0], src.BindNormal[1], src.BindNormal[2]);
			glm::vec3 N = N0*w0 + N1*w1 + N2*w2 + N3*w3;
			const float nLenSq = glm::dot(N, N);
			if (nLenSq > 1e-12f) N *= 1.0f / std::sqrt(nLenSq);

			const glm::vec3 T0 = transformDir(charBones[i0], src.BindTangent[0], src.BindTangent[1], src.BindTangent[2]);
			const glm::vec3 T1 = transformDir(charBones[i1], src.BindTangent[0], src.BindTangent[1], src.BindTangent[2]);
			const glm::vec3 T2 = transformDir(charBones[i2], src.BindTangent[0], src.BindTangent[1], src.BindTangent[2]);
			const glm::vec3 T3 = transformDir(charBones[i3], src.BindTangent[0], src.BindTangent[1], src.BindTangent[2]);
			glm::vec3 T = T0*w0 + T1*w1 + T2*w2 + T3*w3;
			T = T - N * glm::dot(N, T);
			const float tLenSq = glm::dot(T, T);
			if (tLenSq > 1e-12f) T *= 1.0f / std::sqrt(tLenSq);

			dst[v].Position = glm::vec4(P, 1.0f);
			dst[v].UV = glm::vec2(src.UV[0], src.UV[1]);
			dst[v].Normal = N;
			dst[v].Tangent = T;
		}
	};

	// Parallel skin across characters (work-stealing pool).
	const unsigned int hwThreads = std::max(1u, std::thread::hardware_concurrency());
	const size_t jobCount = SkeletalUnifiedCharCount;
	const size_t kParallelThreshold = 4;
	if (jobCount <= kParallelThreshold || hwThreads <= 1)
	{
		for (size_t j = 0; j < jobCount; ++j)
			skinOne(j);
	}
	else
	{
		const unsigned int workerCount = static_cast<unsigned int>(std::min<size_t>(hwThreads, jobCount));
		std::vector<std::thread> workers;
		workers.reserve(workerCount - 1u);
		std::atomic<size_t> nextJob = 0;
		auto runWorker = [&]() {
			for (;;) {
				const size_t j = nextJob.fetch_add(1, std::memory_order_relaxed);
				if (j >= jobCount) return;
				skinOne(j);
			}
		};
		for (unsigned int w = 1; w < workerCount; ++w)
			workers.emplace_back(runWorker);
		runWorker();
		for (std::thread& t : workers) t.join();
	}

	// Upload to a fresh UPLOAD-heap VB and park it in a small ring so the
	// previous frame's VB (which the GPU may still be reading) is held
	// alive long enough to satisfy the D3D12 debug layer.
	SkeletalUnifiedCpuSkinnedVbRingIndex =
		(SkeletalUnifiedCpuSkinnedVbRingIndex + 1) %
		static_cast<uint32_t>(SkeletalUnifiedCpuSkinnedVbRing.size());
	auto& slot = SkeletalUnifiedCpuSkinnedVbRing[SkeletalUnifiedCpuSkinnedVbRingIndex];
	slot = renderBackend->CreateUploadVertexBuffer(
		static_cast<UINT32>(skinned.size() * sizeof(StandardVertex)),
		sizeof(StandardVertex),
		skinned.data());
	SkeletalUnifiedCpuSkinnedVb = slot;
}

void Corona::UpdateSkeletalUnifiedInstanceTransforms()
{
	if (!SkeletalUnifiedInstanceTransforms || SkeletalUnifiedCharCount == 0)
		return;

	// mat3x4 packing for the per-instance world transform. The HLSL VS
	// rebuilds a 4x4 matrix from these 3 rows (last row is implicit
	// 0, 0, 0, 1). Matches the SkinBone_t layout that
	// SkeletalUnifiedBoneMatrices uses, which Adreno's compiler accepts
	// even though it rejects StructuredBuffer<float4x4>.
	struct InstanceXform { float r0[4]; float r1[4]; float r2[4]; };
	static_assert(sizeof(InstanceXform) == 48, "Instance xform layout drift");
	std::vector<InstanceXform> transforms(SkeletalUnifiedCharCount);
	const InstanceXform identity = {
		{1.0f, 0.0f, 0.0f, 0.0f},
		{0.0f, 1.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 1.0f, 0.0f}
	};
	for (auto& m : transforms) m = identity;

	for (const SceneObject& object : RenderWorld.SceneObjects)
	{
		if (!object.bVisible || !object.ScenePtr)
			continue;
		for (const std::shared_ptr<Mesh>& mesh : object.ScenePtr->meshes)
		{
			if (!mesh || !mesh->bSkeletalSkinned)
				continue;
			if (mesh->SkeletalOutputVb != SkeletalUnifiedOutputVb)
				continue;
			const uint32_t slot = mesh->SkeletalCharIndex;
			if (slot >= SkeletalUnifiedCharCount) continue;

			const glm::mat4 world = object.Transform * mesh->transform;
			// Transpose to row layout (mul-from-the-left in HLSL),
			// then drop the implicit last row.
			const glm::mat4 t = glm::transpose(world);
			InstanceXform& dst = transforms[slot];
			dst.r0[0] = t[0][0]; dst.r0[1] = t[0][1]; dst.r0[2] = t[0][2]; dst.r0[3] = t[0][3];
			dst.r1[0] = t[1][0]; dst.r1[1] = t[1][1]; dst.r1[2] = t[1][2]; dst.r1[3] = t[1][3];
			dst.r2[0] = t[2][0]; dst.r2[1] = t[2][1]; dst.r2[2] = t[2][2]; dst.r2[3] = t[2][3];
		}
	}
	renderBackend->UpdateUploadStructuredBuffer(
		SkeletalUnifiedInstanceTransforms.get(),
		transforms.data(),
		static_cast<UINT32>(transforms.size() * sizeof(InstanceXform)));
}

bool Corona::DrawSkeletalVsInlineClusterDesktop()
{
	// Desktop-only Phase D: a single DrawIndexedInstanced covers every
	// skeletal character. The cluster VS (`SkeletalVsInlineClusterVSMain`)
	// reads its per-character world matrix from
	// `SkeletalInstanceTransforms[SV_InstanceID]` and skins the bind-pose
	// vertex from the current/prev bone palettes -- no compute pre-pass
	// and no per-mesh draw call.
	//
	// Mobile (Adreno) cannot compile the SV_InstanceID +
	// SkeletalInstanceTransforms variant, so this PSO is null on mobile
	// and callers fall back to the per-mesh VS-inline path.
	GraphicsPipelineHandle* pso = SkeletalVsInlineClusterGraphicsPipeline
		? SkeletalVsInlineClusterGraphicsPipeline.get() : nullptr;
	if (!pso ||
		!SkeletalUnifiedBindVb || !SkeletalUnifiedIb ||
		!SkeletalUnifiedInputVertices || !SkeletalUnifiedPrevBoneMatrices ||
		!SkeletalUnifiedBoneMatrices || !SkeletalUnifiedInstanceTransforms ||
		!SkeletalUnifiedMaterial ||
		SkeletalUnifiedCharCount == 0 || SkeletalUnifiedIndexCount == 0)
	{
		return false;
	}

	renderBackend->BindGraphicsPipeline(pso);
	renderBackend->BindGraphicsPipelineSampler(pso, "samplerWrap", samplerWrap.get());
	renderBackend->BindGraphicsPipelineBuffer(pso, "SkeletalInputs", SkeletalUnifiedInputVertices.get());
	renderBackend->BindGraphicsPipelineBuffer(pso, "SkeletalPrevBones", SkeletalUnifiedPrevBoneMatrices.get());
	renderBackend->BindGraphicsPipelineBuffer(pso, "SkeletalCurrBones", SkeletalUnifiedBoneMatrices.get());
	renderBackend->BindGraphicsPipelineBuffer(pso, "SkeletalInstanceTransforms", SkeletalUnifiedInstanceTransforms.get());

	renderBackend->BindMeshBuffers(SkeletalUnifiedBindVb.get(), SkeletalUnifiedIb.get());

	GBufferConstantBuffer objCB = {};
	objCB.ViewProjectionMatrix = glm::transpose(ViewProjMat);
	objCB.PrevViewProjectionMatrix = glm::transpose(PrevViewProjMat);
	// Per-instance transform comes from the SBV at SV_InstanceID;
	// CB.WorldMatrix is unused by the cluster VS.
	objCB.WorldMatrix = glm::mat4x4(1.0f);
	objCB.UnjitteredViewProjMat = glm::transpose(UnjitteredViewProjMat);
	objCB.PrevUnjitteredViewProjMat = glm::transpose(PrevUnjitteredViewProjMat);
	objCB.ViewDir.x = RenderFrameCameraLookDirection.x;
	objCB.ViewDir.y = RenderFrameCameraLookDirection.y;
	objCB.ViewDir.z = RenderFrameCameraLookDirection.z;
	objCB.ViewDir.w = 0.0f;
	objCB.BaseColorFactor = SkeletalUnifiedMaterial->BaseColorFactor;
	objCB.RTSize.x = GetRenderWidth();
	objCB.RTSize.y = GetRenderHeight();
	objCB.RougnessMetalic.x = 0.6f;
	objCB.RougnessMetalic.y = 0.0f;
	objCB.bOverrideRougnessMetallic = 1u;
	objCB.bTwoSidedLighting = 0u;
	objCB.bUnlitMaterial = 0u;
	objCB.SpineVertexBase = 0u;
	// Cluster VS picks the char index from SV_InstanceID, not from CB.
	objCB.SkeletalCharIndex = 0u;
	objCB.SkeletalVertsPerChar = SkeletalUnifiedVertsPerChar;
	objCB.SkeletalBoneCount = SkeletalUnifiedBoneCount;
	renderBackend->SetGraphicsPipelineConstantData(pso, 0, &objCB, sizeof(objCB));

	Texture* albedo = SkeletalUnifiedMaterial->Diffuse ? SkeletalUnifiedMaterial->Diffuse.get() : DefaultWhiteTex.get();
	Texture* normal = SkeletalUnifiedMaterial->Normal ? SkeletalUnifiedMaterial->Normal.get() : DefaultNormalTex.get();
	Texture* rough  = SkeletalUnifiedMaterial->Roughness ? SkeletalUnifiedMaterial->Roughness.get() : DefaultRougnessTex.get();
	Texture* metal  = SkeletalUnifiedMaterial->Metallic ? SkeletalUnifiedMaterial->Metallic.get() : DefaultBlackTex.get();
	renderBackend->BindGraphicsPipelineTexture(pso, "AlbedoTex", albedo);
	renderBackend->BindGraphicsPipelineTexture(pso, "NormalTex", normal);
	renderBackend->BindGraphicsPipelineTexture(pso, "RoughnessTex", rough);
	renderBackend->BindGraphicsPipelineTexture(pso, "MetallicTex", metal);

	renderBackend->DrawIndexedInstanced(
		SkeletalUnifiedIndexCount,
		SkeletalUnifiedCharCount,
		0, 0, 0);

	return true;
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
