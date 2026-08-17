/*
 * Minimal PPU->GPU bridge for the mzm 3DS port. Not an adaptation of
 * port_ppu_3ds.c (that file is zelda-tmc-3ds's original, still deeply
 * coupled to TMC-only subsystems -- second screen, HDMA, widescreen, save
 * states, perf HUD -- see its own header comment and
 * docs/3ds-port-status-2026-08-17.md section 8). This is a first cut: bind
 * the port/ppu software renderer (port/ppu/src/{virtuappu,mode1}.c,
 * shared first-party source, not TMC-specific) directly to mzm's emulated
 * GBA memory (port_gba_mem.h's gIoMem/gVram/gBgPltt/gObjPltt/gOamMem) and
 * push the resulting frame to the top screen via platform_gpu_3ds.c (also
 * reused as-is -- it only touches citro2d/citro3d and raw pixel buffers,
 * no TMC coupling). No second-screen content, no HDMA, no widescreen: just
 * enough to get real gameplay visible on real hardware for the first time.
 */
#include "virtuappu.h"
#include "cpu/mode1.h"
#include "port_gba_mem.h"
#include "platform_gpu_3ds.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Must match platform_gpu_3ds.c's TOP_TEXTURE_WIDTH -- the buffer
 * PlatformGpu3DS_TopBuffer() hands back is allocated at that pitch. */
#define TOP_PITCH 512
#define TOP_NATIVE_W 240

static uint32_t* sTopBuffer;
static uint32_t* sBottomBuffer;
static bool sReady;

/* platform_gpu_3ds.c calls these (FPS HUD / aspect ratio / display style
 * config, normally backed by a settings menu that doesn't exist yet).
 * Fixed defaults: no FPS overlay, wide aspect, pixel-perfect scaling. */
extern bool Platform3DS_IsNew3DS(void);

bool Port_Config_GetShowFps(void) { return false; }
int Port_Config_Get3DSAspectRatio(void) { return 0; /* TOP_ASPECT_WIDE */ }
int Port_Config_Get3DSDisplayStyle(void) { return 0; /* TOP_DISPLAY_PIXEL_PERFECT */ }
double Port_PPU_3DS_CurrentFps(void) { return 0.0; }

bool Port_PPU_Init(void) {
    VirtuaPPUMode1GbaMemory memory = { gIoMem, gVram, gBgPltt, gObjPltt, gOamMem };
    virtuappu_mode1_bind_gba_memory(&memory);
    virtuappu_mode1_set_old3ds_profile(!Platform3DS_IsNew3DS());

    if (!PlatformGpu3DS_Init(!Platform3DS_IsNew3DS())) return false;
    sTopBuffer = PlatformGpu3DS_TopBuffer();
    sBottomBuffer = PlatformGpu3DS_BottomBuffer(0);
    if (!sTopBuffer || !sBottomBuffer) return false;
    /* Bottom screen stays solid black -- PlatformGpu3DS_EndBottom() still
     * needs a non-NULL buffer to finish the frame (C2D_Flush/C3D_FrameEnd),
     * it just skips re-uploading it every frame since changed=false. */
    memset(sBottomBuffer, 0, 512u * 256u * sizeof(uint32_t));

    virtuappu_mode1_set_output_buffer(sTopBuffer, TOP_PITCH);
    sReady = true;
    return true;
}

extern void Port_DebugLog(const char* msg);
static unsigned sPresentFrameCount;

void Port_PPU_PresentFrame(void) {
    if (!sReady) return;

    const uint16_t dispcnt = (uint16_t)(gIoMem[0] | (gIoMem[1] << 8));
    const uint8_t gbaMode = (uint8_t)(dispcnt & 7);

    PPUMemory ppu;
    ppu.frame_width = TOP_NATIVE_W;
    ppu.frame_pitch = TOP_PITCH;
    /* virtuappu internal mode: 1 = tiled (GBA mode 0), 2 = affine (GBA
     * modes 1/2) -- see port/ppu/src/virtuappu.c's virtuappu_render_frame.
     * Zero Mission only ever uses GBA modes 0 and 1 (docs/3ds-port-ppu-audit.md). */
    ppu.mode = (gbaMode == 1 || gbaMode == 2) ? 2 : 1;

    virtuappu_mode1_set_frame_geometry(&ppu);
    virtuappu_mode1_render_frame(&ppu);

    if (sPresentFrameCount < 5) {
        char msg[256];
        __builtin_snprintf(msg, sizeof(msg),
            "Port_PPU_PresentFrame[%u]: dispcnt=%04x mode=%u vram[0..3]=%02x%02x%02x%02x "
            "pal[0..1]=%04x,%04x out[0..3]=%08lx,%08lx,%08lx,%08lx",
            sPresentFrameCount, dispcnt, ppu.mode, gVram[0], gVram[1], gVram[2], gVram[3],
            gBgPltt[0], gBgPltt[1],
            (unsigned long)sTopBuffer[0], (unsigned long)sTopBuffer[1],
            (unsigned long)sTopBuffer[TOP_PITCH], (unsigned long)sTopBuffer[TOP_PITCH + 1]);
        Port_DebugLog(msg);
    }
    ++sPresentFrameCount;

    PlatformGpu3DS_BeginTop(sTopBuffer, TOP_NATIVE_W);
    PlatformGpu3DS_EndBottom(sBottomBuffer, false);
}
