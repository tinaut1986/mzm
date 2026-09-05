/*
 * Renders samples of an on-device scene recording (mzm-rec-NN.bin) through
 * the port's own CPU PPU, on the host, and writes each frame as a PPM.
 *
 * This is the reference half of the renderer regression harness. The point
 * is NOT to test the CPU PPU -- it is the oracle, not the subject. The
 * subject is the GPU renderer (port_gpu_renderer.c), which cannot run
 * anywhere but a PICA200, so the only way to check it is to render the same
 * recorded PPU state both ways and diff:
 *
 *   1. here, on the host, through mode1.c                  -> reference PPM
 *   2. on the console, SCREEN DUMP at 1:1                  -> mzm-dump-NN-left.rgb
 *   3. tools/compare_render.py                             -> per-pixel diff
 *
 * The two are directly comparable only with the 3D slider at 0 and the
 * display style at PIXEL PERFECT: every stereo eye offset in the GPU
 * renderer is `slider3d * TierPx(tier)`, so at slider 0 they all vanish and
 * both renderers must produce the same flat 240x160 GBA frame. Any
 * difference under those conditions is a GPU renderer bug -- which is
 * exactly the shape of issue #17, where the CPU reconstruction of a frame
 * showed a correct Samus and the console showed a scrambled sprite. That
 * comparison was done by hand, for weeks. This automates it.
 *
 * A screen dump is the better input of the two, and needs no golden images
 * at all: one press writes the GPU output AND the PPU state it was drawn
 * from, at the same instant, sharing one set number. So each dump validates
 * itself -- render its state here, diff against its own -left.rgb, and any
 * disagreement is a GPU renderer bug. Nothing to keep up to date, and no
 * need to reproduce a scene frame-exactly.
 *
 * Build and run:
 *   make -C platform/3ds rec-render
 *   platform/3ds/build/rec_render rec  mzm-rec-05.bin out/      # every sample
 *   platform/3ds/build/rec_render rec  mzm-rec-05.bin out/ 12   # just sample 12
 *   platform/3ds/build/rec_render dump dumps/ 03 out/           # a screen-dump set
 *
 * or let tools/compare_render.py drive the whole comparison.
 */

#include "cpu/mode1.h"
#include "ppu_memory.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* mzm-rec-NN.bin layout -- keep in step with PlatformGpu3DS_RecordTick and
 * the format section of docs/3ds-debug-tools.md. */
#define REC_MAGIC_MZM4 0x344D5A4Du
#define REC_HEADER_BYTES 64u
#define REC_IO_BYTES 0x400u
#define REC_PLTT_BYTES 512u
#define REC_OAM_BYTES 0x400u
#define REC_VRAM_BYTES 0x18000u
#define REC_FIXED_BYTES (REC_HEADER_BYTES + REC_IO_BYTES + 2u * REC_PLTT_BYTES + REC_OAM_BYTES + REC_VRAM_BYTES)

#define GBA_W 240
#define GBA_H 160

static uint32_t Read32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The stride is REC_FIXED_BYTES plus a clip/camera block whose size can
 * change between builds, so derive it from the file instead of hardcoding
 * it: find the second header magic, skipping anything nearer than one
 * sample's fixed part (a word inside VRAM can read as 'MZM4' by chance). */
static size_t DetectStride(const uint8_t* data, size_t size) {
    if (size < REC_FIXED_BYTES || Read32(data) != REC_MAGIC_MZM4) return 0;
    for (size_t off = REC_FIXED_BYTES; off + 4u <= size; off += 4u) {
        if (Read32(data + off) == REC_MAGIC_MZM4) return off;
    }
    return size; /* single-sample file */
}

static bool WritePpm(const char* path, const uint32_t* pixels) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", GBA_W, GBA_H);
    for (int i = 0; i < GBA_W * GBA_H; ++i) {
        /* mode1 renders ABGR8888 (see virtuappu_mode1_rgb555_to_abgr8888). */
        const uint32_t p = pixels[i];
        const uint8_t rgb[3] = { (uint8_t)(p & 0xFF), (uint8_t)((p >> 8) & 0xFF), (uint8_t)((p >> 16) & 0xFF) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

/* Buffers the PPU renders from. File scope so both input modes fill the
 * same ones; copied into rather than pointed at the file image because
 * mode1 wants writable, aligned blocks. */
static uint8_t sIo[REC_IO_BYTES];
static uint8_t sVram[REC_VRAM_BYTES];
static uint16_t sBgPltt[REC_PLTT_BYTES / 2];
static uint16_t sObjPltt[REC_PLTT_BYTES / 2];
static uint16_t sOam[REC_OAM_BYTES / 2];
static uint32_t sFrame[GBA_W * GBA_H];

static void SetupPpu(PPUMemory* ppu) {
    const VirtuaPPUMode1GbaMemory bound = {
        .io_mem = sIo, .vram = sVram, .bg_palette = sBgPltt, .obj_palette = sObjPltt, .oam_mem = sOam,
    };
    virtuappu_mode1_bind_gba_memory(&bound);
    virtuappu_mode1_set_output_buffer(sFrame, GBA_W);
    virtuappu_mode1_set_right_output_buffer(NULL, 0);
    /* Slider 0: the comparison against the GPU renderer is only defined with
     * no stereo separation -- see the header comment. */
    virtuappu_mode1_set_3d_slider(0.0f);
    /* Colour correction off: a display-side preference both renderers apply
     * from the same config, so leaving it out of both keeps the reference a
     * function of the recorded GBA state alone. */
    virtuappu_mode1_set_color_correction(false);
    virtuappu_mode1_set_old3ds_profile(false);

    memset(ppu, 0, sizeof(*ppu));
    ppu->frame_width = GBA_W;
    ppu->frame_pitch = GBA_W;
    ppu->mode = 1;
    virtuappu_mode1_set_frame_geometry(ppu);
}

/* Reads a whole file, requiring EXACTLY the expected size: a short or
 * oversized dump means a mismatched build or a truncated fetch, and
 * rendering it anyway would show up as a pixel "difference" that is really
 * a parsing error. */
static bool ReadExact(const char* path, void* dst, size_t expect) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    const size_t got = fread(dst, 1, expect + 1u, f);
    fclose(f);
    if (got != expect) {
        fprintf(stderr, "%s: expected %zu bytes, got %zu\n", path, expect, got);
        return false;
    }
    return true;
}

/* One screen-dump set: mzm-dump-NN-io.bin and friends, all from one press. */
static int RenderDumpSet(const char* dumpDir, const char* setId, const char* outDir) {
    char path[512];
    const struct { const char* suffix; void* dst; size_t bytes; } parts[] = {
        { "io.bin",      sIo,      REC_IO_BYTES   },
        { "vram.bin",    sVram,    REC_VRAM_BYTES },
        { "bgpltt.bin",  sBgPltt,  REC_PLTT_BYTES },
        { "objpltt.bin", sObjPltt, REC_PLTT_BYTES },
        { "oam.bin",     sOam,     REC_OAM_BYTES  },
    };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        snprintf(path, sizeof(path), "%s/mzm-dump-%s-%s", dumpDir, setId, parts[i].suffix);
        if (!ReadExact(path, parts[i].dst, parts[i].bytes)) return 1;
    }

    PPUMemory ppu;
    SetupPpu(&ppu);
    memset(sFrame, 0, sizeof(sFrame));
    virtuappu_mode1_render_frame(&ppu);

    snprintf(path, sizeof(path), "%s/reference-%s.ppm", outDir, setId);
    if (!WritePpm(path, sFrame)) return 1;
    const uint16_t dispcnt = (uint16_t)(sIo[0] | (sIo[1] << 8));
    printf("dump set %s: DISPCNT=%04X -> %s\n", setId, dispcnt, path);
    virtuappu_mode1_shutdown_workers();
    return 0;
}

static int RenderRecFile(const char* recPath, const char* outDir, long onlySample) {

    FILE* f = fopen(recPath, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", recPath);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    const long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize <= 0) {
        fprintf(stderr, "%s is empty\n", recPath);
        fclose(f);
        return 1;
    }
    uint8_t* data = (uint8_t*)malloc((size_t)fileSize);
    if (!data || fread(data, 1, (size_t)fileSize, f) != (size_t)fileSize) {
        fprintf(stderr, "cannot read %s\n", recPath);
        free(data);
        fclose(f);
        return 1;
    }
    fclose(f);

    const size_t stride = DetectStride(data, (size_t)fileSize);
    if (stride == 0) {
        fprintf(stderr, "%s does not start with an 'MZM4' record header\n", recPath);
        free(data);
        return 1;
    }
    const size_t samples = (size_t)fileSize / stride;
    printf("%s: %zu samples, stride %zu (clip block %zu bytes)\n", recPath, samples, stride,
           stride - REC_FIXED_BYTES);

    PPUMemory ppu;
    SetupPpu(&ppu);

    int written = 0;
    for (size_t i = 0; i < samples; ++i) {
        if (onlySample >= 0 && (size_t)onlySample != i) continue;
        const uint8_t* rec = data + i * stride;
        const uint8_t* p = rec + REC_HEADER_BYTES;
        memcpy(sIo, p, REC_IO_BYTES);           p += REC_IO_BYTES;
        memcpy(sBgPltt, p, REC_PLTT_BYTES);     p += REC_PLTT_BYTES;
        memcpy(sObjPltt, p, REC_PLTT_BYTES);    p += REC_PLTT_BYTES;
        memcpy(sOam, p, REC_OAM_BYTES);         p += REC_OAM_BYTES;
        memcpy(sVram, p, REC_VRAM_BYTES);

        memset(sFrame, 0, sizeof(sFrame));
        virtuappu_mode1_render_frame(&ppu);

        char path[512];
        snprintf(path, sizeof(path), "%s/frame-%04zu.ppm", outDir, i);
        if (!WritePpm(path, sFrame)) {
            free(data);
            return 1;
        }
        ++written;

        const uint32_t frameCounter = Read32(rec + 4);
        const uint16_t dispcnt = (uint16_t)(sIo[0] | (sIo[1] << 8));
        printf("  sample %zu: gba frame %u, DISPCNT=%04X -> %s\n", i, frameCounter, dispcnt, path);
    }

    virtuappu_mode1_shutdown_workers();
    free(data);
    if (written == 0) {
        fprintf(stderr, "no samples rendered (index out of range?)\n");
        return 1;
    }
    printf("%d frame(s) written\n", written);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 5 && strcmp(argv[1], "dump") == 0)
        return RenderDumpSet(argv[2], argv[3], argv[4]);
    if (argc >= 4 && strcmp(argv[1], "rec") == 0)
        return RenderRecFile(argv[2], argv[3], (argc > 4) ? strtol(argv[4], NULL, 10) : -1);
    fprintf(stderr,
            "usage:\n"
            "  %s rec  <mzm-rec-NN.bin> <out-dir> [sample-index]\n"
            "      renders recorded PPU states as <out-dir>/frame-SSSS.ppm\n"
            "  %s dump <dump-dir> <NN> <out-dir>\n"
            "      renders one screen-dump set as <out-dir>/reference-NN.ppm\n",
            argv[0], argv[0]);
    return 2;
}
