#include "port_debug_log.h"

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
