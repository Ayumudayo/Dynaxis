# ==============================================================================
# Knights Server - Application Build
# ==============================================================================
# Multi-stage build for minimal runtime image
# ==============================================================================

# ==============================================================================
# Stage 1: Builder
# ==============================================================================
FROM knights-base:latest AS builder

WORKDIR /app

# Copy source code
COPY . .

# Configure CMake with Linux preset (no vcpkg, uses system packages)
ENV VCPKG_DISABLE_METRICS=1
RUN apt-get update && apt-get install -y ninja-build && rm -rf /var/lib/apt/lists/*
RUN cmake --preset linux-release

# Build server components only (devclient excluded - requires ftxui from vcpkg)
RUN cmake --build --preset linux-release --target \
    server_app \
    wb_worker \
    wb_dlq_replayer \
    gateway_app \
    migrations_runner

# ==============================================================================
# Stage 2: Runtime
# ==============================================================================
FROM ubuntu:24.04

# Install runtime dependencies only (smaller than build dependencies)
RUN apt-get update && apt-get install -y \
    ca-certificates \
    libpq5 \
    libprotobuf-dev \
    libboost-system-dev \
    libhiredis-dev \
    liblz4-1 \
    dos2unix \
    && rm -rf /var/lib/apt/lists/*

# Copy custom-built libraries from builder
COPY --from=builder /usr/local/lib /usr/local/lib
RUN ldconfig

WORKDIR /app

# Copy binaries
COPY --from=builder /app/build-linux/server/server_app .
COPY --from=builder /app/build-linux/wb_worker .
COPY --from=builder /app/build-linux/wb_dlq_replayer .
COPY --from=builder /app/build-linux/gateway/gateway_app .
COPY --from=builder /app/build-linux/migrations_runner .

# Copy migration SQL files
COPY tools/migrations /app/migrations

# Copy and configure entrypoint
# Copy and configure entrypoint
COPY scripts/docker_entrypoint.sh /app/entrypoint.sh
RUN dos2unix /app/entrypoint.sh && chmod +x /app/entrypoint.sh

ENTRYPOINT ["/app/entrypoint.sh"]
