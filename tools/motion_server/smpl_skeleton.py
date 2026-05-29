"""SMPL 24-joint skeleton — shared between the server (BVH writer) and
mock motion generator. Matches the canonical SMPL joint ordering so that
the engine side can address bones by index without any remapping.
"""

from dataclasses import dataclass
from typing import List, Tuple


# (name, parent_index, offset_from_parent_xyz_meters)
# Offsets are approximate canonical SMPL T-pose joint positions (Y-up).
# Real SMPL offsets depend on beta (shape); this is the neutral mean shape.
JOINTS: List[Tuple[str, int, Tuple[float, float, float]]] = [
    # idx 0
    ("pelvis",         -1, (0.000,  0.000,  0.000)),
    # 1, 2 — hips
    ("left_hip",        0, ( 0.058, -0.082,  0.014)),
    ("right_hip",       0, (-0.060, -0.090,  0.013)),
    # 3 — spine1
    ("spine1",          0, ( 0.005,  0.124, -0.038)),
    # 4, 5 — knees
    ("left_knee",       1, ( 0.039, -0.391, -0.011)),
    ("right_knee",      2, (-0.039, -0.404, -0.013)),
    # 6 — spine2
    ("spine2",          3, (-0.004,  0.139,  0.026)),
    # 7, 8 — ankles
    ("left_ankle",      4, (-0.028, -0.418, -0.039)),
    ("right_ankle",     5, ( 0.030, -0.421, -0.039)),
    # 9 — spine3
    ("spine3",          6, (-0.004,  0.055,  0.005)),
    # 10, 11 — feet
    ("left_foot",       7, ( 0.029, -0.064,  0.130)),
    ("right_foot",      8, (-0.030, -0.061,  0.131)),
    # 12 — neck
    ("neck",            9, ( 0.011,  0.219, -0.039)),
    # 13, 14 — collars
    ("left_collar",     9, (-0.071,  0.114, -0.018)),
    ("right_collar",    9, ( 0.083,  0.114, -0.022)),
    # 15 — head
    ("head",           12, ( 0.001,  0.089,  0.050)),
    # 16, 17 — shoulders
    ("left_shoulder",  13, ( 0.121,  0.046, -0.008)),
    ("right_shoulder", 14, (-0.114,  0.047, -0.010)),
    # 18, 19 — elbows
    ("left_elbow",     16, ( 0.255, -0.015, -0.022)),
    ("right_elbow",    17, (-0.260, -0.013, -0.022)),
    # 20, 21 — wrists
    ("left_wrist",     18, ( 0.265,  0.011, -0.007)),
    ("right_wrist",    19, (-0.269,  0.007, -0.006)),
    # 22, 23 — hands
    ("left_hand",      20, ( 0.087, -0.010, -0.018)),
    ("right_hand",     21, (-0.089, -0.007, -0.018)),
]


NUM_JOINTS = 24
assert len(JOINTS) == NUM_JOINTS


@dataclass
class JointInfo:
    name: str
    parent: int
    offset: Tuple[float, float, float]


def joints() -> List[JointInfo]:
    return [JointInfo(name=n, parent=p, offset=o) for (n, p, o) in JOINTS]


def children_of(idx: int) -> List[int]:
    return [i for i, (_, p, _) in enumerate(JOINTS) if p == idx]
