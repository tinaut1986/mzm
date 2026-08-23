/* Regression lock for the 3DS GPU renderer's stereo depth-tier decisions
 * (platform/3ds/source/depth_tier.c). Table-drives every tier assignment
 * already confirmed correct on real hardware (gameplay HUD elevation,
 * map/pause marker tier, world OBJ tier) plus the cutscene BG0 fix for
 * GitHub issue #4, so future tuning of this heuristic can't silently
 * re-break a case that was already fixed once.
 *
 * Host-buildable, no citro3d/ctrulib/framework dependency -- same shape as
 * port/ppu/tests/mode1_native_fast_path_test.c. Build/run manually:
 *   gcc -I../source depth_tier_test.c ../source/depth_tier.c -o /tmp/depth_tier_test && /tmp/depth_tier_test
 */

#include "depth_tier.h"

#include <stdio.h>

static int sFailures;

static void CheckBg(const char* label, int bgIndex, int priority, int inCutscene, int expected) {
    int actual = DepthTierForBg(bgIndex, priority, inCutscene);
    if (actual != expected) {
        fprintf(stderr, "FAIL %s: DepthTierForBg(bg=%d, prio=%d, cutscene=%d) = %d, expected %d\n", label, bgIndex,
                priority, inCutscene, actual, expected);
        ++sFailures;
    }
}

static void CheckSprite(const char* label, int palBank, int shape, int priority, int inMapOrPauseScreen, int expected) {
    int actual = DepthTierForSprite(palBank, shape, priority, inMapOrPauseScreen);
    if (actual != expected) {
        fprintf(stderr,
                "FAIL %s: DepthTierForSprite(palBank=%d, shape=%d, prio=%d, mapOrPause=%d) = %d, expected %d\n",
                label, palBank, shape, priority, inMapOrPauseScreen, actual, expected);
        ++sFailures;
    }
}

static void CheckSortBg(const char* label, int bgIndex, int priority, int expected) {
    int actual = SortKeyForBg(bgIndex, priority);
    if (actual != expected) {
        fprintf(stderr, "FAIL %s: SortKeyForBg(bg=%d, prio=%d) = %d, expected %d\n", label, bgIndex, priority, actual,
                expected);
        ++sFailures;
    }
}

/* Asserts a full back-to-front draw ORDER for a named real screen (values
 * are raw BGCNT priorities straight from pause_screen.c, see each case's
 * comment) -- checking relative order, not just the tier count, is the
 * actual regression lock for GitHub issue #5 ("layers render in front of
 * the character/HUD"): the underlying values could each individually look
 * plausible while their RELATIVE order is wrong, which is exactly the class
 * of bug #5 was. */
static void CheckDrawOrder(const char* label, const int* keys, int count) {
    for (int i = 1; i < count; ++i) {
        if (keys[i - 1] >= keys[i]) {
            fprintf(stderr, "FAIL %s: sortKey[%d]=%d is not < sortKey[%d]=%d (back-to-front order violated)\n",
                    label, i - 1, keys[i - 1], i, keys[i]);
            ++sFailures;
        }
    }
}

int main(void) {
    /* --- BG tiers, normal gameplay/menus (inCutscene=0): BG0 is ALWAYS the
     * HUD/text layer regardless of its own priority field -- this is the
     * pre-existing, hardware-confirmed behavior and must stay unchanged. */
    CheckBg("gameplay BG0 prio0", 0, 0, 0, 3);
    CheckBg("gameplay BG0 prio1", 0, 1, 0, 3);
    CheckBg("gameplay BG0 prio2", 0, 2, 0, 3);
    CheckBg("gameplay BG0 prio3", 0, 3, 0, 3);
    /* World BGs (BG1-3), any game mode: priority-based tier, untouched by
     * this change since it only special-cases bgIndex==0. */
    CheckBg("world BG1 prio0 (platforms)", 1, 0, 0, 2);
    CheckBg("world BG2 prio1 (platforms)", 2, 1, 0, 2);
    CheckBg("world BG2 prio2 (mid bg)", 2, 2, 0, 1);
    CheckBg("world BG3 prio3 (far bg)", 3, 3, 0, 0);

    /* --- BG0 during cutscenes (GitHub issue #4, Mother Brain close-up):
     * BG0 is NOT the HUD here, falls through to the same priority mapping
     * as world BGs instead of being forced to tier 3. */
    CheckBg("cutscene BG0 prio0 (elevator shaft, foreground)", 0, 0, 1, 2);
    CheckBg("cutscene BG0 prio1", 0, 1, 1, 2);
    CheckBg("cutscene BG0 prio2", 0, 2, 1, 1);
    CheckBg("cutscene BG0 prio3", 0, 3, 1, 0);

    /* --- Sprite tiers, normal gameplay (inMapOrPauseScreen=0). */
    CheckSprite("HUD bar (bank4, wide shape)", 4, 1, 1, 0, 5);
    CheckSprite("minimap icon (bank5)", 5, 0, 1, 0, 5);
    CheckSprite("Morph Ball bomb (bank4, square shape -- NOT HUD)", 4, 0, 1, 0, 4);
    CheckSprite("world OBJ, other bank", 2, 0, 1, 0, 4);
    CheckSprite("world OBJ, priority 0 (explosion/impact flash)", 2, 0, 0, 0, 4);

    /* --- Sprite tiers, map/pause screen (GM_MAP_SCREEN == 5). */
    CheckSprite("map/pause marker, priority 0", 2, 0, 0, 1, 5);
    CheckSprite("map/pause icon, priority 1", 2, 0, 1, 1, 6);
    CheckSprite("map/pause icon, priority 3", 2, 0, 3, 1, 6);

    /* --- Draw-order regression lock for GitHub issue #5, using the exact
     * real BGCNT priorities pause_screen.c writes for each named screen
     * (src/menus/pause_screen.c, PAUSE_SCREEN_DATA setup around line 2745).
     * sPauseScreen_BgCntPriority[] is an identity map (src/data/menus/
     * pause_screen_data.c), so BGCNT_HIGH_PRIORITY=0, _HIGH_MID=1,
     * _LOW_MID=2, _LOW=3 pass straight through as the raw priority field. */

    /* Chozo-statue hint screen (PAUSE_SCREEN_TYPE_CHOZO_STATUE_HINT):
     * BG3=map/platform preview (priority 1, HIGH_MID), BG1=blended grid
     * (priority 2, LOW_MID), BG2=statue art (priority 3, LOW), BG0 unused.
     * Confirmed-correct order (per pause_screen.c's comment, decoded from a
     * real-hardware VRAM dump of this screen): art (BG2) furthest back, grid
     * (BG1) above it, map preview (BG3) on top of both -- lowering BG3
     * (tried once, per that comment) made the art wrongly paint over the
     * map instead. */
    {
        int keys[3] = { SortKeyForBg(2, 3) /* BG2 art */, SortKeyForBg(1, 2) /* BG1 grid */,
                        SortKeyForBg(3, 1) /* BG3 map preview */ };
        CheckDrawOrder("chozo-hint screen: art < grid < map preview", keys, 3);
    }

    /* Map-download screen (PAUSE_SCREEN_TYPE_DOWNLOADING_MAP): BG0=download
     * progress overlay (priority 0, HIGH -- topmost), BG3=map/platform
     * preview (priority 1), BG1=grid (priority 2), BG2=art (priority 3). */
    {
        int keys[4] = { SortKeyForBg(2, 3) /* BG2 art */, SortKeyForBg(1, 2) /* BG1 grid */,
                        SortKeyForBg(3, 1) /* BG3 map preview */, SortKeyForBg(0, 0) /* BG0 download overlay */ };
        CheckDrawOrder("map-download screen: art < grid < map preview < overlay", keys, 4);
    }

    /* In-game HUD (docs/future-roadmap-and-architecture.md's BG0=HUD table):
     * BG0 at its normal gameplay priority (0) must outrank every world BG
     * regardless of their own priority field, including a world BG
     * pathologically set to priority 0 too (equal-priority tiebreak: lower
     * bgIndex draws later/on top, matching real GBA hardware). */
    {
        int keys[4] = { SortKeyForBg(3, 3) /* BG3 far bg */, SortKeyForBg(2, 2) /* BG2 mid bg */,
                        SortKeyForBg(1, 0) /* BG1 platforms, even at prio 0 */,
                        SortKeyForBg(0, 0) /* BG0 HUD, same prio as BG1 above */ };
        CheckDrawOrder("in-game HUD (BG0) always on top, even vs. equal-priority world BG", keys, 4);
    }

    if (sFailures != 0) {
        fprintf(stderr, "depth_tier_test: %d FAILURE(S)\n", sFailures);
        return 1;
    }
    printf("depth_tier_test: all cases PASS\n");
    return 0;
}
