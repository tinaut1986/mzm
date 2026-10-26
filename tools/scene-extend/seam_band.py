#!/usr/bin/env python3
"""A repair pass for the AI: the seam painted out in magenta.

    seam_band.py PACKAGE_DIR SCENE BASE.png OUT_DIR [WIDTH]

BASE.png is a full canvas whose original rectangle is exact (a variant that came out of
refit.py / match_tones.py with the original put back, not feathered). OUT_DIR gets:

  repair-WxH.png     BASE with a band of WIDTH px (default 20) just outside the original's
                     rectangle painted pure magenta (255, 0, 255): the AI repaints only that
  repair-mask.png    the same band, white on black
  README-REPAIR.md   what to ask the AI

The original's rectangle and everything beyond the band stay as they are in BASE.
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_package import SCENES  # noqa: E402

README = """# Repair the seam: repaint only the magenta band

The picture is a Game Boy Advance pixel-art background (Metroid: Zero Mission) that has been made
bigger: the middle {w}x{h} rectangle (x={X}..{X2}, y={Y}..{Y2}) is the game's own art; everything around it was
drawn by an AI and does not meet the original cleanly at the edge.

The pure magenta (255, 0, 255) band, {n} px wide, runs around the outside of the original's
rectangle. **Repaint only the magenta pixels**, so that the picture crosses the original's edge
without a visible seam.

## Rules
1. Change nothing that is not magenta. The original in the middle and the area outside the band
   must stay pixel for pixel as they are.
2. Fill the band with the continuation of what touches it on both sides: lines, streaks, shapes and
   colours that reach the original's edge go on through the band and join what is outside it, with
   the original's pixel-art look (hard 1-pixel edges, no blur, no glow, no new objects).
3. Use only colours that already appear in the picture. No magenta may be left.
4. Do not draw a border, frame, outline or shadow along the band.
5. Output: a PNG exactly {W}x{H}, no scaling, no smoothing.
"""


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    pkg, scene, base, out = sys.argv[1:5]
    n = int(sys.argv[5]) if len(sys.argv) == 6 else 20
    sc = SCENES[scene]
    cx0, cy0, cx1, cy1 = sc['canvas']
    px0, py0, px1, py1 = sc['picture']
    X, Y, w, h = px0 - cx0, py0 - cy0, px1 - px0, py1 - py0
    img = np.array(Image.open(base).convert('RGB'))
    H, W = img.shape[:2]
    yy, xx = np.mgrid[0:H, 0:W]
    dx = np.maximum(np.maximum(X - xx, xx - (X + w - 1)), 0)
    dy = np.maximum(np.maximum(Y - yy, yy - (Y + h - 1)), 0)
    band = (np.maximum(dx, dy) > 0) & (np.maximum(dx, dy) <= n)      # square rings, like the rectangle
    orig = np.array(Image.open(os.path.join(pkg, f'mzm-{scene}', '2-original-picture.png')).convert('RGB'))
    assert (img[Y:Y + h, X:X + w] == orig).all(), 'BASE changes the original: use a variant with it exact'
    img[band] = (255, 0, 255)
    os.makedirs(out, exist_ok=True)
    Image.fromarray(img).save(os.path.join(out, f'repair-{W}x{H}.png'))
    Image.fromarray((band * 255).astype(np.uint8)).save(os.path.join(out, 'repair-mask.png'))
    with open(os.path.join(out, 'README-REPAIR.md'), 'w') as f:
        f.write(README.format(w=w, h=h, X=X, X2=X + w - 1, Y=Y, Y2=Y + h - 1, n=n, W=W, H=H))
    print(f'{out}: band of {n} px, {int(band.sum())} px to repaint')


if __name__ == '__main__':
    main()
