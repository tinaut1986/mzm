#ifndef PORT_OAM_CENSUS_H
#define PORT_OAM_CENSUS_H

#include <stdint.h>

/* Counts what a frame's OAM actually contains, for the perf/scene recorders
 * (issue #20). Its own translation unit so the host test target can feed it
 * synthetic OAM -- see platform/3ds/tests/oam_census_test.c. Two traps live
 * here, both of which cost a round of misleading captures:
 *
 *  - "Disabled" is objMode == 2 (attr0 bits 9-8 = 10): rotation flag clear,
 *    double-size/disable flag set. It is NOT attr0 & 0x0300 == 0x0100, which
 *    is affine-single-size -- a perfectly visible sprite. Testing for that
 *    skipped every affine sprite and counted every disabled slot, which is
 *    why every sample recorded before 2026-09-04 reads spriteCount=128,
 *    affineSpriteCount=0.
 *
 *  - MZM does not use the disable flag at all: it parks unused slots
 *    offscreen at Y=0xFF. So even a correct non-disabled count is 128 in
 *    practically every frame of every room and says nothing about scene
 *    load. `visible` is the number that correlates with cost, and getting it
 *    right needs the size decoded from shape/size plus the GBA's 256/512
 *    coordinate wrap (a sprite at Y=250 IS on screen if it is tall enough
 *    to wrap past 256).
 *
 * `oam` is the emulated OAM block (gOamMem): 128 entries of four u16, of
 * which the first two are attr0/attr1. Any output pointer may be NULL.
 */
void Port_OamCensus(const uint16_t* oam, unsigned* outTotal, unsigned* outVisible,
                    unsigned* outAffine);

#endif /* PORT_OAM_CENSUS_H */
