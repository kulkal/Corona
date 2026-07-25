# Corona

A hybrid deferred renderer / game engine for Windows built on D3D12 (with an
experimental Vulkan backend). Ray-traced shadows, reflections, and diffuse GI
are denoised by DLSS Ray Reconstruction; scripting is Luau; physics is PhysX.

## Requirements

- Windows 10/11 x64 with an NVIDIA RTX GPU (DLSS / Ray Reconstruction)
- Visual Studio 2022 with the "Desktop development with C++" workload
- CMake 3.21+ and git

## Build

Clone with submodules (PhysX, Luau, MathLib):

```powershell
git clone --recurse-submodules https://github.com/kulkal/Corona.git
cd Corona
```

Run the bootstrap once. It restores the NuGet runtime packages (D3D12 Agility
SDK, WinPixEventRuntime), installs the Vulkan SDK locally under `.deps/`,
builds PhysX, and then builds the engine:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap_windows.ps1
```

After that, incremental builds only need CMake:

```powershell
cmake --preset vs2022-x64          # configure (first time only)
cmake --build --preset release     # or: --preset debug
```

Binaries land in `bin\` (`Corona.exe` plus the `CoronaMeshImport` /
`CoronaTextureImport` asset tools).

## Run

```bat
editor.bat
```

opens the Sponza map in editor mode with DLSS-RR. Extra arguments are
forwarded to the engine, e.g.:

```bat
editor.bat --4k                     rem 3840x2160 backbuffer
editor.bat --backend vulkan        rem Vulkan backend (hybrid mode)
editor.bat --exit-after-frames 300 rem smoke test: exit after N frames
```

Meshes referenced by maps (`assets\maps\*.map`) are FBX/OBJ sources; on first
load they are converted to a sidecar `.cmesh` cache by `CoronaMeshImport`
(ufbx-based) and reused afterwards.
