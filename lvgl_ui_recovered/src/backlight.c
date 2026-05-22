#include "backlight.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BL_PATH "/sys/class/backlight/mp3309-bl/brightness"

/* Raw ambient-light reading from the Toon 2's LTR-303 sensor, or -1 if absent
   (Toon 1 has none). Found by name so the iio:deviceN index can shift. */
int backlight_als_raw(void) {
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

/* Map ambient light to a backlight level between the user's dim/active bounds.
   Returns -1 when there's no sensor (caller falls back to the fixed value). */
int backlight_auto_level(int dim, int active) {
    int raw = backlight_als_raw();
    if (raw < 0) return -1;
    const int RAW_FULL = 400;          /* raw at/above which we go full-bright */
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
