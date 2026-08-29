/*
 * Per-block layer-correction tests. Host-only, no ROM, no 3DS, no GPU.
 *
 * port_layer_fixes.c keys each correction to an ABSOLUTE room-block
 * position. The bug this test pins: an earlier version matched on the
 * screenmap position instead, which wraps every 32 blocks across and 16
 * down, so a correction authored for block (9,43) also fired on (9,59),
 * (9,27), (9,11)... -- moving whichever alias the camera happened to be
 * showing. On hardware that put a Kraid-room-2 wall tile (block (9,59),
 * value 0x00D9) in front of Samus while the entry only named (9,43).
 *
 * These tests compile against the REAL port_layer_fixes.inc, so they also
 * fail loudly if an entry there stops matching the room data below.
 *
 * Build and run:  make -C platform/3ds test
 */

#include "port_layer_fixes.h"

#include <stdio.h>
#include <string.h>

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

/* Kraid sala 2 (area 1, room 2): 19x74 blocks. Only the two cells the
 * regression turns on need real values; the rest stays 0. */
#define ROOM_W 19
#define ROOM_H 74

static uint16_t sBg2[ROOM_W * ROOM_H];

static void BuildRoom(void) {
    memset(sBg2, 0, sizeof(sBg2));
    sBg2[43 * ROOM_W + 9] = 0x0082; /* the block the correction names */
    sBg2[59 * ROOM_W + 9] = 0x00D9; /* its (blockY & 15) alias, must NOT move */
}

/* area 1 / room 2 has corrections only on BG2; BG0/BG1 maps can be NULL. */
static void SelectRoom(void) {
    const uint16_t* data[4] = { NULL, NULL, sBg2, NULL };
    const uint16_t w[4] = { 0, 0, ROOM_W, 0 };
    const uint16_t h[4] = { 0, 0, ROOM_H, 0 };
    PortLayerFix_SetRoom(1, 2, data, w, h);
}

static void TestNamedBlockMoves(void) {
    if (!PortLayerFix_Present()) {
        printf("  SKIP TestNamedBlockMoves: no port_layer_fixes.inc compiled in\n");
        return;
    }
    BuildRoom();
    SelectRoom();
    CHECK(PortLayerFix_ActiveCount() >= 1,
          "expected the (9,43) BG2 correction to be live for area 1 room 2");
    CHECK(PortLayerFix_DestFor(2, 9, 43) == 1,
          "block (9,43) on BG2 should move to BG1, got %d",
          PortLayerFix_DestFor(2, 9, 43));
}

static void TestAliasBlockIsUntouched(void) {
    if (!PortLayerFix_Present()) return;
    BuildRoom();
    SelectRoom();
    /* 59 == 43 + 16, same column: the exact screenmap-wrap collision. */
    CHECK(PortLayerFix_DestFor(2, 9, 59) == -1,
          "block (9,59) is not named by any correction and must be left alone, got %d",
          PortLayerFix_DestFor(2, 9, 59));
    /* A few more lattice aliases of (9,43): 43 & 15 == 11. */
    CHECK(PortLayerFix_DestFor(2, 9, 11) == -1, "alias (9,11) must be left alone");
    CHECK(PortLayerFix_DestFor(2, 9, 27) == -1, "alias (9,27) must be left alone");
    /* Column alias: 9 & 31 == 9, so (9 + 32, 43) used to collide too. */
    CHECK(PortLayerFix_DestFor(2, 41, 43) == -1, "alias (41,43) must be left alone");
}

static void TestChecksumDropsMovedData(void) {
    if (!PortLayerFix_Present()) return;
    memset(sBg2, 0, sizeof(sBg2));
    sBg2[43 * ROOM_W + 9] = 0x0001; /* room data no longer matches the entry */
    SelectRoom();
    CHECK(PortLayerFix_DestFor(2, 9, 43) == -1,
          "a correction whose checksum no longer matches must be dropped, got %d",
          PortLayerFix_DestFor(2, 9, 43));
}

static void TestWrongLayerIsUntouched(void) {
    if (!PortLayerFix_Present()) return;
    BuildRoom();
    SelectRoom();
    CHECK(PortLayerFix_DestFor(0, 9, 43) == -1, "the (9,43) entry is BG2-only, not BG0");
    CHECK(PortLayerFix_DestFor(1, 9, 43) == -1, "the (9,43) entry is BG2-only, not BG1");
}

int main(void) {
    TestNamedBlockMoves();
    TestAliasBlockIsUntouched();
    TestChecksumDropsMovedData();
    TestWrongLayerIsUntouched();

    printf("layer_fixes_test: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
