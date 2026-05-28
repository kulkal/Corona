# motion_server

Modules used by `tools/motion_gen.py` to synthesize text-conditioned motion
on the SMPL 24-joint skeleton.

The name is "server" because the eventual MoMask backend will likely run as
a long-running service (model load is too expensive per-call). For now the
mock backend runs as a one-shot CLI matching the `tripo_gen.py` pattern.

## Modules

- `smpl_skeleton.py` — canonical SMPL 24-joint hierarchy + T-pose offsets,
  shared by the BVH writer and any motion generator. Engine side mirrors
  this table so joints can be addressed by index without remapping.
- `bvh_writer.py` — minimal BVH writer (Y-up, channel order
  `Zrotation Xrotation Yrotation`, root has 6 channels).
- `mock_motion.py` — procedural synth keyed off keywords in the prompt
  (`walk`, `wave`, `jump`, default `idle`).

## Calling from the engine

The engine console invokes `tools/motion_gen.py` via subprocess and parses
the last stdout line:

```
RESULT_BVH=<absolute path>
```

See `docs/design/llm_motion_generation_pipeline.md` and
`docs/design/llm_procedural_animation_progress.md` for the full pipeline.

## Mock mode requirements

Stdlib only — no external dependencies. Python 3.8+.

## Future: MoMask mode

Reserved (`--mode momask` in `motion_gen.py`). Needs:
- MoMask checkpoints
- SMPL body model (`smpl.is.tue.mpg.de`, license required)
- CUDA-capable GPU
