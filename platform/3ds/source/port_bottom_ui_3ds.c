#include "port_bottom_ui_3ds.h"

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

#include "platform_gpu_3ds.h"

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

struct BottomEquipmentView {
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
extern struct BottomEquipmentView gEquipment;

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

/* Modals & Overlays */
static bool sShowRemapModal = false;
static bool sShowCollectiblesModal = false;
static bool sShowAchievementsModal = false;

/* Config & Display Helper Externs */
extern bool Port_Config_GetAutoHideHud(void);
extern void Port_Config_SetAutoHideHud(bool on);
extern int Port_Config_GetButtonMapping(int buttonIndex);
extern void Port_Config_CycleButtonMapping(int buttonIndex);
extern const char* Port_Config_GetActionName(int action, int lang);

/* RetroAchievements Helpers */
#include "port_retroachievements_3ds.h"

/* Area selector & zoom state */
static uint8_t sViewArea = 0;
static bool sFollowSamus = true;
static int sZoomLevel = 1; /* 0 = 1x (Overview), 1 = 2x (Detail), 2 = 3x (Ultra) */
static float sScrollX = 0.0f;
static float sScrollY = 0.0f;
static float sOptionsScrollY = 0.0f;
static float sAchievementsScrollY = 0.0f;
static int sLastTouchX = -1;
static int sLastTouchY = -1;
static int sTouchStartX = -1;
static int sTouchStartY = -1;
static bool sIsDragging = false;
static bool sIsTouchDragging = false;

int Port_BottomUI_GetZoom(void) { return sZoomLevel; }
void Port_BottomUI_SetZoom(int zoom) { if (zoom >= 0 && zoom <= 2) sZoomLevel = zoom; }

int Port_BottomUI_GetViewArea(void) { return (int)sViewArea; }
void Port_BottomUI_SetViewArea(int area) { if (area >= 0 && area <= 6) sViewArea = (uint8_t)area; }

bool Port_BottomUI_GetFollowSamus(void) { return sFollowSamus; }
void Port_BottomUI_SetFollowSamus(bool follow) { sFollowSamus = follow; }

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
    if (c == '*' || c == 'v' || c == '#') return checkmark;
    if (c == '+') return plus;
    if (c == '=') return equal;
    if (c == '!') return exclam;
    if (c == '?') return question;
    if (c == '>') return gt;
    if (c == '<') return lt;
    return NULL;
}

static void DrawTextClipped(float x, float y, float scale, const char* text, uint32_t color, float clipY0, float clipY1) {
    if (y + 7.0f * scale <= clipY0 || y >= clipY1) return;
    float charW = 6.0f * scale;
    for (; *text; ++text, x += charW) {
        const uint8_t* glyph = GetGlyph(*text);
        if (!glyph) continue;
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
    }
}

static void DrawTextMaxWClipped(float x, float y, float scale, const char* text, uint32_t color, float clipY0, float clipY1, float maxW) {
    if (y + 7.0f * scale <= clipY0 || y >= clipY1) return;
    float charW = 6.0f * scale;
    float startX = x;
    for (; *text; ++text, x += charW) {
        if (x + charW > startX + maxW) break;
        const uint8_t* glyph = GetGlyph(*text);
        if (!glyph) continue;
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
    }
}

static int MeasureWrappedTextLines(const char* text, float maxWidth, float scale) {
    if (!text || !*text) return 1;
    float charW = 6.0f * scale;
    int maxCharsPerLine = (int)(maxWidth / charW);
    if (maxCharsPerLine < 10) maxCharsPerLine = 10;

    int lines = 1;
    int curLineLen = 0;
    const char* ptr = text;

    while (*ptr) {
        /* find next word */
        const char* wordStart = ptr;
        while (*ptr && *ptr != ' ') ++ptr;
        int wordLen = (int)(ptr - wordStart);

        if (curLineLen == 0) {
            curLineLen = wordLen;
        } else if (curLineLen + 1 + wordLen <= maxCharsPerLine) {
            curLineLen += 1 + wordLen;
        } else {
            ++lines;
            curLineLen = wordLen;
        }
        while (*ptr == ' ') ++ptr;
    }
    return lines;
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

static void DrawTextCentered(float cx, float y, float scale, const char* text, uint32_t color) {
    float length = (float)strlen(text) * 6.0f * scale;
    DrawText(cx - (length / 2.0f), y, scale, text, color);
}

static void DrawTextCenteredClipped(float cx, float y, float scale, const char* text, uint32_t color, float clipY0, float clipY1) {
    float length = (float)strlen(text) * 6.0f * scale;
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

void Port_BottomUI_HandleTouchDrag(int x, int y, bool isNewTap) {
    /* 4-Tab Top Navigation Bar (Y: 2 to 24) */
    if (y >= 2 && y <= 24) {
        if (isNewTap) {
            PortBottomTab prevTab = sCurrentTab;
            if (x >= 4 && x <= 78) sCurrentTab = BOTTOM_TAB_MAP;
            else if (x >= 82 && x <= 156) sCurrentTab = BOTTOM_TAB_STATUS;
            else if (x >= 160 && x <= 234) sCurrentTab = BOTTOM_TAB_DEBUG;
            else if (x >= 238 && x <= 316) sCurrentTab = BOTTOM_TAB_OPTIONS;
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
    if (sCurrentTab == BOTTOM_TAB_STATUS && isNewTap) {
        if (sShowCollectiblesModal) {
            sShowCollectiblesModal = false;
            return;
        }
        if (x >= 180 && x <= 310 && y >= 166 && y <= 186) {
            sShowCollectiblesModal = true;
            return;
        }
    }

    /* Modal: Achievements View Scrolling & Close */
    if (sCurrentTab == BOTTOM_TAB_OPTIONS && sShowAchievementsModal) {
        uint32_t achCount = Port_RA_GetAchievementCount();
        float totalContentH = 0.0f;
        for (uint32_t i = 0; i < achCount; ++i) {
            const RetroAchievementItem* ach = Port_RA_GetAchievement(i);
            if (!ach) continue;
            int lines = MeasureWrappedTextLines(ach->description, 230.0f, 1.0f);
            float cardH = 24.0f + (float)lines * 9.0f + 6.0f;
            if (cardH < 38.0f) cardH = 38.0f;
            totalContentH += cardH + 6.0f;
        }
        float maxAchScroll = (totalContentH > 154.0f) ? (totalContentH - 154.0f) : 0.0f;

        if (isNewTap) {
            if (x >= 100 && x <= 220 && y >= 204 && y <= 230) {
                sShowAchievementsModal = false;
                sLastTouchX = -1;
                sLastTouchY = -1;
                return;
            }
            /* Direct scrollbar touch on right (x >= 300) */
            if (x >= 300 && maxAchScroll > 0.0f) {
                float trackY = (float)y - 48.0f;
                if (trackY < 0.0f) trackY = 0.0f;
                if (trackY > 124.0f) trackY = 124.0f;
                sAchievementsScrollY = (trackY / 124.0f) * maxAchScroll;
                sIsTouchDragging = true;
            } else {
                sTouchStartX = x;
                sTouchStartY = y;
                sLastTouchX = x;
                sLastTouchY = y;
                sIsTouchDragging = false;
            }
        } else if (x >= 300 && maxAchScroll > 0.0f) {
            /* Dragging scrollbar directly */
            float trackY = (float)y - 48.0f;
            if (trackY < 0.0f) trackY = 0.0f;
            if (trackY > 124.0f) trackY = 124.0f;
            sAchievementsScrollY = (trackY / 124.0f) * maxAchScroll;
            sIsTouchDragging = true;
        } else if (sLastTouchX >= 0 && sLastTouchY >= 0) {
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

    /* Options Tab Interactive Rows & Touch Scrolling */
    if (sCurrentTab == BOTTOM_TAB_OPTIONS) {
        if (sShowRemapModal) {
            if (isNewTap && x >= 16 && x <= 304) {
                if (y >= 50 && y <= 76) Port_Config_CycleButtonMapping(0); /* X */
                else if (y >= 82 && y <= 108) Port_Config_CycleButtonMapping(1); /* Y */
                else if (y >= 114 && y <= 140) Port_Config_CycleButtonMapping(2); /* ZL */
                else if (y >= 146 && y <= 172) Port_Config_CycleButtonMapping(3); /* ZR */
                else if (y >= 200 && y <= 230) sShowRemapModal = false; /* Close */
            }
            return;
        }

        /* Options scrolling drag handling */
        const float maxScroll = 166.0f;
        if (y >= 28 && y <= 236 && x >= 4 && x <= 316) {
            if (isNewTap) {
                /* Direct scrollbar touch on right (x >= 305) */
                if (x >= 305) {
                    float trackY = (float)(y - 28) - 17.5f;
                    if (trackY < 0.0f) trackY = 0.0f;
                    if (trackY > 169.0f) trackY = 169.0f;
                    sOptionsScrollY = (trackY / 169.0f) * maxScroll;
                    sIsTouchDragging = true;
                } else {
                    sTouchStartX = x;
                    sTouchStartY = y;
                    sLastTouchX = x;
                    sLastTouchY = y;
                    sIsTouchDragging = false;
                }
            } else if (x >= 305) {
                /* Dragging scrollbar directly */
                float trackY = (float)(y - 28) - 17.5f;
                if (trackY < 0.0f) trackY = 0.0f;
                if (trackY > 169.0f) trackY = 169.0f;
                sOptionsScrollY = (trackY / 169.0f) * maxScroll;
                sIsTouchDragging = true;
            } else if (sLastTouchX >= 0 && sLastTouchY >= 0) {
                float dy = (float)(sLastTouchY - y);
                if (dy != 0.0f) {
                    sOptionsScrollY += dy;
                    if (sOptionsScrollY < 0.0f) sOptionsScrollY = 0.0f;
                    if (sOptionsScrollY > maxScroll) sOptionsScrollY = maxScroll;
                    if (dy > 2.0f || dy < -2.0f) {
                        sIsTouchDragging = true;
                    }
                }
                sLastTouchX = x;
                sLastTouchY = y;
            }
        }
    }
}

void Port_BottomUI_TouchReleased(void) {
    if (sCurrentTab == BOTTOM_TAB_OPTIONS && !sShowRemapModal && !sShowAchievementsModal && !sIsTouchDragging && sTouchStartX >= 0 && sTouchStartY >= 0) {
        int x = sTouchStartX;
        int y = sTouchStartY + (int)sOptionsScrollY;

        if (x >= 10 && x <= 308) {
            if (y >= 48 && y <= 72) {
                Port_Config_Cycle3DSAspectRatio();
            } else if (y >= 76 && y <= 100) {
                Port_Config_Cycle3DSDisplayStyle();
            } else if (y >= 104 && y <= 128) {
                Port_Config_SetShowFps(!Port_Config_GetShowFps());
            } else if (y >= 132 && y <= 156) {
                Port_Config_SetAutoHideHud(!Port_Config_GetAutoHideHud());
            } else if (y >= 160 && y <= 186) {
                sShowRemapModal = true;
            } else if (y >= 198 && y <= 216) {
                /* Touch RetroAchievements header -> trigger swkbd login */
                Port_RA_PromptLogin();
            } else if (y >= 218 && y <= 242) {
                /* User Login Row (FIRST) */
                Port_RA_PromptLogin();
            } else if (y >= 246 && y <= 270) {
                /* Enable / Disable */
                Port_RA_SetEnabled(!Port_RA_IsEnabled());
                Port_Config_Save();
            } else if (y >= 274 && y <= 298) {
                /* Hardcore Mode */
                Port_RA_SetHardcore(!Port_RA_IsHardcore());
                Port_Config_Save();
            } else if (y >= 302 && y <= 326) {
                /* Notification Sound */
                Port_RA_SetNotificationSound(!Port_RA_GetNotificationSound());
                Port_Config_Save();
            } else if (y >= 330 && y <= 358) {
                /* View achievements modal */
                sAchievementsScrollY = 0.0f;
                sShowAchievementsModal = true;
            }
        }
    }

    sLastTouchX = -1;
    sLastTouchY = -1;
    sTouchStartX = -1;
    sTouchStartY = -1;
    sIsDragging = false;
    sIsTouchDragging = false;
}

/* Render 4-Tab Navigation Bar */
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

    struct {
        float x;
        float w;
        PortBottomTab tab;
    } tabs[] = {
        { 4.0f,   74.0f, BOTTOM_TAB_MAP },
        { 82.0f,  74.0f, BOTTOM_TAB_STATUS },
        { 160.0f, 74.0f, BOTTOM_TAB_DEBUG },
        { 238.0f, 78.0f, BOTTOM_TAB_OPTIONS }
    };

    for (int i = 0; i < 4; ++i) {
        bool active = (sCurrentTab == tabs[i].tab);
        uint32_t bg = active ? C2D_Color32(18, 70, 130, 255) : C2D_Color32(26, 30, 42, 255);
        uint32_t border = active ? C2D_Color32(45, 150, 240, 255) : C2D_Color32(50, 56, 75, 255);
        uint32_t textColor = active ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(140, 150, 175, 255);

        C2D_DrawRectSolid(tabs[i].x, 3.0f, 0.4f, tabs[i].w, 20.0f, border);
        C2D_DrawRectSolid(tabs[i].x + 1.0f, 4.0f, 0.5f, tabs[i].w - 2.0f, 18.0f, bg);
        DrawTextCentered(tabs[i].x + tabs[i].w / 2.0f, 9.0f, 1.0f, tabNames[lang][i], textColor);
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

static const char* GetAspectRatioDisplayName(int lang) {
    int ar = Port_Config_Get3DSAspectRatio();
    if (lang == 6) {
        switch (ar) {
            case 0: return "PANORAMICO";
            case 1: return "ORIGINAL (3:2)";
            case 2: return "ESTIRADO (16:9)";
            default: return "ORIGINAL";
        }
    }
    switch (ar) {
        case 0: return "WIDE";
        case 1: return "ORIGINAL (3:2)";
        case 2: return "STRETCH";
        default: return "ORIGINAL";
    }
}

static const char* GetDisplayStyleDisplayName(int lang) {
    int ds = Port_Config_Get3DSDisplayStyle();
    if (lang == 6) {
        switch (ds) {
            case 0: return "PIXEL PERFECT (1:1)";
            case 1: return "ESCALADO NITIDO";
            case 2: return "SUAVIZADO";
            default: return "ESCALADO";
        }
    }
    switch (ds) {
        case 0: return "PIXEL PERFECT (1:1)";
        case 1: return "SCALED (SHARP)";
        case 2: return "BLUR (SMOOTH)";
        default: return "SCALED";
    }
}

static const char* GetFpsOverlayDisplayName(int lang) {
    bool on = Port_Config_GetShowFps();
    if (lang == 6) {
        return on ? "ACTIVADO" : "DESACTIVADO";
    }
    return on ? "ON" : "OFF";
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

    /* Count bits from gMinimapTilesWithObtainedItems for this area */
    for (int row = 0; row < 32; ++row) {
        uint32_t bits = gMinimapTilesWithObtainedItems[area * 32 + row];
        while (bits) {
            if (bits & 1) outStats->totalObtained++;
            bits >>= 1;
        }
    }
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

    struct AreaItemStats globalStats;
    GetGlobalItemStats(&globalStats);
    char globTitle[64];
    unsigned pct = globalStats.totalItems > 0 ? (globalStats.totalObtained * 100 / globalStats.totalItems) : 0;
    snprintf(globTitle, sizeof(globTitle), (lang == 6) ? "COLECCIONABLES POR ZONA: %u/%u (%u%%)" : "COLLECTIBLES BY AREA: %u/%u (%u%%)",
             globalStats.totalObtained, globalStats.totalItems, pct);
    DrawText(18.0f, 32.0f, 1.0f, globTitle, C2D_Color32(255, 215, 0, 255));

    /* Table Headers */
    DrawText(18.0f, 48.0f, 1.0f, (lang == 6) ? "ZONA" : "AREA", C2D_Color32(100, 220, 255, 255));
    DrawEnergyIcon(108.0f, 46.0f);
    DrawMissileIcon(148.0f, 46.0f);
    DrawSuperMissileIcon(193.0f, 46.0f);
    DrawPowerBombIcon(233.0f, 46.0f);
    DrawText(272.0f, 48.0f, 1.0f, "TOT", C2D_Color32(255, 255, 255, 255));

    for (int a = 0; a < 7; ++a) {
        struct AreaItemStats aStats;
        GetAreaStats(a, &aStats);
        float py = 64.0f + (float)a * 19.0f;

        C2D_DrawRectSolid(16.0f, py, 0.88f, 288.0f, 17.0f, (a % 2 == 0) ? C2D_Color32(20, 26, 40, 255) : C2D_Color32(14, 18, 30, 255));

        DrawText(20.0f, py + 3.0f, 1.0f, AreaName(a), C2D_Color32(220, 235, 255, 255));

        char cBuf[16];
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

    C2D_DrawRectSolid(100.0f, 204.0f, 0.9f, 120.0f, 20.0f, C2D_Color32(20, 70, 130, 255));
    DrawTextCentered(160.0f, 209.0f, 1.0f, (lang == 6) ? "CERRAR" : "CLOSE", C2D_Color32(255, 255, 255, 255));
}

/* Render RetroAchievements List Modal with Touch Scrolling */
static void RenderAchievementsModal(int lang) {
    const float mX = 10.0f;
    const float mY = 26.0f;
    const float mW = 300.0f;
    const float mH = 206.0f;
    const float clipY0 = mY + 22.0f; /* 48.0f */
    const float clipY1 = mY + mH - 30.0f; /* 202.0f */

    C2D_DrawRectSolid(mX, mY, 0.85f, mW, mH, C2D_Color32(10, 14, 24, 250));
    C2D_DrawRectSolid(mX, mY, 0.84f, mW, mH, C2D_Color32(40, 70, 120, 255));

    const char* titles[7] = {
        "RETROACHIEVEMENTS LIST", "RETROACHIEVEMENTS LIST", "RETROACHIEVEMENTS LIST",
        "ERFOLGSLISTE", "LISTE DES SUCCES", "LISTA DEGLI OBIETTIVI", "LISTA DE LOGROS"
    };
    DrawText(20.0f, 32.0f, 1.0f, titles[lang], C2D_Color32(255, 215, 0, 255));

    uint32_t count = Port_RA_GetAchievementCount();
    uint32_t unlocked = Port_RA_GetUnlockedCount();
    uint32_t hardcore = Port_RA_GetHardcoreUnlockedCount();
    uint32_t totalPts = Port_RA_GetTotalPoints();
    uint32_t unlPts = Port_RA_GetUnlockedPoints();

    char summaryBuf[64];
    if (hardcore > 0) {
        snprintf(summaryBuf, sizeof(summaryBuf), "%u/%u (%u HC) %uP", (unsigned)unlocked, (unsigned)count, (unsigned)hardcore, (unsigned)unlPts);
    } else {
        snprintf(summaryBuf, sizeof(summaryBuf), "%u/%u (%u/%u PTS)", (unsigned)unlocked, (unsigned)count, (unsigned)unlPts, (unsigned)totalPts);
    }
    DrawText(160.0f, 32.0f, 1.0f, summaryBuf, hardcore > 0 ? C2D_Color32(255, 215, 0, 255) : C2D_Color32(80, 255, 120, 255));

    /* Calculate total content height dynamically based on wrapped description lines */
    float totalContentH = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const RetroAchievementItem* ach = Port_RA_GetAchievement(i);
        if (!ach) continue;
        int lines = MeasureWrappedTextLines(ach->description, 230.0f, 1.0f);
        float cardH = 24.0f + (float)lines * 9.0f + 6.0f;
        if (cardH < 38.0f) cardH = 38.0f;
        totalContentH += cardH + 6.0f;
    }

    float viewH = clipY1 - clipY0;
    float maxAchScroll = (totalContentH > viewH) ? (totalContentH - viewH) : 0.0f;
    if (sAchievementsScrollY > maxAchScroll) sAchievementsScrollY = maxAchScroll;

    /* Clip and draw dynamic achievement cards */
    float curCardY = clipY0 - sAchievementsScrollY;
    for (uint32_t i = 0; i < count; ++i) {
        const RetroAchievementItem* ach = Port_RA_GetAchievement(i);
        if (!ach) continue;

        int descLines = MeasureWrappedTextLines(ach->description, 230.0f, 1.0f);
        float cardH = 24.0f + (float)descLines * 9.0f + 6.0f;
        if (cardH < 38.0f) cardH = 38.0f;

        float py = curCardY;
        curCardY += cardH + 6.0f;

        if (py + cardH < clipY0 || py > clipY1) continue;

        float drawY0 = (py < clipY0) ? clipY0 : py;
        float drawY1 = (py + cardH > clipY1) ? clipY1 : (py + cardH);
        float drawH  = drawY1 - drawY0;
        if (drawH <= 0.0f) continue;

        /* Visual hierarchy: Hardcore (Gold), Softcore (Emerald Green), Locked (Slate/Navy) */
        uint32_t boxBg = ach->hardcoreUnlocked ? C2D_Color32(36, 30, 10, 255) :
                         (ach->unlocked ? C2D_Color32(14, 38, 26, 255) : C2D_Color32(20, 26, 40, 255));
        uint32_t boxBorder = ach->hardcoreUnlocked ? C2D_Color32(255, 200, 40, 255) :
                             (ach->unlocked ? C2D_Color32(40, 180, 90, 255) : C2D_Color32(45, 60, 90, 255));

        C2D_DrawRectSolid(14.0f, drawY0, 0.9f, 292.0f, drawH, boxBg);
        if (py >= clipY0 && py <= clipY1) {
            C2D_DrawRectSolid(14.0f, py, 0.88f, 292.0f, 1.0f, boxBorder);
        }
        if (ach->hardcoreUnlocked) {
            float btmY = py + cardH - 1.0f;
            if (btmY >= clipY0 && btmY <= clipY1) {
                /* Extra bottom glow border for hardcore, properly clipped */
                C2D_DrawRectSolid(14.0f, btmY, 0.88f, 292.0f, 1.0f, C2D_Color32(200, 150, 20, 255));
            }
        }

        /* 1. Badge Icon / Artwork Frame on Left */
        float iconBoxY = py + 4.0f;
        float iconBoxH = cardH - 8.0f;
        float drawIconY0 = (iconBoxY < clipY0) ? clipY0 : iconBoxY;
        float drawIconY1 = (iconBoxY + iconBoxH > clipY1) ? clipY1 : (iconBoxY + iconBoxH);
        if (drawIconY1 > drawIconY0) {
            uint32_t iconBg = ach->hardcoreUnlocked ? C2D_Color32(65, 50, 15, 255) :
                              (ach->unlocked ? C2D_Color32(25, 65, 38, 255) : C2D_Color32(20, 24, 35, 255));
            uint32_t iconBord = ach->hardcoreUnlocked ? C2D_Color32(255, 215, 0, 255) :
                                (ach->unlocked ? C2D_Color32(80, 220, 120, 255) : C2D_Color32(50, 65, 90, 255));

            C2D_DrawRectSolid(18.0f, drawIconY0, 0.91f, 24.0f, drawIconY1 - drawIconY0, iconBg);
            if (iconBoxY >= clipY0) {
                C2D_DrawRectSolid(18.0f, iconBoxY, 0.92f, 24.0f, 1.0f, iconBord);
            }

            /* Draw real 20x20 RetroAchievements Badge Artwork */
            const uint32_t* badgePixels = Port_RA_GetBadgePixels(ach->badgeName);
            float badgeX = 20.0f;
            float badgeY = py + (cardH - 20.0f) / 2.0f;

            if (badgePixels) {
                for (int by = 0; by < 20; ++by) {
                    float rY = badgeY + (float)by;
                    if (rY < clipY0 || rY >= clipY1) continue;
                    for (int bx = 0; bx < 20; ++bx) {
                        uint32_t pColor = badgePixels[by * 20 + bx];
                        /* If locked, display in dim grayscale */
                        if (!ach->unlocked) {
                            uint32_t r = pColor & 0xFF;
                            uint32_t g = (pColor >> 8) & 0xFF;
                            uint32_t b = (pColor >> 16) & 0xFF;
                            uint32_t gray = (r * 30 + g * 59 + b * 11) / 250; /* slightly darker grayscale */
                            pColor = C2D_Color32((uint8_t)gray, (uint8_t)gray, (uint8_t)gray, 255);
                        }
                        C2D_DrawRectSolid(badgeX + (float)bx, rY, 0.93f, 1.0f, 1.0f, pColor);
                    }
                }
            } else {
                /* Fallback if badge pixel data unavailable */
                float iconCenterY = py + (cardH / 2.0f);
                if (iconCenterY >= clipY0 + 6.0f && iconCenterY <= clipY1 - 6.0f) {
                    if (ach->hardcoreUnlocked) {
                        C2D_DrawRectSolid(24.0f, iconCenterY - 4.0f, 0.93f, 12.0f, 8.0f, C2D_Color32(255, 215, 0, 255));
                    } else if (ach->unlocked) {
                        C2D_DrawRectSolid(25.0f, iconCenterY - 2.0f, 0.93f, 10.0f, 4.0f, C2D_Color32(80, 255, 140, 255));
                    } else {
                        C2D_DrawRectSolid(25.0f, iconCenterY - 2.0f, 0.93f, 10.0f, 7.0f, C2D_Color32(70, 90, 120, 255));
                    }
                }
            }
        }

        /* 2. Graphical Status Mini-Icon on Top-Right */
        float statX = 286.0f;
        float statY = py + 5.0f;
        if (statY >= clipY0 && statY + 8.0f <= clipY1) {
            if (ach->hardcoreUnlocked) {
                /* Hardcore Mini Crown (Gold) */
                uint32_t cGold = C2D_Color32(255, 215, 0, 255);
                C2D_DrawRectSolid(statX, statY + 3.0f, 0.93f, 8.0f, 4.0f, cGold);
                C2D_DrawRectSolid(statX + 1.0f, statY + 1.0f, 0.93f, 2.0f, 2.0f, cGold);
                C2D_DrawRectSolid(statX + 3.0f, statY, 0.93f, 2.0f, 2.0f, cGold);
                C2D_DrawRectSolid(statX + 5.0f, statY + 1.0f, 0.93f, 2.0f, 2.0f, cGold);
            } else if (ach->unlocked) {
                /* Softcore Unlocked Mini Padlock Open / Checkmark (Green) */
                uint32_t cGreen = C2D_Color32(80, 255, 120, 255);
                /* Open shackle */
                C2D_DrawRectSolid(statX + 3.0f, statY, 0.93f, 4.0f, 1.0f, cGreen);
                C2D_DrawRectSolid(statX + 6.0f, statY + 1.0f, 0.93f, 1.0f, 2.0f, cGreen);
                /* Body */
                C2D_DrawRectSolid(statX + 1.0f, statY + 3.0f, 0.93f, 6.0f, 4.0f, cGreen);
            } else {
                /* Locked Mini Padlock Closed (Slate Gray) */
                uint32_t cLock = C2D_Color32(110, 130, 160, 255);
                /* Closed shackle */
                C2D_DrawRectSolid(statX + 2.0f, statY, 0.93f, 4.0f, 1.0f, cLock);
                C2D_DrawRectSolid(statX + 2.0f, statY + 1.0f, 0.93f, 1.0f, 2.0f, cLock);
                C2D_DrawRectSolid(statX + 5.0f, statY + 1.0f, 0.93f, 1.0f, 2.0f, cLock);
                /* Body */
                C2D_DrawRectSolid(statX + 1.0f, statY + 3.0f, 0.93f, 6.0f, 4.0f, cLock);
            }
        }

        /* 3. Graphical Type Mini-Icon on Bottom-Right (Missable / Progression / Win) */
        float tagX = 286.0f;
        float tagY = py + cardH - 12.0f;
        if (tagY >= clipY0 && tagY + 8.0f <= clipY1) {
            if (ach->type == RA_ACH_TYPE_MISSABLE) {
                /* Warning Triangle / Hourglass (Orange/Coral) */
                uint32_t cMiss = C2D_Color32(255, 120, 60, 255);
                C2D_DrawRectSolid(tagX + 3.0f, tagY, 0.93f, 2.0f, 2.0f, cMiss);
                C2D_DrawRectSolid(tagX + 2.0f, tagY + 2.0f, 0.93f, 4.0f, 2.0f, cMiss);
                C2D_DrawRectSolid(tagX + 1.0f, tagY + 4.0f, 0.93f, 6.0f, 2.0f, cMiss);
                C2D_DrawRectSolid(tagX, tagY + 6.0f, 0.93f, 8.0f, 2.0f, cMiss);
                C2D_DrawRectSolid(tagX + 3.0f, tagY + 3.0f, 0.94f, 2.0f, 3.0f, C2D_Color32(20, 26, 40, 255));
            } else if (ach->type == RA_ACH_TYPE_PROGRESSION) {
                /* Story Progression Flag / Bookmark (Cyan / Sky Blue) */
                uint32_t cProg = C2D_Color32(80, 200, 255, 255);
                C2D_DrawRectSolid(tagX + 1.0f, tagY, 0.93f, 2.0f, 8.0f, cProg);
                C2D_DrawRectSolid(tagX + 3.0f, tagY, 0.93f, 5.0f, 4.0f, cProg);
                C2D_DrawRectSolid(tagX + 7.0f, tagY + 1.0f, 0.94f, 1.0f, 2.0f, C2D_Color32(20, 26, 40, 255));
            } else if (ach->type == RA_ACH_TYPE_WIN_CONDITION) {
                /* Trophy / Laurel (Gold) */
                uint32_t cWin = C2D_Color32(255, 215, 0, 255);
                C2D_DrawRectSolid(tagX + 1.0f, tagY, 0.93f, 6.0f, 5.0f, cWin);
                C2D_DrawRectSolid(tagX + 3.0f, tagY + 5.0f, 0.93f, 2.0f, 2.0f, cWin);
                C2D_DrawRectSolid(tagX + 2.0f, tagY + 7.0f, 0.93f, 4.0f, 1.0f, cWin);
            }
        }

        /* 4. Title + Points (Full width up to statX - 6.0f = 232px) */
        char titleBuf[80];
        snprintf(titleBuf, sizeof(titleBuf), "%s (%uP)", ach->title, (unsigned)ach->points);
        uint32_t titleCol = ach->hardcoreUnlocked ? C2D_Color32(255, 225, 80, 255) :
                            (ach->unlocked ? C2D_Color32(140, 240, 170, 255) : C2D_Color32(220, 235, 255, 255));
        DrawTextMaxWClipped(48.0f, py + 4.0f, 1.0f, titleBuf, titleCol, clipY0, clipY1, 232.0f);

        /* 5. Multi-line Word-Wrapped Description */
        DrawWrappedTextClipped(48.0f, py + 16.0f, 1.0f, ach->description, 232.0f, 9.0f, C2D_Color32(145, 165, 195, 255), clipY0, clipY1);
    }

    /* Achievements modal scrollbar */
    if (maxAchScroll > 0.0f) {
        float trackH = (clipY1 - clipY0);
        float thumbH = 30.0f;
        float thumbY = clipY0 + (sAchievementsScrollY / maxAchScroll) * (trackH - thumbH);
        C2D_DrawRectSolid(306.0f, clipY0, 0.92f, 2.0f, trackH, C2D_Color32(25, 35, 55, 255));
        C2D_DrawRectSolid(306.0f, thumbY, 0.94f, 2.0f, thumbH, C2D_Color32(80, 160, 240, 255));
    }

    C2D_DrawRectSolid(100.0f, 204.0f, 0.96f, 120.0f, 20.0f, C2D_Color32(20, 70, 130, 255));
    DrawTextCentered(160.0f, 209.0f, 1.0f, (lang == 6) ? "VOLVER" : "BACK", C2D_Color32(255, 255, 255, 255));
}

/* Render Button Remap Modal */
static void RenderRemapModal(int lang) {
    C2D_DrawRectSolid(10.0f, 26.0f, 0.85f, 300.0f, 206.0f, C2D_Color32(10, 14, 24, 250));
    C2D_DrawRectSolid(10.0f, 26.0f, 0.84f, 300.0f, 206.0f, C2D_Color32(40, 70, 120, 255));

    DrawText(20.0f, 32.0f, 1.0f, (lang == 6) ? "REASIGNAR BOTONES (TOCA PARA CAMBIAR)" : "BUTTON REMAPPING (TOUCH TO CYCLE)", C2D_Color32(255, 215, 0, 255));

    const char* btnNames[4] = { "BOTON X:", "BOTON Y:", "BOTON ZL:", "BOTON ZR:" };
    const char* btnNamesEN[4] = { "BUTTON X:", "BUTTON Y:", "BUTTON ZL:", "BUTTON ZR:" };

    for (int i = 0; i < 4; ++i) {
        float py = 50.0f + (float)i * 32.0f;
        C2D_DrawRectSolid(16.0f, py, 0.9f, 288.0f, 26.0f, C2D_Color32(24, 32, 50, 255));
        C2D_DrawRectSolid(16.0f, py, 0.88f, 288.0f, 1.0f, C2D_Color32(60, 85, 130, 255));

        DrawText(24.0f, py + 8.0f, 1.0f, (lang == 6) ? btnNames[i] : btnNamesEN[i], C2D_Color32(255, 255, 255, 255));
        int act = Port_Config_GetButtonMapping(i);
        DrawText(130.0f, py + 8.0f, 1.0f, Port_Config_GetActionName(act, lang), C2D_Color32(255, 215, 0, 255));
    }

    DrawText(16.0f, 180.0f, 1.0f, (lang == 6) ? "STICK IZQ/DCHO: MOVIMIENTO Y APUNTAR" : "ANALOG STICKS: MOVEMENT & AIMING", C2D_Color32(120, 220, 180, 255));

    C2D_DrawRectSolid(100.0f, 204.0f, 0.9f, 120.0f, 20.0f, C2D_Color32(20, 70, 130, 255));
    DrawTextCentered(160.0f, 209.0f, 1.0f, (lang == 6) ? "GUARDAR Y VOLVER" : "SAVE & RETURN", C2D_Color32(255, 255, 255, 255));
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

    /* Area Items Badge (Shows e.g. "5/12") */
    char itemBadge[24];
    snprintf(itemBadge, sizeof(itemBadge), "%u/%u", areaStats.totalObtained, areaStats.totalItems);
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

}

/* Render Status (Estado) View */
static void RenderStatusView(void) {
    int lang = GetLang();

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

    /* 1. Header Resource Card (X: 8, Y: 44 to 72, W: 304, H: 28) */
    C2D_DrawRectSolid(8.0f, 44.0f, 0.45f, 304.0f, 30.0f, C2D_Color32(20, 26, 40, 255));

    /* Health */
    DrawEnergyIcon(14.0f, 55.0f);
    char eBuf[32];
    snprintf(eBuf, sizeof(eBuf), "%02u/%02u", gEquipment.currentEnergy, gEquipment.maxEnergy);
    DrawText(31.0f, 56.0f, 1.0f, eBuf, C2D_Color32(255, 215, 0, 255));

    /* Missiles */
    DrawMissileIcon(90.0f, 55.0f);
    char mBuf[32];
    if (gEquipment.maxMissiles > 0) {
        snprintf(mBuf, sizeof(mBuf), "%03u/%03u", gEquipment.currentMissiles, gEquipment.maxMissiles);
        DrawText(107.0f, 56.0f, 1.0f, mBuf, C2D_Color32(255, 140, 140, 255));
    } else {
        DrawText(107.0f, 56.0f, 1.0f, "---/---", C2D_Color32(100, 110, 130, 255));
    }

    /* Super Missiles */
    DrawSuperMissileIcon(166.0f, 55.0f);
    char smBuf[32];
    if (gEquipment.maxSuperMissiles > 0) {
        snprintf(smBuf, sizeof(smBuf), "%02u/%02u", gEquipment.currentSuperMissiles, gEquipment.maxSuperMissiles);
        DrawText(183.0f, 56.0f, 1.0f, smBuf, C2D_Color32(100, 255, 140, 255));
    } else {
        DrawText(183.0f, 56.0f, 1.0f, "--/--", C2D_Color32(100, 110, 130, 255));
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

    /* 2. Beams & Weapons Column (X: 8, W: 148, Y: 78 to 162) */
    C2D_DrawRectSolid(8.0f, 78.0f, 0.45f, 148.0f, 84.0f, C2D_Color32(18, 22, 34, 255));
    const char* beamColTitles[7] = {
        "BEAMS & BOMBS", "BEAMS & BOMBS", "BEAMS & BOMBS",
        "BEAMS & BOMBEN", "RAYONS & BOMBES", "RAGGI E BOMBE", "RAYOS Y BOMBAS"
    };
    DrawText(14.0f, 83.0f, 1.0f, beamColTitles[lang], C2D_Color32(255, 215, 0, 255));

    struct {
        const char* name[7];
        uint8_t flag;
    } beams[] = {
        { { "LONG BEAM", "LONG BEAM", "LONG BEAM", "LONG BEAM", "RAYON LONG", "RAGGIO LUNGO", "RAYO LARGO" }, 1 << 0 },
        { { "ICE BEAM", "ICE BEAM", "ICE BEAM", "EIS BEAM", "RAYON GLACE", "RAGGIO GELO", "RAYO HIELO" }, 1 << 1 },
        { { "WAVE BEAM", "WAVE BEAM", "WAVE BEAM", "WAVE BEAM", "RAYON ONDES", "RAGGIO ONDA", "RAYO ONDAS" }, 1 << 2 },
        { { "PLASMA BEAM", "PLASMA BEAM", "PLASMA BEAM", "PLASMA BEAM", "RAYON PLASMA", "RAGGIO PLASMA", "RAYO PLASMA" }, 1 << 3 },
        { { "CHARGE BEAM", "CHARGE BEAM", "CHARGE BEAM", "CHARGE BEAM", "RAYON CHARGE", "RAGGIO CARICA", "RAYO CARGA" }, 1 << 4 },
        { { "BOMBS", "BOMBS", "NORMAL BOMBS", "NORMAL BOMBEN", "BOMBES", "BOMBE NORMALI", "BOMBAS" }, 1 << 7 }
    };
    for (int i = 0; i < 6; ++i) {
        bool has = (gEquipment.beamBombs & beams[i].flag) != 0;
        uint32_t col = has ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(75, 85, 105, 255);
        float py = 95.0f + (float)i * 10.0f;
        DrawText(14.0f, py, 1.0f, beams[i].name[lang], col);
        DrawText(130.0f, py, 1.0f, has ? "OK" : "--", col);
    }

    /* 3. Suits & Movement Column (X: 164, W: 148, Y: 78 to 162) */
    C2D_DrawRectSolid(164.0f, 78.0f, 0.45f, 148.0f, 84.0f, C2D_Color32(18, 22, 34, 255));
    const char* suitColTitles[7] = {
        "SUITS & MISC", "SUITS & MISC", "SUITS & MISC",
        "ANZUEGE & ITEMS", "COMBINAISONS", "TUTE E OGGETTI", "TRAJES Y EQUIPO"
    };
    DrawText(170.0f, 83.0f, 1.0f, suitColTitles[lang], C2D_Color32(255, 215, 0, 255));

    struct {
        const char* name[7];
        uint8_t flag;
    } suits[] = {
        { { "HI-JUMP", "HI-JUMP", "HIGH JUMP", "HOCHSPRUNG", "SUPER SAUT", "SALTO IN ALTO", "SALTO ALTO" }, 1 << 0 },
        { { "SPEEDBOOSTER", "SPEEDBOOSTER", "SPEED BOOSTER", "SPEED BOOSTER", "ACCELERATION", "SUPERVELOCITA", "ACELERADOR" }, 1 << 1 },
        { { "SPACE JUMP", "SPACE JUMP", "SPACE JUMP", "SPACE JUMP", "SAUT SPATIAL", "SALTO SPAZIALE", "SALTO ESPACIO" }, 1 << 2 },
        { { "SCREW ATTACK", "SCREW ATTACK", "SCREW ATTACK", "SCREW ATTACK", "ATTAQUE VRILLE", "ATTACCO A VITE", "ATAQUE ESPIRAL" }, 1 << 3 },
        { { "VARIA SUIT", "VARIA SUIT", "VARIA SUIT", "VARIA SUIT", "COSTUME VARIA", "TUTA VARIA", "TRAJE VARIA" }, 1 << 4 },
        { { "GRAVITY SUIT", "GRAVITY SUIT", "GRAVITY SUIT", "GRAVITY SUIT", "COSTUME GRAVITE", "TUTA GRAVITA", "TRAJE GRAVEDAD" }, 1 << 5 },
        { { "MORPH BALL", "MORPH BALL", "MORPH BALL", "MORPH BALL", "MORPHING", "MORFOSFERA", "MORFOSFERA" }, 1 << 6 }
    };
    for (int i = 0; i < 7; ++i) {
        bool has = (gEquipment.suitMisc & suits[i].flag) != 0;
        uint32_t col = has ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(75, 85, 105, 255);
        float py = 95.0f + (float)i * 10.0f;
        DrawText(170.0f, py, 1.0f, suits[i].name[lang], col);
        DrawText(286.0f, py, 1.0f, has ? "OK" : "--", col);
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
    unsigned pct = globalStats.totalItems > 0 ? (globalStats.totalObtained * 100 / globalStats.totalItems) : 0;
    snprintf(globBtn, sizeof(globBtn), (lang == 6) ? "ITEMS: %u/%u (%u%%)" : "ITEMS: %u/%u (%u%%)",
             globalStats.totalObtained, globalStats.totalItems, pct);
    C2D_DrawRectSolid(182.0f, 170.0f, 0.5f, 126.0f, 14.0f, C2D_Color32(20, 50, 90, 255));
    C2D_DrawRectSolid(183.0f, 171.0f, 0.55f, 124.0f, 12.0f, C2D_Color32(12, 30, 60, 255));
    DrawTextCentered(245.0f, 173.0f, 1.0f, globBtn, C2D_Color32(255, 215, 0, 255));

    /* Row 1: 4 areas (Brinstar, Kraid, Norfair, Ridley) */
    for (int i = 0; i < 4; ++i) {
        bool dl = (gEquipment.downloadedMapStatus & (1 << i)) != 0;
        float bx = 14.0f + (float)i * 73.0f;
        float by = 188.0f;
        uint32_t boxBg = dl ? C2D_Color32(16, 50, 95, 255) : C2D_Color32(22, 26, 38, 255);
        uint32_t boxBorder = dl ? C2D_Color32(40, 160, 250, 255) : C2D_Color32(45, 52, 70, 255);
        uint32_t textCol = dl ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(90, 100, 120, 255);

        C2D_DrawRectSolid(bx, by, 0.5f, 68.0f, 18.0f, boxBorder);
        C2D_DrawRectSolid(bx + 1.0f, by + 1.0f, 0.55f, 66.0f, 16.0f, boxBg);
        DrawTextCentered(bx + 34.0f, by + 5.0f, 1.0f, AreaName(i), textCol);
    }

    /* Row 2: 3 areas (Tourian, Crateria, Chozodia) */
    for (int i = 4; i < 7; ++i) {
        bool dl = (gEquipment.downloadedMapStatus & (1 << i)) != 0;
        float bx = 14.0f + (float)(i - 4) * 98.0f;
        float by = 210.0f;
        uint32_t boxBg = dl ? C2D_Color32(16, 50, 95, 255) : C2D_Color32(22, 26, 38, 255);
        uint32_t boxBorder = dl ? C2D_Color32(40, 160, 250, 255) : C2D_Color32(45, 52, 70, 255);
        uint32_t textCol = dl ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(90, 100, 120, 255);

        C2D_DrawRectSolid(bx, by, 0.5f, 92.0f, 18.0f, boxBorder);
        C2D_DrawRectSolid(bx + 1.0f, by + 1.0f, 0.55f, 90.0f, 16.0f, boxBg);
        DrawTextCentered(bx + 46.0f, by + 5.0f, 1.0f, AreaName(i), textCol);
    }

    if (sShowCollectiblesModal) {
        RenderCollectiblesModal(lang);
    }
}

/* Render Options View with Smooth Touch Scrolling & RetroAchievements Group Box */
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

    float baseY = 34.0f - sOptionsScrollY;

    /* Section 1: General Display & System Settings */
    const char* optTitles[7] = {
        "DISPLAY & SYSTEM SETTINGS", "DISPLAY & SYSTEM SETTINGS", "DISPLAY & SYSTEM SETTINGS",
        "ANZEIGE- & SYSTEMEINSTELLUNGEN", "PARAMETRES D'AFFICHAGE", "IMPOSTAZIONI SISTEMA",
        "AJUSTES DE PANTALLA Y SISTEMA"
    };
    DrawTextClipped(16.0f, baseY, 1.0f, optTitles[lang], C2D_Color32(100, 220, 255, 255), viewY0, viewY1);

    /* Helper macro/lambda for clipped box drawing */
    #define DRAW_CLIPPED_ROW(y, h, bgCol, borderCol) do { \
        float dy0 = (y < viewY0) ? viewY0 : y; \
        float dy1 = (y + h > viewY1) ? viewY1 : (y + h); \
        if (dy1 > dy0) { \
            C2D_DrawRectSolid(12.0f, dy0, 0.45f, 296.0f, dy1 - dy0, bgCol); \
            if (y >= viewY0 && y <= viewY1) { \
                C2D_DrawRectSolid(12.0f, y, 0.4f, 296.0f, 1.0f, borderCol); \
            } \
        } \
    } while(0)

    /* Option 1: Aspect Ratio */
    float rY = 48.0f - sOptionsScrollY;
    DRAW_CLIPPED_ROW(rY, 24.0f, C2D_Color32(26, 32, 48, 255), C2D_Color32(60, 75, 110, 255));
    const char* arLabels[7] = {
        "ASPECT RATIO:", "ASPECT RATIO:", "ASPECT RATIO:",
        "BILDVERHAELTNIS:", "FORMAT IMAGE:", "FORMATO SCHERMO:", "RELACION DE ASPECTO:"
    };
    DrawTextClipped(20.0f, rY + 7.0f, 1.0f, arLabels[lang], C2D_Color32(255, 255, 255, 255), viewY0, viewY1);
    DrawTextClipped(170.0f, rY + 7.0f, 1.0f, GetAspectRatioDisplayName(lang), C2D_Color32(255, 215, 0, 255), viewY0, viewY1);

    /* Option 2: Display Style */
    rY = 76.0f - sOptionsScrollY;
    DRAW_CLIPPED_ROW(rY, 24.0f, C2D_Color32(26, 32, 48, 255), C2D_Color32(60, 75, 110, 255));
    const char* dsLabels[7] = {
        "DISPLAY STYLE:", "DISPLAY STYLE:", "DISPLAY STYLE:",
        "ANZEIGE-STIL:", "STYLE AFFICHAGE:", "STILE DISPLAY:", "ESTILO DE PANTALLA:"
    };
    DrawTextClipped(20.0f, rY + 7.0f, 1.0f, dsLabels[lang], C2D_Color32(255, 255, 255, 255), viewY0, viewY1);
    DrawTextClipped(170.0f, rY + 7.0f, 1.0f, GetDisplayStyleDisplayName(lang), C2D_Color32(255, 215, 0, 255), viewY0, viewY1);

    /* Option 3: FPS Overlay */
    rY = 104.0f - sOptionsScrollY;
    DRAW_CLIPPED_ROW(rY, 24.0f, C2D_Color32(26, 32, 48, 255), C2D_Color32(60, 75, 110, 255));
    const char* fpsLabels[7] = {
        "FPS OVERLAY:", "FPS OVERLAY:", "FPS OVERLAY:",
        "FPS-ANZEIGE:", "COMPTEUR FPS:", "OVERLAY FPS:", "CONTADOR FPS:"
    };
    DrawTextClipped(20.0f, rY + 7.0f, 1.0f, fpsLabels[lang], C2D_Color32(255, 255, 255, 255), viewY0, viewY1);
    DrawTextClipped(170.0f, rY + 7.0f, 1.0f, GetFpsOverlayDisplayName(lang),
             Port_Config_GetShowFps() ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255), viewY0, viewY1);

    /* Option 4: Auto-Hide HUD Option */
    rY = 132.0f - sOptionsScrollY;
    DRAW_CLIPPED_ROW(rY, 24.0f, C2D_Color32(26, 32, 48, 255), C2D_Color32(60, 75, 110, 255));
    DrawTextClipped(20.0f, rY + 7.0f, 1.0f, (lang == 6) ? "AUTOESCONDER HUD:" : "AUTO-HIDE HUD:", C2D_Color32(255, 255, 255, 255), viewY0, viewY1);
    bool autoHide = Port_Config_GetAutoHideHud();
    DrawTextClipped(170.0f, rY + 7.0f, 1.0f, autoHide ? ((lang == 6) ? "ACTIVADO" : "ON") : ((lang == 6) ? "DESACTIVADO" : "OFF"),
             autoHide ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255), viewY0, viewY1);

    /* Option 5: Button Remapping Submenu Button */
    rY = 160.0f - sOptionsScrollY;
    DRAW_CLIPPED_ROW(rY, 26.0f, C2D_Color32(20, 48, 80, 255), C2D_Color32(50, 100, 180, 255));
    DrawTextCenteredClipped(160.0f, rY + 8.0f, 1.0f, (lang == 6) ? "CONFIGURAR CONTROLES (X / Y / ZL / ZR)" : "CUSTOMIZE BUTTON CONTROLS (X / Y / ZL / ZR)", C2D_Color32(255, 220, 100, 255), viewY0, viewY1);

    /* Section 2: RetroAchievements Group Box */
    float gbY = 196.0f - sOptionsScrollY;
    float gbH = 166.0f;
    float gbDrawY0 = (gbY < viewY0) ? viewY0 : gbY;
    float gbDrawY1 = (gbY + gbH > viewY1) ? viewY1 : (gbY + gbH);
    if (gbDrawY1 > gbDrawY0) {
        /* Groupbox background & frame */
        C2D_DrawRectSolid(10.0f, gbDrawY0, 0.42f, 300.0f, gbDrawY1 - gbDrawY0, C2D_Color32(18, 22, 36, 255));
        if (gbY >= viewY0 && gbY <= viewY1) {
            C2D_DrawRectSolid(10.0f, gbY, 0.41f, 300.0f, 1.0f, C2D_Color32(70, 95, 140, 255));
        }
        if (gbY + gbH - 1.0f >= viewY0 && gbY + gbH - 1.0f <= viewY1) {
            C2D_DrawRectSolid(10.0f, gbY + gbH - 1.0f, 0.41f, 300.0f, 1.0f, C2D_Color32(70, 95, 140, 255));
        }
    }

    /* Groupbox Header */
    float raHeaderY = 202.0f - sOptionsScrollY;
    DrawTextClipped(18.0f, raHeaderY, 1.0f, (lang == 6) ? "RETROACHIEVEMENTS" : "RETROACHIEVEMENTS", C2D_Color32(255, 215, 0, 255), viewY0, viewY1);
    uint32_t statusCol = C2D_Color32(140, 160, 190, 255);
    switch (Port_RA_GetStatus()) {
        case RA_STATUS_CONNECTED: statusCol = C2D_Color32(80, 255, 120, 255); break;
        case RA_STATUS_CONNECTING: statusCol = C2D_Color32(255, 220, 80, 255); break;
        case RA_STATUS_ERROR: statusCol = C2D_Color32(255, 90, 90, 255); break;
        default: statusCol = C2D_Color32(140, 160, 190, 255); break;
    }
    DrawTextClipped(210.0f, raHeaderY, 1.0f, Port_RA_GetStatusString(lang), statusCol, viewY0, viewY1);

    /* RA Option 1: User Login (FIRST OPTION) */
    rY = 218.0f - sOptionsScrollY;
    DRAW_CLIPPED_ROW(rY, 24.0f, C2D_Color32(26, 32, 48, 255), C2D_Color32(60, 75, 110, 255));
    DrawTextClipped(20.0f, rY + 7.0f, 1.0f, (lang == 6) ? "USUARIO:" : "USER:", C2D_Color32(255, 255, 255, 255), viewY0, viewY1);
    const char* user = Port_RA_GetUsername();
    if (user && user[0] != '\0') {
        DrawTextClipped(170.0f, rY + 7.0f, 1.0f, user, C2D_Color32(255, 215, 0, 255), viewY0, viewY1);
    } else {
        DrawTextClipped(170.0f, rY + 7.0f, 1.0f, (lang == 6) ? "INICIAR SESION" : "LOGIN", C2D_Color32(255, 140, 80, 255), viewY0, viewY1);
    }

    /* RA Option 2: Enable / Disable */
    rY = 246.0f - sOptionsScrollY;
    DRAW_CLIPPED_ROW(rY, 24.0f, C2D_Color32(26, 32, 48, 255), C2D_Color32(60, 75, 110, 255));
    DrawTextClipped(20.0f, rY + 7.0f, 1.0f, (lang == 6) ? "SISTEMA DE LOGROS:" : "RETROACHIEVEMENTS:", C2D_Color32(255, 255, 255, 255), viewY0, viewY1);
    bool raEn = Port_RA_IsEnabled();
    DrawTextClipped(170.0f, rY + 7.0f, 1.0f, raEn ? ((lang == 6) ? "ACTIVADO" : "ENABLED") : ((lang == 6) ? "DESACTIVADO" : "DISABLED"),
             raEn ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255), viewY0, viewY1);

    /* RA Option 3: Hardcore Mode */
    rY = 274.0f - sOptionsScrollY;
    DRAW_CLIPPED_ROW(rY, 24.0f, C2D_Color32(26, 32, 48, 255), C2D_Color32(60, 75, 110, 255));
    DrawTextClipped(20.0f, rY + 7.0f, 1.0f, (lang == 6) ? "MODO HARDCORE:" : "HARDCORE MODE:", C2D_Color32(255, 255, 255, 255), viewY0, viewY1);
    bool hc = Port_RA_IsHardcore();
    DrawTextClipped(170.0f, rY + 7.0f, 1.0f, hc ? ((lang == 6) ? "ACTIVADO" : "ON") : ((lang == 6) ? "DESACTIVADO" : "OFF"),
             hc ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255), viewY0, viewY1);

    /* RA Option 4: Sound Popup Notification */
    rY = 302.0f - sOptionsScrollY;
    DRAW_CLIPPED_ROW(rY, 24.0f, C2D_Color32(26, 32, 48, 255), C2D_Color32(60, 75, 110, 255));
    DrawTextClipped(20.0f, rY + 7.0f, 1.0f, (lang == 6) ? "SONIDO DE LOGRO:" : "ACHIEVEMENT SOUND:", C2D_Color32(255, 255, 255, 255), viewY0, viewY1);
    bool snd = Port_RA_GetNotificationSound();
    DrawTextClipped(170.0f, rY + 7.0f, 1.0f, snd ? ((lang == 6) ? "ACTIVADO" : "ON") : ((lang == 6) ? "DESACTIVADO" : "OFF"),
             snd ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255), viewY0, viewY1);

    /* RA Option 5: View Achievements List */
    rY = 330.0f - sOptionsScrollY;
    DRAW_CLIPPED_ROW(rY, 28.0f, C2D_Color32(28, 50, 40, 255), C2D_Color32(60, 160, 100, 255));
    DrawTextCenteredClipped(160.0f, rY + 8.0f, 1.0f, (lang == 6) ? "VER LISTA Y ESTADO DE LOGROS" : "VIEW ACHIEVEMENTS & PROGRESS", C2D_Color32(120, 255, 180, 255), viewY0, viewY1);

    /* Informational bottom footer with version */
    float footY2 = 376.0f - sOptionsScrollY;
    DrawTextCenteredClipped(160.0f, footY2, 1.0f, "METROID ZERO MISSION 3DS " MZM_PORT_VERSION, C2D_Color32(90, 115, 145, 255), viewY0, viewY1);

    /* Scroll Bar Indicator (Right side) - Supports direct scrollbar dragging */
    const float maxScroll = 166.0f;
    float thumbH = 35.0f;
    float trackH = viewH - thumbH;
    float thumbY = viewY0 + (sOptionsScrollY / maxScroll) * trackH;
    C2D_DrawRectSolid(310.0f, viewY0, 0.5f, 3.0f, viewH, C2D_Color32(25, 32, 50, 255));
    C2D_DrawRectSolid(310.0f, thumbY, 0.55f, 3.0f, thumbH, C2D_Color32(80, 160, 240, 255));

    #undef DRAW_CLIPPED_ROW

    if (sShowRemapModal) {
        RenderRemapModal(lang);
    } else if (sShowAchievementsModal) {
        RenderAchievementsModal(lang);
    }
}

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

    DrawText(16.0f, 212.0f, 1.0f, (GetLang() == 6) ? "TOCA LA PESTANA [MAPA] PARA VOLVER" : "TOUCH [MAP] TAB TO RETURN TO MAP VIEW", C2D_Color32(120, 140, 170, 255));
}

void Port_BottomUI_Render(void) {
    ++sFrameCounter;

    /* Background clear for UI */
    C2D_DrawRectSolid(0.0f, 0.0f, 0.1f, 320.0f, 240.0f, C2D_Color32(6, 8, 14, 255));

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

    /* Update and render RetroAchievements toast if active */
    Port_RA_Update();
    Port_RA_RenderToastOverlay();
}
