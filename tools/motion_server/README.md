# motion_server

Text-to-motion generation server for Corona engine.

Provides an HTTP endpoint that takes a natural-language prompt and returns a
motion file (BVH) on the SMPL 24-joint skeleton, ready to be loaded and
retargeted in the engine.

See `docs/design/llm_motion_generation_pipeline.md` for the full design.

## Modes

- `mock` — produces a synthetic procedural BVH (no model, no GPU). Used for
  pipeline bring-up and CI.
- `momask` — runs MoMask inference. Requires model checkpoints, SMPL body
  model, and a CUDA GPU. (Not implemented yet.)

## Quick start

```
pip install -r requirements.txt
python server.py --mode mock --port 8000
```

## Endpoint

```
POST /generate
{
  "text": "a person walks forward",
  "duration": 3.0,
  "seed": 42
}
```

Response:

```
{
  "status": "ok",
  "path": "C:/dev/Corona_aux/_motions/<hash>.bvh",
  "duration": 3.0,
  "fps": 20,
  "joints": 24,
  "format": "bvh"
}
```
