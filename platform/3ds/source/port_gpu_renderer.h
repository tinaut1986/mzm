#ifndef PORT_GPU_RENDERER_H
#define PORT_GPU_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool Port_GpuRenderer_Init(void);
void Port_GpuRenderer_RenderFrame(void);
void Port_GpuRenderer_Shutdown(void);
bool Port_GpuRenderer_IsActive(void);
void Port_GpuRenderer_SetActive(bool active);

#ifdef __cplusplus
}
#endif

#endif /* PORT_GPU_RENDERER_H */
