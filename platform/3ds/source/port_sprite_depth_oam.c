/*
 * Per-sprite stereo depth overrides, keyed by OAM slot for the current
 * frame.
 *
 * The stereo renderer puts every world sprite on ONE plane (OBJ_P1, -0.8px)
 * regardless of the sprite's own OAM priority. That is the right default --
 * splitting sprites across four planes by priority was tried and made more
 * things read wrong than right (see port_stereo_depth.c). But a few sprite
 * TYPES are authored to composite with a specific BG and look broken on the
 * one plane:
 *
 *   - The Kraid / Ridley boss-room statues (src/sprites_ai/boss_statues.c)
 *     set bgPriority to BG1's priority so the sprite face blends with the BG
 *     that carries the top of the statue's head. Forced to -0.8 the face
 *     detaches from the head, which sits at the BG plane.
 *
 * SpriteDraw knows the sprite's identity and the OAM slot span it just
 * emitted, so it tags that span here with what the renderer should do
 * instead. Same lifetime and clearing as port_overlay_text_oam.c:
 * SpriteDrawAll_HighPriority resets the table every gameplay frame.
 *
 * Codes:
 *   PORT_SPRITE_DEPTH_NONE (-1)  no override (the vast majority of slots)
 *   PORT_SPRITE_DEPTH_BG_COPLANAR (-2)  place at the BG tier for this
 *       slot's OAM priority (PortStereoDepth_BgTierForPriority) -- i.e.
 *       coplanar with a BG of the same priority
 *   >= 0  an explicit PORT_TIER_* value
 *
 * Own translation unit for the same reason as port_hud_oam.c: src/sprite.c
 * is GBA-side code and port_gpu_renderer.c cannot include its headers.
 */

#include <stdint.h>

#include "port_sprite_depth_oam.h"

static int8_t sCode[128];

void Port_SpriteDepth_BeginFrame(void) {
    for (int i = 0; i < 128; ++i) sCode[i] = PORT_SPRITE_DEPTH_NONE;
}

void Port_SpriteDepth_NoteSlots(int firstSlot, int endSlot, int code) {
    if (firstSlot < 0) firstSlot = 0;
    if (endSlot > 128) endSlot = 128;
    for (int s = firstSlot; s < endSlot; ++s) sCode[s] = (int8_t)code;
}

int Port_SpriteDepth_SlotCode(int oamIndex) {
    if (oamIndex < 0 || oamIndex >= 128) return PORT_SPRITE_DEPTH_NONE;
    return sCode[oamIndex];
}
