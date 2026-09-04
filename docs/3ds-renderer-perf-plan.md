# 3DS port: renderer performance work order

Living tracker for issue #20 (frame rate) and the renderer changes it leads
to. Tick items as they land; add new ones as they come up. Everything under
**Measured** was taken off a real New3DS and should not be re-derived by
argument -- and everything under **Dead ends** was believed and then
disproved, so please read that section before proposing a fix.

> **Status 2026-09-04 (night).** With the 3D slider down a New3DS already
> holds 60 FPS. The 45 FPS case is the second eye, and the cost is per-quad,
> not per-pixel. Step A (16x16 atlas blocks) is **implemented and building,
> not yet verified on hardware or through the harness** -- that is the next
> thing to do. Branch: `work/perf-instrumentation-and-harness`.

## Measured, and not to be re-argued

New3DS, same room, standing still, so quad counts are identical between
groups (`mzm-perf-08/09.bin`, 2026-09-04):

| | quads | GPU draw+proc | CPU tile+submit | FPS |
|---|---|---|---|---|
| 1 eye, pixel perfect | 2267 | 8,98 ms | 2,30 + 3,23 | 59,8 |
| 1 eye, scaled 1.5x | 2267 | 9,10 ms | 2,30 + 3,18 | 59,6 |
| 2 eyes, pixel perfect | 4534 | 17,75 ms | 2,31 + 6,50 | 56,3 |
| 2 eyes, scaled 1.5x | 4532 | 15,74 ms | 2,30 + 6,38 | 49,0 |

- **Cost is per-quad, not per-pixel.** 2.25x the pixels at an identical quad
  count costs +1.3%. Marginal fill is **0,66 ns/pixel**; a marginal quad is
  **3,87 us**. One quad's setup is worth ~5900 pixels of fill, and an 8x8
  tile is 64 pixels.
- **Per-quad cost is linear.** 2267 -> 4534 quads gives 8,98 -> 17,75 ms
  (1,98x), so a reduction in quad count buys back time proportionally.
- **The second eye is a pure duplicate of submission and rasterisation.**
  Tile decode stays at 2,30 ms in every row above (collection runs once per
  frame, not once per eye) while quads, submission and GPU time all double.
- **It is not a hitch, it is a flat cost sitting just over the budget.** The
  45 FPS case is uniformly ~17,7 ms against a 16,675 ms vblank, and
  `C3D_FrameSync` quantises the miss to a whole extra vblank.
- Beware the median-of-sums: the scaled 2-eye row has a *lower* median
  draw+proc than the pixel-perfect one yet fewer FPS, because its p90 is
  22,51 against 18,31. More pixels means a longer tail, not a worse median.
- **Old3DS will not reach 60 with this renderer.** The GPU is the same
  PICA200 at the same clock so its ~9-10 ms would not move, but the CPU half
  (2,30 tile + 3,23 submit at 804 MHz) becomes ~17 ms at 268 MHz -- past the
  whole frame budget before the GPU does anything. The port's Core1 grant is
  New3DS-only, so the real figure is worse than a naive 3x.

## Dead ends

- **Alpha blending / batch breaking.** `BLDCNT` looked like it tracked the
  slow rooms across a sample of three. It was coincidence:
  `blendTransitions` is **4 per frame** in all 429 frames of the first
  instrumented capture. Do not re-derive this from `BLDCNT` alone; measure
  the flush count.
- **Affine sprites (the Skree hypothesis).** The 45 FPS rooms carried FEWER
  visible sprites than the 60 FPS one (9 against 37) and no affine ones at
  all.
- **Merging runs of horizontally adjacent tiles into one quad.**
  Impossible: the atlas is a content-keyed cache, so two tiles adjacent on
  screen are not adjacent in it, and one quad samples one contiguous UV
  rect. This is what step A works around by composing 16x16 blocks in the
  atlas instead.
- **Fill rate / the window scissor pass / the BG3 haze pass.** All ruled out
  by the table above (`scissorPasses` is 1 and haze is inactive throughout).

## Plan

### A. 16x16 atlas blocks -- implemented, unverified

One quad per tilemap-aligned 2x2 group instead of four.

- [x] Atlas grows to 512x1024, split into a tile region (rows 0..63,
      addressing untouched) and a 16x16 block region (rows 64..127, 1024
      blocks). One texture on purpose: citro2d rebinds and breaks the batch
      when the texture pointer changes, and the draw order interleaves
      layers.
- [x] Separate block cache with its own key (four tilemap entries +
      charBase + brightness) and the same staleness checks as tiles
      (source-byte memcmp, palette hash, evy).
- [x] Block-region overflow degrades to per-tile drawing and clears the
      cache at the *start* of the next frame, never mid-frame.
- [x] Emission is purely subtractive: anything the block pass declines falls
      through to the untouched per-tile loop.
- [x] Alignment arithmetic verified exhaustively on the host (every map size
      x every scroll position: no group crosses a 32x32 screen block or a
      32-entry row).
- [ ] **Verify on hardware through the harness.** Dumps in 5-6 varied rooms,
      and specifically a lava room and a red-heat-tint room -- animated
      palettes are where a hole in the per-block staleness check would show
      as frozen or mis-coloured tiles.
- [ ] **Measure the win.** Expect `bgItems` ~2244 -> ~700 per eye and 2-eye
      GPU 17,75 -> ~5-6 ms. If it lands, 60 FPS with the slider up.
- [ ] Consider 32x32 blocks once 16x16 is proven (a further /4 on the
      interior, at 16x the cache-key space and coarser invalidation).

### B. Cache whole scrolling layers in render targets -- not started

One quad per layer: ~2244 -> 4. The big prize, and the only route that also
puts Old3DS in range, since it collapses the CPU submission too.

- [ ] Decide what to do about animated palettes. MZM re-animates palettes
      constantly (lava, heat tint) and a palette change invalidates a
      composed target wholesale, so this may buy nothing in exactly the
      rooms that need it most. Measure how many rooms actually animate the
      palette of a *large* layer before committing.
- [ ] Handle `PortLayerFix` corrections, which move individual tiles to
      another plane and draw order.
- [ ] Handle scroll seams and the wrap when the camera leaves the cached
      margin.
- [ ] The BG3 haze path (`sHazeRT`) already does exactly this for one layer
      -- read it first, it is the working precedent.

### C. Occlusion culling -- not started, and now lower priority

Skip tiles fully covered by an opaque higher-priority tile.

- [ ] Needs a coverage mask over the 32x21 screen cells walked
      front-to-back, plus a "tile is FULLY opaque" test (today's is "has any
      opaque pixel").
- [ ] Must never cull what sits under an alpha-blended first-target layer.
- [ ] Demoted below A and B: it removes quads (which is what matters) but
      only where layers actually overlap, whereas A removes them everywhere.

### D. The second eye -- superseded, keep for reference

- [ ] Re-submitting the whole scene per eye is what costs the 45 FPS, but
      there is no cheap fix in isolation: per-tier parallax means the eyes
      cannot share a rasterisation, and the offsets differ per depth plane.
      A and B both fix it as a side effect by making the scene cheap enough
      to draw twice. Only revisit if both fail.

## Open, unrelated to speed

- [ ] **A one-pixel sprite placement divergence.** Dump set `04` of the
      2026-09-04 baseline: 17 pixels in a 3x11 box where
      `console(112,51)` holds what the oracle has at `(111,51)` -- a small
      sprite drawn one pixel right. Real off-by-one, invisible by eye, found
      by the harness.
- [ ] **Blend rounding in tinted and lava regions.** Dump sets `02`, `03`,
      `08`: small deltas (mean 8-10 per channel) over the affected area.
      Consistent with the two renderers rounding a blend differently rather
      than a compositing bug, but unconfirmed.
- [ ] Room 1 of area 2 (`mzm-rec-3.bin`) showed `drawingTime` of 13-19 ms
      with only 988 BG quads -- the lightest scene of three. Quad count
      cannot be its limiter; something else is going on in that room and it
      was never chased.

## How to verify

Both recipes and the formats they produce are documented in
[3ds-debug-tools.md](3ds-debug-tools.md).

**Correctness** -- the GPU renderer against the CPU PPU oracle:

```sh
make -C platform/3ds rec-render
python3 tools/compare_render.py <dir-with-a-fetched-dump-set>
```

Capture with the 3D slider at 0, PIXEL PERFECT, and the FPS overlay, bezel
and colour correction off; the script refuses dumps that break those
conditions rather than reporting nonsense. Baseline to compare against
(2026-09-04, before step A): **six of ten sets pixel-identical**, three
differing slightly over tinted/lava regions, one with the 17-pixel sprite
shift above.

**Speed** -- one perf capture, standing still, cycling the settings:
`captureFlags` records the display style, slider, overlay, bezel, FX grade
and whether the frame used the GPU renderer at all, so a single file can
hold several conditions and they can be told apart afterwards. Group the
samples by `captureFlags` and drop ~45 samples either side of each
transition (opening the options menu redraws the bottom screen).

## Where the code is

| | |
|---|---|
| GPU tile renderer | `platform/3ds/source/port_gpu_renderer.c` |
| Atlas geometry and the block region | the `ATLAS_*` enum in that file |
| CPU PPU (the oracle) | `port/ppu/src/mode1.c` |
| Perf / scene recorders, file rotation | `platform/3ds/source/platform_gpu_3ds.c`, `port/port_debug_files.c` |
| Reference renderer (host) | `platform/3ds/tests/rec_render.c` |
| Comparison harness | `tools/compare_render.py` |
| Depth assignment unit tests | `platform/3ds/tests/stereo_depth_test.c` |
| OAM census unit tests | `platform/3ds/tests/oam_census_test.c` |
