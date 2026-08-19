#!/usr/bin/env bash
# Build the 3DS CIA and run it in Azahar (flatpak) headlessly on this machine
# for quick local iteration, without touching real hardware over FTP.
#
# Usage:
#   tools/run_azahar_test.sh [seconds] [--no-build] [--no-audio-dump]
#
# What it does:
#   1. make in platform/3ds (unless --no-build)
#   2. kills any running Azahar instance
#   3. clears the previous debug log / audio dump on Azahar's virtual SD
#      (mzm-debug.log is opened in append mode by the port, and
#      mzm_audio_dump.bin in "ab" too -- stale data from earlier runs
#      would otherwise leak into this run's results, see docs/3ds-port-status-2026-08-17.md 6k)
#   4. launches Azahar directly on the built CIA for <seconds> (default 20)
#   5. prints the debug log and, if requested, converts the raw PCM dump to
#      a WAV + spectrogram PNG under /tmp for visual inspection (same
#      methodology as the 2026-08-19 audio session, see docs/3ds-port-status-2026-08-17.md 6k)
#
# Requires: flatpak (org.azahar_emu.Azahar installed), ffmpeg (for --audio-dump).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDMC="$HOME/.var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc"
CIA="$REPO_ROOT/platform/3ds/mzm-3ds.cia"

# UniqueId from platform/3ds/cia/mzm3ds.rsf (0x01986) -> title ID 0004000000198600.
# Booting the installed content's .app file directly (found under Azahar's
# virtual SD, real-3DS layout) avoids the interactive "install this CIA?"
# dialog that `-f <raw .cia path>` triggers even when already installed.
TITLE_CONTENT_DIR="$SDMC/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000/00198600/content"

SECONDS_TO_RUN=20
DO_BUILD=1
DO_AUDIO_DUMP=1

for arg in "$@"; do
    case "$arg" in
        --no-build) DO_BUILD=0 ;;
        --no-audio-dump) DO_AUDIO_DUMP=0 ;;
        [0-9]*) SECONDS_TO_RUN="$arg" ;;
        *) echo "Unknown arg: $arg" >&2; exit 1 ;;
    esac
done

if [[ "$DO_BUILD" == "1" ]]; then
    echo "==> Building 3DS CIA"
    (
        cd "$REPO_ROOT/platform/3ds"
        export DEVKITPRO=/opt/devkitpro
        export DEVKITARM=/opt/devkitpro/devkitARM
        export PATH="$DEVKITARM/bin:$HOME/tools/bin:$PATH"
        make
    )
fi

if [[ ! -f "$CIA" ]]; then
    echo "CIA not found at $CIA (build it first, or drop --no-build)" >&2
    exit 1
fi

kill_azahar() {
    pkill -9 -x azahar 2>/dev/null || true
    pkill -9 -f azahar-launcher 2>/dev/null || true
}

echo "==> Killing any running Azahar instance"
kill_azahar
sleep 1

echo "==> Clearing previous log / audio dump (both are opened append-only by the port)"
rm -f "$SDMC/3ds/mzm-debug.log" "$SDMC/3ds/mzm_audio_dump.bin"

echo "==> Installing CIA (silent, no dialog)"
DISPLAY="${DISPLAY:-:0}" flatpak run org.azahar_emu.Azahar -i "$CIA" \
    > /tmp/azahar_test_install.log 2>&1
grep -qi "Installed CIA successfully" /tmp/azahar_test_install.log \
    || { echo "CIA install failed, see /tmp/azahar_test_install.log" >&2; cat /tmp/azahar_test_install.log >&2; exit 1; }

APP_FILE="$(find "$TITLE_CONTENT_DIR" -maxdepth 1 -iname '*.app' | head -1)"
if [[ -z "$APP_FILE" ]]; then
    echo "No installed .app content found under $TITLE_CONTENT_DIR (install step above should have created it)" >&2
    exit 1
fi

echo "==> Launching Azahar on $APP_FILE for ${SECONDS_TO_RUN}s"
DISPLAY="${DISPLAY:-:0}" flatpak run org.azahar_emu.Azahar -f "$APP_FILE" \
    > /tmp/azahar_test_run.log 2>&1 &
AZAHAR_LAUNCHER_PID=$!

sleep "$SECONDS_TO_RUN"

echo "==> Taking screenshot"
spectacle -b -n -o /tmp/azahar_test_screenshot_full.png >/dev/null 2>&1 || true
# Multi-monitor setup: Azahar renders on the first (leftmost) display only.
# Crop to it so callers don't have to read/ship the whole multi-monitor image.
ffmpeg -y -i /tmp/azahar_test_screenshot_full.png -vf "crop=1920:1080:0:0" \
    -frames:v 1 /tmp/azahar_test_screenshot.png >/dev/null 2>&1 || cp /tmp/azahar_test_screenshot_full.png /tmp/azahar_test_screenshot.png

echo "==> Stopping Azahar"
kill_azahar
wait "$AZAHAR_LAUNCHER_PID" 2>/dev/null || true

echo
echo "===== mzm-debug.log ====="
if [[ -f "$SDMC/3ds/mzm-debug.log" ]]; then
    cat "$SDMC/3ds/mzm-debug.log"
else
    echo "(no log produced)"
fi

if [[ "$DO_AUDIO_DUMP" == "1" && -f "$SDMC/3ds/mzm_audio_dump.bin" ]]; then
    echo
    echo "==> Converting audio dump to WAV + spectrogram"
    # Raw dump is interleaved u8 stereo PCM at the engine's native rate
    # (see MZM_AUDIO_OUT_RATE in port/port_mzm_audio_glue.h) as captured
    # right before it's handed to NDSP.
    RATE="$(grep -oP '(?<=#define MZM_AUDIO_OUT_RATE )\d+' "$REPO_ROOT/port/port_mzm_audio_glue.h" || echo 16364)"
    ffmpeg -y -f u8 -ar "$RATE" -ac 2 -i "$SDMC/3ds/mzm_audio_dump.bin" \
        /tmp/azahar_test_dump.wav >/tmp/azahar_test_ffmpeg.log 2>&1 || true
    ffmpeg -y -i /tmp/azahar_test_dump.wav -lavfi showspectrumpic=s=1024x512 \
        /tmp/azahar_test_spectrogram.png >>/tmp/azahar_test_ffmpeg.log 2>&1 || true
    echo "WAV: /tmp/azahar_test_dump.wav"
    echo "Spectrogram: /tmp/azahar_test_spectrogram.png"
fi

echo
echo "Screenshot: /tmp/azahar_test_screenshot.png"
