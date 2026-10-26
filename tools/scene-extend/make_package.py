#!/usr/bin/env python3
"""Delivery packages for the generative AI: one folder per scene with the scene's background
picture in a bigger canvas and a README the AI reads (docs/3ds-wide-scene-extension-plan.md,
section 8b).

    make_package.py DUMPS_DIR OUT_DIR [SCENE ...]

DUMPS_DIR holds the screen dumps named in SCENES below (`<dir>/<n>-io.bin`, ...: the files
`mzm-dump-NN-*` of DEBUG -> SCREEN DUMP, renamed per capture). OUT_DIR gets `mzm-<scene>/`
for each scene (default: all of them) with:

  1-template-WxH.png        the canvas: the picture at its place, transparent everywhere else
  1-template-WxH-black.png  the same with black instead of transparent
  2-original-picture.png    the picture alone
  3-final-composite.png     what the player sees (every layer and sprite), at its place
  4-do-not-paint.png        magenta = picture; the other pixels are what the game draws over it
  README-AI.md              the task, the numbers and the rules

Coordinates are BG pixels of the scene's background BG (its own scroll space). A scene that
streams its graphics (the title) is put together from the rows each dump's frame shows, the
only ones VRAM is sure to hold the picture in.

All of it is the game's art: keep OUT_DIR out of the repository.
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gba_dump import Dump, backdrop, bg_frame, render_bg, render_sprites  # noqa: E402

# picture: BG rect (x0, y0, x1, y1) of the picture; canvas: BG rect of the canvas.
# dumps: captures to take the picture from; final: the one the composite shows.
# frame_only: take from each dump only what its frame shows (streamed scenes).
SCENES = {
    'title': dict(
        name="Metroid: Zero Mission title screen",
        bg=3, dumps=['title/01', 'title/02', 'title/03'], final='title/03', frame_only=True,
        picture=(0, 0, 240, 360), canvas=(-80, -40, 320, 400),
        about="When the title appears the game scrolls this tall background down, from deep space "
              "(stars, a blue nebula, purple haze) to the rocky surface of planet Zebes at the bottom, "
              "where it stops and the logo shows. The canvas is the whole scroll with room around it, "
              "so that at the end the logo stays in the middle of the 400x240 screen.",
        sides=[
            "Left and right, along the whole height: more of the same space (stars of the same sizes "
            "and colours, the blue nebula and the purple haze continuing past the edges), and at the "
            "bottom more of the rocky ground, at the same height and with the same horizon line.",
            "Above: more deep space, darker, with stars.",
            "Below: the rocky ground goes on down to the bottom edge of the canvas (more rocks, a bit "
            "darker as it goes down). No horizon or sky down there.",
        ],
        over="the METROID ZERO MISSION logo, the menu text (JUGAR, Idioma, (c) 2004 Nintendo) and "
             "the sparkles: another layer and sprites the game draws over the background"),
    'intro-space': dict(
        name="intro, Samus's ship in deep space",
        bg=0, dumps=['intro-110/1'], final='intro-110/1',
        picture=(0, 0, 240, 160), canvas=(-80, -40, 320, 200),
        about="Deep space: a dark starfield with blue and purple nebulae. Samus's gunship flies "
              "towards the camera in the middle (a sprite, not part of the background).",
        sides=["All around: more of the starfield and nebulae, stars of the same sizes and "
               "colours; the nebulae continue past the edges and fade into black space."],
        over="Samus's gunship (sprites)"),
    'file-select': dict(
        name="file select (\"Datos de Samus\")",
        bg=3, dumps=['file-select/1'], final='file-select/1',
        picture=(0, 0, 240, 160), canvas=(-80, -40, 320, 200),
        about="Backdrop of the save-file menu: the face of a Chozo statue, its hand at the brow, "
              "in dark teal and green, over a wall of carved square spiral glyphs (a circuit-like "
              "pattern in the same teal); his shoulders and body fill the lower part. The whole picture is shaded with fine horizontal "
              "scanlines that belong to the art. The canvas is the 400x240 screen with the "
              "picture in the middle.",
        sides=[
            "Left and right: in the top half the wall of carved glyphs goes on (the same square "
            "spirals and channels, same size and spacing, same dark teal). Lower down, the "
            "statue's shoulders and rocky body, which the original cuts at its left and right edges "
            "and at the bottom, continue naturally outwards. Do not add another face.",
            "Above: more of the glyph wall. Below: the statue's body and the dark rocks under it "
            "go on down to the bottom edge, darker as they go down.",
            "Keep the horizontal scanlines of the original, same thickness and spacing, across "
            "the added areas.",
        ],
        over="the menu (DATOS DE SAMUS, the three save files with their times and areas, COPIAR / "
             "BORRAR / OPCIONES): another layer and sprites the game draws over the background"),
    'intro-cockpit': dict(
        name="intro, Samus in her ship's cockpit",
        bg=0, dumps=['intro-160/1'], final='intro-160/1',
        picture=(0, -8, 256, 168), canvas=(-72, -40, 328, 200),
        about="Samus in her orange Power Suit at the controls of her ship while it flies at great "
              "speed: the picture is full of long diagonal speed streaks in red, orange and yellow, "
              "sloping up to the right, with some dark metal hull at the right. The game shakes this "
              "picture by a pixel, so the canvas keeps its real size (256x176). What matters in the "
              "new area is the feeling of speed and motion, not the ship.",
        sides=[
            "Almost everything you add should be speed and motion: long, hard-edged diagonal streaks "
            "at the same slope, thickness and spacing as the original's, in the same reds, oranges "
            "and yellows, brighter and sparser away from Samus and darker in the shadows. They run "
            "off the edges of the canvas in the same direction, as if the whole scene were rushing "
            "past.",
            "Do not draw the ship or the cockpit in detail: no panels, controls, windows or seats. "
            "Where the original shows the dark hull at the right, let it fade into dark streaked "
            "shadow instead of adding more machinery. Where Samus's body or arm cannon is cut at "
            "the edge, continue only what is needed so the cut is invisible; no second Samus.",
            "Same pixel-art look as the original: hard 1-pixel edges, no soft glow, no sparks, "
            "no smooth gradients.",
        ],
        over="nothing (the whole picture is background)"),
    'intro-zebes': dict(
        name="intro, planet Zebes from orbit",
        bg=0, dumps=['intro-600/1'], final='intro-600/1',
        picture=(0, 0, 256, 160), canvas=(-88, -40, 328, 200),
        about="Planet Zebes seen from orbit: its yellow-green surface curves across the picture with "
              "a strip of starry space above the horizon. The game pans this picture 16 px to the "
              "left while Samus's ship dives towards the planet with a red flame trail (sprites), so "
              "the canvas is 16 px wider than the screen.",
        sides=[
            "Left and right: the planet's curve and surface continue (same colours, same curvature, "
            "so the horizon stays one smooth arc), with the space above it.",
            "Above: more starry space above the horizon. Below: more of the planet's surface.",
        ],
        over="Samus's ship and its red flame trail (sprites)"),
    'intro-motherbrain': dict(
        name="intro, Mother Brain's tank",
        bg=0, dumps=['intro-960/1'], final='intro-960/1',  # VRAM holds the whole map at once
        picture=(0, 0, 240, 256), canvas=(-80, 0, 320, 256),
        about="Mother Brain's glass tank in Tourian, all red: the tank and its rim at the top, the "
              "organic machinery, pipes and cables around it, the tank's base below. The game "
              "scrolls this picture up from its bottom to its top; it is as tall as the canvas, so "
              "only the sides are missing.",
        sides=["Left and right, along the whole height: more of the red machinery, pipes and "
               "cables of the room around the tank. Do not add another tank."],
        over="nothing (the whole picture is background)"),
}

README = """# Extend a Game Boy Advance background: {name}

Pixel-art background from Metroid: Zero Mission (Game Boy Advance, 1 pixel = 1 game pixel). It
has to be made bigger by drawing the missing area around it: the game is shown on a wider screen
(400x240 instead of the GBA's 240x160) and the new area must look like it was always part of the
picture.

{about}

## Important: continue the picture, do not frame it
This is outpainting: the result is ONE continuous picture that fills the whole canvas from edge to
edge, in which the original is just the middle part. Do NOT draw a border, frame, black outline,
shadow, gap or "picture inside a picture" around the original. Do NOT shrink, copy, tile or mirror
the original into the new area. Whatever crosses the original's edge (lines, streaks, shapes,
colours) has to carry on across it with no break, and the area next to the edge must look like
the same drawing, not like a different one.

## Files
- `1-template-{W}x{H}.png`: **the canvas to fill.** The original is at x={X}..{X2}, y={Y}..{Y2};
  everything else is transparent and is what you must draw. `1-template-{W}x{H}-black.png` is the
  same canvas with black instead of transparent, if your tool prefers it: the black is not part of
  the picture, it is the area to fill (do not keep it as a frame).
- `1-template-{W}x{H}-extended.png`: **start from this one if the others make you draw a frame.** The
  new area is only the original's edge pixels stretched outwards, a rough hint of where the lines
  and colours continue. Repaint all of it as real detail; the original stays as it is.
- `2-original-picture.png`: the original alone ({w}x{h}), for reference.
- `3-final-composite.png`: what the player sees (the background plus everything the game draws on
  top), at its place in the canvas. Context only.
- `4-do-not-paint.png`: magenta is the background; the other pixels are {over}. Do not paint them.

## What to add
{amounts}

{sides}

## Rules
1. Do not change a single pixel inside the original rectangle. It is the game's own art and is
   drawn over yours; any change shows as a seam.
2. Use only colours that already appear in the original ({colours} colours). No anti-aliasing,
   gradients, blur, dithering the original does not use, or new hues.
3. Same pixel scale and style: hard 1-pixel edges, the same outlines and shading steps.
4. Continue what touches the original's edges so the seams are invisible. Add more of the same
   kind of elements; no new objects, characters or text.
5. Do not repeat or mirror the original.
6. Do not paint anything from `3-final-composite.png` or `4-do-not-paint.png` that is not
   background ({over}).
7. Output: a PNG exactly {W}x{H}, no transparency left, no scaling, no smoothing. Give 2 or 3
   variants as separate files.
"""


def pixel(img, x, y):
    return img.getpixel((x % img.width, y % img.height))


def build(scene, sc, dumps_dir, out_dir):
    cx0, cy0, cx1, cy1 = sc['canvas']
    px0, py0, px1, py1 = sc['picture']
    W, H, w, h = cx1 - cx0, cy1 - cy0, px1 - px0, py1 - py0
    bg = sc['bg']

    # The picture, over the backdrop colour (what the GBA shows where the BG is transparent).
    pic = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    got = Image.new('1', (w, h), 0)
    for name in sc['dumps']:
        d = Dump(os.path.join(dumps_dir, name))
        full = render_bg(d, bg)
        ox, oy = d.bg_scroll(bg)
        bw, bh = full.size
        base = Image.new('RGBA', full.size, backdrop(d))
        base.alpha_composite(full)
        for y in range(h):
            for x in range(w):
                bx, by = px0 + x, py0 + y
                if sc.get('frame_only') and ((bx - ox) % bw >= 240 or (by - oy) % bh >= 160):
                    continue
                pic.putpixel((x, y), pixel(base, bx, by))
                got.putpixel((x, y), 1)
    missing = sum(1 for v in got.get_flattened_data() if not v)
    if missing:
        print(f'{scene}: {missing} picture pixels no dump showed (left transparent)')

    # The final frame as the player sees it, and the layers over the picture.
    d = Dump(os.path.join(dumps_dir, sc['final']))
    ox, oy = d.bg_scroll(bg)
    # The frame's top-left in BG coords: the copy (the BG wraps) whose centre is nearest the
    # picture's centre.
    bw, bh = d.bg_size(bg)
    fx = ox + round(((px0 + px1) / 2 - (ox + 120)) / bw) * bw
    fy = oy + round(((py0 + py1) / 2 - (oy + 80)) / bh) * bh
    at = (fx - cx0, fy - cy0)
    frame = Image.new('RGBA', (240, 160), backdrop(d))
    over = Image.new('RGBA', (240, 160), (0, 0, 0, 0))
    cnts = [(d.bg_cnt(b) & 3, -b, b) for b in range(4) if d.bg_enabled(b) and (d.dispcnt & 7) == 0]
    for _, _, b in sorted(cnts, reverse=True):
        layer = bg_frame(d, b)
        frame.alpha_composite(layer)
        if b != bg:
            over.alpha_composite(layer)
    sprites = render_sprites(d)
    frame.alpha_composite(sprites)
    over.alpha_composite(sprites)

    folder = os.path.join(out_dir, f'mzm-{scene}')
    os.makedirs(folder, exist_ok=True)
    template = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    template.alpha_composite(pic, (px0 - cx0, py0 - cy0))
    template.save(os.path.join(folder, f'1-template-{W}x{H}.png'))
    ext = np.pad(np.array(pic.convert('RGB')), ((Y_ := py0 - cy0, H - (py0 - cy0) - h),
                                                (px0 - cx0, W - (px0 - cx0) - w), (0, 0)), mode='edge')
    Image.fromarray(ext).save(os.path.join(folder, f'1-template-{W}x{H}-extended.png'))
    black = Image.new('RGBA', (W, H), (0, 0, 0, 255))
    black.alpha_composite(pic, (px0 - cx0, py0 - cy0))
    black.convert('RGB').save(os.path.join(folder, f'1-template-{W}x{H}-black.png'))
    pic.save(os.path.join(folder, '2-original-picture.png'))
    comp = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    comp.alpha_composite(pic, (px0 - cx0, py0 - cy0))
    comp.alpha_composite(frame, at)
    comp.save(os.path.join(folder, '3-final-composite.png'))
    dnp = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    dnp.paste(Image.new('RGBA', (w, h), (255, 0, 255, 255)), (px0 - cx0, py0 - cy0))
    dnp.alpha_composite(over, at)
    dnp.save(os.path.join(folder, '4-do-not-paint.png'))

    X, Y = px0 - cx0, py0 - cy0
    left, right, top, bottom = X, W - X - w, Y, H - Y - h
    parts = [f'{n} px {side}' for n, side in ((left, 'to the left'), (right, 'to the right'),
                                               (top, 'above'), (bottom, 'below')) if n > 0]
    colours = len({c for c in pic.get_flattened_data() if c[3]})
    with open(os.path.join(folder, 'README-AI.md'), 'w') as f:
        f.write(README.format(
            name=sc['name'], about=sc['about'], W=W, H=H, X=X, X2=X + w - 1, Y=Y, Y2=Y + h - 1,
            w=w, h=h, over=sc['over'], colours=colours,
            amounts='Canvas {}x{}, original {}x{}: add {}.'.format(W, H, w, h, ', '.join(parts)),
            sides='\n'.join(f'- {s}' for s in sc['sides'])))
    print(f'{scene}: {folder} ({W}x{H}, picture {w}x{h} at {X},{Y}, {colours} colours)')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    dumps_dir, out_dir = sys.argv[1], sys.argv[2]
    for scene in sys.argv[3:] or SCENES:
        build(scene, SCENES[scene], dumps_dir, out_dir)


if __name__ == '__main__':
    main()
