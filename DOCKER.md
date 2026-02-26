# 도커(Docker) 빌드/배포 가이드

## 개요

Knights 서버는 멀티 스테이지 Docker 빌드 전략을 사용합니다.
- **Dockerfile.base**: C++ 의존성을 포함한 베이스 이미지
- **Dockerfile**: 애플리케이션 빌드 단계
- **docker-compose.yml**: 멀티 서비스 오케스트레이션

## 빠른 시작

```bash
# 전체 서비스 빌드 + 시작
.\scripts\deploy_docker.ps1 -Action up

# 빌드만 수행(시작 안 함)
.\scripts\deploy_docker.ps1 -Action build

# 베이스 이미지 재빌드(클린 빌드)
.\scripts\deploy_docker.ps1 -Action build -NoCache
```

## 아키텍처

### 서비스 구성

| 서비스 | 포트 | 용도 |
|---------|------|---------|
| `postgres` | 5432 | PostgreSQL 데이터베이스 |
| `redis` | 6379 | Redis(세션, write-back 큐) |
| `load_balancer` | 7001 | gRPC 로드밸런서 |
| `gateway` | 6000 | TCP 게이트웨이(클라이언트 연결) |
| `server-1` | 10001 | 게임 서버 인스턴스 #1 |
| `server-2` | 10002 | 게임 서버 인스턴스 #2 |
| `wb_worker` | - | 백그라운드 write-behind 워커 |
| `prometheus` | 9090 | 메트릭 수집 |
| `grafana` | 3000 | 메트릭 시각화 |

### 네트워크 구조

```
Client → Gateway:6000 → Load Balancer:7001 → Server:10001/10002
                                            ↓
                                      Redis + PostgreSQL
```

## 빌드 전략

### 하이브리드 의존성 관리

**Windows 개발 환경**: vcpkg(정리된 개발 경험)
**Docker/Linux 런타임**: 소스 빌드(안정적이고 재현 가능)

### 하이브리드 전략을 쓰는 이유

- **libpqxx**: ABI 호환성을 위해 C++20 소스 빌드 사용
- **redis-plus-plus**: Ubuntu 기본 저장소 미제공
- **Boost, Protobuf, gRPC**: 안정적인 시스템 apt 패키지 사용
- **ftxui**: Docker 빌드 제외(Windows 전용 devclient)

## 이미지 빌드

### 베이스 이미지 (`knights-base`)

모든 의존성이 포함됩니다. 아래 상황에서 재빌드합니다.
- C++ 라이브러리 버전 변경
- 빌드 도구(CMake 등) 변경
- 새 의존성 추가

```bash
docker build -f Dockerfile.base -t knights-base:latest .
```

**빌드 시간**: 약 5~10분(네트워크 상태에 따라 변동)

### 애플리케이션 이미지

`knights-base`를 기반으로 빌드합니다. 아래 상황에서 재빌드합니다.
- 애플리케이션 코드 변경
- 설정 변경

```bash
docker compose build
```

**빌드 시간**: 약 2~3분(베이스 캐시 사용 시)

## 환경 변수

`.env` 파일에서 설정합니다(`.env.example` 복사 후 사용).

```env
# 데이터베이스(Database)
DB_URI=postgresql://knights:password@postgres:5432/knights

# 레디스(Redis)
REDIS_URI=redis://redis:6379

# 로드밸런서(Load Balancer)
LB_GRPC_LISTEN=0.0.0.0:7001
LB_BACKEND_ENDPOINTS=server-1:10001,server-2:10002

# 게이트웨이(Gateway)
GATEWAY_ID=gw1
GATEWAY_LB_ADDRESS=load_balancer:7001
```

## 자주 사용하는 작업

### 로그 보기

```bash
# 전체 서비스
docker compose logs -f

# 특정 서비스
docker compose logs -f server-1
docker compose logs -f load_balancer
```

### 마이그레이션 실행

```bash
docker compose run --rm migrator
```

### 서버 스케일 확장

`docker-compose.yml`에 서버 인스턴스를 추가한 뒤 실행합니다.

```bash
docker compose up -d --scale server-1=3
```

## 문제 해결

### 빌드 실패

**vcpkg 다운로드 오류**:
```
error: building boost-mpl:x64-linux failed
```
**해결 방법**: Linux Docker 환경에서는 정상일 수 있습니다. 서버 컴포넌트는 vcpkg가 필수는 아닙니다.

**libpqxx 미발견**:
```
CMake Error: libpqxx not found
```
**해결 방법**: `--no-cache` 옵션으로 베이스 이미지를 재빌드합니다.

### 런타임 실패

**연결 거부(Connection refused)**:
- 서비스 상태 확인: `docker compose ps`
- 로그 확인: `docker compose logs <service>`
- 네트워크 확인: `docker network inspect knights_default`

**데이터베이스 마이그레이션 오류**:
- migrator 실행 전 postgres가 healthy 상태인지 확인
- `tools/migrations/`의 SQL 파일 확인

## 성능 팁

1. **빌드 캐시 활용**: 필요하지 않다면 `--no-cache`를 사용하지 않습니다.
2. **레이어 최적화**: `.dockerignore`로 불필요 파일을 제외합니다.
3. **멀티코어 빌드**: 소스 빌드에서 `-j$(nproc)`를 사용합니다.
4. **즉시 정리**: `RUN rm -rf`로 빌드 산출물을 바로 제거합니다.

## 보안 참고

- **`.env` 커밋 금지**: 민감한 자격증명이 포함됩니다.
- **운영 환경은 시크릿 사용**: 환경 변수를 Docker secrets로 대체합니다.
- **베이스 이미지 주기적 갱신**: 보안 패치를 반영해 정기 재빌드합니다.

## 추가 참고

- [DEPENDENCIES.md](./DEPENDENCIES.md): 의존성 관리 전략
- [docker-compose.yml](./docker-compose.yml): 서비스 정의
- [scripts/deploy_docker.ps1](./scripts/deploy_docker.ps1): 배포 스크립트
