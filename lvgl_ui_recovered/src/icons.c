/* Single-TU wrapper that pulls in the generated bitmap headers. */
#include "icons.h"
#include "icon_flame.h"
#include "icon_faucet.h"
#include "icon_drop.h"
#include "icon_fan.h"
#include "icon_radiator.h"
#include "icon_wx_sun.h"
#include "icon_wx_sun_cloud.h"
#include "icon_wx_cloud.h"
#include "icon_wx_rain_light.h"
#include "icon_wx_rain_heavy.h"
#include "icon_wx_thunder.h"
#include "icon_wx_bolt.h"
#include "icon_wx_moon.h"
#include "icon_wx_moon_new.h"
#include "icon_wx_moon_wax_cres.h"
#include "icon_wx_moon_first_q.h"
#include "icon_wx_moon_wax_gib.h"
#include "icon_wx_moon_full.h"
#include "icon_wx_moon_wan_gib.h"
#include "icon_wx_moon_last_q.h"
#include "icon_wx_moon_wan_cres.h"
#include "icon_wx_snow.h"
#include "icon_wx_fog.h"
#include "icon_wx_sun_lg.h"
#include "icon_wx_sun_cloud_lg.h"
#include "icon_wx_cloud_lg.h"
#include "icon_wx_rain_light_lg.h"
#include "icon_wx_rain_heavy_lg.h"
#include "icon_wx_thunder_lg.h"
#include "icon_wx_bolt_lg.h"
#include "icon_wx_moon_lg.h"
#include "icon_wx_moon_new_lg.h"
#include "icon_wx_moon_wax_cres_lg.h"
#include "icon_wx_moon_first_q_lg.h"
#include "icon_wx_moon_wax_gib_lg.h"
#include "icon_wx_moon_full_lg.h"
#include "icon_wx_moon_wan_gib_lg.h"
#include "icon_wx_moon_last_q_lg.h"
#include "icon_wx_moon_wan_cres_lg.h"
#include "icon_wx_snow_lg.h"
#include "icon_wx_fog_lg.h"
#include "icon_trash_lg.h"
#include "icon_trash.h"
#include "icon_news.h"
#include "icon_milk.h"
#include "icon_leaf.h"
#include "icon_wind_arrow.h"
#include <string.h>
#include <time.h>
#include <math.h>

int wind_dir_angle(const char * dir) {
    if (!dir || !*dir) return -1;
    /* Buienradar codes (Dutch): N/O/Z/W. Two-letter combos for diagonals,
     * three-letter for the in-between bearings. Compass north = 0°,
     * clockwise positive. LVGL uses 0.1° units. */
    static const struct { const char * c; int deg; } map[] = {
        {"N",      0}, {"NNO",  22}, {"NO",   45}, {"ONO",  67},
        {"O",     90}, {"OZO", 112}, {"ZO",  135}, {"ZZO", 157},
        {"Z",    180}, {"ZZW", 202}, {"ZW",  225}, {"WZW", 247},
        {"W",    270}, {"WNW", 292}, {"NW",  315}, {"NNW", 337},
        /* English fallbacks in case the buienradar locale changes. */
        {"NE",    45}, {"SE",  135}, {"S",   180}, {"SW",  225},
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++)
        if (strcmp(dir, map[i].c) == 0) return map[i].deg * 10;
    return -1;
}

/* Map a waste-type label to the icon and accent colour used on the home
 * tile. Match by prefix so localisation quirks ("Plastic" vs "PMD") still
 * route to the same artwork. Unknown labels fall back to a grey trashcan. */
const lv_img_dsc_t * waste_icon_for_label(const char * label) {
    if (!label) return &icon_trash;
    if (!strncasecmp(label, "Pap",  3)) return &icon_news;   /* Papier */
    if (!strncasecmp(label, "PMD",  3)) return &icon_milk;
    if (!strncasecmp(label, "Plas", 4)) return &icon_milk;
    if (!strncasecmp(label, "GFT",  3)) return &icon_leaf;
    return &icon_trash;
}

unsigned int waste_accent_for_label(const char * label) {
    if (!label) return 0x888888;
    if (!strncasecmp(label, "Pap",  3)) return 0x44aaff;     /* blue */
    if (!strncasecmp(label, "PMD",  3)) return 0xffaa44;     /* orange */
    if (!strncasecmp(label, "Plas", 4)) return 0xffaa44;
    if (!strncasecmp(label, "GFT",  3)) return 0x88dd66;     /* green */
    return 0x888888;                                          /* Restafval */
}

/* Buienradar code → icon. Mapping from their public documentation
   (https://www.buienradar.nl/overbuienradar/gratis-weerdata).
   Night variants (double-letter codes "aa","bb","dd",...) substitute a
   moon for the sun so the icon doesn't show a glowing sun at midnight.
   Partly-cloudy 'bb' returns the plain cloud — for the bicolor home-
   forecast slot a separate moon overlay is layered on top via
   set_forecast_icon(), but the single-icon callers (dim screen) get a
   clean cloud-only fallback. */
static int wx_is_night(const char * letter) {
    return letter && letter[0] && letter[1] == letter[0];
}

const lv_img_dsc_t * weather_icon_for(const char * letter) {
    if (!letter || !letter[0]) return &icon_wx_cloud;
    int night = wx_is_night(letter);
    switch (letter[0]) {
        case 'a':                                       /* zonnig / helder */
            return night ? moon_phase_icon(40) : &icon_wx_sun;
        case 'b':                                       /* half bewolkt */
        case 'j':                                       /* half bewolkt nacht */
            return night ? &icon_wx_cloud : &icon_wx_sun_cloud;
        case 'c':                                       /* half bewolkt + regen */
        case 'f':                                       /* afwisselend bewolkt */
        case 'l':                                       /* mist + lichte regen */
            return &icon_wx_rain_light;
        case 'd':                                       /* overwegend bewolkt */
        case 'r':                                       /* zwaar bewolkt */
            return &icon_wx_cloud;
        case 'e':                                       /* bewolkt + regen */
        case 'h':                                       /* zware buien */
        case 'q':                                       /* regen */
            return &icon_wx_rain_heavy;
        case 'g':                                       /* onweer */
        case 'm':                                       /* onweerbuien */
            return &icon_wx_thunder;
        case 'n':                                       /* sneeuw */
        case 'p':                                       /* sneeuwbuien */
        case 'u':                                       /* sneeuw / hagel */
            return &icon_wx_snow;
        case 'k':                                       /* mist */
        case 'i':                                       /* heiig */
            return &icon_wx_fog;
        default:
            return &icon_wx_cloud;
    }
}

const lv_img_dsc_t * weather_icon_for_lg(const char * letter) {
    if (!letter || !letter[0]) return &icon_wx_cloud_lg;
    int night = wx_is_night(letter);
    switch (letter[0]) {
        case 'a': return night ? moon_phase_icon(80) : &icon_wx_sun_lg;
        case 'b': case 'j': return night ? &icon_wx_cloud_lg : &icon_wx_sun_cloud_lg;
        case 'c': case 'f': case 'l': return &icon_wx_rain_light_lg;
        case 'd': case 'r': return &icon_wx_cloud_lg;
        case 'e': case 'h': case 'q': return &icon_wx_rain_heavy_lg;
        case 'g': case 'm': return &icon_wx_thunder_lg;
        case 'n': case 'p': case 'u': return &icon_wx_snow_lg;
        case 'k': case 'i': return &icon_wx_fog_lg;
        default:  return &icon_wx_cloud_lg;
    }
}

/* Approximate moon phase 0..1 (0=new, 0.5=full) for the current UTC date.
 * Uses Conway's simple algorithm — accurate to about ±1 day, more than
 * enough to pick the right icon out of 8 slots. */
static double current_moon_phase01(void) {
    time_t now = time(NULL);
    struct tm tm; gmtime_r(&now, &tm);
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1, d = tm.tm_mday;
    if (m < 3) { y--; m += 12; }
    double r = y % 100;
    r = r * 1.2848 + d + 0.4115 * (m + 1);
    r += (y > 1999) ? 8.3115 : 9.3115;
    r = r - (int)(r / 30.0) * 30.0;
    return r / 30.0;  /* 0..1 across the lunar month */
}

const lv_img_dsc_t * moon_phase_icon(int size_px) {
    double p = current_moon_phase01();
    /* Map 0..1 to 8 phase slots — quantise to nearest 1/8th. */
    int slot = (int)(p * 8.0 + 0.5) % 8;
    if (size_px >= 60) {
        switch (slot) {
            case 0: return &icon_wx_moon_new_lg;
            case 1: return &icon_wx_moon_wax_cres_lg;
            case 2: return &icon_wx_moon_first_q_lg;
            case 3: return &icon_wx_moon_wax_gib_lg;
            case 4: return &icon_wx_moon_full_lg;
            case 5: return &icon_wx_moon_wan_gib_lg;
            case 6: return &icon_wx_moon_last_q_lg;
            case 7: return &icon_wx_moon_wan_cres_lg;
        }
        return &icon_wx_moon_lg;
    }
    switch (slot) {
        case 0: return &icon_wx_moon_new;
        case 1: return &icon_wx_moon_wax_cres;
        case 2: return &icon_wx_moon_first_q;
        case 3: return &icon_wx_moon_wax_gib;
        case 4: return &icon_wx_moon_full;
        case 5: return &icon_wx_moon_wan_gib;
        case 6: return &icon_wx_moon_last_q;
        case 7: return &icon_wx_moon_wan_cres;
    }
    return &icon_wx_moon;
}

unsigned int weather_icon_color_for(const char * letter) {
    if (!letter || !letter[0]) return 0xc8d4e0;
    switch (letter[0]) {
        case 'a':                       return 0xffd24a;   /* sun        — warm yellow */
        case 'b': case 'j':             return 0xf2c66a;   /* sun+cloud  — softer yellow */
        case 'c': case 'f': case 'l':   return 0x6aa8ff;   /* rain_light — sky blue   */
        case 'd': case 'r':             return 0xc8d4e0;   /* cloud      — blue-grey  */
        case 'e': case 'h': case 'q':   return 0x3a78d6;   /* rain_heavy — deep blue  */
        case 'g': case 'm':             return 0xffb938;   /* thunder    — amber bolt */
        case 'n': case 'p': case 'u':   return 0xe8f0ff;   /* snow/hail  — ice white  */
        case 'k': case 'i':             return 0x9aa6b3;   /* fog        — neutral grey */
        default:                        return 0xc8d4e0;
    }
}
