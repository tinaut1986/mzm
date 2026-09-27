#include "port_wide_view.h"

#include <stdint.h>

extern int Port_Config_Get3DSAspectRatio(void);
extern int Port_Config_Get3DSDisplayStyle(void);

/* Kept as a hand-written extern, like the renderer does: this file stays out
 * of the game headers. GM_INGAME == 4 (include/constants/game_state.h). */
extern short gMainGameMode;

enum { ASPECT_WIDE = 3, STYLE_PIXEL_PERFECT = 0 };
enum { GM_INGAME = 4 };

/* SCALED draws at 1.5x, so the 400px screen holds 400 / 1.5 = 266.7 GBA px:
 * 13.3 more on each side, rounded up. PIXEL PERFECT holds 400x240 outright. */
enum {
    MARGIN_X_SCALED = 14,
    MARGIN_Y_SCALED = 0,
    MARGIN_X_PIXEL_PERFECT = 80,
    MARGIN_Y_PIXEL_PERFECT = 40,
    SUB_PIXELS_PER_PIXEL = 4, /* SUB_PIXEL_RATIO in include/types.h */
};

static bool sFrameDrawn;

bool PortWide_Selected(void) {
    return Port_Config_Get3DSAspectRatio() == ASPECT_WIDE;
}

bool PortWide_GameActive(void) {
    return PortWide_Selected() && gMainGameMode == GM_INGAME;
}

int PortWide_MarginX(void) {
    if (!PortWide_GameActive()) return 0;
    return Port_Config_Get3DSDisplayStyle() == STYLE_PIXEL_PERFECT ? MARGIN_X_PIXEL_PERFECT
                                                                   : MARGIN_X_SCALED;
}

int PortWide_MarginY(void) {
    if (!PortWide_GameActive()) return 0;
    return Port_Config_Get3DSDisplayStyle() == STYLE_PIXEL_PERFECT ? MARGIN_Y_PIXEL_PERFECT
                                                                   : MARGIN_Y_SCALED;
}

void PortWide_SetFrameDrawn(bool drawn) { sFrameDrawn = drawn; }
bool PortWide_FrameDrawn(void) { return sFrameDrawn; }

/* Gated on the last frame really having been widened, not just on the setting:
 * the renderer leaves some frames plain (haze, windows, the CPU fallback), and
 * a sprite kept alive out in a margin nobody draws would land in the wrong
 * place -- OAM Y/X wrap, so a far-out sprite reads as one on the other side. */
/* Twice the margin: the view slides inside the room, so on the side it slides
 * toward it can reach the margin plus the slide, up to twice the margin. */
int Port_WideMarginSubPixelX(void) { return sFrameDrawn ? 2 * PortWide_MarginX() * SUB_PIXELS_PER_PIXEL : 0; }
int Port_WideMarginSubPixelY(void) { return sFrameDrawn ? 2 * PortWide_MarginY() * SUB_PIXELS_PER_PIXEL : 0; }

/* ---- OAM position tags ---------------------------------------------------
 * The game's sprite drawers tag each OAM span they emit with the sprite's true
 * screen position (see the callers in src/). Cleared once per frame by
 * SpriteDrawAll_HighPriority, like the other per-slot tags. */
enum { OAM_SLOTS = 128 };
static int16_t sOriginY[OAM_SLOTS], sOriginX[OAM_SLOTS];
static bool sTagged[OAM_SLOTS];

void Port_Wide_BeginFrame(void) {
    for (int i = 0; i < OAM_SLOTS; ++i) sTagged[i] = false;
}

void Port_Wide_NoteSlots(int firstSlot, int endSlot, int originY, int originX) {
    if (firstSlot < 0) firstSlot = 0;
    if (endSlot > OAM_SLOTS) endSlot = OAM_SLOTS;
    for (int s = firstSlot; s < endSlot; ++s) {
        sOriginY[s] = (int16_t)originY;
        sOriginX[s] = (int16_t)originX;
        sTagged[s] = true;
    }
}

/* The renderer draws a merged OAM (port/port_bios.c, Port_Bios_OamMergeTick):
 * each slot is either this frame's entry or one kept from a skipped frame.
 * These are the tags that go with the merged OAM, slot for slot. */
static int16_t sMergedY[OAM_SLOTS], sMergedX[OAM_SLOTS];
static bool sMergedTagged[OAM_SLOTS];

void Port_Wide_MergeTags(const bool* tookLive) {
    for (int s = 0; s < OAM_SLOTS; ++s) {
        if (!tookLive[s]) continue;
        sMergedY[s] = sOriginY[s];
        sMergedX[s] = sOriginX[s];
        sMergedTagged[s] = sTagged[s];
    }
}

bool PortWide_SlotOrigin(int oamIndex, int* outY, int* outX) {
    if (oamIndex < 0 || oamIndex >= OAM_SLOTS || !sMergedTagged[oamIndex]) return false;
    *outY = sMergedY[oamIndex];
    *outX = sMergedX[oamIndex];
    return true;
}
