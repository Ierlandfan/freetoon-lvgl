/*
 * doorbell_daemon — tiny HTTP shim that lets Home Assistant (or any
 * webhook source) flip the Toon's live-video tile on and off.
 *
 *   POST /show     -> toonui camera_open()   (video appears)
 *   POST /hide     -> toonui camera_close()  (video disappears)
 *   GET  /status   -> 200 OK, "{\"ok\":true}\n"
 *
 * GET is accepted too for /show and /hide (curl test convenience). The
 * daemon never touches LVGL or the VPU directly -- it forwards the
 * command to toonui over a UNIX socket (/tmp/toonui.cmd), and toonui's
 * camera_open/close does the SIGUSR1/SIGUSR2 to vpu_stream and the
 * cutout/overlay setup. Keeps all UI state inside toonui where it
 * belongs and means the daemon is stateless / safe to restart.
 *
 * Auth: if /mnt/data/doorbell.token exists and is non-empty, requests
 * must carry "X-Doorbell-Token: <same value>". If the file is missing
 * or empty, no auth is enforced (LAN-only deployment assumed).
 *
 * Usage: doorbell_daemon [port]   (default 8765)
 *
 * HA example (configuration.yaml):
 *   rest_command:
 *     toon_doorbell_show:
 *       url: http://192.168.2.102:8765/show
 *       method: POST
 *       headers:
 *         X-Doorbell-Token: !secret toon_doorbell_token
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT  8765
#define UI_SOCK       "/tmp/toonui.cmd"
#define TOKEN_PATH    "/mnt/data/doorbell.token"

static char g_token[256];
static int  g_token_len = 0;

static void load_token(void)
{
    FILE * f = fopen(TOKEN_PATH, "r");
    if (!f) { g_token_len = 0; return; }
    if (fgets(g_token, sizeof g_token, f)) {
        int n = (int)strlen(g_token);
        while (n > 0 && (g_token[n-1] == '\n' || g_token[n-1] == '\r' ||
                         g_token[n-1] == ' '  || g_token[n-1] == '\t'))
            g_token[--n] = 0;
        g_token_len = n;
    }
    fclose(f);
}

/* Connect to /tmp/toonui.cmd and write one line. Returns 0 on success,
 * -1 if toonui isn't listening. */
static int notify_toonui(const char * cmd)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, UI_SOCK, sizeof(sa.sun_path) - 1);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(s);
        return -1;
    }
    char buf[16];
    int n = snprintf(buf, sizeof buf, "%s\n", cmd);
    int rc = (write(s, buf, n) == n) ? 0 : -1;
    close(s);
    return rc;
}

/* Very small HTTP/1.x parser. We only need the request line + headers,
 * body is ignored. Buffer is whatever read() gave us. */
static int parse_request(const char * buf, int len,
                         char * method, int method_sz,
                         char * path,   int path_sz,
                         int  * token_ok)
{
    const char * p   = buf;
    const char * end = buf + len;
    int i;

    /* METHOD */
    i = 0;
    while (p < end && *p != ' ' && *p != '\r' && i < method_sz - 1)
        method[i++] = *p++;
    method[i] = 0;
    if (p >= end || *p != ' ') return -1;
    p++;

    /* PATH */
    i = 0;
    while (p < end && *p != ' ' && *p != '\r' && i < path_sz - 1)
        path[i++] = *p++;
    path[i] = 0;
    if (p >= end || *p != ' ') return -1;

    /* skip to end of request line */
    while (p < end && *p != '\n') p++;
    if (p < end) p++;

    /* HEADERS: only scan for X-Doorbell-Token. If no token configured,
     * accept unconditionally. */
    *token_ok = (g_token_len == 0) ? 1 : 0;
    while (p < end) {
        const char * line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;
        if (line_end - p <= 1) break;   /* blank line = end of headers */
        if (g_token_len > 0 &&
            (line_end - p) > 17 &&
            strncasecmp(p, "X-Doorbell-Token:", 17) == 0) {
            const char * v = p + 17;
            while (v < line_end && (*v == ' ' || *v == '\t')) v++;
            int vlen = (int)(line_end - v);
            /* trim trailing \r */
            while (vlen > 0 && (v[vlen-1] == '\r' || v[vlen-1] == ' '))
                vlen--;
            if (vlen == g_token_len && memcmp(v, g_token, vlen) == 0)
                *token_ok = 1;
        }
        if (line_end >= end) break;
        p = line_end + 1;
    }
    return 0;
}

static void send_response(int sock, int code, const char * status, const char * body)
{
    char buf[512];
    int blen = (int)strlen(body);
    int n = snprintf(buf, sizeof buf,
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s", code, status, blen, body);
    write(sock, buf, n);
}

static void handle(int sock, const struct sockaddr_in * peer)
{
    char buf[2048];
    int n = read(sock, buf, sizeof(buf) - 1);
    if (n <= 0) { close(sock); return; }
    buf[n] = 0;

    char method[16], path[64];
    int token_ok = 0;
    if (parse_request(buf, n, method, sizeof method, path, sizeof path, &token_ok) < 0) {
        send_response(sock, 400, "Bad Request", "malformed request\n");
        close(sock);
        return;
    }

    char peer_s[32] = "?";
    if (peer) inet_ntop(AF_INET, &peer->sin_addr, peer_s, sizeof peer_s);

    if (!token_ok) {
        send_response(sock, 401, "Unauthorized",
                      "missing or wrong X-Doorbell-Token\n");
        fprintf(stderr, "[doorbell] %s %s %s -> 401\n", peer_s, method, path);
        close(sock);
        return;
    }

    const char * cmd = NULL;
    const char * verb = NULL;
    if (!strcmp(path, "/show") || !strcmp(path, "/doorbell")) {
        cmd = "show"; verb = "shown";
    } else if (!strcmp(path, "/hide")) {
        cmd = "hide"; verb = "hidden";
    } else if (!strcmp(path, "/status")) {
        send_response(sock, 200, "OK", "{\"ok\":true}\n");
        close(sock);
        return;
    } else {
        send_response(sock, 404, "Not Found", "");
        fprintf(stderr, "[doorbell] %s %s %s -> 404\n", peer_s, method, path);
        close(sock);
        return;
    }

    if (notify_toonui(cmd) < 0) {
        send_response(sock, 503, "Service Unavailable",
                      "toonui not listening on " UI_SOCK "\n");
        fprintf(stderr, "[doorbell] %s %s %s -> 503 (toonui down)\n",
                peer_s, method, path);
    } else {
        char body[64];
        snprintf(body, sizeof body, "%s\n", verb);
        send_response(sock, 200, "OK", body);
        fprintf(stderr, "[doorbell] %s %s %s -> 200 (%s)\n",
                peer_s, method, path, verb);
    }
    close(sock);
}

int main(int argc, char ** argv)
{
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);

    load_token();

    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(lsock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); return 1;
    }
    listen(lsock, 4);

    fprintf(stderr, "[doorbell] listening on :%d (auth: %s)\n", port,
            g_token_len > 0 ? "X-Doorbell-Token required" : "DISABLED");

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int sock = accept(lsock, (struct sockaddr *)&peer, &plen);
        if (sock < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        handle(sock, &peer);
    }
    close(lsock);
    return 0;
}
