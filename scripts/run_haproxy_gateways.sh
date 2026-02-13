#!/usr/bin/env bash
# Local dev helper: run HAProxy + multiple gateway_app instances.
set -euo pipefail

CONFIG="${CONFIG:-Debug}"
BUILD_DIR="${BUILD_DIR:-build-linux}"
GATEWAY_COUNT="${GATEWAY_COUNT:-2}"
HAPROXY_PORT="${HAPROXY_PORT:-6000}"
GATEWAY_BASE_PORT="${GATEWAY_BASE_PORT:-6101}"
METRICS_BASE_PORT="${METRICS_BASE_PORT:-6201}"
GATEWAY_ID_PREFIX="${GATEWAY_ID_PREFIX:-gateway-local}"
HAPROXY_BIN="${HAPROXY_BIN:-haproxy}"
NO_DOTENV="${NO_DOTENV:-0}"
NO_BUILD="${NO_BUILD:-0}"

info(){ printf '[info] %s\n' "$1" >&2; }
warn(){ printf '[warn] %s\n' "$1" >&2; }
fail(){ printf '[fail] %s\n' "$1" >&2; exit 1; }

usage(){
  cat <<EOF
Usage: run_haproxy_gateways.sh [options]

Environment variables (optional):
  CONFIG, BUILD_DIR, GATEWAY_COUNT, HAPROXY_PORT, GATEWAY_BASE_PORT, METRICS_BASE_PORT,
  GATEWAY_ID_PREFIX, HAPROXY_BIN, NO_DOTENV=1, NO_BUILD=1

Example:
  CONFIG=Debug BUILD_DIR=build-linux GATEWAY_COUNT=2 ./scripts/run_haproxy_gateways.sh
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then usage; exit 0; fi

if [[ "$NO_DOTENV" != "1" && -f ./.env ]]; then
  info "Loading env file: ./.env"
  # NOTE: .env must be POSIX-shell compatible for this to work.
  set -a
  # shellcheck disable=SC1091
  . ./.env
  set +a
fi

if [[ "$NO_BUILD" != "1" ]]; then
  info "Build gateway_app ($CONFIG, $BUILD_DIR)"
  ./scripts/build.sh -c "$CONFIG" -b "$BUILD_DIR" -r all -t gateway_app >/dev/null
fi

GATEWAY_EXE="$BUILD_DIR/gateway/gateway_app"
[[ -x "$GATEWAY_EXE" ]] || GATEWAY_EXE="$BUILD_DIR/gateway_app"
[[ -x "$GATEWAY_EXE" ]] || fail "gateway_app executable not found under $BUILD_DIR"

command -v "$HAPROXY_BIN" >/dev/null 2>&1 || fail "haproxy not found: $HAPROXY_BIN"

if ! [[ "$GATEWAY_COUNT" =~ ^[0-9]+$ ]] || [[ "$GATEWAY_COUNT" -lt 1 ]]; then
  fail "GATEWAY_COUNT must be >= 1"
fi

tmp_cfg=""
hap_pid=""
declare -a gw_pids=()

cleanup(){
  set +e
  if [[ -n "$hap_pid" ]]; then kill "$hap_pid" 2>/dev/null || true; fi
  for pid in "${gw_pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  if [[ -n "$tmp_cfg" && -f "$tmp_cfg" ]]; then rm -f "$tmp_cfg" || true; fi
}
trap cleanup EXIT INT TERM

info "Starting $GATEWAY_COUNT gateway_app instances"
for ((i=0; i<GATEWAY_COUNT; i++)); do
  listen_port=$((GATEWAY_BASE_PORT + i))
  metrics_port=$((METRICS_BASE_PORT + i))
  gateway_id="${GATEWAY_ID_PREFIX}-${listen_port}"

  ( \
    GATEWAY_LISTEN="127.0.0.1:${listen_port}" \
    METRICS_PORT="${metrics_port}" \
    GATEWAY_ID="${gateway_id}" \
    "$GATEWAY_EXE" \
  ) &
  pid=$!
  gw_pids+=("$pid")
  info "gateway pid=$pid listen=127.0.0.1:${listen_port} metrics=:${metrics_port} id=${gateway_id}"
  sleep 0.2
  kill -0 "$pid" 2>/dev/null || fail "gateway_app exited early (listen=${listen_port})"
done

tmp_cfg="$(mktemp -t knights-haproxy.XXXXXX.cfg)"
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
} >"$tmp_cfg"
info "HAProxy config: $tmp_cfg"

info "Starting HAProxy on :$HAPROXY_PORT"
"$HAPROXY_BIN" -f "$tmp_cfg" -db &
hap_pid=$!
sleep 0.2
kill -0 "$hap_pid" 2>/dev/null || fail "haproxy exited early"
info "haproxy pid=$hap_pid"

info "Ready: Client -> 127.0.0.1:$HAPROXY_PORT -> gateways (count=$GATEWAY_COUNT)"
info "Stop: Ctrl+C"
while true; do sleep 1; done
