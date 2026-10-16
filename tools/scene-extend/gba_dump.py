"""Draw a GBA screen dump's layers (text BGs and sprites) as RGBA pictures.

A dump is the files `mzm-dump-NN-{io,vram,bgpltt,objpltt,oam}.bin` the DEBUG -> SCREEN DUMP
writes (docs/3ds-debug-tools.md); load() takes their common prefix. Colours are the GBA's
BGR555 widened to 8 bits per channel (c << 3), the same values every package uses, so a
picture drawn from two dumps of one scene lines up pixel for pixel.

The pictures are the game's art: keep them out of the repository.
"""
import struct

from PIL import Image

BG_SIZES = [(256, 256), (512, 256), (256, 512), (512, 512)]
# OBJ sizes by shape (square, wide, tall) and size bits, GBATEK.
OBJ_SIZES = [[(8, 8), (16, 16), (32, 32), (64, 64)],
             [(16, 8), (32, 8), (32, 16), (64, 32)],
             [(8, 16), (8, 32), (16, 32), (32, 64)]]


class Dump:
    def __init__(self, prefix):
        def read(name):
            with open(f'{prefix}-{name}', 'rb') as f:
                return f.read()
        self.io = read('io.bin')
        self.vram = read('vram.bin')
        self.bgpal = read('bgpltt.bin')
        self.objpal = read('objpltt.bin')
        self.oam = read('oam.bin')

    def reg(self, offset):
        return struct.unpack_from('<H', self.io, offset)[0]

    @property
    def dispcnt(self):
        return self.reg(0)

    def bg_enabled(self, bg):
        return bool(self.dispcnt & (0x100 << bg))

    def bg_cnt(self, bg):
        return self.reg(8 + 2 * bg)

    def bg_size(self, bg):
        return BG_SIZES[self.bg_cnt(bg) >> 14]

    def bg_scroll(self, bg):
        """(hofs, vofs) masked to the BG's size."""
        w, h = self.bg_size(bg)
        return self.reg(0x10 + 4 * bg) & (w - 1), self.reg(0x12 + 4 * bg) & (h - 1)


def colour(pal, i):
    c = struct.unpack_from('<H', pal, i * 2)[0]
    return ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3, 255)


def backdrop(d):
    return colour(d.bgpal, 0)


def render_bg(d, bg):
    """The whole BG map as VRAM holds it, transparent where colour 0."""
    cnt = d.bg_cnt(bg)
    cb, sb = ((cnt >> 2) & 3) * 0x4000, ((cnt >> 8) & 31) * 0x800
    c256 = (cnt >> 7) & 1
    w, h = d.bg_size(bg)
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    px = img.load()
    v = d.vram
    for ty in range(h // 8):
        for tx in range(w // 8):
            block = (tx // 32) + (ty // 32) * (w // 256)
            e = struct.unpack_from('<H', v, sb + block * 0x800 + ((ty % 32) * 32 + (tx % 32)) * 2)[0]
            t, hf, vf, pb = e & 0x3ff, (e >> 10) & 1, (e >> 11) & 1, e >> 12
            for y in range(8):
                for x in range(8):
                    sx, sy = (7 - x if hf else x), (7 - y if vf else y)
                    if c256:
                        o = cb + t * 64 + sy * 8 + sx
                        i = v[o] if o < 0x10000 else 0
                    else:
                        o = cb + t * 32 + sy * 4 + sx // 2
                        b = v[o] if o < 0x10000 else 0
                        i = (b >> (4 * (sx & 1))) & 15
                        i = pb * 16 + i if i else 0
                    if i:
                        px[tx * 8 + x, ty * 8 + y] = colour(d.bgpal, i)
    return img


def bg_frame(d, bg, full=None):
    """The 240x160 the GBA shows of the BG at its scroll (wrapping)."""
    full = full or render_bg(d, bg)
    w, h = full.size
    ox, oy = d.bg_scroll(bg)
    out = Image.new('RGBA', (240, 160), (0, 0, 0, 0))
    for y0 in range(0, 160 + h, h):
        for x0 in range(0, 240 + w, w):
            out.alpha_composite(full, (x0 - ox, y0 - oy))
    return out


def render_sprites(d, size=(240, 160), origin=(0, 0)):
    """The sprites of the dump (OBJ priority ignored), in a canvas where the GBA frame's
    top-left is at `origin`. Affine (rotated / scaled) sprites are left out."""
    img = Image.new('RGBA', size, (0, 0, 0, 0))
    if not d.dispcnt & 0x1000:
        return img
    px = img.load()
    v = d.vram
    one_d = bool(d.dispcnt & 0x40)
    for n in range(127, -1, -1):
        a0, a1, a2 = struct.unpack_from('<3H', d.oam, n * 8)  # 8-byte slots, last u16 unused
        affine, hidden = (a0 >> 8) & 1, (a0 >> 9) & 1
        if affine or hidden:
            continue
        shape, sz = a0 >> 14, a1 >> 14
        if shape == 3:
            continue
        w, h = OBJ_SIZES[shape][sz]
        y, x = a0 & 0xff, a1 & 0x1ff
        if y >= 160:
            y -= 256
        if x >= 240:
            x -= 512
        c256 = (a0 >> 13) & 1
        hf, vf = (a1 >> 12) & 1, (a1 >> 13) & 1
        tile, pb = a2 & 0x3ff, a2 >> 12
        for yy in range(h):
            for xx in range(w):
                sx, sy = (w - 1 - xx if hf else xx), (h - 1 - yy if vf else yy)
                tx, ty = sx // 8, sy // 8
                if c256:
                    t = tile + (ty * (w // 8) * 2 + tx * 2 if one_d else ty * 32 + tx * 2)
                    o = 0x10000 + (t & 0x3ff) * 32 + (sy % 8) * 8 + sx % 8
                    i = v[o] if o < len(v) else 0
                else:
                    t = tile + (ty * (w // 8) + tx if one_d else ty * 32 + tx)
                    o = 0x10000 + (t & 0x3ff) * 32 + (sy % 8) * 4 + (sx % 8) // 2
                    b = v[o] if o < len(v) else 0
                    i = (b >> (4 * (sx & 1))) & 15
                    i = pb * 16 + i if i else 0
                if not i:
                    continue
                X, Y = origin[0] + x + xx, origin[1] + y + yy
                if 0 <= X < size[0] and 0 <= Y < size[1]:
                    px[X, Y] = colour(d.objpal, i)
    return img
