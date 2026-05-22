#ifndef TOON_BACKLIGHT_H
#define TOON_BACKLIGHT_H

/* Write to /sys/class/backlight/mp3309-bl/brightness.
   Valid range 0..1000 on this hardware. */
void backlight_set(int level);
int  backlight_get(void);
/* Ambient-light sensor (Toon 2 LTR-303). raw: -1 if no sensor. auto_level maps
   ambient light into [dim..active]; -1 when no sensor (use the fixed value). */
int  backlight_als_raw(void);            /* cached, non-blocking */
int  backlight_auto_level(int dim, int active);
void backlight_als_start(void);          /* start the background sensor poller */

#endif
