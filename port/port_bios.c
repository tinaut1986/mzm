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

void CpuSet(void* src, void* dst, u32 ctrl) {
    u32 count = ctrl & CPUSET_COUNT_MASK;
    int fill = (ctrl & CPUSET_FIXED_SRC) != 0;

    if (ctrl & CPUSET_32BIT) {
        const u32* s = (const u32*)src;
        u32* d = (u32*)dst;
        for (u32 i = 0; i < count; i++) {
            d[i] = fill ? s[0] : s[i];
        }
    } else {
        const u16* s = (const u16*)src;
        u16* d = (u16*)dst;
        for (u32 i = 0; i < count; i++) {
            d[i] = fill ? s[0] : s[i];
        }
    }
}

void CpuFastSet(void* src, void* dst, u16 ctrl) {
    /* Always 32-bit, count rounded up to a multiple of 8 words (one 32-byte
     * block) -- matches real BIOS behavior of processing whole blocks. The
     * fill/datasize control bits (24+) don't fit in this codebase's u16
     * ctrl parameter, so every call site here only ever varies the count;
     * fill-mode CpuFastSet calls (if any exist) would already have been
     * broken by that same truncation in the original decompiled build. */
    u32 count = (((u32)ctrl & CPUSET_COUNT_MASK) + 7u) & ~7u;
    int fill = ((u32)ctrl & CPUSET_FIXED_SRC) != 0;
    const u32* s = (const u32*)src;
    u32* d = (u32*)dst;
    for (u32 i = 0; i < count; i++) {
        d[i] = fill ? s[0] : s[i];
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
    u32 header = (u32)src[0] | ((u32)src[1] << 8) | ((u32)src[2] << 16) | ((u32)src[3] << 24);
    u32 decompressedSize = header >> 8;
    src += 4;

    u8* out = dst;
    u8* outEnd = dst + decompressedSize;

    while (out < outEnd) {
        u8 flags = *src++;
        for (int bit = 7; bit >= 0 && out < outEnd; bit--) {
            if ((flags & (1 << bit)) == 0) {
                *out++ = *src++;
            } else {
                u8 b0 = *src++;
                u8 b1 = *src++;
                u32 length = (u32)(b0 >> 4) + 3;
                u32 distance = (((u32)(b0 & 0xF) << 8) | b1) + 1;
                const u8* copySrc = out - distance;
                for (u32 i = 0; i < length && out < outEnd; i++) {
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

/* Real MidiKey2Freq semantics use a lookup table baked into the BIOS ROM
 * (per-waveform pitch table) that isn't available here. Placeholder until
 * the real audio backend (UpdateMusic/TrackVariables, see
 * docs/3ds-port-rom-loading.md's audio-engine note) is wired up: standard
 * 12-tone equal temperament from MIDI key 60 = middle C, ignoring
 * waveData's per-sample base frequency. Revisit once audio is being tested
 * for real -- getting this exactly right matters for pitch accuracy, not
 * for anything currently being verified. */
u32 MidiKey2Freq(u32* waveData, u8 midiKey, u8 fineAdjust) {
    (void)waveData;
    double semitones = (double)(midiKey - 60) + (double)fineAdjust / 256.0;
    double freq = 261.626 /* middle C, Hz */ * __builtin_exp2(semitones / 12.0);
    return (u32)freq;
}

extern void CallbackCallVblank(void);

/* Deliberately not `#include <3ds.h>`: 3ds/types.h typedefs u32/s32/vu32/...
 * as (distinct, same-size) aliases of the stdint types, which conflicts
 * with mzm's own types.h (typedef'd from the plain C integer types) the
 * moment both are visible in one translation unit. Forward-declare just the
 * one libctru function actually needed instead of pulling in the whole
 * header -- real signature per libctru's gspgpu.h. */
extern void gspWaitForVBlank(void);

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
void Port_Bios_Halt(void) {
    gspWaitForVBlank();
    CallbackCallVblank();
}

void VBlankIntrWait(void) {
    Port_Bios_Halt();
}
