/* SPDX-License-Identifier: GPL-3.0-only */
/* Core-owned button delivery. All state is protected by a short mutex; the
 * realtime read/epoll paths only try-lock. Frames are copied before any filters.
 * No disk, allocation or mod callback occurs while the mutex is held. */
#ifndef SDK_PANEL_BUS_H
#define SDK_PANEL_BUS_H
#include <pthread.h>
#include <sys/epoll.h>
#include "azmod-mod.h"
#include "../common/aztimestamp.h"

struct sdk_panel_bus {
    int fd, epfd, ready, len;
    uint64_t data;
    unsigned char frame[128];
};
static struct sdk_panel_bus g_panel_bus[2] = {{.fd = -1, .epfd = -1}, {.fd = -1, .epfd = -1}};
static pthread_mutex_t g_panel_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t token, deadline;
    const void *owner;
    unsigned bus, byte, mask, hold_ms;
    int phase, result; /* 1 queued, 2 held, 0 complete */
} g_panel_job;

static uint64_t panel_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L || (uint64_t) ts.tv_sec > UINT64_MAX / 1000000000u - 4)
        return 0;
    return (uint64_t) ts.tv_sec * 1000000000u + (uint64_t) ts.tv_nsec;
}
static int panel_due(uint64_t now, uint64_t deadline) {
    int order = compare_timestamps(now, deadline, 1000000000u, 1000000000u, 1);
    return !now || order == AZ_TIME_INVALID || order >= AZ_TIME_EQUAL;
}
static int panel_button_map(unsigned button, unsigned *bus, unsigned *byte, unsigned *mask) {
    static const unsigned char map[][3] = {
        {0, 0, 0}, {0, 32, 128}, {0, 32, 64}, {0, 33, 128}, {0, 32, 4}, {0, 32, 8}, {0, 31, 1}, {0, 33, 8}, {0, 33, 4}, {0, 33, 2}, {0, 33, 1}, {1, 8, 1}, {1, 16, 1}, {1, 9, 1}, {1, 17, 1}, {1, 10, 2}, {1, 18, 2}, {1, 10, 4}, {1, 18, 4}};
    if (!button || button >= sizeof map / sizeof map[0]) return 0;
    *bus = map[button][0];
    *byte = map[button][1];
    *mask = map[button][2];
    return 1;
}
static int panel_frame_valid(unsigned bus, const unsigned char *f, size_t n) {
    size_t crc = bus ? 28 : 96;
    return n == 128 && (!bus || (f[24] & (unsigned char) ~6u) == 1) &&
           crc16_x25(f, crc) == (uint16_t) (f[crc] | ((unsigned) f[crc + 1] << 8));
}
static void panel_finish(int result) {
    g_panel_job.phase = 0;
    g_panel_job.result = result;
}
static void panel_forget(int fd) {
    pthread_mutex_lock(&g_panel_mu);
    for (unsigned i = 0; i < 2; ++i) {
        struct sdk_panel_bus *b = &g_panel_bus[i];
        if (b->fd == fd || b->epfd == fd) {
            if (g_panel_job.phase && g_panel_job.bus == i) panel_finish(AZ_STALE);
            if (b->fd == fd) {
                b->fd = -1;
                b->len = 0;
            }
            b->epfd = -1;
            b->ready = 0;
        }
    }
    pthread_mutex_unlock(&g_panel_mu);
}
static void panel_note(int fd, unsigned bus) {
    pthread_mutex_lock(&g_panel_mu);
    g_panel_bus[bus] = (struct sdk_panel_bus) {.fd = fd, .epfd = -1};
    pthread_mutex_unlock(&g_panel_mu);
}
static void panel_registration(int epfd, int op, int fd, const struct epoll_event *ev) {
    pthread_mutex_lock(&g_panel_mu);
    for (unsigned i = 0; i < 2; ++i) {
        struct sdk_panel_bus *b = &g_panel_bus[i];
        if (b->fd != fd) continue;
        if (g_panel_job.phase && g_panel_job.bus == i) panel_finish(AZ_STALE);
        b->epfd = -1;
        b->ready = 0;
        if (op != EPOLL_CTL_DEL && ev && (ev->events & EPOLLIN) &&
            !(ev->events & (EPOLLET | EPOLLONESHOT))) {
            b->epfd = epfd;
            b->data = ev->data.u64;
        }
    }
    pthread_mutex_unlock(&g_panel_mu);
}
static int panel_submit(const void *owner, unsigned button, unsigned hold_ms, uint64_t *token) {
    unsigned bus, byte, mask;
    if (!owner || !token || hold_ms < 40 || hold_ms > 3000 ||
        !panel_button_map(button, &bus, &byte, &mask)) return AZ_INVALID;
    if (pthread_mutex_trylock(&g_panel_mu)) return AZ_BUSY;
    struct sdk_panel_bus *b = &g_panel_bus[bus];
    uint64_t now = panel_now();
    int rc = AZ_BUSY;
    if (!g_panel_job.phase && b->ready && b->len == 128 &&
        !(b->frame[byte] & mask) && now) {
        if (g_panel_job.token == UINT64_MAX) rc = AZ_LIMIT;
        else {
            ++g_panel_job.token;
            g_panel_job.owner = owner;
            g_panel_job.bus = bus;
            g_panel_job.byte = byte;
            g_panel_job.mask = mask;
            g_panel_job.hold_ms = hold_ms;
            g_panel_job.deadline = now + 2000000000u;
            g_panel_job.phase = 1;
            g_panel_job.result = AZ_PENDING;
            *token = g_panel_job.token;
            rc = AZ_OK;
        }
    }
    pthread_mutex_unlock(&g_panel_mu);
    return rc;
}
static int panel_poll(const void *owner, uint64_t token) {
    if (pthread_mutex_trylock(&g_panel_mu)) return AZ_BUSY;
    int rc = AZ_STALE;
    if (token && token == g_panel_job.token && owner == g_panel_job.owner) {
        if (g_panel_job.phase == 1 && panel_due(panel_now(), g_panel_job.deadline)) panel_finish(AZ_FAILED);
        rc = g_panel_job.result;
    }
    pthread_mutex_unlock(&g_panel_mu);
    return rc;
}
/* Runs with the lock held, on the firmware's original input thread. */
static void panel_apply(unsigned bus, unsigned char *f) {
    if (!g_panel_job.phase || g_panel_job.bus != bus) return;
    uint64_t now = panel_now();
    if (g_panel_job.phase == 1) {
        if (panel_due(now, g_panel_job.deadline)) {
            panel_finish(AZ_FAILED);
            return;
        }
        g_panel_job.phase = 2;
        g_panel_job.deadline = now + (uint64_t) g_panel_job.hold_ms * 1000000u;
    } else if (panel_due(now, g_panel_job.deadline)) {
        panel_finish(AZ_OK); /* this frame contains the current physical release */
        return;
    }
    f[g_panel_job.byte] |= (unsigned char) g_panel_job.mask;
    if (!bus) f[6] &= (unsigned char) ~8u; /* mixer press latch */
    size_t crc = bus ? 28 : 96;
    uint16_t v = crc16_x25(f, crc);
    f[crc] = (unsigned char) v;
    f[crc + 1] = (unsigned char) (v >> 8);
}
static void panel_observe(int fd, unsigned char *f, ssize_t n) {
    if (pthread_mutex_trylock(&g_panel_mu)) return;
    for (unsigned i = 0; i < 2; ++i) {
        struct sdk_panel_bus *b = &g_panel_bus[i];
        if (b->fd == fd && n > 0 && panel_frame_valid(i, f, (size_t) n)) {
            memcpy(b->frame, f, (size_t) n);
            b->len = (int) n;
            panel_apply(i, f);
        }
    }
    pthread_mutex_unlock(&g_panel_mu);
}
/* The qualified native reader polls every millisecond. Only a matching observed
 * bounded poll can establish readiness. Preserve kernel events and their data.
 * A synthetic event is emitted only for an otherwise empty result. */
static __thread int g_panel_synthetic_fd = -1;
static int panel_epoll(int epfd, struct epoll_event *events, int count, int result, int timeout) {
    g_panel_synthetic_fd = -1;
    if (pthread_mutex_trylock(&g_panel_mu)) return result;
    for (unsigned i = 0; i < 2; ++i) {
        struct sdk_panel_bus *b = &g_panel_bus[i];
        if (b->epfd != epfd) continue;
        b->ready = timeout >= 0 && timeout <= 10;
        if (!b->ready || result || count < 1 || !events || !g_panel_job.phase || g_panel_job.bus != i) continue;
        if (g_panel_job.phase == 2 && !panel_due(panel_now(), g_panel_job.deadline)) continue;
        events[0].events = EPOLLIN;
        events[0].data.u64 = b->data;
        g_panel_synthetic_fd = b->fd;
        result = 1;
    }
    pthread_mutex_unlock(&g_panel_mu);
    return result;
}
static ssize_t panel_read(int fd, void *out, size_t cap) {
    if (fd < 0 || fd != g_panel_synthetic_fd) return -2;
    g_panel_synthetic_fd = -1;
    if (pthread_mutex_trylock(&g_panel_mu)) {
        errno = EAGAIN;
        return -1;
    }
    ssize_t n = -1;
    for (unsigned i = 0; i < 2; ++i) {
        struct sdk_panel_bus *b = &g_panel_bus[i];
        if (b->fd == fd && b->len == 128 && cap >= 128 && out) {
            memcpy(out, b->frame, 128);
            panel_apply(i, out);
            n = 128;
        }
    }
    pthread_mutex_unlock(&g_panel_mu);
    if (n < 0) errno = EAGAIN;
    return n;
}
#endif
