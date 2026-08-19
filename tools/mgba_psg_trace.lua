--[[
  mGBA Lua script: trace which GBA sound channels Metroid Zero Mission actually
  drives, on real (emulated) hardware.

  WHY THIS EXISTS
  ---------------
  The 3DS port software-mixes the engine's Direct Sound voices, and now also
  synthesises the four PSG voices (port/port_psg_synth.c). Music still sounds
  like it is missing tracks. The open question is which side is at fault, and
  from inside the port both possibilities look identical:

    (a) the engine never starts notes on those voices in the port  -> port bug
    (b) the real game does not use those voices much either        -> the
        missing tracks are Direct Sound ones, and the bug is elsewhere

  Running this against the real ROM in mGBA answers that directly: it reports
  which channels the genuine engine drives, how often, and with what values.
  Whatever this prints is ground truth to compare the port against.

  HOW TO USE
  ----------
  1. Open the ROM in mGBA.
  2. Tools > Scripting...  ->  File > Load script...  -> pick this file.
  3. Play normally. Prefer a spot with full music (title screen, or Brinstar
     after starting a game). Let it run at least 30 seconds.
  4. Read the summary lines in the scripting console. If file writing is
     permitted, a CSV also lands next to the ROM as mzm_psg_trace.csv.

  The summary line looks like:

    [ 30s] active now: sq1=  8% sq2= 41% wave=  0% noise= 55%   ever: sq1 sq2 noise

  "active" means the channel has a non-zero volume that frame, i.e. the engine
  is actually sounding a note on it. What matters most is the "ever" list: any
  channel that never appears there is one the real game does not use in the
  section you played.
]]--

-- GBA sound registers (GBATek).
local REG_SOUND1CNT_H = 0x04000062  -- ch1 duty/length + envelope
local REG_SOUND1CNT_X = 0x04000064  -- ch1 frequency + control
local REG_SOUND2CNT_L = 0x04000068  -- ch2 duty/length + envelope
local REG_SOUND2CNT_H = 0x0400006C  -- ch2 frequency + control
local REG_SOUND3CNT_L = 0x04000070  -- ch3 enable
local REG_SOUND3CNT_H = 0x04000072  -- ch3 length + volume
local REG_SOUND3CNT_X = 0x04000074  -- ch3 frequency + control
local REG_SOUND4CNT_L = 0x04000078  -- ch4 length + envelope
local REG_SOUND4CNT_H = 0x0400007C  -- ch4 noise params + control
local REG_SOUNDCNT_L  = 0x04000080  -- master volume + per-channel panning
local REG_SOUNDCNT_H  = 0x04000082  -- PSG/DMA volume ratios
local REG_SOUNDCNT_X  = 0x04000084  -- master enable

local REPORT_EVERY = 300  -- frames between summaries (~5s at 60fps)

local frames = 0
local windowFrames = 0
local activeInWindow = { 0, 0, 0, 0 }
local everActive = { false, false, false, false }
local names = { "sq1", "sq2", "wave", "noise" }

-- Optional CSV, so a long session can be sent somewhere for analysis rather
-- than read off the console. Silently skipped if the sandbox forbids io.
local csv = nil
do
    local ok, handle = pcall(io.open, "mzm_psg_trace.csv", "w")
    if ok and handle then
        csv = handle
        csv:write("frame,sq1_vol,sq1_freq,sq2_vol,sq2_freq,wave_on,wave_vol,wave_freq,noise_vol,noise_param,panning,master\n")
    end
end

local function channelState()
    local s1 = emu:read16(REG_SOUND1CNT_H)
    local s2 = emu:read16(REG_SOUND2CNT_L)
    local s3ctl = emu:read8(REG_SOUND3CNT_L)
    local s3vol = emu:read16(REG_SOUND3CNT_H)
    local s4 = emu:read16(REG_SOUND4CNT_L)

    -- Envelope registers keep the current level in bits 12-15 of the 16-bit
    -- read (initial volume nibble of NRx2).
    local v1 = (s1 >> 12) & 0xF
    local v2 = (s2 >> 12) & 0xF
    local v4 = (s4 >> 12) & 0xF
    -- The wave channel has no envelope: it is on when NR30 bit 7 is set and
    -- its volume code (bits 13-14 here) is non-zero.
    local waveOn = ((s3ctl & 0x80) ~= 0) and (((s3vol >> 13) & 0x3) ~= 0)

    return v1, v2, waveOn, v4,
           emu:read16(REG_SOUND1CNT_X) & 0x7FF,
           emu:read16(REG_SOUND2CNT_H) & 0x7FF,
           emu:read16(REG_SOUND3CNT_X) & 0x7FF,
           emu:read16(REG_SOUND4CNT_H),
           (s3vol >> 13) & 0x3
end

local function onFrame()
    frames = frames + 1
    windowFrames = windowFrames + 1

    local v1, v2, waveOn, v4, f1, f2, f3, n4, wvol = channelState()
    local master = emu:read8(REG_SOUNDCNT_X)
    local panning = emu:read16(REG_SOUNDCNT_L)

    local on = { v1 > 0, v2 > 0, waveOn, v4 > 0 }
    for i = 1, 4 do
        if on[i] then
            activeInWindow[i] = activeInWindow[i] + 1
            everActive[i] = true
        end
    end

    if csv then
        csv:write(string.format("%d,%d,%d,%d,%d,%d,%d,%d,%d,%04X,%04X,%02X\n",
            frames, v1, f1, v2, f2, waveOn and 1 or 0, wvol, f3, v4, n4,
            panning, master))
    end

    if windowFrames >= REPORT_EVERY then
        local pct = {}
        for i = 1, 4 do
            pct[i] = math.floor(100 * activeInWindow[i] / windowFrames + 0.5)
        end
        local ever = ""
        for i = 1, 4 do
            if everActive[i] then ever = ever .. names[i] .. " " end
        end
        if ever == "" then ever = "(none yet)" end

        console:log(string.format(
            "[%4ds] active now: sq1=%3d%% sq2=%3d%% wave=%3d%% noise=%3d%%   ever: %s",
            math.floor(frames / 60), pct[1], pct[2], pct[3], pct[4], ever))

        -- Master enable off would make every reading above meaningless, so say
        -- so loudly rather than letting it look like "the game uses no PSG".
        if (master & 0x80) == 0 then
            console:log("         NOTE: SOUNDCNT_X master enable is OFF this frame")
        end

        windowFrames = 0
        for i = 1, 4 do activeInWindow[i] = 0 end
        if csv then csv:flush() end
    end
end

callbacks:add("frame", onFrame)

console:log("mzm PSG trace loaded. Play with music for 30s+; a summary prints every ~5s.")
if csv then
    console:log("Also writing mzm_psg_trace.csv next to the ROM.")
else
    console:log("File writing unavailable; console output only.")
end
