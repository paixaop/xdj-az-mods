/* SPDX-License-Identifier: GPL-3.0-only */
/* Opt-in OWN azmod scalar diagnostic. Never resolves or calls an EP147 address.
 * Build macros come from one SHA-checked full/stripped azmod artifact pair:
 *   AZSDK_SOURCE_DIAGNOSTIC_DELTA = signed(candidate - azmod_dm_key)
 *   AZSDK_SOURCE_DIAGNOSTIC_BUILD_ID = exact GNU build-ID hex
 * Enable only with AZSDK_SOURCE_PROBE=1. No playback commands or source qualification.
 * Candidate calls copy scalar registry records only; this thread never touches native
 * owners, SQLite, database files, proc/sysfs or paths other than its bounded log file.
 */
#define _GNU_SOURCE
#include "../../src/common/aztimestamp.h"
#include "../../src/common/azsources.h"
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <link.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifndef AZSDK_SOURCE_DIAGNOSTIC_ANCHOR
#define AZSDK_SOURCE_DIAGNOSTIC_ANCHOR "azmod_dm_key"
#endif
#if defined(AZSDK_SOURCE_DIAGNOSTIC_DELTA) != defined(AZSDK_SOURCE_DIAGNOSTIC_BUILD_ID)
#error "source probe requires delta and GNU build ID from one matching OWN azmod pair"
#endif
#if !defined(AZSDK_SOURCE_DIAGNOSTIC_DELTA) && !defined(AZSDK_SOURCE_PROBE_TEST)
#error "source probe requires its exact OWN azmod artifact macros"
#endif
#if defined(AZSDK_SOURCE_DIAGNOSTIC_DELTA) && defined(AZSDK_SOURCE_DIAGNOSTIC_BUILD_ID)
/* Stored compiler inputs prevent a manifest-only correction of a misbound probe. */
static const struct __attribute__((packed)) {
    char magic[8];
    uint32_t size, version;
    int64_t delta;
    char build_id[129], anchor[64], target[64];
} source_compiled_binding __attribute__((used, section(".azsdk.binding"), aligned(1))) = {
    "AZSDKPB1", 281, 1, (int64_t) AZSDK_SOURCE_DIAGNOSTIC_DELTA,
    AZSDK_SOURCE_DIAGNOSTIC_BUILD_ID, AZSDK_SOURCE_DIAGNOSTIC_ANCHOR, "azsdk_sources_candidate"
};
_Static_assert(sizeof(source_compiled_binding) == 281, "compiled binding ABI changed");
#endif
_Static_assert(sizeof(uintptr_t) == 8 && sizeof(struct azsdk_source_sample) == 80,
               "source probe requires the current private 64-bit scalar ABI");
typedef int (*source_candidate_fn)(uint16_t, void *, size_t);
struct diagnostic_range {
    uintptr_t begin, end;
    unsigned flags;
};
struct diagnostic_module {
    uintptr_t base, anchor, target;
    uint8_t expected[64];
    size_t expected_size;
    int valid;
};
static int diagnostic_span(uintptr_t base, uint64_t address, uint64_t bytes,
                           uintptr_t *begin, uintptr_t *end) {
    if (address > UINTPTR_MAX - base) return 0;
    uintptr_t start = base + (uintptr_t) address;
    if (bytes > UINTPTR_MAX - start) return 0;
    *begin = start;
    *end = start + (uintptr_t) bytes;
    return 1;
}
static int diagnostic_rx(const struct diagnostic_range *ranges, size_t count, uintptr_t p) {
    for (size_t i = 0; i < count; i++)
        if ((ranges[i].flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X) &&
            p >= ranges[i].begin && p <= ranges[i].end && ranges[i].end - p >= 4) return 1;
    return 0;
}
static int diagnostic_readable(const struct diagnostic_range *ranges, size_t count,
                               uintptr_t begin, uintptr_t end) {
    for (size_t i = 0; i < count; i++)
        if ((ranges[i].flags & PF_R) && begin >= ranges[i].begin && end <= ranges[i].end) return 1;
    return 0;
}
/* Only loader-owned phdrs of the already-identified OWN azmod module are visited.
 * No note address is followed until its entire span is inside a readable LOAD.
 * Limits:128 phdrs,16KiB per NOTE,64KiB total notes,256 note records. */
static int diagnostic_phdr(struct dl_phdr_info *info, size_t size, void *opaque) {
    struct diagnostic_module *m = opaque;
    if (size < offsetof(struct dl_phdr_info, dlpi_phnum) + sizeof info->dlpi_phnum ||
        (uintptr_t) info->dlpi_addr != m->base) return 0;
    if (!info->dlpi_phdr || !info->dlpi_phnum || info->dlpi_phnum > 128) return 1;
    struct diagnostic_range ranges[128];
    size_t range_count = 0;
    for (size_t i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD) continue;
        struct diagnostic_range r;
        if (!diagnostic_span(m->base, p->p_vaddr, p->p_memsz, &r.begin, &r.end)) return 1;
        r.flags = p->p_flags;
        ranges[range_count++] = r;
    }
    if (!diagnostic_rx(ranges, range_count, m->anchor) ||
        !diagnostic_rx(ranges, range_count, m->target)) return 1;
    size_t note_bytes = 0, note_records = 0;
    unsigned build_ids = 0, matched = 0;
    for (size_t i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_NOTE || !p->p_filesz) continue;
        if (p->p_filesz > 16384 || p->p_filesz > 65536 - note_bytes) return 1;
        note_bytes += (size_t) p->p_filesz;
        uintptr_t begin, end;
        if (!diagnostic_span(m->base, p->p_vaddr, p->p_filesz, &begin, &end) ||
            !diagnostic_readable(ranges, range_count, begin, end)) return 1;
        const uint8_t *bytes = (const uint8_t *) begin;
        size_t off = 0, length = (size_t) p->p_filesz;
        while (off < length) {
            if (++note_records > 256 || length - off < sizeof(ElfW(Nhdr))) return 1;
            ElfW(Nhdr) note;
            memcpy(&note, bytes + off, sizeof note);
            off += sizeof note;
            uint64_t name_bytes = ((uint64_t) note.n_namesz + 3u) & ~UINT64_C(3);
            uint64_t desc_bytes = ((uint64_t) note.n_descsz + 3u) & ~UINT64_C(3);
            if (name_bytes > length - off) return 1;
            const uint8_t *name = bytes + off;
            off += (size_t) name_bytes;
            if (desc_bytes > length - off) return 1;
            const uint8_t *desc = bytes + off;
            off += (size_t) desc_bytes;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 && !memcmp(name, "GNU\0", 4)) {
                build_ids++;
                if (note.n_descsz == m->expected_size && !memcmp(desc, m->expected, m->expected_size)) matched++;
            }
        }
    }
    m->valid = build_ids == 1 && matched == 1;
    return 1;
}
static int diagnostic_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int diagnostic_delta(uintptr_t anchor, int64_t delta, uintptr_t *out) {
    if (delta < 0) {
        uint64_t magnitude = (uint64_t) (-(delta + 1)) + 1u;
        if (magnitude > anchor) return 0;
        *out = anchor - (uintptr_t) magnitude;
    } else {
        if ((uint64_t) delta > UINTPTR_MAX - anchor) return 0;
        *out = anchor + (uintptr_t) delta;
    }
    return 1;
}
static source_candidate_fn diagnostic_resolve(void *anchor, const char *expected, int64_t delta) {
    if (!anchor || !expected) return NULL;
    size_t expected_length = strnlen(expected, 129);
    if (!expected_length || expected_length > 128 || expected_length % 2) return NULL;
    struct diagnostic_module m = {0};
    m.expected_size = expected_length / 2;
    for (size_t i = 0; i < m.expected_size; i++) {
        int high = diagnostic_hex(expected[i * 2]), low = diagnostic_hex(expected[i * 2 + 1]);
        if (high < 0 || low < 0) return NULL;
        m.expected[i] = (uint8_t) (high * 16 + low);
    }
    Dl_info module;
    if (!dladdr(anchor, &module) || !module.dli_fname || !module.dli_fbase ||
        !module.dli_sname || strcmp(module.dli_sname, AZSDK_SOURCE_DIAGNOSTIC_ANCHOR) ||
        module.dli_saddr != anchor) return NULL;
    const char *base_name = strrchr(module.dli_fname, '/');
    base_name = base_name ? base_name + 1 : module.dli_fname;
    if (strcmp(base_name, "azmod.so")) return NULL;
    m.base = (uintptr_t) module.dli_fbase;
    m.anchor = (uintptr_t) anchor;
    if (!delta || !diagnostic_delta(m.anchor, delta, &m.target)) return NULL;
#if defined(__aarch64__)
    if (m.target & 3u) return NULL;
#endif
    dl_iterate_phdr(diagnostic_phdr, &m);
    if (!m.valid) return NULL;
    /* Query candidate identity only after the range/build-ID guards prove it is
     * inside the own helper. This never asks dladdr about an EP147 candidate. */
    void *target = (void *) m.target;
    Dl_info candidate;
    if (!dladdr(target, &candidate) || candidate.dli_fbase != module.dli_fbase ||
        !candidate.dli_fname || strcmp(candidate.dli_fname, module.dli_fname)) return NULL;
    source_candidate_fn result;
    _Static_assert(sizeof result == sizeof target, "unsupported private function-pointer representation");
    memcpy(&result, &target, sizeof result);
    return result;
}

#define SOURCE_LOG_LIMIT (64u * 1024u)
#define SOURCE_LOG_RESERVE 512u
#define SOURCE_BATCH 256u
#define SOURCE_HISTORY 64u
#define SOURCE_BUDGET_NS UINT64_C(300000000000)
#define SOURCE_INTERVAL_NS UINT64_C(100000000)
struct source_log {
    FILE *file;
    size_t bytes;
    int stopped;
};
static int source_line(struct source_log *log, const char *format, ...) __attribute__((format(printf, 2, 3)));
static int source_line(struct source_log *log, const char *format, ...) {
    if (!log || !log->file || log->stopped) return 0;
    char line[768];
    va_list arguments;
    va_start(arguments, format);
    int n = vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    if (n < 0 || (size_t) n >= sizeof line ||
        log->bytes > SOURCE_LOG_LIMIT - SOURCE_LOG_RESERVE ||
        (size_t) n > SOURCE_LOG_LIMIT - SOURCE_LOG_RESERVE - log->bytes) {
        if (log) log->stopped = 1;
        return 0;
    }
    if (fwrite(line, 1, (size_t) n, log->file) != (size_t) n) {
        log->stopped = 1;
        return 0;
    }
    log->bytes += (size_t) n;
    return 1;
}
static uint64_t source_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) return 0;
    uint64_t seconds = (uint64_t) t.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t) t.tv_nsec) / UINT64_C(1000000000)) return 0;
    return seconds * UINT64_C(1000000000) + (uint64_t) t.tv_nsec;
}
static int source_sleep_until(uint64_t until) {
    struct timespec deadline = {(time_t)(until / UINT64_C(1000000000)),
                                (long)(until % UINT64_C(1000000000))};
    if (deadline.tv_sec < 0 || (uint64_t)deadline.tv_sec != until / UINT64_C(1000000000)) return 0;
    /* Absolute time preserves the slow cadence under signals. Refuse after four
     * interrupted attempts instead of retrying indefinitely or spinning faster. */
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        int result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        if (!result) return 1;
        if (result != EINTR) return 0;
    }
    return 0;
}
struct source_budget { uint64_t last, deadline; };
static int source_budget_start(struct source_budget *budget) {
    budget->last = source_now();
    budget->deadline = 0;
    if (!budget->last || budget->last > UINT64_MAX - SOURCE_BUDGET_NS) return 0;
    budget->deadline = budget->last + SOURCE_BUDGET_NS;
    return 1;
}
/* -1 is a refused clock, 0 is an elapsed budget, 1 permits more work. */
static int source_budget_read(struct source_budget *budget) {
    uint64_t now = source_now();
    if (!now || compare_timestamps(now,budget->last,1000000000,1000000000,1)==AZ_TIME_BEFORE) return -1;
    budget->last = now;
    return compare_timestamps(now,budget->deadline,1000000000,1000000000,1)==AZ_TIME_BEFORE;
}
/* -2 is a refused cadence, otherwise the same result as budget_read. */
static int source_delay(struct source_budget *budget) {
    int result = source_budget_read(budget);
    if (result <= 0) return result;
    uint64_t until = compare_timestamps(budget->deadline-budget->last,SOURCE_INTERVAL_NS,1000000000,1000000000,1)==AZ_TIME_BEFORE ?
                     budget->deadline : budget->last + SOURCE_INTERVAL_NS;
    if (!source_sleep_until(until)) return -2;
    return source_budget_read(budget);
}
static int source_record_valid(int result, uint16_t media, const struct azsdk_source_sample *s,
                               uint64_t now) {
    if (!s || !media || !now || s->struct_size != sizeof *s || s->native_media_id != media || s->reserved) return 0;
    if (result == AZS_AMBIGUOUS)
        return s->flags == (AZS_HISTORY_INCOMPLETE | AZS_MEDIA_AMBIGUOUS) &&
               !s->owner_epoch && !s->revision && !s->observed_ns && !s->physical_status &&
               !s->device && !s->inode && !s->mount_id && !s->file_size && !s->physical_observed_ns;
    if (result != AZS_OK || (s->flags & ~31u) ||
        (s->flags & (AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE)) !=
            (AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE) ||
        (s->flags & AZS_MEDIA_AMBIGUOUS) || !s->owner_epoch || !s->revision ||
        !s->observed_ns || compare_timestamps(s->observed_ns,now,1000000000,1000000000,1)==AZ_TIME_AFTER || s->physical_status < 0 || s->physical_status > 8) return 0;
    if (s->flags & AZS_PHYSICAL)
        return s->physical_status == AZS_PHYSICAL_OK && s->mount_id && s->inode &&
               compare_timestamps(s->physical_observed_ns,s->observed_ns,1000000000,1000000000,1)>=AZ_TIME_EQUAL && compare_timestamps(s->physical_observed_ns,now,1000000000,1000000000,1)<=AZ_TIME_EQUAL;
    return s->physical_status != AZS_PHYSICAL_OK && !s->device && !s->inode && !s->mount_id &&
           !s->file_size && !s->physical_observed_ns;
}
struct source_history {
    uint16_t media;
    int result;
    struct azsdk_source_sample sample;
};
static int source_changed(struct source_log *log, struct source_history *history,
                          uint16_t media, int result, const struct azsdk_source_sample *sample) {
    struct source_history *slot = NULL, *empty = NULL;
    for (unsigned i = 0; i < SOURCE_HISTORY; ++i) {
        if (history[i].media == media) {
            slot = &history[i];
            break;
        }
        if (!history[i].media && !empty) empty = &history[i];
    }
    if (!slot && result == AZS_UNKNOWN) return 1;
    if (!slot) slot = empty;
    if (!slot) return 0; /* Explicit scan history-limit status; never evict and spam. */
    if (slot->media && slot->result == result &&
        (result == AZS_UNKNOWN || !memcmp(&slot->sample, sample, sizeof *sample))) return 1;
    int written;
    if (result == AZS_UNKNOWN) {
        written = source_line(log, "source status=unknown media=0x%04x previous_epoch=%" PRIu64 " history_incomplete=1 loaded_source_valid=0\n", media, slot->sample.owner_epoch);
    } else {
        written = source_line(log, "source status=%s media=0x%04x epoch=%" PRIu64 " revision=%" PRIu64 " received_ns=%" PRIu64 " flags=0x%x physical_status=%d dev=%" PRIu64 " ino=%" PRIu64 " mnt_id=%" PRIu64 " size=%" PRIu64 " physical_received_ns=%" PRIu64 " history_incomplete=1 loaded_source_valid=0\n",
                              result == AZS_OK ? "current" : "ambiguous", media, sample->owner_epoch,
                              sample->revision, sample->observed_ns, sample->flags, sample->physical_status,
                              sample->device, sample->inode, sample->mount_id, sample->file_size,
                              sample->physical_observed_ns);
    }
    if (written) {
        slot->media = media;
        slot->result = result;
        if (sample) slot->sample = *sample;
    }
    return written;
}
/* Keep rejection evidence finite and scalar-only. Counters/failure semantics stay
 * unchanged; malformed native records must not disappear behind an invalid= tally. */
static void source_invalid_record(struct source_log *log, unsigned *reported, uint16_t media,
                                  int result, const struct azsdk_source_sample *s,
                                  uint64_t now, int error_output_changed) {
    if (*reported >= 8) return;
    ++*reported;
    (void)source_line(log, "source_invalid media=0x%04x result=%d error_output_changed=%d now_ns=%" PRIu64
        " struct_size=%u flags=0x%x epoch=%" PRIu64 " revision=%" PRIu64 " received_ns=%" PRIu64
        " sample_media=0x%04x reserved=%u physical_status=%d dev=%" PRIu64 " ino=%" PRIu64
        " mnt_id=%" PRIu64 " size=%" PRIu64 " physical_received_ns=%" PRIu64 "\n",
        media, result, error_output_changed, now, s->struct_size, s->flags,
        s->owner_epoch, s->revision, s->observed_ns, s->native_media_id, s->reserved,
        s->physical_status, s->device, s->inode, s->mount_id, s->file_size, s->physical_observed_ns);
}
static void source_scan(source_candidate_fn candidate, struct source_log *log,
                         struct source_budget *budget) {
    FILE *file = log->file;
    struct source_history history[SOURCE_HISTORY] = {{0}};
    uint32_t media = 1;
    unsigned passes = 0, current = 0, ambiguous = 0, unknown = 0, busy = 0, invalid = 0, history_limit = 0;
    unsigned ticks = 0, invalid_reported = 0;
    const char *status = "time_limit";
    int running = budget->deadline != 0;
    if (!running) status = "clock_refused";
    while (running && !log->stopped) {
        ++ticks;
        for (unsigned batch = 0; batch < SOURCE_BATCH && !log->stopped; ++batch) {
            int time_result = source_budget_read(budget);
            if (time_result <= 0) {
                if (time_result < 0) status = "clock_refused";
                running = 0;
                break;
            }
            struct azsdk_source_sample sample, before;
            memset(&sample, 0xa5, sizeof sample);
            before = sample;
            int result = candidate((uint16_t) media, &sample, sizeof sample);
            /* Include query cost, not only intentional sleeps, in the budget.
             * The own scalar query is never interrupted halfway through. */
            time_result = source_budget_read(budget);
            if (time_result <= 0) {
                if (time_result < 0) status = "clock_refused";
                running = 0;
            }
            if (result == AZS_OK || result == AZS_AMBIGUOUS) {
                if (time_result < 0 || !source_record_valid(result, (uint16_t) media, &sample, budget->last)) {
                    ++invalid;
                    source_invalid_record(log, &invalid_reported, (uint16_t)media, result,
                                          &sample, budget->last, 0);
                } else {
                    if (result == AZS_OK) ++current;
                    else ++ambiguous;
                    if (!source_changed(log, history, (uint16_t) media, result, &sample) && !log->stopped) ++history_limit;
                }
            } else if (memcmp(&sample, &before, sizeof sample)) {
                ++invalid;
                source_invalid_record(log, &invalid_reported, (uint16_t)media, result,
                                      &sample, budget->last, 1);
            } else if (result == AZS_UNKNOWN) {
                ++unknown;
                if (!source_changed(log, history, (uint16_t) media, result, NULL) && !log->stopped) ++history_limit;
            } else if (result == AZS_BUSY) ++busy;
            else {
                ++invalid;
                source_invalid_record(log, &invalid_reported, (uint16_t)media, result,
                                      &sample, budget->last, 0);
            }
            if (++media > UINT16_MAX) {
                ++passes;
                source_line(log, "scan pass=%u queried=65535 current=%u ambiguous=%u unknown=%u busy=%u invalid=%u history_limit=%u\n",
                            passes, current, ambiguous, unknown, busy, invalid, history_limit);
                current = ambiguous = unknown = busy = invalid = history_limit = 0;
                media = 1;
            }
            if (!running) break;
        }
        fflush(file);
        if (running && !log->stopped) {
            int time_result = source_delay(budget);
            if (time_result <= 0) {
                if (time_result < 0) status = time_result == -2 ? "cadence_refused" : "clock_refused";
                running = 0;
            }
        }
    }
    /* Footer has a reserved 512-byte budget beyond the regular-line cap. */
    char footer[256];
    int n = snprintf(footer, sizeof footer, "complete status=%s ticks=%u passes=%u next_media=%u current=%u ambiguous=%u unknown=%u busy=%u invalid=%u history_limit=%u\n",
                     log->stopped ? "log_limit_or_io" : status,
                     ticks, passes, media,
                     current, ambiguous, unknown, busy, invalid, history_limit);
    if (n > 0 && (size_t) n < sizeof footer && log->bytes <= SOURCE_LOG_LIMIT &&
        (size_t) n <= SOURCE_LOG_LIMIT - log->bytes) fwrite(footer, 1, (size_t) n, file);
}
#ifndef AZSDK_SOURCE_PROBE_TEST
static void *source_observe(void *unused) {
    (void) unused;
    struct source_budget budget;
    int clock_ready = source_budget_start(&budget);
    int nice_result = setpriority(PRIO_PROCESS, 0, 10);
    char path[128];
    snprintf(path, sizeof path, "/tmp/az-sdk-source-probe.%ld.log", (long) getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return NULL;
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        return NULL;
    }
    struct source_log log = {file, 0, 0};
    source_line(&log, "pid=%ld configured=1 nice_requested=10 nice_result=%d batch=%u interval_ms=100 budget_ms=300000 clock=monotonic cap=%u history_incomplete=1 loaded_source_valid=0\n",
                (long) getpid(), nice_result, SOURCE_BATCH, SOURCE_LOG_LIMIT);
    if (nice_result) {
        source_line(&log, "complete status=nice_refused\n");
        fclose(file);
        return NULL;
    }
    if (!clock_ready) {
        source_line(&log, "complete status=clock_refused\n");
        fclose(file);
        return NULL;
    }
    source_candidate_fn candidate = NULL;
    const char *resolver_status = "resolver_refused";
    for (unsigned attempt = 0; attempt < 30 && !candidate; ++attempt) {
        int time_result = source_budget_read(&budget);
        if (time_result <= 0) {
            resolver_status = time_result < 0 ? "clock_refused" : "time_limit";
            break;
        }
        void *anchor = dlsym(RTLD_DEFAULT, AZSDK_SOURCE_DIAGNOSTIC_ANCHOR);
        candidate = diagnostic_resolve(anchor, AZSDK_SOURCE_DIAGNOSTIC_BUILD_ID,
                                       (int64_t) AZSDK_SOURCE_DIAGNOSTIC_DELTA);
        if (!candidate && (time_result = source_delay(&budget)) <= 0) {
            resolver_status = time_result == -2 ? "cadence_refused" :
                              time_result < 0 ? "clock_refused" : "time_limit";
            break;
        }
    }
    source_line(&log, "resolver_available=%d module=azmod.so build_id=%s\n", candidate != NULL,
                AZSDK_SOURCE_DIAGNOSTIC_BUILD_ID);
    if (!candidate) {
        source_line(&log, "complete status=%s\n", resolver_status);
        fclose(file);
        return NULL;
    }
    source_scan(candidate, &log, &budget);
    fclose(file);
    return NULL;
}
__attribute__((constructor)) static void source_probe_start(void) {
    const char *opt = getenv("AZSDK_SOURCE_PROBE");
    if (!opt || strcmp(opt, "1")) return;
    pthread_t thread;
    if (!pthread_create(&thread, NULL, source_observe, NULL)) pthread_detach(thread);
}
#endif
