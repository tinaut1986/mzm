# The Minish Cap 3DS Platform

This target builds the native dual-screen Nintendo 3DS frontend.

Outside gameplay, the bottom screen shows a procedural gold-framed Triforce;
touching it opens the existing Minish Cap Settings hierarchy.

## Console Installation

Install the universal CIA:

```text
tmc-3ds-v1.2-E1.cia
```

Then create this directory on the SD card:

```text
sdmc:/3ds/The Minish Cap 3DS/
```

Place a legally obtained clean ROM there. Any `.gba` filename is accepted.
The port detects USA and European game codes at runtime and activates the
matching internal data profile before boot. Expected SHA-1 values:

```text
USA:    b4bd50e4131b027c334547b4524e2dbbd4227130
Europe: cff199b36ff173fb6faf152653d1bccf87c26fb7
```

The ROM is read locally from the SD card and is never copied into the CIA.

Audio requires a working 3DS DSP firmware setup. On Luma3DS, use Rosalina's
`Dump DSP firmware` option if homebrew audio is unavailable.

## Display

- Top screen: selectable Wide, Original, and Stretch aspect ratios.
- Display styles: centered one-pixel-per-source-pixel Pixel Perfect, nearest-
  neighbor Scaled and linearly filtered Blur.
- Bottom screen: 320x240 map, dungeon/status information and touch item UI.
- Rendering: PICA200/Citro2D presenter fed by the software GBA PPU.
- Performance profile: selected automatically from the detected console model.
- New 3DS: requests 804 MHz, L2 cache and access to the extra application core.
- New 3DS turbo: hold the C-stick in any direction and select 2x through 5x
  game speed from the Gameplay settings.
- Old 3DS: experimental CPU-renderer and audio optimizations plus adaptive
  presentation skipping target 60 Hz engine timing when visual rendering falls
  behind. Input, touch, audio and lifecycle processing continue on skipped
  presentation ticks; visual FPS may be lower.
- Bottom-screen scheduling: touch state is sampled on every engine tick;
  interactive hitboxes are promoted with the physically visible buffer, and
  unchanged static pages avoid redundant paint/upload work.
- Settings: Minish Cap-themed Screen, Gameplay, Developer, and Randomizer submenus with
  persistent options, a manual memory-dump command, and a live diagnostics
  overlay.
- Randomizer: persistent Project Picori randomizer mode in its own submenu. Mode
  changes require confirmation, clear only the active profile and related
  state, keep the ROM, and restart with isolated normal/randomized saves.
- Show FPS: measured presentation cadence in a compact lower-left top-screen
  box.
- Diagnostics: press `L + R + A` to pause the game, display `DUMP SAVED`, and
  create `dumps/dump-*` with top and bottom physical-framebuffer BMP and raw
  captures, EWRAM, IWRAM, VRAM, palettes, OAM, I/O and game state, frame
  visual and engine cadence, adaptive-skip data, per-core PPU timings, GPU
  work, audio buffer health, save
  persistence state, memory availability, lifecycle state and complete input
  data.
- System lifecycle: HOME, sleep and application close events are handled by the
  regular 3DS applet loop.
- Console: the development boot console remains visible during startup. Once
  gameplay begins, later stdout/stderr logs are detached from the bottom
  framebuffer so they cannot flicker over the map or touch interface.

The CIA metadata uses a stable title ID and requests SD card access for local
ROM and save data.

## Requirements

- devkitPro with devkitARM, libctru, Citro2D and Citro3D
- CMake with the devkitPro Nintendo 3DS toolchain
- `makerom` and `bannertool` for CIA packaging

Run:

```sh
chmod +x platform/3ds/build.sh
platform/3ds/build.sh
```

The universal packages are written under `build-3ds/game/`. No ROM, extracted
asset package or save data is included in either package.

## Building for local development (devkitPro Makefile)

The `build.sh` above wraps the CMake/CIA packaging path used for releases.
For day-to-day dev iteration (compiling, testing a single flag change) use
the plain Makefile directly instead — it is what every debugging session on
this port has actually used:

```sh
cd platform/3ds
make clean && make -j$(nproc)
```

`make clean` matters whenever you change `EXTRA_CFLAGS` between builds: the
Makefile does not track flag changes as a dependency, so stale `.o` files
from a previous flag combination will silently survive an incremental
`make`. Always `clean` before rebuilding with different flags.

### Optional debug flags (`EXTRA_CFLAGS`)

None of these are on by default — the normal build a player runs has none of
this instrumentation compiled in. Pass one or more via `EXTRA_CFLAGS` (space-
separated `-D` list):

```sh
make clean && make EXTRA_CFLAGS="-DPORT_GPU_TILE_RENDERER -DPORT_GPU_RENDERER_DIAG_LOG" -j$(nproc)
```

| Flag | What it does |
| --- | --- |
| `PORT_GPU_TILE_RENDERER` | Enables the experimental native-GPU (PICA200) tile/sprite renderer (`source/port_gpu_renderer.c`), currently in development on `feat/native-gpu-renderer`. Without it the build only has the CPU renderer (`port/ppu/src/mode1.c`), which is the correctness-verified baseline. See `docs/3ds-port-gpu-renderer-status-2026-08-20.md` before touching this code — it documents bugs already found and fixed once. |
| `PORT_GPU_RENDERER_DIAG_LOG` | Requires `PORT_GPU_TILE_RENDERER`. Logs GPU-renderer frame stats (item/cache counts, BG scroll, y-range) every 5 frames, and — since 2026-08-20 — logs *why* a frame was rejected and fell back to the CPU renderer (`GPU_REJECT: <reason>`, one of `forced blank`, `mode != 0`, `WIN0`, `WIN1`, `OBJWIN`, `mosaic BG`, `affine OBJ`), throttled to 1 in 30 rejections. This is the flag to use when chasing "why does gameplay stay on the CPU path" issues. |
| `PORT_AUDIO_DIAG_LOG` | Logs audio pipeline diagnostics. Does real SD-card I/O on the audio path and costs real frame rate — only for audio debugging sessions, not general use. |
| `PORT_PPU_PERF_LOG` | Logs CPU-renderer (`mode1.c`) per-frame timing, used to measure the ~18-20ms/frame cost that motivated writing the GPU renderer in the first place. |
| `PORT_VERBOSE_FRAME_LOG` | Very chatty per-frame CPU-renderer logging (`port_ppu_mzm.c`) — noisy, only for deep single-frame debugging. |

All of the above write to `Port_DebugLog()`, which appends to
`mzm-debug.log` on the SD card root (`sdmc:/mzm-debug.log` on real hardware,
or the Azahar virtual SD path below).

### Testing without hardware (Azahar, local machine)

`tools/run_azahar_test.sh` builds, installs into the Azahar flatpak, and runs
it headlessly — much faster than the FTP-to-real-3DS cycle, but **cannot
send button input** (no `xdotool`/`wtype` installed in this environment), so
it can only observe screens reachable without pressing anything (intro,
title screen). Anything past that (file select once a button is pressed,
real gameplay) needs the real console.

```sh
tools/run_azahar_test.sh <seconds> [--no-build] [--no-audio-dump]
```

- Rebuilds with whatever `EXTRA_CFLAGS` you last built the CIA with unless
  `--no-build` is passed (it just calls `make` in `platform/3ds`, so set
  `EXTRA_CFLAGS` via the Makefile invocation beforehand, then re-run with
  `--no-build` to iterate on the emulator side only).
- Debug log: `~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc/3ds/mzm-debug.log`
  (opened in append mode by the port — the script clears stale logs from
  previous runs before launching).
- Screenshot: `/tmp/azahar_test_screenshot_full.png` (full desktop; crop to
  the Azahar window region) and `/tmp/azahar_test_screenshot.png`.
- Quick way to tell GPU vs CPU render path from a screenshot alone: the
  on-screen "FPS NN" text overlay is only drawn by the CPU path
  (`platform_gpu_3ds.c`'s `DrawTopImageStereo`); the GPU path
  (`Port_GpuRenderer_RenderFrame`) does not draw it.

### Testing on real hardware (FTP)

For anything requiring button input (file select, real gameplay) — install
the built CIA via FTP to the console as usual, play, then pull
`sdmc:/mzm-debug.log` back over FTP to read the diagnostics logged during
that session. This is currently the only way to exercise `PORT_GPU_RENDERER_DIAG_LOG`
during actual gameplay.
