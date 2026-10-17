#include "data/empty_datatypes.h"
#include "types.h"

#include "constants/block.h"
#include "constants/connection.h"
#include "constants/power_bomb_explosion.h"

#include "structs/screen_shake.h"

#if defined(MZM_3DS) || defined(PORT_NATIVE)
#undef sHatchData_Empty
#undef sCoordsX_Empty
#undef sLockScreen_Empty
#undef sBackgroundEffect_Empty
#undef sBrokenBlock_Empty
#undef sBombChain_Empty
#undef sPowerBomb_Empty
#undef sScreenShake_Empty
#undef sBg0Movement_Empty
#undef sBg3Movement_Empty
#undef sUnusedStruct_3005504_Empty
#undef sWaterMovement_Empty
#undef sTransparencyRelated_Empty
#undef sLastElevatorUsed_Empty
#undef sBldalphaData_Empty
#undef sBldyData_Empty
#undef sDefaultTransparency_Empty
#else
// 345868
#endif

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct HatchData sHatchData_Empty_Compiled = {
#else
const struct HatchData sHatchData_Empty = {
#endif
    .exists = FALSE,
    .currentAnimationFrame = 0,
    .facingRight = FALSE,
    .securityLevel = 0,
    .state = HATCH_STATE_CLOSED,
    .locked = HATCH_LOCK_STATE_UNLOCKED,
    .flashingTimer = 0,
    .hitTimer = 0,
    .hits = 0,
    .type = HATCH_NONE,
    .animationDurationCounter = 0,
    .xPosition = 0,
    .yPosition = 0,
    .sourceDoor = UCHAR_MAX
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct Coordinates sCoordsX_Empty_Compiled = {
#else
const struct Coordinates sCoordsX_Empty = {
#endif
    .x = USHORT_MAX,
    .y = USHORT_MAX
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct LockScreen sLockScreen_Empty_Compiled = {
#else
const struct LockScreen sLockScreen_Empty = {
#endif
    .lock = FALSE,
    .xPositionCenter = USHORT_MAX,
    .yPositionCenter = USHORT_MAX
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct BackgroundEffect sBackgroundEffect_Empty_Compiled = {
#else
const struct BackgroundEffect sBackgroundEffect_Empty = {
#endif
    .unused = 0,
    .timer = 0,
    .colorStage = 0,
    .type = 0,
    .stage = 0,
    .unk_7 = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct BrokenBlock sBrokenBlock_Empty_Compiled = {
#else
const struct BrokenBlock sBrokenBlock_Empty = {
#endif
    .broken = FALSE,
    .stage = 0,
    .type = BLOCK_TYPE_NONE,
    .xPosition = 0,
    .yPosition = 0,
    .timer = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct BombChain sBombChain_Empty_Compiled = {
#else
const struct BombChain sBombChain_Empty = {
#endif
    .currentOffset = 0,
    .srcXPosition = 0,
    .srcYPosition = 0,
    .type = 0,
    .padding = 0,
    .flipped = 0,
    .unk = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct PowerBomb sPowerBomb_Empty_Compiled = {
#else
const struct PowerBomb sPowerBomb_Empty = {
#endif
    .animationState = PB_STATE_NONE,
    .stage = 0,
    .semiMinorAxis = 0,
    .unk_3 = 0,
    .xPosition = 0,
    .yPosition = 0,
    .hitboxLeft = 0,
    .hitboxRight = 0,
    .hitboxTop = 0,
    .hitboxBottom = 0,
    .powerBombPlaced = FALSE,
    .owner = 0,
    .unk_12 = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct ScreenShake sScreenShake_Empty_Compiled = {
#else
const struct ScreenShake sScreenShake_Empty = {
#endif
    .timer = 0,
    .delay = 0,
    .intensity = 0,
    .direction = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct BG0Movement sBg0Movement_Empty_Compiled = {
#else
const struct BG0Movement sBg0Movement_Empty = {
#endif
    .type = 0,
    .counter = 0,
    .unused = 0,
    .xOffset = 0,
    .yOffset = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct BG3Movement sBg3Movement_Empty_Compiled = {
#else
const struct BG3Movement sBg3Movement_Empty = {
#endif
    .active = FALSE,
    .counter = 0,
    .xOffset = 0,
    .undefined = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct Unused_3005504 sUnusedStruct_3005504_Empty_Compiled = {
#else
const struct Unused_3005504 sUnusedStruct_3005504_Empty = {
#endif
    .field_0 = 0,
    .field_4 = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct WaterMovement sWaterMovement_Empty_Compiled = {
#else
const struct WaterMovement sWaterMovement_Empty = {
#endif
    .moving = FALSE,
    .stage = 0,
    .loopCounter = 0,
    .yOffset = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct TransparencyRelated sTransparencyRelated_Empty_Compiled = {
#else
const struct TransparencyRelated sTransparencyRelated_Empty = {
#endif
    .unk_0 = 0,
    .unk_1 = 0,
    .unk_2 = 0,
    .unk_3 = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct LastElevatorUsed sLastElevatorUsed_Empty_Compiled = {
#else
const struct LastElevatorUsed sLastElevatorUsed_Empty = {
#endif
    .unused = 0,
    .route = ELEVATOR_ROUTE_NONE,
    .direction = 0
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct BldalphaData sBldalphaData_Empty_Compiled = {
#else
const struct BldalphaData sBldalphaData_Empty = {
#endif
    .BLDCNT = 0,
    .activeFlag = FALSE,
    .evbCoef = 0,
    .evaCoef = 0,
    .delayMax = 0,
    .delay = 0,
    .intensity = 1
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct BldyData sBldyData_Empty_Compiled = {
#else
const struct BldyData sBldyData_Empty = {
#endif
    .BLDCNT = 0,
    .activeFlag = FALSE,
    .value = 0,
    .delayMax = 0,
    .delay = 0,
    .intensity = 1
};

#if defined(MZM_3DS) || defined(PORT_NATIVE)
const struct DefaultTransparency sDefaultTransparency_Empty_Compiled = {
#else
const struct DefaultTransparency sDefaultTransparency_Empty = {
#endif
    .unk_0 = 0,
    .unk_1 = 0,
    .evbCoef = 0,
    .evaCoef = 0,
    .bldcnt = 0
};
