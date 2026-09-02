"""Sprite thumbnails for the layer workbench's SPRITES mode.

    python3 tools/layer-workbench/build_sprite_thumbs.py

Writes `thumbs/<tag>.png` (the decompressed, palettised tile sheet) and, when
one can be assembled, `thumbs/<tag>.anim.png` (a filmstrip of one animation,
composed from the `OAM_ENTRY` list in the sprite's own `.c`). It also folds each
thumbnail's metadata into `sprites.json`, under the `thumb` field of every
primary entry.

Why this is possible even though the AI is not run: a sprite's AI fixes its
pose, `bgPriority` and position at runtime -- which is what SPRITES mode needs
for depth decisions. Recognising the creature needs none of that: the graphics
(LZ77), the palette (BGR555) and every frame's `OAM_ENTRY` are static data in
`src/data/sprites/*.c`.

Known limits, all because the game is not run:
  - Primaries only. A secondary has no `sSpritesGraphicsPointers` entry (it
    inherits its parent's graphics slot), so its sheet cannot be resolved
    reliably without the room.
  - The OAM tile base is a VRAM address the runtime fixes. It is approximated
    with the lowest tile index the file uses. It is usually right; when it is
    not, the tile sheet still identifies the sprite.
  - Colour variants that share `*Gfx` and `*Pal` (yellow/red zoomer,
    purple/orange sova): the `*Pal` holds 2+ rows of 16 and the AI picks the row
    with `if (spriteId == PSPRITE_X) paletteRow = N`. That `if` is read from
    `src/sprites_AI/`. Row changes driven by state (hit flash, area tint,
    frozen) are not readable statically.
  - Multi-part bosses (Kraid, Ridley, Mecha Ridley) are assembled by the AI from
    several spawns; a single frame looks incomplete but usually still reads.

No dependencies: PNG is written by hand with the stdlib's zlib.
"""
import glob
import json
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SPRITE_C = os.path.join(ROOT, "src", "sprite.c")
SPRITES_DIR = os.path.join(ROOT, "src", "data", "sprites")
EXTRACTED = os.path.join(ROOT, "include")
SPRITES_JSON = os.path.join(HERE, "sprites.json")
THUMBS = os.path.join(HERE, "thumbs")

# Keywords that rank one animation over another: a resting pose first, turns and
# crashes (which do not say what the creature is) last.
ANIM_PREF = ["idle", "still", "stand", "main", "float", "hover", "swim", "fly",
             "fly", "walk", "crawl", "moving", "move", "ground", "wall", "open",
             "spit", "attack"]
ANIM_AVOID = ["turn", "crash", "crashing", "dying", "death", "die", "spawn",
              "explo", "hit", "corner", "edge", "slope", "unused", "part",
              "segment", "extra", "debris", "eyes", "warning", "bonking",
              "electricity"]

FPS = 60


# --------------------------------------------------------------------------- LZ77

def lz77_decompress(data):
    """GBA BIOS LZ77 (type 0x10). `data` starts with the 4-byte header."""
    if not data or data[0] != 0x10:
        raise ValueError("not LZ77 0x10")
    out_len = data[1] | (data[2] << 8) | (data[3] << 16)
    out = bytearray()
    i = 4
    while len(out) < out_len and i < len(data):
        flags = data[i]; i += 1
        for bit in range(8):
            if len(out) >= out_len:
                break
            if flags & (0x80 >> bit):
                if i + 1 >= len(data):
                    break
                b0, b1 = data[i], data[i + 1]; i += 2
                length = (b0 >> 4) + 3
                disp = ((b0 & 0x0F) << 8 | b1) + 1
                start = len(out) - disp
                for k in range(length):
                    out.append(out[start + k])
            else:
                if i >= len(data):
                    break
                out.append(data[i]); i += 1
    return bytes(out[:out_len])


def read_word_inc(path):
    """`.inc` shaped like `123u,45u,...` -> bytes (little-endian u32)."""
    txt = open(path, "r", encoding="utf-8", errors="replace").read()
    words = [int(m) for m in re.findall(r"(\d+)u?\b", txt)]
    return b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in words)


def read_half_inc(path):
    """u16 `.inc` -> list of ints."""
    txt = open(path, "r", encoding="utf-8", errors="replace").read()
    return [int(m) & 0xFFFF for m in re.findall(r"(\d+)u?\b", txt)]


def read_byte_inc(path):
    """u8 `.inc` -> bytes (some `*Gfx` arrays are declared `const u8`)."""
    txt = open(path, "r", encoding="utf-8", errors="replace").read()
    return bytes(int(m) & 0xFF for m in re.findall(r"(\d+)u?\b", txt))


# ---------------------------------------------------------------- PNG by hand

def write_png(path, width, height, rgba):
    """rgba: bytearray of width*height*4. RGBA8 PNG, no filters."""
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ------------------------------------------------------------- graphics/palette

def bgr555_to_rgba(c, transparent):
    r = (c & 0x1F) * 255 // 31
    g = ((c >> 5) & 0x1F) * 255 // 31
    b = ((c >> 10) & 0x1F) * 255 // 31
    return (r, g, b, 0 if transparent else 255)


def tile_pixels(gfx, tile_index):
    """16 colours, 8x8, 32 bytes per tile. Returns 64 palette indices."""
    off = tile_index * 32
    px = []
    for i in range(32):
        byte = gfx[off + i] if off + i < len(gfx) else 0
        px.append(byte & 0x0F)
        px.append(byte >> 4)
    return px


def blit_tile(dst, dw, dh, gfx, tile_index, dx, dy, pal, flip_x, flip_y):
    px = tile_pixels(gfx, tile_index)
    for ty in range(8):
        for tx in range(8):
            pi = px[ty * 8 + tx]
            if pi == 0:
                continue
            ox = dx + (7 - tx if flip_x else tx)
            oy = dy + (7 - ty if flip_y else ty)
            if 0 <= ox < dw and 0 <= oy < dh:
                r, g, b, _ = pal[pi] if pi < len(pal) else (255, 0, 255, 255)
                o = (oy * dw + ox) * 4
                dst[o:o + 4] = bytes((r, g, b, 255))


DIMS = {
    "OAM_DIMS_8x8": (1, 1), "OAM_DIMS_16x16": (2, 2), "OAM_DIMS_32x32": (4, 4),
    "OAM_DIMS_64x64": (8, 8), "OAM_DIMS_16x8": (2, 1), "OAM_DIMS_32x8": (4, 1),
    "OAM_DIMS_32x16": (4, 2), "OAM_DIMS_64x32": (8, 4), "OAM_DIMS_8x16": (1, 2),
    "OAM_DIMS_8x32": (1, 4), "OAM_DIMS_16x32": (2, 4), "OAM_DIMS_32x64": (4, 8),
}
FLIP = {"OAM_NO_FLIP": (0, 0), "OAM_X_FLIP": (1, 0), "OAM_Y_FLIP": (0, 1),
        "OAM_XY_FLIP": (1, 1)}

OAM_RE = re.compile(
    r"OAM_ENTRY(?:_MODE)?\s*\(\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(OAM_DIMS_\w+)\s*,"
    r"\s*(OAM_\w*FLIP)\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*(\d+)\s*,\s*(\d+)")


def parse_frame_entries(text, name):
    """The OAM_ENTRY items of `static const u16 <name>[...] = { n, ... };`."""
    m = re.search(re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if not m:
        return None
    out = []
    for e in OAM_RE.finditer(m.group(1)):
        x, y, dims, flip, tile, pal_row, _prio = e.groups()
        out.append({
            "x": int(x), "y": int(y),
            "w": DIMS[dims][0], "h": DIMS[dims][1],
            "tile": int(tile, 0),
            "flipx": FLIP[flip][0], "flipy": FLIP[flip][1],
            "palrow": int(pal_row),
        })
    return out


def _lcs(a, b):
    """Length of the longest common substring (to match group <-> type name)."""
    best = 0
    for i in range(len(a)):
        for j in range(i + best + 1, len(a) + 1):
            if a[i:j] in b:
                best = j - i
            else:
                break
    return best


def pick_anim(text, hint=""):
    """Choose a `FrameData s...Oam_<grp>[N]`. `hint` = the type name without
    `PSPRITE_`/`_`/lowercased: if a group resembles it (several drops or poses
    in one .c) it wins. Returns (group_name, frame_names, delays)."""
    h = re.sub(r"[^a-z0-9]", "", re.sub(r"^[ps]sprite_?", "", hint.lower()))
    cands = []
    for m in re.finditer(
            r"(?:const\s+)?struct\s+FrameData\s+(s\w+)\s*\[\d+\]\s*=\s*\{(.*?)\n\}\s*;",
            text, re.S):
        gname, body = m.group(1), m.group(2)
        frames = re.findall(r"\.pFrame\s*=\s*(s\w+)", body)
        timers = re.findall(r"\.timer\s*=\s*CONVERT_SECONDS\(([^)]+)\)", body)
        if not frames:
            continue
        delays = []
        for t in timers:
            try:
                delays.append(max(1, round(eval(t.replace("f", "")) * FPS)))
            except Exception:
                delays.append(6)
        while len(delays) < len(frames):
            delays.append(delays[-1] if delays else 6)
        low = gname.lower()
        grp = re.sub(r"^s\w*oam", "", low)
        grp = re.sub(r"[^a-z0-9]", "", grp)
        score = 0
        for i, kw in enumerate(ANIM_PREF):
            if kw in low:
                score = 100 - i
                break
        if any(kw in low for kw in ANIM_AVOID):
            score -= 50
        score += min(len(frames), 8)
        n = _lcs(h, grp) if (h and grp) else 0
        if n >= 4:
            score += 4 * n   # tie-break variants in one .c (drops, geruta)
        cands.append((score, gname, frames, delays[:len(frames)]))
    if not cands:
        return None
    cands.sort(key=lambda c: -c[0])
    return cands[0][1], cands[0][2], cands[0][3]


# ------------------------------------------------------------------- catalogue

def gfx_table():
    src = open(SPRITE_C, "r", encoding="utf-8", errors="replace").read()
    def grab(sym_re):
        m = re.search(r"static const \w+\* " + sym_re +
                      r"\[[^\]]*\]\s*=\s*\{(.*?)\n\};", src, re.S)
        return dict(re.findall(
            r"PSPRITE_OFFSET_FOR_GRAPHICS\((PSPRITE_\w+)\)\]\s*=\s*(\w+)", m.group(1)))
    return grab("sSpritesGraphicsPointers"), grab("sSpritesPalettePointers")


AI_DIR = os.path.join(ROOT, "src", "sprites_AI")
HUD_C = os.path.join(ROOT, "src", "data", "hud_data.c")

# sCommonSpritesGfx is copied to VRAM_OBJ + 0x800 -> tile base 0x40; its palette
# (sCommonSpritesPal, 6 rows of 16) to PALRAM_BASE + 0x240 -> OBJ row 2. Drops,
# the water jet and others draw from that always-resident block, not from an own
# `*Gfx` (their table entry is filler, sZeelaGfx).
COMMON_BASE_TILE = 0x40
COMMON_PAL_ROW0 = 2


def _common_cache(_c={}):
    if "gfx" not in _c:
        t = open(HUD_C, "r", encoding="utf-8", errors="replace").read()
        gi = include_path(t, "sCommonSpritesGfx", "gfx")
        pi = include_path(t, "sCommonSpritesPal", "pal")
        _c["gfx"] = read_word_inc(gi) if gi and os.path.isfile(gi) else b""
        _c["pal"] = read_half_inc(pi) if pi and os.path.isfile(pi) else []
    return _c["gfx"], _c["pal"]


def ai_map():
    """PSPRITE_* -> (ai_func, path of the AI .c)."""
    src = open(SPRITE_C, "r", encoding="utf-8", errors="replace").read()
    m = re.search(r"sPrimarySpritesAIPointers\[[^\]]*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    ai = dict(re.findall(r"\[(PSPRITE_\w+)\]\s*=\s*(\w+)", m.group(1))) if m else {}
    texts = {f: None for f in glob.glob(os.path.join(AI_DIR, "*.c"))}
    func_file = {}
    for name, func in ai.items():
        if func in func_file:
            continue
        for f in texts:
            if texts[f] is None:
                texts[f] = open(f, "r", encoding="utf-8", errors="replace").read()
            if re.search(r"\bvoid\s+" + re.escape(func) + r"\s*\(", texts[f]):
                func_file[func] = f
                break
    return {n: (fn, func_file.get(fn)) for n, fn in ai.items()}


def palette_rows():
    """PSPRITE_* -> fixed palette row its AI selects by `spriteId`.

    Several colour variants share `*Gfx` and `*Pal` (yellow/red zoomer,
    purple/orange sova): the `*Pal` holds 2+ rows of 16 and the AI does
      if (spriteId == PSPRITE_X) { paletteRow = N; }
    That is static and readable. Row changes driven by state (hit flash, area
    tint, frozen) are not: those need the AI running."""
    src = open(SPRITE_C, "r", encoding="utf-8", errors="replace").read()
    m = re.search(r"sPrimarySpritesAIPointers\[[^\]]*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    ai = dict(re.findall(r"\[(PSPRITE_\w+)\]\s*=\s*(\w+)", m.group(1))) if m else {}
    func_src = {}
    for f in glob.glob(os.path.join(AI_DIR, "*.c")):
        func_src[f] = None  # lazy
    def text_for(func):
        for f in glob.glob(os.path.join(AI_DIR, "*.c")):
            if func_src[f] is None:
                func_src[f] = open(f, "r", encoding="utf-8", errors="replace").read()
            if re.search(r"\bvoid\s+" + re.escape(func) + r"\s*\(", func_src[f]):
                return func_src[f]
        return ""
    out = {}
    for name, func in ai.items():
        t = text_for(func)
        if not t:
            continue
        mm = re.search(r"spriteId\s*==\s*" + re.escape(name) + r"\b[\s\S]{0,160}?"
                       r"(?:absolutePaletteRow|paletteRow)\s*=\s*(\d+)", t)
        if mm:
            out[name] = int(mm.group(1))
    return out


def sym_files():
    gfx, pal = {}, {}
    for f in glob.glob(os.path.join(SPRITES_DIR, "*.c")):
        t = open(f, "r", encoding="utf-8", errors="replace").read()
        for s in re.findall(r"const u32 (s\w+Gfx)\[", t):
            gfx[s] = f
        for s in re.findall(r"const u16 (s\w+Pal)\[", t):
            pal[s] = f
    return gfx, pal


def include_path(text, sym, suffix):
    """The path the array `sym` (gfx or pal) `#include`s."""
    m = re.search(re.escape(sym) + r"\s*\[[^\]]*\]\s*=\s*\{\s*#include\s+\"([^\"]+)\"",
                  text, re.S)
    if not m:
        return None
    return os.path.join(EXTRACTED, m.group(1).replace("/", os.sep))


def load_gfx(text, sym):
    inc = include_path(text, sym, "gfx")
    if not inc or not os.path.isfile(inc):
        return None
    u8 = re.search(r"const u8 " + re.escape(sym) + r"\b", text) is not None
    raw = read_byte_inc(inc) if u8 else read_word_inc(inc)
    if inc.endswith(".lz.inc"):
        return lz77_decompress(raw)
    return raw


def own_gfx_syms(text):
    """The `s*Gfx` symbols (u32 or u8) defined in this .c, in order."""
    return re.findall(r"const u(?:32|8) (s\w+Gfx)\s*\[", text)


def load_pal(text, sym):
    inc = include_path(text, sym, "pal")
    if not inc or not os.path.isfile(inc):
        return None
    halfs = read_half_inc(inc)
    return halfs


def _slug(name):
    return re.sub(r"[^a-z0-9]+", "_", name.replace("PSPRITE_", "").lower()).strip("_")


def render_sheet(gfx, pal, first_tile, last_tile):
    """Tiles [first,last) of the blob, on the 32-wide OBJ grid."""
    ntiles = max(1, len(gfx) // 32)
    first = max(0, first_tile - (first_tile % 32))
    last = min(ntiles, ((last_tile + 31) // 32) * 32) or ntiles
    span = max(1, last - first)
    cols = 32 if span > 32 else span
    rows = (span + cols - 1) // cols
    w, h = cols * 8, rows * 8
    buf = bytearray(w * h * 4)
    for t in range(first, last):
        blit_tile(buf, w, h, gfx, t, ((t - first) % cols) * 8,
                  ((t - first) // cols) * 8, pal, 0, 0)
    return buf, w, h, cols, span


def render_anim(gfx, pal, frames, base):
    ntiles = max(1, len(gfx) // 32)
    minx = min((e["x"] for fr in frames for e in fr), default=-8)
    miny = min((e["y"] for fr in frames for e in fr), default=-8)
    maxx = max((e["x"] + e["w"] * 8 for fr in frames for e in fr), default=8)
    maxy = max((e["y"] + e["h"] * 8 for fr in frames for e in fr), default=8)
    cw, ch = max(1, maxx - minx), max(1, maxy - miny)
    ox, oy = -minx, -miny
    strip_w = cw * len(frames)
    strip = bytearray(strip_w * ch * 4)
    for fi, fr in enumerate(frames):
        for e in fr:
            tw = e["w"]
            for row in range(e["h"]):
                for col in range(tw):
                    ti = (e["tile"] - base) + row * 32 + col   # 2D OBJ grid
                    if ti < 0 or ti >= ntiles:
                        continue
                    sx = e["x"] + ox + fi * cw + \
                        (tw - 1 - col if e["flipx"] else col) * 8
                    sy = e["y"] + oy + \
                        (e["h"] - 1 - row if e["flipy"] else row) * 8
                    blit_tile(strip, strip_w, ch, gfx, ti, sx, sy,
                              pal, e["flipx"], e["flipy"])
    return strip, strip_w, ch, cw, ox, oy


def build():
    if not os.path.isfile(SPRITES_JSON):
        sys.exit("sprites.json missing; run build_sprites.py first")
    catalog = json.load(open(SPRITES_JSON, "r", encoding="utf-8"))
    g_tab, p_tab = gfx_table()
    g_file, p_file = sym_files()
    p_row = palette_rows()
    ai = ai_map()
    os.makedirs(THUMBS, exist_ok=True)

    done = {}          # tag -> info, to avoid redoing work across variants
    made = 0
    for entry in catalog["primary"]:
        name = entry["name"]
        entry.pop("thumb", None)
        gsym = g_tab.get(name)
        psym = p_tab.get(name)
        prow = p_row.get(name, 0)

        # A sprite's OAM lives in its data .c, which matches the AI .c by name --
        # not always the `*Gfx` .c (drops point at filler sZeelaGfx but are
        # actually drawn from the common block).
        _ai_func, ai_file = ai.get(name, (None, None))
        data_file = None
        if ai_file:
            cand = os.path.join(SPRITES_DIR, os.path.basename(ai_file))
            if os.path.isfile(cand) and "Oam" in open(cand, encoding="utf-8",
                                                       errors="replace").read():
                data_file = cand
        # sZeelaGfx is the repo's filler for "no own graphics": it does not count
        # as a data .c unless the sprite really is a zeela.
        if not data_file and gsym and gsym in g_file and gsym != "sZeelaGfx":
            data_file = g_file[gsym]
        if not data_file:
            continue
        data_text = open(data_file, "r", encoding="utf-8", errors="replace").read()

        anim = pick_anim(data_text, hint=name)
        frames, all_tiles = [], []
        if anim:
            for fn in anim[1]:
                ents = parse_frame_entries(data_text, fn)
                frames.append(ents or [])
                all_tiles += [e["tile"] for e in (ents or [])]
        if not all_tiles:
            continue      # no usable OAM (invisible event trigger)

        use_common = max(all_tiles) < 0x200
        if use_common:
            gfx, pal_src = _common_cache()
            if not gfx:
                continue
            base = COMMON_BASE_TILE
            rowf = max((e["palrow"] for fr in frames for e in fr),
                       default=COMMON_PAL_ROW0)
            po = max(0, rowf - COMMON_PAL_ROW0) * 16
            if po + 16 > len(pal_src):
                po = 0
            tag = "common.%s" % _slug(name)
        else:
            # Graphics source, by reliability:
            #  1) an s*Gfx defined in the sprite's own data .c (the one the OAM
            #     matches). sZeelaGfx is the repo filler for "no own graphics",
            #     so it does not count unless the sprite really is a zeela.
            #  2) the table entry, if it is not that filler.
            hint = re.sub(r"[^a-z0-9]", "", _slug(name))
            owns = own_gfx_syms(data_text)
            gtext, gpick, ppick = data_text, None, None
            if owns:
                owns.sort(key=lambda s: -_lcs(hint, s.lower()))
                gpick = owns[0]
                stem = gpick[:-3]           # sFooGfx -> sFoo
                ppick = stem + "Pal" if (stem + "Pal") in data_text else None
                if ppick is None:
                    pm = re.search(r"const u16 (s\w+Pal)\s*\[", data_text)
                    ppick = pm.group(1) if pm else None
            elif gsym and gsym in g_file and gsym != "sZeelaGfx":
                gtext = open(g_file[gsym], "r", encoding="utf-8",
                             errors="replace").read()
                gpick, ppick = gsym, psym
            elif (gsym in g_file and
                  os.path.basename(data_file) == os.path.basename(g_file[gsym])):
                gpick, ppick = gsym, psym    # really is a zeela sprite

            if not gpick:
                continue                     # invisible trigger: no thumb
            gfx = load_gfx(gtext, gpick)
            if gfx is None:
                continue
            pal_src = (load_pal(gtext, ppick) if ppick else None) or \
                ([0] + [0x7FFF] * 15)
            po = prow * 16 if (prow + 1) * 16 <= len(pal_src) else 0
            ftiles = [t for t in
                      (int(e.group(5), 0) for e in OAM_RE.finditer(data_text))
                      if t >= 0x200]
            base = (min(ftiles) if ftiles else min(all_tiles)) & ~31
            tag = gpick + ("" if prow == 0 else ".p%d" % prow)
            if owns and len(owns) > 1:
                tag += "." + _slug(name)

        if tag in done:
            entry["thumb"] = done[tag]
            continue

        pal = [bgr555_to_rgba(pal_src[po + i] if po + i < len(pal_src) else 0,
                              i == 0) for i in range(16)]

        lo = min(all_tiles) - base
        hi = max(all_tiles) - base + 4
        sheet, sw, sh, cols, ntiles = render_sheet(gfx, pal, lo, hi)
        sheet_name = "thumbs/%s.png" % tag
        write_png(os.path.join(HERE, sheet_name), sw, sh, sheet)
        made += 1
        info = {"sheet": sheet_name, "sw": sw, "sh": sh, "cols": cols,
                "tiles": ntiles, "common": use_common}

        if any(frames):
            strip, strip_w, ch, cw, ox, oy = render_anim(gfx, pal, frames, base)
            anim_name = "thumbs/%s.anim.png" % tag
            write_png(os.path.join(HERE, anim_name), strip_w, ch, strip)
            info["anim"] = {"img": anim_name, "n": len(frames), "cw": cw,
                            "ch": ch, "ox": ox, "oy": oy, "group": anim[0],
                            "delays": [round(d) for d in anim[2]]}

        done[tag] = info
        entry["thumb"] = info

    json.dump(catalog, open(SPRITES_JSON, "w", encoding="utf-8"),
              ensure_ascii=False, separators=(",", ":"))
    withthumb = sum(1 for e in catalog["primary"] if e.get("thumb"))
    withanim = sum(1 for e in catalog["primary"] if (e.get("thumb") or {}).get("anim"))
    print("thumbs: %d new sheets, %d/%d primaries with a thumbnail, %d animated"
          % (made, withthumb, len(catalog["primary"]), withanim))


if __name__ == "__main__":
    build()
