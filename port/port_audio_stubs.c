#include "types.h"
#include "structs/audio.h"
#include <stddef.h>

#ifdef PORT_NATIVE_AUDIO_STUBS
void InitTrack(struct TrackData* pTrack, const struct SoundHeader* pHeader) { (void)pTrack; (void)pHeader; }
void StopMusicOrSound(struct TrackData* pTrack) { (void)pTrack; }
void ResetTrack(struct TrackData* pTrack) { (void)pTrack; }
void unk_4cfc(struct TrackData* pTrack) { (void)pTrack; }
void unk_4d1c(struct TrackData* pTrack) { (void)pTrack; }
void UpdateAudio(void) {}
void unk_4e10(struct TrackData* pTrack, u32 unk) { (void)pTrack; (void)unk; }
void unk_4eb4(struct TrackData* pTrack, struct TrackVariables* pVariables) { (void)pTrack; (void)pVariables; }
void unk_4f10(struct TrackVariables* pVariables) { (void)pVariables; }
void unk_4f8c(struct TrackVariables* pVariables) { (void)pVariables; }
void AudioCommand_Goto(struct TrackVariables* pVariables) { (void)pVariables; }
void AudioCommand_PatternPlay(struct TrackVariables* pVariables) { (void)pVariables; }
void UploadSampleToWaveRam(void* src) { (void)src; }
void unk_5104(struct PsgSoundData* pSound) { (void)pSound; }

void CallSoundCodeA(struct SoundChannel* pChannel, u32* param_2, u8 param_3) { (void)pChannel; (void)param_2; (void)param_3; }
u8* CallSoundCodeB(u32* param_1, u32* param_2, u32* param_3, u8 param_4) { (void)param_1; (void)param_2; (void)param_3; (void)param_4; return NULL; }
u8* CallSoundCodeC(u32* param_1, u32* param_2, u8 param_3) { (void)param_1; (void)param_2; (void)param_3; return NULL; }
u32 CallGetNoteFrequency(u32 key, u32 unk) { (void)key; (void)unk; return 0; }
void sub_08004ad8(void) {}
#endif
