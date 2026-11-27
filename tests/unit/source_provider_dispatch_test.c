/* SPDX-License-Identifier: GPL-3.0-only */
#include "source_provider_dispatch.c"

#include <assert.h>
#include <stdio.h>

static uintptr_t deck_objects[DISPATCH_DECKS] = {
    TRACK_LOADER_VTABLE,
    TRACK_LOADER_VTABLE,
    TRACK_LOADER_VTABLE,
    TRACK_LOADER_VTABLE,
};
static struct azspd_search search_request;
static struct azspd_load load_event;
static struct azspd_preview preview_event;
static struct azspd_track_id delivered_id;
static struct hierarchy_item delivered_items[HIERARCHY_LIMIT];
static void *delivered_loader;
static uint32_t delivered_index, delivered_count, delivered_sort;
static unsigned delivered_category, delivered_order, delivered_depth;
static unsigned updates, searches, native_requests, loads, indexed, located, patches;
static unsigned previews, preview_started, preview_stopped, preview_unloaded, preview_cancelled;
static uintptr_t preview_loader = PREVIEW_VTABLE;
static float preview_offset;
static int on_thread = 1;
static bool accepts_search = true, accepts_load = true, accepts_preview = true, reject_patch;
static uint32_t native_search_status;
static const uint64_t provider_media = UINT64_C(0x0000014200000001);
static unsigned char search_browser[BROWSER_MEDIA + sizeof(uint64_t)];
static unsigned char search_tab[SEARCH_TAB_CATEGORY + sizeof(uint16_t)];
static unsigned char search_view[SEARCH_VIEW_FACADE + sizeof(void *)];
static unsigned char search_facade[SEARCH_FACADE_HANDLER + sizeof(void *)];
static void *search_handler = search_browser;
static void *tab_ref = search_tab;
static bool search_visible = true, native_emits_event = true;

static int message_thread(void) {
    return on_thread;
}

static bool owns_browse(uint64_t media) {
    return media == provider_media;
}

static bool owns_track(const struct azspd_track_id *id) {
    return id->player == 1 && id->media == 0x142;
}

static bool prepare(const struct azspd_load *request) {
    ++loads;
    load_event = *request;
    return accepts_load;
}

static bool submit_search(const struct azspd_search *request) {
    ++searches;
    search_request = *request;
    return accepts_search;
}

static bool submit_preview(const struct azspd_preview *request) {
    ++previews;
    preview_event = *request;
    return accepts_preview;
}

static uint32_t stock_search(void *event) {
    assert(event);
    ++updates;
    return native_search_status;
}

static void *visible_search_view(void) {
    return search_visible ? search_view : NULL;
}

static void set_pointer(void *object, size_t offset, const void *pointer) {
    memcpy((unsigned char *) object + offset, &pointer, sizeof(pointer));
}

static void set_search(void *event, void *browser, uint64_t media, const char *query) {
    set_pointer(event, SEARCH_BROWSER, browser);
    set_pointer(event, SEARCH_STRING, query);
    if (browser)
        memcpy((unsigned char *) browser + BROWSER_MEDIA, &media, sizeof(media));
}

static void **stock_active_tab(void *browser) {
    assert(browser == search_browser);
    return &tab_ref;
}

static void stock_string_create(void *string, const char *query) {
    set_pointer(string, 0, query);
}

static void stock_string_delete(void *string) {
    assert(native_pointer(string, 0));
    set_pointer(string, 0, NULL);
}

static void stock_search_request(void *facade, const void *string) {
    assert(facade == search_facade);
    ++native_requests;
    if (!native_emits_event)
        return;
    unsigned char event[SEARCH_STRING + sizeof(void *)] = {0};
    set_search(event, search_browser, browse_media(search_browser), native_pointer(string, 0));
    (void) dispatch_search(event);
}

static void set_search_category(uint16_t category) {
    memcpy(search_tab + SEARCH_TAB_CATEGORY, &category, sizeof(category));
}

static bool stock_resolve(struct mapped_loader *mapped, bool report_missing) {
    assert(!report_missing);
    unsigned index = 0;
    while (index < DISPATCH_DECKS && mapped->key != loader_ids[index])
        ++index;
    assert(index < DISPATCH_DECKS);
    mapped->object = deck_objects[index] ? &deck_objects[index] : NULL;
    return mapped->object != NULL;
}

static void record_load(void *loader, const struct azspd_track_id *id,
                        const struct hierarchy_view *hierarchy, const uint32_t *sort,
                        const uint8_t *order) {
    size_t depth;
    assert(hierarchy_size(hierarchy, &depth));
    delivered_loader = loader;
    delivered_id = *id;
    delivered_category = hierarchy->category;
    delivered_sort = *sort;
    delivered_order = *order;
    delivered_depth = (unsigned) depth;
    if (depth)
        memcpy(delivered_items, hierarchy->begin, depth * sizeof(delivered_items[0]));
}

static void stock_index(void *loader, uint32_t index, uint32_t count,
                        const struct azspd_track_id *id, const struct hierarchy_view *hierarchy,
                        const uint32_t *sort, const uint8_t *order) {
    ++indexed;
    delivered_index = index;
    delivered_count = count;
    record_load(loader, id, hierarchy, sort, order);
}

static void stock_locate(void *loader, const struct azspd_track_id *id,
                         const struct hierarchy_view *hierarchy, const uint32_t *sort,
                         const uint8_t *order) {
    ++located;
    record_load(loader, id, hierarchy, sort, order);
}

static bool stock_preview_resolve(struct mapped_loader *mapped, bool report_missing) {
    assert(!report_missing && mapped->key == PREVIEW_ALIAS);
    mapped->object = preview_loader ? &preview_loader : NULL;
    return mapped->object != NULL;
}

static void stock_preview(void *loader, const struct azspd_track_id *id, float offset,
                          const struct hierarchy_view *hierarchy) {
    ++preview_started;
    preview_offset = offset;
    const uint32_t sort = 0;
    const uint8_t order = 0;
    record_load(loader, id, hierarchy, &sort, &order);
}

static void stock_preview_stop(void *loader) {
    assert(loader == &preview_loader);
    ++preview_stopped;
}

static void stock_preview_unload(void *loader, const void *media) {
    assert(loader == &preview_loader && media);
    ++preview_unloaded;
}

static void stock_preview_cancel(void *loader, const struct azspd_track_id *id) {
    assert(loader == &preview_loader && id);
    ++preview_cancelled;
}

static bool patch(uintptr_t slot, uintptr_t original, uintptr_t hook) {
    ++patches;
    switch (slot) {
    case SEARCH_UPDATE_SLOT:
        assert(original == SEARCH_UPDATE && hook == (uintptr_t) dispatch_search);
        break;
    case LOADER_INDEX_SLOT:
        assert(original == LOADER_INDEX && hook == (uintptr_t) dispatch_index);
        break;
    case LOADER_LOCATE_SLOT:
        assert(original == LOADER_LOCATE && hook == (uintptr_t) dispatch_locate);
        break;
    case PREVIEW_START_SLOT:
        assert(original == PREVIEW_START && hook == (uintptr_t) dispatch_preview);
        break;
    case PREVIEW_STOP_SLOT:
        assert(original == PREVIEW_STOP && hook == (uintptr_t) dispatch_preview_stop);
        break;
    case PREVIEW_UNLOAD_SLOT:
        assert(original == PREVIEW_UNLOAD && hook == (uintptr_t) dispatch_preview_unload);
        break;
    case PREVIEW_CANCEL_SLOT:
        assert(original == PREVIEW_CANCEL && hook == (uintptr_t) dispatch_preview_cancel);
        break;
    default:
        assert(false);
    }
    return !reject_patch;
}

static void install_fixture(void) {
    const struct azspd_host host = {
        .on_message_thread = message_thread,
        .owns_browse = owns_browse,
        .owns_track = owns_track,
        .prepare = prepare,
        .search = submit_search,
        .preview = submit_preview,
        .search_view = visible_search_view,
    };
    assert(azspd_install(1, &host, patch) == AZ_UNSUPPORTED && !patches);
    assert(azspd_install(2, NULL, patch) == AZ_INVALID && !patches);
    reject_patch = true;
    assert(azspd_install(2, &host, patch) == AZ_UNSUPPORTED && patches == 1);
    assert(!azspd_search_current(1) && !azspd_load_current(1));
    reject_patch = false;
    assert(azspd_install(2, &host, patch) == AZ_OK && patches == 8);
    assert(azspd_install(2, &host, patch) == AZ_BUSY && patches == 8);
    original_search = stock_search;
    search_active_tab = stock_active_tab;
    search_facade_request = stock_search_request;
    search_string_create = stock_string_create;
    search_string_delete = stock_string_delete;
    set_pointer(search_view, 0, (void *) (uintptr_t) SEARCH_VIEW_VTABLE);
    set_pointer(search_view, SEARCH_VIEW_FACADE, search_facade);
    set_pointer(search_facade, 0, (void *) (uintptr_t) SEARCH_FACADE_VTABLE);
    set_pointer(search_facade, SEARCH_FACADE_HANDLER, &search_handler);
    set_search_category(SEARCH_CATEGORY);
    original_locate = stock_locate;
    original_index = stock_index;
    loader_resolve = stock_resolve;
    preview_resolve = stock_preview_resolve;
    original_preview = stock_preview;
    original_preview_stop = stock_preview_stop;
    original_preview_unload = stock_preview_unload;
    original_preview_cancel = stock_preview_cancel;
}

static void native_search_requests(void) {
    unsigned char first[SEARCH_STRING + sizeof(void *)] = {0};
    unsigned char second[sizeof(first)] = {0};
    char long_query[AZ_SOURCE_QUERY_BYTES + 1];
    memset(long_query, 'w', AZ_SOURCE_QUERY_BYTES);
    long_query[AZ_SOURCE_QUERY_BYTES] = 0;
    set_search(first, NULL, 0, "uninitialized");
    assert(dispatch_search(first) == 0 && !searches && updates == 1);
    set_search(first, search_browser, 0x102, "stock");
    assert(dispatch_search(first) == 0 && !searches && updates == 2);
    set_search(first, search_browser, provider_media, NULL);
    assert(dispatch_search(first) == 0 && !searches && updates == 3);
    set_search(first, search_browser, provider_media, long_query);
    assert(dispatch_search(first) == 0 && !searches && updates == 4);
    set_search(first, search_browser, provider_media, "électronique");
    assert(dispatch_search(first) == 0 && searches == 1 && updates == 4);
    assert(search_request.media == provider_media && !strcmp(search_request.query, "électronique"));
    uint64_t first_ticket = search_request.ticket;
    assert(first_ticket && azspd_search_current(first_ticket));
    set_search(second, search_browser, provider_media, "électronique");
    assert(dispatch_search(second) == 0 && searches == 1 && azspd_search_current(first_ticket));
    set_search(second, search_browser, provider_media, "chicago");
    assert(dispatch_search(second) == 0 && searches == 2 && updates == 4);
    assert(!azspd_search_current(first_ticket));
    assert(azspd_resume_search(first_ticket) == AZ_STALE);
    on_thread = 0;
    assert(azspd_resume_search(search_request.ticket) == AZ_STALE && updates == 4);
    on_thread = 1;
    memset(second, 0, sizeof(second)); /* The stock handler may discard its first event. */
    assert(azspd_resume_search(search_request.ticket) == AZ_OK && updates == 5);
    assert(native_requests == 1 && searches == 2);
    assert(azspd_resume_search(search_request.ticket) == AZ_STALE);
}

static void cancelled_search(void) {
    unsigned char event[SEARCH_STRING + sizeof(void *)] = {0};
    unsigned prior_updates = updates;
    unsigned prior_requests = native_requests;
    set_search(event, search_browser, provider_media, "");
    assert(dispatch_search(event) == 0 && search_request.query[0] == '\0');
    azspd_cancel_search(search_request.ticket);
    assert(!azspd_search_current(search_request.ticket));
    assert(azspd_resume_search(search_request.ticket) == AZ_STALE && updates == prior_updates);
    assert(dispatch_search(event) == 0 && azspd_search_current(search_request.ticket));
    set_search(event, search_browser, 0x102, "");
    assert(azspd_resume_search(search_request.ticket) == AZ_STALE && updates == prior_updates);
    set_search(event, search_browser, provider_media, "");
    search_visible = false;
    assert(dispatch_search(event) == 0);
    assert(azspd_resume_search(search_request.ticket) == AZ_STALE);
    search_visible = true;
    set_search_category(4);
    assert(dispatch_search(event) == 0);
    assert(azspd_resume_search(search_request.ticket) == AZ_STALE);
    set_search_category(SEARCH_CATEGORY);
    assert(native_requests == prior_requests && updates == prior_updates);
    assert(dispatch_search(event) == 0);
    native_emits_event = false;
    assert(azspd_resume_search(search_request.ticket) == AZ_FAILED && updates == prior_updates);
    native_emits_event = true;
    assert(dispatch_search(event) == 0);
    native_search_status = UINT32_MAX;
    assert(azspd_resume_search(search_request.ticket) == AZ_FAILED && updates == ++prior_updates);
    accepts_search = false;
    assert(dispatch_search(event) == UINT32_MAX && updates == ++prior_updates);
    assert(!azspd_search_current(search_request.ticket));
}

static void deferred_loads(void) {
    struct azspd_track_id track = {.player = 1, .media = 0x142, .kind = 1, .track = 47};
    struct hierarchy_item items[2] = {{{1, 2, 3}}, {{4, 5, 6}}};
    struct hierarchy_view hierarchy = {.category = 8, .begin = items, .end = items + 2, .capacity = items + 2};
    uint32_t sort = 14;
    uint8_t order = 1;
    dispatch_index(&deck_objects[2], 6, 12, &track, &hierarchy, &sort, &order);
    assert(loads == 1 && !indexed && load_event.deck == 3 && load_event.id.track == 47);
    uint64_t first_ticket = load_event.ticket;
    dispatch_locate(&deck_objects[0], &track, &hierarchy, &sort, &order);
    uint64_t second_ticket = load_event.ticket;
    assert(second_ticket != first_ticket && loads == 2 && !located && load_event.deck == 1);
    assert(azspd_load_current(first_ticket) && azspd_load_current(second_ticket));
    sort = 87;
    order = 0;
    track.track = 99;
    items[1].words[2] = 66;
    assert(azspd_resume_load(first_ticket, NULL) == AZ_OK && indexed == 1);
    assert(delivered_id.track == 47 && delivered_loader == &deck_objects[2]);
    assert(delivered_index == 6 && delivered_count == 12 && delivered_category == 8);
    assert(delivered_sort == 14 && delivered_order == 1 && delivered_depth == 2);
    assert(delivered_items[1].words[2] == 6);
    assert(azspd_resume_load(first_ticket, NULL) == AZ_STALE);
    assert(azspd_resume_load(second_ticket, NULL) == AZ_OK && located == 1);
    assert(delivered_id.track == 47 && delivered_loader == &deck_objects[0]);
}

static void superseded_loads(void) {
    const struct azspd_track_id provider = {.player = 1, .media = 0x142, .kind = 1, .track = 47};
    const struct azspd_track_id stock = {.player = 1, .media = 0x102, .kind = 1, .track = 47};
    const struct hierarchy_view hierarchy = {.category = 8};
    const uint32_t sort = 0;
    const uint8_t order = 0;
    unsigned previous_loads = loads, previous_located = located;
    dispatch_locate(&deck_objects[3], &provider, &hierarchy, &sort, &order);
    uint64_t first_ticket = load_event.ticket;
    assert(azspd_load_current(first_ticket) && loads == previous_loads + 1);
    dispatch_locate(&deck_objects[3], &stock, &hierarchy, &sort, &order);
    assert(!azspd_load_current(first_ticket) && located == previous_located + 1);
    assert(delivered_id.media == 0x102 && azspd_resume_load(first_ticket, NULL) == AZ_STALE);
    dispatch_locate(&deck_objects[3], &provider, &hierarchy, &sort, &order);
    azspd_cancel_load(load_event.ticket);
    assert(!azspd_load_current(load_event.ticket));
    dispatch_locate(&deck_objects[3], &provider, &hierarchy, &sort, &order);
    const struct azspd_target target = {.id = stock, .category = 4, .order = 1, .sort = 15};
    assert(azspd_resume_load(load_event.ticket, &target) == AZ_OK && located == previous_located + 2);
    assert(delivered_id.media == 0x102 && delivered_category == 4 && delivered_depth == 0);
    assert(delivered_loader == &deck_objects[3] && delivered_sort == 15 && delivered_order == 1);
}

static void unavailable_loads(void) {
    const struct azspd_track_id provider = {.player = 1, .media = 0x142, .kind = 1, .track = 47};
    const struct hierarchy_view hierarchy = {.category = 8};
    const uint32_t sort = 0;
    const uint8_t order = 0;
    unsigned previous_loads = loads, previous_located = located;
    dispatch_locate(&deck_objects[1], &provider, &hierarchy, &sort, &order);
    assert(loads == previous_loads + 1 && located == previous_located);
    deck_objects[1] = 0;
    assert(azspd_resume_load(load_event.ticket, NULL) == AZ_STALE);
    deck_objects[1] = TRACK_LOADER_VTABLE;
    accepts_load = false;
    dispatch_locate(&deck_objects[1], &provider, &hierarchy, &sort, &order);
    assert(loads == previous_loads + 2 && !azspd_load_current(load_event.ticket));
    on_thread = 0;
    dispatch_locate(&deck_objects[1], &provider, &hierarchy, &sort, &order);
    on_thread = 1;
    assert(loads == previous_loads + 2 && located == previous_located + 1);
}

static void preview_requests(void) {
    struct azspd_track_id track = {.player = 1, .media = 0x142, .kind = 1, .track = 47};
    struct hierarchy_item item = {{1, 2, 3}};
    const struct hierarchy_view hierarchy = {.category = 8, .begin = &item, .end = &item + 1, .capacity = &item + 1};
    dispatch_preview(&preview_loader, &track, 0.5f, &hierarchy);
    assert(previews == 1 && preview_started == 0 && azspd_preview_current(preview_event.ticket));
    uint64_t first = preview_event.ticket;
    on_thread = 0;
    dispatch_preview_stop(&preview_loader);
    on_thread = 1;
    assert(preview_stopped == 1 && azspd_resume_preview(first) == AZ_STALE);
    dispatch_preview(&preview_loader, &track, 0.25f, &hierarchy);
    track.track = 23;
    item.words[2] = 8;
    assert(azspd_resume_preview(preview_event.ticket) == AZ_OK && preview_started == 1);
    assert(preview_offset == 0.25f && delivered_depth == 1 && delivered_category == 8);
    assert(delivered_id.track == 47 && delivered_items[0].words[2] == 3);
    assert(azspd_resume_preview(preview_event.ticket) == AZ_STALE);
    dispatch_preview(&preview_loader, &track, 0.5f, &hierarchy);
    dispatch_preview_unload(&preview_loader, &track.media);
    assert(preview_unloaded == 1 && azspd_resume_preview(preview_event.ticket) == AZ_STALE);
    dispatch_preview(&preview_loader, &track, 0.5f, &hierarchy);
    dispatch_preview_cancel(&preview_loader, &track);
    assert(preview_cancelled == 1 && azspd_resume_preview(preview_event.ticket) == AZ_STALE);
    dispatch_preview(&preview_loader, &track, 0.5f, &hierarchy);
    uint64_t superseded = preview_event.ticket;
    track.media = 0x102;
    dispatch_preview(&preview_loader, &track, 0, &hierarchy);
    assert(preview_started == 2 && delivered_id.media == 0x102);
    assert(azspd_resume_preview(superseded) == AZ_STALE);
}

static void unavailable_preview(void) {
    const struct azspd_track_id track = {.player = 1, .media = 0x142, .kind = 1, .track = 47};
    const struct hierarchy_view hierarchy = {.category = 8};
    dispatch_preview(&preview_loader, &track, 0, &hierarchy);
    preview_loader = 0;
    assert(azspd_resume_preview(preview_event.ticket) == AZ_STALE);
    preview_loader = 1;
    unsigned previous_starts = preview_started;
    dispatch_preview(&preview_loader, &track, 0, &hierarchy);
    assert(preview_started == previous_starts + 1);
    preview_loader = PREVIEW_VTABLE;
    accepts_preview = false;
    dispatch_preview(&preview_loader, &track, 0, &hierarchy);
    assert(!azspd_preview_current(preview_event.ticket));
}

static struct azstatus_native_receipt deck_seed;
static struct azspd_track_id deck_track;
static bool seed_available = true, supersede_at_match;

static int read_deck_seed(unsigned deck, struct azstatus_native_receipt *out) {
    assert(deck == 1 && out->size == sizeof(*out) && out->version == 1);
    if (!seed_available)
        return 0;
    *out = deck_seed;
    return 1;
}

static int match_deck_track(unsigned deck, uint64_t epoch, uint64_t load, const unsigned char *track) {
    assert(deck == 1 && epoch == deck_seed.epoch && load == deck_seed.load_generation);
    if (supersede_at_match)
        ++deck_seed.epoch;
    return !memcmp(track, &deck_track, TRACK_ID_BYTES);
}

static const struct azstatus_native_v1 deck_api = {
    .size = sizeof(deck_api),
    .version = 1,
    .load_seed = read_deck_seed,
    .track_matches = match_deck_track,
};

static struct azstatus_native_receipt deck_event(int state, uint64_t epoch) {
    return (struct azstatus_native_receipt) {
        .size = sizeof(deck_seed),
        .version = 1,
        .kind = AZN_LOAD,
        .deck = 1,
        .a = state,
        .valid = state == AZN_LOADED,
        .unit = 12,
        .epoch = epoch,
        .load_generation = state == AZN_LOADED ? epoch * 2 : 0,
    };
}

static void track_lifecycle(void) {
    const struct azspd_track_id track = {.player = 1, .media = 0x142, .kind = 1, .track = 47};
    struct azspd_deck_watch watch = {.id = track, .deck = 2, .unit = 12, .epoch = 4};
    deck_track = track;
    struct azstatus_native_receipt receipt = deck_event(AZN_LOADED, 4);
    deck_seed = receipt;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_PENDING);
    receipt = deck_event(AZN_EMPTY, 5);
    deck_seed = receipt;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_PENDING);
    receipt = deck_event(AZN_LOADED, 6);
    deck_seed = receipt;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_ACCEPTED && watch.epoch == 6);
    receipt = deck_event(AZN_UNKNOWN, 7);
    deck_seed = receipt;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_ACCEPTED);
    receipt = deck_event(AZN_FAILED, 8);
    deck_seed = receipt;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_ACCEPTED);
    receipt = deck_event(AZN_EMPTY, 9);
    deck_seed = receipt;
    seed_available = false;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_ACCEPTED);
    seed_available = true;
    receipt.unit = 13;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_ACCEPTED);
    receipt = deck_seed;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_RETIRED);
}

static void stable_replacement(void) {
    struct azspd_deck_watch watch = {
        .id = {.player = 1, .media = 0x142, .kind = 1, .track = 47},
        .deck = 2,
    };
    struct azstatus_native_receipt receipt = deck_event(AZN_LOADED, 12);
    deck_track = watch.id;
    deck_seed = receipt;
    assert(azspd_deck_observe(&watch, &deck_api, NULL) == AZSPD_DECK_PENDING);
    assert(azspd_deck_observe(&watch, NULL, &receipt) == AZSPD_DECK_PENDING);
    assert(azspd_deck_observe(NULL, &deck_api, &receipt) == AZSPD_DECK_PENDING);
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_ACCEPTED && watch.unit == 12);
    receipt = deck_event(AZN_LOADED, 13);
    deck_seed = receipt;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_ACCEPTED && watch.epoch == 13);
    receipt = deck_event(AZN_LOADED, 14);
    deck_seed = receipt;
    deck_track.media = 0x102;
    supersede_at_match = true;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_ACCEPTED && watch.epoch == 13);
    supersede_at_match = false;
    receipt = deck_seed;
    assert(azspd_deck_observe(&watch, &deck_api, &receipt) == AZSPD_DECK_RETIRED);
}

int main(void) {
    install_fixture();
    native_search_requests();
    cancelled_search();
    deferred_loads();
    superseded_loads();
    unavailable_loads();
    preview_requests();
    unavailable_preview();
    track_lifecycle();
    stable_replacement();
    puts("source provider dispatch ok");
    return 0;
}
