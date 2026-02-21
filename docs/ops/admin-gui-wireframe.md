# 관리자 GUI MVP 정보구조/와이어프레임

본 문서는 2단계(제어면) 기준 운영 GUI 정보구조(IA)와 화면 와이어프레임을 정의한다.

관련 문서:

- 아키텍처: `docs/ops/admin-console.md`
- API 계약: `docs/ops/admin-api-contract.md`

## 1. 정보구조(IA)

2단계 네비게이션:

1. 대시보드
2. 인스턴스
3. 접속 사용자
4. 세션 조회
5. Write-behind 상태
6. 운영 액션
7. 메트릭 링크

공통 UI 요소:

- 상단 환경 배지 (환경/env, 프로젝트/project)
- 마지막 갱신 시각
- 전역 검색(세션/인스턴스 id)
- 자동 새로고침 토글 (5s/10s/30s)

## 2. 화면별 와이어프레임

## 2.1 대시보드

목적:

- 운영자가 첫 화면에서 전체 상태를 5초 내 파악

구성:

- 요약 카드
  - gateway up/ready
  - server up/ready
  - wb_worker up/ready
  - haproxy up
- 경고 배너
- ready=false 인스턴스 존재 시 강조
- 최근 에러 카운터
  - `admin_http_errors_total`
  - `admin_http_server_errors_total`
  - `admin_http_unauthorized_total`, `admin_http_forbidden_total`
- 감사 트렌드 패널 (스파크라인/sparkline)
  - total / 5xx / 401 / 403 카드별 최근 변화량(delta) + 구간 최고점(peak) 표시
  - 모바일(좁은 화면)에서는 1열 스택으로 가독성 유지

API 매핑:

- `GET /api/v1/overview`

## 2.2 인스턴스

목적:

- backend registry 상태 점검

구성:

- 테이블 컬럼
  - instance_id
  - host:port
  - role
  - ready
  - active_sessions
  - last_heartbeat
- 정렬
  - active_sessions 내림차순/오름차순(desc/asc)
  - last_heartbeat 최신순
- 행 상세 패널
  - `/readyz` 사유(reason)
  - metrics URL 링크

API 매핑:

- `GET /api/v1/instances`
- `GET /api/v1/instances/{instance_id}`

## 2.3 접속 사용자

목적:

- 현재 접속 사용자 목록을 확인하고 운영 액션 대상을 선택

구성:

- 테이블 컬럼
  - 선택 체크박스(checkbox)
  - client_id
  - room
  - backend_instance_id
  - ready
  - session_key
- 일괄 연결 종료(Disconnect) 버튼 + 사유(reason) 입력

API 매핑:

- `GET /api/v1/users`
- `POST /api/v1/users/disconnect`

## 2.4 세션 조회

목적:

- 특정 client_id가 어느 backend에 sticky 되었는지 확인

구성:

- 입력 폼
  - `client_id`
- 결과 카드
  - backend_instance_id
  - backend ready 상태
  - registry key/session key

API 매핑:

- `GET /api/v1/sessions/{client_id}`

## 2.5 Write-behind 상태

목적:

- DB 적재 지연/실패 추세 파악

구성:

- 핵심 지표(KPI) 카드
  - pending
  - flush ok/fail
  - dlq
  - ack fail
- 시계열 미니 차트 (1단계는 단순 값 + delta)

API 매핑:

- `GET /api/v1/worker/write-behind`

## 2.6 운영 액션

목적:

- 서버 전체 공지 및 런타임 설정 변경

구성:

- 공지 전송 폼(`text`, `priority`)
- 런타임 설정 폼(`key`, `value`)
- 역할 기반(role-gated) UX(`operator`/`admin`)

API 매핑:

- `POST /api/v1/announcements`
- `PATCH /api/v1/settings`

## 2.7 메트릭 링크

목적:

- Grafana/Prometheus로 빠르게 점프

구성:

- 링크 목록
  - server 대시보드
  - write-behind 대시보드
  - 인프라 대시보드
- 쿼리 템플릿 복사

API 매핑:

- `GET /api/v1/metrics/links`

## 3. 상태/에러 UX

1. 로딩 상태
   - 스켈레톤(skeleton) + 마지막 정상 응답 시각 유지
2. 일시적 장애
   - 토스트(toast) + 자동 재시도 백오프
3. 권한 오류
   - 401/403 별도 안내
4. 조회 결과 없음(Not Found)
   - 세션 조회 화면에서 빈 결과를 정상 UX로 표시

## 4. 반응형 기준

- 데스크톱 우선(Desktop-first)
- 태블릿/모바일에서는 카드 우선 배치, 테이블은 축약 컬럼 표시

## 5. 접근성/운영성

- 색상 외 상태 표기(아이콘+텍스트)
- 키보드 포커스 이동 가능
- 타임스탬프는 UTC 기준 고정 표시

## 6. 구현 우선순위

1단계 구현 순서:

1. 대시보드
2. 인스턴스
3. 접속 사용자
4. 세션 조회
5. Write-behind 상태
6. 운영 액션
7. 메트릭 링크
