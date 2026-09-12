/*
 * Affine subtile probe -- standalone .3dsx.
 *
 * Renders the 64x64 Tourian-escape ship sprite (ship_sprite.h) the way
 * port_gpu_renderer.c renders an affine OBJ: one C2D quad per 8x8 subtile,
 * placed/rotated through port_affine_subtile.h (the SAME header the renderer
 * uses). That ship is PURE SCALE in game (PB=PC=0, ~0.12x..2x zoom, no
 * rotation) -- so its subtiles staying upright is correct; the artefact is
 * scale seams.
 *
 * Modes (A cycles):
 *   NO BLEED  raw per-subtile snap  -> transparent seams
 *   BLEED     grow interior edges, same UV -> seams gone but edges stretch
 *   SNAP      shared-edge snap (no overlap, no stretch) -- the non-affine
 *             path's trick; exact for angle 0, falls back to BLEED if rotated
 *   ONE QUAD  whole sprite, one quad -- reference
 *
 * Controls:
 *   Circle pad X / Y   rotate / scale      D-pad L/R,U/D  nudge
 *   L / R              bleed px - / +      (0, 0.5, 1, 1.5, 2)
 *   A / B  mode next / prev    Y  checker tint    X  reset
 *   SELECT  screenshot -> SD          START  exit
 */

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_affine_subtile.h"
#include "ship_sprite.h"

#define SPR 64
#define TILES (SPR / 8)
#define TOP_W 400
#define TOP_H 240

static const u8 kSwizzle[64] = {
     0,  1,  4,  5, 16, 17, 20, 21,   2,  3,  6,  7, 18, 19, 22, 23,
     8,  9, 12, 13, 24, 25, 28, 29,  10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,  34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,  42, 43, 46, 47, 58, 59, 62, 63,
};

static C3D_Tex sTex;
static Tex3DS_SubTexture sSub[TILES][TILES];
static Tex3DS_SubTexture sFull;

static void BuildTexture(void) {
    C3D_TexInit(&sTex, SPR, SPR, GPU_RGBA8);
    C3D_TexSetFilter(&sTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    u32* dst = (u32*)sTex.data;
    for (int y = 0; y < SPR; ++y)
        for (int x = 0; x < SPR; ++x) {
            u32 t = (y / 8) * (SPR / 8) + (x / 8);
            dst[t * 64 + kSwizzle[(y % 8) * 8 + (x % 8)]] = kShipSprite[y * SPR + x];
        }
    GSPGPU_FlushDataCache(sTex.data, SPR * SPR * sizeof(u32));
    for (int ty = 0; ty < TILES; ++ty)
        for (int tx = 0; tx < TILES; ++tx) {
            sSub[ty][tx] = (Tex3DS_SubTexture){ 8, 8,
                (tx * 8) / (float)SPR, 1.0f - (ty * 8) / (float)SPR,
                (tx * 8 + 8) / (float)SPR, 1.0f - (ty * 8 + 8) / (float)SPR };
        }
    sFull = (Tex3DS_SubTexture){ SPR, SPR, 0.0f, 1.0f, 1.0f, 0.0f };
}

/* ---- screenshot: top screen -> sdmc:/affine_probe_NNNN.bmp ------------- */
static int sShotN = 0;
static bool Screenshot(void) {
    u16 fbw = 0, fbh = 0;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbw, &fbh); /* 240 x 400, BGR8 */
    if (!fb) return false;
    const int W = 400, H = 240, rowsz = W * 3;
    u8 hdr[54] = {0};
    u32 fsize = 54 + rowsz * H;
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=fsize; hdr[3]=fsize>>8; hdr[4]=fsize>>16; hdr[5]=fsize>>24;
    hdr[10]=54;
    hdr[14]=40;
    hdr[18]=(u8)W; hdr[19]=(u8)(W>>8); hdr[22]=(u8)H; hdr[23]=(u8)(H>>8);
    hdr[26]=1; hdr[28]=24;
    char path[64];
    snprintf(path, sizeof(path), "sdmc:/affine_probe_%04d.bmp", sShotN);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(hdr, 1, 54, f);
    u8* row = (u8*)malloc(rowsz);
    for (int y = H - 1; y >= 0; --y) {            /* BMP is bottom-up */
        for (int x = 0; x < W; ++x) {
            /* on-screen (x,y) -> portrait framebuffer, BGR8 */
            const u8* p = fb + (x * fbw + (fbh ? (H - 1 - y) : y)) * 3;
            row[x * 3 + 0] = p[0];               /* already B,G,R -> BMP wants B,G,R */
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[2];
        }
        fwrite(row, 1, rowsz, f);
    }
    free(row);
    fclose(f);
    ++sShotN;
    return true;
}

enum { MODE_NOBLEED, MODE_BLEED, MODE_SNAP, MODE_ONEQUAD, MODE_COUNT };
static const char* kModeName[] = { "NO BLEED (raw seams)", "BLEED (grow interior edges)",
                                   "SNAP (shared edges)", "ONE QUAD (ref)" };
static const float kBleedSteps[] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f };

static float sAngleDeg = 0.0f;
static float sScale    = 2.0f;
static int   sBleedIdx = 2;
static int   sMode     = MODE_NOBLEED;
static bool  sChecker  = false;

static void DrawSprite(float originX, float originY) {
    float rad = sAngleDeg * (float)M_PI / 180.0f;
    float inv = 1.0f / sScale;
    PortAffineDecomp d = PortAffine_Decompose(
        cosf(rad) * inv, sinf(rad) * inv, -sinf(rad) * inv, cosf(rad) * inv);
    if (d.degenerate) return;
    const bool rotated = fabsf(d.angle) > 0.0005f;

    if (sMode == MODE_ONEQUAD) {
        /* Same snap as a subtile, for the whole sprite -> identical centring. */
        PortAffineQuad q = PortAffine_SubtileQuad(originX, originY,
            SPR * fabsf(d.scaleX), SPR * fabsf(d.scaleY), 0u, 0.0f);
        C2D_DrawParams p = { { q.x, q.y, q.w, q.h }, { q.cx, q.cy }, 0.5f, d.angle };
        C2D_Image whole = { &sTex, &sFull };
        C2D_DrawImage(whole, &p, NULL);
        return;
    }

    float bleed = (sMode == MODE_BLEED) ? kBleedSteps[sBleedIdx] : 0.0f;

    C2D_ImageTint ta, tb;
    C2D_PlainImageTint(&ta, C2D_Color32(255, 90, 90, 255), 0.40f);
    C2D_PlainImageTint(&tb, C2D_Color32(90, 160, 255, 255), 0.40f);

    /* SNAP (angle 0): one snapped lattice, shared edges, no overlap. */
    float ox0 = floorf(originX - SPR * fabsf(d.scaleX) * 0.5f + 0.5f);
    float oy0 = floorf(originY - SPR * fabsf(d.scaleY) * 0.5f + 0.5f);

    for (int ty = 0; ty < TILES; ++ty) {
        for (int tx = 0; tx < TILES; ++tx) {
            C2D_DrawParams p;
            if (sMode == MODE_SNAP && !rotated) {
                float l = ox0 + floorf(tx       * 8.0f * fabsf(d.scaleX) + 0.5f);
                float r = ox0 + floorf((tx + 1) * 8.0f * fabsf(d.scaleX) + 0.5f);
                float t = oy0 + floorf(ty       * 8.0f * fabsf(d.scaleY) + 0.5f);
                float b = oy0 + floorf((ty + 1) * 8.0f * fabsf(d.scaleY) + 0.5f);
                p = (C2D_DrawParams){ { l, t, r - l, b - t }, { 0, 0 }, 0.5f, 0.0f };
            } else {
                float scx, scy;
                PortAffine_SubtileCentre(&d, tx, ty, SPR, SPR, originX, originY, &scx, &scy);
                unsigned e = (bleed > 0.0f || sMode == MODE_SNAP)
                    ? PortAffine_SubtileBleedEdges(tx, ty, TILES, TILES) : 0u;
                float useBleed = (sMode == MODE_SNAP) ? 1.0f : bleed; /* rotated SNAP -> bleed 1 */
                float ow = 8.0f * fabsf(d.scaleX), oh = 8.0f * fabsf(d.scaleY);
                PortAffineQuad q = PortAffine_SubtileQuad(scx, scy, ow, oh, e, useBleed);
                p = (C2D_DrawParams){ { q.x, q.y, q.w, q.h }, { q.cx, q.cy }, 0.5f, d.angle };
            }
            C2D_Image img = { &sTex, &sSub[ty][tx] };
            const C2D_ImageTint* tint = sChecker ? (((tx + ty) & 1) ? &ta : &tb) : NULL;
            C2D_DrawImage(img, &p, tint);
        }
    }
}

int main(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    BuildTexture();

    C2D_TextBuf tb = C2D_TextBufNew(1024);
    const u32 bg = C2D_Color32(40, 40, 48, 255);
    const u32 fg = C2D_Color32(235, 235, 235, 255);
    char note[48] = "";
    int noteTtl = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kD = hidKeysDown(), kH = hidKeysHeld();
        if (kD & KEY_START) break;
        circlePosition cp; hidCircleRead(&cp);
        if (abs(cp.dx) > 20) sAngleDeg += cp.dx * 0.0015f;
        if (abs(cp.dy) > 20) sScale   += cp.dy * 0.00020f;
        if (kH & KEY_DLEFT)  sAngleDeg -= 1.0f;
        if (kH & KEY_DRIGHT) sAngleDeg += 1.0f;
        if (kH & KEY_DUP)    sScale += 0.02f;
        if (kH & KEY_DDOWN)  sScale -= 0.02f;
        if (sScale < 0.25f) sScale = 0.25f;
        if (sScale > 10.0f) sScale = 10.0f;
        if ((kD & KEY_R) && sBleedIdx < 4) sBleedIdx++;
        if ((kD & KEY_L) && sBleedIdx > 0) sBleedIdx--;
        if (kD & KEY_A) sMode = (sMode + 1) % MODE_COUNT;
        if (kD & KEY_B) sMode = (sMode + MODE_COUNT - 1) % MODE_COUNT;
        if (kD & KEY_Y) sChecker = !sChecker;
        if (kD & KEY_X) { sAngleDeg = 0.0f; sScale = 2.0f; }
        if (kD & KEY_SELECT) {
            bool ok = Screenshot();
            snprintf(note, sizeof(note), ok ? "saved affine_probe_%04d.bmp" : "screenshot FAILED",
                     sShotN - 1);
            noteTtl = 120;
        }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, bg);
        C2D_SceneBegin(top);
        DrawSprite(TOP_W * 0.5f, TOP_H * 0.5f);

        C2D_TargetClear(bot, bg);
        C2D_SceneBegin(bot);
        C2D_TextBufClear(tb);
        char s[360];
        snprintf(s, sizeof(s),
            "affine subtile probe  (ship sprite)\n\n"
            "mode   : %s\n"
            "angle  : %6.1f deg%s\n"
            "scale  : %5.2f x\n"
            "bleed  : %.1f px%s\n"
            "checker: %s\n"
            "%s\n"
            "CirclePad rot/scale   L/R bleed\n"
            "A/B mode  Y checker  X reset  SELECT shot  START exit",
            kModeName[sMode], (double)sAngleDeg,
            (fabsf(sAngleDeg) < 0.05f) ? " (ship = no rotation)" : "",
            (double)sScale,
            (double)((sMode == MODE_BLEED) ? kBleedSteps[sBleedIdx] : 0.0f),
            (sMode == MODE_BLEED) ? "" : "  (this mode ignores it)",
            sChecker ? "on" : "off",
            (noteTtl > 0) ? note : "");
        if (noteTtl > 0) --noteTtl;
        C2D_Text txt;
        C2D_TextParse(&txt, tb, s);
        C2D_TextOptimize(&txt);
        C2D_DrawText(&txt, C2D_WithColor, 8.0f, 8.0f, 0.5f, 0.5f, 0.5f, fg);
        C3D_FrameEnd(0);
    }

    C2D_TextBufDelete(tb);
    C3D_TexDelete(&sTex);
    C2D_Fini(); C3D_Fini(); gfxExit();
    return 0;
}
