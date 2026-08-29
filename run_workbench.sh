#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Buscar ejecutable de Python 3
if command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="python3"
elif command -v python >/dev/null 2>&1; then
    PYTHON_BIN="python"
else
    echo "Error: No se encontró Python 3 instalado en el sistema." >&2
    exit 1
fi

echo "Iniciando Layer Workbench..."
exec "$PYTHON_BIN" "$ROOT_DIR/tools/layer-workbench/serve.py" "$@"
