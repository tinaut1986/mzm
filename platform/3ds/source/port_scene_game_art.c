#include "port_scene_pictures.h"

#include <string.h>

/* The game's own pictures for the WIDE scene pictures (PortScenePic_FillGameArt), read from
 * the game's memory when the picture is loaded: nothing of the game's art is shipped. Only
 * scenes whose VRAM never holds the whole picture at once need one. */

extern uint8_t gVram[];
extern uint8_t* sEwramPointer; /* union EwramData* (include/data/shortcut_pointers.h) */
/* union NonGameplayRam* (the same header); its first byte is the intro's stage
 * (struct IntroData, include/structs/intro.h), 4 = the view of Zebes. */
extern uint8_t* sNonGameplayRamPointer;

enum { GM_INTRO = 1 }; /* include/constants/game_state.h */

int PortScenePic_SceneKey(int gameMode) {
    if (gameMode == GM_INTRO && sNonGameplayRamPointer) return sNonGameplayRamPointer[0];
    return 0;
}

/* One 4bpp text-BG tile's pixel into a code (port_scene_pictures_data.c's codes: the BG
 * palette entry, 16 for the backdrop where the tile is transparent). */
static uint8_t TileCode(const uint8_t* tile, uint16_t entry, int px, int py) {
    if (entry & 0x400) px = 7 - px;
    if (entry & 0x800) py = 7 - py;
    const uint8_t b = tile[py * 4 + px / 2];
    const int v = (px & 1) ? (b >> 4) : (b & 15);
    return v ? (uint8_t)(((entry >> 12) << 4) | v) : 16u;
}

static uint32_t Swizzle(int x, int y, int texWidth) {
    const uint32_t tile = (uint32_t)((y >> 3) * (texWidth >> 3) + (x >> 3));
    return tile * 64u + (uint32_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
                                   ((x & 4) << 2) | ((y & 4) << 3));
}

/* The title (src/menus/title_screen.c): BG3, 256x512, map at VRAM 0x0000, tiles at 0x4000.
 * Its 240x360 picture is a pan from deep space to the ground. Tiles 0..0x19F are two sets
 * that take turns in VRAM while it scrolls: the space one (map rows 0-15) is kept at
 * sEwramPointer + 0x0000 and the ground one (rows 32 and down) at + 0x4000, and the pan
 * copies 1 KB of the right one into VRAM ahead of the frame. The tiles past those stay in
 * VRAM, as the title loaded them (this runs on its first frame, before anything else lands
 * there). The map itself does not change. */
static bool TitleArt(const PortScenePictureDef* def, uint8_t* codes) {
    enum { STREAMED_TILES = 0x1A0, MAP = 0x0000, CHARS = 0x4000 };
    if (!sEwramPointer) return false;
    for (int by = def->pictureY0; by < def->pictureY1; ++by) {
        const int ty = by >> 3;
        for (int bx = def->pictureX0; bx < def->pictureX1; ++bx) {
            const int tx = bx >> 3;
            const uint32_t at = MAP + (uint32_t)(ty >> 5) * 0x800u + (uint32_t)(((ty & 31) * 32 + tx) * 2);
            const uint16_t entry = (uint16_t)(gVram[at] | (gVram[at + 1] << 8));
            const uint32_t t = entry & 0x3FFu;
            const uint8_t* tile;
            if (t < STREAMED_TILES)
                tile = sEwramPointer + (ty < 32 ? 0x0000u : 0x4000u) + t * 32u;
            else
                tile = gVram + CHARS + t * 32u;
            codes[Swizzle(bx - def->canvasX, by - def->canvasY, def->texWidth)] =
                TileCode(tile, entry, bx & 7, by & 7);
        }
    }
    return true;
}

bool PortScenePic_FillGameArt(const PortScenePictureDef* def, uint8_t* codes) {
    if (strcmp(def->name, "title") == 0) return TitleArt(def, codes);
    return false;
}
