# CI Build Cache Optimization Report

## 1) 배경

- 최근 CI 구성에서 Windows 테스트 잡을 `fast/slow`로 분리했을 때, 러너가 서로 달라 빌드 산출물(`build-windows`)을 공유하지 못해 사실상 중복 컴파일이 발생했다.
- 현재는 Windows 테스트를 단일 잡으로 합쳐 같은 잡 내부에서는 증분 빌드를 활용하도록 정리한 상태다.

## 2) 현재 상태 진단

### Phase A 적용 현황 (2026-03-01)

- Windows 두 잡(`core-api-consumer-windows`, `windows-fast-tests`)의 vcpkg 캐시 단계를 `actions/cache@v4` 단일 호출에서 `restore/save` 분리 방식으로 전환했다.
- 캐시 키 입력에서 변동성이 큰 `CMakePresets.json`을 제외하고, 의존성/툴체인 영향 파일만 유지했다.
  - 사용 키 입력: `vcpkg.json`, `scripts/setup_vcpkg.ps1`, `cmake/knights_vcpkg_toolchain.cmake`
- 각 Windows 잡의 job summary에 다음 telemetry를 출력하도록 반영했다.
  - `restore_hit`, `restore_elapsed_sec`, `restore_key`
  - `save_status`, `save_elapsed_sec` (exact hit 시 save skip)

### 재활용되는 것

- **vcpkg 의존성 캐시** (`actions/cache`)
  - `.cache/vcpkg/archives`
  - `.cache/vcpkg/downloads`
  - `build-windows/vcpkg_installed`
- **Docker 레이어 캐시** (`docker/build-push-action`의 `cache-from/cache-to`)

### 재활용되지 않는 것

- **C++ 빌드 산출물 자체** (`.obj/.lib/.exe`)의 cross-run 재활용
  - `actions/upload-artifact` / `actions/download-artifact` 기반 전달이 없음
  - 매 run마다 소스 컴파일은 새로 수행됨

### 확인된 수치(실측)

- 분리형(이전):
  - run `22522833769`: `1:07:37`
  - `windows-fast-tests` `0:32:04` + `windows-slow-tests` `0:35:27` (사실상 직렬)
- 통합형(현재):
  - run `22524244601`: `0:37:23`
  - Windows 단일 잡 `0:37:17`
- 기대 효과(이미 확인): 총 CI wall-clock 약 **44.7% 단축**

### 최근 PR CI baseline (최신 10회 목표, 가용 6회)

표본(run):

- `22524244601` success 2243s
- `22522833769` success 4057s
- `22521561330` success 4102s
- `22520503819` failure 3980s
- `22516687589` failure 3800s
- `22516463703` failure 90s

요약 통계(전체 6회):

- 평균: `3045.33s` (50m 45s)
- 중앙값: `3890s` (64m 50s)
- 분산: `2,160,145.22 sec^2`

요약 통계(success 3회):

- 평균: `3467.33s` (57m 47s)
- 중앙값: `4057s` (67m 37s)
- 분산: `749,833.56 sec^2`

Windows vcpkg cache restore step 소요(과거 step 기준, 6회):

- 평균: `30.67s`
- 중앙값: `30.5s`
- 분산: `10.56 sec^2`

## 3) 문제 정의

1. 의존성(vcpkg)은 캐시되지만, 코드 컴파일 결과는 매번 재생성된다.
2. vcpkg baseline이 동일해도 캐시 키 구성/러너 상태/ABI 조건에 따라 miss가 발생할 수 있다.
3. 장기적으로는 vcpkg 자체 비용(bootstrap + toolchain orchestration)을 더 줄이고 싶다.

## 4) 실행 계획

### Phase A (즉시 적용, 리스크 낮음)

1. **현 구조 유지 + 키 안정화 점검**
   - vcpkg 캐시 키에 반드시 필요한 입력만 남기고, 불필요한 변동 인자를 줄인다.
   - 목표: cache hit rate 상승, cold-start 빈도 감소.

2. **중복 빌드 지점 제거 유지**
   - Windows 단일 잡 유지(이미 반영).
   - 같은 잡 내 `cmake --build` 호출은 target 최소화로 추가 최적화 검토.

3. **관측 지표 추가**
   - 잡 요약에 다음 시간을 기록(또는 log grep):
     - vcpkg cache restore 소요
     - `Build (Debug)` 소요
     - `Build RUDP ON Targets` 소요
   - 목표: 최적화 전/후를 데이터로 비교.

### Phase B (중기, 효과 큼)

1. **원격 binary cache 강화**
   - vcpkg binary cache를 `actions/cache` + (선택) 외부 저장소(Blob/Packages)로 이중화.
   - 브랜치/PR 간 히트율 개선.

2. **컴파일 캐시 도입 검토 (`sccache`)**
   - MSVC + CMake 조합에서 object-level 캐시 도입.
   - 기대: 코드 변경이 작을 때 compile 단계 추가 단축.

3. **워크플로우 분리 전략 재정의(필요 시만)**
   - 다시 multi-job로 나눌 경우, 반드시 산출물 전달(artifact) 전략 동반.
   - 산출물 공유 없는 분리는 금지.

### Phase C (장기, 대체 패키지 전략)

1. **vcpkg 대체 가능성 조사**
   - 후보: Conan 2 + lockfile + binary remote
   - 기준: Windows/MSVC 호환성, 팀 운영 복잡도, 캐시 hit율, 전환 비용.

2. **비교 PoC**
   - 동일 의존성 집합으로 vcpkg vs Conan 시나리오를 1~2주간 병행 측정.

## 5) 기대 효과

- **즉시(Phase A)**
  - 현재 대비 추가 5~10% 개선 가능(캐시 키/타깃 정리 기준)
  - 회귀 리스크 매우 낮음

- **중기(Phase B)**
  - 재빌드 중심 PR에서 10~25% 추가 단축 가능
  - 특히 반복 PR에서 체감 효과 큼

- **장기(Phase C)**
  - 도구 전환 성공 시 의존성 해석/설치 시간의 구조적 절감 가능
  - 단, 마이그레이션 비용/운영 복잡도 증가 가능성 있음

## 6) 리스크 및 대응

- 캐시 오염/ABI mismatch
  - 대응: triplet/compiler/toolchain 버전 포함 키 정책 유지
- 캐시 미스 증가
  - 대응: restore-keys 계층화, baseline 고정, 불필요한 키 입력 제거
- 도구 전환 실패(Conan 등)
  - 대응: PoC 단계에서 성능/안정성 기준 미달 시 중단

## 6.1 캐시 miss 분류표 (운영용)

1. **키 변경 miss**
   - 징후: `restore_key`가 비어 있고 `cache-hit=false`, 최근 키 입력 파일 변경 존재
   - 원인 후보: `vcpkg.json`/toolchain 스크립트 변경
   - 대응: 의도된 miss로 처리, 새 키 저장 완료 여부 확인

2. **부분 복원(restore-key fallback) miss**
   - 징후: `cache-hit=false` + `restore_key`는 존재
   - 원인 후보: 정확 키 미존재, prefix 키만 적중
   - 대응: 빌드 완료 후 `save_status=success` 확인, 다음 run에서 exact hit 기대

3. **캐시 스토리지/전송 이슈 miss**
   - 징후: cache 단계 실패/타임아웃, restore/save 단계 오류 로그
   - 원인 후보: GitHub cache backend/network 문제
   - 대응: 재시도(run re-run), 장기화 시 prewarm 및 보조 원격 cache 검토

4. **환경 불일치 miss**
   - 징후: 키는 같아도 ABI/툴체인 차이로 실효성 저하
   - 원인 후보: runner image/toolset 변경, triplet 차이
   - 대응: 키 설계에 toolchain 영향 요소 유지, image 갱신 시 baseline 재측정

## 7) 제안 결론

- 단기적으로는 **vcpkg 유지 + 캐시 전략 고도화**가 가장 효율적이다.
- 이미 반영된 Windows 단일 잡 구조는 옳은 방향이며, 현재 수치로도 큰 개선이 확인된다.
- "vcpkg 완전 대체"는 즉시 실행보다는 PoC 기반의 단계적 판단이 적절하다.
