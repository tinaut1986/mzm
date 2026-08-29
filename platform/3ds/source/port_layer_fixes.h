#pragma once

/*
 * Curated per-block depth corrections.
 *
 * port_stereo_depth.h derives depth from BGCNT priority and nothing else, and
 * says why: every attempt to INFER exceptions broke something. It also says
 * where the exceptions belong -- "a curated correction list, where a human has
 * looked at each entry". This is that list.
 *
 * A correction moves the block in BOTH senses -- its depth plane and its 2D
 * draw order -- because that is what "wrong layer" means on screen: a tile the
 * room paints over Samus has to stop painting over her, not merely stop
 * standing out in relief.
 *
 * The list is authored in tools/layer-workbench, which renders every room from
 * the repo's own data and lets a human move one block at a time. It is written
 * out as platform/3ds/source/port_layer_fixes.inc, an X-macro list:
 *
 *   PORT_LAYER_FIX(area, room, fromBg, blockX, blockY, block, destBg)
 *
 * The file is OPTIONAL. Without it the port behaves exactly as before -- the
 * table is empty and the lookup is skipped entirely -- so a build with no
 * corrections and a build with them differ only by that one file.
 *
 * No <3ds.h> and no game headers, for the same reason as port_stereo_depth.h:
 * platform/3ds/tests can drive this on the host.
 */

#include <stdbool.h>
#include <stdint.h>

/* True when a correction list was compiled in at all. Lets the render loop
 * skip the per-tile lookup completely on a stock build. */
bool PortLayerFix_Present(void);

/*
 * Selects the corrections for a room and validates them.
 *
 * `bgData`/`bgW`/`bgH` are the room's decompressed block maps, one per BG
 * (NULL where the room has no such layer). Every correction carries the block
 * value it was authored against; one that no longer matches the room data is
 * DROPPED rather than applied, because the alternative is silently moving a
 * different block than the human picked. Returns how many survived.
 *
 * Cheap enough to call on every room change: the list is a handful of entries.
 */
int PortLayerFix_SetRoom(int area, int room,
                         const uint16_t* const* bgData,
                         const uint16_t* bgW, const uint16_t* bgH);

/* How many corrections are live for the current room. */
int PortLayerFix_ActiveCount(void);

/*
 * Destination layer for a tile being drawn, or -1 for "leave it alone".
 *
 * colBlock/rowBlock are the tile's ABSOLUTE block position in the room --
 * the same coordinate the corrections are authored against, not the
 * screenmap position. The screenmap wraps every 32 blocks across and 16
 * down (RoomUpdate*Tilemap masks with tmpX&0xF / yPos&0xF, src/room.c:795),
 * so matching on it aliased every correction onto a lattice of blocks 32
 * across / 16 down and moved whichever alias happened to be on screen --
 * e.g. an entry for room block (9,43) also hit (9,59). The caller
 * (port_gpu_renderer.c) now recovers the absolute block from the BG scroll
 * registers via PortPpuMzm_ScreenOrigin.
 *
 * 4 means "sprite level"; 0..3 are BG levels.
 */
int PortLayerFix_DestFor(int bg, int colBlock, int rowBlock);
