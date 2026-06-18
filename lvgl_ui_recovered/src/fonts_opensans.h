/*
 * fonts_opensans.h — Open Sans LVGL fonts for the "stock theme" home.
 *
 * The stock qt-gui renders in Open Sans (Light for big readouts, Regular for
 * body/labels, SemiBold for the TOON wordmark). These are the freely-licensed
 * OpenSans TTFs lifted from the device's drawables.rcc, baked to LVGL 4bpp at
 * the sizes themes/Fonts.qml uses. Pickers scale design-px by the panel ratio
 * exactly like display.h's SF(), so Toon 1 (800x480) shrinks cleanly.
 *
 * Big Light fonts are glyph-subset to the readout charset (digits, °, % + - .
 * / : and b a r W t) to keep flash small.
 */
#ifndef FONTS_OPENSANS_H
#define FONTS_OPENSANS_H

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif
#include "display.h"   /* DISP_HOR / DESIGN_HOR */

LV_FONT_DECLARE(lv_font_os_light_30)
LV_FONT_DECLARE(lv_font_os_light_40)
LV_FONT_DECLARE(lv_font_os_light_50)
LV_FONT_DECLARE(lv_font_os_reg_13)
LV_FONT_DECLARE(lv_font_os_reg_14)
LV_FONT_DECLARE(lv_font_os_reg_15)
LV_FONT_DECLARE(lv_font_os_reg_16)
LV_FONT_DECLARE(lv_font_os_reg_18)
LV_FONT_DECLARE(lv_font_os_reg_20)
LV_FONT_DECLARE(lv_font_os_reg_24)
LV_FONT_DECLARE(lv_font_os_reg_30)
LV_FONT_DECLARE(lv_font_os_semi_28)

static inline const lv_font_t * os_light(int px) {
    int p = (px * DISP_HOR) / DESIGN_HOR;
    if (p >= 45) return &lv_font_os_light_50;
    if (p >= 35) return &lv_font_os_light_40;
    return &lv_font_os_light_30;
}
static inline const lv_font_t * os_reg(int px) {
    int p = (px * DISP_HOR) / DESIGN_HOR;
    if (p >= 28) return &lv_font_os_reg_30;
    if (p >= 22) return &lv_font_os_reg_24;
    if (p >= 19) return &lv_font_os_reg_20;
    if (p >= 17) return &lv_font_os_reg_18;
    if (p >= 16) return &lv_font_os_reg_16;
    if (p >= 15) return &lv_font_os_reg_15;
    if (p >= 14) return &lv_font_os_reg_14;
    return &lv_font_os_reg_13;
}
static inline const lv_font_t * os_semi(int px) { (void)px; return &lv_font_os_semi_28; }

#define OSL(px) os_light(px)
#define OSR(px) os_reg(px)
#define OSS(px) os_semi(px)

#endif /* FONTS_OPENSANS_H */
