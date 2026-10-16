#include "port_scene_sprites.h"

#include "port_scene_pictures.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_FRAMES = 4 };

extern bool Port_Config_GetWideAi(void);
extern uint8_t gVram[];

/* One frame's texture, and its texels by code (code 0, transparent, left out):
 * texels[first[c] .. first[c + 1]), so a palette change repaints only the colours it moved. */
typedef struct SpriteFrame {
    C3D_Tex tex;
    bool ready;
    uint32_t* texels;
    uint32_t first[257];
    uint16_t lut[256]; /* code -> RGBA5551, as painted last */
    bool lutValid;
} SpriteFrame;

/* The one sprite loaded (a scene shows one at a time). */
static const PortSceneSpriteDef* sDef;
static SpriteFrame sFrames[MAX_FRAMES];
static bool sFailed; /* sDef could not be loaded: not retried until the scene changes */

/* OBJ sizes by shape (square, wide, tall) and size bits, GBATEK. */
static const uint8_t kObjW[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t kObjH[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

const PortSceneSpriteDef* PortSceneSprite_Find(int gameMode) {
    if (!Port_Config_GetWideAi()) return NULL;
    int key = -1;
    for (int i = 0; i < gPortSceneSpriteCount; ++i) {
        const PortSceneSpriteDef* def = &gPortSceneSprites[i];
        if (def->gameMode != gameMode) continue;
        if (key < 0) key = PortScenePic_SceneKey(gameMode);
        if (def->sceneKey == key) return def;
    }
    return NULL;
}

/* A piece of the game's OAM data and an OAM entry are the same piece: tile, palette, shape
 * and size (the position, priority and mode are the game's to set). */
static bool SamePiece(const uint16_t* piece, const uint16_t* entry) {
    return (piece[2] & 0xF3FFu) == (entry[2] & 0xF3FFu) && (piece[0] >> 14) == (entry[0] >> 14) &&
           (piece[1] >> 14) == (entry[1] >> 14);
}

bool PortSceneSprite_Match(const PortSceneSpriteDef* def, const uint16_t* oam, bool hide[128], int* frame,
                           int* anchorX, int* anchorY, int* firstSlot) {
    for (int f = 0; f < def->frameCount && f < MAX_FRAMES; ++f) {
        const uint16_t* data = PortSceneSprite_GameOam(def, f);
        if (!data || data[0] == 0) continue;
        const uint16_t* first = data + 1;
        int slot = -1;
        for (int i = 0; i < 128 && slot < 0; ++i) {
            const uint16_t* e = oam + i * 4;
            if (((e[0] >> 8) & 3u) == 2u) continue; /* hidden */
            if (SamePiece(first, e)) slot = i;
        }
        if (slot < 0) continue;
        /* The game writes each piece at its offset plus the anchor (8-bit y, 9-bit x). */
        const uint16_t* e = oam + slot * 4;
        *anchorY = (int)((e[0] - first[0]) & 0xFFu);
        *anchorX = (int)((e[1] - first[1]) & 0x1FFu);
        *frame = f;
        *firstSlot = slot;
        for (int i = 0; i < 128; ++i) {
            const uint16_t* o = oam + i * 4;
            if (((o[0] >> 8) & 3u) == 2u) continue;
            for (int p = 0; p < data[0]; ++p) {
                if (SamePiece(data + 1 + p * 3, o)) {
                    hide[i] = true;
                    break;
                }
            }
        }
        return true;
    }
    return false;
}

static uint32_t Swizzle(int x, int y, int texWidth) {
    const uint32_t tile = (uint32_t)((y >> 3) * (texWidth >> 3) + (x >> 3));
    return tile * 64u + (uint32_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
                                   ((x & 4) << 2) | ((y & 4) << 3));
}

/* The game's pieces of `data` into `codes` (OBJ palette entries, texture order), from OBJ VRAM.
 * 4bpp pieces only, which is all the scene sprites use. */
static void DrawGameArt(const PortSceneSpriteDef* def, const uint16_t* data, bool obj1D, uint8_t* codes) {
    for (int p = 0; p < data[0]; ++p) {
        const uint16_t* a = data + 1 + p * 3;
        const int shape = a[0] >> 14, size = a[1] >> 14;
        if (shape == 3 || (a[0] & 0x2000u)) continue;
        const int w = kObjW[shape][size], h = kObjH[shape][size];
        const bool hflip = (a[1] & 0x1000u) != 0, vflip = (a[1] & 0x2000u) != 0;
        const int tile = a[2] & 0x3FF, bank = a[2] >> 12;
        /* Offsets are signed: 8-bit y, 9-bit x. */
        const int ox = ((a[1] & 0x1FF) ^ 0x100) - 0x100 - def->canvasDX;
        const int oy = (int)(int8_t)(a[0] & 0xFF) - def->canvasDY;
        for (int yy = 0; yy < h; ++yy) {
            const int ty = oy + yy;
            if (ty < 0 || ty >= def->height) continue;
            const int sy = vflip ? h - 1 - yy : yy;
            for (int xx = 0; xx < w; ++xx) {
                const int tx = ox + xx;
                if (tx < 0 || tx >= def->width) continue;
                const int sx = hflip ? w - 1 - xx : xx;
                const int t = obj1D ? tile + (sy >> 3) * (w >> 3) + (sx >> 3) : tile + (sy >> 3) * 32 + (sx >> 3);
                const uint8_t b = gVram[0x10000u + (uint32_t)(t & 0x3FF) * 32u + (uint32_t)((sy & 7) * 4 + (sx & 7) / 2)];
                const int v = (sx & 1) ? (b >> 4) : (b & 15);
                if (v) codes[Swizzle(tx, ty, def->texWidth)] = (uint8_t)(bank * 16 + v);
            }
        }
    }
}

void PortSceneSprite_Release(void) {
    for (int f = 0; f < MAX_FRAMES; ++f) {
        SpriteFrame* fr = &sFrames[f];
        if (fr->ready) C3D_TexDelete(&fr->tex);
        free(fr->texels);
        fr->texels = NULL;
        fr->ready = false;
        fr->lutValid = false;
    }
    sDef = NULL;
    sFailed = false;
}

static bool LoadFrame(const PortSceneSpriteDef* def, int f, bool obj1D) {
    SpriteFrame* fr = &sFrames[f];
    const size_t n = (size_t)def->texWidth * (size_t)def->texHeight;
    const uint16_t* data = PortSceneSprite_GameOam(def, f);
    uint8_t* codes = (uint8_t*)malloc(n);
    if (!codes || !data) {
        free(codes);
        return false;
    }
    if (!PortScenePic_Lz4Decompress(def->packed[f], def->packedSize[f], codes, n) ||
        !C3D_TexInit(&fr->tex, (u16)def->texWidth, (u16)def->texHeight, GPU_RGBA5551)) {
        free(codes);
        return false;
    }
    /* The generated parts are where the game has nothing: its own pieces go over them. */
    DrawGameArt(def, data, obj1D, codes);
    uint32_t count[256] = { 0 };
    for (size_t i = 0; i < n; ++i) ++count[codes[i]];
    fr->first[0] = fr->first[1] = 0;
    for (int c = 1; c < 256; ++c) fr->first[c + 1] = fr->first[c] + count[c];
    fr->texels = (uint32_t*)malloc((fr->first[256] ? fr->first[256] : 1) * sizeof(uint32_t));
    if (!fr->texels) {
        C3D_TexDelete(&fr->tex);
        free(codes);
        return false;
    }
    uint32_t fill[256];
    memcpy(fill, fr->first, sizeof(fill));
    for (size_t i = 0; i < n; ++i)
        if (codes[i]) fr->texels[fill[codes[i]]++] = (uint32_t)i;
    free(codes);
    memset(fr->tex.data, 0, n * sizeof(uint16_t));
    C3D_TexSetFilter(&fr->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&fr->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    fr->ready = true;
    fr->lutValid = false;
    return true;
}

C3D_Tex* PortSceneSprite_Texture(const PortSceneSpriteDef* def, int frame, const uint16_t* objPalette, bool obj1D) {
    if (!def || frame < 0 || frame >= def->frameCount || frame >= MAX_FRAMES) return NULL;
    if (sDef != def) {
        PortSceneSprite_Release();
        sDef = def;
        /* Every frame at once: the game loaded all of their tiles with the scene, and one
         * may be gone from VRAM by the time a later frame first shows. */
        for (int f = 0; f < def->frameCount && f < MAX_FRAMES && !sFailed; ++f)
            if (!LoadFrame(def, f, obj1D)) sFailed = true;
    }
    if (sFailed) return NULL;
    SpriteFrame* fr = &sFrames[frame];

    /* Same texel as the atlas (RGBA5551). Code 0 and entry 0 of a bank are transparent. */
    uint16_t lut[256];
    for (int i = 0; i < 256; ++i) {
        const uint16_t c = objPalette[i];
        lut[i] = (i & 15) ? (uint16_t)(((c & 31u) << 11) | (((c >> 5) & 31u) << 6) | (((c >> 10) & 31u) << 1) | 1u)
                          : 0;
    }
    uint16_t* texels = (uint16_t*)fr->tex.data;
    bool changed = false;
    for (int c = 1; c < 256; ++c) {
        if (fr->lutValid && lut[c] == fr->lut[c]) continue;
        const uint16_t v = lut[c];
        for (uint32_t k = fr->first[c]; k < fr->first[c + 1]; ++k) texels[fr->texels[k]] = v;
        changed = changed || fr->first[c + 1] > fr->first[c];
    }
    memcpy(fr->lut, lut, sizeof(lut));
    fr->lutValid = true;
    if (changed)
        GSPGPU_FlushDataCache(fr->tex.data, (size_t)def->texWidth * (size_t)def->texHeight * sizeof(uint16_t));
    return &fr->tex;
}
