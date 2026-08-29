/*
 * How many OAM slots the HUD occupies this frame.
 *
 * HudUpdateOam (src/hud.c) fills OAM from slot 0 and publishes the count
 * here; every other sprite in the frame is allocated after it. So "is this
 * sprite part of the HUD" is exactly "is its OAM index below this count" --
 * the game's own definition, covering the health bar, charge bar, missile /
 * super missile / power bomb counters and the minimap, and nothing else.
 *
 * It replaces a palette-bank-and-shape heuristic in the stereo renderer.
 * That heuristic happened to be right in the room recorded on 2026-08-28,
 * but it is a guess: it classifies by how a sprite LOOKS rather than by
 * what drew it, so a world sprite using the HUD palette can land on the
 * HUD depth plane, in front of everything, for no reason the player can
 * see. Kept in its own translation unit because src/hud.c is GBA-side code
 * and port_gpu_renderer.c cannot include its headers.
 */

static int sHudOamCount;

void Port_Hud_SetOamCount(int count) {
    if (count < 0) count = 0;
    if (count > 128) count = 128;
    sHudOamCount = count;
}

/* 0 when the HUD is hidden (gHideHud, or either half switched off by the
 * bottom screen's auto-hide) -- then no sprite is a HUD sprite, which is
 * the correct answer rather than a special case. */
int Port_Hud_GetOamCount(void) { return sHudOamCount; }
