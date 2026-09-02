# Issue #17 — session 2026-09-02: root cause found from a CPU-vs-GPU scene recording

Previous sessions (see `3ds-issue17-session-2026-08-25.md`) verified tile decode,
cache-slot bookkeeping, UV/draw placement and CPU/GPU frame-overlap
synchronisation as individually correct on hardware and still could not explain
Samus's death animation rendering wrong through the GPU renderer. The scene was
force-fallen-back to the CPU scanline renderer as a symptom-level workaround.

That workaround has now been removed and this session found the cause.

## The recording

One capture, two deaths, taken with the runtime GPU/CPU renderer toggle:
death #1 in CPU mode, death #2 in GPU mode. 162 samples (`MZM4`,
101668 B/record) plus 41 companion `.rgb` screenshots at 1-in-4.

- CPU death ≈ samples 12–46
- GPU death ≈ samples 100–147

## What the recording proves

**The emulated state is identical between the two runs.** Matching on the Samus
death phase (`header[2:5]`, i.e. area / room / phase), every register matches
exactly:

    dispcnt=5000  bldcnt=3f40  bldalpha=0010  bldy=0000
    win1h=00f0  win1v=00a0  winin=3f00  winout=0020  sprites=128

Only OBJ + WIN1 are enabled; WIN1 covers the whole screen with all six layers
and *no* colour effect (WININ bit 5 clear). No BG layer is enabled at all — so
everything the player sees behind Samus is the **backdrop**, BG palette entry 0.

**The screen output does not match.** At the same phase:

| phase `(51,24,N)` | backdrop in the recording | CPU screen | GPU screen |
|---|---|---|---|
| N=2 | (22,22,22) | (22,22,22) | **(0,0,0)** |
| N=4 | (31,31,31) | (31,31,31) | **(0,0,0)** |
| N=6 | (31,31,31) | (31,31,31) | **(0,0,0)** |

Frame-to-frame diffs of the GPU run's screenshots show the entire top screen
frozen from sample 120 on — the only changing pixels are the FPS counter's
glyph box.

Note the `.rgb` dumps are **BGR**-ordered, and the GPU path expands RGB5→RGB8 as
`(v<<3)|(v>>2)` while `mode1.c` (no colour correction) uses `v<<3`. That
difference is how this session confirmed the GPU-run frames really were drawn by
the GPU renderer and not by a silent CPU fallback. Both are cosmetic, neither is
the bug.

## Cause 1 — the backdrop was never rendered

`Port_GpuRenderer_RenderFrame` cleared each eye target to hardcoded
`C2D_Color32(0,0,0,255)`. On the GBA, any pixel not covered by an enabled layer
shows BG palette entry 0. Normally invisible, because rooms cover the screen in
BG tiles — but the death animation enables no BG layer at all and fades the
backdrop to white through palette RAM. The whole fade was being dropped.

Fixed by clearing to `BackdropClearColor()` (`gBgPltt[0]`, same 5→8 expansion as
the atlas). `mode1.c` uses `mode1_bg_abgr_lut[0]` raw for the same purpose — no
BLDCNT brighten/darken applied to the backdrop — so the two now match exactly.
The letterbox/pillarbox masks drawn at the end of the eye pass re-blacken
everything outside the 240x160 frame, so filling the whole target is safe in
every display style.

## Cause 2 — the PICA200's texture cache never got invalidated

Samus's on-screen colours in the GPU run come from **earlier palette phases**.
Attributing each distinct colour in one captured frame (shot 124) back to the
sample whose OBJ bank 1 contains it:

    (12,7,3) (4,21,7) (0,13,0) (20,3,0) (0,31,14) (31,12,9) (18,13,0)  -> samples ~97-102
    (0,6,9) (13,13,13) (23,23,23) (8,8,8)                              -> samples ~109-119

Two different freeze times **inside a single frame**. That is per-slot
staleness — and it is not staleness in *our* cache: `GetOrDecodeTileSlot`'s
`sCachePalHash` check does fire, and redecodes those slots in place correctly.

The gap is between the CPU write and the GPU read:

- `FlushAtlasRange` (`svcFlushProcessDataCache`) makes the new texels visible in
  memory. It says nothing about the PICA200's own texture cache.
- citro2d calls `C3D_TexBind` only when the texture *pointer* changes between
  draws. Every draw in this renderer uses the one shared atlas, so after the
  first frame the bind never happens again — and neither does the texture-unit
  config write that carries `GPUREG_TEXUNIT_CONFIG`'s cache-clear bit.
- Any slot redecoded **in place** therefore keeps drawing with whatever texels
  the GPU cached the last time it sampled that slot.

Why it hid for so long: a gameplay frame samples hundreds of distinct tiles
across a 1 MB atlas, so the GPU's texture cache is thrashed constantly and stale
entries are evicted before anyone sees them. It only becomes visible when the
working set is tiny *and* the palette is what changes — exactly the death
animation: no BG layers, a handful of sprite tiles whose VRAM bytes never
change, and a palette rewritten every frame. It also explains why every earlier
check passed: the decode, the cache bookkeeping, the UVs and the frame
synchronisation were all genuinely correct. The correct pixels were sitting in
memory the whole time; the GPU just wasn't reading them.

Fixed by calling `C3D_TexBind(0, &sAtlasTexture)` once per frame, immediately
after the dirty-row flush, on frames that actually rewrote a slot.

## Status

**Fixed and confirmed on hardware (2026-09-02).** Both fixes are in
`platform/3ds/source/port_gpu_renderer.c`, and the CPU-fallback workaround in
`Port_GpuRenderer_CanRenderFrame` has been removed -- the death scene now
renders correctly through the GPU path.
