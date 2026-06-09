/*
 * Air-quality history recorder — see airhist.h. Samples toon_state.eco2/tvoc
 * every 5 minutes into a persisted ring buffer so the Stats screen can graph
 * CO2/TVOC (which the Toon's RRD doesn't log).
 */
#include "airhist.h"
#include "boxtalk.h"     /* toon_state */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define AH_CAP     2016            /* 7 days at one sample / 5 min */
#define AH_PERIOD  300             /* sample cadence, seconds */
#define AH_FILE    "/mnt/data/freetoon_airhist.bin"
#define AH_TMP     "/mnt/data/freetoon_airhist.bin.tmp"
#define AH_MAGIC   0x41483031u     /* "AH01" */

typedef struct { long ts; short eco2; short tvoc; } ah_sample_t;

static ah_sample_t ring[AH_CAP];
static int ah_head = 0;            /* next write slot */
static int ah_count = 0;
static pthread_mutex_t ah_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ---- CH water-pressure ring: hourly, ~400 days ---- */
#define PH_CAP    9600
#define PH_FILE   "/mnt/data/freetoon_preshist.bin"
#define PH_TMP    "/mnt/data/freetoon_preshist.bin.tmp"
#define PH_MAGIC  0x50483031u      /* "PH01" */
typedef struct { long ts; float bar; } ph_sample_t;
static ph_sample_t pring[PH_CAP];
static int ph_head = 0, ph_count = 0;
static pthread_mutex_t ph_mtx = PTHREAD_MUTEX_INITIALIZER;

static void ph_load(void) {
    FILE * f = fopen(PH_FILE, "rb"); if (!f) return;
    unsigned magic = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == PH_MAGIC &&
        fread(&ph_head, sizeof ph_head, 1, f) == 1 &&
        fread(&ph_count, sizeof ph_count, 1, f) == 1 &&
        fread(pring, sizeof pring, 1, f) == 1) {
        if (ph_count < 0 || ph_count > PH_CAP || ph_head < 0 || ph_head >= PH_CAP) {
            ph_head = 0; ph_count = 0; memset(pring, 0, sizeof pring);
        }
    }
    fclose(f);
}
static void ph_save(void) {
    FILE * f = fopen(PH_TMP, "wb"); if (!f) return;
    unsigned magic = PH_MAGIC;
    fwrite(&magic, 4, 1, f); fwrite(&ph_head, sizeof ph_head, 1, f);
    fwrite(&ph_count, sizeof ph_count, 1, f); fwrite(pring, sizeof pring, 1, f);
    fclose(f); rename(PH_TMP, PH_FILE);
}
static void ph_push(float bar) {
    pthread_mutex_lock(&ph_mtx);
    pring[ph_head].ts = (long)time(NULL); pring[ph_head].bar = bar;
    ph_head = (ph_head + 1) % PH_CAP; if (ph_count < PH_CAP) ph_count++;
    pthread_mutex_unlock(&ph_mtx);
}

static void ah_load(void) {
    FILE * f = fopen(AH_FILE, "rb");
    if (!f) return;
    unsigned magic = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == AH_MAGIC &&
        fread(&ah_head, sizeof ah_head, 1, f) == 1 &&
        fread(&ah_count, sizeof ah_count, 1, f) == 1 &&
        fread(ring, sizeof ring, 1, f) == 1) {
        if (ah_count < 0 || ah_count > AH_CAP || ah_head < 0 || ah_head >= AH_CAP) {
            ah_head = 0; ah_count = 0; memset(ring, 0, sizeof ring);
        }
    }
    fclose(f);
}

static void ah_save(void) {
    FILE * f = fopen(AH_TMP, "wb");
    if (!f) return;
    unsigned magic = AH_MAGIC;
    fwrite(&magic, 4, 1, f);
    fwrite(&ah_head, sizeof ah_head, 1, f);
    fwrite(&ah_count, sizeof ah_count, 1, f);
    fwrite(ring, sizeof ring, 1, f);
    fclose(f);
    rename(AH_TMP, AH_FILE);
}

static void ah_push(int eco2, int tvoc) {
    pthread_mutex_lock(&ah_mtx);
    ring[ah_head].ts   = (long)time(NULL);
    ring[ah_head].eco2 = (short)eco2;
    ring[ah_head].tvoc = (short)tvoc;
    ah_head = (ah_head + 1) % AH_CAP;
    if (ah_count < AH_CAP) ah_count++;
    pthread_mutex_unlock(&ah_mtx);
}

static void * ah_thread(void * arg) {
    (void)arg;
    ah_load();
    ph_load();
    sleep(40);   /* let BoxTalk populate the values before the first sample */
    int tick = 0;
    for (;;) {
        int e = toon_state.eco2, t = toon_state.tvoc;
        if (e > 0 || t > 0) { ah_push(e, t); ah_save(); }
        if (tick % 12 == 0) {                       /* pressure hourly (12 × 5 min) */
            float bar = toon_state.water_pressure;
            if (bar > 0.1f) { ph_push(bar); ph_save(); }
        }
        tick++;
        sleep(AH_PERIOD);
    }
    return NULL;
}

void airhist_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, ah_thread, NULL) == 0) pthread_detach(th);
}

int airhist_series(int which, long window_seconds, int max_samples, stats_series_t * out) {
    out->n = 0; out->min = 1e30; out->max = -1e30;
    if (max_samples > STATS_MAX_SAMPLES) max_samples = STATS_MAX_SAMPLES;
    long now = (long)time(NULL);
    long cutoff = window_seconds ? now - window_seconds : 0;
    pthread_mutex_lock(&ah_mtx);
    for (int i = 0; i < ah_count && out->n < max_samples; i++) {
        int idx = (ah_head - ah_count + i + 2 * AH_CAP) % AH_CAP;   /* oldest → newest */
        if (ring[idx].ts < cutoff) continue;
        double v = which ? ring[idx].tvoc : ring[idx].eco2;
        out->samples[out->n] = v;
        out->ts[out->n] = ring[idx].ts;
        struct tm tm; time_t tt = (time_t)ring[idx].ts; localtime_r(&tt, &tm);
        strftime(out->labels[out->n], sizeof out->labels[0], "%d-%m %H:%M", &tm);
        snprintf(out->year2[out->n], 3, "%02d", (tm.tm_year + 1900) % 100);
        if (v < out->min) out->min = v;
        if (v > out->max) out->max = v;
        out->n++;
    }
    pthread_mutex_unlock(&ah_mtx);
    return out->n > 0 ? 0 : -1;
}

int airhist_pres_series(long window_seconds, int max_samples, stats_series_t * out) {
    out->n = 0; out->min = 1e30; out->max = -1e30;
    if (max_samples > STATS_MAX_SAMPLES) max_samples = STATS_MAX_SAMPLES;
    if (max_samples < 1) max_samples = 1;
    long now = (long)time(NULL);
    long cutoff = window_seconds ? now - window_seconds : 0;
    pthread_mutex_lock(&ph_mtx);
    /* count in-window, then stride-downsample to fit max_samples (year/month). */
    int inwin = 0;
    for (int i = 0; i < ph_count; i++) {
        int idx = (ph_head - ph_count + i + 2 * PH_CAP) % PH_CAP;
        if (pring[idx].ts >= cutoff) inwin++;
    }
    int stride = (inwin > max_samples) ? (inwin + max_samples - 1) / max_samples : 1;
    int seen = 0;
    for (int i = 0; i < ph_count && out->n < max_samples; i++) {
        int idx = (ph_head - ph_count + i + 2 * PH_CAP) % PH_CAP;
        if (pring[idx].ts < cutoff) continue;
        if ((seen++ % stride) != 0) continue;
        double v = pring[idx].bar;
        out->samples[out->n] = v;
        out->ts[out->n] = pring[idx].ts;
        struct tm tm; time_t tt = (time_t)pring[idx].ts; localtime_r(&tt, &tm);
        strftime(out->labels[out->n], sizeof out->labels[0], "%d-%m %H:%M", &tm);
        snprintf(out->year2[out->n], 3, "%02d", (tm.tm_year + 1900) % 100);
        if (v < out->min) out->min = v;
        if (v > out->max) out->max = v;
        out->n++;
    }
    pthread_mutex_unlock(&ph_mtx);
    return out->n > 0 ? 0 : -1;
}
