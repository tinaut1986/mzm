#include "port_layer_fixes.h"

#include <stddef.h>

/* The list is optional: a stock build simply has no port_layer_fixes.inc and
 * every lookup here compiles down to "no". */
#if defined(__has_include)
#  if __has_include("port_layer_fixes.inc")
#    define PORT_HAVE_LAYER_FIXES 1
#  endif
#endif

typedef struct {
    uint8_t area;
    uint8_t room;
    uint8_t fromBg;
    uint8_t dest;
    uint16_t blockX;
    uint16_t blockY;
    uint16_t block;
} PortLayerFixEntry;

#ifdef PORT_HAVE_LAYER_FIXES
#define PORT_LAYER_FIX(area, room, fromBg, bx, by, blk, dest) \
    { (uint8_t)(area), (uint8_t)(room), (uint8_t)(fromBg), (uint8_t)(dest), \
      (uint16_t)(bx), (uint16_t)(by), (uint16_t)(blk) },
static const PortLayerFixEntry sAll[] = {
#include "port_layer_fixes.inc"
};
#undef PORT_LAYER_FIX
#define PORT_FIX_COUNT ((int)(sizeof(sAll) / sizeof(sAll[0])))
#else
static const PortLayerFixEntry sAll[1];
#define PORT_FIX_COUNT 0
#endif

/* Corrections live for the current room. Bounded so a bad list can never make
 * the render loop unbounded; a room with more than this many is not something
 * hand-authoring produces. */
#define PORT_FIX_MAX_ACTIVE 64

typedef struct {
    uint8_t bg;
    uint8_t dest;
    uint8_t col;   /* blockX wrapped to the screenmap period */
    uint8_t row;   /* blockY wrapped */
} ActiveFix;

static ActiveFix sActive[PORT_FIX_MAX_ACTIVE];
static int sActiveCount;

bool PortLayerFix_Present(void) { return PORT_FIX_COUNT > 0; }
int PortLayerFix_ActiveCount(void) { return sActiveCount; }

int PortLayerFix_SetRoom(int area, int room,
                         const uint16_t* const* bgData,
                         const uint16_t* bgW, const uint16_t* bgH) {
    sActiveCount = 0;
    if (PORT_FIX_COUNT == 0) return 0;

    for (int i = 0; i < PORT_FIX_COUNT && sActiveCount < PORT_FIX_MAX_ACTIVE; ++i) {
        const PortLayerFixEntry* f = &sAll[i];
        if (f->area != (uint8_t)area || f->room != (uint8_t)room) continue;
        if (f->fromBg > 3 || f->dest > 4) continue;

        /* Checksum against the room's own data. A correction is a human
         * pointing at one block; if the data moved under it, dropping it is
         * the only honest option -- applying it would move a different block
         * than the one that was looked at. */
        if (bgData != NULL) {
            const uint16_t* map = bgData[f->fromBg];
            if (map == NULL) continue;
            uint16_t w = bgW[f->fromBg], h = bgH[f->fromBg];
            if (f->blockX >= w || f->blockY >= h) continue;
            if (map[(uint32_t)f->blockY * w + f->blockX] != f->block) continue;
        }

        sActive[sActiveCount].bg = f->fromBg;
        sActive[sActiveCount].dest = f->dest;
        sActive[sActiveCount].col = (uint8_t)(f->blockX & 31u);
        sActive[sActiveCount].row = (uint8_t)(f->blockY & 15u);
        sActiveCount++;
    }
    return sActiveCount;
}

int PortLayerFix_DestFor(int bg, int colBlock, int rowBlock) {
    if (sActiveCount == 0) return -1;
    uint8_t c = (uint8_t)(colBlock & 31), r = (uint8_t)(rowBlock & 15);
    for (int i = 0; i < sActiveCount; ++i) {
        if (sActive[i].bg == (uint8_t)bg && sActive[i].col == c && sActive[i].row == r)
            return (int)sActive[i].dest;
    }
    return -1;
}
