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
#include "port_stereo_depth.h"
#include "port_layer_fixes.h"
#include "platform_gpu_3ds.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef PORT_GPU_RENDERER_DIAG_LOG
#include <stdio.h>
/* Buffered, not Port_DebugLog -- this is per-frame diagnostic logging, not
 * a boot/hang checkpoint. See port_debug_log.h's comment: the unbuffered
 * version's per-call fopen/fwrite/fclose measured as the actual dominant
 * cost misattributed to the atlas upload for most of this session (see
 * docs/3ds-port-gpu-renderer-status-2026-08-20.md section 16). */
#include "port_debug_log.h"
#define Port_DebugLog Port_DebugLogBuffered
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
    uint8_t brightAdjust; /* BrightAdjust -- deliberately NOT keyed on evy
                           * (see sCacheEvy below): an earlier attempt at
                           * putting evy in this key fixed a stale-brightness
                           * bug but caused a WORSE regression on real
                           * hardware -- during a fade (evy ramping every
                           * frame) it minted a brand-new atlas slot per
                           * distinct evy value for every affected tile,
                           * exhausting the whole 4096-slot atlas (confirmed
                           * via GPUDIAG showing cache=4096 exactly, twice, in
                           * one hardware session) within a single fade,
                           * after which every further tile that frame
                           * aliased to slot 0's stale content -- the actual
                           * cause of "scenery/menus missing" reported after
                           * that fix. evy is tracked per-slot instead (see
                           * sCacheEvy) and treated as a staleness condition
                           * that redecodes IN PLACE (same slot), not a key
                           * that mints a new one. */
} TileCacheKey;

/* Per-layer visibility relative to the single active GBA window (WIN0 xor
 * WIN1 -- see sWindowActive/sLayerWinVis in Port_GpuRenderer_RenderFrame).
 * WIN_VIS_NEVER means the whole layer is disabled while the window is
 * active (neither WININ's inside bit nor WINOUT's outside bit set for it) --
 * items are never pushed for such a layer at all (see ComputeLayerWinVis),
 * so it never appears on a DrawItem. */
typedef enum { WIN_VIS_ALWAYS, WIN_VIS_INSIDE_ONLY, WIN_VIS_OUTSIDE_ONLY, WIN_VIS_NEVER } WindowVis;

typedef struct DrawItem {
    C2D_Image img;
    /* Non-affine item: x,y is the top-left corner in GBA screen space, w/h
     * are always 8 (one atlas tile). Affine OBJ item (affine=true): x,y is
     * instead this subtile's already-transformed CENTER in GBA screen space
     * (see CollectSprite's affine path), w/h are the subtile's rotation-
     * decomposed scaled size, and angle is the sprite's decomposed rotation
     * -- see the big comment on affine OBJ handling in CollectSprite for why
     * per-subtile rotation about the sprite's own pivot reproduces the whole
     * sprite's rotation exactly for the common rotate+uniform-scale case. */
    float x, y, w, h;
    float angle;
    int sortKey;   /* ascending draw order: lower = drawn first (further back) */
    int8_t depthTier; /* stereo parallax tier, see kTierEyeOffsetScale */
    bool blendAlpha; /* BLDCNT effect 1 (alpha blend) active for this item's
                      * layer -- drawn in a second pass with GPU blending
                      * against whatever's already in the framebuffer,
                      * approximating GBA's 1st-target/2nd-target blend
                      * (see Port_GpuRenderer_RenderFrame). */
    bool affine;
    WindowVis winVis;
} DrawItem;

static C3D_Tex sAtlasTexture;
static TileCacheKey sCacheKeys[ATLAS_MAX_SLOTS];
static int sCacheCount;
static DrawItem sDrawItems[MAX_DRAW_ITEMS];
static int sDrawItemCount;
static bool sAnyDirtySlot;
static int sLastObjItemCount;

/* O(1)-amortized tile cache lookup, replacing a linear scan over
 * sCacheKeys[0..sCacheCount) that used to run once per tile REFERENCE (not
 * per unique tile) -- up to ~2500-2700 references/frame in real gameplay,
 * each scanning up to a few hundred cache entries: the single largest CPU
 * cost in this renderer, confirmed the top suspect once correctness bugs
 * were fixed (see docs/3ds-port-gpu-renderer-status-2026-08-20.md section
 * 5.9). Open-chaining hash table. Unlike an earlier version of this cache,
 * the table (and the decoded atlas contents it points at) now PERSISTS
 * across frames instead of being wiped every frame -- see the big comment
 * on GetOrDecodeTileSlot below for why: resetting sCacheCount to 0 every
 * frame meant every one of a frame's few hundred *unique* tiles got
 * fully re-decoded (palette lookup + branch per pixel, 64 pixels/tile) on
 * every single frame even when the underlying VRAM tile data was identical
 * to the previous frame -- which is the common case (most tiles don't
 * animate frame-to-frame; only scroll registers change during normal
 * scrolling). That redundant redecoding, not draw-call count or atlas
 * upload size (see sections 7.1-7.3 of the doc), turned out to be the
 * dominant remaining CPU cost once those were fixed. */
enum { HASH_BUCKETS = 8192, HASH_MASK = HASH_BUCKETS - 1 };
static int32_t sHashBucketHead[HASH_BUCKETS];
static int32_t sHashChainNext[ATLAS_MAX_SLOTS];
/* Raw source bytes (up to 64 for 8bpp) as of each slot's last decode --
 * compared via memcmp on a cache hit to detect whether the underlying VRAM
 * tile actually changed (animated tiles: water, lava, etc.) since the slot
 * was last decoded. A memcmp of <=64 bytes is far cheaper than a full
 * decode (palette lookup + optional brighten/darken per pixel). */
static uint8_t sCacheSourceBytes[ATLAS_MAX_SLOTS][64];
/* Hash of the palette bytes actually sampled for this slot's last decode --
 * the bank's 32 bytes (16 colors) for a 4bpp tile, or the full 512-byte
 * palette (256 colors) for an 8bpp tile (which indexes the whole palette
 * directly, no banking). MISSING from an earlier version of this cache:
 * TileCacheKey only stores `palBank`, a numeric index, never the actual
 * RGB content living at that bank -- so a hash hit only proved the same
 * VRAM tile GRAPHIC (pixel indices) was being reused with the same bank
 * NUMBER, never that the bank still held the same COLORS. GBA games
 * routinely reuse the same generic tile shapes across many rooms while
 * loading a different palette per room into the same bank slots -- exactly
 * this project's case (confirmed on hardware: after the cache was made
 * persistent, only the room just re-entered showed correctly, forcing a
 * fresh decode with its own palette; every other already-visited room
 * stayed rendered with whichever room's palette had been cached first for
 * that tile+bank combination, appearing black when that first cached
 * palette happened to be a dark one).
 *
 * A first fix stored and memcmp'd the raw palette bytes per slot (up to
 * 512 bytes) on EVERY tile reference (not just unique tiles -- up to
 * ~2500+ references/frame), which showed up as real cost in
 * GPUTIME collectMs on hardware. The palette bytes being compared are the
 * same for every reference sharing a (bpp, bank) this frame (gBgPltt/
 * gObjPltt don't change mid-frame), so hashing each relevant bank ONCE per
 * frame (see sBgPalBankHash/sBgPalFullHash/sObjPalBankHash/sObjPalFullHash
 * in Port_GpuRenderer_RenderFrame) and comparing that 4-byte hash per
 * reference instead is far cheaper while catching the exact same staleness
 * condition -- a 32-bit FNV-1a hash collision between two genuinely
 * different palettes is astronomically unlikely for this program's actual
 * palette value space, an accepted tradeoff (same one any hash-based cache
 * makes) for cutting a >100KB/frame memcmp bill down to a few thousand
 * integer compares. */
static uint32_t sCachePalHash[ATLAS_MAX_SLOTS];
/* evy (0..16) used the last time this slot was decoded, only meaningful
 * when the slot's brightAdjust != NONE. NOT part of the hash key (see
 * TileCacheKey's comment) -- checked as a second staleness condition
 * alongside the source-bytes memcmp in GetOrDecodeTileSlot, redecoding the
 * SAME slot in place when it no longer matches instead of minting a new
 * one, so a fade (evy changing every frame) can't exhaust the atlas. */
static uint8_t sCacheEvy[ATLAS_MAX_SLOTS];
/* Bitmask of tile-rows actually written to the atlas THIS frame (a cache
 * hit with unchanged source bytes writes nothing) -- ATLAS_MAX_SLOTS/
 * ATLAS_TILES_PER_ROW is exactly 64 rows, so one bit per row fits a single
 * uint64_t. Lets the upload at the end of RenderFrame transfer only the
 * rows that actually changed. A single min..max CONTIGUOUS RANGE (an
 * earlier version of this) looked sufficient but wasn't: once the cache
 * persists across frames, a frame can touch a low-numbered row (an
 * in-place redecode of some old, still-referenced slot -- an animated
 * tile, or a stale palette/evy) and a high-numbered row (a genuinely new
 * tile, appended at the current sCacheCount) in the SAME frame, and a
 * min..max range covering both ends up spanning nearly the WHOLE atlas
 * even though only two 8px rows actually changed. Confirmed on hardware:
 * GPUTIME's new tile/upload split (see PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC's
 * comment) showed the tile-collection work itself costing ~1ms/frame while
 * the "only transfer the dirty rows" atlas upload still cost ~20ms/frame --
 * the range-based version wasn't actually narrow in practice. A bitmask +
 * transferring each contiguous RUN of set bits separately fixes this
 * properly instead of guessing at a better single range. */
static uint64_t sDirtyRowMask;

/* Per-frame palette hashes (see sCachePalHash's comment): computed ONCE per
 * frame in Port_GpuRenderer_RenderFrame from gBgPltt/gObjPltt, then reused
 * by every tile reference that frame instead of re-hashing/re-comparing
 * palette bytes per reference. Bank hashes cover the 16 four-bit banks (32
 * bytes/16 colors each); full hashes cover the whole 512-byte/256-color
 * palette (what an 8bpp tile, which doesn't bank, actually samples from). */
static uint32_t sBgPalBankHash[16], sObjPalBankHash[16];
static uint32_t sBgPalFullHash, sObjPalFullHash;

#ifdef PORT_GPU_RENDERER_DIAG_LOG
/* Set once per frame in Port_GpuRenderer_RenderFrame -- see its comment.
 * Gates the per-tile diagnostic logging in GetOrDecodeTileSlot/CollectSprite
 * added for issue #17, so it only fires during the one rare scene shape
 * being investigated (no BG layers, OBJ+non-clipping-WIN1 only). */
static bool sDiagObjSceneLog;
#endif

static inline uint32_t HashBytes(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) h = (h ^ data[i]) * 16777619u;
    return h;
}

static inline uint32_t HashTileCacheKey(const TileCacheKey* k) {
    uint32_t h = k->byteOffset;
    h = h * 2654435761u + ((uint32_t)k->bpp8 | ((uint32_t)k->palBank << 1) | ((uint32_t)k->hflip << 9) |
                            ((uint32_t)k->vflip << 10) | ((uint32_t)k->isObj << 11) |
                            ((uint32_t)k->brightAdjust << 12));
    h ^= h >> 15;
    return h & HASH_MASK;
}

static inline bool TileCacheKeyEqual(const TileCacheKey* a, const TileCacheKey* b) {
    return a->byteOffset == b->byteOffset && a->bpp8 == b->bpp8 && a->palBank == b->palBank &&
           a->hflip == b->hflip && a->vflip == b->vflip && a->isObj == b->isObj &&
           a->brightAdjust == b->brightAdjust;
}

/* Counting/bucket sort replacing qsort() for draw-order. sortKey only ever
 * takes values in [0, 34] ((3-priority)*10 + tiebreak, tiebreak in [0,4] --
 * see CollectBgLayer/CollectSprite), so an O(n) bucket pass beats qsort's
 * O(n log n) with its per-comparison indirect call, and -- unlike qsort,
 * which never guaranteed stability -- this preserves insertion order within
 * a bucket, which OBJ same-priority draw order actually depends on (lower
 * OAM index on top, via back-to-front iteration order -- see CollectSprite's
 * comment). */
enum { SORT_KEY_BUCKETS = 35 };
static int32_t sBucketHead[SORT_KEY_BUCKETS];
static int32_t sBucketTail[SORT_KEY_BUCKETS];
static int32_t sBucketNext[MAX_DRAW_ITEMS];
static int32_t sOpaqueOrder[MAX_DRAW_ITEMS];
static int sOpaqueCount;
static int32_t sBlendOrder[MAX_DRAW_ITEMS];
static int sBlendCount;
/* True back-to-front draw sequence across BOTH opaque and blend items
 * together, in real GBA sortKey order -- see the single merged draw loop in
 * Port_GpuRenderer_RenderFrame for why this exists instead of the old
 * opaque-pass-then-blend-pass split. */
static int32_t sDrawOrder[MAX_DRAW_ITEMS];
static int sDrawOrderCount;

/* GBA sprite shape/size -> pixel dimensions (attr0 bits14-15 = shape,
 * attr1 bits14-15 = size). Same table as port/ppu/src/mode1.c's
 * mode1_obj_widths/heights -- duplicated here rather than shared because
 * that file's tables are static/internal to the CPU renderer and this
 * module intentionally doesn't reach into it (independent, swappable
 * renderer backends behind the same PPU memory). */
static const uint8_t kObjWidths[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t kObjHeights[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

/* Stereo depth lives in port_stereo_depth.c as a pure function of register
 * state, so platform/3ds/tests/stereo_depth_test.c can enumerate the whole
 * input space on the host -- no GPU, no 3DS, no ROM. Every past depth bug
 * (the split crate, the torn ramp, the column in front of Samus) is a
 * property of that mapping, not of the drawing, so that is where new rules
 * belong. Building the state here once per frame is all this file does. */
static PortStereoDepthState sDepthState;

extern int Port_Hud_GetOamCount(void);

/* Room the correction list was last selected for. Re-selecting on every frame
 * would rescan the whole list for nothing; the room only changes on a door. */
static int sFixArea = -1, sFixRoom = -1;

static void UpdateLayerFixRoom(void) {
    if (!PortLayerFix_Present()) return;

    extern u8 gCurrentArea;
    extern u8 gCurrentRoom;
    if ((int)gCurrentArea == sFixArea && (int)gCurrentRoom == sFixRoom) return;
    sFixArea = (int)gCurrentArea;
    sFixRoom = (int)gCurrentRoom;

    /* The room's decompressed block maps, which is what each correction's
     * checksum is validated against. Only BG0..BG2 exist here -- BG3 is a
     * separate LZ77 backdrop and has no block map (src/room.c:454). */
    extern struct {
        struct { u16* pDecomp; u16 width; u16 height; } backgrounds[3];
        u16* pClipDecomp;
        u16 clipdataWidth;
        u16 clipdataHeight;
    } gBgPointersAndDimensions;

    const uint16_t* data[4];
    uint16_t w[4], h[4];
    for (int bg = 0; bg < 4; ++bg) {
        if (bg < 3) {
            data[bg] = (const uint16_t*)gBgPointersAndDimensions.backgrounds[bg].pDecomp;
            w[bg] = gBgPointersAndDimensions.backgrounds[bg].width;
            h[bg] = gBgPointersAndDimensions.backgrounds[bg].height;
        } else {
            data[bg] = NULL; w[bg] = 0; h[bg] = 0;
        }
    }
    PortLayerFix_SetRoom(sFixArea, sFixRoom, data, w, h);
}

static void ComputeDepthState(uint16_t dispcnt) {
    extern s16 gMainGameMode;
    (void)dispcnt;
    sDepthState.inGameplay = (gMainGameMode == 4);
    for (int bg = 0; bg < 4; ++bg) {
        sDepthState.priority[bg] =
            (uint8_t)(((uint16_t)(gIoMem[0x08 + bg * 2] | (gIoMem[0x09 + bg * 2] << 8))) & 3u);
    }
    UpdateLayerFixRoom();
}

bool Port_GpuRenderer_IsActive(void) { return sGpuRendererActive; }
void Port_GpuRenderer_SetActive(bool active) { sGpuRendererActive = active; }

/* Cache-maintenance for CPU-written texture memory (see
 * Port_GpuRenderer_Init's comment on why sAtlasTexture is CPU-writable
 * linear memory now). svcFlushProcessDataCache is a KERNEL SYSCALL --
 * costs at most a few microseconds for a region this size -- unlike
 * GSPGPU_FlushDataCache, which despite the similar name is an IPC call to
 * the gsp::Gpu *service process* (it returns a Result, ctrulib's
 * convention for service calls): a real round trip through another
 * process, which is what turned out to still cost ~18-30ms/frame on
 * hardware even after removing the actual C3D_SyncDisplayTransfer -- the
 * exact same class of cost this whole change was meant to eliminate,
 * just hiding behind a deceptively similar-sounding function name.
 * citro3d's own C3D_TexFlush() (which this could have called instead)
 * wraps this same syscall but only flushes a whole texture at once; this
 * wrapper keeps the dirty-row-range granularity from sDirtyRowMask. */
static inline void FlushAtlasRange(void* addr, size_t size) {
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)addr, (u32)size);
}

static Tex3DS_SubTexture sSlotSubtexTable[ATLAS_MAX_SLOTS];

static void InitSlotSubtexTable(void) {
    const float invAtlas = 1.0f / (float)ATLAS_DIM;
    for (int slot = 0; slot < ATLAS_MAX_SLOTS; ++slot) {
        int sx = (slot % ATLAS_TILES_PER_ROW) * 8;
        int sy = (slot / ATLAS_TILES_PER_ROW) * 8;
        sSlotSubtexTable[slot] = (Tex3DS_SubTexture){
            .width = 8,
            .height = 8,
            .left = ((float)sx + 0.5f) * invAtlas,
            .top = 1.0f - ((float)sy + 0.5f) * invAtlas,
            .right = ((float)sx + 7.5f) * invAtlas,
            .bottom = 1.0f - ((float)sy + 7.5f) * invAtlas,
        };
    }
}

bool Port_GpuRenderer_Init(void) {
    if (sInitialized) return true;

    /* C3D_TexInit (NOT C3D_TexInitVRAM) -- allocates the texture's backing
     * store in regular linear (FCRAM) memory via linearAlloc internally,
     * which the CPU can write directly. Deliberate: see
     * DecodeTileIntoSlot's comment for why this lets tile decoding skip
     * C3D_SyncDisplayTransfer (the GX/GSP-IPC-bound blocking call that
     * measured as ~19-20ms/frame on real hardware, dominating collectMs,
     * independent of how little data actually changed -- see
     * docs/3ds-port-gpu-renderer-status-2026-08-20.md section 14)
     * entirely, in favor of writing already-swizzled pixels straight into
     * the texture and a plain cache-maintenance syscall (see
     * FlushAtlasRange below) -- no GSP IPC round trip. No separate
     * CPU-side staging buffer needed either -- sAtlasTexture.data itself
     * is written directly now. VRAM-backed textures trade this
     * CPU-writability for a separate memory bus (less FCRAM/GPU
     * contention), not worth it here given the transfer step's cost
     * dwarfed any bandwidth benefit. */
    if (!C3D_TexInit(&sAtlasTexture, ATLAS_DIM, ATLAS_DIM, GPU_RGBA8)) return false;
    memset(sAtlasTexture.data, 0, ATLAS_DIM * ATLAS_DIM * sizeof(u32));
    FlushAtlasRange(sAtlasTexture.data, ATLAS_DIM * ATLAS_DIM * sizeof(u32));
    C3D_TexSetFilter(&sAtlasTexture, GPU_NEAREST, GPU_NEAREST);

    InitSlotSubtexTable();

    for (int i = 0; i < HASH_BUCKETS; ++i) sHashBucketHead[i] = -1;
    sCacheCount = 0;

    sInitialized = true;
    return true;
}

void Port_GpuRenderer_Shutdown(void) {
    if (!sInitialized) return;
    C3D_TexDelete(&sAtlasTexture); /* also frees the linearAlloc'd backing store */
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

/* Frame-global window-clip state (WIN0 xor WIN1 -- see
 * Port_GpuRenderer_CanRenderFrame's scope note: two simultaneously-clipping
 * windows still fall back to CPU, so at most one of WIN0/WIN1 is ever the
 * source here), computed once per frame in Port_GpuRenderer_RenderFrame and
 * consumed by CollectBgLayer/CollectSprite (per-layer visibility) and the
 * draw loop (the scissor rect itself). sWinLeft/Top/Right/Bottom are in raw
 * GBA screen pixels (0..240, 0..160), converted to the render target's
 * scissor space at draw time. */
static bool sWindowActive;
static int sWinLeft, sWinTop, sWinRight, sWinBottom;
static WindowVis sLayerWinVis[5]; /* index 0-3 = BG0-3, 4 = OBJ, same layer-id convention as BldIsFirstTarget */

/* OBJ-window (OBJWIN, DISPCNT bit15) state: a second, mutually-exclusive-
 * with-sWindowActive clipping source (Port_GpuRenderer_CanRenderFrame
 * rejects the combination of OBJWIN with an actually-clipping WIN0/WIN1),
 * where "inside" is not a rectangle but the union of every live OBJ-mode-2
 * sprite's opaque pixels (confirmed occurring: the pause screen's suit-view
 * wireframe Samus reuses itself as an OBJWIN mask, see
 * docs/3ds-gpu-renderer-window-affine-mosaic-feasibility-2026-08-21.md's
 * OBJWIN section).
 *
 * A first attempt rendered this mask into the PICA200's stencil buffer
 * (extra GPU render-target format change + a stencil-write pre-pass +
 * GPU_EQUAL stencil-test draw passes) -- it built clean but rendered
 * garbled on real hardware/Azahar (see that section's "REVERTED" update),
 * most likely because this environment has no display to verify an
 * undocumented depth/stencil fixed-function interaction against before
 * shipping it. This version deliberately avoids the GPU stencil unit
 * entirely: the mask is rasterized on the CPU into a coarse 8x8-cell
 * coverage grid (sObjWinCovered, see ComputeObjWinMask) BEFORE
 * CollectBgLayer/CollectSprite run, and each candidate item's visibility is
 * resolved by a single grid lookup at COLLECTION time (see
 * ObjWinItemVisible) -- no second draw pass, no new render-target state, no
 * GPU feature this renderer hasn't already relied on elsewhere (VRAM/OAM
 * reads and a plain boolean array). Coarser than the stencil approach would
 * have been (8x8-cell granularity, not per-pixel; an affine mask sprite's
 * coverage is approximated as its whole rotated bounding box, not its exact
 * rotated silhouette -- see ComputeObjWinMask), but low-risk and reuses only
 * mechanisms already proven working in this file. */
static bool sObjWindowActive;
enum { OBJWIN_GRID_COLS = 240 / 8, OBJWIN_GRID_ROWS = 160 / 8 };
static bool sObjWinCovered[OBJWIN_GRID_ROWS][OBJWIN_GRID_COLS];

/* WININ/WINOUT per-layer bit -> WindowVis, for the single active window's
 * inside mask (WININ low byte for WIN0, high byte for WIN1) and the shared
 * WINOUT "outside any window" mask. Bit layout (GBATek): bits0-3 BG0-3,
 * bit4 OBJ, bit5 special-effect-enable (ignored -- see the existing
 * WindowCoversFullScreen comment on approximating this bit away, same
 * rationale extended from the gating-only case to the real-clip case here). */
static inline WindowVis ComputeLayerWinVis(uint8_t insideMask, uint8_t outsideMask, int layerBit) {
    bool in = ((insideMask >> layerBit) & 1u) != 0u;
    bool out = ((outsideMask >> layerBit) & 1u) != 0u;
    if (in && out) return WIN_VIS_ALWAYS;
    if (in) return WIN_VIS_INSIDE_ONLY;
    if (out) return WIN_VIS_OUTSIDE_ONLY;
    return WIN_VIS_NEVER;
}

/* True if `byteOffset` (into gVram) names a tile with at least one non-zero
 * (opaque) palette index. Deliberately raw/minimal -- just enough to
 * rasterize OBJWIN mask coverage (see ComputeObjWinMask), unlike
 * GetOrDecodeTileSlot's full decode (palette lookup, brighten/darken,
 * atlas write, caching): a mask tile's actual COLOR never matters, only
 * whether it has any opaque texel at all. */
static inline bool TileHasOpaquePixel(uint32_t byteOffset, bool bpp8) {
    const uint32_t* src = (const uint32_t*)(gVram + byteOffset);
    if (!bpp8) {
        return (src[0] | src[1] | src[2] | src[3] | src[4] | src[5] | src[6] | src[7]) != 0;
    } else {
        return (src[0] | src[1] | src[2] | src[3] | src[4] | src[5] | src[6] | src[7] |
                src[8] | src[9] | src[10] | src[11] | src[12] | src[13] | src[14] | src[15]) != 0;
    }
}

/* Rasterizes every live OBJWIN sprite (attr0 objMode==2) into
 * sObjWinCovered, an 8x8-cell-granularity coverage grid over the 240x160
 * GBA screen -- called once per frame, before CollectBgLayer/CollectSprite,
 * only when sObjWindowActive. Mirrors CollectSprite's own OAM decode
 * (shape/size/position/flip/tile-mapping) closely enough to place tiles
 * correctly, but stays CPU-only and boolean: no atlas slot, no DrawItem, no
 * GPU state at all (see sObjWindowActive's comment for why that's
 * deliberate this time).
 *
 * Non-affine mask sprites get exact per-TILE coverage (a screen cell is
 * "covered" if the tile mapped to it has any opaque texel -- see
 * TileHasOpaquePixel); this matches the granularity CollectBgLayer/
 * CollectSprite already draw at, so it's not a coarser approximation than
 * the rest of this renderer's tile-atlas approach. Affine mask sprites
 * (the confirmed pause-screen wireframe case can rotate) instead mark their
 * entire rotated BOUNDING BOX as covered -- an intentionally coarser
 * approximation (a rotated silhouette's true shape is smaller than its
 * axis-aligned bounding box), accepted here to avoid re-deriving the same
 * inverse-affine-matrix machinery CollectSprite's drawable path already has
 * just to rasterize a boolean mask; revisit if a room turns up where this
 * over-covers noticeably. */
static void ComputeObjWinMask(bool obj1D) {
    memset(sObjWinCovered, 0, sizeof(sObjWinCovered));
    const uint16_t* oam = (const uint16_t*)gOamMem;
    const uint8_t* objBase = gVram + 0x10000u;

    for (int oamIndex = 0; oamIndex < 128; ++oamIndex) {
        uint16_t attr0 = oam[oamIndex * 4 + 0];
        uint16_t attr1 = oam[oamIndex * 4 + 1];
        uint16_t attr2 = oam[oamIndex * 4 + 2];

        bool isAffine = ((attr0 >> 8) & 1u) != 0u;
        if (((attr0 >> 9) & 1u) && !isAffine) continue; /* disabled (non-affine hidden bit) */
        uint8_t objMode = (uint8_t)((attr0 >> 10) & 3u);
        if (objMode != 2) continue; /* only OBJWIN mask sprites contribute */

        uint8_t shape = (uint8_t)((attr0 >> 14) & 3u);
        uint8_t size = (uint8_t)((attr1 >> 14) & 3u);
        if (shape == 3) continue;
        int width = kObjWidths[shape][size];
        int height = kObjHeights[shape][size];
        bool doubleSize = isAffine && (((attr0 >> 9) & 1u) != 0u);
        int boundsWidth = doubleSize ? width * 2 : width;
        int boundsHeight = doubleSize ? height * 2 : height;

        int y = attr0 & 0xFFu;
        if (y >= 160) y -= 256;
        int x = (int)(attr1 & 0x1FFu);
        if (x >= 240) x -= 512;
        if (y >= 160 || y + boundsHeight <= 0 || x >= 240 || x + boundsWidth <= 0) continue;

        if (isAffine) {
            /* Coarse approximation (see this function's header comment):
             * whole bounding box, no attempt at the rotated silhouette. */
            int cellX0 = x < 0 ? 0 : x / 8;
            int cellY0 = y < 0 ? 0 : y / 8;
            int cellX1 = (x + boundsWidth) > 240 ? 240 : x + boundsWidth;
            int cellY1 = (y + boundsHeight) > 160 ? 160 : y + boundsHeight;
            for (int cy = cellY0; cy * 8 < cellY1; ++cy) {
                for (int cx = cellX0; cx * 8 < cellX1; ++cx) sObjWinCovered[cy][cx] = true;
            }
            continue;
        }

        bool bpp8 = ((attr0 >> 13) & 1u) != 0;
        bool hflip = ((attr1 >> 12) & 1u) != 0;
        bool vflip = ((attr1 >> 13) & 1u) != 0;
        uint16_t baseTile = attr2 & 0x3FFu;
        const int bytesPerTile = bpp8 ? 64 : 32;
        int tilesW = width / 8;
        int tilesH = height / 8;

        for (int ty = 0; ty < tilesH; ++ty) {
            int destY = y + ty * 8;
            if (destY <= -8 || destY >= 160) continue;
            for (int tx = 0; tx < tilesW; ++tx) {
                int destX = x + tx * 8;
                if (destX <= -8 || destX >= 240) continue;
                int srcTx = hflip ? (tilesW - 1 - tx) : tx;
                int srcTy = vflip ? (tilesH - 1 - ty) : ty;
                uint16_t tileIndex;
                if (obj1D) {
                    tileIndex = (uint16_t)(baseTile + (srcTy * tilesW + srcTx) * (bpp8 ? 2 : 1));
                } else {
                    tileIndex = (uint16_t)(baseTile + srcTy * 32 + srcTx * (bpp8 ? 2 : 1));
                }
                uint32_t byteOffset = (uint32_t)(objBase - gVram) + (uint32_t)tileIndex * bytesPerTile;
                if (!TileHasOpaquePixel(byteOffset, bpp8)) continue;
                int cellX = destX / 8, cellY = destY / 8; /* destX/Y already tile-aligned to the 8px grid */
                if (cellX < 0 || cellX >= OBJWIN_GRID_COLS || cellY < 0 || cellY >= OBJWIN_GRID_ROWS) continue;
                sObjWinCovered[cellY][cellX] = true;
            }
        }
    }
}

/* Resolves a candidate item's OBJWIN visibility at COLLECTION time (no
 * draw-time GPU state involved -- see sObjWindowActive's comment): `layerVis`
 * is this item's layer's WININ/WINOUT-derived classification (from
 * sLayerWinVis, same as the WIN0/WIN1 rect mechanism uses), and
 * (screenX, screenY) is the item's GBA-screen-space position used to sample
 * sObjWinCovered. Only called when sObjWindowActive. */
static inline bool ObjWinItemVisible(WindowVis layerVis, float screenX, float screenY) {
    if (layerVis == WIN_VIS_ALWAYS) return true;
    if (layerVis == WIN_VIS_NEVER) return false;
    int cellX = (int)screenX / 8, cellY = (int)screenY / 8;
    if (cellX < 0) cellX = 0; else if (cellX >= OBJWIN_GRID_COLS) cellX = OBJWIN_GRID_COLS - 1;
    if (cellY < 0) cellY = 0; else if (cellY >= OBJWIN_GRID_ROWS) cellY = OBJWIN_GRID_ROWS - 1;
    bool covered = sObjWinCovered[cellY][cellX];
    return (layerVis == WIN_VIS_INSIDE_ONLY) ? covered : !covered;
}

/* PICA200 8x8-texel tile swizzle (Z-order/Morton within the tile) -- the
 * exact per-texel reordering that GX_TRANSFER_OUT_TILED(1) used to apply
 * for us when tile pixels were decoded into a plain linear staging buffer
 * and then handed to C3D_SyncDisplayTransfer. Now that DecodeTileIntoSlot
 * writes straight into the (CPU-writable, see Port_GpuRenderer_Init)
 * texture memory, this table does that reordering by hand instead, so the
 * GX transfer step (and its ~19-20ms/frame blocking cost, independent of
 * how little data changed -- see
 * docs/3ds-port-gpu-renderer-status-2026-08-20.md section 14) isn't
 * needed at all. Standard table, widely reproduced across 3DS homebuild
 * texture tooling for exactly this format. */
static const uint8_t kSwizzleLUT[64] = {
    0,  1,  4,  5, 16, 17, 20, 21,
    2,  3,  6,  7, 18, 19, 22, 23,
    8,  9, 12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,
    34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,
    42, 43, 46, 47, 58, 59, 62, 63,
};

/* Decodes one 8x8 tile (4bpp or 8bpp, GBA-packed) from `src` directly into
 * this slot's swizzled position in sAtlasTexture.data (see kSwizzleLUT),
 * applying `pal`/`palBank`, flips, and (when this tile's layer is a
 * BLDCNT first-target and the active effect is brighten/darken) the
 * brightness adjustment. Records the raw source bytes into
 * sCacheSourceBytes so a future call can detect via memcmp whether the
 * underlying VRAM tile changed (see GetOrDecodeTileSlot) without
 * redecoding pixel-by-pixel, and sets this slot's row bit in
 * sDirtyRowMask so the end-of-frame cache flush only covers the atlas
 * rows actually touched this frame. */
static void DecodeTileIntoSlot(int slot, const uint8_t* src, bool bpp8, const uint16_t* pal, int palBank,
                               bool hflip, bool vflip, BrightAdjust brightAdjust, uint32_t palHash) {
    /* Each slot is one 64-texel (8x8) block; blocks are stored row-major
     * across the atlas (ATLAS_TILES_PER_ROW blocks per block-row), same
     * layout GX_TRANSFER_OUT_TILED(1) used to produce -- confirmed by the
     * dirty-row byte-offset math elsewhere in this file (row*8*rowBytes)
     * already relying on exactly this ordering. */
    u32* blockBase = (u32*)sAtlasTexture.data + (size_t)slot * 64;

    for (int row = 0; row < 8; ++row) {
        int srcRow = vflip ? (7 - row) : row;
        u32 pixels[8];
        if (!bpp8) {
            const uint8_t* rowSrc = src + srcRow * 4;
            for (int col = 0; col < 8; ++col) {
                int srcCol = hflip ? (7 - col) : col;
                uint8_t byte = rowSrc[srcCol / 2];
                uint8_t idx = (srcCol & 1) ? (byte >> 4) & 0x0Fu : byte & 0x0Fu;
                pixels[col] = Bgr555ToRgba8(pal[palBank * 16 + idx], idx == 0);
            }
        } else {
            const uint8_t* rowSrc = src + srcRow * 8;
            for (int col = 0; col < 8; ++col) {
                int srcCol = hflip ? (7 - col) : col;
                uint8_t idx = rowSrc[srcCol];
                pixels[col] = Bgr555ToRgba8(pal[idx], idx == 0);
            }
        }
        if (brightAdjust == BRIGHT_ADJUST_BRIGHTEN) {
            for (int col = 0; col < 8; ++col) pixels[col] = ApplyBrighten(pixels[col], sBldEvy);
        } else if (brightAdjust == BRIGHT_ADJUST_DARKEN) {
            for (int col = 0; col < 8; ++col) pixels[col] = ApplyDarken(pixels[col], sBldEvy);
        }
        for (int col = 0; col < 8; ++col) blockBase[kSwizzleLUT[row * 8 + col]] = pixels[col];
    }
    memcpy(sCacheSourceBytes[slot], src, bpp8 ? 64u : 32u);
    sCachePalHash[slot] = palHash;
    sCacheEvy[slot] = (brightAdjust != BRIGHT_ADJUST_NONE) ? (uint8_t)sBldEvy : 0;

    int tileRow = slot / ATLAS_TILES_PER_ROW;
    sDirtyRowMask |= (1ull << tileRow);
    sAnyDirtySlot = true;
}

/* Returns the atlas slot for this tile, decoding it only if needed. Unlike
 * an earlier version of this cache, entries PERSIST across frames (see the
 * comment on sHashBucketHead above) -- a hash hit means this exact (offset,
 * bpp, palette bank, flip, brightness) combination was decoded on some
 * earlier frame, but the underlying VRAM bytes might have changed since
 * (animated tiles: water, lava, etc.), so a memcmp against the bytes
 * captured at that decode is still needed before trusting the cached
 * pixels. That memcmp (<=64 bytes) is far cheaper than a full redecode
 * (palette lookup + branches over 64 pixels), which is why this still wins
 * even though every reference (not just every unique tile) pays it. */
static int GetOrDecodeTileSlot(uint32_t byteOffset, bool bpp8, const uint16_t* pal, int palBank, bool hflip,
                               bool vflip, bool isObj, BrightAdjust brightAdjust) {
    TileCacheKey key = {
        byteOffset,       (uint8_t)bpp8,   (uint8_t)(bpp8 ? 0 : palBank), (uint8_t)hflip,
        (uint8_t)vflip,   (uint8_t)isObj,  (uint8_t)brightAdjust,
    };
    const uint8_t* src = gVram + byteOffset;
    const size_t tileBytes = bpp8 ? 64u : 32u;
    /* Precomputed once per frame (see sBgPalBankHash/sBgPalFullHash/
     * sObjPalBankHash/sObjPalFullHash) instead of hashing/comparing
     * palette bytes per reference -- see sCachePalHash's comment. */
    const uint32_t palHash =
        isObj ? (bpp8 ? sObjPalFullHash : sObjPalBankHash[palBank]) : (bpp8 ? sBgPalFullHash : sBgPalBankHash[palBank]);
    const uint8_t curEvy = (brightAdjust != BRIGHT_ADJUST_NONE) ? (uint8_t)sBldEvy : 0;
    uint32_t h = HashTileCacheKey(&key);
    for (int32_t i = sHashBucketHead[h]; i >= 0; i = sHashChainNext[i]) {
        if (!TileCacheKeyEqual(&sCacheKeys[i], &key)) continue;
        /* Stale if the underlying VRAM tile graphic changed (animated
         * tiles), or the palette bank's actual colors changed (rooms
         * commonly reuse the same tile shapes with a different palette
         * loaded into the same bank -- see sCachePalHash's comment), or --
         * for a brighten/darken tile -- evy changed since this exact slot
         * was last decoded (a fade ramping evy frame to frame). Any of the
         * three redecodes IN PLACE (same slot) rather than minting a new
         * one -- see TileCacheKey's comment for why the latter must never
         * allocate a fresh slot. */
        if (memcmp(sCacheSourceBytes[i], src, tileBytes) == 0 && sCachePalHash[i] == palHash && sCacheEvy[i] == curEvy) {
#ifdef PORT_GPU_RENDERER_DIAG_LOG
            if (isObj && sDiagObjSceneLog) {
                extern u16 gFrameCounter16Bit;
                char buf[128];
                snprintf(buf, sizeof(buf), "[F%u] OBJTILE HIT off=%05lX pb=%u hf=%u vf=%u ba=%u slot=%d palHash=%08lX evy=%u",
                         (unsigned)gFrameCounter16Bit, (unsigned long)byteOffset, palBank, hflip, vflip, brightAdjust, (int)i, (unsigned long)palHash, curEvy);
                Port_DebugLog(buf);
            }
#endif
            return i;
        }
#ifdef PORT_GPU_RENDERER_DIAG_LOG
        if (isObj && sDiagObjSceneLog) {
            extern u16 gFrameCounter16Bit;
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "[F%u] OBJTILE STALE off=%05lX pb=%u hf=%u vf=%u ba=%u slot=%d oldPalHash=%08lX newPalHash=%08lX oldEvy=%u newEvy=%u srcMatch=%d",
                     (unsigned)gFrameCounter16Bit, (unsigned long)byteOffset, palBank, hflip, vflip, brightAdjust, (int)i, (unsigned long)sCachePalHash[i],
                     (unsigned long)palHash, sCacheEvy[i], curEvy, memcmp(sCacheSourceBytes[i], src, tileBytes) == 0);
            Port_DebugLog(buf);
        }
#endif
        DecodeTileIntoSlot(i, src, bpp8, pal, palBank, hflip, vflip, brightAdjust, palHash);
        return i;
    }
    if (sCacheCount >= ATLAS_MAX_SLOTS) {
        /* Cache exhausted (pathological frame with far more unique tiles
         * than the atlas holds) -- reuse slot 0 rather than overrun. Wrong
         * pixels for the overflowing tiles only, not a crash; extremely
         * unlikely given ATLAS_MAX_SLOTS=4096 vs. a realistic frame's few
         * hundred unique tiles, and the near-full proactive reset at the
         * top of Port_GpuRenderer_RenderFrame keeps this from being reached
         * by slow accumulation over a long play session. */
#ifdef PORT_GPU_RENDERER_DIAG_LOG
        if (isObj && sDiagObjSceneLog) Port_DebugLog("OBJTILE OVERFLOW -- reusing slot 0");
#endif
        return 0;
    }
    int slot = sCacheCount++;
    sCacheKeys[slot] = key;
    sHashChainNext[slot] = sHashBucketHead[h];
    sHashBucketHead[h] = slot;
#ifdef PORT_GPU_RENDERER_DIAG_LOG
    if (isObj && sDiagObjSceneLog) {
        extern u16 gFrameCounter16Bit;
        char buf[128];
        snprintf(buf, sizeof(buf), "[F%u] OBJTILE NEW off=%05lX pb=%u hf=%u vf=%u ba=%u slot=%d palHash=%08lX cacheCount=%d",
                 (unsigned)gFrameCounter16Bit, (unsigned long)byteOffset, palBank, hflip, vflip, brightAdjust, slot, (unsigned long)palHash, sCacheCount);
        Port_DebugLog(buf);
    }
#endif
    DecodeTileIntoSlot(slot, src, bpp8, pal, palBank, hflip, vflip, brightAdjust, palHash);
    return slot;
}

static inline int AllocDrawItem(int slot, int sortKey) {
    if (sDrawItemCount >= MAX_DRAW_ITEMS) return -1;
    int idx = sDrawItemCount++;
    DrawItem* item = &sDrawItems[idx];
    item->img.tex = &sAtlasTexture;
    item->img.subtex = &sSlotSubtexTable[slot];
    item->sortKey = sortKey;

    sBucketNext[idx] = -1;
    if (sBucketHead[sortKey] < 0) sBucketHead[sortKey] = idx;
    else sBucketNext[sBucketTail[sortKey]] = idx;
    sBucketTail[sortKey] = idx;
    return idx;
}

static inline void PushItem(int slot, float x, float y, int sortKey, int depthTier, bool blendAlpha,
                            WindowVis winVis) {
    int idx = AllocDrawItem(slot, sortKey);
    if (idx < 0) return;
    DrawItem* item = &sDrawItems[idx];
    item->x = x;
    item->y = y;
    item->w = 8.0f;
    item->h = 8.0f;
    item->angle = 0.0f;
    item->depthTier = (int8_t)depthTier;
    item->blendAlpha = blendAlpha;
    item->affine = false;
    item->winVis = winVis;
}

/* Affine OBJ variant: x,y is the subtile's already-transformed screen-space
 * CENTER (not top-left), w/h are the decomposed scaled subtile size (see
 * CollectSprite's affine path), and angle (radians) is the sprite's
 * decomposed rotation -- consumed by the draw loop via C2D_DrawParams'
 * center+angle instead of the plain top-left placement non-affine items use. */
static inline void PushAffineItem(int slot, float centerX, float centerY, float angle, float scaleX, float scaleY,
                                  int sortKey, int depthTier, bool blendAlpha, WindowVis winVis) {
    int idx = AllocDrawItem(slot, sortKey);
    if (idx < 0) return;
    DrawItem* item = &sDrawItems[idx];
    item->x = centerX;
    item->y = centerY;
    item->w = 8.0f * scaleX;
    item->h = 8.0f * scaleY;
    item->angle = angle;
    item->depthTier = (int8_t)depthTier;
    item->blendAlpha = blendAlpha;
    item->affine = true;
    item->winVis = winVis;
}

/* Text-mode BG tilemap addressing, byte-identical to the formula validated
 * in port/ppu/src/mode1.c (screen_block_x/y + blocks_per_row quadrant
 * layout for the 32x32/64x32/32x64/64x64 GBA screen sizes). */
static void CollectBgLayer(int bgIndex) {
    /* Two independent, mutually-exclusive window-clip sources tag/gate this
     * layer differently: rectWinVis (WIN0/WIN1) is carried on each pushed
     * item as a tag, resolved later at DRAW time via the PICA200 scissor
     * test (see ItemPassesWindow); objWinVis (OBJWIN) is instead resolved
     * per-TILE right here at COLLECTION time via ObjWinItemVisible/
     * sObjWinCovered (see sObjWindowActive's comment for why). Whole-layer
     * NEVER (from either source) skips collection entirely. */
    WindowVis rectWinVis = sWindowActive ? sLayerWinVis[bgIndex] : WIN_VIS_ALWAYS;
    if (rectWinVis == WIN_VIS_NEVER) return;
    WindowVis objWinVis = sObjWindowActive ? sLayerWinVis[bgIndex] : WIN_VIS_ALWAYS;
    if (objWinVis == WIN_VIS_NEVER) return;

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
    int startTileX = scrollX / 8;
    int startTileY = scrollY / 8;
    int fineX = scrollX % 8;
    int fineY = scrollY % 8;

    /* World tile shown at screen (0,0), de-wrapped from the 9-bit BG scroll
     * via the camera (PortPpuMzm_ScreenOrigin). The per-block layer
     * corrections (port_layer_fixes.c) are keyed to ABSOLUTE room-block
     * positions, so screen tile (tx,ty) has to be turned back into a room
     * block before the lookup -- matching on the raw screenmap position
     * aliased every correction onto a 32x16-block lattice (an entry for
     * block (9,43) also fired on (9,59)). Every room layer a correction can
     * target scrolls with the camera 1:1, so one origin serves them all.
     * Only computed when a list is actually compiled in. */
    int fixOriginTileX = 0, fixOriginTileY = 0;
    if (PortLayerFix_ActiveCount() > 0) {
        extern void PortPpuMzm_ScreenOrigin(int* outX, int* outY);
        int originX = 0, originY = 0;
        PortPpuMzm_ScreenOrigin(&originX, &originY);
        fixOriginTileX = originX >> 3;
        fixOriginTileY = originY >> 3;
    }

    for (int ty = 0; ty <= 20; ++ty) {
        int tileRow = (startTileY + ty) & (mapHeightTiles - 1);
        int screenBlockY = tileRow / 32;
        int localRow = tileRow % 32;
        for (int tx = -1; tx <= 31; ++tx) {
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
            float drawX = (float)(tx * 8 - fineX);
            float drawY = (float)(ty * 8 - fineY);
            if (drawY <= -8.0f || drawY >= 160.0f || drawX <= -16.0f || drawX >= 248.0f) continue;

            uint32_t byteOffset = charBase + (uint32_t)tileId * bytesPerTile;
            if (!TileHasOpaquePixel(byteOffset, bpp8)) continue;
            if (sObjWindowActive && !ObjWinItemVisible(objWinVis, drawX, drawY)) continue;

            int slot = GetOrDecodeTileSlot(byteOffset, bpp8, pal, palBank, hflip, vflip, false, brightAdjust);

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
            /* Determine depthTier. Parallax has to follow the SAME ordering
             * the 2D compositor uses, which on GBA is BGCNT priority -- not
             * the BG index.
             *
             * This used to force bgIndex 0 to tier 3 (+1.8f, nearest of all)
             * on the assumption that BG0 is always the HUD/dialog/text
             * overlay. That holds on menu and text screens; it is flatly
             * wrong during gameplay, where BG0 is just another world layer
             * whose priority the room picks. Crateria room 8 is the case
             * that exposed it: BG0CNT=0x4005 (priority 1) carries the big
             * Chozo statue backdrop while BG1CNT=0x4204 (priority 0) carries
             * the platforms. Samus is OBJ priority 1, which beats a
             * priority-1 BG (equal priority -> OBJ wins), so she correctly
             * draws OVER the statue in 2D -- but the +1.8f tier shoved that
             * same statue in front of her the moment the 3D slider came up.
             * Confirmed from an on-device IO dump of that room.
             *
             * So: the BG0-is-an-overlay rule now applies only outside real
             * gameplay (GM_INGAME == 4, include/constants/game_state.h),
             * which is where BG0 genuinely is the text/dialog layer. In
             * gameplay every BG, BG0 included, maps by its own priority. */
            int depthTier = PortStereoDepth_BgTier(&sDepthState, bgIndex);
            /* Hand-picked exceptions, if a correction list was built in.
             *
             * The block moves in BOTH senses: its depth plane AND its 2D draw
             * order. Changing only the depth was tried first and is not what
             * the corrections are for -- a tile the room paints over Samus
             * still painted over her, just flat, so the thing being corrected
             * stayed on screen. These entries exist precisely because the
             * room's own layering is wrong for a stereo image, so honouring
             * half of it fixes nothing. */
            if (PortLayerFix_ActiveCount() > 0) {
                int dest = PortLayerFix_DestFor(bgIndex, (fixOriginTileX + tx) >> 1,
                                                (fixOriginTileY + ty) >> 1);
                if (dest >= 0) {
                    if (dest >= 4) {
                        /* Sprite level, which the workbench offers as "in
                         * front of everything": the frontmost bucket, above
                         * every BG and every sprite. */
                        depthTier = PortStereoDepth_ObjTier(&sDepthState, 1);
                        sortKey = (3 - 0) * 10 + 4;
                    } else {
                        depthTier = PortStereoDepth_BgTier(&sDepthState, dest);
                        sortKey = (3 - sDepthState.priority[dest]) * 10 + (3 - dest);
                    }
                }
            }
            PushItem(slot, drawX, drawY, sortKey, depthTier, blendAlpha, rectWinVis);
        }
    }
}

/* Multi-tile OBJ blit: walks the sprite's width/height in 8x8 tiles and
 * pushes one atlas quad per subtile, honoring 1D/2D OBJ char mapping
 * (DISPCNT bit 6) and whole-sprite hflip/vflip (subtile position AND
 * texture flip both mirror, matching hardware). Affine OBJs (attr0 bit8) are
 * handled below via a per-subtile rotation+scale decomposition -- see the
 * comment further down. */
static void CollectSprite(int oamIndex, bool obj1D) {
    const uint16_t* oam = (const uint16_t*)gOamMem;
    uint16_t attr0 = oam[oamIndex * 4 + 0];
    uint16_t attr1 = oam[oamIndex * 4 + 1];
    uint16_t attr2 = oam[oamIndex * 4 + 2];

    bool isAffine = ((attr0 >> 8) & 1u) != 0u;
    if (((attr0 >> 9) & 1u) && !isAffine) return; /* disabled (non-affine hidden bit) */
    uint8_t objMode = (uint8_t)((attr0 >> 10) & 3u);
    if (objMode == 2) return; /* OBJ window: not a drawable sprite */

    uint8_t shape = (uint8_t)((attr0 >> 14) & 3u);
    uint8_t size = (uint8_t)((attr1 >> 14) & 3u);
    if (shape == 3) return; /* prohibited shape value */
    int width = kObjWidths[shape][size];
    int height = kObjHeights[shape][size];

    /* Double-size (attr0 bit9, only meaningful when affine): the sprite's
     * on-screen bounding box doubles so a rotated/scaled sprite has room to
     * grow without clipping against its own unrotated footprint (GBATek
     * 6.4.4). The sprite's own texture dimensions (width/height above) are
     * unaffected -- only the placement/culling box and the affine pivot
     * change. */
    bool doubleSize = isAffine && (((attr0 >> 9) & 1u) != 0u);
    int boundsWidth = doubleSize ? width * 2 : width;
    int boundsHeight = doubleSize ? height * 2 : height;

    int y = attr0 & 0xFFu;
    if (y >= 160) y -= 256;
    int x = (int)(attr1 & 0x1FFu);
    if (x >= 240) x -= 512;

    bool bpp8 = ((attr0 >> 13) & 1u) != 0;
    /* attr1 bits12-13 are hflip/vflip only for non-affine OBJs -- for
     * affine OBJs those same bits are the top two bits of the 5-bit affine
     * parameter group index (attr1 bits9-13) instead, and flipping is
     * instead expressed via the sign of the affine matrix itself (GBATek
     * 6.4.4), so no separate flip here. */
    bool hflip = !isAffine && (((attr1 >> 12) & 1u) != 0u);
    bool vflip = !isAffine && (((attr1 >> 13) & 1u) != 0u);
    uint16_t baseTile = attr2 & 0x3FFu;
    uint8_t priority = (uint8_t)((attr2 >> 10) & 3u);
    uint8_t palBank = (uint8_t)((attr2 >> 12) & 0x0Fu);

    if (y >= 160 || y + boundsHeight <= 0 || x >= 240 || x + boundsWidth <= 0) return;

    /* See CollectBgLayer's comment: rectWinVis tags items for the WIN0/WIN1
     * draw-time scissor mechanism; objWinVis is resolved per-subtile right
     * here via ObjWinItemVisible/sObjWinCovered instead. */
    WindowVis rectWinVis = sWindowActive ? sLayerWinVis[4] : WIN_VIS_ALWAYS;
    if (rectWinVis == WIN_VIS_NEVER) return;
    WindowVis objWinVis = sObjWindowActive ? sLayerWinVis[4] : WIN_VIS_ALWAYS;
    if (objWinVis == WIN_VIS_NEVER) return;

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

    /* Priority inverted for the same reason as CollectBgLayer above
     * (0=highest/on top, 3=lowest/backmost). OBJ draws above any BG of
     * equal priority (tiebreak=4, higher than any BG's 0-3), and lower OAM
     * index draws above higher index at equal priority -- callers iterate
     * OAM back-to-front (127..0) so a stable sort already preserves that
     * via insertion order. */
    int sortKey = (3 - priority) * 10 + 4;

    float affM00 = 1.0f, affM01 = 0.0f, affM10 = 0.0f, affM11 = 1.0f, affAngle = 0.0f;
    float affScaleX = 1.0f, affScaleY = 1.0f;
    float pivotX = 0.0f, pivotY = 0.0f;
    if (isAffine) {
        /* OBJ affine parameter groups reuse OAM's normally-unused 4th
         * halfword (each OAM entry is 4 halfwords: attr0,1,2,pad) across 4
         * consecutive entries -- group g's PA/PB/PC/PD live in that pad
         * halfword of OAM entries g*4+0..3 respectively (GBATek 6.4.4),
         * fixed-point 8.8 signed. Same values port/ppu/src/mode1.c's affine
         * OBJ path reads (its own mode1_memory.oam_mem layout differs, but
         * this project's gOamMem mirrors real GBA OAM layout directly, same
         * as the non-affine attr0/1/2 reads just above). */
        int affineGroup = (int)((attr1 >> 9) & 0x1Fu);
        int16_t pa = (int16_t)oam[(affineGroup * 4 + 0) * 4 + 3];
        int16_t pb = (int16_t)oam[(affineGroup * 4 + 1) * 4 + 3];
        int16_t pc = (int16_t)oam[(affineGroup * 4 + 2) * 4 + 3];
        int16_t pd = (int16_t)oam[(affineGroup * 4 + 3) * 4 + 3];
        float mPa = (float)pa / 256.0f, mPb = (float)pb / 256.0f;
        float mPc = (float)pc / 256.0f, mPd = (float)pd / 256.0f;

        /* GBA's affine OBJ matrix is a BACKWARD (screen -> texture) mapping
         * used for per-pixel sampling on real hardware (same formula
         * mode1.c's software path applies: texRel = M * screenRel, see its
         * affine OBJ pixel loop). This is a forward per-quad renderer, so
         * the matrix needs inverting to get screenRel = M^-1 * texRel
         * instead, placing each subtile at its correctly transformed screen
         * position. */
        float det = mPa * mPd - mPb * mPc;
        if (det > -0.0001f && det < 0.0001f) {
            /* Degenerate (non-invertible) matrix -- extremely rare
             * (effectively zero scale on one axis); draw nothing rather
             * than risk a divide-by-near-zero blowing up subtile positions
             * off-screen. */
            return;
        }
        affM00 = mPd / det;
        affM01 = -mPb / det;
        affM10 = -mPc / det;
        affM11 = mPa / det;

        /* Decompose the inverted matrix into rotation + independent x/y
         * scale for C2D_DrawParams (which only supports rotation about a
         * pivot plus axis-aligned w/h scale, no general shear) -- exact for
         * the common rotate+uniform-scale case (explosions, zoom effects;
         * see docs/3ds-gpu-renderer-window-affine-mosaic-feasibility-2026-08-21.md's
         * "confirmed occurring" note), an approximation only when the
         * source matrix also carries shear (rare in practice). Applying the
         * FULL (non-decomposed) matrix to each subtile's pivot-relative
         * center below keeps subtile PLACEMENT exact regardless of shear --
         * only each subtile's own shape can't shear via C2D_DrawParams. */
        affAngle = atan2f(affM10, affM00);
        affScaleX = sqrtf(affM00 * affM00 + affM10 * affM10);
        affScaleY = sqrtf(affM01 * affM01 + affM11 * affM11);
        /* Preserve a pure axis flip (negative determinant, e.g. mirrored
         * affine sprites) as a negated Y scale rather than losing it into
         * an extra 180-degree rotation from atan2 alone. */
        if (affM00 * affM11 - affM01 * affM10 < 0.0f) affScaleY = -affScaleY;

        pivotX = (float)(x + boundsWidth / 2);
        pivotY = (float)(y + boundsHeight / 2);
    }

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
            if (!TileHasOpaquePixel(byteOffset, bpp8)) continue;

            /* GM_MAP_SCREEN == 5 (include/constants/game_state.h's GameMode
             * enum) covers every PauseScreenXxx variant -- plain map, chozo
             * hint, map download, item pickup -- not just gameplay. Tier 6
             * only applies there so real in-game Samus/enemy depth is
             * untouched. */
            extern s16 gMainGameMode;
            bool inMapOrPauseScreen = gMainGameMode == 5;
            /* HUD sprites are exactly the OAM slots HudUpdateOam wrote
             * this frame -- it fills OAM from slot 0 and runs before every
             * sprite system in in_game.c's frame (SpriteDrawAll_*,
             * ParticleProcessAll, ProjectileDrawAll_*, SamusDraw), so the
             * count it publishes is a real boundary. See port_hud_oam.c.
             *
             * Three earlier signals for "this is real HUD, elevate it" each
             * failed on hardware:
             *  - OAM priority 0: also catches explosions, shot impacts,
             *    reload flashes and bombs, which use priority 0 as a
             *    "draw above everything" tool, not because they are HUD.
             *  - gNextOamSlot: a running cursor every sprite system keeps
             *    advancing all frame, so by the time it was read it covered
             *    Samus, enemies and save/map stations. Reading oamSlot
             *    INSIDE HudUpdateOam instead is what makes this work.
             *  - Palette bank 4/5 plus sprite shape: classifies a sprite by
             *    how it LOOKS rather than by what drew it. Bank 4 is not
             *    exclusively HUD in the real ROM data (the Morph Ball bomb
             *    sprite sits there too, which is why a shape test was bolted
             *    on), and the 2026-08-28 recording has a pulsing item orb on
             *    bank 4 in the middle of the play field. It happened to fall
             *    the right side of the shape test; nothing guaranteed it.
             *
             * Gated on gameplay so a stale count cannot leak into a mode
             * that never calls HudDraw: the map/pause branch above already
             * handles GM_MAP_SCREEN, and everywhere else these are world
             * sprites. */
            bool isRealHud = gMainGameMode == 4 && oamIndex < Port_Hud_GetOamCount();
            int depthTier;
            if (inMapOrPauseScreen) {
                depthTier = (priority == 0) ? 5 : 6;
            } else if (isRealHud) {
                depthTier = 5;
            } else {
                /* World sprites: parallax must follow the same ordering the
                 * 2D compositor already uses. An OBJ of priority p draws in
                 * front of BGs whose priority is >= p and BEHIND those with
                 * a lower priority, so a high-priority-number sprite has to
                 * get a farther offset too -- otherwise a sprite the BGs
                 * paint over still appears nearest to the viewer in stereo.
                 * Priority 0/1 keeps tier 4's tuned -0.8f (Samus, enemies,
                 * particles); 2 and 3 map to the new intermediate tiers. */
                depthTier = PortStereoDepth_ObjTier(&sDepthState, priority);
            }
            if (!isAffine) {
                float drawX = (float)(x + tx * 8);
                float drawY = (float)(y + ty * 8);
                if (drawY <= -8.0f || drawY >= 160.0f || drawX <= -8.0f || drawX >= 240.0f) continue;
                if (sObjWindowActive && !ObjWinItemVisible(objWinVis, drawX, drawY)) continue;
                int slot = GetOrDecodeTileSlot(byteOffset, bpp8, pal, palBank, hflip, vflip, true, brightAdjust);
#ifdef PORT_GPU_RENDERER_DIAG_LOG
                if (sDiagObjSceneLog) {
                    extern u16 gFrameCounter16Bit;
                    char buf[128];
                    snprintf(buf, sizeof(buf), "[F%u] COLLECT off=%05lX slot=%d drawX=%.0f drawY=%.0f hf=%u vf=%u pb=%u",
                             (unsigned)gFrameCounter16Bit, (unsigned long)byteOffset, slot, drawX, drawY, hflip, vflip, palBank);
                    Port_DebugLog(buf);
                }
#endif
                PushItem(slot, drawX, drawY, sortKey, depthTier, blendAlpha, rectWinVis);
                continue;
            }

            int slot = GetOrDecodeTileSlot(byteOffset, bpp8, pal, palBank, hflip, vflip, true, brightAdjust);

            /* Subtile center relative to the sprite's own (unrotated)
             * texture pivot, then mapped through the FULL inverted affine
             * matrix (not the rotation/scale decomposition) to get this
             * subtile's exact transformed center in screen space -- see the
             * big comment above for why this keeps placement exact even
             * when the source matrix carries shear. */
            float relX = (float)(tx * 8 + 4) - (float)width * 0.5f;
            float relY = (float)(ty * 8 + 4) - (float)height * 0.5f;
            float dx = affM00 * relX + affM01 * relY;
            float dy = affM10 * relX + affM11 * relY;
            float screenCenterX = pivotX + dx;
            float screenCenterY = pivotY + dy;
            if (sObjWindowActive && !ObjWinItemVisible(objWinVis, screenCenterX, screenCenterY)) continue;
            PushAffineItem(slot, screenCenterX, screenCenterY, affAngle, affScaleX, affScaleY, sortKey, depthTier, blendAlpha,
                           rectWinVis);
        }
    }
}

/* Frames outside this scope fall back to the CPU renderer (port/ppu) for
 * that frame rather than drawing them wrong: forced blank (DISPCNT bit7 --
 * real hardware shows a blank white screen and skips all BG/OBJ rendering
 * entirely while this is set; a common trick during scene transitions/loads
 * to hide VRAM being rewritten -- ignoring it meant briefly rendering
 * whatever half-updated VRAM content existed at the exact moment a
 * transition hit, which lines up with "the image was cut/corrupted right as
 * a scene changed" from testing), affine BG (GBA mode != 0, the Tourian
 * self-destruct sequence per docs/3ds-port-ppu-audit.md), BOTH WIN0 and WIN1
 * simultaneously clipping the screen (a single active window -- WIN0 xor
 * WIN1 -- that actually clips IS now supported via the PICA200 scissor test,
 * see Port_GpuRenderer_RenderFrame's window-clip pass; two independently-
 * shaped rects at once is a rect-minus-a-rect region, not expressible as a
 * single scissor rect, so still falls back), or BG/OBJ mosaic. OBJ window
 * (attr0 objMode==2 sprites, DISPCNT bit15) IS now supported, on its own
 * (not combined with an actually-clipping WIN0/WIN1 -- see the OBJWIN check
 * below), via a CPU-rasterized coverage grid resolved at collection time --
 * see CollectBgLayer/CollectSprite's objWinVis handling and
 * ComputeObjWinMask in Port_GpuRenderer_RenderFrame. Affine OBJ
 * (attr0 bit8 set, rotated/scaled items -- explosions, per the "What I
 * actually measured" section of docs/3ds-gpu-renderer-window-affine-mosaic-feasibility-2026-08-21.md)
 * is now handled directly in CollectSprite, not rejected here.
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
     * effects per layer, not to clip any region of the screen; that
     * full-screen-no-op case is allowed through unconditionally below (it
     * needs no scissor at all -- Port_GpuRenderer_RenderFrame's
     * sWindowActive stays false for it). A window that DOES shrink below
     * full screen (e.g. gSuitFlashEffect's shrunk WIN1 rect during the suit
     * flash, or file-select's reveal/wipe transition -- both confirmed via
     * the KEY_Y-marked play session, see the feasibility doc referenced
     * above) is now rendered via the PICA200 scissor test instead of falling
     * back, as long as only ONE of WIN0/WIN1 is doing the clipping -- see
     * the scope note in this function's header comment. */
    /* Multi-window clipping: WIN0 or WIN1 clipping alone is rendered via
     * the PICA200 scissor test (see Port_GpuRenderer_RenderFrame's
     * window-clip pass). OBJWIN alone is handled via a CPU-rasterized
     * coverage grid (ComputeObjWinMask). Neither of those needed a
     * fallback and still doesn't.
     *
     * The two cases below -- BOTH WIN0 and WIN1 actually clipping at once,
     * and BG mosaic -- are NOT implemented on the GPU path (no code here
     * combines two independently-shaped scissor rects into one, and
     * CollectBgLayer has no mosaic handling), unlike what an earlier
     * version of this comment claimed ("rendered natively on GPU"). Falls
     * back to the CPU rasterizer for both instead of silently drawing them
     * wrong, same as before this branch's rewrite removed these checks.
     * Not yet confirmed reachable in actual play (tracked in issue #15) --
     * restored defensively since there's no cost when they never fire, and
     * a real visual bug if they do and this doesn't fall back. */
    uint16_t win1h = (uint16_t)(gIoMem[0x42] | (gIoMem[0x43] << 8));
    uint16_t win1v = (uint16_t)(gIoMem[0x46] | (gIoMem[0x47] << 8));
    bool win0On = (dispcnt & (1u << 13)) != 0u;
    bool win1On = (dispcnt & (1u << 14)) != 0u;
    if (win0On && win1On) {
        uint16_t win0h = (uint16_t)(gIoMem[0x40] | (gIoMem[0x41] << 8));
        uint16_t win0v = (uint16_t)(gIoMem[0x44] | (gIoMem[0x45] << 8));
        bool win0Clips = !WindowCoversFullScreen(win0h, win0v);
        bool win1Clips = !WindowCoversFullScreen(win1h, win1v);
        if (win0Clips || win1Clips) REJECT("WIN0+WIN1");
    }

    uint16_t mosaic = (uint16_t)(gIoMem[0x4C] | (gIoMem[0x4D] << 8));
    if (mosaic != 0) {
        for (int bg = 0; bg < 4; ++bg) {
            uint16_t bgcnt = (uint16_t)(gIoMem[0x08 + bg * 2] | (gIoMem[0x09 + bg * 2] << 8));
            if ((dispcnt & (1u << (8 + bg))) && ((bgcnt >> 6) & 1u)) REJECT("mosaic BG");
        }
    }
    if ((mosaic >> 8) != 0u) {
        /* OBJ mosaic (attr0 bit12, GBATek 6.4.4): the header comment above
         * has always claimed this falls back alongside BG mosaic, but until
         * now nothing here actually looked at individual sprites' mosaic
         * bit -- only BGCNT's per-BG mosaic enable (restored above) was
         * checked. Any sprite using OBJ mosaic
         * (e.g. Kraid's head fading in via SPRITE_STATUS_MOSAIC during
         * KRAID_POSE_GO_UP, src/sprites_ai/kraid.c's KraidInit) was
         * silently drawn unmosaic'd by the GPU path below -- CollectSprite
         * has no mosaic handling at all -- instead of falling back to the
         * CPU rasterizer that renders it correctly. Confirmed via L+R+X
         * memory dump during the Kraid fight: the un-mosaic'd GPU draw and
         * a second, correctly-mosaic'd draw both landed on screen, reading
         * as a duplicated "two heads" sprite. OBJ mosaic size lives in
         * mosaic's high byte (bits 8-11 horizontal, 12-15 vertical);
         * either being non-zero means mosaic would visibly apply if any
         * enabled sprite requests it. */
        const uint16_t* oam = (const uint16_t*)gOamMem;
        for (int i = 0; i < 128; ++i) {
            uint16_t attr0 = oam[i * 4 + 0];
            bool isAffine = ((attr0 >> 8) & 1u) != 0u;
            bool disabled = ((attr0 >> 9) & 1u) != 0u && !isAffine;
            if (disabled) continue;
            if (((attr0 >> 12) & 1u) != 0u) REJECT("OBJ mosaic");
        }
    }

    /* Issue #17 workaround: Samus's death animation (SPOSE_DYING's
     * walljumpTimer flash phase, src/samus.c) renders as fragmented,
     * disconnected color blocks through this renderer on real hardware --
     * confirmed root cause NOT found after two full investigation sessions
     * (see docs/3ds-issue17-session-2026-08-25.md): tile decode content,
     * cache-slot bookkeeping, UV/draw placement, and CPU/GPU frame-overlap
     * synchronization were all individually verified correct on real
     * hardware and the bug persists regardless. The CPU scanline renderer
     * (port/ppu/src/mode1.c) has been confirmed correct for this exact
     * scene throughout -- that's how the bug was first isolated as
     * GPU-renderer-specific to begin with. Falling back for just this
     * scene's distinctive DISPCNT signature (only OBJ+WIN1 active, WIN1
     * used purely as a BLDCNT-effect gate per the comment above, no BG
     * layer at all -- confirmed via the same L+R+X/scene-recorder sessions
     * that diagnosed this bug) is a narrow, low-risk fix for the visible
     * symptom while the real GPU-side cause remains open. This DISPCNT
     * pattern is rare enough outside this scene (only other confirmed match
     * across two sessions of play was a static file-select Samus portrait,
     * itself cheap to render on CPU) that the perf cost of the fallback is
     * negligible. If a real fix for the GPU renderer itself is found later,
     * remove this check rather than leaving it as dead-but-harmless code --
     * it exists only to route around an unresolved bug, not because this
     * DISPCNT shape is inherently unsupported. */
    if ((dispcnt & 0xF00u) == 0u && (dispcnt & (1u << 12)) != 0u) REJECT("issue #17 death-scene workaround");

    return true;
}
#undef REJECT

/* Renders the current GBA frame's tiles/sprites into both stereo top
 * targets (or just the left one when the 3D slider is off). Must run
 * inside a frame already opened with PlatformGpu3DS_BeginTopSceneGpu(); the
 * caller still finishes the frame with PlatformGpu3DS_EndBottom() as usual
 * (bottom screen is untouched here). */
/* ARM11 tick rate, same constant port_ppu_mzm.c's PORT_PPU_PERF_LOG uses
 * for its own (CPU-renderer-only, see that file's comment) timings --
 * needed here because NEITHER port_ppu_mzm.c's mode1 stats (only updated
 * when the GPU renderer is NOT used -- stale/leftover numbers otherwise,
 * not this frame's cost) NOR PlatformGpu3DS_GetStats's citro3d counters
 * (only cover actual GPU submission/texture-upload time) measure the CPU
 * cost of collection+hashing+decoding+sorting done in THIS function below
 * -- a real blind spot that made it impossible to tell whether previous
 * rounds of optimization here were even touching the actual bottleneck. */
#define PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC (268111856.0 / 1000.0)
static float sLastCollectMs, sLastDrawMs;
static float sLastTileCollectMs, sLastAtlasUploadMs; /* collectMs split in two, see below */

void Port_GpuRenderer_GetLastFrameTimingMs(float* outCollectMs, float* outDrawMs) {
    if (outCollectMs) *outCollectMs = sLastCollectMs;
    if (outDrawMs) *outDrawMs = sLastDrawMs;
}

/* Builds this item's C2D_DrawParams. Non-affine items keep the original
 * top-left placement (center at the origin, no rotation); affine OBJ
 * subtiles (item->affine) are instead centered on their already-transformed
 * screen position (item->x/y, see CollectSprite) and rotated about their own
 * center by item->angle -- see DrawItem's comment for why x/y/w/h mean
 * different things depending on this flag. */
extern bool Port_Config_GetShowFps(void);
extern int Port_Config_Get3DSAspectRatio(void);
extern int Port_Config_Get3DSDisplayStyle(void);
extern double Port_PPU_3DS_CurrentFps(void);

static inline C2D_DrawParams BuildDrawParams(const DrawItem* item, float screenBaseX, float screenBaseY, float eyeOffset, float scaleX, float scaleY) {
    C2D_DrawParams params;
    params.depth = 0.5f;
    if (item->affine) {
        float w = item->w * scaleX, h = item->h * scaleY;
        params.pos.x = screenBaseX + eyeOffset + item->x * scaleX - w * 0.5f;
        params.pos.y = screenBaseY + item->y * scaleY - h * 0.5f;
        params.pos.w = w;
        params.pos.h = h;
        params.center.x = w * 0.5f;
        params.center.y = h * 0.5f;
        params.angle = item->angle;
    } else {
        /* Snap the quad to whole device pixels, deriving the size from the
         * snapped edges rather than rounding the size on its own.
         *
         * Every 8x8 tile is its own quad, so tile N's right edge and tile
         * N+1's left edge are computed independently. At a non-integer scale
         * (the 400/240 = 1.6667 stretch modes: 8 * 1.6667 = 13.333px per
         * tile) those two edges land on different fractional positions and
         * the rasterizer rounds each one on its own -- dropping or
         * duplicating a column of pixels at tile boundaries. On 8px-wide
         * text glyphs that reads as a letter losing a stroke.
         *
         * Computing right = round(x + w) and then w = right - x guarantees
         * tile N's right edge IS tile N+1's left edge, so the tiling is
         * seamless whatever the scale. Individual tiles end up 13 or 14
         * device px wide, which is what an honest nearest-neighbour upscale
         * of a 13.333px tile looks like.
         *
         * Affine items are deliberately left unsnapped: they are rotated or
         * scaled sprites where snapping the bounding quad would quantize the
         * rotation, and they carry no text. */
        float left = screenBaseX + eyeOffset + item->x * scaleX;
        float top = screenBaseY + item->y * scaleY;
        float right = screenBaseX + eyeOffset + (item->x + item->w) * scaleX;
        float bottom = screenBaseY + (item->y + item->h) * scaleY;
        float sl = floorf(left + 0.5f);
        float st = floorf(top + 0.5f);
        params.pos.x = sl;
        params.pos.y = st;
        params.pos.w = floorf(right + 0.5f) - sl;
        params.pos.h = floorf(bottom + 0.5f) - st;
        params.center.x = 0.0f;
        params.center.y = 0.0f;
        params.angle = 0.0f;
    }
    return params;
}

/* Whether this item should be drawn in the current window-clip scissor pass
 * -- see the two-pass NORMAL/INVERT scheme in Port_GpuRenderer_RenderFrame's
 * draw loop (an ALWAYS item is drawn in BOTH passes; since the two passes'
 * scissor rects are exact complements of each other, each on-screen pixel
 * only ever survives from one of the two draws, so this does not
 * double-composite anything -- it just lets a single ALWAYS item span both
 * the inside and outside regions correctly). When no window is active every
 * item passes unconditionally (the common case, single unscissored pass). */
static inline bool ItemPassesWindow(bool windowActive, WindowVis winVis, bool insidePass) {
    if (!windowActive) return true;
    if (winVis == WIN_VIS_ALWAYS) return true;
    return insidePass ? (winVis == WIN_VIS_INSIDE_ONLY) : (winVis == WIN_VIS_OUTSIDE_ONLY);
}

void Port_GpuRenderer_RenderFrame(void) {
    if (!sInitialized) return;
    u64 tStart = svcGetSystemTick();

    /* The tile cache (sCacheKeys/sCacheCount/hash table) intentionally does
     * NOT reset here -- see the comment on sHashBucketHead. Only reclaim it
     * proactively when it's nearly full, at this safe frame boundary (never
     * mid-frame: a mid-frame reset would invalidate slot indices already
     * baked into this frame's earlier DrawItems via SlotToUV). Headroom
     * used to be a mere 256 slots, which a real hardware session blew
     * straight through: GPUDIAG logged cache=4096 (the hard ceiling) twice
     * in one play session, once during a full-screen darken fade -- a
     * SINGLE frame's worth of newly-seen (offset,flip,palBank,...) tile
     * identities can comfortably exceed 256 (up to ~2600 raw BG tile
     * references alone in the worst case, before OBJ), so 256 headroom
     * left this reset unable to fire before overflow hit mid-frame and
     * every tile past the limit silently aliased to slot 0's stale
     * content -- confirmed as the actual cause of "scenery/menus missing"
     * on hardware. Half the atlas (2048) as headroom comfortably absorbs
     * any single frame's worst case in practice. */
    if (sCacheCount >= ATLAS_MAX_SLOTS / 2) {
#ifdef PORT_GPU_RENDERER_DIAG_LOG
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "OBJTILE CACHE RESET at count=%d", sCacheCount);
            Port_DebugLog(buf);
        }
#endif
        for (int i = 0; i < HASH_BUCKETS; ++i) sHashBucketHead[i] = -1;
        sCacheCount = 0;
    }
    sDrawItemCount = 0;
    sAnyDirtySlot = false;
    sDirtyRowMask = 0;
    for (int i = 0; i < SORT_KEY_BUCKETS; ++i) sBucketHead[i] = -1;

    /* Palette hashes computed ONCE per frame here rather than per tile
     * reference -- see sCachePalHash's comment for why (used to be a
     * per-reference memcmp of up to 512 bytes, a real cost in GPUTIME
     * collectMs on hardware). gBgPltt/gObjPltt don't change mid-frame, so
     * every reference this frame can safely reuse these. */
    for (int b = 0; b < 16; ++b) {
        sBgPalBankHash[b] = HashBytes(gBgPltt + b * 32, 32);
        sObjPalBankHash[b] = HashBytes(gObjPltt + b * 32, 32);
    }
    sBgPalFullHash = HashBytes(gBgPltt, 512);
    sObjPalFullHash = HashBytes(gObjPltt, 512);

    uint16_t dispcnt = (uint16_t)(gIoMem[0] | (gIoMem[1] << 8));
    bool obj1D = (dispcnt & (1u << 6)) != 0;

#ifdef PORT_GPU_RENDERER_DIAG_LOG
    /* Issue #17 (Samus's death animation showing garbled sprites, GPU
     * renderer only -- confirmed by RENDERER=cpu build/hardware test): the
     * scene is DISPCNT=OBJ+WIN1 with WIN1 covering the whole screen (a
     * no-op, gating BLDCNT only) and NO BG layers at all -- rare enough
     * outside this one sequence that logging every OBJ tile slot lookup
     * only while this exact shape is on screen won't flood mzm-debug.log
     * during normal play. See GetOrDecodeTileSlot's use of sDiagObjSceneLog
     * below. */
    sDiagObjSceneLog = (dispcnt & 0xF00u) == 0u && (dispcnt & (1u << 12)) != 0u;
#endif

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

    /* Window-clip state (see sWindowActive's comment): exactly one of
     * WIN0/WIN1 clipping is the only case Port_GpuRenderer_CanRenderFrame
     * lets through (both simultaneously clipping still falls back), so at
     * most one of these branches ever fires. WIN0 takes priority when both
     * are enabled but neither clips (the ordinary full-screen WININ-gating
     * case) -- matches GBA hardware's WIN0-over-WIN1 priority for pixels
     * inside both, though here both cover the whole screen so priority
     * between them doesn't actually matter for clipping, only which mask
     * gates layers. */
    sWindowActive = false;
    {
        bool win0On = (dispcnt & (1u << 13)) != 0u;
        bool win1On = (dispcnt & (1u << 14)) != 0u;
        uint16_t winin = (uint16_t)(gIoMem[0x48] | (gIoMem[0x49] << 8));
        uint16_t winout = (uint16_t)(gIoMem[0x4A] | (gIoMem[0x4B] << 8));
        uint16_t winH = 0, winV = 0;
        uint8_t insideMask = 0, outsideMask = (uint8_t)(winout & 0xFFu);
        if (win0On) {
            winH = (uint16_t)(gIoMem[0x40] | (gIoMem[0x41] << 8));
            winV = (uint16_t)(gIoMem[0x44] | (gIoMem[0x45] << 8));
            insideMask = (uint8_t)(winin & 0xFFu);
        }
        if (win1On && (!win0On || WindowCoversFullScreen(winH, winV))) {
            winH = (uint16_t)(gIoMem[0x42] | (gIoMem[0x43] << 8));
            winV = (uint16_t)(gIoMem[0x46] | (gIoMem[0x47] << 8));
            insideMask = (uint8_t)((winin >> 8) & 0xFFu);
        }
        if ((win0On || win1On) && !WindowCoversFullScreen(winH, winV)) {
            sWindowActive = true;
            /* WINxH packs left in the high byte, right in the low byte;
             * WINxV packs top high, bottom low (GBATEK). Clamp to the GBA
             * screen bounds and to a non-negative width/height -- real
             * hardware treats a right<left or bottom<top rect as an empty
             * (zero-area) window rather than something to special-case
             * here. */
            int left = (int)(winH >> 8), right = (int)(winH & 0xFFu);
            int top = (int)(winV >> 8), bottom = (int)(winV & 0xFFu);
            if (left > 240) left = 240;
            if (right > 240) right = 240;
            if (top > 160) top = 160;
            if (bottom > 160) bottom = 160;
            if (right < left) right = left;
            if (bottom < top) bottom = top;
            sWinLeft = left;
            sWinTop = top;
            sWinRight = right;
            sWinBottom = bottom;
            for (int l = 0; l < 4; ++l) sLayerWinVis[l] = ComputeLayerWinVis(insideMask, outsideMask, l);
            sLayerWinVis[4] = ComputeLayerWinVis(insideMask, outsideMask, 4);
        }
    }

    /* OBJWIN state (see sObjWindowActive's comment): mutually exclusive
     * with sWindowActive by construction (Port_GpuRenderer_CanRenderFrame
     * rejects OBJWIN combined with an active WIN0/WIN1). WINOUT's high byte
     * (bits8-13) is the "inside the OBJWIN mask" per-layer enable -- the
     * OBJWIN counterpart to WININ's inside mask for WIN0/WIN1 -- while its
     * low byte (bits0-5, same bits sWindowActive's outsideMask already
     * reads above) is shared: "outside every active window" means the same
     * thing whether that window is a WIN0/WIN1 rect or the OBJWIN mask.
     * ComputeObjWinMask rasterizes the actual mask sprites into
     * sObjWinCovered BEFORE the BG/OBJ collection loops below run, since
     * CollectBgLayer/CollectSprite need it ready to resolve per-tile
     * visibility as they go. */
    sObjWindowActive = !sWindowActive && (dispcnt & (1u << 15)) != 0u;
    if (sObjWindowActive) {
        uint16_t winout = (uint16_t)(gIoMem[0x4A] | (gIoMem[0x4B] << 8));
        uint8_t insideMask = (uint8_t)((winout >> 8) & 0xFFu);
        uint8_t outsideMask = (uint8_t)(winout & 0xFFu);
        for (int l = 0; l < 4; ++l) sLayerWinVis[l] = ComputeLayerWinVis(insideMask, outsideMask, l);
        sLayerWinVis[4] = ComputeLayerWinVis(insideMask, outsideMask, 4);
        ComputeObjWinMask(obj1D);
    }

    /* Depth depends on register state the collection loops below read one
     * layer at a time, so it is snapshotted once, up front, for all of them. */
    ComputeDepthState(dispcnt);

    for (int bg = 3; bg >= 0; --bg) {
        if (dispcnt & (1u << (8 + bg))) CollectBgLayer(bg);
    }
    if (dispcnt & (1u << 12)) {
        for (int i = 127; i >= 0; --i) CollectSprite(i, obj1D);
    }
    sOpaqueCount = 0;
    sBlendCount = 0;
    sDrawOrderCount = 0;
    for (int b = 0; b < SORT_KEY_BUCKETS; ++b) {
        for (int32_t i = sBucketHead[b]; i >= 0; i = sBucketNext[i]) {
            /* sDrawOrder keeps every item (opaque and blend) in one true
             * back-to-front sortKey sequence -- see its declaration and the
             * draw loop for why this replaced two fully separate passes.
             * sOpaqueOrder/sBlendOrder are still built for the stats/diag
             * log below (sBlendCount) and are otherwise unused now. */
            sDrawOrder[sDrawOrderCount++] = i;
            if (sDrawItems[i].blendAlpha) {
                sBlendOrder[sBlendCount++] = i;
            } else {
                sOpaqueOrder[sOpaqueCount++] = i;
            }
        }
    }

    {
        int objItems = 0;
        for (int i = 0; i < sDrawItemCount; ++i) {
            if (sDrawItems[i].depthTier == 4 || sDrawItems[i].depthTier == 7 ||
                    sDrawItems[i].depthTier == 8 || sDrawItems[i].depthTier == 10) ++objItems;
        }
        sLastObjItemCount = objItems;
    }

#ifdef PORT_GPU_RENDERER_DIAG_LOG
    {
        static unsigned sDiagCounter;
        if ((sDiagCounter++ % 5u) == 0u) {
            char msg[512];
            int objItems = 0, cacheSlots = sCacheCount;
            float minY = 999.0f, maxY = -999.0f;
            for (int i = 0; i < sDrawItemCount; ++i) {
                if (sDrawItems[i].depthTier == 4 || sDrawItems[i].depthTier == 7 ||
                    sDrawItems[i].depthTier == 8 || sDrawItems[i].depthTier == 10) ++objItems;
                if (sDrawItems[i].y < minY) minY = sDrawItems[i].y;
                if (sDrawItems[i].y > maxY) maxY = sDrawItems[i].y;
            }
            int blendItems = sBlendCount;
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

    /* Split out from the rest of collectMs (see PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC's
     * comment) so a hardware session can tell apart tile collection/hash/
     * decode/sort cost from the atlas cache-flush below. Placed AFTER the
     * GPUDIAG block above, not before it: an earlier version of this
     * timestamp sat before that block, silently folding its
     * Port_DebugLog() call (real file I/O to the SD card, on 1-in-5
     * frames) into "upload" -- which stayed ~18-26ms across two different
     * flush-primitive fixes (section 15) simply because neither fix
     * touched the actual cost being measured. Moved here so "upload"
     * finally isolates just the flush call itself. */
    u64 tBeforeUpload = svcGetSystemTick();
    sLastTileCollectMs = (float)((double)(tBeforeUpload - tStart) / PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC);

    if (sAnyDirtySlot) {
        /* DecodeTileIntoSlot now writes swizzled texels straight into
         * sAtlasTexture.data (see its comment and kSwizzleLUT) -- no GX
         * transfer needed at all, just make sure the GPU sees the CPU's
         * writes via a plain cache flush. This is what actually removes
         * the ~19-20ms/frame blocking cost that C3D_SyncDisplayTransfer
         * had, independent of transferred size (see
         * docs/3ds-port-gpu-renderer-status-2026-08-20.md section 14) --
         * flushing each dirty row-run separately (same byte-offset math as
         * the old transfer-based version) rather than the whole atlas is
         * still worth doing, just far cheaper to begin with now. */
        size_t rowBytes = (size_t)ATLAS_DIM * sizeof(u32);
        uint64_t mask = sDirtyRowMask;
        while (mask != 0) {
            int startRow = __builtin_ctzll(mask);
            uint64_t shifted = mask >> startRow; /* bit 0 is guaranteed set */
            /* Run length = index of the first zero bit above startRow.
             * __builtin_ctzll(~shifted) is undefined for an all-ones input
             * in C, but shifted here is guaranteed non-zero and has at most
             * 64 - startRow valid bits, so ~shifted is never 0 for any
             * startRow > 0. For startRow == 0 and all 64 bits dirty,
             * runLength = 64 directly. */
            int runLength = (~shifted == 0) ? 64 : __builtin_ctzll(~shifted);
            u32* flushStart = (u32*)sAtlasTexture.data + (size_t)startRow * 8 * ATLAS_DIM;
            size_t flushBytes = (size_t)runLength * 8 * rowBytes;
            FlushAtlasRange(flushStart, flushBytes);
            mask &= ~(((1ull << runLength) - 1ull) << startRow);
        }
        sDirtyRowMask = 0;
    }

    u64 tAfterCollect = svcGetSystemTick();
    sLastCollectMs = (float)((double)(tAfterCollect - tStart) / PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC);
    sLastAtlasUploadMs = (float)((double)(tAfterCollect - tBeforeUpload) / PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC);

    float slider3d = osGet3DSliderState();
    const int style = Port_Config_Get3DSDisplayStyle();
    const int aspect = Port_Config_Get3DSAspectRatio();

    /* Emulated GBA main game mode (GM_INGAME == 4, GM_DEMO == 11, etc.) */
    extern s16 gMainGameMode;
    bool isGameplay = (gMainGameMode == 4 || gMainGameMode == 11);

    float scaleX = 1.5f;
    float scaleY = 1.5f;
    float screenBaseX = 20.0f;
    float screenBaseY = 0.0f;
    float nativeWidth = 240.0f;
    float nativeHeight = 160.0f;

    if (style == 0) { /* PIXEL PERFECT (1:1) */
        scaleX = 1.0f;
        scaleY = 1.0f;
        screenBaseX = 80.0f; /* (400 - 240) / 2 */
        screenBaseY = 40.0f; /* (240 - 160) / 2 */
    } else {
        if (aspect == 2) { /* STRETCH (400x240) */
            scaleX = 400.0f / 240.0f; /* 1.6666667f */
            scaleY = 1.5f;
            screenBaseX = 0.0f;
            screenBaseY = 0.0f;
        } else if (aspect == 0) { /* WIDE (16:9) */
            scaleX = 400.0f / 240.0f;
            scaleY = 1.5f;
            screenBaseX = 0.0f;
            screenBaseY = 0.0f;
        } else { /* ORIGINAL (3:2 / 360x240) */
            scaleX = 1.5f;
            scaleY = 1.5f;
            screenBaseX = 20.0f;
            screenBaseY = 0.0f;
        }
    }

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
        float eyeSign = (eye == 0) ? 1.0f : -1.0f;

        C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(target);
        ConfigureAtlasTextureEnv();
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
        C3D_AlphaTest(true, GPU_GREATER, 0);
        /* Opaque pass: all passing fragments have alpha=255. Use ONE/ZERO blend
         * to avoid reading back the destination framebuffer from VRAM, saving
         * massive memory bandwidth during scaled / full-screen rendering. */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

        /* Single pass over sDrawOrder (true back-to-front GBA priority
         * order, opaque and blend items interleaved), toggling the GPU
         * blend equation per item instead of running two fully separate
         * passes (all opaque, then unconditionally all blend on top).
         * The old two-pass split always painted every blend (first-target)
         * item on top of the ENTIRE opaque scene, regardless of its real
         * priority -- so a blend layer behind a higher-priority opaque
         * layer (e.g. pause_screen.c's chozo-hint screen, where the grid
         * is BG1 at priority 2 behind the map's BG3 at priority 1) painted
         * over that opaque layer instead of being occluded by it. Confirmed
         * against a real-hardware VRAM dump: the grid must respect BG3's
         * priority, not always win. */
        int drawCount = 0;
        bool reassertedTexEnv = false;
        int scissorPasses = sWindowActive ? 2 : 1;
        bool blendModeActive = false;
        for (int sp = 0; sp < scissorPasses; ++sp) {
            bool insidePass = (sp == 0);
            for (int oi = 0; oi < sDrawOrderCount; ++oi) {
                const DrawItem* item = &sDrawItems[sDrawOrder[oi]];
                if (!ItemPassesWindow(sWindowActive, item->winVis, insidePass)) continue;
                bool wantBlend = item->blendAlpha && sBldEffect == 1;
                if (wantBlend != blendModeActive) {
                    C2D_Flush();
                    if (wantBlend) {
                        C3D_BlendingColor(C2D_Color32(0, 0, 0, (u32)((sBldEva * 255) / 16)));
                        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA,
                                       GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA);
                    } else {
                        /* Opaque mode: ONE/ZERO, see the comment above the
                         * initial C3D_AlphaBlend call before this loop. */
                        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
                    }
                    blendModeActive = wantBlend;
                }
                /* Whole device pixels, rounded ONCE per tier per eye.
                 *
                 * A fractional parallax offset cannot be drawn: with
                 * GPU_NEAREST every tile quad rounds it independently, so
                 * part of a layer shifts by a pixel and part of it doesn't.
                 * Within one layer that tears glyphs apart -- and because
                 * the fractional part changes with the slider, WHICH glyphs
                 * tear changes as the slider moves, which is the "text gets
                 * cut when I change the depth" symptom exactly.
                 *
                 * Rounding here makes each tier shift rigidly, as one plane.
                 * The cost is that a tier whose offset never reaches half a
                 * pixel (BG priority 0's -0.3f at any slider position) now
                 * renders with no parallax at all -- but it never really had
                 * any: what it had was per-tile rounding noise that read as
                 * shimmer. */
                float eyeOffset = floorf(eyeSign * slider3d * PortStereoDepth_TierPx(item->depthTier) + 0.5f);
                C2D_DrawParams params = BuildDrawParams(item, screenBaseX, screenBaseY, eyeOffset, scaleX, scaleY);
#ifdef PORT_GPU_RENDERER_DIAG_LOG
                if (sDiagObjSceneLog && eye == 0) {
                    extern u16 gFrameCounter16Bit;
                    int slot = (int)(item->img.subtex - sSlotSubtexTable);
                    char buf[192];
                    snprintf(buf, sizeof(buf),
                             "[F%u] DRAW slot=%d x=%.1f y=%.1f w=%.1f h=%.1f uv=(%.4f,%.4f,%.4f,%.4f) angle=%.3f params.pos=(%.1f,%.1f,%.1f,%.1f)",
                             (unsigned)gFrameCounter16Bit, slot, item->x, item->y, item->w, item->h,
                             (double)item->img.subtex->left, (double)item->img.subtex->top,
                             (double)item->img.subtex->right, (double)item->img.subtex->bottom,
                             (double)item->angle, (double)params.pos.x, (double)params.pos.y,
                             (double)params.pos.w, (double)params.pos.h);
                    Port_DebugLog(buf);
                }
#endif
                C2D_DrawImage(item->img, &params, NULL);
                ++drawCount;
                if (!reassertedTexEnv) {
                    ConfigureAtlasTextureEnv();
                    reassertedTexEnv = true;
                }
            }
        }
        if (blendModeActive) {
            C2D_Flush();
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        }

        /* Draw solid black border masks over letterbox/pillarbox areas (covers any
         * tiles or 3D stereo parallax layers that extend beyond the GBA frame). */
        if (screenBaseX > 0.0f || screenBaseY > 0.0f) {
            float gameW = 240.0f * scaleX;
            float gameH = 160.0f * scaleY;
            float topY = screenBaseY;
            float bottomY = screenBaseY + gameH;
            float leftX = screenBaseX;
            float rightX = screenBaseX + gameW;

            if (leftX > 0.0f) {
                C2D_DrawRectSolid(0.0f, 0.0f, 0.6f, leftX, 240.0f, C2D_Color32(0, 0, 0, 255));
            }
            if (rightX < 400.0f) {
                C2D_DrawRectSolid(rightX, 0.0f, 0.6f, 400.0f - rightX, 240.0f, C2D_Color32(0, 0, 0, 255));
            }
            if (topY > 0.0f) {
                C2D_DrawRectSolid(0.0f, 0.0f, 0.6f, 400.0f, topY, C2D_Color32(0, 0, 0, 255));
            }
            if (bottomY < 240.0f) {
                C2D_DrawRectSolid(0.0f, bottomY, 0.6f, 400.0f, 240.0f - bottomY, C2D_Color32(0, 0, 0, 255));
            }
        }

        if (Port_Config_GetShowFps()) {
            char label[20];
            double fps = Port_PPU_3DS_CurrentFps();
            unsigned rounded = fps > 0.0 ? (unsigned)(fps + 0.5) : 0u;
            if (rounded > 999u) rounded = 999u;
            snprintf(label, sizeof(label), "FPS %u", rounded);
            float fpsEyeOffset = eyeSign * slider3d * (+1.0f);
            C2D_DrawRectSolid(5.0f + fpsEyeOffset, 216.0f, 0.7f, 65.0f, 18.0f, C2D_Color32(0, 0, 0, 200));
            PlatformGpu3DS_DrawStatusText(8.0f + fpsEyeOffset, 220.0f, 1.5f, label);
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

    u64 tEnd = svcGetSystemTick();
    sLastDrawMs = (float)((double)(tEnd - tAfterCollect) / PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC);

#ifdef PORT_GPU_RENDERER_DIAG_LOG
    /* The only place in this codebase that actually measures this
     * renderer's own CPU-side cost -- see the comment on
     * PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC above for why neither the
     * PORT_PPU_PERF_LOG numbers in port_ppu_mzm.c nor citro3d's own
     * gpuDraw/gpuProc counters cover it. collectMs = tileMs (VRAM reads +
     * tile cache hashing/memcmp/decoding + bucket sort) + uploadMs (the
     * atlas GPU transfer, C3D_SyncDisplayTransfer -- blocks the CPU, split
     * out separately since real DMA/bus latency on hardware isn't
     * something Azahar models accurately); drawMs = both eyes' C2D_DrawImage
     * submission. Compare each against the 16.67ms/frame budget for 60 FPS. */
    {
        static unsigned sTimingLogCounter;
        if ((sTimingLogCounter++ % 30u) == 0u) {
            char msg[112];
            snprintf(msg, sizeof(msg), "GPUTIME collectMs=%.2f(tile=%.2f upload=%.2f) drawMs=%.2f",
                     (double)sLastCollectMs, (double)sLastTileCollectMs, (double)sLastAtlasUploadMs,
                     (double)sLastDrawMs);
            Port_DebugLog(msg);
        }
    }
#endif
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

/* Issue #17 diagnosis: dumps the atlas texture verbatim (PPM, RGB8, no
 * alpha) plus a CSV of every populated slot's cache key, so a session can
 * SEE whether the corruption already exists inside the atlas (a decode/
 * swizzle bug -- DecodeTileIntoSlot wrote the wrong pixels) or only shows up
 * at draw time (atlas is clean, but UV/slot addressing or draw-call state
 * picks the wrong region of it). Reads sAtlasTexture.data directly (already
 * CPU-visible, same memory DecodeTileIntoSlot writes into -- see
 * Port_GpuRenderer_Init's comment) rather than reading back through the GPU,
 * so this reflects exactly what's resident right now, independent of
 * whether a draw call has run yet. */
void Port_GpuRenderer_DumpAtlas(const char* ppmPath, const char* csvPath) {
    FILE* f = fopen(ppmPath, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", ATLAS_DIM, ATLAS_DIM);
        const u32* px = (const u32*)sAtlasTexture.data;
        for (int y = 0; y < ATLAS_DIM; ++y) {
            for (int x = 0; x < ATLAS_DIM; ++x) {
                int tileCol = x / 8, tileRow = y / 8;
                int slot = tileRow * ATLAS_TILES_PER_ROW + tileCol;
                int localX = x % 8, localY = y % 8;
                u32 texel = px[(size_t)slot * 64 + kSwizzleLUT[localY * 8 + localX]];
                uint8_t r = (uint8_t)(texel & 0xFFu);
                uint8_t g = (uint8_t)((texel >> 8) & 0xFFu);
                uint8_t b = (uint8_t)((texel >> 16) & 0xFFu);
                uint8_t rgb[3] = { r, g, b };
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
    }

    FILE* c = fopen(csvPath, "w");
    if (c) {
        fprintf(c, "slot,byteOffset,bpp8,palBank,hflip,vflip,isObj,brightAdjust,palHash,evy\n");
        for (int slot = 0; slot < sCacheCount; ++slot) {
            const TileCacheKey* k = &sCacheKeys[slot];
            fprintf(c, "%d,0x%05lX,%u,%u,%u,%u,%u,%u,0x%08lX,%u\n", slot, (unsigned long)k->byteOffset,
                    k->bpp8, k->palBank, k->hflip, k->vflip, k->isObj, k->brightAdjust,
                    (unsigned long)sCachePalHash[slot], sCacheEvy[slot]);
        }
        fclose(c);
    }
}
