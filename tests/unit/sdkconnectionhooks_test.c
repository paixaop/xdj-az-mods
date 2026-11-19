/* SPDX-License-Identifier: GPL-3.0-only */
/* Typed forwarding only. Synthetic installation mints no native module proof. */
#include "../../src/common/azconnectionepoch.h"
#include "native_abi_fixture.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x, why)                                                                              \
    do {                                                                                             \
        if (!(x)) {                                                                                  \
            fprintf(stderr, "FAIL connection-hooks: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
            abort();                                                                                 \
        }                                                                                            \
    } while (0)
#include "../../src/common/azconnectionhooks.h"
#define ADAPTER_PRESENT 1
struct sqlite3 {
    unsigned fixture_id;
};
struct NativeCall {
    const char *path, *vfs;
    struct sqlite3 **output, *connection;
    int flags, mode, result, entry_errno, exit_errno;
    unsigned forwards;
    bool raises;
    void (*inside)(struct NativeCall *);
    void *context;
};
#define NATIVE_CALL_INIT {.entry_errno = EDOM, .exit_errno = ERANGE}
static _Thread_local struct NativeCall *current;
struct CallScope {
    struct NativeCall *old;
};
static struct CallScope enter_call(struct NativeCall *c) {
    struct CallScope scope = {current};
    current = c;
    errno = c->entry_errno;
    return scope;
}
static void leave_call(struct CallScope *s) { current = s->old; }
static int native_open(const char *p, struct sqlite3 **o, int f, const char *v) {
    struct NativeCall *c = current;
    REQUIRE(p == c->path && o == c->output && f == c->flags && v == c->vfs, "exact native open four arguments and output destination");
    REQUIRE(errno == c->entry_errno, "native open sees unchanged entry errno");
    ++c->forwards;
    if (c->inside) c->inside(c);
    if (o) *o = c->connection;
    errno = c->exit_errno;
    if (c->raises) fixture_raise();
    return c->result;
}
static int native_close(struct sqlite3 *p, int m) {
    struct NativeCall *c = current;
    REQUIRE(p == c->connection && m == c->mode, "exact native close connection/mode and distinct original slot");
    REQUIRE(errno == c->entry_errno, "native close sees unchanged entry errno");
    ++c->forwards;
    if (c->inside) c->inside(c);
    errno = c->exit_errno;
    if (c->raises) fixture_raise();
    return c->result;
}
static void unknown(struct az_connection_state *s, const void *p, const char *why) {
    struct az_connection_receipt r = {901};
    REQUIRE(!az_connection_read(s, p, &r) && r.generation == 901, why);
}
static struct az_connection_receipt known(struct az_connection_state *s, const void *p) {
    struct az_connection_receipt r = {0};
    REQUIRE(az_connection_read(s, p, &r) && r.generation, "live observed adapter connection generation");
    return r;
}
struct FixtureInstaller {
    struct az_connection_state *epochs;
    uintptr_t base;
    az_native_sql_open open_target;
    az_native_sql_close close_target;
    void **slots[2];
    unsigned installs;
    int fail_kind, empty_kind;
    struct sqlite3 probe;
    bool diagnostics;
};
static int install_fixture(uintptr_t target, void *thunk, void **slot, void *context) {
    struct FixtureInstaller *self = context;
    unsigned kind = target == self->base + 0x647b0 ? 0 : target == self->base + 0x512c0 ? 1
                                                                                        : 2;
    REQUIRE(kind < 2 && thunk && slot, "exact reviewed open/nonnull-close target and original slot");
    REQUIRE((self->installs == 0 && kind == 1) || (self->installs == 1 && kind == 0), "close installs before open admission");
    ++self->installs;
    self->slots[kind] = slot;
    if (kind == (unsigned) self->fail_kind) return -1;
    if (kind == (unsigned) self->empty_kind) return 1;
    void *original = kind ? (void *) native_close : (void *) native_open;
    __atomic_store_n(slot, original, __ATOMIC_RELEASE);
    REQUIRE(__atomic_load_n(slot, __ATOMIC_ACQUIRE) == original, "original published before entry is reachable");
    if (kind) self->close_target = (az_native_sql_close) thunk;
    else self->open_target = (az_native_sql_open) thunk;
    REQUIRE(!az_connection_ready(), "tracking cannot arm during installer callback");
    struct NativeCall c = NATIVE_CALL_INIT;
    c.connection = &self->probe;
    c.mode = 1;
    struct sqlite3 *output = NULL;
    c.output = &output;
    struct CallScope scope __attribute__((cleanup(leave_call))) = enter_call(&c);
    int result = kind ? self->close_target(&self->probe, 1) : self->open_target(NULL, &output, 0, NULL);
    REQUIRE(result == 0 && c.forwards == 1 && errno == c.exit_errno, "entry during registration forwards original once with unchanged result/errno");
    if (!kind) REQUIRE(output == &self->probe, "registration-time native output preserved");
    unknown(self->epochs, &self->probe, "partial installation cannot publish a birth");
    return 1;
}
static int install_open(uintptr_t entry, az_native_sql_open replacement, az_native_sql_open *original, void *context) {
    return install_fixture(entry, (void *) replacement, (void **) original, context);
}
static int install_close(uintptr_t entry, az_native_sql_close replacement, az_native_sql_close *original, void *context) {
    return install_fixture(entry, (void *) replacement, (void **) original, context);
}
static struct az_connection_installer contract(struct FixtureInstaller *i) { return (struct az_connection_installer) {install_open, install_close, i}; }
struct Database {
    struct sqlite3 *connection;
};
struct OwnerContext {
    struct fixture_owner database, weak;
};
FIXTURE_VTABLE(database_vtable, fixture_noop);
struct Harness {
    struct az_connection_state epochs;
    struct FixtureInstaller installer;
};
static struct Harness harness_state = {.epochs = AZ_CONNECTION_STATE_INIT};
static unsigned profile;
static struct Harness *harness(void) {
    struct Harness *h = &harness_state;
    if (!h->installer.epochs) h->installer = (struct FixtureInstaller) {.epochs = &h->epochs, .base = UINT64_C(0x100000000) + profile * UINT64_C(0x10000000), .open_target = native_open, .close_target = native_close, .fail_kind = -1, .empty_kind = -1, .probe = {100}, .diagnostics = true};
    return h;
}
static int open_call(struct FixtureInstaller *i, struct NativeCall *c) {
    struct CallScope scope __attribute__((cleanup(leave_call))) = enter_call(c);
    int result = i->open_target(c->path, c->output, c->flags, c->vfs);
    REQUIRE(result == c->result && c->forwards == 1 && errno == c->exit_errno, "native open result/errno and single forwarding unchanged");
    return result;
}
static int close_call(struct FixtureInstaller *i, struct NativeCall *c) {
    struct CallScope scope __attribute__((cleanup(leave_call))) = enter_call(c);
    int result = i->close_target(c->connection, c->mode);
    REQUIRE(result == c->result && c->forwards == 1 && errno == c->exit_errno, "native close result/errno and single forwarding unchanged");
    return result;
}
static void retirement_visible(struct NativeCall *c) { unknown(c->context, c->connection, "retirement visible before native close work"); }
static void ordinary(void) {
    struct Harness *h = harness();
    REQUIRE(az_connection_test_install(&h->epochs, contract(&h->installer), h->installer.base) == 1 && h->installer.installs == 2 && h->installer.slots[0] != h->installer.slots[1], "two distinct original slots accept installer forwarding");
    struct OwnerContext owners[4];
    struct sqlite3 connections[4] = {{1}, {2}, {3}, {4}};
    struct az_connection_receipt receipts[4] = {0};
    const char paths[4] = {1, 2, 3, 4}, vfss[4] = {5, 6, 7, 8};
    for (unsigned n = 0; n < 4; ++n) {
        owners[n].database = fixture_make(sizeof(struct Database), database_vtable);
        owners[n].weak = fixture_weak(owners[n].database);
        struct NativeCall c = NATIVE_CALL_INIT;
        c.path = &paths[n];
        c.vfs = &vfss[n];
        c.flags = (int) (0x12340000 + n);
        c.output = &((struct Database *) owners[n].database.object)->connection;
        c.connection = &connections[n];
        int strong = owners[n].database.control->strong;
        open_call(&h->installer, &c);
        REQUIRE(*c.output == &connections[n], "actual native output destination filled");
        printf("NATIVE_FORWARD_OK build=%u context=%u adapter=%d\n", profile, n, ADAPTER_PRESENT);
        fflush(stdout);
        REQUIRE(az_connection_read(&h->epochs, c.connection, &receipts[n]) && receipts[n].generation, "missing adapter-owned successful connection birth");
        REQUIRE(owners[n].database.control->strong == strong && !fixture_expired(owners[n].weak), "adapter does not retain GNU native owner");
        if (n) REQUIRE(receipts[n].generation > receipts[n - 1].generation, "independent contexts get fresh incarnations");
    }
    REQUIRE(az_connection_ready(), "both installed originals arm tracking");
    h->installer.diagnostics = false;
    for (unsigned n = 0; n < 4; ++n) {
        struct NativeCall c = NATIVE_CALL_INIT;
        c.connection = &connections[n];
        c.mode = n & 1u;
        c.result = n == 0 ? 5 : n == 1 ? 21
                            : n == 2   ? -11
                                       : 0;
        c.inside = retirement_visible;
        c.context = &h->epochs;
        close_call(&h->installer, &c);
        unknown(&h->epochs, c.connection, "BUSY/error/zombie/success cannot revive with caller diagnostics disabled");
        struct NativeCall reopened = NATIVE_CALL_INIT;
        reopened.output = &((struct Database *) owners[n].database.object)->connection;
        reopened.connection = &connections[n];
        open_call(&h->installer, &reopened);
        REQUIRE(known(&h->epochs, &connections[n]).generation > receipts[n].generation, "same address/owner needs fresh observed adapter open");
    }
    struct az_connection_state other __attribute__((cleanup(az_connection_state_drop))) = AZ_CONNECTION_STATE_INIT;
    REQUIRE(az_connection_test_install(&other, contract(&h->installer), h->installer.base) != 1 && h->installer.installs == 2, "process-lifetime binding rejects replacement without original mutation");
    struct NativeCall c = NATIVE_CALL_INIT;
    c.connection = &connections[0];
    c.mode = 1;
    close_call(&h->installer, &c);
    unknown(&h->epochs, c.connection, "rejected rebind cannot move original State retirement");
    unknown(&other, c.connection, "replacement State stays untouched");
    for (unsigned n = 0; n < 4; ++n) {
        fixture_drop(&owners[n].database);
        fixture_weak_drop(&owners[n].weak);
    }
}
struct BirthCall {
    struct sqlite3 **output, *connection;
};
static int birth_call(void *opaque) {
    struct BirthCall *c = opaque;
    *c->output = c->connection;
    return 0;
}
static void partial(bool fail_close, bool empty) {
    struct Harness *h = harness();
    int kind = fail_close ? 1 : 0;
    if (empty) h->installer.empty_kind = kind;
    else h->installer.fail_kind = kind;
    REQUIRE(az_connection_test_install(&h->epochs, contract(&h->installer), h->installer.base) != 1 && !az_connection_ready(), "partial/malformed install fails closed");
    REQUIRE(h->installer.installs == (fail_close ? 1u : 2u), "failed close does not enable an open hook");
    struct sqlite3 token = {1}, *output = NULL;
    struct NativeCall c = NATIVE_CALL_INIT;
    c.output = &output;
    c.connection = &token;
    open_call(&h->installer, &c);
    unknown(&h->epochs, &token, "partial install cannot admit births");
    if (!fail_close) {
        struct BirthCall birth = {&output, &token};
        az_connection_forward_open(&h->epochs, &output, birth_call, &birth, NULL);
        known(&h->epochs, &token);
        h->installer.diagnostics = false;
        struct NativeCall closing = NATIVE_CALL_INIT;
        closing.connection = &token;
        closing.mode = 1;
        closing.result = 5;
        close_call(&h->installer, &closing);
        unknown(&h->epochs, &token, "installed close observes lifetime after open-install failure");
    }
}
struct NestedClose {
    struct FixtureInstaller *installer;
    struct sqlite3 *connection;
};
static void nested_close(struct NativeCall *c) {
    struct NestedClose *n = c->context;
    struct NativeCall close = NATIVE_CALL_INIT;
    close.connection = n->connection;
    close.result = 5;
    close_call(n->installer, &close);
}
struct UnwindCall {
    struct FixtureInstaller *installer;
    struct NativeCall *call;
    bool closing;
};
static void unwind_call(void *opaque) {
    struct UnwindCall *c = opaque;
    if (c->closing) close_call(c->installer, c->call);
    else open_call(c->installer, c->call);
    REQUIRE(false, "native failure propagates");
}
static void failures(void) {
    struct Harness *h = harness();
    REQUIRE(az_connection_test_install(&h->epochs, contract(&h->installer), h->installer.base) == 1 && az_connection_ready(), "complete installation");
    struct sqlite3 a = {1}, b = {2}, *output = NULL;
    const int results[] = {1, 5, 7, -9};
    for (unsigned i = 0; i < 4; ++i) {
        struct NativeCall c = NATIVE_CALL_INIT;
        c.output = &output;
        c.connection = &a;
        c.result = results[i];
        open_call(&h->installer, &c);
        REQUIRE(output == &a, "failed native open output preserved");
        unknown(&h->epochs, &a, "failed nonnull adapter open refused");
    }
    struct NativeCall first = NATIVE_CALL_INIT;
    first.output = &output;
    first.connection = &a;
    open_call(&h->installer, &first);
    struct az_connection_receipt old = known(&h->epochs, &a);
    struct NativeCall outer = NATIVE_CALL_INIT;
    outer.output = &output;
    outer.connection = &b;
    struct NestedClose nested = {&h->installer, &a};
    outer.inside = nested_close;
    outer.context = &nested;
    open_call(&h->installer, &outer);
    unknown(&h->epochs, &a, "nested native close retires old birth");
    unknown(&h->epochs, &b, "nested close invalidates outer adapter admission");
    struct NativeCall fresh = NATIVE_CALL_INIT;
    fresh.output = &output;
    fresh.connection = &a;
    open_call(&h->installer, &fresh);
    REQUIRE(known(&h->epochs, &a).generation > old.generation, "subsequent actual open is fresh");
    struct NativeCall tc = NATIVE_CALL_INIT;
    tc.connection = &a;
    tc.mode = 1;
    tc.raises = true;
    unsigned deleted = fixture_exception_deletions;
    struct UnwindCall call = {&h->installer, &tc, true};
    REQUIRE(!fixture_native_try(unwind_call, &call) && tc.forwards == 1 && errno == tc.exit_errno && fixture_exception_deletions == deleted + 1 && !current, "same close unwind, errno, forwarding count and TLS restoration");
    unknown(&h->epochs, &a, "exception cannot undo close retirement");
    struct NativeCall to = NATIVE_CALL_INIT;
    to.output = &output;
    to.connection = &b;
    to.raises = true;
    call = (struct UnwindCall) {&h->installer, &to, false};
    REQUIRE(!fixture_native_try(unwind_call, &call) && to.forwards == 1 && errno == to.exit_errno && fixture_exception_deletions == deleted + 2 && !current, "same open unwind, errno, forwarding count and TLS restoration");
    REQUIRE(output == &b, "output written before exception preserved");
    unknown(&h->epochs, &b, "unwinding open cannot publish a birth");
}
int main(int argc, char **argv) {
    REQUIRE(argc == 3 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "separate process per profile and immutable binding scenario");
    profile = (unsigned) (argv[1][0] - '0');
    const char *scenario = argv[2];
    if (!strcmp(scenario, "ordinary")) ordinary();
    else if (!strcmp(scenario, "close-fails")) partial(true, false);
    else if (!strcmp(scenario, "open-fails")) partial(false, false);
    else if (!strcmp(scenario, "empty-close")) partial(true, true);
    else if (!strcmp(scenario, "empty-open")) partial(false, true);
    else if (!strcmp(scenario, "failures")) failures();
    else REQUIRE(false, "known scenario");
    printf("PASS connection-hooks build=%u scenario=%s: synthetic native forwarding/epochs; module proof NOT exercised\n", profile, scenario);
    return 0;
}
