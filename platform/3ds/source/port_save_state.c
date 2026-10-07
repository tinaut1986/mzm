/* Whole-machine save states for the 3DS port. See port_save_state.h for the
 * design rationale (why this file is GBA-side and what it deliberately does
 * NOT snapshot). */

#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "port_gba_mem.h"        /* gEwram/gIwram/... + the u8/u16 typedefs */
#include "structs/samus.h"       /* struct Equipment */
#include "port_gpu_renderer.h"   /* Port_GpuRenderer_InvalidateAll */
#include "port_save_state.h"
#include "port_state_thumb.h"
#include "port_state_vars.h"
#include "port_state_ptrs.h"
#include "port_rom.h"          /* gRomRegion */
#include "structs/audio.h"
#include "structs/in_game_timer.h"   /* gInGameTimer */     /* struct MusicInfo: the few fields kept across a load */
#include "constants/game_state.h" /* GM_INGAME, SUB_GAME_MODE_PLAYING */
#include "port_paths.h"

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

/* Re-derive every host pointer into ROM data from the loaded ROM, the same two
 * calls Port_LoadRom ends with (port/generated/port_all_rom_init.c and
 * port/port_constructor_init.c): pure assignments, safe to repeat. */
extern void PortGen_All_Init(void);
extern void Port_InitConstructorPointers(void);

/* Bounds of the decompilation's scattered .data/.bss, bracketed by
 * ewram_symbols.ld. Linker symbols: take their addresses, never their
 * "values". */
extern char __ss_data_start[], __ss_data_end[];
extern char __ss_bss_start[],  __ss_bss_end[];

/* ------------------------------------------------------------------------- */

#define SS_MAGIC    0x314D5A53u   /* "SZM1" */
#define SS_VERSION  3          /* build-bound format (host pointers kept as they are) */
#define SS_VERSION_V4 4        /* build-independent format, see Ss4Save */
#define SS_VERSION_V5 5        /* format 4 with the play time in the header */
#define SS4_MEM_REGIONS 8      /* EWRAM, IWRAM, IO, BGPAL, OBJPAL, OAM, VRAM, SRAM */
#define SS4_END_MARK 0x21444E45u   /* "END!" */
#define SS_REGIONS  10
#define SS_PATH_FMT PORT_STATES_DIR "/mzm-state%u.bin"
#define SS_THUMB_FMT PORT_STATES_DIR "/mzm-state%u.thm"

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
    uint32_t savedAt;            /* Unix seconds, console clock */
    uint16_t energy, maxEnergy;
    uint16_t missiles, maxMissiles;
    uint8_t  superMissiles, maxSuperMissiles;
    uint8_t  powerBombs, maxPowerBombs;
    /* Version 5: the game's own clock (gInGameTimer) at the time of the save. */
    uint8_t  igtHours, igtMinutes, igtSeconds, igtValid;
    uint32_t reserved;
} SsHeader;
#define SS_HEADER_V4_SIZE 84u   /* sizeof(SsHeader) before the play time */

/* Version 2 wrote the same fields up to buildFingerprint followed by two
 * reserved words, i.e. 76 bytes; version 3 appended the stats after that.
 * The payload starts right after the header, so a v2 file has to be read
 * with the v2 length or every region would be misaligned. */
#define SS_VERSION_V2     2
#define SS_HEADER_V2_SIZE 76u
_Static_assert(offsetof(SsHeader, savedAt) <= SS_HEADER_V2_SIZE &&
               sizeof(SsHeader) > SS_HEADER_V2_SIZE, "SsHeader layout");

/* Reads a slot's header, leaving the file positioned at the payload.
 * Accepts the current version and v2 (its stats stay zero). Returns 0 for a
 * short read or a file that is not a save state, else the file's version. */
static uint32_t SsReadHeader(FILE* f, SsHeader* h) {
    memset(h, 0, sizeof(*h));
    if (fread(h, SS_HEADER_V2_SIZE, 1, f) != 1 || h->magic != SS_MAGIC) return 0;
    if (h->version == SS_VERSION_V2) {
        /* v2's two reserved words overlap the start of the new fields. */
        h->savedAt = 0;
        return SS_VERSION_V2;
    }
    if (h->version != SS_VERSION && h->version != SS_VERSION_V4 && h->version != SS_VERSION_V5)
        return h->version ? h->version : 0;
    const size_t full = h->version == SS_VERSION_V5 ? sizeof(*h) : SS_HEADER_V4_SIZE;
    if (fread((char*)h + SS_HEADER_V2_SIZE, full - SS_HEADER_V2_SIZE, 1, f) != 1) return 0;
    return h->version;
}


/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */

/* Consecutive frames Port_SaveState_Available() has held true; a save/load is
 * only serviced once this reaches SS_READY_FRAMES. ~half a second at 60fps. */
#define SS_READY_FRAMES 30
static int  sReadyFrames = 0;

/* A job waits this many service calls before it runs, so the bottom screen
 * has drawn (and the display swapped) its "wait" message by the time the SD
 * card blocks the main loop. A save or load that cannot start for this many
 * frames after that is dropped rather than leaving the message up forever. */
#define SS_JOB_DELAY_FRAMES 3
#define SS_JOB_GIVE_UP_FRAMES 180

static PortSaveStateJob sJob = PORT_SS_JOB_NONE;
static uint32_t sJobId = 0;
static int      sJobDelay = 0;
static int      sJobWaited = 0;

static char sMsg[48] = "";
static int  sMsgTtl   = 0;

/* One entry per state file, newest first. */
typedef struct {
    uint32_t          id;
    PortSaveStateInfo info;
} SsEntry;

static SsEntry* sEntries = NULL;
static int      sCount = 0;
static int      sCap = 0;
static bool     sScanned = false;

/* Result of the last thumbnail job, until the UI takes it. */
static uint16_t sThumbBuf[PORT_THUMB_PIXELS];
static uint32_t sThumbBufId = 0;
static bool     sThumbBufFresh = false;

#define SS_THUMB_MAGIC 0x314D4854u   /* "THM1" */

const char* Port_SaveState_AreaName(unsigned a) {
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

static void SsPath(char* out, size_t n, uint32_t id) { snprintf(out, n, SS_PATH_FMT, (unsigned)id); }
static void SsThumbPath(char* out, size_t n, uint32_t id) { snprintf(out, n, SS_THUMB_FMT, (unsigned)id); }

/* ------------------------------------------------------------------------- */
/* The in-RAM list. */

static int SsEntryCmp(const void* a, const void* b) {
    const SsEntry* x = (const SsEntry*)a;
    const SsEntry* y = (const SsEntry*)b;
    if (x->info.savedAt != y->info.savedAt) return x->info.savedAt < y->info.savedAt ? 1 : -1;
    if (x->id != y->id) return x->id < y->id ? 1 : -1;
    return 0;
}

static int SsFind(uint32_t id) {
    for (int i = 0; i < sCount; ++i)
        if (sEntries[i].id == id) return i;
    return -1;
}

static void SsUpsert(const SsEntry* e) {
    int i = SsFind(e->id);
    if (i < 0) {
        if (sCount == sCap) {
            int cap = sCap ? sCap * 2 : 16;
            SsEntry* grown = (SsEntry*)realloc(sEntries, (size_t)cap * sizeof(SsEntry));
            if (!grown) return;
            sEntries = grown;
            sCap = cap;
        }
        i = sCount++;
    }
    sEntries[i] = *e;
    qsort(sEntries, (size_t)sCount, sizeof(SsEntry), SsEntryCmp);
}

static void SsRemove(uint32_t id) {
    int i = SsFind(id);
    if (i < 0) return;
    memmove(&sEntries[i], &sEntries[i + 1], (size_t)(sCount - i - 1) * sizeof(SsEntry));
    --sCount;
}

/* The number the player sees: the position in the list, newest first, from 1.
 * The id only names the file. */
static unsigned SsNumber(uint32_t id) {
    const int i = SsFind(id);
    return i < 0 ? 0u : (unsigned)i + 1u;
}

static uint32_t SsNextId(void) {
    uint32_t max = 0;
    for (int i = 0; i < sCount; ++i)
        if (sEntries[i].id > max) max = sEntries[i].id;
    return max + 1;
}

static uint32_t SsRomIdentity(void);

static bool SsHeaderLoadable(const SsHeader* h, uint32_t version) {
    if (version == SS_VERSION_V4 || version == SS_VERSION_V5) {
        /* Format 4 only has to belong to this ROM, and the memory regions have
         * to be the size this build has. */
        if (h->buildFingerprint != SsRomIdentity() || h->regionCount != SS4_MEM_REGIONS) return false;
        SsRegion r[SS_REGIONS];
        SsBuildRegions(r);
        for (int i = 0; i < SS4_MEM_REGIONS; ++i)
            if (h->regionSize[i] != r[i].size) return false;
        return true;
    }
    if (version != SS_VERSION && version != SS_VERSION_V2) return false;
    if (h->regionCount != SS_REGIONS) return false;
    if (h->buildFingerprint != SsBuildFingerprint()) return false;
    SsRegion r[SS_REGIONS];
    int n = SsBuildRegions(r);
    for (int i = 0; i < n; ++i)
        if (h->regionSize[i] != r[i].size) return false;
    return true;
}

static void SsInfoFromHeader(const SsHeader* h, uint32_t version, PortSaveStateInfo* info) {
    memset(info, 0, sizeof(*info));
    info->area = (uint8_t)h->area;
    info->room = (uint8_t)h->room;
    info->compatible = SsHeaderLoadable(h, version);
    if (version < SS_VERSION) return;   /* v2: area/room only, no stats */
    info->hasStats = true;
    info->savedAt = h->savedAt;
    info->energy = h->energy;
    info->maxEnergy = h->maxEnergy;
    info->missiles = h->missiles;
    info->maxMissiles = h->maxMissiles;
    info->superMissiles = h->superMissiles;
    info->maxSuperMissiles = h->maxSuperMissiles;
    info->powerBombs = h->powerBombs;
    info->maxPowerBombs = h->maxPowerBombs;
    if (version >= SS_VERSION_V5 && h->igtValid) {
        info->hasPlayTime = true;
        info->playHours = h->igtHours;
        info->playMinutes = h->igtMinutes;
        info->playSeconds = h->igtSeconds;
    }
}

static bool SsFileExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Reads one state file's header into `e`. False if it is not a save state. */
static bool SsReadEntry(uint32_t id, SsEntry* e) {
    char path[160];
    SsPath(path, sizeof(path), id);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    SsHeader h;
    const uint32_t version = SsReadHeader(f, &h);
    fclose(f);
    if (version == 0) return false;
    e->id = id;
    SsInfoFromHeader(&h, version, &e->info);
    SsThumbPath(path, sizeof(path), id);
    e->info.hasThumb = SsFileExists(path);
    e->info.id = id;
    return true;
}

static void SsScanNow(void) {
    sCount = 0;
    DIR* d = opendir(PORT_STATES_DIR);
    if (d) {
        struct dirent* de;
        while ((de = readdir(d)) != NULL) {
            unsigned id = 0;
            char tail[8] = "";
            if (sscanf(de->d_name, "mzm-state%u%7s", &id, tail) != 2 || strcmp(tail, ".bin") != 0 || id == 0)
                continue;
            SsEntry e;
            if (SsReadEntry(id, &e)) SsUpsert(&e);
        }
        closedir(d);
    }
    sScanned = true;
}

/* ------------------------------------------------------------------------- */
/* Thumbnails: a tiny header plus PORT_THUMB_PIXELS RGB565 values. */

static bool SsWriteThumb(uint32_t id, const uint16_t* pixels) {
    char path[160];
    SsThumbPath(path, sizeof(path), id);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t hdr[2] = { SS_THUMB_MAGIC, ((uint32_t)PORT_THUMB_W << 16) | (uint32_t)PORT_THUMB_H };
    bool ok = fwrite(hdr, sizeof(hdr), 1, f) == 1 &&
              fwrite(pixels, sizeof(uint16_t), PORT_THUMB_PIXELS, f) == PORT_THUMB_PIXELS;
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

static bool SsReadThumb(uint32_t id, uint16_t* pixels) {
    char path[160];
    SsThumbPath(path, sizeof(path), id);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint32_t hdr[2] = { 0, 0 };
    bool ok = fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == SS_THUMB_MAGIC &&
              hdr[1] == (((uint32_t)PORT_THUMB_W << 16) | (uint32_t)PORT_THUMB_H) &&
              fread(pixels, sizeof(uint16_t), PORT_THUMB_PIXELS, f) == PORT_THUMB_PIXELS;
    fclose(f);
    return ok;
}


/* ------------------------------------------------------------------------- */
/* Pointer census (debug builds). Every save also writes
 * debug/state-census-<id>.txt: the 32-bit words of the state's writable
 * regions that look like host pointers, grouped by what they point at. This
 * is the evidence for the save state format that carries no host pointers:
 * which of them can be turned back into GBA addresses (they point into the
 * emulated memory arrays or the ROM image) and which need rebuilding on load
 * (code, constant tables, the decomp's own globals). tools/state_census_report.py
 * names the targets using the ELF of the same build. */
#ifdef PORT_DEBUG_TOOLS

extern char __start__[], __end__[];   /* image bounds, from 3dsx.ld */
extern u8* gRomData;
extern u32 gRomSize;

typedef struct { const char* name; const void* base; uint32_t size; } SsSpace;

static int SsSpaces(SsSpace out[16]) {
    int n = 0;
    out[n++] = (SsSpace){ "EWRAM",  gEwram,   (uint32_t)sizeof(gEwram)   };
    out[n++] = (SsSpace){ "IWRAM",  gIwram,   (uint32_t)sizeof(gIwram)   };
    out[n++] = (SsSpace){ "IO",     gIoMem,   (uint32_t)sizeof(gIoMem)   };
    out[n++] = (SsSpace){ "BGPAL",  gBgPltt,  (uint32_t)sizeof(gBgPltt)  };
    out[n++] = (SsSpace){ "OBJPAL", gObjPltt, (uint32_t)sizeof(gObjPltt) };
    out[n++] = (SsSpace){ "OAM",    gOamMem,  (uint32_t)sizeof(gOamMem)  };
    out[n++] = (SsSpace){ "VRAM",   gVram,    (uint32_t)sizeof(gVram)    };
    out[n++] = (SsSpace){ "SRAM",   gSramMem, (uint32_t)sizeof(gSramMem) };
    out[n++] = (SsSpace){ "ROM",    gRomData, gRomSize };
    out[n++] = (SsSpace){ "DDATA",  __ss_data_start, (uint32_t)(__ss_data_end - __ss_data_start) };
    out[n++] = (SsSpace){ "DBSS",   __ss_bss_start,  (uint32_t)(__ss_bss_end - __ss_bss_start) };
    return n;
}

/* Raw GBA addresses are build-independent and need no work. */
static bool SsIsGbaAddress(uint32_t v, uint32_t romSize) {
    return (v >= 0x02000000u && v < 0x02040000u) || (v >= 0x03000000u && v < 0x03008000u) ||
           (v >= 0x04000000u && v < 0x04000400u) || (v >= 0x05000000u && v < 0x05000400u) ||
           (v >= 0x06000000u && v < 0x06018000u) || (v >= 0x07000000u && v < 0x07000400u) ||
           (v >= 0x08000000u && v < 0x08000000u + romSize) || (v >= 0x0E000000u && v < 0x0E010000u);
}

/* Classifies one word. Returns NULL for words that are plainly not pointers
 * into this process. `detail` gets "<space>+<offset>" for a known space. */
static const char* SsClassify(uint32_t v, const SsSpace* spaces, int nSpaces, char* detail, size_t dn) {
    detail[0] = '\0';
    for (int i = 0; i < nSpaces; ++i) {
        const uintptr_t base = (uintptr_t)spaces[i].base;
        if (base && v >= base && v < base + spaces[i].size) {
            snprintf(detail, dn, "%s+0x%X", spaces[i].name, (unsigned)(v - base));
            return (i >= 9) ? "BRACKET" : "HOSTARRAY";
        }
    }
    if (SsIsGbaAddress(v, gRomSize)) return NULL;
    if (v >= (uintptr_t)__start__ && v < (uintptr_t)__end__) return "IMAGE";
    if ((v >= 0x08000000u && v < 0x10000000u) || (v >= 0x14000000u && v < 0x20000000u)) return "HEAP";
    return NULL;
}

static void SsWriteCensus(uint32_t id) {
    char path[160];
    snprintf(path, sizeof(path), PORT_DEBUG_DIR "/state-census-%u.txt", (unsigned)id);
    FILE* f = fopen(path, "w");
    if (!f) return;

    SsSpace spaces[16];
    const int nSpaces = SsSpaces(spaces);
    fprintf(f, "# state %u, fingerprint %08X, image %p-%p\n", (unsigned)id,
            (unsigned)SsBuildFingerprint(), (void*)__start__, (void*)__end__);
    for (int i = 0; i < nSpaces; ++i)
        fprintf(f, "S %s %08X %X\n", spaces[i].name, (unsigned)(uintptr_t)spaces[i].base, (unsigned)spaces[i].size);

    /* Only the regions a state restores can hold pointers worth knowing. */
    static const char* const kScan[] = { "EWRAM", "IWRAM", "DDATA", "DBSS" };
    unsigned counts[4][4] = {{0}};   /* region x { HOSTARRAY, BRACKET, IMAGE, HEAP } */
    enum { MAX_LINES = 6000 };
    unsigned lines = 0;
    for (size_t r = 0; r < sizeof(kScan) / sizeof(kScan[0]); ++r) {
        const SsSpace* sp = NULL;
        for (int i = 0; i < nSpaces; ++i)
            if (!strcmp(spaces[i].name, kScan[r])) sp = &spaces[i];
        if (!sp) continue;
        const uint32_t* w = (const uint32_t*)sp->base;
        for (uint32_t off = 0; off + 4 <= sp->size; off += 4) {
            const uint32_t v = w[off / 4];
            if (v < 0x100000u) continue;
            char detail[40];
            const char* cls = SsClassify(v, spaces, nSpaces, detail, sizeof(detail));
            if (!cls) continue;
            ++counts[r][cls[0] == 'H' ? (cls[1] == 'O' ? 0 : 3) : cls[0] == 'B' ? 1 : 2];
            if (lines < MAX_LINES) {
                fprintf(f, "W %s %X %08X %s %s\n", kScan[r], (unsigned)off, (unsigned)v, cls, detail);
                ++lines;
            }
        }
    }
    /* What the format 4 rules (port_state_ptrs.c) do not cover: decomp globals
     * with words that look like pointers into this binary and that no
     * relocation handles. Mixed data (tiles, positions) shows up here too;
     * read the list for a global that really holds a pointer. */
    if (PortStateVars_Ready()) {
        PortStateReloc* rel = NULL;
        const int nRel = PortStatePtrs_Collect(&rel);
        fprintf(f, "# covered by relocations: %d\n", nRel);
        for (int i = 0; i < PortStateVars_Count(); ++i) {
            const PortStateVar* v = PortStateVars_At(i);
            if (PortStatePtrs_IsExcluded(v->name) || (((uintptr_t)v->ptr) & 3u)) continue;
            const uint32_t* w = (const uint32_t*)v->ptr;
            unsigned uncovered = 0;
            for (uint32_t k = 0; k < v->size / 4u; ++k) {
                char detail[40];
                if (w[k] < 0x100000u || !SsClassify(w[k], spaces, nSpaces, detail, sizeof(detail))) continue;
                bool covered = false;
                for (int j = 0; j < nRel && !covered; ++j)
                    covered = rel[j].wordOff == k * 4u && !strcmp(rel[j].holder, v->name);
                if (!covered) ++uncovered;
            }
            if (uncovered) fprintf(f, "U %s %u\n", v->name, uncovered);
        }
        free(rel);
    }
    for (size_t r = 0; r < sizeof(kScan) / sizeof(kScan[0]); ++r)
        fprintf(f, "# %s: hostarray=%u bracket=%u image=%u heap=%u\n", kScan[r],
                counts[r][0], counts[r][1], counts[r][2], counts[r][3]);
    if (lines >= MAX_LINES) fprintf(f, "# listing truncated at %u lines\n", (unsigned)MAX_LINES);
    fclose(f);
}
#endif /* PORT_DEBUG_TOOLS */

/* ------------------------------------------------------------------------- */

bool Port_SaveState_Available(void) {
    return gMainGameMode == GM_INGAME && gSubGameMode1 == SUB_GAME_MODE_PLAYING;
}

static void SsRequest(PortSaveStateJob job, uint32_t id) {
    if (sJob != PORT_SS_JOB_NONE) return;
    sJob = job;
    sJobId = id;
    sJobDelay = SS_JOB_DELAY_FRAMES;
    sJobWaited = 0;
}

void Port_SaveState_RequestScan(void)               { SsRequest(PORT_SS_JOB_SCAN, 0); }
void Port_SaveState_RequestSaveNew(void)            { SsRequest(PORT_SS_JOB_SAVE, 0); }
void Port_SaveState_RequestOverwrite(uint32_t id)   { SsRequest(PORT_SS_JOB_OVERWRITE, id); }
void Port_SaveState_RequestLoad(uint32_t id)        { SsRequest(PORT_SS_JOB_LOAD, id); }
void Port_SaveState_RequestDelete(uint32_t id)      { SsRequest(PORT_SS_JOB_DELETE, id); }
void Port_SaveState_RequestThumb(uint32_t id)       { SsRequest(PORT_SS_JOB_THUMB, id); }

PortSaveStateJob Port_SaveState_CurrentJob(void) { return sJob; }
bool Port_SaveState_IsBusy(void) { return sJob != PORT_SS_JOB_NONE; }
bool Port_SaveState_Scanned(void) { return sScanned; }

/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Format 4: no host pointers, no dependence on where the build put things.
 *
 *   header (SsHeader; version 4; regionCount/regionSize = the 8 memory regions;
 *           buildFingerprint = SsRomIdentity, the ROM the state belongs to)
 *   the 8 memory regions, raw: EWRAM IWRAM IO BGPAL OBJPAL OAM VRAM SRAM
 *   u32 n, then n x { u16 nameLen, name, u32 size, bytes }
 *       the decomp's globals by name, except those PortStatePtrs_IsExcluded
 *   music: u16 musicTrack, u16 musicTrackOnTransition, u8 priority, u8 0
 *   u32 n, then n relocations (port_state_ptrs.h)
 *   u32 SS4_END_MARK
 *
 * A load puts the bytes back, re-derives the tables of ROM pointers, applies
 * the relocations and restarts the music from the saved track. See
 * docs/3ds-save-states.md for what the decomp's globals have to respect. */

static uint32_t SsRomIdentity(void) {
    return (((uint32_t)gRomRegion + 1u) * 0x01000193u) ^ gRomSize;
}

static bool SsPutU8(FILE* f, uint8_t v)   { return fwrite(&v, 1, 1, f) == 1; }
static bool SsPutU16(FILE* f, uint16_t v) { return fwrite(&v, 2, 1, f) == 1; }
static bool SsPutU32(FILE* f, uint32_t v) { return fwrite(&v, 4, 1, f) == 1; }
static bool SsPutName(FILE* f, const char* s, bool wide) {
    const size_t n = strlen(s);
    return (wide ? SsPutU16(f, (uint16_t)n) : SsPutU8(f, (uint8_t)n)) && fwrite(s, 1, n, f) == n;
}

static bool Ss4WriteBody(FILE* f, const SsRegion* mem) {
    for (int i = 0; i < SS4_MEM_REGIONS; ++i)
        if (fwrite(mem[i].ptr, 1, mem[i].size, f) != mem[i].size) return false;

    uint32_t count = 0;
    const int nVars = PortStateVars_Count();
    for (int i = 0; i < nVars; ++i)
        if (!PortStatePtrs_IsExcluded(PortStateVars_At(i)->name)) ++count;
    if (!SsPutU32(f, count)) return false;
    for (int i = 0; i < nVars; ++i) {
        const PortStateVar* v = PortStateVars_At(i);
        if (PortStatePtrs_IsExcluded(v->name)) continue;
        if (!SsPutName(f, v->name, true) || !SsPutU32(f, v->size) ||
            fwrite(v->ptr, 1, v->size, f) != v->size) return false;
    }

    if (!SsPutU16(f, gMusicInfo.musicTrack) || !SsPutU16(f, gMusicInfo.musicTrackOnTransition) ||
        !SsPutU8(f, gMusicInfo.priority) || !SsPutU8(f, 0)) return false;

    PortStateReloc* rel = NULL;
    const int nRel = PortStatePtrs_Collect(&rel);
    bool ok = SsPutU32(f, (uint32_t)nRel);
    for (int i = 0; i < nRel && ok; ++i) {
        const PortStateReloc* r = &rel[i];
        ok = SsPutU8(f, r->kind) && SsPutName(f, r->holder, false) && SsPutU32(f, r->wordOff);
        if (!ok) break;
        if (r->kind == PSR_SPACE) ok = SsPutU32(f, r->code);
        else if (r->kind == PSR_FUNC) ok = SsPutName(f, r->target, false);
        else ok = SsPutName(f, r->target, false) && SsPutU32(f, r->targetOff);
    }
    free(rel);
    return ok && SsPutU32(f, SS4_END_MARK);
}

/* Bounds-checked reader over a loaded file. */
typedef struct { const unsigned char* p; const unsigned char* end; bool bad; } SsCursor;

static const unsigned char* SsTake(SsCursor* c, size_t n) {
    if (c->bad || (size_t)(c->end - c->p) < n) { c->bad = true; return NULL; }
    const unsigned char* r = c->p;
    c->p += n;
    return r;
}
static uint32_t SsGetU32(SsCursor* c) { uint32_t v = 0; const unsigned char* p = SsTake(c, 4); if (p) memcpy(&v, p, 4); return v; }
static uint16_t SsGetU16(SsCursor* c) { uint16_t v = 0; const unsigned char* p = SsTake(c, 2); if (p) memcpy(&v, p, 2); return v; }
static uint8_t  SsGetU8(SsCursor* c)  { const unsigned char* p = SsTake(c, 1); return p ? *p : 0; }
static void SsGetName(SsCursor* c, bool wide, char* out, size_t n) {
    const size_t len = wide ? SsGetU16(c) : SsGetU8(c);
    const unsigned char* p = SsTake(c, len);
    if (!p || len >= n) { c->bad = true; out[0] = '\0'; return; }
    memcpy(out, p, len);
    out[len] = '\0';
}

/* `f` is positioned after the header. */
static void Ss4Load(FILE* f, const SsHeader* h, uint32_t id) {
    SsRegion mem[SS_REGIONS];
    SsBuildRegions(mem);
    uint32_t memTotal = 0;
    for (int i = 0; i < SS4_MEM_REGIONS; ++i) {
        if (h->regionSize[i] != mem[i].size) { fclose(f); SsSetMsg("ESTADO DE OTRA VERSION"); return; }
        memTotal += mem[i].size;
    }

    /* Read it all before touching live memory: a short file must not leave
     * the machine half restored. */
    const long here = ftell(f);
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, here, SEEK_SET);
    if (here < 0 || size < here) { fclose(f); SsSetMsg("ESTADO CORRUPTO"); return; }
    const size_t bodySize = (size_t)(size - here);
    unsigned char* buf = (unsigned char*)malloc(bodySize);
    if (!buf) { fclose(f); SsSetMsg("SIN MEMORIA"); return; }
    const bool read = fread(buf, 1, bodySize, f) == bodySize;
    fclose(f);
    if (!read) { free(buf); SsSetMsg("ESTADO CORRUPTO"); return; }

    /* Dry run over the variable and relocation blocks: everything has to
     * parse and end in the marker before anything is applied. */
    SsCursor c = { buf, buf + bodySize, false };
    SsTake(&c, memTotal);
    char name[64];
    const uint32_t nVars = SsGetU32(&c);
    for (uint32_t i = 0; i < nVars && !c.bad; ++i) {
        SsGetName(&c, true, name, sizeof(name));
        const uint32_t sz = SsGetU32(&c);
        SsTake(&c, sz);
    }
    SsTake(&c, 6);
    const uint32_t nRel = SsGetU32(&c);
    for (uint32_t i = 0; i < nRel && !c.bad; ++i) {
        const uint8_t kind = SsGetU8(&c);
        SsGetName(&c, false, name, sizeof(name));
        SsGetU32(&c);
        if (kind == PSR_SPACE) SsGetU32(&c);
        else if (kind == PSR_FUNC) SsGetName(&c, false, name, sizeof(name));
        else { SsGetName(&c, false, name, sizeof(name)); SsGetU32(&c); }
    }
    if (c.bad || SsGetU32(&c) != SS4_END_MARK) { free(buf); SsSetMsg("ESTADO CORRUPTO"); return; }

    /* Apply. */
    c = (SsCursor){ buf, buf + bodySize, false };
    for (int i = 0; i < SS4_MEM_REGIONS; ++i)
        memcpy(mem[i].ptr, SsTake(&c, mem[i].size), mem[i].size);

    const uint32_t nVarsApply = SsGetU32(&c);
    for (uint32_t n = 0; n < nVarsApply; ++n) {
        SsGetName(&c, true, name, sizeof(name));
        const uint32_t sz = SsGetU32(&c);
        const unsigned char* data = SsTake(&c, sz);
        const PortStateVar* v = PortStateVars_Find(name);
        if (v && !PortStatePtrs_IsExcluded(name)) memcpy(v->ptr, data, sz < v->size ? sz : v->size);
    }
    const uint16_t musicTrack = SsGetU16(&c);
    const uint16_t musicOnTransition = SsGetU16(&c);
    const uint8_t musicPriority = SsGetU8(&c);
    SsGetU8(&c);

    /* Tables of ROM pointers come from the ROM, then the pointers the state
     * carried go back in. */
    PortGen_All_Init();
    Port_InitConstructorPointers();

    const uint32_t relCount = SsGetU32(&c);
    uint32_t dropped = 0;
    for (uint32_t i = 0; i < relCount; ++i) {
        PortStateReloc r;
        memset(&r, 0, sizeof(r));
        r.kind = SsGetU8(&c);
        SsGetName(&c, false, r.holder, sizeof(r.holder));
        r.wordOff = SsGetU32(&c);
        if (r.kind == PSR_SPACE) r.code = SsGetU32(&c);
        else if (r.kind == PSR_FUNC) SsGetName(&c, false, r.target, sizeof(r.target));
        else { SsGetName(&c, false, r.target, sizeof(r.target)); r.targetOff = SsGetU32(&c); }
        if (!PortStatePtrs_Apply(&r)) ++dropped;
    }
    free(buf);
    (void)dropped;

    /* The audio engine kept its live state; start the saved music again. */
    gMusicInfo.musicTrack = musicTrack;
    gMusicInfo.musicTrackOnTransition = musicOnTransition;
    gMusicInfo.priority = musicPriority;

    Port_GpuRenderer_InvalidateAll();
    PlayCurrentMusicTrack();

    char m[48];
    snprintf(m, sizeof(m), "ESTADO %u CARGADO", SsNumber(id));
    SsSetMsg(m);
}

/* Writes the machine to state file `id`. */
static void SsDoSave(uint32_t id, bool isNew) {
    if (!sScanned) SsScanNow();
    if (isNew) id = SsNextId();

    /* Grab the screenshot first: it is what the player was looking at when
     * they asked for the save. */
    const bool haveThumb = PortStateThumb_Capture(sThumbBuf);

    SsRegion r[SS_REGIONS];
    int n = SsBuildRegions(r);

    char path[160], tmp[168];
    SsPath(path, sizeof(path), id);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) { SsSetMsg("GUARDADO FALLIDO (SD)"); return; }

    /* Format 4 needs the variable table; a binary that was not post-processed
     * (tools/gen_state_vars.py) saves the build-bound format 3. */
    const bool v4 = PortStateVars_Ready();
    if (v4) n = SS4_MEM_REGIONS;

    SsHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = SS_MAGIC;
    h.version = v4 ? SS_VERSION_V5 : SS_VERSION;
    h.regionCount = (uint32_t)n;
    for (int i = 0; i < n; ++i) h.regionSize[i] = r[i].size;
    h.area = gCurrentArea;
    h.room = gCurrentRoom;
    h.frame16 = gFrameCounter16Bit;
    h.buildFingerprint = v4 ? SsRomIdentity() : SsBuildFingerprint();
    h.savedAt = (uint32_t)time(NULL);
    h.energy = gEquipment.currentEnergy;
    h.maxEnergy = gEquipment.maxEnergy;
    h.missiles = gEquipment.currentMissiles;
    h.maxMissiles = gEquipment.maxMissiles;
    h.superMissiles = gEquipment.currentSuperMissiles;
    h.maxSuperMissiles = gEquipment.maxSuperMissiles;
    h.powerBombs = gEquipment.currentPowerBombs;
    h.maxPowerBombs = gEquipment.maxPowerBombs;
    h.igtHours = gInGameTimer.hours;
    h.igtMinutes = gInGameTimer.minutes;
    h.igtSeconds = gInGameTimer.seconds;
    h.igtValid = 1;

    /* Format 3 keeps the header it always had; format 5 has the play time after it. */
    bool ok = fwrite(&h, v4 ? sizeof(h) : SS_HEADER_V4_SIZE, 1, f) == 1;
    if (v4) {
        ok = ok && Ss4WriteBody(f, r);
    } else {
        for (int i = 0; i < n && ok; ++i)
            ok = fwrite(r[i].ptr, 1, r[i].size, f) == r[i].size;
    }
    if (fclose(f) != 0) ok = false;

    /* Written beside the old file and swapped in, so a failed save leaves the
     * state it was going to replace alone. */
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        SsSetMsg("GUARDADO FALLIDO");
        return;
    }

    char tpath[160];
    SsThumbPath(tpath, sizeof(tpath), id);
    const bool thumbOk = haveThumb && SsWriteThumb(id, sThumbBuf);
    if (!thumbOk) remove(tpath);   /* never leave the previous picture next to new data */

    SsEntry e;
    e.id = id;
    SsInfoFromHeader(&h, h.version, &e.info);
    e.info.id = id;
    e.info.hasThumb = thumbOk;
    SsUpsert(&e);

#ifdef PORT_DEBUG_TOOLS
    SsWriteCensus(id);
#endif

    char m[48];
    snprintf(m, sizeof(m), "ESTADO %u GUARDADO", SsNumber(id));
    SsSetMsg(m);
}

static void SsDoLoad(uint32_t id) {
    char path[160];
    SsPath(path, sizeof(path), id);
    FILE* f = fopen(path, "rb");
    if (!f) { SsSetMsg("ESTADO NO ENCONTRADO"); return; }

    SsHeader h;
    const uint32_t version = SsReadHeader(f, &h);
    if (version == SS_VERSION_V4 || version == SS_VERSION_V5) {
        if (!SsHeaderLoadable(&h, version)) { fclose(f); SsSetMsg("ESTADO DE OTRA ROM"); return; }
        Ss4Load(f, &h, id);   /* closes f */
        return;
    }
    if ((version != SS_VERSION && version != SS_VERSION_V2) ||
        h.regionCount != SS_REGIONS) {
        fclose(f);
        SsSetMsg("ESTADO INCOMPATIBLE");
        return;
    }

    /* Reject a slot from any other build: its snapshot is full of absolute
     * host pointers that only resolve against that build's layout (see
     * SsBuildFingerprint). Restoring it anyway is the StopMusicOrSound /
     * UpdateTrack data-abort. */
    if (h.buildFingerprint != SsBuildFingerprint()) {
        fclose(f);
        SsSetMsg("ESTADO DE OTRA BUILD");
        return;
    }

    SsRegion r[SS_REGIONS];
    int n = SsBuildRegions(r);
    uint32_t total = 0;
    for (int i = 0; i < n; ++i) {
        if (h.regionSize[i] != r[i].size) {
            fclose(f);
            SsSetMsg("ESTADO DE OTRA VERSION");
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
    if (!ok) { free(buf); SsSetMsg("ESTADO CORRUPTO"); return; }

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
    snprintf(m, sizeof(m), "ESTADO %u CARGADO", SsNumber(id));
    SsSetMsg(m);
}

static void SsDoDelete(uint32_t id) {
    const unsigned number = SsNumber(id);
    char path[160];
    SsPath(path, sizeof(path), id);
    const bool ok = remove(path) == 0 || !SsFileExists(path);
    SsThumbPath(path, sizeof(path), id);
    remove(path);
    if (ok) SsRemove(id);
    char m[48];
    snprintf(m, sizeof(m), ok ? "ESTADO %u BORRADO" : "BORRADO FALLIDO", number);
    SsSetMsg(m);
}

static void SsDoThumb(uint32_t id) {
    sThumbBufFresh = false;
    if (SsReadThumb(id, sThumbBuf)) {
        sThumbBufId = id;
        sThumbBufFresh = true;
    }
}

const uint16_t* Port_SaveState_TakeThumb(uint32_t* outId) {
    if (!sThumbBufFresh) return NULL;
    sThumbBufFresh = false;
    if (outId) *outId = sThumbBufId;
    return sThumbBuf;
}

void Port_SaveState_ServicePending(void) {
#ifdef PORT_DEBUG_TOOLS
    /* Called at the top of the game's main loop, between frames, in every
     * build. The DEBUG_TOOLS-only work that has to happen there used to sit
     * in src/agbmain.c behind #ifdef PORT_DEBUG_TOOLS, which laid the
     * decompilation out differently in debug and release builds, so a save
     * state from one was refused by the other. It lives here instead. */
    {
        extern void Port_DebugLog(const char* msg);
        extern void PortPpuMzm_DebugApplyPendingWarp(void);
        static u8 sLastGM = 0xFF, sLastSub1 = 0xFF;
        if (gMainGameMode != sLastGM || gSubGameMode1 != sLastSub1) {
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "ModeChange -> GM: 0x%02X, Sub1: 0x%02X", gMainGameMode, gSubGameMode1);
            Port_DebugLog(dbg);
            sLastGM = gMainGameMode;
            sLastSub1 = gSubGameMode1;
        }
        /* Debug warp point (DEBUG -> HERRAMIENTAS), applied between frames
         * rather than from the touch handler, which runs mid-frame. See
         * PortPpuMzm_DebugApplyPendingWarp in port_ppu_mzm.c. */
        PortPpuMzm_DebugApplyPendingWarp();
    }
#endif
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

    if (sJob == PORT_SS_JOB_NONE) return;
    if (sJobDelay > 0) { --sJobDelay; return; }

    /* Saving needs the room to be live (the screenshot and the stats are of
     * it). Loading does not: a state replaces the whole game, so it can start
     * from the title, a menu or a transition. */
    const bool needsGameplay = sJob == PORT_SS_JOB_SAVE || sJob == PORT_SS_JOB_OVERWRITE;
    if (needsGameplay && sReadyFrames < SS_READY_FRAMES) {
        if (++sJobWaited > SS_JOB_GIVE_UP_FRAMES) {
            sJob = PORT_SS_JOB_NONE;
            SsSetMsg("SOLO DURANTE LA PARTIDA");
        }
        return;
    }

    const PortSaveStateJob job = sJob;
    const uint32_t id = sJobId;
    switch (job) {
    case PORT_SS_JOB_SCAN:      SsScanNow(); break;
    case PORT_SS_JOB_SAVE:      SsDoSave(0, true); break;
    case PORT_SS_JOB_OVERWRITE: SsDoSave(id, false); break;
    case PORT_SS_JOB_LOAD:      SsDoLoad(id); break;
    case PORT_SS_JOB_DELETE:    SsDoDelete(id); break;
    case PORT_SS_JOB_THUMB:     SsDoThumb(id); break;
    default: break;
    }
    sJob = PORT_SS_JOB_NONE;
}

/* ------------------------------------------------------------------------- */

int Port_SaveState_Count(void) { return sCount; }

bool Port_SaveState_GetInfoAt(int index, PortSaveStateInfo* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (index < 0 || index >= sCount) return false;
    *out = sEntries[index].info;
    return true;
}

const char* Port_SaveState_LastMessage(void) { return sMsg; }
int Port_SaveState_MessageTtl(void) { return sMsgTtl; }
