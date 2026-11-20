#!/usr/bin/env bash
# Same native build on Linux, macOS and Windows: python scripts/build.py --help.
set -euo pipefail
exec python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build.py" "$@"
