#ifndef TOON_I18N_H
#define TOON_I18N_H

#include "settings.h"
#include <time.h>
#include <stdio.h>

typedef enum { LANG_NL = 0, LANG_EN = 1 } lang_t;

/* Return the Dutch or English string based on the current UI language.
 * Takes effect on the next UI build/refresh — no restart needed. */
static inline const char * tr(const char *nl, const char *en) {
    return settings.language == LANG_EN ? en : nl;
}

/* Localised date formatting. strftime() honours the C locale, which on the
 * Toon is plain "C" → always English day/month names regardless of the UI
 * language. So we build the date string ourselves from the language tables
 * instead of relying on %A/%B. Day name is capitalised to match the old
 * "Tuesday 16 June" look. tm_wday: 0=Sun..6=Sat; tm_mon: 0=Jan..11=Dec. */
static inline const char * i18n_day_long(int wday) {
    static const char * nl[7] = { "Zondag","Maandag","Dinsdag","Woensdag",
                                  "Donderdag","Vrijdag","Zaterdag" };
    static const char * en[7] = { "Sunday","Monday","Tuesday","Wednesday",
                                  "Thursday","Friday","Saturday" };
    if (wday < 0 || wday > 6) wday = 0;
    return settings.language == LANG_EN ? en[wday] : nl[wday];
}
static inline const char * i18n_month_long(int mon) {
    static const char * nl[12] = { "januari","februari","maart","april","mei",
        "juni","juli","augustus","september","oktober","november","december" };
    static const char * en[12] = { "January","February","March","April","May",
        "June","July","August","September","October","November","December" };
    if (mon < 0 || mon > 11) mon = 0;
    return settings.language == LANG_EN ? en[mon] : nl[mon];
}
static inline const char * i18n_day_short(int wday) {
    static const char * nl[7] = { "zo","ma","di","wo","do","vr","za" };
    static const char * en[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    if (wday < 0 || wday > 6) wday = 0;
    return settings.language == LANG_EN ? en[wday] : nl[wday];
}
static inline const char * i18n_month_short(int mon) {
    static const char * nl[12] = { "jan","feb","mrt","apr","mei","jun",
                                   "jul","aug","sep","okt","nov","dec" };
    static const char * en[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec" };
    if (mon < 0 || mon > 11) mon = 0;
    return settings.language == LANG_EN ? en[mon] : nl[mon];
}
/* "Dinsdag 16 juni" / "Tuesday 16 June" */
static inline void i18n_date_long(char *out, int n, const struct tm *tm) {
    snprintf(out, n, "%s %d %s", i18n_day_long(tm->tm_wday),
             tm->tm_mday, i18n_month_long(tm->tm_mon));
}
/* "di 16 jun" / "Tue 16 Jun" */
static inline void i18n_date_short(char *out, int n, const struct tm *tm) {
    snprintf(out, n, "%s %d %s", i18n_day_short(tm->tm_wday),
             tm->tm_mday, i18n_month_short(tm->tm_mon));
}

#endif
