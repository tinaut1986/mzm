# Building the GBA ROM from this tree

`src/`, `include/` and `asm/` come from [metroidret/mzm](https://github.com/metroidret/mzm), the decompilation
this port is built on. The port edits some of those files, and the decompilation's own build, which
makes the GBA ROM, has to keep working: with `make REGION=eu` the ROM must come out byte for byte
the retail one (`mzm_eu.sha1`). It does, as of v0.7.8.

## Building it

You need the retail ROM of the region as `mzm_<region>_baserom.gba` next to the `Makefile` (not in
the repository: `.gitignore` keeps `*.gba` out), `agbcc` (`tools/agbcc`) and devkitARM.

```sh
tools/check_gba_rom.sh eu      # or us, jp
```

It copies the committed sources to a scratch directory, builds there and compares the SHA-1. By hand,
in a clean checkout:

```sh
cp /path/to/rom.gba mzm_eu_baserom.gba
make extract REGION=eu         # the graphics and data come out of the retail ROM
make -j REGION=eu              # -> mzm_eu.gba
```

Two things trip a first build: `make extract` has to come first, and the NES Metroid sub-makes find
`agbcc` on `PATH` with `libgcc.a` and `libc.a` in the same directory (the script sets that up).
Checked here only for EUR; USA and JP need their own ROM.

## What the port may do to those files

A change to a file that exists upstream goes inside the port's guard, with upstream's text kept for
the GBA:

```c
#if defined(MZM_3DS) || defined(PORT_NATIVE)
    /* the port's version */
#else
    /* upstream's version, unchanged */
#endif
```

Files the port adds are not affected (new files in `src/` are compiled by the GBA build too, so wrap
a whole new file in the guard). The port's versions of `asm/audio_internal.s` and
`asm/linker_symbols.s` live in `port/asm/`; `asm/` is upstream's.

`tools/guard_port.py` checks it without needing the ROM: the "GBA view" of a file (the port's
conditionals resolved as if no port macro were defined) must be upstream's text.

```sh
tools/guard_port.py check      # lists the files that differ; CI runs this (check-gba-sources.yml)
tools/guard_port.py fix        # wraps what differs in the guard above
```

`fix` leaves what the port builds exactly as it was (it checks that) and writes upstream's text
into the `#else`. It stops, and says which file, when a difference cannot be wrapped on its own (an
`#if` that mixes a port macro with another one), and that one is fixed by hand.

## After merging upstream

1. Merge `upstream/main` as usual and resolve conflicts, keeping the port's changes.
2. Put the upstream commit that was merged in `tools/upstream_base`.
3. `tools/guard_port.py fix`, then `tools/guard_port.py check`.
4. `tools/check_gba_rom.sh eu`, to see the ROM still matches.
