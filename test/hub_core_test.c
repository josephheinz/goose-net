/* Unit and integration tests for src/hub_core.c (no Goose needed):
   cc -std=c11 -Wall -Wextra -pthread src/hub_core.c test/hub_core_test.c -o hub_test && ./hub_test */
#define _GNU_SOURCE
#include "../src/hub_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>

static int checks, failed;
#define CHECK(cond) do { checks++; if (!(cond)) { failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define S(x) ((const uint8_t *)(x))
static const uint8_t MASK[4] = {0x37, 0xfa, 0x21, 0x3d};

/* ---------- pure codec tests ---------- */

static void test_sha1_base64(void) {
    uint8_t d[20]; char b[64];
    hub_sha1(S("abc"), 3, d);
    static const uint8_t abc[20] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
                                    0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    CHECK(memcmp(d, abc, 20) == 0);
    hub_sha1(S(""), 0, d);
    CHECK(d[0] == 0xda && d[1] == 0x39 && d[19] == 0x09);
    /* 56 bytes forces a second padding block */
    hub_sha1(S("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), 56, d);
    CHECK(d[0] == 0x84 && d[1] == 0x98 && d[19] == 0xf1);
    size_t n = hub_base64(S("Man"), 3, b); CHECK(n == 4 && memcmp(b, "TWFu", 4) == 0);
    n = hub_base64(S("Ma"), 2, b);  CHECK(n == 4 && memcmp(b, "TWE=", 4) == 0);
    n = hub_base64(S("M"), 1, b);   CHECK(n == 4 && memcmp(b, "TQ==", 4) == 0);
    n = hub_base64(S(""), 0, b);    CHECK(n == 0);
    char key[28];
    hub_ws_accept_key(S("dGhlIHNhbXBsZSBub25jZQ=="), 24, key);
    CHECK(memcmp(key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", 28) == 0);
}

static int utf8(const char *s, size_t n) { return hub_utf8_valid(S(s), n); }

static void test_utf8(void) {
    CHECK(utf8("", 0));
    CHECK(utf8("hello", 5));
    CHECK(utf8("\xc3\xa9", 2));               /* e acute */
    CHECK(utf8("\xe2\x82\xac", 3));           /* euro */
    CHECK(utf8("\xf0\x9f\x98\x80", 4));       /* emoji */
    CHECK(utf8("\xf4\x8f\xbf\xbf", 4));       /* U+10FFFF */
    CHECK(!utf8("\xc0\x80", 2));              /* overlong */
    CHECK(!utf8("\xe0\x80\x80", 3));          /* overlong */
    CHECK(!utf8("\xf0\x80\x80\x80", 4));      /* overlong */
    CHECK(!utf8("\xed\xa0\x80", 3));          /* surrogate */
    CHECK(!utf8("\xf4\x90\x80\x80", 4));      /* above U+10FFFF */
    CHECK(!utf8("\xf5\x80\x80\x80", 4));
    CHECK(!utf8("\x80", 1));                  /* lone continuation */
    CHECK(!utf8("\xc3", 1));                  /* truncated */
    CHECK(!utf8("\xe2\x82", 2));
    CHECK(!utf8("\xc3\x28", 2));              /* bad continuation */
    CHECK(!utf8("\xff", 1));
}

static void test_frames(void) {
    size_t sizes[] = {0, 5, 125, 126, 127, 65535, 65536, 70000};
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        size_t n = sizes[k];
        uint8_t *data = malloc(n + 1);
        for (size_t i = 0; i < n; i++) data[i] = (uint8_t)(i * 7 + 3);
        hub_buf f = {0}, pl = {0};
        hub_ws_frame fr;
        /* server frame: unmasked */
        CHECK(hub_ws_encode(&f, WS_BINARY, 1, data, n, NULL));
        size_t hl = n < 126 ? 2 : n < 65536 ? 4 : 10;
        CHECK(f.len == hl + n);
        CHECK(f.p[0] == 0x82);
        CHECK(f.p[1] == (n < 126 ? n : n < 65536 ? 126 : 127));
        long used = hub_ws_decode(f.p, f.len, 0, &fr, &pl);
        CHECK(used == (long)f.len && fr.fin && fr.opcode == WS_BINARY && !fr.masked &&
              fr.len == n && pl.len == n && memcmp(pl.p, data, n) == 0);
        /* incomplete input waits */
        CHECK(hub_ws_decode(f.p, f.len - 1, 0, &fr, &pl) == 0 || n == 0);
        /* unmasked is a protocol error when masks are required */
        CHECK(hub_ws_decode(f.p, f.len, 1, &fr, &pl) == -1);
        /* client frame: masked */
        f.len = 0;
        CHECK(hub_ws_encode(&f, WS_BINARY, 1, data, n, MASK));
        CHECK(f.len == hl + 4 + n && (f.p[1] & 0x80));
        used = hub_ws_decode(f.p, f.len, 1, &fr, &pl);
        CHECK(used == (long)f.len && fr.masked && pl.len == n && memcmp(pl.p, data, n) == 0);
        hub_buf_free(&f); hub_buf_free(&pl); free(data);
    }
    hub_ws_frame fr;
    long hl;
    /* the RFC 6455 section 5.7 masked "Hello" example */
    static const uint8_t rfc[] = {0x81,0x85,0x37,0xfa,0x21,0x3d,0x7f,0x9f,0x4d,0x51,0x58};
    hub_buf pl = {0};
    CHECK(hub_ws_decode(rfc, sizeof rfc, 1, &fr, &pl) == (long)sizeof rfc);
    CHECK(pl.len == 5 && memcmp(pl.p, "Hello", 5) == 0 && fr.opcode == WS_TEXT);
    hub_buf_free(&pl);
    /* header errors */
    static const uint8_t rsv[] = {0xC1, 0x80, 0, 0, 0, 0};
    CHECK(hub_ws_parse_head(rsv, sizeof rsv, 1, &fr) == -1);
    static const uint8_t badop[] = {0x83, 0x80, 0, 0, 0, 0};
    CHECK(hub_ws_parse_head(badop, sizeof badop, 1, &fr) == -1);
    static const uint8_t fragctl[] = {0x09, 0x80, 0, 0, 0, 0};   /* ping without FIN */
    CHECK(hub_ws_parse_head(fragctl, sizeof fragctl, 1, &fr) == -1);
    static const uint8_t longctl[] = {0x89, 0xFE, 0, 126, 0, 0, 0, 0};
    CHECK(hub_ws_parse_head(longctl, sizeof longctl, 1, &fr) == -1);
    static const uint8_t top[] = {0x82, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(hub_ws_parse_head(top, sizeof top, 1, &fr) == -1);
    static const uint8_t half[] = {0x82, 0xFE, 0x01};
    CHECK(hub_ws_parse_head(half, sizeof half, 1, &fr) == 0);
    CHECK(hub_ws_parse_head(half, 1, 1, &fr) == 0);
    /* 64-bit length parses without the payload being present */
    static const uint8_t big[] = {0x82, 0xFF, 0, 0, 0, 0, 0, 1, 0, 0, 1, 2, 3, 4};
    hl = hub_ws_parse_head(big, sizeof big, 1, &fr);
    CHECK(hl == 14 && fr.len == 65536 && fr.mask[3] == 4);
}

/* ---------- integration harness ---------- */

typedef struct { uint64_t id; int type; hub_buf data, topic; } evt;

typedef struct {
    hub *h;
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    evt *q; size_t n, cap, head;
    volatile int stop;
} runner;

static void *run_main(void *a) {
    runner *r = a;
    hub_buf out = {0}, topic = {0};
    for (;;) {
        uint64_t id;
        int t = hub_next(r->h, &out, &topic, &id);
        pthread_mutex_lock(&r->mu);
        if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 64; r->q = realloc(r->q, r->cap * sizeof *r->q); }
        evt *e = &r->q[r->n++];
        memset(e, 0, sizeof *e);
        e->id = id; e->type = t;
        hub_buf_append(&e->data, out.p, out.len);
        hub_buf_append(&e->topic, topic.p, topic.len);
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->mu);
        if (r->stop || t == HUB_FAILED) break;
    }
    hub_buf_free(&out); hub_buf_free(&topic);
    return NULL;
}

static runner *rn_start(int64_t max_conns, int64_t max_msg, int64_t out_limit, int64_t ping_ms) {
    runner *r = calloc(1, sizeof *r);
    r->h = hub_new(max_conns, max_msg, out_limit, ping_ms);
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->cv, NULL);
    pthread_create(&r->th, NULL, run_main, r);
    return r;
}

static void rn_stop(runner *r) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    r->stop = 1;
    hub_add(r->h, sv[0], HUB_STREAM, S(""), 0, NULL, 0, NULL, 0);   /* wakes next() once */
    pthread_join(r->th, NULL);
    close(sv[1]);
    hub_free(r->h);
    for (size_t i = 0; i < r->n; i++) { hub_buf_free(&r->q[i].data); hub_buf_free(&r->q[i].topic); }
    free(r->q); free(r);
}

/* Takes the next event, waiting up to ms. Copies into *out (caller frees buffers). */
static int rn_wait(runner *r, evt *out, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&r->mu);
    while (r->head == r->n) {
        if (pthread_cond_timedwait(&r->cv, &r->mu, &ts) != 0) break;
    }
    int ok = r->head < r->n;
    if (ok) *out = r->q[r->head++];
    pthread_mutex_unlock(&r->mu);
    return ok;
}

/* Waits for an event of a type and id (id 0 = any); checks its payload when given. */
static int expect(runner *r, int type, uint64_t id, const char *payload) {
    evt e;
    if (!rn_wait(r, &e, 3000)) return 0;
    int ok = e.type == type && (id == 0 || e.id == id);
    if (payload) ok = ok && e.data.len == strlen(payload) && memcmp(e.data.p, payload, e.data.len) == 0;
    return ok;
}

static int no_event(runner *r, int ms) { evt e; return !rn_wait(r, &e, ms); }

static void set_timeout(int fd, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* Makes a connection pair: returns the client fd; *id is the hub's connection id. */
static int connect_hub(runner *r, int kind, const char *topic, const char *pending,
                       const char *hb, uint64_t *id) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    set_timeout(sv[1], 2000);
    *id = hub_add(r->h, sv[0], kind, S(topic), strlen(topic), S(pending), strlen(pending),
                  S(hb), strlen(hb));
    return sv[1];
}

typedef struct { int fd; hub_buf buf; } client;

/* Reads exactly n bytes (or fewer on EOF/timeout); returns the count. */
static size_t read_n(int fd, uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = recv(fd, p + got, n - got, 0);
        if (k <= 0) break;
        got += (size_t)k;
    }
    return got;
}

static int read_is(int fd, const char *want) {
    size_t n = strlen(want);
    uint8_t *b = malloc(n + 1);
    int ok = read_n(fd, b, n) == n && memcmp(b, want, n) == 0;
    free(b);
    return ok;
}

/* Reads one server frame. */
static int read_frame(client *c, hub_ws_frame *f, hub_buf *pl) {
    for (;;) {
        long used = hub_ws_decode(c->buf.p, c->buf.len, 0, f, pl);
        if (used > 0) {
            memmove(c->buf.p, c->buf.p + used, c->buf.len - (size_t)used);
            c->buf.len -= (size_t)used;
            return 1;
        }
        if (used < 0) return 0;
        if (!hub_buf_reserve(&c->buf, 4096)) return 0;
        ssize_t k = recv(c->fd, c->buf.p + c->buf.len, 4096, 0);
        if (k <= 0) return 0;
        c->buf.len += (size_t)k;
    }
}

static void send_frame(int fd, int op, int fin, const void *data, size_t n) {
    hub_buf b = {0};
    hub_ws_encode(&b, op, fin, data, n, MASK);
    send(fd, b.p, b.len, 0);
    hub_buf_free(&b);
}

/* True when the peer has closed (recv sees EOF). */
static int at_eof(int fd) { uint8_t b; return recv(fd, &b, 1, 0) == 0; }

/* Expects a close frame with this code, then EOF. */
static int expect_close_code(client *c, int code) {
    hub_ws_frame f; hub_buf pl = {0};
    int ok = read_frame(c, &f, &pl) && f.opcode == WS_CLOSE && pl.len >= 2 &&
             (pl.p[0] << 8 | pl.p[1]) == code && at_eof(c->fd);
    hub_buf_free(&pl);
    return ok;
}

/* ---------- integration tests ---------- */

static void test_stream(void) {
    runner *r = rn_start(10, 65536, 1 << 20, 0);
    uint64_t a, b, c;
    int fa = connect_hub(r, HUB_STREAM, "news", "", "", &a);
    int fb = connect_hub(r, HUB_STREAM, "news", "ignored input", "", &b);
    int fc = connect_hub(r, HUB_STREAM, "other", "", "", &c);
    CHECK(a == 1 && b == 2 && c == 3);
    CHECK(expect(r, HUB_OPEN, a, NULL));
    CHECK(expect(r, HUB_OPEN, b, NULL));
    CHECK(expect(r, HUB_OPEN, c, NULL));
    CHECK(hub_count(r->h, S("news"), 4) == 2);
    CHECK(hub_count(r->h, S("other"), 5) == 1);
    CHECK(hub_count(r->h, S("none"), 4) == 0);
    CHECK(hub_count(r->h, S(""), 0) == 3);

    CHECK(hub_publish(r->h, S("news"), 4, S("hello"), 5, 0));
    CHECK(read_is(fa, "5\r\nhello\r\n"));
    CHECK(read_is(fb, "5\r\nhello\r\n"));
    CHECK(hub_send(r->h, c, S("0123456789abcdef"), 16, 0));    /* 16 = 0x10 */
    CHECK(read_is(fc, "10\r\n0123456789abcdef\r\n"));
    CHECK(hub_publish(r->h, S("none"), 4, S("x"), 1, 0));       /* nobody listens: fine */
    CHECK(hub_send(r->h, 999, S("x"), 1, 0));                   /* unknown id: dropped */

    /* client input is read and dropped, no event */
    send(fa, "garbage", 7, 0);
    CHECK(no_event(r, 100));

    /* graceful close sends the terminating chunk first */
    hub_close(r->h, a);
    CHECK(read_is(fa, "0\r\n\r\n"));
    CHECK(at_eof(fa));
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    CHECK(hub_count(r->h, S("news"), 4) == 1);

    /* client EOF closes */
    close(fb);
    CHECK(expect(r, HUB_CLOSE, b, NULL));
    close(fc);
    CHECK(expect(r, HUB_CLOSE, c, NULL));
    close(fa);
    CHECK(no_event(r, 100));          /* Close is reported once */
    CHECK(hub_count(r->h, S(""), 0) == 0);
    rn_stop(r);
}

static void test_stream_heartbeat(void) {
    runner *r = rn_start(10, 65536, 1 << 20, 50);
    uint64_t a;
    int fa = connect_hub(r, HUB_STREAM, "t", "", ": ping\n\n", &a);
    CHECK(expect(r, HUB_OPEN, a, NULL));
    CHECK(read_is(fa, "8\r\n: ping\n\n\r\n"));
    CHECK(read_is(fa, "8\r\n: ping\n\n\r\n"));
    close(fa);
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    rn_stop(r);
}

static void test_ids_and_full(void) {
    runner *r = rn_start(2, 65536, 1 << 20, 0);
    uint64_t a, b, c, d;
    int fa = connect_hub(r, HUB_STREAM, "t", "", "", &a);
    int fb = connect_hub(r, HUB_STREAM, "t", "", "", &b);
    int fc = connect_hub(r, HUB_STREAM, "t", "", "", &c);
    CHECK(a == 1 && b == 2 && c == 0);       /* full: refused */
    CHECK(at_eof(fc));                        /* and the fd was closed */
    CHECK(expect(r, HUB_OPEN, a, NULL));
    CHECK(expect(r, HUB_OPEN, b, NULL));
    close(fa);
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    int fd = connect_hub(r, HUB_STREAM, "t", "", "", &d);
    CHECK(d == 3);                            /* a refused add uses no id; ids are not reused */
    CHECK(d > b);
    CHECK(expect(r, HUB_OPEN, d, NULL));
    /* a topic over 64 bytes is refused too */
    char longt[70]; memset(longt, 'x', sizeof longt); longt[69] = 0;
    uint64_t e;
    int fe = connect_hub(r, HUB_STREAM, longt, "", "", &e);
    CHECK(e == 0 && at_eof(fe));
    close(fb); close(fc); close(fd); close(fe);
    rn_stop(r);
}

static void test_ws_basic(void) {
    runner *r = rn_start(10, 1024, 1 << 20, 0);
    uint64_t a;
    int fa = connect_hub(r, HUB_WEBSOCKET, "room", "", "", &a);
    client ca = {fa, {0}};
    CHECK(expect(r, HUB_OPEN, a, NULL));

    /* text and binary arrive as events, with the topic */
    send_frame(fa, WS_TEXT, 1, "hi there", 8);
    evt e;
    CHECK(rn_wait(r, &e, 3000) && e.type == HUB_TEXT && e.id == a && e.data.len == 8 &&
          memcmp(e.data.p, "hi there", 8) == 0 && e.topic.len == 4 && memcmp(e.topic.p, "room", 4) == 0);
    send_frame(fa, WS_BINARY, 1, "\x00\xff\x01", 3);
    CHECK(rn_wait(r, &e, 3000) && e.type == HUB_BINARY && e.data.len == 3 && e.data.p[1] == 0xff);
    /* an empty message */
    send_frame(fa, WS_TEXT, 1, "", 0);
    CHECK(rn_wait(r, &e, 3000) && e.type == HUB_TEXT && e.data.len == 0);

    /* server frames are unmasked text or binary */
    CHECK(hub_send(r->h, a, S("pong!"), 5, 0));
    CHECK(hub_publish(r->h, S("room"), 4, S("\x01\x02"), 2, 1));
    hub_ws_frame f; hub_buf pl = {0};
    CHECK(read_frame(&ca, &f, &pl) && f.opcode == WS_TEXT && !f.masked && pl.len == 5 &&
          memcmp(pl.p, "pong!", 5) == 0);
    CHECK(read_frame(&ca, &f, &pl) && f.opcode == WS_BINARY && pl.len == 2 && pl.p[1] == 2);

    /* ping is answered with a pong carrying the same payload */
    send_frame(fa, WS_PING, 1, "abc", 3);
    CHECK(read_frame(&ca, &f, &pl) && f.opcode == WS_PONG && pl.len == 3 && memcmp(pl.p, "abc", 3) == 0);
    /* an unsolicited pong is ignored */
    send_frame(fa, WS_PONG, 1, "", 0);
    CHECK(no_event(r, 50));

    /* a large message with a 16-bit length, still within max_message */
    uint8_t big[1000]; memset(big, 'z', sizeof big);
    send_frame(fa, WS_BINARY, 1, big, sizeof big);
    CHECK(rn_wait(r, &e, 3000) && e.type == HUB_BINARY && e.data.len == 1000);

    /* a close frame is echoed and the socket closed */
    uint8_t cl[2 + 4] = {0x03, 0xe8, 'b', 'y', 'e', '!'};
    send_frame(fa, WS_CLOSE, 1, cl, sizeof cl);
    CHECK(expect_close_code(&ca, 1000));
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    hub_buf_free(&pl); hub_buf_free(&ca.buf);
    close(fa);
    rn_stop(r);
}

static void test_ws_pending(void) {
    runner *r = rn_start(10, 1024, 1 << 20, 0);
    /* The first frame arrived with the upgrade request: pass it as pending input. */
    hub_buf pend = {0};
    hub_ws_encode(&pend, WS_TEXT, 1, S("early"), 5, MASK);
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    uint64_t id = hub_add(r->h, sv[0], HUB_WEBSOCKET, S("t"), 1, pend.p, pend.len, NULL, 0);
    CHECK(expect(r, HUB_OPEN, id, NULL));
    CHECK(expect(r, HUB_TEXT, id, "early"));
    close(sv[1]);
    CHECK(expect(r, HUB_CLOSE, id, NULL));
    hub_buf_free(&pend);
    rn_stop(r);
}

static void test_ws_fragments(void) {
    runner *r = rn_start(10, 20, 1 << 20, 0);
    uint64_t a;
    int fa = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &a);
    client ca = {fa, {0}};
    CHECK(expect(r, HUB_OPEN, a, NULL));
    hub_ws_frame f; hub_buf pl = {0};

    /* text split in three, with a ping between the parts */
    send_frame(fa, WS_TEXT, 0, "Hel", 3);
    send_frame(fa, WS_PING, 1, "p", 1);
    send_frame(fa, WS_CONT, 0, "lo, ", 4);
    send_frame(fa, WS_CONT, 1, "world", 5);
    CHECK(read_frame(&ca, &f, &pl) && f.opcode == WS_PONG && pl.len == 1 && pl.p[0] == 'p');
    CHECK(expect(r, HUB_TEXT, a, "Hello, world"));

    /* a multi-byte character split across fragments is valid as a whole */
    send_frame(fa, WS_TEXT, 0, "\xe2\x82", 2);
    send_frame(fa, WS_CONT, 1, "\xac", 1);
    CHECK(expect(r, HUB_TEXT, a, "\xe2\x82\xac"));

    /* exactly max_message (20) is fine */
    send_frame(fa, WS_BINARY, 0, "0123456789", 10);
    send_frame(fa, WS_CONT, 1, "0123456789", 10);
    CHECK(expect(r, HUB_BINARY, a, "01234567890123456789"));

    /* one byte over: 1009 */
    send_frame(fa, WS_BINARY, 0, "0123456789", 10);
    send_frame(fa, WS_CONT, 0, "0123456789", 10);
    send_frame(fa, WS_CONT, 1, "x", 1);
    CHECK(expect_close_code(&ca, 1009));
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    close(fa);

    /* a continuation with nothing to continue, and a new data frame mid-message: 1002 */
    fa = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &a); ca.fd = fa; ca.buf.len = 0;
    CHECK(expect(r, HUB_OPEN, a, NULL));
    send_frame(fa, WS_CONT, 1, "x", 1);
    CHECK(expect_close_code(&ca, 1002));
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    close(fa);
    fa = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &a); ca.fd = fa; ca.buf.len = 0;
    CHECK(expect(r, HUB_OPEN, a, NULL));
    send_frame(fa, WS_TEXT, 0, "a", 1);
    send_frame(fa, WS_TEXT, 1, "b", 1);
    CHECK(expect_close_code(&ca, 1002));
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    close(fa);
    hub_buf_free(&pl); hub_buf_free(&ca.buf);
    rn_stop(r);
}

/* One connection that sends a bad frame and must be closed with the given code. */
static void bad_peer(runner *r, int code, int op, int fin, const void *data, size_t n, int masked) {
    uint64_t a;
    int fa = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &a);
    client ca = {fa, {0}};
    CHECK(expect(r, HUB_OPEN, a, NULL));
    if (masked) send_frame(fa, op, fin, data, n);
    else { hub_buf b = {0}; hub_ws_encode(&b, op, fin, data, n, NULL); send(fa, b.p, b.len, 0); hub_buf_free(&b); }
    CHECK(expect_close_code(&ca, code));
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    hub_buf_free(&ca.buf);
    close(fa);
}

static void test_ws_close_codes(void) {
    runner *r = rn_start(10, 100, 1 << 20, 0);
    bad_peer(r, 1002, WS_TEXT, 1, "hi", 2, 0);                  /* unmasked client frame */
    bad_peer(r, 1007, WS_TEXT, 1, "\xff\xfe", 2, 1);            /* invalid UTF-8 text */
    bad_peer(r, 1007, WS_TEXT, 1, "\xc0\x80", 2, 1);            /* overlong */
    bad_peer(r, 1009, WS_BINARY, 1, memset(malloc(101), 'a', 101), 101, 1);   /* too big */
    bad_peer(r, 1002, 3, 1, "x", 1, 1);                         /* reserved opcode */
    bad_peer(r, 1002, WS_PING, 0, "x", 1, 1);                   /* fragmented control frame */
    bad_peer(r, 1002, WS_CLOSE, 1, "\x03", 1, 1);               /* 1-byte close payload */
    bad_peer(r, 1002, WS_CLOSE, 1, "\x03\xed", 2, 1);           /* code 1005 must not be sent */
    bad_peer(r, 1002, WS_CLOSE, 1, "\x00\x05", 2, 1);           /* code 5 */
    bad_peer(r, 1007, WS_CLOSE, 1, "\x03\xe8\xff", 3, 1);       /* reason not UTF-8 */

    /* an RSV bit: build by hand */
    uint64_t a;
    int fa = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &a);
    client ca = {fa, {0}};
    CHECK(expect(r, HUB_OPEN, a, NULL));
    uint8_t rsv[] = {0xC1, 0x80, 1, 2, 3, 4};
    send(fa, rsv, sizeof rsv, 0);
    CHECK(expect_close_code(&ca, 1002));
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    close(fa); hub_buf_free(&ca.buf);

    /* a huge declared length is refused on the header alone, before any payload arrives */
    fa = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &a); ca.fd = fa; ca.buf.len = 0;
    CHECK(expect(r, HUB_OPEN, a, NULL));
    uint8_t huge[] = {0x82, 0xFF, 0, 0, 0, 0, 0x10, 0, 0, 0, 1, 2, 3, 4};
    send(fa, huge, sizeof huge, 0);
    CHECK(expect_close_code(&ca, 1009));
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    close(fa); hub_buf_free(&ca.buf);

    /* a close frame with no payload is echoed with none */
    fa = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &a); ca.fd = fa; ca.buf.len = 0;
    CHECK(expect(r, HUB_OPEN, a, NULL));
    send_frame(fa, WS_CLOSE, 1, "", 0);
    hub_ws_frame f; hub_buf pl = {0};
    CHECK(read_frame(&ca, &f, &pl) && f.opcode == WS_CLOSE && pl.len == 0 && at_eof(fa));
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    close(fa); hub_buf_free(&ca.buf); hub_buf_free(&pl);

    /* hub_close on a WebSocket sends 1000 */
    fa = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &a); ca.fd = fa; ca.buf.len = 0;
    CHECK(expect(r, HUB_OPEN, a, NULL));
    hub_close(r->h, a);
    CHECK(expect_close_code(&ca, 1000));
    CHECK(expect(r, HUB_CLOSE, a, NULL));
    close(fa); hub_buf_free(&ca.buf);
    rn_stop(r);
}

static void test_ws_keepalive(void) {
    runner *r = rn_start(10, 1024, 1 << 20, 80);
    uint64_t a, b;
    int fa = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &a);
    int fb = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &b);
    client ca = {fa, {0}};
    CHECK(expect(r, HUB_OPEN, a, NULL));
    CHECK(expect(r, HUB_OPEN, b, NULL));
    /* the server pings; b answers every ping so it stays, a is silent after its first reply */
    hub_ws_frame f; hub_buf pl = {0};
    CHECK(read_frame(&ca, &f, &pl) && f.opcode == WS_PING && !f.masked && pl.len == 0);
    send_frame(fa, WS_PONG, 1, "", 0);
    /* a stays quiet: closed after 2.5 x 80 ms of silence */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int got_close = 0;
    for (int i = 0; i < 20 && !got_close; i++) {
        evt e;
        if (!rn_wait(r, &e, 200)) continue;
        if (e.type == HUB_CLOSE && e.id == a) got_close = 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(got_close);
    CHECK(ms >= 60 && ms < 1500);
    close(fa);
    hub_buf_free(&pl); hub_buf_free(&ca.buf);
    close(fb);
    rn_stop(r);
}

typedef struct { hub *h; uint64_t id; int ok; } sender;
static void *send_later(void *a) {
    sender *s = a;
    usleep(100000);
    s->ok = hub_send(s->h, s->id, S("woke"), 4, 0);
    return NULL;
}

static void test_wake(void) {
    /* no timers: the poll can only be woken by the pipe */
    runner *r = rn_start(10, 1024, 1 << 20, 0);
    uint64_t a;
    int fa = connect_hub(r, HUB_WEBSOCKET, "t", "", "", &a);
    client ca = {fa, {0}};
    CHECK(expect(r, HUB_OPEN, a, NULL));
    usleep(50000);                       /* let the hub thread settle into poll */
    sender s = {r->h, a, 0};
    pthread_t th;
    pthread_create(&th, NULL, send_later, &s);
    hub_ws_frame f; hub_buf pl = {0};
    CHECK(read_frame(&ca, &f, &pl) && f.opcode == WS_TEXT && pl.len == 4 && memcmp(pl.p, "woke", 4) == 0);
    pthread_join(th, NULL);
    CHECK(s.ok);
    /* several threads sending at once: every message arrives */
    enum { THREADS = 4 };
    pthread_t ts[THREADS];
    sender ss[THREADS];
    for (int i = 0; i < THREADS; i++) {
        ss[i] = (sender){r->h, a, 0};
        pthread_create(&ts[i], NULL, send_later, &ss[i]);
    }
    for (int i = 0; i < THREADS; i++) pthread_join(ts[i], NULL);
    int seen = 0;
    for (int i = 0; i < THREADS; i++) if (read_frame(&ca, &f, &pl)) seen++;
    CHECK(seen == THREADS);
    hub_buf_free(&pl); hub_buf_free(&ca.buf);
    close(fa);
    rn_stop(r);
}

static void test_slow_consumer(void) {
    runner *r = rn_start(10, 1024, 8192, 0);
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int small = 2048;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    uint64_t slow = hub_add(r->h, sv[0], HUB_STREAM, S("t"), 1, NULL, 0, NULL, 0);
    uint64_t good;
    int fg = connect_hub(r, HUB_STREAM, "u", "", "", &good);
    CHECK(expect(r, HUB_OPEN, slow, NULL));
    CHECK(expect(r, HUB_OPEN, good, NULL));
    /* nobody reads sv[1]: the buffer passes out_limit and the connection is dropped */
    uint8_t chunk[1000]; memset(chunk, 'q', sizeof chunk);
    for (int i = 0; i < 200; i++) hub_publish(r->h, S("t"), 1, chunk, sizeof chunk, 0);
    CHECK(expect(r, HUB_CLOSE, slow, NULL));
    CHECK(hub_count(r->h, S("t"), 1) == 0);
    CHECK(hub_count(r->h, S("u"), 1) == 1);      /* a connection on another topic is unaffected */
    CHECK(no_event(r, 100));
    close(sv[1]); close(fg);
    rn_stop(r);

    /* a reader that keeps up is not dropped, even over many times out_limit in total */
    r = rn_start(10, 1024, 8192, 0);
    uint64_t id;
    int fc = connect_hub(r, HUB_STREAM, "t", "", "", &id);
    CHECK(expect(r, HUB_OPEN, id, NULL));
    int ok = 1;
    for (int i = 0; i < 100 && ok; i++) {
        hub_publish(r->h, S("t"), 1, chunk, 1000, 0);
        uint8_t buf[1100];
        ok = read_n(fc, buf, 1000 + 7) == 1007;   /* "3e8\r\n" + 1000 + "\r\n" */
    }
    CHECK(ok);
    CHECK(no_event(r, 50));
    close(fc);
    rn_stop(r);
}

static void test_queue_cap(void) {
    /* No thread runs next(), so the queue only fills. */
    hub *h = hub_new(10, 1024, 1 << 20, 0);
    size_t n = 33u * 1024 * 1024;
    uint8_t *big = calloc(1, n);
    CHECK(hub_send(h, 1, big, n, 0));
    CHECK(!hub_send(h, 1, big, n, 0));            /* 66 MB total: over the 64 MB cap */
    CHECK(!hub_publish(h, S("t"), 1, big, n, 0));
    CHECK(hub_send(h, 1, S("small"), 5, 0));      /* still room for small ones */
    free(big);
    hub_free(h);
}

int main(void) {
    test_sha1_base64();
    test_utf8();
    test_frames();
    test_stream();
    test_stream_heartbeat();
    test_ids_and_full();
    test_ws_basic();
    test_ws_pending();
    test_ws_fragments();
    test_ws_close_codes();
    test_ws_keepalive();
    test_wake();
    test_slow_consumer();
    test_queue_cap();
    printf("%d of %d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}
