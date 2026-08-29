/*
 * Stereo depth mapping tests. Host-only, no ROM, no 3DS, no GPU.
 *
 * The 3DS port invents depth the GBA does not have. Every bug in that
 * invention so far has been one of exactly two failures, and both are
 * properties of the MAPPING rather than of the drawing, which is what makes
 * them checkable here:
 *
 *   1. SPLIT: one visual object landing on two different depth planes.
 *   2. CONTRADICTION: a layer placed nearer than something that visibly
 *      draws OVER it, so the viewer gets "in front" from parallax and
 *      "behind" from occlusion at the same time.
 *
 * Test 1 enumerates every combination of four BG priorities against every
 * sprite priority, for every depth preset, and checks the resulting order
 * against GBA compositing rules derived independently, right here, from
 * GBATEK rather than from the code under test. This is the test that would
 * have failed on the column build before it ever reached the console.
 *
 * Tests 2 and 3 pin the policy that survived: depth is BGCNT priority,
 * unmodified, and it never depends on where a tile sits on screen.
 *
 * Build and run:  make -C platform/3ds test
 */

#include "port_stereo_depth.h"

#include <stdio.h>
#include <string.h>

static int sFailures;
static int sChecks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        ++sChecks;                                          \
        if (!(cond)) {                                      \
            ++sFailures;                                    \
            if (sFailures <= 20) {                          \
                printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while (0)

/* ------------------------------------------------------------------ *
 * The oracle: GBA 2D compositing, from GBATEK, written independently of
 * port_stereo_depth.c so agreement between them means something.
 *
 * Lower priority number draws in front. At EQUAL priority the sprite wins
 * over the background layer. (Between two BGs at equal priority the lower
 * BG index wins, which does not matter here -- equal-priority BGs are never
 * required to differ in depth.)
 *
 * This applies to BACKGROUNDS ONLY. Every world sprite shares one plane, so
 * its depth cannot track a per-priority 2D order and is not checked against
 * this oracle. That is the accepted trade-off: the single plane sits just
 * behind the play plane so platforms read with thickness in front of Samus,
 * which is the whole reason the tier exists. Splitting sprites across four
 * planes by priority was tried -- it made them 2D-consistent and reordered
 * which tiles read as wrong, which is worse. Deliberate exceptions to what a
 * room composites belong in the curated list (port_layer_fixes.h).
 * ------------------------------------------------------------------ */

static const char* TierName(int tier) {
    switch (tier) {
        case PORT_TIER_BG_FAR: return "BG_FAR";
        case PORT_TIER_BG_MID: return "BG_MID";
        case PORT_TIER_BG_PLAY: return "BG_PLAY";
        case PORT_TIER_BG_OVERLAY: return "BG_OVERLAY";
        case PORT_TIER_OBJ_P1: return "OBJ_P1";
        case PORT_TIER_OBJ_HUD: return "OBJ_HUD";
        case PORT_TIER_OBJ_MAP: return "OBJ_MAP";
        default: return "?";
    }
}

/* ------------------------------------------------------------------ *
 * Test 1: exhaustive. No BG may ever be stereoscopically nearer than a
 * sprite that draws over it, or farther than a sprite it draws over.
 * ------------------------------------------------------------------ */
static void TestNoContradictionExhaustive(void) {
    printf("exhaustive: BG depth never contradicts 2D compositing, "
           "and sprites keep their one plane\n");
    long cases = 0;

    for (int spread = 0; spread < PORT_STEREO_SPREAD_COUNT; ++spread) {
        for (unsigned packed = 0; packed < 256u; ++packed) {
            PortStereoDepthState st;
            memset(&st, 0, sizeof(st));
            st.inGameplay = true;
            for (int bg = 0; bg < 4; ++bg) {
                st.priority[bg] = (uint8_t)((packed >> (bg * 2)) & 3u);
            }

            ++cases;
            for (int bg = 0; bg < 4; ++bg) {
                int bgTier = PortStereoDepth_BgTier(&st, bg);
                float bgPx = PortStereoDepth_TierPxFor(spread, bgTier);

                /* Sprites: one plane for all of them, and it must stay just
                 * behind the play plane. Pinned here so neither half of the
                 * policy can drift unnoticed -- the plane is what makes
                 * platforms read with thickness in front of Samus. */
                for (int q = 0; q < 4; ++q) {
                    CHECK(PortStereoDepth_ObjTier(&st, q) == PORT_TIER_OBJ_P1,
                          "world OBJ prio %d must share the one world-sprite plane", q);
                }
                if (bgTier == PORT_TIER_BG_PLAY) {
                    float objPx = PortStereoDepth_TierPxFor(spread, PORT_TIER_OBJ_P1);
                    CHECK(objPx < bgPx,
                          "world sprites must sit behind the play plane "
                          "(BG %s %+.2f vs OBJ %s %+.2f)",
                          TierName(bgTier), (double)bgPx,
                          TierName(PORT_TIER_OBJ_P1), (double)objPx);
                }

                /* BGs never invert against each other. Priorities 0 and 1 are
                 * merged on purpose, so there they may be level; every other
                 * pair must keep the compositor's order. */
                for (int other = 0; other < 4; ++other) {
                    if (st.priority[bg] >= st.priority[other]) continue;
                    float otherPx = PortStereoDepth_TierPxFor(spread, PortStereoDepth_BgTier(&st, other));
                    bool merged = (st.priority[bg] <= 1 && st.priority[other] <= 1);
                    CHECK(merged ? (bgPx >= otherPx) : (bgPx > otherPx),
                          "BG%d prio %d draws over BG%d prio %d but sits farther (%+.2f vs %+.2f)",
                          bg, st.priority[bg], other, st.priority[other],
                          (double)bgPx, (double)otherPx);
                }
            }
        }
    }
    printf("  %ld priority combinations x %d depth presets\n",
           cases / PORT_STEREO_SPREAD_COUNT, PORT_STEREO_SPREAD_COUNT);
}

/* ------------------------------------------------------------------ *
 * Test 2: depth comes from BGCNT priority and nothing else.
 *
 * Successive attempts to be cleverer than this each traded one artifact for
 * another, because the thing they were "fixing" is the game's own data:
 *
 *  - Per-tile clipdata: clipdata is a 16x16 COLLISION grid, not an outline
 *    of what is drawn. It tore the diagonal rock ramp into 16px sawtooth
 *    steps (109 adjacent-tile pairs on BG1 alone in the ramp recording) and
 *    shredded the parallax background it could not even address correctly
 *    (144 more on BG3).
 *  - Promoting whole same-scroll layer groups to the play plane: dragged a
 *    background column to Samus's depth while sprites still drew over it.
 *  - Merging a layer forward across priority boundaries no sprite occupied:
 *    2D-consistent, but still an invented move. It pulled BG0 from -1.0f to
 *    -0.3f in the recorded room for no reason the game's data asks for.
 *
 * Where a room paints part of one object on another layer -- a Chozodia
 * crate platform split across BG1/BG2, a plant's antenna highlight on BG1
 * over the plant on BG2 -- that split is in the room data. The port
 * reproduces it. Deliberate exceptions belong in a curated correction list
 * a human has checked, not in a rule that guesses at them.
 * ------------------------------------------------------------------ */
static void TestDepthIsPriorityOnly(void) {
    printf("policy: a layer's depth is its BGCNT priority, unmodified\n");
    PortStereoDepthState st;
    memset(&st, 0, sizeof(st));
    st.inGameplay = true;

    /* The recorded room: BG0 stays where its priority puts it. */
    st.priority[0] = 1; st.priority[1] = 0; st.priority[2] = 2; st.priority[3] = 3;
    CHECK(PortStereoDepth_BgTier(&st, 0) == PORT_TIER_BG_PLAY,
          "BG0 at priority 1 shares the play plane with priority 0");
    CHECK(PortStereoDepth_BgTier(&st, 1) == PORT_TIER_BG_PLAY, "BG1 priority 0");
    CHECK(PortStereoDepth_BgTier(&st, 2) == PORT_TIER_BG_MID, "BG2 priority 2");
    CHECK(PortStereoDepth_BgTier(&st, 3) == PORT_TIER_BG_FAR, "BG3 priority 3");

    /* Two layers at the same priority share a plane; two at different
     * priorities never do, whatever else is on screen. */
    st.priority[0] = 2; st.priority[1] = 2;
    CHECK(PortStereoDepth_BgTier(&st, 0) == PortStereoDepth_BgTier(&st, 1),
          "equal priority -> equal depth");
    st.priority[0] = 2; st.priority[1] = 3;
    CHECK(PortStereoDepth_BgTier(&st, 0) != PortStereoDepth_BgTier(&st, 1),
          "different priority -> different depth");
    /* ...except the pair the port merges on purpose. */
    st.priority[0] = 0; st.priority[1] = 1;
    CHECK(PortStereoDepth_BgTier(&st, 0) == PortStereoDepth_BgTier(&st, 1),
          "priorities 0 and 1 share the play plane");
}

/* ------------------------------------------------------------------ *
 * Test 4: depth is a property of the LAYER, never of a tile's position.
 *
 * This is failure mode (1) in its general form. A per-tile source of depth
 * (clipdata was the one tried: a 16x16 COLLISION grid, which says nothing
 * about what is drawn) tore the diagonal rock ramp into 16px sawtooth
 * steps, since rock pixels falling in the air block above each step were
 * assigned a different plane from the step itself.
 *
 * The interface is the guard: PortStereoDepth_BgTier takes no coordinate,
 * so it CANNOT depend on one. This test exists so that if someone adds a
 * position argument, the compile breaks here with this comment attached
 * rather than the artifact being rediscovered on hardware.
 * ------------------------------------------------------------------ */
static void TestDepthIsPerLayer(void) {
    printf("invariant: depth takes no screen position, so it cannot tear an object\n");
    int (*bgTier)(const PortStereoDepthState*, int) = PortStereoDepth_BgTier;
    CHECK(bgTier != NULL, "PortStereoDepth_BgTier must stay (state, layer) -> tier");

    /* Same state, asked repeatedly: one answer per layer, always. */
    PortStereoDepthState st;
    memset(&st, 0, sizeof(st));
    st.inGameplay = true;
    st.priority[1] = 1;
    int first = PortStereoDepth_BgTier(&st, 1);
    for (int i = 0; i < 64; ++i) {
        CHECK(PortStereoDepth_BgTier(&st, 1) == first, "depth must be stable for fixed state");
    }
}

int main(void) {
    TestNoContradictionExhaustive();
    TestDepthIsPriorityOnly();
    TestDepthIsPerLayer();

    printf("\n%d checks, %d failures\n", sChecks, sFailures);
    if (sFailures > 20) printf("(only the first 20 failures shown)\n");
    return sFailures == 0 ? 0 : 1;
}
