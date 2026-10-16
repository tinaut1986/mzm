# Plan: extending non-gameplay screens in WIDE / Pixel Perfect with generated art

**Status: planned, nothing built (2026-10-14).** This file is the whole brief: when the owner
says "let's build it", start at "Order of work" and follow it. Nothing here needs to be
re-researched except the items marked **VERIFY**, which need a screen dump from the console.

## 1. What and why

In WIDE + PIXEL PERFECT the top screen is 400x240 and gameplay shows real world data around
the GBA's 240x160 frame (margins 80 x 40, `port_wide_view.c`). Everything that is **not**
`GM_INGAME` (title, intro, file select, cutscenes, ending, credits, game over, gallery) still
draws as ORIGINAL: a 240x160 picture with nothing around it. The owner wants those screens to
fill 400x240 too. The game's data has nothing past the 240x160 frame, so the missing parts are
**generated with an image AI from a screen dump** and drawn by the port around the game's own
picture. The same system exists and works in the sibling port `../zelda-tmc-3ds`
(Minish Cap); this plan is how to bring it here.

Scope: `platform/3ds/` and `tools/` only. **Nothing in `src/` changes** unless a section
below says so, and then read `docs/3ds-save-states.md` first (a new global shifts the
decomp's layout and invalidates every save state; never `#ifdef PORT_DEBUG_TOOLS` in `src/`).

## 2. The reference implementation (read these, do not re-derive)

All in `../zelda-tmc-3ds` (branch `release/v0.1.3`, 2026-10-14):

| What | Where |
|---|---|
| The whole procedure, prompt, checks | `docs/wide-scene-extension.md` |
| Dump -> BG picture + 400x240 template | `tools/scene-extend/extract_bg.py` |
| Verify the AI's picture | `tools/scene-extend/check_extension.py` |
| Picture -> `.bin` for the console (RGB565, strips, or palette indices) + FTP upload | `tools/scene-extend/to_console.py` |
| Continue a palette-animated layer without AI (parallel strokes) | `tools/scene-extend/extend_rays.py` |
| Loader of a shipped/SD picture, colour correction, fade | `platform/3ds/source/scene_pictures_3ds.[ch]` |
| Presenter: draws the pieces outside the game's frame, scrolls them, veils them with the game's fade | `DrawScenePictures`, `DrawCutsceneStrip`, `DrawFrameEdges` in `platform/3ds/source/platform_gpu_3ds.c` |
| Gating by scene (`..._ViewLive`, `..._CutsceneStrips(&x,&y)`) | `port/port_widescreen.h`, `port/port_linked_stubs.c` |
| BG repeat across the wide view | `virtuappu_mode1_bg_repeat_mask` set in `port_ppu_3ds.c` |

How it works there, in one paragraph: the game draws its scene as usual. For the scene's
screen the port widens the frame to 400x240, lets the scene's BG wrap (repeat) where the
game's picture ends, and the presenter draws the AI picture's pieces *outside* the original
area, scrolled with the game's own scroll, through the game's colour conversion and fades.
Only the generated parts are ever stored (the original area is transparent in the shipped
files), and a file of the same name in the SD data folder overrides the shipped one so the
owner can try a new picture without a build.

## 3. What MZM has today (verified 2026-10-14)

- `PortWide_GameActive()` = WIDE selected **and** `gMainGameMode` is `GM_INGAME` or `GM_DEMO`
  (`platform/3ds/source/port_wide_view.c`). Margins in PIXEL PERFECT: 80 x 40.
- **Since 2026-10-16 the intro (`GM_INTRO`) is widened with its own data, no AI**
  (`PortWide_SceneActive()`, `ComputeSceneWideView` in `port_gpu_renderer.c`): the view
  shows as much of the background BG's picture as the BG really holds (the frame grown
  line by line while the next tile column/row is drawn across the frame), centred when it
  fits, scrolled with the game when it does not, black past it. Its BGs are 256x256 with
  256x160/176 pictures, so it gains columns and a few rows. Adding a mode to
  `PortWide_SceneActive()` is all another scene needs for that part; the AI work below is
  for what the BG does not hold. Every other mode still returns margin 0 (plain 240x160).
- Game modes (`include/constants/game_state.h`): `GM_SOFT_RESET, GM_INTRO, GM_TITLE,
  GM_FILE_SELECT, GM_INGAME(4), GM_MAP_SCREEN, GM_GAMEOVER, GM_CHOZODIA_ESCAPE, GM_CREDITS,
  GM_TOURIAN_ESCAPE, GM_CUTSCENE, GM_DEMO, GM_GALLERY, GM_FUSION_GALLERY, ...`.
  `gMainGameMode` is already read in `port_gpu_renderer.c` (cutscene depth) and
  `port_wide_view.c`.
- In-game WIDE already solved the renderer side for *real* world data (tile/sprite ranges
  widened, tiles past VRAM's window rebuilt from the room's block map, OAM Y/X wrap tags).
  None of that applies to scenes: they have no world past the frame. What carries over is the
  `sExtX/sExtY` plumbing in `port_gpu_renderer.c` and the rule "skip WIDE on frames with
  windows, BG3 haze or affine BG2" (`sWideOn`, line ~4346).
- Stereo: cutscenes get depth from `port_cutscene_depth.[ch]` / `.inc` (per-scene tables).
  **Every extension piece needs a depth plane** (Zelda has no stereo, so nothing to copy).
  Rule of thumb: the extension goes on the same plane as the BG it continues.
- Dumps: DEBUG tab -> SCREEN DUMP writes `mzm-dump-NN-{vram,io,bgpltt,objpltt,oam}.bin`,
  `-left/-right/-bottom.rgb` (`docs/3ds-debug-tools.md`, "Screen dump"; the `.rgb` are
  portrait 240x400, rotate 90 degrees). `tools/mzm-link/mzm-link.py` can load a save state
  and take the dump from this machine (`docs/3ds-debug-tools.md`, "FTP server, install and
  remote control"). Fetch files **by name**, not by listing the folder (the Zelda notes: a
  listing with a thousand files hung the FTP server).
- Plain debug build first: `make clean && make DEBUG_TOOLS=1` (a plain build compiles the
  menu out; see the CIA-delivery note in CLAUDE.md's console section).

## 4. Scene inventory

BG sizes come from the `screenSize` fields in `src/data/cutscenes/*_data.c` and
`src/data/menus/title_screen_data.c` (`0` = 256x256, `0x4000` = 512x256, `0x8000` = 256x512,
`0xC000` = 512x512). Everything in the "Recipe" column is a *hypothesis from the data
tables*; confirm each with a dump (**VERIFY**) before spending an AI run on it.

| Screen | Mode | BGs (size) | Probable recipe |
|---|---|---|---|
| Title | `GM_TITLE` | BG1 title/logo (256x256, prio 1), BG3 space background (256x512, prio 3) | **C**: the space BG scrolls/wraps; extend it. The logo (BG1) and sparkles (OBJ) stay centred. **VERIFY** what actually fills the corners |
| Intro (`src/intro.c`) | `GM_INTRO` | BG0, BG1 256x256 | **A** or **B**, **VERIFY** |
| File select | `GM_FILE_SELECT` | **VERIFY** (`src/menus/file_select.c`, `data/menus/file_select`) | **A** (+ **F** if palette-cycled) |
| Language select, boot debug, erase SRAM | menus | plain | **B** |
| Game over | `GM_GAMEOVER` | **VERIFY** | **A** or **B** |
| Ending images 0-7 (`image_top_N`, `image_bottom_N`) | `GM_CREDITS` | mix of 256x256, 256x512, 512x256 (`ending_and_gallery_data.c`) | **A** per image, one picture each; best ratio of effort to result |
| Credits chozo wall / drawing | `GM_CREDITS` | 512x256 and 256x512 | **D** or **C**, **VERIFY** |
| Chozodia escape, Tourian escape | `GM_CHOZODIA_ESCAPE`, `GM_TOURIAN_ESCAPE` | 256x256 x7 + one 256x512 | **VERIFY**; mostly gameplay-like, probably leave as ORIGINAL |
| Getting fully powered suit | `GM_CUTSCENE` | 2 x 256x512, 1 x 256x256, uses WIN1 | **C** (tall, vertical scroll) for the BG; windows -> **E** during the flash. Closest relative of the Minish portal scene |
| Ridley in space | `GM_CUTSCENE` | 2 x 256x512, 4 x 256x256 | **C** + **G** (stars) ; affine/zoom parts -> **E**. **Left ORIGINAL (2026-10-16)**: widened with its own data its layers grew by different amounts (a patchwork, the view jumping between layers); excluded in `PortWide_SceneActive` |
| Kraid rising | `GM_CUTSCENE` | 4 x 256x256 | **C**-like: vertical scroll, BG wraps at 256 |
| Statue opening | `GM_CUTSCENE` | 3 x 256x256 | **A**/**C**, **VERIFY** |
| Samus in blue ship | `GM_CUTSCENE` | 1 x 256x256 | **A**/**G** |
| Mecha sees Samus | `GM_CUTSCENE` | 1 x 256x256 | **A** |
| Story text | `GM_CUTSCENE` | 1 x 256x256 | **B** (text on plain) |
| Before ruins test (chozo wall) | `GM_CUTSCENE` | 4 x 512x256, 5 x 256x256 | **D**: the wall is wider than the screen; only the scroll's ends need art. **Left ORIGINAL (2026-10-16)**, at the owner's request: every cutscene gets a proper pass later |
| Enter Tourian | `GM_CUTSCENE` | 256x256 + 512x256 | **D**. **Left ORIGINAL (2026-10-16)**: the game fills the 512 BG in as it pans, so the extent grew while it played and the picture jumped; needs a fixed extent (or an AI picture) |
| Mother Brain close-up, Ridley landing, Ridley spawn | `GM_CUTSCENE` | no page table found in `src/data/cutscenes/` | **VERIFY**; close-up uses affine/zoom -> **E** (leave ORIGINAL) |
| Map screen / pause | `GM_MAP_SCREEN` etc. | menus | leave ORIGINAL unless the owner asks |

## 5. Recipes (what to do in each case)

**A. Fixed picture, 240x160 (title logo area, ending images, file select backdrop).**
1. Dump the screen (section 7). Find the BG that holds the picture from `io.bin`
   (`BGnCNT`: char base, screen base, size, 4/8 bpp).
2. Extract it with the template at the original's place (80,40 for 400x240).
3. AI prompt (section 8), 2-3 variants; owner picks.
4. `check_extension`: 0 changed pixels in the original area, 0 off-palette.
5. Convert to a full 400x240 `.bin` (centre ignored), ship the generated parts only.
6. Presenter draws four pieces around the frame (top, bottom, left, right), as
   `DrawScenePictures` does for the Zelda title.

**B. Plain colour background (menus, story text, blank frames).** No AI. The presenter
stretches the frame's edge columns and rows to the screen's edges
(`DrawFrameEdges` / `Port_Widescreen_StretchEdges` in Zelda). Do this first for every
screen that qualifies: it is cheap and removes the black bars immediately.

**C. Tall BG, vertical scroll, 256 wide (portal-like: suit, Ridley, Kraid, title's space BG).**
1. Let the BG repeat horizontally across the 400-wide view (renderer repeat mask for that
   BG; the wrapped columns are the problem the strips cover).
2. Two side strips from AI: columns left of the 256-wide picture (up to 80) and right (up
   to 64), as tall as the BG (256 or 512). Template: the whole BG centred.
3. Presenter draws the strips scrolled with the BG's own Y (and X when it moves), like
   `DrawCutsceneStrip`. Without a file the strips are black (the Zelda default).
4. If the scene has several BGs, extend only the one that fills the edges; the rest stay
   where the game puts them.

**D. Wide BG (512x256) scrolling horizontally.** The game already has more picture than
the screen. Only where the scroll reaches the BG's end (or the frame is wider than the data
beyond that end) does the 400 view show something that is not there. Extend those ends
only (a strip at each end), and let the view's scroll range grow by the same amount if the
scene's own clamp would otherwise cut it.

**E. Affine, zoom, HDMA, windows, mosaic.** Leave as ORIGINAL (240x160 centred, bars). The
port already skips WIDE on frames with windows or affine BG2 for gameplay; the same rule
applies here. Revisit scene by scene only if the owner insists.

**F. Palette-cycled background.** Do not ship colours: ship palette **indices** and rebuild
the texture from the palettes of the frame being shown, so the extension shimmers on the same
frames as the original (Zelda: `to_console.py fileselect --palettes`,
`FadeGpu3DS_ShownPalettes`). Needed wherever the game rotates a BG palette bank.

**G. Stars / noise / repeating texture.** Procedural: continue by tiling a stretch of the
original, like `extend_rays.py`. No AI, nothing shipped.

## 6. Port work, in order

1. **Scene view gating** (new file, e.g. `port_scene_view.[ch]`; keep `port_wide_view.c`'s
   in-game logic untouched). Provide `PortScene_Active()` and the per-scene queries, driven
   by `gMainGameMode` and the cutscene id. Output: the 400x240 frame for those scenes with
   the game's 240x160 centred at (80,40). Respect WIDE + PIXEL PERFECT only (in Scaled the
   margin is too small to be worth it; decide with the owner).
2. **Frame size**: the top screen presenter must hand the renderer a 400x240 frame in these
   modes (see how `PortWide_FrameDrawn` is set by the GPU renderer, and how the Zelda
   `sTopPresentMode == FULL_VIEW_OUTDOOR_1X` is chosen).
3. **Edge stretch (recipe B)** for every screen: immediate visible win, no assets.
4. **Picture loader** (`scene_pictures_*`): SD file first, romfs second, lazy, released when
   the scene ends; RGB565 or indices; colour correction applied like the game's. Port from
   `scene_pictures_3ds.c`, generalised to a table of scenes (the Zelda file is one scene
   per global; with ~20 scenes use a small descriptor array).
5. **Presenter**: `DrawScenePictures` equivalent in `platform_gpu_3ds.c`, after the game's
   frame, with the fade veil. Pieces: four-sided (A), strips (C/D), indexed (F).
6. **BG repeat** hook for recipe C/D in `port_gpu_renderer.c` (a per-frame BG mask like
   `virtuappu_mode1_bg_repeat_mask`).
7. **Depth planes** for the pieces in `port_cutscene_depth` (extension = plane of the BG it
   continues). Check in 3D on the console: the seam must not show.
8. **Fades**: iris/fade-to-white/black must veil the extension exactly like the frame
   (Zelda: `VeilScenePicture`).
9. **Save states**: scene pictures are presenter state, not game state, so nothing is
   saved. If a hook in `src/` turns out to be unavoidable, `docs/3ds-save-states.md` first.
10. **Settings text**: check whether the WIDE option's help text (`port_bottom_ui_3ds.c`)
    still says menus and cutscenes are not widened, and update it when the first scene ships.

## 7. Tooling work

**Done so far (2026-10-16):** `tools/scene-extend/` (README) has `capture.sh` (dumps from the
console), `gba_dump.py` (draws BGs and sprites from a dump) and `make_package.py` (the section 8b
folder per scene, from a `SCENES` table) and `to_console.py` (a chosen picture ->
`port_scene_pictures_data.c`). Packages written for the title (400x440, the whole scroll with
40 px above and below so the logo ends centred) and four intro scenes. Still to do from the
list below: `check_extension.py`.

**Shipped (2026-10-16): the title.** The owner's variant 2 of the title package is in
`port_scene_pictures_data.c`: only the generated parts, stored as BG palette entries (painted
each frame with the game's palette, so it fades with it; a palette change repaints only the
texels of the colours that moved), LZ4, ~45 KB. `port_scene_pictures.[ch]` load it on the
scene's first frame and free it when the scene ends; the renderer draws it behind the BG it
extends, on that BG's stereo plane and with its BLDY brightness, and the view's extent is the
canvas. The title streams its pan (two tile sets take turns in VRAM), so the game's own part
outside the GBA frame is not in VRAM when it shows: `port_scene_game_art.c` builds it into the
same texture from the game's memory when the picture loads (nothing of the game's art is in
the repository), and the BG then draws only the frame's tile lines. Checked on the 2DS: the
pan, skipping it, ~59-60 fps. Two traps met on the way: citro2d skips binding a texture it
thinks is bound while the batch binds the atlas behind its back (bind it explicitly), and a
layer map's quad covers the whole widened view, cached cells of the last room included (bound
it to the layer's clipped range in scenes).

Copy `tools/scene-extend/` from the Zelda repo into `tools/scene-extend/` here, in English,
and adapt:

- `extract_bg.py`: dump file names `mzm-dump-NN-{vram,io,bgpltt,oam}.bin` (Zelda's are
  `tmc-dump-NNNN-vram.bin`, `-io-registers.bin`, `-palettes.bin`). GBA BG formats are the
  same (text BG, 4bpp/8bpp, 32x32 screen blocks, sizes 256x256 / 512x256 / 256x512 /
  512x512, tile flip bits), so the decoder should carry over. Palette: BG palette RAM is
  `bgpltt.bin` (256 x BGR555).
- `extract_bg.py` should also write the delivery package of section 8b in one go
  (`--package`: template, original, composite, do-not-paint layer, `README-AI.md` filled
  from the dump's numbers, palette count included).
- `check_extension.py`: unchanged.
- `to_console.py`: scene table for MZM (name -> picture size, columns kept, output file,
  mode: four-sided / strips / indexed). Output names `wide-scenes/<scene>.bin` in
  `platform/3ds/romfs/`, sources in `platform/3ds/assets/wide-scenes/` (generated parts only,
  original area transparent).
- Upload over the game's FTP server (port 5000, the credentials in
  `platform/3ds/source/ftp_credentials_3ds.h`) into the data folder of the app.
- A scene-recorder for cutscenes (Zelda: `tmc-link.py scenerec` + `tools/scene-rec/decode.py`)
  is useful to find which frame of a cutscene to dump; `mzm-link.py` has the "Scene recorder"
  in the debug tools (`mzm-rec-NN.bin`, see `docs/3ds-debug-tools.md`).

## 8. The AI prompt (starting point; proven on the Minish portal and title)

Give the tool `template.png` (to fill), `picture.png` and `picture-with-sprites.png`
(references), and:

> This is a background from a Game Boy Advance game (pixel art, 1 pixel = 1 game pixel). Fill
> the transparent areas of the template so the picture continues naturally. Rules:
> - Do not change a single pixel of the existing picture (the rectangle X..X): it is the
>   game's own art and will be drawn on top of yours.
> - Use only colours that already appear in the picture. No anti-aliasing, gradients, blur or
>   dithering the original does not use, no new hues.
> - Same pixel scale and style: hard pixel edges, the same outlines and shading steps.
> - Continue what reaches the edges so the seams are invisible; add more of the same kind of
>   thing, not new elements. Do not repeat or mirror the existing picture.
> - Leave out anything that is a sprite in the reference with sprites: only the background.
> - Output: a PNG exactly W x H (the template's size), no transparency left.

Per scene add: which sides to fill, the picture's rectangle, and what must not be painted
(logo, text, light effects, anything the game draws over the background). Ask for 2-3
variants; the owner chooses. Keep the owner's raw material (dumps, templates, whole
pictures with the game's art) **off the repository**; only generated parts ship.

## 8b. The delivery package for the AI (one folder per scene)

The AI tool is given **one folder** and must need nothing else. Zelda did this by hand
(`~/Imágenes/<scene>/` with a `PLAN-antigravity.md` or `LEEME.txt`; see
`~/Imágenes/elige-archivo/` and `~/Imágenes/intro-titulo/` for finished examples). For MZM the
extract tool should write the whole package in one command, so nothing is assembled by hand.

Folder `~/Imágenes/mzm-<scene>/` (outside the repository, always: it is the ROM's art):

| File | Content | Why the AI needs it |
|---|---|---|
| `README-AI.md` | The task, the rules, the exact numbers (see template below) | The only document it has to read |
| `1-template-WxH.png` | **The canvas to fill.** The original picture at its place, transparent everywhere else | What it edits; its size is the output's size |
| `2-original-picture.png` | The game's background alone, 1:1, no scaling | Reference for style, palette, pixel scale |
| `3-final-composite.png` | The screen as the player sees it (background + every layer and sprite, at that moment) | Context only: shows what the game draws *over* the background |
| `4-do-not-paint.png` (optional) | The layers/sprites that go over it (logo, rays, text, characters), alone on transparent | Says exactly what must not end up in the background |

Dump sources (`mzm-dump-NN-*`) can sit in a `source/` subfolder for Claude; the AI is told to
ignore it.

Rules that make the package work (all learned on the Zelda scenes):

- **Original pixels exactly as the game has them**: no resampling, no colour conversion, no
  JPEG, PNG only. Template and picture must be pixel-identical in the shared area.
- **Fixed size and position, said in numbers** in `README-AI.md` (template WxH, original
  rectangle x,y,w,h, how many pixels to add on each side). Never "make it bigger".
- **Palette count** of the original, and a note of which area uses which colours when it
  matters (dark edges vs bright centre).
- **Transparent means "fill me"**; the output has no transparency left.
- The composite (3) and the do-not-paint layer (4) are **context, not canvas**: the AI must not
  copy sprites/logos from them into the background.
- Ask for 2-3 variants; each returned as its own PNG, same size as the template.
- The AI's output is **never trusted**: `check_extension.py` against `2-original-picture.png`
  must report 0 changed pixels in the original area and 0 off-palette pixels before anything
  is converted (if the original area changed, paste the original back over it).

### `README-AI.md` template (fill the `<...>`, keep it in English)

```markdown
# Extend a Game Boy Advance background: <scene name>

You get a pixel-art background from Metroid: Zero Mission (240x160, 1 pixel = 1 game
pixel). It must be enlarged to <W>x<H> by generating the missing area around it.

## Files
- `1-template-<W>x<H>.png`: the canvas. The original is at x=<X>..<X+w-1>, y=<Y>..<Y+h-1>;
  everything else is transparent and is what you must fill.
- `2-original-picture.png`: the original alone, for reference.
- `3-final-composite.png`: how the game shows it (background plus <logo / text / sprites>).
  Context only.
- `4-do-not-paint.png`: <what the game draws over the background>. Context only.

## What to add
<sides and sizes, e.g. 80 px left, 80 px right, 40 px above, 40 px below>.
<What continues at each edge: a sentence per side, e.g. "left and right: more of the
stone wall and the pipes; top: the ceiling beams; bottom: floor plating".>

## Rules
1. Do not change a single pixel inside the original rectangle. It is the game's own art and
   is drawn over yours; any change shows as a seam.
2. Use only colours that already appear in the original (<N> colours). No anti-aliasing,
   gradients, blur, dithering the original does not use, or new hues.
3. Same pixel scale and style: hard 1-pixel edges, the same outlines and shading steps.
4. Continue what touches the original's edges so the seams are invisible. Add more of the
   same kind of elements; no new objects, characters or text.
5. Do not repeat or mirror the original.
6. Do not paint anything listed as "do not paint": <list>. Background only.
7. Output: a PNG exactly <W>x<H>, no transparency, no scaling, no smoothing.
   Give <2-3> variants as separate files.
```

Fill the per-scene sentences from the dump (what the picture is, which sides need what).
The scene-specific details are what made the Zelda variants usable; a generic prompt gives
generic results.

## 9. Order of work

1. Port tooling (section 7) and take dumps of every candidate screen in one console session
   (title, intro, file select, game over, each ending image, then each cutscene via save
   states/recorder). Fill the **VERIFY** cells of the table in section 4.
2. Scene gating + 400x240 frame + edge stretch (recipe B) for all non-gameplay screens.
3. **Pilot: title screen** (single mode, static) end to end with an AI picture: loader,
   presenter, depth, fades. Console check in 3D.
4. Ending images (recipe A), then file select (A/F), intro, game over.
5. Scrolling cutscenes: getting fully powered suit and Kraid rising first (recipe C), then
   Ridley in space (C + G), then 512-wide ones (D).
6. Leave E scenes ORIGINAL; note them in the user-facing docs.
7. Release notes line per shipped batch (`docs/release-notes/<tag>.md`, player-facing
   English, no jargon); ask before choosing a minor bump.

Done when, per screen: the owner sees it at 400x240 on the console in WIDE + PIXEL PERFECT,
no seam at the original's edges in 2D or 3D, fades and palette animation continue into the
extension, and with no extension file the sides are plain (black or edge-stretched), never
garbage.

## 10. Open decisions for the owner

- ~~Ship the generated parts in the romfs?~~ **Decided 2026-10-14 (owner): yes.** The
  generated parts ship in the romfs so players get them with no setup, as the Zelda port
  does (decision of 2026-10-13). Only the generated areas ship, with the game's original
  area left transparent (no pixel of the game's art in the files); a file of the same name in
  the SD data folder still overrides the shipped one. The dump, template, whole pictures and
  the AI delivery folders stay out of the repository.
- **Scaled style:** scenes only in PIXEL PERFECT, or also scaled (its WIDE margin is only 14 x 0 px)?
- **Which cutscenes are worth an AI run** and which stay ORIGINAL (recipe E ones).
- **Size budget:** a full 400x240 RGB565 picture is ~188 KB; strips are smaller. With ~20
  scenes expect 2-4 MB of romfs; check the CIA size and the Old 3DS memory (load on demand,
  release at scene end).
- **Old 3DS cost:** each extra textured quad per frame counts; measure with the PERF recorder
  before and after the pilot.
