# port/ppu — vendored software GBA PPU

In-tree software GBA Picture Processing Unit (PPU) for the native port.
**GPL-3.0-or-later** (matches the project license).

## Provenance

Derived from **VirtuaPPU** by Mathéo Vignaud
(<https://github.com/MatheoVignaud/VirtuaPPU>), upstream commit
`5cf5e990d3ecb08ae00d266fea833ccc56286bd5`, plus accuracy and portability
patches.

## What's here

- `src/virtuappu.c` — dispatcher (`virtuappu_render_frame` switches on mode).
- `src/mode1.c` — scanline software renderer: 4 text BGs (GBA mode 0), affine
  BG2 path (GBA modes 1/2), OBJ, windows, blending, and mosaic.
- `include/` — public API (`virtuappu.h`, `ppu_memory.h`) + internal headers.
