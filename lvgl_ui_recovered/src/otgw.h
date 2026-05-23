#ifndef TOON_OTGW_H
#define TOON_OTGW_H

/* OTGW telemetry poller. When settings.otgw_host is set, periodically reads the
 * OpenTherm Gateway's otmonitor and fills the boiler-telemetry fields of
 * toon_state (flow/return temps, modulation, burner) that happ_thermstat does
 * not expose over BoxTalk in "off" OT-bridge mode. Read-only HTTP GET — OTGW is
 * the boiler truth and far more robust than the old quby_bridge proxy sniff.
 * Water pressure is only filled as a fallback (happ's value wins when present). */
int otgw_start(void);

#endif
