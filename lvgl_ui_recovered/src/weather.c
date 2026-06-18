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

/* The configured location's exact lat/lon, resolved by the Open-Meteo geocoder
 * (weather_geocode). Drives the Open-Meteo current+forecast fetch. 0 = unresolved. */
static double g_cfg_lat = 0.0, g_cfg_lon = 0.0;

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
        D->min_temp    = (float)js_arr_num(dy, "temperature_2m_min", i, 0);
        D->max_temp    = (float)js_arr_num(dy, "temperature_2m_max", i, 0);
        D->rain_chance = (int)js_arr_num(dy, "precipitation_probability_max", i, 0);
        D->wind_bft    = kmh_to_bft(js_arr_num(dy, "wind_speed_10m_max", i, 0));
        deg_to_nl(js_arr_num(dy, "wind_direction_10m_dominant", i, 0), D->wind_dir, sizeof D->wind_dir);
        int code = (int)js_arr_num(dy, "weather_code", i, 3);
        wmo_icon(code, 1, D->icon);
        snprintf(D->desc, sizeof D->desc, "%s", wmo_desc(code));
        dc++;
    }
    weather_state.day_count = dc;

    /* hourly: 6 slots, 3-hour steps from the current local hour. Open-Meteo's
       hourly[] starts at 00:00 today (timezone set), 1-hour steps → index=hour. */
    const char * hr = strstr(body, "\"hourly\":");
    int hc = 0;
    if (hr) {
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
            H->wind_bft    = kmh_to_bft(js_arr_num(hr, "wind_speed_10m", idx, 0));
            deg_to_nl(js_arr_num(hr, "wind_direction_10m", idx, 0), H->wind_dir, sizeof H->wind_dir);
            wmo_icon((int)js_arr_num(hr, "weather_code", idx, 3), 1, H->icon);
            hc++;
        }
    }
    weather_state.hour_count = hc;
    return 0;
}

static int fetch_openmeteo(void) {
    if (g_cfg_lat == 0.0 && g_cfg_lon == 0.0) return -1;
    char url[480];
    snprintf(url, sizeof url,
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&timezone=Europe%%2FAmsterdam&forecast_days=5"
        "&current=temperature_2m,apparent_temperature,weather_code,is_day"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
        "wind_speed_10m_max,wind_direction_10m_dominant"
        "&hourly=temperature_2m,weather_code,wind_speed_10m,wind_direction_10m",
        g_cfg_lat, g_cfg_lon);
    static char body[48 * 1024];
    if (http_fetch(url, body, sizeof body) != 0) return -1;
    return parse_openmeteo(body);
}

/* buienradar feed — used ONLY for the radar-map URL + the NL weather-report
 * narrative; all temperatures/forecast now come from Open-Meteo (exact point). */
static int parse_buienradar_extras(const char * body) {

    /* --- radar image URL (lives in "actual" object) --- */
    js_str(body, body + strlen(body), "actualradarurl",
           weather_state.radar_url, sizeof(weather_state.radar_url));

    /* --- weatherreport title + text --- */
    const char * wr = ci_strstr(body, "\"weatherreport\":");
    if (wr) {
        const char * wr_end = ci_strstr(wr, "\"shortterm\"");
        if (!wr_end) wr_end = wr + 4096;
        js_str(wr, wr_end, "title", weather_state.weatherreport_title,
               sizeof(weather_state.weatherreport_title));
        js_str(wr, wr_end, "text", weather_state.weatherreport_text,
               sizeof(weather_state.weatherreport_text));
        /* Replace HTML entities the buienradar feed sometimes leaks
           (`&nbsp;`) with regular spaces. */
        char * p;
        while ((p = strstr(weather_state.weatherreport_text, "&nbsp;")) != NULL) {
            *p = ' ';
            memmove(p + 1, p + 6, strlen(p + 6) + 1);
        }
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
    /* Quote-safe by construction: the URL came from buienradar's JSON
       which uses only safe chars. */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s -k -L --max-time 10 -A 'toonui/1.0' "
        "-o /tmp/toonui_radar.gif.tmp '%s' && mv /tmp/toonui_radar.gif.tmp /tmp/toonui_radar.gif",
        weather_state.radar_url);
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
    if (http_fetch(url, body, sizeof body) != 0) return 0;
    /* First "id": in the response is the top result's GeoNames id. */
    const char * p = strstr(body, "\"id\":");
    if (!p) return 0;
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
    static char body[64 * 1024];
    int radar_tick = 0;
    while (1) {
        /* Resolve the configured city's exact lat/lon once (re-tried each loop
           until it sticks). Everything temperature/forecast keys off this. */
        if (g_cfg_lat == 0.0 && g_cfg_lon == 0.0 && settings.weather_location[0])
            weather_geocode(settings.weather_location);

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

        /* buienradar feed — radar-map URL + NL weather-report narrative only. */
        if (http_fetch("https://data.buienradar.nl/2.0/feed/json",
                       body, sizeof(body)) == 0)
            parse_buienradar_extras(body);

        /* Fetch the radar image every 5 minutes (3 ticks of 15-min loop is
           too slow; we do an inner sleep loop instead). */
        for (int i = 0; i < 3; i++) {
            if (fetch_radar_image() == 0)
                fprintf(stderr, "[wx] radar refreshed (tick %d)\n", radar_tick++);
            else
                fprintf(stderr, "[wx] radar fetch failed\n");
            sleep(5 * 60);
        }
    }
    return NULL;
}

int weather_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, wx_thread, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}
