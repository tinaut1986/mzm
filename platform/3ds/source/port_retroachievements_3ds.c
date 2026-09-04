/* RetroAchievements for the 3DS port, on top of rcheevos.
 *
 * rcheevos owns everything that used to be hand-written here: the trigger
 * language, the login and session handling, the unlock queue and the
 * hardcore rules.  What is left in this file is the three things only the
 * port can supply -- how to reach the network, where GBA memory really is,
 * and what an unlock should look like on screen -- plus the accessors the
 * bottom-screen UI reads.
 *
 * Threading: every rc_client call is made from the main thread.  HTTP runs
 * on a worker thread, but its responses are queued and handed back to
 * rcheevos from Port_RA_Update, so rc_client callbacks -- including the ones
 * that fire toasts and rebuild the achievement list -- never run off-thread.
 */

#include "port_retroachievements_3ds.h"
#include "port_ra_iwram_map.h"
#include "port_debug_tools.h" /* PORT_DEBUG_TOOLS_ACTIVE */

#include "rc_client.h"

#include <3ds.h>
#include <citro2d.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp, for the title sort */

#include "md5.h"

/* The whole ROM is already in memory; the RA hash for a GBA game is just its
 * MD5, so the game is identified from that rather than from a hardcoded ID
 * and the server decides which set applies. RetroAchievements currently maps
 * four ROM hashes onto one game for this title, so the regional dumps share a
 * set, but nothing here depends on that staying true. See port/port_rom.h. */
extern unsigned char* gRomData;
extern unsigned int gRomSize;

extern uint8_t gEwram[0x40000];

#define RA_LOG_PATH   "sdmc:/3ds/Metroid Zero Mission 3DS/retroachievements.log"

/* Server-call queue sizes; see the async section below. */
#define RA_MAX_PENDING   8
#define RA_REQUEST_MAX   1024
#define RA_RESPONSE_MAX  65536

/* RA's flat view of GBA memory: IWRAM first, then EWRAM from 0x8000. */
#define RA_EWRAM_BASE 0x8000
#define RA_EWRAM_SIZE 0x40000

static rc_client_t* sClient = NULL;

static bool sRAEnabled = false;
/* Off unless the player turned it on last session (restored from config) and
 * the build allows it. Never defaults on: a fresh install is softcore. */
static bool sRAHardcore = false;
static bool sRANotifSound = true;
static char sRAUsername[64] = "";
static char sRAToken[64] = "";
static RetroAchievementsStatus sRAStatus = RA_STATUS_DISABLED;
static char sLastStatusMsg[64] = "";

/* Active toast notification */
static struct {
    bool active;
    uint32_t timer;
    char title[64];
    char badge[16];
    uint32_t points;
    bool hardcore;
} sToast = { false, 0, "", "", 0, false };

/* The classic "got a tank" jingle (MUSIC_GETTING_TANK_JINGLE, see
 * include/constants/audio.h). Played through the decomp sound engine when an
 * unlock fires and the notification sound is enabled. SoundPlay runs on the
 * game-logic thread, which is where this event handler already executes -- the
 * unlock is delivered from rc_client_do_frame inside Port_RA_EvaluateTriggers,
 * called once per frame from agbmain. */
#define RA_UNLOCK_JINGLE 0x3Au
extern void SoundPlay(uint16_t sound);

static void LogLine(const char* fmt, ...) {
    va_list args;
    FILE* file = fopen(RA_LOG_PATH, "a");
    if (!file) {
        return;
    }
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

/* ========================================================================= */
/* GBA memory                                                                */
/* ========================================================================= */

/* EWRAM needs no translation: ewram_symbols.ld already places the decomp's
 * EWRAM globals at their real offsets inside gEwram.  IWRAM does, and the
 * generated table is what makes an IWRAM address mean anything at all --
 * see port_ra_iwram_map.h. */
static const PortRaIwramEntry* FindIwramEntry(uint32_t address) {
    unsigned int low = 0;
    unsigned int high = gPortRaIwramMapCount;

    while (low < high) {
        unsigned int mid = low + (high - low) / 2;
        const PortRaIwramEntry* entry = &gPortRaIwramMap[mid];
        if (address < entry->address) {
            high = mid;
        } else if (address >= (uint32_t)entry->address + entry->size) {
            low = mid + 1;
        } else {
            return entry;
        }
    }
    return NULL;
}

/* Reads are byte at a time on purpose: an rcheevos read can straddle two
 * variables, and a variable the decomp does not model leaves a hole in the
 * middle of one.  Stopping at the first byte that is not backed by anything
 * is what rcheevos expects -- a short read tells it the address is invalid,
 * which is the honest answer. */
static uint32_t ReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client) {
    uint32_t done = 0;
    (void)client;

    while (done < num_bytes) {
        uint32_t at = address + done;

        if (at >= RA_EWRAM_BASE) {
            uint32_t offset = at - RA_EWRAM_BASE;
            if (offset >= RA_EWRAM_SIZE) {
                break;
            }
            buffer[done++] = gEwram[offset];
            continue;
        }

        const PortRaIwramEntry* entry = FindIwramEntry(at);
        if (!entry) {
            break;
        }
        buffer[done++] = ((const uint8_t*)entry->storage)[at - entry->address];
    }

    return done;
}

/* ========================================================================= */
/* HTTP                                                                      */
/* ========================================================================= */

static bool sHttpInitialized = false;

static void EnsureHttpInit(void) {
    Result res;

    if (sHttpInitialized) {
        return;
    }

    res = httpcInit(0x20000); /* 128KB shared memory */
    if (R_SUCCEEDED(res)) {
        sHttpInitialized = true;
        return;
    }

    /* Worth spelling out, because the usual cause is not the network: an
     * installed title only gets http:C if cia/mzm3ds.rsf grants it, and
     * without it this fails on the first request and everything downstream
     * just looks "offline". Under the Homebrew Launcher the .3dsx inherits
     * broader permissions and the same code works, which makes it easy to
     * misread as a CIA-only network fault. */
    LogLine("httpcInit FAILED: 0x%08lX (is http:C granted in cia/mzm3ds.rsf?)",
            (unsigned long)res);
}

/* Extracts just the numeric X.X.X portion from MZM_PORT_VERSION (e.g.
 * "v0.2.2-dev.5+abc123" -> "0.2.2"): RA's hardcore validation requires the
 * User-Agent version to be purely numeric. */
static void GetNumericVersion(char* out, size_t outSize) {
    const char* src = MZM_PORT_VERSION;
    size_t d = 0;

    while (*src && !(*src >= '0' && *src <= '9')) src++;
    while (*src && d + 1 < outSize && ((*src >= '0' && *src <= '9') || *src == '.')) {
        out[d++] = *src++;
    }
    while (d > 0 && out[d - 1] == '.') d--; /* trim trailing dot */
    out[d] = '\0';

    if (out[0] == '\0') {
        strncpy(out, "0.0", outSize - 1);
        out[outSize - 1] = '\0';
    }
}

/* One HTTP request. Returns the number of body bytes, or a negative value.
 * post_data being NULL means GET. */
static int HttpPerformOnce(const char* url, const char* post_data, const char* content_type,
                           char* outBuf, size_t outSize, int* outStatusCode) {
    httpcContext context;
    Result ret;
    u32 statuscode = 0;
    u32 totalDownloaded = 0;
    char versionBuf[16];
    char userAgent[64];

    EnsureHttpInit();
    if (!sHttpInitialized) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "HTTPC INIT FAILED");
        return -1;
    }

    ret = httpcOpenContext(&context, post_data ? HTTPC_METHOD_POST : HTTPC_METHOD_GET, url, 1);
    if (R_FAILED(ret)) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "OPEN CTX ERR: 0x%08lX", (unsigned long)ret);
        return -2;
    }

    httpcSetSSLOpt(&context, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_DISABLED);

    GetNumericVersion(versionBuf, sizeof(versionBuf));
    snprintf(userAgent, sizeof(userAgent), "MZM3DS/%s (Nintendo 3DS)", versionBuf);
    httpcAddRequestHeaderField(&context, "User-Agent", userAgent);
    httpcAddRequestHeaderField(&context, "Accept", "*/*");
    httpcAddRequestHeaderField(&context, "Connection", "close");

    if (post_data) {
        httpcAddRequestHeaderField(&context, "Content-Type",
                                   content_type ? content_type : "application/x-www-form-urlencoded");
        httpcAddPostDataRaw(&context, (u32*)(void*)post_data, (u32)strlen(post_data));
    }

    ret = httpcBeginRequest(&context);
    if (R_FAILED(ret)) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "REQ ERR: 0x%08lX", (unsigned long)ret);
        httpcCloseContext(&context);
        return -3;
    }

    ret = httpcGetResponseStatusCodeTimeout(&context, &statuscode, 15000000000ULL); /* 15s */
    if (R_FAILED(ret) && statuscode == 0) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "RESP ERR: 0x%08lX", (unsigned long)ret);
        httpcCloseContext(&context);
        return -4;
    }
    if (outStatusCode) {
        *outStatusCode = (int)statuscode;
    }

    /* The download loop from devkitPro's own network/http_post example:
     * httpcDownloadData reports how much it wrote and returns
     * DOWNLOADPENDING while more remains.
     *
     * The previous version drove this from httpcGetDownloadSizeState's
     * "downloaded size" instead, which does not track what has been read out
     * of the context, so every response came back as zero bytes -- the server
     * answered 200 with a perfectly good body and rcheevos was handed
     * nothing, reporting "No response" and, in the end, "offline".
     *
     * The body must also be drained completely: httpcCloseContext hangs on a
     * context with content still pending. */
    do {
        u32 readsize = 0;
        u32 space = (u32)(outSize - 1) - totalDownloaded;
        if (space == 0) {
            break;
        }
        ret = httpcDownloadData(&context, (u8*)outBuf + totalDownloaded, space, &readsize);
        totalDownloaded += readsize;
    } while (ret == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);

    outBuf[totalDownloaded] = '\0';
    httpcCloseContext(&context);

    /* An empty body is a failed read, not an answer.
     *
     * This console returns a 200 with nothing behind it for every HTTPS
     * request to retroachievements.org -- the download fails with 0xD8A0A016
     * inside the HTTP module -- while the identical request over plain HTTP
     * returns the full body. The hand-written client reported that as an
     * error, which is what let its HTTP fallback take over; reporting it as
     * "success, zero bytes" instead meant the fallback never ran and rcheevos
     * was handed an empty response.
     *
     * RetroAchievements always answers with a body, so treating a missing one
     * as a transport failure costs nothing. */
    if (totalDownloaded == 0) {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "EMPTY (CODE %lu, 0x%08lX)",
                 (unsigned long)statuscode, (unsigned long)ret);
        return -5;
    }

    return (int)totalDownloaded;
}

/* Same request, but falling back to plain HTTP when TLS fails.
 *
 * The console's TLS stack is old enough that the handshake with
 * retroachievements.org is regularly rejected outright, so an HTTPS-only
 * client reports "offline" on a network that works fine. The hand-written
 * client this module replaced retried over HTTP for exactly that reason;
 * rcheevos always builds https:// URLs, so the retry has to live here.
 *
 * Only transport failures fall back. An HTTP status code -- including a 4xx
 * -- means the server was reached and answered, and rcheevos is entitled to
 * see that answer as-is.
 *
 * Once the fallback has worked the session sticks to HTTP. A rejected
 * handshake costs a timeout, and paying it again on every request would make
 * loading the game and syncing unlocks crawl. */
static bool sTlsUnusable = false;

static int HttpPerform(const char* url, const char* post_data, const char* content_type,
                       char* outBuf, size_t outSize, int* outStatusCode) {
    static const char kHttps[] = "https://";
    static const char kHttp[] = "http://";
    char plainUrl[RA_REQUEST_MAX];
    bool isHttps = (strncmp(url, kHttps, sizeof(kHttps) - 1) == 0);
    int result;

    if (isHttps) {
        /* Built by hand rather than with snprintf("%s"): the length is
         * bounded here, where it is obvious, instead of trusting a caller
         * the compiler cannot see through. */
        const char* rest = url + sizeof(kHttps) - 1;
        size_t restLen = strlen(rest);
        if (restLen > sizeof(plainUrl) - sizeof(kHttp)) {
            restLen = sizeof(plainUrl) - sizeof(kHttp);
        }
        memcpy(plainUrl, kHttp, sizeof(kHttp) - 1);
        memcpy(plainUrl + sizeof(kHttp) - 1, rest, restLen);
        plainUrl[sizeof(kHttp) - 1 + restLen] = '\0';
        if (sTlsUnusable) {
            return HttpPerformOnce(plainUrl, post_data, content_type, outBuf, outSize, outStatusCode);
        }
    }

    result = HttpPerformOnce(url, post_data, content_type, outBuf, outSize, outStatusCode);
    if (result >= 0 || !isHttps) {
        return result;
    }

    LogLine("HTTPS FAILED (%d, %s); retrying over HTTP", result, sLastStatusMsg);
    result = HttpPerformOnce(plainUrl, post_data, content_type, outBuf, outSize, outStatusCode);
    if (result >= 0) {
        LogLine("HTTP WORKS; using it for the rest of this session");
        sTlsUnusable = true;
    }
    return result;
}

/* ========================================================================= */
/* Async server calls                                                        */
/* ========================================================================= */

/* rcheevos hands us a request and a callback to invoke with the response.
 * The request is copied onto a queue, a worker thread performs it, and the
 * finished response goes on a second queue that Port_RA_Update drains on the
 * main thread.  Nothing inside rcheevos is ever touched from the worker. */

typedef struct {
    char url[RA_REQUEST_MAX];
    char post_data[RA_REQUEST_MAX];
    char content_type[64];
    bool has_post;

    rc_client_server_callback_t callback;
    void* callback_data;

    /* Filled in by the worker. */
    char* body;
    int body_length;
    int http_status_code;
} RaServerCall;

static RaServerCall sPending[RA_MAX_PENDING];
static int sPendingCount = 0;
static RaServerCall sCompleted[RA_MAX_PENDING];
static int sCompletedCount = 0;

/* One shared response buffer: the worker handles a single request at a time,
 * and a completed call's body is copied out before the next one starts. */
static char sResponseBuf[RA_RESPONSE_MAX];

static LightLock sQueueLock;
static Thread sWorkerThread = NULL;
static bool sWorkerRunning = false;
static volatile bool sWorkerStop = false;

static void ServerCallWorker(void* arg) {
    (void)arg;

    while (!sWorkerStop) {
        RaServerCall call;
        bool have = false;

        LightLock_Lock(&sQueueLock);
        if (sPendingCount > 0 && sCompletedCount < RA_MAX_PENDING) {
            call = sPending[0];
            memmove(&sPending[0], &sPending[1], (size_t)(sPendingCount - 1) * sizeof(sPending[0]));
            sPendingCount--;
            have = true;
        }
        LightLock_Unlock(&sQueueLock);

        if (!have) {
            svcSleepThread(20000000ULL); /* 20ms */
            continue;
        }

        int status = 0;
        int length = HttpPerform(call.url, call.has_post ? call.post_data : NULL,
                                 call.has_post ? call.content_type : NULL,
                                 sResponseBuf, sizeof(sResponseBuf), &status);

        if (length < 0) {
            /* Let rcheevos decide whether to retry: it treats a client error
             * as "the request never reached the server", which is exactly
             * what a failed handshake or a missing network is. */
            call.body = NULL;
            call.body_length = 0;
            call.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        } else {
            call.body = sResponseBuf;
            call.body_length = length;
            call.http_status_code = status;
        }

        /* Only the part of the URL before '?' is logged: the query string
         * carries the token, and on a login it carries the password. */
        LogLine("REQ %.*s -> http %d, %d bytes",
                (int)strcspn(call.url, "?"), call.url, call.http_status_code, length);

        /* On an error the body says why, and an error body carries no
         * token -- unlike a successful login's, which is never logged. */
        if (length > 0 && (call.http_status_code < 200 || call.http_status_code >= 300)) {
            LogLine("  body: %.200s", sResponseBuf);
        }

        LightLock_Lock(&sQueueLock);
        sCompleted[sCompletedCount++] = call;
        LightLock_Unlock(&sQueueLock);

        /* The body lives in the shared buffer until the main thread has
         * copied it out, so wait for the queue to drain before taking the
         * next request. */
        for (;;) {
            bool drained;
            LightLock_Lock(&sQueueLock);
            drained = (sCompletedCount == 0);
            LightLock_Unlock(&sQueueLock);
            if (drained || sWorkerStop) {
                break;
            }
            svcSleepThread(5000000ULL); /* 5ms */
        }
    }
}

static void EnsureWorkerThread(void) {
    if (sWorkerRunning) {
        return;
    }
    LightLock_Init(&sQueueLock);
    sWorkerStop = false;
    sWorkerThread = threadCreate(ServerCallWorker, NULL, 32 * 1024, 0x31, -1, false);
    sWorkerRunning = (sWorkerThread != NULL);
}

static void ServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback,
                       void* callback_data, rc_client_t* client) {
    RaServerCall call;
    bool queued = false;
    (void)client;

    EnsureWorkerThread();

    memset(&call, 0, sizeof(call));
    snprintf(call.url, sizeof(call.url), "%s", request->url ? request->url : "");
    if (request->post_data && request->post_data[0]) {
        call.has_post = true;
        snprintf(call.post_data, sizeof(call.post_data), "%s", request->post_data);
        snprintf(call.content_type, sizeof(call.content_type), "%s",
                 request->content_type ? request->content_type : "application/x-www-form-urlencoded");
    }
    call.callback = callback;
    call.callback_data = callback_data;

    LightLock_Lock(&sQueueLock);
    if (sPendingCount < RA_MAX_PENDING) {
        sPending[sPendingCount++] = call;
        queued = true;
    }
    LightLock_Unlock(&sQueueLock);

    if (!queued) {
        /* The queue only fills if the network has stalled badly. Report it as
         * a retryable failure rather than dropping the request on the floor,
         * which would leave rcheevos waiting for a response forever. */
        rc_api_server_response_t response;
        memset(&response, 0, sizeof(response));
        response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        callback(&response, callback_data);
    }
}

/* Hands finished responses back to rcheevos on the main thread. */
static void DrainServerResponses(void) {
    for (;;) {
        RaServerCall call;
        static char body[RA_RESPONSE_MAX];
        bool have = false;

        LightLock_Lock(&sQueueLock);
        if (sCompletedCount > 0) {
            call = sCompleted[0];
            if (call.body) {
                int length = call.body_length;
                if (length > (int)sizeof(body) - 1) {
                    length = (int)sizeof(body) - 1;
                }
                memcpy(body, call.body, (size_t)length);
                body[length] = '\0';
                call.body = body;
                call.body_length = length;
            }
            memmove(&sCompleted[0], &sCompleted[1],
                    (size_t)(sCompletedCount - 1) * sizeof(sCompleted[0]));
            sCompletedCount--;
            have = true;
        }
        LightLock_Unlock(&sQueueLock);

        if (!have) {
            return;
        }

        rc_api_server_response_t response;
        memset(&response, 0, sizeof(response));
        response.body = call.body;
        response.body_length = (size_t)call.body_length;
        response.http_status_code = call.http_status_code;
        call.callback(&response, call.callback_data);
    }
}

/* ========================================================================= */
/* Achievement list for the bottom-screen UI                                 */
/* ========================================================================= */

/* Sized for the core set plus its subsets: the server returns every set the
 * game has in one response and rcheevos activates all of them, so this is not
 * just the 62 achievements of the base set. */
#define MAX_RA_ACHIEVEMENTS 256
static RetroAchievementItem sAchievements[MAX_RA_ACHIEVEMENTS];
static uint32_t sAchievementCount = 0;

/* Subsets ("packs"). Most games have only their core set; the server can
 * return several in one response and rcheevos activates every one. 8 is far
 * above anything seen in practice -- overflowing it just stops recording new
 * sets, and every achievement still lists and unlocks normally. */
#define MAX_RA_SUBSETS 8
static RetroAchievementSubset sSubsets[MAX_RA_SUBSETS];
static uint32_t sSubsetCount = 0;

/* Filtered + sorted index into sAchievements, walked by the list UI. Kept
 * separate from the raw array so the counters, toasts and unlock bookkeeping
 * keep seeing every achievement whatever the list is filtered to. */
static uint16_t sView[MAX_RA_ACHIEVEMENTS];
static uint32_t sViewCount = 0;
static uint32_t sViewSubset = 0; /* 0 = every set */
static RetroAchievementSort sViewSort = RA_SORT_DEFAULT;
static bool sViewDescending = false;

static RetroAchievementType TranslateType(uint8_t type) {
    switch (type) {
        case RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION: return RA_ACH_TYPE_PROGRESSION;
        case RC_CLIENT_ACHIEVEMENT_TYPE_WIN:         return RA_ACH_TYPE_WIN_CONDITION;
        case RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE:    return RA_ACH_TYPE_MISSABLE;
        default:                                     return RA_ACH_TYPE_STANDARD;
    }
}

/* Per-subset totals, derived from the snapshot above rather than from
 * rc_client_get_user_subset_summary: the two would have to agree anyway, and
 * computing them here keeps the pack list and the achievement list from ever
 * disagreeing about what a set contains. */
static void RefreshSubsetList(void) {
    rc_client_subset_list_t* list;
    uint32_t i;

    sSubsetCount = 0;
    if (!sClient) return;

    list = rc_client_create_subset_list(sClient);
    if (!list) return;

    for (i = 0; i < list->num_subsets && sSubsetCount < MAX_RA_SUBSETS; ++i) {
        const rc_client_subset_t* source = list->subsets[i];
        RetroAchievementSubset* subset = &sSubsets[sSubsetCount++];
        uint32_t a;

        memset(subset, 0, sizeof(*subset));
        subset->id = source->id;
        snprintf(subset->title, sizeof(subset->title), "%s", source->title ? source->title : "");

        for (a = 0; a < sAchievementCount; ++a) {
            const RetroAchievementItem* item = &sAchievements[a];
            if (item->subsetId != subset->id) continue;
            ++subset->total;
            subset->totalPoints += item->points;
            if (item->unlocked) {
                ++subset->unlocked;
                subset->unlockedPoints += item->points;
            }
            if (item->hardcoreUnlocked) ++subset->hardcoreUnlocked;
        }
    }

    rc_client_destroy_subset_list(list);
}

/* qsort comparators over sView. All three are written ASCENDING and consult
 * sViewDescending themselves rather than having RebuildView reverse the
 * finished array: reversing wholesale would also flip the tiebreak (making
 * equal entries shuffle rather than hold still) and, for the recent order,
 * would drag every still-locked achievement to the top.
 *
 * The tiebreak is the entry's position in sAchievements, and it always runs
 * ascending -- qsort is not stable, so without it two equal-points entries
 * could swap places between rebuilds and make the list look like it shuffles
 * itself. */
static int ApplyDir(int cmp) { return sViewDescending ? -cmp : cmp; }

static int CompareByTitle(const void* a, const void* b) {
    uint16_t ia = *(const uint16_t*)a, ib = *(const uint16_t*)b;
    int cmp = strcasecmp(sAchievements[ia].title, sAchievements[ib].title);
    if (cmp != 0) return ApplyDir(cmp);
    return (int)ia - (int)ib;
}

static int CompareByPoints(const void* a, const void* b) {
    uint16_t ia = *(const uint16_t*)a, ib = *(const uint16_t*)b;
    uint32_t pa = sAchievements[ia].points, pb = sAchievements[ib].points;
    if (pa != pb) return ApplyDir((pa < pb) ? -1 : 1);
    return (int)ia - (int)ib;
}

/* By unlock time. The unlocked-before-locked split is NOT reversed by the
 * direction toggle: a still-locked achievement has no unlock time at all, so
 * flipping it would just pile every locked entry on top and bury the thing
 * the order exists to show. Only the time ordering within the unlocked ones
 * flips. */
static int CompareByRecent(const void* a, const void* b) {
    uint16_t ia = *(const uint16_t*)a, ib = *(const uint16_t*)b;
    const RetroAchievementItem* x = &sAchievements[ia];
    const RetroAchievementItem* y = &sAchievements[ib];
    if (x->unlocked != y->unlocked) return x->unlocked ? -1 : 1;
    if (x->unlockTime != y->unlockTime) return ApplyDir((x->unlockTime < y->unlockTime) ? -1 : 1);
    return (int)ia - (int)ib;
}

/* Applies sViewSubset + sViewSort + sViewDescending to sAchievements. A few
 * hundred entries at most, so it just redoes the whole thing whenever any of
 * them changes. */
static void RebuildView(void) {
    uint32_t i;

    sViewCount = 0;
    for (i = 0; i < sAchievementCount; ++i) {
        if (sViewSubset != 0 && sAchievements[i].subsetId != sViewSubset) continue;
        sView[sViewCount++] = (uint16_t)i;
    }

    switch (sViewSort) {
        case RA_SORT_TITLE:  qsort(sView, sViewCount, sizeof(sView[0]), CompareByTitle);  break;
        case RA_SORT_POINTS: qsort(sView, sViewCount, sizeof(sView[0]), CompareByPoints); break;
        case RA_SORT_RECENT: qsort(sView, sViewCount, sizeof(sView[0]), CompareByRecent); break;
        default:
            /* RA_SORT_DEFAULT keeps rcheevos' own grouping, so there is no
             * comparator to flip -- descending is the reversed array. */
            if (sViewDescending) {
                for (i = 0; i < sViewCount / 2u; ++i) {
                    uint16_t tmp = sView[i];
                    sView[i] = sView[sViewCount - 1u - i];
                    sView[sViewCount - 1u - i] = tmp;
                }
            }
            break;
    }
}

/* Snapshots rcheevos' achievement list into the flat array the UI reads.
 * Called whenever something could have changed it -- a game load, an unlock,
 * a hardcore toggle -- rather than every frame, since it allocates. */
static void RefreshAchievementList(void) {
    rc_client_achievement_list_t* list;
    uint32_t bucket;

    if (!sClient) {
        sAchievementCount = 0;
        return;
    }

    list = rc_client_create_achievement_list(sClient, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                             RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) {
        sAchievementCount = 0;
        return;
    }

    sAchievementCount = 0;
    for (bucket = 0; bucket < list->num_buckets; ++bucket) {
        const rc_client_achievement_bucket_t* group = &list->buckets[bucket];
        uint32_t index;

        for (index = 0; index < group->num_achievements; ++index) {
            const rc_client_achievement_t* source = group->achievements[index];
            RetroAchievementItem* item;

            if (sAchievementCount >= MAX_RA_ACHIEVEMENTS) {
                /* Only the list the UI shows is capped -- rcheevos still
                 * evaluates and unlocks every achievement it loaded -- but
                 * say so rather than quietly showing a short list. */
                LogLine("ACHIEVEMENT LIST TRUNCATED AT %d; raise MAX_RA_ACHIEVEMENTS",
                        MAX_RA_ACHIEVEMENTS);
                break;
            }
            item = &sAchievements[sAchievementCount++];

            item->id = source->id;
            item->subsetId = group->subset_id;
            snprintf(item->title, sizeof(item->title), "%s", source->title ? source->title : "");
            snprintf(item->description, sizeof(item->description), "%s",
                     source->description ? source->description : "");
            snprintf(item->badgeName, sizeof(item->badgeName), "%s", source->badge_name);
            /* The trigger expression is rcheevos' business now; the UI only
             * ever displayed it, and nothing reads it. */
            item->memAddr[0] = '\0';
            item->points = source->points;
            item->type = TranslateType(source->type);
            item->unlocked = (source->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE);
            item->hardcoreUnlocked =
                (source->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE) != 0;
            item->unlockTime = (uint32_t)source->unlock_time;
        }
    }

    rc_client_destroy_achievement_list(list);

    RefreshSubsetList();
    RebuildView();
}

/* ========================================================================= */
/* Events                                                                    */
/* ========================================================================= */

static void ShowUnlockToast(const rc_client_achievement_t* achievement) {
    sToast.active = true;
    sToast.timer = 180; /* 3 seconds at 60fps */
    snprintf(sToast.title, sizeof(sToast.title), "%s",
             achievement->title ? achievement->title : "");
    snprintf(sToast.badge, sizeof(sToast.badge), "%s", achievement->badge_name);
    sToast.points = achievement->points;
    /* Snapshot the mode the unlock counted for, so a later hardcore toggle
     * cannot relabel a toast that is still on screen. */
    sToast.hardcore = sRAHardcore;

    if (sRANotifSound) {
        SoundPlay(RA_UNLOCK_JINGLE);
    }
}

static void EventHandler(const rc_client_event_t* event, rc_client_t* client) {
    (void)client;

    switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            LogLine("UNLOCKED: %lu '%s' (+%lu)",
                    (unsigned long)event->achievement->id,
                    event->achievement->title ? event->achievement->title : "",
                    (unsigned long)event->achievement->points);
            ShowUnlockToast(event->achievement);
            RefreshAchievementList();
            break;

        case RC_CLIENT_EVENT_GAME_COMPLETED:
        case RC_CLIENT_EVENT_SUBSET_COMPLETED:
            RefreshAchievementList();
            break;

        case RC_CLIENT_EVENT_SERVER_ERROR:
            snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "%s",
                     event->server_error->error_message ? event->server_error->error_message
                                                        : "SERVER ERROR");
            LogLine("SERVER ERROR (%s): %s", event->server_error->api,
                    event->server_error->error_message ? event->server_error->error_message : "");
            break;

        case RC_CLIENT_EVENT_RESET:
            /* Hardcore was turned on mid-session and rcheevos wants the game
             * restarted. The only path that enables it -- OPTIONS -> hardcore
             * -- already restarts the game on confirmation, so there is
             * nothing to do but record it if it ever fires from elsewhere. */
            LogLine("RESET REQUESTED (hardcore enabled)");
            break;

        case RC_CLIENT_EVENT_DISCONNECTED:
            sRAStatus = RA_STATUS_OFFLINE;
            snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "OFFLINE, UNLOCKS PENDING");
            break;

        case RC_CLIENT_EVENT_RECONNECTED:
            sRAStatus = RA_STATUS_CONNECTED;
            snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "RECONNECTED");
            break;

        default:
            break;
    }
}

/* ========================================================================= */
/* Login and game identification                                             */
/* ========================================================================= */

/* The ROM's MD5, computed once.
 *
 * This is the whole 8MB cartridge, which is far too expensive to redo
 * casually -- hashing it per frame is by itself enough to drop the game from
 * 60 FPS to single digits. The ROM does not change while the game runs. */
static char sRomHash[33] = "";

static const char* RomHash(void) {
    md5_state_t state;
    md5_byte_t digest[16];
    static const char hexDigits[] = "0123456789abcdef";
    int i;

    if (sRomHash[0] != '\0') {
        return sRomHash;
    }
    if (!gRomData || !gRomSize) {
        return NULL;
    }

    md5_init(&state);
    md5_append(&state, (const md5_byte_t*)gRomData, (int)gRomSize);
    md5_finish(&state, digest);

    for (i = 0; i < 16; ++i) {
        sRomHash[i * 2] = hexDigits[(digest[i] >> 4) & 0xF];
        sRomHash[i * 2 + 1] = hexDigits[digest[i] & 0xF];
    }
    sRomHash[32] = '\0';

    LogLine("ROM IDENTIFIED: md5=%s size=%lu", sRomHash, (unsigned long)gRomSize);
    return sRomHash;
}

/* Guards against starting a second load while one is still in flight, and
 * paces the retries after a failure.
 *
 * Without this the per-frame "is the game loaded yet?" check kept starting
 * fresh loads, each aborting the last with RC_ABORTED ("the requested game is
 * no longer active") and pulling the 40KB achievement set down again. The
 * load could never finish, and the console spent every frame on it. */
static bool sLoadInFlight = false;
static int sLoadRetryDelay = 0;
static int sLoadAttempts = 0;

#define RA_LOAD_RETRY_FRAMES 600 /* 10s at 60fps */
#define RA_LOAD_MAX_ATTEMPTS 5

static void LoadGameCallback(int result, const char* error_message, rc_client_t* client, void* userdata) {
    (void)client;
    (void)userdata;

    sLoadInFlight = false;
    sLoadRetryDelay = RA_LOAD_RETRY_FRAMES;

    if (result == RC_OK) {
        const rc_client_game_t* game = rc_client_get_game_info(sClient);
        RefreshAchievementList();
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "GAME %lu, %lu ACHIEVEMENTS",
                 (unsigned long)(game ? game->id : 0), (unsigned long)sAchievementCount);
        LogLine("GAME LOADED: id=%lu title='%s' hash=%s achievements=%lu",
                (unsigned long)(game ? game->id : 0),
                (game && game->title) ? game->title : "",
                (game && game->hash) ? game->hash : "",
                (unsigned long)sAchievementCount);
    } else if (result == RC_NO_GAME_LOADED) {
        /* The hash resolved to no game: an unrecognised ROM, not an error,
         * and retrying will not change the answer. */
        sLoadAttempts = RA_LOAD_MAX_ATTEMPTS;
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "ROM NOT RECOGNISED BY RA");
        LogLine("GAME LOAD: hash not recognised");
    } else {
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "%s", error_message ? error_message : "LOAD FAILED");
        LogLine("GAME LOAD FAILED (%d): %s", result, error_message ? error_message : "");
    }
}

static void BeginLoadGame(void) {
    const char* hash;

    if (!sClient || sLoadInFlight || rc_client_is_game_loaded(sClient)) {
        return;
    }
    if (sLoadRetryDelay > 0 || sLoadAttempts >= RA_LOAD_MAX_ATTEMPTS) {
        return;
    }

    hash = RomHash();
    if (!hash) {
        return; /* the ROM is not mapped yet */
    }

    sLoadInFlight = true;
    sLoadAttempts++;
    rc_client_begin_load_game(sClient, hash, LoadGameCallback, NULL);
}

static void LoginCallback(int result, const char* error_message, rc_client_t* client, void* userdata) {
    (void)client;
    (void)userdata;

    if (result != RC_OK) {
        sRAStatus = (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN)
                        ? RA_STATUS_ERROR : RA_STATUS_OFFLINE;
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "%s", error_message ? error_message : "LOGIN FAILED");
        LogLine("LOGIN FAILED (%d): %s", result, error_message ? error_message : "");
        return;
    }

    const rc_client_user_t* user = rc_client_get_user_info(sClient);
    if (user) {
        snprintf(sRAUsername, sizeof(sRAUsername), "%s", user->username ? user->username : "");
        snprintf(sRAToken, sizeof(sRAToken), "%s", user->token ? user->token : "");
        /* Persist the token so the next launch logs in without a password. */
        extern void Port_Config_Save(void);
        Port_Config_Save();
    }

    sRAStatus = RA_STATUS_CONNECTED;
    snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "CONNECTED");
    LogLine("LOGIN OK: %s", sRAUsername);

    /* A fresh session deserves a fresh set of load attempts. */
    sLoadAttempts = 0;
    sLoadRetryDelay = 0;

    BeginLoadGame();
}

static void BeginLogin(const char* password) {
    if (!sClient) {
        sRAStatus = RA_STATUS_ERROR;
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "RA CLIENT NOT READY");
        return;
    }
    if (sRAUsername[0] == '\0') {
        sRAStatus = RA_STATUS_NO_ACCOUNT;
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "NO USERNAME SET");
        return;
    }

    sRAStatus = RA_STATUS_CONNECTING;
    snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "CONNECTING TO RA...");

    if (password && password[0]) {
        rc_client_begin_login_with_password(sClient, sRAUsername, password, LoginCallback, NULL);
    } else if (sRAToken[0]) {
        rc_client_begin_login_with_token(sClient, sRAUsername, sRAToken, LoginCallback, NULL);
    } else {
        sRAStatus = RA_STATUS_NO_ACCOUNT;
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "NO PASSWORD OR TOKEN; USE LOGIN");
    }
}

/* ========================================================================= */
/* Lifecycle                                                                 */
/* ========================================================================= */

static void ClientLog(const char* message, const rc_client_t* client) {
    (void)client;
    LogLine("rcheevos: %s", message);
}

void Port_RA_Init(void) {
    if (sClient) {
        return;
    }

    /* Stamped so the log identifies the build that wrote it: diagnosing this
     * module from the log is otherwise guesswork about whether the console is
     * even running the version being discussed. */
    LogLine("--- Port_RA_Init, mzm 3DS %s ---", MZM_PORT_VERSION);

    EnsureWorkerThread();

    sClient = rc_client_create(ReadMemory, ServerCall);
    if (!sClient) {
        sRAStatus = RA_STATUS_ERROR;
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "RCHEEVOS INIT FAILED");
        return;
    }

    rc_client_set_event_handler(sClient, EventHandler);
    rc_client_set_hardcore_enabled(sClient, sRAHardcore ? 1 : 0);
    rc_client_enable_logging(sClient, RC_CLIENT_LOG_LEVEL_WARN, ClientLog);

    if (sRAEnabled && sRAUsername[0] != '\0' && sRAToken[0] != '\0') {
        BeginLogin(NULL);
    } else if (sRAEnabled) {
        sRAStatus = RA_STATUS_NO_ACCOUNT;
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "NO SAVED LOGIN; USE LOGIN");
    } else {
        sRAStatus = RA_STATUS_DISABLED;
    }
}

void Port_RA_Shutdown(void) {
    /* Stop the worker before destroying the client: a request in flight is
     * holding an rc_client callback pointer. */
    sWorkerStop = true;
    if (sWorkerThread) {
        threadJoin(sWorkerThread, U64_MAX);
        threadFree(sWorkerThread);
        sWorkerThread = NULL;
        sWorkerRunning = false;
    }

    if (sClient) {
        rc_client_destroy(sClient);
        sClient = NULL;
    }
    if (sHttpInitialized) {
        httpcExit();
        sHttpInitialized = false;
    }
}

void Port_RA_Update(void) {
    DrainServerResponses();

    if (sLoadRetryDelay > 0) {
        --sLoadRetryDelay;
    }

    if (sClient) {
        /* Keeps the session alive and retries pending unlocks while the game
         * itself is not running frames. */
        rc_client_idle(sClient);
    }

    if (sToast.active) {
        if (sToast.timer > 0) {
            --sToast.timer;
        } else {
            sToast.active = false;
        }
    }
}

void Port_RA_EvaluateTriggers(void) {
    if (!sClient || !sRAEnabled) {
        return;
    }

    /* The ROM is not mapped yet when Port_RA_Init runs, so the game is
     * identified on the first frame that has one. */
    if (!rc_client_is_game_loaded(sClient) && sRAStatus == RA_STATUS_CONNECTED) {
        BeginLoadGame();
    }

    rc_client_do_frame(sClient);
}

/* ========================================================================= */
/* Settings and status, read by the bottom-screen UI                         */
/* ========================================================================= */

bool Port_RA_IsEnabled(void) { return sRAEnabled; }

void Port_RA_SetEnabled(bool enabled) {
    sRAEnabled = enabled;
    if (!enabled) {
        sRAStatus = RA_STATUS_DISABLED;
        return;
    }
    if (sRAUsername[0] != '\0' && sRAToken[0] != '\0') {
        BeginLogin(NULL);
    } else {
        /* Turning achievements on is not logging in. Without a saved token
         * there is nothing to connect with, and calling that "offline" sends
         * people hunting for a network fault that does not exist. */
        sRAStatus = RA_STATUS_NO_ACCOUNT;
        snprintf(sLastStatusMsg, sizeof(sLastStatusMsg), "NOT LOGGED IN; USE LOGIN");
    }
}

bool Port_RA_IsHardcore(void) { return sRAHardcore; }

bool Port_RA_HardcoreAllowed(void) {
#ifdef PORT_DEBUG_TOOLS_ACTIVE
    /* This build carries the debug-tools cheat harness (god mode, no-clip);
     * hardcore unlocks must never be possible from it. */
    return false;
#else
    return true;
#endif
}

void Port_RA_SetHardcore(bool hardcore) {
    if (hardcore && !Port_RA_HardcoreAllowed()) {
        hardcore = false;
    }
    sRAHardcore = hardcore;
    if (sClient) {
        rc_client_set_hardcore_enabled(sClient, sRAHardcore ? 1 : 0);
        RefreshAchievementList();
    }
}

bool Port_RA_GetNotificationSound(void) { return sRANotifSound; }
void Port_RA_SetNotificationSound(bool sound) { sRANotifSound = sound; }

const char* Port_RA_GetUsername(void) { return sRAUsername; }

void Port_RA_SetUsername(const char* username) {
    if (username) {
        snprintf(sRAUsername, sizeof(sRAUsername), "%s", username);
    }
}

const char* Port_RA_GetToken(void) { return sRAToken; }

void Port_RA_SetToken(const char* token) {
    if (token) {
        snprintf(sRAToken, sizeof(sRAToken), "%s", token);
    }
}

void Port_RA_PromptLogin(void) {
    SwkbdState swkbd;
    char inputUser[64] = { 0 };
    char inputPass[64] = { 0 };
    SwkbdButton btn;

    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, "Enter RetroAchievements Username");
    if (sRAUsername[0] != '\0') {
        swkbdSetInitialText(&swkbd, sRAUsername);
    }
    btn = swkbdInputText(&swkbd, inputUser, sizeof(inputUser));
    if (btn != SWKBD_BUTTON_CONFIRM || inputUser[0] == '\0') {
        return;
    }

    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdSetHintText(&swkbd, "Enter RetroAchievements Password");
    btn = swkbdInputText(&swkbd, inputPass, sizeof(inputPass));
    if (btn != SWKBD_BUTTON_CONFIRM || inputPass[0] == '\0') {
        return;
    }

    Port_RA_SetUsername(inputUser);
    sRAEnabled = true;
    BeginLogin(inputPass);
    memset(inputPass, 0, sizeof(inputPass));
}

RetroAchievementsStatus Port_RA_GetStatus(void) { return sRAStatus; }

const char* Port_RA_GetLastDebugLog(void) {
    return sLastStatusMsg[0] != '\0' ? sLastStatusMsg : "NO NETWORK LOG";
}

const char* Port_RA_GetStatusString(int lang) {
    switch (lang) {
        case 0: /* JP */
        case 1: /* HIRA */
            switch (sRAStatus) {
                case RA_STATUS_DISABLED: return "むこう";
                case RA_STATUS_OFFLINE: return "オフライン";
                case RA_STATUS_CONNECTING: return "せつぞくちゅう...";
                case RA_STATUS_CONNECTED: return "せつぞくずみ";
                case RA_STATUS_ERROR: return "ログインエラー";
                case RA_STATUS_NO_ACCOUNT: return "みとうろく";
            }
            break;
        case 3: /* DE */
            switch (sRAStatus) {
                case RA_STATUS_DISABLED: return "DEAKTIVIERT";
                case RA_STATUS_OFFLINE: return "OFFLINE";
                case RA_STATUS_CONNECTING: return "VERBINDEN...";
                case RA_STATUS_CONNECTED: return "VERBUNDEN";
                case RA_STATUS_ERROR: return "ANMELDEFEHLER";
                case RA_STATUS_NO_ACCOUNT: return "NICHT ANGEMELDET";
            }
            break;
        case 4: /* FR */
            switch (sRAStatus) {
                case RA_STATUS_DISABLED: return "DESACTIVE";
                case RA_STATUS_OFFLINE: return "HORS LIGNE";
                case RA_STATUS_CONNECTING: return "CONNEXION...";
                case RA_STATUS_CONNECTED: return "CONNECTE";
                case RA_STATUS_ERROR: return "ERREUR LOGIN";
                case RA_STATUS_NO_ACCOUNT: return "NON CONNECTE";
            }
            break;
        case 5: /* IT */
            switch (sRAStatus) {
                case RA_STATUS_DISABLED: return "DISATTIVATO";
                case RA_STATUS_OFFLINE: return "NON IN LINEA";
                case RA_STATUS_CONNECTING: return "CONNESSIONE...";
                case RA_STATUS_CONNECTED: return "CONNESSO";
                case RA_STATUS_ERROR: return "ERRORE LOGIN";
                case RA_STATUS_NO_ACCOUNT: return "NON COLLEGATO";
            }
            break;
        case 6: /* ES */
            switch (sRAStatus) {
                case RA_STATUS_DISABLED: return "DESACTIVADO";
                case RA_STATUS_OFFLINE: return "SIN CONEXION";
                case RA_STATUS_CONNECTING: return "CONECTANDO...";
                case RA_STATUS_CONNECTED: return "CONECTADO";
                case RA_STATUS_ERROR: return "ERROR DE LOGIN";
                case RA_STATUS_NO_ACCOUNT: return "SIN CUENTA";
            }
            break;
        default: /* EN */
            switch (sRAStatus) {
                case RA_STATUS_DISABLED: return "DISABLED";
                case RA_STATUS_OFFLINE: return "OFFLINE";
                case RA_STATUS_CONNECTING: return "CONNECTING...";
                case RA_STATUS_CONNECTED: return "CONNECTED";
                case RA_STATUS_ERROR: return "LOGIN ERROR";
                case RA_STATUS_NO_ACCOUNT: return "NOT LOGGED IN";
            }
            break;
    }
    return "UNKNOWN";
}

uint32_t Port_RA_GetAchievementCount(void) { return sAchievementCount; }

uint32_t Port_RA_GetUnlockedCount(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < sAchievementCount; ++i) {
        if (sAchievements[i].unlocked) ++count;
    }
    return count;
}

uint32_t Port_RA_GetHardcoreUnlockedCount(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < sAchievementCount; ++i) {
        if (sAchievements[i].hardcoreUnlocked) ++count;
    }
    return count;
}

uint32_t Port_RA_GetTotalPoints(void) {
    uint32_t points = 0;
    for (uint32_t i = 0; i < sAchievementCount; ++i) {
        points += sAchievements[i].points;
    }
    return points;
}

uint32_t Port_RA_GetUnlockedPoints(void) {
    uint32_t points = 0;
    for (uint32_t i = 0; i < sAchievementCount; ++i) {
        if (sAchievements[i].unlocked) points += sAchievements[i].points;
    }
    return points;
}

const RetroAchievementItem* Port_RA_GetAchievement(uint32_t index) {
    return (index < sAchievementCount) ? &sAchievements[index] : NULL;
}

uint32_t Port_RA_GetSubsetCount(void) { return sSubsetCount; }

const RetroAchievementSubset* Port_RA_GetSubset(uint32_t index) {
    return (index < sSubsetCount) ? &sSubsets[index] : NULL;
}

void Port_RA_SetListSubset(uint32_t subsetId) {
    if (sViewSubset == subsetId) return;
    sViewSubset = subsetId;
    RebuildView();
}

uint32_t Port_RA_GetListSubset(void) { return sViewSubset; }

void Port_RA_SetListSort(RetroAchievementSort sort) {
    if (sort >= RA_SORT_COUNT || sViewSort == sort) return;
    sViewSort = sort;
    RebuildView();
}

RetroAchievementSort Port_RA_GetListSort(void) { return sViewSort; }

void Port_RA_SetListDescending(bool descending) {
    if (sViewDescending == descending) return;
    sViewDescending = descending;
    RebuildView();
}

bool Port_RA_GetListDescending(void) { return sViewDescending; }

uint32_t Port_RA_GetViewCount(void) { return sViewCount; }

const RetroAchievementItem* Port_RA_GetViewAchievement(uint32_t index) {
    return (index < sViewCount) ? &sAchievements[sView[index]] : NULL;
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

    static const uint8_t n_tilde[7] = { 0x1A, 0x00, 0x11, 0x19, 0x15, 0x13, 0x11 }; /* Ñ */
    static const uint8_t c_cedil[7] = { 0x0E, 0x11, 0x10, 0x10, 0x11, 0x0E, 0x04 }; /* Ç */
    static const uint8_t a_acute[7] = { 0x02, 0x04, 0x0E, 0x11, 0x1F, 0x11, 0x11 }; /* Á */
    static const uint8_t e_acute[7] = { 0x02, 0x04, 0x1F, 0x10, 0x1E, 0x10, 0x1F }; /* É */
    static const uint8_t i_acute[7] = { 0x02, 0x04, 0x0E, 0x04, 0x04, 0x04, 0x0E }; /* Í */
    static const uint8_t o_acute[7] = { 0x02, 0x04, 0x0E, 0x11, 0x11, 0x11, 0x0E }; /* Ó */
    static const uint8_t u_acute[7] = { 0x02, 0x04, 0x11, 0x11, 0x11, 0x11, 0x0E }; /* Ú */
    static const uint8_t a_umlaut[7]= { 0x0A, 0x00, 0x0E, 0x11, 0x1F, 0x11, 0x11 }; /* Ä */
    static const uint8_t o_umlaut[7]= { 0x0A, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E }; /* Ö */
    static const uint8_t u_umlaut[7]= { 0x0A, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0E }; /* Ü */
    static const uint8_t eszett[7]  = { 0x1E, 0x11, 0x1E, 0x11, 0x11, 0x11, 0x1C }; /* ß */

    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    if (c == ':') return colon;
    if (c == '.') return dot;
    if (c == '!') return exclam;
    if (c == '+') return plus;
    if (c == '(' || c == '[') return lbracket;
    if (c == ')' || c == ']') return rbracket;

    switch ((uint8_t)c) {
        case 0xD1: case 0xF1: return n_tilde;
        case 0xC7: case 0xE7: return c_cedil;
        case 0xC1: case 0xE1: case 0xC0: case 0xE0: return a_acute;
        case 0xC9: case 0xE9: case 0xC8: case 0xE8: case 0xCA: case 0xEA: return e_acute;
        case 0xCD: case 0xED: case 0xCC: case 0xEC: return i_acute;
        case 0xD3: case 0xF3: case 0xD2: case 0xF2: return o_acute;
        case 0xDA: case 0xFA: case 0xD9: case 0xF9: return u_acute;
        case 0xC4: case 0xE4: return a_umlaut;
        case 0xD6: case 0xF6: return o_umlaut;
        case 0xDC: case 0xFC: return u_umlaut;
        case 0xDF: return eszett;
        default: break;
    }
    return NULL;
}

static const uint8_t* GetToastUtf8Glyph(const char** textPtr) {
    const uint8_t* s = (const uint8_t*)*textPtr;
    if (!*s) return NULL;
    if (s[0] < 0x80) {
        (*textPtr)++;
        return GetToastGlyph((char)s[0]);
    }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) {
        uint16_t code = (uint16_t)(((s[0] & 0x1F) << 6) | (s[1] & 0x3F));
        *textPtr += 2;
        if (code >= 0x00A0 && code <= 0x00FF) {
            return GetToastGlyph((char)code);
        }
        return NULL;
    }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) { *textPtr += 3; return NULL; }
    if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { *textPtr += 4; return NULL; }
    (*textPtr)++;
    return NULL;
}

static void DrawToastText(float x, float y, const char* text, uint32_t color) {
    float scale = 1.0f;
    float charW = 6.0f;
    while (*text) {
        const uint8_t* glyph = GetToastUtf8Glyph(&text);
        if (!glyph) {
            x += charW;
            continue;
        }
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
        x += charW;
    }
}

void Port_RA_RenderToastOverlay(void) {
    if (!sToast.active) return;

    bool hc = sToast.hardcore;

    float boxW = 280.0f;
    float boxH = 36.0f;
    float boxX = (320.0f - boxW) / 2.0f;
    float boxY = 6.0f;

    /* Hardcore and softcore toasts read differently at a glance: hardcore gets
     * a gold double frame and a red accent stripe, softcore a single green
     * frame and no stripe. */
    uint32_t frameCol = hc ? C2D_Color32(255, 215, 0, 255) : C2D_Color32(80, 220, 120, 255);
    uint32_t accentCol = hc ? C2D_Color32(230, 60, 60, 255) : C2D_Color32(40, 150, 90, 255);

    C2D_DrawRectSolid(boxX, boxY, 0.94f, boxW, boxH, frameCol);
    C2D_DrawRectSolid(boxX + 1.0f, boxY + 1.0f, 0.945f, boxW - 2.0f, boxH - 2.0f, C2D_Color32(14, 20, 32, 250));
    if (hc) {
        /* Inner second frame line, only for hardcore. */
        C2D_DrawRectSolid(boxX + 2.0f, boxY + 2.0f, 0.946f, boxW - 4.0f, 1.0f, frameCol);
        C2D_DrawRectSolid(boxX + 2.0f, boxY + boxH - 3.0f, 0.946f, boxW - 4.0f, 1.0f, frameCol);
    }
    /* Accent stripe down the left edge. */
    C2D_DrawRectSolid(boxX + 2.0f, boxY + 2.0f, 0.95f, 2.0f, boxH - 4.0f, accentCol);

    /* Achievement badge on the left. Falls back to a drawn trophy when the
     * 20x20 pixel copy for this badge is not bundled (see
     * port_ra_badges_data.c). */
    float iconX = boxX + 7.0f;
    float iconY = boxY + 8.0f;
    const uint32_t* badge = Port_RA_GetBadgePixels(sToast.badge);
    if (badge) {
        C2D_DrawRectSolid(iconX - 1.0f, iconY - 1.0f, 0.955f, 22.0f, 22.0f, frameCol);
        for (int by = 0; by < 20; ++by) {
            for (int bx = 0; bx < 20; ++bx) {
                C2D_DrawRectSolid(iconX + (float)bx, iconY + (float)by, 0.96f, 1.0f, 1.0f,
                                  badge[by * 20 + bx]);
            }
        }
    } else {
        uint32_t goldCol = C2D_Color32(255, 215, 0, 255);
        C2D_DrawRectSolid(iconX + 4.0f, iconY, 0.96f, 12.0f, 7.0f, goldCol);
        C2D_DrawRectSolid(iconX + 8.0f, iconY + 7.0f, 0.96f, 4.0f, 6.0f, goldCol);
        C2D_DrawRectSolid(iconX + 5.0f, iconY + 13.0f, 0.96f, 10.0f, 3.0f, goldCol);
    }

    float textX = boxX + 34.0f;
    DrawToastText(textX, boxY + 6.0f,
                  hc ? "! LOGRO DESBLOQUEADO (HARDCORE) !" : "! LOGRO DESBLOQUEADO !",
                  hc ? C2D_Color32(255, 120, 120, 255) : frameCol);

    char titleBuf[80];
    snprintf(titleBuf, sizeof(titleBuf), "%s (+%lu PTS)", sToast.title, (unsigned long)sToast.points);
    DrawToastText(textX, boxY + 20.0f, titleBuf, C2D_Color32(255, 255, 255, 255));
}
