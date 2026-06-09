#ifndef TOON_ENERGY_HIST_H
#define TOON_ENERGY_HIST_H

#include "stats.h"

/* Local ring-buffer history for electricity (W), gas (m3/h), and water (L/min).
 * Records every 5 minutes into a persisted ring buffer so the Statistics screen
 * works even when hcb_rrd isn't running. Also tracks daily accumulated totals
 * (kWh / m3) for the dim-screen bars, persisted via settings so they survive
 * restarts.
 *
 *   energy_hist_start()   — spawn the background recorder thread
 *   energy_hist_series()  — fill a stats_series_t from the ring (elec=0, gas=1, water=2)
 *   energy_hist_daily_kwh/gas_m3/water_m3() — today's totals so far
 */

#define EH_CAP    2016          /* 7 days at one sample / 5 min */

void energy_hist_start(void);

/* metric: 0 = electricity, 1 = gas, 2 = water.
 *
 * Both fill `out` with timestamped ENERGY-per-sample increments (not
 * instantaneous flow), so the Statistics screen can simply sum them into
 * hour/day/week/month/year buckets:
 *   elec  -> kWh, NET of solar (negative = the period exported more than it used)
 *   gas   -> m3
 *   water -> m3
 *
 * energy_hist_hour_series:  the last ~13 h of 5-min increments (Hour view).
 * energy_hist_daily_series: one entry per calendar day in [from_ts, to_ts]
 *   (each = that day's total, incl. today's running total) for Day/Week/Month/
 *   Year and the back-in-time paging.
 * Return 0 if any sample was produced. */
int  energy_hist_hour_series(int metric, stats_series_t * out);
int  energy_hist_daily_series(int metric, long from_ts, long to_ts,
                              stats_series_t * out);

/* Today's accumulated totals — loaded from settings on startup, accumulated
 * live from the active energy sources. */
float energy_hist_daily_kwh(void);
float energy_hist_daily_gas_m3(void);
float energy_hist_daily_water_m3(void);

/* Live instantaneous values from the active source, for the Stats "Now"
 * headline. Electricity is NET (consumption - production) so it goes negative
 * while exporting solar. */
float energy_hist_now_net_w(void);
float energy_hist_now_gas_m3h(void);
float energy_hist_now_water_lpm(void);

#endif
