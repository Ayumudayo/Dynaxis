#!/usr/bin/env bash

set -euo pipefail

COMPOSE=(
  docker compose
  --project-name dynaxis-stack
  --project-directory docker/stack
  -f docker/stack/docker-compose.yml
)

STREAM_KEY="${REDIS_STREAM_KEY:-session_events}"
TIMEOUT_SEC="${WB_SMOKE_TIMEOUT_SEC:-60}"
POLL_INTERVAL_SEC="${WB_SMOKE_POLL_INTERVAL_SEC:-2}"

log() {
  printf '[smoke_wb] %s\n' "$*"
}

wait_for_service_running() {
  local service="$1"
  local deadline=$((SECONDS + TIMEOUT_SEC))
  while (( SECONDS < deadline )); do
    if "${COMPOSE[@]}" ps --status running --services | grep -qx "$service"; then
      return 0
    fi
    sleep 1
  done
  log "service did not reach running state: ${service}"
  "${COMPOSE[@]}" ps
  return 1
}

wait_for_redis() {
  local deadline=$((SECONDS + TIMEOUT_SEC))
  while (( SECONDS < deadline )); do
    if "${COMPOSE[@]}" exec -T redis redis-cli ping >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  log "redis did not become ready"
  return 1
}

wait_for_postgres() {
  local deadline=$((SECONDS + TIMEOUT_SEC))
  while (( SECONDS < deadline )); do
    if "${COMPOSE[@]}" exec -T postgres sh -lc 'PGPASSWORD="${POSTGRES_PASSWORD:-password}" psql -U "${POSTGRES_USER:-dynaxis}" -d "${POSTGRES_DB:-dynaxis_db}" -tAc "select 1"' >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  log "postgres did not become ready"
  return 1
}

query_event_id() {
  local event_id="$1"
  local sql_event_id="${event_id//\'/\'\'}"
  local sql="select event_id from session_events where event_id = '${sql_event_id}' limit 1;"
  "${COMPOSE[@]}" exec -T postgres sh -lc "PGPASSWORD=\"\${POSTGRES_PASSWORD:-password}\" psql -U \"\${POSTGRES_USER:-dynaxis}\" -d \"\${POSTGRES_DB:-dynaxis_db}\" -tAc \"$sql\"" | tr -d '\r'
}

wait_for_service_running redis
wait_for_service_running postgres
wait_for_service_running wb_worker
wait_for_redis
wait_for_postgres

token="ci-wb-$(date +%s)-$$"
now_ms="$(date +%s%3N)"
payload="{\"note\":\"${token}\"}"

log "emitting test event into redis stream ${STREAM_KEY}"
event_id="$("${COMPOSE[@]}" exec -T redis redis-cli XADD "$STREAM_KEY" '*' \
  type session_login \
  ts_ms "$now_ms" \
  session_id 00000000-0000-0000-0000-000000000001 \
  user_id 00000000-0000-0000-0000-000000000002 \
  room_id 00000000-0000-0000-0000-000000000003 \
  payload "$payload" | tr -d '\r')"

if [[ -z "$event_id" ]]; then
  log "redis did not return an event id"
  exit 1
fi

log "waiting for persisted event ${event_id}"
deadline=$((SECONDS + TIMEOUT_SEC))
while (( SECONDS < deadline )); do
  result="$(query_event_id "$event_id" | tr -d '[:space:]')"
  if [[ "$result" == "$event_id" ]]; then
    log "event persisted to session_events"
    exit 0
  fi
  sleep "$POLL_INTERVAL_SEC"
done

log "timed out waiting for session_events row for ${event_id}"
"${COMPOSE[@]}" ps
"${COMPOSE[@]}" logs --no-color --tail 100 wb_worker redis postgres
exit 1
