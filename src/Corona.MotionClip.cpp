// Corona.MotionClip.cpp — BVH loader scoped to the dialect emitted by
// tools/motion_gen.py (Y-up, SMPL 24-joint, ZXY-Euler joint channels).
//
// Design doc: docs/design/llm_motion_generation_pipeline.md

#include "stdafx.h"
#include "Corona.MotionClip.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "glm/gtc/quaternion.hpp"

namespace CoronaMotion
{
	namespace
	{
		struct ParsedJoint
		{
			std::string Name;
			int         Parent = -1;
			glm::vec3   Offset = glm::vec3(0.0f);
			// Channel layout in the BVH motion stream, in order. Encoded as
			// 0..5 = Xpos, Ypos, Zpos, Xrot, Yrot, Zrot. -1 = absent.
			std::vector<int> Channels;
			int         FirstChannelInRow = -1; // start column in motion row
		};

		enum Channel : int
		{
			ChanXpos = 0, ChanYpos, ChanZpos,
			ChanXrot, ChanYrot, ChanZrot,
		};

		static const std::unordered_map<std::string, int>& ChannelNameMap()
		{
			static const std::unordered_map<std::string, int> m = {
				{"Xposition", ChanXpos}, {"Yposition", ChanYpos}, {"Zposition", ChanZpos},
				{"Xrotation", ChanXrot}, {"Yrotation", ChanYrot}, {"Zrotation", ChanZrot},
			};
			return m;
		}

		// Tokenizer that ignores braces and treats them as their own tokens.
		std::vector<std::string> TokenizeLine(const std::string& line)
		{
			std::vector<std::string> out;
			std::string cur;
			auto flush = [&]() { if (!cur.empty()) { out.push_back(cur); cur.clear(); } };
			for (char c : line)
			{
				if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { flush(); }
				else if (c == '{' || c == '}') { flush(); out.push_back(std::string(1, c)); }
				else { cur.push_back(c); }
			}
			flush();
			return out;
		}

		// Apply Euler rotation in degrees with intrinsic Z·X·Y order (matches
		// the BVH channel order our writer produces).
		glm::quat EulerZXYDegToQuat(float zDeg, float xDeg, float yDeg)
		{
			const float toRad = 3.14159265358979323846f / 180.0f;
			const float z = zDeg * toRad;
			const float x = xDeg * toRad;
			const float y = yDeg * toRad;
			// Intrinsic Z then X then Y = quat_z * quat_x * quat_y in
			// right-to-left composition.
			const glm::quat qz(std::cos(z * 0.5f), 0.0f, 0.0f, std::sin(z * 0.5f));
			const glm::quat qx(std::cos(x * 0.5f), std::sin(x * 0.5f), 0.0f, 0.0f);
			const glm::quat qy(std::cos(y * 0.5f), 0.0f, std::sin(y * 0.5f), 0.0f);
			return qz * qx * qy;
		}
	}

	bool LoadBVH(const std::string& path, MotionClip& clipOut, std::string& errorOut)
	{
		std::ifstream f(path);
		if (!f.is_open())
		{
			errorOut = "cannot open BVH file: " + path;
			return false;
		}

		std::vector<ParsedJoint> joints;
		std::vector<int>         stack; // joint-index stack during HIERARCHY walk

		// --- Phase 1: HIERARCHY ---
		std::string line;
		bool inHierarchy = false;
		bool inMotion    = false;
		int  totalChannels = 0;

		auto pushJoint = [&](const std::string& name, bool isRoot) {
			ParsedJoint pj;
			pj.Name   = name;
			pj.Parent = stack.empty() ? -1 : stack.back();
			(void)isRoot;
			joints.push_back(std::move(pj));
			stack.push_back(static_cast<int>(joints.size()) - 1);
		};

		while (std::getline(f, line))
		{
			auto tok = TokenizeLine(line);
			if (tok.empty()) continue;

			if (tok[0] == "HIERARCHY") { inHierarchy = true; continue; }
			if (tok[0] == "MOTION")    { inMotion = true;    break; }

			if (!inHierarchy) continue;

			if (tok[0] == "ROOT" || tok[0] == "JOINT")
			{
				if (tok.size() < 2) { errorOut = "ROOT/JOINT missing name"; return false; }
				pushJoint(tok[1], tok[0] == "ROOT");
			}
			else if (tok[0] == "End")
			{
				// End Site — push a sentinel on the stack so the matching }
				// pops correctly, but skip recording it as a real joint.
				stack.push_back(-2);
			}
			else if (tok[0] == "{")
			{
				// Already pushed by ROOT/JOINT/End Site.
			}
			else if (tok[0] == "}")
			{
				if (!stack.empty()) stack.pop_back();
			}
			else if (tok[0] == "OFFSET")
			{
				if (tok.size() < 4) { errorOut = "OFFSET expects 3 floats"; return false; }
				if (!stack.empty() && stack.back() >= 0)
				{
					int idx = stack.back();
					joints[idx].Offset.x = std::stof(tok[1]);
					joints[idx].Offset.y = std::stof(tok[2]);
					joints[idx].Offset.z = std::stof(tok[3]);
				}
			}
			else if (tok[0] == "CHANNELS")
			{
				if (tok.size() < 2) { errorOut = "CHANNELS missing count"; return false; }
				const int n = std::stoi(tok[1]);
				if (static_cast<int>(tok.size()) < 2 + n)
				{
					errorOut = "CHANNELS count vs tokens mismatch";
					return false;
				}
				if (stack.empty() || stack.back() < 0)
				{
					errorOut = "CHANNELS outside JOINT scope";
					return false;
				}
				int idx = stack.back();
				joints[idx].FirstChannelInRow = totalChannels;
				const auto& nameMap = ChannelNameMap();
				for (int c = 0; c < n; ++c)
				{
					auto it = nameMap.find(tok[2 + c]);
					if (it == nameMap.end())
					{
						errorOut = "unknown channel: " + tok[2 + c];
						return false;
					}
					joints[idx].Channels.push_back(it->second);
				}
				totalChannels += n;
			}
		}

		if (!inMotion)
		{
			errorOut = "MOTION section not found";
			return false;
		}

		// --- Phase 2: MOTION header ---
		int frameCount = -1;
		float frameTime = -1.0f;
		for (int header = 0; header < 2; ++header)
		{
			if (!std::getline(f, line)) { errorOut = "unexpected EOF in MOTION header"; return false; }
			auto tok = TokenizeLine(line);
			if (tok.empty()) { --header; continue; }
			if (tok[0] == "Frames:")
			{
				if (tok.size() < 2) { errorOut = "Frames: missing value"; return false; }
				frameCount = std::stoi(tok[1]);
			}
			else if (tok[0] == "Frame" && tok.size() >= 2 && tok[1] == "Time:")
			{
				if (tok.size() < 3) { errorOut = "Frame Time: missing value"; return false; }
				frameTime = std::stof(tok[2]);
			}
			else
			{
				--header; // not a header line; retry. Shouldn't normally happen.
			}
		}
		if (frameCount <= 0 || frameTime <= 0.0f)
		{
			errorOut = "invalid frame count or frame time";
			return false;
		}

		// --- Phase 3: motion rows ---
		// Build a name → SMPL index map so we can look up joints quickly.
		const int kJoints = CoronaSmpl::kJointCount;
		std::vector<int> bvhToSmpl(joints.size(), -1);
		for (size_t i = 0; i < joints.size(); ++i)
			bvhToSmpl[i] = CoronaSmpl::JointIndexByName(joints[i].Name);

		// Sanity: pelvis must be present.
		bool havePelvis = false;
		for (int idx : bvhToSmpl) { if (idx == CoronaSmpl::Pelvis) havePelvis = true; }
		if (!havePelvis)
		{
			errorOut = "BVH hierarchy lacks SMPL pelvis joint";
			return false;
		}

		std::vector<CoronaSmpl::PoseState> frames;
		frames.reserve(frameCount);

		for (int f_ = 0; f_ < frameCount; ++f_)
		{
			if (!std::getline(f, line)) { errorOut = "EOF before all frames read"; return false; }
			std::istringstream is(line);
			std::vector<float> row;
			row.reserve(totalChannels);
			float v;
			while (is >> v) row.push_back(v);
			if (static_cast<int>(row.size()) < totalChannels)
			{
				errorOut = "frame " + std::to_string(f_) + " short on channels";
				return false;
			}

			CoronaSmpl::PoseState pose;
			CoronaSmpl::InitIdentityPose(pose);

			for (size_t bi = 0; bi < joints.size(); ++bi)
			{
				const ParsedJoint& bj = joints[bi];
				if (bj.FirstChannelInRow < 0) continue;
				const int smpl = bvhToSmpl[bi];

				float xpos = 0, ypos = 0, zpos = 0;
				float xrot = 0, yrot = 0, zrot = 0;
				for (size_t c = 0; c < bj.Channels.size(); ++c)
				{
					const float val = row[bj.FirstChannelInRow + c];
					switch (bj.Channels[c])
					{
					case ChanXpos: xpos = val; break;
					case ChanYpos: ypos = val; break;
					case ChanZpos: zpos = val; break;
					case ChanXrot: xrot = val; break;
					case ChanYrot: yrot = val; break;
					case ChanZrot: zrot = val; break;
					}
				}

				if (smpl < 0) continue; // joint not in SMPL spec — ignore

				if (smpl == CoronaSmpl::Pelvis)
				{
					pose.RootTranslation = glm::vec3(xpos, ypos, zpos);
				}
				pose.LocalRotations[smpl] = EulerZXYDegToQuat(zrot, xrot, yrot);
			}

			frames.push_back(std::move(pose));
		}

		clipOut.SourcePath = path;
		clipOut.FrameTime  = frameTime;
		clipOut.Fps        = (frameTime > 0.0f) ? (1.0f / frameTime) : 0.0f;
		clipOut.FrameCount = frameCount;
		clipOut.Frames     = std::move(frames);
		(void)kJoints;
		return true;
	}

	void SampleClip(const MotionClip& clip, float seconds, CoronaSmpl::PoseState& outPose)
	{
		if (clip.FrameCount <= 0)
		{
			CoronaSmpl::InitIdentityPose(outPose);
			return;
		}
		// Loop.
		const float totalSec = static_cast<float>(clip.FrameCount) * clip.FrameTime;
		float t = std::fmod(seconds, totalSec);
		if (t < 0.0f) t += totalSec;
		const float ff = t / clip.FrameTime;
		int   i0 = static_cast<int>(std::floor(ff));
		int   i1 = (i0 + 1) % clip.FrameCount;
		const float a = ff - static_cast<float>(i0);
		if (i0 >= clip.FrameCount) i0 = clip.FrameCount - 1;

		const auto& A = clip.Frames[i0];
		const auto& B = clip.Frames[i1];
		outPose.RootTranslation = glm::mix(A.RootTranslation, B.RootTranslation, a);
		for (int j = 0; j < CoronaSmpl::kJointCount; ++j)
			outPose.LocalRotations[j] = glm::slerp(A.LocalRotations[j], B.LocalRotations[j], a);
	}
}
