/* ESPHome native API client — Noise_NNpsk0_25519_ChaChaPoly_SHA256
 *
 * Protocol references:
 *   aioesphomeapi/_frame_helper/noise.py — frame format + handshake
 *   aioesphomeapi/_frame_helper/packets.py — post-handshake inner format
 *   Noise Protocol Framework spec (noiseprotocol.org)
 *
 * Crypto: monocypher 4.0.2 for X25519 + ChaCha20-Poly1305 (IETF 12-byte nonce);
 *         sha2.c (vendored) for SHA-256 + HMAC-SHA256 used by HKDF. */

#include "efanlamp.h"
#include "settings.h"
#include "monocypher.h"
#include "sha2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

/* ── public state ───────────────────────────────────────────────────────── */
efanlamp_state_t efanlamp = {0};

/* Debug: set EFANLAMP_DEBUG env (or g_ef_verbose) to dump handshake bytes. */
static int g_ef_verbose = 0;
#define DBG(...) do { if (g_ef_verbose) fprintf(stderr, __VA_ARGS__); } while (0)
static void ef_hex(const char *tag, const uint8_t *d, size_t n) {
    if (!g_ef_verbose) return;
    fprintf(stderr, "[efanlamp] %s (%zu):", tag, n);
    for (size_t i = 0; i < n && i < 64; i++) fprintf(stderr, " %02x", d[i]);
    fprintf(stderr, "\n");
}

/* ── protobuf message type IDs ─────────────────────────────────────────── */
#define ESPHOME_MSG_HELLO_REQUEST         1
#define ESPHOME_MSG_HELLO_RESPONSE        2
#define ESPHOME_MSG_CONNECT_REQUEST       3
#define ESPHOME_MSG_CONNECT_RESPONSE      4
#define ESPHOME_MSG_DISCONNECT_REQUEST    5
#define ESPHOME_MSG_DISCONNECT_RESPONSE   6
#define ESPHOME_MSG_PING_REQUEST          7
#define ESPHOME_MSG_PING_RESPONSE         8
#define ESPHOME_MSG_LIST_ENTITIES_REQUEST 11
#define ESPHOME_MSG_LIST_ENTITIES_DONE    19
#define ESPHOME_MSG_SUBSCRIBE_STATES      20
/* ESPHome api.proto MessageTypes (verified against a live ld2411-ble-fanlamp) */
#define ESPHOME_MSG_LIST_ENTITIES_FAN     14   /* ListEntitiesFanResponse */
#define ESPHOME_MSG_LIST_ENTITIES_LIGHT   15   /* ListEntitiesLightResponse */
#define ESPHOME_MSG_FAN_STATE             23   /* FanStateResponse */
#define ESPHOME_MSG_LIGHT_STATE           24   /* LightStateResponse */
#define ESPHOME_MSG_FAN_COMMAND           31   /* FanCommandRequest */
#define ESPHOME_MSG_LIGHT_COMMAND         32   /* LightCommandRequest */
#define ESPHOME_MSG_LIST_ENTITIES_TEXT_SENSOR 16  /* ListEntitiesTextSensorResponse */
#define ESPHOME_MSG_TEXT_SENSOR_STATE         26  /* TextSensorStateResponse */
#define ESPHOME_MSG_LIST_ENTITIES_SERVICES   41  /* ListEntitiesServicesResponse */
#define ESPHOME_MSG_EXECUTE_SERVICE          42  /* ExecuteServiceRequest */

/* ── Noise state ────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t h[32];    /* running transcript hash */
    uint8_t ck[32];   /* chaining key */
    uint8_t k[32];    /* current cipher key */
    uint64_t n;       /* nonce counter */
    int has_key;
} noise_sym_t;

typedef struct {
    noise_sym_t sym;
    /* ephemeral keypair */
    uint8_t e_priv[32];
    uint8_t e_pub[32];
    /* session cipher states */
    uint8_t send_k[32];
    uint8_t recv_k[32];
    uint64_t send_n;
    uint64_t recv_n;
    int handshake_done;
} noise_ctx_t;

/* entity keys discovered via ListEntities */
static uint32_t g_fan_key    = 0;
static uint32_t g_light_key  = 0;
static uint32_t g_src_sensor_key = 0;
static uint32_t g_service_key = 0;  /* key of the device 'mark_source_toon' user action */

/* ── helpers ────────────────────────────────────────────────────────────── */
static void ef_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    sha256_once(data, len, out);
}

static void ef_sha256_cat(const uint8_t *a, size_t la,
                          const uint8_t *b, size_t lb,
                          uint8_t out[32]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, a, la);
    sha256_update(&ctx, b, lb);
    sha256_final(&ctx, out);
}

static void ef_hmac_sha256(const uint8_t *key, size_t klen,
                           const uint8_t *data, size_t dlen,
                           uint8_t out[32]) {
    hmac_sha256(key, klen, data, dlen, out);
}

/* HKDF-SHA256: extract + expand (2 or 3 outputs of 32 bytes each) */
static void hkdf2(const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len,
                  uint8_t out1[32], uint8_t out2[32]) {
    uint8_t temp[32];
    uint8_t buf[33];
    ef_hmac_sha256(ck, 32, ikm, ikm_len, temp);
    buf[0] = 0x01;
    ef_hmac_sha256(temp, 32, buf, 1, out1);
    memcpy(buf, out1, 32); buf[32] = 0x02;
    ef_hmac_sha256(temp, 32, buf, 33, out2);
}

static void hkdf3(const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len,
                  uint8_t out1[32], uint8_t out2[32], uint8_t out3[32]) {
    uint8_t temp[32];
    uint8_t buf[33];
    ef_hmac_sha256(ck, 32, ikm, ikm_len, temp);
    buf[0] = 0x01;
    ef_hmac_sha256(temp, 32, buf, 1, out1);
    memcpy(buf, out1, 32); buf[32] = 0x02;
    ef_hmac_sha256(temp, 32, buf, 33, out2);
    memcpy(buf, out2, 32); buf[32] = 0x03;
    ef_hmac_sha256(temp, 32, buf, 33, out3);
}

/* Build 12-byte IETF ChaCha20 nonce: 4-byte zero + 8-byte LE uint64 */
static void make_nonce12(uint64_t n, uint8_t nonce[12]) {
    memset(nonce, 0, 4);
    nonce[4] = (uint8_t)(n);
    nonce[5] = (uint8_t)(n >> 8);
    nonce[6] = (uint8_t)(n >> 16);
    nonce[7] = (uint8_t)(n >> 24);
    nonce[8] = (uint8_t)(n >> 32);
    nonce[9] = (uint8_t)(n >> 40);
    nonce[10]= (uint8_t)(n >> 48);
    nonce[11]= (uint8_t)(n >> 56);
}

/* ── Noise symmetric state ops ─────────────────────────────────────────── */
static void noise_init(noise_sym_t *s, const char *proto_name) {
    size_t len = strlen(proto_name);
    if (len <= 32) {
        memset(s->h, 0, 32);
        memcpy(s->h, proto_name, len);
    } else {
        ef_sha256((const uint8_t *)proto_name, len, s->h);
    }
    memcpy(s->ck, s->h, 32);
    memset(s->k, 0, 32);
    s->n = 0;
    s->has_key = 0;
}

static void noise_mix_hash(noise_sym_t *s, const uint8_t *data, size_t len) {
    ef_sha256_cat(s->h, 32, data, len, s->h);
}

static void noise_mix_key(noise_sym_t *s, const uint8_t *ikm, size_t len) {
    uint8_t ck_new[32], temp_k[32];
    hkdf2(s->ck, ikm, len, ck_new, temp_k);
    memcpy(s->ck, ck_new, 32);
    memcpy(s->k, temp_k, 32);
    s->n = 0;
    s->has_key = 1;
}

static void noise_mix_key_and_hash(noise_sym_t *s, const uint8_t *ikm, size_t len) {
    uint8_t ck_new[32], temp_h[32], temp_k[32];
    hkdf3(s->ck, ikm, len, ck_new, temp_h, temp_k);
    memcpy(s->ck, ck_new, 32);
    noise_mix_hash(s, temp_h, 32);
    memcpy(s->k, temp_k, 32);
    s->n = 0;
    s->has_key = 1;
}

/* EncryptAndHash(plaintext) → appends ciphertext+tag to buf, advances ptr.
 * plaintext may be NULL/0 (produces 16-byte tag only). */
static void noise_encrypt_and_hash(noise_sym_t *s,
                                   const uint8_t *pt, size_t pt_len,
                                   uint8_t *out) {
    crypto_aead_ctx ac;
    uint8_t nonce12[12];
    uint8_t mac[16];
    make_nonce12(s->n, nonce12);
    crypto_aead_init_ietf(&ac, s->k, nonce12);
    crypto_aead_write(&ac, out, mac, s->h, 32, pt, pt_len);
    memcpy(out + pt_len, mac, 16);
    noise_mix_hash(s, out, pt_len + 16);  /* mix ciphertext+tag */
    s->n++;
    crypto_wipe(&ac, sizeof ac);
}

/* DecryptAndHash(ciphertext) → returns 0 on success, -1 on auth failure */
static int noise_decrypt_and_hash(noise_sym_t *s,
                                  const uint8_t *ct, size_t ct_len,
                                  uint8_t *out) {
    if (ct_len < 16) return -1;
    size_t pt_len = ct_len - 16;
    const uint8_t *mac = ct + pt_len;
    crypto_aead_ctx ac;
    uint8_t nonce12[12];
    make_nonce12(s->n, nonce12);
    crypto_aead_init_ietf(&ac, s->k, nonce12);
    int r = crypto_aead_read(&ac, out, mac, s->h, 32, ct, pt_len);
    /* mix the ciphertext (ct includes tag) before returning */
    noise_mix_hash(s, ct, ct_len);
    s->n++;
    crypto_wipe(&ac, sizeof ac);
    return r;  /* 0 = ok, -1 = bad MAC */
}

static void noise_split(noise_sym_t *s, uint8_t k1[32], uint8_t k2[32]) {
    hkdf2(s->ck, NULL, 0, k1, k2);
}

/* ── TCP send/recv helpers ─────────────────────────────────────────────── */
static int sock_send_all(int fd, const uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, 0);
        if (n <= 0) return -1;
        buf += n; len -= n;
    }
    return 0;
}

static int sock_recv_all(int fd, uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n <= 0) return -1;
        buf += n; len -= n;
    }
    return 0;
}

/* Read one Noise frame: [0x01][uint16be: len][data]. Returns data length
 * or -1 on error. buf must be at least 65535 bytes. */
static int read_noise_frame(int fd, uint8_t *buf, int max) {
    uint8_t hdr[3];
    int rr = sock_recv_all(fd, hdr, 3);
    DBG("[rnf] sock_recv_all(3)=%d hdr=%02x%02x%02x\n", rr, hdr[0], hdr[1], hdr[2]);
    if (rr < 0) return -1;
    if (hdr[0] != 0x01) { DBG("[rnf] bad indicator %02x\n", hdr[0]); return -1; }
    int len = ((int)hdr[1] << 8) | hdr[2];
    if (len > max) return -1;
    if (len > 0 && sock_recv_all(fd, buf, (size_t)len) < 0) return -1;
    return len;
}

/* Write one Noise frame: [0x01][uint16be: len][data] */
static int write_noise_frame(int fd, const uint8_t *data, size_t len) {
    uint8_t hdr[3];
    hdr[0] = 0x01;
    hdr[1] = (uint8_t)(len >> 8);
    hdr[2] = (uint8_t)(len & 0xFF);
    if (sock_send_all(fd, hdr, 3) < 0) return -1;
    if (len > 0 && sock_send_all(fd, data, len) < 0) return -1;
    return 0;
}

/* ── Noise handshake ────────────────────────────────────────────────────── */
static int do_handshake(int fd, noise_ctx_t *ctx, const uint8_t psk[32]) {
    uint8_t buf[256];
    int len;

    /* ── Init Noise state ── */
    const char *proto = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";
    noise_init(&ctx->sym, proto);

    /* MixHash(prologue) */
    const uint8_t prologue[] = "NoiseAPIInit\x00\x00";
    noise_mix_hash(&ctx->sym, prologue, 14);

    /* MixKeyAndHash(psk) — psk0 */
    noise_mix_key_and_hash(&ctx->sym, psk, 32);

    /* Generate ephemeral keypair */
    {
        int urnd = open("/dev/urandom", O_RDONLY);
        if (urnd < 0) return -1;
        read(urnd, ctx->e_priv, 32);
        close(urnd);
    }
    ctx->e_priv[0]  &= 248;
    ctx->e_priv[31] &= 127;
    ctx->e_priv[31] |= 64;
    crypto_x25519_public_key(ctx->e_pub, ctx->e_priv);

    ef_hex("e_pub", ctx->e_pub, 32);

    /* ── Step 1: send NOISE_HELLO + handshake msg 1 ── */
    /* hello: [0x01][0x00][0x00] */
    if (write_noise_frame(fd, NULL, 0) < 0) { DBG("[efanlamp] FAIL send hello\n"); return -1; }

    /* msg1 = e.public (32) + EncryptAndHash("") (16-byte tag) = 48 bytes.
     * NNpsk0: the "e" token, because a "psk" precedes it, does MixHash AND
     * MixKey on the ephemeral public key (Noise spec §9). */
    uint8_t msg1[48];
    memcpy(msg1, ctx->e_pub, 32);
    noise_mix_hash(&ctx->sym, ctx->e_pub, 32);
    noise_mix_key(&ctx->sym, ctx->e_pub, 32);   /* PSK modifier on "e" */
    noise_encrypt_and_hash(&ctx->sym, NULL, 0, msg1 + 32);  /* 16-byte tag */

    /* frame: [0x01][uint16be: 49][0x00][48-byte msg1] */
    uint8_t frame1[52];
    frame1[0] = 0x01;
    frame1[1] = 0x00;
    frame1[2] = 49;   /* 1 (preamble byte \x00) + 48 */
    frame1[3] = 0x00; /* preamble byte inside frame */
    memcpy(frame1 + 4, msg1, 48);
    ef_hex("send frame1", frame1, 52);
    if (sock_send_all(fd, frame1, 52) < 0) { DBG("[efanlamp] FAIL send frame1\n"); return -1; }

    /* ── Step 2: receive server hello ── */
    len = read_noise_frame(fd, buf, sizeof buf);
    DBG("[efanlamp] server hello frame len=%d\n", len);
    if (len < 1) { DBG("[efanlamp] FAIL read hello (len=%d)\n", len); return -1; }
    ef_hex("server hello", buf, (size_t)len);
    if (buf[0] != 0x01) {
        /* error response from server */
        DBG("[efanlamp] FAIL hello indicator=0x%02x (expected 0x01)\n", buf[0]);
        return -1;
    }
    /* server name follows buf[1] as null-terminated string; ignore it */

    /* ── Step 3: receive server handshake message ── */
    len = read_noise_frame(fd, buf, sizeof buf);
    DBG("[efanlamp] server handshake frame len=%d\n", len);
    if (len < 1) { DBG("[efanlamp] FAIL read handshake (len=%d)\n", len); return -1; }
    ef_hex("server handshake", buf, (size_t)len);
    if (buf[0] != 0x00) { DBG("[efanlamp] FAIL handshake preamble=0x%02x\n", buf[0]); return -1; }  /* error preamble */
    /* buf[1:] = server msg2: e_r.public (32) + tag (16) = 48 bytes */
    if (len < 49) { DBG("[efanlamp] FAIL handshake too short len=%d\n", len); return -1; }
    const uint8_t *msg2 = buf + 1;
    const uint8_t *e_r_pub = msg2;           /* 32 bytes */
    const uint8_t *tag2    = msg2 + 32;      /* 16-byte tag */

    /* "e" token (PSK): MixHash AND MixKey on the server's ephemeral pubkey. */
    noise_mix_hash(&ctx->sym, e_r_pub, 32);
    noise_mix_key(&ctx->sym, e_r_pub, 32);   /* PSK modifier on remote "e" */

    /* DH(e_init, e_r) */
    uint8_t ee[32];
    crypto_x25519(ee, ctx->e_priv, e_r_pub);
    noise_mix_key(&ctx->sym, ee, 32);
    crypto_wipe(ee, 32);

    /* DecryptAndHash("") — verify 16-byte tag */
    uint8_t empty_tag[16];
    memcpy(empty_tag, tag2, 16);
    /* the "ciphertext" is just the 16-byte tag; plaintext is empty */
    if (noise_decrypt_and_hash(&ctx->sym, empty_tag, 16, NULL) != 0) {
        DBG("[efanlamp] FAIL decrypt server tag (bad MAC) — psk/prologue mismatch\n");
        return -1;
    }
    DBG("[efanlamp] handshake OK, session keys derived\n");

    /* ── Split: derive session keys ── */
    noise_split(&ctx->sym, ctx->send_k, ctx->recv_k);
    ctx->send_n = 0;
    ctx->recv_n = 0;
    ctx->handshake_done = 1;

    crypto_wipe(ctx->e_priv, 32);
    return 0;
}

/* ── Protobuf helpers ───────────────────────────────────────────────────── */
static uint8_t * pb_write_tag(uint8_t *p, int field, int wire) {
    uint32_t v = (uint32_t)(field << 3) | (uint32_t)wire;
    while (v >= 0x80) { *p++ = (uint8_t)(v | 0x80); v >>= 7; }
    *p++ = (uint8_t)v;
    return p;
}
static uint8_t * pb_write_varint(uint8_t *p, uint64_t v) {
    while (v >= 0x80) { *p++ = (uint8_t)(v | 0x80); v >>= 7; }
    *p++ = (uint8_t)v;
    return p;
}
static uint8_t * pb_write_str(uint8_t *p, int field, const char *s) {
    size_t n = strlen(s);
    p = pb_write_tag(p, field, 2);
    p = pb_write_varint(p, (uint64_t)n);
    memcpy(p, s, n); p += n;
    return p;
}
static uint8_t * pb_write_bool(uint8_t *p, int field, int v) {
    p = pb_write_tag(p, field, 0);
    *p++ = v ? 1 : 0;
    return p;
}
static uint8_t * pb_write_int32(uint8_t *p, int field, int32_t v) {
    p = pb_write_tag(p, field, 0);
    p = pb_write_varint(p, (uint64_t)(int64_t)v);
    return p;
}
static uint8_t * pb_write_fixed32(uint8_t *p, int field, uint32_t v) {
    p = pb_write_tag(p, field, 5);
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
    p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
    return p + 4;
}
static uint8_t * pb_write_float(uint8_t *p, int field, float v) {
    uint32_t bits; memcpy(&bits, &v, 4);
    return pb_write_fixed32(p, field, bits);
}

/* minimal protobuf decoder */
typedef struct { const uint8_t *p; size_t rem; } pb_reader_t;
static int pb_next(pb_reader_t *r, int *field, int *wire, uint64_t *val,
                   const uint8_t **lp, size_t *ll) {
    if (!r->rem) return 0;
    uint64_t tag = 0; int sh = 0;
    do { if (!r->rem) return -1; uint8_t b = *r->p++; r->rem--;
         tag |= (uint64_t)(b & 0x7F) << sh; sh += 7;
         if (!(b & 0x80)) break; } while (sh < 63);
    *field = (int)(tag >> 3); *wire = (int)(tag & 7);
    if (*wire == 0) { /* varint */
        *val = 0; sh = 0;
        do { if (!r->rem) return -1; uint8_t b = *r->p++; r->rem--;
             *val |= (uint64_t)(b & 0x7F) << sh; sh += 7;
             if (!(b & 0x80)) break; } while (sh < 63);
    } else if (*wire == 5) { /* 32-bit */
        if (r->rem < 4) return -1;
        uint32_t v = (uint32_t)r->p[0] | ((uint32_t)r->p[1]<<8) |
                     ((uint32_t)r->p[2]<<16) | ((uint32_t)r->p[3]<<24);
        *val = v; r->p += 4; r->rem -= 4;
    } else if (*wire == 2) { /* length-delimited */
        uint64_t n = 0; sh = 0;
        do { if (!r->rem) return -1; uint8_t b = *r->p++; r->rem--;
             n |= (uint64_t)(b & 0x7F) << sh; sh += 7;
             if (!(b & 0x80)) break; } while (sh < 63);
        if (n > r->rem) return -1;
        *lp = r->p; *ll = (size_t)n;
        r->p += n; r->rem -= n;
    } else return -1;
    return 1;
}

/* ── session encrypt/decrypt ────────────────────────────────────────────── */
static int session_encrypt(noise_ctx_t *ctx,
                           const uint8_t *pt, size_t pt_len,
                           uint8_t *out) {
    /* out must be pt_len + 16 bytes */
    uint8_t nonce12[12];
    uint8_t mac[16];
    make_nonce12(ctx->send_n++, nonce12);
    crypto_aead_ctx ac;
    crypto_aead_init_ietf(&ac, ctx->send_k, nonce12);
    crypto_aead_write(&ac, out, mac, NULL, 0, pt, pt_len);
    memcpy(out + pt_len, mac, 16);
    crypto_wipe(&ac, sizeof ac);
    return 0;
}

static int session_decrypt(noise_ctx_t *ctx,
                           const uint8_t *ct, size_t ct_len,
                           uint8_t *out) {
    if (ct_len < 16) return -1;
    size_t pt_len = ct_len - 16;
    const uint8_t *mac = ct + pt_len;
    uint8_t nonce12[12];
    make_nonce12(ctx->recv_n++, nonce12);
    crypto_aead_ctx ac;
    crypto_aead_init_ietf(&ac, ctx->recv_k, nonce12);
    int r = crypto_aead_read(&ac, out, mac, NULL, 0, ct, pt_len);
    crypto_wipe(&ac, sizeof ac);
    return r;
}

/* ── send a protobuf message ────────────────────────────────────────────── */
static int send_message(int fd, noise_ctx_t *ctx,
                        uint16_t msg_type, const uint8_t *pb, size_t pb_len) {
    /* inner plaintext: [uint16be: type][uint16be: len][payload] */
    size_t inner_len = 4 + pb_len;
    uint8_t *inner = malloc(inner_len);
    if (!inner) return -1;
    inner[0] = (uint8_t)(msg_type >> 8);
    inner[1] = (uint8_t)(msg_type & 0xFF);
    inner[2] = (uint8_t)(pb_len >> 8);
    inner[3] = (uint8_t)(pb_len & 0xFF);
    memcpy(inner + 4, pb, pb_len);

    size_t enc_len = inner_len + 16;
    uint8_t *enc = malloc(enc_len);
    if (!enc) { free(inner); return -1; }
    session_encrypt(ctx, inner, inner_len, enc);
    free(inner);

    int r = write_noise_frame(fd, enc, enc_len);
    free(enc);
    return r;
}

/* ── receive and parse one incoming message ─────────────────────────────── */
static void handle_fan_state(const uint8_t *pb, size_t pb_len) {
    pb_reader_t r = { pb, pb_len };
    int field, wire; uint64_t val = 0;
    const uint8_t *lp = NULL; size_t ll = 0;
    uint32_t key = 0; int state = 0; int speed_level = 0;
    while (pb_next(&r, &field, &wire, &val, &lp, &ll) > 0) {
        if (field == 1 && wire == 5) key = (uint32_t)val;
        else if (field == 2 && wire == 0) state = (int)val;
        else if (field == 6 && wire == 0) speed_level = (int)val;  /* FanStateResponse.speed_level = 6 */
    }
    if (key && key == g_fan_key) {
        efanlamp.fan_on    = state;
        efanlamp.fan_speed = speed_level;
    }
}

static void handle_light_state(const uint8_t *pb, size_t pb_len) {
    pb_reader_t r = { pb, pb_len };
    int field, wire; uint64_t val = 0;
    const uint8_t *lp = NULL; size_t ll = 0;
    uint32_t key = 0; int state = 0; float brightness = 0.f;
    while (pb_next(&r, &field, &wire, &val, &lp, &ll) > 0) {
        if (field == 1 && wire == 5) key = (uint32_t)val;
        else if (field == 2 && wire == 0) state = (int)val;
        else if (field == 3 && wire == 5) memcpy(&brightness, &val, 4);
    }
    if (key && key == g_light_key) {
        efanlamp.light_on         = state;
        efanlamp.light_brightness = (int)(brightness * 100.f + 0.5f);
    }
}

static void handle_list_text_sensor(const uint8_t *pb, size_t pb_len) {
    pb_reader_t r = { pb, pb_len };
    int field, wire; uint64_t val = 0;
    const uint8_t *lp = NULL; size_t ll = 0;
    const uint8_t *oid = NULL; size_t oid_len = 0;
    uint32_t key = 0;
    while (pb_next(&r, &field, &wire, &val, &lp, &ll) > 0) {
        if (field == 1 && wire == 2) { oid = lp; oid_len = ll; }
        else if (field == 2 && wire == 5) key = (uint32_t)val;
    }
    if (oid && oid_len == 19 && memcmp(oid, "fanlamp_last_source", 19) == 0)
        g_src_sensor_key = key;
}

static void handle_text_sensor_state(const uint8_t *pb, size_t pb_len) {
    pb_reader_t r = { pb, pb_len };
    int field, wire; uint64_t val = 0;
    const uint8_t *lp = NULL; size_t ll = 0;
    uint32_t key = 0;
    const uint8_t *state = NULL; size_t state_len = 0;
    while (pb_next(&r, &field, &wire, &val, &lp, &ll) > 0) {
        if (field == 1 && wire == 5) key = (uint32_t)val;
        else if (field == 2 && wire == 2) { state = lp; state_len = ll; }
    }
    if (key && key == g_src_sensor_key && state) {
        size_t n = state_len < sizeof(efanlamp.last_source) - 1
                   ? state_len : sizeof(efanlamp.last_source) - 1;
        memcpy((void *)efanlamp.last_source, state, n);
        ((char *)efanlamp.last_source)[n] = '\0';
    }
}

static void handle_list_fan(const uint8_t *pb, size_t pb_len) {
    pb_reader_t r = { pb, pb_len };
    int field, wire; uint64_t val = 0;
    const uint8_t *lp = NULL; size_t ll = 0;
    while (pb_next(&r, &field, &wire, &val, &lp, &ll) > 0)
        if (field == 2 && wire == 5) g_fan_key = (uint32_t)val;
}

static void handle_list_light(const uint8_t *pb, size_t pb_len) {
    pb_reader_t r = { pb, pb_len };
    int field, wire; uint64_t val = 0;
    const uint8_t *lp = NULL; size_t ll = 0;
    while (pb_next(&r, &field, &wire, &val, &lp, &ll) > 0)
        if (field == 2 && wire == 5) g_light_key = (uint32_t)val;
}

/* ListEntitiesServicesResponse: field 1 = name (str), field 2 = key (fixed32).
   Record the key of the 'mark_source_toon' action so we can invoke it. */
static void handle_list_services(const uint8_t *pb, size_t pb_len) {
    pb_reader_t r = { pb, pb_len };
    int field, wire; uint64_t val = 0;
    const uint8_t *lp = NULL; size_t ll = 0;
    const uint8_t *name = NULL; size_t name_len = 0;
    uint32_t key = 0;
    while (pb_next(&r, &field, &wire, &val, &lp, &ll) > 0) {
        if (field == 1 && wire == 2) { name = lp; name_len = ll; }
        else if (field == 2 && wire == 5) key = (uint32_t)val;
    }
    if (name && name_len == 16 && memcmp(name, "mark_source_toon", 16) == 0)
        g_service_key = key;
}

/* Receive one Noise data frame, decrypt, dispatch. Returns 0 on success. */
/* shared session state (declared here so recv_and_dispatch can send ping replies) */
static int g_sock = -1;  /* shared fd, set after connect */
static noise_ctx_t g_ctx;
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Send ExecuteServiceRequest (field 1 = fixed32 key, no args) for mark_source_toon.
   Caller must hold g_send_mutex. Sent immediately before a fan/light command so the
   device tags the resulting on_state as source "toon". */
static void send_mark_source_toon_locked(void) {
    if (!g_service_key || g_sock < 0) return;
    uint8_t pb[8]; uint8_t *p = pb;
    p = pb_write_fixed32(p, 1, g_service_key);
    send_message(g_sock, &g_ctx, ESPHOME_MSG_EXECUTE_SERVICE, pb, (size_t)(p - pb));
}

static int recv_and_dispatch(int fd, noise_ctx_t *ctx) {
    static uint8_t frame_buf[4096];
    int len = read_noise_frame(fd, frame_buf, (int)sizeof frame_buf);
    DBG("[rd] read_noise_frame len=%d recv_n=%llu\n", len, (unsigned long long)ctx->recv_n);
    if (len < 0) return -1;

    static uint8_t plain_buf[4096];
    if (session_decrypt(ctx, frame_buf, (size_t)len, plain_buf) != 0) {
        DBG("[rd] DECRYPT FAIL len=%d recv_n=%llu first=%02x%02x%02x%02x\n",
            len, (unsigned long long)ctx->recv_n,
            frame_buf[0], len>1?frame_buf[1]:0, len>2?frame_buf[2]:0, len>3?frame_buf[3]:0);
        return -1;
    }

    size_t pt_len = (size_t)(len - 16);
    if (pt_len < 4) return 0;
    uint16_t msg_type = ((uint16_t)plain_buf[0] << 8) | plain_buf[1];
    uint16_t msg_len  = ((uint16_t)plain_buf[2] << 8) | plain_buf[3];
    const uint8_t *pb = plain_buf + 4;
    if (msg_len > pt_len - 4) msg_len = (uint16_t)(pt_len - 4);

    switch (msg_type) {
        case ESPHOME_MSG_FAN_STATE:               handle_fan_state(pb, msg_len);         break;
        case ESPHOME_MSG_LIGHT_STATE:             handle_light_state(pb, msg_len);       break;
        case ESPHOME_MSG_TEXT_SENSOR_STATE:       handle_text_sensor_state(pb, msg_len); break;
        case ESPHOME_MSG_LIST_ENTITIES_FAN:       handle_list_fan(pb, msg_len);          break;
        case ESPHOME_MSG_LIST_ENTITIES_LIGHT:     handle_list_light(pb, msg_len);        break;
        case ESPHOME_MSG_LIST_ENTITIES_TEXT_SENSOR: handle_list_text_sensor(pb, msg_len); break;
        case ESPHOME_MSG_LIST_ENTITIES_SERVICES:  handle_list_services(pb, msg_len);     break;
        case ESPHOME_MSG_PING_REQUEST:
            /* Keepalive: ESPHome 2026.4.x drops clients that don't pong. */
            pthread_mutex_lock(&g_send_mutex);
            send_message(fd, ctx, ESPHOME_MSG_PING_RESPONSE, NULL, 0);
            pthread_mutex_unlock(&g_send_mutex);
            break;
        default: break;
    }
    return 0;
}

/* ── command API (called from LVGL thread, run on detached thread) ──────── */
typedef struct { int on; int speed; } fan_cmd_t;
typedef struct { int on; int brightness_pct; } light_cmd_t;

static void * fan_cmd_thread(void *arg) {
    fan_cmd_t cmd = *(fan_cmd_t *)arg;
    free(arg);
    if (!g_ctx.handshake_done || g_sock < 0) return NULL;

    uint8_t pb[32]; uint8_t *p = pb;
    if (g_fan_key) p = pb_write_fixed32(p, 1, g_fan_key);
    p = pb_write_bool(p, 2, 1);          /* has_state */
    p = pb_write_bool(p, 3, cmd.on);     /* state */
    if (cmd.on && cmd.speed > 0) {
        p = pb_write_bool(p, 10, 1);     /* has_speed_level */
        p = pb_write_int32(p, 11, cmd.speed);
    }
    pthread_mutex_lock(&g_send_mutex);
    send_mark_source_toon_locked();   /* tag this change as "toon" before it lands */
    send_message(g_sock, &g_ctx, ESPHOME_MSG_FAN_COMMAND, pb, (size_t)(p-pb));
    pthread_mutex_unlock(&g_send_mutex);
    return NULL;
}

static void * light_cmd_thread(void *arg) {
    light_cmd_t cmd = *(light_cmd_t *)arg;
    free(arg);
    if (!g_ctx.handshake_done || g_sock < 0) return NULL;

    uint8_t pb[48]; uint8_t *p = pb;
    if (g_light_key) p = pb_write_fixed32(p, 1, g_light_key);
    p = pb_write_bool(p, 2, 1);           /* has_state */
    p = pb_write_bool(p, 3, cmd.on);      /* state */
    if (cmd.on) {
        p = pb_write_bool(p, 4, 1);       /* has_brightness = 4 */
        float bri = (float)cmd.brightness_pct / 100.f;
        p = pb_write_float(p, 5, bri);    /* brightness = 5 */
    }
    pthread_mutex_lock(&g_send_mutex);
    send_mark_source_toon_locked();   /* tag this change as "toon" before it lands */
    send_message(g_sock, &g_ctx, ESPHOME_MSG_LIGHT_COMMAND, pb, (size_t)(p-pb));
    pthread_mutex_unlock(&g_send_mutex);
    return NULL;
}

/* ── main connection loop ───────────────────────────────────────────────── */
static void * efanlamp_thread(void *arg) {
    (void)arg;

    for (;;) {
        const char *host = settings.efanlamp_host[0] ? settings.efanlamp_host : "192.168.3.34";
        const char *psk_b64 = settings.efanlamp_psk[0] ? settings.efanlamp_psk
                                : "91dT9Peert6ZI1hukAy1AvUzxS0pqgdauqaf7YQ8b04=";

        /* decode PSK */
        uint8_t psk[32] = {0};
        {
            const char *s = psk_b64;
            uint8_t *d = psk; int bits = 0; uint32_t acc = 0;
            for (; *s && d < psk+32; s++) {
                int v;
                if      (*s >= 'A' && *s <= 'Z') v = *s - 'A';
                else if (*s >= 'a' && *s <= 'z') v = *s - 'a' + 26;
                else if (*s >= '0' && *s <= '9') v = *s - '0' + 52;
                else if (*s == '+')               v = 62;
                else if (*s == '/')               v = 63;
                else if (*s == '=')               break;
                else                              continue;
                acc = (acc << 6) | (uint32_t)v; bits += 6;
                if (bits >= 8) { bits -= 8; *d++ = (uint8_t)(acc >> bits); }
            }
        }

        /* TCP connect */
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { sleep(10); continue; }
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(6053);
        if (inet_pton(AF_INET, host, &sa.sin_addr) <= 0)
            sa.sin_addr.s_addr = inet_addr(host);

        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
            close(fd); sleep(30); continue;
        }

        /* Noise handshake */
        memset(&g_ctx, 0, sizeof g_ctx);
        if (do_handshake(fd, &g_ctx, psk) < 0) {
            efanlamp.connected = 0;
            close(fd); sleep(30); continue;
        }

        /* HelloRequest */
        uint8_t hello_pb[32]; uint8_t *p = hello_pb;
        p = pb_write_str(p, 1, "freetoon");
        p = pb_write_varint(pb_write_tag(p, 2, 0), 1);  /* api_version_major=1 */
        p = pb_write_varint(pb_write_tag(p, 3, 0), 14); /* api_version_minor=14 (clears 'outdated API' warning) */
        pthread_mutex_lock(&g_send_mutex);
        g_sock = fd;
        pthread_mutex_unlock(&g_send_mutex);

        if (send_message(fd, &g_ctx, ESPHOME_MSG_HELLO_REQUEST,
                         hello_pb, (size_t)(p - hello_pb)) < 0) {
            efanlamp.connected = 0; close(fd); g_sock = -1; sleep(30); continue;
        }
        /* ESPHome 2026.1.0+: auth happens at Hello time; server may push log frames
         * (from on_client_connected) before sending HelloResponse. Read until type 2. */
        {
            int hello_done = 0;
            for (int limit = 10; limit > 0 && !hello_done; limit--) {
                static uint8_t fb2[4096], pb_h[4096];
                int flen = read_noise_frame(fd, fb2, sizeof fb2);
                if (flen < 0) break;
                if (session_decrypt(&g_ctx, fb2, (size_t)flen, pb_h) != 0) break;
                size_t ptl = (size_t)(flen - 16);
                if (ptl < 2) continue;
                uint16_t mt = ((uint16_t)pb_h[0] << 8) | pb_h[1];
                if (mt == ESPHOME_MSG_HELLO_RESPONSE) hello_done = 1;
                /* else: log/status frame from on_client_connected — discard and loop */
            }
            if (!hello_done) {
                efanlamp.connected = 0; close(fd); g_sock = -1; sleep(30); continue;
            }
        }
        /* No ConnectRequest — removed in ESPHome 2026.1.0; server auto-authenticates at Hello */

        /* ListEntitiesRequest to discover fan + light keys */
        if (send_message(fd, &g_ctx, ESPHOME_MSG_LIST_ENTITIES_REQUEST, NULL, 0) < 0) {
            efanlamp.connected = 0; close(fd); g_sock = -1; sleep(30); continue;
        }
        /* Read until ListEntitiesDone */
        for (int limit = 50; limit > 0; limit--) {
            static uint8_t fb[4096], pb2[4096];
            int flen = read_noise_frame(fd, fb, sizeof fb);
            if (flen < 0) break;
            if (session_decrypt(&g_ctx, fb, (size_t)flen, pb2) != 0) break;
            size_t pt_len2 = (size_t)(flen - 16);
            if (pt_len2 < 4) continue;
            uint16_t mtype = ((uint16_t)pb2[0] << 8) | pb2[1];
            uint16_t mlen  = ((uint16_t)pb2[2] << 8) | pb2[3];
            if (mlen > pt_len2 - 4) mlen = (uint16_t)(pt_len2 - 4);
            if      (mtype == ESPHOME_MSG_LIST_ENTITIES_FAN)   handle_list_fan(pb2+4, mlen);
            else if (mtype == ESPHOME_MSG_LIST_ENTITIES_LIGHT)  handle_list_light(pb2+4, mlen);
            else if (mtype == ESPHOME_MSG_LIST_ENTITIES_SERVICES) handle_list_services(pb2+4, mlen);
            else if (mtype == ESPHOME_MSG_LIST_ENTITIES_DONE)   break;
        }

        /* SubscribeStatesRequest */
        if (send_message(fd, &g_ctx, ESPHOME_MSG_SUBSCRIBE_STATES, NULL, 0) < 0) {
            efanlamp.connected = 0; close(fd); g_sock = -1; sleep(30); continue;
        }

        efanlamp.connected = 1;
        fprintf(stderr, "[efanlamp] connected to %s:6053 (fan_key=0x%08x light_key=0x%08x)\n",
                host, g_fan_key, g_light_key);

        /* Main receive loop — push updates arrive here */
        for (;;) {
            if (recv_and_dispatch(fd, &g_ctx) < 0) break;
        }

        fprintf(stderr, "[efanlamp] disconnected from %s, retrying in 30s\n", host);
        efanlamp.connected = 0;
        pthread_mutex_lock(&g_send_mutex);
        close(fd); g_sock = -1;
        pthread_mutex_unlock(&g_send_mutex);
        sleep(30);
    }
    return NULL;
}

/* ── public API ─────────────────────────────────────────────────────────── */
int efanlamp_start(void) {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &attr, efanlamp_thread, NULL);
    pthread_attr_destroy(&attr);
    return r;
}

void efanlamp_fan_set(int on, int speed_level) {
    fan_cmd_t *cmd = malloc(sizeof *cmd);
    if (!cmd) return;
    cmd->on = on;
    cmd->speed = speed_level;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, fan_cmd_thread, cmd) != 0) free(cmd);
    pthread_attr_destroy(&attr);
}

void efanlamp_fan_toggle(void) {
    int on = efanlamp.fan_on ? 0 : 1;
    int spd = efanlamp.fan_speed > 0 ? efanlamp.fan_speed : 3;
    efanlamp_fan_set(on, spd);
}

void efanlamp_light_set(int on, int brightness_pct) {
    light_cmd_t *cmd = malloc(sizeof *cmd);
    if (!cmd) return;
    cmd->on = on;
    cmd->brightness_pct = brightness_pct;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, light_cmd_thread, cmd) != 0) free(cmd);
    pthread_attr_destroy(&attr);
}

/* ── standalone test harness ─────────────────────────────────────────────
 * Build:  gcc -DEFANLAMP_TEST_MAIN efanlamp.c sha2.c monocypher.c -lpthread -o eftest
 * Run:    ./eftest [host] [psk_b64]
 * Drives the real Noise handshake + entity discovery with verbose byte dumps. */
#ifdef EFANLAMP_TEST_MAIN
settings_t settings;  /* provide the global the client reads host/psk from */

static int ef_b64(const char *s, uint8_t *out, int max) {
    int bits = 0, n = 0; uint32_t acc = 0;
    for (; *s && n < max; s++) {
        int v;
        if      (*s >= 'A' && *s <= 'Z') v = *s - 'A';
        else if (*s >= 'a' && *s <= 'z') v = *s - 'a' + 26;
        else if (*s >= '0' && *s <= '9') v = *s - '0' + 52;
        else if (*s == '+') v = 62; else if (*s == '/') v = 63;
        else if (*s == '=') break; else continue;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out[n++] = (uint8_t)(acc >> bits); }
    }
    return n;
}

int main(int argc, char **argv) {
    g_ef_verbose = 1;
    const char *host = argc > 1 ? argv[1] : "192.168.3.34";
    const char *psk_b64 = argc > 2 ? argv[2] : "91dT9Peert6ZI1hukAy1AvUzxS0pqgdauqaf7YQ8b04=";
    uint8_t psk[32] = {0};
    int pn = ef_b64(psk_b64, psk, 32);
    fprintf(stderr, "[test] host=%s psk_bytes=%d\n", host, pn);
    ef_hex("psk", psk, 32);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { .tv_sec = 8, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = htons(6053);
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("connect"); return 1; }
    fprintf(stderr, "[test] TCP connected\n");

    memset(&g_ctx, 0, sizeof g_ctx);
    if (do_handshake(fd, &g_ctx, psk) < 0) { fprintf(stderr, "[test] HANDSHAKE FAILED\n"); return 2; }
    fprintf(stderr, "[test] HANDSHAKE OK\n");
    g_sock = fd;

    uint8_t hp[32]; uint8_t *p = hp;
    p = pb_write_str(p, 1, "freetoon");
    p = pb_write_varint(pb_write_tag(p, 2, 0), 1);
    p = pb_write_varint(pb_write_tag(p, 3, 0), 14);
    if (send_message(fd, &g_ctx, ESPHOME_MSG_HELLO_REQUEST, hp, (size_t)(p - hp)) < 0) { fprintf(stderr,"[test] hello send fail\n"); return 3; }
    /* ESPHome 2026.1.0+: auth at Hello; on_client_connected may push log frame before HelloResponse */
    {
        int hello_done = 0;
        for (int limit = 10; limit > 0 && !hello_done; limit--) {
            static uint8_t fbt[4096], pbt[4096];
            int flen = read_noise_frame(fd, fbt, sizeof fbt);
            if (flen < 0) { fprintf(stderr,"[test] hello read fail\n"); return 3; }
            if (session_decrypt(&g_ctx, fbt, (size_t)flen, pbt) != 0) { fprintf(stderr,"[test] hello decrypt fail\n"); return 3; }
            size_t ptl = (size_t)(flen - 16);
            if (ptl < 2) continue;
            uint16_t mt = ((uint16_t)pbt[0] << 8) | pbt[1];
            fprintf(stderr, "[test] hello-phase msg type=%u len=%u\n", mt, (unsigned)(ptl-4 < 0xffff ? ptl-4 : 0));
            if (mt == ESPHOME_MSG_HELLO_RESPONSE) hello_done = 1;
        }
        if (!hello_done) { fprintf(stderr,"[test] HelloResponse not received\n"); return 3; }
    }
    fprintf(stderr, "[test] HelloResponse OK (no ConnectRequest needed in 2026.1.0+)\n");

    send_message(fd, &g_ctx, ESPHOME_MSG_LIST_ENTITIES_REQUEST, NULL, 0);
    for (int limit = 60; limit > 0; limit--) {
        static uint8_t fb[4096], pb2[4096];
        int flen = read_noise_frame(fd, fb, sizeof fb);
        if (flen < 0) break;
        if (session_decrypt(&g_ctx, fb, (size_t)flen, pb2) != 0) { fprintf(stderr,"[test] decrypt fail\n"); break; }
        uint16_t mtype = ((uint16_t)pb2[0] << 8) | pb2[1];
        uint16_t mlen  = ((uint16_t)pb2[2] << 8) | pb2[3];
        size_t pt = (size_t)(flen - 16);
        if (mlen > pt - 4) mlen = (uint16_t)(pt - 4);
        fprintf(stderr, "[test] entity msg type=%u len=%u\n", mtype, mlen);
        if      (mtype == ESPHOME_MSG_LIST_ENTITIES_FAN)   handle_list_fan(pb2+4, mlen);
        else if (mtype == ESPHOME_MSG_LIST_ENTITIES_LIGHT) handle_list_light(pb2+4, mlen);
        else if (mtype == ESPHOME_MSG_LIST_ENTITIES_SERVICES) handle_list_services(pb2+4, mlen);
        else if (mtype == ESPHOME_MSG_LIST_ENTITIES_DONE)  { fprintf(stderr,"[test] ListEntitiesDone\n"); break; }
    }
    fprintf(stderr, "[test] fan_key=0x%08x light_key=0x%08x service_key=0x%08x\n", g_fan_key, g_light_key, g_service_key);

    send_message(fd, &g_ctx, ESPHOME_MSG_SUBSCRIBE_STATES, NULL, 0);
    fprintf(stderr, "[test] subscribed.\n");

    int watch_logs = (argc > 3 && strcmp(argv[3], "logs") == 0);
    if (watch_logs) {
        /* SubscribeLogsRequest = 28, field 1 = level (VERY_VERBOSE = 7) */
        uint8_t lp[4]; uint8_t *q = pb_write_varint(pb_write_tag(lp, 1, 0), 7);
        send_message(fd, &g_ctx, 28, lp, (size_t)(q - lp));
        fprintf(stderr, "[test] subscribed to logs; watching ~60s — trigger fan from remote/HA/Toon now\n");
    }
    /* optional command test: argv[3] = "fan:<0|1>:<speed>" e.g. fan:1:5 */
    if (argc > 3 && strncmp(argv[3], "fan:", 4) == 0) {
        int on = 0, spd = 3;
        sscanf(argv[3] + 4, "%d:%d", &on, &spd);
        fprintf(stderr, "[test] sending FanCommand on=%d speed=%d\n", on, spd);
        efanlamp_fan_set(on, spd);
    }

    int iters = watch_logs ? 100000 : 20;
    time_t t0 = time(NULL);
    for (int i = 0; i < iters; i++) {
        if (watch_logs && time(NULL) - t0 > 60) break;
        /* inline decrypt+dispatch so we can also surface log (type 29) messages */
        static uint8_t fb[4096], pb2[4096];
        int flen = read_noise_frame(fd, fb, sizeof fb);
        if (flen < 0) { if (watch_logs) continue; else break; }
        if (session_decrypt(&g_ctx, fb, (size_t)flen, pb2) != 0) break;
        size_t pt = (size_t)(flen - 16);
        if (pt < 4) continue;
        uint16_t mt = ((uint16_t)pb2[0] << 8) | pb2[1];
        uint16_t ml = ((uint16_t)pb2[2] << 8) | pb2[3];
        if (ml > pt - 4) ml = (uint16_t)(pt - 4);
        if (mt == 29) {  /* SubscribeLogsResponse: field 3 = message string */
            pb_reader_t r = { pb2 + 4, ml };
            int f, w; uint64_t v; const uint8_t *lpp; size_t ll;
            while (pb_next(&r, &f, &w, &v, &lpp, &ll) > 0)
                if (f == 3 && w == 2) fprintf(stderr, "[LOG] %.*s\n", (int)ll, lpp);
        } else if (mt == ESPHOME_MSG_FAN_STATE)   { handle_fan_state(pb2+4, ml);
            fprintf(stderr, "[test] FAN state: on=%d speed=%d\n", efanlamp.fan_on, efanlamp.fan_speed);
        } else if (mt == ESPHOME_MSG_LIGHT_STATE) { handle_light_state(pb2+4, ml);
            fprintf(stderr, "[test] LIGHT state: on=%d bri=%d\n", efanlamp.light_on, efanlamp.light_brightness);
        }
    }
    close(fd);
    return 0;
}
#endif
