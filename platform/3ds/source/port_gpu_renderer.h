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
/* The two halves of RenderFrame. CollectFrame is CPU-only and may run before
 * C3D_FrameBegin, overlapping the GPU's work on the previous frame, unless
 * CollectNeedsIdleGpu says it is about to reassign atlas slots that frame may
 * still be sampling. DrawFrame must run inside the frame. */
void Port_GpuRenderer_CollectFrame(void);
void Port_GpuRenderer_DrawFrame(void);
bool Port_GpuRenderer_CollectNeedsIdleGpu(void);
void Port_GpuRenderer_Shutdown(void);
/* Debug: flat-colour every drawn BG layer and sprite by its resolved stereo
 * tier (the same palette the layer workbench uses), so on a fast cutscene you
 * can see at a glance which depth plane each layer landed on. Alpha is kept,
 * so silhouettes stay. Only the main draw loop; the border HUD stays normal.
 * No cache reset, no cost when off. */
void Port_GpuRenderer_SetDepthTint(bool on);
bool Port_GpuRenderer_DepthTintEnabled(void);
bool Port_GpuRenderer_IsActive(void);
void Port_GpuRenderer_SetActive(bool active);
/* Drop every tile/layer cache and re-decode from VRAM over the next
 * couple dozen frames. Called after a save-state load (port_save_state.c)
 * replaces VRAM/palettes/tilemaps wholesale. */
void Port_GpuRenderer_InvalidateAll(void);
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
    uint8_t layerComposes;     /* layer maps with cells redrawn this frame */
    bool layerCacheOn;         /* layer maps enabled at all, so 0 can be told from off */
    uint8_t eyesRendered;      /* 1, or 2 while the 3D slider is up */
    uint8_t scissorPasses;     /* 1, or 2 while a GBA window splits the draw order */
    bool windowActive;
    bool hazeActive;
    uint8_t hazeMode;          /* 3 ripple target (RT), 0 its per-tile fallback (FULL) */
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
