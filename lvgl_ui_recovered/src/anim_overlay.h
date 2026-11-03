#ifndef ANIM_OVERLAY_H
#define ANIM_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANIM_EFFECT_NONE = 0,
    ANIM_EFFECT_BALLOONS,
    ANIM_EFFECT_CONFETTI,
    ANIM_EFFECT_SNOW,
    ANIM_EFFECT_FIREWORKS,
    ANIM_EFFECT_LEAVES,
    ANIM_EFFECT_HEARTS,
    ANIM_EFFECT_SINT,
    ANIM_EFFECT_CHRISTMAS,
    ANIM_EFFECT_COUNT
} anim_effect_t;

/* Initialize the animation overlay subsystem and transparent sys-layer container */
void anim_overlay_init(void);

/* Trigger an animation effect for duration_sec seconds */
void anim_overlay_trigger(anim_effect_t effect, int duration_sec);

/* Trigger an animation effect by name ("balloons", "confetti", "snow", "fireworks", "leaves", "hearts") */
void anim_overlay_trigger_by_name(const char * name, int duration_sec);

/* Stop any currently running animation immediately */
void anim_overlay_stop(void);

/* Query active state and current effect */
int anim_overlay_is_active(void);
anim_effect_t anim_overlay_get_current(void);

/* Names and UI titles */
const char * anim_effect_name(anim_effect_t effect);
const char * anim_effect_title(anim_effect_t effect);
anim_effect_t anim_effect_from_name(const char * name);

/* Periodic check for seasonal dates (Sinterklaas, New Year, winter snow, etc.) */
void anim_overlay_check_seasonal(void);

#ifdef __cplusplus
}
#endif

#endif /* ANIM_OVERLAY_H */

