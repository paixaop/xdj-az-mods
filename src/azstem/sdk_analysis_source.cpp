/* SPDX-License-Identifier: GPL-3.0-only */
/* Beatport's native download worker certifies completion before its load
 * listener runs. Retain only its genuine ITrack, never a guessed cache path.
 * Firmware 2.00: doWaitDecrypt 0xd39610; Track::state 0x178d760 locks +0xa8;
 * Track::onSdkCallback 0x17925e8 sets decrypted only after the final rename.
 * Track::~Track 0x178e5f8 serializes SDK/file cleanup with its global mutex.
 * Profile image SHA-256: 6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925. */
#include "../common/azanalysis_native.h"
#include "../common/azloadobserver.h"
#include "../common/azstatus_native.h"
#include "../azmod/azmod-mod.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <link.h>
#endif
#if defined(_LIBCPP_VERSION)
#error "Native Beatport resources require GNU libstdc++ shared_ptr ABI"
#endif

namespace {
using Track = std::shared_ptr<const void>;
using WeakTrack = std::weak_ptr<const void>;
using WaitDecrypt = int (*)(uintptr_t, const WeakTrack *, int);
constexpr unsigned source_count = 16, lease_count = 8;
struct Source {
    uintptr_t owner = 0;
    uint64_t revision = 0;
    unsigned char id[12] = {};
    char path[1024] = {};
    WeakTrack track;
};
struct Lease {
    uint64_t token = 0, revision = 0, epoch = 0, load = 0;
    unsigned source = 0, deck = 0;
    int fd = -1;
    dev_t device = 0;
    ino_t inode = 0;
    off_t bytes = 0;
    unsigned char id[12] = {};
    char path[1024] = {};
    Track track;
};
Source sources[source_count];
Lease leases[lease_count];
std::mutex mutex, installation;
uint64_t serial = 0;
uintptr_t bias = 0;
void *original_wait;
std::atomic<bool> installed {false};

template<class T> T scalar(uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof value);
    return value;
}
bool typed(const Track &track) {
    return track && scalar<uintptr_t>(reinterpret_cast<uintptr_t>(track.get())) == bias + 0x2c5b028;
}
bool completed(const Track &track) {
    if (!typed(track)) return false;
    /* Native nontrivial return uses x8 under AAPCS64, which this exact C++
     * shared_ptr signature supplies. The getter owns its mutex and copy. */
    using GetState = Track (*)(const void *);
    Track state = reinterpret_cast<GetState>(bias + 0x178d760)(track.get());
    if (!state) return false;
    uintptr_t p = reinterpret_cast<uintptr_t>(state.get());
    return scalar<uintptr_t>(p) == bias + 0x2c5b0b8 && scalar<int>(p + 0xc) == 0 &&
           scalar<int>(p + 0x10) > 0 && scalar<int>(p + 0x14) >= scalar<int>(p + 0x10) &&
           scalar<unsigned char>(p + 0x18) == 1;
}
bool load_matches(unsigned deck, uint64_t epoch, uint64_t load, const unsigned char id[12]) {
    azsdk_load_sample now {};
    return deck < 4 && epoch && load && azsdk_load_read(int(deck), &now) &&
           now.invalidation_epoch == epoch && now.load_generation == load &&
           azn_load_track_matches(deck, epoch, load, id);
}
bool path_copy(const Track &track, char (&out)[1024]) {
    const char *path = scalar<const char *>(reinterpret_cast<uintptr_t>(track.get()) + 0x78);
    if (!path || path[0] != '/') return false;
    size_t length = strnlen(path, sizeof out);
    if (!length || length >= sizeof out) return false;
    std::memcpy(out, path, length + 1);
    return true;
}
void capture(uintptr_t owner, const WeakTrack *weak, int generation,
             const unsigned char before[12]) {
    if (!installed.load(std::memory_order_acquire) || !owner || !weak) return;
    const uintptr_t counter = scalar<uintptr_t>(owner + 0x28);
    if (!counter || __atomic_load_n(reinterpret_cast<const int *>(counter), __ATOMIC_ACQUIRE) != generation ||
        std::memcmp(before, reinterpret_cast<const void *>(owner + 0x68), 12)) return;
    /* Only local Beatport media is admitted. Other services and linked source
     * families need independently verified resource lifetime profiles. */
    if (before[0] != 1 || (before[4] & 31u) != 6) return;
    Track track = weak->lock();
    if (!completed(track) || scalar<uint64_t>(reinterpret_cast<uintptr_t>(track.get()) + 0x28) !=
                             scalar<uint64_t>(owner + 0x160)) return;
    char path[1024] = {};
    if (!path_copy(track, path)) return;
    /* This is the native download worker, never the audio or UI thread. Do
     * not lose its only completion observation to a simultaneous lease read. */
    std::lock_guard<std::mutex> lock(mutex);
    if (serial == UINT64_MAX) return;
    unsigned slot = source_count;
    for (unsigned i = 0; i < source_count; ++i)
        if (sources[i].owner == owner) { slot = i; break; }
    if (slot == source_count)
        for (unsigned i = 0; i < source_count; ++i)
            if (!sources[i].revision || sources[i].track.expired()) { slot = i; break; }
    if (slot == source_count) return;
    Source &s = sources[slot];
    s.owner = owner;
    s.revision = ++serial;
    std::memcpy(s.id, before, 12);
    std::memcpy(s.path, path, sizeof path);
    s.track = track;
}
int wait_decrypt(uintptr_t owner, const WeakTrack *weak, int generation) {
    unsigned char id[12] = {};
    if (owner) std::memcpy(id, reinterpret_cast<const void *>(owner + 0x68), sizeof id);
    const int result = reinterpret_cast<WaitDecrypt>(original_wait)(owner, weak, generation);
    if (result == 0) {
        try { capture(owner, weak, generation, id); } catch (...) { /* preserve native load */ }
    }
    return result;
}
bool current_copy(const Lease &lease) {
    if (!load_matches(lease.deck, lease.epoch, lease.load, lease.id) || !completed(lease.track)) return false;
    struct stat fd {}, path {};
    if (lease.fd < 0 || fstat(lease.fd, &fd) || lstat(lease.path, &path) ||
        !S_ISREG(fd.st_mode) || !S_ISREG(path.st_mode) || fd.st_dev != lease.device ||
        fd.st_ino != lease.inode || fd.st_size != lease.bytes ||
        path.st_dev != fd.st_dev || path.st_ino != fd.st_ino || path.st_size != fd.st_size) return false;
    return load_matches(lease.deck, lease.epoch, lease.load, lease.id);
}
int open_source(unsigned deck, uint64_t epoch, uint64_t load, azn_analysis_source *out) {
    if (!out || out->size < sizeof *out || out->version != 1 || deck >= 4 || !epoch || !load) return AZ_INVALID;
    if (!installed.load(std::memory_order_acquire)) return AZ_UNSUPPORTED;
    Lease next;
    for (unsigned i = 0; i < source_count; ++i) {
        {
            std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
            if (!lock.owns_lock()) return AZ_BUSY;
            Source &s = sources[i];
            if (!s.revision || !load_matches(deck, epoch, load, s.id)) continue;
            Track held = s.track.lock();
            if (!held) continue;
            next.source = i; next.revision = s.revision;
            next.deck = deck; next.epoch = epoch; next.load = load;
            next.track = std::move(held);
            std::memcpy(next.id, s.id, sizeof next.id);
            std::memcpy(next.path, s.path, sizeof next.path);
        }
        if (completed(next.track)) break;
        /* A retired download on another deck must not mask a newer complete
         * resource for the same full TrackID. Release outside our mutex. */
        next.track.reset();
    }
    if (!next.track) return AZ_NOT_FOUND;
    next.fd = ::open(next.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st {};
    if (next.fd < 0 || fstat(next.fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        if (next.fd >= 0) ::close(next.fd);
        return AZ_STALE;
    }
    next.device = st.st_dev; next.inode = st.st_ino; next.bytes = st.st_size;
    int rc = AZ_STALE;
    if (current_copy(next)) {
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) rc = AZ_BUSY;
        else if (sources[next.source].revision == next.revision) {
            rc = AZ_LIMIT;
            for (auto &lease : leases) {
                if (lease.token || serial == UINT64_MAX) continue;
                next.token = ++serial;
                azn_analysis_source result {};
                result.size = sizeof result; result.version = 1; result.token = next.token;
                result.epoch = epoch; result.load = load; result.source_kind = AZ_STATUS_SOURCE_STREAMING;
                std::memcpy(result.track_id, next.id, sizeof result.track_id);
                std::memcpy(result.audio_path, next.path, sizeof result.audio_path);
                lease = std::move(next);
                next.fd = -1;
                *out = result;
                rc = AZ_OK;
                break;
            }
        }
    }
    if (next.fd >= 0) ::close(next.fd);
    return rc;
}
int current_source(uint64_t token) {
    if (!token) return AZ_INVALID;
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return AZ_BUSY;
    for (const auto &lease : leases)
        if (lease.token == token)
            return sources[lease.source].revision == lease.revision && current_copy(lease) ? AZ_OK : AZ_STALE;
    return AZ_STALE;
}
void close_source(uint64_t token) {
    Lease retired;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto &lease : leases) if (lease.token == token && token) {
            retired = std::move(lease);
            lease = Lease {};
            break;
        }
    }
    if (retired.fd >= 0) ::close(retired.fd);
    /* Last counted release can delete native files and run callbacks. It must
     * happen outside our registry lock and away from audio/panel callbacks. */
}
#if defined(__linux__)
int main_image(dl_phdr_info *info, size_t, void *out) {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    *static_cast<uintptr_t *>(out) = info->dlpi_addr;
    return 1;
}
#endif
int install_source(int build, azn_analysis_patch_fn patch) {
#if defined(__linux__) && defined(__aarch64__)
    std::lock_guard<std::mutex> lock(installation);
    if (build != 2 || !patch) return AZ_UNSUPPORTED;
    if (installed.load(std::memory_order_acquire)) return AZ_OK;
    dl_iterate_phdr(main_image, &bias);
    const uint32_t words[] = {0xa9b57bfdu, 0x910003fdu, 0x6d0527e8u, 0x0f000408u};
    const uintptr_t address = bias + 0xd39610;
    if (std::memcmp(reinterpret_cast<const void *>(address), words, sizeof words)) return AZ_UNSUPPORTED;
    if (!patch(address, reinterpret_cast<void *>(wait_decrypt), &original_wait, "sdk-beatport-analysis") || !original_wait)
        return AZ_FAILED;
    installed.store(true, std::memory_order_release);
    return AZ_OK;
#else
    (void) build; (void) patch;
    return AZ_UNSUPPORTED;
#endif
}
}
extern "C" const azn_analysis_source_api *azn_analysis_sources(void) {
    static const azn_analysis_source_api api {sizeof api, 1, install_source, open_source, current_source, close_source};
    return &api;
}
