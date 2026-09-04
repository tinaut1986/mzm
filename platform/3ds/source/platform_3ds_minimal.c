/*
 * Platform3DS implementation for the Metroid: Zero Mission 3DS port.
 *
 * Implements the platform_3ds.h contract: service init, New3DS detection,
 * input handling, timing, and lifecycle management.
 */
#include "platform_3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

static bool sIsNew3DS;
static bool sRunning;
static bool sGameplayDisplayActive;
static bool sCore1Available;
static unsigned sCore1TimeLimit;

/* Body lives in port/port_gba_timing.c (plain C, no <3ds.h> -- see that
 * file's header comment for why). Spawned here because this file is the
 * one place that safely includes the real <3ds.h> for threadCreate's exact
 * signature; the port/ side only needs a single-argument svcSleepThread
 * forward declaration, much lower ABI risk than getting threadCreate's
 * five-parameter signature wrong from a hand-written declaration. */
extern void Port_GbaTiming_ThreadMain(void* arg);

/* Game-logic thread (agbmain), see Platform3DS_StartLogicThread below. */
static Thread sLogicThread;
static void (*sLogicThreadEntry)(void);

/* Held by the logic thread whenever it isn't inside Port_Bios_Halt's
 * render+pace section -- see port_mzm_audio_3ds.c's sAudioStateLock doc
 * comment. Must be acquired before agbmain() runs anything (InitializeGame
 * can trigger sound setup before the first VBlankIntrWait call). */
extern void Port_AudioStateLock_Acquire(void);

static void Platform3DS_LogicThreadTrampoline(void* arg) {
    (void)arg;
    Port_AudioStateLock_Acquire();
    sLogicThreadEntry();
    /* agbmain() never returns in practice -- it's an infinite loop that only
     * ever leaves the process via exit(0) inside Port_Bios_Halt. Nothing
     * sensible to do here if it ever does return. */
}

bool Platform3DS_StartLogicThread(void (*entry)(void)) {
    sLogicThreadEntry = entry;

    /* Same priority pattern as the NDSP audio thread (port_mzm_audio_3ds.c):
     * one step above whatever this (main/present) thread's own priority is,
     * so game logic and audio production aren't starved by the present
     * thread's GPU-bound loop sharing the same core. Default/-1 core: this
     * thread replaces exactly what main() used to do directly, so it keeps
     * main()'s own core budget rather than claiming a new one. */
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    if (priority > 0x18) --priority;

    sLogicThread = threadCreate(Platform3DS_LogicThreadTrampoline, NULL, 32 * 1024, priority, -1, false);
    return sLogicThread != NULL;
}

/* See Platform3DS_IsActiveStackAddress's doc comment: needed to scope that
 * check to the one thread it was designed for. */
Thread Platform3DS_GetLogicThreadHandle(void) {
    return sLogicThread;
}

int Platform3DS_Init(void) {
    gfxInitDefault();

    APT_CheckNew3DS(&sIsNew3DS);
#ifdef PORT_FORCE_OLD3DS_PROFILE
    /* Build-time override (see platform/3ds/Makefile's FORCE_OLD3DS) to make
     * real New3DS hardware behave like an Old3DS for benchmarking: every
     * downstream consumer of Platform3DS_IsNew3DS() (osSetSpeedupEnable
     * below, the Core1 grant right after, virtuappu_mode1's old3ds profile
     * and PlatformGpu3DS_Init's oldProfile flag in port_ppu_mzm.c, and
     * adaptiveFrameskipEnabled) reads sIsNew3DS rather than re-detecting the
     * console, so forcing it false here is enough to flip all of them at
     * once. Note this genuinely changes the CPU's running clock (New3DS
     * stays at Old3DS's base clock without osSetSpeedupEnable) and core
     * count (no Core1 grant), not just a simulated slowdown -- this is what
     * Old3DS hardware would actually give the game. */
    sIsNew3DS = false;
#endif
    if (sIsNew3DS) {
        osSetSpeedupEnable(true);
    }

    /* APT_SetAppCpuTimeLimit grants userland time on Core 1 (the syscore,
     * normally reserved for system services) -- it can fail silently
     * depending on the exheader/access-control the CIA was built with, so
     * the return value must actually be checked and the grant re-verified
     * with APT_GetAppCpuTimeLimit. Getting this wrong previously meant
     * Platform3DS_CanUseCore1() unconditionally trusted sIsNew3DS, mode1.c's
     * render worker for Core 1 tried to spawn there anyway, silently failed
     * threadCreate, and the port permanently rendered with one fewer worker
     * thread than intended -- lost ~1/3 of the CPU render's parallelism
     * without any visible error. Try decreasing candidate percentages since
     * a lower ask is more likely to be granted than a fixed 80/30. */
    static const u32 core1Candidates[] = { 80, 70, 50, 30 };
    for (size_t i = 0; i < sizeof(core1Candidates) / sizeof(core1Candidates[0]); ++i) {
        if (R_FAILED(APT_SetAppCpuTimeLimit(core1Candidates[i]))) continue;
        u32 actual = 0;
        if (R_SUCCEEDED(APT_GetAppCpuTimeLimit(&actual)) && actual > 0) {
            sCore1Available = true;
            sCore1TimeLimit = actual;
            break;
        }
    }

    threadCreate(Port_GbaTiming_ThreadMain, NULL, 4096, 0x20, -1, true);

    sRunning = true;
    return 1;
}

void Platform3DS_Shutdown(void) {
    gfxExit();
    sRunning = false;
}

bool Platform3DS_IsRunning(void) {
    sRunning = sRunning && aptMainLoop();
    return sRunning;
}

bool Platform3DS_IsNew3DS(void) {
    return sIsNew3DS;
}

bool Platform3DS_CanUseCore1(void) {
    return sCore1Available;
}

unsigned Platform3DS_Core1TimeLimit(void) {
    return sCore1TimeLimit;
}

void Platform3DS_ShowSplash(void) {
    printf("\x1b[2J"); /* clear */
    printf("Metroid Zero Mission 3DS\n\n");
}

static bool DummyPrint(void* console, int character) {
    (void)console;
    (void)character;
    return true;
}

void Platform3DS_EnterGameplayDisplay(void) {
    static PrintConsole sDummyConsole;
    sDummyConsole = *consoleGetDefault();
    sDummyConsole.PrintChar = DummyPrint;
    sDummyConsole.consoleInitialised = true;
    consoleSelect(&sDummyConsole);
    sGameplayDisplayActive = true;
}

uint16_t Platform3DS_ReadKeyInput(void) {
    hidScanInput();
    return (uint16_t)hidKeysHeld();
}

/* Bridges real 3DS button state into the emulated GBA REG_KEY_INPUT, which
 * update_input.c's UpdateInput() reads every frame (active-low: 0 bit =
 * pressed). Nothing called this anywhere before -- REG_KEY_INPUT sat at its
 * zero-initialized default forever, which UpdateInput() reads as EVERY key
 * held down simultaneously (~0 & KEY_MASK). That includes the soft-reset
 * combo (A+B+START+SELECT), so the game perpetually re-triggered SoftReset()
 * every single frame from boot -- see soft_reset_input.c's SoftResetCheck().
 * libctru's KEY_A..KEY_L (hid.h) happen to share the exact same bit
 * positions 0-9 as GBA's KEY_A..KEY_L (gba/keys.h), so masking to the low 10
 * bits needs no remapping table (unlike the Linux X11 keysym path in
 * platform_linux.c, which does need one).
 *
 * gba_write16 forward-declared instead of #include "port_gba_mem.h": that
 * header pulls in mzm's types.h, whose u32/s32/... typedefs conflict with
 * <3ds.h>'s own (already included above) the moment both are visible in one
 * translation unit -- same reason port_bios.c/port_gba_timing.c avoid it. */
extern void gba_write16(uint32_t addr, uint16_t v);
#define MZM_REG_KEY_INPUT 0x04000130u
#define MZM_KEY_MASK 0x3FFu

/* KEY_X and KEY_Y (bits 10/11) fall outside MZM_KEY_MASK (bits 0-9, GBA's
 * own button set), so neither reaches the game directly -- both are
 * user-configurable action buttons instead (see ProcessButtonAction below).
 * They used to carry secondary diagnostics functions behind an L+R+<button>
 * chord; those all moved to the bottom screen's DEBUG -> HERRAMIENTAS menu
 * (port_bottom_ui_3ds.c) so nothing here competes with the player's own
 * button mapping anymore. */
extern void Port_DebugLog(const char* msg);

extern int Port_Config_GetButtonMapping(int buttonIndex);
extern int Port_Config_GetCstickMode(void);
extern int Port_Samus_GetPoseClass(void); /* 0 = normal, 1 = crouching, 2 = morphed */

static uint16_t ProcessButtonAction(int action, uint32_t keysHeld, uint32_t keysDown, uint32_t buttonMask, bool* outMorphPulse, bool* outDiagAim) {
    if (!(keysHeld & buttonMask)) return 0;

    switch (action) {
        case 1: { /* AUTODISPARO (RAPID FIRE) - disabled in RA hardcore mode */
            extern bool Port_RA_IsHardcore(void);
            static uint32_t sRapidCounter = 0;
            ++sRapidCounter;
            if (Port_RA_IsHardcore()) return 0;
            /* Pulse KEY_B every other frame (30 shots/sec) */
            return (sRapidCounter & 1) ? (1 << 1) : 0;
        }
        case 2: { /* MORFOSFERA RAPIDA (QUICK MORPH) - disabled in RA hardcore mode */
            extern bool Port_RA_IsHardcore(void);
            if (Port_RA_IsHardcore()) return 0;
            if (keysDown & buttonMask) {
                if (outMorphPulse) *outMorphPulse = true;
            }
            return 0;
        }
        case 3: return (1 << 1); /* DISPARO (KEY_B) */
        case 4: return (1 << 0); /* SALTO (KEY_A) */
        case 5: return (1 << 2); /* MISILES (KEY_SELECT) */
        case 6: { /* APUNTAR (KEY_L) */
            if (outDiagAim) *outDiagAim = true;
            return (1 << 9); /* KEY_L */
        }
        case 7: return (1 << 8); /* ARMAR MISILES (KEY_R) */
        case 8: return (1 << 3); /* PAUSA (KEY_START) */
        default: return 0;
    }
}

void Platform3DS_PollKeysIntoGba(void) {
    hidScanInput();
    const uint32_t held3ds = hidKeysHeld();
    const uint32_t down3ds = hidKeysDown();

    /* 1. D-Pad direct passthrough (fixed to movement, bits 4-7) + Circle Pad
     *    (analog stick, always maps to movement regardless of config). */
    uint16_t gbaKeys = (uint16_t)((held3ds & MZM_KEY_MASK) & 0xF0); /* only D-Pad bits 4-7 */

    circlePosition circle;
    hidCircleRead(&circle);
    const int16_t deadzone = 40;
    if (circle.dx > deadzone) gbaKeys |= (1 << 4);  /* KEY_RIGHT */
    if (circle.dx < -deadzone) gbaKeys |= (1 << 5); /* KEY_LEFT */
    if (circle.dy > deadzone) gbaKeys |= (1 << 6);  /* KEY_UP */
    if (circle.dy < -deadzone) gbaKeys |= (1 << 7); /* KEY_DOWN */

    bool hasDiagAim = false;

    /* 2. C-Stick / New 3DS Right Analog (configurable via cstick_mode) */
    circlePosition cstick;
    irrstCstickRead(&cstick);
    int cstickMode = Port_Config_GetCstickMode();
    /* cstickMode: 0=DESACTIVADO (OFF), 1=SOLO APUNTAR (UP/DOWN ONLY), 2=SOLO MOVIMIENTO (LEFT/RIGHT ONLY), 3=TODO (4 DIRECCIONES COMPLETAS) */
    if (cstickMode == 1) {
        /* SOLO APUNTAR: arriba y abajo (permite apuntar hacia arriba o abajo mientras se corre) */
        if (cstick.dy > deadzone) gbaKeys |= (1 << 6);  /* KEY_UP */
        if (cstick.dy < -deadzone) gbaKeys |= (1 << 7); /* KEY_DOWN */
    } else if (cstickMode == 2) {
        /* SOLO MOVIMIENTO: adelante y atras (horizontal) */
        if (cstick.dx > deadzone) gbaKeys |= (1 << 4);  /* KEY_RIGHT */
        if (cstick.dx < -deadzone) gbaKeys |= (1 << 5); /* KEY_LEFT */
    } else if (cstickMode == 3) {
        /* TODO: movimiento y apuntar/agacharse (4 direcciones completas) */
        if (cstick.dx > deadzone) gbaKeys |= (1 << 4);
        if (cstick.dx < -deadzone) gbaKeys |= (1 << 5);
        if (cstick.dy > deadzone) gbaKeys |= (1 << 6);
        if (cstick.dy < -deadzone) gbaKeys |= (1 << 7);
    }

    /* 3. (was: L+R+<button> debug modifier.) Removed -- every debug tool
     *    it used to trigger now lives behind the bottom screen's
     *    DEBUG -> HERRAMIENTAS menu (port_bottom_ui_3ds.c). The combos
     *    stole L/R/X/Y/START/SELECT from whatever the player had mapped
     *    them to for the frame they fired, and were easy to trip by
     *    accident; the on-screen menu has neither problem and needs no
     *    memorizing. See docs/3ds-debug-tools.md. */

    /* 4. Suppress passthrough for ALL remappable buttons (A/B/X/Y/L/R/ZL/ZR/Start/Select).
     *    D-Pad (bits 4-7) and Circle Pad are already handled above.
     *    Only suppress when L+R debug modifier is NOT active. */
    {
        gbaKeys &= ~(1u << 0); /* KEY_A */
        gbaKeys &= ~(1u << 1); /* KEY_B */
        gbaKeys &= ~(1u << 2); /* KEY_SELECT */
        gbaKeys &= ~(1u << 3); /* KEY_START */
        gbaKeys &= ~(1u << 8); /* KEY_R */
        gbaKeys &= ~(1u << 9); /* KEY_L */
    }

    /* 5. Process all 10 remappable buttons through ProcessButtonAction.
     *    Index mapping: 0=A, 1=B, 2=X, 3=Y, 4=L, 5=R, 6=ZL, 7=ZR, 8=Start, 9=Select.
     *    Physical button -> config index -> action -> GBA bits. */
    static const uint32_t btnMasks[10] = {
        KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_ZL, KEY_ZR, KEY_START, KEY_SELECT
    };

    bool quickMorphPulse = false;

    {
        for (int i = 0; i < 10; ++i) {
            int action = Port_Config_GetButtonMapping(i);
            gbaKeys |= ProcessButtonAction(action, held3ds, down3ds, btnMasks[i], &quickMorphPulse, &hasDiagAim);
        }
    }

    /* 6. Diagonal aim modifier: inject virtual bit 10 into gbaKeys.
     *    SamusAimCannon (samus.c) is modified to also check bit 10. */
    if (hasDiagAim) gbaKeys |= (1u << 10);

    /* Quick Morph toggle handling (unchanged) */
    static int sQuickMorphGoalClass = -1;
    static int sQuickMorphPhaseTimer = 0;
    static bool sQuickMorphPressing = false;
    static int sQuickMorphSafetyFrames = 0;
    if (quickMorphPulse && sQuickMorphGoalClass < 0) {
        sQuickMorphGoalClass = (Port_Samus_GetPoseClass() == 2) ? 0 : 2;
        sQuickMorphPhaseTimer = 0;
        sQuickMorphPressing = false;
        sQuickMorphSafetyFrames = 120;
    }
    {
        /* Hardcore can be turned on while a sequence is mid-flight; drop it
         * so the assist never touches gameplay that counts. */
        extern bool Port_RA_IsHardcore(void);
        if (sQuickMorphGoalClass >= 0 && Port_RA_IsHardcore()) {
            sQuickMorphGoalClass = -1;
        }
    }
    if (sQuickMorphGoalClass >= 0) {
        const uint16_t morphKey = (sQuickMorphGoalClass == 2) ? (1 << 7) : (1 << 6);
        /* Morphing while running: on the ground a running Samus reads a Down
         * press as "aim the cannon down", not "crouch -> ball", so the pulse
         * just bobs her arm until she happens to stop. Releasing left/right
         * for the handful of frames the sequence lasts brings her to a stand
         * and the Down pulse balls her up immediately. Unmorph (goal 0) needs
         * no such help. */
        if (sQuickMorphGoalClass == 2 && Port_Samus_GetPoseClass() != 2) {
            gbaKeys &= ~((1u << 4) | (1u << 5)); /* clear KEY_RIGHT | KEY_LEFT */
        }
        if (Port_Samus_GetPoseClass() == sQuickMorphGoalClass || --sQuickMorphSafetyFrames <= 0) {
            sQuickMorphGoalClass = -1;
        } else if (sQuickMorphPhaseTimer > 0) {
            --sQuickMorphPhaseTimer;
            if (sQuickMorphPressing) gbaKeys |= morphKey;
        } else if (sQuickMorphPressing) {
            sQuickMorphPressing = false;
            sQuickMorphPhaseTimer = 4;
        } else {
            sQuickMorphPressing = true;
            sQuickMorphPhaseTimer = 4;
            gbaKeys |= morphKey;
        }
    }

    /* Alternative soft-reset combo: L+R+START+SELECT on the physical pad, on
     * top of the game's own A+B+START+SELECT (SOFT_RESET_KEYS in
     * src/soft_reset_input.c, the retail GBA combo). Checked on the raw
     * hardware state, so it works whatever the buttons are remapped to, and
     * implemented by injecting the four keys the game already looks for --
     * that way it goes through SoftResetCheck() and keeps honouring
     * gDisableSoftReset instead of forcing a mode switch from outside. */
    if ((held3ds & KEY_L) && (held3ds & KEY_R) && (held3ds & KEY_START) && (held3ds & KEY_SELECT)) {
        gbaKeys |= (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3); /* A | B | SELECT | START */
    }

    /* Reset trace (issue: a reset reportedly happens on L+R+X in a build with
     * no debug combos compiled in). Logs once per transition into "the game
     * can see the soft-reset combo", with the raw 3DS button bits, so the log
     * distinguishes a real key-combo reset from one coming through the bottom
     * screen's RESTART button (Port_DebugLog in TriggerGameRestart) or from
     * neither -- which would point at a crash/other path instead. */
    {
        static bool sResetComboWasSeen = false;
        const uint16_t resetBits = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3);
        const bool comboSeen = (gbaKeys & resetBits) == resetBits;
        if (comboSeen && !sResetComboWasSeen) {
            char msg[96];
            __builtin_snprintf(msg, sizeof(msg),
                "RESET TRACE: soft-reset combo visible to game, raw3ds=0x%08X gba=0x%04X",
                (unsigned)held3ds, (unsigned)gbaKeys);
            Port_DebugLog(msg);
        }
        sResetComboWasSeen = comboSeen;
    }

    gba_write16(MZM_REG_KEY_INPUT, (uint16_t)(~gbaKeys & MZM_KEY_MASK));

    if (held3ds & KEY_TOUCH) {
        touchPosition touch;
        hidTouchRead(&touch);
        bool isNewTap = (down3ds & KEY_TOUCH) != 0;
        extern void Port_BottomUI_HandleTouchDrag(int x, int y, bool isNewTap);
        Port_BottomUI_HandleTouchDrag(touch.px, touch.py, isNewTap);
    } else if (hidKeysUp() & KEY_TOUCH) {
        extern void Port_BottomUI_TouchReleased(void);
        Port_BottomUI_TouchReleased();
    }
}

uint16_t Platform3DS_ReadKeyDownInput(void) {
    return (uint16_t)hidKeysDown();
}

uint32_t Platform3DS_KeysHeld(void) {
    return hidKeysHeld();
}

void Platform3DS_ReadCircle(float* x, float* y) {
    circlePosition pos;
    hidCircleRead(&pos);
    if (x) *x = (float)pos.dx / 156.0f;
    if (y) *y = (float)pos.dy / 156.0f;
}

uint16_t* Platform3DS_GetFramebuffer(int top, uint16_t* width, uint16_t* height) {
    /* No GPU/citro2d presentation path yet (port_ppu_3ds.c not adapted) --
     * callers can't get a real framebuffer to draw into on this build. */
    (void)top;
    if (width) *width = 0;
    if (height) *height = 0;
    return NULL;
}

uint64_t Platform3DS_Milliseconds(void) {
    return osGetTime();
}

uint64_t Platform3DS_SystemTick(void) {
    return svcGetSystemTick();
}

uint64_t Platform3DS_TicksPerSecond(void) {
    return SYSCLOCK_ARM11;
}

int Platform3DS_IsNativeAddress(uintptr_t value) {
    (void)value;
    return 0;
}

int Platform3DS_IsActiveStackAddress(uintptr_t value) {
    /* This heuristic was calibrated against the game-logic thread's own
     * stack (where SramWrite/SramTestFlash -- the calls that motivated it --
     * always run from). It must NOT be applied when called from any other
     * thread: the NDSP audio thread (port_mzm_audio_3ds.c) got its own
     * separate stack once audio production moved off the logic thread, and
     * that stack's address can legitimately land within +/-64KB of a real
     * GBA ROM address purely by coincidence of allocation -- which made
     * port_resolve_addr() misclassify genuine ROM pointers (pRawData,
     * pVoice, ...) read by InitTrack/UpdateTrack as native stack pointers
     * and return them unresolved, silently breaking music while leaving PSG
     * SFX (which don't chase ROM pointers the same way) unaffected. Since
     * threadGetCurrent() can only be called safely once <3ds.h> is usable
     * (true in this file), compare against the recorded logic-thread handle
     * and skip the check entirely for every other thread. */
    extern Thread Platform3DS_GetLogicThreadHandle(void);
    if (threadGetCurrent() != Platform3DS_GetLogicThreadHandle()) {
        return 0;
    }

    uintptr_t currentSp;
    __asm__ volatile("mov %0, sp" : "=r"(currentSp));

    /* GBA ROM addresses (0x08000000..0x08000000+gRomSize) may numerically
     * overlap the 3DS main-thread stack reservation, and stack locals passed
     * straight to the GBA SRAM helpers (SramTestFlash, SramWrite*, etc.)
     * land right there. Only objects close to the current frame are
     * unambiguously native stack pointers; keeping this window bounded
     * preserves normal GBA ROM address mapping. */
    const uintptr_t window = 64u * 1024u;
    return value >= currentSp - window && value <= currentSp + window;
}

bool Platform3DS_SubmitBottomWorker(void) {
    return false; /* No second-screen worker yet. */
}

bool Platform3DS_TryFinishBottomWorker(void) {
    return false;
}

void Platform3DS_ShutdownBottomWorker(void) {}

void Platform3DS_MarkFrameDiscontinuity(Old3DSFramePacerDiscontinuity reason) {
    (void)reason;
}

bool Platform3DS_BeginFrameBoundary(void) {
    return true;
}

void Platform3DS_EndFrameBoundary(void) {}

void Platform3DS_PumpWithoutVBlank(void) {
    hidScanInput();
}

bool Platform3DS_TurboHeld(void) {
    return false;
}

unsigned Platform3DS_TurboMultiplier(void) {
    return 1u;
}

void Platform3DS_SetTurboMultiplier(unsigned multiplier) {
    (void)multiplier;
}

void Platform3DS_GetRuntimeStats(Platform3DSRuntimeStats* stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
}

void Platform3DS_WaitForVBlank(void) {
    gspWaitForVBlank();
}

void Platform3DS_ShowFatal(const char* title, const char* message) {
    consoleInit(GFX_TOP, NULL);
    printf("\x1b[2J");
    printf("FATAL: %s\n\n%s\n\nPress START to exit.\n", title, message);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

void Platform3DS_Debug(const char* message) {
    printf("%s\n", message);
}

bool Platform3DS_SaveDisplayedScreens(const char* topPath, const char* bottomPath) {
    (void)topPath;
    (void)bottomPath;
    return false; /* No framebuffer to capture yet. */
}

bool Platform3DS_SaveDisplayedScreensDetailed(const char* topPath, const char* bottomPath, const char* topRawPath,
                                               const char* bottomRawPath, Platform3DSCaptureStats* stats) {
    (void)topPath;
    (void)bottomPath;
    (void)topRawPath;
    (void)bottomRawPath;
    if (stats) memset(stats, 0, sizeof(*stats));
    return false;
}
