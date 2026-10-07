#include "port_state_thumb.h"
#include "platform_gpu_3ds.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#define THUMB_TEX_W 256
#define THUMB_TEX_H 128

static C3D_Tex sThumbTex;
static bool sThumbTexReady = false;
static bool sThumbHasImage = false;
static Tex3DS_SubTexture sThumbSub;

bool PortStateThumb_Capture(uint16_t* out) {
    C3D_RenderTarget* target = PlatformGpu3DS_GetTopLeftTarget();
    if (!out || !target || !target->frameBuf.colorBuf) return false;

    /* The target is the screen rotated: 240 x 400, one column of the screen
     * per line, bottom pixel first. Read it back unscaled (the transfer the
     * debug screenshot already uses; a scaling transfer left the lines at an
     * unexpected stride) and halve it on the CPU, turning it upright. */
    const u16 srcW = target->frameBuf.width;    /* 240: screen rows */
    const u16 srcH = target->frameBuf.height;   /* 400: screen columns */
    if (srcW != 2u * PORT_THUMB_H || srcH != 2u * PORT_THUMB_W) return false;

    const size_t bytes = (size_t)srcW * srcH * sizeof(u16);
    u16* tmp = (u16*)linearAlloc(bytes);
    if (!tmp) return false;

    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                      GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                      GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_SyncDisplayTransfer((u32*)target->frameBuf.colorBuf, GX_BUFFER_DIM(srcW, srcH),
                            (u32*)tmp, GX_BUFFER_DIM(srcW, srcH), flags);
    GSPGPU_FlushDataCache(tmp, bytes);

#ifdef PORT_DEBUG_TOOLS
    /* The raw readback, for checking the layout assumed below (debug builds). */
    FILE* raw = fopen("sdmc:/3ds/Metroid Zero Mission 3DS/debug/state-thumb-raw.bin", "wb");
    if (raw) { fwrite(tmp, 1, bytes, raw); fclose(raw); }
#endif

    /* Screen pixel (sx, sy), sy from the top, lives at tmp[sx * srcW + (srcW - 1 - sy)].
     * Each thumbnail pixel averages the 2x2 block it covers. */
    for (int y = 0; y < PORT_THUMB_H; ++y) {
        for (int x = 0; x < PORT_THUMB_W; ++x) {
            unsigned r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const u16 p = tmp[(size_t)(2 * x + dx) * srcW + (srcW - 1 - (2 * y + dy))];
                    r += (p >> 11) & 0x1Fu;
                    g += (p >> 5) & 0x3Fu;
                    b += p & 0x1Fu;
                }
            }
            out[y * PORT_THUMB_W + x] = (u16)(((r / 4u) << 11) | ((g / 4u) << 5) | (b / 4u));
        }
    }

    linearFree(tmp);
    return true;
}

static bool EnsureTexture(void) {
    if (sThumbTexReady) return true;
    if (!C3D_TexInit(&sThumbTex, THUMB_TEX_W, THUMB_TEX_H, GPU_RGBA8)) return false;
    C3D_TexSetFilter(&sThumbTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sThumbTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sThumbSub.width = PORT_THUMB_W;
    sThumbSub.height = PORT_THUMB_H;
    sThumbSub.left = 0.0f;
    sThumbSub.right = (float)PORT_THUMB_W / (float)THUMB_TEX_W;
    sThumbSub.top = 1.0f;
    sThumbSub.bottom = 1.0f - (float)PORT_THUMB_H / (float)THUMB_TEX_H;
    sThumbTexReady = true;
    return true;
}

void PortStateThumb_Set(const uint16_t* pixels) {
    if (!pixels) { sThumbHasImage = false; return; }
    if (!EnsureTexture()) { sThumbHasImage = false; return; }

    const size_t bytes = (size_t)THUMB_TEX_W * THUMB_TEX_H * sizeof(u32);
    u32* lin = (u32*)linearAlloc(bytes);
    if (!lin) { sThumbHasImage = false; return; }
    memset(lin, 0, bytes);

    for (int y = 0; y < PORT_THUMB_H; ++y) {
        for (int x = 0; x < PORT_THUMB_W; ++x) {
            const u16 p = pixels[y * PORT_THUMB_W + x];
            const u32 r = ((p >> 11) & 0x1Fu) * 255u / 31u;
            const u32 g = ((p >> 5) & 0x3Fu) * 255u / 63u;
            const u32 b = (p & 0x1Fu) * 255u / 31u;
            lin[y * THUMB_TEX_W + x] = (r << 24) | (g << 16) | (b << 8) | 0xFFu;
        }
    }
    GSPGPU_FlushDataCache(lin, bytes);
    C3D_SyncDisplayTransfer(lin, GX_BUFFER_DIM(THUMB_TEX_W, THUMB_TEX_H),
                            (u32*)sThumbTex.data, GX_BUFFER_DIM(THUMB_TEX_W, THUMB_TEX_H),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
                            GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    linearFree(lin);
    sThumbHasImage = true;
}

bool PortStateThumb_HasImage(void) { return sThumbHasImage; }

void PortStateThumb_Draw(float x, float y, float w, float h, float depth) {
    if (!sThumbHasImage || !sThumbTexReady) return;
    /* Same bracket the bezel uses: solid draws leave TEV units 1..5 in a state
     * a textured draw must not inherit, and vice versa. */
    C2D_Flush();
    PlatformGpu3DS_ResetSolidTexEnv();
    const C2D_Image img = { .tex = &sThumbTex, .subtex = &sThumbSub };
    const C2D_DrawParams params = {
        .pos = { x, y, w, h },
        .center = { 0.0f, 0.0f },
        .depth = depth,
        .angle = 0.0f,
    };
    C2D_DrawImage(img, &params, NULL);
    C2D_Flush();
    PlatformGpu3DS_ResetSolidTexEnv();
}
