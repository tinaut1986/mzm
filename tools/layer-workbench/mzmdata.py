"""Lectura de los datos de sala del decomp, sin ROM y sin emular nada.

Todo lo que hay aquí es una transcripción de código del propio juego, con la
referencia al lado. Si algo no cuadra, la fuente manda.
"""
import re
import os
import struct

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def rle_decompress(src, is_bg=True):
    """RoomRleDecompress (src/room.c:848).

    Dos pasadas: la primera rellena los bytes bajos del destino y la segunda
    los altos, avanzando de dos en dos. Cada pasada empieza con un byte que
    dice si las cabeceras de tramo son de 1 o de 2 bytes.
    """
    dst = bytearray(0x3000 if is_bg else 0x2000)
    i = 0
    if not is_bg:
        i += 1  # sizeType, solo decide el tamaño devuelto

    for half in range(2):
        d = half  # dest = dst (+1 en la segunda pasada)
        num_bytes = src[i]; i += 1

        if num_bytes == 1:
            value = src[i]; i += 1
            while value:
                if value & 0x80:
                    value &= 0x7F
                    b = src[i]
                    if b:
                        for _ in range(value):
                            dst[d] = b; d += 2
                    else:
                        d += value * 2
                    i += 1
                else:
                    for _ in range(value):
                        dst[d] = src[i]; i += 1; d += 2
                value = src[i]; i += 1
        else:
            value = (src[i] << 8) | src[i + 1]; i += 2
            while value:
                if value & 0x8000:
                    value &= 0x7FFF
                    b = src[i]
                    if b:
                        for _ in range(value):
                            dst[d] = b; d += 2
                    else:
                        d += value * 2
                    i += 1
                else:
                    for _ in range(value):
                        dst[d] = src[i]; i += 1; d += 2
                value = (src[i] << 8) | src[i + 1]; i += 2

    return dst, i


def load_room_map(path):
    """Un archivo de sala: ancho, alto y luego los datos RLE (src/room.c:476)."""
    raw = open(path, "rb").read()
    width, height = raw[0], raw[1]
    data, used = rle_decompress(raw[2:], is_bg=True)
    blocks = []
    for y in range(height):
        row = []
        for x in range(width):
            o = (y * width + x) * 2
            row.append(data[o] | (data[o + 1] << 8))
        blocks.append(row)
    return {"width": width, "height": height, "blocks": blocks,
            "bytesUsed": used + 2, "fileSize": len(raw)}


if __name__ == "__main__":
    import sys
    p = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "data", "rooms", "brinstar", "brinstar_0_bg1.gfx")
    r = load_room_map(p)
    print(os.path.basename(p))
    print("  %dx%d bloques   consumidos %d de %d bytes"
          % (r["width"], r["height"], r["bytesUsed"], r["fileSize"]))
    vals = [v for row in r["blocks"] for v in row]
    print("  valores distintos: %d   máximo: 0x%X" % (len(set(vals)), max(vals)))
    print("  primeras filas:")
    for row in r["blocks"][:6]:
        print("   ", " ".join("%03X" % v for v in row[:26]))
