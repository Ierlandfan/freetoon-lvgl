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
#include "notify.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UPDATE_CHECK_INTERVAL_S (6 * 3600)   /* 6 h between polls */
/* Two update channels (settings.update_channel):
 *   1 = beta/dev (default): newest release INCLUDING prereleases. We fetch a
 *       page of recent releases and pick the HIGHEST SEMVER tag ourselves —
 *       NOT index 0 — because GitHub orders /releases by created_at, so a
 *       later edit/re-create of an older release (e.g. v0.9.8) could otherwise
 *       surface as "newest" and mask v0.9.13+.
 *   0 = stable/official only: /releases/latest, which GitHub returns only for
 *       non-prerelease, non-draft releases (404 → no update, banner stays off).
 * All freetoon releases are currently beta, so "stable" finds nothing until a
 * non-prerelease is published. */
/* per_page kept modest: the asset-metadata blob per release is large and the
 * shared http_fetch caps the transfer at a few seconds, so a 30-release page
 * could run past the timeout on the Toon's slow link and fail the whole check.
 * 10 newest is more than enough to find the highest semver even when GitHub's
 * created_at order is scrambled by a re-created older release. */
#define RELEASES_API_BETA   "https://api.github.com/repos/Ierlandfan/freetoon-lvgl/releases?per_page=10"
#define RELEASES_API_STABLE "https://api.github.com/repos/Ierlandfan/freetoon-lvgl/releases/latest"

/* Lightweight connectivity probe used to tell "the GitHub API itself is
 * unreachable / rate-limited / blocked" apart from "the Toon has no internet
 * at all". This is the SAME reliable host the working tiles can reach over
 * plain HTTP, so a green here means general internet is fine even when the
 * api.github.com fetch returned nothing (e.g. Pi-Hole/proxy quirks, the 60/hr
 * unauthenticated GitHub API rate-limit, or a transfer that ran past the
 * fetch timeout). When this succeeds we must NOT report "check internet". */
#define CONNECTIVITY_PROBE_URL "http://detectportal.firefox.com/success.txt"

update_state_t g_update_state = {0};

/* 1 if real internet is reachable right now, 0 otherwise. Cheap captive-portal
 * style probe (small body, plain HTTP, no TLS handshake to gate on). */
static int internet_reachable(void) {
    char tmp[64];
    int rc = http_fetch(CONNECTIVITY_PROBE_URL, tmp, sizeof tmp);
    return (rc == 0 && strstr(tmp, "success")) ? 1 : 0;
}

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
static void parse_ver(const char * s, int * a, int * b, int * c) {
    if (*s == 'v' || *s == 'V') s++;
    *a = *b = *c = 0;
    /* sscanf stops at the first non-digit/dot, so any `-dirty` / `-3-gabc`
     * git-describe suffix on the running build is ignored. */
    sscanf(s, "%d.%d.%d", a, b, c);
}

/* Return 1 ONLY when `tag` is a strictly NEWER release than the running build
 * (numeric major.minor.patch). Equal or older → 0, so we never offer a
 * downgrade — a dev build (e.g. v0.9.5) won't flag the latest release v0.9.4
 * as "available". */
static int is_newer_than_build(const char * tag) {
    if (!nonempty(tag)) return 0;
    int ta, tb, tc, ba, bb, bc;
    parse_ver(tag, &ta, &tb, &tc);
    parse_ver(BUILD_VERSION, &ba, &bb, &bc);
    if (ta != ba) return ta > ba;
    if (tb != bb) return tb > bb;
    return tc > bc;
}

/* Numeric major.minor.patch compare: >0 if a newer than b, <0 older, 0 equal. */
static int ver_cmp(const char * a, const char * b) {
    int aa, ab, ac, ba, bb, bc;
    parse_ver(a, &aa, &ab, &ac);
    parse_ver(b, &ba, &bb, &bc);
    if (aa != ba) return aa - ba;
    if (ab != bb) return ab - bb;
    return ac - bc;
}

void update_check_now(void) {
    if (!settings.update_check_enabled) return;
    /* GitHub's /releases/latest payload runs ~8-12 KB once asset metadata
     * is in there; bump the buffer to 32 KB so curl doesn't EPIPE when we
     * close the read pipe before it's finished writing. */
    static char body[32768];
    body[0] = 0;
    const char * api = settings.update_channel == 0
                       ? RELEASES_API_STABLE : RELEASES_API_BETA;
    int rc = http_fetch(api, body, sizeof body);
    g_update_state.last_check_epoch = (long)time(NULL);
    /* http_fetch returns 0 on success (not byte count). Treat anything
     * non-zero as failure. body[] is also empty after a failure since
     * we cleared it pre-call. */
    if (rc != 0 || body[0] == 0) {
        /* The api.github.com fetch failed — but that is NOT proof the Toon is
         * offline. The GitHub *API* host is the flaky one: unauthenticated
         * requests are rate-limited (60/hr → 403/empty), a Pi-Hole/proxy can
         * single it out, and the larger /releases payload can run past the
         * fetch timeout on a slow link. Meanwhile buienradar/news/waste/
         * domoticz all fetch fine. So only report "check internet" when a real
         * connectivity probe ALSO fails; otherwise treat it as "couldn't reach
         * the update server right now" without the false offline banner. */
        g_update_state.last_check_ok = internet_reachable();
        fprintf(stderr, "[update] github fetch failed (rc=%d), internet=%d\n",
                rc, g_update_state.last_check_ok);
        return;
    }
    g_update_state.last_check_ok = 1;

    char tag[UPDATE_VERSION_MAX] = {0};
    char url[UPDATE_URL_MAX]     = {0};
    char notes[UPDATE_NOTES_MAX] = {0};

    if (settings.update_channel == 0) {
        /* /releases/latest → a single release object. */
        json_extract_str(body, "tag_name", tag,   sizeof tag);
        json_extract_str(body, "html_url", url,   sizeof url);
        json_extract_str(body, "body",     notes, sizeof notes);
    } else {
        /* Beta channel: body is an array of releases. Walk every "tag_name"
         * and keep the highest semver — don't trust GitHub's date order. */
        const char * best_at = NULL;
        const char * pos = body;
        char cur[UPDATE_VERSION_MAX];
        const char * t;
        while ((t = strstr(pos, "\"tag_name\"")) != NULL) {
            if (json_extract_str(t, "tag_name", cur, sizeof cur) && cur[0] &&
                (!tag[0] || ver_cmp(cur, tag) > 0)) {
                snprintf(tag, sizeof tag, "%s", cur);
                best_at = t;
            }
            pos = t + 10;
        }
        if (tag[0]) {
            /* html_url is deterministic — build it instead of parsing the
             * matching release object out of the array (brittle with nested
             * author/assets objects). */
            snprintf(url, sizeof url,
                "https://github.com/Ierlandfan/freetoon-lvgl/releases/tag/%s", tag);
            /* notes = the "body" field that follows this release's tag_name. */
            if (best_at) json_extract_str(best_at, "body", notes, sizeof notes);
        }
    }

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

/* Kick off the on-device self-installer (detached) + a Toon banner. Used by
 * the nightly auto-update and the "Update now" button. */
void update_install_now(void) {
    char msg[96];
    snprintf(msg, sizeof msg, "Updaten naar %s...",
             g_update_state.latest_version[0] ? g_update_state.latest_version : "nieuwste versie");
    notify_show("update", "freetoon", msg);
    fprintf(stderr, "[update] installing now: %s\n", g_update_state.latest_version);
    system("nohup sh -c 'sleep 2; curl -fsSL "
           "https://raw.githubusercontent.com/Ierlandfan/freetoon-lvgl/main/scripts/toon-selfinstall.sh "
           "| sh' >/var/volatile/tmp/selfinstall.log 2>&1 &");
}

static void * update_thread(void * arg) {
    (void)arg;
    /* Stagger the first probe by 30 s so we don't fight with boot-time
     * network setup on the Toon (wifi association can take a moment). */
    sleep(30);
    update_check_now();
    long last_check = (long)time(NULL);
    int  last_auto_yday = -1;     /* so a manual reboot at the hour doesn't double-fire */
    while (1) {
        sleep(60);                /* wake every minute to service the auto-update window */
        long now = (long)time(NULL);
        if (now - last_check >= UPDATE_CHECK_INTERVAL_S) {
            update_check_now();
            last_check = now;
        }
        /* Nightly auto-update: once per day, at the configured hour, if a
         * newer release is available, install it (with a banner). */
        if (settings.auto_update_enabled && g_update_state.available) {
            time_t t = time(NULL); struct tm tm;
            localtime_r(&t, &tm);
            if (tm.tm_hour == settings.auto_update_hour && tm.tm_yday != last_auto_yday) {
                last_auto_yday = tm.tm_yday;
                update_install_now();
            }
        }
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
