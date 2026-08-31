"""Genera sprites.json para el modo SPRITES del banco de capas.

    python3 tools/layer-workbench/build_sprites.py

Es sólo el catálogo de TIPOS de sprite: el nombre y el id de cada
`PSPRITE_*` / `SSPRITE_*`, sacados de include/constants/sprite.h. No hay
gráficos: un tipo de sprite no se dibuja sin correr su IA (fija pose,
bgPriority y posición en runtime), así que la vista previa real sale de una
grabación. El catálogo sirve para decidir a qué profundidad va cada tipo y
escribir platform/3ds/source/port_sprite_depth.inc.

Los tiers salen de platform/3ds/source/port_stereo_depth.h y los códigos
especiales de port_sprite_depth_oam.h, para no repetir la lista aquí.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

SPRITE_H = os.path.join(ROOT, "include", "constants", "sprite.h")
STEREO_H = os.path.join(ROOT, "platform", "3ds", "source", "port_stereo_depth.h")
DEPTH_H = os.path.join(ROOT, "platform", "3ds", "source", "port_sprite_depth_oam.h")
OUT = os.path.join(HERE, "sprites.json")


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def enum_members(text, enum_name):
    """Los identificadores de `MAKE_ENUM(u8, <enum_name>) { ... };`, en orden.

    Las listas de sprite.h son identificadores a secas sin `= valor`, así que
    el índice en la lista ES el valor. Se descarta el `_COUNT` final y las
    líneas en blanco."""
    m = re.search(r"MAKE_ENUM\(\s*\w+\s*,\s*" + re.escape(enum_name) +
                  r"\s*\)\s*(?:ENUM_FLAG\s*)?\{(.*?)\}\s*;", text, re.S)
    if not m:
        raise SystemExit("no encuentro el enum %s en %s" % (enum_name, SPRITE_H))
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    out = []
    for tok in body.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if "=" in tok:                       # por si algún día lo hubiera
            tok = tok.split("=")[0].strip()
        if not re.fullmatch(r"[A-Za-z_]\w*", tok):
            raise SystemExit("token raro en %s: %r" % (enum_name, tok))
        out.append(tok)
    if out and out[-1].endswith("_COUNT"):
        out.pop()
    return out


def tiers():
    """PORT_TIER_* -> valor, de port_stereo_depth.h (enum secuencial)."""
    text = read(STEREO_H)
    m = re.search(r"enum\s*\{([^}]*PORT_TIER_[^}]*)\}", text, re.S)
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    out = []
    val = 0
    for tok in body.split(","):
        tok = tok.strip()
        if not tok or "PORT_TIER_COUNT" in tok:
            continue
        mm = re.match(r"(PORT_TIER_\w+)\s*(?:=\s*(\d+))?", tok)
        if not mm:
            continue
        if mm.group(2) is not None:
            val = int(mm.group(2))
        out.append({"name": mm.group(1), "value": val})
        val += 1
    return out


def special_codes():
    """PORT_SPRITE_DEPTH_* negativos, de port_sprite_depth_oam.h."""
    text = read(DEPTH_H)
    out = []
    for mm in re.finditer(r"(PORT_SPRITE_DEPTH_\w+)\s*=\s*(-?\d+)", text):
        out.append({"name": mm.group(1), "value": int(mm.group(2))})
    return out


def main():
    text = read(SPRITE_H)
    primary = enum_members(text, "PrimarySprite")
    secondary = enum_members(text, "SecondarySprite")
    data = {
        "primary": [{"id": i, "name": n} for i, n in enumerate(primary)],
        "secondary": [{"id": i, "name": n} for i, n in enumerate(secondary)],
        "tiers": tiers(),
        "specialCodes": special_codes(),
    }
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
    print("sprites.json: %d primarios, %d secundarios, %d tiers"
          % (len(primary), len(secondary), len(data["tiers"])))


if __name__ == "__main__":
    main()
