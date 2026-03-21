# MMORPG에서 Worlds로 마이그레이션

## 요약
- 변경 내용: canonical stable public surface가 `server/core/mmorpg/**`에서 `server/core/worlds/**`로 이동했습니다.
- 영향받는 API: `migration`, `topology`, `world_drain`, `world_transfer`
- 현재 상태: `server/core/worlds/**`만 지원되며 `server/core/mmorpg/**` wrapper path는 3.0에서 제거되었습니다.

## 배경
- cross-genre engine platform roadmap에서 장르명이 드러나는 public orchestration surface를 제거하고, world lifecycle/orchestration naming을 canonical contract로 고정해야 했습니다.

## 파괴적 변경 범위
- 제거/이름 변경된 심볼: `server/core/mmorpg/{migration,topology,world_drain,world_transfer}.hpp`
- 시그니처 변경: 없습니다.
- 동작 변경: canonical include path/namespace는 `worlds`만 지원하며, 기존 `mmorpg` 경로는 더 이상 설치/빌드되지 않습니다.

## 마이그레이션 절차
1. include 갱신: `server/core/mmorpg/*.hpp`를 `server/core/worlds/*.hpp`로 바꿉니다.
2. 호출부 갱신: `server::core::mmorpg`를 `server::core::worlds`로 바꿉니다.
3. 동작 검증: public-api smoke, installed consumer, topology/runtime acceptance proof를 다시 실행합니다.

## 변경 전
```cpp
#include "server/core/mmorpg/topology.hpp"

server::core::mmorpg::DesiredTopologyDocument topology;
```

## 변경 후
```cpp
#include "server/core/worlds/topology.hpp"

server::core::worlds::DesiredTopologyDocument topology;
```

## 검증
- 빌드/테스트 근거:
  - `core_public_api_smoke`
  - `core_public_api_stable_header_scenarios`
  - `CoreInstalledPackageConsumer`
- orchestration/runtime 검증:
  - `tests/python/verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance`
  - `ctest --test-dir build-windows -C Release -R "KubernetesWorlds" --output-on-failure`

## 제거 공지

- Removed API: `server/core/mmorpg/{migration,topology,world_drain,world_transfer}.hpp`
- Replacement: `server/core/worlds/{migration,topology,world_drain,world_transfer}.hpp`
- Deprecated in: `2.23.0`
- Removed in: `3.0.0`
- Migration note: `docs/core-api/mmorpg-to-worlds-migration.md`
