#ifndef TOON_CLIENT_LINK_H
#define TOON_CLIENT_LINK_H

/* Client ("slave") mode: this Toon mirrors a master Toon over the master's
 * PWA HTTP API instead of talking to local HCB daemons. When settings.
 * client_mode is set, client_link_start() streams the master's
 * /api/state/stream into toon_state (+ ha_state curtains) and the control
 * helpers POST back to the master. The slave connects ONLY to the master. */

int client_link_start(void);             /* spawn the SSE reader if client_mode */
int client_link_setpoint(float temp);    /* POST /api/setpoint {"value":"x.xx"} */
int client_link_program(int state);      /* POST /api/program  {"state":n}      */
int client_link_curtain(const char * action); /* POST /api/curtain {"action":..} */

#endif
