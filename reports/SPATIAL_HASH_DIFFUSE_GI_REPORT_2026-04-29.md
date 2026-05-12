# SHaRC 스타일 Spatial Hash Diffuse GI 구현 리포트

작성일: 2026-04-29

## 요약

현재 구현은 화면에 보이는 primary hit 표면을 world-space hash cell로 묶고, cell 단위로 diffuse GI ray를 쏘는 sparse radiance cache이다. 이전 단계에서 per-pixel raw diffuse GI를 만든 뒤 평균내는 방식이 아니라, hash table에 등록된 cell entry가 직접 DXR raygen의 작업 단위가 된다.

캐시에는 단순 radiance `float3`가 아니라 RGB SH 4계수(L0 + L1)를 저장한다. 최종 query 단계에서 픽셀 normal로 SH를 평가하므로, 같은 cell 근처라도 표면 방향이 다른 픽셀은 다른 간접광 값을 받을 수 있다.

관련 파일:

- `src/Shaders/SpatialHashDiffuseGI.hlsl`
- `src/Shaders/SpatialHashCellGI.hlsl`
- `src/Corona.h`
- `src/Corona.cpp`
- `src/RTPipelineStateObject.h`
- `src/SimpleDX12.cpp`
- `src/VulkanBackend.cpp`

## 파이프라인

Spatial Hash GI 모드의 현재 흐름은 다음과 같다.

1. `SpatialHashUpdate`
   depth/normal buffer의 primary hit를 읽고, visible surface를 hash cell로 등록한다. 이 단계는 ray를 쏘지 않고 `UpdateKeys`, `CellPosition`, `CellNormal`만 채운다.

2. `SpatialHashCellGI.hlsl` raygen
   hash table의 entry 하나가 cell 하나의 ray 작업 단위가 된다. key가 비어 있는 slot은 즉시 종료하고, active slot만 실제 diffuse ray를 추적한다. `RaysPerCell`과 `MaxBounces`로 cell당 sample 수와 multi-bounce 깊이를 조절한다.

3. `SpatialHashResolve`
   이번 프레임의 traced SH를 이전 프레임의 resolved SH와 temporal blend한다. history frame count는 `ResolvedSH0.w`에 저장한다.

4. `SpatialHashQuery`
   픽셀별 world position/normal로 hash table을 조회한다. 주변 8개 cell을 trilinear 방식으로 보간하고, 보간된 SH를 픽셀 normal로 평가해서 `DiffuseGIHashCached`를 만든다.

5. 기존 denoiser
   temporal/spatial diffuse denoiser는 raw GI 대신 `DiffuseGIHashCached`를 입력으로 사용한다.

## Hash Key 알고리즘

hash key는 위치 cell과 normal bin을 함께 사용한다. 위치만 쓰면 벽의 앞뒤, 코너, 얇은 표면에서 서로 다른 방향의 irradiance가 같은 entry에 섞이기 쉽기 때문이다.

절차:

1. `worldPos / CellSize`를 `floor`해서 `int3 cell`을 만든다.
2. normal을 `[-1, 1]`에서 `[0, 7]`로 양자화한다.
3. cell 좌표와 normal bits를 prime multiplication + xor로 섞는다.
4. `HashUInt`로 한 번 더 avalanche한다.
5. key `0`은 empty sentinel이므로 `0`이 나오면 `1`로 바꾼다.

개념 코드:

```cpp
cell = floor(worldPos / CellSize)
normalBin = quantize(normal, 3 bits per axis)

key = HashUInt(
    cell.x * 73856093 ^
    cell.y * 19349663 ^
    cell.z * 83492791 ^
    normalBits * 2654435761)

if (key == 0)
    key = 1
```

slot은 `HashUInt(key ^ 0x9e3779b9) & HashEntryMask`로 시작하고, 충돌 시 linear probing을 수행한다. probing 길이는 `MaxProbeSteps`로 제한한다.

## 자료 구조

현재 table 크기는 `SpatialHashGIEntryCount = 1 << 20`이다. 모든 resource는 dense buffer로 할당되지만, 의미상으로는 key가 들어간 slot만 active인 sparse hash table이다.

| 리소스 | 타입 | 용도 |
| --- | --- | --- |
| `SpatialHashGIUpdateKeys` | `StructuredBuffer<uint>` | 이번 프레임 visible cell key |
| `SpatialHashGICellPosition` | `StructuredBuffer<float4>` | cell 대표 surface position, `w`는 valid flag |
| `SpatialHashGICellNormal` | `StructuredBuffer<float4>` | cell 대표 normal, `w`는 valid flag |
| `SpatialHashGITraceSH[4]` | `StructuredBuffer<float4>` | cell ray pass가 쓴 이번 프레임 RGB SH 계수 |
| `SpatialHashGIResolvedKeys[2]` | `StructuredBuffer<uint>` | temporal ping-pong key table |
| `SpatialHashGIResolvedSH[2][4]` | `StructuredBuffer<float4>` | temporal ping-pong SH table |
| `DiffuseGIHashCached` | `Texture2D<float4>` | 픽셀별 SH evaluation 결과 |
| `DiffuseGIHashCachedAux` | `Texture2D<float4>` | debug/aux 출력, 현재는 SH0 + history |

`ResolvedSH0.xyz`는 SH coefficient 0이고, `ResolvedSH0.w`는 history frame count이다. `ResolvedSH1..3.xyz`는 L1 coefficient이다.

RT raygen이 structured buffer UAV에 직접 쓰도록 `RTPipelineStateObject`에 `SetBufferUAV`가 추가되었다. DX12는 buffer의 `GpuHandleUAV`를 global root binding에 넣고, Vulkan backend도 storage buffer UAV binding을 받을 수 있게 확장했다.

## Cell Ray Tracing

`SpatialHashCellGI.hlsl`의 raygen은 `DispatchRaysIndex().x`를 hash slot으로 사용한다.

비어 있는 slot은 key가 0이므로 ray를 쏘지 않는다. active slot만 다음 작업을 수행한다.

1. `CellPosition`, `CellNormal`에서 대표 surface를 읽는다.
2. normal 기준 uniform hemisphere 방향을 고른다.
3. 그 방향으로 path를 추적한다.
4. miss면 sky radiance를 더한다.
5. hit면 해당 hit surface의 direct diffuse lighting을 shadow ray로 평가한다.
6. `MaxBounces`가 남아 있으면 hit albedo를 throughput에 곱하고, cosine hemisphere 방향으로 다음 bounce를 이어간다.
7. 첫 sample direction 기준으로 incoming radiance를 SH에 project한다.

cell당 실제 ray 수는 대략 다음과 같다.

```text
active_cell_count * RaysPerCell * MaxBounces
```

shadow ray까지 포함하면 direct lighting hit마다 shadow ray가 하나 더 들어간다. 그래도 per-pixel diffuse GI보다 훨씬 적은 primary diffuse ray 수로 간접광 field를 갱신할 수 있다.

## SH 저장과 Normal 평가

저장하는 SH는 RGB L0/L1 4계수이다.

- `c0`: L0
- `c1`: L1 y
- `c2`: L1 z
- `c3`: L1 x

cell ray pass는 incoming radiance를 sample direction으로 project한다. query pass는 픽셀 normal `N`으로 Lambertian convolution된 값을 평가한다.

개념식:

```cpp
irradiance =
    PI * c0 * Y00(N) +
    2PI/3 * (c1 * Y1y(N) + c2 * Y1z(N) + c3 * Y1x(N))
```

이 결과가 `DiffuseGIHashCached.xyz`로 나가고, lighting pass에서 기존 방식처럼 receiver albedo와 곱해진다.

## 보간

query는 현재 픽셀의 `worldPos / CellSize`에서 fractional 위치를 구하고, 주변 8개 cell을 조회한다. 각 cell 조회는 동일한 normal quantization을 사용한다.

보간 절차:

1. base cell과 fractional coordinate를 구한다.
2. 8-neighbor cell을 hash lookup한다.
3. valid cell만 trilinear weight로 SH 계수를 누적한다.
4. valid weight로 normalize한다.
5. `InterpolationStrength`로 base cell 값과 보간값 사이를 섞는다.
6. 보간된 SH를 pixel normal로 평가한다.

이 방식은 hash table 자체는 sparse하게 유지하면서도 화면에 보이는 grid 경계를 줄인다.

## 한계와 다음 개선점

현재 구현은 active cell list compaction을 하지 않는다. `DispatchRays`는 전체 hash table width로 실행되지만, empty key slot은 ray를 쏘기 전에 return한다. 즉 실제 `TraceRay` 수는 active cell 수에 비례하지만, raygen thread dispatch overhead는 table 크기에 남아 있다.

cell 대표 position/normal은 첫 등록 픽셀 기준이다. 같은 cell 안에서 더 좋은 대표점을 고르려면 depth/normal 안정성 점수나 atomic min/max 기반 priority를 추가할 수 있다.

현재 SH는 L0/L1 4계수이다. 더 방향성이 강한 간접광을 표현하려면 screen probe 쪽처럼 L2까지 포함한 9계수 SH로 확장할 수 있다.

multi-bounce는 path throughput 기반으로 동작하지만, cache 간 재사용으로 bounce를 전파하는 방식은 아직 아니다. 이후에는 priority queue나 active cell budget을 두고, 변화가 큰 cell부터 더 깊은 bounce를 갱신하는 구조로 발전시킬 수 있다.
