/* SPDX-License-Identifier: GPL-3.0-only */
#include "azexit.h"
#include "azmod-mod.h"
#include "aztimestamp.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#include "../../src/azstem/sdk_exit_delivery.h"

namespace {
using namespace azsdk_delivery;
std::atomic<uint64_t> mailbox[4];
std::atomic<unsigned> stopped[4];
std::atomic<bool> advertised {true};
int activity;

void signal() {
    uint64_t one = 1;
    while (write(activity, &one, sizeof one) < 0 && errno == EINTR) {}
}
uint64_t clock_ns() {
    timespec ts = {};
    assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return uint64_t(ts.tv_sec) * UINT64_C(1000000000) + uint64_t(ts.tv_nsec);
}
unsigned mask() {
    return transport.bound.load(std::memory_order_acquire) && advertised.load(std::memory_order_acquire) ? 15u : 0u;
}
uint64_t ready(unsigned deck) { return deck < 4 && mask() ? UINT64_C(1) << AZ_CMD_LOOP_EXIT : 0; }
int target(unsigned deck, uint64_t *load) {
    *load = deck + 10;
    return AZ_OK;
}
int submit(const azexit_request *request) {
    unsigned deck = request->deck;
    assert(!mailbox[deck].load(std::memory_order_acquire));
    assert(attach(*request, deck + 100));
    mailbox[deck].store(request->token, std::memory_order_release);
    signal();
    return AZ_PENDING;
}
int old_poll(unsigned, azexit_result *) { std::abort(); }
int old_control(unsigned, uint64_t) { std::abort(); }
void drive() {
    unsigned decks = transport.decks.exchange(0, std::memory_order_acq_rel);
    for (unsigned deck = 0; deck < 4; ++deck) {
        if (!(decks & (1u << deck))) continue;
        auto *cell = find(mailbox[deck].load(std::memory_order_acquire));
        if (!cell) continue;
        unsigned stops = cell->stops.load(std::memory_order_acquire);
        if (stops && stopped[deck].exchange(stops, std::memory_order_acq_rel) != stops) signal();
        if (cell->native_work.exchange(0, std::memory_order_acq_rel)) {
            mailbox[deck].store(0, std::memory_order_release);
            evidence(*cell, Retired);
            signal();
        }
    }
}
const azexit_delivery_v2 delivery = {
    sizeof delivery, AZEXIT_DELIVERY_VERSION, event_fd, consume, reclaim, control,
    bind, reserve, close_local, drive, ready};
const azexit_service service = {
    sizeof service, 1, mask, target, submit, old_control, old_poll, old_control,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &delivery};
} // namespace

extern "C" const azexit_service *fixture_service(void) {
    activity = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(activity >= 0);
    return &service;
}
extern "C" int fixture_activity_fd(void) { return activity; }
extern "C" uint64_t fixture_token(unsigned deck) { return mailbox[deck].load(std::memory_order_acquire); }
extern "C" unsigned fixture_stop(unsigned deck) { return stopped[deck].load(std::memory_order_acquire); }
extern "C" void fixture_advertise(int enabled) { advertised.store(enabled, std::memory_order_release); }
extern "C" int fixture_claim(unsigned deck, uint64_t token) {
    auto *cell = find(token);
    assert(cell && cell->request.deck == deck);
    return claim(cell->request, clock_ns());
}
extern "C" void fixture_finish(unsigned deck, uint64_t token, unsigned outcome, int reason, unsigned settled) {
    auto *cell = find(token);
    assert(cell && cell->request.deck == deck);
    azexit_receipt value = receipt(cell->request, AZEXIT_RECEIPT_TERMINAL, settled, deck + 100,
                                   clock_ns(), outcome, reason, outcome == AZ_COMMAND_APPLIED ? token : 0);
    assert(publish(&value) == AZ_OK);
}
extern "C" void fixture_settle(unsigned deck, uint64_t token) {
    auto *cell = find(token);
    assert(cell && cell->request.deck == deck);
    azexit_receipt value = receipt(cell->request, AZEXIT_RECEIPT_SETTLED, 1, deck + 100, clock_ns());
    assert(publish(&value) == AZ_OK);
}
extern "C" void fixture_arbitration(void) {
    azexit_binding core = {sizeof core, AZEXIT_DELIVERY_VERSION, AZEXIT_DELIVERY_CAPACITY, AZEXIT_DELIVERY_PER_OWNER};
    assert(bind(&core) == AZ_OK);
    for (uint64_t token = 1000; token < 1003; ++token) {
        azexit_request request = {token, 10, 200, 0, 0};
        assert(reserve(&request) == AZ_OK && attach(request, 100));
        if (token != 1000) assert(claim(request, 100) == AZ_OK);
        azexit_receipt applied = receipt(request, AZEXIT_RECEIPT_TERMINAL, 1, 100, 199, AZ_COMMAND_APPLIED, AZ_OK, 7);
        azexit_receipt winner = {};
        winner.size = sizeof winner;
        if (token == 1001) assert(publish(&applied, false, 199) == AZ_OK);
        /* The dispatcher has not consumed any wake or receipt. */
        assert(control(0, token, AZEXIT_STOP_DEADLINE, 200, &winner) == AZ_OK);
        assert(winner.outcome == (token == 1000 ? AZ_COMMAND_CANCELLED :
                                  token == 1001 ? AZ_COMMAND_APPLIED : AZ_COMMAND_UNCERTAIN));
        assert(winner.settled == (token == 1002 ? 0u : 1u));
        if (token == 1000) assert(claim(request, 199) == AZ_CANCELLED);
        if (token == 1002) assert(publish(&applied) == AZ_OK); // contributes settlement only
        azexit_receipt settled = receipt(request, AZEXIT_RECEIPT_SETTLED, 1, 100, 201);
        assert(publish(&settled) == AZ_OK);
        evidence(*find(token), Retired);
        azexit_receipt copied = {};
        copied.size = sizeof copied;
        assert(consume(&copied) == AZ_OK && copied.kind == AZEXIT_RECEIPT_TERMINAL && copied.outcome == winner.outcome);
        assert(consume(&copied) == AZ_OK && copied.kind == AZEXIT_RECEIPT_SETTLED);
        assert(consume(&copied) == AZ_OK && copied.kind == AZEXIT_RECEIPT_RESOURCES_RETIRED);
        assert(reclaim(&copied) == AZ_OK);
        assert(publish(&applied) == AZ_STALE && consume(&copied) == AZ_EMPTY);
    }
}
