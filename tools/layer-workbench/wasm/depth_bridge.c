/*
 * WebAssembly bridge for the layer workbench: exposes the port's own
 * stereo-depth decision functions (platform/3ds/source/port_stereo_depth.c
 * and port_layer_fixes.c) to JS, with no reimplementation of the logic.
 *
 * The real headers take a struct (PortStereoDepthState) and pointer
 * arguments (PortLayerFix_SetRoom's bgData/bgW/bgH), which are not directly
 * callable from JS across the wasm boundary without a thin adapter. This
 * file is exactly that adapter: it flattens the struct into scalar
 * parameters and builds the struct on the C side, then forwards straight
 * into the unmodified port functions. Nothing here decides a tier -- it
 * only marshals arguments, which is why depth_bridge.c compiling and
 * running is a guarantee the JS is calling the SAME code the 3DS build
 * calls, not a lookalike.
 */
#include <emscripten/emscripten.h>
#include <stddef.h>

#include "port_stereo_depth.h"
#include "port_layer_fixes.h"

/* ---- port_stereo_depth.h ------------------------------------------- */

static PortStereoDepthState BuildState(int p0, int p1, int p2, int p3,
                                        int inGameplay, int bg0IsOverlayText,
                                        int samusOnTopOfBackgrounds,
                                        int flatMenu, int flatMenuBackdropPrio) {
    PortStereoDepthState st;
    st.priority[0] = (uint8_t)p0;
    st.priority[1] = (uint8_t)p1;
    st.priority[2] = (uint8_t)p2;
    st.priority[3] = (uint8_t)p3;
    st.inGameplay = inGameplay != 0;
    st.bg0IsOverlayText = bg0IsOverlayText != 0;
    st.samusOnTopOfBackgrounds = samusOnTopOfBackgrounds != 0;
    st.flatMenu = flatMenu != 0;
    st.flatMenuBackdropPrio = (uint8_t)flatMenuBackdropPrio;
    return st;
}

EMSCRIPTEN_KEEPALIVE
int depth_bg_tier(int p0, int p1, int p2, int p3, int inGameplay,
                   int bg0IsOverlayText, int samusOnTopOfBackgrounds,
                   int flatMenu, int flatMenuBackdropPrio, int bgIndex) {
    PortStereoDepthState st = BuildState(p0, p1, p2, p3, inGameplay,
        bg0IsOverlayText, samusOnTopOfBackgrounds, flatMenu, flatMenuBackdropPrio);
    return PortStereoDepth_BgTier(&st, bgIndex);
}

EMSCRIPTEN_KEEPALIVE
int depth_bg_tier_for_priority(int p0, int p1, int p2, int p3, int inGameplay,
                                int bg0IsOverlayText, int samusOnTopOfBackgrounds,
                                int flatMenu, int flatMenuBackdropPrio, int priority) {
    PortStereoDepthState st = BuildState(p0, p1, p2, p3, inGameplay,
        bg0IsOverlayText, samusOnTopOfBackgrounds, flatMenu, flatMenuBackdropPrio);
    return PortStereoDepth_BgTierForPriority(&st, priority);
}

EMSCRIPTEN_KEEPALIVE
int depth_obj_tier(int p0, int p1, int p2, int p3, int inGameplay,
                    int bg0IsOverlayText, int samusOnTopOfBackgrounds,
                    int flatMenu, int flatMenuBackdropPrio, int objPriority) {
    PortStereoDepthState st = BuildState(p0, p1, p2, p3, inGameplay,
        bg0IsOverlayText, samusOnTopOfBackgrounds, flatMenu, flatMenuBackdropPrio);
    return PortStereoDepth_ObjTier(&st, objPriority);
}

EMSCRIPTEN_KEEPALIVE
float depth_tier_px_for(int spread, int tier) {
    return PortStereoDepth_TierPxFor(spread, tier);
}

EMSCRIPTEN_KEEPALIVE
int depth_tier_count(void) { return PORT_TIER_COUNT; }

EMSCRIPTEN_KEEPALIVE
int depth_spread_count(void) { return PORT_STEREO_SPREAD_COUNT; }

/* ---- port_layer_fixes.h ---------------------------------------------
 * PortLayerFix_SetRoom takes const uint16_t* const* bgData -- an array of
 * per-BG pointers -- which JS cannot build directly. These wrappers keep a
 * small static scratch area on the C side and let JS fill it one BG at a
 * time (Emscripten's HEAPU16 writes go straight into wasm linear memory, so
 * the "upload" is just an offset the caller already has via the returned
 * pointer). */

#define MAX_FIX_BG 4
static uint16_t sScratch[MAX_FIX_BG][4096]; /* generous: largest room's blocks */
static const uint16_t* sPtrs[MAX_FIX_BG];
static uint16_t sW[MAX_FIX_BG];
static uint16_t sH[MAX_FIX_BG];
static int sPresent[MAX_FIX_BG];

EMSCRIPTEN_KEEPALIVE
uint16_t* depth_fix_scratch(int bg) {
    if (bg < 0 || bg >= MAX_FIX_BG) return NULL;
    return sScratch[bg];
}

EMSCRIPTEN_KEEPALIVE
int depth_fix_scratch_capacity(void) { return 4096; }

EMSCRIPTEN_KEEPALIVE
void depth_fix_set_bg(int bg, int present, int w, int h) {
    if (bg < 0 || bg >= MAX_FIX_BG) return;
    sPresent[bg] = present;
    sW[bg] = (uint16_t)w;
    sH[bg] = (uint16_t)h;
    sPtrs[bg] = present ? sScratch[bg] : NULL;
}

EMSCRIPTEN_KEEPALIVE
int depth_fix_set_room(int area, int room) {
    return PortLayerFix_SetRoom(area, room, sPtrs, sW, sH);
}

EMSCRIPTEN_KEEPALIVE
int depth_fix_dest_for(int bg, int colBlock, int rowBlock) {
    return PortLayerFix_DestFor(bg, colBlock, rowBlock);
}

EMSCRIPTEN_KEEPALIVE
int depth_fix_present(void) { return PortLayerFix_Present() ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE
int depth_fix_active_count(void) { return PortLayerFix_ActiveCount(); }
