#ifndef TOON_NEWS_H
#define TOON_NEWS_H

#include <stddef.h>

/* Built-in RSS newsreader. A background thread fetches settings.news_rss_url
 * periodically and keeps a small list of headline + link pairs that the home
 * screen renders as a scrolling ticker. */

#define NEWS_MAX_ITEMS   12
#define NEWS_TITLE_MAX   160
#define NEWS_LINK_MAX    256
#define NEWS_BODY_MAX    900   /* article summary from RSS <description> */

int  news_start(void);                 /* spawn fetch thread if news_enabled */
/* Synchronously fetch+parse `url` (for the settings Test button). Returns the
 * headline count (>=0) or -1 on error; writes a human message into `msg`. */
int  news_test_feed(const char * url, char * msg, size_t msgsz);
int  news_count(void);                 /* number of headlines currently held */
/* Copy the i-th headline + link into the given buffers. Returns 0 on success. */
int  news_item(int i, char * title, size_t tsz, char * link, size_t lsz);
/* Copy the i-th article body (RSS <description>, HTML-stripped). 0 on success. */
int  news_body(int i, char * body, size_t bsz);

/* Slave-bridge setters — let client_link_apply_state() replace the in-memory
 * list with what the master Toon publishes. Safe to call repeatedly. */
void news_set_count(int n);
void news_set_item_data(int i, const char * title, const char * link, int feed);

#define NEWS_MAX_FEEDS   8
/* Per-feed grouping for the detailed reader. */
int  news_feed_count(void);                          /* number of distinct feeds */
int  news_feed_name(int f, char * name, size_t nsz); /* feed f's channel title */
int  news_item_feed(int i);                          /* feed index of item i, or -1 */

#endif
