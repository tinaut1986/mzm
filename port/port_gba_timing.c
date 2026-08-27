/*
 * Minimal GBA scanline/VBlank timing emulation for the 3DS port.
 *
 * Real GBA hardware has a PPU that independently advances REG_VCOUNT (the
 * current scanline, 0-227: 0-159 visible, 160-227 vblank) and the
 * VBlank/HBlank bits of REG_DISPSTAT, entirely decoupled from the CPU.
 * Game code busy-waits on these registers all over the place for precise
 * timing (e.g. src/audio_wrappers.c:205-206:
 *     while (READ_8(REG_VCOUNT) == (SCREEN_SIZE_Y - 1)) {}
 *     while (READ_8(REG_VCOUNT) != (SCREEN_SIZE_Y - 1)) {}
 * ), which without a PPU updating those registers spins forever -- this was
 * the exact cause of the port hanging (unresponsive, not even to Start)
 * right after "Starting engine" on real hardware.
 *
 * This is NOT the real PPU (no pixels get rendered -- see
 * docs/3ds-port-skeleton-import.md for that still-pending work). It's just
 * a background thread body that keeps REG_VCOUNT/REG_DISPSTAT progressing
 * at approximately real GBA scanline rate, so every busy-wait on them
 * eventually completes. Frame-level pacing (Port_Bios_Halt in
 * port_bios.c, called once per agbmain() loop iteration) already ties to
 * the real 3DS vblank via gspWaitForVBlank(); this thread only needs to
 * unblock the intra-frame polling, not be cycle-accurate.
 *
 * Deliberately doesn't `#include <3ds.h>` itself (see port_bios.c for why:
 * 3ds/types.h's u32/s32/... conflict with mzm's own types.h the moment
 * both are visible in one translation unit). svcSleepThread is a single
 * s64-in-void-out syscall wrapper, low ABI risk to forward-declare; the
 * thread itself is spawned from platform_3ds_minimal.c, which is a pure
 * 3DS-side file that safely includes the real <3ds.h>.
 */
#include "port_gba_mem.h"
#include "types.h"

#include "gba/display.h"
#include "gba/interrupt.h"

extern void svcSleepThread(s64 ns);

/* GBA: 16777216 Hz CPU clock, 1232 cycles/scanline, 228 scanlines/frame
 * (160 visible + 68 vblank) -> ~59.7275 Hz refresh, ~73.35us/scanline. */
#define GBA_SCANLINES_PER_FRAME 228
#define GBA_VBLANK_START_LINE 160
#define SCANLINE_NS 73350

static volatile int sTimingThreadRunning = 1;

/* REG_BASE (and so REG_VCOUNT/REG_DISPSTAT) is `(void*)0x04000000`, not an
 * integer -- can't subtract 0x04000000 from it directly. gIoMem covers
 * 0x04000000-0x040003FF 1:1, so index by the literal in-block offsets
 * instead (0x006 and 0x004 respectively, matching gba/interrupt.h and
 * gba/display.h). */
static u8* VCountByte(void) {
    return &gIoMem[0x006];
}

static u16* DispstatWord(void) {
    return (u16*)&gIoMem[0x004];
}

/* One-shot "thread is alive" line at startup, see port/port_debug_log.h.
 * There used to be a periodic heartbeat here too, from back when a boot hang
 * had to be told apart from a dead timing thread; it was removed once that
 * was fixed -- Port_DebugLog is one fopen+fwrite+fclose (~18-20ms on real
 * hardware) and paying it from THIS thread stalls the emulated
 * VCOUNT/DISPSTAT cadence for that long, every 1.5s, forever. */
extern void Port_DebugLog(const char* msg);

void Port_GbaTiming_ThreadMain(void* arg) {
    (void)arg;
    u8 line = 0;
    Port_DebugLog("timing thread: started");
    while (sTimingThreadRunning) {
        svcSleepThread(SCANLINE_NS);

        line++;
        if (line >= GBA_SCANLINES_PER_FRAME) {
            line = 0;
        }
        *VCountByte() = line;

        u16 dispstat = *DispstatWord();
        if (line >= GBA_VBLANK_START_LINE) {
            dispstat |= DSTAT_IN_VBLANK;
        } else {
            dispstat &= ~DSTAT_IN_VBLANK;
        }
        *DispstatWord() = dispstat;
    }
}

void Port_GbaTiming_Stop(void) {
    sTimingThreadRunning = 0;
}
