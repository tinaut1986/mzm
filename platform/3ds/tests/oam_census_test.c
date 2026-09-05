/*
 * OAM census tests. Host-only, no ROM, no 3DS, no GPU.
 *
 * The census feeds the perf/scene recorders' sprite counts, i.e. the numbers
 * a performance investigation reasons from. A wrong census does not crash or
 * look wrong on screen -- it quietly sends the investigation down the wrong
 * road, which is exactly what happened: the original one tested for
 * "disabled" with `attr0 & 0x0300 == 0x0100` (that is affine-single-size,
 * not disabled), so every capture ever taken reported 128 sprites and zero
 * affine ones, and the Skree-affine-sprite hypothesis could not be falsified
 * for weeks.
 *
 * The expectations below are written from GBATEK's OBJ attribute layout
 * directly, not from port_oam_census.c, so agreement between them means
 * something.
 *
 * Build and run:  make -C platform/3ds test
 */

#include "port_oam_census.h"

#include <stdio.h>
#include <string.h>

static int sFailures;
static int sChecks;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        ++sChecks;                                            \
        if (!(cond)) {                                        \
            ++sFailures;                                      \
            if (sFailures <= 20) {                            \
                printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                          \
                printf("\n");                                 \
            }                                                 \
        }                                                     \
    } while (0)

/* ---- OAM construction helpers, straight from GBATEK ---------------- *
 * attr0: bits 0-7 Y, 8 rotation/scaling, 9 double-size (or OBJ disable
 *        when bit 8 is clear), 10-11 OBJ mode, 12 mosaic, 13 colours,
 *        14-15 shape.
 * attr1: bits 0-8 X, 9-13 affine index (or 12 hflip / 13 vflip for a
 *        non-affine OBJ), 14-15 size.
 * ------------------------------------------------------------------- */

enum { OBJ_NORMAL = 0, OBJ_AFFINE = 1, OBJ_DISABLED = 2, OBJ_AFFINE_DOUBLE = 3 };

static uint16_t sOam[128 * 4];

static void OamClear(void) {
    memset(sOam, 0, sizeof(sOam));
    /* An all-zero OAM is 128 visible 8x8 sprites at (0,0), which is a
     * perfectly legal state but a useless baseline. Park every slot the way
     * MZM does -- Y = 0xFF, 8x8 -- so a test only has to set the entries it
     * cares about. An 8x8 at Y=255 spans 255..262, which wraps past 256 and
     * so IS on screen; use 8x8 at Y=200 for genuinely parked instead, which
     * is off the bottom and does not reach the wrap. */
    for (unsigned i = 0; i < 128u; ++i) sOam[i * 4] = 200u;
}

static void OamSet(unsigned slot, unsigned objMode, unsigned shape, unsigned size,
                   unsigned y, unsigned x) {
    sOam[slot * 4 + 0] = (uint16_t)((y & 0xFFu) | ((objMode & 3u) << 8) | ((shape & 3u) << 14));
    sOam[slot * 4 + 1] = (uint16_t)((x & 0x1FFu) | ((size & 3u) << 14));
}

static void Census(unsigned* total, unsigned* visible, unsigned* affine) {
    Port_OamCensus(sOam, total, visible, affine);
}

/* ---- tests --------------------------------------------------------- */

/* The bug that started all of this. */
static void TestDisabledIsObjMode2(void) {
    printf("Disabled detection (the 0x0100 bug)\n");
    unsigned total, visible, affine;

    OamClear();
    OamSet(0, OBJ_DISABLED, 0, 0, 40, 40);
    Census(&total, &visible, &affine);
    CHECK(total == 127, "a disabled slot must not count as present, got total=%u", total);
    CHECK(visible == 0, "a disabled slot cannot be visible, got visible=%u", visible);

    /* Affine-single-size: objMode 1, i.e. attr0 & 0x0300 == 0x0100. The old
     * code treated this as disabled. It is a visible, affine sprite. */
    OamClear();
    OamSet(0, OBJ_AFFINE, 0, 1, 40, 40);
    Census(&total, &visible, &affine);
    CHECK(total == 128, "affine-single-size must count as present, got total=%u", total);
    CHECK(visible == 1, "affine-single-size on screen must be visible, got %u", visible);
    CHECK(affine == 1, "affine-single-size must count as affine, got %u", affine);

    /* Affine double-size: objMode 3, also affine, also visible. */
    OamClear();
    OamSet(0, OBJ_AFFINE_DOUBLE, 0, 1, 40, 40);
    Census(&total, &visible, &affine);
    CHECK(affine == 1, "affine-double-size must count as affine, got %u", affine);
    CHECK(visible == 1, "affine-double-size on screen must be visible, got %u", visible);
}

/* Shape 3 is documented as prohibited; it must not be counted at all. */
static void TestProhibitedShapeIgnored(void) {
    printf("Prohibited shape\n");
    unsigned total, visible, affine;
    OamClear();
    OamSet(0, OBJ_NORMAL, 3, 0, 40, 40);
    Census(&total, &visible, &affine);
    CHECK(total == 127, "shape 3 must not count as present, got total=%u", total);
    CHECK(visible == 0, "shape 3 must not count as visible, got visible=%u", visible);
}

/* The reason `visible` exists: MZM hides sprites by position, not by the
 * disable flag, so `total` stays pinned at 128 while `visible` moves. */
static void TestParkedOffscreenCountsAsPresentButNotVisible(void) {
    printf("Parked-offscreen sprites (MZM's actual hiding method)\n");
    unsigned total, visible, affine;

    OamClear(); /* all 128 parked at Y=200, 8x8 */
    Census(&total, &visible, &affine);
    CHECK(total == 128, "parked slots are still 'present', got total=%u", total);
    CHECK(visible == 0, "parked slots must not be visible, got visible=%u", visible);

    OamSet(5, OBJ_NORMAL, 0, 1, 80, 100); /* one real 16x16 in the middle */
    Census(&total, &visible, &affine);
    CHECK(visible == 1, "exactly one on-screen sprite expected, got %u", visible);
    CHECK(total == 128, "total must not move, got %u", total);
}

/* Y and X wrap (256 / 512). A sprite parked past the bottom edge is only
 * really offscreen if it does not reach round the wrap and back onto the
 * screen -- this is the half most likely to be got wrong, and getting it
 * wrong in the lenient direction inflates `visible` on every frame. */
static void TestCoordinateWrap(void) {
    printf("Coordinate wrap at 256 / 512\n");
    unsigned total, visible, affine;

    /* 8x8 at Y=255 spans 255..262 -> wraps onto lines 0..6: visible. */
    OamClear();
    OamSet(0, OBJ_NORMAL, 0, 0, 255, 100);
    Census(&total, &visible, &affine);
    CHECK(visible == 1, "8x8 at Y=255 wraps onto the screen, expected visible=1 got %u", visible);

    /* 8x8 at Y=200 spans 200..207: below 160, no wrap reached: hidden. */
    OamClear();
    OamSet(0, OBJ_NORMAL, 0, 0, 200, 100);
    Census(&total, &visible, &affine);
    CHECK(visible == 0, "8x8 at Y=200 is off the bottom, expected visible=0 got %u", visible);

    /* 64x64 at Y=200 spans 200..263 -> wraps: visible. Same Y as above, so
     * a census that ignores the sprite's size gets this one wrong. */
    OamClear();
    OamSet(0, OBJ_NORMAL, 0, 3, 200, 100);
    Census(&total, &visible, &affine);
    CHECK(visible == 1, "64x64 at Y=200 wraps onto the screen, expected visible=1 got %u", visible);

    /* Same story horizontally, and this is where the first draft of this
     * test was wrong: the wrap only helps once the sprite actually reaches
     * past 512. 8x8 at X=508 spans 508..515 -> columns 0..3: visible. */
    OamClear();
    OamSet(0, OBJ_NORMAL, 0, 0, 80, 508);
    Census(&total, &visible, &affine);
    CHECK(visible == 1, "8x8 at X=508 wraps onto the screen, expected visible=1 got %u", visible);

    /* 8x8 at X=500 spans 500..507: right of 240, short of the wrap: hidden.
     * (The pair 500/508 is the whole point -- a census that treats "x >= 240"
     * as offscreen unconditionally passes this one and fails the previous
     * case, and one that assumes any large X wraps does the reverse.) */
    OamClear();
    OamSet(0, OBJ_NORMAL, 0, 0, 80, 500);
    Census(&total, &visible, &affine);
    CHECK(visible == 0, "8x8 at X=500 is off the right, expected visible=0 got %u", visible);

    /* 8x8 at X=300 spans 300..307: right of 240, no wrap: hidden. */
    OamClear();
    OamSet(0, OBJ_NORMAL, 0, 0, 80, 300);
    Census(&total, &visible, &affine);
    CHECK(visible == 0, "8x8 at X=300 is off the right, expected visible=0 got %u", visible);
}

/* Double-size doubles the bounding box, so it can pull a sprite that would
 * otherwise be offscreen back onto it. */
static void TestDoubleSizeBounds(void) {
    printf("Affine double-size bounding box\n");
    unsigned total, visible, affine;

    /* 32x32 at Y=200 spans 200..231: hidden. */
    OamClear();
    OamSet(0, OBJ_AFFINE, 0, 2, 200, 100);
    Census(&total, &visible, &affine);
    CHECK(visible == 0, "32x32 affine at Y=200 should be hidden, got visible=%u", visible);

    /* Same sprite double-size: 64x64, spans 200..263 -> wraps: visible. */
    OamClear();
    OamSet(0, OBJ_AFFINE_DOUBLE, 0, 2, 200, 100);
    Census(&total, &visible, &affine);
    CHECK(visible == 1, "double-size 32x32 at Y=200 wraps on, got visible=%u", visible);
}

/* Shape/size table: a vertical 8x32 and a horizontal 32x8 must not be
 * confused, which a transposed table would do silently. */
static void TestShapeSizeTableOrientation(void) {
    printf("Shape/size orientation\n");
    unsigned total, visible, affine;

    /* Shape 2 (vertical), size 1 = 8x32. At Y=140 it spans 140..171, so it
     * pokes onto the bottom of the screen: visible. */
    OamClear();
    OamSet(0, OBJ_NORMAL, 2, 1, 140, 100);
    Census(&total, &visible, &affine);
    CHECK(visible == 1, "8x32 at Y=140 overlaps the screen, got visible=%u", visible);

    /* Shape 1 (horizontal), size 1 = 32x8. At X=225 it spans 225..256, so it
     * pokes onto the right of the screen: visible. */
    OamClear();
    OamSet(0, OBJ_NORMAL, 1, 1, 80, 225);
    Census(&total, &visible, &affine);
    CHECK(visible == 1, "32x8 at X=225 overlaps the screen, got visible=%u", visible);

    /* ... and the same 32x8 at X=245 is past the right edge and does not
     * reach the 512 wrap: hidden. A transposed table would call it 8x32 and
     * still get this right, so the pair above is what separates them. */
    OamClear();
    OamSet(0, OBJ_NORMAL, 1, 1, 80, 245);
    Census(&total, &visible, &affine);
    CHECK(visible == 0, "32x8 at X=245 is off the right, got visible=%u", visible);
}

/* The recorders call this every frame; a NULL OAM must not fault. */
static void TestNullSafe(void) {
    printf("NULL OAM\n");
    unsigned total = 7, visible = 7, affine = 7;
    Port_OamCensus(NULL, &total, &visible, &affine);
    CHECK(total == 0 && visible == 0 && affine == 0,
          "NULL OAM must zero the outputs, got %u/%u/%u", total, visible, affine);
    Port_OamCensus(sOam, NULL, NULL, NULL); /* must not fault */
    ++sChecks;
}

int main(void) {
    TestDisabledIsObjMode2();
    TestProhibitedShapeIgnored();
    TestParkedOffscreenCountsAsPresentButNotVisible();
    TestCoordinateWrap();
    TestDoubleSizeBounds();
    TestShapeSizeTableOrientation();
    TestNullSafe();

    printf("\n%d checks, %d failures\n", sChecks, sFailures);
    if (sFailures > 20) printf("(only the first 20 failures shown)\n");
    return sFailures == 0 ? 0 : 1;
}
