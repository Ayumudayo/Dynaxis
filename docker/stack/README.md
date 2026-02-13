# Docker Stack (Verification)

`docker/stack/docker-compose.yml`는 검증/스모크 테스트용으로 전체 스택을 Docker로 기동한다.

구성:
- Client(호스트) -> HAProxy(container) -> gateway_app(containers) -> server_app(containers)
- Redis/Postgres는 compose로 함께 띄운다.

## 실행

먼저 `knights-base` 이미지를 빌드한다. (`Dockerfile`이 `FROM knights-base:latest`를 사용)

```bash
docker build -f Dockerfile.base -t knights-base .
```

```bash
docker compose -f docker/stack/docker-compose.yml up -d --build
```

접속:
- 게임 트래픽: `127.0.0.1:6000` (HAProxy)
- HAProxy stats: `http://127.0.0.1:8404/`
- gateway metrics: `http://127.0.0.1:36001/metrics`, `http://127.0.0.1:36002/metrics`
- server metrics: `http://127.0.0.1:39091/metrics`, `http://127.0.0.1:39092/metrics`

## 종료

```bash
docker compose -f docker/stack/docker-compose.yml down
```
