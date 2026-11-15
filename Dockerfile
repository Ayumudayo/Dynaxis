# syntax=docker/dockerfile:1
# ==============================================================================
# Dynaxis Server - Application Build
# ==============================================================================
# Multi-stage build for minimal runtime image
# ==============================================================================

# ==============================================================================
# Stage 1: Builder
# ==============================================================================
FROM dynaxis-base AS builder

WORKDIR /app

# Copy source code
COPY . .

# Dependencies are installed once per locked recipe; builds use the same native entry point.
RUN --mount=type=cache,id=dynaxis-conan2,sharing=locked,target=/root/.conan2 \
    python scripts/build.py --profile server --config Release --step all --jobs 4 --target server_app gateway_app wb_worker wb_dlq_replayer migrations_runner

RUN mkdir -p /opt/dynaxis/bin /opt/dynaxis/migrations && \
    cp /app/build/linux/server/release/server/server_app /opt/dynaxis/bin/server_app && \
    cp /app/build/linux/server/release/tools/wb_worker /opt/dynaxis/bin/wb_worker && \
    cp /app/build/linux/server/release/tools/wb_dlq_replayer /opt/dynaxis/bin/wb_dlq_replayer && \
    cp /app/build/linux/server/release/gateway/gateway_app /opt/dynaxis/bin/gateway_app && \
    cp /app/build/linux/server/release/tools/migrations_runner /opt/dynaxis/bin/migrations_runner && \
    cp -r tools/migrations/. /opt/dynaxis/migrations/ && \
    strip --strip-unneeded /opt/dynaxis/bin/server_app && \
    strip --strip-unneeded /opt/dynaxis/bin/wb_worker && \
    strip --strip-unneeded /opt/dynaxis/bin/wb_dlq_replayer && \
    strip --strip-unneeded /opt/dynaxis/bin/gateway_app && \
    strip --strip-unneeded /opt/dynaxis/bin/migrations_runner && \
    true

# ==============================================================================
# Stage 2: Shared Runtime Base
# ==============================================================================
FROM ubuntu:24.04 AS runtime-base

# Only the OS runtime is supplied by Ubuntu. Conan owns C++ dependency binaries.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libstdc++6 libgcc-s1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /app/build/linux/server/release/conan/build/Release/runtime-libs/ /usr/local/lib/
RUN ldconfig

WORKDIR /app

# Copy and configure entrypoint
COPY scripts/docker_entrypoint.sh /app/entrypoint.sh
RUN sed -i 's/\r$//' /app/entrypoint.sh && chmod +x /app/entrypoint.sh

ENTRYPOINT ["/app/entrypoint.sh"]

# ==============================================================================
# Stage 3: Per-service Runtime Images
# ==============================================================================

FROM dynaxis-base AS desktop-build
WORKDIR /app
COPY . .
RUN --mount=type=cache,id=dynaxis-conan2,sharing=locked,target=/root/.conan2 \
    python scripts/build.py --profile full --config Release --step all --jobs 4
RUN --mount=type=cache,id=dynaxis-conan2,sharing=locked,target=/root/.conan2 \
    python scripts/build.py --profile full --config Release --step test --jobs 4
RUN --mount=type=cache,id=dynaxis-conan2,sharing=locked,target=/root/.conan2 \
    LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a build/linux/full/release/client_gui/client_gui --smoke-test

FROM runtime-base AS server-runtime
COPY --from=builder /opt/dynaxis/bin/server_app /app/server_app

FROM runtime-base AS gateway-runtime
COPY --from=builder /opt/dynaxis/bin/gateway_app /app/gateway_app

FROM runtime-base AS worker-runtime
COPY --from=builder /opt/dynaxis/bin/wb_worker /app/wb_worker
COPY --from=builder /opt/dynaxis/bin/wb_dlq_replayer /app/wb_dlq_replayer


FROM runtime-base AS migrator-runtime
COPY --from=builder /opt/dynaxis/bin/migrations_runner /app/migrations_runner
COPY --from=builder /opt/dynaxis/migrations /app/migrations

# Legacy all-in-one runtime image (default target for manual builds)
FROM runtime-base AS all-runtime
COPY --from=builder /opt/dynaxis/bin/server_app /app/server_app
COPY --from=builder /opt/dynaxis/bin/wb_worker /app/wb_worker
COPY --from=builder /opt/dynaxis/bin/wb_dlq_replayer /app/wb_dlq_replayer
COPY --from=builder /opt/dynaxis/bin/gateway_app /app/gateway_app
COPY --from=builder /opt/dynaxis/bin/migrations_runner /app/migrations_runner
COPY --from=builder /opt/dynaxis/migrations /app/migrations
