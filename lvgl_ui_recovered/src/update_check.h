#ifndef TOON_UPDATE_CHECK_H
#define TOON_UPDATE_CHECK_H

/* Periodic GitHub Releases poll. Every UPDATE_CHECK_INTERVAL_S the
 * background thread hits the API for the latest release tag; if it's
 * different from BUILD_VERSION (set at compile time from `git describe`),
 * sets g_update_state.available = 1 and fills in latest_version /
 * release_url / release_notes for the home-tile banner + modal to read.
 *
 * Cheap — one HTTP request every 6 hours, no auth needed for public
 * repos. Skipped entirely when settings.update_check_enabled is 0. */

#define UPDATE_VERSION_MAX 32
#define UPDATE_URL_MAX     200
#define UPDATE_NOTES_MAX   2048

typedef struct {
    volatile int  available;        /* 1 when a newer release was found */
    char latest_version[UPDATE_VERSION_MAX];
    char release_url[UPDATE_URL_MAX];
    char release_notes[UPDATE_NOTES_MAX];
    volatile long last_check_epoch;
    volatile int  last_check_ok;    /* 1 if the most-recent fetch succeeded */
} update_state_t;

extern update_state_t g_update_state;

int  update_check_start(void);
void update_check_now(void);         /* on-demand probe, runs in caller thread */

/* The version string the binary was compiled from. Set by the Makefile
 * via `-DBUILD_VERSION="..."`, or "dev" if the build env doesn't have
 * a git working tree. */
#ifndef BUILD_VERSION
#define BUILD_VERSION "dev"
#endif

#endif
