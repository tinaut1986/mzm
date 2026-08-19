#include "port_mzm_audio_glue.h"

#include "structs/audio.h"
#include "data/audio.h"

#include <stdio.h>

extern void Port_DebugLog(const char* msg);

/* The engine's real CallSoundCodeC (the asm mixdown), captured on first use. */
static SoundCodeCFunc_T sRealSoundCodeC;

/*
 * Producer side of the mzm -> NDSP audio bridge.
 *
 * The real mzm engine (asm/audio_internal.s + asm/soundcode.s) mixes each
 * frame into gMusicInfo.soundRawData[3072]: the first half holds the left
 * channel, the second half the right channel, both mono unsigned 8-bit PCM at
 * gMusicInfo.sampleRate (SetupSoundTransfer in src/audio_wrappers.c). On the
 * GBA this buffer feeds FIFO_A/FIFO_B via DMA; there is no such hardware on
 * the 3DS, so Port_MzmAudio_Capture() is called from agbmain right after
 * UpdateAudio() each frame: it converts a frame's worth of u8 mono PCM to
 * interleaved s16 stereo and appends it to a lock-free ring that the NDSP
 * consumer thread drains.
 *
 * The NDSP output is driven at the engine's own sample rate (reported through
 * the rate hook), so there is NO resampling here -- the bridge is a pure 1:1
 * passthrough. Resampling would otherwise inject drift/underrun clicks because
 * the two clocks (engine DMA cadence vs NDSP) would not be locked.
 *
 * This file deliberately does NOT include <3ds.h>: it must pull in mzm's
 * types.h (via structs/audio.h) whose u32/s32 typedefs clash with libctru's,
 * so the thread/NDSP side lives in the platform/3ds tree instead and shares
 * only this ring via plain C types.
 */

/* Ring state is intentionally trivial (indices + storage only) so both sides
 * can be linked without depending on each other's headers. The producer runs
 * on the main thread, the consumer on the audio thread; indices are updated
 * with 32-bit atomics so there is a happens-before between the data write and
 * the index publish, and the consumer never sees a partial sample. */
#define MZM_RING_ALIGN 64

_Alignas(MZM_RING_ALIGN) short gMzmAudioRing[MZM_AUDIO_RING_FRAMES * 2];
static _Alignas(MZM_RING_ALIGN) unsigned int sRingReadIndex;  /* consumer */
static _Alignas(MZM_RING_ALIGN) unsigned int sRingWriteIndex; /* producer */

unsigned int Port_MzmAudio_RingReadIndex(void) {
    return __atomic_load_n(&sRingReadIndex, __ATOMIC_ACQUIRE);
}

unsigned int Port_MzmAudio_RingWriteIndex(void) {
    return __atomic_load_n(&sRingWriteIndex, __ATOMIC_ACQUIRE);
}

void Port_MzmAudio_AdvanceReadIndex(unsigned int index) {
    __atomic_store_n(&sRingReadIndex, index, __ATOMIC_RELEASE);
}

static int sInitialized;
static unsigned int sEngineRate;
static void (*sRateHook)(unsigned int engineRate);

void Port_MzmAudio_SetRateHook(void (*hook)(unsigned int engineRate)) {
    sRateHook = hook;
    if (sEngineRate && hook) hook(sEngineRate);
}

/*
 * Intercept the engine's final mixdown. CallSoundCodeC (SoundCodeC in
 * asm/soundcode.s) is the ONLY function that writes PCM into
 * gMusicInfo.soundRawData: the engine calls it (via gSoundCodeCPointer) with
 * a destination inside the left half, and it writes count*4 stereo-pair bytes
 * to that left slot (r0) and the mirror slot +0x600 (right half) (r3).
 * Wrapping is handled by the engine making a SECOND call with dest =
 * soundRawData[0]; since we run inside that same call we see both segments.
 *
 * Instead of guessing the write window (unk_11*16 is NOT the real write
 * position -- that's var_4, computed from unk_10/unk_C/unk_E), we run inside
 * the very call that writes the buffer and copy out exactly the bytes it just
 * produced. Each iteration of SoundCodeC writes 4 left bytes (from src) then 4
 * right bytes; so count*4 left samples and count*4 right samples are written.
 */
/*
 * Capture the raw u8 samples to a file on the SD so we can inspect the exact
 * bytes the engine produces (center offset, scale, and whether it is real
 * audio or DC). Writes interleaved L/R u8, capped at ~2 MiB.
 *
 * [PORT] 2026-08-19: OFF by default (build with -DPORT_AUDIO_DUMP_RAW to get
 * it back). This runs INSIDE the engine's mixdown, i.e. once per audio frame,
 * and each call does fopen("ab") + ~2*n fputc + fclose against the SD card --
 * on real hardware that is ~1400 byte-at-a-time stdio calls plus an open and
 * a close, every frame, on the critical path that produces the audio. Section
 * 6p already warned that per-sample file I/O here cost the entire frame rate
 * ("FPS 0") and said to batch the I/O; batching it into one open per call was
 * not enough on real hardware. Leaving this on was actively CAUSING the
 * starvation that section 6r then measured -- the engine could not produce
 * audio fast enough because it was blocked writing the diagnostic that was
 * measuring how little audio it produced. Do not enable this on real hardware
 * while also drawing conclusions about frame rate or ring fill; use it only
 * to inspect sample VALUES, and prefer capturing system audio output instead
 * (section 6r methodology). */
#ifdef PORT_AUDIO_DUMP_RAW
#define MZM_DUMP_PATH "sdmc:/3ds/mzm_audio_dump.bin"
#define MZM_DUMP_MAX (2u * 1024u * 1024u)
static unsigned int sDumpBytes;
static void DumpRaw(const unsigned char* left, const unsigned char* right, unsigned int n) {
    if (sDumpBytes >= MZM_DUMP_MAX) return;
    FILE* f = fopen(MZM_DUMP_PATH, "ab");
    if (!f) return;
    const unsigned int cap = (MZM_DUMP_MAX - sDumpBytes) / 2u;
    const unsigned int take = (n < cap) ? n : cap;
    for (unsigned int i = 0; i < take; ++i) {
        fputc(left[i], f);
        fputc(right[i], f);
    }
    sDumpBytes += take * 2u;
    fclose(f);
}
#else
#define DumpRaw(left, right, n) ((void)0)
#endif

static u8* Port_MzmAudio_SoundCodeC(u32* dest, u32* src, u8 count) {
    u8* ret = sRealSoundCodeC(dest, src, count);

    if (sInitialized) {
        const unsigned int engineRate = gMusicInfo.sampleRate;
        if (engineRate && engineRate != sEngineRate) {
            sEngineRate = engineRate;
            if (sRateHook) sRateHook(engineRate);
        }

        const unsigned int readIdx = __atomic_load_n(&sRingReadIndex, __ATOMIC_ACQUIRE);
        const unsigned int n = (unsigned int)count * 4u;
        if (sRingWriteIndex - readIdx + n < MZM_AUDIO_RING_FRAMES) {
            const unsigned char* left = (const unsigned char*)dest;
            const unsigned char* right = left + PCM_DMA_BUF_SIZE;
            DumpRaw(left, right, n);
            unsigned int wIdx = sRingWriteIndex;
            for (unsigned int i = 0; i < n; ++i) {
                /* [PORT] soundRawData is fed straight to REG_DMA1_SRC/DMA2_SRC
                 * (src/audio_wrappers.c), i.e. it's real GBA Direct Sound FIFO
                 * data -- the GBA hardware FIFO_A/FIFO_B consume SIGNED 8-bit
                 * PCM (GBATek), not unsigned/centered-at-128. Reinterpreting
                 * the byte as signed directly (no -128 bias) is the correct
                 * conversion; the previous bias turned a clean signed
                 * waveform into near-random amplitude swings, which is what
                 * was heard as crackling/static. */
                const int lv = (signed char)left[i];
                const int rv = (signed char)right[i];
                const unsigned int idx = (wIdx % MZM_AUDIO_RING_FRAMES) * 2;
                gMzmAudioRing[idx] = (short)(lv << 8);
                gMzmAudioRing[idx + 1] = (short)(rv << 8);
                ++wIdx;
            }
            __atomic_store_n(&sRingWriteIndex, wIdx, __ATOMIC_RELEASE);
        }

        /* Throttled diagnostic: engine rate, samples written by this call,
         * ring fill, peak amplitude, nonzero-sample count and a few raw
         * bytes. peak==0 with all-zero capture => reading silence.
         *
         * [PORT] 2026-08-19: gated OFF by default (-DPORT_AUDIO_DIAG_LOG to
         * re-enable), same reason as DumpRaw above -- Port_DebugLog is an
         * fopen+fprintf+fclose to the SD (port/port_debug_log.c) and this one
         * additionally scans the whole 3072-byte buffer first. Even throttled
         * to 1-in-64 it is real SD I/O inside the audio production path. */
#ifdef PORT_AUDIO_DIAG_LOG
        static unsigned int sDiagCount;
        if ((++sDiagCount & 0x3F) == 0) {
            /* Scan the WHOLE soundRawData buffer to see if the engine wrote
             * audio anywhere, not just at the wrapper's dest window. */
            unsigned int wholePeak = 0, wholeNonz = 0;
            const unsigned char* wb = gMusicInfo.soundRawData;
            for (unsigned int i = 0; i < 3072u; i += 2) {
                /* Signed PCM (see the conversion above): silence is 0, not 128. */
                int lv = (signed char)wb[i]; if (lv < 0) lv = -lv;
                int rv = (signed char)wb[i + 1]; if (rv < 0) rv = -rv;
                if ((unsigned)lv > wholePeak) wholePeak = (unsigned)lv;
                if ((unsigned)rv > wholePeak) wholePeak = (unsigned)rv;
                if (wb[i] != 0) ++wholeNonz;
                if (wb[i + 1] != 0) ++wholeNonz;
            }
            const unsigned char* left = (const unsigned char*)dest;
            const unsigned char* right = left + PCM_DMA_BUF_SIZE;
            const unsigned int curRead = __atomic_load_n(&sRingReadIndex, __ATOMIC_ACQUIRE);
            char msg[200];
            __builtin_snprintf(msg, sizeof(msg),
                "mzmAudio: rate=%u n=%u fill=%u wholePeak=%u wholeNonz=%u maxCh=%u off=%u l0=%u l1=%u l2=%u dest=%p",
                engineRate, n, sRingWriteIndex - curRead, wholePeak, wholeNonz,
                gMusicInfo.maxSoundChannels, (unsigned int)((const unsigned char*)dest - wb),
                left[0], left[1], left[2], (void*)dest);
            Port_DebugLog(msg);
        }
#endif /* PORT_AUDIO_DIAG_LOG */
    }

    return ret;
}

/* Install (or re-install) the intercept. Must be idempotent and called every
 * frame because the engine's InitializeAudio() re-assigns
 * gSoundCodeCPointer = CallSoundCodeC once at startup, which would otherwise
 * undo our hook. If gSoundCodeCPointer is not yet set (InitializeAudio not
 * run), capture the real one once and install on a later frame. */
void Port_MzmAudio_InstallSoundCodeCHook(void) {
    if (sRealSoundCodeC == NULL)
        sRealSoundCodeC = gSoundCodeCPointer;
    if (sRealSoundCodeC != NULL)
        gSoundCodeCPointer = Port_MzmAudio_SoundCodeC;
}

void Port_MzmAudio_InitGlue(void) {
    sEngineRate = 0;
    sRateHook = NULL;
    sInitialized = 1;
    sRealSoundCodeC = NULL;
    __atomic_store_n(&sRingReadIndex, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&sRingWriteIndex, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < MZM_AUDIO_RING_FRAMES * 2; ++i) gMzmAudioRing[i] = 0;
}