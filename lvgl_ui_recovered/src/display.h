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

#endif /* DISPLAY_H */
