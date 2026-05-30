/*
 * screen_crypto_picker.c — live-search picker for the Crypto integration's
 * tracked coins. Opened from Settings. Searches the provider's coins.tsv cache
 * (~17k CoinGecko coins, id<TAB>symbol<TAB>name), lets the user pick up to 5
 * (the first = the home-tile primary), writes the COIN= lines into the
 * provider's crypto.conf and reloads the daemon.
 *
 * Persistence lives entirely in crypto.conf (the provider's own file), NOT
 * toonui.cfg — so it never touches the settings.c loader that truncated the
 * news feeds.
 */
#include "screens.h"
#include "display.h"   /* SX()/SY() scaling for Toon 1 (800x480) vs Toon 2 (1024x600) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define COINS_TSV   "/mnt/data/integrations/crypto/coins.tsv"
#define CRYPTO_CONF "/mnt/data/integrations/crypto/crypto.conf"
#define MAX_PICK    5
#define MAX_RESULTS 40       /* cap the visible result list — searching narrows it */

static lv_obj_t * scr_root;
static lv_obj_t * ta_search;
static lv_obj_t * lst_results;
static lv_obj_t * lbl_picked;
static lv_obj_t * kb;

/* Selected coins, in order (first = tile primary). */
static char sel_id[MAX_PICK][64];
static char sel_sym[MAX_PICK][24];
static int  sel_n = 0;

static int sel_index(const char * id) {
    for (int i = 0; i < sel_n; i++) if (strcmp(sel_id[i], id) == 0) return i;
    return -1;
}

static void update_picked_label(void) {
    char buf[256]; int o = 0;
    o += snprintf(buf + o, sizeof buf - o, "Gekozen %d/%d: ", sel_n, MAX_PICK);
    for (int i = 0; i < sel_n && o < (int)sizeof buf - 8; i++)
        o += snprintf(buf + o, sizeof buf - o, "%s%s%s",
                      i ? ", " : "", sel_sym[i], i == 0 ? " (tegel)" : "");
    if (sel_n == 0) snprintf(buf, sizeof buf, "Gekozen 0/%d — tik op een munt", MAX_PICK);
    lv_label_set_text(lbl_picked, buf);
}

/* lower-case copy for case-insensitive matching */
static void lc(const char * s, char * out, size_t osz) {
    size_t i = 0; for (; s[i] && i < osz - 1; i++) out[i] = (char)tolower((unsigned char)s[i]); out[i] = 0;
}

static void on_pick(lv_event_t * e);

/* Re-filter coins.tsv against the search box and rebuild the result list. */
static void refilter(void) {
    lv_obj_clean(lst_results);
    const char * q = lv_textarea_get_text(ta_search);
    char ql[64]; lc(q, ql, sizeof ql);
    if (!ql[0]) {
        lv_obj_t * b = lv_list_add_text(lst_results, "Typ om te zoeken (naam of symbool)…");
        (void)b; return;
    }
    FILE * f = fopen(COINS_TSV, "r");
    if (!f) { lv_list_add_text(lst_results, "coins.tsv ontbreekt — crypto-integratie nog niet geinstalleerd?"); return; }
    char line[256]; int shown = 0;
    while (fgets(line, sizeof line, f) && shown < MAX_RESULTS) {
        char * nl = strchr(line, '\n'); if (nl) *nl = 0;
        char * t1 = strchr(line, '\t'); if (!t1) continue; *t1 = 0;
        char * id = line; char * sym = t1 + 1;
        char * t2 = strchr(sym, '\t'); char * name = sym;
        if (t2) { *t2 = 0; name = t2 + 1; }
        char hay[256]; char idl[64], syml[24], naml[160];
        lc(id, idl, sizeof idl); lc(sym, syml, sizeof syml); lc(name, naml, sizeof naml);
        snprintf(hay, sizeof hay, "%s %s %s", idl, syml, naml);
        if (!strstr(hay, ql)) continue;
        char label[128];
        snprintf(label, sizeof label, "%s%s  (%s)%s",
                 sel_index(id) >= 0 ? LV_SYMBOL_OK " " : "", name, sym,
                 sel_index(id) >= 0 ? "" : "");
        lv_obj_t * btn = lv_list_add_btn(lst_results, NULL, label);
        /* stash the id+sym on the button via user_data (id\tsym, heap copy) */
        char * ud = malloc(96);
        snprintf(ud, 96, "%s\t%s", id, sym);
        lv_obj_set_user_data(btn, ud);
        lv_obj_add_event_cb(btn, on_pick, LV_EVENT_CLICKED, NULL);
        shown++;
    }
    fclose(f);
    if (shown == 0) lv_list_add_text(lst_results, "Geen munten gevonden");
    else if (shown >= MAX_RESULTS) lv_list_add_text(lst_results, "… verfijn de zoekopdracht voor meer");
}

static void on_pick(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    char * ud = (char *)lv_obj_get_user_data(btn);
    if (!ud) return;
    char id[64] = "", sym[24] = "";
    char * tab = strchr(ud, '\t');
    if (tab) { *tab = 0; snprintf(id, sizeof id, "%s", ud); snprintf(sym, sizeof sym, "%s", tab + 1); *tab = '\t'; }
    int at = sel_index(id);
    if (at >= 0) {                          /* already picked -> remove */
        for (int i = at; i < sel_n - 1; i++) {
            strcpy(sel_id[i], sel_id[i + 1]); strcpy(sel_sym[i], sel_sym[i + 1]);
        }
        sel_n--;
    } else if (sel_n < MAX_PICK) {          /* add */
        snprintf(sel_id[sel_n], sizeof sel_id[0], "%s", id);
        snprintf(sel_sym[sel_n], sizeof sel_sym[0], "%s", sym[0] ? sym : id);
        sel_n++;
    }
    update_picked_label();
    refilter();                             /* redraw to show/clear the check mark */
}

static void on_search_changed(lv_event_t * e) { (void)e; refilter(); }

static void on_search_focus(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_FOCUSED) { lv_keyboard_set_textarea(kb, ta_search); lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN); }
    else if (c == LV_EVENT_DEFOCUSED) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

/* Read the current COIN= ids from crypto.conf so the picker opens pre-populated. */
static void load_current(void) {
    sel_n = 0;
    FILE * f = fopen(CRYPTO_CONF, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && sel_n < MAX_PICK) {
        if (strncmp(line, "COIN=", 5) != 0) continue;
        char * v = line + 5;
        char * c1 = strchr(v, ','); if (c1) *c1 = 0;
        char id[64]; snprintf(id, sizeof id, "%s", v);
        char * nl = strchr(id, '\n'); if (nl) *nl = 0;
        char sym[24] = "";
        if (c1) { char * c2 = strchr(c1 + 1, ','); if (c2) *c2 = 0; snprintf(sym, sizeof sym, "%s", c1 + 1); }
        if (id[0]) {
            snprintf(sel_id[sel_n], sizeof sel_id[0], "%s", id);
            snprintf(sel_sym[sel_n], sizeof sel_sym[0], "%s", sym[0] ? sym : id);
            sel_n++;
        }
    }
    fclose(f);
}

/* Write the chosen coins to crypto.conf (preserving VS=/KEY= and any non-COIN
 * lines) and reload the provider daemon so it re-subscribes / re-fetches. */
static void save_and_reload(void) {
    /* slurp existing conf, keep every non-COIN line */
    char keep[2048]; keep[0] = 0;
    FILE * in = fopen(CRYPTO_CONF, "r");
    int have_vs = 0;
    if (in) {
        char line[256];
        while (fgets(line, sizeof line, in)) {
            if (strncmp(line, "COIN=", 5) == 0) continue;          /* drop old COINs */
            if (strncmp(line, "VS=", 3) == 0) have_vs = 1;
            strncat(keep, line, sizeof keep - strlen(keep) - 1);
        }
        fclose(in);
    }
    FILE * out = fopen(CRYPTO_CONF, "w");
    if (!out) return;
    if (!have_vs && !strstr(keep, "VS=")) fputs("VS=eur\n", out);
    fputs(keep, out);
    for (int i = 0; i < sel_n; i++)
        fprintf(out, "COIN=%s,%s,0,below,off\n", sel_id[i], sel_sym[i]);
    fclose(out);
    /* reload the daemon (init respawns it) so it picks up the new coins */
    system("pkill -f integrations/crypto/crypto 2>/dev/null");
}

static void on_save(lv_event_t * e) { (void)e; save_and_reload(); ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

lv_obj_t * screen_crypto_picker_create(void) {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x101418), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, SX(8), SY(8));
    lv_obj_t * bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT " Terug");
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t * save = lv_btn_create(scr_root);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, SX(-8), SY(8));
    lv_obj_t * sl = lv_label_create(save); lv_label_set_text(sl, LV_SYMBOL_SAVE " Opslaan");
    lv_obj_add_event_cb(save, on_save, LV_EVENT_CLICKED, NULL);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_font(title, SF(22), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_label_set_text(title, "Crypto munten");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, SY(14));

    ta_search = lv_textarea_create(scr_root);
    lv_textarea_set_one_line(ta_search, true);
    lv_textarea_set_placeholder_text(ta_search, "Zoek munt (bv. bitcoin, eth, solana)…");
    lv_obj_set_width(ta_search, LV_PCT(70));
    lv_obj_align(ta_search, LV_ALIGN_TOP_MID, 0, SY(64));
    lv_obj_add_event_cb(ta_search, on_search_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ta_search, on_search_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta_search, on_search_focus, LV_EVENT_DEFOCUSED, NULL);

    lbl_picked = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_picked, lv_color_hex(0x88dd66), 0);
    lv_obj_align(lbl_picked, LV_ALIGN_TOP_MID, 0, SY(112));

    lst_results = lv_list_create(scr_root);
    lv_obj_set_size(lst_results, LV_PCT(94), SY(330));
    lv_obj_align(lst_results, LV_ALIGN_TOP_MID, 0, SY(140));

    kb = lv_keyboard_create(lv_layer_top());
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    load_current();
    update_picked_label();
    refilter();
    return scr_root;
}
