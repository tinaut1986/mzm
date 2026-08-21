/*
 * Minimal PPU->GPU bridge for the mzm 3DS port. Not an adaptation of
 * port_ppu_3ds.c (that file is zelda-tmc-3ds's original, still deeply
 * coupled to TMC-only subsystems -- second screen, HDMA, widescreen, save
 * states, perf HUD -- see its own header comment and
 * docs/3ds-port-status-2026-08-17.md section 8). This is a first cut: bind
 * the port/ppu software renderer (port/ppu/src/{virtuappu,mode1}.c,
 * shared first-party source, not TMC-specific) directly to mzm's emulated
 * GBA memory (port_gba_mem.h's gIoMem/gVram/gBgPltt/gObjPltt/gOamMem) and
 * push the resulting frame to the top screen via platform_gpu_3ds.c (also
 * reused as-is -- it only touches citro2d/citro3d and raw pixel buffers,
 * no TMC coupling). No second-screen content, no HDMA, no widescreen: just
 * enough to get real gameplay visible on real hardware for the first time.
 */
#include "virtuappu.h"
#include "cpu/mode1.h"
#include "port_gba_mem.h"
#include "platform_gpu_3ds.h"

#ifdef PORT_GPU_TILE_RENDERER
#include "port_gpu_renderer.h"
#endif

#ifdef PORT_PPU_PERF_LOG
/* Deliberately not <3ds.h> here -- it redeclares u32/s32/etc. incompatibly
 * with this project's own include/types.h (already pulled in transitively
 * above), which is a hard conflicting-types error. CPU_TICKS_PER_MSEC's
 * value (3ds/os.h: SYSCLOCK_ARM11 / 1000.0, SYSCLOCK_ARM11 = 268111856*2)
 * is a stable libctru constant, safe to inline instead of the header. */
#define PORT_PPU_PERF_CPU_TICKS_PER_MSEC (268111856.0 / 1000.0)
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void Port_DebugLog(const char* msg);

// #define PORT_VERBOSE_FRAME_LOG


/* Must match platform_gpu_3ds.c's TOP_TEXTURE_WIDTH -- the buffer
 * PlatformGpu3DS_TopBuffer() hands back is allocated at that pitch. */
#define TOP_PITCH 512
#define TOP_NATIVE_W 240

static uint32_t* sTopBuffer;
static uint32_t* sTopRightBuffer;
static uint32_t* sBottomBuffer;
static bool sReady;
static unsigned sPresentFrameCount;

/* Logic-side (CPU scanline render) output buffers, decoupled from the real
 * GPU-mapped ones above. The CPU renderer used to write straight into
 * sTopBuffer/sTopRightBuffer -- the exact buffer PlatformGpu3DS_* then
 * uploaded to the GPU -- on the SAME thread and call, so a slow/stalled GPU
 * submission (the still-unresolved bottom-screen bug's neighborhood) stalled
 * the whole agbmain() loop, which stalled audio production with it (see
 * docs/future-roadmap-and-architecture.md Fase 3 and the branch history).
 * Port_PPU_RenderFrame() (game-logic thread) now renders into one of two
 * ping-pong buffers here and publishes it; Port_PPU_GpuPresentPump() (a
 * separate present thread, main_3ds.c) copies the latest published buffer
 * into the real GPU buffer and does the actual GPU submission at its own
 * pace. Lock-free hand-off (generation counter, release/acquire), matching
 * the existing mzm-audio-ring pattern in port_mzm_audio_glue.c rather than
 * introducing the first mutex/LightLock in this tree. Two slots means the
 * present thread must keep up with one full logic-frame period (it does,
 * see port_ppu_mzm.c's Port_PPU_GpuPresentPump doc comment) or it risks
 * reading a slot the logic thread has started overwriting; a third slot
 * would close that theoretical window at the cost of more RAM and isn't
 * worth it unless it's observed in practice. */
#define LOGIC_BUFFER_SLOTS 2
static uint32_t* sLogicTop[LOGIC_BUFFER_SLOTS];
static uint32_t* sLogicTopRight[LOGIC_BUFFER_SLOTS];
static unsigned sLogicWriteSlot;
static volatile unsigned sPublishedGeneration;
static volatile unsigned sPublishedSlot;
static unsigned sConsumedGeneration;

/* platform_gpu_3ds.c calls these (FPS HUD / aspect ratio / display style
 * config, normally backed by a settings menu that doesn't exist yet). */
extern bool Platform3DS_IsNew3DS(void);
extern uint64_t osGetTime(void); /* libctru: milliseconds since epoch */
extern float osGet3DSliderState(void);

/* Rolling 1-second window: frames presented since sFpsWindowStartMs, turned
 * into a rate once the window closes. sPresentFrameCount alone (a lifetime
 * total) isn't a frame rate -- see UpdateFpsWindow(), called once per
 * presented frame below. */
static uint64_t sFpsWindowStartMs;
static unsigned sFpsWindowFrames;
static double sCurrentFps;

static void UpdateFpsWindow(void) {
    const uint64_t now = osGetTime();
    if (sFpsWindowStartMs == 0) {
        sFpsWindowStartMs = now;
        sFpsWindowFrames = 0;
        return;
    }
    ++sFpsWindowFrames;
    const uint64_t elapsed = now - sFpsWindowStartMs;
    if (elapsed >= 1000u) {
        sCurrentFps = (double)sFpsWindowFrames * 1000.0 / (double)elapsed;
        sFpsWindowStartMs = now;
        sFpsWindowFrames = 0;
    }
}

bool Port_Config_GetShowFps(void) { return true; }
int Port_Config_Get3DSAspectRatio(void) { return 0; /* TOP_ASPECT_WIDE */ }
int Port_Config_Get3DSDisplayStyle(void) { return 0; /* TOP_DISPLAY_PIXEL_PERFECT */ }
double Port_PPU_3DS_CurrentFps(void) { return sCurrentFps; }

/* Which path actually rendered the most recently presented frame -- the
 * top-screen "FPS NN" overlay only ever gets drawn by the CPU path
 * (DrawTopImageStereo), so it can't tell you the GPU path's real cadence;
 * this backs the bottom-screen debug overlay in platform_gpu_3ds.c instead,
 * which is drawn regardless of which path rendered. */
static bool sLastFrameUsedGpu;
bool Port_PPU_3DS_LastFrameUsedGpu(void) { return sLastFrameUsedGpu; }

bool Port_PPU_Init(void) {
    VirtuaPPUMode1GbaMemory memory = { gIoMem, gVram, gBgPltt, gObjPltt, gOamMem };
    virtuappu_mode1_bind_gba_memory(&memory);
    virtuappu_mode1_set_old3ds_profile(!Platform3DS_IsNew3DS());

    if (!PlatformGpu3DS_Init(!Platform3DS_IsNew3DS())) return false;
    sTopBuffer = PlatformGpu3DS_TopBuffer();
    sTopRightBuffer = PlatformGpu3DS_TopRightBuffer();
    sBottomBuffer = PlatformGpu3DS_BottomBuffer(0);
    if (!sTopBuffer || !sBottomBuffer) return false;
    /* Bottom screen stays solid black -- PlatformGpu3DS_EndBottom() still
     * needs a non-NULL buffer to finish the frame (C2D_Flush/C3D_FrameEnd),
     * it just skips re-uploading it every frame since changed=false. */
    memset(sBottomBuffer, 0, 512u * 256u * sizeof(uint32_t));

    for (int i = 0; i < LOGIC_BUFFER_SLOTS; ++i) {
        sLogicTop[i] = (uint32_t*)malloc((size_t)TOP_PITCH * 160u * sizeof(uint32_t));
        if (!sLogicTop[i]) return false;
        memset(sLogicTop[i], 0, (size_t)TOP_PITCH * 160u * sizeof(uint32_t));
        if (sTopRightBuffer) {
            sLogicTopRight[i] = (uint32_t*)malloc((size_t)TOP_PITCH * 160u * sizeof(uint32_t));
            if (!sLogicTopRight[i]) return false;
            memset(sLogicTopRight[i], 0, (size_t)TOP_PITCH * 160u * sizeof(uint32_t));
        }
    }
    virtuappu_mode1_set_output_buffer(sLogicTop[0], TOP_PITCH);
    if (sTopRightBuffer) {
        virtuappu_mode1_set_right_output_buffer(sLogicTopRight[0], TOP_PITCH);
    }

#ifdef PORT_GPU_TILE_RENDERER
    /* Experimental (EXTRA_CFLAGS=-DPORT_GPU_TILE_RENDERER, off by default):
     * offloads mode-0 tile/sprite compositing to the PICA200 instead of the
     * CPU scanline renderer. Falls back to the CPU path per-frame whenever
     * Port_GpuRenderer_CanRenderFrame() says the frame uses something it
     * doesn't support yet (affine BG, blend, windows, mosaic, affine OBJ) --
     * see port_gpu_renderer.c. Init failure here just means every frame
     * takes the CPU fallback, not a hard error. */
    if (Port_GpuRenderer_Init()) {
        Port_GpuRenderer_SetActive(true);
    }
#endif

    sReady = true;
    return true;
}

/* Every call site below this point runs from the per-frame present path
 * (PORT_VERBOSE_FRAME_LOG/PORT_PPU_PERF_LOG diagnostics), never a boot/hang
 * checkpoint, so redirect to the buffered logger -- see port_debug_log.h's
 * comment on why the unbuffered version's per-call file I/O is a real cost
 * here (confirmed dominating "upload" time misattribution for most of this
 * session -- docs/3ds-port-gpu-renderer-status-2026-08-20.md section 16). */
extern void Port_DebugLogBuffered(const char* msg);
#define Port_DebugLog Port_DebugLogBuffered

/* Called every VBlankIntrWait() on the game-logic thread (port_bios.c). Does
 * the CPU scanline render only and publishes the result -- see the doc
 * comment on sLogicTop above for why. The experimental GPU tile renderer
 * (PORT_GPU_TILE_RENDERER, off by default) composites straight into the GPU
 * pipeline as its render step, so it can't be deferred to a separate
 * buffer/thread the same way; it keeps the old synchronous inline present,
 * same as PORT_PPU_TEST_PATTERN's direct-to-GPU-buffer diagnostic path. */
void Port_PPU_RenderFrame(void) {
    if (!sReady) return;

    /* Confirmed root cause of a reported bottom-screen debug-overlay
     * duplication (2 copies of the same FPS/stat text stacked vertically,
     * seen both in Azahar and on real hardware): src/transfer.c calls
     * VBlankIntrWait() directly, separate from agbmain's own per-frame
     * Halt loop -- VBlankIntrWait -> Port_Bios_Halt -> this function (see
     * port_bios.c), so a VRAM transfer that spans multiple
     * VBlankIntrWait() calls makes this run more than once per real
     * ~16.67ms display refresh. Each call redraws (and re-presents) the
     * bottom-screen overlay from scratch -- on New3DS in particular,
     * PlatformGpu3DS_EndBottom's own "skip redraw" branch only exists for
     * the Old3DS profile, so a re-entrant call there was never skipped.
     * A real display refresh is ~16.67ms apart; anything under half of
     * that (8ms) between two calls cannot be a new real frame, so skip
     * presenting again entirely rather than redrawing (and thus visually
     * duplicating) the same content. This was previously just logged as a
     * "re-entrant?" diagnostic without actually being fixed -- see
     * docs/3ds-port-gpu-renderer-status-2026-08-20.md section 18. */
    {
        static uint64_t sLastPresentMs;
        uint64_t nowMs = osGetTime();
        if (sLastPresentMs != 0 && (nowMs - sLastPresentMs) < 8u) {
#ifdef PORT_GPU_RENDERER_DIAG_LOG
            char msg[64];
            __builtin_snprintf(msg, sizeof(msg), "PRESENT gap=%lums (skipped, re-entrant)",
                               (unsigned long)(nowMs - sLastPresentMs));
            Port_DebugLog(msg);
#endif
            return;
        }
        sLastPresentMs = nowMs;
    }

    const uint16_t dispcnt = (uint16_t)(gIoMem[0] | (gIoMem[1] << 8));
    const uint8_t gbaMode = (uint8_t)(dispcnt & 7);

    PPUMemory ppu;
    ppu.frame_width = TOP_NATIVE_W;
    ppu.frame_pitch = TOP_PITCH;
    /* virtuappu internal mode: 1 = tiled (GBA mode 0), 2 = affine (GBA
     * modes 1/2) -- see port/ppu/src/virtuappu.c's virtuappu_render_frame.
     * Zero Mission only ever uses GBA modes 0 and 1 (docs/3ds-port-ppu-audit.md). */
    ppu.mode = (gbaMode == 1 || gbaMode == 2) ? 2 : 1;

    float slider3d = PlatformGpu3DS_Get3DSlider();
    virtuappu_mode1_set_3d_slider(slider3d);

#ifdef PORT_GPU_TILE_RENDERER
    const bool useGpuRenderer = Port_GpuRenderer_IsActive() && Port_GpuRenderer_CanRenderFrame();
#else
    const bool useGpuRenderer = false;
#endif

#ifdef PORT_PPU_TEST_PATTERN
    /* Diagnostic: bypass virtuappu entirely and write a known-good pattern
     * (solid alternating 8px-tall horizontal bars) straight into the output
     * buffer. If this still shows up sheared/diagonal on real hardware, the
     * bug is in the GPU presentation pipeline (platform_gpu_3ds.c) or the
     * buffer geometry (TOP_PITCH/width) below, not in virtuappu/mode1.c.
     * Mutually exclusive with the GPU tile renderer -- both are diagnostic/
     * experimental paths, never built together. */
    for (int row = 0; row < 160; ++row) {
        uint32_t color = ((row / 8) % 2 == 0) ? 0xFFFFFFFFu : 0xFF000000u;
        uint32_t* r = sTopBuffer + (size_t)row * TOP_PITCH;
        for (int col = 0; col < TOP_NATIVE_W; ++col) r[col] = color;
    }
    /* Writes sTopBuffer (the real GPU buffer) directly, bypassing the
     * logic-side ping-pong buffers -- keep the old synchronous inline
     * present below for this diagnostic path. */
    const bool forceSyncPresent = true;
#else
    const bool forceSyncPresent = false;
    if (!useGpuRenderer) {
        virtuappu_mode1_set_output_buffer(sLogicTop[sLogicWriteSlot], TOP_PITCH);
        if (sTopRightBuffer) {
            virtuappu_mode1_set_right_output_buffer(sLogicTopRight[sLogicWriteSlot], TOP_PITCH);
        }
        virtuappu_mode1_set_frame_geometry(&ppu);
        virtuappu_mode1_render_frame(&ppu);
    }
#endif

    /* Log the first few frames unconditionally, then only when DISPCNT
     * actually changes (the interesting boot-sequence transitions --
     * force-blank lifting, mode/BG-enable changes) or every ~5s as a
     * liveness heartbeat, capped so the log can't grow unbounded if the
     * game ends up toggling DISPCNT every frame during normal gameplay.
     * Gated behind PORT_VERBOSE_FRAME_LOG -- even throttled, each triggered
     * dump is 8 flushed file writes, a real cost once boot isn't the thing
     * being debugged anymore. */
#ifdef PORT_VERBOSE_FRAME_LOG
    static uint16_t sLastDispcnt = 0xFFFF;
    static unsigned sInGameDumps;
    static unsigned sBootDumps;
    const bool dispcntChanged = dispcnt != sLastDispcnt;
    sLastDispcnt = dispcnt;
    extern uint8_t gSubGameMode1;
    extern uint8_t gMainGameMode;
    const bool isIngame = (gMainGameMode == 4);
    const bool shouldLog = (isIngame && sInGameDumps < 20 && (sPresentFrameCount % 10 == 0 || dispcntChanged)) ||
                           (!isIngame && (sPresentFrameCount < 10 || dispcntChanged) && sBootDumps < 20);
    if (shouldLog) {
        if (isIngame) ++sInGameDumps; else ++sBootDumps;
        char msg[256];
        const uint16_t bg0cnt = (uint16_t)(gIoMem[0x08] | (gIoMem[0x09] << 8));
        const uint16_t bg1cnt = (uint16_t)(gIoMem[0x0A] | (gIoMem[0x0B] << 8));
        const uint16_t bg2cnt = (uint16_t)(gIoMem[0x0C] | (gIoMem[0x0D] << 8));
        const uint16_t bg3cnt = (uint16_t)(gIoMem[0x0E] | (gIoMem[0x0F] << 8));
        const uint16_t winin = (uint16_t)(gIoMem[0x48] | (gIoMem[0x49] << 8));
        const uint16_t winout = (uint16_t)(gIoMem[0x4A] | (gIoMem[0x4B] << 8));
        const uint16_t win1h = (uint16_t)(gIoMem[0x42] | (gIoMem[0x43] << 8));
        const uint16_t win1v = (uint16_t)(gIoMem[0x46] | (gIoMem[0x47] << 8));
        const uint16_t bldcnt = (uint16_t)(gIoMem[0x50] | (gIoMem[0x51] << 8));
        const uint16_t bldalpha = (uint16_t)(gIoMem[0x52] | (gIoMem[0x53] << 8));
        __builtin_snprintf(msg, sizeof(msg),
            "PPU[%u]: mode=%u/%u dispcnt=%04x bg0-3cnt=%04x,%04x,%04x,%04x winin/out=%04x/%04x win1=%04x,%04x bld=%04x/%04x px[80,120]=%08lx",
            sPresentFrameCount, gMainGameMode, gSubGameMode1, dispcnt,
            bg0cnt, bg1cnt, bg2cnt, bg3cnt, winin, winout, win1h, win1v, bldcnt, bldalpha,
            (unsigned long)(forceSyncPresent ? sTopBuffer : sLogicTop[sLogicWriteSlot])[(size_t)80 * TOP_PITCH + 120]);
        Port_DebugLog(msg);
    }
#endif
    ++sPresentFrameCount;
    UpdateFpsWindow();

    if (!useGpuRenderer && !forceSyncPresent) {
        /* Normal (default-build) path: hand the finished CPU render off to
         * the present thread instead of pushing it to the GPU inline here --
         * see the doc comment on sLogicTop/Port_PPU_RenderFrame above. */
        sPublishedSlot = sLogicWriteSlot;
        __atomic_store_n(&sPublishedGeneration, sPublishedGeneration + 1, __ATOMIC_RELEASE);
        sLogicWriteSlot ^= 1u;
        return;
    }

#if defined(PORT_VERBOSE_FRAME_LOG)
    Port_DebugLog("Port_PPU_PresentFrame: before BeginTop");
#endif
    /* This is the sync path's own direct GPU submission, called from the
     * game-logic thread -- must not run concurrently with the present
     * thread's own submission in Port_PPU_GpuPresentPump below. See
     * platform_gpu_3ds.c's sGpuSubmitLock doc comment. */
    PlatformGpu3DS_SubmitLock_Acquire();
#ifdef PORT_GPU_TILE_RENDERER
    if (useGpuRenderer) {
        if (PlatformGpu3DS_BeginTopSceneGpu()) {
            Port_GpuRenderer_RenderFrame();
            sLastFrameUsedGpu = true;
        } else {
            /* Frame-begin failed (GPU busy/queue full) -- nothing was drawn
             * this frame; EndBottom below no-ops on !sFrameActive, so this
             * frame is silently skipped rather than shown corrupt. */
            sLastFrameUsedGpu = false;
        }
    } else
#endif
    {
        PlatformGpu3DS_BeginTopStereo(sTopBuffer, sTopRightBuffer, TOP_NATIVE_W);
#ifdef PORT_GPU_TILE_RENDERER
        sLastFrameUsedGpu = false;
#endif
    }
#if defined(PORT_VERBOSE_FRAME_LOG)
    Port_DebugLog("Port_PPU_PresentFrame: before EndBottom");
#endif
    PlatformGpu3DS_EndBottom(sBottomBuffer, true);
    PlatformGpu3DS_SubmitLock_Release();
#if defined(PORT_VERBOSE_FRAME_LOG)
    Port_DebugLog("Port_PPU_PresentFrame: done");
#endif

#ifdef PORT_PPU_PERF_LOG
    /* Every ~2s (at the target 60Hz): where is frame time actually going?
     * mode1's main/worker ticks cover the CPU scanline render (per-thread,
     * so on New3DS 3 threads run it in parallel -- main plus 2 workers, one
     * of which shares Core 1 with the audio thread); GPU drawing/processing
     * times come from citro3d's own counters and cover texture upload +
     * compositing on the PICA200. Adding all three CPU-side numbers doesn't
     * give total frame time (they overlap across threads); compare each one
     * individually against the 16.67ms/frame budget for 60 FPS. */
    if ((sPresentFrameCount % 120u) == 0u) {
        VirtuaPPUMode13DSStats mode1Stats;
        virtuappu_mode1_get_3ds_stats(&mode1Stats);
        PlatformGpu3DSStats gpuStats;
        PlatformGpu3DS_GetStats(&gpuStats);
        char msg[220];
        __builtin_snprintf(msg, sizeof(msg),
            "PERF[%u]: fps=%.1f main=%.2fms w0=%.2fms w1=%.2fms(workers=%lu) gpuDraw=%.2fms gpuProc=%.2fms",
            sPresentFrameCount, sCurrentFps,
            (double)mode1Stats.mainLastTicks / PORT_PPU_PERF_CPU_TICKS_PER_MSEC,
            (double)mode1Stats.workerLastTicks[0] / PORT_PPU_PERF_CPU_TICKS_PER_MSEC,
            (double)mode1Stats.workerLastTicks[1] / PORT_PPU_PERF_CPU_TICKS_PER_MSEC,
            (unsigned long)mode1Stats.workerCount,
            (double)gpuStats.drawingTime, (double)gpuStats.processingTime);
        Port_DebugLog(msg);
    }
#endif
}

/* Present thread (main_3ds.c) entry point: consumes the latest frame
 * Port_PPU_RenderFrame() published (if any) and does the actual GPU
 * submission -- the part that can legitimately block/stall on a busy or
 * buggy GPU pipeline (still-unresolved bottom-screen duplication included)
 * without that stall reaching back into game logic or audio production.
 * Returns false when there was nothing new to present, so the caller can
 * fall back to waiting for the next real display refresh instead of
 * spinning. Not reentrant-guarded like the old inline path needed to be --
 * a generation counter can't be observed twice, so there is nothing to
 * de-duplicate here. */
bool Port_PPU_GpuPresentPump(void) {
    if (!sReady) return false;
    const unsigned generation = __atomic_load_n(&sPublishedGeneration, __ATOMIC_ACQUIRE);
    if (generation == sConsumedGeneration) return false;
    sConsumedGeneration = generation;
    const unsigned slot = sPublishedSlot;

    memcpy(sTopBuffer, sLogicTop[slot], (size_t)TOP_PITCH * 160u * sizeof(uint32_t));
    if (sTopRightBuffer) {
        memcpy(sTopRightBuffer, sLogicTopRight[slot], (size_t)TOP_PITCH * 160u * sizeof(uint32_t));
    }

#ifdef PORT_GPU_TILE_RENDERER
    sLastFrameUsedGpu = false;
#endif
    /* See platform_gpu_3ds.c's sGpuSubmitLock doc comment -- the game-logic
     * thread can still be mid-submission via the PORT_GPU_TILE_RENDERER/
     * PORT_PPU_TEST_PATTERN sync path in Port_PPU_RenderFrame above. */
    PlatformGpu3DS_SubmitLock_Acquire();
    PlatformGpu3DS_BeginTopStereo(sTopBuffer, sTopRightBuffer, TOP_NATIVE_W);
    PlatformGpu3DS_EndBottom(sBottomBuffer, true);
    PlatformGpu3DS_SubmitLock_Release();
    return true;
}
