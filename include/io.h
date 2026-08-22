#ifndef IO_H
#define IO_H

#include "types.h"

#if defined(MZM_3DS) || defined(PORT_NATIVE)
#include "port_gba_mem.h"
static inline u32 port_write32(uintptr_t addr, u32 val) {
    if ((addr >= 0x02000000u && addr < 0x08000000u) || (addr >= 0x0E000000u && addr < 0x0E010000u)) {
        gba_write32((uint32_t)addr, val);
    } else {
        *(volatile u32*)addr = val;
    }
    return val;
}

static inline u16 port_write16(uintptr_t addr, u16 val) {
    if ((addr >= 0x02000000u && addr < 0x08000000u) || (addr >= 0x0E000000u && addr < 0x0E010000u)) {
        gba_write16((uint32_t)addr, val);
    } else {
        *(volatile u16*)addr = val;
    }
    return val;
}

static inline u8 port_write8(uintptr_t addr, u8 val) {
    if ((addr >= 0x02000000u && addr < 0x08000000u) || (addr >= 0x0E000000u && addr < 0x0E010000u)) {
        gba_write8((uint32_t)addr, val);
    } else {
        *(volatile u8*)addr = val;
    }
    return val;
}

static inline u64 port_write64(uintptr_t addr, u64 val) {
    port_write32(addr, (u32)val);
    port_write32(addr + 4, (u32)(val >> 32));
    return val;
}

static inline u32 port_read32(uintptr_t addr) {
    if ((addr >= 0x02000000u && addr < 0x08000000u) || (addr >= 0x0E000000u && addr < 0x0E010000u)) {
        return gba_read32((uint32_t)addr);
    }
    return *(const volatile u32*)addr;
}

static inline u16 port_read16(uintptr_t addr) {
    if ((addr >= 0x02000000u && addr < 0x08000000u) || (addr >= 0x0E000000u && addr < 0x0E010000u)) {
        return gba_read16((uint32_t)addr);
    }
    return *(const volatile u16*)addr;
}

static inline u8 port_read8(uintptr_t addr) {
    if ((addr >= 0x02000000u && addr < 0x08000000u) || (addr >= 0x0E000000u && addr < 0x0E010000u)) {
        return gba_read8((uint32_t)addr);
    }
    return *(const volatile u8*)addr;
}

#define WRITE_64(addr, val) port_write64((uintptr_t)(addr), (u64)(val))
#define WRITE_32(addr, val) port_write32((uintptr_t)(addr), (u32)(val))
#define WRITE_16(addr, val) port_write16((uintptr_t)(addr), (u16)(val))
#define WRITE_8(addr, val)  port_write8((uintptr_t)(addr), (u8)(val))

#define READ_64(addr)       (((u64)port_read32((uintptr_t)(addr) + 4) << 32) | port_read32((uintptr_t)(addr)))
#define READ_32(addr)       port_read32((uintptr_t)(addr))
#define READ_16(addr)       port_read16((uintptr_t)(addr))
#define READ_8(addr)        port_read8((uintptr_t)(addr))
#else
#define WRITE_64(addr, val) (*(vu64 *)(addr)) = (val)
#define WRITE_32(addr, val) (*(vu32 *)(addr)) = (val)
#define WRITE_16(addr, val) (*(vu16 *)(addr)) = (val)
#define WRITE_8(addr, val) (*(vu8 *)(addr)) = (val)

#define READ_64(addr) (*(vu64 *)(addr))
#define READ_32(addr) (*(vu32 *)(addr))
#define READ_16(addr) (*(vu16 *)(addr))
#define READ_8(addr) (*(vu8 *)(addr))
#endif

#endif /* IO_H */

