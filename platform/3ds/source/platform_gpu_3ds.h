#ifndef PLATFORM_GPU_3DS_H
#define PLATFORM_GPU_3DS_H

#include <stdbool.h>
#include <stdint.h>

/* Forward-declared instead of pulling in <citro3d.h> here -- only the tile
 * GPU renderer needs the real type, and it already includes citro3d.h
 * itself. Keeps this header includable from files that don't want the full
 * 3ds/citro3d dependency. */
struct C3D_RenderTarget_tag;

typedef struct PlatformGpu3DSStats {
    uint64_t frames;
    uint64_t frameBeginFailures;
    uint64_t topTransfers;
    uint64_t bottomTransfers;
    uint64_t bottomTargetDraws;
    uint64_t bottomTargetReuseSkips;
    uint64_t boundedFlushBytes;
    uint32_t linearHeapBytes;
    uint32_t c2dFlushBytes;
    uintptr_t c2dFlushAddress;
    uintptr_t topUploadAddress;
    uintptr_t bottomUploadAddress[2];
    float drawingTime;
    float processingTime;
} PlatformGpu3DSStats;

bool PlatformGpu3DS_Init(bool old3dsProfile);
float PlatformGpu3DS_Get3DSlider(void);
uint32_t* PlatformGpu3DS_TopBuffer(void);
uint32_t* PlatformGpu3DS_TopRightBuffer(void);
uint32_t* PlatformGpu3DS_BottomBuffer(unsigned index);
void PlatformGpu3DS_BeginTop(const uint32_t* pixels, unsigned width);
/* Must bracket every actual GPU submission call (BeginTop.../EndBottom) made
 * from more than one thread -- see the doc comment on sGpuSubmitLock in
 * platform_gpu_3ds.c for why. */
void PlatformGpu3DS_SubmitLock_Acquire(void);
void PlatformGpu3DS_SubmitLock_Release(void);

void PlatformGpu3DS_BeginTopStereo(const uint32_t* leftPixels, const uint32_t* rightPixels, unsigned width);
/* Returns true only when a Citro3D frame was active and submitted. */
bool PlatformGpu3DS_EndBottom(const uint32_t* pixels, bool changed);

/* GPU tile-renderer integration (port_gpu_renderer.c): opens the frame like
 * PlatformGpu3DS_BeginTopStereo does (C3D_FrameBegin) but does NOT upload or
 * draw a CPU-composited pixel buffer -- the caller draws its own scene
 * (tile/sprite quads) directly into the shared top-screen render targets
 * below, then still finishes the frame with PlatformGpu3DS_EndBottom as
 * usual. Deliberately reuses this module's existing targets/citro3d/citro2d
 * global state instead of the tile renderer creating its own -- two
 * independent C3D_RenderTargetCreate/gfxSet3D sets both pointed at GFX_TOP
 * would double VRAM usage and race over which one actually reaches the
 * screen. */
bool PlatformGpu3DS_BeginTopSceneGpu(void);
struct C3D_RenderTarget_tag* PlatformGpu3DS_GetTopLeftTarget(void);
struct C3D_RenderTarget_tag* PlatformGpu3DS_GetTopRightTarget(void);

/* Sets up the 3-stage TexEnv pipeline that any texture built with this
 * project's "R,G,B,A in memory order" packing (see mode1.c's ABGR8888
 * comment and Bgr555ToRgba8 in port_gpu_renderer.c) needs in order to
 * display correctly. Not cosmetic: the PICA200 samples a GPU_RGBA8 texture
 * with its components in the REVERSE of that memory byte order (component R
 * == memory byte 3, G == byte 2, B == byte 1, A == byte 0), so a naive
 * single-stage passthrough (GPU_REPLACE) shows the wrong channels swapped
 * into the wrong slots -- correct pixel shapes, badly wrong/washed-out
 * colors. This reconstructs true RGB from the swapped components. Any code
 * drawing a texture built with that packing convention (the CPU-composited
 * top buffer in this file, and the GPU tile atlas in port_gpu_renderer.c)
 * must call this before its draws -- TexEnv state is global GPU state, not
 * per-texture, so it doesn't happen automatically. */
void PlatformGpu3DS_ConfigureAbgrTextureEnv(void);
void PlatformGpu3DS_ResetSolidTexEnv(void);
void PlatformGpu3DS_ShowDumpSavedOverlay(void);
void PlatformGpu3DS_DumpScreens(void);
void PlatformGpu3DS_GetStats(PlatformGpu3DSStats* stats);
void PlatformGpu3DS_InvalidateBottomTarget(void);
void PlatformGpu3DS_Shutdown(void);

#endif
