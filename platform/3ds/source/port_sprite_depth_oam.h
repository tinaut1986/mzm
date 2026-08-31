#pragma once

/* Per-sprite stereo depth override, keyed by OAM slot for the current
 * frame. See port_sprite_depth_oam.c for the why. */

enum {
    PORT_SPRITE_DEPTH_NONE = -1,        /* no override */
    PORT_SPRITE_DEPTH_BG_COPLANAR = -2, /* BG tier for this slot's OAM priority */
    /* >= 0: an explicit PORT_TIER_* value */
};

void Port_SpriteDepth_BeginFrame(void);
void Port_SpriteDepth_NoteSlots(int firstSlot, int endSlot, int code);
int  Port_SpriteDepth_SlotCode(int oamIndex);
