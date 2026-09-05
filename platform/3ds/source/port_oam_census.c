#include "port_oam_census.h"

/* GBATEK OBJ dimensions, indexed [shape][size]. */
static const unsigned char kObjW[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const unsigned char kObjH[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

void Port_OamCensus(const uint16_t* oam, unsigned* outTotal, unsigned* outVisible,
                    unsigned* outAffine) {
    unsigned total = 0, visible = 0, affine = 0;
    if (!oam) {
        if (outTotal) *outTotal = 0;
        if (outVisible) *outVisible = 0;
        if (outAffine) *outAffine = 0;
        return;
    }

    for (unsigned i = 0; i < 128u; ++i) {
        const uint16_t attr0 = oam[i * 4];
        const uint16_t attr1 = oam[i * 4 + 1];
        const unsigned objMode = (attr0 >> 8) & 3u;
        if (objMode == 2u) continue;            /* disabled */
        const unsigned shape = (attr0 >> 14) & 3u;
        if (shape == 3u) continue;              /* prohibited shape */
        ++total;
        if (objMode == 1u || objMode == 3u) ++affine;

        const unsigned size = (attr1 >> 14) & 3u;
        unsigned w = kObjW[shape][size];
        unsigned h = kObjH[shape][size];
        if (objMode == 3u) { w *= 2u; h *= 2u; } /* affine double-size */

        /* The GBA wraps these, so "off the bottom" and "off the right" are
         * only true when the sprite does not reach round the wrap point and
         * back onto the screen. */
        const unsigned y = attr0 & 0x00FFu;     /* wraps at 256 */
        const unsigned x = attr1 & 0x01FFu;     /* wraps at 512 */
        if (y >= 160u && (y + h) <= 256u) continue;
        if (x >= 240u && (x + w) <= 512u) continue;
        ++visible;
    }

    if (outTotal) *outTotal = total;
    if (outVisible) *outVisible = visible;
    if (outAffine) *outAffine = affine;
}
