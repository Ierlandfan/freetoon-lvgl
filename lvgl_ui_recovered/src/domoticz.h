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

#define DOMOTICZ_MAX_DEV 24

enum { DZ_SWITCH = 0, DZ_DIMMER = 1, DZ_BLIND = 2 };

typedef struct {
    int  idx;                 /* Domoticz device idx */
    int  kind;                /* DZ_SWITCH / DZ_DIMMER / DZ_BLIND */
    char name[40];
    volatile int on;          /* 0/1 (for blinds: 1 = open) */
    volatile int level;       /* 0..100 dimmer/blind level, -1 if n/a */
} domoticz_dev_t;

typedef struct {
    volatile int   connected;
    volatile int   count;
    domoticz_dev_t dev[DOMOTICZ_MAX_DEV];
} domoticz_state_t;

extern domoticz_state_t domoticz_state;

int  domoticz_start(void);

/* Fire-and-forget control (async; HTTP runs on a detached thread). */
void domoticz_switch_async(int idx, const char * cmd);   /* "On"/"Off"/"Toggle"/"Open"/"Close"/"Stop" */
void domoticz_set_level_async(int idx, int level);        /* 0..100 */

#endif
