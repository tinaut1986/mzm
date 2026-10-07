#!/usr/bin/env python3
"""Name the pointers found in a save state's census.

A debug build writes debug/state-census-<id>.txt next to every save state
(platform/3ds/source/port_save_state.c, SsWriteCensus): the 32-bit words of
EWRAM, IWRAM and the decomp's .data/.bss bracket that look like host pointers,
classified by what they point at.  This script puts names on them using the ELF
of the SAME build, so the pointers can be sorted into the ones a save state
format without host pointers can translate (into the emulated memory arrays or
the ROM image) and the ones it has to rebuild on load (code, constant tables,
the decomp's own globals).

Usage:
    python3 tools/state_census_report.py state-census-1.txt [platform/3ds/mzm-3ds.elf]

The ELF must be the one that wrote the state: the addresses in the census are
that build's.  The tool compares the image bounds in the census header with
the ELF's and warns when they disagree.
"""
import bisect
import collections
import os
import subprocess
import sys

NM = os.path.join(os.environ.get("DEVKITARM", "/opt/devkitpro/devkitARM"), "bin", "arm-none-eabi-nm")


def load_symbols(elf):
    out = subprocess.run([NM, "-n", "-S", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    addrs, syms = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        addr, size, kind, name = int(parts[0], 16), int(parts[1], 16), parts[2], parts[3]
        if kind in "tTdDbBrR":
            addrs.append(addr)
            syms.append((addr, size, kind, name))
    return addrs, syms


def lookup(addrs, syms, value):
    i = bisect.bisect_right(addrs, value) - 1
    if i < 0:
        return None
    addr, size, kind, name = syms[i]
    # A pointer just past a symbol's end still names it (arrays of structs).
    if size and value >= addr + size and value - addr > max(size, 0x40):
        return f"?{name}+0x{value - addr:X}"
    return f"{name}+0x{value - addr:X}" if value != addr else name


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    census = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "platform", "3ds", "mzm-3ds.elf")
    addrs, syms = load_symbols(elf)

    spaces = {}
    words = []
    uncovered = []
    for line in open(census):
        t = line.split()
        if not t:
            continue
        if t[0] == "S":
            spaces[t[1]] = (int(t[2], 16), int(t[3], 16))
        elif t[0] == "W":
            words.append((t[1], int(t[2], 16), int(t[3], 16), t[4], t[5] if len(t) > 5 else ""))
        elif t[0] == "U":
            uncovered.append((t[1], int(t[2])))
        elif t[0] == "#":
            print(line.rstrip())

    for name in ("DDATA", "DBSS"):
        if name in spaces:
            base = spaces[name][0]
            sym = next((s for s in syms if s[3] == ("__ss_data_start" if name == "DDATA" else "__ss_bss_start")), None)
            if sym and sym[0] != base:
                print(f"WARNING: {name} base 0x{base:X} in the census, 0x{sym[0]:X} in the ELF: wrong ELF for this state")

    by_class = collections.defaultdict(list)
    for region, off, value, cls, detail in words:
        # Where the word lives: name the variable for the bracket regions.
        where = f"{region}+0x{off:X}"
        if region in spaces and region in ("DDATA", "DBSS"):
            where = lookup(addrs, syms, spaces[region][0] + off) or where
        if cls == "HOSTARRAY":
            target = detail
        else:
            target = lookup(addrs, syms, value) or f"0x{value:X}"
        by_class[cls].append((where, target, region))

    for cls in ("HOSTARRAY", "BRACKET", "IMAGE", "HEAP"):
        rows = by_class.get(cls, [])
        print(f"\n== {cls}: {len(rows)} words")
        if cls == "HOSTARRAY":
            print("   points into the emulated memory arrays: translatable to a GBA address")
        elif cls == "BRACKET":
            print("   points at another decomp global: needs the variable's name or an offset in a stable layout")
        elif cls == "IMAGE":
            print("   points at code or constant data: needs a symbol table or a rebuild on load")
        else:
            print("   points at the heap or linear memory: runtime objects, rebuilt on load")
        # Group by the variable holding the pointer; that is the unit of work.
        holders = collections.Counter(w[0].split("+")[0] for w in rows)
        for holder, n in holders.most_common(60):
            targets = sorted({w[1].split("+")[0] for w in rows if w[0].split("+")[0] == holder})
            print(f"   {n:5d}  {holder:40s} -> {', '.join(targets[:6])}{' ...' if len(targets) > 6 else ''}")

    if uncovered:
        print("\n== NOT COVERED by the save state's pointer rules (format 4): candidates to review")
        print("   a global that really holds a pointer needs a rule in port_state_ptrs.c;")
        print("   one that only holds tiles, positions or other data is correct as it is")
        for name, n in sorted(uncovered, key=lambda x: -x[1])[:60]:
            print(f"   {n:5d}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
