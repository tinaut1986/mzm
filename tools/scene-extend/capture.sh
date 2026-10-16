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
[ "$WAIT" -gt 0 ] && $L frames "$WAIT" >/dev/null
for k in $(seq -w 1 "$COUNT"); do
  $L dump >/dev/null
  # Every new dump has landed in set 01 so far (the SD files all carry the same date, so the
  # oldest-set pick falls on 01); the registers printed below say whether it is this moment.
  for f in io.bin vram.bin bgpltt.bin objpltt.bin oam.bin; do
    curl -s -f -u "$CRED" -o "$OUT/$k-$f" "$U/mzm-dump-01-$f"
  done
  python3 -c "
import struct; io = open('$OUT/$k-io.bin', 'rb').read()
print('$k', 'dispcnt %04x' % struct.unpack_from('<H', io, 0),
      'bgcnt', ' '.join('%04x' % x for x in struct.unpack_from('<4H', io, 8)),
      'scroll', struct.unpack_from('<8H', io, 0x10))"
  [ "$STEP" -gt 0 ] && $L frames "$STEP" >/dev/null
done
