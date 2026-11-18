/* Drives src/loop.c over real sockets: spool mode and the unchanged default. Run with test/spool.sh. */
#define _GNU_SOURCE
#include "goose.h"
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct { const uint8_t *data; int64_t len; } gnet_bytes;
uint64_t gnet_loop_new(int32_t, int64_t, int64_t, int64_t);
void gnet_loop_free(uint64_t);
int32_t gnet_loop_next(uint64_t, gs_rref, int32_t *);
int32_t gnet_loop_count(uint64_t);
int32_t gnet_loop_set_uploads(uint64_t, int64_t, int64_t, gnet_bytes);
void gnet_loop_spool_path(uint64_t, gs_rref);
void gnet_loop_close_lingering(uint64_t, int32_t, int64_t);

enum { REQUEST = 1, TOO_LARGE = 2, SPOOLED = 6, SPOOL_FAILED = 7 };

static int passed, failed;
static void check(int ok, const char *what) {
    if (ok) passed++; else { failed++; printf("FAIL: %s\n", what); }
}

static int listen_fd, port;
static void start_listener(void) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(listen_fd, (struct sockaddr *)&a, sizeof a);
    listen(listen_fd, 16);
    socklen_t n = sizeof a;
    getsockname(listen_fd, (struct sockaddr *)&a, &n);
    port = ntohs(a.sin_port);
}

/* A client thread: connects, waits delay_ms, sends data, keeps the socket open hold_ms, closes. */
typedef struct { const uint8_t *data; size_t len; int delay_ms, hold_ms; pthread_t th; } client;
static void *client_run(void *arg) {
    client *c = arg;
    usleep((useconds_t)c->delay_ms * 1000);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return NULL; }
    size_t off = 0;
    while (off < c->len) {
        ssize_t w = write(fd, c->data + off, c->len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    usleep((useconds_t)c->hold_ms * 1000);
    close(fd);
    return NULL;
}
static void go(client *c, const void *data, size_t len, int delay_ms, int hold_ms) {
    *c = (client){ .data = data, .len = len, .delay_ms = delay_ms, .hold_ms = hold_ms };
    pthread_create(&c->th, NULL, client_run, c);
}
static void join(client *c) { pthread_join(c->th, NULL); }

static char tmpl[] = "/tmp/gnet-spool-XXXXXX";
static int files_in_dir(void) {
    int n = 0;
    DIR *d = opendir(tmpl);
    struct dirent *e;
    while (d && (e = readdir(d))) if (e->d_name[0] != '.') n++;
    if (d) closedir(d);
    return n;
}

static uint8_t *post(size_t *len, const char *type, size_t body, const char *extra) {
    char head[256];
    int h = snprintf(head, sizeof head, "POST /up HTTP/1.1\r\nHost: x\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n",
                     type, body);
    size_t el = strlen(extra);
    uint8_t *p = malloc((size_t)h + body + el);
    memcpy(p, head, (size_t)h);
    for (size_t i = 0; i < body; i++) p[h + i] = (uint8_t)((i * 31 + 7) & 0xff);
    memcpy(p + h + body, extra, el);
    *len = (size_t)h + body + el;
    return p;
}
#define MP "multipart/form-data; boundary=zz"
static const char GET[] = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";

static uint64_t new_loop(int64_t max_req) { return gnet_loop_new(listen_fd, 5000, max_req, 0); }
static int set_up(uint64_t l, int64_t max_up, int64_t over, const char *dir) {
    return gnet_loop_set_uploads(l, max_up, over, (gnet_bytes){ (const uint8_t *)dir, (int64_t)strlen(dir) });
}

/* A client that sends a whole oversized request, then reads the reply to EOF. */
typedef struct { const uint8_t *data; size_t len; size_t sent; char reply[256]; size_t got; int reset; pthread_t th; } uploader;
static void *upload_run(void *arg) {
    uploader *u = arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); u->reset = 1; return NULL; }
    while (u->sent < u->len) {
        ssize_t w = write(fd, u->data + u->sent, u->len - u->sent);
        if (w <= 0) { if (errno == ECONNRESET || errno == EPIPE) u->reset = 1; break; }
        u->sent += (size_t)w;
    }
    for (;;) {
        ssize_t r = recv(fd, u->reply + u->got, sizeof u->reply - 1 - u->got, 0);
        if (r < 0 && errno == ECONNRESET) u->reset = 1;
        if (r <= 0) break;
        u->got += (size_t)r;
        if (u->got >= sizeof u->reply - 1) break;
    }
    close(fd);
    return NULL;
}


/* Sends GET once the uploader has finished, so the loop keeps running (and draining the lingering body) until then. */
typedef struct { uploader *u; client c; pthread_t th; } after;
static void *after_run(void *arg) {
    after *a = arg;
    pthread_join(a->u->th, NULL);
    go(&a->c, GET, sizeof GET - 1, 0, 300);
    join(&a->c);
    return NULL;
}

/* A client that sends data, then holds the connection without closing; done is set when the server closes it. */
typedef struct { const uint8_t *data; size_t len; int done; int ms; pthread_t th; } holder;
static void *hold_run(void *arg) {
    holder *h = arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return NULL; }
    if (write(fd, h->data, h->len) < 0) { close(fd); return NULL; }
    char buf[512];
    struct pollfd p = { fd, POLLIN, 0 };
    /* the reply, then EOF once the loop closes */
    while (poll(&p, 1, h->ms) == 1) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r <= 0) { h->done = 1; break; }
    }
    close(fd);
    return NULL;
}

static int64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char REPLY413[] = "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

static gs_buf *fresh(gs_buf *b) { free(b->data); *b = (gs_buf){0}; return b; }

/* Reads the spooled file named by the loop, deletes it; returns its size, content in *out. */
static long take_file(uint64_t l, uint8_t **out) {
    gs_buf p = {0};
    gnet_loop_spool_path(l, &p);
    char path[512] = {0};
    memcpy(path, p.data, p.len);
    free(p.data);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    *out = malloc((size_t)n + 1);
    if (fread(*out, 1, (size_t)n, f) != (size_t)n) n = -1;
    fclose(f);
    unlink(path);
    return n;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    start_listener();
    mkdtemp(tmpl);
    gs_buf out = {0};
    int32_t fd;
    client c1, c2;
    size_t len;

    /* small multipart is a plain request */
    uint64_t l = new_loop(1 << 20);
    check(set_up(l, 100000, 1000, tmpl) == 1, "set_uploads on a temp dir");
    uint8_t *req = post(&len, MP, 500, "");
    go(&c1, req, len, 0, 300);
    int ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == REQUEST && out.len == len && !memcmp(out.data, req, len), "small multipart is EV_REQUEST, whole");
    check(files_in_dir() == 0, "small multipart makes no file");
    join(&c1); free(req);
    gnet_loop_free(l);

    /* big multipart is spooled, with a pipelined request behind it */
    l = new_loop(1000);
    set_up(l, 500000, 1000, tmpl);
    req = post(&len, MP, 200000, GET);
    size_t hl = len - 200000 - (sizeof GET - 1);
    go(&c1, req, len, 0, 600);
    ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == SPOOLED, "big multipart is EV_SPOOLED");
    check(out.len == hl && !memcmp(out.data, req, hl) && !memcmp(out.data + hl - 4, "\r\n\r\n", 4), "out holds exactly the head");
    uint8_t *body = NULL;
    long n = take_file(l, &body);
    check(n == 200000 && body && !memcmp(body, req + hl, 200000), "spool file holds the exact body");
    free(body);
    ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == REQUEST && out.len == sizeof GET - 1 && !memcmp(out.data, GET, out.len), "pipelined request after a spooled body is framed");
    check(files_in_dir() == 0, "no file left after take");
    join(&c1); free(req);
    gnet_loop_free(l);

    /* body at the threshold is not spooled */
    l = new_loop(1 << 20);
    set_up(l, 500000, 1000, tmpl);
    req = post(&len, MP, 1000, "");
    go(&c1, req, len, 0, 300);
    ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == REQUEST && files_in_dir() == 0, "length == spool_over stays in memory");
    join(&c1); free(req);
    gnet_loop_free(l);

    /* limits */
    l = new_loop(2000);
    set_up(l, 5000, 100, tmpl);
    req = post(&len, "application/x-www-form-urlencoded", 3000, "");
    go(&c1, req, len, 0, 300);
    ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == TOO_LARGE, "non-multipart over max_request is EV_TOO_LARGE");
    join(&c1); free(req);
    gnet_loop_free(l);

    l = new_loop(2000);
    set_up(l, 5000, 100, tmpl);
    req = post(&len, "Multipart/Form-Data; boundary=q", 6000, "");
    go(&c1, req, len, 0, 300);
    ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == TOO_LARGE && files_in_dir() == 0, "multipart over max_upload is EV_TOO_LARGE");
    hl = len - 6000;
    check(out.len == hl && !memcmp(out.data, req, hl), "EV_TOO_LARGE hands out the head through its blank line");
    join(&c1); free(req);
    gnet_loop_free(l);

    /* close_lingering: the client sends its whole refused body and still reads the reply, without a reset */
    {
        l = new_loop(2000);
        uploader u = {0};
        uint8_t *big = post(&len, "application/octet-stream", 3000000, "");
        u.data = big; u.len = len;
        pthread_create(&u.th, NULL, upload_run, &u);
        ev = gnet_loop_next(l, fresh(&out), &fd);
        check(ev == TOO_LARGE && out.len == len - 3000000, "a 3 MB body over 2000 bytes: EV_TOO_LARGE with the head");
        check(write(fd, REPLY413, sizeof REPLY413 - 1) == (ssize_t)(sizeof REPLY413 - 1), "the reply is written");
        gnet_loop_close_lingering(l, fd, 5000);
        check(gnet_loop_count(l) == 1, "a lingering connection stays in the loop");
        /* a later request unblocks next once the uploader is done; the loop reads and drops its body meanwhile */
        go(&c2, GET, sizeof GET - 1, 0, 300);
        int64_t t0 = ms_now();
        ev = gnet_loop_next(l, fresh(&out), &fd);
        check(ev == REQUEST && out.len == sizeof GET - 1, "requests are still served while one connection lingers");
        after a = { .u = &u };
        pthread_create(&a.th, NULL, after_run, &a);
        ev = gnet_loop_next(l, fresh(&out), &fd);
        check(ev == REQUEST, "the loop drained the lingering body until the uploader finished");
        pthread_join(a.th, NULL);
        check(u.sent == len, "the client sent its whole body");
        check(!u.reset, "the client saw no connection reset");
        check(u.got >= 12 && !memcmp(u.reply, "HTTP/1.1 413", 12), "the client read the 413");
        join(&c2);
        (void)t0;
        gnet_loop_free(l);
        free(big);
    }

    /* close_lingering gives up after ms when the client never closes */
    {
        l = new_loop(2000);
        uint8_t *big = post(&len, "application/octet-stream", 5000, "");
        holder h = { .data = big, .len = len - 3000, .ms = 4000 };   /* part of the body, then silence */
        pthread_create(&h.th, NULL, hold_run, &h);
        ev = gnet_loop_next(l, fresh(&out), &fd);
        check(ev == TOO_LARGE, "a partial body over the limit: EV_TOO_LARGE");
        if (write(fd, REPLY413, sizeof REPLY413 - 1) < 0) check(0, "write the reply");
        int64_t t0 = ms_now();
        gnet_loop_close_lingering(l, fd, 300);
        go(&c2, GET, sizeof GET - 1, 1600, 300);
        ev = gnet_loop_next(l, fresh(&out), &fd);
        check(ev == REQUEST, "a later request is served");
        pthread_join(h.th, NULL);
        check(h.done, "the lingering connection was closed");
        check(ms_now() - t0 < 3500, "before the client gave up");
        check(gnet_loop_count(l) == 1, "only the later connection is left");
        join(&c2);
        gnet_loop_free(l);
        free(big);
    }

    /* mixed case content-type header spools */
    l = new_loop(2000);
    set_up(l, 50000, 100, tmpl);
    req = post(&len, "MULTIPART/FORM-DATA; boundary=q", 4000, "");
    go(&c1, req, len, 0, 300);
    ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == SPOOLED, "content-type matched case-insensitively");
    body = NULL; n = take_file(l, &body); free(body);
    check(n == 4000, "spooled body between max_request and max_upload");
    join(&c1); free(req);
    gnet_loop_free(l);

    /* client closes mid-body: file is removed */
    l = new_loop(2000);
    set_up(l, 500000, 1000, tmpl);
    req = post(&len, MP, 100000, "");
    go(&c1, req, len / 2, 0, 100);
    go(&c2, GET, sizeof GET - 1, 500, 100);
    ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == REQUEST && out.len == sizeof GET - 1, "later request served after a dropped upload");
    check(files_in_dir() == 0, "client closing mid-body leaves no file");
    check(gnet_loop_count(l) == 1, "dropped connection is gone");
    join(&c1); join(&c2); free(req);
    gnet_loop_free(l);

    /* loop freed while a body spools: file is removed */
    l = new_loop(2000);
    set_up(l, 500000, 1000, tmpl);
    req = post(&len, MP, 100000, "");
    go(&c1, req, len / 2, 0, 1500);
    go(&c2, GET, sizeof GET - 1, 400, 100);
    ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == REQUEST, "request served while another body spools");
    check(files_in_dir() == 1, "a spooling connection holds one file");
    gnet_loop_free(l);
    check(files_in_dir() == 0, "gnet_loop_free removes files of spooling connections");
    join(&c1); join(&c2); free(req);

    /* write failure: spool dir removed after set_uploads */
    {
        char d2[] = "/tmp/gnet-spool-gone-XXXXXX";
        mkdtemp(d2);
        l = new_loop(2000);
        set_up(l, 500000, 1000, d2);
        rmdir(d2);
        req = post(&len, MP, 5000, "");
        go(&c1, req, len, 0, 300);
        ev = gnet_loop_next(l, fresh(&out), &fd);
        check(ev == SPOOL_FAILED, "unopenable spool file is EV_SPOOL_FAILED");
        join(&c1); free(req);
        gnet_loop_free(l);
    }

    /* without set_uploads: as before */
    l = new_loop(100000);
    req = post(&len, MP, 50000, "");
    go(&c1, req, len, 0, 300);
    ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == REQUEST && out.len == len && files_in_dir() == 0, "no set_uploads: big multipart is EV_REQUEST in memory");
    join(&c1); free(req);
    gnet_loop_free(l);

    l = new_loop(2000);
    req = post(&len, MP, 5000, "");
    go(&c1, req, len, 0, 300);
    ev = gnet_loop_next(l, fresh(&out), &fd);
    check(ev == TOO_LARGE, "no set_uploads: multipart uses max_request");
    join(&c1); free(req);
    gnet_loop_free(l);

    /* set_uploads refusals */
    l = new_loop(2000);
    check(set_up(l, 5000, 100, "/nonexistent-gnet-dir") == 0, "missing dir -> false");
    check(set_up(l, 5000, 100, "/etc/hosts") == 0, "file, not a dir -> false");
    if (geteuid() != 0) {
        char ro[] = "/tmp/gnet-spool-ro-XXXXXX";
        mkdtemp(ro);
        chmod(ro, 0500);
        check(set_up(l, 5000, 100, ro) == 0, "read-only dir -> false");
        chmod(ro, 0700);
        rmdir(ro);
    }
    gnet_loop_free(l);

    rmdir(tmpl);
    free(out.data);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
