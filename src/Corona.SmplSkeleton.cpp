// Corona.SmplSkeleton.cpp — engine-side SMPL 24-joint table + helpers.
// Mirrors tools/motion_server/smpl_skeleton.py exactly.

#include "stdafx.h"
#include "Corona.SmplSkeleton.h"

namespace CoronaSmpl
{
	static const JointInfo kTable[kJointCount] =
	{
		// idx 0
		{ "pelvis",         -1, glm::vec3( 0.000f,  0.000f,  0.000f) },
		// 1, 2 — hips
		{ "left_hip",        0, glm::vec3( 0.058f, -0.082f,  0.014f) },
		{ "right_hip",       0, glm::vec3(-0.060f, -0.090f,  0.013f) },
		// 3 — spine1
		{ "spine1",          0, glm::vec3( 0.005f,  0.124f, -0.038f) },
		// 4, 5 — knees
		{ "left_knee",       1, glm::vec3( 0.039f, -0.391f, -0.011f) },
		{ "right_knee",      2, glm::vec3(-0.039f, -0.404f, -0.013f) },
		// 6 — spine2
		{ "spine2",          3, glm::vec3(-0.004f,  0.139f,  0.026f) },
		// 7, 8 — ankles
		{ "left_ankle",      4, glm::vec3(-0.028f, -0.418f, -0.039f) },
		{ "right_ankle",     5, glm::vec3( 0.030f, -0.421f, -0.039f) },
		// 9 — spine3
		{ "spine3",          6, glm::vec3(-0.004f,  0.055f,  0.005f) },
		// 10, 11 — feet
		{ "left_foot",       7, glm::vec3( 0.029f, -0.064f,  0.130f) },
		{ "right_foot",      8, glm::vec3(-0.030f, -0.061f,  0.131f) },
		// 12 — neck
		{ "neck",            9, glm::vec3( 0.011f,  0.219f, -0.039f) },
		// 13, 14 — collars
		{ "left_collar",     9, glm::vec3(-0.071f,  0.114f, -0.018f) },
		{ "right_collar",    9, glm::vec3( 0.083f,  0.114f, -0.022f) },
		// 15 — head
		{ "head",           12, glm::vec3( 0.001f,  0.089f,  0.050f) },
		// 16, 17 — shoulders
		{ "left_shoulder",  13, glm::vec3( 0.121f,  0.046f, -0.008f) },
		{ "right_shoulder", 14, glm::vec3(-0.114f,  0.047f, -0.010f) },
		// 18, 19 — elbows
		{ "left_elbow",     16, glm::vec3( 0.255f, -0.015f, -0.022f) },
		{ "right_elbow",    17, glm::vec3(-0.260f, -0.013f, -0.022f) },
		// 20, 21 — wrists
		{ "left_wrist",     18, glm::vec3( 0.265f,  0.011f, -0.007f) },
		{ "right_wrist",    19, glm::vec3(-0.269f,  0.007f, -0.006f) },
		// 22, 23 — hands
		{ "left_hand",      20, glm::vec3( 0.087f, -0.010f, -0.018f) },
		{ "right_hand",     21, glm::vec3(-0.089f, -0.007f, -0.018f) },
	};

	const JointInfo* JointTable() { return kTable; }

	int JointIndexByName(const std::string& name)
	{
		for (int i = 0; i < kJointCount; ++i)
		{
			if (name == kTable[i].Name) return i;
		}
		return -1;
	}

	void ComputeBindPosePositions(std::array<glm::vec3, kJointCount>& out)
	{
		for (int i = 0; i < kJointCount; ++i)
		{
			const JointInfo& j = kTable[i];
			if (j.Parent < 0)
				out[i] = j.RestOffset;
			else
				out[i] = out[j.Parent] + j.RestOffset;
		}
	}

	void InitIdentityPose(PoseState& pose)
	{
		for (auto& q : pose.LocalRotations)
			q = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		pose.RootTranslation = glm::vec3(0.0f);
	}

	void ComputeWorldTransforms(const PoseState& pose, WorldTransforms& outWorld)
	{
		for (int i = 0; i < kJointCount; ++i)
		{
			const JointInfo& j = kTable[i];
			const glm::quat& localRot = pose.LocalRotations[i];

			if (j.Parent < 0)
			{
				// Pelvis sits at RootTranslation. RestOffset is (0,0,0) by
				// convention but we honor it for completeness.
				outWorld.WorldRotations[i] = localRot;
				outWorld.Positions[i]      = pose.RootTranslation + j.RestOffset;
			}
			else
			{
				const glm::quat& parentWorldRot = outWorld.WorldRotations[j.Parent];
				outWorld.WorldRotations[i] = parentWorldRot * localRot;
				outWorld.Positions[i]      =
					outWorld.Positions[j.Parent] + (parentWorldRot * j.RestOffset);
			}
		}
	}
}
