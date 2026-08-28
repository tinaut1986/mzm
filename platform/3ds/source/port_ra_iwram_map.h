#ifndef PORT_RA_IWRAM_MAP_H
#define PORT_RA_IWRAM_MAP_H

/* Where each GBA IWRAM address lives in this build.
 *
 * RetroAchievements addresses GBA memory as IWRAM at 0x0000-0x7fff followed
 * by EWRAM from 0x8000.  EWRAM needs no table -- ewram_symbols.ld puts the
 * decomp's EWRAM globals at their real offsets inside gEwram -- but IWRAM
 * globals are ordinary C variables the compiler places wherever it likes, so
 * an IWRAM address only means something via this map.
 *
 * The table is generated and committed; see tools/gen_ra_iwram_map.py for
 * how the original layout is reconstructed and how it is checked against
 * RetroAchievements' published code notes.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned short address; /* GBA IWRAM offset, i.e. GBA 0x03000000 + this */
    unsigned short size;    /* bytes owned here; entries never overlap */
    void* storage;          /* where those bytes actually live in this build */
} PortRaIwramEntry;

/* Sorted by address, gap-free within each variable but not between them. */
extern const PortRaIwramEntry gPortRaIwramMap[];
extern const unsigned int gPortRaIwramMapCount;

#ifdef __cplusplus
}
#endif

#endif /* PORT_RA_IWRAM_MAP_H */
