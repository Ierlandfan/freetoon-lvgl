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
