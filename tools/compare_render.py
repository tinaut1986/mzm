#!/usr/bin/env python3
"""Compares the 3DS GPU renderer against the port's own CPU PPU.

One screen dump (DEBUG -> tools -> screen dump) writes both halves of the
comparison at the same instant, sharing one set number NN:

    mzm-dump-NN-left.rgb    what the GPU renderer actually put on screen
    mzm-dump-NN-io.bin      the GBA PPU state it drew that from
    mzm-dump-NN-vram.bin        (+ bgpltt / objpltt / oam)

So a dump validates itself. This script renders the state through the CPU
PPU (platform/3ds/build/rec_render, the oracle -- a decompilation-accurate
software PPU) and diffs it against the GPU output. No golden images to keep
up to date, and no need to reproduce a scene frame-exactly.

That makes it the regression check for renderer work: change
port_gpu_renderer.c, take a handful of dumps around the game, and this says
whether the GPU renderer still agrees with the oracle.

Capture conditions -- the script checks these and refuses rather than
reporting nonsense:
  * 3D slider at 0. Every stereo offset is slider * TierPx(tier), so at 0
    they vanish and the two renderers are comparable at all.
  * Display style PIXEL PERFECT (1:1). Otherwise the GPU output is scaled
    1.5x and filtered, and every pixel "differs".
  * FPS overlay and GBA bezel off. Both are drawn onto the top target by the
    port and have no counterpart in a GBA frame.

Two modes:

  * screen-dump sets (default). Self-validating but only for the build that
    took them, so verifying a change means fresh dumps and fresh gameplay.
  * --replay NN, the repeatable one. The console re-renders the SAVED PPU
    states of mzm-rec-NN.bin through whatever build is installed (REPLAY
    RECORDING in the debug tools menu) and writes mzm-replay-NN-SSSS.rgb;
    this compares each against the CPU PPU's render of the same sample. Same
    frames after every change, and two builds can be diffed on identical
    input.

Usage:
    python3 tools/compare_render.py <dump-dir> [NN ...]
    python3 tools/compare_render.py <dir> --replay 01
"""

import argparse
import os
import re
import subprocess
import sys

GBA_W, GBA_H = 240, 160
# The screen dump is the raw top render target: 240x400, portrait/unrotated.
DUMP_W, DUMP_H = 240, 400
# Landscape after rotating 90 degrees, which is how it appears on screen.
TOP_W, TOP_H = 400, 240
# Where a PIXEL PERFECT frame sits in that 400x240 (port_gpu_renderer.c's
# screenBaseX/screenBaseY for display style 0).
GBA_X, GBA_Y = 80, 40

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
REC_RENDER = os.path.join(ROOT, "platform", "3ds", "build", "rec_render")


def load_ppm(path):
    """Minimal binary-PPM reader: returns (w, h, bytes) with 3 bytes/pixel."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary PPM")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":  # comment line
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    w, h, _maxval = fields
    return w, h, data[pos + 1 :]


def load_dump_rgb(path):
    """The .rgb top-screen dump, rotated to landscape and cropped to the GBA
    frame. Returns (pixels, fills_width) where fills_width says the image was
    not pillarboxed, i.e. the console was NOT in PIXEL PERFECT."""
    with open(path, "rb") as f:
        raw = f.read()
    expect = DUMP_W * DUMP_H * 3
    if len(raw) != expect:
        raise ValueError(f"{path}: expected {expect} bytes, got {len(raw)}")

    # ROTATE_90 as in docs/3ds-debug-tools.md: source is stored top-to-bottom
    # in portrait, and (x, y) landscape comes from portrait row/column
    # (DUMP_W-1-x, y) -- verified there against the on-screen HUD text being
    # upright.
    def portrait_px(px, py):
        off = (py * DUMP_W + px) * 3
        # The dump is BGR, not RGB. Established from the data rather than
        # assumed: interpreting a dump's bytes as BGR matches the colours in
        # its own palette files for 79% of pixels, against 45% as RGB, and
        # the residue in both cases is blend/brightness output that is not a
        # raw palette entry. docs/3ds-debug-tools.md used to show an
        # Image.frombytes("RGB", ...) snippet, so every screenshot
        # reconstructed that way had red and blue swapped -- easy to miss on
        # a grey or green scene, which is how it survived.
        return raw[off + 2 : off + 3] + raw[off + 1 : off + 2] + raw[off : off + 1]

    land = bytearray(TOP_W * TOP_H * 3)
    for y in range(TOP_H):
        for x in range(TOP_W):
            land[(y * TOP_W + x) * 3 : (y * TOP_W + x) * 3 + 3] = portrait_px(DUMP_W - 1 - y, x)

    # Pillarbox check: in PIXEL PERFECT everything outside the centred
    # 240x160 is cleared to black.
    def is_black_column(x):
        return all(land[(y * TOP_W + x) * 3 : (y * TOP_W + x) * 3 + 3] == b"\x00\x00\x00" for y in range(TOP_H))

    fills_width = not (is_black_column(2) and is_black_column(TOP_W - 3))

    crop = bytearray(GBA_W * GBA_H * 3)
    for y in range(GBA_H):
        src = ((y + GBA_Y) * TOP_W + GBA_X) * 3
        crop[y * GBA_W * 3 : (y + 1) * GBA_W * 3] = land[src : src + GBA_W * 3]
    return bytes(crop), fills_width


def compare(ref, got, exact=False):
    """Per-pixel comparison. Returns a dict of findings.

    By default both sides are quantised to 5 bits per channel first. The two
    renderers expand GBA 5-bit colour to 8 bits differently -- mode1 shifts
    (`v << 3`, so 31 -> 248) while the GPU path scales to full range (31 ->
    255) -- which is a real divergence but an invisible one, and comparing
    raw bytes flags every single pixel because of it. GBA palette entries and
    GBA blending are both 5-bit, so any colour difference that actually
    matters survives the quantisation. Pass exact=True to see the raw bytes.
    """
    n = GBA_W * GBA_H
    if not exact:
        ref = bytes(v & 0xF8 for v in ref)
        got = bytes(v & 0xF8 for v in got)
    diff_mask = bytearray(n)
    worst = 0
    total_delta = 0
    ndiff = 0
    minx, miny, maxx, maxy = GBA_W, GBA_H, -1, -1
    for i in range(n):
        a = ref[i * 3 : i * 3 + 3]
        b = got[i * 3 : i * 3 + 3]
        if a == b:
            continue
        d = max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))
        ndiff += 1
        total_delta += d
        worst = max(worst, d)
        diff_mask[i] = min(255, d)
        x, y = i % GBA_W, i // GBA_W
        minx, maxx = min(minx, x), max(maxx, x)
        miny, maxy = min(miny, y), max(maxy, y)
    return {
        "ndiff": ndiff,
        "pct": 100.0 * ndiff / n,
        "worst": worst,
        "mean_delta": (total_delta / ndiff) if ndiff else 0.0,
        "bbox": (minx, miny, maxx, maxy) if ndiff else None,
        "mask": bytes(diff_mask),
    }


def write_report_png(out_path, ref, got, mask):
    """Reference | console | amplified diff, side by side. Needs Pillow; a
    PPM triptych is written instead when it is missing, so the tool still
    works on a bare Python."""
    w = GBA_W * 3
    tri = bytearray(w * GBA_H * 3)
    for y in range(GBA_H):
        row = y * w * 3
        tri[row : row + GBA_W * 3] = ref[y * GBA_W * 3 : (y + 1) * GBA_W * 3]
        tri[row + GBA_W * 3 : row + GBA_W * 6] = got[y * GBA_W * 3 : (y + 1) * GBA_W * 3]
        for x in range(GBA_W):
            d = mask[y * GBA_W + x]
            # Red where it differs, scaled so a 1-step difference is still
            # visible rather than a black pixel nobody notices.
            v = 0 if d == 0 else min(255, 64 + d)
            off = row + (GBA_W * 2 + x) * 3
            tri[off : off + 3] = bytes((v, 0, 0))
    try:
        from PIL import Image

        img = Image.frombytes("RGB", (w, GBA_H), bytes(tri))
        img = img.resize((w * 2, GBA_H * 2), Image.NEAREST)
        img.save(out_path)
        return out_path
    except ImportError:
        ppm = os.path.splitext(out_path)[0] + ".ppm"
        with open(ppm, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (w, GBA_H))
            f.write(bytes(tri))
        return ppm


def discover_replays(replay_dir, slot):
    rx = re.compile(rf"^mzm-replay-{slot}-(\d+)\.rgb$")
    return sorted(
        (m.group(1) for m in (rx.match(n) for n in os.listdir(replay_dir)) if m), key=int
    )


def run_replay(args):
    """Compare a replayed recording against the CPU PPU, sample by sample.

    This is the repeatable half of the harness. A screen dump can only ever
    validate the build that took it -- its -left.rgb is what the renderer
    drew that day -- so verifying a renderer change with dumps means going
    back into the game and reproducing scenes by hand. A replay instead
    re-renders the SAVED PPU states of a recording through whatever build is
    installed (REPLAY RECORDING in the debug tools menu), so the same frames
    can be checked after every change, and two builds can be diffed against
    each other on identical input.
    """
    out_dir = args.out or os.path.join(args.dump_dir, "compare")
    os.makedirs(out_dir, exist_ok=True)
    indices = discover_replays(args.dump_dir, args.replay)
    if not indices:
        sys.exit(f"no mzm-replay-{args.replay}-*.rgb found in {args.dump_dir}")
    rec = args.rec or os.path.join(args.dump_dir, f"mzm-rec-{args.replay}.bin")
    if not os.path.isfile(rec):
        sys.exit(f"missing the recording those frames came from: {rec}\n"
                 f"fetch it from the console, or pass --rec")

    failures = 0
    for idx in indices:
        sample = int(idx)
        print(f"=== rec-{args.replay} sample {sample}")
        r = subprocess.run([REC_RENDER, "rec", rec, out_dir, str(sample)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            print("  SKIP: could not render the reference")
            failures += 1
            continue
        _, _, ref = load_ppm(os.path.join(out_dir, f"frame-{sample:04d}.ppm"))
        try:
            got, fills_width = load_dump_rgb(os.path.join(args.dump_dir, f"mzm-replay-{args.replay}-{idx}.rgb"))
        except (OSError, ValueError) as e:
            print(f"  SKIP: {e}")
            failures += 1
            continue
        if fills_width:
            print("  SKIP: not pillarboxed -- the replay should force PIXEL PERFECT,\n"
                  "        so this file probably predates that override.")
            failures += 1
            continue
        res = compare(ref, got, exact=args.exact)
        if res["ndiff"] == 0:
            print(f"  OK: identical to the CPU PPU across all {GBA_W * GBA_H} pixels")
            continue
        failures += 1
        x0, y0, x1, y1 = res["bbox"]
        print(f"  DIFF: {res['ndiff']} px ({res['pct']:.2f}%), worst {res['worst']}, mean {res['mean_delta']:.1f}")
        print(f"        bounding box x {x0}..{x1}, y {y0}..{y1}")
        report = write_report_png(os.path.join(out_dir, f"replay-{args.replay}-{idx}.png"), ref, got, res["mask"])
        print(f"        report (reference | console | diff): {report}")

    print()
    print(f"{len(indices)} replayed sample(s), {failures} problem(s)")
    return 1 if failures else 0


def discover_sets(dump_dir):
    rx = re.compile(r"^mzm-dump-(\d+)-left\.rgb$")
    return sorted({m.group(1) for m in (rx.match(n) for n in os.listdir(dump_dir)) if m})


def main():
    ap = argparse.ArgumentParser(description="Diff the 3DS GPU renderer against the CPU PPU oracle.")
    ap.add_argument("dump_dir", help="directory holding a fetched mzm-dump-NN-* set")
    ap.add_argument("sets", nargs="*", help="set numbers to check (default: all found)")
    ap.add_argument("--out", default=None, help="where to write reports (default: <dump-dir>/compare)")
    ap.add_argument(
        "--replay",
        metavar="NN",
        default=None,
        help="compare a replayed recording (mzm-replay-NN-*.rgb) against the CPU PPU "
        "instead of screen-dump sets -- the repeatable mode, see run_replay()",
    )
    ap.add_argument(
        "--rec",
        default=None,
        help="path to the mzm-rec-NN.bin those replayed frames came from "
        "(default: <dump-dir>/mzm-rec-NN.bin)",
    )
    ap.add_argument(
        "--exact",
        action="store_true",
        help="compare raw bytes instead of quantising to GBA 5-bit colour first "
        "(shows the renderers' differing 5->8 bit expansion; see compare())",
    )
    args = ap.parse_args()

    if not os.path.isfile(REC_RENDER) and not os.path.isfile(REC_RENDER + ".exe"):
        sys.exit(f"missing {REC_RENDER}\nbuild it first:  make -C platform/3ds rec-render")

    if args.replay:
        return run_replay(args)

    out_dir = args.out or os.path.join(args.dump_dir, "compare")
    os.makedirs(out_dir, exist_ok=True)

    sets = args.sets or discover_sets(args.dump_dir)
    if not sets:
        sys.exit(f"no mzm-dump-NN-left.rgb found in {args.dump_dir}")

    failures = 0
    for set_id in sets:
        print(f"=== set {set_id}")
        # 1. reference: the CPU PPU rendering of this dump's own state
        r = subprocess.run(
            [REC_RENDER, "dump", args.dump_dir, set_id, out_dir],
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            print("  SKIP: could not render the reference")
            failures += 1
            continue
        _, _, ref = load_ppm(os.path.join(out_dir, f"reference-{set_id}.ppm"))

        # 2. what the console actually drew
        try:
            got, fills_width = load_dump_rgb(os.path.join(args.dump_dir, f"mzm-dump-{set_id}-left.rgb"))
        except (OSError, ValueError) as e:
            print(f"  SKIP: {e}")
            failures += 1
            continue
        if fills_width:
            print(
                "  SKIP: this dump is not PIXEL PERFECT (no pillarbox, so the image is\n"
                "        scaled and filtered). Set display style to PIXEL PERFECT and\n"
                "        re-dump -- see the capture conditions in this script's header."
            )
            failures += 1
            continue

        # 3. diff
        res = compare(ref, got, exact=args.exact)
        if res["ndiff"] == 0:
            print(f"  OK: identical to the CPU PPU across all {GBA_W * GBA_H} pixels")
            continue

        failures += 1
        x0, y0, x1, y1 = res["bbox"]
        print(f"  DIFF: {res['ndiff']} px ({res['pct']:.2f}%), worst channel delta {res['worst']}, mean {res['mean_delta']:.1f}")
        print(f"        bounding box x {x0}..{x1}, y {y0}..{y1}")
        if res["pct"] > 90 and res["worst"] <= 8:
            print("        -> tiny delta over nearly every pixel: a colour-correction")
            print("           mismatch or the 5->8 bit expansion, not a renderer bug.")
            print("           Turn colour correction off on the console, and drop --exact.")
        elif y1 < 24 or y0 > GBA_H - 24:
            print("        -> confined to a screen edge: check the FPS overlay is off.")
        report = write_report_png(os.path.join(out_dir, f"compare-{set_id}.png"), ref, got, res["mask"])
        print(f"        report (reference | console | diff): {report}")

    print()
    print(f"{len(sets)} set(s), {failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
