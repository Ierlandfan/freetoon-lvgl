#ifndef TOON_HTTP_H
#define TOON_HTTP_H

#include <stddef.h>

/* Fetch a URL via curl (Toon has /usr/bin/curl + libcurl with TLS).
   We popen curl instead of linking libcurl — keeps the cross-build
   self-contained and dodges header / ABI mismatch.
   Returns 0 on success, fills `out` with the response body up to
   `out_max-1` bytes. `out` is NUL-terminated. */
int http_fetch(const char * url, char * out, size_t out_max);

/* As http_fetch, but with a caller-chosen curl --max-time (seconds). Use a
   longer timeout for large payloads such as a full iCal feed. */
int http_fetch_to(const char * url, char * out, size_t out_max, int timeout_s);

/* As http_fetch, but uses a curl cookie jar (read+write) so a session cookie
   from a prior login authenticates the request. `jar` = writable file path. */
int http_fetch_cookies(const char * url, const char * jar, char * out, size_t out_max);

#endif
