/*
 * Pure stereo depth-tier decision logic for the 3DS GPU renderer's BG/OBJ
 * layers (see kTierEyeOffsetPx in port_gpu_renderer.c for what each tier
 * means). Deliberately has zero 3DS/citro3d/GBA-memory dependencies so it
 * can be linked into a plain host-gcc test binary
 * (platform/3ds/tests/depth_tier_test.c) the same way port/ppu's own
 * mode1_native_fast_path_test.c links port/ppu/src/mode1.c directly.
 */
#ifndef PORT_3DS_DEPTH_TIER_H
#define PORT_3DS_DEPTH_TIER_H

#ifdef __cplusplus
extern "C" {
#endif

/* bgIndex: 0-3 (GBA BG0..BG3). priority: raw BGCNT priority field (0=highest
 * .. 3=lowest). inCutscene: true when gMainGameMode == GM_CUTSCENE (BG0 is
 * NOT guaranteed to be the HUD/text layer during cutscenes -- e.g. the
 * Mother Brain close-up uses BG0 as the elevator-shaft foreground with Samus
 * on BG1 behind it, see GitHub issue #4). Returns a depthTier index into
 * kTierEyeOffsetPx (0..3 in this function's outputs; sprite tiers 4..6 are
 * decided separately by DepthTierForSprite). */
int DepthTierForBg(int bgIndex, int priority, int inCutscene);

/* palBank: OBJ palette bank (OAM attr2 bits 12-15). shape: OAM attr0 shape
 * field (0=square,1=wide,2=tall). priority: raw OAM attr2 priority field.
 * inMapOrPauseScreen: true when gMainGameMode == GM_MAP_SCREEN (covers every
 * PauseScreenXxx variant). Returns depthTier 4 (world OBJ), 5 (HUD OBJ), or
 * 6 (map/pause-screen OBJ) -- see the heuristic history comment above this
 * function's call site in CollectSprite for why palBank/shape are used
 * instead of OAM priority or gNextOamSlot. */
int DepthTierForSprite(int palBank, int shape, int priority, int inMapOrPauseScreen);

/* True back-to-front 2D draw-order key for a BG layer (ascending = drawn
 * first = furthest back -- see CollectBgLayer's own comment on this
 * formula). bgIndex: 0-3 (GBA BG0..BG3). priority: raw BGCNT priority field
 * (0=highest/on top .. 3=lowest/backmost). This is the exact mechanism that
 * fixed GitHub issue #5 (layers rendering in front of the character/HUD
 * with the wrong occlusion) -- see commit 36dd5c23's merged opaque/blend
 * draw pass. Locking this in a pure, host-testable function protects that
 * fix from a future silent regression the way the bgIndex==0 depthTier
 * hardcode regressed 36dd5c23's OTHER half (stereo depth) without anyone
 * noticing until this investigation. */
int SortKeyForBg(int bgIndex, int priority);

/* Same draw-order key for an OBJ (sprite) item -- see CollectSprite's own
 * comment on this formula. OBJ always draws above any BG of equal
 * priority (tiebreak +4, higher than any BG's 0-3 bgIndex term). */
int SortKeyForSprite(int priority);

#ifdef __cplusplus
}
#endif

#endif /* PORT_3DS_DEPTH_TIER_H */
