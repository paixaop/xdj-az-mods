/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by sdk_exit.c. Each reservation owns its result and evidence until
 * the core consumes them and returns the token. The native writer never waits. */
#ifndef AZSDK_EXIT_DELIVERY_H
#define AZSDK_EXIT_DELIVERY_H


enum delivery_Decision {
    delivery_Queued = 1,
    delivery_Executing,
    delivery_Committed,
    delivery_ExpiredQueued,
    delivery_ExpiredExecuting,
    delivery_CancelledQueued
};
enum delivery_Evidence {
    delivery_Settled = 1,
    delivery_Retired = 2,
    delivery_Terminal = 4
};
static const uint64_t delivery_decision_bits = 7;
static const uint64_t delivery_max_token = UINT64_C(0x1fffffffffffffff);

typedef struct delivery_Token delivery_Token;
typedef struct delivery_Transport delivery_Transport;
struct delivery_Token {
    // The identity is part of the compare/exchange, including after slot reuse.
    _Atomic(uint64_t) decision, evidence;
    _Atomic(unsigned) native_work;
    _Atomic(uint32_t) stops;
    _Atomic(bool) result_writer;
    _Atomic(uint64_t) lifetime, settled_ns, retired_ns;
    azexit_request request;
    azexit_receipt result, stopped;
    unsigned reported; // core dispatcher only
};
struct delivery_Transport {
    _Atomic(bool) bound;
    _Atomic(unsigned) decks;
    int fd; // set before bound; process lifetime thereafter
    delivery_Token tokens[AZEXIT_DELIVERY_CAPACITY];
};
static delivery_Transport delivery_transport = {.fd = -1};

static void delivery_wake(void) {
#if defined(__linux__)
    if (!atomic_load_explicit(&(delivery_transport.bound), memory_order_acquire)) return;
    uint64_t one = 1;
    int saved_errno = errno;
    while (write(delivery_transport.fd, &one, sizeof one) < 0 && errno == EINTR) {}
    errno = saved_errno;
#endif
}
static void delivery_notify(unsigned deck) {
    if (deck < 4) atomic_fetch_or_explicit(&(delivery_transport.decks), 1u << deck, memory_order_release);
    delivery_wake();
}
static bool delivery_binding_valid(const azexit_binding *core) {
    return core && core->size >= sizeof *core && core->version == AZEXIT_DELIVERY_VERSION &&
           core->capacity == AZEXIT_DELIVERY_CAPACITY && core->per_owner == AZEXIT_DELIVERY_PER_OWNER;
}
static int delivery_bind(const azexit_binding *core) {
    if (!delivery_binding_valid(core)) return AZ_UNSUPPORTED;
    if (atomic_load_explicit(&(delivery_transport.bound), memory_order_acquire)) return AZ_OK;
#if defined(__linux__)
    delivery_transport.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (delivery_transport.fd < 0) return AZ_FAILED;
    atomic_store_explicit(&(delivery_transport.bound), true, memory_order_release);
    return AZ_OK;
#elif defined(AZSDK_EXIT_TESTING)
    atomic_store_explicit(&(delivery_transport.bound), true, memory_order_release);
    return AZ_OK;
#else
    return AZ_UNSUPPORTED;
#endif
}
static int delivery_event_fd(void) {
    return atomic_load_explicit(&(delivery_transport.bound), memory_order_acquire) ? delivery_transport.fd : -1;
}
static delivery_Token *delivery_find(uint64_t token) {
    if (!token || token > delivery_max_token) return NULL;
    {
        for (unsigned range_index_1 = 0; range_index_1 < sizeof(delivery_transport.tokens) / sizeof(delivery_transport.tokens)[0]; ++range_index_1) {
            __auto_type cell = &(delivery_transport.tokens[range_index_1]);
            if (atomic_load_explicit(&(cell->decision), memory_order_acquire) >> 3 == token) return cell;
        }
    }
    return NULL;
}
static bool delivery_request_valid(const azexit_request *request) {
    return request && request->token && request->token <= delivery_max_token && request->deck < 4 &&
           request->load_generation && request->deadline_ns && !request->reserved && !delivery_find(request->token);
}
static int delivery_reserve(const azexit_request *request) {
    if (!atomic_load_explicit(&delivery_transport.bound, memory_order_acquire)) return AZ_UNSUPPORTED;
    if (!delivery_request_valid(request)) return AZ_INVALID;
    // Admission and reservation return are serialized by the core store lock.
    {
        for (unsigned range_index_2 = 0; range_index_2 < sizeof(delivery_transport.tokens) / sizeof(delivery_transport.tokens)[0]; ++range_index_2) {
            __auto_type cell = &(delivery_transport.tokens[range_index_2]);

            if (atomic_load_explicit(&(cell->decision), memory_order_acquire)) continue;
            cell->request = *request;
            cell->reported = 0;
            atomic_store_explicit(&(cell->lifetime), 0, memory_order_relaxed);
            atomic_store_explicit(&(cell->settled_ns), 0, memory_order_relaxed);
            atomic_store_explicit(&(cell->retired_ns), 0, memory_order_relaxed);
            atomic_store_explicit(&(cell->stops), 0, memory_order_relaxed);
            atomic_store_explicit(&(cell->result_writer), false, memory_order_relaxed);
            atomic_store_explicit(&(cell->native_work), 0, memory_order_relaxed);
            atomic_store_explicit(&(cell->evidence), request->token << 3, memory_order_relaxed);
            atomic_store_explicit(&(cell->decision), request->token << 3 | delivery_Queued, memory_order_release);
            return AZ_OK;
        }
    }
    return AZ_LIMIT;
}
static bool delivery_matches(delivery_Token *cell, unsigned deck, uint64_t load) {
    return cell->request.deck == deck && cell->request.load_generation == load;
}
static bool delivery_attach(const azexit_request request, uint64_t lifetime) {
    __auto_type cell = delivery_find(request.token);
    if (!cell || !delivery_matches(cell, request.deck, request.load_generation) || !lifetime) return false;
    atomic_store_explicit(&(cell->lifetime), lifetime, memory_order_release);
    return true;
}
static bool delivery_before(uint64_t now, uint64_t deadline) {
    return now && compare_timestamps(now, deadline, 1000000000, 1000000000, 1) == AZ_TIME_BEFORE;
}
static int delivery_claim(const azexit_request request, uint64_t now) {
    __auto_type cell = delivery_find(request.token);
    if (!cell || !delivery_matches(cell, request.deck, request.load_generation)) return AZ_STALE;
    if (!delivery_before(now, cell->request.deadline_ns)) return AZ_STALE;
    if (atomic_load_explicit(&(cell->stops), memory_order_acquire)) return AZ_CANCELLED;
    uint64_t expected = request.token << 3 | delivery_Queued;
    return atomic_compare_exchange_strong_explicit(&(cell->decision), &(expected), request.token << 3 | delivery_Executing, memory_order_acq_rel, memory_order_acquire)
               ? AZ_OK
               : AZ_CANCELLED;
}
static int delivery_continuation(const azexit_request request, uint64_t now) {
    __auto_type cell = delivery_find(request.token);
    if (!cell || !delivery_matches(cell, request.deck, request.load_generation) ||
        !delivery_before(now, cell->request.deadline_ns)) return AZ_STALE;
    return atomic_load_explicit(&(cell->decision), memory_order_acquire) == (request.token << 3 | delivery_Executing) &&
                   !atomic_load_explicit(&(cell->stops), memory_order_acquire)
               ? AZ_OK
               : AZ_CANCELLED;
}
static azexit_receipt delivery_receipt(const azexit_request request, unsigned kind, unsigned settled, uint64_t lifetime, uint64_t observed, unsigned outcome, int reason, uint64_t revision) {
    return (azexit_receipt) {sizeof(azexit_receipt), AZEXIT_DELIVERY_VERSION, kind, settled, request.deck,
                             request.token, request.load_generation, lifetime, revision, observed, outcome, reason};
}
static uint64_t delivery_observation_time(void) {
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L ||
        (uint64_t) (ts.tv_sec) > (UINT64_MAX - (uint64_t) (ts.tv_nsec)) / UINT64_C(1000000000)) return 0;
    return (uint64_t) (ts.tv_sec) * UINT64_C(1000000000) + (uint64_t) (ts.tv_nsec);
}
static void delivery_evidence(delivery_Token *cell, unsigned flags, uint64_t observed) {
    if (!observed) observed = delivery_observation_time();
    uint64_t empty = 0;
    if (flags & delivery_Settled) atomic_compare_exchange_strong_explicit(&(cell->settled_ns), &(empty), observed, memory_order_relaxed, memory_order_relaxed);
    empty = 0;
    if (flags & delivery_Retired) atomic_compare_exchange_strong_explicit(&(cell->retired_ns), &(empty), observed, memory_order_relaxed, memory_order_relaxed);
    if ((atomic_fetch_or_explicit(&(cell->evidence), flags, memory_order_acq_rel) & flags) != flags) delivery_wake();
}
static bool delivery_copy_winner(delivery_Token *cell, uint64_t decision, azexit_receipt *out) {
    switch (decision & delivery_decision_bits) {
    case delivery_Committed: (*out) = cell->result; return true;
    case delivery_ExpiredQueued:
    case delivery_ExpiredExecuting:
    case delivery_CancelledQueued: (*out) = cell->stopped; return true;
    default: return false;
    }
}
static bool delivery_terminal_failure(const azexit_receipt value) {
    return value.reason >= AZ_NOT_FOUND && value.reason <= AZ_UNSUPPORTED &&
           (value.outcome == AZ_COMMAND_UNCERTAIN ||
            (value.outcome == AZ_COMMAND_REJECTED && value.settled && value.reason != AZ_CANCELLED));
}
static bool delivery_valid_terminal(const azexit_receipt value) {
    if (value.settled > 1) return false;
    if (value.outcome == AZ_COMMAND_APPLIED)
        return value.settled && value.reason == AZ_OK && value.revision;
    if (value.revision) return false;
    if (value.outcome == AZ_COMMAND_CANCELLED)
        return value.settled && (value.reason == AZ_CANCELLED || value.reason == AZ_STALE);
    return delivery_terminal_failure(value);
}
static bool delivery_uncommitted(uint64_t decision) {
    unsigned phase = decision & delivery_decision_bits;
    return phase == delivery_Queued || phase == delivery_Executing;
}
static void delivery_qualify_commit(delivery_Token *cell, const azexit_receipt *value, uint64_t commit_ns) {
    if (cell->result.outcome != AZ_COMMAND_APPLIED) return;
    uint64_t clock = commit_ns ? commit_ns : delivery_observation_time();
    if (!delivery_before(clock, cell->request.deadline_ns) || !delivery_before(value->observed_ns, cell->request.deadline_ns)) {
        cell->result.outcome = AZ_COMMAND_UNCERTAIN;
        cell->result.reason = clock && value->observed_ns ? AZ_STALE : AZ_FAILED;
        cell->result.revision = 0;
    }
}
static void delivery_commit(delivery_Token *cell, const azexit_receipt *value, uint64_t lifetime, uint64_t commit_ns) {
    uint64_t current = atomic_load_explicit(&cell->decision, memory_order_acquire);
    // One terminal producer owns result storage; a stop uses its separate candidate.
    if (!delivery_uncommitted(current) || atomic_exchange_explicit(&cell->result_writer, true, memory_order_acq_rel)) return;
    cell->result = *value;
    cell->result.lifetime_generation = lifetime;
    while (delivery_uncommitted(current)) {
        delivery_qualify_commit(cell, value, commit_ns);
        if (atomic_compare_exchange_strong_explicit(&cell->decision, &current, value->token << 3 | delivery_Committed, memory_order_acq_rel, memory_order_acquire)) {
            delivery_wake();
            break;
        }
    }
}
static bool delivery_publish_value(delivery_Token *cell, const azexit_receipt *value, uint64_t lifetime, uint64_t commit_ns) {
    if (value->kind == AZEXIT_RECEIPT_TERMINAL && delivery_valid_terminal(*value)) {
        delivery_commit(cell, value, lifetime, commit_ns);
        return true;
    }
    return value->kind == AZEXIT_RECEIPT_SETTLED && value->settled == 1;
}
static bool delivery_lifetime_matches(const azexit_receipt *value, uint64_t lifetime, bool local) {
    return (local || lifetime) && (!value->lifetime_generation || value->lifetime_generation == lifetime);
}
static void delivery_publish_settlement(delivery_Token *cell, const azexit_receipt *value, bool local) {
    if (!value->settled) return;
    delivery_evidence(cell, delivery_Settled | (local ? (unsigned) delivery_Retired : 0u), value->observed_ns);
    if (!local) {
        atomic_store_explicit(&cell->native_work, 1, memory_order_release);
        delivery_notify(value->deck);
    }
}
static int delivery_publish(const azexit_receipt *value, bool local, uint64_t commit_ns) {
    if (!value || value->size < sizeof *value || value->version != AZEXIT_DELIVERY_VERSION) return AZ_INVALID;
    delivery_Token *cell = delivery_find(value->token);
    if (!cell || !delivery_matches(cell, value->deck, value->load_generation)) return AZ_STALE;
    uint64_t lifetime = atomic_load_explicit(&cell->lifetime, memory_order_acquire);
    if (!delivery_lifetime_matches(value, lifetime, local)) return AZ_STALE;
    if (!delivery_publish_value(cell, value, lifetime, commit_ns)) return AZ_INVALID;
    delivery_publish_settlement(cell, value, local);
    return AZ_OK;
}
static int delivery_close_local(const azexit_receipt *value) { return delivery_publish(value, true, 0); }
static bool delivery_stoppable(unsigned phase, bool deadline) {
    return phase == delivery_Queued || (phase == delivery_Executing && deadline);
}
static bool delivery_stop_candidate(delivery_Token *cell, uint64_t token, bool deadline, uint64_t now, uint64_t *current, azexit_receipt *winner) {
    bool executing = (*current & delivery_decision_bits) == delivery_Executing;
    cell->stopped = delivery_receipt(cell->request, AZEXIT_RECEIPT_TERMINAL, !executing,
                                     atomic_load_explicit(&cell->lifetime, memory_order_acquire), now,
                                     executing ? AZ_COMMAND_UNCERTAIN : AZ_COMMAND_CANCELLED, deadline ? AZ_STALE : AZ_CANCELLED, 0);
    unsigned next = deadline ? (executing ? delivery_ExpiredExecuting : delivery_ExpiredQueued) : delivery_CancelledQueued;
    if (!atomic_compare_exchange_strong_explicit(&cell->decision, current, token << 3 | next, memory_order_acq_rel, memory_order_acquire)) return false;
    *winner = cell->stopped;
    delivery_wake();
    return true;
}
static int delivery_choose_stop(delivery_Token *cell, uint64_t token, bool deadline, uint64_t now, azexit_receipt *winner) {
    uint64_t current = atomic_load_explicit(&cell->decision, memory_order_acquire);
    for (;;) {
        if (delivery_copy_winner(cell, current, winner)) return AZ_OK;
        if (!delivery_stoppable(current & delivery_decision_bits, deadline)) return AZ_PENDING;
        if (delivery_stop_candidate(cell, token, deadline, now, &current, winner)) return AZ_OK;
    }
}
static void delivery_notify_stop(delivery_Token *cell, unsigned deck, uint32_t reasons) {
    if (!(atomic_load_explicit(&cell->evidence, memory_order_acquire) & delivery_Retired) &&
        (atomic_fetch_or_explicit(&cell->stops, reasons, memory_order_acq_rel) & reasons) != reasons) delivery_notify(deck);
}
static int delivery_control(unsigned deck, uint64_t token, uint32_t reasons, uint64_t now, azexit_receipt *winner) {
    delivery_Token *cell = delivery_find(token);
    if (!cell || cell->request.deck != deck) return AZ_STALE;
    if (!winner || winner->size < sizeof *winner || !reasons || !now) return AZ_INVALID;
    bool deadline = reasons & AZEXIT_STOP_DEADLINE;
    if (deadline && delivery_before(now, cell->request.deadline_ns)) return AZ_PENDING;
    int rc = delivery_choose_stop(cell, token, deadline, now, winner);
    delivery_notify_stop(cell, deck, reasons);
    return rc;
}
static bool delivery_consume_cell(delivery_Token *cell, azexit_receipt *out) {
    uint64_t decision = atomic_load_explicit(&(cell->decision), memory_order_acquire);
    if (!decision) return false;
    unsigned facts = atomic_load_explicit(&(cell->evidence), memory_order_acquire) & delivery_decision_bits;
    if (!(cell->reported & delivery_Terminal) && delivery_copy_winner(cell, decision, &(*out))) {
        cell->reported |= delivery_Terminal;
        return true;
    }
    unsigned ready = facts & ~cell->reported;
    if (!ready) return false;
    bool settled = ready & delivery_Settled;
    uint64_t observed = atomic_load_explicit(settled ? &cell->settled_ns : &cell->retired_ns, memory_order_acquire);
    *out = delivery_receipt(cell->request, settled ? AZEXIT_RECEIPT_SETTLED : AZEXIT_RECEIPT_RESOURCES_RETIRED,
                            1, atomic_load_explicit(&(cell->lifetime), memory_order_acquire), observed, 0, 0, 0);
    cell->reported |= settled ? delivery_Settled : delivery_Retired;
    return true;
}
static int delivery_consume(azexit_receipt *out) {
    if (!out || out->size < sizeof *out) return AZ_INVALID;
    {
        for (unsigned range_index_3 = 0; range_index_3 < sizeof(delivery_transport.tokens) / sizeof(delivery_transport.tokens)[0]; ++range_index_3) {
            __auto_type cell = &(delivery_transport.tokens[range_index_3]);

            if (delivery_consume_cell(cell, out)) return AZ_OK;
        }
    }
    return AZ_EMPTY;
}
static int delivery_reclaim(const azexit_receipt *value) {
    if (!value || value->size < sizeof *value || value->version != AZEXIT_DELIVERY_VERSION)
        return AZ_INVALID;
    __auto_type cell = delivery_find(value->token);
    if (!cell || !delivery_matches(cell, value->deck, value->load_generation) ||
        value->lifetime_generation != atomic_load_explicit(&(cell->lifetime), memory_order_acquire)) return AZ_STALE;
    if (cell->reported != (delivery_Terminal | delivery_Settled | delivery_Retired)) return AZ_BUSY;
    atomic_store_explicit(&(cell->decision), 0, memory_order_release);
    return AZ_OK;
}

#endif
