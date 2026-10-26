#!/bin/bash
# Host test of the DSP firmware validator (platform/3ds/source/dsp_firm.c): a synthetic firmware with real SHA-256
# hashes, then damaged copies. No ROM, no console. Usage: tools/dsp-firm-test/run.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-/tmp/mzm-dsp-firm-test}
mkdir -p "$WORK"
gcc -O1 -g -Wall -Wextra -Werror -I"$ROOT/platform/3ds/source" "$ROOT/platform/3ds/source/dsp_firm.c" "$ROOT/tools/dsp-firm-test/dsp_firm_test.c" \
    -o "$WORK/dsp_firm_test"
"$WORK/dsp_firm_test" "${1:-}"
