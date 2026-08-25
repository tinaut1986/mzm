# Known Differences vs. Original GBA

This document tracks intentional, known behavioral differences between the
native port (3DS/Linux, under `port/` and `platform/`) and the original
Metroid Zero Mission GBA game. It exists so that a difference in behavior
found during testing can be checked against this list before being treated
as a new bug.

See also `docs/3ds-port-status-2026-08-17.md` for the full development log
these entries are drawn from.

## Audio

- The GBA's PSG hardware (2 square channels, 1 wave channel, 1 noise
  channel) doesn't exist on 3DS/Linux. The original engine only
  software-mixes its Direct Sound voices and otherwise just writes PSG
  registers (`0x04000060`-`0x0400009F`) for real hardware to play. This port
  has no such hardware, so all four PSG voices are software-synthesized
  instead (`port/port_psg_synth.c`/`.h`).
- On 3DS, the real mzm music/mixing engine runs as-is and feeds a genuine
  native audio path: a lock-free ring buffer (`port/port_mzm_audio_glue.c`)
  hands its output to the 3DS's real DSP hardware via NDSP
  (`platform/3ds/source/port_mzm_audio_3ds.c`), replacing the GBA's
  DMA-driven Direct Sound mechanism, which has no equivalent outside a GBA.
  This is not emulated or approximated -- it is real hardware audio output.
- On Linux, by contrast, the whole track-playback engine is stubbed out to
  no-ops (`port/port_audio_stubs.c`, gated by `PORT_NATIVE_AUDIO_STUBS` in
  `platform/linux/Makefile`) since there's no NDSP/DSP hardware to drive
  there; the Linux build is silent by design, not a bug.
- BIOS routines invoked via GBA `swi` calls (`MidiKey2Freq`, `CpuSet`,
  `Div`, `LZ77UnCompVram`/`LZ77UnCompWram`, `Sqrt`, `MultiBoot`) are
  clean-room reimplementations from GBATek documentation in
  `port/port_bios.c`, not decompiled code, since real BIOS `swi` calls
  cannot be trapped the same way outside a GBA.

## Timing / Rendering

- The default 3DS build (`RENDERER ?= gpu` in `platform/3ds/Makefile`)
  offloads mode-0 tile/sprite compositing to the PICA200 GPU
  (`port_gpu_renderer.c`), falling back to the CPU scanline renderer
  (`port/ppu/src/mode1.c`, ported from the Minish Cap 3DS port) per-frame
  for anything the GPU path doesn't support yet (affine BG, blend, windows,
  mosaic, affine OBJ -- see `Port_GpuRenderer_CanRenderFrame`). Building
  with `RENDERER=cpu` forces the plain CPU path unconditionally, e.g. to
  isolate whether a bug is renderer-specific. See
  `docs/3ds-port-gpu-renderer-status-2026-08-20.md` for the GPU renderer's
  development history.
- Either renderer runs at a stable 60 FPS via a fixed-rate loop, not driven
  by real GBA VBlank interrupt timing.
- The 3DS PPU bridge actually used, `platform/3ds/source/port_ppu_mzm.c`, is
  a new, minimal one written for this port: no bottom-screen content, no
  HDMA, no widescreen. The original TMC-derived bridge it's based on
  (`platform/3ds/source/port_ppu_3ds.c`) is kept in the tree unused, as
  reference only -- it is deeply coupled to TMC-specific subsystems and was
  never adapted or wired up.

## Memory

- Builds are forced 32-bit (`-m32` on Linux, native ARM32 on 3DS). Several
  structs (e.g. `SoundEntry` in `include/structs/audio.h`) are overlaid
  directly on raw GBA ROM bytes with pointer-sized fields matching the GBA's
  32-bit width; a 64-bit build would desync every field after the first
  pointer in such a struct against the ROM's actual layout.
- `port/port_gba_mem.c` translates numeric GBA addresses (EWRAM, IWRAM, IO,
  palette, VRAM, OAM, ROM, SRAM) to native pointers, and must disambiguate
  those from host stack/heap pointers that numerically collide with the same
  ranges -- notably the ROM range `0x08000000`+, which on 3DS can overlap
  `malloc()` and stack addresses. Getting this disambiguation wrong is what
  caused the `mzm.sav`-all-zeros corruption bug fixed for issue #14 (a live
  stack address in the ROM numeric range was being re-resolved as ROM).
