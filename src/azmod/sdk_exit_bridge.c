/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by sdk_command.c. Native execution uses a scalar mailbox, so neither
 * the engine mutex nor a mod callback ever runs inside the native owner hook. */
#include "../common/azexit.h"
#include <stdatomic.h>
static _Atomic(const struct azexit_service *) g_sdk_exit_service;
static atomic_flag g_sdk_exit_driver = ATOMIC_FLAG_INIT;
static uint64_t g_sdk_exit_tokens[4]; /* the single admitted driver only */
static _Atomic uint32_t g_sdk_command_retired;

static int sdk_exit_has_cues(const struct azexit_service *api) {
    return api && api->size >= AZEXIT_SERVICE_CUE_SIZE && api->commands && api->submit_cue && api->target_cue;
}
static uint64_t sdk_exit_operations(const struct azexit_service *api) {
    if (!api) return 0;
    if (api->size >= AZEXIT_SERVICE_COMMAND_SIZE && api->commands) {
        uint64_t supported = api->submit_command ? (UINT64_C(1) << AZ_CMD_LOOP_EXIT) | (UINT64_C(1) << AZ_CMD_RELOOP) : 0;
        uint64_t loops = (UINT64_C(1) << (AZ_CMD_LOOP_ADJUST_OUT + 1)) - 2;
        if (api->size >= AZEXIT_SERVICE_LOOP_SIZE && api->submit_loop) supported |= loops;
        if (sdk_exit_has_cues(api)) supported |= SCMD_ALL_COMMANDS & ~loops;
        return api->commands() & supported;
    }
    return api->mask() ? UINT64_C(1) << AZ_CMD_LOOP_EXIT : 0;
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
    uint64_t generation;
    int rc = api->target(r->deck, &generation);
    if (rc != AZ_OK) return rc;
    struct scmd_target target = {.load_generation = generation, .commands = sdk_exit_operations(api), .valid = SCMD_TARGET_LOAD, .loaded = 1};
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
    if (rc == AZ_PENDING) {
        g_sdk_exit_tokens[r->deck] = token;
        return AZ_PENDING;
    }
    *out = (struct scmd_completion) {.outcome = AZ_COMMAND_REJECTED, .reason = rc < 0 ? rc : AZ_FAILED, .settled = 1};
    return AZ_OK;
}
static void sdk_exit_end(void *ctx, uint32_t deck) {
    (void) ctx;
    (void) deck;
}
static struct scmd_adapter sdk_exit_adapter(const struct azexit_service *api) {
    return (struct scmd_adapter) {(void *) api, sdk_exit_arguments, sdk_exit_begin, sdk_exit_dispatch, sdk_exit_end};
}
static void sdk_command_collect_retired(const struct azexit_service *api) {
    uint32_t retired = atomic_load_explicit(&g_sdk_command_retired, memory_order_acquire);
    if (!retired || pthread_mutex_trylock(&g_sdk_commands.mu)) return;
    for (unsigned owner = 0; owner < SCMD_OWNERS; ++owner) {
        if (!(retired & (UINT32_C(1) << owner))) continue;
        int remaining = 0;
        for (unsigned i = 0; i < SCMD_SLOTS; ++i) {
            struct scmd_record *r = &g_sdk_commands.records[i];
            if (r->phase == SCMD_FREE || r->owner != (int) owner) continue;
            if (r->phase == SCMD_QUEUED)
                scmd_finish(&g_sdk_commands, r, AZ_COMMAND_CANCELLED, AZ_CANCELLED, 0,
                            SCMD_DETAIL_NONE, scmd_now(&g_sdk_commands), 0);
            if (r->awaiting) (void) api->cancel(r->request.deck, r->token);
            if (r->phase == SCMD_FINAL && !r->pinned && !r->awaiting) memset(r, 0, sizeof *r);
            else remaining = 1;
        }
        if (!remaining) {
            /* A new authenticated module in this loader slot gets a fresh
             * request-ID namespace; global dispatch tokens are never reused. */
            g_sdk_commands.highest_id[owner] = 0;
            atomic_fetch_and_explicit(&g_sdk_command_retired, ~(UINT32_C(1) << owner), memory_order_acq_rel);
        }
    }
    pthread_mutex_unlock(&g_sdk_commands.mu);
}
static void sdk_exit_collect(const struct azexit_service *api, unsigned deck) {
    uint64_t token = g_sdk_exit_tokens[deck];
    struct azexit_result result;
    if (token && api->poll(deck, &result) == AZ_OK && result.token == token) {
        struct scmd_completion completion = {result.outcome, result.reason, result.load_generation, result.revision, 1};
        int rc = scmd_complete(&g_sdk_commands, token, &completion);
        if ((rc == AZ_OK || rc == AZ_STALE) && api->release(deck, token) == AZ_OK) g_sdk_exit_tokens[deck] = 0;
    }
}
static void sdk_command_drive(void) {
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    if (!api || atomic_flag_test_and_set_explicit(&g_sdk_exit_driver, memory_order_acquire)) return;
    sdk_command_collect_retired(api);
    /* Native execution enforces the actual action deadline. Collect its
     * completed receipts first: a late UI poll is not an execution timeout. */
    for (unsigned d = 0; d < 4; ++d) sdk_exit_collect(api, d);
    (void) scmd_expire(&g_sdk_commands);
    struct scmd_adapter adapter = sdk_exit_adapter(api);
    for (unsigned d = 0; d < 4; ++d) {
        uint64_t token = g_sdk_exit_tokens[d];
        if (token) {
            if (!pthread_mutex_trylock(&g_sdk_commands.mu)) {
                struct scmd_record *record = scmd_find_token(&g_sdk_commands, token);
                int expired = record && record->phase == SCMD_FINAL && record->awaiting;
                pthread_mutex_unlock(&g_sdk_commands.mu);
                if (expired) (void) api->cancel(d, token);
            }
            sdk_exit_collect(api, d);
        }
        sdk_command_collect_retired(api);
        if (!g_sdk_exit_tokens[d]) (void) scmd_step(&g_sdk_commands, d, &adapter);
    }
    atomic_flag_clear_explicit(&g_sdk_exit_driver, memory_order_release);
}
static void sdk_command_retire_owner(int owner) {
    if (!scmd_owner_ok(owner)) return;
    atomic_fetch_or_explicit(&g_sdk_command_retired, UINT32_C(1) << (unsigned) owner, memory_order_release);
    sdk_command_drive();
}
static void sdk_exit_cancel_request(int owner, uint64_t request_id) {
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    if (!api || pthread_mutex_trylock(&g_sdk_commands.mu)) return;
    struct scmd_record *r = scmd_find_id(&g_sdk_commands, owner, request_id);
    uint64_t token = r && r->awaiting ? r->token : 0;
    unsigned deck = r ? r->request.deck : 0;
    pthread_mutex_unlock(&g_sdk_commands.mu);
    if (token) (void) api->cancel(deck, token);
}
static int sdk_exit_submit(int owner, const az_command_request *r, az_command_result *out) {
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    if (!api) return scmd_refuse(out, r, AZ_UNSUPPORTED, scmd_now(&g_sdk_commands));
    if (atomic_load_explicit(&g_sdk_command_retired, memory_order_acquire) & (UINT32_C(1) << (unsigned) owner)) {
        sdk_command_drive();
        if (atomic_load_explicit(&g_sdk_command_retired, memory_order_acquire) & (UINT32_C(1) << (unsigned) owner)) return AZ_BUSY;
    }
    struct scmd_adapter adapter = sdk_exit_adapter(api);
    struct scmd_target target = {0};
    int rc = sdk_exit_begin((void *) api, r, &target);
    /* scmd_submit checks retained retries before target/capability changes. */
    int target_pending = rc == AZ_BUSY || rc == AZ_PENDING;
    if (rc != AZ_OK) target.commands = rc == AZ_STALE || target_pending ? sdk_exit_operations(api) : 0;
    rc = scmd_submit_target(&g_sdk_commands, owner, r, &target, &adapter, out, NULL, target_pending);
    if (rc == AZ_OK) sdk_command_drive();
    return rc;
}

static __attribute__((unused)) void sdk_cue_tick(void) {
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    if (api && api->size >= AZEXIT_SERVICE_CUE_SIZE && api->cue_tick) api->cue_tick();
}
static __attribute__((unused)) int sdk_native_cues(int deck, az_native_cue_snapshot *out) {
    if (deck < 0 || deck >= 4 || !out || out->size < sizeof *out) return AZ_INVALID;
    const struct azexit_service *api = atomic_load_explicit(&g_sdk_exit_service, memory_order_acquire);
    return api && api->size >= AZEXIT_SERVICE_CUE_SNAPSHOT_SIZE && api->native_cues
        ? api->native_cues((unsigned) deck, out) : AZ_UNSUPPORTED;
}
