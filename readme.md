# Corona
Corona pandemic made me to spend some time for this hobby project. That is the reason why the name of the project is Corona.

## Build
Requirements:

* Visual Studio 2022 with the MSVC C++ toolchain.
* CMake 3.21 or newer, available on `PATH`.
* Git and Git LFS, available on `PATH`, for submodule restore and large binary dependencies.
* Network access for the first dependency restore.
* The NuGet CLI is vendored at `tools/nuget/nuget.exe`; no system-wide NuGet install is required.

Recommended fresh-clone bootstrap:

```powershell
.\scripts\bootstrap_windows.ps1
```

The bootstrap script keeps MSBuild and CMake as host requirements. It updates submodules, restores NuGet packages with `tools/nuget/nuget.exe`, installs a copy-only Vulkan SDK under `.deps/VulkanSDK/` when a compatible SDK is not already configured, generates the local PhysX install tree, configures CMake, builds the Release preset, and checks the expected runtime DLLs under `bin/`.

Useful bootstrap options:

```powershell
.\scripts\bootstrap_windows.ps1 -Configuration Debug
.\scripts\bootstrap_windows.ps1 -SkipVulkanSdk
.\scripts\bootstrap_windows.ps1 -ForceVulkanSdkInstall
```

Manual setup:

Clone with submodules, or initialize them after cloning:

```powershell
git clone --recurse-submodules <repo-url>
```

If the repository was already cloned:

```powershell
git submodule update --init --recursive
```

`src/external/physx` is tracked as a Git submodule. The top-level repository stores the PhysX commit reference, not the full PhysX file tree. Local PhysX build/install outputs such as `src/external/physx/physx/install/` are generated artifacts and are not committed by the top-level repository.

Dependency layout:

| Dependency | Location | How it is provided | Notes |
|---|---|---|---|
| Luau | `src/external/luau` | Git submodule | Required by CMake through `add_subdirectory`. |
| PhysX source | `src/external/physx` | Git submodule | The source checkout is not enough by itself; Corona links against the generated PhysX SDK install tree. |
| PhysX SDK install | `src/external/physx/physx/install/vc17win64-cpu-only/PhysX` | Generated locally | Must contain `PhysX_64.lib`, `PhysXFoundation_64.lib`, `PhysXCommon_64.lib`, `PhysXCooking_64.lib`, and matching DLLs. |
| DXC import library and headers | `src/external/dxc` | Vendored in this repo | Used for D3D12 runtime shader compilation. |
| DXC runtime DLLs | `bin/dxcompiler.dll`, `bin/dxil.dll`, or `%VULKAN_SDK%\Bin` | Vulkan SDK / bootstrap copy | `bin/` is ignored by Git. The bootstrap script copies these DLLs from the selected Vulkan SDK after a build. |
| DirectX 12 Agility SDK runtime | `src/external/_packages/Microsoft.Direct3D.D3D12.1.619.2/build/native/bin/x64` | NuGet restore through `tools/nuget/nuget.exe` | CMake copies `D3D12Core.dll` and `d3d12SDKLayers.dll` to `bin/D3D12/`. Needed for newer D3D12 features such as SM 6.9/SER. |
| NVIDIA Streamline SDK | `src/external/streamline-sdk` | Vendored in this repo through Git LFS | Enables DLSS SR/RR on the DX12 path when `include/sl.h`, `lib/x64/sl.interposer.lib`, and `bin/x64/sl.interposer.dll` are present. CMake disables Streamline if the runtime DLLs are missing, avoiding launch-time DLL failures. |
| NVIDIA Aftermath | `src/external/GFSDK_Aftermath` | Vendored in this repo | Linked for D3D12 GPU crash diagnostics; DLL is copied to `bin/`. |
| WinPixEventRuntime | `build/packages/WinPixEventRuntime.*` | NuGet restore through `tools/nuget/nuget.exe` | Required at CMake configure time; DLL is copied to `bin/`. |
| DirectXTex | `src/external/DirectXTex July 2017` | Vendored in this repo | Used by the renderer and texture import tool. |
| Assimp | `src/external/assimp` | Vendored in this repo | Used for model import; `lib/assimp.dll` is copied to `bin/` after building. |
| ImGui | `src/external/imgui` | Vendored in this repo | Used by the runtime UI and both DX12/Vulkan backend bindings. |
| enkiTS | `src/external/enkiTS` | Vendored in this repo | Built directly by CMake for task scheduling. |
| glm | `src/external/glm` | Vendored in this repo | Header-only math dependency used by renderer/UI code. |
| Vulkan SDK | `%VULKAN_SDK%`, `C:\VulkanSDK\1.4.341.1`, or `.deps/VulkanSDK/1.4.341.1` | Local SDK install or bootstrap copy-only install | Required for the Vulkan backend and Vulkan SPIR-V shader generation. |

To generate the PhysX install tree expected by Corona:

```powershell
cd src\external\physx\physx
.\generate_projects.bat vc17win64-cpu-only
cmake --build compiler\vc17win64-cpu-only --config release --target INSTALL
cd ..\..\..\..
```

Restore the NuGet runtime packages before configuring CMake:

```powershell
.\tools\nuget\nuget.exe install Microsoft.Direct3D.D3D12 -Version 1.619.2 -OutputDirectory src\external\_packages
.\tools\nuget\nuget.exe install WinPixEventRuntime -Version 1.0.240308001 -OutputDirectory build\packages
```

CMake auto-detects compatible `Microsoft.Direct3D.D3D12.1.619.*` packages under `src/external/_packages` and copies the runtime DLLs to `bin/D3D12/`. The exported `D3D12SDKVersion` is currently `619`, so keep the package on the `1.619.x` line unless `CORONA_D3D12_AGILITY_SDK_VERSION` is updated too. To override the runtime location manually, point CMake at the folder that contains `D3D12Core.dll` and `d3d12SDKLayers.dll`:

```powershell
cmake --preset vs2022-x64 -DCORONA_D3D12_AGILITY_BIN_DIR="C:\path\to\Microsoft.Direct3D.D3D12.1.619.2\build\native\bin\x64"
```

Current CMake lookup uses `build/packages/WinPixEventRuntime.*` for WinPix. Do not rely on `src/external/WinPixEventRuntime` as the active restore path for a fresh clone.

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

The executable is emitted to `bin/Corona.exe`, and the Visual Studio debugger working directory is `bin/`. The solution is generated at `out/build/vs2022-x64/Corona.sln`.

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

* `hybrid`: GBuffer rasterization plus ray-traced shadow, contact AO, sky lighting, reflection, diffuse GI, denoising, lighting, tone mapping, and optional TAA.
* `pathtracing`: full-screen path tracing mode with accumulation, useful as a simpler reference path and for backend validation.

`--render-mode pt`, `--render-mode pathtracing`, and `--render-mode path-tracing` select the path tracing mode. Frame timing logs are written to `logs/fps_perf.log` and include CPU frame timing plus per-pass GPU timings for backend comparisons.

The captures below use the last saved camera/light state. Captions call out the AA mode used for each image.

Hybrid final resolve, AA = DLSS RR/SR:

![Hybrid final resolve](./docs/images/readme_hybrid_final.png)

Hybrid final resolve, AA = TAA at native resolution:

![Hybrid final resolve, TAA native](./docs/images/readme_hybrid_taa_native.png)

Path tracing reference, AA = off, 1000 spp:

![Path tracing reference](./docs/images/readme_path_tracing_current.png)

## GI And Denoising
The hybrid renderer has several real-time diffuse GI paths:

| Mode | Command value | Summary |
|---|---|---|
| Simple raytrace | `simple` | One cosine-hemisphere DXR ray per pixel, followed by temporal and spatial denoising. |
| Spatial hash | `spatial-hash`, `hash`, `sharc` | A SHaRC-style sparse surface cache. Visible primary-hit cells are stored in a hash table, traced per cell, accumulated as RGB SH, and evaluated per pixel normal. |
| Screen probe | `screen-probe`, `probe` | Screen-space probe atlas path used for comparison with cache-based GI. |

The default spatial hash path stores diffuse GI in a sparse surface cache:

* The GBuffer depth and normals reconstruct visible surface cells.
* Active cells trace a small set of cosine-weighted diffuse rays.
* Direct sky diffuse is handled by a separate ray-traced sky lighting pass, so the GI cache stays focused on surface bounce energy.
* A surface hit evaluates direct-light visibility at the hit point and writes Lambert diffuse irradiance into RGB SH.
* Pixels query the cached SH with the pixel normal, then continue through temporal and spatial filtering.

### Contact AO And Sky Lighting
The hybrid lighting path also includes a full-resolution RTAO pass and a dedicated ray-traced sky lighting pass. RTAO fills high-frequency contact occlusion that the lower-frequency diffuse GI cache can miss, then applies it gently to surface-bounce diffuse GI instead of multiplying the whole final image. Sky lighting traces visibility toward an up-guided sky hemisphere, denoises the raw result, and adds the visible sky diffuse term separately in `LightingPS.hlsl`.

Useful controls include `--rtao`, `--no-rtao`, `--rtao-radius`, `--rtao-samples`, `--sky-lighting`, `--no-sky-lighting`, and `--sky-lighting-strength`. A fuller implementation note is available in [`RTAO_SKY_LIGHTING_IMPLEMENTATION.md`](./reports/RTAO_SKY_LIGHTING_IMPLEMENTATION.md).

### Spatial Hash Diffuse GI
The spatial hash GI mode is inspired by NVIDIA SHaRC, but the current implementation is intentionally small and renderer-local. It does not store radiance in a dense voxel texture. Instead, it uses a sparse hash table of visible surface cells backed by `StructuredBuffer` resources:

* Primary-hit pixels reconstruct world position and normal from the GBuffer, quantize the position by `CellSize`, and insert that integer cell key into a `1 << 20` entry hash table.
* The update pass writes `SpatialHashGIUpdateKeys`, `SpatialHashGICellPosition`, `SpatialHashGICellNormal`, and `SpatialHashGICellScore`. If several pixels map to the same cell, the representative sample closest to the cell center wins through an atomic priority score.
* The ray generation shader dispatches over hash slots, not pixels. Empty slots exit immediately; active cells trace `RaysPerCell` diffuse paths and support `MaxBounces`.
* Each active cell stores RGB spherical harmonics using four coefficients, L0 plus L1: `SpatialHashGITraceSH[4]` for the current trace and double-buffered `SpatialHashGIResolvedSH[2][4]` for history. `ResolvedSH0.w` stores the accumulated sample confidence.
* The resolve pass searches the previous hash table by key, so a cell can keep history even if its slot moves after collisions. Current cell samples are added with sample-count weighting instead of replacing the cache.
* Light changes use adaptive history decay. Cells whose SH energy barely changes keep integrating, while cells with visible lighting changes adapt faster.
* The query pass reconstructs each pixel's cell, blends neighboring cells, optionally smooths a 3x3x3 neighborhood, evaluates the cached SH with the pixel normal, and writes the diffuse GI cache image.

This means the cache is surface-centered in practice: only cells touched by visible primary rays are allocated or refreshed, while the storage itself remains a sparse spatial hash. The main cost knob is therefore active cells times `RaysPerCell`, not screen pixels times rays.

Enable it interactively:

```powershell
.\Corona.exe --backend dx12 --render-mode hybrid --gi-mode spatial-hash --spatial-hash-cell 48 --spatial-hash-rays 2 --spatial-hash-bounces 2
```

Specular GI/reflection is produced by the separate ray-traced reflection path and denoised alongside the diffuse GI buffers. The `pathtracing` render mode is a separate accumulated reference path, not the same algorithm used by the real-time hybrid GI pass.

Pre-denoising diffuse GI captures:

Raw diffuse GI:

[![Raw diffuse GI](./docs/images/readme_gi_raw.png)](./docs/images/readme_gi_raw.png)

Screen probe GI:

[![Screen probe GI before denoising](./docs/images/readme_gi_screen_probe_raw.png)](./docs/images/readme_gi_screen_probe_raw.png)

Spatial hash GI:

[![Spatial hash GI before denoising](./docs/images/readme_gi_spatial_hash_raw.png)](./docs/images/readme_gi_spatial_hash_raw.png)

## DLSS SR/RR Integration
DLSS is integrated through NVIDIA Streamline on the DX12 backend:

* DLSS SR uses the lighting buffer, depth, and motion vectors to upscale from render resolution to display resolution.
* DLSS RR runs before SR and reconstructs the render-resolution lighting buffer using color, depth, motion vectors, normals, roughness, albedo, and specular albedo.
* Both SR and RR share the same Streamline frame token and camera constants for the frame.
* The hybrid pipeline order is `Lighting -> DLSS RR -> DLSS SR -> ToneMap/Present` when RR is enabled, with TAA used as the fallback if Streamline evaluation fails.

DLSS SR/RR currently belongs to the DX12/Streamline path. Other backends or unavailable Streamline features fall back to the non-DLSS AA path selected by the runtime.

The important practical result is that the GI shader can stay intentionally simple while the final image becomes much more stable. The real-time diffuse GI pass is only a low-sample estimator focused on surface bounce energy, with sky diffuse now handled by the separate sky lighting pass. By itself, that raw GI signal is visibly noisy and far from converged.

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
Run from the `bin/` directory, or set the working directory to `bin/` when launching from Visual Studio:

```powershell
cd bin
.\Corona.exe
.\Corona.exe --user-mode --backend dx12 --render-mode hybrid --aa dlss-rr
.\Corona.exe --user-mode --backend dx12 --render-mode hybrid --aa taa
.\Corona.exe --user-mode --backend vulkan --render-mode hybrid --aa taa
.\Corona.exe --user-mode --backend dx12 --render-mode pathtracing --aa off
.\Corona.exe --user-mode --backend dx12 --render-mode hybrid --gi-mode spatial-hash --spatial-hash-rays 2 --spatial-hash-bounces 2
```

Launching without arguments starts the interactive DX12 hybrid renderer on the Sponza scene, with DLSS RR selected by default. Use `--startup-scripts` to launch the Luau-driven procedural startup scene instead.
This guide lists the launch options that are useful for interactive/user-mode runs.

Supported options:

| Option | Values | Notes |
|---|---|---|
| `--user-mode`, `--manual`, `-user` | none | Forces an interactive run from a shortcut or script. |
| `--backend`, `-backend` | `dx12`, `vulkan`, `vk` | Selects the render backend. Unknown values fall back to DX12. |
| `--render-mode`, `-render` | `hybrid`, `pt`, `pathtracing`, `path-tracing`, `path_tracing` | Selects hybrid rendering or full-screen path tracing. |
| `--aa`, `-aa` | `off`, `taa`, `dlss`, `dlss-sr`, `sr`, `dlss-rr`, `rr` | Sets the startup AA mode. DLSS modes fall back to TAA when Streamline/DLSS is unavailable. |
| `--sponza`, `--scene-sponza`, `--sponza-fly`, `--fly-camera` | none | Launches the hybrid Sponza inspection scene and disables startup scripts. This is also the no-argument default. |
| `--startup-scripts`, `--game-character`, `--dungeon-character` | none | Enables the Luau startup scripts/procedural scene instead of the no-argument Sponza default. |
| `--no-startup-scripts`, `--disable-startup-scripts` | none | Disables Luau startup scripts without forcing the Sponza fly camera. |
| `--ray-noise`, `--noise`, `-noise`, `-n` | `r2`, `blue`, `stable` | Selects the hybrid RT GI/reflection sampling noise. `r2` is the default for calmer DLSS RR convergence. |
| `--rtao`, `--no-rtao` | none | Enables or disables the hybrid ray-traced contact AO pass. |
| `--rtao-radius`, `--rtao-samples`, `--rtao-power`, `--rtao-bias` | numeric | Tunes the RTAO ray distance, sample count, contrast, and normal bias. |
| `--sky-lighting`, `--no-sky-lighting` | none | Enables or disables the dedicated ray-traced sky diffuse lighting pass. |
| `--sky-lighting-strength` | `0.0`-`1.0` | Scales the separated sky diffuse term in the lighting pass. |
| `--gi-mode`, `-gi` | `simple`, `spatial-hash`, `hash`, `sharc`, `screen-probe`, `probe` | Selects the hybrid diffuse GI method. |
| `--spatial-hash-cell` | `4.0`-`256.0` | World-space cell size for the spatial hash GI cache. Default is `48.0`. |
| `--spatial-hash-rays` | `1`-`8` | Diffuse rays traced per active spatial hash cell. Default is `2`. |
| `--spatial-hash-bounces`, `--spatial-hash-depth` | `1`-`8` | Maximum diffuse path depth for cell tracing. Default is `2`. |
| `--spatial-hash-interp`, `--spatial-hash-interpolation` | `0.0`-`1.0` | Blends the base cell with neighboring cell interpolation during query. Default is `1.0`. |
| `--spatial-hash-smoothing` | `0.0`-`1.0` | Controls 3x3x3 SH neighborhood smoothing after interpolation. Default is `0.65`. |
| `--dlss-jitter-scale` | `0.25`-`16.0` | Multiplies the DLSS/RR jitter phase count. Default is `4.0` for longer phase cycles. |
| `--dlss-jitter-phases` | `0`-`512` | Overrides the DLSS/RR jitter phase count directly. `0` means automatic scaled mode. |
| `--camera-path`, `-camera-path` | `latest` or `.coronapath` path | Loads a recorded camera/light path at startup. `latest` selects the newest recorded path. |
| `--no-imgui`, `--disable-imgui` | none | Starts without the ImGui overlay. |

Options can be written either as `--backend vulkan` or `--backend=vulkan`.

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
	
