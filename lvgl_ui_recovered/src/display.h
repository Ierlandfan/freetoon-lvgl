#ifndef DISPLAY_H
#define DISPLAY_H

#include "lvgl/lvgl.h"

/*
 * Per-target display geometry.
 *
 * The whole UI was authored at 1024x600 (Toon 2's panel) with absolute
 * coordinates. Toon 1 has an 800x480 panel and a soft-float ARMv5 CPU, so it
 * builds with -DTOON1 (see the Makefile TARGET=toon1 variant).
 *
 * Rather than re-author every screen up front, TOON1 keeps the same design
 * space and exposes SX()/SY()/SUNI() helpers that map a 1024x600 coordinate
 * onto the active panel. New / ported layout code should use these so it fits
 * both panels; the existing screens still carry raw 1024x600 coordinates and
 * get hand-tuned for 800x480 once there's hardware to verify against. On a
 * Toon 2 build the macros are the identity, so nothing changes there.
 */

#define DESIGN_HOR 1024
#define DESIGN_VER 600

#ifdef TOON1
  #define DISP_HOR 800
  #define DISP_VER 480
#else
  #define DISP_HOR DESIGN_HOR
  #define DISP_VER DESIGN_VER
#endif

/* Map a design-space X/Y onto the active panel (identity on Toon 2). */
#define SX(x)   ((lv_coord_t)(((int)(x) * DISP_HOR) / DESIGN_HOR))
#define SY(y)   ((lv_coord_t)(((int)(y) * DISP_VER) / DESIGN_VER))
/* Uniform scale for square things (icons, radii, fonts): use the X ratio,
 * which matches on Toon 2 and is the gentler axis on Toon 1's 800x480. */
#define SUNI(v) ((lv_coord_t)(((int)(v) * DISP_HOR) / DESIGN_HOR))

/*
 * Text scaling. Montserrat is a compiled bitmap font (no runtime scaling), so
 * we map a DESIGN-space font height onto the nearest ENABLED Montserrat after
 * scaling by the panel ratio. Enabled sizes: 12,14,18,20,22,28,48,64,96.
 *
 * On Toon 2 the ratio is 1:1, so a design size lands back on the identical
 * font (every design size used in the UI is itself an enabled size) — Toon 2
 * therefore renders byte-for-byte unchanged. On Toon 1 the text shrinks with
 * the layout instead of staying full-size and crowding the smaller panel.
 *
 * Use SF(px) wherever a literal &lv_font_montserrat_<px> would have gone.
 * The 64/96 readout fonts ship as custom glyph subsets (digits/°), declared here.
 */
LV_FONT_DECLARE(lv_font_montserrat_64_custom)
LV_FONT_DECLARE(lv_font_montserrat_96_custom)
static inline const lv_font_t * disp_font(int design_px) {
    int px = (design_px * DISP_HOR) / DESIGN_HOR;
    if (px >= 80) return &lv_font_montserrat_96_custom;
    if (px >= 56) return &lv_font_montserrat_64_custom;
    if (px >= 38) return &lv_font_montserrat_48;
    if (px >= 25) return &lv_font_montserrat_28;
    if (px >= 21) return &lv_font_montserrat_22;
    if (px >= 19) return &lv_font_montserrat_20;
    if (px >= 16) return &lv_font_montserrat_18;
    if (px >= 13) return &lv_font_montserrat_14;
    return &lv_font_montserrat_12;
}
#define SF(px) disp_font(px)

#endif /* DISPLAY_H */
