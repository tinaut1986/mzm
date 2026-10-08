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
#include "port_cutscene_depth.h"
#include "port_affine_subtile.h"   /* shared with tools/affine_probe */
#include "port_layer_fixes.h"
#include "port_sprite_depth_oam.h"
#include "port_haze_3ds.h"
#include "port_gba_bezel.h"
#include "port_wide_view.h"
#include "platform_gpu_3ds.h"
#include "port_debug_tools.h" /* PORT_DEBUG_TOOLS_ACTIVE */

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include "tilequad_shbin.h" /* generated from source/tilequad.v.pica */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef PORT_DEBUG_TOOLS_ACTIVE
#include <stdio.h>
/* Route every diagnostic line in this file through the GPU log stream: it is
 * buffered (per-frame path, not a boot/hang checkpoint -- see
 * port_debug_log.h, and section 16 of
 * docs/3ds-port-gpu-renderer-status-2026-08-20.md for why the unbuffered
 * fopen/fwrite/fclose cost mattered) AND it only writes when DEBUG ->
 * HERRAMIENTAS -> LOG mode is ALL or GPU, so a session chasing e.g. PBFLASH
 * is not drowned by audio/perf lines. */
#include "port_debug_log.h"
#define Port_DebugLog Port_DebugLog_Gpu
#endif

/* Emulated GBA memory buffers (port/port_gba_mem.c). */
extern uint8_t gIoMem[];
extern uint8_t gVram[];
extern uint8_t gBgPltt[];
extern uint8_t gObjPltt[];
extern uint8_t gOamMem[];

/* Power-bomb explosion state, for the issue #28 circular-flash pass below.
 * Declared as raw bytes rather than via structs/power_bomb_explosion.h so
 * this file keeps not including any GBA-port header that would drag in the
 * conflicting u32 typedef alongside <3ds.h> (see port_ppu_mzm.c's header
 * note). Field offsets mirror struct PowerBomb
 * (include/structs/power_bomb_explosion.h):
 *   [0] u8  animationState (PB_STATE_EXPLODING=3, PB_STATE_IMPLODING=4)
 *   [2] u8  semiMinorAxis  (grows 4..159 while expanding, in screen px)
 *   [4] u16 xPosition      (sub-pixel world X of the epicentre)
 *   [6] u16 yPosition      (sub-pixel world Y) */
extern uint8_t gCurrentPowerBomb[];
extern uint16_t gBg1XPosition;
extern uint16_t gBg1YPosition;

static bool sGpuRendererActive = false;
static bool sInitialized = false;

/* The atlas (and the affine BG2 texture) are GPU_RGBA5551, packed by
 * Bgr555ToRgba5551 in the order the PICA200 reads that format, so they sample
 * straight: colour and alpha pass through from texture 0. (They used to be
 * RGBA8 with the channels in reverse byte order, which took a three-stage
 * TEV to put back together.) Index-0 palette entries carry alpha 0 and are
 * dropped by the alpha test. */
static void ConfigureAtlasTextureEnv(void) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvInit(C3D_GetTexEnv(2));
}

/* Debug: flat-colour every layer/sprite by its stereo tier (see sDepthTint /
 * Port_GpuRenderer_SetDepthTint). RGB comes straight from the TEV constant,
 * which the draw loop sets per tier; alpha still comes from the source so a
 * layer keeps its silhouette and transparent pixels are AlphaTest'd away.
 * Both sources now carry straight alpha (the atlas is RGBA5551); the two
 * functions stay separate so the draw loop's per-source switch is unchanged. */
static void ConfigureDepthTintAtlasTexEnv(void) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, C2D_Color32(255, 0, 255, 255)); /* draw loop overrides per tier */
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvInit(C3D_GetTexEnv(2));
}
static void ConfigureDepthTintPlainTexEnv(void) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, C2D_Color32(255, 0, 255, 255));
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvInit(C3D_GetTexEnv(2));
}

/* Tier -> flat colour, same palette as the layer workbench's TIER_MARKER_COLOR
 * (tools/layer-workbench/index.html) so the on-device view and the PC tool
 * read alike. Index by PORT_TIER_* (port_stereo_depth.h). Literal ABGR8888
 * (0xAABBGGRR) since C2D_Color32 is a function, not constant-foldable here. */
static const u32 kDepthTintColor[PORT_TIER_COUNT] = {
    0xffd68f5bu, /* 0 BG_FAR     #5b8fd6 */
    0xff52ae6fu, /* 1 BG_MID     #6fae52 */
    0xff3ba1e9u, /* 2 BG_PLAY    #e9a13b */
    0xff4f4fd2u, /* 3 BG_OVERLAY #d24f4f */
    0xfff2f2f2u, /* 4 OBJ_P1     #f2f2f2 */
    0xffd66fb0u, /* 5 OBJ_HUD    #b06fd6 */
    0xffc7c74fu, /* 6 OBJ_MAP    #4fc7c7 */
};

/* Plain pass-through texenv: output = texture0, verbatim, single stage.
 * Used by the issue #29 strip blit. sHazeTex is a GPU render target: the
 * rasterizer writes GPU_RGBA8 in the byte order that re-samples straight
 * (the RGBA5551 atlas now samples straight too -- see
 * ConfigureAtlasTextureEnv, which is the same pass-through). */
static void ConfigurePlainTextureEnv(void) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvInit(C3D_GetTexEnv(2));
}

/* BLDCNT effect applied at decode time for brighten/darken (effect 1, alpha
 * blend, is applied later at draw time via GPU blending instead -- see
 * kBlendAlpha in DrawItem). NONE/BRIGHTEN/DARKEN affect the palette-to-RGBA
 * conversion itself, so they're part of what makes a decoded tile unique.
 * BG layers are the exception: their tiles are decoded plain and the effect
 * is applied on the GPU when they are drawn (ConfigureFxTextureEnv). */
typedef enum { BRIGHT_ADJUST_NONE, BRIGHT_ADJUST_BRIGHTEN, BRIGHT_ADJUST_DARKEN } BrightAdjust;

/* ---- Batched quads -------------------------------------------------------
 * The scene's items used to go through C2D_DrawImage one by one, which works
 * out rotation, per-corner tints and texture state for every call: measured
 * on an Old 3DS at ~6 us a quad, 5-8 ms a frame in WIDE. The items need none
 * of that -- straight quads (or plainly rotated ones), no tint, a texenv set
 * by the caller -- so they are written here as four vertices each into one
 * buffer and drawn with a minimal shader (tilequad.v.pica), one draw call per
 * run of the same texture / blend / texenv state.
 *
 * Mirrors C2D_DrawImage's geometry exactly (see BatchQuad) so nothing moves.
 * Used only for the main item loop; everything else keeps using citro2d, and
 * BatchEnd hands the GPU state back to it with C2D_Prepare. */
/* tier: the item's stereo tier, which picks this eye's horizontal offset
 * from the shader's tierOffset[] (BATCH_TIER_NONE: no offset). */
typedef struct { float x, y, u, v, tier; } BatchVertex;
enum { BATCH_TIERS = 8, BATCH_TIER_NONE = BATCH_TIERS - 1 };
enum { BATCH_MAX_QUADS = 12288 }; /* both eyes of a full item table */
static BatchVertex* sBatchVerts;  /* linear, 4 per quad, rewritten every frame */
static u16* sBatchIndices;        /* linear, 6 per quad, fixed */
static DVLB_s* sBatchDvlb;
static shaderProgram_s sBatchProgram;
static int sBatchProjectionLoc;
static int sBatchTierOffsetLoc;
/* Device px added to x per tier, for the eye being drawn; zero for targets
 * that are not an eye. Uploaded with the projection (citro2d's shader uses
 * the same uniform registers). */
static float sBatchTierOffset[BATCH_TIERS];
static C3D_AttrInfo sBatchAttr;
static C3D_BufInfo sBatchBuf;
static bool sBatchReady;
static int sBatchUsed;            /* quads written this frame */
/* Replaying: BatchQuad only advances over quads an earlier pass wrote (the
 * second eye re-issuing the first eye's quads, see the eye loop). */
static bool sBatchReplay;
static int sBatchDrawn;           /* quads already submitted */
static const C3D_Tex* sBatchTex;

static bool BatchInit(void) {
    sBatchVerts = (BatchVertex*)linearAlloc(BATCH_MAX_QUADS * 4 * sizeof(BatchVertex));
    sBatchIndices = (u16*)linearAlloc(BATCH_MAX_QUADS * 6 * sizeof(u16));
    if (!sBatchVerts || !sBatchIndices) return false;
    for (int q = 0; q < BATCH_MAX_QUADS; ++q) {
        u16* i = sBatchIndices + q * 6;
        const u16 v = (u16)(q * 4);
        i[0] = v; i[1] = (u16)(v + 1); i[2] = (u16)(v + 2);
        i[3] = v; i[4] = (u16)(v + 2); i[5] = (u16)(v + 3);
    }
    GSPGPU_FlushDataCache(sBatchIndices, BATCH_MAX_QUADS * 6 * sizeof(u16));

    sBatchDvlb = DVLB_ParseFile((u32*)tilequad_shbin, tilequad_shbin_size);
    if (!sBatchDvlb) return false;
    shaderProgramInit(&sBatchProgram);
    shaderProgramSetVsh(&sBatchProgram, &sBatchDvlb->DVLE[0]);
    sBatchProjectionLoc = shaderInstanceGetUniformLocation(sBatchProgram.vertexShader, "projection");
    sBatchTierOffsetLoc = shaderInstanceGetUniformLocation(sBatchProgram.vertexShader, "tierOffset");
    if (sBatchProjectionLoc < 0 || sBatchTierOffsetLoc < 0) return false;

    AttrInfo_Init(&sBatchAttr);
    AttrInfo_AddLoader(&sBatchAttr, 0, GPU_FLOAT, 2); /* v0: position */
    AttrInfo_AddLoader(&sBatchAttr, 1, GPU_FLOAT, 2); /* v1: texcoord */
    AttrInfo_AddLoader(&sBatchAttr, 2, GPU_FLOAT, 1); /* v2: tier */
    BufInfo_Init(&sBatchBuf);
    BufInfo_Add(&sBatchBuf, sBatchVerts, sizeof(BatchVertex), 3, 0x210);
    return true;
}

/* Start of a frame's drawing: the GPU has finished the previous frame
 * (C3D_FrameBegin waited for it), so the vertex buffer is free to reuse. */
static void BatchNewFrame(void) {
    sBatchUsed = 0;
    sBatchDrawn = 0;
}

/* Switch the GPU over from citro2d to the batch for a run of quads drawn
 * into the target the current scene is on: a width x height target, tilted
 * for a screen (stored rotated) -- the same projection citro2d builds. */
static void BatchBeginTarget(float width, float height, bool tilt) {
    C2D_Flush();
    C3D_BindProgram(&sBatchProgram);
    C3D_SetAttrInfo(&sBatchAttr);
    C3D_SetBufInfo(&sBatchBuf);
    C3D_Mtx projection;
    if (tilt) Mtx_OrthoTilt(&projection, 0.0f, width, height, 0.0f, 1.0f, -1.0f, true);
    else Mtx_Ortho(&projection, 0.0f, width, height, 0.0f, 1.0f, -1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sBatchProjectionLoc, &projection);
    for (int t = 0; t < BATCH_TIERS; ++t)
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sBatchTierOffsetLoc + t, sBatchTierOffset[t], 0.0f, 0.0f, 0.0f);
    C3D_CullFace(GPU_CULL_NONE);
    sBatchTex = NULL;
    sBatchDrawn = sBatchUsed;
}

/* The per-tier offsets the next BatchBeginTarget uploads; NULL for none. */
static void BatchSetTierOffsets(const float* offsets) {
    for (int t = 0; t < BATCH_TIERS; ++t) sBatchTierOffset[t] = offsets ? offsets[t] : 0.0f;
}

/* The top screen, 400x240. */
static void BatchBegin(void) {
    BatchBeginTarget(400.0f, 240.0f, true);
}

/* Submit the quads written since the last flush. Call before any GPU state
 * change (blend, texenv) so the change applies to the right quads. */
static void BatchFlush(void) {
    if (sBatchUsed > sBatchDrawn) {
        C3D_DrawElements(GPU_TRIANGLES, (sBatchUsed - sBatchDrawn) * 6, C3D_UNSIGNED_SHORT,
                         sBatchIndices + sBatchDrawn * 6);
        sBatchDrawn = sBatchUsed;
    }
}

/* One item, with C2D_DrawImage's own geometry: the rectangle offset by
 * -center, rotated by angle about the origin, then moved to pos; texture
 * corners from the (non-rotated) subtexture. `tier` picks the eye offset
 * (BATCH_TIER_NONE for none). False when the buffer is full. While
 * replaying, p and st are not read (NULL is fine): the quad is already
 * there. */
static bool BatchQuad(const C3D_Tex* tex, const Tex3DS_SubTexture* st, const C2D_DrawParams* p, int tier) {
    if (sBatchUsed >= BATCH_MAX_QUADS) return false;
    if (tex != sBatchTex) {
        BatchFlush();
        C3D_TexBind(0, (C3D_Tex*)tex);
        sBatchTex = tex;
    }
    if (sBatchReplay) {
        ++sBatchUsed;
        return true;
    }
    const float x0 = -p->center.x, y0 = -p->center.y;
    const float x1 = x0 + p->pos.w, y1 = y0 + p->pos.h;
    float px[4] = { x0, x0, x1, x1 }; /* TL, BL, BR, TR */
    float py[4] = { y0, y1, y1, y0 };
    if (p->angle != 0.0f) {
        const float s = sinf(p->angle), c = cosf(p->angle);
        for (int k = 0; k < 4; ++k) {
            const float rx = px[k] * c - py[k] * s;
            const float ry = py[k] * c + px[k] * s;
            px[k] = rx;
            py[k] = ry;
        }
    }
    BatchVertex* v = sBatchVerts + sBatchUsed * 4;
    const float t = (float)tier;
    v[0] = (BatchVertex){ p->pos.x + px[0], p->pos.y + py[0], st->left, st->top, t };
    v[1] = (BatchVertex){ p->pos.x + px[1], p->pos.y + py[1], st->left, st->bottom, t };
    v[2] = (BatchVertex){ p->pos.x + px[2], p->pos.y + py[2], st->right, st->bottom, t };
    v[3] = (BatchVertex){ p->pos.x + px[3], p->pos.y + py[3], st->right, st->top, t };
    ++sBatchUsed;
    return true;
}

/* Back to citro2d for whatever the scene draws next. C2D_Prepare rebinds its
 * shader and buffers and resets texenv and depth test; the depth test is put
 * back the way the eye pass runs (off). */
static void BatchEnd(void) {
    BatchFlush();
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
}

/* The CPU wrote this frame's vertices through the data cache; the GPU reads
 * memory. Once, after the last eye. */
static void BatchFrameDone(void) {
    if (sBatchUsed > 0)
        GSPGPU_FlushDataCache(sBatchVerts, (u32)sBatchUsed * 4u * sizeof(BatchVertex));
}

/* Tile atlas: every unique (source tile bytes + palette bank + hflip/vflip)
 * combination actually visible this frame gets decoded into one 8x8 slot,
 * shared by BG and OBJ (they live in disjoint gVram ranges in mode 0, so a
 * byte-offset-based key can't collide between them, but isObj is still part
 * of the key for clarity/robustness rather than relying on that). 4096
 * slots of 8x8 -- comfortably above a worst-case frame (4 BGs at
 * ~33x21 visible tiles each, worst case ~2772 tiles, realistically far
 * fewer unique combos due to tile reuse). */
/* The atlas: 1024x256, 128 x 32 slots of 8x8 = 4096 tile slots (see
 * DecodeTileIntoSlot: a slot is 64 contiguous texels, slots run row-major
 * with ATLAS_TILES_PER_ROW per row). RGBA5551, 512KB of linear memory. It
 * used to be 1024 tall, the rest holding 16x16 and 32x32 block regions for
 * the block passes that drew BG layers before the layer maps replaced them.
 *
 * One texture, deliberately: citro2d rebinds (and therefore breaks the
 * batch) whenever the texture pointer changes between draws, and the draw
 * order interleaves layers by priority. */
enum {
    ATLAS_W = 1024,
    ATLAS_H = 256,
    ATLAS_TILES_PER_ROW = ATLAS_W / 8,
    ATLAS_SLOT_ROWS = ATLAS_H / 8,
    ATLAS_MAX_SLOTS = ATLAS_TILES_PER_ROW * ATLAS_SLOT_ROWS,
    /* Sized for the WIDE view, which collects every layer over a 400x240
     * area instead of 240x160. */
    MAX_DRAW_ITEMS = 6144,
};


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
    /* Affine subtiles only: which of this subtile's four edges face another
     * subtile of the SAME sprite (bit0 left, bit1 right, bit2 top, bit3
     * bottom, in the sprite's texture-grid orientation). BuildDrawParams
     * grows the quad outward only on those edges, to overlap the neighbour
     * and hide the per-subtile rounding gap, while leaving the sprite's
     * OUTER silhouette edges exactly where they were. */
    uint8_t affBleedEdges;
    WindowVis winVis;
    bool isHud;
    /* Stays put on the screen instead of moving with the world -- the HUD and
     * the overlay text. Only matters when the WIDE view slides the world
     * (see ComputeWideView). */
    bool screenFixed;
    /* The quad samples a render target (a layer map) rather than the atlas.
     * Both are pass-through texenvs now; the draw loop still switches on it,
     * which costs a batch break per switch -- a handful per eye. */
    bool plainEnv;
    /* ITEM_FX_* applied at draw time (ConfigureFxTextureEnv): BG tiles are
     * decoded without the frame's brightness fade or palette fade. */
    uint8_t gpuFx;
} DrawItem;

static C3D_Tex sAtlasTexture;
/* One atlas texel: GPU_RGBA5551 (see Bgr555ToRgba5551). */
typedef uint16_t AtlasTexel;
static TileCacheKey sCacheKeys[ATLAS_MAX_SLOTS];
static int sCacheCount;
static DrawItem sDrawItems[MAX_DRAW_ITEMS];
/* Added to every BG item's position while one layer is collected: the part of
 * the view's slide that layer does NOT take (see WideLayerShift). Zero for
 * sprites and whenever WIDE is off. */
static float sCollectOffX, sCollectOffY;
static int sDrawItemCount;
static bool sAnyDirtySlot;
static int sLastObjItemCount;

/* --- issue #29: per-scanline BG3 ripple (water / lava / acid / heat haze) ---
 *
 * The GBA rewrites REG_BG3HOFS every scanline via an HBlank DMA; this port
 * has neither HBlank DMA nor per-line IO, so BG3 renders flat. Option C:
 * render BG3 once into an offscreen texture at the frame-level scroll, then
 * blit it to the screen as 160 one-scanline horizontal strips, each shifted
 * by rowDelta[y] from port_haze_3ds. BG3 is the backmost layer
 * in every affected room, so the strip blit is laid down first (opaque) and
 * the normal BG0-2 / OBJ pass composites on top -- including BG0's alpha
 * blend, which reads BG3 back from the framebuffer.
 *
 * Only the single-layer BG3 case exists in the shipped game (every
 * EFFECT_WATER / LAVA / WEAK_ACID / STRONG_ACID / LAVA_HEAT_HAZE room);
 * PortHaze_Bg3RowScroll() returns false for anything else and BG3 then
 * renders normally (flat) as before. */
/* 512 wide so the WIDE view fits: 400 px plus HAZE_MARGIN on each side.
 * 256 rows hold its 240. A plain frame just uses the top-left 256x160. */
enum {
    HAZE_RT_W = 512, HAZE_RT_H = 256, HAZE_MARGIN = 8,
    /* WIDE Pixel Perfect reaches 54 tile columns by 32 rows (see
     * CollectHazeBg3); the plain frame needs 34 by 21. */
    HAZE_MAX_TILES = 56 * 33,
};
/* Double-buffered: frame N renders BG3 into sHaze*[N&1^...] and the eye
 * passes SAMPLE the other buffer -- the one rendered last frame, so a full
 * C3D_FrameEnd/FrameBegin has flushed it and both eyes see identical,
 * complete data (an in-frame render-to-texture then sample raced the GPU
 * write: one eye got 8x8-block garbage). sHazeCur = buffer the eyes read
 * this frame; the offscreen pass writes sHazeCur^1, then sHazeCur flips at
 * end of frame. */
static C3D_Tex sHazeTex[2];
static C3D_RenderTarget *sHazeRT[2];
static bool sHazeRtReady;
static int sHazeCur;
static bool sHazeBufReady[2];

/* --- Affine BG2 (GBA mode 1) -- see Port_GpuRenderer_SetAffineBg ---------
 * The Tourian-escape "Samus surrounded" sub-scene is the one frame class MZM
 * puts in mode 1 (DISPCNT 0x1501: BG0 text + BG2 affine + OBJ). Measured
 * (docs/3ds-gpu-affine-bg-and-obj-seams-feasibility-2026-09-09.md): a 256x256
 * BG2, overflow-transparent, PURE SCALE (PB=PC=0), zoom only, no rotation,
 * matrix written once per VBlank. So: CPU-decode the 256x256 8bpp affine
 * tilemap into one texture (same Bgr555ToRgba5551 + swizzle path the atlas
 * uses), draw it as ONE scaled quad at BG2's priority, with the usual
 * per-tier stereo eye offset. DetectAffineBg2() gates on exactly that
 * config; anything else in mode 1+ still falls back to the CPU renderer. */
#define AFF_BG2_DIM 256
static C3D_Tex  sAffineBg2Tex;
static bool     sAffineBg2TexReady;
static bool     sAffineBg2Active;         /* this frame is the supported case */
static bool     sAffineBg2ComposePending; /* ComposeAffineBg2 due in the draw half */
static float    sAffineBg2InvScale;       /* screen px per texture px (256/PA) */
static float    sAffineBg2RefX, sAffineBg2RefY;      /* BG2X/BG2Y, texture px */
static uint32_t sAffineBg2CharBase, sAffineBg2ScreenBase; /* gVram byte offsets */
static int16_t sHazeBakedRowDelta[2][HAZE_RT_H]; /* per-line shift baked with each buffer */
/* Geometry each buffer was baked with: rows used, and the WIDE margins the
 * bake extends past the GBA frame (0 on a plain frame). The ripple and the
 * blit read these, not the current frame's, because they draw the buffer
 * baked LAST frame. */
static int sHazeBakedRows[2];
static int sHazeBakedMarginX[2], sHazeBakedMarginY[2];
static bool sHazeActive; /* recomputed per frame in Port_GpuRenderer_RenderFrame */
static int16_t sHazeRowDelta[HAZE_RT_H];
static int sHazeRows = 160;
static int sHazeMarginX, sHazeMarginY;
/* This frame's BG3 comes from its layer map (sLmTex[3]) instead of the
 * per-tile bake, and where in it: the scroll the strips start from and how
 * far the bake reaches past the GBA frame on the left / top. */
static bool sHazeFromMap;
static int sHazeMapScrollX, sHazeMapScrollY, sHazeMapEL, sHazeMapET;
static int16_t sHazeBakeHofs;
static Tex3DS_SubTexture sHazeStripSubtex; /* mutated per strip by HazeBlitStrips */
typedef struct { C2D_Image img; float x, y; } HazeTile;

/* ---- Haze mode 3: ripple once into a target, blit one quad per eye ------
 *
 * The per-scanline blit is 6 ms of the frame -- measured 2026-09-05 by
 * splitting the pass three ways: composing the 640-660 tiles is only 1,27 ms
 * of it, and the rest is the 160 strips, drawn TWICE because each eye blits
 * them itself.
 *
 * So do the ripple once, offscreen, and give each eye a single quad. Two
 * things get smaller at once: the strip count halves (160 per frame instead
 * of 160 per eye), and each strip shrinks from 240x1 GBA pixels scaled onto
 * the screen (360x1,5 at 3:2) to 240x1 in a 256x256 target.
 *
 * Written and sampled in the same frame, so it needs C3D_FrameSplit -- the
 * double-buffering trick the compose uses is not available here without
 * putting the ripple a frame behind the scene it belongs to. That sync is
 * the risk in this change, and the reason it is a MODE rather than the
 * default: if the split costs more than the strips saved, the numbers will
 * say so. */
static C3D_Tex sHazeRippleTex;
static C3D_RenderTarget* sHazeRippleRT;
static bool sHazeRippleReady;
static Tex3DS_SubTexture sHazeRippleSubtex;

/* Set by Port_GpuRenderer_InvalidateAll (save-state load); forces the
 * RenderFrame settle window even with area/room/mode unchanged. */
static int sForcedSettleFrames;
/* Set by a settle-window frame, which empties the tile cache: the next frame
 * does it again, so it must wait for the GPU (Port_GpuRenderer_CollectNeedsIdleGpu). */
static bool sSettleResetPending;

/* ---- Layer maps: each BG layer kept in a wrapping render target ----------
 *
 * On an Old 3DS the collection was bound by walking thousands of tiles every
 * frame through cold memory (no L2): ~20 us per 32x32 group, 7-9 ms a frame
 * in WIDE, however cheap each step was made. The layer map stops redoing
 * that work. Each text BG layer lives in a 512x256 render target that works
 * the way the GBA's own tilemap does: it wraps (GPU_REPEAT), layer pixel
 * (x, y) sits at texel (x mod 512, y mod 256), and every 8x8 cell remembers
 * which tilemap entry it holds, the frame it was drawn in and the layer
 * state it was drawn under. Each frame only the cells whose entry, tile
 * pixels (the VRAM change stamps) or palette bank changed are redrawn -- the
 * column scrolling in, an animated tile, a broken block -- and the layer is
 * drawn on screen as one quad.
 *
 * 512x256 holds the widest view there is (WIDE Pixel Perfect: 400 px + a
 * tile each side for the stereo shift, by 240 + a tile), so a cell is never
 * needed twice at once. RGBA5551 like the atlas: lossless for GBA colours,
 * and half the VRAM. Tiles that carry a per-tile correction (layer fix,
 * visible tank, door depth) are left transparent here and drawn as their own
 * items, as before. */
enum {
    LM_W = 512, LM_H = 256,
    LM_COLS = LM_W / 8, LM_ROWS = LM_H / 8,
    LM_MAX_OPS = LM_COLS * LM_ROWS,
    LM_TRANSPARENT_SLOT = ATLAS_MAX_SLOTS - 1, /* never allocated: stays all zero */
    LM_CELL_CORRECTED = 1,                      /* cell kept transparent on purpose */
};
typedef struct {
    uint32_t stamp;  /* frame it was drawn in (full width: a wrapped 16-bit
                      * stamp reads as "changed since" for half its cycle) */
    uint16_t entry;  /* tilemap entry drawn into the cell */
    uint16_t flags;  /* LM_CELL_* */
    int16_t slot;    /* atlas slot its tile was drawn from ... */
    uint16_t slotGen; /* ... under this sAtlasGen (a cache reset reassigns slots) */
    uint16_t used;   /* colour indices that tile uses (see sCacheUsedMask) */
    uint16_t pad;
} LayerMapCell;
/* Bumped whenever the tile cache is emptied, which reassigns every slot. */
static uint16_t sAtlasGen = 1;
/* Tile slots already known current this frame (bit per slot, cleared at the
 * start of each collection): a palette step reaches every cell of its bank,
 * and the slot only needs checking -- and redecoding -- for the first one. */
static uint32_t sSlotFreshThisFrame[ATLAS_MAX_SLOTS / 32];
typedef struct {
    uint32_t charBase;
    uint8_t bpp8;
} LayerMapState;
static C3D_Tex sLmTex[4];
static C3D_RenderTarget* sLmRT[4];
static bool sLmReady[4];
static LayerMapCell sLmCells[4][LM_ROWS][LM_COLS];
/* Frame the layer's tile data base or brightness last changed in: anything
 * drawn before it is stale. (Also a stamp rather than a counter, so it
 * cannot wrap around onto an old value.) */
static uint32_t sLmStateStamp[4];
static LayerMapState sLmState[4];
static uint32_t sLmBankHash[4][17];      /* per bank as last seen; [16] = full palette */
/* Low 16 bits of the frame each bank last changed colour in. A stamp, not a
 * per-frame "changed" flag: a cell that is out of view when its bank changes
 * (a room fading in from black) must still see the change when it scrolls
 * back in, or it keeps the old colours -- black tiles, until something else
 * redraws them. */
static uint32_t sLmBankStamp[4][17];
static uint16_t sLmOps[4][LM_MAX_OPS];   /* cells to redraw: row * LM_COLS + col */
static int16_t sLmOpSlot[4][LM_MAX_OPS]; /* atlas slot for each */
static int sLmOpCount[4];
/* What the layer map looked at last frame, so the next one only has to look
 * at what can have changed (see CollectBgLayer). */
typedef struct {
    bool valid;
    uint32_t key;              /* everything that forces a full look when it changes */
    int startTileX, startTileY;
    int txLo, txHi, tyLo, tyHi; /* visible tile rect, screen tile coordinates */
    int rollRow;               /* next row of the rolling full re-check */
    uint32_t frame;            /* frame stamp it was taken on: must be the previous
                                * one, or changes in between went unseen */
} LayerMapScan;
static LayerMapScan sLmScan[4];
static Tex3DS_SubTexture sLmSubtex[4];

static void LayerMapInit(void) {
    for (int i = 0; i < 4; ++i) {
        sLmReady[i] = false;
        if (!C3D_TexInitVRAM(&sLmTex[i], LM_W, LM_H, GPU_RGBA5551)) continue;
        C3D_TexSetFilter(&sLmTex[i], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sLmTex[i], GPU_REPEAT, GPU_REPEAT);
        sLmRT[i] = C3D_RenderTargetCreateFromTex(&sLmTex[i], GPU_TEXFACE_2D, 0, -1);
        if (!sLmRT[i]) { C3D_TexDelete(&sLmTex[i]); continue; }
        sLmReady[i] = true; /* cells start at stamp 0: older than any state */
    }
}

/* Per-frame bookkeeping before a layer's cells are checked: stamp the layer
 * when its tile data base or brightness changed (nothing drawn under the old
 * one holds), and each palette bank that changed colour. */
typedef struct { uint32_t stampNow; int ops; } LayerMapPass;

static uint32_t sBgPalBankHash[16], sObjPalBankHash[16];
static uint32_t sBgPalFullHash, sObjPalFullHash;
static uint32_t sFrameStamp;

static LayerMapPass LayerMapBegin(int bg, uint32_t charBase, bool bpp8) {
    /* memset first: the struct has a padding byte, and memcmp compares it --
     * left as stack garbage it made the state "change" at random, redrawing
     * the whole map every few frames. */
    LayerMapState st;
    memset(&st, 0, sizeof(st));
    st.charBase = charBase;
    st.bpp8 = (uint8_t)bpp8;
    if (memcmp(&st, &sLmState[bg], sizeof(st)) != 0 || sLmStateStamp[bg] == 0) {
        sLmState[bg] = st;
        sLmStateStamp[bg] = sFrameStamp;
    }
    LayerMapPass pass = { sFrameStamp, 0 };
    for (int b = 0; b < 16; ++b) {
        if (sLmBankHash[bg][b] != sBgPalBankHash[b]) {
            sLmBankHash[bg][b] = sBgPalBankHash[b];
            sLmBankStamp[bg][b] = pass.stampNow;
        }
    }
    if (sLmBankHash[bg][16] != sBgPalFullHash) {
        sLmBankHash[bg][16] = sBgPalFullHash;
        sLmBankStamp[bg][16] = pass.stampNow;
    }
    return pass;
}

/* The BG3 haze pass (issue #29): measured 2026-09-05 at 8,45 ms of a 16,91 ms
 * frame when it composed 640-660 tiles into an offscreen target every frame
 * and blitted it back as one quad per scanline. What stayed: BG3 ripples once
 * into a target (sHazeRippleRT) and each eye draws one quad (17,77 -> 11,26
 * ms), and with the layer maps BG3's rows come straight from its map. The
 * per-tile bake into sHazeTex is the fallback for when the ripple target or
 * BG3's map could not be allocated: without the ripple target it is blitted
 * a scanline strip at a time. (The measurement modes -- no compose, pass off
 * -- were debug-menu switches and are gone.) */

#ifdef PORT_DEBUG_TOOLS_ACTIVE
static int sDiagAffineDrawn[2], sDiagBlendDrawn[2];
static int sDiagSemiTransColl, sDiagAffineColl, sDiagMosaicColl, sDiagSemiTransX, sDiagSemiTransOam;
#endif
/* How many layer maps had cells redrawn this frame. Recorded per perf
 * sample. */
static unsigned sLastLayerComposes;
/* Device pixels covered by the frame's quads, summed over every eye. See
 * where it is accumulated for why it exists. */
static uint32_t sLastDrawnPixels;
static HazeTile sHazeTiles[HAZE_MAX_TILES];
static int sHazeTileCount;

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
/* ---- VRAM change stamps -------------------------------------------------
 * Whether a cached slot still matches VRAM used to be a memcmp of the tile's
 * bytes against a copy kept per slot, on every tile REFERENCE -- thousands a
 * frame, each touching two cold 32-byte lines (the tile and the copy), and
 * TileHasOpaquePixel read the tile a third time. On an Old 3DS, with no L2
 * cache, that memory traffic was most of the collection time.
 *
 * Now VRAM is compared against a shadow copy ONCE per frame, 32 bytes (one
 * 4bpp tile) at a time, in one linear pass. Each 32-byte chunk records the
 * frame it last changed in and whether it has any non-zero byte; a slot
 * records the frame it was decoded in. "Unchanged since decode" is then two
 * integer reads from small, hot arrays. Exactly as strict as the memcmp was:
 * a chunk's stamp moves whenever any of its bytes differ from the last frame,
 * and collection never runs while game code is writing VRAM. */
enum { VRAM_BYTES = 0x18000, VRAM_CHUNK_BYTES = 32, VRAM_CHUNKS = VRAM_BYTES / VRAM_CHUNK_BYTES };
static uint32_t sVramShadow[VRAM_BYTES / 4];
static uint32_t sChunkStamp[VRAM_CHUNKS];   /* frame the chunk last changed in */
static uint8_t sChunkOpaque[VRAM_CHUNKS];   /* any non-zero byte (colour index) */
/* sFrameStamp (current frame; 0 = never diffed) is declared with the layer maps. */
/* BG-region chunks (the first 64KB: tile data and tilemaps) whose stamp is
 * this frame. OBJ tiles are left out: Samus's graphics are copied into OBJ
 * VRAM every frame, which would make every frame look like a BG change. */
static int sChunksChangedNow;
enum { VRAM_BG_CHUNKS = 0x10000 / 32 };

static void VramDiffBeginFrame(void) {
    const bool first = (sFrameStamp == 0);
    ++sFrameStamp;
    sChunksChangedNow = 0;
    const uint32_t* cur = (const uint32_t*)gVram;
    uint32_t* old = sVramShadow;
    for (int c = 0; c < VRAM_CHUNKS; ++c, cur += 8, old += 8) {
        /* Ask for the lines a few chunks ahead: a straight 192KB read is what
         * this pass is, and without a hint the Old 3DS (no L2) stalls on
         * every line in turn. */
        __builtin_prefetch(cur + 32);
        __builtin_prefetch(old + 32);
        const uint32_t diff = (cur[0] ^ old[0]) | (cur[1] ^ old[1]) | (cur[2] ^ old[2]) | (cur[3] ^ old[3]) |
                              (cur[4] ^ old[4]) | (cur[5] ^ old[5]) | (cur[6] ^ old[6]) | (cur[7] ^ old[7]);
        if (diff == 0 && !first) continue;
        memcpy(old, cur, VRAM_CHUNK_BYTES);
        sChunkStamp[c] = sFrameStamp;
        if (c < VRAM_BG_CHUNKS) ++sChunksChangedNow;
        sChunkOpaque[c] = (cur[0] | cur[1] | cur[2] | cur[3] | cur[4] | cur[5] | cur[6] | cur[7]) != 0;
    }
}

/* Latest change stamp over the chunks a tile at byteOffset spans (two for an
 * 8bpp tile). Offsets past VRAM (an 8bpp tile index running off the end)
 * read as never changed. */
static inline uint32_t TileChangeStamp(uint32_t byteOffset, bool bpp8) {
    const uint32_t c = byteOffset / VRAM_CHUNK_BYTES;
    uint32_t stamp = (c < VRAM_CHUNKS) ? sChunkStamp[c] : 0u;
    if (bpp8 && c + 1 < VRAM_CHUNKS && sChunkStamp[c + 1] > stamp) stamp = sChunkStamp[c + 1];
    return stamp;
}

/* Frame each tile slot was last decoded in (see VramDiffBeginFrame). */
static uint32_t sCacheDecodeStamp[ATLAS_MAX_SLOTS];
/* Colour indices a 4bpp slot's tile actually uses (bit per index; 0xFFFF for
 * 8bpp, where it is not tracked). A palette step that only changes colours a
 * tile does not use leaves that tile exactly as it was. */
static uint16_t sCacheUsedMask[ATLAS_MAX_SLOTS];

/* ---- BG palette colour stamps ---------------------------------------------
 * The frame each of the 256 BG palette entries last changed in, from a
 * per-frame compare against last frame's palette. Palette animations change
 * a few entries of one bank (a glow, a light); with these a layer-map cell or
 * atlas slot is stale only if a colour its tile uses changed, not whenever
 * anything in its bank did. */
/* The BG palette every BG tile is decoded from, and the one the change
 * stamps and hashes follow: the palette itself, except the banks a palette
 * fade is being applied on the GPU for, which hold the game's source copy
 * (see PalFadeBeginFrame). */
static uint16_t sBgPalEff[256];
static uint16_t sBgPalPrev[256];
static uint32_t sBgPalColorStamp[256];

static void BgPaletteStampsBeginFrame(void) {
    const uint16_t* pal = sBgPalEff;
    for (int i = 0; i < 256; ++i) {
        if (pal[i] != sBgPalPrev[i] || sBgPalColorStamp[i] == 0) {
            sBgPalPrev[i] = pal[i];
            sBgPalColorStamp[i] = sFrameStamp;
        }
    }
}

/* Whether any colour in `used` (bits of bank `bank`; bit 0, transparent, is
 * ignored) changed after frame `since`. */
static inline bool BgColorsChangedSince(int bank, uint16_t used, uint32_t since) {
    const uint32_t* stamps = &sBgPalColorStamp[bank * 16];
    for (uint32_t m = used & 0xFFFEu; m != 0; m &= m - 1) {
        if (stamps[__builtin_ctz(m)] > since) return true;
    }
    return false;
}
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
static uint64_t sDirtyRowMask[(ATLAS_SLOT_ROWS + 63) / 64];

static inline void MarkAtlasRowDirty(int row) {
    sDirtyRowMask[row >> 6] |= (1ull << (row & 63));
}

/* Tiles decoded during collection are staged here, not written to the atlas:
 * collection runs before C3D_FrameBegin, overlapping the GPU still drawing
 * the previous frame, and a slot redecoded in place (an OBJ tile the game
 * reloaded with other graphics, an animated tile) would change under that
 * frame's feet. The right eye is drawn last, so it was the one that showed
 * the next frame's -- or another sprite's -- graphics for a frame: the 3D
 * "flashes". AtlasApplyStaged copies them in once the GPU is done. One entry
 * per slot (a slot decoded twice keeps the last), so it never overflows. */
static AtlasTexel sStageTexels[ATLAS_MAX_SLOTS][64];
static int16_t sStageSlot[ATLAS_MAX_SLOTS];
static int16_t sStageIndex[ATLAS_MAX_SLOTS]; /* per slot: its entry, or -1 */
static int sStageCount;

static inline AtlasTexel* StageSlot(int slot) {
    int i = sStageIndex[slot];
    if (i < 0) {
        i = sStageCount++;
        sStageIndex[slot] = (int16_t)i;
        sStageSlot[i] = (int16_t)slot;
    }
    return sStageTexels[i];
}


/* Work counters for the perf recorder (PERF_COUNT_*, platform_gpu_3ds.h),
 * handed over once per frame at the end of Port_GpuRenderer_CollectFrame. */
static uint32_t sPerfCount[8];
static inline uint32_t TicksToUs(u64 ticks) { return (uint32_t)(ticks * 1000u / (u64)(SYSCLOCK_ARM11 / 1000u)); }



/* Per-frame palette hashes (see sCachePalHash's comment): computed ONCE per
 * frame in Port_GpuRenderer_RenderFrame from gBgPltt/gObjPltt, then reused
 * by every tile reference that frame instead of re-hashing/re-comparing
 * palette bytes per reference. Bank hashes cover the 16 four-bit banks (32
 * bytes/16 colors each); full hashes cover the whole 512-byte/256-color
 * palette (what an 8bpp tile, which doesn't bank, actually samples from). */
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

/* True for the current frame when a door footprint (see port_ppu_mzm.c) is
 * within the view box, so the per-tile path must run to pull the doorway onto
 * one plane. Cleared otherwise, leaving every position to the layer map. */
static bool sDoorDepthOnScreen = false;

extern int Port_Hud_GetOamCount(void);

/* The game's gBgPointersAndDimensions: the room's decompressed block maps and
 * clipdata size. Declared by hand -- this file stays out of the game headers --
 * once, for every user below. */
extern struct {
    struct { u16* pDecomp; u16 width; u16 height; } backgrounds[3];
    u16* pClipDecomp;
    u16 clipdataWidth;
    u16 clipdataHeight;
} gBgPointersAndDimensions;

/* Room the correction list was last selected for. Re-selecting on every frame
 * would rescan the whole list for nothing; the room only changes on a door. */
static int sFixArea = -1, sFixRoom = -1;

/* Same, for the door-footprint rects. Kept separate from sFixArea/sFixRoom so
 * it still refreshes on a stock build with no correction list compiled in. */
static int sDoorArea = -1, sDoorRoom = -1;

static void UpdateDoorDepthRoom(void) {
    extern u8 gCurrentArea;
    extern u8 gCurrentRoom;
    if ((int)gCurrentArea == sDoorArea && (int)gCurrentRoom == sDoorRoom) return;
    sDoorArea = (int)gCurrentArea;
    sDoorRoom = (int)gCurrentRoom;
    extern void PortPpuMzm_SetDoorDepthRoom(int area, int room);
    PortPpuMzm_SetDoorDepthRoom(sDoorArea, sDoorRoom);
}

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
    extern u8 gSamusOnTopOfBackgrounds;
    /* Cutscene enum (include/constants/cutscene.h: MAKE_ENUM(s8, Cutscene)),
     * valid while GM_CUTSCENE renders. Declared by hand -- this file keeps out
     * of the game struct headers, same as gMainGameMode above. */
    extern signed char gCurrentCutscene;
    sDepthState.inGameplay = (gMainGameMode == 4);
    sDepthState.samusOnTopOfBackgrounds =
        sDepthState.inGameplay && gSamusOnTopOfBackgrounds != 0;
    /* BG0 is the pop-forward overlay layer for menus / dialogs / the pause
     * map -- i.e. everywhere outside gameplay EXCEPT the scene-art cutscenes,
     * which draw full-screen artwork on their BGs while the caption is OBJ:
     *   1  GM_INTRO            (opening story: portraits + Zero-Suit scene
     *                           on BG0/BG1, story text and ship are OBJ)
     *   7  GM_CHOZODIA_ESCAPE  ("mission accomplished" over the blue ship)
     *   9  GM_TOURIAN_ESCAPE   (post-escape montage: rooms exploding, the
     *                           ship leaving, and the closing story text)
     *   10 GM_CUTSCENE         (in-game story cutscenes: Kraid rising, ...)
     * Those get the cutsceneArt mapping instead: BGs spread by raw priority
     * (no 0/1 merge -- that merge is a gameplay-room rule and here it just
     * flattens the parallax), caption OBJ pops forward, actor OBJ on the
     * play plane. See PortStereoDepth_BgTierForPriority / _ObjTier. */
    switch (gMainGameMode) {
        case 1:
        case 7:
        case 9:
        case 10:
            sDepthState.bg0IsOverlayText = false;
            sDepthState.cutsceneArt = true;
            /* Scene id for the optional per-cutscene override list
             * (port_cutscene_depth.h). gCurrentCutscene is only meaningful for
             * GM_CUTSCENE (10); SceneFromGame ignores it for 1/7/9. */
            sDepthState.cutsceneScene = (uint8_t)PortCutsceneDepth_SceneFromGame(
                gMainGameMode, (int)gCurrentCutscene);
            {
                /* Montage page index -- lives behind port_ppu_mzm.c's game
                 * headers, same hand-declared extern style as gCurrentCutscene. */
                extern int PortPpuMzm_CutsceneStage(void);
                sDepthState.cutsceneStage = (uint8_t)PortPpuMzm_CutsceneStage();
            }
            break;
        default:
            sDepthState.bg0IsOverlayText = !sDepthState.inGameplay;
            sDepthState.cutsceneArt = false;
            sDepthState.cutsceneScene = 0;
            sDepthState.cutsceneStage = 0;
            break;
    }
    /* Two-plane flatten for depthless screens (see flatMenu): content
     * forward, backdrop back. GM_FILE_SELECT (3): backdrop is BG3 only, so
     * the split is at priority 3. GM_CREDITS (8): the crew text alternates
     * BG0/BG1 while the Chozo wall is BG2/BG3, so the split is at priority
     * 2 -- all text forward, all wall back. */
    if (gMainGameMode == 3) {
        sDepthState.flatMenu = true;
        sDepthState.flatMenuBackdropPrio = 3;
    } else if (gMainGameMode == 8) {
        sDepthState.flatMenu = true;
        sDepthState.flatMenuBackdropPrio = 2;
    } else {
        sDepthState.flatMenu = false;
        sDepthState.flatMenuBackdropPrio = 0;
    }
    for (int bg = 0; bg < 4; ++bg) {
        sDepthState.priority[bg] =
            (uint8_t)(((uint16_t)(gIoMem[0x08 + bg * 2] | (gIoMem[0x09 + bg * 2] << 8))) & 3u);
    }
    /* Which sub-scene of a montage cutscene this frame is: BG enable + the
     * four priorities. Only used when cutsceneArt; harmless otherwise. */
    sDepthState.cutsceneLayout = sDepthState.cutsceneArt
        ? PortCutsceneDepth_LayerSignature(dispcnt, sDepthState.priority)
        : 0;
    UpdateLayerFixRoom();
    UpdateDoorDepthRoom();

    /* Invalidate the content caches on any room/area/mode change, and keep
     * them invalidated for a SETTLE WINDOW afterwards. A transition takes
     * several frames to fully land -- screenmap, then tile graphics, then
     * palettes, each its own DMA -- and any frame sampled mid-way lets stale
     * VRAM match a stale cache entry, so a cached layer bakes in the
     * previous screen (file-select text, the intro starfield over the whole
     * frame, another room's scenery -- confirmed from a hardware GPUDIAG log
     * where the compose ran with dec=0, i.e. trusting the atlas completely
     * while the atlas was still stale). One clean frame is not enough; a
     * dozen covers the whole multi-DMA settle and is invisible under the
     * transition fade. */
    {
        extern u8 gCurrentArea;
        extern u8 gCurrentRoom;
        static int sCacheRoomArea = -1, sCacheRoomNum = -1, sCacheGameMode = -1;
        static int sCacheSettleFrames;
        if ((int)gCurrentArea != sCacheRoomArea || (int)gCurrentRoom != sCacheRoomNum ||
            (int)gMainGameMode != sCacheGameMode) {
            sCacheRoomArea = (int)gCurrentArea;
            sCacheRoomNum = (int)gCurrentRoom;
            sCacheGameMode = (int)gMainGameMode;
            sCacheSettleFrames = 16;
        }
        if (sForcedSettleFrames > sCacheSettleFrames) sCacheSettleFrames = sForcedSettleFrames;
        if (sForcedSettleFrames > 0) --sForcedSettleFrames;
        if (sCacheSettleFrames > 0) {
            --sCacheSettleFrames;
            sSettleResetPending = true; /* the next frame resets again: see CollectNeedsIdleGpu */
            /* Per-tile cache too: it normally persists (OBJ + fallbacks) but
             * across a transition its stale entries are exactly the problem.
             * Clearing it costs one frame of redecode, hidden by the fade. */
            for (int i = 0; i < HASH_BUCKETS; ++i) sHashBucketHead[i] = -1;
            sCacheCount = 0;
            ++sAtlasGen;
        }
    }

    /* Whether a doorway is actually on screen this frame. Only then does the
     * per-tile path have to run for the door depth pull; walking away from
     * the door leaves the layer map to draw everything. */
    sDoorDepthOnScreen = false;
    {
        extern int PortPpuMzm_DoorDepthCount(void);
        if (sDepthState.inGameplay && PortPpuMzm_DoorDepthCount() > 0) {
            extern void PortPpuMzm_ScreenOrigin(int* outX, int* outY);
            extern bool PortPpuMzm_DoorDepthInView(int bx0, int by0, int bx1, int by1);
            int ox = 0, oy = 0;
            PortPpuMzm_ScreenOrigin(&ox, &oy);
            /* px -> 16px blocks; the 240x160 frame spans ~16x11 blocks. The
             * WIDE view reaches past it -- up to twice the margin on the side
             * it slides toward -- and a door out there needs the pull too. */
            const int mx = 2 * PortWide_MarginX(), my = 2 * PortWide_MarginY();
            sDoorDepthOnScreen = PortPpuMzm_DoorDepthInView(
                (ox - mx) >> 4, (oy - my) >> 4, (ox + 240 + mx) >> 4, (oy + 160 + my) >> 4);
        }
    }

    /* Visible item tanks are animated tiles with no BG block-map entry, so
     * they cannot go through the layer-fix list; the renderer lifts them to
     * Samus's plane directly (see the tank-tile branch below). Rescanned
     * every frame so a hidden tank revealed by a shot is picked up the frame
     * its clipdata flips -- the scan is a cheap block-grid walk. */
    extern void PortPpuMzm_ScanRoomTanks(void);
    PortPpuMzm_ScanRoomTanks();
}

/* Whole-machine save-state load (port_save_state.c) just replaced VRAM,
 * palettes and every other decode input under the renderer's feet. Force the
 * same multi-frame cache rebuild a room transition gets -- the settle check
 * in RenderFrame keys on area/room/mode and would not trip when a reload
 * lands back in the same room. */
void Port_GpuRenderer_InvalidateAll(void) { sForcedSettleFrames = 24; }



/* Debug aid: flat-colour every drawn layer and sprite by its resolved stereo
 * tier (kDepthTintColor), so on a fast cutscene you can see at a glance which
 * plane each layer landed on. Only the main draw loop honours it; the
 * outside-border HUD pass is left readable. Costs one bool test when off, and
 * needs no cache reset -- it swaps the texenv, it never touches atlas texels. */
static bool sDepthTint = false;
void Port_GpuRenderer_SetDepthTint(bool on) { sDepthTint = on; }
bool Port_GpuRenderer_DepthTintEnabled(void) { return sDepthTint; }



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

/* Atlas subtexture UVs.
 *
 * The span is exactly n texels for an n-texel slot, SHIFTED by an eighth of
 * a texel. Both halves of that matter, and both were learned the hard way on
 * hardware (2026-09-05):
 *
 *   - The span must be n, not the n-1 a half-texel inset on each edge gives.
 *     An n-1 span is exact at 1:1 and wrong at every other scale, by an
 *     amount that depends on the span -- so one 16x16 block quad and the four
 *     8x8 tile quads it replaces chose DIFFERENT texels once the display was
 *     scaled, and the GPU tile renderer disagreed with the CPU scanline
 *     renderer for the same reason (the CPU path draws the whole 240-wide
 *     frame as one quad, so its sequence was the ideal one).
 *
 *   - The span must not be pinned at the texel edges either. At 3:2 that puts
 *     every third sample EXACTLY on a texel boundary, where which side it
 *     lands is up to the hardware's rounding; a real PICA200 lands low often
 *     enough to pull in the neighbouring atlas slot, seen as dirt crawling
 *     along tile edges. The shift breaks those ties the same way every time.
 *
 * Any shift between about 1/64 and 5/16 of a texel works; an eighth is the
 * middle of that range, and leaves the outermost samples ~0.46 texels inside
 * the slot at the widest quads in use. PlatformGpu3DS's presentation quad
 * uses the same shift on purpose -- that is what makes the two renderers
 * agree pixel for pixel at 3:2, which is what lets one be the other's
 * oracle. */
#define ATLAS_UV_TIE_SHIFT 0.125f

static void InitSlotSubtexTable(void) {
    /* FULL-texel edges (sx .. sx+n), not the half-texel inset
     * (sx+0.5 .. sx+n-0.5) these tables used to carry.
     *
     * The inset samples a span of n-1 texels for n screen pixels. At 1:1
     * that still lands on texel centres and is exact, which is why it went
     * unnoticed for as long as everything was 8x8. It stops being exact the
     * moment a quad is SCALED, and worse, the error depends on the span: at
     * 1.5x a 16-texel quad picks
     *     0 1 2 2 3 3 4 5 5 6 7 7 8 8 9 10 10 11 12 12 13 13 14 15
     * where two 8-texel quads covering the same 24 pixels pick
     *     0 1 1 2 3 3 4 4 5 6 6 7 8 9 9 10 11 11 12 12 13 14 14 15.
     * So one 16x16 block quad and the four 8x8 tile quads it replaces draw
     * DIFFERENT IMAGES, which is exactly what turning the block pass on and
     * off looked like on hardware (reported 2026-09-05) -- a correctness
     * bug in step A that had nothing to do with the tiles it fetched: the
     * addressing was verified identical across 12285 groups of a real
     * gameplay recording.
     *
     * Full-texel edges sample n texels for n pixels, so the texel sequence
     * depends only on the scale and not on how wide the quad is, and the
     * two agree at every display style. It is also more regular when
     * scaled -- the doubled texels land on a fixed period instead of
     * clustering. Nothing bleeds: with GPU_NEAREST every sample sits at a
     * pixel centre, which is strictly inside the span, never on the edge. */
    const float invU = 1.0f / (float)ATLAS_W;
    const float invV = 1.0f / (float)ATLAS_H;
    const float sh = ATLAS_UV_TIE_SHIFT;
    for (int slot = 0; slot < ATLAS_MAX_SLOTS; ++slot) {
        int sx = (slot % ATLAS_TILES_PER_ROW) * 8;
        int sy = (slot / ATLAS_TILES_PER_ROW) * 8;
        sSlotSubtexTable[slot] = (Tex3DS_SubTexture){
            .width = 8,
            .height = 8,
            .left = ((float)sx + sh) * invU,
            .top = 1.0f - ((float)sy + sh) * invV,
            .right = ((float)(sx + 8) + sh) * invU,
            .bottom = 1.0f - ((float)(sy + 8) + sh) * invV,
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
    /* RGBA5551: a GBA colour is 15 bits plus "transparent", so this holds
     * exactly what RGBA8 did at half the bytes -- and the GPU reads every
     * texel of every quad from here, in FCRAM. Measured on an Old 3DS, the
     * GPU's frame time tracks the pixels drawn (~94 ns each), i.e. it is
     * bound by these texture reads. */
    if (!C3D_TexInit(&sAtlasTexture, ATLAS_W, ATLAS_H, GPU_RGBA5551)) return false;
    memset(sAtlasTexture.data, 0, (size_t)ATLAS_W * ATLAS_H * sizeof(AtlasTexel));
    FlushAtlasRange(sAtlasTexture.data, (size_t)ATLAS_W * ATLAS_H * sizeof(AtlasTexel));
    C3D_TexSetFilter(&sAtlasTexture, GPU_NEAREST, GPU_NEAREST);

    /* Affine BG2 compose target -- CPU-written like the atlas (not a GPU
     * render target), so C3D_TexInit, not VRAM. Non-fatal on failure:
     * DetectAffineBg2 checks sAffineBg2TexReady and the scene just keeps
     * falling back to the CPU renderer. */
    sAffineBg2TexReady = false;
    if (C3D_TexInit(&sAffineBg2Tex, AFF_BG2_DIM, AFF_BG2_DIM, GPU_RGBA5551)) {
        memset(sAffineBg2Tex.data, 0, (size_t)AFF_BG2_DIM * AFF_BG2_DIM * sizeof(AtlasTexel));
        FlushAtlasRange(sAffineBg2Tex.data, (size_t)AFF_BG2_DIM * AFF_BG2_DIM * sizeof(AtlasTexel));
        C3D_TexSetFilter(&sAffineBg2Tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sAffineBg2Tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        sAffineBg2TexReady = true;
    }

    /* Offscreen BG3 target for the issue #29 per-scanline ripple. VRAM-backed
     * (it is a GPU render target, never CPU-written). Non-fatal on failure --
     * sHazeRtReady stays false and haze rooms just render BG3 flat. */
    sHazeRtReady = true;
    for (int b = 0; b < 2; ++b) {
        if (!C3D_TexInitVRAM(&sHazeTex[b], HAZE_RT_W, HAZE_RT_H, GPU_RGBA8)) { sHazeRtReady = false; break; }
        C3D_TexSetFilter(&sHazeTex[b], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sHazeTex[b], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        sHazeRT[b] = C3D_RenderTargetCreateFromTex(&sHazeTex[b], GPU_TEXFACE_2D, 0, -1);
        if (!sHazeRT[b]) { C3D_TexDelete(&sHazeTex[b]); sHazeRtReady = false; break; }
    }

    /* The rippled target -- see sHazeRippleTex. Optional: without it BG3 is
     * baked per tile and blitted a scanline strip at a time. */
    sHazeRippleReady = false;
    if (C3D_TexInitVRAM(&sHazeRippleTex, HAZE_RT_W, HAZE_RT_H, GPU_RGBA8)) {
        C3D_TexSetFilter(&sHazeRippleTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sHazeRippleTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        sHazeRippleRT = C3D_RenderTargetCreateFromTex(&sHazeRippleTex, GPU_TEXFACE_2D, 0, -1);
        if (sHazeRippleRT) sHazeRippleReady = true;
        else C3D_TexDelete(&sHazeRippleTex);
    }

    /* One wrapping target per BG layer. Not fatal if one fails: that layer
     * falls back to the per-tile pass. */
    LayerMapInit();

    InitSlotSubtexTable();
    /* Not fatal: without it the items go through citro2d as before. */
    sBatchReady = BatchInit();

    {
        char msg[96];
        snprintf(msg, sizeof(msg), "GPU renderer init: haze=%d ripple=%d layermaps=%d%d%d%d batch=%d vramFree=%luKB",
                 (int)sHazeRtReady, (int)sHazeRippleReady, (int)sLmReady[0], (int)sLmReady[1],
                 (int)sLmReady[2], (int)sLmReady[3], (int)sBatchReady,
                 (unsigned long)(vramSpaceFree() / 1024u));
        Port_DebugLog_Note(msg);
    }

    for (int i = 0; i < HASH_BUCKETS; ++i) sHashBucketHead[i] = -1;
    sCacheCount = 0;
    memset(sStageIndex, 0xFF, sizeof(sStageIndex));
    sStageCount = 0;

    sInitialized = true;
    return true;
}

void Port_GpuRenderer_Shutdown(void) {
    if (!sInitialized) return;
    C3D_TexDelete(&sAtlasTexture); /* also frees the linearAlloc'd backing store */
    if (sAffineBg2TexReady) { C3D_TexDelete(&sAffineBg2Tex); sAffineBg2TexReady = false; }
    if (sHazeRtReady) {
        for (int b = 0; b < 2; ++b) {
            C3D_RenderTargetDelete(sHazeRT[b]);
            C3D_TexDelete(&sHazeTex[b]);
            sHazeRT[b] = NULL;
        }
        sHazeRtReady = false;
    }
    sInitialized = false;
    sGpuRendererActive = false;
}

static inline AtlasTexel Bgr555ToRgba5551(uint16_t color, bool transparent) {
    if (transparent) return 0u;
    const unsigned r = color & 0x1Fu;
    const unsigned g = (color >> 5) & 0x1Fu;
    const unsigned b = (color >> 10) & 0x1Fu;
    /* GPU_RGBA5551: RRRRRGGGGGBBBBBA, most significant bit first. */
    return (AtlasTexel)((r << 11) | (g << 6) | (b << 1) | 1u);
}

/* The GBA backdrop (BG palette entry 0) as a C2D clear colour. C2D_Color32
 * takes r,g,b,a as arguments, so unlike Bgr555ToRgba5551 -- which packs bits
 * for the atlas texture's own layout -- the channels go in straight. Same
 * 5->8 bit expansion, so a backdrop-coloured clear and a backdrop-coloured
 * tile match to the byte. */
static inline u32 BackdropClearColor(void) {
    const uint16_t color = (uint16_t)(gBgPltt[0] | ((uint16_t)gBgPltt[1] << 8));
    u32 r = color & 0x1Fu, g = (color >> 5) & 0x1Fu, b = (color >> 10) & 0x1Fu;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return C2D_Color32((u8)r, (u8)g, (u8)b, 255);
}

/* GBA brightness increase/decrease on 5-bit channels, the same formula as
 * port/ppu/src/mode1.c's mode1_brighten/mode1_darken (GBATEK:
 * I = I +- I(or 31-I)*evy/16, truncating; evy pre-clamped 0..16). The atlas
 * texel already holds 5-bit channels, so this is the hardware's own
 * arithmetic with nothing to convert. */
static inline AtlasTexel ApplyBrighten(AtlasTexel color, int evy) {
    if ((color & 1u) == 0u) return color; /* transparent stays transparent */
    int r = (color >> 11) & 0x1F, g = (color >> 6) & 0x1F, b = (color >> 1) & 0x1F;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    return (AtlasTexel)((r << 11) | (g << 6) | (b << 1) | 1);
}

static inline AtlasTexel ApplyDarken(AtlasTexel color, int evy) {
    if ((color & 1u) == 0u) return color;
    int r = (color >> 11) & 0x1F, g = (color >> 6) & 0x1F, b = (color >> 1) & 0x1F;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return (AtlasTexel)((r << 11) | (g << 6) | (b << 1) | 1);
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

/* ---- Fades applied on the GPU --------------------------------------------
 * Two kinds of whole-screen fade step every frame, and baked into the tiles
 * each step made every cell of every layer map stale -- a full redraw of
 * every map, every frame of the fade (15-18 ms of BG work on an Old 3DS):
 *
 *  - BLDY brighten / darken (BLDCNT effect 2/3) on a first-target layer.
 *  - The game's palette fades (door transitions, flashes, fades to/from
 *    black): src/color_effects.c ApplySpecialBackgroundFadingColor writes
 *    T(source) into the palette every frame, where source is the game's own
 *    unfaded copy (COLOR_DATA_BG_EWRAM2) and T scales each 5-bit channel
 *    toward black or white by a step k/32.
 *
 * Both are per-channel affine maps, out = in * A + B, so BG tiles are
 * decoded without them and the map is applied by the texenv when the tiles
 * (or a layer map) are drawn. For palette fades that means decoding from
 * the source copy instead of the palette: it does not change while the
 * fade runs, so nothing goes stale. The fade is only taken when the
 * palette is EXACTLY T(source) under the game's integer arithmetic, bank by
 * bank (PalFadeBeginFrame); a bank that is not (the door transition leaves
 * the hatch bank lit) keeps its own colours and its tiles are drawn per
 * tile, outside the layer maps. The GPU result is within one 5-bit level of
 * the GBA's. */
enum { ITEM_FX_BRIGHT = 1, ITEM_FX_PALFADE = 2 };
static bool sPalFadeOn;
static uint16_t sPalFadeBanks;     /* banks drawn from the source copy */
static float sPalFadeA, sPalFadeB; /* the fade as out = in * A + B */
/* gpuFx given to items as they are allocated (AllocDrawItemSubtex). */
static uint8_t sPushFx;

/* out = texture * A + B on RGB, alpha passed through (the alpha test still
 * drops index 0): the item's fades, palette fade first, then BLDY -- the
 * GBA's order (the palette is looked up, then the blend unit applies BLDY).
 * GBATEK: brighten I + (31 - I) * EVY / 16, darken I - I * EVY / 16. */
static void ConfigureFxTextureEnv(int fx) {
    float a = 1.0f, b = 0.0f;
    if (fx & ITEM_FX_PALFADE) {
        a = sPalFadeA;
        b = sPalFadeB;
    }
    if ((fx & ITEM_FX_BRIGHT) && (sBldEffect == 2 || sBldEffect == 3)) {
        const float e = (float)sBldEvy / 16.0f;
        a *= 1.0f - e;
        b = b * (1.0f - e) + (sBldEffect == 2 ? e : 0.0f);
    }
    const u8 a8 = (u8)(a * 255.0f + 0.5f), b8 = (u8)(b * 255.0f + 0.5f);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, C2D_Color32(a8, a8, a8, 255));
    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env); /* previous, replace */
    if (b8 != 0) {
        C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
        C3D_TexEnvColor(env, C2D_Color32(b8, b8, b8, 255));
    }
    C3D_TexEnvInit(C3D_GetTexEnv(2));
}

/* The game's palette fade on one colour (ApplySpecialBackgroundFadingColor,
 * same arithmetic and wrap). */
enum { PAL_FADE_IN, PAL_FADE_FLASH, PAL_FADE_OUT, PAL_FADE_TOWARD_WHITE, PAL_FADE_TYPES };
static inline uint16_t PalFadeColor(int type, uint16_t c, int k) {
    int ch[3] = { c & 31, (c >> 5) & 31, (c >> 10) & 31 };
    for (int i = 0; i < 3; ++i) {
        const int v = ch[i];
        switch (type) {
            case PAL_FADE_IN: ch[i] = ((v * k) >> 5) & 31; break;
            case PAL_FADE_FLASH: ch[i] = (31 - (((31 - v) * k) >> 5)) & 31; break;
            case PAL_FADE_OUT: ch[i] = (v - ((v * k) >> 5)) & 31; break;
            default: ch[i] = (v + ((k * (31 - v)) >> 5)) & 31; break;
        }
    }
    return (uint16_t)(ch[0] | (ch[1] << 5) | (ch[2] << 10));
}

/* Whether bank `b` of the palette is exactly the fade of the source's.
 * Colour 0 is skipped: transparent in a tile (the backdrop is drawn from the
 * palette itself), and the game pokes it on its own (src/haze.c). */
static inline bool PalFadeBankMatches(const uint16_t* pal, const uint16_t* src, int b, int type, int k) {
    for (int i = b * 16 + 1; i < b * 16 + 16; ++i)
        if ((pal[i] & 0x7FFFu) != PalFadeColor(type, src[i], k)) return false;
    return true;
}

/* Works out this frame's effective BG palette (sBgPalEff) and whether a
 * palette fade is taken on the GPU. `allowed` is false whenever a BG layer
 * would not go through a layer map / the per-tile pass (see the caller). */
static void PalFadeBeginFrame(bool allowed) {
    extern uint8_t gEwram[];
    const uint16_t* pal = (const uint16_t*)gBgPltt;
    const uint16_t* src = (const uint16_t*)(gEwram + 0x35000 + 0x400); /* COLOR_DATA_BG_EWRAM2 */
    memcpy(sBgPalEff, pal, sizeof(sBgPalEff));
    sPalFadeOn = false;
    if (!allowed) return;

    /* Banks that differ from the source at all: only those say anything
     * about a fade. None -> no fade (or the identity step at its end). */
    uint16_t differ = 0;
    for (int b = 0; b < 16; ++b)
        for (int i = b * 16 + 1; i < b * 16 + 16; ++i)
            if ((pal[i] & 0x7FFFu) != (src[i] & 0x7FFFu)) { differ |= (uint16_t)(1u << b); break; }
    if (differ == 0) return;

    /* The same (palette, source) as last frame gives the same answer. */
    static bool sLastValid;
    static uint32_t sLastPalHash, sLastSrcHash;
    static int sLastType = -1, sLastK;
    static uint16_t sLastBanks;
    const uint32_t palHash = HashBytes((const uint8_t*)pal, 512);
    const uint32_t srcHash = HashBytes((const uint8_t*)src, 512);
    int bestType = -1, bestK = 0;
    uint16_t bestBanks = 0;
    if (sLastValid && palHash == sLastPalHash && srcHash == sLastSrcHash) {
        bestType = sLastType; bestK = sLastK; bestBanks = sLastBanks;
    } else {
        /* The type and step that explain the most differing banks. The
         * candidates are the ones that reproduce a changed colour of the
         * first two differing banks -- at most one differing bank may go
         * unexplained (below), so one of the two is a faded bank.
         *
         * Taken only if it explains every differing bank but at most one,
         * and at least two: outside a fade the source copy can be stale,
         * and a lone bank that happens to match (all black against any
         * source is IN at step 0) would push every tile of the others out
         * of the layer maps for nothing. Rendering would still be right --
         * a bank that matches is drawn exactly -- just slower. */
        int differCount = 0;
        for (int b = 0; b < 16; ++b) differCount += (differ >> b) & 1u;
        uint64_t candidates[PAL_FADE_TYPES] = { 0 }; /* bit k */
        int probes = 0;
        for (int b = 0; b < 16 && probes < 2; ++b) {
            if (!((differ >> b) & 1u)) continue;
            int i = b * 16 + 1;
            while ((pal[i] & 0x7FFFu) == (src[i] & 0x7FFFu)) ++i; /* the bank differs somewhere */
            for (int type = 0; type < PAL_FADE_TYPES; ++type)
                for (int k = 0; k <= 32; ++k)
                    if (PalFadeColor(type, src[i], k) == (pal[i] & 0x7FFFu)) candidates[type] |= 1ull << k;
            ++probes;
        }
        int bestHits = 0;
        for (int type = 0; type < PAL_FADE_TYPES; ++type) {
            for (int k = 0; k <= 32; ++k) {
                if (!((candidates[type] >> k) & 1u)) continue;
                uint16_t banks = 0;
                int hits = 0;
                for (int b = 0; b < 16; ++b) {
                    if (!PalFadeBankMatches(pal, src, b, type, k)) continue;
                    banks |= (uint16_t)(1u << b);
                    if ((differ >> b) & 1u) ++hits;
                }
                if (hits > bestHits) { bestHits = hits; bestType = type; bestK = k; bestBanks = banks; }
            }
        }
        if (bestHits < 2 || bestHits + 1 < differCount) bestType = -1;
        sLastValid = true;
        sLastPalHash = palHash;
        sLastSrcHash = srcHash;
        sLastType = bestType; sLastK = bestK; sLastBanks = bestBanks;
    }
    if (bestType < 0) return;

    for (int b = 0; b < 16; ++b)
        if ((bestBanks >> b) & 1u) memcpy(&sBgPalEff[b * 16 + 1], &src[b * 16 + 1], 15 * sizeof(uint16_t));
    sPalFadeOn = true;
    sPalFadeBanks = bestBanks;
    const float t = (float)bestK / 32.0f;
    switch (bestType) {
        case PAL_FADE_IN: sPalFadeA = t; sPalFadeB = 0.0f; break;
        case PAL_FADE_FLASH: sPalFadeA = t; sPalFadeB = 1.0f - t; break;
        case PAL_FADE_OUT: sPalFadeA = 1.0f - t; sPalFadeB = 0.0f; break;
        default: sPalFadeA = 1.0f - t; sPalFadeB = t; break;
    }
}

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

/* Power-bomb explosion circular flash (issue #28).
 *
 * On GBA the explosion darkens every BG via BLDCNT brightness-decrease
 * (BLDY) and carves a bright expanding "hole" out of it by resizing the
 * WIN1 rectangle every scanline via an HBlank DMA into REG_WIN1H. This port
 * emulates neither HBlank DMA nor per-scanline IO, so WIN1H stays frozen at
 * 0 -> the darken covers the whole screen uniformly and the circle never
 * appears ("la pantalla se pone oscura pero no se ve el brillo circular").
 *
 * Rather than reintroduce per-scanline work on the CPU, RenderFrame detects
 * the effect (DetectPowerBombFlash) from the live explosion state, draws the
 * scene at full brightness (sBldEffect forced to 0 so no darken is baked
 * into tiles, sWindowActive forced off so the zero-width WIN1 rect stops
 * clipping BG3), then in the per-eye pass lays a translucent-black annulus
 * over everything outside an ellipse centred on the epicentre -- the darken
 * and the bright hole, done entirely on the GPU as ~128 triangles. Fidelity
 * notes: the hole is a smooth ellipse, not GBA's stair-stepped per-line
 * shape; sprites outside the ellipse get dimmed too (GBA only darkens BGs);
 * BG3-outside-the-circle is not re-hidden. */
static bool sPbFlashActive;
static float sPbFlashCxGba, sPbFlashCyGba, sPbFlashRxGba, sPbFlashRyGba;
static int sPbFlashEvy;

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
/* From the per-frame VRAM pass (VramDiffBeginFrame) rather than the tile's
 * bytes. Past the end of VRAM counts as transparent. */
static inline bool TileHasOpaquePixel(uint32_t byteOffset, bool bpp8) {
    const uint32_t c = byteOffset / VRAM_CHUNK_BYTES;
    if (c >= VRAM_CHUNKS) return false;
    if (sChunkOpaque[c]) return true;
    return bpp8 && c + 1 < VRAM_CHUNKS && sChunkOpaque[c + 1];
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
 * brightness adjustment. Records the frame it was decoded in, so a future
 * call can tell from the VRAM change stamps whether the underlying tile
 * changed (see GetOrDecodeTileSlot) without redecoding pixel-by-pixel, and
 * sets this slot's row bit in
 * sDirtyRowMask so the end-of-frame cache flush only covers the atlas
 * rows actually touched this frame. */
/* Writes one 8x8 tile's texels into one atlas slot, and nothing else -- no
 * cache bookkeeping, no dirty-row marking.
 *
 * Each slot is one 64-texel (8x8) block; blocks are stored row-major across
 * the atlas (ATLAS_TILES_PER_ROW blocks per block-row), the same layout
 * GX_TRANSFER_OUT_TILED(1) used to produce -- confirmed by the dirty-row
 * byte-offset math elsewhere in this file (row*8*rowBytes) already relying
 * on exactly this ordering. */
static uint16_t DecodeTileTexels(int slot, const uint8_t* src, bool bpp8, const uint16_t* pal, int palBank,
                             bool hflip, bool vflip, BrightAdjust brightAdjust) {
    AtlasTexel* blockBase = StageSlot(slot);

    if (!bpp8) {
        /* 4bpp: the 16 colours this tile can use, converted (and brightened /
         * darkened) once, then one table read per texel -- the per-texel
         * conversion made a palette animation's mass redecode the costliest
         * part of its frame. */
        AtlasTexel lut[16];
        lut[0] = 0; /* index 0 is transparent */
        for (int i = 1; i < 16; ++i) {
            AtlasTexel c = Bgr555ToRgba5551(pal[palBank * 16 + i], false);
            if (brightAdjust == BRIGHT_ADJUST_BRIGHTEN) c = ApplyBrighten(c, sBldEvy);
            else if (brightAdjust == BRIGHT_ADJUST_DARKEN) c = ApplyDarken(c, sBldEvy);
            lut[i] = c;
        }
        uint16_t used = 0;
        for (int row = 0; row < 8; ++row) {
            const uint8_t* rowSrc = src + (vflip ? (7 - row) : row) * 4;
            const uint32_t bits = (uint32_t)rowSrc[0] | ((uint32_t)rowSrc[1] << 8) |
                                  ((uint32_t)rowSrc[2] << 16) | ((uint32_t)rowSrc[3] << 24);
            for (int col = 0; col < 8; ++col) used |= (uint16_t)(1u << ((bits >> (col * 4)) & 0x0Fu));
            const uint8_t* swz = &kSwizzleLUT[row * 8];
            if (!hflip) {
                for (int col = 0; col < 8; ++col) blockBase[swz[col]] = lut[(bits >> (col * 4)) & 0x0Fu];
            } else {
                for (int col = 0; col < 8; ++col) blockBase[swz[col]] = lut[(bits >> ((7 - col) * 4)) & 0x0Fu];
            }
        }
        return used;
    }

    for (int row = 0; row < 8; ++row) {
        int srcRow = vflip ? (7 - row) : row;
        AtlasTexel pixels[8];
        if (!bpp8) {
            const uint8_t* rowSrc = src + srcRow * 4;
            for (int col = 0; col < 8; ++col) {
                int srcCol = hflip ? (7 - col) : col;
                uint8_t byte = rowSrc[srcCol / 2];
                uint8_t idx = (srcCol & 1) ? (byte >> 4) & 0x0Fu : byte & 0x0Fu;
                pixels[col] = Bgr555ToRgba5551(pal[palBank * 16 + idx], idx == 0);
            }
        } else {
            const uint8_t* rowSrc = src + srcRow * 8;
            for (int col = 0; col < 8; ++col) {
                int srcCol = hflip ? (7 - col) : col;
                uint8_t idx = rowSrc[srcCol];
                pixels[col] = Bgr555ToRgba5551(pal[idx], idx == 0);
            }
        }
        if (brightAdjust == BRIGHT_ADJUST_BRIGHTEN) {
            for (int col = 0; col < 8; ++col) pixels[col] = ApplyBrighten(pixels[col], sBldEvy);
        } else if (brightAdjust == BRIGHT_ADJUST_DARKEN) {
            for (int col = 0; col < 8; ++col) pixels[col] = ApplyDarken(pixels[col], sBldEvy);
        }
        for (int col = 0; col < 8; ++col) blockBase[kSwizzleLUT[row * 8 + col]] = pixels[col];
    }
    return 0xFFFFu; /* 8bpp: colour use not tracked */
}

static void DecodeTileIntoSlot(int slot, const uint8_t* src, bool bpp8, const uint16_t* pal, int palBank,
                               bool hflip, bool vflip, BrightAdjust brightAdjust, uint32_t palHash) {
    sCacheUsedMask[slot] = DecodeTileTexels(slot, src, bpp8, pal, palBank, hflip, vflip, brightAdjust);
    sCacheDecodeStamp[slot] = sFrameStamp;
    ++sPerfCount[PERF_COUNT_TILE_DECODES];
    sCachePalHash[slot] = palHash;
    sCacheEvy[slot] = (brightAdjust != BRIGHT_ADJUST_NONE) ? (uint8_t)sBldEvy : 0;

    MarkAtlasRowDirty(slot / ATLAS_TILES_PER_ROW);
    sAnyDirtySlot = true;
}

/* Returns the atlas slot for this tile, decoding it only if needed. Unlike
 * an earlier version of this cache, entries PERSIST across frames (see the
 * comment on sHashBucketHead above) -- a hash hit means this exact (offset,
 * bpp, palette bank, flip, brightness) combination was decoded on some
 * earlier frame, but the underlying VRAM bytes might have changed since
 * (animated tiles: water, lava, etc.), so the tile's VRAM change stamp is
 * checked against the frame the slot was decoded in before trusting the
 * cached pixels (see VramDiffBeginFrame). */
static int GetOrDecodeTileSlot(uint32_t byteOffset, bool bpp8, const uint16_t* pal, int palBank, bool hflip,
                               bool vflip, bool isObj, BrightAdjust brightAdjust) {
    TileCacheKey key = {
        byteOffset,       (uint8_t)bpp8,   (uint8_t)(bpp8 ? 0 : palBank), (uint8_t)hflip,
        (uint8_t)vflip,   (uint8_t)isObj,  (uint8_t)brightAdjust,
    };
    const uint8_t* src = gVram + byteOffset;
    /* Precomputed once per frame (see sBgPalBankHash/sBgPalFullHash/
     * sObjPalBankHash/sObjPalFullHash) instead of hashing/comparing
     * palette bytes per reference -- see sCachePalHash's comment. */
    const uint32_t palHash =
        isObj ? (bpp8 ? sObjPalFullHash : sObjPalBankHash[palBank]) : (bpp8 ? sBgPalFullHash : sBgPalBankHash[palBank]);
    const uint8_t curEvy = (brightAdjust != BRIGHT_ADJUST_NONE) ? (uint8_t)sBldEvy : 0;
    ++sPerfCount[PERF_COUNT_TILE_LOOKUPS];
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
        if (sCacheDecodeStamp[i] >= TileChangeStamp(byteOffset, bpp8) && sCachePalHash[i] == palHash &&
            sCacheEvy[i] == curEvy) {
            return i;
        }
        DecodeTileIntoSlot(i, src, bpp8, pal, palBank, hflip, vflip, brightAdjust, palHash);
        return i;
    }
    if (sCacheCount >= ATLAS_MAX_SLOTS - 1) { /* the last slot is LM_TRANSPARENT_SLOT */
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

static inline int AllocDrawItemSubtex(const Tex3DS_SubTexture* subtex, int sortKey) {
    if (sDrawItemCount >= MAX_DRAW_ITEMS) return -1;
    int idx = sDrawItemCount++;
    DrawItem* item = &sDrawItems[idx];
    item->img.tex = &sAtlasTexture;
    item->img.subtex = subtex;
    item->sortKey = sortKey;
    item->screenFixed = false;
    item->gpuFx = sPushFx;

    sBucketNext[idx] = -1;
    if (sBucketHead[sortKey] < 0) sBucketHead[sortKey] = idx;
    else sBucketNext[sBucketTail[sortKey]] = idx;
    sBucketTail[sortKey] = idx;
    return idx;
}

static inline int AllocDrawItem(int slot, int sortKey) {
    return AllocDrawItemSubtex(&sSlotSubtexTable[slot], sortKey);
}

/* Size-carrying push. BuildDrawParams reads item->w/h for non-affine
 * items. */
static inline void PushItemSubtex(const Tex3DS_SubTexture* subtex, float x, float y, float w, float h,
                                  int sortKey, int depthTier, bool blendAlpha, WindowVis winVis,
                                  bool isHud) {
    int idx = AllocDrawItemSubtex(subtex, sortKey);
    if (idx < 0) return;
    DrawItem* item = &sDrawItems[idx];
    item->x = x + sCollectOffX;
    item->y = y + sCollectOffY;
    item->w = w;
    item->h = h;
    item->angle = 0.0f;
    item->depthTier = (int8_t)depthTier;
    item->blendAlpha = blendAlpha;
    item->affine = false;
    item->winVis = winVis;
    item->isHud = isHud;
    item->plainEnv = false;
}

static inline void PushItem(int slot, float x, float y, int sortKey, int depthTier, bool blendAlpha,
                            WindowVis winVis, bool isHud) {
    PushItemSubtex(&sSlotSubtexTable[slot], x, y, 8.0f, 8.0f, sortKey, depthTier, blendAlpha, winVis,
                   isHud);
}

/* Affine OBJ variant: x,y is the subtile's already-transformed screen-space
 * CENTER (not top-left), w/h are the decomposed scaled subtile size (see
 * CollectSprite's affine path), and angle (radians) is the sprite's
 * decomposed rotation -- consumed by the draw loop via C2D_DrawParams'
 * center+angle instead of the plain top-left placement non-affine items use. */
static inline void PushAffineItem(int slot, float centerX, float centerY, float angle, float scaleX, float scaleY,
                                  int sortKey, int depthTier, bool blendAlpha, WindowVis winVis, uint8_t bleedEdges) {
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
    item->affBleedEdges = bleedEdges;
    item->winVis = winVis;
    item->isHud = false;
    item->plainEnv = false;
}

/* ---- Affine BG2 (GBA mode 1) ---- */

/* True iff the current frame is the exact supported case: mode 1, BG2 on,
 * 256x256 map, no mosaic, PURE SCALE (PB=PC=0, PA>0). Fills the sAffineBg2*
 * cache. Pure read of gIoMem; safe to call from CanRenderFrame and again
 * from RenderFrame. */
static bool DetectAffineBg2(void) {
    if (!sAffineBg2TexReady) return false;
    uint16_t dispcnt = (uint16_t)(gIoMem[0] | (gIoMem[1] << 8));
    if ((dispcnt & 7u) != 1u) return false;              /* not mode 1 */
    if (!(dispcnt & (1u << 10))) return false;           /* BG2 disabled */
    uint16_t bg2cnt = (uint16_t)(gIoMem[0x0C] | (gIoMem[0x0D] << 8));
    if (((bg2cnt >> 14) & 3u) != 1u) return false;       /* not 256x256 */
    if ((bg2cnt >> 6) & 1u) return false;                /* BG2 mosaic */
    int16_t pa = (int16_t)(gIoMem[0x20] | (gIoMem[0x21] << 8));
    int16_t pb = (int16_t)(gIoMem[0x22] | (gIoMem[0x23] << 8));
    int16_t pc = (int16_t)(gIoMem[0x24] | (gIoMem[0x25] << 8));
    if (pb != 0 || pc != 0) return false;                /* rotation / shear */
    if (pa <= 0) return false;
    /* BG2X/BG2Y: 28-bit signed, 20.8 fixed. Sign-extend from bit 27. */
    int32_t bg2x = (int32_t)((uint32_t)gIoMem[0x28] | ((uint32_t)gIoMem[0x29] << 8) |
                             ((uint32_t)gIoMem[0x2A] << 16) | ((uint32_t)gIoMem[0x2B] << 24));
    int32_t bg2y = (int32_t)((uint32_t)gIoMem[0x2C] | ((uint32_t)gIoMem[0x2D] << 8) |
                             ((uint32_t)gIoMem[0x2E] << 16) | ((uint32_t)gIoMem[0x2F] << 24));
    bg2x = (bg2x << 4) >> 4;
    bg2y = (bg2y << 4) >> 4;
    sAffineBg2InvScale   = 256.0f / (float)pa;
    sAffineBg2RefX       = (float)bg2x / 256.0f;
    sAffineBg2RefY       = (float)bg2y / 256.0f;
    sAffineBg2CharBase   = ((bg2cnt >> 2) & 3u) * 0x4000u;
    sAffineBg2ScreenBase = ((bg2cnt >> 8) & 0x1Fu) * 0x800u;
    return true;
}

/* CPU-decode the 256x256 8bpp affine tilemap (32x32 tiles, 1 index byte per
 * tile) into sAffineBg2Tex, swizzled, same colour path as the atlas. */
static void ComposeAffineBg2(void) {
    extern uint8_t gVram[];
    const uint16_t* pal = (const uint16_t*)gBgPltt;      /* 256 entries (8bpp) */
    const uint8_t* map  = gVram + sAffineBg2ScreenBase;   /* 32*32 index bytes */
    const uint8_t* chr  = gVram + sAffineBg2CharBase;
    AtlasTexel* dst = (AtlasTexel*)sAffineBg2Tex.data;
    for (int cy = 0; cy < 32; ++cy) {
        for (int cx = 0; cx < 32; ++cx) {
            const uint8_t* g = chr + (uint32_t)map[cy * 32 + cx] * 64u;
            int bx = cx * 8, by = cy * 8;
            for (int py = 0; py < 8; ++py) {
                for (int px = 0; px < 8; ++px) {
                    uint8_t idx = g[py * 8 + px];
                    int tx = bx + px, ty = by + py;
                    u32 tile = (uint32_t)(ty / 8) * (AFF_BG2_DIM / 8) + (uint32_t)(tx / 8);
                    dst[tile * 64u + kSwizzleLUT[(ty % 8) * 8 + (tx % 8)]] =
                        Bgr555ToRgba5551(pal[idx], idx == 0);
                }
            }
        }
    }
    FlushAtlasRange(sAffineBg2Tex.data, (size_t)AFF_BG2_DIM * AFF_BG2_DIM * sizeof(AtlasTexel));
}

/* Push the affine BG2 as one scaled quad at BG2's priority. Screen rect: the
 * texture's (0,0) sits at screen (-refX,-refY)*invScale and it spans
 * 256*invScale px; overflow is transparent so nothing outside that rect is
 * drawn. Stereo comes from BG2's tier like any other BG layer. */
static void CollectAffineBg2(void) {
    /* Composed in the draw half (AtlasApplyStaged): the texture may still be
     * sampled by the previous frame while this one is collected. */
    sAffineBg2ComposePending = true;
    static Tex3DS_SubTexture full;
    full = (Tex3DS_SubTexture){ AFF_BG2_DIM, AFF_BG2_DIM, 0.0f, 1.0f, 1.0f, 0.0f };
    int priority = sDepthState.priority[2];
    int sortKey = (3 - priority) * 10 + (3 - 2);
    int idx = AllocDrawItemSubtex(&full, sortKey);
    if (idx < 0) return;
    DrawItem* item = &sDrawItems[idx];
    item->img.tex = &sAffineBg2Tex;
    item->x = -sAffineBg2RefX * sAffineBg2InvScale;
    item->y = -sAffineBg2RefY * sAffineBg2InvScale;
    item->w = (float)AFF_BG2_DIM * sAffineBg2InvScale;
    item->h = item->w;
    item->angle = 0.0f;
    item->depthTier = (int8_t)PortStereoDepth_BgTier(&sDepthState, 2);
    item->blendAlpha = false;
    item->affine = false;
    item->affBleedEdges = 0;
    item->winVis = WIN_VIS_ALWAYS;
    item->isHud = false;
    item->plainEnv = false;   /* atlas texenv: RGBA5551, like the atlas */
}

/* Text-mode BG tilemap addressing, byte-identical to the formula validated
 * in port/ppu/src/mode1.c (screen_block_x/y + blocks_per_row quadrant
 * layout for the 32x32/64x32/32x64/64x64 GBA screen sizes). */

/* ---- WIDE view (port_wide_view.h) ----------------------------------------
 * The GBA only keeps the part of a room around the camera in VRAM, and only
 * processes sprites inside its 240x160 frame. Showing more world therefore
 * needs two things: the game side keeps sprites alive further out (the culling
 * margins in src/), and here the tiles past what VRAM holds are rebuilt from
 * the room's own block maps -- the same data the game copies into VRAM as the
 * camera moves.
 *
 * sExtL/R/T/B: how far past each edge of the 240x160 frame, in GBA px, this
 * frame collects tiles and sprites. Not simply the margin on both sides: the
 * view slides inside the room (WideView), so the range follows it. Both zero unless the frame is widened, in which
 * case every range below collapses back to the original 240x160 one. */
static bool sWideOn;
static int sExtL, sExtR, sExtT, sExtB;

/* WIDE view geometry for the frame -- see ComputeWideView. */
typedef struct WideView {
    int originX, originY;   /* room px at the GBA frame's top-left */
    int shiftX, shiftY;
    int loX, hiX, loY, hiY; /* room extent, room px */
    /* The room extent in GBA frame px (extent minus origin): what
     * DrawWideRoomMasks blacks out beyond. */
    int maskL, maskR, maskT, maskB;
} WideView;
static WideView sWideView;
/* The margin the setting asks for, before the view slides (see WideView). */
static int sWideMarginX, sWideMarginY;

enum { COV_ROWS = 40 };

typedef struct WideBgSource {
    bool on;
    const uint16_t* blocks;   /* the room's block map for this BG, blocksW x blocksH */
    int blocksW, blocksH;
    const uint16_t* tilemap;  /* block -> its four tile entries (TL, TR, BL, BR) */
    int originTileX, originTileY; /* absolute room tile drawn at screen tile (0,0) */
} WideBgSource;

/* Whether screen tile (tx,ty) is one VRAM is known to hold for this BG: the
 * tiles the plain 240x160 path reads. Anything else in a widened frame comes
 * from the block map instead (WideBgEntry). */
static inline bool TileInVramWindow(int tx, int ty) {
    return tx >= -1 && tx <= 31 && ty >= 0 && ty <= 20;
}

/* Sets up the room-data source for a text BG, or leaves it off when the BG
 * has no block map to rebuild from. Only "RLE" BGs have one -- the level
 * layers, streamed into a VRAM window as the camera moves. The LZ77 ones
 * (clouds, dark-room mask, the BG3 backdrop) hold their whole map in VRAM and
 * wrap, which is also right for the extra area, so they read VRAM as usual.
 *
 * scrollX/scrollY are the hardware scroll registers. The room position the
 * game tracks (gBgNPosition, sub-pixels) says where the BG really is; the two
 * differ only by the screen-shake offset the game adds to the registers, and
 * folding that back in keeps rebuilt tiles on the same grid as VRAM ones. */
static WideBgSource WideBgSourceFor(int bgIndex, int scrollX, int scrollY) {
    extern uint16_t gBg0XPosition, gBg0YPosition, gBg2XPosition, gBg2YPosition;
    extern uint8_t gCurrentRoomEntry[]; /* struct RoomEntry: u8 tileset, then bg0..bg3Prop */
    extern struct { u16* pTilemap; } gTilemapAndClipPointers;
    enum { BG_PROP_RLE_COMPRESSED = 1 << 4 }; /* include/constants/room.h */

    WideBgSource out = { 0 };
    if (bgIndex < 0 || bgIndex > 2) return out;
    if (!(gCurrentRoomEntry[1 + bgIndex] & BG_PROP_RLE_COMPRESSED)) return out;

    out.blocks = gBgPointersAndDimensions.backgrounds[bgIndex].pDecomp;
    out.blocksW = gBgPointersAndDimensions.backgrounds[bgIndex].width;
    out.blocksH = gBgPointersAndDimensions.backgrounds[bgIndex].height;
    out.tilemap = gTilemapAndClipPointers.pTilemap;
    if (!out.blocks || !out.tilemap || out.blocksW <= 0 || out.blocksH <= 0) return out;

    int posX, posY;
    switch (bgIndex) {
        case 0: posX = gBg0XPosition; posY = gBg0YPosition; break;
        case 1: posX = gBg1XPosition; posY = gBg1YPosition; break;
        default: posX = gBg2XPosition; posY = gBg2YPosition; break;
    }
    const int roomX = posX / 4, roomY = posY / 4; /* sub-pixels -> px */
    int shakeX = (scrollX - roomX) & 0x1FF;
    int shakeY = (scrollY - roomY) & 0x1FF;
    if (shakeX >= 256) shakeX -= 512;
    if (shakeY >= 256) shakeY -= 512;
    out.originTileX = (roomX + shakeX) >> 3;
    out.originTileY = (roomY + shakeY) >> 3;
    out.on = true;
    return out;
}

/* The tilemap entry the game would have put in VRAM for screen tile (tx,ty).
 * Outside the room's block map it is 0, like the cleared VRAM there. */
static inline uint16_t WideBgEntry(const WideBgSource* src, int tx, int ty) {
    ++sPerfCount[PERF_COUNT_WIDE_REBUILT];
    const int ax = src->originTileX + tx;
    const int ay = src->originTileY + ty;
    const int bx = ax >> 1, by = ay >> 1;
    if (bx < 0 || by < 0 || bx >= src->blocksW || by >= src->blocksH) return 0;
    const unsigned block = src->blocks[bx + by * src->blocksW];
    /* pTilemap is gTilemap, and the game indexes past it on purpose: blocks
     * 0x400.. land in gCommonTilemap, which sits right after it (hatches,
     * for one, live there -- 0x411..). ewram_symbols.ld keeps that layout:
     * 0x400 room blocks, then 0x100 common ones. */
    if (block >= 0x500u) return 0;
    return src->tilemap[block * 4u + (unsigned)(ay & 1) * 2u + (unsigned)(ax & 1)];
}

/* How far a BG slides when the WIDE view slides by `shift` (GBA px): the
 * game scrolls some layers slower than the camera (PortPpuMzm_WideLayerDivisor),
 * and those must slide proportionally less. */
static int WideLayerShift(int bgIndex, int shift, bool vertical) {
    extern int PortPpuMzm_WideLayerDivisor(int bg, int vertical);
    if (shift == 0) return 0;
    const int div = PortPpuMzm_WideLayerDivisor(bgIndex, vertical ? 1 : 0);
    return div > 0 ? shift / div : 0;
}

/* One layer-map cell for this frame: redraw it (queue an op) if it holds a
 * different entry, or the layer state, its tile's pixels or its palette bank
 * changed since it was drawn. `corrected` cells are kept transparent in the
 * map, but their atlas slot is kept current all the same: the per-tile pass
 * that draws them takes it from the cell instead of looking the tile up. */
static inline void LayerMapCheckCell(int bg, LayerMapPass* pass, int cellCol, int cellRow, uint16_t entry,
                                     uint32_t charBase, bool bpp8, bool corrected) {
    LayerMapCell* cell = &sLmCells[bg][cellRow][cellCol];
    const uint16_t flags = corrected ? LM_CELL_CORRECTED : 0;
    const uint32_t byteOffset = charBase + (uint32_t)(entry & 0x3FFu) * (bpp8 ? 64u : 32u);
    const int bank = (entry >> 12) & 0x0F;
    /* Palette: for 4bpp only the colours the cell's tile uses count (a
     * palette animation usually steps a couple of entries of one bank). */
    const bool stale =
        sLmStateStamp[bg] > cell->stamp || cell->entry != entry || cell->flags != flags ||
        (bpp8 ? sLmBankStamp[bg][16] > cell->stamp : BgColorsChangedSince(bank, cell->used, cell->stamp)) ||
        TileChangeStamp(byteOffset, bpp8) > cell->stamp;
    if (!stale) return;
    /* A corrected cell only needs its map texels cleared when they are not
     * already (it was not corrected before, or was never drawn). */
    const bool needsOp = !corrected || cell->flags != LM_CELL_CORRECTED || cell->stamp == 0;
    if (needsOp && pass->ops >= LM_MAX_OPS) {
        /* Op list full (cannot happen with one check per visible cell, but a
         * cell checked twice in a frame must not run past the arrays): leave
         * the cell stale so the next frame redraws it. */
        cell->stamp = 0;
        return;
    }
    int slot = LM_TRANSPARENT_SLOT;
    if (TileHasOpaquePixel(byteOffset, bpp8)) {
        /* Same tile as before under the same layer state (a palette step, an
         * animation frame): the slot it came from still holds that key, so
         * refresh it in place rather than looking it up again -- a palette
         * animation touches every visible cell of its bank at once, and the
         * cache lookups were the expensive part of that frame. */
        if (cell->entry == entry && cell->flags == flags && cell->slotGen == sAtlasGen &&
            cell->slot >= 0 && cell->slot != LM_TRANSPARENT_SLOT && sLmStateStamp[bg] <= cell->stamp) {
            slot = cell->slot;
            uint32_t* fresh = &sSlotFreshThisFrame[slot >> 5];
            if (!((*fresh >> (slot & 31)) & 1u)) {
                const uint32_t palHash = bpp8 ? sBgPalFullHash : sBgPalBankHash[bank];
                /* The slot is stale for the palette only if a colour it uses
                 * changed since it was decoded; if not, its pixels are still
                 * exact and its hash is simply brought up to date. */
                const bool palStale = bpp8 ? sCachePalHash[slot] != palHash
                                           : BgColorsChangedSince(bank, sCacheUsedMask[slot], sCacheDecodeStamp[slot]);
                if (sCacheDecodeStamp[slot] < TileChangeStamp(byteOffset, bpp8) || palStale)
                    DecodeTileIntoSlot(slot, gVram + byteOffset, bpp8, sBgPalEff, bank, (entry & 0x0400u) != 0,
                                       (entry & 0x0800u) != 0, BRIGHT_ADJUST_NONE, palHash);
                else
                    sCachePalHash[slot] = palHash;
                *fresh |= 1u << (slot & 31);
            }
        } else {
            slot = GetOrDecodeTileSlot(byteOffset, bpp8, sBgPalEff, bank, (entry & 0x0400u) != 0,
                                       (entry & 0x0800u) != 0, false, BRIGHT_ADJUST_NONE);
            if (slot >= 0) sSlotFreshThisFrame[slot >> 5] |= 1u << (slot & 31);
        }
    }
    cell->entry = entry;
    cell->flags = flags;
    cell->stamp = pass->stampNow;
    cell->slot = (int16_t)slot;
    cell->slotGen = sAtlasGen;
    cell->used = (slot == LM_TRANSPARENT_SLOT) ? 0u : sCacheUsedMask[slot];
    if (!needsOp) return;
    sLmOps[bg][pass->ops] = (uint16_t)(cellRow * LM_COLS + cellCol);
    sLmOpSlot[bg][pass->ops] = (int16_t)(corrected ? LM_TRANSPARENT_SLOT : slot);
    ++pass->ops;
}

/* Whether a per-tile correction touches a 16x16 room block: the per-tile
 * pass's answer when the correction grid does not cover the view. */
static inline bool BlockGroupNeedsPerTile(int bgIndex, int blockX, int blockY,
                                          bool hasLayerFix, bool hasTank, bool hasDoor) {
    extern int PortLayerFix_DestFor(int bg, int blockX, int blockY);
    extern bool PortPpuMzm_IsVisibleTankBlock(int blockX, int blockY);
    extern bool PortPpuMzm_IsDoorDepthBlock(int blockX, int blockY);
    return (hasLayerFix && PortLayerFix_DestFor(bgIndex, blockX, blockY) >= 0) ||
           (hasTank && PortPpuMzm_IsVisibleTankBlock(blockX, blockY)) ||
           (hasDoor && PortPpuMzm_IsDoorDepthBlock(blockX, blockY));
}

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

    const uint16_t* pal = sBgPalEff;
    const int bytesPerTile = bpp8 ? 64 : 32;

    /* BLDCNT: is this BG a first-target layer for the active effect? Only
     * matters when an effect is active at all (sBldEffect != 0). */
    bool isFirstTarget = sBldEffect != 0 && BldIsFirstTarget(sIoBldcnt, bgIndex);
    /* Brighten / darken is applied when the layer is drawn, not decoded into
     * its tiles (see ConfigureFxTextureEnv): every item this layer pushes
     * carries it. */
    const uint8_t layerFx = (isFirstTarget && (sBldEffect == 2 || sBldEffect == 3)) ? ITEM_FX_BRIGHT : 0;
    const BrightAdjust brightAdjust = BRIGHT_ADJUST_NONE;
    sPushFx = layerFx;
    bool blendAlpha = isFirstTarget && sBldEffect == 1;
    int startTileX = scrollX / 8;
    int startTileY = scrollY / 8;
    int fineX = scrollX % 8;
    int fineY = scrollY % 8;

    /* WIDE: this layer slides by its own share of the view's slide (a
     * parallax backdrop moving at half the camera's speed slides half as
     * far), so its visible range and draw offset are its own. */
    const int lsX = WideLayerShift(bgIndex, sWideView.shiftX, false);
    const int lsY = WideLayerShift(bgIndex, sWideView.shiftY, true);
    const int eL = sWideMarginX - lsX, eR = sWideMarginX + lsX;
    const int eT = sWideMarginY - lsY, eB = sWideMarginY + lsY;
    sCollectOffX = (float)(sWideView.shiftX - lsX);
    sCollectOffY = (float)(sWideView.shiftY - lsY);

    /* Tile ranges the passes below walk. Without the WIDE view they are the
     * original ones (-1..31 by 0..20); with it they grow by however many
     * tiles cover the extra area. wideSrc rebuilds the tiles VRAM does not
     * hold out there -- see WideBgSourceFor. */
    const int txMin = -1 - (eL + 7) / 8, txMax = 31 + (eR + 7) / 8;
    const int tyMin = -(eT + 7) / 8, tyMax = 20 + (eB + 7) / 8;
    /* `covered` is indexed from the range's own start, so it fits however far
     * the view reaches (at most ~54 tiles across, ~32 down). */
    const int covX = -txMin, covY = -tyMin;
    const WideBgSource wideSrc = sWideOn ? WideBgSourceFor(bgIndex, scrollX, scrollY) : (WideBgSource){ 0 };

    /* A room with a visible item tank forces the per-tile path on BG1 (the
     * clipdata layer): the tank is an animated tile with no block-map entry,
     * so it can only be recognised and lifted to Samus's plane one tile at a
     * time -- a cached layer quad or a 16x16 atlas block carries a single
     * depth tier and cannot. Same trade the layer-fix list already makes. */
    extern int PortPpuMzm_RoomTankCount(void);
    extern bool PortPpuMzm_IsVisibleTankBlock(int blockX, int blockY);
    extern bool PortPpuMzm_IsDoorDepthBlock(int blockX, int blockY);
    const bool roomHasTankOnThisBg =
        (bgIndex == 1) && PortPpuMzm_RoomTankCount() > 0;
    /* Door depth pull covers BG0/BG1/BG2 -- the lintel/sill trim that has to
     * rejoin the frame lives on BG2 too. Never BG3 (the LZ77 backdrop). The
     * footprint is deliberately tiny (door span, no side growth, a row or two
     * above/below) so only actual doorway tiles are caught, not open wall. */
    const bool roomHasDoorDepth = sDoorDepthOnScreen && bgIndex < 3;

    /* World tile shown at screen (0,0), de-wrapped from the 9-bit BG scroll
     * via the camera (PortPpuMzm_ScreenOrigin). The per-block layer
     * corrections (port_layer_fixes.c) are keyed to ABSOLUTE room-block
     * positions, so screen tile (tx,ty) has to be turned back into a room
     * block before the lookup -- matching on the raw screenmap position
     * aliased every correction onto a 32x16-block lattice (an entry for
     * block (9,43) also fired on (9,59)). Every room layer a correction can
     * target scrolls with the camera 1:1, so one origin serves them all.
     * Only computed when a list is actually compiled in. */
    const bool sHasLayerFix = PortLayerFix_ActiveCount() > 0;
    const bool roomHasBlockCorrections = sHasLayerFix || roomHasTankOnThisBg || roomHasDoorDepth;
    int fixOriginTileX = 0, fixOriginTileY = 0;
    if (roomHasBlockCorrections) {
        extern void PortPpuMzm_ScreenOrigin(int* outX, int* outY);
        int originX = 0, originY = 0;
        PortPpuMzm_ScreenOrigin(&originX, &originY);
        fixOriginTileX = originX >> 3;
        fixOriginTileY = originY >> 3;
    }
    /* Which corrections touch each 16x16 room block in view, looked up once
     * per block per frame: CORR_LAYER_FIX / CORR_TANK / CORR_DOOR. The
     * passes below used to ask the three lists again for every TILE of every
     * group they examined -- sixteen linear scans per 32x32 group, in almost
     * every room (any door on screen counts). */
    enum { CORR_LAYER_FIX = 1, CORR_TANK = 2, CORR_DOOR = 4, CORR_GRID_W = 40, CORR_GRID_H = 24 };
    uint8_t corrGrid[CORR_GRID_H][CORR_GRID_W];
    const int corrBX0 = (fixOriginTileX + txMin) >> 1; /* arithmetic shift: floor */
    const int corrBY0 = (fixOriginTileY + tyMin) >> 1;
    const int corrW = ((fixOriginTileX + txMax) >> 1) - corrBX0 + 1;
    const int corrH = ((fixOriginTileY + tyMax) >> 1) - corrBY0 + 1;
    const bool corrGridOk = corrW <= CORR_GRID_W && corrH <= CORR_GRID_H;
    if (roomHasBlockCorrections && corrGridOk) {
        /* Painted from the lists rather than asking them about every block:
         * a door span or a tank marks the grid cells it covers. */
        extern int PortLayerFix_DestFor(int bg, int blockX, int blockY);
        extern int PortPpuMzm_DoorDepthCount(void);
        extern void PortPpuMzm_DoorDepthSpan(int i, int* y, int* x0, int* x1);
        extern void PortPpuMzm_RoomTankBlock(int i, int* blockX, int* blockY);
        memset(corrGrid, 0, sizeof(corrGrid));
        if (roomHasDoorDepth) {
            const int n = PortPpuMzm_DoorDepthCount();
            for (int i = 0; i < n; ++i) {
                int y, x0, x1;
                PortPpuMzm_DoorDepthSpan(i, &y, &x0, &x1);
                const int gy = y - corrBY0;
                if (gy < 0 || gy >= corrH) continue;
                for (int gx = (x0 - corrBX0 < 0 ? 0 : x0 - corrBX0); gx <= x1 - corrBX0 && gx < corrW; ++gx)
                    corrGrid[gy][gx] |= CORR_DOOR;
            }
        }
        if (roomHasTankOnThisBg) {
            const int n = PortPpuMzm_RoomTankCount();
            for (int i = 0; i < n; ++i) {
                int bx, by;
                PortPpuMzm_RoomTankBlock(i, &bx, &by);
                const int gx = bx - corrBX0, gy = by - corrBY0;
                if (gx >= 0 && gx < corrW && gy >= 0 && gy < corrH) corrGrid[gy][gx] |= CORR_TANK;
            }
        }
        if (sHasLayerFix) {
            for (int gy = 0; gy < corrH; ++gy)
                for (int gx = 0; gx < corrW; ++gx)
                    if (PortLayerFix_DestFor(bgIndex, corrBX0 + gx, corrBY0 + gy) >= 0)
                        corrGrid[gy][gx] |= CORR_LAYER_FIX;
        }
    }
    /* The corrections touching screen tile (tx,ty)'s 16x16 room block. */
    #define TILE_CORR_MASK(TX, TY)                                                          \
        (!roomHasBlockCorrections ? 0u                                                     \
         : corrGridOk ? (unsigned)corrGrid[((fixOriginTileY + (TY)) >> 1) - corrBY0]        \
                                          [((fixOriginTileX + (TX)) >> 1) - corrBX0]        \
         : (BlockGroupNeedsPerTile(bgIndex, (fixOriginTileX + (TX)) >> 1,                   \
                                   (fixOriginTileY + (TY)) >> 1, sHasLayerFix,              \
                                   roomHasTankOnThisBg, roomHasDoorDepth)                   \
                ? (unsigned)(CORR_LAYER_FIX | CORR_TANK | CORR_DOOR) : 0u))

    /* Positions already drawn (by the layer map): the per-tile pass below
     * skips them. */
    uint64_t covered[COV_ROWS]; /* bit tx + covX of row ty + covY */
    memset(covered, 0, sizeof(covered));
    /* ---- Layer map (see LayerMapInit) ---------------------------------
     * Takes every visible position that has no per-tile correction: redraws
     * the cells that went stale, marks the positions covered, and leaves the
     * corrected ones to the per-tile pass below. Without it (its target could
     * not be allocated, or OBJWIN resolves visibility per tile) every
     * position goes through the per-tile pass. */
    const bool useLayerMap = sLmReady[bgIndex] && !sObjWindowActive;
    const u64 tLm = svcGetSystemTick();
    if (useLayerMap) {
        /* Cells hold the layer without brighten/darken or a palette fade: the
         * quad applies them (ConfigureFxTextureEnv), so a fade does not make
         * the whole map stale every frame. During a palette fade the banks
         * it does not cover keep their own colours, which the quad must not
         * fade: their cells are left out like corrected ones and drawn by
         * the per-tile pass. */
        LayerMapPass pass = LayerMapBegin(bgIndex, charBase, bpp8);
        const uint16_t fadeExcl = sPalFadeOn ? (uint16_t)~sPalFadeBanks : 0u;
#define LM_FADE_EXCLUDED(E) (((fadeExcl >> ((E) >> 12)) & 1u) != 0u)
        const uint32_t now = pass.stampNow;

        /* The visible tile rect: the positions the old per-tile pass drew. */
        int vTxLo = txMax + 1, vTxHi = txMin - 1, vTyLo = tyMax + 1, vTyHi = tyMin - 1;
        for (int tx = txMin; tx <= txMax; ++tx) {
            const float drawX = (float)(tx * 8 - fineX);
            if (drawX <= -16.0f - (float)eL || drawX >= 248.0f + (float)eR) continue;
            if (tx < vTxLo) vTxLo = tx;
            vTxHi = tx;
        }
        for (int ty = tyMin; ty <= tyMax; ++ty) {
            const float drawY = (float)(ty * 8 - fineY);
            if (drawY <= -8.0f - (float)eT || drawY >= 160.0f + (float)eB) continue;
            if (ty < vTyLo) vTyLo = ty;
            vTyHi = ty;
        }

        /* One position: its entry (VRAM, or the room's block map out in the
         * WIDE margins), its correction, and the cell check. */
#define LM_PROCESS(TX, TY)                                                                          \
        do {                                                                                        \
            const int tx_ = (TX), ty_ = (TY);                                                       \
            const int tileRow_ = (startTileY + ty_) & (mapHeightTiles - 1);                         \
            const int tileCol_ = (startTileX + tx_) & (mapWidthTiles - 1);                          \
            const uint32_t mapAddr_ = screenBase +                                                  \
                (uint32_t)(tileCol_ / 32 + (tileRow_ / 32) * blocksPerRow) * 0x800u +               \
                (uint32_t)((tileRow_ % 32) * 32 + tileCol_ % 32) * 2u;                              \
            const uint16_t entry_ = (wideSrc.on && !TileInVramWindow(tx_, ty_))                     \
                                        ? WideBgEntry(&wideSrc, tx_, ty_)                           \
                                        : (uint16_t)(gVram[mapAddr_] | (gVram[mapAddr_ + 1] << 8)); \
            LayerMapCheckCell(bgIndex, &pass, (startTileX + tx_) & (LM_COLS - 1),                   \
                              (startTileY + ty_) & (LM_ROWS - 1), entry_, charBase, bpp8,           \
                              TILE_CORR_MASK(tx_, ty_) != 0u || LM_FADE_EXCLUDED(entry_));           \
        } while (0)

        /* A full look is needed when anything that maps positions to entries
         * or corrections changed: the tilemap's place and size, the WIDE
         * source and how it lines up, the correction lists, the layer state. */
        extern int PortLayerFix_ActiveCount(void);
        uint32_t key = 2166136261u;
#define LM_KEY(V) (key = (key ^ (uint32_t)(V)) * 16777619u)
        LM_KEY(screenBase); LM_KEY(mapWidthTiles); LM_KEY(mapHeightTiles); LM_KEY(bpp8);
        LM_KEY(wideSrc.on); LM_KEY(wideSrc.originTileX - startTileX); LM_KEY(wideSrc.originTileY - startTileY);
        LM_KEY((uintptr_t)wideSrc.blocks);
        LM_KEY(roomHasBlockCorrections); LM_KEY(roomHasTankOnThisBg ? PortPpuMzm_RoomTankCount() : 0);
        LM_KEY(roomHasDoorDepth); LM_KEY(PortLayerFix_ActiveCount());
        LM_KEY(fixOriginTileX - startTileX); LM_KEY(fixOriginTileY - startTileY);
        LM_KEY(fadeExcl);
#undef LM_KEY
        LayerMapScan* sc = &sLmScan[bgIndex];
        int dX = ((startTileX - sc->startTileX) + 32) & 63; dX -= 32;
        int dY = ((startTileY - sc->startTileY) + 32) & 63; dY -= 32;
        const bool full = !sc->valid || sc->key != key || sLmStateStamp[bgIndex] == now ||
                          sc->frame + 1 != now ||
                          dX > 16 || dX < -16 || dY > 16 || dY < -16;
        if (full) {
            for (int ty = vTyLo; ty <= vTyHi; ++ty)
                for (int tx = vTxLo; tx <= vTxHi; ++tx) LM_PROCESS(tx, ty);
        } else {
            /* 1. Positions that came into view (the old rect, shifted by the
             *    scroll, is what was checked last frame). */
            const int oTxLo = sc->txLo - dX, oTxHi = sc->txHi - dX;
            const int oTyLo = sc->tyLo - dY, oTyHi = sc->tyHi - dY;
            for (int ty = vTyLo; ty <= vTyHi; ++ty) {
                const bool rowWasIn = ty >= oTyLo && ty <= oTyHi;
                for (int tx = vTxLo; tx <= vTxHi; ++tx)
                    if (!rowWasIn || tx < oTxLo || tx > oTxHi) LM_PROCESS(tx, ty);
            }
            if (sChunksChangedNow > 0) {
                /* 2. Tilemap entries the game rewrote this frame: each changed
                 *    32-byte chunk is 16 entries of one tilemap row. */
                const uint32_t mapBytes = (uint32_t)(mapWidthTiles / 32) * (uint32_t)(mapHeightTiles / 32) * 0x800u;
                const uint32_t c0 = screenBase / VRAM_CHUNK_BYTES;
                uint32_t c1 = (screenBase + mapBytes) / VRAM_CHUNK_BYTES;
                if (c1 > VRAM_CHUNKS) c1 = VRAM_CHUNKS;
                for (uint32_t c = c0; c < c1; ++c) {
                    if (sChunkStamp[c] != now) continue;
                    const uint32_t off = c * VRAM_CHUNK_BYTES - screenBase;
                    const int sb = (int)(off / 0x800u), within = (int)(off % 0x800u);
                    const int mapRow = (sb / blocksPerRow) * 32 + within / 64;
                    const int mapCol0 = (sb % blocksPerRow) * 32 + (within % 64) / 2;
                    const int ty0 = (mapRow - startTileY) & (mapHeightTiles - 1);
                    for (int ty = ty0 - 2 * mapHeightTiles; ty <= vTyHi; ty += mapHeightTiles) {
                        if (ty < vTyLo) continue;
                        for (int k = 0; k < 16; ++k) {
                            const int tx0 = (mapCol0 + k - startTileX) & (mapWidthTiles - 1);
                            for (int tx = tx0 - 2 * mapWidthTiles; tx <= vTxHi; tx += mapWidthTiles) {
                                if (tx < vTxLo) continue;
                                if (wideSrc.on && !TileInVramWindow(tx, ty)) continue; /* not from VRAM */
                                LM_PROCESS(tx, ty);
                            }
                        }
                    }
                }
            }
            /* 3. Tile pixels or palette banks that changed this frame: re-check
             *    the visible cells that show them (by the entry they hold). */
            const u64 tWalk = svcGetSystemTick();
            bool anyBank = false;
            for (int b = 0; b < 17; ++b) anyBank |= (sLmBankStamp[bgIndex][b] == now);
            bool anyTile = false;
            uint32_t changedTile[1024 / 32];
            if (sChunksChangedNow > 0) {
                memset(changedTile, 0, sizeof(changedTile));
                const uint32_t bytesPer = bpp8 ? 64u : 32u;
                for (uint32_t t = 0; t < 1024; ++t) {
                    const uint32_t c = (charBase + t * bytesPer) / VRAM_CHUNK_BYTES;
                    if (c >= VRAM_CHUNKS) break;
                    if (sChunkStamp[c] == now || (bpp8 && c + 1 < VRAM_CHUNKS && sChunkStamp[c + 1] == now)) {
                        changedTile[t >> 5] |= 1u << (t & 31);
                        anyTile = true;
                    }
                }
            }
            if (anyBank || anyTile) {
                for (int ty = vTyLo; ty <= vTyHi; ++ty) {
                    const int cellRow = (startTileY + ty) & (LM_ROWS - 1);
                    for (int tx = vTxLo; tx <= vTxHi; ++tx) {
                        const int cellCol = (startTileX + tx) & (LM_COLS - 1);
                        const LayerMapCell* cell = &sLmCells[bgIndex][cellRow][cellCol];
                        const unsigned t = cell->entry & 0x3FFu;
                        const int bank = bpp8 ? 16 : (cell->entry >> 12) & 0x0F;
                        /* Corrected cells too: their slot feeds the per-tile
                         * pass. Their correction cannot have changed without
                         * the scan key changing, so it is the cell's own. */
                        if ((anyTile && ((changedTile[t >> 5] >> (t & 31)) & 1u)) ||
                            sLmBankStamp[bgIndex][bank] == now)
                            LayerMapCheckCell(bgIndex, &pass, cellCol, cellRow, cell->entry, charBase, bpp8,
                                              (cell->flags & LM_CELL_CORRECTED) != 0);
                    }
                }
            }
            sPerfCount[PERF_COUNT_LM_WALK_US] += TicksToUs(svcGetSystemTick() - tWalk);
            /* 4. A rolling full re-check, one row a frame (the whole view in
             *    about half a second): catches what no VRAM chunk announces --
             *    the room's block map changing out in the WIDE margins (a
             *    block broken or regrown off the GBA frame). Only needed when
             *    positions come from that block map. */
            for (int r = 0; r < 1 && vTyHi >= vTyLo && wideSrc.on; ++r) {
                const int rows = vTyHi - vTyLo + 1;
                const int ty = vTyLo + (sc->rollRow++ % rows);
                for (int tx = vTxLo; tx <= vTxHi; ++tx) LM_PROCESS(tx, ty);
            }
        }
#undef LM_PROCESS
#undef LM_FADE_EXCLUDED
        sc->valid = true;
        sc->frame = now;
        sc->key = key;
        sc->startTileX = startTileX;
        sc->startTileY = startTileY;
        sc->txLo = vTxLo; sc->txHi = vTxHi; sc->tyLo = vTyLo; sc->tyHi = vTyHi;

        /* Everything is the map's except the corrected positions, which the
         * per-tile pass below draws as their own items. */
        for (int ty = tyMin; ty <= tyMax; ++ty) covered[ty + covY] = ~0ull;
        if (roomHasBlockCorrections && corrGridOk) {
            /* From the grid's marked blocks: each is 2x2 tiles. */
            for (int gy = 0; gy < corrH; ++gy) {
                for (int gx = 0; gx < corrW; ++gx) {
                    if (!corrGrid[gy][gx]) continue;
                    const int bty = (corrBY0 + gy) * 2 - fixOriginTileY;
                    const int btx = (corrBX0 + gx) * 2 - fixOriginTileX;
                    for (int ty = bty; ty <= bty + 1; ++ty) {
                        if (ty < vTyLo || ty > vTyHi) continue;
                        for (int tx = btx; tx <= btx + 1; ++tx)
                            if (tx >= vTxLo && tx <= vTxHi) covered[ty + covY] &= ~(1ull << (tx + covX));
                    }
                }
            }
        } else if (roomHasBlockCorrections) {
            for (int ty = vTyLo; ty <= vTyHi; ++ty)
                for (int tx = vTxLo; tx <= vTxHi; ++tx)
                    if (TILE_CORR_MASK(tx, ty) != 0u) covered[ty + covY] &= ~(1ull << (tx + covX));
        }
        if (fadeExcl) {
            /* The cells left out for a bank the palette fade does not cover
             * (every visible cell was checked under this fade: it is in the
             * scan key). Only during the fade. */
            for (int ty = vTyLo; ty <= vTyHi; ++ty) {
                const LayerMapCell* row = sLmCells[bgIndex][(startTileY + ty) & (LM_ROWS - 1)];
                for (int tx = vTxLo; tx <= vTxHi; ++tx)
                    if (row[(startTileX + tx) & (LM_COLS - 1)].flags & LM_CELL_CORRECTED)
                        covered[ty + covY] &= ~(1ull << (tx + covX));
            }
        }
        const int ops = pass.ops;
        sLmOpCount[bgIndex] = ops;
        sPerfCount[PERF_COUNT_LM_US] += TicksToUs(svcGetSystemTick() - tLm);
        sPerfCount[PERF_COUNT_TILE_POSITIONS] += (uint32_t)ops; /* here: cells redrawn */

        /* The layer on screen: one quad over the visible range plus a tile
         * each side horizontally (the stereo shift reveals that much), its
         * texture coordinates the layer's absolute position -- they run past
         * 1.0 and wrap. Same eighth-texel tie shift as the atlas. */
        const float x0 = (float)(-eL - 8), x1 = (float)(248 + eR);
        const float y0 = (float)(-eT), y1 = (float)(160 + eB);
        const float sh = ATLAS_UV_TIE_SHIFT;
        const float u0 = ((float)scrollX + x0 + sh) / (float)LM_W;
        const float v0 = ((float)scrollY + y0 + sh) / (float)LM_H;
        sLmSubtex[bgIndex] = (Tex3DS_SubTexture){
            .width = (u16)(x1 - x0), .height = (u16)(y1 - y0),
            .left = u0, .right = u0 + (x1 - x0) / (float)LM_W,
            .top = 1.0f - v0, .bottom = 1.0f - v0 - (y1 - y0) / (float)LM_H,
        };
        const int idx = AllocDrawItemSubtex(&sLmSubtex[bgIndex], (3 - priority) * 10 + (3 - bgIndex));
        if (idx >= 0) {
            DrawItem* item = &sDrawItems[idx];
            item->img.tex = &sLmTex[bgIndex];
            item->x = x0 + sCollectOffX;
            item->y = y0 + sCollectOffY;
            item->w = x1 - x0;
            item->h = y1 - y0;
            item->angle = 0.0f;
            item->depthTier = (int8_t)PortStereoDepth_BgTier(&sDepthState, bgIndex);
            item->blendAlpha = blendAlpha;
            item->affine = false;
            item->winVis = rectWinVis;
            item->isHud = false;
            item->plainEnv = true;
            item->gpuFx = (uint8_t)(layerFx | (sPalFadeOn ? ITEM_FX_PALFADE : 0));
        }
    }

    u64 tPass = svcGetSystemTick();
    for (int ty = tyMin; ty <= tyMax; ++ty) {
        int tileRow = (startTileY + ty) & (mapHeightTiles - 1);
        int screenBlockY = tileRow / 32;
        int localRow = tileRow % 32;
        for (int tx = txMin; tx <= txMax; ++tx) {
            if (covered[ty + covY] & (1ull << (tx + covX))) continue;
            ++sPerfCount[PERF_COUNT_TILE_POSITIONS];
            int tileCol = (startTileX + tx) & (mapWidthTiles - 1);
            int screenBlockX = tileCol / 32;
            int localCol = tileCol % 32;
            int screenBlockIndex = screenBlockX + screenBlockY * blocksPerRow;
            uint32_t mapAddr = screenBase + (uint32_t)screenBlockIndex * 0x800u + (uint32_t)(localRow * 32 + localCol) * 2u;
            uint16_t entry = (wideSrc.on && !TileInVramWindow(tx, ty))
                                 ? WideBgEntry(&wideSrc, tx, ty)
                                 : (uint16_t)(gVram[mapAddr] | (gVram[mapAddr + 1] << 8));
            uint16_t tileId = entry & 0x3FFu;
            bool hflip = (entry & 0x0400u) != 0;
            bool vflip = (entry & 0x0800u) != 0;
            int palBank = (entry >> 12) & 0x0Fu;
            float drawX = (float)(tx * 8 - fineX);
            float drawY = (float)(ty * 8 - fineY);
            if (drawY <= -8.0f - (float)eT || drawY >= 160.0f + (float)eB ||
                drawX <= -16.0f - (float)eL || drawX >= 248.0f + (float)eR) continue;

            uint32_t byteOffset = charBase + (uint32_t)tileId * bytesPerTile;
            if (!TileHasOpaquePixel(byteOffset, bpp8)) continue;
            if (sObjWindowActive && !ObjWinItemVisible(objWinVis, drawX, drawY)) continue;

            /* With the layer map on, the positions left here are cells it
             * keeps current (corrected ones included, see LayerMapCheckCell):
             * their slot is already known. The lookup is the fallback. */
            int slot = -1;
            if (useLayerMap) {
                const LayerMapCell* cell =
                    &sLmCells[bgIndex][(startTileY + ty) & (LM_ROWS - 1)][(startTileX + tx) & (LM_COLS - 1)];
                if (cell->entry == entry && cell->stamp != 0 && cell->slotGen == sAtlasGen && cell->slot >= 0) {
                    if (cell->slot == LM_TRANSPARENT_SLOT) continue;
                    slot = cell->slot;
                }
            }
            if (slot < 0) slot = GetOrDecodeTileSlot(byteOffset, bpp8, pal, palBank, hflip, vflip, false, brightAdjust);

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
            const unsigned corr = TILE_CORR_MASK(tx, ty);
            if (corr & CORR_LAYER_FIX) {
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
            /* Visible item tank (animated tile, no block-map entry, so not
             * reachable by the layer-fix list above): as a BG1 tile it would
             * inherit the BG play plane, which sits nearer the viewer than
             * the world-sprite plane and makes the tank read as floating in
             * front of Samus. Move ONLY its depth to Samus's OBJ plane; the
             * 2D draw order (sortKey) is left alone, so occlusion against the
             * scenery is unchanged. */
            if (corr & CORR_TANK) {
                depthTier = PortStereoDepth_ObjTier(&sDepthState, 1);
            }
            /* Door footprint (per-row spans from port_ppu_mzm.c: the door
             * columns, a couple of rows above/below, each grown to the end
             * of the BG2 lintel/sill run): the framed tunnel sits on the
             * play plane while the BG2 ledge stays a plane back, wedging
             * Samus into the gap. Pull those tiles' DEPTH onto the play
             * plane so the doorway reads as one object; 2D draw order is
             * left untouched, so a BG2 ledge tile still draws behind Samus
             * and behind BG1 exactly as before -- only its parallax stops
             * sinking away from the frame. BG3 is excluded (roomHasDoorDepth
             * is false for it). TileHasOpaquePixel above already skipped
             * blank tiles, so the open doorway itself is never touched. */
            if (corr & CORR_DOOR) {
                depthTier = PORT_TIER_BG_PLAY;
            }
            /* A bank the palette fade covers was decoded from the source
             * copy (sBgPalEff), so the fade is applied at draw time. */
            sPushFx = (uint8_t)(layerFx | ((sPalFadeOn && ((sPalFadeBanks >> palBank) & 1u)) ? ITEM_FX_PALFADE : 0));
            PushItem(slot, drawX, drawY, sortKey, depthTier, blendAlpha, rectWinVis, false);
        }
    }
    PlatformGpu3DS_PerfPhaseAdd(PERF_PHASE_BG_TILES, svcGetSystemTick() - tPass);
}

/* Issue #29: gather BG3's visible tiles into sHazeTiles[] for the offscreen
 * ripple pass instead of pushing them to sDrawItems. A stripped-down
 * CollectBgLayer(3): BG3 in every affected room is the backmost, opaque,
 * non-windowed, non-first-target layer, so priority / depth tier / window
 * tags / blend / brighten-darken / layer-fix redirection are all dropped.
 * The frame-level scroll (sHazeBakeHofs, from PortHaze_Bg3RowScroll) is
 * baked in here; the per-scanline delta is applied later in HazeBlitStrips. */
static void CollectHazeBg3(void) {
    sHazeTileCount = 0;

    const int bgIndex = 3;
    uint16_t bgcnt = (uint16_t)(gIoMem[0x08 + bgIndex * 2] | (gIoMem[0x09 + bgIndex * 2] << 8));
    uint32_t charBase = (uint32_t)((bgcnt >> 2) & 3u) * 0x4000u;
    bool bpp8 = ((bgcnt >> 7) & 1u) != 0;
    uint32_t screenBase = (uint32_t)((bgcnt >> 8) & 0x1Fu) * 0x800u;
    uint16_t sizeFlag = (uint16_t)((bgcnt >> 14) & 3u);
    int mapWidthTiles = (sizeFlag & 1u) ? 64 : 32;
    int mapHeightTiles = (sizeFlag & 2u) ? 64 : 32;
    int blocksPerRow = mapWidthTiles / 32;

    /* WIDE: bake past the GBA frame too, the same range CollectBgLayer(3)
     * would walk -- BG3 slides by its own share of the view's slide, so its
     * extent on each side is the margin minus / plus that share. The bake
     * texture's (0,0) is layer pixel (-eL, -eT), HAZE_MARGIN further left. */
    const int lsX = WideLayerShift(bgIndex, sWideView.shiftX, false);
    const int lsY = WideLayerShift(bgIndex, sWideView.shiftY, true);
    const int eL = sWideMarginX - lsX, eR = sWideMarginX + lsX;
    const int eT = sWideMarginY - lsY, eB = sWideMarginY + lsY;
    sHazeMarginX = sWideMarginX;
    sHazeMarginY = sWideMarginY;
    sHazeRows = 160 + eT + eB;
    if (sWideOn) {
        /* The game's table only has the 160 frame lines; compute every row
         * from the wave itself, so the surface lands on the right row even
         * out in the margins. Keeps the table's rows if that is not
         * available yet (first frame of the effect): only the margins then
         * sit still. */
        int16_t tableDelta[160];
        memcpy(tableDelta, sHazeRowDelta, sizeof(tableDelta));
        if (!PortHaze_Bg3WaveRows(sHazeRowDelta, sHazeRows, -eT, sWideView.shiftY - lsY,
                                  &sHazeBakeHofs)) {
            for (int r = 0; r < sHazeRows; ++r) {
                const int line = r - eT;
                sHazeRowDelta[r] = (line >= 0 && line < 160) ? tableDelta[line] : 0;
            }
        }
    }

    int scrollX = (int)(sHazeBakeHofs & 0x1FF);
    int scrollY = (int)((uint16_t)(gIoMem[0x1E] | (gIoMem[0x1F] << 8)) & 0x1FFu);

    const uint16_t *pal = sBgPalEff;
    const int bytesPerTile = bpp8 ? 64 : 32;
    int startTileX = scrollX / 8;
    int startTileY = scrollY / 8;
    int fineX = scrollX % 8;
    int fineY = scrollY % 8;

    /* Layer map: keep BG3's cells current and let the ripple read its strips
     * straight from there (HazeRippleFromMap) -- no per-tile bake, no frame
     * of lag. The strips reach HAZE_MARGIN past the view on each side. */
    sHazeFromMap = sLmReady[bgIndex] && sHazeRippleReady;
    if (sHazeFromMap) {
        LayerMapPass pass = LayerMapBegin(bgIndex, charBase, bpp8);
        const uint32_t now = pass.stampNow;
        const int xLo = -eL - HAZE_MARGIN, xHi = 240 + eR + HAZE_MARGIN; /* layer px, [lo, hi) */
        const int yLo = -eT, yHi = 160 + eB;
        const int txLo = (xLo + fineX) >> 3, txHi = (xHi - 1 + fineX) >> 3;
        const int tyLo = (yLo + fineY) >> 3, tyHi = (yHi - 1 + fineY) >> 3;
#define HZ_PROCESS(TX, TY)                                                                      \
        do {                                                                                    \
            const int tileRow_ = (startTileY + (TY)) & (mapHeightTiles - 1);                    \
            const int tileCol_ = (startTileX + (TX)) & (mapWidthTiles - 1);                     \
            const uint32_t mapAddr_ = screenBase +                                              \
                (uint32_t)(tileCol_ / 32 + (tileRow_ / 32) * blocksPerRow) * 0x800u +           \
                (uint32_t)((tileRow_ % 32) * 32 + tileCol_ % 32) * 2u;                          \
            LayerMapCheckCell(bgIndex, &pass, (startTileX + (TX)) & (LM_COLS - 1),              \
                              (startTileY + (TY)) & (LM_ROWS - 1),                              \
                              (uint16_t)(gVram[mapAddr_] | (gVram[mapAddr_ + 1] << 8)),         \
                              charBase, bpp8, false);                                           \
        } while (0)
        /* Same incremental look as CollectBgLayer's (see there): BG3 here is
         * always plain VRAM, no WIDE block map and no corrections. */
        uint32_t key = 2166136261u;
#define HZ_KEY(V) (key = (key ^ (uint32_t)(V)) * 16777619u)
        HZ_KEY(0x4A2Eu); HZ_KEY(screenBase); HZ_KEY(mapWidthTiles); HZ_KEY(mapHeightTiles); HZ_KEY(bpp8);
#undef HZ_KEY
        LayerMapScan* sc = &sLmScan[bgIndex];
        int dX = ((startTileX - sc->startTileX) + 32) & 63; dX -= 32;
        int dY = ((startTileY - sc->startTileY) + 32) & 63; dY -= 32;
        const bool full = !sc->valid || sc->key != key || sLmStateStamp[bgIndex] == now ||
                          sc->frame + 1 != now || dX > 16 || dX < -16 || dY > 16 || dY < -16;
        if (full) {
            for (int ty = tyLo; ty <= tyHi; ++ty)
                for (int tx = txLo; tx <= txHi; ++tx) HZ_PROCESS(tx, ty);
        } else {
            const int oTxLo = sc->txLo - dX, oTxHi = sc->txHi - dX;
            const int oTyLo = sc->tyLo - dY, oTyHi = sc->tyHi - dY;
            for (int ty = tyLo; ty <= tyHi; ++ty) {
                const bool rowWasIn = ty >= oTyLo && ty <= oTyHi;
                for (int tx = txLo; tx <= txHi; ++tx)
                    if (!rowWasIn || tx < oTxLo || tx > oTxHi) HZ_PROCESS(tx, ty);
            }
            if (sChunksChangedNow > 0) {
                const uint32_t mapBytes = (uint32_t)(mapWidthTiles / 32) * (uint32_t)(mapHeightTiles / 32) * 0x800u;
                const uint32_t c0 = screenBase / VRAM_CHUNK_BYTES;
                uint32_t c1 = (screenBase + mapBytes) / VRAM_CHUNK_BYTES;
                if (c1 > VRAM_CHUNKS) c1 = VRAM_CHUNKS;
                for (uint32_t c = c0; c < c1; ++c) {
                    if (sChunkStamp[c] != now) continue;
                    const uint32_t off = c * VRAM_CHUNK_BYTES - screenBase;
                    const int sb = (int)(off / 0x800u), within = (int)(off % 0x800u);
                    const int mapRow = (sb / blocksPerRow) * 32 + within / 64;
                    const int mapCol0 = (sb % blocksPerRow) * 32 + (within % 64) / 2;
                    const int ty0 = (mapRow - startTileY) & (mapHeightTiles - 1);
                    for (int ty = ty0 - 2 * mapHeightTiles; ty <= tyHi; ty += mapHeightTiles) {
                        if (ty < tyLo) continue;
                        for (int k = 0; k < 16; ++k) {
                            const int tx0 = (mapCol0 + k - startTileX) & (mapWidthTiles - 1);
                            for (int tx = tx0 - 2 * mapWidthTiles; tx <= txHi; tx += mapWidthTiles)
                                if (tx >= txLo) HZ_PROCESS(tx, ty);
                        }
                    }
                }
                uint32_t changedTile[1024 / 32];
                memset(changedTile, 0, sizeof(changedTile));
                bool anyTile = false;
                const uint32_t bytesPer = bpp8 ? 64u : 32u;
                for (uint32_t t = 0; t < 1024; ++t) {
                    const uint32_t c = (charBase + t * bytesPer) / VRAM_CHUNK_BYTES;
                    if (c >= VRAM_CHUNKS) break;
                    if (sChunkStamp[c] == now || (bpp8 && c + 1 < VRAM_CHUNKS && sChunkStamp[c + 1] == now)) {
                        changedTile[t >> 5] |= 1u << (t & 31);
                        anyTile = true;
                    }
                }
                bool anyBank = false;
                for (int b = 0; b < 17; ++b) anyBank |= (sLmBankStamp[bgIndex][b] == now);
                if (anyTile || anyBank) {
                    for (int ty = tyLo; ty <= tyHi; ++ty) {
                        const int cellRow = (startTileY + ty) & (LM_ROWS - 1);
                        for (int tx = txLo; tx <= txHi; ++tx) {
                            const int cellCol = (startTileX + tx) & (LM_COLS - 1);
                            const LayerMapCell* cell = &sLmCells[bgIndex][cellRow][cellCol];
                            const unsigned t = cell->entry & 0x3FFu;
                            const int bank = bpp8 ? 16 : (cell->entry >> 12) & 0x0F;
                            if (((changedTile[t >> 5] >> (t & 31)) & 1u) || sLmBankStamp[bgIndex][bank] == now)
                                LayerMapCheckCell(bgIndex, &pass, cellCol, cellRow, cell->entry, charBase, bpp8,
                                                  false);
                        }
                    }
                }
            } else {
                /* No BG chunk changed; a palette bank still can have. */
                bool anyBank = false;
                for (int b = 0; b < 17; ++b) anyBank |= (sLmBankStamp[bgIndex][b] == now);
                if (anyBank) {
                    for (int ty = tyLo; ty <= tyHi; ++ty)
                        for (int tx = txLo; tx <= txHi; ++tx) HZ_PROCESS(tx, ty);
                }
            }
        }
#undef HZ_PROCESS
        sc->valid = true;
        sc->frame = now;
        sc->key = key;
        sc->startTileX = startTileX;
        sc->startTileY = startTileY;
        sc->txLo = txLo; sc->txHi = txHi; sc->tyLo = tyLo; sc->tyHi = tyHi;
        sLmOpCount[bgIndex] = pass.ops;
        sHazeMapScrollX = scrollX;
        sHazeMapScrollY = scrollY;
        sHazeMapEL = eL;
        sHazeMapET = eT;
        return;
    }

    const int txMin = -1 - (eL + 7) / 8, txMax = 31 + (eR + 7) / 8;
    const int tyMin = -(eT + 7) / 8, tyMax = 20 + (eB + 7) / 8;
    for (int ty = tyMin; ty <= tyMax; ++ty) {
        int tileRow = (startTileY + ty) & (mapHeightTiles - 1);
        int screenBlockY = tileRow / 32;
        int localRow = tileRow % 32;
        for (int tx = txMin; tx <= txMax; ++tx) {
            int tileCol = (startTileX + tx) & (mapWidthTiles - 1);
            int screenBlockX = tileCol / 32;
            int localCol = tileCol % 32;
            int screenBlockIndex = screenBlockX + screenBlockY * blocksPerRow;
            uint32_t mapAddr = screenBase + (uint32_t)screenBlockIndex * 0x800u +
                               (uint32_t)(localRow * 32 + localCol) * 2u;
            uint16_t entry = (uint16_t)(gVram[mapAddr] | (gVram[mapAddr + 1] << 8));
            uint16_t tileId = entry & 0x3FFu;
            bool hflip = (entry & 0x0400u) != 0;
            bool vflip = (entry & 0x0800u) != 0;
            int palBank = (entry >> 12) & 0x0Fu;
            float drawX = (float)(tx * 8 - fineX);
            float drawY = (float)(ty * 8 - fineY);
            if (drawY <= (float)(-eT - 8) || drawY >= (float)(160 + eB) ||
                drawX <= (float)(-eL - 16) || drawX >= (float)(248 + eR)) continue;

            uint32_t byteOffset = charBase + (uint32_t)tileId * bytesPerTile;
            if (!TileHasOpaquePixel(byteOffset, bpp8)) continue;

            int slot = GetOrDecodeTileSlot(byteOffset, bpp8, pal, palBank, hflip, vflip, false,
                                           BRIGHT_ADJUST_NONE);
            if (slot < 0 || sHazeTileCount >= HAZE_MAX_TILES) continue;
            sHazeTiles[sHazeTileCount].img.tex = &sAtlasTexture;
            sHazeTiles[sHazeTileCount].img.subtex = &sSlotSubtexTable[slot];
            sHazeTiles[sHazeTileCount].x = drawX + (float)eL;
            sHazeTiles[sHazeTileCount].y = drawY + (float)eT;
            ++sHazeTileCount;
        }
    }
}

/* Issue #29: draw the baked BG3 offscreen texture to `target` as 160
 * one-scanline horizontal strips, strip y sampled at U offset rowDelta[y]
 * (already relative to the scroll baked into the texture) so each line
 * scrolls independently -- the wobble the HBlank DMA produces on hardware.
 * Called first in the per-eye pass
 * (BG3 is backmost), opaque. */
/* Mode 3: the same 160 strips, but into a 256x256 target at 1:1 instead of
 * onto the screen at display scale, once per frame instead of once per eye.
 * The eyes then each draw HazeBlitRippled's single quad. */
static void HazeRippleIntoTarget(int buf) {
    const int16_t* rowDelta = sHazeBakedRowDelta[buf];
    const int rows = sHazeBakedRows[buf];
    const float width = 240.0f + 2.0f * (float)sHazeBakedMarginX[buf];

    C2D_SceneBegin(sHazeRippleRT);
    C3D_RenderTargetClear(sHazeRippleRT, C3D_CLEAR_COLOR, 0, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_AlphaTest(true, GPU_GREATER, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    ConfigurePlainTextureEnv();

    C2D_Image img = { &sHazeTex[buf], &sHazeStripSubtex };
    bool reasserted = false;
    for (int y = 0; y < rows; ++y) {
        int delta = (int)rowDelta[y];
        if (delta < -HAZE_MARGIN) delta = -HAZE_MARGIN;
        else if (delta > HAZE_MARGIN) delta = HAZE_MARGIN;

        sHazeStripSubtex.width = (u16)width;
        sHazeStripSubtex.height = 1;
        sHazeStripSubtex.left = (float)(HAZE_MARGIN + delta) / (float)HAZE_RT_W;
        sHazeStripSubtex.right = ((float)(HAZE_MARGIN + delta) + width) / (float)HAZE_RT_W;
        sHazeStripSubtex.top = 1.0f - (float)y / (float)HAZE_RT_H;
        sHazeStripSubtex.bottom = 1.0f - (float)(y + 1) / (float)HAZE_RT_H;
        C2D_DrawParams p = { { 0.0f, (float)y, width, 1.0f }, { 0.0f, 0.0f }, 0.0f, 0.0f };
        C2D_DrawImage(img, &p, NULL);
        if (!reasserted) { ConfigurePlainTextureEnv(); reasserted = true; }
    }
    C2D_Flush();
}

/* HazeRippleIntoTarget, reading BG3 from its layer map (see sHazeFromMap):
 * the strips sample the wrapping map at the layer's own position, this
 * frame's scroll and wave. Leaves the geometry in buffer `buf`'s slot, which
 * is what HazeBlitRippled reads. */
static void HazeRippleFromMap(int buf) {
    memcpy(sHazeBakedRowDelta[buf], sHazeRowDelta, sizeof(sHazeBakedRowDelta[buf]));
    sHazeBakedRows[buf] = sHazeRows;
    sHazeBakedMarginX[buf] = sHazeMarginX;
    sHazeBakedMarginY[buf] = sHazeMarginY;
    sHazeBufReady[buf] = true;

    const int rows = sHazeRows;
    const float width = 240.0f + 2.0f * (float)sHazeMarginX;
    C2D_SceneBegin(sHazeRippleRT);
    C3D_RenderTargetClear(sHazeRippleRT, C3D_CLEAR_COLOR, 0, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_AlphaTest(true, GPU_GREATER, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    ConfigurePlainTextureEnv();

    C2D_Image img = { &sLmTex[3], &sHazeStripSubtex };
    bool reasserted = false;
    for (int y = 0; y < rows; ++y) {
        int delta = (int)sHazeRowDelta[y];
        if (delta < -HAZE_MARGIN) delta = -HAZE_MARGIN;
        else if (delta > HAZE_MARGIN) delta = HAZE_MARGIN;
        const float u = (float)(sHazeMapScrollX - sHazeMapEL + delta) / (float)LM_W;
        const float v = (float)(sHazeMapScrollY - sHazeMapET + y) / (float)LM_H;
        sHazeStripSubtex.width = (u16)width;
        sHazeStripSubtex.height = 1;
        sHazeStripSubtex.left = u;
        sHazeStripSubtex.right = u + width / (float)LM_W;
        sHazeStripSubtex.top = 1.0f - v;
        sHazeStripSubtex.bottom = 1.0f - v - 1.0f / (float)LM_H;
        C2D_DrawParams p = { { 0.0f, (float)y, width, 1.0f }, { 0.0f, 0.0f }, 0.0f, 0.0f };
        C2D_DrawImage(img, &p, NULL);
        if (!reasserted) { ConfigurePlainTextureEnv(); reasserted = true; }
    }
    C2D_Flush();
}

/* Texenv for putting the rippled BG3 on screen: plain, or with the frame's
 * brighten/darken when BG3 is its first target. The BG3 bake and the layer
 * map both hold BG3 unfaded, so this is the only place a fade reaches it
 * (before, a haze room's BG3 stayed at full brightness through one). */
static void ConfigureHazeBlitTextureEnv(void) {
    if ((sBldEffect == 2 || sBldEffect == 3) && BldIsFirstTarget(sIoBldcnt, 3)) ConfigureFxTextureEnv(ITEM_FX_BRIGHT);
    else ConfigurePlainTextureEnv();
}

/* Mode 3's per-eye half: one quad. Same shifted-full-span UV convention as
 * everything else that samples a target (see ATLAS_UV_TIE_SHIFT). baseX/Y is
 * the GBA frame's top-left; a WIDE bake reaches its margin past it. */
static void HazeBlitRippled(int buf, float baseX, float baseY, float scaleX, float scaleY) {
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    ConfigureHazeBlitTextureEnv();
    const float width = 240.0f + 2.0f * (float)sHazeBakedMarginX[buf];
    const float height = (float)sHazeBakedRows[buf];
    const float sh = ATLAS_UV_TIE_SHIFT;
    sHazeRippleSubtex = (Tex3DS_SubTexture){
        .width = (u16)width, .height = (u16)height,
        .left = sh / (float)HAZE_RT_W,
        .top = 1.0f - sh / (float)HAZE_RT_H,
        .right = (width + sh) / (float)HAZE_RT_W,
        .bottom = 1.0f - (height + sh) / (float)HAZE_RT_H,
    };
    C2D_Image img = { &sHazeRippleTex, &sHazeRippleSubtex };
    C2D_DrawParams p = { { baseX - (float)sHazeBakedMarginX[buf] * scaleX,
                           baseY - (float)sHazeBakedMarginY[buf] * scaleY,
                           width * scaleX, height * scaleY },
                         { 0.0f, 0.0f }, 0.5f, 0.0f };
    C2D_DrawImage(img, &p, NULL);
    ConfigureHazeBlitTextureEnv(); /* citro2d re-inits on a scene's first draw */
    C2D_Flush();
    ConfigureAtlasTextureEnv(); /* restore for the BG0-2 / OBJ pass that follows */
}

static void HazeBlitStrips(int buf, float baseX, float baseY, float scaleX, float scaleY) {
    const int16_t* rowDelta = sHazeBakedRowDelta[buf];
    const int rows = sHazeBakedRows[buf];
    const float width = 240.0f + 2.0f * (float)sHazeBakedMarginX[buf];
    baseX -= (float)sHazeBakedMarginX[buf] * scaleX;
    baseY -= (float)sHazeBakedMarginY[buf] * scaleY;

    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    ConfigureHazeBlitTextureEnv();

    C2D_Image img = { &sHazeTex[buf], &sHazeStripSubtex };
    bool reasserted = false;
    /* One quad per scanline. (Coalescing runs of equal shift was tried and
     * read as blocky "squares" instead of a wave -- a tall quad over a
     * multi-row V span sampled wrong with NEAREST; per-line is what looked
     * right in testing.) */
    for (int y = 0; y < rows; ++y) {
        int delta = (int)rowDelta[y];
        if (delta < -HAZE_MARGIN) delta = -HAZE_MARGIN;
        else if (delta > HAZE_MARGIN) delta = HAZE_MARGIN;

        float uL = (float)(HAZE_MARGIN + delta) / (float)HAZE_RT_W;
        float uR = ((float)(HAZE_MARGIN + delta) + width) / (float)HAZE_RT_W;
        float vT = (float)y / (float)HAZE_RT_H;
        float vB = (float)(y + 1) / (float)HAZE_RT_H;
        /* Non-rotated subtex; PICA texture origin is bottom-left, so the top
         * edge is the larger V. */
        sHazeStripSubtex.width = (u16)width;
        sHazeStripSubtex.height = 1;
        sHazeStripSubtex.left = uL;
        sHazeStripSubtex.right = uR;
        sHazeStripSubtex.top = 1.0f - vT;
        sHazeStripSubtex.bottom = 1.0f - vB;
        C2D_DrawParams p = {
            { baseX, baseY + (float)y * scaleY, width * scaleX, scaleY + 0.5f },
            { 0.0f, 0.0f }, 0.5f, 0.0f
        };
        C2D_DrawImage(img, &p, NULL);
        /* citro2d re-inits the texenv on the first C2D_DrawImage of a scene
         * (see the sDrawOrder loop's reassertedTexEnv workaround) -- undo
         * that stomp so the rest of the strips keep the plain env. */
        if (!reasserted) { ConfigureHazeBlitTextureEnv(); reasserted = true; }
    }
    C2D_Flush();
    ConfigureAtlasTextureEnv(); /* restore for the BG0-2 / OBJ pass that follows */
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
    /* ResetFreeOam (src/init_helpers.c) parks every unused slot as an 8x8
     * sprite at x=255, y=255: off the GBA's right edge, but inside the WIDE
     * view -- where it drew OBJ tile 0, i.e. a piece of Samus, at the top of
     * the extra area on the right. */
    if (attr0 == 0x00FFu && attr1 == 0x00FFu && attr2 == 0u) return;
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

    /* OAM Y is 8 bits and X 9 (signed, wrapping): a value past the visible
     * range is a position above / left of the frame instead. The WIDE view
     * moves that cut-over out with the frame's edges. That is only a guess
     * for a sprite far out, so the sprites the game tags with their true
     * position (Port_Wide_NoteSlots) skip it. */
    int y = attr0 & 0xFFu;
    int x = (int)(attr1 & 0x1FFu);
    int originY, originX;
    if (sWideOn && PortWide_SlotOrigin(oamIndex, &originY, &originX)) {
        /* The game recorded where this sprite really is, so pick the wrap of
         * y / x that lands nearest it. Exact for any part within 128 px
         * vertically / 256 horizontally of the origin, i.e. all of them. */
        y = originY + ((y - originY + 128) & 255) - 128;
        x = originX + ((x - originX + 256) & 511) - 256;
    } else {
        if (y >= 160 + sExtB) y -= 256;
        if (x >= 240 + sExtR) x -= 512;
    }

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

    if (y >= 160 + sExtB || y + boundsHeight <= -sExtT || x >= 240 + sExtR || x + boundsWidth <= -sExtL) return;

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

    /* BLDCNT layer id 4 = OBJ. A Semi-Transparent OBJ (attr0 mode 1) is
     * ALWAYS a blend 1st target and ALWAYS uses alpha blending, whatever
     * BLDCNT bit4 / bits 6-7 say (GBATEK). The Mother Brain eye is 16 such
     * sprites over Samus on the elevator (BG1) and the eye glow (BG0);
     * without this they draw opaque and hide both behind a solid blob. */
    bool objSemiTransparent = (objMode == 1);
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    if (objSemiTransparent) { ++sDiagSemiTransColl; sDiagSemiTransX = x; sDiagSemiTransOam = oamIndex; }
    if (isAffine) ++sDiagAffineColl;
    if ((attr0 & 0x1000u)) ++sDiagMosaicColl;  /* attr0 bit12 = OBJ mosaic */
#endif
    BrightAdjust brightAdjust = BRIGHT_ADJUST_NONE;
    bool blendAlpha;
    if (objSemiTransparent) {
        blendAlpha = true;
    } else {
        bool isFirstTarget = sBldEffect != 0 && BldIsFirstTarget(sIoBldcnt, 4);
        if (isFirstTarget && sBldEffect == 2) brightAdjust = BRIGHT_ADJUST_BRIGHTEN;
        else if (isFirstTarget && sBldEffect == 3) brightAdjust = BRIGHT_ADJUST_DARKEN;
        blendAlpha = isFirstTarget && sBldEffect == 1;
    }

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

    /* The sprite's depth plane and HUD status: the same for every subtile,
     * so worked out once (it asks four other modules). */
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
    /* In-game message / area-name banners (and the save cursor) are
     * ordinary sprites, so isRealHud never catches them, yet they
     * are an overlay and draw at OAM priority 0 in 2D. SpriteDraw
     * tags their OAM slots (port_overlay_text_oam.c); lift them to
     * the same front tier as the HUD so stereo does not sink the
     * flat text into Samus. */
    extern int Port_OverlayText_IsSlot(int oamIndex);
    bool isOverlayText = gMainGameMode == 4 && Port_OverlayText_IsSlot(oamIndex);
    /* The escape countdown digits (PE_ESCAPE particle, tagged in
     * src/particle.c). Route them exactly like real HUD: HUD depth
     * tier, and off-screen with the HUD when that option is on. */
    extern int Port_OverlayText_IsEscapeSlot(int oamIndex);
    bool isEscapeHud = gMainGameMode == 4 && Port_OverlayText_IsEscapeSlot(oamIndex);
    if (isEscapeHud) isRealHud = true;
    /* Per-sprite depth override (port_sprite_depth_oam.c): a few
     * sprite TYPES are authored to composite with a specific BG --
     * the Kraid/Ridley statues set their OAM priority to BG1's so
     * the sprite face blends with the BG that carries the top of
     * the head. SpriteDraw tags their slots; honour that here
     * instead of the one forced world-sprite plane. */
    int spriteDepthCode = (gMainGameMode == 4) ? Port_SpriteDepth_SlotCode(oamIndex)
                                               : PORT_SPRITE_DEPTH_NONE;
    int depthTier;
    if (inMapOrPauseScreen) {
        depthTier = (priority == 0) ? 5 : 6;
    } else if (isRealHud || isOverlayText) {
        depthTier = 5;
    } else if (spriteDepthCode == PORT_SPRITE_DEPTH_BG_COPLANAR) {
        depthTier = PortStereoDepth_BgTierForPriority(&sDepthState, priority);
    } else if (spriteDepthCode >= 0) {
        depthTier = spriteDepthCode;
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
            if (!isAffine) {
                float drawX = (float)(x + tx * 8);
                float drawY = (float)(y + ty * 8);
                if (drawY <= -8.0f - (float)sExtT || drawY >= 160.0f + (float)sExtB ||
                    drawX <= -8.0f - (float)sExtL || drawX >= 240.0f + (float)sExtR) continue;
                if (sObjWindowActive && !ObjWinItemVisible(objWinVis, drawX, drawY)) continue;
                int slot = GetOrDecodeTileSlot(byteOffset, bpp8, pal, palBank, hflip, vflip, true, brightAdjust);
                const int before = sDrawItemCount;
                PushItem(slot, drawX, drawY, sortKey, depthTier, blendAlpha, rectWinVis, isRealHud);
                if (sDrawItemCount > before && (isRealHud || isOverlayText)) sDrawItems[before].screenFixed = true;
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
            /* Overlap-bleed only toward edges that have a same-sprite
             * neighbour, in texture-grid orientation (tx grows along the
             * quad's local +x, ty along local +y, regardless of the sprite's
             * rotation or flip). A 1x1 sprite gets none and draws exactly as
             * before. */
            uint8_t bleedEdges = 0;
            if (tx > 0)            bleedEdges |= 0x1; /* left  */
            if (tx < tilesW - 1)  bleedEdges |= 0x2; /* right */
            if (ty > 0)            bleedEdges |= 0x4; /* top   */
            if (ty < tilesH - 1)  bleedEdges |= 0x8; /* bottom */
            PushAffineItem(slot, screenCenterX, screenCenterY, affAngle, affScaleX, affScaleY, sortKey, depthTier, blendAlpha,
                           rectWinVis, bleedEdges);
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
#ifdef PORT_DEBUG_TOOLS_ACTIVE
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
    if ((dispcnt & 7u) != 0u) {
        /* Mode 1 with a pure-scale 256x256 BG2 (the Tourian-escape "Samus
         * surrounded" sub-scene) is handled -- see CollectAffineBg2. Every
         * other non-zero mode still falls back to the CPU renderer. */
        if (!DetectAffineBg2()) REJECT("mode != 0");
    }

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

    /* Issue #17 (Samus's death animation rendering wrong through this
     * renderer on real hardware) used to force a CPU-scanline fallback here
     * for the death scene's DISPCNT signature. RESOLVED at the source and
     * the workaround removed -- the scene renders correctly on the GPU path
     * now, confirmed on hardware. Two independent bugs, both of which the
     * death scene is simply the worst case for; see
     * docs/3ds-issue17-session-2026-09-02.md for the recording that
     * separated them:
     *   1. The eye targets were cleared to hardcoded black instead of the
     *      GBA backdrop (BG palette entry 0). Invisible while BG tiles
     *      cover the screen; this scene enables no BG layer at all and
     *      fades the backdrop to white through palette RAM, so the whole
     *      fade was dropped. See BackdropClearColor's use in
     *      Port_GpuRenderer_RenderFrame.
     *   2. The PICA200's texture cache was never invalidated after an
     *      in-place atlas redecode, so slots whose palette changed kept
     *      drawing stale texels. Hidden in ordinary frames, which thrash
     *      that cache; exposed by this scene's tiny working set plus a
     *      palette rewritten every frame. See the C3D_TexBind call after
     *      the dirty-row flush in Port_GpuRenderer_RenderFrame. */

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

/* Issue #20 draw-call census. The frame-time numbers alone (citro3d's
 * drawing/processing times, recorded per sample) say a frame went over
 * budget but not why: a scene can cost the same in submitted quads and
 * still take three times as long because every alpha-blended item breaks
 * the batch and re-reads the destination framebuffer. These make that
 * visible in a recording -- see Port_GpuRenderer_GetLastFrameDrawStats.
 * Summed over both eyes, reset once per RenderFrame. */
static uint32_t sLastDrawCalls;
static uint32_t sLastBlendTransitions;
static uint8_t sLastEyesRendered;

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
extern bool Port_Config_GetHudOutside(void);
extern bool Port_Config_GetGbaBezel(void);
extern double Port_PPU_3DS_CurrentFps(void);

static inline C2D_DrawParams BuildDrawParams(const DrawItem* item, float screenBaseX, float screenBaseY, float eyeOffset, float scaleX, float scaleY, bool hudOutside) {
    C2D_DrawParams params;
    params.depth = (item->isHud && hudOutside) ? 0.7f : 0.5f;
    if (item->affine) {
        /* An affine sprite is drawn as one quad per 8x8 subtile, each centred
         * on its own matrix-transformed centre. Adjacent subtiles' centres
         * are exactly one subtile-span apart, but the origin snap below
         * rounds each one independently, so two neighbours can round apart by
         * up to a whole device pixel -- leaving a gap. With GPU_NEAREST and
         * an atlas slot that carries no apron, that gap shows straight
         * through as a transparent seam, and a scaled-up affine sprite reads
         * as a grid of detached squares (the "costuras" report).
         *
         * Fix: grow each subtile quad by kAffineBleed device px, but ONLY on
         * the edges that face another subtile of the same sprite
         * (item->affBleedEdges, set in CollectSprite). Those interior edges
         * then overlap their neighbour by ~2*kAffineBleed, covering the
         * <=1px rounding gap; the sprite's OUTER silhouette edges are not
         * grown, so it keeps its exact size and outline instead of gaining a
         * 1px smear of its own edge colour. The UV is unchanged, so a grown
         * edge just re-stretches this subtile's own outermost texels over ~1
         * extra px (invisible with nearest sampling) -- it never reaches into
         * an adjacent atlas tile. A 1x1 affine sprite has no interior edges
         * and is untouched. (The seamless answer is one quad for the whole
         * sprite off a scratch target -- see
         * docs/3ds-gpu-affine-bg-and-obj-seams-feasibility-2026-09-09.md.) */
        const float kAffineBleed = 1.0f;
        /* PortAffine_SubtileQuad does the pixel snap + the interior-edge
         * grow; the "keep the two eyes in phase" reasoning for the snap is
         * in that header. Screen base + eye offset fold into the centre. */
        PortAffineQuad q = PortAffine_SubtileQuad(
            screenBaseX + eyeOffset + item->x * scaleX,
            screenBaseY + item->y * scaleY,
            item->w * scaleX, item->h * scaleY,
            item->affBleedEdges, kAffineBleed);
        params.pos.x = q.x;
        params.pos.y = q.y;
        params.pos.w = q.w;
        params.pos.h = q.h;
        params.center.x = q.cx;
        params.center.y = q.cy;
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
        float top;
        float right = screenBaseX + eyeOffset + (item->x + item->w) * scaleX;
        float bottom;
        if (item->isHud && hudOutside) {
            top = 2.0f + item->y;
            bottom = 2.0f + (item->y + item->h);
        } else {
            top = screenBaseY + item->y * scaleY;
            bottom = screenBaseY + (item->y + item->h) * scaleY;
        }
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

/* Recognise the power-bomb explosion darken+hole frame (issue #28, see
 * sPbFlashActive's comment) and derive the bright ellipse from the live
 * explosion state. Gate: the explosion AI is mid-blast
 * (gCurrentPowerBomb.animationState EXPLODING or IMPLODING) AND BLDCNT is in
 * brightness-decrease mode with the 4 BGs as first target -- the exact
 * BLDCNT src/haze.c sets for HAZE_VALUE_POWER_BOMB_EXPANDING/_RETRACTING.
 * Nothing else in the game combines those. Ellipse: centre = epicentre
 * minus BG1 scroll (same transform src/haze.c's Haze_PowerBombExpanding
 * uses), vertical radius = semiMinorAxis (screen px), horizontal radius =
 * 2x that (the game scales the window's X extent by 2). Read from
 * gCurrentPowerBomb, not gHazeValues, so it does not depend on the
 * HBlank/haze DMA plumbing this port stubs out. */
static bool DetectPowerBombFlash(void) {
    unsigned pbState = gCurrentPowerBomb[0];
    if (pbState != 3u && pbState != 4u) return false;   /* not EXPLODING / IMPLODING */

    uint16_t bldcnt = (uint16_t)(gIoMem[0x50] | (gIoMem[0x51] << 8));
    if (((bldcnt >> 6) & 3u) != 3u) return false;       /* not brightness-decrease */
    if ((bldcnt & 0x3Fu) != 0x0Fu) return false;        /* first target != {BG0..BG3} */

    unsigned semiMinor = gCurrentPowerBomb[2];
    if (semiMinor == 0u) return false;                  /* nothing to carve yet */

    int pbX = (int)(gCurrentPowerBomb[4] | (gCurrentPowerBomb[5] << 8));
    int pbY = (int)(gCurrentPowerBomb[6] | (gCurrentPowerBomb[7] << 8));
    sPbFlashCxGba = (float)(pbX - (int)gBg1XPosition) / 4.0f; /* SUB_PIXEL_TO_PIXEL */
    sPbFlashCyGba = (float)(pbY - (int)gBg1YPosition) / 4.0f;
    sPbFlashRyGba = (float)semiMinor;
    sPbFlashRxGba = (float)semiMinor * 2.0f;

    uint16_t bldy = (uint16_t)(gIoMem[0x54] | (gIoMem[0x55] << 8));
    int evy = (int)(bldy & 0x1Fu);
    if (evy > 16) evy = 16;
    sPbFlashEvy = evy;

#ifdef PORT_DEBUG_TOOLS_ACTIVE
    {
        static unsigned sPbLogCounter;
        if ((sPbLogCounter++ % 8u) == 0u) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "PBFLASH state=%u semiMinor=%u c=(%.0f,%.0f) r=(%.0f,%.0f) evy=%d",
                     pbState, semiMinor, (double)sPbFlashCxGba, (double)sPbFlashCyGba,
                     (double)sPbFlashRxGba, (double)sPbFlashRyGba, sPbFlashEvy);
            Port_DebugLog(buf);
        }
    }
#endif
    return true;
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

/* WIDE view geometry for this frame: where the camera is and how far the
 * widened view has to slide to stay inside the room.
 *
 * A view wider than the GBA frame would poke past the camera's limits whenever
 * it is near one and show scenery that was never meant to be seen (or black
 * "outside the map"); instead the view is slid back inside (shiftX/Y, GBA px;
 * the world moves on screen by the negative of it). The camera itself is
 * untouched, so at a limit the scenery simply stops scrolling and Samus walks
 * on toward the border, and the side that is not blocked shows more. An extent
 * smaller than the view is centred, which is what leaves the black borders on
 * both sides of a single-screen room. */

static int WideAxisShift(int origin, int ext, int frame, int lo, int hi) {
    const int viewLo = origin - ext, viewHi = origin + frame + ext;
    if (hi - lo <= viewHi - viewLo) return (lo + hi - viewLo - viewHi) / 2; /* room fits: centre it */
    if (viewLo < lo) return lo - viewLo;
    if (viewHi > hi) return hi - viewHi;
    return 0;
}

/* Leaving-a-room state of ComputeWideView (see there). File scope so the
 * scene recorder can report it (Port_GpuRenderer_GetWideRecord). */
static WideView sHeld;
static bool sHeldValid, sLeaving, sTunnelStarted;
static int sTunnelX0, sTunnelY0;

static void ComputeWideView(void) {
    extern void PortPpuMzm_ScreenOrigin(int* outX, int* outY);
    WideView* v = &sWideView;
    /* Leaving a room. From the moment a room is left (SUB_GAME_MODE_LOADING_ROOM,
     * include/constants/game_state.h) the game loads the next room and
     * moves the camera while the screen still shows the old room, so a view
     * worked out from them would slide on its own: the view the last playing
     * frame had is kept while the old room is up. A door transition then
     * slides its tunnel (BG3) from the old room's door to the new room's, and
     * the view follows it there -- from the old room's view to the new one,
     * in step with the tunnel's own travel -- so the door arrives where the
     * new room's view has it instead of jumping by the difference between
     * the two views when the room changes. Transitions without a tunnel
     * (elevators, fades) take the new view as soon as the new room is in. */
    extern s16 gSubGameMode1;
    enum { SUB_GAME_MODE_LOADING_ROOM = 3 };
    int tunX, tunY, tunTX, tunTY, tunPause;
    const int tunnel = PortPpuMzm_DoorTunnel(&tunX, &tunY, &tunTX, &tunTY, &tunPause);
    if (sHeldValid && (gSubGameMode1 == SUB_GAME_MODE_LOADING_ROOM || (sLeaving && gSubGameMode1 == 0))) {
        sLeaving = true;
        sTunnelStarted = false;
        *v = sHeld;
        return;
    }
    *v = (WideView){ 0 };
    /* The whole room: everything it has data for is shown, even the parts
     * the GBA camera never reaches (a neighbouring scroll region, the padding
     * blocks). Only what lies outside the room is black. Tried the camera's
     * scroll regions instead: the view then jumped every time Samus crossed
     * into another region and hid scenery that is really there. */
    PortPpuMzm_ScreenOrigin(&v->originX, &v->originY);
    const int roomW = gBgPointersAndDimensions.clipdataWidth;
    const int roomH = gBgPointersAndDimensions.clipdataHeight;
    if (roomW <= 0 || roomH <= 0) {
        /* No room loaded: leave the view centred on the frame. */
        v->loX = v->originX; v->hiX = v->originX + 240;
        v->loY = v->originY; v->hiY = v->originY + 160;
        v->maskL = 0; v->maskR = 240; v->maskT = 0; v->maskB = 160;
        return;
    }
    v->loX = 0; v->hiX = roomW * 16;
    v->loY = 0; v->hiY = roomH * 16;
    v->shiftX = WideAxisShift(v->originX, sWideMarginX, 240, v->loX, v->hiX);
    v->shiftY = WideAxisShift(v->originY, sWideMarginY, 160, v->loY, v->hiY);
    /* The view never slides further than its own margin, so what is collected
     * never has to reach past twice the margin on one side. */
    if (v->shiftX < -sWideMarginX) v->shiftX = -sWideMarginX;
    if (v->shiftX > sWideMarginX) v->shiftX = sWideMarginX;
    if (v->shiftY < -sWideMarginY) v->shiftY = -sWideMarginY;
    if (v->shiftY > sWideMarginY) v->shiftY = sWideMarginY;
    v->maskL = v->loX - v->originX; v->maskR = v->hiX - v->originX;
    v->maskT = v->loY - v->originY; v->maskB = v->hiY - v->originY;

    if (sLeaving && tunnel == PORT_DOOR_TUNNEL_SLIDING) {
        if (!sTunnelStarted) {
            sTunnelStarted = true;
            sTunnelX0 = tunX;
            sTunnelY0 = tunY;
        }
        /* How far along each axis the tunnel is. The game slides it
         * vertically, pauses, then slides it horizontally, and the view does
         * the same: an axis the tunnel does not travel on changes during
         * the pause (vertical) or with the other axis (horizontal), so it
         * never turns into a diagonal. */
        const int dx = tunTX - sTunnelX0, dy = tunTY - sTunnelY0;
        float px = dx ? (float)(tunX - sTunnelX0) / (float)dx : -1.0f;
        float py = dy ? (float)(tunY - sTunnelY0) / (float)dy
                      : (float)tunPause / (float)PORT_DOOR_TUNNEL_PAUSE_FRAMES;
        if (px < 0.0f) px = (tunPause >= PORT_DOOR_TUNNEL_PAUSE_FRAMES) ? 1.0f : 0.0f;
        if (px > 1.0f) px = 1.0f;
        if (py > 1.0f) py = 1.0f;
        const WideView* h = &sHeld;
#define WIDE_LERP(A, B, T) ((A) + (int)((float)((B) - (A)) * (T) + ((B) >= (A) ? 0.5f : -0.5f)))
        v->shiftX = WIDE_LERP(h->shiftX, v->shiftX, px);
        v->shiftY = WIDE_LERP(h->shiftY, v->shiftY, py);
        v->maskL = WIDE_LERP(h->maskL, v->maskL, px);
        v->maskR = WIDE_LERP(h->maskR, v->maskR, px);
        v->maskT = WIDE_LERP(h->maskT, v->maskT, py);
        v->maskB = WIDE_LERP(h->maskB, v->maskB, py);
#undef WIDE_LERP
        return; /* sHeld stays the old room's until the slide is over */
    }
    sLeaving = false;
    sTunnelStarted = false;
    sHeld = *v;
    sHeldValid = true;
}

/* Blacks out whatever lies beyond the room extent (see WideView), which only
 * shows for a room smaller than the view. baseX/Y is where the shifted world
 * starts on screen. Clamped to stay clear of the 240x160 frame itself, so a
 * camera that is momentarily outside the extent (a transition) can never
 * black out the game or the HUD. */
static void DrawWideRoomMasks(float baseX, float baseY, float scaleX, float scaleY) {
    const WideView* v = &sWideView;
    /* The tile quads before this are still queued with the atlas texenv. A
     * solid rectangle drawn on top of that samples the atlas instead of being
     * flat black -- the repeating stray-tile pattern seen in the border of
     * small rooms. Flush them, then put the texenv back to plain colour (see
     * PlatformGpu3DS_ResetSolidTexEnv). */
    C2D_Flush();
    PlatformGpu3DS_ResetSolidTexEnv();
    int left = v->maskL - v->shiftX;
    int top = v->maskT - v->shiftY;
    int right = v->maskR - v->shiftX;
    int bottom = v->maskB - v->shiftY;
    if (left > -v->shiftX) left = -v->shiftX;
    if (top > -v->shiftY) top = -v->shiftY;
    if (right < 240 - v->shiftX) right = 240 - v->shiftX;
    if (bottom < 160 - v->shiftY) bottom = 160 - v->shiftY;

    const u32 black = C2D_Color32(0, 0, 0, 255);
    const float x0 = baseX + (float)left * scaleX;
    const float x1 = baseX + (float)right * scaleX;
    const float y0 = baseY + (float)top * scaleY;
    const float y1 = baseY + (float)bottom * scaleY;
    if (x0 > 0.0f) C2D_DrawRectSolid(0.0f, 0.0f, 0.6f, x0, 240.0f, black);
    if (x1 < 400.0f) C2D_DrawRectSolid(x1, 0.0f, 0.6f, 400.0f - x1, 240.0f, black);
    if (y0 > 0.0f) C2D_DrawRectSolid(0.0f, 0.0f, 0.6f, 400.0f, y0, black);
    if (y1 < 240.0f) C2D_DrawRectSolid(0.0f, y1, 0.6f, 400.0f, 240.0f - y1, black);
}

/* The frame is split in two so the CPU half can run while the GPU is still
 * drawing the previous frame (see Port_GpuRenderer_CollectFrame). */
static bool sFrameCollected;
static bool sAtlasRebindPending;

bool Port_GpuRenderer_CollectNeedsIdleGpu(void) {
    /* Each of these reassigns atlas slots the previous frame may still be
     * sampling (the reset at the top of Port_GpuRenderer_CollectFrame, and
     * the settle window after a room change, which empties the tile cache
     * every frame). An in-place redecode is not on this list: it only
     * changes what a slot shows to what it is about to show anyway. */
    return sCacheCount >= ATLAS_MAX_SLOTS / 2 || sSettleResetPending;
}

/* CPU half: walk VRAM/OAM, decode into the atlas, build the sorted item list.
 * Touches no GPU state, so it can run before C3D_FrameBegin -- which blocks
 * until the GPU has finished the previous frame. Running it there overlaps
 * this frame's collection with that frame's drawing instead of queueing one
 * behind the other. The atlas is the one thing both touch; see
 * Port_GpuRenderer_CollectNeedsIdleGpu for when that is not safe. */
void Port_GpuRenderer_CollectFrame(void) {
    if (!sInitialized) return;
    sSettleResetPending = false; /* set again below while the window lasts */
    u64 tStart = svcGetSystemTick();
    VramDiffBeginFrame();
    memset(sSlotFreshThisFrame, 0, sizeof(sSlotFreshThisFrame));
    u64 tPhase = svcGetSystemTick();
    PlatformGpu3DS_PerfPhaseAdd(PERF_PHASE_VRAM_DIFF, tPhase - tStart);

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
#ifdef PORT_DEBUG_TOOLS_ACTIVE
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "OBJTILE CACHE RESET at count=%d", sCacheCount);
            Port_DebugLog(buf);
        }
#endif
        for (int i = 0; i < HASH_BUCKETS; ++i) sHashBucketHead[i] = -1;
        sCacheCount = 0;
        ++sAtlasGen;
    }
    sDrawItemCount = 0;
    sLastLayerComposes = 0;
    /* sAnyDirtySlot / sDirtyRowMask are cleared by AtlasApplyStaged once the
     * rows are flushed: a frame that is collected but never drawn keeps its
     * staged tiles, and their rows, for the next one. */
    for (int i = 0; i < SORT_KEY_BUCKETS; ++i) sBucketHead[i] = -1;

    /* Palette hashes computed ONCE per frame here rather than per tile
     * reference -- see sCachePalHash's comment for why (used to be a
     * per-reference memcmp of up to 512 bytes, a real cost in GPUTIME
     * collectMs on hardware). gBgPltt/gObjPltt don't change mid-frame, so
     * every reference this frame can safely reuse these. */
    for (int b = 0; b < 16; ++b) sObjPalBankHash[b] = HashBytes(gObjPltt + b * 32, 32);
    sObjPalFullHash = HashBytes(gObjPltt, 512);
    /* The BG ones come from the effective palette, further down. */

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

    /* Power-bomb explosion circular flash (issue #28, see sPbFlashActive).
     * When it fires, suppress the whole-screen darken bake (sBldEffect 0);
     * the darken is reapplied only outside the hole in the per-eye pass at
     * the end of RenderFrame. sWindowActive is cleared right after the
     * window block below so the degenerate zero-width WIN1 rect doesn't clip
     * BG3. */
    sPbFlashActive = DetectPowerBombFlash();
    if (sPbFlashActive) sBldEffect = 0;

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

    /* Power-bomb flash (issue #28): the game's WIN1 rect is zero-width here
     * (its per-scanline resize is exactly what this port can't do), which as
     * a real clip would drop BG3 entirely. The circular darken is drawn
     * separately at the end of RenderFrame, so just disable window clipping
     * for this frame. */
    if (sPbFlashActive) sWindowActive = false;

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
    /* OBJWIN is deliberately treated as NON-clipping here (no-op), matching
     * the CPU rasterizer / layer-workbench re-render. Its only confirmed use
     * is the pause SUIT screen (wireframe Samus as its own mask). There the
     * game sets WINOUT-high = BG3|OBJ, which a literal reading turns into
     * "inside the silhouette hide BG0-2, show BG3" -- and BG3 there is the
     * unrelated minimap tilemap (green squares), so on hardware that data
     * bled through Samus's outline (issue #16). The intended look is BG0-3
     * unaffected by the mask (BG2's opaque hexagon motif covers BG3
     * everywhere, inside the silhouette and out), which is exactly what
     * leaving every layer at WIN_VIS_ALWAYS produces. If a scene ever turns
     * up that needs OBJWIN to actually clip, this is where to revisit it. */
    sObjWindowActive = false;
    (void)ComputeObjWinMask;

    /* Depth depends on register state the collection loops below read one
     * layer at a time, so it is snapshotted once, up front, for all of them. */
    ComputeDepthState(dispcnt);

    /* Mode-1 affine BG2. When active, bg==2 in the collect loop below goes
     * to CollectAffineBg2 instead of the text-mode CollectBgLayer. */
    sAffineBg2Active = DetectAffineBg2();

    /* Issue #29: is this frame the single-layer BG3 ripple? If so, collect
     * BG3 for the offscreen strip pass and keep it out of the normal
     * back-to-front list. Windowed / power-bomb-flash frames fall through to
     * the flat path (kept simple: those never coincide with a ripple room). */
    sHazeActive = sHazeRtReady && (dispcnt & (1u << 11)) && !sWindowActive && !sPbFlashActive &&
                  PortHaze_Bg3RowScroll(sHazeRowDelta, &sHazeBakeHofs);
    /* WIDE view (port_wide_view.h). Left off only for the affine BG2 scene,
     * which draws as the plain frame. The BG3 haze pass widens with it
     * (CollectHazeBg3). Window frames widen too: the WIN0/WIN1 rect is not
     * used to clip anything in this renderer, only the per-layer inside /
     * outside visibility is (ItemPassesWindow), and that holds anywhere.
     * Excluding them flashed the plain frame for the one windowed frame at
     * the end of a power bomb. */
    sWideOn = PortWide_GameActive() && !sAffineBg2Active;
    sWideMarginX = sWideOn ? PortWide_MarginX() : 0;
    sWideMarginY = sWideOn ? PortWide_MarginY() : 0;
    PortWide_SetFrameDrawn(sWideOn);
    if (sWideOn) ComputeWideView();
    else sWideView = (WideView){ 0 };
    /* The visible range in frame coordinates is [-margin + shift, 240 + margin
     * + shift] (likewise vertically). */
    sExtL = sWideMarginX - sWideView.shiftX;
    sExtR = sWideMarginX + sWideView.shiftX;
    sExtT = sWideMarginY - sWideView.shiftY;
    sExtB = sWideMarginY + sWideView.shiftY;

    /* The BG palette the tiles are decoded from (see PalFadeBeginFrame), then
     * its change stamps and hashes. A palette fade goes to the GPU only when
     * every BG layer is drawn through a layer map and the per-tile pass,
     * which are what apply it: not with the layer maps off (block passes),
     * an 8bpp text layer (one palette for the whole layer), the affine BG2
     * or the BG3 haze pass. */
    {
        bool fadeAllowed = !sObjWindowActive && !sHazeActive && !sAffineBg2Active;
        for (int bg = 0; bg < 4 && fadeAllowed; ++bg) {
            if (!(dispcnt & (1u << (8 + bg)))) continue;
            const uint16_t cnt = (uint16_t)(gIoMem[0x08 + bg * 2] | (gIoMem[0x09 + bg * 2] << 8));
            if (!sLmReady[bg] || (cnt & 0x80u)) fadeAllowed = false;
        }
        PalFadeBeginFrame(fadeAllowed);
    }
    BgPaletteStampsBeginFrame();
    for (int b = 0; b < 16; ++b) sBgPalBankHash[b] = HashBytes((const uint8_t*)sBgPalEff + b * 32, 32);
    sBgPalFullHash = HashBytes((const uint8_t*)sBgPalEff, 512);

    {
        const u64 now = svcGetSystemTick();
        PlatformGpu3DS_PerfPhaseAdd(PERF_PHASE_COLLECT_REST, now - tPhase);
        tPhase = now;
    }
    sHazeFromMap = false;
    if (sHazeActive) {
        const u64 tHaze = svcGetSystemTick();
        CollectHazeBg3();
        sPerfCount[PERF_COUNT_HAZE_US] += TicksToUs(svcGetSystemTick() - tHaze);
    }

#ifdef PORT_DEBUG_TOOLS_ACTIVE
    sDiagAffineDrawn[0] = sDiagAffineDrawn[1] = 0;
    sDiagBlendDrawn[0] = sDiagBlendDrawn[1] = 0;
    sDiagSemiTransColl = sDiagAffineColl = sDiagMosaicColl = 0;
    sDiagSemiTransX = sDiagSemiTransOam = -1;
#endif
    /* Sprites first. The item table and the tile atlas both have a ceiling,
     * and the WIDE view's extra background tiles come close to it; whatever is
     * collected last is what gets dropped, and a missing enemy is far worse
     * than a missing patch of far-off scenery. Draw order comes from the sort
     * keys, not from collection order, so this changes nothing else. */
    {
        const u64 now = svcGetSystemTick();
        PlatformGpu3DS_PerfPhaseAdd(PERF_PHASE_BG, now - tPhase); /* the haze BG3 */
        tPhase = now;
    }
    sPushFx = 0; /* only BG layers set it */
    if (dispcnt & (1u << 12)) {
        for (int i = 127; i >= 0; --i) CollectSprite(i, obj1D);
    }
    {
        const u64 now = svcGetSystemTick();
        PlatformGpu3DS_PerfPhaseAdd(PERF_PHASE_SPRITES, now - tPhase);
        tPhase = now;
    }
    sCollectOffX = sCollectOffY = 0.0f;
    for (int bg = 3; bg >= 0; --bg) {
        sCollectOffX = sCollectOffY = 0.0f; /* CollectBgLayer sets its own */
        sPushFx = 0;                         /* likewise */
        if (!(dispcnt & (1u << (8 + bg)))) continue;
        if (sHazeActive && bg == 3) continue; /* drawn via the offscreen strip pass */
        if (sAffineBg2Active && bg == 2) { CollectAffineBg2(); continue; }
        const u64 tLayer = svcGetSystemTick();
        CollectBgLayer(bg);
        sPerfCount[PERF_COUNT_BG_LAYERS_US] += TicksToUs(svcGetSystemTick() - tLayer);
    }
    sCollectOffX = sCollectOffY = 0.0f; /* sprites, next frame, take no layer offset */
    sPushFx = 0;
    {
        const u64 now = svcGetSystemTick();
        PlatformGpu3DS_PerfPhaseAdd(PERF_PHASE_BG, now - tPhase);
        tPhase = now;
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

#ifdef PORT_DEBUG_TOOLS_ACTIVE
    {
        static unsigned sDiagCounter;
        if ((sDiagCounter++ % 5u) == 0u) {
            char msg[900];
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
            {
                extern u8 gCurrentArea; extern u8 gCurrentRoom;
                off += __builtin_snprintf(msg + off, sizeof(msg) - (size_t)off,
                                          " room=%u,%u haze=%d semiT=%d aff=%d mos=%d stX=%d stOam=%d",
                                          gCurrentArea, gCurrentRoom, sHazeActive,
                                          sDiagSemiTransColl, sDiagAffineColl, sDiagMosaicColl,
                                          sDiagSemiTransX, sDiagSemiTransOam);
            }
            for (int bg = 0; bg < 4 && off < (int)sizeof(msg); ++bg)
                off += __builtin_snprintf(msg + off, sizeof(msg) - (size_t)off, " LM%d[ops=%d]", bg,
                                          sLmOpCount[bg]);
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


    u64 tAfterCollect = svcGetSystemTick();
    PlatformGpu3DS_PerfPhaseAdd(PERF_PHASE_COLLECT_REST, tAfterCollect - tPhase);
    for (int c = 0; c < PERF_COUNT_COUNT; ++c) {
        PlatformGpu3DS_PerfCountAdd((PerfCounter)c, sPerfCount[c]);
        sPerfCount[c] = 0;
    }
    sLastCollectMs = (float)((double)(tAfterCollect - tStart) / PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC);
    sLastAtlasUploadMs = (float)((double)(tAfterCollect - tBeforeUpload) / PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC);
    sFrameCollected = true;
}

/* Draw half, after C3D_FrameBegin (the GPU has finished the previous frame,
 * which may have been sampling these slots): copy the tiles collection
 * staged into the atlas (see sStageTexels), then flush the rows they touched. */
static void AtlasApplyStaged(void) {
    for (int i = 0; i < sStageCount; ++i) {
        const int slot = sStageSlot[i];
        memcpy((AtlasTexel*)sAtlasTexture.data + (size_t)slot * 64, sStageTexels[i], sizeof(sStageTexels[i]));
        sStageIndex[slot] = -1;
    }
    sStageCount = 0;
    if (sAnyDirtySlot) {
        /* The staged texels are now in sAtlasTexture.data (see kSwizzleLUT) -- no GX
         * transfer needed at all, just make sure the GPU sees the CPU's
         * writes via a plain cache flush. This is what actually removes
         * the ~19-20ms/frame blocking cost that C3D_SyncDisplayTransfer
         * had, independent of transferred size (see
         * docs/3ds-port-gpu-renderer-status-2026-08-20.md section 14) --
         * flushing each dirty row-run separately (same byte-offset math as
         * the old transfer-based version) rather than the whole atlas is
         * still worth doing, just far cheaper to begin with now. */
        size_t rowBytes = (size_t)ATLAS_W * sizeof(AtlasTexel);
        /* One word per 64 slot rows since the atlas grew past 64 rows tall
         * (see the ATLAS_* enum). Runs are found per word, so a run that
         * spans a word boundary is flushed as two, which costs one extra
         * syscall and nothing else. */
        for (size_t w = 0; w < sizeof(sDirtyRowMask) / sizeof(sDirtyRowMask[0]); ++w) {
            uint64_t mask = sDirtyRowMask[w];
            const int rowBase = (int)w * 64;
            while (mask != 0) {
                int startRow = __builtin_ctzll(mask);
                uint64_t shifted = mask >> startRow; /* bit 0 is guaranteed set */
                /* Run length = index of the first zero bit above startRow.
                 * __builtin_ctzll(~shifted) is undefined for an all-ones
                 * input in C, but shifted here is guaranteed non-zero and
                 * has at most 64 - startRow valid bits, so ~shifted is
                 * never 0 for any startRow > 0. For startRow == 0 and all
                 * 64 bits dirty, runLength = 64 directly. */
                int runLength = (~shifted == 0) ? 64 : __builtin_ctzll(~shifted);
                AtlasTexel* flushStart = (AtlasTexel*)sAtlasTexture.data + (size_t)(rowBase + startRow) * 8 * ATLAS_W;
                size_t flushBytes = (size_t)runLength * 8 * rowBytes;
                FlushAtlasRange(flushStart, flushBytes);
                mask &= ~(((1ull << runLength) - 1ull) << startRow);
            }
            sDirtyRowMask[w] = 0;
        }

        /* Issue #17: flushing the CPU data cache above only makes the new
         * texels visible in MEMORY -- it says nothing to the PICA200's own
         * texture cache, which sits in front of it. citro2d binds a texture
         * (C3D_TexBind) only when the texture POINTER changes between draw
         * calls, and every draw in this renderer uses this one atlas: so
         * after the very first frame the bind never happens again, and the
         * texture-unit config write it triggers -- the write that carries
         * GPUREG_TEXUNIT_CONFIG's texture-cache-clear bit -- is never
         * re-issued either. Any slot redecoded IN PLACE (see
         * GetOrDecodeTileSlot: changed VRAM bytes, changed palette colors,
         * or changed evy) then keeps drawing with whatever texels the GPU
         * cached the last time it sampled that slot.
         *
         * Normally invisible: a gameplay frame samples hundreds of distinct
         * atlas tiles spread over a 1MB texture, so the GPU's cache is
         * thrashed constantly and stale entries are evicted before anyone
         * notices. It becomes visible exactly when the working set is tiny
         * and the palette is what's changing -- which is Samus's death
         * animation: no BG layers at all, a handful of sprite tiles whose
         * VRAM bytes never change, and a palette rewritten every single
         * frame. Confirmed against the 2026-09-02 scene recording: within a
         * SINGLE captured frame, Samus's on-screen colors came from two
         * different earlier palette phases at once (some tiles frozen at
         * the recording's sample ~97-102, others at ~109-119), never the
         * current one -- per-slot staleness in the GPU's cache, not in
         * ours, whose palette-hash check (sCachePalHash) had already
         * redecoded them correctly in memory.
         *
         * Re-binding the atlas is the cheap fix: C3D_TexBind sets citro3d's
         * per-unit dirty flag, and the next draw re-emits the texture-unit
         * config -- cache-clear bit included -- before anything samples the
         * atlas this frame. Once per frame, and only on frames that
         * actually rewrote a slot. Done in the draw half, inside the frame. */
        sAtlasRebindPending = true;
        sAnyDirtySlot = false;
    }
    if (sAffineBg2ComposePending) {
        ComposeAffineBg2();
        sAffineBg2ComposePending = false;
    }
}

/* GPU half: everything from here on records GPU commands, so it must run
 * between C3D_FrameBegin and C3D_FrameEnd. */
void Port_GpuRenderer_DrawFrame(void) {
    if (!sInitialized) return;
    if (!sFrameCollected) Port_GpuRenderer_CollectFrame();
    sFrameCollected = false;
    AtlasApplyStaged();
    const u64 tAfterCollect = svcGetSystemTick();
    if (sAtlasRebindPending) {
        C3D_TexBind(0, &sAtlasTexture);
        sAtlasRebindPending = false;
    }
    BatchNewFrame();
    const uint16_t dispcnt = (uint16_t)(gIoMem[0] | (gIoMem[1] << 8));
    (void)dispcnt;

    float slider3d = PlatformGpu3DS_Get3DSlider();
    int style = Port_Config_Get3DSDisplayStyle();
    int aspect = Port_Config_Get3DSAspectRatio();
    /* HUD-in-the-border needs the black border, which WIDE fills with world. */
    bool hudOutside = (style == 0 && Port_Config_GetHudOutside() && !PortWide_Selected());

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
        } else { /* ORIGINAL (3:2 / 360x240), and the retired WIDE (0) with it.
                  * Must stay in lockstep with PlatformGpu3DS_GetTopImageRect:
                  * the screen-space effects position themselves off that rect,
                  * so a disagreement here misplaces them. */
            scaleX = 1.5f;
            scaleY = 1.5f;
            screenBaseX = 20.0f;
            screenBaseY = 0.0f;
        }
    }

    /* Where things start on screen. The world slides by the WIDE view's shift
     * (ComputeWideView); screen-fixed items (HUD, overlay text) stay on the
     * GBA frame, and the HUD itself rides up to the top edge of the screen
     * along with the extra rows the view adds above the frame. All equal
     * screenBase when WIDE is off. */
    const float worldBaseX = screenBaseX - (float)sWideView.shiftX * scaleX;
    const float worldBaseY = screenBaseY - (float)sWideView.shiftY * scaleY;
    /* Keyed on the setting, not on this frame being widened: a door
     * transition draws a few plain frames (the room-settle window), and the
     * HUD must not jump down to the frame and back up for them. */
    const float hudBaseY = screenBaseY - (float)PortWide_MarginY() * scaleY;
    const bool hudOverMasks = sWideOn || PortWide_MarginY() > 0;

    C3D_RenderTarget* leftTarget = PlatformGpu3DS_GetTopLeftTarget();
    C3D_RenderTarget* rightTarget = (slider3d > 0.01f) ? PlatformGpu3DS_GetTopRightTarget() : NULL;

    /* Issue #29: render this frame's BG3 into the BACK buffer (sHazeCur^1).
     * The eye passes below sample the FRONT buffer (sHazeCur), which was
     * rendered last frame and is guaranteed finished. No in-frame
     * render-to-texture race, and both eyes read the same complete buffer.
     * The buffers flip at the end of the frame. */
    /* Layer maps: redraw the cells that went stale (see LayerMapInit). Blend
     * ONE/ZERO with the alpha test off, so a cell is replaced outright --
     * transparent texels included, which is what clears a cell whose tile
     * went transparent. Sampled later this frame, hence the frame split
     * below (sLastLayerComposes). */
    const u64 tMaps = svcGetSystemTick();
    for (int li = 0; li < 4; ++li) {
        const int ops = sLmOpCount[li];
        if (ops == 0 || !sLmReady[li]) continue;
        sLmOpCount[li] = 0;
        C2D_SceneBegin(sLmRT[li]);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        ConfigureAtlasTextureEnv();
        BatchSetTierOffsets(NULL);
        if (sBatchReady) BatchBeginTarget((float)LM_W, (float)LM_H, false);
        for (int i = 0; i < ops; ++i) {
            const int cellIndex = sLmOps[li][i];
            C2D_DrawParams p = { { (float)((cellIndex % LM_COLS) * 8), (float)((cellIndex / LM_COLS) * 8),
                                   8.0f, 8.0f }, { 0.0f, 0.0f }, 0.5f, 0.0f };
            const Tex3DS_SubTexture* sub = &sSlotSubtexTable[sLmOpSlot[li][i]];
            if (!sBatchReady || !BatchQuad(&sAtlasTexture, sub, &p, BATCH_TIER_NONE)) {
                if (sBatchReady) { BatchFlush(); C2D_Prepare(); C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL); }
                C2D_Image img = { &sAtlasTexture, sub };
                C2D_DrawImage(img, &p, NULL);
                C2D_Flush();
                ConfigureAtlasTextureEnv();
                C3D_AlphaTest(false, GPU_ALWAYS, 0);
                if (sBatchReady) BatchBeginTarget((float)LM_W, (float)LM_H, false);
            }
        }
        if (sBatchReady) BatchEnd();
        C2D_Flush();
        ++sLastLayerComposes;
    }
    PlatformGpu3DS_PerfPhaseAdd(PERF_PHASE_DRAW_MAPS, svcGetSystemTick() - tMaps);
    C3D_AlphaTest(true, GPU_GREATER, 0);
    /* Sampled later this frame -- by the eye passes, and by the haze ripple
     * when BG3 comes from its map -- so split here, once: sampling a target
     * rendered in the same C3D frame without one is a read-after-write race
     * (one eye came back as 8x8-block garbage on hardware). Double buffering
     * would avoid the sync but lag the layer a frame behind everything else
     * every time a cell changes. */
    const bool lmSplit = sLastLayerComposes > 0;
    if (lmSplit) C3D_FrameSplit(0);

    const int sHazeBack = sHazeCur ^ 1;
    if (sHazeActive && !sHazeFromMap) {
        C2D_SceneBegin(sHazeRT[sHazeBack]);
        /* C3D_CLEAR_COLOR only -- no depth buffer on these targets. */
        C3D_RenderTargetClear(sHazeRT[sHazeBack], C3D_CLEAR_COLOR, 0, 0);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
        C3D_AlphaTest(true, GPU_GREATER, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        ConfigureAtlasTextureEnv();
        for (int i = 0; i < sHazeTileCount; ++i) {
            C2D_DrawImageAt(sHazeTiles[i].img, (float)HAZE_MARGIN + sHazeTiles[i].x,
                            sHazeTiles[i].y, 0.0f, NULL, 1.0f, 1.0f);
            /* citro2d re-inits the texenv on the first draw of a scene --
             * without undoing that stomp, tiles 1..N land with citro2d's
             * default env, which channel-permutes our RGBA8 atlas (green/red
             * grid where BG3 should be). */
            if (i == 0) ConfigureAtlasTextureEnv();
        }
        C2D_Flush();
        memcpy(sHazeBakedRowDelta[sHazeBack], sHazeRowDelta, sizeof(sHazeBakedRowDelta[sHazeBack]));
        sHazeBakedRows[sHazeBack] = sHazeRows;
        sHazeBakedMarginX[sHazeBack] = sHazeMarginX;
        sHazeBakedMarginY[sHazeBack] = sHazeMarginY;
        sHazeBufReady[sHazeBack] = true;
    }

    /* Mode 3: ripple once into a target here, so each eye below draws one
     * quad instead of 160 strips. Samples the FRONT compose buffer, which
     * was finished last frame, exactly as the eye passes do -- so this adds
     * no lag of its own beyond the one the compose already has. */
    const bool hazeRippleRT = sHazeActive &&
                              sHazeRippleReady && (sHazeFromMap || sHazeBufReady[sHazeCur]);
    if (hazeRippleRT) {
        if (sHazeFromMap) HazeRippleFromMap(sHazeCur);
        else HazeRippleIntoTarget(sHazeCur);
        /* Written and sampled inside one frame -- see sHazeRippleTex. */
        C3D_FrameSplit(0);
    }


#ifdef PORT_DEBUG_TOOLS_ACTIVE
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

    sLastDrawCalls = 0;
    sLastBlendTransitions = 0;
    sLastEyesRendered = 0;
    sLastDrawnPixels = 0;

    /* The scene's quads differ between the eyes only by each tier's
     * horizontal offset, which the batch shader adds from a uniform. So the
     * first eye drawn writes the vertices once, without offsets, and the
     * second re-issues the same quads and state changes with its own offsets
     * (sBatchReplay) instead of building them again -- the per-quad CPU work
     * was the cost of the second eye. The offsets are whole pixels, so
     * adding them after the pixel snap lands where snapping the offset
     * position did. */
    int replayStart = 0, replayEnd = 0;
    bool replayOk = false;
    uint32_t replayPixels = 0;

    for (int eye = 0; eye < 2; ++eye) {
        C3D_RenderTarget* target = (eye == 0) ? leftTarget : rightTarget;
        if (!target) continue;
        float eyeSign = (eye == 0) ? 1.0f : -1.0f;
        /* Whole device pixels, rounded ONCE per tier per eye.
         *
         * A fractional parallax offset cannot be drawn: with GPU_NEAREST
         * every tile quad rounds it independently, so part of a layer shifts
         * by a pixel and part of it doesn't. Within one layer that tears
         * glyphs apart -- and because the fractional part changes with the
         * slider, WHICH glyphs tear changes as the slider moves, which is the
         * "text gets cut when I change the depth" symptom exactly.
         *
         * Rounding here makes each tier shift rigidly, as one plane. The cost
         * is that a tier whose offset never reaches half a pixel (BG priority
         * 0's -0.3f at any slider position) renders with no parallax at all --
         * but it never really had any: what it had was per-tile rounding
         * noise that read as shimmer. */
        float tierOffset[BATCH_TIERS] = { 0 };
        for (int t = 0; t < PORT_TIER_COUNT && t < BATCH_TIER_NONE; ++t)
            tierOffset[t] = floorf(eyeSign * slider3d * PortStereoDepth_TierPx(t) + 0.5f);

        /* Issue #17: the GBA shows BG palette entry 0 -- the backdrop --
         * wherever no enabled layer draws, which this renderer used to
         * replace with a hardcoded black. Usually harmless (rooms fill the
         * screen with BG tiles), but Samus's death animation enables no BG
         * layer at all: the whole screen IS the backdrop, and the game
         * fades it to white through palette RAM. Clearing to black dropped
         * that entire fade. port/ppu/src/mode1.c uses mode1_bg_abgr_lut[0]
         * raw for the same purpose (no BLDCNT brighten/darken applied to
         * the backdrop), so this matches it exactly. The letterbox/
         * pillarbox masks drawn at the end of this pass re-blacken
         * everything outside the 240x160 GBA frame, so filling the whole
         * target here is safe for every display style. */
        C2D_TargetClear(target, BackdropClearColor());
        C2D_SceneBegin(target);
        ConfigureAtlasTextureEnv();
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
        C3D_AlphaTest(true, GPU_GREATER, 0);
        /* Opaque pass: all passing fragments have alpha=255. Use ONE/ZERO blend
         * to avoid reading back the destination framebuffer from VRAM, saving
         * massive memory bandwidth during scaled / full-screen rendering. */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

        /* Issue #29: lay the rippled BG3 down first (it is the backmost
         * layer in every affected room); BG0-2 / OBJ composite on top.
         * Give it the same stereo parallax offset BG3 would get in the
         * normal pass -- without it the strips sit at screen depth while the
         * parallaxed BG0-2 recede behind, so BG3 reads as being in FRONT. */
        if (sHazeActive && sHazeBufReady[sHazeCur]) {
            float eyeOffBg3 = floorf(eyeSign * slider3d *
                                     PortStereoDepth_TierPx(PortStereoDepth_BgTier(&sDepthState, 3)) + 0.5f);
            if (hazeRippleRT) {
                HazeBlitRippled(sHazeCur, screenBaseX + eyeOffBg3, screenBaseY, scaleX, scaleY);
            } else {
                HazeBlitStrips(sHazeCur, screenBaseX + eyeOffBg3, screenBaseY, scaleX, scaleY);
            }
        }

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
        /* Texenv the items are being drawn with: bit 0 plainEnv, bits 1-2
         * gpuFx. Re-applied after anything citro2d may have reset. */
        int envActive = 0;
#define ITEM_ENV_KEY(ITEM) (((ITEM)->plainEnv ? 1 : 0) | ((int)(ITEM)->gpuFx << 1))
#define APPLY_ITEM_ENV(KEY)                                                         \
        do {                                                                        \
            if ((KEY) >> 1) ConfigureFxTextureEnv((KEY) >> 1);                      \
            else if ((KEY) & 1) ConfigurePlainTextureEnv();                         \
            else ConfigureAtlasTextureEnv();                                        \
        } while (0)
        int scissorPasses = sWindowActive ? 2 : 1;
        bool blendModeActive = false;
        /* Items go through the batch (see BatchQuad) unless it could not be
         * set up; either way the state changes below flush what came first. */
        const bool batched = sBatchReady;
        const u64 tItems = svcGetSystemTick();
        BatchSetTierOffsets(tierOffset);
        if (batched) BatchBegin();
        const bool replaying = batched && replayOk;
        if (replaying) {
            sBatchUsed = replayStart;
            sBatchDrawn = replayStart;
            sBatchReplay = true;
        } else {
            replayStart = sBatchUsed;
        }
        bool batchOverflowed = false;
        uint32_t eyePixels = 0;
#define FLUSH_ITEMS() do { if (batched) BatchFlush(); else C2D_Flush(); } while (0)
        for (int sp = 0; sp < scissorPasses; ++sp) {
            bool insidePass = (sp == 0);
            for (int oi = 0; oi < sDrawOrderCount; ++oi) {
                const DrawItem* item = &sDrawItems[sDrawOrder[oi]];
                if (!ItemPassesWindow(sWindowActive, item->winVis, insidePass)) continue;
                if (hudOutside && item->isHud) continue;
                if (hudOverMasks && item->isHud) continue; /* drawn over the masks, below */
                /* item->blendAlpha already implies alpha mode: the BG/OBJ
                 * collect paths only set it when sBldEffect==1, and a
                 * Semi-Transparent OBJ sets it unconditionally (and must
                 * blend even if BLDCNT is left in another mode). */
                bool wantBlend = item->blendAlpha;
                if (wantBlend != blendModeActive) {
                    ++sLastBlendTransitions; /* each one is a batch break */
                    FLUSH_ITEMS();
                    if (wantBlend) {
                        /* GBA alpha blend (GBATEK): out = min(31, src*EVA/16 + dst*EVB/16),
                         * with EVA and EVB independent. Pack EVA/16 into the blend
                         * colour's RGB and EVB/16 into its alpha, then take the src
                         * side from GPU_CONSTANT_COLOR and the dst side from
                         * GPU_CONSTANT_ALPHA so each gets its own coefficient.
                         * GPU_BLEND_ADD saturates in [0,1], matching GBA's clamp to 31.
                         * The old code used dst factor 1-EVA/16 (a normalised lerp that
                         * ignored EVB) -- correct only where EVA+EVB==16, but wrong for
                         * the transparency 0x08-0x17 rooms where EVB is forced to 16
                         * (sum > 16, additive), which came out far too dark. */
                        u32 evaByte = (u32)((sBldEva * 255) / 16);
                        u32 evbByte = (u32)((sBldEvb * 255) / 16);
                        C3D_BlendingColor(C2D_Color32(evaByte, evaByte, evaByte, evbByte));
                        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_CONSTANT_ALPHA,
                                       GPU_CONSTANT_COLOR, GPU_CONSTANT_ALPHA);
                    } else {
                        /* Opaque mode: ONE/ZERO, see the comment above the
                         * initial C3D_AlphaBlend call before this loop. */
                        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
                    }
                    blendModeActive = wantBlend;
                }
                const int tier = (item->depthTier >= 0 && item->depthTier < BATCH_TIER_NONE)
                                     ? item->depthTier : BATCH_TIER_NONE;
                /* Render target or atlas, and the fades applied at draw
                 * time (see ConfigureFxTextureEnv). */
                const int envKey = ITEM_ENV_KEY(item);
                if (envKey != envActive) {
                    FLUSH_ITEMS();
                    APPLY_ITEM_ENV(envKey);
                    envActive = envKey;
                    reassertedTexEnv = true;
                }
                const float baseX = item->screenFixed ? screenBaseX : worldBaseX;
                const float baseY = item->screenFixed ? (item->isHud ? hudBaseY : screenBaseY) : worldBaseY;
                C2D_DrawParams params;
                bool drawn = false;
                if (batched) {
                    /* The eye offset comes from the shader (tierOffset). */
                    if (!replaying) params = BuildDrawParams(item, baseX, baseY, 0.0f, scaleX, scaleY, false);
                    drawn = BatchQuad(item->img.tex, item->img.subtex, replaying ? NULL : &params, tier);
                    if (!drawn) batchOverflowed = true;
                    else if (!replaying) eyePixels += (uint32_t)(params.pos.w * params.pos.h);
                }
                if (!drawn) {
                    /* Batch full (or unavailable): citro2d for this one. */
                    params = BuildDrawParams(item, baseX, baseY, tierOffset[tier], scaleX, scaleY, false);
                    if (batched) { BatchFlush(); C2D_Prepare(); C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL); }
                    C2D_DrawImage(item->img, &params, NULL);
                    C2D_Flush();
                    APPLY_ITEM_ENV(envActive);
                    if (batched) BatchBegin();
                    eyePixels += (uint32_t)(params.pos.w * params.pos.h);
                }
                ++drawCount;
#ifdef PORT_DEBUG_TOOLS_ACTIVE
                if (item->affine)      ++sDiagAffineDrawn[eye & 1];
                if (item->blendAlpha)  ++sDiagBlendDrawn[eye & 1];
#endif
                if (!reassertedTexEnv) {
                    APPLY_ITEM_ENV(envActive);
                    reassertedTexEnv = true;
                }
            }
        }
        if (batched) BatchEnd();
        /* Device pixels the quads covered, summed over every eye: separates
         * "cost is per quad" from "cost is per pixel", which the quad count
         * alone cannot. A replayed eye covers what the first one did. */
        if (replaying) {
            sBatchReplay = false;
            eyePixels = replayPixels;
        } else {
            replayEnd = sBatchUsed;
            replayOk = batched && !batchOverflowed;
            replayPixels = eyePixels;
        }
        if (sBatchUsed < replayEnd) sBatchUsed = replayEnd;
        sLastDrawnPixels += eyePixels;
        PlatformGpu3DS_PerfPhaseAdd(PERF_PHASE_DRAW_ITEMS, svcGetSystemTick() - tItems);
#undef FLUSH_ITEMS
#undef APPLY_ITEM_ENV
#undef ITEM_ENV_KEY
        if (envActive) {
            C2D_Flush();
            ConfigureAtlasTextureEnv();
            envActive = 0;
        }

        /* Depth-tint debug view: re-draw every item's silhouette in its stereo
         * tier's flat colour, over the normal render. A separate pass -- not
         * interleaved with the loop above -- so the normal path's texenv state
         * machine is untouched. Mirrors HazeRippleIntoTarget's proven "set
         * env, loop, reassert-after-first-draw, flush" shape. Ignores window
         * clipping (a debug view doesn't need it).
         *
         * Blended ~78% over the scene rather than opaque: a layer that fills
         * the screen (a cutscene backdrop) would otherwise hide everything, so
         * you keep enough of the real image to tell what you are looking at
         * while the plane colour still dominates. */
        if (sDepthTint) {
            C2D_Flush();
            C3D_BlendingColor(C2D_Color32(0, 0, 0, 200)); /* Ac = 200/255 tint */
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA,
                           GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA);
            blendModeActive = true; /* force the restore below */
            bool tPlain = false, tReasserted = false;
            int tTier = -1;
            ConfigureDepthTintAtlasTexEnv();
            for (int oi = 0; oi < sDrawOrderCount; ++oi) {
                const DrawItem* item = &sDrawItems[sDrawOrder[oi]];
                if (hudOutside && item->isHud) continue;
                if (item->plainEnv != tPlain) {
                    C2D_Flush();
                    if (item->plainEnv) ConfigureDepthTintPlainTexEnv();
                    else                ConfigureDepthTintAtlasTexEnv();
                    tPlain = item->plainEnv; tTier = -1; tReasserted = true;
                }
                if (item->depthTier != tTier) {
                    C2D_Flush();
                    int t = item->depthTier;
                    if (t < 0 || t >= PORT_TIER_COUNT) t = PORT_TIER_OBJ_P1;
                    C3D_TexEnvColor(C3D_GetTexEnv(0), kDepthTintColor[t]);
                    tTier = item->depthTier;
                }
                float eo = floorf(eyeSign * slider3d *
                                  PortStereoDepth_TierPx(item->depthTier) + 0.5f);
                C2D_DrawParams p = BuildDrawParams(item,
                    item->screenFixed ? screenBaseX : worldBaseX,
                    item->screenFixed ? (item->isHud ? hudBaseY : screenBaseY) : worldBaseY,
                    eo, scaleX, scaleY, false);
                C2D_DrawImage(item->img, &p, NULL);
                if (!tReasserted) {
                    if (item->plainEnv) ConfigureDepthTintPlainTexEnv();
                    else                ConfigureDepthTintAtlasTexEnv();
                    tTier = -1;
                    tReasserted = true;
                }
            }
            C2D_Flush();
            ConfigureAtlasTextureEnv();
        }
        if (blendModeActive) {
            C2D_Flush();
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        }

        /* Power-bomb explosion circular flash (issue #28, see sPbFlashActive):
         * the scene above was drawn at full brightness; now lay translucent
         * black over everything OUTSIDE the bright ellipse to recreate the
         * BLDY darken + expanding hole, as a triangle annulus between the
         * ellipse edge and a box well outside the 400x240 screen (so all
         * four corners are covered). GPU_CONSTANT_ALPHA blend with Ac =
         * evy/16 matches GBA's brightness-decrease-toward-black fraction. */
        if (sPbFlashActive && sPbFlashEvy > 0) {
            C2D_Flush();
            u32 darkA = (u32)((sPbFlashEvy * 255) / 16);
            C3D_BlendingColor(C2D_Color32(0, 0, 0, darkA));
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA,
                           GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA);

            const u32 c = C2D_Color32(0, 0, 0, 255); /* vertex colour unused: blend is CONSTANT_ALPHA */
            float cx = worldBaseX + sPbFlashCxGba * scaleX;
            float cy = worldBaseY + sPbFlashCyGba * scaleY;
            float rx = sPbFlashRxGba * scaleX;
            float ry = sPbFlashRyGba * scaleY;
            if (rx < 1.0f) rx = 1.0f;
            if (ry < 1.0f) ry = 1.0f;

            const int kSeg = 64;
            float pix = 0.0f, piy = 0.0f, pox = 0.0f, poy = 0.0f;
            for (int i = 0; i <= kSeg; ++i) {
                float a = (float)i * (2.0f * (float)M_PI / (float)kSeg);
                float ca = cosf(a), sa = sinf(a);
                float ix = cx + rx * ca;
                float iy = cy + ry * sa;
                float m = fabsf(ca) > fabsf(sa) ? fabsf(ca) : fabsf(sa);
                if (m < 1e-4f) m = 1e-4f;
                float ox = cx + 600.0f * ca / m; /* on a half-600 box around the centre */
                float oy = cy + 600.0f * sa / m;
                if (i > 0) {
                    C2D_DrawTriangle(pix, piy, c, pox, poy, c, ix, iy, c, 0.0f);
                    C2D_DrawTriangle(pox, poy, c, ox, oy, c, ix, iy, c, 0.0f);
                }
                pix = ix; piy = iy; pox = ox; poy = oy;
            }

            C2D_Flush();
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        }

        /* WIDE view: the border is world, so the only black is what lies
         * outside the room itself. */
        if (sWideOn) {
            DrawWideRoomMasks(screenBaseX, screenBaseY, scaleX, scaleY);
        }

        /* Draw solid black border masks over letterbox/pillarbox areas (covers any
         * tiles or 3D stereo parallax layers that extend beyond the GBA frame). */
        if (!sWideOn && (screenBaseX > 0.0f || screenBaseY > 0.0f)) {
            /* Queued atlas quads first, then a plain-colour texenv, or the
             * rectangles sample the atlas (see DrawWideRoomMasks). */
            C2D_Flush();
            PlatformGpu3DS_ResetSolidTexEnv();
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

        if (hudOverMasks) {
            /* The HUD sits in the extra rows above the frame, which in a small
             * room are exactly what the masks blacken -- so it goes on top. */
            C2D_Flush();
            ConfigureAtlasTextureEnv();
            C3D_AlphaTest(true, GPU_GREATER, 0);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            bool hudEnvAsserted = false;
            for (int oi = 0; oi < sDrawOrderCount; ++oi) {
                const DrawItem* item = &sDrawItems[sDrawOrder[oi]];
                if (!item->isHud) continue;
                float eyeOffset = floorf(eyeSign * slider3d * PortStereoDepth_TierPx(item->depthTier) + 0.5f);
                C2D_DrawParams params = BuildDrawParams(item, screenBaseX, hudBaseY, eyeOffset, scaleX, scaleY, false);
                C2D_Image img = item->img;
                Tex3DS_SubTexture clippedSub;
                if (item->x + item->w > 240.0f) {
                    /* The minimap sprite overhangs the GBA frame by a few
                     * pixels, which the GBA cuts off; drawn whole it shows a
                     * stray white line down the map's right edge. Same clip
                     * as the HUD-outside pass. */
                    if (item->x >= 240.0f) continue;
                    const float visibleW = 240.0f - item->x;
                    params.pos.w = visibleW * scaleX;
                    clippedSub = *item->img.subtex;
                    clippedSub.right = clippedSub.left + (clippedSub.right - clippedSub.left) * (visibleW / item->w);
                    img.subtex = &clippedSub;
                }
                C2D_DrawImage(img, &params, NULL);
                ++drawCount;
                if (!hudEnvAsserted) {
                    ConfigureAtlasTextureEnv(); /* citro2d re-inits the TEV on a scene's first draw */
                    hudEnvAsserted = true;
                }
            }
            C2D_Flush();
            PlatformGpu3DS_ResetSolidTexEnv();
        }

        /* GBA Bezel overlay */
        if (PortGbaBezel_Active()) {
            PortGbaBezel_Draw(target);
        }

        /* If HUD is placed outside in the top border, draw HUD sprites now so they appear
         * cleanly on top of the black top bar (or top bezel). */
        if (hudOutside) {
            C2D_SceneBegin(target);
            ConfigureAtlasTextureEnv();
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
            C3D_AlphaTest(true, GPU_GREATER, 0);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            bool hudReassertedTexEnv = false;
            for (int oi = 0; oi < sDrawOrderCount; ++oi) {
                const DrawItem* item = &sDrawItems[sDrawOrder[oi]];
                if (!item->isHud) continue;
                if (item->x >= 240.0f || item->x + item->w <= 0.0f) continue;
                float eyeOffset = floorf(eyeSign * slider3d * PortStereoDepth_TierPx(item->depthTier) + 0.5f);
                C2D_DrawParams params = BuildDrawParams(item, screenBaseX, screenBaseY, eyeOffset, scaleX, scaleY, true);
                C2D_Image img = item->img;
                Tex3DS_SubTexture clippedSub;
                if (item->x + item->w > 240.0f) {
                    /* Clip subtile that spills beyond the 240px GBA screen width (e.g. minimap right edge) */
                    float visibleW = 240.0f - item->x;
                    params.pos.w = visibleW;
                    clippedSub = *item->img.subtex;
                    clippedSub.right = clippedSub.left + (clippedSub.right - clippedSub.left) * (visibleW / item->w);
                    img.subtex = &clippedSub;
                }
                C2D_DrawImage(img, &params, NULL);
                ++drawCount;
                if (!hudReassertedTexEnv) {
                    ConfigureAtlasTextureEnv();
                    hudReassertedTexEnv = true;
                }
            }
            C2D_Flush();
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            PlatformGpu3DS_ResetSolidTexEnv();
        }

        /* Shared overlay so a CPU fallback frame draws the exact same box
         * (see PlatformGpu3DS_DrawFpsOverlay). Parallax past the frontmost
         * world tier (HUD, +2.0px) and pixel-snapped like the tile layers,
         * so the counter always reads as being in front of everything. */
        /* No bare ResetSolidTexEnv() here: it would rewrite TEV 0 while this
         * eye's tile quads are still sitting unflushed in citro2d's vertex
         * buffer, and they would then be emitted with the solid-colour env
         * (untinted vertex colour == nothing) instead of the atlas env.
         * DrawFpsOverlay flushes first and resets the TEV itself. */
        PlatformGpu3DS_DrawFpsOverlay(floorf(eyeSign * slider3d * (+2.5f) + 0.5f));
        PlatformGpu3DS_DrawNoticeOverlay(floorf(eyeSign * slider3d * (+2.5f) + 0.5f));

        /* RetroAchievements unlock toast, when the player put it on the top
         * screen. Same frontmost parallax as the FPS box so it floats in
         * front of the whole scene; no-ops when the toast is on the bottom
         * screen or none is active. */
        extern void Port_RA_RenderToastOverlayTop(float eyeXOffset);
        Port_RA_RenderToastOverlayTop(floorf(eyeSign * slider3d * (+2.5f) + 0.5f));

        sLastDrawCalls += (uint32_t)drawCount;
        ++sLastEyesRendered;
#ifdef PORT_DEBUG_TOOLS_ACTIVE
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
                char msg[96];
                snprintf(msg, sizeof(msg), "EYE%d drawCount=%d reasserted=%d affine=%d blend=%d slider=%.2f",
                         eye, drawCount, (int)reassertedTexEnv,
                         sDiagAffineDrawn[eye & 1], sDiagBlendDrawn[eye & 1], (double)slider3d);
                Port_DebugLog(msg);
            }
        }
#endif
    }

    /* Issue #29: flip the haze buffers -- the back buffer we just rendered
     * becomes next frame's read buffer, by which point C3D_FrameEnd has
     * flushed it. */
    if (sHazeActive && !sHazeFromMap) sHazeCur = sHazeBack;

    BatchFrameDone();
    u64 tEnd = svcGetSystemTick();
    sLastDrawMs = (float)((double)(tEnd - tAfterCollect) / PORT_GPU_RENDERER_CPU_TICKS_PER_MSEC);

#ifdef PORT_DEBUG_TOOLS_ACTIVE
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

void Port_GpuRenderer_RenderFrame(void) {
    Port_GpuRenderer_CollectFrame();
    Port_GpuRenderer_DrawFrame();
}

/* Snapshot of the most recently rendered frame's item counts, for the
 * bottom-screen debug overlay in platform_gpu_3ds.c (PlatformGpu3DS_EndBottom)
 * -- lets a dev tell at a glance whether the GPU path is drawing a
 * reasonable scene or something degenerate (e.g. way too many items, which
 * was the tell for the citro2d buffer-exhaustion bug in section 2.5 of
 * docs/3ds-port-gpu-renderer-status-2026-08-20.md). Values hold their last
 * value between RenderFrame calls, which is fine since the overlay is only
 * read once per presented frame anyway. */
/* The WIDE view this frame was drawn with, for the scene recorder: slide
 * x/y, room masks left/right/top/bottom (GBA frame px), and flags (bit 0
 * widened, 1 leaving a room, 2 following the door tunnel). */
void Port_GpuRenderer_GetWideRecord(int16_t out[7]) {
    out[0] = (int16_t)sWideView.shiftX;
    out[1] = (int16_t)sWideView.shiftY;
    out[2] = (int16_t)sWideView.maskL;
    out[3] = (int16_t)sWideView.maskR;
    out[4] = (int16_t)sWideView.maskT;
    out[5] = (int16_t)sWideView.maskB;
    out[6] = (int16_t)((sWideOn ? 1 : 0) | (sLeaving ? 2 : 0) | (sTunnelStarted ? 4 : 0));
}

void Port_GpuRenderer_GetLastFrameStats(int* outItems, int* outObjItems, int* outCacheSlots) {
    if (outItems) *outItems = sDrawItemCount;
    if (outObjItems) *outObjItems = sLastObjItemCount;
    if (outCacheSlots) *outCacheSlots = sCacheCount;
}

/* Why the scene cost what it cost, for the perf/scene recorders -- see the
 * sLastDrawCalls declaration. windowActive doubles the pass over the whole
 * draw order (the scissor in/out split), so a frame with it set submits
 * roughly twice the quads for the same scene; hazeActive adds an offscreen
 * render-to-texture pass of hazeTiles tiles plus a 160-strip blit per eye. */
void Port_GpuRenderer_GetLastFrameDrawStats(PortGpuRendererDrawStats* out) {
    if (!out) return;
    out->drawCount = sLastDrawCalls;
    out->blendTransitions = sLastBlendTransitions;
    out->drawnPixels = sLastDrawnPixels;
    out->layerComposes = sLastLayerComposes;
    out->layerCacheOn = sLmReady[0] && sLmReady[1] && sLmReady[2] && sLmReady[3];
    out->hazeTiles = sHazeActive ? (uint32_t)sHazeTileCount : 0u;
    /* Collected items are per-eye: both eyes draw the same item list, so
     * drawCount is roughly eyesRendered * (bgItems + objItems) minus
     * whatever the window/HUD filters drop. */
    out->objItems = (uint32_t)sLastObjItemCount;
    out->bgItems = (sDrawItemCount > sLastObjItemCount)
                     ? (uint32_t)(sDrawItemCount - sLastObjItemCount) : 0u;
    out->cpuTileX100 = (sLastTileCollectMs > 0.0f) ? (uint32_t)(sLastTileCollectMs * 100.0f) : 0u;
    out->cpuUploadX100 = (sLastAtlasUploadMs > 0.0f) ? (uint32_t)(sLastAtlasUploadMs * 100.0f) : 0u;
    out->cpuDrawX100 = (sLastDrawMs > 0.0f) ? (uint32_t)(sLastDrawMs * 100.0f) : 0u;
    out->eyesRendered = sLastEyesRendered;
    out->scissorPasses = sWindowActive ? 2u : 1u;
    out->windowActive = sWindowActive;
    out->hazeActive = sHazeActive;
    out->hazeMode = sHazeRippleReady ? 3 : 0; /* RT, or the FULL fallback */
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
        /* Whole atlas, both regions: the 8x8 tile slots on top and the
         * 16x16 block slots below them (see the ATLAS_* enum). */
        fprintf(f, "P6\n%d %d\n255\n", ATLAS_W, ATLAS_H);
        const AtlasTexel* px = (const AtlasTexel*)sAtlasTexture.data;
        for (int y = 0; y < ATLAS_H; ++y) {
            for (int x = 0; x < ATLAS_W; ++x) {
                int tileCol = x / 8, tileRow = y / 8;
                int slot = tileRow * ATLAS_TILES_PER_ROW + tileCol;
                int localX = x % 8, localY = y % 8;
                const AtlasTexel texel = px[(size_t)slot * 64 + kSwizzleLUT[localY * 8 + localX]];
                const unsigned r5 = (texel >> 11) & 0x1Fu, g5 = (texel >> 6) & 0x1Fu, b5 = (texel >> 1) & 0x1Fu;
                uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
                uint8_t g = (uint8_t)((g5 << 3) | (g5 >> 2));
                uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
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
