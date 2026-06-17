/*
 * calendar.c — upcoming-events agenda from Home Assistant (REST) and/or an
 * iCal (.ics) URL. Both sources are parsed into a common event list, merged,
 * de-duplicated, sorted by date+time, and trimmed to the next CAL_MAX events.
 *
 * Parsing is deliberately brittle/jq-free (string scanning), matching the rest
 * of the codebase — calendar payloads are regular enough.
 *
 * Times are normalised to the Toon's LOCAL wall-clock: a UTC value (trailing
 * 'Z', or an explicit +HH:MM/-HH:MM offset on the HA side) is converted to
 * local time before display. iCal recurrence is expanded: RRULE
 * (FREQ/INTERVAL/COUNT/UNTIL/BYDAY/BYMONTHDAY/BYMONTH/BYSETPOS) occurrences in
 * the next ~month are generated, EXDATEs are removed, and a RECURRENCE-ID
 * override (a moved single occurrence) replaces its original slot.
 */
#define _GNU_SOURCE
#include "calendar.h"
#include "settings.h"
#include "http.h"
#include "homeassistant.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

calendar_state_t calendar_state = {0};

#define CAL_SCRATCH (4 * 1024 * 1024)  /* iCal feeds carry full history+recurrence — can be >1 MB */
#define CAL_TMP_MAX 256                 /* parse capacity before trim to CAL_MAX */
#define CAL_HORIZON_DAYS 31             /* how far ahead recurrences are expanded */

/* ------------------------------------------------------------------ time math */

/* A wall-clock instant pulled from a feed, plus how to interpret it. */
typedef struct {
    int y, mo, d, h, mi;
    int has_time;       /* 0 = DATE (all-day), 1 = has HH:MM */
    int utc;            /* value carried a 'Z' suffix → it's UTC */
} ics_dt_t;

static int atoin(const char * s, int n) {
    int v = 0;
    for (int i = 0; i < n && isdigit((unsigned char)s[i]); i++) v = v * 10 + (s[i] - '0');
    return v;
}

/* Howard Hinnant's days<->civil (proleptic Gregorian, days since 1970-01-01). */
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468;
}
static void civil_from_days(long z, int * y, int * m, int * d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = (int)yoe + (int)era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp + (mp < 10 ? 3 : -9);
    *y = yy + (mm <= 2);
    *m = (int)mm;
    *d = (int)dd;
}
/* 0=Sunday .. 6=Saturday */
static int weekday_from_days(long z) { return (int)(((z % 7) + 7 + 4) % 7); }
static int days_in_month(int y, int m) {
    int mm = m + 1, yy = y;
    if (mm > 12) { mm = 1; yy++; }
    return (int)(days_from_civil(yy, mm, 1) - days_from_civil(y, m, 1));
}

/* Parse "YYYYMMDD" or "YYYYMMDDThhmmss[Z]" (leading junk skipped). */
static int ics_parse_dt(const char * v, const char * end, ics_dt_t * o) {
    while (v < end && *v == ' ') v++;
    char buf[20]; int k = 0, started = 0;
    for (const char * r = v; r < end && *r != '\r' && *r != '\n' && *r != ',' && k < 19; r++) {
        if (isdigit((unsigned char)*r) || *r == 'T' || *r == 'Z') { buf[k++] = *r; started = 1; }
        else if (started) break;
    }
    buf[k] = 0;
    if (k < 8 || !isdigit((unsigned char)buf[0])) return -1;
    memset(o, 0, sizeof *o);
    o->y = atoin(buf, 4); o->mo = atoin(buf + 4, 2); o->d = atoin(buf + 6, 2);
    if (k >= 13 && buf[8] == 'T') {
        o->has_time = 1; o->h = atoin(buf + 9, 2); o->mi = atoin(buf + 11, 2);
        if (strchr(buf, 'Z')) o->utc = 1;
    }
    return 0;
}

/* UTC → local. timegm() builds the UTC instant; localtime_r() re-expresses it
 * in the Toon's timezone (so 04:00Z becomes 06:00 in CEST). No-op for all-day
 * dates and for values that were already local. */
static void ics_dt_to_local(ics_dt_t * d) {
    if (!d->utc || !d->has_time) return;
    struct tm tm; memset(&tm, 0, sizeof tm);
    tm.tm_year = d->y - 1900; tm.tm_mon = d->mo - 1; tm.tm_mday = d->d;
    tm.tm_hour = d->h; tm.tm_min = d->mi;
    time_t t = timegm(&tm);
    struct tm lo; localtime_r(&t, &lo);
    d->y = lo.tm_year + 1900; d->mo = lo.tm_mon + 1; d->d = lo.tm_mday;
    d->h = lo.tm_hour; d->mi = lo.tm_min; d->utc = 0;
}

/* ------------------------------------------------------------------ RRULE */

typedef struct {
    int freq;                       /* 0 none, 1 DAILY, 2 WEEKLY, 3 MONTHLY, 4 YEARLY */
    int interval;
    int count;                      /* 0 = unset */
    int has_until; ics_dt_t until;
    int byday_n;  int byday_ord[16]; int byday_wd[16];   /* ord 0 = no ordinal */
    int bymonthday_n; int bymonthday[16];                /* may be negative */
    int bymonth_n;    int bymonth[16];
    int bysetpos_n;   int bysetpos[8];
} rrule_t;

/* SU,MO,TU,WE,TH,FR,SA → 0..6 (Sun-based, matches weekday_from_days). */
static int wd_from_ics(const char * s) {
    static const char * nm[7] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
    for (int i = 0; i < 7; i++) if (!strncmp(s, nm[i], 2)) return i;
    return -1;
}
static void parse_int_list(char * val, int * arr, int * n, int cap) {
    char * p = val, * c;
    while (*p && *n < cap) {
        c = p; while (*c && *c != ',') c++;
        char save = *c; *c = 0;
        arr[(*n)++] = atoi(p);
        if (save) p = c + 1; else break;
    }
}
static void parse_rrule(const char * v, const char * end, rrule_t * o) {
    memset(o, 0, sizeof *o); o->interval = 1;
    char line[256]; int k = 0;
    while (v < end && *v != '\r' && *v != '\n' && k < 255) line[k++] = *v++;
    line[k] = 0;
    for (char * tok = strtok(line, ";"); tok; tok = strtok(NULL, ";")) {
        char * eq = strchr(tok, '='); if (!eq) continue;
        *eq = 0; char * key = tok, * val = eq + 1;
        if      (!strcmp(key, "FREQ")) {
            if      (!strcmp(val, "DAILY"))   o->freq = 1;
            else if (!strcmp(val, "WEEKLY"))  o->freq = 2;
            else if (!strcmp(val, "MONTHLY")) o->freq = 3;
            else if (!strcmp(val, "YEARLY"))  o->freq = 4;
        }
        else if (!strcmp(key, "INTERVAL")) { o->interval = atoi(val); if (o->interval < 1) o->interval = 1; }
        else if (!strcmp(key, "COUNT"))    { o->count = atoi(val); }
        else if (!strcmp(key, "UNTIL"))    { if (ics_parse_dt(val, val + strlen(val), &o->until) == 0) o->has_until = 1; }  /* reference frame */
        else if (!strcmp(key, "BYDAY")) {
            char * p = val, * c;
            while (*p && o->byday_n < 16) {
                c = p; while (*c && *c != ',') c++;
                char save = *c; *c = 0;
                int sign = 1, ord = 0; char * q = p;
                if (*q == '+') q++; else if (*q == '-') { sign = -1; q++; }
                while (isdigit((unsigned char)*q)) { ord = ord * 10 + (*q - '0'); q++; }
                int wd = wd_from_ics(q);
                if (wd >= 0) { o->byday_ord[o->byday_n] = sign * ord; o->byday_wd[o->byday_n] = wd; o->byday_n++; }
                if (save) p = c + 1; else break;
            }
        }
        else if (!strcmp(key, "BYMONTHDAY")) parse_int_list(val, o->bymonthday, &o->bymonthday_n, 16);
        else if (!strcmp(key, "BYMONTH"))    parse_int_list(val, o->bymonth,    &o->bymonth_n,    16);
        else if (!strcmp(key, "BYSETPOS"))   parse_int_list(val, o->bysetpos,   &o->bysetpos_n,   8);
    }
}

static int wd_in(const rrule_t * rr, int wd) {
    for (int i = 0; i < rr->byday_n; i++) if (rr->byday_wd[i] == wd) return 1;
    return 0;
}
static void sortl(long * a, int n) {
    for (int i = 1; i < n; i++) { long v = a[i]; int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
}
static void apply_setpos(long * cand, int * cn, const rrule_t * rr) {
    if (!rr->bysetpos_n) return;
    long out[64]; int on = 0;
    for (int i = 0; i < rr->bysetpos_n; i++) {
        int p = rr->bysetpos[i];
        int idx = p > 0 ? p - 1 : *cn + p;       /* negatives count from the end */
        if (idx >= 0 && idx < *cn && on < 64) out[on++] = cand[idx];
    }
    sortl(out, on);
    for (int i = 0; i < on; i++) cand[i] = out[i];
    *cn = on;
}
/* Candidate day-numbers within one (yy,mm), before BYSETPOS. */
static int month_cands(const rrule_t * rr, int yy, int mm, const ics_dt_t * ds, long * cand) {
    int cn = 0, dim = days_in_month(yy, mm);
    if (rr->bymonthday_n) {
        for (int i = 0; i < rr->bymonthday_n; i++) {
            int d = rr->bymonthday[i]; if (d < 0) d = dim + d + 1;
            if (d >= 1 && d <= dim) cand[cn++] = days_from_civil(yy, mm, d);
        }
    } else if (rr->byday_n) {
        long first = days_from_civil(yy, mm, 1); int fw = weekday_from_days(first);
        for (int i = 0; i < rr->byday_n; i++) {
            int wd = rr->byday_wd[i], ord = rr->byday_ord[i];
            int firstdom = 1 + ((wd - fw) + 7) % 7;
            int occ[6], oc = 0;
            for (int dd = firstdom; dd <= dim; dd += 7) occ[oc++] = dd;
            if (ord == 0)              for (int j = 0; j < oc; j++) cand[cn++] = days_from_civil(yy, mm, occ[j]);
            else if (ord > 0 && ord <= oc)        cand[cn++] = days_from_civil(yy, mm, occ[ord - 1]);
            else if (ord < 0 && -ord <= oc)       cand[cn++] = days_from_civil(yy, mm, occ[oc + ord]);
        }
    } else {
        if (ds->d >= 1 && ds->d <= dim) cand[cn++] = days_from_civil(yy, mm, ds->d);
    }
    return cn;
}

/* ------------------------------------------------------------------ events */

/* time field for compare: empty (all-day) sorts to start of day. */
static const char * cmp_time(const char * t) { return t[0] ? t : "00:00"; }

static int ev_cmp(const void * a, const void * b) {
    const calendar_event_t * x = a, * y = b;
    int d = strcmp(x->date, y->date);
    if (d) return d;
    return strcmp(cmp_time(x->time), cmp_time(y->time));
}

/* Current local HH:MM (named to avoid shadowing the `time` parameter below). */
static void cal_now_hhmm(char * out, size_t n) {
    time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
    strftime(out, n, "%H:%M", &lt);
}

/* Append one event to tmp[] if it's today-or-future and not a duplicate. */
static void add_event(calendar_event_t * tmp, int * n, const char * today,
                      const char * date, const char * time, const char * summary) {
    if (!date[0] || !summary[0]) return;
    if (strcmp(date, today) < 0) return;            /* past day */
    if (time[0] && !strcmp(date, today)) {          /* timed event already finished earlier today */
        char now[6]; cal_now_hhmm(now, sizeof now);
        if (strcmp(time, now) < 0) return;
    }
    for (int i = 0; i < *n; i++)                     /* dedup (HA + iCal overlap) */
        if (!strcmp(tmp[i].date, date) && !strcmp(tmp[i].time, time) &&
            !strcmp(tmp[i].summary, summary)) return;
    if (*n >= CAL_TMP_MAX) return;
    calendar_event_t * e = &tmp[*n];
    snprintf(e->date, sizeof e->date, "%s", date);
    snprintf(e->time, sizeof e->time, "%s", time);
    snprintf(e->summary, sizeof e->summary, "%s", summary);
    (*n)++;
}

/* Copy a JSON/ICS string value, unescaping the common ICS/JSON escapes. */
static void copy_text(const char * src, char * out, size_t osz) {
    size_t n = 0;
    while (*src && *src != '"' && *src != '\r' && *src != '\n' && n + 1 < osz) {
        if (*src == '\\' && src[1]) {
            src++;
            out[n++] = (*src == 'n' || *src == 'N') ? ' ' : *src;
            src++;
        } else out[n++] = *src++;
    }
    out[n] = 0;
}

/* ---- Home Assistant: array of {"start":{"dateTime"|"date":..},"summary":..} */
static void parse_ha(const char * body, calendar_event_t * tmp, int * n, const char * today) {
    const char * p = body;
    const char * s;
    while ((s = strstr(p, "\"start\"")) != NULL) {
        const char * next = strstr(s + 7, "\"start\"");
        const char * wend = next ? next : s + strlen(s);

        char date[12] = "", tm[8] = "";
        const char * dt = strstr(s, "\"dateTime\"");
        if (dt && dt < wend) {
            const char * c = strchr(dt, ':');
            if (c) { c++; while (*c == ' ' || *c == '"') c++;
                if (strlen(c) >= 16 && c[4] == '-' && c[10] == 'T') {
                    /* Parse wall-clock fields, then apply any zone (Z / ±HH:MM). */
                    ics_dt_t d; memset(&d, 0, sizeof d);
                    d.y = atoin(c, 4); d.mo = atoin(c + 5, 2); d.d = atoin(c + 8, 2);
                    d.h = atoin(c + 11, 2); d.mi = atoin(c + 14, 2); d.has_time = 1;
                    const char * z = c + 16;                 /* past HH:MM[:SS] region */
                    while (*z && *z != 'Z' && *z != '+' && *z != '-' &&
                           *z != '"' && *z != '\r' && *z != '\n') z++;
                    if (*z == 'Z') { d.utc = 1; ics_dt_to_local(&d); }
                    else if (*z == '+' || *z == '-') {
                        int sgn = (*z == '-') ? -1 : 1;
                        int oh = atoin(z + 1, 2), om = atoin(z + 4, 2);
                        /* shift wall-clock by -offset to get UTC, then to local */
                        struct tm tm0; memset(&tm0, 0, sizeof tm0);
                        tm0.tm_year = d.y - 1900; tm0.tm_mon = d.mo - 1; tm0.tm_mday = d.d;
                        tm0.tm_hour = d.h; tm0.tm_min = d.mi - sgn * (oh * 60 + om);
                        time_t t = timegm(&tm0); struct tm lo; localtime_r(&t, &lo);
                        d.y = lo.tm_year + 1900; d.mo = lo.tm_mon + 1; d.d = lo.tm_mday;
                        d.h = lo.tm_hour; d.mi = lo.tm_min;
                    }
                    snprintf(date, sizeof date, "%04d-%02d-%02d", d.y, d.mo, d.d);
                    snprintf(tm, sizeof tm, "%02d:%02d", d.h, d.mi);
                } }
        } else {
            const char * d = strstr(s, "\"date\"");
            if (d && d < wend) {
                const char * c = strchr(d, ':');
                if (c) { c++; while (*c == ' ' || *c == '"') c++;
                    if (strlen(c) >= 10 && c[4] == '-') snprintf(date, sizeof date, "%.10s", c); }
            }
        }
        char summary[80] = "";
        const char * sm = strstr(s, "\"summary\"");
        if (sm && sm < wend) {
            const char * c = strchr(sm, ':');
            if (c) { c++; while (*c == ' ') c++; if (*c == '"') c++; copy_text(c, summary, sizeof summary); }
        }
        add_event(tmp, n, today, date, tm, summary);
        p = wend;
    }
}

/* ---- iCal: one parsed VEVENT */
typedef struct {
    ics_dt_t ds;   int has_ds;
    rrule_t  rr;
    char     summary[80];
    char     uid[96];
    ics_dt_t recid; int has_recid;     /* RECURRENCE-ID → this is an override */
    char     ex[64][24]; int exn;      /* EXDATE keys "YYYY-MM-DD HH:MM" */
} vevent_t;

static int line_is(const char * q, const char * le, const char * name) {
    size_t nl = strlen(name);
    return (size_t)(le - q) > nl && !strncmp(q, name, nl) && (q[nl] == ':' || q[nl] == ';');
}

static void gather_event(const char * p, const char * evend, vevent_t * ev) {
    memset(ev, 0, sizeof *ev); ev->rr.interval = 1;
    const char * q = p;
    while (q < evend) {
        const char * eol = q; while (eol < evend && *eol != '\n') eol++;
        const char * le = eol; if (le > q && le[-1] == '\r') le--;        /* logical end */
        const char * val; { const char * c = q; while (c < le && *c != ':') c++; val = (c < le) ? c + 1 : le; }

        /* NOTE: times are kept in their ORIGINAL (reference) frame here — NOT
         * converted to local. Recurrence math, EXDATE matching and
         * RECURRENCE-ID matching all happen in that frame so they line up
         * exactly regardless of DST; conversion to local is done per-occurrence
         * at display time (see occ_local / dt_local_str). */
        if      (line_is(q, le, "DTSTART"))       { if (ics_parse_dt(val, le, &ev->ds) == 0)    ev->has_ds = 1; }
        else if (line_is(q, le, "SUMMARY"))       { copy_text(val, ev->summary, sizeof ev->summary); }
        else if (line_is(q, le, "UID"))           { copy_text(val, ev->uid, sizeof ev->uid); }
        else if (line_is(q, le, "RRULE"))         { parse_rrule(val, le, &ev->rr); }
        else if (line_is(q, le, "RECURRENCE-ID")) { if (ics_parse_dt(val, le, &ev->recid) == 0) ev->has_recid = 1; }
        else if (line_is(q, le, "EXDATE")) {
            const char * c = val;
            while (c < le && ev->exn < 64) {
                ics_dt_t e;
                if (ics_parse_dt(c, le, &e) == 0) {
                    char t2[8] = "";
                    if (e.has_time) snprintf(t2, sizeof t2, "%02d:%02d", e.h, e.mi);   /* reference time */
                    snprintf(ev->ex[ev->exn], sizeof ev->ex[ev->exn], "%04d-%02d-%02d %s", e.y, e.mo, e.d, t2);
                    ev->exn++;
                }
                while (c < le && *c != ',') c++;
                if (c < le) c++;
            }
        }
        q = (eol < evend) ? eol + 1 : evend;
    }
}

/* Format a single instant in LOCAL time for display (point events/overrides). */
static void dt_local_str(const ics_dt_t * src, char * date, size_t dz, char * tm, size_t tz) {
    ics_dt_t d = *src; ics_dt_to_local(&d);
    snprintf(date, dz, "%04d-%02d-%02d", d.y, d.mo, d.d);
    if (d.has_time) snprintf(tm, tz, "%02d:%02d", d.h, d.mi); else tm[0] = 0;
}

/* Convert a recurrence occurrence (reference-frame day `dn` + the series'
 * reference time) to a LOCAL display date+time. A UTC-anchored series shifts
 * with DST (10:00Z → 11:00 in winter, 12:00 in summer); a floating/all-day
 * series is shown verbatim. */
static void occ_local(long dn, const ics_dt_t * ds, char * date, size_t dz, char * tm, size_t tz) {
    int y, mo, d; civil_from_days(dn, &y, &mo, &d);
    if (ds->has_time && ds->utc) {
        struct tm t0; memset(&t0, 0, sizeof t0);
        t0.tm_year = y - 1900; t0.tm_mon = mo - 1; t0.tm_mday = d;
        t0.tm_hour = ds->h; t0.tm_min = ds->mi;
        time_t tt = timegm(&t0); struct tm lo; localtime_r(&tt, &lo);
        snprintf(date, dz, "%04d-%02d-%02d", lo.tm_year + 1900, lo.tm_mon + 1, lo.tm_mday);
        snprintf(tm, tz, "%02d:%02d", lo.tm_hour, lo.tm_min);
    } else {
        snprintf(date, dz, "%04d-%02d-%02d", y, mo, d);
        if (ds->has_time) snprintf(tm, tz, "%02d:%02d", ds->h, ds->mi); else tm[0] = 0;
    }
}

/* Record one expanded occurrence (reference-frame day-number `dn`), unless
 * excluded by EXDATE or superseded by a RECURRENCE-ID override for the same
 * UID. Exclusion keys are built in the REFERENCE frame (so they match the raw
 * EXDATE/RECURRENCE-ID values exactly); the event itself is stored in LOCAL
 * time. `horizon` bounds the window after the tz shift. */
static void rec_occ(long dn, const vevent_t * ev, calendar_event_t * tmp, int * n,
                    const char * today, const char * horizon, char ovr[][128], int ovrn) {
    int ry, rmo, rd; civil_from_days(dn, &ry, &rmo, &rd);
    char rtm[8] = "";
    if (ev->ds.has_time) snprintf(rtm, sizeof rtm, "%02d:%02d", ev->ds.h, ev->ds.mi);

    char k1[24]; snprintf(k1, sizeof k1, "%04d-%02d-%02d %s", ry, rmo, rd, rtm);   /* reference-frame */
    for (int i = 0; i < ev->exn; i++) if (!strcmp(ev->ex[i], k1)) return;
    char k2[160]; snprintf(k2, sizeof k2, "%s\t%s", ev->uid, k1);
    for (int i = 0; i < ovrn; i++) if (!strcmp(ovr[i], k2)) return;

    char date[12], tm[8];
    occ_local(dn, &ev->ds, date, sizeof date, tm, sizeof tm);
    if (horizon[0] && strcmp(date, horizon) > 0) return;     /* fell past the window after tz shift */
    add_event(tmp, n, today, date, tm, ev->summary);
}

/* Expand a recurring VEVENT over [today, today+CAL_HORIZON_DAYS]. */
static void expand_rrule(const vevent_t * ev, calendar_event_t * tmp, int * n,
                         const char * today, char ovr[][128], int ovrn) {
    const rrule_t * rr = &ev->rr;
    const ics_dt_t * ds = &ev->ds;
    time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
    long today_dn   = days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    long horizon_dn = today_dn + CAL_HORIZON_DAYS;
    long ds_dn      = days_from_civil(ds->y, ds->mo, ds->d);
    long until_dn   = rr->has_until ? days_from_civil(rr->until.y, rr->until.mo, rr->until.d) : 0;
    int emitted = 0, done = 0, iter = 0;
    /* exact local upper bound for rec_occ; the dn gates below are widened ±1 day
     * because a reference-frame date can shift a calendar day once converted. */
    char horizon[12]; { int hy, hmo, hd; civil_from_days(horizon_dn, &hy, &hmo, &hd);
                        snprintf(horizon, sizeof horizon, "%04d-%02d-%02d", hy, hmo, hd); }

    if (rr->freq == 1) {                            /* DAILY */
        long k = 0;
        if (!rr->count) { long kk = (today_dn - ds_dn) / rr->interval - 1; if (kk > 0) k = kk; }
        for (; iter < 40000 && !done; k++, iter++) {
            long dn = ds_dn + k * (long)rr->interval;
            if (dn < ds_dn) continue;
            if (dn > horizon_dn && !rr->count) break;
            if (rr->has_until && dn > until_dn) break;
            if (rr->byday_n && !wd_in(rr, weekday_from_days(dn))) continue;
            if (rr->count && emitted >= rr->count) break;
            emitted++;
            if (dn >= today_dn - 1 && dn <= horizon_dn + 1) rec_occ(dn, ev, tmp, n, today, horizon, ovr, ovrn);
        }
    } else if (rr->freq == 2) {                     /* WEEKLY */
        int dsw = weekday_from_days(ds_dn);
        long ds_week0 = ds_dn - ((dsw + 6) % 7);    /* Monday of DTSTART's week */
        int wlist[8], wn = 0;
        if (rr->byday_n) { for (int i = 0; i < rr->byday_n && i < 8; i++) wlist[wn++] = rr->byday_wd[i]; }
        else wlist[wn++] = dsw;
        long w = 0;
        if (!rr->count) { long weeks = (today_dn - ds_week0) / 7; long w0 = weeks / rr->interval - 1; if (w0 > 0) w = w0; }
        for (; iter < 40000 && !done; w++, iter++) {
            long wk0 = ds_week0 + w * (long)rr->interval * 7;
            if (wk0 > horizon_dn && !rr->count) break;
            long cand[8]; int cn = 0;
            for (int i = 0; i < wn; i++) cand[cn++] = wk0 + ((wlist[i] + 6) % 7);
            sortl(cand, cn); apply_setpos(cand, &cn, rr);
            for (int i = 0; i < cn; i++) {
                long dn = cand[i];
                if (dn < ds_dn) continue;
                if (rr->has_until && dn > until_dn) { done = 1; break; }
                if (rr->count && emitted >= rr->count) { done = 1; break; }
                emitted++;
                if (dn >= today_dn - 1 && dn <= horizon_dn + 1) rec_occ(dn, ev, tmp, n, today, horizon, ovr, ovrn);
            }
        }
    } else if (rr->freq == 3) {                     /* MONTHLY */
        int basem = ds->y * 12 + (ds->mo - 1);
        long m = 0;
        if (!rr->count) { int months = (lt.tm_year + 1900) * 12 + lt.tm_mon - basem; long m0 = months / rr->interval - 1; if (m0 > 0) m = m0; }
        for (; iter < 6000 && !done; m++, iter++) {
            int total = basem + (int)(m * rr->interval);
            int yy = total / 12, mm = total % 12 + 1;
            if (days_from_civil(yy, mm, 1) > horizon_dn && !rr->count) break;
            long cand[64]; int cn = month_cands(rr, yy, mm, ds, cand);
            sortl(cand, cn); apply_setpos(cand, &cn, rr);
            for (int i = 0; i < cn; i++) {
                long dn = cand[i];
                if (dn < ds_dn) continue;
                if (rr->has_until && dn > until_dn) { done = 1; break; }
                if (rr->count && emitted >= rr->count) { done = 1; break; }
                emitted++;
                if (dn >= today_dn - 1 && dn <= horizon_dn + 1) rec_occ(dn, ev, tmp, n, today, horizon, ovr, ovrn);
            }
        }
    } else if (rr->freq == 4) {                     /* YEARLY */
        long yidx = 0;
        if (!rr->count) { long yrs = (lt.tm_year + 1900) - ds->y; long y0 = yrs / rr->interval - 1; if (y0 > 0) yidx = y0; }
        for (; iter < 2000 && !done; yidx++, iter++) {
            int yy = ds->y + (int)(yidx * rr->interval);
            if (days_from_civil(yy, 1, 1) > horizon_dn && !rr->count) break;
            int months[16], mn = 0;
            if (rr->bymonth_n) { for (int i = 0; i < rr->bymonth_n; i++) months[mn++] = rr->bymonth[i]; }
            else months[mn++] = ds->mo;
            long cand[128]; int cn = 0;
            for (int mi = 0; mi < mn; mi++) {
                long tc[64]; int tn = month_cands(rr, yy, months[mi], ds, tc);
                for (int j = 0; j < tn && cn < 128; j++) cand[cn++] = tc[j];
            }
            sortl(cand, cn); apply_setpos(cand, &cn, rr);
            for (int i = 0; i < cn; i++) {
                long dn = cand[i];
                if (dn < ds_dn) continue;
                if (rr->has_until && dn > until_dn) { done = 1; break; }
                if (rr->count && emitted >= rr->count) { done = 1; break; }
                emitted++;
                if (dn >= today_dn - 1 && dn <= horizon_dn + 1) rec_occ(dn, ev, tmp, n, today, horizon, ovr, ovrn);
            }
        }
    }
}

/* ---- iCal: walk VEVENTs. Two passes: collect RECURRENCE-ID overrides first
 * (so the master series can suppress the moved occurrence), then emit. */
static void parse_ics_cal(const char * body, calendar_event_t * tmp, int * n, const char * today) {
    static char ovr[32][128]; int ovrn = 0;

    const char * p = body;
    while ((p = strstr(p, "BEGIN:VEVENT")) != NULL) {
        const char * evend = strstr(p, "END:VEVENT"); if (!evend) evend = p + strlen(p);
        vevent_t ev; gather_event(p, evend, &ev);
        if (ev.has_recid && ovrn < 32) {
            char t2[8] = "";
            if (ev.recid.has_time) snprintf(t2, sizeof t2, "%02d:%02d", ev.recid.h, ev.recid.mi);
            snprintf(ovr[ovrn], sizeof ovr[ovrn], "%s\t%04d-%02d-%02d %s",
                     ev.uid, ev.recid.y, ev.recid.mo, ev.recid.d, t2);
            ovrn++;
        }
        p = evend;
    }

    p = body;
    while ((p = strstr(p, "BEGIN:VEVENT")) != NULL) {
        const char * evend = strstr(p, "END:VEVENT"); if (!evend) evend = p + strlen(p);
        vevent_t ev; gather_event(p, evend, &ev);
        if (ev.has_ds && ev.summary[0]) {
            if (ev.rr.freq && !ev.has_recid) {
                expand_rrule(&ev, tmp, n, today, ovr, ovrn);
            } else {
                /* point event or moved single occurrence — convert to local for display */
                char date[12], tm[8];
                dt_local_str(&ev.ds, date, sizeof date, tm, sizeof tm);
                add_event(tmp, n, today, date, tm, ev.summary);
            }
        }
        p = evend;
    }
}

static void today_str(char * out, size_t n)   { time_t t=time(NULL); struct tm tm; localtime_r(&t,&tm);
    strftime(out, n, "%Y-%m-%d", &tm); }
static void iso_at(char * out, size_t n, int days_from_now) {
    time_t t = time(NULL) + (time_t)days_from_now * 86400;
    struct tm tm; localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT00:00:00Z", &tm);
}

void calendar_refresh_now(void) {
    if (!settings.calendar_enabled) { calendar_state.count = 0; calendar_state.connected = 0; return; }
    /* Heap, not BSS — the iCal scratch is multi-MB now (full feeds). Allocated
     * per refresh (every 30 min) and freed at the end. */
    char * buf = malloc(CAL_SCRATCH);
    if (!buf) return;
    static calendar_event_t tmp[CAL_TMP_MAX];
    int n = 0, ok = 0;
    char today[12]; today_str(today, sizeof today);

    if (settings.calendar_ha_entity[0]) {
        char start[24], end[24];
        iso_at(start, sizeof start, 0);
        iso_at(end, sizeof end, 31);
        if (ha_fetch_calendar(settings.calendar_ha_entity, start, end, buf, CAL_SCRATCH) == 0) {
            parse_ha(buf, tmp, &n, today); ok = 1;
        }
    }
    if (settings.calendar_ics_url[0]) {
        /* 20 s — a full year-of-history .ics can be a megabyte or two. */
        if (http_fetch_to(settings.calendar_ics_url, buf, CAL_SCRATCH, 20) == 0 && strstr(buf, "VEVENT")) {
            parse_ics_cal(buf, tmp, &n, today); ok = 1;
        }
    }

    qsort(tmp, n, sizeof tmp[0], ev_cmp);
    if (n > CAL_MAX) n = CAL_MAX;
    for (int i = 0; i < n; i++) calendar_state.ev[i] = tmp[i];
    calendar_state.count = n;
    if (ok) calendar_state.connected = 1;
    free(buf);
}

/* One-shot refresh on a detached thread — safe to call from the LVGL/UI thread
 * (the fetch does blocking curl I/O and must never run on the UI thread). */
static void * kick_thread(void * arg) { (void)arg; calendar_refresh_now(); return NULL; }
void calendar_refresh_async(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, kick_thread, NULL) == 0) pthread_detach(t);
}

static void * cal_thread(void * arg) {
    (void)arg;
    for (;;) {
        calendar_refresh_now();
        sleep(1800);                    /* 30 min — calendars change slowly */
    }
    return NULL;
}

int calendar_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, cal_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
