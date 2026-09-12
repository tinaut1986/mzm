/* Whole-machine save states for the 3DS port. See port_save_state.h for the
 * design rationale (why this file is GBA-side and what it deliberately does
 * NOT snapshot). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_gba_mem.h"        /* gEwram/gIwram/... + the u8/u16 typedefs */
#include "port_gpu_renderer.h"   /* Port_GpuRenderer_InvalidateAll */
#include "port_save_state.h"
#include "constants/game_state.h" /* GM_INGAME, SUB_GAME_MODE_PLAYING */

/* Decomp globals used only for the "am I in gameplay" gate and the slot
 * label -- extern'd rather than pulling in the big struct headers. */
extern u8  gMainGameMode;
extern u8  gSubGameMode1;
extern u8  gCurrentArea;
extern u8  gCurrentRoom;
extern u16 gFrameCounter16Bit;

/* Sound engine, same extern-not-header style. PlayCurrentMusicTrack() re-runs
 * InitTrack -> port_resolve_addr for gMusicInfo.musicTrack, rebuilding the
 * current song's resolved ROM pointers from the restored music id instead of
 * trusting the (host) pointers the snapshot carried -- see SsDoLoad. */
extern void PlayCurrentMusicTrack(void);

/* Bounds of the decompilation's scattered .data/.bss, bracketed by
 * ewram_symbols.ld. Linker symbols: take their addresses, never their
 * "values". */
extern char __ss_data_start[], __ss_data_end[];
extern char __ss_bss_start[],  __ss_bss_end[];

/* ------------------------------------------------------------------------- */

#define SS_MAGIC    0x314D5A53u   /* "SZM1" */
#define SS_VERSION  2
#define SS_REGIONS  10
#define SS_PATH_FMT "sdmc:/3ds/mzm-state%d.bin"

/* A snapshot is a raw dump of EWRAM/IWRAM/.data/.bss, and those regions are
 * full of ABSOLUTE host pointers whose targets only exist at the addresses
 * this build placed them: m4a track structs point at gTrackNVariables /
 * SoundChannel pools, clipdata/scroll code pointers point into .text, sprite
 * AI pointers, port_resolve_addr'd ROM pointers, ... Restoring a slot written
 * by a build with a DIFFERENT layout relocates none of that -- every such
 * pointer is then stale by however much things moved, and the first deref (or
 * the first write-through, as StopMusicOrSound does) faults.
 *
 * The header carries a fingerprint of the running build's DECOMPILATION
 * layout; a load whose fingerprint differs is refused rather than applied.
 * It is deliberately layout-based, not a timestamp: a rebuild that does not
 * move the decomp's code or its .data/.bss bracket (e.g. a pure port-side
 * change, with __ss_data_start pinned in 3dsx.ld) keeps the same fingerprint,
 * so slots taken before it still load. Anchors:
 *   - a decomp .text address (shifts if decomp code grows/reorders)
 *   - the decomp .data bracket bounds (shift if decomp globals change)
 *   - the decomp .bss bracket bounds
 * Residual, uncaught risk: reordering two globals strictly inside a bracket
 * without changing its bounds or the .text anchor. Rare; a dev tool. */
static uint32_t SsBuildFingerprint(void) {
    const uintptr_t anchors[] = {
        (uintptr_t)&PlayCurrentMusicTrack,
        (uintptr_t)__ss_data_start, (uintptr_t)__ss_data_end,
        (uintptr_t)__ss_bss_start,  (uintptr_t)__ss_bss_end,
    };
    uint32_t h = 2166136261u;
    const unsigned char* p = (const unsigned char*)anchors;
    for (size_t i = 0; i < sizeof(anchors); ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

typedef struct {
    const char* name;
    void*       ptr;
    uint32_t    size;
} SsRegion;

static int SsBuildRegions(SsRegion r[SS_REGIONS]) {
    int n = 0;
    r[n++] = (SsRegion){ "EWRAM",  gEwram,   (uint32_t)sizeof(gEwram)   };
    r[n++] = (SsRegion){ "IWRAM",  gIwram,   (uint32_t)sizeof(gIwram)   };
    r[n++] = (SsRegion){ "IO",     gIoMem,   (uint32_t)sizeof(gIoMem)   };
    r[n++] = (SsRegion){ "BGPAL",  gBgPltt,  (uint32_t)sizeof(gBgPltt)  };
    r[n++] = (SsRegion){ "OBJPAL", gObjPltt, (uint32_t)sizeof(gObjPltt) };
    r[n++] = (SsRegion){ "OAM",    gOamMem,  (uint32_t)sizeof(gOamMem)  };
    r[n++] = (SsRegion){ "VRAM",   gVram,    (uint32_t)sizeof(gVram)    };
    r[n++] = (SsRegion){ "SRAM",   gSramMem, (uint32_t)sizeof(gSramMem) };
    r[n++] = (SsRegion){ "DDATA",  __ss_data_start,
                         (uint32_t)(__ss_data_end - __ss_data_start) };
    r[n++] = (SsRegion){ "DBSS",   __ss_bss_start,
                         (uint32_t)(__ss_bss_end - __ss_bss_start) };
    return n; /* == SS_REGIONS */
}

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t regionCount;
    uint32_t regionSize[SS_REGIONS];
    uint32_t area;
    uint32_t room;
    uint32_t frame16;
    uint32_t buildFingerprint;   /* SsBuildFingerprint() at save time */
    uint32_t reserved[2];
} SsHeader;

/* ------------------------------------------------------------------------- */

static int  sPendingSave = -1;
static int  sPendingLoad = -1;

/* Consecutive frames Port_SaveState_Available() has held true; a save/load is
 * only serviced once this reaches SS_READY_FRAMES. ~half a second at 60fps. */
#define SS_READY_FRAMES 30
static int  sReadyFrames = 0;

static char sMsg[48] = "";
static int  sMsgTtl   = 0;

static bool sScanned = false;
static bool sSlotUsed[PORT_SAVE_STATE_SLOTS];
static char sSlotLabel[PORT_SAVE_STATE_SLOTS][28];

static const char* SsAreaName(unsigned a) {
    static const char* const kNames[] = {
        "BRINSTAR", "KRAID", "NORFAIR", "RIDLEY",
        "TOURIAN", "CRATERIA", "CHOZODIA",
    };
    return (a < sizeof(kNames) / sizeof(kNames[0])) ? kNames[a] : "???";
}

static void SsSetMsg(const char* m) {
    snprintf(sMsg, sizeof(sMsg), "%s", m);
    sMsgTtl = 180;
}

/* ------------------------------------------------------------------------- */

bool Port_SaveState_Available(void) {
    return gMainGameMode == GM_INGAME && gSubGameMode1 == SUB_GAME_MODE_PLAYING;
}

void Port_SaveState_RequestSave(int slot) {
    if (slot < 0 || slot >= PORT_SAVE_STATE_SLOTS) return;
    sPendingSave = slot;
    sPendingLoad = -1;
}

void Port_SaveState_RequestLoad(int slot) {
    if (slot < 0 || slot >= PORT_SAVE_STATE_SLOTS) return;
    sPendingLoad = slot;
    sPendingSave = -1;
}

/* ------------------------------------------------------------------------- */

static void SsDoSave(int slot) {
    SsRegion r[SS_REGIONS];
    int n = SsBuildRegions(r);

    char path[64];
    snprintf(path, sizeof(path), SS_PATH_FMT, slot + 1);
    FILE* f = fopen(path, "wb");
    if (!f) { SsSetMsg("GUARDADO FALLIDO (SD)"); return; }

    SsHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = SS_MAGIC;
    h.version = SS_VERSION;
    h.regionCount = (uint32_t)n;
    for (int i = 0; i < n; ++i) h.regionSize[i] = r[i].size;
    h.area = gCurrentArea;
    h.room = gCurrentRoom;
    h.frame16 = gFrameCounter16Bit;
    h.buildFingerprint = SsBuildFingerprint();

    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    for (int i = 0; i < n && ok; ++i)
        ok = fwrite(r[i].ptr, 1, r[i].size, f) == r[i].size;
    if (fclose(f) != 0) ok = false;

    if (!ok) { remove(path); SsSetMsg("GUARDADO FALLIDO"); return; }

    sSlotUsed[slot] = true;
    snprintf(sSlotLabel[slot], sizeof(sSlotLabel[slot]), "%s  SALA %u",
             SsAreaName(gCurrentArea), (unsigned)gCurrentRoom);
    char m[48];
    snprintf(m, sizeof(m), "SLOT %d GUARDADO", slot + 1);
    SsSetMsg(m);
}

static void SsDoLoad(int slot) {
    char path[64];
    snprintf(path, sizeof(path), SS_PATH_FMT, slot + 1);
    FILE* f = fopen(path, "rb");
    if (!f) { SsSetMsg("SLOT VACIO"); return; }

    SsHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1 ||
        h.magic != SS_MAGIC || h.version != SS_VERSION ||
        h.regionCount != SS_REGIONS) {
        fclose(f);
        SsSetMsg("SLOT INCOMPATIBLE");
        return;
    }

    /* Reject a slot from any other build: its snapshot is full of absolute
     * host pointers that only resolve against that build's layout (see
     * SsBuildFingerprint). Restoring it anyway is the StopMusicOrSound /
     * UpdateTrack data-abort. */
    if (h.buildFingerprint != SsBuildFingerprint()) {
        fclose(f);
        SsSetMsg("SLOT DE OTRA BUILD");
        return;
    }

    SsRegion r[SS_REGIONS];
    int n = SsBuildRegions(r);
    uint32_t total = 0;
    for (int i = 0; i < n; ++i) {
        if (h.regionSize[i] != r[i].size) {
            fclose(f);
            SsSetMsg("SLOT DE OTRA VERSION");
            return;
        }
        total += r[i].size;
    }

    /* Read the whole payload before touching live memory: a short read must
     * not leave the machine half-restored. */
    unsigned char* buf = (unsigned char*)malloc(total);
    if (!buf) { fclose(f); SsSetMsg("SIN MEMORIA"); return; }
    bool ok = fread(buf, 1, total, f) == total;
    fclose(f);
    if (!ok) { free(buf); SsSetMsg("SLOT CORRUPTO"); return; }

    uint32_t off = 0;
    for (int i = 0; i < n; ++i) {
        memcpy(r[i].ptr, buf + off, r[i].size);
        off += r[i].size;
    }
    free(buf);

    /* VRAM, palettes and every tilemap just changed wholesale -- make the
     * GPU tile renderer rebuild its caches instead of trusting stale ones. */
    Port_GpuRenderer_InvalidateAll();

    /* The snapshot's m4a track structs carry pRawData/pHeader HOST pointers
     * that InitTrack resolved from ROM at song-start (port_resolve_addr in
     * port_gba_mem.c). They are valid for this build (the fingerprint check
     * guaranteed the slot is ours), but a load taken the instant a room's
     * music was still being set up can still restore a half-initialised
     * track. Re-run the current song from its restored id so those pointers
     * are rebuilt rather than trusted. */
    PlayCurrentMusicTrack();

    char m[48];
    snprintf(m, sizeof(m), "SLOT %d CARGADO", slot + 1);
    SsSetMsg(m);
}

void Port_SaveState_ServicePending(void) {
    if (sMsgTtl > 0) --sMsgTtl;

    /* Debounce the "in gameplay" gate. Port_SaveState_Available() only checks
     * GM_INGAME / SUB_GAME_MODE_PLAYING, and both are already true partway
     * through a room load -- while doors, streaming and (the crash that
     * prompted this) the room's music are still being set up. Require the
     * gate to have held for a short run of frames so a save or load only
     * fires once the room is genuinely live. */
    if (Port_SaveState_Available()) {
        if (sReadyFrames < SS_READY_FRAMES) ++sReadyFrames;
    } else {
        sReadyFrames = 0;
    }

    if (sPendingSave < 0 && sPendingLoad < 0) return;
    if (sReadyFrames < SS_READY_FRAMES) return;

    if (sPendingSave >= 0) {
        int s = sPendingSave;
        sPendingSave = -1;
        SsDoSave(s);
    } else {
        int s = sPendingLoad;
        sPendingLoad = -1;
        SsDoLoad(s);
    }
}

/* ------------------------------------------------------------------------- */

static void SsScan(void) {
    if (sScanned) return;
    sScanned = true;
    for (int s = 0; s < PORT_SAVE_STATE_SLOTS; ++s) {
        sSlotUsed[s] = false;
        sSlotLabel[s][0] = '\0';
        char path[64];
        snprintf(path, sizeof(path), SS_PATH_FMT, s + 1);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        SsHeader h;
        if (fread(&h, sizeof(h), 1, f) == 1 && h.magic == SS_MAGIC) {
            sSlotUsed[s] = true;
            snprintf(sSlotLabel[s], sizeof(sSlotLabel[s]), "%s  SALA %u",
                     SsAreaName(h.area), (unsigned)h.room);
        }
        fclose(f);
    }
}

void Port_SaveState_RefreshSlots(void) {
    sScanned = false;
    SsScan();
}

bool Port_SaveState_SlotUsed(int slot) {
    if (slot < 0 || slot >= PORT_SAVE_STATE_SLOTS) return false;
    SsScan();
    return sSlotUsed[slot];
}

void Port_SaveState_SlotLabel(int slot, char* out, int outSize) {
    if (!out || outSize <= 0) return;
    out[0] = '\0';
    if (slot < 0 || slot >= PORT_SAVE_STATE_SLOTS) return;
    SsScan();
    snprintf(out, (size_t)outSize, "%s", sSlotLabel[slot]);
}

const char* Port_SaveState_LastMessage(void) { return sMsg; }
int Port_SaveState_MessageTtl(void) { return sMsgTtl; }
