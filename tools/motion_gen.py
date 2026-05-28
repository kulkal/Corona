"""Motion generation CLI for the Corona console dispatcher.

Usage:
    motion_gen.py --text "a person walks forward" \
                  --output-dir <dir> \
                  [--duration 3.0] [--fps 20] [--seed 0] \
                  [--mode mock|momask]

Stdout (last line): RESULT_BVH=<absolute path to generated .bvh>
Stderr: progress / diagnostics.

Designed to mirror tools/tripo_gen.py — one-shot subprocess called from
the Corona engine console. The mock mode generates a procedural BVH on
the SMPL 24-joint skeleton without any model or GPU, which lets the
engine-side BVH loader / retargeter / playback be brought up before the
real MoMask backend is wired in.

When `--mode momask` lands, the import / load cost of MoMask + SMPL will
make a long-running server preferable; that mode can be migrated to a
server later without changing the engine-side interface (the engine just
calls the same script via `RUN_MOMASK_VIA_SERVER=1` env var or similar).
"""

import argparse
import sys
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--text", required=True, help="natural-language motion description")
    parser.add_argument("--output-dir", required=True, help="directory for generated BVH")
    parser.add_argument("--duration", type=float, default=3.0)
    parser.add_argument("--fps", type=int, default=20)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--mode", choices=("mock", "momask"), default="mock")
    args = parser.parse_args()

    out_dir = Path(args.output_dir).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    # The motion_server package lives next to this script.
    server_dir = Path(__file__).resolve().parent / "motion_server"
    if not server_dir.is_dir():
        print(f"ERROR: motion_server modules not found at {server_dir}", file=sys.stderr)
        return 2
    sys.path.insert(0, str(server_dir))

    if args.mode == "mock":
        # Deferred imports — argparse failures should fire before module load.
        import mock_motion
        from bvh_writer import write_bvh

        t0 = time.perf_counter()
        frame_time, root_xyz, poses = mock_motion.generate(
            args.text,
            duration=args.duration,
            fps=args.fps,
            seed=args.seed,
        )
        print(
            f"  mock synth: {len(root_xyz)} frames in "
            f"{time.perf_counter() - t0:.3f}s",
            file=sys.stderr,
        )

        import hashlib
        h = hashlib.sha1(
            f"{args.text}|{args.duration}|{args.seed}|{args.fps}".encode("utf-8")
        ).hexdigest()[:10]
        safe = "".join(c if c.isalnum() else "_" for c in args.text.lower())[:32]
        out_path = out_dir / f"{safe}_{h}.bvh"

        t0 = time.perf_counter()
        write_bvh(str(out_path), frame_time, root_xyz, poses)
        print(
            f"  bvh written ({out_path.stat().st_size} B) in "
            f"{time.perf_counter() - t0:.3f}s",
            file=sys.stderr,
        )

    elif args.mode == "momask":
        print("ERROR: momask mode not yet implemented", file=sys.stderr)
        return 3

    # Corona-side parser reads this single line from stdout.
    print(f"RESULT_BVH={out_path}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
