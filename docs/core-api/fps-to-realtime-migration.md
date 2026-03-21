# FPS에서 Realtime으로 마이그레이션

## 요약
- 변경 내용: canonical stable public surface가 `server/core/fps/**`에서 `server/core/realtime/**`로 이동했습니다.
- 영향받는 API: `direct_bind`, `direct_delivery`, `transport_quality`, `transport_policy`, `runtime`
- 현재 상태: `server/core/realtime/**`만 지원되며 `server/core/fps/**` wrapper path는 3.0에서 제거되었습니다.

## 배경
- 해결하려는 문제: cross-genre engine platform roadmap에서 장르명을 드러내는 public core surface를 제거하고, 더 넓은 realtime capability naming을 canonical contract로 고정해야 했습니다.
- 기존 API가 부족했던 이유: `fps` naming은 fixed-step replication/transport substrate의 용도보다 더 좁은 장르 의미를 전달했고, 향후 3.0 public surface framing과 맞지 않았습니다.

## 파괴적 변경 범위
- 제거/이름 변경된 심볼: `server/core/fps/{direct_bind,direct_delivery,transport_quality,transport_policy,runtime}.hpp`
- 시그니처 변경: 없습니다.
- 동작 변경: canonical include path/namespace는 `realtime`만 지원하며, 기존 `fps` 경로는 더 이상 설치/빌드되지 않습니다.

## 마이그레이션 절차
1. include 갱신: `server/core/fps/*.hpp`를 `server/core/realtime/*.hpp`로 바꿉니다.
2. 호출부 갱신: `server::core::fps`를 `server::core::realtime`로 바꿉니다.
3. 동작 검증: public-api smoke와 installed consumer를 다시 실행합니다.

## 변경 전
```cpp
#include "server/core/realtime/runtime.hpp"

server::core::realtime::WorldRuntime runtime;
```

## 변경 후
```cpp
#include "server/core/realtime/runtime.hpp"

server::core::realtime::WorldRuntime runtime;
```

## 검증
- 빌드/테스트 근거:
  - `core_public_api_realtime_capability_smoke`
  - `CorePublicApiWorldsAwsSmoke`
  - `CoreInstalledPackageConsumer`
- 런타임/메트릭 검증:
  - `tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance`

## 제거 공지

- Removed API: `server/core/fps/{direct_bind,direct_delivery,transport_quality,transport_policy,runtime}.hpp`
- Replacement: `server/core/realtime/{direct_bind,direct_delivery,transport_quality,transport_policy,runtime}.hpp`
- Deprecated in: `2.18.0`
- Removed in: `3.0.0`
- Migration note: `docs/core-api/fps-to-realtime-migration.md`
