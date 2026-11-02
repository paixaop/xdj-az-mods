/* SPDX-License-Identifier: GPL-3.0-only */
/* Forwarding-adapter TDD. No SQLite/EP147, DB, filename or key operation runs.
 * TEST-ONLY forwarding setup isolates native dispatch from module validation;
 * it cannot establish a production module proof or native hook acceptance. */
#include "../../src/common/azconnectionepoch.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#define REQUIRE(x, why) do { if (!(x)) { std::fprintf(stderr, \
 "FAIL connection-hooks: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); std::abort(); } } while (0)
#ifndef __has_include
#define __has_include(x) 0
#endif
#if __has_include("../../src/common/azconnectionhooks.h")
#include "../../src/common/azconnectionhooks.h"
#define ADAPTER_PRESENT 1
#else
#define ADAPTER_PRESENT 0
struct sqlite3;
namespace azsdk_connection_hooks {
using EpochState = azsdk_connection_epoch::State<32>;
using NativeOpen = int (*)(const char *, sqlite3 **, int, const char *);
using NativeClose = int (*)(sqlite3 *, int);
struct Installer { int (*call)(std::uintptr_t, void *, void **, void *); void *context; };
namespace testing {
/* Absent-adapter baseline: publish/forward originals only. No registry/model. */
static void *originals[2] = {};
static int open(const char *p, sqlite3 **o, int f, const char *v) {
 return reinterpret_cast<NativeOpen>(__atomic_load_n(&originals[0], __ATOMIC_ACQUIRE))(p, o, f, v);
}
static int close(sqlite3 *p, int m) {
 return reinterpret_cast<NativeClose>(__atomic_load_n(&originals[1], __ATOMIC_ACQUIRE))(p, m);
}
static int install_forwarding(EpochState &, Installer i, std::uintptr_t b) {
 if (i.call(b + 0x512c0, reinterpret_cast<void *>(&close), &originals[1], i.context) <= 0) return 0;
 return i.call(b + 0x647b0, reinterpret_cast<void *>(&open), &originals[0], i.context) > 0 ? 1 : 0;
}
static bool ready() { return false; }
}
}
#endif
#if !defined(__linux__) || !defined(__GLIBCXX__)
int main() { std::puts("SKIP: requires Linux and GNU libstdc++"); return 77; }
#else
struct sqlite3 { unsigned fixture_id; }; // opaque fixture token, never a native-layout claim
namespace {
using namespace azsdk_connection_hooks;
using azsdk_connection_epoch::Receipt;
unsigned profile;
struct NativeCall {
 const char *path = nullptr, *vfs = nullptr;
 sqlite3 **output = nullptr, *connection = nullptr;
 int flags = 0, mode = 0, result = 0, entry_errno = EDOM, exit_errno = ERANGE;
 unsigned forwards = 0; bool throws = false;
 std::function<void()> inside;
};
thread_local NativeCall *current;
struct CallScope {
 NativeCall *old;
 explicit CallScope(NativeCall &c) : old(current) { current = &c; errno = c.entry_errno; }
 ~CallScope() { current = old; }
};
int native_open(const char *p, sqlite3 **o, int f, const char *v) {
 auto &c = *current;
 REQUIRE(p == c.path && o == c.output && f == c.flags && v == c.vfs, "exact native open four arguments and output destination");
 REQUIRE(errno == c.entry_errno, "native open sees unchanged entry errno");
 ++c.forwards; if (c.inside) c.inside(); if (o) *o = c.connection; errno = c.exit_errno;
 if (c.throws) throw std::runtime_error("native-open-exception");
 return c.result;
}
int native_close(sqlite3 *p, int m) {
 auto &c = *current;
 REQUIRE(p == c.connection && m == c.mode, "exact native close connection/mode and distinct original slot");
 REQUIRE(errno == c.entry_errno, "native close sees unchanged entry errno");
 ++c.forwards; if (c.inside) c.inside(); errno = c.exit_errno;
 if (c.throws) throw std::runtime_error("native-close-exception");
 return c.result;
}
void unknown(EpochState &s, const void *p, const char *why) {
 Receipt r = {901}; REQUIRE(!s.read(p, r) && r.generation == 901, why);
}
Receipt known(EpochState &s, const void *p) {
 Receipt r = {0}; REQUIRE(s.read(p, r) && r.generation, "live observed adapter connection generation"); return r;
}
struct FixtureInstaller {
 EpochState &epochs; std::uintptr_t base;
 NativeOpen open_target = &native_open; NativeClose close_target = &native_close;
 void **slots[2] = {}; unsigned installs = 0; int fail_kind = -1, empty_kind = -1;
 sqlite3 probe = {100}; bool diagnostics = true; // caller flag unavailable to adapter
 FixtureInstaller(EpochState &s, std::uintptr_t b) : epochs(s), base(b) {}
 static int install(std::uintptr_t target, void *thunk, void **slot, void *context) {
  auto &self = *static_cast<FixtureInstaller *>(context);
  unsigned kind = target == self.base + 0x647b0 ? 0 : target == self.base + 0x512c0 ? 1 : 2;
  REQUIRE(kind < 2 && thunk && slot, "exact reviewed open/nonnull-close target and original slot");
  REQUIRE((self.installs == 0 && kind == 1) || (self.installs == 1 && kind == 0), "close installs before open admission");
  ++self.installs; self.slots[kind] = slot;
  if (kind == unsigned(self.fail_kind)) return -1;
  if (kind == unsigned(self.empty_kind)) return 1; // malformed success, native entry stays unchanged
  void *original = kind ? reinterpret_cast<void *>(&native_close) : reinterpret_cast<void *>(&native_open);
  __atomic_store_n(slot, original, __ATOMIC_RELEASE);
  REQUIRE(__atomic_load_n(slot, __ATOMIC_ACQUIRE) == original, "original published before entry is reachable");
  if (kind) self.close_target = reinterpret_cast<NativeClose>(thunk); else self.open_target = reinterpret_cast<NativeOpen>(thunk);
  REQUIRE(!testing::ready(), "tracking cannot arm during installer callback");
  NativeCall c; c.connection = &self.probe; c.mode = 1; sqlite3 *output = nullptr; c.output = &output;
  CallScope scope(c);
  int result = kind ? self.close_target(&self.probe, 1) : self.open_target(nullptr, &output, 0, nullptr);
  REQUIRE(result == 0 && c.forwards == 1 && errno == c.exit_errno, "entry during registration forwards original once with unchanged result/errno");
  if (!kind) REQUIRE(output == &self.probe, "registration-time native output preserved");
  unknown(self.epochs, &self.probe, "partial installation cannot publish a birth"); return 1;
 }
 Installer contract() { return Installer {install, this}; }
};
struct Database { sqlite3 *connection = nullptr; };
struct OwnerContext { std::shared_ptr<Database> database {new Database}; std::weak_ptr<Database> weak {database}; };
struct Harness {
 EpochState epochs; FixtureInstaller installer;
 explicit Harness(std::uintptr_t b) : installer(epochs, b) {}
};
Harness &harness() {
 // One process-lifetime binding per scenario; no reset/unbind or dangling trampoline owner.
 static Harness *h = new Harness(UINT64_C(0x100000000) + profile * UINT64_C(0x10000000)); return *h;
}
int open(FixtureInstaller &i, NativeCall &c) {
 CallScope scope(c); int result = i.open_target(c.path, c.output, c.flags, c.vfs);
 REQUIRE(result == c.result && c.forwards == 1 && errno == c.exit_errno, "native open result/errno and single forwarding unchanged"); return result;
}
int close(FixtureInstaller &i, NativeCall &c) {
 CallScope scope(c); int result = i.close_target(c.connection, c.mode);
 REQUIRE(result == c.result && c.forwards == 1 && errno == c.exit_errno, "native close result/errno and single forwarding unchanged"); return result;
}
void ordinary() {
 auto &h = harness();
 REQUIRE(testing::install_forwarding(h.epochs, h.installer.contract(), h.installer.base) == 1 &&
         h.installer.installs == 2 && h.installer.slots[0] != h.installer.slots[1], "two distinct original slots accept installer forwarding");
 OwnerContext owners[4]; sqlite3 connections[4] = {{1}, {2}, {3}, {4}}; Receipt receipts[4] = {};
 const char paths[4] = {1, 2, 3, 4}, vfss[4] = {5, 6, 7, 8};
 for (unsigned n = 0; n < 4; ++n) {
  NativeCall c; c.path = &paths[n]; c.vfs = &vfss[n]; c.flags = int(0x12340000 + n);
  c.output = &owners[n].database->connection; c.connection = &connections[n]; long strong = owners[n].database.use_count();
  open(h.installer, c); REQUIRE(*c.output == &connections[n], "actual native output destination filled");
  std::printf("NATIVE_FORWARD_OK build=%u context=%u adapter=%d\n", profile, n, ADAPTER_PRESENT); std::fflush(stdout);
  REQUIRE(h.epochs.read(c.connection, receipts[n]) && receipts[n].generation, "missing adapter-owned successful connection birth");
  REQUIRE(owners[n].database.use_count() == strong && !owners[n].weak.expired(), "adapter does not retain GNU native owner");
  if (n) REQUIRE(receipts[n].generation > receipts[n-1].generation, "independent contexts get fresh incarnations");
 }
 REQUIRE(testing::ready(), "both installed originals arm tracking"); h.installer.diagnostics = false;
 for (unsigned n = 0; n < 4; ++n) {
  NativeCall c; c.connection = &connections[n]; c.mode = n & 1u; c.result = n == 0 ? 5 : n == 1 ? 21 : n == 2 ? -11 : 0;
  c.inside = [&]() { unknown(h.epochs, c.connection, "retirement visible before native close work"); };
  close(h.installer, c); unknown(h.epochs, c.connection, "BUSY/error/zombie/success cannot revive with caller diagnostics disabled");
  NativeCall reopened; reopened.output = &owners[n].database->connection; reopened.connection = &connections[n]; open(h.installer, reopened);
  REQUIRE(known(h.epochs, &connections[n]).generation > receipts[n].generation, "same address/owner needs fresh observed adapter open");
 }
 EpochState other;
 REQUIRE(testing::install_forwarding(other, h.installer.contract(), h.installer.base) != 1 && h.installer.installs == 2, "process-lifetime binding rejects replacement without original mutation");
 NativeCall c; c.connection = &connections[0]; c.mode = 1; close(h.installer, c);
 unknown(h.epochs, c.connection, "rejected rebind cannot move original State retirement"); unknown(other, c.connection, "replacement State stays untouched");
}
void partial(bool fail_close, bool empty) {
 auto &h = harness(); int kind = fail_close ? 1 : 0;
 if (empty) h.installer.empty_kind = kind; else h.installer.fail_kind = kind;
 REQUIRE(testing::install_forwarding(h.epochs, h.installer.contract(), h.installer.base) != 1 && !testing::ready(), "partial/malformed install fails closed");
 REQUIRE(h.installer.installs == (fail_close ? 1u : 2u), "failed close does not enable an open hook");
 sqlite3 token = {1}; sqlite3 *output = nullptr; NativeCall c; c.output = &output; c.connection = &token;
 open(h.installer, c); unknown(h.epochs, &token, "partial install cannot admit births");
 if (!fail_close) {
  h.epochs.forward_open(&output, [&]() { output = &token; return 0; }); known(h.epochs, &token);
  h.installer.diagnostics = false; NativeCall closing; closing.connection = &token; closing.mode = 1; closing.result = 5;
  close(h.installer, closing); unknown(h.epochs, &token, "installed close observes lifetime after open-install failure");
 }
}
void failures() {
 auto &h = harness(); REQUIRE(testing::install_forwarding(h.epochs, h.installer.contract(), h.installer.base) == 1 && testing::ready(), "complete installation");
 sqlite3 a = {1}, b = {2}; sqlite3 *output = nullptr;
 for (int result : {1, 5, 7, -9}) {
  NativeCall c; c.output = &output; c.connection = &a; c.result = result; open(h.installer, c);
  REQUIRE(output == &a, "failed native open output preserved"); unknown(h.epochs, &a, "failed nonnull adapter open refused");
 }
 NativeCall first; first.output = &output; first.connection = &a; open(h.installer, first); auto old = known(h.epochs, &a);
 NativeCall outer; outer.output = &output; outer.connection = &b;
 outer.inside = [&]() { NativeCall c; c.connection = &a; c.mode = 0; c.result = 5; close(h.installer, c); };
 open(h.installer, outer); unknown(h.epochs, &a, "nested native close retires old birth"); unknown(h.epochs, &b, "nested close invalidates outer adapter admission");
 NativeCall fresh; fresh.output = &output; fresh.connection = &a; open(h.installer, fresh);
 REQUIRE(known(h.epochs, &a).generation > old.generation, "subsequent actual open is fresh");
 NativeCall tc; tc.connection = &a; tc.mode = 1; tc.throws = true;
 try { close(h.installer, tc); REQUIRE(false, "native close exception propagates"); }
 catch (const std::runtime_error &e) { REQUIRE(!std::strcmp(e.what(), "native-close-exception") && tc.forwards == 1 && errno == tc.exit_errno, "same close exception/errno/forwarding count"); }
 unknown(h.epochs, &a, "exception cannot undo close retirement");
 NativeCall to; to.output = &output; to.connection = &b; to.throws = true;
 try { open(h.installer, to); REQUIRE(false, "native open exception propagates"); }
 catch (const std::runtime_error &e) { REQUIRE(!std::strcmp(e.what(), "native-open-exception") && to.forwards == 1 && errno == to.exit_errno, "same open exception/errno/forwarding count"); }
 REQUIRE(output == &b, "output written before exception preserved"); unknown(h.epochs, &b, "throwing open cannot publish a birth");
}
}
int main(int argc, char **argv) {
 REQUIRE(argc == 3 && (!std::strcmp(argv[1], "1") || !std::strcmp(argv[1], "2")), "separate process per profile and immutable binding scenario");
 profile = unsigned(argv[1][0] - '0'); std::string scenario = argv[2];
 if (scenario == "ordinary") ordinary(); else if (scenario == "close-fails") partial(true, false);
 else if (scenario == "open-fails") partial(false, false); else if (scenario == "empty-close") partial(true, true);
 else if (scenario == "empty-open") partial(false, true); else if (scenario == "failures") failures(); else REQUIRE(false, "known scenario");
 std::printf("PASS connection-hooks build=%u scenario=%s: synthetic native forwarding/epochs; module proof NOT exercised\n", profile, scenario.c_str()); return 0;
}
#endif
