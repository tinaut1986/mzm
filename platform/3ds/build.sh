#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="$(tr -d '\r\n' < "${ROOT}/platform/3ds/version.txt")"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BUILD="${ROOT}/build-3ds/game"
TOOLS_ROOT="${MZM3DS_TOOLS_ROOT:-${ROOT}/../Tools/bin}"
MAKEROM="${MAKEROM:-${TOOLS_ROOT}/makerom}"
BANNERTOOL="${BANNERTOOL:-${TOOLS_ROOT}/bannertool}"

if [[ ! -x "${MAKEROM}" ]] && command -v makerom >/dev/null 2>&1; then
  MAKEROM="$(command -v makerom)"
fi
if [[ ! -x "${BANNERTOOL}" ]] && command -v bannertool >/dev/null 2>&1; then
  BANNERTOOL="$(command -v bannertool)"
fi
if [[ ! -x "${MAKEROM}" && -x "${DEVKITPRO}/tools/bin/makerom" ]]; then
  MAKEROM="${DEVKITPRO}/tools/bin/makerom"
fi
if [[ ! -x "${BANNERTOOL}" && -x "${DEVKITPRO}/tools/bin/bannertool" ]]; then
  BANNERTOOL="${DEVKITPRO}/tools/bin/bannertool"
fi

export DEVKITPRO
cmake -S "${ROOT}/platform/3ds" -B "${BUILD}" \
  -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD}" --parallel "${MZM3DS_JOBS:-4}"

if [[ ! -x "${MAKEROM}" || ! -x "${BANNERTOOL}" ]]; then
  printf '3DSX ready; makerom/bannertool are unavailable for CIA packaging.\n'
  exit 0
fi

"${BANNERTOOL}" makesmdh \
  -s "Metroid Zero Mission 3DS v${VERSION}" \
  -l "Metroid Zero Mission 3DS v${VERSION}" \
  -p "metroidret + community" \
  -i "${ROOT}/platform/3ds/assets/icon-48.png" \
  -f visible,nosavebackups \
  -o "${BUILD}/mzm-3ds.icn"

"${BANNERTOOL}" makebanner \
  -i "${ROOT}/platform/3ds/assets/banner.png" \
  -a "${ROOT}/platform/3ds/assets/banner.wav" \
  -o "${BUILD}/mzm-3ds.bnr"

(
cd "${ROOT}"
"${MAKEROM}" -f cia -o "${BUILD}/mzm-3ds-v${VERSION}.cia" \
  -DAPP_ROMFS="${BUILD#"${ROOT}/"}/romfs" \
  -rsf "${ROOT}/platform/3ds/cia/mzm3ds.rsf" -target t -exefslogo \
  -elf "${BUILD}/mzm-3ds.elf" -icon "${BUILD}/mzm-3ds.icn" \
  -banner "${BUILD}/mzm-3ds.bnr"
)

printf 'Ready:\n  %s\n  %s\n' \
  "${BUILD}/mzm-3ds-v${VERSION}.3dsx" "${BUILD}/mzm-3ds-v${VERSION}.cia"

