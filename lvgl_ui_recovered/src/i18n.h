#ifndef TOON_I18N_H
#define TOON_I18N_H

#include "settings.h"

typedef enum { LANG_NL = 0, LANG_EN = 1 } lang_t;

/* Return the Dutch or English string based on the current UI language.
 * Takes effect on the next UI build/refresh — no restart needed. */
static inline const char * tr(const char *nl, const char *en) {
    return settings.language == LANG_EN ? en : nl;
}

#endif
