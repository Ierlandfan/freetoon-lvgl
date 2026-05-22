#include "backlight.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define BL_PATH "/sys/class/backlight/mp3309-bl/brightness"

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
void backlight_als_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, als_thread, NULL) == 0) pthread_detach(t);
}

/* Non-blocking: returns the cached value (-1 until the poller's first read). */
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

void backlight_set(int level) {
    if (level < 0)    level = 0;
    if (level > 1000) level = 1000;
    FILE * f = fopen(BL_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", level);
    fclose(f);
}

int backlight_get(void) {
    FILE * f = fopen(BL_PATH, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}
