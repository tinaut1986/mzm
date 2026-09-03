#ifndef PORT_GBA_SCREEN_FX_H
#define PORT_GBA_SCREEN_FX_H

/*
 * Optional "looks like an AGB LCD" post-processing for the top screen.
 *
 * Runs as a self-contained pass over the finished top render target(s) in
 * PlatformGpu3DS_EndBottom -- the one choke point both present paths (the
 * PICA tile renderer and the CPU scanline fallback) go through -- so it is
 * renderer-independent and covers the stereo split. Three effects, each
 * OFF / LOW / MEDIUM / HIGH via the config levels in port_ppu_mzm.c:
 *
 *   - colour grade : mix the whole frame toward a dark green-grey tint
 *   - LCD grid     : dark scanlines, plus a vertical grille at HIGH
 *   - vignette     : darkened screen edges
 *
 * All three are baked into one screen-sized alpha mask (rebuilt only when a
 * level changes) and drawn as a single alpha-tested quad per eye. With
 * every level at 0 the pass returns before touching the GPU.
 */

#include <citro3d.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void PortGbaScreenFx_Init(void);
void PortGbaScreenFx_Shutdown(void);

/* True when at least one effect is enabled -- lets the caller skip the
 * scene re-bind entirely on the common all-off case. */
bool PortGbaScreenFx_Active(void);

/* Draw the enabled effects over `left` (and `right`, when stereo is on and
 * the caller passes it; NULL to skip). Call while the frame is still open,
 * before switching to the bottom scene. Leaves a standard alpha-blend /
 * passthrough-TEV state behind it. */
void PortGbaScreenFx_PostProcessTop(C3D_RenderTarget* left, C3D_RenderTarget* right);

/* Debug: CPU microseconds the most recent PostProcessTop call spent
 * submitting (0 if it did nothing this frame). The GPU-side fill cost is
 * not here -- it lands in the frame's gpuDraw time instead. */
unsigned PortGbaScreenFx_DebugLastSubmitUs(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_GBA_SCREEN_FX_H */
