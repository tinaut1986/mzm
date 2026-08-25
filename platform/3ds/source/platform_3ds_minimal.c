/*
 * Platform3DS implementation for the Metroid: Zero Mission 3DS port.
 *
 * Implements the platform_3ds.h contract: service init, New3DS detection,
 * input handling, timing, and lifecycle management.
 */
#include "platform_3ds.h"
#include "port_debug_tools.h"

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
 * They also each carry a secondary diagnostics function, gated behind
 * holding L+R at the same time (see the lrHeld block in
 * Platform3DS_PollKeysIntoGba) so it doesn't collide with whatever gameplay
 * action X/Y are currently assigned to: L+R+X dumps the actual GPU-rendered
 * content of the left/right/bottom render targets to the SD card
 * (PlatformGpu3DS_DumpScreens, in platform_gpu_3ds.c) -- added to inspect
 * what the right-eye stereo target really contains when the 3D slider is on,
 * since the CPU-side debug log alone couldn't explain reported content
 * missing there. Declared extern rather than #include "platform_gpu_3ds.h"
 * purely out of consistency with this file's existing style of narrow
 * forward declarations. */
extern void PlatformGpu3DS_DumpScreens(void);

/* L+R+START toggles the scene recorder (PlatformGpu3DS_ToggleRecording, in
 * platform_gpu_3ds.c): first press starts sampling emulated GBA state to
 * sdmc:/3ds/mzm-rec.bin every few frames, second press stops and closes the
 * file. See that function's doc comment for why it exists -- a single L+R+X
 * dump couldn't reliably catch the exact moment a fast-changing scene (e.g.
 * issue #17's death animation) actually breaks. */
extern void PlatformGpu3DS_ToggleRecording(void);

/* L+R+Y drops a timestamped "USER MARK" line into mzm-debug.log so a play
 * session can flag "something happened right here" (e.g. a visible CPU/GPU
 * renderer fallback) without having to describe timing after the fact --
 * grep the log for "USER MARK" and read the surrounding GPU_REJECT/GPUDIAG
 * lines (needs -DPORT_GPU_RENDERER_DIAG_LOG for those) or ModeChange lines
 * to see what the game was doing at that exact moment. Unbuffered
 * (Port_DebugLog, not Port_DebugLogBuffered) so the mark is flushed to disk
 * immediately even if the app is killed moments later. */
extern void Port_DebugLog(const char* msg);

extern int Port_Config_GetButtonMapping(int buttonIndex);
extern int Port_Samus_GetPoseClass(void); /* 0 = normal, 1 = crouching, 2 = morphed */

static uint16_t ProcessButtonAction(int action, uint32_t keysHeld, uint32_t keysDown, uint32_t buttonMask, bool* outMorphPulse) {
    if (!(keysHeld & buttonMask)) return 0;

    switch (action) {
        case 1: { /* AUTODISPARO (RAPID FIRE) */
            static uint32_t sRapidCounter = 0;
            ++sRapidCounter;
            /* Pulse KEY_B every other frame (30 shots/sec) */
            return (sRapidCounter & 1) ? (1 << 1) : 0;
        }
        case 2: { /* MORFOSFERA RAPIDA (QUICK MORPH) */
            if (keysDown & buttonMask) {
                if (outMorphPulse) *outMorphPulse = true;
            }
            return 0;
        }
        case 3: return (1 << 1); /* DISPARO (KEY_B) */
        case 4: return (1 << 0); /* SALTO (KEY_A) */
        case 5: return (1 << 2); /* MISILES (KEY_SELECT) */
        case 6: return (1 << 8); /* APUNTAR ARRIBA (KEY_R) */
        case 7: return (1 << 9); /* APUNTAR ABAJO (KEY_L) */
        default: return 0;
    }
}

void Platform3DS_PollKeysIntoGba(void) {
    hidScanInput();
    const uint32_t held3ds = hidKeysHeld();
    const uint32_t down3ds = hidKeysDown();

    /* Standard GBA buttons from 3DS D-Pad / Face buttons */
    uint16_t gbaKeys = (uint16_t)(held3ds & MZM_KEY_MASK);

    /* 1. Circle Pad input (analog stick -> D-Pad) */
    circlePosition circle;
    hidCircleRead(&circle);
    const int16_t deadzone = 40;
    if (circle.dx > deadzone) gbaKeys |= (1 << 4);  /* KEY_RIGHT */
    if (circle.dx < -deadzone) gbaKeys |= (1 << 5); /* KEY_LEFT */
    if (circle.dy > deadzone) gbaKeys |= (1 << 6);  /* KEY_UP */
    if (circle.dy < -deadzone) gbaKeys |= (1 << 7); /* KEY_DOWN */

    /* 2. C-Stick / New 3DS Right Analog Stick (Aim / Directions) */
    circlePosition cstick;
    irrstCstickRead(&cstick);
    if (cstick.dx > deadzone) gbaKeys |= (1 << 4);
    if (cstick.dx < -deadzone) gbaKeys |= (1 << 5);
    if (cstick.dy > deadzone) gbaKeys |= (1 << 8);  /* C-Stick Up -> Aim Up (R) */
    if (cstick.dy < -deadzone) gbaKeys |= (1 << 9); /* C-Stick Down -> Aim Down (L) */

    /* Only actually reserved as a diagnostics modifier in a debug build
     * (see PORT_DEBUG_TOOLS_ACTIVE below) -- stays false in a production
     * build, so section 3 below never suppresses X/Y's normal configurable
     * action and L+R+<button> carries no special meaning at all, just
     * whatever plain input that combination naturally produces. */
    bool lrHeld = false;
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    /* L+R held together is reserved as a diagnostics modifier, freeing X/Y
     * up from their normal configurable gameplay actions for that instant:
     * L+R+X dumps the real GPU-rendered screen content to the SD card
     * (PlatformGpu3DS_DumpScreens -- this used to be plain KEY_X before X
     * became a user-configurable action button, see the comment below),
     * L+R+Y drops a timestamped "USER MARK" line into mzm-debug.log so a
     * play session can flag a moment for later grep'ing without describing
     * timing after the fact. Edge-triggered on down3ds so each physical
     * press dumps/logs exactly once. */
    lrHeld = (held3ds & KEY_L) && (held3ds & KEY_R);
    if (lrHeld) {
        if (down3ds & KEY_X) PlatformGpu3DS_DumpScreens();
        if (down3ds & KEY_Y) Port_DebugLog("USER MARK: L+R+Y pressed");
        if (down3ds & KEY_START) PlatformGpu3DS_ToggleRecording();
        /* L+R+SELECT: debug instant-kill (issue #17 -- lets a session iterate
         * on the death animation without having to actually get hit down to
         * 0 energy in real gameplay every time). Physical SELECT isn't wired
         * to anything else in this file (unlike X/Y/START it's not one of
         * the fixed diagnostics buttons nor a 1:1 GBA passthrough -- only
         * ever appears as a value ProcessButtonAction can return for a
         * *configurable* X/Y/ZL/ZR mapping below, gated by `!lrHeld`), so no
         * masking is needed the way KEY_START needs. Lives in
         * port_ppu_mzm.c (see PortPpuMzm_DebugKillSamus's comment) for the
         * same <3ds.h>/structs-samus.h conflict reason
         * PortPpuMzm_GetSamusRecordState does. */
        if (down3ds & KEY_SELECT) {
            extern void PortPpuMzm_DebugKillSamus(void);
            PortPpuMzm_DebugKillSamus();
        }
        /* L+R+B: issue #17 -- live atlas dump, any time during gameplay.
         * Doesn't need to catch an exact frame: once the death-sequence
         * corruption starts it stays fragmented for the rest of the
         * flash-hold phase (confirmed via the 2026-08-25 recorder session,
         * screenshots 0020 through 0044 all showed the same fragmented
         * look), so pressing this any time after the screen visibly looks
         * wrong is enough. Reuses Port_GpuRenderer_DumpAtlas against
         * whatever's actually resident in the atlas right now -- this is
         * what confirmed "DecodeTileIntoSlot writes wrong pixels on an
         * in-place STALE redecode" is NOT the cause (see
         * docs/3ds-issue17-session-2026-08-25.md). The one-shot fixture-
         * replay harness this superseded (L+R+A, loading a captured sample
         * into live state and rendering a single isolated frame) was
         * removed -- its result turned out misleading (looked correct in a
         * way that didn't generalize to live play, see that doc's Update 1)
         * and this live version answers the same question better. */
        if (down3ds & KEY_B) {
            extern void Port_GpuRenderer_DumpAtlas(const char* ppmPath, const char* csvPath);
            Port_GpuRenderer_DumpAtlas("sdmc:/3ds/mzm-live-atlas.ppm", "sdmc:/3ds/mzm-live-atlas-keys.csv");
        }
        /* START is a real GBA button (bit 3, pause menu) unlike X/Y, so
         * unlike those this combo would otherwise also pause the game --
         * mask it out of gbaKeys for as long as it's held with L+R, not just
         * on the down-edge frame. A first version only cleared it inside the
         * `down3ds & KEY_START` branch above, so it only worked for exactly
         * one frame: holding START a little longer (which is normal -- a
         * "press" easily spans 2+ frames at 60fps) left `held3ds & KEY_START`
         * back in gbaKeys on every frame after that one, opening the pause
         * menu anyway. Same reasoning applies to B (bit 1, shoot). */
        if (held3ds & KEY_START) gbaKeys &= (uint16_t)~(1u << 3);
        if (held3ds & KEY_B) gbaKeys &= (uint16_t)~(1u << 1);
    }
#endif /* PORT_DEBUG_TOOLS_ACTIVE */

    /* 3. Configurable Extra Buttons: X, Y, ZL, ZR (suppressed while L+R is
     * held AND a debug build actually reserves that combo -- see lrHeld
     * above -- so the diagnostics combo doesn't also fire whatever gameplay
     * action the player has assigned to X/Y). */
    bool quickMorphPulse = false;
    if (!lrHeld) {
        gbaKeys |= ProcessButtonAction(Port_Config_GetButtonMapping(0), held3ds, down3ds, KEY_X, &quickMorphPulse);
        gbaKeys |= ProcessButtonAction(Port_Config_GetButtonMapping(1), held3ds, down3ds, KEY_Y, &quickMorphPulse);
    }
    gbaKeys |= ProcessButtonAction(Port_Config_GetButtonMapping(2), held3ds, down3ds, KEY_ZL, &quickMorphPulse);
    gbaKeys |= ProcessButtonAction(Port_Config_GetButtonMapping(3), held3ds, down3ds, KEY_ZR, &quickMorphPulse);

    /* Quick Morph toggle handling.
     *
     * This is goal-directed rather than a fixed-length pulse, because both
     * directions need a *variable* number of KEY_DOWN/KEY_UP edges depending
     * on animation timing, not just one:
     *   - Entering ball form: standing -> crouching on the first DOWN edge,
     *     crouching -> morphing on a second DOWN edge (samus.c's
     *     SamusStanding/SamusCrouching). Goal: pose class reaches MORPHED.
     *   - Leaving ball form: one UP edge starts unmorphing
     *     (SamusMorphball's "Check unmorphing" block), but that only lands
     *     Samus in CROUCHING once the animation ends (SamusUnmorphingGfx
     *     hard-sets it) -- a *second* UP edge is then needed to actually
     *     stand up (SamusCrouching's own KEY_UP check). Goal: pose class
     *     reaches NORMAL (not just "no longer morphed"), so Samus doesn't
     *     end up stuck crouching after unmorphing.
     * Each attempted edge is a short press with a release gap after it (so
     * a held key still produces a fresh gChangedInput edge next attempt),
     * repeated every frame the goal isn't met yet, with a safety frame cap
     * in case a pose transition doesn't behave as expected. */
    static int sQuickMorphGoalClass = -1; /* -1 = idle, else target Port_Samus_GetPoseClass() value */
    static int sQuickMorphPhaseTimer = 0; /* frames left in the current press or release micro-step */
    static bool sQuickMorphPressing = false;
    static int sQuickMorphSafetyFrames = 0;
    if (quickMorphPulse && sQuickMorphGoalClass < 0) {
        sQuickMorphGoalClass = (Port_Samus_GetPoseClass() == 2) ? 0 : 2;
        sQuickMorphPhaseTimer = 0;
        sQuickMorphPressing = false;
        sQuickMorphSafetyFrames = 120; /* ~2s at 60fps */
    }
    if (sQuickMorphGoalClass >= 0) {
        const uint16_t morphKey = (sQuickMorphGoalClass == 2) ? (1 << 7) /* DOWN */ : (1 << 6) /* UP */;
        if (Port_Samus_GetPoseClass() == sQuickMorphGoalClass || --sQuickMorphSafetyFrames <= 0) {
            sQuickMorphGoalClass = -1;
        } else if (sQuickMorphPhaseTimer > 0) {
            --sQuickMorphPhaseTimer;
            if (sQuickMorphPressing) gbaKeys |= morphKey;
        } else if (sQuickMorphPressing) {
            sQuickMorphPressing = false;
            sQuickMorphPhaseTimer = 4; /* release gap before the next edge */
        } else {
            sQuickMorphPressing = true;
            sQuickMorphPhaseTimer = 4;
            gbaKeys |= morphKey;
        }
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
