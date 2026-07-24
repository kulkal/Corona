"""TripoSR CLI wrapper for Corona console dispatcher.

Usage: tripo_gen.py --image <path> --output-dir <dir>

Stdout (last line): RESULT_OBJ=<absolute path to mesh.obj>
Stderr: progress / errors (TripoSR's own logging).

Designed to be called as a one-shot subprocess from Corona. The slow part
(rembg first-load + model checkpoint download) only happens once per machine
because of HuggingFace cache; subsequent calls reuse the cache.
"""

import argparse
import os
import sys
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, help="input image path")
    parser.add_argument("--output-dir", required=True, help="output directory")
    parser.add_argument("--mc-resolution", type=int, default=256)
    parser.add_argument("--device", default="cuda:0")
    parser.add_argument("--chunk-size", type=int, default=8192)
    args = parser.parse_args()

    image_path = Path(args.image).expanduser().resolve()
    if not image_path.is_file():
        print(f"ERROR: image not found: {image_path}", file=sys.stderr)
        return 2
    out_dir = Path(args.output_dir).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    # Make TripoSR importable. Repo layout: this file is at
    #   <repo>\tools\tripo_gen.py  (Corona repo)
    # but the model lives at D:\llm\tripoSR\repo. The Corona-side console
    # passes the absolute repo path via env var TRIPOSR_REPO so the
    # consumer can override the install location without editing this script.
    repo_root = Path(os.environ.get("TRIPOSR_REPO", r"D:\llm\tripoSR\repo"))
    if not (repo_root / "tsr").is_dir():
        print(f"ERROR: TRIPOSR_REPO does not look like a TripoSR clone: {repo_root}", file=sys.stderr)
        return 2
    sys.path.insert(0, str(repo_root))

    # Default HF cache to live next to the repo so the 1.6 GB checkpoint
    # doesn't pollute the user profile drive.
    os.environ.setdefault("HF_HOME", str(repo_root.parent / "hf_cache"))

    # Deferred imports — argparse/path checks should fail fast before we
    # pay the multi-second torch/diffusers cold start.
    import numpy as np
    import rembg
    import torch
    from PIL import Image
    from tsr.system import TSR
    from tsr.utils import remove_background, resize_foreground

    t0 = time.perf_counter()
    print(f"Loading TripoSR model on {args.device} ...", file=sys.stderr)
    model = TSR.from_pretrained(
        "stabilityai/TripoSR",
        config_name="config.yaml",
        weight_name="model.ckpt",
    )
    model.renderer.set_chunk_size(args.chunk_size)
    model.to(args.device)
    print(f"  model loaded in {time.perf_counter() - t0:.2f}s", file=sys.stderr)

    rembg_session = rembg.new_session()

    t0 = time.perf_counter()
    image = Image.open(image_path)
    image = remove_background(image, rembg_session)
    image = resize_foreground(image, 0.85)
    image = np.array(image).astype(np.float32) / 255.0
    image = image[:, :, :3] * image[:, :, 3:4] + (1 - image[:, :, 3:4]) * 0.5
    image = Image.fromarray((image * 255.0).astype(np.uint8))
    print(f"  preprocessing done in {time.perf_counter() - t0:.2f}s", file=sys.stderr)

    t0 = time.perf_counter()
    with torch.no_grad():
        scene_codes = model([image], device=args.device)
    print(f"  forward pass done in {time.perf_counter() - t0:.2f}s", file=sys.stderr)

    t0 = time.perf_counter()
    meshes = model.extract_mesh(scene_codes, has_vertex_color=True, resolution=args.mc_resolution)
    mesh = meshes[0]

    # Skip TripoSR's `to_gradio_3d_orientation` — it composes two rotations
    # specifically to please three.js inside the Gradio demo and leaves the
    # mesh lying on its side when re-imported elsewhere. TripoSR's native
    # marching-cubes output is already Y-up, which matches Corona.

    # Compute smooth vertex normals so trimesh's OBJ exporter writes `vn`
    # lines and faces use the `v/vt/vn` triple form. Without this the
    # exported mesh ships only positions + vertex colors and importers can
    # fall back to flat per-face shading.
    import trimesh as _trimesh
    if not isinstance(mesh, _trimesh.Trimesh):
        mesh = _trimesh.Trimesh(vertices=mesh.vertices, faces=mesh.faces,
                                vertex_colors=getattr(mesh, "visual", None))
    _ = mesh.vertex_normals  # touch to force computation + cache
    print(f"  mesh extraction done in {time.perf_counter() - t0:.2f}s", file=sys.stderr)

    out_path = out_dir / f"mesh_{int(time.time())}.obj"
    mesh.export(str(out_path), include_normals=True)
    print(f"  exported to {out_path}", file=sys.stderr)

    # Corona-side parser reads this single line from stdout.
    print(f"RESULT_OBJ={out_path}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
