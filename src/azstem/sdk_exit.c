/* SPDX-License-Identifier: GPL-3.0-only */
/* Asynchronous native EXIT/RELOOP. The mailbox owns copied SDK values; native command
 * incarnations carry the request through Quantize, Slip and subsequent chains. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/aztimestamp.h"
#ifdef AZ_STATUS_NATIVE_PRODUCER
#include "../common/azstatus_native.h"
#include "../common/azcuetext.h"
#ifdef AZ_STATUS_MEMORY_PRODUCER
#include "../common/azstatus_memory.h"
#endif
#endif
#include "../common/azexit.h"
#include "../common/azcuerecallbridge.h"
#include "../common/azexitprofile.h"
#include "../common/azcuecommands.h"
#include "../common/azcueprofile.h"
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
#include "../common/azcuerecall.h"
#endif
#include "../common/aznativecommands.h"
#include "../common/azloadobserver.h"
#include "../azmod/azmod-mod.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>
#include <float.h>
#include "../common/aznative_c.h"
#include "../common/aznative_sync.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <dlfcn.h>




#include <time.h>
#if defined(__linux__)
#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <link.h>
#endif
#if defined(__linux__) && defined(__aarch64__) && !defined(AZSDK_EXIT_TESTING)
#include "sdk_vtable_patch.h"
#endif
#include "sdk_exit_profiles.h"

#ifdef AZSDK_EXIT_TESTING
__attribute__((weak)) void azsdk_cue_commands_recall_finished(unsigned deck, uint64_t token) {
    (void) deck;
    (void) token;
}
__attribute__((weak)) int azsdk_exit_test_persistent_cue_submit(const struct azexit_cue_request *request) {
    (void) request;
    return AZ_INVALID;
}
#endif

typedef struct az_owner_gate az_owner_gate;
typedef struct azcue_profile azcue_profile;
typedef struct azexit_binding azexit_binding;
typedef struct azexit_cue_request azexit_cue_request;
typedef struct azexit_cue_stage_sample azexit_cue_stage_sample;
typedef struct azexit_cue_target azexit_cue_target;
typedef struct azexit_delivery_v2 azexit_delivery_v2;
typedef struct azexit_loop_request azexit_loop_request;
typedef struct azexit_native_color azexit_native_color;
typedef struct azexit_profile azexit_profile;
typedef struct azexit_receipt azexit_receipt;
typedef struct azexit_request azexit_request;
typedef struct azexit_result azexit_result;
typedef struct azexit_service azexit_service;
typedef struct azn_hot_details azn_hot_details;
typedef struct azn_hot_link azn_hot_link;
typedef struct azn_hot_slot azn_hot_slot;
typedef struct azsdk_load_sample azsdk_load_sample;
typedef struct azstatus_native_receipt azstatus_native_receipt;
typedef struct LoopPoints LoopPoints;
typedef struct Slot Slot;
typedef struct Setting Setting;
typedef struct HotState HotState;
typedef struct Motion Motion;
typedef struct Node Node;
typedef struct State State;
typedef struct Scope Scope;
typedef struct StatusActionScope StatusActionScope;
typedef struct CausalScope CausalScope;
typedef struct LoopView LoopView;
typedef struct FrameToken FrameToken;
typedef struct FailureScope FailureScope;
typedef struct OwnerFrame OwnerFrame;
typedef struct AdoptionRender AdoptionRender;
typedef struct BeatOwnerFrame BeatOwnerFrame;
typedef struct DurationMutation DurationMutation;
typedef struct DurationRead DurationRead;
typedef struct DurationState DurationState;
typedef struct DurationValue DurationValue;
typedef struct EndClamp EndClamp;
typedef struct EndRead EndRead;
typedef struct EndRestore EndRestore;
typedef struct EndState EndState;
typedef struct HotEdit HotEdit;
typedef struct HotEditScope HotEditScope;
typedef struct KeyCapture KeyCapture;
typedef struct KeyProjection KeyProjection;
typedef struct MapBracket MapBracket;
typedef struct MapClock MapClock;
typedef struct MapMutation MapMutation;
typedef struct NoQuaOrigin NoQuaOrigin;
typedef struct NoQuaProfile NoQuaProfile;
typedef struct RateActiveWrite RateActiveWrite;
typedef struct RateProfile RateProfile;
typedef struct RateRenderScope RateRenderScope;
typedef struct RateState RateState;
typedef struct ReloopEdit ReloopEdit;
typedef struct ReloopState ReloopState;
typedef struct ReloopWrite ReloopWrite;
typedef struct SettingProfile SettingProfile;
typedef struct SliderLockWrite SliderLockWrite;
typedef struct SlipMotionCut SlipMotionCut;
typedef struct SlipProjection SlipProjection;
typedef struct SlipWriter SlipWriter;
typedef struct StatusHead StatusHead;
typedef struct StatusLanding StatusLanding;
typedef struct StatusMotionWrite StatusMotionWrite;
typedef struct SyncProfile SyncProfile;
typedef struct SyncRole SyncRole;
typedef struct SyncRoleWriter SyncRoleWriter;
typedef struct SyncValues SyncValues;
typedef struct TransportProfile TransportProfile;
typedef struct TransportState TransportState;
typedef struct azn_hot_receipt azn_hot_receipt;
typedef struct azn_point azn_point;
typedef struct aznm_packet aznm_packet;
typedef struct azsdk_exit_configure_call azsdk_exit_configure_call;
typedef struct azsdk_exit_register_call azsdk_exit_register_call;
typedef struct execute_call execute_call;
typedef struct execute_move_tail_call execute_move_tail_call;
#include "sdk_exit_delivery.h"


enum {
    IDLE,
    WRITING,
    QUEUED,
    RUNNING,
    DRAINING,
    READY,
    READING
};
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2,
               "EXIT owner callbacks require lock-free mailboxes");
struct LoopPoints {
    bool in_valid, out_valid;
    uint64_t in, out, source;
    uint32_t rate, beat, mode;
    unsigned char identity[12];
};
#ifdef AZ_STATUS_NATIVE_PRODUCER
struct Setting {
    _Atomic(uintptr_t) address;
    _Atomic(uint64_t) provider, fault;
    atomic_flag busy;
    uint64_t seed, sealed_fault;
    unsigned last;
    int raw;
};
#endif
#ifdef AZ_STATUS_NATIVE_PRODUCER
struct HotState {
    atomic_flag busy;
    uint64_t load, seed, provider, serial, loss_cut;
    uintptr_t property[8];
    uint32_t revision[8];
    azn_hot_slot previous[8];
    azn_hot_details details;
    bool ready;
};
#endif
#ifdef AZ_STATUS_NATIVE_PRODUCER
enum {
    MOTION_FREE,
    MOTION_HEAD,
    MOTION_MAP,
    MOTION_OBSERVE
};
struct Motion {

    _Atomic(unsigned) busy;
    _Atomic(unsigned) active_writers;
    _Atomic(uint64_t) active_serial;
    _Atomic(uint64_t) interference;
    _Atomic(unsigned) foreign;
    _Atomic(uint64_t) slip_load, slip_generation, slip_interference;
    _Atomic(uint64_t) landing_order, landing_stamp, landing_epoch, landing_lo;
    _Atomic(uint32_t) landing_hi;
    _Atomic(uint64_t) opening_landing;
    _Atomic(uint64_t) adoption_render;
    _Atomic(unsigned) adoption_render_writers;
    uint64_t adoption_request, adoption_render_cut; /* landing_publish owns these scalars */
    _Atomic(uint64_t) adoption_sample;
    _Atomic(uint32_t) adoption_rate;
    _Atomic(bool) opening;
    _Atomic(unsigned) landing_writers;
    atomic_flag landing_publish;
    uint64_t frame_epoch, frame_landing_order;
    unsigned char first_source[12];
    uint64_t load, order, generation, occurrence, seed, captured_interference;
    uint64_t previous;
    uint64_t stationary_order, stationary_load, stationary_map;
    unsigned stationary_flags;
    uint64_t opening_sample;
    unsigned char identity[12];
    uint32_t rate, window_samples, window_rate;
    uint32_t opening_rate;
    bool known;
};
#endif
struct Slot {
    struct az_owner_gate gate;
    _Atomic(uintptr_t) core, draining_unit; /* opaque identities before gate admission */
    uintptr_t unit, facade;
    _Atomic(unsigned) phase;
    azexit_request request;
    uint32_t operation;
    azexit_loop_request arguments;
    azexit_cue_request cue_arguments;
    unsigned char cue_source_before[12];
    bool cue_seek_seen;
    bool cue_playing_before;
    bool cue_aux_before;
    unsigned cue_rgb_before;
    _Atomic(bool) internal_recall;
#ifdef AZ_STATUS_NATIVE_PRODUCER
    atomic_flag status_busy;
    uint64_t status_epoch;
    uintptr_t status_retained;
    uint32_t status_retained_rev, status_working_rev;
    uint64_t status_seed;
    azn_hot_link working_origin;
    uint64_t working_file_row;
    uint64_t status_loop_cut;
    Setting settings[4];
    HotState hot;
    Motion motion;
#endif
    LoopPoints before, retained_before, main_before;
    uintptr_t retained;
    azexit_cue_stage_sample staged_cue;
    uint64_t expected_in, expected_out;
    unsigned move_stage;
    azexit_result result;
    uint64_t revision; /* owner frame only; never reset on replacement */
    _Atomic(uint64_t) token, lost, failed, review, frame_token;
    _Atomic(unsigned) loss_overflow;
    azsdk_load_sample load;
    bool untracked;
    bool caused;
    bool initially_engaged;
    bool maintenance_seen;
    unsigned captured;
};
struct Node {
    /* Zero is free, one is reserved. Fields are atomic because disposal can run
     * outside the processing thread. No saved pointer is dereferenced. */
    _Atomic(uintptr_t) address;
    _Atomic(uint64_t) token, generation, incarnation;
    /* required: 0 borrowed/superseded, 1 finite effect, 2 proven initial loop setup. */
    _Atomic(unsigned) deck, executed, borrowed, required, type;
    _Atomic(unsigned) maintenance;
};
enum {
    NODE_COUNT = 512
};
struct State {
    Slot decks[4];
    Node nodes[NODE_COUNT];
    _Atomic(uint64_t) incarnation;
    pthread_mutex_t management;
    _Atomic(unsigned) mask, enabled;
    azexit_profile profile;
    uintptr_t bias;
    uintptr_t beat_exit, beat_reset;
    int build;
    void *original, *pre_original;
    void *manager_original, *quantize_original;
    void *cancel_original, *core_jump_original;
    void *hot_countdown_original[2];
    bool hot_countdown_ready;
    bool configured;
#ifdef AZ_STATUS_NATIVE_PRODUCER
    bool motion_hooked;
    bool motion_actions;
#endif
};
struct Scope {
    Slot *slot;
    unsigned deck;
    uint64_t token, generation;
    bool maintenance;
    bool setup;
    uintptr_t command;
};
_Thread_local Scope causal = {0};
_Thread_local Slot *processing = NULL;
#ifdef AZ_STATUS_NATIVE_PRODUCER
struct StatusActionScope {
    StatusActionScope *previous;
    uintptr_t command, callback;
    bool controller, beat;
    uint64_t beat_sample;
    unsigned char beat_source[12];
    unsigned direct_kind;
    uint32_t direct_rate;
    uint64_t direct_sample;
    unsigned char direct_source[12];
};
static _Thread_local StatusActionScope *status_action_current;
static void status_action_drop(StatusActionScope *scope) { status_action_current = scope->previous; }
#define STATUS_ACTION(name, cmd, cb, direct)                                                                                                                               \
    StatusActionScope name __attribute__((cleanup(status_action_drop))) = {.previous = status_action_current, .command = (cmd), .callback = (cb), .controller = (direct)}; \
    status_action_current = &name
static void status_direct_request(StatusActionScope *, const void *, int, uintptr_t);
#endif
struct CausalScope {
    Scope previous;
};
static struct CausalScope causal_begin(Scope value) {
    CausalScope result = {causal};
    causal = value;
    return result;
}
static void causal_drop(CausalScope *scope) { causal = scope->previous; }
#define CAUSAL_SCOPE(name, ...) CausalScope name __attribute__((cleanup(causal_drop))) = causal_begin(__VA_ARGS__)
struct OwnerFrame {
    Slot *previous;
};
static OwnerFrame owner_begin(Slot *current) {
    OwnerFrame out = {processing};
    processing = current;
    return out;
}
static void owner_drop(OwnerFrame *frame) { processing = frame->previous; }
struct FrameToken {
    Slot *slot;
    unsigned deck;
};
static void frame_token_drop(FrameToken *frame) {
    Slot *slot = frame->slot;
    uint64_t token = atomic_exchange_explicit(&slot->frame_token, 0, memory_order_acq_rel);
    if (!token || atomic_load_explicit(&slot->phase, memory_order_acquire) != READY) return;
    if (atomic_load_explicit(&slot->internal_recall, memory_order_acquire)) azsdk_cue_commands_recall_finished(frame->deck, token);
    else delivery_notify(frame->deck);
}
struct FailureScope {
    Slot *slot;
    uint64_t token;
    bool completed;
};
static void failure_drop(FailureScope *scope) {
    if (!scope->completed) atomic_store_explicit(&scope->slot->failed, scope->token ? scope->token : atomic_load_explicit(&scope->slot->token, memory_order_acquire), memory_order_release);
}
static bool future_chain(Slot *, uintptr_t, uintptr_t *, unsigned *);
static bool loop_schedule_matches(Slot *, const void *, const void *, int, const void *, double);
static bool settled_work(Slot *, uint64_t);
static void settle_retired(Slot *, uint64_t);
static void supersede_setup(Slot *);
static bool initial_request(Slot *, const void *);
static bool initial_chain(Slot *, uintptr_t);
static uintptr_t future_hidden(Slot *, uintptr_t);
#ifdef AZ_STATUS_NATIVE_PRODUCER
#define EXIT_SLOT_INIT                                                                                                       \
    {                                                                                                                        \
        .gate = AZ_OWNER_GATE_INIT, .operation = AZ_CMD_LOOP_EXIT, .settings = { [0 ... 3] = {.last = UINT_MAX, .raw = -1} } \
    }
#else
#define EXIT_SLOT_INIT {.gate = AZ_OWNER_GATE_INIT, .operation = AZ_CMD_LOOP_EXIT}
#endif
static State g_exit = {.decks = {[0 ... 3] = EXIT_SLOT_INIT}, .management = PTHREAD_MUTEX_INITIALIZER};
#define state() (g_exit)
__attribute__((destructor)) static void exit_state_drop(void) {
    for (unsigned i = 4; i > 0; --i) az_owner_gate_drop(&g_exit.decks[i - 1].gate);
    if (pthread_mutex_destroy(&g_exit.management)) abort();
}
static void *trampoline_address(void *const *source) {
    void *entry = __atomic_load_n(source, __ATOMIC_ACQUIRE);
    if (!entry) abort();
    return entry;
}
static uint64_t now() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return false;
    uint64_t seconds = ((uint64_t) (ts.tv_sec));
    return seconds <= (UINT64_MAX - (uint64_t) (ts.tv_nsec)) / UINT64_C(1000000000)
               ? seconds * UINT64_C(1000000000) + (uint64_t) (ts.tv_nsec)
               : 0;
}
static uintptr_t word(uintptr_t p) {
    uintptr_t v;
    memcpy(&v, ((void *) (p)), sizeof v);
    return v;
}
static int32_t integer(uintptr_t p) {
    int32_t v;
    memcpy(&v, ((void *) (p)), sizeof v);
    return v;
}
static unsigned byte(uintptr_t p) { return *((const unsigned char *) (p)); }
static bool pointer(uintptr_t p) { return p >= 0x10000 && !(p & 7) && p <= UINT64_C(0x0000ffffffffffff); }
static bool table(uintptr_t object, uintptr_t expected) {
    return pointer(object) && word(object) == expected;
}
/* These fields belong to the constructed unit, protected by its actual teardown gate.
 * Admission alone is intentionally NOT an owner-thread claim. Affinity is checked after
 * the original preProcess has bound the native boxes to its current processing thread. */
static bool admitted(void *unused) {
    (void) unused;
    return true;
}
static bool manager_binding(uintptr_t manager) {
    if (!pointer(manager)) return false;
    uintptr_t adapter = word(manager + 0x10);
    if (!adapter) return true;
    __auto_type p = &(state().profile);
    /* Stock request() recognizes this stateless reference-merging adapter and
     * keeps the chain in the same manager. Other adapters may retain it elsewhere. */
    bool valid = table(adapter, p->merger_table) && word(p->merger_table + 16) == p->merger_request;
    return valid;
}
static bool unit_facade_binding(Slot *slot, const azexit_profile *p, uintptr_t core, uintptr_t bundle, uintptr_t facade) {
    return table(slot->unit, p->unit) && table(core, p->core) && pointer(bundle) &&
           word(slot->unit + 0x128) == core && word(bundle + 0x40) == facade;
}
static bool facade_operations(const azexit_profile *p) {
    return word(p->facade + 3 * 8) == p->exit && word(p->facade + 4 * 8) == p->reloop &&
           word(p->facade + 6 * 8) == p->control_jump && word(p->box + 5 * 8) == p->affinity;
}
static bool coherent(Slot *slot) {
    const azexit_profile *p = &state().profile;
    uintptr_t core = atomic_load_explicit(&slot->core, memory_order_acquire);
    uintptr_t bundle = word(slot->unit + 0x190), facade = slot->facade;
    return unit_facade_binding(slot, p, core, bundle, facade) && table(facade, p->facade) &&
           word(facade + 0x18) == core + 0x110 && word(facade + 0x40) == core && table(core + 0x110, p->box) && facade_operations(p);
}
static bool function_types(const azexit_profile *p, uintptr_t beat, uintptr_t quantize, uintptr_t slip, uintptr_t manager, uintptr_t available) {
    return table(beat, p->beat) && word(p->beat + 16 * 8) == state().beat_exit && word(p->beat + 15 * 8) == state().beat_reset &&
           table(quantize, p->quantize) && table(slip, p->slip) && manager_binding(manager) && pointer(available) && byte(available + 8);
}
static bool basis_binding(const azexit_profile *p, uintptr_t basis, uintptr_t jump, uintptr_t core, uintptr_t quantize, uintptr_t beat) {
    return table(jump, p->jump_table) && word(basis + 0x30) == core && word(jump + 0x10) == core &&
           word(quantize + 0x20) == jump && word(beat + 0x38) == basis && word(beat + 0x48) == core;
}
static bool function_children(uintptr_t facade, uintptr_t functions, uintptr_t beat, uintptr_t quantize, uintptr_t slip) {
    return word(beat + 0x70) == word(facade + 0x68) && word(beat + 0x80) == quantize &&
           pointer(functions) && word(functions + 0x20) == quantize && word(functions + 0x30) == slip && word(slip + 0x78) == quantize;
}
static bool bindings(Slot *slot) {
    const azexit_profile *p = &state().profile;
    uintptr_t f = slot->facade, core = atomic_load_explicit(&slot->core, memory_order_relaxed);
    uintptr_t beat = word(f + 0x70), quantize = word(f + 0x50), slip = word(f + 0xa8);
    uintptr_t manager = word(core + 0xa0), available = word(f + 0x28);
    if (!function_types(p, beat, quantize, slip, manager, available)) return false;
    // These must be the constructed unit's children, not unrelated objects of the same nominal type.
    uintptr_t functions = word(slot->unit + 0x188), basis = word(slot->unit + 0x180);
    if (!pointer(basis)) return false;
    uintptr_t jump = word(basis + 0x38);
    return basis_binding(p, basis, jump, core, quantize, beat) && function_children(f, functions, beat, quantize, slip);
}
static bool quantize_idle(Slot *slot) {
    uintptr_t quantize = word(slot->facade + 0x50);
    if (integer(quantize + 0x98) != -1) return false;
    const unsigned holders[] = {0x30, 0xd8, 0xe0, 0xe8, 0xf0, 0xf8};
    {
        for (unsigned range_index_1 = 0; range_index_1 < sizeof(holders) / sizeof(holders)[0]; ++range_index_1) {
            unsigned offset = holders[range_index_1];
            if (word(quantize + offset)) return false;
        }
    }
    return true;
}
static bool idle(Slot *slot) {
    uintptr_t manager = word(atomic_load_explicit(&(slot->core), memory_order_relaxed) + 0xa0);
    return !word(manager) && !word(manager + 8) && quantize_idle(slot);
}
static bool same_load(unsigned deck, Slot *slot) {
    azsdk_load_sample current = {0};
    return azsdk_load_read((int) (deck), &current) && current.load_generation == slot->load.load_generation &&
           current.unit_generation == slot->load.unit_generation &&
           current.invalidation_epoch == slot->load.invalidation_epoch;
}
static void request_review(Slot *slot, uint64_t token) {
    if (token && atomic_load_explicit(&(slot->token), memory_order_acquire) == token &&
        atomic_load_explicit(&(slot->phase), memory_order_acquire) == RUNNING)
        atomic_store_explicit(&(slot->review), token, memory_order_release);
}
static void mark_lost(Slot *slot, uint64_t token) {
    request_review(slot, token);
    /* Dispatch tokens increase process-wide. An old tombstone must not erase
     * a newer request's failure when it is finally executed or destroyed. */
    uint64_t observed = atomic_load_explicit(&(slot->lost), memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        if (observed >= token || atomic_compare_exchange_strong_explicit(&(slot->lost), &(observed), token, memory_order_release, memory_order_relaxed)) return;
    }
    atomic_store_explicit(&(slot->loss_overflow), 1, memory_order_release);
}
static int command_type(uintptr_t object) {
    if (!pointer(object)) return -1;
    uintptr_t vtable = word(object);
    for (unsigned i = 0; i < AZEXIT_COMMAND_TYPES; ++i)
        if (vtable == state().profile.commands[i].table) return (int) (i);
    return -1;
}
static Node *find_node(uintptr_t address) {
    {
        for (unsigned range_index_2 = 0; range_index_2 < sizeof(state().nodes) / sizeof(state().nodes)[0]; ++range_index_2) {
            __auto_type node = &(state().nodes[range_index_2]);
            if (atomic_load_explicit(&(node->address), memory_order_acquire) == address) return node;
        }
    }
    return NULL;
}
static bool node_matches_causal(Node *node) {
    return atomic_load_explicit(&node->token, memory_order_acquire) == causal.token &&
           atomic_load_explicit(&node->generation, memory_order_acquire) == causal.generation;
}
static bool node_borrowable(Node *node, bool borrowed) {
    return borrowed && atomic_load_explicit(&node->borrowed, memory_order_acquire) &&
           atomic_load_explicit(&node->type, memory_order_acquire) == AZEXIT_QUANTIZE_PENDING &&
           atomic_load_explicit(&node->generation, memory_order_acquire) == causal.generation;
}
static void reuse_captured_node(Node *node, bool borrowed) {
    if (node_matches_causal(node)) return;
    if (node_borrowable(node, borrowed)) {
        // A settled token may yield this same live infrastructure incarnation to a later request.
        atomic_store_explicit(&node->token, causal.token, memory_order_release);
        atomic_store_explicit(&node->executed, 0, memory_order_release);
        atomic_store_explicit(&node->required, 0, memory_order_release);
    } else if (!borrowed) causal.slot->untracked = true;
}
static void qualify_captured_node(Node *node, bool borrowed) {
    if (causal.setup && !borrowed && node_matches_causal(node) && !atomic_load_explicit(&node->executed, memory_order_acquire))
        atomic_store_explicit(&node->required, 2, memory_order_release);
    if (causal.maintenance && node_matches_causal(node)) {
        atomic_store_explicit(&node->maintenance, 1, memory_order_release);
        causal.slot->maintenance_seen = true;
    }
}
static void initialize_captured_node(Node *node, uintptr_t address, bool borrowed, int type) {
    uint64_t incarnation = atomic_fetch_add_explicit(&(state().incarnation), 1, memory_order_relaxed) + 1;
    if (!incarnation) abort();
    atomic_store_explicit(&(node->token), causal.token, memory_order_relaxed);
    atomic_store_explicit(&(node->generation), causal.generation, memory_order_relaxed);
    atomic_store_explicit(&(node->deck), causal.deck, memory_order_relaxed);
    atomic_store_explicit(&(node->executed), 0, memory_order_relaxed);
    atomic_store_explicit(&(node->borrowed), borrowed, memory_order_relaxed);
    atomic_store_explicit(&(node->required), borrowed ? 0 : causal.setup ? 2
                                                                         : 1,
                          memory_order_relaxed);
    atomic_store_explicit(&(node->type), (unsigned) (type), memory_order_relaxed);
    atomic_store_explicit(&(node->maintenance), causal.maintenance, memory_order_relaxed);
    atomic_store_explicit(&(node->incarnation), incarnation, memory_order_relaxed);
    atomic_store_explicit(&(node->address), address, memory_order_release);
    if (!borrowed) ++causal.slot->captured;
    if (causal.maintenance) causal.slot->maintenance_seen = true;
}
static Node *reserve_captured_node(uintptr_t address, bool borrowed, int type) {
    for (unsigned i = 0; i < sizeof state().nodes / sizeof state().nodes[0]; ++i) {
        Node *node = &state().nodes[i];
        uintptr_t empty = 0;
        if (!atomic_compare_exchange_strong_explicit(&node->address, &empty, 1, memory_order_acq_rel, memory_order_acquire)) continue;
        initialize_captured_node(node, address, borrowed, type);
        return node;
    }
    causal.slot->untracked = true;
    return NULL;
}
static Node *capture(uintptr_t address, bool borrowed) {
    if (!address || !causal.slot) return NULL;
    Node *node = find_node(address);
    if (node) {
        reuse_captured_node(node, borrowed);
        qualify_captured_node(node, borrowed);
        return node;
    }
    int type = command_type(address);
    if (type < 0) {
        causal.slot->untracked = true;
        return NULL;
    }
    return reserve_captured_node(address, borrowed, type);
}
static void capture_list(uintptr_t address, bool borrowed) {
    unsigned count = 0;
    while (address && count++ < NODE_COUNT) {
        if (!capture(address, borrowed)) return;
        address = word(address + 0x18);
    }
    if (address && causal.slot) causal.slot->untracked = true;
}
static bool pending_nodes(uint64_t token) {
    {
        for (unsigned range_index_4 = 0; range_index_4 < sizeof(state().nodes) / sizeof(state().nodes)[0]; ++range_index_4) {
            __auto_type node = &(state().nodes[range_index_4]);
            if (atomic_load_explicit(&(node->address), memory_order_acquire) && atomic_load_explicit(&(node->token), memory_order_acquire) == token) return true;
        }
    }
    return false;
}
static unsigned free_nodes() {
    unsigned count = 0;
    {
        for (unsigned range_index_5 = 0; range_index_5 < sizeof(state().nodes) / sizeof(state().nodes)[0]; ++range_index_5) {
            __auto_type node = &(state().nodes[range_index_5]);
            count += atomic_load_explicit(&(node->address), memory_order_acquire) == 0;
        }
    }
    return count;
}
/* EXIT's cancel/reset can change an older QuantizePending without submitting a
 * new chain. Adopt that owner's pending infrastructure and affected callback
 * references; execution supplies the scope for the changed Quantize state. */
static void adopt_manager_chain(Slot *slot, uintptr_t quantize, uintptr_t chain) {
    if (!chain) return;
    if (!table(chain, state().profile.chain_table)) {
        slot->untracked = true;
        return;
    }
    uintptr_t node = word(chain + 0x18);
    for (unsigned n = 0; node && n < NODE_COUNT; ++n) {
        int type = command_type(node);
        if (type < 0) {
            slot->untracked = true;
            break;
        }
        if (type == AZEXIT_QUANTIZE_PENDING && word(node + 0x20) == quantize) (void) capture(node, true);
        node = word(node + 0x18);
    }
}
static void adopt_pending(Slot *slot) {
    uintptr_t quantize = word(slot->facade + 0x50);
    uintptr_t manager = word(atomic_load_explicit(&(slot->core), memory_order_relaxed) + 0xa0);
    {
        unsigned range_values_6[] = {0x30u, 0xd8u, 0xe0u, 0xe8u, 0xf0u, 0xf8u};
        for (unsigned range_index_6 = 0; range_index_6 < sizeof(range_values_6) / sizeof(range_values_6)[0]; ++range_index_6) {
            unsigned offset = range_values_6[range_index_6];
            capture_list(word(quantize + offset), true);
        }
    }
    {
        unsigned range_values_7[] = {0u, 8u};
        for (unsigned range_index_7 = 0; range_index_7 < sizeof(range_values_7) / sizeof(range_values_7)[0]; ++range_index_7) {
            unsigned offset = range_values_7[range_index_7];

            adopt_manager_chain(slot, quantize, word(manager + offset));
        }
    }
}
static void require_pending(const Scope scope) {
    {
        for (unsigned range_index_8 = 0; range_index_8 < sizeof(state().nodes) / sizeof(state().nodes)[0]; ++range_index_8) {
            __auto_type node = &(state().nodes[range_index_8]);
            if (atomic_load_explicit(&(node->address), memory_order_acquire) > 1 &&
                atomic_load_explicit(&(node->token), memory_order_acquire) == scope.token &&
                atomic_load_explicit(&(node->generation), memory_order_acquire) == scope.generation &&
                atomic_load_explicit(&(node->type), memory_order_acquire) == AZEXIT_QUANTIZE_PENDING)
                atomic_store_explicit(&(node->required), 1, memory_order_release);
        }
    }
}
static void quantize_cancel_hook(void *self) {
    bool owned = causal.slot && word(causal.slot->facade + 0x50) == ((uintptr_t) (self));
    if (owned) {
        causal.slot->caused = true;
        adopt_pending(&(*causal.slot));
    }
    ((void (*)(void *)) trampoline_address(&state().cancel_original))(self);
    /* Older work intentionally destroyed by EXIT was borrowed, not a failed
     * new command. uintptr_t retained QuantizePending now carries EXIT's changed state
     * and must execute; replacement before execution is not completion. */
    if (owned) require_pending(causal);
}
static void transfer_borrowed_nodes(Slot *slot, unsigned d, uint64_t generation) {
    {
        for (unsigned range_index_9 = 0; range_index_9 < sizeof(state().nodes) / sizeof(state().nodes)[0]; ++range_index_9) {
            __auto_type node = &(state().nodes[range_index_9]);

            uintptr_t address = atomic_load_explicit(&(node->address), memory_order_acquire);
            if (address <= 1 || atomic_load_explicit(&(node->deck), memory_order_acquire) != d ||
                atomic_load_explicit(&(node->generation), memory_order_acquire) != generation ||
                !atomic_load_explicit(&(node->borrowed), memory_order_acquire) ||
                atomic_load_explicit(&(node->type), memory_order_acquire) != AZEXIT_QUANTIZE_PENDING) continue;
            if (atomic_compare_exchange_strong_explicit(&(node->address), &(address), 1, memory_order_acq_rel, memory_order_acquire)) {
                mark_lost(slot, atomic_load_explicit(&(node->token), memory_order_acquire));
                atomic_store_explicit(&(node->address), 0, memory_order_release);
            }
        }
    }
}
static void transfer_pending(void *quantize) {
    for (unsigned d = 0; d < 4; ++d) {
        Slot *slot = &(state().decks[d]);
        if (!atomic_load_explicit(&(slot->core), memory_order_acquire)) continue;
        AZ_OWNER_LEASE(lease);
        if (!az_owner_held(&slot->gate) && az_owner_enter(&slot->gate, &lease) != AZ_OK) continue;
        if (word(slot->facade + 0x50) != ((uintptr_t) (quantize))) continue;
        uint64_t generation = az_owner_generation(&slot->gate);
        transfer_borrowed_nodes(slot, d, generation);
        return;
    }
}
/* The native manager and callbacks retain actual Command references. Capture
 * them before the original can execute or dispose them, never after return. */
static void manager_hook(void *self, const void *reference) {
    if (causal.slot && word(atomic_load_explicit(&(causal.slot->core), memory_order_relaxed) + 0xa0) == ((uintptr_t) (self))) {
        uintptr_t chain = word(((uintptr_t) (reference)));
        if (table(chain, state().profile.chain_table)) {
            unsigned count = 0;
            Scope accepted = causal;
            accepted.maintenance = causal.maintenance && future_chain(&(*causal.slot), chain, NULL, &(count));
            accepted.setup = causal.setup && initial_chain(&(*causal.slot), chain);
            CAUSAL_SCOPE(scope, accepted);
            capture_list(word(chain + 0x18), false);
            ((void (*)(void *, const void *)) trampoline_address(&state().manager_original))(self, reference);
            return;
        } else causal.slot->untracked = true;
    }
    ((void (*)(void *, const void *)) trampoline_address(&state().manager_original))(self, reference);
}
static int quantize_hook(void *self, int selector, const void *reference, const void *options, const void *property, double timing) {
    bool owned = causal.slot && word(causal.slot->facade + 0x50) == ((uintptr_t) (self));
    if (owned) {
        /* RELOOP can replace a callback on an existing QuantizePending without
         * invoking EXIT's cancel helper or submitting a fresh manager chain. */
        if (causal.slot->operation != AZ_CMD_LOOP_EXIT) adopt_pending(&(*causal.slot));
        capture_list(word(((uintptr_t) (reference))), false);
    }
    int result = ((int (*)(void *, int, const void *, const void *, const void *, double)) trampoline_address(&state().quantize_original))(self, selector, reference, options, property, timing);
    if (owned && causal.slot->operation != AZ_CMD_LOOP_EXIT && result != -1) require_pending(causal);
    /* uintptr_t new native request can reuse an existing QuantizePending allocation.
     * Its bookkeeping then belongs to that request. Only an accepted handoff
     * removes the old SDK scope; SDK-created effect callbacks stay tagged. */
    if (!causal.slot && result != -1) transfer_pending(self);
    return result;
}
#ifdef AZ_STATUS_NATIVE_PRODUCER

struct ExitFlagGuard {
    atomic_flag *flag;
};
static void exit_flag_drop(struct ExitFlagGuard *guard) {
    if (guard->flag) atomic_flag_clear_explicit(guard->flag, memory_order_release);
}
#define EXIT_FLAG_GUARD(name, address) struct ExitFlagGuard name __attribute__((cleanup(exit_flag_drop))) = {address}
struct ExitAtomicGuard {
    _Atomic(unsigned) *value;
};
static void exit_atomic_drop(struct ExitAtomicGuard *guard) { atomic_store_explicit(guard->value, 0, memory_order_release); }
#define EXIT_ATOMIC_GUARD(name, address) struct ExitAtomicGuard name __attribute__((cleanup(exit_atomic_drop))) = {address}
static bool status_entry_matches(uintptr_t address, const void *signature, size_t size, void *retained) {
    return retained || (address && !memcmp((const void *) address, signature, size));
}
static bool status_install_entry(uintptr_t address, void *hook, void **original, azexit_install_fn install) {
    return *original || (install(address, hook, original) && *original);
}
#include "sdk_status_noqua.inc"

#endif
static bool jump_controller_owned(Slot *slot, void *self) {
    return slot && word(word(slot->unit + 0x180) + 0x38) == (uintptr_t) self;
}
#ifdef AZ_STATUS_NATIVE_PRODUCER
static bool jump_status_owner(uintptr_t basis, void *self) {
    return pointer(basis) && word(basis + 0x38) == (uintptr_t) self;
}
static bool jump_status_context(void *self, const void *reference, uintptr_t *callback) {
    uintptr_t basis = processing && state().motion_actions ? word(processing->unit + 0x180) : 0;
    if (pointer(basis) && reference && word(basis + 0x38) == (uintptr_t) self) *callback = word((uintptr_t) reference);
    return jump_status_owner(basis, self);
}
#endif
static void jump_hook(void *self, const void *property, int mode, const void *reference, bool option, double timing) {
    Scope next = causal;
    next.setup = jump_controller_owned(causal.slot, self) &&
                 initial_request(&(*causal.slot), reference);
    CAUSAL_SCOPE(scope, next);
    if (jump_controller_owned(next.slot, self))
        capture_list(word(((uintptr_t) (reference))), false);
#ifdef AZ_STATUS_NATIVE_PRODUCER
    uintptr_t callback = 0;
    bool observed_owner = jump_status_context(self, reference, &callback);
    STATUS_ACTION(observed, 0, callback, observed_owner);
    status_noqua_request(&(observed), property, mode, ((uintptr_t) (__builtin_return_address(0))));
    status_direct_request(&(observed), property, mode, ((uintptr_t) (__builtin_return_address(0))));
#endif
    ((void (*)(void *, const void *, int, const void *, bool, double))(state().profile.jump_request))(self, property, mode, reference, option, timing);
}
static void loop_schedule_hook(void *self, const void *forward, const void *reverse, int mode,
                               const void *reference, bool option, double timing) {
    typedef void (*Schedule)(void *, const void *, const void *, int, const void *, bool, double);
    Scope next = causal;
    next.setup = false;
    next.maintenance = causal.slot &&
                       word(word(causal.slot->unit + 0x180) + 0x38) == ((uintptr_t) (self)) &&
                       loop_schedule_matches(&(*causal.slot), forward, reverse, mode, reference, timing);
    CAUSAL_SCOPE(scope, next);
    if (next.maintenance) capture_list(future_hidden(&(*next.slot), word(((uintptr_t) (reference)))), false);
    ((Schedule) (state().profile.jump_schedule))(self, forward, reverse, mode, reference, option, timing);
}
static bool hot_edit(uint32_t operation) {
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
    return operation == AZ_CMD_HOT_SET || operation == AZ_CMD_HOT_CLEAR;
#else
    (void) operation;
    return false;
#endif
}
static bool recall_operation(uint32_t operation) {
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
    return operation == AZ_CMD_MEMORY_RECALL || operation == AZ_CMD_SAVED_RECALL;
#else
    (void) operation;
    return false;
#endif
}
static bool hot_install(uint32_t operation) {
    return operation == AZ_CMD_CUE_POINT_SET || operation == AZ_CMD_CUE_LOOP_SET;
}
static bool cue_operation(uint32_t operation) {
    return hot_install(operation) || hot_edit(operation) || recall_operation(operation) || operation == AZ_CMD_MAIN_SET || operation == AZ_CMD_MAIN_RECALL || operation == AZ_CMD_HOT_TRIGGER;
}
static bool cue_seek_operation(uint32_t operation) {
    return recall_operation(operation) || operation == AZ_CMD_MAIN_RECALL || operation == AZ_CMD_HOT_TRIGGER;
}
static bool cue_position_matches(const Slot *slot) {
    uintptr_t core = atomic_load_explicit(&(slot->core), memory_order_relaxed);
    return slot->retained_before.in_valid &&
           !memcmp(((const void *) (core + 0x18)), slot->retained_before.identity, 12) &&
           compare_timestamps(word(core + 0x28), slot->retained_before.in,
                              slot->retained_before.rate, slot->retained_before.rate, 1) == AZ_TIME_EQUAL;
}
#ifdef AZ_STATUS_NATIVE_PRODUCER

#include "sdk_status_rate.inc"


#include "sdk_status_transport.inc"


#include "sdk_status_motion.inc"

#endif
static bool hot_jump_command_matches(Slot *slot, uintptr_t command, void *self) {
    return table(command, state().profile.commands[0].table) && word(command + 0x20) == (uintptr_t) self &&
           table(command + 0x28, state().profile.interval_source_table) &&
           !memcmp((const void *) (command + 0x30), slot->retained_before.identity, 12);
}
static bool hot_jump_targets(Slot *slot, uint64_t target, uint64_t forward, uint64_t reverse, uint64_t rate, uint64_t window) {
    return compare_timestamps(slot->retained_before.in, forward, rate, rate, window) == AZ_TIME_EQUAL &&
           compare_timestamps(slot->retained_before.in, reverse, rate, rate, window) == AZ_TIME_EQUAL &&
           compare_timestamps(target, forward, rate, rate, window) == AZ_TIME_EQUAL &&
           compare_timestamps(target, reverse, rate, rate, window) == AZ_TIME_EQUAL;
}
static bool hot_jump_window(Slot *slot, uintptr_t command, uint64_t target, uint64_t *window) {
    uint64_t forward = word(command + 0x40), reverse = word(command + 0x48);
    const uint64_t rate = slot->retained_before.rate;
    int order = compare_timestamps(forward, reverse, rate, rate, 1);
    int forward_range = compare_timestamps(forward, UINT64_C(9007199254740992), rate, rate, 1);
    int reverse_range = compare_timestamps(reverse, UINT64_C(9007199254740992), rate, rate, 1);
    if (forward_range == AZ_TIME_INVALID || forward_range > AZ_TIME_EQUAL || reverse_range == AZ_TIME_INVALID ||
        reverse_range > AZ_TIME_EQUAL || order == AZ_TIME_INVALID) return false;
    *window = (order == AZ_TIME_BEFORE ? reverse - forward : forward - reverse) + 1;
    return hot_jump_targets(slot, target, forward, reverse, rate, *window);
}
static bool planned_cue_jump(Slot *slot, void *self, uint64_t target, uint64_t *window) {
    // Borrow the native forward/reverse window only while this request's exact command executes.
    if (!slot || slot->operation != AZ_CMD_HOT_TRIGGER || !causal.command) return true;
    return hot_jump_command_matches(slot, causal.command, self) && hot_jump_window(slot, causal.command, target, window);
}
static bool cue_jump_owned(Slot *slot, void *self, const void *position, uint64_t target, uint64_t window, bool planned) {
    return slot && cue_seek_operation(slot->operation) && atomic_load_explicit(&slot->core, memory_order_relaxed) == (uintptr_t) self &&
           atomic_load_explicit(&slot->token, memory_order_acquire) == causal.token && atomic_load_explicit(&slot->phase, memory_order_acquire) == RUNNING &&
           !memcmp((const unsigned char *) position + 8, slot->retained_before.identity, 12) && planned &&
           compare_timestamps(target, slot->retained_before.in, slot->retained_before.rate, slot->retained_before.rate, window) == AZ_TIME_EQUAL;
}
static bool cue_jump_committed(Slot *slot, uint64_t target, uint64_t window) {
    return same_load(causal.deck, slot) &&
           !memcmp(((const void *) (atomic_load_explicit(&(slot->core), memory_order_relaxed) + 0x18)), slot->retained_before.identity, 12) &&
           compare_timestamps(word(atomic_load_explicit(&(slot->core), memory_order_relaxed) + 0x28), target,
                              slot->retained_before.rate, slot->retained_before.rate, window) == AZ_TIME_EQUAL;
}
static void core_jump_hook(void *self, const void *position) {
#ifdef AZ_STATUS_NATIVE_PRODUCER
    StatusMotionWrite motion __attribute__((cleanup(status_motion_write_drop)));
    status_motion_write_init(&motion, self, true, 1);
    StatusLanding landing __attribute__((cleanup(status_landing_drop)));
    status_landing_init(&landing, self);
#endif
    Slot *slot = causal.slot;
    uint64_t target = word((uintptr_t) position + 0x18), window = 1;
    bool planned = planned_cue_jump(slot, self, target, &window);
    bool owned = cue_jump_owned(slot, self, position, target, window, planned);
    ((void (*)(void *, const void *)) trampoline_address(&state().core_jump_original))(self, position);
#ifdef AZ_STATUS_NATIVE_PRODUCER
    status_landing_finish(&landing, ((uintptr_t) (position)));
    status_motion_write_finish(&motion, ((uintptr_t) (position)));
#endif
    if (owned && cue_jump_committed(slot, target, window)) slot->cue_seek_seen = true;
}
static uint32_t jump_cancel_hook(void *self) {
    if (causal.slot && word(word(causal.slot->unit + 0x180) + 0x38) == ((uintptr_t) (self)))
        supersede_setup(&(*causal.slot));
    return ((uint32_t (*)(void *))(state().profile.jump_cancel))(self);
}
static bool untracked_command(void *self, int type) {
    bool result = ((bool (*)(void *)) state().profile.commands[type].execute)(self);
    if (processing && type == AZEXIT_QUANTIZE_PENDING) request_review(processing, atomic_load_explicit(&processing->token, memory_order_acquire));
    return result;
}
static bool command_owner_valid(Slot *slot, unsigned deck, uint64_t token, uint64_t generation, struct az_owner_lease *lease) {
    bool held = az_owner_held(&slot->gate);
    if ((!held && az_owner_enter(&slot->gate, lease) != AZ_OK) || az_owner_generation(&slot->gate) != generation ||
        atomic_load_explicit(&(slot->token), memory_order_acquire) != token || atomic_load_explicit(&(slot->phase), memory_order_acquire) != RUNNING ||
        !same_load(deck, &((*slot)))) {
        mark_lost(slot, token);
        return false; /* a retired incarnation never follows raw native owners */
    }
    return true;
}
static bool execute_captured_command(void *self, uintptr_t address, int type, Node *node) {
    uint64_t incarnation = atomic_load_explicit(&(node->incarnation), memory_order_acquire);
    unsigned deck = atomic_load_explicit(&(node->deck), memory_order_acquire);
    uint64_t token = atomic_load_explicit(&(node->token), memory_order_acquire), generation = atomic_load_explicit(&(node->generation), memory_order_acquire);
    if (atomic_load_explicit(&(node->address), memory_order_acquire) != address ||
        atomic_load_explicit(&(node->incarnation), memory_order_acquire) != incarnation) return false;
    Slot *slot = &(state().decks[deck]);
    AZ_OWNER_LEASE(lease);
    if (!command_owner_valid(slot, deck, token, generation, &lease)) return false;
    uint64_t time = now();
    if (azsdk_exit_delivery_continue(&slot->request, time) != AZ_OK ||
        ((bool (*)(void *))(state().profile.affinity))(((void *) (atomic_load_explicit(&(slot->core), memory_order_relaxed) + 0x110)))) {
        mark_lost(slot, token);
        return false;
    }
    request_review(slot, token);
    atomic_store_explicit(&(node->executed), 1, memory_order_release);
    CAUSAL_SCOPE(scope, (Scope) {slot, deck, token, generation, false, false, address});
    {
        FailureScope failure __attribute__((cleanup(failure_drop))) = {slot, token, false};

        bool result = ((bool (*)(void *))(state().profile.commands[type].execute))(self);

        failure.completed = true;
        return result;
    }
}
static bool command_hook(void *self) {
    uintptr_t address = ((uintptr_t) (self));
    int type = command_type(address);
    if (type < 0) abort(); /* only verified nominal slots are patched */
#ifdef AZ_STATUS_NATIVE_PRODUCER
    STATUS_ACTION(observed, type == 0 && processing ? address : 0, 0, false);
    if (!(atomic_load_explicit(&(state().enabled), memory_order_acquire) & ~AZEXIT_PASSIVE_STATUS))
        return ((bool (*)(void *))(state().profile.commands[type].execute))(self);
#endif
    Node *node = find_node(address);
    if (!node && causal.slot) node = capture(address, false);
    if (!node) return untracked_command(self, type);
    return execute_captured_command(self, address, type, node);
}
static void destroy_command(void *self, bool deleting) {
    uintptr_t address = ((uintptr_t) (self));
    int type = command_type(address);
    if (type < 0) abort();
    Node *disposed = NULL;
    uint64_t token = 0;
    unsigned deck = 0;
    {
        Node *node = find_node(address);
        if (node) {
            uintptr_t expected = address;
            if (atomic_compare_exchange_strong_explicit(&(node->address), &(expected), 1, memory_order_acq_rel, memory_order_acquire)) {
                disposed = node;
                token = atomic_load_explicit(&(node->token), memory_order_acquire);
                deck = atomic_load_explicit(&(node->deck), memory_order_acquire);
                if (!atomic_load_explicit(&(node->executed), memory_order_acquire) && atomic_load_explicit(&(node->required), memory_order_acquire) &&
                    !atomic_load_explicit(&(node->maintenance), memory_order_acquire))
                    mark_lost(&(state().decks[deck]), token);
            }
        }
    }
    __auto_type p = &(state().profile.commands[type]);
    ((void (*)(void *))(deleting ? p->deleting : p->destroy))(self);
    if (disposed) {
        atomic_store_explicit(&(disposed->address), 0, memory_order_release);
        request_review(&(state().decks[deck]), token);
        settle_retired(&(state().decks[deck]), token);
    }
}
static void destroy_hook(void *self) { destroy_command(self, false); }
static void deleting_hook(void *self) { destroy_command(self, true); }
/* Native CueProperty::isActiveLoopPlay, exactly mirrored on both stock ELFs:
 * 13ec228 / 14a7668, over the working property at context+160. */
static bool engaged(Slot *slot, bool *value) {
    uintptr_t context = word(slot->facade + 0x68);
    if (!pointer(context)) return false;
    uintptr_t p = context + 0x160;
    (*value) = byte(p + 0x30) && (byte(p + 0x34) & 0x1f) && byte(p + 0x36) &&
               integer(p + 0x38) != 0 && word(p + 0x40) != UINT64_C(0x7fffffffffffffff);
    return true;
}

static bool extended(uint32_t operation) {
    return operation >= AZ_CMD_LOOP_IN && operation <= AZ_CMD_LOOP_ADJUST_OUT &&
           operation != AZ_CMD_LOOP_EXIT && operation != AZ_CMD_RELOOP;
}
static bool entering(uint32_t operation) { return operation == AZ_CMD_RELOOP || operation == AZ_CMD_LOOP_ENTER; }
static bool setter_operations(const azexit_profile *p) {
    return word(p->set_table + 2 * 8) == p->set_point &&
           word(p->set_table + 5 * 8) == p->clear_cue && word(p->set_table + 9 * 8) == p->loop_out &&
           word(p->set_table + 14 * 8) == p->adjust_in && word(p->set_table + 15 * 8) == p->adjust_out &&
           word(p->set_table + 16 * 8) == p->length_beat && word(p->set_table + 17 * 8) == p->length_square;
}
static bool set_bindings(Slot *slot) {
    __auto_type p = &(state().profile);
    uintptr_t setter = slot->facade + 0xc8;
    return table(setter, p->set_table) &&
           word(setter + 0x10) == atomic_load_explicit(&(slot->core), memory_order_relaxed) + 0x110 &&
           word(setter + 0x20) == word(slot->facade + 0x28) &&
           word(setter + 0x48) == word(slot->facade + 0x50) &&
           word(setter + 0x60) == word(slot->facade + 0x68) && setter_operations(p);
}
static bool valid_position(uintptr_t source) {
    const unsigned rate = (unsigned) (integer(source + 0x30));
    const int range = compare_timestamps(word(source + 16), UINT64_C(9007199254740992), rate, rate, 1);
    return rate && rate < 0x7fffffffu && byte(source) && (byte(source + 4) & 0x1f) && byte(source + 6) &&
           integer(source + 8) != 0 && range != AZ_TIME_INVALID && range <= AZ_TIME_EQUAL;
}
static bool empty_position(uintptr_t source) {
    /* Native empty constructor values are absence markers, not timestamps.
     * uintptr_t malformed or foreign populated source must still refuse HOT_CLEAR. */
    return !word(source) && !integer(source + 8) &&
           word(source + 16) == UINT64_C(0x7fffffffffffffff) &&
           !word(source + 0x20) && integer(source + 0x30) == 0x7fffffff &&
           word(source + 0x38) == UINT64_C(0x7fffffffffffffff);
}
static LoopPoints points(uintptr_t property) {
    LoopPoints result = {0};
    memcpy(result.identity, ((const void *) (property + 0x30)), 12);
    result.in_valid = valid_position(property + 0x30);
    result.out_valid = valid_position(property + 0x88);
    result.in = word(property + 0x40);
    result.out = word(property + 0x98);
    result.rate = (unsigned) (integer(property + 0x60));
    result.source = word(property + 0x50);
    result.beat = (unsigned) (integer(property + 0xd8));
    result.mode = (unsigned) (integer(property + 0xc8));
    if (!result.rate || result.rate >= 0x7fffffffu || !result.source) result.in_valid = result.out_valid = false;
    /* Stored-cue reconstruction can allocate a separate Source object for
     * each endpoint. SourceInfo and rate identify the same native source;
     * the two counted object addresses need not be identical. */
    if (memcmp(result.identity, ((const void *) (property + 0x88)), 12) ||
        (unsigned) (integer(property + 0xb8)) != result.rate || !word(property + 0xa8))
        result.out_valid = false;
    return result;
}
static uintptr_t working(Slot *slot) { return word(slot->facade + 0x68) + 0x160; }
static bool complete_points(const LoopPoints p) {
    const int order = compare_timestamps(p.out, p.in, p.rate, p.rate, 1);
    return p.in_valid && p.out_valid && order != AZ_TIME_INVALID && order == AZ_TIME_AFTER;
}
static bool same_source(const LoopPoints a, const LoopPoints b) { return a.rate == b.rate && a.source == b.source && !memcmp(a.identity, b.identity, 12); }
struct LoopView {
    uintptr_t property, holder;
    LoopPoints points;
};
static bool loop_view(Slot *slot, LoopView *view) {
    bool active = false;
    if (!engaged(slot, &(active)) || !active) return false;
    view->property = working(slot);
    view->points = points(view->property);
    view->holder = word(view->property + 0x58);
    return complete_points(view->points);
}
static bool interval_binding(uintptr_t object, const LoopView view, bool sourced) {
    __auto_type p = &(state().profile);
    if (!table(object, sourced ? p->interval_source_table : p->interval_table) ||
        memcmp(((void *) (object + 8)), view.points.identity, 12) ||
        (sourced && word(object + 0x28) != view.holder)) return false;
    return true;
}
static bool interval_matches(uintptr_t object, const LoopView view, bool reverse, bool sourced) {
    if (!interval_binding(object, view, sourced)) return false;
    const int begin = compare_timestamps(word(object + 0x18), reverse ? view.points.out : view.points.in, view.points.rate, view.points.rate, 1);
    const int end = compare_timestamps(word(object + 0x20), reverse ? view.points.in : view.points.out, view.points.rate, view.points.rate, 1);
    return begin != AZ_TIME_INVALID && end != AZ_TIME_INVALID && begin == AZ_TIME_EQUAL && end == AZ_TIME_EQUAL;
}
static bool loop_callback(Slot *slot, uintptr_t loop, const LoopView view, uintptr_t *play) {
    __auto_type p = &(state().profile);
    if (!table(loop, p->commands[3].table) || word(loop + 0x20) != word(slot->unit + 0x180) ||
        word(loop + 0x28) != view.property || !interval_matches(loop + 0x38, view, false, true) ||
        !interval_matches(loop + 0x68, view, true, false)) return false;
    (*play) = word(loop + 0x18);
    return !(*play) || (table((*play), p->commands[1].table) &&
                        word((*play) + 0x20) == atomic_load_explicit(&(slot->core), memory_order_relaxed) && !word((*play) + 0x18));
}
/* Quantize's interval request wraps an owner-held Loop callback. The visible
 * wrapper and its hidden callback are both stock future playback work; the
 * selector and exact embedded owner distinguish them from a pending SDK effect. */
static bool future_wrapper(Slot *slot, uintptr_t wrapper) {
    __auto_type p = &(state().profile);
    uintptr_t quantize = word(slot->facade + 0x50), embedded = quantize + 0x20;
    if (!table(wrapper, p->commands[13].table) || word(wrapper + 0x20) != embedded ||
        integer(wrapper + 0x28) != 2 || integer(embedded + 8) != 2 ||
        word(embedded) != word(word(slot->unit + 0x180) + 0x38) || word(embedded + 0x20) != quantize) return false;
    return true;
}
static bool future_play(Slot *slot, uintptr_t wrapper) {
    const azexit_profile *p = &state().profile;
    uintptr_t play = word(wrapper + 0x18);
    if (play && (!table(play, p->commands[1].table) ||
                 word(play + 0x20) != atomic_load_explicit(&(slot->core), memory_order_relaxed) || word(play + 0x18))) return false;
    return true;
}
static uintptr_t future_hidden(Slot *slot, uintptr_t wrapper) {
    if (!future_wrapper(slot, wrapper) || !future_play(slot, wrapper)) return 0;
    uintptr_t embedded = word(slot->facade + 0x50) + 0x20;
    LoopView view = {0};
    uintptr_t hidden = word(embedded + 0x10), hidden_play = 0;
    return loop_view(slot, &(view)) && loop_callback(slot, hidden, view, &(hidden_play)) && !hidden_play ? hidden : 0;
}
static bool loop_schedule_matches(Slot *slot, const void *forward, const void *reverse, int mode, const void *reference, double timing) {
    LoopView view = {0};
    uintptr_t play = 0;
    return mode == 0 && timing == DBL_MAX && loop_view(slot, &(view)) &&
           interval_matches(((uintptr_t) (forward)), view, false, true) &&
           interval_matches(((uintptr_t) (reverse)), view, true, false) &&
           pointer(((uintptr_t) (reference))) &&
           (loop_callback(slot, word(((uintptr_t) (reference))), view, &(play)) ||
            future_hidden(slot, word(((uintptr_t) (reference)))));
}
#ifdef AZ_STATUS_NATIVE_PRODUCER
/* uintptr_t classified occurrence requires the stock JumpCommand or direct JumpController
 * scope which is currently executing, the exact callback owner, and the actual
 * setCurrentPos commit. Loop active plus a negative displacement cannot qualify. */
static bool status_jump_origin(const Slot *slot) {
    __auto_type scope = status_action_current;
    return scope && (scope->controller || (scope->command && table(scope->command, state().profile.commands[0].table) &&
                                           word(scope->command + 0x20) == atomic_load_explicit(&(slot->core), memory_order_acquire)));
}
static bool status_direct_scope(StatusActionScope *out, Slot *slot, uintptr_t pos) {
    return slot && out->controller && !out->callback && !out->command && !out->beat && pointer(pos);
}
static bool status_direct_binding(State *s, Slot *slot) {
    return s->build >= 1 && s->build <= 2 && coherent(slot) && set_bindings(slot);
}
static bool status_direct_pause(State *s, uintptr_t core, uintptr_t basis, uintptr_t pause) {
    return pointer(basis) && table(pause, s->profile.play_pause_table) && word(pause + 0x58) == core;
}
static bool status_direct_seek(State *s, uintptr_t pos, int mode, uintptr_t caller, uintptr_t pause, uintptr_t controller) {
    uintptr_t expected = s->bias + (s->build == 1 ? 0x1421d08u : 0x14dd4b0u);
    return mode == 2 && caller == expected && pos == pause + 0x10 && word(pause + 0x50) == controller;
}
static bool status_direct_quantize(State *s, Slot *slot, uintptr_t caller, uintptr_t pause, uintptr_t controller) {
    const NoQuaProfile *p = &noqua_profiles[s->build - 1];
    uintptr_t quantize = word(slot->facade + 0x50);
    return (caller == s->bias + p->jump_returns[0] || caller == s->bias + p->jump_returns[1]) &&
           table(quantize, s->profile.quantize) && word(quantize + 0x20) == controller &&
           word(quantize + 0x40) == quantize && word(quantize + 0x48) == pause;
}
static bool status_direct_loop_position(Slot *slot, uintptr_t pos, uint32_t rate) {
    LoopView view = {0};
    if (!loop_view(slot, &view) || view.points.rate != rate || memcmp((const void *) (pos + 8), view.points.identity, 12)) return false;
    int in = compare_timestamps(word(pos + 0x18), view.points.in, view.points.rate, view.points.rate, 1);
    int end = compare_timestamps(word(pos + 0x18), view.points.out, view.points.rate, view.points.rate, 1);
    return in != AZ_TIME_INVALID && end != AZ_TIME_INVALID && in >= AZ_TIME_EQUAL && end <= AZ_TIME_EQUAL;
}
static unsigned status_direct_kind(State *s, Slot *slot, uintptr_t pos, int mode, uintptr_t caller, uint32_t rate, uintptr_t pause, uintptr_t controller) {
    // PlayPause commits its stored position through its own JumpController.
    if (status_direct_seek(s, pos, mode, caller, pause, controller)) return AZN_SEEK;
    if (mode != 0 || !status_direct_quantize(s, slot, caller, pause, controller) || !status_direct_loop_position(slot, pos, rate)) return 0;
    // Quantize can preserve an offset inside a loop; require this requested point's exact later commit.
    return AZN_JUMP;
}
static bool status_direct_load(State *s, Slot *slot, uintptr_t pos) {
    azsdk_load_sample load = {0};
    return azsdk_load_read((int) (slot - s->decks), &load) &&
           azn_load_track_matches((unsigned) (slot - s->decks), load.invalidation_epoch, load.load_generation, (const unsigned char *) (pos + 8));
}
static void status_direct_request(StatusActionScope *out, const void *position, int mode, uintptr_t caller) {
    State *s = &state();
    Slot *slot = processing;
    uintptr_t pos = (uintptr_t) position;
    if (!status_direct_scope(out, slot, pos) || !status_direct_binding(s, slot)) return;
    uintptr_t core = atomic_load_explicit(&slot->core, memory_order_acquire);
    uint32_t rate = (uint32_t) integer(core + 0x68);
    uintptr_t basis = word(slot->unit + 0x180), pause = word(slot->facade + 0x58);
    if (!status_direct_pause(s, core, basis, pause)) return;
    uintptr_t controller = word(basis + 0x38);
    unsigned kind = status_direct_kind(s, slot, pos, mode, caller, rate, pause, controller);
    if (!kind || !status_direct_load(s, slot, pos)) return;
    out->direct_kind = kind;
    out->direct_rate = rate;
    out->direct_sample = word(pos + 0x18);
    memcpy(out->direct_source, (const void *) (pos + 8), sizeof out->direct_source);
}
typedef struct StatusDecision {
    bool done;
    unsigned kind;
} StatusDecision;
static StatusDecision status_beat_action(StatusActionScope *scope, const StatusHead to) {
    if (!scope->beat) return (StatusDecision) {0};
    int order = compare_timestamps(scope->beat_sample, to.sample, to.rate, to.rate, 1);
    if (order == AZ_TIME_INVALID) return (StatusDecision) {true, 0};
    if (order == AZ_TIME_EQUAL && !memcmp(scope->beat_source, to.source, 12)) return (StatusDecision) {true, AZN_JUMP};
    return (StatusDecision) {0};
}
static StatusDecision status_direct_action(StatusActionScope *scope, const StatusHead to) {
    if (!scope->direct_kind) return (StatusDecision) {0};
    if (!scope->direct_rate || scope->direct_rate != to.rate) return (StatusDecision) {true, 0};
    int order = compare_timestamps(scope->direct_sample, to.sample, to.rate, to.rate, 1);
    if (order == AZ_TIME_INVALID) return (StatusDecision) {true, 0};
    if (order == AZ_TIME_EQUAL && !memcmp(scope->direct_source, to.source, 12)) return (StatusDecision) {true, scope->direct_kind};
    return (StatusDecision) {0};
}
static bool status_command_owner(Slot *slot, uintptr_t cmd) {
    return table(cmd, state().profile.commands[0].table) && word(cmd + 0x20) == atomic_load_explicit(&slot->core, memory_order_acquire);
}
static unsigned status_needle_action(Slot *slot, uintptr_t cb) {
    const azexit_profile *p = &state().profile;
    if (!table(cb, p->commands[14].table)) return 0;
    uintptr_t needle_vt = state().bias + (state().build == 1 ? 0x28958d0u : 0x2be8390u);
    uintptr_t owner = word(cb + 0x20), pause = word(slot->facade + 0x58);
    return table(owner, needle_vt) && table(pause, p->play_pause_table) && word(owner + 0x18) == pause &&
                   word(pause + 0x58) == atomic_load_explicit(&slot->core, memory_order_acquire)
               ? AZN_SEEK
               : 0;
}
static unsigned status_beat_callback(Slot *slot, uintptr_t cb) {
    const azexit_profile *p = &state().profile;
    if (!table(cb, p->commands[10].table)) return 0;
    // beatJumpOnQua's callback owns the BeatJumpFunction whose Quantize member performs dispatch.
    uintptr_t owner = word(cb + 0x20), quantize = word(slot->facade + 0x50);
    return pointer(owner) && table(quantize, p->quantize) && word(owner + 0x10) == quantize ? AZN_JUMP : 0;
}
static unsigned status_callback_action(Slot *slot, uintptr_t cb, uintptr_t context) {
    unsigned action = status_needle_action(slot, cb);
    if (action) return action;
    const azexit_profile *p = &state().profile;
    if (table(cb, p->commands[8].table) && word(cb + 0x20) == context) return AZN_JUMP;
    action = status_beat_callback(slot, cb);
    if (action) return action;
    if (table(cb, p->commands[7].table) && word(cb + 0x30) == context && word(cb + 0x38) == word(slot->facade + 0x58)) return AZN_JUMP;
    return 0;
}
static bool status_loop_view(Slot *slot, const StatusHead from, const StatusHead to, LoopView *v) {
    return from.ready && from.rate == to.rate && !memcmp(from.source, to.source, 12) &&
           loop_view(slot, v) && v->points.rate == to.rate && !memcmp(v->points.identity, to.source, 12);
}
static bool status_initial_binding(Slot *slot, uintptr_t cmd, const LoopView v, bool direct_loop, uintptr_t play) {
    return direct_loop && play && slot->motion.window_samples && slot->motion.window_rate &&
           table(cmd + 0x28, state().profile.interval_source_table) && !memcmp((const void *) (cmd + 0x30), v.points.identity, 12) && word(cmd + 0x50) == v.holder;
}
static bool status_initial_shape(int shape, int first_range, int second_range, int first_target, int second_target, int lower, int upper) {
    return shape != AZ_TIME_INVALID && shape == AZ_TIME_AFTER && first_range != AZ_TIME_INVALID && second_range != AZ_TIME_INVALID &&
           first_target != AZ_TIME_INVALID && second_target != AZ_TIME_INVALID && lower != AZ_TIME_INVALID && upper != AZ_TIME_INVALID;
}
static unsigned status_initial_targets(int first_range, int second_range, int first_target, int second_target, int lower, int upper) {
    return first_range <= AZ_TIME_EQUAL && second_range <= AZ_TIME_EQUAL && (first_target == AZ_TIME_EQUAL || second_target == AZ_TIME_EQUAL) &&
                   lower >= AZ_TIME_EQUAL && upper <= AZ_TIME_EQUAL
               ? AZN_JUMP
               : 0;
}
static unsigned status_initial_landing(uintptr_t cmd, const LoopView v, const StatusHead to) {
    uint64_t first = word(cmd + 0x40), second = word(cmd + 0x48);
    int shape = compare_timestamps(first, second, to.rate, to.rate, 1);
    int first_range = compare_timestamps(first, UINT64_C(9007199254740992), to.rate, to.rate, 1);
    int second_range = compare_timestamps(second, UINT64_C(9007199254740992), to.rate, to.rate, 1);
    int first_target = compare_timestamps(to.sample, first, to.rate, to.rate, 1);
    int second_target = compare_timestamps(to.sample, second, to.rate, to.rate, 1);
    int lower = compare_timestamps(to.sample, v.points.in, to.rate, to.rate, 1);
    int upper = compare_timestamps(to.sample, v.points.out, to.rate, to.rate, 1);
    if (!status_initial_shape(shape, first_range, second_range, first_target, second_target, lower, upper)) return 0;
    return status_initial_targets(first_range, second_range, first_target, second_target, lower, upper);
}
static unsigned status_wrap_landing(int out_order, int in_order, int landed_in, int landed_out) {
    if (out_order == AZ_TIME_INVALID || in_order == AZ_TIME_INVALID || landed_in == AZ_TIME_INVALID || landed_out == AZ_TIME_INVALID) return 0;
    bool forward = out_order >= AZ_TIME_EQUAL && landed_in == AZ_TIME_EQUAL;
    bool reverse = in_order <= AZ_TIME_EQUAL && landed_out == AZ_TIME_EQUAL;
    return forward || reverse ? AZN_WRAP : 0;
}
static unsigned status_loop_wrap(Slot *slot, const LoopView v, const StatusHead from, const StatusHead to) {
    // Whole output frames can cross the source boundary; retain the measured departure window.
    Motion *motion = &slot->motion;
    if (!motion->window_samples || !motion->window_rate) return 0;
    int out_order = compare_timestamps(from.sample, v.points.out, from.rate, motion->window_rate, motion->window_samples);
    int in_order = compare_timestamps(from.sample, v.points.in, from.rate, motion->window_rate, motion->window_samples);
    int landed_in = compare_timestamps(to.sample, v.points.in, to.rate, to.rate, 1);
    int landed_out = compare_timestamps(to.sample, v.points.out, to.rate, to.rate, 1);
    return status_wrap_landing(out_order, in_order, landed_in, landed_out);
}
static unsigned status_loop_action(Slot *slot, uintptr_t cmd, uintptr_t cb, const StatusHead from, const StatusHead to) {
    LoopView v = {0};
    if (!cmd || !status_loop_view(slot, from, to, &v)) return 0;
    uintptr_t play = 0;
    bool direct_loop = loop_callback(slot, cb, v, &play);
    if (!direct_loop && !future_hidden(slot, cb)) return 0;
    if (!interval_matches(cmd + 0x28, v, false, true)) {
        // Initial activation's owned Loop+Play callback uses its requested point, not a recurring wrap boundary.
        if (!status_initial_binding(slot, cmd, v, direct_loop, play)) return 0;
        return status_initial_landing(cmd, v, to);
    }
    return status_loop_wrap(slot, v, from, to);
}
static unsigned status_classified_action(Slot *slot, StatusActionScope *scope, uintptr_t cb, uintptr_t cmd, const StatusHead from, const StatusHead to) {
    StatusDecision decision = status_beat_action(scope, to);
    if (decision.done) return decision.kind;
    decision = status_direct_action(scope, to);
    if (decision.done) return decision.kind;
    if (!pointer(cb)) return 0;
    unsigned action = status_callback_action(slot, cb, word(slot->facade + 0x68));
    if (action) return action;
    return status_loop_action(slot, cmd, cb, from, to);
}
static unsigned status_action(Slot *slot, const StatusHead from, const StatusHead to) {
    StatusActionScope *scope = status_action_current;
    if (!scope || !to.ready || !coherent(slot) || !set_bindings(slot)) return 0;
    uintptr_t cb = scope->callback, cmd = scope->command;
    if (cmd) {
        if (!status_command_owner(slot, cmd)) return 0;
        cb = word(cmd + 0x18);
    }
    return status_classified_action(slot, scope, cb, cmd, from, to);
}

#endif
static bool future_chain_view(Slot *slot, uintptr_t chain, LoopView *view) {
    __auto_type p = &(state().profile);
    if (!table(chain, p->chain_table) || !loop_view(slot, view) ||
        !interval_matches(chain + 0x20, *view, true, false) ||
        (byte(chain + 0x48) != 0 && byte(chain + 0x48) != 3) || byte(chain + 0x49)) return false;
    double timing;
    memcpy(&timing, ((void *) (chain + 0x50)), sizeof timing);
    if (timing != DBL_MAX) return false;
    return true;
}
static bool future_chain_addresses(uintptr_t jump, uintptr_t callback, uintptr_t play, uintptr_t hidden, uintptr_t *addresses, unsigned *count) {
    {
        uintptr_t range_values_10[] = {jump, callback, play, hidden};
        for (unsigned range_index_10 = 0; range_index_10 < sizeof(range_values_10) / sizeof(range_values_10)[0]; ++range_index_10) {
            uintptr_t address = range_values_10[range_index_10];

            if (!address) continue;
            if ((*count) >= 8) return false;
            if (addresses) addresses[(*count)] = address;
            ++(*count);
        }
    }
    return true;
}
static bool future_chain(Slot *slot, uintptr_t chain, uintptr_t *addresses, unsigned *count) {
    const azexit_profile *p = &state().profile;
    LoopView view = {0};
    if (!future_chain_view(slot, chain, &view)) return false;
    uintptr_t jump = word(chain + 0x18), play = 0;
    if (!table(jump, p->commands[0].table) || word(jump + 0x20) != atomic_load_explicit(&(slot->core), memory_order_relaxed) ||
        !interval_matches(jump + 0x28, view, false, true)) return false;
    uintptr_t callback = word(jump + 0x18), hidden = 0;
    if (!loop_callback(slot, callback, view, &(play))) {
        hidden = future_hidden(slot, callback);
        if (!hidden) return false;
        play = word(callback + 0x18);
    }
    return future_chain_addresses(jump, callback, play, hidden, addresses, count);
}
static bool initial_request(Slot *slot, const void *reference) {
    LoopView view = {0};
    uintptr_t play = 0;
    return pointer(((uintptr_t) (reference))) && loop_view(slot, &(view)) &&
           loop_callback(slot, word(((uintptr_t) (reference))), view, &(play));
}
static bool outside_range(uint64_t sample, uint32_t rate) {
    const int range = compare_timestamps(sample, UINT64_C(9007199254740992), rate, rate, 1);
    return range == AZ_TIME_INVALID || range > AZ_TIME_EQUAL;
}
static bool initial_chain_view(Slot *slot, uintptr_t chain, LoopView *view) {
    if (!table(chain, state().profile.chain_table) || byte(chain + 0x49) || !loop_view(slot, view)) return false;
    double timing;
    memcpy(&timing, (void *) (chain + 0x50), sizeof timing);
    return timing == DBL_MAX && (byte(chain + 0x48) == 0 || byte(chain + 0x48) == 3);
}
static bool initial_jump(Slot *slot, uintptr_t jump, const LoopView view) {
    const azexit_profile *p = &state().profile;
    return table(jump, p->commands[0].table) && word(jump + 0x20) == atomic_load_explicit(&slot->core, memory_order_relaxed) &&
           table(jump + 0x28, p->interval_source_table) && !memcmp((void *) (jump + 0x30), view.points.identity, 12) &&
           !outside_range(word(jump + 0x40), view.points.rate) && !outside_range(word(jump + 0x48), view.points.rate) && word(jump + 0x50) == view.holder;
}
static bool initial_reverse_interval(uintptr_t chain, const LoopView view) {
    return table(chain + 0x20, state().profile.interval_table) && !memcmp((void *) (chain + 0x28), view.points.identity, 12) &&
           !outside_range(word(chain + 0x38), view.points.rate) && !outside_range(word(chain + 0x40), view.points.rate);
}
static bool initial_chain(Slot *slot, uintptr_t chain) {
    LoopView view = {0};
    if (!initial_chain_view(slot, chain, &view)) return false;
    uintptr_t jump = word(chain + 0x18), play = 0;
    if (!initial_jump(slot, jump, view) || !initial_reverse_interval(chain, view)) return false;
    return loop_callback(slot, word(jump + 0x18), view, &play);
}
/* Native OUT initialization can replace an initial position-jump chain before
 * its first execution. Only its exact Loop callback proves this is loop setup;
 * an ordinary one-shot Jump remains a required effect. The replacement must
 * subsequently produce a verified recurring chain before we can acknowledge it. */
static bool setup_current(Slot *slot) {
    return atomic_load_explicit(&slot->phase, memory_order_acquire) == RUNNING &&
           causal.token == atomic_load_explicit(&slot->token, memory_order_acquire) && causal.generation == az_owner_generation(&slot->gate) &&
           (extended(slot->operation) || entering(slot->operation) || cue_operation(slot->operation));
}
static bool setup_node(Node *node) {
    return node && node_matches_causal(node) && !atomic_load_explicit(&node->executed, memory_order_acquire) &&
           !atomic_load_explicit(&node->borrowed, memory_order_acquire) && atomic_load_explicit(&node->required, memory_order_acquire) == 2;
}
static void supersede_chain(Slot *slot, uintptr_t chain) {
    if (!initial_chain(slot, chain)) return;
    uintptr_t jump = word(chain + 0x18), loop = word(jump + 0x18), play = word(loop + 0x18);
    Node *nodes[3] = {0};
    unsigned count = 0;
    const uintptr_t addresses[] = {jump, loop, play};
    for (unsigned i = 0; i < sizeof addresses / sizeof *addresses; ++i) {
        if (!addresses[i]) continue;
        Node *node = find_node(addresses[i]);
        if (!setup_node(node)) return;
        nodes[count++] = node;
    }
    for (unsigned i = 0; i < count; ++i) atomic_store_explicit(&nodes[i]->required, 0, memory_order_release);
    slot->maintenance_seen = true;
}
static void supersede_setup(Slot *slot) {
    if (!setup_current(slot)) return;
    uintptr_t manager = word(atomic_load_explicit(&slot->core, memory_order_relaxed) + 0xa0);
    const unsigned offsets[] = {0, 8};
    for (unsigned i = 0; i < sizeof offsets / sizeof *offsets; ++i) supersede_chain(slot, word(manager + offsets[i]));
}
static bool settled_quantize(uintptr_t quantize) {
    if (integer(quantize + 0x98) != -1) return false;
    {
        unsigned range_values_13[] = {0xd8u, 0xe0u, 0xe8u, 0xf0u, 0xf8u};
        for (unsigned range_index_13 = 0; range_index_13 < sizeof(range_values_13) / sizeof(range_values_13)[0]; ++range_index_13) {
            unsigned offset = range_values_13[range_index_13];
            if (word(quantize + offset)) return false;
        }
    }
    return true;
}
static bool settled_manager_chains(Slot *slot, uintptr_t *addresses, unsigned *count) {
    uintptr_t manager = word(atomic_load_explicit(&(slot->core), memory_order_relaxed) + 0xa0);
    {
        unsigned range_values_14[] = {0u, 8u};
        for (unsigned range_index_14 = 0; range_index_14 < sizeof(range_values_14) / sizeof(range_values_14)[0]; ++range_index_14) {
            unsigned offset = range_values_14[range_index_14];

            uintptr_t chain = word(manager + offset);
            if (chain && !future_chain(slot, chain, addresses, count)) return false;
        }
    }
    return true;
}
static bool settled_empty(Slot *slot, uint64_t token) {
        // uintptr_t Memory point recall deliberately cancels the active loop's setup.
        // Unlike OUT/RELOOP, it must not produce a replacement wrap. Keep the
        // empty-manager/node proof; cue_postcondition still requires this
        // request's seek, source-bound point, disengagement and transport pause.
    bool point_recall = slot->operation == AZ_CMD_MEMORY_RECALL &&
                        slot->retained_before.in_valid && !slot->retained_before.out_valid;
    return (!slot->maintenance_seen || point_recall) && !pending_nodes(token) && idle(slot);
}
static bool settled_nodes(uint64_t token, uint64_t generation, const uintptr_t *addresses, unsigned count) {
    {
        for (unsigned range_index_15 = 0; range_index_15 < sizeof(state().nodes) / sizeof(state().nodes)[0]; ++range_index_15) {
            __auto_type node = &(state().nodes[range_index_15]);

            uintptr_t address = atomic_load_explicit(&(node->address), memory_order_acquire);
            if (address <= 1 || atomic_load_explicit(&(node->token), memory_order_acquire) != token) continue;
            if (atomic_load_explicit(&(node->generation), memory_order_acquire) != generation || !atomic_load_explicit(&(node->maintenance), memory_order_acquire)) return false;
            bool owned = false;
            for (unsigned i = 0; i < count; ++i) owned |= addresses[i] == address;
            if (!owned) return false;
        }
    }
    return true;
}
static bool release_settled_nodes(uint64_t token, uint64_t generation) {
    {
        for (unsigned range_index_16 = 0; range_index_16 < sizeof(state().nodes) / sizeof(state().nodes)[0]; ++range_index_16) {
            __auto_type node = &(state().nodes[range_index_16]);

            uintptr_t address = atomic_load_explicit(&(node->address), memory_order_acquire);
            if (address <= 1 || atomic_load_explicit(&(node->token), memory_order_acquire) != token ||
                atomic_load_explicit(&(node->generation), memory_order_acquire) != generation) continue;
            if (!atomic_compare_exchange_strong_explicit(&(node->address), &(address), 1, memory_order_acq_rel, memory_order_acquire)) return false;
            atomic_store_explicit(&(node->address), 0, memory_order_release);
        }
    }
    return true;
}
static bool settled_work(Slot *slot, uint64_t token) {
    uintptr_t quantize = word(slot->facade + 0x50);
    if (!settled_quantize(quantize)) return false;
    uintptr_t addresses[8] = {0};
    unsigned count = 0;
    if (!settled_manager_chains(slot, addresses, &count)) return false;
    bool wrapped = false;
    for (unsigned i = 0; i < count; ++i) wrapped |= table(addresses[i], state().profile.commands[13].table);
    if (word(quantize + 0x30) && !wrapped) return false;
    if (!count) return settled_empty(slot, token);
    uint64_t generation = az_owner_generation(&slot->gate);
    if (!settled_nodes(token, generation, addresses, count) || !release_settled_nodes(token, generation)) return false;
    // Return these exact native-held continuations to stock ownership without adding references.
    slot->maintenance_seen = false;
    return true;
}
static bool shifted(uint64_t value, double seconds, uint32_t rate, uint64_t *out) {
    /* Mirror the two verified FCVTZS expression orders; reject ranges that
     * could overflow native signed arithmetic before the firmware sees them. */
    double milliseconds = seconds * 1000.0;
    double samples = state().build == 1 ? (milliseconds * rate) / 1000.0 : (milliseconds * 0.001) * rate;
    const int range = compare_timestamps(fabs(samples), 9007199254740992.0, rate, rate, 1);
    if (range == AZ_TIME_INVALID || range > AZ_TIME_EQUAL) return false;
    int64_t delta = ((int64_t) (samples));
    int64_t target = ((int64_t) (value)) + delta;
    const int movement = compare_timestamps(((long double) (delta)), 0.L, rate, rate, 1);
    const int target_range = compare_timestamps(target, INT64_C(9007199254740992), rate, rate, 1);
    if (movement == AZ_TIME_INVALID || movement == AZ_TIME_EQUAL ||
        target_range == AZ_TIME_INVALID || target_range > AZ_TIME_EQUAL) return false;
    (*out) = (uint64_t) (target);
    return true;
}
static void adjust(Slot *slot, bool in, double seconds) {
    void *setter = ((void *) (slot->facade + 0xc8));
    __auto_type p = &(state().profile);
    /* Verified policy0 selects the native 13.33ms minimum. LoopType1 skips
     * copying working points into the selected retained cue; no saved edit. */
    if (in) ((void (*)(void *, double, int))(p->adjust_in))(setter, seconds * 1000.0, 0);
    else ((void (*)(void *, double, int, int))(p->adjust_out))(setter, seconds * 1000.0, 1, 0);
}
static bool loop_context_safe(Slot *slot) {
    if (slot->operation != AZ_CMD_LOOP_ADJUST_IN && slot->operation != AZ_CMD_LOOP_MOVE) return true;
    uintptr_t context = word(slot->facade + 0x68);
    /* Native IN adjustment also copies working points into the selected cue.
     * Restrict that effect to the transient default cue in the ordinary mode. */
    return integer(context + 0x14) == 0 && !byte(context + 0x380);
}
static bool capture_loop_points(Slot *slot) {
    if (!set_bindings(slot) || !loop_context_safe(slot)) return false;
    slot->before = points(working(slot));
    uintptr_t context = word(slot->facade + 0x68);
    slot->retained = word(context + 0x18); /* native CueKind0, separate from working */
    if (!table(slot->retained, state().profile.cue_table)) return false;
    slot->retained_before = points(slot->retained);

    slot->expected_in = slot->before.in;
    slot->expected_out = slot->before.out;
    slot->move_stage = 0;
    return true;
}
static bool prepare_loop_out(Slot *slot) {
    if (slot->operation != AZ_CMD_LOOP_MOVE && slot->operation != AZ_CMD_LOOP_ADJUST_OUT) return true;
    double offset = slot->operation == AZ_CMD_LOOP_MOVE ? slot->arguments.position_s : slot->arguments.end_s;
    return shifted(slot->before.out, offset, slot->before.rate, &slot->expected_out);
}
static bool prepare_loop_targets(Slot *slot) {
    if (!slot->initially_engaged || !complete_points(slot->before)) return false;
    if (slot->operation == AZ_CMD_LOOP_MOVE || slot->operation == AZ_CMD_LOOP_ADJUST_IN)
        if (!shifted(slot->before.in, slot->arguments.position_s, slot->before.rate, &(slot->expected_in))) return false;
    if (!prepare_loop_out(slot)) return false;
    return compare_timestamps(slot->expected_out, slot->expected_in, slot->before.rate, slot->before.rate, 1) == AZ_TIME_AFTER;
}
static bool prepare_loop(Slot *slot) {
    if (!capture_loop_points(slot)) return false;
    switch (slot->operation) {
    case AZ_CMD_LOOP_OUT: return !slot->initially_engaged && slot->retained_before.in_valid;
    case AZ_CMD_LOOP_IN:
    case AZ_CMD_LOOP_CLEAR:
    case AZ_CMD_LOOP_ENTER: return true;
    default: break;
    }
    return prepare_loop_targets(slot);
}
static bool cue_player_binding(const Slot *slot) {
    uintptr_t pause = word(slot->facade + 0x58), bundle = word(slot->facade + 0x78);
    uintptr_t core = atomic_load_explicit(&(slot->core), memory_order_relaxed);
    if (!table(pause, state().profile.play_pause_table) || !pointer(bundle) ||
        word(pause + 0x58) != core || word(bundle) != core ||
        word(bundle + 8) != word(slot->facade + 0x68) || word(bundle + 0xa8) != pause) return false;
    return true;
}
static bool cue_playing(const Slot *slot, bool *playing, bool *auxiliary) {
    if (!cue_player_binding(slot)) return false;
    uintptr_t pause = word(slot->facade + 0x58);
    unsigned value = byte(pause + 8), other = byte(pause + 9);
    if (value > 1 || other > 1) return false;
    (*playing) = value != 0;
    if (auxiliary) *auxiliary = other != 0;
    return true;
}
static unsigned cue_kind(Slot *slot) {
    return slot->operation == AZ_CMD_HOT_TRIGGER || hot_edit(slot->operation) || hot_install(slot->operation) ? slot->cue_arguments.entry_id : 0;
}
static bool select_retained_cue(Slot *slot, uintptr_t context) {
    unsigned kind = cue_kind(slot);
    if (kind > 8) return false;
    slot->retained = word(context + 0x18 + kind * 8);
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
    if (recall_operation(slot->operation)) slot->retained = (uintptr_t) azsdk_cue_recall_property((unsigned) (slot - state().decks), slot->request.token);
#endif
    return table(slot->retained, state().profile.cue_table) || table(slot->retained, state().profile.cue_base_table);
}
static void capture_cue_state(Slot *slot, uintptr_t context) {
    slot->before = points(working(slot));
    slot->retained_before = points(slot->retained);
    if (hot_install(slot->operation)) slot->main_before = points(word(context + 0x18));
    memcpy(slot->cue_source_before, ((const void *) (atomic_load_explicit(&(slot->core), memory_order_relaxed) + 0x18)), 12);
    slot->cue_seek_seen = false;
    slot->staged_cue = (azexit_cue_stage_sample) {0};
    slot->cue_rgb_before = (byte(slot->retained + 0x10) << 16) | (byte(slot->retained + 0x11) << 8) | byte(slot->retained + 0x12);
}
static bool hot_install_compatible(const LoopPoints before, const LoopPoints wanted) {
    return !before.in_valid || (before.rate == wanted.rate && !memcmp(before.identity, wanted.identity, 12) &&
                                compare_timestamps(before.in, wanted.in, wanted.rate, wanted.rate, 1) == AZ_TIME_EQUAL && before.out_valid == wanted.out_valid &&
                                (!wanted.out_valid || compare_timestamps(before.out, wanted.out, wanted.rate, wanted.rate, 1) == AZ_TIME_EQUAL));
}
static bool prepare_hot_install(Slot *slot) {
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
    uintptr_t desired = (uintptr_t) azsdk_cue_recall_property((unsigned) (slot - state().decks), slot->request.token);
    if (!desired) return false;
    LoopPoints wanted = points(desired);
    return wanted.in_valid && !memcmp(wanted.identity, slot->cue_source_before, 12) && hot_install_compatible(slot->retained_before, wanted);
#else
    (void) slot;
    return false;
#endif
}
static bool cue_source_admitted(Slot *slot) {
    return slot->operation == AZ_CMD_MAIN_SET || slot->operation == AZ_CMD_HOT_SET ||
           (slot->operation == AZ_CMD_HOT_CLEAR && empty_position(slot->retained + 0x30) && empty_position(slot->retained + 0x88)) ||
           (slot->retained_before.in_valid && !memcmp(slot->retained_before.identity, slot->cue_source_before, 12));
}
static bool cue_state_admitted(Slot *slot) {
    if (!cue_playing(slot, &slot->cue_playing_before, &slot->cue_aux_before)) return false;
    if (slot->operation == AZ_CMD_HOT_TRIGGER && integer(word(slot->facade + 0x70) + 0x104)) return false;
    if (slot->operation == AZ_CMD_HOT_SET && slot->initially_engaged && !complete_points(slot->before)) return false;
    return hot_install(slot->operation) ? prepare_hot_install(slot) : cue_source_admitted(slot);
}
static bool prepare_cue(Slot *slot) {
    if (!cue_operation(slot->operation)) return false;
    if ((slot->operation == AZ_CMD_MAIN_SET || hot_edit(slot->operation)) && !set_bindings(slot)) return false;
    uintptr_t context = word(slot->facade + 0x68);
    if (!select_retained_cue(slot, context)) return false;
    capture_cue_state(slot, context);
    return cue_state_admitted(slot);
}
static bool dispatch_hot_set(Slot *slot) {
    const azexit_profile *p = &state().profile;
    azexit_native_color color = {((unsigned char) (slot->cue_arguments.rgb >> 16)), ((unsigned char) (slot->cue_arguments.rgb >> 8)), ((unsigned char) (slot->cue_arguments.rgb))};
    if (!p->set_hot || word(p->set_table + 4 * 8) != p->set_hot) return false;
    ((void (*)(void *, int, azexit_native_color, int))(p->set_hot))(((void *) (slot->facade + 0xc8)), (int) (slot->cue_arguments.entry_id), color, 1);
    return true;
}
static bool dispatch_cue(Slot *slot) {
    __auto_type p = &(state().profile);
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
    if (recall_operation(slot->operation) || hot_install(slot->operation)) {
        return azsdk_cue_recall_dispatch((unsigned) (slot - state().decks), slot->request.token,
                                         ((void *) (atomic_load_explicit(&(slot->core), memory_order_relaxed))), ((void *) (slot->facade))) == AZ_OK;
    }
#endif
    if (slot->operation == AZ_CMD_HOT_SET) {
        return dispatch_hot_set(slot);
    } else if (slot->operation == AZ_CMD_HOT_CLEAR)
        ((void (*)(void *, int))(p->clear_cue))(((void *) (slot->facade + 0xc8)), (int) (slot->cue_arguments.entry_id));
    else if (slot->operation == AZ_CMD_MAIN_SET)
        ((void (*)(void *, int, int))(p->set_point))(((void *) (slot->facade + 0xc8)), 0, 1);
    else if (slot->operation == AZ_CMD_MAIN_RECALL)
        ((void (*)(void *, int))(p->control_backcue))(((void *) (slot->facade)), 0);
    else if (slot->operation == AZ_CMD_HOT_TRIGGER)
        ((void (*)(void *, int, int, int))(p->control_jump))(((void *) (slot->facade)), (int) (slot->cue_arguments.entry_id), 0, 0);
    else return false;
    return true;
}
static bool equivalent(const LoopPoints a, const LoopPoints b) {
    if (a.in_valid != b.in_valid || a.out_valid != b.out_valid) return false;
    if (!a.in_valid && !a.out_valid) return true;
    return same_source(a, b) &&
           (!a.in_valid || compare_timestamps(a.in, b.in, a.rate, a.rate, 1) == AZ_TIME_EQUAL) &&
           (!a.out_valid || compare_timestamps(a.out, b.out, a.rate, a.rate, 1) == AZ_TIME_EQUAL);
}
static bool cue_transport_unchanged(Slot *slot, bool playing, bool auxiliary) {
    return playing == slot->cue_playing_before && auxiliary == slot->cue_aux_before;
}
static bool hot_install_transport(Slot *slot, bool playing, bool auxiliary) {
    bool active;
    return !slot->cue_seek_seen && cue_transport_unchanged(slot, playing, auxiliary) && engaged(slot, &active) && active == slot->initially_engaged;
}
static bool hot_install_postcondition(Slot *slot, bool playing, bool auxiliary) {
#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
    uintptr_t context = word(slot->facade + 0x68);
    uintptr_t desired = (uintptr_t) azsdk_cue_recall_property((unsigned) (slot - state().decks), slot->request.token);
    if (!desired) return false;
    LoopPoints wanted = points(desired), actual = points(slot->retained);
    unsigned rgb = (byte(slot->retained + 0x10) << 16) | (byte(slot->retained + 0x11) << 8) | byte(slot->retained + 0x12);
    return hot_install_transport(slot, playing, auxiliary) && word(context + 0x18 + slot->cue_arguments.entry_id * 8) == slot->retained &&
           equivalent(actual, wanted) && rgb == slot->cue_arguments.rgb && equivalent(points(word(context + 0x18)), slot->main_before) && equivalent(points(working(slot)), slot->before);
#else
    (void) slot;
    (void) playing;
    (void) auxiliary;
    return false;
#endif
}
static bool recall_loop_postcondition(Slot *slot) {
    bool active_now = false;
    bool matches = slot->operation != AZ_CMD_MEMORY_RECALL || (engaged(slot, &active_now) && !active_now);
    if (complete_points(slot->retained_before)) {
        LoopPoints active = points(working(slot));
        matches = engaged(slot, &active_now) && active_now && complete_points(active) && same_source(active, slot->retained_before) &&
                  compare_timestamps(active.in, slot->retained_before.in, active.rate, active.rate, 1) == AZ_TIME_EQUAL &&
                  compare_timestamps(active.out, slot->retained_before.out, active.rate, active.rate, 1) == AZ_TIME_EQUAL;
    }
    return matches;
}
static bool retained_start_matches(const LoopPoints current, const LoopPoints before) {
    return current.in_valid && compare_timestamps(current.in, before.in, current.rate, current.rate, 1) == AZ_TIME_EQUAL && same_source(current, before);
}
static bool recall_postcondition(Slot *slot, bool playing, bool auxiliary) {
    LoopPoints current = points(word(word(slot->facade + 0x68) + 0x18));
    bool loop_ok = recall_loop_postcondition(slot);
    return slot->cue_seek_seen && retained_start_matches(current, slot->retained_before) && !playing && !auxiliary && loop_ok &&
           current.out_valid == slot->retained_before.out_valid &&
           (!current.out_valid || compare_timestamps(current.out, slot->retained_before.out, current.rate, current.rate, 1) == AZ_TIME_EQUAL);
}
static bool hot_set_loop_matches(Slot *slot, const LoopPoints retained) {
    return slot->initially_engaged ? complete_points(retained) && same_source(retained, slot->before) &&
                                         compare_timestamps(retained.in, slot->before.in, retained.rate, retained.rate, 1) == AZ_TIME_EQUAL &&
                                         compare_timestamps(retained.out, slot->before.out, retained.rate, retained.rate, 1) == AZ_TIME_EQUAL
                                   : !retained.out_valid;
}
static bool hot_set_changed(Slot *slot, const LoopPoints retained, unsigned rgb) {
    return !slot->retained_before.in_valid || compare_timestamps(retained.in, slot->retained_before.in, retained.rate, retained.rate, 1) != AZ_TIME_EQUAL ||
           retained.out_valid != slot->retained_before.out_valid ||
           (retained.out_valid && compare_timestamps(retained.out, slot->retained_before.out, retained.rate, retained.rate, 1) != AZ_TIME_EQUAL) ||
           rgb != slot->cue_rgb_before || !same_source(retained, slot->retained_before);
}
static bool hot_set_postcondition(Slot *slot, const LoopPoints retained) {
    unsigned rgb = (byte(slot->retained + 0x10) << 16) | (byte(slot->retained + 0x11) << 8) | byte(slot->retained + 0x12);
    bool matches = hot_set_loop_matches(slot, retained) && retained.in_valid && !memcmp(retained.identity, slot->cue_source_before, 12) &&
                   rgb == slot->cue_arguments.rgb && hot_set_changed(slot, retained, rgb);
    if (matches) {
        slot->staged_cue.in_sample = retained.in;
        slot->staged_cue.out_sample = retained.out;
        slot->staged_cue.sample_rate = retained.rate;
        slot->staged_cue.out_valid = retained.out_valid;
        memcpy(slot->staged_cue.source_id, retained.identity, 12);
    }
    return matches;
}
static bool main_set_postcondition(Slot *slot, const LoopPoints retained) {
    return retained.in_valid && !memcmp(retained.identity, slot->cue_source_before, 12) &&
           (!slot->retained_before.in_valid || compare_timestamps(retained.in, slot->retained_before.in, retained.rate, retained.rate, 1) != AZ_TIME_EQUAL);
}
static bool cue_seek_postcondition(Slot *slot, const LoopPoints retained, bool playing, bool auxiliary) {
    if (!retained_start_matches(retained, slot->retained_before)) return false;
    if (slot->operation == AZ_CMD_MAIN_RECALL) return slot->cue_seek_seen && cue_position_matches(slot) && !playing && !auxiliary;
    return slot->cue_seek_seen && playing;
}
static bool edited_cue_transport(Slot *slot, bool playing, bool auxiliary) {
    return !(hot_edit(slot->operation) || slot->operation == AZ_CMD_MAIN_SET) || cue_transport_unchanged(slot, playing, auxiliary);
}
static bool selected_cue_postcondition(Slot *slot, bool playing, bool auxiliary) {
    unsigned kind = cue_kind(slot);
    if (word(word(slot->facade + 0x68) + 0x18 + kind * 8) != slot->retained) return false;
    LoopPoints retained = points(slot->retained);
    if (!edited_cue_transport(slot, playing, auxiliary)) return false;
    if (slot->operation == AZ_CMD_HOT_CLEAR) return !retained.in_valid && !retained.out_valid;
    if (slot->operation == AZ_CMD_HOT_SET) return hot_set_postcondition(slot, retained);
    if (slot->operation == AZ_CMD_MAIN_SET) return main_set_postcondition(slot, retained);
    return cue_seek_postcondition(slot, retained, playing, auxiliary);
}
static bool cue_postcondition(Slot *slot) {
    bool playing = false, auxiliary = false;
    if (!cue_playing(slot, &playing, &auxiliary)) return false;
    if (hot_install(slot->operation)) return hot_install_postcondition(slot, playing, auxiliary);
    if (recall_operation(slot->operation)) return recall_postcondition(slot, playing, auxiliary);
    return selected_cue_postcondition(slot, playing, auxiliary);
}
static bool dispatch_loop_change(Slot *slot) {
    const azexit_profile *p = &state().profile;
    void *setter = (void *) (slot->facade + 0xc8);
    switch (slot->operation) {
    case AZ_CMD_LOOP_DOUBLE:
    case AZ_CMD_LOOP_HALVE:
        ((void (*)(void *, int, int, int))(p->length_square))(setter, slot->operation == AZ_CMD_LOOP_DOUBLE ? 1 : -1, 1, 0);
        break;
    case AZ_CMD_LOOP_LENGTH: {
        uint32_t beat = 0;
        if (!azexit_pack_beat(slot->arguments.beats, &beat)) return false;
        ((void (*)(void *, uint32_t, int, int))(p->length_beat))(setter, beat, 1, 0);
        break;
    }
    case AZ_CMD_LOOP_ADJUST_IN: adjust(slot, true, slot->arguments.position_s); break;
    case AZ_CMD_LOOP_ADJUST_OUT: adjust(slot, false, slot->arguments.end_s); break;
    case AZ_CMD_LOOP_MOVE:
        /* Expand outward before moving the other edge. Each stage must settle
         * before the next starts. This is explicit sequencing, not rollback. */
        slot->move_stage = 1;
        adjust(slot, compare_timestamps(slot->arguments.position_s, 0., 1, slot->before.rate, 1) == AZ_TIME_BEFORE, slot->arguments.position_s);
        break;
    default: return false;
    }
    return true;
}
static bool dispatch_loop(Slot *slot) {
    __auto_type p = &(state().profile);
    void *setter = ((void *) (slot->facade + 0xc8));
    switch (slot->operation) {
    case AZ_CMD_LOOP_IN: ((void (*)(void *, int, int))(p->set_point))(setter, 0, 1); break;
    case AZ_CMD_LOOP_OUT: ((void (*)(void *, int, int))(p->loop_out))(setter, 0, 0); break;
    case AZ_CMD_LOOP_CLEAR: ((void (*)(void *, int))(p->clear_cue))(setter, 0); break;
    case AZ_CMD_LOOP_ENTER: ((void (*)(void *, int))(p->reloop))(((void *) (slot->facade)), 0); break;
    default: return dispatch_loop_change(slot);
    }
    return true;
}
static bool beat_equal(uint32_t actual, uint32_t expected, unsigned numerator_scale, unsigned denominator_scale) {
    uint64_t an = actual & 0xffffu, ad = actual >> 16;
    uint64_t en = expected & 0xffffu, ed = expected >> 16;
    return an && ad && en && ed && an * ed * denominator_scale == en * ad * numerator_scale;
}
static bool loop_in_postcondition(Slot *slot, const LoopPoints retained) {
    return retained.in_valid && (!slot->retained_before.in_valid || compare_timestamps(retained.in, slot->retained_before.in, retained.rate, retained.rate, 1) != AZ_TIME_EQUAL);
}
static bool loop_out_postcondition(Slot *slot, const LoopPoints after) {
    bool active = false;
    return engaged(slot, &active) && active && complete_points(after) &&
           compare_timestamps(after.in, slot->retained_before.in, after.rate, after.rate, 1) == AZ_TIME_EQUAL && same_source(after, slot->retained_before) &&
           (!slot->before.out_valid || compare_timestamps(after.out, slot->before.out, after.rate, after.rate, 1) != AZ_TIME_EQUAL);
}
static bool loop_length_changed(Slot *slot, const LoopPoints after) {
    return compare_timestamps(after.in, slot->before.in, after.rate, after.rate, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(after.out, slot->before.out, after.rate, after.rate, 1) != AZ_TIME_EQUAL;
}
static bool loop_scaled_postcondition(Slot *slot, const LoopPoints after, bool double_length) {
    if (!loop_length_changed(slot, after)) return false;
    if (slot->before.mode == 1) return after.mode == 1 && beat_equal(after.beat, slot->before.beat, double_length ? 2 : 1, double_length ? 1 : 2);
    uint64_t length = double_length ? 2 * (slot->before.out - slot->before.in) : (slot->before.out - slot->before.in) / 2;
    return compare_timestamps(after.out - after.in, length, after.rate, after.rate, 1) == AZ_TIME_EQUAL;
}
static bool loop_beat_postcondition(Slot *slot, const LoopPoints after) {
    uint32_t beat = 0;
    return azexit_pack_beat(slot->arguments.beats, &beat) && after.mode == 1 && beat_equal(after.beat, beat, 1, 1) && loop_length_changed(slot, after);
}
static bool loop_targets_reached(Slot *slot, const LoopPoints after) {
    return compare_timestamps(after.in, slot->expected_in, after.rate, after.rate, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(after.out, slot->expected_out, after.rate, after.rate, 1) == AZ_TIME_EQUAL;
}
static bool loop_change_postcondition(Slot *slot, const LoopPoints after) {
    if (!complete_points(after) || !same_source(slot->before, after)) return false;
    switch (slot->operation) {
    case AZ_CMD_LOOP_MOVE:
    case AZ_CMD_LOOP_ADJUST_IN:
    case AZ_CMD_LOOP_ADJUST_OUT:
        return loop_targets_reached(slot, after);
    case AZ_CMD_LOOP_DOUBLE: return loop_scaled_postcondition(slot, after, true);
    case AZ_CMD_LOOP_HALVE: return loop_scaled_postcondition(slot, after, false);
    case AZ_CMD_LOOP_LENGTH: return loop_beat_postcondition(slot, after);
    default: return false;
    }
}
static bool loop_postcondition(Slot *slot) {
    if (word(word(slot->facade + 0x68) + 0x18) != slot->retained || !table(slot->retained, state().profile.cue_table)) return false;
    LoopPoints after = points(working(slot)), retained = points(slot->retained);
    if (slot->operation == AZ_CMD_LOOP_IN) return loop_in_postcondition(slot, retained);
    if (slot->operation == AZ_CMD_LOOP_OUT) return loop_out_postcondition(slot, after);
    if (slot->operation == AZ_CMD_LOOP_CLEAR) return !retained.in_valid && !retained.out_valid;
    return loop_change_postcondition(slot, after);
}

static void finish(Slot *slot, unsigned outcome, int reason, uint64_t revision) {
    __auto_type request = slot->request;
    bool internal = atomic_load_explicit(&(slot->internal_recall), memory_order_acquire);
    __auto_type value = delivery_receipt(request, AZEXIT_RECEIPT_TERMINAL, 1, 0, now(), outcome, reason, revision);
    slot->result = (azexit_result) {request.token, request.load_generation, revision, outcome, reason};
    atomic_store_explicit(&(slot->phase), READY, memory_order_release);
    if (internal) azsdk_cue_commands_recall_finished(request.deck, request.token);
    else (void) delivery_publish(&value, false, 0);
}
static void settle_retired(Slot *slot, uint64_t token) {
    if (atomic_load_explicit(&(slot->phase), memory_order_acquire) != DRAINING ||
        atomic_load_explicit(&(slot->token), memory_order_acquire) != token ||
        atomic_load_explicit(&(slot->draining_unit), memory_order_acquire) || pending_nodes(token)) return;
    unsigned draining = DRAINING;
    if (atomic_compare_exchange_strong_explicit(&(slot->phase), &(draining), WRITING, memory_order_acq_rel, memory_order_acquire))
        finish(slot, AZ_COMMAND_UNCERTAIN, AZ_STALE, 0);
}
typedef struct execute_move_tail_call {
    unsigned deck;
    Slot *slot;
    uint64_t token;
} execute_move_tail_call;
static bool owner_thread(Slot *slot) {
    return !((bool (*)(void *)) state().profile.affinity)((void *) (atomic_load_explicit(&slot->core, memory_order_relaxed) + 0x110));
}
static bool move_owner_ready(Slot *slot) {
    return coherent(slot) && bindings(slot) && set_bindings(slot) && loop_context_safe(slot) && owner_thread(slot);
}
static bool move_intermediate_matches(Slot *slot, const LoopPoints current, bool positive) {
    return complete_points(current) && same_source(slot->before, current) &&
           compare_timestamps(current.in, positive ? slot->before.in : slot->expected_in, current.rate, current.rate, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(current.out, positive ? slot->expected_out : slot->before.out, current.rate, current.rate, 1) == AZ_TIME_EQUAL;
}
static bool execution_lost(Slot *slot, uint64_t token) {
    return atomic_load_explicit(&slot->failed, memory_order_acquire) == token || atomic_load_explicit(&slot->lost, memory_order_acquire) == token ||
           atomic_load_explicit(&slot->loss_overflow, memory_order_acquire);
}
static bool stopped_move_quiescent(Slot *slot, uint64_t token) {
    return !pending_nodes(token) && coherent(slot) && bindings(slot) && settled_work(slot, token);
}
static void execute_move_tail_body(void *opaque) {
    execute_move_tail_call *call = opaque;
    unsigned deck = call->deck;
    Slot *slot = &(*call->slot);
    uint64_t token = call->token;

    if (!move_owner_ready(slot)) {
        finish(slot, AZ_COMMAND_UNCERTAIN, AZ_UNSUPPORTED, 0);
        return;
    }
    if (!settled_work(slot, token)) return;
    LoopPoints current = points(working(slot));
    bool positive = compare_timestamps(slot->arguments.position_s, 0., 1, slot->before.rate, 1) == AZ_TIME_AFTER;
    if (!move_intermediate_matches(slot, current, positive)) {
        finish(slot, AZ_COMMAND_UNCERTAIN, AZ_FAILED, 0);
        return;
    }
    CAUSAL_SCOPE(scope, (Scope) {slot, deck, token, az_owner_generation(&slot->gate), false, false, 0});
    slot->move_stage = 3;
    request_review(slot, token);
    adjust(slot, positive, slot->arguments.position_s);
}
static void execute_move_tail(unsigned deck, Slot *slot) {
    uint64_t time = now(), token = slot->request.token;
    if (slot->untracked) return;
    if (execution_lost(slot, token)) {
        finish(slot, AZ_COMMAND_UNCERTAIN, AZ_FAILED, 0);
        return;
    }
    if (azsdk_exit_delivery_continue(&slot->request, time) != AZ_OK || !same_load(deck, &((*slot)))) {
        if (stopped_move_quiescent(slot, token))
            finish(slot, AZ_COMMAND_UNCERTAIN, time ? AZ_STALE : AZ_FAILED, 0);
        return;
    }
    FailureScope failure __attribute__((cleanup(failure_drop))) = {slot, token, false};
    execute_move_tail_call call = {deck, slot, token};
    execute_move_tail_body(&call);
    failure.completed = true;
}
typedef struct execute_call {
    unsigned deck;
    Slot *slot;
    uint64_t time;
} execute_call;
static bool execution_capacity(Slot *slot) {
    if (slot->revision != UINT64_MAX && free_nodes() >= 64) return true;
    finish(slot, AZ_COMMAND_REJECTED, slot->revision == UINT64_MAX ? AZ_LIMIT : AZ_BUSY, 0);
    return false;
}
static bool execution_preflight(Slot *slot, unsigned deck) {
    if (!coherent(slot) || !bindings(slot) || !owner_thread(slot)) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED, 0);
        return false;
    }
    if (!execution_capacity(slot)) return false;
    uint64_t time = now();
    if (!time || compare_timestamps(time, slot->request.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL || !same_load(deck, slot)) {
        finish(slot, AZ_COMMAND_REJECTED, time ? AZ_STALE : AZ_FAILED, 0);
        return false;
    }
    return true;
}
static bool execution_prepare(Slot *slot, bool *before) {
    slot->untracked = false;
    slot->maintenance_seen = false;
    slot->captured = 0;
    slot->caused = false;
    if (!engaged(slot, before)) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED, 0);
        return false;
    }
    slot->initially_engaged = *before;
    if ((extended(slot->operation) && !prepare_loop(slot)) || (cue_operation(slot->operation) && !prepare_cue(slot))) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED, 0);
        return false;
    }
    return true;
}
static bool execution_claim(Slot *slot) {
    uint64_t time = now();
    int claim = atomic_load_explicit(&(slot->internal_recall), memory_order_acquire)
                    ? azsdk_exit_delivery_continue(&slot->request, time)
                    : azsdk_exit_delivery_claim(&slot->request, time);
    if (claim != AZ_OK) {
        finish(slot, AZ_COMMAND_CANCELLED, claim == AZ_CANCELLED ? AZ_CANCELLED : AZ_STALE, 0);
        return false;
    }
    return true;
}
static bool execution_dispatch(Slot *slot) {
    if (cue_operation(slot->operation)) {
        if (!dispatch_cue(slot)) {
            atomic_store_explicit(&(slot->failed), slot->request.token, memory_order_release);
            return false;
        }
    } else if (extended(slot->operation)) {
        if (!dispatch_loop(slot)) {
            atomic_store_explicit(&(slot->failed), slot->request.token, memory_order_release);
            return false;
        }
    } else if (slot->operation == AZ_CMD_RELOOP)
        ((void (*)(void *, int))(state().profile.reloop))(((void *) (slot->facade)), 0);
    else ((void (*)(void *))(state().profile.exit))(((void *) (slot->facade)));
    return true;
}
static bool execution_caused(Slot *slot, bool before, bool after) {
    if (extended(slot->operation) || cue_operation(slot->operation)) return true;
    if (slot->operation == AZ_CMD_RELOOP) return (!before && after) || slot->captured != 0;
    return slot->caused || !before || !after || slot->captured != 0;
}
static void execute_body(void *opaque) {
    execute_call *call = opaque;
    unsigned deck = call->deck;
    Slot *slot = call->slot;
    if (!execution_preflight(slot, deck)) return;
    bool before;
    if (!execution_prepare(slot, &before) || !execution_claim(slot)) return;
    request_review(slot, slot->request.token);
    CAUSAL_SCOPE(scope, (Scope) {slot, deck, slot->request.token, az_owner_generation(&slot->gate), false, false, 0});
    if (!execution_dispatch(slot)) return;
    bool after = true;
    if (!engaged(slot, &after)) atomic_store_explicit(&slot->failed, slot->request.token, memory_order_release);
    slot->caused = execution_caused(slot, before, after);
    // Settlement follows the retained Quantize/Slip descendants, never this initial dispatch alone.
}
static bool move_tail_pending(Slot *slot) {
    return atomic_load_explicit(&slot->phase, memory_order_acquire) == RUNNING && slot->operation == AZ_CMD_LOOP_MOVE && slot->move_stage == 2;
}
static void execute(unsigned deck, Slot *slot) {
    if (move_tail_pending(slot)) {
        execute_move_tail(deck, &((*slot)));
        return;
    }
    unsigned expected = QUEUED;
    if (!atomic_compare_exchange_strong_explicit(&(slot->phase), &(expected), RUNNING, memory_order_acq_rel, memory_order_acquire)) return;
    atomic_store_explicit(&(slot->frame_token), slot->request.token, memory_order_release);
    if (!(atomic_load_explicit(&(state().mask), memory_order_acquire) & (1u << deck))) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE, 0);
        return;
    }
    uint64_t time = now();
    if (!time || compare_timestamps(time, slot->request.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL) {
        finish(slot, AZ_COMMAND_CANCELLED, time ? AZ_STALE : AZ_FAILED, 0);
        return;
    }
    if (!azsdk_load_read((int) (deck), &slot->load) || slot->load.load_generation != slot->request.load_generation) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE, 0);
        return;
    }
    FailureScope failure __attribute__((cleanup(failure_drop))) = {slot, slot->request.token, false};
    execute_call call = {deck, slot, time};
    execute_body(&call);
    failure.completed = true;
}
static int settlement_stop_reason(uint64_t token, uint64_t time, bool current) {
    delivery_Token *cell = delivery_find(token);
    unsigned stops = cell ? atomic_load_explicit(&cell->stops, memory_order_acquire) : 0;
    return !time ? AZ_FAILED : !current || !(stops & AZEXIT_STOP_CANCEL) || (stops & AZEXIT_STOP_DEADLINE) ? AZ_STALE
                                                                                                           : AZ_CANCELLED;
}
static bool stopped_hot_pending(Slot *slot) {
    return slot->operation == AZ_CMD_HOT_TRIGGER && (!coherent(slot) || integer(word(slot->facade + 0x70) + 0x104) != 0);
}
static void settle_stopped(Slot *slot, uint64_t token, uint64_t time, bool current) {
    int reason = settlement_stop_reason(token, time, current);
    bool hot_pending = stopped_hot_pending(slot);
    if (!pending_nodes(token) && !hot_pending) finish(slot, AZ_COMMAND_UNCERTAIN, reason, 0);
    else if (!atomic_load_explicit(&slot->internal_recall, memory_order_acquire)) {
        azexit_receipt value = delivery_receipt(slot->request, AZEXIT_RECEIPT_TERMINAL, 0, 0, time, AZ_COMMAND_UNCERTAIN, reason, 0);
        (void) delivery_publish(&value, false, 0);
    }
}
static bool settlement_owner_ready(Slot *slot, uint64_t token) {
    if (!coherent(slot) || !bindings(slot)) {
        atomic_store_explicit(&slot->failed, token, memory_order_release);
        return false; // unknown native ownership cannot prove settlement
    }
    if (slot->operation == AZ_CMD_HOT_TRIGGER) {
        int pending = integer(word(slot->facade + 0x70) + 0x104);
        if (pending < 0 || pending > 3) {
            slot->untracked = true;
            return false;
        }
        if (pending) return false;
    }
    return true;
}
static bool move_first_stage_ready(Slot *slot, uint64_t token) {
    return slot->operation == AZ_CMD_LOOP_MOVE && slot->move_stage == 1 &&
           atomic_load_explicit(&slot->failed, memory_order_acquire) != token && atomic_load_explicit(&slot->lost, memory_order_acquire) != token;
}
static bool settle_move(Slot *slot, uint64_t token) {
    if (move_first_stage_ready(slot, token)) {
        LoopPoints current = points(working(slot));
        bool positive = compare_timestamps(slot->arguments.position_s, 0., 1, slot->before.rate, 1) == AZ_TIME_AFTER;
        if (!set_bindings(slot) || !loop_context_safe(slot) || !move_intermediate_matches(slot, current, positive)) {
            finish(slot, AZ_COMMAND_UNCERTAIN, AZ_FAILED, 0);
            return true;
        }
        // Arm the second edit for the next successful preProcess's direct owner route.
        slot->move_stage = 2;
        return true;
    }
    return slot->operation == AZ_CMD_LOOP_MOVE && slot->move_stage == 2;
}
static bool settlement_verified(Slot *slot) {
    if (cue_operation(slot->operation)) return cue_postcondition(slot);
    if (extended(slot->operation) && !entering(slot->operation)) return set_bindings(slot) && loop_postcondition(slot);
    bool loop = true;
    return engaged(slot, &loop) && loop == entering(slot->operation) && !(entering(slot->operation) && slot->initially_engaged);
}
static void settle_verified(Slot *slot, uint64_t token) {
    bool verified = settlement_verified(slot);
    // Engagement alone cannot attest a seek when RELOOP began engaged.
    if (!verified || slot->untracked || !slot->caused || execution_lost(slot, token)) {
        finish(slot, AZ_COMMAND_UNCERTAIN, AZ_FAILED, 0);
        return;
    }
    finish(slot, AZ_COMMAND_APPLIED, AZ_OK, ++slot->revision);
}
static void settle(unsigned deck, Slot *slot) {
    if (atomic_load_explicit(&slot->phase, memory_order_acquire) != RUNNING) return;
    uint64_t token = slot->request.token;
    if (slot->untracked) return; // an unobserved callback retains the process-lifetime fence
    uint64_t time = now();
    bool current = same_load(deck, slot);
    bool allowed = azsdk_exit_delivery_continue(&slot->request, time) == AZ_OK;
    if (!current || !allowed) {
        settle_stopped(slot, token, time, current);
        return;
    }
    if (!settlement_owner_ready(slot, token) || !settled_work(slot, token)) return;
    if (settle_move(slot, token)) return;
    settle_verified(slot, token);
}

#ifdef AZ_STATUS_NATIVE_PRODUCER

#include "sdk_status_loop.inc"


#include "sdk_status_settings.inc"

#endif
static __attribute__((unused)) bool preprocess_hook(void *self, const void *frames) {
    typedef bool (*Preprocess)(void *, const void *);
    bool result = ((Preprocess) trampoline_address(&state().pre_original))(self, frames);
    if (result && processing && atomic_load_explicit(&(processing->core), memory_order_acquire) == ((uintptr_t) (self)) && (atomic_load_explicit(&(state().enabled), memory_order_acquire) & ~AZEXIT_PASSIVE_STATUS)) {
        unsigned deck = (unsigned) (processing - state().decks);
        execute(deck, &(*processing));
    }
    return result;
}
static __attribute__((unused)) void process_hook(void *self, const void *frames) {
    State *s = &(state());
    typedef void (*Process)(void *, const void *);
    __auto_type original = ((Process) trampoline_address(&s->original));
    for (unsigned d = 0; d < 4; ++d) {
        Slot *slot = &(s->decks[d]);
        if (atomic_load_explicit(&(slot->core), memory_order_acquire) != ((uintptr_t) (self))) continue;
        AZ_OWNER_LEASE(lease);
        if (az_owner_enter(&slot->gate, &lease) != AZ_OK) return;
        OwnerFrame owner __attribute__((cleanup(owner_drop))) = owner_begin(slot);
        (void) owner; /* cleanup attribute owns the scope exit */
        FrameToken frame_token __attribute__((cleanup(frame_token_drop))) = {slot, d};
        (void) frame_token; /* cleanup attribute owns the scope exit */
        if (atomic_load_explicit(&(slot->phase), memory_order_acquire) == RUNNING)
            atomic_store_explicit(&(slot->frame_token), atomic_load_explicit(&(slot->token), memory_order_acquire), memory_order_release);
        {
            FailureScope failure __attribute__((cleanup(failure_drop))) = {slot, 0, false};

            /* Original includes preProcess, rendering, all manager executions
             * and postprocessing. Teardown waits until the entire frame ends. */
#ifdef AZ_STATUS_NATIVE_PRODUCER
            if (atomic_load_explicit(&(s->enabled), memory_order_acquire) & AZEXIT_PASSIVE_STATUS) {
                status_settings_frame(d, &((*slot)));
                transport_observe(d, &((*slot)), TransportSeed, NULL);
                rate_capture(d, &((*slot)), 0, 0);
                status_stationary_seed(d, &((*slot)));
                status_loop_capture(d, &((*slot)), 0, NULL, NULL, 0);
                status_hot_capture(d, &((*slot)), 0, NULL, NULL);
            }
#endif
            original(self, frames);
            /* Check the postcondition on the owner after an action, descendant
             * execution/disposal, load change, or a durable stop request. */
            uint64_t review = atomic_exchange_explicit(&(slot->review), 0, memory_order_acq_rel);
            if (review && review == atomic_load_explicit(&(slot->token), memory_order_acquire)) settle(d, &((*slot)));

            failure.completed = true;
        }
        return;
    }
    original(self, frames);
}
/* The nominal BeatSync callback and its owning task both contain a copy
 * of the same countdown. Observe its final edge inside the verified frame. */
static void hot_countdown_hook_0(void *self) {
    Slot *slot = processing;
    uintptr_t object = ((uintptr_t) (self));
    uintptr_t beat = 0 == 0 ? object : word(object + 0x10);
    bool track = slot && atomic_load_explicit(&(slot->phase), memory_order_acquire) == RUNNING &&
                 slot->operation == AZ_CMD_HOT_TRIGGER && word(slot->facade + 0x70) == beat &&
                 table(beat, state().profile.beat);
    int before = track ? integer(beat + 0x104) : 0;
    ((void (*)(void *)) trampoline_address(&state().hot_countdown_original[0]))(self);
    if (before > 0 && integer(beat + 0x104) == 0)
        request_review(slot, slot->request.token);
}
static void hot_countdown_hook_1(void *self) {
    Slot *slot = processing;
    uintptr_t object = ((uintptr_t) (self));
    uintptr_t beat = 1 == 0 ? object : word(object + 0x10);
    bool track = slot && atomic_load_explicit(&(slot->phase), memory_order_acquire) == RUNNING &&
                 slot->operation == AZ_CMD_HOT_TRIGGER && word(slot->facade + 0x70) == beat &&
                 table(beat, state().profile.beat);
    int before = track ? integer(beat + 0x104) : 0;
    ((void (*)(void *)) trampoline_address(&state().hot_countdown_original[1]))(self);
    if (before > 0 && integer(beat + 0x104) == 0)
        request_review(slot, slot->request.token);
}
static bool install_hot_countdown(State *s, azexit_install_fn install) {
    void *hooks[] = {((void *) (hot_countdown_hook_0)),
                     ((void *) (hot_countdown_hook_1))};
    for (unsigned i = 0; i < 2; ++i) {
        __auto_type entry = &(s->profile.hot_countdown[i]);
        if (!entry->address || memcmp(((void *) (entry->address)), entry->prologue, 16))
            return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!install(s->profile.hot_countdown[i].address, hooks[i], &s->hot_countdown_original[i]) ||
            !s->hot_countdown_original[i]) return false;
    }
    return true;
}
static unsigned deck_mask() { return (atomic_load_explicit(&(state().enabled), memory_order_acquire) & ~AZEXIT_PASSIVE_STATUS) ? atomic_load_explicit(&(state().mask), memory_order_acquire) : 0; }
static bool delivery_bound() {
    return atomic_load_explicit(&(delivery_transport.bound), memory_order_acquire);
}
static int attach_delivery(const azexit_request r) {
#ifdef AZSDK_EXIT_TESTING
    if (!delivery_find(r.token)) return AZ_OK;
#endif
    azsdk_load_sample load = {0};
    if (!azsdk_load_read((int) (r.deck), &load) || r.load_generation != load.load_generation)
        return AZ_STALE;
    return delivery_attach(r, load.unit_generation) ? AZ_OK : AZ_UNSUPPORTED;
}
/* Preserve the original endpoint's EXIT-only capability meaning for old peers. */
static unsigned mask() {
    return delivery_bound() && (atomic_load_explicit(&(state().enabled), memory_order_acquire) & AZEXIT_ENABLE_EXIT) ? deck_mask() : 0;
}
static uint64_t commands() {
    if (!delivery_bound() || !deck_mask()) return 0;
    unsigned enabled = atomic_load_explicit(&(state().enabled), memory_order_acquire);
    return (enabled & AZEXIT_ENABLE_EXIT ? UINT64_C(1) << AZ_CMD_LOOP_EXIT : 0) |
           (enabled & AZEXIT_ENABLE_RELOOP ? UINT64_C(1) << AZ_CMD_RELOOP : 0) |
           (enabled & AZEXIT_ENABLE_LOOP ? (((UINT64_C(1) << (AZ_CMD_LOOP_ADJUST_OUT + 1)) - 2) &
                                            ~((UINT64_C(1) << AZ_CMD_LOOP_EXIT) | (UINT64_C(1) << AZ_CMD_RELOOP)))
                                         : 0) |
           (enabled & AZEXIT_ENABLE_CUE ? (UINT64_C(1) << AZ_CMD_MAIN_SET) |
                                              (UINT64_C(1) << AZ_CMD_MAIN_RECALL) |
                                              (state().hot_countdown_ready ? UINT64_C(1) << AZ_CMD_HOT_TRIGGER : 0)
                                        : 0)
#ifndef AZSDK_EXIT_TESTING
           | azsdk_cue_commands_commands()
#endif
        ;
}
static uint64_t delivery_ready(unsigned deck) {
    if (deck >= 4 || !(deck_mask() & (1u << deck))) return 0;
    uint64_t available = commands();
#ifndef AZSDK_EXIT_TESTING
    available &= ~(azsdk_cue_commands_commands() & ~azsdk_cue_commands_ready(deck));
#endif
    return available;
}
static int target(unsigned deck, uint64_t *out) {
    if (!out || deck >= 4) return AZ_INVALID;
    if (!(deck_mask() & (1u << deck))) return AZ_UNSUPPORTED;
    azsdk_load_sample load = {0};
    if (!azsdk_load_read((int) (deck), &load)) return AZ_STALE;
    *out = load.load_generation;
    return AZ_OK;
}
static bool request_identity_valid(const azexit_request *r) {
    return r && r->deck < 4 && r->token && r->load_generation && r->deadline_ns && !r->reserved;
}
static bool operation_available(unsigned deck, uint32_t operation) {
    return (commands() & (UINT64_C(1) << operation)) && (deck_mask() & (1u << deck));
}
static int reserve_request(Slot *slot, const azexit_request *identity) {
    unsigned expected = IDLE;
    if (!atomic_compare_exchange_strong_explicit(&slot->phase, &expected, WRITING, memory_order_acq_rel, memory_order_acquire)) return AZ_BUSY;
    int rc = attach_delivery(*identity);
    if (rc != AZ_OK) atomic_store_explicit(&slot->phase, IDLE, memory_order_release);
    return rc;
}
static void queue_request(Slot *slot, const azexit_request *identity, uint32_t operation) {
    slot->request = *identity;
    slot->operation = operation;
    atomic_store_explicit(&slot->token, identity->token, memory_order_release);
    atomic_store_explicit(&slot->phase, QUEUED, memory_order_release);
}
static int submit_command(const azexit_request *r, uint32_t operation) {
    if (!request_identity_valid(r)) return AZ_INVALID;
    if (operation != AZ_CMD_LOOP_EXIT && operation != AZ_CMD_RELOOP) return AZ_UNSUPPORTED;
    if (!operation_available(r->deck, operation)) return AZ_UNSUPPORTED;
    Slot *slot = &state().decks[r->deck];
    int rc = reserve_request(slot, r);
    if (rc != AZ_OK) return rc;
    queue_request(slot, r, operation);
    return AZ_PENDING;
}
/* These zero checks validate the request shape before source admission.
 * Unused fields must be zero; a relative-offset request must contain a
 * finite nonzero payload. shifted() later validates its actual integer
 * movement with the admitted source rate and one committed sample. */
static bool loop_offset_shape(double offset, double unused, double beats) {
    return offset != 0 && isfinite(offset * 1000) && unused == 0 && beats == 0;
}
static bool loop_request_shape(const azexit_loop_request *r) {
    uint32_t beat = 0;
    switch (r->operation) {
    case AZ_CMD_LOOP_LENGTH: return r->position_s == 0 && r->end_s == 0 && azexit_pack_beat(r->beats, &beat);
    case AZ_CMD_LOOP_MOVE:
    case AZ_CMD_LOOP_ADJUST_IN: return loop_offset_shape(r->position_s, r->end_s, r->beats);
    case AZ_CMD_LOOP_ADJUST_OUT: return loop_offset_shape(r->end_s, r->position_s, r->beats);
    default: return r->position_s == 0 && r->end_s == 0 && r->beats == 0;
    }
}
static bool loop_request_valid(const azexit_loop_request *r) {
    return request_identity_valid(&r->identity) && isfinite(r->position_s) && isfinite(r->end_s) && isfinite(r->beats) && loop_request_shape(r);
}
static int submit_loop(const azexit_loop_request *r) {
    if (!r || r->size < sizeof *r || !extended(r->operation)) return AZ_INVALID;
    if (!loop_request_valid(r)) return AZ_INVALID;
    const azexit_request *identity = &r->identity;
    if (!operation_available(identity->deck, r->operation)) return AZ_UNSUPPORTED;
    Slot *slot = &state().decks[identity->deck];
    int rc = reserve_request(slot, identity);
    if (rc != AZ_OK) return rc;
    slot->arguments = *r;
    queue_request(slot, identity, r->operation);
    return AZ_PENDING;
}
static int target_cue(unsigned deck, azexit_cue_target *out) {
    uint64_t generation = 0;
    if (!out || deck >= 4) return AZ_INVALID;
    int rc = target(deck, &generation);
    if (rc != AZ_OK) return rc;
    memset(out, 0, sizeof *out);
#ifndef AZSDK_EXIT_TESTING
    azexit_cue_target native = {0};
    rc = azsdk_cue_commands_target_cue(deck, &native);
    if (rc == AZ_BUSY || rc == AZ_PENDING) return rc;
    if (rc == AZ_OK) *out = native;
#endif
    return AZ_OK;
}
static bool cue_request_persistent(uint32_t operation) {
    return hot_edit(operation) || hot_install(operation) || recall_operation(operation) || !cue_operation(operation);
}
static int submit_persistent_cue(const azexit_cue_request *r) {
#ifndef AZSDK_EXIT_TESTING
    if (r->identity.deck >= 4) return AZ_INVALID;
    if (!delivery_bound() || r->operation >= 64 || !(commands() & (UINT64_C(1) << r->operation))) return AZ_UNSUPPORTED;
    int rc = attach_delivery(r->identity);
    return rc == AZ_OK ? azsdk_cue_commands_submit(r) : rc;
#else
    return azsdk_exit_test_persistent_cue_submit(r);
#endif
}
static bool cue_request_payload(const azexit_cue_request *r) {
    return !r->expected_revision && r->flags == AZ_COMMAND_NATIVE_LOAD && !r->rgb && !r->comment[0] &&
           memchr(r->comment, 0, sizeof r->comment) && r->position_s == 0 && r->end_s == 0 && r->beats == 0;
}
static bool cue_request_entry(const azexit_cue_request *r) {
    if (r->operation == AZ_CMD_HOT_TRIGGER) return r->classification == AZ_CUE_HOT && r->entry_id >= 1 && r->entry_id <= 8;
    return r->classification == AZ_CUE_MAIN && !r->entry_id;
}
static int submit_cue(const azexit_cue_request *r) {
    if (!r || r->size < sizeof *r) return AZ_INVALID;
    if (cue_request_persistent(r->operation)) return submit_persistent_cue(r);
    const azexit_request *identity = &r->identity;
    if (!request_identity_valid(identity) || !cue_request_payload(r) || !cue_request_entry(r)) return AZ_INVALID;
    if (!operation_available(identity->deck, r->operation)) return AZ_UNSUPPORTED;
    Slot *slot = &state().decks[identity->deck];
    int rc = reserve_request(slot, identity);
    if (rc != AZ_OK) return rc;
    slot->cue_arguments = *r;
    queue_request(slot, identity, r->operation);
    return AZ_PENDING;
}
static int submit(const azexit_request *r) { return submit_command(r, AZ_CMD_LOOP_EXIT); }
static int cancel(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return AZ_INVALID;
    Slot *slot = &(state().decks[deck]);
#ifndef AZSDK_EXIT_TESTING
    if (atomic_load_explicit(&(slot->phase), memory_order_acquire) == IDLE) return azsdk_cue_commands_cancel(deck, token);
#endif
    unsigned expected = QUEUED;
    if (!atomic_compare_exchange_strong_explicit(&(slot->phase), &(expected), WRITING, memory_order_acq_rel, memory_order_acquire))
        return expected == IDLE ? AZ_STALE : AZ_PENDING;
    if (slot->request.token != token) {
        atomic_store_explicit(&(slot->phase), QUEUED, memory_order_release);
        return AZ_STALE;
    }
    finish(slot, AZ_COMMAND_CANCELLED, AZ_CANCELLED, 0);
    return AZ_OK;
}
static int exit_poll(unsigned deck, azexit_result *out) {
    if (deck >= 4 || !out) return AZ_INVALID;
#ifndef AZSDK_EXIT_TESTING
    if (atomic_load_explicit(&(state().decks[deck].internal_recall), memory_order_acquire)) return azsdk_cue_commands_poll(deck, out);
#endif
    Slot *slot = &(state().decks[deck]);
    unsigned expected = READY;
    if (!atomic_compare_exchange_strong_explicit(&(slot->phase), &(expected), READING, memory_order_acq_rel, memory_order_acquire)) {
#ifndef AZSDK_EXIT_TESTING
        if (expected == IDLE) return azsdk_cue_commands_poll(deck, out);
#endif
        return AZ_PENDING;
    }
    *out = slot->result;
    atomic_store_explicit(&(slot->phase), READY, memory_order_release);
    return AZ_OK;
}
static int release(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return AZ_INVALID;
    if (atomic_load_explicit(&(state().decks[deck].frame_token), memory_order_acquire) == token) return AZ_BUSY;
#ifndef AZSDK_EXIT_TESTING
    if (atomic_load_explicit(&(state().decks[deck].internal_recall), memory_order_acquire)) return azsdk_cue_commands_release(deck, token);
#endif
    Slot *slot = &(state().decks[deck]);
    unsigned expected = READY;
    if (!atomic_compare_exchange_strong_explicit(&(slot->phase), &(expected), READING, memory_order_acq_rel, memory_order_acquire)) {
#ifndef AZSDK_EXIT_TESTING
        if (expected == IDLE) return azsdk_cue_commands_release(deck, token);
#endif
        return AZ_BUSY;
    }
    if (slot->result.token != token) {
        atomic_store_explicit(&(slot->phase), READY, memory_order_release);
        return AZ_STALE;
    }
    atomic_store_explicit(&(slot->phase), IDLE, memory_order_release);
    return AZ_OK;
}
#if defined(__linux__)
static int image(struct dl_phdr_info *info, size_t unused, void *context) {
    (void) unused;
    if (info->dlpi_name && *info->dlpi_name) return 0;
    __auto_type s = ((State *) (context));
    s->bias = info->dlpi_addr;
    return 1;
}
#endif
static __attribute__((unused)) bool patch(uintptr_t address, uintptr_t original, void *replacement) {
#ifdef AZSDK_EXIT_TESTING
    return __atomic_compare_exchange_n(((uintptr_t *) (address)), &original,
                                       ((uintptr_t) (replacement)), false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
#elif defined(__linux__) && defined(__aarch64__)
    void *entry = ((void *) (original));
    return sdk_patch_slot(address, replacement, &entry) == 1;
#else
    (void) address;
    (void) original;
    (void) replacement;
    return false;
#endif
}


int azsdk_exit_delivery_publish(const azexit_receipt *receipt) {
    return delivery_publish(receipt, false, 0);
}
int azsdk_exit_delivery_claim(const azexit_request *request, uint64_t time) {
    if (!request) return AZ_INVALID;
#ifdef AZSDK_EXIT_TESTING
    if (!delivery_find(request->token))
        return delivery_before(time, request->deadline_ns) ? AZ_OK : AZ_STALE;
#endif
    return delivery_claim(*request, time);
}
int azsdk_exit_delivery_continue(const azexit_request *request, uint64_t time) {
    if (!request) return AZ_INVALID;
#ifdef AZSDK_EXIT_TESTING
    if (!delivery_find(request->token))
        return delivery_before(time, request->deadline_ns) ? AZ_OK : AZ_STALE;
#endif
    return delivery_continuation(*request, time);
}
void azsdk_exit_delivery_wake(void) { delivery_wake(); }
void azsdk_exit_delivery_notify(unsigned deck) { delivery_notify(deck); }
void azsdk_exit_load_changed(unsigned deck) {
    if (deck >= 4) return;
    Slot *slot = &(state().decks[deck]);
    request_review(slot, atomic_load_explicit(&(slot->token), memory_order_acquire));
}

static void delivery_retire_native(delivery_Token *cell, unsigned deck, uint64_t token) {
    if (!atomic_exchange_explicit(&cell->native_work, 0, memory_order_acq_rel)) return;
    int rc = release(deck, token);
    if (rc == AZ_OK) delivery_evidence(cell, delivery_Retired, 0);
    else atomic_store_explicit(&cell->native_work, 1, memory_order_release);
}
static void delivery_drive_cell(delivery_Token *cell, unsigned decks) {
    if (!atomic_load_explicit(&cell->decision, memory_order_acquire) || !atomic_load_explicit(&cell->lifetime, memory_order_acquire)) return;
    unsigned deck = cell->request.deck;
    if (!(decks & (1u << deck))) return;
    unsigned facts = atomic_load_explicit(&cell->evidence, memory_order_acquire);
    if (facts & delivery_Retired) return;
    uint64_t token = cell->request.token;
    if (!(facts & delivery_Settled) && atomic_load_explicit(&cell->stops, memory_order_acquire)) {
        Slot *slot = &state().decks[deck];
        request_review(slot, token);
        (void) cancel(deck, token);
    }
    delivery_retire_native(cell, deck, token);
}
static void delivery_drive() {
#ifndef AZSDK_EXIT_TESTING
    azsdk_cue_commands_drive();
#endif
    unsigned decks = atomic_exchange_explicit(&delivery_transport.decks, 0, memory_order_acq_rel);
    for (unsigned i = 0; i < sizeof delivery_transport.tokens / sizeof delivery_transport.tokens[0]; ++i)
        delivery_drive_cell(&delivery_transport.tokens[i], decks);
}
static const azexit_delivery_v2 g_sdk_delivery = {
    sizeof g_sdk_delivery, AZEXIT_DELIVERY_VERSION, delivery_event_fd, delivery_consume,
    delivery_reclaim, delivery_control, delivery_bind, delivery_reserve,
    delivery_close_local, delivery_drive, delivery_ready};

const azexit_service *azstem_exit_service_v1(void) {
#ifndef AZSDK_EXIT_TESTING
    /* Hand the mutating private endpoint only to azmod, which authenticates
     * public mod ownership. Resolve once during community_init, never on audio. */
    Dl_info caller = {0}, core = {0};
    void *anchor = dlsym(RTLD_DEFAULT, "azmod_trackid_path");
    if (!anchor || !dladdr(anchor, &core) ||
        !dladdr(__builtin_extract_return_addr(__builtin_return_address(0)), &caller) ||
        !core.dli_fbase || caller.dli_fbase != core.dli_fbase) return NULL;
#endif
    static const azexit_service api = {sizeof api, 1, mask, target, submit, cancel, exit_poll, release, commands, submit_command, submit_loop, submit_cue, target_cue,
#ifndef AZSDK_EXIT_TESTING
                                       azsdk_cue_commands_tick, azsdk_cue_commands_native_cues
#else
                                       NULL, NULL
#endif
                                       ,
                                       &g_sdk_delivery};
    return &api;
}
#if (defined(__linux__) && defined(__aarch64__)) || defined(AZSDK_EXIT_TESTING)
typedef struct azsdk_exit_configure_call {
    int on, build;
    azexit_install_fn install;
    int result;
} azsdk_exit_configure_call;
static bool rebase_exit_fields(azexit_profile *p, uintptr_t bias) {
    uintptr_t *fields[] = {&p->unit, &p->core, &p->facade, &p->beat,
                           &p->quantize, &p->slip, &p->box, &p->process,
                           &p->preprocess, &p->exit, &p->affinity, &p->reloop, &p->control_jump,
                           &p->manager_submit.address, &p->quantize_submit.address,
                           &p->quantize_jump.address, &p->quantize_cancel.address, &p->chain_table,
                           &p->chain_destroy, &p->chain_deleting,
                           &p->jump_table, &p->jump_request,
                           &p->set_table, &p->set_point, &p->clear_cue, &p->loop_out,
                           &p->adjust_in, &p->adjust_out, &p->length_beat, &p->length_square, &p->cue_table,
                           &p->merger_table, &p->merger_request,
                           &p->jump_schedule, &p->interval_table, &p->interval_source_table, &p->jump_cancel, &p->control_backcue,
                           &p->cue_base_table, &p->core_jump.address, &p->set_hot, &p->play_pause_table,
                           &p->hot_countdown[0].address, &p->hot_countdown[1].address};
    {
        for (unsigned range_index_18 = 0; range_index_18 < sizeof(fields) / sizeof(fields)[0]; ++range_index_18) {
            __auto_type field = fields[range_index_18];

            if (*field > UINTPTR_MAX - bias) {
                return false;
            }
            *field += bias;
        }
    }
    return true;
}
static bool rebase_exit_commands(azexit_profile *p, uintptr_t bias) {
    {
        for (unsigned range_index_19 = 0; range_index_19 < sizeof(p->commands) / sizeof(p->commands)[0]; ++range_index_19) {
            __auto_type command = &(p->commands[range_index_19]);

            uintptr_t *values[] = {&command->table, &command->destroy, &command->deleting, &command->execute};
            {
                for (unsigned range_index_20 = 0; range_index_20 < sizeof(values) / sizeof(values)[0]; ++range_index_20) {
                    __auto_type value = values[range_index_20];

                    if (*value > UINTPTR_MAX - bias) {
                        return false;
                    }
                    *value += bias;
                }
            }
        }
    }
    return true;
}
static bool configure_exit_profile(State *s, int build) {
#ifndef AZSDK_EXIT_TESTING
    dl_iterate_phdr(image, s);
    s->profile = exit_profiles[build - 1];
    const uintptr_t beat_methods[2][2] = {{0x13de978, 0x13de948}, {0x1499a50, 0x1499a20}};
    s->beat_exit = s->bias + beat_methods[build - 1][0];
    s->beat_reset = s->bias + beat_methods[build - 1][1];
    return rebase_exit_fields(&s->profile, s->bias) && rebase_exit_commands(&s->profile, s->bias);
#else
    (void) s;
    (void) build;
    return true;
#endif
}
static bool exit_prologue(uintptr_t address, const void *bytes) {
    return address && !memcmp((void *) address, bytes, 16);
}
static bool exit_profile_prologues(const azexit_profile *p) {
    return exit_prologue(p->preprocess, p->prologue) && exit_prologue(p->process, p->process_prologue) &&
           exit_prologue(p->manager_submit.address, p->manager_submit.prologue) && exit_prologue(p->quantize_submit.address, p->quantize_submit.prologue) &&
           exit_prologue(p->quantize_cancel.address, p->quantize_cancel.prologue);
}
static bool exit_profile_core_tables(const azexit_profile *p) {
    return word(p->core + 24) == p->process && word(p->facade + 24) == p->exit && p->reloop &&
           word(p->facade + 32) == p->reloop && p->control_jump && word(p->facade + 48) == p->control_jump && word(p->box + 40) == p->affinity;
}
static bool exit_profile_jump_tables(const azexit_profile *p) {
    return p->jump_table && p->jump_cancel && word(p->jump_table + 40) == p->jump_cancel && word(p->jump_table + 16) == p->jump_request &&
           p->jump_schedule && word(p->jump_table + 32) == p->jump_schedule && p->interval_table && p->interval_source_table;
}
static bool exit_profile_cue(const azexit_profile *p, int on) {
    return !(on & AZEXIT_ENABLE_CUE) || (p->control_backcue && word(p->facade + 8 * 8) == p->control_backcue &&
                                         p->cue_base_table && exit_prologue(p->core_jump.address, p->core_jump.prologue) && p->play_pause_table);
}
static bool exit_profile_commands(State *s) {
    {
        for (unsigned range_index_21 = 0; range_index_21 < sizeof(s->profile.commands) / sizeof(s->profile.commands)[0]; ++range_index_21) {
            __auto_type command = &(s->profile.commands[range_index_21]);

            if (!command->table || !command->destroy || !command->deleting || !command->execute ||
                word(command->table) != command->destroy || word(command->table + 8) != command->deleting ||
                word(command->table + 16) != command->execute) {
                return false;
            }
        }
    }
    return true;
}
static bool install_exit_processing(State *s, azexit_install_fn install) {
    if (!install(s->profile.process, ((void *) (process_hook)), &s->original) || !s->original ||
        !install(s->profile.preprocess, ((void *) (preprocess_hook)), &s->pre_original) || !s->pre_original) {
        return false;
    }
    return true;
}
static int install_exit_status(State *s, int on, int build, azexit_install_fn install) {
    (void) s;
    (void) build;
    (void) install;

#ifdef AZ_STATUS_NATIVE_PRODUCER
    if ((on & AZEXIT_PASSIVE_STATUS) && !status_install_writers(s, build, install))
        on &= ~(int) AZEXIT_PASSIVE_STATUS;
    if (on & AZEXIT_PASSIVE_STATUS)
        s->motion_hooked = status_install_motion(s, build, install);
    if (s->motion_hooked) status_install_noqua(s, build, install);
#endif
    return on;
}
static int configure_passive_exit(State *s, int on) {
#ifdef AZ_STATUS_NATIVE_PRODUCER
    if (s->motion_hooked)
        s->motion_actions = patch(s->profile.commands[0].table + 16, s->profile.commands[0].execute, ((void *) (command_hook))) &&
                            patch(s->profile.jump_table + 16, s->profile.jump_request, ((void *) (jump_hook)));
#endif
    atomic_store_explicit(&(s->enabled), (unsigned) (on), memory_order_release);
    {
        return on ? 1 : 0;
    }
}
static bool install_exit_command_entries(State *s, azexit_install_fn install) {
    if (
        !install(s->profile.manager_submit.address, ((void *) (manager_hook)), &s->manager_original) || !s->manager_original ||
        !install(s->profile.quantize_submit.address, ((void *) (quantize_hook)), &s->quantize_original) || !s->quantize_original ||
        !install(s->profile.quantize_cancel.address, ((void *) (quantize_cancel_hook)), &s->cancel_original) || !s->cancel_original) {
        return false;
    }
    return true;
}
static bool patch_exit_commands(State *s) {
    {
        for (unsigned range_index_22 = 0; range_index_22 < sizeof(s->profile.commands) / sizeof(s->profile.commands)[0]; ++range_index_22) {
            __auto_type command = &(s->profile.commands[range_index_22]);

            if (!patch(command->table, command->destroy, ((void *) (destroy_hook))) ||
                !patch(command->table + 8, command->deleting, ((void *) (deleting_hook))) ||
                !patch(command->table + 16, command->execute, ((void *) (command_hook)))) {
                return false;
            }
        }
    }
    return true;
}
static bool patch_exit_jump(State *s) {
    if (!patch(s->profile.jump_table + 16, s->profile.jump_request, ((void *) (jump_hook)))) {
        return false;
    }
    if (!patch(s->profile.jump_table + 32, s->profile.jump_schedule, ((void *) (loop_schedule_hook)))) {
        return false;
    }
    if (!patch(s->profile.jump_table + 40, s->profile.jump_cancel, ((void *) (jump_cancel_hook)))) {
        return false;
    }
    return true;
}
static bool install_exit_cue_jump(State *s, int on, azexit_install_fn install) {
    if (((unsigned) (on) &AZEXIT_ENABLE_CUE) &&
#ifdef AZ_STATUS_NATIVE_PRODUCER
        !s->core_jump_original &&
#endif
        (!install(s->profile.core_jump.address, ((void *) (core_jump_hook)), &s->core_jump_original) || !s->core_jump_original)) {
        return false;
    }
    return true;
}
static void configure_exit_cues(State *s, int on, int build, azexit_install_fn install) {
    (void) build;
    if (on & AZEXIT_ENABLE_CUE) {
        s->hot_countdown_ready = install_hot_countdown(s, install);
        if (!s->hot_countdown_ready)
            fprintf(stderr, "azstem: Hot Cue Trigger disabled; countdown delivery hook unavailable\n");
    }
#ifndef AZSDK_EXIT_TESTING
    if (on & AZEXIT_ENABLE_CUE) azsdk_cue_commands_configure(1, build, install);
#endif
}
static bool exit_profile_valid(State *s, int on) {
    return exit_profile_prologues(&s->profile) && exit_profile_core_tables(&s->profile) && exit_profile_jump_tables(&s->profile) &&
           exit_profile_cue(&s->profile, on) && exit_profile_commands(s);
}
static bool install_active_exit(State *s, int on, azexit_install_fn install) {
    return install_exit_command_entries(s, install) && patch_exit_commands(s) && patch_exit_jump(s) && install_exit_cue_jump(s, on, install);
}
static void azsdk_exit_configure_body(void *opaque) {
    azsdk_exit_configure_call *call = opaque;
    int on = call->on, build = call->build;
    azexit_install_fn install = call->install;
    State *s = &state();
    AZ_MUTEX_LOCK(lock, s->management);
    call->result = 0;
    if (s->configured) return;
    s->build = build;
    if (!configure_exit_profile(s, build) || !exit_profile_valid(s, on)) return;
#ifndef AZSDK_EXIT_TESTING
    if (on & AZEXIT_ENABLE_CUE) {
        // Cache the immutable cue binding before writer hooks replace validated bytes.
        azcue_profile verified = {0};
        (void) azsdk_cue_main_profile(build, &verified);
    }
#endif
    s->configured = true; // Never retry a process after reaching the patching phase.
    if (!install_exit_processing(s, install)) return;
    on = install_exit_status(s, on, build, install);
    if (!(on & ~AZEXIT_PASSIVE_STATUS)) {
        call->result = configure_passive_exit(s, on);
        return;
    }
    if (!install_active_exit(s, on, install)) return;
    configure_exit_cues(s, on, build, install);
#ifdef AZ_STATUS_NATIVE_PRODUCER
    s->motion_actions = s->motion_hooked;
#endif
    atomic_store_explicit(&s->enabled, (unsigned) on, memory_order_release);
    call->result = 1;
}

#endif
int azsdk_exit_configure(int on, int build, azexit_install_fn install) {
    State *s = &(state());
    if (!on) {
        atomic_store_explicit(&(s->enabled), 0, memory_order_release);
#ifndef AZSDK_EXIT_TESTING
        azsdk_cue_commands_configure(0, build, install);
#endif
        return 0;
    }
#ifndef AZ_STATUS_NATIVE_PRODUCER
    if ((unsigned) (on) &AZEXIT_PASSIVE_STATUS) return 0;
#endif
    if (build < 1 || build > 2 || !install || ((unsigned) (on) & ~(AZEXIT_ENABLE_EXIT | AZEXIT_ENABLE_RELOOP | AZEXIT_ENABLE_LOOP | AZEXIT_ENABLE_CUE | AZEXIT_PASSIVE_STATUS))) return 0;
#if !(defined(__linux__) && defined(__aarch64__)) && !defined(AZSDK_EXIT_TESTING)
    return 0;
#else
    azsdk_exit_configure_call call = {on, build, install, 0};
    azsdk_exit_configure_body(&call);
    return call.result;
#endif
}
#ifdef AZ_STATUS_NATIVE_PRODUCER
uint64_t azn_map_begin_mask(unsigned mask) { return begin_map_mask(mask); }
void azn_map_end_mask(uint64_t ticket) { end_map_mask(ticket); }
int azn_offer_map_cut(azstatus_native_receipt *r) { return offer_map_cut(r); }
int azn_install_terminal(int build, azn_setting_patch_fn patch) {
    State *s = &(state());
    AZ_MUTEX_LOCK(lock, s->management);
    if (!patch || build < 1 || build > 2 || s->build != build ||
        !(atomic_load_explicit(&(s->enabled), memory_order_acquire) & AZEXIT_PASSIVE_STATUS)) return 0;
    end_install(s, build, patch);
    return atomic_load_explicit(&(end_enabled), memory_order_acquire);
}
int azn_install_settings(int build, azn_setting_patch_fn patch) {
    State *s = &(state());
    AZ_MUTEX_LOCK(lock, s->management);
    if (!patch || build < 1 || build > 2 || build != s->build ||
        !(atomic_load_explicit(&(s->enabled), memory_order_acquire) & AZEXIT_PASSIVE_STATUS)) return 0;
    reloop_install(s, build, patch);
    rate_install(s, build, patch);
    transport_install(s, build, patch);
    azn_maps_install(build, s->bias, patch);
    return status_settings_install(s, build, patch);
}
#endif
typedef struct azsdk_exit_register_call {
    int deck;
    void *unit;
    int result;
} azsdk_exit_register_call;
static bool unique_owner_core(State *s, uintptr_t core) {
    for (unsigned deck = 0; deck < 4; ++deck)
        if (atomic_load_explicit(&s->decks[deck].core, memory_order_acquire) == core) return false;
    return true;
}
static bool publish_owner(Slot *slot, uintptr_t unit, uintptr_t bundle, uintptr_t core) {
    slot->unit = unit;
    slot->facade = word(bundle + 0x40);
    atomic_store_explicit(&slot->core, core, memory_order_release);
    uint64_t generation;
    if (!coherent(slot) || az_owner_publish(&slot->gate, slot, admitted, &generation) != AZ_OK) {
        atomic_store_explicit(&slot->core, 0, memory_order_release);
        return false;
    }
    return true;
}
static void announce_owner_capabilities(State *s, unsigned deck) {
#ifdef AZ_STATUS_NATIVE_PRODUCER
    if (!(atomic_load_explicit(&s->enabled, memory_order_acquire) & AZEXIT_PASSIVE_STATUS)) return;
    unsigned capabilities = AZN_CAP_LOOP | AZN_CAP_LOOP_MODE | AZN_CAP_RELOOP;
    if (atomic_load_explicit(&rate_hooks, memory_order_seq_cst) == 15 && atomic_load_explicit(&rate_generator_hooked, memory_order_seq_cst)) capabilities |= AZN_CAP_RATE;
    if (atomic_load_explicit(&transport_installed, memory_order_seq_cst) == 15) capabilities |= AZN_CAP_TRANSPORT_STATE;
    if (s->motion_hooked) capabilities |= AZN_CAP_MOTION;
    azn_capability(deck, capabilities, 1);
#else
    (void) s;
    (void) deck;
#endif
}
static void azsdk_exit_register_body(void *opaque) {
    azsdk_exit_register_call *call = opaque;
    State *s = &state();
    Slot *slot = &s->decks[call->deck];
    uintptr_t unit = (uintptr_t) call->unit;
    AZ_MUTEX_LOCK(lock, s->management);
    call->result = 0;
    if (az_owner_generation(&slot->gate) || !table(unit, s->profile.unit)) return;
    uintptr_t core = word(unit + 0x128);
    if (!unique_owner_core(s, core)) return;
    uintptr_t bundle = word(unit + 0x190);
    if (!pointer(bundle) || !table(core, s->profile.core)) return;
    if (!publish_owner(slot, unit, bundle, core)) return;
    announce_owner_capabilities(s, (unsigned) call->deck);
    atomic_fetch_or_explicit(&s->mask, 1u << (unsigned) call->deck, memory_order_release);
    delivery_notify((unsigned) call->deck);
    call->result = 1;
}
int azsdk_exit_register(int deck, void *unit) {
    if (deck < 0 || deck >= 4 || !atomic_load_explicit(&(state().enabled), memory_order_acquire)) return 0;
    azsdk_exit_register_call call = {deck, unit, 0};
    azsdk_exit_register_body(&call);
    return call.result;
}
static void withdraw_owner_capabilities(State *s, unsigned deck) {
#ifdef AZ_STATUS_NATIVE_PRODUCER
    if (atomic_load_explicit(&s->enabled, memory_order_acquire) & AZEXIT_PASSIVE_STATUS)
        azn_capability(deck, AZN_CAP_LOOP | AZN_CAP_LOOP_MODE | AZN_CAP_RELOOP | AZN_CAP_MOTION | AZN_CAP_HOT | AZN_CAP_HOT_LINK | AZN_CAP_RATE | AZN_CAP_TRANSPORT_STATE | AZN_CAP_DURATION, 0);
#else
    (void) s;
    (void) deck;
#endif
}
static void retire_owner_status(Slot *slot, unsigned deck) {
#ifdef AZ_STATUS_NATIVE_PRODUCER
    status_settings_retire(deck, slot);
    duration_retire(deck);
    slot->hot.ready = false;
    slot->working_origin = (azn_hot_link) {0};
    status_working_file(slot, 0);
    slot->status_epoch = 0;
    atomic_store_explicit(&reloop_state[deck].retained, 0, memory_order_release);
    atomic_store_explicit(&rate_state[deck].bound_engine, 0, memory_order_release);
    atomic_store_explicit(&transport_state[deck].pause, 0, memory_order_release);
    atomic_store_explicit(&transport_state[deck].cue, 0, memory_order_release);
#else
    (void) slot;
    (void) deck;
#endif
}
static void drain_retired_hot(Slot *slot) {
    if (slot->operation != AZ_CMD_HOT_TRIGGER) return;
    uintptr_t beat = word(slot->facade + 0x70);
    if (!table(beat, state().profile.beat) || integer(beat + 0x104))
        atomic_store_explicit(&slot->draining_unit, slot->unit, memory_order_release);
}
static void retire_running_request(Slot *slot) {
    if (!atomic_load_explicit(&slot->internal_recall, memory_order_acquire)) {
        azexit_receipt value = delivery_receipt(slot->request, AZEXIT_RECEIPT_TERMINAL, 0, 0, now(), AZ_COMMAND_UNCERTAIN, AZ_STALE, 0);
        (void) delivery_publish(&value, false, 0);
    }
    if (slot->untracked) return;
    drain_retired_hot(slot);
    atomic_store_explicit(&slot->phase, DRAINING, memory_order_release);
    settle_retired(slot, atomic_load_explicit(&slot->token, memory_order_acquire));
}
static void retire_request(Slot *slot) {
    unsigned expected = QUEUED;
    if (atomic_compare_exchange_strong_explicit(&slot->phase, &expected, WRITING, memory_order_acq_rel, memory_order_acquire))
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE, 0);
    else if (expected == RUNNING) retire_running_request(slot);
}
void azsdk_exit_retire(int deck) {
    if (deck < 0 || deck >= 4) return;
    State *s = &state();
    Slot *slot = &s->decks[deck];
    if (az_owner_held(&slot->gate)) abort();
    AZ_MUTEX_LOCK(lock, s->management);
    withdraw_owner_capabilities(s, (unsigned) deck);
    atomic_fetch_and_explicit(&s->mask, ~(1u << (unsigned) deck), memory_order_acq_rel);
    uint64_t generation = az_owner_generation(&slot->gate);
    /* Native destruction must never free storage admitted by an in-flight
     * owner frame. Self-destruction violates the native lifecycle. */
    if (generation && az_owner_retire(&slot->gate, generation) != AZ_OK) abort();
    retire_owner_status(slot, (unsigned) deck);
    atomic_store_explicit(&slot->core, 0, memory_order_release);
    retire_request(slot);
    delivery_notify((unsigned) deck);
}
void azsdk_exit_retire_complete(unsigned deck, uintptr_t unit) {
    if (deck >= 4 || !unit) return;
    Slot *slot = &(state().decks[deck]);
    if (atomic_compare_exchange_strong_explicit(&(slot->draining_unit), &(unit), 0, memory_order_acq_rel, memory_order_acquire))
        settle_retired(slot, atomic_load_explicit(&(slot->token), memory_order_acquire));
}
#ifdef AZSDK_EXIT_TESTING
bool azsdk_exit_test_rebase(azexit_profile *profile, uintptr_t bias) {
    return rebase_exit_fields(profile, bias) && rebase_exit_commands(profile, bias);
}
void azsdk_exit_test_profile(const azexit_profile *profile) {
    if (profile && !state().configured) {
        state().profile = *profile;
        state().beat_exit = word(profile->beat + 16 * 8);
        state().beat_reset = word(profile->beat + 15 * 8);
    }
}
#endif

#if !defined(AZSDK_EXIT_TESTING) || defined(AZSDK_CUE_STAGE_TESTING)
static bool cue_stage_operation(const azexit_cue_request *r) {
    if (recall_operation(r->operation) || hot_edit(r->operation)) return true;
    return hot_install(r->operation) && r->classification == AZ_CUE_HOT && r->entry_id >= 1 && r->entry_id <= 8;
}
int azsdk_exit_recall_submit(const azexit_cue_request *r) {
    if (!r || r->identity.deck >= 4 || !cue_stage_operation(r)) return AZ_INVALID;
    Slot *slot = &(state().decks[r->identity.deck]);
    unsigned expected = IDLE;
    if (!atomic_compare_exchange_strong_explicit(&(slot->phase), &(expected), WRITING, memory_order_acq_rel, memory_order_acquire)) return AZ_BUSY;
    atomic_store_explicit(&(slot->internal_recall), true, memory_order_release);
    slot->request = r->identity;
    slot->cue_arguments = *r;
    slot->operation = r->operation;
    atomic_store_explicit(&(slot->token), r->identity.token, memory_order_release);
    atomic_store_explicit(&(slot->phase), QUEUED, memory_order_release);
    return AZ_PENDING;
}
int azsdk_exit_cue_stage_submit(const azexit_cue_request *r) {
    return azsdk_exit_recall_submit(r);
}
int azsdk_exit_cue_stage_capture(unsigned deck, uint64_t token, azexit_cue_stage_sample *out) {
    if (deck >= 4 || !token || !out) return AZ_INVALID;
    Slot *slot = &(state().decks[deck]);
    unsigned expected = READY;
    if (!atomic_load_explicit(&(slot->internal_recall), memory_order_acquire) ||
        !atomic_compare_exchange_strong_explicit(&(slot->phase), &(expected), READING, memory_order_acq_rel, memory_order_acquire)) return AZ_PENDING;
    int result = AZ_STALE;
    if (slot->result.token == token && slot->result.outcome == AZ_COMMAND_APPLIED && hot_edit(slot->operation)) {
        *out = slot->staged_cue;
        result = AZ_OK;
    }
    atomic_store_explicit(&(slot->phase), READY, memory_order_release);
    return result;
}
int azsdk_exit_recall_cancel(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return AZ_INVALID;
    Slot *slot = &(state().decks[deck]);
    unsigned expected = QUEUED;
    if (!atomic_load_explicit(&(slot->internal_recall), memory_order_acquire) || atomic_load_explicit(&(slot->token), memory_order_acquire) != token) return AZ_STALE;
    if (!atomic_compare_exchange_strong_explicit(&(slot->phase), &(expected), WRITING, memory_order_acq_rel, memory_order_acquire)) return AZ_PENDING;
    finish(slot, AZ_COMMAND_CANCELLED, AZ_CANCELLED, 0);
    return AZ_OK;
}
int azsdk_exit_recall_quiescent(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return 0;
    Slot *slot = &(state().decks[deck]);
    unsigned phase = atomic_load_explicit(&(slot->phase), memory_order_acquire);
    return phase == IDLE || !atomic_load_explicit(&(slot->internal_recall), memory_order_acquire) ||
           atomic_load_explicit(&(slot->token), memory_order_acquire) != token || phase == READY || phase == READING;
}
int azsdk_exit_recall_take(unsigned deck, azexit_result *out) {
    if (deck >= 4 || !out) return AZ_INVALID;
    Slot *slot = &(state().decks[deck]);
    unsigned expected = READY;
    if (atomic_load_explicit(&(slot->frame_token), memory_order_acquire) ||
        !atomic_load_explicit(&(slot->internal_recall), memory_order_acquire) ||
        !atomic_compare_exchange_strong_explicit(&(slot->phase), &(expected), READING, memory_order_acq_rel, memory_order_acquire)) return AZ_PENDING;
    *out = slot->result;
    atomic_store_explicit(&(slot->phase), READY, memory_order_release);
    return AZ_OK;
}
int azsdk_exit_recall_poll(unsigned deck, azexit_result *out) {
    return azsdk_exit_recall_take(deck, out);
}
int azsdk_exit_recall_release(unsigned deck, uint64_t token) {
    if (deck >= 4 || !token) return AZ_INVALID;
    if (atomic_load_explicit(&(state().decks[deck].frame_token), memory_order_acquire) == token) return AZ_BUSY;
    Slot *slot = &(state().decks[deck]);
    unsigned expected = READY;
    if (!atomic_load_explicit(&(slot->internal_recall), memory_order_acquire) ||
        !atomic_compare_exchange_strong_explicit(&(slot->phase), &(expected), READING, memory_order_acq_rel, memory_order_acquire)) return AZ_BUSY;
    if (slot->result.token != token) {
        atomic_store_explicit(&(slot->phase), READY, memory_order_release);
        return AZ_STALE;
    }
    atomic_store_explicit(&(slot->internal_recall), false, memory_order_release);
    atomic_store_explicit(&(slot->phase), IDLE, memory_order_release);
    return AZ_OK;
}
#endif
