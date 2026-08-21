#include "port_debug_log.h"

#include <string.h>
#include <stdio.h>

#ifdef PLATFORM_LINUX
#define PORT_DEBUG_LOG_PATH "/tmp/mzm-debug.log"
#else
#define PORT_DEBUG_LOG_PATH "sdmc:/3ds/mzm-debug.log"
#endif

void Port_DebugLog(const char* msg) {
    FILE* f = fopen(PORT_DEBUG_LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

/* 4KB is comfortably more than the throttled per-frame diagnostics
 * (GPUDIAG/GPUTIME/PERF/CMDBUF-style lines, each well under 256 bytes)
 * accumulate between natural flush points in practice -- sized generously
 * rather than tuned tight, since RAM is not the scarce resource here. */
static char sBufLog[4096];
static size_t sBufLogLen;

void Port_DebugLogFlush(void) {
    if (sBufLogLen == 0) return;
    FILE* f = fopen(PORT_DEBUG_LOG_PATH, "a");
    if (f) {
        fwrite(sBufLog, 1, sBufLogLen, f);
        fclose(f);
    }
    sBufLogLen = 0;
}

void Port_DebugLogBuffered(const char* msg) {
    size_t msgLen = strlen(msg);
    if (sBufLogLen + msgLen + 1 > sizeof(sBufLog)) Port_DebugLogFlush();
    if (msgLen + 1 > sizeof(sBufLog)) {
        /* Pathological single line longer than the whole buffer -- fall
         * back to writing it directly rather than truncating silently. */
        Port_DebugLog(msg);
        return;
    }
    memcpy(sBufLog + sBufLogLen, msg, msgLen);
    sBufLogLen += msgLen;
    sBufLog[sBufLogLen++] = '\n';
}
