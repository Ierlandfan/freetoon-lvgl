/* See doorbell.h. The snapshot JPEG is fetched by homeassistant.c into
 * DOORBELL_SNAP_PATH; here we render it on lv_layer_top() so it appears over
 * any screen, and tear it down on tap or after a timeout.
 *
 * HA / Frigate camera snapshots are full-resolution JPEGs (e.g. 1920x1080).
 * LVGL's built-in SJPG decoder only decodes at 1:1, which blows the frame
 * buffer for a frame that size, so we decode it ourselves with the bundled
 * TJpgDec at a scale that fits the screen (bounded memory) into an RGB buffer
 * and blit it via lv_canvas.
 *
 * While the overlay is open we set ha_state.doorbell_live so the poll thread
 * re-fetches the snapshot ~1x/s; each new frame is re-decoded and swapped into
 * the canvas, giving near-live footage (true H.264 isn't available on this
 * stack — this is the MJPEG-style "rapid still" approach Frigate feeds well). */
#include "lvgl/lvgl.h"
#include "lvgl/src/extra/libs/sjpg/tjpgd.h"
#include "doorbell.h"
#include "homeassistant.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>

static int          last_seq = 0;
static int          last_frame = 0;
static lv_obj_t *   overlay = NULL;
static lv_obj_t *   canvas = NULL;
static lv_timer_t * dismiss_timer = NULL;
static lv_color_t * cur_px = NULL;   /* buffer currently shown by the canvas */

/* ---- TJpgDec glue: decode DOORBELL_SNAP_PATH at a screen-fitting scale ---- */
typedef struct { FILE * fp; lv_color_t * out; int ow; int oh; } jdev_t;

static size_t jd_in(JDEC * jd, uint8_t * buf, size_t n) {
    jdev_t * d = (jdev_t *)jd->device;
    if (buf) return fread(buf, 1, n, d->fp);
    return fseek(d->fp, (long)n, SEEK_CUR) == 0 ? n : 0;
}

static int jd_out(JDEC * jd, void * bitmap, JRECT * r) {
    jdev_t * d = (jdev_t *)jd->device;
    const uint8_t * src = (const uint8_t *)bitmap;   /* RGB888 over the rect */
    for (int y = r->top; y <= r->bottom; y++) {
        for (int x = r->left; x <= r->right; x++) {
            if (x < d->ow && y < d->oh)
                d->out[y * d->ow + x] = lv_color_make(src[0], src[1], src[2]);
            src += 3;
        }
    }
    return 1;
}

/* Decode the saved JPEG into a freshly-allocated RGB buffer. Returns the
 * buffer (caller frees with lv_mem_free) and fills *ow/*oh, or NULL on error. */
static lv_color_t * decode_snapshot(int * ow, int * oh) {
    static uint8_t work[4096];
    JDEC jd;
    jdev_t dev = { 0 };
    dev.fp = fopen(DOORBELL_SNAP_PATH, "rb");
    if (!dev.fp) return NULL;
    if (jd_prepare(&jd, jd_in, work, sizeof(work), &dev) != JDR_OK) { fclose(dev.fp); return NULL; }
    lv_coord_t sw = lv_disp_get_hor_res(NULL), sh = lv_disp_get_ver_res(NULL);
    uint8_t scale = 0;
    while (scale < 3 && ((jd.width >> scale) > sw || (jd.height >> scale) > sh))
        scale++;
    dev.ow = jd.width >> scale;
    dev.oh = jd.height >> scale;
    if (dev.ow <= 0 || dev.oh <= 0) { fclose(dev.fp); return NULL; }
    dev.out = lv_mem_alloc((size_t)dev.ow * dev.oh * sizeof(lv_color_t));
    if (!dev.out) { fclose(dev.fp); return NULL; }
    JRESULT rc = jd_decomp(&jd, jd_out, scale);
    fclose(dev.fp);
    if (rc != JDR_OK) { lv_mem_free(dev.out); return NULL; }
    *ow = dev.ow; *oh = dev.oh;
    return dev.out;
}

static void close_overlay(void) {
    ha_state.doorbell_live = 0;        /* stop the poll-thread fast refresh */
    if (dismiss_timer) { lv_timer_del(dismiss_timer); dismiss_timer = NULL; }
    if (overlay)       { lv_obj_del(overlay); overlay = NULL; canvas = NULL; }
    if (cur_px)        { lv_mem_free(cur_px); cur_px = NULL; }
}

static void on_tap(lv_event_t * e)     { (void)e; close_overlay(); }
static void on_dismiss(lv_timer_t * t) { (void)t; dismiss_timer = NULL; close_overlay(); }

/* Swap in a freshly-decoded frame without rebuilding the overlay (no flicker). */
static void update_frame(void) {
    if (!canvas) return;
    int w, h;
    lv_color_t * px = decode_snapshot(&w, &h);
    if (!px) return;
    lv_canvas_set_buffer(canvas, px, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(canvas);
    if (cur_px) lv_mem_free(cur_px);
    cur_px = px;
    lv_obj_invalidate(canvas);
}

static void show_overlay(void) {
    close_overlay();

    overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, on_tap, LV_EVENT_CLICKED, NULL);

    int w, h;
    cur_px = decode_snapshot(&w, &h);
    if (cur_px) {
        canvas = lv_canvas_create(overlay);
        lv_canvas_set_buffer(canvas, cur_px, w, h, LV_IMG_CF_TRUE_COLOR);
        lv_obj_center(canvas);
    } else {
        lv_obj_t * err = lv_label_create(overlay);
        lv_obj_set_style_text_color(err, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(err, &lv_font_montserrat_22, 0);
        lv_label_set_text(err, "Doorbell: snapshot unavailable");
        lv_obj_center(err);
    }

    lv_obj_t * lbl = lv_label_create(overlay);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_bg_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(lbl, 8, 0);
    lv_obj_set_style_radius(lbl, 8, 0);
    lv_label_set_text(lbl, LV_SYMBOL_BELL "  Doorbell - tap to dismiss");
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 12);

    /* Ask the poll thread to stream fresh frames while we're up. */
    last_frame = ha_state.doorbell_frame;
    ha_state.doorbell_live = 1;

    int secs = settings.doorbell_seconds > 0 ? settings.doorbell_seconds : 30;
    dismiss_timer = lv_timer_create(on_dismiss, secs * 1000, NULL);
    lv_timer_set_repeat_count(dismiss_timer, 1);
}

static void watch_cb(lv_timer_t * t) {
    (void)t;
    if (ha_state.doorbell_seq != last_seq) {
        last_seq = ha_state.doorbell_seq;
        show_overlay();
    } else if (overlay && ha_state.doorbell_frame != last_frame) {
        last_frame = ha_state.doorbell_frame;
        update_frame();   /* new live frame arrived */
    }
}

void doorbell_ui_init(void) {
    last_seq = ha_state.doorbell_seq;   /* don't fire for events before boot */
    /* 200 ms → up to ~5 fps on-screen when streaming live MJPEG frames; when
     * idle this is just an int compare, so it's cheap. */
    lv_timer_create(watch_cb, 200, NULL);
}
