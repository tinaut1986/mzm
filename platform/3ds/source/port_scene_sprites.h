#ifndef PORT_SCENE_SPRITES_H
#define PORT_SCENE_SPRITES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <citro3d.h>

/*
 * WIDE scene sprites: a scene's sprite that the GBA screen cut off (its OAM pieces stop where
 * the screen ended), made longer by an image AI (docs/3ds-wide-scene-extension-plan.md, tools
 * in tools/scene-extend/). With WIDE + AI the renderer hides the game's pieces of that sprite
 * and draws one texture per animation frame in their place: the game's own pieces, rasterized
 * from OBJ VRAM when the texture is loaded (nothing of the game's art is shipped), plus the
 * generated parts around them.
 *
 * Stored as OBJ palette entries (port_scene_sprites_data.c, made by sprite_to_console.py), so it
 * is painted with whatever palette the game has on that frame and fades with it.
 */

typedef struct PortSceneSpriteDef {
    const char* name;
    int gameMode;          /* gMainGameMode it belongs to */
    int sceneKey;          /* which scene of that mode (PortScenePic_SceneKey) */
    int canvasDX, canvasDY; /* the canvas's top-left, from the sprite's anchor (GBA px) */
    int width, height;
    int texWidth, texHeight;
    int frameCount;
    const uint8_t* const* packed; /* per frame: LZ4, palette entries in texture order */
    const size_t* packedSize;
} PortSceneSpriteDef;

extern const PortSceneSpriteDef gPortSceneSprites[];
extern const int gPortSceneSpriteCount;

/* The sprite of this game mode's current scene, or NULL. Only with WIDE + AI
 * (Port_Config_GetWideAi): plain WIDE shows nothing an AI drew. */
const PortSceneSpriteDef* PortSceneSprite_Find(int gameMode);

/* Looks for `def` in OAM (`oam`, the GBA's 128 entries). When one of its frames is there,
 * sets hide[i] for every OAM entry of it, the frame, its anchor in GBA px (where the game put
 * the sprite this frame) and the OAM entry of its first piece (its priority and mode stand for
 * the whole sprite); false otherwise. */
bool PortSceneSprite_Match(const PortSceneSpriteDef* def, const uint16_t* oam, bool hide[128], int* frame,
                           int* anchorX, int* anchorY, int* firstSlot);

/* The texture of `frame` of `def` painted with `objPalette` (the GBA's 256 OBJ colours,
 * BGR555). Loaded on first use, with the game's pieces taken from OBJ VRAM (`obj1D`: the
 * DISPCNT mapping); repainted only when a colour it uses changed. NULL if it could not be
 * made. */
C3D_Tex* PortSceneSprite_Texture(const PortSceneSpriteDef* def, int frame, const uint16_t* objPalette, bool obj1D);

/* Frees the loaded sprite, if any (the scene is over). */
void PortSceneSprite_Release(void);

/* The game's OAM data of `frame` of `def` (src/data/: the piece count, then attr0..2 of each
 * piece, offsets from the anchor), or NULL. In port_scene_game_art.c, next to the game's
 * data. */
const uint16_t* PortSceneSprite_GameOam(const PortSceneSpriteDef* def, int frame);

#endif
