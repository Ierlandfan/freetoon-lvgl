#ifndef TOON_SHELL_UTIL_H
#define TOON_SHELL_UTIL_H

/* Safe argument quoting for the popen()/system() calls toonui makes.
 *
 * Every string that originates from settings, the PWA, a remote server or a
 * plugin MUST go through sh_quote() before it is spliced into a shell command.
 * Pasting it between '...' by hand is not enough: a single ' in the value
 * closes the quote and whatever follows runs as root.
 *
 * sh_quote() wraps the value in single quotes and rewrites every embedded '
 * as '\'' — the only character that is special inside POSIX single quotes.
 * The result is always exactly one shell word, whatever the input contains.
 *
 * Returns `dst` on success. If the quoted form does not fit, `dst` is set to
 * '' (an empty word) and NULL is returned, so a truncated value can never
 * produce a half-quoted, injectable command. Callers that care check for NULL.
 *
 *   char q[300];
 *   snprintf(cmd, sizeof cmd, "curl -s %s", sh_quote(q, sizeof q, url));
 */

#include <stddef.h>
#include <string.h>

static inline const char * sh_quote(char * dst, size_t dstsz, const char * src) {
    if (!dst || dstsz < 3) return NULL;
    if (!src) src = "";
    size_t o = 0;
    dst[o++] = '\'';
    for (const char * p = src; *p; p++) {
        if (*p == '\'') {
            if (o + 4 >= dstsz) goto overflow;
            dst[o++] = '\''; dst[o++] = '\\'; dst[o++] = '\''; dst[o++] = '\'';
        } else {
            if (o + 1 >= dstsz) goto overflow;
            dst[o++] = *p;
        }
    }
    if (o + 2 > dstsz) goto overflow;
    dst[o++] = '\'';
    dst[o] = 0;
    return dst;
overflow:
    dst[0] = '\''; dst[1] = '\''; dst[2] = 0;
    return NULL;
}

/* 1 if `s` is a plain identifier: [A-Za-z0-9_-], 1..maxlen chars. Use for ids
 * that also become file names or inittab row keys (no '.', no '/'). */
static inline int sh_is_ident(const char * s, size_t maxlen) {
    if (!s || !*s) return 0;
    size_t n = 0;
    for (const char * p = s; *p; p++, n++) {
        char c = *p;
        if (n >= maxlen) return 0;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* 1 if `s` is a plausible host[:port] — letters, digits, '.', '-', ':' and
 * '[' ']' for IPv6 literals. Used to validate host settings when they are
 * written, so a bad value never reaches a URL or a command line. */
static inline int sh_is_hostport(const char * s) {
    if (!s) return 0;
    for (const char * p = s; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':' ||
              c == '[' || c == ']'))
            return 0;
    }
    return 1;
}

/* 1 if `s` starts with http:// or https:// and has no whitespace or control
 * characters. Quoting stops the shell interpreting a URL, but a value like
 * "-o/etc/inittab" would still be read by curl as an option — so URLs handed
 * to curl must also pass this. */
static inline int sh_is_http_url(const char * s) {
    if (!s) return 0;
    if (strncmp(s, "http://", 7) != 0 && strncmp(s, "https://", 8) != 0) return 0;
    for (const unsigned char * p = (const unsigned char *)s; *p; p++)
        if (*p <= 0x20 || *p == 0x7f) return 0;
    return 1;
}

#endif
