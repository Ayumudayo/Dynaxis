# Operations: Redis Fallback and Recovery

목표: Redis 장애 시 서비스 기능을 유지하고, Postgres로 폴백/복구하는 절차를 정의한다.

## 장애 모드
- Redis 완전 다운/네트워크 분리
- 부분 장애(응답 지연/메모리 압박/쓰기 실패)
- 데이터 손실(캐시/Streams 트림 과도)

## 폴백 정책
- 기본: 기능 지속이 우선. 캐시 미스→Postgres로 폴백, 팬아웃은 인프로세스/지역 브로드캐스트로 축소
- 플래그(예시):
  - `REDIS_REQUIRED=false` (true면 하드 페일)
  - `USE_REDIS_CACHE=true` (false면 캐시 접근 비활성)
  - `USE_REDIS_PUBSUB=true` (false면 노드 로컬 브로드캐스트)
  - `WRITE_BEHIND_ENABLED=false` (장애 시 write-through 전환)

## Redis 복구/재가동 시
- 히스토리 캐시 재가열: 백그라운드에서 최근 N개 메시지 재적재(우선순위: 활성 룸)
- 프레즌스 재구성: 클라이언트 heartbeat 재수신을 통해 점진 복원. 초기에는 프레즌스 기능 제한 안내
- Streams 재동기화: 컨슈머 그룹의 펜딩/누락 재처리, DLQ 모니터링

## 경보(알람) 임계
- Redis ping latency p95 > 20ms (5m)
- 에러율(커맨드 실패) > 1% (1m)
- Pool exhaustion > 80% (5m)
- Streams pending length 증가 추세(원인 역추적)
- 폴백 모드 진입 이벤트(즉시)

## 대시보드 지표
- 캐시 히트율, 키공간 메모리/eviction
- 쿼리 레이턴시(p50/p95/p99), Postgres 폴백 비율
- 팬아웃 방식(REDIS vs LOCAL) 및 메시지 지연
- Write-behind 배치 크기/지연/성공률

## 런북(요약)
- 장애 감지 → 폴백 플래그 적용 확인(`REDIS_REQUIRED=false`, `USE_REDIS_*` 조정)
- Postgres 부하 모니터링(풀/CPU/슬로 쿼리) — 필요 시 `RECENT_HISTORY_LIMIT` 축소
- Redis 복구 후:
  - Streams 컨슈머 재시작 → 펜딩 처리
  - 캐시 재가열 잡 실행(활성 룸부터)
  - 플래그 원복, 알람 정상화 확인

