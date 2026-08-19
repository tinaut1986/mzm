#include "port_gpu_renderer.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <string.h>
#include <stdio.h>

/* Emulated GBA memory buffers */
extern uint8_t gIoMem[];
extern uint8_t gVram[];
extern uint8_t gBgPltt[];
extern uint8_t gObjPltt[];
extern uint8_t gOamMem[];

static bool sGpuRendererActive = true;
static bool sInitialized = false;

static C3D_RenderTarget* sTopLeftTarget = NULL;
static C3D_RenderTarget* sTopRightTarget = NULL;
static C3D_RenderTarget* sBottomTarget = NULL;

static C3D_Tex sBgTileTexture;
static C3D_Tex sObjTileTexture;

static u32* sBgTilePixels = NULL;
static u32* sObjTilePixels = NULL;

enum {
    BG_TEX_WIDTH = 256,
    BG_TEX_HEIGHT = 256,
    OBJ_TEX_WIDTH = 256,
    OBJ_TEX_HEIGHT = 256,
};

static inline u32 Bgr555ToRgba8(uint16_t color, uint8_t alpha) {
    u32 r = (color & 0x1F);
    u32 g = ((color >> 5) & 0x1F);
    u32 b = ((color >> 10) & 0x1F);
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (alpha << 24) | (b << 16) | (g << 8) | r;
}

bool Port_GpuRenderer_IsActive(void) {
    return sGpuRendererActive;
}

void Port_GpuRenderer_SetActive(bool active) {
    sGpuRendererActive = active;
}

bool Port_GpuRenderer_Init(void) {
    if (sInitialized) return true;

    /* Allocate linear buffers for dynamic tile decoding */
    sBgTilePixels = (u32*)linearMemAlign(BG_TEX_WIDTH * BG_TEX_HEIGHT * sizeof(u32), 0x80);
    sObjTilePixels = (u32*)linearMemAlign(OBJ_TEX_WIDTH * OBJ_TEX_HEIGHT * sizeof(u32), 0x80);

    if (!sBgTilePixels || !sObjTilePixels) {
        if (sBgTilePixels) linearFree(sBgTilePixels);
        if (sObjTilePixels) linearFree(sObjTilePixels);
        return false;
    }

    memset(sBgTilePixels, 0, BG_TEX_WIDTH * BG_TEX_HEIGHT * sizeof(u32));
    memset(sObjTilePixels, 0, OBJ_TEX_WIDTH * OBJ_TEX_HEIGHT * sizeof(u32));

    /* Initialize GPU textures in VRAM */
    if (!C3D_TexInitVRAM(&sBgTileTexture, BG_TEX_WIDTH, BG_TEX_HEIGHT, GPU_RGBA8)) {
        linearFree(sBgTilePixels);
        linearFree(sObjTilePixels);
        return false;
    }

    if (!C3D_TexInitVRAM(&sObjTileTexture, OBJ_TEX_WIDTH, OBJ_TEX_HEIGHT, GPU_RGBA8)) {
        C3D_TexDelete(&sBgTileTexture);
        linearFree(sBgTilePixels);
        linearFree(sObjTilePixels);
        return false;
    }

    C3D_TexSetFilter(&sBgTileTexture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetFilter(&sObjTileTexture, GPU_NEAREST, GPU_NEAREST);

    /* Set up stereoscopic 3D render targets for top screen */
    gfxSet3D(true);
    const u32 outputTransfer = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                               GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                               GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                               GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

    sTopLeftTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    sTopRightTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    sBottomTarget = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH16);

    if (sTopLeftTarget) C3D_RenderTargetSetOutput(sTopLeftTarget, GFX_TOP, GFX_LEFT, outputTransfer);
    if (sTopRightTarget) C3D_RenderTargetSetOutput(sTopRightTarget, GFX_TOP, GFX_RIGHT, outputTransfer);
    if (sBottomTarget) C3D_RenderTargetSetOutput(sBottomTarget, GFX_BOTTOM, GFX_LEFT, outputTransfer);

    sInitialized = true;
    sGpuRendererActive = true;
    return true;
}

/* Upload decoded BG tileset to GPU VRAM */
static void UpdateBgTilesTexture(uint8_t charBlock, uint8_t paletteBank) {
    const uint8_t* tileData = gVram + (charBlock * 0x4000);
    const uint16_t* pal = (const uint16_t*)gBgPltt;

    for (int tile = 0; tile < 1024; ++tile) {
        int tileX = (tile % 32) * 8;
        int tileY = (tile / 32) * 8;
        const uint8_t* src = tileData + tile * 32;

        for (int py = 0; py < 8; ++py) {
            u32* dst = sBgTilePixels + (tileY + py) * BG_TEX_WIDTH + tileX;
            for (int px = 0; px < 8; px += 2) {
                uint8_t byte = *src++;
                uint8_t c0 = byte & 0x0F;
                uint8_t c1 = (byte >> 4) & 0x0F;

                dst[px] = c0 ? Bgr555ToRgba8(pal[paletteBank * 16 + c0], 255) : 0;
                dst[px + 1] = c1 ? Bgr555ToRgba8(pal[paletteBank * 16 + c1], 255) : 0;
            }
        }
    }

    GSPGPU_FlushDataCache(sBgTilePixels, BG_TEX_WIDTH * BG_TEX_HEIGHT * sizeof(u32));
    C3D_SyncDisplayTransfer((u32*)sBgTilePixels, GX_BUFFER_DIM(BG_TEX_WIDTH, BG_TEX_HEIGHT),
                            (u32*)sBgTileTexture.data, GX_BUFFER_DIM(BG_TEX_WIDTH, BG_TEX_HEIGHT),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
                            GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

/* Upload decoded OBJ sprites to GPU VRAM */
static void UpdateObjTilesTexture(void) {
    const uint8_t* tileData = gVram + 0x10000;
    const uint16_t* pal = (const uint16_t*)gObjPltt;

    for (int tile = 0; tile < 1024; ++tile) {
        int tileX = (tile % 32) * 8;
        int tileY = (tile / 32) * 8;
        const uint8_t* src = tileData + tile * 32;

        for (int py = 0; py < 8; ++py) {
            u32* dst = sObjTilePixels + (tileY + py) * OBJ_TEX_WIDTH + tileX;
            for (int px = 0; px < 8; px += 2) {
                uint8_t byte = *src++;
                uint8_t c0 = byte & 0x0F;
                uint8_t c1 = (byte >> 4) & 0x0F;

                dst[px] = c0 ? Bgr555ToRgba8(pal[c0], 255) : 0;
                dst[px + 1] = c1 ? Bgr555ToRgba8(pal[c1], 255) : 0;
            }
        }
    }

    GSPGPU_FlushDataCache(sObjTilePixels, OBJ_TEX_WIDTH * OBJ_TEX_HEIGHT * sizeof(u32));
    C3D_SyncDisplayTransfer((u32*)sObjTilePixels, GX_BUFFER_DIM(OBJ_TEX_WIDTH, OBJ_TEX_HEIGHT),
                            (u32*)sObjTileTexture.data, GX_BUFFER_DIM(OBJ_TEX_WIDTH, OBJ_TEX_HEIGHT),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
                            GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

static void RenderTilemapLayer(int bgIndex, float eyeOffset, float depthZ) {
    uint16_t bgCnt = *(const uint16_t*)(gIoMem + 0x08 + bgIndex * 2);
    uint16_t hOfs = *(const uint16_t*)(gIoMem + 0x10 + bgIndex * 4) & 0x1FF;
    uint16_t vOfs = *(const uint16_t*)(gIoMem + 0x12 + bgIndex * 4) & 0x1FF;

    uint8_t charBase = (bgCnt >> 2) & 3;
    uint8_t screenBase = (bgCnt >> 8) & 0x1F;

    UpdateBgTilesTexture(charBase, 0);

    const uint16_t* map = (const uint16_t*)(gVram + screenBase * 0x800);

    float screenBaseX = (400.0f - 240.0f * 1.5f) * 0.5f + eyeOffset;
    float screenBaseY = 0.0f;
    float scale = 1.5f;

    /* Render visible 32x22 tiles for the GBA screen */
    int startTileX = hOfs / 8;
    int startTileY = vOfs / 8;
    int fineX = hOfs % 8;
    int fineY = vOfs % 8;

    for (int ty = 0; ty < 22; ++ty) {
        int my = (startTileY + ty) % 32;
        for (int tx = 0; tx < 32; ++tx) {
            int mx = (startTileX + tx) % 32;
            uint16_t entry = map[my * 32 + mx];
            uint16_t tileId = entry & 0x3FF;

            float u0 = (float)((tileId % 32) * 8) / (float)BG_TEX_WIDTH;
            float v0 = 1.0f - (float)((tileId / 32) * 8) / (float)BG_TEX_HEIGHT;
            float u1 = u0 + 8.0f / (float)BG_TEX_WIDTH;
            float v1 = v0 - 8.0f / (float)BG_TEX_HEIGHT;

            Tex3DS_SubTexture subtex = {
                .width = 8, .height = 8,
                .left = u0, .top = v0,
                .right = u1, .bottom = v1
            };

            C2D_Image img = { .tex = &sBgTileTexture, .subtex = &subtex };
            float drawX = screenBaseX + (tx * 8 - fineX) * scale;
            float drawY = screenBaseY + (ty * 8 - fineY) * scale;

            C2D_DrawParams params = {
                .pos = { .x = drawX, .y = drawY, .w = 8.0f * scale, .h = 8.0f * scale },
                .center = { 0.0f, 0.0f },
                .depth = depthZ,
                .angle = 0.0f
            };

            C2D_DrawImage(img, &params, NULL);
        }
    }
}

static void RenderSprites(float eyeOffset, float depthZ) {
    UpdateObjTilesTexture();

    const uint16_t* oam = (const uint16_t*)gOamMem;
    float screenBaseX = (400.0f - 240.0f * 1.5f) * 0.5f + eyeOffset;
    float screenBaseY = 0.0f;
    float scale = 1.5f;

    for (int i = 0; i < 128; ++i) {
        uint16_t attr0 = oam[i * 4 + 0];
        uint16_t attr1 = oam[i * 4 + 1];
        uint16_t attr2 = oam[i * 4 + 2];

        /* Check if sprite is disabled */
        if ((attr0 & (1 << 9)) && !(attr0 & (1 << 8))) continue;

        int y = attr0 & 0xFF;
        int x = attr1 & 0x1FF;
        if (x >= 240) x -= 512;
        if (y >= 160) y -= 256;

        uint16_t tileId = attr2 & 0x3FF;

        float u0 = (float)((tileId % 32) * 8) / (float)OBJ_TEX_WIDTH;
        float v0 = 1.0f - (float)((tileId / 32) * 8) / (float)OBJ_TEX_HEIGHT;
        float u1 = u0 + 16.0f / (float)OBJ_TEX_WIDTH;
        float v1 = v0 - 16.0f / (float)OBJ_TEX_HEIGHT;

        Tex3DS_SubTexture subtex = {
            .width = 16, .height = 16,
            .left = u0, .top = v0,
            .right = u1, .bottom = v1
        };

        C2D_Image img = { .tex = &sObjTileTexture, .subtex = &subtex };
        C2D_DrawParams params = {
            .pos = { .x = screenBaseX + x * scale, .y = screenBaseY + y * scale,
                     .w = 16.0f * scale, .h = 16.0f * scale },
            .center = { 0.0f, 0.0f },
            .depth = depthZ,
            .angle = 0.0f
        };

        C2D_DrawImage(img, &params, NULL);
    }
}

static void RenderSceneForEye(C3D_RenderTarget* target, float eyeOffset) {
    if (!target) return;

    C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(target);

    uint16_t dispcnt = *(const uint16_t*)(gIoMem + 0x00);

    /* BG3: Distant background (Z = -4.0, parallax factor = 3.0) */
    if (dispcnt & (1 << 11)) {
        RenderTilemapLayer(3, eyeOffset * -3.0f, 0.1f);
    }

    /* BG2: Mid background (Z = -2.0, parallax factor = 1.5) */
    if (dispcnt & (1 << 10)) {
        RenderTilemapLayer(2, eyeOffset * -1.5f, 0.3f);
    }

    /* BG1: Foreground / platforms (Z = -0.5, parallax factor = 0.5) */
    if (dispcnt & (1 << 9)) {
        RenderTilemapLayer(1, eyeOffset * -0.5f, 0.5f);
    }

    /* Sprites: Samus, enemies, projectiles (Z = -0.5) */
    if (dispcnt & (1 << 12)) {
        RenderSprites(eyeOffset * -0.5f, 0.6f);
    }

    /* BG0: Foreground overlay / HUD (Z = 0.0, no parallax) */
    if (dispcnt & (1 << 8)) {
        RenderTilemapLayer(0, 0.0f, 0.8f);
    }
}

void Port_GpuRenderer_RenderFrame(void) {
    if (!sInitialized || !sGpuRendererActive) return;

    if (!C3D_FrameBegin(0)) return;

    float slider3d = osGet3DSliderState();
    float eyeDistance = slider3d * 5.0f; /* 3D parallax intensity */

    /* Render Left Eye */
    RenderSceneForEye(sTopLeftTarget, -eyeDistance);

    /* Render Right Eye */
    if (slider3d > 0.01f && sTopRightTarget) {
        RenderSceneForEye(sTopRightTarget, eyeDistance);
    }

    /* Clear bottom screen */
    if (sBottomTarget) {
        C2D_TargetClear(sBottomTarget, C2D_Color32(0, 0, 0, 255));
    }

    C3D_FrameEnd(0);
}

void Port_GpuRenderer_Shutdown(void) {
    if (!sInitialized) return;

    if (sTopLeftTarget) C3D_RenderTargetDelete(sTopLeftTarget);
    if (sTopRightTarget) C3D_RenderTargetDelete(sTopRightTarget);
    if (sBottomTarget) C3D_RenderTargetDelete(sBottomTarget);

    C3D_TexDelete(&sBgTileTexture);
    C3D_TexDelete(&sObjTileTexture);

    if (sBgTilePixels) linearFree(sBgTilePixels);
    if (sObjTilePixels) linearFree(sObjTilePixels);

    sBgTilePixels = NULL;
    sObjTilePixels = NULL;
    sInitialized = false;
    sGpuRendererActive = false;
}
