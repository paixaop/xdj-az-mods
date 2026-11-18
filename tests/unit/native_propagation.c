/* SPDX-License-Identifier: GPL-3.0-only */
/* This DSO has production C cleanup only. The test's outer handler is linked
 * separately, so it cannot replace this DSO's platform personality. */
#include "../../src/common/aznativecommands.h"
#include "../../src/azmod/azmod-mod.h"
#include <assert.h>

struct propagation {
    struct az_owner_gate gate;
    void (*call)(void *);
    void *argument;
    unsigned *cleaned;
};
static bool check(void *opaque) {
    struct propagation *p = opaque;
    p->call(p->argument);
    return true;
}
static void release(struct propagation *p) {
    assert(!az_owner_held(&p->gate));
    az_owner_gate_drop(&p->gate);
    ++*p->cleaned;
}
__attribute__((visibility("default"))) int native_propagate(void (*call)(void *), void *argument, unsigned *cleaned) {
    struct propagation p __attribute__((cleanup(release))) = {AZ_OWNER_GATE_INIT, call, argument, cleaned};
    uint64_t generation = 0;
    assert(az_owner_publish(&p.gate, &p, check, &generation) == AZ_OK);
    AZ_OWNER_LEASE(lease);
    return az_owner_enter(&p.gate, &lease);
}
