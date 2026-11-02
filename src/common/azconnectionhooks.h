/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCONNECTIONHOOKS_H
#define AZCONNECTIONHOOKS_H
#include "azconnectionepoch.h"
#include <cstddef>
#include <cstdint>

struct sqlite3;
namespace azsdk_connection_hooks {
using EpochState = azsdk_connection_epoch::State<32>;
using NativeOpen = int (*)(const char *, sqlite3 **, int, const char *);
using NativeClose = int (*)(sqlite3 *, int);
using FileControl = int (*)(sqlite3 *, const char *, int, void *);
struct Range { std::uintptr_t begin, end; unsigned flags; };
namespace detail { class ProfileBuilder; }

/* Immutable, privately minted proof of the already loaded exact module.
 * Preparation precedes the first code redirect. The retained module reference
 * and this proof deliberately live through process teardown; consumers reuse
 * the proof rather than rehashing code modified by their own installed hooks.
 * This owns code lifetime, never a SQLite connection or C++ database owner. */
class SqlProfile {
    friend class detail::ProfileBuilder;
    std::uintptr_t base_ = 0;
    Range ranges_[32] = {};
    unsigned count_ = 0;
    FileControl control_ = nullptr;
    void *handle_ = nullptr;
    SqlProfile() noexcept = default;
    ~SqlProfile() = default;
    SqlProfile(const SqlProfile &) = delete;
    SqlProfile &operator=(const SqlProfile &) = delete;
public:
    std::uintptr_t base() const noexcept { return base_; }
    unsigned range_count() const noexcept { return count_; }
    const Range *range(unsigned i) const noexcept { return i < count_ ? &ranges_[i] : nullptr; }
    bool contains(std::uintptr_t address, std::size_t size, unsigned flags) const noexcept {
        if (size > UINTPTR_MAX - address) return false;
        for (unsigned i = 0; i < count_; ++i)
            if ((ranges_[i].flags & flags) == flags && address >= ranges_[i].begin &&
                address + size <= ranges_[i].end) return true;
        return false;
    }
    FileControl file_control() const noexcept { return control_; }
    std::uintptr_t open_entry() const noexcept { return base_ + 0x647b0; }
    std::uintptr_t close_entry() const noexcept { return base_ + 0x512c0; }
    std::uintptr_t io_rva(unsigned i) const noexcept {
        return i == 0 ? 0xb3d98 : i == 1 ? 0xb3e30 : i == 2 ? 0xb3ec8 : 0;
    }
    std::uintptr_t vfs_open_rva() const noexcept { return 0x53968; }
    std::uintptr_t read_rva() const noexcept { return 0x1513c; }
    std::uintptr_t size_rva() const noexcept { return 0x1bdf4; }
    std::uintptr_t control_rva() const noexcept { return 0x536b0; }
};

/* Startup-only real loader/byte validation. No caller trust flag, pathname
 * override or injectable production profile. Unsupported/failed preparation
 * is permanently unavailable for this process. No database API is invoked. */
__attribute__((visibility("hidden"))) const SqlProfile *prepare_profile() noexcept;

struct Installer {
    /* The main-owned island-only installer must publish the genuine callable
     * original in this distinct slot BEFORE making the replacement reachable.
     * Return exactly 1 for an accepted install. Never retain/mutate the slot
     * later or publish an entry with no callable original. Other returns and
     * exceptions leave birth tracking unavailable; a live close stays active. */
    int (*call)(std::uintptr_t entry, void *replacement, void **original, void *context);
    void *context;
};

/* Opt-in dependency only: nothing installs at static initialization. A single
 * immutable process-lifetime binding borrows the supplied existing State.
 * The caller MUST keep that State alive through every later hooked native call,
 * including DSO/atexit teardown; ordinary destructor ordering is insufficient.
 * Close installs first, both originals must exist before births are admitted.
 * No disable/reset/unbind API can silently stop observing an installed close.
 */
__attribute__((visibility("hidden"))) int install(EpochState &, Installer) noexcept;
__attribute__((visibility("hidden"))) bool ready() noexcept;

#if defined(AZSDK_CONNECTION_HOOKS_TESTING)
namespace testing {
/* Synthetic forwarding only. This does not mint/validate a module proof and
 * is absent from ordinary production compilation. One binding per process. */
int install_forwarding(EpochState &, Installer, std::uintptr_t synthetic_base) noexcept;
bool ready() noexcept;
}
#endif
} // namespace azsdk_connection_hooks
#endif
