# `runtime_build_info` 마이그레이션 노트

## 요약
- 변경 내용: build info helper의 기본 Prometheus metric name을 `knights_build_info`에서 `runtime_build_info`로 변경했습니다.
- 영향받는 API: `server/core/build_info.hpp`, `server/core/metrics/build_info.hpp`
- 안정성 레벨: `Stable`

## 배경
- 해결하려는 문제: stable core API와 런타임 메트릭 표면에서 legacy 브랜드 토큰이 직접 노출되고 있었습니다.
- 기존 API가 부족했던 이유: 기본 metric name이 배포 브랜드와 결합되어 있어 리브랜딩이나 중립적 런타임 계약 유지가 어려웠습니다.

## 파괴적 변경 범위
- 제거/이름 변경된 심볼: 기본 metric series 이름 `knights_build_info` -> `runtime_build_info`
- 시그니처 변경: 없음 (`append_build_info(std::ostream&, std::string_view)` 시그니처는 유지)
- 동작 변경: `append_build_info(out)`를 기본 인자로 호출하면 이제 `runtime_build_info` 시리즈를 출력합니다.

## 마이그레이션 절차
1. include 갱신: 기존 include 경로는 유지되므로 변경하지 않습니다.
2. 호출부 갱신: 메트릭 수집기, 대시보드, 알람, 테스트에서 `knights_build_info`를 `runtime_build_info`로 바꿉니다.
3. 동작 검증: `/metrics` 출력, 대시보드 쿼리, consumer 테스트가 새 metric series를 읽는지 확인합니다.

## 변경 전
```cpp
std::ostringstream out;
server::core::metrics::append_build_info(out);
// emits: knights_build_info{...} 1
```

## 변경 후
```cpp
std::ostringstream out;
server::core::metrics::append_build_info(out);
// emits: runtime_build_info{...} 1
```

## 검증
- 빌드/테스트 근거: `pwsh scripts/build.ps1 -Config Release`, `DB_URI=postgresql://dynaxis:password@127.0.0.1:35432/dynaxis_db ENABLE_STACK_PYTHON_TESTS=1 ctest --preset windows-test --parallel 8 --output-on-failure`
- 런타임/메트릭 검증: `pwsh scripts/smoke_metrics.ps1`, `pwsh scripts/check_observability.ps1`, full local Docker stack sweep에서 `runtime_build_info` 노출 확인
