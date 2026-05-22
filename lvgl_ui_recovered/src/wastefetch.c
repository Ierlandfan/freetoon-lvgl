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

#define PLUGIN_BASE "https://raw.githubusercontent.com/ToonSoftwareCollective/wastecollection_plugins/main/"

static char g_dates[64 * 1024];   /* captured wasteDates.txt (date,code lines) */
static char g_ics[128 * 1024];    /* captured wasteDates.ics (if the script makes one) */

/* --- shell out to curl for HTTP(S) GET; returns malloc'd body or NULL --- */
static char * http_get(const char * url) {
    /* Only allow http/https; the script also does file:// PUTs (handled separately). */
    if (strncmp(url, "http", 4) != 0) return NULL;
    char cmd[2048];
    /* -k: provider/CDN certs vary; -L: follow redirects; bounded time. */
    snprintf(cmd, sizeof cmd,
             "/usr/bin/curl -s -k -L --max-time 25 --connect-timeout 8 "
             "-A 'Mozilla/5.0' '%.1900s'", url);
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

    /* 1) download the provider script */
    char url[512];
    snprintf(url, sizeof url, "%swastecollectionProvider_%s.js", PLUGIN_BASE, pid);
    char * script = http_get(url);
    if (!script || strlen(script) < 50) {
        fprintf(stderr, "[wastefetch] could not download provider script %s\n", url);
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
        char call[2048];
        snprintf(call, sizeof call,
            "readCalendar(\"%s\",\"%s\",[],true,\"%s\",\"%s\",\"%s\",\"%s\",\"%s\");",
            zip, nr, icsid, street, street, city, fullics);
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
