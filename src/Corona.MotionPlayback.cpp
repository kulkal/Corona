// Corona.MotionPlayback.cpp — pose driver for an SMPL motion clip.

#include "stdafx.h"
#include "Corona.MotionPlayback.h"

namespace CoronaMotion
{
	void MotionPlayback::SetClip(MotionClip&& clip)
	{
		Clip      = std::move(clip);
		bHasClip  = (Clip.FrameCount > 0);
		PlayTime  = 0.0f;
	}

	void MotionPlayback::Clear()
	{
		Clip      = MotionClip{};
		bHasClip  = false;
		PlayTime  = 0.0f;
	}

	void MotionPlayback::SetWorldAnchor(const glm::vec3& pos)
	{
		Anchor = pos;
	}

	void MotionPlayback::Tick(float dt)
	{
		if (!bHasClip) return;
		PlayTime += dt;
		// Sampling does the looping; we just keep advancing.
	}

	bool MotionPlayback::EvaluateCurrentPose(CoronaSmpl::PoseState& outPose) const
	{
		if (!bHasClip)
		{
			CoronaSmpl::InitIdentityPose(outPose);
			return false;
		}
		SampleClip(Clip, PlayTime, outPose);
		// Layer the world anchor on top of the clip-internal root translation.
		outPose.RootTranslation += Anchor;
		return true;
	}
}
