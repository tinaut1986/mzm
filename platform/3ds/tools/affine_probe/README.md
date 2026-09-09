# affine_probe

A standalone `.3dsx` for iterating on the affine-OBJ subtile geometry
(`platform/3ds/source/port_affine_subtile.h`) **without the game and without
the scene recorder**.

## Why

The renderer draws an affine sprite as one C2D quad per 8x8 subtile. Snapping
each subtile's origin to a device pixel independently lets neighbouring
subtiles round apart by up to a pixel, which shows through as a transparent
**seam** (nearest sampling, no atlas apron). `port_affine_subtile.h` closes
those seams by growing each quad on its *interior* edges only. Getting that
right by rebuilding the whole port, launching it, reaching the Tourian
escape, and recording the scene is slow -- and right now the in-game recorder
stalls to 0-1 FPS on that CPU-rendered scene anyway.

This probe renders a 64x64 test sprite through the **same header the renderer
uses** (`BuildDrawParams` in `port_gpu_renderer.c` calls
`PortAffine_SubtileQuad`), at 60 fps, with live rotate/scale and the bleed
parameters exposed. What you see here is what the renderer does.

## Build

```sh
make -C platform/3ds/tools/affine_probe
```

Needs `DEVKITPRO` / `DEVKITARM` set (same as the main port build). Produces
`affine_probe.3dsx` -- run it with the homebrew launcher, `3dslink`, or load
it in Azahar. Flat non-recursive Makefile on purpose; the devkitPro
`3ds_rules` template's recursive `$(MAKE)` breaks under Windows git-bash +
Cygwin-make.

## Controls

| Input | Effect |
|---|---|
| Circle pad X / Y | rotate / scale |
| D-pad L/R, U/D | nudge angle 1 deg, scale 0.05 |
| L / R | bleed px: 0, 0.5, **1**, 1.5, 2 |
| A | mode: `SUBTILES + BLEED` -> `SUBTILES, NO BLEED` -> `ONE QUAD (ref)` |
| Y | subtile-grid overlay |
| X | reset angle/scale |
| START | exit |

`NO BLEED` reproduces the raw seams. `ONE QUAD` is the reference "no internal
edges" look. The grid outlines each subtile's *un-grown* rect, so overlap and
gaps are visible against it.

## Test sprite

Procedural (`BuildSprite`): a filled disc with four solid quadrants, a 3px
dark outline ring and a 1px white centre cross. Solid fill makes any subtile
gap read as a cut line; the outline ring makes silhouette-fattening obvious.
Swap in real sprite data by replacing `sStaging[]`'s fill if a specific
in-game sprite needs reproducing (the 64x64 double-size affine OBJs in
Tourian-escape samples 285-449 of the `Test 2` recording are the ones that
show the artefact in play).

## Keeping it honest

`port_gpu_renderer.c`'s `BuildDrawParams` uses `PortAffine_SubtileQuad` from
the shared header. `CollectSprite` still has its own in-lined copy of the
matrix invert + decompose and the edge-flag test (byte-identical to
`PortAffine_Decompose` / `PortAffine_SubtileBleedEdges`); folding those in too
is a pending cleanup. Until then, changes to the *decompose* or *edge* logic
must be mirrored in both places.
