#!/usr/bin/env python3
"""Delivery packages for the generative AI, for a scene's sprite that the GBA screen cuts off
(the intro's flame behind Samus's ship stops flat at the screen's top and right edges, which
the WIDE view shows). Same idea as make_package.py, for sprites instead of a background.

    make_sprite_package.py DUMPS_DIR OUT_DIR [SPRITE ...]

DUMPS_DIR holds the screen dump named in SPRITES below (capture.sh names: `<dir>/<n>-vram.bin`
...): the sprite's tiles and OBJ palette are read from it. OUT_DIR gets `mzm-<sprite>/` with,
for every animation frame of the sprite:

  1-template-<frame>-WxH.png           the canvas: the sprite as the game draws it, transparent around it
  1-template-<frame>-WxH-magenta.png   the same with magenta (255, 0, 255) for "empty"
  2-original-<frame>.png               the sprite alone, at its own size
and once:
  3-final-composite.png                the 400x240 WIDE view: background, the other sprites, this one
  4-do-not-paint.png                   the sprites next to it (in canvas coordinates): context only
  README-AI.md

Positions are GBA screen pixels (the 240x160 frame); the WIDE view in PIXEL PERFECT shows that
frame at (80, 40) of 400x240. The frames' OAM entries are copied from src/data/*.c.

All of it is the game's art: keep OUT_DIR out of the repository.
"""
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gba_dump import Dump, OBJ_SIZES  # noqa: E402

# OAM_DIMS name -> (w, h)
DIMS = {f'{w}x{h}': (w, h) for row in OBJ_SIZES for (w, h) in row}

# Frames: name -> [(x, y, dims, tile, palette)], offsets from `anchor` (src/data/intro_data.c).
SPRITES = {
    'intro-zebes-flame': dict(
        name="the intro's view of Zebes: the heat trail behind Samus's ship",
        dump='intro-600/1', anchor=(128, 96), canvas=(96, -40, 336, 112),
        frames={
            'A': [(0, -96, '64x32', 0x210, 4), (64, -96, '64x32', 0x218, 4), (0, -64, '32x16', 0x290, 4),
                  (32, -64, '32x16', 0x294, 4), (64, -64, '32x16', 0x298, 4), (96, -64, '32x16', 0x29c, 4),
                  (0, -48, '32x8', 0x2d0, 4), (32, -48, '32x8', 0x2d4, 4), (64, -48, '32x8', 0x2d8, 4),
                  (96, -48, '32x8', 0x2dc, 4), (-24, -48, '16x32', 0x2cd, 4), (-8, -48, '8x32', 0x2cf, 4),
                  (-24, -16, '16x32', 0x34d, 4), (-8, -16, '8x32', 0x34f, 4), (0, -40, '64x32', 0x2f0, 4),
                  (64, -40, '64x32', 0x2f8, 4), (0, -8, '32x16', 0x370, 4), (32, -8, '32x16', 0x374, 4),
                  (64, -8, '32x16', 0x378, 4), (96, -8, '32x16', 0x37c, 4), (0, 8, '32x8', 0x3b0, 4),
                  (32, 8, '32x8', 0x3b4, 4), (64, 8, '32x8', 0x3b8, 4), (96, 8, '32x8', 0x3bc, 4)],
            'B': [(0, -96, '64x64', 0x50, 4), (64, -96, '64x64', 0x58, 4), (-24, -48, '16x32', 0x10d, 4),
                  (-8, -48, '8x32', 0x10f, 4), (-24, -16, '16x32', 0x18d, 4), (-8, -16, '8x32', 0x18f, 4),
                  (0, -32, '64x32', 0x150, 4), (64, -32, '64x32', 0x158, 4), (0, 0, '32x16', 0x1d0, 4),
                  (32, 0, '32x16', 0x1d4, 4), (64, 0, '32x16', 0x1d8, 4), (96, 0, '32x16', 0x1dc, 4)],
        },
        # sIntroViewOfZebesShipOam: drawn with the trail, over it.
        others=[(-16, -40, '64x32', 0x40, 3), (48, -40, '8x32', 0x48, 3), (-16, -8, '32x16', 0xc0, 3),
                (16, -8, '8x16', 0xc4, 3)],
        # The WIDE background of the scene (port_scene_pictures: the generated picture), its canvas
        # in BG px and the scene's BG scroll at the dump, to place it in the composite.
        background=('~/Imágenes/MZM/mzm-intro-zebes/variant-1.png', (-88, -40)),
        about="Samus's gunship dives towards planet Zebes. Behind it a long red heat trail (a "
              "semi-transparent sprite: the game mixes it with the planet under it) runs up and to the "
              "right, out of the screen. On the GBA's 240x160 screen it was cut at the top and right "
              "edges, where the screen ended; the wider 3DS screen now shows those cuts as straight "
              "lines. The trail flickers between two drawings, A and B, every 4 frames: both must be "
              "extended the same way.",
        task="Continue the trail beyond its straight cut edges (the top edge at y={top} and the right "
             "edge at x={right} of the canvas) up to the canvas's top and right edges, in the same "
             "direction, with the same width, colour bands and streaks, so that it leaves the canvas "
             "the way it used to leave the GBA screen. Nothing else changes."),
}

README = """# Extend a Game Boy Advance sprite: {name}

Pixel art from Metroid: Zero Mission (Game Boy Advance, 1 pixel = 1 game pixel). The game is
now shown on a wider screen and this sprite, which the old screen cut off, must be made longer.

{about}

## Files
{files}
- `3-final-composite.png`: the 400x240 screen as the player sees it now, with the cut trail
  (frame A, see-through as the game draws it). The canvas's top-left corner is at x={cx0},
  y={cy0} of it; the canvas goes {past} px past the screen's right edge on purpose (the screen
  shifts a little during the scene). Context only.
- `4-do-not-paint.png`: the ship (another sprite drawn over the trail), at its place in the
  canvas. Context only: do not paint it, do not change the trail under it.

## What to do
{task}

## Rules
1. Do not change a single existing pixel of the trail. It is the game's own art.
2. Use only the colours the trail already uses ({colours} colours). No anti-aliasing, gradients,
   blur, dithering it does not use, or new hues.
3. Same pixel scale and style: hard 1-pixel edges, the same streaks and shading bands.
4. **The background stays transparent.** This is a sprite drawn over the planet: every pixel that
   is not trail must be fully transparent (or pure magenta 255,0,255 if your tool cannot keep
   transparency). No semi-transparent pixels: the game makes the trail see-through by itself.
5. Frames A and B must match each other where they overlap now, and be extended the same way.
6. Output: for each frame a PNG exactly {W}x{H} (the template's size), no scaling, no smoothing.
   Give 2 or 3 variants of each, as separate files named so that A and B of a variant pair up.
"""


def draw(entries, anchor, d, size, origin):
    """The entries with the dump's OBJ tiles and palette (2D mapping) into `size`, the GBA
    screen's (0, 0) at `origin`."""
    img = Image.new('RGBA', size, (0, 0, 0, 0))
    px = img.load()
    for x, y, dims, tile, pb in entries:
        w, h = DIMS[dims]
        for yy in range(h):
            for xx in range(w):
                t = tile + (yy // 8) * 32 + xx // 8
                o = 0x10000 + (t & 0x3ff) * 32 + (yy % 8) * 4 + (xx % 8) // 2
                b = d.vram[o]
                i = (b >> (4 * (xx & 1))) & 15
                if not i:
                    continue
                c = struct.unpack_from('<H', d.objpal, (pb * 16 + i) * 2)[0]
                X, Y = origin[0] + anchor[0] + x + xx, origin[1] + anchor[1] + y + yy
                if 0 <= X < size[0] and 0 <= Y < size[1]:
                    px[X, Y] = ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3, 255)
    return img


def build(key, sp, dumps_dir, out_dir):
    d = Dump(os.path.join(dumps_dir, sp['dump']))
    cx0, cy0, cx1, cy1 = sp['canvas']
    W, H = cx1 - cx0, cy1 - cy0
    folder = os.path.join(out_dir, f'mzm-{key}')
    os.makedirs(folder, exist_ok=True)
    colours = set()
    files = []
    tops, rights = [], []
    for name, entries in sp['frames'].items():
        img = draw(entries, sp['anchor'], d, (W, H), (-cx0, -cy0))
        colours |= {c for c in img.get_flattened_data() if c[3]}
        bbox = img.getbbox()
        tops.append(bbox[1])
        rights.append(bbox[2])
        img.save(os.path.join(folder, f'1-template-{name}-{W}x{H}.png'))
        mag = Image.new('RGBA', (W, H), (255, 0, 255, 255))
        mag.alpha_composite(img)
        mag.convert('RGB').save(os.path.join(folder, f'1-template-{name}-{W}x{H}-magenta.png'))
        img.crop(bbox).save(os.path.join(folder, f'2-original-{name}.png'))
        files.append(f"- `1-template-{name}-{W}x{H}.png`: **the canvas for frame {name}.** The trail as "
                     f"the game draws it, at its place; the rest is transparent. `...-magenta.png` is "
                     f"the same with magenta for empty, if your tool prefers it.")
        files.append(f"- `2-original-{name}.png`: frame {name} alone, at its own size.")
    others = draw(sp['others'], sp['anchor'], d, (W, H), (-cx0, -cy0))
    others.save(os.path.join(folder, '4-do-not-paint.png'))

    # The composite: the WIDE view (GBA frame at 80, 40) with the background and the sprites.
    view = Image.new('RGBA', (400, 240), (0, 0, 0, 255))
    path, (bx0, by0) = sp['background']
    bg = Image.open(os.path.expanduser(path)).convert('RGBA')
    hofs = d.reg(0x10) & 0xFF
    hofs = hofs - 256 if hofs > 128 else hofs
    view.alpha_composite(bg, (80 + bx0 - hofs, 40 + by0))
    first = draw(list(sp['frames'].values())[0], sp['anchor'], d, (400, 240), (80, 40))
    # Blended like the game: BLDALPHA's EVA/16 of the sprite over EVB/16 of what is under it.
    bldalpha = d.reg(0x52)
    eva, evb = min(16, bldalpha & 31), min(16, (bldalpha >> 8) & 31)
    under = view.copy()
    fp, vp, up = first.load(), view.load(), under.load()
    for y in range(240):
        for x in range(400):
            if fp[x, y][3]:
                vp[x, y] = tuple(min(255, (fp[x, y][i] * eva + up[x, y][i] * evb) // 16) for i in range(3)) + (255,)
    view.alpha_composite(draw(sp['others'], sp['anchor'], d, (400, 240), (80, 40)))
    view.save(os.path.join(folder, '3-final-composite.png'))

    with open(os.path.join(folder, 'README-AI.md'), 'w') as f:
        f.write(README.format(
            name=sp['name'], about=sp['about'], files='\n'.join(files), W=W, H=H,
            cx0=80 + cx0, cy0=40 + cy0, past=max(0, 80 + cx1 - 400), colours=len(colours),
            task=sp['task'].format(top=min(tops), right=max(rights) - 1)))
    print(f'{key}: {folder} ({W}x{H}, {len(sp["frames"])} frames, {len(colours)} colours, '
          f'trail cut at y={min(tops)} and x={max(rights) - 1} of the canvas)')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    for key in sys.argv[3:] or SPRITES:
        build(key, SPRITES[key], sys.argv[1], sys.argv[2])


if __name__ == '__main__':
    main()
