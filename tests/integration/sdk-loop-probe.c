/* SPDX-License-Identifier: GPL-3.0-only */
/* Controlled emulator/hardware preload probe for the azstem/azmod boundary. No EP147 entry points,
 * hooks, pointer inspection, playback commands or community approval records are used.
 * Explicit opt-in: AZSDK_LOOP_PROBE=1. Logs one bounded file per native process. */
#define _GNU_SOURCE
#include "../../src/common/aztimestamp.h"
#include "../../src/common/azdeckinfo.h"
#include "../../src/common/azloadexport.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifndef AZSDK_LOOP_PROBE_TICKS
#define AZSDK_LOOP_PROBE_TICKS 6000
#endif
_Static_assert(AZSDK_LOOP_PROBE_TICKS >= 1 && AZSDK_LOOP_PROBE_TICKS <= 36000,
               "probe duration must be between 1 and 36000 ticks");
#ifndef AZSDK_LOOP_PROBE_OPERATOR
#define AZSDK_LOOP_PROBE_OPERATOR 0
#endif
_Static_assert(AZSDK_LOOP_PROBE_OPERATOR == 0 || AZSDK_LOOP_PROBE_OPERATOR == 1,
               "operator probe flag must be zero or one");
static unsigned long long now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (unsigned long long) t.tv_sec * 1000000000ULL + (unsigned long long) t.tv_nsec;
}
/* Optional OWN-helper diagnostic, never an EP147 address or entry point.
 * Build both macros from one matching pair of freshly built artifacts:
 *   nm -an azstem.so.full: signed(hidden azsdk_loop_read - exported azstem_loop_observation_v2)
 *   readelf -n azstem.so: GNU build-ID hex (verify the stripped/full pair match)
 * Compile -DAZSDK_LOOP_DIAGNOSTIC_DELTA=<signed decimal>LL
 *         -DAZSDK_LOOP_DIAGNOSTIC_BUILD_ID='"<hex>"'. Parent also SHA-verifies upload.
 * No macros: no private call or new log field. One macro alone: compile error.
 * diagnostic_reason=-1 means unavailable (including a concurrent newer publication).
 * 0=valid, 1=inactive, 2..13=the helper's scalar first-rejection code.
 * The 96-byte private ABI additionally logs bounded raw cache-comparison inputs
 * for reason 12 / valid bounds; this probe does not change decoder tolerance. */
#if defined(AZSDK_LOOP_DIAGNOSTIC_DELTA) != defined(AZSDK_LOOP_DIAGNOSTIC_BUILD_ID)
#error "loop diagnostic requires both delta and build ID from the same own helper"
#endif
#if defined(AZSDK_LOOP_DIAGNOSTIC_DELTA) && defined(AZSDK_LOOP_DIAGNOSTIC_BUILD_ID)
#include "../../src/common/azloopobserver.h"
#include <elf.h>
#include <link.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Host package validation compares these actual compiler inputs with exact
 * full-library symbols before this preload is permitted into an emulator. */
static const struct __attribute__((packed)) {
    char magic[8];
    uint32_t size, version;
    int64_t delta;
    char build_id[129], anchor[64], target[64];
} loop_compiled_binding __attribute__((used, section(".azsdk.binding"), aligned(1))) = {
    "AZSDKPB1", 281, 1, (int64_t) AZSDK_LOOP_DIAGNOSTIC_DELTA,
    AZSDK_LOOP_DIAGNOSTIC_BUILD_ID, "azstem_loop_observation_v2", "azsdk_loop_read"
};
_Static_assert(sizeof(loop_compiled_binding) == 281, "compiled binding ABI changed");

_Static_assert(sizeof(struct azsdk_loop_sample) == 96, "private loop sample ABI changed");
typedef int (*diagnostic_read_fn)(int, struct azsdk_loop_sample *);
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
            p >= ranges[i].begin && p < ranges[i].end) return 1;
    return 0;
}
static int diagnostic_readable(const struct diagnostic_range *ranges, size_t count,
                               uintptr_t begin, uintptr_t end) {
    for (size_t i = 0; i < count; i++)
        if ((ranges[i].flags & PF_R) && begin >= ranges[i].begin && end <= ranges[i].end) return 1;
    return 0;
}
/* Only loader-owned phdrs of the already-identified azstem module are visited.
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
static diagnostic_read_fn diagnostic_resolve(azstem_loop_observation_v2_fn read) {
    static const char expected[] = AZSDK_LOOP_DIAGNOSTIC_BUILD_ID;
    if (!read || sizeof expected <= 1 || (sizeof expected - 1) % 2 || sizeof expected - 1 > 128) return NULL;
    struct diagnostic_module m = {0};
    m.expected_size = (sizeof expected - 1) / 2;
    for (size_t i = 0; i < m.expected_size; i++) {
        int high = diagnostic_hex(expected[i * 2]), low = diagnostic_hex(expected[i * 2 + 1]);
        if (high < 0 || low < 0) return NULL;
        m.expected[i] = (uint8_t) (high * 16 + low);
    }
    void *anchor = NULL;
    _Static_assert(sizeof anchor == sizeof read, "unsupported function-pointer representation");
    memcpy(&anchor, &read, sizeof anchor);
    Dl_info module;
    if (!dladdr(anchor, &module) || !module.dli_fname || !module.dli_fbase ||
        !module.dli_sname || strcmp(module.dli_sname, "azstem_loop_observation_v2") ||
        module.dli_saddr != anchor) return NULL;
    const char *base_name = strrchr(module.dli_fname, '/');
    base_name = base_name ? base_name + 1 : module.dli_fname;
    if (strcmp(base_name, "azstem.so")) return NULL;
    m.base = (uintptr_t) module.dli_fbase;
    m.anchor = (uintptr_t) anchor;
    int64_t delta = (int64_t) (AZSDK_LOOP_DIAGNOSTIC_DELTA);
    if (delta < 0) {
        uint64_t magnitude = (uint64_t) (-(delta + 1)) + 1u;
        if (magnitude > m.anchor) return NULL;
        m.target = m.anchor - (uintptr_t) magnitude;
    } else {
        if ((uint64_t) delta > UINTPTR_MAX - m.anchor) return NULL;
        m.target = m.anchor + (uintptr_t) delta;
    }
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
    diagnostic_read_fn result;
    _Static_assert(sizeof result == sizeof target, "unsupported private function-pointer representation");
    memcpy(&result, &target, sizeof result);
    return result;
}
static int diagnostic_reason(diagnostic_read_fn read, int deck,
                             const struct azstem_loop_observation_v2 *v,
                             struct azsdk_loop_sample *captured) {
    if (captured) memset(captured, 0, sizeof *captured);
    if (!read || !v || !captured) return -1;
    struct azsdk_loop_sample s = {0};
    if (!read(deck, &s)) return -1;
    if (s.unit_generation != v->unit_generation || s.revision != v->revision ||
        compare_timestamps(s.observed_ns, v->observed_ns, 1000000000, 1000000000, 1) != AZ_TIME_EQUAL || s.engaged != v->engaged || s.bounds_valid != v->bounds_valid ||
        !s.unit_generation || !s.revision || !s.observed_ns || compare_timestamps(s.observed_ns, now_ns(), 1000000000, 1000000000, 1) == AZ_TIME_AFTER ||
        (s.engaged != 0 && s.engaged != 1) || (s.bounds_valid != 0 && s.bounds_valid != 1) ||
        s.reserved < 0 || s.reserved > 13 || s.reserved2 ||
        ((s.reserved == 0) != (s.bounds_valid != 0)) || ((s.reserved == 1) != (s.engaged == 0)) ||
        !isfinite(s.in_s) || !isfinite(s.out_s) || compare_timestamps(s.in_s, v->in_s, 1, s.check_rate ? s.check_rate : 1000000000, 1) != AZ_TIME_EQUAL || compare_timestamps(s.out_s, v->out_s, 1, s.check_rate ? s.check_rate : 1000000000, 1) != AZ_TIME_EQUAL ||
        (s.bounds_valid ? (!s.engaged || compare_timestamps(s.in_s, 0., 1, s.check_rate ? s.check_rate : 1000000000, 1) == AZ_TIME_BEFORE || compare_timestamps(s.out_s, s.in_s, 1, s.check_rate ? s.check_rate : 1000000000, 1) <= AZ_TIME_EQUAL) : (compare_timestamps(s.in_s, 0., 1, 1000000000, 1) != AZ_TIME_EQUAL || compare_timestamps(s.out_s, 0., 1, 1000000000, 1) != AZ_TIME_EQUAL))) return -1;
    const uint64_t exact_integer = UINT64_C(9007199254740992);
    if (s.check_reserved || s.check_in_samples > exact_integer || s.check_out_samples > exact_integer ||
        s.check_in_cached_us > exact_integer || s.check_out_cached_us > exact_integer) return -1;
    int inputs_present = s.check_rate || s.check_in_samples || s.check_out_samples ||
                         s.check_in_cached_us || s.check_out_cached_us;
    if (inputs_present) {
        if (!s.check_rate || s.check_rate >= UINT32_C(0x7fffffff)) return -1;
    } else if (s.reserved == 12 || s.bounds_valid) return -1;
    /* No cache-equality/tolerance or new native predicate is evaluated here.
     * Earlier rejections can carry the all-zero block; reason13 can retain inputs. */
    *captured = s;
    return s.reserved;
}
static void diagnostic_log(FILE *f, diagnostic_read_fn read, int deck,
                           const struct azstem_loop_observation_v2 *value) {
    struct azsdk_loop_sample captured = {0};
    int reason = diagnostic_reason(read, deck, value, &captured);
    fprintf(f, " diagnostic_reason=%d", reason);
    if (reason >= 0 && (reason == 12 || captured.bounds_valid))
        fprintf(f, " check_in_samples=%llu check_out_samples=%llu check_in_cached_us=%llu check_out_cached_us=%llu check_rate=%u",
                (unsigned long long) captured.check_in_samples, (unsigned long long) captured.check_out_samples,
                (unsigned long long) captured.check_in_cached_us, (unsigned long long) captured.check_out_cached_us,
                (unsigned) captured.check_rate);
}
#endif

/* Only copied, versioned own-helper observations. Preserve each stream across a
 * contended read that omits another component; a unit change resets that history. */
static int live_probe_log(FILE *f, int deck, unsigned submitted,
                          const struct azstem_live_observation_v1 *value,
                          struct azsdk_live_sample *previous, unsigned long long now) {
    if (!f || !value || !previous || deck < 0 || deck >= 4 || (submitted & ~15u)) return -1;
    /* A rejected input can contain only the size prefix. Volatile keeps GCC
     * from widening adjacent size/version checks into an eight-byte read. */
    const uint32_t prefix_size = *(const volatile uint32_t *) (const void *) value;
    if (prefix_size < sizeof *value || value->version != 1) return -1;
    const struct azsdk_live_sample *s = &value->sample;
    if (!s->unit_generation || !s->observed_fields || (s->observed_fields & ~7u) ||
        (s->valid_fields & ~s->observed_fields) || s->reserved) return -1;
    for (unsigned stream = 0; stream < 3; stream++) {
        unsigned bit = 1u << stream;
        if (!(s->observed_fields & bit)) continue;
        if (!s->revisions[stream] || !s->observed_ns[stream] || compare_timestamps(s->observed_ns[stream], now, 1000000000, 1000000000, 1) == AZ_TIME_AFTER) return -1;
        int a = stream == 0 ? s->mode_is_2 : stream == 1 ? s->quantize
                                                         : s->key_lock;
        int b = stream == 0 ? s->play_pause : a, c = stream == 0 ? s->play_aux : a;
        if (s->valid_fields & bit) {
            if (a < 0 || a > 1 || b < 0 || b > 1 || c < 0 || c > 1) return -1;
        } else if (a != -1 || b != -1 || c != -1) return -1;
    }
    if (s->unit_generation != previous->unit_generation) {
        memset(previous, 0, sizeof *previous);
        previous->unit_generation = s->unit_generation;
    }
    int lines = 0;
    for (unsigned stream = 0; stream < 3; stream++) {
        unsigned bit = 1u << stream;
        if (!(s->observed_fields & bit) || s->revisions[stream] == previous->revisions[stream]) continue;
        char line[512];
        int bytes = snprintf(line, sizeof line,
                             "live_deck=%d unit=%llu stream=%s revision=%llu received_ns=%llu age_ns=%llu valid=%u mode_is_2=%d play_pause=%d play_aux=%d quantize=%d key_lock=%d mask=%u\n",
                             deck, (unsigned long long) s->unit_generation,
                             stream == 0 ? "play" : stream == 1 ? "quantize"
                                                                : "key_lock",
                             (unsigned long long) s->revisions[stream], (unsigned long long) s->observed_ns[stream],
                             now - s->observed_ns[stream], (unsigned) ((s->valid_fields & bit) != 0),
                             stream == 0 ? s->mode_is_2 : -1, stream == 0 ? s->play_pause : -1,
                             stream == 0 ? s->play_aux : -1, stream == 1 ? s->quantize : -1,
                             stream == 2 ? s->key_lock : -1, submitted);
        long position = ftell(f);
        if (bytes < 0 || (size_t) bytes >= sizeof line || position < 0) return -1;
        if (position > 256L * 1024 - 128 - bytes) return lines;
        if (fwrite(line, 1, (size_t) bytes, f) != (size_t) bytes) return -1;
        previous->revisions[stream] = s->revisions[stream];
        previous->observed_ns[stream] = s->observed_ns[stream];
        lines++;
    }
    return lines;
}

/* A missing receipt can mean in-flight work, invalidation, disabled capture or
 * contention. The private ABI has no definitive unloaded result. Keep the last
 * validated scalar receipt across unavailability so regressions cannot hide. */
static int load_probe_log(FILE *f, int deck, int result,
                          const struct azstem_load_observation_v1 *value,
                          struct azsdk_load_sample *previous, int *availability,
                          unsigned long long now) {
    if (!f || !previous || !availability || deck < 0 || deck >= 4 || !now ||
        (result != 0 && result != -1)) return -1;
    const struct azsdk_load_sample *sample = NULL;
    if (result == 0) {
        if (!value) return -1;
        const uint32_t prefix_size = *(const volatile uint32_t *) (const void *) value;
        if (prefix_size != sizeof *value || value->version != 1) return -1;
        sample = &value->sample;
        if (!sample->unit_generation || !sample->load_generation || !sample->invalidation_epoch ||
            !sample->observed_ns || compare_timestamps(sample->observed_ns, now, 1000000000, 1000000000, 1) == AZ_TIME_AFTER || sample->reserved) return -1;
        if (previous->unit_generation) {
            if (sample->unit_generation < previous->unit_generation ||
                sample->load_generation < previous->load_generation ||
                sample->invalidation_epoch < previous->invalidation_epoch ||
                compare_timestamps(sample->observed_ns, previous->observed_ns, 1000000000, 1000000000, 1) == AZ_TIME_BEFORE) return -1;
            if (sample->load_generation == previous->load_generation &&
                memcmp(sample, previous, sizeof *sample)) return -1;
            /* Each new committed receipt followed a fresh native attempt. Native
             * ID0 is valid for the first receipt; IDs are not treated as loaded bits. */
            if (sample->load_generation > previous->load_generation &&
                (sample->invalidation_epoch <= previous->invalidation_epoch ||
                 sample->native_request_id <= previous->native_request_id)) return -1;
        }
    }
    if (*availability == result && (!sample || !memcmp(sample, previous, sizeof *sample))) return 0;
    char line[512];
    int bytes;
    if (sample) {
        bytes = snprintf(line, sizeof line,
                         "load_deck=%d availability=receipt unit=%llu load_generation=%llu invalidation_epoch=%llu native_request_id=%u received_ns=%llu age_ns=%llu\n",
                         deck, (unsigned long long) sample->unit_generation,
                         (unsigned long long) sample->load_generation, (unsigned long long) sample->invalidation_epoch,
                         (unsigned) sample->native_request_id, (unsigned long long) sample->observed_ns,
                         now - sample->observed_ns);
    } else {
        bytes = snprintf(line, sizeof line,
                         "load_deck=%d availability=unavailable read_rc=-1 loaded=unknown last_unit=%llu last_load_generation=%llu last_epoch=%llu last_received_ns=%llu\n",
                         deck, (unsigned long long) previous->unit_generation,
                         (unsigned long long) previous->load_generation, (unsigned long long) previous->invalidation_epoch,
                         (unsigned long long) previous->observed_ns);
    }
    long position = ftell(f);
    if (bytes < 0 || (size_t) bytes >= sizeof line || position < 0) return -1;
    if (position > 256L * 1024 - 128 - bytes) return 0;
    if (fwrite(line, 1, (size_t) bytes, f) != (size_t) bytes) return -1;
    if (sample) *previous = *sample;
    *availability = result;
    return 1;
}

static void *observe(void *unused) {
    (void) unused;
    (void) setpriority(PRIO_PROCESS, 0, 10);
    char path[128];
    snprintf(path, sizeof path, "/tmp/az-sdk-loop-probe.%ld.log", (long) getpid());
    FILE *f = fopen(path, "we");
    if (!f) return NULL;
    char stop_path[128];
    snprintf(stop_path, sizeof stop_path, "/tmp/az-sdk-loop-probe.%ld.%llu.stop", (long) getpid(), now_ns());
    if (AZSDK_LOOP_PROBE_OPERATOR) fprintf(f, "operator_mode=1 stop_file=%s\n", stop_path);
    azstem_loop_observation_v2_fn read;
    azstem_loop_observer_mask_v1_fn mask;
    *(void **) &read = dlsym(RTLD_DEFAULT, "azstem_loop_observation_v2");
    *(void **) &mask = dlsym(RTLD_DEFAULT, "azstem_loop_observer_mask_v1");
    fprintf(f, "pid=%ld exports=%d,%d\n", (long) getpid(), read != NULL, mask != NULL);
    azstem_live_observation_v1_fn live_read;
    azstem_live_observer_mask_v1_fn live_mask;
    *(void **) &live_read = dlsym(RTLD_DEFAULT, "azstem_live_observation_v1");
    *(void **) &live_mask = dlsym(RTLD_DEFAULT, "azstem_live_observer_mask_v1");
    fprintf(f, "live_exports=%d,%d\n", live_read != NULL, live_mask != NULL);
    azstem_load_observation_v1_fn load_read;
    *(void **) &load_read = dlsym(RTLD_DEFAULT, "azstem_load_observation_v1");
    fprintf(f, "private_load_export=%d\n", load_read != NULL);
#if defined(AZSDK_LOOP_DIAGNOSTIC_DELTA) && defined(AZSDK_LOOP_DIAGNOSTIC_BUILD_ID)
    diagnostic_read_fn diagnostic = diagnostic_resolve(read);
    fprintf(f, "diagnostic_available=%d\n", diagnostic != NULL);
#endif
    struct azstem_loop_observation_v2 previous[4] = {{0}};
    struct azsdk_live_sample live_previous[4] = {{0}};
    struct azsdk_load_sample load_previous[4] = {{0}};
    int load_availability[4] = {-2, -2, -2, -2};
    unsigned seen = 0, errors = 0, submitted = 0, last_mask = ~0u;
    unsigned live_seen = 0, live_submitted = 0, live_last_mask = ~0u;
    unsigned load_seen = 0;
    for (unsigned long long tick = 0; AZSDK_LOOP_PROBE_OPERATOR || tick < AZSDK_LOOP_PROBE_TICKS; tick++) {
        /* Operator replies can arrive much later than preparation. This opt-in
         * mode waits for an explicit stop, the existing byte cap, or process exit. */
        if (AZSDK_LOOP_PROBE_OPERATOR && !(tick % 10u) && access(stop_path, F_OK) == 0) break;
        /* Reserve four loop/diagnostic records, twelve <=511-byte live rows, four
         * <=511-byte private load rows, masks and footer under the 256KiB cap. */
        long position = ftell(f);
        if (position < 0 || position > 256L * 1024 - 16384) break;
        submitted = mask ? mask() : 0;
        if (submitted != last_mask) {
            fprintf(f, "registration_mask=%u tick=%llu\n", submitted, tick);
            last_mask = submitted;
        }
        live_submitted = live_mask ? live_mask() : 0;
        if (live_submitted != live_last_mask) {
            fprintf(f, "live_registration_mask=%u tick=%llu\n", live_submitted, tick);
            live_last_mask = live_submitted;
        }
        for (int d = 0; d < 4; d++) {
            struct azstem_loop_observation_v2 value = {sizeof value, 2, 0, 0, 0, -1, 0, 0, 0};
            if (!read || read(d, &value)) continue;
            unsigned long long now = now_ns();
            if (value.version != 2 || !value.unit_generation || !value.revision ||
                !value.observed_ns || compare_timestamps(value.observed_ns, now, 1000000000, 1000000000, 1) == AZ_TIME_AFTER || value.engaged < 0 || value.engaged > 1 ||
                (value.bounds_valid && (!value.engaged || compare_timestamps(value.in_s, 0., 1, 1000000000, 1) == AZ_TIME_BEFORE || compare_timestamps(value.out_s, value.in_s, 1, 1000000000, 1) <= AZ_TIME_EQUAL))) {
                errors++;
                continue;
            }
            seen |= 1u << d;
            if (value.unit_generation != previous[d].unit_generation || value.revision != previous[d].revision) {
                fprintf(f, "deck=%d unit=%llu revision=%llu received_ns=%llu engaged=%d bounds=%d in_s=%.9f out_s=%.9f mask=%u", d,
                        value.unit_generation, value.revision, value.observed_ns, value.engaged, value.bounds_valid, value.in_s, value.out_s, submitted);
#if defined(AZSDK_LOOP_DIAGNOSTIC_DELTA) && defined(AZSDK_LOOP_DIAGNOSTIC_BUILD_ID)
                diagnostic_log(f, diagnostic, d, &value);
#endif
                fputc('\n', f);
                previous[d] = value;
            }
        }
        for (int d = 0; d < 4; d++) {
            struct azstem_live_observation_v1 value = {sizeof value, 1, {0}};
            if (!live_read || live_read(d, &value)) continue;
            if (live_probe_log(f, d, live_submitted, &value, &live_previous[d], now_ns()) < 0) errors++;
            else live_seen |= 1u << d;
        }
        for (int d = 0; d < 4; d++) {
            struct azstem_load_observation_v1 value = {sizeof value, 1, {0}};
            int result = load_read ? load_read(d, &value) : -1;
            if (load_probe_log(f, d, result, &value, &load_previous[d], &load_availability[d], now_ns()) < 0) errors++;
            else if (result == 0) load_seen |= 1u << d;
        }
        fflush(f);
        if (ftell(f) > 256L * 1024) break;
        struct timespec wait = {0, 100000000};
        nanosleep(&wait, NULL);
    }
    for (int d = 0; d < 4; d++)
        for (int other = 0; other < d; other++)
            if ((seen & (1u << d)) && (seen & (1u << other)) &&
                previous[d].unit_generation == previous[other].unit_generation) errors++;
    for (int d = 0; d < 4; d++)
        for (int other = 0; other < d; other++)
            if ((live_seen & (1u << d)) && (live_seen & (1u << other)) &&
                live_previous[d].unit_generation == live_previous[other].unit_generation) errors++;
    unsigned load_available = 0;
    for (int d = 0; d < 4; d++)
        if (load_availability[d] == 0) load_available |= 1u << d;
    fprintf(f, "complete seen=%u submitted=%u errors=%u live_seen=%u live_submitted=%u load_seen=%u load_available=%u\n", seen, submitted, errors, live_seen, live_submitted, load_seen, load_available);
    fclose(f);
    return NULL;
}
__attribute__((constructor)) static void start(void) {
    const char *on = getenv("AZSDK_LOOP_PROBE");
    if (!on || strcmp(on, "1")) return;
    char name[32] = {0};
    FILE *f = fopen("/proc/self/comm", "re");
    if (!f) return;
    if (!fgets(name, sizeof name, f)) name[0] = 0;
    fclose(f);
    name[strcspn(name, "\n")] = 0;
    if (strcmp(name, "EP147")) return;
    pthread_t worker;
    if (!pthread_create(&worker, NULL, observe, NULL)) pthread_detach(worker);
}
