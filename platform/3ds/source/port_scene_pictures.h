#ifndef PORT_SCENE_PICTURES_H
#define PORT_SCENE_PICTURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <citro3d.h>

/*
 * WIDE scene pictures: the part of a scene's background the game does not have, drawn by an
 * image AI around the game's own picture (docs/3ds-wide-scene-extension-plan.md, tools in
 * tools/scene-extend/). The renderer draws one behind the scene's background BG; the game's
 * own BG still draws over it wherever it has the picture.
 *
 * Stored as BG palette entries (port_scene_pictures_data.c, made by to_console.py), so it is
 * painted with whatever palette the game has on that frame and fades with it.
 */

typedef struct PortScenePictureDef {
    const char* name;
    int gameMode;        /* gMainGameMode it belongs to */
    int sceneKey;        /* which scene of that mode (PortScenePic_SceneKey) */
    int bg;              /* the background BG it extends */
    int canvasX, canvasY; /* the canvas's top-left, in that BG's pixels */
    int width, height;
    int pictureX0, pictureY0, pictureX1, pictureY1; /* the game's own picture, BG px */
    int texWidth, texHeight;
    const uint8_t* packed; /* LZ4, palette entries in texture order */
    size_t packedSize;
} PortScenePictureDef;

extern const PortScenePictureDef gPortScenePictures[];
extern const int gPortScenePictureCount;

/* The picture for this game mode and background BG, or NULL. Without WIDE + AI
 * (Port_Config_GetWideAi) only pictures that hold the game's own art are used, and then
 * without their generated parts: plain WIDE shows nothing an AI drew. */
const PortScenePictureDef* PortScenePic_Find(int gameMode, int bg);

/* Which scene of `gameMode` is showing, for modes that play several scenes on the same BG
 * (the intro: its stage); 0 for the others. In port_scene_game_art.c. */
int PortScenePic_SceneKey(int gameMode);

/* The texture of `def` painted with `bgPalette` (the GBA's 256 BG colours, BGR555). Loaded
 * on first use; repainted only when a colour it uses changed. NULL if it could not be made. */
C3D_Tex* PortScenePic_Texture(const PortScenePictureDef* def, const uint16_t* bgPalette);

/* Frees the loaded picture, if any (the scene is over). */
void PortScenePic_Release(void);

/* Whether the loaded picture also holds the game's own picture, built from the game's memory
 * when it was loaded (PortScenePic_FillGameArt): then the renderer draws the BG only inside
 * the GBA frame, where VRAM is sure to hold it, and the texture gives the rest. */
bool PortScenePic_HasGameArt(void);

/* Writes the game's own picture of `def` into `codes` (texture order, the same codes as the
 * generated parts) from the game's memory, for scenes whose VRAM does not hold all of it at
 * once (the title streams its pan). False if the scene has no such source. In
 * port_scene_game_art.c, next to the game's data. */
bool PortScenePic_FillGameArt(const PortScenePictureDef* def, uint8_t* codes);

/* Whether PortScenePic_FillGameArt has a source for `def` (same file). */
bool PortScenePic_HasGameArtSource(const PortScenePictureDef* def);

#endif
