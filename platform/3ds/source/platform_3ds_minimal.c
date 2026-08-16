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

int Platform3DS_Init(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    APT_CheckNew3DS(&sIsNew3DS);
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
    sGameplayDisplayActive = true;
}

uint16_t Platform3DS_ReadKeyInput(void) {
    hidScanInput();
    return (uint16_t)hidKeysHeld();
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
    (void)value;
    return 0;
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
