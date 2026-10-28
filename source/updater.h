#pragma once
#include <stdbool.h>

// In-app updater backed by GitHub releases (shared by all the apps).
typedef enum {
	UPD_IDLE, UPD_CHECKING, UPD_UP_TO_DATE, UPD_AVAILABLE,
	UPD_DOWNLOADING, UPD_INSTALLING, UPD_DONE, UPD_ERROR,
} UpdState;

// repo: "ChunoB/Radio3DS"; path_3dsx: argv[0] when launched as .3dsx (may be NULL)
void        updater_init(const char *repo, const char *version, const char *name_3dsx, const char *path_3dsx);
void        updater_check(void);     // async
void        updater_install(void);   // async, only after UPD_AVAILABLE
void        updater_exit(void);      // waits for a running download
bool        updater_busy(void);
bool        updater_is_cia(void);
UpdState    updater_state(void);
float       updater_progress(void);  // 0..1 while downloading
const char *updater_latest(void);    // newest version found ("1.2.0")
const char *updater_message(void);   // status line in Portuguese
const char *updater_version(void);   // running version
