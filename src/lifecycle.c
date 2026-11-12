/* The native part of net::lifecycle: stop signals and the drain phases, systemd notify,
   listener inheritance and the SIGUSR2 handoff to a new process. */
#define _GNU_SOURCE
#include "goose.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#endif

typedef struct { const uint8_t *data; int64_t len; } gnet_bytes;

int32_t gnet_listen(gnet_bytes host, int32_t port, int32_t backlog);

extern char **environ;

#define GRACE_MS 5000

static atomic_int phase;
static _Atomic int64_t deadline;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t pipe_once = PTHREAD_ONCE_INIT;
static int wake_rd = -1, wake_wr = -1;
static int64_t delay_ms = 0, drain_ms = 10000;
static int signals_started;
static sigset_t orig_mask;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static void make_pipe(void) {
    int fds[2];
    if (pipe(fds) < 0) return;
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    wake_rd = fds[0];
    wake_wr = fds[1];
}

int32_t gnet_wake_fd(void) {
    pthread_once(&pipe_once, make_pipe);
    return wake_rd;
}

int64_t gnet_stop_phase(void) { return atomic_load(&phase); }

int64_t gnet_drain_deadline_ms(void) { return atomic_load(&deadline); }

static void set_phase(int p) {
    pthread_mutex_lock(&mu);
    atomic_store(&phase, p);
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

/* One datagram to $NOTIFY_SOCKET, as sd_notify does. A leading '@' is an abstract socket. */
static void notify(const char *msg) {
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || !path[0]) return;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof sa.sun_path) return;
    memcpy(sa.sun_path, path, n);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
    if (path[0] == '@') { sa.sun_path[0] = 0; len--; }
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&sa, len);
    close(fd);
}

/* Runs the stop: phase 1 for delay_ms, then phase 2 and the wake byte, then the hard deadline. */
static void *stop_main(void *arg) {
    (void)arg;
    int64_t d = drain_ms < 0 ? 0 : drain_ms;
    sleep_ms(delay_ms);
    atomic_store(&deadline, now_ms() + d);
    set_phase(2);
    char b = 1;
    if (write(wake_wr, &b, 1) < 0) {}
    sleep_ms(d + GRACE_MS);
    static const char msg[] = "goose-net: drain deadline passed, exiting\n";
    if (write(2, msg, sizeof msg - 1) < 0) {}
    _exit(1);
}

/* 1 when this call started the stop. */
static int begin_stop(void) {
    gnet_wake_fd();
    pthread_mutex_lock(&mu);
    if (atomic_load(&phase) != 0) { pthread_mutex_unlock(&mu); return 0; }
    atomic_store(&phase, 1);
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    notify("STOPPING=1");
    pthread_t t;
    if (pthread_create(&t, NULL, stop_main, NULL) == 0) pthread_detach(t);
    else stop_main(NULL);
    return 1;
}

void gnet_request_stop(void) { begin_stop(); }

int32_t gnet_wait_stop(int64_t ms) {
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    if (ms < 0) ms = 0;
    abs.tv_sec += (time_t)(ms / 1000);
    abs.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&mu);
    while (atomic_load(&phase) == 0 && pthread_cond_timedwait(&cv, &mu, &abs) == 0) {}
    int stopping = atomic_load(&phase) > 0;
    pthread_mutex_unlock(&mu);
    return stopping;
}

/* ---------------------------------------------------------------- environment */

static pthread_mutex_t emu = PTHREAD_MUTEX_INITIALIZER;
static int env_loaded, inherit_used;
static long inherit_fds;
static pid_t inherit_pid, handoff_parent;

static long env_long(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    char *end;
    long n = strtol(v, &end, 10);
    return *end ? 0 : n;
}

/* The inheritance variables are read and removed once, so child processes never see them. */
static void env_load(void) {
    if (env_loaded) return;
    inherit_fds = env_long("LISTEN_FDS");
    inherit_pid = (pid_t)env_long("LISTEN_PID");
    handoff_parent = (pid_t)env_long("GOOSE_NET_HANDOFF_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_PID");
    unsetenv("GOOSE_NET_HANDOFF_PID");
    env_loaded = 1;
}

/* SO_ACCEPTCONN is not readable on macOS; there, an unconnected stream socket has to do. */
static int is_listener(int fd) {
    int v = 0, type = 0;
    socklen_t len = sizeof v;
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &v, &len) == 0) return v != 0;
    if (errno != ENOPROTOOPT) return 0;
    len = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_STREAM) return 0;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    return getpeername(fd, (struct sockaddr *)&ss, &sl) != 0 && errno == ENOTCONN;
}

int32_t gnet_listen_inherited(gnet_bytes host, int32_t port, int32_t backlog, int32_t *inherited) {
    *inherited = 0;
    pthread_mutex_lock(&emu);
    env_load();
    int use = !inherit_used && inherit_fds >= 1 && inherit_pid == getpid();
    if (use) inherit_used = 1;
    pthread_mutex_unlock(&emu);
    if (use) {
        if (is_listener(3)) {
            /* Linux has no SO_NOSIGPIPE; gnet_listen does this for a fresh listener. */
            signal(SIGPIPE, SIG_IGN);
            fcntl(3, F_SETFD, FD_CLOEXEC);
            *inherited = 1;
            return 3;
        }
    }
    return gnet_listen(host, port, backlog);
}

void gnet_ready(void) {
    pthread_mutex_lock(&emu);
    env_load();
    pid_t parent = handoff_parent;
    handoff_parent = 0;
    pthread_mutex_unlock(&emu);
    if (parent > 1) {
        char msg[64];
        snprintf(msg, sizeof msg, "MAINPID=%d\nREADY=1", (int)getpid());
        notify(msg);
        kill(parent, SIGTERM);
    } else {
        notify("READY=1");
    }
}

/* ---------------------------------------------------------------- handoff */

static pthread_mutex_t hmu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int enabled, listener;
    char exe[PATH_MAX];
    char *blob, **argv;
    pid_t child;
} ho;

static int resolve_exe(char *buf) {
#ifdef __APPLE__
    char raw[PATH_MAX];
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0) return 0;
    return realpath(raw, buf) != NULL;
#else
    ssize_t n = readlink("/proc/self/exe", buf, PATH_MAX - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
    return 1;
#endif
}

int32_t gnet_enable_handoff(int32_t fd, gnet_bytes args) {
    char exe[PATH_MAX];
    if (!resolve_exe(exe)) { fprintf(stderr, "goose-net: handoff disabled: cannot resolve the executable path\n"); return 0; }
    size_t len = args.len > 0 ? (size_t)args.len : 0;
    int terminated = len > 0 && args.data[len - 1] == 0;
    char *blob = malloc(len + 2);
    if (!blob) return 0;
    if (len) memcpy(blob, args.data, len);
    if (len > 0 && !terminated) blob[len++] = 0;
    size_t argc = 0;
    for (size_t i = 0; i < len; i++) if (blob[i] == 0) argc++;
    char **argv = calloc(argc + 2, sizeof *argv);
    if (!argv) { free(blob); return 0; }
    size_t k = 0;
    if (argc == 0) k = 1;
    for (size_t i = 0; i < len; k++) { argv[k] = blob + i; i += strlen(blob + i) + 1; }
    argv[k] = NULL;

    pthread_mutex_lock(&hmu);
    free(ho.blob);
    free(ho.argv);
    ho.blob = blob;
    ho.argv = argv;
    if (argc == 0) ho.argv[0] = ho.exe;
    memcpy(ho.exe, exe, sizeof ho.exe);
    ho.listener = fd;
    ho.enabled = 1;
    pthread_mutex_unlock(&hmu);
    return 1;
}

static void close_from_4(int limit) {
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 4U, ~0U, 0) == 0) return;
#endif
    for (int fd = 4; fd < limit; fd++) close(fd);
}

/* The slots the child fills in or reads; allocated before fork, since the child may not malloc. */
typedef struct {
    char listen_pid[48];
    char handoff_pid[48];
    char listen_fds[16];
} env_block;

static void start_handoff(void) {
    pthread_mutex_lock(&hmu);
    if (!ho.enabled || ho.child > 0 || atomic_load(&phase) > 0) { pthread_mutex_unlock(&hmu); return; }

    size_t n = 0;
    while (environ && environ[n]) n++;
    char **envp = malloc((n + 4) * sizeof *envp);
    env_block *blk = malloc(sizeof *blk);
    if (!envp || !blk) {
        free(envp); free(blk);
        pthread_mutex_unlock(&hmu);
        fprintf(stderr, "goose-net: handoff failed: out of memory\n");
        return;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *e = environ[i];
        if (strncmp(e, "LISTEN_FDS=", 11) == 0 || strncmp(e, "LISTEN_PID=", 11) == 0 ||
            strncmp(e, "GOOSE_NET_HANDOFF_PID=", 22) == 0) continue;
        envp[k++] = environ[i];
    }
    strcpy(blk->listen_pid, "LISTEN_PID=");
    snprintf(blk->handoff_pid, sizeof blk->handoff_pid, "GOOSE_NET_HANDOFF_PID=%d", (int)getpid());
    strcpy(blk->listen_fds, "LISTEN_FDS=1");
    envp[k++] = blk->listen_fds;
    envp[k++] = blk->listen_pid;
    envp[k++] = blk->handoff_pid;
    envp[k] = NULL;

    struct rlimit rl;
    int limit = 1024;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) limit = rl.rlim_cur > 65536 ? 65536 : (int)rl.rlim_cur;

    pid_t pid = fork();
    if (pid == 0) {
        if (ho.listener != 3 && dup2(ho.listener, 3) < 0) _exit(127);
        int flags = fcntl(3, F_GETFD);
        if (flags >= 0) fcntl(3, F_SETFD, flags & ~FD_CLOEXEC);
        close_from_4(limit);
        char digits[16];
        int d = 0;
        for (pid_t me = getpid(); me > 0 || d == 0; me /= 10) digits[d++] = (char)('0' + me % 10);
        char *p = blk->listen_pid + 11;
        while (d > 0) *p++ = digits[--d];
        *p = 0;
        pthread_sigmask(SIG_SETMASK, &orig_mask, NULL);
        execve(ho.exe, ho.argv, envp);
        _exit(127);
    }
    free(envp);
    free(blk);
    if (pid < 0) fprintf(stderr, "goose-net: handoff failed: fork: %s\n", strerror(errno));
    else ho.child = pid;
    pthread_mutex_unlock(&hmu);
}

static void reap_child(void) {
    pthread_mutex_lock(&hmu);
    int status;
    if (ho.child > 0 && waitpid(ho.child, &status, WNOHANG) == ho.child) {
        ho.child = 0;
        sigset_t pending;
        sigpending(&pending);
        /* A child that took over signals us before it exits; that SIGTERM may still be queued. */
        int taking_over = sigismember(&pending, SIGTERM) || sigismember(&pending, SIGINT);
        if (atomic_load(&phase) == 0 && !taking_over) {
            if (WIFEXITED(status)) fprintf(stderr, "goose-net: handoff failed: the new process exited with status %d\n", WEXITSTATUS(status));
            else fprintf(stderr, "goose-net: handoff failed: the new process was killed by signal %d\n", WTERMSIG(status));
        }
    }
    pthread_mutex_unlock(&hmu);
}

/* ---------------------------------------------------------------- signals */

static void ignore_chld(int sig) { (void)sig; }

static void *signal_main(void *arg) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR2);
    sigaddset(&set, SIGCHLD);
    (void)arg;
    for (;;) {
        int sig;
        if (sigwait(&set, &sig) != 0) continue;
        if (sig == SIGTERM || sig == SIGINT) { if (!begin_stop()) _exit(130); }
        else if (sig == SIGUSR2) start_handoff();
        else if (sig == SIGCHLD) reap_child();
    }
    return NULL;
}

void gnet_stop_on_signals(int64_t delay, int64_t drain) {
    pthread_mutex_lock(&mu);
    if (signals_started) { pthread_mutex_unlock(&mu); return; }
    signals_started = 1;
    delay_ms = delay < 0 ? 0 : delay;
    drain_ms = drain < 0 ? 0 : drain;
    pthread_mutex_unlock(&mu);

    gnet_wake_fd();
    /* A default-ignored signal is dropped at once on macOS, even when blocked; a handler keeps it pending for sigwait. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ignore_chld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR2);
    sigaddset(&set, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &set, &orig_mask);
    pthread_t t;
    if (pthread_create(&t, NULL, signal_main, NULL) != 0) {
        pthread_sigmask(SIG_SETMASK, &orig_mask, NULL);
        fprintf(stderr, "goose-net: cannot start the signal thread\n");
        return;
    }
    pthread_detach(t);
}
