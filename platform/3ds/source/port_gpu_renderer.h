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

#ifdef __cplusplus
}
#endif

#endif /* PORT_GPU_RENDERER_H */
