#include "port_retroachievements_3ds.h"
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool sRAEnabled = false;
static bool sRAHardcore = true;
static bool sRANotifSound = true;
static char sRAUsername[64] = "";
static char sRAToken[64] = "";
static RetroAchievementsStatus sRAStatus = RA_STATUS_DISABLED;
static char sLastStatusMsg[64] = "";

/* Background worker thread for network operations */
static Thread sNetworkThread = NULL;
static bool sHttpInitialized = false;
static char sPendingPassword[64] = "";
static bool sPendingLogin = false;

/* Active toast notification */
static struct {
    bool active;
    uint32_t timer;
    char title[64];
    uint32_t points;
} sToast = { false, 0, "", 0 };

static void EnsureHttpInit(void) {
    if (!sHttpInitialized) {
        Result res = httpcInit(0x20000); /* 128KB shared memory for HTTP */
        if (R_SUCCEEDED(res)) {
            sHttpInitialized = true;
        }
    }
}

/* Helper to URL-encode special characters in password / username */
static void UrlEncode(const char* src, char* dst, size_t dstSize) {
    static const char hex[] = "0123456789ABCDEF";
    size_t d = 0;
    while (*src && d + 4 < dstSize) {
        unsigned char c = (unsigned char)*src++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[d++] = (char)c;
        } else {
            dst[d++] = '%';
            dst[d++] = hex[(c >> 4) & 0xF];
            dst[d++] = hex[c & 0xF];
        }
    }
    dst[d] = '\0';
}

/* HTTP GET Request Helper */
static int HttpPerformGet(const char* url, char* outBuf, size_t outSize) {
    EnsureHttpInit();
    if (!sHttpInitialized) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "HTTPC INIT FAILED");
        return -1;
    }

    httpcContext context;
    Result ret = httpcOpenContext(&context, HTTPC_METHOD_GET, url, 1);
    if (R_FAILED(ret)) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "OPEN CTX ERR: 0x%08lX", (unsigned long)ret);
        return -2;
    }

    /* Disable SSL certificate verification */
    httpcSetSSLOpt(&context, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(&context, "User-Agent", "RetroAchievements/1.0 (Nintendo 3DS; MZM-Port)");
    httpcAddRequestHeaderField(&context, "Accept", "*/*");
    httpcAddRequestHeaderField(&context, "Connection", "close");

    ret = httpcBeginRequest(&context);
    if (R_FAILED(ret)) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "REQ ERR: 0x%08lX", (unsigned long)ret);
        httpcCloseContext(&context);
        return -3;
    }

    u32 statuscode = 0;
    ret = httpcGetResponseStatusCodeTimeout(&context, &statuscode, 15000000000ULL); /* 15s timeout */
    if (R_FAILED(ret) && statuscode == 0) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "RESP ERR: 0x%08lX", (unsigned long)ret);
        httpcCloseContext(&context);
        return -4;
    }
    if (statuscode != 200 && statuscode != 302 && statuscode != 0) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "HTTP CODE: %lu", (unsigned long)statuscode);
        httpcCloseContext(&context);
        return (int)statuscode;
    }

    u32 contentSize = 0;
    httpcGetDownloadSizeState(&context, NULL, &contentSize);

    u32 totalDownloaded = 0;
    if (contentSize > 0 && contentSize < outSize) {
        ret = httpcDownloadData(&context, (u8*)outBuf, contentSize, &totalDownloaded);
        if (totalDownloaded == 0) totalDownloaded = contentSize;
    } else {
        u32 curDown = 0;
        do {
            u32 chunk = 256;
            if (totalDownloaded + chunk >= outSize) {
                chunk = (u32)(outSize - 1 - totalDownloaded);
                if (chunk == 0) break;
            }
            ret = httpcReceiveData(&context, (u8*)outBuf + totalDownloaded, chunk);
            httpcGetDownloadSizeState(&context, &curDown, NULL);
            if (curDown > totalDownloaded) {
                totalDownloaded = curDown;
            }
            if (ret == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
                svcSleepThread(5000000ULL);
                ret = 0;
            }
        } while (ret == 0 && totalDownloaded < outSize - 1);
    }

    outBuf[totalDownloaded] = '\0';
    httpcCloseContext(&context);

    if (totalDownloaded == 0) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "EMPTY (CODE %lu, 0x%08lX)", (unsigned long)statuscode, (unsigned long)ret);
        return -5;
    }
    return (int)totalDownloaded;
}

#define MAX_RA_ACHIEVEMENTS 128
static RetroAchievementItem sAchievements[MAX_RA_ACHIEVEMENTS];
static uint32_t sAchievementCount = 0;

/* Helper to unescape JSON string values in place */
static void JsonUnescape(char* str) {
    char* src = str;
    char* dst = str;
    while (*src) {
        if (*src == '\\' && *(src + 1)) {
            src++;
            if (*src == '\"') *dst++ = '\"';
            else if (*src == '\\') *dst++ = '\\';
            else if (*src == '/') *dst++ = '/';
            else if (*src == 'n') *dst++ = ' ';
            else if (*src == 'r') *dst++ = ' ';
            else if (*src == 't') *dst++ = ' ';
            else *dst++ = *src;
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Helper to extract a JSON string field value: "Key":"Value" */
static bool JsonGetStringField(const char* jsonStart, const char* jsonEnd, const char* key, char* out, size_t outSize) {
    char searchKey[64];
    snprintf(searchKey, sizeof(searchKey), "\"%s\":\"", key);
    const char* p = strstr(jsonStart, searchKey);
    if (!p || p >= jsonEnd) return false;
    p += strlen(searchKey);

    const char* end = p;
    while (end < jsonEnd && *end != '\0') {
        if (*end == '\"' && *(end - 1) != '\\') break;
        end++;
    }
    if (end >= jsonEnd) return false;

    size_t len = end - p;
    if (len >= outSize) len = outSize - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    JsonUnescape(out);
    return true;
}

/* Helper to extract a JSON integer field value: "Key":123 */
static bool JsonGetIntField(const char* jsonStart, const char* jsonEnd, const char* key, uint32_t* outVal) {
    char searchKey[64];
    snprintf(searchKey, sizeof(searchKey), "\"%s\":", key);
    const char* p = strstr(jsonStart, searchKey);
    if (!p || p >= jsonEnd) return false;
    p += strlen(searchKey);
    while (*p == ' ' && p < jsonEnd) p++;

    if (p >= jsonEnd) return false;
    *outVal = (uint32_t)strtoul(p, NULL, 10);
    return true;
}

/* Parse dynamic achievements list from RetroAchievements PatchData JSON */
static void ParsePatchAchievements(const char* json) {
    if (!json) return;
    const char* achStart = strstr(json, "\"Achievements\":");
    if (!achStart) achStart = strstr(json, "\"Achievements\" :");
    if (!achStart) return;

    const char* openBracket = strchr(achStart, '[');
    if (!openBracket) return;

    sAchievementCount = 0;
    const char* cur = openBracket + 1;

    while (*cur && sAchievementCount < MAX_RA_ACHIEVEMENTS) {
        const char* objStart = strchr(cur, '{');
        if (!objStart) break;

        /* Find matching closing brace */
        const char* objEnd = objStart + 1;
        int depth = 1;
        while (*objEnd && depth > 0) {
            if (*objEnd == '{') depth++;
            else if (*objEnd == '}') depth--;
            objEnd++;
        }
        if (depth != 0) break;

        uint32_t aid = 0;
        uint32_t flags = 0;
        if (JsonGetIntField(objStart, objEnd, "ID", &aid) &&
            JsonGetIntField(objStart, objEnd, "Flags", &flags)) {
            /* Flags: 3 = Core Official Set, 5 = Unofficial */
            if (aid > 0 && aid < 100000000 && flags == 3) {
                RetroAchievementItem* item = &sAchievements[sAchievementCount];
                item->id = aid;
                item->unlocked = false;
                item->hardcoreUnlocked = false;
                item->unlockTime = 0;

                JsonGetStringField(objStart, objEnd, "Title", item->title, sizeof(item->title));
                JsonGetStringField(objStart, objEnd, "Description", item->description, sizeof(item->description));
                JsonGetStringField(objStart, objEnd, "BadgeName", item->badgeName, sizeof(item->badgeName));
                JsonGetIntField(objStart, objEnd, "Points", &item->points);
                char typeStr[32] = { 0 };
                JsonGetStringField(objStart, objEnd, "Type", typeStr, sizeof(typeStr));
                if (strcmp(typeStr, "progression") == 0) {
                    item->type = RA_ACH_TYPE_PROGRESSION;
                } else if (strcmp(typeStr, "win_condition") == 0) {
                    item->type = RA_ACH_TYPE_WIN_CONDITION;
                } else if (strcmp(typeStr, "missable") == 0) {
                    item->type = RA_ACH_TYPE_MISSABLE;
                } else {
                    item->type = RA_ACH_TYPE_STANDARD;
                }

                /* RA Option: Parse MemAddr condition string */
                JsonGetStringField(objStart, objEnd, "MemAddr", item->memAddr, sizeof(item->memAddr));

                sAchievementCount++;
            }
        }
        cur = objEnd;
    }
}

/* Download and cache full official patch definitions from RetroAchievements (r=patch) */
static void FetchGamePatch(void) {
    if (sRAUsername[0] == '\0' || sRAToken[0] == '\0') return;

    char encUser[128] = { 0 };
    char encTok[128] = { 0 };
    UrlEncode(sRAUsername, encUser, sizeof(encUser));
    UrlEncode(sRAToken, encTok, sizeof(encTok));

    char url[768];
    static char sPatchBuf[65536];
    snprintf(url, sizeof(url), "https://retroachievements.org/dorequest.php?r=patch&g=534&u=%s&t=%s",
             encUser, encTok);

    int res = HttpPerformGet(url, sPatchBuf, sizeof(sPatchBuf));
    if (res <= 0) {
        snprintf(url, sizeof(url), "http://retroachievements.org/dorequest.php?r=patch&g=534&u=%s&t=%s",
                 encUser, encTok);
        res = HttpPerformGet(url, sPatchBuf, sizeof(sPatchBuf));
    }

    if (res > 0 && strstr(sPatchBuf, "\"Success\":true") != NULL) {
        ParsePatchAchievements(sPatchBuf);

        /* Save cache to SD */
        FILE* cacheFile = fopen("sdmc:/3ds/Metroid Zero Mission 3DS/ra_cache_534.json", "wb");
        if (cacheFile) {
            fwrite(sPatchBuf, 1, res, cacheFile);
            fclose(cacheFile);
        }
    } else {
        /* Fallback: load previous cached definitions from SD if offline */
        FILE* cacheFile = fopen("sdmc:/3ds/Metroid Zero Mission 3DS/ra_cache_534.json", "rb");
        if (cacheFile) {
            size_t bytes = fread(sPatchBuf, 1, sizeof(sPatchBuf) - 1, cacheFile);
            sPatchBuf[bytes] = '\0';
            fclose(cacheFile);
            ParsePatchAchievements(sPatchBuf);
        }
    }
}

/* Helper to check if a numeric achievement ID exists in a JSON array string like "[5760,5761,5762]" */
static bool JsonArrayContainsId(const char* jsonArray, uint32_t id) {
    if (!jsonArray) return false;
    char idStr[16];
    snprintf(idStr, sizeof(idStr), "%lu", (unsigned long)id);
    size_t idLen = strlen(idStr);

    const char* p = jsonArray;
    while ((p = strstr(p, idStr)) != NULL) {
        char prev = (p == jsonArray) ? '\0' : *(p - 1);
        char next = *(p + idLen);
        bool validPrev = (prev == '[' || prev == ',' || prev == ' ' || prev == ':');
        bool validNext = (next == ']' || next == ',' || next == ' ' || next == '\0' || next == '}');
        if (validPrev && validNext) {
            return true;
        }
        p += idLen;
    }
    return false;
}

/* Helper to fetch unlocked achievements from RetroAchievements (Game ID 534) */
static void FetchUserUnlocks(void) {
    if (sRAUsername[0] == '\0' || sRAToken[0] == '\0') return;

    /* 1. Ensure latest patch definitions are loaded first */
    if (sAchievementCount == 0) {
        FetchGamePatch();
    }

    char encUser[128] = { 0 };
    char encTok[128] = { 0 };
    UrlEncode(sRAUsername, encUser, sizeof(encUser));
    UrlEncode(sRAToken, encTok, sizeof(encTok));

    char url[768];
    char softBuf[4096] = { 0 };
    char hardBuf[4096] = { 0 };

    /* 2. Fetch Softcore unlocks */
    snprintf(url, sizeof(url), "https://retroachievements.org/dorequest.php?r=unlocks&u=%s&t=%s&g=534",
             encUser, encTok);
    int resSoft = HttpPerformGet(url, softBuf, sizeof(softBuf));
    if (resSoft <= 0) {
        snprintf(url, sizeof(url), "http://retroachievements.org/dorequest.php?r=unlocks&u=%s&t=%s&g=534",
                 encUser, encTok);
        resSoft = HttpPerformGet(url, softBuf, sizeof(softBuf));
    }

    /* 3. Fetch Hardcore unlocks (h=1) */
    snprintf(url, sizeof(url), "https://retroachievements.org/dorequest.php?r=unlocks&u=%s&t=%s&g=534&h=1",
             encUser, encTok);
    int resHard = HttpPerformGet(url, hardBuf, sizeof(hardBuf));
    if (resHard <= 0) {
        snprintf(url, sizeof(url), "http://retroachievements.org/dorequest.php?r=unlocks&u=%s&t=%s&g=534&h=1",
                 encUser, encTok);
        resHard = HttpPerformGet(url, hardBuf, sizeof(hardBuf));
    }

    FILE* logFile = fopen("sdmc:/3ds/Metroid Zero Mission 3DS/retroachievements.log", "a");
    if (logFile) {
        fprintf(logFile, "UNLOCKS SYNC: TotalAchievements=%lu, SoftcoreRes=%d, HardcoreRes=%d\n",
                (unsigned long)sAchievementCount, resSoft, resHard);
        fprintf(logFile, "SOFTCORE JSON: %s\n", softBuf);
        fprintf(logFile, "HARDCORE JSON: %s\n\n", hardBuf);
        fclose(logFile);
    }

    const char* softArr = (resSoft > 0) ? strstr(softBuf, "\"UserUnlocks\":") : NULL;
    const char* hardArr = (resHard > 0) ? strstr(hardBuf, "\"UserUnlocks\":") : NULL;

    for (size_t i = 0; i < sAchievementCount; ++i) {
        uint32_t aid = sAchievements[i].id;
        bool isSoft = JsonArrayContainsId(softArr, aid);
        bool isHard = JsonArrayContainsId(hardArr, aid);

        sAchievements[i].unlocked = isSoft || isHard;
        sAchievements[i].hardcoreUnlocked = isHard;
    }
}

/* Background unlock queue */
#define MAX_UNLOCK_QUEUE 16
static uint32_t sUnlockQueue[MAX_UNLOCK_QUEUE];
static int sUnlockQueueCount = 0;
static LightLock sUnlockQueueLock;
static bool sLockInitialized = false;

static void QueueUnlockId(uint32_t achId) {
    if (!sLockInitialized) {
        LightLock_Init(&sUnlockQueueLock);
        sLockInitialized = true;
    }
    LightLock_Lock(&sUnlockQueueLock);
    if (sUnlockQueueCount < MAX_UNLOCK_QUEUE) {
        /* Avoid duplicate entries */
        for (int i = 0; i < sUnlockQueueCount; ++i) {
            if (sUnlockQueue[i] == achId) {
                LightLock_Unlock(&sUnlockQueueLock);
                return;
            }
        }
        sUnlockQueue[sUnlockQueueCount++] = achId;
    }
    LightLock_Unlock(&sUnlockQueueLock);
}

/* Award achievement network thread */
static void RA_AwardThread(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t toAward = 0;
        if (!sLockInitialized) {
            LightLock_Init(&sUnlockQueueLock);
            sLockInitialized = true;
        }
        LightLock_Lock(&sUnlockQueueLock);
        if (sUnlockQueueCount > 0) {
            toAward = sUnlockQueue[0];
            for (int i = 1; i < sUnlockQueueCount; ++i) {
                sUnlockQueue[i - 1] = sUnlockQueue[i];
            }
            sUnlockQueueCount--;
        }
        LightLock_Unlock(&sUnlockQueueLock);

        if (toAward == 0) {
            svcSleepThread(50000000ULL); /* Sleep 50ms */
            continue;
        }

        if (sRAUsername[0] != '\0' && sRAToken[0] != '\0') {
            char encUser[128] = { 0 };
            char encTok[128] = { 0 };
            UrlEncode(sRAUsername, encUser, sizeof(encUser));
            UrlEncode(sRAToken, encTok, sizeof(encTok));

            char url[768];
            char respBuf[1024] = { 0 };
            snprintf(url, sizeof(url),
                     "https://retroachievements.org/dorequest.php?r=awardachievement&u=%s&t=%s&a=%lu&h=%d",
                     encUser, encTok, (unsigned long)toAward, sRAHardcore ? 1 : 0);

            int res = HttpPerformGet(url, respBuf, sizeof(respBuf));
            if (res <= 0) {
                snprintf(url, sizeof(url),
                         "http://retroachievements.org/dorequest.php?r=awardachievement&u=%s&t=%s&a=%lu&h=%d",
                         encUser, encTok, (unsigned long)toAward, sRAHardcore ? 1 : 0);
                res = HttpPerformGet(url, respBuf, sizeof(respBuf));
            }

            FILE* logFile = fopen("sdmc:/3ds/Metroid Zero Mission 3DS/retroachievements.log", "a");
            if (logFile) {
                fprintf(logFile, "AWARD ATTEMPT: AchID=%lu, Hardcore=%d, Res=%d, Resp='%s'\n\n",
                        (unsigned long)toAward, sRAHardcore ? 1 : 0, res, respBuf);
                fclose(logFile);
            }
        }
    }
}

static bool sAwardThreadStarted = false;
static void EnsureAwardThread(void) {
    if (!sAwardThreadStarted) {
        s32 prio = 0x32;
        threadCreate(RA_AwardThread, NULL, 32 * 1024, prio, -1, true);
        sAwardThreadStarted = true;
    }
}

/* Background Async Login Worker */
static void RA_NetworkLoginThread(void* arg) {
    (void)arg;
    sRAStatus = RA_STATUS_CONNECTING;
    snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "CONNECTING TO RA...");

    char url[768];
    char respBuf[2048] = { 0 };

    if (sPendingLogin && sPendingPassword[0] != '\0') {
        char encUser[128] = { 0 };
        char encPass[128] = { 0 };
        UrlEncode(sRAUsername, encUser, sizeof(encUser));
        UrlEncode(sPendingPassword, encPass, sizeof(encPass));
        memset(sPendingPassword, 0, sizeof(sPendingPassword));
        sPendingLogin = false;

        /* Try HTTPS first; if certificate or cipher fails, try HTTP */
        snprintf(url, sizeof(url), "https://retroachievements.org/dorequest.php?r=login&u=%s&p=%s",
                 encUser, encPass);

        int res = HttpPerformGet(url, respBuf, sizeof(respBuf));
        if (res <= 0) {
            /* Fallback to http if TLS handshake was rejected by modern TLS cipher suite */
            snprintf(url, sizeof(url), "http://retroachievements.org/dorequest.php?r=login&u=%s&p=%s",
                     encUser, encPass);
            res = HttpPerformGet(url, respBuf, sizeof(respBuf));
        }

        FILE* logFile = fopen("sdmc:/3ds/Metroid Zero Mission 3DS/retroachievements.log", "a");
        if (logFile) {
            fprintf(logFile, "LOGIN ATTEMPT: User='%s', HttpResult=%d, LastMsg='%s'\n", encUser, res, sLastStatusMsg);
            fprintf(logFile, "RESPONSE (len=%lu): '%s'\n\n", (unsigned long)strlen(respBuf), respBuf);
            fclose(logFile);
        }

        if (res > 0) {
            /* Parse JSON response: {"Success":true,"User":"...","Token":"..."} or {"Success":false,"Error":"..."} */
            if (strstr(respBuf, "\"Success\":true") != NULL || strstr(respBuf, "\"Success\": true") != NULL) {
                char* tokPtr = strstr(respBuf, "\"Token\":\"");
                if (tokPtr) {
                    tokPtr += 9;
                    char* endPtr = strchr(tokPtr, '\"');
                    if (endPtr) {
                        size_t len = endPtr - tokPtr;
                        if (len < sizeof(sRAToken)) {
                            memcpy(sRAToken, tokPtr, len);
                            sRAToken[len] = '\0';
                        }
                    }
                }
                sRAStatus = RA_STATUS_CONNECTED;
                snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "LOGIN OK (TOKEN RECEIVED)");
                extern void Port_Config_Save(void);
                Port_Config_Save();

                /* Sync user unlocked achievements */
                FetchUserUnlocks();
            } else {
                sRAStatus = RA_STATUS_ERROR;
                sRAToken[0] = '\0';
                /* Extract server error message if present */
                char* errPtr = strstr(respBuf, "\"Error\":\"");
                if (errPtr) {
                    errPtr += 9;
                    char* endErr = strchr(errPtr, '\"');
                    if (endErr) {
                        size_t elen = endErr - errPtr;
                        if (elen > 40) elen = 40;
                        memcpy(sLastStatusMsg, errPtr, elen);
                        sLastStatusMsg[elen] = '\0';
                    } else {
                        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "SERVER: INVALID CREDENTIALS");
                    }
                } else {
                    snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "SERVER: %s", (respBuf[0] != '\0') ? respBuf : "NO DATA");
                    sLastStatusMsg[45] = '\0';
                }
            }
        } else {
            sRAStatus = RA_STATUS_OFFLINE;
        }
    } else if (sRAToken[0] != '\0' && sRAUsername[0] != '\0') {
        /* Validate existing token */
        char encUser[128] = { 0 };
        char encTok[128] = { 0 };
        UrlEncode(sRAUsername, encUser, sizeof(encUser));
        UrlEncode(sRAToken, encTok, sizeof(encTok));

        snprintf(url, sizeof(url), "https://retroachievements.org/dorequest.php?r=login&u=%s&t=%s",
                 encUser, encTok);
        int res = HttpPerformGet(url, respBuf, sizeof(respBuf));
        if (res <= 0) {
            snprintf(url, sizeof(url), "http://retroachievements.org/dorequest.php?r=login&u=%s&t=%s",
                     encUser, encTok);
            res = HttpPerformGet(url, respBuf, sizeof(respBuf));
        }

        if (res > 0 && (strstr(respBuf, "\"Success\":true") != NULL || strstr(respBuf, "\"Success\": true") != NULL)) {
            sRAStatus = RA_STATUS_CONNECTED;
            snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "CONNECTED");
            FetchUserUnlocks();
        } else {
            sRAStatus = RA_STATUS_OFFLINE;
            snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "OFFLINE / UNREACHABLE");
        }
    } else {
        sRAStatus = RA_STATUS_OFFLINE;
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "NO CREDENTIALS SET");
    }
}

static void TriggerBackgroundValidation(void) {
    if (!sRAEnabled) {
        sRAStatus = RA_STATUS_DISABLED;
        return;
    }
    if (sRAUsername[0] == '\0') {
        sRAStatus = RA_STATUS_OFFLINE;
        return;
    }

    sRAStatus = RA_STATUS_CONNECTING;
    s32 prio = 0x31;
    sNetworkThread = threadCreate(RA_NetworkLoginThread, NULL, 32 * 1024, prio, -1, true);
}

void Port_RA_Init(void) {
    /* Always load last known cached achievement list from SD */
    if (sAchievementCount == 0) {
        FILE* cacheFile = fopen("sdmc:/3ds/Metroid Zero Mission 3DS/ra_cache_534.json", "rb");
        if (cacheFile) {
            static char sInitBuf[65536];
            size_t bytes = fread(sInitBuf, 1, sizeof(sInitBuf) - 1, cacheFile);
            sInitBuf[bytes] = '\0';
            fclose(cacheFile);
            ParsePatchAchievements(sInitBuf);
        }
    }

    EnsureAwardThread();

    if (sRAEnabled && sRAUsername[0] != '\0' && sRAToken[0] != '\0') {
        TriggerBackgroundValidation();
    } else if (sRAEnabled && sRAUsername[0] != '\0') {
        sRAStatus = RA_STATUS_OFFLINE;
    } else {
        sRAStatus = RA_STATUS_DISABLED;
    }
}

void Port_RA_Shutdown(void) {
    if (sHttpInitialized) {
        httpcExit();
        sHttpInitialized = false;
    }
}

void Port_RA_Update(void) {
    if (sToast.active) {
        if (sToast.timer > 0) {
            --sToast.timer;
        } else {
            sToast.active = false;
        }
    }
}

/* ========================================================================= */
/* MEMORY TRIGGER EVALUATION ENGINE (Direct C Symbol & GBA Memory Mapper)    */
/* ========================================================================= */

#pragma pack(push, 1)
struct RawEquipment {
    uint16_t maxEnergy;
    uint16_t maxMissiles;
    uint8_t maxSuperMissiles;
    uint8_t maxPowerBombs;
    uint16_t currentEnergy;
    uint16_t currentMissiles;
    uint8_t currentSuperMissiles;
    uint8_t currentPowerBombs;
    uint8_t beamBombs;
    uint8_t beamBombsActivation;
    uint8_t suitMisc;
    uint8_t suitMiscActivation;
    uint8_t downloadedMapStatus;
    uint8_t lowHealth;
    uint8_t suitType;
    uint8_t grabbedByMetroid;
};
#pragma pack(pop)

extern struct RawEquipment gEquipment;
extern uint8_t gCurrentArea;
extern uint8_t gCurrentRoom;
extern int16_t gMainGameMode;
extern uint8_t gDifficulty;
extern uint8_t gMinimapX;
extern uint8_t gMinimapY;
extern uint8_t gDemoState;
extern uint8_t gEventsTriggered[];
extern uint8_t gEwram[0x40000];

static struct RawEquipment sPrevEquipment;
static uint8_t sPrevArea = 0;
static uint8_t sPrevRoom = 0;
static int16_t sPrevGameMode = 0;
static uint8_t sPrevDifficulty = 0;
static uint8_t sPrevDemoState = 0;
static uint8_t sPrevEwram[0x40000];
static bool sHasPrevRam = false;

/* Read a value from GBA memory address */
static uint32_t ReadRamValue(uint32_t addr, char sizePrefix, bool readDelta) {
    uint32_t val = 0;

    /* 1. Direct variable mappings based on exact GBA IWRAM offsets in Metroid Zero Mission */
    if (addr >= 0x1530 && addr <= 0x1550) {
        /* Equipment struct at 0x03001530 */
        const uint8_t* eqBytes = readDelta ? (const uint8_t*)&sPrevEquipment : (const uint8_t*)&gEquipment;
        uint32_t off = addr - 0x1530;
        if (off < sizeof(struct RawEquipment)) {
            val = eqBytes[off];
            if (sizePrefix == ' ' || sizePrefix == 'W') {
                if (off + 1 < sizeof(struct RawEquipment)) {
                    val |= ((uint32_t)eqBytes[off + 1] << 8);
                }
            }
        }
    } else if (addr == 0x0054) {
        val = readDelta ? sPrevArea : gCurrentArea;
    } else if (addr == 0x0055) {
        val = readDelta ? sPrevRoom : gCurrentRoom;
    } else if (addr == 0x002c) {
        val = readDelta ? sPrevDifficulty : gDifficulty;
    } else if (addr == 0x0c70) {
        val = readDelta ? sPrevGameMode : gMainGameMode;
    } else if (addr == 0x0058) {
        val = gMinimapX;
    } else if (addr == 0x0059) {
        val = gMinimapY;
    } else if (addr == 0x43fe9 || (addr >= 0x8000 && addr == 0x43fe9)) {
        /* Demo state / Demo in progress flag (0x0203bfe9) */
        val = readDelta ? (sPrevDemoState != 0 ? 77 : 0) : (gDemoState != 0 ? 77 : 0);
    } else if (addr >= 0x3fe00 && addr < 0x3ff00) {
        /* Events triggered buffer (0x02037e00 in GBA EWRAM) */
        uint32_t evOff = addr - 0x3fe00;
        if (evOff < 32) {
            val = gEventsTriggered[evOff];
        }
    } else {
        /* General EWRAM fallback: RA GBA EWRAM is offset by 0x8000 */
        const uint8_t* ram = readDelta ? sPrevEwram : gEwram;
        uint32_t offset = (addr >= 0x8000) ? (addr - 0x8000) : addr;
        if (offset < 0x40000) {
            val = ram[offset];
            if ((sizePrefix == ' ' || sizePrefix == 'W') && offset + 1 < 0x40000) {
                val |= ((uint32_t)ram[offset + 1] << 8);
            }
        }
    }

    /* Bitwise extractions */
    switch (sizePrefix) {
        case 'L': return val & 0x0F;
        case 'U': return (val >> 4) & 0x0F;
        case 'M': return (val >> 0) & 1;
        case 'N': return (val >> 1) & 1;
        case 'O': return (val >> 2) & 1;
        case 'P': return (val >> 3) & 1;
        case 'Q': return (val >> 4) & 1;
        case 'R': return (val >> 5) & 1;
        case 'S': return (val >> 6) & 1;
        case 'T': return (val >> 7) & 1;
        default: return val;
    }
}

/* Parse a single operand (e.g., 0xH000054, d0xS00153e, 100, 0x12) */
static const char* ParseOperand(const char* p, uint32_t* outVal) {
    bool isDelta = false;
    if (*p == 'd' || *p == 'D') {
        isDelta = true;
        p++;
    }

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        char sizePrefix = 'H';
        if (*p >= 'A' && *p <= 'Z' && *p != 'A' && *p != 'B' && *p != 'C' && *p != 'D' && *p != 'E' && *p != 'F') {
            sizePrefix = *p++;
        } else if (*p == ' ') {
            sizePrefix = *p++;
        }

        char* endP = NULL;
        uint32_t addr = (uint32_t)strtoul(p, &endP, 16);
        p = endP;
        *outVal = ReadRamValue(addr, sizePrefix, isDelta);
    } else {
        char* endP = NULL;
        *outVal = (uint32_t)strtoul(p, &endP, 10);
        p = endP;
    }
    return p;
}

/* Evaluate a single condition in the format: [Flag:]Operand Operator Operand [.Hits.] */
static bool EvaluateSingleCondition(const char* condStr, char* outFlag, uint32_t* outAddVal) {
    const char* p = condStr;
    char flag = '\0';
    if (p[0] != '\0' && p[1] == ':') {
        flag = p[0];
        p += 2;
    }
    if (outFlag) *outFlag = flag;

    uint32_t leftVal = 0;
    p = ParseOperand(p, &leftVal);

    /* Read operator */
    char op[3] = { 0 };
    int opLen = 0;
    while (*p == '=' || *p == '!' || *p == '<' || *p == '>' || *p == '<' || *p == '>') {
        if (opLen < 2) op[opLen++] = *p;
        p++;
    }

    uint32_t rightVal = 0;
    p = ParseOperand(p, &rightVal);

    bool result = false;
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
        result = (leftVal == rightVal);
    } else if (strcmp(op, "!=") == 0) {
        result = (leftVal != rightVal);
    } else if (strcmp(op, "<") == 0) {
        result = (leftVal < rightVal);
    } else if (strcmp(op, "<=") == 0) {
        result = (leftVal <= rightVal);
    } else if (strcmp(op, ">") == 0) {
        result = (leftVal > rightVal);
    } else if (strcmp(op, ">=") == 0) {
        result = (leftVal >= rightVal);
    }

    if (flag == 'A' && outAddVal) {
        *outAddVal += leftVal;
    }

    return result;
}

/* Evaluates a single condition group (separated by '_') */
static bool EvaluateConditionGroup(const char* groupStr) {
    char buf[512];
    strncpy(buf, groupStr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* saveptr = NULL;
    char* token = strtok_r(buf, "_", &saveptr);
    uint32_t accumulatedSum = 0;
    bool hasReset = false;
    bool resetTriggered = false;

    while (token != NULL) {
        char flag = '\0';
        bool condPassed = EvaluateSingleCondition(token, &flag, &accumulatedSum);

        if (flag == 'R') {
            hasReset = true;
            if (condPassed) {
                resetTriggered = true;
            }
        } else if (flag == 'A') {
            /* AddSource: accumulated in accumulatedSum */
        } else if (!condPassed) {
            return false;
        }

        token = strtok_r(NULL, "_", &saveptr);
    }

    if (hasReset && resetTriggered) {
        return false;
    }

    return true;
}

/* Full trigger evaluation with Alt Groups support (standalone 'S' separator) */
static bool EvaluateTriggerExpression(const char* memAddr) {
    if (!memAddr || memAddr[0] == '\0') return false;

    /* Check if expression contains Alt Groups (e.g. CoreGroup S AltGroup1 S AltGroup2) */
    /* An Alt Group separator in RA is an uppercase 'S' that is NOT part of a hex token (like 0xS or d0xS) */
    char fullExpr[512];
    strncpy(fullExpr, memAddr, sizeof(fullExpr) - 1);
    fullExpr[sizeof(fullExpr) - 1] = '\0';

    char* groups[16];
    int groupCount = 0;
    groups[groupCount++] = fullExpr;

    for (char* p = fullExpr; *p != '\0'; ++p) {
        if (*p == 'S') {
            bool isPrefix = false;
            if (p > fullExpr && *(p - 1) == 'x') isPrefix = true;
            if (p > fullExpr + 1 && *(p - 2) == '0' && *(p - 1) == 'x') isPrefix = true;

            if (!isPrefix) {
                *p = '\0';
                if (groupCount < 16) {
                    groups[groupCount++] = p + 1;
                }
            }
        }
    }

    /* First group is Core Group */
    if (!EvaluateConditionGroup(groups[0])) {
        return false;
    }

    /* If no Alt Groups exist, Core Group passing is sufficient */
    if (groupCount == 1) {
        return true;
    }

    /* If Alt Groups exist, at least one Alt Group must pass */
    for (int i = 1; i < groupCount; ++i) {
        if (EvaluateConditionGroup(groups[i])) {
            return true;
        }
    }

    return false;
}

void Port_RA_TriggerUnlock(uint32_t achIndex) {
    if (achIndex >= sAchievementCount) return;
    RetroAchievementItem* item = &sAchievements[achIndex];

    if (item->unlocked && (!sRAHardcore || item->hardcoreUnlocked)) {
        return; /* Already unlocked */
    }

    item->unlocked = true;
    if (sRAHardcore) {
        item->hardcoreUnlocked = true;
    }

    FILE* logFile = fopen("sdmc:/3ds/Metroid Zero Mission 3DS/retroachievements.log", "a");
    if (logFile) {
        fprintf(logFile, "TRIGGER FIRED! AchID=%lu, Title='%s', Points=%lu\n",
                (unsigned long)item->id, item->title, (unsigned long)item->points);
        fclose(logFile);
    }

    /* Trigger visual popup toast */
    sToast.active = true;
    sToast.timer = 180; /* 3 seconds at 60fps */
    strncpy(sToast.title, item->title, sizeof(sToast.title) - 1);
    sToast.points = item->points;

    /* Queue network award request */
    QueueUnlockId(item->id);
}

void Port_RA_EvaluateTriggers(void) {
    if (!sRAEnabled || sAchievementCount == 0) return;

    if (!sHasPrevRam) {
        memcpy(&sPrevEquipment, &gEquipment, sizeof(sPrevEquipment));
        sPrevArea = gCurrentArea;
        sPrevRoom = gCurrentRoom;
        sPrevGameMode = gMainGameMode;
        sPrevDifficulty = gDifficulty;
        sPrevDemoState = gDemoState;
        memcpy(sPrevEwram, gEwram, sizeof(gEwram));
        sHasPrevRam = true;
        return;
    }

    /* Debug log when suitMisc or equipment changes */
    if (sPrevEquipment.suitMisc != gEquipment.suitMisc) {
        FILE* logFile = fopen("sdmc:/3ds/Metroid Zero Mission 3DS/retroachievements.log", "a");
        if (logFile) {
            fprintf(logFile, "ITEM CHANGE: prevSuitMisc=0x%02X, nowSuitMisc=0x%02X, Area=%d, Room=%d, GM=%d\n",
                    sPrevEquipment.suitMisc, gEquipment.suitMisc, gCurrentArea, gCurrentRoom, gMainGameMode);
            for (uint32_t j = 0; j < sAchievementCount; ++j) {
                if (sAchievements[j].id == 5760) {
                    fprintf(logFile, "  5760 (MorphBall): unlocked=%d, hardUnlocked=%d, memAddr='%s'\n",
                            sAchievements[j].unlocked, sAchievements[j].hardcoreUnlocked, sAchievements[j].memAddr);
                    bool evalRes = EvaluateTriggerExpression(sAchievements[j].memAddr);
                    fprintf(logFile, "  5760 EvalResult = %d\n", evalRes ? 1 : 0);
                }
            }
            fclose(logFile);
        }
    }

    for (uint32_t i = 0; i < sAchievementCount; ++i) {
        RetroAchievementItem* item = &sAchievements[i];
        if (item->unlocked && (!sRAHardcore || item->hardcoreUnlocked)) {
            continue;
        }

        if (EvaluateTriggerExpression(item->memAddr)) {
            Port_RA_TriggerUnlock(i);
        }
    }

    /* Update previous frame snapshot */
    memcpy(&sPrevEquipment, &gEquipment, sizeof(sPrevEquipment));
    sPrevArea = gCurrentArea;
    sPrevRoom = gCurrentRoom;
    sPrevGameMode = gMainGameMode;
    sPrevDifficulty = gDifficulty;
    sPrevDemoState = gDemoState;
    memcpy(sPrevEwram, gEwram, sizeof(gEwram));
}

bool Port_RA_IsEnabled(void) { return sRAEnabled; }
void Port_RA_SetEnabled(bool enabled) {
    sRAEnabled = enabled;
    if (sRAEnabled) {
        if (sRAUsername[0] != '\0' && sRAToken[0] != '\0') {
            TriggerBackgroundValidation();
        } else {
            sRAStatus = RA_STATUS_OFFLINE;
        }
    } else {
        sRAStatus = RA_STATUS_DISABLED;
    }
}

bool Port_RA_IsHardcore(void) { return sRAHardcore; }
void Port_RA_SetHardcore(bool hardcore) { sRAHardcore = hardcore; }

bool Port_RA_GetNotificationSound(void) { return sRANotifSound; }
void Port_RA_SetNotificationSound(bool sound) { sRANotifSound = sound; }

const char* Port_RA_GetUsername(void) { return sRAUsername; }
void Port_RA_SetUsername(const char* username) {
    if (username) {
        strncpy(sRAUsername, username, sizeof(sRAUsername) - 1);
        sRAUsername[sizeof(sRAUsername) - 1] = '\0';
    }
}

const char* Port_RA_GetToken(void) { return sRAToken; }
void Port_RA_SetToken(const char* token) {
    if (token) {
        strncpy(sRAToken, token, sizeof(sRAToken) - 1);
        sRAToken[sizeof(sRAToken) - 1] = '\0';
    }
}

void Port_RA_PromptLogin(void) {
    SwkbdState swkbd;
    char inputUser[64] = { 0 };
    char inputPass[64] = { 0 };

    /* 1. Prompt Username */
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, "Enter RetroAchievements Username");
    if (sRAUsername[0] != '\0') {
        swkbdSetInitialText(&swkbd, sRAUsername);
    }
    SwkbdButton btn = swkbdInputText(&swkbd, inputUser, sizeof(inputUser));
    if (btn != SWKBD_BUTTON_CONFIRM || inputUser[0] == '\0') {
        return; /* User canceled */
    }

    /* 2. Prompt Password (masked keyboard) */
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdSetHintText(&swkbd, "Enter RetroAchievements Password");
    btn = swkbdInputText(&swkbd, inputPass, sizeof(inputPass));
    if (btn != SWKBD_BUTTON_CONFIRM || inputPass[0] == '\0') {
        return; /* User canceled password -> do not log in */
    }

    /* Store username & password for background thread */
    Port_RA_SetUsername(inputUser);
    strncpy(sPendingPassword, inputPass, sizeof(sPendingPassword) - 1);
    sPendingPassword[sizeof(sPendingPassword) - 1] = '\0';
    sPendingLogin = true;

    sRAEnabled = true;
    TriggerBackgroundValidation();
}

RetroAchievementsStatus Port_RA_GetStatus(void) { return sRAStatus; }

const char* Port_RA_GetLastDebugLog(void) {
    return sLastStatusMsg[0] != '\0' ? sLastStatusMsg : "NO NETWORK LOG";
}

const char* Port_RA_GetStatusString(int lang) {
    if (lang == 6) { /* ES */
        switch (sRAStatus) {
            case RA_STATUS_DISABLED: return "DESACTIVADO";
            case RA_STATUS_OFFLINE: return "SIN CONEXION";
            case RA_STATUS_CONNECTING: return "CONECTANDO...";
            case RA_STATUS_CONNECTED: return "CONECTADO";
            case RA_STATUS_ERROR: return "ERROR DE LOGIN";
        }
    }
    switch (sRAStatus) {
        case RA_STATUS_DISABLED: return "DISABLED";
        case RA_STATUS_OFFLINE: return "OFFLINE";
        case RA_STATUS_CONNECTING: return "CONNECTING...";
        case RA_STATUS_CONNECTED: return "CONNECTED";
        case RA_STATUS_ERROR: return "LOGIN ERROR";
    }
    return "UNKNOWN";
}

uint32_t Port_RA_GetAchievementCount(void) {
    return sAchievementCount;
}

uint32_t Port_RA_GetUnlockedCount(void) {
    uint32_t c = 0;
    for (size_t i = 0; i < sAchievementCount; ++i) {
        if (sAchievements[i].unlocked) ++c;
    }
    return c;
}

uint32_t Port_RA_GetHardcoreUnlockedCount(void) {
    uint32_t c = 0;
    for (size_t i = 0; i < sAchievementCount; ++i) {
        if (sAchievements[i].hardcoreUnlocked) ++c;
    }
    return c;
}

uint32_t Port_RA_GetTotalPoints(void) {
    uint32_t p = 0;
    for (size_t i = 0; i < sAchievementCount; ++i) {
        p += sAchievements[i].points;
    }
    return p;
}

uint32_t Port_RA_GetUnlockedPoints(void) {
    uint32_t p = 0;
    for (size_t i = 0; i < sAchievementCount; ++i) {
        if (sAchievements[i].unlocked) p += sAchievements[i].points;
    }
    return p;
}

const RetroAchievementItem* Port_RA_GetAchievement(uint32_t index) {
    if (index < sAchievementCount) {
        return &sAchievements[index];
    }
    return NULL;
}

/* Toast Graphic & Text Drawing */
static const uint8_t* GetToastGlyph(char c) {
    static const uint8_t digits[10][7] = {
        { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, /* 0 */
        { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* 1 */
        { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, /* 2 */
        { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E }, /* 3 */
        { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, /* 4 */
        { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E }, /* 5 */
        { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E }, /* 6 */
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
        { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, /* 8 */
        { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E }, /* 9 */
    };
    static const uint8_t letters[26][7] = {
        { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* A */
        { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, /* B */
        { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, /* C */
        { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C }, /* D */
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, /* E */
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, /* F */
        { 0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0F }, /* G */
        { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* H */
        { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* I */
        { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, /* J */
        { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, /* K */
        { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, /* L */
        { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, /* M */
        { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 }, /* N */
        { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, /* O */
        { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, /* P */
        { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, /* Q */
        { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, /* R */
        { 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E }, /* S */
        { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, /* T */
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, /* U */
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, /* V */
        { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, /* W */
        { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, /* X */
        { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, /* Y */
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }, /* Z */
    };
    static const uint8_t colon[7]   = { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 };
    static const uint8_t dot[7]     = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C };
    static const uint8_t exclam[7]  = { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 };
    static const uint8_t plus[7]    = { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 };
    static const uint8_t lbracket[7]= { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E };
    static const uint8_t rbracket[7]= { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E };

    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    if (c == ':') return colon;
    if (c == '.') return dot;
    if (c == '!') return exclam;
    if (c == '+') return plus;
    if (c == '(' || c == '[') return lbracket;
    if (c == ')' || c == ']') return rbracket;
    return NULL;
}

static void DrawToastText(float x, float y, const char* text, uint32_t color) {
    float scale = 1.0f;
    float charW = 6.0f;
    for (; *text; ++text, x += charW) {
        const uint8_t* glyph = GetToastGlyph(*text);
        if (!glyph) continue;
        for (int row = 0; row < 7; ++row) {
            float py = y + (float)row * scale;
            uint8_t rowVal = glyph[row];
            for (int col = 0; col < 5;) {
                if ((rowVal & (1u << (4 - col))) == 0) {
                    ++col;
                    continue;
                }
                int end = col + 1;
                while (end < 5 && (rowVal & (1u << (4 - end))) != 0) ++end;
                C2D_DrawRectSolid(x + (float)col * scale, py, 0.96f,
                                  (float)(end - col) * scale, scale, color);
                col = end;
            }
        }
    }
}

void Port_RA_RenderToastOverlay(void) {
    if (!sToast.active) return;

    /* Visual pop-in slide animation */
    float boxW = 280.0f;
    float boxH = 32.0f;
    float boxX = (320.0f - boxW) / 2.0f;
    float boxY = 6.0f;

    /* Outer golden frame */
    C2D_DrawRectSolid(boxX, boxY, 0.94f, boxW, boxH, sRAHardcore ? C2D_Color32(255, 215, 0, 255) : C2D_Color32(80, 220, 120, 255));
    /* Dark background fill */
    C2D_DrawRectSolid(boxX + 1.0f, boxY + 1.0f, 0.95f, boxW - 2.0f, boxH - 2.0f, C2D_Color32(14, 20, 32, 250));

    /* Trophy icon on the left */
    float iconX = boxX + 6.0f;
    float iconY = boxY + 8.0f;
    uint32_t goldCol = C2D_Color32(255, 215, 0, 255);
    C2D_DrawRectSolid(iconX + 2.0f, iconY, 0.96f, 10.0f, 6.0f, goldCol);
    C2D_DrawRectSolid(iconX + 5.0f, iconY + 6.0f, 0.96f, 4.0f, 5.0f, goldCol);
    C2D_DrawRectSolid(iconX + 3.0f, iconY + 11.0f, 0.96f, 8.0f, 3.0f, goldCol);

    /* Text */
    DrawToastText(boxX + 24.0f, boxY + 5.0f, sRAHardcore ? "! LOGRO DESBLOQUEADO (HARDCORE) !" : "! LOGRO DESBLOQUEADO !", goldCol);
    
    char titleBuf[80];
    snprintf(titleBuf, sizeof(titleBuf), "%s (+%lu PTS)", sToast.title, (unsigned long)sToast.points);
    DrawToastText(boxX + 24.0f, boxY + 18.0f, titleBuf, C2D_Color32(255, 255, 255, 255));
}
