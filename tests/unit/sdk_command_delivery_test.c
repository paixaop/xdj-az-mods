/* SPDX-License-Identifier: GPL-3.0-only */
/* Linux descriptor test with the production store, wrappers, table entries and
 * overlay wait. The native action is scripted; X can use a real display. */
#define _GNU_SOURCE
#include "sdk_data_engine.h"
#include "sdk_command_engine.h"
#include <assert.h>
#include <stdio.h>

#define CM_MAX 4
#define AZCM_LOADED 1
typedef struct {
    uintptr_t lo, hi;
    int state;
} cm_mod;
static cm_mod g_cm[CM_MAX];
static const char *const ids[] = {"one", "two", "three", "four"};
static int g_cm_n = CM_MAX, g_cm_loading = -1, g_sdk_in_init, g_cm_in_frame;
static _Thread_local int g_cm_on_ui;
static uintptr_t fixture_caller = 1;
#define CM_RA() fixture_caller
static cm_mod *cm_self(const char *id, uintptr_t caller, const char *service) {
    (void) service;
    for (unsigned i = 0; id && caller == 1 && i < CM_MAX; ++i)
        if (!strcmp(id, ids[i])) return &g_cm[i];
    return NULL;
}
static int cm_command_code(cm_mod *mod, uintptr_t address) {
    return address >= mod->lo && address < mod->hi;
}
static struct st_engine g_st = ST_ENGINE_INIT;
#include SDK_OWNER_INC
#include "sdk_command.c"
#include SDK_SUBMIT_INC
#include "sdk_data_callbacks.c"
#include SDK_LIBRARY_INC

#ifdef AZ_COMMAND_REAL_X
#include <X11/Xlib.h>
struct overlay_ctx {
    Display *dpy;
};
#else
struct overlay_ctx {
    int dpy;
};
#define ConnectionNumber(fd) (fd)
#define QueuedAlready 0
static int XEventsQueued(int fd, int mode) {
    (void) fd;
    (void) mode;
    return 0;
}
#endif
static long now_ms(void) { return (long) (scmd_monotonic(NULL) / 1000000); }
#include SDK_OVERLAY_INC

extern const struct azexit_service *fixture_service(void);
extern int fixture_activity_fd(void);
extern uint64_t fixture_token(unsigned deck);
extern unsigned fixture_stop(unsigned deck);
extern int fixture_claim(unsigned deck, uint64_t token);
extern void fixture_finish(unsigned deck, uint64_t token, unsigned outcome, int reason, unsigned settled);
extern void fixture_settle(unsigned deck, uint64_t token);
extern void fixture_advertise(int enabled);
extern void fixture_arbitration(void);

struct client {
    int owner, terminal, released, retire;
    az_command_request request;
    az_command_completion saved;
};
static pthread_t ui;
static struct overlay_ctx overlay;
static const az_library_v1 *library;

static void completed(const az_command_completion *value, void *user) {
    struct client *client = user;
    const char *id = ids[client->owner];
    assert(pthread_equal(ui, pthread_self()) && !g_sdk_in_init);
    assert(value->result.request_id == client->request.request_id && value->version == AZ_DATA_VERSION);
    az_command_result reply = {.size = sizeof reply};
    assert(library->command_submit_async(id, &client->request, completed, user, &reply) == AZ_OK);
    assert(reply.outcome == value->result.outcome && reply.reason == value->result.reason);
    if (value->kind == AZ_COMMAND_COMPLETION_TERMINAL) {
        assert(!client->terminal++ && !client->released);
        client->saved = *value;
        if (client->retire) {
            sdk_command_retire_owner(client->owner);
            return;
        }
        assert(library->command_acknowledge(id, value->result.request_id) == AZ_OK);
        assert(library->command_acknowledge(id, value->result.request_id) == AZ_OK);
        assert(!client->released);
    } else {
        assert(value->kind == AZ_COMMAND_COMPLETION_RELEASED && value->settled == 1);
        assert(client->terminal == 1 && !client->released++);
        assert(!memcmp(&client->saved.result, &value->result, sizeof value->result));
        assert(library->command_acknowledge(id, value->result.request_id) == AZ_OK);
    }
}
static void prepare(struct client *client, int owner, unsigned deck, uint64_t id, uint64_t duration_ns) {
    memset(client, 0, sizeof *client);
    client->owner = owner;
    client->request = (az_command_request) {
        .size = sizeof client->request, .version = AZ_DATA_VERSION, .deck = deck, .operation = AZ_CMD_LOOP_EXIT, .flags = AZ_COMMAND_NATIVE_LOAD, .request_id = id, .expected_load_generation = deck + 10, .deadline_ns = scmd_monotonic(NULL) + duration_ns};
}
static int submit(struct client *client) {
    az_command_result out = {.size = sizeof out};
    return library->command_submit_async(ids[client->owner], &client->request, completed, client, &out);
}
static void wait_activity(void) {
    struct pollfd p = {fixture_activity_fd(), POLLIN, 0};
    assert(poll(&p, 1, 3000) == 1);
    sdk_drain_fd(p.fd);
}
static uint64_t wait_token(unsigned deck, uint64_t previous) {
    for (;;) {
        uint64_t token = fixture_token(deck);
        if (token && token != previous) return token;
        wait_activity();
    }
}
static unsigned drain_ui(void) {
    unsigned ready = ov_wait(&overlay, 2500000, now_ms());
    assert(ready & OV_WAKE_COMMAND);
    return sdk_command_ui_drain(8);
}
static void cleanup(struct client *client) {
    while (!client->released) (void) drain_ui();
    assert(library->command_acknowledge(ids[client->owner], client->request.request_id) == AZ_STALE);
}
static int refuse_binding(const struct azexit_binding *binding) {
    (void) binding;
    return AZ_UNSUPPORTED;
}
static void mixed_versions(const struct azexit_service *good) {
    struct azexit_service peer = *good;
    struct azexit_delivery_v2 tail = *peer.delivery;
    peer.delivery = &tail;
    for (unsigned i = 0; i < 5; ++i) {
        peer.size = i == 0 ? AZEXIT_SERVICE_V1_SIZE : sizeof peer;
        tail = *good->delivery;
        if (i == 1) peer.delivery = NULL;
        else peer.delivery = &tail;
        if (i == 2) tail.size = 2 * sizeof(uint32_t);
        if (i == 3) tail.version--;
        if (i == 4) tail.bind = refuse_binding;
        assert(sdk_command_start(&peer) != AZ_OK);
        assert(sdk_command_capabilities() == 0);
        struct client c;
        prepare(&c, 0, 0, 1, UINT64_C(5000000000));
        assert(submit(&c) == AZ_UNSUPPORTED);
        az_command_result out = {.size = sizeof out};
        assert(library->command_submit("one", &c.request, &out) == AZ_UNSUPPORTED &&
               out.outcome == AZ_COMMAND_REJECTED && out.request_id == 1 && out.reason == AZ_UNSUPPORTED);
        assert(g_cm_core.command_submit("one", &c.request, &out) == AZ_UNSUPPORTED);
        out.request_id = 100;
        c.request.reserved = 1;
        assert(library->command_submit_async("one", &c.request, completed, &c, &out) == AZ_INVALID &&
               out.request_id == 100 && !g_sdk_commands.next_token);
    }
}
struct delayed_finish {
    unsigned deck;
    uint64_t token;
};
static void *complete_after_wait(void *opaque) {
    const struct delayed_finish *finish = opaque;
    struct timespec delay = {.tv_nsec = 20000000};
    nanosleep(&delay, NULL);
    fixture_finish(finish->deck, finish->token, AZ_COMMAND_APPLIED, AZ_OK, 1);
    return NULL;
}
int main(void) {
    ui = pthread_self();
    g_cm_on_ui = 1;
    for (int i = 0; i < CM_MAX; ++i) g_cm[i] = (cm_mod) {1, UINTPTR_MAX, AZCM_LOADED};
    assert(AZ_HAS(&g_cm_core, library_service));
    library = g_cm_core.library_service(AZ_LIBRARY_ABI);
    assert(!sdk_library_service(AZ_LIBRARY_ABI + 1) && AZ_LIB_HAS(library, command_submit_async) &&
           AZ_LIB_HAS(library, command_acknowledge) && library->command_submit == g_cm_core.command_submit);
    const struct azexit_service *peer = fixture_service();
    assert(!peer->mask());
    mixed_versions(peer);
    fixture_arbitration();
    assert(sdk_command_start(peer) == AZ_OK);
#ifdef AZ_COMMAND_REAL_X
    overlay.dpy = XOpenDisplay(NULL);
    assert(overlay.dpy);
#else
    overlay.dpy = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(overlay.dpy >= 0);
#endif

    const uint64_t long_budget = UINT64_C(5000000000);
    struct client first, second;
    prepare(&first, 0, 0, 1, long_budget);
    prepare(&second, 1, 0, 1, long_budget);
    assert(submit(&first) == AZ_OK && submit(&second) == AZ_OK);
    uint64_t a = wait_token(0, 0);
    assert(fixture_claim(0, a) == AZ_OK);
    fixture_finish(0, a, AZ_COMMAND_APPLIED, AZ_OK, 1);
    uint64_t b = wait_token(0, a);
    assert(fixture_claim(0, b) == AZ_OK && !first.terminal); /* no UI or acknowledgement yet */
    fixture_advertise(0);
    assert(submit(&first) == AZ_OK && submit(&second) == AZ_OK);
    fixture_finish(0, b, AZ_COMMAND_APPLIED, AZ_OK, 1);
    cleanup(&first);
    cleanup(&second);
    fixture_advertise(1);

    struct client asleep;
    prepare(&asleep, 0, 0, 2, long_budget);
    assert(submit(&asleep) == AZ_OK);
    a = wait_token(0, b);
    assert(fixture_claim(0, a) == AZ_OK);
    struct delayed_finish finish = {0, a};
    pthread_t producer;
    assert(!pthread_create(&producer, NULL, complete_after_wait, &finish));
    long start = now_ms();
    cleanup(&asleep);
    assert(compare_timestamps(now_ms(), start + 2000, 1000, 1000, 1) == AZ_TIME_BEFORE);
    assert(!pthread_join(producer, NULL));

    struct client timed;
    prepare(&timed, 0, 1, 3, UINT64_C(150000000));
    assert(submit(&timed) == AZ_OK);
    a = wait_token(1, 0);
    assert(fixture_claim(1, a) == AZ_OK);
    while (!timed.terminal) (void) drain_ui();
    assert(timed.saved.result.outcome == AZ_COMMAND_UNCERTAIN && timed.saved.result.reason == AZ_STALE &&
           !timed.saved.settled && !timed.released);
    while (!(fixture_stop(1) & AZEXIT_STOP_DEADLINE)) wait_activity();
    fixture_settle(1, a);
    cleanup(&timed);

    struct client early;
    prepare(&early, 0, 1, 4, long_budget);
    assert(submit(&early) == AZ_OK);
    a = wait_token(1, a);
    assert(fixture_claim(1, a) == AZ_OK);
    fixture_settle(1, a);
    while (fixture_token(1)) wait_activity();
    fixture_finish(1, a, AZ_COMMAND_UNCERTAIN, AZ_FAILED, 0);
    cleanup(&early);
    assert(early.saved.result.outcome == AZ_COMMAND_UNCERTAIN && early.saved.settled);

    struct client initializing;
    prepare(&initializing, 2, 2, 1, long_budget);
    sdk_command_owner_init(2);
    g_cm[2].state = 0;
    g_sdk_in_init = 1;
    g_cm_loading = 2;
    assert(submit(&initializing) == AZ_OK);
    a = wait_token(2, 0);
    assert(fixture_claim(2, a) == AZ_OK);
    fixture_finish(2, a, AZ_COMMAND_APPLIED, AZ_OK, 1);
    while (fixture_token(2)) wait_activity();
    assert(sdk_command_ui_drain(8) == 0);
    g_sdk_in_init = 0;
    g_cm_loading = -1;
    g_cm[2].state = AZCM_LOADED;
    sdk_command_owner_ready(2);
    cleanup(&initializing);

    struct client retiring;
    prepare(&retiring, 2, 2, 2, long_budget);
    retiring.retire = 1;
    assert(submit(&retiring) == AZ_OK);
    a = wait_token(2, 0);
    assert(fixture_claim(2, a) == AZ_OK);
    fixture_finish(2, a, AZ_COMMAND_UNCERTAIN, AZ_FAILED, 0);
    while (!retiring.terminal) (void) drain_ui();
    assert(!retiring.released && sdk_command_ui_drain(8) == 0 && fixture_token(2) == a);
    sdk_command_owner_init(2);
    sdk_command_owner_ready(2);
    struct client replacement;
    prepare(&replacement, 2, 2, 2, long_budget);
    assert(submit(&replacement) == AZ_OK);
    fixture_settle(2, a);
    b = wait_token(2, a);
    assert(fixture_claim(2, b) == AZ_OK);
    fixture_finish(2, b, AZ_COMMAND_APPLIED, AZ_OK, 1);
    cleanup(&replacement);

    /* Accumulate twelve notifications without pumping the UI. The native
     * mailbox retires after each command even though public results are held. */
    struct client batch[12];
    a = 0;
    for (unsigned i = 0; i < 12; ++i) {
        prepare(&batch[i], (int) (i / 4), 3, 100 + i, long_budget);
        assert(submit(&batch[i]) == AZ_OK);
        a = wait_token(3, a);
        assert(fixture_claim(3, a) == AZ_OK);
        fixture_finish(3, a, AZ_COMMAND_APPLIED, AZ_OK, 1);
        while (fixture_token(3)) wait_activity();
    }
    assert(drain_ui() == 8);
    unsigned terminal_count = 0;
    for (unsigned i = 0; i < 12; ++i) terminal_count += batch[i].terminal;
    assert(terminal_count == 8);
    struct client over_quota;
    prepare(&over_quota, 0, 3, 120, long_budget);
    assert(submit(&over_quota) == AZ_LIMIT); /* RELEASED is still pending */
    for (unsigned i = 0; i < 12; ++i) cleanup(&batch[i]);

#ifdef AZ_COMMAND_REAL_X
    XCloseDisplay(overlay.dpy);
#else
    close(overlay.dpy);
#endif
    atomic_store(&g_sdk_command_running, 0);
    sdk_command_wake();
    assert(!pthread_join(g_sdk_command_thread, NULL));
    puts("async commands: bound transport, deadline arbitration, owner gates and descriptor-driven cleanup passed");
}
