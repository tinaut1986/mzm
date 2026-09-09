/*
 * Per-cutscene stereo-depth override tests. Host-only, no ROM, no 3DS.
 *
 * port_cutscene_depth.c is the optional override layer on top of the built-in
 * scene-art spread in port_stereo_depth.c. These tests pin the two things that
 * must hold regardless of whether a port_cutscene_depth.inc exists:
 *
 *   1. The (gMainGameMode, gCurrentCutscene) -> PORT_CUT_SCENE_* mapping, keyed
 *      on the raw game enum values. port_gpu_renderer.c and the workbench's
 *      WASM engine both call PortCutsceneDepth_SceneFromGame(); this is the one
 *      place the correspondence is checked against the enums it mirrors.
 *   2. With no .inc compiled in, every tier lookup returns the caller's default
 *      unchanged -- so a stock build behaves exactly as before this file
 *      existed.
 *
 * A build WITH an .inc is exercised end to end from the layer workbench and on
 * hardware (see docs); the host test deliberately runs the no-.inc path, which
 * is the one that must never regress.
 *
 * Build and run:  make -C platform/3ds test
 */

#include "port_cutscene_depth.h"
#include "port_stereo_depth.h"   /* PORT_TIER_* */

/* The enums this file's mapping mirrors. Copied here (not #included) for the
 * same reason port_cutscene_depth.c copies them: the unit stays free of game
 * headers. If these ever diverge from the game's include/constants headers
 * this test is where it surfaces. */
enum { GM_INTRO = 1, GM_CHOZODIA_ESCAPE = 7, GM_TOURIAN_ESCAPE = 9, GM_CUTSCENE = 10 };
enum {
    CUTSCENE_NONE = 0, CUTSCENE_INTRO_TEXT, CUTSCENE_MOTHERSHIP_MONOLOGUE,
    CUTSCENE_COULD_I_SURVIVE, CUTSCENE_MOTHER_BRAIN_CLOSE_UP, CUTSCENE_KRAID_RISING,
    CUTSCENE_STATUE_OPENING, CUTSCENE_RIDLEY_IN_SPACE, CUTSCENE_RIDLEY_LANDING,
    CUTSCENE_RIDLEY_SPAWNING, CUTSCENE_ENTER_TOURIAN, CUTSCENE_BEFORE_RUINS_TEST,
    CUTSCENE_GETTING_FULLY_POWERED, CUTSCENE_MECHA_RIDLEY_SEES_SAMUS,
    CUTSCENE_SAMUS_IN_BLUE_SHIP, CUTSCENE_COUNT
};

#include <stdio.h>

static int sFailures;
static int sChecks;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        ++sChecks;                                            \
        if (!(cond)) {                                        \
            ++sFailures;                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

static void TestSceneFromGame(void) {
    printf("mapping: (gameMode, cutsceneId) -> PORT_CUT_SCENE_*\n");

    CHECK(PortCutsceneDepth_SceneFromGame(GM_INTRO, 0) == PORT_CUT_SCENE_INTRO, "GM_INTRO");
    CHECK(PortCutsceneDepth_SceneFromGame(GM_CHOZODIA_ESCAPE, 0) == PORT_CUT_SCENE_CHOZODIA_ESCAPE, "GM_CHOZODIA_ESCAPE");
    CHECK(PortCutsceneDepth_SceneFromGame(GM_TOURIAN_ESCAPE, 0) == PORT_CUT_SCENE_TOURIAN_ESCAPE, "GM_TOURIAN_ESCAPE");

    /* GM_CUTSCENE: enum 1..14 maps 1:1 onto PORT_CUT_SCENE_INTRO_TEXT.. */
    CHECK(PortCutsceneDepth_SceneFromGame(GM_CUTSCENE, CUTSCENE_INTRO_TEXT) == PORT_CUT_SCENE_INTRO_TEXT, "CUTSCENE_INTRO_TEXT");
    CHECK(PortCutsceneDepth_SceneFromGame(GM_CUTSCENE, CUTSCENE_KRAID_RISING) == PORT_CUT_SCENE_KRAID_RISING, "CUTSCENE_KRAID_RISING");
    CHECK(PortCutsceneDepth_SceneFromGame(GM_CUTSCENE, CUTSCENE_SAMUS_IN_BLUE_SHIP) == PORT_CUT_SCENE_SAMUS_IN_BLUE_SHIP, "CUTSCENE_SAMUS_IN_BLUE_SHIP");
    CHECK(PORT_CUT_SCENE_SAMUS_IN_BLUE_SHIP - PORT_CUT_SCENE_INTRO_TEXT == CUTSCENE_SAMUS_IN_BLUE_SHIP - CUTSCENE_INTRO_TEXT,
          "the two enums must stay the same length");

    /* Non-scene modes and out-of-range ids -> NONE. */
    CHECK(PortCutsceneDepth_SceneFromGame(GM_CUTSCENE, CUTSCENE_NONE) == PORT_CUT_SCENE_NONE, "CUTSCENE_NONE");
    CHECK(PortCutsceneDepth_SceneFromGame(GM_CUTSCENE, CUTSCENE_COUNT) == PORT_CUT_SCENE_NONE, "id out of range");
    CHECK(PortCutsceneDepth_SceneFromGame(4 /* GM_INGAME */, 0) == PORT_CUT_SCENE_NONE, "gameplay mode");
    CHECK(PortCutsceneDepth_SceneFromGame(0, 0) == PORT_CUT_SCENE_NONE, "mode 0");
}

static void TestNoIncIsInert(void) {
    printf("empty list: every lookup returns the caller's default\n");
    /* A real port_cutscene_depth.inc may be linked in; force an empty runtime
     * list so this checks the "no overrides" path regardless. */
    static const uint8_t none[1] = {0};
    PortCutsceneDepth_SetRuntimeOverrides(none, 0);
    CHECK(!PortCutsceneDepth_Present(), "empty override list -> not present");

    const uint16_t layouts[] = { 0, PORT_CUT_ANY,
                                 PORT_CUT_LAYOUT(0, 1, 2, 3),
                                 PORT_CUT_LAYOUT(2, 1, PORT_CUT_OFF, PORT_CUT_OFF) };
    for (int scene = 0; scene < PORT_CUT_SCENE_COUNT; ++scene) {
        for (size_t li = 0; li < sizeof(layouts) / sizeof(layouts[0]); ++li) {
            uint16_t L = layouts[li];
            for (int st = 0; st < 3; ++st) {
                int S = (st == 2) ? PORT_CUT_STAGE_ANY : st;
                for (int p = 0; p < 4; ++p) {
                    CHECK(PortCutsceneDepth_TierForPriority(scene, L, S, p, PORT_TIER_BG_MID) == PORT_TIER_BG_MID,
                          "TierForPriority scene %d layout %u stage %d prio %d", scene, L, S, p);
                    CHECK(PortCutsceneDepth_BgTier(scene, L, S, p, p, PORT_TIER_BG_FAR) == PORT_TIER_BG_FAR,
                          "BgTier scene %d layout %u stage %d bg/prio %d", scene, L, S, p);
                }
                CHECK(PortCutsceneDepth_ObjTier(scene, L, S, 0, PORT_TIER_BG_OVERLAY) == PORT_TIER_BG_OVERLAY,
                      "ObjTier caption scene %d layout %u stage %d", scene, L, S);
                CHECK(PortCutsceneDepth_ObjTier(scene, L, S, 1, PORT_TIER_BG_PLAY) == PORT_TIER_BG_PLAY,
                      "ObjTier actor scene %d layout %u stage %d", scene, L, S);
            }
        }
    }
}

static void TestLayerSignature(void) {
    printf("signature: BG enable + BGCNT priorities -> 16-bit layout key\n");
    uint8_t prio[4] = { 2, 1, 0, 3 };
    /* All four BGs on (DISPCNT bits 8-11): nibbles are the priorities. */
    CHECK(PortCutsceneDepth_LayerSignature(0x0F00u, prio) == PORT_CUT_LAYOUT(2, 1, 0, 3),
          "all BGs on -> priorities packed");
    /* BG2 + BG3 off (bits 10,11 clear): their nibbles read PORT_CUT_OFF. */
    CHECK(PortCutsceneDepth_LayerSignature(0x0300u, prio) ==
              PORT_CUT_LAYOUT(2, 1, PORT_CUT_OFF, PORT_CUT_OFF),
          "disabled BGs -> PORT_CUT_OFF nibble");
    /* Two frames of one montage cutscene with different BG usage must differ. */
    CHECK(PortCutsceneDepth_LayerSignature(0x0F00u, prio) !=
              PortCutsceneDepth_LayerSignature(0x0700u, prio),
          "different BG enable -> different signature");
    /* A real cutscene frame never yields PORT_CUT_ANY (all BGs off). */
    CHECK(PortCutsceneDepth_LayerSignature(0x0700u, prio) != PORT_CUT_ANY, "not the wildcard");
}

static void TestTargetEncodingsDistinct(void) {
    printf("encoding: PRIO / BG / ACTOR / CAPTION targets never collide\n");
    int seen[0x40] = {0};
    for (int i = 0; i < 4; ++i) {
        int a = PORT_CUT_PRIO(i), b = PORT_CUT_BG(i);
        CHECK(a >= 0 && a < 0x40 && !seen[a]++, "PORT_CUT_PRIO(%d) unique", i);
        CHECK(b >= 0 && b < 0x40 && !seen[b]++, "PORT_CUT_BG(%d) unique", i);
    }
    CHECK(!seen[PORT_CUT_ACTOR]++, "PORT_CUT_ACTOR unique");
    CHECK(!seen[PORT_CUT_CAPTION]++, "PORT_CUT_CAPTION unique");
}

int main(void) {
    TestSceneFromGame();
    TestLayerSignature();
    TestTargetEncodingsDistinct();
    TestNoIncIsInert();   /* last: it wipes the override list */
    printf("\n%d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
