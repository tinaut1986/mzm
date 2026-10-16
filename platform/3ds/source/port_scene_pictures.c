#include "port_scene_pictures.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

/* The one picture loaded (a scene shows one at a time). */
static const PortScenePictureDef* sDef;
static uint8_t* sCodes;   /* palette entries, texture order */
static C3D_Tex sTex;
static bool sTexReady;
static uint16_t sLut[256]; /* code -> RGBA5551, as painted last */
static bool sLutValid;
static bool sGameArt;
static bool sWithAi; /* the loaded picture has its generated parts */

extern bool Port_Config_GetWideAi(void);
/* The texels of each code (code 0, transparent, left out): sTexels[sFirst[c]
 * .. sFirst[c + 1]), so a palette change repaints only the colours it moved
 * (the title animates part of its palette every frame). */
static uint32_t* sTexels;
static uint32_t sFirst[257];

bool PortScenePic_HasGameArt(void) { return sDef != NULL && sGameArt; }

const PortScenePictureDef* PortScenePic_Find(int gameMode, int bg) {
    int key = -1;
    for (int i = 0; i < gPortScenePictureCount; ++i) {
        const PortScenePictureDef* def = &gPortScenePictures[i];
        if (def->gameMode != gameMode || def->bg != bg) continue;
        if (key < 0) key = PortScenePic_SceneKey(gameMode);
        if (def->sceneKey != key) continue;
        if (!Port_Config_GetWideAi() && !PortScenePic_HasGameArtSource(def)) return NULL;
        return def;
    }
    return NULL;
}

/* Standard LZ4 block, as port_gba_bezel.c unpacks the bezel. */
bool PortScenePic_Lz4Decompress(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    size_t s = 0, d = 0;
    while (d < dstLen && s < srcLen) {
        const uint8_t token = src[s++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do { if (s >= srcLen) return false; b = src[s++]; lit += b; } while (b == 255);
        }
        if (s + lit > srcLen || d + lit > dstLen) return false;
        memcpy(dst + d, src + s, lit);
        d += lit;
        s += lit;
        if (d >= dstLen) break;
        if (s + 2 > srcLen) return false;
        const size_t offset = (size_t)src[s] | ((size_t)src[s + 1] << 8);
        s += 2;
        if (offset == 0 || offset > d) return false;
        size_t match = (token & 15u) + 4;
        if (match == 19) {
            uint8_t b;
            do { if (s >= srcLen) return false; b = src[s++]; match += b; } while (b == 255);
        }
        if (d + match > dstLen) return false;
        for (size_t i = 0; i < match; ++i, ++d) dst[d] = dst[d - offset];
    }
    return d == dstLen;
}

void PortScenePic_Release(void) {
    if (sTexReady) C3D_TexDelete(&sTex);
    free(sCodes);
    sCodes = NULL;
    free(sTexels);
    sTexels = NULL;
    sTexReady = false;
    sLutValid = false;
    sGameArt = false;
    sDef = NULL;
}

static bool Load(const PortScenePictureDef* def, bool withAi) {
    PortScenePic_Release();
    const size_t n = (size_t)def->texWidth * (size_t)def->texHeight;
    sCodes = (uint8_t*)malloc(n);
    if (!sCodes) return false;
    if (!PortScenePic_Lz4Decompress(def->packed, def->packedSize, sCodes, n) ||
        !C3D_TexInit(&sTex, (u16)def->texWidth, (u16)def->texHeight, GPU_RGBA5551)) {
        free(sCodes);
        sCodes = NULL;
        return false;
    }
    /* Plain WIDE: none of the generated parts, only the game's own art below. */
    if (!withAi) memset(sCodes, 0, n);
    sWithAi = withAi;
    sGameArt = PortScenePic_FillGameArt(def, sCodes);
    uint32_t count[256] = { 0 };
    for (size_t i = 0; i < n; ++i) ++count[sCodes[i]];
    sFirst[0] = sFirst[1] = 0;
    for (int c = 1; c < 256; ++c) sFirst[c + 1] = sFirst[c] + count[c];
    sTexels = (uint32_t*)malloc((sFirst[256] ? sFirst[256] : 1) * sizeof(uint32_t));
    if (!sTexels) {
        C3D_TexDelete(&sTex);
        free(sCodes);
        sCodes = NULL;
        return false;
    }
    uint32_t fill[256];
    memcpy(fill, sFirst, sizeof(fill));
    for (size_t i = 0; i < n; ++i)
        if (sCodes[i]) sTexels[fill[sCodes[i]]++] = (uint32_t)i;
    /* Nothing of the codes is needed past this: transparent texels stay 0. */
    memset(sTex.data, 0, n * sizeof(uint16_t));
    free(sCodes);
    sCodes = NULL;
    /* 1:1 or the scaled style's 1.5x: NEAREST keeps the pixels hard, like the tiles. */
    C3D_TexSetFilter(&sTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sTexReady = true;
    sDef = def;
    return true;
}

C3D_Tex* PortScenePic_Texture(const PortScenePictureDef* def, const uint16_t* bgPalette) {
    if (!def) return NULL;
    const bool withAi = Port_Config_GetWideAi();
    if ((sDef != def || sWithAi != withAi) && !Load(def, withAi)) return NULL;

    /* Same texel as the atlas (RGBA5551). Code 0 is transparent; N % 16 == 0 the backdrop. */
    uint16_t lut[256];
    lut[0] = 0;
    for (int i = 1; i < 256; ++i) {
        const uint16_t c = bgPalette[(i & 15) ? i : 0];
        lut[i] = (uint16_t)(((c & 31u) << 11) | (((c >> 5) & 31u) << 6) | (((c >> 10) & 31u) << 1) | 1u);
    }
    uint16_t* texels = (uint16_t*)sTex.data;
    bool changed = false;
    for (int c = 1; c < 256; ++c) {
        if (sLutValid && lut[c] == sLut[c]) continue;
        const uint16_t v = lut[c];
        for (uint32_t k = sFirst[c]; k < sFirst[c + 1]; ++k) texels[sTexels[k]] = v;
        changed = changed || sFirst[c + 1] > sFirst[c];
    }
    memcpy(sLut, lut, sizeof(lut));
    sLutValid = true;
    if (changed)
        GSPGPU_FlushDataCache(sTex.data, (size_t)def->texWidth * (size_t)def->texHeight * sizeof(uint16_t));
    return &sTex;
}
