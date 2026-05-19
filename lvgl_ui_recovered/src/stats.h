#ifndef TOON_STATS_H
#define TOON_STATS_H

#include <stddef.h>

/* hcb_rrd HTTP fetch + JSON parse for the stats screen.
   The endpoint returns: {"DD-MM-YYYY HH:MM:SS": value, ...}
   We parse into parallel arrays of timestamps + floats (NaN = null/missing). */

#define STATS_MAX_SAMPLES 512

typedef struct {
    int    n;
    double samples[STATS_MAX_SAMPLES];   /* value per slot (NaN for null) */
    char   labels[STATS_MAX_SAMPLES][20]; /* "DD-MM HH:MM" short label */
    double min, max;
} stats_series_t;

/* Fetch raw history JSON for a given loggerName + rra. Returns 0 on success.
 * Parsed values land in `out` (max STATS_MAX_SAMPLES samples).
 *
 * `window_seconds` requests only the last N seconds of data via
 * hcb_rrd's `from=<unix>&to=<unix>` params. Pass 0 to omit the
 * time-range filter (full archive). Crucial for the period tabs —
 * without it `samples=N` downsamples across the entire 5-year (or
 * 10-year) RRA span, so Week appeared to show ~21 days.
 *
 * `max_samples` is the secondary cap, applied after the time
 * window. STATS_MAX_SAMPLES (=512) is the safe default that fits
 * the chart and the in-process buffer. */
int stats_fetch(const char * logger_name, const char * rra,
                long window_seconds, int max_samples,
                stats_series_t * out);

/* Convenience pre-defined fetches. */
int stats_elec_flow_5min(stats_series_t * out);
int stats_gas_flow_5min(stats_series_t * out);
int stats_water_flow_5min(stats_series_t * out);

#endif
