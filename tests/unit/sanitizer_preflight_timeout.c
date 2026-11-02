/* SPDX-License-Identifier: GPL-3.0-only */
/* Bound one owned probe process group; no dependency on a target's timeout utility. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void interrupt_probe(int signal_number) {
    interrupted = signal_number;
}

int main(int argc, char **argv) {
    char *end = NULL;
    unsigned long seconds = argc > 2 ? strtoul(argv[1], &end, 10) : 0;
    if (!seconds || seconds > 300 || !end || *end) return 125;
    struct sigaction action = {0};
    action.sa_handler = interrupt_probe;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return 125;
    pid_t child = fork();
    if (child < 0) return 125;
    if (!child) {
        if (setpgid(0, 0)) _exit(125);
        execvp(argv[2], argv + 2);
        _exit(errno == ENOENT ? 127 : 126);
    }
    if (setpgid(child, child) && errno != EACCES && errno != ESRCH) {
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        return 125;
    }
    struct timespec began, now;
    if (clock_gettime(CLOCK_MONOTONIC, &began)) {
        kill(-child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        return 125;
    }
    int status = 0;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno != EINTR) return 125;
        int clock_failed = clock_gettime(CLOCK_MONOTONIC, &now);
        int timed_out = !clock_failed &&
                        (now.tv_sec - began.tv_sec > (time_t) seconds ||
                         (now.tv_sec - began.tv_sec == (time_t) seconds && now.tv_nsec >= began.tv_nsec));
        if (interrupted || clock_failed || timed_out) {
            kill(-child, SIGKILL);
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
            if (interrupted) return 128 + interrupted;
            if (clock_failed) return 125;
            fputs("preflight probe timed out\n", stderr);
            return 124;
        }
        struct timespec delay = {0, 50000000};
        nanosleep(&delay, NULL);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
}
