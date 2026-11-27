/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "source_provider_lifetime.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define SOURCE_INFO_CTOR 0x0141a2d0u
#define SOURCE_INFO_DTOR 0x0141d238u
#define SOURCE_INFO_DELETE 0x0141c390u
#define SOURCE_INFO_DTOR_SLOT 0x02bc7320u
#define SOURCE_INFO_DELETE_SLOT 0x02bc7328u
#define SOURCE_INFO_PATH 0x28u
#define BROWSE_PREVIEW_SOURCE_RETURN 0x0157825cu
#define SOURCE_FIRMWARE_200 2
#define PATH_SLOTS 128u
#define PATH_EXCLUSIVE (1u << 31)
#define ANALYSIS_DROP 0x00cfba98u
#define ANALYSIS_MANAGER_VT 0x02ab8970u
#define ANALYSIS_CONTROLLERS 0x328u
#define ANALYSIS_COUNT 0x338u
#define CONTROLLER_TRACK 0xb0u
#define CONTROLLER_REQUESTERS 0xc8u
#define CONTROLLER_REQUEST_COUNT 0xd8u
#define RAW_TRACK_BYTES 12u
#define REQUESTER_BYTES 12u
#define ANALYSIS_SLOTS 128u

struct path_slot {
    _Atomic uint64_t ticket, counts;
    _Atomic unsigned readers;
    _Atomic uint32_t preview_opened;
    _Atomic bool uncertain;
    char path[AZ_SOURCE_PATH_BYTES];
};

static struct path_slot paths[PATH_SLOTS];
static pthread_mutex_t path_worker = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool path_observing;
static uint64_t path_serial;
static void (*source_construct)(void *, const void *, void *);
static void (*source_destroy)(void *);
static void (*source_delete)(void *);

static bool path_enabled(void) {
    return atomic_load_explicit(&path_observing, memory_order_acquire);
}

static uint64_t current_ticket(const struct path_slot *slot) {
    return atomic_load_explicit(&slot->ticket, memory_order_acquire);
}

static void drop_path(struct path_slot *slot) {
    atomic_fetch_sub_explicit(&slot->readers, 1, memory_order_release);
}

static bool acquire_reader(struct path_slot *slot) {
    unsigned readers = atomic_load_explicit(&slot->readers, memory_order_relaxed);
    while (!(readers & PATH_EXCLUSIVE)) {
        if (atomic_compare_exchange_weak_explicit(&slot->readers, &readers, readers + 1,
                                                  memory_order_acquire, memory_order_relaxed))
            return true;
    }
    return false;
}

static bool borrow_path(struct path_slot *slot, const char *path) {
    uint64_t ticket = current_ticket(slot);
    if (!ticket || !acquire_reader(slot))
        return false;
    if (current_ticket(slot) == ticket && !strcmp(slot->path, path))
        return true;
    drop_path(slot);
    return false;
}

static struct path_slot *borrow_source(const char *path) {
    if (!path_enabled() || !path || !*path)
        return NULL;
    for (unsigned i = 0; i < PATH_SLOTS; ++i) {
        if (borrow_path(&paths[i], path))
            return &paths[i];
    }
    return NULL;
}

static struct path_slot *begin_source_construction(const char *path) {
    struct path_slot *slot = borrow_source(path);
    if (!slot)
        return NULL;
    uint64_t prior = atomic_fetch_add_explicit(&slot->counts, 1, memory_order_acq_rel);
    if ((uint32_t) prior == UINT32_MAX)
        atomic_store_explicit(&slot->uncertain, true, memory_order_release);
    return slot;
}

static void source_opened(struct path_slot *slot) {
    uint64_t prior = atomic_fetch_add_explicit(&slot->counts, UINT64_C(1) << 32, memory_order_acq_rel);
    if ((uint32_t) (prior >> 32) == UINT32_MAX)
        atomic_store_explicit(&slot->uncertain, true, memory_order_release);
}

static void source_closed(struct path_slot *slot) {
    uint64_t prior = atomic_load_explicit(&slot->counts, memory_order_acquire);
    while ((uint32_t) prior) {
        if (atomic_compare_exchange_weak_explicit(&slot->counts, &prior, prior - 1,
                                                  memory_order_acq_rel, memory_order_acquire))
            return;
    }
    atomic_store_explicit(&slot->uncertain, true, memory_order_release);
}

static const char *source_path(const void *object, size_t offset) {
    const char *path;
    memcpy(&path, (const unsigned char *) object + offset, sizeof path);
    return path;
}

static void source_preview_opened(struct path_slot *slot) {
    if (atomic_fetch_add_explicit(&slot->preview_opened, 1, memory_order_acq_rel) == UINT32_MAX)
        atomic_store_explicit(&slot->uncertain, true, memory_order_release);
}

struct source_construction {
    struct path_slot *slot;
    bool completed;
};

static void finish_source_construction(struct source_construction *construction) {
    if (!construction->slot)
        return;
    if (!construction->completed)
        source_closed(construction->slot);
    drop_path(construction->slot);
}

static void source_from(void *object, const void *file, void *disposer, uintptr_t caller) {
    struct path_slot *slot = begin_source_construction(source_path(file, 0));
    /* Keep a tentative owner during construction. A native unwind leaves no
     * constructed object to deliver the normal release. */
    struct source_construction construction __attribute__((cleanup(finish_source_construction))) = {.slot = slot};
    source_construct(object, file, disposer);
    if (!slot)
        return;
    construction.completed = true;
    source_opened(slot);
    if (caller == BROWSE_PREVIEW_SOURCE_RETURN)
        source_preview_opened(slot);
}

static void native_source_construct(void *object, const void *file, void *disposer) {
    uintptr_t caller = (uintptr_t) __builtin_extract_return_addr(__builtin_return_address(0));
    source_from(object, file, disposer, caller);
}

static void close_native_source(void *object, void (*destroy)(void *)) {
    struct path_slot *slot = borrow_source(source_path(object, SOURCE_INFO_PATH));
    destroy(object);
    if (slot) {
        source_closed(slot);
        drop_path(slot);
    }
}

static void native_source_destroy(void *object) {
    close_native_source(object, source_destroy);
}

static void native_source_delete(void *object) {
    close_native_source(object, source_delete);
}

static bool install_path_slots(azspd_patch patch) {
    source_destroy = (__typeof__(source_destroy)) (uintptr_t) SOURCE_INFO_DTOR;
    source_delete = (__typeof__(source_delete)) (uintptr_t) SOURCE_INFO_DELETE;
    return patch(SOURCE_INFO_DTOR_SLOT, SOURCE_INFO_DTOR, (uintptr_t) native_source_destroy) &&
           patch(SOURCE_INFO_DELETE_SLOT, SOURCE_INFO_DELETE, (uintptr_t) native_source_delete);
}

int azspd_paths_install(int firmware, azspd_code_patch code, azspd_patch slot) {
    if (firmware != SOURCE_FIRMWARE_200)
        return AZ_UNSUPPORTED;
    if (!code || !slot)
        return AZ_INVALID;
    if (path_enabled())
        return AZ_BUSY;
    if (!install_path_slots(slot))
        return AZ_UNSUPPORTED;
    if (code(SOURCE_INFO_CTOR, (void *) native_source_construct, (void **) &source_construct) != 1)
        return AZ_UNSUPPORTED;
    atomic_store_explicit(&path_observing, true, memory_order_release);
    return AZ_OK;
}

static struct path_slot *find_ticket_locked(uint64_t ticket) {
    if (!ticket)
        return NULL;
    for (unsigned i = 0; i < PATH_SLOTS; ++i) {
        if (current_ticket(&paths[i]) == ticket)
            return &paths[i];
    }
    return NULL;
}

static bool slot_inactive(const struct path_slot *slot) {
    return !current_ticket(slot) && !atomic_load_explicit(&slot->readers, memory_order_acquire);
}

static struct path_slot *reserve_path_locked(const char *path) {
    struct path_slot *unused = NULL;
    for (unsigned i = 0; i < PATH_SLOTS; ++i) {
        struct path_slot *slot = &paths[i];
        if (current_ticket(slot) && !strcmp(slot->path, path))
            return NULL;
        if (!unused && slot_inactive(slot))
            unused = slot;
    }
    return unused;
}

static int watch_path_locked(const char *path, size_t length, uint64_t *ticket) {
    struct path_slot *slot = reserve_path_locked(path);
    if (!slot || path_serial == UINT64_MAX)
        return AZ_LIMIT;
    memcpy(slot->path, path, length + 1);
    atomic_store_explicit(&slot->counts, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->preview_opened, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->uncertain, false, memory_order_relaxed);
    *ticket = ++path_serial;
    atomic_store_explicit(&slot->ticket, *ticket, memory_order_release);
    return AZ_OK;
}

int azspd_path_watch(const char *path, uint64_t *ticket) {
    if (!path_enabled())
        return AZ_UNSUPPORTED;
    if (!path || path[0] != '/' || !ticket)
        return AZ_INVALID;
    size_t length = strnlen(path, AZ_SOURCE_PATH_BYTES);
    if (length == AZ_SOURCE_PATH_BYTES)
        return AZ_INVALID;
    pthread_mutex_lock(&path_worker);
    int status = watch_path_locked(path, length, ticket);
    pthread_mutex_unlock(&path_worker);
    return status;
}

static struct azspd_path_state read_path(const struct path_slot *slot) {
    uint64_t counts = atomic_load_explicit(&slot->counts, memory_order_acquire);
    return (struct azspd_path_state) {
        .opened = (uint32_t) (counts >> 32),
        .owners = (uint32_t) counts,
        .preview_opened = atomic_load_explicit(&slot->preview_opened, memory_order_acquire),
        .uncertain = atomic_load_explicit(&slot->uncertain, memory_order_acquire),
    };
}

int azspd_path_status(uint64_t ticket, struct azspd_path_state *out) {
    if (!out)
        return AZ_INVALID;
    pthread_mutex_lock(&path_worker);
    struct path_slot *slot = find_ticket_locked(ticket);
    if (slot)
        *out = read_path(slot);
    pthread_mutex_unlock(&path_worker);
    return slot ? AZ_OK : AZ_STALE;
}

static bool stop_path(struct path_slot *slot) {
    unsigned idle = 0;
    if (!atomic_compare_exchange_strong_explicit(&slot->readers, &idle, PATH_EXCLUSIVE,
                                                 memory_order_acquire, memory_order_relaxed))
        return false;
    struct azspd_path_state state = read_path(slot);
    bool settled = !state.owners && !state.uncertain;
    if (settled)
        atomic_store_explicit(&slot->ticket, 0, memory_order_release);
    atomic_store_explicit(&slot->readers, 0, memory_order_release);
    return settled;
}

int azspd_path_unwatch(uint64_t ticket) {
    pthread_mutex_lock(&path_worker);
    struct path_slot *slot = find_ticket_locked(ticket);
    int status = slot ? (stop_path(slot) ? AZ_OK : AZ_BUSY) : AZ_STALE;
    pthread_mutex_unlock(&path_worker);
    return status;
}

struct analysis_watch {
    uint64_t ticket;
    unsigned char raw_id[RAW_TRACK_BYTES];
    unsigned char requester[REQUESTER_BYTES];
    bool terminal;
};

struct analysis_owner {
    const void *object, *shared_count;
};

static struct analysis_watch analyses[ANALYSIS_SLOTS];
static pthread_mutex_t analysis_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool analysis_observing;
static uint64_t analysis_serial;
static void (*remove_analysis_controller)(void *, const unsigned char *);

static struct analysis_watch *analysis_ticket_locked(uint64_t ticket) {
    if (!ticket)
        return NULL;
    for (unsigned i = 0; i < ANALYSIS_SLOTS; ++i) {
        if (analyses[i].ticket == ticket)
            return &analyses[i];
    }
    return NULL;
}

static struct analysis_watch *analysis_id_locked(const unsigned char *id) {
    for (unsigned i = 0; i < ANALYSIS_SLOTS; ++i) {
        if (analyses[i].ticket && !memcmp(analyses[i].raw_id, id, RAW_TRACK_BYTES))
            return &analyses[i];
    }
    return NULL;
}

static struct analysis_watch current_analysis(const unsigned char *id) {
    pthread_mutex_lock(&analysis_lock);
    const struct analysis_watch *watch = analysis_id_locked(id);
    struct analysis_watch current = watch ? *watch : (struct analysis_watch) {0};
    pthread_mutex_unlock(&analysis_lock);
    return current;
}

static void terminal_analysis(uint64_t ticket) {
    pthread_mutex_lock(&analysis_lock);
    struct analysis_watch *watch = analysis_ticket_locked(ticket);
    if (watch)
        watch->terminal = true;
    pthread_mutex_unlock(&analysis_lock);
}

static bool analysis_local_medium(uint16_t media) {
    if (media <= 0xff)
        return false;
    unsigned storage = media & 0xff;
    return storage == 0x02 || storage == 0x22 || storage == 0x42;
}

static bool analysis_folder_id(const unsigned char *id) {
    if (!id)
        return false;
    struct azspd_track_id track = {0};
    memcpy(&track, id, RAW_TRACK_BYTES);
    return track.player == 1 && track.kind == 2 && track.track && analysis_local_medium(track.media);
}

/* The four native completion/cancellation callers already own the manager
 * mutex. They pass a still-owned controller vector to the stock removal. */
static int find_controller(const void *manager, const unsigned char *id, const unsigned char **out) {
    unsigned count;
    uintptr_t vector;
    memcpy(&count, (const unsigned char *) manager + ANALYSIS_COUNT, sizeof(count));
    memcpy(&vector, (const unsigned char *) manager + ANALYSIS_CONTROLLERS, sizeof(vector));
    const struct analysis_owner *controllers = (const struct analysis_owner *) vector;
    if (count > 4096 || (count && !controllers))
        return -1;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *owner = controllers[i].object;
        if (owner && !memcmp(owner + CONTROLLER_TRACK, id, RAW_TRACK_BYTES)) {
            *out = owner;
            return 1;
        }
    }
    return 0;
}

static bool controller_has_requester(const unsigned char *owner, const unsigned char *requester) {
    const unsigned char *entries;
    unsigned count;
    memcpy(&entries, owner + CONTROLLER_REQUESTERS, sizeof(entries));
    memcpy(&count, owner + CONTROLLER_REQUEST_COUNT, sizeof(count));
    if (!entries || count > 4096)
        return false;
    for (unsigned i = 0; i < count; ++i) {
        if (!memcmp(entries + i * REQUESTER_BYTES, requester, REQUESTER_BYTES))
            return true;
    }
    return false;
}

static uint64_t terminal_candidate(const void *manager, const unsigned char *id) {
    if (!atomic_load_explicit(&analysis_observing, memory_order_acquire) || !analysis_folder_id(id))
        return 0;
    if ((uintptr_t) source_path(manager, 0) != ANALYSIS_MANAGER_VT)
        return 0;
    struct analysis_watch watch = current_analysis(id);
    const unsigned char *owner;
    if (!watch.ticket || find_controller(manager, id, &owner) != 1)
        return 0;
    return controller_has_requester(owner, watch.requester) ? watch.ticket : 0;
}

static void observe_analysis_removal(void *manager, const unsigned char *id) {
    uint64_t ticket = terminal_candidate(manager, id);
    unsigned char raw[RAW_TRACK_BYTES];
    if (ticket)
        memcpy(raw, id, sizeof(raw));
    remove_analysis_controller(manager, id);
    const unsigned char *ignored;
    if (ticket && find_controller(manager, raw, &ignored) == 0)
        terminal_analysis(ticket);
}

int azspd_analysis_install(int firmware, azspd_code_patch code) {
    if (firmware != SOURCE_FIRMWARE_200)
        return AZ_UNSUPPORTED;
    if (!code)
        return AZ_INVALID;
    if (atomic_load_explicit(&analysis_observing, memory_order_acquire))
        return AZ_BUSY;
    if (code(ANALYSIS_DROP, (void *) observe_analysis_removal, (void **) &remove_analysis_controller) != 1)
        return AZ_UNSUPPORTED;
    atomic_store_explicit(&analysis_observing, true, memory_order_release);
    return AZ_OK;
}

static struct analysis_watch *unused_analysis_locked(void) {
    for (unsigned i = 0; i < ANALYSIS_SLOTS; ++i) {
        if (!analyses[i].ticket)
            return &analyses[i];
    }
    return NULL;
}

static int register_analysis_locked(const unsigned char *id, const unsigned char *requester,
                                    uint64_t *ticket) {
    if (analysis_id_locked(id))
        return AZ_BUSY;
    struct analysis_watch *watch = unused_analysis_locked();
    if (!watch || analysis_serial == UINT64_MAX)
        return AZ_LIMIT;
    memcpy(watch->raw_id, id, RAW_TRACK_BYTES);
    memcpy(watch->requester, requester, REQUESTER_BYTES);
    watch->terminal = false;
    watch->ticket = ++analysis_serial;
    *ticket = watch->ticket;
    return AZ_OK;
}

int azspd_analysis_watch(const unsigned char raw_id[RAW_TRACK_BYTES],
                         const unsigned char requester[REQUESTER_BYTES], uint64_t *ticket) {
    if (!atomic_load_explicit(&analysis_observing, memory_order_acquire))
        return AZ_UNSUPPORTED;
    if (!ticket || !requester || !analysis_folder_id(raw_id))
        return AZ_INVALID;
    pthread_mutex_lock(&analysis_lock);
    int status = register_analysis_locked(raw_id, requester, ticket);
    pthread_mutex_unlock(&analysis_lock);
    return status;
}

static int analysis_status_locked(const struct analysis_watch *watch) {
    if (!watch)
        return AZ_STALE;
    return watch->terminal ? AZ_OK : AZ_PENDING;
}

int azspd_analysis_status(uint64_t ticket) {
    pthread_mutex_lock(&analysis_lock);
    int status = analysis_status_locked(analysis_ticket_locked(ticket));
    pthread_mutex_unlock(&analysis_lock);
    return status;
}

void azspd_analysis_forget(uint64_t ticket) {
    pthread_mutex_lock(&analysis_lock);
    struct analysis_watch *watch = analysis_ticket_locked(ticket);
    if (analysis_status_locked(watch) == AZ_OK)
        watch->ticket = 0;
    pthread_mutex_unlock(&analysis_lock);
}
