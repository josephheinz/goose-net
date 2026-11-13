/* Shared by the C tests. Each case runs in its own process, because the stop state is process-wide
   and one-way. A case must not touch goose-net before it is forked. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

static int checks, failures;

#define CHECK(cond, what) do { checks++; if (!(cond)) { failures++; printf("FAIL %s (line %d)\n", (what), __LINE__); } } while (0)

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Polls cond until it holds or ms pass. */
#define WAIT_FOR(cond, ms) ({ int64_t until_ = mono_ms() + (ms); int ok_ = 0; \
    while (!(ok_ = !!(cond)) && mono_ms() < until_) { nap_ms(10); } ok_; })

#define RESULT_FD 100

static void run_case(const char *name, void (*fn)(void)) {
    int fds[2];
    fflush(stdout);
    if (pipe(fds) < 0) { perror("pipe"); exit(2); }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], RESULT_FD);
        close(fds[1]);
        alarm(60);
        checks = failures = 0;
        fn();
        fflush(stdout);
        int counts[2] = { checks, failures };
        if (write(RESULT_FD, counts, sizeof counts) < 0) _exit(2);
        _exit(0);
    }
    close(fds[1]);
    int counts[2] = { 0, 0 };
    ssize_t n = read(fds[0], counts, sizeof counts);
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    if (n != (ssize_t)sizeof counts || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL case %s did not finish (status %d)\n", name, status);
        checks++; failures++;
        return;
    }
    checks += counts[0];
    failures += counts[1];
}

static int finish(void) {
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("ok %d checks\n", checks);
    return 0;
}

#endif
