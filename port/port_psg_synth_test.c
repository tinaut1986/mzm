/*
 * Unit tests for the software PSG synthesiser (port/port_psg_synth.c).
 *
 * These run on the host with no ROM, no 3DS and no emulator: they write known
 * values into the emulated I/O block and check the synthesiser produces the
 * tone GBATek describes. That matters because every failure mode of this code
 * sounds exactly the same from the outside -- silence -- which is what made
 * the first version of it so hard to judge from a capture: the engine was
 * driving real notes while the output stayed at zero.
 *
 * The noise case below is a direct regression test for that bug: the engine
 * writes the retrigger bit and then clears it from its own copy, so by the
 * time a block renders, the register usually reads back WITHOUT that bit.
 * Loading the envelope only on a retrigger therefore never picked up the
 * level and every voice stayed at volume 0.
 *
 * Build and run with:  cd platform/linux && make test-psg
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The synthesiser reads the emulated I/O block; provide it directly rather
 * than linking the whole translation layer. */
unsigned char gIoMem[0x400];

#include "port_psg_synth.h"

static int sFailures;

static void Check(int condition, const char* what) {
    if (!condition) {
        printf("  FAIL: %s\n", what);
        ++sFailures;
    }
}

static void W8(unsigned int off, unsigned int v) { gIoMem[off] = (unsigned char)v; }
static void W16(unsigned int off, unsigned int v) {
    gIoMem[off] = (unsigned char)(v & 0xFF);
    gIoMem[off + 1] = (unsigned char)((v >> 8) & 0xFF);
}

/* Fresh register block + synth state, then master on, full master volume and
 * every channel panned to both sides unless a test says otherwise. */
static void Begin(void) {
    memset(gIoMem, 0, sizeof(gIoMem));
    PortPsg_Reset();
    W8(0x84, 0x80);     /* SOUNDCNT_X: master enable */
    W8(0x80, 0x77);     /* SOUNDCNT_L: master volume 7 left and right */
    W8(0x81, 0xFF);     /* per-channel panning: all four, both sides */
    W16(0x82, 0x0002);  /* SOUNDCNT_H: PSG ratio 100% */
}

static const unsigned int kRate = 13379; /* the engine's own output rate */

static void PeakOver(int samples, int* peakL, int* peakR) {
    *peakL = *peakR = 0;
    for (int i = 0; i < samples; ++i) {
        int l, r;
        PortPsg_NextSample(&l, &r);
        if (abs(l) > *peakL) *peakL = abs(l);
        if (abs(r) > *peakR) *peakR = abs(r);
    }
}

int main(void) {
    int peakL, peakR;

    /* Square 1: 50% duty, volume 15, freq index 1024 => 131072/(2048-1024)
     * = 128 Hz. Amplitude should be volume * PSG_UNIT_SCALE = 15 * 300. */
    printf("square channel 1 tone\n");
    Begin();
    W16(0x62, 0xF080);              /* duty 50%, initial volume 15, no step */
    W16(0x64, 0x8000 | 1024);       /* frequency + retrigger */
    PortPsg_BeginBlock(kRate);
    {
        int crossings = 0, prev = 0, peak = 0;
        for (unsigned int i = 0; i < kRate; ++i) {
            int l, r;
            PortPsg_NextSample(&l, &r);
            if (abs(l) > peak) peak = abs(l);
            if (i && ((l > 0) != (prev > 0))) ++crossings;
            prev = l;
        }
        const double hz = crossings / 2.0;   /* two sign changes per cycle */
        printf("  peak=%d  measured=%.1f Hz\n", peak, hz);
        Check(fabs(hz - 128.0) <= 2.0, "square 1 frequency should be ~128 Hz");
        Check(peak > 4000 && peak < 5000, "square 1 amplitude should be ~4500");
    }

    printf("volume 0 is silent\n");
    Begin();
    W16(0x62, 0x0080);              /* duty set, volume nibble 0 */
    W16(0x64, 0x8000 | 1024);
    PortPsg_BeginBlock(kRate);
    PeakOver(1000, &peakL, &peakR);
    Check(peakL == 0 && peakR == 0, "a voice at volume 0 must emit nothing");

    printf("master disable silences everything\n");
    Begin();
    W8(0x84, 0x00);                 /* master off */
    W16(0x62, 0xF080);
    W16(0x64, 0x8000 | 1024);
    PortPsg_BeginBlock(kRate);
    PeakOver(1000, &peakL, &peakR);
    Check(peakL == 0 && peakR == 0, "SOUNDCNT_X master enable must be honoured");

    printf("per-channel panning\n");
    Begin();
    W8(0x81, 0x01);                 /* channel 1 enabled on the right only */
    W16(0x62, 0xF080);
    W16(0x64, 0x8000 | 1024);
    PortPsg_BeginBlock(kRate);
    PeakOver(2000, &peakL, &peakR);
    printf("  peakL=%d peakR=%d\n", peakL, peakR);
    Check(peakL == 0, "right-only panning must leave the left channel silent");
    Check(peakR > 0, "right-only panning must still emit on the right");

    /* Regression test for the envelope-load bug: these are the exact register
     * values observed coming from the engine (noise at volume 7, retrigger bit
     * already cleared). This must produce sound. */
    printf("noise voice with the retrigger bit already cleared\n");
    Begin();
    W16(0x78, 0x7000);              /* NR42: initial volume 7, step 0 */
    W16(0x7C, 0x0055);              /* NR43/44: no retrigger bit set */
    PortPsg_BeginBlock(kRate);
    PeakOver(5000, &peakL, &peakR);
    printf("  peak=%d (volume 7 => ~2100)\n", peakL);
    Check(peakL > 1500 && peakL < 2700,
          "noise must load its level from the envelope register, not the retrigger");

    printf("wave channel respects its enable bit\n");
    Begin();
    W16(0x72, 0x2000);              /* NR32: volume code 1 (100%) */
    W16(0x74, 0x8000 | 1024);
    for (unsigned int i = 0; i < 16; ++i) gIoMem[0x90 + i] = (i & 1) ? 0x00 : 0xFF;
    W8(0x70, 0x00);                 /* NR30: channel disabled */
    PortPsg_BeginBlock(kRate);
    PeakOver(2000, &peakL, &peakR);
    Check(peakL == 0, "wave channel must stay silent while NR30 bit 7 is clear");

    W8(0x70, 0x80);                 /* NR30: channel enabled */
    PortPsg_BeginBlock(kRate);
    PeakOver(2000, &peakL, &peakR);
    printf("  enabled peak=%d\n", peakL);
    Check(peakL > 0, "wave channel must emit once enabled");

    if (sFailures == 0) {
        printf("\nAll PSG synthesiser tests passed.\n");
        return 0;
    }
    printf("\n%d PSG synthesiser test(s) FAILED.\n", sFailures);
    return 1;
}
