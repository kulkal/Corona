"""Mock motion generator.

Produces a procedurally synthesized SMPL motion built up in two layers:

Layer 1 — "alive" base: every joint gets small per-axis sinusoidal noise
with its own frequency and phase, so even an "idle" character never sits
fully still and a walking character has secondary motion throughout the
body, not just the swinging limbs.

Layer 2 — action overlay: keyword-driven primary motion (walk / wave /
jump / idle) layered on top of the alive base.

Keyword cues:
- "walk", "run"    -> opposing leg/arm swing, spine counter-rotation,
                       forward root motion
- "wave"           -> right arm raised and oscillating, body lean,
                       head tilt
- "jump"           -> crouch / extend / land with arm swing-through
- (default)        -> idle: weight shift, breathing, gaze drift

Convention: each joint rotation tuple is (Zrot, Xrot, Yrot) in degrees,
matching the BVH channel order our writer emits and the SMPL Z·X·Y
intrinsic-Euler convention we settled on.

Axis intuition in SMPL T-pose (Y up, body facing -Z, arms along ±X):
- X-rotation on a shoulder/hip → arm/leg swings forward/back (sagittal)
- Z-rotation on a shoulder/hip → arm/leg moves to the side (frontal)
- Y-rotation on a shoulder/hip → arm/leg twists around its own length
"""

import math
import random
from typing import List, Tuple

from smpl_skeleton import NUM_JOINTS

# SMPL joint indices used here
PELVIS = 0
L_HIP, R_HIP = 1, 2
SPINE1 = 3
L_KNEE, R_KNEE = 4, 5
SPINE2 = 6
L_ANKLE, R_ANKLE = 7, 8
SPINE3 = 9
L_FOOT, R_FOOT = 10, 11
NECK = 12
L_COLLAR, R_COLLAR = 13, 14
HEAD = 15
L_SHOULDER, R_SHOULDER = 16, 17
L_ELBOW, R_ELBOW = 18, 19
L_WRIST, R_WRIST = 20, 21
L_HAND, R_HAND = 22, 23


def _zero_pose() -> List[List[float]]:
    # Mutable lists per joint so we can additively layer rotations.
    return [[0.0, 0.0, 0.0] for _ in range(NUM_JOINTS)]


def _add(pose: List[List[float]], idx: int, z: float, x: float, y: float) -> None:
    pose[idx][0] += z
    pose[idx][1] += x
    pose[idx][2] += y


def _detect_action(text: str) -> str:
    t = text.lower()
    if "jump" in t:
        return "jump"
    if "wave" in t:
        return "wave"
    if "run" in t or "walk" in t:
        return "walk"
    return "idle"


# Per-joint alive-noise parameters: (freq_hz, amp_deg) for each axis (Z,X,Y).
# Higher amplitude on big chain joints (spine, hips), smaller on fingers.
def _alive_amplitudes() -> List[Tuple[Tuple[float, float], ...]]:
    """Returns a per-joint tuple ((freq,amp_z),(freq,amp_x),(freq,amp_y))."""
    amp: List[Tuple[Tuple[float, float], ...]] = [((0.0, 0.0),) * 3 for _ in range(NUM_JOINTS)]

    def set_j(i, fz, az, fx, ax, fy, ay):
        amp[i] = ((fz, az), (fx, ax), (fy, ay))

    # Spine column — gentle sway and bob.
    set_j(PELVIS, 0.35, 1.5, 0.30, 1.0, 0.45, 1.8)
    set_j(SPINE1, 0.40, 1.2, 0.35, 0.9, 0.50, 1.4)
    set_j(SPINE2, 0.42, 1.0, 0.37, 0.8, 0.52, 1.2)
    set_j(SPINE3, 0.45, 0.9, 0.40, 0.7, 0.55, 1.0)
    set_j(NECK,   0.55, 1.5, 0.45, 1.2, 0.65, 1.8)
    set_j(HEAD,   0.50, 2.5, 0.40, 2.0, 0.60, 2.8)
    # Collars — micro shoulder-roll.
    set_j(L_COLLAR, 0.30, 0.8, 0.35, 0.6, 0.40, 0.5)
    set_j(R_COLLAR, 0.32, 0.8, 0.37, 0.6, 0.42, 0.5)
    # Shoulders — subtle sway.
    set_j(L_SHOULDER, 0.45, 2.0, 0.50, 2.5, 0.55, 1.5)
    set_j(R_SHOULDER, 0.47, 2.0, 0.52, 2.5, 0.57, 1.5)
    # Elbows — small bend variation.
    set_j(L_ELBOW, 0.60, 1.5, 0.65, 2.0, 0.70, 1.0)
    set_j(R_ELBOW, 0.62, 1.5, 0.67, 2.0, 0.72, 1.0)
    # Wrists / hands — bigger flick.
    set_j(L_WRIST, 0.80, 3.0, 0.85, 3.5, 0.90, 2.5)
    set_j(R_WRIST, 0.82, 3.0, 0.87, 3.5, 0.92, 2.5)
    set_j(L_HAND,  1.10, 4.0, 1.20, 4.5, 1.30, 3.0)
    set_j(R_HAND,  1.15, 4.0, 1.25, 4.5, 1.35, 3.0)
    # Hips / knees / ankles — quiet under idle (the walk layer drives them).
    set_j(L_HIP, 0.50, 0.5, 0.55, 0.4, 0.60, 0.3)
    set_j(R_HIP, 0.52, 0.5, 0.57, 0.4, 0.62, 0.3)
    set_j(L_KNEE, 0.65, 0.4, 0.70, 0.3, 0.75, 0.2)
    set_j(R_KNEE, 0.67, 0.4, 0.72, 0.3, 0.77, 0.2)
    set_j(L_ANKLE, 0.80, 0.6, 0.85, 0.5, 0.90, 0.4)
    set_j(R_ANKLE, 0.82, 0.6, 0.87, 0.5, 0.92, 0.4)
    set_j(L_FOOT, 0.90, 0.4, 0.95, 0.3, 1.00, 0.2)
    set_j(R_FOOT, 0.92, 0.4, 0.97, 0.3, 1.02, 0.2)
    return amp


# Joint-specific phase offsets so the noise doesn't ripple in unison.
def _alive_phases(seed: int) -> List[Tuple[float, float, float]]:
    rng = random.Random(seed)
    return [(rng.uniform(0, 2 * math.pi),
             rng.uniform(0, 2 * math.pi),
             rng.uniform(0, 2 * math.pi))
            for _ in range(NUM_JOINTS)]


_ALIVE_AMP = _alive_amplitudes()


def _apply_alive(pose: List[List[float]], t_sec: float,
                 phases: List[Tuple[float, float, float]]) -> None:
    for i in range(NUM_JOINTS):
        (fz, az), (fx, ax), (fy, ay) = _ALIVE_AMP[i]
        pz, px, py = phases[i]
        if az: pose[i][0] += az * math.sin(2 * math.pi * fz * t_sec + pz)
        if ax: pose[i][1] += ax * math.sin(2 * math.pi * fx * t_sec + px)
        if ay: pose[i][2] += ay * math.sin(2 * math.pi * fy * t_sec + py)


def _walk_overlay(pose: List[List[float]], s: float) -> Tuple[float, float]:
    """Walking layer. Returns extra (dy_for_root, dz_for_root) translation."""
    phase = 2.0 * math.pi * 1.0 * s   # 1 Hz stride
    sn = math.sin(phase)
    cn = math.cos(phase)

    # Leg swing — X axis (sagittal). Left and right opposite.
    leg_swing = 28.0
    _add(pose, L_HIP, 0.0,  leg_swing * sn, 0.0)
    _add(pose, R_HIP, 0.0, -leg_swing * sn, 0.0)

    # Knee bend follows the leg's forward swing — bent when leg is forward.
    knee_l = max(0.0,  sn) * 35.0
    knee_r = max(0.0, -sn) * 35.0
    _add(pose, L_KNEE, 0.0, -knee_l, 0.0)
    _add(pose, R_KNEE, 0.0, -knee_r, 0.0)

    # Ankle dorsiflexion roughly opposite the knee so the foot rolls.
    _add(pose, L_ANKLE, 0.0,  knee_l * 0.4, 0.0)
    _add(pose, R_ANKLE, 0.0,  knee_r * 0.4, 0.0)

    # Arm swing opposite the legs (right arm with left leg).
    arm_swing = 25.0
    _add(pose, L_SHOULDER, 0.0, -arm_swing * sn, 0.0)
    _add(pose, R_SHOULDER, 0.0,  arm_swing * sn, 0.0)
    # Elbows bend a bit on the forward swing.
    elbow_l = max(0.0, -sn) * 18.0
    elbow_r = max(0.0,  sn) * 18.0
    _add(pose, L_ELBOW, 0.0, -elbow_l, 0.0)
    _add(pose, R_ELBOW, 0.0, -elbow_r, 0.0)

    # Spine counter-rotation — torso twists opposite to the hips.
    twist = 6.0 * sn
    _add(pose, SPINE1, 0.0, 0.0, -twist)
    _add(pose, SPINE2, 0.0, 0.0, -twist * 0.7)
    _add(pose, SPINE3, 0.0, 0.0, -twist * 0.5)
    # Neck/head lead the gaze — tiny counter-twist to dampen.
    _add(pose, NECK, 0.0, 0.0,  twist * 0.3)
    _add(pose, HEAD, 0.0, 0.0,  twist * 0.2)
    # Hip side-to-side weight shift (Z-rot in our convention is roll).
    _add(pose, PELVIS, 4.0 * sn, 0.0, 0.0)
    # Pelvis pitch — slight forward lean.
    _add(pose, PELVIS, 0.0, -3.0, 0.0)

    # Vertical bob (twice the stride freq) + forward translation.
    dy = 0.02 * math.cos(2.0 * phase)
    dz = -0.6 * s            # ~0.6 m/s forward (engine -Z is forward in clip)
    return dy, dz


def _wave_overlay(pose: List[List[float]], s: float) -> None:
    phase = 2.0 * math.pi * 2.0 * s   # 2 Hz wave

    # Right arm up: shoulder Z-rotation (frontal plane) raises arm to the side.
    _add(pose, R_SHOULDER, -100.0, 0.0, -15.0)
    # Elbow stays bent during wave so the hand is up by the head.
    _add(pose, R_ELBOW, 0.0, -55.0, 0.0)
    # Wrist twist & finger oscillation that drives the "waving" feel.
    _add(pose, R_WRIST, 0.0, 0.0, 25.0 * math.sin(phase))
    _add(pose, R_HAND, 0.0, 18.0 * math.sin(phase + 0.4), 0.0)

    # Body lean / head tilt toward the waving side.
    _add(pose, SPINE1, -4.0, 0.0, 0.0)
    _add(pose, SPINE2, -3.0, 0.0, 0.0)
    _add(pose, NECK,   -4.0, 0.0, -8.0)
    _add(pose, HEAD,   -2.0, 0.0, -4.0)

    # Left arm relaxed but with subtle swing (counter-balance).
    _add(pose, L_SHOULDER, 0.0, 4.0 * math.sin(phase * 0.5), 0.0)
    _add(pose, L_ELBOW,    0.0, -8.0, 0.0)

    # Left foot bears more weight — slight knee bend on the right.
    _add(pose, R_KNEE, 0.0, -8.0, 0.0)
    _add(pose, R_HIP,  0.0,  4.0, 0.0)


def _jump_overlay(pose: List[List[float]], s: float, n_frames: int, frame: int) -> Tuple[float, float]:
    t01 = frame / max(1, n_frames - 1)

    # Phases: crouch (0 .. 0.25), launch + air (0.25 .. 0.75), land (0.75 .. 1.0)
    if t01 < 0.25:
        u = t01 / 0.25
        crouch = 50.0 * u
        knee = crouch
        hip = crouch * 0.6
        ankle = -crouch * 0.5
        arm_back = 50.0 * u
        elbow = 30.0 * u
        spine_fwd = 12.0 * u
        hop_dy = 0.0
    elif t01 < 0.75:
        u = (t01 - 0.25) / 0.5
        knee = 50.0 * (1.0 - u)
        hip = 30.0 * (1.0 - u)
        ankle = -25.0 * (1.0 - u) - 20.0 * u  # plantarflex on launch / air
        # Arm swing forward / up during launch, hold during air.
        arm_back = 50.0 - 130.0 * min(u * 2.0, 1.0)
        elbow = 30.0 * (1.0 - u)
        spine_fwd = 12.0 - 18.0 * u
        hop_dy = 0.55 * math.sin(math.pi * u)
    else:
        u = (t01 - 0.75) / 0.25
        crouch = 35.0 * (1.0 - u)
        knee = crouch
        hip = crouch * 0.6
        ankle = -crouch * 0.5
        arm_back = -80.0 + 100.0 * u  # arms come back forward to neutral
        elbow = 20.0 * (1.0 - u)
        spine_fwd = -6.0 + 6.0 * u
        hop_dy = 0.0

    _add(pose, L_KNEE, 0.0, -knee, 0.0)
    _add(pose, R_KNEE, 0.0, -knee, 0.0)
    _add(pose, L_HIP,  0.0,  hip,  0.0)
    _add(pose, R_HIP,  0.0,  hip,  0.0)
    _add(pose, L_ANKLE, 0.0, ankle, 0.0)
    _add(pose, R_ANKLE, 0.0, ankle, 0.0)
    _add(pose, L_SHOULDER, 0.0, arm_back, 0.0)
    _add(pose, R_SHOULDER, 0.0, arm_back, 0.0)
    _add(pose, L_ELBOW, 0.0, -elbow, 0.0)
    _add(pose, R_ELBOW, 0.0, -elbow, 0.0)
    _add(pose, SPINE1, 0.0, -spine_fwd, 0.0)
    _add(pose, SPINE2, 0.0, -spine_fwd * 0.8, 0.0)
    _add(pose, SPINE3, 0.0, -spine_fwd * 0.6, 0.0)
    _add(pose, NECK,   0.0,  spine_fwd * 0.5, 0.0)  # head looks up at apex

    return hop_dy, 0.0


def _idle_overlay(pose: List[List[float]], s: float) -> None:
    # Breathing — slow chest expansion driven through the upper spine. Phased
    # head/neck so the gaze drifts subtly.
    breathe = math.sin(2.0 * math.pi * 0.25 * s)  # 4-second breath cycle
    _add(pose, SPINE2, 0.0, -1.5 * breathe, 0.0)
    _add(pose, SPINE3, 0.0, -1.5 * breathe, 0.0)

    # Hip weight shift, slow.
    shift = math.sin(2.0 * math.pi * 0.20 * s)
    _add(pose, PELVIS, 5.0 * shift, 0.0, 0.0)
    _add(pose, L_HIP, -3.0 * shift, 0.0, 0.0)
    _add(pose, R_HIP,  3.0 * shift, 0.0, 0.0)
    _add(pose, L_KNEE, 0.0, -4.0 * max(0.0, -shift), 0.0)
    _add(pose, R_KNEE, 0.0, -4.0 * max(0.0,  shift), 0.0)

    # Slow gaze drift on the head/neck.
    gaze = math.sin(2.0 * math.pi * 0.12 * s + 0.7)
    _add(pose, NECK, 0.0, 0.0, 5.0 * gaze)
    _add(pose, HEAD, 0.0, 0.0, 3.0 * gaze)
    _add(pose, HEAD, 0.0, 2.0 * math.sin(2.0 * math.pi * 0.18 * s), 0.0)


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
    action = _detect_action(text)
    frame_time = 1.0 / float(fps)
    n_frames = max(1, int(round(duration * fps)))
    phases = _alive_phases(seed)

    # RootTranslation is relative to the bind-pose pelvis (engine adds the
    # canonical pelvis-from-feet offset when spawning), so base_y = 0 here.
    base_y = 0.0

    root_translations: List[Tuple[float, float, float]] = []
    poses_out: List[List[Tuple[float, float, float]]] = []

    for f in range(n_frames):
        s = f / float(fps)
        pose = _zero_pose()
        _apply_alive(pose, s, phases)

        ry = base_y
        rz = 0.0
        if action == "walk":
            dy, dz = _walk_overlay(pose, s)
            ry += dy
            rz += dz
        elif action == "wave":
            _wave_overlay(pose, s)
        elif action == "jump":
            dy, dz = _jump_overlay(pose, s, n_frames, f)
            ry += dy
            rz += dz
        else:
            _idle_overlay(pose, s)

        # Pack back as tuples in (Zrot, Xrot, Yrot) order.
        poses_out.append([(p[0], p[1], p[2]) for p in pose])
        root_translations.append((0.0, ry, rz))

    return frame_time, root_translations, poses_out
