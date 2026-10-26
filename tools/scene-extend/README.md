# scene-extend: backgrounds for the generative AI

Tools for the WIDE scenes plan (`docs/3ds-wide-scene-extension-plan.md`): they turn screen
dumps of a scene into the folder an image AI is given to draw the part of the background the
game does not have (section 8b of the plan).

| File | What |
|---|---|
| `capture.sh IP OUTDIR MODE COUNT [WAIT] [STEP]` | Soft-resets a running debug build, waits for a game mode and takes screen dumps (needs DEBUG -> FTP SERVER on). |
| `make_package.py DUMPS_DIR OUT_DIR [SCENE ...]` | Writes `mzm-<scene>/` per scene: template (transparent and black), original picture, final composite, do-not-paint layer, `README-AI.md`. The scenes (which dumps, which BG, the picture's and the canvas's rectangles, what to tell the AI) are the `SCENES` table at its top. |
| `make_sprite_package.py DUMPS_DIR OUT_DIR [SPRITE ...]` | The same for a scene's sprite the GBA screen cut off (the intro's heat trail): a canvas per animation frame (transparent, or magenta for empty), the sprite alone, the WIDE view with it blended as the game does, the neighbouring sprites, `README-AI.md`. Its `SPRITES` table holds the frames' OAM entries from `src/data/`. |
| `sprite_to_console.py PACKAGE_DIR VARIANT [SPRITE ...]` | The chosen variant of a sprite package -> `platform/3ds/source/port_scene_sprites_data.c`: only the generated pixels (the console draws the game's pieces from OBJ VRAM), as OBJ palette entries looked up in the build's extracted palette. Refuses a variant that changes a pixel of the template. |
| `refit.py PACKAGE_DIR SCENE SOURCE OUT.png [MIN MAX]` (also writes `OUT.raw.png`, the AI's colours before the palette step; each game pixel takes the majority colour of its cell, `REFIT_MEAN=1` for the mean) | Rescues a variant whose generated sides do not meet the original: from the image the AI drew before cropping (SOURCE), finds where and at what scale the game's picture sits, crops the canvas again from there, maps it to the original's colours and puts the original back. Prints the fit and any canvas pixels SOURCE does not cover. |
| `fix_scanlines.py PACKAGE_DIR SCENE REFIT.png OUT.png` | For scenes with a scanline pattern in their art (the file select): redraws the generated area from `REFIT.raw.png` with the game's colours (histogram-matched over the original's own rectangle), the AI's lines divided out, the seam brightness matched and the dark rows made the way the game makes them. |
| `match_tones.py PACKAGE_DIR SCENE REFIT.png OUT.png` | The same idea for scenes without scanlines: a fitted colour transform AI -> game, a brightness match along the seam, a feathered mix across it, then the game's palette. |
| `seam_band.py PACKAGE_DIR SCENE BASE.png OUT_DIR [WIDTH]` | A repair pass for the AI: the band just outside the original painted magenta, plus a README asking it to repaint only that. |
| `gba_dump.py` | Draws a dump's text BGs and sprites (affine sprites left out). |

The picture of a scene that streams its graphics while it scrolls (the title) is put together
from the rows each dump's frame shows; VRAM outside the frame holds other things by then.

Everything these write is the game's art: keep it out of the repository (the packages so far live
in `~/Imágenes/mzm-<scene>/`). Dumps and pictures need a local ROM's game running on the console;
nothing here reads the ROM itself.
