#pragma once

/*
 * Umbrella gate for the L+R+<button> debug combos (instant-kill, scene
 * recorder, one-shot fixture replay, live atlas dump, one-shot screen/state
 * dump, USER MARK log line -- see docs/3ds-debug-tools.md). Compiled out
 * entirely (not just runtime-disabled) unless one of the flags below is
 * set, so a production build has no source-level path to trigger any of
 * them from a controller, not even by accident.
 *
 * PORT_DEBUG_TOOLS_ACTIVE is on when EITHER:
 *   - PORT_DEBUG_TOOLS is defined directly (make DEBUG_TOOLS=1 -- the
 *     "simple debug" build: just the combos, none of the verbose per-frame
 *     *_DIAG_LOG tracing), or
 *   - any existing per-system *_DIAG_LOG flag is defined (make
 *     EXTRA_CFLAGS=-DPORT_GPU_RENDERER_DIAG_LOG / -DPORT_AUDIO_DIAG_LOG),
 *     so a session that only asked for that system's tracing still gets the
 *     combos for free instead of silently missing them.
 *
 * Adding a new *_DIAG_LOG flag elsewhere in the port? Add it to the #if
 * below too, or its debug build won't get the combos either.
 */
#if defined(PORT_DEBUG_TOOLS) || defined(PORT_GPU_RENDERER_DIAG_LOG) || defined(PORT_AUDIO_DIAG_LOG)
#define PORT_DEBUG_TOOLS_ACTIVE 1
#endif
