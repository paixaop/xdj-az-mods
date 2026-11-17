/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
static uint64_t now_ns = 1000000000u;
static int test_clock(clockid_t clock, struct timespec *ts) {
    assert(clock == CLOCK_MONOTONIC);
    ts->tv_sec = (time_t) (now_ns / 1000000000u);
    ts->tv_nsec = (long) (now_ns % 1000000000u);
    return 0;
}
static uint16_t crc16_x25(const uint8_t *f, size_t n) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < n; i++) {
        crc ^= f[i];
        for (int j = 0; j < 8; j++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc ^ 0xffff;
}
#define clock_gettime test_clock
#include "azmod/sdk_panel_bus.h"
#undef clock_gettime
static int owner, foreign;
static void stamp(unsigned bus, unsigned char *f) {
    size_t n = bus ? 28 : 96;
    uint16_t crc = crc16_x25(f, n);
    f[n] = (unsigned char) crc;
    f[n + 1] = (unsigned char) (crc >> 8);
}
int main(void) {
    assert(panel_read(-1, NULL, 0) == -2);
    int pipes[2];
    assert(!pipe(pipes));
    int ep = epoll_create1(EPOLL_CLOEXEC);
    assert(ep >= 0);
    struct epoll_event registration = {.events = EPOLLIN, .data.u64 = UINT64_C(0xfedcba9876543210)};
    assert(!epoll_ctl(ep, EPOLL_CTL_ADD, pipes[0], &registration));
    panel_note(pipes[0], 0);
    panel_registration(ep, EPOLL_CTL_ADD, pipes[0], &registration);
    struct epoll_event ev;
    uint64_t token = 0;
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 180, &token) == AZ_BUSY);
    unsigned char baseline[128] = {0}, frame[128];
    baseline[6] = 8;
    baseline[40] = 123;
    baseline[72] = 87;
    stamp(0, baseline);
    memcpy(frame, baseline, 128);
    frame[0] ^= 1;
    panel_observe(pipes[0], frame, 128);
    assert(!g_panel_bus[0].len);
    memcpy(frame, baseline, 128);
    panel_observe(pipes[0], frame, 127);
    assert(!g_panel_bus[0].len);
    panel_observe(pipes[0], frame, 128);
    assert(g_panel_bus[0].len == 128);
    assert(!panel_epoll(ep, &ev, 1, 0, -1));
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 180, &token) == AZ_BUSY);
    assert(!panel_epoll(ep, &ev, 1, 0, 1));
    assert(panel_submit(&owner, 999, 180, &token) == AZ_INVALID);
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 0, &token) == AZ_INVALID);
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 3001, &token) == AZ_INVALID);
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 180, NULL) == AZ_INVALID);
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 180, &token) == AZ_OK);
    uint64_t old = token;
    assert(panel_submit(&foreign, AZ_PANEL_BROWSE, 180, &token) == AZ_BUSY && token == old);
    assert(panel_poll(&foreign, token) == AZ_STALE);
    assert(panel_poll(&owner, token) == AZ_PENDING);
    assert(!panel_epoll(ep + 1, &ev, 1, 0, 1));
    assert(panel_read(pipes[0], frame, 128) == -2);
    /* The actual kernel reports no readable hardware. The core supplies one
     * bounded event with the exact registration identity and a valid frame. */
    int n = epoll_wait(ep, &ev, 1, 0);
    assert(n == 0);
    assert(panel_epoll(ep, &ev, 1, n, 1) == 1 && ev.events == EPOLLIN && ev.data.u64 == registration.data.u64);
    assert(panel_read(pipes[0], frame, 128) == 128);
    assert(frame[32] == 128 && frame[6] == 0 && frame[40] == 123 && frame[72] == 87);
    assert(panel_frame_valid(0, frame, 128));
    assert(panel_read(pipes[0], frame, 128) == -2);
    assert(!panel_epoll(ep, &ev, 1, 0, 1));
    now_ns += 180000000u;
    assert(panel_epoll(ep, &ev, 1, 0, 1) == 1);
    assert(panel_read(pipes[0], frame, 128) == 128 && !memcmp(frame, baseline, 128));
    assert(panel_poll(&owner, token) == AZ_OK);
    /* Real events are never replaced. While held, preserve their updated controls. */
    assert(panel_submit(&owner, AZ_PANEL_LOAD_2, 100, &token) == AZ_OK);
    assert(panel_poll(&owner, old) == AZ_STALE);
    ev.events = EPOLLIN | EPOLLHUP;
    ev.data.u64 = 44;
    assert(panel_epoll(ep, &ev, 1, 1, 1) == 1 && ev.data.u64 == 44 && ev.events == (EPOLLIN | EPOLLHUP));
    memcpy(frame, baseline, 128);
    frame[40] = 25;
    stamp(0, frame);
    panel_observe(pipes[0], frame, 128);
    assert(frame[33] == 4 && frame[40] == 25 && panel_frame_valid(0, frame, 128));
    now_ns += 100000000u;
    assert(panel_epoll(ep, &ev, 1, 0, 1) == 1);
    assert(panel_read(pipes[0], frame, 128) == 128 && !frame[33] && frame[40] == 25);
    assert(panel_poll(&owner, token) == AZ_OK);
    /* Queued input times out without ever pressing. */
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 180, &token) == AZ_OK);
    now_ns += 2000000000u;
    assert(panel_poll(&owner, token) == AZ_FAILED);
    assert(!panel_epoll(ep, &ev, 1, 0, 1));
    frame[32] = 128;
    stamp(0, frame);
    panel_observe(pipes[0], frame, 128);
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 180, &token) == AZ_BUSY);
    frame[32] = 0;
    stamp(0, frame);
    panel_observe(pipes[0], frame, 128);
    /* Lock contention cannot block realtime input or lose kernel results. */
    pthread_mutex_lock(&g_panel_mu);
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 180, &token) == AZ_BUSY);
    assert(panel_poll(&owner, token) == AZ_BUSY);
    assert(panel_epoll(ep, &ev, 1, -1, 1) == -1);
    panel_observe(pipes[0], frame, 128);
    pthread_mutex_unlock(&g_panel_mu);
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 180, &token) == AZ_OK);
    panel_forget(ep);
    assert(panel_poll(&owner, token) == AZ_STALE);
    assert(panel_submit(&owner, AZ_PANEL_SOURCE, 180, &token) == AZ_BUSY);
    /* ERP and logical deck selection use their own CRC and byte layout. */
    int fd = 500;
    panel_note(fd, 1);
    registration.data.u64 = fd;
    panel_registration(ep, EPOLL_CTL_ADD, fd, &registration);
    memset(frame, 0, 128);
    frame[24] = 1;
    stamp(1, frame);
    panel_observe(fd, frame, 128);
    assert(!panel_epoll(ep, &ev, 1, 0, 1));
    assert(panel_submit(&owner, AZ_PANEL_DECK_4, 40, &token) == AZ_OK);
    assert(panel_epoll(ep, &ev, 1, 0, 1) == 1 && ev.data.u64 == 500);
    assert(panel_read(fd, frame, 128) == 128 && frame[17] == 1 && panel_frame_valid(1, frame, 128));
    now_ns += 40000000u;
    assert(panel_epoll(ep, &ev, 1, 0, 1) == 1);
    assert(panel_read(fd, frame, 128) == 128 && !frame[17] && panel_poll(&owner, token) == AZ_OK);
    assert(panel_submit(&owner, AZ_PANEL_PLAY_RIGHT, 40, &token) == AZ_OK);
    panel_forget(fd);
    assert(panel_poll(&owner, token) == AZ_STALE);
    assert(panel_read(fd, frame, 128) == -2);
    close(ep);
    close(pipes[0]);
    close(pipes[1]);
    puts("PASS: panel delivery without hardware input, CRC/control preservation, release, ownership, timeout, FD lifecycle and contention");
}
