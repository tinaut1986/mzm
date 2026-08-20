#pragma once

/*
 * File-based checkpoint logger for diagnosing boot hangs on real hardware.
 * A real CPU exception (bad memory access, etc.) gets an automatic crash
 * dump from Luma3DS -- but a genuine infinite loop (no fault, just spinning)
 * produces no dump at all, and there's no way to see on-screen console
 * output after a hang without another deploy/observe cycle. This appends a
 * line to sdmc:/3ds/mzm-debug.log and flushes immediately, so whatever the
 * LAST line written is tells us exactly where execution got stuck.
 *
 * Temporary diagnostic tool, not meant to stay once the hang here is fixed.
 */
void Port_DebugLog(const char* msg);

/*
 * Same destination file, but buffered in RAM and only actually written to
 * the SD card when the buffer fills (or via Port_DebugLogFlush). Every
 * Port_DebugLog() call does its own fopen+fwrite+fclose -- fine for the rare
 * boot-checkpoint case above, but real cost on real hardware
 * (~18-20ms/call, confirmed via the GPU renderer's own frame-timing
 * instrumentation once it was correctly isolated -- see
 * docs/3ds-port-gpu-renderer-status-2026-08-20.md section 16) when called
 * from a per-frame diagnostic path (GPUDIAG/GPUTIME/PERF/CMDBUF-style
 * logging, even throttled to once every few frames). Use THIS for any
 * logging driven by the per-frame render/update loop; keep using the
 * unbuffered Port_DebugLog for one-off boot/init/error checkpoints where
 * losing the last few buffered lines to a hang would defeat the point.
 */
void Port_DebugLogBuffered(const char* msg);
/* Force whatever's currently buffered out to disk now (one fopen+fwrite+
 * fclose for the whole buffer, not one per line). Call this from a safe,
 * infrequent point (e.g. on clean shutdown) if you want buffered lines to
 * survive even when the buffer doesn't happen to fill up. Not required for
 * normal operation -- the buffer flushes itself once full. */
void Port_DebugLogFlush(void);
