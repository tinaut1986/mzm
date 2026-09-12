# PC preview tool for 3DS stereoscopic depth ("game engine" for cinematics/depth)

Working notes for an ongoing effort. Read this before picking the work back up.

## The goal

Be able to see, from the PC, exactly how the 3DS port will place every visual
element (background layers, sprites, menus, cutscenes) on the stereoscopic
depth planes -- *before* deploying to hardware -- driven by the depth logic as
it is in the code **right now**, not a hand-maintained copy of it that can
drift out of sync as the port changes.

This grew out of a conversation about extending the existing
`tools/layer-workbench/` tool (a single-file HTML app, no build step, no
server dependency beyond `serve.py`) rather than building something new from
scratch -- the workbench already decodes real room data from the repo and
already writes the two `.inc` correction tables the port actually compiles
(`port_layer_fixes.inc`, `port_sprite_depth.inc`), so extending it keeps a
single source of truth instead of a second tool that could disagree with it.

## Branch

`claude/game-engine-cinematics-upeves`, based on `release/v0.6.2`.

## Key existing pieces (read these before changing anything)

- `tools/layer-workbench/index.html` + `README.md` -- the tool itself. Large,
  carefully built UX (floating windows, alpha mixing, coordinate systems).
  Read the whole README before touching it.
- `tools/layer-workbench/build_maps.py` -- decodes all 315 rooms from repo
  data into `maps.json`.
- `tools/layer-workbench/build_sprites.py` / `build_sprite_thumbs.py` -- the
  sprite type catalog and thumbnails, from `include/constants/sprite.h` and
  `src/sprites_AI/`.
- `platform/3ds/source/port_stereo_depth.c/.h` -- the actual depth-tier
  decision. Deliberately dependency-free (no `<3ds.h>`, no citro3d, no game
  globals) so it compiles on the host -- `platform/3ds/tests/stereo_depth_test.c`
  already proves this.
- `platform/3ds/source/port_layer_fixes.c/.h/.inc` -- the curated per-block
  depth/draw-order correction list, authored by the workbench's Map mode.
- `platform/3ds/source/port_sprite_depth_oam.c/.h` / `port_sprite_depth.inc`
  -- per sprite-type depth override, authored by the workbench's Sprites mode.
- `platform/3ds/source/port_ppu_mzm.c` + `port_gpu_renderer.c` -- the door
  footprint rule (`PortPpuMzm_IsDoorDepthBlock`, `sDoorDepthOnScreen`): a
  runtime geometric rule (not a curated table) that pulls the door
  lintel/sill tiles onto the play plane whenever a doorway is on screen.

## Plan, in slices (deliberately incremental)

### Slice 1 -- in progress
**Goal:** replace "trust the tool's own JS logic" with "the tool calls the
real compiled C", and close the workbench's own documented gap (sprites are
only a disconnected type catalog, never shown placed in a room).

1. Compile `port_stereo_depth.c` + `port_layer_fixes.c` (plus a thin C shim
   exposing clean exported functions) to WebAssembly via Emscripten. The
   workbench loads that `.wasm` module instead of reimplementing the decision
   logic in JS, so a future change to the real C is picked up automatically
   the next time the module is rebuilt -- no second implementation to keep in
   sync. If Emscripten isn't available in the sandbox this was attempted in,
   that's flagged as a blocker with the exact local build command documented
   instead of faking it with a JS reimplementation (a JS stand-in would
   defeat the entire point of this slice).
2. Extend the workbench's Map mode to place each room's *statically known*
   sprites (type + position, from room spriteset data) on the room view,
   colored by their resolved depth tier from the WASM engine. Sprites whose
   tier is `BG_COPLANAR` (resolved at runtime from a dynamic `bgPriority`) are
   marked distinctly rather than given a fake fixed color, with a pointer to
   the existing Recording mode for the real per-instance value.

Status: an agent was dispatched to build this slice; check `git log` on this
branch for what actually landed, and re-read `tools/layer-workbench/README.md`
for whether it documents a new WASM/sprite-overlay section (it should, if the
slice completed).

### Slice 2 -- not started
**Door depth rule viewer.** The door lintel/sill pull
(`port_ppu_mzm.c`/`port_gpu_renderer.c`) isn't a curated table like the other
two correction lists -- it's a fixed geometric rule computed from door data
every frame. Door data itself (`sBrinstarDoors` and siblings in
`src/data/rooms_data.c`) is static, so the *rule* can be replicated in the
workbench (ideally by also compiling the relevant C rather than
reimplementing it, consistent with Slice 1) and shown overlaid on the room
view. This is view-only -- there is no curated list to edit here, since it's
not table-driven in the port; changing the rule means changing the C, not the
workbench.

### Slice 3 -- not started
**Dynamic-sprite indicator with source.** Statically scan `src/sprites_AI/*.c`
for writes to the sprite's depth/priority field, to:
- Flag which sprite types have runtime-dependent depth (candidates: any type
  whose AI conditionally writes that field, as opposed to a fixed value set
  once at spawn).
- Capture file + line of each such write and feed it into `sprites.json` (via
  `build_sprites.py`) so the workbench can show the actual source snippet for
  a selected dynamic sprite -- literally the current file contents, not a
  reinterpretation, so it can't drift either.
- Where feasible, extract the literal values assigned at each site as an
  "observed values in code" hint (best-effort, not authoritative -- the real
  per-instance value still requires a Recording).

### Slice 4 -- not started
**Cutscene depth corrections.** Cutscenes (`src/cutscenes/*.c`) are C control
flow, not a data table like rooms are, so today the only thing that affects
their depth is the *global*, per-game-mode flags in `port_stereo_depth.h`
(`bg0IsOverlayText`, `flatMenu`, ...) -- there is no per-cutscene, per-step
granularity. Doing what was asked (pick which layer of a specific cutscene
shot goes on which plane, and have it actually apply on real hardware) needs
a **new correction table**, analogous to `port_layer_fixes.inc` but keyed by
cutscene id / step instead of by room / block (e.g.
`PORT_CUTSCENE_DEPTH_FIX(cutsceneId, step, target, tier)`), plus a read point
in the cutscene rendering path analogous to `PortLayerFix_DestFor`. This slice
is mostly design work: deciding the table's key granularity and where to hook
it into `src/cutscenes/*.c`/`src/room_cutscene.c` without coupling it to each
cutscene's individual logic. Not started at all yet -- no code, no table
format decided beyond the sketch above.

## Explicitly out of scope / already decided against

- No JS reimplementation of `port_stereo_depth.c` / `port_layer_fixes.c`
  logic as a "good enough" substitute for WASM -- the whole point of this
  effort is to stop trusting a second copy of the logic.
- No build step / bundler / npm dependency added to the workbench -- it stays
  a single HTML file plus `serve.py`, with the WASM artifact as the one new
  build output (documented, ideally regenerated by `serve.py` the same way it
  already regenerates `maps.json`/`sprites.json`/`thumbs/` when sources are
  newer).
- Not touching `src/`, `include/`, `asm/`, `data/` (upstream decompilation)
  except to read them, per `CLAUDE.md`.

## How to pick this back up

1. `git log --oneline release/v0.6.2..claude/game-engine-cinematics-upeves`
   to see what actually landed vs. what's still just this plan.
2. Re-read `tools/layer-workbench/README.md` -- if Slice 1 landed, it should
   document the WASM engine and the sprite overlay there.
3. Run `python3 tools/layer-workbench/test_maps.py` to confirm the data
   pipeline still passes before building on top of it.
4. Continue with the next not-started slice above, in order -- each slice
   was deliberately scoped so it can land and be useful on its own.
