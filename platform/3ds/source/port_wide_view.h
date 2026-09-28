#ifndef PORT_WIDE_VIEW_H
#define PORT_WIDE_VIEW_H

#include <stdbool.h>

/*
 * "WIDE" display aspect: instead of stretching the 240x160 GBA picture to fit
 * the 400x240 top screen, show the part of the world the GBA hides beyond the
 * frame. Works with both display styles -- SCALED (1.5x, only a sliver more
 * at the left/right) and PIXEL PERFECT (1:1, extra on every side).
 *
 * Only in-game world rendering can be widened: menus and cutscenes have no
 * world data past the frame, so they keep drawing as ORIGINAL.
 */

/* The WIDE aspect is selected in the settings. */
bool PortWide_Selected(void);

/* WIDE is selected AND the game is in a mode with a world to widen. What the
 * renderer asks for; whether it manages it on a given frame is
 * PortWide_FrameDrawn(). */
bool PortWide_GameActive(void);

/* How much extra world is shown on each side of the 240x160 GBA frame, in GBA
 * pixels. Zero unless PortWide_GameActive(). */
int PortWide_MarginX(void);
int PortWide_MarginY(void);

/* Whether the frame that was just drawn actually shows the widened view. The
 * GPU renderer sets it every frame; the CPU fallback clears it. Screen-space
 * effects read it to know how big the picture is. */
void PortWide_SetFrameDrawn(bool drawn);
bool PortWide_FrameDrawn(void);

/* Same margins in sub-pixels, for the culling checks in src/. Nonzero only
 * while the frames are actually being drawn widened (PortWide_FrameDrawn),
 * so the game never keeps sprites alive in an area that is not shown. */
int Port_WideMarginSubPixelX(void);
int Port_WideMarginSubPixelY(void);

/* The true screen position (px, may be outside the frame) the game recorded for
 * the sprite that emitted OAM slot `oamIndex` this frame. False for slots the
 * game did not tag (HUD, screen-fixed sprites). */
bool PortWide_SlotOrigin(int oamIndex, int* outY, int* outX);

/* The door tunnel of a door transition (defined in port_ppu_mzm.c): whether
 * it is on screen, over the room being left or sliding to the new room's
 * door, and where it is / is headed (BG3 scroll, px). While sliding,
 * outPause is how far through the game's pause between the vertical and
 * the horizontal slide it is (0..3 frames; 3 once the horizontal slide has
 * begun). Any pointer may be NULL. The WIDE view follows the tunnel from
 * the old room's view to the new. */
enum { PORT_DOOR_TUNNEL_NONE = 0, PORT_DOOR_TUNNEL_OLD_ROOM, PORT_DOOR_TUNNEL_SLIDING };
enum { PORT_DOOR_TUNNEL_PAUSE_FRAMES = 3 };
int PortPpuMzm_DoorTunnel(int* outX, int* outY, int* outTargetX, int* outTargetY, int* outPause);

#endif /* PORT_WIDE_VIEW_H */
