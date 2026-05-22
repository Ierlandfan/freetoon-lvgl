#ifndef TOON_DOORBELL_H
#define TOON_DOORBELL_H

/* Doorbell snapshot overlay. Registers a small LVGL timer that watches
 * ha_state.doorbell_seq (bumped by homeassistant.c's poll_doorbell when the
 * configured trigger entity fires) and shows the freshly-fetched camera
 * snapshot fullscreen on the top layer, over whatever screen is active.
 * Auto-dismisses after settings.doorbell_seconds, or on tap. */
void doorbell_ui_init(void);

#endif
