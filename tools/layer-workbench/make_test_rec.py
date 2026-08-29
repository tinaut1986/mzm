"""Fabrica un mzm-rec.bin sintético desde maps.json.

    python3 tools/layer-workbench/make_test_rec.py mzm-rec-test.bin

Reproduce el transfer de RoomUpdate*Tilemap (src/room.c:795) sobre una sala de
64x64 bloques, o sea el doble de ancha que el periodo del screenmap. Cargarlo
en el visor comprueba lo único que no se puede comprobar mirando: que las
coordenadas envueltas del screenmap se desenvuelven hasta el bloque de sala
correcto. En el panel de cada tile la fila `bloque` tiene que coincidir con la
que da el modo mapa, y ninguna celda debe salir sin verificar.

Elegir una sala pequeña no probaría nada: sin dar la vuelta, el desenvuelto es
la identidad.
"""
import json, struct, sys

AREA, RID, CAMBX, CAMBY = "crateria", 1, 40, 20   # sala de 64x64: obliga a desenvolver
b = json.load(open('tools/layer-workbench/maps.json'))
room = [r for r in b['areas'][AREA] if r['id'] == RID][0]
ts = b['tilesets'][str(room['tileset'])]; com = b['common']['normal']

O_IO, O_BGPAL, O_OBJPAL, O_OAM, O_VRAM = 64, 1088, 1600, 2112, 3136
VRAM_SIZE = 0x18000
CLIP = 24 + 17 * 12

io = bytearray(1024); vram = bytearray(VRAM_SIZE)
bgpal = bytearray(512); objpal = bytearray(512); oam = bytearray(1024)

def w16(buf, off, v): buf[off] = v & 0xFF; buf[off+1] = (v >> 8) & 0xFF

# DISPCNT: modo 0, sólo BG1 encendido
w16(io, 0, 1 << (8 + 1))
# BG1CNT: char base 0x4000 (bits 2-3 = 1), screen base 0x1000 (bits 8-12 = 2),
# tamaño 64x32 tiles (bits 14-15 = 1), prioridad 0
w16(io, 8 + 1*2, (1 << 2) | (2 << 8) | (1 << 14))
# scroll: la posición de la cámara en píxeles, que el hardware ve módulo 512
w16(io, 0x10 + 1*4, (CAMBX * 16) & 0x1FF)
w16(io, 0x12 + 1*4, (CAMBY * 16) & 0x1FF)

vram[0x4800:0x4800+4096] = bytes.fromhex(com['gfx'])
g = bytes.fromhex(ts['gfx']); vram[0x5800:0x5800+len(g)] = g

for i, c in enumerate((com['pal'] + [0]*48)[:47]): w16(bgpal, (i+1)*2, c)
for i in range(208):
    q = 16 + i
    w16(bgpal, (3*16 + i)*2, ts['pal'][q] if q < len(ts['pal']) else 0)

# El transfer: 21x16 bloques alrededor de la cámara, con las máscaras del juego
for i in range(16):
    ypos = max(0, CAMBY - 3) + i
    if ypos >= room['h']: break
    for j in range(21):
        tmpx = max(0, CAMBX - 3) + j
        if tmpx >= room['w']: break
        blk = room['bg1'][ypos * room['w'] + tmpx]
        base = 0x1000 + (0x800 if (tmpx & 0x10) else 0)
        off = base + ((tmpx & 0xF) * 2 + (ypos & 0xF) * 64) * 2
        for k, e in enumerate(ts['blocks'][blk]):
            w16(vram, off + (k % 2) * 2 + (k // 2) * 64, e)

clip = bytearray(CLIP)
w16(clip, 16, CAMBX * 16); w16(clip, 18, CAMBY * 16)
w16(clip, 20, b['areaIds'][AREA]); w16(clip, 22, RID)

header = bytearray(64)
header[0:4] = struct.pack('<I', 0x344D5A4D)
sample = bytes(header) + bytes(io) + bytes(bgpal) + bytes(objpal) + bytes(oam) + bytes(vram) + bytes(clip)
assert len(sample) == O_VRAM + VRAM_SIZE + CLIP, len(sample)
open(sys.argv[1], 'wb').write(sample * 2)   # dos capturas, para probar el stride
print("mzm-rec sintético:", len(sample)*2, "bytes ·", AREA, RID, "· cámara en bloque", CAMBX, CAMBY)
