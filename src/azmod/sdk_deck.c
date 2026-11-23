/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_deck.c
 * @brief Community mod services: deck_info, deck_grid, on_deck_event
 *
 * Nothing here reads EP147's memory. The track, playhead, stems state and beat grid come from
 * azstem's published per-deck state (../common/azdeckinfo.h), the tempo from the BPM feed
 * community_tick keeps (g_cm_bpm), and the pad mode from pad_mode_fresh(). deck_info and
 * deck_grid are a few atomic loads and one sequence-locked copy: any thread, never blocking.
 * "Playing" and the deck events are worked out on the core's UI thread (sdk_deck_tick, from
 * community_tick) and published from there.
 *
 * Deck numbers are logical decks 0..3. A deck that cannot be told is unknown (-1), never
 * guessed. #included by azmod.c after community.c.
 */
#include "../common/azdeckinfo.h"
#include "../common/azloadexport.h"
#include "../common/azcueexport.h"
#include "sdk_logic.h"
#include "track_service.h"

static _Atomic(azstem_deck_snapshot_fn) g_sd_snap;
static _Atomic(azstem_deck_observation_v1_fn) g_sd_observation;
static _Atomic(azstem_load_observation_v1_fn) g_sd_load_observation;
static _Atomic(azstem_loop_observation_v2_fn) g_sd_loop_observation;
static _Atomic(azstem_loop_observer_mask_v1_fn) g_sd_loop_mask;
static _Atomic(azstem_live_observation_v1_fn) g_sd_live_observation;
static _Atomic(azstem_live_observer_mask_v1_fn) g_sd_live_mask;
static _Atomic(azstem_sync_observation_v1_fn) g_sd_sync_observation;
static _Atomic(azstem_tempo_lock_observation_v1_fn) g_sd_tempo_lock_observation;
static _Atomic(azstem_cue_observation_v1_fn) g_sd_cue_take;
static _Atomic(azstem_cue_observation_v1_fn) g_sd_cue_observation;
static _Atomic(azstem_cue_observer_mask_v1_fn) g_sd_cue_mask;
static _Atomic(azstem_deck_beat_phase_fn) g_sd_phase;
static _Atomic int g_sd_playing[AZMOD_STEM_DECKS] = {-1, -1, -1, -1};

/* UI thread: look azstem's two functions up once it is loaded (dlsym takes a lock). */
static void sd_bind_cue_mask(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_cue_mask, memory_order_acquire)) {
        azstem_cue_observer_mask_v1_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_cue_observer_mask_v1");
        if (f) atomic_store_explicit(&g_sd_cue_mask, f, memory_order_release);
    }
}
static void sd_bind_cue_observation(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_cue_observation, memory_order_acquire)) {
        azstem_cue_observation_v1_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_cue_observation_v1");
        if (f) atomic_store_explicit(&g_sd_cue_observation, f, memory_order_release);
    }
}
static void sd_bind_cue_take(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_cue_take, memory_order_acquire)) {
        azstem_cue_observation_v1_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_cue_take_v1");
        if (f) atomic_store_explicit(&g_sd_cue_take, f, memory_order_release);
    }
}
static void sd_bind_load_observation(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_load_observation, memory_order_acquire)) {
        azstem_load_observation_v1_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_load_observation_v1");
        if (f) atomic_store_explicit(&g_sd_load_observation, f, memory_order_release);
    }
}
static void sd_bind_tempo_lock_observation(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_tempo_lock_observation, memory_order_acquire)) {
        azstem_tempo_lock_observation_v1_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_tempo_lock_observation_v1");
        if (f) atomic_store_explicit(&g_sd_tempo_lock_observation, f, memory_order_release);
    }
}
static void sd_bind_sync_observation(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_sync_observation, memory_order_acquire)) {
        azstem_sync_observation_v1_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_sync_observation_v1");
        if (f) atomic_store_explicit(&g_sd_sync_observation, f, memory_order_release);
    }
}
static void sd_bind_live_observation(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_live_observation, memory_order_acquire)) {
        azstem_live_observation_v1_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_live_observation_v1");
        if (f) atomic_store_explicit(&g_sd_live_observation, f, memory_order_release);
    }
}
static void sd_bind_live_mask(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_live_mask, memory_order_acquire)) {
        azstem_live_observer_mask_v1_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_live_observer_mask_v1");
        if (f) atomic_store_explicit(&g_sd_live_mask, f, memory_order_release);
    }
}
static void sd_bind_loop_observation(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_loop_observation, memory_order_acquire)) {
        azstem_loop_observation_v2_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_loop_observation_v2");
        if (f) atomic_store_explicit(&g_sd_loop_observation, f, memory_order_release);
    }
}
static void sd_bind_loop_mask(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_loop_mask, memory_order_acquire)) {
        azstem_loop_observer_mask_v1_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_loop_observer_mask_v1");
        if (f) atomic_store_explicit(&g_sd_loop_mask, f, memory_order_release);
    }
}
static void sd_bind_observation(void) {
    if (track_service_enabled() && !atomic_load_explicit(&g_sd_observation, memory_order_acquire)) {
        azstem_deck_observation_v1_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_deck_observation_v1");
        if (f) atomic_store_explicit(&g_sd_observation, f, memory_order_release);
    }
}
static void sd_bind_snap(void) {
    if (!atomic_load_explicit(&g_sd_snap, memory_order_acquire)) {
        azstem_deck_snapshot_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_deck_snapshot");
        if (f)
            atomic_store_explicit(&g_sd_snap, f, memory_order_release);
    }
}
static void sd_bind_phase(void) {
    if (!atomic_load_explicit(&g_sd_phase, memory_order_acquire)) {
        azstem_deck_beat_phase_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_deck_beat_phase");
        if (f)
            atomic_store_explicit(&g_sd_phase, f, memory_order_release);
    }
}
static void sd_bind(void) {
    sd_bind_cue_mask();
    sd_bind_cue_observation();
    sd_bind_cue_take();
    sd_bind_load_observation();
    sd_bind_tempo_lock_observation();
    sd_bind_sync_observation();
    sd_bind_live_observation();
    sd_bind_live_mask();
    sd_bind_loop_observation();
    sd_bind_loop_mask();
    sd_bind_observation();
    sd_bind_snap();
    sd_bind_phase();
}

static int sd_snapshot(int deck, struct azstem_deck_snap *s) {
    azstem_deck_snapshot_fn f = atomic_load_explicit(&g_sd_snap, memory_order_acquire);
    s->size = (int) sizeof *s;
    return f ? f(deck, s) : -1;
}

/* az_core_v1.deck_info */
static int sd_deck_info(int deck, az_deck_info *out) {
    if (!out || deck < 0 || deck >= AZMOD_STEM_DECKS)
        return -1;
    int size = out->size;
    if (size < (int) (offsetof(az_deck_info, loaded) + sizeof out->loaded))
        return -1;
    az_deck_info v;
    memset(&v, 0, sizeof v);
    v.loaded = v.playing = v.pad_mode = v.has_stems = v.stems_on = -1;
    v.pos_s = v.len_s = -1.0;
    struct azstem_deck_snap s;
    if (sd_snapshot(deck, &s) == 0) {
        v.loaded = s.loaded;
        v.content_id = s.content_id;
        v.pos_s = s.pos_s;
        v.has_stems = s.has_stems;
        v.stems_on = s.stems_on;
        snprintf(v.title, sizeof v.title, "%s", s.title);
        snprintf(v.artist, sizeof v.artist, "%s", s.artist);
    }
    v.playing = atomic_load_explicit(&g_sd_playing[deck], memory_order_relaxed);
    v.pad_mode = sd_pad_mode(pad_mode_fresh(deck % AZMOD_PAD_DECKS, deck), dg_slip_enum);
    v.bpm = cm_deck_bpm(deck);
    /* only what the caller's struct has room for; its size stays as the caller set it */
    size_t n = (size_t) size < sizeof v ? (size_t) size : sizeof v;
    memcpy((char *) out + sizeof v.size, (char *) &v + sizeof v.size, n - sizeof v.size);
    return 0;
}

/* az_core_v1.deck_grid. The bar position is not in EP147's grid cells (a position and a
 * tempo per beat, no beat number), so it is reported unknown (-1) rather than assumed 4/4
 * from the first beat. */
static int sd_deck_grid(int deck, double *beat_phase, double *bar_phase) {
    if (bar_phase)
        *bar_phase = -1.0;
    if (deck < 0 || deck >= AZMOD_STEM_DECKS)
        return -1;
    azstem_deck_beat_phase_fn f = atomic_load_explicit(&g_sd_phase, memory_order_acquire);
    double ph = -1.0;
    if (!f || f(deck, &ph) != 0)
        return -1;
    if (beat_phase)
        *beat_phase = ph;
    return 0;
}

/* ---- on_deck_event ---- */
#define SD_LISTEN_PER_MOD 4
#define SD_LISTEN_MAX 32
static struct {
    int owner; /* index into g_cm */
    void (*fn)(int deck, int what, void *user);
    void *user;
} g_sd_listen[SD_LISTEN_MAX];
static int g_sd_nlisten;

/* UI/init context only. A failed init leaves its library mapped but must not
 * consume callback capacity needed by later mods. Preserve the current init's
 * entries before its state becomes LOADED, and every already-loaded owner. */
static void sd_prune_listeners(void) {
    int kept = 0;
    for (int i = 0; i < g_sd_nlisten; ++i) {
        int owner = g_sd_listen[i].owner;
        if (owner == g_cm_loading || g_cm[owner].state == AZCM_LOADED)
            g_sd_listen[kept++] = g_sd_listen[i];
    }
    g_sd_nlisten = kept;
}

/* A mod turned off live: its listeners are dropped (init() adds them again). UI thread. */
static void sdk_deck_forget(int owner) {
    int k = 0;
    for (int i = 0; i < g_sd_nlisten; i++)
        if (g_sd_listen[i].owner != owner)
            g_sd_listen[k++] = g_sd_listen[i];
    g_sd_nlisten = k;
}

static int sd_listener_capacity(int owner) {
    int mine = 0;
    for (int i = 0; i < g_sd_nlisten; i++) mine += g_sd_listen[i].owner == owner;
    return mine < SD_LISTEN_PER_MOD && g_sd_nlisten < SD_LISTEN_MAX;
}
/* az_core_v1.on_deck_event: from init() only. */
static int sd_on_deck_event(const char *id, void (*fn)(int deck, int what, void *user),
                            void *user) {
    cm_mod *c = cm_self(id, CM_RA(), "on_deck_event");
    if (!c || !fn)
        return -1;
    int owner = (int) (c - g_cm);
    if (owner != g_cm_loading) {
        cm_logf(c, 1, "on_deck_event refused: call it from init()");
        return -1;
    }
    if (!c->hi || (uintptr_t) fn < c->lo || (uintptr_t) fn >= c->hi) {
        cm_logf(c, 1, "on_deck_event refused: the callback is not in this mod's code");
        return -1;
    }
    sd_prune_listeners();
    if (!sd_listener_capacity(owner)) {
        cm_logf(c, 1, "on_deck_event refused: at most %d per mod", SD_LISTEN_PER_MOD);
        return -1;
    }
    g_sd_listen[g_sd_nlisten].owner = owner;
    g_sd_listen[g_sd_nlisten].fn = fn;
    g_sd_listen[g_sd_nlisten].user = user;
    g_sd_nlisten++;
    return 0;
}

static int sd_cue_receipt_valid(const struct azstem_cue_observation_v1 *sample) {
    return sample->size == sizeof *sample && sample->version == 1 && sample->unit_generation &&
           sample->revision && sample->collected_ns && sample->changed_index_mask &&
           !(sample->changed_index_mask & ~UINT32_C(0x1ff)) && !sample->reserved;
}
/* UI thread, from community_tick (~30 Hz). */
static int sd_native_cue_event(int deck) {
    /* One shared GUI collector, not one native listener or queue per mod.
     * Retiring a unit invalidates its last receipt exactly once. Native cue
     * indices remain internal; no main/hot/memory classification is inferred. */
    static struct {
        uint64_t unit, revision;
    } seen[AZMOD_STEM_DECKS];
    azstem_cue_observation_v1_fn take = atomic_load_explicit(&g_sd_cue_take, memory_order_acquire);
    if (!take) return 0;
    struct azstem_cue_observation_v1 sample = {sizeof sample, 1, 0, 0, 0, 0, 0};
    int rc = track_service_enabled() ? take(deck, &sample) : -1;
    if (rc == 1) return 0;
    if (rc != 0 || !sd_cue_receipt_valid(&sample)) {
        int invalidate = seen[deck].unit != 0;
        seen[deck].unit = seen[deck].revision = 0;
        return invalidate;
    }
    if (seen[deck].unit == sample.unit_generation && seen[deck].revision == sample.revision) return 0;
    seen[deck].unit = sample.unit_generation;
    seen[deck].revision = sample.revision;
    return 1;
}
static void sdk_deck_tick(long now) {
    static struct sd_play play[AZMOD_STEM_DECKS];
    static struct sd_prev prev[AZMOD_STEM_DECKS] = {SD_PREV_INIT, SD_PREV_INIT, SD_PREV_INIT,
                                                    SD_PREV_INIT};
    sd_bind();
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        struct azstem_deck_snap s;
        int loaded = -1;
        long long pos = -1;
        unsigned cid = 0;
        if (sd_snapshot(d, &s) == 0) {
            loaded = s.loaded;
            pos = s.playhead;
            cid = s.content_id;
        }
        int playing = sd_play_step(&play[d], loaded, pos, now);
        atomic_store_explicit(&g_sd_playing[d], playing, memory_order_relaxed);
        int ev[5];
        int n = sd_events(&prev[d], loaded, cid, playing, ev);
        if (sd_native_cue_event(d)) ev[n++] = AZ_DECK_CUES_CHANGED;
        for (int k = 0; k < n; k++)
            for (int i = 0; i < g_sd_nlisten; i++) {
                cm_mod *c = &g_cm[g_sd_listen[i].owner];
                if (cm_running(c))
                    g_sd_listen[i].fn(d, ev[k], g_sd_listen[i].user);
            }
    }
}

static int sd_load_receipt_valid(const struct azstem_load_observation_v1 *receipt) {
    return receipt->size >= sizeof *receipt && receipt->version == 1 && receipt->sample.unit_generation &&
           receipt->sample.load_generation && receipt->sample.invalidation_epoch &&
           receipt->sample.observed_ns && !receipt->sample.reserved;
}
static int sd_native_load_read(int deck, struct azstem_load_observation_v1 *out) {
    azstem_load_observation_v1_fn load = atomic_load_explicit(&g_sd_load_observation, memory_order_acquire);
    struct azstem_load_observation_v1 receipt = {sizeof receipt, 1, {0}};
    if (!track_service_enabled() || !load || load(deck, &receipt) || !sd_load_receipt_valid(&receipt)) return 0;
    *out = receipt;
    return 1;
}
static int sd_native_cue_read(int deck, struct azstem_cue_observation_v1 *out) {
    azstem_cue_observation_v1_fn read = atomic_load_explicit(&g_sd_cue_observation, memory_order_acquire);
    struct azstem_cue_observation_v1 sample = {sizeof sample, 1, 0, 0, 0, 0, 0};
    if (!track_service_enabled() || !read || read(deck, &sample) || !sd_cue_receipt_valid(&sample)) return 0;
    *out = sample;
    return 1;
}
static uint32_t sd_loop_capabilities(void) {
    azstem_loop_observer_mask_v1_fn mask = atomic_load_explicit(&g_sd_loop_mask, memory_order_acquire);
    return mask && (mask() & 15u) && atomic_load_explicit(&g_sd_loop_observation, memory_order_acquire)
               ? AZ_LIVE_LOOP_ENGAGED | AZ_LIVE_LOOP_BOUNDS
               : 0;
}
static uint32_t sd_live_capabilities(azstem_live_observer_mask_v1_fn mask) {
    return mask && (mask() & 15u) && atomic_load_explicit(&g_sd_live_observation, memory_order_acquire)
               ? AZ_LIVE_PLAY_FLAGS | AZ_LIVE_QUANTIZE | AZ_LIVE_KEY_LOCK | AZ_LIVE_MASTER_TEMPO
               : 0;
}
static uint32_t sd_sync_capabilities(azstem_live_observer_mask_v1_fn mask) {
    return mask && (mask() & 15u) && atomic_load_explicit(&g_sd_sync_observation, memory_order_acquire) ? AZ_LIVE_SYNC : 0;
}
static uint32_t sd_tempo_lock_capabilities(azstem_live_observer_mask_v1_fn mask) {
    return mask && (mask() & 15u) && atomic_load_explicit(&g_sd_tempo_lock_observation, memory_order_acquire) ? AZ_LIVE_TEMPO_SLIDER_LOCKED : 0;
}
static uint32_t sd_load_capabilities(void) {
    for (int deck = 0; track_service_enabled() && deck < AZMOD_STEM_DECKS; ++deck) {
        struct azstem_load_observation_v1 receipt;
        if (sd_native_load_read(deck, &receipt)) return AZ_LIVE_NATIVE_LOAD;
    }
    return 0;
}
static uint32_t sd_cue_capabilities(void) {
    azstem_cue_observer_mask_v1_fn cue = atomic_load_explicit(&g_sd_cue_mask, memory_order_acquire);
    return track_service_enabled() && cue && (cue() & 15u) && atomic_load_explicit(&g_sd_cue_observation, memory_order_acquire) ? AZ_LIVE_CUE_REVISION : 0;
}
static int sd_tempo_valid(double bpm, uint64_t generation, uint64_t revision, uint64_t observed_ns) {
    return bpm > 20.0 && bpm < 400.0 && generation && revision && observed_ns;
}
static uint32_t sd_tempo_capabilities(void) {
    for (int deck = 0; track_service_enabled() && deck < AZMOD_STEM_DECKS; ++deck) {
        double bpm;
        uint64_t generation, revision, observed_ns;
        if (cm_tempo_read(deck, &bpm, &generation, &revision, &observed_ns) &&
            sd_tempo_valid(bpm, generation, revision, observed_ns)) return AZ_LIVE_TEMPO;
    }
    return 0;
}
static uint32_t sd_live_fields(void) {
    uint32_t mask = atomic_load_explicit(&g_sd_observation, memory_order_acquire) ? AZ_LIVE_LOADED : 0;
    mask |= sd_loop_capabilities();
    azstem_live_observer_mask_v1_fn live = atomic_load_explicit(&g_sd_live_mask, memory_order_acquire);
    mask |= sd_live_capabilities(live);
    mask |= sd_sync_capabilities(live);
    mask |= sd_tempo_lock_capabilities(live);
    if (track_service_enabled() && master_state_supported()) mask |= AZ_LIVE_MASTER;
    mask |= sd_load_capabilities();
    mask |= sd_cue_capabilities();
    mask |= sd_tempo_capabilities();
    return mask;
}
static void sd_snapshot_init(int deck, uint32_t size, az_deck_snapshot *v) {
    v->size = size;
    v->version = AZ_DATA_VERSION;
    v->deck = (uint32_t) deck;
    v->loaded = -1;
    v->master_tempo = v->quantize = v->play_mode_is_2 = v->play_pause = v->play_aux = v->key_lock = -1;
    v->sync = v->sync_raw_mode = -1;
    v->master = -1;
    v->tempo_slider_locked = -1;
    v->track.size = sizeof v->track;
    v->track.version = AZ_DATA_VERSION;
    v->loop.size = sizeof v->loop;
    v->loop.version = AZ_DATA_VERSION;
    v->loop.defined = v->loop.engaged = v->loop.reloop_available = -1;
    v->main_cue.size = sizeof v->main_cue;
    v->main_cue.version = AZ_DATA_VERSION;
}
static void sd_snapshot_load(int deck, az_deck_snapshot *v) {
    struct azstem_load_observation_v1 receipt;
    if (sd_native_load_read(deck, &receipt)) {
        v->valid |= AZ_LIVE_NATIVE_LOAD;
        v->native_load_generation = receipt.sample.load_generation;
        v->native_load_unit_generation = receipt.sample.unit_generation;
        v->native_load_invalidation_epoch = receipt.sample.invalidation_epoch;
        v->native_load_observed_ns = receipt.sample.observed_ns;
    }
}
static void sd_snapshot_cue(int deck, az_deck_snapshot *v) {
    struct azstem_cue_observation_v1 cue;
    if (sd_native_cue_read(deck, &cue)) {
        v->valid |= AZ_LIVE_CUE_REVISION;
        v->cue_unit_generation = cue.unit_generation;
        v->cue_revision = cue.revision;
        v->cue_observed_ns = cue.collected_ns;
    }
}
static int sd_loaded_receipt_valid(const struct azstem_deck_observation_v1 *receipt) {
    return receipt->size >= sizeof *receipt && receipt->version == 1 && !receipt->reserved &&
           receipt->revision && receipt->observed_ns && (receipt->loaded == 0 || receipt->loaded == 1);
}
static void sd_snapshot_loaded(int deck, az_deck_snapshot *v) {
    azstem_deck_observation_v1_fn f = atomic_load_explicit(&g_sd_observation, memory_order_acquire);
    struct azstem_deck_observation_v1 o = {sizeof o, 1, -1, 0, 0, 0};
    if (track_service_enabled() && f && !f(deck, &o) && sd_loaded_receipt_valid(&o)) {
        v->valid |= AZ_LIVE_LOADED;
        v->loaded = o.loaded;
        v->revision = o.revision;
        v->observed_ns = o.observed_ns;
    }
}
static int sd_loop_receipt_valid(const struct azstem_loop_observation_v2 *receipt) {
    return receipt->size >= sizeof *receipt && receipt->version == 2 && receipt->unit_generation &&
           receipt->revision && receipt->observed_ns;
}
static int sd_loop_bounds_valid(const struct azstem_loop_observation_v2 *loop) {
    return loop->bounds_valid == 1 && __builtin_isfinite(loop->in_s) && __builtin_isfinite(loop->out_s);
}
static void sd_snapshot_loop(int deck, az_deck_snapshot *v) {
    azstem_loop_observation_v2_fn lf = atomic_load_explicit(&g_sd_loop_observation, memory_order_acquire);
    struct azstem_loop_observation_v2 lo = {sizeof lo, 2, 0, 0, 0, -1, 0, 0, 0};
    if (track_service_enabled() && lf && !lf(deck, &lo) && sd_loop_receipt_valid(&lo)) {
        v->loop_unit_generation = lo.unit_generation;
        v->loop_revision = lo.revision;
        v->loop_observed_ns = lo.observed_ns;
        if (lo.engaged == 0 || lo.engaged == 1) {
            v->valid |= AZ_LIVE_LOOP_ENGAGED;
            v->loop.valid |= AZ_LOOP_ENGAGED;
            v->loop.engaged = lo.engaged;
        }
        if (sd_loop_bounds_valid(&lo)) {
            v->valid |= AZ_LIVE_LOOP_BOUNDS;
            v->loop.valid |= AZ_LOOP_BOUNDS;
            v->loop.in_s = lo.in_s;
            v->loop.out_s = lo.out_s;
        }
    }
}
static void sd_snapshot_live(int deck, az_deck_snapshot *v) {
    azstem_live_observation_v1_fn live = atomic_load_explicit(&g_sd_live_observation, memory_order_acquire);
    struct azstem_live_observation_v1 observation = {sizeof observation, 1, {0}};
    if (track_service_enabled() && live && !live(deck, &observation)) {
        const struct azsdk_live_sample *sample = &observation.sample;
        v->live_unit_generation = sample->unit_generation;
        v->play_revision = sample->revisions[0];
        v->play_observed_ns = sample->observed_ns[0];
        v->quantize_revision = sample->revisions[1];
        v->quantize_observed_ns = sample->observed_ns[1];
        v->key_lock_revision = sample->revisions[2];
        v->key_lock_observed_ns = sample->observed_ns[2];
        if (sample->valid_fields & AZSDK_LIVE_PLAY) {
            v->valid |= AZ_LIVE_PLAY_FLAGS;
            v->play_mode_is_2 = sample->mode_is_2;
            v->play_pause = sample->play_pause;
            v->play_aux = sample->play_aux;
        }
        if (sample->valid_fields & AZSDK_LIVE_QUANTIZE) {
            v->valid |= AZ_LIVE_QUANTIZE;
            v->quantize = sample->quantize;
        }
        if ((sample->valid_fields & AZSDK_LIVE_KEY_LOCK) &&
            (sample->key_lock == 0 || sample->key_lock == 1)) {
            /* Both admitted native MasterTempoSetter consumers map this same
             * strict boolean receipt to the Master Tempo setting. Its identity
             * and time remain the key-lock stream's, never the current load's. */
            v->valid |= AZ_LIVE_KEY_LOCK | AZ_LIVE_MASTER_TEMPO;
            v->key_lock = v->master_tempo = sample->key_lock;
        }
    }
}
static void sd_snapshot_sync(int deck, az_deck_snapshot *v) {
    azstem_sync_observation_v1_fn sf = atomic_load_explicit(&g_sd_sync_observation, memory_order_acquire);
    struct azstem_sync_observation_v1 sync = {sizeof sync, 1, 0, 0, 0, -1, 0};
    if (track_service_enabled() && sf && !sf(deck, &sync)) {
        v->sync_unit_generation = sync.unit_generation;
        v->sync_revision = sync.revision;
        v->sync_observed_ns = sync.observed_ns;
        v->sync_raw_mode = sync.raw_mode;
        if (sync.valid == 1 && (sync.raw_mode == 0 || sync.raw_mode == 2)) {
            v->valid |= AZ_LIVE_SYNC;
            v->sync = sync.raw_mode == 2;
        }
    }
}
static void sd_snapshot_tempo_lock(int deck, az_deck_snapshot *v) {
    azstem_tempo_lock_observation_v1_fn tf = atomic_load_explicit(&g_sd_tempo_lock_observation, memory_order_acquire);
    struct azstem_tempo_lock_observation_v1 tempo = {sizeof tempo, 1, 0, 0, 0, -1, 0};
    if (track_service_enabled() && tf && !tf(deck, &tempo)) {
        v->tempo_lock_unit_generation = tempo.unit_generation;
        v->tempo_lock_revision = tempo.revision;
        v->tempo_lock_observed_ns = tempo.observed_ns;
        if (tempo.valid == 1 && (tempo.raw_locked == 0 || tempo.raw_locked == 1)) {
            v->valid |= AZ_LIVE_TEMPO_SLIDER_LOCKED;
            v->tempo_slider_locked = tempo.raw_locked;
        }
    }
}
static void sd_snapshot_tempo_master(int deck, az_deck_snapshot *v) {
    if (track_service_enabled()) {
        int master;
        if (master_state_read(&master, &v->master_revision, &v->master_observed_ns)) {
            v->valid |= AZ_LIVE_MASTER;
            v->master = master == deck;
        }
        double bpm = 0.0;
        int tempo_valid = cm_tempo_read(deck, &bpm, &v->tempo_state_generation,
                                        &v->tempo_revision, &v->tempo_observed_ns);
        if (tempo_valid && bpm > 20.0 && bpm < 400.0 && v->tempo_state_generation &&
            v->tempo_revision && v->tempo_observed_ns) {
            v->valid |= AZ_LIVE_TEMPO;
            v->tempo = bpm;
        }
    }
}
static int sd_deck_snapshot_v1(int deck, az_deck_snapshot *out) {
    /* The earlier development prefix ends immediately before the loop stream timestamps. */
    if (!out || st_record_size(out) < offsetof(az_deck_snapshot, loop_unit_generation) || deck < 0 || deck >= AZMOD_STEM_DECKS) return AZ_INVALID;
    az_deck_snapshot v = {0};
    sd_snapshot_init(deck, st_record_size(out), &v);
    sd_snapshot_load(deck, &v);
    sd_snapshot_cue(deck, &v);
    sd_snapshot_loaded(deck, &v);
    sd_snapshot_loop(deck, &v);
    sd_snapshot_live(deck, &v);
    sd_snapshot_sync(deck, &v);
    sd_snapshot_tempo_lock(deck, &v);
    sd_snapshot_tempo_master(deck, &v);
    size_t bytes = v.size < sizeof v ? v.size : sizeof v;
    memcpy(out, &v, bytes);
    return AZ_OK;
}
