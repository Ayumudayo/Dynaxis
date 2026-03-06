# Extensibility Conflict Policy

이 문서는 동일 hook 영역에서 플러그인/스크립트 충돌을 줄이기 위한 운영 정책을 정의한다.

## 1) 용어

- `hook_scope`: 적용 대상 hook (`on_login`, `on_join`, `on_admin_command` 등)
- `stage`: 실행 단계 (`pre_validate`, `mutate`, `authorize`, `side_effect`, `observe`)
- `priority`: 같은 stage 내 실행 순서(작을수록 먼저 실행)
- `exclusive_group`: 같은 그룹 내 동시 배포를 금지할 식별자

## 2) 기본 규칙

1. 실행 순서: `priority` 오름차순
2. 터미널 결정 우선순위: `block/deny > handled > modify > pass`
3. `observe` stage는 상태 변경 결정을 금지하고 `pass`만 허용
4. 같은 `(hook_scope, stage, exclusive_group)` 조합은 배포 전 precheck 차단 대상(운영 규약)

## 3) 권장 stage 배치

- `pre_validate`: 입력 형식/제약 검사
- `mutate`: 텍스트/파라미터 정규화
- `authorize`: 접근 허용/거부
- `side_effect`: 알림/후속 작업 트리거
- `observe`: 로깅/계측 전용

## 4) 실무 가이드

- deny 권한이 필요한 정책은 `authorize`에만 둔다.
- 텍스트 치환은 `mutate`에서만 수행한다.
- 운영 공지/감사는 `observe` 또는 `side_effect`로 분리한다.
- 동일 목적 기능을 여러 artifact로 쪼갤 때 `exclusive_group`을 고정해 충돌 가능성을 낮춘다.

## 5) precheck 실패 예시

- 동일 `hook_scope=on_join`, `stage=authorize`, `exclusive_group=vip_gate` artifact 2개 동시 배포
- `observe` stage artifact가 `deny` 결정을 반환
- `mutate` stage가 `priority` 없이 다중 배포되어 실행 순서가 불명확

## 6) 현재 상태

- 현재 저장소는 본 문서 규칙을 운영 규약으로 사용한다.
- 관리 콘솔 precheck/API 강제 차단은 구현 완료 상태이며, `/api/v1/ext/precheck`와 `/api/v1/ext/deployments`에서 충돌 정책을 강제한다.
