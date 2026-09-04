#include "port_debug_log.h"
#include "port_debug_files.h"

#include <string.h>
#include <stdio.h>

/* One rotating file per logging SESSION, picked the moment logging leaves
 * OFF (see Port_DebugLog_SetMode). The log used to be a single file that was
 * only ever appended to, never truncated -- so every capture landed on top
 * of every previous one and the documented workflow was "delete it over FTP
 * before you start, and hope you remembered". A session per file makes each
 * capture self-contained; the last PORT_DEBUG_LOG_KEEP of them survive.
 *
 * Empty until the first enable, so a build that never turns logging on
 * touches no file at all. */
#define PORT_DEBUG_LOG_NAME "mzm-debug"
#define PORT_DEBUG_LOG_EXT  ".log"
#define PORT_DEBUG_LOG_KEEP 10u
static char sLogPath[256];

static const char* LogPath(void) {
    /* Fallback for a line logged before any enable (should not happen: the
     * loggers all return early while the mode is NONE) -- better a known
     * filename than a write to "". */
    if (sLogPath[0] == '\0')
        Port_DebugFiles_SetPath(PORT_DEBUG_LOG_NAME, 1u, PORT_DEBUG_LOG_EXT,
                                sLogPath, sizeof(sLogPath));
    return sLogPath;
}

/* Runtime state, flipped from the bottom screen's DEBUG -> HERRAMIENTAS menu
 * (see port_bottom_ui_3ds.c). Writing to the SD card is OFF by default even
 * in a debug build: a debug build used to log every session unconditionally,
 * which costs SD I/O, grows mzm-debug.log without anyone asking, and makes
 * "start recording now" impossible to say. The state is checked in the
 * loggers themselves rather than at every call site, so a disabled build
 * path costs one predictable branch.
 *
 * sLogMode is both the master switch (NONE = nothing writes) and the
 * per-stream filter for the throttled per-frame diagnostics routed through
 * Port_DebugLog_Gpu/_Audio/_Perf. Plain Port_DebugLog() one-offs only care
 * that it is not NONE. */
static PortDebugLogMode sLogMode = PORT_LOG_MODE_NONE;
static bool sLogBuffered = true;

static bool StreamActive(PortDebugLogMode stream) {
    return sLogMode == PORT_LOG_MODE_ALL || sLogMode == stream;
}

/* 4KB is comfortably more than the throttled per-frame diagnostics
 * (GPUDIAG/GPUTIME/PERF/CMDBUF-style lines, each well under 256 bytes)
 * accumulate between natural flush points in practice -- sized generously
 * rather than tuned tight, since RAM is not the scarce resource here. */
static char sBufLog[4096];
static size_t sBufLogLen;

static void WriteDirect(const char* msg, size_t msgLen) {
    FILE* f = fopen(LogPath(), "a");
    if (f) {
        fwrite(msg, 1, msgLen, f);
        fputc('\n', f);
        fclose(f);
    }
}

void Port_DebugLogFlush(void) {
    if (sBufLogLen == 0) return;
    FILE* f = fopen(LogPath(), "a");
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
    if (sLogMode == PORT_LOG_MODE_NONE) return;
    if (sLogBuffered) {
        Port_DebugLogBuffered(msg);
        return;
    }
    WriteDirect(msg, strlen(msg));
}

void Port_DebugLogBuffered(const char* msg) {
    if (sLogMode == PORT_LOG_MODE_NONE) return;
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

void Port_DebugLog_SetMode(PortDebugLogMode mode) {
    if (mode < 0 || mode >= PORT_LOG_MODE_COUNT) return;
    if (mode == sLogMode) return;
    /* Any transition can leave lines that were captured under the old mode
     * sitting in RAM; flush so they land on disk at the moment of the
     * change rather than dying there (or getting mislabelled as the new
     * mode's output). */
    Port_DebugLogFlush();
    /* Leaving OFF starts a new session: claim a fresh slot so this capture
     * cannot be confused with the previous one. Switching between active
     * modes (ALL -> GPU -> ...) keeps writing to the same file, since it is
     * still the same sitting. */
    if (sLogMode == PORT_LOG_MODE_NONE && mode != PORT_LOG_MODE_NONE) {
        Port_DebugFiles_NextPath(PORT_DEBUG_LOG_NAME, PORT_DEBUG_LOG_EXT,
                                 PORT_DEBUG_LOG_KEEP, sLogPath, sizeof(sLogPath));
        FILE* f = fopen(LogPath(), "wb"); /* truncate the slot being reused */
        if (f) fclose(f);
    }
    sLogMode = mode;
}

const char* Port_DebugLog_CurrentPath(void) {
    return (sLogPath[0] == '\0') ? "" : sLogPath;
}

PortDebugLogMode Port_DebugLog_GetMode(void) { return sLogMode; }

void Port_DebugLog_CycleMode(void) {
    Port_DebugLog_SetMode((PortDebugLogMode)((sLogMode + 1) % PORT_LOG_MODE_COUNT));
}

const char* Port_DebugLog_ModeName(void) {
    switch (sLogMode) {
        case PORT_LOG_MODE_ALL:   return "ALL";
        case PORT_LOG_MODE_GPU:   return "GPU";
        case PORT_LOG_MODE_AUDIO: return "AUDIO";
        case PORT_LOG_MODE_PERF:  return "PERF";
        default:                  return "OFF";
    }
}

void Port_DebugLog_Gpu(const char* msg)  { if (StreamActive(PORT_LOG_MODE_GPU))  Port_DebugLogBuffered(msg); }
void Port_DebugLog_Perf(const char* msg) { if (StreamActive(PORT_LOG_MODE_PERF)) Port_DebugLogBuffered(msg); }
/* Audio follows the same buffering knob as everything else (the pre-split
 * audio sites went through Port_DebugLog, which buffers by default); turning
 * "LOG EN BUFFER" off is still how you force immediate writes for a hang. */
void Port_DebugLog_Audio(const char* msg) {
    if (!StreamActive(PORT_LOG_MODE_AUDIO)) return;
    if (sLogBuffered) Port_DebugLogBuffered(msg);
    else WriteDirect(msg, strlen(msg));
}

void Port_DebugLog_SetEnabled(bool enabled) {
    Port_DebugLog_SetMode(enabled ? (sLogMode == PORT_LOG_MODE_NONE ? PORT_LOG_MODE_ALL : sLogMode)
                                  : PORT_LOG_MODE_NONE);
}

bool Port_DebugLog_IsEnabled(void) { return sLogMode != PORT_LOG_MODE_NONE; }

void Port_DebugLog_SetBuffered(bool buffered) {
    if (buffered == sLogBuffered) return;
    if (!buffered) Port_DebugLogFlush();
    sLogBuffered = buffered;
}

bool Port_DebugLog_IsBuffered(void) { return sLogBuffered; }
