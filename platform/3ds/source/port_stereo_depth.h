#pragma once

/*
 * Stereo depth assignment, as a pure function of GBA register state.
 *
 * The 3DS port invents depth the GBA does not have, and every bug in that
 * invention so far has been one of exactly two failures:
 *
 *   1. One visual object split across two depth planes.
 *   2. A contradiction with the 2D compositor -- a layer placed nearer than
 *      something that visibly draws OVER it, so parallax says "in front"
 *      while occlusion says "behind".
 *
 * Both are properties of the MAPPING, not of the rendering, so both can be
 * checked without a GPU, without a 3DS and without the ROM. That is the
 * whole reason this lives in its own header with no <3ds.h>, no citro
 * includes and no globals: platform/3ds/tests/stereo_depth_test.c compiles
 * these exact functions on the host and enumerates the entire input space
 * against independently-derived GBA compositing rules.
 *
 * Anything that changes how depth is picked belongs HERE, not inlined into
 * port_gpu_renderer.c, or it silently escapes those tests.
 */

#include <stdbool.h>
#include <stdint.h>

/* Everything the depth decision is allowed to depend on. Deliberately not a
 * pointer into gIoMem: the test builds these by hand, and a field that has
 * to be filled in explicitly is a field a new rule cannot quietly start
 * reading from a global instead. */
typedef struct {
    uint8_t priority[4];  /* BGCNT bits 0-1, per BG */
    bool inGameplay;      /* gMainGameMode == GM_INGAME */

    /* True only when BG0 genuinely carries the text/dialog/pause-map
     * overlay -- menus, in-game dialogs, the pause map. It is NOT the same
     * as "outside gameplay": a scene-art cutscene (the Chozodia escape's
     * "mission accomplished", in-game story cutscenes) draws its ARTWORK on
     * BG0 while the on-screen caption is OBJ sprites. Lifting BG0 to the
     * overlay tier there floats the artwork stereoscopically in front of
     * its own caption -- occlusion says the letters are on top, parallax
     * says they are behind. Set from gMainGameMode by the renderer; the
     * host test sets it by hand per scene. See BgTier. */
    bool bg0IsOverlayText;

    /* gSamusOnTopOfBackgrounds (src/transparency.c). A handful of room
     * transparency configs set it, and they all lay the room out the same
     * way: BG1 at BGCNT priority 0 is the only foreground layer, every
     * other BG is scenery Samus is meant to stand in FRONT of, and the
     * sprite pipeline bumps every sprite one priority nearer to match
     * (SpriteDraw: `if (gSamusOnTopOfBackgrounds && bgPriority) bgPriority--`).
     * On GBA the priority numbers carry that; in stereo the priority-0/1
     * merge hides it, leaving a priority-1 scenery layer on the play plane
     * IN FRONT of Samus while she visibly draws over it. When this is set,
     * a priority-1 BG drops to the mid plane instead -- see BgTier. Not a
     * hardware register, but snapshotted from room config right next to the
     * priorities, so the mapping stays a pure function of its input. */
    bool samusOnTopOfBackgrounds;
} PortStereoDepthState;

/* Depth tier indices. port_gpu_renderer.c picks the HUD and map tiers by
 * number, so their values are part of the contract. */
enum {
    PORT_TIER_BG_FAR = 0,     /* -4.0f */
    PORT_TIER_BG_MID = 1,     /* -2.0f */
    PORT_TIER_BG_PLAY = 2,    /* -0.3f */
    PORT_TIER_BG_OVERLAY = 3, /* +1.8f */
    PORT_TIER_OBJ_P1 = 4,     /* -0.8f */
    PORT_TIER_OBJ_HUD = 5,    /* +2.0f */
    PORT_TIER_OBJ_MAP = 6,    /* +1.2f */
    PORT_TIER_COUNT = 7
};

/*
 * Depth follows BGCNT priority and nothing else.
 *
 * Earlier versions tried to be cleverer -- deriving depth from collision
 * data per tile, or merging whole layers onto a shared plane -- to stop a
 * single object drawn across two layers from splitting across two depth
 * planes. Every one of those attempts broke something else, because room
 * data genuinely composites several layers into one flat image and the GBA
 * has no depth for any of it to be wrong about. Where a room paints part of
 * an object on another layer, that IS the game's own data, and the port
 * reproduces it rather than second-guessing it.
 *
 * Deliberate exceptions belong in a curated correction list, where a human
 * has looked at each entry -- not in a rule that infers them.
 */

/* Depth tier for a BG layer, including the outside-gameplay BG0 overlay
 * rule (menus and dialogs genuinely do put text on BG0). */
int PortStereoDepth_BgTier(const PortStereoDepthState* st, int bgIndex);

/* The BG-tier mapping keyed on a raw BGCNT priority (0-3) rather than a BG
 * index -- no overlay rule. For a sprite that deliberately sets its OAM
 * priority to match a BG it must composite with. */
int PortStereoDepth_BgTierForPriority(const PortStereoDepthState* st, int priority);

/* Depth tier for a sprite of the given OAM priority. */
int PortStereoDepth_ObjTier(const PortStereoDepthState* st, int objPriority);

/* How far apart the world layers sit. Same ordering in every preset -- only
 * the distance changes. See the table in port_stereo_depth.c for why the
 * distance is a tradeoff at all. */
enum {
    PORT_STEREO_SPREAD_BOLD = 0, /* BG p0..p2 1.7px apart: strongest depth */
    PORT_STEREO_SPREAD_SOFT = 1, /* 0.6px */
    PORT_STEREO_SPREAD_FLAT = 2, /* 0.2px: layer seams round away */
    PORT_STEREO_SPREAD_COUNT = 3
};

void PortStereoDepth_SetSpread(int spread);
int PortStereoDepth_GetSpread(void);
const char* PortStereoDepth_SpreadName(int spread);
const char* PortStereoDepth_SpreadNameLang(int spread, int lang);

/* Parallax shift in px at full slider for a tier; larger = nearer to the
 * viewer. The single source of truth -- port_gpu_renderer.c's render loop
 * reads this same table. */
float PortStereoDepth_TierPx(int tier);
/* Same, for an explicit preset rather than the active one. The test uses
 * this to check every preset without mutating global state. */
float PortStereoDepth_TierPxFor(int spread, int tier);
