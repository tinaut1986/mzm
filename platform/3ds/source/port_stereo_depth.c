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
    /* Outside gameplay BG0 genuinely IS the text/dialog overlay. In
     * gameplay it is just another world layer whose priority the room
     * picks -- Crateria room 8 puts the Chozo statue backdrop on it, and
     * forcing that to the nearest tier shoved the statue in front of
     * Samus the moment the slider came up. */
    if (bgIndex == 0 && !st->inGameplay) return PORT_TIER_BG_OVERLAY;

    /* Priorities 0 and 1 share the play plane. Splitting them onto their own
     * planes was tried and reordered which tiles read as wrong: two layers the
     * room composites as one flat image were pushed 0.7px apart. Deliberate
     * exceptions go in the curated list (port_layer_fixes.h), not here. */
    switch (st->priority[bgIndex]) {
        case 0:
        case 1:  return PORT_TIER_BG_PLAY; /* -0.3f */
        case 2:  return PORT_TIER_BG_MID;  /* -2.0f */
        default: return PORT_TIER_BG_FAR;  /* -4.0f */
    }
}

/* Every world sprite sits on one plane. The HUD and the pause map do not come
 * through here -- port_gpu_renderer.c picks their tiers directly -- which is
 * what keeps the UI in front without it having a layer of its own. */
int PortStereoDepth_ObjTier(const PortStereoDepthState* st, int objPriority) {
    (void)st;
    (void)objPriority;
    return PORT_TIER_OBJ_P1; /* -0.8f */
}
