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
