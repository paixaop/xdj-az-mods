/* SPDX-License-Identifier: GPL-3.0-only */
/* Real GNU virtual dispatch with synthetic native classes; never loads EP147. */
#include "azloadobserver.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL load: %s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)
#if !defined(__linux__)
int main() { std::puts("SKIP: native load fixture requires Linux ELF main-image guards"); return 77; }
#else
template<class Predicate> static void until(Predicate predicate) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
}
namespace dj_player {
class LoadRequestID {
  public:
    explicit LoadRequestID(std::uint32_t value = 0) : value(value) {}
    virtual ~LoadRequestID() {}
    std::atomic<std::uint32_t> value;
};
struct TrackLoadOption { LoadRequestID request; explicit TrackLoadOption(std::uint32_t id) : request(id) {} };
class ITrackLoadable {
  public:
    class Listener {
      public:
        virtual ~Listener() {}
        virtual void onLoadTrackCompleted(const LoadRequestID &, std::uint32_t, const void *) = 0;
    };
    virtual ~ITrackLoadable() {}
    virtual void loadTrack(const void *, const TrackLoadOption &, const void *, const void *, Listener *) = 0;
};
class ITrackUnloadable {
  public:
    virtual ~ITrackUnloadable() {}
    virtual void unloadTrack(const TrackLoadOption &, void *) = 0;
};
class TrackLoadFacade : public ITrackLoadable, public ITrackUnloadable {
  public:
    std::atomic<unsigned> loads {0}, unloads {0};
    std::atomic<unsigned> automatic {0}, result {0};
    std::atomic<unsigned> expect_inline_sample {0};
    const void *track_expected = nullptr, *where_expected = nullptr, *speed_expected = nullptr;
    void loadTrack(const void *track, const TrackLoadOption &option, const void *where, const void *speed, Listener *listener) override {
        CHECK(track == track_expected && where == where_expected && speed == speed_expected);
        loads.fetch_add(1, std::memory_order_relaxed);
        if (automatic.load(std::memory_order_acquire) && listener) {
            LoadRequestID copy(option.request.value.load(std::memory_order_acquire));
            listener->onLoadTrackCompleted(copy, result.load(std::memory_order_relaxed), where);
            azsdk_load_sample sample;
            if (expect_inline_sample.load(std::memory_order_relaxed)) CHECK(azsdk_load_read(0, &sample));
        }
    }
    void unloadTrack(const TrackLoadOption &, void *listener) override {
        CHECK(listener == speed_expected);
        unloads.fetch_add(1, std::memory_order_relaxed);
    }
};
struct Bundle { TrackLoadFacade *facade; unsigned char remaining[0x70] = {}; explicit Bundle(TrackLoadFacade *value) : facade(value) {} };
class DjPlayerUnit {
  public:
    virtual ~DjPlayerUnit() {}
    unsigned char pad[0x188] = {};
    Bundle *bundle;
    explicit DjPlayerUnit(Bundle *bundle) : bundle(bundle) {}
};
}
namespace usecase { namespace load {
class TrackInfoStockerController { public: class Listener { public: virtual ~Listener() {} virtual void stock(const void *) {} }; };
class Prefix { public: virtual ~Prefix() {} unsigned char pad[0x118] = {}; };
struct Pad40 { unsigned char pad[0x28] = {}; };
class LoadController : public Prefix, public TrackInfoStockerController::Listener, public Pad40, public dj_player::ITrackLoadable::Listener {
  public:
    std::atomic<unsigned> callbacks {0};
    std::atomic<std::uint32_t> last_id {0}, last_result {0};
    const void *where_expected = nullptr;
    void onLoadTrackCompleted(const dj_player::LoadRequestID &request, std::uint32_t result, const void *where) override {
        CHECK(!where_expected || where == where_expected);
        last_id.store(request.value.load(std::memory_order_acquire), std::memory_order_relaxed);
        last_result.store(result, std::memory_order_relaxed);
        callbacks.fetch_add(1, std::memory_order_relaxed);
    }
};
} }
using dj_player::DjPlayerUnit;
using dj_player::TrackLoadFacade;
using dj_player::TrackLoadOption;
using usecase::load::LoadController;
static unsigned patch_calls, decline_at, partial_at, mismatch_at;
static void *mismatch_original;
static void other_load(void *self, const void *track, const void *option, const void *where, const void *speed, void *listener) {
    reinterpret_cast<void (*)(void *, const void *, const void *, const void *, const void *, void *)>(mismatch_original)(self, track, option, where, speed, listener);
}
static void other_unload(void *self, const void *option, void *listener) {
    reinterpret_cast<void (*)(void *, const void *, void *)>(mismatch_original)(self, option, listener);
}
static void other_complete(void *self, const void *id, std::uint32_t result, const void *where) {
    reinterpret_cast<void (*)(void *, const void *, std::uint32_t, const void *)>(mismatch_original)(self, id, result, where);
}
static int installer(std::uintptr_t slot, void *hook, void **original) {
    CHECK(original && __atomic_load_n(original, __ATOMIC_ACQUIRE));
    void *expected = __atomic_load_n(original, __ATOMIC_ACQUIRE);
    CHECK(__atomic_load_n(reinterpret_cast<void **>(slot), __ATOMIC_ACQUIRE) == expected);
    ++patch_calls;
    if (decline_at == patch_calls) return 0;
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0 && !(slot & 7u));
    std::uintptr_t page = slot & ~std::uintptr_t(page_size - 1);
    CHECK(mprotect(reinterpret_cast<void *>(page), static_cast<std::size_t>(page_size), PROT_READ | PROT_WRITE) == 0);
    if (mismatch_at == patch_calls) {
        mismatch_original = expected;
        void *other = mismatch_at == 1 ? reinterpret_cast<void *>(other_load) :
                      mismatch_at == 4 ? reinterpret_cast<void *>(other_complete) : reinterpret_cast<void *>(other_unload);
        __atomic_store_n(reinterpret_cast<void **>(slot), other, __ATOMIC_RELEASE);
        CHECK(__atomic_load_n(reinterpret_cast<void **>(slot), __ATOMIC_ACQUIRE) != expected);
        return 0; /* parent's guarded installer declines a competing slot change */
    }
    /* Fixture owns this process. Leave only its vtable data page writable. */
    __atomic_store_n(reinterpret_cast<void **>(slot), hook, __ATOMIC_RELEASE);
    return partial_at == patch_calls ? 0 : 1;
}
__attribute__((noinline)) static void load(dj_player::ITrackLoadable *facade, const TrackLoadOption &option,
                                          const void *track, const void *where, const void *speed,
                                          dj_player::ITrackLoadable::Listener *listener) {
    facade->loadTrack(track, option, where, speed, listener);
}
__attribute__((noinline)) static void unload(dj_player::ITrackUnloadable *facade, const TrackLoadOption &option, void *listener) {
    facade->unloadTrack(option, listener);
}
__attribute__((noinline)) static void primary_unload(TrackLoadFacade *facade, const TrackLoadOption &option, void *listener) {
    facade->unloadTrack(option, listener);
}
__attribute__((noinline)) static void emit(dj_player::ITrackLoadable::Listener *listener, std::uint32_t id, std::uint32_t result, const void *where) {
    dj_player::LoadRequestID request(id);
    listener->onLoadTrackCompleted(request, result, where);
}
struct Fixture {
    TrackLoadFacade facade;
    dj_player::Bundle bundle;
    DjPlayerUnit unit;
    LoadController controller;
    int track = 17, where = 31, speed = 47;
    Fixture() : bundle(&facade), unit(&bundle) {
        facade.track_expected = &track;
        facade.where_expected = &where;
        facade.speed_expected = &speed;
        controller.where_expected = &where;
        CHECK(reinterpret_cast<std::uintptr_t>(&unit.bundle) - reinterpret_cast<std::uintptr_t>(&unit) == 0x190);
        CHECK(reinterpret_cast<std::uintptr_t>(static_cast<dj_player::ITrackUnloadable *>(&facade)) - reinterpret_cast<std::uintptr_t>(&facade) == 8);
        CHECK(reinterpret_cast<std::uintptr_t>(static_cast<dj_player::ITrackLoadable::Listener *>(&controller)) - reinterpret_cast<std::uintptr_t>(&controller) == 0x150);
        CHECK(reinterpret_cast<std::uintptr_t>(static_cast<usecase::load::TrackInfoStockerController::Listener *>(&controller)) - reinterpret_cast<std::uintptr_t>(&controller) == 0x120);
    }
    void start(std::uint32_t id, dj_player::ITrackLoadable::Listener *listener = nullptr) {
        TrackLoadOption option(id);
        load(&facade, option, &track, &where, &speed, listener ? listener : &controller);
    }
    void finish(std::uint32_t id, std::uint32_t result = 0) { emit(&controller, id, result, &where); }
    void eject(std::uint32_t id, bool primary = false) {
        TrackLoadOption option(id);
        if (primary) primary_unload(&facade, option, &speed);
        else unload(&facade, option, &speed);
    }
};
static void unavailable(int deck) {
    azsdk_load_sample value, before;
    std::memset(&value, 0xa5, sizeof value);
    before = value;
    CHECK(!azsdk_load_read(deck, &value));
    CHECK(std::memcmp(&value, &before, sizeof value) == 0);
}
static azsdk_load_sample sample(int deck = 0) {
    azsdk_load_sample value;
    CHECK(azsdk_load_read(deck, &value));
    CHECK(value.unit_generation && value.load_generation && value.invalidation_epoch && value.observed_ns && !value.reserved);
    return value;
}
static std::uintptr_t word(std::uintptr_t p) { std::uintptr_t value; std::memcpy(&value, reinterpret_cast<void *>(p), sizeof value); return value; }
static void native_profile(int build) {
    Fixture f;
    auto primary = word(reinterpret_cast<std::uintptr_t>(&f.facade));
    auto secondary = word(reinterpret_cast<std::uintptr_t>(static_cast<dj_player::ITrackUnloadable *>(&f.facade)));
    auto completed = word(reinterpret_cast<std::uintptr_t>(static_cast<dj_player::ITrackLoadable::Listener *>(&f.controller)));
    azsdk_load_test_profile(build, reinterpret_cast<void *>(word(primary + 16)), reinterpret_cast<void *>(word(primary + 24)),
                           reinterpret_cast<void *>(word(secondary + 16)), reinterpret_cast<void *>(word(completed + 16)));
}
class ShortType { public: virtual ~ShortType() {} };
static const char short_name[] = "N";
static const std::uintptr_t fake_type[] = {0, reinterpret_cast<std::uintptr_t>(short_name)};
static std::uintptr_t fake_table[] = {0, reinterpret_cast<std::uintptr_t>(fake_type), 0, 0};
static void nominal_guards() {
    ShortType short_object;
    CHECK(!azsdk_load_register(0, &short_object)); /* no read at object+0x190 */
    std::uintptr_t fake_object = reinterpret_cast<std::uintptr_t>(fake_table + 2);
    CHECK(!azsdk_load_register(0, &fake_object)); /* name's first NUL is inside ASan's redzone boundary */
    CHECK(!azsdk_load_register(-1, &short_object));
    CHECK(!azsdk_load_register(4, &short_object));
    CHECK(!azsdk_load_register(0, nullptr));
}
static std::atomic<bool> publish_entered {false}, publish_release {false};
static void pause_publish(int deck) {
    CHECK(deck == 0);
    publish_entered.store(true, std::memory_order_release);
    until([] { return publish_release.load(std::memory_order_acquire); });
}
static void epoch_race(Fixture &f, unsigned id, bool retirement) {
    f.start(id);
    publish_entered.store(false, std::memory_order_release);
    publish_release.store(false, std::memory_order_release);
    azsdk_load_test_before_publish(pause_publish);
    std::thread old([&] { f.finish(id); });
    until([] { return publish_entered.load(std::memory_order_acquire); });
    if (retirement) azsdk_load_retire(0);
    else f.eject(id + 1);
    unavailable(0);
    publish_release.store(true, std::memory_order_release);
    old.join();
    azsdk_load_test_before_publish(nullptr);
    unavailable(0);
}
static void coherent_reads(Fixture &f) {
    f.start(100); f.finish(100);
    auto baseline = sample();
    std::atomic<bool> done {false};
    auto reader = [&] {
        while (!done.load(std::memory_order_acquire)) {
            azsdk_load_sample s;
            if (azsdk_load_read(0, &s)) {
                CHECK(s.unit_generation == baseline.unit_generation && s.native_request_id >= 100);
                CHECK(s.load_generation - baseline.load_generation == s.native_request_id - 100);
                CHECK(s.invalidation_epoch - baseline.invalidation_epoch == s.native_request_id - 100);
            }
        }
    };
    std::thread first(reader), second(reader), third(reader);
    for (unsigned id = 101; id <= 1100; ++id) { f.start(id); f.finish(id); }
    done.store(true, std::memory_order_release);
    first.join(); second.join(); third.join();
    CHECK(sample().native_request_id == 1100);
}
static void happy(int build) {
    azsdk_load_configure(1, build, installer);
    nominal_guards();
    Fixture f;
    CHECK(azsdk_load_register(0, &f.unit));
    CHECK(!azsdk_load_register(0, &f.unit));
    CHECK(!azsdk_load_register(1, &f.unit));
    unavailable(0);
    f.facade.automatic.store(1, std::memory_order_release);
    f.facade.expect_inline_sample.store(1, std::memory_order_release);
    f.start(0); /* native ID0 is a valid first value, and receipt precedes load return */
    auto first = sample();
    CHECK(first.native_request_id == 0 && f.facade.loads == 1 && f.controller.callbacks == 1);
    f.facade.automatic.store(0, std::memory_order_release);
    f.facade.expect_inline_sample.store(0, std::memory_order_release);
    f.start(1); unavailable(0);
    f.start(2); unavailable(0);
    f.finish(1); unavailable(0); /* late A cannot complete B */
    f.finish(2);
    auto second = sample();
    CHECK(second.native_request_id == 2 && second.load_generation > first.load_generation);
    f.finish(2); CHECK(sample().load_generation == second.load_generation); /* duplicate */
    f.finish(1); CHECK(sample().load_generation == second.load_generation); /* old receipt after B */
    f.start(3); f.finish(3, 1); unavailable(0);
    f.start(4); f.finish(4, 4); unavailable(0);
    f.start(5); f.finish(5); auto third = sample();
    CHECK(third.load_generation > second.load_generation); /* same opaque track arguments throughout */
    f.start(5); f.finish(5); unavailable(0); /* reused */
    f.start(4); f.finish(4); unavailable(0); /* backward */
    f.start(6); f.finish(6); sample();
    f.eject(7); unavailable(0); /* secondary interface clone */
    f.start(8); f.finish(8); sample();
    f.eject(9, true); unavailable(0); /* primary complete-object implementation */
    auto *wrong_interface = reinterpret_cast<dj_player::ITrackLoadable::Listener *>(static_cast<usecase::load::TrackInfoStockerController::Listener *>(&f.controller));
    f.start(10, wrong_interface); f.finish(10); unavailable(0);
    LoadController unrelated;
    unrelated.where_expected = &f.where;
    f.start(11); emit(&unrelated, 11, 0, &f.where); unavailable(0);
    f.finish(11); sample();
    azsdk_load_test_hold(0, 1);
    f.start(12); /* invalidates and records ID despite lost capture */
    unavailable(0);
    azsdk_load_test_hold(0, 0);
    f.finish(12); unavailable(0);
    f.start(12); f.finish(12); unavailable(0); /* contended IDs cannot later be reused */
    f.start(13);
    azsdk_load_test_hold(0, 1);
    f.finish(13); /* a lost current completion invalidates */
    azsdk_load_test_hold(0, 0);
    f.finish(13); unavailable(0);
    epoch_race(f, 14, false);
    epoch_race(f, 16, true);
    CHECK(azsdk_load_register(0, &f.unit)); /* same addresses, different registration */
    f.finish(16); unavailable(0);
    f.start(16); f.finish(16); unavailable(0); /* history survives retirement */
    f.start(17); f.finish(17); auto replacement = sample();
    CHECK(replacement.unit_generation > first.unit_generation && replacement.load_generation > third.load_generation);
    azsdk_load_configure(0, build, installer);
    unavailable(0);
    unsigned forwarded = f.facade.loads.load();
    f.start(18); f.finish(18);
    CHECK(f.facade.loads == forwarded + 1); unavailable(0); /* disabled hooks forward */
    azsdk_load_configure(1, build, installer);
    CHECK(azsdk_load_register(0, &f.unit));
    f.start(17); f.finish(17); unavailable(0); /* configuration never resets history */
    f.start(19); f.finish(19); sample();
    coherent_reads(f);
    Fixture other;
    CHECK(azsdk_load_register(1, &other.unit));
    other.start(2); other.finish(2); CHECK(sample(1).native_request_id == 2);
    f.start(UINT32_MAX); f.finish(UINT32_MAX); CHECK(sample().native_request_id == UINT32_MAX);
    f.start(0); f.finish(0); unavailable(0); /* wrapped */
    azsdk_load_retire(0);
    CHECK(azsdk_load_register(0, &f.unit));
    f.start(1); f.finish(1); unavailable(0); /* wrap quarantine survives retirement */
    azsdk_load_test_exhaust(1, 1);
    other.start(3); other.finish(3); unavailable(1);
    azsdk_load_retire(1);
    CHECK(!azsdk_load_register(1, &other.unit));
    Fixture sequence_exhausted;
    CHECK(azsdk_load_register(3, &sequence_exhausted.unit));
    azsdk_load_test_exhaust(3, 3);
    sequence_exhausted.start(0); sequence_exhausted.finish(0); unavailable(3);
    azsdk_load_retire(3);
    Fixture exhausted;
    CHECK(azsdk_load_register(2, &exhausted.unit));
    azsdk_load_test_exhaust(2, 2);
    exhausted.start(1); exhausted.finish(1); unavailable(2);
    azsdk_load_retire(0); azsdk_load_retire(2);
    azsdk_load_test_exhaust(0, 4);
    CHECK(!azsdk_load_register(0, &f.unit));
    azsdk_load_configure(1, 99, installer);
    CHECK(!azsdk_load_register(0, &f.unit));
    unavailable(0); unavailable(-1); unavailable(4);
    CHECK(!azsdk_load_read(0, nullptr));
}
static void declined(int build, const char *mode) {
    unsigned number = static_cast<unsigned>(mode[std::strlen(mode) - 1] - '0');
    if (std::strncmp(mode, "decline", 7) == 0) decline_at = number;
    else if (std::strncmp(mode, "partial", 7) == 0) partial_at = number;
    else mismatch_at = number;
    azsdk_load_configure(1, build, installer);
    Fixture f;
    int registered = azsdk_load_register(0, &f.unit);
    CHECK(registered == (number == 4));
    f.start(1); f.finish(1);
    CHECK(f.facade.loads == 1 && f.controller.callbacks == 1);
    unavailable(0);
    f.eject(2); f.eject(3, true);
    CHECK(f.facade.unloads == 2);
    unavailable(0);
    if (partial_at) {
        azsdk_load_retire(0);
        if (number < 4) CHECK(!azsdk_load_register(0, &f.unit));
        else CHECK(azsdk_load_register(0, &f.unit));
        f.start(4); f.finish(4); unavailable(0); /* unacknowledged partial remains forward-only */
    }
}
static void shared(int build, bool concurrent) {
    azsdk_load_configure(1, build, installer);
    Fixture first, second;
    first.controller.where_expected = nullptr;
    CHECK(azsdk_load_register(0, &first.unit) && azsdk_load_register(1, &second.unit));
    second.facade.automatic.store(1, std::memory_order_release);
    if (concurrent) {
        publish_entered.store(false, std::memory_order_release);
        publish_release.store(false, std::memory_order_release);
        azsdk_load_test_before_association(pause_publish);
        std::thread active([&] { first.start(1); });
        until([] { return publish_entered.load(std::memory_order_acquire); });
        second.start(1, &first.controller);
        publish_release.store(true, std::memory_order_release);
        active.join();
        azsdk_load_test_before_association(nullptr);
    } else {
        first.start(1);
        second.start(1, &first.controller); /* before-return callback has same ID */
    }
    unavailable(0); unavailable(1);
    first.finish(1); first.finish(2); unavailable(0);
    second.start(2, &first.controller); first.finish(2); unavailable(0); unavailable(1);
    CHECK(first.facade.loads == 1 && second.facade.loads == 2);
    azsdk_load_retire(0); azsdk_load_retire(1);
    CHECK(!azsdk_load_register(0, &first.unit) && !azsdk_load_register(1, &second.unit));
}
class OtherRequest { public: virtual ~OtherRequest() {} std::atomic<std::uint32_t> value {2}; };
static void unknown_attempt(int build, bool is_unload) {
    azsdk_load_configure(1, build, installer);
    Fixture f;
    CHECK(azsdk_load_register(0, &f.unit));
    f.start(1); f.finish(1); sample();
    OtherRequest unknown;
    const auto &option = *reinterpret_cast<const TrackLoadOption *>(&unknown);
    if (is_unload) unload(&f.facade, option, &f.speed);
    else load(&f.facade, option, &f.track, &f.where, &f.speed, &f.controller);
    unavailable(0);
    f.start(2); f.finish(2); unavailable(0);
    azsdk_load_retire(0);
    CHECK(!azsdk_load_register(0, &f.unit));
}
static std::atomic<bool> enabling {false}, disabling {false}, enable_release {false};
static void configuration_pause(int stage, int on) {
    if (stage == 1 && on) {
        enabling.store(true, std::memory_order_release);
        until([] { return enable_release.load(std::memory_order_acquire); });
    }
    if (stage == 0 && !on) disabling.store(true, std::memory_order_release);
}
static void configure_race(int build) {
    azsdk_load_configure(1, build, installer);
    Fixture f;
    azsdk_load_test_configure_hook(configuration_pause);
    std::thread enable([&] { azsdk_load_configure(1, build, installer); });
    until([] { return enabling.load(std::memory_order_acquire); });
    std::thread disable([&] { azsdk_load_configure(0, build, installer); });
    until([] { return disabling.load(std::memory_order_acquire); });
    enable_release.store(true, std::memory_order_release);
    enable.join(); disable.join();
    azsdk_load_test_configure_hook(nullptr);
    CHECK(!azsdk_load_register(0, &f.unit));
    unavailable(0);
}
static void wrong_function(int build, unsigned which) {
    azsdk_load_configure(1, build, installer);
    Fixture f;
    std::uintptr_t slot = 0;
    if (which <= 2) slot = word(reinterpret_cast<std::uintptr_t>(&f.facade)) + (which == 1 ? 16 : 24);
    else if (which == 3) slot = word(reinterpret_cast<std::uintptr_t>(static_cast<dj_player::ITrackUnloadable *>(&f.facade))) + 16;
    else slot = word(reinterpret_cast<std::uintptr_t>(static_cast<dj_player::ITrackLoadable::Listener *>(&f.controller))) + 16;
    auto expected = reinterpret_cast<void *>(word(slot));
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(mprotect(reinterpret_cast<void *>(slot & ~std::uintptr_t(page_size - 1)), page_size, PROT_READ | PROT_WRITE) == 0);
    __atomic_store_n(reinterpret_cast<void **>(slot), reinterpret_cast<void *>(other_load), __ATOMIC_RELEASE);
    int registered = azsdk_load_register(0, &f.unit);
    CHECK(registered == (which == 4));
    if (which == 4) { f.start(1); CHECK(patch_calls == 3); }
    __atomic_store_n(reinterpret_cast<void **>(slot), expected, __ATOMIC_RELEASE);
    if (which == 4) { f.finish(1); unavailable(0); }
}
static void capacity(int build) {
    azsdk_load_configure(1, build, installer);
    Fixture f;
    LoadController listeners[17];
    CHECK(azsdk_load_register(0, &f.unit));
    for (unsigned n = 0; n < 17; ++n) {
        listeners[n].where_expected = &f.where;
        f.start(n, &listeners[n]);
        emit(&listeners[n], n, 0, &f.where);
        if (n < 16) CHECK(sample().native_request_id == n);
        else unavailable(0);
    }
    azsdk_load_retire(0);
    CHECK(!azsdk_load_register(0, &f.unit));
}
#ifdef AZSDK_LOAD_PUBLIC_TEST
extern "C" void sdk_test_public_load(int deck, const azsdk_load_sample *expected);
extern "C" void sdk_test_public_load_optout(int deck);
static void public_load(int build) {
    azsdk_load_configure(1, build, installer);
    Fixture f[4];
    azsdk_load_sample current[4];
    for (int d = 0; d < 4; ++d) {
        CHECK(azsdk_load_register(d, &f[d].unit));
        sdk_test_public_load(d, nullptr);
        f[d].start(1);
        sdk_test_public_load(d, nullptr);
        f[d].finish(1);
        current[d] = sample(d);
        sdk_test_public_load(d, &current[d]);
    }
    for (int d = 0; d < 4; ++d) {
        auto previous = current[d];
        f[d].start(2); /* Same native track argument, new request. */
        sdk_test_public_load(d, nullptr);
        f[d].finish(1); /* Old completion cannot commit the reload. */
        sdk_test_public_load(d, nullptr);
        for (int other = 0; other < 4; ++other)
            if (other != d) sdk_test_public_load(other, &current[other]);
        f[d].finish(2);
        current[d] = sample(d);
        CHECK(current[d].load_generation > previous.load_generation &&
              current[d].unit_generation == previous.unit_generation &&
              current[d].invalidation_epoch > previous.invalidation_epoch);
        sdk_test_public_load(d, &current[d]);
        sdk_test_public_load_optout(d);
    }
    for (int d = 0; d < 4; ++d) {
        f[d].start(3); f[d].finish(3, 1);
        sdk_test_public_load(d, nullptr);
        f[d].start(4); f[d].finish(4);
        current[d] = sample(d); sdk_test_public_load(d, &current[d]);
        f[d].eject(5); sdk_test_public_load(d, nullptr);
        f[d].finish(4); sdk_test_public_load(d, nullptr);
        azsdk_load_retire(d); sdk_test_public_load(d, nullptr);
        CHECK(azsdk_load_register(d, &f[d].unit));
        f[d].start(6); f[d].finish(6);
        auto replacement = sample(d);
        CHECK(replacement.unit_generation > current[d].unit_generation &&
              replacement.load_generation > current[d].load_generation);
        sdk_test_public_load(d, &replacement);
        azsdk_load_retire(d); sdk_test_public_load(d, nullptr);
    }
}
#endif
int main(int argc, char **argv) {
    CHECK(argc == 3);
    int build = std::atoi(argv[1]);
    CHECK(build == 1 || build == 2);
    native_profile(build);
#ifdef AZSDK_LOAD_PUBLIC_TEST
    if (std::strcmp(argv[2], "public") == 0) public_load(build);
    else
#endif
    if (std::strcmp(argv[2], "happy") == 0) happy(build);
    else if (std::strcmp(argv[2], "shared") == 0) shared(build, false);
    else if (std::strcmp(argv[2], "shared-concurrent") == 0) shared(build, true);
    else if (std::strcmp(argv[2], "unknown") == 0) unknown_attempt(build, false);
    else if (std::strcmp(argv[2], "unknown-unload") == 0) unknown_attempt(build, true);
    else if (std::strcmp(argv[2], "configure") == 0) configure_race(build);
    else if (std::strcmp(argv[2], "capacity") == 0) capacity(build);
    else if (std::strncmp(argv[2], "wrong", 5) == 0) wrong_function(build, unsigned(argv[2][5] - '0'));
    else declined(build, argv[2]);
    std::printf("PASS load: build%d %s (synthetic native virtual dispatch)\n", build, argv[2]);
    return 0;
}
#endif
