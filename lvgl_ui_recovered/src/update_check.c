/*
 * Background update checker. Polls
 *   https://api.github.com/repos/Ierlandfan/freetoon-lvgl/releases/latest
 * every 6 hours, compares the returned tag_name against BUILD_VERSION,
 * sets g_update_state.* so the home tile can render a "v0.7.x available"
 * banner. Optional release notes (body field) are stored too for the
 * tap-to-show modal.
 */
#include "update_check.h"
#include "http.h"
#include "settings.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UPDATE_CHECK_INTERVAL_S (6 * 3600)   /* 6 h between polls */
#define RELEASES_API_URL "https://api.github.com/repos/Ierlandfan/freetoon-lvgl/releases/latest"

update_state_t g_update_state = {0};

/* Pull "key":"value" out of a JSON blob. Brittle but jq-free. */
static int json_extract_str(const char * src, const char * key,
                            char * out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char * p = strstr(src, pat);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\n' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) {
        /* Tolerate JSON-escaped chars in the body (we mostly care about
         * \n in release notes). Pass them through; the renderer can
         * handle them or strip. */
        if (*p == '\\' && p[1]) {
            if (p[1] == 'n' && n + 1 < outsz) {
                out[n++] = '\n';
                p += 2;
                continue;
            }
            if (p[1] == '\\' && n + 1 < outsz) {
                out[n++] = '\\';
                p += 2;
                continue;
            }
            if (p[1] == '"' && n + 1 < outsz) {
                out[n++] = '"';
                p += 2;
                continue;
            }
            p++;   /* skip unknown escape */
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

/* Strip a trailing /Z-suffix etc. — empty/whitespace strings get treated
 * as "missing". Used so notes containing nothing don't get rendered as
 * "release notes:" with an empty body. */
static int nonempty(const char * s) {
    if (!s) return 0;
    while (*s) {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') return 1;
        s++;
    }
    return 0;
}

/* Return 1 if `tag` represents a newer release than the running build.
 * Both strings are expected to be of the form "v0.7.2" or similar — we
 * just check string-inequality after trimming the leading 'v', so the
 * comparison treats "v0.7.10" > "v0.7.2" the same way semver does
 * lexically (good enough — we don't expect to ship >9 minors). */
static int is_newer_than_build(const char * tag) {
    if (!nonempty(tag)) return 0;
    const char * t = tag;
    const char * b = BUILD_VERSION;
    if (*t == 'v') t++;
    if (*b == 'v') b++;
    return strcmp(t, b) != 0;
}

void update_check_now(void) {
    if (!settings.update_check_enabled) return;
    static char body[8192];
    int n = http_fetch(RELEASES_API_URL, body, sizeof body);
    g_update_state.last_check_epoch = (long)time(NULL);
    if (n <= 0) {
        g_update_state.last_check_ok = 0;
        return;
    }
    g_update_state.last_check_ok = 1;

    char tag[UPDATE_VERSION_MAX] = {0};
    char url[UPDATE_URL_MAX]     = {0};
    char notes[UPDATE_NOTES_MAX] = {0};
    json_extract_str(body, "tag_name",    tag,   sizeof tag);
    json_extract_str(body, "html_url",    url,   sizeof url);
    json_extract_str(body, "body",        notes, sizeof notes);

    if (is_newer_than_build(tag)) {
        snprintf(g_update_state.latest_version,
                 sizeof g_update_state.latest_version, "%s", tag);
        snprintf(g_update_state.release_url,
                 sizeof g_update_state.release_url, "%s", url);
        snprintf(g_update_state.release_notes,
                 sizeof g_update_state.release_notes, "%s", notes);
        if (!g_update_state.available) {
            fprintf(stderr, "[update] new version available: %s (running %s)\n",
                    tag, BUILD_VERSION);
        }
        g_update_state.available = 1;
    } else {
        if (g_update_state.available)
            fprintf(stderr, "[update] caught up to %s\n", BUILD_VERSION);
        g_update_state.available = 0;
    }
}

static void * update_thread(void * arg) {
    (void)arg;
    /* Stagger the first probe by 30 s so we don't fight with boot-time
     * network setup on the Toon (wifi association can take a moment). */
    sleep(30);
    while (1) {
        update_check_now();
        sleep(UPDATE_CHECK_INTERVAL_S);
    }
    return NULL;
}

int update_check_start(void) {
    if (!settings.update_check_enabled) {
        fprintf(stderr, "[update] checks disabled in settings\n");
        return 0;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, update_thread, NULL) != 0) return -1;
    pthread_detach(t);
    fprintf(stderr, "[update] checker started (running %s, every %d s)\n",
            BUILD_VERSION, UPDATE_CHECK_INTERVAL_S);
    return 0;
}
