#pragma once

/*
 * Per-cutscene stereo-depth overrides.
 *
 * port_stereo_depth.c gives every scene-art cutscene (GM_INTRO,
 * GM_CHOZODIA_ESCAPE, GM_TOURIAN_ESCAPE, GM_CUTSCENE) ONE hardcoded parallax
 * spread: BGCNT priority 0 -> BG_PLAY, 1 -> BG_MID, 2/3 -> BG_FAR, the caption
 * (OBJ priority 0) -> BG_OVERLAY, an actor (OBJ priority >= 1) -> BG_PLAY.
 *
 * That is a sensible default, but a specific cutscene sometimes wants a layer
 * somewhere else. This is the curated list that says so, authored in
 * tools/layer-workbench (GRABACION mode, from a real capture of the scene) and
 * written out as platform/3ds/source/port_cutscene_depth.inc, an X-macro list:
 *
 *   PORT_CUTSCENE_DEPTH(scene, layout, target, tier)
 *
 *     scene  - a PORT_CUT_SCENE_* value (below). Identifies the cutscene.
 *     layout - which sub-scene of that cutscene the row applies to. A single
 *              GM_CUTSCENE (e.g. the Tourian-escape montage) runs several
 *              pages back to back, each with its own set of BGs and BGCNT
 *              priorities, all under ONE cutscene id. A row's `layout` is a
 *              PORT_CUT_LAYOUT(p0,p1,p2,p3) signature -- the BGCNT priority of
 *              each BG, or PORT_CUT_OFF for a disabled one -- so it only fires
 *              while that page's layer config is on screen. PORT_CUT_ANY
 *              applies to the whole cutscene. A layout-specific row wins over
 *              a PORT_CUT_ANY one.
 *     target - PORT_CUT_PRIO(0..3): a BGCNT priority
 *              PORT_CUT_BG(0..3):   a physical BG index (wins over PRIO)
 *              PORT_CUT_ACTOR:      cutscene actor sprites  (OBJ priority >= 1)
 *              PORT_CUT_CAPTION:    caption sprites         (OBJ priority 0)
 *     tier   - a PORT_TIER_* value from port_stereo_depth.h, or
 *              PORT_CUT_DEFAULT to fall back to the built-in spread.
 *
 * The file is OPTIONAL. Without it every lookup here returns the default that
 * port_stereo_depth.c computed, so a build with no overrides and a build with
 * them differ only by that one file -- same contract as port_layer_fixes.inc.
 *
 * No <3ds.h> and no game headers, so platform/3ds/tests can drive it on the
 * host, same as port_stereo_depth.h / port_layer_fixes.h. The mapping from the
 * game's (gMainGameMode, gCurrentCutscene) to a PORT_CUT_SCENE_* id lives in
 * PortCutsceneDepth_SceneFromGame(), keyed on the raw enum VALUES (pinned by
 * the host test) so port_gpu_renderer.c and the workbench's WASM engine share
 * one implementation.
 */

#include <stdbool.h>
#include <stdint.h>

/* Scene ids. The three single-scene game modes plus one per Cutscene enum
 * value (include/constants/cutscene.h). Values are this table's own -- not the
 * game's -- and only need to be stable, since PortCutsceneDepth_SceneFromGame()
 * is the sole bridge to the game enums. */
enum {
    PORT_CUT_SCENE_NONE = 0,

    /* Single-scene game modes (no per-cutscene id). */
    PORT_CUT_SCENE_INTRO,            /* GM_INTRO (1) */
    PORT_CUT_SCENE_CHOZODIA_ESCAPE,  /* GM_CHOZODIA_ESCAPE (7) */
    PORT_CUT_SCENE_TOURIAN_ESCAPE,   /* GM_TOURIAN_ESCAPE (9) */

    /* GM_CUTSCENE (10) story scenes, one per Cutscene enum value. */
    PORT_CUT_SCENE_INTRO_TEXT,             /* CUTSCENE_INTRO_TEXT (1) */
    PORT_CUT_SCENE_MOTHERSHIP_MONOLOGUE,   /* CUTSCENE_MOTHERSHIP_MONOLOGUE (2) */
    PORT_CUT_SCENE_COULD_I_SURVIVE,        /* CUTSCENE_COULD_I_SURVIVE (3) */
    PORT_CUT_SCENE_MOTHER_BRAIN_CLOSE_UP,  /* CUTSCENE_MOTHER_BRAIN_CLOSE_UP (4) */
    PORT_CUT_SCENE_KRAID_RISING,           /* CUTSCENE_KRAID_RISING (5) */
    PORT_CUT_SCENE_STATUE_OPENING,         /* CUTSCENE_STATUE_OPENING (6) */
    PORT_CUT_SCENE_RIDLEY_IN_SPACE,        /* CUTSCENE_RIDLEY_IN_SPACE (7) */
    PORT_CUT_SCENE_RIDLEY_LANDING,         /* CUTSCENE_RIDLEY_LANDING (8) */
    PORT_CUT_SCENE_RIDLEY_SPAWNING,        /* CUTSCENE_RIDLEY_SPAWNING (9) */
    PORT_CUT_SCENE_ENTER_TOURIAN,          /* CUTSCENE_ENTER_TOURIAN (10) */
    PORT_CUT_SCENE_BEFORE_RUINS_TEST,      /* CUTSCENE_BEFORE_RUINS_TEST (11) */
    PORT_CUT_SCENE_GETTING_FULLY_POWERED,  /* CUTSCENE_GETTING_FULLY_POWERED (12) */
    PORT_CUT_SCENE_MECHA_RIDLEY_SEES_SAMUS,/* CUTSCENE_MECHA_RIDLEY_SEES_SAMUS (13) */
    PORT_CUT_SCENE_SAMUS_IN_BLUE_SHIP,     /* CUTSCENE_SAMUS_IN_BLUE_SHIP (14) */

    PORT_CUT_SCENE_COUNT
};

/* target encodings for the .inc. */
#define PORT_CUT_PRIO(p)   (0x00 | ((p) & 3))   /* BGCNT priority 0..3 */
#define PORT_CUT_BG(i)     (0x10 | ((i) & 3))   /* physical BG index 0..3 */
#define PORT_CUT_ACTOR     0x20                 /* OBJ priority >= 1 */
#define PORT_CUT_CAPTION   0x21                 /* OBJ priority 0 */

/* tier sentinel: "no override, keep the computed default". */
#define PORT_CUT_DEFAULT   (-1)

/* layout signature: 4 bits per BG (BGCNT priority 0..3, or PORT_CUT_OFF for a
 * disabled BG), BG0 in the low nibble. PORT_CUT_ANY (all-off, never a real
 * cutscene frame) means "every sub-scene of this cutscene". */
#define PORT_CUT_OFF          0xF
#define PORT_CUT_LAYOUT(p0, p1, p2, p3) \
    ((uint16_t)(((p0) & 0xF) | (((p1) & 0xF) << 4) | \
                (((p2) & 0xF) << 8) | (((p3) & 0xF) << 12)))
#define PORT_CUT_ANY          ((uint16_t)0xFFFFu)

/* The current frame's layout signature, from DISPCNT bits 8-11 (BG enable)
 * and the four BGCNT priorities. Same function port_gpu_renderer.c and the
 * workbench's WASM engine both use, so a row authored against a capture keys
 * to exactly what the port will compute at run time. */
uint16_t PortCutsceneDepth_LayerSignature(unsigned dispcnt, const uint8_t priority[4]);

/* True when an override list is in effect -- a compiled port_cutscene_depth.inc
 * or a runtime list set below. Lets the caller skip the lookups on a stock
 * build. */
bool PortCutsceneDepth_Present(void);

/* Replace the compiled list with a caller-supplied one for the rest of the
 * process. `entries` is `count` x { uint8 scene, uint8 layoutLo, uint8 layoutHi,
 * uint8 target, int8 tier } (5 bytes, little-endian layout). Pass (NULL, 0) or
 * a negative count to revert to the compiled list.
 *
 * The 3DS build never calls this. It exists for the layer workbench's WASM
 * engine, so it can resolve tiers against edits the user has not written to
 * the .inc yet -- using this exact lookup code, not a JS copy of it. */
void PortCutsceneDepth_SetRuntimeOverrides(const uint8_t* entries, int count);

/* (gMainGameMode, gCurrentCutscene) -> PORT_CUT_SCENE_*, or PORT_CUT_SCENE_NONE
 * when the mode is not a scene-art cutscene. cutsceneId is only read for
 * GM_CUTSCENE. Pure integer mapping -- no game headers -- pinned by the host
 * test. */
int PortCutsceneDepth_SceneFromGame(int gameMode, int cutsceneId);

/* Depth tier for a BG layer of the given index and BGCNT priority, in `scene`
 * while `layout` is on screen. Returns `defaultTier` when nothing matches. A
 * PORT_CUT_BG entry wins over a PORT_CUT_PRIO one; a layout-specific entry
 * wins over a PORT_CUT_ANY one. */
int PortCutsceneDepth_BgTier(int scene, uint16_t layout, int bgIndex, int priority, int defaultTier);

/* Same, keyed on a raw priority only (no BG index) -- for a sprite that set
 * its OAM priority to composite with a BG. PORT_CUT_BG entries are ignored. */
int PortCutsceneDepth_TierForPriority(int scene, uint16_t layout, int priority, int defaultTier);

/* Depth tier for a cutscene sprite: PORT_CUT_CAPTION when objPriority == 0,
 * PORT_CUT_ACTOR otherwise. Returns `defaultTier` when not overridden. */
int PortCutsceneDepth_ObjTier(int scene, uint16_t layout, int objPriority, int defaultTier);
