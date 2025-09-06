#!/usr/bin/env bash
# 빌드/구성 스크립트 (Linux/WSL)
set -euo pipefail

GENERATOR="Unix Makefiles"
CONFIG="RelWithDebInfo"
BUILD_DIR="build-linux"
BOOST_ROOT_ENV="${BOOST_ROOT:-}"
CMD="all" # all|configure|build|clean|install|run-server|run-client
PREFIX=""
PORT=5000

usage(){
  echo "사용: build.sh [옵션]"
  echo "  -g <generator>    (기본: $GENERATOR)"
  echo "  -c <config>       (Debug/Release/RelWithDebInfo) 기본: $CONFIG"
  echo "  -b <build-dir>    (기본: $BUILD_DIR)"
  echo "  -r <command>      all|configure|build|clean|install|run-server|run-client"
  echo "  -p <prefix>       설치 경로(--install)"
  echo "  -P <port>         서버/클라이언트 포트 (기본: 5000)"
}

while getopts ":g:c:b:r:p:P:h" opt; do
  case $opt in
    g) GENERATOR="$OPTARG";;
    c) CONFIG="$OPTARG";;
    b) BUILD_DIR="$OPTARG";;
    r) CMD="$OPTARG";;
    p) PREFIX="$OPTARG";;
    P) PORT="$OPTARG";;
    h) usage; exit 0;;
    :) echo "옵션 -$OPTARG 인자 필요"; usage; exit 2;;
    \?) echo "알 수 없는 옵션 -$OPTARG"; usage; exit 2;;
  esac
done

conf(){
  ARGS=( -S . -B "$BUILD_DIR" -G "$GENERATOR" -DCMAKE_BUILD_TYPE="$CONFIG" )
  if [[ -n "$BOOST_ROOT_ENV" ]]; then
    ARGS+=( "-DBOOST_ROOT=$BOOST_ROOT_ENV" )
  fi
  cmake "${ARGS[@]}"
}

build(){
  cmake --build "$BUILD_DIR" --config "$CONFIG" -j
}

clean(){
  rm -rf "$BUILD_DIR"
}

install(){
  if [[ -z "$PREFIX" ]]; then echo "--install prefix를 -p로 지정하세요"; exit 2; fi
  cmake --install "$BUILD_DIR" --config "$CONFIG" --prefix "$PREFIX"
}

run_server(){
  exe="$BUILD_DIR/server/server_app"
  [[ -x "$exe" ]] || exe="$BUILD_DIR/server_app"
  [[ -x "$exe" ]] || { echo "server_app 실행 파일을 찾을 수 없습니다."; exit 1; }
  "$exe" "$PORT"
}

run_client(){
  exe="$BUILD_DIR/devclient/dev_chat_cli"
  [[ -x "$exe" ]] || exe="$BUILD_DIR/dev_chat_cli"
  [[ -x "$exe" ]] || { echo "dev_chat_cli 실행 파일을 찾을 수 없습니다."; exit 1; }
  "$exe" 127.0.0.1 "$PORT"
}

case "$CMD" in
  all) conf; build ;;
  configure) conf ;;
  build) build ;;
  clean) clean ;;
  install) install ;;
  run-server) run_server ;;
  run-client) run_client ;;
  *) echo "알 수 없는 명령: $CMD"; usage; exit 2;;
esac

echo "완료"

