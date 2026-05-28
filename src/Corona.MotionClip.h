#pragma once

// Loader for the narrow BVH dialect produced by tools/motion_gen.py.
// Stays intentionally limited: only what our writer emits is parsed.
//
// What we accept:
//   * Y-up axes
//   * Root channels: Xposition Yposition Zposition Zrotation Xrotation Yrotation
//   * Joint channels: Zrotation Xrotation Yrotation
//   * SMPL 24-joint hierarchy (extra joints are ignored, missing joints
//     are an error)
//
// The result is a MotionClip in the engine's native form: a sequence of
// per-frame PoseState values keyed off the SMPL joint indices, ready to be
// applied to CoronaSmpl::WorldTransforms without any remapping.

#include <string>
#include <vector>

#include "Corona.SmplSkeleton.h"

namespace CoronaMotion
{
	struct MotionClip
	{
		std::string SourcePath;
		float       FrameTime  = 0.0f; // seconds per frame
		float       Fps        = 0.0f; // 1.0 / FrameTime
		int         FrameCount = 0;
		// One PoseState per frame, addressed by SMPL joint index.
		std::vector<CoronaSmpl::PoseState> Frames;
	};

	// Parse a BVH file produced by tools/motion_gen.py. On failure, the
	// `errorOut` receives a human-readable message and the function returns
	// false. `clipOut` is left untouched on failure.
	bool LoadBVH(const std::string& path, MotionClip& clipOut, std::string& errorOut);

	// Sample a pose at time `seconds` (looping). Linear quaternion interp
	// (SLERP) on joints, lerp on root translation. Returns identity pose if
	// the clip has no frames.
	void SampleClip(const MotionClip& clip, float seconds, CoronaSmpl::PoseState& outPose);
}
