/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azcueprofile.h"
#include "../common/azcuerecall.h"
#include "../azmod/azmod-mod.h"
#include "sdk_cue_profiles.h"
#include <atomic>
#include <cstring>
#include <limits>
#include <initializer_list>
#include <mutex>
#if defined(__linux__)
#include <link.h>
#endif
namespace {
struct State {
    azcue_profile profile {};
    void *ctor = nullptr, *dtor = nullptr;
    std::atomic<bool> enabled {false};
    std::atomic<uint64_t> incarnation {0};
    std::atomic<void *> managers[4] {};
    std::mutex management;
    bool attempted = false;
};
State &state() {
    static State value;
    return value;
}
uintptr_t word(const void *p) {
    uintptr_t value;
    std::memcpy(&value, p, sizeof value);
    return value;
}
void ctor_hook(void *self, const uint64_t *settings, const uint64_t *player, const uint64_t *renew, void *config) {
    State &s = state();
    // Copy the locator scalars before stock construction can use temporary storage.
    uint64_t a = settings ? *settings : 0, b = player ? *player : 0, c = renew ? *renew : 0;
    reinterpret_cast<void (*)(void *, const uint64_t *, const uint64_t *, const uint64_t *, void *)>(s.ctor)(self, settings, player, renew, config);
    if (!s.enabled.load(std::memory_order_acquire) || !self || a != UINT64_C(0x009d3addf06bd1d7) ||
        (b & ~UINT64_C(255)) != UINT64_C(0xb9d7725fd31fc100) ||
        (c & ~UINT64_C(255)) != UINT64_C(0x3dc18c4946733700) || (b & 255) > 3 || (b & 255) != (c & 255) ||
        word(self) != s.profile.actor.manager_vptr || word(static_cast<char *>(self) + 0x110) != a ||
        word(static_cast<char *>(self) + 0x120) != b) return;
    uint64_t generation = s.incarnation.load(std::memory_order_relaxed);
    do {
        if (generation == std::numeric_limits<uint64_t>::max()) return;
    } while (!s.incarnation.compare_exchange_weak(generation, generation + 1, std::memory_order_relaxed));
    const unsigned deck = unsigned(b & 255);
    if (azsdk_cue_actor_register(deck, self, generation + 1) == AZ_OK)
        s.managers[deck].store(self, std::memory_order_release);
}
void dtor_hook(void *self) {
    State &s = state();
    for (unsigned deck = 0; deck < 4; ++deck) {
        void *expected = self;
        if (self && s.managers[deck].compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            azsdk_cue_actor_retire(deck, self);
            azsdk_cue_persist_retire(self);
            azsdk_cue_recall_retire(self);
        }
    }
    reinterpret_cast<void (*)(void *)>(s.dtor)(self);
}
__attribute__((unused)) bool bytes(const azcue_entry &e) { return e.address && !std::memcmp(reinterpret_cast<const void *>(e.address), e.prologue, 16); }
#if defined(__linux__) && defined(__aarch64__)
struct Main {
    uintptr_t bias = 0;
    bool found = false;
    const ElfW(Phdr) *headers = nullptr;
    unsigned count = 0;
    bool contains(uintptr_t address, size_t size, bool execute = false) const {
        if (address > UINTPTR_MAX - size) return false;
        for (unsigned i = 0; i < count; ++i) {
            const auto &p = headers[i];
            if (p.p_type != PT_LOAD || !(p.p_flags & PF_R) || (execute && !(p.p_flags & PF_X))) continue;
            uintptr_t begin = bias + p.p_vaddr;
            if (address >= begin && address + size >= address && address + size <= begin + p.p_memsz) return true;
        }
        return false;
    }
};
__attribute__((unused)) int main_image(dl_phdr_info *p, size_t, void *opaque) {
    if (p->dlpi_name && p->dlpi_name[0]) return 0;
    auto &m = *static_cast<Main *>(opaque);
    m.bias = p->dlpi_addr;
    m.headers = p->dlpi_phdr;
    m.count = p->dlpi_phnum;
    m.found = true;
    return 1;
}
#endif
} // namespace
extern "C" int azsdk_cue_profile_get(int build, uintptr_t bias, azcue_profile *out) noexcept {
    if (!out || build < 1 || build > 2) return 0;
    azcue_profile p = cue_profiles[build - 1];
    auto rebase = [bias](uintptr_t &a) { if (!a || a>UINTPTR_MAX-bias) return false; a+=bias;return true; };
    uintptr_t *actor[] = {&p.actor.manager_vptr, &p.actor.box_vptr, &p.actor.owner_vptr, &p.actor.task_vptr,
                          &p.actor.memory_producer, &p.actor.post, &p.actor.execute, &p.actor.complete_dtor};
    for (auto a : actor)
        if (!rebase(*a)) return 0;
    if (!rebase(p.ctor.address) || !rebase(p.complete_dtor.address) || !rebase(p.deleting_dtor.address)) return 0;
    for (auto &e : p.store)
        if (!rebase(e.address)) return 0;
    uintptr_t *persist[] = {&p.persist.save, &p.persist.erase, &p.persist.request, &p.persist.handler_request,
                            &p.persist.reception_reply, &p.persist.registered_task, &p.persist.handler_table, &p.persist.reception_table,
                            &p.persist.string_allocate, &p.persist.string_assign, &p.persist.string_destroy, &p.persist.color_construct,
                            &p.persist.memory_helper, &p.persist.content_copy, &p.persist.content_release,
                            &p.persist.vector_append, &p.persist.vector_destroy, &p.persist.content_from_vector, &p.persist.content_publish, &p.persist.hot_helper,
                            &p.persist.hot_content_from_vector, &p.persist.hot_content_publish, &p.persist.cue_copy, &p.persist.cue_destroy};
    for (auto a : persist)
        if (!rebase(*a)) return 0;
    for (auto &e : p.persist_hooks)
        if (!rebase(e.address)) return 0;
    for (auto &e : p.persist_metadata)
        if (!rebase(e.address)) return 0;
    for (auto &e : p.actor_entries)
        if (!rebase(e.address)) return 0;
    *out = p;
    return 1;
}
#ifdef AZSDK_CUE_LIFECYCLE_TESTING
static const azcue_profile *lifecycle_test_profile = nullptr;
#endif
extern "C" int azsdk_cue_main_profile(int build, azcue_profile *out) noexcept {
#ifdef AZSDK_CUE_LIFECYCLE_TESTING
    if (!out || build < 1 || build > 2 || !lifecycle_test_profile) return 0;
    *out = *lifecycle_test_profile;
    return 1;
#elif defined(__linux__) && defined(__aarch64__)
    if (!out || build < 1 || build > 2) return 0;
    // Validation precedes our own hooks. Preserve that verified immutable binding:
    // later actor/lifecycle interception legitimately changes these native bytes.
    static std::mutex validation_lock;
    static azcue_profile validated[2] {};
    static bool has_validation[2] {};
    std::lock_guard<std::mutex> guard(validation_lock);
    if (has_validation[build - 1]) {
        *out = validated[build - 1];
        return 1;
    }
    Main main;
    dl_iterate_phdr(main_image, &main);
    azcue_profile p {};
    if (!main.found || !azsdk_cue_profile_get(build, main.bias, &p)) return 0;
    auto valid = [&main](const azcue_entry &e) { return main.contains(e.address, 16, true) && bytes(e); };
    if (!valid(p.ctor) || !valid(p.complete_dtor) || !valid(p.deleting_dtor)) return 0;
    for (const auto &e : p.store)
        if (!valid(e)) return 0;
    for (const auto &e : p.persist_hooks)
        if (!valid(e)) return 0;
    for (const auto &e : p.persist_metadata)
        if (!valid(e)) return 0;
    for (const auto &e : p.actor_entries)
        if (!valid(e)) return 0;
    for (uintptr_t table : {p.actor.manager_vptr, p.actor.box_vptr, p.actor.owner_vptr, p.actor.task_vptr, p.persist.handler_table, p.persist.reception_table})
        if (!main.contains(table, 64)) return 0;
    if (word(reinterpret_cast<void *>(p.actor.manager_vptr)) != p.complete_dtor.address ||
        word(reinterpret_cast<void *>(p.actor.manager_vptr + 8)) != p.deleting_dtor.address ||
        word(reinterpret_cast<void *>(p.actor.manager_vptr + 16)) != p.actor.memory_producer ||
        word(reinterpret_cast<void *>(p.actor.box_vptr + 48)) != p.actor.post ||
        word(reinterpret_cast<void *>(p.actor.task_vptr)) != p.actor.complete_dtor ||
        word(reinterpret_cast<void *>(p.actor.task_vptr + 16)) != p.actor.execute) return 0;
    validated[build - 1] = p;
    has_validation[build - 1] = true;
    *out = p;
    return 1;
#else
    (void) build;
    (void) out;
    return 0;
#endif
}
extern "C" int azsdk_cue_lifecycle_configure(int build, azexit_install_fn install) noexcept {
    if (!install) return 0;
    State &s = state();
    try {
        std::lock_guard<std::mutex> lock(s.management);
        if (s.attempted) return 0;
        azcue_profile p {};
        if (!azsdk_cue_main_profile(build, &p)) return 0;
        s.attempted = true;
        s.profile = p;
        if (!install(p.ctor.address, reinterpret_cast<void *>(ctor_hook), &s.ctor) || !s.ctor ||
            !install(p.complete_dtor.address, reinterpret_cast<void *>(dtor_hook), &s.dtor) || !s.dtor) return 0;
        s.enabled.store(true, std::memory_order_release);
        return 1;
    } catch (...) { return 0; }
}
