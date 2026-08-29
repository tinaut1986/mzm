"""Comprueba los datos que build_maps.py saca del decomp.

    python3 tools/layer-workbench/test_maps.py

Sin dependencias y en segundos. Cubre la mitad del banco de capas donde los
errores no se ven: un decodificador equivocado no revienta, dibuja algo
plausible y falso. Los cinco fallos reales que hubo aquí fueron de esa clase
-- puertas negras por no cargar los tiles comunes, celdas negras en el mapa por
usar 160 tiles donde hay 384, BG0 ausente en 153 salas, la mezcla alfa sin
aplicar, y las prioridades tratadas como fijas cuando las decide la sala.

Los checksums de la última sección son de regresión: fijan lo que hoy sale,
después de haberlo verificado a mano contra el juego. No dicen que sea
correcto, dicen que no ha cambiado sin querer.

La otra mitad -- ventanas, gestos, scroll -- vive en el navegador y se prueba
con `index.html?test`.
"""
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

from mzmdata import load_room_map  # noqa: E402

FALLOS = []
COMPROBACIONES = [0]


def check(cond, msg):
    COMPROBACIONES[0] += 1
    if not cond:
        FALLOS.append(msg)


def igual(got, want, msg):
    check(got == want, "%s: es %r, se esperaba %r" % (msg, got, want))


def cargar():
    ruta = os.path.join(HERE, "maps.json")
    if not os.path.isfile(ruta):
        print("maps.json no está, generándolo...")
        subprocess.run([sys.executable, os.path.join(HERE, "build_maps.py")],
                       check=True, stdout=subprocess.DEVNULL)
    with open(ruta, encoding="utf-8") as f:
        return json.load(f)


# ------------------------------------------------------------------ forma
def test_estructura(b):
    print("estructura: áreas, salas, tilesets y tablas auxiliares")
    igual(sorted(b["areas"]), sorted(b["areaIds"]), "las áreas y sus números")
    igual(sum(len(v) for v in b["areas"].values()), 315, "salas en total")
    igual(len(b["tilesets"]), 69, "tilesets")
    igual(sorted(b["common"]), ["mothership", "normal"], "juegos de tiles comunes")
    igual(len(b["minimap"]["areas"]), 7, "mapas de área")

    # El orden de la enum Area (include/constants/connection.h). El .inc guarda
    # este número, así que desordenarlo apuntaría las correcciones a otra área.
    igual(b["areaIds"],
          {"brinstar": 0, "kraid": 1, "norfair": 2, "ridley": 3,
           "tourian": 4, "crateria": 5, "chozodia": 6},
          "números de área")


def test_salas(b):
    print("salas: dimensiones, capas y tilesets")
    sin_bg0 = con_bg0 = 0
    for area, salas in b["areas"].items():
        for r in salas:
            donde = "%s/%d" % (area, r["id"])
            check(r["w"] > 0 and r["h"] > 0, donde + ": dimensiones vacías")
            check(str(r["tileset"]) in b["tilesets"],
                  donde + ": tileset %d ausente" % r["tileset"])
            igual(len(r["bg1"]), r["w"] * r["h"], donde + ": tamaño de BG1")
            for capa in ("bg0", "bg2", "clip"):
                if r[capa] is not None:
                    igual(len(r[capa]), r["w"] * r["h"],
                          donde + ": tamaño de " + capa)
            igual(len(r["prio"]), 4, donde + ": prioridades")
            check(all(0 <= p <= 3 for p in r["prio"]), donde + ": prioridad fuera de 0..3")
            igual(r["prio"][3], 3, donde + ": BG3 siempre al fondo")
            if r["bg0"] is not None:
                con_bg0 += 1
            else:
                sin_bg0 += 1
    igual(con_bg0, 153, "salas con BG0 en bloques")
    igual(sin_bg0, 162, "salas sin BG0 en bloques")


def test_indices_de_tile(b):
    """Todo bloque usado tiene que resolver a gráficos que existan.

    RoomLoad deja los comunes en VRAM 0x4800 y el tileset en 0x5800, y el char
    base de BG1 es 0x4000: por debajo de 64 no se usa, 64..191 es común y 192+
    es del tileset. Equivocar ese reparto dibuja una sala casi vacía.
    """
    print("índices: cada bloque resuelve a gráficos que existen")
    comunes = len(bytes.fromhex(b["common"]["normal"]["gfx"])) // 32
    igual(comunes, 128, "tiles comunes")
    for area, salas in b["areas"].items():
        for r in salas:
            ts = b["tilesets"][str(r["tileset"])]
            ntiles = len(bytes.fromhex(ts["gfx"])) // 32
            usados = set()
            for capa in ("bg0", "bg1", "bg2"):
                if r[capa]:
                    usados.update(r[capa])
            for blk in usados:
                if blk >= len(ts["blocks"]):
                    FALLOS.append("%s/%d: bloque 0x%X fuera del tileset %d"
                                  % (area, r["id"], blk, r["tileset"]))
                    COMPROBACIONES[0] += 1
                    continue
                COMPROBACIONES[0] += 1
                for e in ts["blocks"][blk]:
                    tid = e & 0x3FF
                    if tid < 64:
                        continue
                    if tid < 192:
                        ok = (tid - 64) < comunes
                    else:
                        ok = (tid - 192) < ntiles
                    if not ok:
                        FALLOS.append("%s/%d: tile 0x%X sin gráficos (tileset %d)"
                                      % (area, r["id"], tid, r["tileset"]))


def test_minimapa(b):
    """El DMA de la pantalla de pausa copia 0x3000 bytes de un array de 5120 y
    se sale al siguiente en ROM. Sin esa segunda mitad, las celdas especiales
    -- guardado, estación de mapa, objetos, nombres de área -- salen negras.
    """
    print("minimapa: 384 tiles, y ninguna entrada fuera de rango")
    gfx = bytes.fromhex(b["minimap"]["tiles"]["gfx"])
    igual(len(gfx), 0x3000, "bytes de gráficos del minimapa")
    igual(len(gfx) // 32, 384, "tiles del minimapa")
    igual(len(b["minimap"]["tiles"]["pal"]), 80, "colores de paleta del minimapa")
    for area, tm in b["minimap"]["areas"].items():
        igual(len(tm), 32 * 32, area + ": celdas del mapa")
        fuera = [e for e in tm if (e & 0x3FF) >= 384]
        igual(len(fuera), 0, area + ": entradas sin gráficos")


def test_prioridades_y_mezcla(b):
    """Las prioridades las decide `transparency` de la sala
    (TransparencySetRoomEffectsTransparency, src/transparency.c:89), y de ahí
    saca el port la profundidad estéreo. Tratarlas como fijas fue un error real.
    """
    print("prioridades y mezcla alfa, contra casos conocidos")
    patrones = {}
    con_mezcla = 0
    for salas in b["areas"].values():
        for r in salas:
            patrones[tuple(r["prio"][:3])] = patrones.get(tuple(r["prio"][:3]), 0) + 1
            if r["blend"]:
                con_mezcla += 1
    igual(patrones.get((0, 1, 2)), 271, "salas con BG0 delante")
    igual(patrones.get((1, 0, 2)), 40, "salas con BG1 delante")
    igual(patrones.get((2, 0, 1)), 4, "salas con BG0 detrás de BG2")
    igual(con_mezcla, 172, "salas que mezclan una capa con la escena")

    # Crateria 1: su BG0 son 4096 bloques iguales de negro sólido, y con estos
    # coeficientes la mezcla lo deja en nada. Sin aplicarla tapaba la sala.
    cr1 = [r for r in b["areas"]["crateria"] if r["id"] == 1][0]
    igual(cr1["blend"], {"first": [0], "eva": 13, "evb": 16}, "mezcla de crateria 1")
    igual(len(set(cr1["bg0"])), 1, "crateria 1: BG0 es un único bloque repetido")


def test_rle_no_se_desvia():
    """RoomRleDecompress no debe salirse del archivo ni quedarse a medias.

    "Consume el archivo entero" NO es propiedad del formato, aunque lo parezca
    con el archivo que se usó para verificarlo: de los 1058 mapas RLE del
    juego, 925 acaban justo al final, 111 dejan una cola de ceros de relleno y
    33 traen datos propios detrás -- hasta 448 bytes en un clipdata. Lo que sí
    tiene que cumplirse siempre es que la decodificación no se pase del final
    (un desvío lo haría, y revienta) y que salga el número de bloques que
    anuncian las dos primeras cabeceras.

    El reparto va como checksum: si cambia, la decodificación se ha movido.
    """
    print("RLE: la decodificación no se desvía en ningún mapa")
    import build_maps as B
    salas = B.parse_rooms()
    vistos, exactos, relleno, con_cola = set(), 0, 0, 0
    for area, rooms in salas.items():
        for r in rooms:
            capas = [r["bg1"], r["bg2"], r["clip"]]
            if "RLE" in r["bg0Prop"]:
                capas.append(r["bg0"])
            for sym in capas:
                ruta = B.symbol_to_path(sym, area)
                if not ruta or ruta in vistos:
                    continue
                vistos.add(ruta)
                donde = os.path.relpath(ruta, ROOT)
                try:
                    m = load_room_map(ruta)
                except Exception as e:            # noqa: BLE001
                    FALLOS.append("%s: la decodificación se sale (%s)" % (donde, e))
                    COMPROBACIONES[0] += 1
                    continue
                check(m["bytesUsed"] <= m["fileSize"],
                      "%s: leídos %d de %d bytes" % (donde, m["bytesUsed"], m["fileSize"]))
                igual(len(m["blocks"]) * len(m["blocks"][0]), m["width"] * m["height"],
                      donde + ": bloques decodificados")
                cola = open(ruta, "rb").read()[m["bytesUsed"]:]
                if not cola:
                    exactos += 1
                elif not any(cola):
                    relleno += 1
                else:
                    con_cola += 1
    print("  %d mapas: %d exactos, %d con relleno, %d con datos detrás"
          % (len(vistos), exactos, relleno, con_cola))
    igual((exactos, relleno, con_cola), (925, 111, 33), "reparto de finales de archivo")


# --------------------------------------------------------------- regresión
def sha(datos):
    h = hashlib.sha256()
    h.update(repr(datos).encode("utf-8"))
    return h.hexdigest()[:16]


ESPERADO = {
    "tilesets": "9b41ba39ed6acfee",
    "comunes": "52f695fbc5e9c70a",
    "minimapa": "a106eb416950efba",
    "salas": "b672ba4e7378d7dc",
    "render_brinstar_0": "bae3bd1a89d5e88e",
}


def render(b, area, rid):
    """Un render completo, el mismo que hace el visor. Es la comprobación de
    punta a punta: paletas, tiles comunes, gráficos del tileset y bloques."""
    r = [x for x in b["areas"][area] if x["id"] == rid][0]
    ts = b["tilesets"][str(r["tileset"])]
    com = b["common"]["normal"]
    gfx = bytes.fromhex(ts["gfx"])
    cgfx = bytes.fromhex(com["gfx"])

    def color(bank, i):
        if bank >= 3:
            q = 16 + (bank - 3) * 16 + i
            c = ts["pal"][q] if q < len(ts["pal"]) else 0
        else:
            n = bank * 16 + i - 1
            c = com["pal"][n] if n < len(com["pal"]) else 0
        return c

    px = bytearray()
    for capa in ("bg2", "bg1", "bg0"):
        datos = r[capa]
        if not datos:
            continue
        for blk in datos:
            if blk >= len(ts["blocks"]):
                continue
            for e in ts["blocks"][blk]:
                tid = e & 0x3FF
                bank = (e >> 12) & 15
                if tid < 64:
                    continue
                src, off = (cgfx, (tid - 64) * 32) if tid < 192 else (gfx, (tid - 192) * 32)
                if off + 32 > len(src):
                    continue
                for i in range(32):
                    v = src[off + i]
                    for nib in (v & 15, v >> 4):
                        c = color(bank, nib) if nib else 0
                        px.append(c & 0xFF)
                        px.append((c >> 8) & 0xFF)
    return hashlib.sha256(bytes(px)).hexdigest()[:16]


def test_regresion(b):
    print("regresión: los datos decodificados no han cambiado")
    got = {
        "tilesets": sha([(k, b["tilesets"][k]["gfx"], b["tilesets"][k]["pal"],
                          b["tilesets"][k]["blocks"])
                         for k in sorted(b["tilesets"], key=int)]),
        "comunes": sha(b["common"]),
        "minimapa": sha([b["minimap"]["tiles"],
                         [(k, b["minimap"]["areas"][k]) for k in sorted(b["minimap"]["areas"])]]),
        "salas": sha([(a, [(r["id"], r["w"], r["h"], r["tileset"], r["prio"],
                            r["blend"], r["bg0"], r["bg1"], r["bg2"], r["clip"])
                           for r in b["areas"][a]])
                      for a in sorted(b["areas"])]),
        "render_brinstar_0": render(b, "brinstar", 0),
    }
    for k, v in sorted(got.items()):
        if ESPERADO.get(k) == "?":
            print("    %-20s %s   (sin fijar)" % (k, v))
            continue
        igual(v, ESPERADO.get(k), "checksum de " + k)
    return got


def main():
    b = cargar()
    test_estructura(b)
    test_salas(b)
    test_indices_de_tile(b)
    test_minimapa(b)
    test_prioridades_y_mezcla(b)
    test_rle_no_se_desvia()
    got = test_regresion(b)

    print()
    if FALLOS:
        for f in FALLOS[:20]:
            print("  FALLO %s" % f)
        if len(FALLOS) > 20:
            print("  ... y %d más" % (len(FALLOS) - 20))
        print("\n%d comprobaciones, %d fallos" % (COMPROBACIONES[0], len(FALLOS)))
        if any("checksum" in f for f in FALLOS):
            print("\nSi el cambio es intencionado, los valores de ahora son:")
            for k, v in sorted(got.items()):
                print('    "%s": "%s",' % (k, v))
        return 1
    print("%d comprobaciones, 0 fallos" % COMPROBACIONES[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
