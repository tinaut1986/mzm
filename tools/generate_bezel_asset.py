#!/usr/bin/env python3
"""Generates port_gba_bezel_data.c from the GBA bezel frame image.

Rescales and crops the bezel to a 400x240 frame matching the 3DS top screen,
with the 240x160 GBA game viewport at [80, 40] to [319, 199] 100% transparent.
Encodes the 512x256 texture into PICA200 Morton Z-order tiled GPU_RGBA8 format,
compressed with LZ4 so it can be decompressed in 20 lines of C with zero external
library dependencies.
"""

import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
INPUT_IMAGE = r"C:\Users\Tinaut1986\.gemini\antigravity\brain\76802c99-7170-4e6e-bf32-fdcf4b123f42\.user_uploaded\media_1788477341964.png"
OUTPUT_C = os.path.join(ROOT, "platform", "3ds", "source", "port_gba_bezel_data.c")

TEX_W = 512
TEX_H = 256
FRAME_W = 400
FRAME_H = 240
CUTOUT_X = 80
CUTOUT_Y = 40
CUTOUT_W = 240
CUTOUT_H = 160


def swizzle_offset(x, y):
    """PICA200 Morton Z-order offset for 8x8 tiles in a TEX_W wide texture."""
    tile = (y >> 3) * (TEX_W >> 3) + (x >> 3)
    in_tile = ((x & 1)) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)
    return tile * 64 + in_tile


def lz4_compress(data):
    """Simple standard LZ4 block compressor."""
    i = 0
    n = len(data)
    out = bytearray()
    anchor = 0
    while i < n - 4:
        ref = -1
        win = max(0, i - 65535)
        cur = data[i : i + 4]
        p = data.rfind(cur, win, i)
        if p != -1:
            match_len = 0
            while i + match_len < n and data[p + match_len] == data[i + match_len]:
                match_len += 1
            if match_len >= 4:
                offset = i - p
                lit_len = i - anchor
                token = (min(15, lit_len) << 4) | min(15, match_len - 4)
                out.append(token)
                if lit_len >= 15:
                    l = lit_len - 15
                    while l >= 255:
                        out.append(255)
                        l -= 255
                    out.append(l)
                out.extend(data[anchor:i])
                out.append(offset & 0xFF)
                out.append((offset >> 8) & 0xFF)
                if match_len - 4 >= 15:
                    l = (match_len - 4) - 15
                    while l >= 255:
                        out.append(255)
                        l -= 255
                    out.append(l)
                i += match_len
                anchor = i
                continue
        i += 1
    lit_len = n - anchor
    token = min(15, lit_len) << 4
    out.append(token)
    if lit_len >= 15:
        l = lit_len - 15
        while l >= 255:
            out.append(255)
            l -= 255
        out.append(l)
    out.extend(data[anchor:n])
    return bytes(out)


def main():
    if not os.path.exists(INPUT_IMAGE):
        print(f"Error: Input image not found: {INPUT_IMAGE}")
        return 1

    im = Image.open(INPUT_IMAGE).convert("RGBA")

    # The screen cutout in the 1024x576 image is at (263, 122) to (760, 453)
    # Center of cutout:
    cx = (263 + 760) / 2.0
    cy = (122 + 453) / 2.0
    cw = 760 - 263 + 1
    ch = 453 - 122 + 1
    scale_x = CUTOUT_W / float(cw)
    scale_y = CUTOUT_H / float(ch)

    new_w = int(round(im.width * scale_x))
    new_h = int(round(im.height * scale_y))
    scaled = im.resize((new_w, new_h), Image.Resampling.LANCZOS)

    scx = int(round(cx * scale_x))
    scy = int(round(cy * scale_y))

    # Center of cutout in 400x240 frame is (CUTOUT_X + CUTOUT_W/2, CUTOUT_Y + CUTOUT_H/2) = (200, 120)
    crop_x0 = scx - 200
    crop_y0 = scy - 120
    crop_x1 = crop_x0 + FRAME_W
    crop_y1 = crop_y0 + FRAME_H

    cropped = scaled.crop((crop_x0, crop_y0, crop_x1, crop_y1))
    pixels = cropped.load()

    # Ensure the 240x160 cutout is 100% transparent
    for y in range(CUTOUT_Y, CUTOUT_Y + CUTOUT_H):
        for x in range(CUTOUT_X, CUTOUT_X + CUTOUT_W):
            pixels[x, y] = (0, 0, 0, 0)

    # Ensure the outer bezel is 100% opaque
    for x in range(FRAME_W):
        for y in range(FRAME_H):
            if x < CUTOUT_X or x >= CUTOUT_X + CUTOUT_W or y < CUTOUT_Y or y >= CUTOUT_Y + CUTOUT_H:
                p = pixels[x, y]
                if p[3] < 255:
                    pixels[x, y] = (p[0], p[1], p[2], 255)

    # Pack into PICA200 swizzled GPU_RGBA8 layout
    # Native PICA order in memory: byte 0 = A, byte 1 = B, byte 2 = G, byte 3 = R
    raw_tex = bytearray(TEX_W * TEX_H * 4)
    for y in range(FRAME_H):
        for x in range(FRAME_W):
            r, g, b, a = pixels[x, y]
            offset = swizzle_offset(x, y) * 4
            raw_tex[offset + 0] = a
            raw_tex[offset + 1] = b
            raw_tex[offset + 2] = g
            raw_tex[offset + 3] = r

    # Scaled side strips for Escalado - Original (360x240 game):
    # Scale console image so that the screen cutout is 360x240, and take
    # the 55px region from the screen edge across the bezel/seam to the purple chassis,
    # resized into the 20px pillarbox strips.
    sy = 240.0 / ch
    scaled_h = int(round(im.height * sy))
    scaled_w = int(round(im.width * sy))
    im_sy = im.resize((scaled_w, scaled_h), Image.Resampling.LANCZOS)
    cutout_top = int(round(122 * sy))
    cutout_bottom = cutout_top + 240
    cutout_left = int(round(263 * sy))
    cutout_right = int(round(760 * sy))

    width_px = 55
    l_crop = im_sy.crop(
        (cutout_left - width_px, cutout_top, cutout_left, cutout_bottom)
    ).resize((20, 240), Image.Resampling.LANCZOS)
    r_crop = im_sy.crop(
        (cutout_right, cutout_top, cutout_right + width_px, cutout_bottom)
    ).resize((20, 240), Image.Resampling.LANCZOS)
    l_pixels = l_crop.load()
    r_pixels = r_crop.load()

    # Left strip at X: 416..436, Y: 0..240
    for y in range(240):
        for x in range(20):
            r, g, b, a = l_pixels[x, y]
            offset = swizzle_offset(416 + x, y) * 4
            raw_tex[offset + 0] = 255
            raw_tex[offset + 1] = b
            raw_tex[offset + 2] = g
            raw_tex[offset + 3] = r

    # Right strip at X: 448..468, Y: 0..240
    for y in range(240):
        for x in range(20):
            r, g, b, a = r_pixels[x, y]
            offset = swizzle_offset(448 + x, y) * 4
            raw_tex[offset + 0] = 255
            raw_tex[offset + 1] = b
            raw_tex[offset + 2] = g
            raw_tex[offset + 3] = r

    compressed = lz4_compress(raw_tex)
    print(f"Compressed {len(raw_tex)} bytes to {len(compressed)} bytes ({len(compressed) / 1024:.1f} KB)")

    with open(OUTPUT_C, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated by tools/generate_bezel_asset.py -- do not edit directly */\n")
        f.write("#include <stddef.h>\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"const size_t gGbaBezelCompressedSize = {len(compressed)};\n")
        f.write(f"const size_t gGbaBezelUncompressedSize = {len(raw_tex)};\n\n")
        f.write("const uint8_t gGbaBezelCompressedData[] = {\n")
        for i in range(0, len(compressed), 16):
            chunk = compressed[i : i + 16]
            hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
            f.write(f"    {hex_str},\n")
        f.write("};\n")

    print(f"Written: {OUTPUT_C}")
    return 0


if __name__ == "__main__":
    exit(main())
