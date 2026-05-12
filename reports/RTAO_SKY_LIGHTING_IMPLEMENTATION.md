# RTAO 및 Sky Lighting 구현 정리

작성일: 2026-05-02

## 요약

하이브리드 렌더러의 음영 차이를 줄이기 위해 diffuse GI에 섞여 있던 역할을 다음처럼 분리했다.

- RTAO는 짧은/중거리 접촉 차폐를 담당한다.
- Sky Lighting은 하늘에서 오는 직접 diffuse 조명을 별도 ray tracing 패스로 담당한다.
- Diffuse GI는 sky miss 에너지를 더하지 않고 표면 bounce 에너지 위주로 유지한다.
- 최종 lighting pass는 직접광, surface-bounce GI, sky diffuse, specular GI를 분리해서 합성한다.

현재 직접광 그림자에는 hidden brightness floor가 없다. `LightingPS.hlsl`에서 직접광 visibility는 `DirectVisibility = Shadow` 그대로 사용한다.

## 주요 파일

| 영역 | 파일 |
| --- | --- |
| RTAO 패스 | `src/Corona.RayTracingAO.cpp` |
| RTAO 셰이더 | `src/Shaders/RaytracedAO.hlsl` |
| Sky lighting RT 패스 | `src/Corona.RayTracingSkyLighting.cpp` |
| Sky lighting RT 셰이더 | `src/Shaders/RaytracedSkyLighting.hlsl` |
| Sky lighting denoise | `src/Corona.DenoisingPasses.cpp`, `src/Shaders/SkyLightingDenoise.hlsl` |
| Lighting 합성 | `src/Corona.RasterPasses.cpp`, `src/Shaders/LightingPS.hlsl` |
| RT scene/pass builder | `src/Corona.RayTracingPassBuilder.cpp`, `src/Corona.h` |
| Diffuse GI sky 분리 | `src/Shaders/RaytracedGI.hlsl`, `src/Shaders/ScreenProbeRaytracedGI.hlsl`, `src/Shaders/SpatialHashCellGI.hlsl` |

## 하이브리드 프레임 내 실행 순서

하이브리드 모드의 stage 7 lighting 단계 기준 흐름은 다음과 같다.

1. GBuffer를 생성한다.
2. Direct shadow ray를 쏘고 shadow denoise를 수행한다.
3. Lighting stage에서 RTAO가 켜져 있으면 `RaytraceAOPass()`를 수행한다.
4. Lighting stage에서 Sky Lighting이 켜져 있으면 `RaytraceSkyLightingPass()`를 수행하고 필요하면 sky denoise를 수행한다.
5. Reflection, diffuse GI, temporal/spatial denoise를 수행한다.
6. `LightingPass()`에서 직접광, surface bounce, sky diffuse, specular를 합성한다.

RTAO와 Sky Lighting은 full-resolution GBuffer depth/normal을 기준으로 화면 픽셀마다 ray를 쏜다.

## RTAO

### 목적

RTAO는 GI 해상도 부족으로 놓치기 쉬운 부다 다리 주변, pedestal 접촉부, 작은 틈 같은 고주파 contact shadow를 보강하기 위한 패스다. 넓은 원거리 음영은 diffuse GI와 sky visibility가 이미 담당하므로 RTAO는 최종 조명 전체를 세게 누르는 용도가 아니다.

### 입력과 출력

입력:

- TLAS: `gRtScene`
- `DepthTex`: unjittered depth
- `WorldNormalTex`: shading/world normal
- `GeoNormalTex`: geometry normal
- `BlueNoiseTex`: ray sequence noise
- hit/anyhit용 mesh vertex/index, albedo, instance property

출력:

- `AmbientOcclusionBuffer`
- 형식은 RGB에 같은 AO 값을 저장하고 alpha는 1이다.
- sky/background 픽셀은 AO 1로 처리한다.

### Ray generation

`RaytracedAO.hlsl`의 raygen은 depth에서 world position을 복원하고, geometry normal과 shading normal을 카메라 방향에 맞춰 보정한다. 이후 `geoNormal + worldNormal * 0.25`를 normalize한 `traceNormal`을 기준으로 cosine hemisphere ray를 샘플링한다.

현재 기본값:

| 파라미터 | 기본값 | 의미 |
| --- | ---: | --- |
| `Radius` | 96.0 | AO ray 최대 거리 |
| `SampleCount` | 16 | 픽셀당 ray 수 |
| `Power` | 1.10 | AO contrast 조절 |
| `NormalBias` | 0.35 | self-intersection 방지 bias |

ray hit이 있으면 `HitT / Radius`로 거리 기반 visibility를 만들고, 가까운 hit이 더 강하게 작동하도록 `(1 - hitVisibility)^2`를 occlusion에 더한다.

최종 AO:

```hlsl
ao = 1.0 - occluded / sampleCount;
ao = pow(saturate(ao), Power);
```

### Alpha-tested geometry

RTAO anyhit은 alpha-tested instance에서 albedo alpha를 읽는다. opacity가 0.10 이하이면 `IgnoreHit()`으로 통과시켜 잎, 천, cutout 재질이 AO를 과하게 막지 않게 한다.

### Lighting 적용

RTAO는 직접광 그림자에는 곱하지 않는다. `LightingPS.hlsl`에서 다음 contact term으로 surface-bounce diffuse GI에만 섞인다.

```hlsl
AmbientOcclusion = AmbientOcclusionTex.x;
ContactAO = lerp(1.0, max(AmbientOcclusion, RTAOIndirectFloor), RTAOIndirectStrength);
SurfaceBounceDiffuse = SurfaceBounceGI * Albedo * ContactAO * SurfaceBounceStrength;
```

현재 기본값:

| 파라미터 | 기본값 | 의미 |
| --- | ---: | --- |
| `RTAOIndirectStrength` | 0.25 | GI에 AO를 섞는 강도 |
| `RTAOIndirectFloor` | 0.55 | GI가 완전히 검게 죽지 않도록 하는 하한 |

즉 RTAO는 접촉부에 질감을 주되, 하이브리드 전체가 path tracer보다 평면적으로 어두워지거나 직접광 shadow가 이중으로 어두워지는 것을 피한다.

## Sky Lighting

### 목적

기존 diffuse GI는 surface bounce와 sky miss를 함께 담고 있어서, sky occlusion과 surface GI의 역할이 섞였다. 이 때문에 sky가 막힌 부분을 path tracer처럼 어둡게 만들기 어렵고, sky를 켜면 간접광 전체가 과하게 밝아질 수 있었다.

Sky Lighting 패스는 하늘에서 직접 들어오는 diffuse irradiance와 그 visibility를 별도 ray로 평가한다.

### 입력과 출력

입력:

- TLAS: `gRtScene`
- `DepthTex`
- `WorldNormalTex`
- `GeoNormalTex`
- `BlueNoiseTex`
- alpha-tested anyhit용 mesh data
- sky gradient: `SkyColorTop`, `SkyColorBottom`, `SkyIntensity`

출력:

- `SkyLightingRawBuffer`: RT raw 결과
- `SkyLightingBuffer`: denoise 후 lighting pass 입력
- `xyz`: sky diffuse radiance
- `w`: sky visibility

### Ray 방향 제한

Sky ray는 단순히 surface normal hemisphere 전체를 균일하게 뒤지는 대신, 더 빨리 converge하도록 world-up 방향으로 guide한다.

```hlsl
guideNormal = normalize(lerp(traceNormal, worldUp, SkyUpBias));
```

현재 기본값:

| 파라미터 | 기본값 | 의미 |
| --- | ---: | --- |
| `RayLength` | 10000.0 | sky visibility ray 거리 |
| `SampleCount` | 32 | 픽셀당 sky ray 수 |
| `NormalBias` | 0.5 | self-intersection 방지 bias |
| `SkyUpBias` | 0.65 | world-up 쪽 sampling 유도 |
| `SkyDirectionPower` | 2.25 | up-guided 방향 집중도 |
| `SkyMinWorldY` | 0.02 | 너무 아래쪽 방향 reject |
| `SkyMaxSampleAttempts` | 4 | 유효 방향 재시도 횟수 |

방향 후보는 다음 조건을 만족해야 한다.

- `dot(candidateDir, traceNormal) > 0.001`
- `candidateDir.y >= SkyMinWorldY`

유효 방향을 못 찾으면 `traceNormal + worldUp * SkyUpBias` 또는 `traceNormal`로 fallback한다.

### Guided PDF 보정

ray 방향은 `guideNormal` 기준으로 몰아서 뽑지만, 실제 diffuse irradiance는 `traceNormal` 기준 cosine integral이다. 그래서 `ComputeDiffuseSkySampleWeight()`에서 guided sampling PDF를 보정한다.

핵심은 다음과 같다.

- sky 방향을 제한해 convergence를 빠르게 한다.
- 표면 normal 기준 diffuse cosine weight는 유지한다.
- PDF 보정 weight는 최대 4로 clamp해 firefly성 밝기 튐을 줄인다.

miss하면 sky gradient를 평가해 radiance를 누적하고, hit하면 occluded로 처리한다.

### Sky denoise

`SkyLightingDenoise.hlsl`은 raw sky 결과를 depth/normal/visibility bilateral 방식으로 공간 필터링한다.

현재 기본값:

| 파라미터 | 기본값 | 의미 |
| --- | ---: | --- |
| `Radius` | 5 | 필터 반경 |
| `DepthSigma` | 48.0 | depth edge 보존 |
| `NormalSigma` | 48.0 | normal edge 보존 |
| `VisibilitySigma` | 8.0 | visibility 차이 보존 |

alpha 채널의 visibility도 같이 필터링하지만, lighting 합성에서는 현재 `xyz` radiance를 사용한다.

### Lighting 적용

Sky diffuse는 surface-bounce GI와 분리된 항으로 합성된다.

```hlsl
SkyDiffuse = SkyLightingTex.xyz * Albedo * (1.0 - Metallic) * SkyLightingStrength;
IndirectDiffuse = SurfaceBounceDiffuse + SkyDiffuse;
```

현재 `SkyLightingStrength` 기본값은 0.35다.

## Diffuse GI와 sky 분리

Sky Lighting을 별도 패스로 추가한 뒤 diffuse GI에 남아 있던 sky miss 에너지는 중복 조명이 되므로 제거했다.

적용된 방향:

- `RaytracedGI.hlsl`: primary/surface miss에서 직접 sky diffuse를 더하지 않는다.
- `ScreenProbeRaytracedGI.hlsl`: probe GI는 surface/direct-light bounce 중심으로 유지한다.
- `SpatialHashCellGI.hlsl`: miss 시 environment radiance를 누적하지 않고 종료한다.

이 구조에서 diffuse GI는 실내/근접 표면 bounce를 담당하고, sky에서 직접 들어오는 diffuse는 Sky Lighting 패스가 담당한다.

## RT Scene/Pass Builder

RTAO와 Sky Lighting을 추가하면서 RT 패스마다 mesh binding이 조금씩 달라져 일부 mesh가 누락될 위험이 있었다. 이를 줄이기 위해 AS API 바로 위에 `RTPassBuilder`를 두었다.

`RTPassBuilder`가 담당하는 일:

- 매 패스 시작 시 `SetNumInstances(RayTracingInstances.size())`
- shader table 시작/종료
- global texture/buffer/AS/sampler/CBV binding
- scene hit program 반복 binding
- 각 instance의 hit program reset
- mesh vertex/index, diffuse texture, instance property의 공통 binding

RTAO와 Sky Lighting은 이제 같은 builder 경로로 `BindSceneHitPrograms()`를 호출하므로, 신규 RT 패스를 추가할 때 instance 수나 hit table binding 누락 가능성이 줄었다.

## Debug와 dump

### Visualize Buffers

`EDebugVisualization::RTAO`가 추가되어 fullscreen visualize와 tile visualize에서 RTAO buffer를 확인할 수 있다.

tile overlay 기준:

```text
Row1: SPEC_HISTORY_LENGTH | RTAO | SPATIAL_FILTERED_DIFFUSE_GI | FINAL_DIFFUSE_GI
```

### Auto dump

하이브리드 stage dump에는 lighting 단계에서 다음 파일들이 생성된다.

- `*_rtao_preview.png`
- `*_sky_lighting_raw_preview.png`
- `*_sky_lighting_preview.png`
- `*_direct_lighting_preview.png`, `*_direct_lighting.hdr`
- `*_lighting_preview.png`, `*_lighting.hdr`
- `*_resolve_preview.png`, `*_resolve.hdr`
- `*_screen_preview.png`

대표 경로:

```text
C:\dev\Corona\dumps\hybrid_pass_stages
```

lighting 비교 dump에서는 같은 종류의 버퍼를 비교용 phase별로 남긴다.

```text
C:\dev\Corona\dumps\lighting_compare
```

## CLI 및 UI

CLI:

```text
--rtao
--no-rtao
--rtao-radius <value>
--rtao-samples <value>
--rtao-power <value>
--rtao-bias <value>
--sky-lighting
--no-sky-lighting
--sky-lighting-strength <value>
--surface-bounce-strength <value>
--no-diffuse-gi
```

ImGui Lighting 패널:

- `Enable RTAO`
- `RTAO Samples`
- `RTAO Radius`
- `RTAO Power`
- `RTAO Normal Bias`
- `Enable Sky Lighting`
- `Sky Lighting Samples`
- `Sky Lighting Ray Length`
- `Sky Lighting Strength`
- `Sky Lighting Normal Bias`
- `Sky Lighting Up Bias`
- `Sky Lighting Direction Power`
- `Sky Lighting Min World Y`
- `Sky Lighting Denoise Radius`
- `Surface Bounce Strength`
- `Surface Bounce Saturation`

## 현재 합성 모델

최종 diffuse/specular 합성은 개념적으로 다음과 같다.

```text
DirectLighting = DirectDiffuse(Shadow) + DirectSpecular(Shadow)

SurfaceBounceDiffuse =
    DiffuseGI
    * Albedo
    * (1 - Metallic)
    * ContactAO
    * SurfaceBounceStrength

SkyDiffuse =
    SkyLighting
    * Albedo
    * (1 - Metallic)
    * SkyLightingStrength

IndirectDiffuse = SurfaceBounceDiffuse + SkyDiffuse
TotalSpecular = DirectSpecular + SpecularGI
FinalColor = DirectDiffuse + TotalSpecular + IndirectDiffuse
```

직접광 shadow floor는 제거되어 `ShadowTex` visibility가 직접광에 그대로 반영된다.

## 남은 관찰 포인트

- Sky Lighting은 현재 spatial denoise 위주다. 최종 화면에서는 TAA/DLSS RR이 temporal 안정화에 도움을 주지만, sky buffer 자체에 temporal accumulation은 없다.
- RTAO는 denoise 없이 multi-sample + noise sequence에 의존한다. 반경을 크게 잡으면 broad occlusion까지 건드릴 수 있으므로, 현재는 GI 합성 강도를 낮게 둔 상태다.
- Sky `w` visibility는 dump/denoise에는 보존하지만 최종 lighting에서는 `xyz` radiance 중심으로 사용한다. 후속으로 visibility-only debug나 specular sky occlusion에 활용할 수 있다.
