/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../../src/common/aztimestamp.h"
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#define AZMOD_STEM_DECKS 4
#define SM_CACHE 3

static struct {
    unsigned long ptr;
    int deck;
} g_wkey[48];
static int g_wkey_n, g_upd_deck = -1, g_upd_bound, g_upd_skip;
static long g_upd_calls, g_upd_skipped;
static int g_gridaa_ok, g_in_root;
static unsigned g_root_pass, g_built_pass[4];
static void (*g_orig_wvupd)(long);
static long fake_ns;
static long long fake_ms, library_signature, stems_signature;
static int fallback_deck, fallback_calls, begin_deck, begin_count, commit_skip, commits;
static long begin_widget, begin_position;
static float begin_zoom;
static long g_ws_n[2], g_ws_rns[2];
static int rendered_count, native_returned;
static long *render_provider;
static char events[2048];
static size_t event_count;
static void event(char value) {
    assert(event_count + 1 < sizeof events);
    events[event_count++] = value;
    events[event_count] = 0;
}
static void clear_events(void) {
    event_count = 0;
    events[0] = 0;
}
static void native_root(long self) { (void) self; }
static int azm_upd_deck_of(long pointer) {
    assert(pointer);
    fallback_calls++;
    event('F');
    return fallback_deck;
}
static void gridaa_enter(long wwb, long pos, float zoom) {
    (void) wwb;
    (void) pos;
    (void) zoom;
    g_gridaa_ok = 1;
    event('G');
}
static void vx_upd_begin(int deck, long wwb, long pos, float zoom) {
    begin_count++;
    begin_deck = deck;
    begin_widget = wwb;
    begin_position = pos;
    begin_zoom = zoom;
    event('B');
}
static void vx_upd_commit(int skipped) {
    commit_skip = skipped;
    commits++;
    event('C');
}
static void vx_upd_rendered(void) {
    assert(native_returned);
    if (render_provider) assert(render_provider[0] == 100 && render_provider[1] == 200);
    rendered_count++;
    event('R');
}
static long ws_now_ns(void) { return fake_ns; }
static int fixture_clock(clockid_t id, struct timespec *out) {
    if (id == CLOCK_MONOTONIC) {
        out->tv_sec = fake_ns / 1000000000L;
        out->tv_nsec = fake_ns % 1000000000L;
        event('T');
    } else {
        assert(id == CLOCK_REALTIME);
        out->tv_sec = 9;
        out->tv_nsec = 100;
    }
    return 0;
}

static struct {
    int st, stale;
} g_smc[SM_CACHE];
static unsigned char g_smpc[24];
static int g_sm_world, g_sm_dirty, g_sm_budget, g_smq_n, g_sm_idle_thread;
static int locked, hotcue_queued, waits, pending_scans, world_notifications, releases;
static int schedule_result, worker_calls, worker_limit;
static pthread_cond_t g_sm_cv;
static pthread_mutex_t g_sm_mu;
static jmp_buf worker_exit;
static FILE *worker_log;
static void sm_lock(void) {
    assert(!locked);
    locked = 1;
    event('(');
}
static void sm_unlock(void) {
    assert(locked);
    locked = 0;
    event(')');
}
static int ph_queued(void) {
    assert(locked);
    event('Q');
    return hotcue_queued;
}
static int fixture_wait(pthread_cond_t *cv, pthread_mutex_t *mu, const struct timespec *until) {
    assert(cv == &g_sm_cv && mu == &g_sm_mu && locked);
    assert(compare_timestamps(until->tv_sec, 10L, 1, 1, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(until->tv_nsec, 100L, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    waits++;
    fake_ms += 1001;
    event('W');
    return 0;
}
static long long drag_now_ms(void) { return fake_ms; }
static long long sm_world_sig(void) {
    event('L');
    return library_signature;
}
static long long sm_stems_sig(void) {
    event('S');
    return stems_signature;
}
static void ph_world_changed(void) {
    assert(locked);
    world_notifications++;
    event('H');
}
static void sm_pend_scan(void) {
    assert(!locked);
    pending_scans++;
    event('P');
}
static void sm_db_release_unused(FILE *log) {
    assert(log == worker_log && !locked);
    releases++;
    event('D');
}
static int sm_work_one(FILE *log) {
    assert(log == worker_log && !locked && events[event_count - 1] == 'D');
    event('J');
    worker_calls++;
    if (worker_calls == 2) {
        library_signature++;
        stems_signature++;
    }
    if (worker_calls >= worker_limit) longjmp(worker_exit, 1);
    return 1;
}
static int fixture_schedule(pthread_t thread, int policy, const struct sched_param *param) {
    (void) thread;
    assert(policy == SCHED_IDLE && param->sched_priority == 0);
    event('I');
    return schedule_result;
}
static FILE *azlog_open(const char *path, const char *mode) {
    assert(strcmp(path, "/tmp/azmod-stemsmark.log") == 0 && strcmp(mode, "a") == 0);
    event('O');
    return worker_log;
}
#define clock_gettime fixture_clock
#define pthread_cond_timedwait fixture_wait
#define pthread_setschedparam fixture_schedule
#include VOCAL_HOOKS_INC
#undef clock_gettime
#undef pthread_cond_timedwait
#undef pthread_setschedparam

static void equal_ns(long actual, long expected) {
    assert(compare_timestamps(actual, expected, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
}
static void reset_wave(void) {
    memset(g_wkey, 0, sizeof g_wkey);
    g_wkey_n = 2;
    g_wkey[0].ptr = 0x10000;
    g_wkey[0].deck = 0;
    g_wkey[1].ptr = 0x20000;
    g_wkey[1].deck = 1;
    g_upd_deck = -1;
    g_upd_bound = 0;
    g_upd_calls = 0;
    g_upd_skip = 0;
    g_upd_skipped = 0;
    g_in_root = 0;
    g_orig_wvupd = native_root;
    g_root_pass = 1;
    memset(g_built_pass, 0, sizeof g_built_pass);
    g_up_deck = -1;
    g_up_t0 = 0;
    g_up_lastend = 0;
    g_up_depth = 0;
    memset(g_up_ns, 0, sizeof g_up_ns);
    memset(g_up_n, 0, sizeof g_up_n);
    memset(g_up_max, 0, sizeof g_up_max);
    memset(g_up_gap_ns, 0, sizeof g_up_gap_ns);
    for (int i = 0; i < 4; i++) g_up_btn[i] = 1;
    fake_ns = 100;
    fallback_deck = -1;
    fallback_calls = 0;
    begin_count = commits = rendered_count = native_returned = 0;
    clear_events();
}
static void lookup_and_duplicates(void) {
    reset_wave();
    assert(azm_upd_lookup(0x10000, 0x30000) == 0 && !fallback_calls);
    assert(azm_upd_lookup(0x30000, 0x20000) == 1 && !fallback_calls);
    fallback_deck = 2;
    assert(azm_upd_lookup(0x30000, 0x40000) == 2 && fallback_calls == 1);
    assert(!azm_upd_duplicate(-1));
    assert(!azm_upd_duplicate(4));
    assert(!azm_upd_duplicate(0));
    g_in_root = 1;
    g_orig_wvupd = 0;
    assert(!azm_upd_duplicate(0));
    g_orig_wvupd = native_root;
    assert(!azm_upd_duplicate(0));
    assert(azm_upd_duplicate(0));
    assert(g_upd_skip && g_upd_skipped == 1);
    g_upd_skip = 0;
    g_root_pass++;
    assert(!azm_upd_duplicate(0));
    assert(!g_upd_skip);
}
static void update_order(void) {
    reset_wave();
    int previous = azm_upd_enter(0x10000, 0x30000, 0.25f);
    assert(previous == -1 && g_upd_deck == 0 && g_upd_calls == 1 && g_upd_bound == 1);
    assert(strcmp(events, "GBT") == 0);
    assert(begin_deck == 0 && begin_widget == 0x10000 && begin_position == 0x30000);
    assert(begin_zoom == 0.25f && begin_count == 1 && g_up_deck == 0);
    assert(!rendered_count);
    fake_ns = 130;
    azm_upd_exit(previous);
    assert(strcmp(events, "GBTCT") == 0 && commits == 1 && !commit_skip);
    assert(g_upd_deck == -1 && g_up_deck == -1 && !g_gridaa_ok);
    equal_ns(g_up_ns[0], 30);
    equal_ns(g_up_max[0], 30);
    equal_ns(g_up_lastend, 130);
    fake_ns = 150;
    previous = azm_upd_enter(0x10000, 0x30000, 0.25f);
    fake_ns = 160;
    azm_upd_exit(previous);
    equal_ns(g_up_ns[0], 40);
    equal_ns(g_up_max[0], 30);
    equal_ns(g_up_gap_ns[0], 20);
    assert(g_up_n[0] == 2);
    reset_wave();
    previous = azm_upd_enter(0x30000, 0x40000, 1.0f);
    assert(strcmp(events, "GFB") == 0 && begin_deck == -1 && g_up_deck == -1);
    azm_upd_exit(previous);
    assert(!g_upd_bound && g_upd_deck == -1);
    azm_upd_profile(4);
    assert(g_up_deck == -1);
}
static void nested_skipped_update(void) {
    reset_wave();
    g_in_root = 1;
    int outer = azm_upd_enter(0x10000, 0x30000, 1.0f);
    fake_ns = 110;
    int inner = azm_upd_enter(0x10000, 0x30000, 1.0f);
    assert(g_upd_skip && g_upd_skipped == 1);
    azm_upd_exit(inner);
    assert(commit_skip && g_upd_deck == 0 && !g_upd_skip);
    assert(g_up_deck == 0);
    equal_ns(g_up_t0, 100);
    fake_ns = 140;
    azm_upd_exit(outer);
    assert(!commit_skip && g_upd_deck == -1 && g_up_n[0] == 1);
    equal_ns(g_up_ns[0], 40);
}
static void nested_other_deck(void) {
    reset_wave();
    int outer = azm_upd_enter(0x10000, 0x30000, 1.0f);
    fake_ns = 110;
    int inner = azm_upd_enter(0x20000, 0x30000, 1.0f);
    fake_ns = 120;
    azm_upd_exit(inner);
    assert(g_up_deck == 0 && g_upd_deck == 0);
    equal_ns(g_up_t0, 100);
    fake_ns = 150;
    azm_upd_exit(outer);
    assert(g_up_n[0] == 1 && g_up_n[1] == 1);
    equal_ns(g_up_ns[0], 50);
    equal_ns(g_up_ns[1], 10);
}
static void bounded_scopes(void) {
    reset_wave();
    g_up_deck = 2;
    g_upd_skip = 1;
    g_up_t0 = 100;
    for (int i = 0; i < 9; i++) azm_upd_scope_begin();
    assert(g_up_depth == 9 && !g_upd_skip);
    g_up_deck = -1;
    g_up_t0 = 200;
    for (int i = 0; i < 9; i++) azm_upd_scope_end();
    assert(!g_up_depth && g_up_deck == 2 && g_upd_skip);
    equal_ns(g_up_t0, 100);
    azm_upd_scope_end();
    assert(g_up_deck == 2 && g_upd_skip);
}
static void renderer_exit(void) {
    reset_wave();
    long provider[2] = {100, 200};
    long save[4] = {22, 11, 100, 1};
    memset(g_ws_n, 0, sizeof g_ws_n);
    memset(g_ws_rns, 0, sizeof g_ws_rns);
    render_provider = provider;
    native_returned = 1;
    fake_ns = 130;
    azm_rw_exit((long) provider, save);
    assert(rendered_count == 1 && provider[0] == 11 && provider[1] == 22);
    assert(g_ws_n[1] == 1 && !g_ws_n[0]);
    equal_ns(g_ws_rns[1], 30);
    render_provider = 0;
    memset(save, 0, sizeof save);
    azm_rw_exit((long) provider, save);
    assert(rendered_count == 2 && provider[0] == 11 && provider[1] == 22);
    save[2] = 120;
    azm_rw_exit((long) provider, save);
    assert(g_ws_n[0] == 1);
    equal_ns(g_ws_rns[0], 10);
}
static __attribute__((noinline)) void trace_profile(void) {
    reset_wave();
    g_up_btn[2] = 0;
    azm_upd_profile(2);
    assert(g_up_btn[2] > 0 && g_up_btn[2] <= 12 && g_up_bt[2][0]);
    int count = g_up_btn[2];
    azm_upd_profile(2);
    assert(g_up_btn[2] == count);
}
static void worker_queue_and_cadence(void) {
    fake_ms = 999;
    pending_scans = 0;
    sm_check_pending();
    assert(!pending_scans);
    fake_ms = 1000;
    sm_check_pending();
    assert(pending_scans == 1);
    fake_ms = 1999;
    sm_check_pending();
    assert(pending_scans == 1);
    fake_ms = 2000;
    sm_check_pending();
    assert(pending_scans == 2);
    g_smq_n = 1;
    hotcue_queued = 0;
    clear_events();
    sm_wait_for_work();
    assert(strcmp(events, "()") == 0);
    g_smq_n = 0;
    hotcue_queued = 1;
    clear_events();
    sm_wait_for_work();
    assert(strcmp(events, "(Q)") == 0);
    hotcue_queued = 0;
    waits = 0;
    clear_events();
    sm_wait_for_work();
    assert(strcmp(events, "(QW)") == 0 && waits == 1 && !locked);
}
static void world_changes(void) {
    struct sm_world_watch watch = {10, 1000, 20};
    library_signature = 10;
    stems_signature = 20;
    world_notifications = 0;
    memset(g_smc, 0, sizeof g_smc);
    g_smc[0].st = 1;
    g_smc[2].st = 2;
    memset(g_smpc, 7, sizeof g_smpc);
    g_sm_dirty = g_sm_world = 0;
    fake_ms = 4000;
    clear_events();
    sm_check_world(&watch, 0);
    assert(!event_count);
    fake_ms = 4001;
    sm_check_world(&watch, 0);
    assert(strcmp(events, "LS") == 0);
    assert(g_smpc[0] == 7 && !g_sm_world && !g_sm_dirty);
    fake_ms = 7002;
    stems_signature = 21;
    sm_check_world(&watch, 0);
    for (size_t i = 0; i < sizeof g_smpc; i++) assert(!g_smpc[i]);
    assert(!g_smc[0].stale && !world_notifications);
    fake_ms = 10003;
    library_signature = 11;
    sm_check_world(&watch, 0);
    assert(g_smc[0].stale && !g_smc[1].stale && g_smc[2].stale);
    assert(g_sm_world && g_sm_dirty && world_notifications == 1 && !locked);
    FILE *log = tmpfile();
    assert(log);
    g_sm_budget = 1;
    sm_mark_world_changed(log);
    assert(g_sm_budget == 0);
    sm_mark_world_changed(log);
    assert(g_sm_budget == -1);
    rewind(log);
    char line[100];
    assert(fgets(line, sizeof line, log));
    assert(strcmp(line, "WORLD changed: statuses marked stale\n") == 0);
    assert(!fgets(line, sizeof line, log));
    fclose(log);
}
static void worker_loop(int logging) {
    worker_log = logging ? tmpfile() : 0;
    assert(!logging || worker_log);
    worker_calls = releases = waits = 0;
    worker_limit = 4;
    fake_ms = 20000 + logging * 20000;
    library_signature = 40;
    stems_signature = 50;
    schedule_result = logging ? 0 : 1;
    g_sm_idle_thread = 0;
    g_smq_n = hotcue_queued = 0;
    g_sm_budget = 1;
    clear_events();
    if (!setjmp(worker_exit)) sm_worker(0);
    assert(worker_calls == 4 && releases == 4 && waits == 4 && !locked);
    assert(g_sm_idle_thread == logging);
    assert(strncmp(events, "IOLS(QW)PDJ", 11) == 0);
    assert(strstr(events, "LS(H)DJ")); /* world check precedes connection release/work */
    if (worker_log) fclose(worker_log);
    worker_log = 0;
}
int main(void) {
    lookup_and_duplicates();
    update_order();
    nested_skipped_update();
    nested_other_deck();
    bounded_scopes();
    renderer_exit();
    trace_profile();
    worker_queue_and_cadence();
    world_changes();
    worker_loop(0);
    worker_loop(1);
    puts("vocal update hooks and stem worker: passed");
    return 0;
}
