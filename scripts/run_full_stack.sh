#!/usr/bin/env bash
# Local full stack runner: (optional) Docker infra + server_app cluster + gateway_app cluster + HAProxy.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CONFIG="${CONFIG:-Debug}"
BUILD_DIR="${BUILD_DIR:-build-linux}"

WITH_DOCKER_INFRA="${WITH_DOCKER_INFRA:-0}"
WITH_POSTGRES="${WITH_POSTGRES:-0}"
RUN_MIGRATIONS="${RUN_MIGRATIONS:-0}"
WITH_WORKER="${WITH_WORKER:-0}"
STOP_INFRA_ON_EXIT="${STOP_INFRA_ON_EXIT:-0}"

SERVER_COUNT="${SERVER_COUNT:-2}"
SERVER_BASE_PORT="${SERVER_BASE_PORT:-5101}"
SERVER_ID_PREFIX="${SERVER_ID_PREFIX:-server-local}"
SERVER_ADVERTISE_HOST="${SERVER_ADVERTISE_HOST:-127.0.0.1}"

GATEWAY_COUNT="${GATEWAY_COUNT:-2}"
GATEWAY_BASE_PORT="${GATEWAY_BASE_PORT:-6101}"
GATEWAY_METRICS_BASE_PORT="${GATEWAY_METRICS_BASE_PORT:-6201}"
GATEWAY_ID_PREFIX="${GATEWAY_ID_PREFIX:-gateway-local}"

HAPROXY_PORT="${HAPROXY_PORT:-6000}"
HAPROXY_BIN="${HAPROXY_BIN:-haproxy}"

REGISTRY_PREFIX="${REGISTRY_PREFIX:-gateway/instances/}"
REGISTRY_TTL="${REGISTRY_TTL:-30}"
HEARTBEAT_INTERVAL="${HEARTBEAT_INTERVAL:-5}"

REDIS_URI="${REDIS_URI:-}"
DB_URI="${DB_URI:-}"

NO_DOTENV="${NO_DOTENV:-0}"
NO_BUILD="${NO_BUILD:-0}"

info(){ printf '[info] %s\n' "$1" >&2; }
warn(){ printf '[warn] %s\n' "$1" >&2; }
fail(){ printf '[fail] %s\n' "$1" >&2; exit 1; }

usage(){
  cat <<EOF
Usage: run_full_stack.sh

Configure via environment variables:
  CONFIG, BUILD_DIR
  WITH_DOCKER_INFRA=1, WITH_POSTGRES=1, RUN_MIGRATIONS=1, WITH_WORKER=1, STOP_INFRA_ON_EXIT=1
  SERVER_COUNT, SERVER_BASE_PORT, SERVER_ID_PREFIX, SERVER_ADVERTISE_HOST
  GATEWAY_COUNT, GATEWAY_BASE_PORT, GATEWAY_METRICS_BASE_PORT, GATEWAY_ID_PREFIX
  HAPROXY_PORT, HAPROXY_BIN
  REDIS_URI, DB_URI
  NO_DOTENV=1, NO_BUILD=1

Example (Docker infra + migrations + worker):
  WITH_DOCKER_INFRA=1 WITH_POSTGRES=1 RUN_MIGRATIONS=1 WITH_WORKER=1 ./scripts/run_full_stack.sh
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then usage; exit 0; fi

cd "$REPO_ROOT"

if [[ "$NO_DOTENV" != "1" && -f ./.env ]]; then
  info "Loading env file: ./.env"
  set -a
  # shellcheck disable=SC1091
  . ./.env
  set +a
fi

if [[ -z "$REDIS_URI" ]]; then
  if [[ "$WITH_DOCKER_INFRA" == "1" ]]; then REDIS_URI="tcp://127.0.0.1:36379"; else REDIS_URI="tcp://127.0.0.1:6379"; fi
fi

if [[ -z "$DB_URI" ]]; then
  if [[ "$WITH_POSTGRES" == "1" ]]; then
    if [[ "$WITH_DOCKER_INFRA" == "1" ]]; then DB_URI="postgresql://knights:password@127.0.0.1:35432/knights_db";
    else fail "DB_URI must be provided when WITH_POSTGRES=1 without WITH_DOCKER_INFRA=1";
    fi
  fi
fi

if [[ "$REGISTRY_PREFIX" != */ ]]; then REGISTRY_PREFIX="${REGISTRY_PREFIX}/"; fi

infra_compose="$REPO_ROOT/docker/infra/docker-compose.yml"

find_binary(){
  local name="$1"
  local -a candidates=(
    "$BUILD_DIR/$name"
    "$BUILD_DIR/$CONFIG/$name"
    "$BUILD_DIR/server/$CONFIG/$name"
    "$BUILD_DIR/gateway/$CONFIG/$name"
    "$BUILD_DIR/tools/$CONFIG/$name"
  )
  for p in "${candidates[@]}"; do
    [[ -x "$p" ]] && { echo "$p"; return 0; }
    [[ -x "$p.exe" ]] && { echo "$p.exe"; return 0; }
  done
  local found
  found=$(find "$BUILD_DIR" -type f -name "$name" -o -name "$name.exe" 2>/dev/null | head -n1 || true)
  [[ -n "$found" ]] && { echo "$found"; return 0; }
  return 1
}

declare -a server_pids=()
declare -a gateway_pids=()
worker_pid=""
hap_pid=""
hap_cfg=""

cleanup(){
  set +e
  if [[ -n "$hap_pid" ]]; then kill "$hap_pid" 2>/dev/null || true; fi
  if [[ -n "$worker_pid" ]]; then kill "$worker_pid" 2>/dev/null || true; fi
  for pid in "${gateway_pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${server_pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  if [[ -n "$hap_cfg" && -f "$hap_cfg" ]]; then rm -f "$hap_cfg" || true; fi
  if [[ "$WITH_DOCKER_INFRA" == "1" && "$STOP_INFRA_ON_EXIT" == "1" ]]; then
    docker compose -f "$infra_compose" down >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

if ! [[ "$SERVER_COUNT" =~ ^[0-9]+$ ]] || [[ "$SERVER_COUNT" -lt 1 ]]; then fail "SERVER_COUNT must be >= 1"; fi
if ! [[ "$GATEWAY_COUNT" =~ ^[0-9]+$ ]] || [[ "$GATEWAY_COUNT" -lt 1 ]]; then fail "GATEWAY_COUNT must be >= 1"; fi

if [[ "$WITH_DOCKER_INFRA" == "1" ]]; then
  [[ -f "$infra_compose" ]] || fail "Missing infra compose file: $infra_compose"
  command -v docker >/dev/null 2>&1 || fail "docker not found in PATH"
  services=(redis)
  if [[ "$WITH_POSTGRES" == "1" ]]; then services+=(postgres); fi
  info "Starting docker infra: ${services[*]}"
  docker compose -f "$infra_compose" up -d "${services[@]}" >/dev/null
  sleep 1
fi

if [[ "$NO_BUILD" != "1" ]]; then
  targets=(server_app gateway_app)
  [[ "$RUN_MIGRATIONS" == "1" ]] && targets+=(migrations_runner)
  [[ "$WITH_WORKER" == "1" ]] && targets+=(wb_worker)
  info "Build targets: ${targets[*]}"
  for t in "${targets[@]}"; do
    ./scripts/build.sh -c "$CONFIG" -b "$BUILD_DIR" -r all -t "$t" >/dev/null
  done
fi

server_exe="$(find_binary server_app)" || fail "server_app executable not found under $BUILD_DIR"
gateway_exe="$(find_binary gateway_app)" || fail "gateway_app executable not found under $BUILD_DIR"

if [[ "$RUN_MIGRATIONS" == "1" ]]; then
  [[ "$WITH_POSTGRES" == "1" ]] || fail "RUN_MIGRATIONS requires WITH_POSTGRES=1"
  mig_exe="$(find_binary migrations_runner)" || fail "migrations_runner executable not found"
  info "Running migrations_runner"
  "$mig_exe" --db-uri "$DB_URI"
fi

if [[ "$WITH_WORKER" == "1" ]]; then
  [[ "$WITH_POSTGRES" == "1" ]] || fail "WITH_WORKER requires WITH_POSTGRES=1"
  worker_exe="$(find_binary wb_worker)" || fail "wb_worker executable not found"
fi

info "Starting $SERVER_COUNT server_app instances"
for ((i=0; i<SERVER_COUNT; i++)); do
  port=$((SERVER_BASE_PORT + i))
  instance_id="${SERVER_ID_PREFIX}-${port}"
  ( \
    PORT="${port}" \
    DB_URI="${DB_URI}" \
    REDIS_URI="${REDIS_URI}" \
    METRICS_PORT="" \
    SERVER_ADVERTISE_HOST="${SERVER_ADVERTISE_HOST}" \
    SERVER_ADVERTISE_PORT="${port}" \
    SERVER_INSTANCE_ID="${instance_id}" \
    SERVER_REGISTRY_PREFIX="${REGISTRY_PREFIX}" \
    SERVER_REGISTRY_TTL="${REGISTRY_TTL}" \
    SERVER_HEARTBEAT_INTERVAL="${HEARTBEAT_INTERVAL}" \
    "$server_exe" "$port" \
  ) &
  pid=$!
  server_pids+=("$pid")
  sleep 0.3
  kill -0 "$pid" 2>/dev/null || fail "server_app exited early (port=${port})"
  info "server pid=$pid port=$port instance=$instance_id"
done

info "Starting $GATEWAY_COUNT gateway_app instances"
for ((i=0; i<GATEWAY_COUNT; i++)); do
  listen_port=$((GATEWAY_BASE_PORT + i))
  metrics_port=$((GATEWAY_METRICS_BASE_PORT + i))
  gateway_id="${GATEWAY_ID_PREFIX}-${listen_port}"
  ( \
    GATEWAY_LISTEN="127.0.0.1:${listen_port}" \
    METRICS_PORT="${metrics_port}" \
    GATEWAY_ID="${gateway_id}" \
    REDIS_URI="${REDIS_URI}" \
    SERVER_REGISTRY_PREFIX="${REGISTRY_PREFIX}" \
    SERVER_REGISTRY_TTL="${REGISTRY_TTL}" \
    ALLOW_ANONYMOUS="1" \
    "$gateway_exe" \
  ) &
  pid=$!
  gateway_pids+=("$pid")
  sleep 0.3
  kill -0 "$pid" 2>/dev/null || fail "gateway_app exited early (listen=${listen_port})"
  info "gateway pid=$pid listen=127.0.0.1:${listen_port} metrics=:${metrics_port} id=${gateway_id}"
done

command -v "$HAPROXY_BIN" >/dev/null 2>&1 || fail "haproxy not found: $HAPROXY_BIN"
hap_cfg="$(mktemp -t knights-haproxy.XXXXXX.cfg)"
{
  echo "global"
  echo "  maxconn 10000"
  echo
  echo "defaults"
  echo "  mode tcp"
  echo "  timeout connect 3s"
  echo "  timeout client  60s"
  echo "  timeout server  60s"
  echo
  echo "frontend fe_gateway"
  echo "  bind 0.0.0.0:${HAPROXY_PORT}"
  echo "  default_backend be_gateway"
  echo
  echo "backend be_gateway"
  echo "  balance roundrobin"
  for ((i=0; i<GATEWAY_COUNT; i++)); do
    listen_port=$((GATEWAY_BASE_PORT + i))
    echo "  server gw$((i+1)) 127.0.0.1:${listen_port} check"
  done
} >"$hap_cfg"
info "HAProxy config: $hap_cfg"

info "Starting HAProxy on :$HAPROXY_PORT"
"$HAPROXY_BIN" -f "$hap_cfg" -db &
hap_pid=$!
sleep 0.3
kill -0 "$hap_pid" 2>/dev/null || fail "haproxy exited early"
info "haproxy pid=$hap_pid"

if [[ "$WITH_WORKER" == "1" ]]; then
  info "Starting wb_worker"
  (DB_URI="$DB_URI" REDIS_URI="$REDIS_URI" "$worker_exe") &
  worker_pid=$!
  sleep 0.3
  kill -0 "$worker_pid" 2>/dev/null || fail "wb_worker exited early"
  info "wb_worker pid=$worker_pid"
fi

info "Ready: Client -> 127.0.0.1:$HAPROXY_PORT -> gateways ($GATEWAY_COUNT) -> servers ($SERVER_COUNT)"
info "Stop: Ctrl+C"
while true; do sleep 1; done
