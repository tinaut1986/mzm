#include "syscalls.h"
#include "data/generic_data.h"
#include "data/menus/language_select_data.h"

#include "gba.h"
#include "port_debug_log.h"
#include "audio/track_internal.h"
#include "constants/game_state.h"

#include "structs/cutscene.h"
#include "structs/demo.h"
#include "structs/game_state.h"
#include "structs/display.h"

/**
 * @brief 23c | 464 | Main loop of the game
 *  
 */
void agbmain(void)
{
#if defined(TMC_3DS) && defined(PORT_VERBOSE_FRAME_LOG)
    /* Temporary boot-hang diagnostic checkpoints, see port/port_debug_log.h.
     * Gated behind PORT_VERBOSE_FRAME_LOG (off by default) since this does a
     * flushed file write per call -- ~10x/frame between this file and
     * Port_Bios_Halt was the single biggest perf cost once boot was no
     * longer hanging. Re-enable (add -DPORT_VERBOSE_FRAME_LOG to the
     * Makefile) when bisecting a new boot-time hang. */
    extern void Port_DebugLog(const char* msg);
    Port_DebugLog("agbmain: before InitializeGame()");
#endif
    InitializeGame();
#if defined(TMC_3DS) && defined(PORT_VERBOSE_FRAME_LOG)
    Port_DebugLog("agbmain: InitializeGame() done, entering main loop");
#endif

    while (TRUE)
    {
#if defined(TMC_3DS) && defined(PORT_VERBOSE_FRAME_LOG)
        {
            static int sLoopCount = 0;
            if (sLoopCount < 5) {
                Port_DebugLog("agbmain: loop iteration start");
                sLoopCount++;
            }
        }
#endif
        gVblankActive = FALSE;
#ifdef REGION_EU
        if (gMainGameMode == GM_INGAME || gMainGameMode == GM_DEMO)
            InGameIoWriteRegisters();
#endif // REGION_EU
#if defined(TMC_3DS) && defined(PORT_VERBOSE_FRAME_LOG)
        Port_DebugLog("agbmain: before UpdateAudio()");
#endif
#if defined(TMC_3DS) && defined(__3DS__)
        {
            /* [PORT] Section 6q: gMusicInfo.unk_10 (the "how far real DMA
             * hardware has consumed" position UpdateMusic() uses to decide
             * how much audio to mix per call, src/audio.c:32-116) is only
             * ever advanced by DMA2IntrCode() (src/music_wrappers.c) -- the
             * ISR real hardware fires each time DMA2 finishes streaming a
             * PCM_DMA_BUF_SIZE/2-byte half-buffer to FIFO_B. Nothing on this
             * port ever calls it (no real DMA2 hardware), so unk_10 sits
             * frozen at its SetupSoundTransfer initial value all game,
             * decoupling UpdateMusic()'s chunk-size math from actual
             * playback progress. Sample-level analysis against a known-good
             * mgba reference found the port's audio has real, measurable
             * discontinuities (739 large jumps vs 0) clustered almost
             * exactly at PCM_DMA_BUF_SIZE/4 = 384 samples into each output
             * chunk -- precisely where that missing interrupt should fire.
             * Drive it here instead, based on how many samples the NDSP
             * ring has actually had consumed since the last frame (real
             * playback progress, not simulated time), firing once per real
             * half-buffer's worth. Deliberately NOT verified by ear (see
             * docs/3ds-port-status-2026-08-17.md section 6q) -- only
             * checked with the section 6o/6p sample-jump counter. Revert
             * this block if that count gets worse, the same way the
             * section 6l buffer tuning was reverted for making things
             * worse. */
            extern unsigned int Port_MzmAudio_RingReadIndex(void);
            extern unsigned int Port_MzmAudio_RingWriteIndex(void);
            extern void DMA2IntrCode(void);
            static unsigned int sLastRingReadIdx;
            static unsigned int sConsumedAccum;
            static int sHaveLastRingReadIdx;
            const unsigned int curReadIdx = Port_MzmAudio_RingReadIndex();
            /* [PORT] Diagnostic added 2026-08-19 while chasing the crackling
             * that survived the section 6q fix: a live PulseAudio capture of
             * Azahar's actual output (not the pre-NDSP dump the 6o-6q
             * analysis used) showed the ring starved ~65% of the time on
             * this dev machine (avg NDSP buffer fill 34.9%, see
             * docs/3ds-port-status-2026-08-17.md section 6r). This logs, per
             * agbmain iteration, how many real samples were "owed" (ring
             * consumption since the last iteration, i.e. how far behind
             * production the loop fell) against wall-clock ms elapsed, so a
             * future session can correlate stutters directly from the log
             * instead of guessing from FPS text on a screenshot. unk_E (the
             * decomp's emulated DMA double-buffer capacity, see
             * SetupSoundTransfer in audio_wrappers.c) is 1536 samples -- if
             * "owed" ever exceeds that between iterations, that iteration's
             * catch-up is provably clipped and the lost audio is
             * unrecoverable, regardless of how large the NDSP ring is. */
#ifdef PORT_AUDIO_DIAG_LOG
            extern u64 osGetTime(void);
            extern void Port_DebugLog(const char* msg);
            static u64 sLastPaceMs;
            static unsigned int sPaceDiagCount;
#endif
            if (sHaveLastRingReadIdx) {
                sConsumedAccum += curReadIdx - sLastRingReadIdx;
#ifdef PORT_AUDIO_DIAG_LOG
                const unsigned int owed = sConsumedAccum;
                const u64 nowMs = osGetTime();
                if ((++sPaceDiagCount & 0x1F) == 0) {
                    const unsigned int ringFill =
                        Port_MzmAudio_RingWriteIndex() - curReadIdx;
                    char msg[96];
                    __builtin_snprintf(msg, sizeof(msg),
                        "audioPace: dt=%ums owed=%u ringFill=%u",
                        (unsigned int)(nowMs - sLastPaceMs), owed, ringFill);
                    Port_DebugLog(msg);
                }
                sLastPaceMs = nowMs;
#endif
                while (sConsumedAccum >= 384u) {
                    DMA2IntrCode();
                    sConsumedAccum -= 384u;
                }
            }
#ifdef PORT_AUDIO_DIAG_LOG
            else {
                sLastPaceMs = osGetTime();
            }
#endif
            sLastRingReadIdx = curReadIdx;
            sHaveLastRingReadIdx = 1;

            /* Bridge the real mzm audio engine to NDSP by intercepting the
             * engine's final mixdown. gSoundCodeCPointer points at
             * CallSoundCodeC (the only function that writes into
             * gMusicInfo.soundRawData); Port_MzmAudio_InstallSoundCodeCHook()
             * substitutes a wrapper that runs inside that call and copies out
             * exactly the PCM the engine just produced (left slot + the
             * +0x600 right slot), so we never need to know the engine's
             * internal write window. Install must precede UpdateAudio(). */
            extern void Port_MzmAudio_InstallSoundCodeCHook(void);
            Port_MzmAudio_InstallSoundCodeCHook();
            UpdateAudio();

            /* [PORT] Decouple how much audio gets MIXED from the frame rate.
             *
             * UpdateAudio() mixes one frame's worth per call, because on a GBA
             * it is called exactly 60 times a second. This port does not hold
             * 60 FPS (~55 on real hardware, less under load), so it produced
             * ~8% less audio per second than NDSP consumes -- a permanent
             * deficit that empties the ring no matter how it is buffered, and
             * comes out as periodic dropouts. Section 6r measured the ring
             * pinned at its floor for entire runs because of exactly this.
             *
             * UpdateMusic() already computes how much to mix from the gap
             * between what hardware has consumed (gMusicInfo.unk_10, which the
             * block above advances from real NDSP ring consumption, section
             * 6q) and what has been produced (unk_11). So calling it again
             * while a gap remains mixes precisely the shortfall and nothing
             * more: once caught up it takes the `var_6 == 0` early return in
             * src/audio.c, before any mixing or envelope stepping, so the
             * extra calls are free no-ops rather than a second frame of sound.
             *
             * That early return is also why this does not distort envelopes:
             * a call only steps them when it actually mixes, so envelope rate
             * stays tied to audio produced rather than to frames drawn, which
             * is what the hardware behaviour amounts to. The note SEQUENCER
             * (UpdateTrack, inside UpdateAudio) deliberately still runs once
             * per frame -- calling it more often would speed the music's
             * tempo. Tempo therefore still drifts slightly slow when frames
             * are slow; that is a separate, much less audible problem than
             * dropouts, and fixing it needs a real fixed-timestep loop.
             *
             * Bounded so a pathological stall cannot spin here for long. */
            {
                extern void UpdateMusic(void);
                for (int catchUp = 0; catchUp < 4; ++catchUp) {
                    const unsigned int producedBefore = Port_MzmAudio_RingWriteIndex();
                    UpdateMusic();
                    if (Port_MzmAudio_RingWriteIndex() == producedBefore)
                        break; /* nothing left owed */
                }
            }
            /* Safety net: normally the dedicated NDSP consumer thread
             * (port_mzm_audio_3ds.c) drains the ring on its own via the
             * ndsp callback. If that thread failed to start (e.g. thread
             * creation rejected for a core not in the exheader's
             * AffinityMask), nothing would ever call FillBuffer() and
             * audio would be silently dead with no error -- Pump() is a
             * no-op when the thread is alive, and does the draining
             * itself otherwise. */
            extern void Port_MzmAudio_Pump(void);
            Port_MzmAudio_Pump();
        }
#else
        UpdateAudio();
#endif

        if (gResetGame)
            break;

        UpdateInput();
#if defined(TMC_3DS) && defined(PORT_VERBOSE_FRAME_LOG)
        Port_DebugLog("agbmain: UpdateInput() done");
#endif
        SoftResetCheck();
#if defined(TMC_3DS) && defined(PORT_VERBOSE_FRAME_LOG)
        Port_DebugLog("agbmain: SoftResetCheck() done");
#endif
        // Increment frame counters
        APPLY_DELTA_TIME_INC(gFrameCounter8Bit);
        APPLY_DELTA_TIME_INC(gFrameCounter16Bit);

#ifdef TMC_3DS
        {
            static u8 sLastGM = 0xFF;
            static u8 sLastSub1 = 0xFF;
            if (gMainGameMode != sLastGM || gSubGameMode1 != sLastSub1) {
                char dbg[64];
                __builtin_snprintf(dbg, sizeof(dbg), "ModeChange -> GM: 0x%02X, Sub1: 0x%02X", gMainGameMode, gSubGameMode1);
                Port_DebugLog(dbg);
                sLastGM = gMainGameMode;
                sLastSub1 = gSubGameMode1;
            }
        }
#endif

        switch (gMainGameMode)

        {
            case GM_SOFT_RESET:
                if (SoftResetHandler())
                {
                    gMainGameMode = GM_INTRO;
                    gSubGameMode1 = 0;
                }
                break;

            case GM_INTRO:
#ifdef DEBUG
                if (gChangedInput & KEY_R)
                {
                    gSubGameMode1 = 0;
                    gMainGameMode = GM_DEBUG_MENU;
                }
                else
#endif // DEBUG
                if (IntroHandler())
                {
                    gMainGameMode = GM_TITLE;
                    gSubGameMode1 = 0;
                }
                break;

            case GM_TITLE:
#ifdef DEBUG
                if (gChangedInput & KEY_R)
                {
                    gSubGameMode1 = 0;
                    gPauseScreenFlag = 0;
                    gSubGameMode2 = 0;
                    gMainGameMode = GM_DEBUG_MENU;
                }
                else
#endif // DEBUG
                if (TitleScreenHandler())
                {
#ifdef REGION_EU
                    gSubGameMode1 = 0;
#endif // REGION_EU
                    if (gSubGameMode2 == 1)
                    {
                        gMainGameMode = GM_FILE_SELECT;
                    }
                    else if (gSubGameMode2 == 2)
                    {
                        DemoStart();
                        gMainGameMode = GM_DEMO;
                    }
#ifdef REGION_EU
                    else if (gSubGameMode2 == 3)
                    {
                        gMainGameMode = GM_SOFT_RESET;
                        gSubGameMode1 = sLanguageSelectGameModeSub1Values[1];
                    }
#endif // REGION_EU
                    else
                    {
#ifdef DEBUG
                        gMainGameMode = GM_DEBUG_MENU;
#else // !DEBUG
                        gMainGameMode = GM_INTRO;
#endif // DEBUG
                    }

#ifndef REGION_EU
                    gSubGameMode1 = 0;
#endif // !REGION_EU
                    gPauseScreenFlag = 0;
                    gSubGameMode2 = 0;
                }
                break;

            case GM_FILE_SELECT:
                if (FileSelectMenuHandler())
                {
                    if (gSubGameMode2 == 1) // If continuing file
                        gMainGameMode = GM_INGAME;
                    else if (gSubGameMode2 == 2) // If starting new file
                        gMainGameMode = GM_INGAME;
                    else if (gSubGameMode2 == 4)
                        gMainGameMode = GM_FUSION_GALLERY;
                    else if (gSubGameMode2 == 5)
                        gMainGameMode = GM_GALLERY;
                    else
                        gMainGameMode = GM_INTRO;

                    gSubGameMode1 = 0;
                    gSubGameMode3 = 0;
                    gSubGameMode2 = 0;
                }
                break;

            case GM_INGAME:
                if (InGameHandler()) 
                {
                    if (gPauseScreenFlag == PAUSE_SCREEN_NONE)
                    {
                        if (gCurrentCutscene != 0)
                        {
                            gMainGameMode = GM_CUTSCENE;
                        }
                        else if (gTourianEscapeCutsceneStage != 0)
                        {
                            gMainGameMode = GM_TOURIAN_ESCAPE;
                        }
                        else
                        {
#ifdef DEBUG
                            gMainGameMode = GM_DEBUG_MENU;
#else // !DEBUG
                            gMainGameMode = GM_TITLE;
#endif // DEBUG
                            gSubGameMode1 = 0;
                        }
                    }
                    else
                    {
                        gMainGameMode = GM_MAP_SCREEN;
                    }
                }
                break;

            case GM_MAP_SCREEN:
                if (PauseScreenHandler())
                {
                    gMainGameMode = gSubGameMode2;
                    gSubGameMode2 = 0;

                    switch (gPauseScreenFlag)
                    {
                        case PAUSE_SCREEN_UNKNOWN_1:
                            gSubGameMode3 = 0;

                        case PAUSE_SCREEN_SUITLESS_ITEMS:
                            gPauseScreenFlag = PAUSE_SCREEN_NONE;
                            break;

                        case PAUSE_SCREEN_UNKNOWN_9:
                            gPauseScreenFlag = PAUSE_SCREEN_NONE;
                            gSubGameMode2 = 1;
                            break;

                        case PAUSE_SCREEN_PAUSE_OR_CUTSCENE:
                        case PAUSE_SCREEN_UNKNOWN_3:
                        case PAUSE_SCREEN_CHOZO_HINT:
                        case PAUSE_SCREEN_MAP_DOWNLOAD:
                        case PAUSE_SCREEN_FULLY_POWERED_SUIT_ITEMS:
                            break;
                    }

                    gSubGameMode1 = 0;
                }
                break;

            case GM_GAMEOVER:
                if (GameOverHandler())
                {
                    gMainGameMode = gSubGameMode2;
                    gSubGameMode1 = 0;
                    gSubGameMode2 = 0;
                }
                break;

            case GM_CHOZODIA_ESCAPE:
                if (ChozodiaEscapeHandler())
                {
                    gSubGameMode1 = 0;
                    gMainGameMode = GM_CREDITS;
                }
                break;

            case GM_CREDITS:
                if (CreditsHandler())
                {
                    gSubGameMode1 = 0;
                    gMainGameMode = GM_INTRO;
#ifdef DEBUG
                    if (gBootDebugActive || gDebugMode)
                        gMainGameMode = GM_DEBUG_MENU;
#endif // DEBUG
                }
                break;

            case GM_TOURIAN_ESCAPE:
                if (TourianEscapeHandler())
                {
                    gSubGameMode1 = 0;
                    gMainGameMode = gSubGameMode2;
#ifdef DEBUG
                    if (gBootDebugActive)
                        gMainGameMode = GM_DEBUG_MENU;
#endif // DEBUG
                }
                break;

            case GM_CUTSCENE:
                if (CutsceneHandler())
                {
                    gSubGameMode1 = 0;

                    if (gPauseScreenFlag == PAUSE_SCREEN_SUITLESS_ITEMS || gPauseScreenFlag == PAUSE_SCREEN_FULLY_POWERED_SUIT_ITEMS)
                    {
                        gMainGameMode = GM_MAP_SCREEN;
                    }
                    else
                    {
                        gMainGameMode = GM_INGAME;
#ifdef DEBUG
                        if (gBootDebugActive)
                            gMainGameMode = gBootDebugActive;
#endif // DEBUG
                    }
                }
                break;

            case GM_DEMO:
                if (InGameHandler())
                {
                    if (gPauseScreenFlag == PAUSE_SCREEN_PAUSE_OR_CUTSCENE)
                    {
                        gPauseScreenFlag = PAUSE_SCREEN_NONE;
                        gSubGameMode3 = 0;
                        gSubGameMode1 = 0;
                        if (gDemoState == 0)
                        {
                            gMainGameMode = gSubGameMode2;
                            gSubGameMode2 = gCurrentDemo.endedWithInput;
                        }
                        else {
                            DemoStart();
                            gMainGameMode = GM_DEMO;
                        }
                    }
                    else
                        gMainGameMode = GM_MAP_SCREEN;
                }
                break;

            case GM_GALLERY:
                if (GalleryHandler())
                {
                    gSubGameMode1 = 0;
                    gMainGameMode = GM_FILE_SELECT;
                }
                break;

            case GM_FUSION_GALLERY:
                if (FusionGalleryHandler())
                {
                    gSubGameMode1 = 0;
                    gMainGameMode = GM_FILE_SELECT;
                }
                break;

            case GM_START_SOFT_RESET:
                SoftReset();
                break;

            case GM_ERASE_SRAM:
                if (EraseSramHandler())
                {
                    if (gSubGameMode2 == 1)
                    {
                        gResetGame = TRUE;
                    }
                    else
                    {
                        gMainGameMode = GM_SOFT_RESET;
#ifdef DEBUG
                        if (gDebugMode)
                            gMainGameMode = GM_DEBUG_MENU;
#endif // DEBUG
                    }

                    gSubGameMode1 = 0;
                    gSubGameMode2 = 0;
                }
                break;

            case GM_DEBUG_MENU:
#ifdef DEBUG
                if (BootDebugHandler())
                {
                    gSubGameMode1 = 0;

                    switch (gSubGameMode2)
                    {
                        case 1:
                            gMainGameMode = GM_INGAME;
                            break;
                        case 2:
#ifdef REGION_EU
                            if (INVALID_EU_LANGUAGE(gLanguage))
                            {
                                gMainGameMode = GM_SOFT_RESET;
                                gSubGameMode1 = sLanguageSelectGameModeSub1Values[2];
                            }
                            else
#endif // REGION_EU
                            {
                                gMainGameMode = GM_INTRO;
                            }
                            break;
                        case 3:
                            gMainGameMode = GM_MAP_SCREEN;
                            break;
                        case 6:
                            gMainGameMode = GM_DEMO;
                            break;
                        case 8:
                            gMainGameMode = GM_CUTSCENE;
                            gSubGameMode2 = 0x10;
                            break;
                        case 7:
                            gMainGameMode = GM_TOURIAN_ESCAPE;
                            gSubGameMode2 = 0x10;
                            break;
                        case 5:
                            gMainGameMode = GM_CREDITS;
                            break;
                        case 4:
                            gMainGameMode = GM_CHOZODIA_ESCAPE;
                            break;
                        default:
                            gMainGameMode = GM_DEBUG_MENU;
                            gWrittenToBldy_NonGameplay = 0;
                            break;
                    }
                }
#else // !DEBUG
                gMainGameMode = GM_INTRO;
                gSubGameMode1 = 0;
                break;
#endif // DEBUG
                break;
        }
        

#if defined(TMC_3DS) && defined(PORT_VERBOSE_FRAME_LOG)
        Port_DebugLog("agbmain: switch done, before Halt wait");
#endif
        gVBlankRequestFlag &= ~TRUE;
        gVblankActive = TRUE;

        do {
            SYSCALL(2); /* SYS_Halt */
        } while (!(gVBlankRequestFlag & 1));
#if defined(TMC_3DS) && defined(PORT_VERBOSE_FRAME_LOG)
        Port_DebugLog("agbmain: Halt wait done, looping");
#endif
    }
}
