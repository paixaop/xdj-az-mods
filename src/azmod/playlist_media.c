/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by playlist_editor.c. A media ID and a mount path can both be reused
 * after unplugging a stick. Keep the source identity with the native mapping. */
#define MM_N 4
static pthread_mutex_t mm_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct mm_entry {
    void *server;
    unsigned media;
    uint32_t sequence;
    char mount[128];
    az_plan_set_source_v1 source;
} g_mm[MM_N];

static int plan_set_usb(const char *location, bool is_route, az_plan_set_source_v1 *out);
static bool plan_set_mount_route(const az_plan_set_source_v1 *source, char *out, size_t capacity);

static int mm_known(void *server) {
    int known = 0;
    pthread_mutex_lock(&mm_mutex);
    for (int i = 0; i < MM_N; ++i)
        known |= g_mm[i].server == server;
    pthread_mutex_unlock(&mm_mutex);
    return known;
}

static int mm_slot(void) {
    static unsigned next;
    for (int i = 0; i < MM_N; ++i)
        if (!g_mm[i].server)
            return i;
    return (int) (next++ % MM_N);
}

static void mm_publish(struct mm_entry *entry) {
    static uint32_t sequence;
    pthread_mutex_lock(&mm_mutex);
    entry->sequence = ++sequence;
    g_mm[mm_slot()] = *entry;
    pthread_mutex_unlock(&mm_mutex);
}

static int mm_read_native(struct mm_entry *entry) {
    /* These objects belong to the server's onMessage call on the library thread. */
    uintptr_t svc = 0, lib = 0, db = 0, conn = 0;
    return !mlserver_chain(entry->server, &svc, &lib, &db, &entry->media) &&
           own_q(db + SQLDB_CONN, &conn) && conn &&
           cp_mount_of((void *) conn, entry->mount, sizeof entry->mount);
}

static int mm_record(void *server, char mount[128]) {
    struct mm_entry entry = {.server = server};
    if (mm_known(server) || !mm_read_native(&entry))
        return 0;
    (void) plan_set_usb(entry.mount, false, &entry.source);
    mm_publish(&entry);
    memcpy(mount, entry.mount, sizeof entry.mount);
    return 1;
}

static const struct mm_entry *mm_latest(unsigned media) {
    const struct mm_entry *best = NULL;
    for (int i = 0; i < MM_N; ++i) {
        const struct mm_entry *entry = &g_mm[i];
        if (entry->media == media && (!best || entry->sequence > best->sequence))
            best = entry;
    }
    return best;
}

static bool mm_copy_mount(const struct mm_entry *entry, char *out, size_t n, bool plan_set) {
    if (plan_set)
        return plan_set_mount_route(&entry->source, out, n);
    int length = snprintf(out, n, "%s", entry->mount);
    return length > 0 && (size_t) length < n && out[0] == '/';
}

static bool media_mount_copy(unsigned media, char *out, size_t n, bool plan_set) {
    if (!n)
        return false;
    out[0] = 0;
    if (!media || pthread_mutex_trylock(&mm_mutex) != 0)
        return false;
    const struct mm_entry *entry = mm_latest(media);
    bool found = entry && mm_copy_mount(entry, out, n, plan_set);
    pthread_mutex_unlock(&mm_mutex);
    return found;
}

static int media_mount(unsigned media, char *out, size_t n) {
    return media_mount_copy(media, out, n, false);
}
