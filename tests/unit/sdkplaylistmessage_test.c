/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "native_abi_fixture.h"
#include "azsources.h"
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include "playlist_declarations.inc"

static _Thread_local uintptr_t t_pst_server;
static _Thread_local int t_pst_own;
static _Thread_local unsigned t_pst_slots;
static _Atomic int g_pst_armed, g_rs_on, g_ls_armed;
static int g_sm_on, g_sdk_sources_on, g_ep147_build = 2;
static int expected_server, expected_message, base_own, execute_result;
static int chain_result, chain_race, log_disabled, log_fd = -1;
static unsigned chain_media, executes, syncs, chains, originals;
static bool throwing;
static char trace[80];
static size_t trace_size;
static void event(char value, void *server) {
    assert(server == &expected_server);
    assert(trace_size + 1 < sizeof trace);
    trace[trace_size++] = value;
    trace[trace_size] = 0;
}
static int mlserver_chain(void *server, uintptr_t *service, uintptr_t *library, uintptr_t *database, unsigned *media) {
    assert(server == &expected_server);
    ++chains;
    *service = 101;
    *library = 102;
    *database = 103;
    *media = chain_media;
    if (chain_race) atomic_store(&g_mv_state, MV_IDLE); /* timeout wins before admission CAS */
    return chain_result;
}
static FILE *azlog_open(const char *path, const char *mode) {
    assert(!strcmp(path, "/tmp/azmod-drag.log") && !strcmp(mode, "a") && log_fd == -1);
    if (log_disabled) return NULL;
    FILE *stream = tmpfile();
    assert(stream);
    log_fd = fcntl(fileno(stream), F_DUPFD_CLOEXEC, 0);
    assert(log_fd >= 0);
    return stream;
}
static void expect_log(const char *expected) {
    assert(log_fd >= 0);
    char actual[256] = {0};
    ssize_t count = pread(log_fd, actual, sizeof actual - 1, 0);
    assert(count >= 0 && !strcmp(actual, expected));
    close(log_fd);
    log_fd = -1;
}
static int mv_execute(void *server, FILE *log) {
    assert(atomic_load(&g_mv_state) == MV_TAKEN && t_pst_own == base_own + 1);
    assert((log != NULL) == !log_disabled);
    ++executes;
    event('E', server);
    return execute_result;
}
static void mv_pdb_sync(void *server) {
    assert(g_mv.rc == 0 && atomic_load(&g_mv_state) == MV_TAKEN && t_pst_own == base_own);
    ++syncs;
    event('Y', server);
}
static void pst_lib_service(void *server) {
    assert(t_pst_server == (uintptr_t) server && !t_pst_slots && t_pst_own == base_own);
    event('P', server);
}
static void mm_note(void *server) { event('M', server); }
static void fc_refresh_service(void *server) {
    assert(t_pst_own == base_own);
    event('C', server);
}
static void ls_service(void *server) {
    assert(t_pst_own == base_own + 1);
    event('L', server);
}
static void original(void *server, void *message, unsigned parameter) {
    assert(message == &expected_message && parameter == 37);
    assert(t_pst_server == (uintptr_t) server && !t_pst_slots && t_pst_own == base_own);
    ++originals;
    event('O', server);
    t_pst_slots = 32;
    if (throwing) fixture_raise();
}
static azsdk_source_message_original g_orig_mlserver_onmsg = original;
void azsdk_sources_forward_owned_message(int build, void *server, void *message, unsigned parameter, azsdk_source_message_original forward) {
    assert(build == g_ep147_build && forward == original && g_sdk_sources_on);
    event('F', server);
    forward(server, message, parameter);
}
int azsdk_sources_observe_owned_message(void *server, void *out, size_t size) {
    assert(!out && !size && g_sdk_sources_on && t_pst_slots == 32 && trace[trace_size - 1] == 'O');
    event('V', server);
    return AZS_OK;
}
static void pst_msg_done(void *server) {
    assert(t_pst_slots == 32 && t_pst_server == (uintptr_t) server && t_pst_own == base_own);
    event('D', server);
}
#include "playlist_functions.inc"

static void reset(void) {
    assert(log_fd == -1);
    chain_result = chain_race = log_disabled = execute_result = 0;
    executes = syncs = chains = originals = 0;
    trace_size = 0;
    trace[0] = 0;
    g_mv = (__typeof__(g_mv)) {.op = MV_OP_MOVE, .pl = 11, .cid = 12, .from = 3, .to = 7, .media = 9};
    chain_media = g_mv.media;
    atomic_store(&g_mv_state, MV_ARMED);
    base_own = t_pst_own = 4;
}
static void log_cases(void) {
    static const struct {
        int operation, result;
        const char *text;
    } cases[] = {
        {MV_OP_TARGET, -7, "TARGET pl=11 rc=-7\n"},
        {MV_OP_ADD, 0, "ADD pl=11 cid=12 rc=0 (appended and read back)\n"},
        {MV_OP_ADD, -7, "ADD pl=11 cid=12 rc=-7 (NOTHING WRITTEN)\n"},
        {MV_OP_REMOVE, 0, "REMOVE pl=11 cid=12 seq 3 rc=0 (removed and read back)\n"},
        {MV_OP_REMOVE, -7, "REMOVE pl=11 cid=12 seq 3 rc=-7 (NOTHING WRITTEN)\n"},
        {MV_OP_MOVE, 0, "COMMIT pl=11 cid=12 3->7 rc=0 (written and read back)\n"},
        {MV_OP_MOVE, -7, "COMMIT pl=11 cid=12 3->7 rc=-7 (NOTHING WRITTEN)\n"}};
    reset();
    mv_log_result(NULL);
    assert(log_fd == -1);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        g_mv.op = cases[i].operation;
        g_mv.rc = cases[i].result;
        mv_log_result(azlog_open("/tmp/azmod-drag.log", "a"));
        expect_log(cases[i].text);
    }
}
static void admission_cases(void) {
    for (unsigned failure = 0; failure < 4; ++failure) {
        reset();
        if (failure == 0) atomic_store(&g_mv_state, MV_DONE);
        if (failure == 1) chain_result = -1;
        if (failure == 2) ++chain_media;
        if (failure == 3) chain_race = 1;
        mv_service_armed(&expected_server);
        assert(!executes && !syncs && log_fd == -1 && t_pst_own == base_own);
        assert(chains == (failure != 0));
        assert(atomic_load(&g_mv_state) == (failure == 0 ? MV_DONE : failure == 3 ? MV_IDLE
                                                                                  : MV_ARMED));
    }
    for (unsigned read = 0; read < 2; ++read)
        for (unsigned failed = 0; failed < 2; ++failed)
            for (log_disabled = 0; log_disabled < 2; ++log_disabled) {
                int disabled = log_disabled;
                reset();
                log_disabled = disabled;
                g_mv.op = read ? MV_OP_TARGET : MV_OP_MOVE;
                execute_result = failed ? -7 : 0;
                mv_service_armed(&expected_server);
                assert(executes == 1 && syncs == (!read && !failed) && g_mv.rc == execute_result);
                assert(atomic_load(&g_mv_state) == MV_DONE && t_pst_own == base_own);
                assert(!strcmp(trace, !read && !failed ? "EY" : "E"));
                if (!disabled) {
                    char expected[160];
                    if (read) snprintf(expected, sizeof expected, "TARGET pl=11 rc=%d\n", execute_result);
                    else snprintf(expected, sizeof expected, "COMMIT pl=11 cid=12 3->7 rc=%d%s\n", execute_result, failed ? " (NOTHING WRITTEN)" : " (written and read back)");
                    expect_log(expected);
                }
                mv_service_armed(&expected_server);
                assert(executes == 1); /* a completed request cannot execute twice */
            }
}
static void invoke_hook(void *unused) {
    (void) unused;
    mlserver_onmsg_hook(&expected_server, &expected_message, 37);
}
static void hook_cases(void) {
    for (unsigned flags = 0; flags < 32; ++flags)
        for (unsigned unwind = 0; unwind < 2; ++unwind) {
            reset();
            log_disabled = 1;
            throwing = unwind;
            g_pst_armed = !!(flags & 1);
            g_sm_on = !!(flags & 2);
            g_rs_on = !!(flags & 4);
            g_ls_armed = !!(flags & 8);
            g_sdk_sources_on = !!(flags & 16);
            t_pst_server = 991;
            t_pst_slots = 13;
            unsigned deletions = fixture_exception_deletions;
            assert(fixture_native_try(invoke_hook, NULL) == !unwind);
            assert(fixture_exception_deletions == deletions + unwind);
            assert(t_pst_server == 991 && t_pst_slots == 13 && t_pst_own == base_own);
            assert(executes == 1 && syncs == 1 && originals == 1 && atomic_load(&g_mv_state) == MV_DONE);
            char expected[80] = "";
            if (g_pst_armed) strcat(expected, "P");
            if (g_sm_on || g_rs_on) strcat(expected, "M");
            strcat(expected, "EYC");
            if (g_ls_armed) strcat(expected, "L");
            if (g_sdk_sources_on) strcat(expected, "F");
            strcat(expected, "O");
            if (!unwind) {
                if (g_sdk_sources_on) strcat(expected, "V");
                strcat(expected, "D");
            }
            assert(!strcmp(trace, expected));
        }
}
int main(void) {
    log_cases();
    admission_cases();
    hook_cases();
    puts("PASS playlist messages: exact edit logs, stick/timeout admission, completion and sync policy, feature ordering, native unwind and TLS restoration");
    return 0;
}
