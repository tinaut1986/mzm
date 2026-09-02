#ifndef PORT_HAZE_3DS_H
#define PORT_HAZE_3DS_H

/*
 * Issue #29: water / lava / acid / heat-haze backgrounds ripple on GBA
 * because the game rewrites REG_BG3HOFS (and, for the unused multi-layer
 * routines, BG2/BG1 HOFS) on every scanline via an HBlank DMA armed in
 * VBlankCodeInGameLoad(). This port emulates neither HBlank DMA nor
 * per-scanline IO, so the scroll register only ever gets its frame-level
 * value and the surface renders flat.
 *
 * This module bridges the game's per-line table (gHazeValues, filled every
 * frame by HazeProcess()) to both renderers:
 *   - CPU (mode1.c): PortHaze_SetCpuPerLine() installs a pre-line callback
 *     that replays the table into the emulated IO regs, a faithful stand-in
 *     for the missing HBlank DMA. Generic: covers every gHazeInfo.active
 *     case, including the power-bomb WIN1H resize (issue #28).
 *   - GPU (port_gpu_renderer.c): PortHaze_Bg3RowScroll() hands back the
 *     per-line BG3 shift so the GPU path can bake BG3 once and re-blit it
 *     in per-scanline strips (Option C).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install (active=true) or remove (active=false) the CPU scanline
 * renderer's pre-line callback. A no-op internally unless gHazeInfo.active,
 * so it is safe to call every frame with active = "the CPU path renders
 * this frame". */
void PortHaze_SetCpuPerLine(bool active);

/* True when the room's ripple effect is active AND it is the ordinary
 * single-layer BG3 horizontal case (every EFFECT_WATER / EFFECT_LAVA /
 * EFFECT_WEAK_ACID / EFFECT_STRONG_ACID / EFFECT_LAVA_HEAT_HAZE room in the
 * game). Fills *bakeHofs with the 9-bit BG3 X scroll the GPU path should
 * bake its offscreen BG3 at, and rowDelta[0..159] with the signed pixel
 * shift to apply to each visible scanline RELATIVE to that bake -- i.e. the
 * pure wobble, bounded by the LUT amplitude (<= 8 px). Both come from the
 * same gHazeValues snapshot, so the delta is free of the u16 wrap and the
 * one-frame lag that differencing against gBackgroundPositions.bg[3].x
 * introduced (see the comment in the .c). Returns false (and touches
 * nothing) for the multi-layer heat-haze routines, the BG3 gradient, and
 * the power-bomb window resize -- those keep rendering as before on the GPU
 * path. */
bool PortHaze_Bg3RowScroll(int16_t rowDelta[160], int16_t *bakeHofs);

#ifdef __cplusplus
}
#endif

#endif /* PORT_HAZE_3DS_H */
