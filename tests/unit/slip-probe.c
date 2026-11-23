/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include "../../src/common/aztimestamp.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../../src/azmod/slip_diagnostic/binding.h"
#define SLIP_PROFILES_H
#define SLIP_PROFILE_TAGS "fixture"
static struct slip_profile slip_profiles[1];
#define dl_iterate_phdr fixture_images
#define nanosleep fixture_sleep
#include "../../src/azmod/slip_diagnostic/diagnostic.c"
#undef dl_iterate_phdr
#undef nanosleep
static unsigned char executable[64];
static char digest[65], last_reason[128];
static int omit_image, thread_error, claim_error, arm_error, sleep_error;
static unsigned threads, claims, captures, sleeps, missing_record;
static uint64_t produced;
void *azdiag_slip_entry_originals[AZCE_KIND_COUNT];
void azdiag_slip_entry_0(void) {}
int fixture_images(int (*fn)(struct dl_phdr_info *, size_t, void *), void *user)
{
    if (omit_image) return 0;
    ElfW(Phdr) segments[] = {
        { .p_type = PT_NOTE },
        { .p_type = PT_LOAD, .p_flags = PF_R },
        { .p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_memsz = sizeof executable },
    };
    struct dl_phdr_info image = { .dlpi_addr = (uintptr_t)executable, .dlpi_name = "shared.so", .dlpi_phdr = segments, .dlpi_phnum = 3 };
    assert(!fn(&image, sizeof image, user));
    image.dlpi_name = "";
    return fn(&image, sizeof image, user);
}
int fixture_sleep(const struct timespec *delay, struct timespec *remaining)
{
    assert(compare_timestamps(delay->tv_sec, 0, 1, 1000000000, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(delay->tv_nsec, 100000000, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL && !remaining);
    ++sleeps;
    errno = EIO;
    return sleep_error ? -1 : 0;
}
int slip_capture_arm(uintptr_t caller)
{
    assert(caller == (uintptr_t)executable + 1);
    return !arm_error;
}
void slip_capture_get_stats(struct slip_capture_stats *out)
{
    *out = (struct slip_capture_stats){ .saved = produced };
}
int slip_capture_read(uint64_t sequence, struct slip_capture_record *out)
{
    assert(sequence && sequence <= produced);
    if (sequence == missing_record) return 0;
    *out = (struct slip_capture_record){ .sequence = sequence, .observed_ns = sequence, .raw = 7 };
    return 1;
}
static void log_line(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    assert(n > 0 && (size_t)n < sizeof line);
    if (strstr(line, "SDK_SLIP_CAPTURE v=1")) ++captures;
    const char *reason = strstr(line, "reason=");
    if (reason) snprintf(last_reason, sizeof last_reason, "%s", reason + 7);
}
static int start(void *(*fn)(void *), void *user)
{
    assert(fn == slip_worker && !user);
    ++threads;
    return thread_error;
}
static int claim(uintptr_t target, void *replacement, void **original)
{
    assert(target == (uintptr_t)executable && replacement == (void *)azdiag_slip_entry_0);
    assert(original == &azdiag_slip_entry_originals[0]);
    ++claims;
    return claim_error;
}
int main(void)
{
    azsha256_hex(executable, sizeof executable, digest);
    slip_profiles[0] = (struct slip_profile){ .name = "test", .fw_tag = "fixture", .callback = { 0, sizeof executable, digest }, .dispatcher = { 0, sizeof executable, digest }, .caller = 1 };
    const struct az_slip_diagnostic_host host = { .log = log_line, .install = claim, .start_worker = start };
    assert(az_slip_diagnostic_start("fixture", NULL) == -1);
    omit_image = 1;
    assert(az_slip_diagnostic_start("fixture", &host) == -1);
    omit_image = 0;
    slip_profiles[0].fw_tag = "wrong";
    assert(az_slip_diagnostic_start("fixture", &host) == -1);
    slip_profiles[0].fw_tag = "fixture";
    arm_error = 1;
    assert(az_slip_diagnostic_start("fixture", &host) == -1);
    arm_error = 0;
    thread_error = 1;
    assert(az_slip_diagnostic_start("fixture", &host) == -1 && threads == 1 && !claims);
    thread_error = 0;
    claim_error = 1;
    assert(az_slip_diagnostic_start("fixture", &host) == -1 && claims == 1 && atomic_load(&slip_installed) == -1);
    slip_worker(NULL);
    assert(!strcmp(last_reason, "install_failed"));
    claim_error = 0;
    assert(!az_slip_diagnostic_start("fixture", &host) && claims == 2 && atomic_load(&slip_installed) == 1);
    produced = 512;
    slip_worker(NULL);
    assert(captures == 128 && !strcmp(last_reason, "record_limit"));
    produced = 3;
    captures = 0;
    missing_record = 2;
    sleep_error = 1;
    slip_worker(NULL);
    assert(captures == 1 && !strcmp(last_reason, "sleep"));
    atomic_store(&slip_installed, 0);
    sleep_error = 0;
    sleeps = 0;
    slip_worker(NULL);
    assert(sleeps == SLIP_MAX_TICKS && !strcmp(last_reason, "time_limit"));
    struct slip_image image = { .base = UINTPTR_MAX };
    ElfW(Phdr) segment = { .p_vaddr = 1, .p_memsz = 2 };
    assert(!slip_image_segment(&image, &segment));
    image.base = UINTPTR_MAX - 1;
    segment.p_vaddr = 0;
    assert(!slip_image_segment(&image, &segment));
    image.base = 0;
    image.count = 16;
    assert(!slip_image_segment(&image, &segment));
    puts("PASS slip probe: binding, install lifetime, bounded drain and shutdown reasons");
    return 0;
}
