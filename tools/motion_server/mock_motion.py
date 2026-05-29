"""Mock motion generator.

Produces a procedurally synthesized SMPL motion based on simple sine waves,
keyed off keywords in the user text. Used to close the loop between the
engine and the server without requiring MoMask, GPU, or SMPL licenses.

Keyword cues:
- "walk", "run"    -> opposing leg/arm swing + forward root translation
- "wave"           -> right arm raised and oscillating
- "jump"           -> single vertical hop on the root
- (default)        -> idle: gentle spine sway

All other joints stay in T-pose (zero rotation).
"""

import math
import random
from typing import List, Tuple

from smpl_skeleton import NUM_JOINTS

# SMPL joint indices used here
PELVIS = 0
L_HIP = 1
R_HIP = 2
L_KNEE = 4
R_KNEE = 5
SPINE1 = 3
SPINE2 = 6
L_SHOULDER = 16
R_SHOULDER = 17
L_ELBOW = 18
R_ELBOW = 19


def _zero_pose() -> List[Tuple[float, float, float]]:
    return [(0.0, 0.0, 0.0) for _ in range(NUM_JOINTS)]


def _detect_action(text: str) -> str:
    t = text.lower()
    if "jump" in t:
        return "jump"
    if "wave" in t:
        return "wave"
    if "run" in t or "walk" in t:
        return "walk"
    return "idle"


def generate(
    text: str,
    duration: float = 3.0,
    fps: int = 20,
    seed: int = 0,
) -> Tuple[float, List[Tuple[float, float, float]], List[List[Tuple[float, float, float]]]]:
    """Synthesize a mock motion clip.

    Returns:
        frame_time, root_translations, per_frame_joint_euler_zxy_deg
    """
    random.seed(seed)
    action = _detect_action(text)
    frame_time = 1.0 / float(fps)
    n_frames = max(1, int(round(duration * fps)))

    # RootTranslation here is *relative to the bind-pose pelvis* — the
    # engine builds its SMPL bind pose with pelvis at object origin, so
    # we report only deviations (forward motion, hop, idle bob). The
    # engine spawn code adds the pelvis-from-feet offset (~0.95 m) to
    # the world spawn Y so feet land on the ground.
    base_y = 0.0

    root_translations: List[Tuple[float, float, float]] = []
    poses: List[List[Tuple[float, float, float]]] = []

    for f in range(n_frames):
        s = f / float(fps)  # seconds
        pose = _zero_pose()
        rx, ry, rz = 0.0, base_y, 0.0

        if action == "walk":
            # 1 Hz stride, 0.6 m/s forward.
            phase = 2.0 * math.pi * 1.0 * s
            swing = 25.0 * math.sin(phase)  # degrees
            knee_bend = 18.0 * max(0.0, math.sin(phase))
            knee_bend_r = 18.0 * max(0.0, -math.sin(phase))
            arm_swing = 20.0 * math.sin(phase)

            pose[L_HIP] = (0.0, swing, 0.0)
            pose[R_HIP] = (0.0, -swing, 0.0)
            pose[L_KNEE] = (0.0, -knee_bend, 0.0)
            pose[R_KNEE] = (0.0, -knee_bend_r, 0.0)
            pose[L_SHOULDER] = (0.0, -arm_swing, 0.0)
            pose[R_SHOULDER] = (0.0, arm_swing, 0.0)

            rz = -0.6 * s  # forward translation along -Z
            ry = base_y + 0.02 * math.sin(2.0 * phase)

        elif action == "wave":
            # Hold right arm up, oscillate the elbow.
            phase = 2.0 * math.pi * 2.0 * s
            pose[R_SHOULDER] = (0.0, -110.0, 0.0)
            pose[R_ELBOW] = (0.0, 30.0 * math.sin(phase), 0.0)
            pose[SPINE1] = (0.0, 0.0, 3.0 * math.sin(0.5 * phase))

        elif action == "jump":
            # Single hop over the whole clip.
            t01 = f / max(1, n_frames - 1)
            crouch = 35.0 if 0.0 <= t01 < 0.25 else 0.0
            hop = 0.0
            if 0.25 <= t01 < 0.75:
                # Parabolic-ish arc.
                u = (t01 - 0.25) / 0.5
                hop = 0.5 * math.sin(math.pi * u)
            land = 35.0 if 0.75 <= t01 <= 1.0 else 0.0
            knee = crouch + land
            pose[L_KNEE] = (0.0, -knee, 0.0)
            pose[R_KNEE] = (0.0, -knee, 0.0)
            pose[L_HIP] = (0.0, knee * 0.5, 0.0)
            pose[R_HIP] = (0.0, knee * 0.5, 0.0)
            ry = base_y + hop

        else:
            # Idle: gentle spine sway + arm float.
            phase = 2.0 * math.pi * 0.5 * s
            pose[SPINE1] = (2.0 * math.sin(phase), 0.0, 0.0)
            pose[SPINE2] = (1.0 * math.sin(phase + 0.3), 0.0, 0.0)
            ry = base_y + 0.01 * math.sin(phase)

        root_translations.append((rx, ry, rz))
        poses.append(pose)

    return frame_time, root_translations, poses
