#!/bin/bash
set -e

CMD=$1
shift

case "$CMD" in
  "server")
    exec ./server_app "$@"
    ;;
  "worker")
    exec ./wb_worker "$@"
    ;;
  "replayer")
    exec ./wb_dlq_replayer "$@"
    ;;
  "gateway")
    exec ./gateway_app "$@"
    ;;
  "load_balancer")
    exec ./load_balancer_app "$@"
    ;;
  "migrate")
    exec ./migrations_runner "$@"
    ;;
  *)
    echo "Unknown command: $CMD"
    echo "Usage: $0 {server|worker|replayer} [args...]"
    exit 1
    ;;
esac
