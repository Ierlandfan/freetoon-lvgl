#ifndef TOON_HA_MQTT_H
#define TOON_HA_MQTT_H

/*
 * freetoon -> Home Assistant push over MQTT auto-discovery.
 *
 * Publishes the live thermostat + boiler state (from boxtalk's toon_state)
 * to the same broker the Itho bridge already uses (/mnt/data/mqtt.cfg,
 * "host:user:pass"). On connect it emits retained HA discovery configs so
 * HA auto-creates a climate entity + boiler/air sensors under one "Toon"
 * device, then republishes a JSON state blob every HA_MQTT_PUBLISH_S.
 *
 * Availability is a retained LWT (freetoon/toon/availability): when the
 * Toon powers off the broker flips it to "offline" and every entity goes
 * unavailable in HA cleanly -- no polling-integration connection-refused
 * error spam.
 *
 * Control (optional, on by default): subscribes to the climate command
 * topics and drives boxtalk_set_setpoint() / boxtalk_set_program(), so the
 * thermostat is adjustable from HA. boxtalk's setters are self-locking.
 *
 * No-op if /mnt/data/mqtt.cfg is absent. Started from main.c after boxtalk.
 */
int ha_mqtt_start(void);

#endif /* TOON_HA_MQTT_H */
