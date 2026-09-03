#include "port_gba_bezel.h"
#include "platform_gpu_3ds.h"

#include <citro2d.h>
#include <citro3d.h>
#include <string.h>

extern const size_t gGbaBezelCompressedSize;
extern const size_t gGbaBezelUncompressedSize;
extern const uint8_t gGbaBezelCompressedData[];

extern bool Port_Config_GetGbaBezel(void);
extern int Port_Config_Get3DSDisplayStyle(void);

#define BEZEL_TEX_W 512
#define BEZEL_TEX_H 256
#define BEZEL_SCR_W 400
#define BEZEL_SCR_H 240

static C3D_Tex sBezelTex;
static Tex3DS_SubTexture sBezelSub;
static bool sBezelReady;

/* Simple in-place standard LZ4 block decompressor with zero external dependencies. */
static bool Lz4Decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len) {
    size_t src_idx = 0;
    size_t dst_idx = 0;

    while (dst_idx < dst_len && src_idx < src_len) {
        uint8_t token = src[src_idx++];
        size_t lit_len = token >> 4;
        if (lit_len == 15) {
            while (src_idx < src_len) {
                uint8_t s = src[src_idx++];
                lit_len += s;
                if (s != 255) break;
            }
        }
        if (src_idx + lit_len > src_len || dst_idx + lit_len > dst_len) return false;
        memcpy(dst + dst_idx, src + src_idx, lit_len);
        dst_idx += lit_len;
        src_idx += lit_len;
        if (dst_idx >= dst_len) break;

        if (src_idx + 2 > src_len) return false;
        size_t offset = (size_t)src[src_idx] | ((size_t)src[src_idx + 1] << 8);
        src_idx += 2;
        if (offset == 0 || offset > dst_idx) return false;

        size_t match_len = (token & 0x0F) + 4;
        if (match_len == 19) {
            while (src_idx < src_len) {
                uint8_t s = src[src_idx++];
                match_len += s;
                if (s != 255) break;
            }
        }
        if (dst_idx + match_len > dst_len) return false;

        for (size_t i = 0; i < match_len; ++i) {
            dst[dst_idx] = dst[dst_idx - offset];
            dst_idx++;
        }
    }

    return dst_idx == dst_len;
}

void PortGbaBezel_Init(void) {
    if (sBezelReady) return;

    if (!C3D_TexInit(&sBezelTex, BEZEL_TEX_W, BEZEL_TEX_H, GPU_RGBA8)) {
        sBezelReady = false;
        return;
    }

    if (!Lz4Decompress(gGbaBezelCompressedData, gGbaBezelCompressedSize,
                       (uint8_t*)sBezelTex.data, gGbaBezelUncompressedSize)) {
        C3D_TexDelete(&sBezelTex);
        sBezelReady = false;
        return;
    }

    GSPGPU_FlushDataCache(sBezelTex.data, gGbaBezelUncompressedSize);
    C3D_TexSetFilter(&sBezelTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sBezelTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    sBezelSub.width = BEZEL_SCR_W;
    sBezelSub.height = BEZEL_SCR_H;
    sBezelSub.left = 0.0f;
    sBezelSub.top = 1.0f;
    sBezelSub.right = (float)BEZEL_SCR_W / (float)BEZEL_TEX_W;
    sBezelSub.bottom = 1.0f - (float)BEZEL_SCR_H / (float)BEZEL_TEX_H;

    sBezelReady = true;
}

void PortGbaBezel_Shutdown(void) {
    if (!sBezelReady) return;
    C3D_TexDelete(&sBezelTex);
    sBezelReady = false;
}

bool PortGbaBezel_Ready(void) {
    return sBezelReady;
}

bool PortGbaBezel_Active(void) {
    /* Bezel is enabled when the user setting is ON and the display style is Pixel Perfect. */
    return sBezelReady && Port_Config_GetGbaBezel() && (Port_Config_Get3DSDisplayStyle() == 0);
}

void PortGbaBezel_Draw(C3D_RenderTarget* target) {
    if (!sBezelReady) return;

    C2D_SceneBegin(target);
    PlatformGpu3DS_ResetSolidTexEnv();

    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    /* Discard transparent pixels (the 240x160 viewport in the middle). */
    C3D_AlphaTest(true, GPU_GREATER, 0);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    const C2D_Image img = { .tex = &sBezelTex, .subtex = &sBezelSub };
    const C2D_DrawParams params = {
        .pos = { 0.0f, 0.0f, (float)BEZEL_SCR_W, (float)BEZEL_SCR_H },
        .center = { 0.0f, 0.0f },
        .depth = 0.65f,
        .angle = 0.0f,
    };
    C2D_DrawImage(img, &params, NULL);
    C2D_Flush();

    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    PlatformGpu3DS_ResetSolidTexEnv();
}
