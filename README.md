# Metroid: Zero Mission — Nintendo 3DS Port

A native, hardware-accelerated Nintendo 3DS port of **Metroid: Zero Mission**, built from the C decompilation codebase.

---

## Features

- **Dual-Screen Layout**:
  - **Top Screen**: Smooth 60 FPS gameplay with selectable display modes (Pixel Perfect, Scaled, Fullscreen) and **Stereoscopic 3D** support with multi-layer depth parallax (foreground HUD, interactive platforms, Samus, background scenery).
  - **Bottom Screen**: Real-time area map, collectible item breakdown (Energy Tanks, Missiles, Super Missiles, Power Bombs), and a quick touch-activated Morph Ball toggle.
- **Hardware Acceleration**: Custom PICA200 GPU renderer (`citro3d`/`citro2d`) for background tiles and sprites with automatic scanline CPU fallback when special GBA PPU effects are active (affine BG, windows, mosaic).
- **Native Audio**: Powered by the decompiled MZM sound engine and output via the 3DS DSP (NDSP) hardware.
- **Console Optimization**: Supports New 3DS 804 MHz CPU boost and L2 cache, along with frame pacing optimizations for Old 3DS and 2DS consoles.

---

## Installation

1. **Install the App**:
   - Install `mzm-3ds.cia` using FBI or your preferred CIA installer.
   - Alternatively, place `mzm-3ds.3dsx` in `sdmc:/3ds/` and launch it via the Homebrew Launcher.

2. **ROM Placement**:
   - Create the directory:
     ```text
     sdmc:/3ds/Metroid Zero Mission 3DS/
     ```
   - Copy a clean, legally obtained Game Boy Advance ROM of Metroid: Zero Mission into this directory. Any `.gba` filename is supported.
   - **Supported Regions**:
     - **USA** (`BMXE`): `sha1: 5de8536afe1f0078ee6fe1089f890e8c7aa0a6e8`
     - **Europe** (`BMXP`): `sha1: 0fd107445a42e6f3a3e5ce8c865f412583179903`

   *Note: The ROM is loaded dynamically from your SD card and is never packaged inside the executable.*

3. **Audio Setup**:
   - Make sure you have dumped your console's DSP firmware (on Luma3DS: press `L + Down + Select` to open Rosalina Menu -> *Miscellaneous options* -> *Dump DSP firmware*).

---

## Building

### Requirements

- [devkitPro](https://devkitpro.org/) with `devkitARM`, `libctru`, `citro2d`, and `citro3d`.
- `makerom` and `bannertool` (for `.cia` packaging).

### Build Commands

To build the 3DS target:

```sh
cd platform/3ds
make clean && make -j$(nproc)
```

Outputs will be generated in `platform/3ds/`:
- `mzm-3ds.cia`
- `mzm-3ds.3dsx`
- `mzm-3ds.elf`

For advanced build options, renderer selection, and debugging flags, see [platform/3ds/README.md](platform/3ds/README.md).

---

## Credits & Acknowledgments

This port is made possible thanks to the work of multiple open-source projects:

- **[metroidret/mzm](https://github.com/metroidret/mzm)** — The decompilation project of *Metroid: Zero Mission*, providing the reverse-engineered C source code.
- **[EstebanPdN / zelda-tmc-3ds](https://github.com/EstebanPdN/zelda-tmc-3ds)** — The Nintendo 3DS port of *The Minish Cap*, whose platform architecture, 3DS frontend implementation, and Citro2D/Citro3D presentation pipeline served as the initial foundation for this port.
- **[devkitPro](https://devkitpro.org/)** — For providing the homebrew toolchain, `libctru`, `citro2d`, and `citro3d`.
- **VirtuaPPU** by Mathéo Vignaud — Upstream base for the software GBA Picture Processing Unit.

---

## License

This project is licensed under the terms of the GNU General Public License v3.0 (GPL-3.0-or-later). See [LICENSE](LICENSE) for details.

