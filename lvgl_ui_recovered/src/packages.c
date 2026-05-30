/* HA-native delivery tracker → Toon banner via MQTT subscribe.
 *
 * HA publishes:
 *   home/packages/state   retained JSON map  {merchant|order_id: {...}}
 *   home/packages/banner  ephemeral JSON event {key,title,message,url}
 *
 * Toon subscribes to both. State updates the cached map (served via
 * /api/packages later if desired). Banner events push directly to the
 * queue rendered on home + dim. No HA YAML edits needed; HA's
 * `mqtt.publish` is built-in.
 */
#define _GNU_SOURCE
#include "packages.h"
#include "display.h"
#include "mqtt_client.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/* Banner type + cap live in packages.h so the master↔slave bridge can move
 * the whole queue as a fixed-layout struct array. */
#define BANNER_MAX    PACKAGES_BANNER_MAX
typedef packages_banner_t banner_t;

static banner_t        g_banners[BANNER_MAX];
static int             g_banner_count = 0;
static pthread_mutex_t g_banner_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Cache of (key → status) so we only banner on advancement. Capped — if
 * a user has 50 active orders we'll silently start re-bannering the
 * lowest-precedence ones, which is fine. */
#define CACHE_MAX 64
typedef struct { char key[96]; char status[16]; } cache_e_t;
static cache_e_t   g_cache[CACHE_MAX];
static int         g_cache_count = 0;
static pthread_mutex_t g_cache_mtx = PTHREAD_MUTEX_INITIALIZER;

static int status_rank(const char * s) {
    if (!s) return 0;
    if (!strcmp(s, "delivered")) return 4;
    if (!strcmp(s, "shipped"))   return 3;
    if (!strcmp(s, "ordered"))   return 2;
    if (!strcmp(s, "received"))  return 5;
    if (!strcmp(s, "pending"))   return 1;
    return 0;
}

static int cache_get_rank(const char * key) {
    pthread_mutex_lock(&g_cache_mtx);
    int r = 0;
    for (int i = 0; i < g_cache_count; i++) {
        if (!strcmp(g_cache[i].key, key)) {
            r = status_rank(g_cache[i].status);
            break;
        }
    }
    pthread_mutex_unlock(&g_cache_mtx);
    return r;
}

static void cache_put(const char * key, const char * status) {
    pthread_mutex_lock(&g_cache_mtx);
    for (int i = 0; i < g_cache_count; i++) {
        if (!strcmp(g_cache[i].key, key)) {
            snprintf(g_cache[i].status, sizeof(g_cache[i].status), "%s", status);
            pthread_mutex_unlock(&g_cache_mtx);
            return;
        }
    }
    if (g_cache_count < CACHE_MAX) {
        snprintf(g_cache[g_cache_count].key, sizeof(g_cache[0].key), "%s", key);
        snprintf(g_cache[g_cache_count].status, sizeof(g_cache[0].status), "%s", status);
        g_cache_count++;
    }
    pthread_mutex_unlock(&g_cache_mtx);
}

/* Strip / replace characters Montserrat doesn't have glyphs for, in
 * place. UTF-8 multibyte sequences become '?' to avoid the missing-
 * glyph squares we'd otherwise paint. */
static void sanitize_ascii(char * s) {
    if (!s) return;
    char * w = s;
    for (char * r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c < 0x80) {                   /* ASCII passthrough */
            *w++ = (char)c;
        } else {                          /* start of multi-byte UTF-8 */
            int more = (c >= 0xC0 && c < 0xE0) ? 1
                     : (c >= 0xE0 && c < 0xF0) ? 2
                     : (c >= 0xF0)             ? 3 : 0;
            *w++ = '?';
            while (more-- && *(r + 1)) r++;
        }
    }
    *w = 0;
}

static void banner_push(const char * key, const char * title,
                        const char * msg, const char * url) {
    pthread_mutex_lock(&g_banner_mtx);
    /* De-dup: if a banner for the same key is already queued, replace
     * its contents instead of stacking another. */
    for (int i = 0; i < g_banner_count; i++) {
        if (!strcmp(g_banners[i].key, key)) {
            snprintf(g_banners[i].title, sizeof(g_banners[0].title), "%s", title);
            snprintf(g_banners[i].msg,   sizeof(g_banners[0].msg),   "%s", msg);
            snprintf(g_banners[i].url,   sizeof(g_banners[0].url),   "%s", url ? url : "");
            pthread_mutex_unlock(&g_banner_mtx);
            return;
        }
    }
    if (g_banner_count >= BANNER_MAX) {
        /* Drop the oldest (FIFO shift). */
        memmove(&g_banners[0], &g_banners[1], sizeof(banner_t) * (BANNER_MAX - 1));
        g_banner_count = BANNER_MAX - 1;
    }
    banner_t * b = &g_banners[g_banner_count++];
    snprintf(b->key,   sizeof(b->key),   "%s", key);
    snprintf(b->title, sizeof(b->title), "%s", title);
    snprintf(b->msg,   sizeof(b->msg),   "%s", msg);
    snprintf(b->url,   sizeof(b->url),   "%s", url ? url : "");
    sanitize_ascii(b->title);
    sanitize_ascii(b->msg);
    pthread_mutex_unlock(&g_banner_mtx);
}

static int banner_peek(banner_t * out) {
    pthread_mutex_lock(&g_banner_mtx);
    int have = g_banner_count > 0;
    if (have) memcpy(out, &g_banners[0], sizeof(banner_t));
    pthread_mutex_unlock(&g_banner_mtx);
    return have;
}

static void banner_dismiss(void) {
    pthread_mutex_lock(&g_banner_mtx);
    if (g_banner_count > 0) {
        memmove(&g_banners[0], &g_banners[1], sizeof(banner_t) * (g_banner_count - 1));
        g_banner_count--;
    }
    pthread_mutex_unlock(&g_banner_mtx);
}

/* ---- master↔slave bridge: queue read + atomic replace --------------------
 * Read accessors used by pwa_server's render_state_json to walk the queue.
 * packages_set_banners_from_remote is the slave-side setter — it replaces
 * the WASM client's queue with what the master is currently showing, so
 * package banners appear on every browser within a frame. */
int packages_banner_count(void) {
    pthread_mutex_lock(&g_banner_mtx);
    int n = g_banner_count;
    pthread_mutex_unlock(&g_banner_mtx);
    return n;
}
int packages_banner_at(int i, packages_banner_t * out) {
    if (!out) return 0;
    pthread_mutex_lock(&g_banner_mtx);
    int ok = (i >= 0 && i < g_banner_count);
    if (ok) *out = g_banners[i];
    pthread_mutex_unlock(&g_banner_mtx);
    return ok;
}
void packages_set_banners_from_remote(int n, const packages_banner_t * src) {
    if (n < 0) n = 0;
    if (n > BANNER_MAX) n = BANNER_MAX;
    pthread_mutex_lock(&g_banner_mtx);
    for (int i = 0; i < n; i++) g_banners[i] = src[i];
    g_banner_count = n;
    pthread_mutex_unlock(&g_banner_mtx);
}

/* ---- MQTT message handlers ---- */

/* Extract a "key":"value" field from a (possibly-escaped) JSON object.
 * Caller-allocated `out`. Handles backslash-escapes. */
static int extract_field(const char * obj, size_t obj_len,
                         const char * field, char * out, size_t outsz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", field);
    const char * p = memmem(obj, obj_len, needle, strlen(needle));
    if (!p) { if (out && outsz) out[0] = 0; return 0; }
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') { if (out && outsz) out[0] = 0; return 0; }
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o < outsz - 1) {
        if (*p == '\\' && p[1]) { out[o++] = p[1]; p += 2; }
        else                    { out[o++] = *p++; }
    }
    out[o] = 0;
    return 1;
}

/* Banner-event payload shape (from HA's mqtt.publish on home/packages/banner):
 *   {"key":"...", "title":"...", "message":"...", "url":"..."}
 * State map payload shape (from home/packages/state retained):
 *   {"merchant|order":{"status":"...","label":"...","eta":"...","url":"...","ts":"..."}, …}
 *
 * For the banner topic we just push the payload directly. For the state
 * topic we iterate and diff against the rank-cache — this gives us a
 * safety net so a state update without a corresponding banner event
 * still surfaces a banner on advancement. */

static void on_banner_event(const unsigned char * payload, size_t len) {
    char key[96] = "", title[64] = "", msg[160] = "", url[256] = "";
    extract_field((const char *)payload, len, "key",     key,   sizeof(key));
    extract_field((const char *)payload, len, "title",   title, sizeof(title));
    extract_field((const char *)payload, len, "message", msg,   sizeof(msg));
    extract_field((const char *)payload, len, "url",     url,   sizeof(url));
    if (!title[0] && !msg[0]) return;
    if (!key[0]) snprintf(key, sizeof(key), "anon-%ld", (long)time(NULL));
    banner_push(key, title, msg, url);
    fprintf(stderr, "[pkg] banner via mqtt: %s — %s\n", title, msg);
}

static void on_state_map(const unsigned char * payload, size_t len) {
    /* Iterate {"key":{...},"key":{...}} flat dict; only diff status to
     * banner queue. (No nested objects deeper than depth 2.) */
    const char * p = (const char *)payload;
    const char * end = p + len;
    while (p < end && *p != '{') p++;
    if (p < end) p++;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r')) p++;
        if (p >= end || *p == '}') break;
        if (*p != '"') break;
        p++;
        char key[96]; size_t kl = 0;
        while (p < end && *p != '"' && kl < sizeof(key) - 1) key[kl++] = *p++;
        key[kl] = 0;
        if (p < end && *p == '"') p++;
        while (p < end && (*p == ' ' || *p == ':' || *p == '\t')) p++;
        if (p >= end || *p != '{') break;
        const char * vs = p; int d = 1; p++;
        while (p < end && d) { if (*p == '{') d++; else if (*p == '}') d--; p++; }
        size_t vlen = (size_t)(p - vs);

        char status[16] = "", label[160] = "", eta[16] = "", url[256] = "";
        extract_field(vs, vlen, "status", status, sizeof(status));
        extract_field(vs, vlen, "label",  label,  sizeof(label));
        extract_field(vs, vlen, "eta",    eta,    sizeof(eta));
        extract_field(vs, vlen, "url",    url,    sizeof(url));
        if (!status[0]) continue;
        int new_rank = status_rank(status);
        int old_rank = cache_get_rank(key);
        if (new_rank <= old_rank) continue;
        cache_put(key, status);
        char merchant[40] = "package";
        const char * bar = strchr(key, '|');
        if (bar) {
            size_t L = (size_t)(bar - key);
            if (L >= sizeof(merchant)) L = sizeof(merchant) - 1;
            memcpy(merchant, key, L); merchant[L] = 0;
        }
        char title[64], msg[160];
        snprintf(title, sizeof(title), "%s: %s", merchant, status);
        snprintf(msg,   sizeof(msg),   "%s%s%s", label, eta[0] ? " - eta " : "", eta);
        banner_push(key, title, msg, url);
        fprintf(stderr, "[pkg] state-diff banner: %s — %s\n", title, msg);
    }
}

static void on_mqtt_msg(const char * topic, const unsigned char * payload,
                         size_t len, void * arg) {
    (void)arg;
    if (!topic || !payload || !len) return;
    if (!strcmp(topic, "home/packages/banner")) on_banner_event(payload, len);
    else if (!strcmp(topic, "home/packages/state")) on_state_map(payload, len);
}

void packages_start(void) {
    /* Topic list is owned by settings.mqtt_topics — edited in
     * Settings → MQTT. Just hand the broker our message callback and the
     * subscriber thread reads everything else from settings. */
    mqtt_client_start(on_mqtt_msg, NULL);
}

/* ---- LVGL banner widget ----
 * One per screen; LVGL timer (every 1s) reads banner_peek() and shows/hides
 * the widget + updates labels. Tap dismisses the top entry. Visible state
 * across attached banners stays in sync because they all read the same
 * shared queue. */
typedef struct {
    lv_obj_t * box;
    lv_obj_t * lbl_title;
    lv_obj_t * lbl_msg;
    lv_timer_t * t;
    char shown_key[96];
} bw_t;

static void bw_tick(lv_timer_t * t) {
    bw_t * bw = (bw_t *)t->user_data;
    if (!bw || !bw->box) return;
    banner_t b;
    if (banner_peek(&b)) {
        if (strcmp(bw->shown_key, b.key) != 0) {
            lv_label_set_text(bw->lbl_title, b.title);
            lv_label_set_text(bw->lbl_msg,   b.msg);
            snprintf(bw->shown_key, sizeof(bw->shown_key), "%s", b.key);
        }
        if (lv_obj_has_flag(bw->box, LV_OBJ_FLAG_HIDDEN))
            lv_obj_clear_flag(bw->box, LV_OBJ_FLAG_HIDDEN);
        /* Re-assert z-order every tick: tiles added after our attach
         * (or LVGL refresh ordering) can put the banner behind. Cheap. */
        lv_obj_move_foreground(bw->box);
    } else {
        if (!lv_obj_has_flag(bw->box, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(bw->box, LV_OBJ_FLAG_HIDDEN);
        bw->shown_key[0] = 0;
    }
}

static void bw_click(lv_event_t * e) {
    (void)e;
    banner_dismiss();
}

void packages_banner_attach(lv_obj_t * parent) {
    if (!parent) return;
    bw_t * bw = (bw_t *)lv_mem_alloc(sizeof(bw_t));
    if (!bw) return;
    memset(bw, 0, sizeof(*bw));
    /* Full-width strip at top, ~58px tall. Sits above the screen tiles. */
    bw->box = lv_obj_create(parent);
    lv_obj_set_size(bw->box, 1024, 58);
    lv_obj_align(bw->box, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bw->box, lv_color_hex(0x2a4060), 0);
    lv_obj_set_style_bg_opa(bw->box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bw->box, lv_color_hex(0xffcc44), 0);
    lv_obj_set_style_border_width(bw->box, 0, 0);
    lv_obj_set_style_border_side(bw->box, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(bw->box, 0, 0);
    lv_obj_set_style_pad_all(bw->box, 8, 0);
    lv_obj_add_flag(bw->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bw->box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(bw->box, bw_click, LV_EVENT_CLICKED, NULL);

    bw->lbl_title = lv_label_create(bw->box);
    lv_obj_set_style_text_color(bw->lbl_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bw->lbl_title, SF(22), 0);
    lv_obj_align(bw->lbl_title, LV_ALIGN_LEFT_MID, 12, -10);
    lv_label_set_text(bw->lbl_title, "");

    bw->lbl_msg = lv_label_create(bw->box);
    lv_obj_set_style_text_color(bw->lbl_msg, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(bw->lbl_msg, SF(18), 0);
    lv_obj_align(bw->lbl_msg, LV_ALIGN_LEFT_MID, 12, 14);
    lv_label_set_text(bw->lbl_msg, "");

    /* Small "tap to dismiss" hint on the right side */
    lv_obj_t * hint = lv_label_create(bw->box);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(hint, SF(14), 0);
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_label_set_text(hint, "tap to dismiss  x");

    /* Force the banner to the top of the z-order so it's not hidden by
     * tiles that get added later. */
    lv_obj_move_foreground(bw->box);

    bw->t = lv_timer_create(bw_tick, 1000, bw);
}
