/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/azconnectionhooks.h"
#include <stdatomic.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#if defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#endif




struct az_sql_profile {
    uintptr_t base_;
    struct az_sql_range ranges_[32];
    unsigned count_;
    az_sql_file_control control_;
    void *handle_;
};
uintptr_t az_sql_profile_base(const struct az_sql_profile *p) { return p->base_; }
unsigned az_sql_profile_range_count(const struct az_sql_profile *p) { return p->count_; }
const struct az_sql_range *az_sql_profile_range(const struct az_sql_profile *p, unsigned i) { return i < p->count_ ? &p->ranges_[i] : NULL; }
bool az_sql_profile_contains(const struct az_sql_profile *p, uintptr_t address, size_t size, unsigned flags) {
    if (size > UINTPTR_MAX - address) return false;
    for (unsigned i = 0; i < p->count_; ++i)
        if ((p->ranges_[i].flags & flags) == flags && address >= p->ranges_[i].begin && address + size <= p->ranges_[i].end) return true;
    return false;
}
az_sql_file_control az_sql_profile_file_control(const struct az_sql_profile *p) { return p->control_; }
uintptr_t az_sql_profile_open_entry(const struct az_sql_profile *p) { return p->base_ + 0x647b0; }
uintptr_t az_sql_profile_close_entry(const struct az_sql_profile *p) { return p->base_ + 0x512c0; }
static bool native_platform() {
#if defined(__linux__) && defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return sizeof(uintptr_t) == 8;
#else
    return false;
#endif
}



#if defined(__linux__)
static int ranges(struct dl_phdr_info *info, size_t unused, void *context) {
    (void) unused;
    __auto_type p = &(*((struct az_sql_profile *) (context)));
    if (info->dlpi_addr != p->base_) return 0;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        __auto_type h = &(info->dlpi_phdr[i]);
        if (h->p_type != PT_LOAD || !(h->p_flags & PF_R)) continue;
        if (p->count_ == 32 || h->p_vaddr > UINTPTR_MAX - p->base_ ||
            h->p_memsz > UINTPTR_MAX - p->base_ - h->p_vaddr) {
            p->count_ = 0;
            return 1;
        }
        p->ranges_[p->count_++] = (struct az_sql_range) {p->base_ + h->p_vaddr, p->base_ + h->p_vaddr + h->p_memsz, h->p_flags};
    }
    return 1;
}
static bool function(const struct az_sql_profile *p, const void *symbol, uintptr_t rva) {
    __auto_type address = ((uintptr_t) (symbol));
    return address >= p->base_ && address - p->base_ == rva && az_sql_profile_contains(p, address, 4, 5);
}
static bool bytes(const struct az_sql_profile *p, uintptr_t rva, const void *expected, size_t n) {
    return rva <= UINTPTR_MAX - p->base_ && az_sql_profile_contains(p, p->base_ + rva, n, 5) &&
           memcmp(((const void *) (p->base_ + rva)), expected, n) == 0;
}
#endif
#if defined(__linux__)
static bool profile_resolve_symbols(struct az_sql_profile *p, void *handle, void **symbols) {
    const char *names[] = {"sqlite3_file_control", "sqlite3_libversion_number", "sqlite3_libversion",
                           "sqlite3_open", "sqlite3_open_v2", "sqlite3_open16", "sqlite3_close", "sqlite3_close_v2"};
    const uintptr_t rvas[] = {0x244b0, 0x1330c, 0x13300, 0x64d50, 0x82488, 0x8248c, 0x5141c, 0x51424};
    bool valid = true;
    for (unsigned i = 0; i < 8; ++i) {
        symbols[i] = dlsym(handle, names[i]);
        if (!symbols[i]) valid = false;
    }
    Dl_info info = {0};
    if (valid && dladdr(symbols[0], &info)) {
        p->base_ = ((uintptr_t) (info.dli_fbase));
        dl_iterate_phdr(ranges, p);
    } else valid = false;
    for (unsigned i = 0; valid && i < 8; ++i) valid = function(p, symbols[i], rvas[i]);
    return valid;
}
static bool profile_entry_bytes(const struct az_sql_profile *p) {
    const uint32_t open_prefix[] = {0xd102c3ff, 0xa9027bfd, 0x910083fd, 0xa90353f3};
    const uint32_t close_prefix[] = {0xa9bd7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3};
    const uint32_t null_test = 0xb4000ac0;
    const uint32_t null_return[] = {0x52800000, 0xd65f03c0};
    return bytes(p, 0x647b0, open_prefix, sizeof open_prefix) &&
           bytes(p, 0x512c0, close_prefix, sizeof close_prefix) &&
           bytes(p, 0x512bc, &null_test, sizeof null_test) &&
           bytes(p, 0x51414, null_return, sizeof null_return) &&
           0xf980 <= UINTPTR_MAX - p->base_ && az_sql_profile_contains(p, p->base_ + 0xf980, 481192, 5);
}
static bool profile_code_digest(const struct az_sql_profile *p, void *handle) {
    /* Hash mapped code once using the public unkeyed OpenSSL API; never a DB or key. */
    __auto_type sha = ((const void *(*) ())(dlsym(handle, "EVP_sha256")));
    __auto_type digest = ((int (*)(const void *, size_t, unsigned char *, unsigned *, const void *, void *))(dlsym(handle, "EVP_Digest")));
    static const unsigned char expected[32] = {
        0x24, 0x3c, 0x4e, 0x20, 0xf5, 0x52, 0xc9, 0x7e, 0xe0, 0xaa, 0x8d, 0xe4, 0x52, 0x75, 0x01, 0xd7,
        0xa3, 0xdd, 0x5a, 0x22, 0x3e, 0x8e, 0x01, 0x27, 0x35, 0x74, 0x01, 0x7f, 0x7a, 0xfc, 0x39, 0x44};
    unsigned char hash[32] = {0};
    unsigned length = 0;
    return sha && digest &&
           digest(((const void *) (p->base_ + 0xf980)), 481192, hash, &length, sha(), NULL) &&
           length == sizeof hash && memcmp(hash, expected, sizeof hash) == 0;
}
static bool profile_version_matches(const struct az_sql_profile *p, void **symbols) {
    /* Invoke public getters only after the code has been checked and hashed. */
    if (((int (*)()) symbols[1])() != 3033000) return false;
    const char *version = ((const char *(*) ()) symbols[2])();
    return az_sql_profile_contains(p, (uintptr_t) version, 7, 4) && memcmp(version, "3.33.0", 7) == 0;
}
#endif
static const struct az_sql_profile *profile_load() {
#if defined(__linux__)
    if (!native_platform()) return NULL;
    void *handle = dlopen("libsqlcipher.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (!handle) return NULL;
    struct az_sql_profile *p = calloc(1, sizeof *p);
    if (!p) {
        dlclose(handle);
        return NULL;
    }
    void *symbols[8] = {0};
    bool valid = profile_resolve_symbols(p, handle, symbols) && profile_entry_bytes(p) &&
                 profile_code_digest(p, handle) && profile_version_matches(p, symbols);
    if (!valid) {
        free(p);
        dlclose(handle);
        return NULL;
    }
    p->control_ = ((az_sql_file_control) (symbols[0]));
    p->handle_ = handle; // deliberately never closed/deleted after publication
    return p;
#else
    return NULL;
#endif
}

static pthread_once_t profile_once = PTHREAD_ONCE_INIT;
static const struct az_sql_profile *prepared_profile;
static void prepare_once(void) { prepared_profile = profile_load(); }
const struct az_sql_profile *az_connection_prepare_profile(void) {
    AZ_NATIVE_ERRNO(preserve);
    if (pthread_once(&profile_once, prepare_once)) return NULL;
    return prepared_profile;
}

/* Immortal forwarding metadata only. EpochState remains the single existing
 * source-owned registry and must itself have process lifetime. */
struct Binding {
    struct az_connection_state *epochs;
    void *originals[2];
    _Atomic(bool) armed;
};
static _Atomic(struct Binding *) binding;
static _Atomic(bool) attempted = false;

static __attribute__((noreturn)) void broken_publication() {
    /* No native result can be preserved if an installer violates the required
     * original-before-entry contract. Never fabricate a successful result. */
    abort();
}
struct OpenCall {
    az_native_sql_open original;
    const char *path;
    struct sqlite3 **output;
    int flags;
    const char *vfs;
};
static int forward_open(void *opaque) {
    struct OpenCall *c = opaque;
    return c->original(c->path, c->output, c->flags, c->vfs);
}
struct CloseCall {
    az_native_sql_close original;
    struct sqlite3 *connection;
    int mode;
};
static int forward_close(void *opaque) {
    struct CloseCall *c = opaque;
    return c->original(c->connection, c->mode);
}
static int open_entry(const char *path, struct sqlite3 **output, int flags, const char *vfs) {
    struct Binding *b = atomic_load_explicit(&(binding), memory_order_acquire);
    if (!b) broken_publication();
    __auto_type original = ((az_native_sql_open) (__atomic_load_n(&b->originals[0], __ATOMIC_ACQUIRE)));
    if (!original || original == &open_entry) broken_publication();
    /* Admission is captured at ENTRY. An operation that started during partial
     * installation cannot become a birth merely because installation finishes. */
    struct sqlite3 **observed = atomic_load_explicit(&(b->armed), memory_order_acquire) ? output : NULL;
    struct OpenCall call = {original, path, output, flags, vfs};
    return az_connection_forward_open(b->epochs, observed, forward_open, &call, NULL);
}
static int close_entry(struct sqlite3 *connection, int mode) {
    struct Binding *b = atomic_load_explicit(&(binding), memory_order_acquire);
    if (!b) broken_publication();
    __auto_type original = ((az_native_sql_close) (__atomic_load_n(&b->originals[1], __ATOMIC_ACQUIRE)));
    if (!original || original == &close_entry) broken_publication();
    // Close observation has no readiness/diagnostics/enable bypass.
    struct CloseCall call = {original, connection, mode};
    return az_connection_forward_close(b->epochs, connection, forward_close, &call);
}
struct InstallCall {
    struct Binding *binding;
    struct az_connection_installer installer;
    uintptr_t base;
    int result;
};
static void install_call(void *opaque) {
    struct InstallCall *c = opaque;
    struct Binding *b = c->binding;
    struct az_connection_installer installer = c->installer;
    uintptr_t base = c->base;

    int closed = installer.call(base + 0x512c0, ((void *) (&close_entry)), &b->originals[1], installer.context);
    void *close_original = __atomic_load_n(&b->originals[1], __ATOMIC_ACQUIRE);
    if (closed != 1 || !close_original || close_original == ((void *) (&close_entry))) return;
    int opened = installer.call(base + 0x647b0, ((void *) (&open_entry)), &b->originals[0], installer.context);
    void *open_original = __atomic_load_n(&b->originals[0], __ATOMIC_ACQUIRE);
    if (opened != 1 || !open_original || open_original == ((void *) (&open_entry)) || open_original == close_original) return;
    atomic_store_explicit(&(b->armed), true, memory_order_release);
    c->result = 1;
}
static int install_claimed(struct az_connection_state *epochs, struct az_connection_installer installer, uintptr_t base) {
    if (!installer.call || base > UINTPTR_MAX - 0x647c0) return 0;
    struct Binding *b = calloc(1, sizeof *b);
    if (!b) return 0;
    b->epochs = epochs;
    struct Binding *empty = NULL;
    if (!atomic_compare_exchange_strong_explicit(&(binding), &(empty), b, memory_order_acq_rel, memory_order_acquire)) {
        free(b);
        return 0;
    }
    // Publication precedes either installer; its callbacks may enter immediately.
    struct InstallCall call = {b, installer, base, 0};
    install_call(&call);
    return call.result;
}

int az_connection_install(struct az_connection_state *epochs, struct az_connection_installer installer) {
    AZ_NATIVE_ERRNO(preserve);
    if (atomic_exchange_explicit(&(attempted), true, memory_order_acq_rel)) return 0;
    const struct az_sql_profile *profile = az_connection_prepare_profile();
    return profile ? install_claimed(epochs, installer, az_sql_profile_base(profile)) : 0;
}
bool az_connection_ready() {
    struct Binding *b = atomic_load_explicit(&(binding), memory_order_acquire);
    return b && atomic_load_explicit(&(b->armed), memory_order_acquire);
}
#if defined(AZSDK_CONNECTION_HOOKS_TESTING)

int az_connection_test_install(struct az_connection_state *epochs, struct az_connection_installer installer, uintptr_t base) {
    AZ_NATIVE_ERRNO(preserve);
    if (atomic_exchange_explicit(&(attempted), true, memory_order_acq_rel)) return 0;
    return install_claimed(epochs, installer, base);
}
#endif
