#include "platform_3ds.h"

#include "port_debug_log.h"
#include "port_rom.h"

/* PPU (screen rendering) and audio init are not wired up yet -- this build
 * proves the rest of the pipeline (all gameplay logic, generated ROM data
 * shims, GBA BIOS reimplementation) compiles, links, and reaches agbmain()
 * on real 3DS hardware. See docs/3ds-port-rom-loading.md and
 * docs/3ds-port-skeleton-import.md for what's left: bridging port/ppu's
 * software renderer to citro2d/citro3d (port_ppu_3ds.c, currently still
 * TMC-coupled), and a real audio backend for mzm's own UpdateMusic/
 * TrackVariables engine (not M4A). */

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_DIR "sdmc:/3ds/Metroid Zero Mission 3DS"
#define ROM_PATH_SIZE 512

extern void agbmain(void);

static int PrepareStorage(void) {
    mkdir("sdmc:/3ds", 0777);
    if (mkdir(APP_DIR, 0777) != 0 && errno != EEXIST) return 0;
    return chdir(APP_DIR) == 0;
}

static int HasGbaExtension(const char* name) {
    const size_t length = strlen(name);
    if (length < 5) return 0;
    const char* ext = name + length - 4;
    return tolower((unsigned char)ext[0]) == '.' &&
           tolower((unsigned char)ext[1]) == 'g' &&
           tolower((unsigned char)ext[2]) == 'b' &&
           tolower((unsigned char)ext[3]) == 'a';
}

static int RomIsSupported(const char* path) {
    char gameCode[4];
    FILE* file = fopen(path, "rb");
    if (!file) return 0;
    int ok = fseek(file, 0xAC, SEEK_SET) == 0 && fread(gameCode, 1, sizeof(gameCode), file) == sizeof(gameCode) &&
             (memcmp(gameCode, "BMXE", sizeof(gameCode)) == 0 || memcmp(gameCode, "BMXP", sizeof(gameCode)) == 0);
    fclose(file);
    return ok;
}

static int FindRom(char* out, size_t outSize) {
    DIR* dir = opendir(".");
    if (!dir) return 0;

    int foundGba = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!HasGbaExtension(entry->d_name)) continue;

        struct stat info;
        if (stat(entry->d_name, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        foundGba = 1;
        if (!RomIsSupported(entry->d_name)) continue;
        snprintf(out, outSize, "%s", entry->d_name);
        closedir(dir);
        return 1;
    }

    closedir(dir);
    return foundGba ? -1 : 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    remove("sdmc:/3ds/mzm-debug.log");
    Port_DebugLog("main: start");
    if (!Platform3DS_Init()) return 1;
    Port_DebugLog("main: Platform3DS_Init done");
    Platform3DS_ShowSplash();

    printf("Metroid Zero Mission 3DS v" MZM_PORT_VERSION "\n\n");
    printf("System: %s\n", Platform3DS_IsNew3DS() ? "New Nintendo 3DS" : "Nintendo 3DS");
    printf("Performance: %s\n",
           Platform3DS_IsNew3DS() ? "New 3DS full presentation"
                                  : "Old 3DS adaptive presentation skip (max 3)");
    printf("PPU worker core 1: %u%%\n", Platform3DS_Core1TimeLimit());
    printf("Extra New 3DS core: %s\n\n", Platform3DS_IsNew3DS() ? "enabled" : "unavailable");
    printf("Preparing storage...\n");
    if (!PrepareStorage()) {
        Platform3DS_ShowFatal("Storage error", "Could not open " APP_DIR ".");
        Platform3DS_Shutdown();
        return 1;
    }

    char romPath[ROM_PATH_SIZE];
    int romResult = FindRom(romPath, sizeof(romPath));
    if (romResult <= 0) {
        char message[512];
        if (romResult < 0) {
            snprintf(message, sizeof(message),
                     "None of the .gba files in:\n%s\n"
                     "is a supported USA (BMXE) or Europe (BMXP) ROM.\n\n"
                     "Expected SHA-1:\nUSA: 5de8536afe1f0078ee6fe1089f890e8c7aa0a6e8\n"
                     "Europe: 0fd107445a42e6f3a3e5ce8c865f412583179903",
                     APP_DIR);
        } else {
            snprintf(message, sizeof(message),
                     "Copy your clean USA or Europe ROM to:\n%s\n\nAny .gba filename is accepted.\n\n"
                     "Expected SHA-1:\nUSA: 5de8536afe1f0078ee6fe1089f890e8c7aa0a6e8\n"
                     "Europe: 0fd107445a42e6f3a3e5ce8c865f412583179903",
                     APP_DIR);
        }
        Platform3DS_ShowFatal(romResult < 0 ? "Unsupported ROM" : "ROM not found", message);
        Platform3DS_Shutdown();
        return 1;
    }

    FILE* rom = fopen(romPath, "rb");
    if (!rom) {
        Platform3DS_ShowFatal("ROM error", "Could not open the selected .gba file.");
        Platform3DS_Shutdown();
        return 1;
    }
    fclose(rom);

    printf("Loading ROM...\n");
    Port_DebugLog("main: before Port_LoadRom");
    if (!Port_LoadRom(romPath)) {
        Platform3DS_ShowFatal("ROM error", "Port_LoadRom failed after the region check passed.");
        Platform3DS_Shutdown();
        return 1;
    }
    Port_DebugLog("main: Port_LoadRom done");
    printf("Loaded region: %s\n", Port_RomRegionLabel(gRomRegion));

    printf("Starting video...\n");
    extern bool Port_PPU_Init(void);
    Port_DebugLog("main: before Port_PPU_Init");
    if (!Port_PPU_Init()) {
        Platform3DS_ShowFatal("Video error", "Port_PPU_Init failed (citro2d/citro3d init).");
        Platform3DS_Shutdown();
        return 1;
    }
    extern void Port_LoadSram(void);
    Port_LoadSram();

    printf("Starting engine (no audio yet)...\n");
    Platform3DS_EnterGameplayDisplay();
    Port_DebugLog("main: before agbmain()");
    agbmain();

    Port_DebugLog("main: agbmain() returned (unexpected -- it should loop forever)");

    Platform3DS_Shutdown();
    return 0;
}
