#ifndef TOON_DOMOTICZ_H
#define TOON_DOMOTICZ_H

/* Domoticz JSON-API client — an alternative to Home Assistant for users who
 * run Domoticz for their lights + blinds. Talks to the device's HTTP JSON API
 * (/json.htm). Config (settings): domoticz_host ("ip:port" or full URL),
 * optional domoticz_user/domoticz_pass (HTTP basic auth, sent in the URL).
 *
 *   list:   /json.htm?type=command&param=getdevices&filter=light&used=true
 *   switch: /json.htm?type=command&param=switchlight&idx=N&switchcmd=On|Off|Toggle
 *   dim:    ...&switchcmd=Set%20Level&level=NN
 *   blind:  ...&switchcmd=Open|Close|Stop
 */

#define DOMOTICZ_MAX_DEV 256   /* a busy Domoticz easily has 100s of used devices */

enum { DZ_SWITCH = 0, DZ_DIMMER = 1, DZ_BLIND = 2, DZ_SELECTOR = 3 };

typedef struct {
    int  idx;                 /* Domoticz device idx */
    int  kind;                /* DZ_SWITCH / DZ_DIMMER / DZ_BLIND / DZ_SELECTOR */
    char name[40];
    volatile int on;          /* 0/1 (for blinds: 1 = open) */
    volatile int level;       /* dimmer/blind 0..100; selector = level value (idx*10); -1 n/a */
    char options[256];        /* selector: pipe-delimited option names ("Off|Weg|…") */
} domoticz_dev_t;

typedef struct {
    volatile int   connected;
    volatile int   count;
    domoticz_dev_t dev[DOMOTICZ_MAX_DEV];
} domoticz_state_t;

extern domoticz_state_t domoticz_state;

/* Start the WebSocket+HTTP client thread (live push over ws://host/json, data
 * over the JSON API). Needs settings.domoticz_host. Returns 0 on success. */
int  domoticz_start(void);

/* Synchronous connection test for the Settings screen. Runs the same auth
 * ladder as the live client (session cookie → re-login → HTTP Basic). Returns
 * the light/blind device count (>=0) on success, or:
 *   DZ_PROBE_AUTH   (-1) reached Domoticz but auth failed (bad user/pass)
 *   DZ_PROBE_NOCONN (-2) could not reach the host at all */
#define DZ_PROBE_AUTH   (-1)
#define DZ_PROBE_NOCONN (-2)
int  domoticz_probe(void);

/* Fire-and-forget control (async; HTTP runs on a detached thread). */
void domoticz_switch_async(int idx, const char * cmd);   /* "On"/"Off"/"Toggle"/"Open"/"Close"/"Stop" */
void domoticz_set_level_async(int idx, int level);        /* 0..100 */

#endif
