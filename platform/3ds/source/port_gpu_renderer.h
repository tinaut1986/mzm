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
bool Port_GpuRenderer_IsActive(void);
void Port_GpuRenderer_SetActive(bool active);
/* Item counts from the most recently rendered GPU frame, for the debug
 * overlay -- see the definition in port_gpu_renderer.c. Any output pointer
 * may be NULL. */
void Port_GpuRenderer_GetLastFrameStats(int* outItems, int* outObjItems, int* outCacheSlots);
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
