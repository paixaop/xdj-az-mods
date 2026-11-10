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

static void azn_stem_publish(unsigned deck) {
    struct azn_stem_deck *s = &azn_stem[deck];
    struct azsdk_load_sample live;
    if (!s->path[0] || !azsdk_load_read((int)deck, &live) || live.load_generation != s->load) return;
    int same = s->playing.load==s->load && s->playing.cut==s->cut && !strcmp(s->playing_path,s->path);
    struct azstatus_native_receipt r = {.kind = AZN_STEMS, .deck = deck, .unit = live.unit_generation,
        .epoch = live.invalidation_epoch, .load_generation = s->load, .attempt = s->cut,
        .a = s->available, .b = same && s->playable && s->control, .valid = s->available_state,
        .c = s->control_known ? AZ_AVAIL_READY : AZ_AVAIL_LOADING};
    if (r.valid != AZ_AVAIL_READY) r.c = r.valid;
    else if (s->available && s->control && !same) r.c = AZ_AVAIL_LOADING;
    if (s->last.attempt == r.attempt && s->last.valid == r.valid && s->last.c == r.c &&
        s->last.a == r.a && s->last.b == r.b) return;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now)!=0) { azn_gap(deck,AZ_STATUS_TOPIC(AZ_STATUS_STEMS)); return; }
    r.size=sizeof r; r.version=1;
    r.observed_ns=(uint64_t)now.tv_sec*UINT64_C(1000000000)+now.tv_nsec;
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
static void *azn_stem_worker(void *unused) {
    (void)unused;
    unsigned rotation = 0;
    pthread_mutex_lock(&azn_stem_mu);
    for (;;) {
        while (!azn_stem_dirty) pthread_cond_wait(&azn_stem_cv, &azn_stem_mu);
        unsigned d = rotation;
        while (!(azn_stem_dirty & (1u << d))) d = (d+1)%4;
        rotation = (d+1)%4;
        azn_stem_dirty &= ~(1u << d);
        struct azn_stem_deck *s = &azn_stem[d];
        uint64_t cut = s->cut, generation = s->generation;
        char path[sizeof s->path];
        memcpy(path, s->path, sizeof path);
        pthread_mutex_unlock(&azn_stem_mu);
        int available = 0, state = azn_stem_probe(path, &available);
        pthread_mutex_lock(&azn_stem_mu);
        if (s->cut == cut && s->generation == generation && s->path[0]) {
            if (state==AZ_AVAIL_LOADING && s->playing.cut==cut && s->playing.load==s->load &&
                s->playable && !strcmp(s->playing_path,path)) {
                state=AZ_AVAIL_READY;
                available=1;
            }
            s->available = available;
            s->available_state = state;
            azn_stem_publish(d);
        }
    }
    return NULL;
}
void azn_stems_start(int (*probe)(const char *, int *)) {
    if (!probe) return;
    pthread_mutex_lock(&azn_stem_mu);
    if (!azn_stem_probe) {
        azn_stem_probe = probe;
        atomic_store_explicit(&azn_stem_enrolled,1,memory_order_release);
        for (unsigned d=0; d<4; ++d) azn_capability(d,AZN_CAP_STEMS,1);
    }
    pthread_mutex_unlock(&azn_stem_mu);
}
static void azn_stem_bind(unsigned deck, uint64_t cut, uint64_t load, const char *path) {
    if (deck >= 4 || !cut) return;
    pthread_mutex_lock(&azn_stem_mu);
    struct azn_stem_deck *s = &azn_stem[deck];
    if (cut > s->cut) {
        if (s->path[0]) s->retired=s->cut;
        s->cut = cut;
        s->load = load;
        s->path[0] = 0;
        if (load && path && path[0]=='/' && strnlen(path,sizeof s->path)<sizeof s->path)
            strcpy(s->path,path);
        /* A decoder may finish before the file worker supplies the first path.
         * Only attach it when the original matched pack still owns this load. */
        struct azsdk_load_sample live;
        if (!s->playing.cut && s->playing.pin && s->playing.load==load && s->playing.retired==s->retired &&
            s->path[0] && !strcmp(s->playing_path,s->path) && azsdk_load_read((int)deck,&live) &&
            live.load_generation==load && azn_load_source_pin(deck,live.invalidation_epoch,load)==s->playing.pin)
            s->playing.cut=cut;
        s->available = 0;
        s->available_state = AZ_AVAIL_LOADING;
        memset(&s->last,0,sizeof s->last);
        if (s->path[0] && azn_stem_probe && !azn_stem_started) {
            pthread_t worker;
            if (az_thread_create(&worker,NULL,azn_stem_worker,NULL)==0) {
                pthread_detach(worker);
                azn_stem_started=1;
            } else s->available_state=AZ_AVAIL_FAILED;
        }
        if (azn_stem_started) azn_stem_dirty_locked(deck);
        azn_stem_publish(deck);
    }
    pthread_mutex_unlock(&azn_stem_mu);
}
void azn_stems_control(unsigned deck,int enabled) {
    unsigned code=1u+(enabled!=0);
    if (deck>=4 || !atomic_load_explicit(&azn_stem_enrolled,memory_order_acquire) ||
        atomic_load_explicit(&azn_stem_control_seen[deck],memory_order_acquire)==code) return;
    pthread_mutex_lock(&azn_stem_mu);
    struct azn_stem_deck *s=&azn_stem[deck];
    if (!s->control_known || s->control!=enabled) {
        s->control=enabled;
        s->control_known=1;
        azn_stem_publish(deck);
    }
    atomic_store_explicit(&azn_stem_control_seen[deck],code,memory_order_release);
    pthread_mutex_unlock(&azn_stem_mu);
}
struct azn_stem_job azn_stems_job(unsigned deck,uint64_t load,const char *path) {
    struct azn_stem_job job={0};
    struct azsdk_load_sample live;
    if (deck>=4 || !load || !path || path[0]!='/' || strnlen(path,512)>=512 ||
        !atomic_load_explicit(&azn_stem_enrolled,memory_order_acquire) ||
        !azsdk_load_read((int)deck,&live) || load!=live.load_generation) return job;
    pthread_mutex_lock(&azn_stem_mu);
    struct azn_stem_deck *s=&azn_stem[deck];
    job.pin=azn_load_source_pin(deck,live.invalidation_epoch,load);
    job.retired=s->retired;
    if (s->load==load && s->path[0] && !strcmp(s->path,path)) job.cut=s->cut;
    if (job.cut || job.pin) job.load=load;
    pthread_mutex_unlock(&azn_stem_mu);
    return job;
}
void azn_stems_decoded(unsigned deck,struct azn_stem_job job,const char *path,int playable) {
    struct azsdk_load_sample live;
    if (deck>=4 || !job.load || !path || path[0]!='/' || strnlen(path,512)>=512 ||
        !azsdk_load_read((int)deck,&live) || job.load!=live.load_generation) return;
    pthread_mutex_lock(&azn_stem_mu);
    struct azn_stem_deck *s=&azn_stem[deck];
    if (job.retired!=s->retired || (job.cut && job.cut!=s->cut) ||
        (job.pin && job.pin!=azn_load_source_pin(deck,live.invalidation_epoch,job.load))) {
        pthread_mutex_unlock(&azn_stem_mu);
        return;
    }
    s->playing=job;
    strcpy(s->playing_path,path);
    s->playable=playable;
    if (s->load==job.load && !strcmp(s->path,path)) {
        s->playing.cut=s->cut;
        if (playable) {
            /* The completed decode is stronger than an earlier filesystem probe. */
            if (s->generation!=UINT64_MAX) ++s->generation;
            s->available_state=AZ_AVAIL_READY;
            s->available=1;
        } else azn_stem_dirty_locked(deck);
        azn_stem_publish(deck);
    }
    pthread_mutex_unlock(&azn_stem_mu);
}
void azn_stems_rescan(void) {
    if (!atomic_load_explicit(&azn_stem_enrolled,memory_order_acquire)) return;
    pthread_mutex_lock(&azn_stem_mu);
    for (unsigned d=0; d<4; ++d) azn_stem_dirty_locked(d);
    pthread_mutex_unlock(&azn_stem_mu);
}
static int azn_stems_seed(unsigned deck, struct azstatus_native_receipt *out) {
    if (deck>=4 || !out || out->size<offsetof(struct azstatus_native_receipt,content_id) || out->version!=1) return 0;
    pthread_mutex_lock(&azn_stem_mu);
    int found = azn_stem[deck].last.kind == AZN_STEMS && azn_copy_receipt(out,&azn_stem[deck].last);
    pthread_mutex_unlock(&azn_stem_mu);
    return found;
}
