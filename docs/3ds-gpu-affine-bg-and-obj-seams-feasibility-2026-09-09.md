# GPU tile renderer: affine BG on GPU, and affine-OBJ subtile seams

Branch: `feat/3ds-gpu-affine-bg`, based on `release/v0.6.2` (`42e5008b`,
right after the per-cutscene stereo-depth overrides merged). **Both parts
are implemented** -- Part 2 (affine-OBJ seams, shipped as the interior-edge
bleed) and Part 1 (mode-1 affine BG2 on GPU, opt-in via the AFFINE BG debug
cell, default on). Neither is hardware-verified yet. See **Recommendation**.

## The ask

Two things, both about scenes that currently look wrong or flat on the 3DS:

1. **One affine-BG sub-scene falls back to the CPU renderer.** The user
   played through the whole game checking: the **only** frame class that
   ever hits the CPU path is the Tourian-escape "Samus surrounded"
   sub-scene (GBA mode 1, `DISPCNT` `0x1501`: BG0 prio 1, BG2 prio 0 affine,
   OBJ on), samples 257-283 of the `Test 2` recording. Chozodia escape and
   the GM_INTRO cinematic **never** fall back -- so this is a single,
   fully-characterised scene, not a class of them.
   `Port_GpuRenderer_CanRenderFrame()` rejects it at
   `port_gpu_renderer.c:2952` -- `(dispcnt & 7u) != 0u` -> `REJECT("mode != 0")`
   -- so every frame of that ~1 s stretch renders through
   `port/ppu/src/mode1.c`, which has **no stereo depth at all**: the scene
   goes flat, and its output quality is visibly worse than the GPU path.
   `docs/3ds-debug-tools.md` already flags these frames with a magenta
   border under DEPTH TINT precisely because there is no plane data there.

2. **Affine sprites show transparent seams between their 8x8 subtiles when
   scaled up** ("cuando los sprites se acercan a cámara ... se ven cuadrados
   sueltos separados formando la figura"). Confirmed in code -- see
   *Affine-OBJ seams* below. This is independent of (1) but shares the fix
   infrastructure.

## Why CPU fallback is not acceptable here

- No stereo: `port/ppu/src/mode1.c` renders one flat 240x160 GBA frame. The
  3D slider does nothing for these frames. A cutscene that is *about* depth
  (a ship pulling away, a rotating room) is exactly where flatness is most
  obvious.
- `port_cutscene_depth.inc` overrides are dead on these frames -- the
  per-cutscene depth work just merged cannot touch them.
- Quality: the user reports the CPU output itself looks worse on these
  scenes. Not yet root-caused (possibly the affine BG's own sampling in
  mode1.c), but moot if the GPU path takes over.

---

## Part 1 -- Affine BG on the GPU

### What MZM actually does with the affine BG

`src/tourian_escape.c`:

- `TourianEscapeCalculateBg2()` (`:211`) computes the BG2 matrix **once per
  frame**:
  ```c
  gWrittenToBg2Pa = FixedMul(COS(rot), FixedInverse(xScale));
  gWrittenToBg2Pb = FixedMul(SIN(rot), FixedInverse(xScale));
  gWrittenToBg2Pc = FixedMul(-SIN(rot), FixedInverse(yScale));
  gWrittenToBg2Pd = gWrittenToBg2Pa;
  gWrittenToBg2X  = (SCREEN_X_MIDDLE<<8) - Pa*SCREEN_X_MIDDLE - Pb*SCREEN_Y_MIDDLE;
  gWrittenToBg2Y  = (SCREEN_Y_MIDDLE<<8) - Pc*SCREEN_X_MIDDLE - Pd*SCREEN_Y_MIDDLE;
  ```
- `TourianEscapeVBlankSamusSurrounded()` (`:87`) writes `REG_BG2PA..PD`,
  `REG_BG2X/Y` in VBlank only. **No HBlank DMA on the matrix.**

So this is a single global rotate + independent X/Y scale about the screen
centre, constant for the whole frame, no shear (`Pd == Pa`, `Pb/Pc` are
`±sin`). That is the easy case -- one transformed quad reproduces it exactly.

### What the `Test 2` capture actually contains (measured)

Every frame of the fallback stretch (samples 257-283):

| Field | Value | Meaning |
|---|---|---|
| `DISPCNT` | `0x1501` | mode 1, BG0 + BG2 + OBJ |
| `BG2CNT` | `0x4F88` | prio 0, charBase 2, screenBase 15, **mosaic 0**, **overflow bit 0 = transparent outside**, **size 1 = 256x256 px** |
| `BG2PB` / `BG2PC` | `0` / `0` | **no rotation, no shear** in this capture |
| `BG2PA` == `BG2PD` | `42 -> 67 -> ... -> 256` | **pure uniform zoom**: starts ~6x magnified (42/256), settles to 1:1 by sample 275 |
| `BG2X` / `BG2Y` | `100.3, 66.9 -> 0, 0` | reference point, tracks the zoom back to origin |

So in practice the effect is a **scale + translate only** -- an
axis-aligned zoom of BG2, no rotation at all. `C2D_DrawParams` does this
directly (`scaleX`/`scaleY`, `angle = 0`), no matrix decompose needed. The
256x256 map with the overflow bit clear means: scratch RT is exactly
256x256, compose the whole map once, transparent outside.

Keep the general no-shear rotate+scale path anyway -- `TourianEscapeCalculateBg2()`
*can* emit `±sin` terms from `gBg2Rotation`, this capture just never has it
non-zero -- but rotation is not on the critical path for shipping this scene.

**Not in scope:** a per-scanline affine matrix (true "Mode 7" floors, HBlank
DMA into `REG_BG2PA..`). MZM's scene-art cutscenes don't use it. Mode 2
(BG2+BG3 both affine) and bitmap modes 3/4/5 are confirmed unreachable in
this game's cutscenes (whole playthrough checked) -- they stay on CPU.

### Approach: render the affine BG to a scratch target, draw it as one quad

The renderer already has the exact machinery, three times over: `sHazeTex`
(issue #29 BG3 ripple), `sLayerTex[4]` (step-B per-layer cache), all
`C3D_TexInitVRAM` + `C3D_RenderTargetCreateFromTex`, `GPU_NEAREST`,
`GPU_CLAMP_TO_EDGE`. Reuse that shape:

1. **Compose the affine BG's tilemap into a scratch RT** at GBA resolution
   (the map is 128/256/512/1024 px square per `BG2CNT` bits 14-15). For the
   Tourian case the visible content fits a 256x256 target; a 512 or 1024 map
   needs either a bigger target or composing only the visible window plus a
   margin. Start with: compose the whole map when it is <= 512, else clamp to
   512 and accept wrap artefacts at extreme zoom-out (none observed in this
   scene). Tile decode reuses `GetOrDecodeTileSlot` / the atlas exactly like
   `CollectBgLayer`, just written into the scratch RT instead of quads.

2. **Draw the RT as ONE quad** transformed by the affine matrix. GBA affine
   BG is a backward map (`screen -> texture`): `tex = M * (screen - ref) `,
   with `M = [[Pa,Pb],[Pc,Pd]]/256` and `ref = (BG2X,BG2Y)/256`. This is a
   forward quad renderer, so invert once (`det = Pa*Pd - Pb*Pc`; guard
   near-zero, same as `CollectSprite`'s affine path at `:2730`) and place the
   quad's four corners at `ref + M^-1 * texCorner`. Decompose `M^-1` into
   `angle + scaleX + scaleY` for `C2D_DrawParams` (exact here -- no shear),
   or drop C2D and emit the quad directly. Feed the quad the same per-eye
   integer offset every BG layer gets (`floorf(eyeSign*slider*TierPx(tier)+0.5)`)
   so it participates in stereo like any other layer.

3. **Wrap mode** from `BG2CNT` bit 13 (display-area overflow): set ->
   `GPU_REPEAT` on the scratch tex; clear -> transparent outside
   (`GPU_CLAMP_TO_BORDER` with a 0 border, or an alpha-tested apron). Tourian
   escape: check the captured `BG2CNT` in the recording and match it.

4. **Composite order.** Slot the affine-BG quad into the existing
   per-priority draw order at its `BG2CNT` priority, between the text BGs and
   OBJ, exactly where `CollectBgLayer` would have put a normal BG2. BG0/BG1
   keep the current text path.

### Gate change (`Port_GpuRenderer_CanRenderFrame`)

Replace the blanket `(dispcnt & 7) != 0` reject with:

- `mode == 1` **accepted** iff: BG2 is the only affine BG enabled, `MOSAIC`
  clear for BG2, no `BG2CNT` feature we don't handle, and the matrix is
  whole-frame (we cannot detect per-scanline rewrite directly -- rely on
  "MZM never does this in a cutscene" and keep an assert/log if `BG2PA`
  changed between the value we sampled and end-of-frame is ever wired up).
- `mode == 2` (BG2 **and** BG3 both affine): reject, unchanged -- confirmed
  unreachable in this game's cutscenes.
- `mode >= 3` (bitmap): reject, unchanged.
- All existing rejects (forced blank, WIN0+WIN1, mosaic, OBJ mosaic) still
  apply.

Since the one real scene is fully known, the first cut can gate even
tighter -- `mode == 1 && BG2CNT size == 1 (256px) && overflow bit clear` --
and widen only if another `mode == 1` frame ever shows up in a recording.

### Cost

One extra RT compose pass (tile decode into a 256x256 target) + one textured
quad per eye. Comparable to a single step-B cached BG layer, which is
already in budget. The scene is otherwise light (few sprites).

### Risks / open questions

- **Sub-pixel reference point.** `BG2X/Y` are 20.8 fixed (`s32`, bits 27-8
  used); the capture shows fractional values (100.312, 66.875). Carry full
  precision into the corner math; only snap the final quad origin to a
  device pixel per eye (same reasoning as `BuildDrawParams`'s affine branch).
- **Rotation path is untested.** The capture has `PB == PC == 0` throughout,
  so the rotate branch of the corner math will never be exercised by the
  only scene that reaches it. Either implement scale+translate only and
  assert on non-zero `PB/PC`, or implement the general no-shear rotate+scale
  and accept it is unverified against a real frame.
- **Filtering.** Keep `GPU_NEAREST` for parity with the CPU oracle at slider
  0 (the `rec_render` harness diffs against `mode1.c`). A rotated nearest
  sample will stair-step; that matches the GBA.

---

## Part 2 -- Affine-OBJ subtile seams

### Root cause (confirmed)

`CollectSprite`'s affine path (`port_gpu_renderer.c:2765-2892`) pushes **one
quad per 8x8 subtile** (`PushAffineItem`), each centred at its
matrix-transformed centre. `BuildDrawParams`'s affine branch (`:3106-3121`):

```c
float w = item->w * scaleX, h = item->h * scaleY;   // 8 * affScale * displayScale
params.pos.x = floorf(screenBaseX + eyeOffset + item->x*scaleX - w*0.5f + 0.5f);
params.pos.w = w;                                     // <-- NOT snapped
```

The non-affine path deliberately snaps shared edges so tile N's right edge
*is* tile N+1's left edge (`:3122-3159`, "Computing right = round(x+w) and
then w = right - x"). The affine path skips that ("deliberately left
unsnapped ... snapping the bounding quad would quantize the rotation").
Result: at `affScale * displayScale > 1` (a scaled-up / "closer" affine
sprite -- double-size affine OBJs, zoom effects, the map/pause rotate, boss
intros), consecutive subtile quads are placed `8*scale` apart but each is
`8*scale` wide with an **independently floored** origin, so they under- or
over-lap by up to a pixel. With `GPU_NEAREST` and the atlas slot ringed by
other tiles / zeros, an under-lap shows as a transparent seam -- the
"cuadrados sueltos" grid.

### Fix options

1. **Best: stop splitting affine sprites into subtile quads.** Decode the
   whole OBJ (<= 64x64) into a contiguous scratch region and draw it as
   **one** affine quad about the sprite's true pivot. No internal edges, so
   no internal seams, at any scale or angle. Needs a per-sprite scratch
   compose (target switches), so it is the eventual "correct" version, not
   the first move.

2. **Grow each subtile quad on its INTERIOR edges only, keep centre and UV.**
   Add ~1 px to a subtile quad on each edge that faces another subtile of
   the same sprite (`item->affBleedEdges`, set in `CollectSprite` from
   `tx/ty` vs `tilesW/tilesH`, in texture-grid orientation so flip/rotation
   don't matter). The pivot moves by the left/top growth so the centre
   stays put. Interior edges then overlap their neighbour by ~2 px --
   more than the <=1 px rounding error -- so the gap is always covered,
   while the sprite's OUTER silhouette edges are untouched, so it keeps its
   exact size and outline. UV unchanged, so a grown edge only re-stretches
   this subtile's own outermost texels (<8 %, invisible with nearest);
   it never samples an adjacent atlas tile. A 1x1 affine sprite has no
   interior edges and is byte-identical to before.

   *First cut grew all four edges symmetrically and the outer silhouette
   picked up a 1 px smear of its own edge colour on hardware -- hence the
   interior-only version.*

   2b. *(rejected)* Expanding the UV sub-rect instead of the quad: samples
   the neighbour texel, which in this atlas is another tile or zero, so it
   bleeds.

3. **Snap along the rotated subtile grid.** Only well-defined at angle ~= 0.
   Not worth a special path.

**Shipped: option 2, interior edges only.** The geometry is factored into
`platform/3ds/source/port_affine_subtile.h` (`PortAffine_SubtileQuad` +
`PortAffine_SubtileBleedEdges` + `PortAffine_Decompose`); `BuildDrawParams`'s
affine branch calls it, `DrawItem::affBleedEdges` carries the per-subtile
edge mask set in `CollectSprite`. Option 1 stays on the table if the
mid-sprite <8 % edge stretch ever matters.

**Also fixed in passing:** `C2D_DrawParams.pos` is the on-screen location of
the *pivot*, not the top-left -- C2D derives the top-left as `pos - center`
(see `C2D_DrawImageAtRotated`). The old affine branch set
`pos = centre - w/2` *and* `center = w/2`, so every affine sprite was drawn
half a subtile up-left of where it should be. `PortAffine_SubtileQuad` now
returns `x` as the snapped pivot position; the renderer picks that up for
free. Small per-sprite shift, but real -- worth an on-device recheck of the
affine sprites (explosions, boss zooms) alongside the seam fix.

### Iterating on it: `platform/3ds/tools/affine_probe`

A standalone `.3dsx` that renders a 64x64 test sprite through that same
header at 60 fps, with live rotate/scale and the bleed px exposed, plus a
`NO BLEED` mode (raw seams) and a `ONE QUAD` reference. Build with
`make -C platform/3ds/tools/affine_probe`. This exists because reproducing
the artefact in-game is slow and the in-game scene recorder currently
stalls to 0-1 FPS on the (CPU-rendered) Tourian escape. The seam-shaped
sprites in play are the 64x64 double-size affine OBJs in `Test 2` samples
285-449 (the affine-BG "Samus surrounded" sub-scene at 257-283 has no
affine OBJ at all -- its affine element is BG2).

---

## Recommendation -- build order

1. **Part 2 -- affine-OBJ seams. DONE** (option 2, `kAffineBleed` in
   `BuildDrawParams`). No `CanRenderFrame` change, no runtime cost; every
   scaled affine sprite stops shattering into a grid. Needs an on-device
   eye check to confirm.

2. **Part 1 -- mode-1 affine BG2 on the GPU. IMPLEMENTED (opt-in).**
   `port_gpu_renderer.c`:
   - `DetectAffineBg2()` -- gate: mode 1, BG2 on, `BG2CNT` size == 256,
     mosaic clear, `PB == PC == 0` (pure scale), `PA > 0`. Anything else in
     mode 1+ still `REJECT`s to the CPU renderer.
   - `ComposeAffineBg2()` -- CPU-decodes the 256x256 8bpp affine tilemap
     (32x32 one-byte indices) into `sAffineBg2Tex` (a `C3D_TexInit` linear
     texture, swizzled by `kSwizzleLUT`, coloured via `Bgr555ToRgba8` --
     the same path atlas tiles take). No render target, no GX transfer.
   - `CollectAffineBg2()` -- pushes ONE quad at BG2's priority: screen rect
     `(-refX,-refY)*invScale` .. `+256*invScale`, `invScale = 256/PA`,
     `ref = BG2X/BG2Y` (28-bit signed, 20.8). Overflow is transparent, so
     the single quad covering tex [0,256]^2 is the whole layer. Depth tier
     and per-eye offset come from `PortStereoDepth_BgTier(&sDepthState, 2)`
     like any BG.
   - Wired into the `for (bg = 3..0)` collect loop (`bg == 2` ->
     `CollectAffineBg2`) and the `CanRenderFrame` gate.
   - `Port_GpuRenderer_SetAffineBg` / `...Enabled` + an **AFFINE BG** debug
     cell (bottom UI, cell 14). Default **on**; flip off to A/B the CPU
     version.
   Not yet hardware-verified. Rotation path is deliberately excluded
   (asserts via the `PB==PC==0` gate) -- the one real scene never rotates.

3. **Measure on hardware.** Check the "Samus surrounded" sub-scene renders
   right and gets stereo depth; check nothing else regressed (the gate is
   narrow enough that no other frame should reach the new path). Then stop
   -- no mode 2, no map > 256, no per-scanline affine, no other affine-BG
   cutscene in this game.

No further captures needed -- `Test 2` (samples 257-283) is the whole scene.
