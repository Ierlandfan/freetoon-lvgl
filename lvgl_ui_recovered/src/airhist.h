#ifndef TOON_AIRHIST_H
#define TOON_AIRHIST_H

#include "stats.h"   /* stats_series_t */

/* Air-quality history. The Toon's hcb_rrd does NOT log eCO2/TVOC — they only
 * arrive live via BoxTalk (toon_state.eco2 / .tvoc). This records them into a
 * ring buffer (5-min cadence, ~7 days) persisted to /mnt/data so the Stats
 * screen can graph them. History fills in over time from when this first runs. */
void airhist_start(void);

/* Fill `out` with the last `window_seconds` of one metric (which: 0=eCO2,
 * 1=TVOC), capped to `max_samples`. Values are instantaneous (ppm/ppb), not
 * cumulative. Returns 0 if any samples were produced, else -1. */
int  airhist_series(int which, long window_seconds, int max_samples, stats_series_t * out);

/* CH water-pressure history — recorded hourly over ~400 days (also absent from
 * the Toon RRD), so the Stats screen can show day/week/month/year. Fills `out`
 * with the last window_seconds, stride-downsampled to <= max_samples. 0 if any. */
int  airhist_pres_series(long window_seconds, int max_samples, stats_series_t * out);

#endif
