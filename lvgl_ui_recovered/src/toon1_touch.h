#ifndef TOON_TOON1_TOUCH_H
#define TOON_TOON1_TOUCH_H

#include "lvgl/lvgl.h"

/* Toon 1's stock UI runs through Tslib + a TSC2007 4-wire resistive panel.
 * LVGL's stock evdev driver:
 *   - looks at /dev/input/event1 (TSC2007 is at /dev/input/event0)
 *   - passes raw ABS_X/Y ADC values through as if they were pixel coords
 * Both failure modes look identical on screen: UI paints, touch dead.
 *
 * This shim:
 *   - opens /dev/input/event0 (TSC2007 Touchscreen)
 *   - reads ABS_X/Y min/max via EVIOCGABS at init, scales linearly to
 *     the configured DISP_HOR/DISP_VER on every event
 *   - tracks BTN_TOUCH + ABS_PRESSURE for the pressed/released state
 *
 * Wired into main.c under `#ifdef TOON1` instead of evdev_init / evdev_read. */

int  toon1_touch_init(void);
void toon1_touch_read(lv_indev_drv_t * drv, lv_indev_data_t * data);

#endif
