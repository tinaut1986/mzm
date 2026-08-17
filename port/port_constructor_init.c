/*
 * All of these Init_* functions used to be __attribute__((constructor))
 * -- run automatically before main(), by the C runtime. That's too early:
 * they copy ROM-data pointers (macros resolved by PortGen_All_Init(), which
 * itself only runs once Port_LoadRom() is called from main()) into static
 * pointer-table arrays. Running before ROM load meant they permanently
 * captured NULL, causing a null-deref crash the first time any of these
 * tables got indexed (found via src/menus/pause_screen.c's
 * sMaintainedInputDelaysPointers, see docs/3ds-port-status-2026-08-17.md).
 * Called once from Port_LoadRom(), right after PortGen_All_Init(), instead.
 *
 * Declared weak + null-checked because several of these (mostly the
 * per-language src/data/text_data.c ones) only exist for specific
 * REGION_EU/REGION_US/REGION_JP builds -- easier to no-op the ones that
 * don't exist in a given build than to duplicate each file's exact ifdef
 * condition here.
 */

#include "port_constructor_init.h"

extern void Init_sEraseSramTextGfxPointers(void) __attribute__((weak));
extern void Init_sTitleScreenPointers(void) __attribute__((weak));
extern void Init_sGameOverTextPromptGfxPointers(void) __attribute__((weak));
extern void Init_sFileSelectTextGfxPointers(void) __attribute__((weak));
extern void Init_sFileSelectLargeTextGfxPointers(void) __attribute__((weak));
extern void Init_sFileSelectDifficultyTextGfxPointers(void) __attribute__((weak));
extern void Init_sMapScreenGfxPointers(void) __attribute__((weak));
extern void Init_sMapScreenChozoStatueAreaNamesGfxPointers(void) __attribute__((weak));
extern void Init_sMapScreenUnknownItemsNamesGfxPointers(void) __attribute__((weak));
extern void Init_sMapScreenEquipmentNamesGfxPointers(void) __attribute__((weak));
extern void Init_sMapScreenMenuNamesGfxPointers(void) __attribute__((weak));
extern void Init_sMinimapDataPointers(void) __attribute__((weak));
extern void Init_sMaintainedInputDelaysPointers(void) __attribute__((weak));
extern void Init_sBootDebugCutsceneBTextPointers(void) __attribute__((weak));
extern void Init_sPauseDebugEventNamePointers(void) __attribute__((weak));
extern void Init_sChozoStatueTargetPathPointers(void) __attribute__((weak));
extern void Init_sSpritesGfxPalPointers(void) __attribute__((weak));
extern void Init_sSpritesPalettePointers(void) __attribute__((weak));
extern void Init_sSpritesetPointers(void) __attribute__((weak));
extern void Init_sDemoRamDataPointers(void) __attribute__((weak));
extern void Init_sBackgroundEffectBehaviorPointers(void) __attribute__((weak));
extern void Init_sMotherBrainFrameDataPointers(void) __attribute__((weak));
extern void Init_sRidleyFrameDataPointers(void) __attribute__((weak));
extern void Init_sCrocomireFrameDataPointers(void) __attribute__((weak));
extern void Init_sKraidFrameDataPointers(void) __attribute__((weak));
extern void Init_sImagoCocoonFrameDataPointers(void) __attribute__((weak));
extern void Init_sImagoLarvaFrameDataPointers(void) __attribute__((weak));
extern void Init_sTangleVineFrameDataPointers(void) __attribute__((weak));
extern void Init_sChozoStatueFrameDataPointers(void) __attribute__((weak));
extern void Init_sImagoFrameDataPointers(void) __attribute__((weak));
extern void Init_sUnknownItemChozoStatueFrameDataPointers(void) __attribute__((weak));
extern void Init_sMechaRidleyFrameDataPointers(void) __attribute__((weak));
extern void Init_sDescriptionTextPointers(void) __attribute__((weak));
extern void Init_sHatchLockEventsPointers(void) __attribute__((weak));
extern void Init_sJapaneseTextPointers_Message(void) __attribute__((weak));
extern void Init_sJapaneseTextPointers_Location(void) __attribute__((weak));
extern void Init_sHiraganaTextPointers_Message(void) __attribute__((weak));
extern void Init_sHiraganaTextPointers_Location(void) __attribute__((weak));
extern void Init_sEnglishTextPointers_Message(void) __attribute__((weak));
extern void Init_sEnglishTextPointers_Location(void) __attribute__((weak));
extern void Init_sJapaneseTextPointers_Description(void) __attribute__((weak));
extern void Init_sJapaneseTextPointers_Story(void) __attribute__((weak));
extern void Init_sJapaneseTextPointers_FileScreen(void) __attribute__((weak));
extern void Init_sHiraganaTextPointers_Description(void) __attribute__((weak));
extern void Init_sHiraganaTextPointers_Story(void) __attribute__((weak));
extern void Init_sHiraganaTextPointers_FileScreen(void) __attribute__((weak));
extern void Init_sEnglishTextPointers_Description(void) __attribute__((weak));
extern void Init_sEnglishTextPointers_Story(void) __attribute__((weak));
extern void Init_sEnglishTextPointers_FileScreen(void) __attribute__((weak));
extern void Init_sGermanTextPointers_Description(void) __attribute__((weak));
extern void Init_sGermanTextPointers_Story(void) __attribute__((weak));
extern void Init_sFrenchTextPointers_Description(void) __attribute__((weak));
extern void Init_sFrenchTextPointers_Story(void) __attribute__((weak));
extern void Init_sItalianTextPointers_Description(void) __attribute__((weak));
extern void Init_sItalianTextPointers_Story(void) __attribute__((weak));
extern void Init_sSpanishTextPointers_Description(void) __attribute__((weak));
extern void Init_sSpanishTextPointers_Story(void) __attribute__((weak));
extern void Init_sPlayerNumbersStringPointers(void) __attribute__((weak));
extern void Init_sStoryTextPointers(void) __attribute__((weak));
extern void Init_sAreaPointers(void) __attribute__((weak));
extern void Init_sAreaRoomEntryPointers(void) __attribute__((weak));
extern void Init_sScrollTables(void) __attribute__((weak));
extern void Init_sPauseScreenPointers(void) __attribute__((weak));

void Port_InitConstructorPointers(void)
{
    if (Init_sEraseSramTextGfxPointers) Init_sEraseSramTextGfxPointers();
    if (Init_sTitleScreenPointers) Init_sTitleScreenPointers();
    if (Init_sGameOverTextPromptGfxPointers) Init_sGameOverTextPromptGfxPointers();
    if (Init_sFileSelectTextGfxPointers) Init_sFileSelectTextGfxPointers();
    if (Init_sFileSelectLargeTextGfxPointers) Init_sFileSelectLargeTextGfxPointers();
    if (Init_sFileSelectDifficultyTextGfxPointers) Init_sFileSelectDifficultyTextGfxPointers();
    if (Init_sMapScreenGfxPointers) Init_sMapScreenGfxPointers();
    if (Init_sMapScreenChozoStatueAreaNamesGfxPointers) Init_sMapScreenChozoStatueAreaNamesGfxPointers();
    if (Init_sMapScreenUnknownItemsNamesGfxPointers) Init_sMapScreenUnknownItemsNamesGfxPointers();
    if (Init_sMapScreenEquipmentNamesGfxPointers) Init_sMapScreenEquipmentNamesGfxPointers();
    if (Init_sMapScreenMenuNamesGfxPointers) Init_sMapScreenMenuNamesGfxPointers();
    if (Init_sMinimapDataPointers) Init_sMinimapDataPointers();
    if (Init_sMaintainedInputDelaysPointers) Init_sMaintainedInputDelaysPointers();
    if (Init_sBootDebugCutsceneBTextPointers) Init_sBootDebugCutsceneBTextPointers();
    if (Init_sPauseDebugEventNamePointers) Init_sPauseDebugEventNamePointers();
    if (Init_sChozoStatueTargetPathPointers) Init_sChozoStatueTargetPathPointers();
    if (Init_sSpritesGfxPalPointers) Init_sSpritesGfxPalPointers();
    if (Init_sSpritesPalettePointers) Init_sSpritesPalettePointers();
    if (Init_sSpritesetPointers) Init_sSpritesetPointers();
    if (Init_sDemoRamDataPointers) Init_sDemoRamDataPointers();
    if (Init_sBackgroundEffectBehaviorPointers) Init_sBackgroundEffectBehaviorPointers();
    if (Init_sMotherBrainFrameDataPointers) Init_sMotherBrainFrameDataPointers();
    if (Init_sRidleyFrameDataPointers) Init_sRidleyFrameDataPointers();
    if (Init_sCrocomireFrameDataPointers) Init_sCrocomireFrameDataPointers();
    if (Init_sKraidFrameDataPointers) Init_sKraidFrameDataPointers();
    if (Init_sImagoCocoonFrameDataPointers) Init_sImagoCocoonFrameDataPointers();
    if (Init_sImagoLarvaFrameDataPointers) Init_sImagoLarvaFrameDataPointers();
    if (Init_sTangleVineFrameDataPointers) Init_sTangleVineFrameDataPointers();
    if (Init_sChozoStatueFrameDataPointers) Init_sChozoStatueFrameDataPointers();
    if (Init_sImagoFrameDataPointers) Init_sImagoFrameDataPointers();
    if (Init_sUnknownItemChozoStatueFrameDataPointers) Init_sUnknownItemChozoStatueFrameDataPointers();
    if (Init_sMechaRidleyFrameDataPointers) Init_sMechaRidleyFrameDataPointers();
    if (Init_sDescriptionTextPointers) Init_sDescriptionTextPointers();
    if (Init_sHatchLockEventsPointers) Init_sHatchLockEventsPointers();
    if (Init_sJapaneseTextPointers_Message) Init_sJapaneseTextPointers_Message();
    if (Init_sJapaneseTextPointers_Location) Init_sJapaneseTextPointers_Location();
    if (Init_sHiraganaTextPointers_Message) Init_sHiraganaTextPointers_Message();
    if (Init_sHiraganaTextPointers_Location) Init_sHiraganaTextPointers_Location();
    if (Init_sEnglishTextPointers_Message) Init_sEnglishTextPointers_Message();
    if (Init_sEnglishTextPointers_Location) Init_sEnglishTextPointers_Location();
    if (Init_sJapaneseTextPointers_Description) Init_sJapaneseTextPointers_Description();
    if (Init_sJapaneseTextPointers_Story) Init_sJapaneseTextPointers_Story();
    if (Init_sJapaneseTextPointers_FileScreen) Init_sJapaneseTextPointers_FileScreen();
    if (Init_sHiraganaTextPointers_Description) Init_sHiraganaTextPointers_Description();
    if (Init_sHiraganaTextPointers_Story) Init_sHiraganaTextPointers_Story();
    if (Init_sHiraganaTextPointers_FileScreen) Init_sHiraganaTextPointers_FileScreen();
    if (Init_sEnglishTextPointers_Description) Init_sEnglishTextPointers_Description();
    if (Init_sEnglishTextPointers_Story) Init_sEnglishTextPointers_Story();
    if (Init_sEnglishTextPointers_FileScreen) Init_sEnglishTextPointers_FileScreen();
    if (Init_sGermanTextPointers_Description) Init_sGermanTextPointers_Description();
    if (Init_sGermanTextPointers_Story) Init_sGermanTextPointers_Story();
    if (Init_sFrenchTextPointers_Description) Init_sFrenchTextPointers_Description();
    if (Init_sFrenchTextPointers_Story) Init_sFrenchTextPointers_Story();
    if (Init_sItalianTextPointers_Description) Init_sItalianTextPointers_Description();
    if (Init_sItalianTextPointers_Story) Init_sItalianTextPointers_Story();
    if (Init_sSpanishTextPointers_Description) Init_sSpanishTextPointers_Description();
    if (Init_sSpanishTextPointers_Story) Init_sSpanishTextPointers_Story();
    if (Init_sPlayerNumbersStringPointers) Init_sPlayerNumbersStringPointers();
    if (Init_sStoryTextPointers) Init_sStoryTextPointers();
    if (Init_sAreaPointers) Init_sAreaPointers();
    if (Init_sAreaRoomEntryPointers) Init_sAreaRoomEntryPointers();
    if (Init_sScrollTables) Init_sScrollTables();
    if (Init_sPauseScreenPointers) Init_sPauseScreenPointers();
}
