#!/usr/bin/env bash
# Deprecated wrapper.
#
# This repo standardizes on Linux containers (Docker) for running the server stack.
# For the canonical local run (HAProxy + gateways + servers + infra), use:
#   docker compose -f docker/stack/docker-compose.yml up -d --build

set -euo pipefail

info(){ printf '[info] %s\n' "$1" >&2; }
warn(){ printf '[warn] %s\n' "$1" >&2; }
fail(){ printf '[fail] %s\n' "$1" >&2; exit 1; }

usage(){
  cat <<'EOF'
Usage: run_full_stack.sh

Deprecated: use docker stack compose instead.

  docker compose -f docker/stack/docker-compose.yml up -d --build

Optional environment variables:
  NO_BUILD=1      Skip --build
  NO_BASE=1       Skip building dynaxis-base
  NO_CACHE=1      Pass --no-cache when building dynaxis-base
  DETACHED=0      Run attached (no -d)
  PROJECT_NAME=   Override compose project name (default: dynaxis-stack)

Stop:
  docker compose -f docker/stack/docker-compose.yml down
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

warn "scripts/run_full_stack.sh is deprecated."
warn "Use: docker compose -f docker/stack/docker-compose.yml up -d --build"

command -v docker >/dev/null 2>&1 || fail "docker not found in PATH"
docker compose version >/dev/null 2>&1 || fail "docker compose is not available"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
compose_dir="$repo_root/docker/stack"
compose_file="$compose_dir/docker-compose.yml"
project_name="${PROJECT_NAME:-dynaxis-stack}"

if [[ "${NO_BUILD:-0}" != "1" && "${NO_BASE:-0}" != "1" ]]; then
  if [[ -z "$(docker images -q dynaxis-base 2>/dev/null)" ]]; then
    info "Building base image: dynaxis-base"
    build_args=(build -f "$repo_root/Dockerfile.base" -t dynaxis-base "$repo_root")
    if [[ "${NO_CACHE:-0}" == "1" ]]; then build_args+=(--no-cache); fi
    docker "${build_args[@]}"
  fi
fi

args=(compose --project-name "$project_name" --project-directory "$compose_dir" -f "$compose_file" up)
if [[ "${DETACHED:-1}" != "0" ]]; then args+=(-d); fi
if [[ "${NO_BUILD:-0}" != "1" ]]; then args+=(--build); fi

docker "${args[@]}"

info "Stack started. Connect: 127.0.0.1:6000 (HAProxy)"
info "Stop: docker compose -f docker/stack/docker-compose.yml down"
