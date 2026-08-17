#include "platform_linux.h"
#include "port_rom.h"
#include "gba.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

static Display* sDisplay = NULL;
static Window sWindow = 0;
static GC sGC = 0;
static XImage* sXImage = NULL;
static u32 sFb32[240 * 160];
#endif

static bool sHeadless = false;
static bool sQuit = false;
static u16 sCurrentKeys = 0;
static struct timespec sLastFrameTime;
static u64 sFrameCount = 0;

extern void CallbackCallVblank(void);

void Platform_Linux_Init(bool headless) {
    sHeadless = headless;
    clock_gettime(CLOCK_MONOTONIC, &sLastFrameTime);

#ifdef HAVE_X11
    if (!sHeadless) {
        sDisplay = XOpenDisplay(NULL);
        if (sDisplay) {
            int screen = DefaultScreen(sDisplay);
            sWindow = XCreateSimpleWindow(sDisplay, RootWindow(sDisplay, screen),
                                          100, 100, 720, 480, 1,
                                          BlackPixel(sDisplay, screen),
                                          BlackPixel(sDisplay, screen));
            XSelectInput(sDisplay, sWindow, ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask);
            XStoreName(sDisplay, sWindow, "Metroid: Zero Mission (Linux Native)");
            XMapWindow(sDisplay, sWindow);
            sGC = DefaultGC(sDisplay, screen);

            Visual* visual = DefaultVisual(sDisplay, screen);
            int depth = DefaultDepth(sDisplay, screen);
            sXImage = XCreateImage(sDisplay, visual, depth, ZPixmap, 0,
                                   (char*)malloc(720 * 480 * 4), 720, 480, 32, 0);
            printf("[Linux] X11 Window created successfully (720x480)\n");
        } else {
            printf("[Linux] Warning: Could not open X11 Display. Running in headless mode.\n");
            sHeadless = true;
        }
    }
#endif
}

void Platform_Linux_Cleanup(void) {
#ifdef HAVE_X11
    if (sXImage) {
        if (sXImage->data) free(sXImage->data);
        sXImage->data = NULL;
        XDestroyImage(sXImage);
        sXImage = NULL;
    }
    if (sDisplay) {
        if (sWindow) XDestroyWindow(sDisplay, sWindow);
        XCloseDisplay(sDisplay);
        sDisplay = NULL;
    }
#endif
}

/* GBA PPU emulation, minimal: game code busy-waits on REG_VCOUNT/REG_DISPSTAT
 * all over the place (e.g. src/audio_wrappers.c SetupSoundTransfer, called
 * from InitializeAudio() before the main loop even starts). Without
 * something advancing them, those waits spin forever. Mirrors the 3DS port's
 * port_gba_timing.c (which uses a 3DS-native thread); this is the same idea
 * via pthreads since this target doesn't link against libctru. */
#define GBA_SCANLINES_PER_FRAME 228
#define GBA_VBLANK_START_LINE 160
#define SCANLINE_NS 73350L /* ~73.35us/scanline, 228 lines/frame @ ~59.7Hz */

static pthread_t sTimingThread;
static volatile bool sTimingThreadRunning = false;

static void* Platform_Linux_TimingThreadMain(void* arg) {
    (void)arg;
    u8 line = 0;
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = SCANLINE_NS;

    while (sTimingThreadRunning) {
        nanosleep(&ts, NULL);

        line++;
        if (line >= GBA_SCANLINES_PER_FRAME) {
            line = 0;
        }
        gba_write8(REG_VCOUNT, line);

        u16 dispstat = gba_read16(REG_DISPSTAT);
        if (line >= GBA_VBLANK_START_LINE) {
            dispstat |= DSTAT_IN_VBLANK;
        } else {
            dispstat &= ~DSTAT_IN_VBLANK;
        }
        gba_write16(REG_DISPSTAT, dispstat);
    }
    return NULL;
}

void Platform_Linux_StartTimingThread(void) {
    sTimingThreadRunning = true;
    pthread_create(&sTimingThread, NULL, Platform_Linux_TimingThreadMain, NULL);
}

void Platform_Linux_StopTimingThread(void) {
    sTimingThreadRunning = false;
    pthread_join(sTimingThread, NULL);
}

u16 Platform_Linux_PollKeys(void) {
#ifdef HAVE_X11
    if (sDisplay && !sHeadless) {
        while (XPending(sDisplay)) {
            XEvent ev;
            XNextEvent(sDisplay, &ev);
            if (ev.type == KeyPress || ev.type == KeyRelease) {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                bool pressed = (ev.type == KeyPress);
                u16 key = 0;
                switch (sym) {
                    case XK_z: case XK_Z: key = KEY_A; break;
                    case XK_x: case XK_X: key = KEY_B; break;
                    case XK_a: case XK_A: key = KEY_L; break;
                    case XK_s: case XK_S: key = KEY_R; break;
                    case XK_Return:       key = KEY_START; break;
                    case XK_space:        key = KEY_SELECT; break;
                    case XK_Left:         key = KEY_LEFT; break;
                    case XK_Right:        key = KEY_RIGHT; break;
                    case XK_Up:           key = KEY_UP; break;
                    case XK_Down:         key = KEY_DOWN; break;
                    case XK_Escape:       sQuit = true; break;
                    default: break;
                }
                if (pressed) sCurrentKeys |= key;
                else sCurrentKeys &= ~key;
            } else if (ev.type == DestroyNotify) {
                sQuit = true;
            }
        }
    }
#endif
    /* Update GBA REG_KEY_INPUT in IO memory (active low) */
    u16 reg_val = (~sCurrentKeys) & 0x03FF;
    gba_write16(REG_KEY_INPUT, reg_val);
    return sCurrentKeys;
}

bool Platform_Linux_ShouldQuit(void) {
    return sQuit;
}

extern u8 gMainGameMode;
extern u8 gSubGameMode1;
extern bool sTestMode;
extern int sMaxFrames;

void Platform_Linux_VBlank(void) {
    sFrameCount++;

    /* Poll input */
    Platform_Linux_PollKeys();

    if (sFrameCount % 60 == 0 || sFrameCount == 1) {
        printf("[Linux] Frame %5lu | MainGameMode: 0x%02X | SubMode1: 0x%02X\n",
               (unsigned long)sFrameCount, (unsigned int)gMainGameMode, (unsigned int)gSubGameMode1);
        fflush(stdout);
    }

    if (sTestMode && sMaxFrames > 0 && sFrameCount >= (u64)sMaxFrames) {
        printf("[Linux Test] Reached %d frames successfully! Test PASSED without errors.\n", sMaxFrames);
        exit(0);
    }

    /* 60 FPS frame timing (16.666 ms per frame) */
    if (!sHeadless) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        u64 elapsed_ns = (now.tv_sec - sLastFrameTime.tv_sec) * 1000000000ULL +
                         (now.tv_nsec - sLastFrameTime.tv_nsec);
        const u64 frame_target_ns = 16666667ULL; // 1/60th second
        if (elapsed_ns < frame_target_ns) {
            struct timespec sleep_time;
            u64 diff = frame_target_ns - elapsed_ns;
            sleep_time.tv_sec = diff / 1000000000ULL;
            sleep_time.tv_nsec = diff % 1000000000ULL;
            nanosleep(&sleep_time, NULL);
        }
        clock_gettime(CLOCK_MONOTONIC, &sLastFrameTime);
    }
}

