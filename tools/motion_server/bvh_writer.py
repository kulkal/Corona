"""Minimal BVH writer for the SMPL 24-joint skeleton.

The engine-side loader expects:
- Y-up coordinate system
- channel order for joints: Zrotation Xrotation Yrotation (Euler ZXY intrinsic)
- root channels: Xposition Yposition Zposition Zrotation Xrotation Yrotation
"""

from io import StringIO
from typing import List, Tuple

from smpl_skeleton import JOINTS, NUM_JOINTS, children_of


def _write_joint(buf: StringIO, idx: int, depth: int, is_root: bool) -> None:
    name, _, offset = JOINTS[idx]
    indent = "\t" * depth
    if is_root:
        buf.write(f"{indent}ROOT {name}\n")
    else:
        buf.write(f"{indent}JOINT {name}\n")
    buf.write(f"{indent}{{\n")
    inner = "\t" * (depth + 1)
    buf.write(f"{inner}OFFSET {offset[0]:.6f} {offset[1]:.6f} {offset[2]:.6f}\n")
    if is_root:
        buf.write(
            f"{inner}CHANNELS 6 Xposition Yposition Zposition "
            f"Zrotation Xrotation Yrotation\n"
        )
    else:
        buf.write(f"{inner}CHANNELS 3 Zrotation Xrotation Yrotation\n")

    kids = children_of(idx)
    if kids:
        for c in kids:
            _write_joint(buf, c, depth + 1, is_root=False)
    else:
        # End site — required by BVH for leaf joints.
        buf.write(f"{inner}End Site\n")
        buf.write(f"{inner}{{\n")
        buf.write(f"{inner}\tOFFSET 0.000000 0.050000 0.000000\n")
        buf.write(f"{inner}}}\n")
    buf.write(f"{indent}}}\n")


def write_bvh(
    path: str,
    frame_time: float,
    root_translations: List[Tuple[float, float, float]],
    joint_euler_zxy_deg: List[List[Tuple[float, float, float]]],
) -> None:
    """Write a BVH file.

    Args:
        path: output file path
        frame_time: seconds per frame (e.g. 1/20)
        root_translations: per-frame (x, y, z) of pelvis in world meters
        joint_euler_zxy_deg: per-frame list of 24 (Zdeg, Xdeg, Ydeg) tuples,
            ordered by SMPL joint index. Root rotation goes at index 0.

    All three sequences must have the same length (frame count).
    """
    n_frames = len(root_translations)
    if len(joint_euler_zxy_deg) != n_frames:
        raise ValueError("translation/rotation frame count mismatch")
    if any(len(r) != NUM_JOINTS for r in joint_euler_zxy_deg):
        raise ValueError("each frame must list rotations for all 24 joints")

    buf = StringIO()
    buf.write("HIERARCHY\n")
    _write_joint(buf, 0, depth=0, is_root=True)
    buf.write("MOTION\n")
    buf.write(f"Frames: {n_frames}\n")
    buf.write(f"Frame Time: {frame_time:.6f}\n")

    for t in range(n_frames):
        tx, ty, tz = root_translations[t]
        row = [f"{tx:.6f}", f"{ty:.6f}", f"{tz:.6f}"]
        for (zr, xr, yr) in joint_euler_zxy_deg[t]:
            row.extend([f"{zr:.6f}", f"{xr:.6f}", f"{yr:.6f}"])
        buf.write(" ".join(row) + "\n")

    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(buf.getvalue())
