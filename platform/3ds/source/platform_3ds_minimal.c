/*
 * Minimal Platform3DS implementation for the mzm 3DS port.
 *
 * zelda-tmc-3ds's own platform_3ds.c calls straight into its second-screen
 * and audio subsystems throughout its body (Port_SecondScreen_3DS_*,
 * Port_Audio_3DS*), not just in its includes -- neither exists yet for mzm
 * (see docs/3ds-port-skeleton-import.md). Rather than hollow out a file
 * that isn't ours line by line, this is a clean-room implementation of the
 * same platform_3ds.h contract: service init, New3DS detection, input,
 * timing, and a console-text-based fatal/splash screen (no citro2d/GPU
 * rendering yet -- that's port_ppu_3ds.c's job, still to be adapted).
 *
 * Every function declared in platform_3ds.h is implemented so the header's
 * contract stays valid for code that isn't compiled into this build yet;
 * the ones nothing currently calls are intentionally simple stubs, called
 * out below.
 */
#include "platform_3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

static bool sIsNew3DS;
static bool sRunning;
static bool sGameplayDisplayActive;

/* Body lives in port/port_gba_timing.c (plain C, no <3ds.h> -- see that
 * file's header comment for why). Spawned here because this file is the
 * one place that safely includes the real <3ds.h> for threadCreate's exact
 * signature; the port/ side only needs a single-argument svcSleepThread
 * forward declaration, much lower ABI risk than getting threadCreate's
 * five-parameter signature wrong from a hand-written declaration. */
extern void Port_GbaTiming_ThreadMain(void* arg);

int Platform3DS_Init(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);

    APT_CheckNew3DS(&sIsNew3DS);
    if (sIsNew3DS) {
        osSetSpeedupEnable(true);
        APT_SetAppCpuTimeLimit(80);
    }


    /* Without this, every busy-wait on REG_VCOUNT (e.g.
     * src/audio_wrappers.c:205-206) spins forever -- see
     * port_gba_timing.c's header comment. Must start before agbmain() runs
     * (main_3ds.c calls it right after this).
     *
     * Priority: needs to be above the main thread's default 0x30 (lower
     * number = higher priority in Horizon OS) to actually get scheduled
     * roughly every 73us -- but NOT above libctru's own internal GSP event
     * thread (gspEventThreadMain, created by gspInit() at priority 0x1A,
     * confirmed by disassembling libctru.a's gspgpu.o). That thread is what
     * processes the VBlank interrupt relay queue and signals the event
     * gspWaitForEvent() blocks on in port_bios.c's Port_Bios_Halt(). On a
     * single application core, this thread waking every ~73us at a HIGHER
     * priority (0x18 < 0x1A) than the GSP thread starves it indefinitely --
     * this was the exact cause of the port hanging forever at "before
     * gspWaitForEvent" on real hardware (confirmed via
     * sdmc:/3ds/mzm-debug.log bisection). 0x20 sits strictly between the
     * two: still preempts the main thread (0x30), but yields to GSP's
     * thread (0x1A) whenever it's ready to run. */
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
    return sIsNew3DS;
}

unsigned Platform3DS_Core1TimeLimit(void) {
    /* APT_SetAppCpuTimeLimit's percentage of core 1 available to the app on
     * New3DS; TMC's port used 80 as a conservative default leaving headroom
     * for system processes. Not yet wired to APT_SetAppCpuTimeLimit itself
     * -- nothing on this build spawns a second-core worker yet. */
    return sIsNew3DS ? 80u : 0u;
}

void Platform3DS_ShowSplash(void) {
    printf("\x1b[2J"); /* clear */
    printf("Metroid Zero Mission 3DS\n\n");
}

void Platform3DS_EnterGameplayDisplay(void) {
    printf("\x1b[2J"); /* clear bottom console */
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

void Platform3DS_PollKeysIntoGba(void) {
    const uint16_t held = Platform3DS_ReadKeyInput() & MZM_KEY_MASK;
    gba_write16(MZM_REG_KEY_INPUT, (uint16_t)(~held & MZM_KEY_MASK));
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
    return 0; /* Only meaningful for the PC_PORT host-pointer guard in
               * zelda-tmc-3ds's port_rom.h, which mzm's own port_rom.h
               * (this repo) doesn't use. */
}

int Platform3DS_IsActiveStackAddress(uintptr_t value) {
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
