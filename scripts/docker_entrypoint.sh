#!/bin/bash
set -e

CMD=$1
shift

case "$CMD" in
  "server")
    ls -l ./server_app
    ldd ./server_app
    exec ./server_app "$@"
    ;;
  "worker")
    exec ./wb_worker "$@"
    ;;
  "replayer")
    exec ./wb_dlq_replayer "$@"
    ;;
  "gateway")
    ls -l ./gateway_app
    ldd ./gateway_app
    exec ./gateway_app "$@"
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
