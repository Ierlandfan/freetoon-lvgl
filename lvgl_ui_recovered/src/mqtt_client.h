#ifndef TOON_MQTT_CLIENT_H
#define TOON_MQTT_CLIENT_H

#include <stddef.h>

/* Called on each PUBLISH received. Payload is NOT null-terminated. */
typedef void (*mqtt_on_message_cb)(const char * topic,
                                   const unsigned char * payload, size_t len,
                                   void * arg);

/* Spawn the long-running subscriber. Reads broker host/port/user/pass
 * AND topic list from `settings` (settings.h). Safe to call once at
 * startup; mqtt_client_restart() reloads from settings without a process
 * restart. The message callback is the SAME one packages.c registers. */
int mqtt_client_start(mqtt_on_message_cb cb, void * arg);

/* Cancel the current connection so it reconnects on the next tick with
 * whatever's currently in `settings.mqtt_*`. Use after the Settings UI
 * has called settings_save(). */
void mqtt_client_restart(void);

/* Synchronous probe: connect + auth-check + clean disconnect. Returns 0
 * on success, fills err[] with a short reason on failure (sized 128).
 * Doesn't subscribe to anything. Safe to call from a UI worker thread. */
int mqtt_test_connection(const char * host, int port,
                         const char * user, const char * pass,
                         char * err, size_t errsz);

/* One-shot publish (connect → PUBLISH QoS0 → disconnect) to the broker in
 * settings. For device control (HA command topic / domoticz/in). 0 = ok. */
int mqtt_publish(const char * topic, const char * payload);

/* Synchronous topic discovery: connect, subscribe to `wildcard` (e.g.
 * "#" or "home/#"), collect unique topics for `duration_ms`, return them
 * via `cb` (called once per unique topic). Disconnects. Returns count
 * on success, -1 on error. Cap on internal seen-list = 64; older topics
 * drop silently. Safe to call from a UI worker thread. */
typedef void (*mqtt_topic_cb)(const char * topic, void * arg);
int mqtt_discover_topics(const char * host, int port,
                         const char * user, const char * pass,
                         const char * wildcard, int duration_ms,
                         mqtt_topic_cb cb, void * arg);

/* Legacy entry point — kept so callers that still pass their own topic
 * list compile. New code should use mqtt_client_start() + settings. */
int mqtt_subscribe_async(const char ** topics, int n_topics,
                         mqtt_on_message_cb cb, void * arg);

#endif
