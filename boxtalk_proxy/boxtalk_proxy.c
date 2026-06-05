/*
 * boxtalk_proxy — authenticated LAN gateway to the Toon's local BoxTalk bus.
 *
 * The bus (hcb_comm) listens on 127.0.0.1:1337 only. This tiny standalone daemon
 * exposes it on the LAN behind HTTP Basic auth, WITHOUT touching any stock config
 * (qmf_release.xml / CommHost stays "localhost"). Flow per connection:
 *   1. read the HTTP request headers,
 *   2. validate `Authorization: Basic <b64(user:pass)>` against the first line of
 *      /mnt/data/boxtalk_proxy.auth ("user:pass"),
 *   3. on success reply `200`, then transparently splice the socket to
 *      127.0.0.1:1337. After the 200 it is a dumb byte pipe — the client speaks
 *      native BoxTalk (handshake / subscribe / notify), fully push, no polling.
 *
 * Pair with a firewall rule that opens the listen port to the mirror Toon's IP
 * only, e.g.:
 *   iptables -I HCB-INPUT 1 -p tcp -s <dev-ip> --dport 1338 -j ACCEPT
 *
 * ⚠ Basic auth is base64, not encryption — fine on a trusted LAN scoped to the
 * dev IP; it stops casual/accidental connects, not a sniffing attacker. For
 * stronger security put it behind a TLS terminator or use a per-host firewall.
 *
 * No freetoon needed on this Toon — it runs alongside pure stock qt-gui.
 *
 * Usage: boxtalk_proxy [listen_port]   (default 1338)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define DEFAULT_PORT 1338
#define BUS_PORT     1337
#define AUTH_FILE    "/mnt/data/boxtalk_proxy.auth"

static char g_authb64[344];     /* base64 of "user:pass" we expect in the header */
static int  g_authb64_len = 0;

static int b64enc(const unsigned char *in, int n, char *out, int outsz) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int b = in[i] << 16;
        if (i + 1 < n) b |= in[i + 1] << 8;
        if (i + 2 < n) b |= in[i + 2];
        if (o + 5 > outsz) return -1;
        out[o++] = t[(b >> 18) & 63];
        out[o++] = t[(b >> 12) & 63];
        out[o++] = (i + 1 < n) ? t[(b >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? t[b & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static void load_auth(void) {
    FILE *f = fopen(AUTH_FILE, "r");
    if (!f) { g_authb64_len = 0; return; }
    char line[256];
    if (fgets(line, sizeof line, f)) {
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1]=='\n' || line[n-1]=='\r' ||
                         line[n-1]==' '  || line[n-1]=='\t'))
            line[--n] = 0;
        g_authb64_len = b64enc((unsigned char *)line, n, g_authb64, sizeof g_authb64);
        if (g_authb64_len < 0) g_authb64_len = 0;
    }
    fclose(f);
}

static int connect_bus(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(BUS_PORT);
    a.sin_addr.s_addr = htonl(0x7f000001);  /* 127.0.0.1 */
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
    return s;
}

/* Transparent bidirectional byte pipe until either side closes. */
static void splice_loop(int a, int b) {
    char buf[4096];
    int mx = (a > b ? a : b) + 1;
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds); FD_SET(a, &fds); FD_SET(b, &fds);
        if (select(mx, &fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (FD_ISSET(a, &fds)) {
            int n = (int)read(a, buf, sizeof buf);
            if (n <= 0 || write(b, buf, n) != n) break;
        }
        if (FD_ISSET(b, &fds)) {
            int n = (int)read(b, buf, sizeof buf);
            if (n <= 0 || write(a, buf, n) != n) break;
        }
    }
}

static void send_str(int fd, const char *s) { (void)!write(fd, s, strlen(s)); }

static void handle(int c, const char *peer) {
    char req[2048];
    int n = 0;
    while (n < (int)sizeof(req) - 1) {
        int k = (int)read(c, req + n, sizeof(req) - 1 - n);
        if (k <= 0) { close(c); return; }
        n += k; req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;   /* end of headers */
    }

    int ok = (g_authb64_len == 0);            /* no auth file => open (logged) */
    if (g_authb64_len > 0) {
        const char *h = strstr(req, "Authorization: Basic ");
        if (h) {
            h += strlen("Authorization: Basic ");
            if (strncmp(h, g_authb64, g_authb64_len) == 0) {
                char e = h[g_authb64_len];
                ok = (e == '\r' || e == '\n' || e == ' ' || e == '\t' || e == 0);
            }
        }
    }
    if (!ok) {
        send_str(c, "HTTP/1.0 401 Unauthorized\r\n"
                    "WWW-Authenticate: Basic realm=\"boxtalk\"\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n");
        fprintf(stderr, "[boxtalk_proxy] %s -> 401\n", peer);
        close(c); return;
    }

    int bus = connect_bus();
    if (bus < 0) {
        send_str(c, "HTTP/1.0 503 Service Unavailable\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n");
        fprintf(stderr, "[boxtalk_proxy] %s -> 503 (bus 127.0.0.1:%d down)\n", peer, BUS_PORT);
        close(c); return;
    }
    send_str(c, "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\n");
    fprintf(stderr, "[boxtalk_proxy] %s -> 200 (tunnel open)\n", peer);
    splice_loop(c, bus);
    close(bus); close(c);
}

int main(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   /* auto-reap forked handlers */
    load_auth();

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);   /* firewall scopes who can reach it */
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return 1; }
    listen(ls, 8);
    fprintf(stderr, "[boxtalk_proxy] listening on :%d -> 127.0.0.1:%d (auth: %s)\n",
            port, BUS_PORT, g_authb64_len > 0 ? "required" : "OPEN (no auth file!)");

    for (;;) {
        struct sockaddr_in pa;
        socklen_t pl = sizeof pa;
        int c = accept(ls, (struct sockaddr *)&pa, &pl);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        char peer[32] = "?";
        inet_ntop(AF_INET, &pa.sin_addr, peer, sizeof peer);
        pid_t pid = fork();
        if (pid == 0) { close(ls); handle(c, peer); _exit(0); }
        close(c);
    }
    close(ls);
    return 0;
}
