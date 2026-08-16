/* Standalone unit test for the LZ77 decompressor in port_bios.c (the
 * riskiest routine there -- a subtle bug corrupts every decompressed
 * graphic in-game). Not part of any build target.
 *   gcc -I. -Iinclude -Iport port/port_bios.c port/port_bios_selftest.c \
 *     -o /tmp/t && /tmp/t
 * (host gcc is fine here -- no 3ds.h/libctru dependency exercised by these
 * two functions)
 */
#include "types.h"
#include <stdio.h>
#include <string.h>

void LZ77UncompVram(const void* src, void* dst);

static int check(const char* name, const u8* got, const char* want, u32 len) {
    if (memcmp(got, want, len) != 0) {
        printf("FAIL %s: got \"%.*s\"\n", name, (int)len, got);
        return 1;
    }
    printf("OK   %s\n", name);
    return 0;
}

int main(void) {
    int fails = 0;
    u8 out[64];

    /* All-literal: "HELLO" */
    {
        const u8 stream[] = {0x10, 0x05, 0x00, 0x00, 0x00, 'H', 'E', 'L', 'L', 'O'};
        memset(out, 0, sizeof(out));
        LZ77UncompVram(stream, out);
        fails += check("literal", out, "HELLO", 5);
    }

    /* Overlapping back-reference: "AB" + backref(len=4,dist=2) -> "ABABAB"
     * (classic LZSS self-overlapping copy -- the case most likely to break
     * if copySrc/out don't advance in lockstep). */
    {
        const u8 stream[] = {0x10, 0x06, 0x00, 0x00, 0x20, 'A', 'B', 0x10, 0x01};
        memset(out, 0, sizeof(out));
        LZ77UncompVram(stream, out);
        fails += check("overlap backref", out, "ABABAB", 6);
    }

    /* Minimum-length back-reference: "AB" + backref(len=3,dist=2) ->
     * "AB" + "ABA" = "ABABA" (GBA LZ77's minimum match length is 3, i.e.
     * nibble 0 in the encoded byte). */
    {
        const u8 stream[] = {0x10, 0x05, 0x00, 0x00, 0x20, 'A', 'B', 0x00, 0x01};
        memset(out, 0, sizeof(out));
        LZ77UncompVram(stream, out);
        fails += check("min-length backref", out, "ABABA", 5);
    }

    if (fails) {
        printf("%d test(s) FAILED\n", fails);
        return 1;
    }
    printf("all OK\n");
    return 0;
}
