/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual public wrappers, against exact old private-service allocations. */
#include "azmod-mod.h"
#include "azexit.h"
#include "aztimestamp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
extern void sdk_exit_public_bind(const struct azexit_service *);
extern uint64_t sdk_exit_public_capabilities(void);
extern int sdk_exit_public_submit(const char *, const az_command_request *, az_command_result *);
extern int sdk_exit_public_poll(const char *, uint64_t, az_command_result *);
extern int sdk_exit_public_release(const char *, uint64_t);
extern int sdk_exit_public_cancel(const char *, uint64_t, az_command_result *);
extern void sdk_exit_public_clock(uint64_t);
static struct azexit_result receipt;
static unsigned dispatched;
static uint64_t cue_receipt_revision;
static uint64_t target_generation = 1;
static struct azexit_loop_request copied;
static struct azexit_cue_request copied_cue;
static struct azexit_cue_target cue_target = {41, 0, 0, 1, 0};
static int cue_target_rc = AZ_OK;
static unsigned cue_targets, cue_ticks, cue_phase_calls, cue_target_mutation;
static uint64_t advertised = (UINT64_C(1) << 13) - 2;
static unsigned mask(void) { return 15; }
static uint64_t commands(void) { return advertised; }
static int target(unsigned deck, uint64_t *generation) {
    assert(deck < 4);
    *generation = target_generation;
    return AZ_OK;
}
static int submit_command(const struct azexit_request *r, uint32_t operation) {
    assert(operation == AZ_CMD_LOOP_EXIT || operation == AZ_CMD_RELOOP);
    ++dispatched;
    receipt = (struct azexit_result) {r->token, r->load_generation, dispatched, AZ_COMMAND_APPLIED, AZ_OK};
    return AZ_PENDING;
}
static int submit(const struct azexit_request *r) { return submit_command(r, AZ_CMD_LOOP_EXIT); }
static int submit_loop(const struct azexit_loop_request *r) {
    copied = *r;
    ++dispatched;
    receipt = (struct azexit_result) {r->identity.token, r->identity.load_generation, dispatched, AZ_COMMAND_APPLIED, AZ_OK};
    return AZ_PENDING;
}
static int submit_cue(const struct azexit_cue_request *r) {
    copied_cue = *r;
    ++dispatched;
    receipt = (struct azexit_result) {r->identity.token, r->identity.load_generation, cue_receipt_revision ? cue_receipt_revision : cue_target.revision + 1, AZ_COMMAND_APPLIED, AZ_OK};
    return AZ_PENDING;
}
static int target_cue(unsigned deck, struct azexit_cue_target *out) {
    assert(deck < 4);
    cue_targets++;
    if (cue_target_rc == AZ_OK) {
        *out = cue_target;
        if (++cue_phase_calls == 2) {
            if (cue_target_mutation == 1) out->revision++;
            if (cue_target_mutation == 2) out->persistent_commands |= UINT64_C(1) << AZ_CMD_HOT_TRIGGER;
        }
    }
    return cue_target_rc;
}
static void cue_tick(void) { cue_ticks++; }
static int cancel(unsigned deck, uint64_t token) {
    (void) deck;
    (void) token;
    return AZ_PENDING;
}
static int poll(unsigned deck, struct azexit_result *out) {
    (void) deck;
    if (!receipt.token) return AZ_PENDING;
    *out = receipt;
    return AZ_OK;
}
static int release(unsigned deck, uint64_t token) {
    (void) deck;
    assert(receipt.token == token);
    memset(&receipt, 0, sizeof receipt);
    return AZ_OK;
}
static az_command_request request(uint64_t id, unsigned operation) {
    az_command_request r = {0};
    r.size = sizeof r;
    r.version = 1;
    r.operation = operation;
    r.request_id = id;
    r.expected_load_generation = 1;
    r.deadline_ns = UINT64_MAX;
    r.flags = AZ_COMMAND_NATIVE_LOAD;
    return r;
}
static void run(uint64_t id, unsigned operation) {
    az_command_request r = request(id, operation);
    if (operation == AZ_CMD_LOOP_MOVE) r.position_s = -0.25;
    az_command_result out = {.size = sizeof out};
    unsigned before = dispatched;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    assert(dispatched == before + 1);
    assert(sdk_exit_public_poll("one", id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED);
    assert(sdk_exit_public_release("one", id) == AZ_OK);
}
int main(void) {
    const uint64_t old_commands = (UINT64_C(1) << AZ_CMD_LOOP_EXIT) | (UINT64_C(1) << AZ_CMD_RELOOP);
    struct Old {
        uint32_t size, version;
        unsigned (*mask)(void);
        int (*target)(unsigned, uint64_t *);
        int (*submit)(const struct azexit_request *);
        int (*cancel)(unsigned, uint64_t);
        int (*poll)(unsigned, struct azexit_result *);
        int (*release)(unsigned, uint64_t);
        uint64_t (*commands)(void);
        int (*submit_command)(const struct azexit_request *, uint32_t);
    } old = {sizeof old, 1, mask, target, submit, cancel, poll, release, commands, submit_command};
    _Static_assert(sizeof old == AZEXIT_SERVICE_COMMAND_SIZE, "old private prefix changed");
    sdk_exit_public_bind((const struct azexit_service *) &old);
    assert(sdk_exit_public_capabilities() == old_commands);
    az_command_request r = request(1, AZ_CMD_LOOP_DOUBLE);
    az_command_result out = {.size = sizeof out};
    unsigned before;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_UNSUPPORTED && !dispatched);
    run(1, AZ_CMD_RELOOP);
    run(2, AZ_CMD_LOOP_EXIT);
    struct azexit_service current = {.size = sizeof current, .version = 1, .mask = mask, .target = target, .submit = submit, .cancel = cancel, .poll = poll, .release = release, .commands = commands, .submit_command = submit_command};
    sdk_exit_public_bind(&current);
    assert(sdk_exit_public_capabilities() == old_commands);
    current.submit_loop = submit_loop;
    assert(sdk_exit_public_capabilities() == commands());
    run(3, AZ_CMD_LOOP_MOVE);
    assert(copied.size == sizeof copied && copied.operation == AZ_CMD_LOOP_MOVE && copied.position_s == -0.25);
    assert(copied.end_s == 0 && copied.beats == 0 && copied.identity.load_generation == 1);
    /* Exactly sized old LOOP and pre-tick CUE allocations must never expose new fields. */
    struct OldLoop {
        uint32_t size, version;
        unsigned (*mask)(void);
        int (*target)(unsigned, uint64_t *);
        int (*submit)(const struct azexit_request *);
        int (*cancel)(unsigned, uint64_t);
        int (*poll)(unsigned, struct azexit_result *);
        int (*release)(unsigned, uint64_t);
        uint64_t (*commands)(void);
        int (*submit_command)(const struct azexit_request *, uint32_t);
        int (*submit_loop)(const struct azexit_loop_request *);
    } old_loop = {sizeof old_loop, 1, mask, target, submit, cancel, poll, release, commands, submit_command, submit_loop};
    _Static_assert(sizeof old_loop == AZEXIT_SERVICE_LOOP_SIZE, "old LOOP prefix changed");
    _Static_assert(sizeof(struct azexit_cue_request) == 216, "copied cue request layout changed");
    _Static_assert(sizeof(struct azexit_cue_target) == 32, "cue target layout changed");
    advertised = (UINT64_C(1) << (AZ_CMD_CUE_COLOR + 1)) - 2;
    sdk_exit_public_bind((const struct azexit_service *) &old_loop);
    assert(sdk_exit_public_capabilities() == ((UINT64_C(1) << 13) - 2));
    r = request(4, AZ_CMD_CUE_COMMENT);
    before = dispatched;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_UNSUPPORTED && dispatched == before);
    run(4, AZ_CMD_LOOP_DOUBLE);
    sdk_exit_public_bind(&current);
    assert(sdk_exit_public_capabilities() == ((UINT64_C(1) << 13) - 2));
    current.submit_cue = submit_cue;
    assert(sdk_exit_public_capabilities() == ((UINT64_C(1) << 13) - 2));
    current.target_cue = target_cue;
    current.size = AZEXIT_SERVICE_CUE_SIZE - sizeof current.cue_tick;
    assert(sdk_exit_public_capabilities() == ((UINT64_C(1) << 13) - 2));
    current.size = AZEXIT_SERVICE_CUE_SIZE;
    assert(sdk_exit_public_capabilities() == advertised); /* cue_tick is optional */
    current.cue_tick = cue_tick;
    current.submit_cue = NULL;
    assert(sdk_exit_public_capabilities() == ((UINT64_C(1) << 13) - 2));
    current.submit_cue = submit_cue;
    /* Dynamic native masks, rather than hardcoded operation guesses, define fences. */
    uint64_t bit = UINT64_C(1) << AZ_CMD_CUE_COMMENT;
    cue_target.persistent_commands = cue_target.revision_commands = bit;
    r = request(5, AZ_CMD_CUE_COMMENT);
    r.deck = 2;
    r.expected_revision = 41;
    before = dispatched;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID && dispatched == before);
    r.flags |= AZ_COMMAND_PERSIST;
    r.expected_revision = 0;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID && dispatched == before);
    r.expected_revision = 40;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_STALE && dispatched == before);
    r.expected_revision = 41;
    r.classification = AZ_CUE_MEMORY;
    r.entry_id = 7;
    r.rgb = 0x123456;
    r.position_s = 1.25;
    r.end_s = 2.5;
    r.beats = 3.5;
    memset(r.comment, 'x', sizeof r.comment);
    memcpy(r.comment, "saved cue", 10);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && dispatched == before + 1);
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED && out.revision == 42);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    assert(copied_cue.size == sizeof copied_cue && copied_cue.operation == r.operation && copied_cue.expected_revision == 41);
    assert(copied_cue.flags == r.flags && copied_cue.classification == r.classification && copied_cue.entry_id == 7 && copied_cue.rgb == 0x123456);
    assert(copied_cue.position_s == 1.25 && copied_cue.end_s == 2.5 && copied_cue.beats == 3.5);
    assert(copied_cue.identity.deck == 2 && copied_cue.identity.load_generation == 1 && copied_cue.identity.deadline_ns == UINT64_MAX && copied_cue.identity.token);
    assert(!strcmp(copied_cue.comment, "saved cue"));
    for (unsigned i = 10; i < sizeof copied_cue.comment; ++i) assert(!copied_cue.comment[i]);
    memset(r.comment, 0, sizeof r.comment);
    r.request_id = 6;
    before = dispatched;
    for (unsigned bad = 0; bad < 7; ++bad) {
        az_command_request malformed = r;
        switch (bad) {
        case 0: malformed.classification = AZ_CUE_MEMORY + 1; break;
        case 1: malformed.rgb |= 0xff000000u; break;
        case 2: memset(malformed.comment, 'x', sizeof malformed.comment); break;
        case 3: malformed.position_s = NAN; break;
        case 4: malformed.flags |= 0x80u; break;
        case 5: malformed.quantization = AZ_QUANTIZE_ON; break;
        case 6: malformed.operation = AZ_CMD_LOOP_EXIT; break;
        }
        assert(sdk_exit_public_submit("one", &malformed, &out) == AZ_INVALID && dispatched == before);
    }
    cue_target.revision_valid = 0;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_STALE && dispatched == before);
    cue_target.revision_valid = 1;
    cue_target_rc = AZ_STALE;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_STALE && dispatched == before);
    cue_target_rc = AZ_OK;
    cue_target.reserved = 1;
    assert(sdk_exit_public_submit("one", &r, &out) != AZ_OK && dispatched == before);
    cue_target.reserved = 0;
    /* A non-revision/non-persistent command is controlled by provider masks too. */
    r = request(6, AZ_CMD_HOT_TRIGGER);
    r.expected_revision = 0;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && dispatched == before + 1);
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    /* Revalidation reads fresh native metadata rather than reusing admission. */
    cue_target_mutation = 1;
    cue_phase_calls = 0;
    r = request(7, AZ_CMD_CUE_COMMENT);
    r.expected_revision = 41;
    r.flags |= AZ_COMMAND_PERSIST;
    before = dispatched;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_REJECTED && out.reason == AZ_STALE);
    assert(dispatched == before && sdk_exit_public_release("one", r.request_id) == AZ_OK);
    cue_target_mutation = 2;
    cue_phase_calls = 0;
    r = request(8, AZ_CMD_HOT_TRIGGER);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_REJECTED && out.reason == AZ_INVALID);
    assert(dispatched == before && sdk_exit_public_release("one", r.request_id) == AZ_OK);
    cue_target_mutation = 0;
    for (unsigned operation = AZ_CMD_SAVED_RECALL; operation <= AZ_CMD_CUE_COLOR; ++operation) {
        uint64_t operation_bit = UINT64_C(1) << operation;
        r = request(9 + operation - AZ_CMD_SAVED_RECALL, operation);
        if (cue_target.persistent_commands & operation_bit) r.flags |= AZ_COMMAND_PERSIST;
        if (cue_target.revision_commands & operation_bit) r.expected_revision = cue_target.revision;
        before = dispatched;
        assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && dispatched == before + 1);
        assert(copied_cue.operation == operation && copied_cue.flags == r.flags && copied_cue.expected_revision == r.expected_revision);
        assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED);
        assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    }
    /* A collection revision cannot fence an unrelated DSP receipt counter. */
    cue_target.revision = 50;
    cue_receipt_revision = 1;
    const unsigned transient[] = {AZ_CMD_MAIN_SET, AZ_CMD_MAIN_RECALL, AZ_CMD_HOT_TRIGGER};
    for (unsigned i = 0; i < sizeof transient / sizeof transient[0]; ++i) {
        r = request(100 + i, transient[i]);
        before = dispatched;
        assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && dispatched == before + 1);
        assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED && out.revision == 1);
        assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    }
    cue_receipt_revision = 0;
    /* Selected persistent commands still require the exact collection revision. */
    r = request(103, AZ_CMD_CUE_COMMENT);
    r.flags |= AZ_COMMAND_PERSIST;
    r.expected_revision = 49;
    before = dispatched;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_STALE && dispatched == before);
    r.expected_revision = 50;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && dispatched == before + 1);
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED && out.revision == 51);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    /* A temporary target lookup failure must retain an admitted request until
     * fresh metadata is available, without dispatch or a new deadline. */
    cue_target.persistent_commands |= UINT64_C(1) << AZ_CMD_MEMORY_DELETE;
    cue_target.revision_commands |= UINT64_C(1) << AZ_CMD_MEMORY_DELETE;
    r = request(104, AZ_CMD_MEMORY_DELETE);
    r.flags |= AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_CUE_IDS;
    r.classification = AZ_CUE_MEMORY;
    r.entry_id = 2;
    r.expected_revision = cue_target.revision;
    cue_target_rc = AZ_BUSY;
    before = dispatched;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED && dispatched == before);
    uint64_t first_observed = out.observed_ns;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED &&
           compare_timestamps(out.observed_ns, first_observed, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL && dispatched == before);
    cue_target_rc = AZ_OK;
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED && dispatched == before + 1);
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED && dispatched == before + 1);
    assert(copied_cue.expected_revision == r.expected_revision &&
           compare_timestamps(copied_cue.identity.deadline_ns, r.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL &&
           copied_cue.entry_id == r.entry_id && copied_cue.identity.load_generation == r.expected_load_generation);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    for (unsigned mismatch = 0; mismatch < 2; ++mismatch) {
        r.request_id = 105 + mismatch;
        cue_target_rc = AZ_BUSY;
        before = dispatched;
        assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED && dispatched == before);
        cue_target_rc = AZ_OK;
        if (mismatch) target_generation = 2;
        else ++cue_target.revision;
        assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_REJECTED && out.reason == AZ_STALE && dispatched == before);
        assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
        target_generation = 1;
        cue_target.revision = r.expected_revision;
    }
    sdk_exit_public_clock(1000);
    r.request_id = 107;
    r.deadline_ns = 2000;
    cue_target_rc = AZ_BUSY;
    before = dispatched;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED);
    sdk_exit_public_clock(2000);
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_CANCELLED && out.reason == AZ_STALE && dispatched == before);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    sdk_exit_public_clock(0);
    r.request_id = 108;
    r.deadline_ns = UINT64_MAX;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED);
    assert(sdk_exit_public_cancel("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_CANCELLED && dispatched == before);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    r.request_id = 109;
    r.expected_load_generation = 2;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_STALE && dispatched == before);
    r.expected_load_generation = 1;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED && dispatched == before);
    assert(sdk_exit_public_cancel("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_CANCELLED && dispatched == before);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    cue_target_rc = AZ_OK;
    assert(cue_targets >= 2 && cue_ticks == 0);
    puts("PASS cue bridge: copied scalar payload, exact old prefixes and dynamic revision/persistence fences");
    puts("PASS loop bridge: exact old prefix, filtered capabilities and scalar extension");
}
