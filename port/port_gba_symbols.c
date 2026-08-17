#include "types.h"
#include <stdbool.h>
#include <stdint.h>

/* GBA Linker script symbols for native port */
u32 __nes_metroid_load_address = 0x87D8000;

/* EWRAM Buffers */
u8 gClipdataCollisionTypes[0x400];
u8 gClipdataCollisionTypes_Tilemap[0xc00];
u8 gClipdataBehaviorTypes[0x800];
u8 gClipdataBehaviorTypes_Tilemap[0x800];
u16 gTilemap[0x1000];
u16 gCommonTilemap[0x800];
u16 gDecompBg3Map[0x800 * 32];
u8 gHazeValues[0xa00];
u8 gPreviousHazeValues[0xa00];
u8 gCurrentCharacterGfx[0x80];
u8 gMakeSolidBlocks[0x80];
u16 gDecompClipdataMap[0x1800];
u16 gDecompBg0Map[0x1800];
u16 gDecompBg1Map[0x1800];
u16 gDecompBg2Map[0x1800];
u8 gMinimapTilesWithObtainedItems[0x800];
u8 gDecompressedMinimapVisitedTiles[0x800];
u8 gDecompressedMinimapData[0xc00];
u8 gUnk_02035400[0x800];
u8 gNeverReformBlocks[0x1000];
u8 gItemsCollected[0x800];
u8 gVisitedMinimapTiles[0xa00];
u8 gMinimapTilesGfx[0x1e0];
u8 gSram[0x7080];
u8 gSramDemoInputData[0x200];
u8 gSramDemoInputDuration[0x200];

/* IWRAM Symbols */
u16 gInterruptCheckFlag;
void* gIntrCodePointer;
void* gIntrVector;

/* Assembly referenced ROM symbols */
void* const sDma1ControlPointer = (void*)0x040000BA;
const u32 sDma1ControlValue = 0x84000004;
const void* sIntrTable[13] = {0};

void IntrMain(void) {}

#if !defined(TMC_3DS) || defined(PLATFORM_LINUX)
bool Platform3DS_IsActiveStackAddress(uintptr_t addr) {
    (void)addr;
    return false;
}
#endif
