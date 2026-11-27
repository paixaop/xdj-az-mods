/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "source_provider_dispatch.h"
#include "../common/azstatus_native.h"

#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

/* Exact EP147 2.00. KeywordSearchFacade dispatches SearchKeywordChangeEvent
 * through its update vslot. LoadHandler validates the Browse selection before
 * calling either ITrackLoader entry. Keep every stock request on its own path. */
#define DISPATCH_FIRMWARE_200 2 /* EPB_V200 */
#define SEARCH_UPDATE_SLOT 0x031641b0u
#define SEARCH_UPDATE 0x020014e8u
#define SEARCH_BROWSER 0x58u
#define SEARCH_STRING 0x98u
#define SEARCH_VIEW_VTABLE 0x02d531e8u
#define SEARCH_VIEW_FACADE 0x1738u
#define SEARCH_FACADE_VTABLE 0x0316f270u
#define SEARCH_FACADE_REQUEST 0x0205bc50u
#define SEARCH_FACADE_HANDLER 0x58u
#define SEARCH_ACTIVE_TAB 0x01fd8258u
#define SEARCH_TAB_CATEGORY 0x50u
#define SEARCH_CATEGORY 0x12u
#define JUCE_STRING_UTF8 0x024a1180u
#define JUCE_STRING_DELETE 0x02469aa0u
#define BROWSER_MEDIA 0x1e0u
#define TRACK_LOADER_VTABLE 0x02a06e68u
#define LOADER_INDEX_SLOT 0x02a06e78u
#define LOADER_LOCATE_SLOT 0x02a06e80u
#define LOADER_INDEX 0x00ab40c8u
#define LOADER_LOCATE 0x00ab35b0u
#define LOADER_RESOLVE 0x019e9028u
#define PREVIEW_VTABLE 0x02a23c50u
#define PREVIEW_START_SLOT 0x02a23c60u
#define PREVIEW_START 0x00b11480u
#define PREVIEW_STOP_SLOT 0x02a23c68u
#define PREVIEW_STOP 0x00b0f9d8u
#define PREVIEW_UNLOAD_SLOT 0x02a23c78u
#define PREVIEW_UNLOAD 0x00b10a30u
#define PREVIEW_CANCEL_SLOT 0x02a23c80u
#define PREVIEW_CANCEL 0x00b10e98u
#define PREVIEW_RESOLVE 0x00bddc40u
/* Firmware hash of "GUI->BrowsePreviewLoaderRef". */
#define PREVIEW_ALIAS UINT64_C(0x01884a202cbc7616)
#define DISPATCH_DECKS 4
#define TRACK_ID_BYTES 12u
#define HIERARCHY_LIMIT 32u

struct hierarchy_item {
    uint32_t words[3];
};

struct hierarchy_view {
    uint16_t category;
    const struct hierarchy_item *begin, *end, *capacity;
};

struct hierarchy_snapshot {
    uint16_t category;
    size_t depth;
    struct hierarchy_item items[HIERARCHY_LIMIT];
};

struct load_request {
    struct azspd_load event;
    uint32_t index, count, sort;
    uint8_t order;
    bool indexed;
    struct hierarchy_snapshot hierarchy;
};

struct preview_request {
    struct azspd_preview event;
    uint64_t generation;
    float offset;
    struct hierarchy_snapshot hierarchy;
};

struct mapped_loader {
    void *object;
    uint64_t key;
};

struct search_replay {
    const struct azspd_search *request;
    int status;
};

/* Browse load-handler aliases in logical deck order. The native resolver
 * handles their translation to UsecaseTrackLoader registrar keys. */
static const uint64_t loader_ids[DISPATCH_DECKS] = {
    UINT64_C(0x01d1a70fb8ad4e4b),
    UINT64_C(0x00c00e6ca1e71fe2),
    UINT64_C(0x018cd337caa95a1b),
    UINT64_C(0x007b3a94b3e32bb2),
};

_Static_assert(offsetof(struct azspd_track_id, padding) == TRACK_ID_BYTES, "native TrackID");
_Static_assert(sizeof(struct azspd_track_id) == 16, "native loader argument");
_Static_assert(sizeof(struct hierarchy_item) == 12, "native hierarchy entry");
_Static_assert(offsetof(struct hierarchy_view, begin) == 8, "native hierarchy vector");
_Static_assert(sizeof(struct hierarchy_view) == 32, "native hierarchy set");
_Static_assert(sizeof(struct mapped_loader) == 16, "native MappedObjPtr");

static struct azspd_host dispatch_host;
static _Atomic bool dispatch_ready;
static struct load_request pending[DISPATCH_DECKS];
static struct azspd_search pending_search;
static struct search_replay *active_replay;
static struct preview_request pending_preview;
static _Atomic uint64_t preview_generation;
static uint64_t serial;
static uint32_t (*original_search)(void *);
static void **(*search_active_tab)(void *);
static void (*search_facade_request)(void *, const void *);
static void (*search_string_create)(void *, const char *);
static void (*search_string_delete)(void *);
static void (*original_index)(void *, uint32_t, uint32_t, const struct azspd_track_id *,
                              const struct hierarchy_view *, const uint32_t *, const uint8_t *);
static void (*original_locate)(void *, const struct azspd_track_id *, const struct hierarchy_view *,
                               const uint32_t *, const uint8_t *);
static bool (*loader_resolve)(struct mapped_loader *, bool);
static bool (*preview_resolve)(struct mapped_loader *, bool);
static void (*original_preview)(void *, const struct azspd_track_id *, float,
                                const struct hierarchy_view *);
static void (*original_preview_stop)(void *);
static void (*original_preview_unload)(void *, const void *);
static void (*original_preview_cancel)(void *, const struct azspd_track_id *);

static bool dispatch_on_message_thread(void) {
    return atomic_load_explicit(&dispatch_ready, memory_order_acquire) && dispatch_host.on_message_thread();
}

static void *native_pointer(const void *object, size_t offset) {
    void *ptr;
    memcpy(&ptr, (const unsigned char *) object + offset, sizeof(ptr));
    return ptr;
}

static uint64_t next_ticket(void) {
    if (++serial == 0)
        ++serial;
    return serial;
}

static uint64_t browse_media(const void *browser) {
    uint64_t media = 0;
    if (browser)
        memcpy(&media, (const unsigned char *) browser + BROWSER_MEDIA, sizeof(media));
    return media;
}

static bool search_event(void *event, struct azspd_search *out) {
    out->media = browse_media(native_pointer(event, SEARCH_BROWSER));
    if (!out->media || !dispatch_host.owns_browse(out->media))
        return false;
    const char *query = native_pointer(event, SEARCH_STRING);
    if (!query)
        return false;
    size_t length = strnlen(query, sizeof(out->query));
    if (length == sizeof(out->query))
        return false;
    memcpy(out->query, query, length + 1);
    return true;
}

static bool same_search(const struct azspd_search *a, const struct azspd_search *b) {
    return a->media == b->media && !strcmp(a->query, b->query);
}

static bool defer_search(void *event) {
    struct azspd_search request = {0};
    if (!search_event(event, &request)) {
        pending_search.ticket = 0;
        return false;
    }
    if (pending_search.ticket && same_search(&pending_search, &request))
        return true;
    request.ticket = next_ticket();
    pending_search = request;
    if (dispatch_host.search(&request))
        return true;
    azspd_cancel_search(request.ticket);
    return false;
}

static uint32_t replay_search_event(void *event) {
    struct azspd_search request = {0};
    bool matches = search_event(event, &request) && same_search(&request, active_replay->request);
    uint32_t status = original_search(event);
    if (matches)
        active_replay->status = status == 0 ? AZ_OK : AZ_FAILED;
    return status;
}

static uint32_t dispatch_search(void *event) {
    if (!dispatch_on_message_thread())
        return original_search(event);
    if (active_replay)
        return replay_search_event(event);
    return defer_search(event) ? 0 : original_search(event);
}

bool azspd_search_current(uint64_t ticket) {
    return ticket && dispatch_on_message_thread() && pending_search.ticket == ticket;
}

void azspd_cancel_search(uint64_t ticket) {
    if (azspd_search_current(ticket))
        pending_search.ticket = 0;
}

static void *find_search_facade(void) {
    void *view = dispatch_host.search_view();
    if (!view || (uintptr_t) native_pointer(view, 0) != SEARCH_VIEW_VTABLE)
        return NULL;
    void *facade = native_pointer(view, SEARCH_VIEW_FACADE);
    if (!facade || (uintptr_t) native_pointer(facade, 0) != SEARCH_FACADE_VTABLE)
        return NULL;
    return facade;
}

static bool active_search(void *facade, uint64_t media) {
    void *handler = native_pointer(facade, SEARCH_FACADE_HANDLER);
    void *browser = handler ? native_pointer(handler, 0) : NULL;
    if (browse_media(browser) != media || !dispatch_host.owns_browse(media))
        return false;
    void **tab = search_active_tab(browser);
    uint16_t category = 0;
    if (tab && *tab)
        memcpy(&category, (const unsigned char *) *tab + SEARCH_TAB_CATEGORY, sizeof(category));
    return category == SEARCH_CATEGORY;
}

static int replay_search(void *facade, const struct azspd_search *request) {
    /* The facade creates and owns a new stock event against the live browser.
     * No firmware event or widget is retained while the provider runs. */
    struct search_replay replay = {.request = request, .status = AZ_FAILED};
    void *string = NULL;
    search_string_create(&string, request->query);
    active_replay = &replay;
    search_facade_request(facade, &string);
    active_replay = NULL;
    search_string_delete(&string);
    return replay.status;
}

int azspd_resume_search(uint64_t ticket) {
    if (!azspd_search_current(ticket) || active_replay)
        return AZ_STALE;
    struct azspd_search request = pending_search;
    pending_search.ticket = 0;
    void *facade = find_search_facade();
    if (!facade || !active_search(facade, request.media))
        return AZ_STALE;
    return replay_search(facade, &request);
}

/* Registry returns a borrowed native object. Both acquisition and use happen
 * on the message thread. The pending request carries only the logical deck. */
static void *resolve_deck(unsigned deck_index) {
    struct mapped_loader mapped = {.key = loader_ids[deck_index]};
    return loader_resolve(&mapped, false) ? mapped.object : NULL;
}

static int find_deck(void *loader) {
    for (unsigned i = 0; i < DISPATCH_DECKS; ++i) {
        if (resolve_deck(i) == loader)
            return (int) i;
    }
    return -1;
}

static bool hierarchy_size(const struct hierarchy_view *hierarchy, size_t *out) {
    uintptr_t begin = (uintptr_t) hierarchy->begin;
    uintptr_t end = (uintptr_t) hierarchy->end;
    if (end < begin || end > (uintptr_t) hierarchy->capacity)
        return false;
    size_t bytes = end - begin;
    if (bytes % sizeof(struct hierarchy_item))
        return false;
    *out = bytes / sizeof(struct hierarchy_item);
    return *out <= HIERARCHY_LIMIT;
}

static bool copy_hierarchy(struct hierarchy_snapshot *snapshot,
                           const struct hierarchy_view *hierarchy) {
    if (!hierarchy_size(hierarchy, &snapshot->depth))
        return false;
    snapshot->category = hierarchy->category;
    if (snapshot->depth)
        memcpy(snapshot->items, hierarchy->begin, snapshot->depth * sizeof(snapshot->items[0]));
    return true;
}

static struct hierarchy_view hierarchy_view(const struct hierarchy_snapshot *snapshot) {
    return (struct hierarchy_view) {
        .category = snapshot->category,
        .begin = snapshot->items,
        .end = snapshot->items + snapshot->depth,
        .capacity = snapshot->items + snapshot->depth,
    };
}

static void defer_load(struct load_request *slot, struct load_request *request,
                       const struct hierarchy_view *hierarchy) {
    if (!copy_hierarchy(&request->hierarchy, hierarchy))
        return;
    request->event.ticket = next_ticket();
    *slot = *request;
    if (!dispatch_host.prepare(&request->event))
        azspd_cancel_load(request->event.ticket);
}

static bool intercept_load(void *loader, struct load_request *request,
                           const struct azspd_track_id *id, const struct hierarchy_view *hierarchy) {
    if (!dispatch_on_message_thread())
        return false;
    int deck = find_deck(loader);
    if (deck < 0)
        return false;
    struct load_request *slot = &pending[deck];
    slot->event.ticket = 0;
    memcpy(&request->event.id, id, TRACK_ID_BYTES);
    if (!dispatch_host.owns_track(&request->event.id))
        return false;
    request->event.deck = (unsigned) deck + 1;
    defer_load(slot, request, hierarchy);
    return true;
}

static void dispatch_index(void *loader, uint32_t index, uint32_t count,
                           const struct azspd_track_id *id, const struct hierarchy_view *hierarchy,
                           const uint32_t *sort, const uint8_t *order) {
    struct load_request request = {
        .indexed = true,
        .index = index,
        .count = count,
        .sort = *sort,
        .order = *order,
    };
    if (!intercept_load(loader, &request, id, hierarchy))
        original_index(loader, index, count, id, hierarchy, sort, order);
}

static void dispatch_locate(void *loader, const struct azspd_track_id *id,
                            const struct hierarchy_view *hierarchy, const uint32_t *sort,
                            const uint8_t *order) {
    struct load_request request = {.sort = *sort, .order = *order};
    if (!intercept_load(loader, &request, id, hierarchy))
        original_locate(loader, id, hierarchy, sort, order);
}

static struct load_request *find_ticket(uint64_t ticket) {
    if (!ticket || !dispatch_on_message_thread())
        return NULL;
    for (unsigned i = 0; i < DISPATCH_DECKS; ++i) {
        if (pending[i].event.ticket == ticket)
            return &pending[i];
    }
    return NULL;
}

bool azspd_load_current(uint64_t ticket) {
    return find_ticket(ticket) != NULL;
}

void azspd_cancel_load(uint64_t ticket) {
    struct load_request *slot = find_ticket(ticket);
    if (slot)
        slot->event.ticket = 0;
}

static void replay_carrier(void *loader, const struct load_request *request) {
    struct hierarchy_view hierarchy = hierarchy_view(&request->hierarchy);
    if (request->indexed)
        original_index(loader, request->index, request->count, &request->event.id, &hierarchy,
                       &request->sort, &request->order);
    else
        original_locate(loader, &request->event.id, &hierarchy, &request->sort, &request->order);
}

static void replay_replacement(void *loader, const struct azspd_target *target) {
    const struct hierarchy_view hierarchy = {.category = target->category};
    original_locate(loader, &target->id, &hierarchy, &target->sort, &target->order);
}

static void *current_loader(unsigned deck) {
    void *loader = resolve_deck(deck - 1);
    return loader && (uintptr_t) native_pointer(loader, 0) == TRACK_LOADER_VTABLE ? loader : NULL;
}

int azspd_resume_load(uint64_t ticket, const struct azspd_target *replacement) {
    struct load_request *slot = find_ticket(ticket);
    if (!slot)
        return AZ_STALE;
    struct load_request request = *slot;
    slot->event.ticket = 0;
    void *loader = current_loader(request.event.deck);
    if (!loader)
        return AZ_STALE;
    if (replacement)
        replay_replacement(loader, replacement);
    else
        replay_carrier(loader, &request);
    return AZ_OK;
}

static uint64_t preview_changed(void) {
    return atomic_fetch_add_explicit(&preview_generation, 1, memory_order_acq_rel) + 1;
}

static bool preview_unchanged(uint64_t generation) {
    return atomic_load_explicit(&preview_generation, memory_order_acquire) == generation;
}

static void *current_preview_loader(void) {
    struct mapped_loader mapped = {.key = PREVIEW_ALIAS};
    if (!preview_resolve(&mapped, false) || !mapped.object)
        return NULL;
    return (uintptr_t) native_pointer(mapped.object, 0) == PREVIEW_VTABLE ? mapped.object : NULL;
}

static bool hold_preview(void *loader, struct preview_request *request,
                         const struct hierarchy_view *hierarchy) {
    if (!dispatch_host.owns_track(&request->event.id) || current_preview_loader() != loader)
        return false;
    if (!copy_hierarchy(&request->hierarchy, hierarchy))
        return true;
    request->event.ticket = next_ticket();
    pending_preview = *request;
    if (!dispatch_host.preview(&request->event))
        azspd_cancel_preview(request->event.ticket);
    return true;
}

static void dispatch_preview(void *loader, const struct azspd_track_id *id, float offset,
                             const struct hierarchy_view *hierarchy) {
    struct preview_request request = {.generation = preview_changed(), .offset = offset};
    memcpy(&request.event.id, id, TRACK_ID_BYTES);
    if (!dispatch_on_message_thread() || !hold_preview(loader, &request, hierarchy))
        original_preview(loader, id, offset, hierarchy);
}

static void dispatch_preview_stop(void *loader) {
    (void) preview_changed();
    original_preview_stop(loader);
}

static void dispatch_preview_unload(void *loader, const void *media) {
    (void) preview_changed();
    original_preview_unload(loader, media);
}

static void dispatch_preview_cancel(void *loader, const struct azspd_track_id *id) {
    (void) preview_changed();
    original_preview_cancel(loader, id);
}

bool azspd_preview_current(uint64_t ticket) {
    return ticket && dispatch_on_message_thread() && pending_preview.event.ticket == ticket &&
           preview_unchanged(pending_preview.generation);
}

void azspd_cancel_preview(uint64_t ticket) {
    if (azspd_preview_current(ticket))
        pending_preview.event.ticket = 0;
}

int azspd_resume_preview(uint64_t ticket) {
    if (!azspd_preview_current(ticket))
        return AZ_STALE;
    struct preview_request request = pending_preview;
    pending_preview.event.ticket = 0;
    void *loader = current_preview_loader();
    if (!loader || !preview_unchanged(request.generation))
        return AZ_STALE;
    struct hierarchy_view hierarchy = hierarchy_view(&request.hierarchy);
    original_preview(loader, &request.event.id, request.offset, &hierarchy);
    return AZ_OK;
}

static bool observable_deck(const struct azspd_deck_watch *watch,
                            const struct azstatus_native_v1 *api) {
    if (!watch || !api || api->version != 1)
        return false;
    if (api->size < offsetof(struct azstatus_native_v1, track_matches) + sizeof(api->track_matches))
        return false;
    return api->load_seed && api->track_matches && watch->deck >= 1 && watch->deck <= DISPATCH_DECKS;
}

static bool newer_deck_receipt(const struct azspd_deck_watch *watch,
                               const struct azstatus_native_receipt *receipt) {
    if (!receipt || receipt->size < offsetof(struct azstatus_native_receipt, content_id))
        return false;
    if (receipt->version != 1 || receipt->kind != AZN_LOAD || receipt->deck != watch->deck - 1)
        return false;
    return receipt->unit && receipt->epoch > watch->epoch &&
           (!watch->unit || receipt->unit == watch->unit);
}

static bool same_deck_seed(const struct azstatus_native_receipt *seed,
                           const struct azstatus_native_receipt *event) {
    return seed->kind == AZN_LOAD && seed->deck == event->deck && seed->a == event->a &&
           seed->unit == event->unit && seed->epoch == event->epoch &&
           seed->load_generation == event->load_generation;
}

static bool current_deck_receipt(const struct azstatus_native_v1 *api,
                                 const struct azstatus_native_receipt *event) {
    struct azstatus_native_receipt seed = {.size = sizeof(seed), .version = 1};
    return api->load_seed(event->deck, &seed) && same_deck_seed(&seed, event);
}

static void observe_loaded(struct azspd_deck_watch *watch, const struct azstatus_native_v1 *api,
                           const struct azstatus_native_receipt *receipt) {
    if (!receipt->valid || !receipt->load_generation || !current_deck_receipt(api, receipt))
        return;
    int matches = api->track_matches(receipt->deck, receipt->epoch, receipt->load_generation,
                                     (const unsigned char *) &watch->id);
    if (!current_deck_receipt(api, receipt))
        return;
    if (matches) {
        watch->unit = receipt->unit;
        watch->epoch = receipt->epoch;
        watch->use = AZSPD_DECK_ACCEPTED;
    } else if (watch->use == AZSPD_DECK_ACCEPTED)
        watch->use = AZSPD_DECK_RETIRED;
}

static void observe_deck(struct azspd_deck_watch *watch, const struct azstatus_native_v1 *api,
                         const struct azstatus_native_receipt *receipt) {
    if (receipt->a == AZN_LOADED)
        observe_loaded(watch, api, receipt);
    else if (receipt->a == AZN_EMPTY && watch->use == AZSPD_DECK_ACCEPTED &&
             current_deck_receipt(api, receipt))
        watch->use = AZSPD_DECK_RETIRED;
}

enum azspd_deck_use azspd_deck_observe(struct azspd_deck_watch *watch,
                                       const struct azstatus_native_v1 *api,
                                       const struct azstatus_native_receipt *receipt) {
    if (!watch)
        return AZSPD_DECK_PENDING;
    if (watch->use != AZSPD_DECK_RETIRED && observable_deck(watch, api) && newer_deck_receipt(watch, receipt))
        observe_deck(watch, api, receipt);
    return watch->use;
}

static bool callbacks_present(const struct azspd_host *host) {
    return host && host->on_message_thread && host->owns_browse && host->owns_track &&
           host->search && host->prepare && host->search_view;
}

static bool replace_slots(azspd_patch patch) {
    return patch(SEARCH_UPDATE_SLOT, SEARCH_UPDATE, (uintptr_t) dispatch_search) &&
           patch(LOADER_INDEX_SLOT, LOADER_INDEX, (uintptr_t) dispatch_index) &&
           patch(LOADER_LOCATE_SLOT, LOADER_LOCATE, (uintptr_t) dispatch_locate);
}

static void set_up_search(void) {
    original_search = (__typeof__(original_search)) (uintptr_t) SEARCH_UPDATE;
    search_active_tab = (__typeof__(search_active_tab)) (uintptr_t) SEARCH_ACTIVE_TAB;
    search_facade_request = (__typeof__(search_facade_request)) (uintptr_t) SEARCH_FACADE_REQUEST;
    search_string_create = (__typeof__(search_string_create)) (uintptr_t) JUCE_STRING_UTF8;
    search_string_delete = (__typeof__(search_string_delete)) (uintptr_t) JUCE_STRING_DELETE;
}

static bool replace_preview_slots(azspd_patch patch) {
    preview_resolve = (__typeof__(preview_resolve)) (uintptr_t) PREVIEW_RESOLVE;
    original_preview = (__typeof__(original_preview)) (uintptr_t) PREVIEW_START;
    original_preview_stop = (__typeof__(original_preview_stop)) (uintptr_t) PREVIEW_STOP;
    original_preview_unload = (__typeof__(original_preview_unload)) (uintptr_t) PREVIEW_UNLOAD;
    original_preview_cancel = (__typeof__(original_preview_cancel)) (uintptr_t) PREVIEW_CANCEL;
    return patch(PREVIEW_STOP_SLOT, PREVIEW_STOP, (uintptr_t) dispatch_preview_stop) &&
           patch(PREVIEW_UNLOAD_SLOT, PREVIEW_UNLOAD, (uintptr_t) dispatch_preview_unload) &&
           patch(PREVIEW_CANCEL_SLOT, PREVIEW_CANCEL, (uintptr_t) dispatch_preview_cancel) &&
           patch(PREVIEW_START_SLOT, PREVIEW_START, (uintptr_t) dispatch_preview);
}

int azspd_install(int firmware, const struct azspd_host *host, azspd_patch patch) {
    if (firmware != DISPATCH_FIRMWARE_200)
        return AZ_UNSUPPORTED;
    if (!patch || !callbacks_present(host))
        return AZ_INVALID;
    if (atomic_load_explicit(&dispatch_ready, memory_order_acquire))
        return AZ_BUSY;
    dispatch_host = *host;
    set_up_search();
    original_index = (__typeof__(original_index)) (uintptr_t) LOADER_INDEX;
    original_locate = (__typeof__(original_locate)) (uintptr_t) LOADER_LOCATE;
    loader_resolve = (__typeof__(loader_resolve)) (uintptr_t) LOADER_RESOLVE;
    if (!replace_slots(patch))
        return AZ_UNSUPPORTED;
    if (host->preview && !replace_preview_slots(patch))
        return AZ_UNSUPPORTED;
    atomic_store_explicit(&dispatch_ready, true, memory_order_release);
    return AZ_OK;
}
