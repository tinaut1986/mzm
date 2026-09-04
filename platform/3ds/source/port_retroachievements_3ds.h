#ifndef PORT_RETROACHIEVEMENTS_3DS_H
#define PORT_RETROACHIEVEMENTS_3DS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RA_STATUS_DISABLED = 0,
    RA_STATUS_OFFLINE,      /* the server could not be reached */
    RA_STATUS_CONNECTING,
    RA_STATUS_CONNECTED,
    RA_STATUS_ERROR,        /* the server answered and rejected the login */
    RA_STATUS_NO_ACCOUNT    /* nothing to log in with yet */
} RetroAchievementsStatus;

typedef enum {
    RA_ACH_TYPE_STANDARD = 0,
    RA_ACH_TYPE_PROGRESSION,
    RA_ACH_TYPE_WIN_CONDITION,
    RA_ACH_TYPE_MISSABLE
} RetroAchievementType;

/* Ordering the bottom-screen list can be put in. DEFAULT keeps whatever
 * order rcheevos handed us (grouped by lock state), which is what the list
 * showed before sorting existed. */
typedef enum {
    RA_SORT_DEFAULT = 0,
    RA_SORT_TITLE,      /* A-Z */
    RA_SORT_POINTS,     /* most valuable first */
    RA_SORT_RECENT,     /* most recently unlocked first, still-locked last */
    RA_SORT_COUNT
} RetroAchievementSort;

/* One achievement set: the game's core set, or one of its subsets ("packs").
 * The server returns them all in one response and rcheevos activates every
 * one, so a game can legitimately have several. Counts are over this set
 * only. */
typedef struct {
    uint32_t id;
    char title[64];
    uint32_t total;
    uint32_t unlocked;
    uint32_t hardcoreUnlocked;
    uint32_t totalPoints;
    uint32_t unlockedPoints;
} RetroAchievementSubset;

typedef struct {
    uint32_t id;
    uint32_t subsetId; /* which set this belongs to; matches RetroAchievementSubset::id */
    char title[64];
    char description[128];
    char badgeName[16]; /* e.g. "255208" */
    char memAddr[256];  /* Official RA trigger expression from patch data */
    uint32_t points;
    RetroAchievementType type;
    bool unlocked;
    bool hardcoreUnlocked;
    uint32_t unlockTime;
} RetroAchievementItem;

/* Core RA control & config */
void Port_RA_Init(void);
void Port_RA_Shutdown(void);
void Port_RA_Update(void);           /* Called every frame to update timers/toasts */
void Port_RA_EvaluateTriggers(void); /* Evaluates MemAddr expressions against GBA memory */

bool Port_RA_IsEnabled(void);
void Port_RA_SetEnabled(bool enabled);

bool Port_RA_IsHardcore(void);
void Port_RA_SetHardcore(bool hardcore);
/* False when the build forbids hardcore outright (debug-tools builds carry a
 * god-mode / no-clip cheat harness). Port_RA_SetHardcore(true) is a no-op in
 * that case, and the UI shows hardcore as unavailable rather than off. */
bool Port_RA_HardcoreAllowed(void);

bool Port_RA_GetNotificationSound(void);
void Port_RA_SetNotificationSound(bool sound);

const char* Port_RA_GetUsername(void);
void Port_RA_SetUsername(const char* username);

const char* Port_RA_GetToken(void);
void Port_RA_SetToken(const char* token);

RetroAchievementsStatus Port_RA_GetStatus(void);
const char* Port_RA_GetStatusString(int lang);
const char* Port_RA_GetLastDebugLog(void);

void Port_RA_PromptLogin(void);

/* Achievement querying for UI */
uint32_t Port_RA_GetAchievementCount(void);
uint32_t Port_RA_GetUnlockedCount(void);
uint32_t Port_RA_GetHardcoreUnlockedCount(void);
uint32_t Port_RA_GetTotalPoints(void);
uint32_t Port_RA_GetUnlockedPoints(void);
const RetroAchievementItem* Port_RA_GetAchievement(uint32_t index);

/* Subsets ("packs"). A game with a single set reports a count of 1 -- the UI
 * uses that to skip the pack chooser entirely rather than showing a menu with
 * one entry in it. */
uint32_t Port_RA_GetSubsetCount(void);
const RetroAchievementSubset* Port_RA_GetSubset(uint32_t index);

/* The filtered + sorted view the list UI walks, kept separate from the raw
 * array above so the counters, toasts and unlock bookkeeping keep seeing
 * every achievement regardless of what the list is currently showing.
 * Port_RA_SetListSubset(0) means "all sets". Both setters are cheap and
 * idempotent; the view is rebuilt only when the filter or order changed. */
void Port_RA_SetListSubset(uint32_t subsetId);
uint32_t Port_RA_GetListSubset(void);
void Port_RA_SetListSort(RetroAchievementSort sort);
RetroAchievementSort Port_RA_GetListSort(void);
/* Direction the current order runs in. Every order is defined ascending --
 * A-Z, cheapest first, oldest unlock first -- and this flips it. */
void Port_RA_SetListDescending(bool descending);
bool Port_RA_GetListDescending(void);
uint32_t Port_RA_GetViewCount(void);
const RetroAchievementItem* Port_RA_GetViewAchievement(uint32_t index);
const uint32_t* Port_RA_GetBadgePixels(const char* badgeName);

/* Toast Notification Overlay Render (e.g. top or bottom screen) */
void Port_RA_RenderToastOverlay(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_RETROACHIEVEMENTS_3DS_H */
