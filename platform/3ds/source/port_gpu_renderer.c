/*
 * Native GPU (PICA200) tile/sprite compositor for GBA mode 0 (all-tiled,
 * no affine BG). Replaces the CPU-side per-pixel scanline renderer
 * (port/ppu/src/mode1.c, still the correctness-verified fallback) for the
 * common case, to free the ARM11 for game logic/audio instead of spending
 * ~18-20ms/frame decoding+compositing pixels in software (see the PERF log
 * instrumentation in port_ppu_mzm.c that measured this).
 *
 * Deliberately narrow scope for this first cut -- see
 * Port_GpuRenderer_CanRenderFrame() for the exact eligibility gate. Anything
 * outside it (affine BG2, alpha blending, windows, mosaic, affine OBJ) falls
 * back to the CPU renderer for that frame rather than drawing it wrong.
 * Correctness over coverage: a frame that silently falls back is fine, a
 * frame that renders with the wrong palette bank or wrong sprite size is not.
 */
#include "port_gpu_renderer.h"
#include "platform_gpu_3ds.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdlib.h>
#include <string.h>

#ifdef PORT_GPU_RENDERER_DIAG_LOG
#include <stdio.h>
extern void Port_DebugLog(const char* msg);
#endif

/* Emulated GBA memory buffers (port/port_gba_mem.c). */
extern uint8_t gIoMem[];
extern uint8_t gVram[];
extern uint8_t gBgPltt[];
extern uint8_t gObjPltt[];
extern uint8_t gOamMem[];

static bool sGpuRendererActive = false;
static bool sInitialized = false;

/* Same 3-stage RGB reconstruction as PlatformGpu3DS_ConfigureAbgrTextureEnv
 * (the PICA200 samples a GPU_RGBA8 texture with components in the reverse
 * of memory byte order -- see that function's doc comment in
 * platform_gpu_3ds.h), but that shared version hardcodes output alpha to
 * opaque (255), correct for its own use case (compositing one always-opaque
 * full-screen background quad) but wrong here: tile/sprite quads need real
 * per-pixel transparency (index-0 palette entries) so background layers and
 * other sprites show through. Without this, every "transparent" pixel drew
 * as an opaque black square -- a real contributor to the black-patch/
 * corruption reports, not just the RGB channel-order bug. Stage 0's alpha
 * output is sourced from the texture's sampled-R component here instead of
 * a constant, which -- per the same reversed-component mapping -- is this
 * project's stored alpha byte (Bgr555ToRgba8's memory byte 3). */
static void ConfigureAtlasTextureEnv(void) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_R);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, C2D_Color32(255, 0, 0, 255));

    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_B, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MULTIPLY_ADD);
    C3D_TexEnvColor(env, C2D_Color32(0, 255, 0, 255));

    env = C3D_GetTexEnv(2);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MULTIPLY_ADD);
    C3D_TexEnvColor(env, C2D_Color32(0, 0, 255, 255));
}

/* Tile atlas: every unique (source tile bytes + palette bank + hflip/vflip)
 * combination actually visible this frame gets decoded into one 8x8 slot,
 * shared by BG and OBJ (they live in disjoint gVram ranges in mode 0, so a
 * byte-offset-based key can't collide between them, but isObj is still part
 * of the key for clarity/robustness rather than relying on that). 64x64
 * slots of 8x8 = 4096 -- comfortably above a worst-case frame (4 BGs at
 * ~33x21 visible tiles each, worst case ~2772 tiles, realistically far
 * fewer unique combos due to tile reuse). */
enum {
    ATLAS_DIM = 512,
    ATLAS_TILES_PER_ROW = ATLAS_DIM / 8,
    ATLAS_MAX_SLOTS = ATLAS_TILES_PER_ROW * ATLAS_TILES_PER_ROW,
    MAX_DRAW_ITEMS = 3200,
};

/* BLDCNT effect applied at decode time for brighten/darken (effect 1, alpha
 * blend, is applied later at draw time via GPU blending instead -- see
 * kBlendAlpha in DrawItem). NONE/BRIGHTEN/DARKEN affect the palette-to-RGBA
 * conversion itself, so they're part of what makes a decoded tile unique. */
typedef enum { BRIGHT_ADJUST_NONE, BRIGHT_ADJUST_BRIGHTEN, BRIGHT_ADJUST_DARKEN } BrightAdjust;

typedef struct TileCacheKey {
    uint32_t byteOffset; /* offset of the tile's first byte within gVram */
    uint8_t bpp8;
    uint8_t palBank; /* 0 for 8bpp (single 256-color palette, no banking) */
    uint8_t hflip;
    uint8_t vflip;
    uint8_t isObj;
    uint8_t brightAdjust; /* BrightAdjust -- evy itself is one value for the
                           * whole frame, doesn't need to be part of the key */
} TileCacheKey;

typedef struct DrawItem {
    C2D_Image img;
    float x, y, w, h;
    int sortKey;   /* ascending draw order: lower = drawn first (further back) */
    int8_t depthTier; /* stereo parallax tier, see kTierEyeOffsetScale */
    bool blendAlpha; /* BLDCNT effect 1 (alpha blend) active for this item's
                      * layer -- drawn in a second pass with GPU blending
                      * against whatever's already in the framebuffer,
                      * approximating GBA's 1st-target/2nd-target blend
                      * (see Port_GpuRenderer_RenderFrame). */
} DrawItem;

static C3D_Tex sAtlasTexture;
static u32* sAtlasPixels;
static TileCacheKey sCacheKeys[ATLAS_MAX_SLOTS];
static int sCacheCount;
static DrawItem sDrawItems[MAX_DRAW_ITEMS];
static int sDrawItemCount;
static bool sAnyDirtySlot;
static int sLastObjItemCount;

/* GBA sprite shape/size -> pixel dimensions (attr0 bits14-15 = shape,
 * attr1 bits14-15 = size). Same table as port/ppu/src/mode1.c's
 * mode1_obj_widths/heights -- duplicated here rather than shared because
 * that file's tables are static/internal to the CPU renderer and this
 * module intentionally doesn't reach into it (independent, swappable
 * renderer backends behind the same PPU memory). */
static const uint8_t kObjWidths[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t kObjHeights[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

/* Stereo depth mapping (docs/future-roadmap-and-architecture.md's table):
 * index 0=BG3(farthest) .. 3=BG0/HUD(nearest), 4=OBJ. Values are px of
 * parallax shift at full slider; sign flips between eyes in the caller. */
static const float kTierEyeOffsetPx[5] = {
    -3.5f, /* BG3: far background */
    -1.75f, /* BG2: mid background */
    -0.4f, /* BG1: platforms/gameplay plane */
    0.0f,  /* BG0: HUD/foreground, screen plane */
    -0.4f, /* OBJ: same plane as gameplay by default */
};

bool Port_GpuRenderer_IsActive(void) { return sGpuRendererActive; }
void Port_GpuRenderer_SetActive(bool active) { sGpuRendererActive = active; }

bool Port_GpuRenderer_Init(void) {
    if (sInitialized) return true;

    sAtlasPixels = (u32*)linearMemAlign(ATLAS_DIM * ATLAS_DIM * sizeof(u32), 0x80);
    if (!sAtlasPixels) return false;
    memset(sAtlasPixels, 0, ATLAS_DIM * ATLAS_DIM * sizeof(u32));

    if (!C3D_TexInitVRAM(&sAtlasTexture, ATLAS_DIM, ATLAS_DIM, GPU_RGBA8)) {
        linearFree(sAtlasPixels);
        sAtlasPixels = NULL;
        return false;
    }
    C3D_TexSetFilter(&sAtlasTexture, GPU_NEAREST, GPU_NEAREST);

    sInitialized = true;
    return true;
}

void Port_GpuRenderer_Shutdown(void) {
    if (!sInitialized) return;
    C3D_TexDelete(&sAtlasTexture);
    if (sAtlasPixels) linearFree(sAtlasPixels);
    sAtlasPixels = NULL;
    sInitialized = false;
    sGpuRendererActive = false;
}

static inline u32 Bgr555ToRgba8(uint16_t color, bool transparent) {
    if (transparent) return 0u;
    u32 r = (color & 0x1Fu);
    u32 g = ((color >> 5) & 0x1Fu);
    u32 b = ((color >> 10) & 0x1Fu);
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    /* Byte order r,g,b,a in memory -- matches GPU_RGBA8's expected layout
     * directly, no texenv swizzle hack needed at draw time (unlike the
     * CPU-composited buffer's ABGR quirk in platform_gpu_3ds.c). */
    return (255u << 24) | (b << 16) | (g << 8) | r;
}

/* GBA brightness increase/decrease on 5-bit channels, byte-for-byte the same
 * formula as port/ppu/src/mode1.c's mode1_brighten/mode1_darken (GBATEK:
 * I = I +- I(or 31-I)*evy/16, truncating; evy pre-clamped 0..16). Operates
 * directly on our packed R,G,B,A-in-memory u32 (same convention mode1.c
 * calls "ABGR8888" -- see Bgr555ToRgba8's comment), so the bit shifts match
 * theirs exactly (byte0=R at shift 0, byte1=G at shift 8, byte2=B at shift
 * 16). */
static inline u32 ApplyBrighten(u32 color, int evy) {
    if ((color >> 24) == 0u) return color; /* transparent stays transparent */
    int r = (int)(color & 0xFFu) >> 3;
    int g = (int)((color >> 8) & 0xFFu) >> 3;
    int b = (int)((color >> 16) & 0xFFu) >> 3;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    return (255u << 24) | ((u32)(b << 3) << 16) | ((u32)(g << 3) << 8) | (u32)(r << 3);
}

static inline u32 ApplyDarken(u32 color, int evy) {
    if ((color >> 24) == 0u) return color;
    int r = (int)(color & 0xFFu) >> 3;
    int g = (int)((color >> 8) & 0xFFu) >> 3;
    int b = (int)((color >> 16) & 0xFFu) >> 3;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return (255u << 24) | ((u32)(b << 3) << 16) | ((u32)(g << 3) << 8) | (u32)(r << 3);
}

/* Frame-global BLDCNT/BLDALPHA/BLDY state, computed once per frame in
 * Port_GpuRenderer_RenderFrame and consumed by CollectBgLayer/CollectSprite
 * (to decide whether their layer needs brighten/darken at decode time or
 * gets flagged for the alpha-blend second pass) and by the draw loop (EVA
 * for the GPU blend constant). */
static uint8_t sBldEffect;     /* 0=none, 1=alpha blend, 2=brighten, 3=darken */
static uint16_t sIoBldcnt;
static int sBldEva, sBldEvb, sBldEvy;

static inline bool BldIsFirstTarget(uint16_t bldcnt, int layerId) { return ((bldcnt >> layerId) & 1u) != 0u; }

/* Decodes one 8x8 tile (4bpp or 8bpp, GBA-packed) from `src` into the atlas
 * pixel buffer at `slot`, applying `pal`/`palBank`, flips, and (when this
 * tile's layer is a BLDCNT first-target and the active effect is
 * brighten/darken) the brightness adjustment. Marks the slot's bytes dirty
 * for the single flush-and-upload done once per frame in
 * Port_GpuRenderer_RenderFrame -- decoding into a CPU buffer we still have
 * to flush is unavoidable (that's the actual pixel data), but doing it once
 * per unique tile per frame instead of once per BG-layer-per-eye (the
 * previous version's bug) is the whole point. */
static void DecodeTileIntoSlot(int slot, const uint8_t* src, bool bpp8, const uint16_t* pal, int palBank,
                               bool hflip, bool vflip, BrightAdjust brightAdjust) {
    int slotX = (slot % ATLAS_TILES_PER_ROW) * 8;
    int slotY = (slot / ATLAS_TILES_PER_ROW) * 8;

    for (int row = 0; row < 8; ++row) {
        int srcRow = vflip ? (7 - row) : row;
        u32* dst = sAtlasPixels + (size_t)(slotY + row) * ATLAS_DIM + slotX;
        if (!bpp8) {
            const uint8_t* rowSrc = src + srcRow * 4;
            for (int col = 0; col < 8; ++col) {
                int srcCol = hflip ? (7 - col) : col;
                uint8_t byte = rowSrc[srcCol / 2];
                uint8_t idx = (srcCol & 1) ? (byte >> 4) & 0x0Fu : byte & 0x0Fu;
                dst[col] = Bgr555ToRgba8(pal[palBank * 16 + idx], idx == 0);
            }
        } else {
            const uint8_t* rowSrc = src + srcRow * 8;
            for (int col = 0; col < 8; ++col) {
                int srcCol = hflip ? (7 - col) : col;
                uint8_t idx = rowSrc[srcCol];
                dst[col] = Bgr555ToRgba8(pal[idx], idx == 0);
            }
        }
        if (brightAdjust == BRIGHT_ADJUST_BRIGHTEN) {
            for (int col = 0; col < 8; ++col) dst[col] = ApplyBrighten(dst[col], sBldEvy);
        } else if (brightAdjust == BRIGHT_ADJUST_DARKEN) {
            for (int col = 0; col < 8; ++col) dst[col] = ApplyDarken(dst[col], sBldEvy);
        }
    }
    sAnyDirtySlot = true;
}

/* Returns the atlas slot for this tile, decoding it first if this exact
 * (offset, bpp, palette bank, flip, brightness adjustment) combination
 * hasn't been seen yet this frame. sCacheKeys/sCacheCount are reset once per
 * frame by the caller. */
static int GetOrDecodeTileSlot(uint32_t byteOffset, bool bpp8, const uint16_t* pal, int palBank, bool hflip,
                               bool vflip, bool isObj, BrightAdjust brightAdjust) {
    TileCacheKey key = {
        byteOffset,       (uint8_t)bpp8,   (uint8_t)(bpp8 ? 0 : palBank), (uint8_t)hflip,
        (uint8_t)vflip,   (uint8_t)isObj,  (uint8_t)brightAdjust,
    };
    for (int i = 0; i < sCacheCount; ++i) {
        const TileCacheKey* k = &sCacheKeys[i];
        if (k->byteOffset == key.byteOffset && k->bpp8 == key.bpp8 && k->palBank == key.palBank &&
            k->hflip == key.hflip && k->vflip == key.vflip && k->isObj == key.isObj &&
            k->brightAdjust == key.brightAdjust) {
            return i;
        }
    }
    if (sCacheCount >= ATLAS_MAX_SLOTS) {
        /* Cache exhausted (pathological frame with far more unique tiles
         * than the atlas holds) -- reuse slot 0 rather than overrun. Wrong
         * pixels for the overflowing tiles only, not a crash; extremely
         * unlikely given ATLAS_MAX_SLOTS=4096 vs. a realistic frame's few
         * hundred unique tiles. */
        return 0;
    }
    int slot = sCacheCount++;
    sCacheKeys[slot] = key;
    DecodeTileIntoSlot(slot, gVram + byteOffset, bpp8, pal, palBank, hflip, vflip, brightAdjust);
    return slot;
}

static inline void SlotToUV(int slot, Tex3DS_SubTexture* out) {
    int sx = (slot % ATLAS_TILES_PER_ROW) * 8;
    int sy = (slot / ATLAS_TILES_PER_ROW) * 8;
    out->width = 8;
    out->height = 8;
    out->left = (float)sx / (float)ATLAS_DIM;
    out->top = 1.0f - (float)sy / (float)ATLAS_DIM;
    out->right = (float)(sx + 8) / (float)ATLAS_DIM;
    out->bottom = 1.0f - (float)(sy + 8) / (float)ATLAS_DIM;
}

static inline void PushItem(int slot, float x, float y, int sortKey, int depthTier, bool blendAlpha) {
    if (sDrawItemCount >= MAX_DRAW_ITEMS) return;
    Tex3DS_SubTexture subtex;
    SlotToUV(slot, &subtex);
    DrawItem* item = &sDrawItems[sDrawItemCount++];
    /* subtex is stack-local per call site in the original C2D_Image pattern;
     * store it by value inside the persisted image via a static per-item
     * copy so it stays valid until the draw pass. C2D_Image only holds a
     * pointer, so give each item its own backing subtex. */
    static Tex3DS_SubTexture sSubtexPool[MAX_DRAW_ITEMS];
    sSubtexPool[sDrawItemCount - 1] = subtex;
    item->img.tex = &sAtlasTexture;
    item->img.subtex = &sSubtexPool[sDrawItemCount - 1];
    item->x = x;
    item->y = y;
    item->w = 8.0f;
    item->h = 8.0f;
    item->sortKey = sortKey;
    item->depthTier = (int8_t)depthTier;
    item->blendAlpha = blendAlpha;
}

/* Text-mode BG tilemap addressing, byte-identical to the formula validated
 * in port/ppu/src/mode1.c (screen_block_x/y + blocks_per_row quadrant
 * layout for the 32x32/64x32/32x64/64x64 GBA screen sizes). */
static void CollectBgLayer(int bgIndex) {
    uint16_t bgcnt = (uint16_t)(gIoMem[0x08 + bgIndex * 2] | (gIoMem[0x09 + bgIndex * 2] << 8));
    uint8_t priority = (uint8_t)(bgcnt & 3u);
    uint32_t charBase = (uint32_t)((bgcnt >> 2) & 3u) * 0x4000u;
    bool bpp8 = ((bgcnt >> 7) & 1u) != 0;
    uint32_t screenBase = (uint32_t)((bgcnt >> 8) & 0x1Fu) * 0x800u;
    uint16_t sizeFlag = (uint16_t)((bgcnt >> 14) & 3u);
    int mapWidthTiles = (sizeFlag & 1u) ? 64 : 32;
    int mapHeightTiles = (sizeFlag & 2u) ? 64 : 32;
    int blocksPerRow = mapWidthTiles / 32;

    int hofsAddr = 0x10 + bgIndex * 4;
    int vofsAddr = 0x12 + bgIndex * 4;
    int scrollX = (int)((uint16_t)(gIoMem[hofsAddr] | (gIoMem[hofsAddr + 1] << 8)) & 0x1FFu);
    int scrollY = (int)((uint16_t)(gIoMem[vofsAddr] | (gIoMem[vofsAddr + 1] << 8)) & 0x1FFu);

    const uint16_t* pal = (const uint16_t*)gBgPltt;
    const int bytesPerTile = bpp8 ? 64 : 32;

    /* BLDCNT: is this BG a first-target layer for the active effect? Only
     * matters when an effect is active at all (sBldEffect != 0). */
    bool isFirstTarget = sBldEffect != 0 && BldIsFirstTarget(sIoBldcnt, bgIndex);
    BrightAdjust brightAdjust = BRIGHT_ADJUST_NONE;
    if (isFirstTarget && sBldEffect == 2) brightAdjust = BRIGHT_ADJUST_BRIGHTEN;
    else if (isFirstTarget && sBldEffect == 3) brightAdjust = BRIGHT_ADJUST_DARKEN;
    bool blendAlpha = isFirstTarget && sBldEffect == 1;

    /* 240x160 screen -> 30x20 visible tiles, +1 in each axis to cover the
     * partial tile at the scroll fraction on both edges. */
    int startTileX = scrollX / 8;
    int startTileY = scrollY / 8;
    int fineX = scrollX % 8;
    int fineY = scrollY % 8;

    for (int ty = 0; ty <= 20; ++ty) {
        int tileRow = (startTileY + ty) & (mapHeightTiles - 1);
        int screenBlockY = tileRow / 32;
        int localRow = tileRow % 32;
        for (int tx = 0; tx <= 30; ++tx) {
            int tileCol = (startTileX + tx) & (mapWidthTiles - 1);
            int screenBlockX = tileCol / 32;
            int localCol = tileCol % 32;
            int screenBlockIndex = screenBlockX + screenBlockY * blocksPerRow;
            uint32_t mapAddr = screenBase + (uint32_t)screenBlockIndex * 0x800u + (uint32_t)(localRow * 32 + localCol) * 2u;
            uint16_t entry = (uint16_t)(gVram[mapAddr] | (gVram[mapAddr + 1] << 8));
            uint16_t tileId = entry & 0x3FFu;
            bool hflip = (entry & 0x0400u) != 0;
            bool vflip = (entry & 0x0800u) != 0;
            int palBank = (entry >> 12) & 0x0Fu;

            uint32_t byteOffset = charBase + (uint32_t)tileId * bytesPerTile;
            int slot = GetOrDecodeTileSlot(byteOffset, bpp8, pal, palBank, hflip, vflip, false, brightAdjust);

            float drawX = (float)(tx * 8 - fineX);
            float drawY = (float)(ty * 8 - fineY);
            if (drawY <= -8.0f || drawY >= 160.0f || drawX <= -8.0f || drawX >= 240.0f) continue;

            /* GBA BGCNT priority is 0=highest (drawn on top), 3=lowest
             * (drawn furthest back) -- the inverse of sortKey's own
             * ascending-draws-first-i.e.-furthest-back convention, so it
             * must be inverted here. Getting this backwards (using
             * `priority` directly) meant a BG with priority 3 -- meant to
             * be the backmost layer -- was instead drawn last/on top,
             * painting over every other BG and every OBJ; confirmed via
             * GPUDIAG logs during real gameplay where the priority-3 BG
             * layer was the only thing visible on screen. Same-priority
             * tiebreak: lower BG index draws later (on top), matching GBA
             * hardware (BG0 > BG1 > BG2 > BG3 at equal priority). */
            int sortKey = (3 - priority) * 10 + (3 - bgIndex);
            /* depthTier indexes kTierEyeOffsetPx, which is laid out
             * BG3,BG2,BG1,BG0,OBJ (far to near) -- the inverse of bgIndex
             * (BG0..BG3). Passing bgIndex directly here meant BG0 (meant
             * to get 0.0f/no stereo offset, being the HUD/foreground
             * plane) was instead reading kTierEyeOffsetPx[0] == -3.5f (the
             * far-background BG3 offset), and BG3 read index 3 == 0.0f --
             * silently swapping stereo depth between the nearest and
             * farthest layers. */
            PushItem(slot, drawX, drawY, sortKey, 3 - bgIndex, blendAlpha);
        }
    }
}

/* Multi-tile OBJ blit: walks the sprite's width/height in 8x8 tiles and
 * pushes one atlas quad per subtile, honoring 1D/2D OBJ char mapping
 * (DISPCNT bit 6) and whole-sprite hflip/vflip (subtile position AND
 * texture flip both mirror, matching hardware). Affine OBJs (attr0 bit8)
 * are rejected earlier by Port_GpuRenderer_CanRenderFrame -- not handled
 * here. */
static void CollectSprite(int oamIndex, bool obj1D) {
    const uint16_t* oam = (const uint16_t*)gOamMem;
    uint16_t attr0 = oam[oamIndex * 4 + 0];
    uint16_t attr1 = oam[oamIndex * 4 + 1];
    uint16_t attr2 = oam[oamIndex * 4 + 2];

    if (((attr0 >> 9) & 1u) && !((attr0 >> 8) & 1u)) return; /* disabled (non-affine hidden bit) */
    uint8_t objMode = (uint8_t)((attr0 >> 10) & 3u);
    if (objMode == 2) return; /* OBJ window: not a drawable sprite */

    uint8_t shape = (uint8_t)((attr0 >> 14) & 3u);
    uint8_t size = (uint8_t)((attr1 >> 14) & 3u);
    if (shape == 3) return; /* prohibited shape value */
    int width = kObjWidths[shape][size];
    int height = kObjHeights[shape][size];

    int y = attr0 & 0xFFu;
    if (y >= 160) y -= 256;
    int x = (int)(attr1 & 0x1FFu);
    if (x >= 240) x -= 512;

    bool bpp8 = ((attr0 >> 13) & 1u) != 0;
    bool hflip = ((attr1 >> 12) & 1u) != 0;
    bool vflip = ((attr1 >> 13) & 1u) != 0;
    uint16_t baseTile = attr2 & 0x3FFu;
    uint8_t priority = (uint8_t)((attr2 >> 10) & 3u);
    uint8_t palBank = (uint8_t)((attr2 >> 12) & 0x0Fu);

    if (y >= 160 || y + height <= 0 || x >= 240 || x + width <= 0) return;

    const uint8_t* objBase = gVram + 0x10000u;
    const uint16_t* pal = (const uint16_t*)gObjPltt;
    const int bytesPerTile = bpp8 ? 64 : 32;
    int tilesW = width / 8;
    int tilesH = height / 8;

    /* BLDCNT layer id 4 = OBJ. */
    bool isFirstTarget = sBldEffect != 0 && BldIsFirstTarget(sIoBldcnt, 4);
    BrightAdjust brightAdjust = BRIGHT_ADJUST_NONE;
    if (isFirstTarget && sBldEffect == 2) brightAdjust = BRIGHT_ADJUST_BRIGHTEN;
    else if (isFirstTarget && sBldEffect == 3) brightAdjust = BRIGHT_ADJUST_DARKEN;
    bool blendAlpha = isFirstTarget && sBldEffect == 1;

    for (int ty = 0; ty < tilesH; ++ty) {
        for (int tx = 0; tx < tilesW; ++tx) {
            int srcTx = hflip ? (tilesW - 1 - tx) : tx;
            int srcTy = vflip ? (tilesH - 1 - ty) : ty;
            uint16_t tileIndex;
            if (obj1D) {
                tileIndex = (uint16_t)(baseTile + (srcTy * tilesW + srcTx) * (bpp8 ? 2 : 1));
            } else {
                /* 2D OBJ character mapping: the VRAM grid's row stride is
                 * always 32 char-slots of 0x20 bytes regardless of color
                 * depth (confirmed against port/ppu/src/mode1.c's own OBJ
                 * rendering, e.g. line ~2431: tile_row * 32 + tile_col, *2
                 * for 8bpp) -- only the column step doubles for 8bpp, since
                 * each 8bpp tile spans two horizontally-adjacent 4bpp-sized
                 * slots. Previously halved the row step to 16 for 8bpp,
                 * which is wrong and corrupted every row past the first of
                 * any multi-row 8bpp sprite (e.g. the Samus head portrait on
                 * the file-select screen). */
                tileIndex = (uint16_t)(baseTile + srcTy * 32 + srcTx * (bpp8 ? 2 : 1));
            }
            uint32_t byteOffset = (uint32_t)(objBase - gVram) + (uint32_t)tileIndex * bytesPerTile;
            int slot = GetOrDecodeTileSlot(byteOffset, bpp8, pal, palBank, hflip, vflip, true, brightAdjust);

            float drawX = (float)(x + tx * 8);
            float drawY = (float)(y + ty * 8);
            /* Priority inverted for the same reason as CollectBgLayer above
             * (0=highest/on top, 3=lowest/backmost). OBJ draws above any BG
             * of equal priority (tiebreak=4, higher than any BG's 0-3), and
             * lower OAM index draws above higher index at equal priority --
             * callers iterate OAM back-to-front (127..0) so a stable sort
             * already preserves that via insertion order. */
            int sortKey = (3 - priority) * 10 + 4;
            PushItem(slot, drawX, drawY, sortKey, 4, blendAlpha);
        }
    }
}

static int CompareDrawItems(const void* a, const void* b) {
    const DrawItem* ia = (const DrawItem*)a;
    const DrawItem* ib = (const DrawItem*)b;
    return ia->sortKey - ib->sortKey;
}

/* Frames outside this scope fall back to the CPU renderer (port/ppu) for
 * that frame rather than drawing them wrong: forced blank (DISPCNT bit7 --
 * real hardware shows a blank white screen and skips all BG/OBJ rendering
 * entirely while this is set; a common trick during scene transitions/loads
 * to hide VRAM being rewritten -- ignoring it meant briefly rendering
 * whatever half-updated VRAM content existed at the exact moment a
 * transition hit, which lines up with "the image was cut/corrupted right as
 * a scene changed" from testing), affine BG (GBA mode != 0, the Tourian
 * self-destruct sequence per docs/3ds-port-ppu-audit.md), any window that
 * actually clips the screen (a full-screen no-op WIN0/WIN1 -- the common
 * case in ordinary gameplay, see the WindowCoversFullScreen check below --
 * is allowed through; OBJ window is not approximated at all), BG or OBJ
 * mosaic, or any affine OBJ (attr0 bit8 set, rare -- rotated items/enemies).
 * BLDCNT (alpha blend/brighten/darken) is NOT excluded here anymore --
 * transparency.c sets it routinely for ordinary rooms (water overlays,
 * layering), not just rare fades, so rejecting it meant real gameplay almost
 * never used this renderer at all. See Port_GpuRenderer_RenderFrame for the
 * approximation (brighten/darken applied at tile-decode time, alpha blend
 * via a second GPU-blended draw pass). */
#ifdef PORT_GPU_RENDERER_DIAG_LOG
/* Throttled to avoid flooding mzm-debug.log at 60Hz -- one line every 30
 * *rejected* frames is enough to see the pattern during gameplay without
 * drowning the log. */
static void LogRejectReason(const char* reason) {
    static unsigned sRejectCounter;
    if ((sRejectCounter++ % 30u) == 0u) {
        char buf[96];
        snprintf(buf, sizeof(buf), "GPU_REJECT: %s", reason);
        Port_DebugLog(buf);
    }
}
#define REJECT(reason) do { LogRejectReason(reason); return false; } while (0)
#else
#define REJECT(reason) return false
#endif

/* WIN0H/WIN1H pack left in the high byte, right in the low byte (GBATEK);
 * WIN0V/WIN1V pack top high, bottom low. A window that spans the full
 * 240x160 screen clips nothing -- it is only being used as the vehicle for
 * WININ's per-layer enable bits, not for actual rectangle clipping. */
static bool WindowCoversFullScreen(uint16_t h, uint16_t v) {
    return (h == 0x00F0u /* left=0 right=240 */) && (v == 0x00A0u /* top=0 bottom=160 */);
}

bool Port_GpuRenderer_CanRenderFrame(void) {
    uint16_t dispcnt = (uint16_t)(gIoMem[0] | (gIoMem[1] << 8));
    if (dispcnt & (1u << 7)) REJECT("forced blank"); /* forced blank */
    if ((dispcnt & 7u) != 0u) REJECT("mode != 0"); /* not GBA mode 0 */

    /* src/transparency.c's TransparencySetRoomEffectsTransparency() enables
     * WIN1 unconditionally for essentially every normal room, but sizes it
     * to the full screen (WIN1H=SCREEN_SIZE_X, WIN1V=SCREEN_SIZE_Y) and sets
     * WININ_H to 0x3F (every BG/OBJ/effect layer enabled inside it) -- it is
     * using the window purely as GBA's mechanism for gating BLDCNT special
     * effects per layer, not to clip any region of the screen. Rejecting
     * every frame with DCNT_WIN1 set meant gameplay (which is in this state
     * almost all the time -- confirmed via GPU_REJECT diagnostics logged
     * during real play) never reached the GPU renderer at all. Real
     * clipping use (e.g. gSuitFlashEffect's shrunk WIN1 rect during the suit
     * flash) still shrinks the rect below full-screen, so it still falls
     * back correctly below. WININ != 0x3F while covering the full screen
     * would mean a layer is being selectively disabled -- still rejected,
     * conservatively, since that path isn't approximated. */
    uint16_t win1h = (uint16_t)(gIoMem[0x42] | (gIoMem[0x43] << 8));
    uint16_t win1v = (uint16_t)(gIoMem[0x46] | (gIoMem[0x47] << 8));
    uint16_t winin = (uint16_t)(gIoMem[0x48] | (gIoMem[0x49] << 8));
    if (dispcnt & (1u << 13)) { /* WIN0 */
        uint16_t win0h = (uint16_t)(gIoMem[0x40] | (gIoMem[0x41] << 8));
        uint16_t win0v = (uint16_t)(gIoMem[0x44] | (gIoMem[0x45] << 8));
        if (!WindowCoversFullScreen(win0h, win0v) || (winin & 0x3Fu) != 0x3Fu) REJECT("WIN0");
    }
    if (dispcnt & (1u << 14)) { /* WIN1 */
        if (!WindowCoversFullScreen(win1h, win1v) || ((winin >> 8) & 0x3Fu) != 0x3Fu) REJECT("WIN1");
    }
    if (dispcnt & (1u << 15)) REJECT("OBJWIN");

    uint16_t mosaic = (uint16_t)(gIoMem[0x4C] | (gIoMem[0x4D] << 8));
    if (mosaic != 0) {
        for (int bg = 0; bg < 4; ++bg) {
            uint16_t bgcnt = (uint16_t)(gIoMem[0x08 + bg * 2] | (gIoMem[0x09 + bg * 2] << 8));
            if ((dispcnt & (1u << (8 + bg))) && ((bgcnt >> 6) & 1u)) REJECT("mosaic BG");
        }
    }
    if (dispcnt & (1u << 12)) { /* OBJ layer enabled: reject if any affine OBJ is live */
        const uint16_t* oam = (const uint16_t*)gOamMem;
        for (int i = 0; i < 128; ++i) {
            uint16_t attr0 = oam[i * 4 + 0];
            if ((attr0 >> 8) & 1u) REJECT("affine OBJ"); /* affine flag set */
        }
    }
    return true;
}
#undef REJECT

/* Renders the current GBA frame's tiles/sprites into both stereo top
 * targets (or just the left one when the 3D slider is off). Must run
 * inside a frame already opened with PlatformGpu3DS_BeginTopSceneGpu(); the
 * caller still finishes the frame with PlatformGpu3DS_EndBottom() as usual
 * (bottom screen is untouched here). */
void Port_GpuRenderer_RenderFrame(void) {
    if (!sInitialized) return;

    sCacheCount = 0;
    sDrawItemCount = 0;
    sAnyDirtySlot = false;

    uint16_t dispcnt = (uint16_t)(gIoMem[0] | (gIoMem[1] << 8));
    bool obj1D = (dispcnt & (1u << 6)) != 0;

    /* BLDCNT/BLDALPHA/BLDY: computed once per frame, consumed by
     * CollectBgLayer/CollectSprite below (decide brighten/darken at decode
     * time, or flag items for the alpha-blend second pass) and by the draw
     * loop (EVA feeds the GPU blend constant). EVA/EVB/EVY are 5-bit fields
     * clamped to 16 on real hardware (GBATEK). */
    sIoBldcnt = (uint16_t)(gIoMem[0x50] | (gIoMem[0x51] << 8));
    sBldEffect = (uint8_t)((sIoBldcnt >> 6) & 3u);
    uint16_t bldalpha = (uint16_t)(gIoMem[0x52] | (gIoMem[0x53] << 8));
    uint16_t bldy = (uint16_t)(gIoMem[0x54] | (gIoMem[0x55] << 8));
    sBldEva = (int)(bldalpha & 0x1Fu);
    if (sBldEva > 16) sBldEva = 16;
    sBldEvb = (int)((bldalpha >> 8) & 0x1Fu);
    if (sBldEvb > 16) sBldEvb = 16;
    sBldEvy = (int)(bldy & 0x1Fu);
    if (sBldEvy > 16) sBldEvy = 16;

    for (int bg = 3; bg >= 0; --bg) {
        if (dispcnt & (1u << (8 + bg))) CollectBgLayer(bg);
    }
    if (dispcnt & (1u << 12)) {
        for (int i = 127; i >= 0; --i) CollectSprite(i, obj1D);
    }
    if (sDrawItemCount > 1) qsort(sDrawItems, (size_t)sDrawItemCount, sizeof(DrawItem), CompareDrawItems);

    {
        int objItems = 0;
        for (int i = 0; i < sDrawItemCount; ++i) {
            if (sDrawItems[i].depthTier == 4) ++objItems;
        }
        sLastObjItemCount = objItems;
    }

#ifdef PORT_GPU_RENDERER_DIAG_LOG
    {
        static unsigned sFrameCounter;
        if ((sFrameCounter++ % 5u) == 0u) {
            float minY = 1e9f, maxY = -1e9f;
            int objItems = 0, cacheSlots = sCacheCount, blendItems = 0;
            for (int i = 0; i < sDrawItemCount; ++i) {
                if (sDrawItems[i].y < minY) minY = sDrawItems[i].y;
                if (sDrawItems[i].y > maxY) maxY = sDrawItems[i].y;
                if (sDrawItems[i].depthTier == 4) ++objItems;
                if (sDrawItems[i].blendAlpha) ++blendItems;
            }
            char msg[240];
            int off = __builtin_snprintf(msg, sizeof(msg),
                                         "GPUDIAG dispcnt=%04x items=%d obj=%d cache=%d yrange=[%.0f,%.0f] "
                                         "bldcnt=%04x eff=%d eva=%d evb=%d blend=%d",
                                         dispcnt, sDrawItemCount, objItems, cacheSlots, (double)minY, (double)maxY,
                                         sIoBldcnt, sBldEffect, sBldEva, sBldEvb, blendItems);
            for (int bg = 0; bg < 4; ++bg) {
                if (!(dispcnt & (1u << (8 + bg)))) continue;
                uint16_t bgcnt = (uint16_t)(gIoMem[0x08 + bg * 2] | (gIoMem[0x09 + bg * 2] << 8));
                int hofsAddr = 0x10 + bg * 4, vofsAddr = 0x12 + bg * 4;
                uint16_t hofs = (uint16_t)(gIoMem[hofsAddr] | (gIoMem[hofsAddr + 1] << 8)) & 0x1FFu;
                uint16_t vofs = (uint16_t)(gIoMem[vofsAddr] | (gIoMem[vofsAddr + 1] << 8)) & 0x1FFu;
                off += __builtin_snprintf(msg + off, sizeof(msg) - (size_t)off, " bg%d[cnt=%04x h=%u v=%u]", bg,
                                          bgcnt, hofs, vofs);
                if (off >= (int)sizeof(msg)) break;
            }
            Port_DebugLog(msg);
        }
    }
#endif

    if (sAnyDirtySlot) {
        /* Only the decoded portion of the atlas needs flushing/uploading,
         * but tiles aren't necessarily contiguous slot-to-slot across
         * frames (cache resets every frame) -- flush the whole buffer, it's
         * one GSPGPU_FlushDataCache + one display transfer either way, not
         * per-tile cost. */
        GSPGPU_FlushDataCache(sAtlasPixels, ATLAS_DIM * ATLAS_DIM * sizeof(u32));
        C3D_SyncDisplayTransfer((u32*)sAtlasPixels, GX_BUFFER_DIM(ATLAS_DIM, ATLAS_DIM),
                                (u32*)sAtlasTexture.data, GX_BUFFER_DIM(ATLAS_DIM, ATLAS_DIM),
                                GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
                                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    }

    float slider3d = osGet3DSliderState();
    const float scale = 1.5f;
    const float screenBaseX = (400.0f - 240.0f * scale) * 0.5f;

    C3D_RenderTarget* leftTarget = PlatformGpu3DS_GetTopLeftTarget();
    C3D_RenderTarget* rightTarget = (slider3d > 0.01f) ? PlatformGpu3DS_GetTopRightTarget() : NULL;

#ifdef PORT_GPU_RENDERER_DIAG_LOG
    {
        static unsigned sEyeLogCounter;
        if ((sEyeLogCounter++ % 30u) == 0u) {
            char msg[96];
            snprintf(msg, sizeof(msg), "STEREO slider=%.3f left=%p right=%p", (double)slider3d,
                     (void*)leftTarget, (void*)rightTarget);
            Port_DebugLog(msg);
        }
    }
#endif

    for (int eye = 0; eye < 2; ++eye) {
        C3D_RenderTarget* target = (eye == 0) ? leftTarget : rightTarget;
        if (!target) continue;
        /* kTierEyeOffsetPx is all <=0, scaled larger in magnitude for
         * farther layers (BG3 furthest, -3.5px; BG0/HUD nearest, 0px).
         * Correct stereo convention for something behind the screen plane
         * (the common case here) is "uncrossed"/positive parallax: the
         * right eye's copy sits further RIGHT on-screen than the left
         * eye's, so the viewer's eyes diverge slightly and the object
         * reads as receding. This was backwards (eye0/left=-1,
         * eye1/right=+1): with a negative per-tier offset that shifts the
         * LEFT eye's far background right and the RIGHT eye's left --
         * right-x < left-x, crossed/negative parallax -- which reads as
         * the background popping out IN FRONT of the screen instead of
         * receding into it. Confirmed on hardware: backgrounds appeared to
         * sit in front of the foreground/gameplay layers once the right
         * eye's content was actually visible (fixed separately, see the
         * C2D_Init sizing above). */
        float eyeSign = (eye == 0) ? 1.0f : -1.0f;

        C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(target);
        ConfigureAtlasTextureEnv();
        /* Explicit, rather than trusting citro2d's own default: this is 2D
         * tile/sprite compositing purely by draw order (painter's
         * algorithm, same z=0.5 on every item, see C2D_DrawParams below),
         * with no legitimate use for depth testing. Defensive against the
         * right-eye target (the second one processed in a frame with the
         * 3D slider on) somehow inheriting an enabled depth test/compare
         * state that the first (left) target's own citro2d draws happened
         * to satisfy by coincidence of clear order, while the right
         * target's identical-Z overlapping quads (BG0/menu tiles drawn
         * over floor tiles at the same screen position) get compare-
         * rejected -- reported as "floor/menu tiles missing, right eye
         * only, 3D on only" and not explained by draw-call counts (which
         * are identical between eyes, see the EYE0/EYE1 diag log below). */
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
        /* GBA palette transparency is binary (index 0 = fully transparent),
         * so alpha-test discard handles it regardless of blend pass -- both
         * passes below keep this enabled. */
        C3D_AlphaTest(true, GPU_GREATER, 0);
        /* Standard passthrough blend: for opaque texels (alpha=255, the only
         * ones alpha-test lets through) this reduces to a plain replace, so
         * it's a safe default for every non-BLDCNT-blended item (the common
         * case). Set explicitly every frame rather than relying on
         * whatever citro2d's own default happens to be. */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
                       GPU_ONE_MINUS_SRC_ALPHA);

        int drawCount = 0;
        bool reassertedTexEnv = false;
        for (int i = 0; i < sDrawItemCount; ++i) {
            const DrawItem* item = &sDrawItems[i];
            if (item->blendAlpha) continue; /* second pass, below */
            float eyeOffset = eyeSign * slider3d * kTierEyeOffsetPx[item->depthTier];
            C2D_DrawParams params = {
                .pos = { .x = screenBaseX + eyeOffset + item->x * scale, .y = item->y * scale,
                         .w = item->w * scale, .h = item->h * scale },
                .center = { 0.0f, 0.0f },
                .depth = 0.5f,
                .angle = 0.0f,
            };
            C2D_DrawImage(item->img, &params, NULL);
            /* citro2d silently reprograms TEV unit 0 to its own default
             * whenever it switches between "solid" (C2D_DrawRectSolid,
             * used by the bottom-screen debug overlay in
             * platform_gpu_3ds.c, drawn every frame now) and "textured"
             * (C2D_DrawImage) draws -- it has no idea we hand-configured
             * TEV0 above via ConfigureAtlasTextureEnv() for the PICA200's
             * reversed RGBA8 byte order (see the big comment at the top of
             * this file), so the very first textured draw after a solid
             * draw clobbers it back to citro2d's default, which reads
             * texture channels in the wrong order -- exactly the red/
             * magenta-tinted screen from section 2.1 of
             * docs/3ds-port-gpu-renderer-status-2026-08-20.md, now
             * resurfacing every frame because the debug overlay guarantees
             * the mode ends each frame as "solid". Reassert once right
             * after the first draw of this loop (whether or not a clobber
             * actually happened) rather than before it, since re-running
             * it before would just get clobbered again by this same draw
             * call. */
            if (!reassertedTexEnv) {
                ConfigureAtlasTextureEnv();
                reassertedTexEnv = true;
            }
            /* Defensive: PlatformGpu3DS_Init sizes citro2d's shared vertex
             * buffer for this renderer's documented worst case
             * (MAX_DRAW_ITEMS), but a periodic flush here means a scene that
             * somehow exceeds it degrades to an extra GPU submission instead
             * of silently failing to draw past the buffer's capacity --
             * exactly the bug that made every large scene show only its
             * first ~128 quads (roughly the top of the screen) before the
             * buffer was resized. */
            if ((++drawCount % 512) == 0) C2D_Flush();
        }

        /* BLDCNT effect 1 (alpha blend) second pass: GBA blends a
         * first-target layer's pixel against whatever's the current
         * second-target pixel underneath using EVA/EVB (5-bit, out of 16).
         * Approximated here as: draw first-target items on top of whatever
         * is already in the framebuffer (the pass-1 opaque draws above),
         * blending with the GPU's own blend unit using EVA as the constant
         * blend alpha (assumes EVA+EVB~=16, true for the common crossfade
         * usage -- an exact per-pixel top/second-target match like the CPU
         * renderer does would need reading back the framebuffer per pixel,
         * not practical here). GPU_CONSTANT_ALPHA/GPU_ONE_MINUS_CONSTANT_ALPHA
         * read C3D_BlendingColor's alpha instead of the texture's own alpha,
         * which matters because our texture alpha is binary (0 or 255) from
         * the alpha-test above, not a blend factor. */
        if (sBldEffect == 1) {
            bool flushedForBlend = false;
            for (int i = 0; i < sDrawItemCount; ++i) {
                const DrawItem* item = &sDrawItems[i];
                if (!item->blendAlpha) continue;
                if (!flushedForBlend) {
                    C2D_Flush();
                    C3D_BlendingColor(C2D_Color32(0, 0, 0, (u32)((sBldEva * 255) / 16)));
                    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA,
                                   GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA);
                    flushedForBlend = true;
                }
                float eyeOffset = eyeSign * slider3d * kTierEyeOffsetPx[item->depthTier];
                C2D_DrawParams params = {
                    .pos = { .x = screenBaseX + eyeOffset + item->x * scale, .y = item->y * scale,
                             .w = item->w * scale, .h = item->h * scale },
                    .center = { 0.0f, 0.0f },
                    .depth = 0.5f,
                    .angle = 0.0f,
                };
                C2D_DrawImage(item->img, &params, NULL);
                /* Same TEV-clobber concern as the pass-1 loop above -- only
                 * relevant here if pass 1 drew zero items (mode could still
                 * be "solid" entering this loop). */
                if (!reassertedTexEnv) {
                    ConfigureAtlasTextureEnv();
                    reassertedTexEnv = true;
                }
                if ((++drawCount % 512) == 0) C2D_Flush();
            }
            if (flushedForBlend) {
                C2D_Flush();
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
                               GPU_ONE_MINUS_SRC_ALPHA);
            }
        }
#ifdef PORT_GPU_RENDERER_DIAG_LOG
        {
            /* One counter per eye -- a single shared counter with an even
             * modulo (30) always lands on the same eye's turn every time
             * (eye0 calls fall on even indices, eye1 on odd, so
             * counter%30==0 only ever coincides with eye0), silently never
             * logging EYE1 at all despite it running every frame. Cost a
             * whole round of "why is eye1 never reaching this line"
             * confusion before the parity was noticed. */
            static unsigned sEyeDrawLogCounter[2];
            if ((sEyeDrawLogCounter[eye]++ % 30u) == 0u) {
                char msg[64];
                snprintf(msg, sizeof(msg), "EYE%d drawCount=%d reasserted=%d", eye, drawCount,
                         (int)reassertedTexEnv);
                Port_DebugLog(msg);
            }
        }
#endif
    }
}

/* Snapshot of the most recently rendered frame's item counts, for the
 * bottom-screen debug overlay in platform_gpu_3ds.c (PlatformGpu3DS_EndBottom)
 * -- lets a dev tell at a glance whether the GPU path is drawing a
 * reasonable scene or something degenerate (e.g. way too many items, which
 * was the tell for the citro2d buffer-exhaustion bug in section 2.5 of
 * docs/3ds-port-gpu-renderer-status-2026-08-20.md). Values hold their last
 * value between RenderFrame calls, which is fine since the overlay is only
 * read once per presented frame anyway. */
void Port_GpuRenderer_GetLastFrameStats(int* outItems, int* outObjItems, int* outCacheSlots) {
    if (outItems) *outItems = sDrawItemCount;
    if (outObjItems) *outObjItems = sLastObjItemCount;
    if (outCacheSlots) *outCacheSlots = sCacheCount;
}
