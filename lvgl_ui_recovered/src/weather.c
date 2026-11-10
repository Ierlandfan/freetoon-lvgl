/*
 * Weather poller. Current conditions + 5-day + hourly forecast come from
 * Open-Meteo by the configured location's EXACT lat/lon (resolved once via the
 * Open-Meteo geocoder) — so the temperature is the city's, not a KNMI station
 * 15+ km away. The buienradar feed is fetched only for the radar-map URL and the
 * Dutch weather-report narrative. WMO weather-codes map to our a..z icon set.
 *
 * No JSON library: small strstr-based extractors (incl. array indexers for
 * Open-Meteo's columnar arrays). Both feeds have stable shapes.
 */
#include "weather.h"
#include "shell_util.h"
#include "http.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <unistd.h>
#include <pthread.h>
#include <time.h>

weather_state_t weather_state = {0};
static pthread_mutex_t g_wx_mtx = PTHREAD_MUTEX_INITIALIZER;

void weather_lock(void) {
    pthread_mutex_lock(&g_wx_mtx);
}

void weather_unlock(void) {
    pthread_mutex_unlock(&g_wx_mtx);
}

void weather_get_snapshot(weather_state_t * out) {
    if (!out) return;
    pthread_mutex_lock(&g_wx_mtx);
    *out = weather_state;
    pthread_mutex_unlock(&g_wx_mtx);
}

float weather_display_temp(void) {
    if (settings.weather_use_feel)
        return weather_state.feel_temp;
    return weather_state.current_temp;
}

float weather_hour_display_temp(const weather_hour_t * h) {
    if (!h) return 0.0f;
    if (settings.weather_use_feel)
        return h->feel_temp;
    return h->temperature;
}

/* The configured location's exact lat/lon, resolved by the Open-Meteo geocoder
 * (weather_geocode). Drives the Open-Meteo current+forecast fetch. 0 = unresolved. */
static double g_cfg_lat = 0.0, g_cfg_lon = 0.0;
static volatile int g_wx_wake = 0;

void weather_get_lat_lon(double * lat, double * lon) {
    if (lat) *lat = (g_cfg_lat != 0.0 ? g_cfg_lat : (settings.weather_lat[0] ? atof(settings.weather_lat) : 0.0));
    if (lon) *lon = (g_cfg_lon != 0.0 ? g_cfg_lon : (settings.weather_lon[0] ? atof(settings.weather_lon) : 0.0));
}

/* Case-insensitive strstr. Buienradar's data feed switched from lowercase keys
 * ("stationid","temperature","iconurl","actualradarurl"…) to PascalCase
 * ("StationId","Temperature","IconUrl","ActualRadarUrl"…), which silently broke
 * the current-weather + radar parse. Matching keys case-insensitively makes the
 * parser survive either casing (and any future flip). */
static const char * ci_strstr(const char * hay, const char * needle) {
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, nl) == 0) return hay;
    return NULL;
}

/* Scan a substring of json for "key":<number>; returns parsed double, dflt if missing. */
static double js_num(const char * begin, const char * end, const char * key, double dflt) {
    char n[64];
    snprintf(n, sizeof(n), "\"%s\":", key);
    const char * p = ci_strstr(begin, n);
    if (!p || p >= end) return dflt;
    p += strlen(n);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n' || *p == '"') {
        /* "null" or quoted-number like "13" — handle both */
        if (*p == '"') p++; else return dflt;
    }
    return strtod(p, NULL);
}

/* Scan a substring for "key":"VALUE" — copies into out (max outsz-1). */
static int js_str(const char * begin, const char * end, const char * key,
                  char * out, size_t outsz) {
    char n[64];
    snprintf(n, sizeof(n), "\"%s\":\"", key);
    const char * p = ci_strstr(begin, n);
    if (!p || p >= end) { if (outsz) out[0] = 0; return 0; }
    p += strlen(n);
    const char * e = p;
    while (e < end && *e != '"') {
        if (*e == '\\' && e + 1 < end) e++;
        e++;
    }
    /* Decode the JSON string escapes while copying. Buienradar escapes URLs as
     * "https:\/\/…RadarMapNL?w=500&h=512" — copied raw, the literal \/ and
     * & make curl reject the radar/icon URL ("radar fetch failed"). */
    size_t o = 0;
    for (const char * q = p; q < e && o < outsz - 1; q++) {
        if (*q == '\\' && q + 1 < e) {
            char c = q[1];
            if (c == '/' || c == '"' || c == '\\') { out[o++] = c; q++; }
            else if (c == 'n' || c == 't' || c == 'r') { out[o++] = ' '; q++; }
            else if (c == 'u' && q + 5 < e) {
                unsigned v = 0; int ok = 1;
                for (int k = 2; k <= 5; k++) {
                    char h = q[k]; v <<= 4;
                    if      (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else { ok = 0; break; }
                }
                if (ok && v && v < 0x80) { out[o++] = (char)v; q += 5; }
                else                     { out[o++] = *q; }   /* non-ASCII \uXXXX: leave as-is */
            }
            else { out[o++] = *q; }       /* unknown escape: keep the backslash */
        } else {
            out[o++] = *q;
        }
    }
    out[o] = 0;
    return 1;
}

/* Compute Dutch short-day label "ma 13-5" from a "2026-05-14T00:00:00" string. */
static const char * dutch_dow[] = {"zo","ma","di","wo","do","vr","za"};
static void format_day_label(const char * iso_date, char * out, size_t outsz) {
    if (strlen(iso_date) < 10) { snprintf(out, outsz, "?"); return; }
    int y = atoi(iso_date);
    int mo = atoi(iso_date + 5);
    int d = atoi(iso_date + 8);
    /* Zeller's congruence for day of week */
    int yy = (mo < 3) ? y - 1 : y;
    int mm = (mo < 3) ? mo + 12 : mo;
    int K = yy % 100, J = yy / 100;
    int h = (d + (13*(mm+1))/5 + K + K/4 + J/4 + 5*J) % 7;  /* 0=Sat,1=Sun,2=Mon */
    int dow = (h + 6) % 7;  /* 0=Sun..6=Sat */
    snprintf(out, outsz, "%s %d-%d", dutch_dow[dow], d, mo);
}

/* ---- Open-Meteo: exact-point current + forecast by lat/lon -----------------
 * The buienradar station feed only reports ~52 fixed KNMI stations, so its
 * "current" temperature can be a station 15+ km away. Open-Meteo interpolates to
 * the exact configured lat/lon (KNMI HARMONIE 2 km model over NL) → the city's
 * actual temperature. One call returns current + 5-day + hourly. */

/* WMO weather-code -> our buienradar icon letter + NL/EN description. The icon
 * renderer (weather_icon_for) keys on a..z; a DOUBLED letter ("aa") = night. */
struct wmo_row { int code; char letter; const char * nl; const char * en; };
static const struct wmo_row WMO[] = {
    {0,'a',"Onbewolkt","Clear"}, {1,'a',"Overwegend onbewolkt","Mainly clear"},
    {2,'b',"Half bewolkt","Partly cloudy"}, {3,'r',"Zwaar bewolkt","Overcast"},
    {45,'k',"Mist","Fog"}, {48,'k',"Aanvriezende mist","Rime fog"},
    {51,'c',"Lichte motregen","Light drizzle"}, {53,'c',"Motregen","Drizzle"},
    {55,'c',"Dichte motregen","Dense drizzle"}, {56,'c',"Aanvriezende motregen","Freezing drizzle"},
    {57,'c',"Aanvriezende motregen","Dense freezing drizzle"},
    {61,'q',"Lichte regen","Slight rain"}, {63,'q',"Regen","Rain"},
    {65,'e',"Zware regen","Heavy rain"}, {66,'q',"Aanvriezende regen","Freezing rain"},
    {67,'e',"Zware aanvriezende regen","Heavy freezing rain"},
    {71,'n',"Lichte sneeuw","Slight snow"}, {73,'n',"Sneeuw","Snow"},
    {75,'p',"Zware sneeuw","Heavy snow"}, {77,'n',"Sneeuwkorrels","Snow grains"},
    {80,'c',"Lichte buien","Slight showers"}, {81,'q',"Buien","Showers"},
    {82,'h',"Zware buien","Violent showers"},
    {85,'p',"Sneeuwbuien","Snow showers"}, {86,'p',"Zware sneeuwbuien","Heavy snow showers"},
    {95,'g',"Onweer","Thunderstorm"}, {96,'m',"Onweer met hagel","Thunderstorm with hail"},
    {99,'m',"Zwaar onweer met hagel","Heavy thunderstorm with hail"},
};
static const struct wmo_row * wmo_lookup(int code) {
    for (size_t i = 0; i < sizeof(WMO)/sizeof(WMO[0]); i++)
        if (WMO[i].code == code) return &WMO[i];
    return &WMO[3];  /* unknown -> overcast */
}
static void wmo_icon(int code, int is_day, char * out /* >=4 bytes */) {
    char l = wmo_lookup(code)->letter;
    out[0] = l;
    if (is_day) out[1] = 0;
    else { out[1] = l; out[2] = 0; }   /* doubled letter = night variant */
}
static const char * wmo_desc(int code) {
    const struct wmo_row * w = wmo_lookup(code);
    return settings.language ? w->en : w->nl;
}
static int kmh_to_bft(double kmh) {
    static const double t[12] = {1,6,12,20,29,39,50,62,75,89,103,118};
    int b = 0; for (int i = 0; i < 12; i++) if (kmh >= t[i]) b = i + 1; return b;
}
static void deg_to_nl(double deg, char * out, size_t outsz) {
    static const char * c[16] = {"N","NNO","NO","ONO","O","OZO","ZO","ZZO",
                                 "Z","ZZW","ZW","WZW","W","WNW","NW","NNW"};
    int i = ((int)((deg + 11.25) / 22.5)) & 15;
    snprintf(out, outsz, "%s", c[i]);
}
/* index-th numeric element of  "key":[ a, b, c, ... ]  at/after begin. */
static double js_arr_num(const char * begin, const char * key, int index, double dflt) {
    char n[64]; snprintf(n, sizeof n, "\"%s\":[", key);
    const char * p = strstr(begin, n);
    if (!p) return dflt;
    p += strlen(n);
    const char * ae = strchr(p, ']'); if (!ae) return dflt;
    for (int i = 0; i < index; i++) { p = strchr(p, ','); if (!p || p > ae) return dflt; p++; }
    while (*p == ' ') p++;
    if (*p == ']' || p > ae) return dflt;
    return strtod(p, NULL);
}
/* index-th quoted-string element of  "key":[ "..", .. ] . */
static int js_arr_str(const char * begin, const char * key, int index, char * out, size_t outsz) {
    if (outsz) out[0] = 0;
    char n[64]; snprintf(n, sizeof n, "\"%s\":[", key);
    const char * p = strstr(begin, n);
    if (!p) return 0;
    p += strlen(n);
    const char * ae = strchr(p, ']'); if (!ae) return 0;
    for (int i = 0; i < index; i++) { p = strchr(p, ','); if (!p || p > ae) return 0; p++; }
    p = strchr(p, '"'); if (!p || p > ae) return 0;
    p++;
    size_t o = 0; while (*p && *p != '"' && o < outsz - 1) out[o++] = *p++;
    out[o] = 0;
    return 1;
}

static int parse_openmeteo(const char * body) {
    /* current conditions (exact point) */
    const char * cur = strstr(body, "\"current\":");
    if (!cur) return -1;
    const char * cur_end = strchr(cur, '}');
    if (!cur_end) cur_end = body + strlen(body);
    weather_state.current_temp = (float)js_num(cur, cur_end, "temperature_2m", 0);
    weather_state.feel_temp    = (float)js_num(cur, cur_end, "apparent_temperature", 0);
    int ccode  = (int)js_num(cur, cur_end, "weather_code", 3);
    int is_day = (int)js_num(cur, cur_end, "is_day", 1);
    wmo_icon(ccode, is_day, weather_state.current_icon);
    snprintf(weather_state.current_desc, sizeof weather_state.current_desc, "%s", wmo_desc(ccode));

    /* 5-day daily */
    const char * dy = strstr(body, "\"daily\":");
    int dc = 0;
    if (dy) for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        char iso[16];
        if (!js_arr_str(dy, "time", i, iso, sizeof iso)) break;
        weather_day_t * D = &weather_state.days[dc];
        memset(D, 0, sizeof *D);
        format_day_label(iso, D->day, sizeof D->day);
        strncpy(D->date_iso, iso, sizeof(D->date_iso) - 1);
        D->min_temp    = (float)js_arr_num(dy, "temperature_2m_min", i, 0);
        D->max_temp    = (float)js_arr_num(dy, "temperature_2m_max", i, 0);
        D->rain_chance = (int)js_arr_num(dy, "precipitation_probability_max", i, 0);
        D->wind_bft    = kmh_to_bft(js_arr_num(dy, "wind_speed_10m_max", i, 0));
        D->wind_deg    = (int)js_arr_num(dy, "wind_direction_10m_dominant", i, 0);
        deg_to_nl(D->wind_deg, D->wind_dir, sizeof D->wind_dir);
        int code = (int)js_arr_num(dy, "weather_code", i, 3);
        wmo_icon(code, 1, D->icon);
        snprintf(D->desc, sizeof D->desc, "%s", wmo_desc(code));
        dc++;
    }
    weather_state.day_count = dc;

    /* hourly: Open-Meteo's hourly[] starts at 00:00 today (timezone set), 1-hour steps.
       Populate 24 hours per day for each day in weather_state.days[]. */
    const char * hr = strstr(body, "\"hourly\":");
    if (hr) {
        for (int d = 0; d < dc && d < WEATHER_FORECAST_DAYS; d++) {
            weather_day_t * D = &weather_state.days[d];
            D->hour_count = 0;
            for (int h = 0; h < 24; h++) {
                int idx = d * 24 + h;
                char iso[24];
                if (!js_arr_str(hr, "time", idx, iso, sizeof iso)) break;
                weather_hourly_slot_t * H = &D->hours[h];
                memset(H, 0, sizeof *H);
                if (strlen(iso) >= 16) snprintf(H->label, sizeof H->label, "%.5s", iso + 11);
                else snprintf(H->label, sizeof H->label, "%02d:00", h);
                H->temperature = (float)js_arr_num(hr, "temperature_2m", idx, 0);
                H->feel_temp   = (float)js_arr_num(hr, "apparent_temperature", idx, H->temperature);
                H->rain_chance = (int)js_arr_num(hr, "precipitation_probability", idx, 0);
                H->rain_mm     = (float)js_arr_num(hr, "precipitation", idx, 0);
                H->wind_bft    = kmh_to_bft(js_arr_num(hr, "wind_speed_10m", idx, 0));
                H->wind_deg    = (int)js_arr_num(hr, "wind_direction_10m", idx, 0);
                deg_to_nl(H->wind_deg, H->wind_dir, sizeof H->wind_dir);
                int code       = (int)js_arr_num(hr, "weather_code", idx, 3);
                int is_day     = (h >= 7 && h <= 21);
                wmo_icon(code, is_day, H->icon);
                D->hour_count++;
            }
        }

        /* Legacy/Home strip: 6 slots, 3-hour steps from the current local hour */
        int hc = 0;
        time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
        for (int s = 0; s < WEATHER_FORECAST_HOURS; s++) {
            int idx = lt.tm_hour + s * 3;
            char iso[24];
            if (!js_arr_str(hr, "time", idx, iso, sizeof iso)) break;
            weather_hour_t * H = &weather_state.hours[hc];
            memset(H, 0, sizeof *H);
            if (strlen(iso) >= 16) snprintf(H->label, sizeof H->label, "%.5s", iso + 11);
            else snprintf(H->label, sizeof H->label, "%.7s", iso);
            H->temperature = (float)js_arr_num(hr, "temperature_2m", idx, 0);
            H->feel_temp   = (float)js_arr_num(hr, "apparent_temperature", idx, H->temperature);
            H->wind_bft    = kmh_to_bft(js_arr_num(hr, "wind_speed_10m", idx, 0));
            deg_to_nl(js_arr_num(hr, "wind_direction_10m", idx, 0), H->wind_dir, sizeof H->wind_dir);
            wmo_icon((int)js_arr_num(hr, "weather_code", idx, 3), 1, H->icon);
            hc++;
        }
        weather_state.hour_count = hc;
    }
    return 0;
}

static int fetch_openmeteo(void) {
    if (g_cfg_lat == 0.0 && g_cfg_lon == 0.0) return -1;
    char url[512];
    snprintf(url, sizeof url,
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&timezone=Europe%%2FAmsterdam&forecast_days=5"
        "&current=temperature_2m,apparent_temperature,weather_code,is_day"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
        "wind_speed_10m_max,wind_direction_10m_dominant"
        "&hourly=temperature_2m,apparent_temperature,precipitation_probability,precipitation,weather_code,wind_speed_10m,wind_direction_10m",
        g_cfg_lat, g_cfg_lon);
    static char body[56 * 1024];
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) sleep(2);
        if (http_fetch(url, body, sizeof body) != 0) continue;
        weather_lock();
        int rc = parse_openmeteo(body);
        weather_unlock();
        if (rc == 0) return 0;
    }
    return -1;
}

/* Decode HTML entities, convert UTF-8 diacritics to clean ASCII for fonts,
 * strip HTML tags, and insert paragraph breaks between glued sentences. */
static void sanitize_weather_text(char * s, size_t maxlen) {
    if (!s || !*s || maxlen < 4) return;
    char * buf = (char *)malloc(maxlen + 512);
    if (!buf) return;
    size_t o = 0;
    const char * r = s;
    while (*r && o < maxlen - 4) {
        /* Fix missing space / paragraph break between sentence and next word:
         * e.g. "km/u.Vanochtend" -> "km/u.\n\nVanochtend" */
        if ((*r == '.' || *r == '!' || *r == '?') && (r[1] >= 'A' && r[1] <= 'Z')) {
            buf[o++] = *r++;
            buf[o++] = '\n';
            buf[o++] = '\n';
            continue;
        }

        /* HTML entities */
        if (*r == '&') {
            const char * semi = strchr(r, ';');
            if (semi && (semi - r) <= 12) {
                int len = (int)(semi - r + 1);
                char ent[16];
                memcpy(ent, r, len);
                ent[len] = '\0';

                char rep = 0;
                if (!strcasecmp(ent, "&nbsp;"))        rep = ' ';
                else if (!strcasecmp(ent, "&eacute;")) rep = 'e';
                else if (!strcasecmp(ent, "&egrave;")) rep = 'e';
                else if (!strcasecmp(ent, "&euml;"))   rep = 'e';
                else if (!strcasecmp(ent, "&ecirc;"))  rep = 'e';
                else if (!strcasecmp(ent, "&aacute;")) rep = 'a';
                else if (!strcasecmp(ent, "&agrave;")) rep = 'a';
                else if (!strcasecmp(ent, "&auml;"))   rep = 'a';
                else if (!strcasecmp(ent, "&acirc;"))  rep = 'a';
                else if (!strcasecmp(ent, "&oacute;")) rep = 'o';
                else if (!strcasecmp(ent, "&ograve;")) rep = 'o';
                else if (!strcasecmp(ent, "&ouml;"))   rep = 'o';
                else if (!strcasecmp(ent, "&ocirc;"))  rep = 'o';
                else if (!strcasecmp(ent, "&iacute;")) rep = 'i';
                else if (!strcasecmp(ent, "&igrave;")) rep = 'i';
                else if (!strcasecmp(ent, "&iuml;"))   rep = 'i';
                else if (!strcasecmp(ent, "&icirc;"))  rep = 'i';
                else if (!strcasecmp(ent, "&uacute;")) rep = 'u';
                else if (!strcasecmp(ent, "&ugrave;")) rep = 'u';
                else if (!strcasecmp(ent, "&uuml;"))   rep = 'u';
                else if (!strcasecmp(ent, "&ucirc;"))  rep = 'u';
                else if (!strcasecmp(ent, "&ccedil;")) rep = 'c';
                else if (!strcasecmp(ent, "&amp;"))    rep = '&';
                else if (!strcasecmp(ent, "&quot;"))   rep = '"';
                else if (!strcasecmp(ent, "&apos;"))   rep = '\'';
                else if (!strcasecmp(ent, "&lt;"))     rep = '<';
                else if (!strcasecmp(ent, "&gt;"))     rep = '>';
                else if (!strcasecmp(ent, "&deg;")) {
                    buf[o++] = (char)0xC2; buf[o++] = (char)0xB0; /* degree symbol */
                    r += len;
                    continue;
                } else if (ent[1] == '#') {
                    int val = atoi(ent + 2);
                    if (val > 0 && val < 128) rep = (char)val;
                }

                if (rep) {
                    if (r[1] >= 'A' && r[1] <= 'Z' && rep >= 'a' && rep <= 'z')
                        rep = rep - 'a' + 'A';
                    buf[o++] = rep;
                    r += len;
                    continue;
                } else {
                    /* Unknown entity: drop */
                    r += len;
                    continue;
                }
            }
        }

        /* HTML tags: <br>, <p>, etc. */
        if (*r == '<') {
            const char * endtag = strchr(r, '>');
            if (endtag && (endtag - r) <= 10) {
                int tlen = (int)(endtag - r + 1);
                if (!strncasecmp(r, "<br", 3) || !strncasecmp(r, "<p", 2)) {
                    buf[o++] = '\n';
                }
                r += tlen;
                continue;
            }
        }

        /* UTF-8 accented characters */
        unsigned char c0 = (unsigned char)r[0];
        unsigned char c1 = (unsigned char)r[1];
        if (c0 == 0xC3) {
            char rep = 0;
            switch (c1) {
                case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: rep = 'a'; break;
                case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: rep = 'A'; break;
                case 0xA8: case 0xA9: case 0xAA: case 0xAB: rep = 'e'; break;
                case 0x88: case 0x89: case 0x8A: case 0x8B: rep = 'E'; break;
                case 0xAC: case 0xAD: case 0xAE: case 0xAF: rep = 'i'; break;
                case 0x8C: case 0x8D: case 0x8E: case 0x8F: rep = 'I'; break;
                case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: rep = 'o'; break;
                case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: rep = 'O'; break;
                case 0xB9: case 0xBA: case 0xBB: case 0xBC: rep = 'u'; break;
                case 0x99: case 0x9A: case 0x9B: case 0x9C: rep = 'U'; break;
                case 0xA7: rep = 'c'; break;
                case 0x87: rep = 'C'; break;
                default: break;
            }
            if (rep) {
                buf[o++] = rep;
                r += 2;
                continue;
            }
        } else if (c0 == 0xE2 && c1 == 0x80) {
            unsigned char c2 = (unsigned char)r[2];
            char rep = 0;
            if (c2 == 0x98 || c2 == 0x99) rep = '\'';
            else if (c2 == 0x9C || c2 == 0x9D) rep = '"';
            else if (c2 == 0x93 || c2 == 0x94) rep = '-';
            else if (c2 == 0xA6) {
                buf[o++] = '.'; buf[o++] = '.'; buf[o++] = '.';
                r += 3;
                continue;
            }
            if (rep) {
                buf[o++] = rep;
                r += 3;
                continue;
            }
        }

        buf[o++] = *r++;
    }
    buf[o] = '\0';
    strncpy(s, buf, maxlen - 1);
    s[maxlen - 1] = '\0';
    free(buf);
}

/* buienradar feed — used for the radar-map URL + the NL weather-report
 * narrative, plus nearest-KNMI-station fallback if Open-Meteo is overloaded. */
static int parse_buienradar_extras(const char * body) {

    /* --- radar image URL (lives in "actual" object) --- */
    js_str(body, body + strlen(body), "actualradarurl",
           weather_state.radar_url, sizeof(weather_state.radar_url));

    /* If Open-Meteo failed, pick the closest KNMI station so live real + feel
     * temperatures stay fresh instead of freezing at an old reading. */
    if (!weather_state.connected) {
        const char * sm = ci_strstr(body, "\"stationmeasurements\":");
        if (sm) {
            double ref_lat = (g_cfg_lat != 0.0) ? g_cfg_lat : 52.1009;
            double ref_lon = (g_cfg_lon != 0.0) ? g_cfg_lon : 5.1783;
            double best_d2 = 1e9, best_t = 0.0, best_f = 0.0;
            int found = 0;
            const char * p = sm;
            while ((p = strchr(p, '{')) != NULL) {
                const char * e = strchr(p, '}');
                if (!e) break;
                double slat = js_num(p, e, "lat", 0.0);
                double slon = js_num(p, e, "lon", 0.0);
                double stmp = js_num(p, e, "temperature", -999.0);
                if (slat != 0.0 && stmp > -100.0) {
                    double dlat = slat - ref_lat;
                    double dlon = (slon - ref_lon) * 0.62;
                    double d2 = dlat * dlat + dlon * dlon;
                    if (d2 < best_d2) {
                        best_d2 = d2;
                        best_t  = stmp;
                        best_f  = js_num(p, e, "feeltemperature", stmp);
                        found   = 1;
                    }
                }
                p = e + 1;
                if (*p == ']') break;
            }
            if (found) {
                weather_state.current_temp = (float)best_t;
                weather_state.feel_temp    = (float)best_f;
                weather_state.connected    = 1;
            }
        }
    }

    /* --- weatherreport title + text --- */
    const char * wr = ci_strstr(body, "\"weatherreport\":");
    if (wr) {
        const char * wr_end = ci_strstr(wr, "\"shortterm\"");
        if (!wr_end) wr_end = wr + 4096;
        js_str(wr, wr_end, "title", weather_state.weatherreport_title,
               sizeof(weather_state.weatherreport_title));
        js_str(wr, wr_end, "text", weather_state.weatherreport_text,
               sizeof(weather_state.weatherreport_text));
        sanitize_weather_text(weather_state.weatherreport_title, sizeof(weather_state.weatherreport_title));
        sanitize_weather_text(weather_state.weatherreport_text, sizeof(weather_state.weatherreport_text));
    }

    return 0;
}

/* Hourly forecast fetcher — calls forecast.buienradar.nl with the
 * configured location id and fills weather_state.hours[] with up to
 * WEATHER_FORECAST_HOURS slots spaced ~3 hours apart starting from the
 * first slot >= now. Returns 0 on success.
 *
 * Endpoint shape:
 *   { "days": [
 *       {"date":"…", "hours":[
 *         {"datetime":"2026-05-15T20:30:00", "temperature":24.1,
 *          "iconcode":"rr", "winddirection":"Z", "beaufort":3, …}, …
 *       ]},
 *       {"date":"…", "hours":[…]}
 *     ] }
 *
 * The first day usually contains 1-hour resolution; later days are
 * sparser. We just iterate days→hours linearly, pick every 3rd entry
 * after the first-future, and stop at WEATHER_FORECAST_HOURS. */
/* Fetches the radar GIF to disk via curl. We don't write our own libcurl
   binding — popen out, redirect to file, atomic rename. The forecast
   screen reads /tmp/toonui_radar.gif via LVGL's stdio FS driver + lv_gif. */
static int fetch_radar_image(void) {
    if (!weather_state.radar_url[0]) return -1;
    /* The URL comes from Buienradar's JSON (fetched without certificate
       checks), and js_str decodes \' — so it is NOT quote-safe. Require an
       http(s) URL and shell-quote it. */
    char cmd[1024], qurl[700];
    if (!sh_is_http_url(weather_state.radar_url) ||
        !sh_quote(qurl, sizeof qurl, weather_state.radar_url)) return -1;
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s -k -L --max-time 10 -A 'toonui/1.0' "
        "-o /tmp/toonui_radar.gif.tmp %s && mv /tmp/toonui_radar.gif.tmp /tmp/toonui_radar.gif",
        qurl);
    int rc = system(cmd);
    return (rc == 0) ? 0 : -1;
}

/* Resolve a city name to a Buienradar/GeoNames location id via the free
 * Open-Meteo geocoding API (no key). Buienradar's /forecast/<id> uses GeoNames
 * ids, and Open-Meteo returns the same id, so the first result drops straight
 * into settings.weather_location_id. Returns the id, or 0 if not found.
 * The city is percent-encoded (http_fetch rejects spaces/quotes), so a name
 * like "Sint Pancras" is looked up safely. */
int weather_geocode(const char * city) {
    if (!city || !city[0]) return 0;
    char enc[160]; size_t o = 0;
    for (const char * p = city; *p && o + 4 < sizeof enc; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            enc[o++] = (char)c;
        } else {
            snprintf(enc + o, 4, "%%%02X", c);
            o += 3;
        }
    }
    enc[o] = 0;
    char url[256];
    snprintf(url, sizeof url,
        "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=nl&format=json",
        enc);
    static char body[4096];
    if (http_fetch(url, body, sizeof body) != 0) {
        fprintf(stderr, "[wx] geocode '%s': no response from geocoding-api.open-meteo.com\n", city);
        return 0;
    }
    /* First "id": in the response is the top result's GeoNames id. */
    const char * p = strstr(body, "\"id\":");
    if (!p) {
        fprintf(stderr, "[wx] geocode '%s': no match — check the spelling in "
                        "Settings > Weather\n", city);
        return 0;
    }
    int id = atoi(p + 5);
    /* Capture the exact lat/lon — this is what the Open-Meteo current+forecast
     * fetch keys on (the GeoNames id is only kept for settings back-compat). */
    double lat = js_num(body, body + strlen(body), "latitude", 0);
    double lon = js_num(body, body + strlen(body), "longitude", 0);
    if (lat != 0.0 || lon != 0.0) { g_cfg_lat = lat; g_cfg_lon = lon; }
    fprintf(stderr, "[wx] geocode '%s' -> id %d (%.4f,%.4f)\n", city, id, lat, lon);
    return id;
}

static void * wx_thread(void * arg) {
    (void)arg;
    sleep(5);                            /* initial settling delay for WiFi at boot */
    static char body[64 * 1024];
    int radar_tick = 0;
    int geo_fails = 0, geo_fallback = 0;
    while (1) {
        /* Resolve the configured city's exact lat/lon once (re-tried each loop
           until it sticks). Everything temperature/forecast keys off this. */
        if (g_cfg_lat == 0.0 && g_cfg_lon == 0.0) {
            if (settings.weather_lat[0] && settings.weather_lon[0]) {
                g_cfg_lat = atof(settings.weather_lat);
                g_cfg_lon = atof(settings.weather_lon);
            }
        }
        if (g_cfg_lat == 0.0 && g_cfg_lon == 0.0 && settings.weather_location[0]) {
            weather_geocode(settings.weather_location);
            /* The geocoder is a second host that can be down/blocked/misspelled
               while api.open-meteo.com is fine — and without a lat/lon there is
               no fetch at all, so the user sees no weather whatsoever. After a
               few tries, fall back to the centre of NL so something shows, and
               keep retrying the real location in the background. */
            if (g_cfg_lat == 0.0 && g_cfg_lon == 0.0 && ++geo_fails >= 3) {
                g_cfg_lat = 52.1009; g_cfg_lon = 5.1783;   /* De Bilt (KNMI) */
                geo_fallback = 1;
                fprintf(stderr, "[wx] geocode failed %d times — falling back to "
                                "De Bilt (52.10,5.18); weather will be approximate\n", geo_fails);
            }
        }
        /* Still trying to pin down the real location while running on the fallback. */
        if (geo_fallback && settings.weather_location[0]) {
            double fb_lat = g_cfg_lat, fb_lon = g_cfg_lon;
            g_cfg_lat = g_cfg_lon = 0.0;
            weather_geocode(settings.weather_location);
            if (g_cfg_lat == 0.0 && g_cfg_lon == 0.0) { g_cfg_lat = fb_lat; g_cfg_lon = fb_lon; }
            else { geo_fallback = 0; geo_fails = 0;
                   fprintf(stderr, "[wx] geocode recovered — using the configured location\n"); }
        }

        /* Current conditions + 5-day + hourly — exact point, from Open-Meteo. */
        if (fetch_openmeteo() == 0) {
            weather_state.connected = 1;
            fprintf(stderr, "[wx] %s %.1fC @ (%.3f,%.3f), %d-day %d-hour\n",
                    weather_state.current_desc, weather_state.current_temp,
                    g_cfg_lat, g_cfg_lon, weather_state.day_count, weather_state.hour_count);
        } else {
            weather_state.connected = 0;
            fprintf(stderr, "[wx] open-meteo fetch failed\n");
        }

        /* If weather fetch failed (e.g. WiFi still connecting on boot),
         * retry quickly (every 10s) until we get our first successful fetch,
         * rather than sleeping 15 minutes! */
        if (!weather_state.connected) {
            for (int s = 0; s < 10 && !g_wx_wake; s++)
                sleep(1);
            g_wx_wake = 0;
            continue;
        }

        /* buienradar feed — radar-map URL + NL weather-report narrative only. */
        if (http_fetch("https://data.buienradar.nl/2.0/feed/json",
                       body, sizeof(body)) == 0) {
            weather_lock();
            parse_buienradar_extras(body);
            weather_unlock();
        }

        /* Fetch the radar image every 5 minutes, waking early on weather_refresh_async(). */
        for (int i = 0; i < 3 && !g_wx_wake; i++) {
            if (fetch_radar_image() == 0)
                fprintf(stderr, "[wx] radar refreshed (tick %d)\n", radar_tick++);
            else
                fprintf(stderr, "[wx] radar fetch failed\n");
            for (int s = 0; s < 5 * 60 && !g_wx_wake; s++)
                sleep(1);
        }
        g_wx_wake = 0;
    }
    return NULL;
}

void weather_refresh_async(void) {
    g_wx_wake = 1;
}

int weather_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, wx_thread, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}
