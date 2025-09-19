# 운영 체크리스트(배포/드레인/알람)

## 배포/롤링 업데이트
- 사전 확인
  - [ ] 환경 변수(.env) 값 확인: `GATEWAY_ID` 고유, `USE_REDIS_PUBSUB`, `REDIS_CHANNEL_PREFIX`
  - [ ] DB/Redis 헬스 체크 OK
  - [ ] `/metrics` 및 로그 수집기 동작 확인
- 드레인 절차(HAProxy 예시)
  - [ ] 대상 인스턴스 disable (`echo "disable server <backend>/<server>" | socat ...`)
  - [ ] 연결 소진 대기(기본 60s 이상)
  - [ ] 프로세스 종료(SIGTERM/Ctrl+C) — 구독 stop, 세션 정리
  - [ ] 재기동 후 health OK → enable

## 알람 임계(권장 초기값)
- 분산 브로드캐스트
  - [ ] `subscribe_lag_ms` room별 p95 > 200ms (5m 연속)
  - [ ] `self_echo_drop_total` 증가 추이 비정상 급증(원인: GATEWAY_ID 중복 의심)
- Write-behind
  - [ ] `wb_pending` > 500 (5m 연속)
  - [ ] `wb_fail_total` > 0 (1m) 또는 급증
  - [ ] `wb_dlq_total` 증가 추이(원인: DB 스키마/제약/네트워크 문제)
- 시스템/기반
  - [ ] Redis ping p95 > 20ms, Postgres 쿼리 지연 p95 > 50ms (5m)

## 장애 대응(요약)
- Redis 장애/과부하
  - [ ] 캐시 미스 증가 허용, 필수 기능은 DB 경로로 폴백
  - [ ] write-behind: DLQ 적재, 알람 후 재처리 도구(wb_dlq_replayer) 가동
- DB 장애
  - [ ] 서버 쓰기 경로 일시 백오프, 운영팀 호출
- 네트워크 플랩
  - [ ] Pub/Sub 재연결 백오프(이미 구현) 관측 — `subscribe_lag_ms`, `subscribe_total` 체크

## 점검 리스트(배포 전후)
- [ ] `/metrics` 응답 정상(HTTP 200, 지표 수치 합리적)
- [ ] 로그: `metric=*` 키=값 라인 수집 정상
- [ ] Redis 키/채널 접두사 충돌 없음, Streams MAXLEN 정책 확인
- [ ] DB 마이그레이션 상태 최신
