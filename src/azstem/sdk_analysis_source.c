/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include "../common/aznative_calls.h"
#include "../common/aznative_sync.h"
#include <stdatomic.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <link.h>
#endif
typedef struct az_native_owner Track;
typedef struct az_native_owner WeakTrack;
typedef int (*WaitDecrypt)(uintptr_t, const WeakTrack *, int);
enum {
    source_count = 16,
    lease_count = 8
};
typedef struct Source {
    uintptr_t owner;
    uint64_t revision;
    unsigned char id[12];
    char path[1024];
    WeakTrack track;
} Source;
typedef struct Lease {
    uint64_t token, revision, epoch, load;
    dev_t device;
    ino_t inode;
    off_t bytes;
    Track track;
    unsigned source, deck;
    int fd;
    unsigned char id[12];
    char path[1024];
} Lease;
static Source sources[source_count];
static Lease leases[lease_count];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER, installation = PTHREAD_MUTEX_INITIALIZER;
static uint64_t serial;
static uintptr_t bias;
static void *original_wait;
static _Atomic bool installed;
#define scalar(type, address) ({ type value; memcpy(&value, (const void *)(address), sizeof value); value; })
static void lease_release(Lease *lease) {
    if (lease->fd >= 0) close(lease->fd);
    lease->fd = -1;
    az_native_owner_reset(&lease->track);
}
static bool typed(const Track *track) {
    return track->object && scalar(uintptr_t, (uintptr_t) track->object) == bias + 0x2c5b028;
}
static bool completed(const Track *track) {
    if (!typed(track)) return false;
    /* Firmware returns a nontrivial owner through x8, even though its two-word
     * representation would be returned in x0/x1 by an ordinary C function. */
    Track state __attribute__((cleanup(az_native_owner_reset))) =
        az_native_track_state_acquire((az_native_track_state_fn) (bias + 0x178d760),
                                      (const struct az_native_track *) track->object);
    if (!state.object) return false;
    uintptr_t p = (uintptr_t) state.object;
    return scalar(uintptr_t, p) == bias + 0x2c5b0b8 && scalar(int, p + 0xc) == 0 &&
           scalar(int, p + 0x10) > 0 && scalar(int, p + 0x14) >= scalar(int, p + 0x10) &&
           scalar(unsigned char, p + 0x18) == 1;
}
static bool load_matches(unsigned deck, uint64_t epoch, uint64_t load, const unsigned char id[12]) {
    struct azsdk_load_sample now = {0};
    return deck < 4 && epoch && load && azsdk_load_read((int) deck, &now) &&
           now.invalidation_epoch == epoch && now.load_generation == load &&
           azn_load_track_matches(deck, epoch, load, id);
}
static bool path_copy(const Track *track, char out[1024]) {
    const char *path = scalar(const char *, ((uintptr_t) (track->object)) + 0x78);
    if (!path || path[0] != '/') return false;
    size_t length = strnlen(path, 1024);
    if (!length || length >= 1024) return false;
    memcpy(out, path, length + 1);
    return true;
}
static bool capture_identity_current(uintptr_t owner, int generation, const unsigned char before[12]) {
    const uintptr_t counter = scalar(uintptr_t, owner + 0x28);
    if (!counter || __atomic_load_n(((const int *) (counter)), __ATOMIC_ACQUIRE) != generation ||
        memcmp(before, ((const void *) (owner + 0x68)), 12)) return false;
    /* Only local Beatport media is admitted. Other services and linked source
     * families need independently verified resource lifetime profiles. */
    if (before[0] != 1 || (before[4] & 31u) != 6) return false;
    return true;
}

static unsigned source_slot(uintptr_t owner) {
    unsigned slot = source_count;
    for (unsigned i = 0; i < source_count; ++i)
        if (sources[i].owner == owner) {
            slot = i;
            break;
        }
    if (slot == source_count)
        for (unsigned i = 0; i < source_count; ++i)
            if (!sources[i].revision || !az_native_alive(sources[i].track.control)) {
                slot = i;
                break;
            }
    return slot;
}

static void retain_source(uintptr_t owner, const unsigned char before[12], const char path[1024], const Track *track) {
    /* This is the native download worker, never the audio or UI thread. Do
     * not lose its only completion observation to a simultaneous lease read. */
    AZ_MUTEX_GUARD(lock, mutex, false);
    if (lock.error) return;
    if (serial == UINT64_MAX) return;
    unsigned slot = source_slot(owner);
    if (slot == source_count) return;
    Source *s = &sources[slot];
    s->owner = owner;
    s->revision = ++serial;
    memcpy(s->id, before, 12);
    memcpy(s->path, path, sizeof s->path);
    az_native_weak_copy(&s->track, track);
}
static void capture(uintptr_t owner, const WeakTrack *weak, int generation,
                    const unsigned char before[12]) {
    if (!atomic_load_explicit(&(installed), memory_order_acquire) || !owner || !weak) return;
    if (!capture_identity_current(owner, generation, before)) return;
    Track track __attribute__((cleanup(az_native_owner_reset))) = az_native_lock(weak);
    if (!completed(&track) || scalar(uint64_t, ((uintptr_t) (track.object)) + 0x28) !=
                                  scalar(uint64_t, owner + 0x160)) return;
    char path[1024] = {0};
    if (!path_copy(&track, path)) return;
    retain_source(owner, before, path, &track);
}
struct Capture {
    uintptr_t owner;
    const WeakTrack *weak;
    int generation;
    const unsigned char *id;
};
static void capture_guarded(void *context) {
    const struct Capture *call = context;
    capture(call->owner, call->weak, call->generation, call->id);
}
static int wait_decrypt(uintptr_t owner, const WeakTrack *weak, int generation) {
    unsigned char id[12] = {0};
    if (owner) memcpy(id, ((const void *) (owner + 0x68)), sizeof id);
    const int result = ((WaitDecrypt) (original_wait))(owner, weak, generation);
    if (result == 0) {
        struct Capture call = {owner, weak, generation, id};
        capture_guarded(&call);
    }
    return result;
}
static bool lease_file_matches(const Lease *lease, const struct stat *file) {
    return S_ISREG(file->st_mode) && file->st_dev == lease->device &&
           file->st_ino == lease->inode && file->st_size == lease->bytes;
}
static bool same_file(const struct stat *file, const struct stat *path) {
    return S_ISREG(path->st_mode) && path->st_dev == file->st_dev &&
           path->st_ino == file->st_ino && path->st_size == file->st_size;
}

static bool current_copy(const Lease *lease) {
    if (!load_matches(lease->deck, lease->epoch, lease->load, lease->id) || !completed(&lease->track)) return false;
    struct stat fd = {0}, path = {0};
    if (lease->fd < 0 || fstat(lease->fd, &fd) || lstat(lease->path, &path)) return false;
    if (!lease_file_matches(lease, &fd) || !same_file(&fd, &path)) return false;
    return load_matches(lease->deck, lease->epoch, lease->load, lease->id);
}
static int acquire_source(unsigned i, unsigned deck, uint64_t epoch, uint64_t load, Lease *next) {
    AZ_MUTEX_GUARD(lock, mutex, true);
    if (!lock.held) return AZ_BUSY;
    Source *s = &sources[i];
    if (!s->revision || !load_matches(deck, epoch, load, s->id)) return AZ_NOT_FOUND;
    Track held __attribute__((cleanup(az_native_owner_reset))) = az_native_lock(&s->track);
    if (!held.object) return AZ_NOT_FOUND;
    next->source = i;
    next->revision = s->revision;
    next->deck = deck;
    next->epoch = epoch;
    next->load = load;
    next->track = held;
    held = (Track) {0};
    (void) held; /* Ownership moved to next; the cleanup guard must release nothing. */
    memcpy(next->id, s->id, sizeof next->id);
    memcpy(next->path, s->path, sizeof next->path);
    return AZ_OK;
}

static int find_source(unsigned deck, uint64_t epoch, uint64_t load, Lease *next) {
    for (unsigned i = 0; i < source_count; ++i) {
        int rc = acquire_source(i, deck, epoch, load, next);
        if (rc == AZ_BUSY) return rc;
        if (rc != AZ_OK) continue;
        if (completed(&next->track)) break;
        /* A retired download on another deck must not mask a newer complete
         * resource for the same full TrackID. Release outside our mutex. */
        az_native_owner_reset(&next->track);
    }
    return next->track.object ? AZ_OK : AZ_NOT_FOUND;
}

static bool open_source_file(Lease *next) {
    next->fd = open(next->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st = {0};
    if (next->fd < 0 || fstat(next->fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {

        return false;
    }
    next->device = st.st_dev;
    next->inode = st.st_ino;
    next->bytes = st.st_size;
    return true;
}

static void commit_source(Lease *next, Lease *lease, struct azn_analysis_source *out) {
    next->token = ++serial;
    struct azn_analysis_source result = {0};
    result.size = sizeof result;
    result.version = 1;
    result.token = next->token;
    result.epoch = next->epoch;
    result.load = next->load;
    result.source_kind = AZ_STATUS_SOURCE_STREAMING;
    memcpy(result.track_id, next->id, sizeof result.track_id);
    memcpy(result.audio_path, next->path, sizeof result.audio_path);
    *lease = *next;
    next->track = (Track) {0};
    next->fd = -1;
    *out = result;
}

static int publish_source(Lease *next, struct azn_analysis_source *out) {
    int rc = AZ_STALE;
    if (current_copy(next)) {
        AZ_MUTEX_GUARD(lock, mutex, true);
        if (!lock.held) rc = AZ_BUSY;
        else if (sources[next->source].revision == next->revision) {
            rc = AZ_LIMIT;
            for (unsigned li = 0; li < lease_count; ++li) {
                Lease *lease = &leases[li];
                if (lease->token || serial == UINT64_MAX) continue;
                commit_source(next, lease, out);
                rc = AZ_OK;
                break;
            }
        }
    }

    return rc;
}

static bool source_request_valid(unsigned deck, uint64_t epoch, uint64_t load, const struct azn_analysis_source *out) {
    return out && out->size >= sizeof *out && out->version == 1 && deck < 4 && epoch && load;
}
static int open_source(unsigned deck, uint64_t epoch, uint64_t load, struct azn_analysis_source *out) {
    if (!source_request_valid(deck, epoch, load, out)) return AZ_INVALID;
    if (!atomic_load_explicit(&(installed), memory_order_acquire)) return AZ_UNSUPPORTED;
    Lease next __attribute__((cleanup(lease_release))) = {.fd = -1};
    int found = find_source(deck, epoch, load, &next);
    if (found != AZ_OK) return found;
    if (!open_source_file(&next)) return AZ_STALE;
    return publish_source(&next, out);
}
static int current_source(uint64_t token) {
    if (!token) return AZ_INVALID;
    AZ_MUTEX_GUARD(lock, mutex, true);
    if (!lock.held) return AZ_BUSY;
    for (unsigned li = 0; li < lease_count; ++li) {
        const Lease *lease = &leases[li];
        if (lease->token == token)
            return sources[lease->source].revision == lease->revision && current_copy(lease) ? AZ_OK : AZ_STALE;
    }
    return AZ_STALE;
}
static void close_source(uint64_t token) {
    Lease retired __attribute__((cleanup(lease_release))) = {.fd = -1};
    {
        AZ_MUTEX_LOCK(lock, mutex);
        for (unsigned li = 0; li < lease_count; ++li) {
            Lease *lease = &leases[li];
            if (lease->token != token || !token) continue;
            retired = *lease;
            *lease = (Lease) {.fd = -1};
            break;
        }
    }
    /* Last counted release can delete native files and run callbacks. It must
     * happen outside our registry lock and away from audio/panel callbacks. */
    (void) retired;
}
#if defined(__linux__)
static int main_image(struct dl_phdr_info *info, size_t size, void *out) {
    (void) size;
    if (info->dlpi_name && *info->dlpi_name) return 0;
    *((uintptr_t *) (out)) = info->dlpi_addr;
    return 1;
}
#endif
static int install_source(int build, azn_analysis_patch_fn patch) {
#if defined(__linux__) && defined(__aarch64__)
    AZ_MUTEX_LOCK(lock, installation);
    if (build != 2 || !patch) return AZ_UNSUPPORTED;
    if (atomic_load_explicit(&(installed), memory_order_acquire)) return AZ_OK;
    dl_iterate_phdr(main_image, &bias);
    const uint32_t words[] = {0xa9b57bfdu, 0x910003fdu, 0x6d0527e8u, 0x0f000408u};
    const uintptr_t address = bias + 0xd39610;
    if (memcmp(((const void *) (address)), words, sizeof words)) return AZ_UNSUPPORTED;
    if (!patch(address, ((void *) (wait_decrypt)), &original_wait, "sdk-beatport-analysis") || !original_wait)
        return AZ_FAILED;
    atomic_store_explicit(&(installed), true, memory_order_release);
    return AZ_OK;
#else
    (void) build;
    (void) patch;
    return AZ_UNSUPPORTED;
#endif
}
const struct azn_analysis_source_api *azn_analysis_sources(void) {
    static const struct azn_analysis_source_api api = {sizeof api, 1, install_source, open_source, current_source, close_source};
    return &api;
}

#ifdef AZ_ANALYSIS_SOURCE_TESTING
void azn_analysis_test_initialize(void *original) {
    original_wait = original;
    atomic_store_explicit(&installed, true, memory_order_release);
}
int azn_analysis_test_wait(uintptr_t owner, const void *weak, int generation) {
    return wait_decrypt(owner, weak, generation);
}
#endif
