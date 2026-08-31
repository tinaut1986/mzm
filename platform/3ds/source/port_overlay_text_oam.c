/*
 * Which OAM slots hold "overlay text" this frame -- the in-game message
 * banner (item pickups, Chozo hints, save/map prompts), the area-name
 * banner ("SAVE ROOM", area titles on room entry) and the save YES/NO
 * cursor that rides with the message banner.
 *
 * These are ordinary sprites, spawned long after the HUD's OAM slots, so
 * the stereo renderer's HUD test (oamIndex < Port_Hud_GetOamCount) never
 * catches them and they land on the world OBJ depth plane -- coplanar with
 * Samus. On GBA that is invisible; in stereo the flat text sinks into her.
 *
 * SpriteDraw (src/sprite.c) knows the sprite's identity and the exact OAM
 * slot span it just emitted, so it tags the span here. The renderer then
 * lifts those slots to the front (HUD) depth tier, matching the OAM
 * priority 0 they already draw with in 2D.
 *
 * Identity, not appearance: same rationale as port_hud_oam.c. Kept in its
 * own translation unit because src/sprite.c is GBA-side code and
 * port_gpu_renderer.c cannot include its headers.
 *
 * Lifetime is one frame: SpriteDrawAll_HighPriority clears the mask at the
 * top of every gameplay frame, before any banner re-emits its OAM, and the
 * renderer reads it later that same frame.
 */

#include <stdint.h>

static uint32_t sMask[4]; /* 128 bits, one per OAM slot */

void Port_OverlayText_BeginFrame(void) {
    sMask[0] = sMask[1] = sMask[2] = sMask[3] = 0u;
}

void Port_OverlayText_NoteBannerOam(int firstSlot, int endSlot) {
    if (firstSlot < 0) firstSlot = 0;
    if (endSlot > 128) endSlot = 128;
    for (int s = firstSlot; s < endSlot; ++s)
        sMask[s >> 5] |= (uint32_t)1u << (s & 31);
}

int Port_OverlayText_IsSlot(int oamIndex) {
    if (oamIndex < 0 || oamIndex >= 128) return 0;
    return (sMask[oamIndex >> 5] >> (oamIndex & 31)) & 1u;
}
