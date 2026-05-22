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

#endif
