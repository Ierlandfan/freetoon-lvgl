/*
 * news — built-in RSS newsreader.
 *
 * A background thread fetches settings.news_rss_url with curl every 15 min,
 * parses <item> blocks for <title> + <link>, and stores up to NEWS_MAX_ITEMS
 * headlines for the home-screen ticker. CDATA-aware, entity-light. The URL is
 * validated to a safe character set before it touches the curl command line.
 */
#define _GNU_SOURCE
#include "news.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

typedef struct {
    char title[NEWS_TITLE_MAX];
    char link[NEWS_LINK_MAX];
} news_item_t;

static news_item_t     g_items[NEWS_MAX_ITEMS];
static int             g_count = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static int url_ok(const char * u) {
    if (!u || !*u) return 0;
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) return 0;
    for (const char * p = u; *p; p++)
        if (*p == '\'' || *p == '`' || *p == '"' || (unsigned char)*p < 0x20 || *p == ' ')
            return 0;
    return 1;
}

/* Minimal entity + whitespace cleanup, in place. */
static void clean_text(char * s) {
    /* unescape the few entities RSS titles actually use */
    static const struct { const char * e; char c; } ent[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'},
        {"&#39;", '\''}, {"&apos;", '\''}, {NULL, 0}
    };
    for (int i = 0; ent[i].e; i++) {
        char * p;
        while ((p = strstr(s, ent[i].e))) {
            *p = ent[i].c;
            memmove(p + 1, p + strlen(ent[i].e), strlen(p + strlen(ent[i].e)) + 1);
        }
    }
    /* collapse non-ASCII to '?' (Montserrat has no glyphs for them) and runs
     * of whitespace to single spaces */
    char * w = s;
    int prev_sp = 0;
    for (char * r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!prev_sp) { *w++ = ' '; prev_sp = 1; }
        } else if (c >= 0x80) {
            *w++ = '?'; prev_sp = 0;
        } else {
            *w++ = (char)c; prev_sp = 0;
        }
    }
    *w = 0;
    /* trim trailing space */
    while (w > s && w[-1] == ' ') *--w = 0;
}

/* Extract the inner text of the first <tag>…</tag> inside [blk,blk_end),
 * stripping a CDATA wrapper. Returns 0 on success. */
static int tag_text(const char * blk, const char * blk_end, const char * tag,
                    char * out, size_t outsz) {
    char open[24], close[24];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char * a = memmem(blk, blk_end - blk, open, strlen(open));
    /* also accept <tag ...> with attributes */
    if (!a) {
        char open2[24]; snprintf(open2, sizeof open2, "<%s", tag);
        a = memmem(blk, blk_end - blk, open2, strlen(open2));
        if (!a) return -1;
        a = memchr(a, '>', blk_end - a);
        if (!a) return -1;
        a++;
    } else {
        a += strlen(open);
    }
    const char * b = memmem(a, blk_end - a, close, strlen(close));
    if (!b) return -1;
    const char * s = a; const char * e = b;
    /* unwrap CDATA */
    if (e - s > 12 && !strncmp(s, "<![CDATA[", 9)) {
        s += 9;
        const char * cend = memmem(s, e - s, "]]>", 3);
        if (cend) e = cend;
    }
    size_t len = (size_t)(e - s);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, s, len); out[len] = 0;
    clean_text(out);
    return 0;
}

static void parse_feed(const char * xml, size_t len) {
    const char * p = xml;
    const char * end = xml + len;
    int n = 0;
    pthread_mutex_lock(&g_mtx);
    while (n < NEWS_MAX_ITEMS) {
        const char * it = memmem(p, end - p, "<item", 5);
        if (!it) it = memmem(p, end - p, "<entry", 6);   /* Atom fallback */
        if (!it) break;
        const char * it_end = memmem(it, end - it, "</item", 6);
        if (!it_end) it_end = memmem(it, end - it, "</entry", 7);
        if (!it_end) it_end = end;

        char title[NEWS_TITLE_MAX] = "", link[NEWS_LINK_MAX] = "";
        if (tag_text(it, it_end, "title", title, sizeof title) == 0 && title[0]) {
            tag_text(it, it_end, "link", link, sizeof link);  /* link optional */
            snprintf(g_items[n].title, sizeof g_items[n].title, "%s", title);
            snprintf(g_items[n].link,  sizeof g_items[n].link,  "%s", link);
            n++;
        }
        p = it_end + 1;
        if (p >= end) break;
    }
    g_count = n;
    pthread_mutex_unlock(&g_mtx);
    fprintf(stderr, "[news] parsed %d headlines\n", n);
}

static void * fetch_thread(void * arg) {
    (void)arg;
    static char buf[131072];     /* RSS feeds fit comfortably in 128 KB */
    for (;;) {
        if (settings.news_enabled && url_ok(settings.news_rss_url)) {
            char cmd[400];
            snprintf(cmd, sizeof cmd,
                "curl -s -L -m 20 -A 'freetoon-news/1.0' '%s' 2>/dev/null",
                settings.news_rss_url);
            FILE * f = popen(cmd, "r");
            if (f) {
                size_t got = fread(buf, 1, sizeof buf - 1, f);
                pclose(f);
                buf[got] = 0;
                if (got > 64) parse_feed(buf, got);
            }
        }
        sleep(900);   /* refresh every 15 min */
    }
    return NULL;
}

int news_start(void) {
    if (!settings.news_enabled) return 0;
    pthread_t t;
    if (pthread_create(&t, NULL, fetch_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}

int news_test_feed(const char * url, char * msg, size_t msgsz) {
    if (!url_ok(url)) { snprintf(msg, msgsz, "Ongeldige URL (http/https, geen spaties)"); return -1; }
    char cmd[400];
    snprintf(cmd, sizeof cmd,
        "curl -s -L -m 12 -A 'freetoon-news/1.0' '%s' 2>/dev/null", url);
    FILE * f = popen(cmd, "r");
    if (!f) { snprintf(msg, msgsz, "Kon curl niet starten"); return -1; }
    static char buf[131072];
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    pclose(f);
    buf[got] = 0;
    if (got < 32) { snprintf(msg, msgsz, "Geen data ontvangen"); return -1; }

    /* Count items + grab the first title. */
    int n = 0;
    char first[NEWS_TITLE_MAX] = "";
    const char * p = buf; const char * end = buf + got;
    while (1) {
        const char * it = memmem(p, end - p, "<item", 5);
        if (!it) it = memmem(p, end - p, "<entry", 6);
        if (!it) break;
        const char * it_end = memmem(it, end - it, "</item", 6);
        if (!it_end) it_end = memmem(it, end - it, "</entry", 7);
        if (!it_end) it_end = end;
        char title[NEWS_TITLE_MAX];
        if (tag_text(it, it_end, "title", title, sizeof title) == 0 && title[0]) {
            if (n == 0) snprintf(first, sizeof first, "%s", title);
            n++;
        }
        p = it_end + 1;
        if (p >= end) break;
    }
    if (n == 0) { snprintf(msg, msgsz, "Geen koppen gevonden (geen RSS/Atom?)"); return 0; }
    snprintf(msg, msgsz, "OK: %d koppen. Eerste: %.80s", n, first);
    return n;
}

int news_count(void) {
    pthread_mutex_lock(&g_mtx);
    int c = g_count;
    pthread_mutex_unlock(&g_mtx);
    return c;
}

int news_item(int i, char * title, size_t tsz, char * link, size_t lsz) {
    int rc = -1;
    pthread_mutex_lock(&g_mtx);
    if (i >= 0 && i < g_count) {
        if (title) snprintf(title, tsz, "%s", g_items[i].title);
        if (link)  snprintf(link,  lsz, "%s", g_items[i].link);
        rc = 0;
    }
    pthread_mutex_unlock(&g_mtx);
    return rc;
}
