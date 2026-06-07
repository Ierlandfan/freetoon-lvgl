/*
 * External command channel for toonui. See ui_cmd.h for protocol.
 *
 * Threading: a background pthread accept()s on the UNIX socket and reads
 * one line per connection. It does NOT touch LVGL directly -- instead it
 * sets bits in a volatile sig_atomic_t that an LVGL timer (running on the
 * UI thread) drains every 100 ms and turns into video_open/close calls.
 * That keeps all LVGL state under the UI thread without a mutex.
 *
 * Built only on TOON1 -- the doorbell pipeline doesn't exist on Toon 2.
 */
#include "ui_cmd.h"

#ifndef TOON1
void ui_cmd_start(void) {}
#else

#include "video.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define UI_SOCK_PATH "/tmp/toonui.cmd"

/* Bit 0 = show pending, bit 1 = hide pending. The listener thread ORs
 * into this with __sync; the LVGL drain timer atomically swaps to 0. */
static volatile sig_atomic_t s_pending = 0;
#define CMD_SHOW  0x01
#define CMD_HIDE  0x02

static void drain_cb(lv_timer_t * t)
{
    (void)t;
    /* Atomic swap -> empty so we don't lose a command if one arrives
     * between read and clear. */
    int p = __sync_fetch_and_and(&s_pending, 0);
    if (p == 0) return;
    /* If both show + hide arrived since the last tick (~100ms), the
     * later wins -- that's intentional, otherwise a fast tap-tap from
     * HA would settle in the wrong state. We treat the bits as a set
     * and apply hide last so an unbalanced pair ends hidden. */
    if (p & CMD_SHOW) video_open();
    if (p & CMD_HIDE) video_close();
}

static void * listener(void * unused)
{
    (void)unused;

    /* Stale socket from a previous run -- unlink before bind. */
    unlink(UI_SOCK_PATH);

    int lsock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lsock < 0) { perror("[ui_cmd] socket"); return NULL; }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, UI_SOCK_PATH, sizeof(sa.sun_path) - 1);
    if (bind(lsock, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("[ui_cmd] bind"); close(lsock); return NULL;
    }
    /* 0666: the daemon may run as a different uid than toonui. The
     * doorbell trigger only opens/closes the camera, so it isn't a
     * privilege escalation risk -- but keep it on /tmp so it dies on
     * reboot regardless. */
    chmod(UI_SOCK_PATH, 0666);
    listen(lsock, 4);

    printf("[ui_cmd] listening on %s\n", UI_SOCK_PATH);
    fflush(stdout);

    for (;;) {
        int sock = accept(lsock, NULL, NULL);
        if (sock < 0) {
            if (errno == EINTR) continue;
            perror("[ui_cmd] accept");
            break;
        }
        /* One short line per connection. 64 B is plenty. */
        char buf[64];
        int n = read(sock, buf, sizeof(buf) - 1);
        close(sock);
        if (n <= 0) continue;
        buf[n] = 0;
        /* Strip trailing whitespace incl. \r\n. */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                         buf[n-1] == ' '  || buf[n-1] == '\t')) {
            buf[--n] = 0;
        }
        if (!strcmp(buf, "show")) {
            __sync_or_and_fetch(&s_pending, CMD_SHOW);
            printf("[ui_cmd] -> show\n"); fflush(stdout);
        } else if (!strcmp(buf, "hide")) {
            __sync_or_and_fetch(&s_pending, CMD_HIDE);
            printf("[ui_cmd] -> hide\n"); fflush(stdout);
        } else {
            fprintf(stderr, "[ui_cmd] unknown command: '%s'\n", buf);
        }
    }
    close(lsock);
    unlink(UI_SOCK_PATH);
    return NULL;
}

void ui_cmd_start(void)
{
    /* 100 ms drain interval: HA trigger -> visible video is dominated by
     * the next-I-VOP wait inside vpu_stream (avg ~half a GOP, ~166 ms at
     * GOP=5), so an extra 0-100 ms in the poll is well within the noise
     * and avoids a dedicated wake mechanism. */
    lv_timer_create(drain_cb, 100, NULL);

    pthread_t t;
    if (pthread_create(&t, NULL, listener, NULL) != 0) {
        perror("[ui_cmd] pthread_create");
        return;
    }
    pthread_detach(t);
}

#endif /* TOON1 */
