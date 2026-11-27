/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by azmod.c after the Community host. Deferred firmware objects
 * stay on the message thread. Workers exchange copied requests and tickets. */
#include "source_provider_cache.h"
#include "source_provider_dispatch.h"
#include "source_provider_lifetime.h"
#include "source_provider_query.h"
#include "source_provider_stream.h"
#include "../common/azstatus_native.h"

#include <dlfcn.h>

enum spr_slot {
    SPR_PLAYLISTS,
    SPR_SEARCH,
    SPR_LOAD_1,
    SPR_PREVIEW = SPR_LOAD_1 + 4,
    SPR_REAP,
    SPR_SLOT_COUNT
};

enum spr_start_state {
    SPR_BOOTING,
    SPR_RUNNING,
    SPR_ABORTED
};

struct spr_task {
    cm_mod *provider;
    unsigned slot;
    uint64_t serial;
    enum az_source_storage_mode mode;
    union {
        struct azspd_search search;
        struct azspd_load load;
    } native;
};

struct spr_result {
    uint64_t ticket;
    int status;
    uint32_t rows;
    struct azspd_target target;
    struct azspd_load load;
    struct spr_stream *stream;
};

struct spr_stream {
    struct spr_stream *next;
    uint32_t id;
    struct azsp_stream *lease;
    uint64_t path_watch;
    _Atomic unsigned pins;
    _Atomic int retired;
    uint32_t preview_credit; /* message-thread accounting under a stream pin */
    int ready;
};

struct spr_deck_use {
    struct spr_deck_use *next;
    struct spr_stream *stream;
    struct azspd_deck_watch watch;
    uint32_t preview_before;
    int preview_accepted;
};

static pthread_mutex_t spr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t spr_work = PTHREAD_COND_INITIALIZER;
static struct spr_task spr_tasks[SPR_SLOT_COUNT];
static struct spr_result spr_results[SPR_SLOT_COUNT];
static _Atomic uint64_t spr_latest[SPR_SLOT_COUNT];
static _Atomic(cm_mod *) spr_active;
static _Atomic enum spr_start_state spr_start_state;
static struct azsp_cache *spr_cache;
static struct spr_stream *spr_streams; /* owned by the load worker */
static struct spr_deck_use *spr_uses;  /* owned by the message thread */
static uint64_t spr_claimed_load_epochs[SPR_PREVIEW - SPR_LOAD_1]; /* message thread */
static const struct azstatus_native_v1 *spr_native_api;
static uint64_t spr_serial;          /* owned by the message thread */
static unsigned spr_pending, spr_completed;

static void spr_unpin(struct spr_stream *stream) {
    if (stream)
        atomic_fetch_sub_explicit(&stream->pins, 1, memory_order_acq_rel);
}

static void spr_queue_reap_locked(void) {
    spr_tasks[SPR_REAP].slot = SPR_REAP;
    spr_pending |= 1u << SPR_REAP;
    pthread_cond_broadcast(&spr_work);
}

/* Caller owns a pin. Retirement allows the worker to try disposal after the
 * final pin drops, subject to the analyzer and native file owners. */
static void spr_retire_locked(struct spr_stream *stream) {
    if (!stream)
        return;
    atomic_store_explicit(&stream->retired, 1, memory_order_release);
    spr_unpin(stream);
    spr_queue_reap_locked();
}

static void spr_drop_result(unsigned slot) {
    if (spr_completed & (1u << slot))
        spr_retire_locked(spr_results[slot].stream);
    spr_completed &= ~(1u << slot);
}

static int spr_cancelled(void *data) {
    const struct spr_task *task = data;
    return atomic_load_explicit(&spr_latest[task->slot], memory_order_acquire) != task->serial;
}

static void spr_queue(struct spr_task *task) {
    unsigned slot = task->slot;
    task->serial = ++spr_serial;
    pthread_mutex_lock(&spr_lock);
    atomic_store_explicit(&spr_latest[slot], task->serial, memory_order_release);
    spr_tasks[slot] = *task;
    spr_pending |= 1u << slot;
    spr_drop_result(slot);
    pthread_cond_broadcast(&spr_work);
    pthread_mutex_unlock(&spr_lock);
}

static bool spr_wait(const struct timespec *deadline) {
    if (deadline)
        return pthread_cond_timedwait(&spr_work, &spr_lock, deadline) == 0;
    pthread_cond_wait(&spr_work, &spr_lock);
    return true;
}

static struct spr_task spr_next(unsigned mask, const struct timespec *deadline) {
    pthread_mutex_lock(&spr_lock);
    while (!(spr_pending & mask) && spr_wait(deadline)) {}
    struct spr_task task = {.slot = SPR_REAP};
    if (spr_pending & mask) {
        unsigned slot = (unsigned) __builtin_ctz(spr_pending & mask);
        task = spr_tasks[slot];
        spr_pending &= ~(1u << slot);
    }
    pthread_mutex_unlock(&spr_lock);
    return task;
}

static bool spr_wait_for_start(void) {
    pthread_mutex_lock(&spr_lock);
    while (atomic_load_explicit(&spr_start_state, memory_order_relaxed) == SPR_BOOTING)
        pthread_cond_wait(&spr_work, &spr_lock);
    bool running = atomic_load_explicit(&spr_start_state, memory_order_relaxed) == SPR_RUNNING;
    pthread_mutex_unlock(&spr_lock);
    return running;
}

static void spr_reply(struct spr_task *task, const struct spr_result *result) {
    pthread_mutex_lock(&spr_lock);
    if (!spr_cancelled(task)) {
        spr_results[task->slot] = *result;
        spr_completed |= 1u << task->slot;
    } else
        spr_retire_locked(result->stream);
    pthread_mutex_unlock(&spr_lock);
}

static int spr_take_result(unsigned slot, struct spr_result *out) {
    unsigned bit = 1u << slot;
    pthread_mutex_lock(&spr_lock);
    int ready = !!(spr_completed & bit);
    if (ready) {
        *out = spr_results[slot];
        spr_completed &= ~bit;
    }
    pthread_mutex_unlock(&spr_lock);
    return ready;
}

static void spr_retire(struct spr_stream *stream) {
    if (!stream)
        return;
    pthread_mutex_lock(&spr_lock);
    spr_retire_locked(stream);
    pthread_mutex_unlock(&spr_lock);
}

static const char *spr_query_provider(void) {
    cm_mod *provider = atomic_load_explicit(&spr_active, memory_order_acquire);
    return provider ? provider->id : NULL;
}

static bool spr_browse_owned(uint64_t media) {
    return media && media == azsp_carrier_media() && spr_query_provider();
}

static void *spr_native_search_view(void) {
    if (!atomic_load_explicit(&g_sv_vis, memory_order_acquire))
        return NULL;
    return (void *) (uintptr_t) atomic_load_explicit(&g_sv_obj, memory_order_acquire);
}

static bool spr_track_owned(const struct azspd_track_id *id) {
    uint64_t media = azsp_carrier_media();
    return media && id->player == (uint32_t) media && id->media == (uint16_t) (media >> 32);
}

static bool spr_schedule_search(const struct azspd_search *event) {
    cm_mod *provider = atomic_load_explicit(&spr_active, memory_order_acquire);
    if (!provider)
        return false;
    struct spr_task task = {.provider = provider, .slot = SPR_SEARCH, .native.search = *event};
    spr_queue(&task);
    return true;
}

static bool spr_schedule_load(const struct azspd_load *event) {
    cm_mod *provider = atomic_load_explicit(&spr_active, memory_order_acquire);
    if (!provider || !event->deck || event->deck > 4)
        return false;
    struct spr_task task = {.provider = provider, .slot = SPR_LOAD_1 + event->deck - 1, .mode = azsp_source_storage_mode(provider), .native.load = *event};
    spr_queue(&task);
    return true;
}

static bool spr_schedule_preview(const struct azspd_preview *event) {
    cm_mod *provider = atomic_load_explicit(&spr_active, memory_order_acquire);
    if (!provider)
        return false;
    struct spr_task task = {.provider = provider, .slot = SPR_PREVIEW, .mode = AZ_SOURCE_STREAMING, .native.load = {.ticket = event->ticket, .id = event->id}};
    spr_queue(&task);
    return true;
}

static void spr_clear_slot(unsigned slot) {
    atomic_store_explicit(&spr_latest[slot], 0, memory_order_release);
    spr_pending &= ~(1u << slot);
    spr_drop_result(slot);
}

/* Native selection and result replay both run on the message thread. */
static void spr_clear(void) {
    pthread_mutex_lock(&spr_lock);
    atomic_store_explicit(&spr_active, NULL, memory_order_release);
    uint64_t ticket = spr_tasks[SPR_SEARCH].native.search.ticket;
    spr_tasks[SPR_SEARCH].native.search.ticket = 0;
    spr_clear_slot(SPR_PLAYLISTS);
    spr_clear_slot(SPR_SEARCH);
    pthread_mutex_unlock(&spr_lock);
    if (ticket)
        azspd_cancel_search(ticket);
}

static int spr_select(void *source, void *model) {
    atomic_store_explicit(&spr_active, source, memory_order_release);
    int status = azsp_carrier_select(model);
    if (status != AZ_OK) {
        spr_clear();
        return status;
    }
    struct spr_task task = {.provider = source, .slot = SPR_PLAYLISTS};
    spr_queue(&task);
    return AZ_OK;
}

static int spr_catalog_retry(int status) {
    if (status != AZ_BUSY)
        return 0;
    const struct timespec delay = {.tv_nsec = 20000000};
    (void) nanosleep(&delay, NULL);
    return 1;
}

static int spr_publish_page(struct spr_task *task, const struct azsp_request *request,
                            const struct azsp_result *page) {
    int status;
    do {
        if (spr_cancelled(task))
            return AZ_CANCELLED;
        status = azsp_catalog_apply(task->provider->id, request, page, NULL);
    } while (spr_catalog_retry(status));
    return status;
}

static int spr_get_page(struct spr_task *task, const struct azsp_request *request,
                        struct azsp_result *page) {
    if (spr_cancelled(task))
        return AZ_CANCELLED;
    if (request->operation == AZSP_SEARCH && !request->value.query[0]) {
        *page = (struct azsp_result) {0};
        return AZ_OK;
    }
    return azsp_request_source(task->provider, request, page);
}

static int spr_read_page(struct spr_task *task, struct azsp_request *request, struct azsp_result *page) {
    int status = spr_get_page(task, request, page);
    if (status != AZ_OK)
        return status;
    status = spr_publish_page(task, request, page);
    request->offset += page->page.count;
    return status;
}

static int spr_list_tracks(struct spr_task *task, const az_source_playlist_v1 *playlist) {
    struct azsp_request request = {.operation = AZSP_PLAYLIST_TRACKS, .capacity = AZ_SOURCE_PAGE_MAX};
    memcpy(request.value.playlist_id, playlist->id, sizeof request.value.playlist_id);
    struct azsp_result page;
    int status;
    do {
        status = spr_read_page(task, &request, &page);
    } while (status == AZ_OK && page.page.has_more);
    return status;
}

static int spr_playlist_page(struct spr_task *task, struct azsp_request *request, struct azsp_result *page) {
    int status = spr_read_page(task, request, page);
    if (status != AZ_OK)
        return status;
    for (uint32_t i = 0; i < page->page.count; ++i) {
        status = spr_list_tracks(task, &page->value.playlists[i]);
        if (status != AZ_OK)
            break;
    }
    return status;
}

static int spr_list(struct spr_task *task) {
    struct azsp_request request = {.operation = AZSP_PLAYLISTS, .capacity = AZ_SOURCE_PAGE_MAX};
    struct azsp_result page;
    int status;
    do {
        status = spr_playlist_page(task, &request, &page);
    } while (status == AZ_OK && page.page.has_more);
    return status;
}

static int spr_search(struct spr_task *task, uint32_t *rows) {
    struct azsp_request request = {.operation = AZSP_SEARCH, .capacity = AZ_SOURCE_PAGE_MAX};
    memcpy(request.value.query, task->native.search.query, sizeof request.value.query);
    struct azsp_result page;
    int status;
    do {
        status = spr_read_page(task, &request, &page);
    } while (status == AZ_OK && page.page.has_more);
    *rows = request.offset;
    return status;
}

static void spr_discard_cache(void) {
    azsp_cache_discard(spr_cache);
    spr_cache = NULL;
}

static int spr_catalog_init(void) {
    if (!az_storage_sql_ready())
        return AZ_UNSUPPORTED;
    int status = azsp_cache_open("/tmp/azmod-sources", &spr_cache);
    if (status != AZ_OK)
        return status;
    const char *root = azsp_cache_root(spr_cache);
    status = azsp_catalog_create(root, az_storage_sql(), az_storage_key_database);
    if (status != AZ_OK)
        spr_discard_cache();
    return status;
}

static void *spr_catalog_worker(void *unused) {
    (void) unused;
    if (!spr_wait_for_start())
        return NULL;
    for (;;) {
        struct spr_task task = spr_next((1u << SPR_PLAYLISTS) | (1u << SPR_SEARCH), NULL);
        if (task.slot == SPR_PLAYLISTS) {
            int rc = spr_list(&task);
            if (rc != AZ_OK && rc != AZ_CANCELLED)
                drag_log("source provider playlist request failed: %d\n", rc);
            continue;
        }
        struct spr_result result = {.ticket = task.native.search.ticket};
        result.status = spr_search(&task, &result.rows);
        spr_reply(&task, &result);
    }
}

static int spr_lookup_track(struct spr_task *task, az_source_track_v1 *track) {
    char owner[33];
    int status;
    do {
        if (spr_cancelled(task))
            return AZ_CANCELLED;
        status = azsp_catalog_track(task->native.load.id.track, owner, track);
    } while (spr_catalog_retry(status));
    if (status == AZ_OK && strcmp(owner, task->provider->id))
        return AZ_STALE;
    return status;
}

static int spr_import_track(struct spr_task *task, const az_source_track_v1 *track,
                            struct azspd_target *target) {
    struct azspi_provider_load request = {
        .source = task->provider,
        .provider = task->provider->id,
        .staging_root = azsp_carrier_root(),
        .track = track,
        .firmware = g_ep147_build,
        .cancelled = spr_cancelled,
        .context = task,
    };
    struct azspi_imported_track imported;
    int status = azspi_import_provider(&request, &imported);
    if (status == AZ_OK) {
        *target = (struct azspd_target) {
            .id = {.player = task->native.load.id.player, .media = imported.media, .kind = 1, .track = imported.id},
            .category = 4, /* native Track category */
        };
    }
    return status;
}

static struct spr_stream *spr_stream_lookup(uint32_t id) {
    for (struct spr_stream *entry = spr_streams; entry; entry = entry->next) {
        if (entry->id == id)
            return entry;
    }
    return NULL;
}

static int spr_prepare_stream(struct spr_task *task, const az_source_track_v1 *track,
                              struct spr_stream *entry) {
    struct azsp_stage *stage;
    int status = azsp_prepare_source(task->provider, track, azsp_carrier_root(), AZ_SOURCE_STREAMING, &stage);
    if (status != AZ_OK)
        return status;
    status = azspd_path_watch(azsp_stage_path(stage), &entry->path_watch);
    if (status == AZ_OK)
        status = azsp_stream_prepare(g_ep147_build, azsp_carrier_media(), entry->id, track, stage,
                                     spr_cancelled, task, &entry->lease);
    azsp_stage_release(stage);
    return status;
}

static int spr_use_stream(struct spr_stream *stream, struct spr_result *result) {
    if (!stream->ready)
        return AZ_BUSY;
    atomic_fetch_add_explicit(&stream->pins, 1, memory_order_relaxed);
    result->stream = stream;
    return AZ_OK;
}

static int spr_keep_stream(struct spr_stream *entry, int status, struct spr_result *result) {
    entry->next = spr_streams;
    spr_streams = entry;
    entry->ready = status == AZ_OK;
    if (entry->ready)
        return spr_use_stream(entry, result);
    atomic_store_explicit(&entry->retired, 1, memory_order_release);
    pthread_mutex_lock(&spr_lock);
    spr_queue_reap_locked();
    pthread_mutex_unlock(&spr_lock);
    return status;
}

static int spr_stream_track(struct spr_task *task, const az_source_track_v1 *track, struct spr_result *result) {
    uint32_t id = task->native.load.id.track;
    struct spr_stream *entry = spr_stream_lookup(id);
    if (entry)
        return spr_use_stream(entry, result);
    entry = calloc(1, sizeof *entry);
    if (!entry)
        return AZ_FAILED;
    entry->id = id;
    int status = spr_prepare_stream(task, track, entry);
    if (entry->lease)
        return spr_keep_stream(entry, status, result);
    if (entry->path_watch)
        (void) azspd_path_unwatch(entry->path_watch);
    free(entry);
    return status;
}

static int spr_prepare(struct spr_task *task, struct spr_result *result) {
    az_source_track_v1 track;
    int status = spr_lookup_track(task, &track);
    if (status != AZ_OK)
        return status;
    if (task->mode == AZ_SOURCE_IMPORT)
        return spr_import_track(task, &track, &result->target);
    return spr_stream_track(task, &track, result);
}

static int spr_last_reader(void *context) {
    struct spr_stream *stream = context;
    if (!stream->path_watch)
        return 1;
    if (azspd_path_unwatch(stream->path_watch) != AZ_OK)
        return 0;
    stream->path_watch = 0;
    return 1;
}

static int spr_ready_to_reclaim(struct spr_stream *stream) {
    return atomic_load_explicit(&stream->retired, memory_order_acquire) &&
           !atomic_load_explicit(&stream->pins, memory_order_acquire);
}

static int spr_reclaim_one(struct spr_stream *stream) {
    if (!spr_ready_to_reclaim(stream))
        return AZ_PENDING;
    stream->ready = 0;
    return azsp_stream_release_when(stream->lease, spr_last_reader, stream);
}

static int spr_reclaim(void) {
    int retry = 0;
    struct spr_stream **link = &spr_streams;
    while (*link) {
        struct spr_stream *stream = *link;
        int status = spr_reclaim_one(stream);
        if (status != AZ_OK) {
            retry |= status == AZ_BUSY;
            link = &stream->next;
            continue;
        }
        *link = stream->next;
        drag_log("source provider retired temporary track %u\n", stream->id);
        free(stream);
    }
    return retry;
}

static const struct timespec *spr_reap_deadline(int retry, struct timespec *deadline) {
    if (!retry || clock_gettime(CLOCK_REALTIME, deadline) != 0)
        return NULL;
    /* Native analysis and file owners do not signal spr_work when they settle. */
    deadline->tv_nsec += 250000000;
    if (deadline->tv_nsec >= 1000000000) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000;
    }
    return deadline;
}

static void *spr_load_worker(void *unused) {
    (void) unused;
    if (!spr_wait_for_start())
        return NULL;
    unsigned mask = ((1u << SPR_SLOT_COUNT) - 1) & ~((1u << SPR_LOAD_1) - 1);
    int retry = 0;
    for (;;) {
        struct timespec deadline;
        struct spr_task task = spr_next(mask, spr_reap_deadline(retry, &deadline));
        retry = spr_reclaim();
        if (task.slot == SPR_REAP)
            continue;
        struct spr_result result = {.ticket = task.native.load.ticket, .load = task.native.load};
        result.status = spr_prepare(&task, &result);
        spr_reply(&task, &result);
    }
    return NULL;
}

static const struct azstatus_native_v1 *spr_status_api(void) {
    if (spr_native_api)
        return spr_native_api;
    azstatus_native_fn get = (azstatus_native_fn) dlsym(RTLD_DEFAULT, "azstem_status_service_v1");
    const struct azstatus_native_v1 *api = get ? get() : NULL;
    if (!api || api->size < offsetof(struct azstatus_native_v1, track_matches) + sizeof api->track_matches)
        return NULL;
    if (api->load_seed && api->track_matches)
        spr_native_api = api;
    return spr_native_api;
}

static int spr_deck_seed(const struct azstatus_native_v1 *api, unsigned deck, struct azstatus_native_receipt *receipt) {
    *receipt = (struct azstatus_native_receipt) {.size = sizeof *receipt, .version = 1};
    return api && api->load_seed(deck - 1, receipt);
}

static struct spr_deck_use *spr_capture_use(const struct spr_result *result) {
    if (!result->stream || !result->stream->path_watch)
        return NULL;
    struct azstatus_native_receipt seed;
    (void) spr_deck_seed(spr_status_api(), result->load.deck, &seed);
    struct spr_deck_use *use = calloc(1, sizeof *use);
    if (use) {
        use->stream = result->stream;
        use->watch = (struct azspd_deck_watch) {
            .id = result->load.id,
            .deck = result->load.deck,
            .unit = seed.unit,
            .epoch = seed.epoch,
        };
    }
    return use;
}

/* Start with AZ_OK so an interrupted replay retains its observer and pin.
 * The existing acceptance and release checks decide whether it can retire. */
struct spr_replay {
    struct spr_deck_use *use;
    int status;
};

static void spr_record_use(struct spr_replay *replay) {
    struct spr_deck_use *use = replay->use;
    if (!use)
        return;
    if (replay->status != AZ_OK) {
        spr_retire(use->stream);
        free(use);
        return;
    }
    use->next = spr_uses;
    spr_uses = use;
}

static struct spr_deck_use *spr_capture_preview(const struct spr_result *result) {
    struct azspd_path_state state;
    if (!result->stream || azspd_path_status(result->stream->path_watch, &state) != AZ_OK)
        return NULL;
    struct spr_deck_use *use = calloc(1, sizeof *use);
    if (use) {
        use->stream = result->stream;
        use->preview_before = state.preview_opened;
    }
    return use;
}

static void spr_preview_accept(struct spr_deck_use *use, uint32_t opened) {
    if (use->preview_accepted)
        return;
    uint32_t credited = use->stream->preview_credit;
    if (credited < use->preview_before)
        credited = use->preview_before;
    if (opened > credited) {
        use->stream->preview_credit = credited + 1;
        use->preview_accepted = 1;
    }
}

static int spr_preview_released(struct spr_deck_use *use) {
    struct azspd_path_state state;
    if (azspd_path_status(use->stream->path_watch, &state) != AZ_OK || state.uncertain)
        return 0;
    spr_preview_accept(use, state.preview_opened);
    return use->preview_accepted && !state.owners;
}

static int spr_path_settled(const struct spr_stream *stream) {
    struct azspd_path_state state;
    return azspd_path_status(stream->path_watch, &state) == AZ_OK && state.opened && !state.owners && !state.uncertain;
}

static void spr_observe_load(struct azspd_deck_watch *watch, const struct azstatus_native_v1 *api,
                             const struct azstatus_native_receipt *seed) {
    uint64_t *claimed = &spr_claimed_load_epochs[watch->deck - 1];
    bool pending = watch->use == AZSPD_DECK_PENDING;
    /* Overlapping replays of the same track may match the same load epoch. */
    if (pending && seed->epoch <= *claimed)
        return;
    if (azspd_deck_observe(watch, api, seed) == AZSPD_DECK_ACCEPTED && pending)
        *claimed = seed->epoch;
}

static int spr_deck_released(struct spr_deck_use *use, const struct azstatus_native_v1 *api) {
    struct azstatus_native_receipt seed;
    if (spr_deck_seed(api, use->watch.deck, &seed))
        spr_observe_load(&use->watch, api, &seed);
    return use->watch.use == AZSPD_DECK_RETIRED && spr_path_settled(use->stream);
}

static int spr_use_released(struct spr_deck_use *use, const struct azstatus_native_v1 *api) {
    return use->watch.deck ? spr_deck_released(use, api) : spr_preview_released(use);
}

static void spr_watch_decks(void) {
    struct spr_deck_use **link = &spr_uses;
    if (!*link)
        return;
    const struct azstatus_native_v1 *api = spr_status_api();
    while (*link) {
        struct spr_deck_use *use = *link;
        if (spr_use_released(use, api)) {
            *link = use->next;
            spr_retire(use->stream);
            free(use);
        } else
            link = &use->next;
    }
}

static void spr_search_done(const struct spr_result *result) {
    if (result->status == AZ_OK) {
        int status = azspd_resume_search(result->ticket);
        drag_log("source provider search replay: ticket=%llu status=%d rows=%u\n",
                 (unsigned long long) result->ticket, status, result->rows);
    } else {
        azspd_cancel_search(result->ticket);
        drag_log("source provider search failed: %d\n", result->status);
    }
}

static void spr_load_failed(const struct spr_result *result, int status) {
    spr_retire(result->stream);
    if (azspd_load_current(result->ticket)) {
        azspd_cancel_load(result->ticket);
        drag_log("source provider load failed: %d\n", status);
        if (status != AZ_CANCELLED)
            (void) az_banner_strip("Source track could not be loaded", 1800);
    }
}

static void spr_load_done(const struct spr_result *result) {
    if (result->status != AZ_OK) {
        spr_load_failed(result, result->status);
        return;
    }
    struct spr_deck_use *use = spr_capture_use(result);
    if (result->stream && !use) {
        spr_load_failed(result, AZ_FAILED);
        return;
    }
    const struct azspd_target *target = result->target.id.track ? &result->target : NULL;
    int status;
    {
        struct spr_replay replay __attribute__((cleanup(spr_record_use))) = {.use = use, .status = AZ_OK};
        replay.status = azspd_resume_load(result->ticket, target);
        status = replay.status;
    }
    drag_log("source provider load replay: ticket=%llu status=%d target=%u\n",
             (unsigned long long) result->ticket, status, target ? target->id.track : 0);
}

static void spr_preview_done(const struct spr_result *result) {
    struct spr_deck_use *use = result->status == AZ_OK ? spr_capture_preview(result) : NULL;
    if (!use) {
        spr_retire(result->stream);
        azspd_cancel_preview(result->ticket);
        return;
    }
    struct spr_replay replay __attribute__((cleanup(spr_record_use))) = {.use = use, .status = AZ_OK};
    replay.status = azspd_resume_preview(result->ticket);
}

static void spr_mount(void) {
    if (azsp_carrier_media())
        return;
    (void) azsp_carrier_mount(azsp_cache_root(spr_cache));
}

static void azsp_runtime_tick(void) {
    if (atomic_load_explicit(&spr_start_state, memory_order_acquire) != SPR_RUNNING)
        return;
    spr_mount();
    if (azsp_catalog_take_wake())
        (void) azsp_carrier_wake();
    struct spr_result result;
    if (spr_take_result(SPR_SEARCH, &result))
        spr_search_done(&result);
    for (unsigned slot = SPR_LOAD_1; slot < SPR_PREVIEW; ++slot) {
        if (spr_take_result(slot, &result))
            spr_load_done(&result);
    }
    if (spr_take_result(SPR_PREVIEW, &result))
        spr_preview_done(&result);
    spr_watch_decks();
}

static int spr_code_patch(uintptr_t address, void *replacement, void **original) {
    return azm_install_hook_at(address, replacement, original, "source provider");
}

static bool spr_slot_patch(uintptr_t slot, uintptr_t expected, uintptr_t replacement) {
    if (*(uintptr_t *) slot != expected)
        return false;
    void *original;
    return azm_patch_vtable(slot, (void *) replacement, &original) == 1;
}

static int spr_install_hooks(int build) {
    static const struct azspd_host dispatcher = {
        .on_message_thread = on_msg_thread,
        .owns_browse = spr_browse_owned,
        .owns_track = spr_track_owned,
        .search = spr_schedule_search,
        .prepare = spr_schedule_load,
        .preview = spr_schedule_preview,
        .search_view = spr_native_search_view,
    };
    if (azsp_query_install(build, spr_code_patch) != AZ_OK)
        return 0;
    if (azspd_install(build, &dispatcher, spr_slot_patch) != AZ_OK)
        return 0;
    if (azspd_paths_install(build, spr_code_patch, spr_slot_patch) != AZ_OK ||
        azspd_analysis_install(build, spr_code_patch) != AZ_OK)
        return 0;
    return azsp_carrier_install(build, spr_code_patch) == AZ_OK;
}

static void spr_publish_start(bool running) {
    pthread_mutex_lock(&spr_lock);
    atomic_store_explicit(&spr_start_state, running ? SPR_RUNNING : SPR_ABORTED, memory_order_release);
    pthread_cond_broadcast(&spr_work);
    pthread_mutex_unlock(&spr_lock);
}

static bool spr_start_workers(const struct azsp_native *backend) {
    void *(*const entries[])(void *) = {spr_catalog_worker, spr_load_worker};
    pthread_t workers[sizeof entries / sizeof entries[0]];
    size_t count = 0;
    atomic_store_explicit(&spr_start_state, SPR_BOOTING, memory_order_relaxed);
    for (; count < sizeof workers / sizeof workers[0]; ++count) {
        if (az_thread_create(&workers[count], NULL, entries[count], NULL))
            break;
    }
    bool running = count == sizeof workers / sizeof workers[0] && azsp_bind_native(backend) == AZ_OK;
    spr_publish_start(running);
    for (size_t i = 0; i < count; ++i) {
        if (running)
            pthread_detach(workers[i]);
        else
            pthread_join(workers[i], NULL);
    }
    return running;
}

static void azsp_runtime_start(int build) {
    static const struct azsp_native backend = {.storage_modes = 3, .select = spr_select, .clear = spr_clear};
    if (!spr_install_hooks(build) || !install_library_hooks())
        return;
    /* Mod registration starts after this constructor finishes. */
    int status = spr_catalog_init();
    if (status != AZ_OK) {
        drag_log("source provider catalog creation failed: %d\n", status);
        return;
    }
    if (!spr_start_workers(&backend)) {
        drag_log("source provider worker startup failed\n");
        spr_discard_cache();
    }
}
