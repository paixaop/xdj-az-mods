/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_input.c
 * @brief Community mod services: frame filters on the panel buses and the touch filter
 *
 * frame_filter and touch_filter (azmod-mod.h) let a mod see and edit what EP147 reads from the
 * control surface. Everything here that runs per frame runs on EP147's bus threads, so the
 * dispatch is lock-free and allocation-free: registration happens only while a mod's init()
 * runs (one thread, before any filter is live), and the registered table is published with a
 * release store of its count.
 *
 * Each filter call is timed with the CPU's virtual counter. A filter that overruns its budget
 * SDK_FILTER_STRIKES times, cumulatively, is switched off until EP147 restarts: its
 * frames go on to EP147 untouched and the mod's log says so. Input frames reach filters only if
 * they arrived with a valid CRC, and bus_interpose.c re-stamps the CRC once after the chain.
 *
 * Touch: the touch panel's descriptor is tracked by open() (bus_interpose.c); read() hands its
 * input events here while any touch filter is registered. Ownership of a touch is decided when
 * the finger comes down and holds until it lifts, so EP147 is never left with half a touch.
 * Only the first contact (slot 0) is offered to mods; a taken touch is blanked for EP147.
 *
 * Included by azmod.c after community.c (it uses the community host's mod records).
 */
#include "../common/aztimestamp.h"
#ifdef __linux__
#include <linux/input.h> /* struct input_event and the touch codes */
#endif

#define SDK_FILTER_MAX 32
#define SDK_FILTER_BUDGET_US 200 /* one call; the bus thread reads ~1000 frames a second */
#define SDK_FILTER_STRIKES 8     /* overruns before a filter is switched off until EP147 restarts */
#define SDK_FRAME_MAX 256

struct sdk_filter {
    cm_mod *c;
    unsigned mask; /* 1u << AZ_BUS_*, plus AZ_FILTER_RAW */
    az_frame_fn fn;
    void *user;
    int off;          /* __atomic: switched off (overruns) */
    unsigned strikes; /* __atomic: overruns so far */
    int gone;         /* __atomic: its mod was turned off live (sdk_input_forget) */
};
static struct sdk_filter g_sdk_ff[SDK_FILTER_MAX];
static int g_sdk_ff_n;        /* __atomic: published entries */
static unsigned g_sdk_ff_bus; /* __atomic: union of the registered masks, for a one-load test */

struct sdk_touch_filter {
    cm_mod *c;
    az_touch_fn fn;
    void *user;
    int off; /* __atomic */
    unsigned strikes;
    int gone; /* __atomic */
};
static struct sdk_touch_filter g_sdk_tf[CM_MAX];
static int g_sdk_tf_n; /* __atomic */

/* g_sdk_in_init (declared in community.c) is set around each mod's init(): registration
 * services are refused at any other time. */

/* ---- timing: the generic timer, no system call -------------------------------------- */
static inline uint64_t sdk_monotonic_ticks(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return 0;
    uint64_t seconds = (uint64_t) ts.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t) ts.tv_nsec) / UINT64_C(1000000000)) return 0;
    return seconds * UINT64_C(1000000000) + (uint64_t) ts.tv_nsec;
}
static inline uint64_t sdk_budget_for_frequency(uint64_t frequency) {
    /* Divide after accounting for fractional MHz, without overflowing f * us.
     * SDK_FILTER_BUDGET_US is200, so both bounded products fit in u64. */
    return (frequency / UINT64_C(1000000)) * SDK_FILTER_BUDGET_US +
           (frequency % UINT64_C(1000000)) * SDK_FILTER_BUDGET_US / UINT64_C(1000000);
}
static inline uint64_t sdk_elapsed_ticks(uint64_t start, uint64_t end) {
    return start && compare_timestamps(end, start, 1, 1, 1) >= AZ_TIME_EQUAL ? end - start : 0; /* An unavailable clock is not an overrun. */
}
/* The deck's virtual counter on Linux; elsewhere (a macOS host test) the monotonic clock: a
 * user-space read of cntvct_el0 there does not advance reliably. */
static inline uint64_t sdk_ticks(void) {
#if defined(__aarch64__) && defined(__linux__)
    uint64_t v;
    /* "memory": without it the compiler may move this read across the filter call it times,
     * charging one filter's overrun to another (or to none) */
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
#else
    return sdk_monotonic_ticks();
#endif
}
static uint64_t g_sdk_budget_ticks; /* SDK_FILTER_BUDGET_US in sdk_ticks units, set at init */
static void sdk_timing_init(void) {
#if defined(__aarch64__) && defined(__linux__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    g_sdk_budget_ticks = sdk_budget_for_frequency(f);
#else
    g_sdk_budget_ticks = SDK_FILTER_BUDGET_US * 1000ull;
#endif
}

/* A call ran over: count it, and switch the filter off at the limit (logged once). A filter on
 * two buses runs on two bus threads, so the count is atomic. */
static void sdk_strike(cm_mod *c, unsigned *strikes, int *off, const char *what) {
    if (__atomic_add_fetch(strikes, 1, __ATOMIC_RELAXED) != SDK_FILTER_STRIKES)
        return;
    __atomic_store_n(off, 1, __ATOMIC_RELEASE);
    cm_logf(c, 1, "%s switched off for this session: it took longer than %d us %d times", what,
            SDK_FILTER_BUDGET_US, SDK_FILTER_STRIKES);
}

/* ---- registration (from init() only) ------------------------------------------------- */
static const char *sdk_frame_refusal(unsigned buses, unsigned known, az_frame_fn fn) {
    if (!g_sdk_in_init) return "only from init()";
    if (!fn) return "no function";
    return !buses || (buses & ~known) ? "unknown bus in the mask" : NULL;
}
static int sdk_frame_reuse(cm_mod *c, unsigned mask, az_frame_fn fn, void *user, int count) {
    for (int i = 0; i < count; i++) {
        struct sdk_filter *ff = &g_sdk_ff[i];
        if (ff->c != c || ff->mask != mask || !__atomic_load_n(&ff->gone, __ATOMIC_ACQUIRE)) continue;
        ff->fn = fn;
        ff->user = user;
        __atomic_store_n(&ff->gone, 0, __ATOMIC_RELEASE);
        return 1;
    }
    return 0;
}
static int cm_frame_filter(const char *id, unsigned bus_mask, az_frame_fn fn, void *user) {
    cm_mod *c = cm_self(id, CM_RA(), "frame_filter");
    if (!c)
        return -1;
    unsigned buses = bus_mask & ~AZ_FILTER_RAW;
    const unsigned known = (1u << AZ_BUS_ERP_LED) | (1u << AZ_BUS_ERP_IN) |
                           (1u << AZ_BUS_MIXER_IN) | (1u << AZ_BUS_MIXER_LED) |
                           (1u << AZ_BUS_DECK_IN);
    const char *refusal = sdk_frame_refusal(buses, known, fn);
    if (refusal) {
        cm_logf(c, 1, "frame_filter refused: %s", refusal);
        return -1;
    }
    int n = __atomic_load_n(&g_sdk_ff_n, __ATOMIC_RELAXED);
    /* A restarted mod reuses its bus registration. cm_enter keeps its callback
     * unavailable until init has completed. */
    if (sdk_frame_reuse(c, bus_mask, fn, user, n)) return 0;
    if (n >= SDK_FILTER_MAX) {
        cm_logf(c, 1, "frame_filter refused: %d filters are registered already", SDK_FILTER_MAX);
        return -1;
    }
    g_sdk_ff[n] = (struct sdk_filter) {c, bus_mask, fn, user, 0, 0, 0};
    __atomic_or_fetch(&g_sdk_ff_bus, buses, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sdk_ff_n, n + 1, __ATOMIC_RELEASE);
    return 0;
}

static int cm_touch_filter(const char *id, az_touch_fn fn, void *user) {
    cm_mod *c = cm_self(id, CM_RA(), "touch_filter");
    if (!c)
        return -1;
    if (!g_sdk_in_init || !fn) {
        cm_logf(c, 1, "touch_filter refused: %s", !fn ? "no function" : "only from init()");
        return -1;
    }
    int n = __atomic_load_n(&g_sdk_tf_n, __ATOMIC_RELAXED);
    for (int i = 0; i < n; i++) { /* as for frame filters: a mod turned on again reuses its own */
        struct sdk_touch_filter *tf = &g_sdk_tf[i];
        if (tf->c == c && __atomic_load_n(&tf->gone, __ATOMIC_ACQUIRE)) {
            tf->fn = fn;
            tf->user = user;
            __atomic_store_n(&tf->gone, 0, __ATOMIC_RELEASE);
            return 0;
        }
    }
    if (n >= CM_MAX) {
        cm_logf(c, 1, "touch_filter refused: %d touch filters are registered already", CM_MAX);
        return -1;
    }
    g_sdk_tf[n] = (struct sdk_touch_filter) {c, fn, user, 0, 0, 0};
    __atomic_store_n(&g_sdk_tf_n, n + 1, __ATOMIC_RELEASE);
    return 0;
}

/* ---- frame dispatch (bus threads) ----------------------------------------------------- */

static int sdk_filter_enter(struct sdk_filter *ff, unsigned bit) {
    return (ff->mask & bit) && !__atomic_load_n(&ff->off, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&ff->gone, __ATOMIC_ACQUIRE) && cm_enter(ff->c);
}

/* Run the editing filters of `bus` over the frame. Returns 0 if none ran, 1 if any ran (the
 * caller re-stamps the CRC), 2 if one returned AZ_FRAME_EATEN (the chain stopped there). */
static int sdk_filter_run(int bus, unsigned char *f, int len) {
    unsigned bit = 1u << bus;
    if (!(__atomic_load_n(&g_sdk_ff_bus, __ATOMIC_RELAXED) & bit))
        return 0;
    int n = __atomic_load_n(&g_sdk_ff_n, __ATOMIC_ACQUIRE), ran = 0;
    for (int i = 0; i < n; i++) {
        struct sdk_filter *ff = &g_sdk_ff[i];
        if ((ff->mask & AZ_FILTER_RAW) || !sdk_filter_enter(ff, bit))
            continue;
        g_cm_in_frame = 1;
        uint64_t t0 = sdk_ticks();
        int r = ff->fn(bus, f, len, ff->user);
        if (compare_timestamps(sdk_elapsed_ticks(t0, sdk_ticks()), g_sdk_budget_ticks, 1, 1, 1) == AZ_TIME_AFTER)
            sdk_strike(ff->c, &ff->strikes, &ff->off, "frame filter");
        g_cm_in_frame = 0;
        cm_leave(ff->c);
        ran = 1;
        if (r == AZ_FRAME_EATEN)
            return 2;
    }
    return ran;
}

static int sdk_filter_observe_enter(struct sdk_filter *ff, unsigned bit, int every_filter) {
    if (!every_filter && !(ff->mask & AZ_FILTER_RAW)) return 0;
    return sdk_filter_enter(ff, bit);
}

/* Hand filters a read-only copy of the frame. With every_filter = 0, only the AZ_FILTER_RAW
 * filters of `bus` (input as it arrived, before the core edits it); with 1, every filter of a
 * bus mods may only observe (the deck MCU buses, the mixer LED frame). */
static void sdk_filter_copy(int bus, const unsigned char *f, int len, int every_filter) {
    unsigned bit = 1u << bus;
    if (!(__atomic_load_n(&g_sdk_ff_bus, __ATOMIC_RELAXED) & bit) || len <= 0)
        return;
    int n = __atomic_load_n(&g_sdk_ff_n, __ATOMIC_ACQUIRE);
    unsigned char copy[SDK_FRAME_MAX];
    int m = len < SDK_FRAME_MAX ? len : SDK_FRAME_MAX, copied = 0;
    for (int i = 0; i < n; i++) {
        struct sdk_filter *ff = &g_sdk_ff[i];
        if (!sdk_filter_observe_enter(ff, bit, every_filter))
            continue;
        if (!copied) {
            memcpy(copy, f, (size_t) m);
            copied = 1;
        }
        g_cm_in_frame = 1;
        uint64_t t0 = sdk_ticks();
        ff->fn(bus, copy, m, ff->user);
        if (compare_timestamps(sdk_elapsed_ticks(t0, sdk_ticks()), g_sdk_budget_ticks, 1, 1, 1) == AZ_TIME_AFTER)
            sdk_strike(ff->c, &ff->strikes, &ff->off, "frame filter");
        g_cm_in_frame = 0;
        cm_leave(ff->c);
        memcpy(copy, f, (size_t) m); /* the next filter sees the frame as it arrived */
    }
}

/* ---- touch (the touch panel's read thread) -------------------------------------------- */
static int g_sdk_touch_down, g_sdk_touch_slot, g_sdk_touch_x, g_sdk_touch_y;
static int g_sdk_touch_owner = -1; /* index in g_sdk_tf of the filter that took this touch */
static int g_sdk_touch_decided;     /* this touch was offered (taken or passed) */

/* Offer a new touch to the filters; the first that takes it owns it until it lifts. */
static int sdk_touch_decide(const az_touch *t) {
    int n = __atomic_load_n(&g_sdk_tf_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        struct sdk_touch_filter *tf = &g_sdk_tf[i];
        if (__atomic_load_n(&tf->off, __ATOMIC_ACQUIRE) || __atomic_load_n(&tf->gone, __ATOMIC_ACQUIRE) ||
            !cm_enter(tf->c))
            continue;
        g_cm_in_frame = 1;
        uint64_t t0 = sdk_ticks();
        int r = tf->fn(t, tf->user);
        if (compare_timestamps(sdk_elapsed_ticks(t0, sdk_ticks()), g_sdk_budget_ticks, 1, 1, 1) == AZ_TIME_AFTER)
            sdk_strike(tf->c, &tf->strikes, &tf->off, "touch filter");
        g_cm_in_frame = 0;
        cm_leave(tf->c);
        if (r == AZ_TOUCH_TAKE)
            return i;
    }
    return -1;
}

static void sdk_touch_position(const struct input_event *e) {
    if (e->code == ABS_MT_SLOT) {
        g_sdk_touch_slot = e->value;
        return;
    }
    if (g_sdk_touch_slot != 0) return;
    switch (e->code) {
    case ABS_MT_POSITION_X: g_sdk_touch_x = e->value; break;
    case ABS_MT_POSITION_Y: g_sdk_touch_y = e->value; break;
    case ABS_MT_TRACKING_ID: g_sdk_touch_down = e->value >= 0; break;
    }
}
static void sdk_touch_deliver(const az_touch *t) {
    struct sdk_touch_filter *tf = &g_sdk_tf[g_sdk_touch_owner];
    if (__atomic_load_n(&tf->off, __ATOMIC_ACQUIRE) || __atomic_load_n(&tf->gone, __ATOMIC_ACQUIRE) ||
        !cm_enter(tf->c)) return;
    g_cm_in_frame = 1;
    uint64_t t0 = sdk_ticks();
    tf->fn(t, tf->user);
    if (compare_timestamps(sdk_elapsed_ticks(t0, sdk_ticks()), g_sdk_budget_ticks, 1, 1, 1) == AZ_TIME_AFTER)
        sdk_strike(tf->c, &tf->strikes, &tf->off, "touch filter");
    g_cm_in_frame = 0;
    cm_leave(tf->c);
}
static void sdk_touch_report(void) {
    az_touch t = {0, g_sdk_touch_x, g_sdk_touch_y, g_sdk_touch_down};
    if (t.down && !g_sdk_touch_decided) {
        g_sdk_touch_owner = sdk_touch_decide(&t);
        g_sdk_touch_decided = 1;
    } else if (g_sdk_touch_owner >= 0) {
        /* A disabled owner is not called, but still owns this touch through lift. */
        sdk_touch_deliver(&t);
    }
}
static void sdk_touch_event(const struct input_event *e) {
    if (e->type == EV_ABS) sdk_touch_position(e);
    else if (e->type == EV_KEY && e->code == BTN_TOUCH) g_sdk_touch_down = e->value != 0;
    else if (e->type == EV_SYN && e->code == SYN_REPORT) sdk_touch_report();
}
static void sdk_touch_blank(struct input_event *ev, int n) {
    if (g_sdk_touch_owner < 0) return;
    for (int i = 0; i < n; i++) {
        ev[i].type = EV_SYN;
        ev[i].code = SYN_REPORT;
        ev[i].value = 0;
    }
}
/* One read of panel events. Offer only the first contact; an owned touch remains
 * blanked through its release even if its mod has stopped meanwhile. */
static void sdk_touch_read(void *buf, ssize_t r) {
    if (r <= 0 || !__atomic_load_n(&g_sdk_tf_n, __ATOMIC_ACQUIRE)) return;
    struct input_event *ev = buf;
    int n = (int) (r / (ssize_t) sizeof *ev);
    for (int i = 0; i < n; i++) sdk_touch_event(&ev[i]);
    sdk_touch_blank(ev, n);
    if (!g_sdk_touch_down) {
        g_sdk_touch_owner = -1;
        g_sdk_touch_decided = 0;
    }
}
static void sdk_filter_raw(int bus, const unsigned char *f, int len) {
    sdk_filter_copy(bus, f, len, 0);
}
static void sdk_filter_view(int bus, const unsigned char *f, int len) {
    sdk_filter_copy(bus, f, len, 1);
}

/* A mod turned off live: none of its filters is offered anything again, until init() registers
 * them again (they then reuse these slots). UI thread, after the mod's state left LOADED. */
static void sdk_input_forget(const cm_mod *c) {
    int n = __atomic_load_n(&g_sdk_ff_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (g_sdk_ff[i].c == c)
            __atomic_store_n(&g_sdk_ff[i].gone, 1, __ATOMIC_RELEASE);
    n = __atomic_load_n(&g_sdk_tf_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (g_sdk_tf[i].c == c)
            __atomic_store_n(&g_sdk_tf[i].gone, 1, __ATOMIC_RELEASE);
}
