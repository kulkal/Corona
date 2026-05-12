# DLSS RR Integration Report

작성일: 2026-05-11
범위: 현재 DX12/Streamline 기반 DLSS SR/RR 통합 구현.

## 개요

DLSS RR은 최종 화면 업스케일러가 아니라 render resolution에서 lighting 신호를 재구성하는 단계로 배치되어 있다. 현재 구현은 두 렌더 모드를 다르게 처리한다.

- `HYBRID`: `Lighting -> DLSS RR -> DLSS SR -> ToneMap/Present`
- `PATHTRACING`: `PathTracing -> DLSS RR -> ToneMap/Present`

Hybrid 모드는 DLSS RR 결과를 다시 DLSS SR에 넣어 display resolution으로 올린다. Path tracing 모드는 render resolution을 display resolution과 같게 유지하고, RR만 수행한다.

관련 핵심 파일:

- `src/Corona.cpp`
- `src/Corona.h`
- `src/Shaders/GBuffer.hlsl`
- `src/Shaders/Common.hlsl`
- `src/Corona.IndirectLighting.Reflection.cpp`
- `src/Shaders/RaytracedReflection.hlsl`
- `src/Corona.PathTracingPass.cpp`
- `src/Shaders/PathTracing.hlsl`

## Streamline 초기화

Streamline은 DX12 시작 경로에서 초기화한다. Vulkan 시작 요청이면 Streamline 초기화를 건너뛴다.

`InitStreamline()`에서 로드하는 feature:

```cpp
sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
```

주요 preference:

- `sl::RenderAPI::eD3D12`
- `sl::EngineType::eCustom`
- `sl::PreferenceFlags::eDisableCLStateTracking`
- `sl::PreferenceFlags::eUseFrameBasedResourceTagging`
- 로그/데이터 경로는 `RuntimePaths::LogDirectory()`

DX12 adapter 생성 뒤 `slIsFeatureSupported()`로 지원 여부를 갱신한다.

```cpp
bDLSSAvailable = slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo) == sl::Result::eOk;
bDLSSRRAvailable = slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo) == sl::Result::eOk;
```

## AA 모드 정규화

`NormalizeAntiAliasingMode()`가 backend와 feature availability를 기준으로 요청한 AA 모드를 정규화한다.

Hybrid:

- `DLSS_RR` 요청은 DLSS RR과 DLSS SR이 모두 가능할 때만 유지된다.
- RR은 가능하지만 SR이 없으면 최종 display upscale을 할 수 없으므로 `DLSS_SR` 또는 `TAA`로 fallback된다.
- `DLSS_SR` 요청에서 SR이 없으면 `TAA`로 fallback된다.
- DX12/Streamline 경로가 아니면 DLSS 계열은 `TAA`로 fallback된다.

Path tracing:

- `DLSS_RR` 또는 `DLSS_SR` 요청은 RR이 가능하고 `bEnablePathTracingDLSSRR`가 켜져 있을 때만 `DLSS_RR`로 유지된다.
- 그렇지 않으면 `OFF`로 fallback된다.
- Path tracing에서는 TAA를 사용하지 않으므로 `TAA` 요청도 `OFF`가 된다.

## Streamline Frame State

RR과 SR은 같은 frame 안에서 연속 평가될 수 있다. 그래서 Streamline frame token과 common constants는 pass마다 새로 만들지 않고 frame 단위로 공유한다.

상태:

```cpp
sl::FrameToken* StreamlineFrameToken = nullptr;
uint32_t StreamlineFrameIndex = 0;
bool bStreamlineConstantsSetThisFrame = false;
bool bDLSSResetNeeded = false;
```

함수:

- `BeginStreamlineFrame()`
- `EnsureStreamlineConstants()`

동작:

1. `BeginStreamlineFrame()`이 frame token을 한 번 확보한다.
2. `EnsureStreamlineConstants()`가 같은 frame에서 `slSetConstants()`를 한 번만 호출한다.
3. `DLSSRRPass()`와 `DLSSPass()`가 같은 token/constants를 공유한다.

이 구조는 과거에 발생했던 Streamline 로그 오류를 방지한다.

```text
Setting different 'common' constants multiple times within the same frame is NOT allowed!
```

## Streamline Constants

`BuildStreamlineConstants()`는 현재 unjittered projection, 이전 view, 현재 inverse view를 이용해 reprojection 행렬을 만든다.

```cpp
const glm::mat4x4 currentProj = unjitteredProjMat;
const glm::mat4x4 currentInvProj = glm::inverse(currentProj);
const glm::mat4x4 currentViewToPrevView = prevViewMat * invViewMat;
const glm::mat4x4 clipToPrevClip = currentProj * currentViewToPrevView * currentInvProj;
const glm::mat4x4 prevClipToClip = glm::inverse(clipToPrevClip);
```

현재 설정:

- `cameraViewToClip`: current unjittered projection
- `clipToCameraView`: inverse current projection
- `clipToPrevClip`: current clip에서 previous clip으로 가는 변환
- `prevClipToClip`: `clipToPrevClip`의 inverse
- `jitterOffset`: hybrid에서는 `CurrentJitter * 0.5`, path tracing에서는 `0`
- `mvecScale`: `(-1, -1)`
- `cameraMotionIncluded`: path tracing RR에서는 `false`, 그 외에는 `true`
- `motionVectorsJittered`: `false`
- `motionVectorsDilated`: `false`

`mvecScale = (-1, -1)`은 내부 velocity buffer가 `current - previous` 성격인 것을 DLSS/Streamline 기대 방향에 맞추기 위한 보정이다.

## Render Resolution

`RefreshUpscaleSettings()`가 AA 모드에 맞춰 render resolution을 갱신한다.

Hybrid DLSS SR:

- `slDLSSGetOptimalSettings()` 결과를 render size로 사용한다.
- output size는 display size다.

Hybrid DLSS RR:

- `slDLSSDGetOptimalSettings()` 결과를 render size로 사용한다.
- RR은 render size에서 `DLSSRRBuffer`를 생성한다.
- SR이 RR 결과를 display size로 upscale한다.

Path tracing DLSS RR:

- render size를 display size와 같게 둔다.
- DLSS jitter phase count를 `1`로 설정한다.
- SR pass를 별도로 실행하지 않는다.

관련 helper:

```cpp
bool IsDLSSUpscaleEnabled() const {
  return RenderingMode == ERenderingMode::HYBRID &&
         (IsDLSSSREnabled() || IsDLSSRREnabled());
}

UINT GetRenderWidth() const {
  return IsDLSSUpscaleEnabled() ? RenderWidth : m_width;
}
```

## Resource Layout

DLSS RR 관련 주요 texture:

| Resource | Format | Size | Role |
|---|---|---|---|
| `LightingBuffer` | `RGBA16Float` | render size | hybrid lighting output, RR color input |
| `DLSSRRBuffer` | `RGBA16Float` | render size | RR output |
| `AlbedoBuffer` | `RGBA8Unorm` | render size | RR albedo input |
| `SpecularAlbedoBuffer` | `RGBA8Unorm` | render size | RR specular albedo input |
| `NormalBuffers[]` | `RGBA16Float` | render size | RR normal input |
| `GeomNormalBuffers[]` | `RGBA16Float` | render size | reflection/path diagnostics, guide data source |
| `VelocityBuffer` | `RG16Float` | render size | RR/SR motion vector input |
| `RoughnessMetalicBuffer` | `RGBA8Unorm` | render size | RR roughness input, metallic stored for shading |
| `UnjitteredDepthBuffers[]` | `R32Float` | render size | RR/SR depth input |
| `PathTracingSpecularHitDistanceBuffer` | `R32Float` | render size | optional RR specular hit distance |
| `PathTracingSpecularMotionVectorBuffer` | `RG16Float` | render size | optional RR specular motion vectors |

`RenderResolutionResourcesMatchCurrentState()`는 위 resource들이 현재 display/render size와 맞는지 검사한다. AA 모드 전환, DLSS 품질 변경, render mode 전환 시 `ResetAllAccumulationState()`와 resource reload 경로가 이어진다.

## GBuffer Inputs

`GBuffer.hlsl`은 RR에 필요한 주요 입력을 MRT로 출력한다.

```hlsl
struct PS_OUTPUT
{
    float4 Albedo : SV_Target0;
    float4 SpecularAlbedo : SV_Target1;
    float4 Normal : SV_Target2;
    float4 GeomNormal : SV_Target3;
    float2 Velocity : SV_Target4;
    float4 Material : SV_Target5;
    float UnjitteredDepth : SV_Target6;
};
```

Specular albedo는 shared helper인 `ComputeDLSSRRSpecularAlbedo()`를 사용한다.

```hlsl
float3 surfaceToView = CommonSafeNormalize(-ViewDir.xyz, WorldNormal);
output.SpecularAlbedo.xyz =
    ComputeDLSSRRSpecularAlbedo(Albedo.xyz, output.Material.y, output.Material.x, WorldNormal, surfaceToView);
```

이전 구현의 단순 `lerp(0.04, albedo, metallic)`보다 RR 입력에 맞는 환경 BRDF approximation 기반 값이다.

Velocity는 unjittered current/previous clip position으로 계산한다. Streamline에 넘길 때는 `mvecScale = (-1, -1)`로 부호를 보정한다.

## DLSS RR Pass

`DLSSRRPass()`의 핵심 설정:

```cpp
sl::DLSSDOptions opts{};
opts.mode = ToSLDLSSMode(DLSSQualityMode);
opts.outputWidth = GetRenderWidth();
opts.outputHeight = GetRenderHeight();
opts.colorBuffersHDR = sl::Boolean::eTrue;
opts.preExposure = 1.0f;
opts.exposureScale = 1.0f;
opts.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::eUnpacked;
opts.worldToCameraView = ToSLMatrix(ViewMat);
opts.cameraViewToWorld = ToSLMatrix(InvViewMat);
```

Color input:

- path tracing RR: `PathTracingAccumBuffer[PathTracingWriteIndex]`
- hybrid RR: `LightingBuffer`

Output:

- `DLSSRRBuffer`

항상 tag하는 RR input:

- `kBufferTypeScalingInputColor`
- `kBufferTypeDepth`
- `kBufferTypeMotionVectors`
- `kBufferTypeNormals`
- `kBufferTypeRoughness`
- `kBufferTypeAlbedo`
- `kBufferTypeSpecularAlbedo`
- `kBufferTypeScalingOutputColor`

조건부 tag:

- `kBufferTypeSpecularMotionVectors`
- `kBufferTypeSpecularHitDistance`

조건부 specular metadata는 path tracing과 hybrid가 같은 `PathTracingSpecularHitDistanceBuffer`, `PathTracingSpecularMotionVectorBuffer`를 공유한다.

## DLSS SR Pass

`DLSSPass()`는 hybrid 모드에서 display resolution upscale을 담당한다.

입력 color:

```cpp
Texture* inputColor =
  (bDLSSRROutputValidThisFrame && DLSSRRBuffer) ? DLSSRRBuffer.get() : LightingBuffer.get();
```

출력:

- `ColorBuffers[ColorBufferWriteIndex]`

tag:

- `kBufferTypeScalingInputColor`
- `kBufferTypeDepth`
- `kBufferTypeMotionVectors`
- `kBufferTypeScalingOutputColor`

Hybrid DLSS RR path에서 RR은 SR의 입력을 재구성하고, SR은 display size로 upscale한다. RR 또는 SR이 실패하면 TAA로 fallback한다.

## Hybrid Pipeline

현재 hybrid frame 순서:

1. `GBufferPass()`
2. `RaytraceShadowPass()`
3. optional `RaytraceAOPass()`
4. optional `RaytraceSkyLightingPass()`
5. `RaytraceReflectionPass()`
6. selected diffuse GI pass
   - simple raytrace
   - spatial hash
   - screen probe
7. `TemporalDenoisingPass()`
8. optional `ScreenProbeGIPass()`
9. `LightingPass()`
10. if AA is `DLSS_RR`
    - `DLSSRRPass()`
    - `DLSSPass()`
    - fallback `TemporalAAPass()` if needed
11. else if AA is `DLSS_SR`
    - `DLSSPass()`
    - fallback `TemporalAAPass()` if needed
12. else `TemporalAAPass()`
13. tone map/present

`GetCurrentResolveSource()`는 hybrid에서 fallback 상태를 고려한다.

- `bUseLightingBufferFallbackForToneMap`이 켜져 있으면 `DLSSRRBuffer`가 유효할 때는 RR 결과를, 아니면 `LightingBuffer`를 반환한다.
- 정상 SR resolve가 끝나면 `ColorBuffers[ResolvedColorBufferIndex]`를 반환한다.

## Hybrid Specular RR Metadata

Hybrid reflection pass는 RR specular metadata도 함께 쓸 수 있다.

CPU side:

- `bEnableHybridRRSpecularMotionVectors`
- `bEnableHybridRRSpecularHitDistance`
- `bEnableHybridRRSpecularGuideRay`
- `HybridRRSpecularMotionVectorScale`

Command-line:

```text
--hybrid-rr-specular-mv
--enable-hybrid-rr-specular-mv
--no-hybrid-rr-specular-mv
--disable-hybrid-rr-specular-mv

--hybrid-rr-specular-hit-distance
--hybrid-rr-specular-hitdist
--no-hybrid-rr-specular-hit-distance
--no-hybrid-rr-specular-hitdist

--hybrid-rr-specular-guide-ray
--no-hybrid-rr-specular-guide-ray

--hybrid-rr-specular-mv-scale <value>
```

`RaytraceReflectionPass()`는 매 frame metadata buffer를 clear한 뒤, DLSS RR이 활성화되어 있으면 reflection shader에 metadata write 플래그를 전달한다.

```cpp
RTReflectionViewParam.bWriteRRSpecularMotionVectors =
    (IsDLSSRREnabled() && bEnableHybridRRSpecularMotionVectors) ? 1u : 0u;
RTReflectionViewParam.bWriteRRSpecularHitDistance =
    (IsDLSSRREnabled() && bEnableHybridRRSpecularHitDistance) ? 1u : 0u;
RTReflectionViewParam.bUseRRSpecularGuideRay = bEnableHybridRRSpecularGuideRay ? 1u : 0u;
```

Shader side:

- roughness/metallic/specular energy로 guide ray가 의미 있는지 판단한다.
- 필요하면 mirror direction으로 guide ray를 추가 trace한다.
- guide hit position을 current/previous clip space로 project한다.
- `(prevUV - currentUV) * renderSize * SpecularMotionVectorScale`을 specular motion vector로 쓴다.
- hit distance는 `ProjectionParams.w`로 clamp된다.

RT reflection SER PSO가 활성화되어도 RR metadata contract는 동일하다. SER path는 `TraceReflectionSurfaceRay()` 내부 trace/invoke 방식만 바꾼다.

## Path Tracing RR

Path tracing 모드도 RR에 primary-hit GBuffer를 제공할 수 있다.

활성 조건:

```cpp
RenderingMode == ERenderingMode::PATHTRACING &&
bEnablePathTracingDLSSRR &&
AntiAliasingMode == EAntiAliasingMode::DLSS_RR &&
bDLSSRRAvailable
```

`PathTracingPass()`가 RR primary-hit GBuffer를 쓸 때 출력하는 항목:

- albedo
- specular albedo
- normal
- geometry normal
- velocity
- roughness/metallic
- depth
- specular hit distance
- specular motion vector

Path tracing RR command-line:

```text
--pt-dlss-rr
--pathtracing-dlss-rr
--pt-rr-gbuffer
--no-pt-dlss-rr
--disable-pt-dlss-rr
--no-pt-rr-gbuffer

--pt-rr-specular-mv
--enable-pt-rr-specular-mv
--no-pt-rr-specular-mv
--disable-pt-rr-specular-mv

--pt-rr-specular-hit-distance
--pt-rr-specular-hitdist
--no-pt-rr-specular-hit-distance
--no-pt-rr-specular-hitdist

--pt-rr-stable-primary-rays
--pt-rr-primary-ray-stability
--no-pt-rr-stable-primary-rays
--no-pt-rr-primary-ray-stability

--pt-rr-specular-mv-scale <value>
```

특이점:

- path tracing RR은 SR pass를 실행하지 않는다.
- motion vector buffer는 primary surface 기준 `0`으로 쓴다.
- `cameraMotionIncluded`를 `false`로 넘겨 Streamline이 depth와 clip transform으로 camera motion을 해석하게 한다.
- camera motion 중에는 primary ray sample을 안정화할 수 있다.

## UI And Scripting

C++ ImGui:

- AA mode: `Off`, `TAA`, `DLSS SR`, `DLSS RR`
- DLSS quality: `Quality`, `Balanced`, `Performance`, `Ultra Performance`
- DLSS jitter phase scale
- DLSS jitter phase override
- DLSS SR/RR availability 표시
- render resolution 표시
- hybrid RR specular motion vectors
- hybrid RR specular hit distance
- hybrid RR specular guide ray
- hybrid RR specular MV scale
- path tracing 모드에서 primary-hit GBuffer for DLSS RR
- path tracing 모드에서 stabilize moving primary rays for RR

Luau UI state:

- `aa_mode`
- `dlss_quality`
- `dlss_available`
- `dlss_rr_available`
- `render_width`
- `render_height`
- `dlss_jitter_phase_count`
- `dlss_jitter_phase_count_auto`
- `dlss_jitter_phase_override`
- `dlss_jitter_phase_scale`

현재 Luau user-mode UI는 hybrid RR specular metadata 세부 토글을 별도 항목으로 노출하지 않는다. 해당 토글은 C++ ImGui와 command-line에서 제어한다.

## Diagnostics And Dumps

DLSS/RR 검증에 유용한 dump 경로:

- `dumps/aa_modes`
- `dumps/specular_sequence`
- `dumps/camera_path_frames/<run>/diagnostics`

Camera-path diagnostic frame은 다음 texture를 함께 dump할 수 있다.

- `pt_input`
- `rr_output`
- `depth`
- `motion`
- `specular_hit_distance`
- `specular_motion`
- `roughness_metallic`
- `specular_albedo`
- `albedo`
- `normal`
- `geom_normal`
- `specular_raw`
- `specular_temporal`
- `diffuse_raw`
- `diffuse_temporal`
- optional `screen_probe_gi`

이 dump들은 RR 입력 계약을 확인하는 데 특히 유용하다.

확인 포인트:

- RR output이 render size extent로 생성되는지
- hybrid에서 RR 이후 SR이 display size를 채우는지
- path tracing에서 RR output이 display size와 같은지
- `SpecularAlbedoBuffer`가 비어 있지 않은지
- specular hit distance가 invalid pixel에서 far 값으로 남는지
- specular motion vector가 과도하게 튀지 않는지
- Streamline 로그에 common constants 중복 설정 오류가 없는지

## 과거 주요 문제와 현재 상태

### RR 결과가 화면 일부만 채우던 문제

원인:

- DLSS RR을 display resolution upscale 단계처럼 취급했다.

현재 상태:

- RR output extent는 `GetRenderWidth() x GetRenderHeight()`다.
- Hybrid에서는 RR output을 DLSS SR input으로 넘겨 display resolution으로 올린다.
- Path tracing RR은 render size 자체를 display size로 맞춘다.

### Specular Albedo 누락

원인:

- 초기 GBuffer에 RR이 기대하는 specular albedo가 없었다.

현재 상태:

- `SpecularAlbedoBuffer`가 별도 MRT/UAV로 존재한다.
- `GBuffer.hlsl`과 `PathTracing.hlsl` 모두 `ComputeDLSSRRSpecularAlbedo()`를 사용한다.
- `DLSSRRPass()`가 `kBufferTypeSpecularAlbedo`로 tag한다.

### Streamline common constants 중복 설정

원인:

- RR과 SR pass가 같은 frame에서 각각 frame token/constants를 갱신했다.

현재 상태:

- `BeginStreamlineFrame()`과 `EnsureStreamlineConstants()`가 frame 단위 상태를 공유한다.
- `bStreamlineConstantsSetThisFrame`로 같은 frame의 중복 `slSetConstants()`를 방지한다.

### Camera motion reprojection 불일치

원인:

- reprojection matrix, jitter offset, motion vector 부호가 DLSS 기대값과 맞지 않았다.

현재 상태:

- `clipToPrevClip` / `prevClipToClip`을 실제 camera transform에서 계산한다.
- hybrid jitter는 실제 projection offset 기준으로 `CurrentJitter * 0.5`를 전달한다.
- path tracing RR은 jitter를 `0`으로 전달한다.
- DLSS 경로의 motion vector scale은 `(-1, -1)`이다.

## 현재 한계와 메모

- DLSS SR/RR은 DX12/Streamline 경로에만 있다.
- Vulkan backend에서는 DLSS 계열이 fallback된다.
- Hybrid DLSS RR은 display upscale을 위해 DLSS SR도 필요하다.
- Path tracing RR은 display resolution에서 RR만 수행하므로 성능상 SR 업스케일 이득은 없다.
- Specular motion vector와 hit distance는 optional input이다. 둘 다 항상 좋은 결과를 보장하지 않으므로 scene과 material에 따라 toggle/scale 비교가 필요하다.
- Hybrid RR specular metadata는 reflection pass에서 생성된다. Reflection pass를 끄거나 관련 resource가 없으면 metadata도 없다.
- Path tracing RR metadata는 primary-hit 기반이며, 완전한 path-space motion solution은 아니다.

## 빠른 검증

Hybrid RR:

```powershell
.\Corona.exe --backend dx12 --render-mode hybrid --aa dlss-rr --user-mode
```

Path tracing RR:

```powershell
.\Corona.exe --backend dx12 --render-mode pathtracing --aa dlss-rr --pt-dlss-rr --pt-spp 1
```

Camera-path diagnostics:

```powershell
.\Corona.exe --backend dx12 --render-mode hybrid --aa dlss-rr --camera-path latest --camera-path-dump --camera-path-diagnostics
```

검증 항목:

- GPU timing에 `DLSSRR`가 기록된다.
- Hybrid RR에서는 `DLSSRR` 뒤에 `DLSSSR`가 기록된다.
- Path tracing RR에서는 `PathTracing` 뒤에 `DLSSRR`가 기록되고 `DLSSSR`는 없다.
- RR 실패 시 hybrid는 TAA fallback으로 화면을 유지한다.
- `logs/sl.log`에 common constants 중복 오류가 없다.
- camera 이동/회전 중 texture sliding 또는 overshoot가 재발하지 않는다.
