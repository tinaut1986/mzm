#include "platform_linux.h"
#include "port_rom.h"
#include "port_gba_mem.h"
#include "temp_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

extern void agbmain(void);

bool sTestMode = false;
int sMaxFrames = 0;
int sCurrentFrame = 0;

static void HandleSignal(int sig) {
    printf("\n[Linux] Interrupted by signal %d\n", sig);
    exit(0);
}

int main(int argc, char** argv) {
    signal(SIGINT, HandleSignal);
    signal(SIGTERM, HandleSignal);

    printf("=========================================\n");
    printf(" Metroid: Zero Mission - Linux Native Port\n");
    printf("=========================================\n");

    const char* romPath = "mzm_eu.gba";
    bool headless = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            sTestMode = true;
            headless = true;
            sMaxFrames = 300;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                sMaxFrames = atoi(argv[++i]);
            }
            printf("[Linux] Test mode enabled: running %d frames headless\n", sMaxFrames);
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            romPath = argv[++i];
        }
    }

    /* 1. Try to load ROM */
    const char* candidatePaths[] = {
        romPath,
        "mzm_eu.gba",
        "baserom.gba",
        "../mzm_eu.gba",
        "../../mzm_eu.gba",
        NULL
    };

    bool romLoaded = false;
    for (i = 0; candidatePaths[i] != NULL; i++) {
        FILE* f = fopen(candidatePaths[i], "rb");
        if (f) {
            fclose(f);
            printf("[Linux] Loading ROM from '%s'...\n", candidatePaths[i]);
            if (Port_LoadRom(candidatePaths[i])) {
                romLoaded = true;
                break;
            }
        }
    }

    if (!romLoaded) {
        fprintf(stderr, "FATAL: Could not load Metroid Zero Mission ROM (tried %s, baserom.gba).\n", romPath);
        return 1;
    }

    printf("[Linux] ROM loaded successfully (Region: %d, Size: %u bytes)\n", gRomRegion, (unsigned int)gRomSize);

    /* 2. Initialize Platform */
    Platform_Linux_Init(headless);

    /* 3. Run game */
    printf("[Linux] Starting agbmain()...\n");
    agbmain();

    Platform_Linux_Cleanup();
    printf("[Linux] Game terminated cleanly.\n");
    return 0;
}
