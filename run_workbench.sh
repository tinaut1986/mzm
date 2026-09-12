#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Look for a Python 3 interpreter
if command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="python3"
elif command -v python >/dev/null 2>&1; then
    PYTHON_BIN="python"
else
    echo "Error: no Python 3 interpreter found on this system." >&2
    exit 1
fi

# --- WASM depth engine: provision a local Emscripten on first run -----------
# serve.py already rebuilds depth_engine.js whenever emcc is available; here we
# only make sure it is. A system-wide emcc takes precedence.
WASM_DIR="$ROOT_DIR/tools/layer-workbench/wasm"
if ! command -v emcc >/dev/null 2>&1 && [ ! -x "$WASM_DIR/emsdk/upstream/emscripten/emcc" ]; then
    echo
    echo "The WASM depth engine needs Emscripten and it cannot be found."
    echo "It can be installed inside the repo (tools/layer-workbench/wasm/emsdk, ~1 GB)."
    if [ -t 0 ]; then
        read -r -p "Install it now? [y/N] " reply
        case "$reply" in
            [yYsS]) "$WASM_DIR/setup_emsdk.sh" ;;
            *) echo "Skipping -- the workbench still runs, without the depth engine." ;;
        esac
    else
        echo "Not a terminal -- skipping. Run $WASM_DIR/setup_emsdk.sh to install it."
    fi
fi

echo "Starting Layer Workbench..."
exec "$PYTHON_BIN" "$ROOT_DIR/tools/layer-workbench/serve.py" "$@"
