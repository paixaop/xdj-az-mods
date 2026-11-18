/* SPDX-License-Identifier: GPL-3.0-only */
/* Isolated AArch64 fixture. Production has no synthetic source registration. */
#define AZ_STATUS_NATIVE_PRODUCER
#include "../../src/common/azanalysis_native.h"
#include "../../src/common/azloadobserver.h"
#include "../../src/common/azstatus_native.h"
#include "../../src/azmod/azmod-mod.h"
#include "native_abi_fixture.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
typedef struct fixture_owner Track;
typedef struct fixture_owner WeakTrack;
void azn_analysis_test_initialize(void *original);
int azn_analysis_test_wait(uintptr_t, const void *, int);
static int open_source(unsigned d, uint64_t e, uint64_t l, struct azn_analysis_source *out) {
    return azn_analysis_sources()->open(d, e, l, out);
}
static int current_source(uint64_t token) { return azn_analysis_sources()->current(token); }
static void close_source(uint64_t token) { azn_analysis_sources()->close(token); }
static int install_source(int build, azn_analysis_patch_fn patch) { return azn_analysis_sources()->install(build, patch); }
static int wait_decrypt(uintptr_t owner, const WeakTrack *weak, int generation) {
    return azn_analysis_test_wait(owner, weak, generation);
}
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
static bool loaded[4] = {true, true, false, false};
static uint64_t load_epoch = 7, load_generation = 8;
static unsigned char load_id[12] = {1, 0, 0, 0, 6, 0, 1, 0, 42, 0, 0, 0};
int azsdk_load_read(int deck, struct azsdk_load_sample *out) {
    if (deck < 0 || deck >= 4 || !loaded[deck]) return 0;
    *out = (struct azsdk_load_sample) {1, load_generation, load_epoch, 1, 1, 0};
    return 1;
}
int azn_load_track_matches(unsigned deck, uint64_t epoch, uint64_t load, const unsigned char id[12]) {
    return deck < 4 && loaded[deck] && epoch == load_epoch && load == load_generation && !memcmp(id, load_id, 12);
}
static Track fake_state;
void fixture_get_state(void);
static bool fail_state;
void fixture_get_state_body(void *out) {
    if (fail_state) fixture_raise();
    Track value = fixture_copy(fake_state);
    memcpy(out, &value, sizeof value);
}
static int native_result;
static int wait_result(uintptr_t owner, const WeakTrack *weak, int generation) {
    (void) owner;
    (void) weak;
    (void) generation;
    return native_result;
}
struct Native {
    unsigned char bytes[0x200];
};
#define put(n, off, v)                                   \
    do {                                                 \
        __auto_type value = (v);                         \
        memcpy((n).bytes + (off), &value, sizeof value); \
    } while (0)
static int released;
static void track_dispose(void *control) {
    (void) control;
    ++released;
}
FIXTURE_VTABLE(track_table, track_dispose);
FIXTURE_VTABLE(state_table, fixture_noop);
static struct azn_analysis_source opening(unsigned deck) {
    struct azn_analysis_source out = {0};
    out.size = sizeof out;
    out.version = 1;
    assert(open_source(deck, load_epoch, load_generation, &out) == AZ_OK);
    return out;
}
int main() {
    assert(install_source(1, NULL) == AZ_UNSUPPORTED);
    // Real AAPCS64 nontrivial x8 return exercised through the fixed profile entry.
    void *page = mmap((void *) 0x178d000, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page != (void *) 0x178d000) {
        if (page != MAP_FAILED) munmap(page, 4096);
        puts("SKIP: native profile address unavailable");
        return 77;
    }
    uint32_t jump[] = {0x58000050, 0xd61f0200};
    memcpy((void *) 0x178d760, jump, sizeof jump);
    uintptr_t target = (uintptr_t) fixture_get_state;
    memcpy((void *) 0x178d768, &target, 8);
    assert(mprotect(page, 4096, PROT_READ | PROT_EXEC) == 0);
    __builtin___clear_cache((char *) page, (char *) page + 4096);
    Track state = fixture_make(sizeof(struct Native), state_table);
    put(*(struct Native *) state.object, 0, (uintptr_t) (0x2c5b0b8));
    put(*(struct Native *) state.object, 0x10, 100);
    put(*(struct Native *) state.object, 0x14, 100);
    put(*(struct Native *) state.object, 0x18, (unsigned char) 1);
    fake_state = fixture_copy(state);
    char path[] = "/tmp/beatport-source-XXXXXX";
    int file = mkstemp(path);
    assert(file >= 0);
    assert(write(file, "audio", 5) == 5);
    close(file);
    Track track = fixture_make(sizeof(struct Native), track_table);
    put(*(struct Native *) track.object, 0, (uintptr_t) (0x2c5b028));
    put(*(struct Native *) track.object, 0x28, (uint64_t) (123));
    put(*(struct Native *) track.object, 0x78, (const char *) path);
    WeakTrack weak = fixture_weak(track);
    struct Native owner = {0};
    int generation = 11;
    put(owner, 0x28, (uintptr_t) &generation);
    put(owner, 0x160, (uint64_t) (123));
    memcpy(owner.bytes + 0x68, load_id, 12);
    azn_analysis_test_initialize((void *) wait_result);
    struct azn_analysis_source out = {0};
    out.size = sizeof out;
    out.version = 1;
    native_result = 1;
    assert(wait_decrypt((uintptr_t) &owner, &weak, generation) == 1);
    assert(open_source(0, 7, 8, &out) == AZ_NOT_FOUND);
    native_result = 0;
    fail_state = true;
    assert(wait_decrypt((uintptr_t) &owner, &weak, generation) == 0);
    assert(track.control->strong == 1 && fixture_exception_deletions == 1);
    fail_state = false;
    put(*(struct Native *) state.object, 0x18, (unsigned char) 0);
    wait_decrypt((uintptr_t) &owner, &weak, generation);
    assert(open_source(0, 7, 8, &out) == AZ_NOT_FOUND);
    put(*(struct Native *) state.object, 0x18, (unsigned char) 1);
    put(*(struct Native *) state.object, 0x14, 99);
    wait_decrypt((uintptr_t) &owner, &weak, generation);
    assert(open_source(0, 7, 8, &out) == AZ_NOT_FOUND);
    put(*(struct Native *) state.object, 0x14, 100);
    put(*(struct Native *) state.object, 0xc, 3);
    wait_decrypt((uintptr_t) &owner, &weak, generation);
    assert(open_source(0, 7, 8, &out) == AZ_NOT_FOUND);
    put(*(struct Native *) state.object, 0xc, 0);
    wait_decrypt((uintptr_t) &owner, &weak, generation - 1);
    assert(open_source(0, 7, 8, &out) == AZ_NOT_FOUND);
    wait_decrypt((uintptr_t) &owner, &weak, generation);
    load_id[9] ^= 1;
    assert(open_source(0, 7, 8, &out) == AZ_NOT_FOUND);
    load_id[9] ^= 1;
    struct azn_analysis_source first = opening(0);
    assert(!strcmp(first.audio_path, path));
    assert(!memcmp(first.track_id, load_id, 12));
    assert(current_source(first.token) == AZ_OK);
    struct azn_analysis_source other = opening(1);
    assert(current_source(other.token) == AZ_OK);
    close_source(other.token);
    put(*(struct Native *) state.object, 0xc, 5);
    assert(current_source(first.token) == AZ_STALE);
    put(*(struct Native *) state.object, 0xc, 0);
    ++load_epoch;
    assert(current_source(first.token) == AZ_STALE);
    --load_epoch;
    loaded[0] = false;
    assert(current_source(first.token) == AZ_STALE);
    loaded[0] = true;
    struct azn_analysis_source many[7];
    for (unsigned i = 0; i < 7; ++i) many[i] = opening(0);
    assert(open_source(0, 7, 8, &out) == AZ_LIMIT);
    for (unsigned i = 0; i < 7; ++i) close_source(many[i].token);
    fixture_drop(&track);
    assert(released == 0);
    assert(current_source(first.token) == AZ_OK);
    // Replacing the pathname cannot substitute different audio for this lease.
    char old[256];
    snprintf(old, sizeof old, "%s.old", path);
    assert(rename(path, old) == 0);
    file = open(path, O_CREAT | O_WRONLY | O_EXCL, 0600);
    assert(file >= 0);
    assert(write(file, "other", 5) == 5);
    close(file);
    assert(current_source(first.token) == AZ_STALE);
    close_source(first.token);
    assert(released == 1);
    assert(current_source(first.token) == AZ_STALE);
    close_source(first.token);
    assert(released == 1);
    assert(open_source(0, 7, 8, &out) == AZ_NOT_FOUND);
    unlink(path);
    unlink(old);
    munmap(page, 4096);
    fixture_weak_drop(&weak);
    fixture_drop(&state);
    fixture_drop(&fake_state);
    puts("Beatport native resource lease: readiness, fences, ownership, multi-deck, limits, replacement passed");
}
