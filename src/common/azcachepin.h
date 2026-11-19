/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEPIN_H
#define AZCACHEPIN_H
#include "azcachebirth.h"
#include "azsources.h"
#include <string.h>
/* Pre-forward snapshots from an ALREADY owned/serialized source, Collector and
 * Creator holder. Creator's holder is a real one-word owning raw pointer, not a
 * shared owner; its database at+0x30 and Collector's pool at+0x40 have independently
 * proved layouts. No raw native holder is reread after initialization. */
struct azcp_pins {
    struct az_native_owner mount, database, creator_database, collector;
    struct azcb_source_borrow source;
    const void *creator, *pool;
    int status;
};
static inline void azcp_pins_drop(struct azcp_pins *p) {
    az_native_owner_reset(&p->collector);
    az_native_owner_reset(&p->creator_database);
    az_native_owner_reset(&p->database);
    az_native_owner_reset(&p->mount);
}
static inline bool azcp_source_available(const struct azcb_source_borrow *source) {
    return source->server && source->source_epoch && source->build >= 1 && source->build <= 2 && source->mount && source->database;
}
static inline bool azcp_pin_owners(struct azcp_pins *p, const struct azcb_source_borrow *source,
                                   const struct az_native_owner *actual_collector, const void *receiver_collector) {
    az_native_owner_copy(&p->mount, source->mount);
    az_native_owner_copy(&p->database, source->database);
    az_native_owner_copy(&p->collector, actual_collector);
    return p->mount.object && p->database.object && p->collector.object && p->collector.object == receiver_collector;
}
static inline void azcp_pin_creator(struct azcp_pins *p, const void *creator_holder) {
    memcpy(&p->creator, creator_holder, sizeof p->creator);
    uintptr_t creator = (uintptr_t) p->creator, collector = (uintptr_t) p->collector.object;
    if (!creator || creator > UINTPTR_MAX - 0x30 || collector > UINTPTR_MAX - 0x40) return;
    az_native_owner_copy(&p->creator_database, (const struct az_native_owner *) (creator + 0x30));
    if (!p->creator_database.object || p->creator_database.object != p->database.object || p->creator_database.control != p->database.control) return;
    memcpy(&p->pool, (const void *) (collector + 0x40), sizeof p->pool);
    if (p->pool) p->status = AZS_OK;
}
static inline void azcp_pins_init(struct azcp_pins *p, const struct azcb_source_borrow *source,
                                  const struct az_native_owner *actual_collector, const void *receiver_collector, const void *creator_holder) {
    *p = (struct azcp_pins) {.source = {source->server, source->build, source->source_epoch, &p->mount, &p->database}, .status = AZS_GUARD};
    if (!azcp_source_available(source) || !actual_collector || !creator_holder) return;
    if (!azcp_pin_owners(p, source, actual_collector, receiver_collector)) return;
    azcp_pin_creator(p, creator_holder);
}
static inline bool azcp_pins_ready(const struct azcp_pins *p) { return p->status == AZS_OK; }
static inline struct azcb_birth_borrow azcp_pins_borrow(const struct azcp_pins *p, const struct az_native_owner *native_result) {
    return (struct azcb_birth_borrow) {p->source, &p->collector, &p->creator_database, p->collector.object, p->creator, p->pool, native_result};
}
/* Observer synchronously forwards once; it cannot retain the borrow or catch
 * native unwind. Pins stay in their immovable caller scope and own no result. */
static inline int azcp_forward_owned(const struct azcb_source_borrow *source, const struct az_native_owner *collector,
                                     const void *receiver, const void *creator, const struct az_native_owner *result,
                                     struct azcb_forward original, int (*observe)(const struct azcb_birth_borrow *, struct azcb_forward, void *), void *context) {
    if (!original.call) return AZS_INVALID;
    if (!result) {
        original.call(original.context);
        return AZS_GUARD;
    }
    struct azcp_pins pins __attribute__((cleanup(azcp_pins_drop)));
    azcp_pins_init(&pins, source, collector, receiver, creator);
    if (!azcp_pins_ready(&pins)) {
        original.call(original.context);
        return pins.status;
    }
    const struct azcb_birth_borrow borrow = azcp_pins_borrow(&pins, result);
    return observe(&borrow, original, context);
}
#endif
