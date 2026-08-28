# 3DS port: replacing the hand-written RetroAchievements client with rcheevos

Work order for a fresh session. Everything here was measured against the
repo and against a real console on 2026-08-28; nothing is assumed.

> **Status 2026-08-28 (evening): implemented, not yet run on hardware.**
> Steps 0-4 below are done and the build is clean; what is left is the
> on-console verification in "How to verify", which needs a real 3DS and an
> account. What landed:
>
> * **Step 0 took Route B**, but through a generated address map rather than
>   a linker script. `tools/gen_ra_iwram_map.py` reconstructs the original
>   IWRAM layout from the declaration order of `src/globals1.c` /
>   `src/globals2.c` and emits
>   `platform/3ds/source/port_ra_iwram_map.c`, mapping every IWRAM address
>   onto the decomp global that lives there. The missing symbol map the plan
>   worried about turned out to be unnecessary: RetroAchievements' code notes
>   are public and unauthenticated (`dorequest.php?r=codenotes2&g=534`), and
>   the generator hard-fails unless the reconstruction agrees with all 110 of
>   them plus 28 symbol/address pairings read off them by hand. All 37 IWRAM
>   addresses the set uses now resolve to a real variable; before, ~21 of
>   them returned unrelated EWRAM bytes.
> * **Steps 1-4**: rcheevos is vendored under `third_party/rcheevos`
>   (subset documented in its `VERSION.txt`), the module is rebuilt on
>   `rc_client`, the game is identified by MD5 of `gRomData` so the region's
>   game ID is resolved by the server, and the hand-written evaluator, HTTP
>   client, MD5 and `ReadRamValue` shim are gone. `port_retroachievements_3ds.c`
>   went from ~1350 lines to ~1000, most of which is the toast overlay.
> * The header contract is unchanged, so the bottom-screen UI needed no edits.
>   `RetroAchievementItem.memAddr` is now always empty: nothing read it, and
>   the trigger expression is rcheevos' business.
>
> Two things worth knowing before testing:
>
> * Every `rc_client` call happens on the main thread. HTTP still runs on a
>   worker, but responses are queued and handed back from `Port_RA_Update`,
>   so unlock toasts and list rebuilds never fire off-thread.
> * The old `ra_cache_534.json` is dead. rcheevos keeps its own session, and
>   an offline start now means no achievement list until the next successful
>   login rather than a stale cached one.

## Goal

Replace the hand-written RetroAchievements client and trigger evaluator in
`platform/3ds/source/port_retroachievements_3ds.c` with
[rcheevos](https://github.com/RetroAchievements/rcheevos), the official
library, and make achievements work for all three accepted ROM regions
(EUR / USA / JAP) from the single binary this port already builds.

## Why: what is actually there today

**rcheevos is not in the repo.** No vendored copy, no submodule, nothing.
The module is ~1350 lines of hand-written code:

- an HTTP client hitting `dorequest.php` directly (`httpc`, background
  threads),
- a **hand-written trigger evaluator** (`EvaluateSingleCondition`,
  `EvaluateConditionGroup`, `EvaluateTriggerExpression`),
- a hand-written MD5 used to sign unlock requests,
- a hand-written **address translation shim** (`ReadRamValue`).

Two systemic defects follow from that, both verified:

### 1. The evaluator understands almost none of the trigger language

It handled only the `R` (ResetIf) and `A` (AddSource) flags; every other
flag fell through to "this condition must pass". `O:` (OrNext) therefore
became a plain AND, which is not a degraded approximation but an
unsatisfiable one. Achievement 5763 "I'll Have To Charge Ya" reads:

```
0xH000c70=4_0xH000054=0_O:0xH000055=12_0xH000055=19_d0xQ00153c=0_0xQ00153c=1_0xH043fe9!=77
```

`O:0xH000055=12_0xH000055=19` means "room 12 OR room 19" and became "room
12 AND room 19" — it could never fire, in any room. That is the bug that
started this investigation.

Commit `311cbf6a` implements AndNext/OrNext chaining, which unblocks the 33
achievements that use those flags. **The rest of the language is still
missing**, most importantly hit counts:

| Feature | Achievements affected (of 62) | State after `311cbf6a` |
| --- | --- | --- |
| `N:` / `O:` AndNext / OrNext | 33 | fixed |
| Hit counts `.N.` | 30 | **still broken** — parsed as plain comparisons, no latching, so "was true once" becomes "is true right now" (stricter; blocks unlocks) |
| `A:` AddSource | 6 | **broken** — accumulates a value nothing ever reads |
| `B:` SubSource | 1 | **missing** |
| `P:` PauseIf | 1 | **missing** |
| `T:` Trigger (challenge indicator) | 18 | works by accident (treated as a normal condition) |

### 2. Half the memory the achievements read is garbage

RA addresses GBA memory as IWRAM at `0x0000-0x7FFF` (GBA `0x03000000`) then
EWRAM from `0x8000` (GBA `0x02000000`). Counting distinct addresses across
the 62 achievements of set 534:

- **7 EWRAM addresses** — these work. `platform/3ds/ewram_symbols.ld` places
  the decomp's EWRAM globals at their real offsets inside `gEwram`, so
  `gEwram[addr - 0x8000]` is genuinely correct.
- **37 IWRAM addresses** — these mostly do not. IWRAM globals (`gEquipment`,
  `gCurrentArea`, ...) are ordinary C variables placed wherever the compiler
  likes; they are *not* inside `gIwram`. `ReadRamValue` hand-maps roughly 16
  of them to C symbols, and **the remaining ~21 fall through to a fallback
  that treats an IWRAM address as an EWRAM offset** and returns unrelated
  bytes.

The IWRAM addresses in use:

```
002c 0054 0055 0059 0063 0064 0065 0066 0067 0068 0069 0150 01a8 01d0 0278
057c 05b0 0716 095c 0960 0961 0962 0c70 13d4 13e6 1530 1532 1534 1535 1536
153c 153d 153e 153f 1540 16f3 1d1c
```

**This is the real work.** rcheevos will faithfully evaluate whatever it is
given; if the memory underneath is wrong, correct evaluation of wrong data
is still wrong.

### 3. One region only

Game ID `534` is hardcoded in six places (URLs at
`port_retroachievements_3ds.c:369,374,440,444,450,454`, cache filename at
`:383,390,718`). EUR and JAP are separate games on RetroAchievements with
their own IDs *and their own address sets*.

## Plan

### Step 0 — decide the IWRAM strategy first (do this before writing code)

This choice sets the size of the whole job. Two routes:

**Route B (preferred): give IWRAM a faithful layout.** Place the decomp's
IWRAM globals at their real GBA addresses inside `gIwram`, exactly the way
`platform/3ds/ewram_symbols.ld` already does for EWRAM (read that file — it
is 30 lines of `symbol = gEwram + 0xNNNN;`). If this works, all 37
addresses become correct at once, future achievement-set revisions keep
working, and other regions cost almost nothing.

The obstacle: the repo contains **no IWRAM symbol map**. The original
per-region addresses have to come from somewhere — the RA code notes
(`dorequest.php?r=codenotes2&g=<id>` returns a human description per
address), the decomp's own history, or matching struct layouts by hand.
Note that EWRAM already being uniform across regions is evidence this may
be tractable, but do not assume IWRAM is the same.

**Route A (fallback): a complete translation table**, address → C symbol +
offset, one per region. Straightforward but triples with each region and
rots whenever the set is revised.

Timebox Route B's investigation. If it turns out to need the original ROM
symbol maps and those are not obtainable, fall back to A without regret.

### Step 1 — vendor rcheevos (~2 h)

C89, no dependencies, builds cleanly with devkitARM. Add the sources to
`platform/3ds/Makefile`. Keep it in a clearly-marked third-party directory.

### Step 2 — wire `rc_client` (~1 day)

Prefer `rc_client` over the lower-level `rc_runtime`: it owns login,
session, the unlock queue and hardcore rules, which is most of what the
current file hand-rolls. It needs three callbacks:

- **server call** — the port already has an async HTTP thread using `httpc`
  and a working `Result`/retry pattern; reuse it.
- **memory read** — `read(address, buffer, num_bytes)` over the RA address
  space. This is where Step 0's work lands.
- **event handler** — achievement triggered, login state, etc.

### Step 3 — identify the game by ROM hash (~half a day)

Drop the hardcoded 534. The whole ROM is already in memory
(`gRomData`/`gRomSize`, see `port/port_rom.h`) and the RA hash for GBA is
just the MD5 of the ROM, so this is a few lines — an MD5 implementation
already exists in the module for unlock signing. `rc_client` then resolves
the correct game ID per region on its own.

Region detection itself is already solved and must stay runtime-based:
`REGION=any` is the default build and `gRomRegion` is set by
`Port_LoadRom` (see `include/region.h`). **Do not add compile-time region
variants for this.**

### Step 4 — delete the hand-written engine

Remove `ReadRamValue`, `EvaluateSingleCondition`, `EvaluateConditionGroup`,
`EvaluateTriggerExpression` and the `struct RawEquipment` mirror. Do not
keep them as a fallback: two evaluators disagreeing is worse than one.

## Contracts that must not break

The rest of the port talks to this module through
`platform/3ds/source/port_retroachievements_3ds.h`. Keep that surface
working; the bottom-screen UI leans on it heavily:

- `Port_RA_Init` / `Port_RA_Shutdown` / `Port_RA_Update` — `main_3ds.c`,
  and `Port_RA_Update` + `Port_RA_RenderToastOverlay` are called every
  frame from `Port_BottomUI_Render`.
- `Port_RA_EvaluateTriggers` — called once per frame from `src/agbmain.c:510`.
  With `rc_client` this becomes `rc_client_do_frame`.
- `Port_RA_IsHardcore` — read by `platform_3ds_minimal.c` to disable the
  rapid-fire button mapping, and by the UI to hide spoilers.
- The achievement list accessors (`Port_RA_GetAchievementCount`,
  `Port_RA_GetAchievement`, the point totals, `Port_RA_GetBadgePixels`) back
  the OPTIONS → achievements modal.
- `Port_RA_GetStatusString` / `Port_RA_GetLastDebugLog` are shown on the
  DEBUG tab.

## How to verify

1. **The known-bad achievement.** 5763 "I'll Have To Charge Ya": pick up the
   Charge Beam in Brinstar room 12 or 19. It must unlock. Before `311cbf6a`
   it was unsatisfiable.
2. **The known-bad addresses.** Log what the memory callback returns for the
   37 IWRAM addresses and sanity-check a handful against on-screen state
   (`0x153c` bit 4 is the Charge Beam bit; `0x0054`/`0x0055` are area/room,
   both shown live on the DEBUG tab).
3. **All three regions.** Confirm the resolved game ID differs per ROM and
   that the achievement list loads for each.
4. The cached achievement set for the USA game is on the console at
   `sdmc:/3ds/Metroid Zero Mission 3DS/ra_cache_534.json`; the module also
   logs to `retroachievements.log` in the same folder.

## Build and deploy

```bash
make -C platform/3ds DEBUG_TOOLS=1 FTP_HOST=<console-ip> FTP_PORT=5000 ftp
```

`DEBUG_TOOLS=1` adds the bottom screen's DEBUG → HERRAMIENTAS menu
(`docs/3ds-debug-tools.md`), which is useful here: it can warp to a specific
room by door, toggle individual beams and suit upgrades, and reveal every
map — i.e. it can set up an achievement's preconditions in seconds instead
of by replaying. The equipment toggles are exactly the memory these triggers
watch.

Version strings come from the git branch, so **commit before building** if
you want to be able to tell two builds apart on the console; a rebuild
without a new commit produces an identically-named CIA and it is easy to
install the wrong one.
