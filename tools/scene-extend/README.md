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
| `gba_dump.py` | Draws a dump's text BGs and sprites (affine sprites left out). |

The picture of a scene that streams its graphics while it scrolls (the title) is put together
from the rows each dump's frame shows; VRAM outside the frame holds other things by then.

Everything these write is the game's art: keep it out of the repository (the packages so far live
in `~/Imágenes/mzm-<scene>/`). Dumps and pictures need a local ROM's game running on the console;
nothing here reads the ROM itself.
