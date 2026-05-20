/* Minimal MQTT 3.1.1 client for toonui.
 *
 * Hand-rolled because Toon's musl/glibc image doesn't have libmosquitto
 * and pulling it in (with OpenSSL) is overkill for a subscriber that
 * watches a handful of topics.
 *
 * Scope: QoS 0 SUBSCRIBE + PUBLISH read, CONNECT, PINGREQ keep-alive.
 * Config (broker host/port/creds + subscribe-topic list) comes from
 * settings.h, so the runtime can be reconfigured from the UI via
 * mqtt_client_restart().
 *
 * Also exposes two synchronous helpers used by the Settings UI:
 *   mqtt_test_connection() — connect + authenticate, no subscribe
 *   mqtt_discover_topics() — connect, SUBSCRIBE "#", collect for Nms
 */
#define _GNU_SOURCE
#include "mqtt_client.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define KEEPALIVE_S   60
#define PING_S        30

/* ===================================================================== */
/* Wire-format helpers                                                   */
/* ===================================================================== */
static int mqtt_enc_remlen(uint32_t v, uint8_t * out) {
    int n = 0;
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        out[n++] = b;
    } while (v && n < 4);
    return n;
}
static int mqtt_dec_remlen(const uint8_t * in, size_t in_avail, uint32_t * out) {
    uint32_t mult = 1, val = 0;
    size_t i = 0;
    do {
        if (i >= in_avail || i >= 4) return -1;
        val += (in[i] & 0x7f) * mult;
        mult <<= 7;
        if (!(in[i] & 0x80)) { *out = val; return (int)(i + 1); }
        i++;
    } while (1);
}
static size_t pack_str(uint8_t * buf, const char * s) {
    uint16_t L = (uint16_t)strlen(s);
    buf[0] = L >> 8; buf[1] = L & 0xff;
    memcpy(buf + 2, s, L);
    return 2 + L;
}
static int sock_send_all(int fd, const uint8_t * b, size_t n) {
    while (n) {
        ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
        if (k <= 0) return -1;
        b += k; n -= k;
    }
    return 0;
}
static int sock_recv_all(int fd, uint8_t * b, size_t n) {
    while (n) {
        ssize_t k = recv(fd, b, n, 0);
        if (k <= 0) return -1;
        b += k; n -= k;
    }
    return 0;
}

/* ===================================================================== */
/* CONNECT / SUBSCRIBE / PINGREQ                                          */
/* ===================================================================== */
static int mqtt_open_sock(const char * host, int port,
                          int timeout_s, char * err, size_t errsz) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { if (err) snprintf(err, errsz, "socket: %s", strerror(errno)); return -1; }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {0} };
    a.sin_addr.s_addr = inet_addr(host);
    if (a.sin_addr.s_addr == INADDR_NONE) {
        struct hostent * he = gethostbyname(host);
        if (!he) { close(s); if (err) snprintf(err, errsz, "DNS: %s", host); return -1; }
        memcpy(&a.sin_addr, he->h_addr, he->h_length);
    }
    struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        if (err) snprintf(err, errsz, "connect %s:%d: %s", host, port, strerror(errno));
        close(s); return -1;
    }
    return s;
}

static int mqtt_send_connect(int fd, const char * user, const char * pass,
                             char * err, size_t errsz) {
    char client_id[64];
    snprintf(client_id, sizeof(client_id), "toonui-%ld", (long)time(NULL));
    uint8_t var_pl[512]; size_t o = 0;
    o += pack_str(var_pl + o, "MQTT");
    var_pl[o++] = 4;
    uint8_t flags = 0x02;
    if (user && user[0]) flags |= 0x80;
    if (pass && pass[0]) flags |= 0x40;
    var_pl[o++] = flags;
    var_pl[o++] = (KEEPALIVE_S >> 8) & 0xff;
    var_pl[o++] =  KEEPALIVE_S       & 0xff;
    o += pack_str(var_pl + o, client_id);
    if (user && user[0]) o += pack_str(var_pl + o, user);
    if (pass && pass[0]) o += pack_str(var_pl + o, pass);
    uint8_t hdr[6]; hdr[0] = 0x10;
    int rl = mqtt_enc_remlen((uint32_t)o, hdr + 1);
    if (sock_send_all(fd, hdr, 1 + rl) < 0) { if (err) snprintf(err, errsz, "send CONNECT failed"); return -1; }
    if (sock_send_all(fd, var_pl, o)    < 0) { if (err) snprintf(err, errsz, "send CONNECT body failed"); return -1; }
    uint8_t ack[4];
    if (sock_recv_all(fd, ack, 4) < 0) { if (err) snprintf(err, errsz, "no CONNACK"); return -1; }
    if (ack[0] != 0x20) { if (err) snprintf(err, errsz, "bad packet type 0x%02x", ack[0]); return -1; }
    if (ack[3] != 0x00) {
        const char * msg = "?";
        switch (ack[3]) {
        case 1: msg = "unacceptable protocol"; break;
        case 2: msg = "client id rejected";    break;
        case 3: msg = "broker unavailable";    break;
        case 4: msg = "bad user/pass";         break;
        case 5: msg = "not authorized";        break;
        }
        if (err) snprintf(err, errsz, "CONNACK rc=%u (%s)", ack[3], msg);
        return -1;
    }
    return 0;
}

static int mqtt_send_subscribe(int fd, uint16_t pid, const char * topic) {
    uint8_t var_pl[256]; size_t o = 0;
    var_pl[o++] = (pid >> 8) & 0xff;
    var_pl[o++] =  pid       & 0xff;
    o += pack_str(var_pl + o, topic);
    var_pl[o++] = 0;
    uint8_t hdr[6]; hdr[0] = 0x82;
    int rl = mqtt_enc_remlen((uint32_t)o, hdr + 1);
    if (sock_send_all(fd, hdr, 1 + rl) < 0) return -1;
    if (sock_send_all(fd, var_pl, o)    < 0) return -1;
    uint8_t ack[5];
    if (sock_recv_all(fd, ack, 5) < 0) return -1;
    if (ack[0] != 0x90 || ack[4] >= 0x80) return -1;
    return 0;
}

static int mqtt_send_ping(int fd) {
    uint8_t p[2] = { 0xC0, 0x00 };
    return sock_send_all(fd, p, 2);
}

/* Read one PUBLISH (or PINGRESP) packet. Returns -1 on socket error;
 * 0 if a non-PUBLISH packet was consumed (caller continues); >0 if a
 * PUBLISH was delivered to `cb`. */
static int mqtt_read_one(int fd, mqtt_on_message_cb cb, void * arg) {
    uint8_t hdr;
    if (sock_recv_all(fd, &hdr, 1) < 0) return -1;
    uint8_t rlbuf[4]; uint32_t remlen = 0; int rlb = 0;
    do {
        if (sock_recv_all(fd, &rlbuf[rlb], 1) < 0) return -1;
        rlb++;
    } while ((rlbuf[rlb - 1] & 0x80) && rlb < 4);
    if (mqtt_dec_remlen(rlbuf, rlb, &remlen) < 0) return -1;
    if (remlen > 65536) {
        /* drain oversized packet so the stream stays aligned */
        uint8_t skip[2048];
        while (remlen) {
            size_t want = remlen > sizeof(skip) ? sizeof(skip) : remlen;
            if (sock_recv_all(fd, skip, want) < 0) return -1;
            remlen -= want;
        }
        return 0;
    }
    static uint8_t buf[65536];
    if (remlen && sock_recv_all(fd, buf, remlen) < 0) return -1;
    uint8_t type = (hdr >> 4) & 0x0f;
    uint8_t qos  = (hdr >> 1) & 0x03;
    if (type == 0x0D) return 0;            /* PINGRESP */
    if (type != 0x03) return 0;            /* other — ignore */
    if (remlen < 2) return 0;
    uint16_t tl = ((uint16_t)buf[0] << 8) | buf[1];
    if (tl + 2u > remlen) return 0;
    char topic[256]; size_t tcopy = tl < sizeof(topic) - 1 ? tl : sizeof(topic) - 1;
    memcpy(topic, buf + 2, tcopy); topic[tcopy] = 0;
    size_t off = 2 + tl;
    if (qos > 0) { if (off + 2 > remlen) return 0; off += 2; }
    if (cb) cb(topic, buf + off, remlen - off, arg);
    return 1;
}

/* ===================================================================== */
/* Public: test connection                                               */
/* ===================================================================== */
int mqtt_test_connection(const char * host, int port,
                         const char * user, const char * pass,
                         char * err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    if (!host || !host[0]) { if (err) snprintf(err, errsz, "empty host"); return -1; }
    if (!port) port = 1883;
    int s = mqtt_open_sock(host, port, 5, err, errsz);
    if (s < 0) return -1;
    int rc = mqtt_send_connect(s, user, pass, err, errsz);
    /* polite close — fire DISCONNECT then drop */
    uint8_t dc[2] = { 0xE0, 0x00 };
    if (rc == 0) sock_send_all(s, dc, 2);
    close(s);
    if (rc == 0 && err && errsz) snprintf(err, errsz, "OK");
    return rc;
}

/* ===================================================================== */
/* Public: discover topics                                               */
/* ===================================================================== */
typedef struct {
    char  seen[64][96];
    int   count;
    mqtt_topic_cb cb;
    void * arg;
} discover_state_t;

static void discover_cb(const char * topic, const unsigned char * payload,
                        size_t len, void * arg) {
    (void)payload; (void)len;
    discover_state_t * d = (discover_state_t *)arg;
    if (!topic || !topic[0]) return;
    for (int i = 0; i < d->count; i++)
        if (!strcmp(d->seen[i], topic)) return;
    if (d->count >= (int)(sizeof d->seen / sizeof d->seen[0])) return;
    snprintf(d->seen[d->count], sizeof d->seen[0], "%s", topic);
    d->count++;
    if (d->cb) d->cb(topic, d->arg);
}

int mqtt_discover_topics(const char * host, int port,
                         const char * user, const char * pass,
                         const char * wildcard, int duration_ms,
                         mqtt_topic_cb cb, void * arg) {
    if (!host || !host[0]) return -1;
    if (!port) port = 1883;
    if (!wildcard || !wildcard[0]) wildcard = "#";

    char err[128] = "";
    int s = mqtt_open_sock(host, port, 5, err, sizeof(err));
    if (s < 0) { fprintf(stderr, "[mqtt-disc] %s\n", err); return -1; }
    if (mqtt_send_connect(s, user, pass, err, sizeof(err)) != 0) {
        fprintf(stderr, "[mqtt-disc] %s\n", err); close(s); return -1;
    }
    if (mqtt_send_subscribe(s, 1, wildcard) != 0) {
        fprintf(stderr, "[mqtt-disc] SUBSCRIBE failed\n"); close(s); return -1;
    }
    /* Set a short RX timeout so the read loop wakes regularly to check
     * the elapsed clock. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    discover_state_t st = {0};
    st.cb = cb; st.arg = arg;

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - t0.tv_sec) * 1000
                       + (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (elapsed_ms >= duration_ms) break;
        int r = mqtt_read_one(s, discover_cb, &st);
        if (r < 0) {
            /* timeout vs disconnect — distinguish via errno */
            if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        }
    }
    uint8_t dc[2] = { 0xE0, 0x00 };
    sock_send_all(s, dc, 2);
    close(s);
    return st.count;
}

/* ===================================================================== */
/* Background subscriber thread (settings-driven)                        */
/* ===================================================================== */
static pthread_mutex_t g_restart_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_want_restart = 0;
static mqtt_on_message_cb g_cb = NULL;
static void *             g_cb_arg = NULL;

void mqtt_client_restart(void) {
    pthread_mutex_lock(&g_restart_mtx);
    g_want_restart = 1;
    pthread_mutex_unlock(&g_restart_mtx);
}

static int subscriber_session(void) {
    if (!settings.mqtt_enabled || !settings.mqtt_host[0]) { sleep(5); return 0; }
    char err[128] = "";
    int port = settings.mqtt_port ? settings.mqtt_port : 1883;
    int s = mqtt_open_sock(settings.mqtt_host, port, 6, err, sizeof(err));
    if (s < 0) { fprintf(stderr, "[mqtt] %s\n", err); return -1; }
    if (mqtt_send_connect(s, settings.mqtt_user, settings.mqtt_pass,
                          err, sizeof(err)) != 0) {
        fprintf(stderr, "[mqtt] %s\n", err); close(s); return -1;
    }
    fprintf(stderr, "[mqtt] connected to %s:%d as %s, %d topic(s)\n",
            settings.mqtt_host, port, settings.mqtt_user,
            settings.mqtt_topic_count);
    uint16_t pid = 1;
    for (int i = 0; i < settings.mqtt_topic_count && i < 8; i++) {
        if (!settings.mqtt_topics[i][0]) continue;
        if (mqtt_send_subscribe(s, pid++, settings.mqtt_topics[i]) != 0) {
            fprintf(stderr, "[mqtt] subscribe %s FAIL\n", settings.mqtt_topics[i]);
            close(s); return -1;
        }
        fprintf(stderr, "[mqtt] subscribed: %s\n", settings.mqtt_topics[i]);
    }
    /* Switch RX timeout to PING cadence so we wake to send pings + check
     * the restart flag. */
    struct timeval tv = { .tv_sec = PING_S, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    time_t last_ping = time(NULL);
    while (1) {
        pthread_mutex_lock(&g_restart_mtx);
        int restart = g_want_restart;
        g_want_restart = 0;
        pthread_mutex_unlock(&g_restart_mtx);
        if (restart) { fprintf(stderr, "[mqtt] restart requested\n"); break; }
        int r = mqtt_read_one(s, g_cb, g_cb_arg);
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
        if (time(NULL) - last_ping >= PING_S) {
            if (mqtt_send_ping(s) != 0) break;
            last_ping = time(NULL);
        }
    }
    close(s);
    return 0;
}

static void * subscriber_thread(void * arg) {
    (void)arg;
    sleep(3);                         /* let toonui boot first */
    while (1) {
        if (subscriber_session() < 0) sleep(8);
        else                          sleep(1);
    }
    return NULL;
}

int mqtt_client_start(mqtt_on_message_cb cb, void * arg) {
    g_cb = cb; g_cb_arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, subscriber_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}

/* Legacy entry point: still respected — but we route through the same
 * shared callback + settings-driven loop, ignoring the passed topics list
 * (settings.mqtt_topics is now the source of truth). */
int mqtt_subscribe_async(const char ** topics, int n_topics,
                         mqtt_on_message_cb cb, void * arg) {
    (void)topics; (void)n_topics;
    return mqtt_client_start(cb, arg);
}
