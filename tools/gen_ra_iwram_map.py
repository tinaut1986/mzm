#!/usr/bin/env python3
"""Generate the RetroAchievements IWRAM address map for the 3DS port.

RetroAchievements addresses GBA memory as IWRAM at 0x0000-0x7fff (GBA
0x03000000) followed by EWRAM from 0x8000 (GBA 0x02000000).  EWRAM is easy:
platform/3ds/ewram_symbols.ld already places the decomp's EWRAM globals at
their real offsets inside gEwram, so gEwram[addr - 0x8000] is genuinely the
right byte.

IWRAM is not.  Its globals are ordinary C variables in src/globals1.c and
src/globals2.c, placed by the host compiler wherever it likes, so an RA
IWRAM address means nothing on its own.  On the GBA they land at fixed
offsets because agbcc emits them into the `iwram_data` section in
declaration order, packed at their natural alignment (see linker.ld).

This script reconstructs that GBA layout -- declaration order, natural
alignment -- and pairs each variable with its address in *this* build, so
the RA memory-read callback can translate one into the other.  It emits
platform/3ds/source/port_ra_iwram_map.c, which is committed: the generator
needs a cross compiler and the network, a plain build needs neither.

Sizes and alignments are not guessed.  A probe translation unit is compiled
with the real build flags and asked for sizeof, __alignof__ and
__builtin_classify_type of every variable; the values are read back out of
the object file, since the probe is cross-compiled for the 3DS and cannot be
run here.  Alignment is then the type's natural alignment, raised to four
bytes for aggregates -- structs, unions and arrays.  That last rule is not a
guess either: it is what ARM GCC's ARM_EXPAND_ALIGNMENT does, and laying the
declarations out under it reproduces the addresses the code notes give.

The walk is also floored by this build's own layout.  Compiling globals1.c
and globals2.c with -fno-toplevel-reorder makes devkitARM's GCC emit the
same declaration-ordered, word-aligned-aggregate layout agbcc produced, and
where the two disagree it is always GCC leaving a byte more room, never
less.  So a variable is placed at whichever is later, the sequential walk or
GCC's own offset for it: an anchor's correction then propagates forward only
until the compiler's layout catches up again, which is what the notes show
happening.

The result is checked against RetroAchievements' own public code notes
(dorequest.php?r=codenotes2), which describe ~120 of these addresses in
English, in two ways:

  * no noted address may land in padding between two variables -- notes
    describe real state, so a note in a gap means the walk has drifted;
  * every pairing in EXPECTED, read off the notes by hand, must hold.

Both are hard errors.  Where a note proves the layout drifts anyway -- a
struct the decomp declares a byte short of the original, say -- an ANCHORS
entry pins that variable to the address the notes give and the walk resumes
from there.

Usage:
    DEVKITARM=/opt/devkitpro/devkitARM python3 tools/gen_ra_iwram_map.py
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLOBALS = ["src/globals1.c", "src/globals2.c"]
OUT_C = "platform/3ds/source/port_ra_iwram_map.c"
NOTES_URL = "https://retroachievements.org/dorequest.php?r=codenotes2&g=534"

IWRAM_SIZE = 0x8000

# __builtin_classify_type results for record, union and array.
AGGREGATE_TYPE_CLASSES = (12, 13, 14)

# Variables pinned to an address the code notes prove, overriding the
# sequential walk from the previous variable.  Each one is a place where the
# decomp's declarations do not reproduce the original layout byte for byte;
# the walk continues normally from the anchor.  Keep the evidence.
ANCHORS = {
    # 0x2c "[8-bit] Difficulty", 0x2d "Mothership Doors switch", 0x2e "Time
    # Attack switch" -- one byte more precedes gDifficulty on the original
    # than the decomp's 3-byte struct GameCompletion accounts for.  The slack
    # is reabsorbed by the padding before gMusicTrackInfo, which the notes
    # independently place back at 0x30.
    "gDifficulty": 0x2C,
    # 0x63 "Number of Items collected in Brinstar" .. 0x69 "... in Chozodia":
    # seven of the eight area counters, so the array starts at 0x63.  GCC
    # gives a u8[8] four-byte alignment, agbcc did not.
    "gNumberOfItemsCollected": 0x63,
    # 0x95c "[8-bit] Escape Timer Flag", 0x95d "Escape Timer - Frame Timer",
    # then 0x95e-0x963 for the six digits, which is gCurrentEscapeStatus,
    # gEscapeTimerCounter and gEscapeTimerDigits in that order.  Anchoring
    # the first satisfies the flag and the frame timer.
    "gCurrentEscapeStatus": 0x95C,
    # ...and the digits themselves follow immediately, 0x95e-0x963.  Both
    # compilers put struct EscapeDigits on a word boundary even though it is
    # declared PACKED; the original did not.
    "gEscapeTimerDigits": 0x95E,
    # 0x13d4 "[8-bit] Samus Pose" through 0x13f5, matching struct SamusData
    # field for field -- 0x13e6 "[16-bit] Samus Horizontal Coordinates" is
    # xPosition at +18, and gSamusDataCopy lands on 0x13f4 "Previous
    # Animation State" / 0x13f5 "Previous Collision State".
    "gSamusData": 0x13D4,
    # 0x1530 "[16-bit] Maximum Energy" .. 0x1542 "[8-bit] Suit Type": the
    # whole of struct Equipment, field for field, starting four bytes below
    # where this build's compiler puts it.  0x153c, the unlocked-upgrades
    # bitfield the Charge Beam achievement reads, depends on this.
    "gEquipment": 0x1530,
    # 0x13f4 "[8-bit] Previous Animation State" / 0x13f5 "Previous Collision
    # State" -- pose and standingStatus of the previous frame's copy.  It
    # needs its own anchor because gSamusData above it moved earlier, and the
    # compiler's own offset would otherwise pull this one back.
    "gSamusDataCopy": 0x13F4,
    # 0x1418 "[8-bit] Weapon Cooldown".  Pinned so that gEquipment being
    # pulled down to 0x1530 does not drag the variables between them below
    # where the notes put this one.
    "gSamusWeaponInfo": 0x1418,
}

# Pairings read directly off the code notes.  These are the assertions that
# make the reconstruction checkable rather than plausible.
EXPECTED = {
    0x0002: "gFrameCounter16Bit",      # [16-bit] Frame Counter from Boot
    0x0004: "gStereoFlag",             # [8-bit] Sound Option
    0x0005: "gSubGameModeStage",       # [8-bit] Pause Menu Transition State
    0x0014: "gFileScreenOptionsUnlocked",  # [8-bit] Gallery Unlocks
    0x0020: "gLanguage",               # [8-bit] Language
    0x0024: "gGameCompletion",         # Completed Game Map switch
    0x002C: "gDifficulty",             # [8-bit] Difficulty
    0x0030: "gMusicTrackInfo",         # Current Room's Music
    0x004A: "gHideHud",                # [8-bit] HUD Visibility Flag
    0x0054: "gCurrentArea",            # [8-bit] Area ID
    0x0055: "gCurrentRoom",            # [8-bit] Room ID
    0x0058: "gDisplayLocationText",    # Room Pop-up
    0x0059: "gMinimapX",               # [8-bit] Minimap Horizontal Coordinate
    0x005A: "gMinimapY",               # [8-bit] Minimap Vertical Coordinate
    0x0063: "gNumberOfItemsCollected", # [8-bit] Items collected in Brinstar
    0x0150: "gBestCompletionTimes",    # [8-bit] In-Game Timer - Hours
    0x01A8: "gSpriteData",             # [16-bit] Chozodia Alert Timer
    0x095C: "gCurrentEscapeStatus",    # [8-bit] Escape Timer Flag
    0x095E: "gEscapeTimerDigits",      # [8-bit] Escape Timer - 1st Digit of Centiseconds
    0x0C70: "gMainGameMode",           # [8-bit] Game State
    0x137C: "gButtonInput",            # [8-bit] Input Map
    0x137E: "gPreviousButtonInput",    # [16-bit] Input Map Copy
    0x1380: "gChangedInput",           # [16-bit] Input Map Single Frame
    0x13D4: "gSamusData",              # [8-bit] Samus Pose
    0x13F4: "gSamusDataCopy",          # [8-bit] Previous Animation State
    0x1418: "gSamusWeaponInfo",        # [8-bit] Weapon Cooldown
    0x1530: "gEquipment",              # [16-bit] Maximum Energy
}

# Noted addresses that legitimately do not describe the variable they sit in.
NOTE_EXCEPTIONS = {
    # A header the note author left on the first address ("Credit to @Brian
    # and Ghal416 for the original code notes"), not a description of 0x0000.
    0x0000,
    # 0x1414-0x1417 ("L-Aiming State", "Shot Type", "Equipped Sub-Weapon
    # Type", "Selected Missile Type") sit just past the end of
    # gSamusDataCopy: the original's SamusData carries four bytes that the
    # decomp's struct does not declare, so there is nothing here to point
    # at.  No achievement in set 534 reads them.
    0x1414, 0x1415, 0x1416, 0x1417,
}


def parse_declarations(path):
    """Variable names carrying IWRAM_DATA, in source order.

    Every conditional in these two files is `#ifdef RAM_PADDING`, which the
    3DS build defines (include/config.h, and MODERN is off), so the
    declarations can be taken as they come.
    """
    names = []
    with open(os.path.join(ROOT, path), encoding="utf-8") as handle:
        for line in handle:
            if not line.startswith("IWRAM_DATA"):
                continue
            decl = line[len("IWRAM_DATA"):].split("=")[0].split(";")[0].split("[")[0]
            identifiers = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", decl)
            if not identifiers:
                raise SystemExit("cannot read a name out of: " + line.rstrip())
            names.append(identifiers[-1])
    return names


def probe_metrics(names, cc, cflags):
    """sizeof/__alignof__ per variable, straight from the cross compiler."""
    build = os.path.join(ROOT, "platform/3ds/build/ra_probe")
    os.makedirs(build, exist_ok=True)

    # The probe includes the two translation units whole rather than just
    # their headers: the padding variables (gUnk_300000C and friends) are
    # declared nowhere else, and they take up space like any other.
    body = ['#include "%s"' % os.path.join(ROOT, path).replace("\\", "/") for path in GLOBALS]
    body += ["", "const unsigned rc_probe[] = {"]
    for name in names:
        body.append("    (unsigned)sizeof(%s), (unsigned)__alignof__(%s),"
                    " (unsigned)__builtin_classify_type(%s)," % (name, name, name))
    body.append("};")

    src = os.path.join(build, "probe.c")
    with open(src, "w", encoding="utf-8") as handle:
        handle.write("\n".join(body) + "\n")

    obj = os.path.join(build, "probe.o")
    subprocess.run([cc] + cflags + ["-c", src, "-o", obj], check=True)

    raw = os.path.join(build, "probe.bin")
    objcopy = os.path.join(os.path.dirname(cc), "arm-none-eabi-objcopy")
    subprocess.run([objcopy, "-O", "binary", "--only-section=.rodata", obj, raw], check=True)
    with open(raw, "rb") as handle:
        data = handle.read()

    want = len(names) * 12
    if len(data) < want:
        raise SystemExit("probe produced %d bytes, expected %d" % (len(data), want))
    values = struct.unpack("<%dI" % (want // 4), data[:want])
    return {name: values[i * 3:i * 3 + 3] for i, name in enumerate(names)}


def compiler_offsets(cc, cflags):
    """Where devkitARM's GCC puts each variable, in declaration order.

    -fno-toplevel-reorder stops GCC emitting the definitions backwards, at
    which point its `iwram_data` layout is agbcc's layout in all but a few
    places.  globals2.c follows globals1.c; on the GBA a little libgcc .bss
    sits between them (see linker.ld), but that is far past the last address
    RetroAchievements looks at, so it is not modelled.
    """
    build = os.path.join(ROOT, "platform/3ds/build/ra_probe")
    os.makedirs(build, exist_ok=True)
    nm = os.path.join(os.path.dirname(cc), "arm-none-eabi-nm")

    offsets = {}
    base = 0
    for path in GLOBALS:
        obj = os.path.join(build, os.path.basename(path).replace(".c", ".o"))
        subprocess.run([cc] + cflags + ["-fno-toplevel-reorder", "-c",
                                        os.path.join(ROOT, path), "-o", obj], check=True)
        end = base
        for line in subprocess.run([nm, "-S", obj], check=True,
                                   capture_output=True, text=True).stdout.splitlines():
            fields = line.split()
            if len(fields) != 4:
                continue
            offset, size = int(fields[0], 16), int(fields[1], 16)
            offsets[fields[3]] = base + offset
            end = max(end, base + offset + size)
        base = (end + 3) & ~3
    return offsets


def lay_out(names, metrics, compiled):
    """Walk the declarations the way agbcc did: in order, aggregates on words."""
    layout = []
    offset = 0
    for name in names:
        size, align, type_class = metrics[name]
        align = max(align, 1)
        # ARM_EXPAND_ALIGNMENT: aggregates -- record, union, array -- are
        # placed on a word boundary even when their type does not need it.
        if type_class in AGGREGATE_TYPE_CLASSES and align < 4:
            align = 4
        if name in ANCHORS:
            offset = ANCHORS[name]
        else:
            if offset % align:
                offset += align - (offset % align)
            offset = max(offset, compiled.get(name, 0))
        layout.append((offset, size, name))
        offset += size
    if offset > IWRAM_SIZE:
        raise SystemExit("layout overflows IWRAM: 0x%x > 0x%x" % (offset, IWRAM_SIZE))

    # An anchor that pulls a variable earlier leaves the ones declared before
    # it sitting too high -- the forward walk had no way to know the
    # correction was coming.  Push them back down, in reverse, so declaration
    # order still holds.
    for index in range(len(layout) - 2, -1, -1):
        offset, size, name = layout[index]
        if name in ANCHORS:
            continue  # the notes put it here; nothing else may move it
        limit = layout[index + 1][0] - size
        if offset > limit:
            layout[index] = (limit, size, name)

    # Where two entries still overlap, the decomp declares a variable a few
    # bytes longer than the original did.  Trim the earlier one so the table
    # stays a partition of the address space and the later -- note-backed --
    # variable owns the overlap.  The bytes lost are ones the original did
    # not have.
    trimmed = []
    for index, (offset, size, name) in enumerate(layout):
        if index + 1 < len(layout):
            size = min(size, layout[index + 1][0] - offset)
        if size > 0:
            trimmed.append((offset, size, name))
    return trimmed


def load_notes(path):
    if path:
        with open(path, encoding="utf-8") as handle:
            payload = json.load(handle)
    else:
        with urllib.request.urlopen(NOTES_URL, timeout=60) as response:
            payload = json.loads(response.read().decode("utf-8"))
    if not payload.get("Success"):
        raise SystemExit("code notes request failed: %r" % payload)
    return payload["CodeNotes"]


def check(layout, notes):
    """Fail if the reconstruction disagrees with the published code notes."""
    spans = sorted((off, off + max(size, 1), name) for off, size, name in layout)
    starts = {}
    for offset, _, name in layout:
        starts.setdefault(offset, name)

    problems = []
    for address, name in sorted(EXPECTED.items()):
        got = starts.get(address)
        if got != name:
            problems.append("0x%04x: expected %s to start here, found %s"
                            % (address, name, got or "padding"))

    checked = 0
    for note in notes:
        address = int(note["Address"], 16)
        if address >= IWRAM_SIZE or address in NOTE_EXCEPTIONS:
            continue
        if not any(start <= address < end for start, end, _ in spans):
            problems.append("0x%04x: falls in padding, but is documented as %r"
                            % (address, note["Note"].splitlines()[0][:60]))
            continue
        checked += 1
    return checked, problems


def emit(layout, checked, out_path):
    lines = [
        "/* GENERATED by tools/gen_ra_iwram_map.py -- do not edit by hand.",
        " *",
        " * Maps a RetroAchievements IWRAM address (GBA 0x03000000 + offset) onto",
        " * the decomp global that lives there, so rcheevos reads the same bytes the",
        " * achievement author was looking at.  The generator explains how the GBA",
        " * layout is reconstructed and how it is checked; regenerate with:",
        " *",
        " *     DEVKITARM=/opt/devkitpro/devkitARM python3 tools/gen_ra_iwram_map.py",
        " *",
        " * Agrees with %d RetroAchievements code notes as of generation." % checked,
        " */",
        "",
        '#include "port_ra_iwram_map.h"',
        "",
    ]
    for _, size, name in layout:
        if size:
            lines.append("extern unsigned char %s[];" % name)
    lines += [
        "",
        "/* Sorted by address, so a lookup can binary-search it. */",
        "const PortRaIwramEntry gPortRaIwramMap[] = {",
    ]
    for offset, size, name in layout:
        if size:
            lines.append("    { 0x%04x, %u, %s }," % (offset, size, name))
    lines += [
        "};",
        "",
        "const unsigned int gPortRaIwramMapCount =",
        "    (unsigned int)(sizeof(gPortRaIwramMap) / sizeof(gPortRaIwramMap[0]));",
        "",
    ]
    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--notes", help="local copy of the codenotes2 JSON")
    parser.add_argument("--cc", default=os.environ.get("RA_MAP_CC"))
    parser.add_argument("--dump", action="store_true", help="print the layout")
    args = parser.parse_args()

    devkitarm = os.environ.get("DEVKITARM", "/opt/devkitpro/devkitARM")
    cc = args.cc or os.path.join(devkitarm, "bin", "arm-none-eabi-gcc")

    cflags = [
        "-O2", "-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft",
        "-D__3DS__", "-DMZM_3DS", "-DNON_MATCHING=1", '-DMZM_PORT_VERSION="0"',
        "-w", "-Wno-implicit-function-declaration",
        "-I" + os.path.join(ROOT, "port/generated/shadow"),
        "-I" + ROOT,
        "-I" + os.path.join(ROOT, "include"),
        "-I" + os.path.join(ROOT, "port"),
        "-I" + os.path.join(ROOT, "port/generated"),
        "-I" + os.path.join(ROOT, "port/ppu/include"),
    ]

    names = []
    for path in GLOBALS:
        names += parse_declarations(path)

    layout = lay_out(names, probe_metrics(names, cc, cflags),
                     compiler_offsets(cc, cflags))

    if args.dump:
        for offset, size, name in layout:
            print("%04x %5d %s" % (offset, size, name))

    checked, problems = check(layout, load_notes(args.notes))
    if problems:
        print("\n".join(problems), file=sys.stderr)
        raise SystemExit("%d disagreement(s) with the code notes" % len(problems))

    emit(layout, checked, os.path.join(ROOT, OUT_C))
    print("wrote %s: %d variables, %d code notes agree" % (OUT_C, len(layout), checked))


if __name__ == "__main__":
    main()
