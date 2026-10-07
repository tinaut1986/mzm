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

/* Save states are an open-ended list: every `mzm-state<id>.bin` in the states
 * folder is one entry (ids start at 1 and are never reused while the file
 * exists), each with an optional `mzm-state<id>.thm` screenshot next to it.
 * The list is kept in RAM, newest first, and scanned from SD once. */

/* True only while a state may safely be captured or applied -- i.e. really
 * in gameplay (GM_INGAME / SUB_GAME_MODE_PLAYING), not in a menu, cutscene
 * or door transition. The UI greys its buttons out otherwise. */
bool Port_SaveState_Available(void);

/* Everything that touches the SD card is a job: the UI queues it, the work
 * runs from Port_SaveState_ServicePending at the top of the main loop (the
 * touch handler runs mid-frame inside Port_Bios_Halt), and a couple of frames
 * later than the request so the bottom screen has already drawn its "wait"
 * message. One job at a time; a request while busy is ignored. */
typedef enum {
    PORT_SS_JOB_NONE = 0,
    PORT_SS_JOB_SCAN,
    PORT_SS_JOB_SAVE,       /* new entry */
    PORT_SS_JOB_OVERWRITE,  /* same id, new contents */
    PORT_SS_JOB_LOAD,
    PORT_SS_JOB_DELETE,
    PORT_SS_JOB_THUMB       /* read one entry's screenshot */
} PortSaveStateJob;

/* Save and overwrite need Port_SaveState_Available(); load too. Scan, delete
 * and thumbnail work anywhere. */
void Port_SaveState_RequestScan(void);
void Port_SaveState_RequestSaveNew(void);
void Port_SaveState_RequestOverwrite(uint32_t id);
void Port_SaveState_RequestLoad(uint32_t id);
void Port_SaveState_RequestDelete(uint32_t id);
void Port_SaveState_RequestThumb(uint32_t id);

/* Job queued or running. The UI draws its "wait" message while this is true. */
PortSaveStateJob Port_SaveState_CurrentJob(void);
bool Port_SaveState_IsBusy(void);

/* False until the first scan has finished (Count is 0 until then). */
bool Port_SaveState_Scanned(void);

/* What the list and the detail modal show for one save state. Read from the
 * file's header, so a scan costs a short read per file. */
typedef struct {
    uint32_t id;
    bool     compatible;     /* written by this exact build: it can be loaded */
    bool     hasStats;       /* false for a slot written by an older format:
                              * only area/room are known */
    bool     hasThumb;
    uint8_t  area;
    uint8_t  room;
    uint32_t savedAt;        /* Unix seconds of the console clock; 0 = unknown */
    uint16_t energy, maxEnergy;
    uint16_t missiles, maxMissiles;
    uint8_t  superMissiles, maxSuperMissiles;
    uint8_t  powerBombs, maxPowerBombs;
    bool     hasPlayTime;    /* the game's own clock; absent from states saved before it was kept */
    uint8_t  playHours, playMinutes, playSeconds;
} PortSaveStateInfo;

/* Entries, newest first. */
int Port_SaveState_Count(void);
bool Port_SaveState_GetInfoAt(int index, PortSaveStateInfo* out);

/* Area name as shown in the list ("BRINSTAR", ...). */
const char* Port_SaveState_AreaName(unsigned area);

/* Result of the most recent job, for a one-line status toast. Empty string
 * until the first action. */
const char* Port_SaveState_LastMessage(void);
/* Frames remaining that the toast should be shown (counts itself down). */
int Port_SaveState_MessageTtl(void);

/* Set (and cleared) when a thumbnail read finishes: the id it belongs to, so
 * the UI can hand the pixels to the texture. Returns NULL when none is new. */
const uint16_t* Port_SaveState_TakeThumb(uint32_t* outId);

#ifdef __cplusplus
}
#endif

#endif /* PORT_SAVE_STATE_H */
