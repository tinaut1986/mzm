# GPU tile renderer: closing the CPU-fallback gap (window clipping, affine BG/OBJ, mosaic)

Branch: `feat/gpu-renderer-window-affine-mosaic`, based on `main`
(`c45ecef2`, after the audio-decouple work; later rebased onto `main` again
to pick up the GPU-submit-lock hang fix). Investigation only so far -- no
rendering code changed yet. See "Recommendation" at the bottom for what to
build first and why nothing was implemented blind.

**Update (same day):** added a `KEY_Y` debug-marker hotkey and had the user
play through a session with it, confirming two of the three originally
reported symptoms against real play instead of just inferring them from
code. See "What I actually measured" below.

## The ask

With `RENDERER=gpu` now the default (`platform/3ds/Makefile`), most gameplay
renders via `port_gpu_renderer.c`'s PICA200 tile/sprite compositor. But
`Port_GpuRenderer_CanRenderFrame()` rejects a frame outright -- falling back
to the CPU scanline renderer (`port/ppu/src/mode1.c`) for that frame -- for
several GBA PPU features it doesn't implement. Reported symptoms this
investigation was asked to explain:

1. An intro scene falls back to CPU. **Not yet confirmed.**
2. A menu selection that appears to "crop" something falls back to CPU.
   **Confirmed: file-select's transition animation triggers `GPU_REJECT: WIN0`.**
3. The idle/attract-mode gameplay demo (plays after leaving the title
   screen untouched) falls back to CPU, but only sometimes. **Confirmed
   (at least in part): explosions trigger `GPU_REJECT: affine OBJ`.**

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

**Round 1** (headless, no input): built with `EXTRA_CFLAGS=-DPORT_GPU_RENDERER_DIAG_LOG`
and ran two headless Azahar sessions (45s, then 90s) via
`tools/run_azahar_test.sh`, covering boot through the intro text crawl,
title/file-select, and into an in-game demo (`GM 0x0B`, confirmed via the
debug log's `ModeChange` lines). Only `GPU_REJECT: affine OBJ` appeared (2
log lines; logging is throttled to 1-in-30 rejected frames, so the real
reject count is higher but the frame class is confirmed). No input
automation was available in this environment (no `xdotool` or equivalent),
so the menu case was never exercised in this round.

**Round 2** (manual play, on the user's machine): to work around the lack
of input automation, added a `KEY_Y` debug hotkey
(`platform/3ds/source/platform_3ds_minimal.c`, `Platform3DS_PollKeysIntoGba`)
that drops a numbered, timestamped `USER MARK` line into `mzm-debug.log`.
The user played the same diagnostic build manually, pressing `Y` at two
moments they suspected were falling back to CPU, then handed back the log.
Cross-referencing the marks against the surrounding `GPU_REJECT`/`GPUDIAG`/
sprite-log lines **confirmed both remaining suspected symptoms concretely**:

- **Marks #2/#3** landed right on a burst of `SpriteSpawnSecondary` calls
  (multiple parts of sprite id 12 spawned in a tight cluster -- the shape of
  an explosion's particle sprites) immediately followed by
  `GPU_REJECT: affine OBJ` a few frames later. Confirms explosions are (at
  least sometimes) the "(3) idle gameplay sometimes falls back" case, and
  that they use OAM affine transforms (scale/rotate) rather than plain
  sprites.
- **Mark #4** landed right in the middle of a `FileSelect subMenuStage: 0 ->
  1 -> 2 -> 3 -> 4 -> 6 -> 7` sequence (the file-select screen's own
  transition animation), immediately followed by `GPU_REJECT: WIN0`.
  Confirms **(2) the menu "crop"** is exactly the window-clipping case this
  doc already flagged as the top candidate (see `WindowCoversFullScreen`'s
  existing comment about `gSuitFlashEffect`) -- WIN0 gates the file-select
  reveal/wipe transition.

Two things remain unconfirmed:
- **(1) the intro affine-BG scene** -- neither round captured a `mode != 0`
  reject. Worth re-checking whether Zero Mission's intro actually uses an
  affine BG at all, or whether it's a short-lived moment easy to miss even
  with marks.
- The idle-timeout **attract-mode demo** specifically (as opposed to the
  explosion case above, which happens during regular/demo gameplay) --
  still not deliberately exercised.

Full session reject tally (both rounds combined, log-file-lifetime,
1-in-30-throttled so real counts are higher): `affine OBJ` x3, `WIN0` x1.

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

## Update (same day, later): affine OBJ and window clipping implemented

Both of the top two recommended items are now implemented in
`platform/3ds/source/port_gpu_renderer.c`, per the priority order below
(mosaic and affine BG were intentionally left out of this pass -- lower
priority, per the original assessment, and not requested).

**Affine OBJ**: `CollectSprite` now reads the OAM affine parameter group
(PA/PB/PC/PD, GBATek 6.4.4), inverts it (the on-hardware matrix is a
backward screen->texture sampling map; this renderer draws forward quads, so
it needs the inverse), and decomposes the inverse into rotation + per-axis
scale for `C2D_DrawParams` (which has no general shear support). Each 8x8
subtile is placed by applying the FULL (non-decomposed) inverse matrix to
its pivot-relative center, so subtile *placement* stays exact even when the
source matrix carries shear -- only a sheared subtile's own *shape* is
approximated (rotation+scale only). Double-size sprites (attr0 bit9) widen
the bounding/culling box as GBATek specifies. `Port_GpuRenderer_CanRenderFrame`
no longer rejects affine OBJ frames at all.

**Window clipping**: a single active window (WIN0 xor WIN1, still falling
back to CPU if both are simultaneously clipping -- see the scope note in
`Port_GpuRenderer_CanRenderFrame`'s header comment) is now rendered via two
`C3D_SetScissor` passes (`GPU_SCISSOR_NORMAL` for the inside, `GPU_SCISSOR_INVERT`
for the outside) instead of falling back. Each BG/OBJ layer's visibility
(always / inside-only / outside-only / never) is precomputed once per frame
from WININ/WINOUT and carried per `DrawItem`; an always-visible item is drawn
in both scissor passes, which -- since the two rects are exact complements --
composites correctly without double-drawing any pixel.

**The scissor coordinate-space risk flagged above was NOT resolved.** This
environment still has no display output. The implementation assumes
`C3D_SetScissor`'s left/top/right/bottom are in the same logical 400x240
landscape space citro2d's own draws already use (matching this file's
existing `screenBaseX`/`scale` math), not the physical rotated framebuffer
space -- documented inline at the scissor-rect computation in
`Port_GpuRenderer_RenderFrame`. **This needs verification against a real
screen or an interactive Azahar session before being trusted**: if a
window-clipped scene (file-select's transition, `gSuitFlashEffect`) shows
the wrong region clipped, or a rotated/mirrored clip, that assumption is
wrong and the physical-framebuffer mapping needs to be substituted instead.
Both changes build cleanly (`arm-none-eabi-gcc`, with and without
`PORT_GPU_RENDERER_DIAG_LOG`), but neither has been exercised in a running
session yet.

## Update (same day, later still): OBJWIN confirmed occurring -- pause screen suit view

After the affine OBJ / window clipping changes above landed, a fresh
diagnostic-build play session (`PORT_GPU_RENDERER_DIAG_LOG`, played in Azahar
after an FTP-to-hardware attempt failed to connect) surfaced a THIRD
CPU-fallback reason actually occurring in real play, which this doc had
previously written off ("no evidence... recommend leaving this as a CPU
fallback indefinitely"):

```
GPU_REJECT: OBJWIN
```

Three throttled log lines, right after the user marked (`KEY_Y`) the moment
they were checking their suit on the pause screen, with the diagnostic
`GPUDIAG` line just before it showing `obj=` (visible sprite count) dropping
sharply (32 -> 8 -> 10) -- consistent with several sprites switching to
OBJ-window mode (which stops them being drawn as sprites at all).

**Root cause, confirmed in source**: `PauseScreenUpdateWireframeSamus()`
(`src/menus/pause_screen.c:1007-1011`) duplicates the pause screen's rotating
Samus wireframe sprite into a second OAM slot and sets that copy's
`objMode = 2` (`OAM_OBJ_MODE_WINDOW`, `include/oam.h`):

```c
PAUSE_SCREEN_DATA.miscOam[8].oamId = oamId;      // the drawn wireframe sprite
PAUSE_SCREEN_DATA.miscOam[8].exists = OAM_ID_CHANGED_FLAG;

PAUSE_SCREEN_DATA.miscOam[9] = PAUSE_SCREEN_DATA.miscOam[8];
PAUSE_SCREEN_DATA.miscOam[9].objMode = 2;         // same shape, used as an OBJWIN mask instead
```

I.e. the wireframe Samus silhouette ITSELF is reused as an OBJ-window mask --
almost certainly to gate some effect (a shine/highlight sweep across the
suit, or the equipment-flash overlay) so it only shows up inside the
silhouette's exact shape, not its bounding box. This is not a rare edge
case: every player who opens the pause menu and looks at their equipped suit
hits this path, so it's a common, everyday CPU fallback, not the rare one
originally assumed.

## Update (same day, later still again): OBJWIN attempted via stencil plane, REVERTED -- broken on screen

Implemented the stencil-buffer approach sketched above (the "real" GPU-native
shape mask, not an approximation), built clean, and had the user test it in
Azahar against the pause screen's suit view. **It rendered incorrectly** --
screenshot showed the wireframe Samus as a solid blue blob with a magenta
hex/diamond pattern bleeding across the ENTIRE top screen (HUD bars
included), not confined to the silhouette at all. Symptom shape (something
that should be mask-confined instead appearing everywhere) is consistent
with the stencil test never actually gating anything -- i.e. every fragment
"passing" regardless of the buffer's contents, which would happen if either
pass's `GPU_EQUAL` test silently no-ops. **Reverted in full**
(`git checkout` on both changed source files) rather than iterate blindly --
this environment has no display to debug against, and burning more
build/FTP-or-Azahar/screenshot round trips on an unverified GPU quirk
wasn't a good trade. `Port_GpuRenderer_CanRenderFrame` is back to
unconditionally rejecting OBJWIN (falls back to CPU, as before this
whole OBJWIN investigation started); window clipping and affine OBJ (the
two changes verified working in earlier rounds) are untouched by the
revert.

**Leading suspect, not yet confirmed**: the assumption in
`SetClipPassState`'s comment -- that the PICA200 requires `C3D_DepthTest`
enabled (even with `GPU_ALWAYS` and no depth write) for the stencil unit to
actually run at all -- may be wrong, or the mask-write pre-pass's own
`C3D_DepthTest(true, GPU_ALWAYS, 0)` (zero write-mask, meant to suppress
color output during the mask-only draw) may not behave as assumed on this
GPU/citro3d version. Both are exactly the kind of undocumented
fixed-function-pipeline interaction this environment can't verify without a
real screen, in the same spirit as the still-unresolved
`C3D_SetScissor` coordinate-space assumption for window clipping above (that
one at least degrades gracefully to "clipped in the wrong place"; this one
degraded to "not clipped at all," visually worse).

**Left for a future round, if revisited**: get a minimal, isolated stencil
test working first (e.g. a single hardcoded solid-color rect gated by a
single hardcoded stencil write, verified on an actual screen) before wiring
it back into this renderer's full draw-order/window/blend machinery --
mirrors the same "prove the primitive in isolation before integrating it"
lesson the window-clipping scissor-rect risk note above already called out
in advance; OBJWIN just didn't get that isolated proof step first.

**What was tried, for the record** (none of this code is present on `main`
or any branch after the revert -- kept here only so a future attempt doesn't
have to rediscover the same shape from scratch): top render targets
(`sTopTarget`/`sTopRightTarget` in `platform_gpu_3ds.c`) switched from
`GPU_RB_DEPTH16` to `GPU_RB_DEPTH24_STENCIL8`; `CollectSprite`'s `objMode==2`
sprites collected into a separate `sMaskItems` array (unordered, since a
stencil OR-write doesn't care about draw order) instead of the normal sorted
`sDrawItems`; a per-eye mask-write pre-pass clearing the stencil plane then
drawing every mask subtile with `GPU_STENCIL_REPLACE` (ref=1) and color/depth
writes off; the existing WIN0/WIN1 two-pass scissor scheme generalized into
`SetClipPassState`/`ClearClipPassState` so the same opaque/blend draw loops
could apply either a scissor test or a `GPU_EQUAL` stencil test; and
`WINOUT`'s high byte (the OBJWIN "inside mask" per-layer enable) feeding the
same `ComputeLayerWinVis` machinery WIN0/WIN1 already used.

## Recommendation

Both window clipping and affine OBJ are now confirmed (not just inferred)
against real play, via the marked session in "What I actually measured"
above. Priority order unchanged from the original assessment, since it was
already based on impact + risk rather than just occurrence:

1. Affine OBJ first now, not window clipping -- it's confirmed occurring,
   moderate effort, and has much lower coordinate-transform risk (a 2x2
   matrix on an existing per-sprite quad, not a raw hardware scissor
   register). Reordered ahead of window clipping specifically *because* it
   can be implemented and reasoned about without needing to resolve the
   citro2d-vs-citro3d coordinate-space unknown below first.
2. Window clipping next -- confirmed high-impact (file-select's own
   transition, likely several other menu/effect transitions given
   `gSuitFlashEffect` already suspected), clean hardware match
   (`GPU_SCISSOR_NORMAL`/`INVERT`), but still blocked on resolving whether
   `C3D_SetScissor` expects citro2d's logical or the raw physical (rotated)
   framebuffer coordinates -- see the still-unresolved risk write-up above.
   Test a single hardcoded scissor rect against a known screen location
   interactively (real device or the user's Azahar session) before wiring
   it to actual GBA window registers.
3. Affine BG, then mosaic, as follow-ups -- still unconfirmed against real
   play (see "Two things remain unconfirmed" above); re-check whether Zero
   Mission's intro actually uses affine BG at all before investing effort
   there.

## Update (same day, yet later): OBJWIN re-attempted via a CPU coverage grid instead of the GPU stencil unit

After the stencil-buffer attempt above rendered garbled on real
screen/Azahar and was reverted, re-implemented OBJWIN a second way that
avoids the GPU stencil unit (and the render-target format change it
needed) entirely, on the theory that the garbled render came from an
undocumented depth/stencil fixed-function interaction this environment has
no way to verify before shipping.

**New approach**: `ComputeObjWinMask()` (`port_gpu_renderer.c`) runs once
per frame, only when OBJWIN is active, BEFORE `CollectBgLayer`/
`CollectSprite`. It walks OAM directly (same shape/size/position/flip decode
`CollectSprite` already does) looking for `objMode==2` sprites, and
rasterizes them into `sObjWinCovered`, a plain `bool[20][30]` grid at 8x8-
cell granularity over the 240x160 screen -- for a non-affine mask sprite,
each covered cell is decided by reading the sprite's raw VRAM tile bytes
directly and checking for any non-zero (opaque) palette index
(`TileHasOpaquePixel`), the same granularity BG/OBJ tiles already draw at;
for an affine mask sprite (the confirmed pause-screen wireframe can rotate),
the WHOLE rotated bounding box is marked covered instead -- a deliberately
coarser approximation, accepted to avoid re-deriving the inverse-affine-
matrix machinery `CollectSprite`'s drawable path already has just to
rasterize a boolean mask.

`CollectBgLayer`/`CollectSprite` then resolve OBJWIN visibility per tile
directly at COLLECTION time via `ObjWinItemVisible` (a single grid lookup),
instead of tagging items for a second GPU draw pass -- items that fail the
check are simply never pushed. This runs alongside, not instead of, the
existing WIN0/WIN1 rect mechanism (which still uses its own `rectWinVis` tag
+ scissor draw passes, untouched) -- the two are mutually exclusive per
frame (`Port_GpuRenderer_CanRenderFrame` still rejects OBJWIN combined with
an actively-clipping WIN0/WIN1), so no item is ever resolved by both paths
at once.

**Why this should be lower-risk than the stencil attempt**: no render-target
format change, no new GPU pipeline state (no stencil test, no depth test
toggling, no extra draw pass), nothing beyond VRAM/OAM reads and a small
CPU-side boolean array -- every mechanism this needs was already proven
working elsewhere in this file (VRAM tile reads via `GetOrDecodeTileSlot`'s
neighbors, OAM decode via `CollectSprite`'s existing logic, per-item
collection-time skip via the same pattern `WIN_VIS_NEVER` already uses).
`platform_gpu_3ds.c` is untouched this time.

**Not yet exercised in a running session** -- builds clean
(`arm-none-eabi-gcc`, both with and without `PORT_GPU_RENDERER_DIAG_LOG`),
not yet played against the pause screen's suit view. If this ALSO renders
wrong, the bug is more likely in the coverage rasterization logic itself
(a plain CPU/data bug, much easier to reason about from a log/screenshot
than a GPU fixed-function quirk) than in an unverifiable GPU assumption.
