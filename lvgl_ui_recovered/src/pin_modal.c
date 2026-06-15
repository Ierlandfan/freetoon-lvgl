/*
 * pin_modal.c — see header for design notes.
 *
 * Uses lv_keyboard in LV_KEYBOARD_MODE_NUMBER instead of hand-laying
 * out a numeric keypad — saves code, matches the touch targets the
 * stock Settings textareas already use, and works at both 800×480 and
 * 1024×600 without a per-target layout.
 */
#include "lvgl/lvgl.h"
#include "pin_modal.h"
#include "settings.h"
#include "display.h"
#include "i18n.h"

#include <string.h>
#include <stdio.h>

static lv_obj_t *  g_backdrop = NULL;
static lv_obj_t *  g_ta       = NULL;
static lv_obj_t *  g_err_lbl  = NULL;
static pin_action_cb g_action = NULL;
static void *      g_ctx      = NULL;
static int         g_attempts = 0;

#define MAX_ATTEMPTS 3

int pin_is_armed(void) {
    return settings.pin_enabled && settings.pin_code[0];
}

static void close_modal(void) {
    if (g_backdrop) {
        lv_obj_del(g_backdrop);
        g_backdrop = NULL;
        g_ta = NULL;
        g_err_lbl = NULL;
    }
    g_action = NULL;
    g_ctx = NULL;
    g_attempts = 0;
}

static void on_cancel(lv_event_t * e) {
    (void)e;
    close_modal();
}

static void on_ok(lv_event_t * e) {
    (void)e;
    const char * entered = lv_textarea_get_text(g_ta);
    if (strcmp(entered, settings.pin_code) == 0) {
        pin_action_cb act = g_action;
        void *        ctx = g_ctx;
        close_modal();
        if (act) act(ctx);
        return;
    }
    /* Wrong PIN — clear field, count, give up after MAX_ATTEMPTS without
     * locking the device (that would brick the UI for the homeowner if
     * a kid mashed buttons). Just silently close after 3 misses. */
    g_attempts++;
    lv_textarea_set_text(g_ta, "");
    if (g_attempts >= MAX_ATTEMPTS) {
        close_modal();
        return;
    }
    if (g_err_lbl) {
        lv_label_set_text_fmt(g_err_lbl, tr("Onjuist — nog %d pogingen",
                                            "Incorrect — %d tries left"),
                              MAX_ATTEMPTS - g_attempts);
        lv_obj_set_style_text_color(g_err_lbl, lv_color_hex(0xff6060), 0);
    }
}

static void on_kb_event(lv_event_t * e) {
    /* lv_keyboard fires its own LV_EVENT_READY when the kb's ✓ key is
     * tapped. We hook it as our "OK". The kb default handler also writes
     * into the textarea for digit/back presses, so we don't override
     * LV_EVENT_VALUE_CHANGED. */
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)  on_ok(e);
    if (code == LV_EVENT_CANCEL) on_cancel(e);
}

void pin_gate(pin_action_cb action, void * ctx) {
    if (!pin_is_armed()) {
        /* Fast path — no gate. */
        if (action) action(ctx);
        return;
    }
    /* If a modal is already up (re-entrant tap on something else), close
     * it and re-open with the new action. Avoids stale-action surprises. */
    if (g_backdrop) close_modal();

    g_action = action;
    g_ctx    = ctx;
    g_attempts = 0;

    /* Full-screen 70% black backdrop like the Settings modals. */
    g_backdrop = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(g_backdrop);
    lv_obj_set_size(g_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_backdrop, LV_OPA_70, 0);
    lv_obj_add_flag(g_backdrop, LV_OBJ_FLAG_CLICKABLE);  /* eat taps */
    lv_obj_clear_flag(g_backdrop, LV_OBJ_FLAG_SCROLLABLE);

    /* Centred panel sized to fit a kb + ta + small header on both
     * 800x480 and 1024x600. Keyboard wants ≥220 px high to be tappable;
     * leave 120 px for the title + textarea + status line. */
    int panel_w = (DISP_HOR < 800) ? DISP_HOR - 40 : 480;
    int panel_h = (DISP_VER < 600) ? DISP_VER - 40 : 380;
    lv_obj_t * panel = lv_obj_create(g_backdrop);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x404040), 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(panel);
    lv_label_set_text(title, tr("Voer PIN in", "Enter PIN"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, SF(22), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    g_ta = lv_textarea_create(panel);
    lv_textarea_set_one_line(g_ta, true);
    lv_textarea_set_password_mode(g_ta, true);
    lv_textarea_set_max_length(g_ta, sizeof settings.pin_code - 1);
    lv_textarea_set_placeholder_text(g_ta, "••••");
    lv_obj_set_width(g_ta, panel_w - 80);
    lv_obj_align(g_ta, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_text_font(g_ta, SF(22), 0);
    lv_obj_set_style_text_align(g_ta, LV_TEXT_ALIGN_CENTER, 0);

    g_err_lbl = lv_label_create(panel);
    lv_label_set_text(g_err_lbl, "");
    lv_obj_set_style_text_color(g_err_lbl, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(g_err_lbl, SF(14), 0);
    lv_obj_align_to(g_err_lbl, g_ta, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    /* Numeric keyboard fills the rest of the panel. Built-in ✓ = OK,
     * ✗ = cancel — wired via LV_EVENT_READY / LV_EVENT_CANCEL. */
    lv_obj_t * kb = lv_keyboard_create(panel);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, g_ta);
    int kb_h = panel_h - 130;
    if (kb_h < 180) kb_h = 180;
    lv_obj_set_size(kb, panel_w - 24, kb_h);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_CANCEL, NULL);

    /* Defensive: if the surrounding screen also has key bindings, our
     * backdrop swallows clicks but not key events. The textarea is
     * focused-by-default so the kb-ta wiring is enough for touch UX. */
    lv_obj_add_state(g_ta, LV_STATE_FOCUSED);
}
