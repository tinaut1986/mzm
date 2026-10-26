#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsp_firm.h"
#include "port_debug_log.h"
#include "port_dsp_firm_prompt_3ds.h"

/* ndspInit() needs sdmc:/3ds/dspfirm.cdc; without it the game runs silent. When the audio could not
 * start because the file is missing or damaged, the bottom screen asks (port_bottom_ui_3ds.c draws
 * it) whether to extract it from the console's own Home Menu. It is Nintendo's, so it never ships. */

extern bool Port_MzmAudio_Init(void);

static volatile int sState = DSPFIRM_PROMPT_NONE;
static DspFirmStatus sStatus;
static char sFailure[48];
static int sWorkFrames;

void Port_DspFirm_Raise(void) {
    if (sState != DSPFIRM_PROMPT_NONE) return;
    sStatus = DspFirm_Check(DSPFIRM_PATH);
    if (sStatus == DSPFIRM_OK) return;   /* the audio failed for another reason: nothing to offer */
    Port_DebugLog(sStatus == DSPFIRM_MISSING ? "dsp firmware: missing, asking" : "dsp firmware: damaged, asking");
    sState = DSPFIRM_PROMPT_ASK;
}

int Port_DspFirm_State(void) { return sState; }
bool Port_DspFirm_Missing(void) { return sStatus == DSPFIRM_MISSING; }
const char* Port_DspFirm_Failure(void) { return sFailure; }

void Port_DspFirm_Answer(bool yes) {
    if (sState == DSPFIRM_PROMPT_ASK) {
        if (yes) {
            sWorkFrames = 0;
            sState = DSPFIRM_PROMPT_WORKING;
        } else {
            Port_DebugLog("dsp firmware: extraction declined");
            sState = DSPFIRM_PROMPT_NONE;
        }
    } else if (sState == DSPFIRM_PROMPT_DONE || sState == DSPFIRM_PROMPT_FAILED) {
        sState = DSPFIRM_PROMPT_NONE;
    }
}

/* Called once per frame by the present thread. The extraction takes a moment and blocks it: it
 * starts after "EXTRACTING..." has been on the screen for a few frames. */
void Port_DspFirm_Pump(void) {
    static bool sWasAB;
    const u32 held = hidKeysHeld();
    const bool a = (held & KEY_A) != 0, b = (held & KEY_B) != 0;
    const bool ab = a || b;
    if (sState != DSPFIRM_PROMPT_NONE && sState != DSPFIRM_PROMPT_WORKING && ab && !sWasAB) {
        Port_DspFirm_Answer(a);   /* A = yes / OK, B = no / OK */
    }
    sWasAB = ab;

    if (sState != DSPFIRM_PROMPT_WORKING || ++sWorkFrames < 4) return;
    int32_t detail = 0;
    const DspFirmExtractResult r = DspFirm_Extract(DSPFIRM_PATH, &detail);
    char note[600];
    snprintf(note, sizeof(note), "dsp firmware: extraction %d (%s), detail %08lX | %s", (int)r, DspFirm_ExtractText(r),
             (unsigned long)detail, g_dspfirm_trace);
    Port_DebugLog(note);
    if (r == DSPFIRM_EX_OK && Port_MzmAudio_Init()) {
        sState = DSPFIRM_PROMPT_DONE;
        return;
    }
    snprintf(sFailure, sizeof(sFailure), "ERROR %d / %08lX", (int)r, (unsigned long)detail);
    sState = DSPFIRM_PROMPT_FAILED;
}
