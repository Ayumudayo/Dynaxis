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

# Configure CMake with Linux preset (uses system packages)
RUN cmake --preset linux-release -DBUILD_SERVER_TESTS=OFF

# Build server components only (devclient excluded for image scope)
RUN cmake --build --preset linux-release --target \
    server_app \
    chat_hook_sample \
    chat_hook_sample_v2 \
    chat_hook_tag \
    admin_app \
    wb_worker \
    wb_dlq_replayer \
    gateway_app \
    migrations_runner

RUN mkdir -p /opt/dynaxis/bin /opt/dynaxis/plugins/staging /opt/dynaxis/migrations /opt/dynaxis/scripts && \
    cp /app/build-linux/server/server_app /opt/dynaxis/bin/server_app && \
    cp /app/build-linux/tools/admin_app /opt/dynaxis/bin/admin_app && \
    cp /app/build-linux/tools/wb_worker /opt/dynaxis/bin/wb_worker && \
    cp /app/build-linux/tools/wb_dlq_replayer /opt/dynaxis/bin/wb_dlq_replayer && \
    cp /app/build-linux/gateway/gateway_app /opt/dynaxis/bin/gateway_app && \
    cp /app/build-linux/tools/migrations_runner /opt/dynaxis/bin/migrations_runner && \
    cp /app/build-linux/server/plugins/10_chat_hook_sample.so /opt/dynaxis/plugins/10_chat_hook_sample.so && \
    cp /app/build-linux/server/plugins/20_chat_hook_tag.so /opt/dynaxis/plugins/20_chat_hook_tag.so && \
    cp /app/build-linux/server/plugins/10_chat_hook_sample_v2.so /opt/dynaxis/plugins/staging/10_chat_hook_sample_v2.so && \
    cp -r /app/server/scripts/. /opt/dynaxis/scripts/ && \
    cp tools/admin_app/admin_ui.html /opt/dynaxis/admin_ui.html && \
    cp -r tools/migrations/. /opt/dynaxis/migrations/ && \
    strip --strip-unneeded /opt/dynaxis/bin/server_app && \
    strip --strip-unneeded /opt/dynaxis/bin/admin_app && \
    strip --strip-unneeded /opt/dynaxis/bin/wb_worker && \
    strip --strip-unneeded /opt/dynaxis/bin/wb_dlq_replayer && \
    strip --strip-unneeded /opt/dynaxis/bin/gateway_app && \
    strip --strip-unneeded /opt/dynaxis/bin/migrations_runner && \
    rm -rf /app/build-linux

# ==============================================================================
# Stage 2: Shared Runtime Base
# ==============================================================================
FROM ubuntu:24.04 AS runtime-base

# Install runtime dependencies only (smaller than build dependencies)
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libpq5 \
    libprotobuf32t64 \
    libhiredis1.1.0 \
    liblz4-1 \
    && rm -rf /var/lib/apt/lists/*

# Copy custom-built libraries from builder
COPY --from=builder /usr/local/lib/libpqxx* /usr/local/lib/
COPY --from=builder /usr/local/lib/libredis++* /usr/local/lib/
RUN ldconfig

WORKDIR /app

# Copy and configure entrypoint
COPY scripts/docker_entrypoint.sh /app/entrypoint.sh
RUN sed -i 's/\r$//' /app/entrypoint.sh && chmod +x /app/entrypoint.sh

ENTRYPOINT ["/app/entrypoint.sh"]

# ==============================================================================
# Stage 3: Per-service Runtime Images
# ==============================================================================
FROM runtime-base AS server-runtime
COPY --from=builder /opt/dynaxis/bin/server_app /app/server_app
COPY --from=builder /opt/dynaxis/plugins /app/plugins_builtin
COPY --from=builder /opt/dynaxis/plugins /app/plugins
COPY --from=builder /opt/dynaxis/scripts /app/scripts_builtin
COPY --from=builder /opt/dynaxis/scripts /app/scripts

FROM runtime-base AS gateway-runtime
COPY --from=builder /opt/dynaxis/bin/gateway_app /app/gateway_app

FROM runtime-base AS worker-runtime
COPY --from=builder /opt/dynaxis/bin/wb_worker /app/wb_worker
COPY --from=builder /opt/dynaxis/bin/wb_dlq_replayer /app/wb_dlq_replayer

FROM runtime-base AS admin-runtime
COPY --from=builder /opt/dynaxis/bin/admin_app /app/admin_app
COPY --from=builder /opt/dynaxis/admin_ui.html /app/admin_ui.html

FROM runtime-base AS migrator-runtime
COPY --from=builder /opt/dynaxis/bin/migrations_runner /app/migrations_runner
COPY --from=builder /opt/dynaxis/migrations /app/migrations

# Legacy all-in-one runtime image (default target for manual builds)
FROM runtime-base AS all-runtime
COPY --from=builder /opt/dynaxis/bin/server_app /app/server_app
COPY --from=builder /opt/dynaxis/bin/admin_app /app/admin_app
COPY --from=builder /opt/dynaxis/bin/wb_worker /app/wb_worker
COPY --from=builder /opt/dynaxis/bin/wb_dlq_replayer /app/wb_dlq_replayer
COPY --from=builder /opt/dynaxis/bin/gateway_app /app/gateway_app
COPY --from=builder /opt/dynaxis/bin/migrations_runner /app/migrations_runner
COPY --from=builder /opt/dynaxis/admin_ui.html /app/admin_ui.html
COPY --from=builder /opt/dynaxis/plugins /app/plugins_builtin
COPY --from=builder /opt/dynaxis/plugins /app/plugins
COPY --from=builder /opt/dynaxis/scripts /app/scripts_builtin
COPY --from=builder /opt/dynaxis/scripts /app/scripts
COPY --from=builder /opt/dynaxis/migrations /app/migrations
