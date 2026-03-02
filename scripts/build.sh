#!/usr/bin/env bash
# Linux/WSL build helper for Knights
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GENERATOR="Unix Makefiles"
CONFIG="RelWithDebInfo"
BUILD_DIR="build-linux"
CMD="all"
TARGET=""
PREFIX=""
PORT=5000
RELEASE_DIR=""
RELEASE_LIST="server_app,gateway_app,dev_chat_cli,wb_worker"
RELEASE_ARCHIVE=""

usage(){
  cat <<EOF
Usage: build.sh [options]
  -g <generator>    (default: $GENERATOR)
  -c <config>       Debug/Release/RelWithDebInfo (default: $CONFIG)
  -b <build-dir>    (default: $BUILD_DIR)
  -r <command>      all|configure|build|clean|install|run-server|run-client
  -t <target>       Build target name (default: all/ALL_BUILD)
  -p <prefix>       Install prefix (cmake --install)
  -P <port>         Server/client port (default: 5000)
  -R <dir>          Output directory for release bundle
  -L <names>        Comma list of binaries for release bundle
  -z <archive>      Release tar.gz path
EOF
}

while getopts ":g:c:b:r:p:P:t:R:L:z:h" opt; do
  case $opt in
    g) GENERATOR="$OPTARG";;
    c) CONFIG="$OPTARG";;
    b) BUILD_DIR="$OPTARG";;
    r) CMD="$OPTARG";;
    p) PREFIX="$OPTARG";;
    P) PORT="$OPTARG";;
    t) TARGET="$OPTARG";;
    R) RELEASE_DIR="$OPTARG";;
    L) RELEASE_LIST="$OPTARG";;
    z) RELEASE_ARCHIVE="$OPTARG";;
    h) usage; exit 0;;
    :) echo "Option -$OPTARG requires a value" >&2; usage; exit 2;;
    \?) echo "Unknown option -$OPTARG" >&2; usage; exit 2;;
  esac
done

conf(){
  ARGS=( -S . -B "$BUILD_DIR" -G "$GENERATOR" -DCMAKE_BUILD_TYPE="$CONFIG" )
  cmake "${ARGS[@]}"
}

build(){
  local t="$TARGET"
  if [[ -z "$t" ]]; then
    if [[ "$GENERATOR" == *"Visual Studio"* ]]; then t="ALL_BUILD"; else t="all"; fi
  fi
  cmake --build "$BUILD_DIR" --config "$CONFIG" --target "$t" -j
}

clean(){
  rm -rf "$BUILD_DIR"
}

install(){
  [[ -n "$PREFIX" ]] || { echo "-p <prefix> required" >&2; exit 2; }
  cmake --install "$BUILD_DIR" --config "$CONFIG" --prefix "$PREFIX"
}

run_server(){
  exe="$BUILD_DIR/server/server_app"
  [[ -x "$exe" ]] || exe="$BUILD_DIR/server_app"
  [[ -x "$exe" ]] || { echo "server_app 실행 파일을 찾을 수 없습니다." >&2; exit 1; }
  "$exe" "$PORT"
}

run_client(){
  exe="$BUILD_DIR/devclient/dev_chat_cli"
  [[ -x "$exe" ]] || exe="$BUILD_DIR/dev_chat_cli"
  [[ -x "$exe" ]] || { echo "dev_chat_cli 실행 파일을 찾을 수 없습니다." >&2; exit 1; }
  "$exe" 127.0.0.1 "$PORT"
}

find_binary(){
  local name="$1"
  local -a candidates=(
    "$BUILD_DIR/$name"
    "$BUILD_DIR/$CONFIG/$name"
  )
  for sub in server gateway load_balancer devclient tools wb; do
    candidates+=("$BUILD_DIR/$sub/$name" "$BUILD_DIR/$sub/$CONFIG/$name")
  done
  for path in "${candidates[@]}"; do
    [[ -f "$path" ]] && { echo "$path"; return 0; }
  done
  local found
  found=$(find "$BUILD_DIR" -type f -name "$name" -print -quit 2>/dev/null || true)
  [[ -n "$found" ]] && { echo "$found"; return 0; }
  return 1
}

release_bundle(){
  [[ -n "$RELEASE_DIR" ]] || return 0
  mkdir -p "$RELEASE_DIR"
  local folder="$RELEASE_DIR/release-$CONFIG"
  rm -rf "$folder"
  mkdir -p "$folder"
  IFS=',' read -ra names <<< "$RELEASE_LIST"
  for raw in "${names[@]}"; do
    local name="$(echo "$raw" | xargs)"
    [[ -z "$name" ]] && continue
    local bin
    if bin=$(find_binary "$name"); then
      cp "$bin" "$folder/"
    else
      echo "[warn] release 바이너리를 찾지 못했습니다: $name" >&2
    fi
  done
  if [[ -n "$RELEASE_ARCHIVE" ]]; then
    tar -C "$folder" -czf "$RELEASE_ARCHIVE" .
    echo "[info] release archive: $RELEASE_ARCHIVE"
  fi
  echo "[info] release bundle: $folder"
}

case "$CMD" in
  all) conf; build ;;
  configure) conf ;;
  build) build ;;
  clean) clean ;;
  install) install ;;
  run-server) run_server ;;
  run-client) run_client ;;
  *) echo "알 수 없는 command: $CMD" >&2; usage; exit 2 ;;
esac

if [[ -n "$RELEASE_DIR" ]]; then
  if [[ "$CMD" == "all" || "$CMD" == "build" ]]; then
    release_bundle
  else
    echo "[warn] release 옵션은 all/build 명령과 함께만 동작" >&2
  fi
fi

echo "완료"
