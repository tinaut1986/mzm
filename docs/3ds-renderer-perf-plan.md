# 3DS port: renderer performance work order

Living tracker for issue #20 (frame rate) and the renderer changes it leads
to. Tick items as they land; add new ones as they come up. Everything under
**Measured** was taken off a real New3DS and should not be re-derived by
argument -- and everything under **Dead ends** was believed and then
disproved, so please read that section before proposing a fix.

> **Status 2026-09-04 (night), handing off mid-investigation.**
>
> With the 3D slider down a New3DS already holds 60 FPS. The 45 FPS case is
> the second eye, and the cost is per-quad, not per-pixel. Step A (16x16
> atlas blocks) is implemented and building but **still unverified**.
>
> **RIGHT NOW, PICK UP HERE.** The replay harness runs end to end under
> Azahar, unattended -- and its output is **wrong in a way that is NOT step
> A's fault**. Every replayed frame comes back with ~92% of pixels differing
> from the CPU PPU, and the image is sliced into vertical stripes: the
> content is recognisable (vine and pipe fragments) but scrambled in
> columns. An A/B settles the blame: a build with the block pass forced OFF
> produces **the same 92% striping**, so the fault is in the replay's dump
> path (or in Azahar's handling of it), not in the renderer change.
>
> **The next experiment, and it is a small one:** take an ordinary SCREEN
> DUMP inside Azahar with this same build and run it through
> `compare_render.py` (the non-replay mode). This morning's hardware screen
> dumps were clean -- six of ten pixel-identical -- so:
>   * striped screen dump too -> Azahar's `C3D_SyncDisplayTransfer` out of a
>     render target does not give the linear layout the dump assumes, and
>     the whole Azahar path needs rethinking (or verifying on hardware
>     instead).
>   * clean screen dump -> the fault is in `PlatformGpu3DS_DumpTopLeftTo`
>     being called at the wrong moment in the replay loop (the screen-dump
>     path dumps at a different point in the frame than right after
>     `EndBottom`), which would be a straightforward fix.
>
> Until that is resolved, **nothing has validated step A**, and the perf win
> it was written for is also unmeasured.
>
> Branch: `work/perf-instrumentation-and-harness` (pushed).

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

### 0. Harness: replay saved states through the installed build -- done

- [x] Screen dumps can only validate the build that took them, so every
      verification round needed fresh gameplay. `REPLAY RECORDING` in the
      debug tools menu re-renders a recording's saved PPU states through the
      installed renderer and dumps each frame; `compare_render.py --replay`
      diffs them against the CPU PPU. Non-destructive: the live PPU state is
      snapshotted and restored, so the running game survives.
- [x] Capped at 24 outputs spread across the recording (each screenshot is
      288KB) and forces slider 0 / PIXEL PERFECT itself, so the comparison
      does not depend on how the console was set up.
- [x] Runs unattended under Azahar via the file trigger (see **How to
      verify**), so a correctness pass needs no console and no hands.
- [ ] **Replayed frames come back striped.** ~92% of pixels differ from the
      CPU PPU and the image is sliced into vertical columns. Proven NOT to
      be step A: a build with the block pass forced off shows the same
      striping. See the Status block for the next experiment.
- [ ] Diff two builds' replays against each other on identical input, which
      isolates a change instead of measuring absolute agreement with the
      oracle. Done by hand for the A/B above (rebuild with the pass off);
      not scripted -- `compare_render.py` compares against the oracle only.

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
- [ ] **Verify on hardware through the harness.** Use the replay
      (`--replay`), not fresh dumps: `mzm-rec-01.bin` is on the card as the
      corpus. Animated-palette rooms are where a hole in the per-block
      staleness check would show, as frozen or mis-coloured tiles.
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

**Correctness, repeatably** -- replay a saved recording through whatever
build is installed. This is the mode to use after a renderer change: the
console re-renders the recording's saved PPU states, so the same frames are
checked every time and no gameplay is needed.

```sh
# once: put a corpus on the card (any mzm-rec-NN.bin, kept for reuse)
# then, after each build: debug tools -> REPLAY RECORDING -> side button
python3 tools/compare_render.py <dir-with-the-fetched-replay> --replay 01
```

Screen dumps validate only the build that took them -- their `-left.rgb` is
what the renderer drew that day -- so they cannot check a later build, which
is why the replay exists. Keep using dumps for one-off "what does this room
do" questions.

**Correctness, one-off** -- the GPU renderer against the CPU PPU oracle:

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

**Correctness under Azahar, unattended.** This works today and needs no
console and no hands -- it is how the replay above was exercised. Azahar's
GPU is emulated rather than a real PICA200, so agreement there is weaker
evidence than hardware; treat it as the fast inner loop and hardware as the
final gate. It is not useless as a proxy: issue #17's renderer bug did
reproduce in Azahar (see
[3ds-issue17-session-2026-08-25.md](3ds-issue17-session-2026-08-25.md)).

On this Windows box (the Linux equivalent is `tools/run_azahar_test.sh`,
which predates the replay and only covers the log/audio dump):

```powershell
# SD root: %APPDATA%\Azahar\sdmc  -- readable directly, no FTP
$sd = "$env:APPDATA\Azahar\sdmc\3ds"
Copy-Item <a mzm-rec-NN.bin> "$sd\mzm-rec-01.bin"
Set-Content "$sd\mzm-replay-request.txt" "1"        # the file trigger
& "C:\Program Files\Azahar\azahar.exe" -i <the .cia>   # install
# copy the freshly installed .app to %APPDATA%\Azahar\mzm.cxi, then:
Start-Process "C:\Program Files\Azahar\azahar.exe" -ArgumentList @("-f", $cxi)
# wait ~70s, kill it, and the frames are in $sd\mzm-replay-01-*.rgb
```

`mzm-replay-request.txt` exists precisely for this: Azahar has no touch
input to tap the menu row with. A single digit in the file picks the
recording slot. It is polled every 30 frames for the first ~15 seconds and
then never again, and it works the same on hardware if dropped over FTP.

Azahar is **worthless for speed** -- frame times there measure the host PC,
not the PICA200 -- so every performance number in this document has to come
off real hardware.

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
