/* Standalone sanity check for port_rom.c, not part of any build target.
 * Run manually: gcc -I. -Iinclude -Iport port/port_rom.c
 * port/port_rom_selftest.c -o /tmp/port_rom_selftest && /tmp/port_rom_selftest
 */
#include "port_rom.h"
#include <stdio.h>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "mzm_eu_baserom.gba";

    if (!Port_LoadRom(path)) {
        fprintf(stderr, "FAIL: Port_LoadRom(%s) failed\n", path);
        return 1;
    }
    printf("Loaded %s: region=%s size=%u\n", path, Port_RomRegionLabel(gRomRegion), gRomSize);

    /* sChozodiaEscapeShipHeatingUpPal lives at 0x085dc148 in the EU matching
     * build (mzm_eu.map). Resolve it and print the first few raw bytes;
     * cross-check against `grep sChozodiaEscapeShipHeatingUpPal mzm_eu.map`
     * and the compiled .pal.inc to confirm the bytes line up. */
    u32 knownEuAddr = 0x085dc148;
    void* resolved = Port_ResolveRomData(knownEuAddr);
    if (!resolved) {
        fprintf(stderr, "FAIL: could not resolve 0x%08X\n", knownEuAddr);
        return 1;
    }
    const u8* bytes = (const u8*)resolved;
    printf("Bytes at 0x%08X: %02X %02X %02X %02X %02X %02X %02X %02X\n", knownEuAddr, bytes[0], bytes[1], bytes[2],
           bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);

    if (Port_ResolveRomData(0x07FFFFFF) != NULL) {
        fprintf(stderr, "FAIL: address below ROM range should not resolve\n");
        return 1;
    }
    if (Port_ResolveRomData(0x08000000u + gRomSize) != NULL) {
        fprintf(stderr, "FAIL: address past end of ROM should not resolve\n");
        return 1;
    }

    printf("OK\n");
    return 0;
}
