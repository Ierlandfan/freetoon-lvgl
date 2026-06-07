#ifndef TOON_BACKLIGHT_H
#define TOON_BACKLIGHT_H

/* Set/read the backlight in a fixed logical 0..1000 range. The sysfs node and
   hardware max differ per board (Toon 2 MP3309 = 0..1000, Toon 1 = 0..100);
   backlight.c scales to the hardware range and applies a safety floor. While
   Night mode is active, backlight_set() also scales the value to night_pct%. */
void backlight_set(int level);
int  backlight_get(void);
/* Toon 2 only: LTR-303 ambient-light auto-brightness. auto_level returns a
   level in [dim..active], or -1 when the sensor isn't ready / not present
   (Toon 1 always returns -1, so the caller uses the fixed active value). */
int  backlight_als_raw(void);            /* cached, non-blocking */
int  backlight_auto_level(int dim, int active);
void backlight_als_start(void);          /* start the background poller(s) */
/* Night mode: 1 when the screen should be dimmed right now (per settings:
   fixed local time range, or geocoded sunset->sunrise). 0 otherwise. */
int  backlight_night_active(void);
/* Last fetched sunrise/sunset as UTC epochs (0 = not known yet). Convert with
   localtime for display. */
void backlight_sun_times(long * sunrise, long * sunset);

#endif
