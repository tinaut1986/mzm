# Issue #17 investigation session — 2026-08-25

## Update (same day, GDB session — read this first, supersedes the
## sync/race conclusion below)

Set up live GDB debugging over WiFi against the real console's Luma3DS GDB
stub (`arm-none-eabi-gdb`, ELF rebuilt with `-g` added to `EXTRA_CFLAGS` for
line info — this GDB build has **no Python support**, `source foo.py`
literally tries to run `import` as a GDB command, so pure `commands`/`end`
breakpoint scripting is the only option).

**Hard lesson on breakpoint placement**: a conditional breakpoint on
`port_gpu_renderer.c:1815` (`C2D_DrawImage`, called once **per drawn tile**,
so tens-to-hundreds of hits/frame) evaluating `gSamusData.pose == 51` over
the network round-trip on every single hit effectively wedged the console
hard enough that it needed a forced power-off to recover — don't do this
again. Moving the breakpoint to `Port_GpuRenderer_RenderFrame`'s entry
(`port_gpu_renderer.c:1443`, called exactly once/frame) made it survive, but
still cost real perf: gameplay dropped to ~5 FPS while the breakpoint was
armed with `pose==51` as the condition (checked every frame regardless of
pose). Always pick a once-per-frame anchor point for a live conditional
breakpoint on this stub, never a per-item one.

**The actual finding**: even at ~5 FPS (each frame with huge, ~200ms of
slack for any CPU/GPU cache flush to settle), the death sequence **still
rendered corrupted**, same as always. This is the opposite of what the
end-of-previous-session sync/race hypothesis predicts — if the bug were a
tight cache-coherency race that only manifests at full 60 FPS speed, this
kind of drastic, uniform slowdown should have masked it exactly like the
isolated L+R+A replay did. It didn't.

**Revised leading hypothesis**: the difference between "L+R+A replay looks
correct" and "real gameplay (fast OR slowed via GDB) looks broken" is
probably not wall-clock speed at all -- it's that L+R+A does a single fresh
decode of every tile from the fixture, while real gameplay reaches the same
frame via a long chain of **in-place `OBJTILE STALE` redecodes of the same
13 slots** (driven by the death sequence's fast palette cycling during the
`walljumpTimer` flash phase, confirmed present in `mzm-debug.log` analysis
in the original session below). This re-promotes the *original* leading
hypothesis from before the sync/race detour: a bug in `CollectSprite`'s use
of `sSlotSubtexTable`/atlas slot addressing, but narrowed further -- next
session should specifically look for a bug that only manifests after
**repeated redecodes of an already-cached slot**, not on first decode. Good
candidate: whether a draw item can capture/use a slot's UV table entry (or
the slot index itself) computed *before* that slot gets STALE-redecoded
later in the same frame, i.e. an ordering bug between collection and
decode, rather than a hardware cache-visibility bug.

Next concrete step for a future session: with the same GDB setup (mind the
per-frame-only breakpoint rule above), instrument the exact `slot` values
`CollectSprite` uses for Samus's draw items during the flash phase and cross
-reference them frame-by-frame against `GetOrDecodeTileSlot`'s STALE events
for those same slots, watching specifically for a slot being used for a draw
item in the same frame it gets redecoded (ordering/staleness bug) vs. a slot
mismatch across frames (indexing bug).

## Update 2 (same day) -- the ordering/indexing hypothesis above is now ruled out

Added a new `COLLECT` diag log line in `CollectSprite`'s non-affine path
(right after the `GetOrDecodeTileSlot` call, `port_gpu_renderer.c` around
line 1206 -- guarded by the existing `sDiagObjSceneLog`/
`PORT_GPU_RENDERER_DIAG_LOG`), logging `[F<frame>] COLLECT off=... slot=...
drawX=... drawY=...`, i.e. exactly the (byteOffset, slot) pair each draw
item actually uses, frame-tagged the same way `OBJTILE NEW/STALE/HIT`
already are.

Rebuilt (no GDB this time -- plain normal 60 FPS gameplay, FTP'd the CIA in,
played a real death, pulled `sdmc:/3ds/mzm-debug.log` back over FTP
afterward). Isolated the death-scene frame range (F755-F954, ~200 frames)
and cross-checked every `COLLECT` line against the most recent `OBJTILE`
event for that slot:

- **Zero slot collisions** (no slot ever attributed to two different
  `byteOffset`s within the same frame).
- **Zero ordering/staleness mismatches** (every `COLLECT`'s slot always
  matches the byteOffset most recently associated with it via
  `NEW`/`STALE`/`HIT` -- never a leftover/pre-redecode value).

This rules out "Update 1"'s ordering hypothesis with real data, not just
code reading. Also re-verified `sDirtyRowMask`'s row-index math
(`slot / ATLAS_TILES_PER_ROW`, a `uint64_t` bit) is correct and has no
overflow risk for the slot range actually used in this scene (up to ~879,
nowhere near the 4096-slot/64-bit ceiling).

**Where this leaves things**: byteOffset->tileIndex computation, cache
bookkeeping (slot assignment + staleness detection), dirty-row tracking, and
atlas *content* (confirmed correct in isolation last session) have all now
been checked and found correct. The remaining unchecked link in the chain is
the **UV/subtexture table lookup and the actual GPU draw call** using a
slot's `sSlotSubtexTable` entry -- and, now that the "isolated replay looks
fine, real gameplay doesn't" difference can no longer be explained by either
raw wall-clock speed (ruled out in Update 1) or slot bookkeeping (ruled out
here), it's worth also reconsidering whether the difference is something
about the fixture-replay path itself not being fully representative (e.g.
missing some piece of renderer state that only gets initialized/updated
elsewhere per-frame in the normal pipeline).

**Best next step**: get a visual/frame correlation, not another layer of
log-only reasoning -- use L+R+Start (already captures a real screenshot
every 4th sample alongside `mzm-rec.bin`, which also carries a frame
counter) during a death **at the same time** `PORT_GPU_RENDERER_DIAG_LOG` is
active, so the exact frame the screen starts looking wrong can be pinned
down and its `COLLECT` lines checked by hand against where each subtile
should land (same method as the atlas dump session, but now with the
COLLECT log giving drawX/drawY directly instead of hand-deriving them from
OAM).

## Update 3 (same day, continued into 2026-08-26) -- did the visual
## correlation; content and placement both confirmed correct even live

Followed the plan above. Recorded a real death with L+R+Start (screenshots
every 4th sample) with the `COLLECT` diag logging from Update 2 active at
the same time -- normal 60 FPS gameplay, no GDB. Two frame counters needed
reconciling: `mzm-rec.bin`'s header `frameCounter` is the *recorder's own*
counter (`sRecFrameCounter` in `platform_gpu_3ds.c`, starts at 0/1 when
L+R+Start is first pressed), completely different from `gFrameCounter16Bit`
(the game's absolute frame counter since boot) that `[F#]` in
`mzm-debug.log` uses. Aligned them by matching the `pose`/`currentAnimationFrame`
/`walljumpTimer` progression embedded in both (rec.bin's header carries
these directly; the debug log's own scene-start cluster gave the anchor) --
offset was `diagFrame ≈ recFrame + 1924` for this particular capture.

Rotated/decoded the `mzm-rec-shot-NNNN.rgb` screenshots to PNG and zoomed on
the Samus region. Result: **`currentAnimationFrame=10` (sample 16) still
shows a coherent, recognizable arched silhouette (cyan/blue flash colors).
`currentAnimationFrame=14` (sample 20) is already fragmented** -- disconnected
blobs of color, no coherent shape. Every sample from 20 through 44 (covering
the rest of the flash-hold phase, `walljumpTimer` 0 through 7) stays
fragmented -- **once it breaks, it stays broken for the rest of the
sequence**, it doesn't flicker between correct/broken frame to frame. This
matters operationally: a debug combo timed to this scene does NOT need to
catch an exact frame, just "any time after it visibly looks wrong."

Used this to add a new **L+R+B** combo (`platform_3ds_minimal.c`) that calls
the existing `Port_GpuRenderer_DumpAtlas` against whatever's live in the
atlas *during real gameplay*, no fixture needed (unlike L+R+A, which only
ever exercised a single fresh `NEW` decode of a static fixture -- this is
the first time the *in-place* `STALE`-redecoded content has actually been
inspected). Masked like the other real-GBA-button combos (KEY_B is bit 1,
shoot) so it doesn't fire gameplay input while held with L+R.

The user reproduced the break and pressed L+R+B while the screen was
visibly fragmented. Cross-referenced `mzm-live-atlas-keys.csv` for
`palBank=1, hflip=0, vflip=0` entries at the 13 byteOffsets `COLLECT` was
using right at the break (`0x10000`-`0x104E0`) -- 12 of 13 were present with
`palHash=A987BF40`, matching the exact `STALE` redecode hash seen firing in
the log right when the corruption starts (the 13th, `0x10000`, simply wasn't
resident under that exact key at the moment of the dump -- press timing
relative to the log wasn't synced for this capture, not evidence of
anything). Cropped those 12 slots directly out of the dumped atlas PPM:
**every one is a correct, detailed, recognizable fragment of Samus artwork**
(visor, arm cannon, boot, suit colors) -- not garbage, not a wrong tile.

**This closes off content-correctness as a cause, live and in-place, not
just for a cold fixture replay.** Combined with Update 2 (placement/slot
bookkeeping also verified correct live), every step of the pipeline up to
and including "decode the right tile into the right slot with the right
pixels" has now been checked with real data during an actual broken frame
and found correct. What's left unverified is only the *last* step: whether
`sSlotSubtexTable` (the UV rect table used at actual draw-call time) gets
the right rect for the right slot, and/or whether the PICA200 GPU's own
texture sampling ever disagrees with what the CPU-side atlas memory
correctly holds (a hardware-visibility gap different from the "flush timing"
angle already tested in Update 1 -- that was about wall-clock speed, this
would be about whether the sampled data is stale/torn independent of speed).

Next session: instrument `sSlotSubtexTable[slot]` (or just log the raw
`left/top/right/bottom` a draw item ends up submitting to `C2D_DrawImage`)
for these same 13 slots at the same break point, and compare against
`InitSlotSubtexTable`'s formula by hand for those exact slot numbers -- this
is the one remaining un-inspected link in the chain.

## Update 4 (2026-08-26) -- UV/draw-params also confirmed correct; the
## CPU/GPU frame-overlap hypothesis tested and RULED OUT

Did the UV instrumentation from Update 3's next step: added a `[F#] DRAW
slot=... x=... y=... uv=(left,top,right,bottom) params.pos=(...)` diag log
line right before `C2D_DrawImage` in the main draw loop
(`port_gpu_renderer.c`, guarded by `sDiagObjSceneLog && eye == 0` to avoid
double-logging the stereo pass). Reproduced a real death, pulled
`mzm-debug.log`, found the exact frame the `A987BF40` "broken" palette hash
first appears via `grep` (frame-counter-independent marker, more reliable
than trying to re-derive the recorder/debug-log frame-counter offset every
session) and checked every `DRAW` line for the 13 Samus slots at that frame
and the two frames after.

**Everything checked out by hand against `InitSlotSubtexTable`'s formula**
(`left=(sx+0.5)/512`, `top=1-(sy+0.5)/512`, etc., using `sx=(slot%64)*8`,
`sy=(slot/64)*8`) for multiple slots (871, 808, ...) -- exact match, no
off-by-one, no stale/wrong-slot UV rect. Draw positions were a clean,
consistent 8px-step grid (99/107/115/123 x 67/75/83/91), no overlaps, no
duplicates, no garbage coordinates. **This confirms every CPU-observable
step of the pipeline -- OAM decode, cache bookkeeping, pixel content (both
fresh and in-place-redecoded), UV lookup, and final draw position/size --
is correct at the exact frame the screen visibly breaks.**

The user separately confirmed forcing PIXEL PERFECT (1:1, no 1.5x upscale)
still breaks identically, ruling out the "GPU nearest-filter edge case at
non-integer scale" angle without even needing a build (already fairly
unlikely anyway -- `C3D_TexSetFilter(&sAtlasTexture, GPU_NEAREST,
GPU_NEAREST)` plus the deliberate half-texel UV inset in
`InitSlotSubtexTable` should prevent adjacent-slot bleed regardless of
scale).

With every CPU-side link checked, the last remaining candidate was a
genuine CPU/GPU hardware race: `PlatformGpu3DS_BeginTopSceneGpu`
(`platform_gpu_3ds.c`) calls `C3D_FrameBegin(0)` -- flag `0`, NOT
`C3D_FRAME_SYNCDRAW` -- meaning citro3d lets the CPU start recording/
submitting the NEXT frame's GPU commands (and, critically, start the next
frame's CPU-side tile decode, which overwrites `sAtlasTexture.data` **in
place**, no double-buffering) before the GPU hardware has necessarily
finished consuming the PREVIOUS frame's command list, which could still be
sampling that same atlas memory. This would explain every other finding:
why the isolated one-shot L+R+A replay (no frame N+1 racing behind it) looks
correct, why CPU-side logging always sees correct data (it's read before
submission, before any race could bite), and why slowing everything down via
GDB breakpoints didn't help (a missing synchronization primitive doesn't get
better with more wall-clock time if nothing actually waits on it -- unlike a
narrow timing-margin race, slowing the whole system down doesn't change
whether a `wait` is present or absent).

**Tested and REFUTED**: temporarily changed that one call site to
`C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` (forces the CPU to block until the GPU
finishes the previous frame before proceeding). Real hardware result:
**death sequence still broken, identically** -- plus a game-wide FPS drop
(confirming the flag really was taking effect, not a no-op). Reverted back
to `C3D_FrameBegin(0)` immediately afterward (no reason to pay that
perf cost for a fix that didn't fix anything). **This rules out a plain
CPU/GPU frame-overlap race on the atlas texture as the cause** -- if it
were, forcing full synchronization between frames should have eliminated
it, the same way the earlier flush-timing test (Update 1) should have
worked if that had been the real mechanism.

**Where this leaves things, honestly**: every hypothesis tested so far this
session and the previous one -- wall-clock speed, tile-cache ordering,
decode content (fresh and in-place), UV/placement, and now CPU/GPU
frame-overlap synchronization -- has been tested with real data on real
hardware and ruled out. This is a genuinely hard bug. The diagnostic tooling
built across these two sessions (frame-tagged `OBJTILE`/`COLLECT`/`DRAW`
logging, the L+R+A one-shot fixture replay, the L+R+B live atlas dump, the
GDB setup with its once-per-frame-breakpoint lesson learned) is all in git
and reusable, but the next session likely needs one of:
1. **True GPU-side inspection**: what does the PICA200 actually have
   resident in its own texture cache at the moment it executes the OBJ draw
   commands for this scene -- not what CPU-side memory holds (already
   proven correct), but whether the GPU's read of that memory could ever
   disagree with it. Real difficult to observe without a hardware GPU
   debugger Luma3DS/citro3d don't expose.
2. **A completely different mechanism not yet considered**: worth stepping
   back from the tile-decode/placement/sync framing entirely and
   reconsidering the problem from scratch -- e.g., is something ELSE writing
   into `sAtlasTexture.data` for these same byte ranges that hasn't been
   accounted for (a second, unrelated code path also decoding into the
   atlas, a buffer overrun from a neighboring allocation, stack/heap
   corruption elsewhere landing in this linear-alloc'd region)? Nothing
   found points at this specifically, but it hasn't been actively ruled out
   either, unlike everything on the "normal" render pipeline. A heap/memory
   sanitizer pass (if devkitARM's toolchain offers anything close) or a
   canary/guard-byte check around `sAtlasTexture.data` would be a cheap way
   to test this without more log-reading.
3. Revisit whether the CPU renderer (`port/ppu/src/mode1.c`, confirmed
   correct) and the GPU renderer disagree on something ABOUT THE SCENE
   itself beyond what's logged here -- e.g. re-verify `BLDCNT`/window state
   interpretation for this exact `DISPCNT=0x5000` scene is being read
   identically by both renderers, not just assume it from earlier sessions'
   notes.

---


Context dump for a future session picking this back up. Read this before
re-deriving anything below from scratch — a lot of it took real hardware
time and several false starts to nail down.

## TL;DR for a fresh session

- The bug is **confirmed GPU-renderer-only** (RENDERER=cpu is correct,
  RENDERER=gpu is broken, same VRAM/OAM/palette data either way).
- The bug is **reproducible offline in Azahar**, no real hardware or even
  gameplay needed — see "Local repro harness" below. Iterate in seconds,
  not minutes.
- **New key finding this session**: the individual decoded tiles in the GPU
  atlas texture are visually correct (real Samus artwork fragments, right
  colors) when dumped and inspected directly. So `DecodeTileIntoSlot`
  (palette lookup, color packing, swizzle) is very likely NOT the bug.
  The corruption has to be introduced somewhere in **placement/assembly**:
  `CollectSprite`'s per-subtile screen position math, `PushItem`, or the
  atlas UV table (`sSlotSubtexTable` / `InitSlotSubtexTable` in
  `port_gpu_renderer.c`). **This is the next thing to check.**
- A whole rabbit hole this session (uncommitted "channel packing" changes
  from another AI session, a from-scratch VRAM decode script that turned
  out to be buggy) is written up below so the next session doesn't repeat
  it. Both were ruled out — don't re-investigate them without a new reason.

## Repro fixture (ready to use)

`sdmc:/3ds/mzm-fixture-issue17.bin` (101,376 bytes: IO + BG palette + OBJ
palette + OAM + VRAM, same layout as one `mzm-rec.bin` record minus its
32-byte header) is a captured snapshot of the exact broken frame: Samus
`pose=51` (`SPOSE_DYING`), `currentAnimationFrame=23`, `walljumpTimer=0`,
`DISPCNT=0x5000` (OBJ+WIN1 only, no BG layers), `BLDCNT=0x3E41`. Captured
from a real L+R+Start recording (sample 32 of that session's `mzm-rec.bin`)
during a **debug-forced** death (see the L+R+SELECT combo below) — not a
real enemy kill, but the game code path is identical (see
`PortPpuMzm_DebugKillSamus`'s doc comment).

A copy of this exact fixture should also exist at
`platform/3ds/mzm-fixture-issue17.bin` -- if not, re-extract it from a fresh
`mzm-rec.bin` sample at the same pose/frame (see docs/3ds-debug-tools.md's
recorder section) and re-copy it to both the real SD card and
`%APPDATA%\Azahar\sdmc\3ds\mzm-fixture-issue17.bin`.

## Local repro harness (no console, no gameplay needed)

Three new debug tools were added this session, all documented in
`docs/3ds-debug-tools.md`:

1. **L+R+SELECT** — debug instant-kill (`PortPpuMzm_DebugKillSamus`,
   `port_ppu_mzm.c`). Zeroes `gEquipment.currentEnergy` and calls
   `SamusSetPose(SPOSE_HURT_REQUEST)` — the exact same real code path
   lethal damage uses. Lets you reach the death sequence in one button
   press instead of playing to get killed.
2. **L+R+Start** (existing, extended) — scene recorder now ALSO takes a
   real screenshot every 4th sample (`mzm-rec-shot-NNNN.rgb`, matches the
   sample index in `mzm-rec.bin`), so you can pair "what the emulated GBA
   state says" against "what was actually on screen" at the same instant.
3. **L+R+A** — the actual repro harness
   (`PlatformGpu3DS_ReplayFixture`, `platform_gpu_3ds.c`). Loads
   `sdmc:/3ds/mzm-fixture-issue17.bin` straight into the live
   `gIoMem`/`gBgPltt`/`gObjPltt`/`gOamMem`/`gVram` globals and renders +
   dumps ONE frame with the real GPU renderer:
   - `sdmc:/3ds/mzm-fixture-render.rgb` — the actual GPU-rendered output
     (240x400 portrait raw RGB8, same format as the L+R+X dumps — rotate
     90° to view, see docs/3ds-debug-tools.md's Python snippet).
   - `sdmc:/3ds/mzm-fixture-atlas.ppm` — the ENTIRE atlas texture, dumped
     verbatim from `sAtlasTexture.data` (already CPU-visible memory), as a
     512x512 PPM. Directly viewable with any image tool.
   - `sdmc:/3ds/mzm-fixture-atlas-keys.csv` — one row per populated atlas
     slot: `slot,byteOffset,bpp8,palBank,hflip,vflip,isObj,brightAdjust,palHash,evy`.
     Cross-reference a slot number against the PPM: `col = slot %
     ATLAS_TILES_PER_ROW(64)`, `row = slot / 64`, pixel region is
     `(col*8, row*8)` to `(col*8+8, row*8+8)`.
   - New: `Port_GpuRenderer_DumpAtlas(ppmPath, csvPath)` in
     `port_gpu_renderer.c`/`.h` — the function backing the atlas dump,
     reusable from anywhere.

   **Important gotcha**: the atlas cache PERSISTS across frames (by
   design, see the big comment on `sHashBucketHead`). If you trigger L+R+A
   right after booting (menu screen, lots of BG tiles cached), the CSV will
   be dominated by leftover BG-tile slots from the menu -- filter the CSV
   for `isObj=1` and `byteOffset` in the `0x10000+` range to find the 13
   OBJ slots actually used by this fixture's 3 sprites (tile bases
   0/4/8 in 4bpp OBJ VRAM, palette bank 1).

### Running this without a real 3DS (Azahar, this session's setup)

- Azahar is installed at `C:\Program Files\Azahar\azahar.exe` on this
  Windows machine (the user's own PC, not a sandboxed environment).
- SD card root: `%APPDATA%\Azahar\sdmc\` (i.e.
  `C:\Users\Tinaut1986\AppData\Roaming\Azahar\sdmc\`).
- The ROM is NOT in the repo (gitignored, never needed to build the CIA —
  see "CIA vs ROM" note below). It lives on the user's PC at
  `O:\Consolas y juegos\Juegos\GBA\Metroid\Metroid - zero mission\Metroid - zero mission.gba`
  (Europe BMXP, sha1 `0fd107445a42e6f3a3e5ce8c865f412583179903` — matches
  what `platform/3ds/README.md` expects). Copy it to
  `sdmc:/3ds/Metroid Zero Mission 3DS/` under Azahar's sdmc root once; it
  should still be there for a future session.
- Build: from `platform/3ds/`, with devkitPro at `C:\devkitpro` (NOT the
  `/opt/devkitpro` the env var default claims — that path doesn't exist on
  this Windows box, only `C:\devkitpro`/`/c/devkitpro` does):
  ```powershell
  $env:DEVKITPRO = "C:/devkitpro"
  $env:DEVKITARM = "C:/devkitpro/devkitARM"
  $env:PATH = "C:\devkitpro\devkitARM\bin;C:\devkitpro\tools\bin;$env:PATH"
  $env:TMP = "C:\Users\Tinaut1986\AppData\Local\Temp"   # native gcc needs a real Windows TMP, the Bash tool's env doesn't propagate one
  $env:TEMP = "C:\Users\Tinaut1986\AppData\Local\Temp"
  $env:EXTRA_CFLAGS = "-DPORT_GPU_RENDERER_DIAG_LOG"     # for the OBJTILE/GPUDIAG log lines
  cd D:\Users\Tinaut1986\Source\Repos\mzm\platform\3ds
  make -j4
  ```
  Must run via the **PowerShell tool**, not Bash — Bash (git-bash/MSYS)
  doesn't propagate `TMP`/`TEMP` to the native devkitARM gcc.exe the same
  way, and it fails with "Cannot create temporary file in C:\WINDOWS\".
- Install + launch without the CIA-install confirmation dialog blocking
  everything: `azahar.exe -f <path>` needs a `.cxi`/`.3dsx`/`.app`
  extension to be accepted (a bare `.app` is REJECTED — "Formato de
  aplicación inválida" — despite being the right container format; copy it
  to a `.cxi`-named file first). Full cycle:
  ```powershell
  # 1. Install (still pops a "Installed CIA successfully" dialog you must
  #    click OK on — see gotcha below)
  Start-Process azahar.exe -ArgumentList "-i","<path to mzm-3ds.cia>"
  # 2. Find + dismiss the dialog (see gotcha below), then:
  # 3. Copy the freshly-installed .app to a .cxi-named file
  $appFile = (Get-ChildItem "$env:APPDATA\Azahar\sdmc\Nintendo 3DS\00000000000000000000000000000000\00000000000000000000000000000000\title\00040000\00198600\content" -Filter *.app | Sort LastWriteTime -Descending | Select -First 1).FullName
  Copy-Item $appFile "$env:APPDATA\Azahar\mzm.cxi" -Force
  # 4. Launch directly, no dialog
  Start-Process azahar.exe -ArgumentList "-f","`"$env:APPDATA\Azahar\mzm.cxi`""
  ```
  **Gotcha**: this machine has a 3-monitor setup with the primary/leftmost
  monitor at NEGATIVE virtual-screen coordinates (`VirtualScreen.Location
  = {X=-1920,Y=0}`, total 5760x1080). Azahar's window (and the install
  dialog) can land on any of the 3 monitors run to run — don't hardcode
  screen coordinates for clicking. Use `GetWindowRect` on the actual window
  handle (enumerate windows for the azahar PID with `EnumWindows` +
  `GetWindowThreadProcessId`, since the install dialog isn't the process's
  `MainWindowHandle`) and click its rect's center. The OK button on the
  "Installed CIA successfully" dialog is in the lower-right of that dialog,
  not dead center — a click at the geometric center of the whole dialog
  can miss it (hit blank space instead) and leave it hanging.
- Sending input: Azahar's default keyboard mapping (this profile) is
  `L='Q'`, `R='W'`, `A='A'`, `SELECT='N'` (Qt scan codes in
  `%APPDATA%\Azahar\config\qt-config.ini`, `[Controls]` section,
  `profiles\1\button_*`). Use `keybd_event` (user32.dll) for real key
  down/up events — `SendKeys` doesn't reliably model simultaneous holds.
  Must `SetForegroundWindow`-equivalent (click the window first) before
  sending, or keystrokes go to whatever else has focus.
- `Port_GpuRenderer_RenderFrame()` must NOT be called bare — it hung the
  whole emulated-frame pump the first time this was tried (no crash, no
  further log lines, just silence). It must run wrapped exactly like the
  real per-frame pipeline in `port_ppu_mzm.c`'s `Port_PPU_PresentFrame`:
  `PlatformGpu3DS_SubmitLock_Acquire()` →
  `PlatformGpu3DS_BeginTopSceneGpu()` → `Port_GpuRenderer_RenderFrame()` →
  `PlatformGpu3DS_EndBottom(PlatformGpu3DS_BottomBuffer(0), true)` →
  `PlatformGpu3DS_SubmitLock_Release()`. This is exactly what
  `PlatformGpu3DS_ReplayFixture` now does — if it's ever rewritten, keep
  this wrapping.

### CIA vs ROM (in case this comes up again)

The CIA never needs or embeds the ROM — `platform/3ds/README.md` is
explicit about this ("The ROM is read directly from the SD card and is
never packaged inside the CIA"). Compiling the CIA and running/testing it
in an emulator are two separate concerns with two separate requirements.

## Ruled out this session (don't re-investigate without a new reason)

### 1. Uncommitted "channel packing" WIP from another AI session (Antigravity)

At the START of this session, `platform/3ds/source/port_gpu_renderer.c`
was ALREADY modified in the working tree (visible in `git status` before
any of this session's edits) — leftover from a previous, different AI
session (Antigravity) the user had run and not fully reviewed. That diff
rewrote `Bgr555ToRgba8`/`ApplyBrighten`/`ApplyDarken` (RGBA byte-order
convention) and `ConfigureAtlasTextureEnv` (simplified the GPU TEV setup
from a 3-stage channel-reconstruction hack to a single `GPU_REPLACE`
stage) together as a **self-consistent pair** (same convention used to
encode AND decode).

This was reverted back to HEAD (`git checkout -- port_gpu_renderer.c`)
early in the session out of caution — the 3-stage TEV setup in HEAD has a
doc comment explaining it was a deliberate fix for a real, previously
-confirmed bug ("every transparent pixel drew as an opaque black square").

**Later in the session, with the Azahar harness working, the simplified
version was tested properly (A/B, same fixture) and produced BYTE-IDENTICAL
output to HEAD's version.** This makes sense in hindsight: swapping both the
encode and decode side together for an internal-only texture format (nothing
else reads this texture with a different assumption) is a no-op by
construction, regardless of which convention is used. This whole
"channel packing" angle is a dead end — don't waste time on it again unless
someone finds a reason the two representations WOULDN'T be equivalent.

The original discarded patch is saved at
`.claude-scratch/antigravity-wip-port_gpu_renderer.patch` for reference.
**Do not reapply it** — it's ruled out, kept only for the record and
because it also contains two small unrelated fixes that were never
evaluated (a `runLength==64` shift-overflow guard in the dirty-row flush
loop, and using the real GBA backdrop color instead of hardcoded black for
`C2D_TargetClear`) — worth a quick look in isolation sometime, but neither
is related to issue #17.

### 2. Manual Python VRAM/OAM/palette reconstruction script

Several times this session (and in earlier sessions per the GitHub issue
history), a from-scratch Python script decoding `gVram`/`gOamMem`/
`gObjPltt` by hand (replicating the tile-index formula, hflip, palette
lookup) was used to answer "what should this frame look like, independent
of any renderer". It's how the original "VRAM data itself is corrupted"
hypothesis got traction, and also how the "aligned position, correct
palette-family colors" comparisons earlier this session were made.

**This script's output should no longer be trusted at face value.** Once
the real atlas texture was dumped and inspected directly this session
(ground truth, no hand-written decode involved), the individual tiles
turned out to be correct, detailed Samus artwork — completely different
from what the Python script rendered for the same fixture (which looked
like meaningless vertical gray stripes). The script has an undiscovered
bug, most likely in hflip handling (the one sprite that's genuinely
different about this scene's OAM is the single hflip=1 entry) or the 2D
tile-row stride. If a future session wants a quick "what should this look
like" sanity check again, either fix this script first (compare its
per-tile output against `mzm-fixture-atlas.ppm`'s ground truth, tile by
tile, to find the discrepancy) or just use the atlas dump instead of
reinventing the decode.

## IMPORTANT UPDATE (end of session, real hardware retest) -- read this first

After writing the "leading hypothesis" section below, the user tested the
new CIA (`mzm-3ds-v0.2.2-dev.43+5ce2e11e-atlasdump.cia`, uploaded to the
real console) and reported a pattern that changes the picture:

| | Real death in actual gameplay | L+R+A fixture replay |
| --- | --- | --- |
| **Real 3DS hardware** | Still broken (as always) | Renders CORRECTLY for a split second, then the screen goes black except Samus reverts to whatever she looked like right before L+R+A was pressed |
| **Azahar (emulator)** | Looked correct (user watched a real death play out) | Broken/fragmented (this session's earlier finding) |

This is the OPPOSITE pairing you'd expect from a simple "wrong tile" or
"wrong placement" bug, which should reproduce identically regardless of
where/how the frame is captured. Two things to reconcile:

1. **The L+R+A fixture-replay "revert" is an explained, expected artifact,
   not new information about the bug**: `PlatformGpu3DS_ReplayFixture`
   overwrites `gVram`/`gOamMem`/etc. for exactly one frame, but the game
   logic thread (`agbmain`, Core 1) is still running concurrently and keeps
   writing its OWN real state every emulated frame -- so the forced frame
   renders once, then the live game immediately overwrites it again. The
   "briefly correct, then reverts" behavior is consistent with this, not
   evidence the fixture is wrong. (Whether that flash of correct GPU output
   on real hardware, for the exact fixture that reproduces the bug when
   played live, means anything is still an open question -- see below.)
2. **Real hardware breaks on a real, continuous, live death; Azahar does
   not, for the same build.** This is the important new data point. It
   suggests the bug may be a genuine CPU/GPU **synchronization or cache-
   coherency race**, not a pure logic bug in tile placement/decode:
   - `FlushAtlasRange` (`port_gpu_renderer.c`) uses
     `svcFlushProcessDataCache` -- a LOCAL ARM11 cache flush -- to make the
     CPU's writes into `sAtlasTexture.data` visible to the PICA200 GPU
     before it samples that texture. If this is insufficient in some edge
     case on real silicon (timing, a missing barrier, the GPU starting to
     sample before the flush's effects are actually visible on the bus),
     you'd get exactly this pattern: broken on real hardware, fine on a
     software-emulated GPU (Azahar doesn't model real cache incoherency at
     that level), and fine on a slow, deliberate, isolated one-shot replay
     (plenty of wall-clock time for any real caching effects to settle
     naturally between the write and the eventual draw, unlike 60fps
     continuous real gameplay).
   - This would also explain why the bug is specific to THIS scene and not
     general GPU-rendered gameplay: the death sequence's specific pattern
     (many tiles' palette hash changing every few frames during the
     walljumpTimer flash phase, i.e. frequent in-place redecodes via the
     `OBJTILE STALE` path in `GetOrDecodeTileSlot`) may be the trigger --
     a plain `OBJTILE NEW` (slot allocated once, never rewritten) tile
     wouldn't exercise the write-then-flush-then-sample race the same way
     a same-slot redecode under fast palette cycling would.

**This means the "tile placement/UV mapping" hypothesis in the section
below is now less likely to be the whole story** (a pure placement bug
should reproduce the same way in Azahar as on hardware, and Azahar's
gameplay death looked fine). Re-focus a future session on the atlas
write/flush/draw synchronization instead:
- Read `FlushAtlasRange`'s and `DecodeTileIntoSlot`'s full comments again
  (`port_gpu_renderer.c`) for what's already been tried/reasoned about here.
- Check whether `svcFlushProcessDataCache` is actually sufficient for
  PICA200 texture reads on real hardware, vs. needing `GSPGPU_FlushDataCache`
  (the real GPU-service IPC flush) for texture memory specifically, even
  though the latter was deliberately removed for being slow (~18-30ms/frame,
  see `FlushAtlasRange`'s doc comment) -- there may be a middle ground (e.g.
  only the dirty rows actually touched THIS frame, which is already what
  the dirty-row-range flush does -- so if this hypothesis is right, the
  question becomes why the local flush isn't equivalent to the IPC one for
  this specific access pattern, not just "use the slow one instead").
- Try instrumenting frame-to-frame: does the corruption specifically start
  right after an `OBJTILE STALE` redecode (in-place slot reuse) for a tile
  currently on screen, vs. after an `OBJTILE NEW` (fresh slot)? The
  walljumpTimer flash phase's rapid palette cycling is exactly the
  condition that produces many STALE redecodes in a row for the same 13
  slots -- if the corruption's timing lines up with that, it's a strong
  confirmation.
- **Confirmed by the user right after this note was first written**: the
  L+R+A flash on real hardware showed a NORMAL, correctly-rendered Samus
  (their words: "aspecto correcto... espalda arqueada y traje azul" -- an
  arched-back pose, blue suit, not the garbled/fragmented look). This is a
  strong point in favor of the sync/race hypothesis above: the EXACT SAME
  fixture data (`pose=DYING`, `currentAnimationFrame=23`, the frame that
  reliably shows broken during real continuous gameplay), pushed through
  the EXACT SAME GPU decode/TEV/draw code, rendered CORRECTLY on real
  hardware when done as one deliberate, isolated, slow one-shot call. That
  the only thing different between "broken" and "correct" for identical
  data + identical code is continuous-60fps-real-gameplay vs.
  slow-isolated-single-call is exactly what a CPU/GPU synchronization race
  would produce, and is hard to explain with a pure logic bug (a logic bug
  in tile placement/decode wouldn't care how fast frames are coming).
  **This is the strongest evidence gathered this session** -- a future
  session should treat the sync/race hypothesis as the primary lead, not
  just "worth investigating."

## Why CPU renderer works and GPU renderer doesn't (fits the race hypothesis)

`port/ppu/src/mode1.c` (the CPU/scanline renderer) never writes pixel data
into a texture for the GPU to sample later -- it composites directly into a
CPU-side pixel buffer that gets displayed through a straightforward,
GPU-agnostic path. There is no "CPU writes memory, GPU reads that same
memory shortly after, without the reader being guaranteed to see the
writes" step anywhere in that path -- structurally, it cannot have the kind
of CPU/GPU cache-coherency race described above, regardless of whether such
a race exists. This is consistent with (and predicted by) the sync/race
hypothesis, not just a coincidence -- a plain logic bug in tile decode or
placement would have no reason to respect the CPU/GPU renderer boundary
this cleanly.

## Next session plan: live debugging on real hardware via GDB

Decided at the end of this session, not yet attempted. The goal: pause
execution on the real console at/around the exact moment of corruption,
inspect and poke memory live, and test fixes (e.g. an extra/different
cache flush call) without a recompile-reinstall-replay cycle every time.

1. **Enable Luma3DS's GDB stub** on the console: open Rosalina (hold
   `L+Down+Select` -- the same combo the README already mentions for DSP
   firmware dumping), find the debugging/GDB stub option, enable it (check
   whether it needs to be enabled per-title or globally, and note the
   port/IP it reports).
2. **Connect from this PC** with `arm-none-eabi-gdb`, already present in
   the devkitARM toolchain used to build (`C:\devkitpro\devkitARM\bin\`).
   Point it at the ELF with debug symbols (`platform/3ds/mzm-3ds.elf` --
   confirm it's actually built with `-g`/debug info; check the Makefile's
   `CFLAGS` if breakpoints don't resolve to source lines) and `target
   remote <console-ip>:<stub-port>`.
3. **Good first breakpoint candidates** (`platform/3ds/source/port_gpu_renderer.c`):
   - `FlushAtlasRange` -- see whether it's actually being called for the
     right address range/size right before the corrupted draw, and whether
     adding a real `GSPGPU_FlushDataCache` call (the slower, IPC-based one
     this code deliberately moved away from -- see that function's doc
     comment) at that exact point makes the corruption go away. If it
     does, that CONFIRMS the sync/race hypothesis even if the fix needs to
     be smarter than "always use the slow flush" for performance.
   - `DecodeTileIntoSlot`, specifically the `OBJTILE STALE` in-place-redecode
     path in `GetOrDecodeTileSlot` -- per this session's hypothesis, rapid
     redecoding of the SAME slot (driven by the death sequence's fast
     palette cycling during the walljumpTimer flash phase) is the likely
     trigger condition. Confirm this by breaking there and checking
     redecode frequency/timing right as the scene starts looking wrong.
   - The actual draw call site in `Port_GpuRenderer_RenderFrame` (the
     `C2D_DrawImage`/`C3D_DrawArrays` submission for OBJ items) -- to see
     how much real wall-clock time elapses between the last relevant
     `DecodeTileIntoSlot`/flush and the GPU actually consuming that data,
     and whether that gap is suspiciously small right when corruption
     starts.
4. Cross-reference against **L+R+X** (or the recorder) to know exactly
   which real frame/pose you're breaking on, and the atlas-dump tooling
   (`Port_GpuRenderer_DumpAtlas`, still callable ad hoc from a breakpoint
   or a temporary key combo) to inspect atlas contents at the paused
   instant.
5. If GDB memory writes are usable live: try widening or moving the flush
   call, or forcing a full-texture flush instead of the dirty-row-range one
   (`FlushAtlasRange(sAtlasTexture.data, ATLAS_DIM*ATLAS_DIM*sizeof(u32))`)
   right before the draw, as a quick "does this class of fix help at all"
   probe before writing a real patch.

## Current leading hypothesis (written before the update above -- now
## considered secondary, kept for reference)

Since individual atlas tiles are confirmed correct (real, detailed Samus
artwork, not garbage) but the assembled on-screen sprite is fragmented and
wrong (colorful disconnected pixel clusters, not a coherent silhouette,
roughly in the right screen area but not matching any single frame of real
Samus art), the bug most likely lives in how tiles get **placed on screen**,
not decoded. Candidates in `port_gpu_renderer.c`'s `CollectSprite`:

- The per-subtile screen position math (`drawX`/`drawY` computation, the
  `tx`/`ty` loop) — could be placing tiles in the wrong relative order
  or with wrong spacing for this specific sprite configuration (all 3
  sprites in this scene are `SHAPE_WIDE` size 2 or 0, one is `hflip=1`).
- `sSlotSubtexTable`/`InitSlotSubtexTable` — the UV coordinates used to
  sample each atlas slot. If this ever picks the wrong slot's UV rect for
  a given draw item (an indexing bug, off-by-one, or stale `slot` value
  used after the atlas cache evicted/reused it), you'd see exactly this
  symptom: individually-correct pixel content from a wrong tile drawn in
  the wrong screen position.
- `PushItem`/`AllocDrawItem`/the sort-then-draw pipeline — a bug here could
  reorder or duplicate/skip subtiles.

A good next step: instrument (or re-run with `PORT_GPU_RENDERER_DIAG_LOG`
already on) the exact `drawX`/`drawY`/`slot` triples `CollectSprite`
computes for this fixture's 13 tiles, and manually check each one against
where it SHOULD land on screen (compute by hand from the OAM `x`/`y` +
`tx`/`ty` + tile size, cross-referenced against
`mzm-fixture-atlas-keys.csv`'s byteOffset-to-slot mapping) versus where
`mzm-fixture-render.rgb` actually shows content. The harness (L+R+A) makes
this a 1-button-press, ~5-second turnaround loop now — no need to play the
game or use real hardware for this next phase.

## Files worth knowing about

| Path | What |
| --- | --- |
| `platform/3ds/mzm-fixture-issue17.bin` | Repo copy of the repro fixture (also on the real SD card / Azahar SD, see above) |
| `docs/issue17-assets/mzm-fixture-atlas.ppm` | Snapshot of the atlas dump from this session (512x512 PPM) -- the ground truth that showed individual tiles ARE correct |
| `docs/issue17-assets/mzm-fixture-atlas-keys.csv` | Matching slot-key table for that atlas snapshot |
| `docs/issue17-assets/mzm-fixture-render.rgb` | Matching GPU-rendered output (240x400 portrait raw RGB8, rotate 90°) for the same fixture/atlas snapshot |
| `docs/issue17-assets/atlas_slots_66_78.png` | Pre-cropped, 6x-scaled view of the 13 OBJ slots (see the CSV filtered on `isObj=1`) -- this is the image that showed real Samus artwork fragments, not garbage |
| `sdmc:/3ds/mzm-fixture-issue17.bin` | The repro fixture (see above) |
| `sdmc:/3ds/mzm-fixture-render.rgb` | L+R+A's actual GPU-rendered output |
| `sdmc:/3ds/mzm-fixture-atlas.ppm` | L+R+A's full atlas texture dump |
| `sdmc:/3ds/mzm-fixture-atlas-keys.csv` | L+R+A's populated-slot key table |
| `.claude-scratch/antigravity-wip-port_gpu_renderer.patch` | Discarded WIP, ruled out (see above), kept for the record |
| `docs/3ds-debug-tools.md` | All L+R+X/Y/START/SELECT/A combos, `mzm-rec.bin` format |
| `%APPDATA%\Azahar\run_test.ps1` | Half-working automation script for the install→launch→click→keypress cycle -- had monitor-coordinate bugs, ended up doing the cycle manually instead; fix or replace before relying on it |

## Debug tools added this session (all in git, not scratch)

- `platform/3ds/source/port_ppu_mzm.c`: `PortPpuMzm_DebugKillSamus`
- `platform/3ds/source/platform_gpu_3ds.c`: `PlatformGpu3DS_ReplayFixture`,
  screenshot capture in `PlatformGpu3DS_RecordTick`
- `platform/3ds/source/port_gpu_renderer.c`/`.h`: `Port_GpuRenderer_DumpAtlas`,
  `[F%u]` frame-counter prefix added to the existing `OBJTILE` diag log lines
- `platform/3ds/source/platform_3ds_minimal.c`: L+R+SELECT and L+R+A combo
  wiring (both mask their real GBA button like L+R+START already did)
- `docs/3ds-debug-tools.md`: documents all of the above
