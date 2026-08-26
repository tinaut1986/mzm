#ifndef PORT_BOTTOM_UI_3DS_H
#define PORT_BOTTOM_UI_3DS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOTTOM_TAB_MAP = 0,
    BOTTOM_TAB_STATUS,
    BOTTOM_TAB_DEBUG,
    BOTTOM_TAB_OPTIONS,
    BOTTOM_TAB_COUNT
} PortBottomTab;

void Port_BottomUI_Init(void);
void Port_BottomUI_SetTab(PortBottomTab tab);
PortBottomTab Port_BottomUI_GetTab(void);
int Port_BottomUI_GetZoom(void);
void Port_BottomUI_SetZoom(int zoom);
int Port_BottomUI_GetViewArea(void);
void Port_BottomUI_SetViewArea(int area);
bool Port_BottomUI_GetFollowSamus(void);
void Port_BottomUI_SetFollowSamus(bool follow);
void Port_BottomUI_HandleTouchDrag(int touchX, int touchY, bool isNewTap);
void Port_BottomUI_TouchReleased(void);
void Port_BottomUI_Render(void);

/* Returns true if the Debug tab should be visible (debug builds only).
 * In production builds PORT_DEBUG_TOOLS_ACTIVE is not defined, so this
 * returns false and the tab is hidden from the tab bar. */
bool Port_BottomUI_DebugTabVisible(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_BOTTOM_UI_3DS_H */
