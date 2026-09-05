"""Reads an mzm-perf-NN.bin capture and says where the frames went.

    python3 tools/perf_report.py <mzm-perf-NN.bin> [more.bin ...]

One capture can hold several conditions: `captureFlags` records the display
style, aspect, 3D slider position and which renderer drew the frame, so the
usual way to take one is to stand still and cycle the settings. This groups
by those flags and reports each group separately, dropping samples either
side of a transition (opening the options menu redraws the bottom screen and
pollutes a second of frames).

Read the GPU column against the 16,675 ms vblank budget, and read `px/frame`
next to it: after the 16x16 block pass cut quads about sixfold for 5% of the
frame, quad count stopped explaining frame time, and whether PIXELS explain
it is the question that decides between removing quads (step B) and removing
overdraw (step C). See docs/3ds-renderer-perf-plan.md.
"""
import struct
import sys

HEADER = "<4I"
HEADER_BYTES = 16
# Field order matches PerfSample in platform/3ds/source/platform_gpu_3ds.c.
FIELDS = [
    "frame", "durUs", "sprites", "visibleSprites", "affineSprites",
    "gpuDrawX100", "gpuProcX100", "cpuTileX100", "cpuUploadX100", "cpuDrawX100",
    "drawCount", "bgItems", "objItems", "blendTransitions", "rendererFlags",
    "captureFlags", "drawnPixels",
]
STYLE = {0: "pixel-perfect", 1: "scaled", 2: "scaled", 3: "?"}
ASPECT = {0: "3:2", 1: "3:2", 2: "stretch", 3: "?"}
# Samples to drop either side of a settings change.
SETTLE = 30
# A group smaller than this is a transition, not a condition.
MIN_GROUP = 60


def percentile(sorted_values, q):
    if not sorted_values:
        return 0.0
    return sorted_values[min(len(sorted_values) - 1, int(len(sorted_values) * q))]


def read(path):
    with open(path, "rb") as f:
        raw = f.read()
    magic, sample_size, count, _ = struct.unpack_from(HEADER, raw, 0)
    tag = bytes((magic >> (8 * i)) & 0xFF for i in range(4)).decode("ascii", "replace")
    # Stride by the header's own sampleSize, never by len(FIELDS): that is
    # what lets a capture from an older build still read, minus whatever
    # columns it predates.
    words = min(len(FIELDS), sample_size // 4)
    rows = []
    for i in range(count):
        off = HEADER_BYTES + i * sample_size
        if off + words * 4 > len(raw):
            break
        values = struct.unpack_from("<%dI" % words, raw, off)
        rows.append(dict(zip(FIELDS, values)))
    return tag, sample_size, rows


def report(path):
    tag, sample_size, rows = read(path)
    print("=== %s  (%s, %d samples, %d B each)" % (path, tag, len(rows), sample_size))
    if not rows:
        print("  empty")
        return

    groups = {}
    for r in rows:
        cf = r["captureFlags"]
        key = (cf & 3, (cf >> 2) & 3, (cf >> 4) & 0x7F, (cf >> 18) & 1)
        groups.setdefault(key, []).append(r)

    for key, group in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        if len(group) < MIN_GROUP:
            continue
        group = group[SETTLE:-SETTLE]
        style, aspect, slider, used_gpu = key
        flags = group[len(group) // 2]["rendererFlags"]
        gpu = sorted((r["gpuDrawX100"] + r["gpuProcX100"]) / 100.0 for r in group)
        cpu = sorted((r["cpuTileX100"] + r["cpuUploadX100"] + r["cpuDrawX100"]) / 100.0 for r in group)
        fps = sorted(1e6 / r["durUs"] for r in group if r["durUs"] > 0)
        # Missing when the capture predates the column (see read()).
        avg = lambda k: (sum(r[k] for r in group) // len(group)) if k in group[0] else -1

        print("  %-14s %-7s slider %3d%%  %s  n=%d" % (
            STYLE.get(style, style), ASPECT.get(aspect, aspect), slider,
            "GPU renderer" if used_gpu else "CPU renderer", len(group)))
        print("     GPU %5.2f ms (p90 %5.2f)   CPU %5.2f ms   FPS %5.1f (p10 %5.1f)   budget 16,675 ms" % (
            percentile(gpu, 0.5), percentile(gpu, 0.9), percentile(cpu, 0.5),
            percentile(fps, 0.5), percentile(fps, 0.1)))
        print("     eyes %d  scissor %d  window %d  haze %d (%d tiles)  layer-cache %s (%d composed)" % (
            (flags >> 10) & 3, flags & 0xFF, (flags >> 8) & 1, (flags >> 9) & 1,
            (flags >> 16) & 0xFFFF, "on" if (flags >> 15) & 1 else "off", (flags >> 12) & 7))
        print("     quads: %d draws, %d BG + %d OBJ per eye, %d blend breaks   px/frame %d" % (
            avg("drawCount"), avg("bgItems"), avg("objItems"), avg("blendTransitions"),
            avg("drawnPixels")))
        if "drawnPixels" not in group[0]:
            print("     (px/frame -1: this capture predates the column)")
        print("     sprites: %d total, %d visible, %d affine" % (
            avg("sprites"), avg("visibleSprites"), avg("affineSprites")))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        report(path)


if __name__ == "__main__":
    main()
