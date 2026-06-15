#ifndef TOON_VENTILATION_H
#define TOON_VENTILATION_H

/* Live state pushed from the Itho-Wifi bridge over MQTT (topics
   itho/ithostatus + itho/lastcmd + itho/lwt). MQTT-only: no HTTP status
   polling/fallback. The settings table is the one HTTP-fetched item. */
typedef struct {
    volatile int   connected;        /* freetoon↔broker MQTT link up 0/1 */
    volatile int   itho_online;      /* Itho bridge online per its MQTT LWT
                                        (topic itho/lwt: "online"/"offline").
                                        -1 = unknown (no LWT retained msg yet). */
    volatile int   speed_pct;        /* "Speed status" (was "ExhFanSpeed (%)") — fan output, 0..100 */
    volatile int   exh_fan_pct;      /* "Ventilation setpoint (%)" — commanded, 0..100 */
    volatile int   fan_rpm;          /* "Fan speed (rpm)" */
    volatile int   filter_dirty;     /* 0/1 */
    volatile int   internal_fault;   /* 0/1 */
    volatile int   error_code;       /* "Error" — Itho internal */
    volatile int   total_hours;      /* "Total operation (hours)" */
    volatile int   remaining_min;    /* "RemainingTime (min)" */
    char           fan_info[16];     /* "auto" / "low" / "high" / "timer" / "medium" */
    /* From /api.html?get=lastcmd — last command Itho applied + where it
     * came from (HTML-API vremote vs physical RF remote vs device button). */
    char           last_cmd[16];     /* "low" / "high" / "auto" / "timer1" … */
    char           last_source[64];  /* e.g. "HTML API-vremote-0", "RFT-CO2-1234" */
    volatile long  last_cmd_ts;      /* unix seconds (0 if unknown) */
} vent_state_t;

extern vent_state_t vent_state;

/* Start the poller. Returns 0 on success. */
int vent_start(void);

/* Clean, capitalised display label for the current mode (Low/High/Auto/
 * Manual/Timer), derived from fan_info|last_cmd. Static buffer, LVGL thread
 * only. "speed:N" → "Manual", "timerN" → "Timer". */
const char * vent_mode_label(void);

/* Send a command — uses the Itho virtual remote API. cmd ∈
 * {"away","low","medium","high","auto","autonight","timer1","timer2","timer3"}.
 * Returns 0 on HTTP 200. Blocks briefly (~1 s).
 *
 * Pass the USER-INTENT name (what the button on screen says); on units
 * where Itho's low/high vremote labels are physically inverted, the
 * implementation does the swap. See VENT_SWAP_LOW_HIGH below. */
int vent_send_vremote(const char * cmd);

/* Same as vent_send_vremote, but the HTTP fetch is done on a detached
 * thread so the LVGL event loop stays responsive (button clicks return
 * instantly). The expected post-command speed is written into
 * vent_state.speed_pct + exh_fan_pct immediately as an optimistic
 * update; the next 8-second poll confirms or corrects it. */
void vent_send_vremote_async(const char * cmd);

/* Direct PWM speed set (0..255) via /api.html?speed=N&timer=0.
 * Returns 0 on success. */
int vent_set_speed(int pwm);

/* Same as vent_set_speed but the HTTP fetch runs on a detached thread so the
 * LVGL loop stays responsive (slider release returns instantly). The expected
 * % is written into vent_state.speed_pct/exh_fan_pct immediately; the next
 * MQTT poll confirms. On this CVE the speed= path is the reliable control. */
void vent_set_speed_async(int pwm);

/* Bump current speed by `delta` PWM steps (clamped to 0..255).
 * Reads /api.html?get=currentspeed first, then issues vent_set_speed. */
int vent_bump_speed(int delta);

/* Read all settings (returns NUL-terminated JSON string). Caller does not
 * free. Updated by vent_refresh_settings(). Legacy — kept for callers that
 * already exist; the new flow is the per-index API below. */
const char * vent_settings_json(void);
int vent_refresh_settings(void);

/* One Itho i2c-settings entry, fetched via /api.html?getsetting=N. */
typedef struct {
    int  idx;
    char label[96];
    int  current;
    int  minimum;
    int  maximum;
    int  loaded;          /* 0 = empty, 1 = good, -1 = fetch failed */
} vent_setting_t;

#define VENT_SETTING_COUNT 80
extern vent_setting_t vent_settings[VENT_SETTING_COUNT];

/* Fetch a single setting into vent_settings[idx]. Blocking (~50-200 ms). */
int vent_fetch_one(int idx);

/* Loop 0..VENT_SETTING_COUNT-1 calling vent_fetch_one. Used by the
 * background thread on first run. Returns the number of entries that
 * loaded successfully. */
int vent_fetch_all_settings(void);

/* Send /api.html?setsetting=N&value=V and re-fetch on success.
 * Returns 0 on success, -1 on HTTP/parse error, -2 if the Itho-Wifi
 * settings-write API is disabled (the user must enable it in the
 * addon's web UI before writes are accepted). */
int vent_save_setting(int idx, int value);

#endif
