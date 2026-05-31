/*
 * HVC Groep waste-collection client. Polls 6× per day. Reads postcode +
 * huisnummer from /mnt/data/tsc/wastecollection.userSettings.json so the
 * existing TSC plugin config is reused without duplication.
 */
#include "wastecollection.h"
#include "http.h"
#include "settings.h"
#include <time.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

waste_state_t waste_state = {0};

#define CFG_PATH "/mnt/data/tsc/wastecollection.userSettings.json"

/* Find "key":"VALUE" in a JSON blob, copy VALUE into out. */
static int json_str(const char * json, const char * key,
                    char * out, size_t outsz) {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char * p = strstr(json, needle);
    if (!p) { if (outsz) out[0] = 0; return 0; }
    p += strlen(needle);
    const char * e = strchr(p, '"');
    if (!e) { if (outsz) out[0] = 0; return 0; }
    size_t n = (size_t)(e - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}
/* Find "key":NUMBER or "key":"NUMBER", parse int. */
static int json_int(const char * json, const char * key, int dflt) {
    char tmp[24];
    if (json_str(json, key, tmp, sizeof(tmp))) return atoi(tmp);
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(json, needle);
    if (!p) return dflt;
    return atoi(p + strlen(needle));
}

static int read_config(char * postcode, int psz, char * huis, int hsz) {
    /* Prefer freetoon's own Settings (toonui.cfg) so the postcode/house-number
     * can be set in the UI without editing the stock TSC json. Fall back to the
     * TSC waste plugin's config when the freetoon fields are blank. */
    if (settings.waste_postcode[0] && settings.waste_housenr[0]) {
        /* Normalise the postcode regardless of how it was entered (PWA stores
         * verbatim): strip spaces, uppercase → "1671 ad" becomes "1671AD". */
        int o = 0;
        for (const char * q = settings.waste_postcode; *q && o < psz - 1; q++) {
            unsigned char c = (unsigned char)*q;
            if (c == ' ') continue;
            if (c >= 'a' && c <= 'z') c -= 32;
            postcode[o++] = (char)c;
        }
        postcode[o] = 0;
        snprintf(huis, hsz, "%s", settings.waste_housenr);
        return 0;
    }
    FILE * f = fopen(CFG_PATH, "r");
    if (!f) return -1;
    char body[1024];
    size_t n = fread(body, 1, sizeof(body) - 1, f);
    body[n] = 0;
    fclose(f);
    if (!json_str(body, "Postcode", postcode, psz)) return -1;
    if (!json_str(body, "Huisnummer", huis, hsz)) return -1;
    return 0;
}

static int fetch_bag_id(const char * pc, const char * huis) {
    char url[200];
    snprintf(url, sizeof(url),
        "https://inzamelkalender.hvcgroep.nl/rest/adressen/%s-%s", pc, huis);
    static char body[4096];
    if (http_fetch(url, body, sizeof(body)) != 0) return -1;
    /* The response is a JSON array; first object has bagId. */
    return json_str(body, "bagId", waste_state.bag_id, sizeof(waste_state.bag_id))
           ? 0 : -1;
}

/* Type IDs HVC uses. Order in the array == display order. */
static const struct { int id; const char * label; } HVC_TYPES[WASTE_TYPES] = {
    { 5, "GFT"       },
    { 6, "Plastic"   },
    { 3, "Papier"    },
    { 2, "Restafval" },
};

static int fetch_afvalstromen(void) {
    if (!waste_state.bag_id[0]) return -1;
    char url[200];
    snprintf(url, sizeof(url),
        "https://inzamelkalender.hvcgroep.nl/rest/adressen/%s/afvalstromen",
        waste_state.bag_id);
    /* The full response can be ~250 KB but each entry is small; we walk
       record-by-record to keep the buffer manageable. */
    static char body[300 * 1024];
    if (http_fetch(url, body, sizeof(body)) != 0) return -1;

    /* For each tracked id, find that "id":N entry's ophaaldatum. */
    for (int i = 0; i < WASTE_TYPES; i++) {
        snprintf(waste_state.items[i].label, sizeof(waste_state.items[i].label),
                 "%s", HVC_TYPES[i].label);
        waste_state.items[i].date[0] = 0;

        /* Walk every record looking for the matching id. */
        const char * p = body;
        while ((p = strstr(p, "\"id\":")) != NULL) {
            int id = atoi(p + 5);
            if (id == HVC_TYPES[i].id) {
                /* Find the next ophaaldatum within this record (before
                   the next record marker). */
                const char * end = strstr(p, "\"id\":");  /* search after current */
                if (end) end = strstr(end + 5, "\"id\":");
                const char * dt = strstr(p, "\"ophaaldatum\":\"");
                if (dt && (!end || dt < end)) {
                    dt += strlen("\"ophaaldatum\":\"");
                    const char * dtend = strchr(dt, '"');
                    if (dtend && dtend - dt < (long)sizeof(waste_state.items[i].date)) {
                        size_t n = dtend - dt;
                        memcpy(waste_state.items[i].date, dt, n);
                        waste_state.items[i].date[n] = 0;
                    }
                }
                break;
            }
            p += 5;
        }
    }
    return 0;
}

void waste_next_pickup(char * out_date, int dsz, char * out_labels, int lsz) {
    out_date[0] = 0;
    out_labels[0] = 0;
    char min_date[16] = "9999-99-99";
    for (int i = 0; i < WASTE_TYPES; i++) {
        if (!waste_state.items[i].date[0]) continue;
        if (strcmp(waste_state.items[i].date, min_date) < 0)
            snprintf(min_date, sizeof(min_date), "%s", waste_state.items[i].date);
    }
    if (min_date[0] == '9') return;   /* none scheduled */
    snprintf(out_date, dsz, "%s", min_date);
    for (int i = 0; i < WASTE_TYPES; i++) {
        if (strcmp(waste_state.items[i].date, min_date) == 0) {
            if (out_labels[0]) strncat(out_labels, "+", lsz - strlen(out_labels) - 1);
            strncat(out_labels, waste_state.items[i].label,
                    lsz - strlen(out_labels) - 1);
        }
    }
}

int waste_next_2_pickups(waste_pickup_t * out1, waste_pickup_t * out2) {
    if (out1) { out1->date[0] = 0; out1->labels[0] = 0; }
    if (out2) { out2->date[0] = 0; out2->labels[0] = 0; }
    /* Step 1: find soonest date. */
    char d1[16] = "9999-99-99";
    for (int i = 0; i < WASTE_TYPES; i++) {
        if (!waste_state.items[i].date[0]) continue;
        if (strcmp(waste_state.items[i].date, d1) < 0)
            snprintf(d1, sizeof d1, "%s", waste_state.items[i].date);
    }
    if (d1[0] == '9') return 0;
    /* Step 2: find soonest date strictly greater than d1. */
    char d2[16] = "9999-99-99";
    for (int i = 0; i < WASTE_TYPES; i++) {
        const char *d = waste_state.items[i].date;
        if (!d[0]) continue;
        if (strcmp(d, d1) <= 0) continue;
        if (strcmp(d, d2) < 0) snprintf(d2, sizeof d2, "%s", d);
    }
    /* Populate out1. */
    if (out1) {
        snprintf(out1->date, sizeof out1->date, "%s", d1);
        for (int i = 0; i < WASTE_TYPES; i++) {
            if (strcmp(waste_state.items[i].date, d1) == 0) {
                if (out1->labels[0])
                    strncat(out1->labels, "+",
                            sizeof out1->labels - strlen(out1->labels) - 1);
                strncat(out1->labels, waste_state.items[i].label,
                        sizeof out1->labels - strlen(out1->labels) - 1);
            }
        }
    }
    if (d2[0] == '9') return 1;
    if (out2) {
        snprintf(out2->date, sizeof out2->date, "%s", d2);
        for (int i = 0; i < WASTE_TYPES; i++) {
            if (strcmp(waste_state.items[i].date, d2) == 0) {
                if (out2->labels[0])
                    strncat(out2->labels, "+",
                            sizeof out2->labels - strlen(out2->labels) - 1);
                strncat(out2->labels, waste_state.items[i].label,
                        sizeof out2->labels - strlen(out2->labels) - 1);
            }
        }
    }
    return 2;
}

/* Per-type collection-time cutoff (local hour) on the pickup day: a type stops
 * counting as "upcoming" once this hour passes on its pickup date. Plastic is
 * collected on the late/evening round (~21:00); the grey (Restafval), green
 * (GFT) and paper rounds wrap up in the afternoon (~16:00). */
static int waste_cutoff_hour(const char * label) {
    if (label && (strstr(label, "Plastic") || strstr(label, "plastic") ||
                  strstr(label, "PMD") || strstr(label, "pmd"))) return 21;
    return 16;
}

/* Seconds from now until <date> at the type's cutoff hour; <0 if already past. */
static long waste_secs_until(const char * date, const char * label) {
    if (!date || !date[0]) return -1;
    struct tm t = {0};
    t.tm_year = atoi(date)     - 1900;
    t.tm_mon  = atoi(date + 5) - 1;
    t.tm_mday = atoi(date + 8);
    t.tm_hour = waste_cutoff_hour(label);
    t.tm_isdst = -1;
    return (long)(mktime(&t) - time(NULL));
}

/* Whole days from today's local midnight to <date> (negative if in the past). */
long waste_days_until(const char * date) {
    if (!date || !date[0]) return -9999;
    struct tm t = {0};
    t.tm_year = atoi(date)     - 1900;
    t.tm_mon  = atoi(date + 5) - 1;
    t.tm_mday = atoi(date + 8);
    t.tm_isdst = -1;
    time_t pickup = mktime(&t);
    time_t now = time(NULL);
    struct tm nt; localtime_r(&now, &nt);
    nt.tm_hour = nt.tm_min = nt.tm_sec = 0; nt.tm_isdst = -1;
    return (long)((pickup - mktime(&nt)) / 86400);
}

/* Up to max_n soonest UPCOMING pickup dates within `lead_days`, grouping the
 * still-upcoming types collected on each date (a type drops off once its
 * per-type cutoff time passes on the pickup day). The single source of truth
 * for both the home tile and the dim screen. Returns count filled. */
int waste_next_n_windowed(int lead_days, waste_pickup_t * out, int max_n) {
    for (int i = 0; i < max_n; i++) { out[i].date[0] = 0; out[i].labels[0] = 0; }
    int filled = 0;
    char prev[16] = "0000-00-00";
    while (filled < max_n) {
        /* soonest date strictly after `prev` with a still-upcoming type in window */
        char best[16] = "9999-99-99"; int have = 0;
        for (int i = 0; i < WASTE_TYPES; i++) {
            const char * d  = waste_state.items[i].date;
            const char * lb = waste_state.items[i].label;
            if (!d[0] || strcmp(d, prev) <= 0) continue;
            if (waste_secs_until(d, lb) < 0)        continue;  /* past its cutoff */
            if (waste_days_until(d) > lead_days)    continue;  /* beyond window   */
            if (strcmp(d, best) < 0) { snprintf(best, sizeof best, "%s", d); have = 1; }
        }
        if (!have) break;
        snprintf(out[filled].date, sizeof out[filled].date, "%s", best);
        for (int i = 0; i < WASTE_TYPES; i++) {
            const char * d  = waste_state.items[i].date;
            const char * lb = waste_state.items[i].label;
            if (strcmp(d, best) != 0) continue;
            if (waste_secs_until(d, lb) < 0) continue;        /* skip already-past types */
            if (out[filled].labels[0])
                strncat(out[filled].labels, "+",
                        sizeof out[filled].labels - strlen(out[filled].labels) - 1);
            strncat(out[filled].labels, lb,
                    sizeof out[filled].labels - strlen(out[filled].labels) - 1);
        }
        snprintf(prev, sizeof prev, "%s", best);
        filled++;
    }
    return filled;
}

/* ---- generic ICS provider (prezero / cyclus / dar / cranendonck / katwijk /
 * any provider that exposes a downloadable .ics calendar). One parser handles
 * them all: walk VEVENTs, take DTSTART (date) + SUMMARY (waste type), and keep
 * the soonest upcoming date per type into the 4 fixed slots. ---- */
static void ymd_today(char * out, int sz) {
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    strftime(out, sz, "%Y-%m-%d", &tm);
}

/* Lowercased SUMMARY → slot index, or -1. Slots: 0 GFT, 1 Plastic, 2 Papier,
 * 3 Restafval (same order/labels as the HVC path). */
static int ics_slot(const char * s) {
    if (strstr(s, "gft") || strstr(s, "groente") || strstr(s, "tuin"))     return 0;
    if (strstr(s, "pmd") || strstr(s, "plastic") || strstr(s, "pbd") ||
        strstr(s, "verpakk") || strstr(s, "drankk"))                       return 1;
    if (strstr(s, "papier") || strstr(s, "karton"))                        return 2;
    if (strstr(s, "rest"))                                                 return 3;
    return -1;
}

static void ics_label_from_uid(const char * uid, char * out, size_t outsz) {
    if (outsz) out[0] = 0;
    if (!uid || !uid[0] || !out || outsz == 0) return;

    const char * type = strrchr(uid, '-');
    type = type ? type + 1 : uid;

    char t[32];
    size_t n = 0;
    while (type[n] && type[n] != '\r' && type[n] != '\n' &&
           type[n] != '@' && n < sizeof(t) - 1) {
        unsigned char c = (unsigned char)type[n];
        t[n] = (char)tolower(c);
        n++;
    }
    t[n] = 0;

    if (strcmp(t, "pmd") == 0) {
        snprintf(out, outsz, "PMD");
    } else if (strcmp(t, "gft") == 0) {
        snprintf(out, outsz, "GFT");
    } else if (strcmp(t, "restafval") == 0 || strcmp(t, "rest") == 0) {
        snprintf(out, outsz, "Restafval");
    } else if (strcmp(t, "papier") == 0 || strcmp(t, "paper") == 0) {
        snprintf(out, outsz, "Papier");
    }
}

/* Generic ICS parse: the calendar's own SUMMARY text is the waste-type label,
 * so it works for any municipality (HVC, mijnafvalwijzer, Ximmio, …) regardless
 * of how they name their streams. Each distinct SUMMARY claims one of the
 * WASTE_TYPES slots (first-seen), keeping its soonest upcoming date. */
static int parse_ics(const char * body) {
    char today[16]; ymd_today(today, sizeof today);
    for (int i = 0; i < WASTE_TYPES; i++) {
        waste_state.items[i].label[0] = 0;
        waste_state.items[i].date[0]  = 0;
    }
    int found = 0;
    const char * p = body;
    while ((p = strstr(p, "BEGIN:VEVENT")) != NULL) {
        const char * end = strstr(p, "END:VEVENT");
        const char * evend = end ? end : p + strlen(p);

        /* DTSTART → first run of 8 digits (YYYYMMDD), even inside
         * "DTSTART;VALUE=DATE:20260521" or "DTSTART:20260521T070000Z". */
        char date[16] = "";
        const char * d = strstr(p, "DTSTART");
        if (d && d < evend) {
            for (const char * q = d; *q && q < evend; q++) {
                if (isdigit((unsigned char)*q)) {
                    int n = 0; char buf[9];
                    for (const char * r = q; n < 8 && isdigit((unsigned char)*r); r++) buf[n++] = *r;
                    if (n == 8)
                        snprintf(date, sizeof date, "%.4s-%.2s-%.2s", buf, buf + 4, buf + 6);
                    break;
                }
            }
        }

        /* SUMMARY → label (raw text, trimmed of trailing CR/LF). */
        char label[40] = "";
        const char * s = strstr(p, "SUMMARY");
        if (s && s < evend) {
            const char * c = strchr(s, ':');
            if (c) {
                int n = 0; c++;
                /* Unescape ICS text: "\," → "," , "\;" → ";" , "\\" → "\" , "\n" → space. */
                while (*c && *c != '\r' && *c != '\n' && n < (int)sizeof(label) - 1) {
                    if (*c == '\\' && c[1]) {
                        c++;
                        label[n++] = (*c == 'n' || *c == 'N') ? ' ' : *c;
                        c++;
                    } else {
                        label[n++] = *c++;
                    }
                }
                label[n] = 0;
            }
        }

        /* Prefer short type names from UID when present, e.g.
         * UID:2026-34-pmd -> PMD. Some ICS feeds have very long SUMMARY
         * strings that are poor UI labels but stable, compact UIDs. */
        {
            char uid[80] = "";
            char uid_label[40] = "";
            const char * u = strstr(p, "UID");
            if (u && u < evend) {
                const char * c = strchr(u, ':');
                if (c) {
                    int n = 0; c++;
                    while (*c && *c != '\r' && *c != '\n' &&
                           n < (int)sizeof(uid) - 1)
                        uid[n++] = *c++;
                    uid[n] = 0;
                    ics_label_from_uid(uid, uid_label, sizeof uid_label);
                    if (uid_label[0])
                        snprintf(label, sizeof label, "%s", uid_label);
                }
            }
        }

        if (date[0] && label[0] && strcmp(date, today) >= 0) {
            /* Find this label's slot, or claim the next free one. */
            int slot = -1;
            for (int i = 0; i < WASTE_TYPES; i++)
                if (waste_state.items[i].label[0] &&
                    strcmp(waste_state.items[i].label, label) == 0) { slot = i; break; }
            if (slot < 0)
                for (int i = 0; i < WASTE_TYPES; i++)
                    if (!waste_state.items[i].label[0]) {
                        slot = i;
                        snprintf(waste_state.items[i].label,
                                 sizeof waste_state.items[i].label, "%s", label);
                        break;
                    }
            if (slot >= 0 &&
                (waste_state.items[slot].date[0] == 0 ||
                 strcmp(date, waste_state.items[slot].date) < 0)) {
                snprintf(waste_state.items[slot].date,
                         sizeof waste_state.items[slot].date, "%s", date);
                found = 1;
            }
        }
        p = evend + 1;
    }
    return found ? 0 : -1;
}

static int fetch_ics(void) {
    if (!settings.waste_ics_url[0]) return -1;
    static char body[300 * 1024];
    /* file:///path → read directly (e.g. a Python/cron job writes the ICS) */
    if (strncmp(settings.waste_ics_url, "file://", 7) == 0) {
        FILE * f = fopen(settings.waste_ics_url + 7, "r");
        if (!f) return -1;
        size_t n = fread(body, 1, sizeof body - 1, f);
        body[n] = 0; fclose(f);
    } else {
        if (http_fetch(settings.waste_ics_url, body, sizeof body) != 0) return -1;
    }
    if (!strstr(body, "VEVENT")) return -1;
    return parse_ics(body);
}

/* Provider-plugin path: run the wastefetch helper (embedded QuickJS runs the
 * real ToonSoftwareCollective provider script) → normalized ICS → parse_ics.
 * This is the full stock-app mimic; covers all ~42 providers. */
static int fetch_plugin(void) {
    if (!settings.waste_plugin[0] || strcmp(settings.waste_plugin, "0") == 0) return -1;
    char cmd[1280];
    snprintf(cmd, sizeof cmd,
        "/mnt/data/wastefetch '%s' '%s' '%s' '%s' '%s' '%s' '%s' "
        "/mnt/data/wastedates.ics 2>/tmp/wastefetch.log",
        settings.waste_plugin, settings.waste_postcode, settings.waste_housenr,
        settings.waste_icsid, settings.waste_street, settings.waste_city,
        settings.waste_ics_url);
    if (system(cmd) == -1) return -1;
    /* Log the exit code + first line of stderr so the one-shot fetch can be
       debugged on-device without recompiling */
    FILE * f = fopen("/tmp/wastefetch.log", "r");
    if (f) {
        char line[256];
        if (fgets(line, sizeof line, f))
            fprintf(stderr, "[waste] wastefetch: %s", line);
        fclose(f);
    }
    f = fopen("/mnt/data/wastedates.ics", "r");
    if (!f) return -1;
    static char body[300 * 1024];
    size_t n = fread(body, 1, sizeof body - 1, f);
    body[n] = 0; fclose(f);
    if (!strstr(body, "VEVENT")) return -1;
    return parse_ics(body);
}

static void * waste_thread(void * arg) {
    (void)arg;
    while (1) {
        int ok = 0;
        /* Provider plugin (full stock-app mimic) wins when configured; then a
         * direct iCal URL; then the legacy HVC postcode lookup. */
        if (settings.waste_plugin[0] && strcmp(settings.waste_plugin, "0") != 0) {
            ok = (fetch_plugin() == 0);
        } else if (settings.waste_ics_url[0]) {
            ok = (fetch_ics() == 0);
        } else {
            char pc[16], huis[8];
            if (read_config(pc, sizeof(pc), huis, sizeof(huis)) == 0 &&
                fetch_bag_id(pc, huis) == 0 &&
                fetch_afvalstromen() == 0)
                ok = 1;
        }
        waste_state.connected = ok;
        if (ok)
            fprintf(stderr, "[waste] %s=%s %s=%s %s=%s %s=%s\n",
                    waste_state.items[0].label, waste_state.items[0].date,
                    waste_state.items[1].label, waste_state.items[1].date,
                    waste_state.items[2].label, waste_state.items[2].date,
                    waste_state.items[3].label, waste_state.items[3].date);
        /* Sleep in 10 s steps so a settings-save wake_fetch arrives quickly. */
        for (int s = 0; s < 4 * 60 * 60 && !waste_state.wake_fetch; s += 10)
            sleep(10);
        waste_state.wake_fetch = 0;
    }
    return NULL;
}

int waste_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, waste_thread, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}
