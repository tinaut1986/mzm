#!/bin/bash
# Screen dumps of a scene from a running debug build, for make_package.py.
#
#   capture.sh IP OUTDIR MODE COUNT [WAIT_FRAMES] [STEP_FRAMES]
#
# Soft-resets the game (A+B+START+SELECT), waits for game mode MODE (two hex digits as
# `mzm-link.py state` prints it: 01 intro, 02 title, ...), waits WAIT_FRAMES more, then takes
# COUNT dumps STEP_FRAMES apart. Each lands in OUTDIR as NN-{io,vram,bgpltt,objpltt,oam}.bin,
# and its DISPCNT, BGnCNT and scroll registers are printed. A dump takes about a second, so a
# fast pan is caught at a few positions per run; run it again with another WAIT_FRAMES for more.
# Finding which set the dump went to reads ten small files first (a few seconds on the 2DS), so
# the dump lands that much after WAIT_FRAMES: the intro's view of Zebes was caught with 380.
# Say before running it while the owner plays: the soft reset loses unsaved progress.
set -u
IP=$1; OUT=$2; MODE=$3; COUNT=$4; WAIT=${5:-0}; STEP=${6:-0}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
L="$ROOT/tools/mzm-link/mzm-link.py --ip $IP"
CRED=$(sed -n 's/.*FTP_USER *"\(.*\)".*/\1/p' "$ROOT/platform/3ds/source/ftp_credentials_3ds.h"):$(sed -n 's/.*FTP_PASS *"\(.*\)".*/\1/p' "$ROOT/platform/3ds/source/ftp_credentials_3ds.h")
U="ftp://$IP:5000/3ds/Metroid%20Zero%20Mission%203DS/debug"
mkdir -p "$OUT"
$L keys A+B+START+SELECT 10 >/dev/null 2>&1
for _ in $(seq 1 120); do case "$($L state 2>/dev/null)" in *mode=$MODE*) break;; esac; sleep 0.3; done
if [ "$WAIT" -gt 0 ]; then $L frames "$WAIT" >/dev/null; fi
# The dump goes to one of the sets 01..10 (docs/3ds-debug-tools.md); which one is found by
# comparing each set's small io.bin before and after.
sets() { for n in 01 02 03 04 05 06 07 08 09 10; do
  printf '%s %s\n' "$n" "$(curl -s -u "$CRED" "$U/mzm-dump-$n-io.bin" | md5sum | cut -c1-32)"; done; }
for k in $(seq -w 1 "$COUNT"); do
  before=$(sets)
  $L dump >/dev/null
  set=$(diff <(echo "$before") <(sets) | sed -n 's/^> \([0-9][0-9]\) .*/\1/p' | head -1)
  if [ -z "$set" ]; then echo "$k: could not tell which set the dump went to"; continue; fi
  for f in io.bin vram.bin bgpltt.bin objpltt.bin oam.bin; do
    curl -s -f -u "$CRED" -o "$OUT/$k-$f" "$U/mzm-dump-$set-$f"
  done
  python3 -c "
import struct; io = open('$OUT/$k-io.bin', 'rb').read()
print('$k', 'dispcnt %04x' % struct.unpack_from('<H', io, 0),
      'bgcnt', ' '.join('%04x' % x for x in struct.unpack_from('<4H', io, 8)),
      'scroll', struct.unpack_from('<8H', io, 0x10))"
  if [ "$STEP" -gt 0 ]; then $L frames "$STEP" >/dev/null; fi
done
exit 0
