#include "port_stereo_depth.h"

/* Stereo depth mapping (docs/future-roadmap-and-architecture.md's table).
 * Values are px of parallax shift at full slider; the render loop flips the
 * sign between eyes. Larger = nearer to the viewer.
 *
 * Three presets of the SAME ordering, differing only in how far apart the
 * world layers sit. The order is what keeps stereo from contradicting the
 * 2D compositor and is identical in all three (stereo_depth_test.c checks
 * every preset); the spread is a judgement call about a tradeoff the GBA
 * never had to make.
 *
 * The tradeoff: room data composites several layers into ONE flat image,
 * because on GBA that is all it can be. Rooms therefore paint a few detail
 * pixels on a front layer directly over a backdrop tile on a layer behind
 * it -- in the 2026-08-28 recording, 217 such overlaps, the front tile
 * carrying a median 21 opaque pixels over a near-solid backdrop tile, 43%
 * of them 16 pixels or fewer. Flat, they are specks ON the wall. Given
 * depth, they float in FRONT of the wall they are painted on, by however
 * far apart the two layers are. No per-layer assignment can avoid this:
 * when a sprite priority sits between the two layers (as here), merging
 * them would put that sprite on the wrong side. Only the DISTANCE is
 * negotiable, and shrinking it below the eye-offset rounding in the render
 * loop makes the seam disappear entirely.
 *
 * BG priority-0 vs priority-2 separation, which is where those seams show:
 *   BOLD   1.7px -- the original tuning, strongest depth
 *   SOFT   0.6px
 *   FLAT   0.2px -- seams round away; least depth between layers
 * HUD, dialog-overlay and pause-map tiers are not world layers and do not
 * change between presets. */
static const float kTierEyeOffsetPx[PORT_STEREO_SPREAD_COUNT][PORT_TIER_COUNT] = {
    /* [PORT_STEREO_SPREAD_BOLD] */
    {
        -4.0f, /* 0: BG priority 3: far background / sky */
        -2.0f, /* 1: BG priority 2: mid background / caves */
        -0.3f, /* 2: BG priority 0 and 1: platforms / interactive ground */
        +1.8f, /* 3: BG0 outside gameplay: text overlay / dialogs / map */
        -0.8f, /* 4: world OBJ: Samus / enemies / particles */
        +2.0f, /* 5: HUD OBJ: health bar, missiles, tanks, minimap icons */
        +1.2f, /* 6: Map/pause-screen OBJ */
    },
    /* [PORT_STEREO_SPREAD_SOFT] */
    {
        -1.2f, -0.9f, -0.3f, +1.8f, -0.5f, +2.0f, +1.2f,
    },
    /* [PORT_STEREO_SPREAD_FLAT] */
    {
        -0.50f, -0.40f, -0.20f, +1.8f, -0.25f, +2.0f, +1.2f,
    },
};

static int sSpread = PORT_STEREO_SPREAD_BOLD;

void PortStereoDepth_SetSpread(int spread) {
    if (spread < 0 || spread >= PORT_STEREO_SPREAD_COUNT) return;
    sSpread = spread;
}

int PortStereoDepth_GetSpread(void) { return sSpread; }

const char* PortStereoDepth_SpreadName(int spread) {
    return PortStereoDepth_SpreadNameLang(spread, 6); /* Default to Spanish for backward compatibility */
}

const char* PortStereoDepth_SpreadNameLang(int spread, int lang) {
    if (lang == 6) { /* Spanish */
        switch (spread) {
            case PORT_STEREO_SPREAD_BOLD: return "MARCADO";
            case PORT_STEREO_SPREAD_SOFT: return "SUAVE";
            case PORT_STEREO_SPREAD_FLAT: return "PLANO";
            default: return "?";
        }
    } else if (lang == 3) { /* German */
        switch (spread) {
            case PORT_STEREO_SPREAD_BOLD: return "STARK";
            case PORT_STEREO_SPREAD_SOFT: return "SANFT";
            case PORT_STEREO_SPREAD_FLAT: return "FLACH";
            default: return "?";
        }
    } else if (lang == 4) { /* French */
        switch (spread) {
            case PORT_STEREO_SPREAD_BOLD: return "FORT";
            case PORT_STEREO_SPREAD_SOFT: return "DOUX";
            case PORT_STEREO_SPREAD_FLAT: return "PLAT";
            default: return "?";
        }
    } else if (lang == 5) { /* Italian */
        switch (spread) {
            case PORT_STEREO_SPREAD_BOLD: return "MARCATO";
            case PORT_STEREO_SPREAD_SOFT: return "LEGGERO";
            case PORT_STEREO_SPREAD_FLAT: return "PIATTO";
            default: return "?";
        }
    } else if (lang == 0 || lang == 1) { /* Japanese / Hiragana */
        switch (spread) {
            case PORT_STEREO_SPREAD_BOLD: return "BOLD";
            case PORT_STEREO_SPREAD_SOFT: return "SOFT";
            case PORT_STEREO_SPREAD_FLAT: return "FLAT";
            default: return "?";
        }
    } else { /* English (2) / Default */
        switch (spread) {
            case PORT_STEREO_SPREAD_BOLD: return "BOLD";
            case PORT_STEREO_SPREAD_SOFT: return "SOFT";
            case PORT_STEREO_SPREAD_FLAT: return "FLAT";
            default: return "?";
        }
    }
}

float PortStereoDepth_TierPx(int tier) {
    if (tier < 0 || tier >= PORT_TIER_COUNT) return 0.0f;
    return kTierEyeOffsetPx[sSpread][tier];
}

float PortStereoDepth_TierPxFor(int spread, int tier) {
    if (tier < 0 || tier >= PORT_TIER_COUNT) return 0.0f;
    if (spread < 0 || spread >= PORT_STEREO_SPREAD_COUNT) return 0.0f;
    return kTierEyeOffsetPx[spread][tier];
}

int PortStereoDepth_BgTier(const PortStereoDepthState* st, int bgIndex) {
    /* BG0 gets the pop-forward overlay tier ONLY where it genuinely is the
     * text/dialog/pause-map layer (bg0IsOverlayText). Two ways this used to
     * be wrong:
     *  - In gameplay BG0 is just another world layer whose priority the
     *    room picks -- Crateria room 8 puts the Chozo statue backdrop on
     *    it, and forcing that nearest shoved the statue in front of Samus.
     *  - "Outside gameplay" is not enough: the Chozodia escape cutscene
     *    puts the blue-ship artwork on BG0 (priority 0) while the
     *    "mission accomplished" caption is OBJ sprites, so lifting BG0 here
     *    floated the ship in front of its own caption. */
    if (bgIndex == 0 && st->bg0IsOverlayText) return PORT_TIER_BG_OVERLAY;
    return PortStereoDepth_BgTierForPriority(st, st->priority[bgIndex]);
}

int PortStereoDepth_BgTierForPriority(const PortStereoDepthState* st, int priority) {
    /* Priorities 0 and 1 share the play plane. Splitting them for EVERY room
     * was tried and reordered which tiles read as wrong: two layers the room
     * composites as one flat image were pushed 0.7px apart.
     *
     * The one place the merge is wrong is a samusOnTopOfBackgrounds room (see
     * the field comment): there priority 1 is scenery Samus stands in front
     * of, not part of the play plane, so it drops to the mid plane -- behind
     * the one world-sprite tier (OBJ_P1, -0.8f) exactly as the priority
     * numbers and the sprite bump already say it should be. Priority 0 stays
     * put: it is the room's only real foreground layer. Everything else is
     * unchanged, so a normal room still gets the merge.
     *
     * Split out from BgTier so a sprite that deliberately matches a BG's
     * priority (BossStatue: bgPriority = BG1's priority, so the sprite face
     * composites with the BG that carries the rest of the statue) can land
     * on that same plane instead of the one forced world-sprite tier.
     *
     * Other deliberate exceptions go in the curated list (port_layer_fixes.h),
     * not here. */
    switch (priority & 3) {
        case 0:  return PORT_TIER_BG_PLAY; /* -0.3f */
        case 1:  return st->samusOnTopOfBackgrounds ? PORT_TIER_BG_MID   /* -2.0f */
                                                    : PORT_TIER_BG_PLAY; /* -0.3f */
        case 2:  return PORT_TIER_BG_MID;  /* -2.0f */
        default: return PORT_TIER_BG_FAR;  /* -4.0f */
    }
}

/* Every world sprite sits on one plane. The HUD and the pause map do not come
 * through here -- port_gpu_renderer.c picks their tiers directly -- which is
 * what keeps the UI in front without it having a layer of its own.
 *
 * In gameplay that plane sits just BEHIND the shared BG play tier so
 * foreground platforms read with thickness in front of Samus (she can duck
 * behind them). Outside gameplay the only sprites routed here are cutscene
 * actors, and a cutscene never puts one behind an equal-or-higher-priority
 * BG the way a room's foreground platform does -- the actor is the subject,
 * painted over its own BG backdrops. Leaving it on the gameplay plane makes
 * those backdrops (e.g. the mountains behind the Ridley-landing mothership,
 * BG priority 1 -> BG_PLAY) float stereoscopically IN FRONT of the sprite
 * that visibly draws over them. Park cutscene sprites on the play plane
 * instead: coplanar with a priority-0/1 backdrop (occlusion then orders
 * them, matching the GBA), still behind a priority-0 BG0 overlay. */
int PortStereoDepth_ObjTier(const PortStereoDepthState* st, int objPriority) {
    (void)objPriority;
    if (!st->inGameplay)
        return PORT_TIER_BG_PLAY; /* -0.3f: coplanar with the cutscene backdrop */
    return PORT_TIER_OBJ_P1; /* -0.8f */
}
