/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "diagnostic.h"
#include "capture.h"
#include "profiles.h"
#include <link.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#define SLIP_MAX_TICKS 18000u
extern void azdiag_slip_entry_0(void) __attribute__((visibility("hidden")));
static const struct az_slip_diagnostic_host *slip_core;
static _Atomic int slip_installed;
struct slip_image {
    uintptr_t base;
    struct slip_rx rx[16];
    size_t count;
    int found;
};
static int slip_image_segment(struct slip_image *image, const ElfW(Phdr) * p)
{
    if (image->count == 16 || image->base > UINTPTR_MAX - p->p_vaddr) return 0;
    uintptr_t begin = image->base + p->p_vaddr;
    if (begin > UINTPTR_MAX - p->p_memsz) return 0;
    image->rx[image->count++] = (struct slip_rx){ begin, begin + p->p_memsz };
    return 1;
}

static int slip_main_image(struct dl_phdr_info *info, size_t size, void *user)
{
    (void)size;
    struct slip_image *image = user;
    if (info->dlpi_name && info->dlpi_name[0]) return 0;
    image->base = info->dlpi_addr;
    for (unsigned i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if (!slip_image_segment(image, p)) return 1;
    }
    image->found = 1;
    return 1;
}
static void slip_stats(uint64_t emitted, uint64_t missed, const char *reason)
{
    struct slip_capture_stats s;
    slip_capture_get_stats(&s);
    slip_core->log( "SDK_SLIP_CAPTURE_STATS v=1 saved=%llu emitted=%llu dropped=%llu rejected=%llu missed=%llu reason=%s",
                   (unsigned long long)s.saved, (unsigned long long)emitted, (unsigned long long)s.dropped,
                   (unsigned long long)s.rejected, (unsigned long long)missed, reason);
}
static void slip_emit_records(uint64_t *next, uint64_t *emitted, uint64_t *missed, uint64_t saved)
{
    uint64_t first = saved > 128 ? saved - 127 : 1;
    if ((*next) < first) {
        (*missed) += first - (*next);
        (*next) = first;
    }
    while ((*next) <= saved) {
        struct slip_capture_record record;
        if (!slip_capture_read((*next), &record)) break;
        slip_core->log( "SDK_SLIP_CAPTURE v=1 seq=%llu observed_ns=%llu raw=%u receiver=0x%llx caller=0x%llx",
                       (unsigned long long)record.sequence, (unsigned long long)record.observed_ns, record.raw,
                       (unsigned long long)record.receiver, (unsigned long long)record.caller);
        (*emitted)++;
        (*next)++;
    }
}

static void *slip_worker(void *unused)
{
    (void)unused;
    uint64_t next = 1, emitted = 0, missed = 0;
    for (unsigned tick = 0; tick < SLIP_MAX_TICKS; tick++) {
        int state = atomic_load_explicit(&slip_installed, memory_order_acquire);
        if (state < 0) {
            slip_stats(emitted, missed, "install_failed");
            return NULL;
        }
        if (state > 0) {
            struct slip_capture_stats stats;
            slip_capture_get_stats(&stats);
            slip_emit_records(&next, &emitted, &missed, stats.saved);
            if (tick % 100u == 0) slip_stats(emitted, missed, "running");
            if (stats.saved == 512 && next > stats.saved) {
                slip_stats(emitted, missed, "record_limit");
                return NULL;
            }
        }
        const struct timespec delay = { 0, 100000000 };
        if (nanosleep(&delay, NULL) && errno != EINTR) {
            slip_stats(emitted, missed, "sleep");
            return NULL;
        }
    }
    slip_stats(emitted, missed, "time_limit");
    return NULL;
}
static int slip_core_valid(const struct az_slip_diagnostic_host *core)
{
    return core && core->log && core->start_worker && core->install;
}

static const struct slip_profile *slip_find_profile(const char *tag, const struct slip_image *image)
{
    for (size_t i = 0; i < sizeof slip_profiles / sizeof *slip_profiles; i++)
        if (slip_profile_matches(&slip_profiles[i], tag, image->base, image->rx, image->count)) return &slip_profiles[i];
    return NULL;
}

int az_slip_diagnostic_start(const char *tag, const struct az_slip_diagnostic_host *core)
{
    if (!slip_core_valid(core)) return -1;
    const struct slip_profile *profile = NULL;
    struct slip_image image = { 0 };
    dl_iterate_phdr(slip_main_image, &image);
    if (!image.found) return -1;
    profile = slip_find_profile(tag, &image);
    if (!profile) return -1;
    if (!slip_capture_arm(image.base + profile->caller)) return -1;
    slip_core = core;
    if (core->start_worker(slip_worker, NULL)) return -1;
    /* Last fallible action. Core process ownership keeps this code and its worker mapped. */
    if (core->install( image.base + profile->callback.rva, (void *)azdiag_slip_entry_0, &azdiag_slip_entry_originals[0])) {
        atomic_store_explicit(&slip_installed, -1, memory_order_release);
        return -1;
    }
    atomic_store_explicit(&slip_installed, 1, memory_order_release);
    core->log( "SDK_SLIP_CAPTURE_READY v=1 profile=%s target=0x%llx caller=0x%llx max_records=512 ring=128 semantics=raw_no_deck",
              profile->name, (unsigned long long)(image.base + profile->callback.rva), (unsigned long long)(image.base + profile->caller));
    return 0;
}
