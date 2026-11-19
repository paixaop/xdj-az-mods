/* SPDX-License-Identifier: GPL-3.0-only */
/* Built-in stem discovery and control commits run off the audio thread. Query
 * the file only after core supplies the path qualified for this load. */
#include "../common/azaltstack.h"
struct azn_stem_deck {
    char path[512], playing_path[512];
    uint64_t cut, load, retired, generation;
    struct azn_stem_job playing;
    int available_state, available, control, control_known, playable;
    struct azstatus_native_receipt last;
};
static struct azn_stem_deck azn_stem[4];
static pthread_mutex_t azn_stem_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t azn_stem_cv = PTHREAD_COND_INITIALIZER;
static unsigned azn_stem_dirty;
static int azn_stem_started;
static int (*azn_stem_probe)(const char *, int *);
static _Atomic unsigned azn_stem_enrolled, azn_stem_control_seen[4];

static int azn_stem_same_playback(const struct azn_stem_deck *s) {
    return s->playing.load == s->load && s->playing.cut == s->cut && !strcmp(s->playing_path, s->path);
}
static int azn_stem_control_state(const struct azn_stem_deck *s, int same) {
    if (s->available_state != AZ_AVAIL_READY) return s->available_state;
    if (s->available && s->control && !same) return AZ_AVAIL_LOADING;
    return s->control_known ? AZ_AVAIL_READY : AZ_AVAIL_LOADING;
}
static int azn_stem_same_receipt(const struct azstatus_native_receipt *a, const struct azstatus_native_receipt *b) {
    return a->attempt == b->attempt && a->valid == b->valid && a->c == b->c && a->a == b->a && a->b == b->b;
}
static void azn_stem_publish(unsigned deck) {
    struct azn_stem_deck *s = &azn_stem[deck];
    struct azsdk_load_sample live;
    if (!s->path[0] || !azsdk_load_read((int) deck, &live) || live.load_generation != s->load) return;
    int same = azn_stem_same_playback(s);
    struct azstatus_native_receipt r = {.kind = AZN_STEMS, .deck = deck, .unit = live.unit_generation, .epoch = live.invalidation_epoch, .load_generation = s->load, .attempt = s->cut, .a = s->available, .b = same && s->playable && s->control, .valid = s->available_state, .c = azn_stem_control_state(s, same)};
    if (azn_stem_same_receipt(&s->last, &r)) return;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        azn_gap(deck, AZ_STATUS_TOPIC(AZ_STATUS_STEMS));
        return;
    }
    r.size = sizeof r;
    r.version = 1;
    r.observed_ns = (uint64_t) now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
    azn_emit(&r);
    s->last = r;
}
static void azn_stem_dirty_locked(unsigned deck) {
    struct azn_stem_deck *s = &azn_stem[deck];
    if (!s->path[0] || s->generation == UINT64_MAX) return;
    ++s->generation;
    azn_stem_dirty |= 1u << deck;
    pthread_cond_signal(&azn_stem_cv);
}
static int azn_stem_probe_superseded(const struct azn_stem_deck *s, uint64_t cut, const char *path, int state) {
    return state == AZ_AVAIL_LOADING && s->playing.cut == cut && s->playing.load == s->load &&
           s->playable && !strcmp(s->playing_path, path);
}
static void azn_stem_apply_probe(unsigned d, uint64_t cut, uint64_t generation, const char *path, int state, int available) {
    struct azn_stem_deck *s = &azn_stem[d];
    if (s->cut == cut && s->generation == generation && s->path[0]) {
        if (azn_stem_probe_superseded(s, cut, path, state)) {
            state = AZ_AVAIL_READY;
            available = 1;
        }
        s->available = available;
        s->available_state = state;
        azn_stem_publish(d);
    }
}
static void *azn_stem_worker(void *unused) {
    (void) unused;
    unsigned rotation = 0;
    pthread_mutex_lock(&azn_stem_mu);
    for (;;) {
        while (!azn_stem_dirty) pthread_cond_wait(&azn_stem_cv, &azn_stem_mu);
        unsigned d = rotation;
        while (!(azn_stem_dirty & (1u << d))) d = (d + 1) % 4;
        rotation = (d + 1) % 4;
        azn_stem_dirty &= ~(1u << d);
        struct azn_stem_deck *s = &azn_stem[d];
        uint64_t cut = s->cut, generation = s->generation;
        char path[sizeof s->path];
        memcpy(path, s->path, sizeof path);
        pthread_mutex_unlock(&azn_stem_mu);
        int available = 0, state = azn_stem_probe(path, &available);
        pthread_mutex_lock(&azn_stem_mu);
        azn_stem_apply_probe(d, cut, generation, path, state, available);
    }
    return NULL;
}
void azn_stems_start(int (*probe)(const char *, int *)) {
    if (!probe) return;
    pthread_mutex_lock(&azn_stem_mu);
    if (!azn_stem_probe) {
        azn_stem_probe = probe;
        atomic_store_explicit(&azn_stem_enrolled, 1, memory_order_release);
        for (unsigned d = 0; d < 4; ++d) azn_capability(d, AZN_CAP_STEMS, 1);
    }
    pthread_mutex_unlock(&azn_stem_mu);
}
static int azn_stem_early_decode_matches(const struct azn_stem_deck *s, uint64_t load) {
    return !s->playing.cut && s->playing.pin && s->playing.load == load && s->playing.retired == s->retired &&
           s->path[0] && !strcmp(s->playing_path, s->path);
}
static void azn_stem_attach_early_decode(unsigned deck, uint64_t cut, uint64_t load, struct azn_stem_deck *s) {
    /* A decoder may finish before the file worker supplies the first path.
     * Only attach it when the original matched pack still owns this load. */
    struct azsdk_load_sample live;
    if (azn_stem_early_decode_matches(s, load) && azsdk_load_read((int) deck, &live) &&
        live.load_generation == load && azn_load_source_pin(deck, live.invalidation_epoch, load) == s->playing.pin)
        s->playing.cut = cut;
}
static void azn_stem_start_worker(struct azn_stem_deck *s) {
    if (s->path[0] && azn_stem_probe && !azn_stem_started) {
        pthread_t worker;
        if (az_thread_create(&worker, NULL, azn_stem_worker, NULL) == 0) {
            pthread_detach(worker);
            azn_stem_started = 1;
        } else s->available_state = AZ_AVAIL_FAILED;
    }
}
static int azn_stem_path_valid(const char *path) {
    return path && path[0] == '/' && strnlen(path, sizeof azn_stem[0].path) < sizeof azn_stem[0].path;
}
static void azn_stem_bind(unsigned deck, uint64_t cut, uint64_t load, const char *path) {
    if (deck >= 4 || !cut) return;
    pthread_mutex_lock(&azn_stem_mu);
    struct azn_stem_deck *s = &azn_stem[deck];
    if (cut > s->cut) {
        if (s->path[0]) s->retired = s->cut;
        s->cut = cut;
        s->load = load;
        s->path[0] = 0;
        if (load && azn_stem_path_valid(path))
            strcpy(s->path, path);
        azn_stem_attach_early_decode(deck, cut, load, s);
        s->available = 0;
        s->available_state = AZ_AVAIL_LOADING;
        memset(&s->last, 0, sizeof s->last);
        azn_stem_start_worker(s);
        if (azn_stem_started) azn_stem_dirty_locked(deck);
        azn_stem_publish(deck);
    }
    pthread_mutex_unlock(&azn_stem_mu);
}
void azn_stems_control(unsigned deck, int enabled) {
    unsigned code = 1u + (enabled != 0);
    if (deck >= 4 || !atomic_load_explicit(&azn_stem_enrolled, memory_order_acquire) ||
        atomic_load_explicit(&azn_stem_control_seen[deck], memory_order_acquire) == code) return;
    pthread_mutex_lock(&azn_stem_mu);
    struct azn_stem_deck *s = &azn_stem[deck];
    if (!s->control_known || s->control != enabled) {
        s->control = enabled;
        s->control_known = 1;
        azn_stem_publish(deck);
    }
    atomic_store_explicit(&azn_stem_control_seen[deck], code, memory_order_release);
    pthread_mutex_unlock(&azn_stem_mu);
}
static int azn_stem_job_arguments(unsigned deck, uint64_t load, const char *path) {
    return deck < 4 && load && path && path[0] == '/' && strnlen(path, 512) < 512;
}
static int azn_stem_bound_path(const struct azn_stem_deck *s, uint64_t load, const char *path) {
    return s->load == load && s->path[0] && !strcmp(s->path, path);
}
struct azn_stem_job azn_stems_job(unsigned deck, uint64_t load, const char *path) {
    struct azn_stem_job job = {0};
    struct azsdk_load_sample live;
    if (!azn_stem_job_arguments(deck, load, path) ||
        !atomic_load_explicit(&azn_stem_enrolled, memory_order_acquire) ||
        !azsdk_load_read((int) deck, &live) || load != live.load_generation) return job;
    pthread_mutex_lock(&azn_stem_mu);
    struct azn_stem_deck *s = &azn_stem[deck];
    job.pin = azn_load_source_pin(deck, live.invalidation_epoch, load);
    job.retired = s->retired;
    if (azn_stem_bound_path(s, load, path)) job.cut = s->cut;
    if (job.cut || job.pin) job.load = load;
    pthread_mutex_unlock(&azn_stem_mu);
    return job;
}
static int azn_stem_decode_current(unsigned deck, const struct azn_stem_deck *s,
                                   struct azn_stem_job job, const struct azsdk_load_sample *live) {
    return job.retired == s->retired && (!job.cut || job.cut == s->cut) &&
           (!job.pin || job.pin == azn_load_source_pin(deck, live->invalidation_epoch, job.load));
}
static void azn_stem_apply_decode(unsigned deck, struct azn_stem_deck *s, struct azn_stem_job job, const char *path, int playable) {
    if (s->load != job.load || strcmp(s->path, path)) return;
    s->playing.cut = s->cut;
    if (playable) {
        /* The completed decode is stronger than an earlier filesystem probe. */
        if (s->generation != UINT64_MAX) ++s->generation;
        s->available_state = AZ_AVAIL_READY;
        s->available = 1;
    } else azn_stem_dirty_locked(deck);
    azn_stem_publish(deck);
}
void azn_stems_decoded(unsigned deck, struct azn_stem_job job, const char *path, int playable) {
    struct azsdk_load_sample live;
    if (!azn_stem_job_arguments(deck, job.load, path) ||
        !azsdk_load_read((int) deck, &live) || job.load != live.load_generation) return;
    pthread_mutex_lock(&azn_stem_mu);
    struct azn_stem_deck *s = &azn_stem[deck];
    if (!azn_stem_decode_current(deck, s, job, &live)) {
        pthread_mutex_unlock(&azn_stem_mu);
        return;
    }
    s->playing = job;
    strcpy(s->playing_path, path);
    s->playable = playable;
    azn_stem_apply_decode(deck, s, job, path, playable);
    pthread_mutex_unlock(&azn_stem_mu);
}
void azn_stems_rescan(void) {
    if (!atomic_load_explicit(&azn_stem_enrolled, memory_order_acquire)) return;
    pthread_mutex_lock(&azn_stem_mu);
    for (unsigned d = 0; d < 4; ++d) azn_stem_dirty_locked(d);
    pthread_mutex_unlock(&azn_stem_mu);
}
static int azn_stems_seed(unsigned deck, struct azstatus_native_receipt *out) {
    if (deck >= 4 || !out || out->size < offsetof(struct azstatus_native_receipt, content_id) || out->version != 1) return 0;
    pthread_mutex_lock(&azn_stem_mu);
    int found = azn_stem[deck].last.kind == AZN_STEMS && azn_copy_receipt(out, &azn_stem[deck].last);
    pthread_mutex_unlock(&azn_stem_mu);
    return found;
}
