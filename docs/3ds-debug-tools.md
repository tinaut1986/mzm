# 3DS port: on-device debugging tools

Reference for the on-device diagnostic tools in the 3DS port, added while
investigating [#17](https://github.com/tinaut1986/mzm/issues/17) (Samus's
death animation showing the wrong sprite/palette). Kept here so a future
session (or a future bug) doesn't have to rediscover how these work or how
to parse their output from scratch.

All of them live behind one button: bottom screen -> **DEBUG** tab ->
**HERRAMIENTAS DE DEPURACION** / **DEBUG TOOLS**, which opens a list where
each row triggers one tool. A second screen behind the **TELETRANSPORTE** /
**WARP** row holds the room-jump tools.

These were originally L+R+`<button>` chords held during gameplay. Those are
gone: the chord stole L/R/X/Y/START/SELECT from whatever the player had
mapped them to for the frame it fired, was easy to trip by accident, and had
to be memorized. The menu has none of those problems. Nothing else about
what each tool does changed, so the old section names below still name the
combo they used to be reached by, in parentheses.

**None of this exists in a plain build.** The button, the menu and every
action behind it are compiled out entirely -- not just hidden -- unless the
build defines `PORT_DEBUG_TOOLS_ACTIVE` (`source/port_debug_tools.h`).

There is exactly **one** debug build: `make clean && make DEBUG_TOOLS=1`. It
turns on the menu *and* the verbose per-frame `*_DIAG_LOG` tracing
(`PORT_GPU_RENDERER_DIAG_LOG`, `PORT_AUDIO_DIAG_LOG`). The old split between
a "simple" menu-only build and a separate tracing build is gone -- it only
existed because tracing meant unconditional SD writes on hot paths, and it
no longer does (see **Log to SD / buffering** below).

A plain production build (no flags) shows no button on the DEBUG tab and has
no source-level path to trigger any of these.

All dumped files land in `sdmc:/3ds/` (i.e. the SD card's `3ds` folder, next
to the `Metroid Zero Mission 3DS` save folder), fetchable over FTP if the
console is running an FTP server (see `make ftp` / `FTP_HOST`/`FTP_PORT` in
`platform/3ds/README.md`).

## Screen dump (was L+R+X)

Implemented in `PlatformGpu3DS_DumpScreens` (`platform_gpu_3ds.c`). Writes,
all overwritten in place on every press (no per-press filename suffix):

| File | Contents |
| --- | --- |
| `mzm-dump-left.rgb`, `mzm-dump-right.rgb`, `mzm-dump-bottom.rgb` | Headerless raw RGB8, top-to-bottom, **portrait** (unrotated) dimensions -- top screen targets are 240x400, bottom is 240x320. The real GPU-rendered output straight from VRAM (via `C3D_SyncDisplayTransfer`), not a CPU-side source buffer -- this is what's actually on screen, including any renderer bugs. |
| `mzm-dump-vram.bin` | Full emulated GBA VRAM, `gVram[0x18000]`. OBJ tile data starts at byte offset `0x10000` (tile N is at `0x10000 + N*32` for 4bpp, `N*64` for 8bpp). |
| `mzm-dump-io.bin` | Full emulated GBA IO register block, `gIoMem[0x400]`. Standard GBA IO map (GBATEK) applies directly -- e.g. `DISPCNT` at offset 0, `WIN1H`/`WIN1V` at `0x42`/`0x46`, `BLDCNT`/`BLDALPHA`/`BLDY` at `0x50`/`0x52`/`0x54`. |
| `mzm-dump-bgpltt.bin`, `mzm-dump-objpltt.bin` | BG and OBJ palette RAM, 256 entries x u16 (BGR555) each, i.e. 16 banks of 16 colors. |
| `mzm-dump-oam.bin` | OAM, 128 entries x 3x u16 (the 4th u16 per 8-byte slot is padding on real hardware and unused here). Standard GBA OAM attribute layout. |
| `mzm-dump-samus.txt` | Plain text: `pose`, `currentAnimationFrame`, `walljumpTimer`, `suitType`, `suitMiscActivation`, the four per-body-part gfx DMA sizes (`shoulderGfxSize` etc.), `armCannonGfxUpperSize/LowerSize`, `unk_22`. See `PortPpuMzm_DumpSamusState` in `port_ppu_mzm.c`. |
| `mzm-dump-samusdata.bin`, `mzm-dump-samusphysics.bin` | Raw `struct SamusData` / `struct SamusPhysics` (see `include/structs/samus.h`) for anything not already in the `.txt`. |

**Why the split between `platform_gpu_3ds.c` and `port_ppu_mzm.c`:**
`platform_gpu_3ds.c` includes `<3ds.h>`/`<citro2d.h>`, whose `u32` typedef
(`unsigned long`) conflicts with the GBA-port one dragged in transitively by
`structs/samus.h` (`unsigned int`) -- a hard compile error if both end up in
the same translation unit. `port_ppu_mzm.c` already safely includes
`structs/samus.h` (and not `<3ds.h>`), so any code that needs real Samus
struct fields lives there and is called from `platform_gpu_3ds.c` through a
narrow extern (`PortPpuMzm_DumpSamusState`, `PortPpuMzm_GetSamusRecordState`)
instead of via a shared header. Keep new debug-dump code following this
split rather than trying to include both headers in one file.

### Reconstructing the screenshot

The `.rgb` files are portrait and need rotating 90° to view correctly
(confirmed empirically: `ROTATE_90` puts the on-screen "FPS 60" HUD text
upright and readable; `ROTATE_270` mirrors it):

```python
from PIL import Image
with open('mzm-dump-left.rgb', 'rb') as f:
    data = f.read()
img = Image.frombytes('RGB', (240, 400), data).transpose(Image.ROTATE_90)
img.save('left.png')
# bottom target is 240x320 instead of 240x400
```

## Kill Samus (was L+R+SELECT)

Implemented in `PortPpuMzm_DebugKillSamus` (`port_ppu_mzm.c`), triggered from the
debug tools menu (`port_bottom_ui_3ds.c`). Zeroes
`gEquipment.currentEnergy` and calls `SamusSetPose(SPOSE_HURT_REQUEST)` --
the same real entry point lethal damage uses in
`SpriteUtilTakeDamageFromSprite` (`src/sprite_util.c`) -- so Samus starts the
real death sequence (`SPOSE_DYING`) on the next update, exactly as if she'd
been killed by an enemy, without needing to actually get hit down to 0
energy in gameplay first. No-op while already in a hurt/knockback/dying pose
so mashing the row mid-animation doesn't re-trigger it. Added while
iterating on issue #17 to make repeated death-sequence captures (screen dump,
scene recorder) fast to set up.

## Log to SD / buffering (LOG A SD, LOG EN BUFFER)

Two toggles in the tools menu, backed by `port/port_debug_log.c`.

- **LOG A SD** -- OFF by default, *even in a debug build*. While it is off,
  `Port_DebugLog` / `Port_DebugLogBuffered` return immediately and nothing
  is written to `sdmc:/3ds/mzm-debug.log`: no SD I/O, no file growing
  unasked, no frame-rate cost from the `*_DIAG_LOG` paths that are now
  always compiled in. Switch it on for the stretch of play you actually
  want captured; switching it off flushes whatever is still buffered so the
  tail of the capture isn't lost. Turning it on drops a
  `USER MARK: SD logging enabled` line, so the start of each capture is easy
  to find in a log that already has older sessions appended to it.
- **LOG EN BUFFER** -- ON by default. Lines accumulate in a 4KB RAM buffer
  and hit the card when it fills (one `fopen`/`fwrite`/`fclose` for the
  whole buffer). Turn it OFF to make *both* entry points write immediately,
  at ~18-20ms per call -- only worth it when chasing a hang that would
  otherwise swallow whatever is still in RAM.

Note the ordering trap: the log file is appended to, never truncated. Delete
it over FTP before a capture if you want a clean one.

## Log marker (was L+R+Y)

Drops a `"USER MARK: ..."` line with a timestamp into
`sdmc:/3ds/mzm-debug.log` (via `Port_DebugLog`) so a play session can flag
"something happened right here" without describing timing after the fact.
Grep the log for `USER MARK` and read the surrounding lines (needs
`-DPORT_GPU_RENDERER_DIAG_LOG` for the `GPU_REJECT`/`GPUDIAG` lines) to see
what the renderer was doing at that moment.

## Scene recorder (was L+R+START)

Implemented in `PlatformGpu3DS_ToggleRecording` / `PlatformGpu3DS_RecordTick`
(`platform_gpu_3ds.c`), ticked once per emulated GBA frame from
`Port_Bios_Halt` (`port/port_bios.c`). Exists because a single screen dump
only ever catches one frame -- not enough to find the exact moment a
fast-changing scene breaks (e.g. issue #17's death animation, where the
first few frames -- a green palette flash -- turned out to be *correct*,
and the actual corruption happens some number of frames later).

- **First touch**: opens `sdmc:/3ds/mzm-rec.bin` (truncating any previous
  recording) and starts sampling.
- **While active**: every 4 frames (~15 samples/sec at 60 FPS,
  `kRecordEveryNFrames` in `platform_gpu_3ds.c`), appends one fixed-size
  record to the file: VRAM + IO + BG palette + OBJ palette + OAM (the same
  data as the screen dump, minus the RGB screenshots -- see below for why)
  plus a small header. Capped at `kRecordMaxSamples` (450, ~30s) so
  forgetting to stop it doesn't fill the SD card; recording
  auto-stops at the cap.
- **Touching the row again**: stops sampling and closes the file.
- A small red "● REC" indicator blinks on the bottom screen while active
  (`Port_BottomUI_Render` in `port_bottom_ui_3ds.c`), independent of which
  tab (map/status/debug/options) is currently open.

**Why no screenshots in every sample:** each RGB screenshot is ~800KB
combined (left+right+bottom) and needs a GPU display-transfer sync
(`C3D_SyncDisplayTransfer`). At 15 samples/sec that's either a frame-pump
stall or an unreasonable amount of buffering. The VRAM/OAM/palette state
alone is enough to reconstruct the visual frame-by-frame offline -- already
proven doing exactly that by hand for the screen dumps during the #17
investigation (see the Python snippet above, extended to decode OAM entries
and tiles; ask for the exact script if starting a fresh session on this).

**Companion screenshots, every `kRecordScreenshotEverySamples`-th sample
(default 4, i.e. ~4/sec):** a real left-eye screenshot (same mechanism as
`PlatformGpu3DS_DumpScreens`' single shot) is written to its own file,
`sdmc:/3ds/mzm-rec-shot-NNNN.rgb` (headerless raw RGB8, 240x400 portrait,
same rotate-90 handling as the screen dump `.rgb` files), where `NNNN` is the
0-indexed sample number -- i.e. it lines up with the Nth record when
splitting `mzm-rec.bin` per the snippet below. This is the only way to tell
apart "the emulated state was already wrong at this sample" (visible in the
VRAM/OAM/palette reconstruction alone) from "the state was correct but the
GPU renderer drew it wrong" (only provable by diffing the reconstruction
against what was actually on screen at that same sample) -- needed after a
2026-08-25 #17 session where a *different*, unsynced capture (a screen dump
whose files turned out to be a truncated/mismatched mix from separate
button presses) briefly looked like it showed corrupted source data, when
the actual recorder-based reconstruction across the whole death sequence
(done earlier in the investigation) showed every sample's VRAM/OAM/palette
reconstructing to a correct Samus silhouette while the screen itself showed
the scrambled sprite -- i.e. the bug is in the GPU renderer, not the game's
graphics loading. The per-sample screenshots exist to keep confirming that
without relying on eyeballing real hardware during a 3-4 second death
animation.

### `mzm-rec.bin` format

A sequence of fixed-size records, one per sample, back to back (no
end-of-file marker -- just read records until EOF). Each record is:

```
struct RecordHeader {   // 64 bytes total (was 32 with magic 'MZMR'; bumped to
                        // 'MZM2' for issue #20's perf instrumentation)
    uint32_t magic;                 // 'MZM2' = 0x324D5A4D (read as little-endian bytes)
    uint32_t frameCounter;          // running emulated-frame counter at capture time
    uint32_t pose;                  // gSamusData.pose (SamusPose enum, see constants/samus.h)
    uint32_t currentAnimationFrame; // gSamusData.currentAnimationFrame
    uint32_t walljumpTimer;         // gSamusData.walljumpTimer
    uint32_t suitType;              // gEquipment.suitType (SuitType enum)
    uint32_t suitMiscActivation;    // gEquipment.suitMiscActivation (SuitMiscFlags bitmask)
    uint32_t unk_22;                // gSamusPhysics.unk_22 (arm cannon OAM draw-order flags)
    // -- perf extension (issue #20) --
    uint32_t lastFrameUs;           // wall-clock duration of the emulated frame that
                                    // just ended, in microseconds (~16675 = on budget)
    uint32_t drawingTimeX100;       // citro3d C3D_GetDrawingTime(), hundredths of ms
    uint32_t processingTimeX100;    // citro3d C3D_GetProcessingTime(), hundredths of ms
    uint32_t spriteCount;           // non-disabled OAM entries this frame (of 128)
    uint32_t affineSpriteCount;     // of those, how many use rotation/scaling matrices
    uint32_t reserved[3];           // zero
};
// followed immediately by, back to back:
uint8_t  io[0x400];       // gIoMem
uint16_t bgPltt[256];     // gBgPltt
uint16_t objPltt[256];    // gObjPltt
uint16_t oam[0x200];      // gOamMem
uint8_t  vram[0x18000];   // gVram
```

Record size = 64 + 0x400 + 512 + 512 + 0x400 + 0x18000 = 101,436 bytes
(`sizeof(header) + sizeof(gIoMem) + sizeof(gBgPltt) + sizeof(gObjPltt) +
sizeof(gOamMem) + sizeof(gVram)` -- if any of those extern arrays change
size, recompute this and update `PlatformGpu3DS_RecordTick` and this doc
together). To split a fetched `mzm-rec.bin` into per-sample dumps for
analysis:

```python
REC_SIZE = 64 + 0x400 + 512 + 512 + 0x400 + 0x18000  # 'MZM2' format (issue #20)
with open('mzm-rec.bin', 'rb') as f:
    data = f.read()
n = len(data) // REC_SIZE
print(f"{n} samples")
for i in range(n):
    rec = data[i*REC_SIZE:(i+1)*REC_SIZE]
    (magic, frame, pose, caf, wjt, suit, suitMisc, unk22,
     frameUs, drawX100, procX100, sprites, affine) = struct.unpack_from('<13I', rec, 0)
    io = rec[64:64+0x400]
    bgpltt = rec[64+0x400:64+0x400+512]
    objpltt = rec[64+0x400+512:64+0x400+1024]
    oam = rec[64+0x400+1024:64+0x400+1024+0x400]
    vram = rec[64+0x400+1024+0x400:]
    if frameUs > 17000:
        print(f"sample {i}: HITCH frame={frameUs}us sprites={sprites} affine={affine}")
    # ... decode DISPCNT/OAM/tiles/palette the same way as the screen dump
```

Cross-reference `pose`/`currentAnimationFrame` against the `SamusPose` enum
(`include/constants/samus.h`) and the relevant `SamusXxxGfx`/`SamusXxx`
function in `src/samus.c` to know what the game logic *should* be doing at
that exact sample, then compare against what the reconstructed VRAM/OAM/
palette actually show.

## Warp / teleport (new -- no combo equivalent)

Behind the **TELETRANSPORTE** / **WARP** row of the tools menu. Exists so a
rendering bug that only reproduces in one specific room doesn't cost a full
replay to that room after every new CIA install.

Jumps are addressed by **(area, door id)**, never by a room number plus
coordinates. `RoomReset` (`src/room.c`) derives everything from
`gLastDoorUsed`: `gCurrentRoom = pDoor->sourceRoom`, and Samus's position
from the door's `xStart`/`yEnd`/`xExit`/`yExit` -- exactly what a real door
transition does, so Samus always lands somewhere legal. Setting a room
number plus a guessed x/y instead (what the `PORT_LINUX_DIAG_WARP_TO_DEOREM`
block in `src/agbmain.c` does, with coordinates hand-tuned for one room)
drops her wherever those coordinates happen to land in the new room's
geometry.

| Row | What it does |
| --- | --- |
| `AREA  < name >` | Steps the target area. Resets the door index if the current one is out of range for the new area. |
| `PUERTA < n / max >` | Steps the target door. The next row shows which room that door leads into, so a room can be found by stepping doors without knowing any door ids up front. |
| `IR A ESA PUERTA` | Warps to the selected (area, door). |
| `GUARDAR PUNTO AQUI` | Records the door Samus last came through (`gCurrentArea` + `gLastDoorUsed`), i.e. "bring me back to this room". Persisted to `sdmc:/3ds/mzm-warp-point.txt`, so it survives a reflash. |
| `IR AL PUNTO GUARDADO` | Warps to that saved point. |

The two lines under the rows show the saved point and where Samus is right
now (`AHORA: <area> SALA <n>`), so the spinner has something to aim at.

**Where the jump actually happens:** the menu only raises a pending flag.
`PortPpuMzm_DebugApplyPendingWarp` (`port_ppu_mzm.c`) is called from
`src/agbmain.c` at the top of its main loop, and only fires while really in
gameplay (`GM_INGAME` / `SUB_GAME_MODE_PLAYING`). It cannot run from the
touch handler itself: that runs inside `Port_Bios_Halt`, which
`src/transfer.c` also calls mid-frame, so resetting `gSubGameMode1` from
there could land in the middle of a room's own update. A request raised
from a pause screen stays pending and fires as soon as play resumes.

## Known findings so far (#17)

- The green/cyan palette flash right after death starts is **correct** --
  it comes from `sSamusPal_Generic_Dying`/`sSamusPal_PowerSuit_Dying`
  (`src/data/samus/samus_palette_data.c`), consumed by `SamusUpdatePalette`
  (`src/samus.c`), which is a byte-matching decompiled function (verified
  against the retail ROM) -- not a decomp bug.
- The captured frame that showed this had `DISPCNT=0x5000` (only `WIN1`+
  `OBJ` enabled, no BG layers at all) with `WIN1H=0xF0`/`WIN1V=0xA0` --
  i.e. WIN1 covering the *entire* screen, not clipping anything (just
  gating BLDCNT per layer). So for that specific frame, the GPU renderer's
  window-clip/scissor path (`port_gpu_renderer.c`) never actually engages --
  ruled out as the cause for *that* frame, though it may still be relevant
  for other frames later in the sequence (not yet captured as of this
  writing).
- `gSuitFlashEffect` (`src/in_game_cutscene.c`, `src/transparency.c`) is
  **not** used anywhere in `src/samus.c` -- it's a separate cutscene-only
  mechanism (e.g. getting the Fully Powered Suit), not part of the death
  sequence. Don't assume it's involved without checking again.
- Still unconfirmed: the actual moment the sprite becomes "scrambled/
  unrecognizable" (per the reporter's description of real hardware
  behavior: crouch -> green flashes -> stretch back -> suit disappears to
  reveal Zero Suit, background fades black-to-white in parallel). The single screen dump only ever caught the early, correct-looking flash frames --
  this is exactly why the scene recorder was built. Next step when
  resuming this investigation: get an `mzm-rec.bin` covering the whole
  death sequence, decode every sample, and find where the reconstructed
  image first looks wrong.
