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

struct BottomEquipmentView {
    uint16_t maxEnergy;
    uint16_t maxMissiles;
    uint8_t maxSuperMissiles;
    uint8_t maxPowerBombs;
    uint16_t currentEnergy;
    uint16_t currentMissiles;
    uint8_t currentSuperMissiles;
    uint8_t currentPowerBombs;
    uint32_t beamBombs;
    uint32_t beamBombsActivation;
    uint32_t suitMisc;
    uint32_t suitMiscActivation;
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

/* Area selector & zoom state */
static uint8_t sViewArea = 0;
static bool sFollowSamus = true;
static int sZoomLevel = 1; /* 0 = 1x (Overview), 1 = 2x (Detail), 2 = 3x (Ultra) */
static float sScrollX = 0.0f;
static float sScrollY = 0.0f;
static int sLastTouchX = -1;
static int sLastTouchY = -1;
static bool sIsDragging = false;

/* Cached buffer for viewing other areas */
static uint16_t sOtherAreaTiles[32 * 32];
static uint8_t sCachedOtherArea = 0xFF;

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

/* Bitmap font 5x7 glyph renderer */
static const uint8_t* GetGlyph(char c) {
    static const uint8_t digits[10][7] = {
        { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },
        { 14, 17, 1, 2, 4, 8, 31 },     { 30, 1, 1, 14, 1, 1, 30 },
        { 2, 6, 10, 18, 31, 2, 2 },     { 31, 16, 16, 30, 1, 1, 30 },
        { 14, 16, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
        { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 1, 14 },
    };
    static const uint8_t letters[26][7] = {
        { 14, 17, 17, 31, 17, 17, 17 }, /* A */
        { 30, 17, 17, 30, 17, 17, 30 }, /* B */
        { 31, 16, 16, 16, 16, 16, 31 }, /* C */
        { 30, 17, 17, 17, 17, 17, 30 }, /* D */
        { 31, 16, 16, 30, 16, 16, 31 }, /* E */
        { 31, 16, 16, 30, 16, 16, 16 }, /* F */
        { 14, 17, 16, 23, 17, 17, 14 }, /* G */
        { 17, 17, 17, 31, 17, 17, 17 }, /* H */
        { 31, 4, 4, 4, 4, 4, 31 },      /* I */
        { 7, 2, 2, 2, 2, 18, 12 },      /* J */
        { 17, 18, 20, 24, 20, 18, 17 }, /* K */
        { 16, 16, 16, 16, 16, 16, 31 }, /* L */
        { 17, 27, 21, 21, 17, 17, 17 }, /* M */
        { 17, 25, 21, 19, 17, 17, 17 }, /* N */
        { 14, 17, 17, 17, 17, 17, 14 }, /* O */
        { 30, 17, 17, 30, 16, 16, 16 }, /* P */
        { 14, 17, 17, 21, 18, 9, 22 },  /* Q */
        { 30, 17, 17, 30, 20, 18, 17 }, /* R */
        { 15, 16, 16, 14, 1, 1, 30 },   /* S */
        { 31, 4, 4, 4, 4, 4, 4 },       /* T */
        { 17, 17, 17, 17, 17, 17, 14 }, /* U */
        { 17, 17, 17, 17, 17, 10, 4 },  /* V */
        { 17, 17, 17, 21, 21, 27, 17 }, /* W */
        { 17, 10, 4, 4, 4, 10, 17 },    /* X */
        { 17, 10, 4, 4, 4, 4, 4 },      /* Y */
        { 31, 1, 2, 4, 8, 16, 31 },     /* Z */
    };
    static const uint8_t colon[7]   = { 0, 12, 12, 0, 12, 12, 0 };
    static const uint8_t dot[7]     = { 0, 0, 0, 0, 0, 12, 12 };
    static const uint8_t dash[7]    = { 0, 0, 0, 31, 0, 0, 0 };
    static const uint8_t slash[7]   = { 1, 2, 2, 4, 8, 8, 16 };
    static const uint8_t percent[7] = { 19, 19, 2, 4, 8, 25, 25 };
    static const uint8_t lbracket[7]= { 14, 8, 8, 8, 8, 8, 14 };
    static const uint8_t rbracket[7]= { 14, 2, 2, 2, 2, 2, 14 };
    static const uint8_t lparen[7]  = { 2, 4, 8, 8, 8, 4, 2 };
    static const uint8_t rparen[7]  = { 8, 4, 2, 2, 2, 4, 8 };
    static const uint8_t comma[7]   = { 0, 0, 0, 0, 0, 12, 8 };
    static const uint8_t plus[7]    = { 0, 4, 4, 31, 4, 4, 0 };
    static const uint8_t equal[7]   = { 0, 31, 0, 31, 0, 0, 0 };
    static const uint8_t exclam[7]  = { 4, 4, 4, 4, 4, 0, 4 };
    static const uint8_t question[7]= { 14, 17, 1, 2, 4, 0, 4 };
    static const uint8_t gt[7]      = { 16, 8, 4, 2, 4, 8, 16 };
    static const uint8_t lt[7]      = { 1, 2, 4, 8, 4, 2, 1 };

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
    if (c == '(') return lparen;
    if (c == ')') return rparen;
    if (c == ',') return comma;
    if (c == '+') return plus;
    if (c == '=') return equal;
    if (c == '!') return exclam;
    if (c == '?') return question;
    if (c == '>') return gt;
    if (c == '<') return lt;
    return NULL;
}

static void DrawText(float x, float y, float scale, const char* text, uint32_t color) {
    for (; *text; ++text, x += 6.0f * scale) {
        const uint8_t* glyph = GetGlyph(*text);
        if (!glyph) continue;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5;) {
                if ((glyph[row] & (1u << (4 - col))) == 0) {
                    ++col;
                    continue;
                }
                int end = col + 1;
                while (end < 5 && (glyph[row] & (1u << (4 - end))) != 0) ++end;
                C2D_DrawRectSolid(x + col * scale, y + row * scale, 0.85f,
                                  (end - col) * scale, scale, color);
                col = end;
            }
        }
    }
}

static void DrawTextCentered(float cx, float y, float scale, const char* text, uint32_t color) {
    float length = (float)strlen(text) * 6.0f * scale;
    DrawText(cx - (length / 2.0f), y, scale, text, color);
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

void Port_BottomUI_HandleTouchDrag(int x, int y, bool isNewTap) {
    /* 4-Tab Top Navigation Bar (Y: 2 to 24) */
    if (y >= 2 && y <= 24) {
        if (isNewTap) {
            if (x >= 4 && x <= 78) sCurrentTab = BOTTOM_TAB_MAP;
            else if (x >= 82 && x <= 156) sCurrentTab = BOTTOM_TAB_STATUS;
            else if (x >= 160 && x <= 234) sCurrentTab = BOTTOM_TAB_DEBUG;
            else if (x >= 238 && x <= 316) sCurrentTab = BOTTOM_TAB_OPTIONS;
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
                if (x >= 4 && x <= 22) {
                    sFollowSamus = false;
                    sViewArea = (sViewArea == 0) ? 6 : (sViewArea - 1);
                    CenterMapOnTile(16, 16);
                }
                /* Next Area [ > ] */
                else if (x >= 100 && x <= 120) {
                    sFollowSamus = false;
                    sViewArea = (sViewArea + 1) % 7;
                    CenterMapOnTile(16, 16);
                }
                /* Area Title Box -> toggle back to current area */
                else if (x > 22 && x < 100) {
                    sFollowSamus = true;
                    sViewArea = gCurrentArea;
                    CenterMapOnTile(gMinimapX, gMinimapY);
                }
                /* Zoom Button [ ZOOM ] */
                else if (x >= 124 && x <= 190) {
                    sZoomLevel = (sZoomLevel + 1) % 3;
                    if (sFollowSamus && gCurrentArea < 7) {
                        CenterMapOnTile(gMinimapX, gMinimapY);
                    } else {
                        CenterMapOnTile(16, 16);
                    }
                }
                /* Center on Samus Button [ SAMUS ] */
                else if (x >= 194 && x <= 248) {
                    sFollowSamus = true;
                    sViewArea = gCurrentArea;
                    CenterMapOnTile(gMinimapX, gMinimapY);
                }
                /* Target Button [ ! OBJ ] */
                else if (x >= 252 && x <= 316) {
                    uint8_t tArea = 0, tX = 0, tY = 0;
                    if (GetActiveChozoTarget(&tArea, &tX, &tY)) {
                        sFollowSamus = (tArea == gCurrentArea);
                        sViewArea = tArea;
                        CenterMapOnTile(tX, tY);
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

    /* Options Tab Interactive Row Taps */
    if (sCurrentTab == BOTTOM_TAB_OPTIONS && isNewTap && x >= 12 && x <= 308) {
        if (y >= 38 && y <= 70) {
            Port_Config_Cycle3DSAspectRatio();
        } else if (y >= 76 && y <= 108) {
            Port_Config_Cycle3DSDisplayStyle();
        } else if (y >= 114 && y <= 146) {
            Port_Config_SetShowFps(!Port_Config_GetShowFps());
        }
    }
}

void Port_BottomUI_TouchReleased(void) {
    sLastTouchX = -1;
    sLastTouchY = -1;
    sIsDragging = false;
}

/* Render 4-Tab Navigation Bar */
static void RenderTabBar(void) {
    struct {
        float x;
        float w;
        const char* label;
        PortBottomTab tab;
    } tabs[] = {
        { 4.0f,   74.0f, "MAP",     BOTTOM_TAB_MAP },
        { 82.0f,  74.0f, "STATUS",  BOTTOM_TAB_STATUS },
        { 160.0f, 74.0f, "DEBUG",   BOTTOM_TAB_DEBUG },
        { 238.0f, 78.0f, "OPTIONS", BOTTOM_TAB_OPTIONS }
    };

    for (int i = 0; i < 4; ++i) {
        bool active = (sCurrentTab == tabs[i].tab);
        uint32_t bg = active ? C2D_Color32(18, 70, 130, 255) : C2D_Color32(26, 30, 42, 255);
        uint32_t border = active ? C2D_Color32(45, 150, 240, 255) : C2D_Color32(50, 56, 75, 255);
        uint32_t textColor = active ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(140, 150, 175, 255);

        C2D_DrawRectSolid(tabs[i].x, 3.0f, 0.4f, tabs[i].w, 20.0f, border);
        C2D_DrawRectSolid(tabs[i].x + 1.0f, 4.0f, 0.5f, tabs[i].w - 2.0f, 18.0f, bg);
        DrawTextCentered(tabs[i].x + tabs[i].w / 2.0f, 9.0f, 1.1f, tabs[i].label, textColor);
    }
}

/* Decodes and draws an authentic 8x8 4bpp GBA minimap tile with correct ROM palette */
static void DrawAuthenticMapTile(float dstX, float dstY, float tileSize, uint16_t tileData,
                                float clipX0, float clipY0, float clipX1, float clipY1) {
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
            C2D_DrawRectSolid(rx0, ry0, 0.5f, rx1 - rx0, ry1 - ry0, fbCol);
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
            C2D_DrawRectSolid(drawX0, drawY0, 0.5f, drawX1 - drawX0, drawY1 - drawY0, color);
        }
    }
}

/* Authentic HUD Item Sprites */
static void DrawEnergyTankIcon(float x, float y) {
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

/* Render Map View */
static void RenderMapView(void) {
    if (sFollowSamus) {
        sViewArea = gCurrentArea;
    }

    /* 1. Sub-header bar controls (Y: 26 to 45) */
    /* Area selector [ < ] AreaName [ > ] */
    C2D_DrawRectSolid(4.0f, 26.0f, 0.4f, 18.0f, 18.0f, C2D_Color32(30, 40, 60, 255));
    DrawTextCentered(13.0f, 31.0f, 1.0f, "<", C2D_Color32(100, 220, 255, 255));

    C2D_DrawRectSolid(24.0f, 26.0f, 0.4f, 76.0f, 18.0f, sFollowSamus ? C2D_Color32(16, 50, 95, 255) : C2D_Color32(26, 32, 48, 255));
    DrawTextCentered(62.0f, 31.0f, 1.0f, AreaName(sViewArea), sFollowSamus ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(180, 205, 235, 255));

    C2D_DrawRectSolid(102.0f, 26.0f, 0.4f, 18.0f, 18.0f, C2D_Color32(30, 40, 60, 255));
    DrawTextCentered(111.0f, 31.0f, 1.0f, ">", C2D_Color32(100, 220, 255, 255));

    /* Zoom Toggle Button */
    const char* zoomLabels[] = { "ZOOM 1X", "ZOOM 2X", "ZOOM 3X" };
    C2D_DrawRectSolid(124.0f, 26.0f, 0.4f, 66.0f, 18.0f, C2D_Color32(24, 45, 75, 255));
    C2D_DrawRectSolid(125.0f, 27.0f, 0.45f, 64.0f, 16.0f, C2D_Color32(14, 25, 45, 255));
    DrawTextCentered(157.0f, 31.0f, 1.0f, zoomLabels[sZoomLevel], C2D_Color32(255, 215, 0, 255));

    /* Center on Samus Button */
    C2D_DrawRectSolid(194.0f, 26.0f, 0.4f, 54.0f, 18.0f, sFollowSamus ? C2D_Color32(20, 80, 140, 255) : C2D_Color32(26, 35, 55, 255));
    DrawTextCentered(221.0f, 31.0f, 1.0f, "SAMUS", sFollowSamus ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(140, 160, 190, 255));

    /* Chozo Objective Badge */
    uint8_t targetArea = 0, targetX = 0, targetY = 0;
    bool hasTarget = GetActiveChozoTarget(&targetArea, &targetX, &targetY);
    if (hasTarget) {
        bool inViewArea = (targetArea == sViewArea);
        uint32_t targetBg = inViewArea
            ? (((sFrameCounter % 20) < 10) ? C2D_Color32(200, 40, 40, 255) : C2D_Color32(160, 120, 0, 255))
            : C2D_Color32(80, 60, 20, 255);
        C2D_DrawRectSolid(252.0f, 26.0f, 0.4f, 64.0f, 18.0f, targetBg);
        DrawTextCentered(284.0f, 31.0f, 1.0f, inViewArea ? "! TARGET" : "! OBJ", C2D_Color32(255, 255, 255, 255));
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
                DrawAuthenticMapTile(tileDrawX, tileDrawY, tileSize, tileData, clipX0, clipY0, clipX1, clipY1);
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
    C2D_DrawRectSolid(6.0f, 26.0f, 0.4f, 308.0f, 210.0f, C2D_Color32(12, 16, 26, 255));
    C2D_DrawRectSolid(6.0f, 26.0f, 0.35f, 308.0f, 210.0f, C2D_Color32(35, 50, 75, 255));

    DrawText(14.0f, 32.0f, 1.1f, "SAMUS ARAN - STATUS & EQUIPMENT", C2D_Color32(100, 220, 255, 255));

    /* 1. Health & Ammunition Card (Y: 48 to 82) */
    C2D_DrawRectSolid(10.0f, 48.0f, 0.45f, 300.0f, 34.0f, C2D_Color32(20, 26, 40, 255));
    C2D_DrawRectSolid(10.0f, 48.0f, 0.4f, 300.0f, 1.0f, C2D_Color32(50, 65, 95, 255));

    /* Energy */
    DrawEnergyTankIcon(16.0f, 60.0f);
    char eBuf[32];
    snprintf(eBuf, sizeof(eBuf), "%03u/%03u", gEquipment.currentEnergy, gEquipment.maxEnergy);
    DrawText(33.0f, 60.0f, 1.0f, eBuf, C2D_Color32(255, 255, 255, 255));

    /* Missiles */
    DrawMissileIcon(92.0f, 60.0f);
    char mBuf[32];
    if (gEquipment.maxMissiles > 0) {
        snprintf(mBuf, sizeof(mBuf), "%03u/%03u", gEquipment.currentMissiles, gEquipment.maxMissiles);
        DrawText(109.0f, 60.0f, 1.0f, mBuf, C2D_Color32(255, 140, 140, 255));
    } else {
        DrawText(109.0f, 60.0f, 1.0f, "---/---", C2D_Color32(120, 130, 150, 255));
    }

    /* Super Missiles */
    DrawSuperMissileIcon(168.0f, 60.0f);
    char smBuf[32];
    if (gEquipment.maxSuperMissiles > 0) {
        snprintf(smBuf, sizeof(smBuf), "%02u/%02u", gEquipment.currentSuperMissiles, gEquipment.maxSuperMissiles);
        DrawText(185.0f, 60.0f, 1.0f, smBuf, C2D_Color32(100, 255, 140, 255));
    } else {
        DrawText(185.0f, 60.0f, 1.0f, "--/--", C2D_Color32(120, 130, 150, 255));
    }

    /* Power Bombs */
    DrawPowerBombIcon(244.0f, 60.0f);
    char pbBuf[32];
    if (gEquipment.maxPowerBombs > 0) {
        snprintf(pbBuf, sizeof(pbBuf), "%02u/%02u", gEquipment.currentPowerBombs, gEquipment.maxPowerBombs);
        DrawText(260.0f, 60.0f, 1.0f, pbBuf, C2D_Color32(255, 225, 80, 255));
    } else {
        DrawText(260.0f, 60.0f, 1.0f, "--/--", C2D_Color32(120, 130, 150, 255));
    }

    /* 2. Beams & Weapons Column (X: 10, W: 146, Y: 88 to 184) */
    C2D_DrawRectSolid(10.0f, 88.0f, 0.45f, 146.0f, 96.0f, C2D_Color32(18, 22, 34, 255));
    DrawText(14.0f, 92.0f, 1.0f, "BEAMS & BOMBS", C2D_Color32(255, 215, 0, 255));

    struct {
        const char* name;
        uint8_t flag;
    } beams[] = {
        { "LONG BEAM",   1 << 0 },
        { "ICE BEAM",    1 << 1 },
        { "WAVE BEAM",   1 << 2 },
        { "PLASMA BEAM", 1 << 3 },
        { "CHARGE BEAM", 1 << 4 },
        { "NORMAL BOMBS",1 << 7 }
    };
    for (int i = 0; i < 6; ++i) {
        bool has = (gEquipment.beamBombs & beams[i].flag) != 0;
        uint32_t col = has ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(70, 80, 100, 255);
        DrawText(14.0f, 104.0f + (float)i * 12.0f, 0.9f, beams[i].name, col);
        DrawText(120.0f, 104.0f + (float)i * 12.0f, 0.9f, has ? "ON" : "--", col);
    }

    /* 3. Suits & Upgrades Column (X: 164, W: 146, Y: 88 to 184) */
    C2D_DrawRectSolid(164.0f, 88.0f, 0.45f, 146.0f, 96.0f, C2D_Color32(18, 22, 34, 255));
    DrawText(168.0f, 92.0f, 1.0f, "SUITS & MISC", C2D_Color32(255, 215, 0, 255));

    struct {
        const char* name;
        uint8_t flag;
    } suits[] = {
        { "HIGH JUMP",   1 << 0 },
        { "SPEEDBOOSTER",1 << 1 },
        { "SPACE JUMP",  1 << 2 },
        { "SCREW ATTACK",1 << 3 },
        { "VARIA SUIT",  1 << 4 },
        { "GRAVITY SUIT",1 << 5 },
        { "MORPH BALL",  1 << 6 },
        { "POWER GRIP",  1 << 7 }
    };
    for (int i = 0; i < 8; ++i) {
        bool has = (gEquipment.suitMisc & suits[i].flag) != 0;
        uint32_t col = has ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(70, 80, 100, 255);
        float py = 104.0f + (float)i * 10.0f;
        if (i < 7) {
            DrawText(168.0f, py, 0.85f, suits[i].name, col);
            DrawText(274.0f, py, 0.85f, has ? "OK" : "--", col);
        }
    }

    /* 4. Area Map Downloads (Y: 190 to 230) */
    C2D_DrawRectSolid(10.0f, 190.0f, 0.45f, 300.0f, 40.0f, C2D_Color32(16, 20, 30, 255));
    DrawText(14.0f, 194.0f, 0.9f, "DOWNLOADED MAPS:", C2D_Color32(100, 220, 255, 255));
    for (int i = 0; i < 7; ++i) {
        bool dl = (gEquipment.downloadedMapStatus & (1 << i)) != 0;
        uint32_t c = dl ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(70, 80, 100, 255);
        DrawText(14.0f + (float)i * 42.0f, 208.0f, 0.85f, AreaName(i), c);
    }
}

/* Render Debug View with Map Tile Inspector */
static void RenderDebugView(void) {
    C2D_DrawRectSolid(8.0f, 28.0f, 0.4f, 304.0f, 204.0f, C2D_Color32(14, 18, 28, 255));
    C2D_DrawRectSolid(8.0f, 28.0f, 0.35f, 304.0f, 204.0f, C2D_Color32(40, 60, 90, 255));

    DrawText(16.0f, 32.0f, 1.0f, "PERFORMANCE & MAP TILE DEBUG", C2D_Color32(100, 220, 255, 255));

    char buf[80];
    double fps = Port_PPU_3DS_CurrentFps();
    unsigned roundedFps = fps > 0.0 ? (unsigned)(fps + 0.5) : 0u;
    if (roundedFps > 999u) roundedFps = 999u;
    const bool usedGpu = Port_PPU_3DS_LastFrameUsedGpu();
    const bool isNew3ds = Platform3DS_IsNew3DS();

    snprintf(buf, sizeof(buf), "FPS: %u   RENDER: %s   SYS: %s",
             roundedFps, usedGpu ? "GPU" : "CPU", isNew3ds ? "N3DS" : "O3DS");
    DrawText(16.0f, 46.0f, 0.9f, buf, C2D_Color32(255, 255, 255, 255));

    snprintf(buf, sizeof(buf), "GAME STATE: AREA=%s (%u) ROOM=%u",
             AreaName(gCurrentArea), gCurrentArea, gCurrentRoom);
    DrawText(16.0f, 58.0f, 0.9f, buf, C2D_Color32(180, 240, 160, 255));

    snprintf(buf, sizeof(buf), "MAP COORDS: X=%u Y=%u", gMinimapX, gMinimapY);
    DrawText(16.0f, 70.0f, 0.9f, buf, C2D_Color32(180, 240, 160, 255));

    /* Current room raw tile data & surrounding 3x3 inspect */
    uint16_t curTile = gDecompressedMinimapVisitedTiles[gMinimapX + gMinimapY * 32];
    uint16_t baseTile = gDecompressedMinimapData[gMinimapX + gMinimapY * 32];
    snprintf(buf, sizeof(buf), "CURRENT TILE: VISITED=0x%04X (T=%u P=%u) BASE=0x%04X",
             curTile, curTile & 0x3FF, (curTile >> 12) & 0xF, baseTile);
    DrawText(16.0f, 84.0f, 0.9f, buf, C2D_Color32(255, 220, 100, 255));

    /* 3x3 Grid surrounding Samus */
    DrawText(16.0f, 98.0f, 0.9f, "SURROUNDING 3x3 TILES (VISITED / BASE):", C2D_Color32(140, 200, 255, 255));
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
        DrawText(20.0f, 110.0f + (float)(dy + 1) * 12.0f, 0.85f, rowBuf, C2D_Color32(200, 220, 240, 255));
    }

    /* Active Chozo target status */
    uint8_t tArea = 0, tX = 0, tY = 0;
    bool hasTarget = GetActiveChozoTarget(&tArea, &tX, &tY);
    if (hasTarget) {
        snprintf(buf, sizeof(buf), "CHOZO TARGET: AREA=%s (%u) X=%u Y=%u", AreaName(tArea), tArea, tX, tY);
        DrawText(16.0f, 150.0f, 0.9f, buf, C2D_Color32(255, 120, 120, 255));
    } else {
        DrawText(16.0f, 150.0f, 0.9f, "CHOZO TARGET: NONE ACTIVE", C2D_Color32(140, 150, 170, 255));
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
    DrawText(16.0f, 164.0f, 0.9f, buf, C2D_Color32(180, 240, 160, 255));

    DrawText(16.0f, 212.0f, 0.85f, "TOUCH [MAP] TAB TO RETURN TO MAP VIEW", C2D_Color32(120, 140, 170, 255));
}

/* Render Options View */
static void RenderOptionsView(void) {
    C2D_DrawRectSolid(8.0f, 28.0f, 0.4f, 304.0f, 204.0f, C2D_Color32(14, 18, 28, 255));

    DrawText(16.0f, 32.0f, 1.0f, "SETTINGS (TOUCH AN OPTION TO CHANGE)", C2D_Color32(100, 220, 255, 255));

    /* Option 1: Aspect Ratio */
    C2D_DrawRectSolid(12.0f, 44.0f, 0.45f, 296.0f, 26.0f, C2D_Color32(26, 32, 48, 255));
    C2D_DrawRectSolid(12.0f, 44.0f, 0.4f, 296.0f, 1.0f, C2D_Color32(60, 75, 110, 255));
    DrawText(20.0f, 52.0f, 1.1f, "ASPECT RATIO:", C2D_Color32(255, 255, 255, 255));
    DrawText(170.0f, 52.0f, 1.1f, Port_Config_Get3DSAspectRatioName(), C2D_Color32(255, 215, 0, 255));

    /* Option 2: Display Style */
    C2D_DrawRectSolid(12.0f, 76.0f, 0.45f, 296.0f, 26.0f, C2D_Color32(26, 32, 48, 255));
    C2D_DrawRectSolid(12.0f, 76.0f, 0.4f, 296.0f, 1.0f, C2D_Color32(60, 75, 110, 255));
    DrawText(20.0f, 84.0f, 1.1f, "DISPLAY STYLE:", C2D_Color32(255, 255, 255, 255));
    DrawText(170.0f, 84.0f, 1.1f, Port_Config_Get3DSDisplayStyleName(), C2D_Color32(255, 215, 0, 255));

    /* Option 3: FPS Overlay */
    C2D_DrawRectSolid(12.0f, 108.0f, 0.45f, 296.0f, 26.0f, C2D_Color32(26, 32, 48, 255));
    C2D_DrawRectSolid(12.0f, 108.0f, 0.4f, 296.0f, 1.0f, C2D_Color32(60, 75, 110, 255));
    DrawText(20.0f, 116.0f, 1.1f, "FPS OVERLAY:", C2D_Color32(255, 255, 255, 255));
    DrawText(170.0f, 116.0f, 1.1f, Port_Config_GetShowFps() ? "ON" : "OFF",
             Port_Config_GetShowFps() ? C2D_Color32(80, 255, 120, 255) : C2D_Color32(255, 100, 100, 255));

    /* Informational Notes Box */
    C2D_DrawRectSolid(12.0f, 142.0f, 0.45f, 296.0f, 84.0f, C2D_Color32(18, 22, 34, 255));
    C2D_DrawRectSolid(12.0f, 142.0f, 0.4f, 296.0f, 1.0f, C2D_Color32(50, 65, 95, 255));

    DrawText(20.0f, 150.0f, 1.0f, "NOTES & TIPS:", C2D_Color32(100, 220, 255, 255));
    DrawText(20.0f, 166.0f, 1.0f, "- DRAG WITH STYLUS TO PAN ACROSS ZOOMED MAPS", C2D_Color32(180, 195, 215, 255));
    DrawText(20.0f, 180.0f, 1.0f, "- USE [<] AND [>] TO VIEW UNLOCKED AREAS", C2D_Color32(180, 195, 215, 255));
    DrawText(20.0f, 194.0f, 1.0f, "- TAP [SAMUS] TO INSTANTLY RE-CENTER ON PLAYER", C2D_Color32(180, 195, 215, 255));
    DrawText(20.0f, 210.0f, 1.0f, "- TAP [STATUS] TO VIEW DETAILED SAMUS EQUIPMENT", C2D_Color32(140, 220, 160, 255));
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
}
