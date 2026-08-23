#include "depth_tier.h"

int DepthTierForBg(int bgIndex, int priority, int inCutscene) {
    if (bgIndex == 0 && !inCutscene) {
        return 3; /* Topmost foreground (HUD/text/dialogs) */
    }
    /* World BGs (and BG0 during cutscenes, where it's not the HUD --
     * see this header's comment on inCutscene): map priority to tier.
     * priority 0/1 -> tier 2 (platforms / foreground world, -0.3f)
     * priority 2   -> tier 1 (mid background, -2.0f)
     * priority 3   -> tier 0 (far background / sky, -4.0f) */
    if (priority <= 1) return 2;
    if (priority == 2) return 1;
    return 0;
}

int DepthTierForSprite(int palBank, int shape, int priority, int inMapOrPauseScreen) {
    if (inMapOrPauseScreen) {
        return (priority == 0) ? 5 : 6;
    }
    int isRealHud = (palBank == 4 && shape == 1) || palBank == 5;
    return isRealHud ? 5 : 4;
}

int SortKeyForBg(int bgIndex, int priority) {
    return (3 - priority) * 10 + (3 - bgIndex);
}

int SortKeyForSprite(int priority) {
    return (3 - priority) * 10 + 4;
}
