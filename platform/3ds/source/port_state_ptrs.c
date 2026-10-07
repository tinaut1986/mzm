/* Host pointers in a save state. See docs/3ds-save-states.md.
 *
 * The decomp keeps most of its pointers as GBA addresses, which are the same
 * in every build. The ones that are host pointers (resolved ROM data, the
 * emulated memory arrays, a few functions and globals) are listed here by
 * three rules, applied to the decomp's globals by name:
 *
 *   1. A global that is only pointers (every word is 0 or a pointer into a
 *      space this file knows) is a pointer table: all of it is relocated. This
 *      covers the many `s...Pointers` tables and `p_...` singletons without
 *      naming them.
 *   2. A global that mixes pointers with other data (a sprite, a struct of
 *      physics values) is only scanned if it is in kRules, and only for the
 *      spaces listed there. A scan of an arbitrary struct would take pairs of
 *      16-bit fields for pointers: a position of 0x005F,0x005F is, as a 32-bit
 *      word, an address inside the emulated EWRAM.
 *   3. kNotSaved: the audio engine and the port's own threads and buffers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_state_ptrs.h"
#include "port_state_vars.h"
#include "port_gba_mem.h"

/* ------------------------------------------------------------------------- */
/* Spaces: the host arrays that stand for GBA memory. */

enum { SP_EWRAM, SP_IWRAM, SP_IO, SP_BGPAL, SP_OBJPAL, SP_OAM, SP_VRAM, SP_SRAM, SP_ROM, SP_COUNT };
#define M(s) (1u << (s))
#define M_ALL (M(SP_COUNT) - 1u)

typedef struct { uintptr_t base; uint32_t size; } Space;

static void GetSpaces(Space s[SP_COUNT]) {
    s[SP_EWRAM]  = (Space){ (uintptr_t)gEwram,   (uint32_t)sizeof(gEwram)   };
    s[SP_IWRAM]  = (Space){ (uintptr_t)gIwram,   (uint32_t)sizeof(gIwram)   };
    s[SP_IO]     = (Space){ (uintptr_t)gIoMem,   (uint32_t)sizeof(gIoMem)   };
    s[SP_BGPAL]  = (Space){ (uintptr_t)gBgPltt,  (uint32_t)sizeof(gBgPltt)  };
    s[SP_OBJPAL] = (Space){ (uintptr_t)gObjPltt, (uint32_t)sizeof(gObjPltt) };
    s[SP_OAM]    = (Space){ (uintptr_t)gOamMem,  (uint32_t)sizeof(gOamMem)  };
    s[SP_VRAM]   = (Space){ (uintptr_t)gVram,    (uint32_t)sizeof(gVram)    };
    s[SP_SRAM]   = (Space){ (uintptr_t)gSramMem, (uint32_t)sizeof(gSramMem) };
    s[SP_ROM]    = (Space){ (uintptr_t)gRomData, gRomSize };
}

/* ------------------------------------------------------------------------- */
/* Functions a saved pointer can name. Add a function here if a global can
 * come to hold its address. */

extern void Haze_Bg3(void);
extern void Haze_Bg3StrongWeak(void);
extern void Haze_Bg3NoneWeak(void);
extern void Haze_Bg3Bg2StrongWeakMedium(void);
extern void Haze_Bg3Bg2Bg1(void);
extern void Haze_PowerBombExpanding(void);
extern void Haze_PowerBombRetracting(void);
extern void ClipdataConvertToCollision(void);

typedef struct { const char* name; void (*fn)(void); } FuncEntry;
static const FuncEntry kFuncs[] = {
    { "Haze_Bg3", Haze_Bg3 },
    { "Haze_Bg3StrongWeak", Haze_Bg3StrongWeak },
    { "Haze_Bg3NoneWeak", Haze_Bg3NoneWeak },
    { "Haze_Bg3Bg2StrongWeakMedium", Haze_Bg3Bg2StrongWeakMedium },
    { "Haze_Bg3Bg2Bg1", Haze_Bg3Bg2Bg1 },
    { "Haze_PowerBombExpanding", Haze_PowerBombExpanding },
    { "Haze_PowerBombRetracting", Haze_PowerBombRetracting },
    { "ClipdataConvertToCollision", ClipdataConvertToCollision },
};
#define FUNC_COUNT ((int)(sizeof(kFuncs) / sizeof(kFuncs[0])))

/* ------------------------------------------------------------------------- */
/* Rule 2: globals that mix pointers with other data. */

#define F_VARS 1u    /* also pointers into other decomp globals */

typedef struct { const char* var; unsigned spaces; unsigned flags; } Rule;
static const Rule kRules[] = {
    { "gSpriteData",                    M(SP_ROM), 0 },
    { "gSpriteDebris",                  M(SP_ROM), 0 },
    { "gProjectileData",                M(SP_ROM), 0 },
    { "gSamusPhysics",                  M(SP_ROM), 0 },
    { "gCurrentSprite",                 M(SP_ROM), 0 },
    { "gSubSpriteData1",                M(SP_ROM), 0 },
    { "gCurrentRoomEntry",              M(SP_ROM), 0 },
    { "gBgPointersAndDimensions",       M(SP_EWRAM), 0 },
    { "gTilemapAndClipPointers",        M(SP_EWRAM), 0 },
    { "gHazeInfo",                      M(SP_EWRAM), 0 },
    { "gNonGameplayRam",                M(SP_ROM) | M(SP_EWRAM), F_VARS },
    { "gSaveFilesInfo",                 0, F_VARS },
};
#define RULE_COUNT ((int)(sizeof(kRules) / sizeof(kRules[0])))

/* ------------------------------------------------------------------------- */
/* Rule 3: not saved. They keep their live values; the audio restarts from the
 * saved music id (port_save_state.c). */

static const char* const kNotSaved[] = {
    /* audio engine */
    "gMusicInfo",
    "gTrack0Variables", "gTrack1Variables", "gTrack2Variables", "gTrack3Variables", "gTrack4Variables",
    "gTrack5Variables", "gTrack6Variables", "gTrack7Variables", "gTrack8Variables",
    "gTrackData0", "gTrackData1", "gTrackData2", "gTrackData3", "gTrackData4",
    "gTrackData5", "gTrackData6", "gTrackData7", "gTrackData8",
    "gSoundChannelBackup", "gSoundChannelTrack2Backup", "gSoundQueue",
    "gPsgSounds", "gUnk_300376C",
    "gSoundCodeA", "gSoundCodeB", "gSoundCodeC",
    "gSoundCodeAPointer", "gSoundCodeBPointer", "gSoundCodeCPointer",
    "gInterruptCode", "gVBlankCallback",
    /* the save thread (src/sram/sram.c): handles, a mutex and the snapshot it writes */
    "sSramSnapshot", "sSramDirty", "sSnapshotPending", "sShutdown",
    "sSaveMutex", "sSaveCond", "sSaveThread", "sSaveThreadStarted",
};

bool PortStatePtrs_IsExcluded(const char* var) {
    for (size_t i = 0; i < sizeof(kNotSaved) / sizeof(kNotSaved[0]); ++i)
        if (!strcmp(var, kNotSaved[i])) return true;
    return false;
}

/* ------------------------------------------------------------------------- */

static void CopyName(char* dst, size_t n, const char* src) {
    snprintf(dst, n, "%s", src);
}

/* Classifies `v` and fills `r` (target part only). `spaces` limits the spaces
 * considered; funcs and vars are tried first (exact / interior match). */
static bool Classify(uint32_t v, unsigned spaces, bool vars, bool funcs, const Space sp[SP_COUNT], PortStateReloc* r) {
    if (v == 0) return false;
    if (funcs) {
        for (int i = 0; i < FUNC_COUNT; ++i) {
            if ((uintptr_t)kFuncs[i].fn == v) {
                r->kind = PSR_FUNC;
                CopyName(r->target, sizeof(r->target), kFuncs[i].name);
                return true;
            }
        }
    }
    for (int s = 0; s < SP_COUNT; ++s) {
        if (!(spaces & M(s)) || sp[s].base == 0) continue;
        if (v >= sp[s].base && v < sp[s].base + sp[s].size) {
            r->kind = PSR_SPACE;
            r->code = ((uint32_t)(s + 1) << 24) | (uint32_t)(v - sp[s].base);
            return true;
        }
    }
    if (vars) {
        uint32_t off;
        const PortStateVar* var = PortStateVars_Containing(v, &off);
        if (var) {
            r->kind = PSR_VAR;
            CopyName(r->target, sizeof(r->target), var->name);
            r->targetOff = off;
            return true;
        }
    }
    return false;
}

static int Append(PortStateReloc** arr, int* n, int* cap, const PortStateReloc* r) {
    if (*n == *cap) {
        const int ncap = *cap ? *cap * 2 : 256;
        PortStateReloc* grown = (PortStateReloc*)realloc(*arr, (size_t)ncap * sizeof(PortStateReloc));
        if (!grown) return 0;
        *arr = grown;
        *cap = ncap;
    }
    (*arr)[(*n)++] = *r;
    return 1;
}

static const Rule* FindRule(const char* name) {
    for (int i = 0; i < RULE_COUNT; ++i)
        if (!strcmp(kRules[i].var, name)) return &kRules[i];
    return NULL;
}

int PortStatePtrs_Collect(PortStateReloc** out) {
    *out = NULL;
    Space sp[SP_COUNT];
    GetSpaces(sp);

    PortStateReloc* arr = NULL;
    int n = 0, cap = 0;
    const int count = PortStateVars_Count();
    for (int i = 0; i < count; ++i) {
        const PortStateVar* v = PortStateVars_At(i);
        if (PortStatePtrs_IsExcluded(v->name) || v->size < 4) continue;
        const uint32_t* w = (const uint32_t*)v->ptr;
        const uint32_t words = v->size / 4u;
        const Rule* rule = FindRule(v->name);

        PortStateReloc r;
        if (rule) {
            /* Rule 2: scan the listed spaces only. */
            for (uint32_t k = 0; k < words; ++k) {
                memset(&r, 0, sizeof(r));
                if (Classify(w[k], rule->spaces, (rule->flags & F_VARS) != 0, true, sp, &r)) {
                    CopyName(r.holder, sizeof(r.holder), v->name);
                    r.wordOff = k * 4u;
                    if (!Append(&arr, &n, &cap, &r)) goto done;
                }
            }
            continue;
        }

        /* Rule 1: a pointer table is nothing but zeros and pointers. */
        if ((uintptr_t)v->ptr & 3u) continue;
        bool table = false;
        for (uint32_t k = 0; k < words; ++k) {
            if (w[k] == 0) continue;
            memset(&r, 0, sizeof(r));
            if (!Classify(w[k], M_ALL, true, true, sp, &r)) { table = false; break; }
            table = true;
        }
        if (!table) continue;
        for (uint32_t k = 0; k < words; ++k) {
            memset(&r, 0, sizeof(r));
            if (!Classify(w[k], M_ALL, true, true, sp, &r)) continue;
            CopyName(r.holder, sizeof(r.holder), v->name);
            r.wordOff = k * 4u;
            if (!Append(&arr, &n, &cap, &r)) goto done;
        }
    }
done:
    *out = arr;
    return n;
}

bool PortStatePtrs_Apply(const PortStateReloc* r) {
    const PortStateVar* holder = PortStateVars_Find(r->holder);
    if (!holder || r->wordOff + 4u > holder->size) return false;
    uint32_t* word = (uint32_t*)((char*)holder->ptr + r->wordOff);

    uint32_t value = 0;
    bool ok = false;
    switch (r->kind) {
    case PSR_SPACE: {
        Space sp[SP_COUNT];
        GetSpaces(sp);
        const uint32_t s = (r->code >> 24);
        const uint32_t off = r->code & 0xFFFFFFu;
        if (s >= 1 && s <= SP_COUNT && sp[s - 1].base && off < sp[s - 1].size) {
            value = (uint32_t)(sp[s - 1].base + off);
            ok = true;
        }
        break;
    }
    case PSR_FUNC:
        for (int i = 0; i < FUNC_COUNT; ++i)
            if (!strcmp(kFuncs[i].name, r->target)) { value = (uint32_t)(uintptr_t)kFuncs[i].fn; ok = true; }
        break;
    case PSR_VAR: {
        const PortStateVar* t = PortStateVars_Find(r->target);
        if (t && r->targetOff <= t->size) { value = (uint32_t)((uintptr_t)t->ptr + r->targetOff); ok = true; }
        break;
    }
    default: break;
    }
    *word = ok ? value : 0u;
    return ok;
}
