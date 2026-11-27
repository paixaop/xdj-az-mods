/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "source_import.h"
#include "source_provider_catalog.h"
#include "source_provider_carrier.h"
#include "source_provider_dispatch.h"
#include "source_provider_lifetime.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char id[33];
    enum az_source_storage_mode mode;
} cm_mod;
static int g_ep147_build = 2;
static _Atomic unsigned long g_sv_obj;
static _Atomic int g_sv_vis;
static const uint64_t medium = UINT64_C(0x14200000001);
static cm_mod provider = {.id = "provider", .mode = AZ_SOURCE_IMPORT};
static cm_mod second_provider = {.id = "second-provider", .mode = AZ_SOURCE_STREAMING};
static _Atomic unsigned projected, imports, streams, released, path_tickets, stage_releases, busy_reclaims;
static _Atomic int mounted, wake, lookup_error, retained_failure, watch_failure, release_busy, use_allocation_failure;
static _Atomic unsigned workers_exited, held_searches;
static _Atomic bool hold_search;
static const struct azspd_host *host;
static const struct azsp_native *backend;
static unsigned searched, loaded, previewed, preview_cancelled, banners, mounts, wakes;
static unsigned search_replays, search_cancellations;
static uint64_t last_ticket;
static uint64_t load_epochs[4];
static uint32_t load_tracks[4];
static struct azspd_search native_search;
#if defined(__linux__) && defined(__EXCEPTIONS)
static uint64_t exiting_replay, observed_path;
static struct azspd_path_state observed_state;
#endif

enum start_fault {
    START_NO_FAULT,
    START_SQL,
    START_CACHE,
    START_CATALOG,
    START_FIRST_WORKER,
    START_SECOND_WORKER,
    START_BIND
};

static enum start_fault start_fault;
static unsigned cache_opens, catalog_creations, cache_discards, worker_attempts;
static bool cache_live, catalog_ready;

struct fixture_thread {
    void *(*entry)(void *);
    void *argument;
};

static struct fixture_thread threads[2];

static void drag_log(const char *fmt, ...) { (void) fmt; }
static int az_banner_strip(const char *message, int ms) {
    assert(*message && ms);
    ++banners;
    return 1;
}
static int on_msg_thread(void) { return 1; }
static int install_library_hooks(void) { return 1; }
static void *fixture_worker(void *argument) {
    struct fixture_thread thread = *(const struct fixture_thread *) argument;
    void *result = thread.entry(thread.argument);
    ++workers_exited;
    return result;
}

static int az_thread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    unsigned index = worker_attempts++;
    assert(index < sizeof threads / sizeof threads[0]);
    if ((index == 0 && start_fault == START_FIRST_WORKER) ||
        (index == 1 && start_fault == START_SECOND_WORKER))
        return EAGAIN;
    threads[index] = (struct fixture_thread) {.entry = fn, .argument = arg};
    return pthread_create(thread, attr, fixture_worker, &threads[index]);
}
static int azm_patch_vtable(uintptr_t slot, void *entry, void **out) {
    (void) slot;
    (void) entry;
    (void) out;
    return 1;
}
static int azm_install_hook_at(uintptr_t at, void *entry, void **out, const char *what) {
    (void) at;
    (void) entry;
    (void) out;
    (void) what;
    return 1;
}

static void *fixture_calloc(size_t count, size_t size);
#define calloc fixture_calloc
#include "source_provider_runtime.c"
#undef calloc

static void *fixture_calloc(size_t count, size_t size) {
    if (size == sizeof(struct spr_deck_use) && atomic_exchange(&use_allocation_failure, 0))
        return NULL;
    return calloc(count, size);
}

int azsp_bind_native(const struct azsp_native *api) {
    assert(catalog_ready && cache_live && worker_attempts == 2);
    if (start_fault == START_BIND)
        return AZ_BUSY;
    backend = api;
    return AZ_OK;
}
int azsp_query_install(int build, azsp_carrier_patch_fn patch) { return build == 2 && patch ? AZ_OK : AZ_UNSUPPORTED; }
int azspd_paths_install(int build, azspd_code_patch code, azspd_patch slot) {
    assert(build == 2 && code && slot);
    return AZ_OK;
}
int azspd_analysis_install(int build, azspd_code_patch code) {
    assert(build == 2 && code);
    return AZ_OK;
}
int azspd_install(int build, const struct azspd_host *api, azspd_patch patch) {
    assert(build == 2 && patch);
    host = api;
    return AZ_OK;
}
int azsp_carrier_install(int build, azsp_carrier_patch_fn patch) {
    assert(build == 2 && patch);
    return AZ_OK;
}
int azsp_cache_open(const char *parent, struct azsp_cache **out) {
    assert(!strcmp(parent, "/tmp/azmod-sources"));
    assert(!cache_live);
    ++cache_opens;
    *out = NULL;
    if (start_fault == START_CACHE)
        return AZ_FAILED;
    cache_live = true;
    *out = (void *) &mounted;
    return AZ_OK;
}
const char *azsp_cache_root(const struct azsp_cache *cache) {
    assert(cache && cache_live);
    return "/tmp/carrier";
}
void azsp_cache_discard(struct azsp_cache *cache) {
    assert(cache && cache_live);
    ++cache_discards;
    cache_live = catalog_ready = false;
}
int az_storage_sql_ready(void) { return start_fault != START_SQL; }
const struct az_library_sql *az_storage_sql(void) {
    static const struct az_library_sql sql;
    return &sql;
}
int az_storage_key_database(void *db) {
    (void) db;
    return 0;
}
int azsp_catalog_create(const char *root, const struct az_library_sql *sql, int (*key)(void *)) {
    assert(cache_live && !strcmp(root, "/tmp/carrier") && sql && key);
    ++catalog_creations;
    catalog_ready = start_fault != START_CATALOG;
    return catalog_ready ? AZ_OK : AZ_FAILED;
}
int azsp_carrier_mount(const char *root) {
    assert(catalog_ready && *root);
    ++mounts;
    mounted = 1;
    return AZ_OK;
}
uint64_t azsp_carrier_media(void) { return mounted ? medium : 0; }
const char *azsp_carrier_root(void) { return "/tmp/carrier"; }
int azsp_carrier_select(void *model) { return model && mounted ? AZ_OK : AZ_PENDING; }
int azsp_carrier_wake(void) {
    ++wakes;
    return AZ_OK;
}
int azsp_catalog_take_wake(void) { return atomic_exchange(&wake, 0); }
int azsp_request_source(void *owner, const struct azsp_request *request, struct azsp_result *page) {
    assert((owner == &provider || owner == &second_provider) && request->capacity == AZ_SOURCE_PAGE_MAX);
    if (request->operation == AZSP_SEARCH && hold_search) {
        const struct timespec delay = {.tv_nsec = 1000000};
        ++held_searches;
        while (hold_search)
            (void) nanosleep(&delay, NULL);
    }
    *page = (struct azsp_result) {.page.count = 1};
    if (request->operation == AZSP_PLAYLISTS)
        page->value.playlists[0] = (az_source_playlist_v1) {.id = "one", .name = "One"};
    else
        page->value.tracks[0] = (az_source_track_v1) {.id = "track", .title = "Music"};
    return AZ_OK;
}
int azsp_catalog_apply(const char *id, const struct azsp_request *request,
                       const struct azsp_result *page, uint32_t ids[AZ_SOURCE_PAGE_MAX]) {
    assert((!strcmp(id, provider.id) || !strcmp(id, second_provider.id)) &&
           !ids && !request->offset && page->page.count <= 1);
    ++projected;
    return AZ_OK;
}
int azsp_catalog_track(uint32_t id, char owner[33], az_source_track_v1 *track) {
    if (lookup_error)
        return AZ_NOT_FOUND;
    assert(id == 43 || id == 44);
    strcpy(owner, provider.id);
    *track = (az_source_track_v1) {.id = "track", .title = "Music"};
    return AZ_OK;
}
enum az_source_storage_mode azsp_source_storage_mode(const void *owner) { return ((const cm_mod *) owner)->mode; }
int azspi_import_provider(const struct azspi_provider_load *req, struct azspi_imported_track *out) {
    assert(req->source == &provider && !req->cancelled(req->context));
    ++imports;
    *out = (struct azspi_imported_track) {.media = 0x102, .id = 109};
    return AZ_OK;
}
int azsp_prepare_source(void *owner, const az_source_track_v1 *track, const char *parent,
                        enum az_source_storage_mode mode, struct azsp_stage **out) {
    assert(owner == &provider && *track->id && *parent && mode == AZ_SOURCE_STREAMING);
    *out = (void *) &streams;
    return AZ_OK;
}
void azsp_stage_release(struct azsp_stage *stage) {
    assert((void *) stage == &streams);
    ++stage_releases;
}
const char *azsp_stage_path(const struct azsp_stage *stage) {
    assert(stage);
    return "/tmp/carrier/source/file.m4a";
}
int azspd_path_watch(const char *path, uint64_t *out) {
    assert(*path);
    if (atomic_exchange(&watch_failure, 0))
        return AZ_LIMIT;
    *out = ++path_tickets;
    return AZ_OK;
}
int azspd_path_status(uint64_t ticket, struct azspd_path_state *out) {
    assert(ticket);
    *out = (struct azspd_path_state) {.opened = 1};
#if defined(__linux__) && defined(__EXCEPTIONS)
    if (ticket == observed_path)
        *out = observed_state;
#endif
    return AZ_OK;
}
int azspd_path_unwatch(uint64_t ticket) {
    assert(ticket);
    return AZ_OK;
}
int azsp_stream_prepare(int firmware, uint64_t media, uint32_t id, const az_source_track_v1 *track,
                        struct azsp_stage *stage, int (*cancel)(void *), void *ctx, struct azsp_stream **out) {
    assert(firmware == 2 && media == medium && (id == 43 || id == 44) && track && stage && !cancel(ctx));
    ++streams;
    *out = (void *) stage;
    return atomic_exchange(&retained_failure, 0) ? AZ_FAILED : AZ_OK;
}
int azsp_stream_release_when(struct azsp_stream *stream, int (*last_reader)(void *), void *context) {
    assert(stream);
    if (release_busy) {
        ++busy_reclaims;
        return AZ_BUSY;
    }
    assert(last_reader(context));
    ++released;
    return AZ_OK;
}
int azspd_resume_search(uint64_t ticket) {
    ++search_replays;
    if (native_search.ticket != ticket)
        return AZ_STALE;
    native_search.ticket = 0;
    last_ticket = ticket;
    ++searched;
    return AZ_OK;
}
void azspd_cancel_search(uint64_t ticket) {
    if (native_search.ticket == ticket) {
        native_search.ticket = 0;
        ++search_cancellations;
    }
}
bool azspd_load_current(uint64_t ticket) { return ticket != 0; }
void azspd_cancel_load(uint64_t ticket) { last_ticket = ticket; }
int azspd_resume_load(uint64_t ticket, const struct azspd_target *target) {
    if (ticket == 73)
        assert(target && target->id.track == 109 && target->id.media == 0x102 && target->category == 4);
    else
        assert(!target);
    last_ticket = ticket;
    if (ticket == 79)
        return AZ_STALE;
    ++loaded;
#if defined(__linux__) && defined(__EXCEPTIONS)
    if (ticket == exiting_replay)
        pthread_exit(NULL);
#endif
    return AZ_OK;
}
int azspd_resume_preview(uint64_t ticket) {
    last_ticket = ticket;
    if (ticket == 87)
        return AZ_STALE;
    ++previewed;
#if defined(__linux__) && defined(__EXCEPTIONS)
    if (ticket == exiting_replay)
        pthread_exit(NULL);
#endif
    return AZ_OK;
}
void azspd_cancel_preview(uint64_t ticket) {
    last_ticket = ticket;
    ++preview_cancelled;
}

static int load_seed(unsigned deck, struct azstatus_native_receipt *out) {
    out->deck = deck;
    out->unit = deck + 1;
    out->epoch = load_epochs[deck];
    out->a = load_tracks[deck] ? AZN_LOADED : AZN_EMPTY;
    return 1;
}
static int track_matches(unsigned deck, uint64_t epoch, uint64_t generation, const unsigned char id[12]) {
    (void) deck;
    (void) epoch;
    (void) generation;
    (void) id;
    return 1;
}
enum azspd_deck_use azspd_deck_observe(struct azspd_deck_watch *watch, const struct azstatus_native_v1 *api,
                                       const struct azstatus_native_receipt *receipt) {
    assert(watch && api && receipt->deck == watch->deck - 1);
    if (receipt->epoch <= watch->epoch || watch->use == AZSPD_DECK_RETIRED)
        return watch->use;
    if (load_tracks[receipt->deck] == watch->id.track) {
        watch->epoch = receipt->epoch;
        watch->use = AZSPD_DECK_ACCEPTED;
    } else if (watch->use == AZSPD_DECK_ACCEPTED)
        watch->use = AZSPD_DECK_RETIRED;
    return watch->use;
}

static void verify_startup_failures(void) {
    static const struct {
        enum start_fault fault;
        unsigned opens, catalogs, discards, attempts, exits;
    } cases[] = {
        {START_SQL, 0, 0, 0, 0, 0},
        {START_CACHE, 1, 0, 0, 0, 0},
        {START_CATALOG, 1, 1, 1, 0, 0},
        {START_FIRST_WORKER, 1, 1, 1, 1, 0},
        {START_SECOND_WORKER, 1, 1, 1, 2, 1},
        {START_BIND, 1, 1, 1, 2, 2},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        start_fault = cases[i].fault;
        cache_opens = catalog_creations = cache_discards = worker_attempts = 0;
        workers_exited = 0;
        azsp_runtime_start(2);
        azsp_runtime_tick();
        assert(!backend && !mounts && !cache_live && !catalog_ready);
        assert(cache_opens == cases[i].opens && catalog_creations == cases[i].catalogs);
        assert(cache_discards == cases[i].discards && worker_attempts == cases[i].attempts);
        assert(workers_exited == cases[i].exits);
    }
    start_fault = START_NO_FAULT;
    worker_attempts = 0;
}

static bool defer_search(const struct azspd_search *query) {
    if (native_search.ticket && native_search.media == query->media && !strcmp(native_search.query, query->query))
        return true;
    native_search = *query;
    if (host->search(query))
        return true;
    native_search.ticket = 0;
    return false;
}

static void await(unsigned *value, unsigned expected) {
    const struct timespec delay = {.tv_nsec = 1000000};
    for (unsigned i = 0; *value != expected && i < 2000; ++i) {
        azsp_runtime_tick();
        (void) nanosleep(&delay, NULL);
    }
    assert(*value == expected);
}

static void await_worker(_Atomic unsigned *value, unsigned expected) {
    const struct timespec delay = {.tv_nsec = 1000000};
    for (unsigned i = 0; *value < expected && i < 2000; ++i)
        (void) nanosleep(&delay, NULL);
    assert(*value >= expected);
}

static bool completed_result(unsigned slot, uint64_t ticket) {
    pthread_mutex_lock(&spr_lock);
    bool ready = (spr_completed & (1u << slot)) && spr_results[slot].ticket == ticket;
    pthread_mutex_unlock(&spr_lock);
    return ready;
}

static void await_completed_result(unsigned slot, uint64_t ticket) {
    const struct timespec delay = {.tv_nsec = 1000000};
    for (unsigned i = 0; !completed_result(slot, ticket) && i < 2000; ++i)
        (void) nanosleep(&delay, NULL);
    assert(completed_result(slot, ticket));
}

static void switch_provider(cm_mod *owner) {
    backend->clear();
    assert(backend->select(owner, owner) == AZ_OK);
}

static void verify_search_switch(uint64_t ticket, bool precompute) {
    struct azspd_search query = {.ticket = ticket, .media = medium, .query = "same text"};
    unsigned held = held_searches, replays = search_replays, cancellations = search_cancellations;
    hold_search = !precompute;
    assert(defer_search(&query));
    if (precompute)
        await_completed_result(SPR_SEARCH, ticket);
    else
        await_worker(&held_searches, held + 1);

    switch_provider(&second_provider);
    azsp_runtime_tick();
    assert(search_replays == replays && search_cancellations == cancellations + 1);
    assert(!native_search.ticket);

    hold_search = false;
    ++query.ticket;
    assert(defer_search(&query));
    await(&searched, searched + 1);
    assert(last_ticket == query.ticket && search_replays == replays + 1);
    switch_provider(&provider);
}

static void verify_release(struct azspd_load *load) {
    static const struct azstatus_native_v1 api = {.size = sizeof api, .version = 1, .load_seed = load_seed, .track_matches = track_matches};
    unsigned prior_streams = streams, prior_releases = released;
    unsigned next_load = loaded + 1;
    spr_native_api = &api;
    load->ticket = 77;
    load->id.track = 44;
    assert(host->prepare(load));
    await(&loaded, next_load);
    assert(streams == prior_streams + 1 && released == prior_releases);
    unsigned deck = load->deck - 1;
    load_tracks[deck] = load->id.track;
    ++load_epochs[deck];
    azsp_runtime_tick();
    release_busy = 1;
    load_tracks[deck] = 0;
    ++load_epochs[deck];
    azsp_runtime_tick();
    await_worker(&busy_reclaims, 2);
    assert(released == prior_releases);
    release_busy = 0;
    await_worker(&released, prior_releases + 1);
    assert(released == prior_releases + 1);
}

static void verify_retry(struct azspd_load *load) {
    unsigned prior_streams = streams, prior_releases = released;
    unsigned next_load = loaded + 1;
    retained_failure = 1;
    load->ticket = 78;
    assert(host->prepare(load));
    await(&banners, banners + 1);
    await_worker(&released, prior_releases + 1);
    assert(streams == prior_streams + 1 && released == prior_releases + 1);

    load->ticket = 79;
    assert(host->prepare(load));
    const struct timespec delay = {.tv_nsec = 1000000};
    for (unsigned i = 0; released != prior_releases + 2 && i < 2000; ++i) {
        azsp_runtime_tick();
        (void) nanosleep(&delay, NULL);
    }
    assert(last_ticket == 79 && streams == prior_streams + 2 && released == prior_releases + 2);

    load->ticket = 80;
    assert(host->prepare(load));
    await(&loaded, next_load);
    assert(streams == prior_streams + 3 && last_ticket == 80);
}

static void verify_duplicate_load_receipt(struct azspd_load load) {
    load.deck = 3;
    load.ticket = 84;
    unsigned deck = load.deck - 1;
    unsigned prior_releases = released;
    struct spr_deck_use *other_uses = spr_uses;
    assert(host->prepare(&load));
    await(&loaded, loaded + 1);
    struct spr_deck_use *first = spr_uses;
    assert(first->next == other_uses);

    ++load.ticket;
    assert(host->prepare(&load));
    await(&loaded, loaded + 1);
    assert(spr_uses->next == first && spr_uses->stream == first->stream);

    load_tracks[deck] = load.id.track;
    ++load_epochs[deck];
    azsp_runtime_tick();
    azsp_runtime_tick();
    load_tracks[deck] = 0;
    ++load_epochs[deck];
    azsp_runtime_tick();
    assert(spr_uses != other_uses && spr_uses->next == other_uses && released == prior_releases);

    load_tracks[deck] = load.id.track;
    ++load_epochs[deck];
    azsp_runtime_tick();
    load_tracks[deck] = 0;
    ++load_epochs[deck];
    azsp_runtime_tick();
    assert(spr_uses == other_uses);
    await_worker(&released, prior_releases + 1);
}

static void verify_unwatched_stage(struct azspd_load load) {
    unsigned prior_streams = streams;
    unsigned prior_stages = stage_releases;
    watch_failure = 1;
    load.ticket = 81;
    assert(host->prepare(&load));
    await(&banners, banners + 1);
    assert(last_ticket == 81 && streams == prior_streams && stage_releases == prior_stages + 1);
}

static void verify_untracked_replay(struct azspd_load load) {
    unsigned prior_releases = released;
    unsigned prior_streams = streams;
    unsigned prior_loaded = loaded, prior_previewed = previewed;
    load.ticket = 82;
    use_allocation_failure = 1;
    assert(host->prepare(&load));
    await(&banners, banners + 1);
    assert(last_ticket == 82 && loaded == prior_loaded && streams == prior_streams + 1);
    await_worker(&released, prior_releases + 1);

    const struct azspd_preview preview = {.ticket = 83, .id = load.id};
    use_allocation_failure = 1;
    assert(host->preview(&preview));
    await(&preview_cancelled, 1);
    assert(last_ticket == 83 && previewed == prior_previewed && streams == prior_streams + 2);
    await_worker(&released, prior_releases + 2);
}

static void verify_stale_preview(struct azspd_track_id id) {
    const struct azspd_preview preview = {.ticket = 87, .id = id};
    unsigned prior_releases = released, prior_previewed = previewed;
    struct spr_deck_use *other_uses = spr_uses;
    assert(host->preview(&preview));
    await_completed_result(SPR_PREVIEW, preview.ticket);
    azsp_runtime_tick();
    await_worker(&released, prior_releases + 1);
    assert(last_ticket == preview.ticket && previewed == prior_previewed && spr_uses == other_uses);
}

#if defined(__linux__) && defined(__EXCEPTIONS)
static void *interrupt_replay(void *unused) {
    (void) unused;
    azsp_runtime_tick();
    assert(!"fixture resume should unwind");
    return NULL;
}

static void verify_replay_unwind(struct azspd_load load, bool preview) {
    unsigned prior_releases = released;
    unsigned deck = 3;
    struct spr_deck_use *other_uses = spr_uses;
    unsigned *replayed = preview ? &previewed : &loaded;
    unsigned prior_replays = *replayed;
    exiting_replay = preview ? 89 : 88;
    observed_path = path_tickets + 1;
    observed_state = (struct azspd_path_state) {0};
    if (preview) {
        const struct azspd_preview event = {.ticket = exiting_replay, .id = load.id};
        assert(host->preview(&event));
        await_completed_result(SPR_PREVIEW, event.ticket);
    } else {
        load.ticket = exiting_replay;
        load.deck = deck + 1;
        assert(host->prepare(&load));
        await_completed_result(SPR_LOAD_1 + deck, load.ticket);
    }
    pthread_t message_thread;
    assert(pthread_create(&message_thread, NULL, interrupt_replay, NULL) == 0);
    assert(pthread_join(message_thread, NULL) == 0);
    assert(last_ticket == exiting_replay && *replayed == prior_replays + 1);
    exiting_replay = 0;

    azsp_runtime_tick();
    assert(spr_uses != other_uses && spr_uses->next == other_uses);
    observed_state.opened = observed_state.owners = 1;
    if (preview)
        observed_state.preview_opened = 1;
    else {
        load_tracks[deck] = load.id.track;
        ++load_epochs[deck];
    }
    azsp_runtime_tick();
    if (!preview) {
        load_tracks[deck] = 0;
        ++load_epochs[deck];
        azsp_runtime_tick();
    }
    assert(spr_uses != other_uses && released == prior_releases);
    observed_state.owners = 0;
    azsp_runtime_tick();
    assert(spr_uses == other_uses);
    await_worker(&released, prior_releases + 1);
}
#endif

int main(void) {
    azsp_runtime_start(1);
    assert(!host && !backend);
    verify_startup_failures();
    azsp_runtime_start(2);
    assert(host && backend && backend->storage_modes == 3);
    assert(cache_live && catalog_ready && !mounts);
    await(&mounts, 1);
    assert(!host->search_view());
    g_sv_obj = (uintptr_t) &provider;
    g_sv_vis = 1;
    assert(host->search_view() == &provider);
    assert(!host->owns_browse(medium));
    assert(backend->select(&provider, NULL) == AZ_PENDING);
    assert(backend->select(&provider, &provider) == AZ_OK && host->owns_browse(medium));
    wake = 1;
    await(&wakes, 1);

    const struct azspd_search query = {.ticket = 12, .media = medium, .query = "sound"};
    assert(defer_search(&query));
    await(&searched, 1);
    assert(last_ticket == 12 && projected == 3);
    const struct azspd_search empty = {.ticket = 13, .media = medium};
    assert(defer_search(&empty));
    await(&searched, 2);
    verify_search_switch(14, true);
    verify_search_switch(16, false);

    const struct azspd_track_id id = {.player = 1, .media = 0x142, .kind = 1, .track = 43};
    assert(host->owns_track(&id));
    struct azspd_load load = {.ticket = 73, .deck = 1, .id = id};
    assert(host->prepare(&load));
    await(&loaded, 1);
    assert(imports == 1 && !streams && last_ticket == 73);
    const struct azspd_preview preview = {.ticket = 74, .id = id};
    assert(host->preview(&preview));
    await(&previewed, 1);
    assert(streams == 1);
    load.ticket = 75;
    load.deck = 2;
    provider.mode = AZ_SOURCE_STREAMING;
    assert(host->prepare(&load));
    await(&loaded, 2);
    assert(streams == 1 && last_ticket == 75);
    lookup_error = 1;
    load.ticket = 76;
    assert(host->prepare(&load));
    await(&banners, 1);
    assert(last_ticket == 76);
    lookup_error = 0;
    load.id.track = 44;
    verify_unwatched_stage(load);
    verify_untracked_replay(load);
    verify_release(&load);
    verify_duplicate_load_receipt(load);
    verify_stale_preview(load.id);
#if defined(__linux__) && defined(__EXCEPTIONS)
    verify_replay_unwind(load, false);
    verify_replay_unwind(load, true);
#endif
    verify_retry(&load);
    backend->clear();
    assert(!host->owns_browse(medium) && !host->search(&query));
    puts("Source runtime dispatch, paged catalog, import, preview and stream reuse passed");
    return 0;
}
