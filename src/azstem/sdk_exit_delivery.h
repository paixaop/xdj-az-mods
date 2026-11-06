/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by sdk_exit.cpp. Each reservation owns its result and evidence until
 * the core consumes them and returns the token. The native writer never waits. */
#ifndef AZSDK_EXIT_DELIVERY_H
#define AZSDK_EXIT_DELIVERY_H

namespace azsdk_delivery {
enum Decision {
    Queued = 1,
    Executing,
    Committed,
    ExpiredQueued,
    ExpiredExecuting,
    CancelledQueued
};
enum Evidence {
    Settled = 1,
    Retired = 2,
    Terminal = 4
};
constexpr uint64_t decision_bits = 7;
constexpr uint64_t max_token = UINT64_MAX >> 3;

struct Token {
    // The identity is part of the compare/exchange, including after slot reuse.
    std::atomic<uint64_t> decision {0}, evidence {0};
    std::atomic<unsigned> native_work {0};
    std::atomic<uint32_t> stops {0};
    std::atomic<bool> result_writer {false};
    std::atomic<uint64_t> lifetime {0}, settled_ns {0}, retired_ns {0};
    azexit_request request = {};
    azexit_receipt result = {}, stopped = {};
    unsigned reported = 0; // core dispatcher only
};
struct Transport {
    std::atomic<bool> bound {false};
    std::atomic<unsigned> decks {0};
    int fd = -1; // set before bound; process lifetime thereafter
    Token tokens[AZEXIT_DELIVERY_CAPACITY];
};
static Transport transport;

static void wake() noexcept {
#if defined(__linux__)
    if (!transport.bound.load(std::memory_order_acquire)) return;
    uint64_t one = 1;
    int saved_errno = errno;
    while (write(transport.fd, &one, sizeof one) < 0 && errno == EINTR) {}
    errno = saved_errno;
#endif
}
static void notify(unsigned deck) noexcept {
    if (deck < 4) transport.decks.fetch_or(1u << deck, std::memory_order_release);
    wake();
}
static int bind(const azexit_binding *core) noexcept {
    if (!core || core->size < sizeof *core || core->version != AZEXIT_DELIVERY_VERSION ||
        core->capacity != AZEXIT_DELIVERY_CAPACITY || core->per_owner != AZEXIT_DELIVERY_PER_OWNER)
        return AZ_UNSUPPORTED;
    if (transport.bound.load(std::memory_order_acquire)) return AZ_OK;
#if defined(__linux__)
    transport.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (transport.fd < 0) return AZ_FAILED;
    transport.bound.store(true, std::memory_order_release);
    return AZ_OK;
#elif defined(AZSDK_EXIT_TESTING)
    transport.bound.store(true, std::memory_order_release);
    return AZ_OK;
#else
    return AZ_UNSUPPORTED;
#endif
}
static int event_fd() noexcept {
    return transport.bound.load(std::memory_order_acquire) ? transport.fd : -1;
}
static Token *find(uint64_t token) noexcept {
    if (!token || token > max_token) return nullptr;
    for (auto &cell : transport.tokens)
        if (cell.decision.load(std::memory_order_acquire) >> 3 == token) return &cell;
    return nullptr;
}
static int reserve(const azexit_request *request) noexcept {
    if (!transport.bound.load(std::memory_order_acquire)) return AZ_UNSUPPORTED;
    if (!request || !request->token || request->token > max_token || request->deck >= 4 ||
        !request->load_generation || !request->deadline_ns || request->reserved || find(request->token))
        return AZ_INVALID;
    // Admission and reservation return are serialized by the core store lock.
    for (auto &cell : transport.tokens) {
        if (cell.decision.load(std::memory_order_acquire)) continue;
        cell.request = *request;
        cell.reported = 0;
        cell.lifetime.store(0, std::memory_order_relaxed);
        cell.settled_ns.store(0, std::memory_order_relaxed);
        cell.retired_ns.store(0, std::memory_order_relaxed);
        cell.stops.store(0, std::memory_order_relaxed);
        cell.result_writer.store(false, std::memory_order_relaxed);
        cell.native_work.store(0, std::memory_order_relaxed);
        cell.evidence.store(request->token << 3, std::memory_order_relaxed);
        cell.decision.store(request->token << 3 | Queued, std::memory_order_release);
        return AZ_OK;
    }
    return AZ_LIMIT;
}
static bool matches(const Token &cell, unsigned deck, uint64_t load) noexcept {
    return cell.request.deck == deck && cell.request.load_generation == load;
}
static bool attach(const azexit_request &request, uint64_t lifetime) noexcept {
    auto *cell = find(request.token);
    if (!cell || !matches(*cell, request.deck, request.load_generation) || !lifetime) return false;
    cell->lifetime.store(lifetime, std::memory_order_release);
    return true;
}
static bool before(uint64_t now, uint64_t deadline) noexcept {
    return now && compare_timestamps(now, deadline, 1000000000, 1000000000, 1) == AZ_TIME_BEFORE;
}
static int claim(const azexit_request &request, uint64_t now) noexcept {
    auto *cell = find(request.token);
    if (!cell || !matches(*cell, request.deck, request.load_generation)) return AZ_STALE;
    if (!before(now, cell->request.deadline_ns)) return AZ_STALE;
    if (cell->stops.load(std::memory_order_acquire)) return AZ_CANCELLED;
    uint64_t expected = request.token << 3 | Queued;
    return cell->decision.compare_exchange_strong(expected, request.token << 3 | Executing,
                                                  std::memory_order_acq_rel)
               ? AZ_OK
               : AZ_CANCELLED;
}
static int continuation(const azexit_request &request, uint64_t now) noexcept {
    auto *cell = find(request.token);
    if (!cell || !matches(*cell, request.deck, request.load_generation) ||
        !before(now, cell->request.deadline_ns)) return AZ_STALE;
    return cell->decision.load(std::memory_order_acquire) == (request.token << 3 | Executing) &&
                   !cell->stops.load(std::memory_order_acquire)
               ? AZ_OK
               : AZ_CANCELLED;
}
static azexit_receipt receipt(const azexit_request &request, unsigned kind, unsigned settled,
                              uint64_t lifetime, uint64_t observed, unsigned outcome = 0,
                              int reason = 0, uint64_t revision = 0) noexcept {
    return {sizeof(azexit_receipt), AZEXIT_DELIVERY_VERSION, kind, settled, request.deck,
            request.token, request.load_generation, lifetime, revision, observed, outcome, reason};
}
static uint64_t observation_time() noexcept {
    timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L ||
        uint64_t(ts.tv_sec) > (UINT64_MAX - uint64_t(ts.tv_nsec)) / UINT64_C(1000000000)) return 0;
    return uint64_t(ts.tv_sec) * UINT64_C(1000000000) + uint64_t(ts.tv_nsec);
}
static void evidence(Token &cell, unsigned flags, uint64_t observed = 0) noexcept {
    if (!observed) observed = observation_time();
    uint64_t empty = 0;
    if (flags & Settled) cell.settled_ns.compare_exchange_strong(empty, observed, std::memory_order_relaxed);
    empty = 0;
    if (flags & Retired) cell.retired_ns.compare_exchange_strong(empty, observed, std::memory_order_relaxed);
    if ((cell.evidence.fetch_or(flags, std::memory_order_acq_rel) & flags) != flags) wake();
}
static bool copy_winner(const Token &cell, uint64_t decision, azexit_receipt &out) noexcept {
    switch (decision & decision_bits) {
    case Committed: out = cell.result; return true;
    case ExpiredQueued:
    case ExpiredExecuting:
    case CancelledQueued: out = cell.stopped; return true;
    default: return false;
    }
}
static bool valid_terminal(const azexit_receipt &value) noexcept {
    if (value.settled > 1) return false;
    if (value.outcome == AZ_COMMAND_APPLIED)
        return value.settled && value.reason == AZ_OK && value.revision;
    if (value.revision) return false;
    if (value.outcome == AZ_COMMAND_CANCELLED)
        return value.settled && (value.reason == AZ_CANCELLED || value.reason == AZ_STALE);
    return value.reason >= AZ_NOT_FOUND && value.reason <= AZ_UNSUPPORTED &&
           (value.outcome == AZ_COMMAND_UNCERTAIN ||
            (value.outcome == AZ_COMMAND_REJECTED && value.settled && value.reason != AZ_CANCELLED));
}
static int publish(const azexit_receipt *value, bool local = false, uint64_t commit_ns = 0) noexcept {
    if (!value || value->size < sizeof *value || value->version != AZEXIT_DELIVERY_VERSION)
        return AZ_INVALID;
    auto *cell = find(value->token);
    if (!cell || !matches(*cell, value->deck, value->load_generation)) return AZ_STALE;
    uint64_t lifetime = cell->lifetime.load(std::memory_order_acquire);
    if ((!local && !lifetime) || (value->lifetime_generation && value->lifetime_generation != lifetime))
        return AZ_STALE;
    if (value->kind == AZEXIT_RECEIPT_TERMINAL && valid_terminal(*value)) {
        uint64_t current = cell->decision.load(std::memory_order_acquire);
        /* Each request has one terminal producer: its direct mailbox, cue actor,
         * or local rejection. The stop decision uses its own candidate storage. */
        if (((current & decision_bits) == Queued || (current & decision_bits) == Executing) &&
            !cell->result_writer.exchange(true, std::memory_order_acq_rel)) {
            cell->result = *value;
            cell->result.lifetime_generation = lifetime;
            while ((current & decision_bits) == Queued || (current & decision_bits) == Executing) {
                if (cell->result.outcome == AZ_COMMAND_APPLIED) {
                    uint64_t clock = commit_ns ? commit_ns : observation_time();
                    if (!before(clock, cell->request.deadline_ns) ||
                        !before(value->observed_ns, cell->request.deadline_ns)) {
                        cell->result.outcome = AZ_COMMAND_UNCERTAIN;
                        cell->result.reason = clock && value->observed_ns ? AZ_STALE : AZ_FAILED;
                        cell->result.revision = 0;
                    }
                }
                if (cell->decision.compare_exchange_strong(current, value->token << 3 | Committed,
                                                           std::memory_order_acq_rel)) {
                    wake();
                    break;
                }
            }
        }
    } else if (value->kind != AZEXIT_RECEIPT_SETTLED || value->settled != 1) return AZ_INVALID;
    if (value->settled) {
        evidence(*cell, Settled | (local ? unsigned(Retired) : 0u), value->observed_ns);
        if (!local) {
            cell->native_work.store(1, std::memory_order_release);
            notify(value->deck);
        }
    }
    return AZ_OK;
}
static int close_local(const azexit_receipt *value) noexcept { return publish(value, true); }
static int control(unsigned deck, uint64_t token, uint32_t reasons, uint64_t now,
                   azexit_receipt *winner) noexcept {
    auto *cell = find(token);
    if (!cell || cell->request.deck != deck) return AZ_STALE;
    if (!winner || winner->size < sizeof *winner || !reasons || !now) return AZ_INVALID;
    bool deadline = reasons & AZEXIT_STOP_DEADLINE;
    if (deadline && before(now, cell->request.deadline_ns)) return AZ_PENDING;
    uint64_t current = cell->decision.load(std::memory_order_acquire);
    int rc = AZ_PENDING;
    for (;;) {
        if (copy_winner(*cell, current, *winner)) {
            rc = AZ_OK;
            break;
        }
        unsigned phase = current & decision_bits;
        if (phase != Queued && (phase != Executing || !deadline)) break;
        bool executing = phase == Executing;
        cell->stopped = receipt(cell->request, AZEXIT_RECEIPT_TERMINAL, !executing,
                                cell->lifetime.load(std::memory_order_acquire), now,
                                executing ? AZ_COMMAND_UNCERTAIN : AZ_COMMAND_CANCELLED,
                                deadline ? AZ_STALE : AZ_CANCELLED);
        unsigned next = deadline ? (executing ? ExpiredExecuting : ExpiredQueued) : CancelledQueued;
        if (cell->decision.compare_exchange_strong(current, token << 3 | next,
                                                   std::memory_order_acq_rel)) {
            *winner = cell->stopped;
            rc = AZ_OK;
            wake();
            break;
        }
    }
    if (!(cell->evidence.load(std::memory_order_acquire) & Retired) &&
        (cell->stops.fetch_or(reasons, std::memory_order_acq_rel) & reasons) != reasons)
        notify(deck);
    return rc;
}
static int consume(azexit_receipt *out) noexcept {
    if (!out || out->size < sizeof *out) return AZ_INVALID;
    for (auto &cell : transport.tokens) {
        uint64_t decision = cell.decision.load(std::memory_order_acquire);
        if (!decision) continue;
        unsigned facts = cell.evidence.load(std::memory_order_acquire) & decision_bits;
        if (!(cell.reported & Terminal) && copy_winner(cell, decision, *out)) {
            cell.reported |= Terminal;
            return AZ_OK;
        }
        unsigned ready = facts & ~cell.reported;
        if (!ready) continue;
        bool settled = ready & Settled;
        uint64_t observed = (settled ? cell.settled_ns : cell.retired_ns).load(std::memory_order_acquire);
        *out = receipt(cell.request, settled ? AZEXIT_RECEIPT_SETTLED : AZEXIT_RECEIPT_RESOURCES_RETIRED,
                       1, cell.lifetime.load(std::memory_order_acquire), observed);
        cell.reported |= settled ? Settled : Retired;
        return AZ_OK;
    }
    return AZ_EMPTY;
}
static int reclaim(const azexit_receipt *value) noexcept {
    if (!value || value->size < sizeof *value || value->version != AZEXIT_DELIVERY_VERSION)
        return AZ_INVALID;
    auto *cell = find(value->token);
    if (!cell || !matches(*cell, value->deck, value->load_generation) ||
        value->lifetime_generation != cell->lifetime.load(std::memory_order_acquire)) return AZ_STALE;
    if (cell->reported != (Terminal | Settled | Retired)) return AZ_BUSY;
    cell->decision.store(0, std::memory_order_release);
    return AZ_OK;
}
} // namespace azsdk_delivery
#endif
