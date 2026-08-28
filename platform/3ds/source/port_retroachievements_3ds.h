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

typedef struct {
    uint32_t id;
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
const uint32_t* Port_RA_GetBadgePixels(const char* badgeName);

/* Toast Notification Overlay Render (e.g. top or bottom screen) */
void Port_RA_RenderToastOverlay(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_RETROACHIEVEMENTS_3DS_H */
