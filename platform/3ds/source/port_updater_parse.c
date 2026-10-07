#include "port_updater_parse.h"

#include <stdio.h>
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

bool Updater_IsNewerBuild(const char* current, bool currentIsBeta,
                          const char* remote, bool remotePrerelease) {
    long cur[3], rem[3];
    bool curDev, remDev;

    if (Updater_IsNewer(current, remote)) return true;
    if (!currentIsBeta || remotePrerelease) return false;
    if (!ParseVersion(current, cur, &curDev) || !ParseVersion(remote, rem, &remDev)) return false;
    return !curDev && !remDev && cur[0] == rem[0] && cur[1] == rem[1] && cur[2] == rem[2];
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

/* ------------------------------------------------------------------------- */
/* Release notes                                                             */
/* ------------------------------------------------------------------------- */

#define NOTES_BEGIN "<!-- mzm-notes -->"
#define NOTES_END   "<!-- /mzm-notes -->"
#define NOTES_BLOCK_MAX 3072

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes the JSON string text in [p, end) into `dst` (UTF-8, truncated to
 * `cap` - 1 bytes). */
static void JsonUnescape(const char* p, const char* end, char* dst, size_t cap) {
    size_t n = 0;

    while (p < end && n + 4 < cap) {
        char c = *p++;
        if (c != '\\' || p >= end) {
            dst[n++] = c;
            continue;
        }
        c = *p++;
        switch (c) {
            case 'n': dst[n++] = '\n'; break;
            case 't': dst[n++] = ' '; break;
            case 'r': break;
            case 'u': {
                long cp = 0;
                int i;
                for (i = 0; i < 4 && p < end && HexVal(*p) >= 0; i++) cp = cp * 16 + HexVal(*p++);
                if (cp < 0x80) {
                    dst[n++] = (char)cp;
                } else if (cp < 0x800) {
                    dst[n++] = (char)(0xC0 | (cp >> 6));
                    dst[n++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    dst[n++] = '?';
                }
                break;
            }
            default: dst[n++] = c; break; /* \" \\ \/ */
        }
    }
    dst[n] = '\0';
}

/* Appends `s` (and a newline when `nl`) to out, never splitting a write: if it
 * does not fit, nothing is written and false is returned. Keeps 4 bytes back
 * for the closing "...". */
static bool AppendLine(char* out, size_t outSize, size_t* len, const char* s, bool nl) {
    size_t n = strlen(s);

    if (*len + n + (nl ? 1 : 0) + 4 >= outSize) return false;
    memcpy(out + *len, s, n);
    *len += n;
    if (nl) out[(*len)++] = '\n';
    out[*len] = '\0';
    return true;
}

/* One notes block (plain text, markdown) -> compact "- " lines. */
static bool AppendBlock(char* blk, char* out, size_t outSize, size_t* len) {
    char* line = blk;

    while (*line) {
        char* nl = strchr(line, '\n');
        char clean[160];
        const char* r;
        size_t n = 0;

        if (nl) *nl = '\0';
        r = line;
        while (*r == ' ') r++;
        if (*r != '#' && *r != '\0') {
            if ((r[0] == '*' || r[0] == '-') && r[1] == ' ') {
                clean[n++] = '-';
                clean[n++] = ' ';
                r += 2;
            }
            while (*r && n + 1 < sizeof(clean)) {
                if (*r == '`') { r++; continue; }
                if (r[0] == '*' && r[1] == '*') { r += 2; continue; }
                clean[n++] = *r++;
            }
            clean[n] = '\0';
            if (!AppendLine(out, outSize, len, clean, true)) return false;
        }
        if (!nl) break;
        line = nl + 1;
    }
    return true;
}

int Updater_CollectNotes(const char* json, bool allowBeta, const char* current,
                         bool currentIsBeta, char* out, size_t outSize) {
    static const char kTag[] = "\"tag_name\"";
    const char* p = json;
    size_t len = 0;
    int count = 0;
    bool full = false;

    if (outSize == 0) return 0;
    out[0] = '\0';

    while (!full && (p = strstr(p, kTag)) != NULL) {
        const char* next = strstr(p + sizeof(kTag) - 1, kTag);
        const char* segEnd = next ? next : p + strlen(p);
        char tag[32];
        bool prerelease = false;

        if (FindString(p, segEnd, "tag_name", tag, sizeof(tag))) {
            const char* pre = strstr(p, "\"prerelease\"");
            if (pre && pre < segEnd) {
                pre += sizeof("\"prerelease\"") - 1;
                while (pre < segEnd && (*pre == ' ' || *pre == ':')) pre++;
                prerelease = (strncmp(pre, "true", 4) == 0);
            }
            if ((allowBeta || !prerelease) && Updater_IsNewerBuild(current, currentIsBeta, tag, prerelease)) {
                char head[48];
                const char* body = strstr(p, "\"body\"");
                const char* b0 = NULL;
                const char* b1 = NULL;

                snprintf(head, sizeof(head), "%s== %s ==", count ? "\n" : "", tag);
                if (!AppendLine(out, outSize, &len, head, true)) { full = true; break; }
                count++;

                if (body && body < segEnd) {
                    const char* q = body + sizeof("\"body\"") - 1;
                    while (q < segEnd && (*q == ' ' || *q == ':')) q++;
                    if (q < segEnd && *q == '"') {
                        const char* e = ++q;
                        while (e < segEnd && *e != '"') e += (*e == '\\' && e + 1 < segEnd) ? 2 : 1;
                        /* The raw text, markers included: they hold no escapes. */
                        b0 = q;
                        b1 = e;
                    }
                }
                {
                    const char* m0 = NULL;
                    const char* m1 = b1;
                    const char* s0;

                    for (s0 = b0; b0 && s0 + sizeof(NOTES_BEGIN) - 1 <= b1; s0++) {
                        if (strncmp(s0, NOTES_BEGIN, sizeof(NOTES_BEGIN) - 1) == 0) {
                            m0 = s0 + sizeof(NOTES_BEGIN) - 1;
                            break;
                        }
                    }
                    for (s0 = m0; m0 && s0 + sizeof(NOTES_END) - 1 <= b1; s0++) {
                        if (strncmp(s0, NOTES_END, sizeof(NOTES_END) - 1) == 0) { m1 = s0; break; }
                    }
                    if (m0) {
                        char blk[NOTES_BLOCK_MAX];
                        JsonUnescape(m0, m1, blk, sizeof(blk));
                        if (!AppendBlock(blk, out, outSize, &len)) full = true;
                    } else if (!AppendLine(out, outSize, &len, "- (no notes for this version)", true)) {
                        full = true;
                    }
                }
            }
        }
        p = segEnd;
        if (!next) break;
    }
    if (full && len + 4 < outSize) {
        memcpy(out + len, "...", 4);
    }
    return count;
}
