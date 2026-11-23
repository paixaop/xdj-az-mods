/* SPDX-License-Identifier: GPL-3.0-only */
#include <sys/eventfd.h>
#include "../../src/common/azstatus_cue_origin.h"
#include "../../src/common/azstatus_analysis_origin.h"

/* Linker wrappers let the real reducer worker finish and be joined. They also
 * exercise thread-creation rollback without changing production definitions. */
static _Atomic int native_service_testing, native_service_stop;
static int native_service_fail_create, native_service_fd = -1;
static pthread_t native_service_thread;
static unsigned native_service_created;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*) (void *), void *);
int __real_pthread_detach(pthread_t);
int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*run)(void *), void *arg) {
    if (atomic_load(&native_service_testing) && native_service_fail_create) return EAGAIN;
    int rc = __real_pthread_create(thread, attr, run, arg); /* altstack-ok: test linker forwarding */
    if (!rc && atomic_load(&native_service_testing)) {
        native_service_thread = *thread;
        ++native_service_created;
    }
    return rc;
}
int __wrap_pthread_detach(pthread_t thread) {
    if (atomic_load(&native_service_testing)) return 0;
    return __real_pthread_detach(thread);
}
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (atomic_load(&native_service_testing) && atomic_load(&native_service_stop)) return 0;
    return __real_poll(fds, count, timeout);
}
int __real___poll_chk(struct pollfd *, nfds_t, int, size_t);
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes) {
    if (atomic_load(&native_service_testing) && atomic_load(&native_service_stop)) return 0;
    return __real___poll_chk(fds, count, timeout, bytes);
}
static struct azstatus_native_v1 native_service_api;
static const struct azstatus_native_v1 *native_service_export;
const struct azstatus_native_v1 *azstem_status_service_v1(void) { return native_service_export; }
static unsigned native_service_hooks;
static _Atomic unsigned native_service_takes, native_service_empty, native_service_connected;
static _Atomic uint64_t native_service_interests[4];
static struct azstatus_native_receipt native_service_queue[2];
static _Atomic unsigned native_service_next;
static int native_service_wake(void) { return native_service_fd; }
static int native_service_take(unsigned deck, struct azstatus_native_receipt *out) {
    atomic_fetch_add(&native_service_takes, 1);
    if (deck) return 0;
    unsigned at = atomic_fetch_add(&native_service_next, 1);
    if (at < 2) {
        *out = native_service_queue[at];
        return 1;
    }
    atomic_fetch_add(&native_service_empty, 1);
    return 0;
}
static int native_service_seed(unsigned deck, struct azstatus_native_receipt *out) {
    (void) deck;
    (void) out;
    return 0;
}
static void native_service_interest(unsigned deck, uint64_t topics) { atomic_store(&native_service_interests[deck], topics); }
static int native_service_capture(azn_source_capture_fn capture) {
    assert(capture == azjs_capture);
    native_service_hooks |= 1;
    return 1;
}
static int native_service_settings(int build, azn_setting_patch_fn patch) {
    assert(build == g_ep147_build && patch == az_status_host_install_island);
    native_service_hooks |= 2;
    return 1;
}
static int native_service_terminal(int build, azn_setting_patch_fn patch) {
    assert(build == g_ep147_build && patch == az_status_host_install_island);
    native_service_hooks |= 4;
    return 1;
}
static int native_service_analysis_install(int build, azn_analysis_patch_fn patch) {
    assert(build == g_ep147_build && patch == az_status_host_install_island);
    native_service_hooks |= 8;
    return 1;
}
static struct azn_analysis_source_api native_service_analysis = {.size = sizeof native_service_analysis, .version = 1, .install = native_service_analysis_install};
static const struct azn_analysis_source_api *native_service_analysis_api(void) { return &native_service_analysis; }
const struct azn_file_cue_callbacks *azfile_status_callbacks(void) {
    static const struct azn_file_cue_callbacks callbacks = {.size = sizeof callbacks, .version = 2};
    return &callbacks;
}
const struct azn_file_map_callbacks *azfile_map_callbacks(void) {
    static const struct azn_file_map_callbacks callbacks = {.size = sizeof callbacks, .version = 1};
    return &callbacks;
}
static int native_service_file_install(int build, azn_setting_patch_fn patch, const struct azn_file_cue_callbacks *callbacks) {
    assert(build == g_ep147_build && patch == az_status_host_install_island && callbacks == azfile_status_callbacks());
    native_service_hooks |= 16;
    return 1;
}
static int native_service_map_install(int build, azn_setting_patch_fn patch, const struct azn_file_map_callbacks *callbacks) {
    assert(build == g_ep147_build && patch == az_status_host_install_island && callbacks == azfile_map_callbacks());
    native_service_hooks |= 32;
    return 1;
}
static unsigned native_service_transports;
static void native_service_transport(unsigned stage, uintptr_t packet, unsigned peer) {
    assert(stage == AZN_FILE_SEND && packet == 75 && peer == 2);
    ++native_service_transports;
}
static struct azn_file_cue_api native_service_files = {.size = sizeof native_service_files, .version = 1, .install = native_service_file_install, .transport = native_service_transport, .install_maps = native_service_map_install};
static const struct azn_file_cue_api *native_service_file_api(void) { return &native_service_files; }
static void native_service_memory_connect(void) { atomic_fetch_add(&native_service_connected, 1); }
static const struct azn_memory_api *native_service_memory_api(void) {
    static const struct azn_memory_api api = {.size = sizeof api, .version = 1, .read = read_memory, .release = release_memory, .seed = seed_memory, .connect = native_service_memory_connect};
    return &api;
}
static void native_service_wait(_Atomic unsigned *counter, unsigned minimum) {
    for (unsigned tries = 0; tries < 5000; ++tries) {
        if (atomic_load(counter) >= minimum) return;
        usleep(1000);
    }
    assert(!"native worker did not reach its expected state");
}
static void test_native_service_lifecycle(void) {
    ds.enabled = 1;
    native_service_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(native_service_fd >= 0);
    atomic_store(&native_service_testing, 1);
    native_service_api = (struct azstatus_native_v1) {.size = sizeof native_service_api, .version = 2, .wake_fd = native_service_wake, .take = native_service_take, .load_seed = native_service_seed, .interest = native_service_interest, .register_source_capture = native_service_capture, .install_settings = native_service_settings, .install_terminal = native_service_terminal, .analysis_source = native_service_analysis_api, .file_cues = native_service_file_api, .memory = native_service_memory_api};
    native_service_export = &native_service_api;
    ds_native_start();
    assert(!atomic_load(&ds_native_api) && !native_service_hooks);
    native_service_api.version = 1;
    native_service_api.size = offsetof(struct azstatus_native_v1, interest);
    ds_native_start();
    assert(!atomic_load(&ds_native_api) && !native_service_hooks);
    native_service_api.size = sizeof native_service_api;
    native_service_analysis.version = native_service_files.version = 2;
    native_service_fail_create = 1;
    ds_native_start();
    assert(!atomic_load(&ds_native_api) && native_service_hooks == 7 && !native_service_created);
    native_service_analysis.version = native_service_files.version = 1;
    native_service_hooks = 0;
    ds_native_start();
    assert(!atomic_load(&ds_native_api) && native_service_hooks == 63 && !native_service_created);
    native_service_queue[0] = (struct azstatus_native_receipt) {.size = sizeof(struct azstatus_native_receipt), .version = 1, .kind = AZN_LOAD, .unit = 81, .epoch = 82, .load_generation = 83, .a = AZN_LOADED, .valid = 1};
    native_service_queue[1] = (struct azstatus_native_receipt) {.size = sizeof(struct azstatus_native_receipt), .version = 1, .kind = AZN_PLAY, .unit = 84, .a = 0, .b = 1, .valid = 1, .revision = 1};
    for (unsigned deck = 0; deck < 4; ++deck) atomic_store(&ds_native_topics[deck], AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    native_service_fail_create = 0;
    ds_native_start();
    assert(atomic_load(&ds_native_api) == &native_service_api && native_service_created == 1);
    assert(atomic_load(&native_service_connected) == 1);
    for (unsigned deck = 0; deck < 4; ++deck) assert(atomic_load(&native_service_interests[deck]) == AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    uint64_t wake = 1;
    assert(write(native_service_fd, &wake, sizeof wake) == sizeof wake);
    native_service_wait(&native_service_empty, 2);
    pthread_mutex_lock(&ds.mu);
    assert(ds.deck[0].current.epoch == 83);
    assert_native_int(AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RAW_PLAY_PAUSE, 1);
    pthread_mutex_unlock(&ds.mu);
    ds_native_start();
    assert(native_service_created == 1);
    azfile_native_transport(AZN_FILE_SEND, 75, 2);
    assert(native_service_transports == 1);
    unsigned empty = atomic_load(&native_service_empty);
    azfile_native_refresh(0, 83);
    native_service_wait(&native_service_empty, empty + 1);
    pthread_mutex_lock(&ds_native_reduce_mu);
    assert(!atomic_load(&ds_file_refresh[0]));
    pthread_mutex_unlock(&ds_native_reduce_mu);
    atomic_store(&native_service_stop, 1);
    assert(write(native_service_fd, &wake, sizeof wake) == sizeof wake);
    assert(!pthread_join(native_service_thread, NULL));
    atomic_store(&ds_native_api, NULL);
    atomic_store(&ds_file_cues, NULL);
    atomic_store(&native_service_testing, 0);
    close(native_service_fd);
    puts("PASS native service version/size checks, installation order, failed-thread rollback, single worker, queued receipt publication and refresh wake");
}
