#include "backlight.h"
#include "settings.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#ifdef TOON1
/* Toon 1 (i.MX27): a single PWM backlight at this sysfs node, hardware range
   0..max_brightness (= 100 on this board). No ambient-light sensor. */
#define BL_DIR    "/sys/devices/platform/ed2.0-bl.0/backlight/ed2.0-bl.0"
#define BL_MIN_HW 10        /* safety floor: never let the screen go fully dark
                              (0..100 scale) — you must be able to see to recover */
#else
/* Toon 2: MP3309 backlight (range 0..1000) plus an LTR-303 ambient sensor. */
#define BL_DIR    "/sys/class/backlight/mp3309-bl"
#define BL_MIN_HW 0
#endif
#define BL_PATH     BL_DIR "/brightness"
#define BL_MAX_PATH BL_DIR "/max_brightness"

/* The UI and settings always work in a fixed logical 0..1000 range. The
   hardware maximum differs per board (Toon 1 = 100, Toon 2 = 1000), so scale
   on every write/read. Read max_brightness once and cache it; if it's missing
   assume 1000 (= no scaling, the historical behaviour). */
static int bl_hw_max(void) {
    static int cached = -1;
    if (cached > 0) return cached;
    FILE * f = fopen(BL_MAX_PATH, "r");
    int m = 0;
    if (f) { if (fscanf(f, "%d", &m) != 1) m = 0; fclose(f); }
    cached = (m > 0) ? m : 1000;
    return cached;
}

/* ===================== Night mode: sunset/sunrise fetch =====================
   For the sunset->sunrise trigger we geocode the configured weather location
   (open-meteo) and fetch today's sunrise/sunset (UTC) from sunrise-sunset.org,
   caching the epochs. All maths is in UTC (time(NULL) is UTC; sun times fetched
   with formatted=0 = UTC), so the device timezone is irrelevant. The fixed
   time-range trigger uses LOCAL clock time instead and needs no network. */
static volatile long g_sunrise = 0, g_sunset = 0;   /* UTC epochs, 0 = unknown */

/* Days-from-civil (Howard Hinnant) -> epoch; avoids timegm feature-test macros. */
static long utc_epoch(int y, int m, int d, int hh, int mm, int ss) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return days * 86400L + hh * 3600L + mm * 60L + ss;
}

/* Find "key":"<ISO-8601 UTC>" in a JSON body and convert to a UTC epoch. */
static int json_iso_epoch(const char * body, const char * key, long * out) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char * p = strstr(body, pat);
    if (!p) return -1;
    p += strlen(pat);
    int Y, M, D, h, m, s;
    if (sscanf(p, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return -1;
    *out = utc_epoch(Y, M, D, h, m, s);
    return 0;
}

static int sunset_fetch(void) {
    char loc[64];
    strncpy(loc, settings.weather_location, sizeof loc - 1);
    loc[sizeof loc - 1] = 0;
    if (!loc[0]) return -1;                     /* no location set yet */

    /* URL-encode the location name (same scheme as weather.c geocoding). */
    char enc[128]; size_t o = 0;
    for (const char * p = loc; *p && o + 4 < sizeof enc; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            enc[o++] = (char)c;
        else { snprintf(enc + o, 4, "%%%02X", c); o += 3; }
    }
    enc[o] = 0;

    char url[256];
    static char body[4096];
    snprintf(url, sizeof url,
        "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&format=json",
        enc);
    if (http_fetch(url, body, sizeof body) != 0) return -1;
    const char * plat = strstr(body, "\"latitude\":");
    const char * plon = strstr(body, "\"longitude\":");
    if (!plat || !plon) return -1;
    double lat = atof(plat + 11), lon = atof(plon + 12);

    snprintf(url, sizeof url,
        "https://api.sunrise-sunset.org/json?lat=%.5f&lng=%.5f&date=today&formatted=0",
        lat, lon);
    if (http_fetch(url, body, sizeof body) != 0) return -1;

    long sr = 0, ss = 0;
    if (json_iso_epoch(body, "sunrise", &sr) != 0 ||
        json_iso_epoch(body, "sunset",  &ss) != 0) return -1;
    g_sunrise = sr;
    g_sunset  = ss;
    return 0;
}

static void * sunset_thread(void * arg) {
    (void)arg;
    for (;;) {
        if (settings.night_mode && settings.night_source == 1) {
            if (sunset_fetch() == 0) {
                fprintf(stderr, "[bl] sun times (utc): rise=%ld set=%ld now=%ld\n",
                        g_sunrise, g_sunset, (long)time(NULL));
                sleep(6 * 3600);    /* refresh a few times a day for new times */
            } else {
                sleep(600);         /* no network / no location yet — retry soon */
            }
        } else {
            sleep(60);              /* idle: poll for the mode being switched on */
        }
    }
    return NULL;
}

void backlight_sun_times(long * sunrise, long * sunset) {
    if (sunrise) *sunrise = g_sunrise;
    if (sunset)  *sunset  = g_sunset;
}

/* 1 when the screen should be dimmed for Night mode right now. */
int backlight_night_active(void) {
    if (!settings.night_mode) return 0;
    if (settings.night_source == 1) {           /* sunset -> sunrise (UTC epochs) */
        long sr = g_sunrise, ss = g_sunset;
        if (!sr || !ss) return 0;               /* not fetched yet -> treat as day */
        long now = (long)time(NULL);
        return !(now >= sr && now < ss);        /* night = before sunrise / after sunset */
    }
    /* fixed LOCAL time range [start, end) */
    {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        int cur = lt.tm_hour * 60 + lt.tm_min;
        int s = settings.night_start, e = settings.night_end;
        if (s == e) return 0;
        if (s < e) return (cur >= s && cur < e);
        return (cur >= s || cur < e);           /* range wraps past midnight */
    }
}

void backlight_set(int level) {
    if (level < 0)    level = 0;
    if (level > 1000) level = 1000;
    if (backlight_night_active()) {             /* Night mode: scale to night_pct% */
        int pct = settings.night_pct;
        if (pct < 1)   pct = 1;
        if (pct > 100) pct = 100;
        level = level * pct / 100;
    }
    int hw = level * bl_hw_max() / 1000;        /* logical 0..1000 -> 0..max */
    if (hw < BL_MIN_HW) hw = BL_MIN_HW;         /* safety floor (never fully dark) */
    FILE * f = fopen(BL_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", hw);
    fclose(f);
}

int backlight_get(void) {
    FILE * f = fopen(BL_PATH, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    if (v < 0) return -1;
    return v * 1000 / bl_hw_max();              /* hw 0..max -> logical 0..1000 */
}

#ifndef TOON1
/* ===================== Toon 2: LTR-303 light sensor ===================== */

/* The LTR-303 read takes ~0.5 s (sensor integration time), so it must NEVER run
   on the UI thread — a background poller caches it and the UI reads the cache. */
static volatile int g_als_cache = -1;

/* The actual (slow, blocking) sensor read. Background thread only. */
static int als_read_slow(void) {
    for (int i = 0; i < 8; i++) {
        char pn[96]; snprintf(pn, sizeof pn, "/sys/bus/iio/devices/iio:device%d/name", i);
        FILE * f = fopen(pn, "r"); if (!f) continue;
        char nm[32] = ""; if (fscanf(f, "%31s", nm) != 1) nm[0] = 0; fclose(f);
        if (strcmp(nm, "ltr303") != 0) continue;
        char rp[120]; snprintf(rp, sizeof rp, "/sys/bus/iio/devices/iio:device%d/in_intensity_both_raw", i);
        FILE * g = fopen(rp, "r"); if (!g) return -1;
        int v = -1; if (fscanf(g, "%d", &v) != 1) v = -1; fclose(g);
        return v;
    }
    return -1;
}

/* Background poller: does the slow read off the UI thread, caches the result. */
static void * als_thread(void * arg) {
    (void)arg;
    for (;;) { g_als_cache = als_read_slow(); sleep(3); }
    return NULL;
}

int backlight_als_raw(void) { return g_als_cache; }

/* Map ambient light to a backlight level between the user's dim/active bounds.
   Returns -1 when there's no sensor yet (caller falls back to the fixed value). */
int backlight_auto_level(int dim, int active) {
    int raw = backlight_als_raw();
    if (raw < 0) return -1;
    /* Gentler curve: a normally-lit room (raw ~120+) already reaches full
       brightness; only a genuinely dark room drops toward the dim level. */
    const int RAW_FULL = 130;          /* raw at/above which we go full-bright */
    if (raw > RAW_FULL) raw = RAW_FULL;
    if (active < dim) { int t = active; active = dim; dim = t; }
    return dim + (active - dim) * raw / RAW_FULL;
}

#else  /* ===================== Toon 1: no light sensor ===================== */

int backlight_als_raw(void) { return -1; }
int backlight_auto_level(int dim, int active) { (void)dim; (void)active; return -1; }

#endif  /* TOON1 */

void backlight_als_start(void) {
    pthread_t t;
    /* Night-mode sunset fetcher (both boards; idles unless sunset trigger on). */
    if (pthread_create(&t, NULL, sunset_thread, NULL) == 0) pthread_detach(t);
#ifndef TOON1
    /* Toon 2 ambient-light poller. */
    if (pthread_create(&t, NULL, als_thread, NULL) == 0) pthread_detach(t);
#endif
}
