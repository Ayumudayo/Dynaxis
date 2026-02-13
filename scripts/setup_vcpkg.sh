#!/usr/bin/env bash
# Knights vcpkg bootstrap helper (Linux/WSL/macOS)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VCPKG_ROOT="$REPO_ROOT/external/vcpkg"
VCPKG_REPO="${VCPKG_REPO:-https://github.com/microsoft/vcpkg.git}"
TRIPLET=""
SKIP_INSTALL=0

usage(){
  cat <<'EOF'
Usage: scripts/setup_vcpkg.sh [options]
  -t, --triplet <name>     Triplet to install (default: x64-linux or detected)
      --skip-install       Skip running 'vcpkg install'
      --repo <url>         Override vcpkg git repository
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -t|--triplet) TRIPLET="$2"; shift 2 ;;
    --skip-install) SKIP_INSTALL=1; shift ;;
    --repo) VCPKG_REPO="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ -z "$TRIPLET" ]]; then
  TRIPLET="x64-linux"
fi

info(){ printf '[info] %s\n' "$1" >&2; }
fail(){ printf '[fail] %s\n' "$1" >&2; exit 1 ; }

if [[ ! -d "$VCPKG_ROOT/.git" ]]; then
  info "Cloning vcpkg into $VCPKG_ROOT"
  # vcpkg manifest versioning(builtin-baseline)은 과거 커밋/트리를 필요로 하므로 shallow clone을 피한다.
  git clone "$VCPKG_REPO" "$VCPKG_ROOT"
else
  info "Using existing vcpkg checkout: $VCPKG_ROOT"
  if [[ -f "$VCPKG_ROOT/.git/shallow" ]]; then
    info "vcpkg shallow clone detected; fetching full history"
    (cd "$VCPKG_ROOT" && git fetch --unshallow)
  fi
fi

VCPKG_EXE="$VCPKG_ROOT/vcpkg"
if [[ ! -x "$VCPKG_EXE" ]]; then
  BOOTSTRAP="$VCPKG_ROOT/bootstrap-vcpkg.sh"
  [[ -x "$BOOTSTRAP" ]] || fail "Cannot find $BOOTSTRAP"
  info "Bootstrapping vcpkg..."
  (cd "$VCPKG_ROOT" && bash "$BOOTSTRAP")
fi

if [[ "$SKIP_INSTALL" -eq 0 ]]; then
  info "Installing manifest dependencies for $TRIPLET"
  "$VCPKG_EXE" install --triplet "$TRIPLET"
fi

printf '%s\n' "$VCPKG_ROOT"
