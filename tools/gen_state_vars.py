#!/usr/bin/env python3
"""Fill the save state variable table of a linked 3DS ELF.

A save state stores the decomp's mutable globals by NAME, so it survives builds
that add, remove or move globals.  The names only exist in the ELF's symbol
table, and many of the variables are file-local statics no C code can name,
so the table is built from the linked ELF and patched into it:

  * platform/3ds/source/port_state_blob.c reserves a zero-filled array
    (gStateVarBlob, STATE_VAR_BLOB_SIZE bytes);
  * after the link, this script reads the symbols that fall inside the two save
    state brackets (__ss_data_start..__ss_data_end and __ss_bss_start..
    __ss_bss_end, see platform/3ds/3dsx.ld) and overwrites that array in the ELF
    file.  Nothing moves: the array keeps its size and address.

The table is read back by port_state_vars.c.  Its layout (little endian):

    u32 magic 'SVT1', u32 count, u32 nameBytes, u32 reserved
    count x { u32 nameOffset, u32 offsetAndRegion, u32 size }
        offsetAndRegion: bit 31 set = .bss bracket, else .data; the rest is the
        byte offset of the variable from the start of its bracket.
    nameBytes of NUL-terminated names

A name is the symbol with a trailing `.<digits>` removed (the compiler's suffix
on function-local statics, which changes between builds); when that leaves two
symbols with the same name, the later one gets `#<n>` appended.

Usage:  python3 tools/gen_state_vars.py <elf> [--nm PATH] [--objcopy PATH]
"""
import argparse
import os
import re
import struct
import subprocess
import sys

SYMBOL = "gStateVarBlob"
MAGIC = 0x31545653  # 'SVT1'


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    base = os.path.join(os.environ.get("DEVKITARM", "/opt/devkitpro/devkitARM"), "bin", "arm-none-eabi-")
    ap.add_argument("--nm", default=base + "nm")
    ap.add_argument("--objcopy", default=base + "objcopy", help="unused, kept for the Makefile")
    ap.add_argument("--readelf", default=base + "readelf")
    args = ap.parse_args()

    syms = []
    marks = {}
    for line in run([args.nm, "-n", "-S", "--defined-only", args.elf]).splitlines():
        p = line.split()
        if len(p) == 3 and p[2].startswith("__ss_"):
            marks[p[2]] = int(p[0], 16)
        elif len(p) == 4 and p[2] in "dDbB":
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    for need in ("__ss_data_start", "__ss_data_end", "__ss_bss_start", "__ss_bss_end"):
        if need not in marks:
            sys.exit(f"gen_state_vars: {need} not found in {args.elf}")

    entries = []
    seen = {}
    for addr, size, name in syms:
        if size == 0:
            continue
        if marks["__ss_data_start"] <= addr < marks["__ss_data_end"]:
            region, off = 0, addr - marks["__ss_data_start"]
        elif marks["__ss_bss_start"] <= addr < marks["__ss_bss_end"]:
            region, off = 1, addr - marks["__ss_bss_start"]
        else:
            continue
        key = re.sub(r"\.\d+$", "", name)
        n = seen.get(key, 0)
        seen[key] = n + 1
        if n:
            key = f"{key}#{n}"
        entries.append((key, region, off, size))

    names = b""
    table = b""
    for key, region, off, size in entries:
        table += struct.pack("<III", len(names), (region << 31) | off, size)
        names += key.encode() + b"\0"
    blob = struct.pack("<IIII", MAGIC, len(entries), len(names), 0) + table + names

    # Where the reserved array sits in the file: its symbol gives the address and
    # size, the section table the file offset of the section that holds it.
    sym = None
    for line in run([args.nm, "-n", "-S", "--defined-only", args.elf]).splitlines():
        p = line.split()
        if len(p) == 4 and p[3] == SYMBOL:
            sym = (int(p[0], 16), int(p[1], 16))
    if sym is None:
        sys.exit(f"gen_state_vars: no {SYMBOL} in {args.elf} (is port_state_blob.c linked?)")
    addr, cap = sym
    off = None
    for line in run([args.readelf, "-S", "-W", args.elf]).splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m:
            sec_addr, sec_off, sec_size = int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)
            if sec_addr <= addr and addr + cap <= sec_addr + sec_size:
                off = sec_off + (addr - sec_addr)
    if off is None:
        sys.exit(f"gen_state_vars: {SYMBOL} is not inside a PROGBITS section of {args.elf}")
    if len(blob) > cap:
        sys.exit(f"gen_state_vars: table needs {len(blob)} bytes, {SYMBOL} holds {cap}: raise STATE_VAR_BLOB_SIZE")
    blob += b"\0" * (cap - len(blob))

    with open(args.elf, "r+b") as f:
        f.seek(off)
        f.write(blob)
    print(f"gen_state_vars: {len(entries)} variables, {len(names)} bytes of names, {len(blob)} reserved")


if __name__ == "__main__":
    main()
