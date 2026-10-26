#!/usr/bin/env python3
"""Make the generated area of a picture with scanlines look like the game's own.

    fix_scanlines.py PACKAGE_DIR SCENE REFIT.png OUT.png

A background with a scanline pattern in its art (every third row darker: the file select's statue)
gives itself away where the AI's lines are not the same: another pitch, phase and darkness, and
greens that are never quite the game's. This redraws everything outside the original's rectangle,
starting from REFIT.raw.png (written next to REFIT.png by refit.py: what the AI drew, before its
colours were mapped to the game's):

  1. the AI's own lines are divided out, row by row, without blurring anything: each row is scaled
     by how much brighter or darker it is than the rows around it;
  2. the light rows take the game's colours: the AI drew the original's rectangle too, so each
     brightness it used there is paired with the game's colour at the same rank (histogram
     matching); that table colours the new area. The brightness is nudged where the new area meets
     the original so that the two agree along the seam;
  3. the dark rows are the game's own: each is what the game makes of the light row next to it (the
     colour pairs are read from the original), so the lines are crisp and as dark as the original's.

The pattern (period 3, which rows) is measured from 2-original-picture.png. A scene without
scanlines does not need this: use refit.py's result.
"""
import os
import sys
from collections import Counter

import numpy as np
from PIL import Image
from scipy.ndimage import uniform_filter, uniform_filter1d

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_package import SCENES  # noqa: E402

PERIOD = 3
FADE = 30.0           # px over which the seam correction fades out
GAIN = (0.6, 1.9)     # the most it may darken or lighten the AI's brightness
TOP_RANK = 0.97       # the game's brightest colours are rare: never go above this rank
STAIN_MAX = 56         # colours this dark or darker (and not black) are the stain tones
BASE = (0, 40, 56)     # the game's usual dark base colour
BLOB = 0.55           # a patch of the darkest colour this full is a stain, not texture


def key(c):
    return (int(c[0]) << 16) | (int(c[1]) << 8) | int(c[2])


def learn_dark(orig, phase):
    """(offset of the light row a dark row follows, {light colour: dark colour}) from the original."""
    best = None
    for off in (1, -1):
        votes = {}
        for y in range(phase if phase else PERIOD, orig.shape[0] - 1, PERIOD):
            ys = y + off
            if not 0 <= ys < orig.shape[0]:
                continue
            for x in range(orig.shape[1]):
                votes.setdefault(key(orig[ys, x]), Counter())[key(orig[y, x])] += 1
        table = {k: c.most_common(1)[0][0] for k, c in votes.items()}
        hits = sum(c[table[k]] for k, c in votes.items())
        total = sum(sum(c.values()) for c in votes.values())
        if best is None or hits / total > best[0]:
            best = (hits / total, off, table)
    print(f'dark row = what the game makes of the light row {"below" if best[1] > 0 else "above"} '
          f'({best[0]:.0%} of its pixels follow the table)')
    return best[1], best[2]


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    pkg, scene, src, dst = sys.argv[1:]
    sc = SCENES[scene]
    cx0, cy0, cx1, cy1 = sc['canvas']
    px0, py0, px1, py1 = sc['picture']
    X, Y, w, h = px0 - cx0, py0 - cy0, px1 - px0, py1 - py0
    orig = np.array(Image.open(os.path.join(pkg, f'mzm-{scene}', '2-original-picture.png')).convert('RGB')).astype(int)
    ai = np.array(Image.open(os.path.splitext(src)[0] + '.raw.png').convert('RGB')).astype(float)
    H, W = ai.shape[:2]

    # The pattern, from the original: which row of each three is the dark one.
    lum_rows = orig.sum(2).mean(1)
    phase = int(np.argmin([lum_rows[p::PERIOD].mean() for p in range(PERIOD)]))
    print(f'dark rows: picture row % {PERIOD} == {phase}')
    y = np.arange(H)
    dark_row = ((y - Y) % PERIOD) == phase

    # 1. Divide out the AI's own lines: row gain = row mean over the mean of the rows around it.
    rowmean = ai.sum(2).mean(1)
    around = uniform_filter1d(rowmean, 3 * PERIOD + 2, mode='nearest')
    flat = ai / np.clip(rowmean / np.maximum(around, 1), 0.3, 3.0)[:, None, None]
    lum = flat.sum(2)

    # 2. The light rows' colours: rank in the AI's picture of the original's rectangle -> the
    # game's colour of that rank.
    inside = np.zeros((H, W), bool)
    inside[Y:Y + h, X:X + w] = True
    orows = (np.arange(h) % PERIOD) == phase
    ref = orig[~orows].reshape(-1, 3)
    ref = ref[np.argsort(ref.sum(1), kind='stable')]
    ai_in = np.sort(lum[Y:Y + h, X:X + w][~dark_row[Y:Y + h]].ravel())
    light = ~dark_row[:, None] & ~inside

    def colourise(l):
        res = np.zeros((H, W, 3))
        rank = np.minimum(np.searchsorted(ai_in, l[light]) / max(1, len(ai_in)), TOP_RANK)
        res[light] = ref[np.minimum((rank * len(ref)).astype(int), len(ref) - 1)]
        return res

    # Where the new area meets the original its brightness should be the original's: a gain per
    # position along each edge, found by trying, fading out with the distance from the seam.
    yy, xx = np.mgrid[0:H, 0:W]
    dist = np.stack([X - xx, xx - (X + w - 1), Y - yy, yy - (Y + h - 1)]).astype(float)   # L R T B
    dist[dist <= 0] = 1e9
    edge, d = dist.argmin(0), dist.min(0)
    fade = np.exp(-d / FADE)
    gain = [np.ones(h), np.ones(h), np.ones(w), np.ones(w)]
    lrows = ~(((np.arange(h)) % PERIOD) == phase)          # light rows of the original
    for _ in range(6):
        g = np.ones((H, W))
        for k in range(4):
            idx = np.clip((yy if k < 2 else xx) - (Y if k < 2 else X), 0, (h if k < 2 else w) - 1)
            g = np.where((edge == k) & ~inside, gain[k][idx], g)
        res = colourise(lum * (1 + (g - 1) * fade))
        lsum = res.sum(2)
        for k in range(4):
            if k < 2:
                x0 = X - 4 if k == 0 else X + w
                a = lsum[Y:Y + h, x0:x0 + 4].mean(1)
                b = orig.sum(2)[:, 0:4].mean(1) if k == 0 else orig.sum(2)[:, w - 4:w].mean(1)
                a, b = np.where(lrows, a, np.nan), np.where(lrows, b, np.nan)
            else:
                # the light rows among the 4 rows beside the seam, per column
                rr = range(Y - 4, Y) if k == 2 else range(Y + h, Y + h + 4)
                orr = range(0, 4) if k == 2 else range(h - 4, h)
                ra = [r for r in rr if not dark_row[r]]
                rb = [r for r in orr if (r % PERIOD) != phase]
                a = lsum[ra, X:X + w].mean(0)
                b = orig.sum(2)[rb].mean(0)
            r = np.where(np.isfinite(a) & np.isfinite(b) & (a > 8), b / np.maximum(a, 1), 1.0)
            r = np.convolve(np.pad(r, 8, mode='edge'), np.ones(17) / 17, 'valid')
            gain[k] = np.clip(gain[k] * np.clip(r, 0.8, 1.25), *GAIN)

    # Stains: the game's two darkest colours are texture over its usual dark base, never a flat
    # patch; where the new area has one (the AI drew a shadow), the base colour goes in, with the
    # darker one as a sparse dither so it keeps the grain.
    pal = np.unique(orig.reshape(-1, 3), axis=0)
    dark_set = [tuple(c) for c in pal if 0 < sum(c) <= STAIN_MAX]
    is_d = np.zeros((H, W), bool)
    for c in dark_set:
        is_d |= np.all(res == c, axis=2)
    is_d &= light
    # dark rows carry no colour yet: look at the light row above instead
    is_d[dark_row] = np.roll(is_d, 1, axis=0)[dark_row]
    stain = (uniform_filter(is_d.astype(float), size=(7, 9)) > BLOB) & light
    grain = ((xx + 2 * yy) % 5) == 0
    res[stain] = BASE
    res[stain & grain] = min(dark_set, key=sum)
    # black on a light row is the canvas's edge the AI's picture did not reach, not art
    res[(res.sum(2) == 0) & light & ~inside] = BASE
    print(f'{int(stain.sum())} px of stains redrawn')

    res[Y:Y + h, X:X + w] = orig

    # 3. The dark rows: what the game makes of the light row beside them.
    off, table = learn_dark(orig, phase)
    pal = np.unique(orig.reshape(-1, 3), axis=0)
    for r in np.nonzero(dark_row)[0]:
        s = r + off
        if not 0 <= s < H:
            s = r - off
        for x in range(W):
            if inside[r, x]:
                continue
            k = key(res[s, x])
            if k in table:
                v = table[k]
                res[r, x] = ((v >> 16) & 255, (v >> 8) & 255, v & 255)
            else:   # a colour the original never has on a light row: the nearest to a dimmed copy
                res[r, x] = pal[((pal - res[s, x] * 0.45) ** 2).sum(1).argmin()]
    Image.fromarray(res.astype(np.uint8)).save(dst)
    print(f'{dst}: {W}x{H}, {len(np.unique(res.reshape(-1, 3), axis=0))} colours')


if __name__ == '__main__':
    main()
