#!/usr/bin/env python3
"""Offline per-BG-layer / per-sprite decoder for the L+R+X hardware dump.

PlatformGpu3DS_DumpScreens() (platform/3ds/source/platform_gpu_3ds.c) already
writes the raw emulated GBA memory to the SD card on L+R+X: mzm-dump-vram.bin,
mzm-dump-io.bin, mzm-dump-bgpltt.bin, mzm-dump-objpltt.bin, mzm-dump-oam.bin
(retrieve them over the same FTP server used by `make ftp`, from sdmc:/3ds/).

Rather than adding a second on-console dump feature (which would need a
rebuild+redeploy to iterate on), this reconstructs each BG layer (0-3) as its
own PNG straight from that existing raw dump -- entire tilemap, not just the
currently-scrolled-into-view portion, so tile data loaded into a layer but
scrolled off-screen (or never composited due to a renderer bug) is still
visible. It mirrors the exact addressing logic of CollectBgLayer() in
platform/3ds/source/port_gpu_renderer.c so "what this tool shows" matches
"what the renderer would collect" tile-for-tile. Also decodes the OBJ/sprite
layer straight from OAM into its own composited PNG at actual screen
position, and does NOT filter by DISPCNT enable bits -- disabled layers are
still decoded and labeled, precisely so you can tell "layer has data but
isn't enabled/composited" apart from "layer genuinely has no data".

Usage:
    python3 decode_layer_dump.py <dump_dir> <out_dir>

Requires: Pillow (pip install pillow).
"""
import os
import struct
import sys

from PIL import Image

VRAM_SIZE = 0x18000
BG_TILE_W = 8
GBA_SCREEN_W = 240
GBA_SCREEN_H = 160

OBJ_SHAPE_SIZE = {
    # (shape, size) -> (width_px, height_px), matches kObjWidths/kObjHeights
    # in port_gpu_renderer.c / mode1.c.
    (0, 0): (8, 8), (0, 1): (16, 16), (0, 2): (32, 32), (0, 3): (64, 64),
    (1, 0): (16, 8), (1, 1): (32, 8), (1, 2): (32, 16), (1, 3): (64, 32),
    (2, 0): (8, 16), (2, 1): (8, 32), (2, 2): (16, 32), (2, 3): (32, 64),
}


def bgr555_to_rgb(word):
    r = (word & 0x1F) << 3
    g = ((word >> 5) & 0x1F) << 3
    b = ((word >> 10) & 0x1F) << 3
    return (r, g, b)


def load_palette(path):
    data = open(path, 'rb').read()
    words = struct.unpack('<%dH' % (len(data) // 2), data)
    return [bgr555_to_rgb(w) for w in words]


def decode_tile_pixel(vram, byte_offset, bpp8, tx, ty):
    """Returns palette index (0 = transparent) for pixel (tx,ty) within an
    8x8 tile whose top-left is at byte_offset in VRAM."""
    if bpp8:
        return vram[byte_offset + ty * 8 + tx]
    b = vram[byte_offset + ty * 4 + tx // 2]
    return (b >> 4) if (tx & 1) else (b & 0xF)


def decode_bg_layer(vram, io, pal, bg_index, label, out_dir):
    bgcnt = io[0x08 + bg_index * 2] | (io[0x09 + bg_index * 2] << 8)
    priority = bgcnt & 3
    char_base = ((bgcnt >> 2) & 3) * 0x4000
    bpp8 = bool((bgcnt >> 7) & 1)
    screen_base = ((bgcnt >> 8) & 0x1F) * 0x800
    size_flag = (bgcnt >> 14) & 3
    map_w_tiles = 64 if (size_flag & 1) else 32
    map_h_tiles = 64 if (size_flag & 2) else 32
    blocks_per_row = map_w_tiles // 32
    bytes_per_tile = 64 if bpp8 else 32
    dispcnt = io[0] | (io[1] << 8)
    enabled = bool(dispcnt & (1 << (8 + bg_index)))

    img = Image.new('RGBA', (map_w_tiles * 8, map_h_tiles * 8), (0, 0, 0, 0))
    px = img.load()
    any_opaque = False

    for tile_row in range(map_h_tiles):
        screen_block_y = tile_row // 32
        local_row = tile_row % 32
        for tile_col in range(map_w_tiles):
            screen_block_x = tile_col // 32
            local_col = tile_col % 32
            screen_block_index = screen_block_x + screen_block_y * blocks_per_row
            map_addr = screen_base + screen_block_index * 0x800 + (local_row * 32 + local_col) * 2
            entry = vram[map_addr] | (vram[map_addr + 1] << 8)
            tile_id = entry & 0x3FF
            hflip = bool(entry & 0x0400)
            vflip = bool(entry & 0x0800)
            pal_bank = (entry >> 12) & 0xF
            byte_offset = char_base + tile_id * bytes_per_tile
            if byte_offset + bytes_per_tile > VRAM_SIZE:
                continue
            for ty in range(8):
                sy = 7 - ty if vflip else ty
                for tx in range(8):
                    sx = 7 - tx if hflip else tx
                    idx = decode_tile_pixel(vram, byte_offset, bpp8, sx, sy)
                    if idx == 0:
                        continue
                    any_opaque = True
                    color_idx = idx if bpp8 else (pal_bank * 16 + idx)
                    if color_idx >= len(pal):
                        continue
                    r, g, b = pal[color_idx]
                    px[tile_col * 8 + tx, tile_row * 8 + ty] = (r, g, b, 255)

    status = "ENABLED" if enabled else "disabled"
    data_status = "HAS_DATA" if any_opaque else "empty"
    fname = f"{label}_bg{bg_index}_{status}_{data_status}_prio{priority}.png"
    img.save(os.path.join(out_dir, fname))
    print(f"BG{bg_index}: dispcnt {status}, priority={priority}, bpp8={bpp8}, "
          f"map={map_w_tiles}x{map_h_tiles} tiles, charBase=0x{char_base:04X}, "
          f"screenBase=0x{screen_base:04X} -> {data_status} -> {fname}")


def decode_obj_layer(vram, oam, objpal, label, out_dir):
    img = Image.new('RGBA', (GBA_SCREEN_W, GBA_SCREEN_H), (0, 0, 0, 0))
    px = img.load()
    any_opaque = False
    active_count = 0

    for i in range(128):
        off = i * 8
        attr0, attr1, attr2 = struct.unpack_from('<HHH', oam, off)
        y = attr0 & 0xFF
        if y >= 160 and ((attr0 >> 8) & 3) != 3:
            # y>=160 wraps as negative on real hardware; but the common
            # "disabled" sentinel used by this codebase is y=255 with the
            # rotation/scaling+double-size bits clear and mode!=affine --
            # approximate: skip only the canonical all-0xFFFF-ish disabled
            # slots seen in practice (also covers attr0 bit9 OBJ-disable).
            pass
        rs_flag = (attr0 >> 8) & 1
        disable_or_double = (attr0 >> 9) & 1
        if not rs_flag and disable_or_double:
            continue  # OBJ disabled (non-affine "hidden" bit)
        obj_mode = (attr0 >> 10) & 3
        shape = (attr0 >> 14) & 3
        x = attr1 & 0x1FF
        if x >= 240:
            x -= 512
        size = (attr1 >> 14) & 3
        tile = attr2 & 0x3FF
        priority = (attr2 >> 10) & 3
        pal_bank = (attr2 >> 12) & 0xF
        bpp8 = bool((attr0 >> 13) & 1)

        dims = OBJ_SHAPE_SIZE.get((shape, size))
        if dims is None:
            continue
        w, h = dims
        tiles_w, tiles_h = w // 8, h // 8
        bytes_per_tile = 64 if bpp8 else 32
        obj_base = 0x10000  # OBJ char base, mode 0-2: fixed at VRAM+0x10000
        active_count += 1

        for ty in range(tiles_h):
            for tx in range(tiles_w):
                tile_index = tile + (ty * tiles_w + tx) * (2 if bpp8 else 1)
                byte_offset = obj_base + tile_index * bytes_per_tile
                if byte_offset + bytes_per_tile > VRAM_SIZE:
                    continue
                for py in range(8):
                    sy_img = y + ty * 8 + py
                    if sy_img < 0 or sy_img >= GBA_SCREEN_H:
                        continue
                    for pxl in range(8):
                        sx_img = x + tx * 8 + pxl
                        if sx_img < 0 or sx_img >= GBA_SCREEN_W:
                            continue
                        idx = decode_tile_pixel(vram, byte_offset, bpp8, pxl, py)
                        if idx == 0:
                            continue
                        any_opaque = True
                        color_idx = idx if bpp8 else (pal_bank * 16 + idx)
                        if color_idx >= len(objpal):
                            continue
                        r, g, b = objpal[color_idx]
                        px[sx_img, sy_img] = (r, g, b, 255)

    status = "HAS_DATA" if any_opaque else "empty"
    fname = f"{label}_obj_{status}_active{active_count}.png"
    img.save(os.path.join(out_dir, fname))
    print(f"OBJ layer: {active_count} active OAM slots -> {status} -> {fname}")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    dump_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    vram = bytearray(open(os.path.join(dump_dir, 'mzm-dump-vram.bin'), 'rb').read())
    io = bytearray(open(os.path.join(dump_dir, 'mzm-dump-io.bin'), 'rb').read())
    bgpal = load_palette(os.path.join(dump_dir, 'mzm-dump-bgpltt.bin'))
    objpal = load_palette(os.path.join(dump_dir, 'mzm-dump-objpltt.bin'))
    oam = bytearray(open(os.path.join(dump_dir, 'mzm-dump-oam.bin'), 'rb').read())

    dispcnt = io[0] | (io[1] << 8)
    print(f"DISPCNT=0x{dispcnt:04X} mode={dispcnt & 7}")

    for bg in range(4):
        decode_bg_layer(vram, io, bgpal, bg, "layer", out_dir)
    decode_obj_layer(vram, oam, objpal, "layer", out_dir)


if __name__ == '__main__':
    main()
