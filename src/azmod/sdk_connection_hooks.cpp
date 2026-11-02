/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/azconnectionhooks.h"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#if defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#endif

namespace azsdk_connection_hooks {
namespace {
struct PreserveErrno {
    int value = errno;
    ~PreserveErrno() noexcept { errno = value; }
};
bool native_platform() noexcept {
#if defined(__linux__) && defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return sizeof(std::uintptr_t) == 8;
#else
    return false;
#endif
}
}
namespace detail {
class ProfileBuilder {
#if defined(__linux__)
    static int ranges(dl_phdr_info *info, std::size_t, void *context) noexcept {
        auto &p = *static_cast<SqlProfile *>(context);
        if (info->dlpi_addr != p.base_) return 0;
        for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
            const auto &h = info->dlpi_phdr[i];
            if (h.p_type != PT_LOAD || !(h.p_flags & PF_R)) continue;
            if (p.count_ == 32 || h.p_vaddr > UINTPTR_MAX - p.base_ ||
                h.p_memsz > UINTPTR_MAX - p.base_ - h.p_vaddr) { p.count_ = 0; return 1; }
            p.ranges_[p.count_++] = Range {p.base_ + h.p_vaddr, p.base_ + h.p_vaddr + h.p_memsz, h.p_flags};
        }
        return 1;
    }
    static bool function(const SqlProfile &p, const void *symbol, std::uintptr_t rva) noexcept {
        auto address = reinterpret_cast<std::uintptr_t>(symbol);
        return address >= p.base_ && address - p.base_ == rva && p.contains(address, 4, 5);
    }
    static bool bytes(const SqlProfile &p, std::uintptr_t rva, const void *expected, std::size_t n) noexcept {
        return rva <= UINTPTR_MAX - p.base_ && p.contains(p.base_ + rva, n, 5) &&
               std::memcmp(reinterpret_cast<const void *>(p.base_ + rva), expected, n) == 0;
    }
#endif
public:
    static const SqlProfile *load() noexcept {
#if defined(__linux__)
        if (!native_platform()) return nullptr;
        void *handle = dlopen("libsqlcipher.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
        if (!handle) return nullptr;
        SqlProfile *p = new (std::nothrow) SqlProfile;
        if (!p) { dlclose(handle); return nullptr; }
        const char *names[] = {"sqlite3_file_control", "sqlite3_libversion_number", "sqlite3_libversion",
                               "sqlite3_open", "sqlite3_open_v2", "sqlite3_open16", "sqlite3_close", "sqlite3_close_v2"};
        const std::uintptr_t rvas[] = {0x244b0, 0x1330c, 0x13300, 0x64d50, 0x82488, 0x8248c, 0x5141c, 0x51424};
        void *symbols[8] = {};
        bool valid = true;
        for (unsigned i = 0; i < 8; ++i) { symbols[i] = dlsym(handle, names[i]); if (!symbols[i]) valid = false; }
        Dl_info info = {};
        if (valid && dladdr(symbols[0], &info)) {
            p->base_ = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
            dl_iterate_phdr(ranges, p);
        } else valid = false;
        for (unsigned i = 0; valid && i < 8; ++i) valid = function(*p, symbols[i], rvas[i]);
        const std::uint32_t open_prefix[] = {0xd102c3ff, 0xa9027bfd, 0x910083fd, 0xa90353f3};
        const std::uint32_t close_prefix[] = {0xa9bd7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3};
        const std::uint32_t null_test = 0xb4000ac0;
        const std::uint32_t null_return[] = {0x52800000, 0xd65f03c0};
        valid = valid && bytes(*p, 0x647b0, open_prefix, sizeof open_prefix) &&
                bytes(*p, 0x512c0, close_prefix, sizeof close_prefix) &&
                bytes(*p, 0x512bc, &null_test, sizeof null_test) &&
                bytes(*p, 0x51414, null_return, sizeof null_return) &&
                0xf980 <= UINTPTR_MAX - p->base_ && p->contains(p->base_ + 0xf980, 481192, 5);
        /* Public unkeyed OpenSSL dependency API, exactly as the existing physical
         * profile. This hashes mapped code once at startup, never a DB or key. */
        auto sha = reinterpret_cast<const void *(*)()>(dlsym(handle, "EVP_sha256"));
        auto digest = reinterpret_cast<int (*)(const void *, std::size_t, unsigned char *, unsigned *, const void *, void *)>(dlsym(handle, "EVP_Digest"));
        static const unsigned char expected[32] = {
            0x24,0x3c,0x4e,0x20,0xf5,0x52,0xc9,0x7e,0xe0,0xaa,0x8d,0xe4,0x52,0x75,0x01,0xd7,
            0xa3,0xdd,0x5a,0x22,0x3e,0x8e,0x01,0x27,0x35,0x74,0x01,0x7f,0x7a,0xfc,0x39,0x44};
        unsigned char hash[32] = {}; unsigned length = 0;
        valid = valid && sha && digest &&
                digest(reinterpret_cast<const void *>(p->base_ + 0xf980), 481192, hash, &length, sha(), nullptr) &&
                length == sizeof hash && std::memcmp(hash, expected, sizeof hash) == 0;
        /* Only checked, hashed public getter code is called. No connection API. */
        if (valid) valid = reinterpret_cast<int (*)()>(symbols[1])() == 3033000;
        if (valid) {
            const char *version = reinterpret_cast<const char *(*)()>(symbols[2])();
            valid = p->contains(reinterpret_cast<std::uintptr_t>(version), 7, 4) && std::memcmp(version, "3.33.0", 7) == 0;
        }
        if (!valid) { delete p; dlclose(handle); return nullptr; }
        p->control_ = reinterpret_cast<FileControl>(symbols[0]);
        p->handle_ = handle; // deliberately never closed/deleted after publication
        return p;
#else
        return nullptr;
#endif
    }
};
} // namespace detail

const SqlProfile *prepare_profile() noexcept {
    PreserveErrno preserve;
    static std::once_flag once;
    static const SqlProfile *profile = nullptr;
    try { std::call_once(once, []() { profile = detail::ProfileBuilder::load(); }); }
    catch (...) { return nullptr; }
    return profile;
}

namespace {
/* Immortal forwarding metadata only. EpochState remains the single existing
 * source-owned registry and must itself have process lifetime. */
struct Binding {
    EpochState &epochs;
    void *originals[2] = {};
    std::atomic<bool> armed {false};
    explicit Binding(EpochState &s) noexcept : epochs(s) {}
};
std::atomic<Binding *> binding {nullptr};
std::atomic<bool> attempted {false};

[[noreturn]] void broken_publication() noexcept {
    /* No native result can be preserved if an installer violates the required
     * original-before-entry contract. Never fabricate a successful result. */
    std::abort();
}
int open_entry(const char *path, sqlite3 **output, int flags, const char *vfs) {
    Binding *b = binding.load(std::memory_order_acquire);
    if (!b) broken_publication();
    auto original = reinterpret_cast<NativeOpen>(__atomic_load_n(&b->originals[0], __ATOMIC_ACQUIRE));
    if (!original || original == &open_entry) broken_publication();
    /* Admission is captured at ENTRY. An operation that started during partial
     * installation cannot become a birth merely because installation finishes. */
    sqlite3 **observed = b->armed.load(std::memory_order_acquire) ? output : nullptr;
    return b->epochs.forward_open(observed, [&]() { return original(path, output, flags, vfs); });
}
int close_entry(sqlite3 *connection, int mode) {
    Binding *b = binding.load(std::memory_order_acquire);
    if (!b) broken_publication();
    auto original = reinterpret_cast<NativeClose>(__atomic_load_n(&b->originals[1], __ATOMIC_ACQUIRE));
    if (!original || original == &close_entry) broken_publication();
    // Close observation has no readiness/diagnostics/enable bypass.
    return b->epochs.forward_close(connection, [&]() { return original(connection, mode); });
}
int install_claimed(EpochState &epochs, Installer installer, std::uintptr_t base) noexcept {
    if (!installer.call || base > UINTPTR_MAX - 0x647c0) return 0;
    Binding *b = new (std::nothrow) Binding(epochs);
    if (!b) return 0;
    Binding *empty = nullptr;
    if (!binding.compare_exchange_strong(empty, b, std::memory_order_acq_rel)) { delete b; return 0; }
    // Publication precedes either installer; its callbacks may enter immediately.
    try {
        int closed = installer.call(base + 0x512c0, reinterpret_cast<void *>(&close_entry), &b->originals[1], installer.context);
        void *close_original = __atomic_load_n(&b->originals[1], __ATOMIC_ACQUIRE);
        if (closed != 1 || !close_original || close_original == reinterpret_cast<void *>(&close_entry)) return 0;
        int opened = installer.call(base + 0x647b0, reinterpret_cast<void *>(&open_entry), &b->originals[0], installer.context);
        void *open_original = __atomic_load_n(&b->originals[0], __ATOMIC_ACQUIRE);
        if (opened != 1 || !open_original || open_original == reinterpret_cast<void *>(&open_entry) || open_original == close_original) return 0;
        b->armed.store(true, std::memory_order_release);
        return 1;
    } catch (...) {
        // Existing entries/originals remain intact and close keeps retiring.
        return 0;
    }
}
}

int install(EpochState &epochs, Installer installer) noexcept {
    PreserveErrno preserve;
    if (attempted.exchange(true, std::memory_order_acq_rel)) return 0;
    const SqlProfile *profile = prepare_profile();
    return profile ? install_claimed(epochs, installer, profile->base()) : 0;
}
bool ready() noexcept {
    Binding *b = binding.load(std::memory_order_acquire);
    return b && b->armed.load(std::memory_order_acquire);
}
#if defined(AZSDK_CONNECTION_HOOKS_TESTING)
namespace testing {
int install_forwarding(EpochState &epochs, Installer installer, std::uintptr_t base) noexcept {
    PreserveErrno preserve;
    if (attempted.exchange(true, std::memory_order_acq_rel)) return 0;
    return install_claimed(epochs, installer, base);
}
bool ready() noexcept { return azsdk_connection_hooks::ready(); }
}
#endif
} // namespace azsdk_connection_hooks
