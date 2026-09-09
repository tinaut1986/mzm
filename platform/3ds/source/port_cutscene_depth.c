#include "port_cutscene_depth.h"
#include "port_stereo_depth.h"   /* PORT_TIER_* -- the .inc names them */

#include <stddef.h>

/* Optional, exactly like port_layer_fixes.inc: a stock build has no
 * port_cutscene_depth.inc and every lookup compiles down to "return the
 * default". */
#if defined(__has_include)
#  if __has_include("port_cutscene_depth.inc")
#    define PORT_HAVE_CUTSCENE_DEPTH 1
#  endif
#endif

typedef struct {
    uint8_t  scene;   /* PORT_CUT_SCENE_* */
    uint16_t layout;  /* PORT_CUT_LAYOUT(...) signature, or PORT_CUT_ANY */
    uint8_t  target;  /* PORT_CUT_PRIO/BG/ACTOR/CAPTION encoding */
    int8_t   tier;    /* PORT_TIER_* or PORT_CUT_DEFAULT */
} PortCutsceneDepthEntry;

#ifdef PORT_HAVE_CUTSCENE_DEPTH
#define PORT_CUTSCENE_DEPTH(scene, layout, target, tier) \
    { (uint8_t)(scene), (uint16_t)(layout), (uint8_t)(target), (int8_t)(tier) },
/* Leading sentinel so an .inc with only the header comment still produces a
 * valid (non-empty) initializer. scene 0 == PORT_CUT_SCENE_NONE never matches
 * a real row, and the counts/iteration below skip index 0. */
static const PortCutsceneDepthEntry sAll[] = {
    { 0, PORT_CUT_ANY, 0, PORT_CUT_DEFAULT },
#include "port_cutscene_depth.inc"
};
#define PORT_CUT_COUNT ((int)(sizeof(sAll) / sizeof(sAll[0])) - 1)
#else
static const PortCutsceneDepthEntry sAll[1] = { { 0, PORT_CUT_ANY, 0, PORT_CUT_DEFAULT } };
#define PORT_CUT_COUNT 0
#endif
#define PORT_CUT_FIRST 1   /* skip the sentinel */

/* Runtime override list. The 3DS build never touches this; the layer
 * workbench fills it (PortCutsceneDepth_SetRuntimeOverrides) so it can preview
 * edits that are not written to the .inc yet. When active it REPLACES the
 * compiled list -- the tool shows exactly what its current buffer says. */
static PortCutsceneDepthEntry sRuntime[256];
static int sRuntimeCount = -1;   /* < 0: use the compiled sAll[] */

void PortCutsceneDepth_SetRuntimeOverrides(const uint8_t* entries, int count) {
    if (entries == NULL || count < 0) { sRuntimeCount = -1; return; }
    if (count > (int)(sizeof(sRuntime) / sizeof(sRuntime[0])))
        count = (int)(sizeof(sRuntime) / sizeof(sRuntime[0]));
    for (int i = 0; i < count; ++i) {
        /* 5 bytes/entry: scene, layoutLo, layoutHi, target, tier. */
        sRuntime[i].scene  = entries[i * 5 + 0];
        sRuntime[i].layout = (uint16_t)(entries[i * 5 + 1] | (entries[i * 5 + 2] << 8));
        sRuntime[i].target = entries[i * 5 + 3];
        sRuntime[i].tier   = (int8_t)entries[i * 5 + 4];
    }
    sRuntimeCount = count;
}

bool PortCutsceneDepth_Present(void) {
    return (sRuntimeCount >= 0) ? (sRuntimeCount > 0) : (PORT_CUT_COUNT > 0);
}

uint16_t PortCutsceneDepth_LayerSignature(unsigned dispcnt, const uint8_t priority[4]) {
    uint16_t sig = 0;
    for (int bg = 0; bg < 4; ++bg) {
        int on = (int)((dispcnt >> (8 + bg)) & 1u);
        uint16_t nib = on ? (uint16_t)(priority[bg] & 3u) : (uint16_t)PORT_CUT_OFF;
        sig |= (uint16_t)(nib << (bg * 4));
    }
    return sig;
}

/* Raw game enum values, cited from include/constants/game_state.h and
 * include/constants/cutscene.h. Kept as literals so this file needs no game
 * headers; platform/3ds/tests/stereo_depth_test.c pins the correspondence. */
enum {
    GAME_MODE_INTRO = 1,
    GAME_MODE_CHOZODIA_ESCAPE = 7,
    GAME_MODE_TOURIAN_ESCAPE = 9,
    GAME_MODE_CUTSCENE = 10
};

int PortCutsceneDepth_SceneFromGame(int gameMode, int cutsceneId) {
    switch (gameMode) {
        case GAME_MODE_INTRO:           return PORT_CUT_SCENE_INTRO;
        case GAME_MODE_CHOZODIA_ESCAPE: return PORT_CUT_SCENE_CHOZODIA_ESCAPE;
        case GAME_MODE_TOURIAN_ESCAPE:  return PORT_CUT_SCENE_TOURIAN_ESCAPE;
        case GAME_MODE_CUTSCENE:
            /* Cutscene enum 1..14 maps 1:1, in order, onto
             * PORT_CUT_SCENE_INTRO_TEXT .. PORT_CUT_SCENE_SAMUS_IN_BLUE_SHIP.
             * CUTSCENE_NONE (0) and anything out of range -> NONE. */
            if (cutsceneId >= 1 && cutsceneId <= 14)
                return PORT_CUT_SCENE_INTRO_TEXT + (cutsceneId - 1);
            return PORT_CUT_SCENE_NONE;
        default:
            return PORT_CUT_SCENE_NONE;
    }
}

/* Best matching entry's tier for (scene, layout, target), or PORT_CUT_DEFAULT.
 * A row whose layout equals the current one beats a PORT_CUT_ANY row. Reads
 * the runtime list when the workbench has set one, else the compiled list
 * (whose index 0 is a sentinel -- see above). */
static int LookupTarget(int scene, uint16_t layout, int target) {
    if (scene == PORT_CUT_SCENE_NONE) return PORT_CUT_DEFAULT;
    const PortCutsceneDepthEntry* list;
    int begin, end;
    if (sRuntimeCount >= 0) { list = sRuntime; begin = 0;              end = sRuntimeCount; }
    else                    { list = sAll;     begin = PORT_CUT_FIRST; end = PORT_CUT_FIRST + PORT_CUT_COUNT; }

    int anyTier = PORT_CUT_DEFAULT;
    for (int i = begin; i < end; ++i) {
        if (list[i].scene != (uint8_t)scene || list[i].target != (uint8_t)target)
            continue;
        if (list[i].layout == layout)
            return list[i].tier;              /* exact sub-scene match wins */
        if (list[i].layout == PORT_CUT_ANY && anyTier == PORT_CUT_DEFAULT)
            anyTier = list[i].tier;           /* remember the wildcard fallback */
    }
    return anyTier;
}

int PortCutsceneDepth_TierForPriority(int scene, uint16_t layout, int priority, int defaultTier) {
    int t = LookupTarget(scene, layout, PORT_CUT_PRIO(priority));
    return (t == PORT_CUT_DEFAULT) ? defaultTier : t;
}

int PortCutsceneDepth_BgTier(int scene, uint16_t layout, int bgIndex, int priority, int defaultTier) {
    int t = LookupTarget(scene, layout, PORT_CUT_BG(bgIndex));   /* BG index wins */
    if (t == PORT_CUT_DEFAULT)
        t = LookupTarget(scene, layout, PORT_CUT_PRIO(priority));
    return (t == PORT_CUT_DEFAULT) ? defaultTier : t;
}

int PortCutsceneDepth_ObjTier(int scene, uint16_t layout, int objPriority, int defaultTier) {
    int t = LookupTarget(scene, layout, (objPriority == 0) ? PORT_CUT_CAPTION : PORT_CUT_ACTOR);
    return (t == PORT_CUT_DEFAULT) ? defaultTier : t;
}
