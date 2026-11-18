/* SPDX-License-Identifier: GPL-3.0-only */
#include "azexit.h"
#include "azmod-mod.h"
#include "aztimestamp.h"
#include <stdatomic.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
typedef struct azexit_request azexit_request;
typedef struct azexit_receipt azexit_receipt;
typedef struct azexit_binding azexit_binding;
typedef struct azexit_result azexit_result;
typedef struct azexit_delivery_v2 azexit_delivery_v2;
typedef struct azexit_service azexit_service;
#include "../../src/azstem/sdk_exit_delivery.h"



_Atomic(uint64_t) mailbox[4];
_Atomic(unsigned) stopped[4];
_Atomic(bool) advertised = true;
int activity;

void signal() {
    uint64_t one = 1;
    while (write(activity, &one, sizeof one) < 0 && errno == EINTR) {}
}
uint64_t clock_ns() {
    struct timespec ts = {0};
    assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return (uint64_t) (ts.tv_sec) * UINT64_C(1000000000) + (uint64_t) (ts.tv_nsec);
}
unsigned mask() {
    return atomic_load_explicit(&(delivery_transport.bound), memory_order_acquire) && atomic_load_explicit(&(advertised), memory_order_acquire) ? 15u : 0u;
}
uint64_t ready(unsigned deck) { return deck < 4 && mask() ? UINT64_C(1) << AZ_CMD_LOOP_EXIT : 0; }
int target(unsigned deck, uint64_t *load) {
    *load = deck + 10;
    return AZ_OK;
}
int submit(const azexit_request *request) {
    unsigned deck = request->deck;
    assert(!atomic_load_explicit(&(mailbox[deck]), memory_order_acquire));
    assert(delivery_attach(*request, deck + 100));
    atomic_store_explicit(&(mailbox[deck]), request->token, memory_order_release);
    signal();
    return AZ_PENDING;
}
int old_poll(unsigned unused, azexit_result *out) {
    (void) unused;
    (void) out;
    abort();
}
int old_control(unsigned unused, uint64_t token) {
    (void) unused;
    (void) token;
    abort();
}
void drive() {
    unsigned decks = atomic_exchange_explicit(&(delivery_transport.decks), 0, memory_order_acq_rel);
    for (unsigned deck = 0; deck < 4; ++deck) {
        if (!(decks & (1u << deck))) continue;
        delivery_Token *cell = delivery_find(atomic_load_explicit(&(mailbox[deck]), memory_order_acquire));
        if (!cell) continue;
        unsigned stops = atomic_load_explicit(&(cell->stops), memory_order_acquire);
        if (stops && atomic_exchange_explicit(&(stopped[deck]), stops, memory_order_acq_rel) != stops) signal();
        if (atomic_exchange_explicit(&(cell->native_work), 0, memory_order_acq_rel)) {
            atomic_store_explicit(&(mailbox[deck]), 0, memory_order_release);
            delivery_evidence(cell, delivery_Retired, 0);
            signal();
        }
    }
}
const azexit_delivery_v2 delivery = {
    sizeof delivery, AZEXIT_DELIVERY_VERSION, delivery_event_fd, delivery_consume, delivery_reclaim, delivery_control,
    delivery_bind, delivery_reserve, delivery_close_local, drive, ready};
const azexit_service service = {
    sizeof service, 1, mask, target, submit, old_control, old_poll, old_control,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, &delivery};


const azexit_service *fixture_service(void) {
    activity = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(activity >= 0);
    return &service;
}
int fixture_activity_fd(void) { return activity; }
uint64_t fixture_token(unsigned deck) { return atomic_load_explicit(&(mailbox[deck]), memory_order_acquire); }
unsigned fixture_stop(unsigned deck) { return atomic_load_explicit(&(stopped[deck]), memory_order_acquire); }
void fixture_advertise(int enabled) { atomic_store_explicit(&(advertised), enabled, memory_order_release); }
int fixture_claim(unsigned deck, uint64_t token) {
    delivery_Token *cell = delivery_find(token);
    assert(cell && cell->request.deck == deck);
    return delivery_claim(cell->request, clock_ns());
}
void fixture_finish(unsigned deck, uint64_t token, unsigned outcome, int reason, unsigned settled) {
    delivery_Token *cell = delivery_find(token);
    assert(cell && cell->request.deck == deck);
    azexit_receipt value = delivery_receipt(cell->request, AZEXIT_RECEIPT_TERMINAL, settled, deck + 100,
                                            clock_ns(), outcome, reason, outcome == AZ_COMMAND_APPLIED ? token : 0);
    assert(delivery_publish(&value, false, 0) == AZ_OK);
}
void fixture_settle(unsigned deck, uint64_t token) {
    delivery_Token *cell = delivery_find(token);
    assert(cell && cell->request.deck == deck);
    azexit_receipt value = delivery_receipt(cell->request, AZEXIT_RECEIPT_SETTLED, 1, deck + 100, clock_ns(), 0, 0, 0);
    assert(delivery_publish(&value, false, 0) == AZ_OK);
}
void fixture_arbitration(void) {
    azexit_binding core = {sizeof core, AZEXIT_DELIVERY_VERSION, AZEXIT_DELIVERY_CAPACITY, AZEXIT_DELIVERY_PER_OWNER};
    assert(delivery_bind(&core) == AZ_OK);
    for (uint64_t token = 1000; token < 1003; ++token) {
        azexit_request request = {token, 10, 200, 0, 0};
        assert(delivery_reserve(&request) == AZ_OK && delivery_attach(request, 100));
        if (token != 1000) assert(delivery_claim(request, 100) == AZ_OK);
        azexit_receipt applied = delivery_receipt(request, AZEXIT_RECEIPT_TERMINAL, 1, 100, 199, AZ_COMMAND_APPLIED, AZ_OK, 7);
        azexit_receipt winner = {0};
        winner.size = sizeof winner;
        if (token == 1001) assert(delivery_publish(&applied, false, 199) == AZ_OK);
        /* The dispatcher has not consumed any wake or delivery_receipt. */
        assert(delivery_control(0, token, AZEXIT_STOP_DEADLINE, 200, &winner) == AZ_OK);
        assert(winner.outcome == (token == 1000 ? AZ_COMMAND_CANCELLED : token == 1001 ? AZ_COMMAND_APPLIED
                                                                                       : AZ_COMMAND_UNCERTAIN));
        assert(winner.settled == (token == 1002 ? 0u : 1u));
        if (token == 1000) assert(delivery_claim(request, 199) == AZ_CANCELLED);
        if (token == 1002) assert(delivery_publish(&applied, false, 0) == AZ_OK); // contributes settlement only
        azexit_receipt settled = delivery_receipt(request, AZEXIT_RECEIPT_SETTLED, 1, 100, 201, 0, 0, 0);
        assert(delivery_publish(&settled, false, 0) == AZ_OK);
        delivery_evidence(delivery_find(token), delivery_Retired, 0);
        azexit_receipt copied = {0};
        copied.size = sizeof copied;
        assert(delivery_consume(&copied) == AZ_OK && copied.kind == AZEXIT_RECEIPT_TERMINAL && copied.outcome == winner.outcome);
        assert(delivery_consume(&copied) == AZ_OK && copied.kind == AZEXIT_RECEIPT_SETTLED);
        assert(delivery_consume(&copied) == AZ_OK && copied.kind == AZEXIT_RECEIPT_RESOURCES_RETIRED);
        assert(delivery_reclaim(&copied) == AZ_OK);
        assert(delivery_publish(&applied, false, 0) == AZ_STALE && delivery_consume(&copied) == AZ_EMPTY);
    }
}
