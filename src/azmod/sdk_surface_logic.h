/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_surface_logic.h
 * @brief The drawing services' logic that does not touch X11: which view EP147 shows, the
 *        surface table and its limits, press / release / long-press tracking, premultiplied
 *        image blending and thick lines on an XRGB8888 framebuffer, and the queue that carries
 *        injected pointer events to the overlay thread.
 *
 * sdk_surface.c is the X11 side and the only user in the core; the host test sdkdraw_test.c
 * builds this header natively. Nothing here allocates, blocks or calls the system.
 */
#ifndef AZMOD_SDK_SURFACE_LOGIC_H
#define AZMOD_SDK_SURFACE_LOGIC_H

#include "../common/aztimestamp.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "azmod-mod.h"

#define SDK_SURF_MAX 16     /* surfaces, all mods together */
#define SDK_SURF_PER_MOD 4  /* surfaces one mod may create */
#define SDK_XWIN_MAX 8      /* foreign windows shown at once */
#define SDK_LONG_PRESS_MS 600
#define SDK_SURF_MAX_PX 1280 /* the panel: a surface is never larger than the screen */
#define SDK_SURF_MAX_PY 800

/* ---- views -------------------------------------------------------------------------- */

/* EP147's screen, from what the core already tracks. First match wins: the Mod Menu covers
 * everything while open; the deck screen is EP147's waveform view; Search and Browse come from
 * their views' setVisible (the Tag List and Playlist screens are Browse views); SOURCE from the
 * source list's title. Anything else is AZ_VIEW_OTHER. */
static inline int sdk_view_compute(int menu_open, int deck_view, int search, int source,
                                   int browse) {
    if (menu_open)
        return AZ_VIEW_MENU;
    if (deck_view)
        return AZ_VIEW_DECK;
    if (search)
        return AZ_VIEW_SEARCH;
    if (source)
        return AZ_VIEW_SOURCE;
    if (browse)
        return AZ_VIEW_BROWSE;
    return AZ_VIEW_OTHER;
}

/* ---- surface descriptors and the table ------------------------------------------------ */

/* A caller's descriptor is usable when it is at least as large as the first ABI's fields and
 * names a valid layer and at least one view. */
static inline int sdk_surface_desc_ok(const az_surface_v1 *s) {
    size_t need = offsetof(az_surface_v1, user) + sizeof s->user;
    if (!s || s->size < (int) need || s->size > 4096)
        return 0;
    if (s->layer != AZ_LAYER_OVER_EP147 && s->layer != AZ_LAYER_TOP)
        return 0;
    if (!s->on_paint || !s->views)
        return 0;
    return 1;
}

/* Copy as much of the caller's descriptor as this core knows; later fields stay zero. */
static inline void sdk_surface_desc_copy(az_surface_v1 *dst, const az_surface_v1 *src) {
    memset(dst, 0, sizeof *dst);
    size_t n = (size_t) src->size < sizeof *dst ? (size_t) src->size : sizeof *dst;
    memcpy(dst, src, n);
    dst->size = (int) sizeof *dst;
}

/* The slot a new surface of mod `mod` may take: -1 = the mod has SDK_SURF_PER_MOD already,
 * -2 = the table is full. used[i] is 0 for a free slot, owner[i] the mod of a used one. */
static inline int sdk_surface_slot(const int *used, const int *owner, int mod) {
    int mine = 0, free_slot = -1;
    for (int i = 0; i < SDK_SURF_MAX; i++) {
        if (used[i] && owner[i] == mod)
            mine++;
        else if (!used[i] && free_slot < 0)
            free_slot = i;
    }
    if (mine >= SDK_SURF_PER_MOD)
        return -1;
    return free_slot >= 0 ? free_slot : -2;
}

/* A placement clamped to the screen: x/y inside it, w/h at least 1 and never past its edge. */
static inline az_rect sdk_rect_clamp(az_rect r, int scr_w, int scr_h) {
    if (r.x < 0)
        r.x = 0;
    if (r.y < 0)
        r.y = 0;
    if (r.x > scr_w - 1)
        r.x = scr_w - 1;
    if (r.y > scr_h - 1)
        r.y = scr_h - 1;
    if (r.w < 1)
        r.w = 1;
    if (r.h < 1)
        r.h = 1;
    if (r.w > scr_w - r.x)
        r.w = scr_w - r.x;
    if (r.h > scr_h - r.y)
        r.h = scr_h - r.y;
    return r;
}

/* xwin_show's rectangle. A width or height of 0 or less keeps the window's own size: only its
 * position is applied (clamped onto the screen), and *keep says so. Otherwise as a surface's. */
static inline az_rect sdk_xwin_rect(az_rect r, int scr_w, int scr_h, int *keep) {
    *keep = r.w <= 0 || r.h <= 0;
    if (!*keep)
        return sdk_rect_clamp(r, scr_w, scr_h);
    az_rect p = sdk_rect_clamp((az_rect) {r.x, r.y, 1, 1}, scr_w, scr_h);
    return (az_rect) {p.x, p.y, 0, 0};
}

/* xwin_show's admission of a window: the table entry it uses (>= 0), or why it is refused.
 * owned: the window says it belongs to a program this mod launched (_NET_WM_PID). A window
 * already shown by this mod keeps its entry; hiding one that is not shown is a no-op (-1 with
 * *noop set). */
enum {
    SDK_XW_OTHER = -2,
    SDK_XW_NOTOWNED = -3,
    SDK_XW_FULL = -4
};
struct sdk_xw_ent {
    int used, mod;
    unsigned long xid;
};
static inline int sdk_xwin_admit(const struct sdk_xw_ent *t, int n, int mod, unsigned long xid,
                                 int on, int owned, int *noop) {
    *noop = 0;
    for (int i = 0; i < n; i++)
        if (t[i].used && t[i].xid == xid)
            return t[i].mod == mod ? i : SDK_XW_OTHER;
    if (!on) {
        *noop = 1;
        return -1;
    }
    if (!owned)
        return SDK_XW_NOTOWNED; /* never EP147's own windows, or another program's */
    for (int i = 0; i < n; i++)
        if (!t[i].used)
            return i;
    return SDK_XW_FULL;
}

/* ---- press tracking ----------------------------------------------------------------------
 * The deck's X server sees EP147's re-posted touches, and their releases do not always arrive,
 * so a TAP is delivered on the press (as the Mod Menu does), DOWN with it, UP when a release
 * comes, and LONG once a press has been held SDK_LONG_PRESS_MS without one. */
#define SDK_PRESS_SLOP 12 /* px a finger may move and still be a long press */
struct sdk_press {
    int down;          /* a press is in progress */
    int long_sent;     /* LONG went out for it */
    int x, y;          /* where it landed, screen pixels */
    long long t_ms;    /* when */
};
enum {
    SDK_PRESS_NONE = 0,
    SDK_PRESS_LONG
};
static inline void sdk_press_begin(struct sdk_press *p, int x, int y, long long now) {
    p->down = 1;
    p->long_sent = 0;
    p->x = x;
    p->y = y;
    p->t_ms = now;
}
/* The finger moved to (x, y) during the press: past SDK_PRESS_SLOP of where it landed, the
 * touch is a drag and no LONG goes out for it. */
static inline void sdk_press_move(struct sdk_press *p, int x, int y) {
    int dx = x - p->x, dy = y - p->y;
    if (p->down && (dx < -SDK_PRESS_SLOP || dx > SDK_PRESS_SLOP || dy < -SDK_PRESS_SLOP || dy > SDK_PRESS_SLOP))
        p->long_sent = 1;
}
/* 1 = a release ended a press that was in progress */
static inline int sdk_press_end(struct sdk_press *p) {
    int was = p->down;
    p->down = 0;
    return was;
}
/* Called every tick: SDK_PRESS_LONG exactly once per press held long enough. */
static inline int sdk_press_tick(struct sdk_press *p, long long now) {
    if (p->down && !p->long_sent && compare_timestamps(now - p->t_ms, SDK_LONG_PRESS_MS, 1000, 1000, 1) >= AZ_TIME_EQUAL) {
        p->long_sent = 1;
        return SDK_PRESS_LONG;
    }
    return SDK_PRESS_NONE;
}

/* ---- drawing on an XRGB8888 framebuffer ------------------------------------------------- */

/* A premultiplied RGBA image (bytes R, G, B, A per pixel, rows of w*4) blended at (x, y) of a
 * framebuffer whose pixels are 32-bit 0x00RRGGBB, clipped to it. Returns the pixels written. */
static inline long sdk_blend_image(uint8_t *px, int fw, int fh, int stride, int x, int y,
                                   const uint8_t *rgba, int w, int h) {
    if (!px || !rgba || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return 0;
    /* clipped in 64 bits: a mod's x or y may be anything, and x + w must not wrap around */
    long long X = x, Y = y;
    long long x0 = X < 0 ? -X : 0, y0 = Y < 0 ? -Y : 0;
    long long x1 = X + w > fw ? fw - X : w, y1 = Y + h > fh ? fh - Y : h;
    long n = 0;
    for (long long j = y0; j < y1; j++) {
        uint32_t *d = (uint32_t *) (px + (Y + j) * stride) + X;
        const uint8_t *s = rgba + (j * w) * 4;
        for (long long i = x0; i < x1; i++) {
            const uint8_t *sp = s + i * 4;
            unsigned a = sp[3];
            if (!a)
                continue;
            uint32_t dv = d[i];
            unsigned ia = 255u - a;
            unsigned r = sp[0] + (((dv >> 16) & 0xffu) * ia + 127u) / 255u;
            unsigned g = sp[1] + (((dv >> 8) & 0xffu) * ia + 127u) / 255u;
            unsigned b = sp[2] + ((dv & 0xffu) * ia + 127u) / 255u;
            d[i] = ((r > 255u ? 255u : r) << 16) | ((g > 255u ? 255u : g) << 8) |
                   (b > 255u ? 255u : b);
            n++;
        }
    }
    return n;
}

/* A line of the given width from (x0, y0) to (x1, y1), as square brush stamps along a
 * Bresenham walk. plot(x, y, size, user) paints one stamp (the caller clips). The walk is
 * bounded, so a hostile pair of endpoints cannot spin the overlay thread. */
static inline long sdk_line_walk(int x0, int y0, int x1, int y1, int width,
                                 void (*plot)(int x, int y, int size, void *user), void *user) {
    if (width < 1)
        width = 1;
    if (width > 32)
        width = 32;
    long dx = x1 > x0 ? (long) x1 - x0 : (long) x0 - x1, sx = x0 < x1 ? 1 : -1;
    long dy = y1 > y0 ? (long) y0 - y1 : (long) y1 - y0, sy = y0 < y1 ? 1 : -1;
    long err = dx + dy, steps = 0, x = x0, y = y0;
    int half = width / 2;
    for (;;) {
        plot((int) x - half, (int) y - half, width, user);
        if ((x == x1 && y == y1) || ++steps > 8192)
            break;
        long e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
    return steps;
}

/* ---- injected pointer events ------------------------------------------------------------
 * Any thread may post (a touch filter runs on the bus thread); only the overlay thread takes,
 * because only it owns the X connection. A bounded multi-producer queue: posting never blocks
 * and a full queue drops the event (counted). */
#define SDK_PQ_N 128
struct sdk_pev {
    int x, y, button, down;
};
struct sdk_pq {
    struct {
        unsigned seq; /* __atomic */
        struct sdk_pev ev;
    } cell[SDK_PQ_N];
    unsigned head; /* __atomic: producers claim here */
    unsigned tail; /* consumer only */
    unsigned dropped; /* __atomic */
};
static inline void sdk_pq_init(struct sdk_pq *q) {
    memset(q, 0, sizeof *q);
    for (unsigned i = 0; i < SDK_PQ_N; i++)
        q->cell[i].seq = i;
}
static inline int sdk_pq_post(struct sdk_pq *q, const struct sdk_pev *ev) {
    unsigned pos = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    for (;;) {
        unsigned seq = __atomic_load_n(&q->cell[pos % SDK_PQ_N].seq, __ATOMIC_ACQUIRE);
        int diff = (int) (seq - pos);
        if (diff == 0) {
            if (__atomic_compare_exchange_n(&q->head, &pos, pos + 1, 1, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED))
                break;
        } else if (diff < 0) {
            __atomic_add_fetch(&q->dropped, 1, __ATOMIC_RELAXED);
            return 0;
        } else {
            pos = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
        }
    }
    q->cell[pos % SDK_PQ_N].ev = *ev;
    __atomic_store_n(&q->cell[pos % SDK_PQ_N].seq, pos + 1, __ATOMIC_RELEASE);
    return 1;
}
static inline int sdk_pq_take(struct sdk_pq *q, struct sdk_pev *out) {
    unsigned pos = q->tail;
    unsigned seq = __atomic_load_n(&q->cell[pos % SDK_PQ_N].seq, __ATOMIC_ACQUIRE);
    if ((int) (seq - (pos + 1)) < 0)
        return 0;
    *out = q->cell[pos % SDK_PQ_N].ev;
    __atomic_store_n(&q->cell[pos % SDK_PQ_N].seq, pos + SDK_PQ_N, __ATOMIC_RELEASE);
    q->tail = pos + 1;
    return 1;
}

#endif /* AZMOD_SDK_SURFACE_LOGIC_H */
