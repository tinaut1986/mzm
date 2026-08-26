#ifndef SRAM_SRAM_H
#define SRAM_SRAM_H

#include "types.h"

void SramWriteUnchecked(u8* src, u8* dest, u32 size);
void SramWrite(u8* src, u8* dest, u32 size);
u8* SramCheck(u8* src, u8* dest, u32 size);
u8* SramWriteChecked(u8* src, u8* dest, u32 size);

#if defined(MZM_3DS) || defined(PORT_NATIVE)
void Port_LoadSram(void);
void Port_SaveSram(void);
void Port_FlushSramIfDirty(void);
void Port_FlushSramWait(void);
#endif

#endif /* SRAM_SRAM_H */
