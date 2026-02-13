#!/usr/bin/env bash
# Deprecated wrapper.
#
# This script used to start HAProxy and multiple gateway_app instances on the host.
# The project now standardizes on Linux containers (Docker) for the server stack.
# Use docker/stack instead.

set -euo pipefail

warn(){ printf '[warn] %s\n' "$1" >&2; }

warn "scripts/run_haproxy_gateways.sh is deprecated."
warn "Use: docker compose -f docker/stack/docker-compose.yml up -d --build"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$repo_root/scripts/run_full_stack.sh" "$@"
