#!/usr/bin/env python3
"""
Generate a runtime ROM-resolution shim for one src/data/*.h header.

Background: mzm compiles src/data/*.c straight into the GBA binary from
extracted/*.inc files. The 3DS port doesn't want to ship those assets, so
instead of compiling that data in, we want a `T *p_NAME` pointer per symbol,
pointed at the right offset inside the user's loaded ROM (region-specific)
at startup. See docs/3ds-port-rom-loading.md for the full design and why
this is possible (mzm is a matching decomp, so the .map file gives us the
exact GBA ROM address of every data symbol per region -- no guesswork).

Usage:
    python3 tools/gen_port_rom_offsets.py include/data/chozodia_escape_data.h \\
        --map eu=mzm_eu.map --map us=mzm_us.map --map jp=mzm_jp.map \\
        --out port/generated/chozodia_escape_data_rom.c \\
        --out-header port/generated/chozodia_escape_data_rom.h

Only .map files that are actually present need to be passed -- a symbol
missing from a given region's map just doesn't get an entry for that region
(the init function skips it there, so this only matters once multi-region
support is wired up; single-region use, e.g. EU-only, works with just one
--map).

Scope note: this handles the declaration shapes actually observed in
include/data/*.h (extern const T name[dims]; including struct types,
pointer-to-const-struct arrays, and multi-dimensional / unsized arrays). It
is a regex-based parser, not a real C parser -- function-pointer typedefs or
anything not shaped like "TYPE NAME DIMS;" will be reported as unparsed
rather than silently mishandled.
"""
import argparse
import re
import sys
from pathlib import Path

DECL_RE = re.compile(
    r"^extern\s+(?P<type>.*[\s\*])(?P<name>[A-Za-z_]\w*)\s*(?P<dims>(?:\[[^\]]*\]\s*)*);\s*$"
)
IFDEF_RE = re.compile(r"^#\s*ifdef\s+(\S+)")
IFNDEF_RE = re.compile(r"^#\s*ifndef\s+(\S+)")
ELSE_RE = re.compile(r"^#\s*else\b")
ENDIF_RE = re.compile(r"^#\s*endif\b")


class Symbol:
    def __init__(self, type_, name, dims, guard):
        self.type = type_.strip()
        self.name = name
        self.dims = dims.replace(" ", "")
        self.guard = guard  # e.g. "REGION_EU" or None

    def pointer_dims(self):
        """Array dims for the *pointer* declaration: `T (*p)DIMS`.

        A single unsized leading dimension (`[]`) can't be part of a
        pointer-to-array type as an object declaration, so it decays to a
        flat pointer (dims == ""). An unsized *outer* dim followed by fixed
        inner dims (`[][2]`) drops just that first empty group -- the
        pointer still needs the inner dims to index correctly.
        """
        groups = re.findall(r"\[[^\]]*\]", self.dims)
        if groups and groups[0] == "[]":
            groups = groups[1:]
        return "".join(groups)

    def pointer_var(self, name):
        """Full `T (*name)DIMS` / `T *name` declarator for a given variable
        name (self.type already includes any "const" from the original
        declaration -- it's everything between "extern" and the name)."""
        dims = self.pointer_dims()
        if dims:
            return f"{self.type}(*{name}){dims}"
        return f"{self.type}*{name}"

    def cast_type(self):
        """Same shape as pointer_var() but as an anonymous cast, e.g.
        `T (*)DIMS` / `T *`."""
        dims = self.pointer_dims()
        if dims:
            return f"{self.type}(*){dims}"
        return f"{self.type}*"

    def extern_decl(self):
        """Declaration for the header: external linkage, visible to every
        translation unit that includes it (this is what makes the redirect
        actually work across files, unlike a `static` pointer that would
        give each .c its own unresolved copy)."""
        return f"extern {self.pointer_var(f'p_{self.name}')};"

    def definition(self):
        """Definition for the generated .c: same type, no `extern`, so this
        is the one place the pointer variable actually gets storage."""
        return f"{self.pointer_var(f'p_{self.name}')};"

    def define(self):
        if self.pointer_dims():
            return f"#define {self.name} (*p_{self.name})"
        return f"#define {self.name} p_{self.name}"


def _include_guard_name(lines):
    """Detect the file's own `#ifndef X_H` / `#define X_H` include guard so
    it isn't mistaken for a region/feature #ifdef further down. Only the
    canonical two-line-at-top-of-file form is recognized; anything else
    (e.g. #pragma once) means there's no include guard to exclude."""
    nonblank = [l.strip() for l in lines if l.strip()]
    if len(nonblank) < 2:
        return None
    m1 = IFNDEF_RE.match(nonblank[0])
    if not m1:
        return None
    guard = m1.group(1)
    m2 = re.match(r"^#\s*define\s+(\S+)", nonblank[1])
    if m2 and m2.group(1) == guard:
        return guard
    return None


INCLUDE_RE = re.compile(r'^#\s*include\s+"([^"]+)"')


def parse_header(path):
    symbols = []
    unparsed = []
    includes = []
    guard_stack = []
    lines = Path(path).read_text().splitlines()
    include_guard = _include_guard_name(lines)

    for lineno, raw in enumerate(lines, 1):
        line = raw.strip()
        m = INCLUDE_RE.match(line)
        if m:
            includes.append(m.group(1))
            continue
        m = IFDEF_RE.match(line)
        if m:
            guard_stack.append(m.group(1))
            continue
        m = IFNDEF_RE.match(line)
        if m:
            if m.group(1) == include_guard:
                continue  # the file's own include guard, not a real condition
            guard_stack.append("!" + m.group(1))
            continue
        if ELSE_RE.match(line):
            if guard_stack:
                top = guard_stack[-1]
                guard_stack[-1] = ("!" + top) if not top.startswith("!") else top[1:]
            continue
        if ENDIF_RE.match(line):
            if guard_stack:
                guard_stack.pop()
            continue
        if not line.startswith("extern"):
            continue

        m = DECL_RE.match(line)
        if not m:
            unparsed.append((lineno, raw))
            continue

        guard = guard_stack[-1] if guard_stack else None
        symbols.append(Symbol(m.group("type"), m.group("name"), m.group("dims"), guard))

    return symbols, unparsed, includes


def parse_map(path):
    """Symbol -> GBA address (int), from a binutils ld -Map output.

    Matches lines like:
                    0x085dc148                sChozodiaEscapeShipHeatingUpPal
    """
    addrs = {}
    pattern = re.compile(r"^\s*(0x[0-9a-fA-F]+)\s+([A-Za-z_]\w*)\s*$")
    for line in Path(path).read_text(errors="replace").splitlines():
        m = pattern.match(line)
        if m:
            addrs[m.group(2)] = int(m.group(1), 16)
    return addrs


def emit(symbols, unparsed, header_path, includes, region_maps, out_c, out_h):
    guard_name = "PORT_GEN_" + Path(header_path).stem.upper() + "_ROM_H"
    base = Path(header_path).stem

    h_lines = [
        f"/* Generated by tools/gen_port_rom_offsets.py from {header_path}. Do not edit by hand. */",
        "/* Deliberately does NOT include the original header: that header's",
        " * `extern const T name[..];` declarations would collide with the",
        " * `#define name (*p_name)` macros below if both ever appear in the",
        " * same translation unit. Instead this reproduces the *other*",
        " * includes the original header needed (for types/macros like",
        " * OAM_DATA_SIZE), and is meant to be used INSTEAD of the original",
        " * in any 3DS-port translation unit. */",
        f"#ifndef {guard_name}",
        f"#define {guard_name}",
    ]
    seen_includes = set()
    for inc in ["types.h"] + includes:
        if inc in seen_includes:
            continue
        seen_includes.add(inc)
        h_lines.append(f'#include "{inc}"')
    h_lines.append("")

    regions = sorted(region_maps.keys())

    active_guard = None
    for sym in symbols:
        if sym.guard != active_guard:
            if active_guard is not None:
                h_lines.append("#endif")
            if sym.guard is not None:
                h_lines.append(f"#ifdef {sym.guard}")
            active_guard = sym.guard
        h_lines.append(sym.extern_decl())
        h_lines.append(sym.define())
    if active_guard is not None:
        h_lines.append("#endif")
    h_lines.append("")
    h_lines.append(f"void PortGen_{base}_Init(void);")
    h_lines.append("")
    h_lines.append(f"#endif /* {guard_name} */")
    h_lines.append("")

    c_lines = [
        f"/* Generated by tools/gen_port_rom_offsets.py from {header_path}. Do not edit by hand. */",
        '#include "port_rom.h"',
        f'#include "generated/{base}_rom.h"',
        "",
    ]

    active_guard = None
    for sym in symbols:
        if sym.guard != active_guard:
            if active_guard is not None:
                c_lines.append("#endif")
            if sym.guard is not None:
                c_lines.append(f"#ifdef {sym.guard}")
            active_guard = sym.guard
        c_lines.append(sym.definition())
    if active_guard is not None:
        c_lines.append("#endif")
    c_lines.append("")

    c_lines.append(f"void PortGen_{base}_Init(void) {{")
    c_lines.append("    switch (gRomRegion) {")
    for region in regions:
        c_lines.append(f"        case PORT_ROM_REGION_{region.upper()}: {{")
        addrs = region_maps[region]
        any_emitted = False
        active_guard = None
        for sym in symbols:
            addr = addrs.get(sym.name)
            if addr is None:
                continue
            any_emitted = True
            if sym.guard != active_guard:
                if active_guard is not None:
                    c_lines.append("#endif")
                if sym.guard is not None:
                    c_lines.append(f"#ifdef {sym.guard}")
                active_guard = sym.guard
            c_lines.append(f"            p_{sym.name} = ({sym.cast_type()})Port_ResolveRomData(0x{addr:08x}u);")
        if active_guard is not None:
            c_lines.append("#endif")
        if not any_emitted:
            c_lines.append("            /* no symbols from this header found in this region's .map */")
        c_lines.append("            break;")
        c_lines.append("        }")
    c_lines.append("        default:")
    c_lines.append("            break;")
    c_lines.append("    }")
    c_lines.append("}")
    c_lines.append("")

    Path(out_h).parent.mkdir(parents=True, exist_ok=True)
    Path(out_h).write_text("\n".join(h_lines))
    Path(out_c).write_text("\n".join(c_lines))

    print(f"{header_path}: {len(symbols)} symbols emitted, {len(unparsed)} unparsed lines")
    for lineno, raw in unparsed:
        print(f"  UNPARSED {header_path}:{lineno}: {raw}")
    for region in regions:
        missing = [s.name for s in symbols if s.name not in region_maps[region]]
        if missing:
            print(f"  {region}: {len(missing)}/{len(symbols)} symbols not found in that region's .map "
                  f"(e.g. {missing[:5]})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("header", help="path to include/data/*.h to process")
    ap.add_argument("--map", action="append", default=[], metavar="REGION=path.map",
                     help="region=path.map, may be repeated (e.g. eu=mzm_eu.map)")
    ap.add_argument("--out", required=True, help="output .c path")
    ap.add_argument("--out-header", required=True, help="output .h path")
    args = ap.parse_args()

    region_maps = {}
    for spec in args.map:
        region, _, path = spec.partition("=")
        if not path:
            sys.exit(f"bad --map spec: {spec!r} (expected REGION=path.map)")
        region_maps[region] = parse_map(path)

    symbols, unparsed, includes = parse_header(args.header)
    emit(symbols, unparsed, args.header, includes, region_maps, args.out, args.out_header)


if __name__ == "__main__":
    main()
