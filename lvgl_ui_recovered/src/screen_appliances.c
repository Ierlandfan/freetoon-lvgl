/*
 * screen_appliances.c — Settings → Appliances
 *
 * Lets the user name unrecognised NILM step-change events and manage the
 * resulting custom device signature table.
 *
 * Layout (scrollable):
 *
 *   ← Appliances
 *   ─────────────────────────────────────────────
 *   RECENT UNKNOWN EVENTS
 *   [~144 W ↑  just now]   [Name]
 *   [~82 W ↓   5 min ago]  [Name]
 *   ─────────────────────────────────────────────
 *   YOUR APPLIANCES
 *   [Dishwasher  130–160 W]  [Edit] [Del]
 *   ─────────────────────────────────────────────
 *   BUILT-IN (read-only)
 *   [Bathroom light  13–28 W]
 *   ...
 *
 * Tapping "Name" on an unknown event opens a modal where the user types a
 * name.  On confirm the event's delta ± 20% is saved as a custom signature
 * and settings_save() is called.  "Edit" re-opens the same modal; "Del"
 * removes the signature after a one-tap confirm.
 */

#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "meteradapter.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

/* ── colours ──────────────────────────────────────────────────── */
#define COL_BG       0x0e1a2a
#define COL_CARD     0x1a2940
#define COL_SECTION  0x0a1220
#define COL_TEXT_HI  0xffffff
#define COL_TEXT_DIM 0x88aabb
#define COL_ACCENT   0x3a9fd4
#define COL_ON       0x36c46b
#define COL_OFF      0xd4574f
#define COL_EDIT     0x3a6ea8
#define COL_DEL      0x7a2e2e
#define COL_NAME     0x2e6e3a
#define COL_BULITIN  0x253040

/* ── built-in signature table (mirror of meteradapter.c) ─────── */
typedef struct { const char *name; float lo; float hi; } bi_sig_t;
static const bi_sig_t builtin_sigs[] = {
    { "Bathroom light",  13.0f,  28.0f },
    { "Fridge",          28.0f,  43.0f },
    { "CV boiler",       43.0f,  57.0f },
    { "TV / Decoder",    50.0f,  75.0f },
    { "Itho fan",       115.0f, 170.0f },
};
#define BUILTIN_COUNT ((int)(sizeof builtin_sigs / sizeof builtin_sigs[0]))

/* ── screen state ─────────────────────────────────────────────── */
static lv_obj_t   * scr_root     = NULL;
static lv_obj_t   * scroll_cont  = NULL;   /* scrollable content area */
static lv_obj_t   * unk_cont     = NULL;   /* unknown-events list container */
static lv_obj_t   * cust_cont    = NULL;   /* custom sigs list container */
static lv_timer_t * refresh_timer= NULL;
static int          g_last_unk_count = -1; /* to detect new unknown events */
static int          g_last_sig_count = -1;

/* ── naming modal ─────────────────────────────────────────────── */
typedef enum { MODAL_NEW, MODAL_EDIT } modal_mode_t;
static lv_obj_t  * g_modal     = NULL;
static lv_obj_t  * g_modal_ta  = NULL;
static lv_obj_t  * g_modal_kb  = NULL;
static float       g_modal_lo  = 0;
static float       g_modal_hi  = 0;
static int         g_modal_edit_idx = -1;  /* ≥0 when editing existing custom sig */
static modal_mode_t g_modal_mode = MODAL_NEW;

/* forward declarations */
static void rebuild_unknown_list(void);
static void rebuild_custom_list(void);

/* ── helpers ──────────────────────────────────────────────────── */
static void age_str(time_t ts, char *buf, int sz) {
    int diff = (int)(time(NULL) - ts);
    if (diff < 5)        snprintf(buf, sz, tr("zojuist",    "just now"));
    else if (diff < 60)  snprintf(buf, sz, tr("%d s geleden", "%d s ago"),  diff);
    else if (diff < 3600)snprintf(buf, sz, tr("%d min geleden", "%d min ago"), diff / 60);
    else                 snprintf(buf, sz, tr("%d u geleden",  "%d h ago"),   diff / 3600);
}

/* Make a horizontal row inside a container */
static lv_obj_t * mk_row(lv_obj_t *parent, int h) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, LV_PCT(100), SY(h));
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_pad_all(r, SY(8), 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static lv_obj_t * mk_section_label(lv_obj_t *parent, const char *txt) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(l, SF(14), 0);
    lv_obj_set_style_pad_top(l, SY(10), 0);
    lv_obj_set_style_pad_bottom(l, SY(4), 0);
    lv_label_set_text(l, txt);
    lv_obj_set_size(l, LV_PCT(100), LV_SIZE_CONTENT);
    return l;
}

static lv_obj_t * mk_text(lv_obj_t *parent, const char *txt, int font_px,
                           uint32_t col, lv_align_t align, int ox, int oy) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(l, SF(font_px), 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, align, ox, oy);
    return l;
}

static lv_obj_t * mk_btn(lv_obj_t *parent, const char *txt, uint32_t col,
                          lv_event_cb_t cb, void *udata, lv_align_t align, int ox, int oy) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, SX(110), SY(44));
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_align(b, align, ox, oy);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, udata);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, SF(16), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

/* ── naming modal ─────────────────────────────────────────────── */
static void modal_close(void) {
    if (g_modal_kb) { lv_obj_del(g_modal_kb); g_modal_kb = NULL; }
    if (g_modal)    { lv_obj_del(g_modal);    g_modal    = NULL; }
    g_modal_ta = NULL;
}

static void on_modal_cancel(lv_event_t *e) { (void)e; modal_close(); }

static void on_modal_ok(lv_event_t *e) {
    (void)e;
    if (!g_modal_ta) { modal_close(); return; }
    const char *name = lv_textarea_get_text(g_modal_ta);
    if (!name || name[0] == '\0') { modal_close(); return; }

    if (g_modal_mode == MODAL_EDIT && g_modal_edit_idx >= 0) {
        /* Update existing */
        int i = g_modal_edit_idx;
        snprintf(settings.nilm_sig_name[i], 40, "%s", name);
        settings.nilm_sig_lo[i] = g_modal_lo;
        settings.nilm_sig_hi[i] = g_modal_hi;
    } else {
        /* Add new */
        if (settings.nilm_sig_count < NILM_CUSTOM_MAX) {
            int i = settings.nilm_sig_count;
            snprintf(settings.nilm_sig_name[i], 40, "%s", name);
            settings.nilm_sig_lo[i] = g_modal_lo;
            settings.nilm_sig_hi[i] = g_modal_hi;
            settings.nilm_sig_count++;
        }
    }
    settings_save();
    modal_close();
    rebuild_custom_list();
}

static void on_kb_event(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY)  on_modal_ok(e);
    if (c == LV_EVENT_CANCEL) modal_close();
}

static void open_naming_modal(const char *prefill, float lo, float hi,
                               modal_mode_t mode, int edit_idx) {
    if (g_modal) modal_close();
    g_modal_lo       = lo;
    g_modal_hi       = hi;
    g_modal_mode     = mode;
    g_modal_edit_idx = edit_idx;

    /* Full-screen dim overlay */
    g_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_modal, DISP_HOR, DISP_VER);
    lv_obj_set_pos(g_modal, 0, 0);
    lv_obj_set_style_bg_color(g_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_modal, 0, 0);
    lv_obj_clear_flag(g_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Card */
    lv_obj_t *card = lv_obj_create(g_modal);
    lv_obj_set_size(card, SX(680), SY(230));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, SY(20));
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, SY(16), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(20), 0);
    char heading[80];
    snprintf(heading, sizeof heading,
             tr("Apparaatnaam  (%.0f - %.0f W)", "Appliance name  (%.0f - %.0f W)"),
             (double)lo, (double)hi);
    lv_label_set_text(title, heading);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Textarea */
    g_modal_ta = lv_textarea_create(card);
    lv_obj_set_size(g_modal_ta, SX(620), SY(56));
    lv_obj_align(g_modal_ta, LV_ALIGN_TOP_LEFT, 0, SY(38));
    lv_obj_set_style_text_font(g_modal_ta, SF(20), 0);
    lv_obj_set_style_bg_color(g_modal_ta, lv_color_hex(0x0a1020), 0);
    lv_obj_set_style_text_color(g_modal_ta, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_border_color(g_modal_ta, lv_color_hex(COL_ACCENT), 0);
    lv_textarea_set_max_length(g_modal_ta, 39);
    lv_textarea_set_one_line(g_modal_ta, true);
    if (prefill && prefill[0])
        lv_textarea_set_text(g_modal_ta, prefill);

    /* Buttons */
    mk_btn(card, tr("OK", "OK"), COL_NAME,
           on_modal_ok, NULL, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    mk_btn(card, tr("Annuleer", "Cancel"), COL_DEL,
           on_modal_cancel, NULL, LV_ALIGN_BOTTOM_RIGHT, -SX(120), 0);

    /* Keyboard */
    g_modal_kb = lv_keyboard_create(g_modal);
    lv_obj_set_size(g_modal_kb, DISP_HOR, SY(280));
    lv_obj_align(g_modal_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(g_modal_kb, g_modal_ta);
    lv_obj_add_event_cb(g_modal_kb, on_kb_event, LV_EVENT_ALL, NULL);
}

/* ── unknown event "Name" tap ─────────────────────────────────── */
typedef struct { float delta_w; int direction; } unk_ctx_t;
static void on_name_unknown(lv_event_t *e) {
    unk_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    float margin = ctx->delta_w * 0.20f;
    float lo = ctx->delta_w - margin;
    float hi = ctx->delta_w + margin;
    if (lo < 1.0f) lo = 1.0f;
    open_naming_modal("", lo, hi, MODAL_NEW, -1);
}

/* ── custom sig "Edit" and "Del" taps ─────────────────────────── */
static void on_edit_sig(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= settings.nilm_sig_count) return;
    open_naming_modal(settings.nilm_sig_name[idx],
                      settings.nilm_sig_lo[idx],
                      settings.nilm_sig_hi[idx],
                      MODAL_EDIT, idx);
}

static lv_obj_t *g_del_confirm = NULL;
static int        g_del_idx     = -1;

static void del_confirm_close(void) {
    if (g_del_confirm) { lv_obj_del(g_del_confirm); g_del_confirm = NULL; }
}
static void on_del_confirm_cancel(lv_event_t *e) { (void)e; del_confirm_close(); }
static void on_del_confirm_ok(lv_event_t *e) {
    (void)e;
    del_confirm_close();
    int idx = g_del_idx;
    if (idx < 0 || idx >= settings.nilm_sig_count) return;
    /* Shift array left */
    for (int i = idx; i < settings.nilm_sig_count - 1; i++) {
        memcpy(settings.nilm_sig_name[i], settings.nilm_sig_name[i + 1], 40);
        settings.nilm_sig_lo[i] = settings.nilm_sig_lo[i + 1];
        settings.nilm_sig_hi[i] = settings.nilm_sig_hi[i + 1];
    }
    settings.nilm_sig_count--;
    settings_save();
    rebuild_custom_list();
}

static void on_del_sig(lv_event_t *e) {
    g_del_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_del_idx < 0 || g_del_idx >= settings.nilm_sig_count) return;

    g_del_confirm = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_del_confirm, DISP_HOR, DISP_VER);
    lv_obj_set_pos(g_del_confirm, 0, 0);
    lv_obj_set_style_bg_color(g_del_confirm, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_del_confirm, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_del_confirm, 0, 0);
    lv_obj_clear_flag(g_del_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(g_del_confirm);
    lv_obj_set_size(card, SX(500), SY(180));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, SY(16), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    char msg[80];
    snprintf(msg, sizeof msg,
             tr("Verwijder \"%s\"?", "Delete \"%s\"?"),
             settings.nilm_sig_name[g_del_idx]);
    lv_obj_t *t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(t, SF(20), 0);
    lv_label_set_text(t, msg);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    mk_btn(card, tr("Verwijder", "Delete"), COL_DEL,
           on_del_confirm_ok, NULL, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    mk_btn(card, tr("Annuleer", "Cancel"), COL_EDIT,
           on_del_confirm_cancel, NULL, LV_ALIGN_BOTTOM_RIGHT, -SX(120), 0);
}

/* ── list builders ────────────────────────────────────────────── */
static void rebuild_unknown_list(void) {
    if (!unk_cont) return;
    lv_obj_clean(unk_cont);
    g_last_unk_count = nilm_unknown_count;

    /* Collect up to NILM_UNKNOWN_MAX slots, newest first */
    int total = nilm_unknown_count < NILM_UNKNOWN_MAX
                ? nilm_unknown_count : NILM_UNKNOWN_MAX;

    if (total == 0) {
        lv_obj_t *l = lv_label_create(unk_cont);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(l, SF(16), 0);
        lv_label_set_text(l, meter_state.connected
            ? tr("Geen onbekende stappen gedetecteerd.", "No unknown step-changes detected.")
            : tr("P1 meter offline.", "P1 meter offline."));
        return;
    }

    /* Walk ring backwards from newest slot */
    int head_slot = (nilm_unknown_count - 1) % NILM_UNKNOWN_MAX;
    for (int i = 0; i < total; i++) {
        int slot = (head_slot - i + NILM_UNKNOWN_MAX) % NILM_UNKNOWN_MAX;
        nilm_unknown_t *u = &nilm_unknowns[slot];
        if (u->ts == 0) continue;

        lv_obj_t *row = mk_row(unk_cont, 56);

        /* Watt label */
        char wbuf[32], agebuf[32];
        snprintf(wbuf, sizeof wbuf, "%s%.0f W",
                 u->direction > 0 ? "+" : "-", (double)u->delta_w);
        age_str(u->ts, agebuf, sizeof agebuf);
        char full[80];
        snprintf(full, sizeof full, "%-16s  %s", wbuf, agebuf);
        mk_text(row, full, 18, COL_TEXT_HI, LV_ALIGN_LEFT_MID, SX(8), 0);

        /* "Name" button — carry delta + direction as context */
        unk_ctx_t *ctx = malloc(sizeof *ctx);
        if (ctx) {
            ctx->delta_w  = u->delta_w;
            ctx->direction = u->direction;
            lv_obj_t *btn = mk_btn(row,
                tr("Naam geven", "Name"),
                COL_NAME, on_name_unknown, ctx,
                LV_ALIGN_RIGHT_MID, -SX(4), 0);
            /* free ctx when the button is deleted (row lifetime) */
            lv_obj_add_event_cb(btn, (lv_event_cb_t)(void(*)(lv_event_t*))free,
                                LV_EVENT_DELETE, ctx);
        }
    }
}

static void rebuild_custom_list(void) {
    if (!cust_cont) return;
    lv_obj_clean(cust_cont);
    g_last_sig_count = settings.nilm_sig_count;

    if (settings.nilm_sig_count == 0) {
        lv_obj_t *l = lv_label_create(cust_cont);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(l, SF(16), 0);
        lv_label_set_text(l,
            tr("Nog geen apparaten benoemd.", "No custom appliances yet."));
        return;
    }

    for (int i = 0; i < settings.nilm_sig_count; i++) {
        lv_obj_t *row = mk_row(cust_cont, 56);

        char txt[80];
        snprintf(txt, sizeof txt, "%s   %.0f - %.0f W",
                 settings.nilm_sig_name[i],
                 (double)settings.nilm_sig_lo[i],
                 (double)settings.nilm_sig_hi[i]);
        mk_text(row, txt, 18, COL_TEXT_HI, LV_ALIGN_LEFT_MID, SX(8), 0);

        mk_btn(row, tr("Bewerk", "Edit"), COL_EDIT,
               on_edit_sig, (void *)(intptr_t)i,
               LV_ALIGN_RIGHT_MID, -SX(4), 0);
        mk_btn(row, tr("Wis", "Del"), COL_DEL,
               on_del_sig, (void *)(intptr_t)i,
               LV_ALIGN_RIGHT_MID, -SX(124), 0);
    }
}

/* ── refresh timer ────────────────────────────────────────────── */
static void refresh_cb(lv_timer_t *t) {
    (void)t;
    /* Rebuild lists only when data has changed */
    if (nilm_unknown_count != g_last_unk_count)
        rebuild_unknown_list();
    if (settings.nilm_sig_count != g_last_sig_count)
        rebuild_custom_list();
}

/* ── screen event ─────────────────────────────────────────────── */
static void on_scr_event(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        if (refresh_timer) lv_timer_resume(refresh_timer);
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
        modal_close();
        del_confirm_close();
    }
}

static void back_async(void *u) { (void)u; ui_pop(); }
static void on_back(lv_event_t *e) { (void)e; lv_async_call(back_async, NULL); }

/* ── screen builder ───────────────────────────────────────────── */
lv_obj_t * screen_appliances_create(void) {
    if (scr_root) {
        /* Reuse: refresh data before returning */
        g_last_unk_count = -1;
        g_last_sig_count = -1;
        rebuild_unknown_list();
        rebuild_custom_list();
        return scr_root;
    }

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_ALL, NULL);

    /* ── Back button ── */
    lv_obj_t *back_btn = lv_btn_create(scr_root);
    lv_obj_set_size(back_btn, SX(100), SY(48));
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, SX(8), SY(8));
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x1e3050), 0);
    lv_obj_set_style_radius(back_btn, 10, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(back_lbl, SF(20), 0);
    lv_label_set_text_fmt(back_lbl, LV_SYMBOL_LEFT " %s", tr("Terug", "Back"));
    lv_obj_center(back_lbl);

    /* ── Title ── */
    lv_obj_t *title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(24), 0);
    lv_label_set_text(title, tr("Apparaten", "Appliances"));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, SY(16));

    /* ── Scrollable content ── */
    int content_y = SY(68);
    scroll_cont = lv_obj_create(scr_root);
    lv_obj_set_size(scroll_cont, DISP_HOR - SX(16), DISP_VER - content_y - SY(4));
    lv_obj_align(scroll_cont, LV_ALIGN_TOP_LEFT, SX(8), content_y);
    lv_obj_set_style_bg_color(scroll_cont, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(scroll_cont, 0, 0);
    lv_obj_set_style_pad_all(scroll_cont, 0, 0);
    lv_obj_set_style_pad_row(scroll_cont, SY(6), 0);
    lv_obj_set_flex_flow(scroll_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_VER);

    int list_w = DISP_HOR - SX(32);

    /* ─── SECTION: Recent unknowns ─── */
    mk_section_label(scroll_cont,
        tr("RECENT ONBEKEND", "RECENT UNKNOWNS"));

    unk_cont = lv_obj_create(scroll_cont);
    lv_obj_set_size(unk_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(unk_cont, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(unk_cont, 0, 0);
    lv_obj_set_style_pad_all(unk_cont, 0, 0);
    lv_obj_set_style_pad_row(unk_cont, SY(4), 0);
    lv_obj_set_flex_flow(unk_cont, LV_FLEX_FLOW_COLUMN);

    rebuild_unknown_list();

    /* ─── SECTION: Custom appliances ─── */
    mk_section_label(scroll_cont,
        tr("UW APPARATEN", "YOUR APPLIANCES"));

    cust_cont = lv_obj_create(scroll_cont);
    lv_obj_set_size(cust_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(cust_cont, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(cust_cont, 0, 0);
    lv_obj_set_style_pad_all(cust_cont, 0, 0);
    lv_obj_set_style_pad_row(cust_cont, SY(4), 0);
    lv_obj_set_flex_flow(cust_cont, LV_FLEX_FLOW_COLUMN);

    rebuild_custom_list();

    /* ─── SECTION: Built-in (read-only) ─── */
    mk_section_label(scroll_cont,
        tr("INGEBOUWD (alleen-lezen)", "BUILT-IN (read-only)"));

    for (int i = 0; i < BUILTIN_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(scroll_cont);
        lv_obj_set_size(row, LV_PCT(100), SY(48));
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_BULITIN), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, SY(8), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char txt[80];
        snprintf(txt, sizeof txt, "%s   %.0f - %.0f W",
                 builtin_sigs[i].name,
                 (double)builtin_sigs[i].lo,
                 (double)builtin_sigs[i].hi);
        lv_obj_t *l = lv_label_create(row);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(l, SF(16), 0);
        lv_label_set_text(l, txt);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, SX(8), 0);
    }

    /* ── Refresh timer ── */
    if (!refresh_timer)
        refresh_timer = lv_timer_create(refresh_cb, 2000, NULL);
    lv_timer_pause(refresh_timer);   /* starts on SCREEN_LOADED */

    return scr_root;
}
