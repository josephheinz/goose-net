/* The connection loop behind net::Loop: one thread polls the listening socket and every
   open connection. Ready connections are served round robin; responses are written by
   the caller with a blocking send on the connection's fd, under a send timeout. */
#define _GNU_SOURCE  /* memmem on glibc */
#include "goose.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* What gnet_loop_next found. The Goose side mirrors these in net::Event. */
enum { EV_FAILED = -1, EV_REQUEST = 1, EV_TOO_LARGE = 2, EV_HEAD_TOO_LARGE = 3,
       EV_UNSUPPORTED = 4, EV_BAD_LENGTH = 5, EV_SPOOLED = 6, EV_SPOOL_FAILED = 7, EV_STOPPED = 8 };

int32_t gnet_wake_fd(void);
int64_t gnet_drain_deadline_ms(void);

#define MAX_HEAD 65536
#define PATH_MAX_ 320

typedef struct { const uint8_t *data; int64_t len; } gnet_bytes;

typedef struct {
    int fd;
    uint8_t *buf;
    size_t len, cap;
    int64_t last_ms;
    int spfd;                   /* spool file, -1 when the body is not spooled */
    char *spath;
    size_t hlen;                /* spooling: head bytes (through \r\n\r\n) at the start of buf */
    int64_t remain;             /* spooling: body bytes still to come */
    int failed;                 /* spooling: a write failed */
} gnet_conn;

typedef struct {
    int listen_fd;
    int64_t idle_ms, max_request;
    int64_t accept_max;         /* connections accepted per poll pass; 0 = all */
    int64_t since_poll;         /* requests handed out since the last poll pass */
    gnet_conn *conns;
    size_t count, cap, next;    /* next: where the round robin resumes */
    int wake_fd;                /* readable once the process reaches the drain phase */
    int draining;
    int64_t deadline_ms;        /* when a draining loop gives up on the connections left */
    struct pollfd *pfds;
    size_t pfd_cap;
    int64_t max_upload, spool_over;   /* set_uploads; max_upload 0 = off */
    char dir[256];
    uint64_t spool_n;
    char last_path[PATH_MAX_];        /* the file of the last EV_SPOOLED */
} gnet_loop;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_blocking(int fd, int blocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
}

static void spool_discard(gnet_conn *c) {
    if (c->spfd < 0) return;
    close(c->spfd);
    unlink(c->spath);
    free(c->spath);
    c->spfd = -1;
    c->spath = NULL;
}

static void remove_at(gnet_loop *l, size_t i) {
    spool_discard(&l->conns[i]);
    free(l->conns[i].buf);
    l->conns[i] = l->conns[--l->count];
    if (l->next > l->count) l->next = 0;
}

static void drop(gnet_loop *l, size_t i) {
    close(l->conns[i].fd);
    remove_at(l, i);
}

/* Value of a Content-Length header inside head[0..len): -1 when absent,
   -2 when malformed. Sets *chunked for Transfer-Encoding: chunked and
   *multi for Content-Type: multipart/form-data. */
static int64_t scan_head(const uint8_t *head, size_t len, int *chunked, int *multi) {
    int64_t length = -1;
    *chunked = 0;
    *multi = 0;
    size_t i = 0;
    while (i < len) {
        const uint8_t *eol = memchr(head + i, '\n', len - i);
        size_t end = eol ? (size_t)(eol - head) : len;
        const char *line = (const char *)head + i;
        size_t n = end - i;
        if (n >= 15 && strncasecmp(line, "content-length:", 15) == 0) {
            int64_t v = 0; size_t j = 15; int digits = 0;
            while (j < n && (line[j] == ' ' || line[j] == '\t')) j++;
            while (j < n && line[j] >= '0' && line[j] <= '9') {
                if (v > (INT64_MAX - 9) / 10) return -2;
                v = v * 10 + (line[j++] - '0'); digits++;
            }
            while (j < n && (line[j] == ' ' || line[j] == '\t' || line[j] == '\r')) j++;
            if (!digits || j != n) return -2;
            length = v;
        } else if (n >= 13 && strncasecmp(line, "content-type:", 13) == 0) {
            size_t j = 13;
            while (j < n && (line[j] == ' ' || line[j] == '\t')) j++;
            if (n - j >= 19 && strncasecmp(line + j, "multipart/form-data", 19) == 0) *multi = 1;
        } else if (n >= 18 && strncasecmp(line, "transfer-encoding:", 18) == 0) {
            for (size_t j = 18; j + 7 <= n; j++)
                if (strncasecmp(line + j, "chunked", 7) == 0) { *chunked = 1; break; }
        }
        i = end + 1;
    }
    return length;
}

/* Writes all of p; 0 on failure. */
static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 0;
        p += w; n -= (size_t)w;
    }
    return 1;
}

/* Starts spooling c's body (head is the head's size without the blank line, length the body size):
   the buffered body bytes go to a new file and the rest of the buffer is kept. 0 on failure. */
static int begin_spool(gnet_loop *l, gnet_conn *c, size_t head, int64_t length) {
    char *path = malloc(PATH_MAX_);
    if (!path) return 0;
    snprintf(path, PATH_MAX_, "%s/gnet-%d-%llu", l->dir, (int)getpid(), (unsigned long long)l->spool_n++);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) { free(path); return 0; }
    size_t hlen = head + 4, have = c->len - hlen;
    size_t take = have < (uint64_t)length ? have : (size_t)length;
    if (!write_all(fd, c->buf + hlen, take)) { close(fd); unlink(path); free(path); return 0; }
    memmove(c->buf + hlen, c->buf + hlen + take, have - take);
    c->len -= take;
    c->spfd = fd; c->spath = path; c->hlen = hlen;
    c->remain = length - (int64_t)take;
    return 1;
}

/* How much of the buffer is one whole request: > 0 its size, 0 more is
   needed (also while a body spools), < 0 an EV_ code for a request that
   cannot be served. */
static int64_t frame(gnet_loop *l, gnet_conn *c) {
    if (c->len < 4) return 0;
    const uint8_t *end = memmem(c->buf, c->len, "\r\n\r\n", 4);
    if (!end) return c->len > MAX_HEAD ? -EV_HEAD_TOO_LARGE : 0;
    size_t head = (size_t)(end - c->buf);
    int chunked, multi;
    int64_t length = scan_head(c->buf, head, &chunked, &multi);
    if (chunked) return -EV_UNSUPPORTED;
    if (length == -2) return -EV_BAD_LENGTH;
    if (length < 0) length = 0;
    int64_t limit = multi && l->max_upload > 0 ? l->max_upload : l->max_request;
    if (length > limit) return -EV_TOO_LARGE;
    if (multi && l->max_upload > 0 && length > l->spool_over) {
        if (!begin_spool(l, c, head, length)) return -EV_SPOOL_FAILED;
        return 0;
    }
    int64_t total = (int64_t)head + 4 + length;
    return (int64_t)c->len >= total ? total : 0;
}

uint64_t gnet_loop_new(int32_t listen_fd, int64_t idle_ms, int64_t max_request, int64_t accept_max) {
    gnet_loop *l = calloc(1, sizeof *l);
    if (!l) return 0;
    l->listen_fd = listen_fd;
    l->idle_ms = idle_ms;
    l->max_request = max_request;
    l->accept_max = accept_max;
    l->max_upload = 0;
    l->wake_fd = gnet_wake_fd();
    set_blocking(listen_fd, 0);
    return (uint64_t)(uintptr_t)l;
}

void gnet_loop_free(uint64_t h) {
    gnet_loop *l = (gnet_loop *)(uintptr_t)h;
    if (!l) return;
    while (l->count) drop(l, 0);
    free(l->conns);
    free(l->pfds);
    free(l);
}

/* Turns on spooling: multipart bodies up to max_upload, those over spool_over to files in dir.
   0 when dir is not a writable directory. */
int32_t gnet_loop_set_uploads(uint64_t h, int64_t max_upload, int64_t spool_over, gnet_bytes dir) {
    gnet_loop *l = (gnet_loop *)(uintptr_t)h;
    struct stat st;
    if (max_upload <= 0 || dir.len <= 0 || dir.len >= (int64_t)sizeof l->dir) return 0;
    char d[sizeof l->dir];
    memcpy(d, dir.data, (size_t)dir.len);
    d[dir.len] = 0;
    if (stat(d, &st) < 0 || !S_ISDIR(st.st_mode) || access(d, W_OK | X_OK) < 0) return 0;
    memcpy(l->dir, d, (size_t)dir.len + 1);
    l->max_upload = max_upload;
    l->spool_over = spool_over;
    return 1;
}

/* Appends the path of the file of the last EV_SPOOLED. */
void gnet_loop_spool_path(uint64_t h, gs_rref out) {
    gnet_loop *l = (gnet_loop *)(uintptr_t)h;
    gs_bld_append(out, l->last_path, strlen(l->last_path));
}

void gnet_loop_close(uint64_t h, int32_t fd) {
    gnet_loop *l = (gnet_loop *)(uintptr_t)h;
    for (size_t i = 0; i < l->count; i++)
        if (l->conns[i].fd == fd) { drop(l, i); return; }
}

/* Unlike close, leaves the fd open for the caller; whatever the client sent after the
   request already handed out goes to rest. */
void gnet_loop_detach(uint64_t h, int32_t fd, gs_rref rest) {
    gnet_loop *l = (gnet_loop *)(uintptr_t)h;
    for (size_t i = 0; i < l->count; i++)
        if (l->conns[i].fd == fd) {
            if (l->conns[i].len) gs_bld_append(rest, l->conns[i].buf, l->conns[i].len);
            remove_at(l, i);
            return;
        }
}

/* Returns 1 and a request, or an EV_ code for a request to reject (the caller answers and closes). */
static int take_ready(gnet_loop *l, gs_rref out, int32_t *fd_out) {
    for (size_t k = 0; k < l->count; k++) {
        size_t i = (l->next + k) % l->count;
        gnet_conn *c = &l->conns[i];
        int64_t n = c->spfd < 0 ? frame(l, c) : 0;
        if (c->spfd >= 0 && (c->failed || c->remain == 0)) {
            l->next = (i + 1) % l->count;
            *fd_out = c->fd;
            if (c->failed) { spool_discard(c); c->failed = 0; return EV_SPOOL_FAILED; }
            /* body complete: hand out the head, keep what follows it, give the file away */
            gs_bld_append(out, c->buf, c->hlen);
            memmove(c->buf, c->buf + c->hlen, c->len - c->hlen);
            c->len -= c->hlen;
            snprintf(l->last_path, sizeof l->last_path, "%s", c->spath);
            close(c->spfd);
            free(c->spath);
            c->spfd = -1;
            c->spath = NULL;
            return EV_SPOOLED;
        }
        if (n == 0) continue;
        l->next = (i + 1) % l->count;
        *fd_out = c->fd;
        if (n < 0) return (int)-n;
        gs_bld_append(out, c->buf, (size_t)n);
        memmove(c->buf, c->buf + n, c->len - (size_t)n);
        c->len -= (size_t)n;
        return EV_REQUEST;
    }
    return 0;
}

static void accept_all(gnet_loop *l) {
    for (int64_t taken = 0; l->accept_max <= 0 || taken < l->accept_max; taken++) {
        int fd = accept(l->listen_fd, NULL, NULL);
        if (fd < 0) return;
        set_blocking(fd, 1);  /* BSD sockets inherit O_NONBLOCK from the listener */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        struct timeval tv = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (l->count == l->cap) {
            size_t cap = l->cap ? l->cap * 2 : 64;
            gnet_conn *grown = realloc(l->conns, cap * sizeof *grown);
            if (!grown) { close(fd); return; }
            l->conns = grown;
            l->cap = cap;
        }
        l->conns[l->count++] = (gnet_conn){ .fd = fd, .last_ms = now_ms(), .spfd = -1 };
    }
}

/* Reads what has arrived on c. 0 when the connection ended or failed. */
static int fill(gnet_conn *c) {
    if (c->cap - c->len < 16384) {
        size_t cap = c->cap ? c->cap * 2 : 16384;
        while (cap - c->len < 16384) cap *= 2;
        uint8_t *grown = realloc(c->buf, cap);
        if (!grown) return 0;
        c->buf = grown;
        c->cap = cap;
    }
    if (c->failed) return 1;
    ssize_t n = recv(c->fd, c->buf + c->len, c->cap - c->len, MSG_DONTWAIT);
    if (n > 0 && c->spfd >= 0 && c->remain > 0) {
        /* body bytes go to the file; any bytes after the body stay in the buffer */
        size_t take = (uint64_t)n < (uint64_t)c->remain ? (size_t)n : (size_t)c->remain;
        if (!write_all(c->spfd, c->buf + c->len, take)) c->failed = 1;
        memmove(c->buf + c->len, c->buf + c->len + take, (size_t)n - take);
        c->len += (size_t)n - take;
        c->remain -= (int64_t)take;
        c->last_ms = now_ms();
        return 1;
    }
    if (n > 0) { c->len += (size_t)n; c->last_ms = now_ms(); return 1; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 1;
    return 0;
}

/* Phase 2: stop accepting, take what has already arrived, and close the connections with nothing buffered. */
static void start_drain(gnet_loop *l) {
    l->draining = 1;
    l->deadline_ms = gnet_drain_deadline_ms();
    for (size_t i = l->count; i-- > 0;)
        if (!fill(&l->conns[i])) drop(l, i);
}

static void drop_idle(gnet_loop *l) {
    for (size_t i = l->count; i-- > 0;)
        if (l->conns[i].len == 0) drop(l, i);
}

int32_t gnet_loop_next(uint64_t h, gs_rref out, int32_t *fd_out) {
    gnet_loop *l = (gnet_loop *)(uintptr_t)h;
    *fd_out = -1;
    for (;;) {
        if (l->draining) {
            drop_idle(l);
            if (l->count == 0 || now_ms() >= l->deadline_ms) {
                while (l->count) drop(l, 0);
                return EV_STOPPED;
            }
        }
        /* After 16 requests from buffers, poll once without waiting, so new
           connections are accepted and filled even while every client
           already connected keeps this loop busy. */
        if (l->since_poll < 16) {
            int ev = take_ready(l, out, fd_out);
            if (ev) { l->since_poll++; return ev; }
        }
        int timeout = l->since_poll >= 16 ? 0 : 1000;
        if (l->draining && timeout) {
            int64_t left = l->deadline_ms - now_ms();
            if (left < timeout) timeout = left > 0 ? (int)left : 0;
        }
        l->since_poll = 0;

        if (l->pfd_cap < l->count + 2) {
            size_t cap = (l->count + 2) * 2;
            struct pollfd *grown = realloc(l->pfds, cap * sizeof *grown);
            if (!grown) return EV_FAILED;
            l->pfds = grown;
            l->pfd_cap = cap;
        }
        /* A negative fd is skipped by poll, which keeps the indexes while draining. */
        l->pfds[0] = (struct pollfd){ l->draining ? -1 : l->listen_fd, POLLIN, 0 };
        for (size_t i = 0; i < l->count; i++) l->pfds[i + 1] = (struct pollfd){ l->conns[i].fd, POLLIN, 0 };
        size_t polled = l->count;
        l->pfds[polled + 1] = (struct pollfd){ l->draining ? -1 : l->wake_fd, POLLIN, 0 };
        int r = poll(l->pfds, (nfds_t)(polled + 2), timeout);
        if (r < 0 && errno != EINTR) return EV_FAILED;

        /* Read before accepting, so the indexes still match pfds. Walk
           down, since drop() moves the last connection into the gap. */
        for (size_t i = polled; i-- > 0;) {
            if (l->pfds[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (!fill(&l->conns[i])) drop(l, i);
            }
        }
        if (l->pfds[polled + 1].revents & POLLIN) start_drain(l);
        if (!l->draining && (l->pfds[0].revents & POLLIN)) accept_all(l);

        if (l->idle_ms > 0) {
            int64_t now = now_ms();
            for (size_t i = l->count; i-- > 0;)
                if (now - l->conns[i].last_ms > l->idle_ms) drop(l, i);
        }
    }
}

int32_t gnet_loop_count(uint64_t h) {
    gnet_loop *l = (gnet_loop *)(uintptr_t)h;
    return (int32_t)l->count;
}
