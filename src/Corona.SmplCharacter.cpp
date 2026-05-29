// Corona.SmplCharacter.cpp — procedural 24-bone SMPL skinned character.
//
// Mirrors the bring-up pattern of Corona.Skeletal.cpp's SpawnSkeletalTest
// path but in miniature: one instance, dedicated GPU buffers (no unified
// batch), SMPL 24-joint hierarchy, single rigid box per limb segment.
//
// The mesh is built from 18 axis-aligned boxes covering torso/limbs/head.
// Each box is skinned 100% to one bone (the bone whose rotation should
// drive that segment in SMPL conventions). Per-vertex hard normals: each
// face gets its own 4 unique vertices.
//
// Per-frame: ComputeBonePalette() turns a PoseState into a flat mat3x4[24]
// array which gets uploaded straight to the BoneMatrices SBV that the
// existing skinning compute shader reads.

#include "stdafx.h"
#include "Corona.SmplCharacter.h"
#include "Corona.h"
#include "Corona.MotionPlayback.h"

#include "RenderResources.h"
#include "RenderBackend.h"

#include <algorithm>
#include <vector>

#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/quaternion.hpp"

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	// Must match SkinInputVertex (80 B) in Corona.Skeletal.cpp.
	struct SmplSkinInputVertex
	{
		glm::vec3 BindPosition;
		float     Pad0 = 0.0f;
		glm::vec3 BindNormal;
		float     Pad1 = 0.0f;
		glm::vec3 BindTangent;
		float     Pad2 = 0.0f;
		glm::vec2 UV;
		UINT32    BoneIndicesPacked = 0;
		float     PadEnd = 0.0f;
		glm::vec4 BoneWeights = glm::vec4(0.0f);
	};
	static_assert(sizeof(SmplSkinInputVertex) == 80, "SmplSkinInputVertex must match SkinInputVertex");

	// Must match StandardVertex (48 B) in Corona.Skeletal.cpp.
	struct SmplStandardVertex
	{
		glm::vec4 Position;
		glm::vec2 UV;
		glm::vec3 Normal;
		glm::vec3 Tangent;
	};
	static_assert(sizeof(SmplStandardVertex) == 48, "SmplStandardVertex must match StandardVertex");

	// mat3x4 row-major (48 B). Matches the SkinBone HLSL struct.
	struct BoneMatrix3x4
	{
		float Row0[4];
		float Row1[4];
		float Row2[4];
	};
	static_assert(sizeof(BoneMatrix3x4) == 48, "BoneMatrix3x4 size drift");

	// Append a box (24 verts, 36 indices) skinned rigidly to one bone. The
	// box is aligned to object-space axes; callers shape it via center and
	// halfExtents and pick the long axis to follow the bone direction.
	void AppendSkinnedBox(
		std::vector<SmplStandardVertex>&  outVerts,
		std::vector<UINT32>&              outIndices,
		std::vector<SmplSkinInputVertex>& outSkin,
		const glm::vec3& center,
		const glm::vec3& halfExtents,
		int              boneIndex)
	{
		// Six faces — each with its own normal/tangent so shading is per-face
		// crisp without per-vertex normal averaging across edges.
		struct FaceDef { glm::vec3 N, T, U, V; };
		static const FaceDef faces[6] = {
			{ { 0, 0, +1}, { 1, 0, 0}, { 1, 0, 0}, { 0, 1, 0} }, // +Z
			{ { 0, 0, -1}, {-1, 0, 0}, {-1, 0, 0}, { 0, 1, 0} }, // -Z
			{ {+1, 0, 0}, { 0, 0,-1}, { 0, 0,-1}, { 0, 1, 0} }, // +X
			{ {-1, 0, 0}, { 0, 0,+1}, { 0, 0, 1}, { 0, 1, 0} }, // -X
			{ { 0,+1, 0}, { 1, 0, 0}, { 1, 0, 0}, { 0, 0,-1} }, // +Y
			{ { 0,-1, 0}, { 1, 0, 0}, { 1, 0, 0}, { 0, 0, 1} }, // -Y
		};

		const UINT32 packedBone = static_cast<UINT32>(boneIndex & 0xFF);

		for (const FaceDef& f : faces)
		{
			const UINT32 base = static_cast<UINT32>(outVerts.size());
			const glm::vec3 faceCenter =
				center + f.N * glm::dot(halfExtents, glm::abs(f.N));
			const glm::vec3 uExt = f.U * glm::dot(halfExtents, glm::abs(f.U));
			const glm::vec3 vExt = f.V * glm::dot(halfExtents, glm::abs(f.V));

			// 4 verts per face, ordered (-,-), (+,-), (+,+), (-,+).
			const glm::vec3 corners[4] = {
				faceCenter - uExt - vExt,
				faceCenter + uExt - vExt,
				faceCenter + uExt + vExt,
				faceCenter - uExt + vExt,
			};
			const glm::vec2 uvs[4] = {
				{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f},
			};

			for (int i = 0; i < 4; ++i)
			{
				SmplStandardVertex sv = {};
				sv.Position = glm::vec4(corners[i], 1.0f);
				sv.UV       = uvs[i];
				sv.Normal   = f.N;
				sv.Tangent  = f.T;
				outVerts.push_back(sv);

				SmplSkinInputVertex iv = {};
				iv.BindPosition      = corners[i];
				iv.BindNormal        = f.N;
				iv.BindTangent       = f.T;
				iv.UV                = sv.UV;
				iv.BoneIndicesPacked = packedBone;
				iv.BoneWeights       = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
				outSkin.push_back(iv);
			}

			// Two triangles: 0-1-2, 0-2-3
			outIndices.push_back(base + 0);
			outIndices.push_back(base + 1);
			outIndices.push_back(base + 2);
			outIndices.push_back(base + 0);
			outIndices.push_back(base + 2);
			outIndices.push_back(base + 3);
		}
	}

	// Build the 18-box SMPL body in object space (meters, pelvis at origin).
	void BuildSmplBoxBody(
		std::vector<SmplStandardVertex>&  outVerts,
		std::vector<UINT32>&              outIndices,
		std::vector<SmplSkinInputVertex>& outSkin)
	{
		std::array<glm::vec3, CoronaSmpl::kJointCount> J;
		CoronaSmpl::ComputeBindPosePositions(J);
		using namespace CoronaSmpl;

		auto limbBox = [&](int controllingBone, int childJoint, float thickness)
		{
			const glm::vec3 a = J[controllingBone];
			const glm::vec3 b = J[childJoint];
			const glm::vec3 dir = b - a;
			const float len = glm::length(dir);
			if (len < 1e-4f) return;
			const glm::vec3 absDir = glm::abs(dir);
			int dom = 0;
			if (absDir.y > absDir[dom]) dom = 1;
			if (absDir.z > absDir[dom]) dom = 2;
			glm::vec3 half(thickness, thickness, thickness);
			half[dom] = len * 0.5f;
			const glm::vec3 center = (a + b) * 0.5f;
			AppendSkinnedBox(outVerts, outIndices, outSkin, center, half, controllingBone);
		};

		auto terminalBox = [&](int bone, const glm::vec3& halfExtents, const glm::vec3& localOffset)
		{
			const glm::vec3 center = J[bone] + localOffset;
			AppendSkinnedBox(outVerts, outIndices, outSkin, center, halfExtents, bone);
		};

		// Legs.
		limbBox(LeftHip,    LeftKnee,    0.07f);
		limbBox(RightHip,   RightKnee,   0.07f);
		limbBox(LeftKnee,   LeftAnkle,   0.06f);
		limbBox(RightKnee,  RightAnkle,  0.06f);
		limbBox(LeftAnkle,  LeftFoot,    0.05f);
		limbBox(RightAnkle, RightFoot,   0.05f);

		// Arms.
		limbBox(LeftShoulder,  LeftElbow,  0.05f);
		limbBox(RightShoulder, RightElbow, 0.05f);
		limbBox(LeftElbow,     LeftWrist,  0.045f);
		limbBox(RightElbow,    RightWrist, 0.045f);
		limbBox(LeftWrist,     LeftHand,   0.04f);
		limbBox(RightWrist,    RightHand,  0.04f);

		// Torso — controlled by the joint sitting at the bottom of each segment.
		limbBox(Pelvis,  Spine1, 0.13f); // hips block, ~0.124 m segment
		limbBox(Spine1,  Spine2, 0.13f);
		limbBox(Spine2,  Spine3, 0.13f);
		// Spine3 → Neck spans the chest; widen slightly.
		limbBox(Spine3,  Neck,   0.14f);
		// Neck → Head.
		limbBox(Neck,    Head,   0.05f);
		// Head terminal — block on top of the neck-head segment.
		terminalBox(Head, glm::vec3(0.09f, 0.10f, 0.09f), glm::vec3(0.0f, 0.08f, 0.0f));
	}

	// Pack a glm::mat4x4 as a row-major mat3x4 (drop the last row, which is
	// the (0,0,0,1) affine padding).
	BoneMatrix3x4 Pack3x4(const glm::mat4x4& m)
	{
		// glm matrices are column-major: m[col][row]. To produce row r col c
		// in row-major output we read m[c][r].
		BoneMatrix3x4 out = {};
		for (int c = 0; c < 4; ++c)
		{
			out.Row0[c] = m[c][0];
			out.Row1[c] = m[c][1];
			out.Row2[c] = m[c][2];
		}
		return out;
	}

	// Compute bind-pose world matrices (identity rotation, accumulated rest
	// offsets). One translation matrix per joint, walked through the tree.
	void ComputeBindWorld(std::array<glm::mat4x4, CoronaSmpl::kJointCount>& outBind)
	{
		const CoronaSmpl::JointInfo* tbl = CoronaSmpl::JointTable();
		for (int i = 0; i < CoronaSmpl::kJointCount; ++i)
		{
			const glm::mat4x4 localTranslate = glm::translate(glm::mat4x4(1.0f), tbl[i].RestOffset);
			if (tbl[i].Parent < 0)
				outBind[i] = localTranslate;
			else
				outBind[i] = outBind[tbl[i].Parent] * localTranslate;
		}
	}

	// Compute posed world matrices: rest translation followed by the joint's
	// local rotation, accumulated through the parent chain. The pelvis also
	// has its world-space RootTranslation applied.
	void ComputePosedWorld(
		const CoronaSmpl::PoseState&                       pose,
		std::array<glm::mat4x4, CoronaSmpl::kJointCount>&  outPosed)
	{
		const CoronaSmpl::JointInfo* tbl = CoronaSmpl::JointTable();
		for (int i = 0; i < CoronaSmpl::kJointCount; ++i)
		{
			const glm::mat4x4 R = glm::mat4_cast(pose.LocalRotations[i]);
			const glm::mat4x4 T = glm::translate(glm::mat4x4(1.0f), tbl[i].RestOffset);
			const glm::mat4x4 local = T * R;
			if (tbl[i].Parent < 0)
			{
				const glm::mat4x4 rootT =
					glm::translate(glm::mat4x4(1.0f), pose.RootTranslation);
				outPosed[i] = rootT * local;
			}
			else
			{
				outPosed[i] = outPosed[tbl[i].Parent] * local;
			}
		}
	}
}

// =================================================================
//                     Corona member functions
// =================================================================

bool Corona::SpawnSmplMotionCharacter(const glm::vec3& worldOrigin, float uniformScale)
{
	if (!renderBackend)
	{
		AppendCpuRuntimeTrace(L"[LLMAnim] SpawnSmplMotionCharacter aborted: renderBackend null");
		return false;
	}
	if (SmplMotionCharacter)
	{
		AppendCpuRuntimeTrace(L"[LLMAnim] SpawnSmplMotionCharacter: already spawned, reusing");
		return true;
	}
	{
		wchar_t buf[256];
		swprintf(buf, 256,
			L"[LLMAnim] SpawnSmplMotionCharacter origin=(%.2f,%.2f,%.2f) scale=%.2f",
			worldOrigin.x, worldOrigin.y, worldOrigin.z, uniformScale);
		AppendCpuRuntimeTrace(buf);
	}

	auto res = std::make_unique<CoronaSmpl::CharacterResources>();

	// 1) procedural mesh
	std::vector<SmplStandardVertex>  verts;
	std::vector<UINT32>              indices;
	std::vector<SmplSkinInputVertex> skin;
	verts.reserve(512);
	indices.reserve(1024);
	skin.reserve(512);
	BuildSmplBoxBody(verts, indices, skin);

	const UINT32 vertexCount = static_cast<UINT32>(verts.size());
	const UINT32 indexCount  = static_cast<UINT32>(indices.size());

	// 2) GPU buffers — own set, no unified-batch sharing.
	{
		BufferCreateDesc desc = {};
		desc.NumElements          = vertexCount;
		desc.ElementSize          = sizeof(SmplSkinInputVertex);
		desc.InitialState         = EInitialResourceState::ShaderRead;
		desc.bAllowUnorderedAccess = true;
		desc.InitialData          = skin.data();
		desc.Shape                = EBufferShape::Structured;
		res->InputVertices        = renderBackend->CreateBuffer(desc);
	}
	if (!res->InputVertices) return false;

	const BoneMatrix3x4 identityBone = {
		{1.0f, 0.0f, 0.0f, 0.0f},
		{0.0f, 1.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 1.0f, 0.0f},
	};
	std::vector<BoneMatrix3x4> identityPalette(CoronaSmpl::kJointCount, identityBone);
	res->BoneMatrices = renderBackend->CreateUploadStructuredBuffer(
		static_cast<UINT32>(identityPalette.size()),
		static_cast<UINT32>(sizeof(BoneMatrix3x4)));
	res->PrevBoneMatrices = renderBackend->CreateUploadStructuredBuffer(
		static_cast<UINT32>(identityPalette.size()),
		static_cast<UINT32>(sizeof(BoneMatrix3x4)));
	if (!res->BoneMatrices || !res->PrevBoneMatrices) return false;
	renderBackend->UpdateUploadStructuredBuffer(
		res->BoneMatrices.get(),
		identityPalette.data(),
		static_cast<UINT32>(identityPalette.size() * sizeof(BoneMatrix3x4)));
	renderBackend->UpdateUploadStructuredBuffer(
		res->PrevBoneMatrices.get(),
		identityPalette.data(),
		static_cast<UINT32>(identityPalette.size() * sizeof(BoneMatrix3x4)));

	res->BindVb = renderBackend->CreateVertexBuffer(
		static_cast<UINT32>(sizeof(SmplStandardVertex) * verts.size()),
		sizeof(SmplStandardVertex),
		verts.data());
	res->Ib = renderBackend->CreateIndexBuffer(
		EIndexFormat::U32,
		static_cast<UINT32>(sizeof(UINT32) * indices.size()),
		indices.data());
	res->OutputVb = renderBackend->CreateRWVertexBuffer(
		static_cast<UINT32>(sizeof(SmplStandardVertex) * verts.size()),
		sizeof(SmplStandardVertex));
	if (!res->BindVb || !res->Ib || !res->OutputVb) return false;

	// 3) material + mesh + scene
	auto material = std::make_shared<Material>();
	material->Diffuse   = DefaultWhiteTex;
	material->Normal    = DefaultNormalTex;
	material->Roughness = DefaultBlackTex;
	material->Metallic  = DefaultBlackTex;
	res->Mat = material;

	auto mesh = std::make_shared<Mesh>(renderBackend.get());
	mesh->transform     = glm::mat4x4(1.0f);
	mesh->NumVertices   = vertexCount;
	mesh->NumIndices    = indexCount;
	mesh->VertexStride  = sizeof(SmplStandardVertex);
	mesh->IndexFormat   = EIndexFormat::U32;
	mesh->Mat           = material;
	mesh->Vb            = res->BindVb;
	mesh->Ib            = res->Ib;
	for (const SmplStandardVertex& v : verts)
		mesh->CpuPositions.emplace_back(glm::vec3(v.Position));
	mesh->CpuIndices    = indices;

	Mesh::DrawCall drawCall = {};
	drawCall.mat         = material;
	drawCall.IndexStart  = 0;
	drawCall.IndexCount  = indexCount;
	drawCall.VertexBase  = 0;
	drawCall.VertexCount = vertexCount;
	mesh->Draws.push_back(drawCall);

	mesh->bSkeletalSkinned            = true;
	mesh->SkeletalVertexCount         = vertexCount;
	mesh->SkeletalBoneCount           = CoronaSmpl::kJointCount;
	mesh->SkeletalCharIndex           = 0;
	mesh->SkeletalInputVertices       = res->InputVertices;
	mesh->SkeletalBoneMatrices        = res->BoneMatrices;
	mesh->SkeletalPrevBoneMatrices    = res->PrevBoneMatrices;
	mesh->SkeletalOutputVb            = res->OutputVb;
	res->MeshPtr   = mesh;
	res->VertexCount = vertexCount;
	res->IndexCount  = indexCount;

	auto scene = std::make_shared<Scene>();
	scene->Materials.push_back(material);
	scene->meshes.push_back(mesh);
	scene->bHasBounds = true;
	scene->BoundsMin  = glm::vec3(-0.4f,  0.0f, -0.3f);
	scene->BoundsMax  = glm::vec3( 0.4f,  1.8f,  0.3f);
	res->ScenePtr = scene;

	// 4) bind-pose inverse cache for the per-frame palette compute.
	std::array<glm::mat4x4, CoronaSmpl::kJointCount> bindWorld;
	ComputeBindWorld(bindWorld);
	for (int i = 0; i < CoronaSmpl::kJointCount; ++i)
		res->BindWorldInverse[i] = glm::inverse(bindWorld[i]);

	// 5) attach to scene
	SceneObjectDesc desc;
	desc.ScenePtr     = scene;
	desc.Transform    =
		glm::translate(glm::mat4x4(1.0f), worldOrigin) *
		glm::scale(glm::mat4x4(1.0f), glm::vec3(uniformScale));
	desc.Roughness    = 0.6f;
	desc.Metallic     = 0.0f;
	desc.bOverrideRoughnessMetallic = true;
	desc.bRayTracing  = true;
	desc.bPhysicsQuery = false;
	const Corona::SceneObjectHandle handle = AddSceneObject(desc);
	if (handle == Corona::InvalidSceneObjectHandle)
	{
		AppendCpuRuntimeTrace(L"[LLMAnim] SpawnSmplMotionCharacter: AddSceneObject returned invalid handle");
		return false;
	}
	SmplMotionCharacterHandle = handle;

	SmplMotionCharacter = std::move(res);
	{
		wchar_t buf[256];
		swprintf(buf, 256,
			L"[LLMAnim] SpawnSmplMotionCharacter OK verts=%u indices=%u handle=%u",
			SmplMotionCharacter->VertexCount, SmplMotionCharacter->IndexCount,
			static_cast<unsigned>(handle));
		AppendCpuRuntimeTrace(buf);
	}
	return true;
}

void Corona::SetMotionClipForPlayback(
	CoronaMotion::MotionClip&& clip,
	const glm::vec3&           spawnWorldOrigin,
	float                      uniformScale)
{
	if (!MotionPlayback)
		MotionPlayback = std::make_unique<CoronaMotion::MotionPlayback>();
	MotionPlayback->SetClip(std::move(clip));
	bMotionTickInit = false;

	// Lazy-spawn one SMPL character the first time a clip arrives. Replays
	// of subsequent clips just swap the data inside MotionPlayback; the
	// character resources stay around.
	if (!SmplMotionCharacter)
		SpawnSmplMotionCharacter(spawnWorldOrigin, uniformScale);
}

void Corona::UpdateSmplMotionCharacterPalette()
{
	if (!SmplMotionCharacter || !renderBackend) return;
	if (!MotionPlayback) return;

	// Advance playback time using the real-time delta since the last call.
	const auto now = std::chrono::steady_clock::now();
	if (!bMotionTickInit)
	{
		MotionLastTickTime = now;
		bMotionTickInit = true;
	}
	const float dt = std::chrono::duration<float>(now - MotionLastTickTime).count();
	MotionLastTickTime = now;
	MotionPlayback->Tick(std::clamp(dt, 0.0f, 0.1f));

	CoronaSmpl::PoseState pose;
	if (!MotionPlayback->EvaluateCurrentPose(pose))
		return;

	std::array<glm::mat4x4, CoronaSmpl::kJointCount> posedWorld;
	ComputePosedWorld(pose, posedWorld);

	std::array<BoneMatrix3x4, CoronaSmpl::kJointCount> palette;
	for (int i = 0; i < CoronaSmpl::kJointCount; ++i)
	{
		const glm::mat4x4 boneMatrix = posedWorld[i] * SmplMotionCharacter->BindWorldInverse[i];
		palette[i] = Pack3x4(boneMatrix);
	}

	// Rotate current → prev (motion vectors), then upload the new current.
	std::array<BoneMatrix3x4, CoronaSmpl::kJointCount> prev = palette; // placeholder
	// In a tighter motion-vector implementation we'd keep the previous-frame
	// palette across frames; for the first pass we use the same palette for
	// both so motion vectors are zero on the SMPL character. That keeps TAA
	// stable; the visible cost is no streaking on fast motion which is fine
	// for the bring-up phase.

	renderBackend->UpdateUploadStructuredBuffer(
		SmplMotionCharacter->PrevBoneMatrices.get(),
		prev.data(),
		static_cast<UINT32>(prev.size() * sizeof(BoneMatrix3x4)));
	renderBackend->UpdateUploadStructuredBuffer(
		SmplMotionCharacter->BoneMatrices.get(),
		palette.data(),
		static_cast<UINT32>(palette.size() * sizeof(BoneMatrix3x4)));
}
