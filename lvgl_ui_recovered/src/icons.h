#ifndef TOON_ICONS_H
#define TOON_ICONS_H

#include "lvgl/lvgl.h"

extern const lv_img_dsc_t icon_flame;
extern const lv_img_dsc_t icon_faucet;
extern const lv_img_dsc_t icon_drop;
extern const lv_img_dsc_t icon_fan;
extern const lv_img_dsc_t icon_radiator;

extern const lv_img_dsc_t icon_wx_sun;
extern const lv_img_dsc_t icon_wx_sun_cloud;
extern const lv_img_dsc_t icon_wx_cloud;
extern const lv_img_dsc_t icon_wx_rain_light;
extern const lv_img_dsc_t icon_wx_rain_heavy;
extern const lv_img_dsc_t icon_wx_thunder;
extern const lv_img_dsc_t icon_wx_bolt;
extern const lv_img_dsc_t icon_wx_moon;
extern const lv_img_dsc_t icon_wx_moon_new;
extern const lv_img_dsc_t icon_wx_moon_wax_cres;
extern const lv_img_dsc_t icon_wx_moon_first_q;
extern const lv_img_dsc_t icon_wx_moon_wax_gib;
extern const lv_img_dsc_t icon_wx_moon_full;
extern const lv_img_dsc_t icon_wx_moon_wan_gib;
extern const lv_img_dsc_t icon_wx_moon_last_q;
extern const lv_img_dsc_t icon_wx_moon_wan_cres;
extern const lv_img_dsc_t icon_wx_moon_new_lg;
extern const lv_img_dsc_t icon_wx_moon_wax_cres_lg;
extern const lv_img_dsc_t icon_wx_moon_first_q_lg;
extern const lv_img_dsc_t icon_wx_moon_wax_gib_lg;
extern const lv_img_dsc_t icon_wx_moon_full_lg;
extern const lv_img_dsc_t icon_wx_moon_wan_gib_lg;
extern const lv_img_dsc_t icon_wx_moon_last_q_lg;
extern const lv_img_dsc_t icon_wx_moon_wan_cres_lg;

/* Returns the current moon-phase icon for the given size variant
 * (size=40 for forecast strip, size=80 for dim/forecast detail).
 * Picks one of the 8 phase slots based on the current UTC date.
 * Caller decides where to paint it — usually as the night replacement
 * for the sun overlay on partly-cloudy nights, or as the cloud-icon
 * slot itself on a clear-sky night. */
const lv_img_dsc_t * moon_phase_icon(int size_px);
extern const lv_img_dsc_t icon_wx_snow;
extern const lv_img_dsc_t icon_wx_fog;

/* 80x80 variants for the dim screen + forecast detail strip. */
extern const lv_img_dsc_t icon_wx_sun_lg;
extern const lv_img_dsc_t icon_wx_sun_cloud_lg;
extern const lv_img_dsc_t icon_wx_cloud_lg;
extern const lv_img_dsc_t icon_wx_rain_light_lg;
extern const lv_img_dsc_t icon_wx_rain_heavy_lg;
extern const lv_img_dsc_t icon_wx_thunder_lg;
extern const lv_img_dsc_t icon_wx_bolt_lg;
extern const lv_img_dsc_t icon_wx_moon_lg;
extern const lv_img_dsc_t icon_wx_snow_lg;
extern const lv_img_dsc_t icon_wx_fog_lg;
extern const lv_img_dsc_t icon_trash_lg;
extern const lv_img_dsc_t icon_trash;

/* Per-waste-type icons. Picked at runtime via waste_icon_for_label(). */
extern const lv_img_dsc_t icon_news;
extern const lv_img_dsc_t icon_milk;
extern const lv_img_dsc_t icon_leaf;
extern const lv_img_dsc_t icon_wind_arrow;

const lv_img_dsc_t * waste_icon_for_label(const char * label);
unsigned int         waste_accent_for_label(const char * label);

/* Compass-angle (0.1° units, 0..3599) for the buienradar wind-direction
 * code ("N", "NO", "NNW", …). Returns -1 for unknown / empty. */
int wind_dir_angle(const char * dir);

/* 80x80 variants for the dim screen + forecast detail strip. */
extern const lv_img_dsc_t icon_wx_sun_lg;
extern const lv_img_dsc_t icon_wx_sun_cloud_lg;
extern const lv_img_dsc_t icon_wx_cloud_lg;
extern const lv_img_dsc_t icon_wx_rain_light_lg;
extern const lv_img_dsc_t icon_wx_rain_heavy_lg;
extern const lv_img_dsc_t icon_wx_thunder_lg;
extern const lv_img_dsc_t icon_wx_snow_lg;
extern const lv_img_dsc_t icon_wx_fog_lg;
extern const lv_img_dsc_t icon_trash_lg;

/* Picks a weather icon given a buienradar letter code ("a", "f", "c"...).
   Returns icon_wx_cloud for anything unknown. */
const lv_img_dsc_t * weather_icon_for(const char * letter);
const lv_img_dsc_t * weather_icon_for_lg(const char * letter);

/* RGB tint to apply via lv_obj_set_style_img_recolor when rendering the
 * weather icon. Sun = warm yellow, rain = steel blue, thunder = amber,
 * snow = ice-white, fog = grey, cloud = soft blue-grey. */
unsigned int weather_icon_color_for(const char * letter);

extern const lv_img_dsc_t icon_wx_sun;
extern const lv_img_dsc_t icon_wx_sun_cloud;
extern const lv_img_dsc_t icon_wx_cloud;
extern const lv_img_dsc_t icon_wx_rain_light;
extern const lv_img_dsc_t icon_wx_rain_heavy;
extern const lv_img_dsc_t icon_wx_thunder;
extern const lv_img_dsc_t icon_wx_snow;
extern const lv_img_dsc_t icon_wx_fog;

/* Picks a weather icon given a buienradar letter code ("a", "f", "c"...).
   Returns icon_wx_cloud for anything unknown. */
const lv_img_dsc_t * weather_icon_for(const char * letter);

#endif
