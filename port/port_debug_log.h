#pragma once

#include <stdbool.h>

/*
 * File-based checkpoint logger for diagnosing boot hangs on real hardware.
 * A real CPU exception (bad memory access, etc.) gets an automatic crash
 * dump from Luma3DS -- but a genuine infinite loop (no fault, just spinning)
 * produces no dump at all, and there's no way to see on-screen console
 * output after a hang without another deploy/observe cycle. This appends a
 * line to sdmc:/3ds/mzm-debug.log, so whatever the LAST line written is
 * tells us exactly where execution got stuck.
 *
 * NOTHING IS WRITTEN unless logging has been switched on at runtime from
 * the bottom screen's DEBUG -> HERRAMIENTAS menu (LOG A SD). A debug build
 * is no longer a logging build: call sites stay compiled in, the writing
 * is opt-in per session.
 */
void Port_DebugLog(const char* msg);

/*
 * Same destination file, but buffered in RAM and only actually written to
 * the SD card when the buffer fills (or via Port_DebugLogFlush). An
 * unbuffered write does its own fopen+fwrite+fclose -- fine for the rare
 * boot-checkpoint case above, but real cost on real hardware
 * (~18-20ms/call, confirmed via the GPU renderer's own frame-timing
 * instrumentation once it was correctly isolated -- see
 * docs/3ds-port-gpu-renderer-status-2026-08-20.md section 16) when called
 * from a per-frame diagnostic path (GPUDIAG/GPUTIME/PERF/CMDBUF-style
 * logging, even throttled to once every few frames). Use THIS for any
 * logging driven by the per-frame render/update loop; keep using
 * Port_DebugLog for one-off boot/init/error checkpoints.
 *
 * Buffering is a runtime knob too (LOG EN BUFFER in the same menu, ON by
 * default). Turning it off makes BOTH entry points write immediately, for
 * the case buffering defeats: a hang that swallows whatever is still in RAM.
 */
void Port_DebugLogBuffered(const char* msg);
/* Force whatever's currently buffered out to disk now (one fopen+fwrite+
 * fclose for the whole buffer, not one per line). Call this from a safe,
 * infrequent point (e.g. on clean shutdown) if you want buffered lines to
 * survive even when the buffer doesn't happen to fill up. Not required for
 * normal operation -- the buffer flushes itself once full, and switching
 * either knob off flushes as well. */
void Port_DebugLogFlush(void);

/* Runtime knobs behind the menu toggles. Mode defaults to NONE (no SD
 * writes), buffered to true. Safe to call in any build -- plain statics, not
 * gated on any debug macro, so a non-debug build links the same file and
 * simply never turns logging on.
 *
 * SetEnabled/IsEnabled are kept as thin compatibility wrappers over the mode
 * (SetEnabled(true) -> ALL when currently NONE; IsEnabled() -> mode != NONE)
 * for the many one-off Port_DebugLog() boot/init/error checkpoints that only
 * care whether logging is on at all. */
void Port_DebugLog_SetEnabled(bool enabled);
bool Port_DebugLog_IsEnabled(void);
void Port_DebugLog_SetBuffered(bool buffered);
bool Port_DebugLog_IsBuffered(void);

/* Per-stream filter for the throttled per-frame diagnostic logs, on top of
 * the master switch above. NONE = nothing; ALL = every stream; or exactly
 * one stream. Cycled from DEBUG -> HERRAMIENTAS so a session can capture
 * just the stream it's chasing (e.g. GPU) without the others drowning it. */
typedef enum {
    PORT_LOG_MODE_NONE = 0,
    PORT_LOG_MODE_ALL,
    PORT_LOG_MODE_GPU,
    PORT_LOG_MODE_AUDIO,
    PORT_LOG_MODE_PERF,
    PORT_LOG_MODE_COUNT
} PortDebugLogMode;

void Port_DebugLog_SetMode(PortDebugLogMode mode);
PortDebugLogMode Port_DebugLog_GetMode(void);
void Port_DebugLog_CycleMode(void);
/* Path of the file the current logging session is writing to, or "" while
 * logging has never been enabled. One rotating file per session -- see the
 * comment in port_debug_log.c. */
const char* Port_DebugLog_CurrentPath(void);        /* NONE -> ALL -> GPU -> AUDIO -> PERF -> NONE */
const char* Port_DebugLog_ModeName(void);  /* "OFF" / "ALL" / "GPU" / "AUDIO" / "PERF" */

/* Stream-scoped loggers: write only when the current mode includes that
 * stream (ALL or that one). GPU/PERF go through the RAM buffer; AUDIO writes
 * immediately -- it is called from the audio thread, where a hang must not
 * eat the last lines (same rationale as Port_DebugLog's unbuffered path). A
 * per-frame diagnostic call site funnels through one of these instead of
 * Port_DebugLog directly. */
void Port_DebugLog_Gpu(const char* msg);
void Port_DebugLog_Audio(const char* msg);
void Port_DebugLog_Perf(const char* msg);
