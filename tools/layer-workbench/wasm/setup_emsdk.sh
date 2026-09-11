#!/usr/bin/env bash
# Bootstrap a repo-local Emscripten toolchain under wasm/emsdk/ so the layer
# workbench can compile depth_engine.wasm without a system-wide install.
#
#   tools/layer-workbench/wasm/setup_emsdk.sh
#
# Idempotent: clones emsdk on first run, then installs + activates "latest".
# serve.py picks the local emsdk up automatically (see use_local_emsdk()).
# The emsdk/ tree is git-ignored.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EMSDK_DIR="$HERE/emsdk"

if ! command -v git >/dev/null 2>&1; then
    echo "error: git not on PATH" >&2
    exit 1
fi

if [ ! -f "$EMSDK_DIR/emsdk" ]; then
    echo "cloning emsdk into $EMSDK_DIR ..."
    git clone https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi

cd "$EMSDK_DIR"
./emsdk install latest
./emsdk activate latest

echo
echo "local emsdk ready. run_workbench.bat / serve.py will use it automatically."
