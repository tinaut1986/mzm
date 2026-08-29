"""Genera maps.json: cada sala del juego, decodificada, lista para el visor.

No hace falta la ROM ni emular nada. Todo sale de los datos del decomp, con
los algoritmos transcritos del propio juego (ver mzmdata.py y las referencias
de abajo).

    python3 tools/layer-workbench/build_maps.py   (python, en Windows)

Escribe tools/layer-workbench/maps.json (y maps.json.gz si pesa).
"""
import json
import os
import re
import sys
import gzip
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# Si estamos en un worktree secundario, enlazar data/ y include/extracted/ del repositorio principal
try:
    _common_git = subprocess.check_output(
        ["git", "rev-parse", "--git-common-dir"],
        cwd=ROOT,
        stderr=subprocess.DEVNULL,
        text=True
    ).strip()
    _main_root = os.path.abspath(os.path.join(ROOT, _common_git, ".."))
    if os.path.isdir(_main_root) and _main_root != ROOT:
        for _item in ("include/extracted", "data"):
            _src = os.path.join(_main_root, _item.replace("/", os.sep))
            _dst = os.path.join(ROOT, _item.replace("/", os.sep))
            if os.path.exists(_src) and not os.path.exists(_dst):
                os.makedirs(os.path.dirname(_dst), exist_ok=True)
                try:
                    if hasattr(os, "symlink"):
                        os.symlink(_src, _dst)
                    else:
                        import shutil
                        shutil.copytree(_src, _dst)
                except Exception:
                    pass
except Exception:
    pass

sys.path.insert(0, HERE)
from mzmdata import rle_decompress  # noqa: E402

AREAS = [
    ("brinstar", "sBrinstarRoomEntries"),
    ("kraid", "sKraidRoomEntries"),
    ("norfair", "sNorfairRoomEntries"),
    ("ridley", "sRidleyRoomEntries"),
    ("tourian", "sTourianRoomEntries"),
    ("crateria", "sCrateriaRoomEntries"),
    ("chozodia", "sChozodiaRoomEntries"),
]


# ---------------------------------------------------------------- LZ77
def lz77_decompress(src):
    """GBA BIOS LZ77UnComp (GBATEK). Cabecera: 0x10, luego 3 bytes de tamaño."""
    assert src[0] == 0x10, "no es LZ77: 0x%02X" % src[0]
    size = src[1] | (src[2] << 8) | (src[3] << 16)
    out = bytearray()
    i = 4
    while len(out) < size:
        flags = src[i]; i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                b0, b1 = src[i], src[i + 1]; i += 2
                length = (b0 >> 4) + 3
                disp = (((b0 & 0x0F) << 8) | b1) + 1
                start = len(out) - disp
                for k in range(length):
                    out.append(out[start + k])
            else:
                out.append(src[i]); i += 1
    return bytes(out[:size])


# ------------------------------------------------------- rooms_data.c
FIELD = re.compile(r"\.(\w+)\s*=\s*([^,\n]+?)\s*,\s*(?://.*)?$", re.M)


def parse_rooms():
    """Los datos de sala son inicializadores designados muy regulares, así que
    se leen con una pasada de texto en vez de compilando nada."""
    text = open(os.path.join(ROOT, "src", "data", "rooms_data.c"),
                encoding="utf-8", errors="replace").read()
    # fuera los bloques de región: nos quedamos con la rama por defecto (US/JP)
    text = re.sub(r"#ifdef REGION_\w+.*?#else[^\n]*\n(.*?)#endif[^\n]*\n",
                  r"\1", text, flags=re.S)
    text = re.sub(r"#ifdef REGION_\w+.*?#endif[^\n]*\n", "", text, flags=re.S)

    out = {}
    for area, symbol in AREAS:
        m = re.search(re.escape(symbol) + r"\[\d+\]\s*=\s*\{", text)
        if not m:
            continue
        start = m.end()
        depth, i = 1, start
        while depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        body = text[start:i - 1]

        rooms = []
        for rm in re.finditer(r"\[(\d+)\]\s*=\s*\{(.*?)\n\t\}", body, re.S):
            idx, fields = int(rm.group(1)), dict(FIELD.findall(rm.group(2)))
            rooms.append({
                "id": idx,
                "tileset": int(fields.get("tileset", 0)),
                "mapX": int(fields.get("mapX", 0)),
                "mapY": int(fields.get("mapY", 0)),
                "bg0": fields.get("pBg0Data", ""),
                "bg1": fields.get("pBg1Data", ""),
                "bg2": fields.get("pBg2Data", ""),
                "clip": fields.get("pClipData", ""),
                "bg0Prop": fields.get("bg0Prop", ""),
                "bg2Prop": fields.get("bg2Prop", ""),
                "transparency": fields.get("transparency", "0"),
                "visualEffect": fields.get("visualEffect", ""),
                "music": fields.get("musicTrack", ""),
            })
        out[area] = sorted(rooms, key=lambda r: r["id"])
    return out


def bg_priorities(transparency):
    """Prioridades BGCNT de la sala, de TransparencySetRoomEffectsTransparency
    (src/transparency.c:89).

    No son fijas: las decide el campo `transparency` de la sala, y el orden de
    las capas cambia con él -- en unas salas BG0 va delante de todo y en otras
    detrás de BG2. Importa porque la profundidad estéreo del port sale justo de
    esta prioridad (PortStereoDepth_BgTier).

    El switch enumera los casos uno a uno, pero el patrón es el resto entre 4,
    con 0x0..0x4 metidos todos en el caso por defecto.
    BGCNT: 0 = delante, 3 = al fondo (include/gba/display.h:75).
    """
    try:
        t = int(transparency, 0)
    except ValueError:
        t = 0
    if t <= 4 or t > 0x33:
        p0, p1, p2 = 0, 1, 2
    else:
        p0, p1, p2 = {1: (1, 0, 2), 2: (1, 0, 2), 3: (2, 0, 1)}.get(t % 4, (0, 1, 2))
    return [p0, p1, p2, 3]        # BG3 siempre al fondo (transparency.c:75)


def blend_setup(transparency, visual_effect, bg0_prop):
    """La mezcla alfa de la sala, de TransparencySetRoomEffectsTransparency
    (src/transparency.c:170 y :305).

    Sin esto, una capa como el BG0 de Crateria 1 -- 4096 bloques iguales, todos
    negro sólido -- taparía la sala entera. En el juego no tapa nada: es el
    primer objetivo de una mezcla con la escena, y con sus coeficientes el
    resultado es la escena tal cual. Es una capa de efecto, no de dibujo.

    Devuelve los BG que son primer objetivo y los coeficientes EVA/EVB sobre 16,
    o None si la sala no mezcla.
    """
    try:
        t = int(transparency, 0)
    except ValueError:
        t = 0

    eva = evb = 0
    for lo, hi, v in ((0x18, 0x1B, 0), (0x1C, 0x1F, 3), (0x20, 0x23, 6),
                      (0x24, 0x27, 9), (0x28, 0x2B, 11), (0x2C, 0x2F, 13),
                      (0x30, 0x33, 16)):
        if lo <= t <= hi:
            evb = v
    for lo, hi, v in ((0x8, 0xB, 7), (0xC, 0xF, 10), (0x10, 0x13, 13),
                      (0x14, 0x17, 16)):
        if lo <= t <= hi:
            eva = v

    if eva == 0:
        eva_coef, evb_coef = 16 - evb, evb
    else:
        eva_coef, evb_coef = eva, 16

    # BLDCNT: por debajo de 8 no hay primer objetivo, así que no se mezcla nada.
    first = []
    if t >= 8:
        first.append(0)
    if "BG3_GRADIENT" in visual_effect:
        first.append(3)
    elif "BG2_GRADIENT" in visual_effect:
        first.append(2)
    if "DISABLE_TRANSPARENCY" in bg0_prop and 0 in first:
        first.remove(0)

    if not first:
        return None
    return {"first": first, "eva": eva_coef, "evb": evb_coef}


def symbol_to_path(sym, area):
    """sBrinstar_0_Bg1 -> data/rooms/brinstar/brinstar_0_bg1.gfx"""
    if not sym or sym.startswith("sBackground_Empty") or "Empty" in sym:
        return None
    name = sym[1:] if sym.startswith("s") else sym
    p = os.path.join(ROOT, "data", "rooms", area, name.lower() + ".gfx")
    return p if os.path.exists(p) else None


def load_map_file(path):
    raw = open(path, "rb").read()
    w, h = raw[0], raw[1]
    data, _ = rle_decompress(raw[2:], is_bg=True)
    return w, h, [data[i * 2] | (data[i * 2 + 1] << 8) for i in range(w * h)]


# ------------------------------------------------------- tiles comunes
INC = os.path.join(ROOT, "include", "extracted", "data", "common")


def load_u_array(name):
    """Los .inc extraídos son listas planas de `123u,` separadas por comas."""
    text = open(os.path.join(INC, name), encoding="utf-8").read()
    return [int(m) for m in re.findall(r"(\d+)u", text)]


def common_tiles():
    """RoomLoadTileset (src/room.c:315) carga los tiles comunes en VRAM 0x4800
    -- índices 64..191 para BG1, cuyo char base es 0x4000 -- y su paleta en los
    bancos 0..2.

    El DMA de la paleta (room.c:333) copia 47 colores desde sCommonTilesPal+1
    hacia PALRAM+2, o sea colores 1..47. sCommonTilesPal sólo tiene 16, así que
    se sale al array siguiente en ROM: los 32 primeros de sDoorTransitionPal.
    El decomp ya marca ese desbordamiento con un "write past?" al lado.

    Cuál de los dos juegos se usa depende de gUseMotherShipDoors, que es estado
    de partida y no del mapa, así que van los dos y el visor deja elegir.
    """
    out = {}
    for key, gfx, pal, door in (
            ("normal", "common_tiles.gfx.inc", "common_tiles.pal.inc",
             "door_transition.pal.inc"),
            ("mothership", "common_tiles_mother_ship.gfx.inc",
             "common_tiles_mother_ship.pal.inc",
             "door_transition_mother_ship.pal.inc")):
        colors = (load_u_array(pal) + load_u_array(door))[1:48]
        out[key] = {"gfx": bytes(load_u_array(gfx)).hex(), "pal": colors}
    return out


# ------------------------------------------------------------ minimapa
PAUSE_INC = os.path.join(ROOT, "include", "extracted", "data", "menus",
                         "pause_screen")


def _pause_u_array(name):
    text = open(os.path.join(PAUSE_INC, name), encoding="utf-8").read()
    return [int(m) for m in re.findall(r"(\d+)u", text)]


def minimaps():
    """El mapa de área de la pantalla de pausa.

    PauseScreenGetMinimapData (src/menus/pause_screen.c:2966) descomprime
    sMinimapDataPointers[área] en una rejilla de 32x32 entradas de tilemap
    (MINIMAP_SIZE, include/structs/minimap.h:10). Los gráficos y la paleta son
    los mismos para todas las áreas.
    """
    out = {"tiles": {}, "areas": {}}

    # PauseScreenLoadMapGfx (src/menus/pause_screen.c:2536) copia 0x3000 bytes
    # desde sMinimapTilesGfx, que sólo mide 5120: se sale al array siguiente en
    # ROM, sPauseScreen_40f4c4, y entre los dos suman justo esos 0x3000 -- 384
    # tiles, no 160. Los índices por encima de 160 son las celdas especiales
    # (guardado, estación de mapa, objetos), y sin esta segunda mitad salen
    # negras.
    gfx = bytes(_pause_u_array("minimap_tiles.gfx.inc"))
    gfx += b"".join(struct.pack("<I", v)
                    for v in _pause_u_array("40f4c4.gfx.inc"))
    assert len(gfx) == 0x3000, len(gfx)
    out["tiles"] = {"gfx": gfx.hex(),
                    "pal": _pause_u_array("minimap_tiles.pal.inc")}
    for area, _ in AREAS:
        packed = b"".join(struct.pack("<I", v) for v in
                          _pause_u_array(area + "_minimap.tt.inc"))
        tm = lz77_decompress(packed)
        out["areas"][area] = [tm[i * 2] | (tm[i * 2 + 1] << 8)
                              for i in range(len(tm) // 2)]
    return out


# ------------------------------------------------------------ tilesets
_TS_ENTRIES = None


def tileset_files(n):
    """sTilesetEntries (rooms_data.c:32) reparte gráficos, tilemap y paleta por
    separado, y varias entradas reutilizan los de otra: la 31 dibuja con los
    gráficos de la 2 pero con su propia paleta. Sin esto, trece tilesets se
    quedaban sin archivos."""
    global _TS_ENTRIES
    if _TS_ENTRIES is None:
        text = open(os.path.join(ROOT, "src", "data", "rooms_data.c"),
                    encoding="utf-8", errors="replace").read()
        m = re.search(r"sTilesetEntries\[\d+\]\s*=\s*\{", text)
        start = m.end(); depth = 1; i = start
        while depth:
            if text[i] == "{": depth += 1
            elif text[i] == "}": depth -= 1
            i += 1
        body = text[start:i - 1]
        _TS_ENTRIES = {}
        for e in re.finditer(r"\[(\d+)\]\s*=\s*\{(.*?)\n\t\}", body, re.S):
            f = dict(re.findall(r"\.(\w+)\s*=\s*([^,\n]+?)\s*,?\s*$", e.group(2), re.M))

            def num(sym):
                mm = re.match(r"sTileset_(\d+)_", sym or "")
                return int(mm.group(1)) if mm else None
            _TS_ENTRIES[int(e.group(1))] = (
                num(f.get("pTileGraphics")), num(f.get("pPalette")), num(f.get("pTilemap")))
    return _TS_ENTRIES.get(n, (n, n, n))


def load_tileset(n):
    gn, pn, tn = tileset_files(n)
    d = os.path.join(ROOT, "data", "tilesets")
    tm = open(os.path.join(d, "%d.tm" % (tn if tn is not None else n)), "rb").read()
    pal = open(os.path.join(d, "%d.pal" % (pn if pn is not None else n)), "rb").read()
    gfx = lz77_decompress(open(os.path.join(d, "%d.gfx.lz" % (gn if gn is not None else n)), "rb").read())

    # .tm: 2 bytes de cabecera, luego 4 entradas u16 por bloque (room.c:302)
    body = tm[2:]
    nblocks = len(body) // 8
    blocks = []
    for b in range(nblocks):
        o = b * 8
        blocks.append([body[o + k * 2] | (body[o + k * 2 + 1] << 8) for k in range(4)])

    colors = [pal[i * 2] | (pal[i * 2 + 1] << 8) for i in range(len(pal) // 2)]
    return {"blocks": blocks, "pal": colors, "gfx": gfx}


def main():
    rooms_by_area = parse_rooms()
    tilesets_needed = set()
    bundle = {"areas": {}, "tilesets": {}}

    for area, rooms in rooms_by_area.items():
        entries = []
        for r in rooms:
            p1 = symbol_to_path(r["bg1"], area)
            p2 = symbol_to_path(r["bg2"], area)
            pc = symbol_to_path(r["clip"], area)
            if not p1:
                continue
            w, h, bg1 = load_map_file(p1)
            bg2 = None
            if p2:
                w2, h2, m2 = load_map_file(p2)
                if (w2, h2) == (w, h):
                    bg2 = m2
            # BG0 sólo cuando es RLE: entonces es un mapa de bloques igual que
            # BG1 y BG2 y se dibuja con el mismo tileset. Los 8 que son LZ77
            # son un tilemap crudo con otros gráficos, y ésos no salen.
            bg0 = None
            if "RLE" in r["bg0Prop"]:
                p0 = symbol_to_path(r["bg0"], area)
                if p0:
                    w0, h0, m0 = load_map_file(p0)
                    if (w0, h0) == (w, h):
                        bg0 = m0
            clip = None
            if pc:
                wc, hc, mc = load_map_file(pc)
                if (wc, hc) == (w, h):
                    clip = mc
            tilesets_needed.add(r["tileset"])
            entries.append({"id": r["id"], "tileset": r["tileset"],
                            "mapX": r["mapX"], "mapY": r["mapY"],
                            "w": w, "h": h, "bg0": bg0, "bg1": bg1, "bg2": bg2,
                            "clip": clip, "prio": bg_priorities(r["transparency"]),
                            "blend": blend_setup(r["transparency"],
                                                 r["visualEffect"], r["bg0Prop"])})
        bundle["areas"][area] = entries
        print("%-9s %3d salas  (%d con BG0)"
              % (area, len(entries), sum(1 for e in entries if e["bg0"])))

    for n in sorted(tilesets_needed):
        try:
            t = load_tileset(n)
        except FileNotFoundError:
            print("  tileset %d: sin archivos, se omite" % n)
            continue
        bundle["tilesets"][str(n)] = {
            "blocks": t["blocks"], "pal": t["pal"],
            "gfx": t["gfx"].hex(),
        }
    print("tilesets: %d" % len(bundle["tilesets"]))

    # El número de área es el de la enum Area (include/constants/connection.h),
    # que es el orden de AREAS. Va explícito para que el visor no tenga que
    # repetir la lista y se puedan desincronizar.
    bundle["areaIds"] = {name: i for i, (name, _) in enumerate(AREAS)}

    bundle["common"] = common_tiles()
    print("tiles comunes: %d juegos" % len(bundle["common"]))

    bundle["minimap"] = minimaps()
    print("minimapas: %d áreas de 32x32" % len(bundle["minimap"]["areas"]))

    out = os.path.join(HERE, "maps.json")
    text = json.dumps(bundle, separators=(",", ":"))
    open(out, "w", encoding="utf-8").write(text)
    print("maps.json  %.1f MB" % (len(text) / 1048576))
    with gzip.open(out + ".gz", "wt", encoding="utf-8") as f:
        f.write(text)
    print("maps.json.gz  %.1f MB" % (os.path.getsize(out + ".gz") / 1048576))


if __name__ == "__main__":
    main()
