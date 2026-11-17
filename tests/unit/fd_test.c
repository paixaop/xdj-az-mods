/* SPDX-License-Identifier: GPL-3.0-only */
/* Bus fd tracking (guest/azmod/bus_interpose.c): open() records the subucom bus
 * descriptors by number and read()/ioctl() decode and edit whatever has that
 * number. run.sh extracts gpiodrv_slot, bus_fd_drop, bus_fd_forget, bus_fd_note
 * and the open/close/dup2/dup3 interposers by text, with "/dev/..." rewritten to
 * FDT_DEV "/..." so the "devices" are plain files in a scratch directory. The
 * interposers really interpose here: this binary's open/close/dup2/dup3 are the
 * extracted ones, forwarding to libc through dlsym(RTLD_NEXT), as in EP147.
 *
 * The question each case asks is the one read() asks before it decodes a mixer
 * frame (fd == g_mixer_fd && fd >= 0) or an ErP frame (fd == g_erp_fd): after the
 * bus fd is gone and its number is reused by another file, the answer must be no.
 * The control is the old code (open-only tracking), plus one hook disabled at a
 * time; each must fail. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "check.h"

/* the surroundings bus_interpose.c expects from azmod.c / azmod_internal.h */
#define AZMOD_RELEASE 1
#define AZ_NBUS 4
#define MAX_GPIODRV_FDS 16
#define NEXT(p, name)                   \
    do {                                \
        if (!p)                         \
            p = dlsym(RTLD_NEXT, name); \
    } while (0)
#define dbg(...) ((void) 0)
static volatile int g_azm_inert = 1;
static int g_mixer_fd = -1, g_erp_fd = -1;
static int g_deckbus_fd[2] = {-1, -1}, g_touchin_fd = -1; /* the community filters' descriptors */
static int g_gpiodrv_fds[MAX_GPIODRV_FDS];
static long g_gpiodrv_gpio[MAX_GPIODRV_FDS];
static int g_gpiodrv_last_level[MAX_GPIODRV_FDS];
static int g_gpiodrv_fd_count = 0;
static void tg_note(int fd, const char *path) { (void) fd, (void) path; } /* touch_read_guard.h has its own suite */
static void tg_forget(int fd) { (void) fd; }
/* Button delivery has its own real epoll/CRC suite (tests/sdk-panel.sh). */
#define EPB_V200 200
static int g_ep147_build = EPB_V200;
static void panel_forget(int fd) { (void) fd; }
static void panel_note(int fd, unsigned bus) {
    (void) fd;
    (void) bus;
}
static int (*next_open)(const char *, int, ...);

#include FD_TRACK_INC /* gpiodrv_slot, bus_fd_drop, bus_fd_forget, bus_fd_note */
#include FD_OPEN_INC  /* int open(...) */
#include FD_HOOKS_INC /* close, dup2, dup3 (empty in the old-code control) */

#define MIXER FDT_DEV "/subucom_spi3.0"
#define ERP FDT_DEV "/subucom_spi1.0"
#define DECKA FDT_DEV "/subucom_spi2.0"
#define GPIO FDT_DEV "/gpiodrv"
#define OTHER FDT_DEV "/unrelated.txt"
/* read()'s own gates, verbatim */
static int mixer_gate(int fd) { return fd == g_mixer_fd && fd >= 0; }
static int erp_gate(int fd) { return fd == g_erp_fd; }
static int gpio_tracked(int fd) {
    for (int i = 0; i < g_gpiodrv_fd_count; i++)
        if (g_gpiodrv_fds[i] == fd)
            return 1;
    return 0;
}
static int ufd; /* a spare fd for dup2/dup3 sources */

int main(void) {
    ut_suite = "fd tracking";
    char name[160];
    const char *paths[] = {MIXER, ERP, GPIO, OTHER, FDT_DEV "/subucom_spi2.0", FDT_DEV "/subucom_spi5.0"};
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) {
        FILE *f = fopen(paths[i], "w");
        if (!f) {
            printf("FAIL  setup: cannot create %s\n", paths[i]);
            return 2;
        }
        fclose(f);
    }

    /* 1. outside EP147 (g_azm_inert = 1): open is a pass-through and tracks nothing */
    int a = open(MIXER, O_RDWR);
    CHECK("inert (not EP147): opening the mixer bus is not tracked", a >= 0 && g_mixer_fd == -1 && !mixer_gate(a));
    close(a);
    g_azm_inert = 0; /* azmod_init: this is EP147 with a complete table */

    /* 2. the bus fd is recorded */
    int m = open(MIXER, O_RDWR), e;
    CHECK("mixer bus open is recorded", m >= 0 && mixer_gate(m));

    /* 3. the bus closed, another file gets the same number */
    close(m);
    int o = open(OTHER, O_RDONLY);
    snprintf(name, sizeof name, "precondition: the unrelated file reuses the number (%d -> %d)", m, o);
    CHECK(name, o == m);
    CHECK("close + reuse: the unrelated file is NOT decoded as the mixer bus", !mixer_gate(o));
    close(o);

    /* 3b. reuse that never passes through the open() hook: only close's forget can
     *     protect these (pipe(), and fopen, whose open is libc-internal) */
    m = open(MIXER, O_RDWR);
    close(m);
    int pp[2];
    if (pipe(pp) != 0) {
        printf("FAIL  setup: pipe\n");
        return 2;
    }
    CHECK("close + reuse by pipe(): the pipe is NOT decoded as the mixer bus", pp[0] == m && !mixer_gate(pp[0]));
    close(pp[0]);
    close(pp[1]);

    m = open(MIXER, O_RDWR);
    close(m);
    FILE *uf = fopen(OTHER, "r");
    CHECK("close + reuse by fopen(): the stream is NOT decoded as the mixer bus", uf && fileno(uf) == m && !mixer_gate(fileno(uf)));
    if (uf)
        fclose(uf);

    /* 4. the same for the ErP bus (pads, LED frame ioctl) */
    e = open(ERP, O_RDWR);
    CHECK("ErP bus open is recorded", e >= 0 && erp_gate(e));
    close(e);
    o = open(OTHER, O_RDONLY);
    CHECK("close + reuse: the unrelated file is NOT treated as the ErP bus", o == e && !erp_gate(o));
    close(o);

    /* 4b. a deck MCU bus (community frame filters read it) */
    e = open(DECKA, O_RDWR);
    CHECK("deck bus open is recorded", e >= 0 && g_deckbus_fd[0] == e);
    close(e);
    o = open(OTHER, O_RDONLY);
    CHECK("close + reuse: the unrelated file is NOT treated as a deck bus", o == e && g_deckbus_fd[0] != o);
    close(o);

    /* 4c. the second deck MCU bus, and a file under input/ that is no touch device */
    e = open(FDT_DEV "/subucom_spi5.0", O_RDWR);
    CHECK("the second deck bus open is recorded in its own slot", e >= 0 && g_deckbus_fd[1] == e && g_deckbus_fd[0] != e);
    close(e);
    o = open(OTHER, O_RDONLY);
    CHECK("close + reuse: the unrelated file is NOT treated as the second deck bus", o == e && g_deckbus_fd[1] != o);
    close(o);
    mkdir(FDT_DEV "/input", 0755);
    fclose(fopen(FDT_DEV "/input/event0", "w"));
    o = open(FDT_DEV "/input/event0", O_RDONLY);
    CHECK("a regular file under input/ is not taken for the touch device (only an evdev node is)",
          o >= 0 && g_touchin_fd != o);
    close(o);
    /* 4d. EP147's own creating open: its mode passes through */
    mode_t um = umask(0);
    o = open(FDT_DEV "/created.bin", O_WRONLY | O_CREAT | O_TRUNC, 0640);
    umask(um);
    struct stat cst;
    CHECK("an open with O_CREAT creates the file with the mode it was given",
          o >= 0 && fstat(o, &cst) == 0 && (cst.st_mode & 0777) == 0640);
    close(o);

    /* 5. dup2 of another file onto the bus number */
    ufd = open(OTHER, O_RDONLY);
    m = open(MIXER, O_RDWR);
    CHECK("mixer re-opened after a close is recorded again", mixer_gate(m));
    int r = dup2(ufd, m);
    CHECK("dup2(other, bus fd): the number is no longer the mixer bus", r == m && !mixer_gate(m));
    close(m);
    /* 6. dup3 likewise */
    e = open(ERP, O_RDWR);
    r = dup3(ufd, e, O_CLOEXEC);
    CHECK("dup3(other, bus fd): the number is no longer the ErP bus", r == e && !erp_gate(e));
    close(e);
    /* 7. dup2(fd, fd) and dup3 onto an UNtracked number leave the bus alone */
    m = open(MIXER, O_RDWR);
    r = dup2(m, m);
    CHECK("dup2(bus, bus) closes nothing: still the mixer bus", r == m && mixer_gate(m));
    int spare = open(OTHER, O_RDONLY);
    r = dup3(ufd, spare, 0);
    CHECK("dup3 onto an untracked number: the mixer bus is still tracked", r == spare && mixer_gate(m));
    close(spare);

    /* 8. a close the hooks never saw (raw syscall): the next open() of that number forgets it */
    syscall(SYS_close, m);
    o = open(OTHER, O_RDONLY);
    CHECK("unhooked close + reuse by open(): NOT decoded as the mixer bus", o == m && !mixer_gate(o));
    close(o);

    /* 9. gpiodrv: forgotten on close, its slot reused by the next gpiodrv open */
    int g = open(GPIO, O_RDONLY);
    int cnt = g_gpiodrv_fd_count;
    CHECK("gpiodrv open is tracked", g >= 0 && gpio_tracked(g));
    close(g);
    o = open(OTHER, O_RDONLY);
    CHECK("gpiodrv close + reuse: the unrelated file is not a gpiodrv fd", o == g && !gpio_tracked(o));
    int g2 = open(GPIO, O_RDONLY);
    CHECK("gpiodrv reopen reuses the freed slot (table does not grow)", gpio_tracked(g2) && g_gpiodrv_fd_count == cnt);
    close(g2);
    close(o);

    /* 10. a plain file with a fresh number is never a bus */
    o = open(OTHER, O_RDONLY);
    CHECK("an unrelated open is never a bus", !mixer_gate(o) && !erp_gate(o) && !gpio_tracked(o));
    close(o);
    close(ufd);
    return ut_summary();
}
