# Path Tracing 구현 리포트

작성일: 2026-05-11
범위: 현재 코드 기준의 전체 화면 path tracing 모드 구현 내용.

## 개요

렌더러에는 두 가지 상위 렌더 모드가 있다.

- `HYBRID`: raster GBuffer 이후 RT shadow, AO, sky lighting, reflection, diffuse GI, denoising, lighting, temporal/DLSS resolve를 조합하는 실시간 경로.
- `PATHTRACING`: 전체 화면 DXR path tracing과 progressive accumulation을 사용하는 기준 렌더 경로.

`PATHTRACING`은 hybrid diffuse GI와 같은 알고리즘이 아니다. hybrid의 spatial hash/screen probe/simple GI는 실시간용 저샘플 GI 경로이고, path tracing은 별도의 누적 reference 성격 경로다.

실행 예:

```powershell
.\Corona.exe --backend dx12 --render-mode pathtracing --aa off
.\Corona.exe --backend dx12 --render-mode pt --aa dlss-rr --pt-dlss-rr
```

`--render-mode`는 `pt`, `pathtracing`, `path-tracing`, `path_tracing`, `path tracing` 값을 path tracing으로 해석한다.

## 주요 파일

- `src/Corona.h`
  - `ERenderingMode::PATHTRACING`
  - `PathTracingViewParamCB`
  - path tracing accumulation buffer
  - DLSS RR 보조 버퍼와 런타임 토글
- `src/Corona.PathTracingPass.cpp`
  - `InitPathTracingPass()`
  - `PathTracingPass()`
- `src/Shaders/PathTracing.hlsl`
  - ray generation, closest hit, any hit, miss, shadow miss shader
- `src/Corona.cpp`
  - 렌더 모드 선택
  - 리소스 생성
  - 커맨드라인 파싱
  - DLSS RR 연동
  - ImGui 제어
- `src/Corona.Scripting.cpp`
  - Luau에 노출되는 path tracing 제어값
- `src/scripts/startup/040_imgui_controls.luau`
  - user-mode UI의 path tracing 컨트롤

## 런타임 상태

`PathTracingViewParamCB`는 CPU에서 shader로 전달되는 path tracing constant buffer다. 현재 포함하는 값은 다음과 같다.

- 현재/이전 view, projection matrix
- near/far projection parameter
- 태양 방향, 세기, 색, angular radius, sample count
- sky top/bottom color와 sky intensity
- max bounce count와 samples per pixel
- debug visualization mode
- diffuse GI, specular GI, direct diffuse, direct specular 토글
- 최대 `MaxPointLights`개의 point light
- DLSS RR primary-hit GBuffer 플래그와 specular motion-vector scale

핵심 persistent 상태:

```cpp
shared_ptr<RTPipelineStateObject> PSO_PATH_TRACING;
shared_ptr<Texture> PathTracingAccumBuffer[2];
UINT PathTracingWriteIndex = 0;
UINT32 PathTracingAccumulatedFrames = 0;
UINT32 PathTracingLastDispatchSamplesPerPixel = 1;

shared_ptr<Texture> PathTracingSpecularHitDistanceBuffer;
shared_ptr<Texture> PathTracingSpecularMotionVectorBuffer;
```

`PathTracingAccumBuffer`는 display size의 `RGBA32Float` UAV로 생성된다. 현재 pass는 `PathTracingWriteIndex`가 가리키는 buffer에 누적 결과를 쓰며, 매 프레임 ping-pong하지는 않는다.

DLSS RR 보조 버퍼는 render size로 생성된다.

- `PathTracingSpecularHitDistanceBuffer`: `R32Float`
- `PathTracingSpecularMotionVectorBuffer`: `RG16Float`

## Pipeline 초기화

`InitPathTracingPass()`는 `Shaders\PathTracing.hlsl`로 RT PSO를 만든다.

구성 shader:

- ray generation: `PathTracingRayGen`
- miss: `PathTracingMiss`
- shadow miss: `ShadowMiss`
- hit group: `HitGroup`
  - closest hit: `PathTracingClosestHit`
  - any hit: `PathTracingAnyHit`

Global binding:

- UAV
  - `OutputColor`
  - `OutAlbedo`
  - `OutSpecularAlbedo`
  - `OutNormal`
  - `OutGeomNormal`
  - `OutVelocity`
  - `OutRoughnessMetallic`
  - `OutDepth`
  - `OutSpecularHitDistance`
  - `OutSpecularMotionVector`
- SRV
  - `gRtScene`
- CBV
  - `ViewParameter`
- sampler
  - `sampleWrap`

Hit program binding:

- `vertices`
- `indices`
- `InstanceProperty`
- `AlbedoTex`
- `NormalTex`
- `RoughnessTex`
- `MetallicTex`

PSO는 max recursion depth `8`, payload size `256`, attribute size `sizeof(float) * 2`로 설정된다.

## Frame 실행 흐름

`RenderingMode == ERenderingMode::PATHTRACING`이면 main render loop에서 `PathTracingPass()`를 실행한다.

프레임 흐름:

1. 현재 path tracing accumulation buffer를 선택한다.
2. DLSS RR primary-hit GBuffer를 써야 하는지 판단한다.
3. accumulation buffer를 UAV 상태로 전환한다.
4. RR metadata 출력이 켜져 있으면 primary-hit GBuffer 대상도 UAV 상태로 전환한다.
5. camera, sun, sky 상태 변화가 있으면 accumulation을 reset한다.
6. `PathTracingViewParamCB`를 채운다.
7. `RTPassBuilder`로 TLAS, 출력 UAV, CBV, sampler, instance별 material resource를 바인딩한다.
8. display resolution 전체에 대해 ray dispatch를 수행한다.
9. debug mode가 꺼져 있으면 `PathTracingAccumulatedFrames`를 증가시킨다.
10. 쓴 resource를 shader-read 상태로 되돌린다.
11. path tracing DLSS RR이 활성화되어 있으면 tone mapping 전에 `DLSSRRPass()`를 실행한다.
12. DLSS RR 결과가 유효하면 `DLSSRRBuffer`, 아니면 accumulation buffer를 tone map/present 입력으로 사용한다.

Path tracing 모드에서는 TAA를 사용하지 않는다. TAA 요청은 `OFF`로 정규화된다. DLSS SR/RR 요청은 Streamline RR이 사용 가능하고 path tracing RR이 켜져 있을 때만 `DLSS_RR`로 유지되고, 아니면 `OFF`로 떨어진다.

## Accumulation

shader는 `OutputColor`에 progressive accumulation을 수행한다.

```text
first frame: current radiance
later frames: (previous * frameCount + current) / (frameCount + 1)
```

다음 변화가 있으면 accumulation이 reset된다.

- view matrix
- 태양 방향
- 태양 세기
- sky top color
- sky bottom color
- sky intensity
- debug mode
- path tracing max bounces
- path tracing samples per pixel
- explicit reset command
- global accumulation reset

Debug visualization mode에서는 accumulation을 하지 않고 현재 frame의 diagnostic view만 렌더링한다.

## Sample Scheduling

`PathTracingViewParam.SamplesPerPixel`은 `1..16`으로 clamp된다.

Debug mode가 꺼져 있고 DLSS RR primary-hit GBuffer를 쓰지 않는 경우, CPU가 실제 dispatch SPP를 단계적으로 올린다.

- accumulated frame `0`: `1` spp
- accumulated frames `< 8`: 최대 `2` spp
- 이후 frame: 요청한 spp

이 정책은 초기 반응성을 유지하면서 정지 상태에서는 높은 SPP로 수렴할 수 있게 한다. 실제 dispatch된 SPP는 `PathTracingLastDispatchSamplesPerPixel`로 UI에 표시된다.

Path tracing DLSS RR primary-hit GBuffer 출력이 켜져 있으면 color와 auxiliary buffer alignment를 유지하기 위해 요청한 SPP를 그대로 dispatch한다.

## Shader 알고리즘

`PathTracingRayGen`은 pixel마다 하나 이상의 sample을 추적한다. 각 sample은 다음 순서로 처리된다.

1. pixel, frame, sample index로 PCG 기반 random seed를 만든다.
2. inverse projection과 inverse view matrix로 camera ray를 만든다.
3. DLSS RR metadata 안정화가 필요한 primary ray가 아니면 sub-pixel jitter를 적용한다.
4. `MaxBounces`까지 path를 추적한다.
5. direct lighting과 sky miss radiance를 누적한다.
6. diffuse 또는 specular lobe를 선택해 다음 bounce 방향을 만든다.
7. 일정 bounce 이후 Russian roulette으로 path를 종료한다.
8. firefly clamp 후 accumulation에 더한다.

`PathTracingClosestHit`의 역할:

- vertex/index buffer에서 보간된 vertex attribute를 읽는다.
- hit distance와 texture LOD constant를 이용해 texture mip level을 계산한다.
- albedo, normal, roughness, metallic texture를 sample한다.
- instance roughness/metallic override를 적용한다.
- 유효한 tangent basis가 있으면 normal map을 적용한다.
- imported/two-sided mesh에서 normal이 path를 가두지 않도록 shading normal과 geometric normal을 view 방향으로 뒤집는다.
- 태양광을 spherical cap directional light로 sampling한다.
- point light를 shadow ray와 거리/range attenuation으로 평가한다.
- Fresnel 기반 확률로 diffuse/specular bounce lobe를 선택한다.
- 선택된 lobe 확률로 throughput을 보정한다.

`PathTracingMiss`는 `SkyColorBottom`, `SkyColorTop`, `SkyIntensity`를 이용해 sky gradient radiance를 반환한다.

`PathTracingAnyHit`은 alpha-tested instance에 대해 albedo alpha를 sample하고, alpha가 cutoff보다 낮으면 `IgnoreHit()`으로 traversal을 계속한다.

## Lighting 제어

Path tracing은 renderer의 상위 lighting toggle을 재사용한다.

- `bEnableDiffuseGI`
- `bEnableSpecularGI`
- `bEnableDirectDiffuse`
- `bEnableDirectSpecular`

RTAO는 path tracing pass에서 평가하지 않는다. path tracing shader parameter에서는 `bEnableRTAO`를 `0`으로 전달한다.

Sun 관련 값:

- `RTShadowViewParam.ShadowLightRadius`가 `DirectLightAngularRadius`로 전달된다.
- `PathTracingViewParam.DirectLightSampleCount`는 sun sample count이며 `1..8`로 clamp된다.

Point light는 `ApplyRenderPointLightsToFrameParams()`를 통해 frame constant로 복사되고 closest-hit shader에서 직접 평가된다.

## DLSS RR 연동

Path tracing은 accumulated color와 함께 primary-hit GBuffer를 써서 DLSS RR 입력을 제공할 수 있다.

활성 조건:

```cpp
RenderingMode == PATHTRACING &&
bEnablePathTracingDLSSRR &&
AntiAliasingMode == DLSS_RR &&
bDLSSRRAvailable
```

활성화되면 path tracing pass가 다음 buffer를 쓴다.

- albedo
- specular albedo
- normal
- geometry normal
- velocity
- roughness/metallic
- depth
- specular hit distance
- specular motion vector

Primary surface velocity는 `0`으로 쓴다. PT+RR에서는 Streamline이 depth와 current/previous clip transform에서 camera motion을 재구성하도록 설정한다.

Specular RR metadata는 smooth 또는 metallic primary hit에서 guide reflection ray를 추가로 추적해 만든다.

1. primary material이 specular guide에 의미가 있는지 roughness/metallic/specular energy로 판단한다.
2. primary ray를 geometric normal 기준으로 reflect한다.
3. first-hit guide ray를 trace한다.
4. guide hit distance를 쓴다.
5. guide hit position을 current/previous clip space로 project한다.
6. `PathTracingRRSpecularMotionVectorScale`을 적용한 specular motion vector를 쓴다.

관련 command-line toggle:

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

Path tracing RR은 full display resolution을 render resolution으로 사용하고 DLSS jitter phase count를 `1`로 설정한다.

## UI와 Scripting

C++ ImGui panel은 path tracing 모드일 때만 다음 항목을 보여준다.

- Max Bounces, `1..8`
- Samples Per Pixel, `1..16`
- Primary-hit GBuffer for DLSS RR
- Stabilize moving primary rays for RR
- Accumulated Frames
- Dispatch SPP
- Reset Accumulation
- Debug Visualization

User-mode Luau UI에 노출되는 값:

- `path_max_bounces`
- `path_samples_per_pixel`
- `path_direct_light_samples`
- `path_debug_mode`
- `reset_accumulation` command

Shader에는 다음 debug branch가 있다.

- `0`: none
- `1`: albedo
- `2`: normal
- `3`: roughness
- `4`: metallic
- `5`: world position
- `6`: barycentric
- `7`: UV
- `8`: instance ID
- `9`: triangle index

현재 C++/Luau UI는 `0..6`만 노출한다.

## Command-Line 제어

현재 path tracing 관련 command-line option:

```text
--render-mode pt
--render-mode pathtracing
--render-mode path-tracing
--render-mode path_tracing

--aa off
--aa dlss-rr

--pt-spp <1..16>
--spp <1..16>

--pt-dlss-rr
--no-pt-dlss-rr
--pt-rr-specular-mv
--no-pt-rr-specular-mv
--pt-rr-specular-hit-distance
--no-pt-rr-specular-hit-distance
--pt-rr-stable-primary-rays
--no-pt-rr-stable-primary-rays
--pt-rr-specular-mv-scale <-2.0..2.0>

--path-tracing-dump
--pathtracing-dump
--pt-dump
```

Max bounces와 direct sun sample count는 현재 dedicated command-line option이 아니라 interactive/script control로 조정한다.

## Output Resolve

`GetCurrentResolveSource()`는 path tracing 모드에서 다음 순서로 resolve source를 선택한다.

1. path tracing DLSS RR이 이번 frame에 성공했고 `DLSSRRBuffer`가 유효하면 `DLSSRRBuffer`.
2. 그렇지 않으면 `PathTracingAccumBuffer[PathTracingWriteIndex]`.

이 fallback 덕분에 Streamline RR을 사용할 수 없거나 RR 평가가 실패해도 path tracing 출력은 계속 표시된다.

## 현재 한계와 메모

- 이 path tracer는 실용적인 reference path이며 full PBRT 스타일 renderer는 아니다.
- direct light sampling과 BSDF sampling 사이의 MIS는 아직 없다.
- 태양광은 spherical cap을 최대 8 sample까지 평균낸다.
- diffuse/specular bounce 선택은 Fresnel 기반 확률과 throughput 보정을 사용하지만, 의도적으로 단순한 모델이다.
- hybrid temporal denoiser는 path tracing pass에 적용하지 않는다.
- accumulation buffer는 두 개지만 현재 `PathTracingWriteIndex`를 매 frame rotate하지 않는다.
- Path tracing DLSS RR metadata는 primary-hit 기반이다. 누적 PT color를 RR이 재구성하는 데 필요한 화면 공간 정보를 주기 위한 것이며, 완전한 path-space motion solution은 아니다.
- shader debug mode `7..9`는 존재하지만 현재 UI에는 노출되지 않는다.

## 빠른 검증

권장 smoke test:

```powershell
.\Corona.exe --backend dx12 --render-mode pathtracing --aa off --pt-spp 1 --no-imgui
.\Corona.exe --backend dx12 --render-mode pathtracing --aa dlss-rr --pt-dlss-rr --pt-spp 1
```

확인할 항목:

- GPU timing에 `PathTracing` pass가 기록된다.
- camera/light/sky가 정지해 있으면 accumulated frame count가 증가한다.
- camera를 움직이면 accumulation이 reset된다.
- max bounces 또는 SPP를 바꾸면 accumulation이 reset된다.
- debug mode는 누적되지 않는 diagnostic view를 출력한다.
- DLSS RR 활성 시 `PathTracing` 이후 `DLSSRR`이 실행되고, 성공하면 final resolve가 `DLSSRRBuffer`를 사용한다.
