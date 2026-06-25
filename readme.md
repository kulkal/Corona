# UE5 4K Grid Scaling: Raster vs RT Primary GBuffer + RTDeferred Lighting

Date: 2026-06-24<br>
Project: `C:\TrainingScenes\ArcoThree\ArcoThree.uproject`<br>
Map: `/Game/CyberpunkEnvironmentKit/Maps/Demonstration`<br>
Engine: `G:\dev\nvrtx-5.8_preview`

## Summary

This experiment replaces opaque GBuffer generation with primary-ray hits instead of the raster base pass, then replaces UE raster deferred lighting/Lumen with custom RT lighting passes:

- `RayTracingRTDeferredDirectLighting`
- `RayTracingRTDeferredIndirectLighting`

The goal is to reduce opaque raster draw submission close to zero and test how the path scales as the UE5 cyberpunk map is replicated from `1x1` to `10x10`.

The raster baseline uses Lumen GI/reflections and DLSS-SR at 4K output. The final RT path uses RT Primary GBuffer, DLSS-RR, and custom RTDeferred direct/indirect lighting. A third comparison mode, `RT Primary GBuffer + Lumen GI/Reflection`, keeps RT Primary GBuffer and custom RTDeferred direct lighting but routes diffuse GI/reflections through UE Lumen.

The main `1x`-`10x` tables are opaque GBuffer/lighting comparisons. Decals and translucency are covered separately because they are composited after the opaque RTDeferred path.

Final `10x10` results:

| Mode | FPS | Frame ms | Draw ms | GPU ms |
|---|---:|---:|---:|---:|
| Raster DLSS-SR | 20.63 | 48.479 | 48.438 | 8.962 |
| RT Primary GBuffer + RTDeferred | 162.78 | 6.143 | 5.192 | 5.555 |
| RT Primary GBuffer + Lumen GI/Reflection | 38.53 | 25.956 | 25.924 | 5.934 |

At `10x10`, `RT Primary GBuffer + RTDeferred` is `7.89x` faster than raster. Frame time improves by `87.3%`, Draw thread time by `89.3%`, and GPU time by `38.0%`.

The Lumen comparison is much slower than the final RTDeferred path even though GPU time is similar. Its `10x10` GPU time is `5.934 ms`, but Draw thread time grows to `25.924 ms` because Lumen needs surface-cache/main-pass visibility work that the final RTDeferred path avoids.

## Test Scene

![10x RTDeferred world replication](docs/perf/rtdeferred_10x_world_replication_20260622.png)

The source scene is `/Game/CyberpunkEnvironmentKit/Maps/Demonstration`. The scene is loaded as a `10x10` replicated grid, with replicated static meshes and lights present as real world objects.

## 10x Visual Comparison

The following captures were taken with `10x10`, `3840x2160` launch settings, and `RTDeferredDelayedShot frames=360`. The saved viewport-shot resolution is `2560x1392`.

### Opaque-only

| Raster opaque | RT Primary GBuffer + RTDeferred opaque |
|---|---|
| ![Raster opaque 10x](docs/perf/ue5_10x_raster_opaque_20260624.png) | ![RTDeferred opaque 10x](docs/perf/ue5_10x_rtdeferred_opaque_20260624.png) |

### Decals-only

`decals-only` enables DBuffer decals while keeping `Translucency`, `Distortion`, and `Refraction` disabled.

| Raster decals-only | RT Primary GBuffer + RTDeferred decals-only |
|---|---|
| ![Raster decals-only 10x](docs/perf/ue5_10x_raster_decals_only_20260624.png) | ![RTDeferred decals-only 10x](docs/perf/ue5_10x_rtdeferred_decals_only_20260624.png) |

## RTDeferred Implementation

`RT Primary GBuffer + RTDeferred` uses this structure:

- `r.RTDeferred.RenderMode 1` generates the opaque GBuffer from primary ray hits instead of the raster base pass.
- Primary-hit output fills albedo, normal, roughness/metallic, depth, motion, and DLSS-RR guide buffers.
- `RayTracingRTDeferredDirectLighting` samples the scene light list and evaluates shadowing through RT visibility.
- `RayTracingRTDeferredIndirectLighting` evaluates diffuse and specular indirect/reflection lobes. The current measured path uses `r.RayTracing.RTDeferred.IndirectLighting.FrameInterleave=0`, so diffuse and specular are evaluated in the same frame.
- UE Lumen diffuse/reflection, MegaLights, raster shadow depths, and surface cache updates are disabled in the final RTDeferred path.
- DLSS-RR is used for RTDeferred. The raster baseline uses DLSS-SR.
- `r.RTDeferred.Decals` and `r.RTDeferred.Translucency` are RTDeferred-specific top-level switches. When `r.RTDeferred.RenderMode 1` is applied, these values also drive DBuffer, RT Primary GBuffer decal stencil/apply, ray tracing decal exclusion, and translucency/distortion/refraction showflags.
- The opaque-only comparison uses `r.RTDeferred.Decals=0` and `r.RTDeferred.Translucency=0`. In that mode, RT material command creation also skips `MD_DeferredDecal` materials and translucent materials while the corresponding showflags are disabled, so RT primary hits do not fold decal/sticker geometry into the opaque GBuffer.

Draw-thread behavior in the final path:

- Replicated static mesh primitives are `VisibleInRayTracing=true`, `RenderInMainPass=false`, and `RenderInDepthPass=false`.
- `r.RayTracing.RTDeferred.AllowMainPassHiddenPrimitives=1` keeps main-pass-hidden primitives available to the RT scene and material hit shaders.
- Nearby static mesh copies stay as Actor/Component copies. Distant static mesh copies are grouped into RT-only `UHierarchicalInstancedStaticMeshComponent` far proxies under `RTDeferredGridFarProxy`.
- When the camera moves near a far proxy copy, that copy is promoted back to the original Actor/Component copy. This keeps texture streaming and material residency on the normal UE path for nearby objects.
- HISM promote/demote does not repeatedly remove/add instances. The implementation preserves instance indices and switches between zero-scale hide and original-transform restore, avoiding default-texture exposure and movement-time mesh flicker.
- Copied local light actors are kept only within `RTDeferredLevelGridNearLightDistance=30000`. Distant point/spot light copies are not spawned until the camera moves near them.
- Dynamic distance splitting is enabled with `RTDeferredLevelGridDynamicFarProxy=1`, update interval `0.1s`, move threshold `2000`, demote hysteresis `1.2x`, scan budget `4096 records/tick`, transition budget `128 transitions/tick`, and proxy streaming touch budget `256 components/tick`.

RT instance culling uses an extended-frustum style:

- `r.RayTracing.RTDeferred.ExtendedFrustumCulling=1`
- `r.RayTracing.RTDeferred.ExtendedFrustumMargin=8192`
- The view FOV and side-plane normals are not widened.
- Primitive AABBs are tested against the current view-frustum planes with a world-space margin applied only to plane rejection.
- Nearby surfaces get a small stability margin, while far off-screen objects are still rejected.
- UE's default RT radius/solid-angle GPU instance culling is disabled in RTDeferred mode so CPU-visible candidates are not culled again by the GPU build shader.

## Lumen GI/Reflection Comparison

The Lumen comparison mode uses manual control with `r.RTDeferred.RenderMode -1`. It keeps RT Primary GBuffer and `RayTracingRTDeferredDirectLighting`, replaces `RayTracingRTDeferredIndirectLighting` with UE Lumen diffuse GI/reflections, and keeps Lumen surface-cache/main-pass visibility enabled.

Because this mode needs surface-cache/main-pass visibility, it does not use `RTDeferredLevelGridRTOnlyCopies`. Its GPU pass time remains close to the final RTDeferred path, but its Draw-thread time scales much worse.

Interactive Lumen testing with decals and raster translucency is available through:

```bat
G:\dev\nvrtx-5.8_preview\run_10x_rtprimary_lumen_gi_reflections.bat
```

That launcher is for visual inspection. The profiled Lumen comparison remains the opaque GI/reflection mode shown in the main table.

## Decals And Translucency

The final RTDeferred path now composites decals and translucency on top of the RT-generated opaque scene.

Decals reuse UE's DBuffer/deferred decal pass:

- RT Primary GBuffer first writes device depth and copies it to `SceneDepth`.
- An RT Primary GBuffer decal-stencil pass marks hit pixels that can receive decals.
- UE's existing DBuffer decal pass runs.
- `RayTracingPrimaryGBufferApplyDBuffer` applies DBuffer results back into the RT Primary GBuffer and DLSS-RR guide buffers.
- Translucent decal variants are forced into a modulate-style path with `r.RayTracing.PrimaryGBuffer.Decals.ForceModulateTranslucent=1`.
- Because this decal path is directly integrated into RTDeferred, UE `ShowFlag.Decals` alone is not enough to control it. `r.RTDeferred.Decals=0` disables the RT Primary GBuffer stencil/apply path, sets `r.RayTracing.ExcludeDecals=1`, and skips deferred-decal material commands in the RT material table.
- `r.RTDeferred.Translucency=0` also skips translucent material commands for the opaque-only path, preventing translucent sticker/decal geometry from being hit as opaque RT Primary GBuffer input.

Raster translucency is the preferred translucent path:

- Opaque RTDeferred lighting writes `SceneColor`/`SceneDepth`.
- UE raster translucency, glass/refraction materials, and particle-style translucent draws are composited afterward.
- Refraction uses UE's distortion pass.
- When RT Primary GBuffer is active, `r.RayTracing.PrimaryGBuffer.Distortion.FullScreenApply=1` runs distortion apply/merge full-screen so raster refraction composes correctly over RT-generated depth and scene color.

Ray-traced translucency was also tested, but it did not produce a compelling quality/performance win over raster translucency in the current scene. Reflections on transparent materials remain incomplete. Accurate transparent reflections after DLSS-RR may be possible, but it would be a separate heavy composition path and is not part of this document.

### 10x Decal/Translucency Cost

These values use `10x10`, 4K output, 2400 captured frames, and a 2200-frame average after skipping the first 200 frames.

| Configuration | FPS | Frame | Game | Draw | RHI | GPU | DrawCalls | Prims |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Raster DLSS-SR + decals/raster translucency | 22.79 | 43.883 | 3.053 | 43.860 | 5.245 | 10.320 | 5479 | 140.31M |
| RT Primary GBuffer + RTDeferred opaque-only baseline | 162.78 | 6.143 | 2.465 | 5.192 | 2.956 | 5.555 | 36 | 0.03M |
| RT Primary GBuffer + RTDeferred + decals/raster translucency | 123.88 | 8.072 | 2.639 | 6.775 | 4.289 | 7.460 | 400 | 0.035M |
| RT Primary GBuffer + RTDeferred + decals/RT translucency | 122.33 | 8.175 | 2.599 | 5.720 | 3.354 | 7.604 | 391 | 0.032M |

Enabling decals and raster translucency reduces RTDeferred from `162.78 FPS` to `123.88 FPS` at `10x10`. DrawCalls rise from `36` to `400`, but the added work comes from DBuffer decals, raster translucency, distortion, and support passes, not from opaque base pass, prepass, or shadow-depth rendering.

Switching translucency to RT removes raster translucency/distortion draws, but adds about `0.334 ms` of `RayTracingPrimaryRays`. In this test scene the final result is almost the same: `123.88 FPS` with raster translucency vs `122.33 FPS` with RT translucency.

## 1x-10x 4K Scaling

![FPS, Draw, RHI, and GPU timing by grid multiplier](docs/perf/ue5_grid_scaling_4k_rtdeferred_farproxy_20260623.svg)

The x-axis is replication multiplier. For example, `5x5` is shown as `25`, and `10x10` as `100`.

| Grid | Mult | Mode | FPS | Frame | Game | Draw | RHI | GPU |
|---:|---:|---|---:|---:|---:|---:|---:|---:|
| 1x | 1 | Raster DLSS-SR | 196.66 | 5.085 | 2.184 | 5.077 | 1.948 | 2.824 |
| 1x | 1 | RT Primary GBuffer + RTDeferred | 169.83 | 5.888 | 2.151 | 3.341 | 3.700 | 5.196 |
| 1x | 1 | RT Primary GBuffer + Lumen GI/Reflection | 186.80 | 5.353 | 2.388 | 4.505 | 3.386 | 4.542 |
| 2x | 4 | Raster DLSS-SR | 163.49 | 6.116 | 2.298 | 6.107 | 2.059 | 3.191 |
| 2x | 4 | RT Primary GBuffer + RTDeferred | 166.45 | 6.008 | 2.362 | 3.528 | 3.801 | 5.392 |
| 2x | 4 | RT Primary GBuffer + Lumen GI/Reflection | 180.03 | 5.555 | 2.581 | 4.881 | 3.540 | 4.665 |
| 3x | 9 | Raster DLSS-SR | 98.94 | 10.107 | 2.333 | 10.098 | 2.058 | 3.852 |
| 3x | 9 | RT Primary GBuffer + RTDeferred | 166.57 | 6.004 | 2.456 | 3.934 | 2.804 | 5.419 |
| 3x | 9 | RT Primary GBuffer + Lumen GI/Reflection | 164.45 | 6.081 | 2.955 | 6.070 | 3.570 | 4.960 |
| 4x | 16 | Raster DLSS-SR | 81.34 | 12.294 | 2.361 | 12.284 | 2.047 | 4.232 |
| 4x | 16 | RT Primary GBuffer + RTDeferred | 162.55 | 6.152 | 2.479 | 4.528 | 2.959 | 5.575 |
| 4x | 16 | RT Primary GBuffer + Lumen GI/Reflection | 164.20 | 6.090 | 2.944 | 6.079 | 3.701 | 4.957 |
| 5x | 25 | Raster DLSS-SR | 57.22 | 17.477 | 2.533 | 17.468 | 2.110 | 4.861 |
| 5x | 25 | RT Primary GBuffer + RTDeferred | 162.99 | 6.135 | 2.466 | 4.570 | 2.935 | 5.565 |
| 5x | 25 | RT Primary GBuffer + Lumen GI/Reflection | 104.06 | 9.610 | 3.381 | 9.601 | 3.891 | 4.990 |
| 6x | 36 | Raster DLSS-SR | 47.18 | 21.196 | 2.660 | 21.183 | 2.153 | 5.546 |
| 6x | 36 | RT Primary GBuffer + RTDeferred | 163.73 | 6.107 | 2.413 | 4.439 | 2.780 | 5.525 |
| 6x | 36 | RT Primary GBuffer + Lumen GI/Reflection | 97.13 | 10.296 | 3.574 | 10.283 | 3.904 | 5.052 |
| 7x | 49 | Raster DLSS-SR | 36.90 | 27.102 | 2.868 | 27.079 | 2.256 | 6.578 |
| 7x | 49 | RT Primary GBuffer + RTDeferred | 160.44 | 6.233 | 2.449 | 4.648 | 2.898 | 5.647 |
| 7x | 49 | RT Primary GBuffer + Lumen GI/Reflection | 60.59 | 16.505 | 4.075 | 16.495 | 4.329 | 5.196 |
| 8x | 64 | Raster DLSS-SR | 30.90 | 32.364 | 2.992 | 32.349 | 2.282 | 7.026 |
| 8x | 64 | RT Primary GBuffer + RTDeferred | 160.93 | 6.214 | 2.436 | 4.700 | 2.858 | 5.634 |
| 8x | 64 | RT Primary GBuffer + Lumen GI/Reflection | 59.09 | 16.924 | 4.110 | 16.916 | 4.354 | 5.199 |
| 9x | 81 | Raster DLSS-SR | 25.90 | 38.615 | 3.206 | 38.598 | 2.375 | 7.729 |
| 9x | 81 | RT Primary GBuffer + RTDeferred | 159.21 | 6.281 | 2.436 | 4.901 | 2.881 | 5.679 |
| 9x | 81 | RT Primary GBuffer + Lumen GI/Reflection | 40.25 | 24.846 | 4.770 | 24.822 | 4.592 | 5.902 |
| 10x | 100 | Raster DLSS-SR | 20.63 | 48.479 | 4.023 | 48.438 | 2.715 | 8.962 |
| 10x | 100 | RT Primary GBuffer + RTDeferred | 162.78 | 6.143 | 2.465 | 5.192 | 2.956 | 5.555 |
| 10x | 100 | RT Primary GBuffer + Lumen GI/Reflection | 38.53 | 25.956 | 4.818 | 25.924 | 4.645 | 5.934 |

## Test Hardware

- CPU: `12th Gen Intel(R) Core(TM) i9-12900K`, 24 logical processors
- GPU: `NVIDIA GeForce RTX 5090`, driver `610.37`, VRAM `32607 MiB`
- System memory: `63.7 GiB`
- OS: `Microsoft Windows 10.0.26200.8655`
## Batch Files

The mode batch files live at the UE repository root:

```text
G:\dev\nvrtx-5.8_preview
```

Available launchers:

```text
G:\dev\nvrtx-5.8_preview\run_mode.bat
G:\dev\nvrtx-5.8_preview\run_10x_raster_opaque.bat
G:\dev\nvrtx-5.8_preview\run_10x_raster_decals_only.bat
G:\dev\nvrtx-5.8_preview\run_10x_raster_decals_translucency.bat
G:\dev\nvrtx-5.8_preview\run_10x_rtdeferred_opaque.bat
G:\dev\nvrtx-5.8_preview\run_10x_rtdeferred_decals_only.bat
G:\dev\nvrtx-5.8_preview\run_10x_rtdeferred_decals_raster_translucency.bat
G:\dev\nvrtx-5.8_preview\run_10x_rtdeferred_decals_rt_translucency.bat
G:\dev\nvrtx-5.8_preview\run_10x_rtprimary_lumen_gi_reflections.bat
G:\dev\nvrtx-5.8_preview\profile_all_10x_4k_2400.bat
```

`frames=0` in `run_mode.bat` launches an interactive window. Any positive frame count runs CSV profiling and exits after capture.

`run_10x_rtdeferred_decals_rt_translucency.bat` is kept only as a compatibility alias to the raster translucency path. RT translucency is not an active final mode.

Mode-specific startup cvars live under:

```text
G:\dev\nvrtx-5.8_preview\RTDeferredModes\cvars
```

---
