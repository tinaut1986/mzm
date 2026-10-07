# Save states (3DS port)

A save state is a file `mzm-state<id>.bin` in `states/`, with the screenshot in
`mzm-state<id>.thm`. The list is open ended and read from the SD card once. The id
only names the file; the number shown is the position in the list (newest first,
from 1), so it changes when a state is saved or deleted.
Code: `platform/3ds/source/port_save_state.c` (files and jobs),
`port_state_vars.c` (the decomp's globals by name), `port_state_ptrs.c` (host
pointers), `port_state_thumb.c` (screenshot). UI: the STATE tab in
`port_bottom_ui_3ds.c`.

## Format 4: independent of the build

Earlier states (format 3) were a raw dump of memory, full of absolute host
pointers, and only loaded into the very build that wrote them. Format 4 does
not depend on the build, only on the ROM: a state loads after any rebuild, as
long as the rules below hold. Format 5 is format 4 with the game's clock added to the header (the file is
otherwise the same); formats 4, 2 and 3 are still read, and load only
when the build's layout fingerprint matches, as before.

What a state holds:

| Part | How |
|---|---|
| EWRAM, IWRAM, IO, palettes, OAM, VRAM, SRAM | raw. Their sizes are the GBA's |
| The decomp's mutable globals (`.data`/`.bss` of `src/`) | by **name**, with their size |
| Pointers inside those globals | as relocations: a space + offset (emulated memory, ROM), a function name, or a global's name + offset |
| Music | only the track id and priority; the track restarts on load |

The names come from the linked ELF: `tools/gen_state_vars.py` runs after the
link (Makefile, `$(TARGET).elf`) and writes the table into `gStateVarBlob`
(`port_state_blob.c`). If that step is skipped the binary saves format 3.

On load: the memory regions and globals are copied back by name (a global that
no longer exists is dropped; a new one keeps its live value; a global whose size
changed copies the smaller of the two), the tables of ROM pointers are derived
again (`PortGen_All_Init`, `Port_InitConstructorPointers`), the relocations are
applied, the screen caches are invalidated and the music is started.

## What to keep in mind when changing code

**Adding, removing, renaming or resizing a decomp global is safe.** Renaming
loses that global's value in old states (it is a new variable to them); resizing
keeps the common prefix.

**A new global that holds a host pointer** needs to be covered, otherwise a
state keeps a stale address and crashes after a rebuild:

- A global that is *only* pointers (a table of ROM data, `p_...`, a function
  pointer) is found by itself: every word is 0 or a pointer to a space, a
  function in `kFuncs` or another global.
- A global that mixes pointers with other data (a sprite, a physics struct) must
  be listed in `kRules` in `port_state_ptrs.c`, with the spaces its pointers can
  point into. It is not scanned automatically: a 32-bit word made of two 16-bit
  positions can equal an address in the emulated EWRAM.
- A pointer to a **function** works only for the functions in `kFuncs`. Add the
  function there if a global can come to hold it.
- A pointer to something that is not emulated memory, ROM, one of these
  functions or a decomp global (heap objects, libctru handles, mutexes) cannot be
  saved. Keep it out of the decomp's globals, or add the global to `kNotSaved`.

**Anything that is a runtime object, not game state, goes in `kNotSaved`** in
`port_state_ptrs.c`: threads, mutexes, handles, buffers the port fills. Today:
the audio engine and the save thread in `src/sram/sram.c`. A state that
overwrote them would restore dead handles.

**Audio**: the audio engine is not saved. Its structures point into each other
and into sample data. A loaded state restarts the saved track with
`PlayCurrentMusicTrack`; sound effects in flight are not restored.

**The port's own state** (`platform/3ds/source`, renderer caches, citro3d) is not
part of a state and must not become part of the decomp's globals.

## Checking for a pointer that is not covered

A debug build (`DEBUG_TOOLS=1`) writes `debug/state-census-<id>.txt` with every
save. Put it through the ELF of the same build:

    python3 tools/state_census_report.py state-census-1.txt platform/3ds/mzm-3ds.elf

The `NOT COVERED` section lists globals with words that look like pointers into
the binary and that no rule handles. Most are tiles or positions; one that really
holds a pointer needs a rule as above.

`make -C platform/3ds test-state` runs the host test of the pointer codec (a
32-bit build; needs `gcc-multilib`).

## Not done

- Loading works from anywhere (title, menus, transitions): the job runs between
  frames, at the top of the main loop, where no game function is half way
  through. Saving still needs gameplay (`Port_SaveState_Available`), because the
  screenshot and the stats describe a live room. Loading outside gameplay is new
  and has had little testing; if a mode misbehaves after a load, that mode keeps
  runtime state outside the decomp's globals that a state does not restore.
- Format 3 states are not converted.
