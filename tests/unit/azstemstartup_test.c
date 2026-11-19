/* SPDX-License-Identifier: GPL-3.0-only */
/* Complete production startup functions; record OS/native installation boundaries. */
#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../src/azstem/ep147addr.h"
#define NDECK 4
#define NPROBE 8
#define GP_L 2048
#define AZ_STATUS_NATIVE_PRODUCER 1
static const char *process_name, *override_text;
static int build_id, identify_calls, clear_calls, lease_build, deck_build;
static int hook_fail_at, hook_calls, main_hook_result, stem_worker_calls;
static int thread_fail_mask, thread_calls, detached, no_native_id, probe_flag, status_off;
static uintptr_t installed_addresses[32];
static _Atomic uintptr_t g_djpu_vt;
static uintptr_t g_probe_addr[NPROBE], g_lsctor_addr, g_func_addr;
static uintptr_t g_pbread_addr, g_fread_addr, g_operate_addr, g_kf_addr;
static uintptr_t g_fgpr_setsrc_addr, g_fgpr_read_addr, g_ringfill_addr, g_fgpr_op_addr;
static void (*g_meow_destroy_fn)(void *, void *);
static void *g_orig_lsdtor, *g_orig_lti_update, *g_orig_lsctor;
static void *g_orig_pbread, *g_orig_fread, *g_orig_operate, *g_orig_kf;
static void *g_orig_fgpr_setsrc, *g_orig_fgpr_read, *g_orig_ringfill, *g_orig_fgpr_op;
static int g_lti_probe, g_lti_on, g_status_lti_on, g_maskpoll_up;
static _Atomic int g_installed, g_hooks_late;
static struct {
    int cur, prevS, anchorS;
    double rate;
} g_dk[NDECK];
static _Atomic long long g_play_obs[NDECK];
static float g_hann[GP_L];
static FILE *stream(const char *text) {
    if (!text) return NULL;
    FILE *f = tmpfile();
    assert(f && fputs(text, f) >= 0 && !fseek(f, 0, SEEK_SET));
    return f;
}
static FILE *process_open(const char *path, const char *mode) {
    assert(!strcmp(path, "/proc/self/comm") && !strcmp(mode, "re"));
    return stream(process_name);
}
static int identify(const char *path, char *md5, long *size) {
    assert(!strcmp(path, "/proc/self/exe"));
    strcpy(md5, "fixture");
    *size = 123;
    ++identify_calls;
    return build_id;
}
static FILE *azlog_fopen_capped(const char *path, long limit) {
    assert(!strcmp(path, "/tmp/azstem-build.log") && limit == 16L * 1024);
    return tmpfile();
}
static void azov_lease_configure(int build, void (*release)(void *, void *)) {
    assert(release && release == g_meow_destroy_fn);
    lease_build = build;
}
static void clear_published_deck_state(void) { ++clear_calls; }
static FILE *addr_override_open(const char *path) {
    assert(path);
    return stream(override_text);
}
static void logln(const char *format, ...) { assert(format); }
static int dev_flag(const char *path) { return strstr(path, "nonativeid") ? no_native_id : probe_flag; }
static int setting_access(const char *path, int mode) {
    assert(mode == F_OK && !strcmp(path, "/home/root/settings/azmod.deck-status.off"));
    return status_off ? 0 : -1;
}
static void *install_hook_at(uintptr_t address, void *hook, void **original) {
    assert(address && hook && original && hook_calls < 32);
    installed_addresses[hook_calls++] = address;
    if (hook_fail_at == hook_calls) return NULL;
    *original = hook;
    return hook;
}
#define HOOK(name) \
    static void name(void) {}
HOOK(lsdtor_hook)
HOOK(lti_update_hook)
HOOK(lsctor_hook)
HOOK(pbread_hook)
HOOK(fread_hook)
HOOK(operate_hook)
HOOK(kf_hook)
HOOK(fgpr_setsrc_hook)
HOOK(fgpr_read_hook)
HOOK(ringfill_hook)
HOOK(fgpr_op_hook)
static void *flusher(void *arg) { return arg; }
static void *maskpoll_thread(void *arg) { return arg; }
static int az_thread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *arg) {
    assert(!attr && !arg && entry == (thread_calls ? maskpoll_thread : flusher));
    *thread = (pthread_t) ++thread_calls;
    return thread_fail_mask & (1 << (thread_calls - 1)) ? 11 : 0;
}
static int detach_thread(pthread_t thread) {
    assert(thread);
    ++detached;
    return 0;
}
static uintptr_t read_target_addr(void) { return 0x123456; }
static void azstem_deck_hooks(unsigned long long address, int build) {
    assert(address == (uintptr_t) EP_14a2560);
    deck_build = build;
}
static int install_hook(void) { return main_hook_result; }
static void status_stem_probe(void) {}
static void azn_stems_start(void (*probe)(void)) {
    assert(probe == status_stem_probe);
    ++stem_worker_calls;
}
#define ep147_identify identify
#define fopen process_open
#define access setting_access
#define pthread_detach detach_thread
#include STARTUP_INC
#undef pthread_detach
#undef access
#undef fopen
#undef ep147_identify
static void reset(void) {
    hook_calls = hook_fail_at = thread_calls = detached = thread_fail_mask = 0;
    clear_calls = lease_build = deck_build = identify_calls = stem_worker_calls = 0;
    no_native_id = probe_flag = status_off = 0;
    g_lti_on = g_lti_probe = g_status_lti_on = g_maskpoll_up = 0;
    g_hooks_late = g_installed = 0;
    override_text = NULL;
    main_hook_result = 1;
}
static void startup_matrix(void) {
    for (int build = 1; build <= 2; ++build)
        for (int fail = 0; fail <= 3; ++fail) {
            reset();
            build_id = build;
            process_name = "EP147\n";
            hook_fail_at = fail;
            azstem_init();
            assert(identify_calls == 1 && clear_calls == 1 && lease_build == build && deck_build == build);
            assert(g_djpu_vt == DJPU_VTABLE && g_func_addr == 0x123456 && g_installed == 1 && g_hooks_late);
            assert(g_probe_addr[0] == EP_7670a8 && g_probe_addr[7] == EP_d58ee0);
            assert(g_status_lti_on && g_lti_on == (fail == 0));
            assert(hook_calls == (fail ? fail : 3) + 8 && installed_addresses[0] == LSDTOR_DEFAULT);
            if (fail != 1) assert(installed_addresses[1] == LTI_UPDATE_DEFAULT);
            if (!fail || fail == 3) assert(installed_addresses[2] == LSCTOR_DEFAULT);
            assert(stem_worker_calls == 1 && thread_calls == 2 && detached == 2 && g_maskpoll_up);
            for (int deck = 0; deck < NDECK; ++deck)
                assert(g_dk[deck].cur == -1 && g_dk[deck].prevS == -1 && g_dk[deck].anchorS == -1 && g_dk[deck].rate == 1 && g_play_obs[deck] == -1);
            assert(g_hann[0] == 0 && fabsf(g_hann[GP_L / 2] - 1) < 0.00001f);
        }
    const char *names[] = {NULL, "", "other\n", "EP147-extra\n"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
        reset();
        process_name = names[i];
        azstem_init();
        assert(!identify_calls && !clear_calls && !hook_calls && !thread_calls);
    }
    reset();
    process_name = "EP147";
    build_id = EPB_UNKNOWN;
    azstem_init();
    assert(identify_calls == 1 && !clear_calls && !hook_calls && !thread_calls);
    for (int fail = 0; fail < 4; ++fail) {
        reset();
        build_id = 1;
        process_name = "EP147";
        thread_fail_mask = fail;
        main_hook_result = -1;
        status_off = 1;
        azstem_init();
        assert(g_installed == -1 && !stem_worker_calls && !g_status_lti_on);
        assert(thread_calls == 2 && detached == 2 - !!(fail & 1) - !!(fail & 2) && g_maskpoll_up == !(fail & 2));
    }
    reset();
    assert(ep147_addr_init(1));
    no_native_id = 1;
    azstem_configure_lti();
    assert(!hook_calls && !g_lti_on && !g_lti_probe);
    probe_flag = 1;
    azstem_configure_lti();
    assert(hook_calls == 3 && !g_lti_on && g_lti_probe);
    const char *values[] = {NULL, "", "nonnumeric", "0", "abcd"};
    const unsigned long long expected[] = {23, 23, 23, 23, 0xabcd};
    for (unsigned i = 0; i < 5; ++i) {
        override_text = values[i];
        assert(azstem_hook_address("test", 23) == expected[i]);
    }
    override_text = NULL;
    g_djpu_vt = 42;
    azstem_vtable_override();
    assert(g_djpu_vt == 42);
    override_text = "nonnumeric";
    azstem_vtable_override();
    assert(g_djpu_vt == 42);
    override_text = "0";
    azstem_vtable_override();
    assert(!g_djpu_vt);
    override_text = "abcd";
    azstem_vtable_override();
    assert(g_djpu_vt == 0xabcd);
    hook_calls = 0;
    hook_fail_at = 1;
    azstem_install_named("failure", 1, (void *) lsdtor_hook, &g_orig_lsdtor);
    assert(hook_calls == 1);
}
struct Meow {
    uintptr_t vtable;
    uint32_t pad;
    _Atomic int refs;
    uint32_t guard, pad2;
    void *owner;
};
static unsigned destroyed;
static void destroy(void *owner, void *object) {
    assert(owner == object);
    ++destroyed;
}
static void *pin_worker(void *arg) {
    for (unsigned i = 0; i < 10000; ++i)
        if (azn_ref_pin((uintptr_t) arg)) azn_ref_release((uintptr_t) arg);
    return NULL;
}
static void pin_lifetime(void) {
    struct Meow object = {.refs = 1, .guard = BG_XOCR};
    object.owner = &object;
    uintptr_t p = (uintptr_t) &object;
    g_meow_destroy_fn = NULL;
    assert(!azn_ref_pin(p) && object.refs == 1);
    g_meow_destroy_fn = destroy;
    assert(!azn_ref_pin(0) && !azn_ref_pin(p + 1));
    object.guard = 0;
    assert(!azn_ref_pin(p));
    object.guard = BG_XOCR;
    object.refs = 0;
    assert(!azn_ref_pin(p));
    object.refs = INT_MAX;
    assert(!azn_ref_pin(p) && object.refs == INT_MAX);
    object.refs = 1;
    assert(azn_ref_pin(p) && object.refs == 2);
    azn_ref_release(p);
    assert(object.refs == 1 && !destroyed);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, pin_worker, &object));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    assert(object.refs == 1 && !destroyed);
    azn_ref_release(p);
    assert(object.refs == 0 && destroyed == 1);
    assert(!azn_ref_pin(p));
}
int main(void) {
    startup_matrix();
    pin_lifetime();
    puts("PASS azstem startup: identity, both profiles, ordered hooks, failures, pollers and owned refcounts");
}
