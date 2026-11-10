/*
 * wastefetch — runs a ToonSoftwareCollective wastecollection provider script
 * (the exact community JS, downloaded per provider) inside QuickJS, so freetoon
 * mimics the stock Toon "Afvalkalender" app for ALL providers.
 *
 * Flow: download wastecollectionProvider_<id>.js → eval in QuickJS with a
 * synchronous XMLHttpRequest shim (GET → curl, PUT file:// → captured) →
 * call readCalendar(...) → the script writes "YYYY-MM-DD,<code>" lines (and an
 * ICS). We turn those into a clean ICS via the script's own wasteTypeFriendlyName()
 * and write it to the output path; toonui's parse_ics() reads it as usual.
 *
 *   wastefetch <providerId> <zip> <housenr> <ICSId> <street> <city> <fullICSUrl> <out.ics>
 *
 * Build: links quickjs core objects (quickjs/cutils/libregexp/libunicode).
 */
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "shell_util.h"

/* Re-download a cached provider script after this long, so upstream fixes
 * arrive and a bad copy doesn't live forever. The cache stays as fallback. */
#define PLUGIN_CACHE_MAX_AGE (7 * 24 * 3600)

#define PLUGIN_BASE "https://raw.githubusercontent.com/ToonSoftwareCollective/wastecollection_plugins/main/"

static char g_dates[64 * 1024];   /* captured wasteDates.txt (date,code lines) */
static char g_ics[128 * 1024];    /* captured wasteDates.ics (if the script makes one) */

/* --- shell out to curl for HTTP(S) GET; returns malloc'd body or NULL --- */
static char * http_get_ex(const char * url, int verify_tls) {
    /* Only allow http/https; the script also does file:// PUTs (handled separately).
     * The URL is built by the provider JS from API data, so it is quoted as a
     * single shell word — never pasted between '...' by hand. */
    if (!sh_is_http_url(url)) return NULL;
    char qurl[2400];
    if (!sh_quote(qurl, sizeof qurl, url)) return NULL;
    char cmd[2600];
    /* Provider API calls: -k, provider/CDN certs vary and the data is only
     * parsed. The provider *script* itself is code, so it is fetched with
     * verification (verify_tls=1). -L: follow redirects; bounded time. */
    snprintf(cmd, sizeof cmd,
             "curl -s %s-L --max-time 25 --connect-timeout 8 "
             "-A 'Mozilla/5.0' %s", verify_tls ? "" : "-k ", qurl);
    FILE * fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 65536, len = 0;
    char * buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }
    size_t n;
    while ((n = fread(buf + len, 1, cap - 1 - len, fp)) > 0) {
        len += n;
        if (len + 1 >= cap) { cap *= 2; char * nb = realloc(buf, cap); if (!nb) break; buf = nb; }
    }
    buf[len] = 0;
    pclose(fp);
    return buf;
}

static char * http_get(const char * url) { return http_get_ex(url, 0); }

/* Write `s` as the body of a JS double-quoted string literal. */
static size_t js_escape(char * dst, size_t dstsz, const char * s) {
    size_t o = 0;
    if (!dstsz) return 0;
    for (; s && *s && o + 7 < dstsz; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c < 0x20 || c == 0x7f) o += (size_t)snprintf(dst + o, dstsz - o, "\\u%04x", c);
        else dst[o++] = (char)c;
    }
    dst[o] = 0;
    return o;
}

/* JS global: __http_get(url) -> string|null */
static JSValue js_http_get(JSContext * ctx, JSValueConst this_val, int argc, JSValueConst * argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char * url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_NULL;
    char * body = http_get(url);
    JS_FreeCString(ctx, url);
    if (!body) return JS_NULL;
    JSValue r = JS_NewString(ctx, body);
    free(body);
    return r;
}

/* JS global: __file_put(url, body) -> capture wasteDates.txt / .ics */
static JSValue js_file_put(JSContext * ctx, JSValueConst this_val, int argc, JSValueConst * argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    const char * url  = JS_ToCString(ctx, argv[0]);
    const char * body = JS_ToCString(ctx, argv[1]);
    if (url && body) {
        if (strstr(url, ".ics"))
            snprintf(g_ics, sizeof g_ics, "%s", body);
        else if (strstr(url, "wasteDates") || strstr(url, ".txt"))
            snprintf(g_dates, sizeof g_dates, "%s", body);
    }
    if (url)  JS_FreeCString(ctx, url);
    if (body) JS_FreeCString(ctx, body);
    return JS_UNDEFINED;
}

/* Synchronous XMLHttpRequest implemented in JS over the two C globals above. */
static const char * XHR_PRELUDE =
    "function XMLHttpRequest(){this.readyState=0;this.status=0;this.responseText='';}"
    "XMLHttpRequest.DONE=4;"
    "XMLHttpRequest.prototype.open=function(m,u,a){this._m=m;this._u=u;};"
    "XMLHttpRequest.prototype.setRequestHeader=function(){};"
    "XMLHttpRequest.prototype.abort=function(){};"
    "XMLHttpRequest.prototype.getAllResponseHeaders=function(){return '';};"
    "XMLHttpRequest.prototype.send=function(body){"
    "  if(this._m==='PUT'||this._m==='POST'&&this._u.indexOf('file:')===0){__file_put(this._u,body||'');this.status=0;}"
    "  else if(this._u.indexOf('file:')===0){__file_put(this._u,body||'');this.status=0;}"
    "  else{var r=__http_get(this._u);this.responseText=(r===null?'':r);this.status=(r===null?0:200);}"
    "  this.readyState=4;"
    "  if(typeof this.onreadystatechange==='function')this.onreadystatechange();"
    "  if(typeof this.onload==='function')this.onload();"
    "};"
    /* QML-side callbacks the provider scripts reference — harmless no-op stubs. */
    "function updateWasteIcon(){};"
    "function updateWasteIcon2(){};"
    "var registry={registerWidget:function(){},unregisterWidget:function(){}};"
    "var iconLabelsJson={};"
    /* code -> friendly name, copied verbatim from the stock WastecollectionApp.qml */
    "function wasteTypeFriendlyName(c){switch(String(c)){"
    "case '0':return 'Restafval';case '1':return 'Plastic/Metaal/Drankpakken';"
    "case '2':return 'Papier/karton';case '3':return 'Groente/Fruit/Tuinafval';"
    "case '4':return 'Snoeiafval';case '5':return 'Textiel';case '6':return 'Steen en puin';"
    "case '7':return 'KGA';case '8':return 'Groot vuil';case '9':return 'Kunststoffen';"
    "case '!':return 'BEST tas';case '#':return 'Kerstboom';case 'z':return 'Reinigen container';"
    "case '?':return 'Onbekend';"
    "default:return (iconLabelsJson[c]!==undefined)?iconLabelsJson[c]:String(c);}};";

static int eval_str(JSContext * ctx, const char * code, const char * name) {
    JSValue v = JS_Eval(ctx, code, strlen(code), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char * s = JS_ToCString(ctx, e);
        fprintf(stderr, "[wastefetch] JS error in %s: %s\n", name, s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

int main(int argc, char ** argv) {
    if (argc < 9) {
        fprintf(stderr, "usage: %s <providerId> <zip> <housenr> <ICSId> <street> <city> <fullICSUrl> <out.ics>\n", argv[0]);
        return 2;
    }
    const char * pid = argv[1], * zip = argv[2], * nr = argv[3], * icsid = argv[4],
               * street = argv[5], * city = argv[6], * fullics = argv[7], * out = argv[8];

    /* 1) load provider script: check local cache in /mnt/data/waste_plugins/ first,
       otherwise download from PLUGIN_BASE and cache locally. */
    /* The provider id becomes a file name and part of a URL. */
    if (!sh_is_ident(pid, 32)) {
        fprintf(stderr, "[wastefetch] invalid provider id\n");
        return 2;
    }
    char local_path[256];
    snprintf(local_path, sizeof local_path, "/mnt/data/waste_plugins/wastecollectionProvider_%s.js", pid);
    char * script = NULL;
    /* Fresh download if the cache is missing or older than PLUGIN_CACHE_MAX_AGE;
     * the cached copy is the fallback when the download fails (offline). */
    struct stat cst;
    int cache_fresh = (stat(local_path, &cst) == 0) &&
                      (time(NULL) - cst.st_mtime) < PLUGIN_CACHE_MAX_AGE;
    if (!cache_fresh) {
        char url[512];
        snprintf(url, sizeof url, "%swastecollectionProvider_%s.js", PLUGIN_BASE, pid);
        script = http_get_ex(url, 1);
        if (script && strlen(script) >= 50) {
            mkdir("/mnt/data/waste_plugins", 0755);
            char tmp_path[280];
            snprintf(tmp_path, sizeof tmp_path, "%s.tmp", local_path);
            FILE * wf = fopen(tmp_path, "w");
            if (wf) {
                int ok = fputs(script, wf) >= 0;
                ok = (fclose(wf) == 0) && ok;
                if (ok) rename(tmp_path, local_path); else unlink(tmp_path);
            }
        } else if (script) { free(script); script = NULL; }
    }
    FILE * lf = script ? NULL : fopen(local_path, "r");
    if (lf) {
        fseek(lf, 0, SEEK_END);
        long sz = ftell(lf);
        fseek(lf, 0, SEEK_SET);
        if (sz > 50) {
            script = malloc(sz + 1);
            if (script) {
                size_t nr_bytes = fread(script, 1, sz, lf);
                script[nr_bytes] = 0;
            }
        }
        fclose(lf);
    }
    if (!script || strlen(script) < 50) {
        fprintf(stderr, "[wastefetch] could not load provider script for id %s\n", pid);
        free(script); return 1;
    }

    /* 2) QuickJS runtime + XHR shim + the provider script */
    JSRuntime * rt = JS_NewRuntime();
    JSContext * ctx = JS_NewContext(rt);
    JSValue glob = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, glob, "__http_get", JS_NewCFunction(ctx, js_http_get, "__http_get", 1));
    JS_SetPropertyStr(ctx, glob, "__file_put", JS_NewCFunction(ctx, js_file_put, "__file_put", 2));
    JS_FreeValue(ctx, glob);

    int rc = 1;
    if (eval_str(ctx, XHR_PRELUDE, "<xhr>") == 0 &&
        eval_str(ctx, script, "<provider>") == 0) {
        /* 3) call readCalendar(zip, nr, [], true, ICSId, street, streetName, city, fullICSUrl) */
        /* readCalendar(zip, housenr, extraDates, enableCreateICS, ICSId,
           street, streetName, city, fullICSUrl) — args quoted as JS strings. */
        /* Settings values become JS string literals: escape them, or a " in a
         * street name breaks (or rewrites) the call. */
        char e_zip[128], e_nr[64], e_ics[256], e_street[384], e_city[256], e_full[1024];
        js_escape(e_zip, sizeof e_zip, zip);
        js_escape(e_nr, sizeof e_nr, nr);
        js_escape(e_ics, sizeof e_ics, icsid);
        js_escape(e_street, sizeof e_street, street);
        js_escape(e_city, sizeof e_city, city);
        js_escape(e_full, sizeof e_full, fullics);
        char call[4096];
        snprintf(call, sizeof call,
            "readCalendar(\"%s\",\"%s\",[],true,\"%s\",\"%s\",\"%s\",\"%s\",\"%s\");",
            e_zip, e_nr, e_ics, e_street, e_street, e_city, e_full);
        if (eval_str(ctx, call, "<call>") == 0) {
            /* 4) prefer the captured ICS; else build one from date,code lines via
                  the script's own wasteTypeFriendlyName(). */
            FILE * f = fopen(out, "w");
            if (f) {
                fputs("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n", f);
                if (g_dates[0]) {
                    char * save = NULL;
                    for (char * line = strtok_r(g_dates, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                        if (strlen(line) < 11 || line[4] != '-') continue;
                        char date[11]; snprintf(date, sizeof date, "%.10s", line);
                        const char * code = line + 11;          /* after "YYYY-MM-DD," */
                        char jc[64], name[96] = "";
                        snprintf(jc, sizeof jc, "wasteTypeFriendlyName(\"%.1s\")", code);
                        JSValue nv = JS_Eval(ctx, jc, strlen(jc), "<name>", JS_EVAL_TYPE_GLOBAL);
                        const char * ns = JS_IsException(nv) ? NULL : JS_ToCString(ctx, nv);
                        snprintf(name, sizeof name, "%s", ns && ns[0] ? ns : code);
                        if (ns) JS_FreeCString(ctx, ns);
                        JS_FreeValue(ctx, nv);
                        fprintf(f, "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:%.4s%.2s%.2s\r\nSUMMARY:%s\r\nEND:VEVENT\r\n",
                                date, date + 5, date + 8, name);
                    }
                    rc = 0;
                } else if (g_ics[0]) {
                    fputs(g_ics, f);
                    rc = 0;
                }
                fputs("END:VCALENDAR\r\n", f);
                fclose(f);
            }
        }
    }
    free(script);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    fprintf(stderr, "[wastefetch] %s (dates=%zu ics=%zu) -> %s\n",
            rc == 0 ? "OK" : "FAILED", strlen(g_dates), strlen(g_ics), out);
    return rc;
}
