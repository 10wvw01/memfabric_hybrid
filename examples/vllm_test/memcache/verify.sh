#!/usr/bin/env bash
# Verify source revisions and the artifacts loaded by the current Python environment.
set -euo pipefail
readonly PKG_DIR="$(cd "$(dirname "$0")" && pwd)"
python3 "$PKG_DIR/source_manifest.py" verify "$@"
