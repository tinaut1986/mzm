# 3DS port: renderer performance work order

Living tracker for issue #20 (frame rate) and the renderer changes it leads
to. Tick items as they land; add new ones as they come up. Everything under
**Measured** was taken off a real New3DS and should not be re-derived by
argument -- and everything under **Dead ends** was believed and then
disproved, so please read that section before proposing a fix.

> **Status 2026-09-05. The automated harness is PARKED. Test on hardware.**
>
> Two sessions went into making the correctness harness run unattended under
> Azahar on Linux. The plumbing all works -- build, install, launch, trigger,
> fetch, compare, with no console and no hands -- and it still could not
> answer a single question about the renderer, because everything it
> compared was wrong for reasons of its own. The decision is to stop and
> test on hardware by hand. `REPLAY RECORDING` and the file triggers are
> gone from the console build (see **What was removed**); the host-side
> scripts are kept because they cost nothing.
>
> What the effort actually established is below. Most of it is negative, and
> all of it is expensive to rediscover.

## What Linux/Azahar established

**The striping is in the DUMP path, and it is neither renderer.** Every
replayed frame came back sliced into vertical bands of recognisable content,
and that was read for a long time as a renderer or replay bug. It is
neither: forcing the renderer from the dump trigger showed a dump taken with
the **CPU scanline renderer** tears exactly the same way. Bands are vertical
because the 3DS framebuffer is column-major -- a run of bytes from the wrong
frame is a run of columns.

`PlatformGpu3DS_DumpScreens` / `DumpTopLeftTo` now hold the GPU submit lock
and let the queue drain first, which closes a real race against the present
thread (`Port_PPU_GpuPresentPump` draws into the very same targets). **It
does not fix the tearing** -- unchanged with the lock held. Something else
still writes those targets, or the read is not coherent. That is the open
thread, and it matters beyond the harness: it is the same code path the
`SCREEN DUMP` menu row uses, so on-device dumps cannot be fully trusted
either until it is understood.

**The black-frame trap.** A long run of "pixel-identical" results turned out
to be black compared against black. The title screen holds DISPCNT with
layers enabled while every BG colour is still 0, and both renderers draw
that as black. Any comparison result predating 2026-09-05 should be
re-checked against the palette before it is believed. When taking a dump by
hand, confirm the frame has something on it.

**Do NOT use `gspWaitForP3D` / `gspWaitForPPF` to sync a dump.** They look
like the precise primitives for "the render finished" and "the transfer
finished". Each waits on an event that has usually already been consumed, so
the SECOND target of a three-target dump blocks forever -- observed as a
dump set containing only `-left.rgb` and nothing else. VBlank always comes.

**The replay itself was faithful.** Pack a live screen dump into a
one-sample corpus (`tools/rec_from_dump.py`) and replay it, and the output
is byte-identical (`cmp`) to that dump's own `-left.rgb`. Injected state is
byte-exact too: dumping VRAM/OAM/palettes/IO back out right before the
renderer runs and diffing against the recording gives zero differing bytes
except DISPSTAT/VCOUNT, which the port advances itself. And the oracle is
self-consistent: `rec_render dump` and `rec_render rec` produce identical
PPMs from the same state. None of these was the problem.

**Tried against the striping and disproved.** Each left the output
unchanged, which is itself the tell that none was ever in the path:

- `gspWaitForP3D()` + `gspWaitForPPF()` before the dump transfer (and see
  the deadlock warning above).
- Deferring each sample's dump by one replay iteration.
- Emptying the tile AND 16x16-block caches between samples.
- Rendering each injected sample twice and dumping the second.
- `GSPGPU_FlushDataCache` -- the gsp SERVICE call, which emulators watch to
  invalidate cached textures -- on every dirty atlas range.
- Disabling the 16x16 block pass entirely.

**Azahar cannot reach gameplay unattended.** Nothing drives the game past
the title screen, so every measurement above is of the title screen. That
alone caps what the harness could ever have proved: a renderer is not
interesting on a static menu.

**Azahar remains worthless for speed** -- frame times there measure the host
PC -- so this was never going to answer the FPS question anyway.

## What was removed, and what replaced it

- `REPLAY RECORDING` is gone from the debug tools menu. The implementation
  (`PortPpuMzm_DebugApplyPendingReplay`) is still in the tree but nothing
  calls it, including the main loop.
- The `mzm-dump-request.txt` / `mzm-record-request.txt` /
  `mzm-log-request.txt` file triggers are gone. They polled the SD every
  frame-group for a feature nobody was using, and an `fopen` of an absent
  file is an FS service round trip.
- `mzm-replay-request.txt` is unreferenced now that the replay has no call
  site.
- **Added in their place: `BLOQUES 16x16` in the debug tools menu.** Step A
  has never been measured, so it needed a switch on the only machine that
  can measure it. Same scene, one press, compare the FPS overlay. Off falls
  through to the untouched per-tile loop -- the pass is purely subtractive,
  so it is a clean A/B and not a second code path.
- Kept, because they run on the host and cost the port nothing:
  `tools/run_azahar_test.sh`, `tools/compare_render.py`,
  `tools/rec_from_dump.py`.

## Open on hardware

- **This branch is slower than an older build on the same scene** (60 FPS
  became 39-49, reported 2026-09-05). It carries step A and the whole v0.4.x
  line, which the older build did not. Step A is the first thing to switch
  off, and that is what `BLOQUES 16x16` is for.
- **The CPU scanline renderer shows artefacts on hardware** -- noise along
  straight horizontal lines. Unknown whether this is a regression on this
  branch or predates it. Worth checking against an older CIA before hunting
  it.
- **FIXED 2026-09-05: the GPU and CPU renderers disagreed whenever the
  display was scaled**, and step A drew a different image again on top of
  that. One root cause, confirmed on hardware. The CPU path draws the whole
  240-wide frame as ONE quad, so its texel sequence is the ideal one; the
  GPU path draws a quad per tile and its subtexture UVs inset half a texel,
  sampling n-1 texels for n pixels. Exact at 1:1 -- which is why PIXEL
  PERFECT always agreed, GPU against CPU and blocks against tiles alike --
  and wrong at every other scale, by an amount that depends on the span.
  Pinning the UVs at the full texel edges makes the spans right but puts
  every third sample exactly ON a texel boundary at 3:2, where the hardware
  rounds low often enough to pull in the neighbouring atlas slot: seen on
  hardware as dirt crawling along tile edges. The span is now the full n
  texels SHIFTED by an eighth of a texel, which breaks those ties
  consistently and keeps the outermost samples ~0.46 texels inside the slot.
  At 3:2 the GPU per-tile output now matches the CPU whole-frame output
  pixel for pixel (360/360), where the old convention differed on 60 of 360.
  STRETCH cannot be made exact -- 400/240 is 13.33 px per tile and quads snap
  to whole pixels -- but improves from 70 to 40 differing pixels of 400.
  Switchable at runtime (`UV ATLAS`) to compare the two on hardware.
- **FIXED 2026-09-05: step A drew a different image from the per-tile path**
  at any scaled display style. Reported from hardware by toggling
  `BLOQUES 16x16`. Not an addressing bug -- the four entries a block reads
  were verified byte-identical to what the per-tile loop reads across 12285
  groups of a real gameplay recording. It was the subtexture UV convention:
  a half-texel inset samples n-1 texels for n screen pixels, which is exact
  at 1:1 but scale-dependent otherwise, and the error depends on the SPAN,
  so a 16-texel quad and two 8-texel quads covering the same 24 pixels chose
  different texels. Both tables now use full-texel edges, which sample n
  texels for n pixels and agree at every scale. See InitSlotSubtexTable.
  This also changes scaled output for ordinary 8x8 tiles -- the doubled
  texels now fall on a fixed period instead of clustering -- so it is worth
  a look on hardware in its own right.

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

## Step A measured, at last (2026-09-05, New3DS, hardware)

Same room, same spot, toggling `BLOQUES 16x16` from the debug tools menu:

| | FPS |
|---|---|
| 16x16 block pass ON | **60** |
| 16x16 block pass OFF (per-tile) | **45** |

That is the win the whole of step A was written for, and it lands: a room
that could not hold the frame budget per-tile holds it comfortably with one
quad per 16x16 group. It also confirms the model in **Measured** above --
cost tracks quad count, so cutting BG quads by ~4x buys back the frame.

The 39-49 FPS reported earlier the same day, on the same scene, was NOT the
branch: that build carried the debug file triggers, which fopen()ed absent
files on the SD card twice a second from the frame loop. Removing them
restored 60. An absent-file open is an FS service round trip, and a debug
feature nobody is using must not cost one per frame-group -- worth
remembering before adding another poll to the main loop.

Step A is therefore worth keeping and worth finishing. What it still needs
is to look right (see the UV convention entry under **Open on hardware**)
and to be checked in rooms with animated palettes, where per-block staleness
has the most room to be wrong.

## The fire room, with step A on (2026-09-05, New3DS, hardware)

`mzm-perf-01.bin` / `mzm-perf-10.bin`, a room with fire, grouped by
`captureFlags`, medians over the steady part of each group:

| eyes | style | BG quads/eye | haze tiles | GPU draw+proc | FPS p10 |
|---|---|---|---|---|---|
| 1 | pixel perfect | 370 | 660 | 8,56 ms | 59,7 |
| 1 | scaled 3:2 | 369 | 660 | 9,12 ms | 58,9 |
| 2 | pixel perfect | 369 | 660 | 16,04 ms | 58,5 |
| 2 | scaled 3:2 | 369 | 660 | 18,37 ms | 30,0 |

Three things fall out of this, and they redirect the rest of the plan:

- **Step A works, and then some.** BG quads are ~370 per eye where the
  per-tile path collected ~2244 in the earlier captures: a 6x cut, better
  than the /4 the change was designed for, because this room's backgrounds
  are mostly full 2x2 groups. On hardware it is the difference between 45
  and 60 FPS with the slider down.
- **Cost has stopped tracking quad count.** 2267 quads cost 8,98 ms in the
  older capture; 370 quads cost 8,56 ms here. Six times fewer quads bought
  5%. The per-quad model in **Measured** held while quads were the
  bottleneck and does not hold any more -- so any further plan justified by
  "this removes N quads" has to be re-argued from a fresh measurement,
  step B included.
- **The BG3 haze pass is now the biggest single consumer.** It is active in
  every sample of both captures at 640-660 tiles per frame, which is nearly
  TWICE the BG quad count, and it is not included in `drawCount` -- it is an
  extra offscreen pass on top. The earlier captures had haze inactive
  throughout, which is why it was ruled out; in this room it is the main
  event.

So the room that still misses 60 (two eyes, scaled) spends its frame on a
pass nobody has looked at. Read `sHazeRT` / `HazeBlitStrips` before
collapsing BG layers.

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

### 0. Harness -- PARKED, see the status block

Verification is by hand on hardware for now: install, reproduce the scene,
read the FPS overlay, take a `SCREEN DUMP` when a still frame is needed.

- [x] Built and abandoned: an unattended Azahar loop, a replay of recorded
      PPU states, file triggers, a dump-to-corpus packer. Everything it
      compared was confounded first by black frames and then by torn dumps.
- [ ] **The dump path tears.** Until that is understood, a `SCREEN DUMP` is
      not a trustworthy record of what was on screen, on hardware either.
      This blocks any future attempt at automated comparison, so it is the
      one thing worth fixing before restarting this.
- [ ] Reaching gameplay without hands (start a file, then warp) is the other
      prerequisite. Without it Azahar only ever sees the title screen.

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
- [x] **A real correctness bug found and fixed** (the UV convention -- see
      "Open on hardware"). Until 2026-09-05 the pass was not merely
      unverified, it was drawing a different image whenever the display was
      scaled.
- [ ] **Verify on hardware.** Use the replay
      (`--replay`), not fresh dumps: `mzm-rec-01.bin` is on the card as the
      corpus. Animated-palette rooms are where a hole in the per-block
      staleness check would show, as frozen or mis-coloured tiles.
- [ ] **Measure the win.** Expect `bgItems` ~2244 -> ~700 per eye and 2-eye
      GPU 17,75 -> ~5-6 ms. If it lands, 60 FPS with the slider up.
- [ ] Consider 32x32 blocks once 16x16 is proven (a further /4 on the
      interior, at 16x the cache-key space and coarser invalidation).

### B. Cache whole scrolling layers in render targets -- premise weakened

One quad per layer. This was written as "~2244 -> 4, the big prize" when
cost tracked quad count. After step A the BG layers are already down to ~370
quads per eye and six times fewer quads bought 5% of the frame, so the
arithmetic that justified this no longer holds -- see **The fire room** above.
It may still be worth doing for the CPU submission it also collapses, and
the same technique applied to the BG3 haze pass (640-660 tiles per frame,
more than every BG layer put together) is now the better first target.

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

Everything here is on hardware and by hand. The formats each recipe produces
are documented in [3ds-debug-tools.md](3ds-debug-tools.md).

**Speed** -- the only measurement that means anything, and the reason the
rest of this document exists.

One perf capture, standing still in the same room, cycling the settings:
`captureFlags` records the display style, slider, overlay, bezel, FX grade
and whether the frame used the GPU renderer at all, so a single file can
hold several conditions and they can be told apart afterwards. Group the
samples by `captureFlags` and drop ~45 samples either side of each
transition (opening the options menu redraws the bottom screen).

For a quick A/B, the FPS overlay is enough: same room, same spot, toggle
`BLOQUES 16x16` in the debug tools menu and read it again.

**Correctness** -- one screen dump against the CPU PPU oracle:

```sh
make -C platform/3ds rec-render
python3 tools/compare_render.py <dir-with-a-fetched-dump-set>
```

Capture with the 3D slider at 0, PIXEL PERFECT, and the FPS overlay, bezel
and colour correction off; the script refuses dumps that break those
conditions rather than reporting nonsense.

Two things to check before believing a result:

- **Is the frame blank?** DISPCNT with no layer enabled, or an all-zero BG
  palette, renders black in both renderers and "pixel-identical" then means
  nothing. Common around fades and transitions.
- **Is the dump torn?** See the status block: the dump path can capture a
  mix of two frames, sliced into vertical bands. Look at the image before
  trusting the number.

Baseline to compare against (2026-09-04, before step A): **six of ten sets
pixel-identical**, three differing slightly over tinted/lava regions, one
with the 17-pixel sprite shift noted above. That baseline predates the
black-frame and tearing findings, so treat it as indicative rather than
exact.

**Azahar** is worthless for speed -- frame times there measure the host PC,
not the PICA200 -- and its correctness loop is parked. `tools/run_azahar_test.sh`
still builds, installs and launches a CIA on Linux for a smoke test, which
is all it should be relied on for today.

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
