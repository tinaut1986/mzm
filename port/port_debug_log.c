#include "port_debug_log.h"

#include <stdio.h>

void Port_DebugLog(const char* msg) {
    FILE* f = fopen("sdmc:/3ds/mzm-debug.log", "a");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}
