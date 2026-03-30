# 문서 인덱스

이 문서는 저장소 문서의 기준 진입점이다.

여기에는 현재 상태를 설명하는 문서만 남긴다. 과거 브랜치나 tranche 메모는 active docs tree에 두지 않고 git history 또는 local task note로 관리한다. 이렇게 해야 독자가 오래된 문서를 현재 기준으로 오해하지 않는다.

## 먼저 읽을 문서

- `README.md`
  - 저장소 개요와 상위 링크
- `docs/getting-started.md`
  - 로컬 설치와 첫 실행 순서
- `docs/build.md`
  - 빌드 환경, Docker 스택, package smoke
- `docs/configuration.md`
  - 런타임 환경 변수와 운영상 의미
- `docs/tests.md`
  - 로컬/CI 검증 진입점
- `docs/rename_boundary.md`
  - rename 정책과 허용된 legacy 기록
- `docs/naming-conventions.md`
  - 명명 규칙과 주석 규칙

## 런타임과 운영

- `docs/core-design.md`
  - core/runtime layering과 ownership 요약
- `docs/core-architecture-rationale.md`
  - `server_core`가 왜 현재 구조를 갖는지에 대한 심층 배경
- `docs/architecture/README.md`
  - architecture/build-test ownership 재정리 문서 인덱스
- `docs/protocol.md`
  - 프로토콜 개요
- `docs/protocol/snapshot.md`
  - room snapshot, recent-history cache, join-to-fanout 흐름
- `docs/protocol/rudp.md`
  - 현재 UDP/RUDP 동작
- `docs/db/architecture.md`
  - 현재 DB/runtime 저장소 구조
- `docs/db/redis-strategy.md`
  - Redis 역할, key pattern, failure model
- `docs/db/migrations.md`
  - 현재 migration 진입점
- `docs/db/write-behind.md`
  - write-behind 런타임과 운영
- `docs/ops/observability.md`
  - 관측성 설정과 대시보드
- `docs/ops/gateway-and-lb.md`
  - gateway와 load balancer 운영
- `docs/ops/runbook.md`
  - 운영 체크리스트와 장애 대응
- `docs/ops/architecture-diagrams.md`
  - control-plane과 failure-path 다이어그램
- `docs/ops/engine-readiness-baseline.md`
  - 공통 readiness 기준선
- `docs/ops/engine-readiness-decision.md`
  - readiness 기준선 채택 결정과 후속 ownership
- `docs/ops/quantified-release-evidence.md`
  - 릴리스 증거물과 임계치
- `docs/ops/worlds-kubernetes-localdev.md`
  - topology-driven Kubernetes local/dev harness
- `docs/ops/session-continuity-contract.md`
  - 현재 continuity ownership과 restore 규칙
- `docs/ops/mmorpg-world-residency-contract.md`
  - 현재 world residency/lifecycle/runtime 계약
- `docs/ops/mmorpg-desired-topology-contract.md`
  - 현재 desired-topology, actuation, orchestration 계약
- `docs/ops/realtime-runtime-contract.md`
  - 현재 realtime transport/runtime substrate 계약

## 확장성

- `docs/extensibility/overview.md`
  - 현재 확장 기능 개요와 entrypoint
- `docs/extensibility/governance.md`
  - ownership과 호환성 규칙
- `docs/extensibility/control-plane-api.md`
  - admin/control-plane API 계약
- `docs/extensibility/conflict-policy.md`
  - rollout/conflict 정책
- `docs/extensibility/plugin-quickstart.md`
  - plugin 빠른 시작
- `docs/extensibility/lua-quickstart.md`
  - Lua 빠른 시작
- `docs/extensibility/recipes.md`
  - 구현/배포 레시피

## Core API

- `docs/core-api/overview.md`
  - public `server_core` package surface 지도와 읽기 순서
- `docs/core-api/quickstart.md`
  - 최소 consumer setup과 installed-package 흐름
- `docs/core-api/compatibility-policy.md`
  - stable/transitional 거버넌스와 버전 규칙
- `docs/core-api/checklists.md`
  - PR/release 검증 체크리스트와 명령
- `docs/core-api/adoption-cutover.md`
  - 현재 stable/transitional/internal 컷오버 상태
- `docs/core-api/extensions.md`
  - extensibility 관련 public API 설명

## 도구 문서

- `tools/README.md`
  - 도구 인덱스
- `tools/loadgen/README.md`
  - load generator 시나리오와 transport proof 카탈로그
- `tools/admin_app/README.md`
  - admin control-plane 동작과 endpoint
- `tools/migrations/README.md`
  - migration runner 사용법
- `tools/wb_worker/README.md`
  - write-behind worker 런타임 가이드
