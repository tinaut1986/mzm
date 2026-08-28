#include "platform_3ds.h"

#include "port_debug_log.h"
#include "port_rom.h"

/* Metroid Zero Mission 3DS - Main Entrypoint */

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

/* Region priority when several supported ROMs are present in APP_DIR.
 * Lower rank wins. EU first: it is the reference build the port's
 * generated offset tables are byte-matched against (US/JP offset tables
 * land in Step B of multiregion-wip/PLAN.md). Could become an on-screen
 * picker later; a fixed order keeps FindRom deterministic for now. */
static int RomRegionRank(const char* gameCode) {
    if (memcmp(gameCode, "BMXP", 4) == 0) return 0; /* Europe */
    if (memcmp(gameCode, "BMXE", 4) == 0) return 1; /* USA    */
    if (memcmp(gameCode, "BMXJ", 4) == 0) return 2; /* Japan  */
    return -1;
}

/* Reads the 4-byte GBA game code at header offset 0xAC. Returns the
 * region rank (>= 0) for a supported ROM, or -1 for anything else. */
static int RomRegionRankOfFile(const char* path) {
    char gameCode[4];
    FILE* file = fopen(path, "rb");
    if (!file) return -1;
    int ok = fseek(file, 0xAC, SEEK_SET) == 0 &&
             fread(gameCode, 1, sizeof(gameCode), file) == sizeof(gameCode);
    fclose(file);
    return ok ? RomRegionRank(gameCode) : -1;
}

static int FindRom(char* out, size_t outSize) {
    DIR* dir = opendir(".");
    if (!dir) return 0;

    int foundGba = 0;
    int bestRank = -1;
    char bestName[ROM_PATH_SIZE];
    bestName[0] = '\0';

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!HasGbaExtension(entry->d_name)) continue;

        struct stat info;
        if (stat(entry->d_name, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        foundGba = 1;

        int rank = RomRegionRankOfFile(entry->d_name);
        if (rank < 0) continue;

        /* Deterministic selection independent of readdir() order: the
         * highest-priority region, and within one region the
         * lexicographically-smallest filename. */
        if (bestRank < 0 || rank < bestRank ||
            (rank == bestRank && strcmp(entry->d_name, bestName) < 0)) {
            bestRank = rank;
            snprintf(bestName, sizeof(bestName), "%s", entry->d_name);
        }
    }

    closedir(dir);

    if (bestRank >= 0) {
        snprintf(out, outSize, "%s", bestName);
        return 1;
    }
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

    printf("Metroid Zero Mission 3DS " MZM_PORT_VERSION "\n\n");
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
                     "is a supported Europe (BMXP), USA (BMXE) or Japan (BMXJ) ROM.\n\n"
                     "Expected SHA-1:\nEurope: 0fd107445a42e6f3a3e5ce8c865f412583179903\n"
                     "USA: 5de8536afe1f0078ee6fe1089f890e8c7aa0a6e8\n"
                     "Japan: 096f07685a3dc9286e71aa0b761f233b5efa2fcd",
                     APP_DIR);
        } else {
            snprintf(message, sizeof(message),
                     "Copy your clean Europe, USA or Japan ROM to:\n%s\n\nAny .gba filename is accepted.\n\n"
                     "Expected SHA-1:\nEurope: 0fd107445a42e6f3a3e5ce8c865f412583179903\n"
                     "USA: 5de8536afe1f0078ee6fe1089f890e8c7aa0a6e8\n"
                     "Japan: 096f07685a3dc9286e71aa0b761f233b5efa2fcd",
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
    extern void Port_Config_Load(void);
    Port_Config_Load();
    extern void Port_RA_Init(void);
    Port_RA_Init();
    extern void Port_LoadSram(void);
    Port_LoadSram();

    /* NDSP audio consumer: drains the mzm engine's PCM ring into the DSP.
     * Init'd before agbmain() so the per-frame capture in agbmain has a
     * consumer ready. Requires a working 3DS DSP firmware (Luma3DS Rosalina:
     * "Dump DSP firmware", produces sdmc:/3ds/dspfirm.cdc) or ndspInit()
     * fails and only the visual game runs -- see platform/3ds/README.md. */
    extern bool Port_MzmAudio_Init(void);
    Port_DebugLog("main: before Port_MzmAudio_Init");
    if (!Port_MzmAudio_Init()) {
        Port_DebugLog("main: Port_MzmAudio_Init failed (no DSP firmware?) -- audio disabled");
        printf("Warning: audio unavailable (ndspInit failed). Check DSP firmware dump.\n");
    } else {
        Port_DebugLog("main: Port_MzmAudio_Init done");
    }

    printf("Starting engine...\n");
    Platform3DS_EnterGameplayDisplay();

    /* agbmain() (game logic + audio production, see port/port_bios.c) now
     * runs on its own thread, paced by its own real-time deadline instead of
     * by GPU present. This thread keeps doing what it always did before
     * that split: own the GPU/citro3d context (from Port_PPU_Init above) and
     * push finished frames to the screen, now via a loop instead of a direct
     * call, at whatever pace the GPU can sustain -- see
     * platform/3ds/source/port_ppu_mzm.c's Port_PPU_GpuPresentPump. */
    Port_DebugLog("main: before starting agbmain() logic thread");
    if (!Platform3DS_StartLogicThread(agbmain)) {
        Platform3DS_ShowFatal("Thread error", "Could not start the game-logic thread.");
        Platform3DS_Shutdown();
        return 1;
    }

    extern bool Port_PPU_GpuPresentPump(void);
    Port_DebugLog("main: entering GPU present loop");
    for (;;) {
        if (!Port_PPU_GpuPresentPump()) {
            Platform3DS_WaitForVBlank();
        }
    }
}
