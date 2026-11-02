/* SPDX-License-Identifier: GPL-3.0-only */
/* Private connection-operation bookkeeping fixture. No SQLite, database,
 * filename, key, native hook, EP147 execution or public capability is involved.
 * Profile 1/2 are independent caller contexts, not native profile acceptance.
 * The missing-header branch is a forwarding-only RED adapter: it records no
 * state and deliberately cannot manufacture a connection birth or lifetime.
 */
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#define REQUIRE(x, why) do { if (!(x)) { std::fprintf(stderr, \
    "FAIL connection-epoch: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
    std::abort(); } } while (0)

#ifndef AZCONNECTION_EPOCH_HEADER
#define AZCONNECTION_EPOCH_HEADER "../../src/common/azconnectionepoch.h"
#endif
#ifndef __has_include
#define __has_include(x) 0
#endif
#if __has_include(AZCONNECTION_EPOCH_HEADER)
#include AZCONNECTION_EPOCH_HEADER
#define EPOCH_IMPLEMENTATION_PRESENT 1
#else
#define EPOCH_IMPLEMENTATION_PRESENT 0
namespace azsdk_connection_epoch {
struct Receipt { std::uint64_t generation; };
template <std::size_t Capacity = 32,
          std::uint64_t MaxGeneration = UINT64_MAX,
          std::uint64_t MaxSequence = UINT64_MAX, unsigned MaxActive = 32>
class State {
public:
    template <class T, class F>
    int forward_open(T **, F &&original, Receipt * = nullptr) {
        return std::forward<F>(original)();
    }
    template <class F>
    int forward_close(const void *, F &&original) {
        return std::forward<F>(original)();
    }
    bool read(const void *, Receipt &) const { return false; }
};
}
#endif

#if !defined(__linux__) || !defined(__GLIBCXX__)
int main() { std::puts("SKIP: requires Linux and GNU libstdc++"); return 77; }
#else
namespace {
using azsdk_connection_epoch::Receipt;
using azsdk_connection_epoch::State;
unsigned profile;
struct Connection { unsigned fixture_id; };
struct Database {
    Connection *connection = nullptr;
    explicit Database(Connection *p) : connection(p) {}
};
struct SourceContext {
    std::shared_ptr<Database> database;
    std::weak_ptr<Database> weak;
    explicit SourceContext(Connection *p) : database(new Database(p)), weak(database) {}
};
template <class S>
Receipt known(S &s, const void *p) {
    Receipt receipt = {0};
    REQUIRE(s.read(p, receipt) && receipt.generation, "actual observed live connection receipt");
    return receipt;
}
template <class S>
void unknown(S &s, const void *p, const char *why) {
    Receipt out = {UINT64_C(0xabcdef)};
    REQUIRE(!s.read(p, out) && out.generation == UINT64_C(0xabcdef), why);
}
template <class S>
Receipt birth(S &s, Connection *p) {
    Connection *output = nullptr;
    Receipt receipt = {0};
    unsigned forwards = 0;
    int rc = s.forward_open(&output, [&]() { ++forwards; output = p; return 0; }, &receipt);
    REQUIRE(rc == 0 && forwards == 1 && output == p, "birth forwards actual result/output exactly once");
    REQUIRE(receipt.generation == known(s, p).generation, "native open observation publishes the read receipt");
    return receipt;
}

void four_owner_contexts() {
    State<> state;
    Connection pool[4] = {{1}, {2}, {3}, {4}};
    SourceContext contexts[4] = {SourceContext(&pool[0]), SourceContext(&pool[1]),
                                 SourceContext(&pool[2]), SourceContext(&pool[3])};
    Receipt receipts[4] = {};
    for (unsigned i = 0; i < 4; ++i) {
        auto &context = contexts[i];
        unknown(state, context.database->connection, "pre-observation connection is unknown");
        const long strong = context.database.use_count();
        unsigned forwards = 0;
        Connection *output = nullptr;
        int rc = state.forward_open(&output, [&]() {
            ++forwards; output = context.database->connection; errno = EINTR; return 0;
        }, &receipts[i]);
        REQUIRE(rc == 0 && forwards == 1 && output == &pool[i] && errno == EINTR,
                "unchanged native result, output and errno before the first observer assertion");
        std::printf("NATIVE_FORWARD_OK build=%u context=%u implementation=%d\n",
                    profile, i, EPOCH_IMPLEMENTATION_PRESENT);
        std::fflush(stdout);
        REQUIRE(receipts[i].generation && state.read(output, receipts[i]),
                "missing observed successful connection birth");
        REQUIRE(context.database.use_count() == strong && !context.weak.expired(),
                "connection receipt does not acquire or replace the genuine GNU owner");
        if (i) REQUIRE(receipts[i].generation > receipts[i - 1].generation,
                       "four independent source contexts receive nonreused generations");
    }
    for (unsigned i = 0; i < 4; ++i) {
        auto &context = contexts[i];
        const auto old = receipts[i];
        unsigned forwards = 0;
        const int native_result = i == 0 ? 5 : i == 1 ? 21 : i == 2 ? -7 : 0;
        int rc = state.forward_close(context.database->connection, [&]() {
            ++forwards;
            unknown(state, &pool[i], "generation retires BEFORE original close executes");
            errno = EBUSY; return native_result;
        });
        REQUIRE(rc == native_result && forwards == 1 && errno == EBUSY,
                "BUSY/error/zombie/success result and errno forward unchanged");
        unknown(state, &pool[i], "no close result revives its retired generation");
        auto fresh = birth(state, &pool[i]);
        REQUIRE(fresh.generation > old.generation && !context.weak.expired(),
                "same pointer and same live C++ owner require a fresh observed connection generation");
        for (unsigned j = i + 1; j < 4; ++j)
            REQUIRE(known(state, &pool[j]).generation == receipts[j].generation,
                    "retiring one source preserves independent connection receipts");
    }
}

void refused_opens_and_duplicates() {
    State<> state;
    Connection a = {1}, b = {2};
    for (int result : {1, 5, 7, -1}) {
        Connection *output = nullptr; Receipt receipt = {81}; unsigned forwards = 0;
        int rc = state.forward_open(&output, [&]() {
            ++forwards; output = &a; errno = EIO; return result;
        }, &receipt);
        REQUIRE(rc == result && forwards == 1 && output == &a && errno == EIO && receipt.generation == 81,
                "failed nonnull open is forwarded but cannot publish or alter a receipt");
        unknown(state, &a, "failed nonnull native open is ineligible");
    }
    Connection *output = nullptr; Receipt receipt = {82};
    REQUIRE(state.forward_open(&output, []() { return 0; }, &receipt) == 0 && receipt.generation == 82,
            "successful null output cannot publish a birth");
    REQUIRE(state.forward_open<Connection>(nullptr, []() { return 7; }, &receipt) == 7 && receipt.generation == 82,
            "null caller output is not dereferenced by bookkeeping");
    auto old = birth(state, &a);
    output = &b; receipt.generation = 83;
    state.forward_open(&output, [&]() { output = &a; return 0; }, &receipt);
    REQUIRE(receipt.generation == 83, "duplicate live pointer has no verified retirement and is refused");
    unknown(state, &a, "ambiguous duplicate live pointer retires the previous receipt");
    state.forward_close(&a, []() { return 0; });
    REQUIRE(birth(state, &a).generation > old.generation, "only a subsequent actual open can create a new incarnation");
}

void nesting_and_exceptions() {
    State<> state;
    Connection a = {1}, b = {2}, c = {3};
    birth(state, &a);
    Connection *output = nullptr; Receipt outer = {91};
    state.forward_open(&output, [&]() {
        unknown(state, &a, "scalar reads refuse while native lifecycle work is active");
        state.forward_close(&a, [&]() { unknown(state, &a, "nested close retires before forwarding"); return 5; });
        output = &b; return 0;
    }, &outer);
    REQUIRE(outer.generation == 91, "close during open invalidates its post-return admission");
    unknown(state, &a, "nested BUSY cannot revive the old receipt");
    unknown(state, &b, "open overlapping close is unqualified");
    output = nullptr; outer.generation = 92;
    state.forward_open(&output, [&]() {
        Connection *inner_output = nullptr; Receipt inner = {93};
        state.forward_open(&inner_output, [&]() { inner_output = &c; return 0; }, &inner);
        REQUIRE(inner.generation == 93, "nested open is not an independent observed birth");
        output = &b; return 0;
    }, &outer);
    REQUIRE(outer.generation == 92, "nested open invalidates the outer birth too");
    unknown(state, &b, "outer overlapping open remains unknown");
    unknown(state, &c, "inner overlapping open remains unknown");
    birth(state, &a);
    state.forward_close(&a, [&]() {
        Connection *inner_output = nullptr; Receipt inner = {94};
        state.forward_open(&inner_output, [&]() { inner_output = &b; return 0; }, &inner);
        REQUIRE(inner.generation == 94, "open during close is refused"); return 0;
    });
    unknown(state, &b, "closing scope cannot admit a nested open");
    Receipt before = birth(state, &a); unsigned forwards = 0;
    try {
        state.forward_close(&a, [&]() -> int { ++forwards; errno = ERANGE; throw std::runtime_error("native-close"); });
        REQUIRE(false, "native close exception propagates");
    } catch (const std::runtime_error &e) {
        REQUIRE(std::strcmp(e.what(), "native-close") == 0 && forwards == 1 && errno == ERANGE,
                "close exception identity/errno and single forwarding survive bookkeeping unwind");
    }
    unknown(state, &a, "exception cannot undo entry retirement");
    output = nullptr; outer.generation = 95; forwards = 0;
    try {
        state.forward_open(&output, [&]() -> int {
            ++forwards; output = &b; errno = EDOM; throw std::runtime_error("native-open");
        }, &outer);
        REQUIRE(false, "native open exception propagates");
    } catch (const std::runtime_error &e) {
        REQUIRE(std::strcmp(e.what(), "native-open") == 0 && forwards == 1 && errno == EDOM && outer.generation == 95,
                "throwing open never publishes and forwards its exception/errno");
    }
    unknown(state, &b, "written output before exception is not a completed birth");
    REQUIRE(birth(state, &a).generation > before.generation, "normal operation resumes after balanced exception unwind");
}

void quota_and_saturation() {
    Connection a = {1}, b = {2}, c = {3};
    State<2> quota;
    auto first = birth(quota, &a); auto second = birth(quota, &b);
    Connection *output = nullptr; Receipt refused = {101}; unsigned forwards = 0;
    quota.forward_open(&output, [&]() { ++forwards; output = &c; return 0; }, &refused);
    REQUIRE(forwards == 1 && refused.generation == 101 && known(quota, &a).generation == first.generation &&
                known(quota, &b).generation == second.generation, "quota cannot evict a live generation");
    unknown(quota, &c, "quota-refused native result remains unknown");
    quota.forward_close(&a, []() { return 0; });
    REQUIRE(birth(quota, &c).generation > second.generation, "retired slots can hold fresh generations without identity reuse");

    State<2, 2> generations;
    birth(generations, &a); generations.forward_close(&a, []() { return 0; });
    auto last = birth(generations, &a); REQUIRE(last.generation == 2, "last finite generation is usable");
    generations.forward_close(&a, []() { return 0; });
    output = nullptr; refused.generation = 102;
    generations.forward_open(&output, [&]() { output = &a; return 0; }, &refused);
    REQUIRE(refused.generation == 102, "generation exhaustion cannot wrap");
    unknown(generations, &a, "exhausted generation publication stays unavailable");

    State<2, UINT64_MAX, 2> sequences;
    birth(sequences, &a); sequences.forward_close(&a, []() { return 0; });
    output = nullptr; refused.generation = 103;
    sequences.forward_open(&output, [&]() { output = &a; return 0; }, &refused);
    REQUIRE(refused.generation == 103, "operation sequence exhaustion refuses admission");
    unknown(sequences, &a, "operation sequence never wraps into an older admission token");

    State<2, UINT64_MAX, UINT64_MAX, 1> depth;
    output = nullptr; refused.generation = 104;
    depth.forward_open(&output, [&]() {
        depth.forward_close(&a, []() { return 0; }); output = &a; return 0;
    }, &refused);
    REQUIRE(refused.generation == 104, "active-depth saturation invalidates admission");
    output = nullptr;
    depth.forward_open(&output, [&]() { output = &b; return 0; }, &refused);
    REQUIRE(refused.generation == 104, "depth saturation remains fail closed after unwind");
}

void concurrent_overlap_and_reads() {
    State<> state;
    Connection a = {1}, b = {2};
    auto old = birth(state, &a);
    std::atomic<unsigned> stage {0}; Receipt candidate = {111};
    std::thread opening([&]() {
        Connection *output = nullptr;
        state.forward_open(&output, [&]() {
            stage.store(1, std::memory_order_release);
            while (stage.load(std::memory_order_acquire) != 2) std::this_thread::yield();
            output = &b; return 0;
        }, &candidate);
    });
    while (stage.load(std::memory_order_acquire) != 1) std::this_thread::yield();
    state.forward_close(&a, [&]() {
        unknown(state, &a, "concurrent close retires before original");
        stage.store(2, std::memory_order_release); return 0;
    });
    opening.join();
    REQUIRE(candidate.generation == 111, "concurrent close invalidates an overlapping open");
    unknown(state, &a, "concurrent retirement stays retired");
    unknown(state, &b, "overlapping open is not admitted after close finishes");
    REQUIRE(birth(state, &a).generation > old.generation, "subsequent nonoverlapping open is fresh");

    stage.store(0, std::memory_order_release); candidate.generation = 112;
    std::thread closing([&]() {
        state.forward_close(&a, [&]() {
            stage.store(1, std::memory_order_release);
            while (stage.load(std::memory_order_acquire) != 2) std::this_thread::yield();
            return 0;
        });
    });
    while (stage.load(std::memory_order_acquire) != 1) std::this_thread::yield();
    Connection *output = nullptr;
    state.forward_open(&output, [&]() { output = &a; return 0; }, &candidate);
    stage.store(2, std::memory_order_release); closing.join();
    REQUIRE(candidate.generation == 112, "same-address reopen before old close returns is conservatively refused");
    unknown(state, &a, "old close completion cannot publish or revive a reused address");

    auto fresh = birth(state, &a);
    std::atomic<bool> done {false};
    std::thread reader([&]() {
        while (!done.load(std::memory_order_acquire)) {
            Receipt r = {0};
            if (state.read(&a, r)) REQUIRE(r.generation >= fresh.generation,
                    "concurrent scalar readers never observe an old or zero generation");
        }
    });
    for (unsigned i = 0; i < 200; ++i) {
        state.forward_close(&a, []() { return 0; });
        birth(state, &a);
    }
    done.store(true, std::memory_order_release); reader.join();
}
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (std::strcmp(argv[1], "1") == 0 || std::strcmp(argv[1], "2") == 0),
            "one independent process per configured build profile");
    profile = unsigned(argv[1][0] - '0');
    four_owner_contexts();
    refused_opens_and_duplicates();
    nesting_and_exceptions();
    quota_and_saturation();
    concurrent_overlap_and_reads();
    std::printf("PASS connection-epoch build=%u: four GNU owner contexts, native-operation forwarding, retirement, reuse and overlap\n", profile);
    return 0;
}
#endif
