# Metroid Zero Mission 3DS Platform

This target builds the native dual-screen Nintendo 3DS port of **Metroid: Zero Mission**.

## Features

- **Top Screen**: Selectable aspect ratios (Pixel Perfect, Scaled, Fullscreen), solid 60 FPS gameplay, and **Stereoscopic 3D** with multi-layer depth mapping (HUD overlay pop-out, foreground platforms, characters, mid and far background layers).
- **Rendering Engine**: Hybrid renderer utilizing hardware GPU acceleration via PICA200/Citro3D/Citro2D with an automatic scanline CPU fallback when advanced GBA PPU special effects are active (affine backgrounds, windows, mosaic).
- **Bottom Screen**: Real-time interactive area map, collectible item breakdown (Energy Tanks, Missiles, Super Missiles, Power Bombs), and a quick touch-activated Morph Ball toggle.
- **Audio**: Native sound output driven by the decompiled MZM sound engine and output through 3DS DSP (NDSP) hardware.
- **Hardware Optimization**: Dynamic clock boost and L2 cache on New 3DS (804 MHz) plus optimized execution and adaptive frame pacing for Old 3DS / 2DS systems.

---

## Console Installation

1. Download the CIA from the [Releases page](https://github.com/tinaut1986/mzm/releases)
   and install it using FBI or your preferred CIA installer:
   ```text
   mzm-3ds.cia
   ```
   *(Or launch `mzm-3ds.3dsx` through the Homebrew Launcher).*

   Releases named **Beta** are pre-releases cut from a branch that is still
   being worked on — pick a plain **Release** for a stable build.

2. Create the following folder on your SD card:
   ```text
   sdmc:/3ds/Metroid Zero Mission 3DS/
   ```

3. Place a legally obtained clean Game Boy Advance ROM of Metroid: Zero Mission in that directory. Any `.gba` filename is accepted.
   Supported versions:
   - **USA** (`BMXE`): `sha1: 5de8536afe1f0078ee6fe1089f890e8c7aa0a6e8`
   - **Europe** (`BMXP`): `sha1: 0fd107445a42e6f3a3e5ce8c865f412583179903`

   The ROM is read directly from the SD card and is never packaged inside the CIA.

4. **Audio**: Ensure DSP firmware is dumped on your console (on Luma3DS, press `L + Down + Select` to open Rosalina Menu -> *Miscellaneous options* -> *Dump DSP firmware*).

---

## Building for Development (devkitPro)

### Prerequisites

- [devkitPro](https://devkitpro.org/) with `devkitARM`, `libctru`, `citro2d`, and `citro3d` installed.
- `makerom` and `bannertool` (optional, required for `.cia` generation).

### Compilation

#### Quick Compilation & Interactive Assistant (Linux & Windows)

You can use the helper scripts from the repository root:

- **Interactive Menu**: Run without arguments to launch the arrow-key terminal assistant:
  ```sh
  ./build_3ds.sh        # Linux / macOS
  build_3ds.bat         # Windows
  ```
- **CLI Options**:
  ```sh
  # Debug / Test mode (DEBUG_TOOLS=1)
  ./build_3ds.sh --mode test

  # Production / Release mode (DEBUG_TOOLS=0)
  ./build_3ds.sh --mode prod

  # Build & FTP upload directly to 3DS (defaults to debug mode unless specified)
  ./build_3ds.sh --ftp 192.168.1.50

  # Custom FTP port and production mode
  ./build_3ds.sh --mode prod --ftp 192.168.1.50:5000

  # Incremental build (skip make clean)
  ./build_3ds.sh --no-clean
  ```

#### Manual Compilation with Make

From the repository root:

```sh
cd platform/3ds
make clean && make -j$(nproc)
```

This produces `mzm-3ds.cia` (if `makerom` is available), `mzm-3ds.3dsx`, and `mzm-3ds.elf`.

### Build Flags & Useful Targets

You can customize the build using Makefile variables and `EXTRA_CFLAGS`:

| Option / Flag | Description |
| --- | --- |
| `RENDERER=gpu` *(default)* | Offloads Mode 0 tile and sprite rendering to the 3DS PICA200 GPU (`source/port_gpu_renderer.c`), falling back to CPU scanlines only when needed. |
| `RENDERER=cpu` | Forces pure CPU scanline rendering (`port/ppu/src/mode1.c`) unconditionally for all frames. Useful for baseline testing and comparison. |
| `FORCE_OLD3DS=1` | Runs on New 3DS hardware using Old 3DS clock rates, disabling L2 cache and Core 1 worker threads to benchmark Old 3DS / 2DS performance. |
| `DEBUG_TOOLS=1` | **The** debug build. Compiles in the bottom screen's DEBUG -> HERRAMIENTAS menu and every tool behind it (instant-kill, scene recorder, live atlas dump, one-shot screen/state dump, room warp, equipment -- see `docs/3ds-debug-tools.md`) *and* the verbose per-frame `PORT_GPU_RENDERER_DIAG_LOG` / `PORT_AUDIO_DIAG_LOG` tracing. Source-level gate, not just runtime-disabled: a plain build has no code path to trigger any of them, not even by accident. Nothing is written to `sdmc:/3ds/mzm-debug.log` until the menu's LOG A SD toggle is switched on, which is why the old "simple vs tracing" build split is gone. |

#### Deployment & Utility Targets:
- **FTP Upload**: Deploy directly to a console running FBI or an FTP server:
  ```sh
  FTP_HOST=192.168.1.xxx FTP_PORT=5000 make ftp
  ```
  *(Automatically names the CIA with the current version tag/commit hash, e.g. `mzm-3ds-v0.2.0-dev.N+hash.cia`).*
- **Print Version**: Display the dynamically computed version string:
  ```sh
  make print-version
  ```

---

## Testing & Diagnostics

- **Local Testing (Azahar emulator)**:
  ```sh
  tools/run_azahar_test.sh 15
  ```
- **Real Hardware Diagnostics** (full details, file formats, and guide in [`docs/3ds-debug-tools.md`](../../docs/3ds-debug-tools.md)):
  When built with `DEBUG_TOOLS=1`, accessible from the bottom-screen **DEBUG** tab -> **HERRAMIENTAS / DEBUG TOOLS** menu:
  - **DUMP SCREEN**: One-shot dump of framebuffers, GBA VRAM/OAM/palettes/IO registers, and Samus's pose/animation state to `sdmc:/3ds/`.
  - **LOG MARK**: Writes a timestamped marker in `sdmc:/3ds/mzm-debug.log`.
  - **SCENE RECORDER**: Toggles sampling emulated GBA state every few frames to `sdmc:/3ds/mzm-rec.bin` (with blinking "REC" indicator) until toggled off.
  - **WARP & EQUIPMENT**: Room warp, save/restore position, map unlock, and equipment toggles.
