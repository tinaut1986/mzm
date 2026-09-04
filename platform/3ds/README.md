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

1. Install the CIA package using FBI or your preferred CIA installer:
   ```text
   mzm-3ds.cia
   ```
   *(Or launch `mzm-3ds.3dsx` through the Homebrew Launcher).*

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

---

## Release Process

Version numbers are meaningful, not sequential: a **minor** bump marks a
milestone (v0.4.0 followed v0.3.4 because the port became playable start to
finish), a **patch** is an ordinary fix round. The next release branch is
`release/vX.Y.(Z+1)` unless a milestone is landing.

`main` means **stable**. A `release/*` branch is where work accumulates, and it
stays off `main` for as long as there are known things left to resolve — being
on `release/*` is itself the statement "not stable yet".

### Two ways to publish

Both are supported, and the CI picks the channel on its own (see below). Which
one you use depends on whether the branch is ready to be called stable.

**Beta** — tag the `release/*` branch without merging. Publishes a
pre-release; work continues on the same branch afterwards.

```sh
git tag -a v0.4.4 -m "v0.4.4"
git push origin release/v0.4.4
git push origin v0.4.4          # -> GitHub pre-release, "Beta v0.4.4"
```

**Stable** — merge into `main` first, then tag the merge commit.

```sh
git checkout main
git merge --no-ff release/v0.4.4
git tag -a v0.4.4 -m "v0.4.4"
git push origin main            # main FIRST -- see the note below
git push origin v0.4.4          # -> GitHub release, "Release v0.4.4"
```

A tag that already shipped as a beta is promoted in place: once its commit is
reachable from `main`, re-run **Build 3DS Release** from the Actions tab
(`workflow_dispatch`) and the same release page flips to stable.

### How the CI decides

`.github/workflows/build-release.yml` runs on any `v*` tag push, and on manual
dispatch. It marks the GitHub Release as a pre-release when **`main` cannot
reach the built commit** — not by whether a tag was pushed. So:

| Built from | Channel |
|---|---|
| Tag on an unmerged `release/*` branch | `Beta` (pre-release) |
| Tag on `main`, or on a commit merged into `main` | `Release` |
| Manual dispatch on any branch | `Beta` (pre-release) |

**Push `main` before the tag.** The tag build resolves the channel against
`origin/main`, so a tag that arrives first cannot see the merge and publishes
as a beta. (Recoverable — re-dispatch the workflow — but avoidable.)

Pre-releases keep the `Beta` prefix in the release name, carry a banner in the
release notes, and do not become the repository's "Latest release".

### After tagging

Rename the release branch to the next patch version and delete the old remote
branch — the tag is what identifies that line from then on:

```sh
git branch -m release/v0.4.4 release/v0.4.5
git branch --unset-upstream                     # tracking still points at the old name
git push -u origin release/v0.4.5
git push origin --delete release/v0.4.4
```

The version string baked into the build (`make print-version`, and the CIA
filename) is derived from git: an exact tag on `HEAD` gives `vX.Y.Z`, anything
else on a release branch gives `vX.Y.Z-dev.<commits>+<hash>`.
