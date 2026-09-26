#include "port_updater_parse.h"

#include <stdlib.h>
#include <string.h>

static bool ParseVersion(const char* s, long out[3], bool* isDev) {
    char* end;
    int i;

    if (*s == 'v' || *s == 'V') s++;
    for (i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return false;
        out[i] = strtol(s, &end, 10);
        s = end;
        if (i < 2) {
            if (*s != '.') return false;
            s++;
        }
    }
    *isDev = (strncmp(s, "-dev", 4) == 0);
    return true;
}

bool Updater_IsNewer(const char* current, const char* remote) {
    long cur[3], rem[3];
    bool curDev, remDev;
    int i;

    if (!ParseVersion(current, cur, &curDev) || !ParseVersion(remote, rem, &remDev)) {
        return false;
    }
    for (i = 0; i < 3; i++) {
        if (rem[i] != cur[i]) return rem[i] > cur[i];
    }
    return curDev && !remDev;
}

/* Copies the JSON string value that follows `"key":` inside [seg, segEnd). */
static bool FindString(const char* seg, const char* segEnd, const char* key,
                       char* out, size_t outSize) {
    char pat[40];
    const char* p;
    size_t n = 0;

    strcpy(pat, "\"");
    strncat(pat, key, sizeof(pat) - 4);
    strcat(pat, "\"");
    p = strstr(seg, pat);
    if (!p || p >= segEnd) return false;
    p += strlen(pat);
    while (p < segEnd && (*p == ' ' || *p == ':')) p++;
    if (p >= segEnd || *p != '"') return false;
    p++;
    while (p < segEnd && *p != '"' && n + 1 < outSize) {
        out[n++] = *p++;
    }
    if (p >= segEnd || *p != '"') return false; /* truncated or too long */
    out[n] = '\0';
    return true;
}

static bool FindCiaUrl(const char* seg, const char* segEnd, char* out, size_t outSize) {
    static const char kKey[] = "\"browser_download_url\"";
    const char* p = seg;

    while ((p = strstr(p, kKey)) != NULL && p < segEnd) {
        char url[384];
        size_t len;

        if (FindString(p, segEnd, "browser_download_url", url, sizeof(url))) {
            len = strlen(url);
            if (len > 4 && strcmp(url + len - 4, ".cia") == 0) {
                strncpy(out, url, outSize - 1);
                out[outSize - 1] = '\0';
                return true;
            }
        }
        p += sizeof(kKey) - 1;
    }
    return false;
}

bool Updater_PickRelease(const char* json, bool allowBeta, UpdaterRelease* out) {
    static const char kTag[] = "\"tag_name\"";
    const char* p = json;

    while ((p = strstr(p, kTag)) != NULL) {
        const char* next = strstr(p + sizeof(kTag) - 1, kTag);
        const char* segEnd = next ? next : p + strlen(p);
        UpdaterRelease rel;
        char flag[8];

        memset(&rel, 0, sizeof(rel));
        p += sizeof(kTag) - 1;
        if (FindString(p - (sizeof(kTag) - 1), segEnd, "tag_name", rel.tag, sizeof(rel.tag))) {
            const char* pre = strstr(p, "\"prerelease\"");
            rel.prerelease = false;
            if (pre && pre < segEnd) {
                pre += sizeof("\"prerelease\"") - 1;
                while (pre < segEnd && (*pre == ' ' || *pre == ':')) pre++;
                strncpy(flag, pre, 4);
                flag[4] = '\0';
                rel.prerelease = (strcmp(flag, "true") == 0);
            }
            if ((allowBeta || !rel.prerelease) &&
                FindCiaUrl(p, segEnd, rel.cia_url, sizeof(rel.cia_url))) {
                *out = rel;
                return true;
            }
        }
        p = segEnd;
        if (!next) break;
    }
    return false;
}
