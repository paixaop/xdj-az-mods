/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by sdk_command.c. Native execution uses a scalar mailbox, so neither
 * the engine mutex nor a mod callback ever runs inside the native owner hook. */
#include "../common/azexit.h"
#include "../common/azcuecommands.h"
#if !defined(AZALTSTACK_H) && !defined(az_thread_create)
/* sdk_command.c is also extracted into host fixtures without azmod.c's
 * unity-build prologue. Production includes azaltstack.h before this file. */
#define AZSDK_THREAD_START_(base, name) base##name
#define AZSDK_THREAD_START(base, name) AZSDK_THREAD_START_(base, name)
static int az_thread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    return AZSDK_THREAD_START(pthread_, create)(thread, attr, fn, arg);
}
#undef AZSDK_THREAD_START
#undef AZSDK_THREAD_START_
#endif
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#endif
#include <unistd.h>
static _Atomic(const struct azexit_service *) g_sdk_exit_service;
static _Atomic(const struct azexit_delivery_v2 *) g_sdk_exit_bound;
static int g_sdk_command_fd = -1;
static int g_sdk_ui_fd = -1;
static int g_sdk_timer_fd = -1;
static pthread_t g_sdk_command_thread;
static _Atomic int g_sdk_command_running;

static int sdk_eventfd_create(void) {
#if defined(__linux__)
    return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#else
    return -1;
#endif
}
static int sdk_timerfd_create(void) {
#if defined(__linux__)
    return timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
#else
    return -1;
#endif
}

static void sdk_signal_fd(int fd) {
    if (fd < 0) return;
    uint64_t one = 1;
    while (write(fd, &one, sizeof one) < 0 && errno == EINTR) {}
}
static void sdk_drain_fd(int fd) {
    if (fd < 0) return;
    uint64_t value;
    ssize_t count;
    do count = read(fd, &value, sizeof value);
    while (count == sizeof value || (count < 0 && errno == EINTR));
}
static void sdk_command_wake(void) { sdk_signal_fd(g_sdk_command_fd); }
static void sdk_ui_wake(void) { sdk_signal_fd(g_sdk_ui_fd); }
/* Keep the earlier private exports available to already loaded companions. */
void azsdk_exit_command_wake(void) { sdk_command_wake(); }
__attribute__((weak)) void azsdk_cue_commands_drive(void) {}
static int sdk_exit_has_cues(const struct azexit_service *api) {
    return api && api->size >= AZEXIT_SERVICE_CUE_SIZE && api->commands && api->submit_cue && api->target_cue;
}
static const struct azexit_delivery_v2 *sdk_exit_delivery(const struct azexit_service *api) {
    if (!api || api->version != 1 || api->size < AZEXIT_SERVICE_DELIVERY_SIZE) return NULL;
    const struct azexit_delivery_v2 *d = api->delivery;
    return d && d->size >= sizeof *d && d->version == AZEXIT_DELIVERY_VERSION && d->event_fd &&
                   d->consume && d->reclaim && d->control && d->bind && d->reserve && d->close_local && d->drive && d->ready
               ? d
               : NULL;
}
static uint64_t sdk_exit_operations(const struct azexit_service *api) {
    const struct azexit_delivery_v2 *bound = atomic_load_explicit(&g_sdk_exit_bound, memory_order_acquire);
    if (!bound || bound != sdk_exit_delivery(api)) return 0;
    if (api->size >= AZEXIT_SERVICE_COMMAND_SIZE && api->commands) {
        uint64_t basic = (UINT64_C(1) << AZ_CMD_LOOP_EXIT) | (UINT64_C(1) << AZ_CMD_RELOOP);
        uint64_t loops = ((UINT64_C(1) << (AZ_CMD_LOOP_ADJUST_OUT + 1)) - 2) & ~basic;
        uint64_t supported = api->submit_command ? basic : api->submit ? UINT64_C(1) << AZ_CMD_LOOP_EXIT
                                                                       : 0;
        if (api->size >= AZEXIT_SERVICE_LOOP_SIZE && api->submit_loop) supported |= loops;
        if (sdk_exit_has_cues(api)) supported |= SCMD_ALL_COMMANDS & ~(loops | basic);
        return api->commands() & supported;
    }
    return api->mask && api->submit && api->mask() ? UINT64_C(1) << AZ_CMD_LOOP_EXIT : 0;
}
static uint64_t sdk_command_capabilities(void) {
    return sdk_exit_operations(atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire));
}
static int sdk_exit_arguments(void *ctx, const az_command_request *r, const struct scmd_target *target) {
    (void) ctx;
    (void) target;
    if (r->operation < AZ_CMD_LOOP_IN || r->operation > AZ_CMD_CUE_COLOR ||
        !(r->flags & AZ_COMMAND_NATIVE_LOAD)) return AZ_UNSUPPORTED;
    if (r->quantization != AZ_QUANTIZE_NATIVE) return AZ_INVALID;
    /* Native cue adapters own the operation-specific selection/unit contract. */
    if (r->operation > AZ_CMD_LOOP_ADJUST_OUT) return AZ_OK;
    if (r->flags != AZ_COMMAND_NATIVE_LOAD) return AZ_INVALID;
    if (r->expected_revision || r->quantization != AZ_QUANTIZE_NATIVE || r->classification || r->entry_id ||
        r->rgb || r->comment[0]) return AZ_INVALID;
    switch (r->operation) {
    case AZ_CMD_LOOP_LENGTH: {
        uint32_t packed;
        if (r->position_s != 0 || r->end_s != 0 || !azexit_pack_beat(r->beats, &packed)) return AZ_INVALID;
        break;
    }
    case AZ_CMD_LOOP_MOVE:
    case AZ_CMD_LOOP_ADJUST_IN:
        if (!isfinite(r->position_s) || r->position_s == 0 || !isfinite(r->position_s * 1000.0) ||
            r->end_s != 0 || r->beats != 0) return AZ_INVALID;
        break;
    case AZ_CMD_LOOP_ADJUST_OUT:
        if (!isfinite(r->end_s) || r->end_s == 0 || !isfinite(r->end_s * 1000.0) ||
            r->position_s != 0 || r->beats != 0) return AZ_INVALID;
        break;
    default:
        if (r->position_s != 0 || r->end_s != 0 || r->beats != 0) return AZ_INVALID;
    }
    return AZ_OK;
}
static int sdk_exit_begin(void *ctx, const az_command_request *r, struct scmd_target *out) {
    const struct azexit_service *api = ctx;
    uint64_t commands = sdk_exit_operations(api);
    if (commands) commands &= api->delivery->ready(r->deck);
    if (!(commands & (UINT64_C(1) << r->operation))) return AZ_UNSUPPORTED;
    uint64_t generation;
    int rc = api->target(r->deck, &generation);
    if (rc != AZ_OK) return rc;
    struct scmd_target target = {.load_generation = generation, .commands = commands, .valid = SCMD_TARGET_LOAD, .loaded = 1};
    if (r->operation > AZ_CMD_LOOP_ADJUST_OUT) {
        if (!sdk_exit_has_cues(api)) return AZ_UNSUPPORTED;
        struct azexit_cue_target cue = {0};
        rc = api->target_cue(r->deck, &cue);
        if (rc != AZ_OK) {
            if (rc == AZ_BUSY || rc == AZ_PENDING) *out = target;
            return rc;
        }
        if (cue.revision_valid > 1 || cue.reserved) return AZ_FAILED;
        uint64_t cue_commands = target.commands & ~((UINT64_C(1) << (AZ_CMD_LOOP_ADJUST_OUT + 1)) - 2);
        target.persistent_commands = cue.persistent_commands & cue_commands;
        target.revision_commands = cue.revision_commands & cue_commands;
        /* Collection revisions and transient DSP receipt counters are distinct
         * namespaces. Only operations fenced by this collection bind its revision. */
        uint64_t operation_bit = UINT64_C(1) << r->operation;
        if (cue.revision_valid && cue.revision &&
            (operation_bit & (target.revision_commands | target.persistent_commands))) {
            target.revision = cue.revision;
            target.valid |= SCMD_TARGET_REVISION;
        }
    }
    *out = target;
    return AZ_OK;
}
static int sdk_exit_dispatch(void *ctx, const az_command_request *r, uint64_t token, struct scmd_completion *out) {
    const struct azexit_service *api = ctx;
    struct azexit_request request = {token, r->expected_load_generation, r->deadline_ns, r->deck, 0};
    int rc = AZ_UNSUPPORTED;
    if (r->operation > AZ_CMD_LOOP_ADJUST_OUT && sdk_exit_has_cues(api)) {
        struct azexit_cue_request cue = {.size = sizeof cue, .operation = r->operation, .identity = request, .expected_revision = r->expected_revision, .flags = r->flags, .classification = r->classification, .entry_id = r->entry_id, .rgb = r->rgb, .position_s = r->position_s, .end_s = r->end_s, .beats = r->beats};
        memcpy(cue.comment, r->comment, sizeof cue.comment);
        rc = api->submit_cue(&cue);
    } else if (r->operation <= AZ_CMD_LOOP_ADJUST_OUT && r->operation != AZ_CMD_LOOP_EXIT && r->operation != AZ_CMD_RELOOP &&
               api->size >= AZEXIT_SERVICE_LOOP_SIZE && api->submit_loop) {
        struct azexit_loop_request loop = {sizeof loop, r->operation, request, r->position_s, r->end_s, r->beats};
        rc = api->submit_loop(&loop);
    } else if (api->size >= AZEXIT_SERVICE_COMMAND_SIZE && api->commands && api->submit_command)
        rc = api->submit_command(&request, r->operation);
    else if (r->operation == AZ_CMD_LOOP_EXIT) rc = api->submit(&request);
    (void) out;
    return rc;
}
static void sdk_exit_end(void *ctx, uint32_t deck) {
    (void) ctx;
    (void) deck;
}
static struct scmd_adapter sdk_exit_adapter(const struct azexit_service *api) {
    return (struct scmd_adapter) {(void *) api, sdk_exit_arguments, sdk_exit_begin, sdk_exit_dispatch, sdk_exit_end};
}

static struct azexit_receipt sdk_exit_identity(const struct scmd_record *r, uint32_t kind) {
    return (struct azexit_receipt) {
        .size = sizeof(struct azexit_receipt), .version = AZEXIT_DELIVERY_VERSION, .kind = kind, .deck = r->request.deck, .token = r->token, .load_generation = r->request.expected_load_generation, .lifetime_generation = r->native_lifetime};
}
/* All receipt projection and reservation return is serialized with admission.
 * Terminal values, settlement and native resource retirement are independent. */
static void sdk_exit_project(struct scmd_record *r, const struct azexit_receipt *value) {
    if (!r || value->size < sizeof *value || value->version != AZEXIT_DELIVERY_VERSION ||
        value->deck != r->request.deck || value->token != r->token ||
        value->load_generation != r->request.expected_load_generation || value->settled > 1 ||
        (r->native_lifetime && value->lifetime_generation && r->native_lifetime != value->lifetime_generation))
        return;
    if (value->kind == AZEXIT_RECEIPT_TERMINAL) {
        struct scmd_completion completion = {
            value->outcome, value->reason, value->load_generation, value->revision, value->settled};
        int deadline = compare_timestamps(value->observed_ns, r->request.deadline_ns, 1000000000, 1000000000, 1);
        int expired = value->outcome == AZ_COMMAND_CANCELLED && value->reason == AZ_STALE &&
                      value->settled && !value->revision;
        if ((!expired && !scmd_completion_valid(&completion)) ||
            (value->outcome == AZ_COMMAND_APPLIED && (!value->observed_ns || deadline != AZ_TIME_BEFORE))) return;
        if (r->phase != SCMD_FINAL) {
            r->settled |= value->settled;
            scmd_finish(&g_sdk_commands, r, value->outcome, value->reason, value->revision,
                        expired ? SCMD_DETAIL_DEADLINE : SCMD_DETAIL_NONE, value->observed_ns, 1);
        }
    } else if (value->kind == AZEXIT_RECEIPT_SETTLED && value->settled) r->settled = 1;
    else if (value->kind == AZEXIT_RECEIPT_RESOURCES_RETIRED && value->settled) r->native_retired = 1;
    else return;
    if (value->lifetime_generation) r->native_lifetime = value->lifetime_generation;
}
static void sdk_exit_join(struct scmd_record *r) {
    if (r->phase == SCMD_FINAL && !r->pinned && r->awaiting && r->settled && r->native_retired) {
        struct azexit_receipt identity = sdk_exit_identity(r, AZEXIT_RECEIPT_RESOURCES_RETIRED);
        if (g_sdk_commands.delivery->reclaim(&identity) == AZ_OK) {
            r->awaiting = 0;
            scmd_clear_barrier(&g_sdk_commands, r);
        }
    }
    if (r->retired && !r->callback_active) {
        r->callback = NULL;
        r->callback_user = NULL;
        if (r->phase == SCMD_FINAL && !r->awaiting && !r->pinned) memset(r, 0, sizeof *r);
    } else (void) scmd_move_to_pending_locked(&g_sdk_commands, r);
}
static void sdk_exit_close_local(struct scmd_record *r, int reason, uint64_t now) {
    struct azexit_receipt value = sdk_exit_identity(r, r->phase == SCMD_FINAL ? AZEXIT_RECEIPT_SETTLED : AZEXIT_RECEIPT_TERMINAL);
    value.settled = 1;
    value.observed_ns = now;
    value.outcome = AZ_COMMAND_REJECTED;
    value.reason = scmd_error_status(reason);
    (void) g_sdk_commands.delivery->close_local(&value);
}
static int sdk_exit_control(struct scmd_record *r, uint32_t reasons, uint64_t now) {
    struct azexit_receipt winner = {.size = sizeof winner};
    int rc = g_sdk_commands.delivery->control(r->request.deck, r->token, reasons, now, &winner);
    if (rc == AZ_OK) sdk_exit_project(r, &winner);
    if (r->phase == SCMD_FINAL && !r->native_submitted && !r->pinned)
        sdk_exit_close_local(r, AZ_CANCELLED, now);
    return rc;
}
static void sdk_exit_collect_locked(void) {
    struct azexit_receipt value = {.size = sizeof value};
    while (g_sdk_commands.delivery->consume(&value) == AZ_OK) {
        struct scmd_record *r = scmd_find_token(&g_sdk_commands, value.token);
        if (r) {
            sdk_exit_project(r, &value);
            sdk_exit_join(r);
        }
        value.size = sizeof value;
    }
}
static void sdk_exit_service_pending(void) {
    pthread_mutex_lock(&g_sdk_commands.mu);
    sdk_exit_collect_locked();
    uint64_t now = scmd_now(&g_sdk_commands);
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_record *r = &g_sdk_commands.records[i];
        if (r->phase == SCMD_FREE) continue;
        uint32_t reasons = 0;
        if (r->retired && (r->phase != SCMD_FINAL || r->awaiting)) reasons = AZEXIT_STOP_RETIRE;
        int order = compare_timestamps(now, r->request.deadline_ns, 1000000000, 1000000000, 1);
        if (now && !r->deadline_fired && (r->phase != SCMD_FINAL || r->awaiting) &&
            (order == AZ_TIME_EQUAL || order == AZ_TIME_AFTER)) {
            r->deadline_fired = 1;
            reasons |= AZEXIT_STOP_DEADLINE;
        }
        if (reasons && now) (void) sdk_exit_control(r, reasons, now);
        sdk_exit_join(r);
    }
    sdk_exit_collect_locked();
    pthread_mutex_unlock(&g_sdk_commands.mu);
}
static void sdk_exit_step(unsigned deck, const struct scmd_adapter *adapter) {
    struct scmd_engine *e = &g_sdk_commands;
    pthread_mutex_lock(&e->mu);
    struct scmd_record *r = NULL;
    if (!e->active[deck]) {
        for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
            struct scmd_record *candidate = &e->records[i];
            if (candidate->phase == SCMD_QUEUED && !candidate->retired && candidate->request.deck == deck &&
                (!r || candidate->token < r->token)) r = candidate;
        }
    }
    if (!r) {
        pthread_mutex_unlock(&e->mu);
        return;
    }
    r->pinned = 1;
    r->phase = SCMD_PREPARING;
    uint64_t token = r->token;
    e->active[deck] = token;
    az_command_request request = r->request;
    pthread_mutex_unlock(&e->mu);

    struct scmd_target target = {0};
    int rc = adapter->begin(adapter->context, &request, &target);
    if (rc == AZ_OK) {
        enum scmd_detail detail;
        rc = scmd_target_valid(&request, &target, adapter, &detail);
    }
    pthread_mutex_lock(&e->mu);
    uint64_t now = scmd_now(e);
    int order = compare_timestamps(now, request.deadline_ns, 1000000000, 1000000000, 1);
    if (now && !r->deadline_fired && (order == AZ_TIME_EQUAL || order == AZ_TIME_AFTER)) {
        r->deadline_fired = 1;
        (void) sdk_exit_control(r, AZEXIT_STOP_DEADLINE, now);
    }
    int send = r->phase != SCMD_FINAL && !r->retired && now && rc == AZ_OK;
    if (send) r->phase = SCMD_DISPATCHED;
    pthread_mutex_unlock(&e->mu);
    if (send) rc = adapter->dispatch(adapter->context, &request, token, NULL);
    pthread_mutex_lock(&e->mu);
    r->pinned = 0;
    if (send && rc == AZ_PENDING) {
        r->native_submitted = 1;
    } else if (r->phase == SCMD_FINAL) {
        sdk_exit_close_local(r, AZ_STALE, now);
    } else if (r->retired) {
        (void) sdk_exit_control(r, AZEXIT_STOP_RETIRE, now);
    } else if (rc == AZ_BUSY || rc == AZ_PENDING) {
        r->phase = SCMD_QUEUED;
        e->active[deck] = 0;
    } else {
        sdk_exit_close_local(r, rc < 0 ? rc : AZ_FAILED, now);
    }
    sdk_exit_collect_locked();
    pthread_mutex_unlock(&e->mu);
}
static void sdk_command_drive(void) {
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    const struct azexit_delivery_v2 *delivery = atomic_load_explicit(&g_sdk_exit_bound, memory_order_acquire);
    if (!api || !delivery) return;
    delivery->drive();
    sdk_exit_service_pending();
    struct scmd_adapter adapter = sdk_exit_adapter(api);
    for (unsigned d = 0; d < SCMD_DECKS; ++d) sdk_exit_step(d, &adapter);
    if (sdk_command_has_pending_notification()) sdk_ui_wake();
}
static void sdk_command_arm_deadline(void) {
#if defined(__linux__)
    if (g_sdk_timer_fd < 0) return;
    uint64_t nearest = 0;
    pthread_mutex_lock(&g_sdk_commands.mu);
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_record *r = &g_sdk_commands.records[i];
        if (r->phase == SCMD_FREE || r->deadline_fired || (r->phase == SCMD_FINAL && !r->awaiting)) continue;
        int order = nearest ? compare_timestamps(r->request.deadline_ns, nearest, 1000000000, 1000000000, 1) : AZ_TIME_BEFORE;
        if (order == AZ_TIME_BEFORE) nearest = r->request.deadline_ns;
    }
    pthread_mutex_unlock(&g_sdk_commands.mu);
    struct itimerspec timer = {};
    if (nearest) {
        timer.it_value.tv_sec = (time_t) (nearest / UINT64_C(1000000000));
        timer.it_value.tv_nsec = (long) (nearest % UINT64_C(1000000000));
    }
    (void) timerfd_settime(g_sdk_timer_fd, TFD_TIMER_ABSTIME, &timer, NULL);
#endif
}
static void *sdk_command_worker(void *unused) {
    (void) unused;
    while (atomic_load_explicit(&g_sdk_command_running, memory_order_acquire)) {
        const struct azexit_delivery_v2 *delivery = atomic_load_explicit(&g_sdk_exit_bound, memory_order_acquire);
        int native_fd = delivery ? delivery->event_fd() : -1;
        struct pollfd fds[3];
        nfds_t n = 0;
        fds[n++] = (struct pollfd) {g_sdk_command_fd, POLLIN, 0};
        if (native_fd >= 0) fds[n++] = (struct pollfd) {native_fd, POLLIN, 0};
        if (g_sdk_timer_fd >= 0) fds[n++] = (struct pollfd) {g_sdk_timer_fd, POLLIN, 0};
        if (poll(fds, n, -1) < 0 && errno == EINTR) continue;
        sdk_drain_fd(g_sdk_command_fd);
        sdk_drain_fd(native_fd);
        sdk_drain_fd(g_sdk_timer_fd);
        sdk_command_drive();
        sdk_command_arm_deadline();
    }
    return NULL;
}
static __attribute__((unused)) int sdk_command_start(const struct azexit_service *api) {
    const struct azexit_delivery_v2 *delivery = sdk_exit_delivery(api);
    if (!delivery || !api->target) return AZ_UNSUPPORTED;
    const struct azexit_delivery_v2 *bound = atomic_load_explicit(&g_sdk_exit_bound, memory_order_acquire);
    if (bound) return bound == delivery && api == atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire)
                          ? AZ_OK
                          : AZ_UNSUPPORTED;
    g_sdk_command_fd = sdk_eventfd_create();
    g_sdk_ui_fd = sdk_eventfd_create();
    g_sdk_timer_fd = sdk_timerfd_create();
    if (g_sdk_command_fd >= 0 && g_sdk_ui_fd >= 0 && g_sdk_timer_fd >= 0) {
        atomic_store_explicit(&g_sdk_command_running, 1, memory_order_release);
        if (!az_thread_create(&g_sdk_command_thread, NULL, sdk_command_worker, NULL)) {
            struct azexit_binding binding = {sizeof binding, AZEXIT_DELIVERY_VERSION, SCMD_SLOTS, SCMD_PER_OWNER};
            if (delivery->bind(&binding) == AZ_OK && delivery->event_fd() >= 0) {
                g_sdk_commands.delivery = delivery;
                atomic_store_explicit(&g_sdk_exit_service, api, memory_order_release);
                atomic_store_explicit(&g_sdk_exit_bound, delivery, memory_order_release);
                sdk_command_wake();
                return AZ_OK;
            }
            atomic_store_explicit(&g_sdk_command_running, 0, memory_order_release);
            sdk_command_wake();
            pthread_join(g_sdk_command_thread, NULL);
        }
        atomic_store_explicit(&g_sdk_command_running, 0, memory_order_release);
    }
    if (g_sdk_command_fd >= 0) close(g_sdk_command_fd);
    if (g_sdk_ui_fd >= 0) close(g_sdk_ui_fd);
    if (g_sdk_timer_fd >= 0) close(g_sdk_timer_fd);
    g_sdk_command_fd = g_sdk_ui_fd = g_sdk_timer_fd = -1;
    return AZ_FAILED;
}
static __attribute__((unused)) int sdk_command_ui_fd(void) { return g_sdk_ui_fd; }
static __attribute__((unused)) void sdk_command_ui_clear(void) { sdk_drain_fd(g_sdk_ui_fd); }

static __attribute__((unused)) void sdk_command_owner_init(int owner) {
    if (!scmd_owner_ok(owner)) return;
    pthread_mutex_lock(&g_sdk_commands.mu);
    g_sdk_commands.owner_lifetime[owner]++;
    g_sdk_commands.highest_id[owner] = 0;
    g_sdk_commands.owner_closed[owner] = 0;
    g_sdk_commands.owner_initializing[owner] = 1;
    pthread_mutex_unlock(&g_sdk_commands.mu);
}
static __attribute__((unused)) void sdk_command_owner_ready(int owner) {
    if (!scmd_owner_ok(owner)) return;
    pthread_mutex_lock(&g_sdk_commands.mu);
    g_sdk_commands.owner_initializing[owner] = 0;
    pthread_mutex_unlock(&g_sdk_commands.mu);
    if (sdk_command_has_pending_notification()) sdk_ui_wake();
}
static void sdk_command_retire_owner(int owner) {
    if (!scmd_owner_ok(owner)) return;
    pthread_mutex_lock(&g_sdk_commands.mu);
    struct scmd_engine *e = &g_sdk_commands;
    e->owner_closed[owner] = 1;
    uint64_t now = scmd_now(e);
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_record *r = &e->records[i];
        if (r->phase != SCMD_FREE && r->owner == owner && r->owner_lifetime == e->owner_lifetime[owner]) {
            r->retired = 1;
            r->acknowledged = 1;
            if (!r->callback_active) {
                r->callback = NULL;
                r->callback_user = NULL;
            }
            if (e->delivery && now && (r->phase != SCMD_FINAL || r->awaiting))
                (void) sdk_exit_control(r, AZEXIT_STOP_RETIRE, now);
        }
        struct scmd_pending_delivery *p = &e->pending[i];
        if (p->occupied && p->owner == owner && p->owner_lifetime == e->owner_lifetime[owner] && !p->callback_active)
            memset(p, 0, sizeof *p);
    }
    pthread_mutex_unlock(&e->mu);
    sdk_command_wake();
}
static int sdk_exit_cancel_request(int owner, uint64_t request_id, az_command_result *out) {
    struct scmd_engine *e = &g_sdk_commands;
    if (!scmd_owner_ok(owner) || !request_id || !scmd_output_ok(out)) return AZ_INVALID;
    if (!atomic_load_explicit(&g_sdk_exit_bound, memory_order_acquire))
        return scmd_cancel(e, owner, request_id, out);
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct scmd_record *r = scmd_find_id(e, owner, request_id);
    const az_command_result *result = NULL;
    int rc = AZ_STALE;
    if (r) {
        if (r->phase != SCMD_FINAL || r->awaiting) {
            uint64_t now = scmd_now(e);
            if (!now) {
                pthread_mutex_unlock(&e->mu);
                return AZ_FAILED;
            }
            (void) sdk_exit_control(r, AZEXIT_STOP_CANCEL, now);
        }
        result = &r->result;
    } else {
        struct scmd_pending_delivery *p = scmd_find_pending_id(e, owner, request_id);
        if (p) result = &p->result;
    }
    if (result) {
        rc = result->outcome == AZ_COMMAND_CANCELLED ? AZ_OK : r && r->phase != SCMD_FINAL ? AZ_PENDING
                                                                                           : AZ_STALE;
        scmd_copy_result(out, result);
    }
    pthread_mutex_unlock(&e->mu);
    sdk_command_wake();
    return rc;
}
static int sdk_exit_submit_common(int owner, const az_command_request *r,
                                  az_command_completion_fn callback, void *user, az_command_result *out) {
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    const struct azexit_delivery_v2 *bound = atomic_load_explicit(&g_sdk_exit_bound, memory_order_acquire);
    if ((r->flags & AZ_COMMAND_NATIVE_LOAD) && sdk_exit_arguments(NULL, r, NULL) == AZ_INVALID)
        return AZ_INVALID;
    struct scmd_adapter adapter = sdk_exit_adapter(api);
    const struct scmd_adapter *enabled = bound && (r->flags & AZ_COMMAND_NATIVE_LOAD) ? &adapter : NULL;
    int rc = scmd_admit(&g_sdk_commands, owner, r, callback, user, NULL, enabled, out, NULL, 0);
    if (rc == AZ_OK) sdk_command_wake();
    return rc;
}
static int sdk_exit_submit(int owner, const az_command_request *r, az_command_result *out) {
    return sdk_exit_submit_common(owner, r, NULL, NULL, out);
}
static __attribute__((unused)) int sdk_exit_submit_async(int owner, const az_command_request *r,
                                                         az_command_completion_fn callback, void *user,
                                                         az_command_result *out) {
    return sdk_exit_submit_common(owner, r, callback, user, out);
}

static __attribute__((unused)) void sdk_cue_tick(void) {
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    if (api && api->size >= AZEXIT_SERVICE_CUE_SIZE && api->cue_tick) api->cue_tick();
}
static __attribute__((unused)) int sdk_native_cues(int deck, az_native_cue_snapshot *out) {
    if (deck < 0 || deck >= 4 || !out || out->size < sizeof *out) return AZ_INVALID;
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    return api && api->size >= AZEXIT_SERVICE_CUE_SNAPSHOT_SIZE && api->native_cues
               ? api->native_cues((unsigned) deck, out)
               : AZ_UNSUPPORTED;
}
