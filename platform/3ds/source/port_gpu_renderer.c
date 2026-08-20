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
static int32_t sSortedOrder[MAX_DRAW_ITEMS];
static int sSortedCount;

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
            return i;
        }
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
        return 0;
    }
    int slot = sCacheCount++;
    sCacheKeys[slot] = key;
    sHashChainNext[slot] = sHashBucketHead[h];
    sHashBucketHead[h] = slot;
    DecodeTileIntoSlot(slot, src, bpp8, pal, palBank, hflip, vflip, brightAdjust, palHash);
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

    int idx = sDrawItemCount - 1;
    sBucketNext[idx] = -1;
    if (sBucketHead[sortKey] < 0) sBucketHead[sortKey] = idx;
    else sBucketNext[sBucketTail[sortKey]] = idx;
    sBucketTail[sortKey] = idx;
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
    sSortedCount = 0;
    for (int b = 0; b < SORT_KEY_BUCKETS; ++b) {
        for (int32_t i = sBucketHead[b]; i >= 0; i = sBucketNext[i]) sSortedOrder[sSortedCount++] = i;
    }

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
             * (~shifted == 0), which happens exactly when every remaining
             * row from startRow to 63 is dirty -- handled directly instead. */
            int runLen = (shifted == ~0ull) ? (64 - startRow) : __builtin_ctzll(~shifted);
            uint64_t clearMask = (runLen >= 64) ? ~0ull : (((1ull << runLen) - 1ull) << startRow);
            mask &= ~clearMask;

            int height = runLen * 8;
            size_t byteOffset = (size_t)startRow * 8 * rowBytes;
            FlushAtlasRange((uint8_t*)sAtlasTexture.data + byteOffset, (size_t)height * rowBytes);
        }
    }

    u64 tAfterCollect = svcGetSystemTick();
    sLastCollectMs = (float)((double)(tAfterCollect - tStart) / PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC);
    sLastAtlasUploadMs = (float)((double)(tAfterCollect - tBeforeUpload) / PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC);

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
        for (int oi = 0; oi < sSortedCount; ++oi) {
            const DrawItem* item = &sDrawItems[sSortedOrder[oi]];
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
            for (int oi = 0; oi < sSortedCount; ++oi) {
                const DrawItem* item = &sDrawItems[sSortedOrder[oi]];
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
