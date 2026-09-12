#ifndef PORT_SAVE_STATE_H
#define PORT_SAVE_STATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whole-machine save states for the 3DS port (issue: "save/load state").
 *
 * The GBA machine state this port mutates lives in two places: the flat
 * emulated-memory arrays in port/port_gba_mem.c (EWRAM/IWRAM/IO/palette/OAM/
 * VRAM/SRAM) and the decompilation's own scattered .data/.bss/iwram_data
 * globals. The linker script (ewram_symbols.ld) brackets the latter into
 * [__ss_data_start,__ss_data_end) + [__ss_bss_start,__ss_bss_end) so both
 * halves can be snapshotted with a handful of memcpy calls. Nothing here
 * touches the port's own runtime state (citro3d handles, threads, renderer
 * caches, heap) -- restoring those wholesale would leave dangling pointers.
 *
 * This module is deliberately GBA-side (no <3ds.h>): it must see the real
 * u32 typedef and the emulated-memory symbols, exactly like the debug warp
 * code in port_ppu_mzm.c. The bottom-screen tab that drives it lives in
 * port_bottom_ui_3ds.c and only calls the functions below. */

#define PORT_SAVE_STATE_SLOTS 6

/* True only while a state may safely be captured or applied -- i.e. really
 * in gameplay (GM_INGAME / SUB_GAME_MODE_PLAYING), not in a menu, cutscene
 * or door transition. The UI greys its buttons out otherwise. */
bool Port_SaveState_Available(void);

/* Queue a save/load of `slot` (0..PORT_SAVE_STATE_SLOTS-1). The work itself
 * runs from Port_SaveState_ServicePending at the top of the main loop, for
 * the same reason the debug warp is deferred there (the touch handler runs
 * mid-frame inside Port_Bios_Halt). A newer request replaces a pending one. */
void Port_SaveState_RequestSave(int slot);
void Port_SaveState_RequestLoad(int slot);

/* Called once per main-loop iteration from src/agbmain.c. No-op unless a
 * request is pending and Port_SaveState_Available(). */
void Port_SaveState_ServicePending(void);

/* Slot directory, refreshed from SD on the first call and after each save.
 * Port_SaveState_RefreshSlots forces a re-scan (call when the tab opens). */
void Port_SaveState_RefreshSlots(void);
bool Port_SaveState_SlotUsed(int slot);
/* Short human label for the slot: "AREA 2  SALA 8" style, or "" if empty. */
void Port_SaveState_SlotLabel(int slot, char* out, int outSize);

/* Result of the most recent save/load, for a one-line status toast. Empty
 * string until the first action. */
const char* Port_SaveState_LastMessage(void);
/* Frames remaining that the toast should be shown (counts itself down). */
int Port_SaveState_MessageTtl(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_SAVE_STATE_H */
