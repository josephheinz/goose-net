/* The native part of the Goose `net` package: thin wrappers over POSIX sockets.
   A function that fails records a message for gnet_error. */
#define _GNU_SOURCE

#include "goose.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

typedef struct { const uint8_t *data; int64_t len; } gnet_bytes;

static _Thread_local char gnet_last_error[256];

static void set_error(const char *what, const char *why) {
    snprintf(gnet_last_error, sizeof gnet_last_error, "%s: %s", what, why);
}

static void set_errno(const char *what) { set_error(what, strerror(errno)); }

/* Copies a slice into a NUL-terminated buffer; 0 if it does not fit. */
static int to_cstr(gnet_bytes s, char *buf, size_t size) {
    if (s.len < 0 || (size_t)s.len >= size) return 0;
    memcpy(buf, s.data, (size_t)s.len);
    buf[s.len] = 0;
    return 1;
}

static void ignore_sigpipe(void) {
    static int done = 0;
    if (!done) { signal(SIGPIPE, SIG_IGN); done = 1; }
}

void gnet_error(gs_rref out) {
    gs_bld_append(out, gnet_last_error, strlen(gnet_last_error));
}

static int open_socket(gnet_bytes host_s, int32_t port, int passive, int backlog, int64_t timeout_ms) {
    ignore_sigpipe();
    char host[256], service[16];
    if (!to_cstr(host_s, host, sizeof host)) { set_error("resolve", "host name too long"); return -1; }
    snprintf(service, sizeof service, "%d", (int)port);

    struct addrinfo hints, *list = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (passive) hints.ai_flags = AI_PASSIVE;
    int rc = getaddrinfo(host[0] ? host : NULL, service, &hints, &list);
    if (rc != 0) { set_error(host, gai_strerror(rc)); return -1; }

    int fd = -1;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) { set_errno("socket"); continue; }
        if (passive) {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, backlog) == 0) break;
            set_errno("bind");
        } else {
            /* Non-blocking connect, so a dead host gives up after timeout_ms. */
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            int c = connect(fd, a->ai_addr, a->ai_addrlen);
            if (c < 0 && errno == EINPROGRESS) {
                struct pollfd p = { fd, POLLOUT, 0 };
                int r = poll(&p, 1, timeout_ms < 0 ? -1 : (int)timeout_ms);
                if (r == 1) {
                    int err = 0; socklen_t len = sizeof err;
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err == 0) c = 0; else errno = err;
                } else if (r == 0) {
                    errno = ETIMEDOUT;
                }
            }
            if (c == 0) {
                fcntl(fd, F_SETFL, flags);
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                break;
            }
            set_errno("connect");
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}

int32_t gnet_listen(gnet_bytes host, int32_t port, int32_t backlog) {
    return open_socket(host, port, 1, backlog, 0);
}

int32_t gnet_connect(gnet_bytes host, int32_t port, int64_t timeout_ms) {
    return open_socket(host, port, 0, 0, timeout_ms);
}

int32_t gnet_accept(int32_t listen_fd) {
    int fd;
    do { fd = accept(listen_fd, NULL, NULL); } while (fd < 0 && errno == EINTR);
    if (fd < 0) set_errno("accept");
    return fd;
}

int64_t gnet_recv(int32_t fd, gs_rref out, int64_t max) {
    char buf[65536];
    if (max <= 0) return 0;
    if (max > (int64_t)sizeof buf) max = sizeof buf;
    ssize_t n;
    do { n = recv(fd, buf, (size_t)max, 0); } while (n < 0 && errno == EINTR);
    if (n < 0) { set_errno("recv"); return -1; }
    if (n > 0) gs_bld_append(out, buf, (size_t)n);
    return (int64_t)n;
}

int32_t gnet_send_all(int32_t fd, gnet_bytes data) {
    const uint8_t *p = data.data;
    int64_t len = data.len;
    while (len > 0) {
        ssize_t n = send(fd, p, (size_t)len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            set_errno("send");
            return 0;
        }
        p += n;
        len -= n;
    }
    return 1;
}

int32_t gnet_send2(int32_t fd, gnet_bytes a, gnet_bytes b) {
    struct iovec iov[2] = { { (void *)a.data, (size_t)a.len }, { (void *)b.data, (size_t)b.len } };
    int n = 2, i = 0;
    while (i < n) {
        ssize_t w = writev(fd, iov + i, n - i);
        if (w < 0) {
            if (errno == EINTR) continue;
            set_errno("writev");
            return 0;
        }
        while (i < n && (size_t)w >= iov[i].iov_len) { w -= (ssize_t)iov[i].iov_len; i++; }
        if (i < n) { iov[i].iov_base = (char *)iov[i].iov_base + w; iov[i].iov_len -= (size_t)w; }
    }
    return 1;
}

void gnet_close(int32_t fd) { if (fd >= 0) close(fd); }

void gnet_shutdown_write(int32_t fd) { shutdown(fd, SHUT_WR); }

int32_t gnet_set_timeout(int32_t fd, int64_t ms) {
    struct timeval tv;
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0) {
        set_errno("setsockopt");
        return 0;
    }
    return 1;
}

static int32_t addr_port(struct sockaddr_storage *ss) {
    if (ss->ss_family == AF_INET) return ntohs(((struct sockaddr_in *)ss)->sin_port);
    if (ss->ss_family == AF_INET6) return ntohs(((struct sockaddr_in6 *)ss)->sin6_port);
    return -1;
}

int32_t gnet_local_port(int32_t fd) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &len) < 0) { set_errno("getsockname"); return -1; }
    return addr_port(&ss);
}

/* Appends the peer as "ip:port" (IPv6 as "[ip]:port"). 0 on failure. */
int32_t gnet_peer(int32_t fd, gs_rref out) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &len) < 0) { set_errno("getpeername"); return 0; }
    char ip[INET6_ADDRSTRLEN], text[INET6_ADDRSTRLEN + 16];
    const void *a = ss.ss_family == AF_INET6
        ? (const void *)&((struct sockaddr_in6 *)&ss)->sin6_addr
        : (const void *)&((struct sockaddr_in *)&ss)->sin_addr;
    if (!inet_ntop(ss.ss_family, a, ip, sizeof ip)) { set_errno("inet_ntop"); return 0; }
    int n = snprintf(text, sizeof text, ss.ss_family == AF_INET6 ? "[%s]:%d" : "%s:%d", ip, (int)addr_port(&ss));
    gs_bld_append(out, text, (size_t)n);
    return 1;
}
