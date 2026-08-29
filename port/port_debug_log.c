#include "port_debug_log.h"

#include <string.h>
#include <stdio.h>

#ifdef PLATFORM_LINUX
#define PORT_DEBUG_LOG_PATH "/tmp/mzm-debug.log"
#else
#define PORT_DEBUG_LOG_PATH "sdmc:/3ds/mzm-debug.log"
#endif

/* Both knobs are runtime state, flipped from the bottom screen's DEBUG ->
 * HERRAMIENTAS menu (see port_bottom_ui_3ds.c). Writing to the SD card is
 * OFF by default even in a debug build: a debug build used to log every
 * session unconditionally, which costs SD I/O, grows mzm-debug.log without
 * anyone asking, and makes "start recording now" impossible to say. The
 * flags are checked in the loggers themselves rather than at every call
 * site, so a disabled build path costs one predictable branch. */
static bool sLogEnabled = false;
static bool sLogBuffered = true;

/* 4KB is comfortably more than the throttled per-frame diagnostics
 * (GPUDIAG/GPUTIME/PERF/CMDBUF-style lines, each well under 256 bytes)
 * accumulate between natural flush points in practice -- sized generously
 * rather than tuned tight, since RAM is not the scarce resource here. */
static char sBufLog[4096];
static size_t sBufLogLen;

static void WriteDirect(const char* msg, size_t msgLen) {
    FILE* f = fopen(PORT_DEBUG_LOG_PATH, "a");
    if (f) {
        fwrite(msg, 1, msgLen, f);
        fputc('\n', f);
        fclose(f);
    }
}

void Port_DebugLogFlush(void) {
    if (sBufLogLen == 0) return;
    FILE* f = fopen(PORT_DEBUG_LOG_PATH, "a");
    if (f) {
        fwrite(sBufLog, 1, sBufLogLen, f);
        fclose(f);
    }
    sBufLogLen = 0;
}

/* Synchronous SD writes (open/append/close per call) from hot game paths
 * (RoomLoad, SpriteLoadAllData, music wrappers, ...) were stalling the logic
 * thread long enough to drain the audio ring -- visible as a full stop with
 * music dropout during room transitions (issue #22). That's why the default
 * even for this "unbuffered" entry point is the in-memory buffer; turning
 * buffering off from the menu restores the write-immediately behavior for
 * the case it exists for (a hang that eats whatever is still in RAM). */
void Port_DebugLog(const char* msg) {
    if (!sLogEnabled) return;
    if (sLogBuffered) {
        Port_DebugLogBuffered(msg);
        return;
    }
    WriteDirect(msg, strlen(msg));
}

void Port_DebugLogBuffered(const char* msg) {
    if (!sLogEnabled) return;
    size_t msgLen = strlen(msg);
    if (!sLogBuffered) {
        WriteDirect(msg, msgLen);
        return;
    }
    if (sBufLogLen + msgLen + 1 > sizeof(sBufLog)) Port_DebugLogFlush();
    if (msgLen + 1 > sizeof(sBufLog)) {
        /* Pathological single line longer than the whole buffer -- fall
         * back to writing it directly rather than truncating silently. */
        WriteDirect(msg, msgLen);
        return;
    }
    memcpy(sBufLog + sBufLogLen, msg, msgLen);
    sBufLogLen += msgLen;
    sBufLog[sBufLogLen++] = '\n';
}

void Port_DebugLog_SetEnabled(bool enabled) {
    if (enabled == sLogEnabled) return;
    /* Turning it off flushes first, so the tail of what was captured lands
     * on disk instead of dying in RAM at the moment the user says "stop". */
    if (!enabled) Port_DebugLogFlush();
    sLogEnabled = enabled;
}

bool Port_DebugLog_IsEnabled(void) { return sLogEnabled; }

void Port_DebugLog_SetBuffered(bool buffered) {
    if (buffered == sLogBuffered) return;
    if (!buffered) Port_DebugLogFlush();
    sLogBuffered = buffered;
}

bool Port_DebugLog_IsBuffered(void) { return sLogBuffered; }
