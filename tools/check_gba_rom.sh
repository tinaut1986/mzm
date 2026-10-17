#!/bin/sh
# Builds the GBA ROM from the committed sources in a scratch copy and compares it
# with the retail one (mzm_<region>.sha1). Needs the retail ROM next to the
# Makefile as mzm_<region>_baserom.gba (it is not in the repository), agbcc
# (tools/agbcc, or on PATH) and devkitARM. Does not touch the working tree.
#
#   tools/check_gba_rom.sh [eu|us|jp]
#
# See docs/gba-rom-build.md.
set -eu
REGION=${1:-eu}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BASEROM="$ROOT/mzm_${REGION}_baserom.gba"
[ -f "$BASEROM" ] || { echo "missing $BASEROM (the retail ROM for region $REGION)"; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
git -C "$ROOT" archive HEAD | tar -x -C "$WORK"
cp "$BASEROM" "$WORK/"

# agbcc: the NES Metroid sub-makes look for it on PATH, with libgcc.a and libc.a beside it.
if [ -d "$ROOT/tools/agbcc/bin" ]; then
    mkdir -p "$WORK/tools/agbcc" "$WORK/agbcc-path"
    cp -r "$ROOT/tools/agbcc/." "$WORK/tools/agbcc/"
    for f in "$WORK"/tools/agbcc/bin/* "$WORK"/tools/agbcc/lib/*; do ln -sf "$f" "$WORK/agbcc-path/"; done
    PATH="$WORK/agbcc-path:$PATH"
fi
[ -d /opt/devkitpro/devkitARM/bin ] && PATH="/opt/devkitpro/devkitARM/bin:$PATH"
export PATH

cd "$WORK"
make extract REGION="$REGION" >/dev/null
make -j"$(nproc)" REGION="$REGION" >"$WORK/build.log" 2>&1 || { tail -30 "$WORK/build.log"; echo "build failed"; exit 1; }

got=$(sha1sum "mzm_${REGION}.gba" | cut -d' ' -f1)
want=$(cut -d' ' -f1 "mzm_${REGION}.sha1")
if [ "$got" = "$want" ]; then
    echo "ok: mzm_${REGION}.gba matches the retail ROM ($got)"
else
    echo "MISMATCH: built $got, retail $want"
    exit 1
fi
