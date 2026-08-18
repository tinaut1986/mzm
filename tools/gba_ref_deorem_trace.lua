-- Reference trace for the Deorem "first missile tank" encounter, run against
-- the real EU retail ROM in mGBA (Tools > Scripting). Addresses come from
-- mzm_eu.map (matching decomp build), so this is ground truth independent of
-- any C-source reading.
--
-- Usage: mgba-qt mzm_eu.gba, then Tools > Scripting > load this file.
-- Output goes to the mGBA script console (Tools > Scripting window) AND to
-- gba_ref_deorem_trace.log next to the ROM if io.open is permitted.

local ADDR_G_CURRENT_AREA = 0x03000054
local ADDR_G_CURRENT_ROOM = 0x03000055
local ADDR_G_SPRITE_DATA = 0x030001ac
local ADDR_G_CURRENT_SPRITE = 0x03000738
local ADDR_G_SAMUS_DATA = 0x030013d4
local ADDR_G_EQUIPMENT = 0x03001530
local ADDR_G_PREVENT_MOVEMENT_TIMER = 0x03001606
local ADDR_G_COLLECTING_TANK = 0x03000044       -- s8
local ADDR_G_DISABLE_PAUSE = 0x03000049         -- s8
local ADDR_G_LOCK_SCREEN = 0x03000100           -- struct { u8 lock; u16 xCenter; u16 yCenter; }
local ADDR_G_PAUSE_SCREEN_FLAG = 0x03000bf0     -- s8 (PauseScreenFlag)
local ADDR_G_CURRENT_ITEM_BEING_ACQUIRED = 0x03000bf2 -- s8

-- struct SpriteData layout offsets, hand-computed from include/structs/sprite.h
-- (all enum-backed fields there are u8 except SpriteStatus which is u16;
-- pOam is a 4-byte pointer forcing 4-byte struct alignment/padding).
-- status(u16)@0, yPosition(u16)@2, xPosition(u16)@4, yPositionSpawn(u16)@6,
-- xPositionSpawn(u16)@8, hitboxTop@10, hitboxBottom@12, hitboxLeft@14,
-- hitboxRight@16, scaling@18, health@20, currentAnimationFrame@22, pOam@24(ptr),
-- animationDurationCounter@28(u8), spriteId@29(u8), roomSlot@30, spritesetGfxSlot@31,
-- paletteRow@32, bgPriority@33, drawOrder@34, primarySpriteRamSlot@35, pose@36(u8),
-- samusCollision@37, ignoreSamusCollisionTimer@38, drawDistanceTop@39,
-- drawDistanceBottom@40, drawDistanceHorizontal@41, rotation@42,
-- invincibilityStunFlashTimer@43, work0@44(u8), work1@45, work2@46, work3@47,
-- freezeTimer@48, standingOnSprite@49, properties@50, frozenPaletteRowOffset@51,
-- absolutePaletteRow@52 -> struct size 53, padded to 4-byte alignment = 56 (0x38).
-- VERIFY: if the trace's field values look nonsensical, this is the first
-- thing to double check (e.g. via a debugger on the Linux native port, which
-- can print sizeof(struct SpriteData) directly).
local SPRITE_SIZE = 0x38
local OFF_STATUS = 0x00      -- SpriteStatus status; (u16)
local OFF_Y_POSITION = 0x02  -- u16 yPosition;
local OFF_X_POSITION = 0x04  -- u16 xPosition;
local OFF_SPRITE_ID = 0x1D   -- u8 spriteId; (29)
local OFF_POSE = 0x24        -- u8 pose; (36)
local OFF_WORK0 = 0x2C       -- u8 work0; (44)

-- Confirmed empirically from 3DS hardware logs this same session: Deorem's
-- spriteId reads as 66 (SpriteInitPrimary/SpriteSpawnPrimary logs).
local PSPRITE_DEOREM_FIRST_LOCATION = 66

local MAX_SPRITES = 24

local logFile = nil
local ok, f = pcall(io.open, "gba_ref_deorem_trace.log", "w")
if ok and f then logFile = f end

local function logLine(s)
    console:log(s)
    if logFile then
        logFile:write(s .. "\n")
        logFile:flush()
    end
end

local frame = 0
local lastMaxMissiles = -1
local lastDeoremStatus = -1
local lastDeoremPose = -1
local deoremEverSeen = false
local lastPauseFlag = -999
local lastCollectingTank = -999
local lastLockScreen = -999
local lastItemBeingAcquired = -999
local lastDisablePause = -999

logLine(string.format("=== trace start === SPRITE_SIZE=0x%X (VERIFY against include/structs/sprite.h!)", SPRITE_SIZE))

callbacks:add("frame", function()
    frame = frame + 1

    local area = emu:read8(ADDR_G_CURRENT_AREA)
    local room = emu:read8(ADDR_G_CURRENT_ROOM)
    -- gEquipment.maxMissiles: struct Equipment { u16 maxEnergy; u16 maxMissiles; ... }
    -- (include/structs/samus.h) -> offset 0x02
    local maxMissiles = emu:read16(ADDR_G_EQUIPMENT + 0x02)
    local pmt = emu:read16(ADDR_G_PREVENT_MOVEMENT_TIMER)
    local pauseFlag = emu:read8(ADDR_G_PAUSE_SCREEN_FLAG)
    local collectingTank = emu:read8(ADDR_G_COLLECTING_TANK)
    local lockScreen = emu:read8(ADDR_G_LOCK_SCREEN)
    local itemBeingAcquired = emu:read8(ADDR_G_CURRENT_ITEM_BEING_ACQUIRED)
    local disablePause = emu:read8(ADDR_G_DISABLE_PAUSE)

    if maxMissiles ~= lastMaxMissiles then
        logLine(string.format("[f=%d] maxMissiles changed: %d -> %d (area=%d room=%d)", frame, lastMaxMissiles, maxMissiles, area, room))
        lastMaxMissiles = maxMissiles
    end

    if pauseFlag ~= lastPauseFlag or collectingTank ~= lastCollectingTank or lockScreen ~= lastLockScreen
        or itemBeingAcquired ~= lastItemBeingAcquired or disablePause ~= lastDisablePause then
        logLine(string.format("[f=%d] STATE pauseFlag=%d collectingTank=%d lockScreen=%d itemBeingAcquired=%d disablePause=%d pmt=%d",
            frame, pauseFlag, collectingTank, lockScreen, itemBeingAcquired, disablePause, pmt))
        lastPauseFlag = pauseFlag
        lastCollectingTank = collectingTank
        lastLockScreen = lockScreen
        lastItemBeingAcquired = itemBeingAcquired
        lastDisablePause = disablePause
    end

    -- Scan gSpriteData for Deorem
    for i = 0, MAX_SPRITES - 1 do
        local base = ADDR_G_SPRITE_DATA + i * SPRITE_SIZE
        local spriteId = emu:read8(base + OFF_SPRITE_ID)
        if spriteId == PSPRITE_DEOREM_FIRST_LOCATION then
            deoremEverSeen = true
            local status = emu:read16(base + OFF_STATUS)
            local pose = emu:read8(base + OFF_POSE)
            local y = emu:read16(base + OFF_Y_POSITION)
            local x = emu:read16(base + OFF_X_POSITION)
            local work0 = emu:read8(base + OFF_WORK0)
            if status ~= lastDeoremStatus or pose ~= lastDeoremPose then
                logLine(string.format("[f=%d] Deorem slot=%d status=0x%04X pose=%d y=%d x=%d work0=%d pmt=%d",
                    frame, i, status, pose, y, x, work0, pmt))
                lastDeoremStatus = status
                lastDeoremPose = pose
            end
            return
        end
    end

    if deoremEverSeen and lastDeoremStatus ~= 0 then
        logLine(string.format("[f=%d] Deorem VANISHED from gSpriteData (was status=0x%04X pose=%d)", frame, lastDeoremStatus, lastDeoremPose))
        lastDeoremStatus = 0
    end
end)
