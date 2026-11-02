/* SPDX-License-Identifier: GPL-3.0-only */
/* The deck's track snapshot under concurrency (deck_track_map.c, extracted by run.sh):
 *   - torn read: a reader never gets one track's TrackID with another's title or artist, while
 *     EP147's update rewrites the LoadedTrackInfo and the hook copies it, flat out;
 *   - lost update: two writers (the update hook and a LoadState teardown/rebuild) never
 *     interleave inside the snapshot, so neither's write is lost or half applied;
 *   - stale generation: once the LoadState is torn down, a copy of its LoadedTrackInfo is never
 *     served again, even when an update of the dying object races the teardown.
 * Usage: ltisnap_test [seconds per phase]. */
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define NDECK 4
#define MAX_DECKS 8
static const int STEM_SLOT[NDECK] = {0, 1, 2, 3};
#define logln(...) ((void) 0)
#define EP_2ddb878 ((uintptr_t) 0x2ddb878)
#define EP_2dd9288 ((uintptr_t) 0x2dd9288)
#define EP_1f91210 ((uintptr_t) 0x1f91210)
#define EP_14a2560 ((uintptr_t) 0x14a2560)
#define EP_14a2270 ((uintptr_t) 0x14a2270)
#define EP_1f9c498 ((uintptr_t) 0x1f9c498)
#define EP_1fa16d0 ((uintptr_t) 0x1fa16d0)
#define AK_VT_PBU ((uintptr_t) 0x289f798)
#define AK_VT_MGR ((uintptr_t) 0x27b8a50)
#define FGPR_VTABLE ((uintptr_t) 0x27ba108)
#define AK_SUB 0xe0UL
#define IDMAP_PBU_OFF 0x180UL
#define IDMAP_MGR_OFF 0x10UL
#define IDMAP_SUB_OFF 0x48UL
static _Atomic(void *) g_deck_self[MAX_DECKS];
static _Atomic unsigned g_ctor_calls[MAX_DECKS], g_ap_rebind, g_ap_ctor;
/* Native loop registration is covered by its separate C++ fixture. */
static _Atomic uintptr_t g_djpu_vt = 0x289f878;
static int g_sdk_loop_on;
static int azsdk_loop_register(int deck, void *unit) {
    (void) deck;
    (void) unit;
    return 0;
}
static void azsdk_loop_retire(int deck) { (void) deck; }
static int g_sdk_live_on;
static int azsdk_live_register(int deck, void *unit) {
    (void) deck;
    (void) unit;
    return 0;
}
static void azsdk_live_retire(int deck) { (void) deck; }
static int azsdk_cue_register(int deck, void *unit) { (void)deck; (void)unit; return 0; }
static void azsdk_cue_retire(int deck) { (void)deck; }
static _Atomic int g_sdk_load_on;
static int azsdk_load_register(int deck, void *unit) {
    (void) deck;
    (void) unit;
    return 0;
}
static void azsdk_load_retire(int deck) { (void) deck; }
static int azsdk_exit_register(int deck, void *unit) { (void) deck; (void) unit; return 0; }
static void azsdk_exit_retire(int deck) { (void) deck; }
/* Test hook, harness only: every __atomic_load_n in the extracted code (the snapshot reader
 * copies its fields byte by byte with it, lti_snap_get) first passes its address to ut_before,
 * which can run one injected call just before the load of a chosen address (the torn-read case
 * below: the first byte of the title). Production is unchanged; with nothing armed it is the
 * plain load. Inside the macro, __atomic_load_n is the builtin (a macro does not expand itself). */
static const void *ut_inject_at; /* the address that triggers the injection */
static void (*ut_inject)(void);  /* run once, then disarmed */
static void ut_before(const void *addr) {
    if (ut_inject && addr == ut_inject_at) {
        void (*f)(void) = ut_inject;
        ut_inject = 0;
        f();
    }
}
#define __atomic_load_n(p, o) (ut_before((const void *) (p)), __atomic_load_n(p, o))
#include LIFE_INC
#include "check.h"

#define NTRACK 4
static char titles[NTRACK][160], artists[NTRACK][160];
static void *g_lsobj;  /* the deck's LoadState */
static uintptr_t g_lti; /* its LoadedTrackInfo */
static _Atomic int stop;
static _Atomic long torn, served, cycles, late_served;

/* EP147's update: rewrite the LoadedTrackInfo field by field (as the real one does). */
static _Atomic int upd_k;
static long fake_update(void *s, void *a1, void *a2, void *a3, void *a4, void *a5, void *a6, void *a7) {
    (void) a1;
    (void) a2;
    (void) a3;
    (void) a4;
    (void) a5;
    (void) a6;
    (void) a7;
    int k = atomic_load(&upd_k);
    uintptr_t l = (uintptr_t) s;
    memset((void *) (l + LTI_ID_OFF), 0x40 + k, 16);
    const char *t = titles[k], *a = artists[k];
    memcpy((void *) (l + LTI_TITLE_OFF), &t, 8);
    memcpy((void *) (l + LTI_ARTIST_OFF), &a, 8);
    return 0;
}
static void fake_lsctor(void *s, void *a1, void *a2, void *a3, long deck, void *a5, void *a6, void *a7) {
    (void) s;
    (void) a1;
    (void) a2;
    (void) a3;
    (void) deck;
    (void) a5;
    (void) a6;
    (void) a7;
}
static void fake_lsdtor(void *s) { (void) s; }
static void fake_djctor(void *s, const void *id, void *ctx, int cap) {
    (void) s;
    (void) id;
    (void) ctx;
    (void) cap;
}
static void fake_djdtor(void *s) { (void) s; }

/* The loader thread: updates as fast as it can, cycling the tracks. The LoadedTrackInfo is
 * written only here (one writer, as in EP147), and azstem's hook copies it right after. */
static void *updater(void *arg) {
    (void) arg;
    int k = 0;
    while (!atomic_load(&stop)) {
        atomic_store(&upd_k, k);
        lti_update_hook((void *) g_lti, 0, 0, 0, 0, 0, 0, 0);
        k = (k + 1) % NTRACK;
        for (volatile int i = 0; i < 200; i++) /* EP147 does other work between updates */
            ;
    }
    return NULL;
}
/* A second snapshot writer: invalidation, as a LoadState teardown does. It never reads the
 * LoadedTrackInfo (that is only ever copied by the thread that writes it). */
static void *churner(void *arg) {
    (void) arg;
    while (!atomic_load(&stop)) {
        lti_invalidate(1);
        for (volatile int i = 0; i < 200; i++);
    }
    return NULL;
}
static int consistent(const unsigned char *id, const char *ti, const char *ar) {
    int k = id[0] - 0x40;
    if (k < 0 || k >= NTRACK)
        return 0;
    for (int i = 1; i < 16; i++)
        if (id[i] != id[0])
            return 0;
    return !strcmp(ti, titles[k]) && !strcmp(ar, artists[k]);
}
static void *reader(void *arg) {
    (void) arg;
    unsigned char id[16];
    char ti[200], ar[200];
    while (!atomic_load(&stop)) {
        if (lti_read(1, id, ti, sizeof ti, ar, sizeof ar)) {
            atomic_fetch_add(&served, 1);
            if (!consistent(id, ti, ar))
                atomic_fetch_add(&torn, 1);
        }
    }
    return NULL;
}
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
struct fake_allocation {
    void *address;
    struct fake_allocation *next;
};
static struct fake_allocation *fake_allocations;
static void *mk(size_t n) {
    void *p = aligned_alloc(16, n);
    struct fake_allocation *allocation = malloc(sizeof *allocation);
    if (!p || !allocation) abort();
    allocation->address = p;
    allocation->next = fake_allocations;
    fake_allocations = allocation;
    memset(p, 0, n);
    return p;
}
/* Keep retired native fakes mapped until every late-read check and thread join
 * has finished, then release them so native LSan verifies the fixture as well. */
static void release_fake_allocations(void) {
    struct fake_allocation *allocation = fake_allocations;
    fake_allocations = NULL;
    while (allocation) {
        struct fake_allocation *next = allocation->next;
        free(allocation->address);
        free(allocation);
        allocation = next;
    }
}
static void new_loadstate(void) {
    g_lsobj = mk(0x4b0);
    g_lti = (uintptr_t) mk(0x80);
    uintptr_t vt = EP_2ddb878, lvt = LTI_VT;
    unsigned d = 1, m = LS_MAGIC;
    memcpy(g_lsobj, &vt, 8);
    memcpy((char *) g_lsobj + LS_LTI_OFF, &g_lti, 8);
    memcpy((char *) g_lsobj + LS_DECK_OFF, &d, 4);
    memcpy((char *) g_lsobj + LS_MAGIC_OFF, &m, 4);
    memcpy((void *) g_lti, &lvt, 8);
    atomic_store(&upd_k, 0);
    fake_update((void *) g_lti, 0, 0, 0, 0, 0, 0, 0);
}

/* ---- the two races pinned down, single-threaded, before the stress phases */
/* torn read: EP147's update lands between the reader's copy of the TrackID and of the title */
static void inject_update_2(void) {
    atomic_store(&upd_k, 2);
    lti_update_hook((void *) g_lti, 0, 0, 0, 0, 0, 0, 0);
}
/* lost update: a second writer while one is inside the snapshot (its seq odd). lti_wlock makes
 * the second writer spin; without serialization it enters at once. A CPU-time
 * timer (ITIMER_VIRTUAL: it advances only while this process runs user code) ends the spin:
 * a writer that enters needs microseconds of CPU, not 200 ms, so descheduling cannot make it
 * look like a wait. The SIGALRM bound (10 s of wall time) only ends a spin that sleeps. */
#if defined(__SANITIZE_THREAD__)
#define UT_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UT_TSAN 1
#endif
#endif
#ifdef UT_TSAN
/* Under ThreadSanitizer (run.sh SAN=tsan) a siglongjmp out of the timer's handler leaves the
 * sanitizer believing the thread is still inside a signal handler for the rest of the run. There,
 * writer B runs on a thread of its own and is given 2 s of wall time to enter. The controls that
 * judge this case (ctl-nolock) run in the plain pass, which uses the CPU-time timer below. */
static _Atomic int ut_b_entered;
static void *ut_writer_b(void *arg) {
    lti_invalidate(1); /* writer B */
    atomic_store(&ut_b_entered, 1);
    return arg;
}
static int second_writer_enters(void) {
    _Atomic unsigned *seq = &g_lti_snap[1].seq;
    unsigned s0 = atomic_load(seq) & ~1u;
    atomic_store(seq, s0 + 1u); /* writer A is inside */
    atomic_store(&ut_b_entered, 0);
    pthread_t b;
    if (pthread_create(&b, NULL, ut_writer_b, NULL) != 0)
        return 1;
    for (int i = 0; i < 200 && !atomic_load(&ut_b_entered); i++)
        usleep(10000);
    int entered = atomic_load(&ut_b_entered);
    atomic_store(seq, s0 + 2u); /* writer A leaves: B enters now and finishes */
    pthread_join(b, NULL);
    return entered;
}
#else
static sigjmp_buf ut_jb;
static void ut_on_timer(int sig) { siglongjmp(ut_jb, sig == SIGVTALRM ? 1 : 2); }
static int second_writer_enters(void) {
    _Atomic unsigned *seq = &g_lti_snap[1].seq;
    unsigned s0 = atomic_load(seq) & ~1u;
    atomic_store(seq, s0 + 1u); /* writer A is inside */
    signal(SIGVTALRM, ut_on_timer);
    signal(SIGALRM, ut_on_timer);
    struct itimerval it = {{0, 0}, {0, 200000}}, off = {{0, 0}, {0, 0}};
    volatile int entered = 0;
    if (!sigsetjmp(ut_jb, 1)) {
        alarm(10);
        setitimer(ITIMER_VIRTUAL, &it, NULL);
        lti_invalidate(1); /* writer B */
        entered = 1;
    }
    setitimer(ITIMER_VIRTUAL, &off, NULL);
    alarm(0);
    signal(SIGVTALRM, SIG_DFL);
    signal(SIGALRM, SIG_DFL);
    atomic_store(seq, s0 + 2u); /* writer A leaves */
    return entered;
}
#endif

int main(int argc, char **argv) {
    ut_suite = "ltisnap";
    double secs = argc > 1 ? atof(argv[1]) : 1.0;
    for (int k = 0; k < NTRACK; k++) { /* different lengths: a mix shows up as a mismatch */
        memset(titles[k], 'a' + k, 20 + 40 * k);
        memset(artists[k], 'A' + k, 150 - 30 * k);
    }
    g_orig_lsctor = fake_lsctor;
    g_orig_lsdtor = fake_lsdtor;
    g_orig_lti_update = fake_update;
    g_orig_djpuctor = fake_djctor;
    g_orig_djpudtor = fake_djdtor;

    /* phase 0: the interleavings the stress phases hunt for, pinned down */
    new_loadstate();
    lsctor_hook(g_lsobj, 0, 0, 0, 1, 0, 0, 0); /* the deck's copy: track 0 */
    {
        unsigned char id[16];
        char ti[200], ar[200];
        ut_inject_at = g_lti_snap[1].title;
        ut_inject = inject_update_2;
        int r = lti_read(1, id, ti, sizeof ti, ar, sizeof ar);
        CHECK("torn read (pinned): an update between the TrackID and the title copies is not served torn",
              !ut_inject && r == 1 && consistent(id, ti, ar));
        ut_inject = 0;
        CHECK("lost update (pinned): a second snapshot writer waits while one is inside",
              !second_writer_enters());
        atomic_store(&upd_k, 0);
        lti_update_hook((void *) g_lti, 0, 0, 0, 0, 0, 0, 0);
        CHECK("lost update (pinned): the snapshot is served again once the writer left",
              lti_read(1, id, ti, sizeof ti, ar, sizeof ar) == 1 && id[0] == 0x40);
    }

    /* phase 1: one EP147 writer, two readers; no read may be torn */
    pthread_t w, r1, r2, c;
    pthread_create(&w, NULL, updater, NULL);
    pthread_create(&r1, NULL, reader, NULL);
    pthread_create(&r2, NULL, reader, NULL);
    /* Each stress phase runs `secs`, then on until it has done what its check counts (a loaded
     * host or a sanitizer build serves far fewer copies a second, a few hundred or fewer under
     * TSan); the 60 s cap only ends a run that makes no progress. */
    double t0 = now_s();
    while (now_s() - t0 < 60.0 && (now_s() - t0 < secs || atomic_load(&served) <= 1000))
        usleep(10000);
    atomic_store(&stop, 1);
    pthread_join(w, NULL), pthread_join(r1, NULL), pthread_join(r2, NULL);
    printf("      phase 1: served %ld, torn %ld\n", atomic_load(&served), atomic_load(&torn));
    CHECK("torn read: TrackID, title and artist always from one update", atomic_load(&torn) == 0 &&
                                                                             atomic_load(&served) > 1000);

    /* phase 2: two snapshot writers and two readers: writers serialized, nothing half written */
    atomic_store(&stop, 0), atomic_store(&served, 0), atomic_store(&torn, 0);
    pthread_create(&w, NULL, updater, NULL);
    pthread_create(&c, NULL, churner, NULL);
    pthread_create(&r1, NULL, reader, NULL);
    pthread_create(&r2, NULL, reader, NULL);
    t0 = now_s();
    while (now_s() - t0 < 60.0 && (now_s() - t0 < secs || atomic_load(&served) <= 100))
        usleep(10000);
    atomic_store(&stop, 1);
    pthread_join(w, NULL), pthread_join(c, NULL), pthread_join(r1, NULL), pthread_join(r2, NULL);
    printf("      phase 2: served %ld, torn %ld, seq %u\n", atomic_load(&served), atomic_load(&torn),
           atomic_load(&g_lti_snap[1].seq));
    CHECK("lost update: concurrent snapshot writers never leave a half-written copy",
          atomic_load(&torn) == 0 && atomic_load(&served) > 100);
    CHECK("lost update: the sequence ends even (every writer finished its write)",
          (atomic_load(&g_lti_snap[1].seq) & 1u) == 0);
    /* nothing lost: one last update is the copy */
    atomic_store(&upd_k, 3);
    lti_update_hook((void *) g_lti, 0, 0, 0, 0, 0, 0, 0);
    unsigned char id[16];
    char ti[200], ar[200];
    CHECK("lost update: the last update is what a reader gets",
          lti_read(1, id, ti, sizeof ti, ar, sizeof ar) == 1 && id[0] == 0x43 && !strcmp(ti, titles[3]));

    /* phase 3: teardown races updates of the dying object: never served afterwards.
     * First the race pinned down: an update hook that matched the deck just before the
     * teardown and copies just after it. */
    new_loadstate();
    lsctor_hook(g_lsobj, 0, 0, 0, 1, 0, 0, 0);
    lsdtor_hook(g_lsobj);
    lti_capture(1, g_lti); /* the in-flight hook's copy, after the destructor hook ran */
    CHECK("stale generation: a copy taken after the teardown started is not served",
          lti_read(1, id, ti, sizeof ti, ar, sizeof ar) == 0);
    long late = 0, rounds = 0;
    t0 = now_s();
    while (now_s() - t0 < 60.0 && (now_s() - t0 < secs || rounds <= 10)) {
        new_loadstate();
        lsctor_hook(g_lsobj, 0, 0, 0, 1, 0, 0, 0);
        atomic_store(&stop, 0);
        pthread_create(&w, NULL, updater, NULL);
        usleep(200);
        lsdtor_hook(g_lsobj); /* EP147 may still be inside an update of its LoadedTrackInfo */
        usleep(100);
        if (lti_read(1, id, ti, sizeof ti, ar, sizeof ar))
            late++; /* the dead LoadState's track served after its destructor */
        atomic_store(&stop, 1);
        pthread_join(w, NULL);
        if (lti_read(1, id, ti, sizeof ti, ar, sizeof ar))
            late++;
        rounds++;
    }
    printf("      phase 3: %ld teardowns, %ld stale reads\n", rounds, late);
    CHECK("stale generation: a torn-down LoadState's track is never served again", late == 0 && rounds > 10);
    release_fake_allocations();
    return ut_summary();
}
