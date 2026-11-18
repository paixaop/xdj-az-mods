/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aztimestamp.h"
#include "../common/azcueactor.h"
#include "../common/azcuecommands.h"
#include "../azmod/azmod-mod.h"
#include <stdatomic.h>
#include <string.h>
#include "../common/aznative_sync.h"
#include "../common/aznative_c.h"
#include <stdlib.h>
#include <time.h>
#if defined(AZSDK_CUE_DIAGNOSTIC)
#include <stdio.h>
#endif
#if defined(__linux__) && defined(__aarch64__) && !defined(AZSDK_CUE_ACTOR_TESTING)
#include "sdk_vtable_patch.h"
#endif

#ifdef AZSDK_CUE_ACTOR_TESTING
int azsdk_exit_delivery_publish(const struct azexit_receipt *receipt __attribute__((unused))) { return AZ_UNSUPPORTED; }
int azsdk_exit_delivery_claim(const struct azexit_request *request __attribute__((unused)), uint64_t clock __attribute__((unused))) { return AZ_OK; }
void azsdk_cue_commands_notify(unsigned deck __attribute__((unused))) {}
void azsdk_cue_commands_actor_finished(unsigned deck __attribute__((unused)), uint64_t token __attribute__((unused))) {}
#endif


#define TAGS 512
enum Phase {
    Idle,
    Queued,
    Posted,
    Executing,
    Waiting,
    Ready
};
struct Slot {
    void *manager;
    uint64_t generation, epoch;
    enum Phase phase;
    bool continuation, work_started;
    struct azexit_cue_request request;
    struct azexit_result result;
};
struct Tag {
    bool reserved, cancelled, executed;
    void *task, *manager;
    unsigned deck;
    uint64_t epoch;
    struct azexit_cue_request request;
};
struct State {
    pthread_mutex_t mutex;
    _Atomic(unsigned) waiters, registered;
    struct Slot slots[4];
    struct Tag tags[TAGS];
    struct azcue_actor_profile profile;
    struct azcue_actor_callbacks callbacks;
    void *post, *execute, *dtor;
    bool configured, installation_attempted;
};
static struct State storage;
static pthread_once_t initialized = PTHREAD_ONCE_INIT;
static void initialize(void) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr)) abort();
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) abort();
    if (pthread_mutex_init(&storage.mutex, &attr)) abort();
    if (pthread_mutexattr_destroy(&attr)) abort();
}
static struct State *state(void) {
    if (pthread_once(&initialized, initialize)) abort();
    return &storage;
}
/* Dispatch never waits for another deck's actor frame. Release signals the
 * affected decks after the outermost owner unlocks; retrying once after
 * registering interest closes the unlock-before-registration race. */
static _Thread_local unsigned depth;
struct ActorLock {
    struct State *state;
    struct az_mutex_guard mutex;
};
static struct ActorLock actor_lock(struct State *s, unsigned deck) {
    struct ActorLock lock = {s, az_mutex_acquire(&s->mutex, deck < 4)};
    if (deck < 4 && !lock.mutex.held) {
        atomic_fetch_or_explicit(&s->waiters, 1u << deck, memory_order_release);
        lock.mutex = az_mutex_acquire(&s->mutex, true);
    }
    if (lock.mutex.held) ++depth;
    else if (deck >= 4) abort();
    return lock;
}
static void actor_unlock(struct ActorLock *lock) {
    if (!lock->mutex.held) return;
    bool outermost = --depth == 0;
    az_mutex_release(&lock->mutex);
    if (!outermost) return;
    unsigned waiters = atomic_exchange_explicit(&lock->state->waiters, 0, memory_order_acq_rel);
    for (unsigned deck = 0; waiters && deck < 4; ++deck)
        if (waiters & (1u << deck)) azsdk_cue_commands_notify(deck);
}
#define ACTOR_LOCK(name, s, deck) \
    struct ActorLock name __attribute__((cleanup(actor_unlock))) = actor_lock(s, deck)
static _Thread_local struct Tag *capture;
static uintptr_t word(const void *object, unsigned offset) {
    uintptr_t value;
    memcpy(&value, ((const char *) (object)) + offset, sizeof value);
    return value;
}
static uint64_t now() {
    struct timespec t = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0) return 0;
    return (uint64_t) (t.tv_sec) * UINT64_C(1000000000) + (uint64_t) (t.tv_nsec);
}
#if defined(AZSDK_CUE_DIAGNOSTIC)
static void actor_diagnostic(unsigned deck, unsigned group, unsigned stage, const char *name, const struct azexit_cue_request *request, bool started) {
    static _Atomic(unsigned) last[2][4];
    static _Atomic(uint64_t) tokens[2][4];
    if (deck >= 4 || group >= 2 || !request->operation) return;
    bool changed = atomic_exchange_explicit(&(tokens[group][deck]), request->identity.token, memory_order_relaxed) != request->identity.token;
    unsigned marker = stage * 2 + (unsigned) (started) + 1;
    if (atomic_exchange_explicit(&(last[group][deck]), marker, memory_order_relaxed) == marker && !changed) return;
    fprintf(stderr, "AZCUE actor deck=%u stage=%s token=%llu load=%llu clock=%llu deadline=%llu started=%u\n",
            deck, name, (unsigned long long) request->identity.token, (unsigned long long) request->identity.load_generation,
            (unsigned long long) now(), (unsigned long long) request->identity.deadline_ns, (unsigned) (started));
}
#define ACTOR_DIAG(deck, group, stage, name, request, started) actor_diagnostic(deck, group, stage, name, &(request), started)
#else
#define ACTOR_DIAG(...) ((void) 0)
#endif
static struct Tag *find(void *task) {
    for (unsigned i = 0; i < TAGS; ++i) {
        struct Tag *tag = &state()->tags[i];
        if (tag->reserved && tag->task == task) return tag;
    }
    return NULL;
}
static void finish_result(struct Slot *slot, struct azexit_result result, unsigned settled) {
    const struct azexit_request *request = &slot->request.identity;
    if (result.outcome == AZ_COMMAND_REJECTED && result.reason == AZ_CANCELLED)
        result.outcome = AZ_COMMAND_CANCELLED;
    if (slot->work_started && (result.outcome == AZ_COMMAND_REJECTED || result.outcome == AZ_COMMAND_CANCELLED))
        result.outcome = AZ_COMMAND_UNCERTAIN;
    slot->result = result;
    slot->phase = Ready;
    if (!(request->token & (UINT64_C(1) << 63))) {
        struct azexit_receipt receipt = {
            sizeof receipt, AZEXIT_DELIVERY_VERSION, AZEXIT_RECEIPT_TERMINAL, settled, request->deck,
            request->token, request->load_generation, 0, result.revision, now(), result.outcome, result.reason};
        (void) azsdk_exit_delivery_publish(&receipt);
    }
    azsdk_cue_commands_actor_finished(request->deck, request->token);
}
static void finish(struct Slot *slot, unsigned outcome, int reason) {
    finish_result(slot, (struct azexit_result) {slot->request.identity.token, slot->request.identity.load_generation, 0, outcome, reason}, 1);
}
static bool matches(const struct Tag *tag, const struct Slot *slot) {
    return tag->epoch == slot->epoch && tag->manager == slot->manager &&
           tag->request.identity.token == slot->request.identity.token;
}
static uint64_t post_hook(void *box, void *task) {
    struct State *s = state();
    /* Shared stock notification posts never enter the SDK management lock. */
    if (!capture) return ((uint64_t (*)(void *, void *))(__atomic_load_n(&s->post, __ATOMIC_ACQUIRE)))(box, task);
    ACTOR_LOCK(lock, s, 4);
    if (capture) {
        struct Tag *tag = capture;
        /* Only the original guarded producer is called while capture is set.
         * An ABI/profile violation must never release an untagged memory save. */
        if (tag->task || box != ((char *) (tag->manager)) + 8 || !task ||
            word(task, 0) != s->profile.task_vptr || word(task, 0x18) != (uintptr_t) (tag->manager)) abort();
        tag->task = task;
        struct Slot *slot = &(s->slots[tag->deck]);
        if (matches(tag, slot)) slot->phase = Posted;
        capture = NULL; /* no inherited capture during native reentry */
    }
    return ((uint64_t (*)(void *, void *))(s->post))(box, task);
}
static void execute_hook(void *task) {
    struct State *s = state();
    ACTOR_LOCK(lock, s, 4);
    struct Tag *tag = find(task);
    if (!tag) {
        void *original = s->execute;
        actor_unlock(&lock);
        ((void (*)(void *))(original))(task);
        return;
    }
    /* A tombstone suppresses the original operation until native destruction,
     * including cancellation, owner retirement, and released SDK mailboxes. */
    if (tag->executed) return;
    tag->executed = true;
    struct Slot *slot = &(s->slots[tag->deck]);
    if (tag->cancelled || !matches(tag, slot) || slot->phase != Posted) return;
    if (!slot->work_started && !s->callbacks.current(tag->deck, tag->request.identity.load_generation, s->callbacks.context)) {
        ACTOR_DIAG(tag->deck, 1, 1, "execute-current", tag->request, slot->work_started);
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
        return;
    }
    uint64_t clock = now();
    if (!slot->work_started && (!clock || compare_timestamps(clock, tag->request.identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL)) {
        ACTOR_DIAG(tag->deck, 1, 2, "execute-deadline", tag->request, slot->work_started);
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
        return;
    }
    if (!slot->work_started && tag->request.operation &&
        azsdk_exit_delivery_claim(&tag->request.identity, clock) != AZ_OK) {
        finish(slot, AZ_COMMAND_CANCELLED, AZ_CANCELLED);
        return;
    }
    ACTOR_DIAG(tag->deck, 1, 0, "execute-admitted", tag->request, slot->work_started);
    slot->phase = Executing;
    struct azexit_result result = {0};
    int rc = s->callbacks.execute(&tag->request, tag->manager, &result, s->callbacks.context);
    if (slot->phase != Executing) return; /* callback may synchronously complete */
    if (rc == AZ_PENDING) {
        slot->work_started = true;
        slot->phase = slot->continuation ? Queued : Waiting;
        slot->continuation = false;
        if (slot->phase == Queued) azsdk_cue_commands_notify(tag->deck);
    } else if (rc == AZ_OK && result.token == tag->request.identity.token &&
               result.load_generation == tag->request.identity.load_generation) {
        finish_result(slot, result, 1);
    } else finish(slot, AZ_COMMAND_REJECTED, rc == AZ_OK ? AZ_INVALID : rc);
}
static void dtor_hook(void *task) {
    struct State *s = state();
    ACTOR_LOCK(lock, s, 4);
    struct Tag *tag = find(task);
    if (tag) {
        struct Slot *slot = &(s->slots[tag->deck]);
        if (matches(tag, slot) && !tag->executed && slot->phase == Posted)
            finish(slot, AZ_COMMAND_REJECTED, AZ_CANCELLED);
        /* Native dtor owns token/watchdog accounting; allocator runs after it. */
        unsigned deck = tag->deck;
        ((void (*)(void *))(s->dtor))(task);
        *tag = (struct Tag) {0};
        actor_unlock(&lock);
        azsdk_cue_commands_notify(deck);
        return;
    }
    void *original = s->dtor;
    actor_unlock(&lock);
    ((void (*)(void *))(original))(task);
}

static int configure(const struct azcue_actor_profile *p, azexit_install_fn install,
                     const struct azcue_actor_callbacks *callbacks) {
    struct State *s = state();
    ACTOR_LOCK(lock, s, 4);
    if (!p || !install || !callbacks || !callbacks->execute || !callbacks->current) return AZ_INVALID;
    if (s->configured || s->installation_attempted) return AZ_BUSY;
    if (!p->manager_vptr || !p->box_vptr || !p->owner_vptr || !p->task_vptr ||
        !p->memory_producer || !p->post || !p->execute || !p->complete_dtor) return AZ_UNSUPPORTED;
    s->profile = *p;
    s->callbacks = *callbacks;
    s->installation_attempted = true;
    /* Installer publishes each original before its patch becomes reachable. */
    if (!install(p->post, ((void *) (post_hook)), &s->post) || !s->post) return AZ_UNSUPPORTED;
#if defined(__linux__) && defined(__aarch64__) && !defined(AZSDK_CUE_ACTOR_TESTING)
    s->execute = ((void *) (p->execute));
    if (sdk_patch_slot(p->task_vptr + 16, ((void *) (execute_hook)), &s->execute) != 1) return AZ_UNSUPPORTED;
#else
    if (!install(p->execute, ((void *) (execute_hook)), &s->execute) || !s->execute) return AZ_UNSUPPORTED;
#endif
    if (!install(p->complete_dtor, ((void *) (dtor_hook)), &s->dtor) || !s->dtor) return AZ_UNSUPPORTED;
    s->configured = true;
    return AZ_OK;
}
int azsdk_cue_actor_register(unsigned deck, void *manager, uint64_t generation) {
    struct State *s = state();
    ACTOR_LOCK(lock, s, 4);
    if (deck >= 4 || !manager || !generation) return AZ_INVALID;
    if (!s->configured || word(manager, 0) != s->profile.manager_vptr ||
        word(manager, 8) != s->profile.box_vptr) return AZ_UNSUPPORTED;
    struct Slot *slot = &(s->slots[deck]);
    if (slot->manager) return AZ_BUSY;
    if (++slot->epoch == 0) abort();
    slot->manager = manager;
    slot->generation = generation;
    atomic_fetch_or_explicit(&(s->registered), 1u << deck, memory_order_release);
    azsdk_cue_commands_notify(deck);
    return AZ_OK;
}
unsigned azsdk_cue_actor_mask(void) {
    return atomic_load_explicit(&(state()->registered), memory_order_acquire);
}
void azsdk_cue_actor_retire(unsigned deck, void *manager) {
    struct State *s = state();
    ACTOR_LOCK(lock, s, 4);
    if (deck >= 4) return;
    struct Slot *slot = &(s->slots[deck]);
    if (slot->manager != manager) return;
    atomic_fetch_and_explicit(&(s->registered), ~(1u << deck), memory_order_release);
    for (unsigned i = 0; i < TAGS; ++i) {
        struct Tag *tag = &s->tags[i];
        if (tag->reserved && tag->deck == deck && tag->epoch == slot->epoch) tag->cancelled = true;
    }
    if (slot->phase != Idle && slot->phase != Ready) {
#ifndef AZSDK_CUE_ACTOR_TESTING
        const struct azexit_request *request = &slot->request.identity;
        if (slot->work_started && slot->request.operation) {
            finish_result(slot, (struct azexit_result) {request->token, request->load_generation, 0, AZ_COMMAND_UNCERTAIN, AZ_STALE}, 0);
            azsdk_cue_commands_actor_retired(deck, request->token, request->load_generation);
        } else
#endif
            finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
    }
    slot->manager = NULL;
    slot->generation = 0;
}
int azsdk_cue_actor_enqueue(const struct azexit_cue_request *request) {
    if (!request || request->size < sizeof *request || request->identity.deck >= 4 ||
        !request->identity.token || !request->identity.load_generation || !request->identity.deadline_ns ||
        request->identity.reserved || !memchr(request->comment, 0, sizeof request->comment)) return AZ_INVALID;
    struct State *s = state();
    ACTOR_LOCK(lock, s, request->identity.deck);
    if (!lock.mutex.held) return AZ_BUSY;
    struct Slot *slot = &(s->slots[request->identity.deck]);
    if (!s->configured || !slot->manager) return AZ_UNSUPPORTED;
    if (slot->phase != Idle) return AZ_BUSY;
    for (unsigned i = 0; i < TAGS; ++i) {
        struct Tag *tag = &s->tags[i];
        if (tag->reserved && tag->deck == request->identity.deck &&
            tag->request.identity.token == request->identity.token) return AZ_STALE;
    }
    slot->request = *request;
    slot->work_started = false;
    slot->continuation = false;
    slot->phase = Queued;
    return AZ_PENDING;
}
static void drive(unsigned deck) {
    struct State *s = state();
    if (deck >= 4) return;
    ACTOR_LOCK(lock, s, deck);
    if (!lock.mutex.held) return;
    struct Slot *slot = &(s->slots[deck]);
    if (slot->phase != Queued) return;
    if (!slot->manager || (!slot->work_started && !s->callbacks.current(deck, slot->request.identity.load_generation, s->callbacks.context))) {
        ACTOR_DIAG(deck, 0, 1, "drive-current-or-manager", slot->request, slot->work_started);
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
        return;
    }
    uint64_t clock = now();
    if (!slot->work_started && (!clock || compare_timestamps(clock, slot->request.identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL)) {
        ACTOR_DIAG(deck, 0, 2, "drive-deadline", slot->request, slot->work_started);
        finish(slot, AZ_COMMAND_REJECTED, AZ_STALE);
        return;
    }
    ACTOR_DIAG(deck, 0, 0, "drive-admitted", slot->request, slot->work_started);
    void *box = ((char *) (slot->manager)) + 8;
    uintptr_t owner = word(box, 0x28);
    if (word(slot->manager, 0) != s->profile.manager_vptr || word(box, 0) != s->profile.box_vptr ||
        !owner || word(((void *) (owner)), 0) != s->profile.owner_vptr ||
        !((bool (*)(void *))(word(((void *) (s->profile.box_vptr)), 0x28)))(box)) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        return;
    }
    struct Tag *tag = NULL;
    for (unsigned i = 0; i < TAGS; ++i) {
        struct Tag *candidate = &s->tags[i];
        if (!candidate->reserved) {
            tag = candidate;
            break;
        }
    }
    if (!tag) {
        finish(slot, AZ_COMMAND_REJECTED, AZ_BUSY);
        return;
    }
    tag->reserved = true;
    tag->deck = deck;
    tag->manager = slot->manager;
    tag->epoch = slot->epoch;
    tag->request = slot->request;
    if (capture) abort();
    capture = tag;
    ((void (*)(void *))(s->profile.memory_producer))(slot->manager);
    capture = NULL;
    if (!tag->task && tag->reserved) {
        *tag = (struct Tag) {0};
        finish(slot, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
    }
}
int azsdk_cue_actor_cancel(unsigned deck, uint64_t token) {
    struct State *s = state();
    if (deck >= 4 || !token) return AZ_INVALID;
    ACTOR_LOCK(lock, s, deck);
    if (!lock.mutex.held) return AZ_PENDING;
    struct Slot *slot = &(s->slots[deck]);
    if (slot->phase == Idle || slot->request.identity.token != token) return AZ_STALE;
    if (slot->phase == Ready) return AZ_OK;
    for (unsigned i = 0; i < TAGS; ++i) {
        struct Tag *tag = &s->tags[i];
        if (tag->reserved && tag->deck == deck && matches(tag, slot)) tag->cancelled = true;
    }
    finish(slot, AZ_COMMAND_CANCELLED, AZ_CANCELLED);
    return AZ_OK;
}
int azsdk_cue_actor_peek(unsigned deck, struct azexit_result *result) {
    struct State *s = state();
    if (deck >= 4 || !result) return AZ_INVALID;
    ACTOR_LOCK(lock, s, deck);
    if (!lock.mutex.held) return AZ_PENDING;
    if (s->slots[deck].phase != Ready) return AZ_PENDING;
    *result = s->slots[deck].result;
    return AZ_OK;
}
int azsdk_cue_actor_take(unsigned deck, struct azexit_result *result) {
    return azsdk_cue_actor_peek(deck, result);
}
int azsdk_cue_actor_poll(unsigned deck, struct azexit_result *result) {
    return azsdk_cue_actor_peek(deck, result);
}
int azsdk_cue_actor_release(unsigned deck, uint64_t token) {
    struct State *s = state();
    if (deck >= 4 || !token) return AZ_INVALID;
    ACTOR_LOCK(lock, s, deck);
    if (!lock.mutex.held) return AZ_BUSY;
    struct Slot *slot = &(s->slots[deck]);
    if (slot->phase != Ready) return AZ_BUSY;
    if (slot->result.token != token) return AZ_STALE;
#ifndef AZSDK_CUE_ACTOR_TESTING
    if (!(token & (UINT64_C(1) << 63))) {
        for (unsigned i = 0; i < TAGS; ++i) {
            struct Tag *tag = &s->tags[i];
            if (tag->reserved && tag->deck == deck && tag->request.identity.token == token) return AZ_BUSY;
        }
    }
#endif
    slot->phase = Idle;
    return AZ_OK;
}
int azsdk_cue_actor_complete(unsigned deck, uint64_t token, const struct azexit_result *result) {
    struct State *s = state();
    if (deck >= 4 || !token || !result) return AZ_INVALID;
    ACTOR_LOCK(lock, s, deck);
    if (!lock.mutex.held) return AZ_BUSY;
    struct Slot *slot = &(s->slots[deck]);
    if ((slot->phase != Waiting && slot->phase != Executing) || slot->request.identity.token != token ||
        result->token != token || result->load_generation != slot->request.identity.load_generation) return AZ_STALE;
    finish_result(slot, *result, 1);
    return AZ_OK;
}

int azsdk_cue_actor_resume(unsigned deck, uint64_t token) {
    struct State *s = state();
    if (deck >= 4 || !token) return AZ_INVALID;
    ACTOR_LOCK(lock, s, deck);
    if (!lock.mutex.held) return AZ_BUSY;
    struct Slot *slot = &(s->slots[deck]);
    if (slot->request.identity.token != token) return AZ_STALE;
    if (slot->phase == Executing) {
        slot->continuation = true;
        return AZ_OK;
    }
    if (slot->phase != Waiting) return AZ_STALE;
    slot->phase = Queued;
    return AZ_OK;
}

int azsdk_cue_actor_configure(const struct azcue_actor_profile *profile, azexit_install_fn install,
                              const struct azcue_actor_callbacks *callbacks) {
    return configure(profile, install, callbacks);
}
void azsdk_cue_actor_drive(unsigned deck) {
    drive(deck);
}
