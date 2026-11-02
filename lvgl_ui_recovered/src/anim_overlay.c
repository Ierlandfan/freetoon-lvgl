#include "anim_overlay.h"
#include "settings.h"
#include "display.h"
#include "screens.h"
#include "i18n.h"
#include "icon_anim_balloon.h"
#include "icon_anim_heart.h"
#include "icon_anim_leaf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define MAX_PARTICLES 24
#define TICK_MS 35

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    lv_obj_t * cont;
    lv_obj_t * body;
    lv_obj_t * tail;
    lv_obj_t * lbl;
    lv_obj_t * img;
    lv_obj_t * gloss;
    float x, y;
    float base_x;
    float vx, vy;
    float phase;
    float amp;
    float freq;
    float tumble_angle;
    float tumble_speed;
    int base_w, base_h;
    int life;
    int max_life;
    int group;
    uint32_t color;
    int active;
} particle_t;

static lv_obj_t * s_overlay = NULL;
static lv_timer_t * s_timer = NULL;
static lv_timer_t * s_pending_timer = NULL;
static lv_timer_t * s_seasonal_timer = NULL;
static anim_effect_t s_current_effect = ANIM_EFFECT_NONE;
static uint32_t s_stop_tick = 0;
static int s_active_day = -1;
static uint32_t s_tick_counter = 0;
static particle_t s_particles[MAX_PARTICLES];
static volatile int s_pending_effect = -1;
static volatile int s_pending_duration = 0;
static void anim_pending_cb(lv_timer_t * t);

/* Color palettes */
static const uint32_t PALETTE_BALLOONS[] = {
    0xe53935, /* Red */
    0x1e88e5, /* Royal Blue */
    0xfbc02d, /* Yellow Gold */
    0x43a047, /* Emerald Green */
    0x8e24aa, /* Purple */
    0xfb8c00, /* Vibrant Orange */
    0xd81b60, /* Deep Pink */
    0x00acc1  /* Cyan */
};
#define NUM_BALLOONS (sizeof(PALETTE_BALLOONS)/sizeof(PALETTE_BALLOONS[0]))

static const uint32_t PALETTE_CONFETTI[] = {
    0xffeb3b, /* Bright Yellow */
    0xff4081, /* Hot Pink */
    0x00e5ff, /* Cyan */
    0x76ff03, /* Neon Lime */
    0xff6e40, /* Orange */
    0xd500f9, /* Magenta */
    0xffffff  /* White */
};
#define NUM_CONFETTI (sizeof(PALETTE_CONFETTI)/sizeof(PALETTE_CONFETTI[0]))

static const uint32_t PALETTE_SNOW[] = {
    0xffffff,
    0xf0f9ff,
    0xe0f2fe,
    0xbae6fd
};
#define NUM_SNOW (sizeof(PALETTE_SNOW)/sizeof(PALETTE_SNOW[0]))

static const uint32_t PALETTE_LEAVES[] = {
    0xd97706, /* Amber */
    0xb91c1c, /* Crimson */
    0xeab308, /* Gold */
    0x9a3412, /* Russet / copper */
    0x78350f  /* Deep chestnut */
};
#define NUM_LEAVES (sizeof(PALETTE_LEAVES)/sizeof(PALETTE_LEAVES[0]))

static const uint32_t PALETTE_FIREWORKS[] = {
    0xffeb3b, /* Gold */
    0xff1744, /* Ruby */
    0x00e676, /* Spring Green */
    0x00e5ff, /* Cyan */
    0xe040fb, /* Violet */
    0xff9100  /* Amber Orange */
};
#define NUM_FIREWORKS (sizeof(PALETTE_FIREWORKS)/sizeof(PALETTE_FIREWORKS[0]))

/* Helper random float in range [min, max] */
static float frand(float min, float max) {
    float r = (float)rand() / (float)RAND_MAX;
    return min + r * (max - min);
}

static void spawn_balloon(particle_t * p, int stagger) {
    p->active = 1;
    /* Full restored size: 54x84 native asset with gentle organic variation (+-8%) */
    int zoom = 256 + (rand() % 41) - 20;

    p->base_w = (54 * zoom) / 256;
    p->base_h = (84 * zoom) / 256;
    p->color = PALETTE_BALLOONS[rand() % NUM_BALLOONS];
    p->base_x = frand(SUNI(15), DISP_HOR - SUNI(65));
    p->amp = frand(SUNI(14), SUNI(30));
    p->freq = frand(0.030f, 0.055f);
    p->phase = frand(0, (float)(2 * M_PI));
    p->vy = -frand(1.8f, 3.2f);
    p->x = p->base_x;

    if (stagger) {
        /* Distribute across the screen height on initial trigger so screen is immediately festive */
        p->y = frand(0, (float)(DISP_VER + 100));
    } else {
        /* Respawn below bottom */
        p->y = (float)(DISP_VER + frand(10, 100));
    }

    lv_obj_clear_flag(p->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p->gloss, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->tail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->lbl, LV_OBJ_FLAG_HIDDEN);

    /* Balloon image (teardrop dome, tapered neck, flared knot, wavy ribbon) */
    lv_img_set_src(p->img, &icon_anim_balloon);
    lv_img_set_angle(p->img, 0);
    lv_img_set_pivot(p->img, 27, 42);
    lv_img_set_zoom(p->img, zoom);
    lv_obj_set_style_img_recolor(p->img, lv_color_hex(p->color), 0);
    lv_obj_set_style_img_recolor_opa(p->img, 255, 0);
    lv_obj_set_pos(p->img, 0, 0);

    /* Specular gloss highlight (curved white reflection oval near top-left of dome) */
    int gw = (12 * zoom) / 256; if (gw < 4) gw = 4;
    int gh = (18 * zoom) / 256; if (gh < 6) gh = 6;
    int gx = (13 * zoom) / 256;
    int gy = (10 * zoom) / 256;
    lv_obj_set_size(p->gloss, gw, gh);
    lv_obj_set_pos(p->gloss, gx, gy);
    lv_obj_set_style_radius(p->gloss, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p->gloss, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(p->gloss, LV_OPA_60, 0);

    lv_obj_set_size(p->cont, p->base_w + 4, p->base_h + 4);
    lv_obj_clear_flag(p->cont, LV_OBJ_FLAG_HIDDEN);
}

static void spawn_confetti(particle_t * p, int stagger) {
    p->active = 1;
    p->base_w = SUNI(10) + (rand() % SUNI(8));
    p->base_h = SUNI(12) + (rand() % SUNI(8));
    p->color = PALETTE_CONFETTI[rand() % NUM_CONFETTI];
    p->base_x = frand(SUNI(10), DISP_HOR - SUNI(30));
    p->amp = frand(SUNI(12), SUNI(28));
    p->freq = frand(0.04f, 0.08f);
    p->phase = frand(0, (float)(2 * M_PI));
    p->tumble_angle = frand(0, (float)(2 * M_PI));
    p->tumble_speed = frand(0.12f, 0.28f);
    p->vy = frand(3.0f, 5.5f);
    p->x = p->base_x;

    if (stagger) {
        p->y = frand(-50, (float)DISP_VER);
    } else {
        p->y = -frand(15, 80);
    }

    lv_obj_clear_flag(p->body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->tail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->gloss, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_size(p->body, p->base_w, p->base_h);
    lv_obj_set_pos(p->body, 0, 0);
    lv_obj_set_style_radius(p->body, 3, 0);
    lv_obj_set_style_bg_color(p->body, lv_color_hex(p->color), 0);
    lv_obj_set_style_bg_opa(p->body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p->body, 0, 0);

    lv_obj_set_size(p->cont, p->base_w + 4, p->base_h + 4);
    lv_obj_clear_flag(p->cont, LV_OBJ_FLAG_HIDDEN);
}

static void spawn_snow(particle_t * p, int stagger) {
    p->active = 1;
    int sz = SUNI(6) + (rand() % SUNI(9));
    p->base_w = sz;
    p->base_h = sz;
    p->color = PALETTE_SNOW[rand() % NUM_SNOW];
    p->base_x = frand(SUNI(10), DISP_HOR - SUNI(20));
    p->amp = frand(SUNI(10), SUNI(24));
    p->freq = frand(0.025f, 0.05f);
    p->phase = frand(0, (float)(2 * M_PI));
    p->vy = frand(1.2f, 2.6f);
    p->x = p->base_x;

    if (stagger) {
        p->y = frand(-30, (float)DISP_VER);
    } else {
        p->y = -frand(10, 60);
    }

    lv_obj_clear_flag(p->body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->tail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->gloss, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_size(p->body, sz, sz);
    lv_obj_set_pos(p->body, 0, 0);
    lv_obj_set_style_radius(p->body, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p->body, lv_color_hex(p->color), 0);
    lv_obj_set_style_bg_opa(p->body, (sz > SUNI(10)) ? LV_OPA_90 : LV_OPA_70, 0);
    lv_obj_set_style_border_width(p->body, 0, 0);

    lv_obj_set_size(p->cont, sz + 2, sz + 2);
    lv_obj_clear_flag(p->cont, LV_OBJ_FLAG_HIDDEN);
}

static void spawn_leaf(particle_t * p, int stagger) {
    p->active = 1;
    /* Full bold 64x64 autumn oak/maple leaf asset with gentle size variation */
    int zoom = 256 + (rand() % 41) - 20;

    p->base_w = (64 * zoom) / 256;
    p->base_h = (64 * zoom) / 256;
    p->color = PALETTE_LEAVES[rand() % NUM_LEAVES];
    p->base_x = frand(SUNI(20), DISP_HOR - SUNI(70));
    p->amp = frand(SUNI(20), SUNI(45));
    p->freq = frand(0.025f, 0.055f);
    p->phase = frand(0, (float)(2 * M_PI));
    p->tumble_angle = frand(-0.35f, 0.35f);
    p->tumble_speed = frand(0.05f, 0.12f);
    p->vy = frand(1.6f, 3.0f);
    p->x = p->base_x;

    if (stagger) {
        p->y = frand(-40, (float)DISP_VER);
    } else {
        p->y = -frand(20, 80);
    }

    lv_obj_clear_flag(p->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->gloss, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->tail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->lbl, LV_OBJ_FLAG_HIDDEN);

    /* Leaf image descriptor (broad full-face oak/maple leaf, 64x64 native) */
    lv_img_set_src(p->img, &icon_anim_leaf);
    lv_img_set_pivot(p->img, 32, 32);
    lv_img_set_angle(p->img, 0);
    lv_img_set_zoom(p->img, zoom);
    lv_obj_set_style_img_recolor(p->img, lv_color_hex(p->color), 0);
    lv_obj_set_style_img_recolor_opa(p->img, 255, 0);
    lv_obj_set_pos(p->img, 0, 0);

    lv_obj_set_size(p->cont, p->base_w + 12, p->base_h + 12);
    lv_obj_clear_flag(p->cont, LV_OBJ_FLAG_HIDDEN);
}

static void spawn_heart(particle_t * p, int stagger) {
    p->active = 1;
    /* Bold 64x64 Valentine heart asset with gentle organic size variation */
    int zoom = 256 + (rand() % 41) - 20;

    p->base_w = (64 * zoom) / 256;
    p->base_h = (64 * zoom) / 256;
    static const uint32_t HEART_COLORS[] = { 0xe11d48, 0xf43f5e, 0xff2d55, 0xec4899 };
    p->color = HEART_COLORS[rand() % 4];
    p->base_x = frand(SUNI(15), DISP_HOR - SUNI(60));
    p->amp = frand(SUNI(12), SUNI(28));
    p->freq = frand(0.035f, 0.065f);
    p->phase = frand(0, (float)(2 * M_PI));
    p->vy = -frand(1.8f, 3.2f);
    p->x = p->base_x;

    if (stagger) {
        p->y = frand(0, (float)(DISP_VER + 60));
    } else {
        p->y = (float)(DISP_VER + frand(10, 80));
    }

    lv_obj_clear_flag(p->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->gloss, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->tail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->lbl, LV_OBJ_FLAG_HIDDEN);

    /* Heart anti-aliased image descriptor (64x64 native) */
    lv_img_set_src(p->img, &icon_anim_heart);
    lv_img_set_pivot(p->img, 32, 32);
    lv_img_set_angle(p->img, 0);
    lv_img_set_zoom(p->img, zoom);
    lv_obj_set_style_img_recolor(p->img, lv_color_hex(p->color), 0);
    lv_obj_set_style_img_recolor_opa(p->img, 255, 0);
    lv_obj_set_pos(p->img, 0, 0);

    lv_obj_set_size(p->cont, p->base_w + 10, p->base_h + 10);
    lv_obj_clear_flag(p->cont, LV_OBJ_FLAG_HIDDEN);
}

/* Firework burst group: 12 sparks radiating from (cx, cy) */
static void spawn_firework_burst(int group) {
    int start_idx = group * (MAX_PARTICLES / 2);
    int count = MAX_PARTICLES / 2;

    float cx = frand(DISP_HOR * 0.18f, DISP_HOR * 0.82f);
    float cy = frand(DISP_VER * 0.18f, DISP_VER * 0.52f);
    uint32_t color = PALETTE_FIREWORKS[rand() % NUM_FIREWORKS];

    for (int i = 0; i < count; i++) {
        particle_t * p = &s_particles[start_idx + i];
        p->active = 1;
        p->group = group;
        p->x = cx;
        p->y = cy;
        p->color = color;
        p->life = 32;
        p->max_life = 32;

        float angle = (float)((i * 2 * M_PI) / count) + frand(-0.25f, 0.25f);
        float spd = frand(4.0f, 7.5f);
        p->vx = cosf(angle) * spd;
        p->vy = sinf(angle) * spd;

        int sz = SUNI(5) + (rand() % SUNI(4));
        p->base_w = sz;
        p->base_h = sz;

        lv_obj_clear_flag(p->body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->tail, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->gloss, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(p->body, sz, sz);
        lv_obj_set_pos(p->body, 0, 0);
        lv_obj_set_style_radius(p->body, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(p->body, lv_color_hex(p->color), 0);
        lv_obj_set_style_bg_opa(p->body, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(p->body, 0, 0);

        lv_obj_set_size(p->cont, sz + 2, sz + 2);
        lv_obj_clear_flag(p->cont, LV_OBJ_FLAG_HIDDEN);
    }
}

static void anim_timer_tick(lv_timer_t * timer) {
    (void)timer;
    if (s_current_effect == ANIM_EFFECT_NONE) return;

    uint32_t now = lv_tick_get();
    if (s_stop_tick > 0) {
        if (now >= s_stop_tick) {
            anim_overlay_stop();
            return;
        }
    } else {
        /* Continuous mode: check every ~3.5s (100 ticks) if day rolled over past midnight */
        if (s_tick_counter % 100 == 0 && s_active_day >= 0) {
            time_t now_sec = time(NULL);
            struct tm tm_now;
            if (localtime_r(&now_sec, &tm_now) && tm_now.tm_yday != s_active_day) {
                anim_overlay_stop();
                return;
            }
        }
    }

    /* Respect dim setting: if dimmed and user hasn't opted into dim animations, hide overlay */
    if (ui_is_dimmed() && !settings.anim_dim_enabled) {
        if (!lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    } else {
        if (lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_tick_counter++;

    if (s_current_effect == ANIM_EFFECT_BALLOONS) {
        for (int i = 0; i < MAX_PARTICLES; i++) {
            particle_t * p = &s_particles[i];
            if (!p->active) continue;
            p->y += p->vy;
            p->x = p->base_x + p->amp * sinf(p->phase + (float)s_tick_counter * p->freq);

            if (p->y < -SUNI(80)) {
                spawn_balloon(p, 0);
            }
            lv_obj_set_pos(p->cont, (lv_coord_t)p->x, (lv_coord_t)p->y);
        }
    }
    else if (s_current_effect == ANIM_EFFECT_CONFETTI) {
        for (int i = 0; i < MAX_PARTICLES; i++) {
            particle_t * p = &s_particles[i];
            if (!p->active) continue;
            p->y += p->vy;
            p->x = p->base_x + p->amp * sinf(p->phase + (float)s_tick_counter * p->freq);

            /* 3D tumble flutter */
            p->tumble_angle += p->tumble_speed;
            int cur_w = (int)(fabsf(cosf(p->tumble_angle)) * (float)p->base_w);
            if (cur_w < 2) cur_w = 2;
            lv_obj_set_width(p->body, cur_w);

            if (p->y > DISP_VER + SUNI(20)) {
                spawn_confetti(p, 0);
            }
            lv_obj_set_pos(p->cont, (lv_coord_t)p->x, (lv_coord_t)p->y);
        }
    }
    else if (s_current_effect == ANIM_EFFECT_SNOW) {
        for (int i = 0; i < MAX_PARTICLES; i++) {
            particle_t * p = &s_particles[i];
            if (!p->active) continue;
            p->y += p->vy;
            p->x = p->base_x + p->amp * sinf(p->phase + (float)s_tick_counter * p->freq);

            if (p->y > DISP_VER + SUNI(10)) {
                spawn_snow(p, 0);
            }
            lv_obj_set_pos(p->cont, (lv_coord_t)p->x, (lv_coord_t)p->y);
        }
    }
    else if (s_current_effect == ANIM_EFFECT_LEAVES) {
        for (int i = 0; i < MAX_PARTICLES; i++) {
            particle_t * p = &s_particles[i];
            if (!p->active) continue;
            p->y += p->vy;
            float sway = sinf(p->phase + (float)s_tick_counter * p->freq);
            p->x = p->base_x + p->amp * sway;

            /* Gentle aerodynamic rocking angle strictly max +-45 degrees from front view */
            int angle_tenth_deg = (int)(sway * 350.0f); /* -35.0 to +35.0 deg, strictly max 45 deg */
            if (angle_tenth_deg > 450) angle_tenth_deg = 450;
            if (angle_tenth_deg < -450) angle_tenth_deg = -450;
            lv_img_set_angle(p->img, angle_tenth_deg);

            if (p->y > DISP_VER + SUNI(20)) {
                spawn_leaf(p, 0);
            }
            lv_obj_set_pos(p->cont, (lv_coord_t)p->x, (lv_coord_t)p->y);
        }
    }
    else if (s_current_effect == ANIM_EFFECT_HEARTS) {
        for (int i = 0; i < MAX_PARTICLES; i++) {
            particle_t * p = &s_particles[i];
            if (!p->active) continue;
            p->y += p->vy;
            float sway = sinf(p->phase + (float)s_tick_counter * p->freq);
            p->x = p->base_x + p->amp * sway;

            /* Gentle floating tilt strictly max +-25 degrees from front view */
            int angle_tenth_deg = (int)(sway * 220.0f); /* -22.0 to +22.0 deg, strictly max 45 deg */
            if (angle_tenth_deg > 450) angle_tenth_deg = 450;
            if (angle_tenth_deg < -450) angle_tenth_deg = -450;
            lv_img_set_angle(p->img, angle_tenth_deg);

            if (p->y < -SUNI(50)) {
                spawn_heart(p, 0);
            }
            lv_obj_set_pos(p->cont, (lv_coord_t)p->x, (lv_coord_t)p->y);
        }
    }
    else if (s_current_effect == ANIM_EFFECT_FIREWORKS) {
        int group0_done = 1;
        int group1_done = 1;
        int half = MAX_PARTICLES / 2;

        for (int i = 0; i < MAX_PARTICLES; i++) {
            particle_t * p = &s_particles[i];
            if (!p->active) continue;

            p->x += p->vx;
            p->y += p->vy;
            p->vx *= 0.94f;
            p->vy = p->vy * 0.94f + 0.22f; /* gravity */

            p->life--;
            if (p->life <= 0) {
                p->active = 0;
                lv_obj_add_flag(p->cont, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_opa_t opa = (lv_opa_t)((p->life * 255) / p->max_life);
                lv_obj_set_style_bg_opa(p->body, opa, 0);
                lv_obj_set_pos(p->cont, (lv_coord_t)p->x, (lv_coord_t)p->y);
                if (i < half) group0_done = 0;
                else group1_done = 0;
            }
        }

        /* If a firework burst has burnt out, launch another */
        if (group0_done && (s_tick_counter % 25 == 0)) {
            spawn_firework_burst(0);
        }
        if (group1_done && ((s_tick_counter + 12) % 25 == 0)) {
            spawn_firework_burst(1);
        }
    }
}

void anim_overlay_init(void) {
    if (s_overlay) return;

    /* Transparent layer on top of all screens */
    s_overlay = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, DISP_HOR, DISP_VER);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Allocate static object pool */
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particle_t * p = &s_particles[i];
        memset(p, 0, sizeof(*p));

        p->cont = lv_obj_create(s_overlay);
        lv_obj_remove_style_all(p->cont);
        lv_obj_clear_flag(p->cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(p->cont, LV_OPA_TRANSP, 0);

        p->body = lv_obj_create(p->cont);
        lv_obj_remove_style_all(p->body);
        lv_obj_clear_flag(p->body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        p->tail = lv_obj_create(p->cont);
        lv_obj_remove_style_all(p->tail);
        lv_obj_clear_flag(p->tail, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        p->lbl = lv_label_create(p->cont);
        lv_obj_clear_flag(p->lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        p->img = lv_img_create(p->cont);
        lv_obj_clear_flag(p->img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        p->gloss = lv_obj_create(p->cont);
        lv_obj_remove_style_all(p->gloss);
        lv_obj_clear_flag(p->gloss, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_add_flag(p->cont, LV_OBJ_FLAG_HIDDEN);
    }

    s_timer = lv_timer_create(anim_timer_tick, TICK_MS, NULL);
    lv_timer_pause(s_timer);

    s_pending_timer = lv_timer_create(anim_pending_cb, 50, NULL);

    /* Check seasonal auto-triggers on boot and hourly */
    anim_overlay_check_seasonal();
    s_seasonal_timer = lv_timer_create((lv_timer_cb_t)anim_overlay_check_seasonal, 3600000, NULL);
}

static void anim_overlay_apply_stop(void) {
    s_current_effect = ANIM_EFFECT_NONE;
    s_stop_tick = 0;
    if (s_timer) lv_timer_pause(s_timer);

    for (int i = 0; i < MAX_PARTICLES; i++) {
        s_particles[i].active = 0;
        if (s_particles[i].cont) {
            lv_obj_add_flag(s_particles[i].cont, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_overlay) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void anim_overlay_apply_trigger(anim_effect_t effect, int duration_sec) {
    if (!settings.anim_enabled || effect == ANIM_EFFECT_NONE || effect >= ANIM_EFFECT_COUNT) {
        anim_overlay_apply_stop();
        return;
    }
    if (!s_overlay) anim_overlay_init();

    s_current_effect = effect;
    if (duration_sec < 0) duration_sec = settings.anim_duration_sec > 0 ? settings.anim_duration_sec : 25;
    if (duration_sec == 0) {
        s_stop_tick = 0; /* Continuous mode: runs all day (24h) until stopped or date changes */
    } else {
        s_stop_tick = lv_tick_get() + (uint32_t)duration_sec * 1000u;
    }
    s_tick_counter = 0;

    time_t now_sec = time(NULL);
    struct tm tm_now;
    if (localtime_r(&now_sec, &tm_now)) {
        s_active_day = tm_now.tm_yday;
    } else {
        s_active_day = -1;
    }

    /* Particle count limits:
     * - Balloons: reduced by a third (16 for test burst, 8 for continuous 24h mode)
     * - Continuous mode for other effects: moderate counts so screen stays clear
     * - Short burst mode: MAX_PARTICLES (24) */
    int active_limit = MAX_PARTICLES;
    if (effect == ANIM_EFFECT_BALLOONS) {
        active_limit = (s_stop_tick == 0) ? 8 : 16;
    } else if (s_stop_tick == 0) {
        if (effect == ANIM_EFFECT_HEARTS) active_limit = 12;
        else if (effect == ANIM_EFFECT_LEAVES) active_limit = 12;
        else if (effect == ANIM_EFFECT_SNOW || effect == ANIM_EFFECT_CONFETTI) active_limit = 14;
    }

    /* Initialize particles for chosen effect */
    for (int i = 0; i < active_limit; i++) {
        if (effect == ANIM_EFFECT_BALLOONS)       spawn_balloon(&s_particles[i], 1);
        else if (effect == ANIM_EFFECT_CONFETTI)   spawn_confetti(&s_particles[i], 1);
        else if (effect == ANIM_EFFECT_SNOW)       spawn_snow(&s_particles[i], 1);
        else if (effect == ANIM_EFFECT_LEAVES)     spawn_leaf(&s_particles[i], 1);
        else if (effect == ANIM_EFFECT_HEARTS)     spawn_heart(&s_particles[i], 1);
    }
    for (int i = active_limit; i < MAX_PARTICLES; i++) {
        s_particles[i].active = 0;
        if (s_particles[i].cont) lv_obj_add_flag(s_particles[i].cont, LV_OBJ_FLAG_HIDDEN);
    }

    if (effect == ANIM_EFFECT_FIREWORKS) {
        for (int i = 0; i < MAX_PARTICLES; i++) {
            s_particles[i].active = 0;
            lv_obj_add_flag(s_particles[i].cont, LV_OBJ_FLAG_HIDDEN);
        }
        spawn_firework_burst(0);
        spawn_firework_burst(1);
    }

    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(s_timer);
}

static void anim_pending_cb(lv_timer_t * t) {
    (void)t;
    if (s_pending_effect >= 0) {
        int eff = s_pending_effect;
        int dur = s_pending_duration;
        s_pending_effect = -1;
        if (eff == 0) anim_overlay_apply_stop();
        else anim_overlay_apply_trigger((anim_effect_t)eff, dur);
    }
}

void anim_overlay_trigger(anim_effect_t effect, int duration_sec) {
    s_pending_duration = duration_sec;
    s_pending_effect = (int)effect;
}

void anim_overlay_trigger_by_name(const char * name, int duration_sec) {
    anim_effect_t eff = anim_effect_from_name(name);
    anim_overlay_trigger(eff, duration_sec);
}

void anim_overlay_stop(void) {
    s_pending_effect = 0;
}

int anim_overlay_is_active(void) {
    return (s_current_effect != ANIM_EFFECT_NONE);
}

anim_effect_t anim_overlay_get_current(void) {
    return s_current_effect;
}

const char * anim_effect_name(anim_effect_t effect) {
    switch (effect) {
        case ANIM_EFFECT_BALLOONS:  return "balloons";
        case ANIM_EFFECT_CONFETTI:  return "confetti";
        case ANIM_EFFECT_SNOW:      return "snow";
        case ANIM_EFFECT_FIREWORKS: return "fireworks";
        case ANIM_EFFECT_LEAVES:    return "leaves";
        case ANIM_EFFECT_HEARTS:    return "hearts";
        default:                    return "none";
    }
}

const char * anim_effect_title(anim_effect_t effect) {
    switch (effect) {
        case ANIM_EFFECT_BALLOONS:  return tr("Ballonnen", "Balloons");
        case ANIM_EFFECT_CONFETTI:  return tr("Confetti", "Confetti");
        case ANIM_EFFECT_SNOW:      return tr("Sneeuw", "Snow");
        case ANIM_EFFECT_FIREWORKS: return tr("Vuurwerk", "Fireworks");
        case ANIM_EFFECT_LEAVES:    return tr("Herfstbladeren", "Autumn leaves");
        case ANIM_EFFECT_HEARTS:    return tr("Hartjes", "Hearts");
        default:                    return tr("Geen", "None");
    }
}

anim_effect_t anim_effect_from_name(const char * name) {
    if (!name || !name[0]) return ANIM_EFFECT_NONE;
    if (strcasecmp(name, "balloons") == 0 || strcasecmp(name, "balloon") == 0 || strcasecmp(name, "ballonnen") == 0)
        return ANIM_EFFECT_BALLOONS;
    if (strcasecmp(name, "confetti") == 0)
        return ANIM_EFFECT_CONFETTI;
    if (strcasecmp(name, "snow") == 0 || strcasecmp(name, "sneeuw") == 0)
        return ANIM_EFFECT_SNOW;
    if (strcasecmp(name, "fireworks") == 0 || strcasecmp(name, "firework") == 0 || strcasecmp(name, "vuurwerk") == 0)
        return ANIM_EFFECT_FIREWORKS;
    if (strcasecmp(name, "leaves") == 0 || strcasecmp(name, "leaf") == 0 || strcasecmp(name, "herfst") == 0)
        return ANIM_EFFECT_LEAVES;
    if (strcasecmp(name, "hearts") == 0 || strcasecmp(name, "heart") == 0 || strcasecmp(name, "hartjes") == 0)
        return ANIM_EFFECT_HEARTS;
    return ANIM_EFFECT_NONE;
}

void anim_overlay_check_seasonal(void) {
    if (!settings.anim_enabled || !settings.anim_seasonal_enabled) return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    static int s_last_seasonal_day = -1;
    if (tm.tm_yday == s_last_seasonal_day) return;

    int mon  = tm.tm_mon + 1; /* 1-12 */
    int mday = tm.tm_mday;
    int hour = tm.tm_hour;

    /* Oud & Nieuw vuurwerk: Dec 31 vanaf 18:00 uur of Jan 1 voor 06:00 uur */
    if ((mon == 12 && mday == 31 && hour >= 18) || (mon == 1 && mday == 1 && hour < 6)) {
        s_last_seasonal_day = tm.tm_yday;
        anim_overlay_trigger(ANIM_EFFECT_FIREWORKS, 35);
    }
    /* Sinterklaas: 5 december */
    else if (mon == 12 && mday == 5) {
        s_last_seasonal_day = tm.tm_yday;
        anim_overlay_trigger(ANIM_EFFECT_BALLOONS, 30);
    }
    /* Valentijnsdag: 14 februari */
    else if (mon == 2 && mday == 14) {
        s_last_seasonal_day = tm.tm_yday;
        anim_overlay_trigger(ANIM_EFFECT_HEARTS, 30);
    }
    /* Kerst / Winterdagen: 24-26 december of 1-5 januari */
    else if ((mon == 12 && mday >= 24 && mday <= 26) || (mon == 1 && mday >= 2 && mday <= 5)) {
        s_last_seasonal_day = tm.tm_yday;
        anim_overlay_trigger(ANIM_EFFECT_SNOW, 35);
    }
}
