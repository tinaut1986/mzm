/*
 * PPU->GPU bridge for the Metroid: Zero Mission 3DS port. Binds
 * the port/ppu software renderer (port/ppu/src/{virtuappu,mode1}.c)
 * directly to mzm's emulated GBA memory and presents frames to the
 * screen via Citro2D/Citro3D.
 */
#include "virtuappu.h"
#include "cpu/mode1.h"
#include "port_gba_mem.h"
#include "platform_gpu_3ds.h"
#include "structs/samus.h"
#include "constants/samus.h"
#include "samus.h"
#include "gba/memory.h"
#include "structs/bg_clip.h"
#include "constants/block.h"
#include "constants/game_state.h"
#include "structs/game_state.h"
#include "structs/connection.h"
#include "structs/minimap.h"
#include "structs/room.h"
#include "constants/minimap.h"
#include "minimap.h"
#include "macros.h"

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
#include <stdio.h>
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

#include "port_bottom_ui_3ds.h"
#include <stdio.h>

static bool sShowFps = true;
static int sAspectRatio = 0; /* 0 = WIDE, 1 = ORIGINAL, 2 = STRETCH */
static int sDisplayStyle = 0; /* 0 = PIXEL PERFECT, 1 = SCALED, 2 = BLUR */
static const char* const sConfigPath = "mzm3ds.ini";

static bool sAutoHideHud = true;
static bool sHideSpoilers = false;

/* Button Actions:
 * 0 = NINGUNA (NONE)
 * 1 = AUTODISPARO (RAPID FIRE)
 * 2 = MORFOSFERA RAPIDA (QUICK MORPH BALL)
 * 3 = DISPARO (FIRE / B)
 * 4 = SALTAR (JUMP / A)
 * 5 = MISILES (SELECT / swap missile type)
 * 6 = APUNTAR (AIM / DIAGONAL AIM MODIFIER / KEY_L)
 * 7 = ARMAR MISILES (ARM WEAPON / KEY_R)
 * 8 = PAUSA (PAUSE / KEY_START)
 */
#define BTN_ACTION_COUNT 9

/* Remappable button indices:
 * 0=A, 1=B, 2=X, 3=Y, 4=L, 5=R, 6=ZL, 7=ZR, 8=Start, 9=Select
 * D-Pad and Circle Pad are always fixed to movement. */
#define BTN_REMAP_COUNT 10

static int sBtnRemap[BTN_REMAP_COUNT] = {
    4, /* 0: A      -> JUMP (4) */
    3, /* 1: B      -> FIRE (3) */
    1, /* 2: X      -> RAPID FIRE (1) */
    2, /* 3: Y      -> QUICK MORPH (2) */
    6, /* 4: L      -> AIM (6) */
    7, /* 5: R      -> ARM WEAPON (7) */
    6, /* 6: ZL     -> AIM (6) */
    7, /* 7: ZR     -> ARM WEAPON (7) */
    8, /* 8: Start  -> PAUSE (8) */
    5, /* 9: Select -> MISSILES (5) */
};

/* C-Stick mode: 0=OFF, 1=AIM (UP/DOWN), 2=MOVEMENT (LEFT/RIGHT), 3=ALL (4 DIRECTIONS) */
static int sCstickMode = 0;

void Port_Config_Save(void);

void Port_Config_ResetButtonMappingDefault(void) {
    sBtnRemap[0] = 4; /* A      -> JUMP (4) */
    sBtnRemap[1] = 3; /* B      -> FIRE (3) */
    sBtnRemap[2] = 1; /* X      -> RAPID FIRE (1) */
    sBtnRemap[3] = 2; /* Y      -> QUICK MORPH (2) */
    sBtnRemap[4] = 6; /* L      -> AIM (6) */
    sBtnRemap[5] = 7; /* R      -> ARM WEAPON (7) */
    sBtnRemap[6] = 6; /* ZL     -> AIM (6) */
    sBtnRemap[7] = 7; /* ZR     -> ARM WEAPON (7) */
    sBtnRemap[8] = 8; /* Start  -> PAUSE (8) */
    sBtnRemap[9] = 5; /* Select -> MISSILES (5) */
    sCstickMode = 0;  /* OFF */
    Port_Config_Save();
}

void Port_Config_Save(void) {
    FILE* file = fopen(sConfigPath, "wb");
    if (!file) return;
    fprintf(file, "# Metroid Zero Mission 3DS runtime settings\n");
    fprintf(file, "aspect_ratio=%d\n", sAspectRatio);
    fprintf(file, "display_style=%d\n", sDisplayStyle);
    fprintf(file, "show_fps=%u\n", sShowFps ? 1u : 0u);
    fprintf(file, "auto_hide_hud=%u\n", sAutoHideHud ? 1u : 0u);
    fprintf(file, "hide_spoilers=%u\n", sHideSpoilers ? 1u : 0u);
    fprintf(file, "btn_map_a=%d\n", sBtnRemap[0]);
    fprintf(file, "btn_map_b=%d\n", sBtnRemap[1]);
    fprintf(file, "btn_map_x=%d\n", sBtnRemap[2]);
    fprintf(file, "btn_map_y=%d\n", sBtnRemap[3]);
    fprintf(file, "btn_map_l=%d\n", sBtnRemap[4]);
    fprintf(file, "btn_map_r=%d\n", sBtnRemap[5]);
    fprintf(file, "btn_map_zl=%d\n", sBtnRemap[6]);
    fprintf(file, "btn_map_zr=%d\n", sBtnRemap[7]);
    fprintf(file, "btn_map_start=%d\n", sBtnRemap[8]);
    fprintf(file, "btn_map_select=%d\n", sBtnRemap[9]);
    fprintf(file, "cstick_mode=%d\n", sCstickMode);
    fprintf(file, "bottom_tab=%d\n", (int)Port_BottomUI_GetTab());
    fprintf(file, "bottom_zoom=%d\n", Port_BottomUI_GetZoom());
    fprintf(file, "bottom_area=%d\n", Port_BottomUI_GetViewArea());
    fprintf(file, "bottom_follow=%u\n", Port_BottomUI_GetFollowSamus() ? 1u : 0u);
    extern bool Port_RA_IsEnabled(void);
    extern bool Port_RA_IsHardcore(void);
    extern bool Port_RA_GetNotificationSound(void);
    extern const char* Port_RA_GetUsername(void);
    extern const char* Port_RA_GetToken(void);
    fprintf(file, "ra_enabled=%u\n", Port_RA_IsEnabled() ? 1u : 0u);
    fprintf(file, "ra_hardcore=%u\n", Port_RA_IsHardcore() ? 1u : 0u);
    fprintf(file, "ra_sound=%u\n", Port_RA_GetNotificationSound() ? 1u : 0u);
    fprintf(file, "ra_username=%s\n", Port_RA_GetUsername());
    fprintf(file, "ra_token=%s\n", Port_RA_GetToken());
    fclose(file);
}

void Port_Config_Load(void) {
    FILE* file = fopen(sConfigPath, "rb");
    if (!file) return;
    char line[128];
    while (fgets(line, sizeof(line), file) != NULL) {
        char key[64];
        int val = 0;
        if (line[0] == '#') continue;
        if (sscanf(line, " ra_username=%63[^\r\n]", key) == 1) {
            extern void Port_RA_SetUsername(const char*);
            Port_RA_SetUsername(key);
            continue;
        }
        if (sscanf(line, " ra_token=%63[^\r\n]", key) == 1) {
            extern void Port_RA_SetToken(const char*);
            Port_RA_SetToken(key);
            continue;
        }
        if (sscanf(line, " %63[^=]=%d", key, &val) != 2) continue;
        if (strcmp(key, "ra_enabled") == 0) {
            extern void Port_RA_SetEnabled(bool);
            Port_RA_SetEnabled(val != 0);
        } else if (strcmp(key, "ra_hardcore") == 0) {
            extern void Port_RA_SetHardcore(bool);
            /* Hardcore forced to false for now */
            Port_RA_SetHardcore(false);
        } else if (strcmp(key, "ra_sound") == 0) {
            extern void Port_RA_SetNotificationSound(bool);
            Port_RA_SetNotificationSound(val != 0);
        } else if (strcmp(key, "aspect_ratio") == 0) {
            if (val >= 0 && val < 3) sAspectRatio = val;
        } else if (strcmp(key, "display_style") == 0) {
            if (val >= 0 && val < 3) sDisplayStyle = val;
        } else if (strcmp(key, "show_fps") == 0) {
            sShowFps = (val != 0);
        } else if (strcmp(key, "auto_hide_hud") == 0) {
            sAutoHideHud = (val != 0);
        } else if (strcmp(key, "hide_spoilers") == 0) {
            sHideSpoilers = (val != 0);
        } else if (strcmp(key, "btn_map_a") == 0) {
            if (val >= 0 && val < BTN_ACTION_COUNT) sBtnRemap[0] = val;
        } else if (strcmp(key, "btn_map_b") == 0) {
            if (val >= 0 && val < BTN_ACTION_COUNT) sBtnRemap[1] = val;
        } else if (strcmp(key, "btn_map_x") == 0) {
            if (val >= 0 && val < BTN_ACTION_COUNT) sBtnRemap[2] = val;
        } else if (strcmp(key, "btn_map_y") == 0) {
            if (val >= 0 && val < BTN_ACTION_COUNT) sBtnRemap[3] = val;
        } else if (strcmp(key, "btn_map_l") == 0) {
            if (val >= 0 && val < BTN_ACTION_COUNT) sBtnRemap[4] = val;
        } else if (strcmp(key, "btn_map_r") == 0) {
            if (val >= 0 && val < BTN_ACTION_COUNT) sBtnRemap[5] = val;
        } else if (strcmp(key, "btn_map_zl") == 0) {
            if (val >= 0 && val < BTN_ACTION_COUNT) sBtnRemap[6] = val;
        } else if (strcmp(key, "btn_map_zr") == 0) {
            if (val >= 0 && val < BTN_ACTION_COUNT) sBtnRemap[7] = val;
        } else if (strcmp(key, "btn_map_start") == 0) {
            if (val >= 0 && val < BTN_ACTION_COUNT) sBtnRemap[8] = val;
        } else if (strcmp(key, "btn_map_select") == 0) {
            if (val >= 0 && val < BTN_ACTION_COUNT) sBtnRemap[9] = val;
        } else if (strcmp(key, "cstick_mode") == 0) {
            if (val >= 0 && val < 4) sCstickMode = val;
        } else if (strcmp(key, "bottom_tab") == 0) {
            if (val >= 0 && val < BOTTOM_TAB_COUNT) Port_BottomUI_SetTab((PortBottomTab)val);
        } else if (strcmp(key, "bottom_zoom") == 0) {
            Port_BottomUI_SetZoom(val);
        } else if (strcmp(key, "bottom_area") == 0) {
            Port_BottomUI_SetViewArea(val);
        } else if (strcmp(key, "bottom_follow") == 0) {
            Port_BottomUI_SetFollowSamus(val != 0);
        }
    }
    fclose(file);
}

int Port_Config_GetButtonMapping(int buttonIndex) {
    if (buttonIndex >= 0 && buttonIndex < BTN_REMAP_COUNT)
        return sBtnRemap[buttonIndex];
    return 0;
}

void Port_Config_CycleButtonMapping(int buttonIndex) {
    if (buttonIndex < 0 || buttonIndex >= BTN_REMAP_COUNT) return;
    int val = (sBtnRemap[buttonIndex] + 1) % BTN_ACTION_COUNT;
    sBtnRemap[buttonIndex] = val;
    Port_Config_Save();
}

void Port_Config_SetButtonMapping(int buttonIndex, int action) {
    if (buttonIndex < 0 || buttonIndex >= BTN_REMAP_COUNT) return;
    if (action < 0 || action >= BTN_ACTION_COUNT) return;
    sBtnRemap[buttonIndex] = action;
    Port_Config_Save();
}

int Port_Config_GetCstickMode(void) { return sCstickMode; }
void Port_Config_SetCstickMode(int mode) { if (mode >= 0 && mode < 4) { sCstickMode = mode; Port_Config_Save(); } }

const char* Port_Config_GetActionName(int action, int lang) {
    if (lang == 6) {
        switch (action) {
            case 0: return "NINGUNA";
            case 1: return "AUTODISPARO";
            case 2: return "MORFOSFERA RAPIDA";
            case 3: return "DISPARAR (B)";
            case 4: return "SALTAR (A)";
            case 5: return "MISILES (SELECT)";
            case 6: return "APUNTAR (L)";
            case 7: return "ARMAR MISILES (R)";
            case 8: return "PAUSA (START)";
            default: return "NINGUNA";
        }
    }
    switch (action) {
        case 0: return "NONE";
        case 1: return "RAPID FIRE";
        case 2: return "QUICK MORPH";
        case 3: return "FIRE (B)";
        case 4: return "JUMP (A)";
        case 5: return "MISSILES (SELECT)";
        case 6: return "AIM (L)";
        case 7: return "ARM WEAPON (R)";
        case 8: return "PAUSE (START)";
        default: return "NONE";
    }
}

bool Port_Config_GetHideSpoilers(void) { return sHideSpoilers; }
void Port_Config_SetHideSpoilers(bool on) { sHideSpoilers = on; Port_Config_Save(); }
bool Port_Config_GetAutoHideHud(void) { return sAutoHideHud; }
void Port_Config_SetAutoHideHud(bool on) { sAutoHideHud = on; Port_Config_Save(); }

/* Quick Morph (platform_3ds_minimal.c) needs to know Samus's current pose
 * "class" to decide which key to simulate and when it's done:
 *  - entering ball form takes KEY_DOWN TWICE (standing -> crouching on the
 *    first gChangedInput edge, crouching -> morphing on the second -- see
 *    samus.c's SamusCrouching), so the caller must keep sending DOWN edges
 *    until the class becomes MORPHED;
 *  - leaving it takes KEY_UP once to start unmorphing (SamusMorphball's
 *    "Check unmorphing" block), but that only lands Samus in CROUCHING
 *    (SamusUnmorphingGfx hard-sets SPOSE_CROUCHING once the animation ends)
 *    -- a second UP edge is needed from there to actually stand up
 *    (SamusCrouching's own KEY_UP check), so the caller must keep sending UP
 *    edges until the class becomes NORMAL, not just until it leaves MORPHED.
 * A previous version only checked "is Samus in the ball family" and fired a
 * single fixed-length KEY_DOWN pulse, which produces just one edge -- Samus
 * would crouch and get stuck, i.e. behave like a plain Down button. */
int Port_Samus_GetPoseClass(void) {
    switch (gSamusData.pose) {
        case SPOSE_MORPHING:
        case SPOSE_MORPH_BALL:
        case SPOSE_ROLLING:
        case SPOSE_UNMORPHING:
        case SPOSE_MORPH_BALL_MIDAIR:
        case SPOSE_MORPH_BALL_ON_ZIPLINE:
        case SPOSE_PULLING_YOURSELF_INTO_A_MORPH_BALL_TUNNEL:
        case SPOSE_GETTING_HURT_IN_MORPH_BALL:
        case SPOSE_GETTING_KNOCKED_BACK_IN_MORPH_BALL:
            return 2; /* morphed (or mid-transition into/out of ball form) */
        case SPOSE_CROUCHING:
        case SPOSE_UNCROUCHING_FROM_CRAWLING:
        case SPOSE_UNCROUCHING_SUITLESS:
            return 1; /* crouching (or standing back up from it) */
        default:
            return 0; /* normal (standing, walking, jumping, shooting, ...) */
    }
}

/* Per-area, per-tank-type collected counts for the bottom-screen collectibles
 * breakdown (port_bottom_ui_3ds.c's RenderCollectiblesModal). The map's
 * "obtained item" bitmap (gMinimapTilesWithObtainedItems) only says a tile
 * had *something* collected, not what -- BgClipSetItemAsCollected (bg_clip.c)
 * is the thing that actually knows the type, and it logs every pickup (tanks
 * and major items alike) into gItemsCollected[area][0..gNumberOfItemsCollected[area]),
 * so walk that log and tally by type instead. Major items (ITEM_TYPE_ABILITY)
 * are logged too but deliberately not counted here -- this is a tank/ammo
 * breakdown, matching the per-area totals table (sTotalTanksTable) it's
 * compared against. */
void Port_GetAreaItemTypeCounts(int area, int* outEnergy, int* outMissile, int* outSuper, int* outPowerBomb) {
    *outEnergy = 0;
    *outMissile = 0;
    *outSuper = 0;
    *outPowerBomb = 0;
    if (area < 0 || area >= MAX_AMOUNT_OF_AREAS) return;

    int count = gNumberOfItemsCollected[area];
    if (count > MAX_AMOUNT_OF_ITEMS_PER_AREA) count = MAX_AMOUNT_OF_ITEMS_PER_AREA;
    for (int i = 0; i < count; i++) {
        switch (gItemsCollected[area][i].type) {
            case ITEM_TYPE_ENERGY: (*outEnergy)++; break;
            case ITEM_TYPE_MISSILE: (*outMissile)++; break;
            case ITEM_TYPE_SUPER_MISSILE: (*outSuper)++; break;
            case ITEM_TYPE_POWER_BOMB: (*outPowerBomb)++; break;
            default: break;
        }
    }
}

bool Port_Config_GetShowFps(void) { return sShowFps; }
void Port_Config_SetShowFps(bool on) { sShowFps = on; Port_Config_Save(); }

int Port_Config_Get3DSAspectRatio(void) { return sAspectRatio; }
const char* Port_Config_Get3DSAspectRatioName(void) {
    static const char* const names[] = { "WIDE", "ORIGINAL", "STRETCH" };
    return (sAspectRatio >= 0 && sAspectRatio < 3) ? names[sAspectRatio] : "WIDE";
}
void Port_Config_Cycle3DSAspectRatio(void) {
    sAspectRatio = (sAspectRatio + 1) % 3;
    Port_Config_Save();
}

int Port_Config_Get3DSDisplayStyle(void) { return sDisplayStyle; }
const char* Port_Config_Get3DSDisplayStyleName(void) {
    static const char* const names[] = { "PIXEL PERFECT", "SCALED", "BLUR" };
    return (sDisplayStyle >= 0 && sDisplayStyle < 3) ? names[sDisplayStyle] : "PIXEL PERFECT";
}
void Port_Config_Cycle3DSDisplayStyle(void) {
    sDisplayStyle = (sDisplayStyle + 1) % 3;
    Port_Config_Save();
}

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

/**
 * Debug dump of Samus's animation/graphics state, for tracking down bugs
 * like #17 (wrong sprite/palette during the death animation) where the
 * VRAM/OAM/palette dump alone doesn't say which pose/frame/suit produced it.
 * Triggered together with the VRAM/OAM/palette dump from the L+R+X combo.
 */
void PortPpuMzm_DumpSamusState(void) {
    FILE* f = fopen("sdmc:/3ds/mzm-dump-samus.txt", "w");
    if (!f)
        return;

    fprintf(f, "pose=%u\n", (unsigned)gSamusData.pose);
    fprintf(f, "currentAnimationFrame=%u\n", (unsigned)gSamusData.currentAnimationFrame);
    fprintf(f, "walljumpTimer=%u\n", (unsigned)gSamusData.walljumpTimer);
    fprintf(f, "suitType=%u\n", (unsigned)gEquipment.suitType);
    fprintf(f, "suitMiscActivation=%u\n", (unsigned)gEquipment.suitMiscActivation);
    fprintf(f, "shoulderGfxSize=%u\n", (unsigned)gSamusPhysics.shoulderGfxSize);
    fprintf(f, "torsoGfxSize=%u\n", (unsigned)gSamusPhysics.torsoGfxSize);
    fprintf(f, "legsGfxSize=%u\n", (unsigned)gSamusPhysics.legsGfxSize);
    fprintf(f, "bodyLowerHalfGfxSize=%u\n", (unsigned)gSamusPhysics.bodyLowerHalfGfxSize);
    fprintf(f, "armCannonGfxUpperSize=%u\n", (unsigned)gSamusPhysics.armCannonGfxUpperSize);
    fprintf(f, "armCannonGfxLowerSize=%u\n", (unsigned)gSamusPhysics.armCannonGfxLowerSize);
    fprintf(f, "unk_22=%u\n", (unsigned)gSamusPhysics.unk_22);

    fclose(f);

    FILE* fb = fopen("sdmc:/3ds/mzm-dump-samusdata.bin", "wb");
    if (fb) {
        fwrite(&gSamusData, 1, sizeof(gSamusData), fb);
        fclose(fb);
    }

    fb = fopen("sdmc:/3ds/mzm-dump-samusphysics.bin", "wb");
    if (fb) {
        fwrite(&gSamusPhysics, 1, sizeof(gSamusPhysics), fb);
        fclose(fb);
    }
}

/**
 * Debug instant-kill, for iterating on issue #17 (death animation) without
 * having to actually get hit down to 0 energy in real gameplay every time.
 * Mirrors exactly what lethal damage does in src/sprite_util.c's
 * SpriteUtilTakeDamageFromSprite (the `else` branch: zero energy, then
 * request the hurt pose) rather than poking gSamusData.pose directly --
 * SamusSetPose(SPOSE_HURT_REQUEST) is the real entry point that backs up
 * Samus's data into gSamusDataCopy and calls SamusChangeToHurtPose, which
 * itself checks gEquipment.currentEnergy and only then transitions to
 * SPOSE_DYING (src/samus.c). Zeroing energy first reproduces that exact
 * path instead of a synthetic one. Triggered by L+R+SELECT (see
 * Platform3DS_PollKeysIntoGba in platform_3ds_minimal.c, which can't call
 * SamusSetPose directly for the same <3ds.h>/structs-samus.h conflict
 * reason PortPpuMzm_GetSamusRecordState exists). No-op if Samus is already
 * in a hurt/dying/getting-knocked-back pose so mashing the combo doesn't
 * re-trigger mid-animation.
 */
void PortPpuMzm_DebugKillSamus(void) {
    switch (gSamusData.pose) {
        case SPOSE_GETTING_HURT:
        case SPOSE_GETTING_HURT_IN_MORPH_BALL:
        case SPOSE_GETTING_KNOCKED_BACK:
        case SPOSE_GETTING_KNOCKED_BACK_IN_MORPH_BALL:
        case SPOSE_DYING:
            return;
        default:
            break;
    }
    gEquipment.currentEnergy = 0;
    SamusSetPose(SPOSE_HURT_REQUEST);
}

/**
 * Compact Samus state for the L+R+START scene recorder (platform_gpu_3ds.c):
 * that file can't include structs/samus.h directly (its u32 typedef conflicts
 * with <3ds.h>'s, see PortPpuMzm_DumpSamusState's comment above), so it gets
 * the handful of fields worth recording per sample through here instead.
 * `out` must have space for PORT_PPU_MZM_RECORD_STATE_WORDS u32s.
 */
#define PORT_PPU_MZM_RECORD_STATE_WORDS 6
void PortPpuMzm_GetSamusRecordState(uint32_t* out) {
    out[0] = (uint32_t)gSamusData.pose;
    out[1] = (uint32_t)gSamusData.currentAnimationFrame;
    out[2] = (uint32_t)gSamusData.walljumpTimer;
    out[3] = (uint32_t)gEquipment.suitType;
    out[4] = (uint32_t)gEquipment.suitMiscActivation;
    out[5] = (uint32_t)gSamusPhysics.unk_22;
}

/* ---------------------------------------------------------------------
 * Debug warp ("teletransporte")
 *
 * Lives here rather than in port_bottom_ui_3ds.c (which owns the menu that
 * triggers it) for the same reason PortPpuMzm_DebugKillSamus does: that
 * file includes <3ds.h>, whose u32 typedef conflicts with the GBA port's
 * own, so it can't touch gSamusData/gCurrentArea/the door tables directly.
 *
 * Warps are addressed by (area, DOOR id) rather than by (area, room) plus
 * raw coordinates. src/room.c's RoomReset derives EVERYTHING from
 * gLastDoorUsed: gCurrentRoom = pDoor->sourceRoom, and Samus's position
 * from the door's xStart/yEnd/xExit/yExit -- exactly what a normal door
 * transition does. Setting a room number and a guessed x/y instead (what
 * the PORT_LINUX_DIAG_WARP_TO_DEOREM block in src/agbmain.c does, where the
 * coordinates were hand-tuned for one specific room) would drop Samus
 * wherever those coordinates happen to land in the new room's geometry --
 * fine for one known room, useless as a general "jump anywhere" tool.
 * RoomReset recomputes gCurrentRoom from the door regardless, so the room
 * number here is only ever advisory/for display.
 *
 * The point is persisted to sdmc:/3ds/mzm-warp-point.txt so it survives a
 * reflash: the whole reason this exists is iterating on a rendering bug
 * that only shows up in one specific room, where re-walking there after
 * every new CIA install is the actual cost.
 * ------------------------------------------------------------------- */
#define PORT_WARP_POINT_PATH "sdmc:/3ds/mzm-warp-point.txt"

/* Populated by RoomInitDoors (src/room.c). Same extern src/menus/boot_debug.c
 * uses for the original game's own room/door debug menu. */
extern const struct Door* sAreaDoorsPointers[AREA_ENTRY_COUNT];

/* Safety cap for the door-table walk below. No real area comes close, but
 * the table is terminated by a DOOR_TYPE_NONE entry rather than by a count,
 * so an unexpected table must not turn into an unbounded scan. */
#define PORT_WARP_MAX_DOORS 128

static bool sWarpPointValid;
static bool sWarpPending;
static u8 sWarpArea;
static u8 sWarpDoor;
static bool sWarpLoadTried;

/* Selection shown in the menu's manual AREA/DOOR spinner. Starts on
 * Crateria rather than area 0 purely because that's where the room this
 * tool was first needed for lives. */
static u8 sWarpSelArea = AREA_CRATERIA;
static u8 sWarpSelDoor = 0;

int PortPpuMzm_DebugGetDoorCount(int area) {
    if (area < 0 || area >= AREA_ENTRY_COUNT) return 0;
    const struct Door* doors = sAreaDoorsPointers[area];
    if (doors == NULL) return 0;
    int n = 0;
    while (n < PORT_WARP_MAX_DOORS && doors[n].type != DOOR_TYPE_NONE) ++n;
    return n;
}

/* Room a given door leads into, or -1 if the door id is out of range. Lets
 * the menu show "PUERTA 12 -> SALA 8", so a room can be found by stepping
 * doors without knowing door ids up front. */
int PortPpuMzm_DebugGetDoorRoom(int area, int door) {
    if (door < 0 || door >= PortPpuMzm_DebugGetDoorCount(area)) return -1;
    return (int)sAreaDoorsPointers[area][door].sourceRoom;
}

static void PortPpuMzm_WarpPointLoad(void) {
    sWarpLoadTried = true;
    FILE* f = fopen(PORT_WARP_POINT_PATH, "r");
    if (!f) return;
    unsigned a = 0, d = 0;
    if (fscanf(f, "%u %u", &a, &d) == 2 && a < AREA_ENTRY_COUNT) {
        sWarpArea = (u8)a;
        sWarpDoor = (u8)d;
        sWarpPointValid = true;
        sWarpSelArea = sWarpArea;
        sWarpSelDoor = sWarpDoor;
    }
    fclose(f);
}

/* Records the door Samus last came through as the warp target, i.e.
 * "bring me back to this room". */
void PortPpuMzm_DebugSaveWarpPoint(void) {
    sWarpArea = (u8)gCurrentArea;
    sWarpDoor = gLastDoorUsed;
    sWarpPointValid = true;
    sWarpLoadTried = true;
    sWarpSelArea = sWarpArea;
    sWarpSelDoor = sWarpDoor;

    FILE* f = fopen(PORT_WARP_POINT_PATH, "w");
    if (f) {
        fprintf(f, "%u %u\n", sWarpArea, sWarpDoor);
        fclose(f);
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "WARP POINT SAVED: area=%u door=%u room=%u",
             sWarpArea, sWarpDoor, gCurrentRoom);
    Port_DebugLog(msg);
}

bool PortPpuMzm_DebugHasWarpPoint(void) {
    if (!sWarpPointValid && !sWarpLoadTried) PortPpuMzm_WarpPointLoad();
    return sWarpPointValid;
}

void PortPpuMzm_DebugGetWarpPointInfo(char* out, int outSize) {
    if (!out || outSize <= 0) return;
    if (!PortPpuMzm_DebugHasWarpPoint()) {
        snprintf(out, (size_t)outSize, "SIN PUNTO GUARDADO");
        return;
    }
    int room = PortPpuMzm_DebugGetDoorRoom(sWarpArea, sWarpDoor);
    snprintf(out, (size_t)outSize, "AREA %u PUERTA %u -> SALA %d", sWarpArea, sWarpDoor, room);
}

/* Manual (area, door) spinner state, driven by the menu's arrows. */
void PortPpuMzm_DebugGetWarpSelection(int* outArea, int* outDoor, int* outRoom) {
    if (!sWarpLoadTried) PortPpuMzm_WarpPointLoad();
    if (outArea) *outArea = sWarpSelArea;
    if (outDoor) *outDoor = sWarpSelDoor;
    if (outRoom) *outRoom = PortPpuMzm_DebugGetDoorRoom(sWarpSelArea, sWarpSelDoor);
}

void PortPpuMzm_DebugStepWarpArea(int delta) {
    int a = (int)sWarpSelArea + delta;
    if (a < 0) a = AREA_ENTRY_COUNT - 1;
    if (a >= AREA_ENTRY_COUNT) a = 0;
    sWarpSelArea = (u8)a;
    /* A door id valid in the previous area usually isn't in the new one. */
    if (sWarpSelDoor >= PortPpuMzm_DebugGetDoorCount(sWarpSelArea)) sWarpSelDoor = 0;
}

void PortPpuMzm_DebugStepWarpDoor(int delta) {
    int count = PortPpuMzm_DebugGetDoorCount(sWarpSelArea);
    if (count <= 0) { sWarpSelDoor = 0; return; }
    int d = (int)sWarpSelDoor + delta;
    if (d < 0) d = count - 1;
    if (d >= count) d = 0;
    sWarpSelDoor = (u8)d;
}

/* Requests a warp; the jump itself happens in src/agbmain.c's loop. */
static bool PortPpuMzm_RequestWarpTo(u8 area, u8 door) {
    if (PortPpuMzm_DebugGetDoorRoom(area, door) < 0) return false;
    sWarpArea = area;
    sWarpDoor = door;
    sWarpPending = true;
    return true;
}

bool PortPpuMzm_DebugRequestWarp(void) {
    if (!PortPpuMzm_DebugHasWarpPoint()) return false;
    return PortPpuMzm_RequestWarpTo(sWarpArea, sWarpDoor);
}

bool PortPpuMzm_DebugRequestWarpToSelection(void) {
    return PortPpuMzm_RequestWarpTo(sWarpSelArea, sWarpSelDoor);
}

/* Called once per main-loop iteration from src/agbmain.c. Only fires while
 * really in gameplay (GM_INGAME / SUB_GAME_MODE_PLAYING): applying it during
 * a menu, a cutscene or a door transition would fight whatever state machine
 * owns gSubGameMode1 at that moment. The request stays pending until
 * gameplay is reached, so triggering it from the pause screen warps as soon
 * as the game resumes instead of being dropped. */
void PortPpuMzm_DebugApplyPendingWarp(void) {
    if (!sWarpPending) return;
    if (gMainGameMode != GM_INGAME || gSubGameMode1 != SUB_GAME_MODE_PLAYING) return;

    gCurrentArea = (Area)sWarpArea;
    gLastDoorUsed = sWarpDoor;
    /* Advisory only -- RoomReset overwrites this from the door -- but it
     * keeps anything reading gCurrentRoom before then consistent. */
    gCurrentRoom = sAreaDoorsPointers[sWarpArea][sWarpDoor].sourceRoom;
    gSubGameMode1 = 0; /* re-enter InitAndLoadGenerics -> real RoomLoad */
    sWarpPending = false;

    char msg[96];
    snprintf(msg, sizeof(msg), "WARP APPLIED: area=%u door=%u room=%u",
             sWarpArea, sWarpDoor, gCurrentRoom);
    Port_DebugLog(msg);
}


/* ---------------------------------------------------------------------
 * Debug: warp to a minimap tile, reveal every map, edit equipment.
 *
 * Same placement rationale as the warp block above: everything here needs
 * the GBA-side structs that port_bottom_ui_3ds.c (which owns the menus)
 * cannot include alongside <3ds.h>.
 * ------------------------------------------------------------------- */

/* Room table, populated by RoomInitRoomEntries (src/room.c). Needed to turn
 * a door into the minimap tile it sits on. */
extern const struct RoomEntryRom* sAreaRoomEntryPointers[AREA_ENTRY_COUNT];

/* Minimap tile a door occupies. Same arithmetic as
 * BootDebugUpdateMapScreenPosition (src/menus/boot_debug.c), which is the
 * game's own "where on the area map is this door" code: the room's map
 * origin plus the door's offset within the room, in screens. The constant
 * 2-block bias is that function's `xOffset`/`yOffset` default -- doors sit
 * two blocks inside the screen edge. */
static bool PortPpuMzm_DoorMapTile(int area, int door, int* outX, int* outY) {
    if (PortPpuMzm_DebugGetDoorRoom(area, door) < 0) return false;
    const struct Door* pDoor = &sAreaDoorsPointers[area][door];
    const struct RoomEntryRom* pRoom = &sAreaRoomEntryPointers[area][pDoor->sourceRoom];
    *outX = (int)pRoom->mapX + ((int)pDoor->xStart - 2) / SCREEN_SIZE_X_BLOCKS;
    *outY = (int)pRoom->mapY + ((int)pDoor->yStart - 2) / SCREEN_SIZE_Y_BLOCKS;
    return true;
}

/* Door whose map tile is nearest to (tileX, tileY), or -1 if the area has
 * none within `maxDist` tiles. Nearest-match rather than exact because a
 * room can span several map tiles while its doors only sit on the tiles at
 * its edges -- tapping the middle of a big room should still work. */
int PortPpuMzm_DebugFindDoorNearMapTile(int area, int tileX, int tileY, int maxDist) {
    int best = -1;
    int bestDist = maxDist * maxDist + 1;
    int count = PortPpuMzm_DebugGetDoorCount(area);
    for (int d = 0; d < count; ++d) {
        int dx, dy;
        if (!PortPpuMzm_DoorMapTile(area, d, &dx, &dy)) continue;
        int ddx = dx - tileX;
        int ddy = dy - tileY;
        int dist = ddx * ddx + ddy * ddy;
        if (dist < bestDist) {
            bestDist = dist;
            best = d;
        }
    }
    return best;
}

/* Warps to whatever door is nearest the given map tile. Returns the door id
 * used, or -1 when the area has nothing close enough. */
int PortPpuMzm_DebugRequestWarpToMapTile(int area, int tileX, int tileY) {
    int door = PortPpuMzm_DebugFindDoorNearMapTile(area, tileX, tileY, 6);
    if (door < 0) return -1;
    if (!PortPpuMzm_RequestWarpTo((u8)area, (u8)door)) return -1;
    char msg[96];
    snprintf(msg, sizeof(msg), "MAP WARP: area=%d tile=%d,%d -> door=%d", area, tileX, tileY, door);
    Port_DebugLog(msg);
    return door;
}

/* Marks every tile of every area as explored, and every area map as
 * downloaded.
 *
 * gVisitedMinimapTiles is the persistent per-area bitfield (one bit per
 * column, 32 rows) the save file carries; setting it wholesale is what
 * "explored" means. downloadedMapStatus is the separate per-area
 * map-station bit that MinimapSetDownloadedTiles checks.
 *
 * The current area additionally keeps a decompressed working copy
 * (gDecompressedMinimapVisitedTiles) that is only rebuilt on an area
 * transition, so it has to be refreshed by hand here -- with exactly the
 * sequence MinimapCheckOnTransition uses (copy the raw map over it, then
 * MinimapSetDownloadedTiles), or the tiles the player is standing among
 * would stay dark until they walked to another area and back. */
void PortPpuMzm_DebugRevealAllMaps(void) {
    /* [area][row]: with USE_EWRAM_SYMBOLS off (this build), structs/minimap.h
     * defines gVisitedMinimapTiles as a two-dimensional hardcoded-pointer
     * cast, not the flat one-dimensional symbol the other variant declares. */
    for (int a = 0; a < MAX_AMOUNT_OF_AREAS; ++a) {
        for (int row = 0; row < MINIMAP_SIZE; ++row) {
            gVisitedMinimapTiles[a][row] = 0xFFFFFFFFu;
        }
    }
    gEquipment.downloadedMapStatus = 0xFF;

    memcpy(gDecompressedMinimapVisitedTiles, gDecompressedMinimapData,
           MINIMAP_SIZE * MINIMAP_SIZE * sizeof(u16));
    MinimapSetDownloadedTiles(gCurrentArea, gDecompressedMinimapVisitedTiles);

    Port_DebugLog("DEBUG: all maps revealed");
}

/* ---- Equipment -------------------------------------------------------
 * Every flag exists twice in gEquipment: the "owned" set (beamBombs /
 * suitMisc) and the "currently switched on" set (beamBombsActivation /
 * suitMiscActivation) that the pause screen toggles. A debug toggle has to
 * move both together, or an item reads as collected but does nothing (or,
 * worse, as active but not owned).
 * -------------------------------------------------------------------- */
void PortPpuMzm_DebugGetEquipment(unsigned* outBeams, unsigned* outMisc) {
    if (outBeams) *outBeams = gEquipment.beamBombs;
    if (outMisc) *outMisc = gEquipment.suitMisc;
}

void PortPpuMzm_DebugToggleBeam(unsigned bit) {
    if (gEquipment.beamBombs & bit) {
        gEquipment.beamBombs &= (u8)~bit;
        gEquipment.beamBombsActivation &= (u8)~bit;
    } else {
        gEquipment.beamBombs |= (u8)bit;
        gEquipment.beamBombsActivation |= (u8)bit;
    }
}

void PortPpuMzm_DebugToggleMisc(unsigned bit) {
    if (gEquipment.suitMisc & bit) {
        gEquipment.suitMisc &= (u8)~bit;
        gEquipment.suitMiscActivation &= (u8)~bit;
    } else {
        gEquipment.suitMisc |= (u8)bit;
        gEquipment.suitMiscActivation |= (u8)bit;
    }
}

void PortPpuMzm_DebugSetAllEquipment(bool on) {
    u8 v = on ? 0xFF : 0x00;
    gEquipment.beamBombs = v;
    gEquipment.beamBombsActivation = v;
    gEquipment.suitMisc = v;
    gEquipment.suitMiscActivation = v;
    /* suitType tracks which suit sprite Samus wears; keep it consistent with
     * having/not having the suit upgrades rather than leaving a fully
     * powered suit on a Samus who now owns nothing. */
    gEquipment.suitType = on ? SUIT_FULLY_POWERED : SUIT_NORMAL;
}

/* Ammo/energy. The maxima are the real 100%-run totals for normal
 * difficulty, not arbitrary big numbers, so a debug-maxed file behaves like
 * a legitimately completed one. */
#define PORT_DEBUG_MAX_ENERGY        1299
#define PORT_DEBUG_MAX_MISSILES      250
#define PORT_DEBUG_MAX_SUPERS        30
#define PORT_DEBUG_MAX_POWER_BOMBS   30

void PortPpuMzm_DebugSetAmmo(bool full) {
    if (full) {
        gEquipment.maxEnergy = PORT_DEBUG_MAX_ENERGY;
        gEquipment.maxMissiles = PORT_DEBUG_MAX_MISSILES;
        gEquipment.maxSuperMissiles = PORT_DEBUG_MAX_SUPERS;
        gEquipment.maxPowerBombs = PORT_DEBUG_MAX_POWER_BOMBS;
    } else {
        gEquipment.maxEnergy = 99;
        gEquipment.maxMissiles = 0;
        gEquipment.maxSuperMissiles = 0;
        gEquipment.maxPowerBombs = 0;
    }
    gEquipment.currentEnergy = gEquipment.maxEnergy;
    gEquipment.currentMissiles = gEquipment.maxMissiles;
    gEquipment.currentSuperMissiles = gEquipment.maxSuperMissiles;
    gEquipment.currentPowerBombs = gEquipment.maxPowerBombs;
}

/* Tops up the current counts without touching the capacities. */
void PortPpuMzm_DebugRefillAmmo(void) {
    gEquipment.currentEnergy = gEquipment.maxEnergy;
    gEquipment.currentMissiles = gEquipment.maxMissiles;
    gEquipment.currentSuperMissiles = gEquipment.maxSuperMissiles;
    gEquipment.currentPowerBombs = gEquipment.maxPowerBombs;
}

void PortPpuMzm_DebugGetAmmoText(char* out, int outSize) {
    if (!out || outSize <= 0) return;
    snprintf(out, (size_t)outSize, "E%u/%u M%u S%u P%u",
             gEquipment.currentEnergy, gEquipment.maxEnergy,
             gEquipment.maxMissiles, gEquipment.maxSuperMissiles,
             gEquipment.maxPowerBombs);
}
