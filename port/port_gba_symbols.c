#include "types.h"
#include <stdbool.h>
#include <stdint.h>

/* GBA Linker script symbols for native port */
u32 __nes_metroid_load_address = 0x87D8000;

/* EWRAM Buffers are mapped into gEwram in asm/linker_symbols.s matching GBA layout */


/* IWRAM Symbols */
u16 gInterruptCheckFlag;
void* gIntrCodePointer;
void* gIntrVector;

/* Assembly referenced ROM symbols */
void* const sDma1ControlPointer = (void*)0x040000BA;
const u32 sDma1ControlValue = 0x84000004;
const void* sIntrTable[13] = {0};

#if defined(PLATFORM_LINUX)
void IntrMain(void) {}
bool Platform3DS_IsActiveStackAddress(uintptr_t addr) {
    (void)addr;
    return false;
}
#endif
