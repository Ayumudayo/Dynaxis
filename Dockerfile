# Application Build Stage
# We use the locally built 'knights-base' image which contains all dependencies
FROM knights-base:latest AS builder

# Set working directory (inherited from base, but good to be explicit)
WORKDIR /app

# Copy the actual source code
# Since dependencies are already installed in the base image, we just copy sources
COPY . .

# Build the project
# Configure CMake again to detect the real source files
# The base image only configured for dummy files, so we need to regenerate the build system
RUN cmake --preset linux

# Build the project
# We use the same preset as the base image
RUN cmake --build --preset linux-release --target server_app wb_worker wb_dlq_replayer

# Runtime Stage
FROM ubuntu:24.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    ca-certificates \
    libpq5 \
    libprotobuf-dev \
    libgrpc++-dev \
    libboost-system-dev \
    libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy custom built libraries (libpqxx, redis++)
COPY --from=builder /usr/local/lib /usr/local/lib
RUN ldconfig

WORKDIR /app

# Copy executables from builder
COPY --from=builder /app/build-linux/server/server_app .
COPY --from=builder /app/build-linux/wb_worker .
COPY --from=builder /app/build-linux/wb_dlq_replayer .

# Create a startup script to choose which binary to run
COPY scripts/docker_entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

ENTRYPOINT ["/app/entrypoint.sh"]
