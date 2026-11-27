/* SPDX-License-Identifier: GPL-3.0-only */
#include "source_provider_lifetime.c"

#include <assert.h>
#include <stdio.h>

static const char path[] = "/tmp/private/source-file/track.m4a";
static const char other[] = "/tmp/unrelated/file.m4a";
static uint64_t current;
static unsigned starts, deletes, destroys;
static bool flag_reject_slot, flag_reject_code, check_construct, check_destroy;
#if defined(__linux__) && defined(__EXCEPTIONS)
static bool unwind_construction;
#endif

static void write_path(void *object, const char *name) {
    memcpy((unsigned char *) object + SOURCE_INFO_PATH, &name, sizeof name);
}

static void fixture_start(void *object, const void *file, void *disposer) {
    assert(!disposer);
    if (check_construct) {
        struct azspd_path_state state;
        assert(azspd_path_status(current, &state) == AZ_OK);
        assert(state.owners == 1 && !state.opened && !state.preview_opened);
        assert(azspd_path_unwatch(current) == AZ_BUSY);
    }
#if defined(__linux__) && defined(__EXCEPTIONS)
    if (unwind_construction) {
        assert(azspd_path_unwatch(current) == AZ_BUSY);
        pthread_exit(NULL);
    }
#endif
    ++starts;
    write_path(object, source_path(file, 0));
}

static void assert_busy_during_destroy(void) {
    if (!check_destroy)
        return;
    struct azspd_path_state state;
    assert(azspd_path_status(current, &state) == AZ_OK && state.owners >= 1);
    assert(azspd_path_unwatch(current) == AZ_BUSY);
}

static void fixture_destroy(void *object) {
    ++destroys;
    assert_busy_during_destroy();
    write_path(object, NULL);
}

static void fixture_delete(void *object) {
    ++deletes;
    assert_busy_during_destroy();
    write_path(object, NULL);
}

static bool patch_slot(uintptr_t addr, uintptr_t expected, uintptr_t replacement) {
    if (addr == SOURCE_INFO_DTOR_SLOT) {
        assert(expected == SOURCE_INFO_DTOR && replacement == (uintptr_t) native_source_destroy);
        return !flag_reject_slot;
    }
    assert(addr == SOURCE_INFO_DELETE_SLOT);
    assert(expected == SOURCE_INFO_DELETE && replacement == (uintptr_t) native_source_delete);
    return true;
}

static int patch_code(uintptr_t addr, void *replacement, void **original) {
    assert(addr == SOURCE_INFO_CTOR && replacement == (void *) native_source_construct);
    *original = (void *) fixture_start;
    return !flag_reject_code;
}

static void install_fixture(void) {
    uint64_t ignored;
    assert(azspd_path_watch(path, &ignored) == AZ_UNSUPPORTED);
    assert(azspd_paths_install(1, patch_code, patch_slot) == AZ_UNSUPPORTED);
    assert(azspd_paths_install(2, NULL, patch_slot) == AZ_INVALID);
    flag_reject_slot = true;
    assert(azspd_paths_install(2, patch_code, patch_slot) == AZ_UNSUPPORTED);
    flag_reject_slot = false;
    flag_reject_code = true;
    assert(azspd_paths_install(2, patch_code, patch_slot) == AZ_UNSUPPORTED);
    flag_reject_code = false;
    assert(azspd_paths_install(2, patch_code, patch_slot) == AZ_OK);
    assert(azspd_paths_install(2, patch_code, patch_slot) == AZ_BUSY);
    source_destroy = fixture_destroy;
    source_delete = fixture_delete;
}

static void exact_path_owners(void) {
    unsigned char first[0x80], second[0x80], unknown[0x80];
    struct azspd_path_state state = {0};
    assert(azspd_path_watch(path, &current) == AZ_OK);
    uint64_t duplicate;
    assert(azspd_path_watch(path, &duplicate) == AZ_LIMIT);
    const char *stream = path, *unrelated = other, *empty = "";
    check_construct = true;
    source_from(first, &stream, NULL, BROWSE_PREVIEW_SOURCE_RETURN);
    check_construct = false;
    native_source_construct(second, &stream, NULL);
    native_source_construct(unknown, &unrelated, NULL);
    assert(starts == 3);
    assert(azspd_path_status(current, &state) == AZ_OK);
    assert(state.opened == 2 && state.owners == 2 && state.preview_opened == 1 && !state.uncertain);
    native_source_delete(unknown);
    native_source_construct(unknown, &empty, NULL);
    native_source_destroy(unknown);
    assert(azspd_path_status(current, &state) == AZ_OK && state.owners == 2);
    assert(azspd_path_unwatch(current) == AZ_BUSY);
    check_destroy = true;
    native_source_delete(first);
    assert(azspd_path_status(current, &state) == AZ_OK && state.owners == 1);
    native_source_destroy(second);
    check_destroy = false;
    assert(azspd_path_status(current, &state) == AZ_OK);
    assert(state.opened == 2 && state.owners == 0 && state.preview_opened == 1 && !state.uncertain);
    assert(deletes == 2 && destroys == 2);
    assert(azspd_path_unwatch(current) == AZ_OK);
    assert(azspd_path_status(current, &state) == AZ_STALE);
    assert(azspd_path_unwatch(current) == AZ_STALE);
}

#if defined(__linux__) && defined(__EXCEPTIONS)
static void *undelivered_source(void *argument) {
    const char *audio = argument;
    unsigned char native[0x80] = {0};
    source_from(native, &audio, NULL, BROWSE_PREVIEW_SOURCE_RETURN);
    assert(!"fixture constructor should unwind");
    return NULL;
}

static void constructor_unwind(void) {
    assert(azspd_path_watch(path, &current) == AZ_OK);
    unsigned char existing[0x80] = {0};
    const char *audio = path;
    native_source_construct(existing, &audio, NULL);
    unwind_construction = true;
    pthread_t worker;
    assert(pthread_create(&worker, NULL, undelivered_source, (void *) path) == 0);
    assert(pthread_join(worker, NULL) == 0);
    unwind_construction = false;

    struct azspd_path_state state;
    assert(azspd_path_status(current, &state) == AZ_OK);
    assert(state.opened == 1 && state.owners == 1 && !state.preview_opened && !state.uncertain);
    native_source_destroy(existing);
    assert(azspd_path_unwatch(current) == AZ_OK);
}
#endif

static void preserve_unqualified_path(void) {
    assert(azspd_path_watch(path, &current) == AZ_OK);
    unsigned char native[0x80];
    write_path(native, path);
    native_source_delete(native);
    struct azspd_path_state state;
    assert(azspd_path_status(current, &state) == AZ_OK);
    assert(!state.opened && !state.owners && state.uncertain);
    assert(azspd_path_unwatch(current) == AZ_BUSY);
    uint64_t clean;
    assert(azspd_path_watch(other, &clean) == AZ_OK);
    assert(azspd_path_unwatch(clean) == AZ_OK);
}

static void invalid_inputs(void) {
    uint64_t ignored;
    assert(azspd_path_watch("relative", &ignored) == AZ_INVALID);
    assert(azspd_path_status(current, NULL) == AZ_INVALID);
    char oversized[AZ_SOURCE_PATH_BYTES + 1];
    memset(oversized, 'x', sizeof oversized);
    oversized[0] = '/';
    oversized[AZ_SOURCE_PATH_BYTES] = 0;
    assert(azspd_path_watch(oversized, &ignored) == AZ_INVALID);
}

static bool remove_controller = true;

static void write_member(void *object, size_t offset, const void *value, size_t size) {
    memcpy((unsigned char *) object + offset, value, size);
}

static void fixture_analysis_remove(void *manager, const unsigned char *id) {
    assert(id);
    if (remove_controller) {
        unsigned empty = 0;
        write_member(manager, ANALYSIS_COUNT, &empty, sizeof(empty));
    }
}

static int patch_analysis(uintptr_t addr, void *replacement, void **original) {
    assert(addr == ANALYSIS_DROP && replacement == (void *) observe_analysis_removal);
    *original = (void *) fixture_analysis_remove;
    return !flag_reject_code;
}

static void prepare_manager(void *manager, struct analysis_owner *owner, unsigned count) {
    uintptr_t vtable = ANALYSIS_MANAGER_VT;
    write_member(manager, 0, &vtable, sizeof(vtable));
    write_member(manager, ANALYSIS_COUNT, &count, sizeof(count));
    write_member(manager, ANALYSIS_CONTROLLERS, &owner, sizeof(owner));
}

static void analysis_terminal(void) {
    unsigned char raw[12] = {1, 0, 0, 0, 0x42, 1, 2, 0, 48};
    unsigned char requester[12] = {14, 0, 0, 0, 2, 0, 0, 0, 0x12, 0, 0, 0x40};
    assert(azspd_analysis_watch(raw, requester, &current) == AZ_UNSUPPORTED);
    assert(azspd_analysis_install(1, patch_analysis) == AZ_UNSUPPORTED);
    assert(azspd_analysis_install(2, NULL) == AZ_INVALID);
    flag_reject_code = true;
    assert(azspd_analysis_install(2, patch_analysis) == AZ_UNSUPPORTED);
    flag_reject_code = false;
    assert(azspd_analysis_install(2, patch_analysis) == AZ_OK);
    assert(azspd_analysis_install(2, patch_analysis) == AZ_BUSY);
    assert(azspd_analysis_watch(raw, NULL, &current) == AZ_INVALID);
    assert(azspd_analysis_watch(raw, requester, &current) == AZ_OK);
    assert(azspd_analysis_watch(raw, requester, &current) == AZ_BUSY);
    assert(azspd_analysis_status(current) == AZ_PENDING);
    azspd_analysis_forget(current);
    assert(azspd_analysis_status(current) == AZ_PENDING);

    unsigned char manager[0x340] = {0}, controller[0xe0] = {0};
    const unsigned char *entries = requester;
    const unsigned count = 1;
    struct analysis_owner owner = {.object = controller};
    write_member(controller, CONTROLLER_TRACK, raw, sizeof(raw));
    write_member(controller, CONTROLLER_REQUESTERS, &entries, sizeof(entries));
    write_member(controller, CONTROLLER_REQUEST_COUNT, &count, sizeof(count));
    prepare_manager(manager, &owner, 4097);
    observe_analysis_removal(manager, raw);
    assert(azspd_analysis_status(current) == AZ_PENDING);
    prepare_manager(manager, &owner, 1);
    requester[8] = 0x03;
    observe_analysis_removal(manager, raw);
    assert(azspd_analysis_status(current) == AZ_PENDING);
    prepare_manager(manager, &owner, 1);
    requester[8] = 0x12;
    remove_controller = false;
    observe_analysis_removal(manager, raw);
    assert(azspd_analysis_status(current) == AZ_PENDING);
    remove_controller = true;
    observe_analysis_removal(manager, raw);
    assert(azspd_analysis_status(current) == AZ_OK);
    azspd_analysis_forget(current);
    assert(azspd_analysis_status(current) == AZ_STALE);
}

static void physical_analysis_terminal(void) {
    unsigned char raw[12] = {1, 0, 0, 0, 0x02, 1, 2, 0, 48};
    const unsigned char requester[12] = {14, 0, 0, 0, 2, 0, 0, 0, 0x34, 0, 0, 0x40};
    unsigned char unrelated[12];
    memcpy(unrelated, raw, sizeof unrelated);
    unrelated[4] = 0x62;
    assert(azspd_analysis_watch(unrelated, requester, &current) == AZ_INVALID);
    unrelated[4] = 0x02;
    unrelated[5] = 0;
    assert(azspd_analysis_watch(unrelated, requester, &current) == AZ_INVALID);
    unrelated[5] = 1;
    unrelated[6] = 1;
    assert(azspd_analysis_watch(unrelated, requester, &current) == AZ_INVALID);

    unsigned char manager[0x340] = {0}, controller[0xe0] = {0};
    const unsigned char *entries = requester;
    const unsigned count = 1;
    struct analysis_owner owner = {.object = controller};
    write_member(controller, CONTROLLER_REQUESTERS, &entries, sizeof(entries));
    write_member(controller, CONTROLLER_REQUEST_COUNT, &count, sizeof(count));
    for (unsigned port = 0; port < 2; ++port) {
        raw[4] = 0x02 + port * 0x20;
        assert(azspd_analysis_watch(raw, requester, &current) == AZ_OK);
        write_member(controller, CONTROLLER_TRACK, raw, sizeof(raw));
        memcpy(unrelated, raw, sizeof unrelated);
        unrelated[5] = 2; /* Same USB port with a different media generation. */
        prepare_manager(manager, &owner, 1);
        observe_analysis_removal(manager, unrelated);
        assert(azspd_analysis_status(current) == AZ_PENDING);
        prepare_manager(manager, &owner, 1);
        observe_analysis_removal(manager, raw);
        assert(azspd_analysis_status(current) == AZ_OK);
        azspd_analysis_forget(current);
        assert(azspd_analysis_status(current) == AZ_STALE);
    }
}

int main(void) {
    install_fixture();
    exact_path_owners();
#if defined(__linux__) && defined(__EXCEPTIONS)
    constructor_unwind();
#endif
    preserve_unqualified_path();
    invalid_inputs();
    analysis_terminal();
    physical_analysis_terminal();
    puts("source provider native file owners ok");
    return 0;
}
