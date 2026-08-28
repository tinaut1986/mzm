#pragma once

/*
 * Region tests for game code.
 *
 * On the native ports (3DS) one binary must work with an EUR, USA or JAP
 * ROM, so the region is only known at runtime (gRomRegion, set by
 * Port_LoadRom). On the GBA build the region is fixed at compile time by
 * -DREGION_EU / -DREGION_JP (USA = neither), so these collapse to
 * constants and the compiler folds the branches away exactly like the
 * original #if did.
 *
 * Use these instead of #if REGION_* around anything the ports compile.
 */

#if defined(MZM_3DS) || defined(PORT_NATIVE)

#include "port_rom.h"

#define REGION_IS_EU() (gRomRegion == PORT_ROM_REGION_EU)
#define REGION_IS_JP() (gRomRegion == PORT_ROM_REGION_JP)
#define REGION_IS_US() (gRomRegion == PORT_ROM_REGION_US)

#else // GBA build: fixed at compile time

#ifdef REGION_EU
#define REGION_IS_EU() 1
#else
#define REGION_IS_EU() 0
#endif

#ifdef REGION_JP
#define REGION_IS_JP() 1
#else
#define REGION_IS_JP() 0
#endif

#if !defined(REGION_EU) && !defined(REGION_JP)
#define REGION_IS_US() 1
#else
#define REGION_IS_US() 0
#endif

#endif

/*
 * Several screens hard-code language behaviour per region (JAP checks for
 * hiragana, the others cannot have it), but the decomp's DEBUG builds already
 * carry a runtime gLanguage check for the same spots, since DEBUG allows any
 * language. That DEBUG form is exactly what a one-binary port needs, so define
 * this to compile it: guards written as
 *
 *     #if defined(REGION_LANGUAGE_RUNTIME) || defined(REGION_JP)
 *
 * pick the runtime variant on the ports and in DEBUG builds, and keep the
 * retail per-region behaviour on the GBA build.
 */
#if defined(DEBUG) || defined(MZM_3DS) || defined(PORT_NATIVE)
#define REGION_LANGUAGE_RUNTIME 1
#endif

/*
 * Runtime LANGUAGE_DEFAULT: the constant in constants/game_state.h is chosen at
 * compile time (JAPANESE for JAP, ENGLISH otherwise).
 */
#define REGION_DEFAULT_LANGUAGE() (REGION_IS_JP() ? LANGUAGE_JAPANESE : LANGUAGE_ENGLISH)
