#pragma once

// MotionPlayback — drives an SMPL pose from a MotionClip.
//
// Used by Corona.Console: after a successful `genmotion`, the clip is
// handed to MotionPlayback, which advances playback time on each Tick()
// and exposes the current PoseState. The visualisation system
// (Corona.SmplCharacter) reads from EvaluateCurrentPose() each frame to
// drive its bone palette.

#include "Corona.MotionClip.h"
#include "Corona.SmplSkeleton.h"

#include "glm/glm.hpp"

namespace CoronaMotion
{
	class MotionPlayback
	{
	public:
		// Replace the active clip and reset playback time.
		void SetClip(MotionClip&& clip);
		void Clear();

		bool HasClip() const { return bHasClip; }

		// World-space anchor for the pelvis (added on top of the clip's own
		// root translation). Set by the spawning code (e.g. terrain).
		void SetWorldAnchor(const glm::vec3& pos);
		const glm::vec3& WorldAnchor() const { return Anchor; }

		// Advance playback by `dt` seconds. No-op when no clip is set.
		void Tick(float dt);

		// Snapshot of the current pose (sampled at PlayTime). Returns true
		// when a clip is loaded; false otherwise (out is set to identity).
		bool EvaluateCurrentPose(CoronaSmpl::PoseState& outPose) const;

	private:
		MotionClip Clip;
		bool       bHasClip = false;
		float      PlayTime = 0.0f;
		glm::vec3  Anchor   = glm::vec3(0.0f);
	};
}
