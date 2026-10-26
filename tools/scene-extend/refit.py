#!/usr/bin/env python3
"""Rescue an AI picture that does not keep the game's pixels at 1:1.

    refit.py PACKAGE_DIR SCENE SOURCE OUT.png [MIN_SCALE MAX_SCALE]

Some image AIs draw the whole canvas again at their own size and place (a 1264x848 JPEG, the
game's picture at about 2x somewhere in it) and then paste the original over the middle of a
crop that does not line up with it, so the sides they drew do not meet the original. When the
image they drew before cropping is still at hand (SOURCE), this finds where the game's picture
sits in it and crops the canvas again from there:

  1. a coarse search over uniform scales (MIN_SCALE..MAX_SCALE source px per game px, default
     1.5..3.5) by normalised cross-correlation of the grey levels;
  2. a refinement of the x and y scales and the offset, separately, to sub-pixel;
  3. the canvas sampled from SOURCE at that grid (box-filtered over a game pixel), each pixel
     given the nearest colour of the original, and the original put back in its rectangle.

It prints the fit (a correlation near 0.99 is a good one) and how many canvas pixels fall
outside SOURCE: those are left black and need drawing. SCENE is a key of make_package.SCENES;
PACKAGE_DIR holds its mzm-<scene>/ folder (2-original-picture.png is read from there). The
result is a variant like any other: check it, then give it to to_console.py.
"""
import os
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import map_coordinates, uniform_filter
from scipy.optimize import minimize
from scipy.signal import fftconvolve

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_package import SCENES  # noqa: E402


def coarse(orig, src, lo, hi, step=0.02):
    """Best uniform scale and the picture's top-left in SOURCE px, by NCC."""
    t = orig - orig.mean()
    tn = np.sqrt((t ** 2).sum())
    ones = np.ones_like(t)
    oh, ow = orig.shape
    img = Image.fromarray(src.astype(np.uint8))
    best = None
    for s in np.arange(lo, hi, step):
        w, h = round(img.width / s), round(img.height / s)
        if w < ow or h < oh:
            continue
        a = np.asarray(img.resize((w, h), Image.BOX), dtype=np.float64)
        num = fftconvolve(a, t[::-1, ::-1], mode='valid')
        s1 = fftconvolve(a, ones, mode='valid')
        s2 = fftconvolve(a * a, ones, mode='valid')
        ncc = num / (np.sqrt(np.maximum(s2 - s1 * s1 / t.size, 1e-9)) * tn)
        y, x = np.unravel_index(np.argmax(ncc), ncc.shape)
        if best is None or ncc[y, x] > best[0]:
            best = (ncc[y, x], s, x * s, y * s)
    if best is None:
        sys.exit('the picture does not fit in SOURCE at any scale of the range')
    return best


def refine(orig, src, s, x0, y0):
    """x/y scales and offset maximising the NCC of SOURCE sampled at the picture's grid."""
    oh, ow = orig.shape
    yy, xx = np.mgrid[0:oh, 0:ow].astype(np.float64)
    b = orig - orig.mean()
    blurred = uniform_filter(src, max(1, int(round(s))))

    def cost(p):
        sx, sy, px, py = p
        a = map_coordinates(blurred, [py + (yy + 0.5) * sy - 0.5, px + (xx + 0.5) * sx - 0.5],
                            order=1, mode='nearest')
        a = a - a.mean()
        return -(a * b).sum() / np.sqrt((a * a).sum() * (b * b).sum())

    p0 = [s, s, x0, y0]
    simplex = [p0, [s + .03, s, x0, y0], [s, s + .03, x0, y0], [s, s, x0 + 2, y0], [s, s, x0, y0 + 2]]
    r = minimize(cost, p0, method='Nelder-Mead',
                 options=dict(xatol=0.01, fatol=1e-6, maxiter=2000, initial_simplex=simplex))
    return -r.fun, r.x


def main():
    if len(sys.argv) not in (5, 7):
        sys.exit(__doc__)
    package_dir, scene, source, out = sys.argv[1:5]
    lo, hi = (float(v) for v in sys.argv[5:7]) if len(sys.argv) == 7 else (1.5, 3.5)
    sc = SCENES[scene]
    cx0, cy0, cx1, cy1 = sc['canvas']
    px0, py0, px1, py1 = sc['picture']
    orig_rgb = np.asarray(Image.open(os.path.join(package_dir, f'mzm-{scene}', '2-original-picture.png'))
                          .convert('RGB'))
    orig = np.asarray(Image.fromarray(orig_rgb).convert('L'), dtype=np.float64)
    src_rgb = np.asarray(Image.open(source).convert('RGB'), dtype=np.float64)
    src = np.asarray(Image.open(source).convert('L'), dtype=np.float64)

    _, s, x0, y0 = coarse(orig, src, lo, hi)
    ncc, (sx, sy, x0, y0) = refine(orig, src, s, x0, y0)
    print(f'fit: correlation {ncc:.4f}, scale x {sx:.4f} y {sy:.4f}, picture at ({x0:.1f}, {y0:.1f}) of SOURCE')

    # The canvas grid in SOURCE px (canvas and picture are in the scene's BG px).
    W, H = cx1 - cx0, cy1 - cy0
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    gx = x0 + (xx + cx0 - px0 + 0.5) * sx - 0.5
    gy = y0 + (yy + cy0 - py0 + 0.5) * sy - 0.5
    k = max(1, int(round((sx + sy) / 2)))
    img = np.stack([map_coordinates(uniform_filter(src_rgb[..., c], k), [gy, gx], order=1, mode='nearest')
                    for c in range(3)], -1)
    outside = (gx < 0) | (gx > src.shape[1] - 1) | (gy < 0) | (gy > src.shape[0] - 1)
    # Majority vote instead of the mean (REFIT_MEAN=1 for the mean): the source is quantised to the game's colours first, then each
    # game pixel takes the colour most of its centre points (a 3x3 grid inside the cell) have. A
    # mean mixes the colours on either side of an edge into one that is in neither; the vote keeps
    # the edge where the AI put it and snaps it to the game's grid.
    if not os.environ.get('REFIT_MEAN'):
        palf = np.unique(orig_rgb.reshape(-1, 3), axis=0).astype(np.float64)
        votes = []
        for oy in (-1, 0, 1):
            for ox in (-1, 0, 1):
                sy_ = np.clip(np.rint(gy + oy * sy * 0.25).astype(int), 0, src.shape[0] - 1)
                sx_ = np.clip(np.rint(gx + ox * sx * 0.25).astype(int), 0, src.shape[1] - 1)
                px = src_rgb[sy_, sx_]
                idx = np.empty(px.shape[:2], int)
                for y in range(px.shape[0]):
                    idx[y] = ((px[y, :, None, :] - palf[None]) ** 2).sum(-1).argmin(-1)
                votes.append(idx)
        votes = np.stack(votes, -1)
        counts = np.stack([(votes == k).sum(-1) for k in range(len(palf))], -1)
        img = palf[counts.argmax(-1)]
    # What the AI drew at this grid, before the nearest-colour step (fix_scanlines.py reads it).
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(os.path.splitext(out)[0] + '.raw.png')
    pal = np.unique(orig_rgb.reshape(-1, 3), axis=0).astype(np.float64)
    res = np.zeros((H, W, 3), np.uint8)
    for y in range(H):  # row by row: the distance table of the whole canvas is large
        d = ((img[y, :, None, :] - pal[None]) ** 2).sum(-1)
        res[y] = pal[d.argmin(-1)].astype(np.uint8)
    res[outside] = 0
    res[py0 - cy0:py1 - cy0, px0 - cx0:px1 - cx0] = orig_rgb
    Image.fromarray(res).save(out)
    print(f'{out}: {W}x{H}, {len(pal)} colours; {int(outside.sum())} canvas px outside SOURCE (left black)')


if __name__ == '__main__':
    main()
