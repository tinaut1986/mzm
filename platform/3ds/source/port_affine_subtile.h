#pragma once

/*
 * Affine-OBJ subtile geometry, factored out so the on-device renderer
 * (port_gpu_renderer.c) and the standalone probe
 * (platform/3ds/tools/affine_probe) compute it with the SAME code -- the
 * probe is only a faithful debugging aid if it runs the real formula.
 *
 * The GBA draws an affine sprite by sampling one backward matrix per pixel.
 * This port is a forward per-quad renderer, so it splits the sprite into its
 * 8x8 subtiles, transforms each subtile's centre through the inverted matrix,
 * and draws one rotated/scaled quad per subtile. Two independent things then
 * have to be got right, and both live here:
 *
 *   1. PortAffine_Decompose(): invert the GBA matrix and split it into the
 *      rotation + independent X/Y scale a C2D_DrawParams can express. Exact
 *      for rotate + (possibly mirrored) scale; an approximation only when the
 *      source matrix also shears (rare in MZM). Placement stays exact
 *      regardless -- see PortAffine_SubtileCentre, which uses the full
 *      inverted matrix, not the decomposition.
 *
 *   2. PortAffine_SubtileQuad(): turn one subtile's transformed centre +
 *      scaled size into a pixel-snapped quad. Snapping each subtile's origin
 *      independently is what stops the two stereo eyes sampling a rotating
 *      sprite at different sub-pixel phases -- but it also lets neighbouring
 *      subtiles round apart by up to a pixel, leaving a transparent seam
 *      (GPU_NEAREST, no atlas apron). The fix: grow each quad by `bleedPx`
 *      ONLY on edges that face another subtile of the same sprite
 *      (PortAffine_SubtileBleedEdges), so interior joins overlap while the
 *      outer silhouette keeps its exact size and outline.
 *
 * Pure math, no <3ds.h> / citro headers, so host tests and the probe can
 * both drive it.
 */

#include <math.h>
#include <stdint.h>

/* Bleed-edge bits: which sides of a subtile face a same-sprite neighbour. */
enum {
    PORT_AFFINE_BLEED_LEFT   = 0x1,
    PORT_AFFINE_BLEED_RIGHT  = 0x2,
    PORT_AFFINE_BLEED_TOP    = 0x4,
    PORT_AFFINE_BLEED_BOTTOM = 0x8,
};

typedef struct {
    /* Inverted matrix (screen = M * texture), rows [m00 m01; m10 m11]. */
    float m00, m01, m10, m11;
    /* C2D-expressible decomposition of the same. */
    float angle;   /* radians */
    float scaleX;  /* may be negative to carry a pure axis flip */
    float scaleY;
    int   degenerate; /* 1 => matrix not invertible; draw nothing */
} PortAffineDecomp;

typedef struct {
    /* C2D_DrawParams convention: (x,y) is the on-screen location of the
     * pivot -- C2D derives the top-left as (x - cx, y - cy) -- and it is
     * snapped so that DERIVED top-left lands on a whole device pixel.
     * Assign straight into params.pos.{x,y}/.{w,h} and params.center. */
    float x, y;
    float w, h;   /* quad size, device px */
    float cx, cy; /* pivot offset from the top-left: the ORIGINAL centre */
} PortAffineQuad;

/* GBA OAM affine params are 8.8 fixed. Pass them already divided by 256.0f
 * (mPa..mPd). Mirrors port_gpu_renderer.c's CollectSprite affine block. */
static inline PortAffineDecomp PortAffine_Decompose(float mPa, float mPb, float mPc, float mPd) {
    PortAffineDecomp d;
    float det = mPa * mPd - mPb * mPc;
    if (det > -0.0001f && det < 0.0001f) {
        d.m00 = d.m01 = d.m10 = d.m11 = 0.0f;
        d.angle = d.scaleX = d.scaleY = 0.0f;
        d.degenerate = 1;
        return d;
    }
    d.m00 =  mPd / det;
    d.m01 = -mPb / det;
    d.m10 = -mPc / det;
    d.m11 =  mPa / det;
    d.angle  = atan2f(d.m10, d.m00);
    d.scaleX = sqrtf(d.m00 * d.m00 + d.m10 * d.m10);
    d.scaleY = sqrtf(d.m01 * d.m01 + d.m11 * d.m11);
    /* Preserve a pure axis flip (negative determinant) as a negated Y scale
     * rather than an extra 180-degree rotation out of atan2. */
    if (d.m00 * d.m11 - d.m01 * d.m10 < 0.0f) d.scaleY = -d.scaleY;
    d.degenerate = 0;
    return d;
}

/* Transformed screen-space centre of the subtile at grid (tx,ty) of a
 * (spriteW x spriteH) sprite whose unrotated pivot is (pivotX,pivotY) in
 * screen space. Full inverted matrix -> placement stays exact under shear. */
static inline void PortAffine_SubtileCentre(const PortAffineDecomp* d,
                                            int tx, int ty, int spriteW, int spriteH,
                                            float pivotX, float pivotY,
                                            float* outX, float* outY) {
    float relX = (float)(tx * 8 + 4) - (float)spriteW * 0.5f;
    float relY = (float)(ty * 8 + 4) - (float)spriteH * 0.5f;
    *outX = pivotX + d->m00 * relX + d->m01 * relY;
    *outY = pivotY + d->m10 * relX + d->m11 * relY;
}

/* Which edges of subtile (tx,ty) face another subtile of the same sprite,
 * in texture-grid orientation (independent of the sprite's flip/rotation --
 * the quad's local +x follows increasing tx, local +y increasing ty). */
static inline unsigned PortAffine_SubtileBleedEdges(int tx, int ty, int tilesW, int tilesH) {
    unsigned e = 0;
    if (tx > 0)            e |= PORT_AFFINE_BLEED_LEFT;
    if (tx < tilesW - 1)   e |= PORT_AFFINE_BLEED_RIGHT;
    if (ty > 0)            e |= PORT_AFFINE_BLEED_TOP;
    if (ty < tilesH - 1)   e |= PORT_AFFINE_BLEED_BOTTOM;
    return e;
}

/* centreX/centreY: the subtile's already-transformed centre, device px.
 * ow/oh: its scaled size, device px (8 * |scale| * any display stretch).
 * bleedEdges: PortAffine_SubtileBleedEdges(). bleedPx: 0 disables. */
static inline PortAffineQuad PortAffine_SubtileQuad(float centreX, float centreY,
                                                    float ow, float oh,
                                                    unsigned bleedEdges, float bleedPx) {
    float bl = (bleedEdges & PORT_AFFINE_BLEED_LEFT)   ? bleedPx : 0.0f;
    float br = (bleedEdges & PORT_AFFINE_BLEED_RIGHT)  ? bleedPx : 0.0f;
    float bt = (bleedEdges & PORT_AFFINE_BLEED_TOP)    ? bleedPx : 0.0f;
    float bb = (bleedEdges & PORT_AFFINE_BLEED_BOTTOM) ? bleedPx : 0.0f;
    PortAffineQuad q;
    q.w  = ow + bl + br;
    q.h  = oh + bt + bb;
    /* Pivot moves by the left/top growth so the extra width is added
     * outward on each grown edge and the centre does not move. */
    q.cx = bl + ow * 0.5f;
    q.cy = bt + oh * 0.5f;
    /* Snap the DERIVED top-left (x - cx) to a whole pixel, then add the
     * pivot offset back so x is the pivot's screen position -- the value
     * C2D_DrawParams.pos wants (see C2D_DrawImageAtRotated: pos is the
     * centre location, C2D subtracts .center to get the top-left). */
    q.x  = floorf(centreX - q.cx + 0.5f) + q.cx;
    q.y  = floorf(centreY - q.cy + 0.5f) + q.cy;
    return q;
}
