# Docker Build & Deployment Guide

## Overview

The Knights server uses a multi-stage Docker build strategy:
- **Dockerfile.base**: Base image with all C++ dependencies
- **Dockerfile**: Application build stage  
- **docker-compose.yml**: Multi-service orchestration

## Quick Start

```bash
# Build and start all services
.\scripts\deploy_docker.ps1 -Action up

# Build only (no start)
.\scripts\deploy_docker.ps1 -Action build

# Rebuild base image (clean build)
.\scripts\deploy_docker.ps1 -Action build -NoCache
```

## Architecture

### Services

| Service | Port | Purpose |
|---------|------|---------|
| `postgres` | 5432 | PostgreSQL database |
| `redis` | 6379 | Redis (sessions, write-back queue) |
| `load_balancer` | 7001 | gRPC load balancer |
| `gateway` | 6000 | TCP gateway (client connections) |
| `server-1` | 10001 | Game server instance #1 |
| `server-2` | 10002 | Game server instance #2 |
| `wb_worker` | - | Write-behind worker (background) |
| `prometheus` | 9090 | Metrics collection |
| `grafana` | 3000 | Metrics visualization |

### Network Architecture

```
Client → Gateway:6000 → Load Balancer:7001 → Server:10001/10002
                                            ↓
                                      Redis + PostgreSQL
```

## Build Strategy

### Hybrid Dependency Management

**Windows Development**: vcpkg (clean, modern)
**Docker/Linux Production**: Source builds (stable, reproducible)

### Why Hybrid?

- **libpqxx**: Built from source with C++20 for ABI compatibility
- **redis-plus-plus**: Not available in Ubuntu repos
- **Boost, Protobuf, gRPC**: System apt packages (stable)
- **ftxui**: Excluded from Docker builds (Windows-only devclient)

## Building Images

### Base Image (`knights-base`)

Contains all dependencies. Rebuild when:
- Updating C++ library versions
- Changing build tools (CMake, etc.)
- Adding new dependencies

```bash
docker build -f Dockerfile.base -t knights-base:latest .
```

**Build time**: ~5-10 minutes (with network)

### Application Images

Built from `knights-base`. Rebuild when:
- Changing application code
- Updating configuration

```bash
docker compose build
```

**Build time**: ~2-3 minutes (cached base)

## Environment Variables

Configured in `.env` file (copy from `.env.example`):

```env
# Database
DB_URI=postgresql://knights:password@postgres:5432/knights

# Redis
REDIS_URI=redis://redis:6379

# Load Balancer
LB_GRPC_LISTEN=0.0.0.0:7001
LB_BACKEND_ENDPOINTS=server-1:10001,server-2:10002

# Gateway
GATEWAY_ID=gw1
GATEWAY_LB_ADDRESS=load_balancer:7001
```

## Common Operations

### View Logs

```bash
# All services
docker compose logs -f

# Specific service
docker compose logs -f server-1
docker compose logs -f load_balancer
```

### Run Migrations

```bash
docker compose run --rm migrator
```

### Scale Servers

Edit `docker-compose.yml` to add more server instances, then:

```bash
docker compose up -d --scale server-1=3
```

## Troubleshooting

### Build Failures

**vcpkg download errors**:
```
error: building boost-mpl:x64-linux failed
```
**Solution**: Normal on Linux Docker. Server components don't need vcpkg.

**libpqxx not found**:
```
CMake Error: libpqxx not found
```
**Solution**: Rebuild base image with `--no-cache`

### Runtime Failures

**Connection refused**:
- Check service health: `docker compose ps`
- Check logs: `docker compose logs <service>`
- Verify network: `docker network inspect knights_default`

**Database migration errors**:
- Ensure postgres is healthy before migrator runs
- Check migration SQL files in `tools/migrations/`

## Performance Tips

1. **Use build cache**: Don't use `--no-cache` unless necessary
2. **Layer optimization**: `.dockerignore` excludes unnecessary files
3. **Multi-core builds**: `-j$(nproc)` used in all source builds
4. **Cleanup**: `RUN rm -rf` removes build artifacts immediately

## Security Notes

- **Never commit `.env`**: Contains sensitive credentials
- **Use secrets in production**: Replace environment variables with Docker secrets
- **Update base image**: Regularly rebuild with security patches

## Further Reading

- [DEPENDENCIES.md](./DEPENDENCIES.md): Dependency management strategy
- [docker-compose.yml](./docker-compose.yml): Service definitions
- [scripts/deploy_docker.ps1](./scripts/deploy_docker.ps1): Deployment script
