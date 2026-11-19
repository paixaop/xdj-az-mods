/* SPDX-License-Identifier: GPL-3.0-only */
#include "azstatus_native.h"
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define AZ_STATUS_NATIVE_PRODUCER 1
#define NDECK 4
#define LTI_VT ((uintptr_t) 0x100000)
#define LTI_ID_OFF 0x08
#define LTI_TITLE_OFF 0x38
#define LTI_ARTIST_OFF 0x40
static _Atomic uint64_t g_ls_gen[NDECK];
static int g_status_lti_on = 1;
static uint64_t lifecycle = 7, seed = 11, empty_fence = 13;
static unsigned offers, emits, commits, capability;
static int accept_offer = 1, native_change;
static struct azstatus_native_receipt offered;
uint64_t azn_lifecycle_fence(unsigned deck) {
    assert(deck < NDECK);
    return lifecycle;
}
uint64_t azn_seed_id(unsigned deck) {
    assert(deck < NDECK);
    return seed;
}
uint64_t azn_empty_fence(unsigned deck) {
    assert(deck < NDECK);
    return empty_fence;
}
void azn_empty_commit(unsigned deck, uint64_t fence) {
    assert(deck < NDECK && fence == empty_fence);
    ++commits;
}
void azn_capability(unsigned deck, unsigned flags, int ready) {
    assert(deck < NDECK && flags == AZN_CAP_LTI);
    capability = ready;
}
int azn_offer(struct azstatus_native_receipt *r) {
    offered = *r;
    ++offers;
    return accept_offer;
}
void azn_emit(struct azstatus_native_receipt *r) {
    offered = *r;
    ++emits;
}
#include LTI_INC

static _Alignas(8) unsigned char object[0x80];
static void track(uint32_t id, const char *title, const char *artist) {
    uintptr_t vt = LTI_VT;
    memset(object, 0, sizeof object);
    memcpy(object, &vt, sizeof vt);
    memcpy(object + 0x10, &id, sizeof id);
    memcpy(object + LTI_TITLE_OFF, &title, sizeof title);
    memcpy(object + LTI_ARTIST_OFF, &artist, sizeof artist);
}
static long native_update(void *self, void *a1, void *a2, void *a3, void *a4, void *a5, void *a6, void *a7) {
    assert(self == object || self == NULL);
    assert(a1 == (void *) 1 && a2 == (void *) 2 && a3 == (void *) 3 && a4 == (void *) 4 && a5 == (void *) 5 && a6 == (void *) 6 && a7 == (void *) 7);
    if (native_change == 1) ++lifecycle;
    if (native_change == 2) atomic_fetch_add(&g_ls_gen[0], 1);
    return 37;
}
static void update(void *self) {
    assert(lti_update_hook(self, (void *) 1, (void *) 2, (void *) 3, (void *) 4, (void *) 5, (void *) 6, (void *) 7) == 37);
}
static void copied_metadata(void) {
    atomic_store(&g_ls_gen[0], 5);
    atomic_store(&g_lti_tok[0], (uintptr_t) object);
    track(42, "Track", "Artist");
    lti_capture(0, (uintptr_t) object);
    assert(offers == 1 && capability && offered.kind == AZN_METADATA && offered.deck == 0 && offered.epoch == lifecycle && offered.unit == 5);
    assert(offered.valid && !offered.a && offered.content_id == 42 && !strcmp(offered.title, "Track") && !strcmp(offered.artist, "Artist"));
    lti_capture(0, (uintptr_t) object);
    assert(offers == 1);
    track(42, "Edited", "Artist");
    lti_capture(0, (uintptr_t) object);
    assert(offers == 2 && !strcmp(offered.title, "Edited"));
    ++seed;
    lti_capture(0, (uintptr_t) object);
    assert(offers == 3);
    accept_offer = 0;
    track(43, "Next", "Artist");
    lti_capture(0, (uintptr_t) object);
    assert(offers == 4);
    accept_offer = 1;
    lti_capture(0, (uintptr_t) object);
    assert(offers == 5 && offered.content_id == 43);
    struct azstatus_native_receipt out = {.size = sizeof out, .version = 1};
    assert(azn_metadata_seed(0, &out) && out.valid && out.content_id == 43 && !strcmp(out.title, "Next"));
    atomic_fetch_add(&g_ls_gen[0], 1);
    assert(!azn_metadata_seed(0, &out));
    atomic_fetch_sub(&g_ls_gen[0], 1);
    ++lifecycle;
    assert(!azn_metadata_seed(0, &out));
    --lifecycle;
    atomic_fetch_add(&g_lti_snap[0].seq, 1);
    assert(!azn_metadata_seed(0, &out));
    atomic_fetch_add(&g_lti_snap[0].seq, 1);
    out.version = 0;
    assert(!azn_metadata_seed(0, &out));
    out.version = 1;
    out.size--;
    assert(!azn_metadata_seed(0, &out));
    out.size++;
    assert(!azn_metadata_seed(NDECK, &out) && !azn_metadata_seed(0, NULL));
}
static void native_ownership(void) {
    g_orig_lti_update = native_update;
    native_change = 1;
    unsigned before = offers;
    update(object);
    assert(offers == before && !atomic_load(&g_lti_snap[0].status_owner));
    native_change = 2;
    update(object);
    assert(offers == before && !atomic_load(&g_lti_snap[0].status_owner));
    native_change = 0;
    update(object);
    assert(offers == before + 1 && offered.unit == atomic_load(&g_ls_gen[0]));
    update(NULL);
    assert(atomic_load(&g_lti_orphans) == 1);
    atomic_store(&g_lti_tok[0], 0);
    before = offers;
    lti_capture(0, (uintptr_t) object);
    assert(offers == before);
    atomic_store(&g_lti_tok[0], (uintptr_t) object);
    g_status_lti_on = 0;
    lti_capture(0, (uintptr_t) object);
    assert(offers == before && !atomic_load(&g_lti_snap[0].status_owner));
    g_status_lti_on = 1;
}
static void empty_and_retired(void) {
    track(0, "Old", "Old Artist");
    lti_capture(0, (uintptr_t) object);
    assert(commits == 1 && offered.a && offered.valid && !offered.content_id && !offered.title[0] && !offered.artist[0]);
    uintptr_t bad_type = 0;
    memcpy(object, &bad_type, sizeof bad_type);
    lti_capture(0, (uintptr_t) object);
    assert(!offered.valid && !offered.content_id && !offered.title[0]);
    lti_invalidate(0);
    assert(emits == 1 && !offered.valid && !capability);
    g_status_lti_on = 0;
    lti_invalidate(0);
    assert(emits == 1);
}
int main(void) {
    copied_metadata();
    native_ownership();
    empty_and_retired();
    puts("PASS native LTI: copied metadata, dedup/retry, seed coherence, native load changes, empty commits and retirement");
    return 0;
}
