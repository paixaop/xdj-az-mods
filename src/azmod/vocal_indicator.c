/* SPDX-License-Identifier: GPL-3.0-only */
/* Upstream 1.2.9 painting and stem-envelope detection. The stem-marks worker
 * supplies PVDI through the shared USB/Beatport resolver. Missing data waits for
 * Track Analysis; disabled or unsuccessful analysis permits the stem fallback.
 * Native load identities fence both worker results and cached painting. */
#include "analysis_vocals.h"
#include "analysis_runtime.h"
#include "analysis_events.h"
#include "sdk_status_native.h"
#include "../common/azdeckinfo.h"

#define VX_H 4
#define VX_MIN_BAND 10
#define VX_DASH 4
#define VX_GAP 2
#define VX_RGB 0x5cb8ffu
#define VX_RGB_LIGHT 0x1060c8u
#define VX_MAXW 1280
#define VX_POLL_MS 250

static int vx_on(void) { return az_on(AZF_VOCAL_INDICATOR) && az_active(AZF_VOCAL_INDICATOR); }
static int vx_row_on(int f) { return vx_on() && az_on(f) && az_active(f); }
static int vx_prev_on(void) { return vx_row_on(AZF_VOCAL_IND_PREVIEW); }
static int vx_wave_on(void) { return vx_row_on(AZF_VOCAL_IND_WAVE); }

static void vx_completed(const az_analysis_complete_v1 *event, void *user) {
    (void) user;
    if (!(event->available_components & AZ_ANALYSIS_VOCALS)) return;
    sm_lock();
    pthread_cond_signal(&g_sm_cv);
    sm_unlock();
}
void vocal_indicator_events_init(void) {
    int rc = analysis_events_core_listen(vx_completed, NULL);
    if (rc != AZ_OK) analysis_diagnostic("Vocal Indicator completion subscription failed");
}

/* g_sm_mu protects display data. Only vx_poll writes load/state on the worker.
 * st: 0 empty, 1 checking source, 2 PVDI ready, 3 stem fallback, 4 awaiting extraction. */
static struct vx_deck {
    uint64_t load;
    unsigned gen;
    int st, analysis_on;
    struct vd_track t;
    unsigned len_s;
    struct vd_track ts;
    unsigned len_ss;
} g_vx[AZMOD_STEM_DECKS];
static unsigned g_vx_stemgen[AZMOD_STEM_DECKS];
static int vx_load(int deck, struct azstatus_native_receipt *load) {
    if (!ds_native_load_seed((unsigned) deck, load)) return 0;
    return load->a == AZN_LOADED && load->unit && load->epoch && load->load_generation;
}
static int vx_current(int deck, uint64_t generation) {
    struct azstatus_native_receipt load;
    return vx_load(deck, &load) && load.load_generation == generation;
}
static int vx_source(const struct vx_deck *e) {
    if (e->st == 2 && e->t.n && e->len_s) return VD_RB;
    if (e->analysis_on != !!az_on(AZF_VOCAL_EXTRACTION)) return VD_NONE;
    return e->st == 3 && e->ts.n && e->len_ss ? VD_STEM : VD_NONE;
}
/* g_sm_mu held; no files, model inference or scheduler locks on the paint path. */
static int vx_show(int d, const struct vd_track **t, unsigned *len) {
    struct vx_deck *e = &g_vx[d];
    int src = vx_current(d, e->load) ? vx_source(e) : VD_NONE;
    if (t) *t = src == VD_STEM ? &e->ts : &e->t;
    if (len) *len = src == VD_STEM ? e->len_ss : e->len_s;
    return src;
}
static void vx_replace(int d, uint64_t load, int state, struct vd_track *track, unsigned length) {
    sm_lock();
    struct vx_deck *e = &g_vx[d];
    uint8_t *old = e->t.v, *stems = e->ts.v;
    unsigned generation = e->gen + 1;
    *e = (struct vx_deck) {.load = load, .gen = generation, .st = state, .analysis_on = !!az_on(AZF_VOCAL_EXTRACTION), .len_s = length};
    if (track) e->t = *track;
    sm_unlock();
    free(old);
    free(stems);
    g_vx_stemgen[d] = 0;
}
static void vx_state(int d, uint64_t load, int state) {
    const struct vx_deck *e = &g_vx[d];
    if (e->load == load && e->st == state && e->analysis_on == !!az_on(AZF_VOCAL_EXTRACTION)) return;
    vx_replace(d, load, state, NULL, 0);
}
/* A status for a different load never authorizes fallback. Startup failure also
 * counts as unavailable analysis; the core reports its normal diagnostic. */
static int vx_waiting(int d, uint64_t load) {
    if (!az_on(AZF_VOCAL_EXTRACTION) || !azmod_analysis_status) return 0;
    az_analysis_status_v1 status = {.size = sizeof status};
    int rc = azmod_analysis_status((unsigned) d, &status);
    if (rc == AZ_UNSUPPORTED) return 0;
    if (rc != AZ_OK || status.load_epoch != load) return 1;
    return status.state < AZ_ANALYSIS_COMPLETE;
}
static void vx_refresh(int d, uint64_t load) {
    if (g_vx[d].st == 4 && vx_waiting(d, load)) return;
    struct vd_track track = {0};
    unsigned length = 0;
    int rc = analysis_vocal_track_loaded((unsigned) d, load, &track, &length);
    if (!vx_current(d, load)) {
        free(track.v);
        return;
    }
    if (rc == AZ_OK) {
        vx_replace(d, load, 2, &track, length);
        return;
    }
    if (rc == AZ_STALE || rc == AZ_BUSY) {
        vx_state(d, load, 1);
        return;
    }
    vx_state(d, load, vx_waiting(d, load) ? 4 : 3);
}
/* The levels from deck d's stems; t = NULL, none. */
static void vx_set_stems(int d, struct vd_track *t, unsigned len_s) {
    sm_lock();
    struct vx_deck *e = &g_vx[d];
    uint8_t *old = e->ts.v;
    if (t)
        e->ts = *t;
    else
        memset(&e->ts, 0, sizeof e->ts);
    e->len_ss = len_s;
    e->gen++;
    sm_unlock();
    free(old);
}
#define VX_STEM_VOCALS 0 /* azstem's slots */
#define VX_STEM_OTHER 1
/* Deck d's levels from its stems, whenever their envelope appears or changes. */
static void vx_from_stems(int d, unsigned cid, FILE *lg) {
    static int (*env_fn)(int, unsigned, const unsigned char **, int *, int *, long *, int *,
                         unsigned *);
    static unsigned (*gen_fn)(int);
    if (!env_fn || !gen_fn) {
        *(void **) &env_fn = dlsym(RTLD_DEFAULT, "azstem_track_env");
        *(void **) &gen_fn = dlsym(RTLD_DEFAULT, "azstem_env_gen");
        if (!env_fn || !gen_fn)
            return;
    }
    const unsigned char *env = 0;
    int nb = 0, ns = 0, rate = 0;
    long frames = 0;
    unsigned gen = 0;
    if (!env_fn(d, cid, &env, &nb, &ns, &frames, &rate, &gen) || nb <= 0 || ns <= VX_STEM_OTHER) {
        if (g_vx_stemgen[d]) { /* its stems went */
            g_vx_stemgen[d] = 0;
            vx_set_stems(d, 0, 0);
        }
        return;
    }
    if (g_vx_stemgen[d] == gen + 1)
        return;
    struct vd_track t = {0};
    int ok = vd_from_stem_env(env + (size_t) VX_STEM_VOCALS * nb, env + (size_t) VX_STEM_OTHER * nb,
                              (uint32_t) nb, (uint64_t) frames, (uint32_t) rate, &t);
    if (gen_fn(d) != gen) { /* rebuilt under the copy: the next poll sees the new one */
        free(t.v);
        return;
    }
    g_vx_stemgen[d] = gen + 1;
    unsigned len = (unsigned) ((frames + rate / 2) / rate);
    if (lg)
        fprintf(lg, "deck %d cid=%u -> from stems ok=%d n=%u len=%u\n", d, cid, ok, t.n, len);
    vx_set_stems(d, ok ? &t : 0, ok ? len : 0);
}
static void vx_poll_deck(int d, int enabled) {
    struct azstatus_native_receipt load;
    if (!enabled || !vx_load(d, &load)) {
        vx_state(d, 0, 0);
        return;
    }
    if (g_vx[d].load != load.load_generation) vx_state(d, load.load_generation, 1);
    if (g_vx[d].st != 2) vx_refresh(d, load.load_generation);
    if (g_vx[d].st == 3) vx_from_stems(d, load.content_id, NULL);
}
static void vx_poll(void) {
    static long long at;
    if (drag_now_ms() - at < VX_POLL_MS) return;
    at = drag_now_ms();
    int on = vx_prev_on() || vx_wave_on();
    for (int d = 0; d < AZMOD_STEM_DECKS; ++d) vx_poll_deck(d, on);
}

/* ---- the detailed waveform's scroll (message thread, from waveform.c azm_upd_enter) ----- */
/* EP147's update of deck d's detailed waveform, every frame it is built: the playhead in ms of
 * track time and the zoom (the zoom level over the deck's tempo). EP147 scrolls the image so
 * that x = (t - pos) * 0.15 * zoom from its playhead line, t in ms. */
static long long g_vx_wpos[AZMOD_STEM_DECKS];
static float g_vx_wzoom[AZMOD_STEM_DECKS];
static long g_vx_wwb[AZMOD_STEM_DECKS];
static void vx_upd(int d, long wwb, long pos_p, float zoom) {
    if (!pos_p || (pos_p & 7))
        return;
    g_vx_wpos[d] = *(volatile long long *) pos_p;
    g_vx_wzoom[d] = zoom;
    g_vx_wwb[d] = wwb;
}
/* ---- the paint (message thread) ------------------------------------------------------------ */
static void vx_bar(void *g, int x, int y, int w, int h, uint32_t argb) {
#ifdef AZMOD_HAVE_DAYMODE
    t_dm_bypass = 1; /* blended over whatever the theme painted */
#endif
    ((void (*)(void *, uint32_t *)) EP_23af140)(g, &argb);
    ((void (*)(void *, int, int, int, int)) EP_23af680)(g, x, y, w, h);
#ifdef AZMOD_HAVE_DAYMODE
    t_dm_bypass = 0;
#endif
}
static struct {
    unsigned gen;
    int w, valid; /* valid: col[] holds deck gen's levels for width w */
    uint8_t col[VX_MAXW];
} g_vx_cols[AZMOD_STEM_DECKS];
/* what the last repaint was asked for: gen + 1, 0 = nothing. One request per change is enough:
 * JUCE merges dirty areas, so the paint that follows covers the whole panel whatever partial
 * paints come between, and a panel that is not showing is painted whole when it shows. */
static unsigned g_vx_asked[AZMOD_STEM_DECKS];

static uintptr_t vx_full_vt(uintptr_t c) { /* the primary vtable of a child's full object, or 0 */
    uintptr_t v = *(volatile uintptr_t *) c;
    if (!ht_ptr_ok(v))
        return 0;
    uintptr_t full = c + (uintptr_t) *(volatile long *) (v - 0x10);
    return ht_ptr_ok(full) ? *(volatile uintptr_t *) full : 0;
}
static int vx_kids(uintptr_t c, uintptr_t **kids) {
    uintptr_t k = *(volatile uintptr_t *) (c + 0x40); /* childComponentList.data */
    int n = *(volatile int *) (c + 0x50);             /* .numUsed */
    if (!ht_ptr_ok(k) || n <= 0 || n > 64)
        return 0;
    *kids = (uintptr_t *) k;
    return n;
}
/* The strip's rect in the panel's coordinates: the overview's x range, centered in the band
 * above the waveform group. 1, or 0 when the panel has no overview or no band for it. */
static int vx_geom(uintptr_t panel, int r[4]) {
    uintptr_t *kids, *k2, *k3;
    int n = vx_kids(panel, &kids), gi = -1, ox = 0, ow = 0;
    for (int i = 0; i < n && gi < 0; i++) {
        uintptr_t g = kids[i];
        int n2 = ht_ptr_ok(g) ? vx_kids(g, &k2) : 0;
        for (int j = 0; j < n2 && gi < 0; j++) {
            uintptr_t w = k2[j];
            int n3 = ht_ptr_ok(w) ? vx_kids(w, &k3) : 0;
            for (int m = 0; m < n3; m++)
                if (ht_ptr_ok(k3[m]) && vx_full_vt(k3[m]) == OV_WIDGET_VT) {
                    gi = i;
                    ox = *(volatile int *) (g + 0x20) + *(volatile int *) (w + 0x20);
                    ow = *(volatile int *) (w + 0x28);
                    break;
                }
        }
    }
    if (gi < 0 || ow <= 0 || ow > VX_MAXW)
        return 0;
    int gy = *(volatile int *) (kids[gi] + 0x24), top = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t c = kids[i];
        if (i == gi || !ht_ptr_ok(c) || !(*(volatile uint32_t *) (c + 0xc0) & 2))
            continue; /* hidden overlays (input select, analyzing) do not take the band */
        int cx = *(volatile int *) (c + 0x20), cy = *(volatile int *) (c + 0x24),
            cw = *(volatile int *) (c + 0x28), ch = *(volatile int *) (c + 0x2c);
        if (cx >= ox + ow || cx + cw <= ox || cy >= gy)
            continue; /* not above the overview */
        if (cy + ch > gy)
            return 0; /* reaches into the group: no band */
        if (cy + ch > top)
            top = cy + ch;
    }
    if (gy - top < VX_MIN_BAND)
        return 0;
    r[0] = ox;
    r[1] = top + (gy - top - VX_H) / 2;
    r[2] = ow;
    r[3] = VX_H;
    return 1;
}
/* deck d's levels for a w px strip into g_vx_cols[d]; 1 = something to draw */
static int vx_cols(int d, int w) {
    sm_lock();
    struct vx_deck *e = &g_vx[d];
    const struct vd_track *t;
    unsigned len;
    int ok = vx_prev_on() && vx_show(d, &t, &len) != VD_NONE;
    if (ok && !(g_vx_cols[d].valid && g_vx_cols[d].gen == e->gen && g_vx_cols[d].w == w)) {
        vd_columns(t, len, g_vx_cols[d].col, w);
        g_vx_cols[d].gen = e->gen;
        g_vx_cols[d].w = w;
        g_vx_cols[d].valid = 1;
    }
    sm_unlock();
    return ok;
}
/* the panel's paintOverChildren, deck d (extras.c ht_paintover: panel and deck checked) */
static void vx_strip(void *g, int x, int y, int w, int h, const uint8_t *col);
static void vx_paint(void *g, uintptr_t panel, int d) {
    int r[4];
    if (d < 0 || d >= AZMOD_STEM_DECKS || !vx_geom(panel, r) || !vx_cols(d, r[2]))
        return;
    uint8_t dash[VX_MAXW];
    for (int x = 0; x < r[2]; x++) /* dashes on a fixed rhythm */
        dash[x] = (x % (VX_DASH + VX_GAP)) < VX_DASH ? g_vx_cols[d].col[x] : 0;
    vx_strip(g, r[0], r[1], r[2], r[3], dash);
}
static unsigned g_vx_wasked[AZMOD_STEM_DECKS]; /* as g_vx_asked, for the lanes */
static int vx_wave_want(int d) { /* g_sm_mu held */
    return vx_wave_on() && vx_show(d, 0, 0) != VD_NONE ? (int) g_vx[d].gen + 1 : 0;
}
/* the title setter, deck d on screen (message thread): repaint the panel when what the strip
 * should show changes */
static void vx_title(uintptr_t panel, int d) {
    if (d < 0 || d >= AZMOD_STEM_DECKS || !ht_ptr_ok(panel))
        return;
    sm_lock();
    unsigned want = vx_prev_on() && vx_show(d, 0, 0) != VD_NONE ? g_vx[d].gen + 1 : 0;
    sm_unlock();
    if (want != g_vx_asked[d]) {
        g_vx_asked[d] = want;
        ((void (*)(void *)) JUCE_COMPONENT_REPAINT)((void *) panel);
    }
    sm_lock();
    unsigned wwant = (unsigned) vx_wave_want(d);
    sm_unlock();
    if (wwant != g_vx_wasked[d]) { /* the lanes: the WaveformView this panel sits in */
        uintptr_t v = panel;
        for (int k = 0; k < 8 && ht_ptr_ok(v) && *(volatile uintptr_t *) v != PM_VIEW_VT; k++)
            v = *(volatile uintptr_t *) (v + 0x18);
        if (ht_ptr_ok(v) && *(volatile uintptr_t *) v == PM_VIEW_VT) {
            g_vx_wasked[d] = wwant;
            ((void (*)(void *)) JUCE_COMPONENT_REPAINT)((void *) v);
        }
    }
}

/* ---- above the detailed waveform (WaveformView's paintOverChildren, extras.c pm_poc_hook) - */
#define VX_WTOP 25 /* in the gap between the lane's top row of beat ticks (22 px) and the waveform */
/* deck d's level for each of w screen columns from x0, with EP147's playhead line at px */
static int vx_wave_cols(int d, int x0, int w, int px, uint8_t *col) {
    float zoom = g_vx_wzoom[d];
    if (!(zoom > 0.0f) || w <= 0 || w > VX_MAXW)
        return 0;
    sm_lock();
    int ok = vx_wave_want(d) != 0;
    if (ok) {
        const struct vd_track *t;
        vx_show(d, &t, 0);
        vd_wave_columns(t, (double) g_vx_wpos[d], 0.15 * (double) zoom, x0 - px, col, w);
    }
    sm_unlock();
    return ok;
}
static void vx_strip(void *g, int x, int y, int w, int h, const uint8_t *col) {
    static const uint8_t alpha_dark[VD_MAX_LEVEL + 1] = {0, 0x55, 0x8c, 0xc4, 0xff};
    static const uint8_t alpha_light[VD_MAX_LEVEL + 1] = {0, 0x70, 0xa0, 0xd0, 0xff};
    int light = 0;
#ifdef AZMOD_HAVE_DAYMODE
    light = __atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) && dm_flip_on();
#endif
    const uint8_t *alpha = light ? alpha_light : alpha_dark;
    uint32_t rgb = light ? VX_RGB_LIGHT : VX_RGB;
    for (int s = 0; s < w;) {
        int lv = col[s], e = s + 1;
        while (e < w && col[e] == lv)
            e++;
        if (lv)
            vx_bar(g, x + s, y, e - s, h, (uint32_t) alpha[lv] << 24 | rgb);
        s = e;
    }
}
/* the view's paintOverChildren: each deck's lane on screen, a strip along its top, clear of the
 * view's other visible children (the deck panels, the Beat FX panel) */
/* gui::waveform::DeckInfoFieldWidget and the DeckInfo{2,4}DeckFieldWidget inside it: containers
 * as wide as the deck info area (584 px) that draw nothing themselves, only their panels. A
 * panel (DeckInfo{2,4}DeckWidget) is as wide too but paints its own background in x 0..172 only
 * (its body and the divider at 170, EP147's paint and extras.c pl_panel_paint_hook alike). */
#define VX_DIF_VT EP_2d0b700
#define VX_DIF2_VT EP_2cee580
#define VX_DIF4_VT EP_2cfe3a8
#define VX_PANEL_W 172
static void vx_cut_rect(const int cr[4], int px, int *xa, int *xb) {
    if (cr[0] + cr[2] <= px) {
        if (cr[0] + cr[2] > *xa)
            *xa = cr[0] + cr[2];
    } else if (cr[0] >= px) {
        if (cr[0] < *xb)
            *xb = cr[0];
    } else {
        *xb = *xa; /* over the playhead: no strip */
    }
}
/* Narrow [*xa, *xb) of the strip at row y to what component c (visible) leaves of it: whatever
 * c draws left of the playhead px moves the start, right of it the end. The deck info
 * containers draw nothing of their own and a panel only its 172 px: both are looked into (up
 * to `deep` levels) for what their children draw; anything else counts by its bounds. */
static void vx_cut(uintptr_t c, int y, int px, int *xa, int *xb, int deep) {
    int cr[4];
    if (!ht_ptr_ok(c) || !(*(volatile uint32_t *) (c + 0xc0) & 2) || !ht_screen_rect(c, cr) ||
        cr[1] >= y + VX_H || cr[1] + cr[3] <= y || cr[0] >= *xb || cr[0] + cr[2] <= *xa)
        return;
    uintptr_t v = vx_full_vt(c);
    int container = v == VX_DIF_VT || v == VX_DIF2_VT || v == VX_DIF4_VT;
    int panel = v == KT_DI2_VT || v == KT_DI4_VT;
    if (deep <= 0 || !(container || panel)) {
        vx_cut_rect(cr, px, xa, xb);
        return;
    }
    if (panel) {
        int own[4] = {cr[0], cr[1], cr[2] < VX_PANEL_W ? cr[2] : VX_PANEL_W, cr[3]};
        vx_cut_rect(own, px, xa, xb);
    }
    uintptr_t kids = *(volatile uintptr_t *) (c + 0x40);
    int n = *(volatile int *) (c + 0x50);
    for (int k = 0; ht_ptr_ok(kids) && k < n && k < 64; k++)
        vx_cut(*(volatile uintptr_t *) (kids + 8 * k), y, px, xa, xb, deep - 1);
}
static void vx_wave_paint(uintptr_t view, void *g) {
    int rv[4];
    if (!g || !vx_wave_on() || pm_trackinfo_open(view) || !ht_screen_rect(view, rv))
        return;
    uintptr_t kids = *(volatile uintptr_t *) (view + 0x40);
    int nk = *(volatile int *) (view + 0x50);
    if (!ht_ptr_ok(kids) || nk <= 0 || nk > 64)
        return;
    static uint8_t col[VX_MAXW];
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        int r[4];
        uintptr_t box = pm_deckbox(view, d, r);
        uintptr_t ph = box ? pm_child(box, PM_PLAYHEAD_VT, 1) : 0;
        if (!ph || r[2] <= 0 || r[2] > VX_MAXW || r[3] < VX_WTOP + VX_H)
            continue;
        int px = r[0] + *(volatile int *) (ph + 0x20) + *(volatile int *) (ph + 0x28) / 2;
        int y = r[1] + VX_WTOP, xa = r[0], xb = r[0] + r[2];
        uintptr_t lane = box; /* the view's child that holds this lane */
        for (int k = 0; k < 8 && ht_ptr_ok(lane); k++) { /* and inside every level of it: the */
            int ar[4];                                    /* lane's field (1030 wide) clips it */
            if (ht_screen_rect(lane, ar)) {
                if (ar[0] > xa)
                    xa = ar[0];
                if (ar[0] + ar[2] < xb)
                    xb = ar[0] + ar[2];
            }
            uintptr_t up = *(volatile uintptr_t *) (lane + 0x18);
            if (up == view)
                break;
            lane = up;
        }
        for (int k = 0; k < nk; k++) { /* a visible sibling over the strip cuts it short */
            uintptr_t c = *(volatile uintptr_t *) (kids + 8 * k);
            if (c != lane)
                vx_cut(c, y, px, &xa, &xb, 4);
        }
        if (xb - xa < 8)
            continue;
        if (vx_wave_cols(d, xa, xb - xa, px, col))
            vx_strip(g, xa - rv[0], y - rv[1], xb - xa, VX_H, col);
    }
}
