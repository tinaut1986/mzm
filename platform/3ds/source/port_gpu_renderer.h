#ifndef PORT_GPU_RENDERER_H
#define PORT_GPU_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool Port_GpuRenderer_Init(void);
/* True if the current GBA PPU state (DISPCNT/BLDCNT/MOSAIC/OAM) is within
 * this renderer's supported subset -- see the function body in
 * port_gpu_renderer.c for the exact list. Callers must check this every
 * frame (state changes frame to frame, e.g. Haze fades toggle BLDCNT) and
 * fall back to the CPU renderer when false. */
bool Port_GpuRenderer_CanRenderFrame(void);
void Port_GpuRenderer_RenderFrame(void);
void Port_GpuRenderer_Shutdown(void);
/* Step A: one quad per tilemap-aligned 16x16 block instead of four. Off
 * falls back to the per-tile loop, for measuring the change on hardware. */
void Port_GpuRenderer_SetBlockPass(bool on);
bool Port_GpuRenderer_BlockPassEnabled(void);
/* Step B: compose each eligible scrolling BG layer into its own render
 * target once per frame and draw it as ONE quad per eye. See the definition
 * -- off by default, because whether it pays depends on the room. */
void Port_GpuRenderer_SetLayerCache(bool on);
bool Port_GpuRenderer_LayerCacheEnabled(void);
bool Port_GpuRenderer_IsActive(void);
void Port_GpuRenderer_SetActive(bool active);
/* Item counts from the most recently rendered GPU frame, for the debug
 * overlay -- see the definition in port_gpu_renderer.c. Any output pointer
 * may be NULL. */
void Port_GpuRenderer_GetLastFrameStats(int* outItems, int* outObjItems, int* outCacheSlots);
/* Why the last GPU frame cost what it cost, recorded per sample by both
 * on-device recorders (issue #20). Frame times say a frame missed the
 * 16.67ms budget; these say what it spent it on. */
typedef struct {
    uint32_t drawCount;        /* C2D_DrawImage calls, summed over every eye drawn */
    uint32_t blendTransitions; /* opaque<->alpha switches; each breaks the batch */
    uint32_t hazeTiles;        /* BG3 tiles in the offscreen haze pass, 0 if inactive */
    uint32_t bgItems;          /* collected items (ONE eye): BG tile quads */
    uint32_t objItems;         /* collected items (ONE eye): OBJ subtile quads */
    /* CPU-side cost of this renderer, hundredths of a ms. The GPU counters
     * (C3D_GetDrawingTime/GetProcessingTime) do not cover any of this, and
     * it is the half that scales with the CPU clock -- i.e. the half that
     * tells you what an Old3DS would do with the same scene. */
    uint32_t cpuTileX100;      /* VRAM reads + tile cache hash/decode + sort */
    uint32_t cpuUploadX100;    /* atlas texture upload (blocking transfer) */
    uint32_t cpuDrawX100;      /* draw-call submission, every eye */
    /* Device pixels the frame's quads covered, summed over every eye. The
     * quad count alone cannot separate "cost is per-quad" from "cost is per
     * pixel", and after step A the two disagree -- see the definition. */
    uint32_t drawnPixels;
    uint8_t layerComposes;     /* step B: layers re-composed this frame; 0 = all reused */
    bool layerCacheOn;         /* step B enabled at all, so 0 composes can be told from off */
    uint8_t eyesRendered;      /* 1, or 2 while the 3D slider is up */
    uint8_t scissorPasses;     /* 1, or 2 while a GBA window splits the draw order */
    bool windowActive;
    bool hazeActive;
} PortGpuRendererDrawStats;
void Port_GpuRenderer_GetLastFrameDrawStats(PortGpuRendererDrawStats* out);
/* CPU time (ms) spent in the most recent Port_GpuRenderer_RenderFrame call:
 * collectMs = VRAM reads + tile cache lookup/decode + sort, drawMs = atlas
 * upload + draw-call submission for both eyes. Neither PORT_PPU_PERF_LOG's
 * mode1 stats (stale when the GPU renderer is active) nor citro3d's own
 * gpuDraw/gpuProc counters (GPU-side only) cover this. */
void Port_GpuRenderer_GetLastFrameTimingMs(float* outCollectMs, float* outDrawMs);
/* Issue #17 diagnosis: dumps the atlas texture (PPM) and populated-slot
 * cache keys (CSV) exactly as they are right now -- see the definition in
 * port_gpu_renderer.c. */
void Port_GpuRenderer_DumpAtlas(const char* ppmPath, const char* csvPath);

#ifdef __cplusplus
}
#endif

#endif /* PORT_GPU_RENDERER_H */
