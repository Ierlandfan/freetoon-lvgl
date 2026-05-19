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
#include "icon_wx_snow.h"
#include "icon_wx_fog.h"
#include "icon_wx_sun_lg.h"
#include "icon_wx_sun_cloud_lg.h"
#include "icon_wx_cloud_lg.h"
#include "icon_wx_rain_light_lg.h"
#include "icon_wx_rain_heavy_lg.h"
#include "icon_wx_thunder_lg.h"
#include "icon_wx_bolt_lg.h"
#include "icon_wx_snow_lg.h"
#include "icon_wx_fog_lg.h"
#include "icon_trash_lg.h"
#include "icon_trash.h"
#include "icon_news.h"
#include "icon_milk.h"
#include "icon_leaf.h"
#include "icon_wind_arrow.h"
#include <string.h>

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
   (https://www.buienradar.nl/overbuienradar/gratis-weerdata). */
const lv_img_dsc_t * weather_icon_for(const char * letter) {
    if (!letter || !letter[0]) return &icon_wx_cloud;
    switch (letter[0]) {
        case 'a':                                       /* zonnig / helder */
            return &icon_wx_sun;
        case 'b':                                       /* half bewolkt */
        case 'j':                                       /* half bewolkt nacht */
            return &icon_wx_sun_cloud;
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
    switch (letter[0]) {
        case 'a': return &icon_wx_sun_lg;
        case 'b': case 'j': return &icon_wx_sun_cloud_lg;
        case 'c': case 'f': case 'l': return &icon_wx_rain_light_lg;
        case 'd': case 'r': return &icon_wx_cloud_lg;
        case 'e': case 'h': case 'q': return &icon_wx_rain_heavy_lg;
        case 'g': case 'm': return &icon_wx_thunder_lg;
        case 'n': case 'p': case 'u': return &icon_wx_snow_lg;
        case 'k': case 'i': return &icon_wx_fog_lg;
        default:  return &icon_wx_cloud_lg;
    }
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
