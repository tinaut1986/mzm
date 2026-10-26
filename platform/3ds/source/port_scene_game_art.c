#include "port_scene_pictures.h"
#include "port_scene_sprites.h"

#include <string.h>

/* The game's own pictures for the WIDE scene pictures (PortScenePic_FillGameArt), read from
 * the game's memory when the picture is loaded: nothing of the game's art is shipped. Only
 * scenes whose VRAM never holds the whole picture at once need one. */

extern uint8_t gVram[];
extern uint8_t* sEwramPointer; /* union EwramData* (include/data/shortcut_pointers.h) */
/* union NonGameplayRam* (the same header); its first byte is the intro's stage
 * (struct IntroData, include/structs/intro.h), 4 = the view of Zebes. */
extern uint8_t* sNonGameplayRamPointer;

extern int16_t gSubGameMode1; /* include/structs/game_state.h */

enum { GM_INTRO = 1, GM_TITLE = 2 }; /* include/constants/game_state.h */

/* No picture matches this key. */
enum { SCENE_NOT_READY = -2 };

int PortScenePic_SceneKey(int gameMode) {
    if (gameMode == GM_INTRO && sNonGameplayRamPointer) return sNonGameplayRamPointer[0];
    /* The title's picture is read from the game's memory once, when it is
     * loaded (TitleArt), and TitleScreenInit is what puts it there. On the
     * frame the game switches to the title (coming back from a demo, say) that
     * has not run yet (sub mode 0): memory and VRAM are still the previous
     * scene's, and the picture would be built from them and kept. */
    if (gameMode == GM_TITLE && gSubGameMode1 == 0) return SCENE_NOT_READY;
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

/* The intro's fuzz (src/intro.c IntroFuzz, stage 7): 17 OBJ parts that cover the GBA screen,
 * the noise being their palette changing every few frames. The texture is that picture
 * (decoded from the game's OAM and OBJ tiles when the stage starts) repeated over a 400x240
 * canvas; the GBA frame's own 240x160 is left transparent, the game draws it. Codes are OBJ
 * palette entries (PortScenePic_UsesObjPalette). */
enum { FUZZ_STAGE = 7 };
static const PortScenePictureDef sFuzzDef = {
    "intro-fuzz", GM_INTRO, FUZZ_STAGE, 0, -80, -40, 400, 240, 0, 0, 240, 160, 512, 256, NULL, 0,
};

extern uint8_t gOamData[];
extern uint8_t gIoMem[];

const PortScenePictureDef* PortScenePic_FindFuzz(int gameMode) {
    return gameMode == GM_INTRO && PortScenePic_SceneKey(GM_INTRO) == FUZZ_STAGE ? &sFuzzDef : NULL;
}

bool PortScenePic_UsesObjPalette(const PortScenePictureDef* def) { return def == &sFuzzDef; }

static bool FuzzArt(const PortScenePictureDef* def, uint8_t* codes) {
    static const uint8_t kDims[3][4][2] = {
        { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
        { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
        { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
    };
    const bool oneD = (gIoMem[0] & 0x40) != 0;
    /* Static: 38 KB is too much for the present thread's stack. */
    static uint8_t screen[160][240];
    memset(screen, 0, sizeof(screen));
    int parts = 0;
    for (int i = 0; i < 128; ++i) {
        const uint16_t* o = (const uint16_t*)(gOamData + i * 8);
        const unsigned a0 = o[0], a1 = o[1], a2 = o[2];
        if ((a0 & 0x300) == 0x200 || (a0 & 0x300) == 0x300 || (a0 & 0x2000)) continue; /* hidden, affine, 8bpp */
        const unsigned shape = a0 >> 14;
        if (shape > 2) continue;
        const int w = kDims[shape][a1 >> 14][0], h = kDims[shape][a1 >> 14][1];
        const int x0 = (int)(a1 & 0x1FF) >= 256 ? (int)(a1 & 0x1FF) - 512 : (int)(a1 & 0x1FF);
        const int y0 = (int)(a0 & 0xFF) >= 160 ? (int)(a0 & 0xFF) - 256 : (int)(a0 & 0xFF);
        const unsigned tile = a2 & 0x3FF, bank = a2 >> 12;
        ++parts;
        for (int ty = 0; ty < h / 8; ++ty)
            for (int tx = 0; tx < w / 8; ++tx) {
                const unsigned t = (tile + (oneD ? (unsigned)(ty * (w / 8) + tx) : (unsigned)(ty * 32 + tx))) & 0x3FF;
                const uint8_t* data = gVram + 0x10000u + t * 32u;
                for (int py = 0; py < 8; ++py)
                    for (int px = 0; px < 8; ++px) {
                        const int sx = x0 + tx * 8 + px, sy = y0 + ty * 8 + py;
                        if (sx < 0 || sx >= 240 || sy < 0 || sy >= 160) continue;
                        const uint8_t b = data[py * 4 + px / 2];
                        const int v = (px & 1) ? (b >> 4) : (b & 15);
                        if (v) screen[sy][sx] = (uint8_t)((bank << 4) | v);
                    }
            }
    }
    if (parts < 8) return false; /* the stage has not drawn its noise yet */
    const int cx = -def->canvasX, cy = -def->canvasY; /* where the GBA frame sits in the canvas */
    for (int y = 0; y < def->height; ++y)
        for (int x = 0; x < def->width; ++x) {
            if (x >= cx && x < cx + 240 && y >= cy && y < cy + 160) continue;
            const int sx = ((x - cx) % 240 + 240) % 240, sy = ((y - cy) % 160 + 160) % 160;
            codes[Swizzle(x, y, def->texWidth)] = screen[sy][sx];
        }
    return true;
}

bool PortScenePic_HasGameArtSource(const PortScenePictureDef* def) {
    return strcmp(def->name, "title") == 0 || def == &sFuzzDef;
}

bool PortScenePic_FillGameArt(const PortScenePictureDef* def, uint8_t* codes) {
    if (strcmp(def->name, "title") == 0) return TitleArt(def, codes);
    if (def == &sFuzzDef) return FuzzArt(def, codes);
    return false;
}

/* The intro's heat trail (src/data/intro_data.c, drawn by IntroViewOfZebesProcessOam): frame A
 * is sIntroViewOfZebesHeatOam_1, B sIntroViewOfZebesHeatOam_2 (make_sprite_package.py). */
extern const uint16_t (*p_sIntroViewOfZebesHeatOam_1)[73]; /* port/generated/intro_data_rom.h */
extern const uint16_t (*p_sIntroViewOfZebesHeatOam_2)[37];

const uint16_t* PortSceneSprite_GameOam(const PortSceneSpriteDef* def, int frame) {
    if (strcmp(def->name, "intro-zebes-flame") == 0)
        return frame == 0 && p_sIntroViewOfZebesHeatOam_1   ? *p_sIntroViewOfZebesHeatOam_1
               : frame == 1 && p_sIntroViewOfZebesHeatOam_2 ? *p_sIntroViewOfZebesHeatOam_2
                                                            : NULL;
    return NULL;
}
