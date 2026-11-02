/* SPDX-License-Identifier: GPL-3.0-only */
/* bpmstate_capture / bpmstate_read (extracted from azmod/stems_display.c by run.sh): a
 * deck's gui::BpmState is used only through EP147's own shared_ptr protocol. It lives 0x10 into
 * a make_shared control block {vtable, int use, int weak}. The ctor hook takes a weak reference
 * after checking the block's vtable and deck byte. Weak retention protects storage only;
 * reads must promote/release a genuine shared owner and reject zero/overflow counts.
 * Every case below runs on a fake GNU-layout control block. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#define AZMOD_STEM_DECKS 4
#define BPMSTATE_LIVE 0xa0
static const void *read_probe;
static int *read_use;
static int drop_native, saw_shared_lease, disposed;
static void (*load_action)(void);
static void observe_load(const void *p) {
    if (p == read_probe && load_action) {
        void (*action)(void) = load_action;
        load_action = NULL;
        action();
    }
    if (drop_native && p == read_probe) {
        drop_native = 0;
        saw_shared_lease = __atomic_load_n(read_use, __ATOMIC_RELAXED) == 2;
        (void) __atomic_fetch_sub(read_use, 1, __ATOMIC_ACQ_REL);
    }
}
#define __atomic_load_n(p, order) __extension__ ({ __typeof__(*(p)) v = __atomic_load_n((p), (order)); observe_load(p); v; })
#include BPM_INC
#undef __atomic_load_n
#include "check.h"

static void fake_dispose(void *control) { (void) control; disposed++; }
static void fake_destroy(void *control) { (void) control; abort(); }
static void (*fake_vtable[5])(void *) = {NULL, NULL, fake_dispose, fake_destroy, NULL};
#define FAKE_VT ((uintptr_t) fake_vtable)
struct blk {
    _Alignas(16) unsigned char b[0xd0];
};
static struct blk *fixture_blocks[128];
static unsigned fixture_count;
static unsigned long obj_of(struct blk *k) { return (unsigned long) (k->b + BPMCTRL_OFF); }
static int *use_of(struct blk *k) { return (int *) (k->b + BPMCTRL_USE); }
static int *weak_of(struct blk *k) { return (int *) (k->b + BPMCTRL_WEAK); }
static struct blk *mk(int deck, double live, double track) {
    struct blk *k = aligned_alloc(16, sizeof *k);
    if (!k || fixture_count == sizeof fixture_blocks / sizeof *fixture_blocks) abort();
    fixture_blocks[fixture_count++] = k;
    memset(k, 0, sizeof *k);
    uintptr_t vt = FAKE_VT;
    memcpy(k->b, &vt, 8);
    *use_of(k) = 1;
    *weak_of(k) = 1;
    unsigned long id = (unsigned long) deck | (0x5a5a5a5aUL << 8);
    memcpy(k->b + BPMCTRL_OFF + BPMSTATE_DECK, &id, 8);
    memcpy(k->b + BPMCTRL_OFF + BPMSTATE_LIVE, &live, 8);
    memcpy(k->b + BPMCTRL_OFF + BPMSTATE_TRACK, &track, 8);
    return k;
}
static unsigned long replacement;
static void replace_during_read(void) {
    if (!bpmstate_capture(replacement, 1, FAKE_VT)) abort();
}

int main(void) {
    ut_suite = "bpmstate";
    double live = 0, track = 0;
    CHECK("nothing captured: deck 1 unknown", !bpmstate_read(1, &live, &track));

    struct blk *a = mk(1, 126.5, 128.0);
    CHECK("capture: the make_shared block's vtable and deck byte accepted",
          bpmstate_capture(obj_of(a), 1, FAKE_VT) == 1);
    CHECK("capture takes a weak reference (weak count 1 -> 2), use count untouched",
          *weak_of(a) == 2 && *use_of(a) == 1);
    CHECK("read: live and analyzed BPM of deck 1",
          bpmstate_read(1, &live, &track) == 1 && live == 126.5 && track == 128.0);
    CHECK("read: another deck stays unknown", !bpmstate_read(0, &live, NULL));

    struct blk *lease = mk(3, 123.5, 124.0);
    CHECK("lease fixture captured", bpmstate_capture(obj_of(lease), 3, FAKE_VT));
    read_probe = lease->b + BPMCTRL_OFF + BPMSTATE_DECK;
    read_use = use_of(lease);
    drop_native = 1;
    int copied = bpmstate_read(3, &live, &track);
    CHECK("last native owner drops during copy: reader holds a promoted shared owner",
          saw_shared_lease && copied && live == 123.5 && track == 124.0);
    CHECK("final reader release disposes once and releases only the implicit weak owner",
          disposed == 1 && *use_of(lease) == 0 && *weak_of(lease) == 1);
    read_probe = NULL;

    /* EP147 drops its last shared_ptr: use count 0, the object is being (or was) disposed */
    *use_of(a) = 0;
    CHECK("disposed (use count 0): unknown, never the stale value", !bpmstate_read(1, &live, NULL));
    *use_of(a) = 1;

    /* the block's memory now holds another deck's object (+0x08 word changed) */
    unsigned long other = 1 | (0x6b6b6b6bUL << 8);
    memcpy(a->b + BPMCTRL_OFF + BPMSTATE_DECK, &other, 8);
    CHECK("object no longer the one captured (+0x08 changed): unknown", !bpmstate_read(1, &live, NULL));

    struct blk *w = mk(2, 100.0, 100.0);
    uintptr_t bad = 0x1234560;
    memcpy(w->b, &bad, 8);
    CHECK("capture: a block with another vtable is refused and not pinned",
          bpmstate_capture(obj_of(w), 2, FAKE_VT) == 0 && *weak_of(w) == 1 &&
              !bpmstate_read(2, &live, NULL));

    struct blk *m = mk(3, 90.0, 90.0);
    CHECK("capture: the deck argument must match the object's deck byte",
          bpmstate_capture(obj_of(m), 2, FAKE_VT) == 0 && *weak_of(m) == 1);
    CHECK("capture: misaligned object refused", bpmstate_capture(obj_of(m) + 4, 3, FAKE_VT) == 0);

    /* a rebuilt BpmState for deck 1 replaces the old one */
    struct blk *b = mk(1, 140.0, 140.0);
    unsigned long original = 1 | (0x5a5a5a5aUL << 8);
    memcpy(a->b + BPMCTRL_OFF + BPMSTATE_DECK, &original, 8);
    replacement = obj_of(b);
    read_probe = a->b + BPMCTRL_OFF + BPMSTATE_DECK;
    load_action = replace_during_read;
    int superseded = bpmstate_read(1, &live, NULL);
    read_probe = NULL;
    CHECK("replacement during copy refuses the old state, even with the same deck/hash",
          !superseded && bpmstate_read(1, &live, NULL) == 1 &&
              live == 140.0);

    /* pins are bounded: past BPMSTATE_PIN_MAX a new object is not used */
    int took = 0;
    for (int i = 0; i < BPMSTATE_PIN_MAX + 8; i++) {
        struct blk *k = mk(0, 120.0 + i, 0);
        took += bpmstate_capture(obj_of(k), 0, FAKE_VT);
    }
    CHECK("pins bounded at BPMSTATE_PIN_MAX (no unbounded leak of control blocks)",
          took == BPMSTATE_PIN_MAX - 3 && g_bpmstate_refused == 11);
    int status = ut_summary();
    /* No native calls follow fixture teardown. These are test-owned control
     * models, not actual GNU allocations whose weak pins must outlive hooks. */
    memset(g_bpmstate, 0, sizeof g_bpmstate);
    memset(g_bpmstate_generation, 0, sizeof g_bpmstate_generation);
    for (unsigned i = 0; i < fixture_count; ++i) free(fixture_blocks[i]);
    return status;
}
