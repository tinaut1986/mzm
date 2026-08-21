# GPU tile renderer: closing the CPU-fallback gap (window clipping, affine BG/OBJ, mosaic)

Branch: `feat/gpu-renderer-window-affine-mosaic`, based on `main`
(`c45ecef2`, after the audio-decouple work). Investigation only in this
first commit -- no rendering code changed yet. See "Recommendation" at the
bottom for what to build first and why nothing was implemented blind.

## The ask

With `RENDERER=gpu` now the default (`platform/3ds/Makefile`), most gameplay
renders via `port_gpu_renderer.c`'s PICA200 tile/sprite compositor. But
`Port_GpuRenderer_CanRenderFrame()` rejects a frame outright -- falling back
to the CPU scanline renderer (`port/ppu/src/mode1.c`) for that frame -- for
several GBA PPU features it doesn't implement. Reported symptoms this
investigation was asked to explain:

1. An intro scene falls back to CPU.
2. A menu selection that appears to "crop" something falls back to CPU.
3. The idle/attract-mode gameplay demo (plays after leaving the title
   screen untouched) falls back to CPU, but only sometimes.

## What actually rejects a frame today

`Port_GpuRenderer_CanRenderFrame()` (`platform/3ds/source/port_gpu_renderer.c:826-872`):

| Rejection | Trigger | Likely matches |
|---|---|---|
| `forced blank` | DISPCNT bit 7 | scene transitions hiding VRAM writes |
| `mode != 0` | DISPCNT mode 1/2 (affine BG active) | **(1) intro** -- GBA intros commonly use an affine BG for zoom/rotate logo effects; also the Tourian self-destruct sequence per `docs/3ds-port-ppu-audit.md` |
| `WIN0` / `WIN1` | window enabled AND actually clips (not the full-screen WININ-only-gating no-op case already allowed through) | **(2) menu crop** -- a shrinking/expanding window rect is a standard GBA menu-open transition; also `gSuitFlashEffect`'s shrunk WIN1 rect (mentioned in the existing code comment at line ~838) is exactly the kind of gameplay effect that would explain **(3) "sometimes" during idle gameplay** |
| `OBJWIN` | DISPCNT bit 15 | rare (object-shaped window mask); not investigated further, no evidence it's involved here |
| `mosaic BG` | any visible BG has BGCNT mosaic bit set while MOSAIC reg is nonzero | possible for **(3)**, e.g. a pixelation transition |
| `affine OBJ` | any live OAM entry has the affine flag set | **confirmed occurring** -- see below |

### What I actually measured

Built with `EXTRA_CFLAGS=-DPORT_GPU_RENDERER_DIAG_LOG` and ran two headless
Azahar sessions (45s, then 90s) via `tools/run_azahar_test.sh`, covering boot
through the intro text crawl, title/file-select, and into an in-game demo
(`GM 0x0B`, confirmed via the debug log's `ModeChange` lines). Only
`GPU_REJECT: affine OBJ` appeared (2 log lines; logging is throttled to
1-in-30 rejected frames, so the real reject count is higher but the frame
class is confirmed).

This is a **partial** measurement, not a full confirmation of all three
symptoms:
- No input automation is available in this environment (no `xdotool` or
  equivalent), so the menu-crop case (which needs an actual menu open/close)
  was never exercised.
- The run may not have lingered on whatever affine-BG intro moment exists
  long enough, or that scene may be shorter than expected, or Zero
  Mission's intro may not actually use an affine BG (this needs
  re-checking against `docs/3ds-port-ppu-audit.md` and/or a longer/targeted
  capture).
- 90s did not appear to reach the actual idle-timeout attract-mode demo
  (title screen inactivity timer); the `GM 0x0B` reached was likely a
  cutscene-triggered in-game sequence, not the attract-mode demo per se.

**Next session with input automation or manual play should re-run the same
diagnostic build and specifically: sit on the title screen until attract
mode starts, open an in-game menu, and step through the intro slowly**, to
get a real reason breakdown for all three reported symptoms instead of just
the one confirmed here (affine OBJ).

## Feasibility per rejection category

### Window clipping (WIN0/WIN1) -- recommended first target

The PICA200's scissor test is a direct hardware match for GBA's window
rectangles: `C3D_SetScissor(GPU_SCISSORMODE mode, left, top, right, bottom)`
(`libctru/include/c3d/base.h`) supports `GPU_SCISSOR_NORMAL` (keep pixels
*inside* the rect) and, importantly, **`GPU_SCISSOR_INVERT`** (keep pixels
*outside* the rect) -- which maps exactly onto GBA's WININ (layers visible
inside the window) vs WINOUT (layers visible outside it) semantics, no
stencil buffer needed.

Scope that's actually tractable in one pass:
- **Single active window only** (WIN0 *xor* WIN1). Two simultaneously active
  windows with different rects isn't a single scissor rectangle (WIN1-but-
  not-WIN0 is a rect-minus-a-rect, non-convex) -- keep falling back to CPU
  for that combination, consistent with this file's existing "narrow scope,
  correctness over coverage" philosophy (see its header comment).
- **Layer visibility only**, not the window's separate "special effect
  enable" bit (WININ/WINOUT bit 5, which can gate BLDCNT independently of
  layer visibility). Approximating this away (treat blend as window-
  independent, same as it's computed today) is consistent with the
  existing EVA+EVB~=16 blend approximation already documented in
  `Port_GpuRenderer_RenderFrame`'s comments.
- Implementation shape: give `DrawItem` a small enum (`ALWAYS` /
  `INSIDE_ONLY` / `OUTSIDE_ONLY`) computed once per layer at collection time
  from WININ/WINOUT (`CollectBgLayer`/`CollectSprite`, mirroring how
  `blendAlpha` is already precomputed per item). At draw time, when a window
  is active, run the existing opaque-item loop and the existing blend-item
  loop twice each (once per scissor mode), skipping items that don't belong
  to that pass; when no window is active (the common case), skip straight
  to today's single-pass code unchanged -- zero risk to the far more common
  non-windowed path.

**The one real unknown, and why this wasn't implemented blind:**
`C3D_SetScissor`'s `left/top/right/bottom` are in the GPU's raw framebuffer
pixel space. The 3DS top screen framebuffer is physically portrait
(rotated 90°) in memory; citro2d's own draw calls present a rotation-
corrected "logical" 400x240 landscape coordinate space (confirmed by this
file's existing `screenBaseX`/`scale` math at `port_gpu_renderer.c:1059-1060`,
which treats the render target as plain 400x240 with no rotation handling
of its own -- citro2d must be doing that internally). `C3D_SetScissor` is a
**citro3d** call, one level below citro2d's abstraction, and nothing in the
installed headers documents whether it expects logical or physical
(rotated) coordinates. Getting this wrong doesn't fail to compile or crash
-- it silently clips the wrong rectangle, which is exactly the class of bug
this file's own commit history is full of (layer priority inverted, stereo
eye offset backwards, TEV byte order swapped, the still-open bottom-screen
duplication bug) -- every one of them only ever caught by looking at an
actual screen. This environment has no display output (Azahar can only be
screenshotted after the fact, headless, no interactive input), so a scissor
rect coordinate bug here could not have been caught before landing. Better
to hand this off with the exact risk identified than ship an unverified
guess into an already fragile file.

### Affine BG (GBA mode 1/2)

Architecturally a good fit for the PICA200 -- affine transforms are what
GPUs are built for, arguably *easier* than the current orthogonal tile-atlas
approach. The real work is elsewhere: GBA mode 1/2 affine backgrounds use a
different tilemap format (8bpp only, no per-tile flip, wraps or clamps at
map edges per BGCNT) than the mode-0 text backgrounds this renderer already
decodes, so it needs its own tilemap-to-texture decode path (or reuse the
CPU renderer's existing affine tilemap walk from `mode1.c` and just upload
the result as one texture, then draw it as a single quad with the GBA's
PA/PB/PC/PD/reference-point matrix as the model transform). Moderate,
self-contained effort; lower coordinate-transform risk than window clipping
since it's one full-screen quad with a matrix citro3d already knows how to
apply, not a raw hardware register.

### Affine OBJ (confirmed occurring, per the measurement above)

Same idea as affine BG but per-sprite: each OAM entry with the affine flag
set carries a rotation/scale matrix (via an OBJ affine parameter group, GBATek
6.4.4) instead of a flip flag. Natively expressible as a per-quad 2x2 matrix
in the existing sprite draw call. Moderate effort, same shape of work as
affine BG but scoped per-item instead of per-layer.

### Mosaic (BG/OBJ pixelation)

Different technique entirely from the atlas-quad approach: needs a
downsample-then-nearest-upsample post-process (render the affected layer(s)
at reduced resolution, then blit back up with point sampling), not a
per-item draw parameter. Higher effort, and mosaic is a much less common
effect in this specific game than window-gated transitions/effects per the
`transparency.c` comment already in this file. Lowest priority of the four.

### OBJWIN (object-shaped window mask)

Not investigated in depth -- no evidence from the measurement above that
Zero Mission uses it, and it's the hardest of the four (needs deriving a
window mask from a sprite's rendered shape, effectively a stencil pass).
Recommend leaving this as a CPU fallback indefinitely unless a concrete
in-game case turns up.

## Recommendation

1. Re-run the diagnostic build with real input (manual play, or add input
   automation to this environment) specifically covering: the intro scene
   in full, opening at least one in-game menu, and the title-screen
   attract-mode demo, to get real reason counts for all three reported
   symptoms instead of just the one confirmed here.
2. Implement window clipping first (highest apparent impact per the
   `transparency.c`/`gSuitFlashEffect` evidence already in this codebase,
   and the scissor test is a clean hardware match) -- but resolve the
   citro2d-vs-citro3d coordinate-space question first, either by finding
   authoritative documentation/source for citro2d's rotation handling, or
   by testing a single hardcoded scissor rect against a known screen
   location on a real device/interactive Azahar session before wiring it
   to actual GBA window registers.
3. Affine OBJ next (confirmed occurring, moderate effort, lower coordinate
   risk than window clipping).
4. Affine BG, then mosaic, in that order, as follow-ups.
