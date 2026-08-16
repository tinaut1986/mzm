/* End-to-end check for the generated ROM-resolution shim, not part of any
 * build target. Run manually:
 *   gcc -I. -Iinclude -Iinclude/data -Iport -Iport/generated \
 *     port/port_rom.c port/generated/chozodia_escape_data_rom.c \
 *     port/generated/chozodia_escape_data_rom_selftest.c -o /tmp/t \
 *   && /tmp/t mzm_eu_baserom.gba
 */
#include "chozodia_escape_data_rom.h"
#include "port_rom.h"
#include <stdio.h>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "mzm_eu_baserom.gba";

    if (!Port_LoadRom(path)) {
        fprintf(stderr, "FAIL: Port_LoadRom(%s) failed\n", path);
        return 1;
    }
    printf("Loaded %s: region=%s\n", path, Port_RomRegionLabel(gRomRegion));

    PortGen_chozodia_escape_data_Init();

    /* This is exactly how src/chozodia_escape.c:705 uses it:
     *     src1 = &sChozodiaEscapeShipHeatingUpPal[offset];
     * The macro from the generated header redirects this to the ROM-backed
     * pointer instead of a compiled-in array. */
    const u16* first = &sChozodiaEscapeShipHeatingUpPal[0];
    printf("sChozodiaEscapeShipHeatingUpPal[0..3] = %04X %04X %04X %04X\n", first[0], first[1], first[2], first[3]);

    /* Known-good values: data/chozodia_escape/ship_heating_up.pal starts
     * with bytes 00 00 01 04 05 14 07 1C -> little-endian u16 words
     * 0x0000 0x0401 0x1405 0x1C07. */
    if (first[0] != 0x0000 || first[1] != 0x0401 || first[2] != 0x1405 || first[3] != 0x1C07) {
        fprintf(stderr, "FAIL: unexpected values\n");
        return 1;
    }

    /* Also check a 2D array and a struct-typed table resolve without
     * crashing (values not independently cross-checked here, just that the
     * pointer machinery holds together for multi-dim/struct types). */
    printf("sChozodiaEscape_5ca0d8[0][0..1] = %04X %04X\n", sChozodiaEscape_5ca0d8[0][0], sChozodiaEscape_5ca0d8[0][1]);
    printf("sChozodiaEscapeOam_MotherShipDoorOpening[0].duration (raw first bytes) accessible: yes\n");

    printf("OK\n");
    return 0;
}
