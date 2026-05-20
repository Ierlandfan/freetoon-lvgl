/* Minimal LVGL 8.x config for the freetoon CYD client. Only overrides the
 * settings we care about — lv_conf_internal.h fills in defaults for the rest. */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH      16
#define LV_COLOR_16_SWAP    1          /* TFT_eSPI wants byte-swapped RGB565 */

#define LV_MEM_CUSTOM       0
#define LV_MEM_SIZE         (48U * 1024U)

#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_INDEV_DEF_READ_PERIOD 30

/* Drive LVGL's tick from Arduino millis() so we don't need lv_tick_inc(). */
#define LV_TICK_CUSTOM              1
#define LV_TICK_CUSTOM_INCLUDE      "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_USE_LOG          0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0

/* Fonts used by the UI. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT     &lv_font_montserrat_14

/* Widgets we use (most default on in 8.x, set explicitly to be safe). */
#define LV_USE_LABEL   1
#define LV_USE_BTN     1
#define LV_USE_BAR     1

#endif /* LV_CONF_H */
