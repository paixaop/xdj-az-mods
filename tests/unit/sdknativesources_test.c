#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent native owner/RTTI layouts expressed in C. No native firmware code
 * runs unless the explicit scratch-database mode is requested. */
#include "native_abi_fixture.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
typedef struct SqliteDataBase {
    const void *vtable;
    uint16_t media;
    unsigned char pad[0x38 - 10];
    void *connection;
} SqliteDataBase;
typedef struct MountPoint {
    const void *vtable;
    uint16_t media;
    unsigned char pad[0x30 - 10];
    struct fixture_owner database;
} MountPoint;
typedef MountPoint SqliteMountPoint;
typedef struct InformationUpdater {
    const void *vtable;
    struct fixture_owner mount;
} InformationUpdater;
typedef struct MusicLibraryServer {
    const void *vtable;
    unsigned char pad[0x1d0 - 8];
    pthread_t worker;
    _Atomic unsigned affinity_calls;
    bool throws;
} MusicLibraryServer;
static _Atomic unsigned mount_destroyed, database_destroyed;
static bool fixture_needs_async(void *opaque) {
    MusicLibraryServer *s = opaque;
    ++s->affinity_calls;
    if (s->throws) fixture_raise();
    return !pthread_equal(s->worker, pthread_self());
}
struct FixtureType {
    const void *vtable;
    const char *name;
};
static const char bad_mount_name[64] = "N13music_library14NoDBMountPointE";
struct FixtureTable {
    uintptr_t offset;
    const struct FixtureType *type;
    const void *slots[11];
};
static const struct FixtureType db_type = {NULL, "N13music_library14SqliteDataBaseE"}, mount_type = {NULL, "N13music_library16SqliteMountPointE"}, bad_mount_type = {NULL, bad_mount_name}, updater_type = {NULL, "N13music_library18InformationUpdaterE"}, server_type = {NULL, "N13music_library18MusicLibraryServerE"};
static const struct FixtureTable db_table = {0, &db_type, {0}}, mount_table = {0, &mount_type, {0}}, bad_mount_table = {0, &bad_mount_type, {0}}, updater_table = {0, &updater_type, {0}}, server_table = {0, &server_type, {[5] = (const void *) fixture_needs_async}};
static void source_owner_set(struct fixture_owner *dst, struct fixture_owner src) {
    struct fixture_owner copy = fixture_copy(src);
    fixture_drop(dst);
    *dst = copy;
}
static void db_dispose(void *c) {
    (void) c;
    ++database_destroyed;
}
FIXTURE_VTABLE(db_control, db_dispose);
static void mount_dispose(void *c) {
    MountPoint *m = (void *) ((struct fixture_control *) c + 1);
    fixture_drop(&m->database);
    ++mount_destroyed;
}
FIXTURE_VTABLE(mount_control, mount_dispose);
FIXTURE_VTABLE(borrow_control, fixture_noop);
static struct fixture_owner db_new(uint16_t media, void *connection) {
    struct fixture_owner o = fixture_make(sizeof(SqliteDataBase), db_control);
    SqliteDataBase *d = o.object;
    d->vtable = db_table.slots;
    d->media = media;
    d->connection = connection;
    return o;
}
static struct fixture_owner mount_new(uint16_t media, void *connection, bool supported) {
    struct fixture_owner o = fixture_make(sizeof(MountPoint), mount_control);
    MountPoint *m = o.object;
    m->vtable = supported ? mount_table.slots : bad_mount_table.slots;
    m->media = media;
    if (supported) m->database = db_new(media, connection);
    return o;
}
static struct fixture_owner borrowed_owner(void *object) {
    struct fixture_owner o = fixture_make(0, borrow_control);
    o.object = object;
    return o;
}
#if defined(AZS_SAMPLE_BOUNDARY)
static void (*source_sample_boundary)(void);
#endif
#ifndef SOURCES_INC
#define SOURCES_INC "../../src/azmod/sdk_native_sources.c"
#endif
#include SOURCES_INC


typedef struct Handler Handler;
struct Handler {
    unsigned char padding[0x30];
    InformationUpdater *updater;
};
typedef struct Dispatcher Dispatcher;
struct Dispatcher {
    unsigned char padding[0x20];
    Handler *handler;
};
unsigned test_build;
_Atomic(unsigned) sql_calls = 0, stat_calls = 0, open_calls = 0, read_calls = 0, close_calls = 0;
_Atomic(unsigned) size_calls = 0, native_close_calls = 0, native_read_calls = 0;
int forbidden_native_fd = -1, metadata_mode = 0, size_mode = 0;
typedef struct FileStorage FileStorage;
struct FileStorage {
    const NativeIO *methods;
    NativeVFS *vfs;
    uint64_t padding;
    int32_t fd;
    unsigned char tail[120 - 28];
};
_Static_assert(offsetof(FileStorage, fd) == 0x18 && sizeof(FileStorage) == 120, "Unix file fixture");
typedef struct Connection Connection;
struct Connection {
    FileStorage *file;
    NativeVFS *vfs;
    int failed_opcode;
};
NativeIO fixture_io[3];
NativeVFS fixture_vfs;
int fake_close(NativeFile *unused_0) {
    (void) unused_0;
    ++native_close_calls;
    return 0;
}
int fake_read(NativeFile *unused_0, void *unused_1, int unused_2, int64_t unused_3) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    (void) unused_3;
    ++native_read_calls;
    return 0;
}
int fake_control(NativeFile *unused_0, int unused_1, void *unused_2) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    return 0;
}
int fake_open(NativeVFS *unused_0, const char *unused_1, NativeFile *unused_2, int unused_3, int *unused_4) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    (void) unused_3;
    (void) unused_4;
    return 0;
}
int fake_size(NativeFile *file, int64_t *out) {
    ++size_calls;
    __auto_type f = &(*((FileStorage *) (file)));
    struct stat value;
    if (fstat(f->fd, &value)) return 1;
    *out = value.st_size;
    if (size_mode == 1) ++*out;
    if (size_mode == 2) ++f->fd;
    if (size_mode == 3) f->methods = &fixture_io[1];
    if (size_mode == 4) f->vfs = NULL;
    return size_mode == 5 ? 1 : 0;
}
int fake_sql_control(struct sqlite3 *db, const char *schema, int opcode, void *out) {
    ++sql_calls;
    assert(!strcmp(schema, "main"));
    __auto_type c = &(*((Connection *) (db)));
    if (c->failed_opcode == opcode) return 1;
    if (opcode == 7) *((NativeFile **) (out)) = ((NativeFile *) (c->file));
    else if (opcode == 27) *((NativeVFS **) (out)) = c->vfs;
    else assert(false);
    return 0;
}
int counted_stat(int fd, struct stat *out) {
    unsigned call = ++stat_calls;
    assert(fd == forbidden_native_fd);
    if (metadata_mode == 1 || (metadata_mode == 2 && call == 2)) return -1;
    int result = fstat(fd, out);
    if (!result && metadata_mode == 3 && call == 2) ++out->st_ino;
    if (!result && metadata_mode == 4) out->st_mode = S_IFIFO;
    return result;
}
int counted_open(const char *path, int flags) {
    ++open_calls;
    assert(!strncmp(path, "/proc/self/fdinfo/", 18));
    assert((flags & (O_RDONLY | O_CLOEXEC | O_NONBLOCK)) == (O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    return metadata_mode == 5 ? -1 : open(path, flags);
}
ssize_t counted_read(int fd, void *out, size_t size) {
    ++read_calls;
    assert(fd != forbidden_native_fd && size <= 4096);
    if (metadata_mode == 6) return -1;
    if (metadata_mode == 7) {
        memset(out, 'x', size);
        return ((ssize_t) (size));
    }
    if (metadata_mode == 8) {
        *((char *) (out)) = 'x';
        return 1;
    }
    if (metadata_mode == 9) size = size < 32 ? size : 32;
    return read(fd, out, size);
}
int counted_close(int fd) {
    ++close_calls;
    assert(fd != forbidden_native_fd); /* Must never close/dup-close SQLite's own FD. */
    int result = close(fd);
    return metadata_mode == 10 ? -1 : result;
}
static const MetadataOps counted_ops = {counted_stat, counted_open, counted_read, counted_close};
void clear_counts() {
    sql_calls = stat_calls = open_calls = read_calls = close_calls = 0;
    size_calls = native_close_calls = native_read_calls = 0;
    metadata_mode = size_mode = 0;
}
void no_physical_calls() {
    assert(!sql_calls && !stat_calls && !open_calls && !read_calls && !close_calls && !size_calls);
    assert(!native_close_calls && !native_read_calls);
}
void no_filesystem_calls() {
    assert(!stat_calls && !open_calls && !read_calls && !close_calls && !size_calls);
    assert(!native_close_calls && !native_read_calls);
}
void configure(unsigned policy) {
    assert(azsdk_sources_configure(0, ((int) (test_build)), 0) == AZS_UNSUPPORTED);
    azsdk_sources_sweep();
    assert(azsdk_sources_configure(1, ((int) (test_build)), policy) == AZS_OK);
}
#define put(object, offset, value)                                            \
    do {                                                                      \
        __auto_type put_value_ = (value);                                     \
        memcpy((char *) (object) + (offset), &put_value_, sizeof put_value_); \
    } while (0)
typedef struct Fixture {
    MusicLibraryServer server;
    InformationUpdater updater;
    Handler handler;
    Dispatcher dispatcher;
    Connection connection;
} Fixture;
static void fixture_marker(Fixture *f, unsigned char v) { put(&f->server, 0xd2, v); }
static MountPoint *fixture_mount(Fixture *f) { return f->updater.mount.object; }
static void fixture_replace(Fixture *f, uint16_t media) {
    struct fixture_owner next = mount_new(media, &f->connection, true);
    fixture_drop(&f->updater.mount);
    f->updater.mount = next;
}
static void fixture_init(Fixture *f, uint16_t media) {
    *f = (Fixture) {0};
    f->server.vtable = server_table.slots;
    f->server.worker = pthread_self();
    f->updater.vtable = updater_table.slots;
    f->handler.updater = &f->updater;
    f->dispatcher.handler = &f->handler;
    put(&f->server, builds[test_build].dispatcher, &f->dispatcher);
    put(&f->server, 0x28, &f->server.worker);
    fixture_marker(f, 1);
    fixture_replace(f, media);
    assert(offsetof(InformationUpdater, mount) == 8 && offsetof(MountPoint, database) == 0x30 && offsetof(SqliteDataBase, connection) == 0x38);
}
static void source_fixture_drop(Fixture *f) { fixture_drop(&f->updater.mount); }
static Build fixture_profile(const Fixture *f) {
    uintptr_t table = (uintptr_t) f->server.vtable;
    return (Build) {builds[test_build].dispatcher, load(uintptr_t, table + 5 * 8) - source_state().main_image.base};
}
SqlProfile empty_sql = {0};
struct Capture {
    Fixture *fixture;
    struct azsdk_source_sample *out;
    const SqlProfile *profile;
    int result;
};
static void capture_call(void *opaque) {
    struct Capture *c = opaque;
    Build b = fixture_profile(c->fixture);
    c->result = observe((uintptr_t) &c->fixture->server, &b, c->profile, &counted_ops, c->out, c->out ? sizeof *c->out : 0);
}
int capture(Fixture *f, struct azsdk_source_sample *out, const SqlProfile *p) {
    struct Capture c = {f, out, p, AZS_GUARD};
    return fixture_native_try(capture_call, &c) ? c.result : AZS_GUARD;
}
struct ForeignCapture {
    Fixture *fixture;
    const SqlProfile *profile;
    int result;
};
static void *foreign_capture(void *opaque) {
    struct ForeignCapture *c = opaque;
    c->result = capture(c->fixture, NULL, c->profile);
    return NULL;
}
struct azsdk_source_sample captured(Fixture *f, const SqlProfile *p) {
    struct azsdk_source_sample out = {0};
    assert(capture(f, &out, &((*p))) == AZS_OK);
    assert(out.struct_size == sizeof out && out.owner_epoch && out.revision && out.observed_ns);
    assert((out.flags & (AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE)) ==
           (AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE));
    assert(!out.reserved && out.native_media_id == fixture_mount(f)->media);
    return out;
}
#define UNCHANGED(output_name, expression, expected_code)      \
    do {                                                       \
        struct azsdk_source_sample sample_, snapshot_;         \
        memset(&sample_, 0xa5, sizeof sample_);                \
        snapshot_ = sample_;                                   \
        struct azsdk_source_sample *output_name = &sample_;    \
        assert((expression) == (expected_code));               \
        assert(!memcmp(&sample_, &snapshot_, sizeof sample_)); \
    } while (0)
void api_and_guards() {
    assert(azsdk_sources_configure(0, 0, 0) == AZS_UNSUPPORTED);
    UNCHANGED(o, azsdk_sources_observe_owned_message(NULL, o, sizeof *o), AZS_UNSUPPORTED);
    assert(azsdk_sources_configure(1, 0, 0) == AZS_UNSUPPORTED);
    assert(azsdk_sources_configure(1, ((int) (test_build)), 2) == AZS_UNSUPPORTED);
    configure(AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER);
    Fixture f __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&f, 0x101);
    clear_counts();
    /* The real entry point refuses this synthetic method RVA even with correct RTTI. */
    UNCHANGED(o, azsdk_sources_observe_owned_message(&f.server, o, sizeof *o), AZS_GUARD);
    assert(!f.server.affinity_calls);
    no_physical_calls();
    assert(azsdk_sources_observe_owned_message(&f.server, NULL, 1) == AZS_INVALID);
    unsigned char prefix[7] = {0};
    assert(azsdk_sources_observe_owned_message(&f.server, prefix, sizeof prefix) == AZS_INVALID);
    fixture_marker(&f, 0);
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    assert(!f.server.affinity_calls);
    fixture_marker(&f, 2);
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    assert(!f.server.affinity_calls);
    fixture_marker(&f, 1);
    uintptr_t no_worker = 0;
    put(&f.server, 0x28, no_worker);
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    assert(!f.server.affinity_calls);
    __auto_type worker = &f.server.worker;
    put(&f.server, 0x28, worker);
    int result = 0;
    struct ForeignCapture foreign_context = {&f, &empty_sql, 0};
    pthread_t foreign;
    assert(!pthread_create(&foreign, NULL, foreign_capture, &foreign_context));
    assert(!pthread_join(foreign, NULL));
    result = foreign_context.result;
    assert(result == AZS_GUARD && f.server.affinity_calls == 1);
    no_physical_calls();
    f.server.throws = true;
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    f.server.throws = false;
    no_physical_calls();
    __auto_type server = ((uintptr_t) (&f.server));
    __auto_type table = load(uintptr_t, server);
    uintptr_t heap_table[11] = {0};
    __auto_type outside_image = ((uintptr_t) (heap_table));
    put(&f.server, 0, outside_image);
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    put(&f.server, 0, table);
    __auto_type dispatcher = &f.dispatcher;
    uintptr_t null_pointer = 0;
    put(&f.server, builds[test_build].dispatcher, null_pointer);
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    put(&f.server, builds[test_build].dispatcher, dispatcher);
    f.dispatcher.handler = NULL;
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    f.dispatcher.handler = &f.handler;
    f.handler.updater = NULL;
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    f.handler.updater = &f.updater;
    __auto_type good_updater = f.handler.updater;
    f.handler.updater = ((InformationUpdater *) (&f.server));
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    f.handler.updater = good_updater;
    fixture_drop(&f.updater.mount);
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    fixture_drop(&f.updater.mount);
    f.updater.mount = mount_new(0x101, NULL, false);
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    fixture_replace(&f, 0x101);
    struct fixture_owner good_mount __attribute__((cleanup(fixture_drop))) = fixture_copy(f.updater.mount);
    fixture_drop(&f.updater.mount);
    f.updater.mount = (struct fixture_owner) {good_mount.object, NULL};
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    source_owner_set(&f.updater.mount, good_mount);
    struct fixture_owner good_database __attribute__((cleanup(fixture_drop))) = fixture_copy(fixture_mount(&f)->database);
    fixture_drop(&fixture_mount(&f)->database);
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    source_owner_set(&fixture_mount(&f)->database, good_database);
    fixture_mount(&f)->database.object = &f.server;
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    source_owner_set(&fixture_mount(&f)->database, good_database);
    ((SqliteDataBase *) fixture_mount(&f)->database.object)->media = 0;
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    ((SqliteDataBase *) fixture_mount(&f)->database.object)->media = 0x102;
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    ((SqliteDataBase *) fixture_mount(&f)->database.object)->media = 0x101;
    ((SqliteDataBase *) fixture_mount(&f)->database.object)->connection = NULL;
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    ((SqliteDataBase *) fixture_mount(&f)->database.object)->connection = &f.connection;
    struct fixture_owner db __attribute__((cleanup(fixture_drop))) = fixture_copy(fixture_mount(&f)->database);
    fixture_drop(&fixture_mount(&f)->database);
    fixture_mount(&f)->database = (struct fixture_owner) {db.object, NULL};
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_GUARD);
    source_owner_set(&fixture_mount(&f)->database, db);
    no_physical_calls();
    __auto_type out = captured(&(f), &(empty_sql)); /* Profile unavailable: no native/filesystem calls. */
    assert(out.physical_status == AZS_PHYSICAL_PROFILE && !(out.flags & AZS_PHYSICAL));
    no_physical_calls();
    assert(azsdk_sources_configure(1, test_build == 1 ? 2 : 1, 0) == AZS_UNSUPPORTED);
    struct azsdk_source_sample old;
    assert(azsdk_sources_read(out.owner_epoch, &old, sizeof old) == AZS_STALE);
    assert(!(old.flags & (AZS_CURRENT | AZS_PHYSICAL)));
}
void weak_lifetime_and_outputs() {
    configure(0);
    Fixture f __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&f, 0x110);
    int m_count = f.updater.mount.control->strong, d_count = fixture_mount(&f)->database.control->strong;
    __auto_type first = captured(&(f), &(empty_sql));
    assert(f.updater.mount.control->strong == m_count && fixture_mount(&f)->database.control->strong == d_count);
    __auto_type second = captured(&(f), &(empty_sql));
    assert(second.owner_epoch == first.owner_epoch && second.revision > first.revision);
    struct azsdk_source_sample out;
    assert(azsdk_sources_candidate(0x110, &out, sizeof out) == AZS_OK && out.owner_epoch == first.owner_epoch);
    /* Legacy observation cannot stand in for an authenticated status connection. */
    UNCHANGED(o, azsdk_sources_status_read(first.owner_epoch, o, sizeof *o), AZS_STALE);
    UNCHANGED(o, azsdk_sources_status_read(0, o, sizeof *o), AZS_INVALID);
    UNCHANGED(o, azsdk_sources_status_read(UINT64_MAX, o, sizeof *o), AZS_UNKNOWN);
    assert(azsdk_sources_status_read(first.owner_epoch, NULL, sizeof out) == AZS_INVALID);
    UNCHANGED(o, azsdk_sources_read(0, o, sizeof *o), AZS_INVALID);
    UNCHANGED(o, azsdk_sources_read(UINT64_MAX, o, sizeof *o), AZS_UNKNOWN);
    UNCHANGED(o, azsdk_sources_candidate(0, o, sizeof *o), AZS_INVALID);
    assert(azsdk_sources_read(first.owner_epoch, NULL, sizeof out) == AZS_INVALID);
    unsigned char prefix[17], expected[17];
    memset(prefix, 0xa5, sizeof prefix);
    memcpy(expected, prefix, sizeof prefix);
    assert(azsdk_sources_read(first.owner_epoch, prefix + 1, 8) == AZS_OK);
    assert(prefix[0] == expected[0] && !memcmp(prefix + 9, expected + 9, 8));
    uint32_t size;
    memcpy(&size, prefix + 1, 4);
    assert(size == sizeof out); /* Unaligned, exactly eight-byte earlier prefix is valid. */
    struct Extended {
        struct azsdk_source_sample sample;
        unsigned char tail[16];
    } extended;
    memset(&extended, 0xa5, sizeof extended);
    assert(azsdk_sources_read(first.owner_epoch, &extended, sizeof extended) == AZS_OK);
    {
        for (unsigned range_index_1 = 0; range_index_1 < sizeof(extended.tail) / sizeof(extended.tail)[0]; ++range_index_1) {
            __auto_type byte = extended.tail[range_index_1];
            assert(byte == 0xa5);
        }
    }
    unsigned destroyed_m = mount_destroyed, destroyed_d = database_destroyed;
    fixture_drop(&f.updater.mount);
    assert(mount_destroyed == destroyed_m + 1 && database_destroyed == destroyed_d + 1);
    assert(azsdk_sources_read(first.owner_epoch, &out, sizeof out) == AZS_STALE);
    assert(!(out.flags & AZS_CURRENT));
    assert(azsdk_sources_candidate(0x110, &out, sizeof out) == AZS_UNKNOWN);
    assert(azsdk_sources_sweep() == 1);
    UNCHANGED(o, azsdk_sources_read(first.owner_epoch, o, sizeof *o), AZS_UNKNOWN);
    fixture_replace(&f, 0x110);
    __auto_type next = captured(&(f), &(empty_sql));
    assert(next.owner_epoch > first.owner_epoch && (next.flags & AZS_MEDIA_AMBIGUOUS));
    assert(azsdk_sources_candidate(0x110, &out, sizeof out) == AZS_AMBIGUOUS && !out.owner_epoch);
    fixture_drop(&f.updater.mount);
    azsdk_sources_sweep();
    assert(azsdk_sources_candidate(0x110, &out, sizeof out) == AZS_AMBIGUOUS && !out.owner_epoch);
}
#if defined(AZS_SAMPLE_BOUNDARY)
static Fixture *expiring_fixture;
static unsigned sample_boundary_calls;
void expire_source_before_sample() {
    ++sample_boundary_calls;
    fixture_drop(&expiring_fixture->updater.mount);
}
void candidate_expiry_during_sample() {
    configure(0);
    Fixture f __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&f, 0x111);
    __auto_type first = captured(&(f), &(empty_sql));
    expiring_fixture = &f;
    sample_boundary_calls = 0;
    source_sample_boundary = expire_source_before_sample;
    /* Expire after candidate selection but before the copied snapshot's lifetime
     * check. An OK result must describe that exact copied snapshot. */
    UNCHANGED(out, azsdk_sources_candidate(0x111, out, sizeof *out), AZS_UNKNOWN);
    source_sample_boundary = NULL;
    expiring_fixture = NULL;
    assert(sample_boundary_calls == 1);
    struct azsdk_source_sample stale;
    assert(azsdk_sources_read(first.owner_epoch, &stale, sizeof stale) == AZS_STALE);
    assert(!(stale.flags & (AZS_CURRENT | AZS_PHYSICAL)));
    assert(!stale.device && !stale.inode && !stale.mount_id && !stale.file_size && !stale.physical_observed_ns);
}
#endif
void owner_address_reuse(void) {
    configure(0);
    Fixture f __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&f, 0x120);
    struct fixture_owner raw __attribute__((cleanup(fixture_drop))) = mount_new(0x120, &f.connection, true);
    struct fixture_owner first_owner __attribute__((cleanup(fixture_drop))) = borrowed_owner(raw.object);
    source_owner_set(&f.updater.mount, first_owner);
    __auto_type a = captured(&(f), &(empty_sql));
    struct fixture_owner second_owner __attribute__((cleanup(fixture_drop))) = borrowed_owner(raw.object);
    source_owner_set(&f.updater.mount, second_owner);
    __auto_type b = captured(&(f), &(empty_sql));
    assert(a.owner_epoch != b.owner_epoch && (b.flags & AZS_MEDIA_AMBIGUOUS));
    assert(first_owner.control->strong == 1 && second_owner.control->strong == 2);
    struct azsdk_source_sample out;
    assert(azsdk_sources_read(a.owner_epoch, &out, sizeof out) == AZS_STALE);
    struct fixture_owner db __attribute__((cleanup(fixture_drop))) = db_new(0x120, &f.connection);
    struct fixture_owner db_first __attribute__((cleanup(fixture_drop))) = borrowed_owner(db.object);
    source_owner_set(&fixture_mount(&f)->database, db_first);
    __auto_type c = captured(&(f), &(empty_sql));
    fixture_drop(&fixture_mount(&f)->database);
    fixture_mount(&f)->database = borrowed_owner(db.object);
    __auto_type d = captured(&(f), &(empty_sql));
    assert(c.owner_epoch != d.owner_epoch && db_first.control->strong == 1);
    Connection other = {0};
    ((SqliteDataBase *) fixture_mount(&f)->database.object)->connection = &other;
    __auto_type e = captured(&(f), &(empty_sql));
    assert(e.owner_epoch != d.owner_epoch);
    fixture_drop(&f.updater.mount);
    azsdk_sources_sweep();
    assert(azsdk_sources_candidate(0x120, &out, sizeof out) == AZS_AMBIGUOUS);
 /* One control with two different live aliases. The enclosing owner retains
  * both actual Mount/DB objects until both source observations are retired. */
    struct fixture_owner pair_a __attribute__((cleanup(fixture_drop))) = mount_new(0x121, &f.connection, true), pair_b __attribute__((cleanup(fixture_drop))) = mount_new(0x121, &f.connection, true);
    struct fixture_owner pair __attribute__((cleanup(fixture_drop))) = borrowed_owner(pair_a.object);
    source_owner_set(&f.updater.mount, pair);
    a = captured(&(f), &(empty_sql));
    f.updater.mount.object = pair_b.object;
    b = captured(&(f), &(empty_sql));
    assert(a.owner_epoch != b.owner_epoch);
 /* Release fixture aliases before the complete pointees leave this scope. */
    fixture_drop(&f.updater.mount);
}
void binding_and_quota() {
    configure(0);
    Fixture a __attribute__((cleanup(source_fixture_drop))), b __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&a, 0x130);
    fixture_init(&b, 0x130);
    source_owner_set(&b.updater.mount, a.updater.mount);
    struct azsdk_source_sample first = captured(&(a), &(empty_sql)), second = captured(&(b), &(empty_sql));
    assert(first.owner_epoch == second.owner_epoch);
    fixture_drop(&a.updater.mount);
    assert(capture(&(a), NULL, &(empty_sql)) == AZS_GUARD);
    struct azsdk_source_sample out;
    assert(azsdk_sources_read(first.owner_epoch, &out, sizeof out) == AZS_OK);
    fixture_drop(&b.updater.mount);
    assert(capture(&(b), NULL, &(empty_sql)) == AZS_GUARD);
    assert(azsdk_sources_read(first.owner_epoch, &out, sizeof out) == AZS_STALE);
    configure(0);
    Fixture f __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&f, 0x140);
    for (unsigned i = 0; i < kSlots; ++i) {
        fixture_replace(&f, ((uint16_t) (0x140 + i)));
        captured(&(f), &(empty_sql));
    }
    fixture_replace(&f, 0x180);
    atomic_store_explicit(&(source_state().capture_policy), AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER, memory_order_seq_cst);
    clear_counts();
    UNCHANGED(o, capture(&(f), o, &(empty_sql)), AZS_LIMIT);
    no_physical_calls();
    assert(azsdk_sources_sweep() == kSlots);
    __auto_type after_limit = captured(&(f), &(empty_sql));
    assert(after_limit.flags & AZS_MEDIA_AMBIGUOUS);
    configure(0);
    Fixture *servers[33] = {0};
    unsigned server_count = 0;
    servers[server_count] = calloc(1, sizeof(Fixture));
    assert(servers[server_count]);
    fixture_init(servers[server_count++], 0x190);
    first = captured(&(*servers[0]), &(empty_sql));
    for (unsigned i = 1; i < kSlots + 1; ++i) {
        servers[server_count] = calloc(1, sizeof(Fixture));
        assert(servers[server_count]);
        fixture_init(servers[server_count++], 0x190);
        source_owner_set(&servers[server_count - 1]->updater.mount, servers[0]->updater.mount);
        if (i < kSlots) assert(captured(&(*servers[server_count - 1]), &(empty_sql)).owner_epoch == first.owner_epoch);
    }
    UNCHANGED(o, capture(&(*servers[server_count - 1]), o, &(empty_sql)), AZS_LIMIT);
    assert(azsdk_sources_candidate(0x190, &out, sizeof out) == AZS_AMBIGUOUS && !out.owner_epoch);
    for (unsigned i = 0; i < server_count; i++) {
        source_fixture_drop(servers[i]);
        free(servers[i]);
    }
    configure(0);
    Fixture last __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&last, 0x191);
    __auto_type saved_epoch = source_state().next_epoch;
    source_state().next_epoch = UINT64_MAX;
    UNCHANGED(o, capture(&(last), o, &(empty_sql)), AZS_LIMIT);
    source_state().next_epoch = saved_epoch; /* Isolated exhaustion fixture; production never resets it. */
    __auto_type saved_revision = source_state().next_revision;
    source_state().next_revision = UINT64_MAX;
    UNCHANGED(o, capture(&(last), o, &(empty_sql)), AZS_LIMIT);
    source_state().next_revision = saved_revision;
}
SqlProfile physical_profile() {
    for (unsigned i = 0; i < 3; ++i) {
        fixture_io[i] = (__typeof__(fixture_io[i])) {0};
        fixture_io[i].version = i == 2 ? 1 : 3;
        fixture_io[i].close = fake_close;
        fixture_io[i].read = fake_read;
        fixture_io[i].size = fake_size;
        fixture_io[i].control = fake_control;
    }
    fixture_vfs = (__typeof__(fixture_vfs)) {0};
    fixture_vfs.version = 3;
    fixture_vfs.os_size = 120;
    fixture_vfs.max_path = 512;
    fixture_vfs.name = "unix";
    fixture_vfs.open = fake_open;
    SqlProfile p = {0};
    p.image = source_state().main_image;
    p.file_control = fake_sql_control;
    for (unsigned i = 0; i < 3; ++i) p.io_rvas[i] = ((uintptr_t) (&fixture_io[i])) - p.image.base;
    p.open_rva = ((uintptr_t) (fake_open)) - p.image.base;
    p.read_rva = ((uintptr_t) (fake_read)) - p.image.base;
    p.size_rva = ((uintptr_t) (fake_size)) - p.image.base;
    p.control_rva = ((uintptr_t) (fake_control)) - p.image.base;
    p.ready = true;
    return p;
}
void borrowed_descriptor_identity() {
    char name[] = "/tmp/azstatus-input-fd.XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0 && !unlink(name));
    assert(!fcntl(fd, F_SETFD, FD_CLOEXEC));
    struct stat sb;
    uint64_t mount = 0;
    assert(!fstat(fd, &sb) && descriptor_mount_id(fd, &(metadata_ops), &(mount)));
    assert(azsdk_sources_descriptor_identity(fd, mount, sb.st_dev, sb.st_ino));
    assert(!azsdk_sources_descriptor_identity(fd, mount + 1, sb.st_dev, sb.st_ino));
    assert(!azsdk_sources_descriptor_identity(fd, mount, sb.st_dev, sb.st_ino + 1));
    assert(!azsdk_sources_descriptor_identity(-1, mount, sb.st_dev, sb.st_ino));
    assert(fcntl(fd, F_GETFD) >= 0 && !close(fd));
}
void mount_id_parser() {
    uint64_t out = 99;
#define parse(text_) parse_mount_id((text_), strlen(text_), &out)
    assert(parse("pos:\t0\nflags:\t0100000\nmnt_id:\t1234\nino:\t7\n") && out == 1234);
    assert(parse("mnt_id: 18446744073709551615 \n") && out == UINT64_MAX);
    assert(!parse("mnt_id: 18446744073709551616\n"));
    assert(!parse("mnt_id: 0\n") && !parse("mnt_id: -1\n") && !parse("mnt_id: +1\n"));
    assert(!parse("mnt_id: 7x\n") && !parse("mnt_id:\n") && !parse("mnt_id: 7"));
    assert(!parse("mnt_id: 7\nmnt_id: 8\n") && !parse("other: 7\n") && !parse("xmnt_id: 8\n"));
}
void vfs_name_boundaries() {
    /* The image check is a module-span bound, not a string-object bound. Put each
     * valid C string immediately before an inaccessible page while the synthetic
     * image span includes both pages, so plain builds catch widened comparisons. */
    long page_size = sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    __auto_type bytes = ((size_t) (page_size));
    __auto_type memory = ((char *) (mmap(NULL, 2 * bytes, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)));
    assert(memory != MAP_FAILED);
    assert(!mprotect(memory + bytes, bytes, PROT_NONE));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        SqlProfile p = {0};
        __auto_type begin = ((uintptr_t) (memory));
        p.image.ranges[0] = (__typeof__(p.image.ranges[0])) {begin, begin + 2 * bytes, kRead};
        p.image.count = 1;
        const char *const accepted[] = {"unix", "unix-excl", "unix-none", "unix-dotfile"};
        const char *const rejected[] = {"", "u", "uni", "unix-", "unix-e", "unix-exc", "unix-dotfil",
                                        "other", "unix-exclX", "unix-none-", "unix-dotfile-"};
        {
            for (unsigned range_index_2 = 0; range_index_2 < sizeof(accepted) / sizeof(accepted)[0]; ++range_index_2) {
                __auto_type name = accepted[range_index_2];

                __auto_type size = strlen(name) + 1;
                char *at = memory + bytes - size;
                memcpy(at, name, size);
                assert(supported_name(&(p), at));
            }
        }
        {
            for (unsigned range_index_3 = 0; range_index_3 < sizeof(rejected) / sizeof(rejected)[0]; ++range_index_3) {
                __auto_type name = rejected[range_index_3];

                __auto_type size = strlen(name) + 1;
                char *at = memory + bytes - size;
                memcpy(at, name, size);
                assert(!supported_name(&(p), at));
            }
        }
        /* Bounds/overflow must be rejected before the first invalid byte read. */
        assert(!supported_name(&(p), NULL));
        assert(!supported_name(&(p), ((const char *) (UINTPTR_MAX))));
        p.image.ranges[0].end = begin + bytes;
        char *unterminated = memory + bytes - 4;
        memcpy(unterminated, "unix", 4);
        assert(!supported_name(&(p), unterminated));
        p.image.ranges[0].flags = 0;
        assert(!supported_name(&(p), unterminated));
        _exit(0);
    }
    int vfs_name_child_status = 0;
    assert(waitpid(child, &vfs_name_child_status, 0) == child);
    assert(WIFEXITED(vfs_name_child_status) && WEXITSTATUS(vfs_name_child_status) == 0);
    assert(!munmap(memory, 2 * bytes));
}
void rtti_name_boundaries() {
    long page_size = sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    __auto_type bytes = ((size_t) (page_size));
    __auto_type memory = ((char *) (mmap(NULL, 2 * bytes, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)));
    assert(memory != MAP_FAILED);
    assert(!mprotect(memory + bytes, bytes, PROT_NONE));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        /* Native-shaped metadata tests rejection before any virtual invocation.
         * A short unrelated RTTI C string is a valid input, including GNU '*'. */
        _Alignas(8) uintptr_t type[2] = {0};
        _Alignas(8) uintptr_t table[3] = {0, ((uintptr_t) (type)), 0};
        _Alignas(8) uintptr_t object = ((uintptr_t) (table + 2));
        __auto_type begin = ((uintptr_t) (memory));
        Image image = {0};
        image.ranges[0] = (__typeof__(image.ranges[0])) {begin, begin + 2 * bytes, kRead};
        image.ranges[1] = (__typeof__(image.ranges[1])) {((uintptr_t) (type)), ((uintptr_t) (type + 2)), kRead};
        image.ranges[2] = (__typeof__(image.ranges[2])) {((uintptr_t) (table)), ((uintptr_t) (table + 3)), kRead};
        image.count = 3;
        source_state().main_image = image; /* Private child; parent profile is unchanged. */
        const char *expected = "N13music_library18MusicLibraryServerE";
        const char *const rejected[] = {"", "*", "N1aE", "*N1aE", "N13music_library18MusicLibraryServerEX"};
        {
            for (unsigned range_index_4 = 0; range_index_4 < sizeof(rejected) / sizeof(rejected)[0]; ++range_index_4) {
                __auto_type name = rejected[range_index_4];

                __auto_type size = strlen(name) + 1;
                char *at = memory + bytes - size;
                memcpy(at, name, size);
                type[1] = ((uintptr_t) (at));
                assert(!checked_vtable(((uintptr_t) (&object)), expected, 1));
            }
        }
        {
            __typeof__(expected) range_values_5[] = {expected, "*N13music_library18MusicLibraryServerE"};
            for (unsigned range_index_5 = 0; range_index_5 < sizeof(range_values_5) / sizeof(range_values_5)[0]; ++range_index_5) {
                __auto_type name = range_values_5[range_index_5];

                __auto_type size = strlen(name) + 1;
                char *at = memory + bytes - size;
                memcpy(at, name, size);
                type[1] = ((uintptr_t) (at));
                assert(checked_vtable(((uintptr_t) (&object)), expected, 1) == object);
            }
        }
        type[1] = UINTPTR_MAX;
        assert(!checked_vtable(((uintptr_t) (&object)), expected, 1));
        _exit(0);
    }
    int rtti_name_child_status = 0;
    assert(waitpid(child, &rtti_name_child_status, 0) == child);
    assert(WIFEXITED(rtti_name_child_status) && WEXITSTATUS(rtti_name_child_status) == 0);
    assert(!munmap(memory, 2 * bytes));
}
void physical_capture() {
    configure(AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER);
    Fixture f __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&f, 0x210);
    char path[] = "/tmp/az-source-fixture.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unsigned char bytes[4096] = {0};
    assert(write(fd, bytes, sizeof bytes) == ((ssize_t) (sizeof bytes)));
    forbidden_native_fd = fd;
    struct flock record_lock = {0};
    record_lock.l_type = F_WRLCK;
    record_lock.l_whence = SEEK_SET;
    assert(!fcntl(fd, F_SETLK, &record_lock));
    FileStorage file = {0};
    __auto_type profile = physical_profile();
    file.methods = &fixture_io[0];
    file.vfs = &fixture_vfs;
    file.fd = fd;
    f.connection.file = &file;
    f.connection.vfs = &fixture_vfs;
    clear_counts();
    __auto_type good = captured(&(f), &(profile));
    assert(good.physical_status == AZS_PHYSICAL_OK && (good.flags & AZS_PHYSICAL));
    assert(sql_calls == 2 && stat_calls == 2 && open_calls == 1 && close_calls == 1 && size_calls == 1);
    assert(read_calls > 0 && read_calls <= 8 && !native_read_calls && !native_close_calls);
    struct stat original;
    assert(!fstat(fd, &original));
    assert(good.device == ((uint64_t) (original.st_dev)) && good.inode == original.st_ino &&
           good.file_size == sizeof bytes && good.mount_id && good.physical_observed_ns);
    char moved[128];
    snprintf(moved, sizeof moved, "%s.old", path);
    assert(!rename(path, moved));
    int replacement = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(replacement >= 0);
    struct stat replaced;
    assert(!fstat(replacement, &replaced) && original.st_ino != replaced.st_ino);
    __auto_type still_original = captured(&(f), &(profile));
    assert(still_original.owner_epoch == good.owner_epoch && still_original.inode == good.inode);
    assert(!close(replacement) && !unlink(path) && !unlink(moved));
    clear_counts();
    atomic_store_explicit(&(source_state().capture_policy), 0, memory_order_seq_cst);
    __auto_type disabled = captured(&(f), &(profile));
    assert(disabled.physical_status == AZS_PHYSICAL_DISABLED && !(disabled.flags & AZS_PHYSICAL));
    no_physical_calls();
    atomic_store_explicit(&(source_state().capture_policy), AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER, memory_order_seq_cst);
    profile.ready = false; /* Even with callable API pointers, no native/filesystem calls. */
    __auto_type missing = captured(&(f), &(profile));
    assert(missing.physical_status == AZS_PHYSICAL_PROFILE);
    no_physical_calls();
    profile.ready = true;
    fixture_marker(&f, 0);
    UNCHANGED(o, capture(&(f), o, &(profile)), AZS_GUARD);
    no_physical_calls();
    fixture_marker(&f, 1);
    int result = 0;
    struct ForeignCapture foreign_context = {&f, &profile, 0};
    pthread_t foreign;
    assert(!pthread_create(&foreign, NULL, foreign_capture, &foreign_context));
    assert(!pthread_join(foreign, NULL));
    result = foreign_context.result;
    assert(result == AZS_GUARD);
    no_physical_calls();
    {
        int range_values_6[] = {7, 27};
        for (unsigned range_index_6 = 0; range_index_6 < sizeof(range_values_6) / sizeof(range_values_6)[0]; ++range_index_6) {
            int opcode = range_values_6[range_index_6];

            clear_counts();
            f.connection.failed_opcode = opcode;
            __auto_type rejected = captured(&(f), &(profile));
            assert(rejected.physical_status == AZS_PHYSICAL_API && !(rejected.flags & AZS_PHYSICAL));
            no_filesystem_calls();
        }
    }
    f.connection.failed_opcode = 0;
    for (unsigned which = 0; which < 6; ++which) {
        clear_counts();
        __auto_type p = profile;
        switch (which) {
        case 0: fixture_vfs.name = "other"; break;
        case 1: fixture_vfs.version = 2; break;
        case 2: fixture_vfs.os_size = 119; break;
        case 3: fixture_vfs.max_path = 513; break;
        case 4: ++p.open_rva; break;
        case 5: f.connection.vfs = ((NativeVFS *) (((unsigned char *) (&fixture_vfs)) + 1)); break;
        }
        assert(captured(&(f), &(p)).physical_status == AZS_PHYSICAL_VFS);
        no_filesystem_calls();
        profile = physical_profile();
        f.connection.vfs = &fixture_vfs;
    }
    for (unsigned which = 0; which < 6; ++which) {
        clear_counts();
        __auto_type p = profile;
        switch (which) {
        case 0: fixture_io[0].version = 1; break;
        case 1: ++p.io_rvas[0]; break;
        case 2: ++p.read_rva; break;
        case 3: ++p.size_rva; break;
        case 4: ++p.control_rva; break;
        case 5: file.methods = ((NativeIO *) (((unsigned char *) (&fixture_io[0])) + 1)); break;
        }
        assert(captured(&(f), &(p)).physical_status == AZS_PHYSICAL_METHODS);
        no_filesystem_calls();
        profile = physical_profile();
        file.methods = &fixture_io[0];
    }
    {
        int range_values_7[] = {0, 1};
        for (unsigned range_index_7 = 0; range_index_7 < sizeof(range_values_7) / sizeof(range_values_7)[0]; ++range_index_7) {
            int which = range_values_7[range_index_7];

            clear_counts();
            if (which) file.fd = -1;
            else file.vfs = NULL;
            assert(captured(&(f), &(profile)).physical_status == AZS_PHYSICAL_DESCRIPTOR);
            no_filesystem_calls();
            file.fd = fd;
            file.vfs = &fixture_vfs;
        }
    }
    for (int mode = 1; mode <= 10; ++mode) {
        clear_counts();
        metadata_mode = mode;
        __auto_type rejected = captured(&(f), &(profile));
        assert(rejected.physical_status == (mode == 3 ? AZS_PHYSICAL_CHANGED : mode == 9 ? AZS_PHYSICAL_OK
                                                                                         : AZS_PHYSICAL_METADATA));
        assert(read_calls <= 8 && close_calls == open_calls - (mode == 5 ? 1u : 0u));
        if (mode == 8) assert(read_calls == 8);
        assert(!native_read_calls && !native_close_calls);
    }
    for (int mode = 1; mode <= 5; ++mode) {
        clear_counts();
        size_mode = mode;
        __auto_type rejected = captured(&(f), &(profile));
        assert(rejected.physical_status == (mode == 5 ? AZS_PHYSICAL_METADATA : AZS_PHYSICAL_CHANGED));
        assert(!(rejected.flags & AZS_PHYSICAL) && !rejected.inode && !rejected.mount_id);
        file.fd = fd;
        file.methods = &fixture_io[0];
        file.vfs = &fixture_vfs;
    }
    clear_counts();
    for (unsigned i = 0; i < 3; ++i) {
        file.methods = &fixture_io[i];
        assert(captured(&(f), &(profile)).physical_status == AZS_PHYSICAL_OK);
    }
    __auto_type last = captured(&(f), &(profile));
    fixture_drop(&f.updater.mount);
    struct azsdk_source_sample stale;
    assert(azsdk_sources_read(last.owner_epoch, &stale, sizeof stale) == AZS_STALE);
    assert(!(stale.flags & AZS_PHYSICAL) && !stale.device && !stale.inode && !stale.mount_id && !stale.file_size);
    assert(!native_read_calls && !native_close_calls);
    /* POSIX record locks disappear if ANY extra descriptor for this inode is closed
     * by the same process. Merely checking that the original FD still works misses
     * the dangerous dup-and-close pattern. A child must still conflict with our lock. */
    char fd_path[64];
    snprintf(fd_path, sizeof fd_path, "/proc/self/fd/%d", fd);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        int probe = open(fd_path, O_RDWR | O_CLOEXEC);
        if (probe < 0) _exit(2);
        int result = fcntl(probe, F_SETLK, &record_lock);
        int error = errno;
        close(probe);
        _exit(result < 0 && (error == EACCES || error == EAGAIN) ? 0 : 1);
    }
    int child_status;
    assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && !WEXITSTATUS(child_status));
    /* The test owns this scratch FD; production never owns or closes SQLite's FD. */
    forbidden_native_fd = -1;
    assert(!close(fd));
}
struct Concurrent {
    _Atomic bool holding, release, start, done;
    _Atomic uint64_t epoch;
};
static void *hold_registry(void *opaque) {
    struct Concurrent *c = opaque;
    AZ_MUTEX_LOCK(lock, source_state().registry_mutex);
    atomic_store_explicit(&c->holding, true, memory_order_release);
    while (!atomic_load_explicit(&c->release, memory_order_acquire)) sched_yield();
    return NULL;
}
static void *produce_samples(void *opaque) {
    struct Concurrent *c = opaque;
    Fixture f __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&f, 0x320);
    while (!atomic_load_explicit(&c->start, memory_order_acquire)) sched_yield();
    for (unsigned i = 0; i < 20000; i++) {
        struct azsdk_source_sample out;
        int result = capture(&(f), &out, &(empty_sql));
        assert(result == AZS_OK || result == AZS_BUSY);
        if (result == AZS_OK) atomic_store_explicit(&c->epoch, out.owner_epoch, memory_order_release);
    }
    atomic_store_explicit(&c->done, true, memory_order_release);
    return NULL;
}
static void *read_samples(void *opaque) {
    struct Concurrent *c = opaque;
    while (!atomic_load_explicit(&c->start, memory_order_acquire)) sched_yield();
    uint64_t revision = 0;
    do {
        uint64_t id = atomic_load_explicit(&c->epoch, memory_order_acquire);
        if (!id) continue;
        struct azsdk_source_sample out;
        int result = azsdk_sources_read(id, &out, sizeof out);
        assert(result == AZS_OK || result == AZS_STALE || result == AZS_BUSY);
        if (result == AZS_BUSY) continue;
        assert(out.owner_epoch == id && out.native_media_id == 0x320 && out.revision >= revision);
        assert(out.struct_size == sizeof out && out.observed_ns && (out.flags & AZS_HISTORY_INCOMPLETE));
        revision = out.revision;
    } while (!atomic_load_explicit(&c->done, memory_order_acquire));
    return NULL;
}
void concurrency_and_busy(void) {
    configure(0);
    Fixture fixture __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&fixture, 0x310);
    __auto_type initial = captured(&(fixture), &(empty_sql));
    struct Concurrent c = {0};
    pthread_t holder, producer, readers[4];
    assert(!pthread_create(&holder, NULL, hold_registry, &c));
    while (!atomic_load_explicit(&c.holding, memory_order_acquire)) sched_yield();
    clear_counts();
    UNCHANGED(o, azsdk_sources_read(initial.owner_epoch, o, sizeof *o), AZS_BUSY);
    UNCHANGED(o, azsdk_sources_status_read(initial.owner_epoch, o, sizeof *o), AZS_BUSY);
    UNCHANGED(o, azsdk_sources_candidate(0x310, o, sizeof *o), AZS_BUSY);
    UNCHANGED(o, capture(&(fixture), o, &(empty_sql)), AZS_BUSY);
    no_physical_calls();
    atomic_store_explicit(&c.release, true, memory_order_release);
    assert(!pthread_join(holder, NULL));
    assert(!pthread_create(&producer, NULL, produce_samples, &c));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&readers[i], NULL, read_samples, &c));
    atomic_store_explicit(&c.start, true, memory_order_release);
    assert(!pthread_join(producer, NULL));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(readers[i], NULL));
    assert(atomic_load(&c.epoch));
    struct azsdk_source_sample final;
    assert(azsdk_sources_read(atomic_load(&c.epoch), &final, sizeof final) == AZS_STALE);
    no_physical_calls();
    assert(azsdk_sources_configure(0, test_build, 0) == AZS_UNSUPPORTED);
    azsdk_sources_sweep();
}
static struct azsdk_source_sample native_sample(Fixture *fixture) {
    struct azsdk_source_sample sample = {0};
    Build native_profile = fixture_profile(fixture);
    assert(observe(((uintptr_t) (&fixture->server)), &native_profile, &(source_state().sql_profile), &(metadata_ops), &sample, sizeof sample) == AZS_OK);
    assert(sample.physical_status == AZS_PHYSICAL_OK && (sample.flags & AZS_PHYSICAL));
    return sample;
}
void native_sqlcipher(const char *path) {
    /* Explicit optional research run: public SQLite calls on fresh, unkeyed scratch DBs
     * only. The real SQLCipher profile and Unix file adapter are used unchanged. The C++
     * worker/ownership inputs remain the synthetic model, not an EP147 hook acceptance. */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "native library load failed: %s\n", dlerror());
        abort();
    }
    assert(azsdk_sources_configure(1, test_build, AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER) == AZS_OK);
    assert(azsdk_sources_physical_ready());
    typedef int (*Open)(const char *, void **, int, const char *);
    typedef int (*Close)(void *);
    typedef int (*Exec)(void *, const char *, int (*Exec)(void *, int, char **, char **), void *, char **);
    __auto_type open_db = ((Open) (dlsym(handle, "sqlite3_open_v2")));
    __auto_type close_db = ((Close) (dlsym(handle, "sqlite3_close")));
    __auto_type exec = ((Exec) (dlsym(handle, "sqlite3_exec")));
    {
        __typeof__(((uintptr_t) (open_db))) range_values_8[] = {((uintptr_t) (open_db)), ((uintptr_t) (close_db)),
                                                                ((uintptr_t) (exec))};
        for (unsigned range_index_8 = 0; range_index_8 < sizeof(range_values_8) / sizeof(range_values_8)[0]; ++range_index_8) {
            __auto_type entry = range_values_8[range_index_8];
            assert(entry && image_contains(&source_state().sql_profile.image, entry, 4, kRead | kExecute));
        }
    }
    char original_path[] = "/tmp/az-source-native.XXXXXX";
    int placeholder = mkstemp(original_path);
    assert(placeholder >= 0 && !close(placeholder));
    void *connection = NULL, *replacement = NULL;
    assert(!open_db(original_path, &connection, 6, NULL) && connection);
    assert(!exec(connection, "CREATE TABLE source_fixture(id INTEGER); INSERT INTO source_fixture VALUES(1);",
                 NULL, NULL, NULL));
    Fixture fixture __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&fixture, 0x410);
    ((SqliteDataBase *) fixture_mount(&fixture)->database.object)->connection = connection;

    __auto_type before = native_sample(&(fixture));
    struct stat original_stat;
    assert(!stat(original_path, &original_stat));
    assert(before.device == ((uint64_t) (original_stat.st_dev)) &&
           before.inode == original_stat.st_ino && before.file_size == ((uint64_t) (original_stat.st_size)));
    int reader = azsdk_sources_reader_fd(connection, before.mount_id, before.device, before.inode);
    struct stat reader_stat;
    assert(reader >= 0 && !fstat(reader, &reader_stat) && reader_stat.st_ino == original_stat.st_ino);
    assert(azsdk_sources_reader_fd(NULL, before.mount_id, before.device, before.inode) == -1);
    assert(azsdk_sources_reader_fd(connection, 0, before.device, before.inode) == -1);
    assert(azsdk_sources_reader_fd(connection, before.mount_id, before.device, 0) == -1);
    assert(azsdk_sources_reader_fd(connection, before.mount_id + 1, before.device, before.inode) == -1);
    assert(azsdk_sources_reader_fd(connection, before.mount_id, before.device ^ 1, before.inode) == -1);
    assert(azsdk_sources_reader_fd(connection, before.mount_id, before.device, before.inode + 1) == -1);
    assert(!fstat(reader, &reader_stat)); /* Rejection never closes the borrowed native descriptor. */
    char moved[128];
    snprintf(moved, sizeof moved, "%s.old", original_path);
    assert(!rename(original_path, moved));
    assert(!open_db(original_path, &replacement, 6, NULL) && replacement);
    assert(!exec(replacement, "CREATE TABLE source_fixture(id INTEGER); INSERT INTO source_fixture VALUES(2);",
                 NULL, NULL, NULL));
    struct stat replacement_stat;
    assert(!stat(original_path, &replacement_stat) && replacement_stat.st_ino != original_stat.st_ino);
    __auto_type after = native_sample(&(fixture));
    assert(after.owner_epoch == before.owner_epoch && after.inode == before.inode && after.device == before.device &&
           after.mount_id == before.mount_id && after.file_size == before.file_size);
    assert(azsdk_sources_reader_fd(connection, before.mount_id, before.device, before.inode) == reader);
    assert(azsdk_sources_reader_fd(replacement, before.mount_id, before.device, before.inode) == -1);
    fixture_drop(&fixture.updater.mount);
    struct azsdk_source_sample stale;
    assert(azsdk_sources_read(before.owner_epoch, &stale, sizeof stale) == AZS_STALE && !(stale.flags & AZS_PHYSICAL));
    assert(!close_db(connection) && !close_db(replacement));
    assert(!unlink(original_path) && !unlink(moved));
    assert(azsdk_sources_configure(0, test_build, 0) == AZS_UNSUPPORTED);
    azsdk_sources_sweep();
    dlclose(handle); /* The successful production profile holds its own DSO reference. */
    printf("native SQLCipher profile/owned-file rename fixture passed (build %u)\n", test_build);
}
#if defined(AZS_CONSTRUCTOR_REGRESSION)
uint64_t early_epoch = 0;
/* The C azmod_init constructor may precede other library initialization.
 * Publish a real weak-owner record at priority101, then verify it survived until main. */
__attribute__((constructor(101))) void early_configure_and_capture() {
    test_build = 1;
    assert(azsdk_sources_configure(1, 1, 0) == AZS_OK);
    Fixture f __attribute__((cleanup(source_fixture_drop)));
    fixture_init(&f, 0x510);
    SqlProfile local_profile = {0};
    early_epoch = captured(&(f), &(local_profile)).owner_epoch;
}
#endif

int main(int argc, char **argv) {
#if defined(AZS_CONSTRUCTOR_REGRESSION)
    struct azsdk_source_sample early;
    assert(early_epoch && azsdk_sources_read(early_epoch, &early, sizeof early) == AZS_STALE);
#endif
    if (argc == 4 && !strcmp(argv[1], "--native-sqlcipher")) {
        test_build = ((unsigned) (strtoul(argv[3], NULL, 10)));
        assert(test_build == 1 || test_build == 2);
        native_sqlcipher(argv[2]);
        return 0;
    }
    test_build = argc == 2 ? ((unsigned) (strtoul(argv[1], NULL, 10))) : 1;
    assert(test_build == 1 || test_build == 2);
    api_and_guards();
    weak_lifetime_and_outputs();
#if defined(AZS_SAMPLE_BOUNDARY)
    candidate_expiry_during_sample();
#endif
    owner_address_reuse();
    binding_and_quota();
    mount_id_parser();
    borrowed_descriptor_identity();
    vfs_name_boundaries();
    rtti_name_boundaries();
    physical_capture();
    concurrency_and_busy();
    printf("native source ownership/physical fixtures passed (build %u)\n", test_build);
    return 0;
}
