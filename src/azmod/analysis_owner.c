/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_source.h"
#include <stdatomic.h>
#define ANALYSIS_OWNERS 32
static _Atomic uint64_t generations[ANALYSIS_OWNERS + 1];
static _Atomic uint64_t sequence;
static _Thread_local az_analysis_owner thread_owner;
static uint64_t next_generation(void) {
    uint64_t before = atomic_load_explicit(&sequence, memory_order_relaxed);
    do {
        if (before == UINT64_MAX) return 0;
    } while (!atomic_compare_exchange_weak_explicit(&sequence, &before, before + 1,
                                                   memory_order_relaxed, memory_order_relaxed));
    return before + 1;
}
az_analysis_owner analysis_owner_begin(unsigned slot) {
    if (slot >= ANALYSIS_OWNERS) return (az_analysis_owner) {0};
    uint64_t generation = next_generation();
    atomic_store_explicit(&generations[slot], generation, memory_order_release);
    return (az_analysis_owner) {generation, slot, AZ_ANALYSIS_OWNER_MOD};
}
az_analysis_owner analysis_owner_get(unsigned slot) {
    if (slot >= ANALYSIS_OWNERS) return (az_analysis_owner) {0};
    return (az_analysis_owner) {atomic_load_explicit(&generations[slot], memory_order_acquire), slot, AZ_ANALYSIS_OWNER_MOD};
}
void analysis_owner_thread_bind(az_analysis_owner owner) {
    thread_owner = owner;
}
az_analysis_owner analysis_owner_for_caller(unsigned slot) {
    if (thread_owner.kind == AZ_ANALYSIS_OWNER_MOD && thread_owner.slot == slot) return thread_owner;
    return analysis_owner_get(slot);
}
az_analysis_owner analysis_owner_core(void) {
    return (az_analysis_owner) {atomic_load_explicit(&generations[ANALYSIS_OWNERS], memory_order_acquire), 0, AZ_ANALYSIS_OWNER_CORE};
}
az_analysis_owner analysis_owner_core_begin(void) {
    uint64_t generation = next_generation();
    atomic_store_explicit(&generations[ANALYSIS_OWNERS], generation, memory_order_release);
    return (az_analysis_owner) {generation, 0, AZ_ANALYSIS_OWNER_CORE};
}
static int owner_index(az_analysis_owner owner) {
    if (owner.kind == AZ_ANALYSIS_OWNER_CORE) return owner.slot ? -1 : ANALYSIS_OWNERS;
    if (owner.kind != AZ_ANALYSIS_OWNER_MOD || owner.slot >= ANALYSIS_OWNERS) return -1;
    return (int) owner.slot;
}
int analysis_owner_current(az_analysis_owner owner) {
    int index = owner_index(owner);
    if (index < 0 || !owner.generation) return 0;
    return atomic_load_explicit(&generations[index], memory_order_acquire) == owner.generation;
}
az_analysis_owner analysis_owner_retire(unsigned slot) {
    if (slot >= ANALYSIS_OWNERS) return (az_analysis_owner) {0};
    uint64_t generation = atomic_exchange_explicit(&generations[slot], 0, memory_order_acq_rel);
    return (az_analysis_owner) {generation, slot, AZ_ANALYSIS_OWNER_MOD};
}
void analysis_owner_core_retire(void) {
    atomic_store_explicit(&generations[ANALYSIS_OWNERS], 0, memory_order_release);
}
