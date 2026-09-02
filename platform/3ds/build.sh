#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Version string: single source of truth is the Makefile's VERSION (git-derived,
# see platform/3ds/Makefile GIT_VERSION). Both this cmake flow and the
# Makefile / build_3ds.py flow must name artefacts identically no matter which
# OS the build runs on. Fall back to version.txt, then a sentinel.
VERSION="$(make -C "${ROOT}/platform/3ds" -s --no-print-directory print-version 2>/dev/null | tail -n1)"
if [[ -z "${VERSION}" ]]; then
  VERSION="$(tr -d '\r\n' < "${ROOT}/platform/3ds/version.txt" 2>/dev/null || true)"
fi
VERSION="${VERSION:-0.0-dev}"
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

# Title / description / author kept identical to the Makefile (APP_TITLE,
# APP_DESCRIPTION, APP_AUTHOR) so both build flows produce the same SMDH.
SMDH_DESC="Native 3DS port of Metroid Zero Mission ${VERSION}"
SMDH_DESC="${SMDH_DESC:0:64}"
"${BANNERTOOL}" makesmdh \
  -s "Metroid Zero Mission 3DS" \
  -l "${SMDH_DESC}" \
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
"${MAKEROM}" -f cia -o "${BUILD}/mzm-3ds-${VERSION}.cia" \
  -DAPP_ROMFS="${BUILD#"${ROOT}/"}/romfs" \
  -rsf "${ROOT}/platform/3ds/cia/mzm3ds.rsf" -target t -exefslogo \
  -elf "${BUILD}/mzm-3ds.elf" -icon "${BUILD}/mzm-3ds.icn" \
  -banner "${BUILD}/mzm-3ds.bnr"
)

CIA_OUT="${BUILD}/mzm-3ds-${VERSION}.cia"
CIA_SIZE="$(du -h "${CIA_OUT}" 2>/dev/null | cut -f1)"
printf 'Build successful.\n'
printf '  CIA generated: %s (%s)\n' "$(basename "${CIA_OUT}")" "${CIA_SIZE:-unknown size}"
printf '  Saved at:      %s\n' "${CIA_OUT}"
printf '  3DSX:          %s\n' "${BUILD}/mzm-3ds-${VERSION}.3dsx"

