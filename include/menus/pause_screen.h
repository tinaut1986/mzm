#ifndef PAUSE_SCREEN_MENU_H
#define PAUSE_SCREEN_MENU_H

#include "types.h"

#include "constants/connection.h"
#include "constants/menus/pause_screen.h"

#include "structs/menu.h"
#include "structs/cutscene.h"

u32 PauseScreenInitFading(u16 targetBldAlpha, u8 bldAlphaStepLevel, s8 bldAlphaStepDelayThreshold);
u8 PauseScreenApplyFading(void);
u32 PauseScreenUpdateOrStartFading(PauseScreenFading stage);
void PauseScreenCopyPalramToEwram_Unused(u8 param_1);
void PauseScreenCopyBackgroundPalette_Unused(void);
void PauseScreenUpdateMapArrows(void);
void PauseScreenUpdateBossIcons(void);
void PauseScreenDrawCompletionInfo(u8 dontDraw);
u8 PauseScreenStatusScreenShouldDrawHeader(SamusWireframeDataId samusWireframeDataIndex);
u32 PauseScreenUpdateStatusScreenOam(u8 param_1);
void PauseScreenUpdateWireframeSamus(u8 updateWireframeOption);
void PauseScreenFadeWireframeSamus(void);
void PauseScreenUpdateWorldMapHighlight(Area area);
void PauseScreenUpdateWorldMap(u8 onWorldMap);
void PauseScreenLoadAreaNamesAndIcons(void);
void PauseScreenProcessOam(void);
void ProcessMenuOam(u8 length, struct MenuOamData* pOam, const struct OamArray* pOamData);
void ProcessComplexMenuOam(u8 length, struct MenuOamData* pOam, const struct OamArray* pOamData);
void ProcessCutsceneOam(u8 length, struct CutsceneOamData* pOam, const struct OamArray* pOamData);
u32 PauseScreenHandler(void);
void PauseScreenVBlank(void);
void PauseScreenVBlank_Empty(void);
void PauseScreenInit(void);
void PauseScreenDetermineMapsViewable(void);
void PauseScreenUpdateBottomVisorOverlay(u8 param_1, u8 param_2);
void PauseScreenGetMinimapData(Area area, u16* dst);
u32 PauseScreenCallStateHandler(void);
void PauseScreenMoveDebugCursor(u8 allowOverflow);
u32 unk_6b66c_Unused(u16* param_1, u16 param_2);
u32 unk_6b6c4_Unused(u16* param_1, u16 param_2);
void PauseScreenUpdateTopVisorOverlay(u8 oamId);
s32 PauseScreenSuitChangingStart(void);
s32 PauseScreenStatusScreenInit(void);
s32 PauseScreenQuitStatusScreen(void);
s32 PauseScreenEasySleepInit(void);
s32 PauseScreenQuitEasySleep(void);
/*
 * EUR picks between a fast and a slow key-repeat table, so its version takes
 * a speed; USA/JAP only have the fast table and take no argument. The ports
 * serve every region from one binary, so there the speed is always a
 * parameter and the table choice happens at runtime.
 *
 * Call it through CHECK_MAINTAINED_INPUT() to stay region-neutral: on the
 * GBA USA/JAP build the argument is discarded, everywhere else it is passed.
 */
#if defined(MZM_3DS) || defined(PORT_NATIVE) || defined(REGION_EU)
void CheckForMaintainedInput(MaintainedInputSpeed speed);
#define CHECK_MAINTAINED_INPUT(speed) CheckForMaintainedInput(speed)
#else
void CheckForMaintainedInput(void);
#define CHECK_MAINTAINED_INPUT(speed) ((void)(speed), CheckForMaintainedInput())
#endif

#endif /* PAUSE_SCREEN_MENU_H */
