#!/usr/bin/env python3
"""Make the generated area of a picture take the game's colours and meet the original at the seam.

    match_tones.py PACKAGE_DIR SCENE REFIT.png OUT.png

For a scene without a scanline pattern (fix_scanlines.py is for the ones with it). Starting from
REFIT.raw.png (written by refit.py: what the AI drew, before its colours were mapped):

  1. the AI drew the original's own rectangle too, so a colour transform is fitted from the AI's
     colours there to the game's (a quadratic least-squares fit on RGB) and applied to the new area:
     the AI's reds and yellows come out as the game's, not brighter or more saturated;
  2. brightness is nudged along each seam (a gain per position along each edge, found by trying and
     fading out with the distance), so the new area starts where the original stops;
  3. the original and the new area are mixed over a few pixels on each side of the seam (the
     original's outermost rows change a little, the rest of it does not);
  4. every pixel takes the nearest colour the original uses, which gives the pixel-art look
     back; the original goes back in its place, but for that band.
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_package import SCENES  # noqa: E402

FADE = 55.0           # px over which the seam's brightness correction fades out
GAIN = (0.5, 1.8)
FEATHER = 9           # px each side of the seam over which the original and the new area are mixed


def feats(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    return np.stack([r, g, b, r * r / 255, g * g / 255, b * b / 255, r * g / 255, r * b / 255, g * b / 255,
                     np.ones_like(r) * 255], -1)


def nearest(img, pal):
    out = np.empty_like(img)
    for y in range(img.shape[0]):
        d = ((img[y, :, None, :] - pal[None]) ** 2).sum(-1)
        out[y] = pal[d.argmin(-1)]
    return out


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    pkg, scene, src, dst = sys.argv[1:]
    sc = SCENES[scene]
    cx0, cy0, cx1, cy1 = sc['canvas']
    px0, py0, px1, py1 = sc['picture']
    X, Y, w, h = px0 - cx0, py0 - cy0, px1 - px0, py1 - py0
    orig = np.array(Image.open(os.path.join(pkg, f'mzm-{scene}', '2-original-picture.png')).convert('RGB')).astype(float)
    ai = np.array(Image.open(os.path.splitext(src)[0] + '.raw.png').convert('RGB')).astype(float)
    H, W = ai.shape[:2]
    pal = np.unique(orig.reshape(-1, 3), axis=0)

    # 1. colour transform AI -> game over the original's rectangle.
    A = feats(ai[Y:Y + h, X:X + w]).reshape(-1, 10)
    coef, *_ = np.linalg.lstsq(A, orig.reshape(-1, 3), rcond=None)
    mapped = np.clip(feats(ai).reshape(-1, 10) @ coef, 0, 255).reshape(H, W, 3)
    err = np.abs(mapped[Y:Y + h, X:X + w] - orig).mean()
    print(f'colour transform: mean error {err:.1f} / 255 over the original')

    # 2. seam gain.
    inside = np.zeros((H, W), bool)
    inside[Y:Y + h, X:X + w] = True
    yy, xx = np.mgrid[0:H, 0:W]
    dist = np.stack([X - xx, xx - (X + w - 1), Y - yy, yy - (Y + h - 1)]).astype(float)
    dist[dist <= 0] = 1e9
    edge, d = dist.argmin(0), dist.min(0)
    fade = np.exp(-d / FADE)
    gain = [np.ones(h), np.ones(h), np.ones(w), np.ones(w)]
    band = 4
    tgt = [orig[:, :band].sum(2).mean(1), orig[:, -band:].sum(2).mean(1),
           orig[:band].sum(2).mean(0), orig[-band:].sum(2).mean(0)]
    for _ in range(6):
        g = np.ones((H, W))
        for k in range(4):
            idx = np.clip((yy if k < 2 else xx) - (Y if k < 2 else X), 0, (h if k < 2 else w) - 1)
            g = np.where((edge == k) & ~inside, gain[k][idx], g)
        cur = mapped * (1 + (g - 1) * fade)[..., None]
        seen = [cur[Y:Y + h, X - band:X].sum(2).mean(1), cur[Y:Y + h, X + w:X + w + band].sum(2).mean(1),
                cur[Y - band:Y, X:X + w].sum(2).mean(0), cur[Y + h:Y + h + band, X:X + w].sum(2).mean(0)]
        for k in range(4):
            r = np.where(seen[k] > 8, tgt[k] / np.maximum(seen[k], 1), 1.0)
            r = np.convolve(np.pad(r, 8, mode='edge'), np.ones(17) / 17, 'valid')
            gain[k] = np.clip(gain[k] * np.clip(r, 0.85, 1.18), *GAIN)

    # 3. mix across the seam: weight of the original 0 at FEATHER px outside, 1 at FEATHER px inside.
    inner = np.minimum.reduce([xx - X, X + w - 1 - xx, yy - Y, Y + h - 1 - yy]).astype(float)   # > 0 inside
    wgt = np.clip((inner + FEATHER) / (2 * FEATHER), 0, 1)
    full = cur.copy()
    full[Y:Y + h, X:X + w] = orig
    mix = full * wgt[..., None] + cur * (1 - wgt[..., None])

    # 4. the game's colours, the original back away from the seam.
    res = nearest(np.clip(mix, 0, 255), pal)
    deep = inner >= FEATHER
    res[deep] = full[deep]
    Image.fromarray(res.astype(np.uint8)).save(dst)
    print(f'{dst}: {W}x{H}, {len(np.unique(res.reshape(-1, 3), axis=0))} colours')


if __name__ == '__main__':
    main()
