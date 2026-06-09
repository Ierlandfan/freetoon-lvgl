/*
 * Energy history recorder — see energy_hist.h.
 * Samples power_w / gas_hour_m3 / water_lpm every 5 minutes into persisted
 * ring buffers so the Statistics screen works without hcb_rrd. Also accumulates
 * daily totals from cumulative source readings and persists them via settings
 * so the dim-screen bars survive restarts.
 */
#include "energy_hist.h"
#include "settings.h"
#include "homewizard.h"
#include "meteradapter.h"
#include "homeassistant.h"
#include "domoticz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>

/* Per-source dispatch helpers — same as screen_home.c but self-contained. */
static float src_elec_power_w(void) {
    switch (settings.energy_elec_source) {
    case ENERGY_SRC_ZWAVE:    return meter_state.power_w;
    case ENERGY_SRC_HW_P1:    return hw_state.power_w;
    case ENERGY_SRC_HA:       return ha_energy.power_w;
    case ENERGY_SRC_DOMOTICZ: return dz_energy.power_w;
    default:                  return 0;
    }
}
static float src_elec_prod_w(void) {
    if (settings.energy_elec_source == ENERGY_SRC_HA &&
        settings.energy_elec_prod_ha_entity[0])
        return ha_energy.power_prod_w;
    if (settings.energy_elec_source == ENERGY_SRC_DOMOTICZ &&
        settings.energy_elec_prod_dz_idx > 0)
        return dz_energy.power_prod_w;
    return 0;
}
static float src_gas_hour_m3(void) {
    switch (settings.energy_gas_source) {
    case ENERGY_SRC_ZWAVE:    return meter_state.gas_connected ? meter_state.gas_hour_m3 : 0;
    case ENERGY_SRC_HW_P1:    return hw_state.connected_p1 ? hw_state.gas_hour_m3 : 0;
    case ENERGY_SRC_HA:       return ha_energy.connected ? ha_energy.gas_hour_m3 : 0;
    case ENERGY_SRC_DOMOTICZ: return dz_energy.connected ? dz_energy.gas_hour_m3 : 0;
    default:                  return 0;
    }
}
static float src_gas_cum_m3(void) {
    switch (settings.energy_gas_source) {
    case ENERGY_SRC_ZWAVE:    return meter_state.gas_connected ? meter_state.gas_m3 : -1;
    case ENERGY_SRC_HW_P1:    return hw_state.connected_p1 ? hw_state.gas_m3 : -1;
    case ENERGY_SRC_HA:       return ha_energy.connected ? ha_energy.gas_m3 : -1;
    case ENERGY_SRC_DOMOTICZ: return dz_energy.connected ? dz_energy.gas_m3 : -1;
    default:                  return -1;
    }
}
static float src_water_cum_m3(void) {
    switch (settings.energy_water_source) {
    case ENERGY_SRC_HW_P1:    return hw_state.connected_water ? hw_state.water_total_m3 : -1;
    case ENERGY_SRC_HA:       return ha_energy.connected ? ha_energy.water_m3 : -1;
    case ENERGY_SRC_DOMOTICZ: return dz_energy.connected ? dz_energy.water_m3 : -1;
    default:                  return -1;
    }
}

/* ---- Ring-buffer storage ---- */
#define EH_MAGIC 0x45483032u   /* "EH02" — bumped: eh_sample_t grew a prod_w field */
#define ED_MAGIC 0x45443031u   /* "ED01" — daily-totals store */
/* File paths — /mnt/data on the device, overridable via $TOONUI_DATA_DIR so the
 * headless sim/tests can seed them. tmp=1 → the .tmp write target. `name` is the
 * base filename. */
static const char * eh_named_path(const char * name, int tmp) {
    static char buf[256];
    const char * d = getenv("TOONUI_DATA_DIR");
    snprintf(buf, sizeof buf, "%s/%s%s", (d && *d) ? d : "/mnt/data", name,
             tmp ? ".tmp" : "");
    return buf;
}
static const char * eh_path(int tmp) { return eh_named_path("freetoon_energyhist.bin", tmp); }
static const char * ed_path(int tmp) { return eh_named_path("freetoon_energydays.bin", tmp); }

typedef struct {
    long ts;           /* epoch seconds */
    short power_w;     /* consumption W (capped to 32767) */
    short prod_w;      /* production W (solar) */
    short gas_dlph;    /* gas m³/h × 100 — dL per hour */
    short water_clpm;  /* water L/min × 100 */
} eh_sample_t;

/* Long-term daily totals — one record per calendar day, kept for years so the
 * Day/Week/Month/Year views and the back-in-time paging have history beyond the
 * 7-day 5-min ring. net_kwh is signed (negative = the day exported more than it
 * used). Persisted to its own file. */
typedef struct {
    int   ymd;         /* YYYYMMDD */
    float net_kwh;     /* electricity, net of solar (signed) */
    float gas_m3;
    float water_m3;
} eh_day_t;
#define EH_DAYS 3100   /* ~8.5 years */
static eh_day_t days[EH_DAYS];
static int days_n = 0;
static pthread_mutex_t days_mtx = PTHREAD_MUTEX_INITIALIZER;

static int  ymd_of(time_t t) {
    struct tm tm; localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}
static long ymd_midnight(int ymd) {
    struct tm tm; memset(&tm, 0, sizeof tm);
    tm.tm_year = ymd / 10000 - 1900;
    tm.tm_mon  = (ymd / 100) % 100 - 1;
    tm.tm_mday = ymd % 100;
    tm.tm_hour = 12; tm.tm_isdst = -1;   /* noon avoids DST edge slipping the date */
    return (long)mktime(&tm);
}

static eh_sample_t ring[EH_CAP];
static int eh_head = 0, eh_count = 0;
static pthread_mutex_t eh_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ---- Daily accumulation (persisted via settings) ---- */
static float daily_kwh = 0, daily_gas_m3 = 0, daily_water_m3 = 0;
static float daily_net_kwh = 0;   /* signed (negative = net export) — for Stats */
static char  daily_date[16] = "";
/* Baseline cumulative readings for the current day (delta = current - base). */
static float base_gas_m3 = 0, base_water_m3 = 0;
/* The midnight baseline is only meaningful once the source has reported a real
 * cumulative reading. Until then these stay 0 so we DON'T treat the whole meter
 * total as today's usage (the "Day shows 13421 m3" bug). */
static int   gas_base_valid = 0, water_base_valid = 0;
/* Electricity integration state (no cumulative kWh on most sources). */
static time_t last_elec_tick = 0;
static float  last_power_w = 0;
static pthread_mutex_t daily_mtx = PTHREAD_MUTEX_INITIALIZER;

float energy_hist_daily_kwh(void) {
    float v; pthread_mutex_lock(&daily_mtx); v = daily_kwh; pthread_mutex_unlock(&daily_mtx);
    return v;
}
float energy_hist_daily_gas_m3(void) {
    float v; pthread_mutex_lock(&daily_mtx); v = daily_gas_m3; pthread_mutex_unlock(&daily_mtx);
    return v;
}
float energy_hist_daily_water_m3(void) {
    float v; pthread_mutex_lock(&daily_mtx); v = daily_water_m3; pthread_mutex_unlock(&daily_mtx);
    return v;
}

/* ---- Persistence ---- */
static void eh_load(void) {
    FILE * f = fopen(eh_path(0), "rb");
    if (!f) return;
    unsigned magic = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == EH_MAGIC &&
        fread(&eh_head, sizeof eh_head, 1, f) == 1 &&
        fread(&eh_count, sizeof eh_count, 1, f) == 1 &&
        fread(ring, sizeof ring, 1, f) == 1) {
        if (eh_count < 0 || eh_count > EH_CAP || eh_head < 0 || eh_head >= EH_CAP)
            { eh_head = 0; eh_count = 0; memset(ring, 0, sizeof ring); }
    }
    fclose(f);
}
static void eh_save(void) {
    char tmp[256]; snprintf(tmp, sizeof tmp, "%s", eh_path(1));
    FILE * f = fopen(tmp, "wb");
    if (!f) return;
    unsigned magic = EH_MAGIC;
    fwrite(&magic, 4, 1, f);
    fwrite(&eh_head, sizeof eh_head, 1, f);
    fwrite(&eh_count, sizeof eh_count, 1, f);
    fwrite(ring, sizeof ring, 1, f);
    fclose(f);
    rename(tmp, eh_path(0));
}
static void eh_push(long ts, short pw, short prod, short g_dlph, short w_clpm) {
    pthread_mutex_lock(&eh_mtx);
    ring[eh_head].ts         = ts;
    ring[eh_head].power_w    = pw;
    ring[eh_head].prod_w     = prod;
    ring[eh_head].gas_dlph   = g_dlph;
    ring[eh_head].water_clpm = w_clpm;
    eh_head = (eh_head + 1) % EH_CAP;
    if (eh_count < EH_CAP) eh_count++;
    pthread_mutex_unlock(&eh_mtx);
}

/* ---- Daily-totals store ---- */
static void days_load(void) {
    FILE * f = fopen(ed_path(0), "rb");
    if (!f) return;
    unsigned magic = 0; int n = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == ED_MAGIC &&
        fread(&n, sizeof n, 1, f) == 1 && n >= 0 && n <= EH_DAYS &&
        fread(days, sizeof(eh_day_t), n, f) == (size_t)n)
        days_n = n;
    fclose(f);
}
static void days_save(void) {
    char tmp[256]; snprintf(tmp, sizeof tmp, "%s", ed_path(1));
    FILE * f = fopen(tmp, "wb");
    if (!f) return;
    unsigned magic = ED_MAGIC;
    pthread_mutex_lock(&days_mtx);
    fwrite(&magic, 4, 1, f);
    fwrite(&days_n, sizeof days_n, 1, f);
    fwrite(days, sizeof(eh_day_t), days_n, f);
    pthread_mutex_unlock(&days_mtx);
    fclose(f);
    rename(tmp, ed_path(0));
}
/* Commit a finished day's totals (append, or update if the same ymd recurs). */
static void days_commit(int ymd, float net_kwh, float gas_m3, float water_m3) {
    pthread_mutex_lock(&days_mtx);
    int at = -1;
    if (days_n > 0 && days[days_n - 1].ymd == ymd) at = days_n - 1;
    if (at < 0) {
        if (days_n >= EH_DAYS) {           /* full → drop the oldest */
            memmove(days, days + 1, (EH_DAYS - 1) * sizeof(eh_day_t));
            days_n = EH_DAYS - 1;
        }
        at = days_n++;
        days[at].ymd = ymd;
    }
    days[at].net_kwh = net_kwh; days[at].gas_m3 = gas_m3; days[at].water_m3 = water_m3;
    pthread_mutex_unlock(&days_mtx);
    days_save();
}

/* ---- Daily accumulation tick ---- */
static void daily_tick(void) {
    time_t now_t = time(NULL);
    struct tm tm; localtime_r(&now_t, &tm);
    char today[16]; snprintf(today, sizeof today, "%04d-%02d-%02d",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    pthread_mutex_lock(&daily_mtx);

    /* Day rollover: commit the finished day to the long-term store, then reset
     * accumulators + snapshot new baselines. */
    if (strcmp(daily_date, today) != 0) {
        if (daily_date[0]) {
            int oy, om, od;
            if (sscanf(daily_date, "%d-%d-%d", &oy, &om, &od) == 3)
                days_commit(oy * 10000 + om * 100 + od,
                            daily_net_kwh, daily_gas_m3, daily_water_m3);
        }
        snprintf(daily_date, sizeof daily_date, "%s", today);
        daily_kwh = 0; daily_net_kwh = 0; daily_gas_m3 = 0; daily_water_m3 = 0;
        gas_base_valid = 0; water_base_valid = 0;  /* re-baseline at the new day's
                                                    * first real reading (≈ 00:00) */
        last_elec_tick = 0;
    }

    /* Electricity: integrate power_w over time. Use net (consumption - production)
     * so a solar day shows net usage; the dim bar handles the sign. */
    float pw = src_elec_power_w();
    float prod = src_elec_prod_w();
    float net_w = pw - prod;  /* negative = exporting */
    if (last_elec_tick > 0) {
        double dt = difftime(now_t, last_elec_tick);
        if (dt > 0 && dt < 3600) {    /* sanity: skip >1h gaps (clock set / resume) */
            double inc = (double)((net_w + last_power_w) * 0.5f) * (dt / 3600.0) / 1000.0;
            daily_kwh     += inc;
            daily_net_kwh += inc;     /* unclamped: keeps the solar-export sign for Stats */
        }
        if (daily_kwh < 0) daily_kwh = 0;
    }
    last_elec_tick = now_t;
    last_power_w = net_w;

    /* Gas: usage = current cumulative reading − the reading at 00:00. The
     * baseline is set from the FIRST valid reading of the day (continuing any
     * total restored from settings), never from a 0 fallback. */
    float g = src_gas_cum_m3();
    if (g >= 0) {
        if (!gas_base_valid) {
            base_gas_m3 = g - daily_gas_m3;          /* continue from restored total */
            if (base_gas_m3 < 0) base_gas_m3 = g;
            gas_base_valid = 1;
        }
        if (g >= base_gas_m3) daily_gas_m3 = g - base_gas_m3;
    }

    /* Water: same midnight-baseline logic. */
    float w = src_water_cum_m3();
    if (w >= 0) {
        if (!water_base_valid) {
            base_water_m3 = w - daily_water_m3;
            if (base_water_m3 < 0) base_water_m3 = w;
            water_base_valid = 1;
        }
        if (w >= base_water_m3) daily_water_m3 = w - base_water_m3;
    }

    pthread_mutex_unlock(&daily_mtx);
}

/* ---- Background thread ---- */
static void * eh_thread(void * arg) {
    (void)arg;
    /* ring + daily totals already restored synchronously by energy_hist_start() */
    sleep(30);  /* let the energy sources start polling */

    int tick = 0;
    for (;;) {
        /* Daily accumulation + settings persist every 30 s. */
        daily_tick();

        /* Persist daily totals to settings every 5 min (10 ticks). */
        if (tick % 10 == 0) {
            pthread_mutex_lock(&daily_mtx);
            snprintf(settings.energy_daily_date, sizeof settings.energy_daily_date,
                     "%s", daily_date);
            settings.energy_daily_kwh      = daily_kwh;
            settings.energy_daily_net_kwh  = daily_net_kwh;
            settings.energy_daily_gas_m3   = daily_gas_m3;
            settings.energy_daily_water_m3 = daily_water_m3;
            pthread_mutex_unlock(&daily_mtx);
            settings_save();
        }

        /* Record samples into the ring buffer every 5 min (10 ticks). */
        if (tick % 10 == 0) {
            float pw = src_elec_power_w();
            float pr = src_elec_prod_w();
            float gh = src_gas_hour_m3();
            /* water_lpm from active source */
            float wl = 0;
            switch (settings.energy_water_source) {
            case ENERGY_SRC_HW_P1:    wl = hw_state.water_lpm; break;
            /* HA/Domoticz: water is cumulative-only, no live L/min.
             * Record 0 — the stats screen shows daily totals for water. */
            default: break;
            }
            short spw = (short)(pw > 32767 ? 32767 : pw);
            short spr = (short)(pr > 32767 ? 32767 : pr);
            short sgh = (short)(gh * 100.0f + 0.5f);   /* dL/h */
            short swl = (short)(wl * 100.0f + 0.5f);   /* cL/min */
            eh_push((long)time(NULL), spw, spr, sgh, swl);
            eh_save();
        }

        tick++;
        sleep(30);
    }
    return NULL;
}

/* Live "Now" values for the Stats headline. */
float energy_hist_now_net_w(void)     { return src_elec_power_w() - src_elec_prod_w(); }
float energy_hist_now_gas_m3h(void)   { return src_gas_hour_m3(); }
float energy_hist_now_water_lpm(void) {
    return (settings.energy_water_source == ENERGY_SRC_HW_P1) ? hw_state.water_lpm : 0;
}

void energy_hist_start(void) {
    eh_load();   /* synchronous so the Statistics screen sees data immediately */
    days_load();
    /* Restore today's running totals from settings synchronously, so the dim
     * screen + Stats show today's data on the very first frame. */
    pthread_mutex_lock(&daily_mtx);
    snprintf(daily_date, sizeof daily_date, "%s", settings.energy_daily_date);
    daily_kwh      = settings.energy_daily_kwh;
    daily_net_kwh  = settings.energy_daily_net_kwh;
    daily_gas_m3   = settings.energy_daily_gas_m3;
    daily_water_m3 = settings.energy_daily_water_m3;
    /* A daily gas/water total over ~100 m3 is impossible for a home — it's a
     * leftover whole-meter-total from the old baseline bug; start fresh today. */
    if (daily_gas_m3   > 100 || daily_gas_m3   < 0) daily_gas_m3   = 0;
    if (daily_water_m3 > 100 || daily_water_m3 < 0) daily_water_m3 = 0;
    /* Don't baseline here — the source isn't polling yet, so reading it now
     * gives -1 and a 0 baseline (→ "today = whole meter total"). daily_tick()
     * sets the baseline from the first real reading, continuing these totals. */
    gas_base_valid = 0; water_base_valid = 0;
    pthread_mutex_unlock(&daily_mtx);
    pthread_t th;
    if (pthread_create(&th, NULL, eh_thread, NULL) == 0) pthread_detach(th);
}

/* ---- Series access for the Statistics screen ---- */

/* metric → energy increment for one 5-min ring sample (recorder cadence). */
static double eh_sample_energy(int metric, const eh_sample_t * s) {
    switch (metric) {
    case 0: return (s->power_w - s->prod_w) / 12000.0; /* net kWh per 5 min */
    case 1: return s->gas_dlph   / 1200.0;             /* m³ per 5 min      */
    case 2: return s->water_clpm / 20000.0;            /* m³ per 5 min      */
    default: return 0;
    }
}

static void series_emit(stats_series_t * out, long ts, double v) {
    if (out->n >= STATS_MAX_SAMPLES) return;
    out->samples[out->n] = v;
    out->ts[out->n] = ts;
    struct tm tm; time_t tt = (time_t)ts; localtime_r(&tt, &tm);
    strftime(out->labels[out->n], sizeof out->labels[0], "%d-%m %H:%M", &tm);
    snprintf(out->year2[out->n], 3, "%02d", (tm.tm_year + 1900) % 100);
    if (v < out->min) out->min = v;
    if (v > out->max) out->max = v;
    out->n++;
}

int energy_hist_hour_series(int metric, stats_series_t * out) {
    out->n = 0; out->min = 1e30; out->max = -1e30;
    long cutoff = (long)time(NULL) - 13 * 3600;   /* 12 hour buckets + margin */
    pthread_mutex_lock(&eh_mtx);
    for (int i = 0; i < eh_count; i++) {
        int idx = (eh_head - eh_count + i + 2 * EH_CAP) % EH_CAP;
        if (ring[idx].ts < cutoff) continue;
        series_emit(out, ring[idx].ts, eh_sample_energy(metric, &ring[idx]));
    }
    pthread_mutex_unlock(&eh_mtx);
    return out->n > 0 ? 0 : -1;
}

int energy_hist_daily_series(int metric, long from_ts, long to_ts,
                             stats_series_t * out) {
    out->n = 0; out->min = 1e30; out->max = -1e30;
    int from_ymd = ymd_of((time_t)from_ts), to_ymd = ymd_of((time_t)to_ts);
    int today    = ymd_of(time(NULL));
    pthread_mutex_lock(&days_mtx);
    for (int i = 0; i < days_n; i++) {
        if (days[i].ymd < from_ymd || days[i].ymd > to_ymd || days[i].ymd == today)
            continue;
        double v = metric == 0 ? days[i].net_kwh
                 : metric == 1 ? days[i].gas_m3 : days[i].water_m3;
        /* Drop legacy bogus gas/water days (whole meter total stored as usage). */
        if (metric != 0 && (v > 100 || v < 0)) continue;
        series_emit(out, ymd_midnight(days[i].ymd), v);
    }
    pthread_mutex_unlock(&days_mtx);
    /* Today's running total (not yet committed to the store). */
    if (today >= from_ymd && today <= to_ymd) {
        pthread_mutex_lock(&daily_mtx);
        double v = metric == 0 ? daily_net_kwh
                 : metric == 1 ? daily_gas_m3 : daily_water_m3;
        pthread_mutex_unlock(&daily_mtx);
        series_emit(out, ymd_midnight(today), v);
    }
    return out->n > 0 ? 0 : -1;
}
