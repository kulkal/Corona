# Corona
Corona pandemic made me to spend some time for this hobby project. That is the reason why the name of the project is Corona.

## Build
Requirements:

* Visual Studio 2022 with the MSVC C++ toolchain.
* CMake with the repository presets.
* `WinPixEventRuntime` under `build/packages/`.
* Vulkan SDK with DXC available when building the Vulkan backend.

Generate and build:

```powershell
cmake --preset vs2022-x64
cmake --build --preset release
```

If the Vulkan SDK is not globally configured, set it before building:

```powershell
$env:VULKAN_SDK='C:\VulkanSDK\1.4.341.1'
cmake --build --preset release
```

The executable is emitted to `src/Corona.exe`, and the Visual Studio debugger working directory is `src/`. The solution is generated at `out/build/vs2022-x64/Corona.sln`.

Shader build outputs:

* DX12 uses the HLSL sources directly through the existing shader compile path.
* Vulkan SPIR-V files are generated during the CMake build.
* Generated Vulkan shader files are written under the CMake build directory, not the source shader directory.

## Render Backends
Corona now has a small render backend abstraction with two runtime backends:

* `dx12`: the original Direct3D 12/DXR backend.
* `vulkan`: a Vulkan backend with raster, compute, ray tracing, ImGui, GPU timing, and window presentation support.

The backend can be selected from the command line:

```powershell
.\Corona.exe --backend dx12
.\Corona.exe --backend vulkan
```

The Vulkan backend is designed to share the same HLSL shader sources as much as possible. SPIR-V is generated during the CMake build, and the runtime loads those compiled modules from the build output directory. Both backends use uncapped presentation paths where supported: DX12 uses tearing-enabled present when available, and Vulkan prefers immediate/mailbox present modes before falling back to FIFO.

## Render Modes
Two main render modes are available:

* `hybrid`: GBuffer rasterization plus ray-traced shadow, reflection, diffuse GI, denoising, lighting, tone mapping, and optional TAA.
* `pathtracing`: full-screen path tracing mode with accumulation, useful as a simpler reference path and for backend validation.

`--render-mode pt`, `--render-mode pathtracing`, and `--render-mode path-tracing` select the path tracing mode. Frame timing logs are written to `dumps/fps_perf.log` and include CPU frame timing plus per-pass GPU timings for backend comparisons.

The captures below were generated with ImGui disabled.

Hybrid final resolve:

![Hybrid final resolve](./docs/images/readme_hybrid_final.png)

Path tracing reference:

![Path tracing reference](./docs/images/readme_path_tracing_current.png)

## GI And Denoising
The hybrid renderer uses a ray-traced one-bounce diffuse GI approximation:

* The GBuffer depth and world normal reconstruct the shaded surface point.
* One blue-noise, frame-indexed cosine-hemisphere ray is traced per pixel.
* A sky miss evaluates the procedural sky color.
* A surface hit evaluates direct-light visibility at the hit point and writes Lambert diffuse irradiance.
* The diffuse result is stored as both irradiance color and SH data, then accumulated with temporal reprojection and edge-aware spatial filtering.

Specular GI/reflection is produced by the separate ray-traced reflection path and denoised alongside the diffuse GI buffers. The `pathtracing` render mode is a separate accumulated reference path, not the same algorithm used by the real-time hybrid GI pass.

| Raw diffuse GI | Spatially filtered diffuse GI |
|---|---|
| ![Raw diffuse GI](docs/images/readme_gi_raw.png) | ![Spatially filtered diffuse GI](docs/images/readme_gi_spatial.png) |

## DLSS SR/RR Integration
DLSS is integrated through NVIDIA Streamline on the DX12 backend:

* DLSS SR uses the lighting buffer, depth, and motion vectors to upscale from render resolution to display resolution.
* DLSS RR runs before SR and reconstructs the render-resolution lighting buffer using color, depth, motion vectors, normals, roughness, albedo, and specular albedo.
* Both SR and RR share the same Streamline frame token and camera constants for the frame.
* The hybrid pipeline order is `Lighting -> DLSS RR -> DLSS SR -> ToneMap/Present` when RR is enabled, with TAA used as the fallback if Streamline evaluation fails.

DLSS SR/RR currently belongs to the DX12/Streamline path. Other backends or unavailable Streamline features fall back to the non-DLSS AA path selected by the runtime.

The important practical result is that the GI shader can stay intentionally simple while the final image becomes much more stable. The real-time diffuse GI pass is only a 1spp estimator: one cosine-weighted ray per pixel, a sky miss path, and one direct-light visibility query at the secondary hit. By itself, that raw GI signal is visibly noisy and far from converged.

DLSS RR changes the quality tradeoff. It does not increase the GI sample count, and it does not make the raw GI buffer physically converged. Instead, it reconstructs the final lighting buffer using the noisy RT result plus strong scene context: depth, motion vectors, normals, roughness, albedo, and specular albedo. In practice, this can hide almost all visible stochastic GI noise in the resolved image while preserving geometric and material edges much better than a simple blur. This is the main reason the hybrid path can use a very small GI algorithm and still look close to a high-sample result after RR/SR.

Simple one-sample GI input:

![Raw one-sample diffuse GI](./docs/images/readme_gi_raw.png)

Final output with DLSS RR/SR:

![DLSS RR/SR output capture](./docs/images/readme_dlss_rr_sr_current.png)

Camera-path playback comparison:

Inline preview, left = DLSS RR/SR and right = TAA:

![DLSS RR/SR vs TAA camera-path preview](./docs/media/dlss_rr_vs_taa_preview.gif)

These two 30fps camera-path captures make the moving-camera difference much easier to see than a still frame. The renderer is feeding both modes a very small 1spp GI signal. With TAA, stochastic diffuse GI noise remains visible during motion and tends to shimmer as the camera moves. With DLSS RR/SR, the reconstructed result stays far more stable: GI noise is strongly suppressed while Sponza edges, shadow boundaries, and material transitions remain readable. This is the practical strength of the current hybrid path: simple RT GI plus rich GBuffer context gives RR enough information to produce a clean moving image, not just a clean still frame.

## Command-line Guide
Run from the `src/` directory, or set the working directory to `src/` when launching from Visual Studio:

```powershell
cd src
.\Corona.exe --backend dx12 --render-mode hybrid --aa taa --user-mode
.\Corona.exe --backend vulkan --render-mode hybrid --aa taa --user-mode
.\Corona.exe --backend vulkan --render-mode pathtracing --aa off --user-mode
.\Corona.exe --backend dx12 --render-mode hybrid --auto-dump --no-imgui
```

Supported options:

| Option | Values | Notes |
|---|---|---|
| `--backend`, `-backend` | `dx12`, `vulkan`, `vk` | Selects the render backend. Unknown values fall back to DX12. |
| `--render-mode`, `-render` | `hybrid`, `pt`, `pathtracing`, `path-tracing`, `path_tracing` | Selects hybrid rendering or full-screen path tracing. |
| `--aa`, `-aa` | `off`, `taa`, `dlss`, `dlss-sr`, `sr`, `dlss-rr`, `rr` | DLSS modes fall back to TAA when Streamline/DLSS is unavailable. |
| `--user-mode`, `--manual`, `-user` | none | Disables auto dump mode and runs interactively. |
| `--auto-dump`, `-dump` | none | Enables automated capture/dump mode. |
| `--no-imgui`, `--disable-imgui` | none | Disables ImGui initialization and rendering, useful for clean screenshots. |

Options can be written either as `--backend vulkan` or `--backend=vulkan`.

Environment overrides are also available for startup automation:

| Environment variable | Example | Notes |
|---|---|---|
| `CORONA_AUTO_DUMP` | `1`, `true`, `yes`, `dump` | Enables auto dump mode unless overridden by command-line flags. |
| `CORONA_START_RENDER_MODE` | `hybrid`, `pathtracing`, `pt` | Sets the initial render mode. |
| `CORONA_START_AA` | `off`, `taa`, `dlss`, `rr` | Sets the initial AA mode. |

## Third-party libs
* [enkiTS](https://github.com/dougbinks/enkiTS)
* [glm](https://glm.g-truc.net/0.9.9/index.html)
* [NV Aftermath](https://developer.nvidia.com/nvidia-aftermath)
* [CMake](https://cmake.org/)
* [imgui gizmo(compatible with glm)](https://github.com/DarisaLLC/imGuIZMO-1)

## Useful link
* Tone map
	* [Baking lab](https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ToneMapping.hlsl)
	* [MS MiniEngine Histogram based exposure control](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/MiniEngine/Core/Shaders)
* DX12
	* [MJP github](https://github.com/TheRealMJP)
* DXR
	* [Instance property](https://developer.nvidia.com/rtx/raytracing/dxr/DX12-Raytracing-tutorial/Extra/dxr_tutorial_extra2_simple_lighting)
	* [Adam Mars intro dxr](https://github.com/acmarrs/IntroToDXR)
* Raytracing
	* [VNDF](http://jcgt.org/published/0007/04/01/paper.pdf)
	* [Raytracing best practice](https://www.gdcvault.com/play/1026721/RTX-Ray-Tracing-Best-Practices)
	* [Raytracing reflection in youngblood](https://www.gdcvault.com/play/1026723/Ray-Traced-Reflections-in-Wolfenstein)
	* [Q2RT Indirect diffuse denoiser](https://github.com/NVIDIA/Q2RTX/blob/master/src/refresh/vkpt/shader/asvgf_lf.comp)
	* [Q2 RT GTC presentation from Alexey Pantellev](https://developer.nvidia.com/gtc/2019/video/S91046/video)
	* [Q2 RT GDC presentation from Alexey Pantellev](https://www.youtube.com/watch?v=FewqoJjHR0A)
	* [MS RTAO SFGV denoiser](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12Raytracing/src/D3D12RaytracingRealTimeDenoisedAmbientOcclusion)
* fbx file
	* [https://github.com/derkreature/IBLBaker](https://github.com/derkreature/IBLBaker)
* Pef
	* [Nsight](https://news.developer.nvidia.com/nsight-graphics-2020-2/)
	
