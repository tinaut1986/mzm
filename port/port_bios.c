/*
 * Clean-room C reimplementation of the GBA BIOS routines mzm calls as plain
 * functions (CpuSet, Div, LZ77 decompression, Sqrt, ...) via the thin
 * asm/syscalls.s trampolines (`swi SYSCALL_X; bx lr`). Real hardware `swi`
 * traps into the GBA BIOS; on 3DS the same instruction traps into Horizon
 * OS's own syscall table instead, which does something unrelated (and,
 * depending on the immediate, may be flatly illegal from user mode) --
 * silently "compiles fine, corrupts memory or crashes on real hardware"
 * territory, not a build error. asm/syscalls.s is excluded from the 3DS
 * build; this file provides the same symbols.
 *
 * Algorithms here follow the public GBA BIOS behavior as documented by
 * GBATek (the community hardware reference) -- register-level semantics,
 * not decompiled/copied code.
 */
#include "types.h"

#include <string.h>
#include <stdlib.h>

s32 DivarmDiv(s32 num, s32 denom) {
    return denom != 0 ? num / denom : 0;
}

s32 DivarmMod(s32 num, s32 denom) {
    return denom != 0 ? num % denom : 0;
}

/* Signatures below match include/syscalls.h exactly (reproduced verbatim
 * in the 3DS shadow copy, port/generated/shadow/syscalls.h) -- every
 * gameplay .c still includes that header for these prototypes, so a
 * mismatch here would be a hard compile error, not just a style nit. */

u16 Sqrt(u32 value) {
    u32 result = 0;
    u32 bit = 1u << 30;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        u32 candidate = result + bit;
        if (value >= candidate) {
            value -= candidate;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

/* ctrl bit layout (GBATek "3.4 CpuSet/CpuFastSet"):
 *   0-20  word count
 *   24    datasize: 0 = 16-bit, 1 = 32-bit (ignored by CpuFastSet, always 32)
 *   25    fixed source: 0 = copy, 1 = fill (repeat src[0]) */
#define CPUSET_COUNT_MASK 0x001FFFFFu
#define CPUSET_32BIT (1u << 24)
#define CPUSET_FIXED_SRC (1u << 25)

#include "port_gba_mem.h"

void CpuSet(void* src, void* dst, u32 ctrl) {
    u32 count = ctrl & CPUSET_COUNT_MASK;
    int fill = (ctrl & (CPUSET_FIXED_SRC | (1u << 24))) != 0;
    u32 i;

    void* d = port_resolve_write_addr((uintptr_t)dst);
    const void* s = port_resolve_copy_src(src, count * 4);
    if (!d || !s) return;

    if (ctrl & CPUSET_32BIT) {
        const u32* src32 = (const u32*)s;
        u32* dst32 = (u32*)d;
        for (i = 0; i < count; i++) {
            dst32[i] = fill ? src32[0] : src32[i];
        }
    } else {
        const u16* src16 = (const u16*)s;
        u16* dst16 = (u16*)d;
        for (i = 0; i < count; i++) {
            dst16[i] = fill ? src16[0] : src16[i];
        }
    }
}

void CpuFastSet(void* src, void* dst, u32 ctrl) {
    u32 count = (((u32)ctrl & CPUSET_COUNT_MASK) + 7u) & ~7u;
    int fill = (ctrl & (CPUSET_FIXED_SRC | (1u << 24))) != 0;
    u32 i;

    void* d = port_resolve_write_addr((uintptr_t)dst);
    const void* s = port_resolve_copy_src(src, count * 4);
    if (!d || !s) return;

    const u32* src32 = (const u32*)s;
    u32* dst32 = (u32*)d;
    for (i = 0; i < count; i++) {
        dst32[i] = fill ? src32[0] : src32[i];
    }
}

/* Standard GBA LZ77 stream: 4-byte header (0x10, then 24-bit LE
 * decompressed size), followed by blocks of one flag byte (MSB-first, 1 bit
 * per token) and tokens:
 *   flag bit 0 -> literal byte, copied as-is
 *   flag bit 1 -> back-reference: 2 bytes, big-endian-ish packed as
 *                 (length-3):4 bits high nibble, (distance-1):12 bits
 * Real hardware processes VRAM in 16-bit units because the GBA's VRAM bus
 * doesn't support 8-bit writes; our "VRAM" is a plain RAM buffer (see
 * port_gba_mem.c), so both variants share one byte-addressable
 * implementation -- there's no bus-width hazard to work around here. */
static void Lz77Uncomp(const u8* src, u8* dst) {
    const u8* s = (const u8*)port_resolve_copy_src(src, 4);
    u8* d = (u8*)port_resolve_write_addr((uintptr_t)dst);
    int bit;
    u32 i;
    if (!s || !d) return;

    u32 header = (u32)s[0] | ((u32)s[1] << 8) | ((u32)s[2] << 16) | ((u32)s[3] << 24);
    u32 decompressedSize = header >> 8;
    s += 4;

    u8* out = d;
    u8* outEnd = d + decompressedSize;

    while (out < outEnd) {
        u8 flags = *s++;
        for (bit = 7; bit >= 0 && out < outEnd; bit--) {
            if ((flags & (1 << bit)) == 0) {
                *out++ = *s++;
            } else {
                u8 b0 = *s++;
                u8 b1 = *s++;
                u32 length = (u32)(b0 >> 4) + 3;
                u32 distance = (((u32)(b0 & 0xF) << 8) | b1) + 1;
                const u8* copySrc = out - distance;
                for (i = 0; i < length && out < outEnd; i++) {
                    *out++ = *copySrc++;
                }
            }
        }
    }
}

void LZ77UncompVram(const void* src, void* dst) {
    Lz77Uncomp((const u8*)src, (u8*)dst);
}

void LZ77UncompWram(const void* src, void* dst) {
    Lz77Uncomp((const u8*)src, (u8*)dst);
}

/* Multiplayer boot: no multiplayer on this port, always report failure. */
s32 Multiboot(void* mbp) {
    (void)mbp;
    return 1;
}

/* GBA analog sound DAC bias ramp -- no equivalent on 3DS's own audio
 * hardware, safe no-ops. */
void SoundBias0(void) {}
void SoundBias200(void) {}

/* Real MidiKey2Freq semantics: calculates sample playback frequency in Hz
 * given a ToneData/WaveData pointer, MIDI key (0-127, 60=middle C), and
 * fine tune adjust (0-255). waveData[1] is the base frequency stored in
 * the sample header in 1024ths of a Hz (i.e. Hz * 1024). */
u32 MidiKey2Freq(u32* waveData, u8 midiKey, u8 fineAdjust) {
    if (!waveData)
        return 0;

    const u32* wd = (const u32*)GBA_RESOLVE((const void*)waveData);
    u32 baseFreq = wd[1]; // base frequency stored as (Hz * 1024)
    if (baseFreq == 0)
        return 0;

    double semitones = (double)(midiKey - 60) + (double)fineAdjust / 256.0;
    double freq = ((double)baseFreq * __builtin_exp2(semitones / 12.0)) / 1024.0;
    return (u32)(freq + 0.5);
}

extern void CallbackCallVblank(void);

/* Deliberately not `#include <3ds.h>`: 3ds/types.h typedefs u32/s32/vu32/...
 * as (distinct, same-size) aliases of the stdint types, which conflicts
 * with mzm's own types.h (typedef'd from the plain C integer types) the
 * moment both are visible in one translation unit. Forward-declare just the
 * one libctru function actually needed instead of pulling in the whole
 * header -- real signature per libctru's gspgpu.h. */
/* gspWaitForEvent forward declaration (GSPGPU_EVENT_VBlank0 = 0). Currently
 * unused below -- see the "temporarily" comment at the Port_Bios_Halt call
 * site -- kept forward-declared for when real GPU presentation work makes
 * genuine vblank sync necessary again. */
extern void gspWaitForEvent(int event, bool nextEvent);
extern void svcSleepThread(long long ns);

/* aptMainLoop forward declaration. Returns false once the user requests the
 * app close (HOME menu "Close", sleep-then-close, power button, ...). Never
 * having called this meant the app couldn't respond to HOME at all -- the
 * only way out was powering off the console. Also pumps APT's internal
 * event queue, which real hardware needs serviced regularly for good
 * cooperative behavior with the system (untested whether this is also
 * needed for gspWaitForEvent to ever unblock -- see the call site). */
extern bool aptMainLoop(void);
extern void gfxExit(void);

/* Synchronizes the GBA-side cooperative loop to real 60Hz vblank timing.
 * agbmain()'s frame loop is:
 *     do { SYSCALL(2); } while (!(gVBlankRequestFlag & 1));
 * On real hardware, SYSCALL(2) (Halt) suspends the CPU until any interrupt
 * fires; the vblank ISR (CallbackCallVblank, src/callbacks.c) then sets
 * gVBlankRequestFlag, and the loop re-checks and exits. There's no
 * interrupt controller here to fire that ISR automatically, so this
 * function does both halves itself: wait for the real vblank, then invoke
 * the same ISR body directly. include/syscalls.h is shadowed (see
 * port/generated/shadow/syscalls.h) to route SYSCALL(num) here for every
 * num -- the two calls to SYSCALL(2)/(3) that exist across the whole
 * codebase both use it as a wait-for-next-vblank point, not to
 * differentiate GBA's Halt from Stop. */
#if defined(PLATFORM_LINUX)
extern void Platform_Linux_VBlank(void);
#endif

#ifdef MZM_3DS
extern void Port_DebugLog(const char* msg);
extern void Port_PPU_RenderFrame(void);
extern void Platform3DS_PollKeysIntoGba(void);
extern void PlatformGpu3DS_RecordTick(void);
extern void PlatformGpu3DS_PerfRecordTick(void);
extern u64 Platform3DS_SystemTick(void);
extern u64 Platform3DS_TicksPerSecond(void);

/* Guards gMusicInfo/TrackData against the audio thread's own production
 * ticks (see platform/3ds/source/port_mzm_audio_3ds.c's doc comment on
 * sAudioStateLock). Released for the render+pace work below, which never
 * touches audio state, so the audio thread gets a real window to run in
 * every VBlankIntrWait() call regardless of how deep in game logic it was
 * called from (src/transfer.c calls it directly mid-frame, not just
 * agbmain's own loop -- see Port_PPU_RenderFrame's reentrancy comment). */
extern void Port_AudioStateLock_Acquire(void);
extern void Port_AudioStateLock_Release(void);

/* Real GBA frame period: 228 scanlines * 73350ns/scanline (see
 * port/port_gba_timing.c for where that constant comes from) ~= 59.79Hz.
 * Port_PPU_RenderFrame() no longer blocks on the GPU (see
 * platform/3ds/source/port_ppu_mzm.c's Port_PPU_RenderFrame/
 * Port_PPU_GpuPresentPump split), so nothing paces agbmain()'s loop -- and
 * with it game logic and audio production -- to real time anymore unless
 * this does it explicitly. Deadline-accumulates instead of sleeping a fixed
 * amount every call, so small per-call jitter doesn't drift the average
 * rate; resyncs instead of burning through a backlog if a single iteration
 * falls more than one frame behind (a debugger breakpoint, a slow SD
 * write, ...), rather than free-running to catch up. */
#define PORT_BIOS_FRAME_NS 16724400ull
static u64 sNextFrameDeadlineTicks;

static void Port_Bios_PaceFrame(void) {
    const u64 ticksPerSec = Platform3DS_TicksPerSecond();
    const u64 ticksPerFrame = (ticksPerSec * PORT_BIOS_FRAME_NS) / 1000000000ull;
    const u64 now = Platform3DS_SystemTick();

    if (sNextFrameDeadlineTicks == 0 || now >= sNextFrameDeadlineTicks + ticksPerFrame) {
        sNextFrameDeadlineTicks = now;
    }
    sNextFrameDeadlineTicks += ticksPerFrame;

    if (now < sNextFrameDeadlineTicks) {
        const u64 remainingTicks = sNextFrameDeadlineTicks - now;
        const s64 remainingNs = (s64)((remainingTicks * 1000000000ull) / ticksPerSec);
        if (remainingNs > 0) svcSleepThread(remainingNs);
    }
}
#endif

void Port_Bios_Halt(void) {
#if defined(PLATFORM_LINUX)
    Platform_Linux_VBlank();
#elif defined(MZM_3DS)
#ifdef PORT_VERBOSE_FRAME_LOG
    Port_DebugLog("Port_Bios_Halt: before aptMainLoop");
#endif
    if (!aptMainLoop()) {
        Port_DebugLog("Port_Bios_Halt: aptMainLoop returned false, exiting");
        gfxExit();
        exit(0);
    }
    Platform3DS_PollKeysIntoGba();
    PlatformGpu3DS_RecordTick();
    PlatformGpu3DS_PerfRecordTick();
    /* Temporarily sleep-paced instead of gspWaitForEvent(0, true): the
     * latter never unblocks on real hardware here (confirmed via
     * sdmc:/3ds/mzm-debug.log bisection -- neither the GSP-event-thread
     * priority collision theory (fixed, no change) nor missing
     * aptMainLoop() pumping (added above, no change) explained it). Since
     * nothing is actually presented to the GPU yet (port_ppu_3ds.c isn't
     * adapted -- see docs/3ds-port-skeleton-import.md), real vblank sync
     * isn't needed yet either; a plain 60Hz sleep unblocks gameplay-logic
     * testing now and can be swapped back once gfxSwapBuffers()-driven
     * presentation exists to investigate the gspWaitForEvent hang for real. */
#endif
#if defined(MZM_3DS) && !defined(PLATFORM_LINUX)
    Port_AudioStateLock_Release();
#endif
    CallbackCallVblank();

extern bool Port_PPU_3DS_LastFrameUsedGpu(void);

#if defined(MZM_3DS) && !defined(PLATFORM_LINUX)
#ifdef PORT_VERBOSE_FRAME_LOG
    Port_DebugLog("Port_Bios_Halt: after CallbackCallVblank");
#endif
    Port_PPU_RenderFrame();
#ifdef PORT_VERBOSE_FRAME_LOG
    Port_DebugLog("Port_Bios_Halt: after Port_PPU_RenderFrame");
#endif
    /* If the frame was submitted via the synchronous GPU renderer, C3D_FrameSync()
     * in PlatformGpu3DS_EndBottom has already synchronized to hardware VBlank (60Hz).
     * Only the asynchronous CPU renderer handoff path needs software pacing. */
    if (!Port_PPU_3DS_LastFrameUsedGpu()) {
        Port_Bios_PaceFrame();
    } else {
        sNextFrameDeadlineTicks = 0;
    }
    Port_AudioStateLock_Acquire();
#endif
}

void VBlankIntrWait(void) {
    Port_Bios_Halt();
}
