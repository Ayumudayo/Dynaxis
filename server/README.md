# server_app

`server/` 는 채팅 서버 실행 파일(server_app)을 포함합니다. 방 관리, snapshot, Redis Pub/Sub, write-behind, Instance Registry 등을 담당합니다.

## 디렉터리 개요
```
server/
├─ include/server/
│  ├─ app/      # 부트스트랩, 라우터, 메트릭
│  ├─ chat/     # 채팅 로직, 스냅샷, 핸들러
│  ├─ state/    # Presence/Instance registry
│  └─ storage/  # DB/Redis 어댑터
└─ src/
   ├─ app/
   ├─ chat/
   ├─ state/
   └─ storage/
```

## 특징
- **Opcode 라우팅**: `core::Dispatcher` 가 wire opcode 에 따라 `/login`, `/join`, `/chat`, `/whisper` 를 처리합니다.
- **Snapshot + Redis Cache**: 최근 N개 메시지는 Redis LIST/LRU 에서 우선 조회 후 DB로 폴백합니다.
- **Write-behind**: `WRITE_BEHIND_ENABLED=1`이면 Redis Streams 로 이벤트를 남기고 `wb_worker`가 DB로 flush 합니다.
- **Instance Registry**: `SERVER_ADVERTISE_HOST/PORT` 를 기준으로 Redis registry에 heartbeat를 올려 Load Balancer가 backend를 자동 감지합니다.
- **Metrics**: `/metrics` HTTP 엔드포인트에 `chat_*` 지표를 노출합니다.

## 주요 환경 변수
| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `SERVER_BIND_ADDR`, `SERVER_PORT` | 서버 listen 주소/포트 | `0.0.0.0`, `5000` |
| `DB_URI` | PostgreSQL 연결 문자열 | (필수) |
| `REDIS_URI` | Redis 연결 문자열 | 선택 |
| `WRITE_BEHIND_ENABLED` | Redis Streams write-behind | `1` |
| `USE_REDIS_PUBSUB` | Redis Pub/Sub fan-out 사용 | `0` |
| `SERVER_ADVERTISE_HOST/PORT` | Instance Registry에 노출할 주소 | listen 값 |
| `METRICS_PORT` | `/metrics` 포트 | `9090` |

전체 목록은 `docs/configuration.md` 에 정리되어 있습니다.

## 빌드 & 실행
```powershell
cmake --build build-msvc --target server_app
.uild-msvc\server\Debug\server_app.exe 5000
```
PowerShell 스크립트: `scripts/build.ps1 -UseVcpkg -Config Debug -Target server_app -Run`.

## 참고 자료
- 구조: `docs/server-architecture.md`
- 운영: `docs/ops/deployment.md`, `docs/ops/runbook.md`
- 데이터 플로우: `docs/db/write-behind.md`
