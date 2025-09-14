# 캐시 프리워밍(Prewarming) 절차

목표: Redis 복구/재시작 또는 대규모 배포 이후, 자주 사용되는 데이터(최근 메시지/멤버십/세션)를 선제적으로 채워 초기 지연과 DB 부하를 줄인다.

## 대상 및 우선순위
- 1순위: 활성 사용자 수가 많은 룸의 최근 메시지(`room:{room_id}:recent`, `msg:{id}`)
- 2순위: 룸 멤버십 집합(`room:{room_id}:members`)
- 3순위: 세션/프레즌스는 트래픽에 의해 자연 복원되므로 별도 워밍 불필요(옵션)

## 소스와 일관성
- 소스 오브 트루스는 Postgres다. 프리워밍은 Postgres에서 읽어 Redis에 적재한다.
- 트랜잭션 시점 스냅샷을 사용하거나, `wm`(워터마크) 기준으로 범위를 고정한다.

## 절차
1) 룸 랭킹 선정: 최근 1h 트래픽/동접 기준 상위 N개 룸
2) 각 룸에 대해:
   - `SELECT id DESC LIMIT recent_len`으로 메시지 id/본문 조회
   - Redis 파이프라인으로 `LPUSH/LTRIM room:recent`와 `SETEX msg:{id}` 수행
3) 검증: 표본 룸에 대해 `LRANGE`와 DB 결과 대조(샘플링)

## 스로틀링/병렬성
- 동시 작업 수를 제한(`PREWARM_CONCURRENCY`), DB/Redis CPU/IO 모니터링
- 배치 크기 조절: 룸당 메시지 개수(`ROOM_RECENT_MAXLEN`)와 파이프라인 크기 조정

## 트리거
- 수동 실행(런북) 또는 Redis 복구 이벤트 감지 시 자동 실행(플래그 제어)

## 구성 값
- `PREWARM_ENABLED` (bool)
- `PREWARM_TOP_ROOMS` (예: 100~1000)
- `PREWARM_CONCURRENCY` (예: 2~8)

