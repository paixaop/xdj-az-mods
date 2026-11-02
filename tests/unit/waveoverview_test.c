/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual production overview scaler/hook, with an independent native-layout consumer. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/common/azoverviewlease.h"
#include OVERVIEW_CONSTANTS_INC
#define AZMOD_STEM_DECKS 4
static _Atomic unsigned checks;
#define CHECK(test)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(test)) {                                                  \
            fprintf(stderr, "FAIL overview:%d: %s\n", __LINE__, #test); \
            exit(1);                                                    \
        }                                                               \
    } while (0)
_Static_assert(sizeof(long) == sizeof(void *), "native hook fixture requires LP64");
static struct {
    long self, content;
    int cnt, deck;
    const unsigned char *src;
    int scnt;
    unsigned gen;
    int color;
    unsigned char *snap, *scaled;
} g_ovi[OVINST_MAX];
static int g_ovi_n, g_ovtile_n, g_ovprobe = -1, g_wavemute, g_wavecolor;
static int g_att_have[AZMOD_STEM_DECKS], g_att_ident[AZMOD_STEM_DECKS];
static unsigned g_att_gen[AZMOD_STEM_DECKS];
static unsigned short g_att[AZMOD_STEM_DECKS][ATT_N];
static unsigned g_col[AZMOD_STEM_DECKS][ATT_N];
static long g_ov_calls, g_ov_applied, g_ov_acquired;
static int g_ov_bind_status;
static int selected_deck;
static void (*g_orig_ov)(long);
static pthread_t fixture_thread;
static int on_msg_thread(void) { return pthread_equal(pthread_self(), fixture_thread); }
static int missing_helper, invalid_helper, rejected_helper;
static int helper_query(uint32_t version, struct azov_lease_api *out, size_t size) {
    if (rejected_helper) return 0;
    int result = azstem_overview_lease_v1(version, out, size);
    if (result && invalid_helper) out->abi_version++;
    return result;
}
static void wwb_log(const char *format, ...) { (void) format; }
static int ov_deck_via_tile(long self) {
    (void) self;
    return selected_deck;
}
static void *overview_dlsym(void *handle, const char *name) {
    (void) handle;
    CHECK(!strcmp(name, "azstem_overview_lease_v1"));
    return missing_helper ? NULL : (void *) helper_query;
}
#define RTLD_DEFAULT NULL
#define dlsym overview_dlsym
#include OVERVIEW_FUNCTIONS_INC
#undef dlsym
#if defined(__linux__) && defined(__aarch64__)
/* Test caller owns its Content for the complete call. Unwind information is required here
 * too: the actual production entry shim is traversed by the throwing C++ fixtures. */
__asm__(".text\n.align 2\n.globl overview_test_native_call\n"
        ".type overview_test_native_call,%function\noverview_test_native_call:\n.cfi_startproc\n"
        "stp x29,x30,[sp,#-32]!\n.cfi_def_cfa_offset 32\n.cfi_offset 29,-32\n.cfi_offset 30,-24\n"
        "stp x24,x25,[sp,#16]\n.cfi_offset 24,-16\n.cfi_offset 25,-8\nmov x29,sp\n"
        "mov x24,x1\nbl azm_ov_entry\n.globl overview_test_native_return\noverview_test_native_return:\n"
        "ldp x24,x25,[sp,#16]\nldp x29,x30,[sp],#32\n.cfi_def_cfa_offset 0\nret\n"
        ".cfi_endproc\n.size overview_test_native_call,.-overview_test_native_call\n");
extern void overview_test_native_call(long, uintptr_t);
extern char overview_test_native_return[];
static int expect_native_capture;
#endif

struct azov_entry {
    uintptr_t content;
    void *cookie;
};
static pthread_mutex_t owner_mutex = PTHREAD_MUTEX_INITIALIZER;
static int trusted = 1, profile_ready = 1, acquire_fail;
static int force_contention;
static unsigned retain_attempts;
static unsigned destroys;
static unsigned char *destroy_expected_points;
int azov_test_trusted(void) { return trusted; }
int azov_test_profile_ready(void) { return profile_ready; }
int azov_test_fail_cas(void) {
    retain_attempts++;
    /* A missing production attempt bound must fail an assertion, never hang this test. */
    return force_contention && retain_attempts <= 64;
}
void azov_test_before_publish(void) {
    struct azov_lease_api out, sentinel;
    memset(&sentinel, 0xa5, sizeof sentinel);
    out = sentinel;
    CHECK(!azstem_overview_lease_v1(AZOV_LEASE_ABI, &out, sizeof out));
    CHECK(!memcmp(&out, &sentinel, sizeof out));
}
static void *startup_queries(void *unused) {
    (void) unused;
    struct azov_lease_api out, sentinel;
    memset(&sentinel, 0xa5, sizeof sentinel);
    for (unsigned i = 0; i < 10000; ++i) {
        out = sentinel;
        CHECK(!azstem_overview_lease_v1(AZOV_LEASE_ABI, &out, sizeof out));
        CHECK(!memcmp(&out, &sentinel, sizeof out));
    }
    return NULL;
}
int azov_test_enter_owned(const struct azov_owned_entry *capture, struct azov_entry *entry) {
    if (acquire_fail) return 0;
    CHECK(!pthread_mutex_lock(&owner_mutex));
    uintptr_t widget = capture->widget;
    uintptr_t shared, content;
    memcpy(&shared, (const unsigned char *) widget + 0x80, sizeof shared);
    memcpy(&content, (const unsigned char *) shared + 0x28, sizeof content);
#if defined(__linux__) && defined(__aarch64__)
    if (expect_native_capture) {
        CHECK(capture->held_content == content);
        CHECK(capture->native_return == (uintptr_t) overview_test_native_return);
        CHECK(capture->native_stack && !(capture->native_stack & 15u));
    }
#endif
    entry->content = content;
    entry->cookie = &owner_mutex;
    return 1;
}
void azov_test_leave_owned(struct azov_entry *entry) {
    CHECK(entry->cookie == &owner_mutex);
    CHECK(!pthread_mutex_unlock(&owner_mutex));
}

static void store_pointer(unsigned char *object, size_t offset, const void *pointer) {
    uintptr_t value = (uintptr_t) pointer;
    memcpy(object + offset, &value, sizeof value);
}
static unsigned char *read_pointer(const unsigned char *object, size_t offset) {
    uintptr_t value;
    memcpy(&value, object + offset, sizeof value);
    return (unsigned char *) value;
}
static void fake_owner_release(void *owner, void *object) {
    CHECK(owner == &owner_mutex);
    CHECK(read_pointer(object, 0x28) == destroy_expected_points);
    destroys++;
    free(object); /* Real retirement, so sanitizers also observe incorrect late restoration. */
}
static void native_header(unsigned char *content) {
    int references = 1;
    uint32_t guard = UINT32_C(0x52434f58);
    memcpy(content + 0x0c, &references, sizeof references);
    memcpy(content + 0x10, &guard, sizeof guard);
    store_pointer(content, 0x18, &owner_mutex);
}
static unsigned stock_calls;
static void stock_no_read(long self) {
    CHECK(self == 1);
    stock_calls++;
}
static void *off_thread_stock(void *unused) {
    (void) unused;
    ov_hook(1);
    return NULL;
}
static void stock_fallbacks(void) {
    g_orig_ov = stock_no_read;
    CHECK(__atomic_load_n(&g_ov_bind_status, __ATOMIC_RELAXED) == OV_LEASE_UNBOUND);
    CHECK(__atomic_load_n(&g_ov_acquired, __ATOMIC_RELAXED) == 0);
    const int expected[] = {OV_LEASE_MISSING, OV_LEASE_PROFILE_UNAVAILABLE,
                            OV_LEASE_INCOMPATIBLE, OV_LEASE_QUERY_REJECTED};
    for (int reason = 0; reason < 4; ++reason) {
        missing_helper = reason == 0;
        profile_ready = reason != 1;
        invalid_helper = reason == 2;
        rejected_helper = reason == 3;
        g_ovlease_bound = 0;
        memset(&g_ovlease, 0, sizeof g_ovlease);
        ov_hook(1); /* An unavailable helper/profile must not dereference even the widget. */
        CHECK(stock_calls == (unsigned) reason + 1);
        CHECK(__atomic_load_n(&g_ov_bind_status, __ATOMIC_ACQUIRE) == expected[reason]);
        CHECK(__atomic_load_n(&g_ov_acquired, __ATOMIC_RELAXED) == 0);
    }
    missing_helper = invalid_helper = rejected_helper = 0;
    profile_ready = 1;
    g_ovlease_bound = 0;
    memset(&g_ovlease, 0, sizeof g_ovlease);
    __atomic_store_n(&g_ov_bind_status, OV_LEASE_UNBOUND, __ATOMIC_RELEASE);
    pthread_t other;
    unsigned before = stock_calls;
    CHECK(!pthread_create(&other, NULL, off_thread_stock, NULL));
    CHECK(!pthread_join(other, NULL));
    CHECK(stock_calls == before + 1 && !g_ovlease_bound && !g_ovlease.begin);
    CHECK(__atomic_load_n(&g_ov_bind_status, __ATOMIC_ACQUIRE) == OV_LEASE_UNBOUND);
    CHECK(__atomic_load_n(&g_ov_acquired, __ATOMIC_RELAXED) == 0);
}
static void store_count(unsigned char *object, int count) { memcpy(object + 0x30, &count, sizeof count); }
static unsigned char observed[2048 * 12];
static unsigned char *input_array, *replacement_array;
static int expected_count, expect_copy, concurrent;
static unsigned consumer_calls;
/* Literal offsets follow the independently checked native instructions, not OV_HGT_*.
 * self+80 -> shared+28 -> Content, Content+28 / +30, stride12, heights4/5. */
static void native_consumer(long self) {
    unsigned char *shared = read_pointer((const unsigned char *) self, 0x80);
    unsigned char *content = read_pointer(shared, 0x28);
    unsigned char *points = read_pointer(content, 0x28);
    int count;
    memcpy(&count, content + 0x30, sizeof count);
    CHECK(count == expected_count && count > 0 && count <= 2048);
    CHECK(expect_copy ? points != input_array : points == input_array);
    memcpy(observed, points, (size_t) count * 12);
    if (concurrent)
        for (int i = 0; i < count; i++) {
            CHECK(points[i * 12 + 4] <= input_array[i * 12 + 4]);
            CHECK(points[i * 12 + 5] <= points[i * 12 + 4]);
        }
    if (replacement_array) store_pointer(content, 0x28, replacement_array);
    consumer_calls++;
}
/* The proven PWV4 payload conversion produces these synthetic native records.
 * RGB, blue palette index, max/high heights, then the retained six wire bytes. */
static void native_record(unsigned char *out, unsigned seed) {
    unsigned char raw[6] = {(unsigned char) (3 + seed % 17), (unsigned char) (9 + seed % 19),
                            (unsigned char) (seed % 7), (unsigned char) (80 + seed % 48),
                            (unsigned char) (20 + seed % 51), (unsigned char) (32 + seed % 47)};
    unsigned maximum = raw[3];
    if (maximum < raw[4]) maximum = raw[4];
    if (maximum < raw[5]) maximum = raw[5];
    for (unsigned i = 0; i < 3; i++) out[i] = (unsigned char) (raw[i + 3] * 255u / maximum);
    out[3] = 4;
    out[4] = (unsigned char) maximum;
    out[5] = raw[5];
    memcpy(out + 6, raw, sizeof raw);
}
static void curve(unsigned factor, unsigned color) {
    for (int i = 0; i < ATT_N; i++) {
        __atomic_store_n(&g_att[0][i], (unsigned short) factor, __ATOMIC_RELAXED);
        __atomic_store_n(&g_col[0][i], i & 1 ? color : 0, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&g_att_gen[0], 1, __ATOMIC_RELEASE);
}
static void verify_frame(unsigned char *widget, unsigned char *content, const unsigned char *original,
                         int count, unsigned factor, unsigned tint, int copy) {
    expected_count = count;
    expect_copy = copy;
    unsigned before = consumer_calls;
    long acquired = __atomic_load_n(&g_ov_acquired, __ATOMIC_RELAXED);
    long applied = __atomic_load_n(&g_ov_applied, __ATOMIC_RELAXED);
    ov_hook((long) widget);
    CHECK(consumer_calls == before + 1);
    CHECK(__atomic_load_n(&g_ov_bind_status, __ATOMIC_ACQUIRE) == OV_LEASE_READY);
    CHECK(__atomic_load_n(&g_ov_acquired, __ATOMIC_RELAXED) == acquired + 1);
    CHECK(__atomic_load_n(&g_ov_applied, __ATOMIC_RELAXED) == applied + copy);
    CHECK(read_pointer(content, 0x28) == input_array);
    CHECK(!memcmp(input_array, original, (size_t) count * 12));
    for (int i = 0; i < count; i++) {
        unsigned char expected[12];
        memcpy(expected, original + i * 12, sizeof expected);
        if (copy) {
            expected[4] = (unsigned char) (expected[4] * factor / 256u);
            expected[5] = (unsigned char) (expected[5] * factor / 256u);
            unsigned cell = (unsigned) ((long) i * ATT_N / count);
            unsigned color = cell & 1 ? tint : 0;
            if (color) {
                for (int span = 0; span <= 6; span += 6) {
                    expected[span] = (unsigned char) (color >> 16);
                    expected[span + 1] = (unsigned char) (color >> 8);
                    expected[span + 2] = (unsigned char) color;
                }
            }
        }
        CHECK(!memcmp(observed + i * 12, expected, sizeof expected));
        /* Independent native-consumer draw stage: both scalar heights must attenuate. */
        CHECK((observed[i * 12 + 4] * 16384u >> 15) == (expected[4] * 64u / 128u));
        CHECK((observed[i * 12 + 5] * 16384u >> 15) == (expected[5] * 64u / 128u));
    }
}
static void *curve_writer(void *unused) {
    (void) unused;
    for (unsigned round = 0; round < 100; round++) curve(round & 1 ? 0 : 256, 0);
    return NULL;
}
int main(void) {
    fixture_thread = pthread_self();
    CHECK(OV_STRIDE == 12 && OV_MAX == 2048);
    unsigned char *widget = calloc(1, 0x88), *shared = calloc(1, 0x30), *content = calloc(1, 0x38);
    input_array = malloc(2048 * 12);
    unsigned char *original = malloc(2048 * 12);
    CHECK(widget && shared && content && input_array && original);
    native_header(content);
    pthread_t startup;
    CHECK(!pthread_create(&startup, NULL, startup_queries, NULL));
    azov_lease_configure(2, fake_owner_release);
    CHECK(!pthread_join(startup, NULL));
    stock_fallbacks();
    for (int i = 0; i < 2048; i++) native_record(input_array + i * 12, (unsigned) i);
    memcpy(original, input_array, 2048 * 12);
    store_pointer(widget, 0x80, shared);
    store_pointer(shared, 0x28, content);
    store_pointer(content, 0x28, input_array);
    g_orig_ov = native_consumer;
    g_wavemute = g_att_have[0] = 1;
    selected_deck = 0;
    const unsigned factors[] = {0, 1, 64, 128, 255, 256};
    const int counts[] = {1, 3, 17, 2048};
    for (unsigned c = 0; c < sizeof counts / sizeof *counts; c++) {
        store_count(content, counts[c]);
        for (unsigned a = 0; a < sizeof factors / sizeof *factors; a++)
            for (unsigned tinted = 0; tinted < 2; tinted++) {
                g_wavecolor = (int) tinted;
                curve(factors[a], tinted ? 0x123456 : 0);
                verify_frame(widget, content, original, counts[c], factors[a], tinted ? 0x123456 : 0, 1);
            }
    }
    store_count(content, 3);
    g_wavecolor = 0;
    curve(256, 0);
    for (int reason = 0; reason < 4; reason++) {
        selected_deck = reason == 0 ? -1 : 0;
        g_wavemute = reason != 1;
        g_att_have[0] = reason != 2;
        g_att_ident[0] = reason == 3;
        verify_frame(widget, content, original, 3, 256, 0, 0);
    }
    selected_deck = 0;
    g_wavemute = g_att_have[0] = 1;
    g_att_ident[0] = 0;
    CHECK(!ov_scaled_for(0, NULL, 3));
    CHECK(!ov_scaled_for(0, input_array, 0));
    CHECK(!ov_scaled_for(0, input_array, 2049));
    curve(128, 0);
    g_wavecolor = 0;
    verify_frame(widget, content, original, 3, 128, 0, 1);
    /* A changed source high-height byte alone must refresh the cached copy. */
    original[5] = input_array[5] = 17;
    verify_frame(widget, content, original, 3, 128, 0, 1);
    pthread_t writer;
    concurrent = 1;
    expected_count = 3;
    expect_copy = 1;
    CHECK(!pthread_create(&writer, NULL, curve_writer, NULL));
    for (int i = 0; i < 100; i++) ov_hook((long) widget);
    CHECK(!pthread_join(writer, NULL));
    concurrent = 0;
    curve(128, 0);
    replacement_array = original;
    ov_hook((long) widget);
    CHECK(read_pointer(content, 0x28) == replacement_array);
    CHECK(!memcmp(input_array, original, 3 * 12));
    for (int i = 0; i < OVINST_MAX; i++) {
        free(g_ovi[i].snap);
        free(g_ovi[i].scaled);
    }
    memset(g_ovi, 0, sizeof g_ovi);
    g_ovi_n = 0;
    free(input_array);
    free(original);
    free(content);
    free(shared);
    free(widget);
    printf("wave overview: %u checks passed; native heights4/5, immutable source, style and hook restoration\n", checks);
    return 0;
}

/* Called by the C++ driver. The C hook itself is compiled with the release unwind flags. */
static unsigned char *throw_widget, *throw_shared, *throw_content, *throw_original;
static int throw_mode;
static long throw_acquired;
void overview_exception_start(void (*renderer)(long), int mode) {
    fixture_thread = pthread_self();
    azov_lease_configure(2, fake_owner_release);
    throw_widget = calloc(1, 0x88);
    throw_shared = calloc(1, 0x30);
    throw_content = calloc(1, 0x38);
    input_array = malloc(3 * 12);
    throw_original = malloc(3 * 12);
    CHECK(throw_widget && throw_shared && throw_content && input_array && throw_original);
    native_header(throw_content);
    for (int i = 0; i < 3; ++i) native_record(input_array + i * 12, (unsigned) i);
    memcpy(throw_original, input_array, 3 * 12);
    store_pointer(throw_widget, 0x80, throw_shared);
    store_pointer(throw_shared, 0x28, throw_content);
    store_pointer(throw_content, 0x28, input_array);
    store_count(throw_content, 3);
    throw_mode = mode;
    destroys = 0;
    destroy_expected_points = mode & 1 ? throw_original : input_array;
    selected_deck = 0;
    g_wavemute = g_att_have[0] = 1;
    g_att_ident[0] = g_wavecolor = 0;
    curve(128, 0);
    acquire_fail = !!(mode & 4);
    g_orig_ov = renderer;
    throw_acquired = __atomic_load_n(&g_ov_acquired, __ATOMIC_RELAXED);
#if defined(__linux__) && defined(__aarch64__)
    expect_native_capture = 1;
    overview_test_native_call((long) throw_widget, (uintptr_t) throw_content);
#else
    ov_hook((long) throw_widget);
#endif
    CHECK(0); /* All callbacks must propagate their original C++ exception. */
}
void overview_exception_observe(long self) {
    CHECK(self == (long) throw_widget);
    unsigned char *points = read_pointer(throw_content, 0x28);
    CHECK(acquire_fail ? points == input_array : points != input_array);
    if (!acquire_fail) {
        CHECK(points[4] == input_array[4] / 2);
        CHECK(points[5] == input_array[5] / 2);
    }
    if (throw_mode & 1) store_pointer(throw_content, 0x28, throw_original);
    if (throw_mode & 2) {
        int old = __atomic_fetch_sub((int *) (throw_content + 0x0c), 1, __ATOMIC_ACQ_REL);
        if (old == 1) fake_owner_release(&owner_mutex, throw_content);
        CHECK(destroys == 0); /* A genuine retained lease prevents retirement in the callback. */
    }
}
void overview_exception_check(void) {
    CHECK(__atomic_load_n(&g_ov_bind_status, __ATOMIC_ACQUIRE) == OV_LEASE_READY);
    CHECK(__atomic_load_n(&g_ov_acquired, __ATOMIC_RELAXED) == throw_acquired + !acquire_fail);
    if (throw_mode & 2) {
        CHECK(destroys == 1);
    } else {
        CHECK(destroys == 0);
        CHECK(read_pointer(throw_content, 0x28) == destroy_expected_points);
        CHECK(__atomic_load_n((int *) (throw_content + 0x0c), __ATOMIC_ACQUIRE) == 1);
        free(throw_content);
    }
    free(throw_widget);
    free(throw_shared);
    free(input_array);
    free(throw_original);
    acquire_fail = 0;
#if defined(__linux__) && defined(__aarch64__)
    expect_native_capture = 0;
#endif
}
extern int azov_test_validate_capture(const struct azov_owned_entry *, uintptr_t, uintptr_t,
                                      uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static void native_capture_guards(void) {
    _Alignas(16) unsigned char frame[0xe0] = {0};
    _Alignas(8) uintptr_t content[7] = {UINT64_C(0x2906700)};
    uintptr_t sp = (uintptr_t) frame;
    uintptr_t child = 0x1f68a90, parent = 0x1d80c7c, vt = content[0];
    memcpy(frame + 8, &parent, sizeof parent);
    store_pointer(frame, 0xd0, content);
    struct azov_owned_entry capture = {1, (uintptr_t) content, child, sp}, changed;
#define VALIDATE(c, lo, hi, here) azov_test_validate_capture(&(c), (lo), (hi), (here), child, parent, vt)
    CHECK(VALIDATE(capture, sp, sp + sizeof frame, sp - 64));
    changed = capture;
    changed.native_return++;
    CHECK(!VALIDATE(changed, sp, sp + sizeof frame, sp - 64));
    changed = capture;
    changed.held_content = 0;
    CHECK(!VALIDATE(changed, sp, sp + sizeof frame, sp - 64));
    changed = capture;
    changed.held_content = UINTPTR_MAX;
    CHECK(!VALIDATE(changed, sp, sp + sizeof frame, sp - 64));
    changed = capture;
    changed.native_stack = 1;
    CHECK(!VALIDATE(changed, sp, sp + sizeof frame, sp - 64));
    changed = capture;
    changed.native_stack = UINTPTR_MAX - 15;
    CHECK(!VALIDATE(changed, sp, sp + sizeof frame, sp - 64));
    CHECK(!VALIDATE(capture, sp + 16, sp + sizeof frame, sp - 64));
    CHECK(!VALIDATE(capture, sp, sp + 0xd7, sp - 64));
    CHECK(!VALIDATE(capture, sp, sp + sizeof frame, sp));
    CHECK(!VALIDATE(capture, sp, sp + sizeof frame, sp - 65537));
    parent++;
    CHECK(!VALIDATE(capture, sp, sp + sizeof frame, sp - 64));
    parent--;
    store_pointer(frame, 0xd0, NULL);
    CHECK(!VALIDATE(capture, sp, sp + sizeof frame, sp - 64));
    store_pointer(frame, 0xd0, content);
    content[0]++;
    CHECK(!VALIDATE(capture, sp, sp + sizeof frame, sp - 64));
#undef VALIDATE
}
struct wrong_thread_input {
    const struct azov_lease_api *api;
    const struct azov_lease *lease;
};
static void *wrong_thread(void *argument) {
    const struct wrong_thread_input *input = argument;
    struct azov_lease out;
    struct azov_owned_entry invalid = {1, 0, 0, 0};
    CHECK(!input->api->begin(&invalid, &out, sizeof out));
    CHECK(!input->api->install(input->lease->token, (unsigned char *) 1));
    CHECK(!input->api->finish(input->lease->token));
    return NULL;
}
void overview_lease_checks(void) {
    native_capture_guards();
    unsigned char widget[0x88] = {0}, shared[0x30] = {0}, content[0x38] = {0}, points[12] = {0};
    native_header(content);
    store_pointer(widget, 0x80, shared);
    store_pointer(shared, 0x28, content);
    store_pointer(content, 0x28, points);
    store_count(content, 1);
    struct azov_lease_api api = {0}, unchanged;
    memset(&unchanged, 0xa5, sizeof unchanged);
    api = unchanged;
    CHECK(!azstem_overview_lease_v1(99, &api, sizeof api));
    CHECK(!memcmp(&api, &unchanged, sizeof api));
    CHECK(!azstem_overview_lease_v1(AZOV_LEASE_ABI, &api, sizeof api - 1));
    CHECK(!memcmp(&api, &unchanged, sizeof api));
    CHECK(azstem_overview_lease_v1(AZOV_LEASE_ABI, &api, sizeof api));
    struct azov_lease lease, second, sentinel;
    struct azov_owned_entry capture = {(uintptr_t) widget, 0, 0, 0};
    struct azov_owned_entry invalid = {1, 0, 0, 0};
    memset(&sentinel, 0xa5, sizeof sentinel);
    lease = sentinel;
    profile_ready = 0;
    CHECK(!api.begin(&invalid, &lease, sizeof lease)); /* Reject before arbitrary memory. */
    CHECK(!memcmp(&lease, &sentinel, sizeof lease));
    profile_ready = 1;
    trusted = 0;
    CHECK(!api.begin(&invalid, &lease, sizeof lease));
    trusted = 1;
    acquire_fail = 1;
    CHECK(!api.begin(&capture, &lease, sizeof lease));
    CHECK(!memcmp(&lease, &sentinel, sizeof lease));
    acquire_fail = 0;
    CHECK(api.begin(&capture, &lease, sizeof lease));
    CHECK(__atomic_load_n((int *) (content + 0x0c), __ATOMIC_ACQUIRE) == 2);
    pthread_t thread;
    struct wrong_thread_input input = {&api, &lease};
    CHECK(!pthread_create(&thread, NULL, wrong_thread, &input));
    CHECK(!pthread_join(thread, NULL));
    CHECK(!api.begin(&capture, &second, sizeof second));
    uint64_t wrong[2] = {lease.token[0] + 1, lease.token[1]};
    CHECK(!api.finish(wrong));
    CHECK(__atomic_load_n((int *) (content + 0x0c), __ATOMIC_ACQUIRE) == 2);
    trusted = 0;
    CHECK(!api.finish(lease.token));
    trusted = 1;
    CHECK(api.finish(lease.token));
    CHECK(!api.finish(lease.token));
    const int bad_counts[] = {0, -1, 2049};
    for (unsigned i = 0; i < sizeof bad_counts / sizeof *bad_counts; ++i) {
        store_count(content, bad_counts[i]);
        second = sentinel;
        CHECK(!api.begin(&capture, &second, sizeof second));
        CHECK(!memcmp(&second, &sentinel, sizeof second));
        CHECK(__atomic_load_n((int *) (content + 0x0c), __ATOMIC_ACQUIRE) == 1);
    }
    store_count(content, 1);
    __atomic_store_n((int *) (content + 0x0c), INT32_MAX, __ATOMIC_RELEASE);
    CHECK(!api.begin(&capture, &second, sizeof second));
    CHECK(__atomic_load_n((int *) (content + 0x0c), __ATOMIC_ACQUIRE) == INT32_MAX);
    __atomic_store_n((int *) (content + 0x0c), INT32_MAX - 1, __ATOMIC_RELEASE);
    CHECK(!api.begin(&capture, &second, sizeof second));
    CHECK(__atomic_load_n((int *) (content + 0x0c), __ATOMIC_ACQUIRE) == INT32_MAX - 1);
    __atomic_store_n((int *) (content + 0x0c), INT32_MAX - 2, __ATOMIC_RELEASE);
    CHECK(api.begin(&capture, &second, sizeof second));
    CHECK(__atomic_add_fetch((int *) (content + 0x0c), 1, __ATOMIC_ACQ_REL) == INT32_MAX);
    CHECK(__atomic_sub_fetch((int *) (content + 0x0c), 1, __ATOMIC_ACQ_REL) == INT32_MAX - 1);
    CHECK(api.finish(second.token));
    CHECK(__atomic_load_n((int *) (content + 0x0c), __ATOMIC_ACQUIRE) == INT32_MAX - 2);
    __atomic_store_n((int *) (content + 0x0c), 1, __ATOMIC_RELEASE);
    force_contention = 1;
    retain_attempts = 0;
    second = sentinel;
    CHECK(!api.begin(&capture, &second, sizeof second));
    CHECK(retain_attempts == 8 && !memcmp(&second, &sentinel, sizeof second));
    CHECK(__atomic_load_n((int *) (content + 0x0c), __ATOMIC_ACQUIRE) == 1);
    force_contention = 0;
    uint32_t guard = 0;
    memcpy(content + 0x10, &guard, sizeof guard);
    CHECK(!api.begin(&capture, &second, sizeof second));
    guard = UINT32_C(0x52434f58);
    memcpy(content + 0x10, &guard, sizeof guard);
    CHECK(!api.begin(&capture, &second, sizeof second - 1));
    /* Capacity is bounded; releasing consumes every slot without resetting generations. */
    _Alignas(8) unsigned char widgets[9][0x88] = {{0}}, shareds[9][0x30] = {{0}}, contents[9][0x38] = {{0}};
    struct azov_lease leases[8];
    for (unsigned i = 0; i < 9; ++i) {
        native_header(contents[i]);
        store_pointer(widgets[i], 0x80, shareds[i]);
        store_pointer(shareds[i], 0x28, contents[i]);
        store_pointer(contents[i], 0x28, points);
        store_count(contents[i], 1);
        struct azov_owned_entry other = {(uintptr_t) widgets[i], 0, 0, 0};
        if (i < 8) CHECK(api.begin(&other, &leases[i], sizeof leases[i]));
        else CHECK(!api.begin(&other, &second, sizeof second));
    }
    for (unsigned i = 0; i < 8; ++i) CHECK(api.finish(leases[i].token));
    CHECK(api.begin(&capture, &second, sizeof second));
    CHECK(second.token[0] != lease.token[0]);
    CHECK(!api.finish(lease.token));
    azov_lease_configure(0, NULL); /* Retire new acquisition without abandoning this lease. */
    CHECK(!api.begin(&capture, &lease, sizeof lease));
    CHECK(api.finish(second.token));
    CHECK(__atomic_load_n((int *) (content + 0x0c), __ATOMIC_ACQUIRE) == 1);
    printf("overview lease: tokens, failed acquisition, trust, unavailable profile and retirement passed\n");
}
