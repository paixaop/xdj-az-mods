/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the production overlay wait with deterministic descriptor readiness. */
#include "../../src/common/aztimestamp.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

struct overlay_ctx {
    void *dpy;
};
struct readiness {
    long elapsed;
    short x, command, status, timer;
};
static struct readiness steps[32];
static unsigned next_step, step_count, polls, commands, statuses;
static long elapsed, callback_duration;
static int pending_x;
static int ds_event_fd = 3, ds_timer_fd = 4;
enum {
    QueuedAlready = 0
};
static long now_ms(void) { return elapsed; }
static int ConnectionNumber(void *unused) {
    (void) unused;
    return 1;
}
static int sdk_command_ui_fd(void) { return 2; }
static int XEventsQueued(void *unused, int mode) {
    (void) unused;
    (void) mode;
    return pending_x;
}
static unsigned sdk_command_ui_drain(unsigned budget) {
    assert(budget == 8);
    ++commands;
    return 1;
}
static void sdk_status_ui_drain(void) {
    ++statuses;
    elapsed += callback_duration;
}
static int simulated_poll(struct pollfd *fds, nfds_t count, int timeout) {
    assert(count == 4 && ++polls < 32 && timeout >= 0);
    for (unsigned i = 0; i < 4; ++i) fds[i].revents = 0;
    if (next_step >= step_count || compare_timestamps(steps[next_step].elapsed, elapsed + timeout, 1000, 1000, 1) == AZ_TIME_AFTER) {
        elapsed += timeout;
        return 0;
    }
    struct readiness event = steps[next_step++];
    elapsed = event.elapsed;
    fds[0].revents = event.x;
    fds[1].revents = event.command;
    fds[2].revents = event.status;
    fds[3].revents = event.timer;
    return 1;
}
#define poll simulated_poll
#include "status-wait.inc"
#undef poll

static void reset(void) {
    next_step = step_count = polls = commands = statuses = 0;
    elapsed = callback_duration = pending_x = 0;
}
int main(void) {
    struct overlay_ctx context = {0};
    reset();
    for (unsigned i = 0; i < 6; ++i) steps[step_count++] = (struct readiness) {i + 1, 0, 0, POLLIN, 0};
    ov_wait(&context, 10000, 0);
    assert(statuses == 6 && compare_timestamps(elapsed, 10, 1000, 1000, 1) == AZ_TIME_EQUAL && polls == 7);

    reset();
    steps[step_count++] = (struct readiness) {2, 0, POLLIN, 0, 0};
    steps[step_count++] = (struct readiness) {3, 0, 0, 0, POLLIN};
    steps[step_count++] = (struct readiness) {5, POLLIN, POLLIN, POLLIN, 0};
    ov_wait(&context, 30000, 0);
    assert(commands == 2 && statuses == 2 && compare_timestamps(elapsed, 5, 1000, 1000, 1) == AZ_TIME_EQUAL);

    reset();
    for (unsigned i = 0; i < 20; ++i) steps[step_count++] = (struct readiness) {i + 1, 0, 0, POLLIN, 0};
    ov_wait(&context, 10000, 0);
    assert(statuses == 10 && polls == 10 && compare_timestamps(elapsed, 10, 1000, 1000, 1) == AZ_TIME_EQUAL);

    reset();
    pending_x = 1;
    steps[step_count++] = (struct readiness) {0, 0, POLLIN, POLLIN, POLLIN};
    ov_wait(&context, 30000, 0);
    assert(commands == 1 && statuses == 1 && polls == 1 && compare_timestamps(elapsed, 0, 1000, 1000, 1) == AZ_TIME_EQUAL);

    reset();
    elapsed = 31;
    ov_wait(&context, 30000, 0);
    assert(polls == 1 && compare_timestamps(elapsed, 31, 1000, 1000, 1) == AZ_TIME_EQUAL);

    reset();
    callback_duration = 40;
    steps[step_count++] = (struct readiness) {1, 0, 0, POLLIN, 0};
    ov_wait(&context, 10000, 0);
    assert(polls == 1 && statuses == 1 && compare_timestamps(elapsed, 41, 1000, 1000, 1) == AZ_TIME_EQUAL);

    reset();
    steps[step_count++] = (struct readiness) {1, 0, 0, POLLNVAL, 0};
    ov_wait(&context, 10000, 0);
    assert(polls == 1 && statuses == 0);
    puts("status UI wake pacing, independent commands, timer and X input passed");
}
