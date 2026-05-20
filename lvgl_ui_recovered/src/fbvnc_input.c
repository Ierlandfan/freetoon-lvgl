/*
 * fbvnc_input — input bridge for `x11vnc -rawfb` on the Toon.
 *
 * x11vnc has no X server to inject into in -rawfb mode, so it pipes every
 * remote pointer/key event (one per line) to this program's stdin via
 * `-pipeinput`. We translate pointer events into multitouch writes on
 * /dev/input/event1 (the Solomon SSD254x touchscreen), so a VNC client can
 * actually tap/drag the Toon UI.
 *
 * x11vnc pipeinput line format:
 *   "Pointer <clientid> <x> <y> <buttonmask>"
 *   "Keysym  <clientid> <down> <keycode> <keysym> <name>"   (ignored here)
 * buttonmask bit0 = left button = "finger down".
 *
 * Build: arm-linux-gnueabihf-gcc -O2 fbvnc_input.c -o fbvnc_input
 * Used by: x11vnc -pipeinput reopen:/mnt/data/fbvnc_input
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <time.h>

#define TS_DEV "/dev/input/event1"
#define SCR_W  1024
#define SCR_H  600

static int ts_fd = -1;

static void ev(int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof(e));
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    e.time.tv_sec  = ts.tv_sec;
    e.time.tv_usec = ts.tv_nsec / 1000;
    e.type = type; e.code = code; e.value = value;
    if (write(ts_fd, &e, sizeof(e)) != (ssize_t)sizeof(e)) {
        /* device may briefly reject — not fatal, next event retries */
    }
}

int main(void) {
    ts_fd = open(TS_DEV, O_WRONLY);
    if (ts_fd < 0) { perror("open " TS_DEV); return 1; }

    /* Line-buffered stdin from x11vnc. */
    char line[256];
    int  pressed = 0;        /* current finger-down state */
    int  last_x = -1, last_y = -1;
    int  tracking = 100;

    while (fgets(line, sizeof(line), stdin)) {
        int cid, x, y, mask;
        if (sscanf(line, "Pointer %d %d %d %d", &cid, &x, &y, &mask) != 4)
            continue;                       /* Keysym / other — ignore */

        if (x < 0) x = 0; if (x >= SCR_W) x = SCR_W - 1;
        if (y < 0) y = 0; if (y >= SCR_H) y = SCR_H - 1;

        int down = (mask & 1) ? 1 : 0;

        if (down && !pressed) {
            /* finger down — defeat the kernel's ABS dedup by nudging the
               first coordinate if it equals the previous touch's. */
            int sx = x, sy = y;
            if (sx == last_x && sy == last_y) { sx = (sx > 0) ? sx - 1 : sx + 1; }
            ev(EV_ABS, ABS_MT_TRACKING_ID, ++tracking);
            ev(EV_KEY, BTN_TOUCH, 1);
            ev(EV_ABS, ABS_MT_POSITION_X, sx);
            ev(EV_ABS, ABS_MT_POSITION_Y, sy);
            ev(EV_SYN, SYN_REPORT, 0);
            if (sx != x || sy != y) {        /* settle on the true point */
                ev(EV_ABS, ABS_MT_POSITION_X, x);
                ev(EV_ABS, ABS_MT_POSITION_Y, y);
                ev(EV_SYN, SYN_REPORT, 0);
            }
            pressed = 1;
        } else if (down && pressed) {
            /* drag — only emit if the point actually moved */
            if (x != last_x || y != last_y) {
                ev(EV_ABS, ABS_MT_POSITION_X, x);
                ev(EV_ABS, ABS_MT_POSITION_Y, y);
                ev(EV_SYN, SYN_REPORT, 0);
            }
        } else if (!down && pressed) {
            /* finger up */
            ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
            ev(EV_KEY, BTN_TOUCH, 0);
            ev(EV_SYN, SYN_REPORT, 0);
            pressed = 0;
        }
        last_x = x; last_y = y;
    }

    if (ts_fd >= 0) close(ts_fd);
    return 0;
}
