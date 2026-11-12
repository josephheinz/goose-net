#define _GNU_SOURCE
#include "goose.h"
#include "check.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>

typedef struct { const uint8_t *data; int64_t len; } bytes;

void gnet_stop_on_signals(int64_t delay_ms, int64_t drain_ms);
void gnet_request_stop(void);
int64_t gnet_stop_phase(void);
int32_t gnet_wait_stop(int64_t ms);
int32_t gnet_wake_fd(void);
int64_t gnet_drain_deadline_ms(void);
int32_t gnet_listen(bytes host, int32_t port, int32_t backlog);
int32_t gnet_listen_inherited(bytes host, int32_t port, int32_t backlog, int32_t *inherited);
int32_t gnet_enable_handoff(int32_t fd, bytes argv_blob);
void gnet_ready(void);

static const char *self_path;
static const bytes LOCAL = { (const uint8_t *)"127.0.0.1", 9 };

static int readable(int fd, int ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, ms) == 1 && (p.revents & POLLIN);
}

static int is_listening(int fd) {
    int v = 0;
    socklen_t len = sizeof v;
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &v, &len) == 0) return v != 0;
    struct sockaddr_storage ss;
    len = sizeof ss;
    return errno == ENOPROTOOPT && getpeername(fd, (struct sockaddr *)&ss, &len) != 0 && errno == ENOTCONN;
}

/* ---------------------------------------------------------------- the handoff child */

static void child_note(char *buf, size_t size, const char *what) {
    size_t n = strlen(buf);
    snprintf(buf + n, size - n, "FAIL %s\n", what);
}

static int handoff_child(const char *mode, const char *outfile) {
    if (strcmp(mode, "exit3") == 0) _exit(3);
    char report[1024] = "";
    const char *fds = getenv("LISTEN_FDS"), *lpid = getenv("LISTEN_PID"), *hpid = getenv("GOOSE_NET_HANDOFF_PID");
    if (!fds || strcmp(fds, "1") != 0) child_note(report, sizeof report, "LISTEN_FDS is 1");
    if (!lpid || atoi(lpid) != (int)getpid()) child_note(report, sizeof report, "LISTEN_PID is the child pid");
    if (!hpid || atoi(hpid) != (int)getppid()) child_note(report, sizeof report, "GOOSE_NET_HANDOFF_PID is the parent pid");
    if (!is_listening(3)) child_note(report, sizeof report, "fd 3 listens");
    for (int fd = 4; fd < 256; fd++)
        if (fcntl(fd, F_GETFD) != -1) { child_note(report, sizeof report, "no fd above 3 is inherited"); break; }
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    if (sigismember(&cur, SIGTERM)) child_note(report, sizeof report, "the signal mask is restored");
    int inherited = 0;
    int fd = gnet_listen_inherited(LOCAL, 0, 16, &inherited);
    if (fd != 3 || !inherited) child_note(report, sizeof report, "listen_inherited returns fd 3");
    if (getenv("LISTEN_FDS") || getenv("LISTEN_PID") || getenv("GOOSE_NET_HANDOFF_PID")) child_note(report, sizeof report, "the variables are unset");
    if (!report[0]) strcpy(report, "ok\n");
    int out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out >= 0) { if (write(out, report, strlen(report)) < 0) {} close(out); }
    gnet_ready();
    nap_ms(1500);
    return 0;
}

/* ---------------------------------------------------------------- helpers for the cases */

static int notify_socket(char *path, size_t size) {
    snprintf(path, size, "/tmp/gn%d.sock", (int)getpid());
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("bind"); exit(2); }
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static int recv_text(int fd, char *buf, size_t size) {
    ssize_t n = recv(fd, buf, size - 1, 0);
    buf[n > 0 ? n : 0] = 0;
    return n > 0;
}

static int listener(void) {
    int fd = gnet_listen(LOCAL, 0, 16);
    if (fd < 0) { perror("listen"); exit(2); }
    return fd;
}

static size_t make_blob(char *buf, size_t size, const char *mode, const char *outfile) {
    return (size_t)snprintf(buf, size, "%s%c--handoff-child%c%s%c%s%c", self_path, 0, 0, mode, 0, outfile, 0);
}

static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return n > 0;
}

/* ---------------------------------------------------------------- cases */

static void case_phases(void) {
    gnet_stop_on_signals(200, 300);
    CHECK(gnet_stop_phase() == 0, "starts running");
    CHECK(!gnet_wait_stop(30), "wait_stop times out while running");
    CHECK(gnet_drain_deadline_ms() == 0, "no drain deadline while running");
    int wake = gnet_wake_fd();
    CHECK(wake >= 0, "wake fd");
    CHECK(!readable(wake, 0), "wake fd is quiet while running");
    int64_t t0 = mono_ms();
    gnet_request_stop();
    CHECK(gnet_stop_phase() == 1, "phase 1 right after the stop");
    CHECK(gnet_wait_stop(0), "wait_stop true once stopping");
    CHECK(!readable(wake, 0), "wake fd is quiet in phase 1");
    CHECK(WAIT_FOR(gnet_stop_phase() == 2, 3000), "phase 2 follows");
    int64_t waited = mono_ms() - t0;
    CHECK(waited >= 190 && waited < 1500, "phase 2 came after delay_ms");
    CHECK(readable(wake, 1000), "wake fd readable in phase 2");
    int64_t left = gnet_drain_deadline_ms() - mono_ms();
    CHECK(left > 0 && left <= 300, "drain deadline is drain_ms from phase 2");
    gnet_request_stop();
    CHECK(gnet_stop_phase() == 2, "a second request_stop changes nothing");
}

static void *stop_later(void *arg) {
    (void)arg;
    nap_ms(150);
    gnet_request_stop();
    return NULL;
}

static void case_wait_stop(void) {
    pthread_t t;
    pthread_create(&t, NULL, stop_later, NULL);
    int64_t t0 = mono_ms();
    CHECK(gnet_wait_stop(5000), "wait_stop returns true");
    int64_t waited = mono_ms() - t0;
    CHECK(waited >= 100 && waited < 2000, "wait_stop returned early on the stop");
    pthread_join(t, NULL);
}

static void case_sigterm(void) {
    gnet_stop_on_signals(0, 10000);
    kill(getpid(), SIGTERM);
    CHECK(gnet_wait_stop(3000), "SIGTERM starts the stop");
    CHECK(gnet_stop_phase() >= 1, "phase moved");
}

static void case_sigint(void) {
    gnet_stop_on_signals(0, 10000);
    kill(getpid(), SIGINT);
    CHECK(gnet_wait_stop(3000), "SIGINT starts the stop");
}

static void case_second_signal(void) {
    pid_t pid = fork();
    if (pid == 0) {
        gnet_stop_on_signals(0, 10000);
        kill(getpid(), SIGTERM);
        gnet_wait_stop(3000);
        kill(getpid(), SIGINT);
        nap_ms(5000);
        _exit(99);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 130, "a second signal exits with 130");
}

static void case_hard_deadline(void) {
    int err[2];
    if (pipe(err) < 0) exit(2);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(err[1], 2);
        close(err[0]);
        close(err[1]);
        gnet_stop_on_signals(0, 0);
        gnet_request_stop();
        nap_ms(9000);
        _exit(99);
    }
    close(err[1]);
    int64_t t0 = mono_ms();
    int status;
    waitpid(pid, &status, 0);
    int64_t took = mono_ms() - t0;
    char text[256];
    ssize_t n = read(err[0], text, sizeof text - 1);
    text[n > 0 ? n : 0] = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1, "the hard deadline exits with 1");
    CHECK(took >= 4500 && took < 8000, "the hard deadline is delay + drain + 5000 ms");
    CHECK(strstr(text, "goose-net:") != NULL, "the hard deadline says why on stderr");
}

static void case_usr2_ignored(void) {
    gnet_stop_on_signals(0, 10000);
    kill(getpid(), SIGUSR2);
    nap_ms(200);
    CHECK(gnet_stop_phase() == 0, "SIGUSR2 without a handoff is ignored");
}

static void case_inherited(void) {
    int lis = listener();
    if (lis != 3) { dup2(lis, 3); close(lis); }
    char pid[16];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    setenv("LISTEN_FDS", "1", 1);
    setenv("LISTEN_PID", pid, 1);
    int inh = -1;
    int fd = gnet_listen_inherited(LOCAL, 0, 16, &inh);
    CHECK(fd == 3 && inh == 1, "fd 3 is used when LISTEN_PID matches");
    CHECK(getenv("LISTEN_FDS") == NULL && getenv("LISTEN_PID") == NULL, "LISTEN_FDS and LISTEN_PID are unset");
    int second = gnet_listen_inherited(LOCAL, 0, 16, &inh);
    CHECK(second != 3 && second >= 0 && inh == 0, "a second call listens fresh");
}

static void case_wrong_pid(void) {
    int lis = listener();
    if (lis != 3) { dup2(lis, 3); close(lis); }
    char pid[16];
    snprintf(pid, sizeof pid, "%d", (int)getpid() + 1);
    setenv("LISTEN_FDS", "1", 1);
    setenv("LISTEN_PID", pid, 1);
    int inh = -1;
    int fd = gnet_listen_inherited(LOCAL, 0, 16, &inh);
    CHECK(fd >= 0 && fd != 3 && inh == 0, "a wrong LISTEN_PID listens fresh");
    CHECK(is_listening(fd), "the fresh socket listens");
    CHECK(getenv("LISTEN_FDS") == NULL && getenv("LISTEN_PID") == NULL, "the variables are unset anyway");
}

static void case_not_listening(void) {
    int dev = open("/dev/null", O_RDONLY);
    dup2(dev, 3);
    if (dev != 3) close(dev);
    char pid[16];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    setenv("LISTEN_FDS", "1", 1);
    setenv("LISTEN_PID", pid, 1);
    int inh = -1;
    int fd = gnet_listen_inherited(LOCAL, 0, 16, &inh);
    CHECK(fd >= 0 && fd != 3 && inh == 0, "a non-listening fd 3 is refused");
}

static void case_ready(void) {
    char path[64], text[64];
    int ns = notify_socket(path, sizeof path);
    setenv("NOTIFY_SOCKET", path, 1);
    gnet_ready();
    CHECK(recv_text(ns, text, sizeof text) && strcmp(text, "READY=1") == 0, "ready sends READY=1");
    gnet_request_stop();
    CHECK(recv_text(ns, text, sizeof text) && strcmp(text, "STOPPING=1") == 0, "a stop sends STOPPING=1");
    unlink(path);
}

static void case_handoff(void) {
    int leak = open("/dev/null", O_RDONLY);
    dup2(leak, 20);
    close(leak);
    int pad = open("/dev/null", O_RDONLY);
    int lis = listener();
    CHECK(lis != 3 && pad >= 3, "the listener is not on fd 3 here");

    char path[64], text[256], out[256], outfile[64], blob[512];
    int ns = notify_socket(path, sizeof path);
    setenv("NOTIFY_SOCKET", path, 1);
    snprintf(outfile, sizeof outfile, "/tmp/gn%d.out", (int)getpid());
    unlink(outfile);

    gnet_stop_on_signals(0, 2000);
    size_t n = make_blob(blob, sizeof blob, "check", outfile);
    CHECK(gnet_enable_handoff(lis, (bytes){ (const uint8_t *)blob, (int64_t)n }) == 1, "enable_handoff");
    kill(getpid(), SIGUSR2);
    CHECK(gnet_wait_stop(8000), "ready() in the child stops the parent");
    CHECK(read_file(outfile, out, sizeof out) && strcmp(out, "ok\n") == 0, "the child saw fd 3, the variables and no other fd");
    if (strcmp(out, "ok\n") != 0) printf("%s", out);
    CHECK(recv_text(ns, text, sizeof text) && strncmp(text, "MAINPID=", 8) == 0 && strstr(text, "\nREADY=1") != NULL
          && atoi(text + 8) != (int)getpid() && atoi(text + 8) > 0, "the child sends MAINPID and READY");
    CHECK(recv_text(ns, text, sizeof text) && strcmp(text, "STOPPING=1") == 0, "the parent then sends STOPPING=1");
    unlink(path);
    unlink(outfile);
}

static void case_handoff_failed(void) {
    char errfile[64], outfile[64], blob[512], text[512], out[256];
    snprintf(errfile, sizeof errfile, "/tmp/gn%d.err", (int)getpid());
    snprintf(outfile, sizeof outfile, "/tmp/gn%d.out", (int)getpid());
    unlink(outfile);
    int ef = open(errfile, O_RDWR | O_CREAT | O_TRUNC, 0600);
    dup2(ef, 2);
    close(ef);
    int lis = listener();

    gnet_stop_on_signals(0, 2000);
    size_t n = make_blob(blob, sizeof blob, "exit3", outfile);
    gnet_enable_handoff(lis, (bytes){ (const uint8_t *)blob, (int64_t)n });
    kill(getpid(), SIGUSR2);
    int seen = WAIT_FOR(read_file(errfile, text, sizeof text) && strstr(text, "handoff failed") != NULL, 5000);
    CHECK(seen, "a child that exits early is reported");
    CHECK(strstr(text, "goose-net: handoff failed: the new process exited with status 3") != NULL, "the message names the status");
    CHECK(gnet_stop_phase() == 0, "the parent keeps running");

    n = make_blob(blob, sizeof blob, "check", outfile);
    gnet_enable_handoff(lis, (bytes){ (const uint8_t *)blob, (int64_t)n });
    kill(getpid(), SIGUSR2);
    CHECK(gnet_wait_stop(8000), "a later SIGUSR2 hands off");
    CHECK(read_file(outfile, out, sizeof out) && strcmp(out, "ok\n") == 0, "the second child took over");
    unlink(errfile);
    unlink(outfile);
}

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "--handoff-child") == 0) return handoff_child(argv[2], argv[3]);
    self_path = argv[0];
    run_case("phases", case_phases);
    run_case("wait_stop", case_wait_stop);
    run_case("sigterm", case_sigterm);
    run_case("sigint", case_sigint);
    run_case("second signal", case_second_signal);
    run_case("usr2 ignored", case_usr2_ignored);
    run_case("inherited", case_inherited);
    run_case("wrong pid", case_wrong_pid);
    run_case("not listening", case_not_listening);
    run_case("ready", case_ready);
    run_case("handoff", case_handoff);
    run_case("handoff failed", case_handoff_failed);
    run_case("hard deadline", case_hard_deadline);
    return finish();
}
