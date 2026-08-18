/* Regression coverage for the explicit read, write, and copy-source address
 * resolvers used by the native ports (port_resolve_addr/port_resolve_write_addr/
 * port_resolve_copy_src in port_gba_mem.c), including the TMC_3DS-guarded
 * stack-vs-ROM-address disambiguation that caused the SRAM corruption bug in
 * docs/3ds-port-status-2026-08-17.md section 6i: a host stack pointer can
 * land numerically inside the GBA ROM range (0x08000000+), and if that's
 * mistaken for a raw GBA address, it gets translated a second time into
 * garbage.
 *
 * Both real build targets (platform/linux and platform/3ds) always define
 * TMC_3DS, so this test does too -- it stands in for
 * Platform3DS_IsActiveStackAddress() with a test double instead of linking
 * the real one (ARM/3DS-only). */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "port_gba_mem.h"

u8* gRomData;
u32 gRomSize;

/* Test double for the real platform_3ds.c/platform_3ds_minimal.c
 * implementation: reports sFakeStackAddr (if nonzero) as the only address
 * currently "on the stack". */
static uintptr_t sFakeStackAddr;
int Platform3DS_IsActiveStackAddress(uintptr_t value) {
    return sFakeStackAddr != 0 && value == sFakeStackAddr;
}

static int sFailures;

#define CHECK(condition, message)                                                                                 \
    do {                                                                                                          \
        if (!(condition)) {                                                                                       \
            fprintf(stderr, "FAIL: %s\n", message);                                                              \
            sFailures++;                                                                                          \
        }                                                                                                         \
    } while (0)

int main(void) {
    static u8 fakeRom[0x8000];
    u32 stackValue = 0x12345678;

    gRomData = fakeRom;
    gRomSize = sizeof(fakeRom);
    sFakeStackAddr = 0;

    CHECK(port_resolve_addr(0x08000004u) == fakeRom + 4, "ROM address resolves into loaded ROM");
    CHECK(port_resolve_addr((uintptr_t)(fakeRom + 8)) == fakeRom + 8, "native ROM pointer stays native");
    CHECK(port_resolve_addr(0x02000010u) == gEwram + 0x10, "read address resolves into EWRAM");

    CHECK(port_resolve_write_addr(0x06000020u) == gVram + 0x20, "write address resolves into VRAM");
    CHECK(port_resolve_write_addr(0x08000004u) == (void*)(uintptr_t)0x08000004u,
          "ROM-range write destination stays native");

    CHECK(port_resolve_copy_src((const void*)(uintptr_t)0x0800000Cu, 4) == fakeRom + 12,
          "raw ROM copy source resolves into loaded ROM");
    CHECK(port_resolve_copy_src(fakeRom + 16, 4) == fakeRom + 16, "native ROM copy source stays native");
    CHECK(port_resolve_copy_src(&stackValue, sizeof(stackValue)) == &stackValue,
          "ordinary native copy source stays native");

    /* Regression: section 6i. A native stack address that numerically
     * collides with the ROM range must NOT be translated -- if it is, it
     * gets reinterpreted as a ROM offset and corrupts whatever it's copied
     * into (this is exactly how mzm.sav ended up all zeros on hardware). */
    sFakeStackAddr = 0x08007b28u; /* real value observed on hardware */
    CHECK(port_resolve_copy_src((const void*)(uintptr_t)0x08007b28u, 16) == (const void*)(uintptr_t)0x08007b28u,
          "active stack address in ROM numeric range is not re-resolved as ROM (copy src)");
    CHECK(port_resolve_addr(0x08007b28u) == (void*)(uintptr_t)0x08007b28u,
          "active stack address in ROM numeric range is not re-resolved as ROM (read)");
    CHECK(port_resolve_write_addr(0x08007b28u) == (void*)(uintptr_t)0x08007b28u,
          "active stack address in ROM numeric range is not re-resolved as ROM (write)");

    /* Same numeric value, but the stack detector reports nothing active
     * there right now: must resolve as an ordinary ROM address again, same
     * as any other value in range -- the stack check must not misfire once
     * the address stops being a real stack address. */
    sFakeStackAddr = 0;
    CHECK(port_resolve_copy_src((const void*)(uintptr_t)0x08007b28u, 16) == fakeRom + 0x7b28,
          "same numeric value resolves into ROM when it is not an active stack address");

    /* IsWithinRomDataBuffer must win over the stack check: an
     * already-resolved ROM pointer is unambiguous and must never be
     * second-guessed, even if some other unrelated stack frame happens to
     * be flagged nearby. */
    sFakeStackAddr = (uintptr_t)(fakeRom + 4);
    CHECK(port_resolve_addr((uintptr_t)(fakeRom + 4)) == fakeRom + 4,
          "already-resolved ROM pointer stays native regardless of stack flag");
    sFakeStackAddr = 0;

    if (sFailures == 0) {
        puts("port_gba_mem_test: ALL PASS");
        return 0;
    }
    fprintf(stderr, "port_gba_mem_test: %d FAILED\n", sFailures);
    return 1;
}
