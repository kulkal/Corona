#pragma once

// SMPL 24-joint procedural skeleton — engine-side mirror of
// tools/motion_server/smpl_skeleton.py. The two tables must stay in lock
// step so the Python BVH writer and the C++ loader/retargeter agree on
// joint ordering, parent indices, and rest-pose offsets.
//
// Used by:
//   - BVH loader (Corona.MotionClip.*)
//   - LLM motion playback / retargeting (Corona.MotionPlayback.*)
//   - Debug line visualisation
//
// Design doc: docs/design/llm_motion_generation_pipeline.md

#include <array>
#include <string>

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

namespace CoronaSmpl
{
	constexpr int kJointCount = 24;

	// Canonical SMPL joint ordering. Indices match the SMPL paper and every
	// downstream motion-gen model we expect to consume (MoMask, MotionGPT,
	// HumanML3D pipelines, etc.).
	enum Joint : int
	{
		Pelvis = 0,
		LeftHip,
		RightHip,
		Spine1,
		LeftKnee,
		RightKnee,
		Spine2,
		LeftAnkle,
		RightAnkle,
		Spine3,
		LeftFoot,
		RightFoot,
		Neck,
		LeftCollar,
		RightCollar,
		Head,
		LeftShoulder,
		RightShoulder,
		LeftElbow,
		RightElbow,
		LeftWrist,
		RightWrist,
		LeftHand,
		RightHand,
	};

	struct JointInfo
	{
		const char* Name;
		int         Parent;     // -1 only for pelvis
		glm::vec3   RestOffset; // translation from parent in bind T-pose (meters)
	};

	// 24-entry joint table. Backed by file-static storage; the returned
	// pointer is valid for the lifetime of the process.
	const JointInfo* JointTable();

	// Linear scan over the 24 entries. Returns -1 when not found.
	int JointIndexByName(const std::string& name);

	// World-space bind-pose joint positions (pelvis at origin). Just walks
	// the parent chain accumulating rest offsets.
	void ComputeBindPosePositions(std::array<glm::vec3, kJointCount>& outPositions);

	// A single posed-character state ready to be evaluated.
	struct PoseState
	{
		std::array<glm::quat, kJointCount> LocalRotations; // parent-relative
		glm::vec3                          RootTranslation = glm::vec3(0.0f);
	};

	void InitIdentityPose(PoseState& pose);

	// World transforms derived from a PoseState. Positions[i] is the joint
	// head in world space; WorldRotations[i] is the cumulative world rotation
	// at that joint (parent_world * local). Use this to drive line debug
	// rendering or downstream skinning.
	struct WorldTransforms
	{
		std::array<glm::vec3, kJointCount> Positions;
		std::array<glm::quat, kJointCount> WorldRotations;
	};

	void ComputeWorldTransforms(const PoseState& pose, WorldTransforms& outWorld);
}
