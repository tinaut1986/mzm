#ifndef PORT_UPDATER_PARSE_H
#define PORT_UPDATER_PARSE_H

/* Pure logic of the self-updater: version comparison and release-list
 * parsing. No 3DS headers, so it builds and is unit-tested on the host
 * (tests/updater_parse_test.c). */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char tag[32];       /* "v0.6.5" */
    char cia_url[384];  /* browser_download_url of the .cia asset */
    bool prerelease;
} UpdaterRelease;

/* Compares "vX.Y.Z[-dev...]" strings. Returns true when `remote` is a newer
 * build than `current`. A "-dev" build sits on the way to its own X.Y.Z, so
 * the plain X.Y.Z release counts as newer than it. Unparseable input -> false. */
bool Updater_IsNewer(const char* current, const char* remote);

/* Scans a GitHub `GET /repos/{owner}/{repo}/releases` JSON body (newest
 * first) and picks the first release that has a .cia asset and, unless
 * `allowBeta`, is not a prerelease. Tolerates a truncated body. */
bool Updater_PickRelease(const char* json, bool allowBeta, UpdaterRelease* out);

#endif
