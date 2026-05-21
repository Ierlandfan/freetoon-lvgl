/*
 * HVC Groep waste-collection client. Polls 6× per day. Reads postcode +
 * huisnummer from /mnt/data/tsc/wastecollection.userSettings.json so the
 * existing TSC plugin config is reused without duplication.
 */
#include "wastecollection.h"
#include "http.h"
#include "settings.h"
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

static void * waste_thread(void * arg) {
    (void)arg;
    char pc[16], huis[8];
    if (read_config(pc, sizeof(pc), huis, sizeof(huis)) != 0) {
        fprintf(stderr, "[waste] no config — bailing\n");
        return NULL;
    }
    fprintf(stderr, "[waste] postcode=%s huisnummer=%s\n", pc, huis);
    while (1) {
        if (fetch_bag_id(pc, huis) == 0) {
            if (fetch_afvalstromen() == 0) {
                waste_state.connected = 1;
                fprintf(stderr, "[waste] %s=%s %s=%s %s=%s %s=%s\n",
                        waste_state.items[0].label, waste_state.items[0].date,
                        waste_state.items[1].label, waste_state.items[1].date,
                        waste_state.items[2].label, waste_state.items[2].date,
                        waste_state.items[3].label, waste_state.items[3].date);
            } else waste_state.connected = 0;
        } else waste_state.connected = 0;
        sleep(4 * 60 * 60);   /* 4 hours */
    }
    return NULL;
}

int waste_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, waste_thread, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}
