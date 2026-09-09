#!/usr/bin/env bash
# Compiles the port's own stereo-depth decision logic to WebAssembly so the
# layer workbench can call the REAL C, not a JS reimplementation that could
# drift from it.
#
#   tools/layer-workbench/wasm/build.sh
#
# Needs Emscripten's emcc on PATH (https://emscripten.org/docs/getting_started/downloads.html).
# Produces tools/layer-workbench/depth_engine.js (+ .wasm, inlined as base64
# by MODULARIZE so the workbench stays a single directory to open, no extra
# fetch/CORS step over file://).
#
# Inputs are exactly the host-testable files -- no <3ds.h>, no citro3d, no
# game globals -- plus this directory's thin bridge:
#   platform/3ds/source/port_stereo_depth.c
#   platform/3ds/source/port_cutscene_depth.c
#   platform/3ds/source/port_layer_fixes.c
#   tools/layer-workbench/wasm/depth_bridge.c
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/platform/3ds/source"
OUT_DIR="$(cd "$HERE/.." && pwd)"

if ! command -v emcc >/dev/null 2>&1; then
    echo "error: emcc not found. Install Emscripten and re-run:" >&2
    echo "  https://emscripten.org/docs/getting_started/downloads.html" >&2
    echo "  (Debian/Ubuntu: apt-get install emscripten)" >&2
    exit 1
fi

EXPORTED='["_depth_bg_tier","_depth_bg_tier_for_priority","_depth_obj_tier",
"_depth_tier_px_for","_depth_tier_count","_depth_spread_count",
"_depth_cut_scene_from_game","_depth_cut_present","_depth_cut_layer_signature",
"_depth_cut_scratch","_depth_cut_scratch_capacity","_depth_cut_set_overrides",
"_depth_fix_scratch","_depth_fix_scratch_capacity","_depth_fix_set_bg",
"_depth_fix_set_room","_depth_fix_dest_for","_depth_fix_present",
"_depth_fix_active_count","_malloc","_free"]'

echo "building depth_engine.wasm from the real port_stereo_depth.c / port_cutscene_depth.c / port_layer_fixes.c ..."
emcc -O2 -std=c11 \
    -I"$SRC" \
    "$SRC/port_stereo_depth.c" \
    "$SRC/port_cutscene_depth.c" \
    "$SRC/port_layer_fixes.c" \
    "$HERE/depth_bridge.c" \
    -o "$OUT_DIR/depth_engine.js" \
    -sMODULARIZE=1 \
    -sEXPORT_NAME=DepthEngineModule \
    -sSINGLE_FILE=1 \
    -sEXPORTED_FUNCTIONS="$EXPORTED" \
    -sEXPORTED_RUNTIME_METHODS='["cwrap","HEAPU8","HEAPU16"]' \
    -sALLOW_MEMORY_GROWTH=1 \
    -sENVIRONMENT=web

echo "wrote $OUT_DIR/depth_engine.js (wasm inlined via SINGLE_FILE)"
