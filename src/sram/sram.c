#include "sram/sram.h"

#include "gba.h"
#include "io.h"

static const char sSramVersion[] = "SRAM_V113";

#if defined(TMC_3DS) || defined(PORT_NATIVE)
#include <string.h>
#include "port_gba_mem.h"

void SramWriteUnchecked(u8* src, u8* dest, u32 size)
{
    void* d = port_resolve_write_addr((uintptr_t)dest);
    const void* s = port_resolve_copy_src(src, size);
    if (d && s) memcpy(d, s, size);
}

void SramWrite(u8* src, u8* dest, u32 size)
{
    void* d = port_resolve_write_addr((uintptr_t)dest);
    const void* s = port_resolve_copy_src(src, size);
    if (d && s) memcpy(d, s, size);
}

u8* SramCheck(u8* src, u8* dest, u32 size)
{
    u32 i;
    void* d = port_resolve_write_addr((uintptr_t)dest);
    const void* s = port_resolve_copy_src(src, size);
    if (d && s) {
        u8* d8 = (u8*)d;
        const u8* s8 = (const u8*)s;
        for (i = 0; i < size; i++) {
            if (d8[i] != s8[i]) return dest + i;
        }
    }
    return NULL;
}

u8* SramWriteChecked(u8* src, u8* dest, u32 size)
{
    SramWrite(src, dest, size);
    return SramCheck(src, dest, size);
}

#else

static void SramWriteUncheckedInternal(u8* src, u8* dest, u32 size)
{
    while (size-- != 0)
        *dest++ = *src++;
}

void SramWriteUnchecked(u8* src, u8* dest, u32 size)
{
    u16 code[0x40];
    u16* code_ptr;
    u16* func_ptr;
    u16 csize;
    void* (*func)(u8*, u8*, u32);

    WRITE_16(REG_WAITCNT, READ_16(REG_WAITCNT) & ~WAIT_SRAM_CYCLES_MASK | WAIT_SRAM_8CYCLES);

    func_ptr = (u16*)SramWriteUncheckedInternal;
    func_ptr = (u16*)((u32)func_ptr & ~1);
    code_ptr = code;

    for (csize = ((u32)SramWriteUnchecked - (u32)SramWriteUncheckedInternal) / 2; csize > 0; --csize)
        *code_ptr++ = *func_ptr++;

    func = (void* (*)(u8*, u8*, u32))code + 1;
    func(src, dest, size);
}

void SramWrite(u8* src, u8* dest, u32 size)
{
    u16 w = READ_16(REG_WAITCNT) & ~WAIT_SRAM_CYCLES_MASK | WAIT_SRAM_8CYCLES;
    WRITE_16(REG_WAITCNT, w);

    while (size-- != 0)
        *dest++ = *src++;
}

static u8* SramCheckInternal(u8* src, u8* dest, u32 size)
{
    while (size-- != 0)
    {
        if (*dest++ != *src++)
            return dest - 1;
    }

    return NULL;
}

u8* SramCheck(u8* src, u8* dest, u32 size)
{
    u16 code[0x60];
    u16* code_ptr;
    u16* func_ptr;
    u16 csize;
    void* (*func)(u8*, u8*, u32);

    WRITE_16(REG_WAITCNT, READ_16(REG_WAITCNT) & ~WAIT_SRAM_CYCLES_MASK | WAIT_SRAM_8CYCLES);

    func_ptr = (u16*)SramCheckInternal;
    func_ptr = (u16*)((u32)func_ptr & ~1);
    code_ptr = code;

    for (csize = ((u32)SramCheck - (u32)SramCheckInternal) / 2; csize > 0; --csize)
        *code_ptr++ = *func_ptr++;

    func = (void* (*)(u8*, u8*, u32))code + 1;
    return func(src, dest, size);
}

u8* SramWriteChecked(u8* src, u8* dest, u32 size)
{
    u8* diff;
    u8 i;

#ifdef BUGFIX
    diff = NULL;
#endif // BUGFIX

    for (i = 0; i < 3; ++i)
    {
        SramWrite(src, dest, size);
        diff = SramCheck(src, dest, size);
        if (!diff)
            break;
    }

    return diff;
}
#endif

