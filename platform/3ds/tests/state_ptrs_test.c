/*
 * Save state pointer relocation tests. Host-only (32-bit, like the console), no
 * ROM, no 3DS.
 *
 * A state stores the decomp's host pointers as spaces/offsets/names and puts
 * them back on load. What matters is that the pointers come back right after
 * the ROM image has moved (it is malloc'd, so it moves between runs) and that
 * data which only looks like a pointer is left alone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "port_gba_mem.h"
#include "port_state_ptrs.h"
#include "port_state_vars.h"

/* ---- stand-ins for what the console provides ---- */
u8 gIoMem[0x400];
u8 gEwram[0x40000];
u8 gIwram[0x8000];
u16 gBgPltt[256];
u16 gObjPltt[256];
u16 gOamMem[0x400 / 2];
u8 gVram[0x18000];
u8 gSramMem[0x10000];
u8* gRomData;
u32 gRomSize;

void Port_LogRomAccess(u32 a, const char* c) { (void)a; (void)c; }
void Haze_Bg3(void) {}
void Haze_Bg3StrongWeak(void) {}
void Haze_Bg3NoneWeak(void) {}
void Haze_Bg3Bg2StrongWeakMedium(void) {}
void Haze_Bg3Bg2Bg1(void) {}
void Haze_PowerBombExpanding(void) {}
void Haze_PowerBombRetracting(void) {}
void ClipdataConvertToCollision(void) {}

/* The brackets: the test's own "decomp globals". __ss_data_end is defined by
 * the linker command line (see the Makefile). */
char __ss_data_start[0x400];
char __ss_bss_start[0x400];
extern char __ss_data_end[], __ss_bss_end[];

unsigned char gStateVarBlob[STATE_VAR_BLOB_SIZE];

/* Variables, at fixed offsets in the data bracket. */
enum { OFF_SPRITE = 0x000, OFF_TABLE = 0x100, OFF_FUNC = 0x140, OFF_SCALAR = 0x144, OFF_TARGET = 0x180,
       OFF_BGPTR = 0x1C0, OFF_AUDIO = 0x200 };

static void AddVar(unsigned char** table, char** names, uint32_t* nameBytes, uint32_t* count,
                   const char* name, uint32_t off, uint32_t size) {
    const uint32_t nameOff = *nameBytes;
    memcpy(*names + nameOff, name, strlen(name) + 1);
    *nameBytes += (uint32_t)strlen(name) + 1;
    memcpy(*table, &nameOff, 4);
    memcpy(*table + 4, &off, 4);
    memcpy(*table + 8, &size, 4);
    *table += 12;
    ++*count;
}

static void BuildTable(void) {
    unsigned char tableBuf[12 * 16];
    char nameBuf[512];
    unsigned char* t = tableBuf;
    char* n = nameBuf;
    uint32_t nameBytes = 0, count = 0;
    AddVar(&t, &n, &nameBytes, &count, "gSpriteData", OFF_SPRITE, 0x40);
    AddVar(&t, &n, &nameBytes, &count, "sTablePointers", OFF_TABLE, 0x20);
    AddVar(&t, &n, &nameBytes, &count, "gHazeProcessCodePointer", OFF_FUNC, 4);
    AddVar(&t, &n, &nameBytes, &count, "gLives", OFF_SCALAR, 4);
    AddVar(&t, &n, &nameBytes, &count, "gTarget", OFF_TARGET, 0x20);
    AddVar(&t, &n, &nameBytes, &count, "gBgPointersAndDimensions", OFF_BGPTR, 0x20);
    AddVar(&t, &n, &nameBytes, &count, "gMusicInfo", OFF_AUDIO, 0x40);
    const uint32_t head[4] = { 0x31545653u, count, nameBytes, 0 };
    memcpy(gStateVarBlob, head, 16);
    memcpy(gStateVarBlob + 16, tableBuf, (size_t)(t - tableBuf));
    memcpy(gStateVarBlob + 16 + (t - tableBuf), nameBuf, nameBytes);
}

static int sFailed = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); ++sFailed; } } while (0)

static uint32_t* W(uint32_t off) { return (uint32_t*)(__ss_data_start + off); }

int main(void) {
    BuildTable();
    CHECK(PortStateVars_Ready(), "table parses");
    CHECK(PortStateVars_Count() == 7, "seven variables");
    CHECK(PortStateVars_Find("gSpriteData") != NULL, "find by name");
    CHECK(PortStateVars_Find("nope") == NULL, "missing name");

    /* A ROM image somewhere. */
    gRomSize = 0x100000;
    u8* romA = (u8*)malloc(gRomSize);
    gRomData = romA;

    /* gSpriteData: a ROM pointer at word 6, and positions that look like an
     * address inside EWRAM when read as one 32-bit word. */
    *W(OFF_SPRITE + 24) = (uint32_t)(uintptr_t)(romA + 0x1234);
    *W(OFF_SPRITE + 8) = (uint32_t)(uintptr_t)(gEwram + 0x100);   /* inside EWRAM: NOT a ROM pointer */
    /* A pointer table: ROM, EWRAM, a function, a zero, and a global. */
    *W(OFF_TABLE + 0) = (uint32_t)(uintptr_t)(romA + 0x5000);
    *W(OFF_TABLE + 4) = (uint32_t)(uintptr_t)(gEwram + 0x2000);
    *W(OFF_TABLE + 8) = (uint32_t)(uintptr_t)&Haze_Bg3Bg2Bg1;
    *W(OFF_TABLE + 12) = 0;
    *W(OFF_TABLE + 16) = (uint32_t)(uintptr_t)(__ss_data_start + OFF_TARGET + 8);
    *W(OFF_FUNC) = (uint32_t)(uintptr_t)&Haze_PowerBombExpanding;
    *W(OFF_SCALAR) = 3;                                           /* plain value */
    *W(OFF_BGPTR) = (uint32_t)(uintptr_t)(gEwram + 0x2A800);
    *W(OFF_BGPTR + 4) = 0x00180013u;                              /* dimensions */
    *W(OFF_AUDIO) = (uint32_t)(uintptr_t)(romA + 0x10);           /* excluded variable */

    PortStateReloc* rel = NULL;
    const int n = PortStatePtrs_Collect(&rel);
    int sprite = 0, table = 0, func = 0, bg = 0, audio = 0, scalar = 0;
    for (int i = 0; i < n; ++i) {
        if (!strcmp(rel[i].holder, "gSpriteData")) ++sprite;
        if (!strcmp(rel[i].holder, "sTablePointers")) ++table;
        if (!strcmp(rel[i].holder, "gHazeProcessCodePointer")) ++func;
        if (!strcmp(rel[i].holder, "gBgPointersAndDimensions")) ++bg;
        if (!strcmp(rel[i].holder, "gMusicInfo")) ++audio;
        if (!strcmp(rel[i].holder, "gLives")) ++scalar;
    }
    CHECK(sprite == 1, "gSpriteData: only the ROM pointer, not the EWRAM-looking word");
    CHECK(table == 4, "pointer table: rom, ewram, function, global; the zero is skipped");
    CHECK(func == 1, "a lone function pointer is a table");
    CHECK(bg == 1, "gBgPointersAndDimensions: the EWRAM pointer, not the dimensions");
    CHECK(audio == 0, "excluded variables are not scanned");
    CHECK(scalar == 0, "a plain value is left alone");

    /* The next run: the ROM moved, and everything the state held is gone from
     * memory. Apply must put every pointer back relative to the new image. */
    u8* romB = (u8*)malloc(gRomSize);
    CHECK(romB != romA, "a different image");
    gRomData = romB;
    memset(__ss_data_start, 0, 0x400);
    int applied = 0;
    for (int i = 0; i < n; ++i) applied += PortStatePtrs_Apply(&rel[i]) ? 1 : 0;
    CHECK(applied == n, "every relocation applies");
    CHECK(*W(OFF_SPRITE + 24) == (uint32_t)(uintptr_t)(romB + 0x1234), "rom pointer follows the new image");
    CHECK(*W(OFF_TABLE + 0) == (uint32_t)(uintptr_t)(romB + 0x5000), "table: rom");
    CHECK(*W(OFF_TABLE + 4) == (uint32_t)(uintptr_t)(gEwram + 0x2000), "table: ewram");
    CHECK(*W(OFF_TABLE + 8) == (uint32_t)(uintptr_t)&Haze_Bg3Bg2Bg1, "table: function");
    CHECK(*W(OFF_TABLE + 16) == (uint32_t)(uintptr_t)(__ss_data_start + OFF_TARGET + 8), "table: global + offset");
    CHECK(*W(OFF_FUNC) == (uint32_t)(uintptr_t)&Haze_PowerBombExpanding, "function pointer");
    CHECK(*W(OFF_BGPTR) == (uint32_t)(uintptr_t)(gEwram + 0x2A800), "bg pointer");

    /* A relocation whose target is gone writes null, not a stale pointer. */
    PortStateReloc gone;
    memset(&gone, 0, sizeof(gone));
    gone.kind = PSR_VAR;
    snprintf(gone.holder, sizeof(gone.holder), "sTablePointers");
    snprintf(gone.target, sizeof(gone.target), "gRemovedGlobal");
    *W(OFF_TABLE + 20) = 0x12345678u;
    gone.wordOff = 20;
    CHECK(!PortStatePtrs_Apply(&gone), "missing target is reported");
    CHECK(*W(OFF_TABLE + 20) == 0, "and leaves null");

    free(rel);
    free(romA);
    free(romB);
    printf(sFailed ? "state_ptrs_test: %d FAILED\n" : "state_ptrs_test: all passed\n", sFailed);
    return sFailed ? 1 : 0;
}
