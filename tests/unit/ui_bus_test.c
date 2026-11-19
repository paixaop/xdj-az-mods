/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#define EPB_V200 200
#define MAX_GPIODRV_FDS 16
#define AZ_BUS_DECK_IN 2
#define dbg(...) ((void) 0)
#define NEXT(pointer, name) assert(pointer)
static int g_ep147_build = EPB_V200, g_azm_inert, g_debug = -1;
static int g_mixer_fd = -1, g_erp_fd = -1, g_deckbus_fd[2] = {-1, -1}, g_touchin_fd = -1;
static int g_gpiodrv_fds[MAX_GPIODRV_FDS], g_gpiodrv_fd_count;
static long g_gpiodrv_gpio[MAX_GPIODRV_FDS];
static int g_gpiodrv_last_level[MAX_GPIODRV_FDS];
static int panel_fd = -1, panel_bus = -1, forgotten_fd = -1, touch_forgotten = -1;
static const char *noted_path;
static void panel_note(int fd, unsigned bus) {
    panel_fd = fd;
    panel_bus = (int) bus;
}
static void panel_forget(int fd) { forgotten_fd = fd; }
static void tg_forget(int fd) { touch_forgotten = fd; }
static void tg_note(int fd, const char *path) {
    assert(fd == forgotten_fd);
    noted_path = path;
}
static int stat_error;
static mode_t stat_mode = S_IFCHR;
static unsigned stat_major = 13;
static int fixture_fstat(int fd, struct stat *st) {
    assert(fd == 70);
    if (stat_error) return -1;
    memset(st, 0, sizeof *st);
    st->st_mode = stat_mode;
    st->st_rdev = makedev(stat_major, 0);
    return 0;
}
static char calls[128];
static unsigned ncalls;
static void note(char c) {
    assert(ncalls + 1 < sizeof calls);
    calls[ncalls++] = c;
    calls[ncalls] = 0;
}
static char buffer[8];
static ssize_t native_result = sizeof buffer, panel_result = -2;
static uint64_t frame_config = 123;
static ssize_t native_read(int fd, void *buf, size_t n) {
    (void) fd;
    assert(buf == buffer && n == sizeof buffer);
    note('R');
    return native_result;
}
static ssize_t (*next_read)(int, void *, size_t) = native_read;
static ssize_t panel_read(int fd, void *buf, size_t n) {
    (void) fd;
    assert(buf == buffer && n == sizeof buffer);
    note('P');
    return panel_result;
}
static void panel_observe(int fd, void *buf, ssize_t n) {
    assert((fd == g_mixer_fd || fd == g_erp_fd) && buf == buffer && n == native_result);
    note('O');
}
static uint64_t bus_cfg(void) {
    note('C');
    return frame_config;
}
static void bus_capture_read(int fd, void *buf, ssize_t r, size_t n, uintptr_t caller) {
    (void) fd;
    assert(buf == buffer && r == (panel_result == -2 ? native_result : panel_result));
    assert(n == sizeof buffer && caller);
    note('A');
}
static void gpiodrv_note(int fd, void *buf, ssize_t r) {
    (void) fd;
    (void) r;
    assert(buf == buffer);
    note('G');
}
static ssize_t tg_result(int fd, ssize_t r) {
    assert(fd == 70 && r == -1);
    note('T');
    return 0;
}
static void bus_mixer_frame(void *buf, ssize_t r, uint64_t cfg) {
    assert(buf == buffer && r == sizeof buffer && cfg == frame_config);
    note('M');
}
static void bus_erp_frame(void *buf, ssize_t r) {
    assert(buf == buffer && r == sizeof buffer);
    note('E');
}
static void sdk_filter_view(int bus, void *buf, int r) {
    assert(bus == AZ_BUS_DECK_IN && buf == buffer && r == sizeof buffer);
    note('D');
}
static void sdk_touch_read(void *buf, ssize_t r) {
    assert(buf == buffer && r == sizeof buffer);
    note('S');
}
static int epoll_result;
static int native_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    assert(epfd == 4 && op == EPOLL_CTL_ADD && fd == 70 && event);
    note('c');
    return epoll_result;
}
static int native_wait(int epfd, struct epoll_event *events, int count, int timeout) {
    assert(epfd == 4 && events && count == 1 && timeout == 8);
    note('w');
    return epoll_result;
}
static int (*next_epoll_ctl)(int, int, int, struct epoll_event *) = native_ctl;
static int (*next_epoll_wait)(int, struct epoll_event *, int, int) = native_wait;
static void panel_registration(int epfd, int op, int fd, struct epoll_event *event) {
    assert(epfd == 4 && op == EPOLL_CTL_ADD && fd == 70 && event);
    note('r');
}
static int panel_epoll(int epfd, struct epoll_event *events, int count, int result, int timeout) {
    assert(epfd == 4 && events && count == 1 && result == epoll_result && timeout == 8);
    note('e');
    return result + 1;
}
/* Symbol aliases prevent the fixture from intercepting the C runtime's own IO.
 * The complete production definitions and all device path strings stay unchanged. */
#define read fixture_read
#define epoll_ctl fixture_epoll_ctl
#define epoll_wait fixture_epoll_wait
#define fstat fixture_fstat
#include ROUTING_IMPL_INC
#undef read
#undef epoll_ctl
#undef epoll_wait
#undef fstat
static void expected(const char *sequence) {
    assert(!strcmp(calls, sequence));
    ncalls = 0;
    calls[0] = 0;
}
static void devices(void) {
    bus_fd_forget(-1);
    assert(forgotten_fd == -1);
    bus_fd_note(10, "/dev/subucom_spi3.0");
    assert(g_mixer_fd == 10 && panel_fd == 10 && panel_bus == 0);
    assert(!strcmp(noted_path, "/dev/subucom_spi3.0"));
    bus_fd_note(11, "/dev/subucom_spi1.0");
    assert(g_erp_fd == 11 && panel_fd == 11 && panel_bus == 1);
    g_ep147_build = 130;
    bus_fd_note(12, "/dev/subucom_spi3.0");
    bus_fd_note(13, "/dev/subucom_spi1.0");
    assert(g_mixer_fd == 12 && g_erp_fd == 13 && panel_fd == 11);
    g_ep147_build = EPB_V200;
    bus_fd_note(20, "/dev/subucom_spi2.0");
    bus_fd_note(21, "/dev/subucom_spi5.0");
    assert(g_deckbus_fd[0] == 20 && g_deckbus_fd[1] == 21);
    bus_fd_note(30, "/dev/gpiodrv");
    assert(g_gpiodrv_fd_count == 1 && g_gpiodrv_fds[0] == 30);
    bus_fd_note(30, "/dev/gpiodrv");
    assert(g_gpiodrv_fd_count == 1 && g_gpiodrv_gpio[0] == -1);
    stat_error = 1;
    bus_fd_note(70, "/dev/input/event0");
    assert(g_touchin_fd == -1);
    stat_error = 0;
    stat_mode = S_IFREG;
    bus_fd_note(70, "/dev/input/event0");
    assert(g_touchin_fd == -1);
    stat_mode = S_IFCHR;
    stat_major = 1;
    bus_fd_note(70, "/dev/input/event0");
    assert(g_touchin_fd == -1);
    stat_major = 13;
    bus_fd_note(70, "/dev/input/event0");
    assert(g_touchin_fd == 70);
    bus_fd_note(12, "/tmp/unrelated");
    assert(g_mixer_fd == -1 && forgotten_fd == 12 && touch_forgotten == 12);
    bus_fd_note(13, NULL);
    assert(g_erp_fd == -1);
    bus_fd_forget(30);
    assert(g_gpiodrv_fds[0] == -1 && g_gpiodrv_gpio[0] == -1);
    bus_fd_forget(70);
    assert(g_touchin_fd == -1);
    bus_fd_forget(20);
    bus_fd_forget(21);
    assert(g_deckbus_fd[0] == -1 && g_deckbus_fd[1] == -1);
}
static void frames(void) {
    g_mixer_fd = 10;
    g_erp_fd = 11;
    g_deckbus_fd[0] = 20;
    g_deckbus_fd[1] = 21;
    g_touchin_fd = 70;
    const int fds[] = {10, 11, 20, 21, 70, 99};
    const char *sequence[] = {"PROCAGM", "PROCAGE", "PRCAGD", "PRCAGD", "PRCAGS", "PRCAG"};
    for (unsigned i = 0; i < sizeof fds / sizeof *fds; i++) {
        assert(fixture_read(fds[i], buffer, sizeof buffer) == sizeof buffer);
        expected(sequence[i]);
    }
    panel_result = sizeof buffer;
    assert(fixture_read(10, buffer, sizeof buffer) == sizeof buffer);
    expected("PCAGM"); /* panel-owned delivery does not reread or observe twice */
    panel_result = -2;
    native_result = -1;
    assert(fixture_read(70, buffer, sizeof buffer) == 0);
    expected("PRCAGT");
    native_result = 0;
    assert(fixture_read(70, buffer, sizeof buffer) == 0);
    expected("PRCAG");
    native_result = sizeof buffer;
    assert(fixture_read(-1, buffer, sizeof buffer) == sizeof buffer);
    expected("PRCAG");
    g_azm_inert = 1;
    assert(fixture_read(10, buffer, sizeof buffer) == sizeof buffer);
    expected("R");
    g_azm_inert = 0;
}
static void epoll_routing(void) {
    struct epoll_event event = {0};
    assert(!fixture_epoll_ctl(4, EPOLL_CTL_ADD, 70, &event));
    expected("cr");
    epoll_result = -1;
    assert(fixture_epoll_ctl(4, EPOLL_CTL_ADD, 70, &event) == -1);
    expected("c");
    epoll_result = 0;
    assert(fixture_epoll_wait(4, &event, 1, 8) == 1);
    expected("we");
    for (int inert = 0; inert < 2; inert++) {
        g_azm_inert = inert;
        g_ep147_build = inert ? EPB_V200 : 130;
        assert(!fixture_epoll_ctl(4, EPOLL_CTL_ADD, 70, &event));
        expected("c");
        assert(!fixture_epoll_wait(4, &event, 1, 8));
        expected("w");
    }
}
int main(void) {
    devices();
    frames();
    epoll_routing();
    puts("PASS bus routing: exact device admission, descriptor reuse, read ordering and epoll gates");
    return 0;
}
