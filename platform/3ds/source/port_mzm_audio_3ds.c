#include "port_mzm_audio_glue.h"

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

extern void Port_DebugLog(const char* msg);
extern bool Platform3DS_CanUseCore1(void);

/*
 * NDSP consumer for the mzm port's real audio engine.
 *
 * port_mzm_audio_glue.c (producer, main thread) converts the engine's
 * gMusicInfo.soundRawData u8 mono PCM into interleaved s16 stereo at
 * MZM_AUDIO_OUT_RATE and appends it to a lock-free ring. This file runs a
 * dedicated audio thread that drains that ring into NDSP wave buffers on
 * channel 0, mirroring the buffer-management pattern of the (TMC-coupled)
 * port_audio_3ds.c but sourcing samples from the mzm ring instead of the
 * agbplay/M4A backend.
 */

#define BUFFER_FRAMES 224
#define BUFFER_COUNT 4
#define AUDIO_THREAD_STACK (64u * 1024u)

#define RING_FLOOR 0

static ndspWaveBuf sWave[BUFFER_COUNT];
static int16_t* sSamples;
static bool sInitialized;
static bool sPaused;
static volatile bool sAudioThreadRunning;
static Thread sAudioThread;
static LightEvent sAudioWake;

/* Rate the NDSP channel is currently configured at. Defaults to
 * MZM_AUDIO_OUT_RATE until the engine's real rate is reported via the rate
 * hook (agbmain -> InitializeGame -> SetupSoundTransfer). */
static unsigned int sOutRate = MZM_AUDIO_OUT_RATE;

static void OnEngineRate(unsigned int engineRate) {
    if (!sInitialized || engineRate == 0 || engineRate == sOutRate) return;
    sOutRate = engineRate;
    ndspChnSetRate(0, engineRate);
}

static void FillBuffer(int index) {
    int16_t* dst = sSamples + index * BUFFER_FRAMES * 2;

    /* Drain as much of the ring as the buffer holds, consuming contiguous
     * frames. The ring is circular; copy in up to two linear segments. */
    unsigned int readIdx = Port_MzmAudio_RingReadIndex();
    const unsigned int writeIdx = Port_MzmAudio_RingWriteIndex();
    const unsigned int available = writeIdx - readIdx;

    /* Leave at least RING_FLOOR frames in the ring as a backlog cushion. */
    const unsigned int availForUse =
        (available > RING_FLOOR) ? (available - RING_FLOOR) : 0;
    const unsigned int toRead =
        (availForUse < BUFFER_FRAMES) ? availForUse : BUFFER_FRAMES;

    unsigned int consumed = 0;
    while (consumed < toRead) {
        const unsigned int linear = MZM_AUDIO_RING_FRAMES - (readIdx % MZM_AUDIO_RING_FRAMES);
        const unsigned int chunk = (toRead - consumed < linear) ? (toRead - consumed) : linear;
        memcpy(dst + consumed * 2, gMzmAudioRing + (readIdx % MZM_AUDIO_RING_FRAMES) * 2,
               chunk * 2 * sizeof(int16_t));
        readIdx += chunk;
        consumed += chunk;
    }

    if (toRead < BUFFER_FRAMES) {
        if (toRead > 0) {
            /* Smoothly fade out the last few samples to eliminate harsh pop/click on underrun */
            const unsigned int fadeLen = (toRead >= 8) ? 8 : toRead;
            for (unsigned int f = 0; f < fadeLen; ++f) {
                const unsigned int idx = (toRead - fadeLen + f) * 2;
                const int factor = (int)(fadeLen - 1 - f);
                dst[idx] = (int16_t)(((int)dst[idx] * factor) / (int)fadeLen);
                dst[idx + 1] = (int16_t)(((int)dst[idx + 1] * factor) / (int)fadeLen);
            }
            memset(dst + toRead * 2, 0, (BUFFER_FRAMES - toRead) * 2 * sizeof(int16_t));
        } else {
            memset(dst, 0, BUFFER_FRAMES * 2 * sizeof(int16_t));
        }
    }

    /* Publish how many frames we consumed back to the producer. */
    Port_MzmAudio_AdvanceReadIndex(readIdx);
    DSP_FlushDataCache(dst, BUFFER_FRAMES * 2 * sizeof(int16_t));
    sWave[index].data_pcm16 = dst;
    sWave[index].nsamples = BUFFER_FRAMES;
    sWave[index].status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(0, &sWave[index]);

    /* Throttled diagnostic: how much of each NDSP buffer actually held audio
     * (toRead < BUFFER_FRAMES means we under-ran the ring and had to pad with
     * silence, which is exactly what the "clicks" would sound like).
     *
     * [PORT] 2026-08-19: gated OFF by default (-DPORT_AUDIO_DIAG_LOG), see
     * port_mzm_audio_glue.c. This one runs on the AUDIO THREAD, so its SD
     * write (Port_DebugLog = fopen+fprintf+fclose) blocks the very thread
     * that is supposed to be feeding NDSP on time. */
#ifdef PORT_AUDIO_DIAG_LOG
    static unsigned int sDiagCount;
    if ((++sDiagCount & 0x1F) == 0) {
        char msg[96];
        __builtin_snprintf(msg, sizeof(msg), "ndsp: toRead=%u/%u rate=%u",
            toRead, BUFFER_FRAMES, sOutRate);
        Port_DebugLog(msg);
    }
#endif
}

static uint32_t CountDoneBuffers(void) {
    uint32_t done = 0;
    for (int i = 0; i < BUFFER_COUNT; ++i) {
        if (sWave[i].status == NDSP_WBUF_DONE) ++done;
    }
    return done;
}

static int FindDoneBuffer(void) {
    for (int i = 0; i < BUFFER_COUNT; ++i) {
        if (sWave[i].status == NDSP_WBUF_DONE) return i;
    }
    return -1;
}

static void AudioFrameFinished(void* data) {
    (void)data;
    if (CountDoneBuffers() == 0) return;
    LightEvent_Signal(&sAudioWake);
}

static void AudioThreadMain(void* argument) {
    (void)argument;
    while (__atomic_load_n(&sAudioThreadRunning, __ATOMIC_ACQUIRE)) {
        LightEvent_Wait(&sAudioWake);
        if (!__atomic_load_n(&sAudioThreadRunning, __ATOMIC_ACQUIRE)) break;
        int bufferIndex;
        while ((bufferIndex = FindDoneBuffer()) >= 0) {
            FillBuffer(bufferIndex);
        }
    }
}

bool Port_MzmAudio_Init(void) {
    if (sInitialized) return true;
    if (ndspInit() != 0) return false;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, MZM_AUDIO_OUT_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 1.0f, 0.0f, 0.0f, 1.0f };
    ndspChnSetMix(0, mix);

    sSamples = (int16_t*)linearAlloc(BUFFER_COUNT * BUFFER_FRAMES * 2 * sizeof(int16_t));
    if (!sSamples) {
        ndspExit();
        return false;
    }
    sOutRate = MZM_AUDIO_OUT_RATE;
    Port_MzmAudio_InitGlue();
    Port_MzmAudio_SetRateHook(OnEngineRate);
    memset(sWave, 0, sizeof(sWave));
    memset(sSamples, 0, BUFFER_COUNT * BUFFER_FRAMES * 2 * sizeof(int16_t));
    LightEvent_Init(&sAudioWake, RESET_ONESHOT);
    sInitialized = true;
    sPaused = false;
    ndspSetCallback(AudioFrameFinished, NULL);
    for (int i = 0; i < BUFFER_COUNT; ++i) FillBuffer(i);

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    if (priority > 0x18) --priority;
    /* Core 0 (APPCORE) already hosts the main game loop and the GBA timing
     * thread (which wakes every ~73us, see platform_3ds_minimal.c). Putting
     * this thread there too means it competes for CPU time with both,
     * especially when it wakes to refill several NDSP buffers back to back
     * on an underrun -- a real slowdown, not just an audio glitch. On
     * New3DS the port already claims a CPU budget on Core 1 (SYSCORE) via
     * APT_SetAppCpuTimeLimit (see Platform3DS_CanUseCore1/Core1TimeLimit)
     * but nothing was using it yet. Old3DS has no such budget, so it must
     * stay on core 0 there. */
    const int audioThreadCore = Platform3DS_CanUseCore1() ? 1 : 0;
    __atomic_store_n(&sAudioThreadRunning, true, __ATOMIC_RELEASE);
    sAudioThread = threadCreate(AudioThreadMain, NULL, AUDIO_THREAD_STACK, priority, audioThreadCore, false);
    if (!sAudioThread) {
        __atomic_store_n(&sAudioThreadRunning, false, __ATOMIC_RELEASE);
    }
    return true;
}

void Port_MzmAudio_Pump(void) {
    if (!sInitialized) return;
    if (sPaused) return;
    if (CountDoneBuffers() > 0) LightEvent_Signal(&sAudioWake);
    if (!sAudioThread) {
        int bufferIndex;
        while ((bufferIndex = FindDoneBuffer()) >= 0) {
            FillBuffer(bufferIndex);
        }
    }
}

void Port_MzmAudio_Shutdown(void) {
    if (!sInitialized) return;
    ndspSetCallback(NULL, NULL);
    __atomic_store_n(&sAudioThreadRunning, false, __ATOMIC_RELEASE);
    LightEvent_Signal(&sAudioWake);
    if (sAudioThread) {
        threadJoin(sAudioThread, U64_MAX);
        threadFree(sAudioThread);
        sAudioThread = NULL;
    }
    ndspChnWaveBufClear(0);
    linearFree(sSamples);
    sSamples = NULL;
    ndspExit();
    sInitialized = false;
    sPaused = false;
}