/*
 * Minimal ADB-over-TCP client. Protocol reference:
 *   https://android.googlesource.com/platform/packages/modules/adb/+/HEAD/protocol.txt
 */
#include "adb.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/version.h>

#ifdef __3DS__
#include <3ds.h>
#else
#include <time.h>
#endif

/* ---------- protocol constants ---------- */
#define A_CNXN 0x4e584e43
#define A_AUTH 0x48545541
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_CLSE 0x45534c43
#define A_WRTE 0x45545257

#define A_VERSION   0x01000000
#define MAX_PAYLOAD 4096                 /* what we advertise / send */
#define MAX_RX      (256 * 1024)         /* what we accept */

#define AUTH_TOKEN        1
#define AUTH_SIGNATURE    2
#define AUTH_RSAPUBLICKEY 3

#define LOCAL_ID 1
#define QUEUE_LEN 64
#define CMD_LEN   256
#define WRITE_STALL_MS 6000

/* ---------- state ---------- */
static mbedtls_pk_context       g_pk;
static mbedtls_ctr_drbg_context g_drbg;
static bool g_crypto_ready = false;

static int      g_sock = -1;
static bool     g_connected = false;
static uint32_t g_remote_id = 0;
static bool     g_can_write = false;
static uint64_t g_write_time = 0;

static uint8_t  g_rbuf[24 + MAX_RX];
static size_t   g_rlen = 0;
static uint8_t  g_payload[MAX_RX + 1];

static char g_queue[QUEUE_LEN][CMD_LEN];
static int  g_qhead = 0, g_qcount = 0;

static char g_err[160] = "";
static char g_devname[64] = "";

typedef struct { uint32_t cmd, arg0, arg1, len; uint8_t *data; } amsg;

/* ---------- platform helpers ---------- */
static uint64_t now_ms(void) {
#ifdef __3DS__
    return osGetTime();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void __attribute__((unused)) sleep_ms(int ms) {
#ifdef __3DS__
    svcSleepThread((s64)ms * 1000000LL);
#else
    usleep(ms * 1000);
#endif
}

static int entropy_cb(void *unused, unsigned char *buf, size_t len) {
    (void)unused;
#ifdef __3DS__
    if (R_SUCCEEDED(PS_GenerateRandomBytes(buf, len))) return 0;
    /* fallback: weak, but better than nothing */
    for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)(svcGetSystemTick() >> (i % 7));
    return 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
#endif
}

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

static bool progress(adb_progress_fn cb, void *user, const char *msg) {
    return cb ? cb(msg, user) : true;
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static uint32_t get_le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set_nonblocking(int fd, bool on) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

/* ---------- key handling ---------- */
static int read_file(const char *path, unsigned char **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024) { fclose(f); return -1; }
    *out = malloc(n + 1);
    if (!*out) { fclose(f); return -1; }
    size_t got = fread(*out, 1, n, f);
    fclose(f);
    (*out)[got] = 0;
    *len = got + 1;               /* mbedtls wants the NUL counted for PEM */
    return 0;
}

static int parse_key(const unsigned char *buf, size_t len) {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    return mbedtls_pk_parse_key(&g_pk, buf, len, NULL, 0, mbedtls_ctr_drbg_random, &g_drbg);
#else
    return mbedtls_pk_parse_key(&g_pk, buf, len, NULL, 0);
#endif
}

int adb_init(const char *key_path, adb_progress_fn cb, void *user) {
#ifdef __3DS__
    psInit();
#endif
    mbedtls_pk_init(&g_pk);
    mbedtls_ctr_drbg_init(&g_drbg);
    const char *pers = "firetv-3ds-adb";
    if (mbedtls_ctr_drbg_seed(&g_drbg, entropy_cb, NULL,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        set_err("RNG init failed");
        return -1;
    }
    g_crypto_ready = true;

    unsigned char *buf = NULL;
    size_t len = 0;
    if (read_file(key_path, &buf, &len) == 0) {
        int r = parse_key(buf, len);
        free(buf);
        if (r == 0 && mbedtls_pk_get_type(&g_pk) == MBEDTLS_PK_RSA) return 0;
        mbedtls_pk_free(&g_pk);
        mbedtls_pk_init(&g_pk);
        progress(cb, user, "Existing key unreadable, making a new one");
    }

    progress(cb, user, "Generating RSA key (one-time, can take a few minutes)...");
    if (mbedtls_pk_setup(&g_pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0 ||
        mbedtls_rsa_gen_key(mbedtls_pk_rsa(g_pk), mbedtls_ctr_drbg_random, &g_drbg, 2048, 65537) != 0) {
        set_err("RSA key generation failed");
        return -1;
    }

    static unsigned char pem[8192];
    if (mbedtls_pk_write_key_pem(&g_pk, pem, sizeof(pem)) == 0) {
        FILE *f = fopen(key_path, "wb");
        if (f) { fputs((char *)pem, f); fclose(f); }
    }
    return 0;
}

void adb_shutdown(void) {
    adb_disconnect();
    if (g_crypto_ready) {
        mbedtls_pk_free(&g_pk);
        mbedtls_ctr_drbg_free(&g_drbg);
        g_crypto_ready = false;
    }
#ifdef __3DS__
    psExit();
#endif
}

/* Android's custom public key format: base64(RSAPublicKey) + " user@host" */
static int make_pubkey(char *out, size_t outlen) {
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(g_pk);
    mbedtls_mpi N, E, r32, n0, n0inv, rr;
    uint8_t blob[4 + 4 + 256 + 256 + 4];
    int ret = -1;

    mbedtls_mpi_init(&N); mbedtls_mpi_init(&E); mbedtls_mpi_init(&r32);
    mbedtls_mpi_init(&n0); mbedtls_mpi_init(&n0inv); mbedtls_mpi_init(&rr);

    if (mbedtls_rsa_export(rsa, &N, NULL, NULL, NULL, &E) != 0) goto done;
    if (mbedtls_mpi_size(&N) != 256) goto done;            /* 2048-bit only */

    /* n0inv = -1 / N[0] mod 2^32 */
    if (mbedtls_mpi_lset(&r32, 1) || mbedtls_mpi_shift_l(&r32, 32)) goto done;
    if (mbedtls_mpi_mod_mpi(&n0, &N, &r32)) goto done;
    if (mbedtls_mpi_inv_mod(&n0inv, &n0, &r32)) goto done;
    if (mbedtls_mpi_sub_mpi(&n0inv, &r32, &n0inv)) goto done;

    /* rr = (2^2048)^2 mod N */
    if (mbedtls_mpi_lset(&rr, 1) || mbedtls_mpi_shift_l(&rr, 4096)) goto done;
    if (mbedtls_mpi_mod_mpi(&rr, &rr, &N)) goto done;

    uint8_t tmp[4];
    put_le32(blob, 64);
    if (mbedtls_mpi_write_binary(&n0inv, tmp, 4)) goto done;
    put_le32(blob + 4, ((uint32_t)tmp[0] << 24) | (tmp[1] << 16) | (tmp[2] << 8) | tmp[3]);
    if (mbedtls_mpi_write_binary_le(&N, blob + 8, 256)) goto done;
    if (mbedtls_mpi_write_binary_le(&rr, blob + 264, 256)) goto done;
    if (mbedtls_mpi_write_binary(&E, tmp, 4)) goto done;
    put_le32(blob + 520, ((uint32_t)tmp[0] << 24) | (tmp[1] << 16) | (tmp[2] << 8) | tmp[3]);

    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, outlen, &olen, blob, sizeof(blob))) goto done;
    const char *suffix = " firetv-remote@3ds";
    if (olen + strlen(suffix) + 1 > outlen) goto done;
    strcpy(out + olen, suffix);
    ret = 0;
done:
    mbedtls_mpi_free(&N); mbedtls_mpi_free(&E); mbedtls_mpi_free(&r32);
    mbedtls_mpi_free(&n0); mbedtls_mpi_free(&n0inv); mbedtls_mpi_free(&rr);
    return ret;
}

/* adbd verifies with RSA_verify(NID_sha1, token...): PKCS#1 v1.5 + SHA-1 DigestInfo,
 * with the 20-byte token used directly as the "digest". */
static int sign_token(const uint8_t *token, size_t len, uint8_t sig[256]) {
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(g_pk);
    if (len != 20) return -1;
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    return mbedtls_rsa_pkcs1_sign(rsa, mbedtls_ctr_drbg_random, &g_drbg,
                                  MBEDTLS_MD_SHA1, 20, token, sig);
#else
    return mbedtls_rsa_pkcs1_sign(rsa, mbedtls_ctr_drbg_random, &g_drbg,
                                  MBEDTLS_RSA_PRIVATE, MBEDTLS_MD_SHA1, 20, token, sig);
#endif
}

/* ---------- transport ---------- */
static int send_all(const uint8_t *buf, size_t len, int timeout_ms) {
    uint64_t deadline = now_ms() + timeout_ms;
    while (len > 0) {
        ssize_t n = send(g_sock, buf, len, 0);
        if (n > 0) { buf += n; len -= n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (now_ms() > deadline) return -1;
            struct pollfd p = { g_sock, POLLOUT, 0 };
            poll(&p, 1, 50);
            continue;
        }
        return -1;
    }
    return 0;
}

static int send_msg(uint32_t cmd, uint32_t a0, uint32_t a1, const void *data, uint32_t len) {
    static uint8_t buf[24 + MAX_PAYLOAD];
    if (len > MAX_PAYLOAD) return -1;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += ((const uint8_t *)data)[i];
    put_le32(buf + 0, cmd);
    put_le32(buf + 4, a0);
    put_le32(buf + 8, a1);
    put_le32(buf + 12, len);
    put_le32(buf + 16, sum);
    put_le32(buf + 20, cmd ^ 0xffffffff);
    if (len) memcpy(buf + 24, data, len);
    return send_all(buf, 24 + len, 3000);
}

/* Read whatever is available within timeout. 1 = got data, 0 = nothing, -1 = closed/error. */
static int fill(int timeout_ms) {
    if (g_rlen >= sizeof(g_rbuf)) return 1;
    struct pollfd p = { g_sock, POLLIN, 0 };
    int pr = poll(&p, 1, timeout_ms);
    if (pr <= 0) return 0;
    if (p.revents & (POLLERR | POLLNVAL)) return -1;
    ssize_t n = recv(g_sock, g_rbuf + g_rlen, sizeof(g_rbuf) - g_rlen, 0);
    if (n > 0) { g_rlen += n; return 1; }
    if (n == 0) return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

/* 1 = parsed a message, 0 = need more bytes, -1 = protocol error */
static int try_parse(amsg *m) {
    if (g_rlen < 24) return 0;
    m->cmd  = get_le32(g_rbuf);
    m->arg0 = get_le32(g_rbuf + 4);
    m->arg1 = get_le32(g_rbuf + 8);
    m->len  = get_le32(g_rbuf + 12);
    if (get_le32(g_rbuf + 20) != (m->cmd ^ 0xffffffff) || m->len > MAX_RX) return -1;
    if (g_rlen < 24 + m->len) return 0;
    memcpy(g_payload, g_rbuf + 24, m->len);
    g_payload[m->len] = 0;
    m->data = g_payload;
    g_rlen -= 24 + m->len;
    memmove(g_rbuf, g_rbuf + 24 + m->len, g_rlen);
    return 1;
}

/* Blocking receive with a deadline; keeps calling the progress callback. */
static int recv_msg(amsg *m, uint64_t deadline, const char *status, adb_progress_fn cb, void *user) {
    for (;;) {
        int r = try_parse(m);
        if (r != 0) return r;
        if (now_ms() > deadline) return 0;
        if (!progress(cb, user, status)) return -2;
        if (fill(50) < 0) return -1;
    }
}

static int tcp_connect(const char *ip, int port, int timeout_ms) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (!inet_aton(ip, &a.sin_addr)) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    set_nonblocking(fd, true);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) return fd;
    if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) { close(fd); return -1; }

    struct pollfd p = { fd, POLLOUT, 0 };
    if (poll(&p, 1, timeout_ms) <= 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        close(fd);
        return -1;
    }
#ifdef SO_ERROR
    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err != 0) { close(fd); return -1; }
#endif
    return fd;
}

/* ---------- public API ---------- */
static void parse_banner(const char *b) {
    /* "device::ro.product.name=X;ro.product.model=Y;..." */
    const char *k = strstr(b, "ro.product.model=");
    g_devname[0] = 0;
    if (k) {
        k += strlen("ro.product.model=");
        size_t n = strcspn(k, ";");
        if (n >= sizeof(g_devname)) n = sizeof(g_devname) - 1;
        memcpy(g_devname, k, n);
        g_devname[n] = 0;
    }
    if (!g_devname[0]) strcpy(g_devname, "Android device");
}

int adb_connect(const char *ip, int port, int auth_wait_ms, adb_progress_fn cb, void *user) {
    adb_disconnect();
    if (!g_crypto_ready) { set_err("adb_init not called"); return -1; }

    progress(cb, user, "Connecting...");
    g_sock = tcp_connect(ip, port, 3000);
    if (g_sock < 0) { set_err("Can't reach %s:%d (is ADB debugging on?)", ip, port); return -1; }

    static const char banner[] = "host::";
    if (send_msg(A_CNXN, A_VERSION, MAX_PAYLOAD, banner, sizeof(banner)) < 0) goto io_err;

    bool sent_sig = false, sent_key = false;
    uint64_t deadline = now_ms() + 5000;
    const char *status = "Handshaking...";
    amsg m;
    for (;;) {
        int r = recv_msg(&m, deadline, status, cb, user);
        if (r == -2) { set_err("Cancelled"); goto fail; }
        if (r < 0) goto io_err;
        if (r == 0) {
            set_err(sent_key ? "Timed out waiting for you to accept on the TV" : "No reply from TV");
            goto fail;
        }
        if (m.cmd == A_CNXN) { parse_banner((char *)m.data); break; }
        if (m.cmd != A_AUTH || m.arg0 != AUTH_TOKEN) continue;

        if (!sent_sig) {
            uint8_t sig[256];
            if (sign_token(m.data, m.len, sig) != 0) { set_err("Signing failed"); goto fail; }
            if (send_msg(A_AUTH, AUTH_SIGNATURE, 0, sig, sizeof(sig)) < 0) goto io_err;
            sent_sig = true;
        } else if (!sent_key) {
            static char pub[1024];
            if (make_pubkey(pub, sizeof(pub)) != 0) { set_err("Public key export failed"); goto fail; }
            if (send_msg(A_AUTH, AUTH_RSAPUBLICKEY, 0, pub, strlen(pub) + 1) < 0) goto io_err;
            sent_key = true;
            deadline = now_ms() + auth_wait_ms;
            status = "Accept 'Allow USB debugging?' on the TV";
        } else {
            set_err("TV rejected the key");
            goto fail;
        }
    }

    progress(cb, user, "Opening shell...");
    static const char svc[] = "shell:";
    if (send_msg(A_OPEN, LOCAL_ID, 0, svc, sizeof(svc)) < 0) goto io_err;
    deadline = now_ms() + 5000;
    for (;;) {
        int r = recv_msg(&m, deadline, "Opening shell...", cb, user);
        if (r == -2) { set_err("Cancelled"); goto fail; }
        if (r < 0) goto io_err;
        if (r == 0) { set_err("Shell open timed out"); goto fail; }
        if (m.cmd == A_OKAY && m.arg1 == LOCAL_ID) { g_remote_id = m.arg0; break; }
        if (m.cmd == A_CLSE) { set_err("TV refused shell"); goto fail; }
    }

    g_connected = true;
    g_can_write = true;
    g_qhead = g_qcount = 0;
    g_err[0] = 0;
    return 0;

io_err:
    set_err("Connection lost during handshake");
fail:
    adb_disconnect();
    return -1;
}

void adb_disconnect(void) {
    if (g_sock >= 0) close(g_sock);
    g_sock = -1;
    g_connected = false;
    g_can_write = false;
    g_rlen = 0;
}

bool adb_is_connected(void) { return g_connected; }
const char *adb_last_error(void) { return g_err; }
const char *adb_device_name(void) { return g_devname; }

bool adb_queue(const char *cmd) {
    if (g_qcount >= QUEUE_LEN || strlen(cmd) >= CMD_LEN - 2) return false;
    int idx = (g_qhead + g_qcount) % QUEUE_LEN;
    snprintf(g_queue[idx], CMD_LEN, "%s", cmd);
    g_qcount++;
    return true;
}

static bool is_plain_keyevent(const char *c) {
    return strncmp(c, "input keyevent ", 15) == 0 && !strstr(c, "--");
}

/* Pop one command, merging a run of plain keyevents into a single `input` call. */
static void build_next(char *out, size_t outlen) {
    snprintf(out, outlen, "%s", g_queue[g_qhead]);
    bool merge = is_plain_keyevent(out);
    g_qhead = (g_qhead + 1) % QUEUE_LEN;
    g_qcount--;
    while (merge && g_qcount > 0 && is_plain_keyevent(g_queue[g_qhead])) {
        const char *codes = g_queue[g_qhead] + 15;
        if (strlen(out) + strlen(codes) + 3 >= outlen) break;
        strcat(out, " ");
        strcat(out, codes);
        g_qhead = (g_qhead + 1) % QUEUE_LEN;
        g_qcount--;
    }
    strcat(out, "\n");
}

bool adb_poll(void) {
    if (!g_connected) return false;

    int f;
    while ((f = fill(0)) > 0) {}
    if (f < 0) { set_err("Connection to TV lost"); adb_disconnect(); return false; }

    amsg m;
    int r;
    while ((r = try_parse(&m)) > 0) {
        if (m.cmd == A_WRTE && m.arg1 == LOCAL_ID) {
            send_msg(A_OKAY, LOCAL_ID, m.arg0, NULL, 0);   /* ack shell output */
        } else if (m.cmd == A_OKAY && m.arg1 == LOCAL_ID) {
            g_can_write = true;
        } else if (m.cmd == A_CLSE) {
            set_err("TV closed the shell");
            adb_disconnect();
            return false;
        }
    }
    if (r < 0) { set_err("Protocol error"); adb_disconnect(); return false; }

    if (g_can_write && g_qcount > 0) {
        char cmd[CMD_LEN + 16];
        build_next(cmd, sizeof(cmd));
        if (send_msg(A_WRTE, LOCAL_ID, g_remote_id, cmd, strlen(cmd)) < 0) {
            set_err("Send failed");
            adb_disconnect();
            return false;
        }
        g_can_write = false;
        g_write_time = now_ms();
    } else if (!g_can_write && now_ms() - g_write_time > WRITE_STALL_MS) {
        set_err("TV stopped responding");
        adb_disconnect();
        return false;
    }
    return true;
}

int adb_scan(const char *ip_in_subnet, int port, char out[][16], int max,
             adb_progress_fn cb, void *user) {
    struct in_addr base;
    if (!inet_aton(ip_in_subnet, &base)) return 0;
    uint32_t net = ntohl(base.s_addr) & 0xFFFFFF00u;
    uint32_t self = ntohl(base.s_addr);

    enum { BATCH = 12, WAIT_MS = 500 };   /* 3DS has a small socket limit */
    int found = 0;
    char msg[64];

    for (int start = 1; start < 255 && found < max; start += BATCH) {
        snprintf(msg, sizeof(msg), "Scanning for TV... %d%%", start * 100 / 254);
        if (!progress(cb, user, msg)) return -1;

        struct pollfd pf[BATCH];
        uint32_t ips[BATCH];
        int n = 0;
        for (int h = start; h < start + BATCH && h < 255; h++) {
            uint32_t ip = net | h;
            if (ip == self) continue;
            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            a.sin_addr.s_addr = htonl(ip);
            int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (fd < 0) continue;
            set_nonblocking(fd, true);
            int c = connect(fd, (struct sockaddr *)&a, sizeof(a));
            if (c != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) { close(fd); continue; }
            pf[n].fd = fd; pf[n].events = POLLOUT; pf[n].revents = 0;
            ips[n] = ip;
            n++;
        }

        bool done[BATCH] = { false };
        uint64_t deadline = now_ms() + WAIT_MS;
        while (now_ms() < deadline) {
            if (poll(pf, n, 50) > 0) {
                for (int i = 0; i < n; i++) {
                    if (done[i] || !pf[i].revents) continue;
                    done[i] = true;
                    bool ok = (pf[i].revents & POLLOUT) && !(pf[i].revents & (POLLERR | POLLHUP | POLLNVAL));
#ifdef SO_ERROR
                    if (ok) {
                        int err = 0; socklen_t el = sizeof(err);
                        if (getsockopt(pf[i].fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err) ok = false;
                    }
#endif
                    if (ok && found < max) {
                        struct in_addr ia; ia.s_addr = htonl(ips[i]);
                        snprintf(out[found++], 16, "%s", inet_ntoa(ia));
                    }
                    pf[i].events = 0;   /* stop polling this one */
                }
            }
            bool all = true;
            for (int i = 0; i < n; i++) if (!done[i]) all = false;
            if (all) break;
        }
        for (int i = 0; i < n; i++) close(pf[i].fd);
    }
    return found;
}
