#pragma once

#include <stdbool.h>

/* The question about the DSP firmware (sdmc:/3ds/dspfirm.cdc), drawn by the bottom screen. */

enum {
    DSPFIRM_PROMPT_NONE = 0,
    DSPFIRM_PROMPT_ASK,      /* missing or damaged: EXTRACT / NO SOUND */
    DSPFIRM_PROMPT_WORKING,  /* extracting */
    DSPFIRM_PROMPT_DONE,
    DSPFIRM_PROMPT_FAILED,
};

/* After the audio failed to start: opens the question if the firmware is the reason. */
void Port_DspFirm_Raise(void);
int Port_DspFirm_State(void);
bool Port_DspFirm_Missing(void);        /* else damaged */
const char* Port_DspFirm_Failure(void); /* "ERROR n / code" after a failure */
void Port_DspFirm_Answer(bool yes);     /* the buttons; also A and B through Port_DspFirm_Pump */
void Port_DspFirm_Pump(void);           /* once per frame, present thread */
