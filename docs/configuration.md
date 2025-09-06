# 설정/운영 설계

## 설정 파일 형식
- 권장: YAML 또는 TOML. 예시는 YAML.

## 예시(yaml)
```yaml
server:
  listen_address: 0.0.0.0
  listen_port: 5000
  io_threads: auto          # auto | 정수
  max_connections: 10000
  recv_max_payload: 32768   # bytes
  send_queue_max: 262144    # bytes
  heartbeat_interval_ms: 10000
  read_timeout_ms: 15000
  write_timeout_ms: 15000
  backlog: 512
  sticky_sessions: true     # L4 뒤에서 클라→GW 점착 필요 시 true

security:
  tls_enabled: false
  tls_cert_file: ""
  tls_key_file: ""
  mtls_enabled: false
  ca_cert_file: ""
  ip_whitelist: []
  ip_blacklist: []

logging:
  level: info               # trace/debug/info/warn/error
  file: logs/server.log
  rotate_mb: 64
  rotate_files: 5

metrics:
  enabled: true
  exporter: prometheus
  port: 9090

limits:
  rate_limit_per_ip: 200    # req/s (게이트웨이)
  rate_limit_per_user: 100  # req/s
  send_queue_high_watermark: 2097152 # bytes
  send_queue_low_watermark:  1048576 # bytes
```

## 핫 리로드
- 읽기 전용 항목(포트/백로그)은 재시작 필요.
- 스레드/제한/타임아웃/로깅 레벨은 런타임 재적용 가능하게 설계.

### 재적용 처리(권장)
- 변경 감지: 파일 변경 감시 또는 관리 명령(`/reload`).
- 적용 순서: 로깅 레벨 → 레이트/리밋 → 타임아웃/하트비트 → 워터마크.
- 실패 시 롤백: 이전 설정 스냅샷으로 되돌림 + 경고 로그.

## 운영 가이드
- 시작/중지: 서비스 관리자 또는 단일 바이너리.
- 롤링 업데이트: 커넥션 드레이닝, 헬스 체크(accept 가능/큐 길이/오류율).
- 장애 대응: 과부하 시 새 연결 제한, 낮은 우선순위 메시지 드롭, 방화벽/RateLimit 연동.

## 우선순위/정책 값(권장 기본)
- `recv_max_payload`: 32KB, 최대 64KB.
- `heartbeat_interval_ms`: 10_000ms, miss 3회.
- `read/write_timeout_ms`: 15_000ms.
- `send_queue_max`: 256KB(샘플), 고수준 서비스는 메시지 크기/빈도에 맞춰 조정.

## 구성 소스/우선순위
1) 커맨드라인 플래그(예: `--port`, `--config`)
2) 환경변수(접두사 `SRV_`), 예: `SRV_PORT=5000`
3) 설정 파일(YAML)
4) 내장 기본값

## 검증/스키마(요약)
- 타입/범위 검증: 포트(1~65535), 타임아웃/인터벌(>0), 길이/워터마크(64KB 이하 권장), 레이트(>=0).
- 상호 제약: `send_queue_low_watermark < high_watermark <= send_queue_max`.
