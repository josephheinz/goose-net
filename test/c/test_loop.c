#define _GNU_SOURCE
#include "goose.h"
#include "check.h"

#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct { const uint8_t *data; int64_t len; } bytes;

void gnet_stop_on_signals(int64_t delay_ms, int64_t drain_ms);
void gnet_request_stop(void);
int64_t gnet_stop_phase(void);
int32_t gnet_listen(bytes host, int32_t port, int32_t backlog);
uint64_t gnet_loop_new(int32_t listen_fd, int64_t idle_ms, int64_t max_request, int64_t accept_max);
int32_t gnet_loop_next(uint64_t h, gs_rref out, int32_t *fd_out);
void gnet_loop_free(uint64_t h);
int32_t gnet_loop_count(uint64_t h);

enum { EV_REQUEST = 1, EV_STOPPED = 8 };   /* src/loop.c: 6 and 7 are the spool events */

#define REQ "GET / HTTP/1.1\r\nHost: x\r\n\r\n"

static const bytes LOCAL = { (const uint8_t *)"127.0.0.1", 9 };

typedef struct { int listen_fd, port; uint64_t loop; } rig;

static rig setup(int64_t delay_ms, int64_t drain_ms) {
    rig r;
    r.listen_fd = gnet_listen(LOCAL, 0, 16);
    struct sockaddr_in sa;
    socklen_t len = sizeof sa;
    getsockname(r.listen_fd, (struct sockaddr *)&sa, &len);
    r.port = ntohs(sa.sin_port);
    r.loop = gnet_loop_new(r.listen_fd, 0, 1 << 20, 0);
    gnet_stop_on_signals(delay_ms, drain_ms);
    return r;
}

static int dial(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("connect"); exit(2); }
    return fd;
}

static void put(int fd, const char *s) {
    if (send(fd, s, strlen(s), 0) < 0) perror("send");
}

/* The next event, with the request bytes in *got when there is one. */
static int next(rig *r, gs_buf *got, int32_t *fd) {
    got->len = 0;
    return gnet_loop_next(r->loop, got, fd);
}

/* True when the peer has closed the connection. */
static int closed_by_loop(int fd, int ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, ms) != 1) return 0;
    char c;
    return recv(fd, &c, 1, 0) == 0;
}

static void case_baseline(void) {
    rig r = setup(5000, 1000);
    gs_buf got = { 0 };
    int32_t fd;
    int c = dial(r.port);
    put(c, REQ);
    CHECK(next(&r, &got, &fd) == EV_REQUEST && got.len == strlen(REQ), "a request is served before a stop");
    CHECK(gnet_loop_count(r.loop) == 1, "the connection stays open");
}

static void case_drain(void) {
    rig r = setup(0, 5000);
    gs_buf got = { 0 };
    int32_t fd;
    int idle = dial(r.port);
    int busy = dial(r.port);
    put(busy, REQ);
    nap_ms(50);
    CHECK(next(&r, &got, &fd) == EV_REQUEST, "the first request is served");
    CHECK(gnet_loop_count(r.loop) == 2, "both connections are open");

    gnet_request_stop();
    CHECK(WAIT_FOR(gnet_stop_phase() == 2, 2000), "phase 2");
    put(busy, REQ);
    nap_ms(50);
    int64_t t0 = mono_ms();
    CHECK(next(&r, &got, &fd) == EV_REQUEST && got.len == strlen(REQ), "a whole request is still handed out while draining");
    CHECK(next(&r, &got, &fd) == EV_STOPPED, "then the loop reports Stopped");
    CHECK(mono_ms() - t0 < 1000, "without waiting for the deadline");
    CHECK(fd == -1, "Stopped carries no connection");
    CHECK(closed_by_loop(idle, 1000), "the idle connection was closed");
    CHECK(closed_by_loop(busy, 1000), "the served connection is closed with the loop");
    CHECK(gnet_loop_count(r.loop) == 0, "no connection is left");
    CHECK(next(&r, &got, &fd) == EV_STOPPED, "Stopped repeats");
}

typedef struct { int fd; } late;

static void *send_rest(void *arg) {
    nap_ms(300);
    put(((late *)arg)->fd, "st: x\r\n\r\n");
    return NULL;
}

/* A connection holding the first bytes of a second request, so it is not idle. */
static int partial_client(rig *r, gs_buf *got) {
    int32_t fd;
    int c = dial(r->port);
    put(c, REQ "GET / HTTP/1.1\r\nHo");
    CHECK(next(r, got, &fd) == EV_REQUEST, "the whole request comes first");
    return c;
}

static void case_partial_finishes(void) {
    rig r = setup(0, 3000);
    gs_buf got = { 0 };
    int32_t fd;
    late l = { partial_client(&r, &got) };
    gnet_request_stop();
    pthread_t t;
    pthread_create(&t, NULL, send_rest, &l);
    int64_t t0 = mono_ms();
    CHECK(next(&r, &got, &fd) == EV_REQUEST, "a partial request finishes within drain_ms");
    int64_t took = mono_ms() - t0;
    CHECK(took >= 250 && took < 2000, "it was handed out when it completed");
    CHECK(got.len == strlen("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), "with all its bytes");
    CHECK(next(&r, &got, &fd) == EV_STOPPED, "then Stopped");
    pthread_join(t, NULL);
}

static void case_partial_dropped(void) {
    rig r = setup(0, 400);
    gs_buf got = { 0 };
    int32_t fd;
    int c = partial_client(&r, &got);
    gnet_request_stop();
    int64_t t0 = mono_ms();
    CHECK(next(&r, &got, &fd) == EV_STOPPED, "a partial request past drain_ms is dropped");
    int64_t took = mono_ms() - t0;
    CHECK(took >= 350 && took < 1500, "Stopped came after about drain_ms");
    CHECK(closed_by_loop(c, 1000), "its connection was closed");
}

static void case_phase1_accepts(void) {
    rig r = setup(5000, 1000);
    gs_buf got = { 0 };
    int32_t fd;
    gnet_request_stop();
    CHECK(gnet_stop_phase() == 1, "phase 1");
    int c = dial(r.port);
    put(c, REQ);
    CHECK(next(&r, &got, &fd) == EV_REQUEST, "a new connection is served in phase 1");
}

static void case_phase2_refuses(void) {
    rig r = setup(0, 400);
    gs_buf got = { 0 };
    int32_t fd;
    int holder = partial_client(&r, &got);
    gnet_request_stop();
    CHECK(WAIT_FOR(gnet_stop_phase() == 2, 2000), "phase 2");
    int late_client = dial(r.port);
    put(late_client, REQ);
    int ev = next(&r, &got, &fd);
    CHECK(ev == EV_STOPPED, "a connection made in phase 2 is never served");
    CHECK(closed_by_loop(holder, 1000), "the holder was closed at the deadline");
    struct pollfd p = { late_client, POLLIN, 0 };
    CHECK(poll(&p, 1, 100) == 0, "the late client was neither answered nor closed");
}

typedef struct { uint64_t loop; int result; } runner;

static void *run_loop(void *arg) {
    runner *ru = arg;
    gs_buf got = { 0 };
    int32_t fd;
    ru->result = gnet_loop_next(ru->loop, &got, &fd);
    return NULL;
}

static void case_two_loops(void) {
    rig r = setup(0, 2000);
    runner a = { r.loop, 0 }, b = { gnet_loop_new(r.listen_fd, 0, 1 << 20, 0), 0 };
    pthread_t ta, tb;
    pthread_create(&ta, NULL, run_loop, &a);
    pthread_create(&tb, NULL, run_loop, &b);
    nap_ms(100);
    int64_t t0 = mono_ms();
    gnet_request_stop();
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK(a.result == EV_STOPPED && b.result == EV_STOPPED, "both loops on one listener report Stopped");
    CHECK(mono_ms() - t0 < 1500, "promptly");
    int probe = dial(r.port);
    CHECK(probe >= 0, "the listening socket is still open");
}

int main(void) {
    run_case("baseline", case_baseline);
    run_case("drain", case_drain);
    run_case("partial finishes", case_partial_finishes);
    run_case("partial dropped", case_partial_dropped);
    run_case("phase 1 accepts", case_phase1_accepts);
    run_case("phase 2 refuses", case_phase2_refuses);
    run_case("two loops", case_two_loops);
    return finish();
}
