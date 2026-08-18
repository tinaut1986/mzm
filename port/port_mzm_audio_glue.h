#ifndef TMC_PORT_MZM_AUDIO_GLUE_H
#define TMC_PORT_MZM_AUDIO_GLUE_H

/*
 * Shared contract between the mzm audio producer (port_mzm_audio_glue.c,
 * plain C, runs on the main thread) and the NDSP consumer
 * (platform/3ds/source/port_mzm_audio_3ds.c, includes <3ds.h>).
 *
 * Uses only plain C types so both sides can include this without the u32/s32
 * typedef clash between mzm's types.h and libctru.
 */

#define MZM_AUDIO_OUT_RATE 16364u
#define MZM_AUDIO_RING_FRAMES 16384u

extern short gMzmAudioRing[MZM_AUDIO_RING_FRAMES * 2];

unsigned int Port_MzmAudio_RingReadIndex(void);
unsigned int Port_MzmAudio_RingWriteIndex(void);
void Port_MzmAudio_AdvanceReadIndex(unsigned int index);

/* Optional hook the 3DS side registers so the producer can reconfigure the
 * NDSP output rate the moment the engine's real sample rate becomes known
 * (InitializeGame sets it inside agbmain, i.e. after Port_MzmAudio_Init). If
 * left NULL the producer falls back to a fixed MZM_AUDIO_OUT_RATE passthrough
 * sized by the engine rate it sees at first capture. */
void Port_MzmAudio_SetRateHook(void (*hook)(unsigned int engineRate));

void Port_MzmAudio_InitGlue(void);
void Port_MzmAudio_InstallSoundCodeCHook(void);

#endif