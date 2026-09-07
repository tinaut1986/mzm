#pragma once

/* Per-sprite stereo depth override, keyed by OAM slot for the current
 * frame. See port_sprite_depth_oam.c for the why. */

/* For the PORT_TIER_* values a >= 0 code names -- so port_sprite_depth.inc
 * can use them by name (e.g. PORT_TIER_BG_PLAY) wherever this header is
 * included, including src/sprite.c's X-macro expansion. Header is clean
 * (stdint/stdbool only, no <3ds.h>). */
#include "port_stereo_depth.h"

enum {
    PORT_SPRITE_DEPTH_NONE = -1,        /* no override */
    PORT_SPRITE_DEPTH_BG_COPLANAR = -2, /* BG tier for this slot's OAM priority */
    /* >= 0: an explicit PORT_TIER_* value (see port_stereo_depth.h) */
};

void Port_SpriteDepth_BeginFrame(void);
void Port_SpriteDepth_NoteSlots(int firstSlot, int endSlot, int code);
int  Port_SpriteDepth_SlotCode(int oamIndex);
