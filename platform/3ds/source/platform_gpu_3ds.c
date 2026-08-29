#include "platform_gpu_3ds.h"
#include "port_debug_tools.h" /* PORT_DEBUG_TOOLS_ACTIVE */

#include <3ds.h>
#include <citro2d.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Implemented in port_ppu_mzm.c, which can safely include structs/samus.h
 * (this file can't: it also pulls in <3ds.h>/<citro3d.h>, whose u32 typedef
 * conflicts with the GBA-port one dragged in by structs/samus.h). */
void PortPpuMzm_DumpSamusState(void);

static C3D_RenderTarget* sTopTarget;
static C3D_RenderTarget* sTopRightTarget;
static C3D_RenderTarget* sBottomTarget;
static C3D_Tex sTopTexture;
static C3D_Tex sTopRightTexture;
static C3D_Tex sBottomTexture;
static Tex3DS_SubTexture sTopSubtexture;
static Tex3DS_SubTexture sBottomSubtexture;
static uint32_t* sTopUpload;
static uint32_t* sTopRightUpload;
static uint32_t* sBottomUploads[2];
static void* sC2dFlushBase;
static size_t sC2dFlushSize;
static bool sFrameActive;
static bool sReady;
static bool sOld3DSProfile;
static bool sBottomTargetValid;
static PlatformGpu3DSStats sStats;
static unsigned sTopPresentWidth = 240;

/* Serializes every actual GPU submission (C3D_FrameBegin/End, citro2d
 * draws) across the two threads that can now issue them: the present
 * thread (port_ppu_mzm.c's Port_PPU_GpuPresentPump, consuming a CPU-
 * rendered frame published by the game-logic thread) and the game-logic
 * thread itself, which still calls straight into this file synchronously
 * for the experimental GPU tile renderer path (PORT_GPU_TILE_RENDERER) and
 * the PORT_PPU_TEST_PATTERN diagnostic -- neither produces a CPU pixel
 * buffer that can be handed off the way the normal path does, so they were
 * left calling BeginTop.../EndBottom inline, same as before the present
 * thread existed. That was fine when everything ran on one thread; once
 * present moved to its own thread, those two call sites became a second,
 * completely unsynchronized caller of citro3d/citro2d (neither of which is
 * thread-safe) racing the present thread's own calls -- confirmed on
 * hardware as a hang (not a crash) exactly at a CPU<->GPU renderer
 * transition: the screen freezes while gameplay and audio (a separate
 * thread untouched by any of this) keep running normally, matching a
 * wedged GPU command queue rather than a crashed thread. */
static LightLock sGpuSubmitLock;

void PlatformGpu3DS_SubmitLock_Acquire(void) { LightLock_Lock(&sGpuSubmitLock); }
void PlatformGpu3DS_SubmitLock_Release(void) { LightLock_Unlock(&sGpuSubmitLock); }

enum {
    TOP_TEXTURE_WIDTH = 512,
    TOP_TEXTURE_HEIGHT = 256,
};

extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;
extern bool Port_Config_GetShowFps(void);
extern int Port_Config_Get3DSAspectRatio(void);
extern int Port_Config_Get3DSDisplayStyle(void);
extern double Port_PPU_3DS_CurrentFps(void);
extern bool Port_PPU_3DS_LastFrameUsedGpu(void);
extern void Port_GpuRenderer_GetLastFrameStats(int* outItems, int* outObjItems, int* outCacheSlots);

enum {
    TOP_ASPECT_WIDE = 0,
    TOP_ASPECT_ORIGINAL,
    TOP_ASPECT_STRETCH,
};

enum {
    TOP_DISPLAY_PIXEL_PERFECT = 0,
    TOP_DISPLAY_SCALED,
    TOP_DISPLAY_BLUR,
};

static const uint8_t* StatusGlyph(char c) {
    static const uint8_t digits[10][7] = {
        { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },
        { 14, 17, 1, 2, 4, 8, 31 },     { 30, 1, 1, 14, 1, 1, 30 },
        { 2, 6, 10, 18, 31, 2, 2 },     { 31, 16, 16, 30, 1, 1, 30 },
        { 14, 16, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
        { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 1, 14 },
    };
    /* A-Y minus Q/Z (never needed so far) -- extended from the original
     * A,C,D,E,F,G,M,P,S,U,V set to label the debug overlay properly
     * (NEW3DS/OLD3DS, item/obj/cache prefixes) instead of unlabeled
     * numbers -- see PlatformGpu3DS_EndBottom. */
    static const uint8_t letters[24][7] = {
        { 14, 17, 17, 31, 17, 17, 17 }, /* A */
        { 30, 17, 17, 30, 17, 17, 30 }, /* B */
        { 31, 16, 16, 16, 16, 16, 31 }, /* C */
        { 30, 17, 17, 17, 17, 17, 30 }, /* D */
        { 31, 16, 16, 30, 16, 16, 31 }, /* E */
        { 31, 16, 16, 30, 16, 16, 16 }, /* F */
        { 14, 17, 16, 23, 17, 17, 14 }, /* G */
        { 17, 17, 17, 31, 17, 17, 17 }, /* H */
        { 31, 4, 4, 4, 4, 4, 31 },      /* I */
        { 7, 2, 2, 2, 2, 18, 12 },      /* J */
        { 17, 18, 20, 24, 20, 18, 17 }, /* K */
        { 16, 16, 16, 16, 16, 16, 31 }, /* L */
        { 17, 27, 21, 21, 17, 17, 17 }, /* M */
        { 17, 25, 21, 19, 17, 17, 17 }, /* N */
        { 14, 17, 17, 17, 17, 17, 14 }, /* O */
        { 30, 17, 17, 30, 16, 16, 16 }, /* P */
        { 30, 17, 17, 30, 20, 18, 17 }, /* R */
        { 15, 16, 16, 14, 1, 1, 30 },   /* S */
        { 31, 4, 4, 4, 4, 4, 4 },       /* T */
        { 17, 17, 17, 17, 17, 17, 14 }, /* U */
        { 17, 17, 17, 17, 17, 10, 4 },  /* V */
        { 17, 17, 17, 21, 21, 27, 17 }, /* W */
        { 17, 10, 4, 4, 4, 10, 17 },    /* X */
        { 17, 10, 4, 4, 4, 4, 4 },      /* Y */
    };
    static const uint8_t letterIds[26] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        13, 14, 15, 255, 16, 17, 18, 19, 20, 21, 22, 23, 255,
    };
    static const uint8_t colon[7] = { 0, 12, 12, 0, 12, 12, 0 };
    static const uint8_t dot[7] = { 0, 0, 0, 0, 0, 12, 12 };
    static const uint8_t dash[7] = { 0, 0, 0, 31, 0, 0, 0 };
    static const uint8_t slash[7] = { 1, 2, 2, 4, 8, 8, 16 };
    static const uint8_t percent[7] = { 19, 19, 2, 4, 8, 25, 25 };
    if (c == ':') return colon;
    if (c == '.') return dot;
    if (c == '-') return dash;
    if (c == '/') return slash;
    if (c == '%') return percent;
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') {
        uint8_t id = letterIds[c - 'A'];
        if (id != 255) return letters[id];
    }
    return NULL;
}

void PlatformGpu3DS_DrawStatusText(float x, float y, float scale, const char* text) {
    const uint32_t color = C2D_Color32(255, 255, 255, 255);
    for (; *text; ++text, x += 6.0f * scale) {
        const uint8_t* glyph = StatusGlyph(*text);
        if (!glyph) continue;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5;) {
                if ((glyph[row] & (1u << (4 - col))) == 0) {
                    ++col;
                    continue;
                }
                int end = col + 1;
                while (end < 5 && (glyph[row] & (1u << (4 - end))) != 0) ++end;
                C2D_DrawRectSolid(x + col * scale, y + row * scale, 0.8f,
                                  (end - col) * scale, scale, color);
                col = end;
            }
        }
    }
}

static u32 TextureTransfer(void) {
    return GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
           GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
           GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
           GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
}

void PlatformGpu3DS_ConfigureAbgrTextureEnv(void) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
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

/* ConfigureAbgrTextureEnv() above wires TEV units 1 and 2 to sample
 * GPU_TEXTURE0 (the 3-stage byte-swizzle needed for the ABGR-composited
 * bottom-screen image). citro2d's own solid-color draws (C2D_DrawRectSolid,
 * used by the debug overlay and its font) only reprogram unit 0 when
 * switching from textured to solid mode -- it doesn't know our units 1/2
 * exist, so they stay wired to sample whatever texture unit 0 last pointed
 * at even though a solid draw binds no texture at all. Confirmed on real
 * hardware: a plain C2D_DrawRectSolid drawn right after
 * ConfigureAbgrTextureEnv() + C2D_DrawImage came out colorless/banded
 * instead of solid, exactly this leftover-TEV-state pattern (see
 * docs/3ds-port-gpu-renderer-status-2026-08-20.md section 19). Call this
 * after the textured draw and before any solid draw in the same scene to
 * put units 1/2 back to a harmless passthrough. */
void PlatformGpu3DS_ResetSolidTexEnv(void) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    for (int i = 1; i < 6; ++i) {
        env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
    }
}

bool PlatformGpu3DS_Init(bool old3dsProfile) {
    memset(&sStats, 0, sizeof(sStats));
    LightLock_Init(&sGpuSubmitLock);
    sOld3DSProfile = old3dsProfile;
    sBottomTargetValid = false;
    sC2dFlushBase = NULL;
    sC2dFlushSize = 0;
    sTopUpload = (uint32_t*)linearMemAlign(TOP_TEXTURE_WIDTH * TOP_TEXTURE_HEIGHT * sizeof(uint32_t), 0x80);
    sTopRightUpload = (uint32_t*)linearMemAlign(TOP_TEXTURE_WIDTH * TOP_TEXTURE_HEIGHT * sizeof(uint32_t), 0x80);
    sBottomUploads[0] = (uint32_t*)linearMemAlign(512u * 256u * sizeof(uint32_t), 0x80);
    sBottomUploads[1] = (uint32_t*)linearMemAlign(512u * 256u * sizeof(uint32_t), 0x80);
    if (!sTopUpload || !sTopRightUpload || !sBottomUploads[0] || !sBottomUploads[1]) goto fail_linear;
    memset(sTopUpload, 0, TOP_TEXTURE_WIDTH * TOP_TEXTURE_HEIGHT * sizeof(uint32_t));
    memset(sTopRightUpload, 0, TOP_TEXTURE_WIDTH * TOP_TEXTURE_HEIGHT * sizeof(uint32_t));
    memset(sBottomUploads[0], 0, 512u * 256u * sizeof(uint32_t));
    memset(sBottomUploads[1], 0, 512u * 256u * sizeof(uint32_t));
    GSPGPU_FlushDataCache(sTopUpload, TOP_TEXTURE_WIDTH * TOP_TEXTURE_HEIGHT * sizeof(uint32_t));
    GSPGPU_FlushDataCache(sTopRightUpload, TOP_TEXTURE_WIDTH * TOP_TEXTURE_HEIGHT * sizeof(uint32_t));
    GSPGPU_FlushDataCache(sBottomUploads[0], 512u * 256u * sizeof(uint32_t));
    GSPGPU_FlushDataCache(sBottomUploads[1], 512u * 256u * sizeof(uint32_t));
    /* C3D_DEFAULT_CMDBUF_SIZE (256KB) is sized for typical homebrew scenes
     * (a handful of draws/frame). The GPU tile renderer draws 600-2500+
     * C2D_DrawImage calls per eye, and with the 3D slider on that doubles
     * (both eyes queue into the same one GPU command list before
     * C3D_FrameEnd) plus the bottom-screen composite and debug overlay on
     * top -- once *this* buffer (distinct from citro2d's own vertex buffer,
     * sized separately via C2D_Init below) fills up mid-frame, every GPU
     * command queued after that point is silently dropped for the rest of
     * the frame, with no error. That reads as exactly what turned up
     * testing with the 3D slider on: parts of the right eye missing
     * (queued after the left eye already used a large chunk of the buffer)
     * and the bottom-screen overlay (queued last, after both eyes) entirely
     * absent -- the same "silently drops everything past a fixed capacity"
     * failure mode as the citro2d vertex-buffer exhaustion bug fixed
     * earlier (see the C2D_Init comment below), just a different buffer.
     * 4x default gives real headroom for the worst case (2 eyes x
     * MAX_DRAW_ITEMS-ish); PORT_GPU_RENDERER_DIAG_LOG logs
     * C3D_GetCmdBufUsage() each frame to confirm this empirically instead
     * of guessing at a size. */
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) goto fail_linear;
    /* 128 was sized for this module's own original use case (a couple of
     * big background quads plus the small FPS-text overlay, well under
     * 100 quads/frame). port_gpu_renderer.c's tile/sprite compositor draws
     * 600-1200+ C2D_DrawImage calls in a single scene for a full-screen BG
     * -- once that exceeds citro2d's internal vertex buffer capacity here,
     * everything past the limit silently doesn't render, which reads as
     * "only the first few rows/top of the screen show, the rest is black"
     * regardless of what the scene actually contains. Matches exactly what
     * showed up testing the GPU renderer. 4096 covers the GPU renderer's
     * documented worst case (port_gpu_renderer.c's MAX_DRAW_ITEMS) with
     * headroom. */
    /* Bumped from 4096: that covered one eye's worst case (MAX_DRAW_ITEMS)
     * alone, but with the 3D slider on, both eyes' draws land in this same
     * buffer within one C3D frame before it's fully consumed -- confirmed
     * via a real-hardware screenshot (KEY_X dump, PlatformGpu3DS_DumpScreens)
     * showing the topmost BG layer (menu box, HUD) completely absent from
     * the right eye's target specifically while the same item COUNT was
     * submitted for both eyes (see the EYE0/EYE1 diag log in
     * port_gpu_renderer.c) -- i.e. late-in-the-frame content silently not
     * rendering, the same failure signature as the original citro2d
     * buffer-exhaustion bug (see the comment below), just triggered by
     * cumulative draws across both eyes instead of within a single one. 3x
     * covers two full eyes' worth of MAX_DRAW_ITEMS with headroom. */
    if (!C2D_Init(4096 * 3)) {
        C3D_Fini();
        goto fail_linear;
    }
    C2D_Prepare();
    C3D_BufInfo* c2dBuffers = C3D_GetBufInfo();
    if (c2dBuffers && c2dBuffers->bufCount > 0) {
        const u32 heapPhysical = osConvertVirtToPhys((void*)__ctru_linear_heap);
        const u32 vertexPhysical = c2dBuffers->base_paddr + c2dBuffers->buffers[0].offset;
        const uintptr_t heapStart = (uintptr_t)__ctru_linear_heap;
        const uintptr_t heapEnd = heapStart + __ctru_linear_heap_size;
        const uintptr_t vertexAddress = heapStart + (u32)(vertexPhysical - heapPhysical);
        const uintptr_t flushStart = vertexAddress & ~(uintptr_t)0x7Fu;
        uintptr_t flushEnd = flushStart + 64u * 1024u;
        if (flushEnd > heapEnd) flushEnd = heapEnd;
        if (flushStart >= heapStart && flushStart < flushEnd) {
            sC2dFlushBase = (void*)flushStart;
            sC2dFlushSize = flushEnd - flushStart;
        }
    }
    sStats.linearHeapBytes = __ctru_linear_heap_size;
    sStats.c2dFlushBytes = (uint32_t)sC2dFlushSize;
    sStats.c2dFlushAddress = (uintptr_t)sC2dFlushBase;
    sStats.topUploadAddress = (uintptr_t)sTopUpload;
    sStats.bottomUploadAddress[0] = (uintptr_t)sBottomUploads[0];
    sStats.bottomUploadAddress[1] = (uintptr_t)sBottomUploads[1];
    sTopRightUpload = (uint32_t*)linearMemAlign(TOP_TEXTURE_WIDTH * TOP_TEXTURE_HEIGHT * sizeof(uint32_t), 0x80);
    if (!C3D_TexInitVRAM(&sTopTexture, TOP_TEXTURE_WIDTH, TOP_TEXTURE_HEIGHT, GPU_RGBA8)) goto fail;
    if (!C3D_TexInitVRAM(&sTopRightTexture, TOP_TEXTURE_WIDTH, TOP_TEXTURE_HEIGHT, GPU_RGBA8)) goto fail_top_texture;
    if (!C3D_TexInitVRAM(&sBottomTexture, 512, 256, GPU_RGBA8)) goto fail_top_right_texture;
    C3D_TexSetFilter(&sTopTexture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetFilter(&sTopRightTexture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetFilter(&sBottomTexture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTopTexture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetWrap(&sTopRightTexture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetWrap(&sBottomTexture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    gfxSet3D(true);
    sTopTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    sTopRightTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    sBottomTarget = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!sTopTarget || !sBottomTarget) goto fail_targets;
    const u32 output = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                       GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                       GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                       GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_RenderTargetSetOutput(sTopTarget, GFX_TOP, GFX_LEFT, output);
    if (sTopRightTarget) C3D_RenderTargetSetOutput(sTopRightTarget, GFX_TOP, GFX_RIGHT, output);
    C3D_RenderTargetSetOutput(sBottomTarget, GFX_BOTTOM, GFX_LEFT, output);
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    {
        /* Diagnostic (docs/.../3ds-port-gpu-renderer-status-2026-08-20.md
         * section 20): dump every field of both targets' C3D_FrameBuf/
         * C3D_RenderTarget at creation, side by side, looking for any
         * register-level difference between sTopTarget (never shows the
         * duplication bug) and sBottomTarget (always does). Confirmed on
         * hardware (2026-08-21): both structurally identical aside from
         * expected dims/addresses/screen id -- ruled out a config
         * difference as the cause. */
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "TARGETCMP top: w=%u h=%u colorBuf=%p depthBuf=%p colorFmt=%d linked=%d screen=%d side=%d xfer=%08lx",
                 sTopTarget->frameBuf.width, sTopTarget->frameBuf.height, sTopTarget->frameBuf.colorBuf,
                 sTopTarget->frameBuf.depthBuf, (int)sTopTarget->frameBuf.colorFmt, (int)sTopTarget->linked,
                 (int)sTopTarget->screen, (int)sTopTarget->side, (unsigned long)sTopTarget->transferFlags);
        extern void Port_DebugLog_Gpu(const char* msg);
        Port_DebugLog_Gpu(msg);
        snprintf(msg, sizeof(msg),
                 "TARGETCMP bot: w=%u h=%u colorBuf=%p depthBuf=%p colorFmt=%d linked=%d screen=%d side=%d xfer=%08lx",
                 sBottomTarget->frameBuf.width, sBottomTarget->frameBuf.height, sBottomTarget->frameBuf.colorBuf,
                 sBottomTarget->frameBuf.depthBuf, (int)sBottomTarget->frameBuf.colorFmt, (int)sBottomTarget->linked,
                 (int)sBottomTarget->screen, (int)sBottomTarget->side, (unsigned long)sBottomTarget->transferFlags);
        Port_DebugLog_Gpu(msg);
    }
#endif
    sReady = true;
    return true;

fail_targets:
    if (sBottomTarget) C3D_RenderTargetDelete(sBottomTarget);
    if (sTopRightTarget) C3D_RenderTargetDelete(sTopRightTarget);
    if (sTopTarget) C3D_RenderTargetDelete(sTopTarget);
    C3D_TexDelete(&sBottomTexture);
fail_top_right_texture:
    C3D_TexDelete(&sTopRightTexture);
fail_top_texture:
    C3D_TexDelete(&sTopTexture);
fail:
    C2D_Fini();
    C3D_Fini();
fail_linear:
    if (sBottomUploads[1]) linearFree(sBottomUploads[1]);
    if (sBottomUploads[0]) linearFree(sBottomUploads[0]);
    if (sTopRightUpload) linearFree(sTopRightUpload);
    if (sTopUpload) linearFree(sTopUpload);
    sBottomUploads[0] = NULL;
    sBottomUploads[1] = NULL;
    sTopRightUpload = NULL;
    sTopUpload = NULL;
    return false;
}

float PlatformGpu3DS_Get3DSlider(void) { return osGet3DSliderState(); }
uint32_t* PlatformGpu3DS_TopBuffer(void) { return sTopUpload; }
uint32_t* PlatformGpu3DS_TopRightBuffer(void) { return sTopRightUpload; }
uint32_t* PlatformGpu3DS_BottomBuffer(unsigned index) {
    return index < 2 ? sBottomUploads[index] : NULL;
}

static void DrawTopImageStereo(const uint32_t* leftPixels, const uint32_t* rightPixels, unsigned width) {
    if (width < 240u) width = 240u;
    if (width > 266u) width = 266u;
    sTopPresentWidth = width;

    GSPGPU_FlushDataCache(leftPixels, TOP_TEXTURE_WIDTH * 160u * sizeof(uint32_t));
    const unsigned sourceHeight = 160u;
    C3D_SyncDisplayTransfer((u32*)leftPixels, GX_BUFFER_DIM(TOP_TEXTURE_WIDTH, sourceHeight),
                            (u32*)sTopTexture.data, GX_BUFFER_DIM(TOP_TEXTURE_WIDTH, TOP_TEXTURE_HEIGHT),
                            TextureTransfer());

    const uint32_t* rightSrc = (rightPixels && sTopRightTexture.data) ? rightPixels : leftPixels;
    C3D_Tex* rightTex = (rightPixels && sTopRightTexture.data) ? &sTopRightTexture : &sTopTexture;
    if (rightPixels && rightPixels != leftPixels && sTopRightTexture.data) {
        GSPGPU_FlushDataCache(rightPixels, TOP_TEXTURE_WIDTH * 160u * sizeof(uint32_t));
        C3D_SyncDisplayTransfer((u32*)rightPixels, GX_BUFFER_DIM(TOP_TEXTURE_WIDTH, sourceHeight),
                                (u32*)sTopRightTexture.data, GX_BUFFER_DIM(TOP_TEXTURE_WIDTH, TOP_TEXTURE_HEIGHT),
                                TextureTransfer());
    }

    sTopSubtexture = (Tex3DS_SubTexture){
        .width = (u16)width, .height = 160, .left = 0.0f, .top = 1.0f,
        .right = (float)width / TOP_TEXTURE_WIDTH, .bottom = 1.0f - 160.0f / TOP_TEXTURE_HEIGHT,
    };
    const C2D_Image imageLeft = { .tex = &sTopTexture, .subtex = &sTopSubtexture };
    const C2D_Image imageRight = { .tex = rightTex, .subtex = &sTopSubtexture };
    const int style = Port_Config_Get3DSDisplayStyle();
    C3D_TexSetFilter(&sTopTexture, style == TOP_DISPLAY_BLUR ? GPU_LINEAR : GPU_NEAREST,
                     style == TOP_DISPLAY_BLUR ? GPU_LINEAR : GPU_NEAREST);
    if (sTopRightTexture.data) {
        C3D_TexSetFilter(&sTopRightTexture, style == TOP_DISPLAY_BLUR ? GPU_LINEAR : GPU_NEAREST,
                         style == TOP_DISPLAY_BLUR ? GPU_LINEAR : GPU_NEAREST);
    }

    float drawW;
    float drawH;
    if (style == TOP_DISPLAY_PIXEL_PERFECT) {
        drawW = (float)width;
        drawH = 160.0f;
    } else {
        drawH = 240.0f;
        switch (Port_Config_Get3DSAspectRatio()) {
            case TOP_ASPECT_STRETCH: drawW = 400.0f; break;
            case TOP_ASPECT_ORIGINAL: drawW = 360.0f; break;
            case TOP_ASPECT_WIDE:
            default: drawW = width >= 266u ? 400.0f : 360.0f; break;
        }
    }
    const C2D_DrawParams params = {
        .pos = { .x = (400.0f - drawW) * 0.5f, .y = (240.0f - drawH) * 0.5f,
                 .w = drawW, .h = drawH },
        .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
    };

    C2D_TargetClear(sTopTarget, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(sTopTarget);
    C2D_DrawImage(imageLeft, &params, NULL);
    PlatformGpu3DS_ConfigureAbgrTextureEnv();
    if (Port_Config_GetShowFps()) {
        char label[20];
        double fps = Port_PPU_3DS_CurrentFps();
        unsigned rounded = fps > 0.0 ? (unsigned)(fps + 0.5) : 0u;
        if (rounded > 999u) rounded = 999u;
        snprintf(label, sizeof(label), "FPS %u", rounded);
        C2D_DrawRectSolid(5.0f, 216.0f, 0.7f, 100.0f, 20.0f, C2D_Color32(0, 0, 0, 210));
        PlatformGpu3DS_DrawStatusText(10.0f, 219.0f, 2.0f, label);
    }

    if (sTopRightTarget) {
        C2D_TargetClear(sTopRightTarget, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(sTopRightTarget);
        C2D_DrawImage(imageRight, &params, NULL);
        PlatformGpu3DS_ConfigureAbgrTextureEnv();
        if (Port_Config_GetShowFps()) {
            char label[20];
            double fps = Port_PPU_3DS_CurrentFps();
            unsigned rounded = fps > 0.0 ? (unsigned)(fps + 0.5) : 0u;
            if (rounded > 999u) rounded = 999u;
            snprintf(label, sizeof(label), "FPS %u", rounded);
            C2D_DrawRectSolid(5.0f, 216.0f, 0.7f, 100.0f, 20.0f, C2D_Color32(0, 0, 0, 210));
            PlatformGpu3DS_DrawStatusText(10.0f, 219.0f, 2.0f, label);
        }
    }
}

void PlatformGpu3DS_BeginTop(const uint32_t* pixels, unsigned width) {
    PlatformGpu3DS_BeginTopStereo(pixels, NULL, width);
}

void PlatformGpu3DS_BeginTopStereo(const uint32_t* leftPixels, const uint32_t* rightPixels, unsigned width) {
    if (!sReady || !leftPixels) return;
    if (!C3D_FrameBegin(0)) {
        ++sStats.frameBeginFailures;
        return;
    }
    sFrameActive = true;
    DrawTopImageStereo(leftPixels, rightPixels, width);
    ++sStats.topTransfers;
}

bool PlatformGpu3DS_BeginTopSceneGpu(void) {
    if (!sReady) return false;
    /* Issue #17: tested C3D_FRAME_SYNCDRAW here (forces the CPU to wait for
     * the GPU to finish the previous frame before this frame's CPU-side
     * atlas decode writes start) as a test for a CPU/GPU frame-overlap race
     * on the shared, non-double-buffered atlas texture. Confirmed on real
     * hardware (2026-08-26) NOT to fix the death-scene corruption -- still
     * broken, just slower (dropped FPS game-wide). Reverted to plain
     * C3D_FrameBegin(0). See docs/3ds-issue17-session-2026-08-25.md's final
     * update -- this rules out frame-overlap as the cause. */
    if (!C3D_FrameBegin(0)) {
        ++sStats.frameBeginFailures;
        return false;
    }
    sFrameActive = true;
    ++sStats.topTransfers;
    return true;
}

C3D_RenderTarget* PlatformGpu3DS_GetTopLeftTarget(void) { return sTopTarget; }
C3D_RenderTarget* PlatformGpu3DS_GetTopRightTarget(void) { return sTopRightTarget; }

bool PlatformGpu3DS_EndBottom(const uint32_t* pixels, bool changed) {
    if (!sFrameActive || !pixels) return false;
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    {
        /* Diagnostic (docs/.../3ds-port-gpu-renderer-status-2026-08-20.md
         * section 20 pendiente item 4): log every real EndBottom call
         * unthrottled for the first 120 (~2s at 60fps) so we can see
         * whether the duplication is present from frame 1 or appears
         * later, then throttle to keep the log file from growing forever
         * during a real play session. Confirmed on hardware (2026-08-21):
         * present from frame 1, changed=1 and the "real draw" branch taken
         * on every single call (New3DS never hits EndBottom's own/else
         * skip branch) -- ruled out reentrancy/skip-branch theories. */
        static unsigned sEndBottomCallCount;
        static u64 sFirstCallTick;
        const u64 nowTick = svcGetSystemTick();
        if (sFirstCallTick == 0) sFirstCallTick = nowTick;
        const unsigned n = ++sEndBottomCallCount;
        if (n <= 120 || (n % 60u) == 0u) {
            char msg[128];
            const double msSinceFirst = (double)(nowTick - sFirstCallTick) / (double)CPU_TICKS_PER_MSEC;
            snprintf(msg, sizeof(msg), "ENDBOTTOM n=%u changed=%d valid=%d old3ds=%d t=%.0fms",
                     n, (int)changed, (int)sBottomTargetValid, (int)sOld3DSProfile, msSinceFirst);
            extern void Port_DebugLog_Gpu(const char* msg);
            Port_DebugLog_Gpu(msg);
        }
    }
#endif
    /* `changed` is always true from the callers and is no longer the redraw
     * signal: the bottom UI owns its own cadence now. It advances per-frame
     * state every frame, but the actual rasterise (several thousand
     * immediate-mode quads for a text/map view -- enough to tip the game
     * frame past the vsync budget) only runs on a ~20Hz throttle, or
     * immediately after any interaction. The persistent render target is
     * scanned out unchanged on the frames in between. */
    (void)changed;
    extern void Port_BottomUI_FrameTick(void);
    extern bool Port_BottomUI_WantsRedraw(void);
    Port_BottomUI_FrameTick();
    const bool redrawBottom = Port_BottomUI_WantsRedraw();

    if (redrawBottom || !sBottomTargetValid) {
        C2D_TargetClear(sBottomTarget, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(sBottomTarget);
        PlatformGpu3DS_ResetSolidTexEnv();
        extern void Port_BottomUI_Render(void);
        Port_BottomUI_Render();
        sBottomTargetValid = true;
        ++sStats.bottomTargetDraws;
    } else {
        ++sStats.bottomTargetReuseSkips;
    }
    C2D_Flush();
    if (sC2dFlushBase && sC2dFlushSize) {
        GSPGPU_FlushDataCache(sC2dFlushBase, sC2dFlushSize);
        sStats.boundedFlushBytes += sC2dFlushSize;
    }
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    {
        static unsigned sCmdBufLogCounter;
        if ((sCmdBufLogCounter++ % 30u) == 0u) {
            char msg[64];
            snprintf(msg, sizeof(msg), "CMDBUF usage=%.1f%%", (double)(C3D_GetCmdBufUsage() * 100.0f));
            /* GPU log stream: buffered (per-frame, throttled -- see
             * port_debug_log.h) and only when LOG mode is ALL or GPU. */
            extern void Port_DebugLog_Gpu(const char* msg);
            Port_DebugLog_Gpu(msg);
        }
    }
#endif
    C3D_FrameEnd(0);
    /* Cap presentation to the LCD refresh rate. Without this, nothing paces
     * the main loop to VBlank and it free-runs as fast as the CPU/GPU allow
     * (60-120+ FPS depending on scene load), speeding up game logic and
     * audio with it. Regressed by 1dae106c, which dropped this call thinking
     * it was redundant with C3D_FrameEnd's flags. */
    C3D_FrameSync();
    ++sStats.frames;
    sStats.drawingTime = C3D_GetDrawingTime();
    sStats.processingTime = C3D_GetProcessingTime();
    sFrameActive = false;
    return true;

}

void PlatformGpu3DS_ShowDumpSavedOverlay(void) {
    if (!sReady || !sTopUpload || !sBottomUploads[0] || !C3D_FrameBegin(0)) return;
    sFrameActive = true;
    DrawTopImage(sTopUpload, sTopPresentWidth);
    C2D_DrawRectSolid(132.0f, 12.0f, 0.7f, 136.0f, 24.0f, C2D_Color32(0, 0, 0, 220));
    PlatformGpu3DS_DrawStatusText(141.0f, 17.0f, 2.0f, "DUMP SAVED");

    sBottomSubtexture = (Tex3DS_SubTexture){
        .width = 320, .height = 240, .left = 0.0f, .top = 1.0f,
        .right = 320.0f / 512.0f, .bottom = 1.0f - 240.0f / 256.0f,
    };
    const C2D_Image bottomImage = { .tex = &sBottomTexture, .subtex = &sBottomSubtexture };
    const C2D_DrawParams bottomParams = {
        .pos = { .x = 0.0f, .y = 0.0f, .w = 320.0f, .h = 240.0f },
        .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
    };
    C2D_SceneBegin(sBottomTarget);
    C2D_DrawImage(bottomImage, &bottomParams, NULL);
    PlatformGpu3DS_ConfigureAbgrTextureEnv();
    C2D_Flush();
    if (sC2dFlushBase && sC2dFlushSize) GSPGPU_FlushDataCache(sC2dFlushBase, sC2dFlushSize);
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    sFrameActive = false;
    gspWaitForEvent(GSPGPU_EVENT_VBlank0, false);
    svcSleepThread(600000000LL);
}

void PlatformGpu3DS_GetStats(PlatformGpu3DSStats* stats) {
    if (stats) *stats = sStats;
}

void PlatformGpu3DS_InvalidateBottomTarget(void) {
    /* HOME and lid sleep may invalidate or rotate the physical framebuffer.
     * Force one opaque redraw after APT resumes before Old 3DS starts reusing
     * the unchanged target again. */
    sBottomTargetValid = false;
}

void PlatformGpu3DS_Shutdown(void) {
    if (!sReady) return;
    if (sFrameActive) {
        C2D_Flush();
        if (sC2dFlushBase && sC2dFlushSize) GSPGPU_FlushDataCache(sC2dFlushBase, sC2dFlushSize);
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
    }
    if (!aptShouldClose()) C3D_FrameSync();
    C3D_RenderTargetDelete(sBottomTarget);
    C3D_RenderTargetDelete(sTopTarget);
    C3D_TexDelete(&sBottomTexture);
    C3D_TexDelete(&sTopTexture);
    C2D_Fini();
    C3D_Fini();
    linearFree(sBottomUploads[1]);
    linearFree(sBottomUploads[0]);
    linearFree(sTopUpload);
    sBottomUploads[0] = NULL;
    sBottomUploads[1] = NULL;
    sTopUpload = NULL;
    sC2dFlushBase = NULL;
    sC2dFlushSize = 0;
    sFrameActive = false;
    sReady = false;
    sOld3DSProfile = false;
    sBottomTargetValid = false;
}

/* Diagnostic screenshot: reads back the actual GPU-rendered content of all
 * three render targets (left eye, right eye if stereo is active, bottom)
 * straight from VRAM and writes each as a headerless raw RGB8 file to the
 * SD card -- unlike any CPU-side source buffer, this shows exactly what the
 * PICA200 actually produced, which is the only way to see what's really in
 * the right-eye target for the "menu/floor invisible in right eye with 3D
 * on" bug (docs/3ds-port-gpu-renderer-status-2026-08-20.md is being updated
 * with the investigation). Files are raw top-to-bottom RGB8, no header, at
 * each target's native (portrait, unrotated) dimensions -- rotate 90
 * degrees when viewing. Triggered by KEY_X (see Platform3DS_PollKeysIntoGba
 * in platform_3ds_minimal.c). Not gated behind any diag flag since it does
 * nothing unless the player actually presses X. */
static void DumpOneTarget(C3D_RenderTarget* target, const char* path) {
    if (!target || !target->frameBuf.colorBuf) return;
    const u16 w = target->frameBuf.width;
    const u16 h = target->frameBuf.height;
    const size_t sz = (size_t)w * h * 3u;
    u32* buf = (u32*)linearAlloc(sz);
    if (!buf) return;
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                       GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                       GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_SyncDisplayTransfer((u32*)target->frameBuf.colorBuf, GX_BUFFER_DIM(w, h), buf, GX_BUFFER_DIM(w, h), flags);
    GSPGPU_FlushDataCache(buf, sz);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(buf, 1, sz, f);
        fclose(f);
    }
    linearFree(buf);
}

static void WriteBlob(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, size, f);
    fclose(f);
}

/* Scene recorder: L+R+START (see Platform3DS_PollKeysIntoGba) toggles this
 * on/off. Unlike PlatformGpu3DS_DumpScreens' one-shot dump, this samples the
 * emulated GBA state (VRAM/OAM/palettes/IO + a bit of Samus state) every
 * kRecordEveryNFrames frames and appends each sample to one growing file on
 * the SD card -- built for tracking down issue #17, where a single L+R+X
 * press couldn't catch the exact moment the death animation actually breaks
 * (as opposed to the correct green-flash frames right after it starts).
 * Deliberately skips the RGB screenshots DumpScreens takes (each is ~800KB
 * and requires a GPU display-transfer sync): at 15 samples/sec those would
 * either stall the frame pump or need buffering far bigger than this format,
 * and the VRAM/OAM/palette state alone is enough to reconstruct the visual
 * frame-by-frame offline (already proven doing that for the L+R+X dumps).
 * One sample is ~101KB, so a multi-second recording (say 5s at 15Hz = 75
 * samples, ~7.5MB) is trivial for the SD card; kRecordMaxSamples caps it so
 * forgetting to press the combo again doesn't fill the card.
 *
 * Companion screenshots (issue #17 follow-up): every
 * kRecordScreenshotEverySamples-th sample ALSO gets a real left-eye
 * DumpOneTarget() screenshot (same ~800KB/GPU-sync cost DumpScreens' single
 * shot has), written to its own indexed file rather than folded into
 * mzm-rec.bin's fixed-size records. At 1-in-4 (~4/sec) this is affordable
 * for the few seconds #17's death sequence lasts, unlike doing it every
 * sample (see the no-screenshots reasoning above). This is the only way to
 * tell apart "the emulated state was already wrong" (visible in every
 * sample via the existing VRAM/OAM/palette reconstruction) from "the state
 * was fine but the GPU renderer drew it wrong" (only provable by comparing
 * against what was ACTUALLY on screen at that exact sample). */
static bool sRecording;
static FILE* sRecFile;
static unsigned sRecFrameCounter;
static unsigned sRecSampleCount;
/* Wall-clock ticks of the emulated frame that just ended, measured between
 * consecutive PlatformGpu3DS_RecordTick calls (which Port_Bios_Halt calls
 * exactly once per emulated GBA frame). Recorded per sample so a recording
 * can show WHEN frames overran the 16.7ms budget (issue #20's Skree-explosion
 * hitch) instead of only what was on screen. */
static u64 sRecLastTick;
static u64 sRecLastFrameTicks;

extern u64 Platform3DS_SystemTick(void);
extern u64 Platform3DS_TicksPerSecond(void);
static const unsigned kRecordEveryNFrames = 4; /* ~15Hz at 60fps */
static const unsigned kRecordMaxSamples = 450; /* ~30s at 15Hz */
static const unsigned kRecordScreenshotEverySamples = 4; /* ~1 screenshot/sec */

bool PlatformGpu3DS_IsRecording(void) { return sRecording; }

/* ---- Perf-only frame-time recorder (issue #20) ----------------------
 * The full scene recorder above writes ~100KB/sample to SD (~1.5MB/s
 * sustained), which by itself slows the whole game down -- masking exactly
 * the kind of transient hitch we're hunting. This sibling records ONLY a
 * 16-byte entry per emulated frame into a RAM ring buffer (no SD I/O while
 * running, no screenshots): frame counter, wall-clock duration of that
 * frame in microseconds, and the OAM census (total + affine sprites).
 * L+R+A toggles; on stop the buffer is flushed to sdmc:/3ds/mzm-perf.bin
 * as a flat little-endian array of PerfSample structs.
 * 60 samples/sec * 120s capacity = 7200 entries * 16B = ~115KB BSS. ---- */
typedef struct {
    uint32_t frameCounter;
    uint32_t durationUs;
    uint32_t spriteCount;
    uint32_t affineSpriteCount;
} PerfSample;
static const unsigned kPerfMaxSamples = 7200; /* ~2min at 60fps */
static bool sPerfRecording;
static PerfSample* sPerfSamples;
static unsigned sPerfCount;
static u64 sPerfLastTick;

bool PlatformGpu3DS_IsPerfRecording(void) { return sPerfRecording; }

void PlatformGpu3DS_TogglePerfRecording(void) {
    if (sPerfRecording) {
        extern void Port_DebugLog(const char* msg);
        char msg[80];
        snprintf(msg, sizeof(msg), "PERF REC STOP: %u frames -> mzm-perf.bin", sPerfCount);
        Port_DebugLog(msg);
        FILE* f = fopen("sdmc:/3ds/mzm-perf.bin", "wb");
        if (f && sPerfSamples && sPerfCount) fwrite(sPerfSamples, sizeof(PerfSample), sPerfCount, f);
        if (f) fclose(f);
        linearFree(sPerfSamples);
        sPerfSamples = NULL;
        sPerfCount = 0;
        sPerfRecording = false;
        return;
    }
    if (!sPerfSamples) sPerfSamples = (PerfSample*)linearAlloc(kPerfMaxSamples * sizeof(PerfSample));
    if (!sPerfSamples) {
        extern void Port_DebugLog(const char* msg);
        Port_DebugLog("PERF REC: linearAlloc FAILED (need 115KB linear heap)");
        return;
    }
    sPerfCount = 0;
    sPerfLastTick = 0;
    sPerfRecording = true;
}

void PlatformGpu3DS_PerfRecordTick(void) {
    if (!sPerfRecording || !sPerfSamples) return;
    extern u16 gOamMem[0x400 / 2];

    const u64 now = Platform3DS_SystemTick();
    u64 durTicks = 0;
    if (sPerfLastTick != 0) durTicks = now - sPerfLastTick;
    sPerfLastTick = now;

    if (sPerfCount >= kPerfMaxSamples) {
        PlatformGpu3DS_TogglePerfRecording(); /* flush + stop at capacity */
        return;
    }

    unsigned spriteCount = 0;
    unsigned affineSpriteCount = 0;
    for (unsigned i = 0; i < 128u; ++i) {
        const uint16_t attr0 = gOamMem[i * 4];
        if ((attr0 & 0xC000u) == 0xC000u) continue;
        if ((attr0 & 0x0300u) == 0x0100u) continue;
        ++spriteCount;
        if (attr0 & 0x0100u) ++affineSpriteCount;
    }

    const u64 ticksPerSec = Platform3DS_TicksPerSecond();
    sPerfSamples[sPerfCount++] = (PerfSample){
        .frameCounter = sStats.frames,
        .durationUs = (uint32_t)((durTicks * 1000000ull) / (ticksPerSec ? ticksPerSec : 1)),
        .spriteCount = spriteCount,
        .affineSpriteCount = affineSpriteCount,
    };
}

void PlatformGpu3DS_ToggleRecording(void) {
    if (sRecording) {
        if (sRecFile) { fclose(sRecFile); sRecFile = NULL; }
        sRecording = false;
        return;
    }

    sRecFile = fopen("sdmc:/3ds/mzm-rec.bin", "wb");
    if (!sRecFile) return;
    sRecording = true;
    sRecFrameCounter = 0;
    sRecSampleCount = 0;
    sRecLastTick = 0;
    sRecLastFrameTicks = 0;
}

void PlatformGpu3DS_RecordTick(void) {
    if (!sRecording) return;

    const u64 now = Platform3DS_SystemTick();
    if (sRecLastTick != 0) sRecLastFrameTicks = now - sRecLastTick;
    sRecLastTick = now;

    if (sRecFrameCounter++ % kRecordEveryNFrames != 0) return;

    if (sRecSampleCount >= kRecordMaxSamples) {
        PlatformGpu3DS_ToggleRecording();
        return;
    }
    sRecSampleCount++;

    extern u8 gIoMem[0x400];
    extern u16 gBgPltt[256];
    extern u16 gObjPltt[256];
    extern u16 gOamMem[0x400 / 2];
    extern u8 gVram[0x18000];
    extern void PortPpuMzm_GetSamusRecordState(uint32_t* out);
    extern void PortPpuMzm_GetClipRecordBlock(uint8_t* out);
    extern int PortPpuMzm_GetClipRecordBlockSize(void);

    /* Perf extension (issue #20): header grew from 32 to 64 bytes, magic
     * bumped 'MZMR' -> 'MZM2' so old parsers fail loudly instead of
     * misparsing. New fields: last emulated frame's wall-clock duration in
     * microseconds, citro3d drawing/processing times (hundredths of ms,
     * clamped to u32), and OAM census -- total non-disabled sprites and how
     * many use affine rotation/scaling matrices (the Skree explosion spawns
     * 4 of those at once; the hypothesis is that they're what blows the
     * frame budget). */
    unsigned spriteCount = 0;
    unsigned affineSpriteCount = 0;
    for (unsigned i = 0; i < 128u; ++i) {
        const uint16_t attr0 = gOamMem[i * 4];
        /* GBA disabled check: shape bits 15-14 = 3, OR bit8=0 bit9=1.
         * NOT 0x0300 (bits 8-9 = 11) — that's affine double-size,
         * used by e.g. Skree explosions. (This was the old bug: counting
         * affine-double-size sprites as disabled → affine=0 always.) */
        if ((attr0 & 0xC000u) == 0xC000u) continue;
        if ((attr0 & 0x0300u) == 0x0100u) continue;
        ++spriteCount;
        if (attr0 & 0x0100u) ++affineSpriteCount;
    }

    const u64 ticksPerSec = Platform3DS_TicksPerSecond();
    const uint32_t frameUs =
        (uint32_t)((sRecLastFrameTicks * 1000000ull) / (ticksPerSec ? ticksPerSec : 1));
    PlatformGpu3DSStats stats;
    PlatformGpu3DS_GetStats(&stats);
    uint32_t drawX100 = (uint32_t)(stats.drawingTime * 100.0f);
    uint32_t procX100 = (uint32_t)(stats.processingTime * 100.0f);
    /* citro3d can report negative/absurd values outside an active frame;
     * clamp so the recorded u32 stays sane for offline parsing. */
    if (stats.drawingTime < 0.0f) drawX100 = 0;
    if (stats.processingTime < 0.0f) procX100 = 0;

    uint32_t header[2 + 6 + 8]; /* magic, frame counter, 6 Samus words, perf */
    /* 'MZM4': the clip/camera block after VRAM now also carries the area
     * and room number, which a tile correction has to be keyed to. Magic
     * bumped, as with 'MZM3' before it, so an older parser fails loudly
     * instead of walking off the end of every sample. */
    header[0] = 0x344D5A4Du;
    header[1] = sRecFrameCounter;
    PortPpuMzm_GetSamusRecordState(&header[2]);
    header[8]  = frameUs;
    header[9]  = drawX100;
    header[10] = procX100;
    header[11] = spriteCount;
    header[12] = affineSpriteCount;
    header[13] = 0; /* reserved */
    header[14] = 0; /* reserved */
    header[15] = 0; /* reserved */

    fwrite(header, sizeof(header), 1, sRecFile);
    fwrite(gIoMem, 1, sizeof(gIoMem), sRecFile);
    fwrite(gBgPltt, 1, sizeof(gBgPltt), sRecFile);
    fwrite(gObjPltt, 1, sizeof(gObjPltt), sRecFile);
    fwrite(gOamMem, 1, sizeof(gOamMem), sRecFile);
    fwrite(gVram, 1, sizeof(gVram), sRecFile);

    /* Clip/camera block -- see PortPpuMzm_GetClipRecordBlock. Small (220
     * bytes against the sample's ~99KB), so it costs nothing next to VRAM. */
    {
        uint8_t clipBlock[256];
        int clipSize = PortPpuMzm_GetClipRecordBlockSize();
        if (clipSize > (int)sizeof(clipBlock)) clipSize = (int)sizeof(clipBlock);
        memset(clipBlock, 0, sizeof(clipBlock));
        PortPpuMzm_GetClipRecordBlock(clipBlock);
        fwrite(clipBlock, 1, (size_t)clipSize, sRecFile);
    }

    /* sRecSampleCount was already incremented above, so sample #1 (the
     * first one written this recording) always gets a screenshot too. File
     * name embeds the sample index so it lines up with mzm-rec.bin's Nth
     * record (0-indexed, matching the Python splitting snippet in
     * docs/3ds-debug-tools.md) without needing to also parse frameCounter. */
    if ((sRecSampleCount - 1) % kRecordScreenshotEverySamples == 0) {
        char path[64];
        snprintf(path, sizeof(path), "sdmc:/3ds/mzm-rec-shot-%04u.rgb", sRecSampleCount - 1);
        DumpOneTarget(sTopTarget, path);
    }
}

void PlatformGpu3DS_DumpScreens(void) {
    if (!sReady) return;
    DumpOneTarget(sTopTarget, "sdmc:/3ds/mzm-dump-left.rgb");
    DumpOneTarget(sTopRightTarget, "sdmc:/3ds/mzm-dump-right.rgb");
    DumpOneTarget(sBottomTarget, "sdmc:/3ds/mzm-dump-bottom.rgb");

    extern u8 gIoMem[0x400];
    extern u16 gBgPltt[256];
    extern u16 gObjPltt[256];
    extern u16 gOamMem[0x400 / 2];
    extern u8 gVram[0x18000];
    WriteBlob("sdmc:/3ds/mzm-dump-vram.bin", gVram, sizeof(gVram));
    WriteBlob("sdmc:/3ds/mzm-dump-io.bin", gIoMem, sizeof(gIoMem));
    WriteBlob("sdmc:/3ds/mzm-dump-bgpltt.bin", gBgPltt, sizeof(gBgPltt));
    WriteBlob("sdmc:/3ds/mzm-dump-objpltt.bin", gObjPltt, sizeof(gObjPltt));
    WriteBlob("sdmc:/3ds/mzm-dump-oam.bin", gOamMem, sizeof(gOamMem));

    PortPpuMzm_DumpSamusState();

    uint16_t dispcnt = (uint16_t)(gIoMem[0x00] | (gIoMem[0x01] << 8));
    uint16_t bg0cnt  = (uint16_t)(gIoMem[0x08] | (gIoMem[0x09] << 8));
    uint16_t bg1cnt  = (uint16_t)(gIoMem[0x0A] | (gIoMem[0x0B] << 8));
    uint16_t bg2cnt  = (uint16_t)(gIoMem[0x0C] | (gIoMem[0x0D] << 8));
    uint16_t bg3cnt  = (uint16_t)(gIoMem[0x0E] | (gIoMem[0x0F] << 8));
    uint16_t bldcnt  = (uint16_t)(gIoMem[0x50] | (gIoMem[0x51] << 8));

    char msg[256];
    snprintf(msg, sizeof(msg),
        "DUMP: DISPCNT=%04X BG0CNT=%04X(prio=%u) BG1CNT=%04X(prio=%u) BG2CNT=%04X(prio=%u) BG3CNT=%04X(prio=%u) BLDCNT=%04X",
        dispcnt, bg0cnt, bg0cnt & 3, bg1cnt, bg1cnt & 3, bg2cnt, bg2cnt & 3, bg3cnt, bg3cnt & 3, bldcnt);
    extern void Port_DebugLog(const char* msg);
    Port_DebugLog(msg);

    /* Flush any buffered Port_DebugLogBuffered() lines (e.g. kraid.c's
     * KraidSync/KraidPrimary diagnostics) now, so whatever led up to this
     * L+R+X capture is on disk instead of sitting in RAM. */
    extern void Port_DebugLogFlush(void);
    Port_DebugLogFlush();
}

