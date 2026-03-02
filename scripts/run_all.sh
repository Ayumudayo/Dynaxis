#!/usr/bin/env bash
# 원클릭 실행 스크립트(WSL/Linux): 서버 + 워커(+옵션 DLQ/클라이언트) 및 스모크
set -euo pipefail

CONFIG="${1:-Debug}"
BUILDDIR="${2:-build-linux}"
PORT="${3:-5000}"
RUN_DLQ="${RUN_DLQ:-0}"
WITH_CLIENT="${WITH_CLIENT:-0}"
SMOKE="${SMOKE:-0}"

info(){ echo -e "\033[36m[info]\033[0m $*"; }
warn(){ echo -e "\033[33m[warn]\033[0m $*"; }
fail(){ echo -e "\033[31m[fail]\033[0m $*"; exit 1; }

if [[ ! -f .env ]]; then warn ".env 없음 — OS 환경변수 사용"; fi

if [[ "$WITH_CLIENT" == "1" ]]; then
  warn "WITH_CLIENT is ignored on Linux/WSL (GUI client is Windows-only)."
  WITH_CLIENT=0
fi

info "필요 타깃 빌드(server_app, wb_worker${RUN_DLQ:+, wb_dlq_replayer})"
pwsh ./scripts/build.ps1 -Config "$CONFIG" -BuildDir "$BUILDDIR" -Target server_app >/dev/null
pwsh ./scripts/build.ps1 -Config "$CONFIG" -BuildDir "$BUILDDIR" -Target wb_worker   >/dev/null
if [[ "$RUN_DLQ" == "1" ]]; then pwsh ./scripts/build.ps1 -Config "$CONFIG" -BuildDir "$BUILDDIR" -Target wb_dlq_replayer >/dev/null; fi

serverExe="$BUILDDIR/server/$CONFIG/server_app"
[[ -f "$serverExe.exe" ]] && serverExe="$BUILDDIR/server/$CONFIG/server_app.exe"
workerExe="$BUILDDIR/$CONFIG/wb_worker"
[[ -f "$workerExe.exe" ]] && workerExe="$BUILDDIR/$CONFIG/wb_worker.exe"
dlqExe="$BUILDDIR/$CONFIG/wb_dlq_replayer"; [[ -f "$dlqExe.exe" ]] && dlqExe="$BUILDDIR/$CONFIG/wb_dlq_replayer.exe"

[[ -f "$serverExe" ]] || fail "server_app 실행 파일을 찾을 수 없습니다: $serverExe"
[[ -f "$workerExe" ]] || fail "wb_worker 실행 파일을 찾을 수 없습니다: $workerExe"

info "server_app 시작 :$PORT"
"$serverExe" "$PORT" &
srv_pid=$!
sleep 0.8

info "wb_worker 시작"
"$workerExe" &
wb_pid=$!
sleep 0.8

if [[ "$RUN_DLQ" == "1" ]]; then
  info "wb_dlq_replayer 시작"
  if [[ -f "$dlqExe" ]]; then "$dlqExe" & dlq_pid=$!; fi
fi

if [[ "$SMOKE" == "1" ]]; then
  info "스모크: wb_emit → wb_check"
  pwsh ./scripts/build.ps1 -Config "$CONFIG" -BuildDir "$BUILDDIR" -Target wb_emit  >/dev/null
  pwsh ./scripts/build.ps1 -Config "$CONFIG" -BuildDir "$BUILDDIR" -Target wb_check >/dev/null
  emitExe="$BUILDDIR/$CONFIG/wb_emit"; [[ -f "$emitExe.exe" ]] && emitExe="$BUILDDIR/$CONFIG/wb_emit.exe"
  checkExe="$BUILDDIR/$CONFIG/wb_check"; [[ -f "$checkExe.exe" ]] && checkExe="$BUILDDIR/$CONFIG/wb_check.exe"
  if [[ -f "$emitExe" ]]; then
    eid=$("$emitExe" session_login | head -n1 || true)
    if [[ -n "$eid" ]]; then info "event_id=$eid"; sleep 1; [[ -f "$checkExe" ]] && "$checkExe" "$eid" && info "스모크 성공" || warn "스모크 확인 실패"; else warn "wb_emit 실패"; fi
  fi
fi

info "종료: Ctrl+C를 누르면 종료합니다."
trap 'info "종료 중..."; [[ -n "${wb_pid:-}" ]] && kill -9 $wb_pid 2>/dev/null || true; [[ -n "${dlq_pid:-}" ]] && kill -9 $dlq_pid 2>/dev/null || true; [[ -n "${srv_pid:-}" ]] && kill -9 $srv_pid 2>/dev/null || true; info "완료"; exit 0' INT TERM
wait

