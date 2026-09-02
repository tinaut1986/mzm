/*
 * Issue #29 / #28 bridge -- see port_haze_3ds.h for the full rationale.
 *
 * This file is "game-aware" (it pulls in include/structs/haze.h and the
 * gHazeValues EWRAM window) the same way port_ppu_mzm.c is, so like that
 * file it must NOT include <3ds.h> -- libctru's u32/s32 typedefs conflict
 * with this project's include/types.h.
 */

#include "port_haze_3ds.h"

#include <stdint.h>
#include <string.h>

#include "cpu/mode1.h"       /* virtuappu_mode1_pre_line_callback */
#include "gba/memory.h"      /* REG_BASE, EWRAM_BASE */
#include "macros.h"          /* CAST_TO_ARRAY (gHazeValues macro) */
#include "port_gba_mem.h"    /* gIoMem, gba_MemPtr (used by the gHazeValues macro) */
#include "structs/haze.h"    /* gHazeInfo, gHazeValues */

/* Byte offset of REG_BG3HOFS within the IO file (REG_BASE + 0x1C). */
#define HAZE_BG3HOFS_OFF 0x1Cu

static unsigned PortHaze_IoOffset(void)
{
    return (unsigned)((uintptr_t)gHazeInfo.pAffected - (uintptr_t)REG_BASE);
}

/*
 * Faithful stand-in for the HBlank DMA armed in VBlankCodeInGameLoad():
 *   DMA_SET(0, gHazeValues, gHazeInfo.pAffected,
 *           ... DMA_ENABLE | DMA_START_HBLANK | DMA_REPEAT | DMA_DEST_RELOAD,
 *           gHazeInfo.size / 2)
 * i.e. on every scanline's HBlank the DMA controller copies `size` bytes
 * from the next slice of gHazeValues to pAffected (BG3HOFS, or BG2/BG1HOFS
 * for the unused multi-layer routines, or WIN1H for the power bomb).
 *
 * GBA timing: the DMA latched during line N's HBlank takes effect for line
 * N+1; line 0 uses whatever VBlankCodeInGameLoad already wrote. mode1.c
 * calls this before rendering each line, so apply row (line - 1).
 */
static void PortHaze_PreLine(int line)
{
    const struct Haze *h = &gHazeInfo;
    if (!h->active || h->pAffected == NULL || h->size == 0)
        return;

    unsigned rows = h->unk_4 / h->size;
    if (rows == 0 || (unsigned)line >= rows)
        return;

    unsigned row = ((unsigned)line == 0u) ? 0u : (unsigned)line - 1u;
    unsigned off = PortHaze_IoOffset();
    if (off + h->size > 0x400u)
        return; /* pAffected not in the IO file -- nothing sane to do */

    memcpy(gIoMem + off, &gHazeValues[row * (h->size / 2u)], h->size);
}

void PortHaze_SetCpuPerLine(bool active)
{
    virtuappu_mode1_pre_line_callback =
        (active && gHazeInfo.active) ? PortHaze_PreLine : NULL;
}

bool PortHaze_Bg3RowScroll(int16_t rowDelta[160], int16_t *bakeHofs)
{
    const struct Haze *h = &gHazeInfo;

    /* Single-layer BG3 ripple only: size 2 (one u16 per line) writing to
     * REG_BG3HOFS. Everything else (size 8/12 multi-layer -- unused in the
     * shipped game -- the palette gradient, and the power-bomb WIN1H
     * resize) returns false and the GPU path renders it as it does now. */
    if (!h->active || h->size != 2 || PortHaze_IoOffset() != HAZE_BG3HOFS_OFF)
        return false;

    unsigned rows = h->unk_4 / h->size;
    if (rows < 160u)
        return false;

    /* The reference to bake the offscreen BG3 at is taken from the haze
     * table itself, not from gBackgroundPositions.bg[3].x, for two reasons:
     *
     *  - Wrap. Haze_Bg3 writes `sHaze_Bg3_StrongEffect[i] + bg[3].x` into a
     *    u16. Where bg[3].x is small (the "bad" room in issue #29 has BG3
     *    decoupled at HOFS 0) the negative half of the +/-2..4 px wobble
     *    wraps to 0xFFFE..0xFFFF. Masking each side to 9 bits separately and
     *    subtracting then yields ~+511 instead of -2, which slams into the
     *    +/-HAZE_MARGIN clamp: the submerged band alternates between a 1-2 px
     *    wobble and a hard 8 px jump every few scanlines -- the "moving
     *    squares". Rooms with a large bg[3].x (the "good" room, HOFS 480)
     *    never wrap, which is exactly why only some rooms broke.
     *  - Frame lag. HazeProcess fills gPreviousHazeValues and VBlank copies
     *    it into gHazeValues, so this frame's table was computed from the
     *    PREVIOUS frame's bg[3].x. Differencing against the current bg[3].x
     *    leaves a stray DC offset every frame the camera scrolls.
     *
     * gHazeValues[0] is bg[3].x plus at most one LUT entry, so differencing
     * against it is wrap-free (u16 arithmetic then sign-extended), lag-free,
     * and bounded by twice the LUT amplitude (<= 8 px = HAZE_MARGIN). */
    uint16_t base = (uint16_t)gHazeValues[0];

    for (int y = 0; y < 160; ++y)
        rowDelta[y] = (int16_t)((uint16_t)gHazeValues[y] - base);

    *bakeHofs = (int16_t)(base & 0x1FFu);
    return true;
}
