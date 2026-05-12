# Screen Probe GI SH History And Fallback Report

작성일: 2026-04-29

이 문서는 screen-probe diffuse GI의 SH 누적, 라이트 변경 bootstrap, 카메라 이동 시 새로 시야에 들어오는 probe fallback 구현을 소스 라인 링크와 함께 정리한다. 라인 링크는 이 리포트 작성 시점의 현재 worktree 기준이다.

## 요약

현재 screen-probe GI는 각 probe가 직접 ray를 쏴서 radiance SH3 계수를 갱신하고, 정상적인 경우에는 velocity 기반 temporal reprojection으로 이전 atlas의 SH를 이어받는다.

카메라 이동으로 이전 UV가 화면 밖으로 나가면 기존 reprojection은 실패한다. 이 경우 새 probe가 1-frame noisy SH에서 시작하지 않도록, 직전 프레임 화면 경계에서 안쪽 방향으로 corridor search를 수행하고, 이미 converge 된 인접 probe 하나를 seed로 가져온다.

라이트가 바뀌는 경우에는 이전 라이트의 SH를 그대로 섞지 않는다. 대신 current light 기준으로 bootstrap을 수행한다. 태양광 그림자 영역은 검은 SH에서 시작하고, 태양에 노출된 probe는 100 rays로 초기 SH를 만든 뒤 다시 일반 누적 경로로 돌아간다.

## 핵심 소스 링크

| 영역 | 코드 위치 | 설명 |
| --- | --- | --- |
| RT probe cbuffer | [src/Shaders/ScreenProbeRaytracedGI.hlsl#L70](src/Shaders/ScreenProbeRaytracedGI.hlsl#L70) | `LightingBootstrap`, `BootstrapRays` shader parameter |
| 인접 fallback 상수 | [src/Shaders/ScreenProbeRaytracedGI.hlsl#L79](src/Shaders/ScreenProbeRaytracedGI.hlsl#L79) | fallback search radius, inward steps, history threshold, seed frame cap |
| RT probe ray tracing | [src/Shaders/ScreenProbeRaytracedGI.hlsl#L390](src/Shaders/ScreenProbeRaytracedGI.hlsl#L390) | probe sample ray가 diffuse incident radiance를 계산 |
| Probe temporal reprojection | [src/Shaders/ScreenProbeRaytracedGI.hlsl#L458](src/Shaders/ScreenProbeRaytracedGI.hlsl#L458) | 이전 probe atlas SH/radiance를 depth/normal/velocity로 검증 후 누적 |
| Camera-boundary fallback | [src/Shaders/ScreenProbeRaytracedGI.hlsl#L553](src/Shaders/ScreenProbeRaytracedGI.hlsl#L553) | 화면 밖 reprojection 실패 시 인접 converge probe를 seed로 사용 |
| Fallback inward corridor | [src/Shaders/ScreenProbeRaytracedGI.hlsl#L572](src/Shaders/ScreenProbeRaytracedGI.hlsl#L572) | 벗어난 픽셀 거리만큼 화면 경계에서 안쪽으로 probe 후보 검색 |
| Sun visibility bootstrap | [src/Shaders/ScreenProbeRaytracedGI.hlsl#L642](src/Shaders/ScreenProbeRaytracedGI.hlsl#L642) | 태양광 직접 노출 여부를 shadow ray로 판정 |
| Raygen main flow | [src/Shaders/ScreenProbeRaytracedGI.hlsl#L673](src/Shaders/ScreenProbeRaytracedGI.hlsl#L673) | bootstrap, ray sampling, temporal history, fallback 연결 |
| CPU RT view param | [src/Corona.h#L340](src/Corona.h#L340) | `RTScreenProbeGIViewParamCB` 정의 |
| CPU bootstrap state | [src/Corona.h#L609](src/Corona.h#L609) | screen-probe atlas/history validity 및 lighting bootstrap pending flag |
| Light-change detection | [src/Corona.cpp#L5726](src/Corona.cpp#L5726) | 라이트 변경 시 100-ray bootstrap 준비 |
| CPU shader param update | [src/Corona.cpp#L5810](src/Corona.cpp#L5810) | bootstrap 중 atlas history 사용 차단 및 shader flag 전달 |
| RT screen-probe pass | [src/Corona.cpp#L7946](src/Corona.cpp#L7946) | atlas write/read ping-pong 및 RT shader resource binding |
| Resolve history gating | [src/Corona.cpp#L8142](src/Corona.cpp#L8142) | bootstrap frame에서 screen-space resolve history도 차단 |
| Bootstrap clear | [src/Corona.cpp#L8152](src/Corona.cpp#L8152) | resolve pass 이후 bootstrap pending 해제 |
| SH resolve | [src/Shaders/ScreenProbeGI.hlsl#L193](src/Shaders/ScreenProbeGI.hlsl#L193) | SH3 계수를 diffuse irradiance로 평가 |
| Edge-aware gather | [src/Shaders/ScreenProbeGI.hlsl#L118](src/Shaders/ScreenProbeGI.hlsl#L118) | probe gather 시 depth/normal edge crossing 억제 |
| Resolve temporal history | [src/Shaders/ScreenProbeGI.hlsl#L309](src/Shaders/ScreenProbeGI.hlsl#L309) | resolved diffuse GI의 screen-space temporal accumulation |

## 동작 흐름

1. Probe ray tracing pass가 probe atlas를 ping-pong으로 갱신한다.

`ScreenProbeRaytraceGIPass()`는 write/read atlas index를 교체하고, 현재 atlas를 UAV로, 이전 atlas를 SRV로 바인딩한다. 이 pass가 RT shader의 `rayGen()`을 probe grid 크기만큼 dispatch한다. 관련 코드: [src/Corona.cpp#L7946](src/Corona.cpp#L7946), [src/Corona.cpp#L8004](src/Corona.cpp#L8004).

2. 각 probe는 ray sampling으로 current-frame SH를 만든다.

`TraceDiffuseProbeRay()`는 uniform hemisphere sample 방향으로 ray를 쏘고, hit 지점에서 태양광 shadow ray를 추가로 쏴서 incident diffuse radiance를 계산한다. 이후 `rayGen()`은 sample radiance를 SH3 계수로 project하고 평균낸다. 관련 코드: [src/Shaders/ScreenProbeRaytracedGI.hlsl#L390](src/Shaders/ScreenProbeRaytracedGI.hlsl#L390), [src/Shaders/ScreenProbeRaytracedGI.hlsl#L702](src/Shaders/ScreenProbeRaytracedGI.hlsl#L702).

3. 정상 reprojection이 가능하면 이전 SH를 누적한다.

`ApplyProbeTemporalHistory()`는 velocity로 이전 UV를 찾고, 이전 probe lattice를 bilinear로 샘플링한다. depth/normal compatibility가 충분한 경우에만 이전 radiance/SH를 current sample과 누적한다. 이 함수가 `true`를 반환하면 fallback은 실행되지 않는다. 관련 코드: [src/Shaders/ScreenProbeRaytracedGI.hlsl#L458](src/Shaders/ScreenProbeRaytracedGI.hlsl#L458), [src/Shaders/ScreenProbeRaytracedGI.hlsl#L719](src/Shaders/ScreenProbeRaytracedGI.hlsl#L719).

4. 화면 밖 reprojection은 인접 converge probe로 seed한다.

카메라 이동으로 `prevUV`가 화면 밖이면 `ApplyAdjacentAtlasFallback()`이 실행된다. 이 함수는 `prevUV`를 화면 경계로 clamp하고, 벗어난 픽셀 거리를 probe spacing으로 나눠 inward corridor 길이를 정한다. 그런 다음 화면 안쪽 방향으로 최대 12 probe step까지 들어가며, 각 step 주변 5x5 후보 중 history가 12 frames 이상이고 normal이 가장 잘 맞는 probe 하나를 고른다. 관련 코드: [src/Shaders/ScreenProbeRaytracedGI.hlsl#L553](src/Shaders/ScreenProbeRaytracedGI.hlsl#L553), [src/Shaders/ScreenProbeRaytracedGI.hlsl#L572](src/Shaders/ScreenProbeRaytracedGI.hlsl#L572), [src/Shaders/ScreenProbeRaytracedGI.hlsl#L579](src/Shaders/ScreenProbeRaytracedGI.hlsl#L579).

5. Fallback seed는 오래 끌고 가지 않는다.

선택된 인접 probe의 radiance/SH를 그대로 가져오지만, history confidence는 최대 8 frames로 제한한다. 따라서 새 영역이 완전히 noisy하게 시작하는 문제는 줄이되, 잘못 가져온 seed가 장시간 ghosting으로 남지 않도록 했다. 관련 코드: [src/Shaders/ScreenProbeRaytracedGI.hlsl#L632](src/Shaders/ScreenProbeRaytracedGI.hlsl#L632).

6. 라이트 변경 시에는 current-light bootstrap을 사용한다.

CPU는 indirect light direction/intensity 또는 sky가 바뀌면 `bScreenProbeLightingBootstrapPending`을 설정한다. 이 frame에서는 이전 atlas history와 screen-space resolve history를 둘 다 사용하지 않도록 차단한다. shader는 `LightingBootstrap`이 켜진 경우, 태양광 shadow 여부를 검사한다. 그림자 probe는 검은 SH에서 시작하고, 태양에 노출된 probe는 `BootstrapRays = 100`으로 더 많은 ray를 쏴서 초기 SH를 만든다. 관련 코드: [src/Corona.cpp#L5726](src/Corona.cpp#L5726), [src/Corona.cpp#L5810](src/Corona.cpp#L5810), [src/Shaders/ScreenProbeRaytracedGI.hlsl#L642](src/Shaders/ScreenProbeRaytracedGI.hlsl#L642), [src/Shaders/ScreenProbeRaytracedGI.hlsl#L692](src/Shaders/ScreenProbeRaytracedGI.hlsl#L692).

## 주요 파라미터

| 파라미터 | 값 | 의미 |
| --- | --- | --- |
| `BootstrapRays` | 100 | 라이트 변경 bootstrap에서 태양 노출 probe가 한 번에 쏘는 ray 수 |
| `ADJACENT_ATLAS_FALLBACK_LATERAL_RADIUS` | 2 | inward corridor 각 step에서 좌우/상하 5x5 후보 검색 |
| `ADJACENT_ATLAS_FALLBACK_MAX_INWARD_STEPS` | 12 | 화면 경계에서 안쪽으로 최대 12 probe step 검색 |
| `ADJACENT_ATLAS_FALLBACK_MIN_HISTORY_FRAMES` | 12 | fallback 후보로 인정할 최소 누적 frame 수 |
| `ADJACENT_ATLAS_FALLBACK_SEED_FRAMES` | 8 | 가져온 probe seed가 가질 수 있는 history frame 상한 |

기본 probe spacing이 8이면 inward fallback은 화면 경계에서 약 96px 안쪽까지 후보를 볼 수 있다. 따라서 수십 픽셀 정도의 카메라 이동으로 새로 드러난 영역은, 직전 프레임 시야 안에 있던 converge probe를 가져올 가능성이 높다.

## 기대 효과와 한계

이 방식은 평균 SH fallback보다 방향성과 지역성을 더 잘 보존한다. 새로 들어온 화면 영역이 전체 평균색으로 물드는 대신, 직전 프레임 화면 경계 근처에서 실제로 보였던 converge probe를 이어받기 때문이다.

한계도 있다. 화면 밖에서 새로 드러난 표면이 직전 화면 경계의 표면과 실제로 다른 공간인데 normal이 우연히 비슷하면, 짧은 시간 동안 잘못된 GI seed가 보일 수 있다. 이를 줄이기 위해 history threshold를 12 frames로 두고, seed confidence를 8 frames로 제한했으며, 이후 frame부터는 실제 ray-sampled SH가 다시 누적되도록 했다.

## 검증

문서 작성 직전 `cmake --build --preset release`를 실행했고 빌드는 통과했다. Vulkan shader 변환도 함께 통과했으며, 기존 shader conversion warning만 출력되었다. 자동 dump는 실행하지 않았다.
