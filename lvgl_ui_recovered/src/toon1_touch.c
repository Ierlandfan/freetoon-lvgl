/*
 * toon1_touch.c — TSC2007 evdev shim for Toon 1.
 *
 * Whole file compiles to nothing unless TOON1 is defined, so it can sit in
 * the unconditional source list without affecting the Toon 2 build.
 *
 * What it does (vs. lv_drivers/indev/evdev.c, which doesn't fit Toon 1):
 *   - opens /dev/input/event0 (the TSC2007 reported in /proc/bus/input/devices
 *     on a real Toon 1; LVGL's EVDEV_NAME default of /dev/input/event1 is
 *     wrong here).
 *   - queries ABS_X / ABS_Y min/max via EVIOCGABS once at init and scales
 *     each event linearly to DISP_HOR × DISP_VER (the kernel reports raw
 *     12-bit ADC, not pixel coords — without scaling, taps register at
 *     thousand-px coords and miss every target).
 *   - tracks press state from BTN_TOUCH and (as a fallback) ABS_PRESSURE.
 *   - non-blocking read so the LVGL timer thread doesn't stall on idle.
 */
#ifdef TOON1

#include "toon1_touch.h"
#include "display.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define TOUCH_DEVICE "/dev/input/event0"

/* Mirrors evdev_read_with_activity in main.c: any press marks activity so
 * the idle/dim timers reset. Defined in ui_stack.c; declared inline here to
 * avoid pulling another header through main.c. */
extern void ui_mark_activity(void);

static int  g_fd = -1;
static int  g_x = 0, g_y = 0;
static int  g_pressed = 0;
static long g_abs_x_min = 0,    g_abs_x_max = 4095;
static long g_abs_y_min = 0,    g_abs_y_max = 4095;

int toon1_touch_init(void) {
    g_fd = open(TOUCH_DEVICE, O_RDONLY | O_NONBLOCK);
    if (g_fd < 0) {
        fprintf(stderr, "[toon1_touch] open %s failed\n", TOUCH_DEVICE);
        return -1;
    }
    struct input_absinfo ai;
    if (ioctl(g_fd, EVIOCGABS(ABS_X), &ai) == 0) {
        g_abs_x_min = ai.minimum;
        g_abs_x_max = ai.maximum;
    }
    if (ioctl(g_fd, EVIOCGABS(ABS_Y), &ai) == 0) {
        g_abs_y_min = ai.minimum;
        g_abs_y_max = ai.maximum;
    }
    fprintf(stderr,
        "[toon1_touch] %s ABS_X=%ld..%ld ABS_Y=%ld..%ld -> %dx%d\n",
        TOUCH_DEVICE, g_abs_x_min, g_abs_x_max,
        g_abs_y_min, g_abs_y_max, DISP_HOR, DISP_VER);
    return 0;
}

void toon1_touch_read(lv_indev_drv_t * drv, lv_indev_data_t * data) {
    (void)drv;
    if (g_fd < 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int prev_pressed = g_pressed;
    struct input_event ev;
    while (read(g_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) {
                long w = g_abs_x_max - g_abs_x_min;
                if (w > 0) {
                    long v = ((long)(ev.value - g_abs_x_min)) * (long)DISP_HOR / w;
                    if (v < 0) v = 0;
                    if (v >= DISP_HOR) v = DISP_HOR - 1;
                    g_x = (int)v;
                }
            } else if (ev.code == ABS_Y) {
                long h = g_abs_y_max - g_abs_y_min;
                if (h > 0) {
                    long v = ((long)(ev.value - g_abs_y_min)) * (long)DISP_VER / h;
                    if (v < 0) v = 0;
                    if (v >= DISP_VER) v = DISP_VER - 1;
                    g_y = (int)v;
                }
            } else if (ev.code == ABS_PRESSURE) {
                /* Fallback when the driver doesn't emit BTN_TOUCH:
                 * any non-zero pressure counts as "down". */
                if (!g_pressed) g_pressed = (ev.value > 0);
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            g_pressed = (ev.value != 0);
        }
        /* EV_SYN frames the report; values accumulate into g_x/g_y/g_pressed. */
    }
    data->point.x = g_x;
    data->point.y = g_y;
    data->state   = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (g_pressed && !prev_pressed) ui_mark_activity();
}

#endif /* TOON1 */
