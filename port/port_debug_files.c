#include "port_debug_files.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef PLATFORM_LINUX
#define PORT_DEBUG_FILES_DIR "/tmp"
#else
#define PORT_DEBUG_FILES_DIR "sdmc:/3ds"
#endif

#define PORT_DEBUG_FILES_MAX_KEEP 99u

static unsigned ClampKeep(unsigned keep) {
    if (keep < 1u) return 1u;
    if (keep > PORT_DEBUG_FILES_MAX_KEEP) return PORT_DEBUG_FILES_MAX_KEEP;
    return keep;
}

/* Composes one slot path. Returns false if it would not fit, so callers can
 * fail loudly instead of writing to a silently truncated filename. */
static bool BuildPath(const char* name, unsigned index, const char* suffix,
                      char* out, size_t outLen) {
    if (!out || outLen == 0) return false;
    out[0] = '\0';
    const int n = snprintf(out, outLen, "%s/%s-%02u%s", PORT_DEBUG_FILES_DIR, name,
                           index, suffix ? suffix : "");
    if (n < 0 || (size_t)n >= outLen) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* The slot choice shared by both public entry points: probe every slot once
 * (a bounded number of stat() calls -- `keep` of them, not a scan of the
 * whole directory), take the first free one, else the least recently
 * modified. stat() rather than fopen() so probing never creates or
 * truncates anything. */
static unsigned PickSlot(const char* name, const char* probeSuffix, unsigned keep) {
    keep = ClampKeep(keep);

    unsigned oldestIndex = 1u;
    long oldestMtime = 0;
    bool haveOldest = false;

    for (unsigned i = 1u; i <= keep; ++i) {
        char path[256];
        if (!BuildPath(name, i, probeSuffix, path, sizeof(path))) continue;

        struct stat st;
        if (stat(path, &st) != 0) return i; /* free slot */

        const long mtime = (long)st.st_mtime;
        if (!haveOldest || mtime < oldestMtime) {
            oldestMtime = mtime;
            oldestIndex = i;
            haveOldest = true;
        }
    }
    return oldestIndex;
}

bool Port_DebugFiles_NextPath(const char* name, const char* ext, unsigned keep,
                              char* out, size_t outLen) {
    const unsigned slot = PickSlot(name, ext, keep);
    return BuildPath(name, slot, ext, out, outLen);
}

unsigned Port_DebugFiles_NextSetIndex(const char* name, const char* probeSuffix,
                                      unsigned keep) {
    return PickSlot(name, probeSuffix, keep);
}

bool Port_DebugFiles_SetPath(const char* name, unsigned index, const char* suffix,
                             char* out, size_t outLen) {
    return BuildPath(name, index, suffix, out, outLen);
}
