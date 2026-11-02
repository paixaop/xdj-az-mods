/* SPDX-License-Identifier: GPL-3.0-only */
/* sdk_logic.h: the decision logic of the deck and UI services for community mods, kept
 * free of EP147, X11 and the deck so the host tests run it as it ships (sdk_deck.c and
 * sdk_ui.c call it; the host test sdk_test.c drives it).
 *
 *   sd_play_step     is the deck playing: its playhead moved forward recently
 *   sd_events        load / eject / play / stop edges between two snapshots
 *   sd_pad_mode      EP147's pad mode number -> the AZ_* pad mode a mod sees
 *   sdq_push/pop     a bounded multi-producer queue (Vyukov), lock-free on both sides
 *   sc_*             the confirm dialog's state machine and its input
 *   sd_thread_check  the rules thread_start enforces before it starts anything */
#ifndef AZMOD_SDK_LOGIC_H
#define AZMOD_SDK_LOGIC_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../common/aztimestamp.h"

/* ---- playing ----------------------------------------------------------------------
 * "Playing" is the playhead advancing: it moved forward within the last SD_PLAY_HOLD_MS.
 * The audio hook publishes the playhead every block (a few ms) while EP147 renders the
 * deck, so a paused deck stops moving within one block. A cue or a seek backwards is not
 * playing; a scratch forwards is, for as long as it lasts. */
#define SD_PLAY_HOLD_MS 250

struct sd_play {
    int have;          /* last_pos is valid */
    long long last_pos;
    long last_move_ms; /* when it last moved forward; valid only if moved */
    int moved;
};

/* loaded: 1 / 0 / -1 unknown; pos: playhead, -1 unknown. Returns 1 playing, 0 not, -1 unknown. */
static inline int sd_play_step(struct sd_play *p, int loaded, long long pos, long now_ms) {
    if (loaded == 0) {
        memset(p, 0, sizeof *p);
        return 0;
    }
    if (loaded < 0 || pos < 0)
        return -1;
    if (!p->have) { /* the first sample: no movement can be judged yet */
        p->have = 1;
        p->last_pos = pos;
        return -1;
    }
    /* These coordinates already use whole source samples; the one-sample
     * window cancels the rate when both sides are in the same sample clock. */
    if (compare_timestamps(pos, p->last_pos, 1, 1, 1) == AZ_TIME_AFTER) {
        p->moved = 1;
        p->last_move_ms = now_ms;
    }
    p->last_pos = pos;
    return p->moved && compare_timestamps(now_ms, p->last_move_ms + SD_PLAY_HOLD_MS,
                                          1000, 1000, 1) <= AZ_TIME_EQUAL ? 1 : 0;
}

/* ---- deck events ------------------------------------------------------------------
 * Edges only between known states. The first known state of a deck is its baseline (no
 * event: a mod asks deck_info for the state it starts in). An unknown sample changes
 * nothing. A new track on a loaded deck (another content id) is a load. Order within one
 * step: STOP, then EJECTED or LOADED, then PLAY. */
enum {
    SD_EV_LOADED = 1,
    SD_EV_EJECTED,
    SD_EV_PLAY,
    SD_EV_STOP
}; /* = AZ_DECK_* */

struct sd_prev {
    int loaded;  /* -2 = never known */
    unsigned cid;
    int playing; /* -2 = never known */
};
#define SD_PREV_INIT {-2, 0, -2}

static inline int sd_events(struct sd_prev *pv, int loaded, unsigned cid, int playing, int out[4]) {
    int n = 0;
    if (playing >= 0) {
        if (pv->playing == 1 && playing == 0)
            out[n++] = SD_EV_STOP;
    }
    if (loaded >= 0) {
        if (pv->loaded != -2) {
            if (loaded == 1 && (pv->loaded != 1 || (cid && pv->cid && cid != pv->cid)))
                out[n++] = SD_EV_LOADED;
            else if (loaded == 0 && pv->loaded == 1)
                out[n++] = SD_EV_EJECTED;
        }
        pv->loaded = loaded;
        if (loaded == 1 && cid)
            pv->cid = cid;
        else if (loaded == 0)
            pv->cid = 0;
    }
    if (playing >= 0) {
        if (pv->playing == 0 && playing == 1)
            out[n++] = SD_EV_PLAY;
        pv->playing = playing;
    }
    return n;
}

/* ---- pad mode ---------------------------------------------------------------------
 * fresh: pad_mode_fresh()'s answer (> 0 = EP147's mode + 1, published after this side's
 * last mode-button edge; 0 = not committed; -1 = stale). EP147's modes 3 and 4 are BEAT
 * LOOP's two pages; slip_enum is SLIP LOOP's. HOT CUE's and BEAT JUMP's numbers are not
 * established, so they are reported as unknown rather than guessed. */
enum {
    SD_PAD_HOTCUE = 0,
    SD_PAD_BEATLOOP = 1,
    SD_PAD_SLIPLOOP = 2,
    SD_PAD_BEATJUMP = 3
};
static inline int sd_pad_mode(int fresh, int slip_enum) {
    if (fresh <= 0)
        return -1;
    int m = fresh - 1;
    if (m == 3 || m == 4)
        return SD_PAD_BEATLOOP;
    if (m == slip_enum)
        return SD_PAD_SLIPLOOP;
    return -1;
}

/* ---- the job queues ---------------------------------------------------------------
 * Any thread posts; one thread drains (EP147's message thread for banners, the core's UI
 * thread for post_ui). Bounded: a full queue refuses (the caller logs it) instead of growing
 * or waiting. Dmitry Vyukov's bounded MPMC queue. */
#define SDQ_N 64 /* power of two */
enum {
    SDJ_BANNER = 1,
    SDJ_POST
};
struct sd_job {
    int kind;
    int owner;               /* the mod's index, for attribution */
    void (*fn)(void *user);  /* SDJ_POST */
    void *user;
    int ms;                  /* SDJ_BANNER */
    char text[112];          /* SDJ_BANNER: a banner shows at most 111 bytes */
};
struct sdq_cell {
    _Atomic size_t seq;
    struct sd_job job;
};
struct sdq {
    struct sdq_cell cell[SDQ_N];
    _Atomic size_t head; /* next to write */
    _Atomic size_t tail; /* next to read */
};
static inline void sdq_init(struct sdq *q) {
    for (size_t i = 0; i < SDQ_N; i++)
        atomic_store_explicit(&q->cell[i].seq, i, memory_order_relaxed);
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
}
/* 0 = queued, -1 = full. */
static inline int sdq_push(struct sdq *q, const struct sd_job *j) {
    size_t pos = atomic_load_explicit(&q->head, memory_order_relaxed);
    for (;;) {
        struct sdq_cell *c = &q->cell[pos & (SDQ_N - 1)];
        size_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        intptr_t dif = (intptr_t) seq - (intptr_t) pos;
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->head, &pos, pos + 1, memory_order_relaxed,
                                                      memory_order_relaxed)) {
                c->job = *j;
                atomic_store_explicit(&c->seq, pos + 1, memory_order_release);
                return 0;
            }
        } else if (dif < 0) {
            return -1; /* full */
        } else {
            pos = atomic_load_explicit(&q->head, memory_order_relaxed);
        }
    }
}
/* 1 = *out holds the oldest job, 0 = empty. */
static inline int sdq_pop(struct sdq *q, struct sd_job *out) {
    size_t pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
    for (;;) {
        struct sdq_cell *c = &q->cell[pos & (SDQ_N - 1)];
        size_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        intptr_t dif = (intptr_t) seq - (intptr_t) (pos + 1);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->tail, &pos, pos + 1, memory_order_relaxed,
                                                      memory_order_relaxed)) {
                *out = c->job;
                atomic_store_explicit(&c->seq, pos + SDQ_N, memory_order_release);
                return 1;
            }
        } else if (dif < 0) {
            return 0; /* empty */
        } else {
            pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
        }
    }
}

/* ---- confirm --------------------------------------------------------------------
 * One question at a time, from a mod (owner = its index) or from the core itself (owner -1).
 * A question is taken (sc_request) or refused at once because another is open; the caller
 * returns that to the asker, and a refused question gets no answer. The UI thread shows it
 * (sc_tick) as a dialog over whatever is on screen; the DJ answers (sc_answer), or the
 * dialog cannot be shown or goes away (no). Every taken question ends in exactly one answer,
 * delivered by the caller (fn(yes, user)) outside the lock that guards this state. */
enum {
    SC_IDLE = 0,
    SC_PENDING,
    SC_SHOWN
};
struct sd_confirm {
    int state;
    int owner; /* the asking mod's index, -1 = the core */
    char title[48], text[240], yes[24];
    void (*fn)(int yes, void *user);
    void *user;
};
struct sc_answer {
    void (*fn)(int yes, void *user); /* NULL = nothing to deliver */
    void *user;
    int yes;
    int owner;
};
/* 0 = taken; -1 = refused (another question is open, or no callback). */
static inline int sc_request(struct sd_confirm *c, int owner, const char *title, const char *text,
                             const char *yes, void (*fn)(int, void *), void *user) {
    if (c->state != SC_IDLE || !fn)
        return -1;
    c->owner = owner;
    snprintf(c->title, sizeof c->title, "%s", title && *title ? title : "Are you sure?");
    snprintf(c->text, sizeof c->text, "%s", text ? text : "");
    snprintf(c->yes, sizeof c->yes, "%s", yes && *yes ? yes : "OK");
    c->fn = fn;
    c->user = user;
    c->state = SC_PENDING;
    return 0;
}
static inline struct sc_answer sc_end(struct sd_confirm *c, int yes) {
    struct sc_answer a = {c->fn, c->user, yes, c->owner};
    c->state = SC_IDLE;
    c->fn = NULL;
    c->user = NULL;
    return a;
}
/* The UI thread, every tick. can_show: the dialog can be put on screen (the overlay is up).
 * Returns 1 when the caller must show the question now; *a carries an answer to deliver
 * (fn != NULL) when it ended here. */
static inline int sc_tick(struct sd_confirm *c, int can_show, struct sc_answer *a) {
    a->fn = NULL;
    if (c->state == SC_PENDING) {
        if (!can_show) {
            *a = sc_end(c, 0); /* nowhere to ask: no */
            return 0;
        }
        c->state = SC_SHOWN;
        return 1;
    }
    if (c->state == SC_SHOWN && !can_show)
        *a = sc_end(c, 0); /* the dialog went away under the question: no */
    return 0;
}
/* The DJ's answer, from the dialog. Ignored unless a question is on screen. */
static inline struct sc_answer sc_answer(struct sd_confirm *c, int yes) {
    struct sc_answer a = {NULL, NULL, 0, -1};
    if (c->state == SC_SHOWN)
        a = sc_end(c, yes ? 1 : 0);
    return a;
}
/* The dialog's input. *sel: the focused button, 0 = Cancel (where it starts), 1 = yes. kind:
 * SC_IN_KNOB (dir -1/+1), SC_IN_CLICK, SC_IN_BACK, SC_IN_TAP (hit: 0 Cancel, 1 yes, -1 neither).
 * Returns the answer (0 / 1), or -1 = no answer yet. */
enum {
    SC_IN_KNOB = 1,
    SC_IN_CLICK,
    SC_IN_BACK,
    SC_IN_TAP
};
static inline int sc_input(int *sel, int kind, int dir, int hit) {
    switch (kind) {
    case SC_IN_KNOB:
        *sel = dir < 0 ? 0 : 1;
        return -1;
    case SC_IN_CLICK:
        return *sel ? 1 : 0;
    case SC_IN_BACK:
        return 0;
    case SC_IN_TAP:
        return hit == 0 || hit == 1 ? hit : -1;
    }
    return -1;
}

/* ---- thread_start ---------------------------------------------------------------- */
#define SD_MAX_THREADS 4 /* running at once, per mod; the core cannot stop them */
enum {
    SDT_OK = 0,
    SDT_FRAME,
    SDT_STATE,
    SDT_FN,
    SDT_MANY
};
static inline int sd_thread_check(int in_frame, int loaded, uintptr_t fn, uintptr_t lo, uintptr_t hi,
                                  int started) {
    if (in_frame)
        return SDT_FRAME; /* never from the bus thread */
    if (!loaded)
        return SDT_STATE; /* a mod that is not (or no longer) loaded */
    if (!hi || fn < lo || fn >= hi)
        return SDT_FN; /* the entry point must be the mod's own code */
    if (started >= SD_MAX_THREADS)
        return SDT_MANY;
    return SDT_OK;
}

#endif
