#ifndef PORT_UPDATER_3DS_H
#define PORT_UPDATER_3DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Self-updater for the installed CIA. Asks a GitHub-style releases endpoint
 * for the newest build, downloads its .cia and streams it straight into the
 * system installer (am:net). Everything runs on a worker thread; the UI polls
 * Port_Updater_GetState(). */

typedef enum {
    UPDATER_IDLE = 0,
    UPDATER_CHECKING,
    UPDATER_UP_TO_DATE,
    UPDATER_AVAILABLE,    /* newer build found, waiting for the player */
    UPDATER_DOWNLOADING,  /* download streaming into the installer */
    UPDATER_INSTALLED,    /* installed; restart to run it */
    UPDATER_ERROR
} UpdaterState;

/* What the on-screen prompt (drawn over any bottom-screen tab) is asking. The
 * automatic check never installs by itself: it only raises ASK_INSTALL. */
typedef enum {
    UPDATER_PROMPT_NONE = 0,
    UPDATER_PROMPT_ASK_INSTALL,  /* new version found: install? YES/NO */
    UPDATER_PROMPT_PROGRESS,     /* installing, no buttons */
    UPDATER_PROMPT_ASK_RESTART,  /* installed: restart now? YES/NO */
    UPDATER_PROMPT_ERROR         /* the install failed; OK dismisses */
} UpdaterPrompt;

UpdaterPrompt Port_Updater_GetPrompt(void);
/* True when a failed install left the downloaded CIA at <game folder>/update/mzm-update.cia
 * so it can be installed by hand (FBI). */
bool Port_Updater_KeptCia(void);
/* YES/NO (or OK) on the current prompt. */
void Port_Updater_AnswerPrompt(bool yes);

/* Called once after the config is loaded: starts the automatic check when
 * "auto update" is on. */
void Port_Updater_Init(void);

/* Manual check. The result is UPDATER_AVAILABLE (then call
 * Port_Updater_Install) or UPDATER_UP_TO_DATE / UPDATER_ERROR. */
void Port_Updater_CheckNow(void);
void Port_Updater_Install(void);

/* Relaunches the game so the freshly installed build runs. */
void Port_Updater_Restart(void);

UpdaterState Port_Updater_GetState(void);
int Port_Updater_GetProgress(void);           /* 0..100 while DOWNLOADING */
const char* Port_Updater_GetRemoteTag(void);  /* "" until a check succeeded */
const char* Port_Updater_GetMessage(void);    /* short status / error text */
/* Copies the "what's new" text of the last successful check -- every release
 * newer than this build, "== vX.Y.Z ==" headers over "- " lines -- into `out`
 * and returns its length (0 = none known). Safe from the UI thread. */
size_t Port_Updater_CopyNotes(char* out, size_t cap);

bool Port_Updater_GetAuto(void);
void Port_Updater_SetAuto(bool enabled);
bool Port_Updater_GetBeta(void);
void Port_Updater_SetBeta(bool enabled);

/* Releases endpoint. Empty override = the default GitHub URL. Point it at a
 * local server (mzm3ds.ini: update_url=http://192.168.x.x:8000/releases.json)
 * to test without publishing anything; see tools/update-mock-server.py. */
const char* Port_Updater_GetUrlOverride(void);
void Port_Updater_SetUrlOverride(const char* url);

#endif
