/* Hub core: see hub_core.h. One thread runs hub_next and does all socket I/O. Other threads
   only append commands to a mutex-guarded queue and poke a wake pipe. */
#define _GNU_SOURCE
#include "hub_core.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   /* macOS: SO_NOSIGPIPE is set on the socket instead */
#endif

#define CLOSE_GRACE_MS 5000   /* how long a closing connection may take to flush */
#define READ_CHUNK 16384

/* ---------- buffers ---------- */

int hub_buf_reserve(hub_buf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len) return 0;
    if (b->len + extra <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra) {
        if (cap > SIZE_MAX / 2) { cap = b->len + extra; break; }
        cap *= 2;
    }
    uint8_t *p = realloc(b->p, cap);
    if (!p) return 0;
    b->p = p; b->cap = cap;
    return 1;
}

int hub_buf_append(hub_buf *b, const void *data, size_t n) {
    if (!hub_buf_reserve(b, n)) return 0;
    if (n) memcpy(b->p + b->len, data, n);
    b->len += n;
    return 1;
}

void hub_buf_free(hub_buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* ---------- SHA-1 and base64 (for Sec-WebSocket-Accept) ---------- */

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *blk) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)blk[4*i] << 24 | (uint32_t)blk[4*i+1] << 16 | (uint32_t)blk[4*i+2] << 8 | blk[4*i+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void hub_sha1(const uint8_t *data, size_t n, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha1_block(h, data + i);
    uint8_t tail[128] = {0};
    size_t rem = n - i;
    if (rem) memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    size_t total = rem < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int j = 0; j < 8; j++) tail[total - 1 - j] = (uint8_t)(bits >> (8 * j));
    sha1_block(h, tail);
    if (total == 128) sha1_block(h, tail + 64);
    for (int j = 0; j < 5; j++) {
        out[4*j] = (uint8_t)(h[j] >> 24); out[4*j+1] = (uint8_t)(h[j] >> 16);
        out[4*j+2] = (uint8_t)(h[j] >> 8); out[4*j+3] = (uint8_t)h[j];
    }
}

size_t hub_base64(const uint8_t *d, size_t n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i+1] << 8;
        if (i + 2 < n) v |= d[i+2];
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

void hub_ws_accept_key(const uint8_t *key, size_t keylen, char out[28]) {
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t buf[256], dig[20];
    /* Keys are 24 bytes; clamp so a hostile header cannot overflow (the result is then
       just a wrong key, which the caller has already validated against). */
    if (keylen > 200) keylen = 200;
    memcpy(buf, key, keylen);
    memcpy(buf + keylen, GUID, 36);
    hub_sha1(buf, keylen + 36, dig);
    hub_base64(dig, 20, out);
}

/* ---------- UTF-8 ---------- */

int hub_utf8_valid(const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        size_t need; uint32_t cp, min;
        if (c >= 0xC2 && c <= 0xDF)      { need = 1; cp = c & 0x1F; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; min = 0x10000; }
        else return 0;
        if (i + need >= n) return 0;
        for (size_t k = 1; k <= need; k++) {
            if ((p[i+k] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (p[i+k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += need + 1;
    }
    return 1;
}

/* ---------- WebSocket frames ---------- */

long hub_ws_parse_head(const uint8_t *in, size_t n, int require_mask, hub_ws_frame *f) {
    if (n < 2) return 0;
    if (in[0] & 0x70) return -1;                       /* RSV bits: no extensions */
    f->fin = in[0] >> 7;
    f->opcode = in[0] & 15;
    f->masked = in[1] >> 7;
    int op = f->opcode;
    if ((op > 2 && op < 8) || op > 10) return -1;
    uint64_t len = in[1] & 127;
    size_t hl = 2;
    if (len == 126) {
        if (n < 4) return 0;
        len = (uint64_t)in[2] << 8 | in[3]; hl = 4;
    } else if (len == 127) {
        if (n < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | in[2 + i];
        if (len >> 63) return -1;
        hl = 10;
    }
    if (op >= 8 && (!f->fin || len > 125)) return -1;  /* control frames: whole and short */
    if (require_mask && !f->masked) return -1;
    if (f->masked) {
        if (n < hl + 4) return 0;
        memcpy(f->mask, in + hl, 4); hl += 4;
    }
    f->len = len;
    return (long)hl;
}

void hub_ws_unmask(uint8_t *p, size_t n, const uint8_t mask[4]) {
    for (size_t i = 0; i < n; i++) p[i] ^= mask[i & 3];
}

long hub_ws_decode(const uint8_t *in, size_t n, int require_mask, hub_ws_frame *f,
                   hub_buf *payload) {
    long hl = hub_ws_parse_head(in, n, require_mask, f);
    if (hl <= 0) return hl;
    if (f->len > n - (size_t)hl) return 0;
    payload->len = 0;
    if (!hub_buf_append(payload, in + hl, (size_t)f->len)) return -1;
    if (f->masked) hub_ws_unmask(payload->p, (size_t)f->len, f->mask);
    return hl + (long)f->len;
}

int hub_ws_encode(hub_buf *out, int opcode, int fin, const uint8_t *p, size_t n,
                  const uint8_t *mask) {
    uint8_t h[14];
    size_t hl = 2;
    h[0] = (uint8_t)((fin ? 0x80 : 0) | (opcode & 15));
    uint8_t mbit = mask ? 0x80 : 0;
    if (n < 126) h[1] = mbit | (uint8_t)n;
    else if (n < 65536) { h[1] = mbit | 126; h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; hl = 4; }
    else {
        h[1] = mbit | 127;
        for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)n >> (56 - 8 * i));
        hl = 10;
    }
    if (mask) { memcpy(h + hl, mask, 4); hl += 4; }
    if (!hub_buf_reserve(out, hl + n)) return 0;
    hub_buf_append(out, h, hl);
    size_t at = out->len;
    hub_buf_append(out, p, n);
    if (mask) hub_ws_unmask(out->p + at, n, mask);
    return 1;
}

/* ---------- hub ---------- */

typedef struct cmd {
    struct cmd *next;
    enum { C_ADD, C_SEND, C_PUBLISH, C_CLOSE } type;
    uint64_t id;
    int fd, kind, binary;
    uint8_t topic[HUB_MAX_TOPIC];
    size_t topic_len;
    hub_buf data;       /* payload; for C_ADD the pending input */
    hub_buf extra;      /* C_ADD: heartbeat */
} cmd;

typedef struct {
    int fd, kind;
    uint64_t id;
    uint8_t topic[HUB_MAX_TOPIC];
    size_t topic_len;
    int closing, dead;
    int64_t closing_at;
    hub_buf out; size_t out_off;
    hub_buf in;
    hub_buf frag; int frag_op;      /* WebSocket message being joined; frag_op 0 = none */
    hub_buf heartbeat;
    int64_t last_rx, last_ping, last_hb;
} conn;

typedef struct {
    uint64_t id; int type;
    hub_buf data;
    uint8_t topic[HUB_MAX_TOPIC]; size_t topic_len;
} event;

struct hub {
    int64_t max_conns, max_message, out_limit, ping_ms;
    pthread_mutex_t mu;
    cmd *q_head, *q_tail;
    size_t q_bytes;
    int64_t total;              /* connections in the table plus queued adds */
    uint64_t next_id;
    int wake[2];
    /* Hub thread only (the table is also read by hub_count, under mu): */
    conn **conns; size_t n, cap;
    event *ev; size_t ev_head, ev_n, ev_cap;
    struct pollfd *pfds; size_t pfd_cap;
    hub_buf scratch;
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

hub *hub_new(int64_t max_conns, int64_t max_message, int64_t out_limit, int64_t ping_ms) {
    hub *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->max_conns = max_conns > 0 ? max_conns : 1;
    h->max_message = max_message > 0 ? max_message : 65536;
    h->out_limit = out_limit > 0 ? out_limit : 1048576;
    h->ping_ms = ping_ms;
    h->next_id = 1;
    if (pipe(h->wake) != 0) { free(h); return NULL; }
    for (int i = 0; i < 2; i++) {
        set_nonblock(h->wake[i]);
        fcntl(h->wake[i], F_SETFD, FD_CLOEXEC);
    }
    pthread_mutex_init(&h->mu, NULL);
    return h;
}

static void cmd_free(cmd *c) {
    hub_buf_free(&c->data); hub_buf_free(&c->extra); free(c);
}

static void conn_free(conn *c) {
    if (c->fd >= 0) close(c->fd);
    hub_buf_free(&c->out); hub_buf_free(&c->in); hub_buf_free(&c->frag);
    hub_buf_free(&c->heartbeat);
    free(c);
}

void hub_free(hub *h) {
    if (!h) return;
    for (cmd *c = h->q_head, *nx; c; c = nx) {
        nx = c->next;
        if (c->type == C_ADD) close(c->fd);
        cmd_free(c);
    }
    for (size_t i = 0; i < h->n; i++) conn_free(h->conns[i]);
    for (size_t i = h->ev_head; i < h->ev_n; i++) hub_buf_free(&h->ev[i].data);
    free(h->conns); free(h->ev); free(h->pfds);
    hub_buf_free(&h->scratch);
    close(h->wake[0]); close(h->wake[1]);
    pthread_mutex_destroy(&h->mu);
    free(h);
}

/* Appends c to the queue; consumes it. Caller holds no lock. */
static void enqueue(hub *h, cmd *c) {
    pthread_mutex_lock(&h->mu);
    int was_empty = h->q_head == NULL;
    c->next = NULL;
    if (h->q_tail) h->q_tail->next = c; else h->q_head = c;
    h->q_tail = c;
    pthread_mutex_unlock(&h->mu);
    /* One byte per empty->non-empty transition is enough; a full pipe already means a wake. */
    if (was_empty) { char b = 1; ssize_t r = write(h->wake[1], &b, 1); (void)r; }
}

uint64_t hub_add(hub *h, int fd, int kind, const uint8_t *topic, size_t topic_len,
                 const uint8_t *pending, size_t pending_len,
                 const uint8_t *heartbeat, size_t heartbeat_len) {
    cmd *c = NULL;
    if ((kind != HUB_STREAM && kind != HUB_WEBSOCKET) || topic_len > HUB_MAX_TOPIC) goto refuse;
    c = calloc(1, sizeof *c);
    if (!c) goto refuse;
    c->type = C_ADD; c->fd = fd; c->kind = kind;
    if (topic_len) memcpy(c->topic, topic, topic_len);
    c->topic_len = topic_len;
    if (!hub_buf_append(&c->data, pending, pending_len) ||
        !hub_buf_append(&c->extra, heartbeat, heartbeat_len)) goto refuse;
    pthread_mutex_lock(&h->mu);
    if (h->total >= h->max_conns) { pthread_mutex_unlock(&h->mu); goto refuse; }
    h->total++;
    c->id = h->next_id++;
    pthread_mutex_unlock(&h->mu);
    uint64_t id = c->id;
    enqueue(h, c);
    return id;
refuse:
    if (c) cmd_free(c);
    close(fd);
    return 0;
}

/* Reserves queue budget for n payload bytes; 0 when it would pass the cap. */
static int reserve_bytes(hub *h, size_t n) {
    pthread_mutex_lock(&h->mu);
    int ok = n <= HUB_MAX_QUEUE && h->q_bytes + n <= HUB_MAX_QUEUE;
    if (ok) h->q_bytes += n;
    pthread_mutex_unlock(&h->mu);
    return ok;
}

static int queue_data(hub *h, int type, uint64_t id, const uint8_t *topic, size_t tl,
                      const uint8_t *data, size_t n, int binary) {
    if (tl > HUB_MAX_TOPIC) return 0;
    if (!reserve_bytes(h, n)) return 0;
    cmd *c = calloc(1, sizeof *c);
    if (!c || !hub_buf_append(&c->data, data, n)) {
        if (c) cmd_free(c);
        pthread_mutex_lock(&h->mu); h->q_bytes -= n; pthread_mutex_unlock(&h->mu);
        return 0;
    }
    c->type = type; c->id = id; c->binary = binary;
    if (tl) memcpy(c->topic, topic, tl);
    c->topic_len = tl;
    enqueue(h, c);
    return 1;
}

int hub_send(hub *h, uint64_t id, const uint8_t *data, size_t n, int binary) {
    return queue_data(h, C_SEND, id, NULL, 0, data, n, binary);
}

int hub_publish(hub *h, const uint8_t *topic, size_t tl, const uint8_t *data, size_t n,
                int binary) {
    return queue_data(h, C_PUBLISH, 0, topic, tl, data, n, binary);
}

void hub_close(hub *h, uint64_t id) {
    cmd *c = calloc(1, sizeof *c);
    if (!c) return;
    c->type = C_CLOSE; c->id = id;
    enqueue(h, c);
}

int64_t hub_count(hub *h, const uint8_t *topic, size_t tl) {
    pthread_mutex_lock(&h->mu);
    int64_t n = 0;
    if (tl == 0) n = h->total;
    else {
        for (size_t i = 0; i < h->n; i++)
            if (!h->conns[i]->dead && h->conns[i]->topic_len == tl &&
                memcmp(h->conns[i]->topic, topic, tl) == 0) n++;
        for (cmd *c = h->q_head; c; c = c->next)
            if (c->type == C_ADD && c->topic_len == tl && memcmp(c->topic, topic, tl) == 0) n++;
    }
    pthread_mutex_unlock(&h->mu);
    return n;
}

/* ---------- hub thread ---------- */

/* Queues an event; takes the buffer's memory (data may be NULL). */
static void emit(hub *h, conn *c, int type, hub_buf *data) {
    if (h->ev_n == h->ev_cap) {
        if (h->ev_head > 0 && h->ev_head == h->ev_n) h->ev_head = h->ev_n = 0;
        else {
            size_t cap = h->ev_cap ? h->ev_cap * 2 : 64;
            event *e = realloc(h->ev, cap * sizeof *e);
            if (!e) { if (data) hub_buf_free(data); return; }
            h->ev = e; h->ev_cap = cap;
        }
    }
    event *e = &h->ev[h->ev_n++];
    memset(e, 0, sizeof *e);
    e->id = c->id; e->type = type;
    memcpy(e->topic, c->topic, c->topic_len); e->topic_len = c->topic_len;
    if (data) { e->data = *data; memset(data, 0, sizeof *data); }
}

/* Appends bytes to the output buffer; a consumer over out_limit loses the connection. */
static void conn_out(hub *h, conn *c, const void *p, size_t n) {
    if (c->dead) return;
    if (c->out_off == c->out.len) c->out.len = c->out_off = 0;
    else if (c->out_off > 4096 && c->out_off * 2 > c->out.len) {
        memmove(c->out.p, c->out.p + c->out_off, c->out.len - c->out_off);
        c->out.len -= c->out_off; c->out_off = 0;
    }
    size_t pend = c->out.len - c->out_off, limit = (size_t)h->out_limit;
    if (pend > limit || n > limit - pend || !hub_buf_append(&c->out, p, n)) c->dead = 1;
}

static void conn_ws_frame(hub *h, conn *c, int op, const uint8_t *p, size_t n) {
    h->scratch.len = 0;
    if (!hub_ws_encode(&h->scratch, op, 1, p, n, NULL)) { c->dead = 1; return; }
    conn_out(h, c, h->scratch.p, h->scratch.len);
}

static void conn_stream_chunk(hub *h, conn *c, const uint8_t *p, size_t n) {
    if (n == 0) return;                      /* a zero-size chunk would end the body */
    char head[24];
    int hl = snprintf(head, sizeof head, "%zx\r\n", n);
    h->scratch.len = 0;
    if (!hub_buf_append(&h->scratch, head, (size_t)hl) || !hub_buf_append(&h->scratch, p, n) ||
        !hub_buf_append(&h->scratch, "\r\n", 2)) { c->dead = 1; return; }
    conn_out(h, c, h->scratch.p, h->scratch.len);
}

static void conn_begin_close(hub *h, conn *c) {
    if (c->closing) return;
    c->closing = 1;
    c->closing_at = now_ms();
    if (c->kind == HUB_STREAM) conn_out(h, c, "0\r\n\r\n", 5);
}

/* Sends a close frame with code and stops reading. */
static void ws_close_with(hub *h, conn *c, int code) {
    if (c->closing) return;
    uint8_t b[2] = {(uint8_t)(code >> 8), (uint8_t)code};
    conn_ws_frame(h, c, WS_CLOSE, b, 2);
    c->closing = 1;
    c->closing_at = now_ms();
}

static void conn_send(hub *h, conn *c, const uint8_t *p, size_t n, int binary) {
    if (c->closing) return;
    if (c->kind == HUB_STREAM) conn_stream_chunk(h, c, p, n);
    else conn_ws_frame(h, c, binary ? WS_BINARY : WS_TEXT, p, n);
}

static int close_code_ok(int c) {
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) || (c >= 3000 && c <= 4999);
}

/* Consumes whole frames from c->in. */
static void ws_input(hub *h, conn *c) {
    size_t off = 0;
    while (!c->closing && !c->dead && off < c->in.len) {
        hub_ws_frame f;
        long hl = hub_ws_parse_head(c->in.p + off, c->in.len - off, 1, &f);
        if (hl == 0) break;
        if (hl < 0) { ws_close_with(h, c, 1002); break; }
        if (f.opcode == WS_CONT && !c->frag_op) { ws_close_with(h, c, 1002); break; }
        if ((f.opcode == WS_TEXT || f.opcode == WS_BINARY) && c->frag_op) {
            ws_close_with(h, c, 1002); break;
        }
        if (f.opcode < 8) {
            uint64_t have = c->frag_op ? c->frag.len : 0;
            if (f.len > (uint64_t)h->max_message || have + f.len > (uint64_t)h->max_message) {
                ws_close_with(h, c, 1009); break;
            }
        }
        size_t avail = c->in.len - off - (size_t)hl;
        if (f.len > avail) break;                      /* wait for the rest of the payload */
        uint8_t *pl = c->in.p + off + hl;
        size_t pn = (size_t)f.len;
        hub_ws_unmask(pl, pn, f.mask);
        off += (size_t)hl + pn;
        switch (f.opcode) {
        case WS_TEXT: case WS_BINARY: case WS_CONT:
            if (f.opcode != WS_CONT) { c->frag_op = f.opcode; c->frag.len = 0; }
            if (!hub_buf_append(&c->frag, pl, pn)) { c->dead = 1; break; }
            if (f.fin) {
                int op = c->frag_op;
                c->frag_op = 0;
                if (op == WS_TEXT && !hub_utf8_valid(c->frag.p, c->frag.len)) {
                    ws_close_with(h, c, 1007); break;
                }
                emit(h, c, op == WS_TEXT ? HUB_TEXT : HUB_BINARY, &c->frag);
            }
            break;
        case WS_PING:
            conn_ws_frame(h, c, WS_PONG, pl, pn);
            break;
        case WS_PONG:
            break;
        case WS_CLOSE: {
            int code = 0;
            if (pn == 1) { ws_close_with(h, c, 1002); break; }
            if (pn >= 2) {
                code = pl[0] << 8 | pl[1];
                if (!close_code_ok(code)) { ws_close_with(h, c, 1002); break; }
                if (!hub_utf8_valid(pl + 2, pn - 2)) { ws_close_with(h, c, 1007); break; }
                ws_close_with(h, c, code);
            } else {
                conn_ws_frame(h, c, WS_CLOSE, NULL, 0);
                c->closing = 1; c->closing_at = now_ms();
            }
            break;
        }
        }
    }
    if (off) {
        memmove(c->in.p, c->in.p + off, c->in.len - off);
        c->in.len -= off;
    }
}

static void conn_input(hub *h, conn *c) {
    if (c->kind == HUB_STREAM) { c->in.len = 0; return; }
    ws_input(h, c);
}

static void conn_read(hub *h, conn *c) {
    for (int rounds = 0; rounds < 4 && !c->closing && !c->dead; rounds++) {
        if (!hub_buf_reserve(&c->in, READ_CHUNK)) { c->dead = 1; return; }
        ssize_t r = recv(c->fd, c->in.p + c->in.len, READ_CHUNK, 0);
        if (r > 0) {
            c->in.len += (size_t)r;
            c->last_rx = now_ms();
            conn_input(h, c);
            if (r < READ_CHUNK) return;
        } else if (r == 0) { c->dead = 1; return; }
        else if (errno == EINTR) continue;
        else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) c->dead = 1;
            return;
        }
    }
}

static void conn_flush(conn *c) {
    while (!c->dead && c->out_off < c->out.len) {
        ssize_t r = send(c->fd, c->out.p + c->out_off, c->out.len - c->out_off, MSG_NOSIGNAL);
        if (r > 0) c->out_off += (size_t)r;
        else if (r < 0 && errno == EINTR) continue;
        else {
            if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) c->dead = 1;
            return;
        }
    }
}

static int table_add(hub *h, conn *c) {
    pthread_mutex_lock(&h->mu);
    if (h->n == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 64;
        conn **p = realloc(h->conns, cap * sizeof *p);
        if (!p) { pthread_mutex_unlock(&h->mu); return 0; }
        h->conns = p; h->cap = cap;
    }
    h->conns[h->n++] = c;
    pthread_mutex_unlock(&h->mu);
    return 1;
}

static void apply_add(hub *h, cmd *k) {
    conn *c = calloc(1, sizeof *c);
    if (!c) {
        close(k->fd);
        pthread_mutex_lock(&h->mu); h->total--; pthread_mutex_unlock(&h->mu);
        return;
    }
    c->fd = k->fd; c->kind = k->kind; c->id = k->id;
    memcpy(c->topic, k->topic, k->topic_len); c->topic_len = k->topic_len;
    c->heartbeat = k->extra; memset(&k->extra, 0, sizeof k->extra);
    set_nonblock(c->fd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(c->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    c->last_rx = c->last_ping = c->last_hb = now_ms();
    if (!table_add(h, c)) {
        conn_free(c);
        pthread_mutex_lock(&h->mu); h->total--; pthread_mutex_unlock(&h->mu);
        return;
    }
    emit(h, c, HUB_OPEN, NULL);
    if (k->data.len && c->kind == HUB_WEBSOCKET && !c->dead) {
        c->in = k->data; memset(&k->data, 0, sizeof k->data);
        conn_input(h, c);
    }
}

static conn *find(hub *h, uint64_t id) {
    for (size_t i = 0; i < h->n; i++) if (h->conns[i]->id == id) return h->conns[i];
    return NULL;
}

static void drain_commands(hub *h) {
    pthread_mutex_lock(&h->mu);
    cmd *list = h->q_head;
    h->q_head = h->q_tail = NULL;
    h->q_bytes = 0;
    pthread_mutex_unlock(&h->mu);
    for (cmd *k = list, *nx; k; k = nx) {
        nx = k->next;
        switch (k->type) {
        case C_ADD: apply_add(h, k); break;
        case C_SEND: {
            conn *c = find(h, k->id);
            if (c) conn_send(h, c, k->data.p, k->data.len, k->binary);
            break;
        }
        case C_PUBLISH:
            for (size_t i = 0; i < h->n; i++) {
                conn *c = h->conns[i];
                if (c->topic_len == k->topic_len && memcmp(c->topic, k->topic, k->topic_len) == 0)
                    conn_send(h, c, k->data.p, k->data.len, k->binary);
            }
            break;
        case C_CLOSE: {
            conn *c = find(h, k->id);
            if (!c || c->closing) break;
            if (c->kind == HUB_WEBSOCKET) ws_close_with(h, c, 1000);
            else conn_begin_close(h, c);
            break;
        }
        }
        cmd_free(k);
    }
}

/* Removes finished connections and reports one Close for each. */
static void sweep(hub *h, int64_t now) {
    for (size_t i = 0; i < h->n; ) {
        conn *c = h->conns[i];
        int done = c->dead || (c->closing && (c->out_off >= c->out.len ||
                                              now - c->closing_at > CLOSE_GRACE_MS));
        if (!done) { i++; continue; }
        emit(h, c, HUB_CLOSE, NULL);
        pthread_mutex_lock(&h->mu);
        h->conns[i] = h->conns[--h->n];
        h->total--;
        pthread_mutex_unlock(&h->mu);
        conn_free(c);
    }
}

static void timers(hub *h, int64_t now) {
    if (h->ping_ms <= 0) return;
    for (size_t i = 0; i < h->n; i++) {
        conn *c = h->conns[i];
        if (c->closing || c->dead) continue;
        if (c->kind == HUB_WEBSOCKET) {
            if (now - c->last_rx > h->ping_ms * 5 / 2) { c->dead = 1; continue; }
            if (now - c->last_ping >= h->ping_ms) {
                conn_ws_frame(h, c, WS_PING, NULL, 0);
                c->last_ping = now;
            }
        } else if (c->heartbeat.len && now - c->last_hb >= h->ping_ms) {
            conn_stream_chunk(h, c, c->heartbeat.p, c->heartbeat.len);
            c->last_hb = now;
        }
    }
}

/* Next poll timeout in ms (-1 = wait for I/O or a command). */
static int poll_timeout(hub *h, int64_t now) {
    int64_t best = -1;
    for (size_t i = 0; i < h->n; i++) {
        conn *c = h->conns[i];
        int64_t t = -1;
        if (c->closing) t = c->closing_at + CLOSE_GRACE_MS;
        else if (h->ping_ms > 0) {
            if (c->kind == HUB_WEBSOCKET) {
                t = c->last_ping + h->ping_ms;
                int64_t s = c->last_rx + h->ping_ms * 5 / 2 + 1;
                if (s < t) t = s;
            } else if (c->heartbeat.len) t = c->last_hb + h->ping_ms;
        }
        if (t >= 0 && (best < 0 || t < best)) best = t;
    }
    if (best < 0) return -1;
    best -= now;
    return best < 0 ? 0 : (best > 60000 ? 60000 : (int)best);
}

/* One pass: commands, flush, poll, read, timers, sweep. Returns 0 if poll itself failed. */
static int step(hub *h) {
    drain_commands(h);
    for (size_t i = 0; i < h->n; i++) conn_flush(h->conns[i]);
    int64_t now = now_ms();
    sweep(h, now);
    if (h->ev_head < h->ev_n) return 1;           /* events are waiting; hand them out first */

    if (h->pfd_cap < h->n + 1) {
        size_t cap = h->n + 64;
        struct pollfd *p = realloc(h->pfds, cap * sizeof *p);
        if (!p) return 0;
        h->pfds = p; h->pfd_cap = cap;
    }
    h->pfds[0].fd = h->wake[0]; h->pfds[0].events = POLLIN; h->pfds[0].revents = 0;
    for (size_t i = 0; i < h->n; i++) {
        conn *c = h->conns[i];
        short ev = 0;
        if (!c->closing) ev |= POLLIN;
        if (c->out_off < c->out.len) ev |= POLLOUT;
        h->pfds[i + 1].fd = c->fd; h->pfds[i + 1].events = ev; h->pfds[i + 1].revents = 0;
    }
    int rc = poll(h->pfds, (nfds_t)(h->n + 1), poll_timeout(h, now));
    if (rc < 0) return errno == EINTR ? 1 : 0;
    if (h->pfds[0].revents) {
        char buf[64];
        while (read(h->wake[0], buf, sizeof buf) > 0) {}
    }
    for (size_t i = 0; i < h->n; i++) {
        conn *c = h->conns[i];
        short re = h->pfds[i + 1].revents;
        if (!re) continue;
        if (re & POLLNVAL) { c->dead = 1; continue; }
        if (re & POLLOUT) conn_flush(c);
        if (!c->closing && (re & (POLLIN | POLLHUP | POLLERR))) conn_read(h, c);
        else if (re & (POLLHUP | POLLERR)) c->dead = 1;
        if (!c->dead) conn_flush(c);
    }
    now = now_ms();
    timers(h, now);
    for (size_t i = 0; i < h->n; i++) conn_flush(h->conns[i]);
    sweep(h, now);
    return 1;
}

int hub_next(hub *h, hub_buf *out, hub_buf *topic, uint64_t *id) {
    for (;;) {
        if (h->ev_head < h->ev_n) {
            event *e = &h->ev[h->ev_head++];
            int type = e->type;
            *id = e->id;
            out->len = 0; topic->len = 0;
            hub_buf_append(out, e->data.p, e->data.len);
            hub_buf_append(topic, e->topic, e->topic_len);
            hub_buf_free(&e->data);
            if (h->ev_head == h->ev_n) h->ev_head = h->ev_n = 0;
            return type;
        }
        if (!step(h)) { *id = 0; out->len = 0; topic->len = 0; return HUB_FAILED; }
    }
}
