/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* One core command store. Host authentication passes a validated owner; native
 * execution uses its existing scalar mailbox and owner-thread adapter. */
#include "sdk_command_service.h"
static struct scmd_engine g_sdk_commands = SCMD_ENGINE_INIT;
static int sdk_command_has_pending_notification(void);
/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by sdk_command.c. Native execution uses a scalar mailbox, so neither
 * the engine mutex nor a mod callback ever runs inside the native owner hook. */
#include "../common/azexit.h"
#include "../common/azcuecommands.h"
#ifndef az_thread_create
#include "../common/azaltstack.h"
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
static int sdk_exit_delivery_receipts(const struct azexit_delivery_v2 *delivery) {
    return delivery->event_fd && delivery->consume && delivery->reclaim && delivery->control && delivery->bind;
}
static int sdk_exit_delivery_admission(const struct azexit_delivery_v2 *delivery) {
    return delivery->reserve && delivery->close_local && delivery->drive && delivery->ready;
}
static const struct azexit_delivery_v2 *sdk_exit_delivery(const struct azexit_service *api) {
    if (!api || api->version != 1 || api->size < AZEXIT_SERVICE_DELIVERY_SIZE) return NULL;
    const struct azexit_delivery_v2 *delivery = api->delivery;
    return delivery && delivery->size >= sizeof *delivery && delivery->version == AZEXIT_DELIVERY_VERSION &&
                   sdk_exit_delivery_receipts(delivery) && sdk_exit_delivery_admission(delivery)
               ? delivery
               : NULL;
}
static uint64_t sdk_exit_supported_operations(const struct azexit_service *api) {
    uint64_t basic = (UINT64_C(1) << AZ_CMD_LOOP_EXIT) | (UINT64_C(1) << AZ_CMD_RELOOP);
    uint64_t loops = ((UINT64_C(1) << (AZ_CMD_LOOP_ADJUST_OUT + 1)) - 2) & ~basic;
    uint64_t supported = api->submit_command ? basic : api->submit ? UINT64_C(1) << AZ_CMD_LOOP_EXIT
                                                                   : 0;
    if (api->size >= AZEXIT_SERVICE_LOOP_SIZE && api->submit_loop) supported |= loops;
    if (sdk_exit_has_cues(api)) supported |= SCMD_ALL_COMMANDS & ~(loops | basic);
    return api->commands() & supported;
}
static uint64_t sdk_exit_operations(const struct azexit_service *api) {
    const struct azexit_delivery_v2 *bound = atomic_load_explicit(&g_sdk_exit_bound, memory_order_acquire);
    if (!bound || bound != sdk_exit_delivery(api)) return 0;
    if (api->size >= AZEXIT_SERVICE_COMMAND_SIZE && api->commands) return sdk_exit_supported_operations(api);
    return api->mask && api->submit && api->mask() ? UINT64_C(1) << AZ_CMD_LOOP_EXIT : 0;
}
uint64_t sdk_command_capabilities(void) {
    return sdk_exit_operations(atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire));
}
static int sdk_exit_loop_metadata(const az_command_request *request) {
    return request->expected_revision || request->quantization != AZ_QUANTIZE_NATIVE || request->classification ||
           request->entry_id || request->rgb || request->comment[0];
}
static int sdk_exit_loop_length(const az_command_request *request) {
    uint32_t packed;
    return request->position_s == 0 && request->end_s == 0 && azexit_pack_beat(request->beats, &packed);
}
static int sdk_exit_loop_delta(double delta, double other, double beats) {
    return isfinite(delta) && delta != 0 && isfinite(delta * 1000.0) && other == 0 && beats == 0;
}
static int sdk_exit_loop_values(const az_command_request *request) {
    switch (request->operation) {
    case AZ_CMD_LOOP_LENGTH: return sdk_exit_loop_length(request);
    case AZ_CMD_LOOP_MOVE:
    case AZ_CMD_LOOP_ADJUST_IN: return sdk_exit_loop_delta(request->position_s, request->end_s, request->beats);
    case AZ_CMD_LOOP_ADJUST_OUT: return sdk_exit_loop_delta(request->end_s, request->position_s, request->beats);
    default: return request->position_s == 0 && request->end_s == 0 && request->beats == 0;
    }
}
static int sdk_exit_arguments(void *ctx, const az_command_request *r, const struct scmd_target *target) {
    (void) ctx;
    (void) target;
    if (r->operation < AZ_CMD_LOOP_IN || r->operation > AZ_CMD_CUE_LOOP_SET ||
        !(r->flags & AZ_COMMAND_NATIVE_LOAD)) return AZ_UNSUPPORTED;
    if (r->quantization != AZ_QUANTIZE_NATIVE) return AZ_INVALID;
    /* Native cue adapters own the operation-specific selection/unit contract. */
    if (r->operation > AZ_CMD_LOOP_ADJUST_OUT) return AZ_OK;
    if (r->flags != AZ_COMMAND_NATIVE_LOAD || sdk_exit_loop_metadata(r)) return AZ_INVALID;
    return sdk_exit_loop_values(r) ? AZ_OK : AZ_INVALID;
}
static void sdk_exit_cue_revision(const az_command_request *request, const struct azexit_cue_target *cue, struct scmd_target *target) {
    uint64_t cue_commands = target->commands & ~((UINT64_C(1) << (AZ_CMD_LOOP_ADJUST_OUT + 1)) - 2);
    target->persistent_commands = cue->persistent_commands & cue_commands;
    target->revision_commands = cue->revision_commands & cue_commands;
    /* Collection revisions and transient DSP receipt counters are distinct
     * namespaces. Only operations fenced by this collection bind its revision. */
    uint64_t operation_bit = UINT64_C(1) << request->operation;
    if (cue->revision_valid && cue->revision &&
        (operation_bit & (target->revision_commands | target->persistent_commands))) {
        target->revision = cue->revision;
        target->valid |= SCMD_TARGET_REVISION;
    }
}
static int sdk_exit_cue_target(const struct azexit_service *api, const az_command_request *request, struct scmd_target *target) {
    if (!sdk_exit_has_cues(api)) return AZ_UNSUPPORTED;
    struct azexit_cue_target cue = {0};
    int rc = api->target_cue(request->deck, &cue);
    if (rc != AZ_OK) return rc;
    if (cue.revision_valid > 1 || cue.reserved) return AZ_FAILED;
    sdk_exit_cue_revision(request, &cue, target);
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
        rc = sdk_exit_cue_target(api, r, &target);
        if (rc != AZ_OK) {
            if (rc == AZ_BUSY || rc == AZ_PENDING) *out = target;
            return rc;
        }
    }
    *out = target;
    return AZ_OK;
}
static int sdk_exit_loop_submission(const struct azexit_service *api, unsigned operation) {
    return operation <= AZ_CMD_LOOP_ADJUST_OUT && operation != AZ_CMD_LOOP_EXIT && operation != AZ_CMD_RELOOP &&
           api->size >= AZEXIT_SERVICE_LOOP_SIZE && api->submit_loop;
}
static int sdk_exit_dispatch(void *ctx, const az_command_request *r, uint64_t token, struct scmd_completion *out) {
    const struct azexit_service *api = ctx;
    struct azexit_request request = {token, r->expected_load_generation, r->deadline_ns, r->deck, 0};
    int rc = AZ_UNSUPPORTED;
    if (r->operation > AZ_CMD_LOOP_ADJUST_OUT && sdk_exit_has_cues(api)) {
        struct azexit_cue_request cue = {.size = sizeof cue, .operation = r->operation, .identity = request, .expected_revision = r->expected_revision, .flags = r->flags, .classification = r->classification, .entry_id = r->entry_id, .rgb = r->rgb, .position_s = r->position_s, .end_s = r->end_s, .beats = r->beats};
        memcpy(cue.comment, r->comment, sizeof cue.comment);
        rc = api->submit_cue(&cue);
    } else if (sdk_exit_loop_submission(api, r->operation)) {
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
static int sdk_exit_receipt_lifetime(const struct scmd_record *record, const struct azexit_receipt *value) {
    return !record->native_lifetime || !value->lifetime_generation || record->native_lifetime == value->lifetime_generation;
}
static int sdk_exit_receipt_identity(const struct scmd_record *record, const struct azexit_receipt *value) {
    return record && value->size >= sizeof *value && value->version == AZEXIT_DELIVERY_VERSION &&
           value->deck == record->request.deck && value->token == record->token &&
           value->load_generation == record->request.expected_load_generation && value->settled <= 1 &&
           sdk_exit_receipt_lifetime(record, value);
}
static int sdk_exit_expired_receipt(const struct azexit_receipt *value) {
    return value->outcome == AZ_COMMAND_CANCELLED && value->reason == AZ_STALE && value->settled && !value->revision;
}
static int sdk_exit_terminal_valid(const struct scmd_record *record, const struct azexit_receipt *value, int expired) {
    struct scmd_completion completion = {value->outcome, value->reason, value->load_generation, value->revision, value->settled};
    int deadline = compare_timestamps(value->observed_ns, record->request.deadline_ns, 1000000000, 1000000000, 1);
    if ((!expired && !scmd_completion_valid(&completion)) ||
        (value->outcome == AZ_COMMAND_APPLIED && (!value->observed_ns || deadline != AZ_TIME_BEFORE))) return 0;
    return 1;
}
static int sdk_exit_terminal(struct scmd_record *record, const struct azexit_receipt *value) {
    int expired = sdk_exit_expired_receipt(value);
    if (!sdk_exit_terminal_valid(record, value, expired)) return 0;
    if (record->phase != SCMD_FINAL) {
        record->settled |= value->settled;
        scmd_finish(&g_sdk_commands, record, value->outcome, value->reason, value->revision,
                    expired ? SCMD_DETAIL_DEADLINE : SCMD_DETAIL_NONE, value->observed_ns, 1);
    }
    return 1;
}
static void sdk_exit_project(struct scmd_record *record, const struct azexit_receipt *value) {
    if (!sdk_exit_receipt_identity(record, value)) return;
    if (value->kind == AZEXIT_RECEIPT_TERMINAL) {
        if (!sdk_exit_terminal(record, value)) return;
    } else if (value->kind == AZEXIT_RECEIPT_SETTLED && value->settled) record->settled = 1;
    else if (value->kind == AZEXIT_RECEIPT_RESOURCES_RETIRED && value->settled) record->native_retired = 1;
    else return;
    if (value->lifetime_generation) record->native_lifetime = value->lifetime_generation;
}
static int sdk_exit_reclaimable(const struct scmd_record *record) {
    return record->phase == SCMD_FINAL && !record->pinned && record->awaiting && record->settled && record->native_retired;
}
static void sdk_exit_join(struct scmd_record *record) {
    if (sdk_exit_reclaimable(record)) {
        struct azexit_receipt identity = sdk_exit_identity(record, AZEXIT_RECEIPT_RESOURCES_RETIRED);
        if (g_sdk_commands.delivery->reclaim(&identity) == AZ_OK) {
            record->awaiting = 0;
            scmd_clear_barrier(&g_sdk_commands, record);
        }
    }
    if (record->retired && !record->callback_active) {
        record->callback = NULL;
        record->callback_user = NULL;
        if (record->phase == SCMD_FINAL && !record->awaiting && !record->pinned) memset(record, 0, sizeof *record);
    } else (void) scmd_move_to_pending_locked(&g_sdk_commands, record);
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
static int sdk_exit_deadline_due(const struct scmd_record *record, uint64_t now) {
    int order = compare_timestamps(now, record->request.deadline_ns, 1000000000, 1000000000, 1);
    return now && !record->deadline_fired && (order == AZ_TIME_EQUAL || order == AZ_TIME_AFTER);
}
static uint32_t sdk_exit_stop_reasons(struct scmd_record *record, uint64_t now) {
    uint32_t reasons = 0;
    if (record->retired && (record->phase != SCMD_FINAL || record->awaiting)) reasons = AZEXIT_STOP_RETIRE;
    if (sdk_exit_deadline_due(record, now) && (record->phase != SCMD_FINAL || record->awaiting)) {
        record->deadline_fired = 1;
        reasons |= AZEXIT_STOP_DEADLINE;
    }
    return reasons;
}
static void sdk_exit_service_pending(void) {
    pthread_mutex_lock(&g_sdk_commands.mu);
    sdk_exit_collect_locked();
    uint64_t now = scmd_now(&g_sdk_commands);
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_record *r = &g_sdk_commands.records[i];
        if (r->phase == SCMD_FREE) continue;
        uint32_t reasons = sdk_exit_stop_reasons(r, now);
        if (reasons && now) (void) sdk_exit_control(r, reasons, now);
        sdk_exit_join(r);
    }
    sdk_exit_collect_locked();
    pthread_mutex_unlock(&g_sdk_commands.mu);
}
static struct scmd_record *sdk_exit_next_command(struct scmd_engine *engine, unsigned deck) {
    if (engine->active[deck]) return NULL;
    struct scmd_record *next = NULL;
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_record *candidate = &engine->records[i];
        if (candidate->phase == SCMD_QUEUED && !candidate->retired && candidate->request.deck == deck &&
            (!next || candidate->token < next->token)) next = candidate;
    }
    return next;
}
static int sdk_exit_prepare(const struct scmd_adapter *adapter, const az_command_request *request) {
    struct scmd_target target = {0};
    int result = adapter->begin(adapter->context, request, &target);
    if (result == AZ_OK) {
        enum scmd_detail detail;
        result = scmd_target_valid(request, &target, adapter, &detail);
    }
    return result;
}
static int sdk_exit_dispatch_ready(struct scmd_record *record, uint64_t now, int result) {
    if (sdk_exit_deadline_due(record, now)) {
        record->deadline_fired = 1;
        (void) sdk_exit_control(record, AZEXIT_STOP_DEADLINE, now);
    }
    int send = record->phase != SCMD_FINAL && !record->retired && now && result == AZ_OK;
    if (send) record->phase = SCMD_DISPATCHED;
    return send;
}
static void sdk_exit_dispatch_result(struct scmd_engine *engine, struct scmd_record *record, int sent, int result, uint64_t now) {
    record->pinned = 0;
    if (sent && result == AZ_PENDING) record->native_submitted = 1;
    else if (record->phase == SCMD_FINAL) sdk_exit_close_local(record, AZ_STALE, now);
    else if (record->retired) (void) sdk_exit_control(record, AZEXIT_STOP_RETIRE, now);
    else if (result == AZ_BUSY || result == AZ_PENDING) {
        record->phase = SCMD_QUEUED;
        engine->active[record->request.deck] = 0;
    } else sdk_exit_close_local(record, result < 0 ? result : AZ_FAILED, now);
}
static void sdk_exit_step(unsigned deck, const struct scmd_adapter *adapter) {
    struct scmd_engine *e = &g_sdk_commands;
    pthread_mutex_lock(&e->mu);
    struct scmd_record *r = sdk_exit_next_command(e, deck);
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

    int rc = sdk_exit_prepare(adapter, &request);
    pthread_mutex_lock(&e->mu);
    uint64_t now = scmd_now(e);
    int send = sdk_exit_dispatch_ready(r, now, rc);
    pthread_mutex_unlock(&e->mu);
    if (send) rc = adapter->dispatch(adapter->context, &request, token, NULL);
    pthread_mutex_lock(&e->mu);
    sdk_exit_dispatch_result(e, r, send, rc, now);
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
static __attribute__((unused)) uint64_t sdk_command_nearest_deadline(void) {
    uint64_t nearest = 0;
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        const struct scmd_record *record = &g_sdk_commands.records[i];
        if (record->phase == SCMD_FREE || record->deadline_fired || (record->phase == SCMD_FINAL && !record->awaiting)) continue;
        int order = nearest ? compare_timestamps(record->request.deadline_ns, nearest, 1000000000, 1000000000, 1) : AZ_TIME_BEFORE;
        if (order == AZ_TIME_BEFORE) nearest = record->request.deadline_ns;
    }
    return nearest;
}
static void sdk_command_arm_deadline(void) {
#if defined(__linux__)
    if (g_sdk_timer_fd < 0) return;
    pthread_mutex_lock(&g_sdk_commands.mu);
    uint64_t nearest = sdk_command_nearest_deadline();
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
static void sdk_command_close_signals(void) {
    if (g_sdk_command_fd >= 0) close(g_sdk_command_fd);
    if (g_sdk_ui_fd >= 0) close(g_sdk_ui_fd);
    if (g_sdk_timer_fd >= 0) close(g_sdk_timer_fd);
    g_sdk_command_fd = g_sdk_ui_fd = g_sdk_timer_fd = -1;
}
static int sdk_command_bind(const struct azexit_service *api, const struct azexit_delivery_v2 *delivery) {
    struct azexit_binding binding = {sizeof binding, AZEXIT_DELIVERY_VERSION, SCMD_SLOTS, SCMD_PER_OWNER};
    if (delivery->bind(&binding) != AZ_OK || delivery->event_fd() < 0) return 0;
    g_sdk_commands.delivery = delivery;
    atomic_store_explicit(&g_sdk_exit_service, api, memory_order_release);
    atomic_store_explicit(&g_sdk_exit_bound, delivery, memory_order_release);
    sdk_command_wake();
    return 1;
}
static int sdk_command_start_worker(const struct azexit_service *api, const struct azexit_delivery_v2 *delivery) {
    atomic_store_explicit(&g_sdk_command_running, 1, memory_order_release);
    if (!az_thread_create(&g_sdk_command_thread, NULL, sdk_command_worker, NULL)) {
        if (sdk_command_bind(api, delivery)) return 1;
        atomic_store_explicit(&g_sdk_command_running, 0, memory_order_release);
        sdk_command_wake();
        pthread_join(g_sdk_command_thread, NULL);
    }
    atomic_store_explicit(&g_sdk_command_running, 0, memory_order_release);
    return 0;
}
static int sdk_command_create_signals(void) {
    g_sdk_command_fd = sdk_eventfd_create();
    g_sdk_ui_fd = sdk_eventfd_create();
    g_sdk_timer_fd = sdk_timerfd_create();
    return g_sdk_command_fd >= 0 && g_sdk_ui_fd >= 0 && g_sdk_timer_fd >= 0;
}
int sdk_command_start(const struct azexit_service *api) {
    const struct azexit_delivery_v2 *delivery = sdk_exit_delivery(api);
    if (!delivery || !api->target) return AZ_UNSUPPORTED;
    const struct azexit_delivery_v2 *bound = atomic_load_explicit(&g_sdk_exit_bound, memory_order_acquire);
    if (bound) return bound == delivery && api == atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire)
                          ? AZ_OK
                          : AZ_UNSUPPORTED;
    if (sdk_command_create_signals() && sdk_command_start_worker(api, delivery)) return AZ_OK;
    sdk_command_close_signals();
    return AZ_FAILED;
}
int sdk_command_ui_fd(void) { return g_sdk_ui_fd; }
static __attribute__((unused)) void sdk_command_ui_clear(void) { sdk_drain_fd(g_sdk_ui_fd); }

void sdk_command_owner_init(int owner) {
    if (!scmd_owner_ok(owner)) return;
    pthread_mutex_lock(&g_sdk_commands.mu);
    g_sdk_commands.owner_lifetime[owner]++;
    g_sdk_commands.highest_id[owner] = 0;
    g_sdk_commands.owner_closed[owner] = 0;
    g_sdk_commands.owner_initializing[owner] = 1;
    pthread_mutex_unlock(&g_sdk_commands.mu);
}
void sdk_command_owner_ready(int owner) {
    if (!scmd_owner_ok(owner)) return;
    pthread_mutex_lock(&g_sdk_commands.mu);
    g_sdk_commands.owner_initializing[owner] = 0;
    pthread_mutex_unlock(&g_sdk_commands.mu);
    if (sdk_command_has_pending_notification()) sdk_ui_wake();
}
static void sdk_exit_retire_record(struct scmd_engine *engine, struct scmd_record *record, int owner, uint64_t now) {
    if (record->phase == SCMD_FREE || record->owner != owner || record->owner_lifetime != engine->owner_lifetime[owner]) return;
    record->retired = 1;
    record->acknowledged = 1;
    if (!record->callback_active) {
        record->callback = NULL;
        record->callback_user = NULL;
    }
    if (engine->delivery && now && (record->phase != SCMD_FINAL || record->awaiting))
        (void) sdk_exit_control(record, AZEXIT_STOP_RETIRE, now);
}
static void sdk_exit_retire_notification(struct scmd_engine *engine, struct scmd_pending_delivery *pending, int owner) {
    if (pending->occupied && pending->owner == owner && pending->owner_lifetime == engine->owner_lifetime[owner] && !pending->callback_active)
        memset(pending, 0, sizeof *pending);
}
void sdk_command_retire_owner(int owner) {
    if (!scmd_owner_ok(owner)) return;
    pthread_mutex_lock(&g_sdk_commands.mu);
    struct scmd_engine *e = &g_sdk_commands;
    e->owner_closed[owner] = 1;
    uint64_t now = scmd_now(e);
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        sdk_exit_retire_record(e, &e->records[i], owner, now);
        sdk_exit_retire_notification(e, &e->pending[i], owner);
    }
    pthread_mutex_unlock(&e->mu);
    sdk_command_wake();
}
static int sdk_exit_cancel_record(struct scmd_engine *engine, struct scmd_record *record) {
    if (record->phase == SCMD_FINAL && !record->awaiting) return AZ_OK;
    uint64_t now = scmd_now(engine);
    if (!now) return AZ_FAILED;
    (void) sdk_exit_control(record, AZEXIT_STOP_CANCEL, now);
    return AZ_OK;
}
static int sdk_exit_cancel_result(const struct scmd_record *record, const az_command_result *result, az_command_result *out) {
    if (!result) return AZ_STALE;
    int rc = result->outcome == AZ_COMMAND_CANCELLED ? AZ_OK : record && record->phase != SCMD_FINAL ? AZ_PENDING
                                                                                                     : AZ_STALE;
    scmd_copy_result(out, result);
    return rc;
}
static int sdk_exit_cancel_locked(struct scmd_engine *engine, int owner, uint64_t request_id, az_command_result *out) {
    struct scmd_record *record = scmd_find_id(engine, owner, request_id);
    const az_command_result *result = NULL;
    if (record) {
        int rc = sdk_exit_cancel_record(engine, record);
        if (rc != AZ_OK) return rc;
        result = &record->result;
    } else {
        const struct scmd_pending_delivery *pending = scmd_find_pending_id(engine, owner, request_id);
        if (pending) result = &pending->result;
    }
    return sdk_exit_cancel_result(record, result, out);
}
int sdk_exit_cancel_request(int owner, uint64_t request_id, az_command_result *out) {
    struct scmd_engine *engine = &g_sdk_commands;
    if (!scmd_owner_ok(owner) || !request_id || !scmd_output_ok(out)) return AZ_INVALID;
    if (!atomic_load_explicit(&g_sdk_exit_bound, memory_order_acquire)) return scmd_cancel(engine, owner, request_id, out);
    if (pthread_mutex_trylock(&engine->mu)) return AZ_BUSY;
    int rc = sdk_exit_cancel_locked(engine, owner, request_id, out);
    pthread_mutex_unlock(&engine->mu);
    if (rc != AZ_FAILED) sdk_command_wake();
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
int sdk_exit_submit(int owner, const az_command_request *r, az_command_result *out) {
    return sdk_exit_submit_common(owner, r, NULL, NULL, out);
}
int sdk_exit_submit_async(int owner, const az_command_request *r,
                                                         az_command_completion_fn callback, void *user,
                                                         az_command_result *out) {
    return sdk_exit_submit_common(owner, r, callback, user, out);
}

void sdk_cue_tick(void) {
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    if (api && api->size >= AZEXIT_SERVICE_CUE_SIZE && api->cue_tick) api->cue_tick();
}
int sdk_native_cues(int deck, az_native_cue_snapshot *out) {
    if (deck < 0 || deck >= 4 || !out || out->size < AZ_NATIVE_CUE_SNAPSHOT_V1_SIZE) return AZ_INVALID;
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    return api && api->size >= AZEXIT_SERVICE_CUE_SNAPSHOT_SIZE && api->native_cues
               ? api->native_cues((unsigned) deck, out)
               : AZ_UNSUPPORTED;
}


static int sdk_command_terminal_pending(const struct scmd_record *r) {
    return r->asynchronous && r->phase == SCMD_FINAL && r->callback && !r->retired &&
           !r->terminal_delivered && scmd_owner_deliverable(&g_sdk_commands, r->owner, r->owner_lifetime);
}
static int sdk_command_pending_locked(void) {
    for (unsigned i = 0; i < SCMD_SLOTS; ++i)
        if (sdk_command_terminal_pending(&g_sdk_commands.records[i])) return 1;
    for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
        struct scmd_pending_delivery *p = &g_sdk_commands.pending[i];
        if (p->occupied && !p->released_delivered &&
            scmd_owner_deliverable(&g_sdk_commands, p->owner, p->owner_lifetime)) return 1;
    }
    return 0;
}
static int sdk_command_has_pending_notification(void) {
    pthread_mutex_lock(&g_sdk_commands.mu);
    int pending = sdk_command_pending_locked();
    pthread_mutex_unlock(&g_sdk_commands.mu);
    return pending;
}

static uint64_t g_sdk_command_notified[SCMD_SLOTS];
static int sdk_command_callback_ready(const struct scmd_record *record, int slot) {
    return record->phase == SCMD_FINAL && !record->retired && !record->asynchronous && !record->pinned &&
           !record->awaiting && record->token != g_sdk_command_notified[slot];
}
static int sdk_command_callback_owner(const struct scmd_record *record, int (*ready)(int)) {
    int owner = record->owner;
    return owner >= 0 && owner < SCMD_OWNERS && record->owner_lifetime == g_sdk_commands.owner_lifetime[owner] &&
           ready(owner);
}
/* Retained records provide exactly-once completion despite journal overruns.
 * Copy under the lock, then release it before entering mod code. */
int sdk_command_record_callback(int slot, int (*ready)(int), void (*deliver_result)(int, const az_command_result *)) {
    if (pthread_mutex_trylock(&g_sdk_commands.mu)) return 0;
    const struct scmd_record *record = &g_sdk_commands.records[slot];
    int owner = record->owner;
    int deliver = sdk_command_callback_ready(record, slot) && sdk_command_callback_owner(record, ready);
    az_command_result result;
    if (deliver) {
        result = record->result;
        g_sdk_command_notified[slot] = record->token;
    }
    pthread_mutex_unlock(&g_sdk_commands.mu);
    if (deliver) deliver_result(owner, &result);
    return 1;
}

/* Autonomous command delivery is drained by the overlay loop, not by
 * community_tick.  At most eight callbacks are reserved per pass; the caller
 * leaves the UI descriptor readable when more remain. */
unsigned sdk_command_ui_drain(unsigned budget) {
    sdk_command_ui_clear();
    unsigned delivered = 0;
    while (delivered < budget) {
        struct scmd_notification notification;
        int rc = scmd_next_notification(&g_sdk_commands, &notification);
        if (rc != AZ_OK) break;
        if (notification.callback) notification.callback(&notification.completion, notification.user);
        scmd_notification_done(&g_sdk_commands, &notification);
        sdk_command_wake();
        delivered++;
    }
    if (sdk_command_has_pending_notification()) sdk_ui_wake();
    return delivered;
}

int sdk_command_read(int owner, uint64_t request, az_command_result *out) {
    return scmd_poll(&g_sdk_commands, owner, request, out);
}
int sdk_command_drop(int owner, uint64_t request) {
    return scmd_release(&g_sdk_commands, owner, request);
}
int sdk_command_ack(int owner, uint64_t request) {
    int rc = scmd_acknowledge(&g_sdk_commands, owner, request);
    if (rc == AZ_OK) sdk_command_wake();
    return rc;
}

void sdk_command_observe_native(const struct azexit_service *api) {
    atomic_store_explicit(&g_sdk_exit_service, api, memory_order_release);
}
int sdk_command_native_bound(void) {
    return atomic_load_explicit(&g_sdk_exit_bound, memory_order_acquire) != NULL;
}
