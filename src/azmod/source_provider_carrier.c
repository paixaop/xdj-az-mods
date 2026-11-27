/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "source_provider_carrier.h"

#include "azmod-mod.h"
#include <dlfcn.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

static _Atomic uint64_t mounted_media;
static _Atomic unsigned mount_requested;
static char carrier_root[512];

#if defined(__aarch64__) && defined(__linux__)
/* ep147build.h also defines the host's installer helpers in this translation unit. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../common/ep147build.h"
#pragma GCC diagnostic pop

enum {
    CARRIER_STORAGE = 0x42,
    CARRIER_SOURCE_MODEL_VTABLE = 0x2dc82e0,
    CARRIER_DISPENSER_VTABLE = 0x2934fe0,
    CARRIER_SQL_DATABASE_VTABLE = 0x2b9d368,
    CARRIER_NEW = 0x426570,
    CARRIER_EASY_ID = 0x7c88f8,
    CARRIER_SET_THREAD = 0x7dbf28,
    CARRIER_BIND_DB_INFO = 0x7ddf80,
    CARRIER_REGISTER_DEPENDENCY = 0x2381d00,
    CARRIER_LOOKUP_DEPENDENCY = 0x2382cc0,
    CARRIER_WORKER = 0x1385378,
    CARRIER_WRAP_LIBRARY = 0x13860c8,
    CARRIER_MOUNT_OBSERVER = 0x1384ae8,
    CARRIER_NEW_REPOSITORY = 0x1974f50,
    CARRIER_WRAP_REPOSITORY = 0x1992aa8,
    CARRIER_START_REPOSITORY = 0x7d58b8,
    CARRIER_SELECT = 0x1be7e38,
    CARRIER_STRING = 0x24a1180,
    CARRIER_DROP_STRING = 0x2469aa0,
    CARRIER_MOUNT_NOTIFY = 0xbb25b0,
    CARRIER_MOUNT_START = 0xb91868,
    CARRIER_MOUNT_SET_PATH = 0xb91908,
    CARRIER_MOUNT_ACTIVATE = 0xb91af8,
    CARRIER_RECEIVE_MESSAGE = 0xe6b1c8,
    CARRIER_MESSAGE_CONSTRUCT = 0xf07768,
    CARRIER_MESSAGE_SET_UINT = 0xf02d18,
    CARRIER_MESSAGE_DESTROY = 0xf00de0,
    CARRIER_ROOT_MENU = 0x1000,
    CARRIER_ROOT_LOCATION = 0x10800,
    CARRIER_MESSAGE_SIZE = 0x40,
    CARRIER_WAKE_SENDER = 0x415a574b,
};

struct carrier_owner { void *object, *control; };
struct carrier_vector { struct carrier_owner *begin, *end, *capacity; };
struct carrier_list_node { struct carrier_list_node *next, *previous; void *server; };

struct carrier_detection {
    void *path;
    uint32_t device;
    uint8_t storage;
    void *filesystem;
    uint8_t flags, protection, count;
};

_Static_assert(offsetof(struct carrier_detection, storage) == 0xc, "native storage byte");
_Static_assert(offsetof(struct carrier_detection, filesystem) == 0x10, "native filesystem string");

/* Firmware retains a copy in each mount aggregator. The original references
 * keep the independently registered workers alive until process exit. */
static struct carrier_owner carrier_library, carrier_repository;
static void (*library_module)(void **);
static void (*repository_module)(void **);
static void (*library_aggregate)(void *, const struct carrier_vector *, const struct carrier_vector *);
static void (*repository_aggregate)(void *, const struct carrier_vector *, const struct carrier_vector *, const struct carrier_vector *);
static void (*library_sender)(void *, uint64_t);
static void (*repository_sender)(void *, uint64_t);
static void (*library_mount)(void **);
static unsigned (*storage_worker)(void *, unsigned, unsigned, const void *, const void *, unsigned, unsigned);
static uint64_t (*media_issuer)(void *, uint8_t, unsigned);
static void (*start_framework)(void *);
static void (*append_server_node)(struct carrier_list_node *, struct carrier_list_node *);
static void (*release_native_object)(void *, size_t);
static _Atomic unsigned carrier_enabled;
static _Atomic unsigned carrier_dispenser_ready;
static unsigned carrier_install_attempted;
static int carrier_repository_db_ready;
static _Atomic(void *) storage_controller;
static _Atomic uint64_t issued_media;
static _Atomic unsigned mount_queued;
static _Atomic(void *) carrier_pending_wake;

static void *carrier_pointer(const void *object, size_t offset) {
    void *value = NULL;
    if (object) memcpy(&value, (const unsigned char *) object + offset, sizeof value);
    return value;
}

static void carrier_put(void *object, size_t offset, uint64_t value) {
    memcpy((unsigned char *) object + offset, &value, sizeof value);
}

static uint64_t carrier_id(const char *name) {
    return ((uint64_t (*)(const char *)) CARRIER_EASY_ID)(name);
}

static void carrier_string(void **value, const char *text) {
    ((void (*)(void **, const char *)) CARRIER_STRING)(value, text);
}

static void carrier_string_drop(void **value) {
    ((void (*)(void **)) CARRIER_DROP_STRING)(value);
}

static int carrier_live(void) {
    return atomic_load_explicit(&carrier_enabled, memory_order_acquire) != 0;
}

static int carrier_create_owner(const struct carrier_owner *owner) {
    return carrier_live() && !owner->object;
}

/* The native factory returns std::unique_ptr in x8. Forward that out pointer
 * in x4 for C, then restore x8 before calling the original. */
__attribute__((used)) static void *library_factory;
__asm__(".text\n"
        ".type carrier_factory_entry,%function\n"
        "carrier_factory_entry: mov x4, x8\n"
        "b carrier_factory_hook\n"
        ".type carrier_call_factory,%function\n"
        "carrier_call_factory: mov x8, x4\n"
        "adrp x16, library_factory\n"
        "ldr x16, [x16, :lo12:library_factory]\n"
        "br x16\n");
extern void carrier_factory_entry(void);
extern void carrier_call_factory(const struct carrier_vector *, const void *, const uint8_t *, const uint8_t *, void **);

static void carrier_create_library(const struct carrier_vector *workers, const void *name, const uint8_t *config) {
    const uint8_t storage = CARRIER_STORAGE;
    void *created = NULL;
    carrier_call_factory(workers, name, &storage, config, &created);
    if (!created) return;
    ((void (*)(struct carrier_owner *, void **)) CARRIER_WRAP_LIBRARY)(&carrier_library, &created);
    if (created) ((void (**)(void *)) carrier_pointer(created, 0))[1](created);
}

__attribute__((used, visibility("hidden")))
void *carrier_factory_hook(const struct carrier_vector *workers, const void *name,
                           const uint8_t *storage, const uint8_t *config, void **result) {
    carrier_call_factory(workers, name, storage, config, result);
    if (*storage == 2 && carrier_create_owner(&carrier_library))
        carrier_create_library(workers, name, config);
    return result;
}

static void carrier_create_repository(void) {
    const uint8_t storage = CARRIER_STORAGE, config = 0;
    void *name;
    void *server = ((void *(*)(size_t)) CARRIER_NEW)(0x210);
    carrier_string(&name, "TInfRepSrvUsb3");
    ((void (*)(void *, const void *, const uint8_t *, const uint8_t *)) CARRIER_NEW_REPOSITORY)(server, &name, &storage, &config);
    carrier_string_drop(&name);
    carrier_repository.object = server;
    ((void (*)(void **, void *)) CARRIER_WRAP_REPOSITORY)(&carrier_repository.control, server);
}

static void carrier_add_worker(void *server, uint64_t owner) {
    carrier_put(server, 0x140, owner);
    carrier_put(server, 0x150, (uintptr_t) ((void *(*)(void *)) CARRIER_WORKER)(server));
}

static void carrier_configure_library(void **module, void *server) {
    const uint8_t storage = CARRIER_STORAGE;
    void *observer = ((void *(*)(size_t)) CARRIER_NEW)(0x18);
    ((void (*)(void *, const uint8_t *)) CARRIER_MOUNT_OBSERVER)(observer, &storage);
    carrier_add_worker(server, UINT64_C(0x40a6b12e49b2a2));
    void (**methods)(void *, void *) = carrier_pointer(server, 0);
    methods[0x88 / 8](server, observer);
    methods[0x90 / 8](server, module[0x3a]);
    methods[0x98 / 8](server, module[0x27]);
}

static void carrier_library_module(void **module) {
    library_module(module);
    if (carrier_library.object) carrier_configure_library(module, carrier_library.object);
}

static void carrier_repository_module(void **module) {
    if (carrier_create_owner(&carrier_repository)) carrier_create_repository();
    repository_module(module);
    void *server = carrier_repository.object;
    if (!server) return;
    carrier_add_worker(server, UINT64_C(0xf6f7150c992444));
    ((void (*)(void *, int, unsigned)) CARRIER_START_REPOSITORY)(server, 5, 0);
}

static void carrier_extend_vector(struct carrier_vector *out, struct carrier_owner *dest,
                                  const struct carrier_vector *source, size_t count, struct carrier_owner owner) {
    memcpy(dest, source->begin, count * sizeof *dest);
    dest[count] = owner;
    *out = (struct carrier_vector) { dest, dest + count + 1, dest + count + 1 };
}

static void carrier_library_aggregate(void *self, const struct carrier_vector *inputs, const struct carrier_vector *servers) {
    enum { stock_count = 17 };
    struct carrier_owner elements[stock_count + 1];
    struct carrier_vector extended;
    if (carrier_library.object && servers->end - servers->begin == stock_count) {
        carrier_extend_vector(&extended, elements, servers, stock_count, carrier_library);
        servers = &extended;
    }
    library_aggregate(self, inputs, servers);
}

static void carrier_repository_aggregate(void *self, const struct carrier_vector *clients,
                                         const struct carrier_vector *servers, const struct carrier_vector *streaming) {
    enum { stock_count = 5 };
    struct carrier_owner elements[stock_count + 1];
    struct carrier_vector extended;
    if (carrier_repository.object && servers->end - servers->begin == stock_count) {
        carrier_extend_vector(&extended, elements, servers, stock_count, carrier_repository);
        servers = &extended;
    }
    repository_aggregate(self, clients, servers, streaming);
}

static void carrier_bind_sender(void *server, uint64_t sender, const char *thread_name) {
    void (**methods)(void *, uint64_t) = carrier_pointer(server, 0);
    methods[0x58 / 8](server, sender);
    carrier_put(server, 0xe0, carrier_id("ClientServerFrameworkMountInfoReferencePtr"));
    carrier_put(server, 0xe8, carrier_id("ClientServerFrameworkLinkInfoReferencePtr"));
    uint64_t thread = carrier_id(thread_name);
    ((void (*)(const uint64_t *, void *)) CARRIER_SET_THREAD)(&thread, server);
}

static void carrier_library_sender(void *module, uint64_t sender) {
    library_sender(module, sender);
    void *server = carrier_library.object;
    if (!server) return;
    carrier_bind_sender(server, sender, "MusicLibraryServerThreadUsb3");
    carrier_put(server, 0x190, carrier_id("MusicLibrary->MySettingManager"));
    carrier_put(server, 0x1a0, carrier_id("MusicLibrary->TaggedTrackCacheService"));
    uint64_t info = carrier_id("MusicLibraryDataBaseInformationProviderUsb3");
    ((void (*)(const uint64_t *, void *)) CARRIER_BIND_DB_INFO)(&info, (unsigned char *) server + 0x160);
}

static int carrier_map_repository_db(uint64_t dependency) {
    uint64_t owner = carrier_id("MusicLibraryDataBaseInformationProviderUsb3");
    uint64_t (*lookup)(const uint64_t *) = (void *) CARRIER_LOOKUP_DEPENDENCY;
    uint64_t existing = lookup(&dependency);
    if (existing) return existing == owner;
    ((unsigned (*)(const uint64_t *, const uint64_t *)) CARRIER_REGISTER_DEPENDENCY)(&dependency, &owner);
    return lookup(&dependency) == owner;
}

static void carrier_repository_sender(void *module, uint64_t sender) {
    repository_sender(module, sender);
    void *server = carrier_repository.object;
    if (!server) return;
    carrier_bind_sender(server, sender, "TrackInfoRepositoryServerThreadUsb3");
    uint64_t dependency = carrier_id("MusicLibraryDataBaseInformationProviderUsb3Ptr");
    /* The persistent server resolves this alias to obtain the track's write identity. */
    carrier_repository_db_ready = carrier_map_repository_db(dependency);
    if (carrier_repository_db_ready) carrier_put(server, 0x170, dependency);
}

static uint16_t carrier_local_media(uint64_t media) {
    return (uint16_t) (media >> 32);
}

static uint64_t carrier_media_issuer(void *issuer, uint8_t storage, unsigned link) {
    uint64_t media = media_issuer(issuer, storage, link);
    if (storage == CARRIER_STORAGE)
        atomic_store_explicit(&issued_media, media, memory_order_release);
    return media;
}

static int carrier_sqlite_ready(void *server) {
    void *dispatcher = carrier_pointer(server, 0x1c8);
    void *handler = carrier_pointer(dispatcher, 0x20);
    void *updater = carrier_pointer(handler, 0x30);
    void *library = carrier_pointer(updater, 8);
    void *database = carrier_pointer(library, 0x30);
    return (uintptr_t) carrier_pointer(database, 0) == CARRIER_SQL_DATABASE_VTABLE;
}

static int carrier_mount_matches(void *server, uint16_t selected, uint64_t media) {
    uint16_t mounted;
    memcpy(&mounted, (const unsigned char *) server + 0x188, sizeof mounted);
    return selected == mounted && mounted == carrier_local_media(media);
}

static void carrier_commit_mount(void *server, uint16_t selected) {
    uint64_t media = atomic_load_explicit(&issued_media, memory_order_acquire);
    if (carrier_mount_matches(server, selected, media) && carrier_sqlite_ready(server))
        atomic_store_explicit(&mounted_media, media, memory_order_release);
}

static void carrier_library_mount(void **request) {
    void *server = request[0];
    uint16_t media;
    memcpy(&media, request + 1, sizeof media);
    library_mount(request);
    if (server == carrier_library.object) carrier_commit_mount(server, media);
}

static unsigned carrier_start_progress(void *controller, const void *path, const void *filesystem) {
    void *progress = carrier_pointer(controller, 0x518);
    if (!progress) return 0;
    ((void (*)(void *, unsigned, unsigned, unsigned)) CARRIER_MOUNT_START)(progress, 1, CARRIER_STORAGE, 0);
    ((void (*)(void *, unsigned, unsigned, const void *, const void *, unsigned)) CARRIER_MOUNT_SET_PATH)(
        progress, 1, CARRIER_STORAGE, path, filesystem, 0);
    ((void (*)(void *, unsigned, unsigned, unsigned, const void *, const void *, unsigned, unsigned)) CARRIER_MOUNT_ACTIVATE)(
        progress, 1, CARRIER_STORAGE, 0, path, filesystem, 0, 1);
    return 1;
}

static int carrier_own_mount(unsigned storage, const void *path) {
    if (storage != CARRIER_STORAGE || !atomic_load_explicit(&mount_requested, memory_order_acquire)) return 0;
    return strcmp(*(const char * const *) path, carrier_root) == 0;
}

static void carrier_queue_mount(void *controller) {
    struct carrier_detection detection = {.device = CARRIER_STORAGE, .storage = CARRIER_STORAGE, .count = 1};
    carrier_string(&detection.path, carrier_root);
    carrier_string(&detection.filesystem, "vfat");
    ((void (*)(void *, unsigned, const struct carrier_detection *)) CARRIER_MOUNT_NOTIFY)(
        controller, CARRIER_STORAGE, &detection);
    carrier_string_drop(&detection.filesystem);
    carrier_string_drop(&detection.path);
}

static void carrier_try_mount(void *controller) {
    if (!atomic_load_explicit(&mount_requested, memory_order_acquire)) return;
    if (!atomic_exchange_explicit(&mount_queued, 1, memory_order_acq_rel)) carrier_queue_mount(controller);
}

static void carrier_note_controller(void *controller, unsigned storage) {
    if (storage != 9 || !carrier_live()) return;
    atomic_store_explicit(&storage_controller, controller, memory_order_release);
    carrier_try_mount(controller);
}

static unsigned carrier_storage_worker(void *controller, unsigned link, unsigned storage, const void *path,
                                       const void *filesystem, unsigned flags, unsigned protection) {
    if (carrier_own_mount(storage, path) && controller == atomic_load_explicit(&storage_controller, memory_order_acquire))
        return carrier_start_progress(controller, path, filesystem);
    unsigned status = storage_worker(controller, link, storage, path, filesystem, flags, protection);
    carrier_note_controller(controller, storage);
    return status;
}

static void carrier_add_dispatch_route(void *dispenser, void *server) {
    unsigned char *object = dispenser;
    struct carrier_list_node *sentinel = (void *) (object + 0x2c0);
    struct carrier_list_node *node = ((void *(*)(size_t)) CARRIER_NEW)(sizeof *node);
    node->server = server;
    append_server_node(node, sentinel);
    size_t *count = (void *) (object + 0x2d0);
    ++*count;
}

static void *carrier_dispenser(void *framework) {
    void *module = carrier_pointer(framework, 0x18);
    void *dispenser = carrier_pointer(module, 0);
    if ((uintptr_t) carrier_pointer(dispenser, 0) != CARRIER_DISPENSER_VTABLE) return NULL;
    return dispenser;
}

/* This is the same append operation as 0x7d88b0, using the already owned native
 * servers. The framework starts dispatching only after this entry returns. */
static int carrier_servers_ready(void) {
    return carrier_library.object && carrier_repository.object && carrier_repository_db_ready;
}

static void carrier_add_dispatch_routes(void *framework) {
    void *dispenser = carrier_dispenser(framework);
    if (!dispenser || !carrier_servers_ready()) return;
    carrier_add_dispatch_route(dispenser, carrier_library.object);
    carrier_add_dispatch_route(dispenser, carrier_repository.object);
    atomic_store_explicit(&carrier_dispenser_ready, 1, memory_order_release);
}

static void carrier_start_framework(void *framework) {
    if (!atomic_load_explicit(&carrier_dispenser_ready, memory_order_acquire))
        carrier_add_dispatch_routes(framework);
    start_framework(framework);
}

struct carrier_hook {
    uintptr_t address;
    void *entry, **original;
    uint32_t instructions[4];
};

#define CARRIER_HOOK(address, name, a, b, c, d) \
    { (address), carrier_##name, (void **) &(name), {(a), (b), (c), (d)} }

static const struct carrier_hook carrier_hooks[] = {
    {0x137bc60, carrier_factory_entry, &library_factory, {0xa9bd7bfd, 0x910003fd, 0x39400044, 0xf9400025}},
    CARRIER_HOOK(0x117bf08, library_aggregate, 0xa9bc7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3),
    CARRIER_HOOK(0x13809f0, library_module, 0xa9a17bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3),
    CARRIER_HOOK(0x198f260, repository_module, 0xd10943ff, 0xa9007bfd, 0x910003fd, 0xa90153f3),
    CARRIER_HOOK(0x1927fc8, repository_aggregate, 0xa9bb7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3),
    CARRIER_HOOK(0x7d4540, library_sender, 0xa9bd7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f4),
    CARRIER_HOOK(0x7d53a8, repository_sender, 0xa9bd7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f4),
    CARRIER_HOOK(0xbc00e0, storage_worker, 0xa9b47bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3),
    CARRIER_HOOK(0x111c2e8, media_issuer, 0xa9b97bfd, 0x910003fd, 0xa90153f3, 0x9101e014),
    CARRIER_HOOK(0x1376c98, library_mount, 0xa9ae7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3),
    CARRIER_HOOK(0x7c8b60, start_framework, 0xa9be7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f4),
};

#undef CARRIER_HOOK

static int carrier_verify_dependency_calls(void) {
    static const uint32_t register_entry[] = {0xa9b67bfd, 0x910003fd, 0xa90153f3, 0x9000e113};
    static const uint32_t lookup_entry[] = {0xa9bd7bfd, 0x910003fd, 0xa90153f3, 0xf000e0f4};
    return !memcmp((const void *) CARRIER_REGISTER_DEPENDENCY, register_entry, sizeof register_entry) &&
           !memcmp((const void *) CARRIER_LOOKUP_DEPENDENCY, lookup_entry, sizeof lookup_entry);
}

static int carrier_verify(void) {
    for (size_t i = 0; i < sizeof carrier_hooks / sizeof carrier_hooks[0]; ++i) {
        const struct carrier_hook *hook = carrier_hooks + i;
        if (memcmp((const void *) hook->address, hook->instructions, sizeof hook->instructions)) return 0;
    }
    return carrier_verify_dependency_calls();
}

static int carrier_patch_hooks(azsp_carrier_patch_fn patch) {
    for (size_t i = 0; i < sizeof carrier_hooks / sizeof carrier_hooks[0]; ++i) {
        const struct carrier_hook *hook = carrier_hooks + i;
        if (patch(hook->address, hook->entry, hook->original) != 1) return 0;
    }
    return 1;
}

static int carrier_prepare_hooks(azsp_carrier_patch_fn patch) {
    append_server_node = dlsym(RTLD_DEFAULT, "_ZNSt8__detail15_List_node_base7_M_hookEPS0_");
    release_native_object = dlsym(RTLD_DEFAULT, "_ZdlPvm");
    if (!append_server_node || !release_native_object) return 0;
    if (!carrier_verify()) return 0;
    return carrier_patch_hooks(patch);
}

static int carrier_claim_install(int build) {
    if (build != EPB_V200 || carrier_install_attempted) return 0;
    carrier_install_attempted = 1;
    return 1;
}

static void *carrier_create_wake(uint64_t media) {
    void *message = ((void *(*)(size_t)) CARRIER_NEW)(CARRIER_MESSAGE_SIZE);
    ((void (*)(void *, unsigned, unsigned)) CARRIER_MESSAGE_CONSTRUCT)(message, CARRIER_ROOT_MENU, 1);
    unsigned location = CARRIER_ROOT_LOCATION | (unsigned) (media & 0xff);
    ((void (*)(void *, unsigned, unsigned)) CARRIER_MESSAGE_SET_UINT)(message, 0, location);
    return message;
}

static void carrier_destroy_message(void *message) {
    ((void (*)(void *)) CARRIER_MESSAGE_DESTROY)(message);
    release_native_object(message, CARRIER_MESSAGE_SIZE);
}

static int carrier_reserve_wake(void *message) {
    void *empty = NULL;
    return atomic_compare_exchange_strong_explicit(&carrier_pending_wake, &empty, message,
                                                   memory_order_release, memory_order_relaxed);
}

static int carrier_take_wake(void *message) {
    return atomic_compare_exchange_strong_explicit(&carrier_pending_wake, &message, NULL,
                                                   memory_order_acq_rel, memory_order_relaxed);
}

static int carrier_queue_wake(void *server, uint64_t media) {
    void *message = carrier_create_wake(media);
    if (carrier_reserve_wake(message)) {
        int declined = ((int (*)(void *, void *, unsigned)) CARRIER_RECEIVE_MESSAGE)(server, message, CARRIER_WAKE_SENDER);
        if (!declined) return AZ_OK;
        (void) carrier_take_wake(message);
    }
    carrier_destroy_message(message);
    return AZ_BUSY;
}
#endif

int azsp_carrier_install(int build, azsp_carrier_patch_fn patch) {
#if defined(__aarch64__) && defined(__linux__)
    if (!patch || !carrier_claim_install(build)) return AZ_UNSUPPORTED;
    if (!carrier_prepare_hooks(patch)) return AZ_UNSUPPORTED;
    atomic_store_explicit(&carrier_enabled, 1, memory_order_release);
    return AZ_OK;
#else
    (void) build;
    (void) patch;
    return AZ_UNSUPPORTED;
#endif
}

static int carrier_mount_state(const char *root) {
    if (!atomic_load_explicit(&mount_requested, memory_order_acquire)) return AZ_EMPTY;
    if (strcmp(root, carrier_root)) return AZ_BUSY;
    return azsp_carrier_media() ? AZ_OK : AZ_PENDING;
}

static int carrier_root_status(const char *root) {
    if (!root || root[0] != '/') return AZ_INVALID;
    return strlen(root) < sizeof carrier_root ? AZ_OK : AZ_LIMIT;
}

static int carrier_mount_start(const char *root) {
#if defined(__aarch64__) && defined(__linux__)
    if (!carrier_live()) return AZ_UNSUPPORTED;
    memcpy(carrier_root, root, strlen(root) + 1);
    atomic_store_explicit(&mount_requested, 1, memory_order_release);
    void *controller = atomic_load_explicit(&storage_controller, memory_order_acquire);
    if (controller) carrier_try_mount(controller);
    return AZ_PENDING;
#else
    (void) root;
    return AZ_UNSUPPORTED;
#endif
}

int azsp_carrier_mount(const char *root) {
    int validation = carrier_root_status(root);
    if (validation != AZ_OK) return validation;
    int status = carrier_mount_state(root);
    return status == AZ_EMPTY ? carrier_mount_start(root) : status;
}

uint64_t azsp_carrier_media(void) {
#if defined(__aarch64__) && defined(__linux__)
    if (!atomic_load_explicit(&carrier_dispenser_ready, memory_order_acquire)) return 0;
#endif
    return atomic_load_explicit(&mounted_media, memory_order_acquire);
}

const char *azsp_carrier_root(void) {
    return azsp_carrier_media() ? carrier_root : NULL;
}

uint16_t azsp_carrier_library_media(const void *server) {
#if defined(__aarch64__) && defined(__linux__)
    if (!server || server != carrier_library.object) return 0;
    return carrier_local_media(azsp_carrier_media());
#else
    (void) server;
    return 0;
#endif
}

int azsp_carrier_select(void *source_list_model) {
#if defined(__aarch64__) && defined(__linux__)
    if (!source_list_model || (uintptr_t) carrier_pointer(source_list_model, 0) != CARRIER_SOURCE_MODEL_VTABLE)
        return AZ_INVALID;
    uint64_t media = azsp_carrier_media();
    if (!media) return AZ_PENDING;
    ((void (*)(void *, unsigned, const uint64_t *)) CARRIER_SELECT)(source_list_model, 0, &media);
    return AZ_OK;
#else
    (void) source_list_model;
    return AZ_UNSUPPORTED;
#endif
}

int azsp_carrier_wake(void) {
#if defined(__aarch64__) && defined(__linux__)
    if (!carrier_live()) return AZ_UNSUPPORTED;
    uint64_t media = azsp_carrier_media();
    return media ? carrier_queue_wake(carrier_library.object, media) : AZ_PENDING;
#else
    return AZ_UNSUPPORTED;
#endif
}

int azsp_carrier_consume_wake(void *server, void *message, unsigned sender) {
#if defined(__aarch64__) && defined(__linux__)
    if (server != carrier_library.object || sender != CARRIER_WAKE_SENDER) return 0;
    if (!message || !carrier_take_wake(message)) return 0;
    /* The native queue task releases its queue node before invoking onMessage.
     * The normal RequestDispatcher owns the message's destruction from here. */
    carrier_destroy_message(message);
    return 1;
#else
    (void) server;
    (void) message;
    (void) sender;
    return 0;
#endif
}
