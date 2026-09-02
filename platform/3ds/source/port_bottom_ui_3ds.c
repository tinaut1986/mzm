#include "port_bottom_ui_3ds.h"

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

#include "platform_gpu_3ds.h"
#include "port_debug_tools.h"
#include "port_debug_log.h"

/* GBA & MZM minimap and state globals */
extern uint16_t gDecompressedMinimapVisitedTiles[32 * 32];
extern uint16_t gDecompressedMinimapData[32 * 32];
extern uint32_t gVisitedMinimapTiles[8 * 32];
extern uint32_t gMinimapTilesWithObtainedItems[8 * 32];
extern uint8_t gMinimapX;
extern uint8_t gMinimapY;
extern uint8_t gCurrentArea;
extern uint8_t gCurrentRoom;
extern int8_t gLanguage;

/* Equipment struct (from include/structs/samus.h) — declared here to avoid
 * pulling in the GBA types.h which conflicts with the 3DS SDK headers. */
struct Equipment {
    uint16_t maxEnergy;
    uint16_t maxMissiles;
    uint8_t maxSuperMissiles;
    uint8_t maxPowerBombs;
    uint16_t currentEnergy;
    uint16_t currentMissiles;
    uint8_t currentSuperMissiles;
    uint8_t currentPowerBombs;
    uint8_t beamBombs;
    uint8_t beamBombsActivation;
    uint8_t suitMisc;
    uint8_t suitMiscActivation;
    uint8_t downloadedMapStatus;
    uint8_t lowHealth;
    uint8_t suitType;
    uint8_t grabbedByMetroid;
};
extern struct Equipment gEquipment;

/* Weapon state — missilesSelected is at offset 3 in the real struct.
 * We only need this one field, so declare a matching stub. */
struct WeaponInfoStub {
    uint8_t _pad[3];            /* diagonalAim + newProjectile + weaponHighlighted */
    uint8_t missilesSelected;   /* FALSE = normals, TRUE = supers */
};
extern struct WeaponInfoStub gSamusWeaponInfo;

/* ROM-resolved minimap graphic and palette tables */
extern const uint8_t(*p_sMinimapTilesGfx)[5120];
extern const uint16_t(*p_sMinimapTilesPal)[5 * 16];

/* External area map loaders */
extern void PauseScreenGetMinimapData(uint8_t area, uint16_t* dst);
extern void MinimapSetDownloadedTiles(uint8_t area, uint16_t* dst);

/* Chozo target tracking */
struct ChozoStatueTargetView {
    uint8_t statueArea;
    uint8_t statueXStart;
    uint8_t statueXEnd;
    uint8_t statueYStart;
    uint8_t statueYEnd;
    uint8_t startIcon;

    uint8_t targetArea;
    uint8_t targetX;
    uint8_t targetY;
    uint8_t endIcon;
    uint8_t _pad[2];
};
extern const struct ChozoStatueTargetView(*p_sChozoStatueTargets)[16];
extern int32_t ChozoStatueHintCheckTargetIsActivated(uint8_t target) __attribute__((weak));

/* Platform and Config functions */
extern bool Port_Config_GetShowFps(void);
extern void Port_Config_SetShowFps(bool on);
extern int Port_Config_Get3DSAspectRatio(void);
extern const char* Port_Config_Get3DSAspectRatioName(void);
extern void Port_Config_Cycle3DSAspectRatio(void);
extern int Port_Config_Get3DSDisplayStyle(void);
extern const char* Port_Config_Get3DSDisplayStyleName(void);
extern void Port_Config_Cycle3DSDisplayStyle(void);

extern bool Platform3DS_IsNew3DS(void);
extern double Port_PPU_3DS_CurrentFps(void);
extern bool Port_PPU_3DS_LastFrameUsedGpu(void);
extern void Port_GpuRenderer_GetLastFrameStats(int* outItems, int* outObjItems, int* outCacheSlots);

static PortBottomTab sCurrentTab = BOTTOM_TAB_MAP;
static uint32_t sFrameCounter = 0;

/* Bottom-screen redraw throttle.
 *
 * Every view here is rasterised from scratch each call: the 5x7 bitmap font
 * and DrawAuthenticMapTile both emit one C2D_DrawRectSolid per pixel run,
 * so a text-heavy DEBUG view or a zoomed-out MAP view is several thousand
 * immediate-mode quads assembled on the main thread every frame. With the
 * game frame already sitting on the 16.6ms vsync edge that was enough to
 * push it over -- MAP at 1X measured ~30fps, at 3X (far fewer cells on
 * screen) a full 60. None of this content needs 60Hz: redraw it at ~20Hz
 * and reuse the persistent render target on the frames in between. A
 * genuine change (tab switch, zoom, pan, any touch) forces the next frame
 * to redraw immediately via Port_BottomUI_MarkDirty so interaction still
 * feels instant. */
#define PORT_BOTTOM_UI_REDRAW_INTERVAL 3 /* frames; 3 -> ~20Hz at 60fps */
static bool sBottomUiDirty = true;
static uint32_t sBottomUiRedrawThrottle = 0;

void Port_BottomUI_MarkDirty(void) { sBottomUiDirty = true; }

/* Called once per frame (even on frames the UI is not redrawn) so time-based
 * state keeps advancing: the blink counter and the RA session pump. */
void Port_BottomUI_FrameTick(void) {
    extern void Port_RA_Update(void); /* port_retroachievements_3ds.h, included below */
    ++sFrameCounter;
    Port_RA_Update();
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    { extern void PortPpuMzm_DebugCheatTick(void); PortPpuMzm_DebugCheatTick(); }
#endif
}

/* Whether Port_BottomUI_Render should run this frame. Has the side effect of
 * advancing the throttle, so call exactly once per frame. */
bool Port_BottomUI_WantsRedraw(void) {
    if (sBottomUiDirty) {
        sBottomUiDirty = false;
        sBottomUiRedrawThrottle = 0;
        return true;
    }
    if (++sBottomUiRedrawThrottle >= PORT_BOTTOM_UI_REDRAW_INTERVAL) {
        sBottomUiRedrawThrottle = 0;
        return true;
    }
    return false;
}

/* Modals & Overlays */
static bool sShowRemapModal = false;
static int sRemapSelectButtonIdx = -1; /* >= 0 when action picker popup is open */
static bool sShowCollectiblesModal = false;
static bool sShowAchievementsModal = false;
/* Pack chooser shown ahead of the achievement list. Only ever opened when the
 * game actually has more than one set -- with a single set a chooser with one
 * entry in it is pure friction, so the LOGROS button goes straight to the
 * list (see OpenAchievementsUi). sAchFromPacks records which way we came in so
 * BACK returns to the chooser instead of closing the whole modal. */
static bool sShowAchPacksModal = false;
static bool sAchFromPacks = false;
static bool sShowRASettingsModal = false;
static bool sShowDisplayModal = false;

#ifdef PORT_DEBUG_TOOLS_ACTIVE
/* DEBUG tab -> [HERRAMIENTAS] modal. Touchable equivalent of the L+R+<btn>
 * combos documented in docs/3ds-debug-tools.md: same entry points, just
 * reachable without memorizing (or mis-pressing) a hold-two-shoulders
 * chord mid-gameplay. The combos stay wired up in
 * Platform3DS_PollKeysIntoGba -- this is an addition, not a replacement,
 * since some of them (the scene recorder especially) are worth triggering
 * without taking a hand off the controller. */
static bool sShowDebugToolsModal = false;
static bool sShowDebugWarpModal = false;
static bool sShowDebugEquipModal = false;
/* MAP tab: when armed from the tools menu, the next tap on the map canvas
 * warps to the door nearest that tile instead of panning. One-shot -- it
 * disarms itself on use -- so a stray tap can't teleport the player later. */
static bool sDebugMapWarpArmed = false;
/* Largest distance (squared, px) the stylus wandered from where it first
 * touched down, for the current touch. See the tap test in
 * Port_BottomUI_TouchReleased. */
static int sDebugMapWarpDevSq = 0;
/* Transient one-line feedback under the list ("PUNTO GUARDADO", ...).
 * Frame-counted rather than timed: this UI already has sFrameCounter and
 * no clock source of its own. */
static char sDebugToolsMsg[48] = { 0 };
static uint32_t sDebugToolsMsgUntil = 0;

extern void PortPpuMzm_DebugKillSamus(void);
extern void Port_GpuRenderer_DumpAtlas(const char* ppmPath, const char* csvPath);
extern bool Port_GpuRenderer_IsActive(void);
extern void Port_GpuRenderer_SetActive(bool active);
extern void Port_DebugLog(const char* msg);
extern void PortStereoDepth_SetSpread(int spread);
extern int PortStereoDepth_GetSpread(void);
extern const char* PortStereoDepth_SpreadName(int spread);
extern const char* PortStereoDepth_SpreadNameLang(int spread, int lang);
extern bool Port_DebugLog_IsEnabled(void);
extern void Port_DebugLog_SetBuffered(bool buffered);
extern bool Port_DebugLog_IsBuffered(void);
extern void Port_DebugLog_CycleMode(void);       /* OFF -> ALL -> GPU -> AUDIO -> PERF -> OFF */
extern const char* Port_DebugLog_ModeName(void); /* "OFF"/"ALL"/"GPU"/"AUDIO"/"PERF" */
extern void PortPpuMzm_DebugSaveWarpPoint(void);
extern bool PortPpuMzm_DebugHasWarpPoint(void);
extern void PortPpuMzm_DebugGetWarpPointInfo(char* out, int outSize);
extern bool PortPpuMzm_DebugRequestWarp(void);
extern int PortPpuMzm_DebugGetDoorCount(int area);
extern int PortPpuMzm_DebugGetDoorRoom(int area, int door);
extern void PortPpuMzm_DebugGetWarpSelection(int* outArea, int* outDoor, int* outRoom);
extern void PortPpuMzm_DebugStepWarpArea(int delta);
extern void PortPpuMzm_DebugStepWarpDoor(int delta);
extern bool PortPpuMzm_DebugRequestWarpToSelection(void);
extern int PortPpuMzm_DebugRequestWarpToMapTile(int area, int tileX, int tileY);
extern void PortPpuMzm_DebugRevealAllMaps(void);
extern void PortPpuMzm_DebugRevealAreaMap(int area);
extern void PortPpuMzm_DebugMarkAreaVisited(int area);
extern void PortPpuMzm_DebugClearAreaMap(int area);
extern int  PortPpuMzm_DebugAreaMapPercent(int area);

/* Per-area STATUS map-box debug cycle: 0 actual, 1 forced-download, 2 forced-100%.
 * Read by RenderStatusView to colour the box; advanced by the touch handler. */
static uint8_t sMapDebugState[7] = { 0, 0, 0, 0, 0, 0, 0 };

/* STATUS suit rows: per-row cycle position. Only index 1 (Speed Booster) and
 * index 3 (Screw Attack) use all three steps (none / owned / always-active). */
static uint8_t sSuitCycle[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
extern void PortPpuMzm_DebugSetGod(bool on);
extern bool PortPpuMzm_DebugGetGod(void);
extern void PortPpuMzm_DebugGetEquipment(unsigned* outBeams, unsigned* outMisc);
extern void PortPpuMzm_DebugToggleBeam(unsigned bit);
extern void PortPpuMzm_DebugToggleMisc(unsigned bit);
extern void PortPpuMzm_DebugSetMisc(unsigned bit, bool on);
extern void PortPpuMzm_DebugSetForceEffect(unsigned smfBit, bool on);
extern bool PortPpuMzm_DebugGetForceEffect(unsigned smfBit);
extern void PortPpuMzm_DebugSetAllEquipment(bool on);
extern void PortPpuMzm_DebugSetAmmo(bool full);
extern void PortPpuMzm_DebugRefillAmmo(void);
extern void PortPpuMzm_DebugGetAmmoText(char* out, int outSize);

static void RenderDebugToolsModal(int lang);
static bool HandleDebugToolsModalTouch(int x, int y);
static void RenderDebugWarpModal(int lang);
static void HandleDebugWarpModalTouch(int x, int y);
static void RenderDebugEquipModal(int lang);
static void HandleDebugEquipModalTouch(int x, int y);

static void DebugToolsSetMsg(const char* msg) {
    snprintf(sDebugToolsMsg, sizeof(sDebugToolsMsg), "%s", msg);
    sDebugToolsMsgUntil = sFrameCounter + 180; /* ~3s at 60Hz */
}
#endif
/* Confirmation dialog: false = enable hardcore (+restart), true = plain restart */
static bool sShowConfirmModal = false;
static bool sConfirmIsRestart = false;

/* Remap modal scroll state */
static float sRemapScrollY = 0.0f;

/* Triggers the game's soft reset flow (returns to intro/title like L+R+Start+Select).
 * GameMode is s16; GM_START_SOFT_RESET == 14. */
extern int16_t gMainGameMode;
static void TriggerGameRestart(void) {
    /* Traced so mzm-debug.log can tell this apart from a controller-combo
     * reset -- see the RESET TRACE block in Platform3DS_PollKeysIntoGba. */
    extern void Port_DebugLog(const char* msg);
    Port_DebugLog("RESET TRACE: bottom-screen RESTART button");
    gMainGameMode = 14; /* GM_START_SOFT_RESET */
}

/* Config & Display Helper Externs */
extern bool Port_Config_GetAutoHideHud(void);
extern void Port_Config_SetAutoHideHud(bool on);
extern bool Port_Config_GetHideSpoilers(void);
extern void Port_Config_SetHideSpoilers(bool on);
extern int Port_Config_GetButtonMapping(int buttonIndex);
extern void Port_Config_SetButtonMapping(int buttonIndex, int action);
extern void Port_Config_CycleButtonMapping(int buttonIndex);
extern void Port_Config_ResetButtonMappingDefault(void);
extern const char* Port_Config_GetActionName(int action, int lang);
extern int Port_Config_GetCstickMode(void);
extern void Port_Config_SetCstickMode(int mode);

/* RetroAchievements Helpers */
#include "port_retroachievements_3ds.h"

/* Area selector & zoom state */
static uint8_t sViewArea = 0;
static bool sFollowSamus = true;
static int sZoomLevel = 1; /* 0 = 1x (Overview), 1 = 2x (Detail), 2 = 3x (Ultra) */
static float sScrollX = 0.0f;
static float sScrollY = 0.0f;
static float sAchievementsScrollY = 0.0f;
static float sAchPacksScrollY = 0.0f;
/* Latched on a press that actually lands on the achievements/pack scrollbar
 * TRACK, and the only thing the drag path keys off afterwards. The hit test
 * used to be a bare `x >= 300` on every move event, with no vertical bound:
 * the whole right-hand column counted as the scrollbar, footer row included.
 * Tapping the sort or direction button at x >= 300 fired the button on the
 * press and then, while the finger was still down, the next move event was
 * read as a scrollbar drag at y ~= 204 -- past the end of the 48..200 track,
 * so it clamped and threw the list to the bottom. Latching on the press also
 * means a drag that starts on the thumb keeps tracking when the finger
 * wanders off the track, which the bare coordinate test got wrong the other
 * way. */
static bool sAchScrollbarDrag = false;

/* ---- Achievements list geometry -------------------------------------
 * The card height lives in ONE place because the render pass and the touch
 * pass both need it and they used to disagree: rendering laid out fixed 34px
 * cards while the hit-testing measured wrapped description text
 * (24 + lines*9 + 6, min 38, +6 gap). The scroll extent was therefore
 * computed for a taller list than was ever drawn, which is what let the
 * content scroll down past the modal and collide with the BACK button
 * (issue #32). Both passes call this now, so they cannot drift again. */
#define ACH_LIST_CLIP_Y0 48.0f
#define ACH_LIST_CLIP_Y1 200.0f
#define ACH_CARD_H 34.0f

static float AchievementsContentHeight(void) {
    return (float)Port_RA_GetViewCount() * ACH_CARD_H;
}

static float AchievementsMaxScroll(void) {
    float view = ACH_LIST_CLIP_Y1 - ACH_LIST_CLIP_Y0;
    float content = AchievementsContentHeight();
    return (content > view) ? (content - view) : 0.0f;
}

#define ACH_PACK_CARD_H 40.0f

/* Touch Y on the scrollbar track -> scroll offset. The track spans the same
 * band the cards are clipped to, so it is derived from that rather than from
 * the 48.0f / 124.0f literals this was open-coded with in four places. */
static float AchScrollbarValue(int touchY, float maxScroll) {
    float trackH = ACH_LIST_CLIP_Y1 - ACH_LIST_CLIP_Y0;
    float trackY = (float)touchY - ACH_LIST_CLIP_Y0;
    if (trackY < 0.0f) trackY = 0.0f;
    if (trackY > trackH) trackY = trackH;
    return (trackY / trackH) * maxScroll;
}

static float AchPacksMaxScroll(void) {
    float view = ACH_LIST_CLIP_Y1 - ACH_LIST_CLIP_Y0;
    /* +1 for the "ALL SETS" entry that heads the chooser. */
    float content = (float)(Port_RA_GetSubsetCount() + 1u) * ACH_PACK_CARD_H;
    return (content > view) ? (content - view) : 0.0f;
}

/* A solid rect clipped to a vertical band. The card backgrounds used to be
 * drawn unclipped while only their contents (badge, status pip, title) tested
 * against the band, so a partially-scrolled card painted its full height over
 * the modal chrome below. */
static void DrawRectClipped(float x, float y, float depth, float w, float h, uint32_t color,
                            float clipY0, float clipY1) {
    float y0 = (y < clipY0) ? clipY0 : y;
    float y1 = (y + h > clipY1) ? clipY1 : (y + h);
    if (y1 <= y0) return;
    C2D_DrawRectSolid(x, y0, depth, w, y1 - y0, color);
}

/* Opens the achievements UI from the OPTIONS tab, picking the entry point:
 * the pack chooser when the game has more than one set, the flat list
 * otherwise. */
static void OpenAchievementsUi(void) {
    sAchievementsScrollY = 0.0f;
    sAchPacksScrollY = 0.0f;
    if (Port_RA_GetSubsetCount() > 1u) {
        sShowAchPacksModal = true;
        sShowAchievementsModal = false;
        sAchFromPacks = true;
    } else {
        Port_RA_SetListSubset(0);
        sShowAchPacksModal = false;
        sShowAchievementsModal = true;
        sAchFromPacks = false;
    }
}


/* BACK from the achievement list: to the pack chooser when we came in through
 * it, otherwise straight out of the modal. */
static void AchievementsBack(void) {
    sShowAchievementsModal = false;
    if (sAchFromPacks) {
        sShowAchPacksModal = true;
        Port_RA_SetListSubset(0);
    }
}

static void CycleAchievementsSort(void) {
    RetroAchievementSort next = (RetroAchievementSort)((Port_RA_GetListSort() + 1) % RA_SORT_COUNT);
    Port_RA_SetListSort(next);
    sAchievementsScrollY = 0.0f; /* the old offset means nothing in a new order */
}

static void ToggleAchievementsSortDir(void) {
    Port_RA_SetListDescending(!Port_RA_GetListDescending());
    sAchievementsScrollY = 0.0f;
}

static int sLastTouchX = -1;
static int sLastTouchY = -1;
static int sTouchStartX = -1;
static int sTouchStartY = -1;
static bool sIsDragging = false;
static bool sIsTouchDragging = false;

/* Pack chooser touch: same scroll/drag shape as the achievement list, with
 * row 0 reserved for the "all sets" entry. */
static void HandleAchPacksTouch(int x, int y, bool isNewTap) {
    float maxScroll = AchPacksMaxScroll();

    if (isNewTap) {
        if (x >= 100 && x <= 220 && y >= 204 && y <= 230) {
            sShowAchPacksModal = false;
            sLastTouchX = -1;
            sLastTouchY = -1;
            return;
        }
        if (x >= 300 && (float)y >= ACH_LIST_CLIP_Y0 && (float)y < ACH_LIST_CLIP_Y1 &&
            maxScroll > 0.0f) {
            sAchScrollbarDrag = true;
            sAchPacksScrollY = AchScrollbarValue(y, maxScroll);
            sIsTouchDragging = true;
            return;
        }
        /* Picking a set happens on RELEASE, in Port_BottomUI_TouchReleased,
         * and only when the touch never turned into a drag -- the same shape
         * the remap modal uses. Opening on the press instead would fire the
         * moment a finger landed on a card to scroll the list. */
        sTouchStartX = x;
        sTouchStartY = y;
        sLastTouchX = x;
        sLastTouchY = y;
        sIsTouchDragging = false;
        return;
    }

    if (sAchScrollbarDrag && maxScroll > 0.0f) {
        sAchPacksScrollY = AchScrollbarValue(y, maxScroll);
        sIsTouchDragging = true;
    } else if (!sAchScrollbarDrag && sLastTouchX >= 0 && sLastTouchY >= 0) {
        float dy = (float)(sLastTouchY - y);
        sAchPacksScrollY += dy;
        if (sAchPacksScrollY < 0.0f) sAchPacksScrollY = 0.0f;
        if (sAchPacksScrollY > maxScroll) sAchPacksScrollY = maxScroll;
        sLastTouchX = x;
        sLastTouchY = y;
        sIsTouchDragging = true;
    }
}

int Port_BottomUI_GetZoom(void) { return sZoomLevel; }
void Port_BottomUI_SetZoom(int zoom) { if (zoom >= 0 && zoom <= 2) { sZoomLevel = zoom; Port_BottomUI_MarkDirty(); } }

int Port_BottomUI_GetViewArea(void) { return (int)sViewArea; }
void Port_BottomUI_SetViewArea(int area) { if (area >= 0 && area <= 6) { sViewArea = (uint8_t)area; Port_BottomUI_MarkDirty(); } }

bool Port_BottomUI_GetFollowSamus(void) { return sFollowSamus; }
void Port_BottomUI_SetFollowSamus(bool follow) { sFollowSamus = follow; Port_BottomUI_MarkDirty(); }

#define LANGUAGE_COUNT 7
static const char* Port_Config_GetLanguageDisplayName(int lang);
static void Port_Config_CycleLanguage(void);
static const char* GetAspectRatioDisplayName(int lang);
static const char* GetDisplayStyleDisplayName(int lang);
static const char* GetFpsOverlayDisplayName(int lang);

/* Cached buffer for viewing other areas */
static uint16_t sOtherAreaTiles[32 * 32];
static uint8_t sCachedOtherArea = 0xFF;

/* Language helpers (0=JP, 1=HIRA, 2=EN, 3=DE, 4=FR, 5=IT, 6=ES) */
static inline int GetLang(void) {
    if (gLanguage >= 0 && gLanguage <= 6) return (int)gLanguage;
    return 2; /* Default English */
}

bool Port_BottomUI_DebugTabVisible(void) {
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    return true;
#else
    return false;
#endif
}

static const char* AreaName(uint8_t area) {
    switch (area) {
        case 0: return "BRINSTAR";
        case 1: return "KRAID";
        case 2: return "NORFAIR";
        case 3: return "RIDLEY";
        case 4: return "TOURIAN";
        case 5: return "CRATERIA";
        case 6: return "CHOZODIA";
        case 7: return "DEBUG";
        default: return "UNKNOWN";
    }
}

/* Crisp 5x7 bitmap font glyph table */
static const uint8_t* GetGlyph(char c) {
    static const uint8_t digits[10][7] = {
        { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, /* 0 */
        { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* 1 */
        { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, /* 2 */
        { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E }, /* 3 */
        { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, /* 4 */
        { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E }, /* 5 */
        { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E }, /* 6 */
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
        { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, /* 8 */
        { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E }, /* 9 */
    };
    static const uint8_t letters[26][7] = {
        { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* A */
        { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, /* B */
        { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, /* C */
        { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C }, /* D */
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, /* E */
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, /* F */
        { 0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0F }, /* G */
        { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* H */
        { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* I */
        { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, /* J */
        { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, /* K */
        { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, /* L */
        { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, /* M */
        { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 }, /* N */
        { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, /* O */
        { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, /* P */
        { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, /* Q */
        { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, /* R */
        { 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E }, /* S */
        { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, /* T */
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, /* U */
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, /* V */
        { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, /* W */
        { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, /* X */
        { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, /* Y */
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }, /* Z */
    };
    static const uint8_t colon[7]   = { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 };
    static const uint8_t dot[7]     = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C };
    static const uint8_t dash[7]    = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 };
    static const uint8_t slash[7]   = { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 };
    static const uint8_t percent[7] = { 0x13, 0x13, 0x02, 0x04, 0x08, 0x19, 0x19 };
    static const uint8_t lbracket[7]= { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E };
    static const uint8_t rbracket[7]= { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E };
    static const uint8_t checkmark[7]={ 0x00, 0x01, 0x01, 0x02, 0x12, 0x0C, 0x00 }; /* ✓ / * */
    static const uint8_t plus[7]    = { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 };
    static const uint8_t equal[7]   = { 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00 };
    static const uint8_t exclam[7]  = { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 };
    static const uint8_t question[7]= { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 };
    static const uint8_t gt[7]      = { 0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10 };
    static const uint8_t lt[7]      = { 0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01 };

    static const uint8_t n_tilde[7] = { 0x1A, 0x00, 0x11, 0x19, 0x15, 0x13, 0x11 }; /* Ñ */
    static const uint8_t c_cedil[7] = { 0x0E, 0x11, 0x10, 0x10, 0x11, 0x0E, 0x04 }; /* Ç */
    static const uint8_t a_acute[7] = { 0x02, 0x04, 0x0E, 0x11, 0x1F, 0x11, 0x11 }; /* Á */
    static const uint8_t e_acute[7] = { 0x02, 0x04, 0x1F, 0x10, 0x1E, 0x10, 0x1F }; /* É */
    static const uint8_t i_acute[7] = { 0x02, 0x04, 0x0E, 0x04, 0x04, 0x04, 0x0E }; /* Í */
    static const uint8_t o_acute[7] = { 0x02, 0x04, 0x0E, 0x11, 0x11, 0x11, 0x0E }; /* Ó */
    static const uint8_t u_acute[7] = { 0x02, 0x04, 0x11, 0x11, 0x11, 0x11, 0x0E }; /* Ú */
    static const uint8_t a_umlaut[7]= { 0x0A, 0x00, 0x0E, 0x11, 0x1F, 0x11, 0x11 }; /* Ä */
    static const uint8_t o_umlaut[7]= { 0x0A, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E }; /* Ö */
    static const uint8_t u_umlaut[7]= { 0x0A, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0E }; /* Ü */
    static const uint8_t eszett[7]  = { 0x1E, 0x11, 0x1E, 0x11, 0x11, 0x11, 0x1C }; /* ß */

    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    if (c == ':') return colon;
    if (c == '.') return dot;
    if (c == '-') return dash;
    if (c == '/') return slash;
    if (c == '%') return percent;
    if (c == '[') return lbracket;
    if (c == ']') return rbracket;
    if (c == '(') return lbracket;
    if (c == ')') return rbracket;
    if (c == '*' || c == 'v' || c == '#') return checkmark;
    if (c == '+') return plus;
    if (c == '=') return equal;
    if (c == '!') return exclam;
    if (c == '?') return question;
    if (c == '>') return gt;
    if (c == '<') return lt;

    /* Single-byte ISO-8859-1 or normalized codepoints */
    switch ((uint8_t)c) {
        case 0xD1: case 0xF1: return n_tilde; /* Ñ, ñ */
        case 0xC7: case 0xE7: return c_cedil; /* Ç, ç */
        case 0xC1: case 0xE1: case 0xC0: case 0xE0: return a_acute; /* Á, á, À, à */
        case 0xC9: case 0xE9: case 0xC8: case 0xE8: case 0xCA: case 0xEA: return e_acute; /* É, é, È, è, Ê, ê */
        case 0xCD: case 0xED: case 0xCC: case 0xEC: return i_acute; /* Í, í, Ì, ì */
        case 0xD3: case 0xF3: case 0xD2: case 0xF2: return o_acute; /* Ó, ó, Ò, ò */
        case 0xDA: case 0xFA: case 0xD9: case 0xF9: return u_acute; /* Ú, ú, Ù, ù */
        case 0xC4: case 0xE4: return a_umlaut; /* Ä, ä */
        case 0xD6: case 0xF6: return o_umlaut; /* Ö, ö */
        case 0xDC: case 0xFC: return u_umlaut; /* Ü, ü */
        case 0xDF: return eszett; /* ß */
        default: break;
    }
    return NULL;
}

static const uint8_t* GetUtf8Glyph(const char** textPtr) {
    const uint8_t* s = (const uint8_t*)*textPtr;
    if (!*s) return NULL;
    if (s[0] < 0x80) {
        (*textPtr)++;
        return GetGlyph((char)s[0]);
    }
    /* 2-byte UTF-8 */
    if ((s[0] & 0xE0) == 0xC0 && s[1]) {
        uint16_t code = (uint16_t)(((s[0] & 0x1F) << 6) | (s[1] & 0x3F));
        *textPtr += 2;
        if (code >= 0x00A0 && code <= 0x00FF) {
            return GetGlyph((char)code);
        }
        return NULL;
    }
    /* 3-byte UTF-8 (e.g. CJK / Katakana / Hiragana) */
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        *textPtr += 3;
        return NULL;
    }
    /* 4-byte UTF-8 */
    if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *textPtr += 4;
        return NULL;
    }
    (*textPtr)++;
    return NULL;
}

static void DrawTextClipped(float x, float y, float scale, const char* text, uint32_t color, float clipY0, float clipY1) {
    x = (float)(int)(x + (x >= 0.0f ? 0.5f : -0.5f));
    y = (float)(int)(y + (y >= 0.0f ? 0.5f : -0.5f));
    if (y + 7.0f * scale <= clipY0 || y >= clipY1) return;
    float charW = 6.0f * scale;
    while (*text) {
        const uint8_t* glyph = GetUtf8Glyph(&text);
        if (!glyph) {
            x += charW;
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            float py = y + (float)row * scale;
            if (py < clipY0 || py + scale > clipY1) continue;
            uint8_t rowVal = glyph[row];
            for (int col = 0; col < 5;) {
                if ((rowVal & (1u << (4 - col))) == 0) {
                    ++col;
                    continue;
                }
                int end = col + 1;
                while (end < 5 && (rowVal & (1u << (4 - end))) != 0) ++end;
                C2D_DrawRectSolid(x + (float)col * scale, py, 0.85f,
                                  (float)(end - col) * scale, scale, color);
                col = end;
            }
        }
        x += charW;
    }
}

static void DrawTextMaxWClipped(float x, float y, float scale, const char* text, uint32_t color, float clipY0, float clipY1, float maxW) {
    x = (float)(int)(x + (x >= 0.0f ? 0.5f : -0.5f));
    y = (float)(int)(y + (y >= 0.0f ? 0.5f : -0.5f));
    if (y + 7.0f * scale <= clipY0 || y >= clipY1) return;
    float charW = 6.0f * scale;
    float startX = x;
    while (*text) {
        if (x + charW > startX + maxW) break;
        const uint8_t* glyph = GetUtf8Glyph(&text);
        if (!glyph) {
            x += charW;
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            float py = y + (float)row * scale;
            if (py < clipY0 || py + scale > clipY1) continue;
            uint8_t rowVal = glyph[row];
            for (int col = 0; col < 5;) {
                if ((rowVal & (1u << (4 - col))) == 0) {
                    ++col;
                    continue;
                }
                int end = col + 1;
                while (end < 5 && (rowVal & (1u << (4 - end))) != 0) ++end;
                C2D_DrawRectSolid(x + (float)col * scale, py, 0.85f,
                                  (float)(end - col) * scale, scale, color);
                col = end;
            }
        }
        x += charW;
    }
}


static void DrawWrappedTextClipped(float x, float y, float scale, const char* text, float maxWidth, float lineHeight, uint32_t color, float clipY0, float clipY1) {
    if (!text || !*text) return;
    float charW = 6.0f * scale;
    int maxCharsPerLine = (int)(maxWidth / charW);
    if (maxCharsPerLine < 10) maxCharsPerLine = 10;

    char lineBuf[128];
    int lineBufLen = 0;
    float curY = y;
    const char* ptr = text;

    while (*ptr) {
        const char* wordStart = ptr;
        while (*ptr && *ptr != ' ') ++ptr;
        int wordLen = (int)(ptr - wordStart);

        if (lineBufLen == 0) {
            if (wordLen < (int)sizeof(lineBuf) - 1) {
                memcpy(lineBuf, wordStart, wordLen);
                lineBufLen = wordLen;
                lineBuf[lineBufLen] = '\0';
            }
        } else if (lineBufLen + 1 + wordLen <= maxCharsPerLine && lineBufLen + 1 + wordLen < (int)sizeof(lineBuf) - 1) {
            lineBuf[lineBufLen++] = ' ';
            memcpy(lineBuf + lineBufLen, wordStart, wordLen);
            lineBufLen += wordLen;
            lineBuf[lineBufLen] = '\0';
        } else {
            /* flush current line */
            DrawTextClipped(x, curY, scale, lineBuf, color, clipY0, clipY1);
            curY += lineHeight;
            memcpy(lineBuf, wordStart, wordLen);
            lineBufLen = wordLen;
            lineBuf[lineBufLen] = '\0';
        }
        while (*ptr == ' ') ++ptr;
    }
    if (lineBufLen > 0) {
        DrawTextClipped(x, curY, scale, lineBuf, color, clipY0, clipY1);
    }
}

static void DrawText(float x, float y, float scale, const char* text, uint32_t color) {
    DrawTextClipped(x, y, scale, text, color, -1000.0f, 1000.0f);
}

static int Utf8CharCount(const char* text) {
    if (!text) return 0;
    int count = 0;
    while (*text) {
        GetUtf8Glyph(&text);
        ++count;
    }
    return count;
}

static void DrawTextCentered(float cx, float y, float scale, const char* text, uint32_t color) {
    float length = (float)Utf8CharCount(text) * 6.0f * scale;
    DrawText(cx - (length / 2.0f), y, scale, text, color);
}

static void DrawTextCenteredClipped(float cx, float y, float scale, const char* text, uint32_t color, float clipY0, float clipY1) {
    float length = (float)Utf8CharCount(text) * 6.0f * scale;
    DrawTextClipped(cx - (length / 2.0f), y, scale, text, color, clipY0, clipY1);
}

static inline uint32_t Bgr555ToRgba8(uint16_t bgr, bool transparent) {
    if (transparent) return 0;
    uint32_t r = (bgr & 0x1F) * 255 / 31;
    uint32_t g = ((bgr >> 5) & 0x1F) * 255 / 31;
    uint32_t b = ((bgr >> 10) & 0x1F) * 255 / 31;
    return C2D_Color32(r, g, b, 255);
}

static float GetTileSizeForZoom(int zoom) {
    switch (zoom) {
        case 0: return 5.5f;   /* 1x Overview: 32x32 = 176x176 px */
        case 1: return 12.0f;  /* 2x Detail: 32x32 = 384x384 px */
        case 2: return 18.0f;  /* 3x Ultra: 32x32 = 576x576 px */
        default: return 12.0f;
    }
}

/* Inverse of the map canvas' draw transform in RenderMapView -- screen
 * pixel to map tile. Kept next to CenterMapOnTile (its forward twin) so the
 * two stay in sync; the canvas origin/size constants are the ones that
 * function hardcodes. Returns false when the tap falls outside the 32x32
 * map. */
static bool MapTileAtTouch(int x, int y, int* outTileX, int* outTileY) {
    const float canvasX = 4.0f, canvasY = 48.0f, canvasW = 312.0f, canvasH = 188.0f;
    float tileSize = GetTileSizeForZoom(sZoomLevel);
    float startDrawX, startDrawY;
    if (sZoomLevel == 0) {
        startDrawX = canvasX + (canvasW - 32.0f * tileSize) / 2.0f;
        startDrawY = canvasY + (canvasH - 32.0f * tileSize) / 2.0f;
    } else {
        startDrawX = canvasX - sScrollX;
        startDrawY = canvasY - sScrollY;
    }
    int tx = (int)(((float)x - startDrawX) / tileSize);
    int ty = (int)(((float)y - startDrawY) / tileSize);
    if (tx < 0 || tx > 31 || ty < 0 || ty > 31) return false;
    *outTileX = tx;
    *outTileY = ty;
    return true;
}

static void CenterMapOnTile(uint8_t tileX, uint8_t tileY) {
    float tileSize = GetTileSizeForZoom(sZoomLevel);
    const float viewW = 312.0f;
    const float viewH = 188.0f;

    if (sZoomLevel == 0) {
        sScrollX = 0.0f;
        sScrollY = 0.0f;
    } else {
        float focusX = (float)tileX * tileSize + tileSize / 2.0f;
        float focusY = (float)tileY * tileSize + tileSize / 2.0f;
        sScrollX = focusX - (viewW / 2.0f);
        sScrollY = focusY - (viewH / 2.0f);

        float maxScrollX = (32.0f * tileSize) - viewW;
        float maxScrollY = (32.0f * tileSize) - viewH;
        if (maxScrollX < 0.0f) maxScrollX = 0.0f;
        if (maxScrollY < 0.0f) maxScrollY = 0.0f;

        if (sScrollX < 0.0f) sScrollX = 0.0f;
        if (sScrollX > maxScrollX) sScrollX = maxScrollX;
        if (sScrollY < 0.0f) sScrollY = 0.0f;
        if (sScrollY > maxScrollY) sScrollY = maxScrollY;
    }
}

void Port_BottomUI_Init(void) {
    sCurrentTab = BOTTOM_TAB_MAP;
    sFrameCounter = 0;
    sViewArea = 0;
    sFollowSamus = true;
    sZoomLevel = 1;
    sScrollX = 0.0f;
    sScrollY = 0.0f;
    sLastTouchX = -1;
    sLastTouchY = -1;
    sIsDragging = false;
    sCachedOtherArea = 0xFF;
}

void Port_BottomUI_SetTab(PortBottomTab tab) {
    if (tab < BOTTOM_TAB_COUNT) {
        sCurrentTab = tab;
        Port_BottomUI_MarkDirty();
    }
}

PortBottomTab Port_BottomUI_GetTab(void) {
    return sCurrentTab;
}

/* Check active Chozo target */
static bool GetActiveChozoTarget(uint8_t* outArea, uint8_t* outX, uint8_t* outY) {
    if (!p_sChozoStatueTargets || !ChozoStatueHintCheckTargetIsActivated) return false;
    for (uint8_t i = 0; i < 16; ++i) {
        if (ChozoStatueHintCheckTargetIsActivated(i) > 0) {
            *outArea = (*p_sChozoStatueTargets)[i].targetArea;
            *outX = (*p_sChozoStatueTargets)[i].targetX;
            *outY = (*p_sChozoStatueTargets)[i].targetY;
            return true;
        }
    }
    return false;
}

extern void Port_Config_Save(void);

#ifdef PORT_DEBUG_TOOLS_ACTIVE
/* Equipment bits per row of the STATUS page's two columns, in draw order --
 * mirrors the beams[]/suits[] tables in RenderStatusView. Used by the debug
 * tap-to-toggle in Port_BottomUI_HandleTouchDrag. */
static const uint8_t kStatusBeamBits[6] = { 1u<<0, 1u<<1, 1u<<2, 1u<<3, 1u<<4, 1u<<7 };
static const uint8_t kStatusSuitBits[8] = { 1u<<0, 1u<<1, 1u<<2, 1u<<3, 1u<<4, 1u<<5, 1u<<6, 1u<<7 };
static bool sDebugStatsFull = false; /* the max<->min button's toggle state */

/* Two small icon buttons to the right of the STATUS title (debug only):
 *   0: vida+municion a tope <-> al minimo (up/down arrows)
 *   1: god mode toggle (filled/tinted when on)
 * Geometry shared with the touch handler. */
#define STATUS_BTN_Y  27
#define STATUS_BTN_W  16
#define STATUS_BTN_H  14
static const float kStatusBtnX[2] = { 274.0f, 294.0f };

static int DebugStatusBtnHit(int x, int y) {
    if (y < STATUS_BTN_Y || y > STATUS_BTN_Y + STATUS_BTN_H) return -1;
    for (int i = 0; i < 2; ++i)
        if ((float)x >= kStatusBtnX[i] && (float)x <= kStatusBtnX[i] + STATUS_BTN_W) return i;
    return -1;
}

static void DrawStatusDebugButtons(void) {
    const bool god = PortPpuMzm_DebugGetGod();
    for (int i = 0; i < 2; ++i) {
        float bx = kStatusBtnX[i], by = (float)STATUS_BTN_Y;
        bool on = (i == 0) ? sDebugStatsFull : god;
        C2D_DrawRectSolid(bx, by, 0.9f, (float)STATUS_BTN_W, (float)STATUS_BTN_H,
                          on ? C2D_Color32(110, 85, 20, 255) : C2D_Color32(24, 34, 52, 255));
        C2D_DrawRectSolid(bx, by, 0.91f, (float)STATUS_BTN_W, 1.0f, C2D_Color32(60, 90, 140, 255));
        float mx = bx + STATUS_BTN_W * 0.5f, my = by + STATUS_BTN_H * 0.5f;
        uint32_t c = on ? C2D_Color32(255, 220, 90, 255) : C2D_Color32(150, 190, 230, 255);
        if (i == 0) {
            C2D_DrawTriangle(mx - 4.0f, my - 1.5f, c, mx + 4.0f, my - 1.5f, c, mx, my - 6.0f, c, 0.92f);
            C2D_DrawTriangle(mx - 4.0f, my + 1.5f, c, mx + 4.0f, my + 1.5f, c, mx, my + 6.0f, c, 0.92f);
        } else {
            C2D_DrawTriangle(mx, my - 6.0f, c, mx - 6.0f, my, c, mx + 6.0f, my, c, 0.92f);
            C2D_DrawTriangle(mx, my + 6.0f, c, mx - 6.0f, my, c, mx + 6.0f, my, c, 0.92f);
        }
    }
}
#endif /* PORT_DEBUG_TOOLS_ACTIVE */

void Port_BottomUI_HandleTouchDrag(int x, int y, bool isNewTap) {
    /* Any stylus contact can move something (pan, button, modal); redraw the
     * next frame instead of waiting for the throttle. */
    Port_BottomUI_MarkDirty();

    /* 4-Tab Top Navigation Bar (Y: 2 to 24) */
    if (y >= 2 && y <= 24) {
        if (isNewTap) {
            PortBottomTab prevTab = sCurrentTab;
            bool showDebug = Port_BottomUI_DebugTabVisible();
            float totalGap = showDebug ? 12.0f : 8.0f;
            float tabW = (320.0f - 8.0f - totalGap) / (showDebug ? 4.0f : 3.0f);
            if (tabW > 80.0f) tabW = 80.0f;

            float curX = 4.0f;
            if (x >= curX && x <= curX + tabW) sCurrentTab = BOTTOM_TAB_MAP;
            curX += tabW + 4.0f;
            if (x >= curX && x <= curX + tabW) sCurrentTab = BOTTOM_TAB_STATUS;
            curX += tabW + 4.0f;
            if (showDebug) {
                if (x >= curX && x <= curX + tabW) sCurrentTab = BOTTOM_TAB_DEBUG;
                curX += tabW + 4.0f;
            }
            if (x >= curX && x <= curX + tabW) sCurrentTab = BOTTOM_TAB_OPTIONS;
            if (sCurrentTab != prevTab) Port_Config_Save();
        }
        sLastTouchX = -1;
        sLastTouchY = -1;
        sIsDragging = false;
        return;
    }

    if (sCurrentTab == BOTTOM_TAB_MAP) {
        /* Map Subheader controls (Y: 26 to 46) */
        if (y >= 26 && y <= 46) {
            if (isNewTap) {
                /* Previous Area [ < ] */
                if (x >= 4 && x <= 20) {
                    sFollowSamus = false;
                    sViewArea = (sViewArea == 0) ? 6 : (sViewArea - 1);
                    CenterMapOnTile(16, 16);
                    Port_Config_Save();
                }
                /* Area Title Box -> toggle back to current area */
                else if (x > 20 && x < 92) {
                    sFollowSamus = true;
                    sViewArea = gCurrentArea;
                    CenterMapOnTile(gMinimapX, gMinimapY);
                    Port_Config_Save();
                }
                /* Next Area [ > ] */
                else if (x >= 92 && x <= 110) {
                    sFollowSamus = false;
                    sViewArea = (sViewArea + 1) % 7;
                    CenterMapOnTile(16, 16);
                    Port_Config_Save();
                }
                /* Zoom Button [ 1X / 2X / 3X ] */
                else if (x >= 156 && x <= 196) {
                    sZoomLevel = (sZoomLevel + 1) % 3;
                    if (sFollowSamus && gCurrentArea < 7) {
                        CenterMapOnTile(gMinimapX, gMinimapY);
                    } else {
                        CenterMapOnTile(16, 16);
                    }
                    Port_Config_Save();
                }
                /* Center on Samus Button [ SAMUS ] */
                else if (x >= 198 && x <= 250) {
                    sFollowSamus = true;
                    sViewArea = gCurrentArea;
                    CenterMapOnTile(gMinimapX, gMinimapY);
                    Port_Config_Save();
                }
                /* Target Button [ ! OBJ / COORDS ] */
                else if (x >= 252 && x <= 316) {
                    uint8_t tArea = 0, tX = 0, tY = 0;
                    if (GetActiveChozoTarget(&tArea, &tX, &tY)) {
                        sFollowSamus = false;
                        sViewArea = tArea;
                        CenterMapOnTile(tX, tY);
                        Port_Config_Save();
                    }
                }
            }
            sLastTouchX = -1;
            sLastTouchY = -1;
            sIsDragging = false;
            return;
        }

        /* Map Canvas Dragging Area (Y: 48 to 236, X: 4 to 316) */
        if (y >= 48 && y <= 236 && x >= 4 && x <= 316) {
#ifdef PORT_DEBUG_TOOLS_ACTIVE
            /* Persistent WARP button tap toggle (bottom right of map canvas;
             * kept in sync with warpBtn* in RenderMapView -- 58x16 flush to
             * the 312/232 corner, with a couple px of slack). */
            if (isNewTap && x >= 252 && x <= 312 && y >= 214 && y <= 232) {
                sDebugMapWarpArmed = !sDebugMapWarpArmed;
                sLastTouchX = -1;
                sLastTouchY = -1;
                sTouchStartX = -1;
                sTouchStartY = -1;
                sIsDragging = false;
                return;
            }
            /* Armed tap-to-warp. The warp fires on RELEASE in
             * Port_BottomUI_TouchReleased, and only if the finger barely moved. */
            if (sDebugMapWarpArmed) {
                if (isNewTap) {
                    sTouchStartX = x;
                    sTouchStartY = y;
                    sDebugMapWarpDevSq = 0;
                } else if (sTouchStartX >= 0) {
                    /* Track the WORST deviation over the whole touch, not
                     * just where the stylus happened to be at release: a
                     * pan that drifts out and comes back is still a pan. */
                    int dx = x - sTouchStartX;
                    int dy = y - sTouchStartY;
                    int devSq = dx * dx + dy * dy;
                    if (devSq > sDebugMapWarpDevSq) sDebugMapWarpDevSq = devSq;
                }
            }
#endif
            if (sZoomLevel > 0) {
                if (!isNewTap && sLastTouchX >= 0 && sLastTouchY >= 0) {
                    float dx = (float)(sLastTouchX - x);
                    float dy = (float)(sLastTouchY - y);
                    if (dx != 0.0f || dy != 0.0f) {
                        sFollowSamus = false;
                    }
                    sScrollX += dx;
                    sScrollY += dy;

                    float tileSize = GetTileSizeForZoom(sZoomLevel);
                    float maxScrollX = (32.0f * tileSize) - 312.0f;
                    float maxScrollY = (32.0f * tileSize) - 188.0f;
                    if (maxScrollX < 0.0f) maxScrollX = 0.0f;
                    if (maxScrollY < 0.0f) maxScrollY = 0.0f;

                    if (sScrollX < 0.0f) sScrollX = 0.0f;
                    if (sScrollX > maxScrollX) sScrollX = maxScrollX;
                    if (sScrollY < 0.0f) sScrollY = 0.0f;
                    if (sScrollY > maxScrollY) sScrollY = maxScrollY;

                    sIsDragging = true;
                }
                sLastTouchX = x;
                sLastTouchY = y;
            }
            return;
        }
    }

    /* Status Tab Interactive Taps (Collectibles breakdown modal toggle) */
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    if (sCurrentTab == BOTTOM_TAB_DEBUG) {
        if (isNewTap) {
            if (sShowDebugToolsModal) {
                HandleDebugToolsModalTouch(x, y);
            } else if (sShowDebugWarpModal) {
                HandleDebugWarpModalTouch(x, y);
            } else if (sShowDebugEquipModal) {
                HandleDebugEquipModalTouch(x, y);
            } else if (x >= 16 && x <= 304 && y >= 198 && y <= 224) {
                sShowDebugToolsModal = true;
            }
        }
        return;
    }
#endif

    if (sCurrentTab == BOTTOM_TAB_STATUS && isNewTap) {
        if (sShowCollectiblesModal) {
            sShowCollectiblesModal = false;
            return;
        }
#ifdef PORT_DEBUG_TOOLS_ACTIVE
        if (Port_BottomUI_DebugTabVisible()) {
            /* Title-bar icon buttons */
            int btn = DebugStatusBtnHit(x, y);
            if (btn == 0) {
                sDebugStatsFull = !sDebugStatsFull;
                PortPpuMzm_DebugSetAmmo(sDebugStatsFull);
                Port_BottomUI_MarkDirty();
                return;
            }
            if (btn == 1) {
                bool g = !PortPpuMzm_DebugGetGod();
                PortPpuMzm_DebugSetGod(g);
                Port_BottomUI_MarkDirty();
                return;
            }
            /* Beams & bombs column: rows at py = 92 + i*11, x 8..156 */
            if (x >= 8 && x <= 156) {
                for (int i = 0; i < 6; ++i) {
                    int py = 92 + i * 11;
                    if (y >= py && y <= py + 11) {
                        PortPpuMzm_DebugToggleBeam(kStatusBeamBits[i]);
                        Port_BottomUI_MarkDirty();
                        return;
                    }
                }
            }
            /* Suits & movement column: rows at py = 91 + i*9, x 164..312 */
            if (x >= 164 && x <= 312) {
                for (int i = 0; i < 8; ++i) {
                    int py = 91 + i * 9;
                    if (y >= py && y <= py + 9) {
                        /* Speed Booster (i=1) and Screw Attack (i=3) get a
                         * 3-state cycle: none -> owned -> always-active.
                         * Every other row is a plain owned/not toggle. */
                        if (i == 1 || i == 3) {
                            unsigned smf = kStatusSuitBits[i];
                            uint8_t* st = &sSuitCycle[i];
                            *st = (uint8_t)((*st + 1) % 3);
                            PortPpuMzm_DebugSetMisc(smf, *st != 0);
                            PortPpuMzm_DebugSetForceEffect(smf, *st == 2);
                        } else {
                            PortPpuMzm_DebugToggleMisc(kStatusSuitBits[i]);
                        }
                        Port_BottomUI_MarkDirty();
                        return;
                    }
                }
            }
            /* Downloaded-maps card: tapping an area box cycles it
             *   nothing -> downloaded -> fully visited -> nothing
             * (1st tap = get the map, 2nd = mark the whole area explored).
             * The MAP tab's cache of the last non-current area is now stale. */
            {
                int hit = -1;
                if (y >= 188 && y <= 206) {
                    for (int i = 0; i < 4; ++i) { int bx = 14 + i * 73;
                        if (x >= bx && x <= bx + 68) { hit = i; break; } }
                } else if (y >= 210 && y <= 228) {
                    for (int i = 4; i < 7; ++i) { int bx = 14 + (i - 4) * 98;
                        if (x >= bx && x <= bx + 92) { hit = i; break; } }
                }
                if (hit >= 0) {
                    /* Unconditional 3-state cycle:
                     *   0 actual   (restore real state)
                     *   1 revealed (force downloaded / outline)
                     *   2 marked   (force 100% visited)
                     * -> back to 0. No "wiped" / "hidden" step. */
                    uint8_t* st = &sMapDebugState[hit];
                    *st = (uint8_t)((*st + 1) % 3);
                    if (*st == 1)      PortPpuMzm_DebugRevealAreaMap(hit);
                    else if (*st == 2) PortPpuMzm_DebugMarkAreaVisited(hit);
                    else               PortPpuMzm_DebugClearAreaMap(hit);
                    sCachedOtherArea = 0xFF;
                    Port_BottomUI_MarkDirty();
                    return;
                }
            }
        }
#endif
        if (x >= 180 && x <= 310 && y >= 166 && y <= 186) {
            sShowCollectiblesModal = true;
            return;
        }
    }

    /* Modal: pack chooser (only reachable when the game has >1 set) */
    if (sCurrentTab == BOTTOM_TAB_OPTIONS && sShowAchPacksModal) {
        HandleAchPacksTouch(x, y, isNewTap);
        return;
    }

    /* Modal: Achievements View Scrolling & Close */
    if (sCurrentTab == BOTTOM_TAB_OPTIONS && sShowAchievementsModal) {
        float maxAchScroll = AchievementsMaxScroll();

        if (isNewTap) {
            if (x >= 100 && x <= 220 && y >= 204 && y <= 230) {
                AchievementsBack();
                sLastTouchX = -1;
                sLastTouchY = -1;
                return;
            }
            if (x >= 224 && x <= 286 && y >= 204 && y <= 230) {
                CycleAchievementsSort();
                sLastTouchX = -1;
                sLastTouchY = -1;
                return;
            }
            if (x >= 288 && x <= 306 && y >= 204 && y <= 230) {
                ToggleAchievementsSortDir();
                sLastTouchX = -1;
                sLastTouchY = -1;
                return;
            }
            /* Scrollbar press: on the track only -- see sAchScrollbarDrag. */
            if (x >= 300 && (float)y >= ACH_LIST_CLIP_Y0 && (float)y < ACH_LIST_CLIP_Y1 &&
                maxAchScroll > 0.0f) {
                sAchScrollbarDrag = true;
                sAchievementsScrollY = AchScrollbarValue(y, maxAchScroll);
                sIsTouchDragging = true;
            } else {
                sTouchStartX = x;
                sTouchStartY = y;
                sLastTouchX = x;
                sLastTouchY = y;
                sIsTouchDragging = false;
            }
        } else if (sAchScrollbarDrag && maxAchScroll > 0.0f) {
            /* Dragging the scrollbar, latched on the press */
            sAchievementsScrollY = AchScrollbarValue(y, maxAchScroll);
            sIsTouchDragging = true;
        } else if (!sAchScrollbarDrag && sLastTouchX >= 0 && sLastTouchY >= 0) {
            float dy = (float)(sLastTouchY - y);
            sAchievementsScrollY += dy;
            if (sAchievementsScrollY < 0.0f) sAchievementsScrollY = 0.0f;
            if (sAchievementsScrollY > maxAchScroll) sAchievementsScrollY = maxAchScroll;
            sLastTouchX = x;
            sLastTouchY = y;
            sIsTouchDragging = true;
        }
        return;
    }

    /* Options Tab — new modal-based layout (no scroll) */
    if (sCurrentTab == BOTTOM_TAB_OPTIONS) {
        /* Confirmation dialog (hardcore enable / restart game) */
        if (sShowConfirmModal) {
            if (isNewTap) {
                if (x >= 40 && x <= 150 && y >= 140 && y <= 168) {
                    /* ACCEPT */
                    bool wasRestart = sConfirmIsRestart;
                    sShowConfirmModal = false;
                    if (!wasRestart) {
                        Port_RA_SetHardcore(true);
                        Port_Config_Save();
                    }
                    TriggerGameRestart();
                } else if (x >= 170 && x <= 280 && y >= 140 && y <= 168) {
                    /* CANCEL */
                    sShowConfirmModal = false;
                }
            }
            return;
        }

        if (sShowRemapModal) {
            /* Remap action selector popup */
            if (sRemapSelectButtonIdx >= 0) {
                if (isNewTap) {
                    /* Check 9 action buttons + cancel (5 in col 0, 4 in col 1 + cancel) */
                    for (int act = 0; act < 9; ++act) {
                        int col = act / 5;
                        int row = act % 5;
                        float bx = (col == 0) ? 18.0f : 162.0f;
                        float by = 48.0f + (float)row * 28.0f;
                        if (x >= (int)bx && x <= (int)(bx + 140.0f) && y >= (int)by && y <= (int)(by + 24.0f)) {
                            Port_Config_SetButtonMapping(sRemapSelectButtonIdx, act);
                            sRemapSelectButtonIdx = -1;
                            return;
                        }
                    }
                    /* Check Cancel button in col 1, row 4 */
                    if (x >= 162 && x <= 302 && y >= (int)(48.0f + 4.0f * 28.0f) && y <= (int)(48.0f + 4.0f * 28.0f + 24.0f)) {
                        sRemapSelectButtonIdx = -1;
                        return;
                    }
                }
                return;
            }

            /* Remap modal main view: scrollable list of 10 buttons + C-Stick mode */
            const float viewY0 = 44.0f;
            const float viewY1 = 202.0f;
            const float maxScroll = 128.0f; /* (11 items * 26px = 286px) - 158px */

            if (isNewTap) {
                /* Check Bottom Buttons (Y: 204 to 230): RESTABLECER (16..156) and CERRAR (164..304) */
                if (y >= 204 && y <= 230) {
                    if (x >= 16 && x <= 156) {
                        /* Reset Defaults */
                        Port_Config_ResetButtonMappingDefault();
                        return;
                    } else if (x >= 164 && x <= 304) {
                        /* Close */
                        sShowRemapModal = false;
                        sRemapScrollY = 0.0f;
                        sRemapSelectButtonIdx = -1;
                        return;
                    }
                }

                /* Direct scrollbar touch on right */
                if (x >= 300 && maxScroll > 0.0f && y >= (int)viewY0 && y <= (int)viewY1) {
                    float trackY = (float)y - viewY0;
                    if (trackY < 0.0f) trackY = 0.0f;
                    if (trackY > (viewY1 - viewY0)) trackY = viewY1 - viewY0;
                    sRemapScrollY = (trackY / (viewY1 - viewY0)) * maxScroll;
                    sIsTouchDragging = true;
                } else if (y >= (int)viewY0 && y <= (int)viewY1 && x >= 10 && x <= 300) {
                    sTouchStartX = x;
                    sTouchStartY = y;
                    sLastTouchX = x;
                    sLastTouchY = y;
                    sIsTouchDragging = false;
                }
            } else if (x >= 300 && maxScroll > 0.0f && y >= (int)viewY0 && y <= (int)viewY1) {
                float trackY = (float)y - viewY0;
                if (trackY < 0.0f) trackY = 0.0f;
                if (trackY > (viewY1 - viewY0)) trackY = viewY1 - viewY0;
                sRemapScrollY = (trackY / (viewY1 - viewY0)) * maxScroll;
                sIsTouchDragging = true;
            } else if (sLastTouchX >= 0 && sLastTouchY >= 0) {
                float dy = (float)(sLastTouchY - y);
                if (dy != 0.0f) {
                    sRemapScrollY += dy;
                    if (sRemapScrollY < 0.0f) sRemapScrollY = 0.0f;
                    if (sRemapScrollY > maxScroll) sRemapScrollY = maxScroll;
                    if (dy > 2.0f || dy < -2.0f || (sTouchStartY >= 0 && (y - sTouchStartY > 4 || sTouchStartY - y > 4))) {
                        sIsTouchDragging = true;
                    }
                }
                sLastTouchX = x;
                sLastTouchY = y;
            }
            return;
        }

        if (sShowRASettingsModal) {
            if (isNewTap) {
                if (x >= 100 && x <= 220 && y >= 204 && y <= 230) {
                    sShowRASettingsModal = false;
                } else if (x >= 10 && x <= 308) {
                    if (y >= 56 && y <= 80) {
                        /* Login prompt via swkbd */
                        Port_RA_PromptLogin();
                        Port_Config_Save();
                    } else if (y >= 84 && y <= 108) {
                        /* Enable / Disable toggle */
                        bool cur = Port_RA_IsEnabled();
                        Port_RA_SetEnabled(!cur);
                        Port_Config_Save();
                    } else if (y >= 112 && y <= 136) {
                        /* Hardcore Mode currently unavailable / coming soon */
                    } else if (y >= 140 && y <= 164) {
                        /* Sound Notification toggle */
                        bool snd = Port_RA_GetNotificationSound();
                        Port_RA_SetNotificationSound(!snd);
                        Port_Config_Save();
                    }
                }
            }
            return;
        }

        if (sShowDisplayModal) {
            if (isNewTap) {
                if (x >= 100 && x <= 220 && y >= 204 && y <= 230) {
                    sShowDisplayModal = false;
                } else if (x >= 10 && x <= 308) {
                    if (y >= 46 && y <= 67) {
                        Port_Config_CycleLanguage();
                    } else if (y >= 69 && y <= 90) {
                        Port_Config_Cycle3DSAspectRatio();
                    } else if (y >= 92 && y <= 113) {
                        Port_Config_Cycle3DSDisplayStyle();
                    } else if (y >= 115 && y <= 136) {
                        int next = (PortStereoDepth_GetSpread() + 1) % 3;
                        PortStereoDepth_SetSpread(next);
                        Port_Config_Save();
                    } else if (y >= 138 && y <= 159) {
                        Port_Config_SetShowFps(!Port_Config_GetShowFps());
                    } else if (y >= 161 && y <= 182) {
                        Port_Config_SetAutoHideHud(!Port_Config_GetAutoHideHud());
                    } else if (y >= 184 && y <= 204) {
                        Port_Config_SetHideSpoilers(!Port_Config_GetHideSpoilers());
                    }
                }
            }
            return;
        }

        if (sShowAchPacksModal) {
            HandleAchPacksTouch(x, y, isNewTap);
            return;
        }

        if (sShowAchievementsModal) {
            /* Achievements modal scrolling */
            float maxAchScroll = AchievementsMaxScroll();

            if (isNewTap) {
                if (x >= 100 && x <= 220 && y >= 204 && y <= 230) {
                    AchievementsBack();
                    sLastTouchX = -1; sLastTouchY = -1;
                    return;
                }
                if (x >= 224 && x <= 286 && y >= 204 && y <= 230) {
                    CycleAchievementsSort();
                    sLastTouchX = -1; sLastTouchY = -1;
                    return;
                }
                if (x >= 288 && x <= 306 && y >= 204 && y <= 230) {
                    ToggleAchievementsSortDir();
                    sLastTouchX = -1; sLastTouchY = -1;
                    return;
                }
                if (x >= 300 && (float)y >= ACH_LIST_CLIP_Y0 && (float)y < ACH_LIST_CLIP_Y1 &&
                    maxAchScroll > 0.0f) {
                    sAchScrollbarDrag = true;
                    sAchievementsScrollY = AchScrollbarValue(y, maxAchScroll);
                    sIsTouchDragging = true;
                } else {
                    sTouchStartX = x; sTouchStartY = y;
                    sLastTouchX = x; sLastTouchY = y;
                    sIsTouchDragging = false;
                }
            } else if (sAchScrollbarDrag && maxAchScroll > 0.0f) {
                sAchievementsScrollY = AchScrollbarValue(y, maxAchScroll);
                sIsTouchDragging = true;
            } else if (!sAchScrollbarDrag && sLastTouchX >= 0 && sLastTouchY >= 0) {
                float dy = (float)(sLastTouchY - y);
                sAchievementsScrollY += dy;
                if (sAchievementsScrollY < 0.0f) sAchievementsScrollY = 0.0f;
                if (sAchievementsScrollY > maxAchScroll) sAchievementsScrollY = maxAchScroll;
                sLastTouchX = x; sLastTouchY = y;
                sIsTouchDragging = true;
            }
            return;
        }

        /* Main options buttons (no scrolling needed) */
        if (isNewTap) {
            if (x >= 16 && x <= 304 && y >= 48 && y <= 76) {
                sShowDisplayModal = true;
            } else if (y >= 82 && y <= 118) {
                if (x >= 16 && x <= 156) {
                    sShowRASettingsModal = true;
                } else if (x >= 164 && x <= 304) {
                    OpenAchievementsUi();
                }
            } else if (x >= 16 && x <= 304 && y >= 124 && y <= 156) {
                sShowRemapModal = true;
                sRemapScrollY = 0.0f;
                sRemapSelectButtonIdx = -1;
            } else if (x >= 16 && x <= 304 && y >= 164 && y <= 192) {
                sConfirmIsRestart = true;
                sShowConfirmModal = true;
            }
        }
    }
}

void Port_BottomUI_TouchReleased(void) {
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    /* Tap-to-warp: a release close to where the touch started counts as a
     * tap, anything further was a pan. */
    if (sDebugMapWarpArmed && sCurrentTab == BOTTOM_TAB_MAP &&
        sTouchStartX >= 0 && sTouchStartY >= 0) {
        /* Distance-only test, and a generous one. Two earlier attempts were
         * both far too strict for a stylus: `!sIsDragging` is useless here
         * because the pan handler sets that flag on a SINGLE pixel of
         * movement, and a 6px radius is smaller than the wobble the act of
         * pressing a stylus against the screen produces on its own -- it
         * took the better part of ten tries to land a warp. 20px is still
         * well under any deliberate pan, which crosses tens of pixels. */
        if (sDebugMapWarpDevSq <= 400) {
            int tx = 0, ty = 0;
            if (MapTileAtTouch(sTouchStartX, sTouchStartY, &tx, &ty)) {
                /* Stays armed until the user manually turns it off */
                int door = PortPpuMzm_DebugRequestWarpToMapTile(sViewArea, tx, ty);
                DebugToolsSetMsg(door >= 0 ? "TELETRANSPORTANDO..." : "SIN PUERTA CERCA");
            }
        }
    }
#endif

    /* Pack chooser: a release that never became a drag opens that set. Row 0
     * is the "all sets" entry, so subsets start at row 1. */
    if (sCurrentTab == BOTTOM_TAB_OPTIONS && sShowAchPacksModal) {
        if (!sIsTouchDragging && sTouchStartX >= 16 && sTouchStartX <= 300 &&
            (float)sTouchStartY >= ACH_LIST_CLIP_Y0 && (float)sTouchStartY < ACH_LIST_CLIP_Y1) {
            float local = (float)sTouchStartY - ACH_LIST_CLIP_Y0 + sAchPacksScrollY;
            uint32_t row = (uint32_t)(local / ACH_PACK_CARD_H);
            bool open = false;
            if (row == 0u) {
                Port_RA_SetListSubset(0);
                open = true;
            } else {
                const RetroAchievementSubset* subset = Port_RA_GetSubset(row - 1u);
                if (subset) {
                    Port_RA_SetListSubset(subset->id);
                    open = true;
                }
            }
            if (open) {
                sShowAchPacksModal = false;
                sShowAchievementsModal = true;
                sAchievementsScrollY = 0.0f;
            }
        }
    }

    /* Handle tap selection on remap modal without accidental drag triggering */
    if (sCurrentTab == BOTTOM_TAB_OPTIONS && sShowRemapModal && sRemapSelectButtonIdx < 0) {
        if (!sIsTouchDragging && sTouchStartX >= 0 && sTouchStartY >= 0) {
            float viewY0 = 44.0f;
            float viewY1 = 202.0f;
            if (sTouchStartY >= (int)viewY0 && sTouchStartY <= (int)viewY1 && sTouchStartX >= 16 && sTouchStartX <= 300) {
                float contentY = viewY0 - sRemapScrollY;
                float relY = (float)sTouchStartY - contentY;
                int tapIdx = (int)(relY / 26.0f);
                if (tapIdx >= 0 && tapIdx < 10) {
                    /* Open action picker popup */
                    sRemapSelectButtonIdx = tapIdx;
                } else if (tapIdx == 10) {
                    /* Cycle C-Stick mode directly: 0=OFF, 1=AIM, 2=MOVEMENT, 3=ALL */
                    int mode = Port_Config_GetCstickMode();
                    Port_Config_SetCstickMode((mode + 1) % 4);
                }
            }
        }
    }

    sLastTouchX = -1;
    sLastTouchY = -1;
    sTouchStartX = -1;
    sTouchStartY = -1;
    sIsDragging = false;
    sIsTouchDragging = false;
    sAchScrollbarDrag = false;
}

/* Render Navigation Bar (3 or 4 tabs depending on debug visibility) */
static void RenderTabBar(void) {
    int lang = GetLang();
    const char* tabNames[7][4] = {
        { "MAP", "STATUS", "DEBUG", "OPTIONS" },  /* JP */
        { "MAP", "STATUS", "DEBUG", "OPTIONS" },  /* HIRA */
        { "MAP", "STATUS", "DEBUG", "OPTIONS" },  /* EN */
        { "KARTE", "STATUS", "DEBUG", "OPTIONEN" }, /* DE */
        { "CARTE", "STATUT", "DEBUG", "OPTIONS" },  /* FR */
        { "MAPPA", "STATO", "DEBUG", "OPZIONI" },  /* IT */
        { "MAPA", "ESTADO", "DEBUG", "OPCIONES" }  /* ES */
    };

    bool showDebug = Port_BottomUI_DebugTabVisible();
    int tabCount = showDebug ? 4 : 3;

    /* Distribute tabs evenly across 320px with small gaps */
    float totalGap = (float)(tabCount - 1) * 4.0f; /* 4px gap between tabs */
    float tabW = (320.0f - 8.0f - totalGap) / (float)tabCount; /* 4px margin each side */
    if (tabW > 80.0f) tabW = 80.0f;

    struct {
        float x;
        float w;
        int nameIdx; /* index into tabNames[][4] -- debug is always idx 2 */
        PortBottomTab tab;
    } tabs[4];

    float curX = 4.0f;
    tabs[0].x = curX; tabs[0].w = tabW; tabs[0].nameIdx = 0; tabs[0].tab = BOTTOM_TAB_MAP;      curX += tabW + 4.0f;
    tabs[1].x = curX; tabs[1].w = tabW; tabs[1].nameIdx = 1; tabs[1].tab = BOTTOM_TAB_STATUS;   curX += tabW + 4.0f;
    if (showDebug) {
        tabs[2].x = curX; tabs[2].w = tabW; tabs[2].nameIdx = 2; tabs[2].tab = BOTTOM_TAB_DEBUG; curX += tabW + 4.0f;
    }
    tabs[showDebug ? 3 : 2].x = curX;
    tabs[showDebug ? 3 : 2].w = tabW;
    tabs[showDebug ? 3 : 2].nameIdx = 3;
    tabs[showDebug ? 3 : 2].tab = BOTTOM_TAB_OPTIONS;

    /* Health tint: only active during real gameplay. Blinks to catch attention
     * even without looking directly at the screen. */
    bool inRealGameplay = ((uint8_t)gMainGameMode == 4 /* GM_INGAME */);
    uint16_t curE = gEquipment.currentEnergy;
    bool warnYellow = inRealGameplay && curE >= 30 && curE < 60;
    bool warnRed    = inRealGameplay && curE < 30;
    bool blinkOn    = ((sFrameCounter & 0x0F) < 8);

    for (int i = 0; i < tabCount; ++i) {
        bool active = (sCurrentTab == tabs[i].tab);
        uint32_t bg, border, textColor;

        if (warnRed) {
            if (blinkOn) {
                bg = active ? C2D_Color32(140, 28, 22, 255) : C2D_Color32(60, 18, 16, 255);
                border = active ? C2D_Color32(230, 55, 40, 255) : C2D_Color32(100, 30, 24, 255);
            } else {
                bg = active ? C2D_Color32(80, 18, 18, 255) : C2D_Color32(35, 12, 12, 255);
                border = active ? C2D_Color32(160, 38, 30, 255) : C2D_Color32(65, 22, 18, 255);
            }
            textColor = active ? C2D_Color32(255, 200, 190, 255) : C2D_Color32(200, 120, 110, 255);
        } else if (warnYellow) {
            if (blinkOn) {
                bg = active ? C2D_Color32(130, 100, 24, 255) : C2D_Color32(55, 44, 16, 255);
                border = active ? C2D_Color32(230, 180, 45, 255) : C2D_Color32(100, 80, 28, 255);
            } else {
                bg = active ? C2D_Color32(90, 70, 18, 255) : C2D_Color32(38, 32, 12, 255);
                border = active ? C2D_Color32(170, 130, 35, 255) : C2D_Color32(70, 58, 22, 255);
            }
            textColor = active ? C2D_Color32(255, 245, 200, 255) : C2D_Color32(200, 175, 110, 255);
        } else {
            bg = active ? C2D_Color32(18, 70, 130, 255) : C2D_Color32(26, 30, 42, 255);
            border = active ? C2D_Color32(45, 150, 240, 255) : C2D_Color32(50, 56, 75, 255);
            textColor = active ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(140, 150, 175, 255);
        }

        C2D_DrawRectSolid(tabs[i].x, 3.0f, 0.4f, tabs[i].w, 20.0f, border);
        C2D_DrawRectSolid(tabs[i].x + 1.0f, 4.0f, 0.5f, tabs[i].w - 2.0f, 18.0f, bg);
        DrawTextCentered(tabs[i].x + tabs[i].w / 2.0f, 9.0f, 1.0f, tabNames[lang][tabs[i].nameIdx], textColor);
    }
}

/* Decodes and draws an authentic 8x8 4bpp GBA minimap tile with correct ROM palette */
static void DrawAuthenticMapTile(float dstX, float dstY, float tileSize, uint16_t tileData,
                                float clipX0, float clipY0, float clipX1, float clipY1, float zDepth) {
    if (tileData == 0 || tileData == 0x140) return;

    uint32_t rawTileId = tileData & 0x3FF;
    bool xFlip = (tileData & 0x400) != 0;
    bool yFlip = (tileData & 0x800) != 0;
    uint32_t palId = (tileData >> 12) & 0xF;
    if (palId >= 5) palId = 0;

    const uint8_t* tileGfx = NULL;

    /* p_sMinimapTilesGfx points to the continuous tile graphic ROM array */
    if (rawTileId < 400 && p_sMinimapTilesGfx) {
        tileGfx = &((const uint8_t*)*p_sMinimapTilesGfx)[rawTileId * 32];
    } else if (rawTileId >= 0x200 && rawTileId < (0x200 + 400) && p_sMinimapTilesGfx) {
        tileGfx = &((const uint8_t*)*p_sMinimapTilesGfx)[(rawTileId - 0x200) * 32];
    }

    /* Fallback if tile graphic could not be found */
    if (!tileGfx || !p_sMinimapTilesPal) {
        float rx0 = dstX < clipX0 ? clipX0 : dstX;
        float ry0 = dstY < clipY0 ? clipY0 : dstY;
        float rx1 = (dstX + tileSize) > clipX1 ? clipX1 : (dstX + tileSize);
        float ry1 = (dstY + tileSize) > clipY1 ? clipY1 : (dstY + tileSize);
        if (rx1 > rx0 && ry1 > ry0) {
            uint32_t fbCol = (palId == 1) ? C2D_Color32(30, 140, 60, 255) : C2D_Color32(20, 50, 110, 255);
            C2D_DrawRectSolid(rx0, ry0, zDepth, rx1 - rx0, ry1 - ry0, fbCol);
        }
        return;
    }

    const uint16_t* palette = &(*p_sMinimapTilesPal)[palId * 16];
    float pixelSize = tileSize / 8.0f;

    for (int row = 0; row < 8; ++row) {
        int r = yFlip ? (7 - row) : row;
        const uint8_t* rowBytes = &tileGfx[r * 4];
        float py0 = dstY + (float)row * pixelSize;
        float py1 = py0 + pixelSize;
        if (py1 <= clipY0 || py0 >= clipY1) continue;

        for (int col = 0; col < 8;) {
            int c = xFlip ? (7 - col) : col;
            uint8_t byteVal = rowBytes[c / 2];
            uint8_t colorIdx = (c & 1) ? (byteVal >> 4) : (byteVal & 0xF);

            if (colorIdx == 0) {
                col++;
                continue;
            }

            int startCol = col;
            col++;
            while (col < 8) {
                int nextC = xFlip ? (7 - col) : col;
                uint8_t nextByte = rowBytes[nextC / 2];
                uint8_t nextColor = (nextC & 1) ? (nextByte >> 4) : (nextByte & 0xF);
                if (nextColor != colorIdx) break;
                col++;
            }

            float px0 = dstX + (float)startCol * pixelSize;
            float px1 = dstX + (float)col * pixelSize;
            if (px1 <= clipX0 || px0 >= clipX1) continue;

            float drawX0 = px0 < clipX0 ? clipX0 : px0;
            float drawX1 = px1 > clipX1 ? clipX1 : px1;
            float drawY0 = py0 < clipY0 ? clipY0 : py0;
            float drawY1 = py1 > clipY1 ? clipY1 : py1;

            uint32_t color = Bgr555ToRgba8(palette[colorIdx], false);
            C2D_DrawRectSolid(drawX0, drawY0, zDepth, drawX1 - drawX0, drawY1 - drawY0, color);
        }
    }
}

/* Authentic HUD Item Sprites */
static void DrawEnergyIcon(float x, float y) {
    C2D_DrawRectSolid(x, y, 0.6f, 14.0f, 10.0f, C2D_Color32(30, 45, 70, 255));
    C2D_DrawRectSolid(x + 1.0f, y + 1.0f, 0.65f, 12.0f, 8.0f, C2D_Color32(10, 20, 35, 255));
    C2D_DrawRectSolid(x, y + 2.0f, 0.7f, 2.0f, 6.0f, C2D_Color32(180, 195, 220, 255));
    C2D_DrawRectSolid(x + 12.0f, y + 2.0f, 0.7f, 2.0f, 6.0f, C2D_Color32(180, 195, 220, 255));
    C2D_DrawRectSolid(x + 4.0f, y + 2.0f, 0.75f, 6.0f, 2.0f, C2D_Color32(255, 60, 140, 255));
    C2D_DrawRectSolid(x + 4.0f, y + 4.0f, 0.75f, 4.0f, 2.0f, C2D_Color32(255, 120, 180, 255));
    C2D_DrawRectSolid(x + 4.0f, y + 6.0f, 0.75f, 6.0f, 2.0f, C2D_Color32(255, 60, 140, 255));
    C2D_DrawRectSolid(x + 3.0f, y + 2.0f, 0.75f, 2.0f, 6.0f, C2D_Color32(255, 200, 220, 255));
}

static void DrawMissileIcon(float x, float y) {
    C2D_DrawRectSolid(x + 1.0f, y + 3.0f, 0.6f, 3.0f, 4.0f, C2D_Color32(220, 230, 240, 255));
    C2D_DrawRectSolid(x + 3.0f, y + 2.0f, 0.65f, 2.0f, 6.0f, C2D_Color32(160, 180, 200, 255));
    C2D_DrawRectSolid(x + 5.0f, y + 2.0f, 0.6f, 7.0f, 6.0f, C2D_Color32(230, 35, 35, 255));
    C2D_DrawRectSolid(x + 6.0f, y + 3.0f, 0.65f, 5.0f, 2.0f, C2D_Color32(255, 120, 120, 255));
    C2D_DrawRectSolid(x + 10.0f, y, 0.6f, 3.0f, 2.0f, C2D_Color32(180, 195, 215, 255));
    C2D_DrawRectSolid(x + 10.0f, y + 8.0f, 0.6f, 3.0f, 2.0f, C2D_Color32(180, 195, 215, 255));
}

static void DrawSuperMissileIcon(float x, float y) {
    C2D_DrawRectSolid(x + 1.0f, y + 3.0f, 0.6f, 3.0f, 4.0f, C2D_Color32(240, 255, 220, 255));
    C2D_DrawRectSolid(x + 3.0f, y + 2.0f, 0.65f, 2.0f, 6.0f, C2D_Color32(180, 220, 160, 255));
    C2D_DrawRectSolid(x + 5.0f, y + 2.0f, 0.6f, 7.0f, 6.0f, C2D_Color32(30, 210, 60, 255));
    C2D_DrawRectSolid(x + 6.0f, y + 3.0f, 0.65f, 5.0f, 2.0f, C2D_Color32(140, 255, 160, 255));
    C2D_DrawRectSolid(x + 10.0f, y, 0.6f, 3.0f, 2.0f, C2D_Color32(20, 140, 45, 255));
    C2D_DrawRectSolid(x + 10.0f, y + 8.0f, 0.6f, 3.0f, 2.0f, C2D_Color32(20, 140, 45, 255));
}

static void DrawPowerBombIcon(float x, float y) {
    C2D_DrawRectSolid(x + 2.0f, y, 0.6f, 8.0f, 10.0f, C2D_Color32(255, 180, 0, 255));
    C2D_DrawRectSolid(x, y + 2.0f, 0.6f, 12.0f, 6.0f, C2D_Color32(255, 180, 0, 255));
    C2D_DrawRectSolid(x + 2.0f, y + 2.0f, 0.65f, 8.0f, 6.0f, C2D_Color32(255, 240, 80, 255));
    C2D_DrawRectSolid(x + 4.0f, y + 3.0f, 0.7f, 4.0f, 4.0f, C2D_Color32(255, 100, 0, 255));
    C2D_DrawRectSolid(x + 5.0f, y + 4.0f, 0.75f, 2.0f, 2.0f, C2D_Color32(255, 255, 255, 255));
}

static const char* Port_Config_GetLanguageDisplayName(int lang) {
    switch (lang) {
        case 0: return "0: JAPANESE (KANJI)";
        case 1: return "1: JAPANESE (HIRAGANA)";
        case 2: return "2: ENGLISH";
        case 3: return "3: DEUTSCH";
        case 4: return "4: FRANÇAIS";
        case 5: return "5: ITALIANO";
        case 6: return "6: ESPAÑOL";
        default: return "2: ENGLISH";
    }
}

static void Port_Config_CycleLanguage(void) {
    extern void SramWrite_Language(void);
    gLanguage = (gLanguage + 1) % LANGUAGE_COUNT;
    SramWrite_Language();
    Port_Config_Save();
}

static const char* GetAspectRatioDisplayName(int lang) {
    int ar = Port_Config_Get3DSAspectRatio();
    switch (lang) {
        case 0:
        case 1:
            switch (ar) {
                case 0: return "WIDE";
                case 1: return "ORIGINAL (3:2)";
                case 2: return "STRETCH (16:9)";
                default: return "ORIGINAL";
            }
        case 3: /* DE */
            switch (ar) {
                case 0: return "BREITBILD";
                case 1: return "ORIGINAL (3:2)";
                case 2: return "GESTRECKT (16:9)";
                default: return "ORIGINAL";
            }
        case 4: /* FR */
            switch (ar) {
                case 0: return "LARGE";
                case 1: return "ORIGINAL (3:2)";
                case 2: return "ETIRE (16:9)";
                default: return "ORIGINAL";
            }
        case 5: /* IT */
            switch (ar) {
                case 0: return "PANORAMICO";
                case 1: return "ORIGINALE (3:2)";
                case 2: return "ALLARGATO (16:9)";
                default: return "ORIGINALE";
            }
        case 6: /* ES */
        default: /* EN */
            switch (ar) {
                case 0: return "PANORAMICO";
                case 1: return "ORIGINAL (3:2)";
                case 2: return "ESTIRADO (16:9)";
                default: return "ORIGINAL";
            }
    }
}

static const char* GetDisplayStyleDisplayName(int lang) {
    int ds = Port_Config_Get3DSDisplayStyle();
    switch (lang) {
        case 0:
        case 1:
            switch (ds) {
                case 0: return "PIXEL PERFECT (1:1)";
                case 1: return "SCALED (SHARP)";
                case 2: return "BLUR (SMOOTH)";
                default: return "SCALED";
            }
        case 3: /* DE */
            switch (ds) {
                case 0: return "PIXELGENAU (1:1)";
                case 1: return "SCHARF";
                case 2: return "GEGLAETTET";
                default: return "SKALIERT";
            }
        case 4: /* FR */
            switch (ds) {
                case 0: return "PIXEL PARFAIT (1:1)";
                case 1: return "NET (SHARP)";
                case 2: return "LISSE (SMOOTH)";
                default: return "ECHELLE";
            }
        case 5: /* IT */
            switch (ds) {
                case 0: return "PIXEL PERFECT (1:1)";
                case 1: return "NITIDO (SHARP)";
                case 2: return "SMUSSATO (SMOOTH)";
                default: return "SCALATO";
            }
        case 6: /* ES */
            switch (ds) {
                case 0: return "PIXEL PERFECT (1:1)";
                case 1: return "ESCALADO NITIDO";
                case 2: return "SUAVIZADO";
                default: return "ESCALADO";
            }
        default: /* EN */
            switch (ds) {
                case 0: return "PIXEL PERFECT (1:1)";
                case 1: return "SCALED (SHARP)";
                case 2: return "BLUR (SMOOTH)";
                default: return "SCALED";
            }
    }
}

static const char* GetStereoDepthDisplayName(int lang) {
    return PortStereoDepth_SpreadNameLang(PortStereoDepth_GetSpread(), lang);
}

static const char* GetFpsOverlayDisplayName(int lang) {
    bool on = Port_Config_GetShowFps();
    switch (lang) {
        case 0:
        case 1: return on ? "ON" : "OFF";
        case 3: return on ? "EIN" : "AUS";
        case 4: return on ? "ACTIVE" : "DESACTIVE";
        case 5: return on ? "ATTIVO" : "DISATTIVO";
        case 6: return on ? "ACTIVADO" : "DESACTIVADO";
        default: return on ? "ON" : "OFF";
    }
}

struct AreaItemStats {
    uint8_t energyObtained;
    uint8_t energyTotal;
    uint8_t missileObtained;
    uint8_t missileTotal;
    uint8_t superObtained;
    uint8_t superTotal;
    uint8_t powerBombObtained;
    uint8_t powerBombTotal;
    uint8_t totalObtained;
    uint8_t totalItems;
};

static const uint8_t sTotalTanksTable[7][4] = {
    { 3, 10, 1, 0 }, /* Brinstar */
    { 2,  9, 0, 0 }, /* Kraid */
    { 1, 13, 2, 1 }, /* Norfair */
    { 3, 13, 3, 0 }, /* Ridley */
    { 0,  1, 0, 1 }, /* Tourian */
    { 0,  3, 1, 1 }, /* Crateria */
    { 3,  1, 8, 6 }, /* Chozodia */
};

extern void Port_GetAreaItemTypeCounts(int area, int* outEnergy, int* outMissile, int* outSuper, int* outPowerBomb);

static void GetAreaStats(int area, struct AreaItemStats* outStats) {
    if (!outStats) return;
    memset(outStats, 0, sizeof(struct AreaItemStats));
    if (area < 0 || area >= 7) return;

    outStats->energyTotal = sTotalTanksTable[area][0];
    outStats->missileTotal = sTotalTanksTable[area][1];
    outStats->superTotal = sTotalTanksTable[area][2];
    outStats->powerBombTotal = sTotalTanksTable[area][3];
    outStats->totalItems = outStats->energyTotal + outStats->missileTotal + outStats->superTotal + outStats->powerBombTotal;

    int energyGot = 0, missileGot = 0, superGot = 0, powerBombGot = 0;
    Port_GetAreaItemTypeCounts(area, &energyGot, &missileGot, &superGot, &powerBombGot);
    outStats->energyObtained = (uint8_t)energyGot;
    outStats->missileObtained = (uint8_t)missileGot;
    outStats->superObtained = (uint8_t)superGot;
    outStats->powerBombObtained = (uint8_t)powerBombGot;

    /* Total = sum of the four tank/ammo types, so it always matches the
     * per-type breakdown above (the map's obtained-items bitmap also logs
     * major items, which are not part of these totals). */
    outStats->totalObtained = (uint8_t)(energyGot + missileGot + superGot + powerBombGot);
}

static void GetGlobalItemStats(struct AreaItemStats* outStats) {
    if (!outStats) return;
    memset(outStats, 0, sizeof(struct AreaItemStats));

    outStats->energyTotal = 12;
    outStats->missileTotal = 50;
    outStats->superTotal = 15;
    outStats->powerBombTotal = 9;
    outStats->totalItems = 86;

    for (int a = 0; a < 7; ++a) {
        struct AreaItemStats aStats;
        GetAreaStats(a, &aStats);
        outStats->totalObtained += aStats.totalObtained;
        outStats->energyObtained += aStats.energyObtained;
        outStats->missileObtained += aStats.missileObtained;
        outStats->superObtained += aStats.superObtained;
        outStats->powerBombObtained += aStats.powerBombObtained;
    }
}

/* Render Collectibles Per-Area Breakdown Modal */
static void RenderCollectiblesModal(int lang) {
    C2D_DrawRectSolid(10.0f, 26.0f, 0.85f, 300.0f, 206.0f, C2D_Color32(10, 14, 24, 252));
    C2D_DrawRectSolid(10.0f, 26.0f, 0.84f, 300.0f, 206.0f, C2D_Color32(40, 70, 120, 255));

    /* Hardcore mode or Hide Spoilers: only show what has already been collected -- never totals
     * or remaining counts (that would leak achievement-relevant info / spoilers). */
    bool hideSpoilers = Port_RA_IsHardcore() || Port_Config_GetHideSpoilers();

    struct AreaItemStats globalStats;
    GetGlobalItemStats(&globalStats);
    char globTitle[64];
    static const char* const collTitles[7] = {
        "COLLECTIBLES BY AREA", "COLLECTIBLES BY AREA", "COLLECTIBLES BY AREA",
        "OBJEKTE NACH GEBIET", "OBJETS PAR REGION", "OGGETTI PER AREA", "COLECCIONABLES POR ZONA"
    };
    if (hideSpoilers) {
        snprintf(globTitle, sizeof(globTitle), "%s: %u", collTitles[lang],
                 globalStats.totalObtained);
    } else {
        unsigned pct = globalStats.totalItems > 0 ? (globalStats.totalObtained * 100 / globalStats.totalItems) : 0;
        snprintf(globTitle, sizeof(globTitle), "%s: %u/%u (%u%%)", collTitles[lang],
                 globalStats.totalObtained, globalStats.totalItems, pct);
    }
    DrawText(18.0f, 32.0f, 1.0f, globTitle, C2D_Color32(255, 215, 0, 255));

    /* Table Headers */
    static const char* const areaHeaderTitles[7] = {
        "AREA", "AREA", "AREA",
        "GEBIET", "REGION", "AREA", "ZONA"
    };
    DrawText(18.0f, 48.0f, 1.0f, areaHeaderTitles[lang], C2D_Color32(100, 220, 255, 255));
    DrawEnergyIcon(108.0f, 46.0f);
    DrawMissileIcon(148.0f, 46.0f);
    DrawSuperMissileIcon(193.0f, 46.0f);
    DrawPowerBombIcon(233.0f, 46.0f);
    if (!hideSpoilers) DrawText(272.0f, 48.0f, 1.0f, "TOT", C2D_Color32(255, 255, 255, 255));

    for (int a = 0; a < 7; ++a) {
        struct AreaItemStats aStats;
        GetAreaStats(a, &aStats);
        float py = 64.0f + (float)a * 19.0f;

        C2D_DrawRectSolid(16.0f, py, 0.88f, 288.0f, 17.0f, (a % 2 == 0) ? C2D_Color32(20, 26, 40, 255) : C2D_Color32(14, 18, 30, 255));

        DrawText(20.0f, py + 3.0f, 1.0f, AreaName(a), C2D_Color32(220, 235, 255, 255));

        char cBuf[16];
        if (hideSpoilers) {
            snprintf(cBuf, sizeof(cBuf), "%u", aStats.energyObtained);
            DrawText(115.0f, py + 3.0f, 1.0f, cBuf, aStats.energyObtained > 0 ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(90, 100, 120, 255));

            snprintf(cBuf, sizeof(cBuf), "%u", aStats.missileObtained);
            DrawText(155.0f, py + 3.0f, 1.0f, cBuf, aStats.missileObtained > 0 ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(90, 100, 120, 255));

            snprintf(cBuf, sizeof(cBuf), "%u", aStats.superObtained);
            DrawText(200.0f, py + 3.0f, 1.0f, cBuf, aStats.superObtained > 0 ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(90, 100, 120, 255));

            snprintf(cBuf, sizeof(cBuf), "%u", aStats.powerBombObtained);
            DrawText(240.0f, py + 3.0f, 1.0f, cBuf, aStats.powerBombObtained > 0 ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(90, 100, 120, 255));
        } else {
            snprintf(cBuf, sizeof(cBuf), "%u/%u", aStats.energyObtained, aStats.energyTotal);
            DrawText(105.0f, py + 3.0f, 1.0f, cBuf, aStats.energyObtained == aStats.energyTotal ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 215, 0, 255));

            snprintf(cBuf, sizeof(cBuf), "%u/%u", aStats.missileObtained, aStats.missileTotal);
            DrawText(145.0f, py + 3.0f, 1.0f, cBuf, aStats.missileObtained == aStats.missileTotal ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 140, 140, 255));

            snprintf(cBuf, sizeof(cBuf), "%u/%u", aStats.superObtained, aStats.superTotal);
            DrawText(190.0f, py + 3.0f, 1.0f, cBuf, aStats.superObtained == aStats.superTotal ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(100, 255, 140, 255));

            snprintf(cBuf, sizeof(cBuf), "%u/%u", aStats.powerBombObtained, aStats.powerBombTotal);
            DrawText(230.0f, py + 3.0f, 1.0f, cBuf, aStats.powerBombObtained == aStats.powerBombTotal ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 225, 80, 255));

            snprintf(cBuf, sizeof(cBuf), "%u/%u", aStats.totalObtained, aStats.totalItems);
            DrawText(268.0f, py + 3.0f, 1.0f, cBuf, aStats.totalObtained == aStats.totalItems ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 255, 255, 255));
        }
    }

    static const char* const closeCollectiblesLabels[7] = {
        "CLOSE", "CLOSE", "CLOSE",
        "SCHLIESSEN", "FERMER", "CHIUDI", "CERRAR"
    };
    C2D_DrawRectSolid(100.0f, 204.0f, 0.9f, 120.0f, 20.0f, C2D_Color32(20, 70, 130, 255));
    DrawTextCentered(160.0f, 209.0f, 1.0f, closeCollectiblesLabels[lang], C2D_Color32(255, 255, 255, 255));
}

static const char* AchSortLabel(int lang) {
    /* [sort mode][lang]; langs 0-2 English, 3 DE, 4 FR, 5 IT, 6 ES. */
    static const char* labels[RA_SORT_COUNT][7] = {
        { "DEFAULT", "DEFAULT", "DEFAULT", "STANDARD", "DEFAUT", "PREDEFINITO", "POR DEFECTO" },
        { "A-Z", "A-Z", "A-Z", "A-Z", "A-Z", "A-Z", "A-Z" },
        { "POINTS", "POINTS", "POINTS", "PUNKTE", "POINTS", "PUNTI", "PUNTOS" },
        { "RECENT", "RECENT", "RECENT", "NEUESTE", "RECENTS", "RECENTI", "RECIENTES" },
    };
    RetroAchievementSort sort = Port_RA_GetListSort();
    if (sort >= RA_SORT_COUNT) sort = RA_SORT_DEFAULT;
    return labels[sort][lang];
}

/* Pack chooser: one card per set plus an "all sets" entry at the top. */
static void RenderAchPacksModal(int lang) {
    const float mX = 10.0f;
    const float mY = 26.0f;
    const float mW = 300.0f;
    const float mH = 206.0f;
    const float clipY0 = ACH_LIST_CLIP_Y0;
    const float clipY1 = ACH_LIST_CLIP_Y1;

    C2D_DrawRectSolid(mX, mY, 0.85f, mW, mH, C2D_Color32(10, 14, 24, 250));
    C2D_DrawRectSolid(mX, mY, 0.84f, mW, mH, C2D_Color32(40, 70, 120, 255));

    static const char* titles[7] = {
        "ACHIEVEMENT SETS", "ACHIEVEMENT SETS", "ACHIEVEMENT SETS",
        "ERFOLGSSAETZE", "SERIES DE SUCCES", "SERIE DI OBIETTIVI", "SERIES DE LOGROS"
    };
    DrawText(20.0f, 32.0f, 1.0f, titles[lang], C2D_Color32(255, 215, 0, 255));

    static const char* allLabels[7] = {
        "ALL SETS", "ALL SETS", "ALL SETS",
        "ALLE SAETZE", "TOUTES LES SERIES", "TUTTE LE SERIE", "TODAS LAS SERIES"
    };

    float maxScroll = AchPacksMaxScroll();
    if (sAchPacksScrollY > maxScroll) sAchPacksScrollY = maxScroll;

    uint32_t subsetCount = Port_RA_GetSubsetCount();
    for (uint32_t row = 0; row <= subsetCount; ++row) {
        float py = clipY0 - sAchPacksScrollY + (float)row * ACH_PACK_CARD_H;
        if (py + ACH_PACK_CARD_H <= clipY0 || py >= clipY1) continue;

        const char* name;
        uint32_t total, unlocked, hardcore;
        if (row == 0) {
            name = allLabels[lang];
            total = Port_RA_GetAchievementCount();
            unlocked = Port_RA_GetUnlockedCount();
            hardcore = Port_RA_GetHardcoreUnlockedCount();
        } else {
            const RetroAchievementSubset* subset = Port_RA_GetSubset(row - 1u);
            if (!subset) continue;
            name = subset->title;
            total = subset->total;
            unlocked = subset->unlocked;
            hardcore = subset->hardcoreUnlocked;
        }

        bool complete = (total > 0u && unlocked >= total);
        uint32_t borderCol = complete ? C2D_Color32(160, 130, 30, 255) : C2D_Color32(35, 45, 65, 255);
        uint32_t bgCol = complete ? C2D_Color32(40, 36, 16, 255) : C2D_Color32(18, 22, 34, 255);
        DrawRectClipped(16.0f, py, 0.88f, 284.0f, ACH_PACK_CARD_H - 4.0f, borderCol, clipY0, clipY1);
        DrawRectClipped(17.0f, py + 1.0f, 0.89f, 282.0f, ACH_PACK_CARD_H - 6.0f, bgCol, clipY0, clipY1);

        DrawTextMaxWClipped(24.0f, py + 6.0f, 1.0f, name,
                            complete ? C2D_Color32(255, 225, 80, 255) : C2D_Color32(220, 235, 255, 255),
                            clipY0, clipY1, 270.0f);

        char progressBuf[48];
        if (hardcore > 0u) {
            snprintf(progressBuf, sizeof(progressBuf), "%u/%u (%u HC)",
                     (unsigned)unlocked, (unsigned)total, (unsigned)hardcore);
        } else {
            snprintf(progressBuf, sizeof(progressBuf), "%u/%u", (unsigned)unlocked, (unsigned)total);
        }
        DrawTextMaxWClipped(24.0f, py + 20.0f, 1.0f, progressBuf,
                            complete ? C2D_Color32(255, 215, 0, 255) : C2D_Color32(80, 255, 120, 255),
                            clipY0, clipY1, 270.0f);
    }

    if (maxScroll > 0.0f) {
        float trackH = (clipY1 - clipY0);
        float thumbH = 30.0f;
        float thumbY = clipY0 + (sAchPacksScrollY / maxScroll) * (trackH - thumbH);
        C2D_DrawRectSolid(306.0f, clipY0, 0.92f, 2.0f, trackH, C2D_Color32(255, 35, 55, 255));
        C2D_DrawRectSolid(306.0f, thumbY, 0.94f, 2.0f, thumbH, C2D_Color32(80, 160, 240, 255));
    }

    static const char* backLabels[7] = {
        "BACK", "BACK", "BACK", "ZURUECK", "RETOUR", "INDIETRO", "VOLVER"
    };
    C2D_DrawRectSolid(100.0f, 204.0f, 0.96f, 120.0f, 20.0f, C2D_Color32(20, 70, 130, 255));
    DrawTextCentered(160.0f, 209.0f, 1.0f, backLabels[lang], C2D_Color32(255, 255, 255, 255));
}

/* Render RetroAchievements List Modal with Touch Scrolling */
static void RenderAchievementsModal(int lang) {
    const float mX = 10.0f;
    const float mY = 26.0f;
    const float mW = 300.0f;
    const float mH = 206.0f;
    const float clipY0 = ACH_LIST_CLIP_Y0;
    const float clipY1 = ACH_LIST_CLIP_Y1;

    C2D_DrawRectSolid(mX, mY, 0.85f, mW, mH, C2D_Color32(10, 14, 24, 250));
    C2D_DrawRectSolid(mX, mY, 0.84f, mW, mH, C2D_Color32(40, 70, 120, 255));

    /* Header: the set's name when the list is filtered to one, the generic
     * title when it is showing everything. */
    const char* titles[7] = {
        "RETROACHIEVEMENTS LIST", "RETROACHIEVEMENTS LIST", "RETROACHIEVEMENTS LIST",
        "ERFOLGSLISTE", "LISTE DES SUCCES", "LISTA DEGLI OBIETTIVI", "LISTA DE LOGROS"
    };
    const char* header = titles[lang];
    uint32_t filter = Port_RA_GetListSubset();
    if (filter != 0u) {
        for (uint32_t i = 0; i < Port_RA_GetSubsetCount(); ++i) {
            const RetroAchievementSubset* subset = Port_RA_GetSubset(i);
            if (subset && subset->id == filter) { header = subset->title; break; }
        }
    }
    DrawTextMaxWClipped(20.0f, 32.0f, 1.0f, header, C2D_Color32(255, 215, 0, 255), 26.0f, 46.0f, 130.0f);

    /* Counters follow the filter, so a set's card and its list agree. */
    uint32_t count = Port_RA_GetViewCount();
    uint32_t unlocked = 0, hardcore = 0, totalPts = 0, unlPts = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const RetroAchievementItem* ach = Port_RA_GetViewAchievement(i);
        if (!ach) continue;
        totalPts += ach->points;
        if (ach->unlocked) { ++unlocked; unlPts += ach->points; }
        if (ach->hardcoreUnlocked) ++hardcore;
    }

    char summaryBuf[64];
    if (hardcore > 0) {
        snprintf(summaryBuf, sizeof(summaryBuf), "%u/%u (%u HC) %uP", (unsigned)unlocked, (unsigned)count, (unsigned)hardcore, (unsigned)unlPts);
    } else {
        snprintf(summaryBuf, sizeof(summaryBuf), "%u/%u (%u/%u PTS)", (unsigned)unlocked, (unsigned)count, (unsigned)unlPts, (unsigned)totalPts);
    }
    DrawText(156.0f, 32.0f, 1.0f, summaryBuf, hardcore > 0 ? C2D_Color32(255, 215, 0, 255) : C2D_Color32(80, 255, 120, 255));

    float cardH = ACH_CARD_H;
    float maxAchScroll = AchievementsMaxScroll();
    if (sAchievementsScrollY > maxAchScroll) sAchievementsScrollY = maxAchScroll;

    for (uint32_t i = 0; i < count; ++i) {
        const RetroAchievementItem* ach = Port_RA_GetViewAchievement(i);
        if (!ach) continue;

        float py = clipY0 - sAchievementsScrollY + (float)i * cardH;
        if (py + cardH <= clipY0 || py >= clipY1) continue;

        /* Card background */
        uint32_t bgCol = ach->hardcoreUnlocked ? C2D_Color32(40, 36, 16, 255) :
                         (ach->unlocked ? C2D_Color32(16, 38, 26, 255) : C2D_Color32(18, 22, 34, 255));
        uint32_t borderCol = ach->hardcoreUnlocked ? C2D_Color32(160, 130, 30, 255) :
                             (ach->unlocked ? C2D_Color32(35, 120, 65, 255) : C2D_Color32(35, 45, 65, 255));

        DrawRectClipped(16.0f, py, 0.88f, 284.0f, cardH - 2.0f, borderCol, clipY0, clipY1);
        DrawRectClipped(17.0f, py + 1.0f, 0.89f, 282.0f, cardH - 4.0f, bgCol, clipY0, clipY1);

        /* 1. Badge / Icon on Left (X: 19, Y: py + 3, W: 24, H: 24) */
        float iconX = 19.0f;
        float iconY = py + 3.0f;
        if (iconY + 24.0f > clipY0 && iconY < clipY1) {
            uint32_t iconBorder = ach->hardcoreUnlocked ? C2D_Color32(255, 215, 0, 255) :
                                  (ach->unlocked ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(60, 75, 100, 255));
            DrawRectClipped(iconX, iconY, 0.91f, 24.0f, 24.0f, iconBorder, clipY0, clipY1);
            DrawRectClipped(iconX + 1.0f, iconY + 1.0f, 0.92f, 22.0f, 22.0f, C2D_Color32(12, 16, 24, 255), clipY0, clipY1);

            const uint32_t* badgePixels = Port_RA_GetBadgePixels(ach->badgeName);
            if (badgePixels) {
                float badgeX = iconX + 2.0f;
                float badgeY = iconY + 2.0f;
                for (int by = 0; by < 20; ++by) {
                    float rY = badgeY + (float)by;
                    if (rY < clipY0 || rY >= clipY1) continue;
                    for (int bx = 0; bx < 20; ++bx) {
                        uint32_t pColor = badgePixels[by * 20 + bx];
                        if (!ach->unlocked && !ach->hardcoreUnlocked) {
                            uint32_t r = pColor & 0xFF;
                            uint32_t g = (pColor >> 8) & 0xFF;
                            uint32_t b = (pColor >> 16) & 0xFF;
                            uint32_t gray = (r * 30 + g * 59 + b * 11) / 250;
                            pColor = C2D_Color32((uint8_t)gray, (uint8_t)gray, (uint8_t)gray, 255);
                        }
                        C2D_DrawRectSolid(badgeX + (float)bx, rY, 0.93f, 1.0f, 1.0f, pColor);
                    }
                }
            } else {
                if (ach->hardcoreUnlocked) {
                    DrawRectClipped(iconX + 4.0f, iconY + 4.0f, 0.93f, 14.0f, 14.0f, C2D_Color32(255, 200, 50, 255), clipY0, clipY1);
                    DrawTextMaxWClipped(iconX + 8.0f, iconY + 7.0f, 1.0f, "H", C2D_Color32(20, 20, 20, 255), clipY0, clipY1, 8.0f);
                } else if (ach->unlocked) {
                    DrawRectClipped(iconX + 4.0f, iconY + 4.0f, 0.93f, 14.0f, 14.0f, C2D_Color32(60, 200, 100, 255), clipY0, clipY1);
                    DrawTextMaxWClipped(iconX + 8.0f, iconY + 7.0f, 1.0f, "*", C2D_Color32(255, 255, 255, 255), clipY0, clipY1, 8.0f);
                } else {
                    DrawRectClipped(iconX + 4.0f, iconY + 4.0f, 0.93f, 14.0f, 14.0f, C2D_Color32(40, 50, 70, 255), clipY0, clipY1);
                    DrawTextMaxWClipped(iconX + 8.0f, iconY + 7.0f, 1.0f, "?", C2D_Color32(120, 140, 170, 255), clipY0, clipY1, 8.0f);
                }
            }
        }

        /* 2. Graphical Status Indicator on Top-Right */
        float statX = 286.0f;
        float statY = py + 4.0f;
        if (statY + 8.0f > clipY0 && statY < clipY1) {
            if (ach->hardcoreUnlocked) {
                /* Hardcore Mini Crown (Gold) */
                uint32_t cGold = C2D_Color32(255, 215, 0, 255);
                DrawRectClipped(statX, statY + 3.0f, 0.93f, 8.0f, 4.0f, cGold, clipY0, clipY1);
                DrawRectClipped(statX + 1.0f, statY + 1.0f, 0.93f, 2.0f, 2.0f, cGold, clipY0, clipY1);
                DrawRectClipped(statX + 3.0f, statY, 0.93f, 2.0f, 2.0f, cGold, clipY0, clipY1);
                DrawRectClipped(statX + 5.0f, statY + 1.0f, 0.93f, 2.0f, 2.0f, cGold, clipY0, clipY1);
            } else if (ach->unlocked) {
                /* Softcore Unlocked Mini Padlock Open / Checkmark (Green) */
                uint32_t cGreen = C2D_Color32(80, 255, 120, 255);
                /* Open shackle */
                DrawRectClipped(statX + 3.0f, statY, 0.93f, 4.0f, 1.0f, cGreen, clipY0, clipY1);
                DrawRectClipped(statX + 6.0f, statY + 1.0f, 0.93f, 1.0f, 2.0f, cGreen, clipY0, clipY1);
                /* Body */
                DrawRectClipped(statX + 1.0f, statY + 3.0f, 0.93f, 6.0f, 4.0f, cGreen, clipY0, clipY1);
            } else {
                /* Locked Mini Padlock Closed (Slate Gray) */
                uint32_t cLock = C2D_Color32(110, 130, 160, 255);
                /* Closed shackle */
                DrawRectClipped(statX + 2.0f, statY, 0.93f, 4.0f, 1.0f, cLock, clipY0, clipY1);
                DrawRectClipped(statX + 2.0f, statY + 1.0f, 0.93f, 1.0f, 2.0f, cLock, clipY0, clipY1);
                DrawRectClipped(statX + 5.0f, statY + 1.0f, 0.93f, 1.0f, 2.0f, cLock, clipY0, clipY1);
                /* Body */
                DrawRectClipped(statX + 1.0f, statY + 3.0f, 0.93f, 6.0f, 4.0f, cLock, clipY0, clipY1);
            }
        }

        /* 3. Title + Points (Full width up to statX - 6.0f = 232px) */
        char titleBuf[80];
        snprintf(titleBuf, sizeof(titleBuf), "%s (%uP)", ach->title, (unsigned)ach->points);
        uint32_t titleCol = ach->hardcoreUnlocked ? C2D_Color32(255, 225, 80, 255) :
                            (ach->unlocked ? C2D_Color32(140, 240, 170, 255) : C2D_Color32(220, 235, 255, 255));
        DrawTextMaxWClipped(48.0f, py + 4.0f, 1.0f, titleBuf, titleCol, clipY0, clipY1, 232.0f);
    }

    /* Achievements modal scrollbar */
    if (maxAchScroll > 0.0f) {
        float trackH = (clipY1 - clipY0);
        float thumbH = 30.0f;
        float thumbY = clipY0 + (sAchievementsScrollY / maxAchScroll) * (trackH - thumbH);
        C2D_DrawRectSolid(306.0f, clipY0, 0.92f, 2.0f, trackH, C2D_Color32(255, 35, 55, 255));
        C2D_DrawRectSolid(306.0f, thumbY, 0.94f, 2.0f, thumbH, C2D_Color32(80, 160, 240, 255));
    }

    const char* backLabels[7] = {
        "BACK", "BACK", "BACK",
        "ZURUECK", "RETOUR", "INDIETRO", "VOLVER"
    };
    C2D_DrawRectSolid(100.0f, 204.0f, 0.96f, 120.0f, 20.0f, C2D_Color32(20, 70, 130, 255));
    DrawTextCentered(160.0f, 209.0f, 1.0f, backLabels[lang], C2D_Color32(255, 255, 255, 255));

    /* Sort cycler plus its direction arrow, on the footer bar beside BACK so
     * neither can collide with the scrolling card band above them. */
    static const char* sortLabels[7] = {
        "SORT", "SORT", "SORT", "SORT", "TRI", "ORDINA", "ORDEN"
    };
    C2D_DrawRectSolid(224.0f, 204.0f, 0.96f, 62.0f, 20.0f, C2D_Color32(24, 40, 70, 255));
    DrawTextCentered(255.0f, 206.0f, 1.0f, sortLabels[lang], C2D_Color32(120, 150, 190, 255));
    DrawTextCentered(255.0f, 214.0f, 1.0f, AchSortLabel(lang), C2D_Color32(150, 200, 255, 255));

    /* Direction toggle: a chevron pointing down for descending (Z-A, most
     * points first, newest first) and up for ascending. Drawn as stacked
     * rows because this UI has no glyph for it. */
    bool descending = Port_RA_GetListDescending();
    C2D_DrawRectSolid(288.0f, 204.0f, 0.96f, 18.0f, 20.0f, C2D_Color32(24, 40, 70, 255));
    for (int row = 0; row < 4; ++row) {
        /* Widest row at the flat end, narrowest at the point. */
        int step = descending ? (3 - row) : row;
        float w = 2.0f + (float)step * 2.0f;
        float px = 297.0f - w * 0.5f;
        float py = 209.0f + (float)row * 2.0f;
        C2D_DrawRectSolid(px, py, 0.97f, w, 2.0f, C2D_Color32(150, 200, 255, 255));
    }
}

/* Render Confirmation Dialog (hardcore enable / restart game) */
static void RenderConfirmModal(int lang) {
    C2D_DrawRectSolid(10.0f, 26.0f, 0.85f, 300.0f, 206.0f, C2D_Color32(10, 14, 24, 250));
    C2D_DrawRectSolid(10.0f, 26.0f, 0.84f, 300.0f, 206.0f, C2D_Color32(40, 70, 120, 255));

    static const char* const restartTitles[7] = {
        "RESTART GAME", "RESTART GAME", "RESTART GAME",
        "SPIEL NEUSTARTEN", "RECOMMENCER PARTIE", "RIAVVIA PARTITA", "REINICIAR PARTIDA"
    };
    static const char* const hcTitles[7] = {
        "HARDCORE MODE", "HARDCORE MODE", "HARDCORE MODE",
        "HARDCORE-MODUS", "MODE HARDCORE", "MODO HARDCORE", "MODO HARDCORE"
    };
    const char* title = sConfirmIsRestart ? restartTitles[lang] : hcTitles[lang];
    DrawTextCentered(160.0f, 60.0f, 1.0f, title, C2D_Color32(255, 215, 0, 255));

    static const char* const restartMsgs[7] = {
        "THE GAME WILL BE RESTARTED. ALL UNSAVED PROGRESS WILL BE LOST. CONTINUE?",
        "THE GAME WILL BE RESTARTED. ALL UNSAVED PROGRESS WILL BE LOST. CONTINUE?",
        "THE GAME WILL BE RESTARTED. ALL UNSAVED PROGRESS WILL BE LOST. CONTINUE?",
        "DAS SPIEL WIRD NEU GESTARTET. NICHT GESPEICHERTER FORTSCHRITT GEHT VERLOREN. WEITER?",
        "LA PARTIE VA RECOMMENCER. TOUTE PROGRESSION NON SAUVEGARDEE SERA PERDUE. CONTINUER?",
        "LA PARTITA VERRA RIAVVIATA. TUTTI I PROGRESSI NON SALVATI ANDRANNO PERSI. CONTINUARE?",
        "SE REINICIARA LA PARTIDA. TODO EL PROGRESO NO GUARDADO SE PERDERA. CONTINUAR?"
    };
    static const char* const hcMsgs[7] = {
        "ENABLING HARDCORE MODE WILL RESTART THE GAME FROM THE BEGINNING. CONTINUE?",
        "ENABLING HARDCORE MODE WILL RESTART THE GAME FROM THE BEGINNING. CONTINUE?",
        "ENABLING HARDCORE MODE WILL RESTART THE GAME FROM THE BEGINNING. CONTINUE?",
        "DAS AKTIVIEREN DES HARDCORE-MODUS STARTET DAS SPIEL VON VORN. WEITER?",
        "ACTIVER LE MODE HARDCORE RECOMMENCERA LE JEU DEPUIS LE DEBUT. CONTINUER?",
        "ATTIVARE LA MODALITA HARDCORE RIAVVIERA IL GIOCO DALL'INIZIO. CONTINUARE?",
        "AL ACTIVAR EL MODO HARDCORE LA PARTIDA SE REINICIARA DESDE EL PRINCIPIO. CONTINUAR?"
    };
    const char* msg = sConfirmIsRestart ? restartMsgs[lang] : hcMsgs[lang];
    DrawWrappedTextClipped(30.0f, 84.0f, 1.0f, msg, 260.0f, 11.0f, C2D_Color32(220, 235, 255, 255), 50.0f, 136.0f);

    /* ACCEPT button */
    static const char* const acceptLabels[7] = {
        "ACCEPT", "ACCEPT", "ACCEPT",
        "JA", "ACCEPTER", "ACCETTA", "ACEPTAR"
    };
    C2D_DrawRectSolid(40.0f, 140.0f, 0.9f, 110.0f, 28.0f, C2D_Color32(20, 90, 45, 255));
    DrawTextCentered(95.0f, 149.0f, 1.0f, acceptLabels[lang], C2D_Color32(255, 255, 255, 255));

    /* CANCEL button */
    static const char* const cancelModalLabels[7] = {
        "CANCEL", "CANCEL", "CANCEL",
        "NEIN", "ANNULER", "ANNULLA", "CANCELAR"
    };
    C2D_DrawRectSolid(170.0f, 140.0f, 0.9f, 110.0f, 28.0f, C2D_Color32(110, 30, 30, 255));
    DrawTextCentered(225.0f, 149.0f, 1.0f, cancelModalLabels[lang], C2D_Color32(255, 255, 255, 255));
}

/* Render Button Remap Modal */
static void RenderDisplayModal(int lang) {
    C2D_DrawRectSolid(10.0f, 26.0f, 0.85f, 300.0f, 206.0f, C2D_Color32(10, 14, 24, 250));
    C2D_DrawRectSolid(10.0f, 26.0f, 0.84f, 300.0f, 206.0f, C2D_Color32(40, 70, 120, 255));

    const char* displayTitles[7] = {
        "DISPLAY SETTINGS", "DISPLAY SETTINGS", "DISPLAY SETTINGS",
        "BILDSCHIRMEINSTELLUNGEN", "PARAMETRES D'AFFICHAGE", "IMPOSTAZIONI SCHERMO", "CONFIGURACION DE PANTALLA"
    };
    DrawText(20.0f, 30.0f, 1.0f, displayTitles[lang], C2D_Color32(255, 215, 0, 255));

    /* Language row (Y: 46 to 67) */
    C2D_DrawRectSolid(16.0f, 46.0f, 0.9f, 288.0f, 21.0f, C2D_Color32(24, 32, 50, 255));
    const char* langLabels[7] = {
        "LANGUAGE:", "LANGUAGE:", "LANGUAGE:",
        "SPRACHE:", "LANGUE:", "LINGUA:", "IDIOMA:"
    };
    DrawText(24.0f, 50.0f, 1.0f, langLabels[lang], C2D_Color32(255, 255, 255, 255));
    DrawText(160.0f, 50.0f, 1.0f, Port_Config_GetLanguageDisplayName(lang), C2D_Color32(255, 215, 0, 255));

    /* Aspect Ratio row (Y: 69 to 90) */
    C2D_DrawRectSolid(16.0f, 69.0f, 0.9f, 288.0f, 21.0f, C2D_Color32(24, 32, 50, 255));
    const char* aspectLabels[7] = {
        "ASPECT RATIO:", "ASPECT RATIO:", "ASPECT RATIO:",
        "BILDVERHAELTNIS:", "FORMAT D'IMAGE:", "FORMATO:", "ASPECTO:"
    };
    DrawText(24.0f, 73.0f, 1.0f, aspectLabels[lang], C2D_Color32(255, 255, 255, 255));
    DrawText(160.0f, 73.0f, 1.0f, GetAspectRatioDisplayName(lang), C2D_Color32(255, 215, 0, 255));

    /* Display Style row (Y: 92 to 113) */
    C2D_DrawRectSolid(16.0f, 92.0f, 0.9f, 288.0f, 21.0f, C2D_Color32(24, 32, 50, 255));
    const char* styleLabels[7] = {
        "DISPLAY STYLE:", "DISPLAY STYLE:", "DISPLAY STYLE:",
        "DARSTELLUNG:", "STYLE D'AFFICHAGE:", "STILE DISPLAY:", "ESTILO:"
    };
    DrawText(24.0f, 96.0f, 1.0f, styleLabels[lang], C2D_Color32(255, 255, 255, 255));
    DrawText(160.0f, 96.0f, 1.0f, GetDisplayStyleDisplayName(lang), C2D_Color32(255, 215, 0, 255));

    /* 3D Depth row (Y: 115 to 136) */
    C2D_DrawRectSolid(16.0f, 115.0f, 0.9f, 288.0f, 21.0f, C2D_Color32(24, 32, 50, 255));
    const char* depthLabels[7] = {
        "3D DEPTH:", "3D DEPTH:", "3D DEPTH:",
        "3D-TIEFE:", "PROFONDEUR 3D:", "PROFONDITA 3D:", "PROFUNDIDAD 3D:"
    };
    DrawText(24.0f, 119.0f, 1.0f, depthLabels[lang], C2D_Color32(255, 255, 255, 255));
    DrawText(160.0f, 119.0f, 1.0f, GetStereoDepthDisplayName(lang), C2D_Color32(255, 215, 0, 255));

    /* FPS Overlay toggle row (Y: 138 to 159) */
    C2D_DrawRectSolid(16.0f, 138.0f, 0.9f, 288.0f, 21.0f, C2D_Color32(24, 32, 50, 255));
    const char* fpsLabels[7] = {
        "SHOW FPS:", "SHOW FPS:", "SHOW FPS:",
        "FPS ANZEIGEN:", "AFFICHER FPS:", "MOSTRA FPS:", "MOSTRAR FPS:"
    };
    DrawText(24.0f, 142.0f, 1.0f, fpsLabels[lang], C2D_Color32(255, 255, 255, 255));
    bool fpsOn = Port_Config_GetShowFps();
    DrawText(160.0f, 142.0f, 1.0f, GetFpsOverlayDisplayName(lang),
             fpsOn ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255));

    /* Auto-Hide HUD toggle row (Y: 161 to 182) */
    C2D_DrawRectSolid(16.0f, 161.0f, 0.9f, 288.0f, 21.0f, C2D_Color32(24, 32, 50, 255));
    const char* hudLabels[7] = {
        "AUTO-HIDE HUD:", "AUTO-HIDE HUD:", "AUTO-HIDE HUD:",
        "HUD AUTOM. AUSBLENDEN:", "MASQUER HUD AUTO:", "NASCONDI HUD AUTO:", "AUTOESCONDER HUD:"
    };
    DrawText(24.0f, 165.0f, 1.0f, hudLabels[lang], C2D_Color32(255, 255, 255, 255));
    bool ahOn = Port_Config_GetAutoHideHud();
    DrawText(160.0f, 165.0f, 1.0f, ahOn ? ((lang == 6) ? "ACTIVADO" : ((lang == 3) ? "EIN" : ((lang == 4) ? "ACTIVE" : ((lang == 5) ? "ATTIVO" : "ON"))))
                                         : ((lang == 6) ? "DESACTIVADO" : ((lang == 3) ? "AUS" : ((lang == 4) ? "DESACTIVE" : ((lang == 5) ? "DISATTIVO" : "OFF")))),
             ahOn ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255));

    /* Hide Spoilers toggle row (Y: 184 to 204) */
    C2D_DrawRectSolid(16.0f, 184.0f, 0.9f, 288.0f, 20.0f, C2D_Color32(24, 32, 50, 255));
    const char* spoilerLabels[7] = {
        "HIDE SPOILERS:", "HIDE SPOILERS:", "HIDE SPOILERS:",
        "SPOILER VERBERGEN:", "MASQUER SPOILERS:", "NASCONDI SPOILER:", "OCULTAR SPOILERS:"
    };
    DrawText(24.0f, 187.0f, 1.0f, spoilerLabels[lang], C2D_Color32(255, 255, 255, 255));
    bool spOn = Port_Config_GetHideSpoilers();
    DrawText(160.0f, 187.0f, 1.0f, spOn ? ((lang == 6) ? "ACTIVADO" : ((lang == 3) ? "EIN" : ((lang == 4) ? "ACTIVE" : ((lang == 5) ? "ATTIVO" : "ON"))))
                                         : ((lang == 6) ? "DESACTIVADO" : ((lang == 3) ? "AUS" : ((lang == 4) ? "DESACTIVE" : ((lang == 5) ? "DISATTIVO" : "OFF")))),
             spOn ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255));

    /* Close button (Y: 206 to 228) */
    C2D_DrawRectSolid(100.0f, 206.0f, 0.9f, 120.0f, 22.0f, C2D_Color32(20, 70, 130, 255));
    const char* closeLabels[7] = {
        "CLOSE", "CLOSE", "CLOSE",
        "SCHLIESSEN", "FERMER", "CHIUDI", "CERRAR"
    };
    DrawTextCentered(160.0f, 211.0f, 1.0f, closeLabels[lang], C2D_Color32(255, 255, 255, 255));
}

/* Render RetroAchievements Settings Modal */
static void RenderRASettingsModal(int lang) {
    C2D_DrawRectSolid(10.0f, 26.0f, 0.85f, 300.0f, 206.0f, C2D_Color32(10, 14, 24, 250));
    C2D_DrawRectSolid(10.0f, 26.0f, 0.84f, 300.0f, 206.0f, C2D_Color32(40, 70, 120, 255));

    const char* raTitles[7] = {
        "RETROACHIEVEMENTS SETTINGS", "RETROACHIEVEMENTS SETTINGS", "RETROACHIEVEMENTS SETTINGS",
        "RETROACHIEVEMENTS EINSTELLUNGEN", "PARAMETRES RETROACHIEVEMENTS", "IMPOSTAZIONI RETROACHIEVEMENTS", "AJUSTES DE RETROACHIEVEMENTS"
    };
    DrawText(20.0f, 32.0f, 1.0f, raTitles[lang], C2D_Color32(255, 215, 0, 255));

    /* Status indicator text top-right */
    uint32_t statusCol = C2D_Color32(140, 160, 190, 255);
    switch (Port_RA_GetStatus()) {
        case RA_STATUS_CONNECTED: statusCol = C2D_Color32(80, 255, 120, 255); break;
        case RA_STATUS_CONNECTING: statusCol = C2D_Color32(255, 220, 80, 255); break;
        case RA_STATUS_ERROR: statusCol = C2D_Color32(255, 90, 90, 255); break;
        default: break;
    }
    DrawText(200.0f, 32.0f, 1.0f, Port_RA_GetStatusString(lang), statusCol);

    /* Row 1: User Login (Y: 56 to 80) */
    C2D_DrawRectSolid(16.0f, 56.0f, 0.9f, 288.0f, 24.0f, C2D_Color32(24, 32, 50, 255));
    const char* userLabels[7] = {
        "USER:", "USER:", "USER:",
        "BENUTZER:", "UTILISATEUR:", "UTENTE:", "USUARIO:"
    };
    DrawText(24.0f, 62.0f, 1.0f, userLabels[lang], C2D_Color32(255, 255, 255, 255));
    const char* user = Port_RA_GetUsername();
    if (user && user[0] != '\0') {
        DrawText(170.0f, 62.0f, 1.0f, user, C2D_Color32(255, 215, 0, 255));
    } else {
        const char* loginLabels[7] = {
            "LOGIN", "LOGIN", "LOGIN",
            "ANMELDEN", "CONNEXION", "ACCEDI", "INICIAR SESION"
        };
        DrawText(170.0f, 62.0f, 1.0f, loginLabels[lang], C2D_Color32(255, 140, 80, 255));
    }

    /* Row 2: Enable / Disable (Y: 84 to 108) */
    C2D_DrawRectSolid(16.0f, 84.0f, 0.9f, 288.0f, 24.0f, C2D_Color32(24, 32, 50, 255));
    const char* sysLabels[7] = {
        "ACHIEVEMENTS SYS:", "ACHIEVEMENTS SYS:", "ACHIEVEMENTS SYS:",
        "ERFOLGE-SYSTEM:", "SYSTEME SUCCES:", "SISTEMA OBIETTIVI:", "SISTEMA LOGROS:"
    };
    DrawText(24.0f, 90.0f, 1.0f, sysLabels[lang], C2D_Color32(255, 255, 255, 255));
    bool raEn = Port_RA_IsEnabled();
    const char* onStr = (lang == 6) ? "ACTIVADO" : ((lang == 3) ? "AKTIVIERT" : ((lang == 4) ? "ACTIVE" : ((lang == 5) ? "ATTIVO" : "ENABLED")));
    const char* offStr = (lang == 6) ? "DESACTIVADO" : ((lang == 3) ? "DEAKTIVIERT" : ((lang == 4) ? "DESACTIVE" : ((lang == 5) ? "DISATTIVO" : "DISABLED")));
    DrawText(170.0f, 90.0f, 1.0f, raEn ? onStr : offStr,
             raEn ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255));

    /* Row 3: Hardcore Mode (Disabled / Proximamente) (Y: 112 to 136) */
    C2D_DrawRectSolid(16.0f, 112.0f, 0.9f, 288.0f, 24.0f, C2D_Color32(18, 24, 38, 255));
    const char* hcLabels[7] = {
        "HARDCORE MODE:", "HARDCORE MODE:", "HARDCORE MODE:",
        "HARDCORE-MODUS:", "MODE HARDCORE:", "MODO HARDCORE:", "MODO HARDCORE:"
    };
    DrawText(24.0f, 118.0f, 1.0f, hcLabels[lang], C2D_Color32(140, 150, 170, 255));
    const char* soonLabels[7] = {
        "COMING SOON", "COMING SOON", "COMING SOON",
        "DEMNAECHST", "BIENTOT", "PROSSIMAMENTE", "PROXIMAMENTE"
    };
    DrawText(170.0f, 118.0f, 1.0f, soonLabels[lang], C2D_Color32(130, 140, 160, 255));

    /* Row 4: Achievement Notification Sound (Y: 140 to 164) */
    C2D_DrawRectSolid(16.0f, 140.0f, 0.9f, 288.0f, 24.0f, C2D_Color32(24, 32, 50, 255));
    const char* sndLabels[7] = {
        "ACHIEVEMENT SOUND:", "ACHIEVEMENT SOUND:", "ACHIEVEMENT SOUND:",
        "ERFOLG-TON:", "SON NOTIFICATION:", "SUONO NOTIFICA:", "SONIDO LOGRO:"
    };
    DrawText(24.0f, 146.0f, 1.0f, sndLabels[lang], C2D_Color32(255, 255, 255, 255));
    bool snd = Port_RA_GetNotificationSound();
    DrawText(170.0f, 146.0f, 1.0f, snd ? ((lang == 6) ? "ACTIVADO" : ((lang == 3) ? "EIN" : ((lang == 4) ? "ACTIVE" : ((lang == 5) ? "ATTIVO" : "ON"))))
                                       : ((lang == 6) ? "DESACTIVADO" : ((lang == 3) ? "AUS" : ((lang == 4) ? "DESACTIVE" : ((lang == 5) ? "DISATTIVO" : "OFF")))),
             snd ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255));

    /* Close button */
    C2D_DrawRectSolid(100.0f, 206.0f, 0.9f, 120.0f, 22.0f, C2D_Color32(20, 70, 130, 255));
    const char* closeRALabels[7] = {
        "CLOSE", "CLOSE", "CLOSE",
        "SCHLIESSEN", "FERMER", "CHIUDI", "CERRAR"
    };
    DrawTextCentered(160.0f, 212.0f, 1.0f, closeRALabels[lang], C2D_Color32(255, 255, 255, 255));
}

/* Action picker popup for remapping */
static void RenderRemapSelectModal(int lang) {
    if (sRemapSelectButtonIdx < 0 || sRemapSelectButtonIdx >= 10) return;

    /* Dark semi-transparent background overlay */
    C2D_DrawRectSolid(12.0f, 28.0f, 0.96f, 296.0f, 204.0f, C2D_Color32(8, 12, 20, 252));
    C2D_DrawRectSolid(12.0f, 28.0f, 0.95f, 296.0f, 204.0f, C2D_Color32(40, 80, 140, 255));

    static const char* const btnNames[7][10] = {
        { "BUTTON A", "BUTTON B", "BUTTON X", "BUTTON Y", "BUTTON L", "BUTTON R", "BUTTON ZL", "BUTTON ZR", "START", "SELECT" },
        { "BUTTON A", "BUTTON B", "BUTTON X", "BUTTON Y", "BUTTON L", "BUTTON R", "BUTTON ZL", "BUTTON ZR", "START", "SELECT" },
        { "BUTTON A", "BUTTON B", "BUTTON X", "BUTTON Y", "BUTTON L", "BUTTON R", "BUTTON ZL", "BUTTON ZR", "START", "SELECT" },
        { "TASTE A", "TASTE B", "TASTE X", "TASTE Y", "TASTE L", "TASTE R", "TASTE ZL", "TASTE ZR", "START", "SELECT" },
        { "BOUTON A", "BOUTON B", "BOUTON X", "BOUTON Y", "BOUTON L", "BOUTON R", "BOUTON ZL", "BOUTON ZR", "START", "SELECT" },
        { "PULSANTE A", "PULSANTE B", "PULSANTE X", "PULSANTE Y", "PULSANTE L", "PULSANTE R", "PULSANTE ZL", "PULSANTE ZR", "START", "SELECT" },
        { "BOTON A", "BOTON B", "BOTON X", "BOTON Y", "BOTON L", "BOTON R", "BOTON ZL", "BOTON ZR", "START", "SELECT" }
    };

    char header[64];
    const char* selectActionTitles[7] = {
        "SELECT ACTION (%s)", "SELECT ACTION (%s)", "SELECT ACTION (%s)",
        "AKTION WAEHLEN (%s)", "CHOISIR ACTION (%s)", "SELEZIONA AZIONE (%s)", "SELECCIONAR ACCION (%s)"
    };
    snprintf(header, sizeof(header), selectActionTitles[lang], btnNames[lang][sRemapSelectButtonIdx]);
    DrawTextCentered(160.0f, 32.0f, 1.0f, header, C2D_Color32(255, 215, 0, 255));

    int currentAct = Port_Config_GetButtonMapping(sRemapSelectButtonIdx);

    extern bool Port_RA_IsHardcore(void);
    bool hcActive = Port_RA_IsHardcore();

    /* 9 actions arranged in 2 columns: 5 in col 0 (left: 0..4), 4 in col 1 (right: 5..8) + CANCEL */
    for (int act = 0; act < 9; ++act) {
        int col = act / 5;
        int row = act % 5;
        float bx = (col == 0) ? 18.0f : 162.0f;
        float by = 48.0f + (float)row * 28.0f;
        float bw = 140.0f;
        float bh = 24.0f;

        bool isDisabled = (act == 1 && hcActive);
        bool isCur = (act == currentAct);
        uint32_t bgCol, bdrCol, textCol;

        if (isDisabled) {
            bgCol = C2D_Color32(16, 20, 28, 255);
            bdrCol = C2D_Color32(35, 42, 55, 255);
            textCol = C2D_Color32(80, 90, 110, 255);
        } else if (isCur) {
            bgCol = C2D_Color32(30, 80, 140, 255);
            bdrCol = C2D_Color32(80, 180, 255, 255);
            textCol = C2D_Color32(255, 255, 255, 255);
        } else {
            bgCol = C2D_Color32(20, 28, 44, 255);
            bdrCol = C2D_Color32(45, 58, 85, 255);
            textCol = C2D_Color32(180, 200, 225, 255);
        }

        C2D_DrawRectSolid(bx, by, 0.97f, bw, bh, bdrCol);
        C2D_DrawRectSolid(bx + 1.0f, by + 1.0f, 0.98f, bw - 2.0f, bh - 2.0f, bgCol);
        DrawTextCentered(bx + bw / 2.0f, by + 7.0f, 1.0f, Port_Config_GetActionName(act, lang), textCol);
    }

    /* Cancel button in col 1, row 4 */
    float cx = 162.0f;
    float cy = 48.0f + 4.0f * 28.0f;
    C2D_DrawRectSolid(cx, cy, 0.97f, 140.0f, 24.0f, C2D_Color32(100, 30, 30, 255));
    C2D_DrawRectSolid(cx + 1.0f, cy + 1.0f, 0.98f, 138.0f, 22.0f, C2D_Color32(60, 20, 20, 255));
    const char* cancelLabels[7] = {
        "CANCEL", "CANCEL", "CANCEL",
        "ABBRECHEN", "ANNULER", "ANNULLA", "CANCELAR"
    };
    DrawTextCentered(cx + 70.0f, cy + 7.0f, 1.0f, cancelLabels[lang], C2D_Color32(255, 140, 140, 255));
}

static void RenderRemapModal(int lang) {
    /* Background overlay */
    C2D_DrawRectSolid(10.0f, 26.0f, 0.85f, 300.0f, 206.0f, C2D_Color32(10, 14, 24, 250));
    C2D_DrawRectSolid(10.0f, 26.0f, 0.84f, 300.0f, 206.0f, C2D_Color32(40, 70, 120, 255));

    /* Title at top */
    const char* remapTitles[7] = {
        "BUTTON REMAPPING (TAP TO CHANGE)", "BUTTON REMAPPING (TAP TO CHANGE)", "BUTTON REMAPPING (TAP TO CHANGE)",
        "TASTENBELEGUNG (ZUM AENDERN TIPPEN)", "RECONFIGURATION TOUCHES (TOUCHER)", "CONFIGURAZIONE TASTI (TOCCA)", "REASIGNAR BOTONES (TOCA PARA CAMBIAR)"
    };
    DrawText(20.0f, 31.0f, 1.0f, remapTitles[lang], C2D_Color32(255, 215, 0, 255));

    /* Viewable / scrollable region: Y 44 to 202 (158px) */
    const float viewY0 = 44.0f;
    const float viewY1 = 202.0f;
    const float maxScroll = 128.0f; /* 11 items * 26px = 286px; 286 - 158 = 128px */

    /* 10 remappable buttons + 1 C-Stick row = 11 items, each 26px tall */
    static const char* const btnRowNames[7][10] = {
        { "BUTTON A:", "BUTTON B:", "BUTTON X:", "BUTTON Y:", "BUTTON L:", "BUTTON R:", "BUTTON ZL:", "BUTTON ZR:", "START:", "SELECT:" },
        { "BUTTON A:", "BUTTON B:", "BUTTON X:", "BUTTON Y:", "BUTTON L:", "BUTTON R:", "BUTTON ZL:", "BUTTON ZR:", "START:", "SELECT:" },
        { "BUTTON A:", "BUTTON B:", "BUTTON X:", "BUTTON Y:", "BUTTON L:", "BUTTON R:", "BUTTON ZL:", "BUTTON ZR:", "START:", "SELECT:" },
        { "TASTE A:", "TASTE B:", "TASTE X:", "TASTE Y:", "TASTE L:", "TASTE R:", "TASTE ZL:", "TASTE ZR:", "START:", "SELECT:" },
        { "BOUTON A:", "BOUTON B:", "BOUTON X:", "BOUTON Y:", "BOUTON L:", "BOUTON R:", "BOUTON ZL:", "BOUTON ZR:", "START:", "SELECT:" },
        { "PULSANTE A:", "PULSANTE B:", "PULSANTE X:", "PULSANTE Y:", "PULSANTE L:", "PULSANTE R:", "PULSANTE ZL:", "PULSANTE ZR:", "START:", "SELECT:" },
        { "BOTON A:", "BOTON B:", "BOTON X:", "BOTON Y:", "BOTON L:", "BOTON R:", "BOTON ZL:", "BOTON ZR:", "START:", "SELECT:" }
    };

    float contentY = viewY0 - sRemapScrollY;
    extern bool Port_RA_IsHardcore(void);
    bool hcActive = Port_RA_IsHardcore();

    /* Draw 10 button rows with strict clipping */
    for (int i = 0; i < 10; ++i) {
        float py = contentY + (float)i * 26.0f;
        if (py + 24.0f <= viewY0 || py >= viewY1) continue;

        float rY0 = (py < viewY0) ? viewY0 : py;
        float rY1 = (py + 24.0f > viewY1) ? viewY1 : (py + 24.0f);
        if (rY1 > rY0) {
            C2D_DrawRectSolid(16.0f, rY0, 0.88f, 284.0f, rY1 - rY0, C2D_Color32(24, 32, 50, 255));
            C2D_DrawRectSolid(16.0f, rY0, 0.89f, 284.0f, 1.0f, C2D_Color32(45, 60, 90, 255));
        }

        DrawTextClipped(24.0f, py + 7.0f, 1.0f,
            btnRowNames[lang][i],
            C2D_Color32(255, 255, 255, 255), viewY0, viewY1);
        int act = Port_Config_GetButtonMapping(i);
        bool actDisabled = (act == 1 && hcActive);
        uint32_t actCol = actDisabled ? C2D_Color32(110, 120, 140, 255) : C2D_Color32(255, 215, 0, 255);
        DrawTextClipped(150.0f, py + 7.0f, 1.0f,
            Port_Config_GetActionName(act, lang),
            actCol, viewY0, viewY1);
    }

    /* C-Stick mode row (item 11) */
    float cstickY = contentY + 10.0f * 26.0f;
    if (cstickY + 24.0f > viewY0 && cstickY < viewY1) {
        float rY0 = (cstickY < viewY0) ? viewY0 : cstickY;
        float rY1 = (cstickY + 24.0f > viewY1) ? viewY1 : (cstickY + 24.0f);
        if (rY1 > rY0) {
            C2D_DrawRectSolid(16.0f, rY0, 0.88f, 284.0f, rY1 - rY0, C2D_Color32(28, 40, 60, 255));
            C2D_DrawRectSolid(16.0f, rY0, 0.89f, 284.0f, 1.0f, C2D_Color32(50, 80, 120, 255));
        }

        DrawTextClipped(24.0f, cstickY + 7.0f, 1.0f,
            "C-STICK:",
            C2D_Color32(255, 255, 255, 255), viewY0, viewY1);
        int cmode = Port_Config_GetCstickMode();
        static const char* const cstickModeNames[7][4] = {
            { "OFF", "AIM ONLY", "MOVEMENT ONLY", "ALL" },
            { "OFF", "AIM ONLY", "MOVEMENT ONLY", "ALL" },
            { "OFF", "AIM ONLY", "MOVEMENT ONLY", "ALL" },
            { "AUS", "NUR ZIELEN", "NUR BEWEGUNG", "ALLES" },
            { "DESACTIVE", "VISER SEUL.", "DEPLAC. SEUL.", "TOUT" },
            { "DISATTIVO", "SOLO MIRA", "SOLO MOVIMENTO", "TUTTO" },
            { "DESACTIVADO", "SOLO APUNTAR", "SOLO MOVIMIENTO", "TODO" }
        };
        DrawTextClipped(150.0f, cstickY + 7.0f, 1.0f,
            cstickModeNames[lang][cmode],
            C2D_Color32(255, 215, 0, 255), viewY0, viewY1);
    }

    /* Scroll indicator on right edge */
    if (maxScroll > 0.0f) {
        float trackH = viewY1 - viewY0;
        float thumbH = 30.0f;
        float thumbY = viewY0 + (sRemapScrollY / maxScroll) * (trackH - thumbH);
        C2D_DrawRectSolid(304.0f, viewY0, 0.91f, 3.0f, trackH, C2D_Color32(20, 28, 44, 255));
        C2D_DrawRectSolid(304.0f, thumbY, 0.93f, 3.0f, thumbH, C2D_Color32(80, 160, 240, 255));
    }

    /* Bottom buttons (Fixed at bottom): RESTABLECER (Left) + CERRAR (Right) */
    /* Reset Defaults Button (X: 16 to 156, W: 140) */
    C2D_DrawRectSolid(16.0f, 206.0f, 0.94f, 140.0f, 22.0f, C2D_Color32(90, 35, 35, 255));
    C2D_DrawRectSolid(17.0f, 207.0f, 0.95f, 138.0f, 20.0f, C2D_Color32(60, 22, 22, 255));
    DrawTextCentered(86.0f, 212.0f, 1.0f,
        (lang == 6) ? "RESTABLECER" : "RESET DEFAULTS",
        C2D_Color32(255, 170, 170, 255));

    /* Close button (X: 164 to 304, W: 140) */
    C2D_DrawRectSolid(164.0f, 206.0f, 0.94f, 140.0f, 22.0f, C2D_Color32(20, 70, 130, 255));
    C2D_DrawRectSolid(165.0f, 207.0f, 0.95f, 138.0f, 20.0f, C2D_Color32(14, 45, 85, 255));
    DrawTextCentered(234.0f, 212.0f, 1.0f,
        (lang == 6) ? "CERRAR" : "CLOSE",
        C2D_Color32(255, 255, 255, 255));

    /* Render action picker popup if active */
    if (sRemapSelectButtonIdx >= 0) {
        RenderRemapSelectModal(lang);
    }
}

/* Render Map View */
static void RenderMapView(void) {
    int lang = GetLang();
    if (sFollowSamus) {
        sViewArea = gCurrentArea;
        if (gCurrentArea < 7) {
            CenterMapOnTile(gMinimapX, gMinimapY);
        }
    }

    /* Area Stats for current/viewed area */
    struct AreaItemStats areaStats;
    GetAreaStats(sViewArea, &areaStats);

    /* 1. Sub-header bar controls (Y: 26 to 45) */
    /* Area selector [ < ] AreaName [ > ] */
    C2D_DrawRectSolid(4.0f, 26.0f, 0.4f, 16.0f, 18.0f, C2D_Color32(30, 40, 60, 255));
    DrawTextCentered(12.0f, 31.0f, 1.0f, "<", C2D_Color32(100, 220, 255, 255));

    C2D_DrawRectSolid(22.0f, 26.0f, 0.4f, 70.0f, 18.0f, sFollowSamus ? C2D_Color32(16, 50, 95, 255) : C2D_Color32(26, 32, 48, 255));
    DrawTextCentered(57.0f, 31.0f, 1.0f, AreaName(sViewArea), sFollowSamus ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(180, 205, 235, 255));

    C2D_DrawRectSolid(94.0f, 26.0f, 0.4f, 16.0f, 18.0f, C2D_Color32(30, 40, 60, 255));
    DrawTextCentered(102.0f, 31.0f, 1.0f, ">", C2D_Color32(100, 220, 255, 255));

    /* Area Items Badge (Shows e.g. "5/12"; totals hidden in RA hardcore mode or if hide spoilers enabled) */
    char itemBadge[24];
    bool hideSpoilers = Port_RA_IsHardcore() || Port_Config_GetHideSpoilers();
    if (hideSpoilers) {
        snprintf(itemBadge, sizeof(itemBadge), "%u", areaStats.totalObtained);
    } else {
        snprintf(itemBadge, sizeof(itemBadge), "%u/%u", areaStats.totalObtained, areaStats.totalItems);
    }
    C2D_DrawRectSolid(112.0f, 26.0f, 0.4f, 44.0f, 18.0f, C2D_Color32(20, 45, 35, 255));
    C2D_DrawRectSolid(113.0f, 27.0f, 0.45f, 42.0f, 16.0f, C2D_Color32(12, 28, 20, 255));
    DrawTextCentered(134.0f, 31.0f, 1.0f, itemBadge, C2D_Color32(120, 255, 160, 255));

    /* Zoom Toggle Button */
    const char* zoomLabels[] = { "1X", "2X", "3X" };
    C2D_DrawRectSolid(156.0f, 26.0f, 0.4f, 40.0f, 18.0f, C2D_Color32(24, 45, 75, 255));
    C2D_DrawRectSolid(157.0f, 27.0f, 0.45f, 38.0f, 16.0f, C2D_Color32(14, 25, 45, 255));
    DrawTextCentered(176.0f, 31.0f, 1.0f, zoomLabels[sZoomLevel], C2D_Color32(255, 215, 0, 255));

    /* Center on Samus Button */
    C2D_DrawRectSolid(198.0f, 26.0f, 0.4f, 52.0f, 18.0f, sFollowSamus ? C2D_Color32(20, 80, 140, 255) : C2D_Color32(26, 35, 55, 255));
    DrawTextCentered(224.0f, 31.0f, 1.0f, "SAMUS", sFollowSamus ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(140, 160, 190, 255));

    /* Chozo Objective / Coords Badge */
    uint8_t targetArea = 0, targetX = 0, targetY = 0;
    bool hasTarget = GetActiveChozoTarget(&targetArea, &targetX, &targetY);
    const char* objLabel = (lang == 6) ? "! OBJETIVO" : ((lang == 3) ? "! ZIEL" : "! TARGET");
    if (hasTarget) {
        bool inViewArea = (targetArea == sViewArea);
        uint32_t targetBg = inViewArea
            ? (((sFrameCounter % 20) < 10) ? C2D_Color32(200, 40, 40, 255) : C2D_Color32(160, 120, 0, 255))
            : C2D_Color32(80, 60, 20, 255);
        C2D_DrawRectSolid(252.0f, 26.0f, 0.4f, 64.0f, 18.0f, targetBg);
        DrawTextCentered(284.0f, 31.0f, 1.0f, objLabel, C2D_Color32(255, 255, 255, 255));
    } else {
        C2D_DrawRectSolid(252.0f, 26.0f, 0.4f, 64.0f, 18.0f, C2D_Color32(20, 25, 38, 255));
        char pBuf[16];
        snprintf(pBuf, sizeof(pBuf), "%02u,%02u", gMinimapX, gMinimapY);
        DrawTextCentered(284.0f, 31.0f, 1.0f, pBuf, C2D_Color32(140, 160, 190, 255));
    }

    /* 2. Full-height Map Canvas (Y: 48 to 236, X: 4 to 316, W: 312, H: 188) */
    const float canvasX = 4.0f;
    const float canvasY = 48.0f;
    const float canvasW = 312.0f;
    const float canvasH = 188.0f;
    const float clipX0 = canvasX;
    const float clipY0 = canvasY;
    const float clipX1 = canvasX + canvasW;
    const float clipY1 = canvasY + canvasH;

    /* Bezel & Canvas background */
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    /* Armed tap-to-warp: a warning border, since the next tap on this canvas
     * teleports instead of panning. */
    if (sDebugMapWarpArmed) {
        C2D_DrawRectSolid(canvasX - 3.0f, canvasY - 3.0f, 0.28f, canvasW + 6.0f, canvasH + 6.0f,
                          ((sFrameCounter & 0x10) != 0) ? C2D_Color32(255, 200, 60, 255)
                                                        : C2D_Color32(120, 90, 20, 255));
    }
#endif
    C2D_DrawRectSolid(canvasX - 1.0f, canvasY - 1.0f, 0.3f, canvasW + 2.0f, canvasH + 2.0f, C2D_Color32(35, 50, 75, 255));
    C2D_DrawRectSolid(canvasX, canvasY, 0.35f, canvasW, canvasH, C2D_Color32(6, 10, 18, 255));

    /* Prepare map tiles */
    uint16_t* mapTiles = NULL;
    if (sViewArea == gCurrentArea) {
        mapTiles = gDecompressedMinimapVisitedTiles;
    } else {
        if (sCachedOtherArea != sViewArea) {
            PauseScreenGetMinimapData(sViewArea, sOtherAreaTiles);
            MinimapSetDownloadedTiles(sViewArea, sOtherAreaTiles);
            sCachedOtherArea = sViewArea;
        }
        mapTiles = sOtherAreaTiles;
    }

    float tileSize = GetTileSizeForZoom(sZoomLevel);
    float startDrawX = canvasX;
    float startDrawY = canvasY;

    if (sZoomLevel == 0) {
        /* Overview mode: center the 32x32 map in the large canvas */
        float totalMapW = 32.0f * tileSize;
        float totalMapH = 32.0f * tileSize;
        startDrawX = canvasX + (canvasW - totalMapW) / 2.0f;
        startDrawY = canvasY + (canvasH - totalMapH) / 2.0f;
    } else {
        startDrawX = canvasX - sScrollX;
        startDrawY = canvasY - sScrollY;
    }

    /* Draw All Authentic Map Tiles */
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            float tileDrawX = startDrawX + (float)x * tileSize;
            float tileDrawY = startDrawY + (float)y * tileSize;

            if (tileDrawX + tileSize < clipX0 || tileDrawX >= clipX1 ||
                tileDrawY + tileSize < clipY0 || tileDrawY >= clipY1) {
                continue;
            }

            uint16_t tileData = mapTiles[x + y * 32];
            if (tileData != 0 && tileData != 0x140) {
                DrawAuthenticMapTile(tileDrawX, tileDrawY, tileSize, tileData, clipX0, clipY0, clipX1, clipY1, 0.5f);
            }
        }
    }

    /* Draw Chozo Target Indicator on the map */
    if (hasTarget && targetArea == sViewArea && targetX < 32 && targetY < 32) {
        float tx = startDrawX + (float)targetX * tileSize;
        float ty = startDrawY + (float)targetY * tileSize;

        if (tx + tileSize >= clipX0 && tx <= clipX1 &&
            ty + tileSize >= clipY0 && ty <= clipY1) {
            uint32_t targetColor = ((sFrameCounter % 20) < 10) ? C2D_Color32(255, 50, 50, 255) : C2D_Color32(255, 220, 0, 255);
            float cx0 = tx < clipX0 ? clipX0 : tx;
            float cx1 = (tx + tileSize) > clipX1 ? clipX1 : (tx + tileSize);
            float cy0 = ty < clipY0 ? clipY0 : ty;
            float cy1 = (ty + tileSize) > clipY1 ? clipY1 : (ty + tileSize);

            if (cx1 > cx0 && cy1 > cy0) {
                C2D_DrawRectSolid(cx0, ty + (tileSize / 2.0f) - 1.0f, 0.75f, cx1 - cx0, 2.0f, targetColor);
                C2D_DrawRectSolid(tx + (tileSize / 2.0f) - 1.0f, cy0, 0.75f, 2.0f, cy1 - cy0, targetColor);
            }
        }
    }

    /* Draw Samus Player indicator (when viewing current area) */
    if (sViewArea == gCurrentArea && gMinimapX < 32 && gMinimapY < 32) {
        float px = startDrawX + (float)gMinimapX * tileSize;
        float py = startDrawY + (float)gMinimapY * tileSize;

        if (px + tileSize >= clipX0 && px <= clipX1 &&
            py + tileSize >= clipY0 && py <= clipY1) {
            if ((sFrameCounter % 40) < 26) {
                C2D_DrawRectSolid(px - 1.0f, py - 1.0f, 0.8f, tileSize + 2.0f, tileSize + 2.0f, C2D_Color32(255, 220, 0, 255));
                C2D_DrawRectSolid(px, py, 0.9f, tileSize, tileSize, C2D_Color32(255, 40, 20, 255));
                C2D_DrawRectSolid(px + (tileSize * 0.25f), py + (tileSize * 0.25f), 0.95f, tileSize * 0.5f, tileSize * 0.5f, C2D_Color32(255, 255, 255, 255));
            }
        }
    }

#ifdef PORT_DEBUG_TOOLS_ACTIVE
    /* Map completion badge, top-left of the canvas. */
    if (Port_BottomUI_DebugTabVisible()) {
        char pctBuf[16];
        snprintf(pctBuf, sizeof(pctBuf), "%d%%", PortPpuMzm_DebugAreaMapPercent((int)sViewArea));
        float bw = (float)(Utf8CharCount(pctBuf) * 6 + 8);
        C2D_DrawRectSolid(canvasX + 3.0f, canvasY + 3.0f, 0.9f, bw, 12.0f, C2D_Color32(10, 16, 26, 220));
        C2D_DrawRectSolid(canvasX + 3.0f, canvasY + 3.0f, 0.91f, bw, 1.0f, C2D_Color32(60, 90, 140, 255));
        DrawText(canvasX + 7.0f, canvasY + 5.0f, 1.0f, pctBuf, C2D_Color32(120, 220, 255, 255));
    }

    /* Persistent Tap-to-Warp toggle button on the map canvas. Sized to the
     * text ("WARP: OFF" == 8 chars * 6px) with a small margin. */
    const float warpBtnW = 58.0f;
    const float warpBtnH = 16.0f;
    const float warpBtnX = 312.0f - warpBtnW;
    const float warpBtnY = 232.0f - warpBtnH;
    uint32_t warpBtnBg = sDebugMapWarpArmed
        ? (((sFrameCounter & 0x10) != 0) ? C2D_Color32(190, 140, 15, 255) : C2D_Color32(140, 95, 10, 255))
        : C2D_Color32(16, 24, 40, 230);
    uint32_t warpBtnBorder = sDebugMapWarpArmed
        ? C2D_Color32(255, 220, 50, 255)
        : C2D_Color32(45, 65, 95, 255);
    uint32_t warpBtnTextCol = sDebugMapWarpArmed
        ? C2D_Color32(255, 255, 255, 255)
        : C2D_Color32(140, 160, 190, 255);

    C2D_DrawRectSolid(warpBtnX - 1.0f, warpBtnY - 1.0f, 0.94f, warpBtnW + 2.0f, warpBtnH + 2.0f, warpBtnBorder);
    C2D_DrawRectSolid(warpBtnX, warpBtnY, 0.95f, warpBtnW, warpBtnH, warpBtnBg);
    DrawTextCentered(warpBtnX + warpBtnW / 2.0f, warpBtnY + (warpBtnH - 8.0f) / 2.0f, 1.0f,
                     sDebugMapWarpArmed ? "WARP: ON" : "WARP: OFF", warpBtnTextCol);
#endif
}

/* Render Status (Estado) View */
static void RenderStatusView(void) {
    int lang = GetLang();
    /* In RA hardcore mode or if hide spoilers enabled, unobtained equipment is hidden as "----" (no spoilers) */
    bool hcStatus = Port_RA_IsHardcore() || Port_Config_GetHideSpoilers();

    C2D_DrawRectSolid(4.0f, 26.0f, 0.4f, 312.0f, 210.0f, C2D_Color32(12, 16, 26, 255));
    C2D_DrawRectSolid(4.0f, 26.0f, 0.35f, 312.0f, 210.0f, C2D_Color32(35, 50, 75, 255));

    const char* titles[7] = {
        "SAMUS ARAN - STATUS & EQUIPMENT",
        "SAMUS ARAN - STATUS & EQUIPMENT",
        "SAMUS ARAN - STATUS & EQUIPMENT",
        "SAMUS ARAN - STATUS & AUSRUESTUNG",
        "SAMUS ARAN - STATUT & EQUIPEMENT",
        "SAMUS ARAN - STATO ED EQUIPAGGIAMENTO",
        "SAMUS ARAN - ESTADO Y EQUIPAMIENTO"
    };
    DrawText(12.0f, 30.0f, 1.0f, titles[lang], C2D_Color32(100, 220, 255, 255));

#ifdef PORT_DEBUG_TOOLS_ACTIVE
    if (Port_BottomUI_DebugTabVisible()) DrawStatusDebugButtons();
#endif

    /* 1. Header Resource Card (X: 8, Y: 44 to 72, W: 304, H: 28) */
    C2D_DrawRectSolid(8.0f, 44.0f, 0.45f, 304.0f, 30.0f, C2D_Color32(20, 26, 40, 255));

    /* Health — color shifts with life level (only during real gameplay) */
    DrawEnergyIcon(14.0f, 55.0f);
    char eBuf[32];
    snprintf(eBuf, sizeof(eBuf), "%02u/%02u", gEquipment.currentEnergy, gEquipment.maxEnergy);
    bool inRealGameplay = ((uint8_t)gMainGameMode == 4 /* GM_INGAME */);
    uint32_t energyCol = C2D_Color32(255, 215, 0, 255); /* normal gold */
    if (inRealGameplay && gEquipment.currentEnergy < 30)
        energyCol = C2D_Color32(255, 70, 50, 255);       /* critical red */
    else if (inRealGameplay && gEquipment.currentEnergy < 60)
        energyCol = C2D_Color32(255, 200, 50, 255);      /* warning yellow */
    DrawText(31.0f, 56.0f, 1.0f, eBuf, energyCol);

    /* Missiles — highlight when selected */
    {
        bool normSelected = (gSamusWeaponInfo.missilesSelected == 0);
        if (normSelected && gEquipment.maxMissiles > 0) {
            C2D_DrawRectSolid(86.0f, 50.0f, 0.46f, 74.0f, 22.0f, C2D_Color32(30, 50, 90, 255));
            C2D_DrawRectSolid(86.0f, 50.0f, 0.47f, 74.0f, 1.0f, C2D_Color32(80, 180, 255, 255));
            C2D_DrawRectSolid(86.0f, 71.0f, 0.47f, 74.0f, 1.0f, C2D_Color32(80, 180, 255, 255));
        }
        DrawMissileIcon(90.0f, 55.0f);
        char mBuf[32];
        if (gEquipment.maxMissiles > 0) {
            snprintf(mBuf, sizeof(mBuf), "%03u/%03u", gEquipment.currentMissiles, gEquipment.maxMissiles);
            DrawText(107.0f, 56.0f, 1.0f, mBuf, normSelected ? C2D_Color32(255, 200, 200, 255) : C2D_Color32(255, 140, 140, 255));
        } else {
            DrawText(107.0f, 56.0f, 1.0f, "---/---", C2D_Color32(100, 110, 130, 255));
        }
    }

    /* Super Missiles — highlight when selected */
    {
        bool superSelected = (gSamusWeaponInfo.missilesSelected != 0);
        if (superSelected && gEquipment.maxSuperMissiles > 0) {
            C2D_DrawRectSolid(162.0f, 50.0f, 0.46f, 74.0f, 22.0f, C2D_Color32(20, 50, 30, 255));
            C2D_DrawRectSolid(162.0f, 50.0f, 0.47f, 74.0f, 1.0f, C2D_Color32(80, 255, 140, 255));
            C2D_DrawRectSolid(162.0f, 71.0f, 0.47f, 74.0f, 1.0f, C2D_Color32(80, 255, 140, 255));
        }
        DrawSuperMissileIcon(166.0f, 55.0f);
        char smBuf[32];
        if (gEquipment.maxSuperMissiles > 0) {
            snprintf(smBuf, sizeof(smBuf), "%02u/%02u", gEquipment.currentSuperMissiles, gEquipment.maxSuperMissiles);
            DrawText(183.0f, 56.0f, 1.0f, smBuf, superSelected ? C2D_Color32(160, 255, 200, 255) : C2D_Color32(100, 255, 140, 255));
        } else {
            DrawText(183.0f, 56.0f, 1.0f, "--/--", C2D_Color32(100, 110, 130, 255));
        }
    }

    /* Power Bombs */
    DrawPowerBombIcon(238.0f, 55.0f);
    char pbBuf[32];
    if (gEquipment.maxPowerBombs > 0) {
        snprintf(pbBuf, sizeof(pbBuf), "%02u/%02u", gEquipment.currentPowerBombs, gEquipment.maxPowerBombs);
        DrawText(254.0f, 56.0f, 1.0f, pbBuf, C2D_Color32(255, 225, 80, 255));
    } else {
        DrawText(254.0f, 56.0f, 1.0f, "--/--", C2D_Color32(100, 110, 130, 255));
    }

    /* 2. Beams & Weapons Column (X: 8, W: 148, Y: 76 to 164) */
    C2D_DrawRectSolid(8.0f, 76.0f, 0.45f, 148.0f, 88.0f, C2D_Color32(18, 22, 34, 255));
    const char* beamColTitles[7] = {
        "BEAMS & BOMBS", "BEAMS & BOMBS", "BEAMS & BOMBS",
        "BEAMS & BOMBEN", "RAYONS & BOMBES", "RAGGI E BOMBE", "RAYOS Y BOMBAS"
    };
    DrawText(14.0f, 80.0f, 1.0f, beamColTitles[lang], C2D_Color32(255, 215, 0, 255));

    const char* unknownNames[7] = {
        "UNKNOWN ITEM", "UNKNOWN ITEM", "UNKNOWN ITEM", "UNBEKANNT", "OBJET INCONNU", "OGGETTO SCONOSCIUTO", "OBJETO DESCONOCIDO"
    };
    const char* pistolNames[7] = {
        "PARALYZER", "PARALYZER", "PARALYZER", "NOTFALLPISTOLE", "PISTOLET URGENCE", "PISTOLA EMERGENZA", "PISTOLA EMERGENCIA"
    };

    bool isSuitless = (gEquipment.suitType == 2 /* SUIT_SUITLESS */);
    bool isFullyPowered = (gEquipment.suitType == 1 /* SUIT_FULLY_POWERED */);

    struct {
        const char* name[7];
        uint8_t flag;
        bool isUnknownItem;
    } beams[] = {
        { { "LONG BEAM", "LONG BEAM", "LONG BEAM", "DISTANCE BEAM", "RAYON LONG", "RAGGIO LUNGO", "RAYO LARGO" }, 1 << 0, false },
        { { "ICE BEAM", "ICE BEAM", "ICE BEAM", "ICE BEAM", "RAYON GLACE", "RAGGIO GELO", "RAYO HIELO" }, 1 << 1, false },
        { { "WAVE BEAM", "WAVE BEAM", "WAVE BEAM", "WAVE BEAM", "RAYON ONDES", "RAGGIO ONDA", "RAYO ONDAS" }, 1 << 2, false },
        { { "PLASMA BEAM", "PLASMA BEAM", "PLASMA BEAM", "PLASMA BEAM", "RAYON PLASMA", "RAGGIO PLASMA", "RAYO PLASMA" }, 1 << 3, true },
        { { "CHARGE BEAM", "CHARGE BEAM", "CHARGE BEAM", "CHARGE BEAM", "RAYON CHARGE", "RAGGIO CARICA", "RAYO RECARGA" }, 1 << 4, false },
        { { "BOMBS", "BOMBS", "BOMBS", "BOMBEN", "BOMBES", "BOMBE", "BOMBAS" }, 1 << 7, false }
    };
    for (int i = 0; i < 6; ++i) {
        float py = 92.0f + (float)i * 11.0f;
        if (isSuitless) {
            if (i == 0) {
                DrawText(14.0f, py, 1.0f, pistolNames[lang], C2D_Color32(80, 255, 120, 255));
                DrawText(130.0f, py, 1.0f, "OK", C2D_Color32(80, 255, 120, 255));
            } else {
                bool gotSuitless = (gEquipment.suitMisc & beams[i].flag) != 0;
                DrawText(14.0f, py, 1.0f,
                         (hcStatus && !gotSuitless) ? "----" : beams[i].name[lang],
                         C2D_Color32(75, 85, 105, 255));
                DrawText(130.0f, py, 1.0f, "--", C2D_Color32(75, 85, 105, 255));
            }
            continue;
        }

        bool has = (gEquipment.beamBombs & beams[i].flag) != 0;
        bool active = (gEquipment.beamBombsActivation & beams[i].flag) != 0;
        bool unknown = beams[i].isUnknownItem && !isFullyPowered && has;

        const char* label = (unknown && hcStatus) ? unknownNames[lang] : beams[i].name[lang];
        if (hcStatus && !has) label = "----";
        const char* statusStr = "--";
        uint32_t col = C2D_Color32(75, 85, 105, 255);

        if (has) {
            if (unknown) {
                statusStr = "??";
                col = C2D_Color32(255, 180, 50, 255);
            } else if (active) {
                statusStr = "OK";
                col = C2D_Color32(80, 255, 120, 255);
            } else {
                statusStr = "OFF";
                col = C2D_Color32(220, 160, 60, 255);
            }
        }

        DrawText(14.0f, py, 1.0f, label, col);
        DrawText(130.0f, py, 1.0f, statusStr, col);
    }

    /* 3. Suits & Movement Column (X: 164, W: 148, Y: 76 to 164) */
    C2D_DrawRectSolid(164.0f, 76.0f, 0.45f, 148.0f, 88.0f, C2D_Color32(18, 22, 34, 255));
    const char* suitColTitles[7] = {
        "SUITS & MISC", "SUITS & MISC", "SUITS & MISC",
        "ANZUEGE & ITEMS", "COMBINAISONS", "TUTE E OGGETTI", "TRAJES Y EQUIPO"
    };
    DrawText(170.0f, 80.0f, 1.0f, suitColTitles[lang], C2D_Color32(255, 215, 0, 255));

    struct {
        const char* name[7];
        uint8_t flag;
        bool isUnknownItem;
    } suits[] = {
        { { "HIGH JUMP", "HIGH JUMP", "HIGH JUMP", "HOCHSPRUNG", "SUPER SAUT", "SALTO IN ALTO", "SUPERSALTO" }, 1 << 0, false },
        { { "SPEED BOOSTER", "SPEED BOOSTER", "SPEED BOOSTER", "SPEED BOOSTER", "ACCELERATEUR", "SUPERVELOCITA", "ACELERACION" }, 1 << 1, false },
        { { "SPACE JUMP", "SPACE JUMP", "SPACE JUMP", "SPACE JUMP", "SAUT SPATIAL", "SALTO SPAZIALE", "SALTO ESPACIAL" }, 1 << 2, true },
        { { "SCREW ATTACK", "SCREW ATTACK", "SCREW ATTACK", "SCREW ATTACK", "ATTAQUE VRILLE", "ATTACCO A VITE", "SALTO EN BARRENA" }, 1 << 3, false },
        { { "VARIA SUIT", "VARIA SUIT", "VARIA SUIT", "VARIA SUIT", "COSTUME VARIA", "TUTA VARIA", "TRAJE CLIMATICO" }, 1 << 4, false },
        { { "GRAVITY SUIT", "GRAVITY SUIT", "GRAVITY SUIT", "GRAVITY SUIT", "COSTUME GRAVITE", "TUTA GRAVITA", "TRAJE GRAVITATORIO" }, 1 << 5, true },
        { { "MORPH BALL", "MORPH BALL", "MORPH BALL", "MORPH BALL", "BOULE MORPHING", "MORFOSFERA", "MORFOSFERA" }, 1 << 6, false },
        { { "POWER GRIP", "POWER GRIP", "POWER GRIP", "POWER GRIP", "POIGNE DE FER", "PRESA FORZA", "AGARRE" }, 1 << 7, false }
    };
    for (int i = 0; i < 8; ++i) {
        float py = 91.0f + (float)i * 9.0f;
        uint8_t flag = suits[i].flag;

        if (isSuitless) {
            bool hasSuitlessItem = (flag == (1 << 6) || flag == (1 << 7)) && ((gEquipment.suitMisc & flag) != 0);
            if (hasSuitlessItem) {
                DrawText(170.0f, py, 1.0f, suits[i].name[lang], C2D_Color32(80, 255, 120, 255));
                DrawText(286.0f, py, 1.0f, "OK", C2D_Color32(80, 255, 120, 255));
            } else {
                DrawText(170.0f, py, 1.0f, suits[i].name[lang], C2D_Color32(75, 85, 105, 255));
                DrawText(286.0f, py, 1.0f, "--", C2D_Color32(75, 85, 105, 255));
            }
            continue;
        }

        bool has = (gEquipment.suitMisc & flag) != 0;
        bool active = (gEquipment.suitMiscActivation & flag) != 0;
        bool unknown = suits[i].isUnknownItem && !isFullyPowered && has;

        const char* label = (unknown && hcStatus) ? unknownNames[lang] : suits[i].name[lang];
        if (hcStatus && !has) label = "----";
        const char* statusStr = "--";
        uint32_t col = C2D_Color32(75, 85, 105, 255);

        if (has) {
            if (unknown) {
                statusStr = "??";
                col = C2D_Color32(255, 180, 50, 255);
            } else if (active) {
#ifdef PORT_DEBUG_TOOLS_ACTIVE
                if (Port_BottomUI_DebugTabVisible() && PortPpuMzm_DebugGetForceEffect(flag)) {
                    statusStr = "AUTO";
                    col = C2D_Color32(120, 220, 255, 255);
                } else
#endif
                {
                    statusStr = "OK";
                    col = C2D_Color32(80, 255, 120, 255);
                }
            } else {
                statusStr = "OFF";
                col = C2D_Color32(220, 160, 60, 255);
            }
        }

        DrawText(170.0f, py, 1.0f, label, col);
        DrawText(286.0f, py, 1.0f, statusStr, col);
    }

    /* 4. Area Map Downloads Card & Collectibles Button (Y: 168 to 234) */
    C2D_DrawRectSolid(8.0f, 168.0f, 0.45f, 304.0f, 66.0f, C2D_Color32(16, 20, 30, 255));

    const char* mapHeaderTitles[7] = {
        "DOWNLOADED MAPS:", "DOWNLOADED MAPS:", "DOWNLOADED MAPS:",
        "HERUNTERGELADENE KARTEN:", "CARTES TELECHARGEES:", "MAPPE SCARICATE:", "MAPAS DESCARGADOS:"
    };
    DrawText(14.0f, 172.0f, 1.0f, mapHeaderTitles[lang], C2D_Color32(100, 220, 255, 255));

    /* Button for opening Collectibles Detail Modal */
    struct AreaItemStats globalStats;
    GetGlobalItemStats(&globalStats);
    char globBtn[48];
    static const char* const itemsBtnLabels[7] = {
        "アイテム", "アイテム", "ITEMS",
        "OBJEKTE", "OBJETS", "OGGETTI", "OBJETOS"
    };
    if (hcStatus) {
        snprintf(globBtn, sizeof(globBtn), "%s: %u", itemsBtnLabels[lang],
                 globalStats.totalObtained);
    } else {
        unsigned pct = globalStats.totalItems > 0 ? (globalStats.totalObtained * 100 / globalStats.totalItems) : 0;
        snprintf(globBtn, sizeof(globBtn), "%s: %u/%u (%u%%)", itemsBtnLabels[lang],
                 globalStats.totalObtained, globalStats.totalItems, pct);
    }
    C2D_DrawRectSolid(182.0f, 170.0f, 0.5f, 126.0f, 14.0f, C2D_Color32(20, 50, 90, 255));
    C2D_DrawRectSolid(183.0f, 171.0f, 0.55f, 124.0f, 12.0f, C2D_Color32(12, 30, 60, 255));
    DrawTextCentered(245.0f, 173.0f, 1.0f, globBtn, C2D_Color32(255, 215, 0, 255));

    /* Box colour is driven by the per-area debug cycle state, not by % :
     *   not downloaded            -> grey
     *   really downloaded (save)  -> green   (cycle state 0 + real dl bit)
     *   forced download  (debug)  -> purple  (cycle state 1)
     *   forced 100%      (debug)  -> orange  (cycle state 2)
     * In state 0 the downloaded bit reflects the genuine save (ClearAreaMap
     * restores it from the backup), so it is safe to read here. */
    for (int row = 0; row < 2; ++row) {
        int i0 = row == 0 ? 0 : 4, i1 = row == 0 ? 4 : 7;
        float by = row == 0 ? 188.0f : 210.0f;
        float bw = row == 0 ? 68.0f : 92.0f;
        float step = row == 0 ? 73.0f : 98.0f;
        for (int i = i0; i < i1; ++i) {
            bool dl = (gEquipment.downloadedMapStatus & (1 << i)) != 0;
            int mst = 0;
#ifdef PORT_DEBUG_TOOLS_ACTIVE
            if (Port_BottomUI_DebugTabVisible()) mst = sMapDebugState[i];
#endif
            float bx = 14.0f + (float)(i - i0) * step;
            uint32_t boxBg, boxBorder;
            bool lit;
            if (mst == 2) {                 /* forced 100% -> orange */
                boxBg = C2D_Color32(70, 45, 15, 255);
                boxBorder = C2D_Color32(255, 170, 60, 255); lit = true;
            } else if (mst == 1) {          /* forced download -> purple */
                boxBg = C2D_Color32(45, 30, 70, 255);
                boxBorder = C2D_Color32(170, 110, 240, 255); lit = true;
            } else if (dl) {                /* genuinely downloaded -> green */
                boxBg = C2D_Color32(18, 62, 32, 255);
                boxBorder = C2D_Color32(70, 220, 110, 255); lit = true;
            } else {                        /* not downloaded -> grey */
                boxBg = C2D_Color32(22, 26, 38, 255);
                boxBorder = C2D_Color32(45, 52, 70, 255); lit = false;
            }
            uint32_t textCol = lit ? C2D_Color32(255, 255, 255, 255)
                                   : C2D_Color32(90, 100, 120, 255);
            C2D_DrawRectSolid(bx, by, 0.5f, bw, 18.0f, boxBorder);
            C2D_DrawRectSolid(bx + 1.0f, by + 1.0f, 0.55f, bw - 2.0f, 16.0f, boxBg);
            DrawTextCentered(bx + bw / 2.0f, by + 5.0f, 1.0f, AreaName(i), textCol);
        }
    }

    if (sShowCollectiblesModal) {
        RenderCollectiblesModal(lang);
    }
}

/* Render Options View — 3 modal buttons + restart, no scrolling needed */
static void RenderOptionsView(void) {
    int lang = GetLang();

    const float viewX0 = 8.0f;
    const float viewY0 = 28.0f;
    const float viewW  = 304.0f;
    const float viewH  = 204.0f;
    const float viewY1 = viewY0 + viewH;

    /* Background panel */
    C2D_DrawRectSolid(viewX0, viewY0, 0.4f, viewW, viewH, C2D_Color32(14, 18, 28, 255));
    C2D_DrawRectSolid(viewX0, viewY0, 0.38f, viewW, viewH, C2D_Color32(35, 45, 70, 255));

    /* Button 1: Display settings (Y: 48 to 76, H: 28) */
    C2D_DrawRectSolid(16.0f, 48.0f, 0.5f, 288.0f, 28.0f, C2D_Color32(20, 50, 90, 255));
    C2D_DrawRectSolid(17.0f, 49.0f, 0.55f, 286.0f, 26.0f, C2D_Color32(12, 30, 60, 255));
    static const char* const displayBtnTitles[7] = {
        "DISPLAY", "DISPLAY", "DISPLAY",
        "BILDSCHIRM", "AFFICHAGE", "SCHERMO", "PANTALLA"
    };
    static const char* const displayBtnSubs[7] = {
        "LANGUAGE, ASPECT, STYLE, FPS", "LANGUAGE, ASPECT, STYLE, FPS", "LANGUAGE, ASPECT, STYLE, FPS",
        "SPRACHE, BILD, STIL, FPS", "LANGUE, FORMAT, STYLE, FPS", "LINGUA, FORMATO, STILE, FPS", "IDIOMA, ASPECTO, ESTILO, FPS"
    };
    DrawTextCentered(160.0f, 52.0f, 1.0f, displayBtnTitles[lang], C2D_Color32(100, 200, 255, 255));
    DrawTextCentered(160.0f, 63.0f, 1.0f, displayBtnSubs[lang], C2D_Color32(140, 160, 190, 255));

    /* Button 2 & 3: RetroAchievements Split into 2 side-by-side buttons (Y: 82 to 118, H: 36) */
    /* 2A: RA Settings / Login (Left: X 16 to 156, W: 140) */
    C2D_DrawRectSolid(16.0f, 82.0f, 0.5f, 140.0f, 36.0f, C2D_Color32(20, 60, 40, 255));
    C2D_DrawRectSolid(17.0f, 83.0f, 0.55f, 138.0f, 34.0f, C2D_Color32(12, 38, 24, 255));
    static const char* const raSettingsTitles[7] = {
        "RA SETTINGS", "RA SETTINGS", "RA SETTINGS",
        "RA-EINSTELL.", "PARAMETRES RA", "IMPOSTAZIONI RA", "AJUSTES RA"
    };
    DrawTextCentered(86.0f, 87.0f, 1.0f, raSettingsTitles[lang], C2D_Color32(100, 255, 160, 255));
    uint32_t raStatusCol = C2D_Color32(140, 160, 190, 255);
    switch (Port_RA_GetStatus()) {
        case RA_STATUS_CONNECTED: raStatusCol = C2D_Color32(80, 255, 120, 255); break;
        case RA_STATUS_CONNECTING: raStatusCol = C2D_Color32(255, 220, 80, 255); break;
        case RA_STATUS_ERROR: raStatusCol = C2D_Color32(255, 90, 90, 255); break;
        default: break;
    }
    DrawTextCentered(86.0f, 100.0f, 1.0f, Port_RA_GetStatusString(lang), raStatusCol);

    /* 2B: RA Achievement List & Progress (Right: X 164 to 304, W: 140) */
    C2D_DrawRectSolid(164.0f, 82.0f, 0.5f, 140.0f, 36.0f, C2D_Color32(20, 60, 40, 255));
    C2D_DrawRectSolid(165.0f, 83.0f, 0.55f, 138.0f, 34.0f, C2D_Color32(12, 38, 24, 255));
    static const char* const viewAchTitles[7] = {
        "VIEW ACHIEVEMENTS", "VIEW ACHIEVEMENTS", "VIEW ACHIEVEMENTS",
        "ERFOLGE", "VOIR SUCCES", "VEDI OBIETTIVI", "VER LOGROS"
    };
    DrawTextCentered(234.0f, 87.0f, 1.0f, viewAchTitles[lang], C2D_Color32(100, 255, 160, 255));
    uint32_t count = Port_RA_GetAchievementCount();
    uint32_t unlocked = Port_RA_GetUnlockedCount();
    char achSummary[32];
    if (count > 0) snprintf(achSummary, sizeof(achSummary), "%lu/%lu", (unsigned long)unlocked, (unsigned long)count);
    else {
        static const char* const achEmptyLabels[7] = {
            "LIST", "LIST", "LIST",
            "LISTE", "LISTE", "LISTA", "LISTA LOGROS"
        };
        snprintf(achSummary, sizeof(achSummary), "%s", achEmptyLabels[lang]);
    }
    DrawTextCentered(234.0f, 100.0f, 1.0f, achSummary, C2D_Color32(140, 240, 180, 255));

    /* Button 4: Controls remapping (Y: 124 to 156, H: 32) */
    C2D_DrawRectSolid(16.0f, 124.0f, 0.5f, 288.0f, 32.0f, C2D_Color32(50, 35, 15, 255));
    C2D_DrawRectSolid(17.0f, 125.0f, 0.55f, 286.0f, 30.0f, C2D_Color32(35, 24, 10, 255));
    static const char* const ctrlTitles[7] = {
        "CONTROLS", "CONTROLS", "CONTROLS",
        "STEUERUNG", "COMMANDES", "CONTROLLI", "CONTROLES"
    };
    static const char* const ctrlSubs[7] = {
        "REMAPPABLE BUTTONS & C-STICK", "REMAPPABLE BUTTONS & C-STICK", "REMAPPABLE BUTTONS & C-STICK",
        "TASTEN & C-STICK BELEGEN", "TOUCHES ET C-STICK", "RIMAPPA TASTI E C-STICK", "MAPEAR BOTONES Y C-STICK"
    };
    DrawTextCentered(160.0f, 129.0f, 1.0f, ctrlTitles[lang], C2D_Color32(255, 210, 80, 255));
    DrawTextCentered(160.0f, 140.0f, 1.0f, ctrlSubs[lang], C2D_Color32(180, 160, 110, 255));

    /* Restart button (Y: 164 to 192, H: 28) */
    C2D_DrawRectSolid(16.0f, 164.0f, 0.5f, 288.0f, 28.0f, C2D_Color32(70, 25, 25, 255));
    C2D_DrawRectSolid(17.0f, 165.0f, 0.55f, 286.0f, 26.0f, C2D_Color32(50, 18, 18, 255));
    C2D_DrawRectSolid(17.0f, 165.0f, 0.56f, 286.0f, 1.0f, C2D_Color32(180, 60, 60, 255));
    static const char* const restartBtnTitles[7] = {
        "RESTART GAME", "RESTART GAME", "RESTART GAME",
        "SPIEL NEUSTARTEN", "RECOMMENCER PARTIE", "RIAVVIA PARTITA", "REINICIAR PARTIDA"
    };
    DrawTextCentered(160.0f, 171.0f, 1.0f, restartBtnTitles[lang], C2D_Color32(255, 130, 130, 255));

    /* Footer */
    DrawTextCentered(160.0f, 212.0f, 1.0f, "METROID ZERO MISSION 3DS " MZM_PORT_VERSION, C2D_Color32(90, 115, 145, 255));

    /* Render active modal on top */
    if (sShowDisplayModal) RenderDisplayModal(lang);
    else if (sShowRASettingsModal) RenderRASettingsModal(lang);
    else if (sShowAchPacksModal) RenderAchPacksModal(lang);
    else if (sShowAchievementsModal) RenderAchievementsModal(lang);
    else if (sShowRemapModal) RenderRemapModal(lang);
    else if (sShowConfirmModal) RenderConfirmModal(lang);
}

#ifdef PORT_DEBUG_TOOLS_ACTIVE
/* Geometry shared by the debug modals and their touch handlers. The modal
 * body is y 26..232 with a close button at 208, so anything drawn past ~204
 * lands under it -- which is exactly what a one-row-per-tool list did once
 * there were nine tools. Everything below is laid out as a two-per-line
 * grid instead, which fits every tool on screen without scrolling. */
#define DBGTOOL_COL_L_X   16
#define DBGTOOL_COL_R_X   164
#define DBGTOOL_COL_W     140
#define DBGTOOL_GRID_Y0   46
#define DBGTOOL_GRID_PITCH 26
#define DBGTOOL_CELL_H    24
#define DBGTOOL_GRID_ROWS 6
/* Cell 11 (RENDERER GPU/CPU) only exists when the GPU tile renderer is
 * compiled in -- a RENDERER=cpu build has nothing to switch to. */
#ifdef PORT_GPU_TILE_RENDERER
#define DBGTOOL_COUNT     12
#else
#define DBGTOOL_COUNT     11
#endif

#define DBGTOOL_CELL_Y(r) ((float)(DBGTOOL_GRID_Y0 + (r) * DBGTOOL_GRID_PITCH))
#define DBGTOOL_CELL_X(c) ((float)((c) == 0 ? DBGTOOL_COL_L_X : DBGTOOL_COL_R_X))

/* A cell is a label plus a small state/affordance line under it. `accent`
 * tints the state line -- green for an active toggle, yellow for something
 * that opens a submenu, muted blue for a plain action. */
static void DrawDebugCell(int index, const char* label, const char* state, uint32_t accent) {
    float x = DBGTOOL_CELL_X(index & 1);
    float y = DBGTOOL_CELL_Y(index >> 1);
    C2D_DrawRectSolid(x, y, 0.9f, (float)DBGTOOL_COL_W, (float)DBGTOOL_CELL_H, C2D_Color32(24, 32, 50, 255));
    C2D_DrawRectSolid(x, y, 0.91f, (float)DBGTOOL_COL_W, 1.0f, C2D_Color32(50, 80, 130, 255));
    DrawTextMaxWClipped(x + 6.0f, y + 2.0f, 1.0f, label, C2D_Color32(255, 255, 255, 255),
                        0.0f, 240.0f, (float)DBGTOOL_COL_W - 12.0f);
    if (state) DrawText(x + 6.0f, y + 13.0f, 1.0f, state, accent);
}

/* The right ~48px of a cell is a start/stop side button (DebugCellRightZoneHit):
 * a divider, a tinted panel, and either a play triangle (idle) or a stop
 * square (running). The button's colour is the only running indicator -- the
 * cell's own label/state stays put. Drawn over the cell after it. */
static void DrawDebugCellSideButton(int index, bool running) {
    float xr = DBGTOOL_CELL_X(index & 1) + (float)DBGTOOL_COL_W;
    float y = DBGTOOL_CELL_Y(index >> 1);
    float cy = y + (float)DBGTOOL_CELL_H * 0.5f;
    float bx = xr - 46.0f;                 /* panel left edge */
    const uint32_t fg   = running ? C2D_Color32(255, 90, 90, 255)   /* red = tap to stop */
                                  : C2D_Color32(120, 230, 140, 255);/* green = tap to start */
    const uint32_t bg   = running ? C2D_Color32(70, 22, 22, 255)
                                  : C2D_Color32(22, 44, 30, 255);
    C2D_DrawRectSolid(bx - 2.0f, y + 3.0f, 0.92f, 1.0f, (float)DBGTOOL_CELL_H - 6.0f,
                      C2D_Color32(90, 110, 150, 255));
    C2D_DrawRectSolid(bx, y + 3.0f, 0.92f, 44.0f, (float)DBGTOOL_CELL_H - 6.0f, bg);
    float mx = bx + 22.0f;                 /* glyph centre */
    if (running) {
        C2D_DrawRectSolid(mx - 5.0f, cy - 5.0f, 0.93f, 10.0f, 10.0f, fg);
    } else {
        C2D_DrawTriangle(mx - 4.0f, cy - 6.0f, fg,
                         mx - 4.0f, cy + 6.0f, fg,
                         mx + 6.0f, cy,        fg, 0.93f);
    }
}

/* Stream the LOG A SD cell has selected (never NONE -- on/off is the side
 * button). Middle taps cycle it; when logging is on it is applied live. */
static PortDebugLogMode sLogSel = PORT_LOG_MODE_ALL;

static const char* DebugLogSelName(void) {
    switch (sLogSel) {
        case PORT_LOG_MODE_GPU:   return "GPU";
        case PORT_LOG_MODE_AUDIO: return "AUDIO";
        case PORT_LOG_MODE_PERF:  return "PERF";
        default:                  return "ALL";
    }
}

/* Index of the grid cell a tap landed on, or -1. */
static int DebugCellHit(int x, int y, int count) {
    int col;
    if (x >= DBGTOOL_COL_L_X && x <= DBGTOOL_COL_L_X + DBGTOOL_COL_W) col = 0;
    else if (x >= DBGTOOL_COL_R_X && x <= DBGTOOL_COL_R_X + DBGTOOL_COL_W) col = 1;
    else return -1;
    for (int r = 0; r < DBGTOOL_GRID_ROWS; ++r) {
        int cy = DBGTOOL_GRID_Y0 + r * DBGTOOL_GRID_PITCH;
        if (y < cy || y > cy + DBGTOOL_CELL_H) continue;
        int idx = r * 2 + col;
        return (idx < count) ? idx : -1;
    }
    return -1;
}

/* Single-row helper, still used by the warp submenu's spinner rows. */
#define DBGTOOL_ROW_X0    16
#define DBGTOOL_ROW_X1    304
#define DBGTOOL_ROW_Y0    48
#define DBGTOOL_ROW_PITCH 21
#define DBGTOOL_ROW_H     20
#define DBGTOOL_ROW_Y(i) ((float)(DBGTOOL_ROW_Y0 + (i) * DBGTOOL_ROW_PITCH))

static void DrawDebugRow(int i, const char* label, const char* value, uint32_t valueCol) {
    float ry = DBGTOOL_ROW_Y(i);
    C2D_DrawRectSolid((float)DBGTOOL_ROW_X0, ry, 0.9f,
                      (float)(DBGTOOL_ROW_X1 - DBGTOOL_ROW_X0), (float)DBGTOOL_ROW_H,
                      C2D_Color32(24, 32, 50, 255));
    DrawText((float)DBGTOOL_ROW_X0 + 8.0f, ry + 6.0f, 1.0f, label, C2D_Color32(255, 255, 255, 255));
    if (value) DrawText(224.0f, ry + 6.0f, 1.0f, value, valueCol);
}

static int DebugRowHit(int x, int y, int rowCount) {
    if (x < DBGTOOL_ROW_X0 || x > DBGTOOL_ROW_X1) return -1;
    for (int i = 0; i < rowCount; ++i) {
        int ry = DBGTOOL_ROW_Y0 + i * DBGTOOL_ROW_PITCH;
        if (y >= ry && y <= ry + DBGTOOL_ROW_H) return i;
    }
    return -1;
}

static void DrawDebugModalFrame(int lang, const char* titleEs, const char* titleEn) {
    C2D_DrawRectSolid(10.0f, 26.0f, 0.85f, 300.0f, 210.0f, C2D_Color32(10, 14, 24, 250));
    C2D_DrawRectSolid(10.0f, 26.0f, 0.84f, 300.0f, 210.0f, C2D_Color32(40, 70, 120, 255));
    DrawText(20.0f, 32.0f, 1.0f, (lang == 6) ? titleEs : titleEn, C2D_Color32(255, 215, 0, 255));
    C2D_DrawRectSolid(100.0f, 214.0f, 0.9f, 120.0f, 20.0f, C2D_Color32(20, 70, 130, 255));
    DrawTextCentered(160.0f, 219.0f, 1.0f, (lang == 6) ? "CERRAR" : "CLOSE", C2D_Color32(255, 255, 255, 255));
}

static bool DebugCloseHit(int x, int y) {
    return (x >= 100 && x <= 220 && y >= 214 && y <= 234);
}

static void RenderDebugToolsModal(int lang) {
    DrawDebugModalFrame(lang, "HERRAMIENTAS DE DEPURACION", "DEBUG TOOLS");

    const bool rec = PlatformGpu3DS_IsRecording();
    const bool perf = PlatformGpu3DS_IsPerfRecording();
    const char* onTxt = (lang == 6) ? "ACTIVO" : "ON";
    const char* offTxt = (lang == 6) ? "PARADO" : "OFF";
    const uint32_t colAct = C2D_Color32(140, 170, 210, 255);
    const uint32_t colOn = C2D_Color32(80, 255, 120, 255);
    const uint32_t colMenu = C2D_Color32(255, 215, 0, 255);

    DrawDebugCell(0, (lang == 6) ? "VOLCADO PANTALLA" : "SCREEN DUMP",
                  (lang == 6) ? "VOLCAR" : "DUMP", colAct);
    DrawDebugCell(1, (lang == 6) ? "MARCA EN EL LOG" : "LOG MARKER",
                  (lang == 6) ? "MARCAR" : "MARK", colAct);
    /* Middle taps the state line to cycle the capture preset (always shown
     * in yellow); the side button starts/stops -- its colour is the running
     * indicator, so the cell text never changes. */
    DrawDebugCell(2, (lang == 6) ? "GRAB. ESCENA" : "SCENE RECORDER",
                  PlatformGpu3DS_RecordPresetLabel(), colMenu);
    DrawDebugCellSideButton(2, rec);
    DrawDebugCell(3, (lang == 6) ? "GRAB. RENDIMIENTO" : "PERF RECORDER",
                  perf ? onTxt : offTxt, perf ? colOn : colAct);
    DrawDebugCell(4, (lang == 6) ? "ATLAS GPU" : "GPU ATLAS",
                  (lang == 6) ? "VOLCAR" : "DUMP", colAct);
    DrawDebugCell(5, (lang == 6) ? "MATAR A SAMUS" : "KILL SAMUS",
                  (lang == 6) ? "MATAR" : "KILL", C2D_Color32(255, 130, 130, 255));
    DrawDebugCell(6, (lang == 6) ? "TELETRANSPORTE" : "WARP", ">", colMenu);
    DrawDebugCell(7, (lang == 6) ? "EQUIPO Y OBJETOS" : "EQUIPMENT", ">", colMenu);
    DrawDebugCell(8, (lang == 6) ? "REVELAR MAPAS" : "REVEAL MAPS",
                  (lang == 6) ? "REVELAR" : "REVEAL", colMenu);

    /* Nothing is written to the SD card while LOG is stopped even in a debug
     * build (see port_debug_log.h). Tapping the middle of LOG A SD cycles
     * the stream filter -- OFF / ALL / GPU / AUDIO / PERF (the label keeps
     * showing the pick); the side button starts/stops logging and its
     * colour is the running indicator. BUFFER is "stop holding lines in
     * RAM" for a hang. */
    const bool logOn = Port_DebugLog_IsEnabled();
    const bool logBuf = Port_DebugLog_IsBuffered();
    DrawDebugCell(9, (lang == 6) ? "LOG A SD" : "SD LOGGING",
                  DebugLogSelName(), colMenu);
    DrawDebugCellSideButton(9, logOn);
    DrawDebugCell(10, (lang == 6) ? "LOG EN BUFFER" : "LOG BUFFERING",
                  logBuf ? onTxt : offTxt, logBuf ? colOn : colAct);

#ifdef PORT_GPU_TILE_RENDERER
    /* Force the whole top screen through the CPU scanline renderer
     * (port/ppu/src/mode1.c) instead of the PICA200 tile renderer, to
     * isolate renderer-specific bugs without a RENDERER=cpu rebuild. The
     * per-frame CanRenderFrame() fallback still applies on top of this. */
    {
        const bool gpuOn = Port_GpuRenderer_IsActive();
        DrawDebugCell(11, (lang == 6) ? "RENDERER" : "RENDERER",
                      gpuOn ? "GPU" : "CPU", gpuOn ? colAct : colOn);
    }
#endif

    if (sDebugToolsMsg[0] && sFrameCounter < sDebugToolsMsgUntil) {
        DrawTextCentered(160.0f, 204.0f, 1.0f, sDebugToolsMsg, C2D_Color32(120, 255, 160, 255));
    }
}

/* True when the tap is in the right ~48px of grid cell `cell` -- the
 * start/stop side button (DrawDebugCellSideButton). The rest of the cell
 * cycles that cell's option. */
static bool DebugCellRightZoneHit(int x, int cell) {
    float cx = DBGTOOL_CELL_X(cell & 1);
    return (float)x >= cx + (float)DBGTOOL_COL_W - 48.0f;
}

static bool HandleDebugToolsModalTouch(int x, int y) {
    if (DebugCloseHit(x, y)) {
        sShowDebugToolsModal = false;
        return true;
    }
    int cell = DebugCellHit(x, y, DBGTOOL_COUNT);
    /* SCENE RECORDER / SD LOGGING: the side button starts/stops; the rest of
     * the cell cycles the option. */
    if (cell == 2 && DebugCellRightZoneHit(x, 2)) {
        PlatformGpu3DS_ToggleRecording();
        const char* last = PlatformGpu3DS_RecordLastFile();
        DebugToolsSetMsg(PlatformGpu3DS_IsRecording() ? "REC ON"
                         : (last && last[0]) ? last : "REC OFF");
        return true;
    }
    if (cell == 9 && DebugCellRightZoneHit(x, 9)) {
        /* Side button: start/stop logging on the selected stream. */
        if (Port_DebugLog_IsEnabled()) {
            Port_DebugLog_SetMode(PORT_LOG_MODE_NONE);
            DebugToolsSetMsg("LOG SD: OFF");
        } else {
            Port_DebugLog_SetMode(sLogSel);
            Port_DebugLog("USER MARK: SD logging enabled");
            char msg[24];
            snprintf(msg, sizeof(msg), "LOG SD: %s", DebugLogSelName());
            DebugToolsSetMsg(msg);
        }
        return true;
    }
    switch (cell) {
        case 0:
            PlatformGpu3DS_DumpScreens();
            DebugToolsSetMsg("DUMP -> sdmc:/3ds/");
            break;
        case 1:
            Port_DebugLog("USER MARK: debug tools menu");
            DebugToolsSetMsg("MARCA EN EL LOG");
            break;
        case 2:
            /* Middle: cycle the capture preset (no-op while recording). */
            PlatformGpu3DS_CycleRecordPreset();
            DebugToolsSetMsg(PlatformGpu3DS_RecordPresetLabel());
            break;
        case 3:
            PlatformGpu3DS_TogglePerfRecording();
            DebugToolsSetMsg(PlatformGpu3DS_IsPerfRecording() ? "PERF ON" : "PERF OFF");
            break;
        case 4:
            Port_GpuRenderer_DumpAtlas("sdmc:/3ds/mzm-live-atlas.ppm", "sdmc:/3ds/mzm-live-atlas-keys.csv");
            DebugToolsSetMsg("ATLAS -> sdmc:/3ds/");
            break;
        case 5:
            PortPpuMzm_DebugKillSamus();
            DebugToolsSetMsg("SAMUS MUERTA");
            break;
        case 6:
            sShowDebugToolsModal = false;
            sShowDebugWarpModal = true;
            break;
        case 7:
            sShowDebugToolsModal = false;
            sShowDebugEquipModal = true;
            break;
        case 8:
            PortPpuMzm_DebugRevealAllMaps();
            /* The MAP tab caches the decompressed tiles of whichever area it
             * last drew that wasn't the current one; that copy is now stale. */
            sCachedOtherArea = 0xFF;
            DebugToolsSetMsg("MAPAS REVELADOS");
            break;
        case 9: {
            /* Middle: cycle the stream selection (ALL/GPU/AUDIO/PERF, no
             * OFF -- the side button does on/off). Applied live if logging
             * is running. */
            sLogSel = (PortDebugLogMode)(sLogSel + 1);
            if (sLogSel >= PORT_LOG_MODE_COUNT) sLogSel = PORT_LOG_MODE_ALL;
            if (Port_DebugLog_IsEnabled()) Port_DebugLog_SetMode(sLogSel);
            char msg[24];
            snprintf(msg, sizeof(msg), "LOG SD: %s", DebugLogSelName());
            DebugToolsSetMsg(msg);
            break;
        }
        case 10: {
            const bool buf = !Port_DebugLog_IsBuffered();
            Port_DebugLog_SetBuffered(buf);
            DebugToolsSetMsg(buf ? "LOG EN BUFFER" : "LOG DIRECTO");
            break;
        }
#ifdef PORT_GPU_TILE_RENDERER
        case 11: {
            const bool gpuOn = !Port_GpuRenderer_IsActive();
            Port_GpuRenderer_SetActive(gpuOn);
            Port_DebugLog(gpuOn ? "USER MARK: renderer -> GPU"
                                : "USER MARK: renderer -> CPU");
            DebugToolsSetMsg(gpuOn ? "RENDERER: GPU" : "RENDERER: CPU");
            break;
        }
#endif
        default:
            break;
    }
    return true;
}

/* Warp submenu. Jumps are addressed by (area, door id) -- see the long
 * comment on the warp block in port_ppu_mzm.c for why a door and not a room
 * number plus coordinates. The door spinner shows which room each door
 * leads into, so a room can be found by stepping doors without knowing any
 * door ids beforehand. */
#define DBGWARP_ROW_COUNT 6

static void RenderDebugWarpModal(int lang) {
    DrawDebugModalFrame(lang, "TELETRANSPORTE", "WARP");

    int selArea = 0, selDoor = 0, selRoom = -1;
    PortPpuMzm_DebugGetWarpSelection(&selArea, &selDoor, &selRoom);
    const int doorCount = PortPpuMzm_DebugGetDoorCount(selArea);
    const bool hasWarp = PortPpuMzm_DebugHasWarpPoint();

    char valBuf[24];

    /* Row 0/1: the spinner. The whole row is a hit target split in three:
     * left third = previous, right third = next, middle = nothing, so a
     * mis-tap in the middle does nothing rather than jumping a step. */
    DrawDebugRow(0, (lang == 6) ? "AREA" : "AREA", NULL, 0);
    DrawText(120.0f, DBGTOOL_ROW_Y(0) + 6.0f, 1.0f, "<", C2D_Color32(255, 215, 0, 255));
    DrawText(140.0f, DBGTOOL_ROW_Y(0) + 6.0f, 1.0f, AreaName((uint8_t)selArea), C2D_Color32(220, 235, 255, 255));
    DrawText(288.0f, DBGTOOL_ROW_Y(0) + 6.0f, 1.0f, ">", C2D_Color32(255, 215, 0, 255));

    snprintf(valBuf, sizeof(valBuf), "%d / %d", selDoor, doorCount > 0 ? doorCount - 1 : 0);
    DrawDebugRow(1, (lang == 6) ? "PUERTA" : "DOOR", NULL, 0);
    DrawText(120.0f, DBGTOOL_ROW_Y(1) + 6.0f, 1.0f, "<", C2D_Color32(255, 215, 0, 255));
    DrawText(140.0f, DBGTOOL_ROW_Y(1) + 6.0f, 1.0f, valBuf, C2D_Color32(220, 235, 255, 255));
    DrawText(288.0f, DBGTOOL_ROW_Y(1) + 6.0f, 1.0f, ">", C2D_Color32(255, 215, 0, 255));

    if (selRoom >= 0) snprintf(valBuf, sizeof(valBuf), "%s %d", (lang == 6) ? "SALA" : "ROOM", selRoom);
    else snprintf(valBuf, sizeof(valBuf), "--");
    DrawDebugRow(2, (lang == 6) ? "IR A ESA PUERTA" : "GO TO THAT DOOR",
                 valBuf, selRoom >= 0 ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(110, 120, 140, 255));

    DrawDebugRow(3, (lang == 6) ? "GUARDAR PUNTO AQUI" : "SAVE POINT HERE",
                 (lang == 6) ? "GUARDAR" : "SAVE", C2D_Color32(255, 215, 0, 255));
    DrawDebugRow(4, (lang == 6) ? "IR AL PUNTO GUARDADO" : "GO TO SAVED POINT",
                 hasWarp ? ((lang == 6) ? "IR" : "GO") : "--",
                 hasWarp ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(110, 120, 140, 255));

    DrawDebugRow(5, (lang == 6) ? "IR TOCANDO EL MAPA" : "WARP BY TOUCHING MAP",
                 sDebugMapWarpArmed ? ((lang == 6) ? "ARMADO" : "ARMED")
                                    : ((lang == 6) ? "ARMAR" : "ARM"),
                 sDebugMapWarpArmed ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 215, 0, 255));

    char warpInfo[48];
    PortPpuMzm_DebugGetWarpPointInfo(warpInfo, (int)sizeof(warpInfo));
    DrawText((float)DBGTOOL_ROW_X0, 176.0f, 1.0f, warpInfo, C2D_Color32(150, 190, 240, 255));

    /* Current position, so "save point here" is verifiable before pressing
     * it and the spinner has something to aim at. */
    char hereBuf[64];
    snprintf(hereBuf, sizeof(hereBuf), "%s: %s %s %u",
             (lang == 6) ? "AHORA" : "NOW", AreaName(gCurrentArea),
             (lang == 6) ? "SALA" : "ROOM", gCurrentRoom);
    DrawText((float)DBGTOOL_ROW_X0, 187.0f, 1.0f, hereBuf, C2D_Color32(150, 190, 240, 255));

    if (sDebugToolsMsg[0] && sFrameCounter < sDebugToolsMsgUntil) {
        DrawTextCentered(160.0f, 198.0f, 1.0f, sDebugToolsMsg, C2D_Color32(120, 255, 160, 255));
    }
}

static void HandleDebugWarpModalTouch(int x, int y) {
    if (DebugCloseHit(x, y)) {
        sShowDebugWarpModal = false;
        return;
    }
    int row = DebugRowHit(x, y, DBGWARP_ROW_COUNT);
    const int step = (x < 140) ? -1 : (x > 270 ? 1 : 0);
    switch (row) {
        case 0:
            if (step) PortPpuMzm_DebugStepWarpArea(step);
            break;
        case 1:
            if (step) PortPpuMzm_DebugStepWarpDoor(step);
            break;
        case 2:
            /* The jump itself is deferred to src/agbmain.c's loop (see
             * PortPpuMzm_DebugApplyPendingWarp). Closing the modal so the
             * top screen is watchable the moment the room reloads. */
            if (PortPpuMzm_DebugRequestWarpToSelection()) {
                DebugToolsSetMsg("TELETRANSPORTANDO...");
                sShowDebugWarpModal = false;
            } else {
                DebugToolsSetMsg("PUERTA NO VALIDA");
            }
            break;
        case 3:
            PortPpuMzm_DebugSaveWarpPoint();
            DebugToolsSetMsg("PUNTO GUARDADO");
            break;
        case 4:
            if (PortPpuMzm_DebugRequestWarp()) {
                DebugToolsSetMsg("TELETRANSPORTANDO...");
                sShowDebugWarpModal = false;
            } else {
                DebugToolsSetMsg("NO HAY PUNTO GUARDADO");
            }
            break;
        case 5:
            /* Arms the MAP tab's tap-to-warp and gets out of the way, so the
             * next thing the player touches is the map itself. */
            sDebugMapWarpArmed = !sDebugMapWarpArmed;
            if (sDebugMapWarpArmed) {
                sShowDebugWarpModal = false;
                sCurrentTab = BOTTOM_TAB_MAP;
                DebugToolsSetMsg("TOCA UN PUNTO DEL MAPA");
            }
            break;
        default:
            break;
    }
}

/* Equipment editor. Every flag is a two-state chip in a two-column grid;
 * the bottom rows are the bulk actions. The bit constants are duplicated
 * from include/constants/samus.h (the BBF_ and SMF_ enums) rather than
 * included,
 * because this file can't pull in the GBA-side headers -- same <3ds.h>
 * typedef clash that keeps all the game-state pokes in port_ppu_mzm.c. */
#define DBG_BBF_LONG_BEAM   (1u << 0)
#define DBG_BBF_ICE_BEAM    (1u << 1)
#define DBG_BBF_WAVE_BEAM   (1u << 2)
#define DBG_BBF_PLASMA_BEAM (1u << 3)
#define DBG_BBF_CHARGE_BEAM (1u << 4)
#define DBG_BBF_BOMBS       (1u << 7)

#define DBG_SMF_HIGH_JUMP    (1u << 0)
#define DBG_SMF_SPEEDBOOSTER (1u << 1)
#define DBG_SMF_SPACE_JUMP   (1u << 2)
#define DBG_SMF_SCREW_ATTACK (1u << 3)
#define DBG_SMF_VARIA_SUIT   (1u << 4)
#define DBG_SMF_GRAVITY_SUIT (1u << 5)
#define DBG_SMF_MORPH_BALL   (1u << 6)
#define DBG_SMF_POWER_GRIP   (1u << 7)

/* isBeam picks which of the two equipment bytes the bit belongs to. */
struct DebugEquipEntry { const char* es; const char* en; unsigned bit; bool isBeam; };

static const struct DebugEquipEntry kDebugEquipLeft[] = {
    { "LARGO",   "LONG",   DBG_BBF_LONG_BEAM,   true },
    { "HIELO",   "ICE",    DBG_BBF_ICE_BEAM,    true },
    { "ONDA",    "WAVE",   DBG_BBF_WAVE_BEAM,   true },
    { "PLASMA",  "PLASMA", DBG_BBF_PLASMA_BEAM, true },
    { "CARGA",   "CHARGE", DBG_BBF_CHARGE_BEAM, true },
    { "BOMBAS",  "BOMBS",  DBG_BBF_BOMBS,       true },
    { "MORFO",   "MORPH",  DBG_SMF_MORPH_BALL,  false },
};

static const struct DebugEquipEntry kDebugEquipRight[] = {
    { "SALTO ALTO", "HIGH JUMP",  DBG_SMF_HIGH_JUMP,    false },
    { "TURBO",      "SPEED",      DBG_SMF_SPEEDBOOSTER, false },
    { "ESPACIAL",   "SPACE JUMP", DBG_SMF_SPACE_JUMP,   false },
    { "TORNILLO",   "SCREW",      DBG_SMF_SCREW_ATTACK, false },
    { "VARIA",      "VARIA",      DBG_SMF_VARIA_SUIT,   false },
    { "GRAVEDAD",   "GRAVITY",    DBG_SMF_GRAVITY_SUIT, false },
    { "AGARRE",     "POWER GRIP", DBG_SMF_POWER_GRIP,   false },
};

#define DBGEQUIP_ROWS      7
#define DBGEQUIP_COL_W     140
#define DBGEQUIP_COL_L_X   16
#define DBGEQUIP_COL_R_X   164
#define DBGEQUIP_GRID_Y0   46
#define DBGEQUIP_GRID_PITCH 17
#define DBGEQUIP_CELL_H    16
/* Bulk-action rows, below the grid. The second one has to end before the
 * frame's close button at y=208, which is what the previous layout ran
 * into. */
#define DBGEQUIP_ACT_Y0    166
#define DBGEQUIP_ACT_H     18
#define DBGEQUIP_ACT2_Y0   186

static bool DebugEquipHas(const struct DebugEquipEntry* e, unsigned beams, unsigned misc) {
    return ((e->isBeam ? beams : misc) & e->bit) != 0;
}

static void DrawEquipCell(float x, float y, const struct DebugEquipEntry* e, int lang,
                          unsigned beams, unsigned misc) {
    bool on = DebugEquipHas(e, beams, misc);
    C2D_DrawRectSolid(x, y, 0.9f, (float)DBGEQUIP_COL_W, (float)DBGEQUIP_CELL_H,
                      on ? C2D_Color32(20, 60, 35, 255) : C2D_Color32(24, 32, 50, 255));
    DrawText(x + 6.0f, y + 5.0f, 1.0f, (lang == 6) ? e->es : e->en,
             on ? C2D_Color32(120, 255, 160, 255) : C2D_Color32(140, 150, 170, 255));
    DrawText(x + (float)DBGEQUIP_COL_W - 26.0f, y + 5.0f, 1.0f, on ? "ON" : "--",
             on ? C2D_Color32(120, 255, 160, 255) : C2D_Color32(110, 120, 140, 255));
}

static void DrawEquipAction(float x, float y, float w, const char* label, uint32_t col) {
    C2D_DrawRectSolid(x, y, 0.9f, w, (float)DBGEQUIP_ACT_H, C2D_Color32(28, 44, 70, 255));
    DrawTextCentered(x + w / 2.0f, y + 5.0f, 1.0f, label, col);
}

static void RenderDebugEquipModal(int lang) {
    DrawDebugModalFrame(lang, "EQUIPO Y OBJETOS", "EQUIPMENT & ITEMS");

    unsigned beams = 0, misc = 0;
    PortPpuMzm_DebugGetEquipment(&beams, &misc);

    for (int i = 0; i < DBGEQUIP_ROWS; ++i) {
        float y = (float)(DBGEQUIP_GRID_Y0 + i * DBGEQUIP_GRID_PITCH);
        DrawEquipCell((float)DBGEQUIP_COL_L_X, y, &kDebugEquipLeft[i], lang, beams, misc);
        DrawEquipCell((float)DBGEQUIP_COL_R_X, y, &kDebugEquipRight[i], lang, beams, misc);
    }

    DrawEquipAction((float)DBGEQUIP_COL_L_X, (float)DBGEQUIP_ACT_Y0, 140.0f,
                    (lang == 6) ? "TODO ON" : "ALL ON", C2D_Color32(120, 255, 160, 255));
    DrawEquipAction((float)DBGEQUIP_COL_R_X, (float)DBGEQUIP_ACT_Y0, 140.0f,
                    (lang == 6) ? "TODO OFF" : "ALL OFF", C2D_Color32(255, 150, 150, 255));

    float ay = (float)DBGEQUIP_ACT2_Y0;
    DrawEquipAction((float)DBGEQUIP_COL_L_X, ay, 90.0f,
                    (lang == 6) ? "MUNIC MAX" : "AMMO MAX", C2D_Color32(255, 215, 0, 255));
    DrawEquipAction((float)(DBGEQUIP_COL_L_X + 94), ay, 90.0f,
                    (lang == 6) ? "MUNIC MIN" : "AMMO MIN", C2D_Color32(255, 150, 150, 255));
    DrawEquipAction((float)(DBGEQUIP_COL_L_X + 188), ay, 100.0f,
                    (lang == 6) ? "RELLENAR" : "REFILL", C2D_Color32(120, 255, 160, 255));

    /* Deliberately no energy/missile readout here: the game's own HUD on the
     * top screen already shows all of it, and the line collided with the
     * action buttons. */
}

static void HandleDebugEquipModalTouch(int x, int y) {
    if (DebugCloseHit(x, y)) {
        sShowDebugEquipModal = false;
        return;
    }

    /* Toggle grid */
    for (int i = 0; i < DBGEQUIP_ROWS; ++i) {
        int cy = DBGEQUIP_GRID_Y0 + i * DBGEQUIP_GRID_PITCH;
        if (y < cy || y > cy + DBGEQUIP_CELL_H) continue;
        const struct DebugEquipEntry* e = NULL;
        if (x >= DBGEQUIP_COL_L_X && x <= DBGEQUIP_COL_L_X + DBGEQUIP_COL_W) e = &kDebugEquipLeft[i];
        else if (x >= DBGEQUIP_COL_R_X && x <= DBGEQUIP_COL_R_X + DBGEQUIP_COL_W) e = &kDebugEquipRight[i];
        if (!e) return;
        if (e->isBeam) PortPpuMzm_DebugToggleBeam(e->bit);
        else PortPpuMzm_DebugToggleMisc(e->bit);
        return;
    }

    /* Bulk equipment rows */
    if (y >= DBGEQUIP_ACT_Y0 && y <= DBGEQUIP_ACT_Y0 + DBGEQUIP_ACT_H) {
        if (x >= DBGEQUIP_COL_L_X && x <= DBGEQUIP_COL_L_X + DBGEQUIP_COL_W) {
            PortPpuMzm_DebugSetAllEquipment(true);
            DebugToolsSetMsg("EQUIPO COMPLETO");
        } else if (x >= DBGEQUIP_COL_R_X && x <= DBGEQUIP_COL_R_X + DBGEQUIP_COL_W) {
            PortPpuMzm_DebugSetAllEquipment(false);
            DebugToolsSetMsg("EQUIPO VACIADO");
        }
        return;
    }

    int ay = DBGEQUIP_ACT2_Y0;
    if (y >= ay && y <= ay + DBGEQUIP_ACT_H) {
        if (x >= DBGEQUIP_COL_L_X && x < DBGEQUIP_COL_L_X + 94) {
            PortPpuMzm_DebugSetAmmo(true);
            DebugToolsSetMsg("MUNICION AL MAXIMO");
        } else if (x >= DBGEQUIP_COL_L_X + 94 && x < DBGEQUIP_COL_L_X + 188) {
            PortPpuMzm_DebugSetAmmo(false);
            DebugToolsSetMsg("MUNICION A CERO");
        } else if (x >= DBGEQUIP_COL_L_X + 188 && x <= DBGEQUIP_COL_L_X + 288) {
            PortPpuMzm_DebugRefillAmmo();
            DebugToolsSetMsg("RECARGADO");
        }
    }
}
#endif /* PORT_DEBUG_TOOLS_ACTIVE */

/* Render Debug View with Map Tile Inspector */
static void RenderDebugView(void) {
    C2D_DrawRectSolid(8.0f, 28.0f, 0.4f, 304.0f, 204.0f, C2D_Color32(14, 18, 28, 255));
    C2D_DrawRectSolid(8.0f, 28.0f, 0.35f, 304.0f, 204.0f, C2D_Color32(40, 60, 90, 255));

    DrawText(16.0f, 32.0f, 1.0f, "PERFORMANCE & MAP DEBUG (" MZM_PORT_VERSION ")", C2D_Color32(100, 220, 255, 255));

    char buf[80];
    double fps = Port_PPU_3DS_CurrentFps();
    unsigned roundedFps = fps > 0.0 ? (unsigned)(fps + 0.5) : 0u;
    if (roundedFps > 999u) roundedFps = 999u;
    const bool usedGpu = Port_PPU_3DS_LastFrameUsedGpu();
    const bool isNew3ds = Platform3DS_IsNew3DS();

    snprintf(buf, sizeof(buf), "FPS: %u   RENDER: %s   SYS: %s",
             roundedFps, usedGpu ? "GPU" : "CPU", isNew3ds ? "N3DS" : "O3DS");
    DrawText(16.0f, 46.0f, 1.0f, buf, C2D_Color32(255, 255, 255, 255));

    snprintf(buf, sizeof(buf), "GAME STATE: AREA=%s (%u) ROOM=%u",
             AreaName(gCurrentArea), gCurrentArea, gCurrentRoom);
    DrawText(16.0f, 58.0f, 1.0f, buf, C2D_Color32(180, 240, 160, 255));

    snprintf(buf, sizeof(buf), "MAP COORDS: X=%u Y=%u", gMinimapX, gMinimapY);
    DrawText(16.0f, 70.0f, 1.0f, buf, C2D_Color32(180, 240, 160, 255));

    /* Current room raw tile data & surrounding 3x3 inspect */
    uint16_t curTile = gDecompressedMinimapVisitedTiles[gMinimapX + gMinimapY * 32];
    uint16_t baseTile = gDecompressedMinimapData[gMinimapX + gMinimapY * 32];
    snprintf(buf, sizeof(buf), "CURRENT TILE: VISITED=0x%04X (T=%u P=%u)",
             curTile, curTile & 0x3FF, (curTile >> 12) & 0xF);
    DrawText(16.0f, 82.0f, 1.0f, buf, C2D_Color32(255, 220, 100, 255));

    snprintf(buf, sizeof(buf), "BASE MAP TILE: 0x%04X (T=%u P=%u)",
             baseTile, baseTile & 0x3FF, (baseTile >> 12) & 0xF);
    DrawText(16.0f, 93.0f, 1.0f, buf, C2D_Color32(255, 200, 80, 255));

    /* 3x3 Grid surrounding Samus */
    DrawText(16.0f, 107.0f, 1.0f, "SURROUNDING 3x3 TILES:", C2D_Color32(140, 200, 255, 255));
    for (int dy = -1; dy <= 1; ++dy) {
        int ty = (int)gMinimapY + dy;
        char rowBuf[64];
        if (ty >= 0 && ty < 32) {
            uint16_t t0 = ((int)gMinimapX - 1 >= 0) ? gDecompressedMinimapVisitedTiles[((int)gMinimapX - 1) + ty * 32] : 0;
            uint16_t t1 = gDecompressedMinimapVisitedTiles[gMinimapX + ty * 32];
            uint16_t t2 = ((int)gMinimapX + 1 < 32) ? gDecompressedMinimapVisitedTiles[((int)gMinimapX + 1) + ty * 32] : 0;
            snprintf(rowBuf, sizeof(rowBuf), "Y%+d: [0x%04X] [0x%04X] [0x%04X]", dy, t0, t1, t2);
        } else {
            snprintf(rowBuf, sizeof(rowBuf), "Y%+d: OUT OF BOUNDS", dy);
        }
        DrawText(20.0f, 118.0f + (float)(dy + 1) * 11.0f, 1.0f, rowBuf, C2D_Color32(200, 220, 240, 255));
    }

    /* Active Chozo target status */
    uint8_t tArea = 0, tX = 0, tY = 0;
    bool hasTarget = GetActiveChozoTarget(&tArea, &tX, &tY);
    if (hasTarget) {
        snprintf(buf, sizeof(buf), "CHOZO TARGET: AREA=%s (%u) X=%u Y=%u", AreaName(tArea), tArea, tX, tY);
        DrawText(16.0f, 158.0f, 1.0f, buf, C2D_Color32(255, 120, 120, 255));
    } else {
        DrawText(16.0f, 158.0f, 1.0f, "CHOZO TARGET: NONE ACTIVE", C2D_Color32(140, 150, 170, 255));
    }

    /* Check obtained items count */
    uint32_t totalItems = 0;
    for (int a = 0; a < 8; ++a) {
        for (int row = 0; row < 32; ++row) {
            uint32_t bits = gMinimapTilesWithObtainedItems[a * 32 + row];
            while (bits) {
                if (bits & 1) totalItems++;
                bits >>= 1;
            }
        }
    }
    snprintf(buf, sizeof(buf), "TOTAL ITEMS OBTAINED: %lu", (unsigned long)totalItems);
    DrawText(16.0f, 171.0f, 1.0f, buf, C2D_Color32(180, 240, 160, 255));

    /* RetroAchievements Network & Session Diagnostics */
    char raDiag[80];
    snprintf(raDiag, sizeof(raDiag), "RA: %s (%s)", Port_RA_GetStatusString(GetLang()), Port_RA_GetLastDebugLog());
    DrawText(16.0f, 186.0f, 1.0f, raDiag, C2D_Color32(255, 220, 100, 255));

#ifdef PORT_DEBUG_TOOLS_ACTIVE
    /* [HERRAMIENTAS] button (Y: 198 to 224). Only exists in a DEBUG_TOOLS /
     * *_DIAG_LOG build -- a production build has no way to reach any of the
     * actions behind it, matching how the L+R+<btn> combos are compiled out
     * entirely rather than just hidden (see port_debug_tools.h). */
    C2D_DrawRectSolid(16.0f, 198.0f, 0.5f, 288.0f, 26.0f, C2D_Color32(30, 55, 90, 255));
    C2D_DrawRectSolid(17.0f, 199.0f, 0.55f, 286.0f, 24.0f, C2D_Color32(18, 34, 58, 255));
    C2D_DrawRectSolid(17.0f, 199.0f, 0.56f, 286.0f, 1.0f, C2D_Color32(90, 160, 240, 255));
    DrawTextCentered(160.0f, 205.0f, 1.0f,
        (GetLang() == 6) ? "HERRAMIENTAS DE DEPURACION" : "DEBUG TOOLS",
        C2D_Color32(150, 210, 255, 255));

    if (sShowDebugToolsModal) RenderDebugToolsModal(GetLang());
    else if (sShowDebugWarpModal) RenderDebugWarpModal(GetLang());
    else if (sShowDebugEquipModal) RenderDebugEquipModal(GetLang());
#else
    DrawText(16.0f, 212.0f, 1.0f, (GetLang() == 6) ? "TOCA LA PESTANA [MAPA] PARA VOLVER" : "TOUCH [MAP] TAB TO RETURN TO MAP VIEW", C2D_Color32(120, 140, 170, 255));
#endif
}

void Port_BottomUI_Render(void) {
    /* sFrameCounter is advanced by Port_BottomUI_FrameTick every frame, not
     * here -- this function is throttled (see Port_BottomUI_WantsRedraw). */

    /* Background clear for UI — blinks during low health to catch attention.
     * Blink pattern: 14 frames ON (warning color), 14 frames dimmed.
     * The entire screen background participates so even a glance shows it. */
    {
        bool inRealGameplay = ((uint8_t)gMainGameMode == 4 /* GM_INGAME */);
        uint16_t curE = gEquipment.currentEnergy;
        bool blinkOn = ((sFrameCounter & 0x0F) < 8);
        uint32_t bgCol;
        if (inRealGameplay && curE < 30) {
            bgCol = blinkOn ? C2D_Color32(30, 8, 8, 255)
                            : C2D_Color32(12, 5, 5, 255);
        } else if (inRealGameplay && curE < 60) {
            bgCol = blinkOn ? C2D_Color32(30, 22, 8, 255)
                            : C2D_Color32(12, 10, 5, 255);
        } else {
            bgCol = C2D_Color32(6, 8, 14, 255);       /* original dark blue */
        }
        C2D_DrawRectSolid(0.0f, 0.0f, 0.1f, 320.0f, 240.0f, bgCol);
    }

    /* Always draw the top tab navigation bar */
    RenderTabBar();

    /* Render active view */
    switch (sCurrentTab) {
        case BOTTOM_TAB_MAP:
            RenderMapView();
            break;
        case BOTTOM_TAB_STATUS:
            RenderStatusView();
            break;
        case BOTTOM_TAB_DEBUG:
            RenderDebugView();
            break;
        case BOTTOM_TAB_OPTIONS:
            RenderOptionsView();
            break;
        default:
            RenderMapView();
            break;
    }

    /* RA session pump runs every frame in Port_BottomUI_FrameTick; here we
     * only draw the toast (if one is active) onto this frame's target. */
    Port_RA_RenderToastOverlay();

    /* L+R+START scene recorder indicator (platform_gpu_3ds.c) -- drawn last,
     * on top of whichever tab is active, so it's never hidden by one. Blinks
     * so it's noticeable even glanced at briefly mid-gameplay. Uses the same
     * border+fill panel style as the rest of the bottom UI so it doesn't
     * disappear against busy background art. */
    extern bool PlatformGpu3DS_IsRecording(void);
    extern bool PlatformGpu3DS_IsPerfRecording(void);
    const bool blink = (sFrameCounter & 0x20) != 0;
    /* 10x10 colored square + 5x7 bitmap text (7px tall). Text vertically
     * centered in the square: offset = (10-7)/2 = 1px. Panel wraps the
     * content with 3px border + 2px fill padding on all sides. */
    if (blink && (PlatformGpu3DS_IsRecording() || PlatformGpu3DS_IsPerfRecording())) {
        C2D_DrawRectSolid(5.0f, 5.0f, 0.90f, 46.0f, 28.0f, C2D_Color32(40, 70, 120, 255));
        C2D_DrawRectSolid(6.0f, 6.0f, 0.91f, 44.0f, 26.0f, C2D_Color32(14, 20, 32, 240));
    }
    if (PlatformGpu3DS_IsRecording() && blink) {
        C2D_DrawRectSolid(8.0f, 8.0f, 0.95f, 10.0f, 10.0f, C2D_Color32(230, 30, 30, 255));
        DrawText(24.0f, 9.0f, 0.95f, "REC", C2D_Color32(230, 30, 30, 255));
    }
    if (PlatformGpu3DS_IsPerfRecording() && blink) {
        C2D_DrawRectSolid(8.0f, 20.0f, 0.95f, 10.0f, 10.0f, C2D_Color32(30, 120, 230, 255));
        DrawText(24.0f, 21.0f, 0.95f, "PERF", C2D_Color32(30, 120, 230, 255));
    }
}
