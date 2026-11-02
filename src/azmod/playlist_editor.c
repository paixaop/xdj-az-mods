/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file playlist_editor.c
 * @brief Playlist Editing: reorder, create, rename and delete playlists and folders in Browse
 *
 * Reordering is a view plus one native write. Holding the browse knob arms a grab (EP147's
 * own long press, seen through knobev_hook); turning the knob moves the held row by
 * permuting which data row EP147's row painters are asked to draw, and a click commits.
 * The commit runs EP147's music_library::InformationUpdater::movePlaylistTrack on the
 * library thread at a MusicLibraryServer onMessage boundary (mlserver_onmsg_hook), after
 * the row identity captured from the browse hierarchy has been re-proven and the
 * connection is in autocommit. BACK cancels. The hooks live on the message thread.
 *
 * Creating, renaming and deleting playlists and folders use EP147's SoftwareKeyboardWidget,
 * lent from SearchView to BrowseView and fed through intercepted keyboard listener slots
 * (nm_* and nl_*), then write the library rows through EP147's own sqlite connection
 * (nl_create, nl_rename, fd_delete). The label paint hook (hlabel_paint_hook) draws the
 * landing flash, selection brackets, cut ghost and stems mark on playlist and track labels;
 * either feature installs it via install_hlabel_paint_slot. The reorder and naming hooks are
 * installed by install_reorder and follow the Mod Menu's Playlist Editing toggle live; off,
 * the deck is stock. The selection hooks (mediaobj_find_hook, lsfind_hook) are shared with
 * MAKE STEMS.
 *
 * Entry points: azmod_naming_playlist, azmod_naming_folder, azmod_naming_rename,
 * azmod_naming_create, azmod_naming_cancel, azmod_naming_dirty, install_reorder,
 * install_selection_hooks, pledit_at_start.
 */
#include "../common/azsources.h"
/* Set once before any library hooks are installed. This diagnostic registry never
 * qualifies a loaded track from a current media number. */
static int g_sdk_sources_on;
/* gui::disp_data::BrowseDispData: BrowseDispDataCreator+0x10 (vptr 0x2dcc3a0), also
 * reachable via meow::MappedObjPtr<IBrowseDispDataAccess>. Sub-objects, as stored by its
 * ctor 0x1f3fb20:
 *   +0x58 BrowseDispStatus      (vptr 0x29c4f88)
 *   +0x60 BrowsePosition        (vptr 0x29c4ff0)
 *   +0x68 RootCategoryDispList  (vptr 0x29c5088)
 *   +0x70 SubCategoryDispList   (vptr 0x29672e0)
 *   +0x78 BrowseTrackInfoData   (vptr 0x2967338)
 *   +0x80 array of BrowseDispList (vptr 0x2967398), stride 0x1d8, count at +0x90,
 *         shared out-of-range default at +0x98
 * Vtable slot +0x98 (0x1f40098) indexes the +0x80 array. It is getListState(paneIndex),
 * one element per pane, not getTrack(row). */
#define BDD_STRIDE 0x1d8

/* fillPane(creator, paneIndex) at 0x1f45660 is the disp_data fill loop; it calls the line
 * builder 0x1f44418 (return address 0x1f46060). Chain to the source side:
 *   [creator+0x10] -> BrowseDispData (destination)
 *   [creator+0x08] -> AccessWrapperCreator; its +0x08 is the object BrowseDataListAccess
 *                     (vtable 0x2dcc700) wraps
 * That source object has flags at +0x1ba/+0x1e0 and two containers at +0x248/+0x250
 * (selected by a bool) of 0x70-stride elements. It is the last object before the display
 * snapshot, so the first place where row identity and row index are both in scope. */
/* Row background. The grabbed state is shown only by recoloring the selected row, so if
 * the row cannot be recolored the gesture has no indication at all.
 *
 * gui::browse::TrackListWidget is its own juce::TableListBoxModel (model subobject at
 * this+0x30, vtable 0x2b7f588). Model slot 3 is paintRowBackground:
 *     0x19af348 -> model+0x88 -> 0x1ba2d78 -> 0x1bcfb78
 *     (this, juce::Graphics*, int row, int width, int height, bool rowIsSelected, bool extra)
 *
 * Diagnostic flags, inert by default:
 *   /home/root/settings/azmod-rowpaint.log   install + log args
 *   /tmp/azmod-rowskip                        also skip the original for the SELECTED row */
/* 0x1bcfb78 builds an 8-byte fill object via 0x1bcfa20(this, row, w5, w6), then branches
 * on [this+4]:
 *   path A: 0x23af140 setColour(Graphics&, Colour*) + 0x23af680 fillRect(x,y,w,h), then a
 *           second color (static 0x3b8e898) for the 1px separator fillRect(0, h-1, w, 1)
 *   path B: 0x23af7d0(Graphics*, Colour*), a single call
 * The color arrives by pointer, so it can be read and overwritten in place.
 * Gating is thread-local and spans only the original call, so no other widget's
 * setColour is touched. */
#define SETCOLOUR_ADDR EP_23af140
    /* Set while the playlist-edit gesture holds a row. */
static _Atomic int g_row_grabbed;
/* A juce::Colour here is 4 bytes in B,G,R,A order. Selected row fill is #007DE1,
 * a normal row #3C3C3C, the separator (second setColour of each row) black.
 * Substitution is guarded twice: only the first setColour of a row (the fill), and only
 * when the bytes are exactly EP147's selection blue. If the native color ever changes we
 * stop substituting and the row stays native, rather than showing a misleading color. */
#define ROW_SEL_B 0xe1 /* #007DE1 in memory order */
#define ROW_SEL_G 0x7d
#define ROW_SEL_R 0x00
/* Grabbed color #B25A00 (amber), chosen for EP147's white row text: contrast 4.82:1,
 * better than the native selected blue's 4.18:1, and distinguishable from it by hue. */
#define ROW_GRAB_B 0x00 /* #B25A00 in memory order (B,G,R) */
#define ROW_GRAB_G 0x5a
#define ROW_GRAB_R 0xb2
/* The narrow preview list takes path A; the wide track list takes path B, so hooking
 * setColour alone cannot recolor the visible row. Three widget instances paint this list. */
/* Cell text paints into the same juce::Graphics as the row background but is drawn by
 * child Components (gui::IconHoldableScrollLabel), so a Graphics-pointer scope cannot
 * single out one row. paintCell's first instruction is a pc-relative tbnz (not inline
 * hookable), so per-cell work goes through the model vtable slot instead. */
/* Path B (0x23af7d0) cannot be hooked inline: its first four instructions contain a
 * pc-relative branch, which azm_install_hook declines. Instead we overpaint from inside
 * paintRowBackground: backgrounds paint before cell contents, so a rect drawn here sits
 * under the text like the native bar. EP147 is non-PIE, so its fillRect is called directly.
 * The fill stops at h-1 to keep the native separator row. */
#define g_juce_fillrect ((void (*)(void *, int, int, int, int)) EP_23af680)
/* Which list has knob focus: EP147 paints that list's selected row blue and other lists'
 * selected rows gray. The row painter's w5 on the SELECTED row is 1 exactly while that
 * list has the knob. A move may only start on a list whose selection
 * is blue, otherwise a gray row in an unfocused list could be grabbed. */
static __thread uintptr_t g_rowpaint_w;
#define LF_N 6
static struct {
    _Atomic unsigned long w;
    _Atomic int blue;
} g_lfocus[LF_N];
static __thread int g_in_rowpaint; /* 0 none, 1 normal row, 2 SELECTED row */
static void (*g_orig_setcolour)(void *, void *);
static __thread int g_rowpaint_seq; /* which color call within this row */
static __thread int g_in_grabcell;
static int g_rowprobe; /* azmod-rowpaint.log at init: enables the row-paint probe logs + /tmp test
                          triggers */
static void row_try_yellow(void *col) {
    if (g_in_rowpaint != 2 || g_rowpaint_seq != 0 || !col)
        return;
    if (!__atomic_load_n(&g_row_grabbed, __ATOMIC_RELAXED))
        return;
    volatile unsigned char *p = (volatile unsigned char *) col;
    if (p[0] != ROW_SEL_B || p[1] != ROW_SEL_G || p[2] != ROW_SEL_R)
        return;
    p[0] = ROW_GRAB_B;
    p[1] = ROW_GRAB_G;
    p[2] = ROW_GRAB_R;
}
static void lfocus_note(uintptr_t w, int blue) {
    for (int i = 0; i < LF_N; i++)
        if (__atomic_load_n(&g_lfocus[i].w, __ATOMIC_RELAXED) == w) {
            __atomic_store_n(&g_lfocus[i].blue, blue, __ATOMIC_RELAXED);
            return;
        }
    static _Atomic int next;
    int i = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % LF_N;
    __atomic_store_n(&g_lfocus[i].blue, blue, __ATOMIC_RELAXED);
    __atomic_store_n(&g_lfocus[i].w, (unsigned long) w, __ATOMIC_RELEASE);
}
/* 1 = this list's selection is blue (it has the knob), 0 = gray, -1 = not seen */
static int list_has_knob(unsigned long w) {
    if (!w)
        return -1;
    for (int i = 0; i < LF_N; i++)
        if (__atomic_load_n(&g_lfocus[i].w, __ATOMIC_ACQUIRE) == w)
            return __atomic_load_n(&g_lfocus[i].blue, __ATOMIC_RELAXED);
    return -1;
}
static void setcolour_hook(void *g, void *col) {
    /* Cells of the grabbed row paint some selected blue of their own (right margins)
     * inside paintCell, outside the row painter. Swap only that exact blue. */
    if (g_in_grabcell && col && __atomic_load_n(&g_row_grabbed, __ATOMIC_RELAXED)) {
        volatile unsigned char *q = (volatile unsigned char *) col;
        if (q[0] == ROW_SEL_B && q[1] == ROW_SEL_G && q[2] == ROW_SEL_R) {
            q[0] = ROW_GRAB_B;
            q[1] = ROW_GRAB_G;
            q[2] = ROW_GRAB_R;
        }
    }
    /* Record which precondition rejected the substitution, for the probe log. */
    int grabbed = __atomic_load_n(&g_row_grabbed, __ATOMIC_RELAXED);
    int did = 0, why = 0;
    if (g_in_rowpaint != 2)
        why = 1;
    else if (g_rowpaint_seq != 0)
        why = 2;
    else if (!col)
        why = 3;
    else if (!grabbed)
        why = 4;
    else {
        volatile unsigned char *p = (volatile unsigned char *) col;
        if (p[0] != ROW_SEL_B || p[1] != ROW_SEL_G || p[2] != ROW_SEL_R)
            why = 5;
        else {
            row_try_yellow(col);
            did = 1;
        }
    }
    if (g_rowprobe && g_in_rowpaint == 2) {
        static _Atomic int sbudget = 400;
        if (__atomic_fetch_sub(&sbudget, 1, __ATOMIC_RELAXED) > 0) {
            FILE *f = azlog_open("/tmp/azmod-rowpaint.log", "a");
            if (f) {
                fprintf(f, "  SUBST did=%d why=%d seq=%d grabbed=%d\n", did, why, g_rowpaint_seq,
                        grabbed);
                fclose(f);
            }
        }
    }
    g_rowpaint_seq++;
    if (g_rowprobe && g_in_rowpaint) {
        static _Atomic int budget = 40;
        if (__atomic_fetch_sub(&budget, 1, __ATOMIC_RELAXED) > 0) {
            FILE *f = azlog_open("/tmp/azmod-rowpaint.log", "a");
            if (f) {
                unsigned char b[8];
                int ok = 0;
                uintptr_t v = 0;
                if (own_q((uintptr_t) col, &v)) { /* the color EP147 passed in */
                    memcpy(b, &v, 8);
                    ok = 1;
                }
                fprintf(f, "  SETCOLOUR %s col=%lx", g_in_rowpaint == 2 ? "SELECTED" : "normal  ",
                        (unsigned long) (uintptr_t) col);
                if (ok)
                    for (int i = 0; i < 8; i++)
                        fprintf(f, " %02x", b[i]);
                fprintf(f, "\n");
                fclose(f);
            }
        }
    }
    g_orig_setcolour(g, col);
}
/* ==== Drag view ==========================================================
 * The drag is a view; the database is written once, at commit. A move per detent
 * would be unsafe: the writer trusts its `from` position, so one stale index
 * silently scrambles the playlist.
 *
 * With the row grabbed at data row `o` and held over display row `t`:
 *     display t            -> data o          (the held track)
 *     display r, o<=r<t    -> data r+1        (everything shifted up)
 *     display r, t<r<=o    -> data r-1        (everything shifted down)
 *     anything else        -> itself
 * EP147's own painters draw the mapped data row, so the list renders natively.
 * The permutation is dropped when fillPane runs: EP147 has re-read the data then,
 * and it would double-apply. */
static _Atomic int g_drag_on; /* 1 while a row is held */
/* Which list is dragged (tracks, playlists or folders). The permutation applies only to
 * that list's widget: painter 0x1bcfb78 is shared by the track and playlist lists, and
 * both can be on screen at once. */
#define DRAG_KIND_TRACK 0
static _Atomic int g_drag_kind;
static _Atomic unsigned long
    g_drag_widget;              /* the dragged list's widget; compared, re-validated at use */
static _Atomic int g_knob_kind; /* the kind the current press would grab */
/* Drop into a folder: while a playlist row is dragged, a step past a folder first stops
 * on the folder (amber); a press there moves the row inside it. Written by the bus thread
 * (knob), read by the painters. */
static _Atomic int g_drag_into = -1;      /* snapshot (data) row of the folder hovered, -1 = none */
static _Atomic int g_drag_into_disp = -1; /* its display row in the current preview */
static _Atomic int g_drag_into_dir;       /* the direction that reached it (+1 / -1) */
static _Atomic int g_drag_into_note; /* the hover changed: the message thread re-posts the banner */
static _Atomic int g_sel_row =
    -1; /* EP147's selected display row, per the row painter (stale after a touch) */
static _Atomic int g_sel_row_lb =
    -1; /* JUCE's own ListBox selection, refreshed by the message tick; -1 off screen */
static _Atomic int g_tl_showing;  /* the track list is on screen (tracklist_showing), same tick */
static _Atomic int g_drag_rows;   /* row count of the list being dragged */
static _Atomic int g_drag_origin; /* where it started, a DATA row */
static _Atomic int g_drag_target; /* where it is now, a display row */
static _Atomic int
    g_drag_committing; /* commit handed to the library thread; keep the preview, no amber */
static _Atomic int g_drag_want_update;  /* ask the message thread for ListBox::updateContent */
static _Atomic int g_drag_pin_row = -1; /* after the drag: where EP147's selection must end up */
/* The press, as the knob handler (mixer-bus thread) sees it; the GRAB itself is
 * decided on the message tick. See drag_try_grab(). */
static _Atomic int g_knob_held;  /* an eligible press is down right now */
static _Atomic int g_shift_down; /* either deck's SHIFT, from the last ErP frame with a valid CRC */
static _Atomic long long g_knob_press_ms;
static _Atomic long long g_drag_grab_ms; /* when the current drag was grabbed (drag_try_grab) */
static _Atomic int g_knob_raw;           /* the latest byte-34 knob position */
static _Atomic long long g_drag_pin_deadline;

static int drag_remap_any(int display_row);
/* Settle: the write completes ~0.3-0.5 s after the press and EP147 re-lists ~0.35-0.6 s
 * after that. The preview stays until the list itself holds the new order
 * (drag_data_new), so the old order never flashes in between; msg_tick ends it after 2 s
 * at most. */
/* MENU submenus: while the second row of NEW > / EDIT > shows, BACK returns to the parent
 * row instead of closing the panel. The bus thread swallows that BACK and raises
 * g_mp_subback; menu_popup.c's mp_anim_tick (message thread) acts on it and clears
 * g_mp_submode whenever the panel is gone, so BACK can never stay captured. */
static _Atomic int g_mp_submode, g_mp_subback;
static _Atomic int g_drag_settle;
static long long g_drag_settle_t0;
/* A press while a drop saves (0.8-2 s on the deck) or while the list catches up after it is
 * held for us rather than handed to EP147 (which would open stock quick scroll), and
 * drag_try_grab retries while the knob stays down. g_edit_done_ms: when the last edit's
 * result came back. */
static _Atomic long long g_edit_done_ms;
#define DRAG_RETRY_MS 1500
static int drag_data_new(void);
static int drag_remap(int display_row) {
    if (!__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return display_row;
    if (__atomic_load_n(&g_drag_settle, __ATOMIC_ACQUIRE) && drag_data_new())
        return display_row; /* already the new order */
    return drag_remap_any(display_row);
}
/* is `w` the list being dragged right now */
static int drag_is_widget(uintptr_t w) {
    return __atomic_load_n(&g_drag_on, __ATOMIC_RELAXED) && w &&
           __atomic_load_n(&g_drag_widget, __ATOMIC_RELAXED) == (unsigned long) w;
}
static int drag_remap_any(int display_row) {
    int o = __atomic_load_n(&g_drag_origin, __ATOMIC_RELAXED);
    int t = __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED);
    if (o == t)
        return display_row;
    if (display_row == t)
        return o;
    if (t > o && display_row >= o && display_row < t)
        return display_row + 1;
    if (t < o && display_row > t && display_row <= o)
        return display_row - 1;
    return display_row;
}
static void drag_clear(void) {
    __atomic_store_n(&g_drag_into, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_into_disp, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_on, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_committing, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_settle, 0, __ATOMIC_RELEASE);
}
/* juce is not thread-safe: the knob handler runs on the mixer-bus thread, so it
 * only asks; msg_tick() does ListBox::updateContent on the message thread. */
static void drag_request_update(void) {
    __atomic_store_n(&g_drag_want_update, 1, __ATOMIC_RELEASE);
}
/* After a drag, EP147 may apply the detents turned during it as one jump and move its
 * selection by (target - origin). Once that lands, ListBox::selectRow puts the selection
 * where it belongs: the origin after a cancel or refusal, the target after a written
 * move. */
static long long drag_now_ms(void);
#define DRAG_PIN_MS 300
/* A programmatic selectRow moves the highlight but not EP147's browse cursor, which LOAD
 * and the knob click act on; left unsynced, LOAD would load the neighbor of the row on
 * screen. Every turn during a drag is consumed at EP147's turn handler (knobturn_hook), so
 * the cursor stays on the grab row (g_cursor_row); the pin then moves it with EP147's own
 * handler by pin - g_cursor_row (0 after a cancel). -1 = no drag behind this pin (a
 * REMOVE): nothing to sync. */
static _Atomic int g_cursor_row = -1; /* where EP147's cursor is while a drag holds it */
static _Atomic int g_cursor_sync;     /* a pin is waiting to move EP147's cursor */
static void knob_cursor_to(int row);  /* with knobturn_hook */
static void drag_pin(int row) {
    __atomic_store_n(&g_drag_pin_deadline, drag_now_ms() + DRAG_PIN_MS, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_pin_row, row, __ATOMIC_RELEASE);
    if (__atomic_load_n(&g_cursor_row, __ATOMIC_RELAXED) >= 0)
        __atomic_store_n(&g_cursor_sync, 1, __ATOMIC_RELEASE);
}
static void drag_update_content(void); /* defined with the widget capture below */
static int drag_num_rows(void);
/* the PLAYLIST list (HierarchyListWidget), defined with its hooks below */
static _Atomic unsigned long
    g_hl_widget; /* the one hierarchy list on screen (msg tick), 0 = none or ambiguous */
static _Atomic int g_hl_rows,
    g_hl_sel = -1; /* its row count and JUCE selection (msg tick copies for the bus thread) */
/* Selected playlist's row mark. A row record carries no playlist id (only row index, kind
 * and name), so the row is recognized by what was on screen when it was selected: the
 * list's row count and the names of the row and both neighbors. Any mismatch = no mark
 * (unknown, never a wrong row). plist_tick recomputes the marked display row when the list
 * changes; the painter only compares two ints. */
static struct {
    int on;
    unsigned pl, media;
    int rows;
    char prev[64], name[64], next[64];
} g_mk;
static struct {
    int rows;
    char prev[64], name[64], next[64];
} g_mkr;                             /* MV_OP_MKREAD's answer */
static _Atomic unsigned long g_mk_w; /* the widget showing the marked row, 0 = none */
static _Atomic int g_mk_row = -1;
static _Atomic unsigned long
    g_hl_seen; /* the last hierarchy widget EP147 built a cell for (hier_refresh_hook) */
#define HIER_WIDGET_VT \
    EP_2b79058 /* gui::browse::HierarchyListWidget (also defined with its hooks) */
static int plist_widget_ok(uintptr_t w, uintptr_t *lb);
static int plist_rows(uintptr_t w);
static void plist_tick(void);
static int plist_snapshot_names(uintptr_t w, int rows, int must);
static int plist_refresh_names(uintptr_t w);

/* ---- Reading EP147's objects (ep_mem.h) ---------------------------------------------------
 * Every read of EP147 memory in this file is a plain load. It is safe because the object is
 * alive for the whole call, for one of these reasons, and never because a read "succeeded":
 *  - it is a hook's argument, or reached from one through EP147's own fields, during EP147's
 *    call (on whichever thread EP147 makes it);
 *  - it is a list widget azmod keeps a pointer to, and that pointer is in one of the two live
 *    sets below: widgets EP147 itself handed to a hook on the message thread, each slot cleared
 *    by juce::Component's destructor (wref) before the memory is reused;
 *  - it is a view kept in a registered wref slot (SearchView, BrowseView, the naming host, the
 *    lent keyboard and its home, the browse knob's view) and the slot still holds it.
 * Widgets and views belong to the message thread: anywhere else these read as unknown. */
static void pe_wref_init(void);
static int g_pe_wref_ok; /* the live sets and view slots are registered (pe_wref_init) */
#define PE_HLW_N 16
#define PE_TLW_N 8
static _Atomic uintptr_t g_pe_hlw[PE_HLW_N]; /* live gui::browse::HierarchyListWidgets */
static _Atomic uintptr_t g_pe_tlw[PE_TLW_N]; /* live gui::browse::TrackListWidgets */
/* Remember a widget EP147 is calling us about right now (so it is alive), message thread only:
 * the destructor that clears a slot runs there too, so no stale add can land after it. */
static void pe_set_add(_Atomic uintptr_t *set, int n, uintptr_t w, unsigned *rr) {
    if (!g_pe_wref_ok || !w || (w & 7) || !on_msg_thread())
        return;
    int free_i = -1;
    for (int i = 0; i < n; i++) {
        uintptr_t v = wref_get(&set[i]);
        if (v == w)
            return;
        if (!v && free_i < 0)
            free_i = i;
    }
    wref_set(&set[free_i >= 0 ? free_i : (int) ((*rr)++ % (unsigned) n)], w);
}
static int pe_set_has(_Atomic uintptr_t *set, int n, uintptr_t w) {
    if (!g_pe_wref_ok || !w || (w & 7) || !on_msg_thread())
        return 0;
    for (int i = 0; i < n; i++)
        if (wref_get(&set[i]) == w)
            return 1;
    return 0;
}
/* 1 = `w` is a live list widget of that class: plain loads through it are safe for this call */
static int pe_hier_live(uintptr_t w) {
    return pe_set_has(g_pe_hlw, PE_HLW_N, w);
}
static int pe_track_live(uintptr_t w) {
    return pe_set_has(g_pe_tlw, PE_TLW_N, w);
}
/* A view slot's value, or 0: only on the message thread and only once the slot is tracked. */
static uintptr_t pe_view(_Atomic uintptr_t *slot) {
    return g_pe_wref_ok && on_msg_thread() ? wref_get(slot) : 0;
}
/* Take a view slot's value (the slot reads 0 afterwards), same rules as pe_view. */
static uintptr_t pe_view_take(_Atomic uintptr_t *slot) {
    uintptr_t v = atomic_exchange_explicit(slot, 0, memory_order_acq_rel);
    return g_pe_wref_ok && on_msg_thread() ? v : 0;
}

/* ==== Reorder gesture =====================================================
 *   press and hold the browse knob   -> the selected row turns amber (GRAB)
 *   turn the knob                    -> the held row moves through the list
 *   CLICK                            -> commit
 *   BACK                             -> cancel, the row returns to where it was
 *
 * Switched by the Mod Menu's Playlist Editing toggle; off, the deck is stock.
 *
 * EP147 sees every press. Its own long press (ev 2, 500 ms) arms the grab instead of quick
 * scroll (knobev_hook), a short press is its own click, and only the drop press (one made
 * while a row is held) is swallowed. Timing the hold in azmod and swallowing presses
 * leaves EP147 holding the knob into quick scroll on the deck's event-driven bus.
 * DRAG_HOLD_MS is not a wait: knobev_hook back-dates the press by it so drag_try_grab's
 * check is already met, and drag_retry_ok's window counts from it. */
#define DRAG_HOLD_MS 300
static long long drag_now_ms(void);
static int drag_retry_ok(
    void) { /* still inside the catch-up window, from the press or the edit, whichever is later */
    long long p = __atomic_load_n(&g_knob_press_ms, __ATOMIC_RELAXED),
              e = __atomic_load_n(&g_edit_done_ms, __ATOMIC_RELAXED);
    return drag_now_ms() - (p > e ? p : e) < DRAG_HOLD_MS + DRAG_RETRY_MS;
}
static _Atomic int g_reorder_enabled;
static int g_reorder_forced_off; /* an install declined: never enable until EP147 restarts */
static int g_reorder_hooks;      /* every hook installed at start (flag was present) */
static long long drag_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void drag_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-drag.log", "a");
    if (!f)
        return;
    fprintf(f, "[%lld] ", drag_now_ms() % 1000000); /* ms, so ordering and gaps are readable */
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
/* ==== Reorder commit: EP147's own movePlaylistTrack =======================
 * Every piece is EP147's own; azmod only decides when, and refuses whenever it cannot
 * prove the answer.
 *
 *   Which track / playlist  EP147's browse hierarchy (the object 0x1e461f8 returns; the
 *                           stock TAG TRACK facade 0x1e43b70 reads the same fields).
 *                           Snapshotted as values on EP147's own lookup, never held as a
 *                           pointer.
 *   The write               music_library::InformationUpdater::movePlaylistTrack 0x1272830
 *                           (validate, lock, SqliteDataBase -> SqlitePlaylist ->
 *                           SqlitePlaylistUpdater move -> removePlaylistTrackListCache ->
 *                           notify). Compiled in but unreferenced on the AZ (its only
 *                           caller, request handler 0x12b2058, has no callers).
 *   Where it runs           EP147's library thread, at a message boundary:
 *                           MusicLibraryServer::onMessage (vtable slot +0x50) runs us
 *                           before the message is dispatched, so no EP147 transaction is
 *                           open. Checked anyway: sqlite3_get_autocommit == 1.
 *   What wakes that thread  PlaylistRenewalNoticeHandler::onPlaylistRenewed() (the PRO DJ
 *                           LINK "playlists changed" notice), which posts a library message
 *                           that re-reads the playlist table: harmless and native.
 *   The refresh             native: movePlaylistTrack invalidates the track-list cache and
 *                           EP147 re-queries and redraws, its selection following the
 *                           moved track.
 *
 * Hazard: EP147's move keys the moved row on `sequenceNo = from` alone. A wrong `from`
 * moves the wrong track, and a duplicated sequenceNo moves two. So right before the write,
 * on the same connection, the DB must show the playlist is 1..N contiguous with no
 * duplicates and the row at `from` is the grabbed track. Otherwise nothing is written and
 * the list shows its real order again. */

/* ---- 1. what is on screen: a value snapshot of EP147's browse hierarchy --- */
#define LISTSTATE_FIND EP_1e461f8 /* returns &shared_ptr<list state> */
#define LS_CATEGORY_Q 80          /* u16 at +80: 5 PLAYLIST, 22 HISTORY, 2 ARTIST */
#define LS_CURSOR_Q 96            /* u32 at +100 (high half): active level */
#define LS_LEVELS 104             /* [begin, end) of 0x28-byte levels */
#define LS_LEVEL_STRIDE 0x28
#define LS_CATEGORY_PLAYLIST 5
#define LS_CATEGORY_FOLDER 17 /* the Browse FOLDER view: the stick's file system */
#define REC_KIND_FOLDER_ENTRY 1 /* a folder row in the FOLDER view (a file row is kind 4) */
#define LV_SORT_Q 8   /* u32 +8 SortKind (0 = "#"), u8 +12 ListOrder (1 = desc) */
#define LV_REC 24     /* the level's selected record */
#define REC_ROW_Q 0   /* u32 at +4: display row of the selection */
#define REC_KIND_Q 16 /* u8 +16 kind (4 track, 8 playlist, 36 history), u32 +20 id */
#define REC_KIND_TRACK 4
#define REC_KIND_PLAYLIST 8
#define REC_KIND_FOLDER \
    1 /* a playlist FOLDER row, id = its playlist_id */


/* Which stick. EP147 reaches a list state in two lookups: 0x1e46af0(map, &MediaID) returns
 * the per-media object, and 0x1e461f8(obj + 0x38, key) the list state inside it. We record
 * which object EP147's map returned for which MediaID; the second lookup hands us obj +
 * 0x38, and the media follows by pointer equality with an entry EP147 itself produced
 * (never dereferenced, never inferred from call order). MediaID: u8 valid at +0, and at +4
 * the u16 SqliteDataBase also carries at +8 (e.g. 0x0102 for USB1). */
#define MEDIAOBJ_FIND EP_1e46af0
#define MEDIAOBJ_LSMAP 0x38
#define MEDIATAB_N 4
static _Atomic unsigned long g_mediatab_obj[MEDIATAB_N];
static _Atomic unsigned g_mediatab_id[MEDIATAB_N];
static void **(*g_orig_mediaobj_find)(void *, void *);
static void **mediaobj_find_hook(void *map, void *mid) {
    void **slot = g_orig_mediaobj_find(map, mid);
    uintptr_t obj = 0, m = 0;
    /* both are EP147's: its return value and its argument, during its own call */
    if (slot && own_q((uintptr_t) slot, &obj) && obj && own_q((uintptr_t) mid, &m) &&
        (m & 0xff)) {
        unsigned id = (unsigned) ((m >> 32) & 0xffff);
        int free_i = -1;
        for (int i = 0; i < MEDIATAB_N; i++) {
            if (__atomic_load_n(&g_mediatab_obj[i], __ATOMIC_ACQUIRE) == obj) {
                __atomic_store_n(&g_mediatab_id[i], id, __ATOMIC_RELEASE);
                return slot;
            }
            if (free_i < 0 && !__atomic_load_n(&g_mediatab_obj[i], __ATOMIC_RELAXED))
                free_i = i;
        }
        static unsigned rr;
        int i = free_i >= 0 ? free_i : (int) (rr++ % MEDIATAB_N);
        __atomic_store_n(&g_mediatab_obj[i], 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_mediatab_id[i], id, __ATOMIC_RELEASE);
        __atomic_store_n(&g_mediatab_obj[i], (unsigned long) obj, __ATOMIC_RELEASE);
    }
    return slot;
}
static unsigned media_of_lsmap(uintptr_t lsmap) {
    uintptr_t obj = lsmap - MEDIAOBJ_LSMAP;
    for (int i = 0; i < MEDIATAB_N; i++)
        if (__atomic_load_n(&g_mediatab_obj[i], __ATOMIC_ACQUIRE) == obj)
            return __atomic_load_n(&g_mediatab_id[i], __ATOMIC_ACQUIRE);
    return 0; /* unknown: not eligible */
}
static pthread_mutex_t g_plsnap_mu = PTHREAD_MUTEX_INITIALIZER;
static struct plsnap g_plsnap;
static void **(*g_orig_lsfind)(void *, void *);

/* Eligible only for a USB playlist's track list in "#" ascending order: the one
 * view in which the row a DJ sees is the playlist position. Anything else
 * (history, tag list, artist, a BPM sort, "#" descending) is left stock. */
static void plsnap_take(uintptr_t ls, unsigned media) {
    /* lsfind_hook's caller: `ls` is the list state EP147's own lookup is returning, read
     * before that call returns. */
    struct plsnap s;
    memset(&s, 0, sizeof s);
    uintptr_t cat = 0, cur = 0, b = 0, e = 0;
    /* Any category: what the active level's selected record is. A track (kind 4) carries its
     * content_id at +20; a playlist row (kind 8) on the PLAYLIST list carries its
     * playlist_id. */
    if (media && own_q(ls + LS_CATEGORY_Q, &cat) && own_q(ls + LS_CURSOR_Q, &cur) &&
        own_q(ls + LS_LEVELS, &b) && own_q(ls + LS_LEVELS + 8, &e) && b && e > b) {
        long n = (long) ((e - b) / LS_LEVEL_STRIDE);
        unsigned c = (unsigned) (cur >> 32);
        uintptr_t lrec = 0, lk = 0, row = 0;
        if ((long) c < n && n < 64 && own_q(b + (uintptr_t) c * LS_LEVEL_STRIDE + LV_REC, &lrec) &&
            lrec && own_q(lrec + REC_KIND_Q, &lk) && own_q(lrec + REC_ROW_Q, &row) &&
            (lk >> 32)) {
            if ((lk & 0xff) == REC_KIND_TRACK && (cat & 0xffff) != LS_CATEGORY_FOLDER) {
                /* (in the FOLDER view a file row's id is a folder entry, not a content id) */
                s.any_cid = (unsigned) (lk >> 32);
                s.any_row = (unsigned) (row >> 32);
                s.any_media = media;
            } else if (((lk & 0xff) == REC_KIND_PLAYLIST || (lk & 0xff) == REC_KIND_FOLDER) &&
                       (cat & 0xffff) == LS_CATEGORY_PLAYLIST) {
                s.sel_pl = (unsigned) (lk >> 32);
                s.any_media = media;
                s.sel_folder = (lk & 0xff) == REC_KIND_FOLDER;
                s.sel_row = (unsigned) (row >> 32);
            }
            /* the row's displayed name: a juce::String at +0x18 (the MENU title) */
            {
                uintptr_t sp = 0;
                if (own_q(lrec + 0x18, &sp) && sp)
                    own_str(sp, s.sel_title, sizeof s.sel_title);
            }
        }
    }
    /* the Browse FOLDER view: the selected entry's path from the level names, 0..cursor (its
     * stick's own file names; folder_copy.h checks each before it is joined) */
    cat = cur = b = e = 0;
    if (media && own_q(ls + LS_CATEGORY_Q, &cat) && (cat & 0xffff) == LS_CATEGORY_FOLDER &&
        own_q(ls + LS_CURSOR_Q, &cur) && own_q(ls + LS_LEVELS, &b) &&
        own_q(ls + LS_LEVELS + 8, &e) && b && e > b) {
        long n = (long) ((e - b) / LS_LEVEL_STRIDE);
        int c = (int) (unsigned) (cur >> 32);
        static char nm[FC_MAX_DEPTH + 1][FC_NAME_MAX + 1]; /* this hook's thread only */
        const char *names[FC_MAX_DEPTH + 1];
        int k = 0, kind = 0;
        unsigned ids[FC_MAX_DEPTH + 1];
        for (int i = 0; i <= c && i < n && n < 64 && i <= FC_MAX_DEPTH; i++) {
            uintptr_t r = 0, kq = 0, sp = 0;
            if (!own_q(b + (uintptr_t) i * LS_LEVEL_STRIDE + LV_REC, &r) || !r ||
                !own_q(r + REC_KIND_Q, &kq) || !own_q(r + 0x18, &sp) || !sp)
                break;
            own_str(sp, nm[i], sizeof nm[i]);
            ids[k] = (unsigned) (kq >> 32);
            names[k++] = nm[i];
            kind = (int) (kq & 0xff);
        }
        if (k >= 1 && k == c + 1 && (kind == REC_KIND_FOLDER_ENTRY || kind == REC_KIND_TRACK) &&
            !fc_rel_from_levels(names, k, s.fsel_path, sizeof s.fsel_path) &&
            (kind == REC_KIND_FOLDER_ENTRY
                 ? !fc_rel_from_levels(names, k, s.fsel_dir, sizeof s.fsel_dir)
                 : (k == 1 ? (s.fsel_dir[0] = 0, 1)
                           : !fc_rel_from_levels(names, k - 1, s.fsel_dir, sizeof s.fsel_dir)))) {
            s.fsel_media = media;
            s.fsel_kind = kind == REC_KIND_FOLDER_ENTRY ? 1 : 4;
            unsigned dir_id = 0; /* the folder the selection is in: itself for a folder entry */
            if (kind == REC_KIND_FOLDER_ENTRY)
                dir_id = ids[k - 1];
            else if (k >= 2)
                dir_id = ids[k - 2];
            s.fsel_dir_id = dir_id;
        } else
            s.fsel_path[0] = s.fsel_dir[0] = 0;
    }
    cat = cur = b = e = 0;
    if (own_q(ls + LS_CATEGORY_Q, &cat) && (cat & 0xffff) == LS_CATEGORY_PLAYLIST &&
        own_q(ls + LS_CURSOR_Q, &cur) && own_q(ls + LS_LEVELS, &b) &&
        own_q(ls + LS_LEVELS + 8, &e) && b && e > b) {
        long n = (long) ((e - b) / LS_LEVEL_STRIDE);
        unsigned c = (unsigned) (cur >> 32);
        uintptr_t leaf = b + (uintptr_t) c * LS_LEVEL_STRIDE, parent = leaf - LS_LEVEL_STRIDE;
        uintptr_t sort = 0, lrec = 0, prec = 0, lk = 0, pk = 0, row = 0;
        if (c >= 1 && (long) c < n && n < 64 && own_q(leaf + LV_SORT_Q, &sort) &&
            own_q(leaf + LV_REC, &lrec) && lrec && own_q(parent + LV_REC, &prec) && prec &&
            own_q(lrec + REC_KIND_Q, &lk) && own_q(prec + REC_KIND_Q, &pk) &&
            own_q(lrec + REC_ROW_Q, &row) && (lk & 0xff) == REC_KIND_TRACK &&
            (pk & 0xff) == REC_KIND_PLAYLIST) {
            if (media)
                s.open_pl = (unsigned) (pk >> 32);
            if ((unsigned) (sort & 0xffffffff) == 0 && ((sort >> 32) & 0xff) == 0) {
                s.playlist_id = (unsigned) (pk >> 32);
                s.content_id = (unsigned) (lk >> 32);
                s.row = (unsigned) (row >> 32);
                s.media = media;
                if (!s.content_id || !s.media)
                    s.playlist_id = 0;
            } else if (media) {
                s.sort_block = 1; /* only to say why; never makes a row eligible */
            }
        }
    }
    pthread_mutex_lock(&g_plsnap_mu);
    g_plsnap = s;
    pthread_mutex_unlock(&g_plsnap_mu);
}
static struct plsnap plsnap_get(void) {
    pthread_mutex_lock(&g_plsnap_mu);
    struct plsnap s = g_plsnap;
    pthread_mutex_unlock(&g_plsnap_mu);
    return s;
}
static _Atomic long long g_lsfind_ms; /* telemetry: last ListState lookup */
static void **lsfind_hook(void *map, void *key) {
    void **slot = g_orig_lsfind(map, key);
    __atomic_store_n(&g_lsfind_ms, drag_now_ms(), __ATOMIC_RELAXED);
    uintptr_t ls = 0;
    if (slot && own_q((uintptr_t) slot, &ls) && ls) /* EP147's return value, before it returns */
        plsnap_take(ls, media_of_lsmap((uintptr_t) map));
    return slot;
}

/* ---- 2. the move request, handed from the message thread to the library thread ------
 * One slot, a state machine and a deadline. The message thread fills and arms it (drag_commit,
 * on the tick after the bus read thread recorded the knob drop: drag_handoff.h);
 * the library thread takes it at its next message boundary; the message thread consumes
 * the result or, if nothing took it in time, expires it. Every transition is a CAS, so
 * exactly one party wins and a request never runs late against a changed list. */
enum {
    MV_IDLE,
    MV_ARMED,
    MV_TAKEN,
    MV_DONE
};
#define MV_DEADLINE_MS 2500
static _Atomic int g_mv_state = MV_IDLE;
static _Atomic int g_mv_kick; /* message thread: post the renewal */
/* REMOVE on a folder (stock disables DELETE on folders): count what is inside (library
 * thread), confirm with EP147's own OK / CANCEL box, then delete the whole tree in one
 * transaction inside the undo journal (UNDO restores it). */
#define FD_MAX 500
static struct {
    unsigned media, folder;
    char name[256];
    uintptr_t ctrl;
    int nlists, nfolders, n;
    long long ids[FD_MAX];
    unsigned char isfolder[FD_MAX];
} g_fd;
static unsigned g_tag_cids[TAG_MAX];
static int g_tag_n, g_tag_skipped, g_tag_want, g_tag_added;
static struct {
    int op;
    unsigned pl, cid, from, to, media;
    long long deadline;
    int rc;
    char name[96];
    long long n;
    unsigned parent;
    char pname[96]; /* MV_OP_TARGET: the row's parent folder (0 = top) + its name */
    char mount[128];
} g_mv; /* mount: the stick's /media/usb/<dev>, from EP147's own DB path */

/* ---- 3. the wake-up: PlaylistRenewalNoticeHandler, from its constructor ---- */
#define PLRENEW_CTOR EP_9d4ac0  /* stores vptr 0x26a3f60 */
#define PLRENEW_VPTR EP_26a3f60 /* slot +0x10 = onPlaylistRenewed() 0x9d1ae8 */
static _Atomic unsigned long g_plrenew_obj;
static void *(*g_orig_plrenew_ctor)(void *, void *, void *, void *);
/* The handler is held by a shared_ptr whose release deletes it through vtable slot +8 (the
 * deleting destructor). That slot is hooked (install_reorder) to forget the handler before it
 * is freed; without that hook g_plrenew_obj is never used. */
static void (*g_orig_plrenew_del)(void *);
static int g_plrenew_del_ok;
static void plrenew_del_hook(void *self) {
    unsigned long exp = (unsigned long) (uintptr_t) self;
    __atomic_compare_exchange_n(&g_plrenew_obj, &exp, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    g_orig_plrenew_del(self);
}
static void *plrenew_ctor_hook(void *self, void *a1, void *a2, void *a3) {
    void *r = g_orig_plrenew_ctor(self, a1, a2, a3);
    __atomic_store_n(&g_plrenew_obj, (unsigned long) (uintptr_t) self, __ATOMIC_RELEASE);
    return r;
}
/* Message thread only. The handler is alive while g_plrenew_obj holds it (plrenew_del_hook);
 * the vtable is still checked at use. */
static int plrenew_kick(void) {
    unsigned long o = __atomic_load_n(&g_plrenew_obj, __ATOMIC_ACQUIRE);
    uintptr_t vt = 0;
    if (!g_plrenew_del_ok || !o || (o & 7) || !own_q(o, &vt) || vt != PLRENEW_VPTR) {
        __atomic_store_n(&g_plrenew_obj, 0, __ATOMIC_RELAXED);
        return 0;
    }
    ((void (*)(void *))(*(void ***) (uintptr_t) o)[2])((void *) (uintptr_t) o);
    return 1;
}

/* ---- 4. the write, on the library thread ---------------------------------- */
#define MLSERVER_VPTR EP_285f078 /* music_library::MusicLibraryServer */
#define MLSERVER_ONMSG_SLOT (MLSERVER_VPTR + 0x50)
#define MLSERVER_DISP \
    ep_off(448, 456)            /* onMessage: x0 = [server+448]; b 0x12acc28 (1.30)                \
                                 * 2.00: ldr x0,[x0,#456] at 0x1370cf0 (the class is 8 bytes     \
                                 * larger; offset 448 reads 0 there) */
#define MLDISP_HANDLER 32       /* the dispatcher calls handlers with [disp+32] */
#define MLHANDLER_INFOUPD 48    /* ...which reach the service at [handler+48] */
#define INFOUPD_VPTR EP_2857330 /* music_library::InformationUpdater */
#define INFOUPD_LIB 8           /* [svc+8] = library; [lib+48] = SqliteDataBase */
#define MLLIB_SQLDB 48
#define SQLDB_VPTR EP_284b2b8 /* music_library::SqliteDataBase */
#define SQLDB_MEDIA_Q 8       /* u16 media id, same as TrackID bytes 4..5 */
#define PL_MOVE_TRACK EP_1272830 /* InformationUpdater::movePlaylistTrack */
/* InformationUpdater::removePlaylistTrack(svc, playlist_id, content_id, seq) 0x12671d0:
 * validate, library lock, disk check, SqlitePlaylist -> updater slot +0x40 (remove, then
 * renumber the rest down), removePlaylistTrackListCache, notify [svc+0x38]. Returns 0. Like
 * the move, its request handler is never reached on the AZ. */
#define PL_REMOVE_TRACK EP_12671d0
/* ADD has no InformationUpdater wrapper; the device updater's add slot is called only by
 * Tag List -> CREATE PLAYLIST (0x126b600, from 0x1271070). We follow that code: accessor
 * chain [lib+0x30] vt+0x20 -> vt+0x58 = SqlitePlaylistUpdater (vt 0x284ab30), row keys from
 * 0x126eab0 (a vector of 16-byte juce::Uuid, checked unique against the DB), then slot
 * +0x18(upd, &key, playlist_id, content_id, sequenceNo) and the per-playlist cache drop. */
#define PL_KEYS_FN EP_126eab0 /* vector<Uuid> (svc, n): result via x8 */
#define PL_UUID_DTOR EP_230b460
#define PLUPD_ADD_SLOT 0x18
#define PL_ATTR_LIST 0 /* playlist.attribute of a plain list (logged, and refused otherwise) */
static void (*g_orig_mlserver_onmsg)(void *, void *, unsigned);

/* One read-only row of up to 4 integers. Returns the number of columns read
 * (0 = no row / error). Two bound parameters, both integers. */
#define SQLITE_ROW 100

#define PL_TGT_N 4
static char g_plo_names[PLO_MAXROWS][PL_TGT_NAME_MAX];
static int g_plo_n;
static unsigned char
    g_plo_folder[PLO_MAXROWS]; /* snapshot row is a FOLDER (row record +0x10 == 1; 0x0f = a list) */
/* EP147 keeps a record for every row but fills a row's name only once it has loaded that
 * row (off-screen rows can have an empty name). Known = its name was seen; only known rows
 * are checked or offered as a folder. */
static unsigned char g_plo_have[PLO_MAXROWS];
/* ==== NEW PLAYLIST in Browse, named with EP147's own keyboard ==============
 * EP147 has no named create (its only create is the Tag List conversion, auto-named
 * "TAG LIST 001"), and its keyboard is not a dialog but a keystroke source
 * (gui::SoftwareKeyboardWidget -> controller -> listeners). The Search keyboard widget is
 * [[SearchView+0x40]+0] (vt 0x2c29228); its controller (vt 0x2c29088) has one listener,
 * SearchView's own sub-object (vt 0x29db4c8: key 0x19bcf68, backspace 0x19bcf80).
 *   1. MENU on a playlist-list row > NEW PLAYLIST: the keyboard is moved into the view on
 *      screen (juce addAndMakeVisible), so the DJ stays where they are;
 *   2. SearchView's two listener slots are intercepted: while naming, keys build azmod's
 *      name (nothing is searched, the search field is untouched);
 *   3. the overlay replaces the title bar with the name, CANCEL and CREATE; hardware BACK
 *      cancels (swallowed, so Browse does not go up a level);
 *   4. CREATE writes the row on the same stick's library thread through EP147's own
 *      connection (id = max(16384, max id) + 1, next sibling sequenceNo, image_id 0),
 *      reads it back and drops the caches.
 * The keyboard goes home to SearchView on create, cancel, leaving Browse, and before
 * Search can open. */
static _Atomic unsigned long g_nm_kb; /* the keyboard while it is lent to BrowseView */
static _Atomic int g_nm_create_req, g_nm_cancel_req, g_nm_dirty;
#define SKW_VT EP_2c29228    /* gui::SoftwareKeyboardWidget */
#define SV_KBL_VT EP_29db4c8 /* SearchView's ISoftwareKeyboard::IListener sub-object */
#define SV_KBL_KEY_ORIG EP_19bcf68
#define SV_KBL_BS_ORIG EP_19bcf80
#define BP_KBL_VT \
    EP_2a19970 /* the streaming-login widget controller's keyboard listener (the keyboard          \
                * that lives in BrowseView from start-up) */
#define BP_KBL_KEY_ORIG EP_1a29670
#define BP_KBL_BS_ORIG EP_1a29830
#define JUCE_ADD_VISIBLE EP_249d6b0 /* juce::Component::addAndMakeVisible(child, zOrder) */
static void (*g_orig_sv_key)(void *, void *);
static void (*g_orig_sv_bs)(void *, int);
static void (*g_orig_bp_key)(void *, void *);
static void (*g_orig_bp_bs)(void *, int);
static _Atomic unsigned long g_nm_kb_parent; /* where the lent keyboard came from */
/* message thread: the Search keyboard, vtable-checked, and it is SearchView's */
static uintptr_t nm_search_kb(uintptr_t sv) { /* sv: a live view (pe_view) */
    uintptr_t h = 0, kb = 0, vt = 0, par = 0;
    if (!sv || (sv & 7) || !own_q(sv, &vt) || vt != SEARCHVIEW_VT)
        return 0;
    if (!own_q(sv + 0x40, &h) || !h || !own_q(h, &kb) || !kb || (kb & 7) || !own_q(kb, &vt) ||
        vt != SKW_VT)
        return 0;
    if (!own_q(kb + 0x18, &par))
        return 0;
    return kb;
}
static void nm_kb_home(void) { /* message thread: back to where it was lent from */
    uintptr_t kb = pe_view_take(&g_nm_kb); /* both still alive: wref slots */
    uintptr_t par = pe_view_take(&g_nm_kb_parent);
    uintptr_t vt = 0, pvt = 0;
    if (!kb || !par || !own_q(kb, &vt) || vt != SKW_VT || !own_q(par, &pvt) || !pvt)
        return;
    ((void (*)(uintptr_t, uintptr_t, int)) JUCE_ADD_VISIBLE)(par, kb, -1);
    drag_log("NEWPL: keyboard %lx back in %lx (vt %lx)\n", kb, par, (unsigned long) pvt);
}
static _Atomic unsigned long g_nm_host; /* the Browse/Playlist view naming is shown in (nl_begin) */
static void nm_end(const char *why) {
    nm_kb_home();
    __atomic_store_n(&g_nm.state, NM_IDLE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_nm_create_req, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_nm_cancel_req, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_nm_dirty, 1, __ATOMIC_RELEASE);
    wref_set(&g_nm_host, 0);
    drag_log("NEWPL: %s\n", why);
}
static void nm_view_changed(int search_shown) { /* setvis_hook, message thread */
    if (__atomic_load_n(&g_nm.state, __ATOMIC_ACQUIRE) != NM_NAMING)
        return;
    nm_end(search_shown ? "Search opened: canceled" : "left Browse: canceled");
}
/* SearchView's listener slots: while naming, the keys are the playlist name */
static void nm_key(void *str) {
    uintptr_t p = 0;
    char k[16] = "";
    if (str && own_q((uintptr_t) str, &p) && p > 0x10000) /* the keystroke EP147 passes in */
        own_str(p, k, sizeof k);
    size_t n = strlen(g_nm.name), m = strlen(k);
    if (m && n + m < 64 && (unsigned char) k[0] >= 0x20) {
        memcpy(g_nm.name + n, k, m + 1);
        __atomic_store_n(&g_nm_dirty, 1, __ATOMIC_RELEASE);
    }
}
/* the listener's slot 1: 0 = backspace (the login keyboard's runs TextEditor::deleteBackwards),
 * 1 = the keyboard's second action (Search's +0x78): clear */
static void nm_action(int a) {
    if (a == 1) {
        g_nm.name[0] = 0;
        __atomic_store_n(&g_nm_dirty, 1, __ATOMIC_RELEASE);
        return;
    }
    if (a != 0)
        return;
    size_t n = strlen(g_nm.name);
    while (n && ((unsigned char) g_nm.name[n - 1] & 0xc0) == 0x80)
        n--; /* whole UTF-8 characters */
    if (n)
        n--;
    g_nm.name[n] = 0;
    __atomic_store_n(&g_nm_dirty, 1, __ATOMIC_RELEASE);
}
static int nm_naming(void) {
    return __atomic_load_n(&g_nm.state, __ATOMIC_ACQUIRE) == NM_NAMING;
}
static void sv_key_hook(void *self, void *str) {
    if (nm_naming())
        nm_key(str);
    else
        g_orig_sv_key(self, str);
}
static void sv_bs_hook(void *self, int a) {
    if (nm_naming())
        nm_action(a);
    else
        g_orig_sv_bs(self, a);
}
static void bp_key_hook(void *self, void *str) {
    if (nm_naming())
        nm_key(str);
    else
        g_orig_bp_key(self, str);
}
static void bp_bs_hook(void *self, int a) {
    if (nm_naming())
        nm_action(a);
    else
        g_orig_bp_bs(self, a);
}
static int g_nm_hooked;
static void install_nm_hooks(void) { /* with Playlist Editing (install_reorder) */
    uintptr_t a = 0, b = 0;
    int k = -9, bs = -9;
    if (img_q(SV_KBL_VT, &a) && a == SV_KBL_KEY_ORIG && img_q(SV_KBL_VT + 8, &b) &&
        b == SV_KBL_BS_ORIG) {
        k = azm_patch_vtable(SV_KBL_VT, (void *) sv_key_hook, (void **) &g_orig_sv_key);
        if (k == 1)
            bs = azm_patch_vtable(SV_KBL_VT + 8, (void *) sv_bs_hook, (void **) &g_orig_sv_bs);
    }
    int bk = -9, bb = -9;
    if (img_q(BP_KBL_VT, &a) && a == BP_KBL_KEY_ORIG && img_q(BP_KBL_VT + 8, &b) &&
        b == BP_KBL_BS_ORIG) {
        bk = azm_patch_vtable(BP_KBL_VT, (void *) bp_key_hook, (void **) &g_orig_bp_key);
        if (bk == 1)
            bb = azm_patch_vtable(BP_KBL_VT + 8, (void *) bp_bs_hook, (void **) &g_orig_bp_bs);
    }
    g_nm_hooked =
        (k == 1 && bs == 1) |
        ((bk == 1 && bb == 1) << 1); /* bit 0 Search's keyboard, bit 1 the login keyboard */
    drag_log("NEWPL: keyboard listener hooks search=%d,%d loginkbd=%d,%d -> %d\n", k, bs, bk, bb,
             g_nm_hooked);
}
/* library thread, the stick's own connection: the row, EP147's way */
static int nl_create(uintptr_t conn, uintptr_t cache, FILE *lg) {
    static int (*bind_text)(void *, int, const char *, int, void *);
    if (!bind_text)
        *(void **) &bind_text = dlsym(RTLD_DEFAULT, "sqlite3_bind_text");
    if (!bind_text || !cache)
        return -40;
    long long par[1] = {0}, nid[1] = {0}, seq[1] = {0};
    if (g_nm.ref_pl &&
        sq_row((void *) conn, "SELECT playlist_id_parent FROM playlist WHERE playlist_id = ?1",
               g_nm.ref_pl, -1, par, 1) != 1)
        return -20;
    if (sq_row((void *) conn, "SELECT MAX(16384, COALESCE(MAX(playlist_id), 0)) + 1 FROM playlist",
               0, -1, nid, 1) != 1)
        return -41;
    if (sq_row(
            (void *) conn,
            "SELECT COALESCE(MAX(sequenceNo), 0) + 1 FROM playlist WHERE playlist_id_parent = ?1",
            par[0], -1, seq, 1) != 1)
        return -41;
    {
        void *st0 = 0;
        long long dup = 0; /* rekordbox refuses a taken name: so does NEW */
        if (g_sq.prepare(
                (void *) conn,
                "SELECT COUNT(*) FROM playlist WHERE playlist_id_parent = ?1 AND name = ?2", -1,
                &st0, 0) != 0 ||
            !st0)
            return -42;
        g_sq.bind_int64(st0, 1, par[0]);
        bind_text(st0, 2, g_nm.name, -1, (void *) -1);
        if (g_sq.step(st0) == 100)
            dup = g_sq.column_int64(st0, 0);
        g_sq.finalize(st0);
        if (dup)
            return -61;
    }
    jr_scope_parent((void *) conn, par[0]); /* undo: the list it is created in */
    void *st = 0;
    if (g_sq.prepare((void *) conn,
                     "INSERT INTO playlist (playlist_id, sequenceNo, name, image_id, attribute, "
                     "playlist_id_parent) "
                     "VALUES (?1, ?2, ?3, 0, ?5, ?4)",
                     -1, &st, 0) != 0 ||
        !st)
        return -42;
    g_sq.bind_int64(st, 1, nid[0]);
    g_sq.bind_int64(st, 2, seq[0]);
    bind_text(st, 3, g_nm.name, -1, (void *) -1 /* SQLITE_TRANSIENT */);
    g_sq.bind_int64(st, 4, par[0]);
    g_sq.bind_int64(st, 5, g_nm.folder ? PL_ATTR_FOLDER : PL_ATTR_LIST);
    int rc = g_sq.step(st);
    g_sq.finalize(st);
    if (rc != 101 /* SQLITE_DONE */) {
        if (lg)
            fprintf(lg, "NEWPL insert failed rc=%d\n", rc);
        return -43;
    }
    char back[256] = "";
    if (!sq_text((void *) conn, "SELECT name FROM playlist WHERE playlist_id = ?1", nid[0], back,
                 sizeof back) ||
        strcmp(back, g_nm.name)) {
        if (lg)
            fprintf(lg, "NEWPL read-back mismatch\n");
        return -44;
    }
    if (lg)
        fprintf(lg, "NEWPL %lld \"%s\" under %lld (seq %lld)%s\n", nid[0], g_nm.name, par[0],
                seq[0], g_nm.folder ? " FOLDER" : "");
    lib_invalidate(cache, (void *) conn, lg);
    return 0;
}
/* RENAME, library thread: the row must still carry the name it had when RENAME was pressed
 * (so it is the row the DJ meant), and no sibling may already use the new name. One UPDATE,
 * read back, inside the undo journal (which restores whole rows, name included). */
static int nl_rename(uintptr_t conn, uintptr_t cache, FILE *lg) {
    static int (*bind_text)(void *, int, const char *, int, void *);
    if (!bind_text)
        *(void **) &bind_text = dlsym(RTLD_DEFAULT, "sqlite3_bind_text");
    if (!bind_text || !cache)
        return -40;
    void *c = (void *) conn, *st = 0;
    long long par[1] = {0};
    char cur[256] = "";
    if (sq_row(c, "SELECT playlist_id_parent FROM playlist WHERE playlist_id = ?1", g_nm.rename_pl,
               -1, par, 1) != 1 ||
        !sq_text(c, "SELECT name FROM playlist WHERE playlist_id = ?1", g_nm.rename_pl, cur,
                 sizeof cur))
        return -20;
    if (strcmp(cur, g_nm.oldname))
        return -26; /* not the row the DJ picked any more */
    long long dup = 0;
    if (g_sq.prepare(c,
                     "SELECT COUNT(*) FROM playlist WHERE playlist_id_parent = ?1 AND name = ?2 "
                     "AND playlist_id <> ?3",
                     -1, &st, 0) != 0 ||
        !st)
        return -42;
    g_sq.bind_int64(st, 1, par[0]);
    bind_text(st, 2, g_mv.name, -1, (void *) -1);
    g_sq.bind_int64(st, 3, g_nm.rename_pl);
    if (g_sq.step(st) == 100)
        dup = g_sq.column_int64(st, 0);
    g_sq.finalize(st);
    st = 0;
    if (dup)
        return -61; /* "Name already used there" */
    jr_scope_parent(c, par[0]);
    if (g_sq.prepare(c, "UPDATE playlist SET name = ?1 WHERE playlist_id = ?2", -1, &st, 0) != 0 ||
        !st)
        return -42;
    bind_text(st, 1, g_mv.name, -1, (void *) -1);
    g_sq.bind_int64(st, 2, g_nm.rename_pl);
    int rc = g_sq.step(st);
    g_sq.finalize(st);
    if (rc != 101)
        return -43;
    char back[256] = "";
    if (!sq_text(c, "SELECT name FROM playlist WHERE playlist_id = ?1", g_nm.rename_pl, back,
                 sizeof back) ||
        strcmp(back, g_mv.name))
        return -44;
    if (lg)
        fprintf(lg, "RENAME %u \"%s\" -> \"%s\" (parent %lld)\n", g_nm.rename_pl, g_nm.oldname,
                back, par[0]);
    lib_invalidate(cache, c, lg);
    return 0;
}
/* Folder REMOVE, library thread: the folder must still be a folder with the name the
 * DJ saw; its whole tree (breadth-first) into g_fd. */
static int fd_collect(uintptr_t conn, FILE *lg) {
    void *c = (void *) conn;
    long long at[1] = {-1};
    char nm[256] = "";
    g_fd.n = g_fd.nlists = g_fd.nfolders = 0;
    if (sq_row(c, "SELECT attribute FROM playlist WHERE playlist_id = ?1", g_fd.folder, -1, at,
               1) != 1 ||
        !sq_text(c, "SELECT name FROM playlist WHERE playlist_id = ?1", g_fd.folder, nm, sizeof nm))
        return -20;
    if (strcmp(nm, g_fd.name))
        return -26;
    if (at[0] != PL_ATTR_FOLDER) { /* a PLAYLIST root: itself and its tracks */
        g_fd.ids[g_fd.n] = g_fd.folder;
        g_fd.isfolder[g_fd.n++] = 0;
        g_fd.nlists = -1;
        if (lg)
            fprintf(lg, "PLAYLIST %u \"%s\": remove\n", g_fd.folder, g_fd.name);
        return 0;
    }
    g_fd.ids[g_fd.n] = g_fd.folder;
    g_fd.isfolder[g_fd.n++] = 1;
    for (int q = 0; q < g_fd.n; q++) { /* the queue is the list itself */
        if (!g_fd.isfolder[q])
            continue;
        void *st = 0;
        if (g_sq.prepare(
                c, "SELECT playlist_id, attribute FROM playlist WHERE playlist_id_parent = ?1", -1,
                &st, 0) != 0 ||
            !st)
            return -42;
        g_sq.bind_int64(st, 1, g_fd.ids[q]);
        while (g_sq.step(st) == 100) {
            if (g_fd.n >= FD_MAX) {
                g_sq.finalize(st);
                return -46;
            }
            long long id = g_sq.column_int64(st, 0), a = g_sq.column_int64(st, 1);
            g_fd.ids[g_fd.n] = id;
            g_fd.isfolder[g_fd.n++] = a == PL_ATTR_FOLDER;
            if (a == PL_ATTR_FOLDER)
                g_fd.nfolders++;
            else
                g_fd.nlists++;
        }
        g_sq.finalize(st);
    }
    if (lg)
        fprintf(lg, "FOLDER %u \"%s\": %d playlists, %d folders inside\n", g_fd.folder, g_fd.name,
                g_fd.nlists, g_fd.nfolders);
    return 0;
}
static int fd_delete(uintptr_t conn, uintptr_t cache, FILE *lg) {
    void *c = (void *) conn;
    if (!cache)
        return -40;
    int rc = fd_collect(conn, lg); /* fresh: prove it again, as it is now */
    if (rc)
        return rc;
    long long par[1] = {0};
    if (sq_row(c, "SELECT playlist_id_parent FROM playlist WHERE playlist_id = ?1", g_fd.folder, -1,
               par, 1) != 1)
        return -20;
    jr_scope_parent(c, par[0]);        /* undo: the folder's own row ... */
    for (int i = 0; i < g_fd.n; i++) { /* ... every row under it, and every list's tracks */
        if (g_fd.isfolder[i])
            jr_scope_parent(c, g_fd.ids[i]);
        else
            jr_scope_pc(c, g_fd.ids[i]);
    }
    rc = lp_exec(c, "BEGIN", lg);
    for (int i = 0; !rc && i < g_fd.n; i++)
        if (!g_fd.isfolder[i])
            rc = sq_exec4(c, "DELETE FROM playlist_content WHERE playlist_id = ?1", g_fd.ids[i], 0,
                          0, 0);
    for (int i = 0; !rc && i < g_fd.n; i++)
        rc = sq_exec4(c, "DELETE FROM playlist WHERE playlist_id = ?1", g_fd.ids[i], 0, 0, 0);
    long long left[1] = {1};
    if (!rc && (sq_row(c, "SELECT COUNT(*) FROM playlist WHERE playlist_id = ?1", g_fd.folder, -1,
                       left, 1) != 1 ||
                left[0]))
        rc = -44;
    lp_exec(c, rc ? "ROLLBACK" : "COMMIT", lg);
    if (lg)
        fprintf(lg, "FOLDER DELETE %u: %d rows rc=%d\n", g_fd.folder, g_fd.n, rc);
    if (rc)
        return rc > 0 ? -43 : rc;
    lib_invalidate(cache, c, lg);
    return 0;
}
/* SearchView, found structurally when setVisible has not reported it yet (Search never
 * opened since EP147 started): a sibling of BrowseView under their common parent, or one
 * level down. juce's child list is an Array<Component*> whose offset is discovered (the
 * array under BrowseView's parent that holds BrowseView), not assumed. Every candidate is
 * vtable-checked. */
static uintptr_t nm_anchor(void) { /* an attached view to learn juce's child-list layout from */
    uintptr_t h = pe_view(&g_nm_host), par = 0;
    if (h && own_q(h + 0x18, &par) && par)
        return h;
    return pe_view(&g_bv_obj);
}
/* Message thread. `c` and every view passed in are live components; their child arrays and the
 * children in them are EP147's own fields of live components. */
static int nm_children(uintptr_t c, uintptr_t *arr, int *n) {
    uintptr_t bv = nm_anchor(), par = 0;
    static int off = -1, uoff = 0; /* data offset, and where numUsed sits after it */
    if (off < 0 && bv && own_q(bv + 0x18, &par) && par) { /* learn the child-list layout once */
        static const int uo[] = {0xc, 0x10, 0x8};
        for (int o = 0x20; o < 0x180 && off < 0; o += 8)
            for (int v = 0; v < 3 && off < 0; v++) {
                uintptr_t d = 0, q = 0;
                if (!own_q(par + o, &d) || !d || (d & 7) || !own_q((par + o + uo[v]) & ~7UL, &q))
                    continue;
                int u = (int) (((par + o + uo[v]) & 4) ? (q >> 32) : (q & 0xffffffff));
                if (u < 1 || u > 64)
                    continue;
                for (int i = 0; i < u; i++) {
                    uintptr_t e = 0;
                    if (own_q(d + i * 8, &e) && e == bv) {
                        off = o;
                        uoff = uo[v];
                        break;
                    }
                }
            }
        drag_log("NEWPL: juce child list at +%#x (count at +%#x)\n", off, uoff);
    }
    uintptr_t d = 0, q = 0;
    if (off < 0 || !own_q(c + off, &d) || !d || !own_q((c + off + uoff) & ~7UL, &q))
        return 0;
    *arr = d;
    *n = (int) (((c + off + uoff) & 4) ? (q >> 32) : (q & 0xffffffff));
    if (*n < 0 || *n > 256)
        return 0;
    return 1;
}
static uintptr_t nm_find_sv(void) {
    uintptr_t bv = nm_anchor(), par = 0, d = 0, vt = 0;
    int n = 0;
    if (!bv || !own_q(bv + 0x18, &par) || !par || !nm_children(par, &d, &n))
        return 0;
    for (int i = 0; i < n; i++) {
        uintptr_t c = 0;
        if (!own_q(d + i * 8, &c) || !c || (c & 7) || !own_q(c, &vt))
            continue;
        if (vt == SEARCHVIEW_VT)
            return c;
        uintptr_t d2 = 0;
        int n2 = 0;
        if (nm_children(c, &d2, &n2))
            for (int j = 0; j < n2; j++) {
                uintptr_t g = 0;
                if (own_q(d2 + j * 8, &g) && g && !(g & 7) && own_q(g, &vt) &&
                    vt == SEARCHVIEW_VT)
                    return g;
            }
    }
    return 0;
}
/* the keyboard that lives inside BrowseView from start-up (the streaming-login
 * layer's): a SoftwareKeyboardWidget anywhere up to 3 levels below BrowseView */
static uintptr_t nm_find_bv_kb(uintptr_t c, int depth) {
    uintptr_t d = 0, vt = 0;
    int n = 0;
    if (depth > 3 || !nm_children(c, &d, &n))
        return 0;
    for (int i = 0; i < n; i++) {
        uintptr_t k = 0;
        if (!own_q(d + i * 8, &k) || !k || (k & 7) || !own_q(k, &vt))
            continue;
        if (vt == SKW_VT)
            return k;
        uintptr_t r = nm_find_bv_kb(k, depth + 1);
        if (r)
            return r;
    }
    return 0;
}
/* The view the playlist list is on right now, walked up from the list itself: BROWSE's
 * playlist category lives in gui::browse::BrowseView, the PLAYLIST tab in
 * gui::browse::PlaylistView. On the PLAYLIST tab BrowseView is detached (parent NULL), so
 * the keyboard must go to whichever is actually on screen. 0 = not on screen. */
static int lb_showing(uintptr_t c);
static int plist_widget_ok(uintptr_t w, uintptr_t *lb);
static uintptr_t nm_host_view(void) {
    uintptr_t w = __atomic_load_n(&g_hl_widget, __ATOMIC_RELAXED), c = 0, vt = 0;
    if (!plist_widget_ok(w, &c) || !lb_showing(c)) /* a live widget, then its parents */
        return 0;
    for (int d = 0; d < 12 && c; d++) {
        if (!own_q(c, &vt))
            return 0;
        if (vt == BROWSEVIEW_VT || vt == PLAYLISTVIEW_VT)
            return c;
        if (!own_q(c + 0x18, &c))
            return 0;
    }
    return 0;
}
/* MENU > NEW PLAYLIST (message thread): lend the Search keyboard to the Browse or Playlist view on
 * screen */
static int nl_begin(unsigned media, unsigned ref_pl, unsigned folder) {
    if (!media)
        return -3;
    if (!g_nm_hooked)
        return -9;
    if (__atomic_load_n(&g_nm.state, __ATOMIC_ACQUIRE) != NM_IDLE)
        return -2;
    uintptr_t host = nm_host_view();
    if (!host) {
        drag_log("NEWPL: no Browse/Playlist view on screen above the list\n");
        return -6;
    }
    if (!wref_set(&g_nm_host, host)) {
        drag_log("NEWPL: the naming host cannot be tracked\n");
        return -6;
    }
    uintptr_t bv = pe_view(&g_bv_obj);
    if (!pe_view(&g_sv_obj)) { /* Search never opened since EP147 start */
        uintptr_t sv = nm_find_sv();
        drag_log("NEWPL: SearchView found structurally: %lx\n", (unsigned long) sv);
        if (sv)
            wref_set(&g_sv_obj, sv);
    }
    uintptr_t vt = 0, kb = (g_nm_hooked & 1) ? nm_search_kb(pe_view(&g_sv_obj)) : 0, par = 0;
    if (!kb && (g_nm_hooked & 2) && bv)
        kb = nm_find_bv_kb(bv, 0); /* Search never built: Browse's own keyboard */
    if (!kb) { /* anywhere below the views we know: Search's, the host's, Browse's */
        uintptr_t cand[3] = {pe_view(&g_sv_obj), host, bv};
        for (int i = 0; i < 3 && !kb; i++)
            if (cand[i])
                kb = nm_find_bv_kb(cand[i], 0);
        if (kb)
            drag_log("NEWPL: keyboard %lx found by walking the views' children\n",
                     (unsigned long) kb);
    }
    if (!kb || !own_q(kb + 0x18, &par) || !par) {
        drag_log("NEWPL: no keyboard (hooked %d sv %lx bv %lx kb %lx parent %lx)\n", g_nm_hooked,
                 (unsigned long) pe_view(&g_sv_obj), (unsigned long) bv, (unsigned long) kb,
                 (unsigned long) par);
        wref_set(&g_nm_host, 0);
        return -5;
    }
    g_nm.media = media;
    g_nm.ref_pl = ref_pl;
    g_nm.folder = folder;
    g_nm.name[0] = 0;
    g_nm.rename_pl = 0;
    g_nm.oldname[0] = 0;
    g_nm.fl_w = __atomic_load_n(&g_hl_widget,
                                __ATOMIC_ACQUIRE); /* nl_create appends it to the level shown: */
    g_nm.fl_row = __atomic_load_n(&g_hl_rows, __ATOMIC_RELAXED); /* its row is today's count */
    __atomic_store_n(&g_nm_create_req, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_nm_cancel_req, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_nm.state, NM_NAMING, __ATOMIC_RELEASE);
    wref_set(&g_nm_kb_parent, par); /* tracked: pe_wref_init registered both slots */
    ((void (*)(uintptr_t, uintptr_t, int)) JUCE_ADD_VISIBLE)(host, kb, -1);
    wref_set(&g_nm_kb, kb);
    __atomic_store_n(&g_nm_dirty, 1, __ATOMIC_RELEASE);
    drag_log("NEWPL: naming in %s (media %04x, beside playlist %u), keyboard %lx\n",
             own_q(host, &vt) && vt == PLAYLISTVIEW_VT ? "PlaylistView" : "BrowseView", media,
             ref_pl, (unsigned long) kb);
    return 0;
}
/* MENU > RENAME: the same keyboard, prefilled with the current name. */
static int nl_rename_begin(unsigned media, unsigned pl, const char *cur, unsigned folder) {
    if (!cur || !cur[0])
        return -3;
    int rc = nl_begin(media, pl, folder);
    if (rc)
        return rc;
    g_nm.rename_pl = pl;
    utf8_copy(g_nm.oldname, sizeof g_nm.oldname, cur);
    utf8_copy(g_nm.name, sizeof g_nm.name, cur);
    __atomic_store_n(&g_nm_dirty, 1, __ATOMIC_RELEASE);
    drag_log("RENAME: naming %u \"%s\"\n", pl, cur);
    return 0;
}
static void pl_edit_notify(int op, int rc);
/* message thread, from msg_tick */
static void nm_tick(void) {
    if (__atomic_load_n(&g_nm.state, __ATOMIC_ACQUIRE) != NM_NAMING)
        return;
    {
        uintptr_t h = pe_view(&g_nm_host), par = 0; /* the screen went away under us */
        if (!h || !own_q(h + 0x18, &par) || !par) { /* destroyed (slot cleared) or detached */
            nm_end("left Browse: canceled");
            return;
        }
    }
    if (__atomic_exchange_n(&g_nm_cancel_req, 0, __ATOMIC_ACQ_REL)) {
        nm_end("canceled");
        return;
    }
    if (!__atomic_exchange_n(&g_nm_create_req, 0, __ATOMIC_ACQ_REL))
        return;
    size_t k = strlen(g_nm.name);
    while (k && g_nm.name[k - 1] == ' ')
        g_nm.name[--k] = 0;
    char *b = g_nm.name;
    while (*b == ' ')
        b++;
    if (b != g_nm.name)
        memmove(g_nm.name, b, strlen(b) + 1);
    if (!g_nm.name[0]) {
        az_banner_strip(g_nm.folder ? "Type a name for the folder first"
                                    : "Type a name for the playlist first",
                        RS_MSG_MS);
        return;
    }
    if (g_nm.rename_pl && !strcmp(g_nm.name, g_nm.oldname)) {
        nm_end("rename: no change");
        az_banner_strip("No change", 2000);
        return;
    }
    /* a rename starts from the playlist's current name, which can be longer than the
     * operation carries: refuse it rather than write the name cut short */
    size_t len = strlen(g_nm.name);
    if (len >= sizeof g_mv.name) {
        az_banner_strip("Name too long", RS_MSG_MS);
        return;
    }
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE) {
        az_banner_strip("Busy \xc2\xb7 try again", RS_MSG_MS);
        return;
    }
    nm_kb_home();
    g_mv.op = g_nm.rename_pl ? MV_OP_RENAME : MV_OP_NEWPL;
    g_mv.media = g_nm.media;
    g_mv.pl = g_nm.rename_pl;
    g_mv.cid = 0;
    g_mv.from = g_mv.to = 0;
    memcpy(g_mv.name, g_nm.name, len + 1);
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    __atomic_store_n(&g_nm.state, NM_CREATING, __ATOMIC_RELEASE);
    __atomic_store_n(&g_nm_dirty, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    drag_log("NEWPL: creating \"%s\"\n", g_nm.name);
}
static void nl_rename_fixups(unsigned media, unsigned pl, const char *name);
static void fl_start(unsigned long w, int row, int plist);
static void nm_done(int rc) {
    __atomic_store_n(&g_nm.state, NM_IDLE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_nm_dirty, 1, __ATOMIC_RELEASE);
    static char msg[300];
    const char *what = g_nm.folder ? "folder" : "playlist";
    if (g_nm.rename_pl) {
        if (rc == 0) {
            snprintf(msg, sizeof msg, "Renamed to %.200s", g_nm.name);
            nl_rename_fixups(g_nm.media, g_nm.rename_pl,
                             g_nm.name); /* the target / clipboard carry names */
        } else
            snprintf(msg, sizeof msg, "%s",
                     rc == -61   ? "Name already used there"
                     : rc == -26 ? "That list changed \xc2\xb7 try again"
                                 : "Couldn't rename it");
        g_nm.rename_pl = 0;
    } else if (rc == 0) {
        snprintf(msg, sizeof msg, "Created %s %.200s", what, g_nm.name);
        if (g_nm.fl_w && g_nm.fl_row > 0)
            fl_start(g_nm.fl_w, g_nm.fl_row, 1); /* glow + scroll to it */
    } else if (rc == -61)
        snprintf(msg, sizeof msg, "Name already used there");
    else
        snprintf(msg, sizeof msg, "The %s could not be created (%d)", what, rc);
    if (rc)
        az_banner_strip(msg, RS_MSG_MS); /* success is on screen: silent */
    else
        drag_log("NEWPL/RENAME: \"%s\" (silent: on screen)\n", msg);
}
int azmod_naming_playlist(void) {
    return __atomic_load_n(&g_nm.state, __ATOMIC_ACQUIRE) == NM_NAMING;
}
const char *azmod_naming_text(void) {
    return g_nm.name;
}
int azmod_naming_folder(void) {
    return g_nm.folder != 0;
}
int azmod_naming_rename(void) {
    return g_nm.rename_pl != 0;
}
void azmod_naming_create(void) {
    __atomic_store_n(&g_nm_create_req, 1, __ATOMIC_RELEASE);
}
void azmod_naming_cancel(void) {
    __atomic_store_n(&g_nm_cancel_req, 1, __ATOMIC_RELEASE);
}
int azmod_naming_dirty(void) {
    return __atomic_exchange_n(&g_nm_dirty, 0, __ATOMIC_ACQ_REL);
}
/* Which mount a media id is on: each library server's database carries its media id
 * (SQLDB_MEDIA_Q) and its sqlite file path gives the mount. Recorded once per server
 * (library thread) for the stems mark, which must resolve a playlist on the stick being
 * browsed: playlist ids are per-stick counters, so two sticks' playlists with the same
 * id differ. */
#define MM_N 4
static struct {
    _Atomic unsigned long server;
    _Atomic unsigned media;
    _Atomic unsigned seq;
    char mount[128];
} g_mm[MM_N];
static void mm_note(void *server) {
    for (int i = 0; i < MM_N; i++)
        if (__atomic_load_n(&g_mm[i].server, __ATOMIC_ACQUIRE) ==
            (unsigned long) (uintptr_t) server)
            return;
    uintptr_t svc = 0, lib = 0, db = 0, conn = 0;
    unsigned media = 0;
    char mnt[128];
    /* library thread, inside MusicLibraryServer::onMessage: `server` and the chain from it are
     * EP147's own objects for this call */
    if (mlserver_chain(server, &svc, &lib, &db, &media) || !own_q(db + SQLDB_CONN, &conn) ||
        !conn || !cp_mount_of((void *) conn, mnt, sizeof mnt))
        return;
    static unsigned seq, rr;
    int k = -1;
    for (int i = 0; i < MM_N && k < 0; i++)
        if (!__atomic_load_n(&g_mm[i].server, __ATOMIC_ACQUIRE))
            k = i;
    if (k < 0)
        k = (int) (rr++ % MM_N);
    __atomic_store_n(&g_mm[k].server, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mm[k].media, 0, __ATOMIC_RELEASE);
    /* media reads 0 before any byte of the new mount is visible (media_mount re-checks it) */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    snprintf(g_mm[k].mount, sizeof g_mm[k].mount, "%s", mnt);
    __atomic_store_n(&g_mm[k].seq, ++seq, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mm[k].media, media, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mm[k].server, (unsigned long) (uintptr_t) server, __ATOMIC_RELEASE);
    fc_mount_cleanup(mnt); /* an interrupted FOLDER-view copy onto this stick */
}
/* the newest server's mount for `media`; 0 = unknown (then nothing is drawn) */
static int media_mount(unsigned media, char *out, size_t n) {
    unsigned best = 0;
    out[0] = 0;
    for (int i = 0; i < MM_N; i++) {
        if (!media || __atomic_load_n(&g_mm[i].media, __ATOMIC_ACQUIRE) != media)
            continue;
        unsigned sq = __atomic_load_n(&g_mm[i].seq, __ATOMIC_ACQUIRE);
        if (sq <= best)
            continue;
        /* mm_note can reuse this slot while we copy (a fifth server): keep the copy only if
         * media and seq are unchanged after it. A rewrite zeroes media before the mount and
         * moves seq before restoring media, so either change shows. */
        char tmp[sizeof g_mm[i].mount];
        snprintf(tmp, sizeof tmp, "%s", g_mm[i].mount);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_mm[i].media, __ATOMIC_ACQUIRE) != media ||
            __atomic_load_n(&g_mm[i].seq, __ATOMIC_RELAXED) != sq)
            continue;
        best = sq;
        snprintf(out, n, "%s", tmp);
    }
    return out[0] == '/';
}
/* Every Playlist Editing write also updates the stick's export.pdb (the Device Library that
 * older players and rekordbox read), so the Device Library never shows the old list. Same
 * library thread, EP147's own connection, after the edit's COMMIT; pdbsync builds the new
 * file whole and renames it over export.pdb (ps_replace), and a failure leaves export.pdb as
 * it was. */
static int mv_op_writes(int op) {
    switch (op) {
    case MV_OP_MOVE:
    case MV_OP_REMOVE:
    case MV_OP_ADD:
    case MV_OP_NEWPL:
    case MV_OP_PLCOPY:
    case MV_OP_PLMOVE:
    case MV_OP_PLORDER:
    case MV_OP_PLINTO:
    case MV_OP_UNDO:
    case MV_OP_ADDN:
    case MV_OP_RENAME:
    case MV_OP_FDEL:
        return 1;
    default:
        return 0;
    }
}
static void mv_pdb_sync(void *server) {
    uintptr_t svc = 0, lib = 0, db = 0, conn = 0;
    unsigned media = 0;
    char root[300];
    if (mlserver_chain(server, &svc, &lib, &db, &media) || !own_q(db + SQLDB_CONN, &conn) ||
        !conn) /* onMessage's own server, on its thread */
        return;
    if (!sq_resolve() || !cp_mount_of((void *) conn, root, sizeof root))
        return;
    FILE *lg = azlog_open("/tmp/azmod-libspool.log", "a");
    if (lg)
        setvbuf(lg, NULL, _IONBF, 0);
    char why[32];
    snprintf(why, sizeof why, "edit op %d", g_mv.op);
    ls_pdb_sync_root((void *) conn, root, 0, why, lg);
    if (lg)
        fclose(lg);
}
/* ---- export.pdb after EP147's own playlist writes (always, not only Playlist Editing) ---
 * Stock TAG LIST > CREATE PLAYLIST and stock DELETE / ALL DELETE write the stick's
 * exportLibrary.db through music_library::SqlitePlaylistUpdater (the device implementation
 * of IPlaylistUpdater, vt 0x284ab30) and never touch export.pdb. Its write slots +0x10..+0x50
 * (create, add track, delete all, delete by parent, delete all contents, delete one list's
 * contents, remove track, delete by row id, move track) are the narrowest native point that
 * says "a playlist write happened": its callers (InformationUpdater::deleteAllPlaylist
 * 0x1266b50: +0x30 then +0x20; the Tag List conversion 0x126b600: +0x10, then +0x18 per
 * track) reach it from the library thread, inside MusicLibraryServer::onMessage, on that
 * stick's own server. So a slot hit inside onMessage marks the message; when the message
 * returns (its transaction is over) the stick is noted in pdb_stock.c's schedule, and once
 * it has been quiet for PST_DEBOUNCE_MS the message tick wakes the library threads and that
 * stick's server runs the same pdb sync azmod's own edits use, at the start of a later
 * message. This sync runs whether or not the Playlist Editing mod is on (the Device Library
 * must mirror a stock playlist write either way); the mod's own gesture and edit menu are
 * unaffected. azmod's own edits and spool go through the writer too; they are excluded
 * (t_pst_own) because they already sync themselves. A slot hit outside onMessage has no
 * stick to name and is only counted. The writer's history and cloud siblings are other
 * classes and are not touched. */
#include "pdb_stock.c"
static struct pst g_pst = PST_INIT;
static _Atomic int g_pst_armed;         /* a sync is due: library threads look at g_pst */
static _Atomic unsigned g_pst_outside;  /* writer hits with no library message (diagnostic) */
static __thread uintptr_t t_pst_server; /* the server whose onMessage runs on this thread */
static __thread int t_pst_own;          /* >0 while azmod's own edit or spool writes */
static __thread unsigned t_pst_slots;   /* writer slots stock hit in this message */
#define PST_SLOTS 9                     /* +0x10 .. +0x50 */
typedef long (*pst_fn)(long, long, long, long, long, long, long, long);
static pst_fn g_pst_orig[PST_SLOTS];
static void pst_hit(int i) {
    if (t_pst_own)
        return;
    if (!t_pst_server) {
        __atomic_add_fetch(&g_pst_outside, 1, __ATOMIC_RELAXED);
        return;
    }
    t_pst_slots |= 1u << i;
}
/* Every slot returns an int status in w0 and takes at most seven register arguments
 * (create: updater, id, seq, name, image, attribute, parent), so eight pass straight
 * through and nothing is read from the caller's stack. */
#define PST_HOOK(i)                                                                           \
    static long pst_slot##i(long a, long b, long c, long d, long e, long f, long g, long h) { \
        pst_hit(i);                                                                           \
        return g_pst_orig[i](a, b, c, d, e, f, g, h);                                         \
    }
PST_HOOK(0)
PST_HOOK(1)
PST_HOOK(2)
PST_HOOK(3)
PST_HOOK(4)
PST_HOOK(5)
PST_HOOK(6)
PST_HOOK(7)
PST_HOOK(8)
static const pst_fn g_pst_hook[PST_SLOTS] = {pst_slot0, pst_slot1, pst_slot2, pst_slot3, pst_slot4,
                                             pst_slot5, pst_slot6, pst_slot7, pst_slot8};
static void pst_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-libspool.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
/* Once, from install_reorder, after the library hooks. Every slot must hold a code address
 * below the vtable (EP147's text) and none may be ours already; anything else declines the
 * whole set, and stock writes are then not followed (export.pdb stays as stock leaves it). */
static int install_pdb_stock(void) {
    static int done = 0, ok = 0;
    if (done)
        return ok;
    done = 1;
    uintptr_t vt = PLUPD_VT, v = 0, ti = 0, nm = 0;
    if (!vt) {
        pst_log("STOCK sync: unknown EP147 build: not installed\n");
        return 0;
    }
    /* the class, by its RTTI name (both firmwares carry it at vt-8 -> typeinfo +8) */
    static const char want[] = "N13music_library21SqlitePlaylistUpdaterE";
    char got[sizeof want + 7] = "";
    int named = img_q(vt - 8, &ti) && ti && img_q(ti + 8, &nm) && nm; /* EP147's image only */
    for (size_t o = 0; named && o < sizeof want; o += 8)
        named = img_q(nm + o, (uintptr_t *) (void *) (got + o));
    if (!named || memcmp(got, want, sizeof want)) {
        pst_log("STOCK sync: %lx is not SqlitePlaylistUpdater's vtable: not installed\n",
                (unsigned long) vt);
        return 0;
    }
    for (int i = 0; i < PST_SLOTS; i++) {
        if (!img_q(vt + 0x10 + 8 * i, &v) || v < 0x400000 || v >= vt || (v & 3)) {
            pst_log("STOCK sync: writer slot +0x%x holds %lx: not installed\n", 0x10 + 8 * i,
                    (unsigned long) v);
            return 0;
        }
    }
    for (int i = 0; i < PST_SLOTS; i++)
        if (azm_patch_vtable(vt + 0x10 + 8 * i, (void *) g_pst_hook[i],
                             (void **) &g_pst_orig[i]) != 1) {
            for (int j = 0; j < i; j++) { /* all or nothing */
                void *mine;
                azm_patch_vtable(vt + 0x10 + 8 * j, (void *) g_pst_orig[j], &mine);
            }
            pst_log("STOCK sync: patching writer slot +0x%x failed: not installed\n",
                    0x10 + 8 * i);
            return 0;
        }
    ok = 1;
    pst_log("STOCK sync: playlist writer slots +0x10..+0x50 hooked\n");
    return 1;
}
struct pst_scope {
    uintptr_t prev_server;
    unsigned prev_slots;
};
/* Every exit from onMessage, an exception included: the thread forgets the message, so a
 * later writer hit is never credited to it. */
static void pst_scope_end(struct pst_scope *s) {
    t_pst_server = s->prev_server;
    t_pst_slots = s->prev_slots;
}
/* onMessage returned normally: if stock wrote playlists in it, note this server's stick. */
static void pst_msg_done(void *server) {
    unsigned s = t_pst_slots;
    if (!s)
        return;
    uintptr_t a = 0, b = 0, c = 0;
    unsigned media = 0;
    if (mlserver_chain(server, &a, &b, &c, &media))
        media = 0;
    int pe = __atomic_load_n(&g_reorder_enabled, __ATOMIC_RELAXED);
    int r = pst_note(&g_pst, media, drag_now_ms());
    pst_log("STOCK write: writer slots %03x on media %04x, Playlist Editing %s -> %s\n", s, media,
            pe ? "on" : "off", /* the mod's toggle: logged, but the sync runs either way */
            r == 1   ? "export.pdb sync scheduled"
            : r == 0 ? "not followed (unknown stick)"
                     : "schedule full, not followed");
}
struct pst_ctx {
    void *server;
    uintptr_t conn;
    unsigned media;
};
static int pst_mount_cb(void *ctx, char *out, size_t n) {
    struct pst_ctx *c = ctx;
    return c->conn && sq_resolve() && cp_mount_of((void *) c->conn, out, n);
}
static void pst_sync_cb(void *ctx, const char *root) {
    struct pst_ctx *c = ctx;
    if (g_sq.get_autocommit((void *) c->conn) != 1) { /* inside a transaction: later */
        pst_note(&g_pst, c->media, drag_now_ms());
        pst_log("STOCK sync: %s is mid-transaction: retried later\n", root);
        return;
    }
    FILE *lg = azlog_open("/tmp/azmod-libspool.log", "a");
    if (lg)
        setvbuf(lg, NULL, _IONBF, 0);
    int rc = ls_pdb_sync_root((void *) c->conn, root, 0, "stock playlist write", lg);
    /* The paired backup only once export.pdb matches the library again, so the pair it
     * keeps is consistent; before the sync it would pair the new library with the old
     * export.pdb. A failed sync takes none and the earlier pair stands. */
    if (rc >= 0) {
        int br = lib_backup((void *) c->conn, lg);
        if (lg)
            fprintf(lg, "STOCK sync: paired backup rc=%d\n", br);
    }
    if (lg)
        fclose(lg);
}
/* At the start of a message, on `server`'s own library thread. */
static void pst_lib_service(void *server) {
    struct pst_ctx c = {server, 0, 0};
    uintptr_t svc = 0, lib = 0, db = 0;
    if (mlserver_chain(server, &svc, &lib, &db, &c.media))
        return; /* a library we cannot name takes nothing */
    if (!own_q(db + SQLDB_CONN, &c.conn)) /* onMessage's own server, on its thread */
        c.conn = 0;
    int r = pst_service(&g_pst, c.media, drag_now_ms(), pst_mount_cb, pst_sync_cb, &c);
    if (r < 0)
        pst_log("STOCK sync: media %04x has no stick mount: not synced\n", c.media);
}
/* Message thread, from msg_tick: wake the library threads while a sync is due. */
static void pst_tick(void) {
    static long long next;
    long long now = drag_now_ms();
    if (now < next)
        return;
    next = now + 250;
    int due = pst_any_due(&g_pst, now);
    __atomic_store_n(&g_pst_armed, due, __ATOMIC_RELEASE);
    if (due && !plrenew_kick())
        pst_log("STOCK sync: no renewal handler to wake the library thread\n");
}
static void mlserver_onmsg_hook(void *server, void *msg, unsigned param) {
    struct pst_scope pst_sc __attribute__((cleanup(pst_scope_end))) = {t_pst_server, t_pst_slots};
    t_pst_server = (uintptr_t) server;
    t_pst_slots = 0;
    if (__atomic_load_n(&g_pst_armed, __ATOMIC_ACQUIRE))
        pst_lib_service(server); /* a stock write's export.pdb sync, due on this stick */
    if (g_sm_on || __atomic_load_n(&g_rs_on, __ATOMIC_RELAXED))
        mm_note(server);
    int want = MV_ARMED;
    uintptr_t c_svc = 0, c_lib = 0, c_db = 0;
    unsigned c_media = 0;
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) == MV_ARMED
        /* only this stick's server may take it: the others leave it armed */
        && mlserver_chain(server, &c_svc, &c_lib, &c_db, &c_media) == 0 && c_media == g_mv.media &&
        __atomic_compare_exchange_n(&g_mv_state, &want, MV_TAKEN, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        FILE *lg = azlog_open("/tmp/azmod-drag.log", "a");
        if (lg)
            setvbuf(lg, NULL, _IONBF, 0); /* a crash must not eat the last line */
        t_pst_own++; /* our own edit syncs export.pdb itself (mv_pdb_sync) */
        g_mv.rc = mv_execute(server, lg);
        t_pst_own--;
        if (lg) {
            if (g_mv.op == MV_OP_TARGET)
                fprintf(lg, "TARGET pl=%u rc=%d\n", g_mv.pl, g_mv.rc);
            else if (g_mv.op == MV_OP_ADD)
                fprintf(lg, "ADD pl=%u cid=%u rc=%d%s\n", g_mv.pl, g_mv.cid, g_mv.rc,
                        g_mv.rc == 0 ? " (appended and read back)" : " (NOTHING WRITTEN)");
            else if (g_mv.op == MV_OP_REMOVE)
                fprintf(lg, "REMOVE pl=%u cid=%u seq %u rc=%d%s\n", g_mv.pl, g_mv.cid, g_mv.from,
                        g_mv.rc, g_mv.rc == 0 ? " (removed and read back)" : " (NOTHING WRITTEN)");
            else
                fprintf(lg, "COMMIT pl=%u cid=%u %u->%u rc=%d%s\n", g_mv.pl, g_mv.cid, g_mv.from,
                        g_mv.to, g_mv.rc,
                        g_mv.rc == 0 ? " (written and read back)" : " (NOTHING WRITTEN)");
            fclose(lg);
        }
        if (g_mv.rc == 0 && mv_op_writes(g_mv.op))
            mv_pdb_sync(server); /* Device Library follows the edit */
        __atomic_store_n(&g_mv_state, MV_DONE, __ATOMIC_RELEASE);
    }
    fc_refresh_service(server); /* a FOLDER-view copy landed on this stick: re-list it */
    if (__atomic_load_n(&g_ls_armed, __ATOMIC_ACQUIRE)) {
        t_pst_own++;        /* the spool syncs export.pdb itself (ls_pdb_sync) */
        ls_service(server); /* library spool, armed by ls_tick */
        t_pst_own--;
    }
    /* Original message forwarding boundary. */
    if (g_sdk_sources_on)
        azsdk_sources_forward_owned_message(g_ep147_build, server, msg, param, g_orig_mlserver_onmsg);
    else
        g_orig_mlserver_onmsg(server, msg, param);
    if (g_sdk_sources_on) azsdk_sources_observe_owned_message(server, NULL, 0);
    pst_msg_done(server); /* stock wrote playlists in it: schedule the export.pdb sync */
}
static _Atomic unsigned long g_tracklist_widget; /* tentative; defined with the widget capture */
static unsigned char fl_alpha(int full, long long e) {
    if (e < FL_HOLD)
        return (unsigned char) full;
    return (unsigned char) (full * (FL_MS - e) / (FL_MS - FL_HOLD));
}
static void cg_outline(void *g, int w, int h) { /* 2 px dashes, 8 on / 6 off, inside the row */
    unsigned char c[8] = {ROW_GRAB_B, ROW_GRAB_G, ROW_GRAB_R, 0xff, 0, 0, 0, 0};
    g_orig_setcolour(g, c);
    int t = 2, y0 = 2, y1 = h - 3, x0 = 2, x1 = w - 2;
    for (int x = x0; x < x1; x += 14) {
        int l = x + 8 > x1 ? x1 - x : 8;
        g_juce_fillrect(g, x, y0, l, t);
        g_juce_fillrect(g, x, y1 - t, l, t);
    }
    for (int y = y0; y < y1; y += 14) {
        int l = y + 8 > y1 ? y1 - y : 8;
        g_juce_fillrect(g, x0, y, t, l);
        g_juce_fillrect(g, x1 - t, y, t, l);
    }
}
static void fl_start(unsigned long w, int row, int plist) {
    if (!w || row < 0)
        return;
    g_fl.row = row;
    g_fl.plist = plist;
    g_fl.t0 = drag_now_ms();
    __atomic_store_n(&g_fl.w, w, __ATOMIC_RELEASE);
}
/* The grab, on the message thread. Once an eligible press (g_knob_held, set by the knob
 * handler only where a drop could commit) has been down DRAG_HOLD_MS, re-check that
 * EP147's hierarchy still agrees on the selected row, then grab. The tick runs
 * constantly, so the hold is honored whether or not the bus sends frames while the knob
 * sits still. One attempt per press. */
static void drag_try_grab(void) {
    if (!__atomic_load_n(&g_knob_held, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_drag_on, __ATOMIC_RELAXED) ||
        __atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED))
        return;
    if (drag_now_ms() - __atomic_load_n(&g_knob_press_ms, __ATOMIC_RELAXED) < DRAG_HOLD_MS)
        return;
    __atomic_store_n(&g_knob_held, 0, __ATOMIC_RELEASE); /* one attempt per press */
    if (__atomic_load_n(&g_knob_kind, __ATOMIC_RELAXED) == DRAG_KIND_PLIST) {
        /* A playlist/folder row. Re-prove it on the message thread: the one hierarchy list
         * on screen, JUCE's own selection there, and EP147's hierarchy agreeing on that
         * row. Then snapshot every displayed name: the library thread compares them with
         * the DB's sibling order before writing. */
        uintptr_t w = __atomic_load_n(&g_hl_widget, __ATOMIC_RELAXED), lb = 0;
        struct plsnap sn = plsnap_get();
        int rows = w ? plist_rows(w) : 0, sel = -1;
        if (w && plist_widget_ok(w, &lb) && lb_showing(lb))
            sel = ((int (*)(void *, int)) LISTBOX_GETSELECTED)((void *) lb, 0);
        if (!(rows > 1 && sel >= 0 && sel < rows && sn.sel_pl && sn.any_media &&
              (unsigned) sel == sn.sel_row)) {
            if (drag_retry_ok()) {
                __atomic_store_n(&g_knob_held, 1, __ATOMIC_RELEASE);
                return;
            } /* still catching up: next tick */
            drag_log("GRAB refused (playlist list): w=%lx sel=%d rows=%d snap pl=%u row=%u\n",
                     (unsigned long) w, sel, rows, sn.sel_pl, sn.sel_row);
            return;
        }
        if (!plist_snapshot_names(w, rows, sel)) {
            drag_log(
                "GRAB refused (playlist list): the grabbed row's name is not readable (%d rows)\n",
                rows);
            az_banner_strip("This list can't be reordered", PL_MSG_READ);
            return;
        }
        if (!dh_open(&g_dh)) { /* never: drag_handoff_drain ran first on this tick */
            drag_log("GRAB refused: the previous drag's end is still untaken\n");
            return;
        }
        g_grab = sn;
        __atomic_store_n(&g_drag_kind, DRAG_KIND_PLIST, __ATOMIC_RELAXED);
        __atomic_store_n(&g_drag_widget, (unsigned long) w, __ATOMIC_RELAXED);
        __atomic_store_n(&g_drag_origin, sel, __ATOMIC_RELAXED);
        __atomic_store_n(&g_drag_target, sel, __ATOMIC_RELAXED);
        __atomic_store_n(&g_drag_rows, rows, __ATOMIC_RELAXED);
        __atomic_store_n(&g_drag_grab_ms, drag_now_ms(), __ATOMIC_RELAXED);
        __atomic_store_n(&g_cursor_row, sel, __ATOMIC_RELAXED);
        __atomic_store_n(&g_cursor_sync, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_drag_on, 1, __ATOMIC_RELEASE);
        drag_log("GRAB playlist-list row=%d of %d: %s %u media=%04x \"%s\"\n", sel, rows,
                 sn.sel_folder ? "folder" : "playlist", sn.sel_pl, sn.any_media, sn.sel_title);
        az_banner_strip("Turn to move \xc2\xb7 Press to drop \xc2\xb7 BACK to cancel", PL_MSG_HOLD);
        drag_request_update();
        return;
    }
    int rows = drag_num_rows();
    int sel = tracklist_selected_row(); /* JUCE's selection, and only of a list on screen */
    struct plsnap sn = plsnap_get();
    if (!(rows > 1 && sel >= 0 && sel < rows && sn.playlist_id && (unsigned) sel == sn.row)) {
        if (drag_retry_ok()) {
            __atomic_store_n(&g_knob_held, 1, __ATOMIC_RELEASE);
            return;
        } /* still catching up: next tick */
        drag_log("GRAB refused: sel=%d rows=%d snap pl=%u row=%u\n", sel, rows, sn.playlist_id,
                 sn.row);
        return;
    }
    if (!dh_open(&g_dh)) { /* never: drag_handoff_drain ran first on this tick */
        drag_log("GRAB refused: the previous drag's end is still untaken\n");
        return;
    }
    g_grab = sn;
    __atomic_store_n(&g_drag_kind, DRAG_KIND_TRACK, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_widget, __atomic_load_n(&g_tracklist_widget, __ATOMIC_RELAXED),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_origin, sel, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_target, sel, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_rows, rows, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_grab_ms, drag_now_ms(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_cursor_row, sel, __ATOMIC_RELAXED);
    __atomic_store_n(&g_cursor_sync, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_on, 1, __ATOMIC_RELEASE);
    drag_log("GRAB row=%d of %d: pl=%u cid=%u media=%04x\n", sel, rows, sn.playlist_id,
             sn.content_id, sn.media);
    /* The amber row alone does not tell a DJ how to drop or back out. */
    az_banner_strip("Turn to move \xc2\xb7 Press to drop \xc2\xb7 BACK to cancel",
                    PL_MSG_HOLD); /* until the drag ends */
    /* Repaint on the next tick: repainting inside the timer callback that decided the
     * grab leaves the screen blue until the knob turns (cause unknown); deferred by one
     * tick it shows at once. */
    drag_request_update();
}
static void fl_tick(void) {
    unsigned long w = __atomic_load_n(&g_fl.w, __ATOMIC_ACQUIRE);
    if (!w)
        return;
    static long long next;
    long long now = drag_now_ms(), e = now - g_fl.t0;
    if (e < FL_MS && now < next)
        return;
    next = now + 33;
    uintptr_t vt = 0, lb = 0, lbvt = 0;
    /* the flash's list must still be a live widget of its kind (live sets), then plain loads */
    int ok = !(w & 7) && (g_fl.plist ? pe_hier_live(w) : pe_track_live(w)) && own_q(w, &vt);
    if (ok)
        ok = g_fl.plist ? (vt == HIER_WIDGET_VT && plist_widget_ok(w, &lb))
                        : (vt == TRACKLIST_WIDGET_VT && own_q(w + 0x48, &lb));
    if (ok)
        ok = lb >= 0x10000 && !(lb & 7) && own_q(lb, &lbvt) && lbvt;
    /* Scroll to the row: a paste lands at the end of a playlist and a new playlist/folder at
     * the end of its level, often off screen. Done every flash frame since EP147 re-lists a
     * moment after the write and the row may not exist yet; a no-op for a row already on
     * screen. Message thread. */
    if (ok && e < FL_MS)
        drag_scroll_to(lb, g_fl.row);
    if (ok)
        ((void (*)(void *, int)) EP_21e7540)(
            (void *) lb,
            g_fl.row); /* juce::ListBox::repaintRow (LISTBOX_REPAINTROW, defined below) */
    if (!ok || e >= FL_MS)
        __atomic_store_n(&g_fl.w, 0, __ATOMIC_RELEASE); /* done (the repaint above drew it clean) */
}
/* The drop, on the message thread (drag_handoff_drain), with the rows the bus thread
 * recorded on the press. */
static void drag_commit(const struct dh_intent *it) {
    int o = it->origin;
    int t = it->target;
    int into = it->into;
    if (o == t && into < 0) {
        drag_log("COMMIT no-op (origin == target)\n");
        az_banner_strip("No change", PL_MSG_BRIEF);
        drag_pin(o);
        drag_clear();
        drag_request_update();
        return;
    }
    /* The row a DJ sees is the playlist position only in "#" ascending order over a
     * contiguous 1..N playlist. GRAB required the first; the library thread proves the
     * second, and that seq `from` holds this content_id, against the DB before anything is
     * written (mv_execute). */
    if (!(__atomic_load_n(&g_drag_kind, __ATOMIC_RELAXED) == DRAG_KIND_PLIST
              ? g_grab.sel_pl && g_grab.any_media
              : g_grab.playlist_id)) {
        drag_log("COMMIT REFUSED %d->%d: no identity was recorded at GRAB. NOTHING WRITTEN\n", o,
                 t);
        az_banner_strip("Couldn't move this track", PL_MSG_READ);
        drag_pin(o);
        drag_clear();
        drag_request_update();
        return;
    }
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE) {
        drag_log("COMMIT REFUSED %d->%d: the previous commit is still in flight. NOTHING WRITTEN\n",
                 o, t);
        az_banner_strip("Busy \xc2\xb7 try again", PL_MSG_READ);
        drag_pin(o);
        drag_clear();
        drag_request_update();
        return;
    }
    if (__atomic_load_n(&g_drag_kind, __ATOMIC_RELAXED) == DRAG_KIND_PLIST) {
        g_mv.op =
            into >= 0
                ? MV_OP_PLINTO
                : MV_OP_PLORDER; /* the row's id; siblings and order proven on the library thread */
        g_mv.pl = g_grab.sel_pl;
        g_mv.cid = 0;
        g_mv.media = g_grab.any_media;
    } else {
        g_mv.op = MV_OP_MOVE;
        g_mv.pl = g_grab.playlist_id;
        g_mv.cid = g_grab.content_id;
        g_mv.media = g_grab.media;
    }
    g_mv.from = (unsigned) o + 1;
    g_mv.to = (unsigned) (into >= 0 ? into : t) + 1; /* PLINTO: the folder's position */
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    __atomic_store_n(&g_drag_committing, 1,
                     __ATOMIC_RELAXED); /* keep the preview, drop the amber */
    { /* the landing flash starts on the press: the preview already shows the row there
       * (canceled if the write fails) */
        int fd = it->into_disp;
        int land =
            into >= 0 ? (fd > o ? fd - 1 : fd) : t; /* a folder moves up when a row above it left */
        fl_start(__atomic_load_n(&g_drag_widget, __ATOMIC_RELAXED), land,
                 __atomic_load_n(&g_drag_kind, __ATOMIC_RELAXED) == DRAG_KIND_PLIST);
    }
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    drag_log("COMMIT armed: pl=%u cid=%u seq %u -> %u (media %04x)\n", g_mv.pl, g_mv.cid, g_mv.from,
             g_mv.to, g_mv.media);
    drag_request_update();
}
static void drag_cancel(void) {
    drag_log("CANCEL: nothing was ever written\n");
    az_banner_strip_clear(); /* ends the held drag line; no "Canceled" banner */
    drag_pin(__atomic_load_n(&g_drag_origin, __ATOMIC_RELAXED));
    drag_clear();
    drag_request_update();
}
/* Message thread: perform the drop or BACK the bus read thread recorded (drag_handoff.h).
 * Runs first in msg_tick, before anything else there looks at the drag, and whether or not
 * Playlist Editing is still on: the press was already swallowed, so it must be answered. */
static void drag_handoff_drain(void) {
    struct dh_intent it;
    if (!dh_take(&g_dh, &it))
        return;
    /* The record can only exist for an open drag (the latch is reopened at GRAB and
     * closed by the record itself), so this is a guard, never a path. */
    if (!__atomic_load_n(&g_drag_on, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED)) {
        drag_log("HANDOFF %s dropped: no open drag\n", it.op == DH_COMMIT ? "COMMIT" : "CANCEL");
        return;
    }
    if (it.op == DH_COMMIT)
        drag_commit(&it);
    else
        drag_cancel();
}
/* Message thread: end the open drag here (the list changed or left the screen). If the bus
 * thread recorded a drop or BACK first, that one is the end: perform it instead. */
static void drag_end_local(void) {
    if (dh_claim_local(&g_dh))
        drag_cancel();
    else
        drag_handoff_drain();
}
static _Atomic unsigned long g_tracklist_widget;
static __thread int g_in_update;
static __thread int g_in_update_msg; /* inside our updateContent (seen by the setRowData hook) */
/* `model` is the TableListBoxModel EP147 is calling right now (its widget is alive): the widget
 * joins the live set, and only a widget in it is ever read through g_tracklist_widget. */
static void drag_capture_widget(void *model) {
    static unsigned rr;
    uintptr_t vt = 0, wvt = 0, w = (uintptr_t) model - 0x30;
    /* Called for every cell refresh (hundreds per scroll): nothing to do when the widget is
     * the one we already hold and it is still in the live set. */
    if (model && __atomic_load_n(&g_tracklist_widget, __ATOMIC_RELAXED) == (unsigned long) w &&
        pe_track_live(w))
        return;
    if (!model || !own_q((uintptr_t) model, &vt) || vt != TRACKLIST_MODEL_VT ||
        !own_q(w, &wvt) || wvt != TRACKLIST_WIDGET_VT)
        return;
    pe_set_add(g_pe_tlw, PE_TLW_N, w, &rr);
    wref_set(&g_tracklist_widget, w); /* a wref slot: the widget's destructor clears it */
}
static int drag_num_rows(void) {
    unsigned long w = __atomic_load_n(&g_tracklist_widget, __ATOMIC_RELAXED);
    uintptr_t vt = 0, n = 0;
    if (!pe_track_live(w) || !own_q(w, &vt) || vt != TRACKLIST_WIDGET_VT)
        return 0;
    /* getNumRows is two instructions: `return *(int*)(this+0x2c0)` */
    if (!own_q(w + 0x2c0, &n))
        return 0;
    return (int) (unsigned) (n & 0xffffffffu);
}
/* an int field of a live object (the caller's) */
static int rd_i32(uintptr_t a, int *out) {
    uintptr_t q = 0;
    if (!own_q(a & ~7UL, &q))
        return 0;
    *out = (int) (unsigned) ((a & 4) ? (q >> 32) : (q & 0xffffffffu));
    return 1;
}
/* juce::ListBox::scrollToEnsureRowIsOnscreen, as selectRowInternal 0x21e7dd0
 * inlines it: viewport [lb+0xe0], row height [lb+0xfc]; the viewport's first /
 * last visible rows at +0x29c / +0x2a0, x at +0x120, visible height at +0x16c;
 * Viewport::setViewPosition(Point<int>) 0x21e3440. EP147's list is frozen during
 * a drag, so this keeps the held track on screen. It must not touch EP147's
 * selection: selectRow would move the browse cursor and start a track-info load
 * under the drag. */
/* Message thread: `lb` is the ListBox of a live list widget (the caller checked the widget in
 * this same call), and its viewport is its own member. */
static void drag_scroll_to(uintptr_t lb, int row) {
    uintptr_t vp = 0;
    int rowh = 0, first = 0, last = 0, x = 0, vish = 0;
    if (!on_msg_thread() || !own_q(lb + 0xe0, &vp) || vp < 0x10000)
        return;
    if (!rd_i32(lb + 0xfc, &rowh) || rowh <= 0 || rowh > 1000)
        return;
    if (!rd_i32(vp + 0x29c, &first) || !rd_i32(vp + 0x2a0, &last) || !rd_i32(vp + 0x120, &x) ||
        !rd_i32(vp + 0x16c, &vish))
        return;
    int y;
    if (row < first)
        y = row * rowh;
    else if (row >= last) {
        y = (row + 1) * rowh - vish;
        if (y < 0)
            y = 0;
    } else
        return;
    int pt[2] = {x, y};
    ((void (*)(void *, int *)) VIEWPORT_SETVIEWPOS)((void *) vp, pt);
}
static void drag_update_content(void) {
    int plk = __atomic_load_n(&g_drag_kind, __ATOMIC_RELAXED) == DRAG_KIND_PLIST;
    unsigned long w = plk ? __atomic_load_n(&g_drag_widget, __ATOMIC_RELAXED)
                          : __atomic_load_n(&g_tracklist_widget, __ATOMIC_RELAXED);
    uintptr_t vt = 0, lb = 0;
    if (!w || g_in_update)
        return;
    if (!(plk ? pe_hier_live(w) : pe_track_live(w)) || !own_q(w, &vt) ||
        vt != (plk ? HIER_WIDGET_VT : TRACKLIST_WIDGET_VT)) {
        if (!plk)
            wref_set(&g_tracklist_widget, 0);
        drag_log("UPDATE refused: widget vptr %lx\n", (unsigned long) vt);
        return;
    }
    if (!own_q(w + 0x48, &lb) || lb < 0x10000) {
        drag_log("UPDATE refused: listbox %lx\n", (unsigned long) lb);
        return;
    }
    void (*update_content)(void *) = (void (*)(void *)) LISTBOX_UPDATECONTENT;
    g_in_update = 1;
    g_in_update_msg = 1;
    update_content((void *) lb);
    g_in_update = 0;
    g_in_update_msg = 0;
    /* updateContent refreshes only the component cells (preview strip, title), and juce
     * repaints only those children, so the paintCell cells ("#", art, BPM, Key) and the row
     * background would keep stale pixels. Repaint the whole changed rows: the union of the
     * permuted range now and the one last painted, which also covers the drag ending.
     * juce::ListBox::repaintRow 0x21e7540 = Component::repaint(row rect). */
    {
        static int last_lo = -1, last_hi = -1;
        int lo = -1, hi = -1;
        if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED)) {
            int o = __atomic_load_n(&g_drag_origin, __ATOMIC_RELAXED);
            int t = __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED);
            lo = o < t ? o : t;
            hi = o < t ? t : o;
            if (lo > 0)
                lo--; /* the folder being hovered sits next to the target */
            hi++;
        }
        int a = lo, b = hi;
        if (last_lo >= 0) {
            if (a < 0 || last_lo < a)
                a = last_lo;
            if (last_hi > b)
                b = last_hi;
        }
        int n = plk ? plist_rows(w) : drag_num_rows();
        if (a >= 0 && n > 0) {
            if (b > n - 1)
                b = n - 1;
            void (*repaint_row)(void *, int) = (void (*)(void *, int)) LISTBOX_REPAINTROW;
            for (int r = a; r <= b; r++)
                repaint_row((void *) lb, r);
        }
        last_lo = lo;
        last_hi = hi;
        if (lo >= 0)
            drag_scroll_to(lb, __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED));
    }
}
static void (*g_orig_rowpaint)(void *, void *, int, int, int, int, int);
static void rowpaint_hook(void *self, void *g, int row, int w, int h, int sel, int extra) {
    if (extra)
        lfocus_note((uintptr_t) self - 0x38,
                    sel != 0); /* w5 on the selected row = this list has the knob */
    static _Atomic int budget = 24;
    if (g_rowprobe && __atomic_fetch_sub(&budget, 1, __ATOMIC_RELAXED) > 0) {
        FILE *f = azlog_open("/tmp/azmod-rowpaint.log", "a");
        if (f) {
            fprintf(f, "ROWPAINT self=%lx g=%lx row=%d w=%d h=%d sel=%d extra=%d\n",
                    (unsigned long) (uintptr_t) self, (unsigned long) (uintptr_t) g, row, w, h, sel,
                    extra);
            fclose(f);
        }
    }
    /* Probe: skip only the selected row while the flag file exists. `extra` (w6), not `sel`
     * (w5), marks the selected row: w5 is 1 on every row of the focused list. */
    if (g_rowprobe && extra && AZ_PROBE_ON("/tmp/azmod-rowskip"))
        return;
    /* Mark the window so setcolour_hook knows this color belongs to a row background, and
     * whether it is the selected one. Cleared on every path. */
    /* While dragging, azmod owns the highlight: amber on the row the held row is over, and
     * EP147's blue suppressed, since its selection stays at the origin and would paint a
     * second highlight on a row the DJ is not pointing at. */
    int dragged_here = drag_is_widget((uintptr_t) self - 0x38); /* this painter serves both lists */
    int drop_folder = 0; /* this display row is the folder the held row would drop into */
    if (dragged_here) {
        int fd = __atomic_load_n(&g_drag_into_disp, __ATOMIC_RELAXED);
        drop_folder = fd >= 0 && row == fd &&
                      __atomic_load_n(&g_drag_into, __ATOMIC_RELAXED) >= 0 &&
                      !__atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED);
        /* the held row stays amber while hovering a folder (amber on the folder would read as
         * the folder being grabbed); the folder is outlined instead */
        extra = row == __atomic_load_n(&g_drag_target,
                                       __ATOMIC_RELAXED); /* committing too: the plain selection
                                                             (amber is g_row_grabbed's) */
        row = drag_remap(row);
    } else if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED)) {
        /* the other list while one is dragged: stock, untouched */
    } else if (extra) {
        /* Not dragging: EP147's own selection, the row the gesture would pick up. The row
         * painter is the only place azmod is told which row is selected. */
        __atomic_store_n(&g_sel_row, row, __ATOMIC_RELAXED);
    }
    g_in_rowpaint = extra ? 2 : 1;
    g_rowpaint_w = (uintptr_t) self - 0x38; /* the list widget, as drag_is_widget keys it */
    g_rowpaint_seq = 0;
    __atomic_store_n(&g_row_grabbed,
                     (dragged_here && !__atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED)) ||
                         (g_rowprobe && AZ_PROBE_ON("/tmp/azmod-rowyellow")),
                     __ATOMIC_RELAXED);
    g_orig_rowpaint(self, g, row, w, h, sel, extra);
    /* The folder the held row would drop into: an amber outline, 3 px, around the row. */
    if (drop_folder && g_orig_setcolour && h > 12 && w > 12) {
        unsigned char c[8];
        c[0] = ROW_GRAB_B;
        c[1] = ROW_GRAB_G;
        c[2] = ROW_GRAB_R;
        c[3] = 0xff;
        c[4] = c[5] = c[6] = c[7] = 0;
        g_orig_setcolour(g, c);
        g_juce_fillrect(g, 0, 0, w, 5);
        g_juce_fillrect(g, 0, h - 6, w, 5);
        g_juce_fillrect(g, 0, 0, 5, h - 1);
        g_juce_fillrect(g, w - 5, 0, 5, h - 1);
    }
    /* Landing flash: the accent color fading out over FL_MS, behind the row's text. */
    /* also while the drop commits and settles (the flash starts on the press). `row` is the
     * model row here (remapped above); the flash's row is a display row */
    if ((!dragged_here || __atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED)) &&
        (unsigned long) ((uintptr_t) self - 0x38) == __atomic_load_n(&g_fl.w, __ATOMIC_ACQUIRE) &&
        row == (dragged_here ? drag_remap(g_fl.row) : g_fl.row) && g_orig_setcolour && h > 1) {
        long long e = drag_now_ms() - g_fl.t0;
        if (e >= 0 && e < FL_MS) {
            unsigned char c[8];
            c[0] = ROW_GRAB_B;
            c[1] = ROW_GRAB_G;
            c[2] = ROW_GRAB_R;
            c[3] = fl_alpha(170, e);
            c[4] = c[5] = c[6] = c[7] = 0;
            g_orig_setcolour(g, c);
            g_juce_fillrect(g, 0, 0, w, h - 1);
        }
    }
    if (!dragged_here && g_orig_setcolour && h > 8 && w > 40 && g_clip.kind == CLIP_TRACK &&
        g_clip.move && cg_track_row((uintptr_t) self - 0x38, row))
        cg_outline(g, w, h);
    /* The selected (ADD TO) playlist's row: orange brackets, "[" at its left edge and
     * "]" at its right, 4 px strokes, 14 px arms, 4 px in. */
    if (!dragged_here && row == __atomic_load_n(&g_mk_row, __ATOMIC_RELAXED) &&
        (unsigned long) ((uintptr_t) self - 0x38) == __atomic_load_n(&g_mk_w, __ATOMIC_ACQUIRE) &&
        g_orig_setcolour && h > 16 && w > 40) {
        unsigned char c[8];
        c[0] = ROW_GRAB_B;
        c[1] = ROW_GRAB_G;
        c[2] = ROW_GRAB_R;
        c[3] = 0xff;
        c[4] = c[5] = c[6] = c[7] = 0;
        g_orig_setcolour(g, c);
        int t = 4, arm = 14, in = 4, y0 = 4, y1 = h - 5;
        g_juce_fillrect(g, in, y0, t, y1 - y0); /* [ */
        g_juce_fillrect(g, in, y0, arm, t);
        g_juce_fillrect(g, in, y1 - t, arm, t);
        g_juce_fillrect(g, w - in - t, y0, t, y1 - y0); /* ] */
        g_juce_fillrect(g, w - in - arm, y0, arm, t);
        g_juce_fillrect(g, w - in - arm, y1 - t, arm, t);
    }
    /* The grabbed row, amber, over EP147's own bar. Full height: on the selected row EP147's
     * last pixel row is drawn in the selection blue, not the grid gray. */
    if (extra && __atomic_load_n(&g_row_grabbed, __ATOMIC_RELAXED) && g_orig_setcolour && h > 1) {
        unsigned char c[8];
        c[0] = ROW_GRAB_B;
        c[1] = ROW_GRAB_G;
        c[2] = ROW_GRAB_R;
        c[3] = 0xff;
        c[4] = c[5] = c[6] = c[7] = 0;
        g_orig_setcolour(g, c);
        g_juce_fillrect(g, 0, 0, w, h);
    }
    g_in_rowpaint = 0;
}
/* The row currently being built. Set by whichever hook is told a row number (the line
 * builder, or refreshComponentForCell) and read by the waveform probe, so an identity
 * lookup is attributed to a row rather than guessed from arrival order. */
static __thread int g_cur_row = -1;
/* A row's text is not drawn by paintCell: refreshComponentForCell builds a child
 * Component per cell that juce paints separately, so a row remap must be applied
 * there too or the highlight moves without the titles.
 *   juce signature: Component* refreshComponentForCell(int row, int columnId,
 *                       bool isRowSelected, Component* existingToUpdate) */
/* paintRowBackground's first argument is neither the widget nor the model: its
 * first quadword is packed data (0x100000003), not a vptr; `ldrb [x0,#4]` picks
 * the draw path. refreshComponentForCell is patched on the model's vtable, so it
 * receives the model subobject, and the widget is 0x30 below it (the
 * TableListBoxModel subobject sits at widget+0x30). Validate the model's vptr
 * before trusting that arithmetic. */
static void *(*g_orig_refreshcell)(void *, int, int, int, void *);
static void *refreshcell_hook(void *model, int row, int col, int sel, void *existing) {
    drag_capture_widget(model);
    g_cur_row = row; /* so a waveform lookup made here is attributable */
    {
        static _Atomic int b = 6;
        if (g_rowprobe && __atomic_fetch_sub(&b, 1, __ATOMIC_RELAXED) > 0) {
            FILE *f = azlog_open("/tmp/azmod-drag.log", "a");
            if (f) {
                fprintf(f, "REFRESHCELL row=%d col=%d sel=%d drag=%d\n", row, col, sel,
                        __atomic_load_n(&g_drag_on, __ATOMIC_RELAXED));
                fclose(f);
            }
        }
    }
    if (drag_is_widget((uintptr_t) model - 0x30)) {
        sel = row == __atomic_load_n(&g_drag_target,
                                     __ATOMIC_RELAXED); /* committing too: the plain selection
                                                           (amber is g_row_grabbed's) */
        row = drag_remap(row);
    }
    {
        void *r = g_orig_refreshcell(model, row, col, sel, existing);
        g_cur_row = -1;
        return r;
    }
}

/* Playlist writer: music_library::SqlitePlaylistUpdater, vtable 0x284ab30. Slot +0x50
 * (0x119c058) moves a track:
 *     move(this, uint32 playlist_id, uint32 content_id, uint32 from_seq, uint32 to_seq)
 * Positions are 1-based (it returns early if either is 0); from == to is a no-op.
 * Slot +0x18 inserts a track (run by the stock Tag List -> CREATE PLAYLIST save).
 * A cached instance must have its vptr re-checked against 0x284ab30 before every call:
 * unmounting the stick invalidates it. */
#define PLUPD_VT EP_284ab30






/* Keeping the preview through EP147's own row updates. setRowData 0x1bdb7e0 (row = a DATA
 * row) rewrites ring slot `row` and then pushes it into the column-1 (preview) and column-4
 * (title) components of display row `row` (0x1bdb498 via getCellComponent); the widget's
 * "refresh row" (vtable +0xd8, 0x1bdba60) does the same. EP147's list-diff update calls
 * them asynchronously after every updateContent, so answering with another updateContent
 * loops forever. Instead, while a drag is live and the row lies inside the permuted range,
 * re-run refreshComponentForCell on those two components for the remapped row. Outside the
 * range display row == data row, so the push was already right. The ring write is never
 * touched (it is data-keyed and must stay correct), and a commit in flight is left alone.
 * Message thread. */
#define TLW_GETCELLCOMP EP_1ba2e28                       /* (widget, columnId, row) -> Component* */
#define TLW_REFRESHROW_SLOT (TRACKLIST_WIDGET_VT + 0xd8) /* 0x1bdba60 (widget, row) */
static void drag_repair_row(void *widget, int r) {
    if (!__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED) ||
        __atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED) || !g_orig_refreshcell)
        return;
    int o = __atomic_load_n(&g_drag_origin, __ATOMIC_RELAXED);
    int t = __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED);
    int lo = o < t ? o : t, hi = o < t ? t : o;
    if (r < lo || r > hi || o == t)
        return;
    uintptr_t vt = 0; /* `widget` is the self of the EP147 call this runs inside */
    if (!widget || !own_q((uintptr_t) widget, &vt) || vt != TRACKLIST_WIDGET_VT)
        return;
    void *(*getcell)(void *, int, int) = (void *(*) (void *, int, int) ) TLW_GETCELLCOMP;
    static const int cols[2] = {1, 4};
    for (int i = 0; i < 2; i++) {
        void *comp = getcell(widget, cols[i], r);
        if (!comp)
            continue;
        void *got =
            g_orig_refreshcell((char *) widget + 0x30, drag_remap(r), cols[i], r == t, comp);
        if (got != comp)
            drag_request_update(); /* EP147 built a new one: let juce place it */
    }
}
static void (*g_orig_setrow_drag)(void *, int, const void *);
static void setrow_drag_hook(void *self, int row, const void *src) {
    g_orig_setrow_drag(self, row, src);
    drag_repair_row(self, row);
}
static void (*g_orig_refreshrow)(void *, int);
static void refreshrow_hook(void *self, int row) {
    g_orig_refreshrow(self, row);
    drag_repair_row(self, row);
}
/* Which track / playlist row is selected, on which stick: EP147's own hierarchy
 * (mediaobj_find_hook + lsfind_hook -> plsnap). Shared by Playlist Editing and MAKE STEMS,
 * so neither depends on the other being installed. Installed once. */
static int install_selection_hooks(void) {
    static int done = 0, ok = 0;
    if (done)
        return ok;
    done = 1;
    pe_wref_init();
    int m = azm_install_hook(MEDIAOBJ_FIND, (void *) mediaobj_find_hook,
                             (void **) &g_orig_mediaobj_find);
    int a = azm_install_hook(LISTSTATE_FIND, (void *) lsfind_hook, (void **) &g_orig_lsfind);
    ok = m == 1 && a == 1;
    return ok;
}
static int g_plist_drag_ok; /* the playlist list's cell hook is in: its drag may engage */
/* Playlist Editing at EP147 start, for the hooks installed now. azmod_init loads the
 * settings (load_settings_early) before any hook installs, so this is the row's value as the
 * menu will show it. */
static int pledit_at_start(void) {
    return az_on(AZF_PLAYLIST_EDITING);
}
static void (*g_orig_knobev)(long *, int *);
static void knobev_hook(long *self, int *ev) {
    if (ev && *ev == 2 && !on_msg_thread()) { /* the grab reads widgets: message thread only */
        static int said;
        if (!said++)
            drag_log("LONG PRESS off the message thread: the grab reads nothing here\n");
    }
    if (!ev || *ev != 2 || !__atomic_load_n(&g_reorder_enabled, __ATOMIC_RELAXED)) {
        g_orig_knobev(self, ev);
        return;
    }
    int shift = __atomic_load_n(&g_shift_down, __ATOMIC_RELAXED) != 0;
    int shift_mode = az_on(AZF_PLAYLIST_GRAB) &&
                     az_active(AZF_PLAYLIST_GRAB);
    if (shift != shift_mode) {
        drag_log("LONG PRESS: shift=%d mode=%s -> stock quick scroll\n", shift,
                 shift_mode ? "SHIFT+PRESS" : "PRESS");
        g_orig_knobev(self, ev);
        return;
    }
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED) ||
        __atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED)) {
        drag_log("LONG PRESS: a row is held or saving -> ignored\n");
        return;
    }
    /* Where a drop can commit: a USB playlist's track list in "#" order with EP147's
     * hierarchy agreeing on the selected row, or a playlist/folder row, and only on the
     * list with the blue (knob) selection. Right after an edit the list may not have
     * settled yet; drag_try_grab retries inside its catch-up window. */
    struct plsnap sn = plsnap_get();
    int rows = drag_num_rows(), sel = __atomic_load_n(&g_sel_row_lb, __ATOMIC_RELAXED);
    int tk = list_has_knob(__atomic_load_n(&g_tracklist_widget, __ATOMIC_RELAXED)) == 1;
    int pk = list_has_knob(__atomic_load_n(&g_hl_widget, __ATOMIC_RELAXED)) == 1;
    int hs = __atomic_load_n(&g_hl_sel, __ATOMIC_RELAXED),
        hr = __atomic_load_n(&g_hl_rows, __ATOMIC_RELAXED);
    int recent = drag_now_ms() - __atomic_load_n(&g_edit_done_ms, __ATOMIC_RELAXED) < DRAG_RETRY_MS;
    int plist_base = g_plist_drag_ok && sn.sel_pl && sn.any_media && pk &&
                     __atomic_load_n(&g_hl_widget, __ATOMIC_RELAXED) && hr > 1;
    int kind = -1;
    if (rows > 1 && sn.playlist_id && tk && ((sel >= 0 && (unsigned) sel == sn.row) || recent))
        kind = DRAG_KIND_TRACK;
    else if (plist_base && ((hs >= 0 && (unsigned) hs == sn.sel_row) || recent))
        kind = DRAG_KIND_PLIST;
    drag_log("LONG PRESS rows=%d sel=%d snap pl=%u row=%u | playlists rows=%d sel=%d pl=%u row=%u "
             "| knob: tracks=%d playlists=%d recent=%d -> %s\n",
             rows, sel, sn.playlist_id, sn.row, hr, hs, sn.sel_pl, sn.sel_row, tk, pk, recent,
             kind == DRAG_KIND_TRACK   ? "grab track"
             : kind == DRAG_KIND_PLIST ? "grab playlist"
                                       : "nothing (no quick scroll)");
    if (kind < 0) {
        if (sn.sort_block && tk && __atomic_load_n(&g_tl_showing, __ATOMIC_RELAXED))
            az_banner_strip("Sort by # to move tracks", PL_MSG_READ);
        return;
    }
    __atomic_store_n(&g_knob_kind, kind, __ATOMIC_RELAXED);
    __atomic_store_n(&g_knob_press_ms, drag_now_ms() - DRAG_HOLD_MS,
                     __ATOMIC_RELAXED);                  /* the hold is EP147's, already met */
    __atomic_store_n(&g_knob_held, 1, __ATOMIC_RELEASE); /* drag_try_grab, next tick */
}
/* `v` is the turn handler's own view, or the one g_turn_view kept (a wref slot) */
static int knob_view_ok(uintptr_t v) {
    uintptr_t vt = 0;
    return v && !(v & 7) && own_q(v, &vt) &&
           (vt == EP_29d67a0 /* BrowseView */ || vt == EP_29d4830 /* BrowseListView */ ||
            vt == EP_29dbec8 /* TagListView */);
}
static long (*g_orig_knobturn)(long, long, int);
static _Atomic uintptr_t g_turn_view;
static long knobturn_hook(long self, long a2, int delta) {
    if (__atomic_load_n(&g_drag_on, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_cursor_sync, __ATOMIC_ACQUIRE)) {
        if (on_msg_thread() && knob_view_ok((uintptr_t) self))
            wref_set(&g_turn_view, (uintptr_t) self); /* cleared if the view is destroyed */
        return 1; /* the drag's detent, not EP147's */
    }
    return g_orig_knobturn(self, a2, delta);
}
static void knob_cursor_to(int row) {
    int from = __atomic_exchange_n(&g_cursor_row, -1, __ATOMIC_ACQ_REL), d = row - from;
    uintptr_t v = pe_view_take(&g_turn_view);
    if (from < 0 || !d) {
        drag_log("CURSOR stays on row %d\n", row);
        return;
    }
    if (!knob_view_ok(v)) {
        drag_log("CURSOR NOT MOVED: no live browse view (from %d to %d)\n", from, row);
        return;
    }
    g_orig_knobturn((long) v, 0, d);
    drag_log("CURSOR %d -> %d (EP147's own turn, %+d)\n", from, row, d);
}
/* juce::Component is a virtual base of gui::WidgetBase, so in a complete list widget it sits at
 * the virtual-base offset the primary vtable carries at -24 (TrackListWidget 0x2e0,
 * HierarchyListWidget 0x268 on both firmwares). That is the pointer Component's destructor
 * receives; read it from EP147's own vtable rather than assume it. */
static int pe_vbase_off(uintptr_t vt) {
    uintptr_t v = 0;
    if (!vt || !img_q(vt - 24, &v) || (intptr_t) v <= 0 || v >= 0x1000 || (v & 7))
        return -1;
    return (int) v;
}
/* Constructor phase, before any widget exists: register the live sets and the view slots
 * (ep_mem.h wref). If any registration fails nothing here is ever read through a cached
 * pointer (pe_view / pe_*_live answer unknown), and the features refuse. */
static void pe_wref_init(void) {
    static int done;
    if (done)
        return;
    done = 1;
    int ho = pe_vbase_off(HIER_WIDGET_VT), to = pe_vbase_off(TRACKLIST_WIDGET_VT);
    g_pe_wref_ok = ho > 0 && to > 0 && wref_track(g_pe_hlw, PE_HLW_N, ho) &&
                   wref_track(g_pe_tlw, PE_TLW_N, to) && wref_track(&g_tracklist_widget, 1, to) &&
                   wref_track(&g_sv_obj, 1, 0) &&
                   wref_track(&g_bv_obj, 1, 0) && wref_track(&g_nm_host, 1, 0) &&
                   wref_track(&g_nm_kb, 1, 0) && wref_track(&g_nm_kb_parent, 1, 0) &&
                   wref_track(&g_turn_view, 1, 0);
    drag_log("cached widgets: list Component at +%#x (playlists) +%#x (tracks), tracked=%d\n", ho,
             to, g_pe_wref_ok);
}
/* The renewal handler's deleting destructor (vtable slot +8), which its shared_ptr's release
 * calls: forget the handler first. Only over the expected function. */
static int install_plrenew_del(void) {
    uintptr_t cur = 0;
    if (!PLRENEW_VPTR || !img_q(PLRENEW_VPTR + 8, &cur) || cur < 0x400000 ||
        cur >= PLRENEW_VPTR || (cur & 3))
        return -1;
    g_plrenew_del_ok = azm_patch_vtable(PLRENEW_VPTR + 8, (void *) plrenew_del_hook,
                                        (void **) &g_orig_plrenew_del) == 1;
    return g_plrenew_del_ok ? 1 : -1;
}
static void install_reorder(void) {
    pe_wref_init();
    /* Always installed, so turning Playlist Editing on needs no Reload: the hooks can only
     * go in at EP147 start (the renewal handler's ctor runs then). Off, they are inert:
     * g_reorder_enabled follows the toggle live (the mixer-bus tick) and the MENU popup
     * gates its buttons at use. pledit_at_start() only seeds the first value. */
    __atomic_store_n(&g_reorder_enabled, pledit_at_start(), __ATOMIC_RELAXED);
    /* The browse probe hooks setRowData too; never hook one address twice. */
    int r = AZ_PROBE_ON("/home/root/settings/azmod-browse.log")
                ? 1
                : azm_install_hook(BROWSE_SETROWDATA, (void *) setrow_drag_hook,
                                   (void **) &g_orig_setrow_drag);
    int rr = azm_patch_vtable(TLW_REFRESHROW_SLOT, (void *) refreshrow_hook,
                              (void **) &g_orig_refreshrow);
    int v = install_drag_view();
    int m = install_selection_hooks(), a = m; /* shared with MAKE STEMS */
    int lh = install_library_hooks();         /* shared with the library spool */
    int pd = install_plrenew_del();           /* the renewal handler is never used once freed */
    drag_log("renewal handler delete hook: %d\n", pd);
    if (lh == 1)
        install_pdb_stock(); /* export.pdb follows stock playlist writes, mod toggle or not */
    install_nm_hooks();                       /* NEW PLAYLIST: EP147's keyboard, in Browse */
    int kv = azm_install_hook(KNOBEV_FN, (void *) knobev_hook,
                              (void **) &g_orig_knobev); /* the hold that grabs */
    if (kv == 1)
        kv = azm_install_hook(KNOBTURN_FN, (void *) knobturn_hook,
                              (void **) &g_orig_knobturn); /* turns during a drag */
    int hr =
        install_hier_refresh_slot(); /* the playlist list's drag (shared with the stems mark) */
    g_plist_drag_ok = hr == 1;
    /* the playlist list's landing flash, selection brackets and cut ghost (shared with
     * the stems mark; installed here too so they never depend on that feature) */
    int hl = hr == 1 ? install_hlabel_paint_slot() : -9;
    drag_log("playlist-list drag: refresh slot=%d label paint=%d -> %s\n", hr, hl,
             g_plist_drag_ok ? "on" : "OFF (tracks only)");
    int b = lh, c = lh, d = lh;
    drag_log("reorder gesture ENABLED: view=%d media=%d hierarchy=%d renewal=%d library=%d "
             "msgtick=%d rowdata=%d,%d knob=%d\n",
             v, m, a, b, c, d, r, rr, kv);
    /* A half-installed commit path must not arm: without the library hook a commit would
     * only ever expire, and without the tick nothing would redraw. */
    if (v != 1 || m != 1 || a != 1 || b != 1 || c != 1 || d != 1 || r != 1 || rr != 1 || kv != 1) {
        __atomic_store_n(&g_reorder_enabled, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_reorder_forced_off, 1, __ATOMIC_RELAXED);
        drag_log("reorder gesture DISABLED: an install declined\n");
    } else
        __atomic_store_n(&g_reorder_hooks, 1, __ATOMIC_RELAXED);
}
/* The drag view: row background (amber), paintCell and refreshComponentForCell (the
 * remap), setColour. Installed once, by the reorder gesture or by the row-paint probe;
 * the gesture must never depend on a probe's flag. Returns 1 when every hook took. */
static int install_drag_view(void) {
    static int done = 0, ok = 0;
    if (done)
        return ok;
    done = 1;
    int rc = azm_install_hook(ROWPAINT_ADDR, (void *) rowpaint_hook, (void **) &g_orig_rowpaint);
    int rc2 =
        azm_install_hook(SETCOLOUR_ADDR, (void *) setcolour_hook, (void **) &g_orig_setcolour);
    int rc3 = install_paintcell_slot();
    int rc4 = azm_patch_vtable(REFRESHCELL_SLOT, (void *) refreshcell_hook,
                               (void **) &g_orig_refreshcell);
    /* One install line naming each result, so a declined hook reads as a decline. */
    FILE *f = azlog_open("/tmp/azmod-rowpaint.log", "a");
    if (f) {
        fprintf(f, "install paintRowBackground %#lx rc=%d  setColour %#lx rc=%d\n",
                (unsigned long) ROWPAINT_ADDR, rc, (unsigned long) SETCOLOUR_ADDR, rc2);
        fprintf(f, "install paintCell vtable %#lx rc=%d  refreshCell %#lx rc=%d\n",
                (unsigned long) PAINTCELL_SLOT, rc3, (unsigned long) REFRESHCELL_SLOT, rc4);
        fclose(f);
    }
    ok = (rc == 1 && rc2 == 1 && rc3 == 1 && rc4 == 1);
    return ok;
}
/* ---- Playlist rows: HierarchyListWidget (vt 0x2b79058, the category lists: playlists,
 * artists, ...) has no paintCell; each row is a label component
 * (gui::IconHoldableScrollLabel, vt 0x2b799c0) built in refreshComponentForCell (model vt
 * 0x2b79158 slot 5). Its row cache is a vector at [w+0x248, w+0x250), stride 0x30, the
 * displayed name a string at +0x18. refreshComponentForCell registers label -> (widget,
 * row); the label's paint (slot 26, 0x1bad940) draws after itself when the row's displayed
 * name matches the list's own reply page (kind 8 = playlist), the same identity rule as a
 * track row. juce::Component bounds at +0x20 {x, y, w, h}. */
#define HIER_MODEL_VT EP_2b79158
#define HIER_REFRESH_SLOT (HIER_MODEL_VT + 5 * 8)
#define HLABEL_VT EP_2b799c0
#define HLABEL_PAINT_SLOT (HLABEL_VT + 26 * 8)
#define HLABEL_PAINT_ORIG EP_1bad940
#define SM_HL 48
static struct {
    _Atomic unsigned long comp, w;
    _Atomic int row;
} g_hl[SM_HL]; /* message thread writes */
static struct {
    _Atomic unsigned long w;
    _Atomic int row;
} g_sm_lpend[SM_PROWS]; /* playlist rows waiting for an answer */
static struct {
    _Atomic unsigned long w;
    _Atomic int row;
} g_sm_lanim[SM_PROWS]; /* playlist rows animating (a track in it is stemmed) */
static _Atomic unsigned long
    g_hl_lb_vt; /* the ListBox at [hier widget+0x48], its vptr while painting */
static void *(*g_orig_hier_refresh)(void *, int, int, int, void *);
static void *hier_refresh_hook(void *model, int row, int col, int sel, void *existing) {
    uintptr_t w = (uintptr_t) model - 0x30, vt = 0;
    /* This list is being dragged: ask EP147 for the permuted row (as the track list's
     * refreshcell does), amber only on the target. */
    if (drag_is_widget(w)) {
        sel = row == __atomic_load_n(&g_drag_target,
                                     __ATOMIC_RELAXED); /* committing too: the plain selection
                                                           (amber is g_row_grabbed's) */
        row = drag_remap(row);
    }
    void *c = g_orig_hier_refresh(model, row, col, sel, existing);
    if (!c || !own_q(w, &vt) || vt != HIER_WIDGET_VT) /* EP147 is calling this widget's model */
        return c;
    {
        static unsigned rr;
        pe_set_add(g_pe_hlw, PE_HLW_N, w, &rr); /* alive now: from here only the live set says so */
    }
    __atomic_store_n(&g_hl_seen, (unsigned long) w,
                     __ATOMIC_RELAXED); /* the playlist drag's candidate list */
    int free_i = -1, hit = -1;
    for (int i = 0; i < SM_HL; i++) {
        unsigned long cc = __atomic_load_n(&g_hl[i].comp, __ATOMIC_RELAXED);
        if (cc == (unsigned long) (uintptr_t) c) {
            hit = i;
            break;
        }
        if (!cc && free_i < 0)
            free_i = i;
    }
    static unsigned rr;
    int i = hit >= 0 ? hit : free_i >= 0 ? free_i
                                         : (int) (rr++ % SM_HL);
    __atomic_store_n(&g_hl[i].comp, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hl[i].w, (unsigned long) w, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hl[i].row, row, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hl[i].comp, (unsigned long) (uintptr_t) c, __ATOMIC_RELEASE);
    return c;
}
/* ---- playlist-list drag helpers (Playlist Editing) ------- */
/* Every read through a cached playlist-list widget starts here or at pe_hier_live: the widget
 * must be in the live set (message thread), then its class and ListBox are checked. */
static int plist_widget_ok(uintptr_t w, uintptr_t *lb) {
    uintptr_t vt = 0;
    return w && !(w & 7) && pe_hier_live(w) && own_q(w, &vt) && vt == HIER_WIDGET_VT &&
           own_q(w + 0x48, lb) && *lb >= 0x10000 && !(*lb & 7);
}
/* getNumRows 0x1bad3d8 = `ldr w0, [x0, #0x260]` */
static int plist_rows(uintptr_t w) {
    uintptr_t n = 0, vt = 0;
    if (!w || (w & 7) || !pe_hier_live(w) || !own_q(w, &vt) || vt != HIER_WIDGET_VT ||
        !own_q(w + 0x260, &n))
        return 0;
    int r = (int) (unsigned) (n & 0xffffffffu);
    return r < 0 || r > 4096 ? 0 : r;
}
/* Displayed name of row r from the widget's row cache; the record's u32 +0x00 must equal r. */
static int plist_name_at(uintptr_t w, int r, char *out, size_t osz) {
    uintptr_t vb = 0, ve = 0, np = 0, iq = 0;
    out[0] = 0;
    if (r < 0 || !pe_hier_live(w) || !own_q(w + 0x248, &vb) || !own_q(w + 0x250, &ve) || !vb ||
        ve <= vb || (ve - vb) % 0x30 || (uintptr_t) r >= (ve - vb) / 0x30)
        return 0;
    if (!own_q(vb + (uintptr_t) r * 0x30, &iq) || (unsigned) (iq & 0xffffffffu) != (unsigned) r)
        return 0;
    return own_q(vb + (uintptr_t) r * 0x30 + 0x18, &np) && np >= 0x10000 &&
           own_str(np, out, (int) osz) && out[0];
}
/* Settle: does the dragged list already show the new order? Read where the item landed: a
 * playlist row's name in the widget's row cache, a track row's title in EP147's row ring
 * (as sm_after_paint reads it); dropped into a folder, the row count drops. Any doubt
 * answers no (the preview stays; msg_tick's timeout ends it). */
static int drag_data_new(void) {
    uintptr_t w = __atomic_load_n(&g_drag_widget, __ATOMIC_RELAXED);
    int t = __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED);
    if (!w || t < 0 || !g_grab.sel_title[0])
        return 0;
    char nm[160];
    if (__atomic_load_n(&g_drag_kind, __ATOMIC_RELAXED) == DRAG_KIND_PLIST) {
        uintptr_t lb = 0;
        if (!plist_widget_ok(w, &lb))
            return 0;
        if (__atomic_load_n(&g_drag_into, __ATOMIC_RELAXED) >= 0)
            return plist_rows(w) == __atomic_load_n(&g_drag_rows, __ATOMIC_RELAXED) - 1;
        if (!plist_name_at(w, t, nm, sizeof nm))
            return 0;
    } else {
        uintptr_t vt = 0, ring = 0, slot, v = 0, tp = 0;
        if ((w & 7) || !pe_track_live(w) || !own_q(w, &vt) || vt != TRACKLIST_WIDGET_VT ||
            !own_q(w + 0x2a8, &ring) || !ring)
            return 0;
        slot = ring + (uintptr_t) (t % 0x1b) * 0x78;
        if (!own_q(slot, &v) || (int) (v & 0xffffffff) != t)
            return 0;
        if (!own_q(slot + 0x70, &v) || (v & 0xff))
            return 0;
        if (!own_q(slot + 0x28, &tp) || tp < 0x10000 || !own_str(tp, nm, sizeof nm))
            return 0;
    }
    size_t gl = strlen(g_grab.sel_title); /* a capped copy: a full one is a prefix */
    return gl >= sizeof g_grab.sel_title - 1 ? !strncmp(nm, g_grab.sel_title, gl)
                                             : !strcmp(nm, g_grab.sel_title);
}
static int mk_row_is(uintptr_t w,
                     int r) { /* row r = the marked playlist's name between its neighbors */
    char a[64], b[64];
    if (!plist_name_at(w, r, a, sizeof a) || strcmp(a, g_mk.name))
        return 0;
    if (r > 0 ? !plist_name_at(w, r - 1, b, sizeof b) || strcmp(b, g_mk.prev) : g_mk.prev[0] != 0)
        return 0;
    if (r + 1 < g_mk.rows ? !plist_name_at(w, r + 1, b, sizeof b) || strcmp(b, g_mk.next)
                          : g_mk.next[0] != 0)
        return 0;
    return 1;
}
/* The mark changed row or went away: repaint the rows it was / is on, or the old amber edge
 * stays until that row happens to redraw. */
static void mk_set(uintptr_t w, int row) {
    uintptr_t ow = __atomic_load_n(&g_mk_w, __ATOMIC_RELAXED);
    int orow = __atomic_load_n(&g_mk_row, __ATOMIC_RELAXED);
    static unsigned flpl; /* the playlist that last glowed: once per mark, not per re-find */
    if (!g_mk.on)
        flpl = 0;
    else if (w && row >= 0 && g_mk.pl != flpl) {
        flpl = g_mk.pl;
        fl_start(w, row, 1);
    } /* newly marked: it glows once */
    __atomic_store_n(&g_mk_row, row, __ATOMIC_RELAXED);
    __atomic_store_n(&g_mk_w, (unsigned long) w, __ATOMIC_RELEASE);
    if (ow == w && orow == row)
        return;
    /* ListBox::repaintRow, not Component::repaint on the list: each playlist row's label is
     * cached as an image, so a list-wide repaint redraws the backgrounds but reuses the
     * stale label. */
    uintptr_t lb = 0;
    void (*repaint_row)(void *, int) = (void (*)(void *, int)) EP_21e7540; /* LISTBOX_REPAINTROW */
    if (ow && orow >= 0 && plist_widget_ok(ow, &lb))
        repaint_row((void *) lb, orow);
    if (w && row >= 0 && plist_widget_ok(w, &lb))
        repaint_row((void *) lb, row);
}
/* The display row of the selected playlist in the list on screen, or none. Rescans only
 * when that list's row cache or the selection changes. Message thread. */
static void mk_tick(void) {
    static uintptr_t kw, kvb, kve;
    static unsigned kpl;
    static int krows;
    struct pl_addtgt *t = g_mk.on ? pl_target_for(g_mk.media) : 0;
    if (!t || t->pl != g_mk.pl) {
        if (g_mk.on)
            memset(&g_mk, 0, sizeof g_mk);
        mk_set(0, -1);
        kw = 0;
        return;
    }
    /* the list to look in: every hierarchy list azmod has seen cells for, showing */
    uintptr_t cand[SM_HL + 1];
    int nc = 0;
    cand[nc++] = __atomic_load_n(&g_hl_seen, __ATOMIC_RELAXED);
    for (int i = 0; i < SM_HL; i++) {
        uintptr_t cw = __atomic_load_n(&g_hl[i].w, __ATOMIC_RELAXED);
        int dup = !cw;
        for (int j = 0; j < nc && !dup; j++)
            dup = cand[j] == cw;
        if (!dup)
            cand[nc++] = cw;
    }
    uintptr_t hw = 0;
    int hr = -1, hits = 0;
    for (int i = 0; i < nc; i++) {
        uintptr_t l = 0, vb = 0, ve = 0;
        if (!plist_widget_ok(cand[i], &l) || !lb_showing(l) || plist_rows(cand[i]) != g_mk.rows)
            continue;
        if (!own_q(cand[i] + 0x248, &vb) || !own_q(cand[i] + 0x250, &ve)) /* live: checked above */
            continue;
        /* Same cache arrays is not the same content: EP147 rewrites the records in place on a
         * reorder, so the kept row is re-proved every tick (three reads) and rescanned if it
         * fails. */
        int kr = __atomic_load_n(&g_mk_row, __ATOMIC_RELAXED);
        if (cand[i] == kw && vb == kvb && ve == kve && kpl == g_mk.pl && krows == g_mk.rows &&
            kr >= 0 && mk_row_is(cand[i], kr)) {
            hw = kw;
            hr = kr;
            hits++;
            continue;
        }
        int n = (int) ((ve - vb) / 0x30);
        for (int r = 0; r < n && r < PLO_MAXROWS; r++)
            if (mk_row_is(cand[i], r)) {
                hw = cand[i];
                hr = r;
                hits++;
            }
        kw = cand[i];
        kvb = vb;
        kve = ve;
        kpl = g_mk.pl;
        krows = g_mk.rows;
    }
    if (hits != 1) {
        hw = 0;
        hr = -1;
    } /* two candidates = unknown: no mark */
    /* Not found (e.g. the list changed around it): ask the DB where it is now, once
     * per change of the list on screen. */
    static uintptr_t ask_w;
    static int ask_rows;
    static long long ask_after;
    if (hits == 0) {
        uintptr_t sw = __atomic_load_n(&g_hl_seen, __ATOMIC_RELAXED);
        int srows = sw ? plist_rows(sw) : 0;
        long long now = drag_now_ms();
        if (sw && (sw != ask_w || srows != ask_rows) && now > ask_after &&
            __atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) == MV_IDLE &&
            !__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED)) {
            ask_w = sw;
            ask_rows = srows;
            ask_after = now + 1500;
            g_mv.op = MV_OP_MKREAD;
            g_mv.pl = g_mk.pl;
            g_mv.media = g_mk.media;
            g_mv.cid = 0;
            g_mv.from = g_mv.to = 0;
            g_mv.name[0] = 0;
            g_mv.deadline = now + MV_DEADLINE_MS;
            g_mv.rc = 0;
            __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
            __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
            kw = 0; /* rescan once the answer is in */
        }
    }
    mk_set(hw, hr);
}
/* the cut playlist row's fingerprint, taken from the highlighted row when CUT is pressed */
static void cg_capture(void) {
    memset(&g_cg, 0, sizeof g_cg);
    if (!g_clip.move || (g_clip.kind != CLIP_LIST && g_clip.kind != CLIP_FOLDER))
        return;
    uintptr_t w = __atomic_load_n(&g_hl_widget, __ATOMIC_ACQUIRE);
    int sel = __atomic_load_n(&g_hl_sel, __ATOMIC_RELAXED),
        rows = __atomic_load_n(&g_hl_rows, __ATOMIC_RELAXED);
    struct plsnap sn = plsnap_get();
    if (!w || sel < 0 || sn.sel_pl != g_clip.id || sn.any_media != g_clip.media)
        return;
    if (!plist_name_at(w, sel, g_cg.name, sizeof g_cg.name) ||
        (sel > 0 && !plist_name_at(w, sel - 1, g_cg.prev, sizeof g_cg.prev)) ||
        (sel + 1 < rows && !plist_name_at(w, sel + 1, g_cg.next, sizeof g_cg.next)))
        return;
    g_cg.id = g_clip.id;
    g_cg.media = g_clip.media;
    g_cg.rows = rows;
    g_cg.on = 1;
}
static int cg_row_is(uintptr_t w, int r) {
    char a[64], b[64];
    if (!plist_name_at(w, r, a, sizeof a) || strcmp(a, g_cg.name))
        return 0;
    if (r > 0 ? !plist_name_at(w, r - 1, b, sizeof b) || strcmp(b, g_cg.prev) : g_cg.prev[0] != 0)
        return 0;
    if (r + 1 < g_cg.rows ? !plist_name_at(w, r + 1, b, sizeof b) || strcmp(b, g_cg.next)
                          : g_cg.next[0] != 0)
        return 0;
    return 1;
}
/* message thread: where the cut playlist row is now (exactly one match, else none),
 * and repaint whatever showed a ghost when it moves or ends */
static void cg_tick(void) {
    uintptr_t hw = 0;
    int hr = -1;
    int live = g_cg.on && g_clip.move && g_clip.media == g_cg.media && g_clip.id == g_cg.id &&
               (g_clip.kind == CLIP_LIST || g_clip.kind == CLIP_FOLDER);
    if (!live && g_cg.on)
        memset(&g_cg, 0, sizeof g_cg);
    if (live) {
        static uintptr_t kw;
        static int kr = -1;
        uintptr_t cand[SM_HL + 1];
        int nc = 0, hits = 0;
        cand[nc++] = __atomic_load_n(&g_hl_seen, __ATOMIC_RELAXED);
        for (int i = 0; i < SM_HL; i++) {
            uintptr_t cw = __atomic_load_n(&g_hl[i].w, __ATOMIC_RELAXED);
            int dup = !cw;
            for (int j = 0; j < nc && !dup; j++)
                dup = cand[j] == cw;
            if (!dup)
                cand[nc++] = cw;
        }
        for (int i = 0; i < nc; i++) {
            uintptr_t l = 0;
            if (!plist_widget_ok(cand[i], &l) || !lb_showing(l) || plist_rows(cand[i]) != g_cg.rows)
                continue;
            if (cand[i] == kw && kr >= 0 && cg_row_is(cand[i], kr)) {
                hw = kw;
                hr = kr;
                hits++;
                continue;
            }
            int n = plist_rows(cand[i]);
            for (int r = 0; r < n && r < PLO_MAXROWS; r++)
                if (cg_row_is(cand[i], r)) {
                    hw = cand[i];
                    hr = r;
                    hits++;
                }
        }
        if (hits != 1) {
            hw = 0;
            hr = -1;
        } /* two candidates = unknown: no ghost */
        kw = hw;
        kr = hr;
    }
    uintptr_t ow = __atomic_load_n(&g_cg_w, __ATOMIC_RELAXED);
    int orow = __atomic_load_n(&g_cg_row, __ATOMIC_RELAXED);
    if (ow != hw || orow != hr) {
        __atomic_store_n(&g_cg_row, hr, __ATOMIC_RELAXED);
        __atomic_store_n(&g_cg_w, (unsigned long) hw, __ATOMIC_RELEASE);
        uintptr_t lb = 0;
        if (ow && plist_widget_ok(ow, &lb))
            ((void (*)(uintptr_t)) EP_2488570)(lb); /* juce::Component::repaint() */
        if (hw && hw != ow && plist_widget_ok(hw, &lb))
            ((void (*)(uintptr_t)) EP_2488570)(lb);
    }
    /* a track ghost ended (pasted, cleared): repaint the list that drew it */
    uintptr_t tw = __atomic_load_n(&g_cg_tw, __ATOMIC_ACQUIRE);
    if (tw && !(g_clip.kind == CLIP_TRACK && g_clip.move)) {
        uintptr_t vt = 0, lb = 0;
        __atomic_store_n(&g_cg_tw, 0, __ATOMIC_RELEASE);
        if (!(tw & 7) && pe_track_live(tw) && own_q(tw, &vt) && vt == TRACKLIST_WIDGET_VT &&
            own_q(tw + 0x48, &lb) && lb && !(lb & 7))
            ((void (*)(uintptr_t)) EP_2488570)(lb);
    }
}
/* Paint thread: is row `row` of track list tw the CUT track? All of: the clipboard holds a
 * cut track, this list shows the playlist it was cut from (sorted by #, which CUT required),
 * the row is its cut position, and EP147's row cache shows its title there. Any doubt: no. */
static int cg_track_row(uintptr_t tw, int row) {
    if (g_clip.kind != CLIP_TRACK || !g_clip.move || !g_clip.src_seq ||
        row != (int) g_clip.src_seq - 1)
        return 0;
    /* `tw` is the widget whose row painter is running (rowpaint_hook's self - 0x38) */
    uintptr_t vt = 0, ring = 0, slot, v = 0, tp = 0;
    char tt[160];
    if (!own_q(tw, &vt) || vt != TRACKLIST_WIDGET_VT)
        return 0;
    struct plsnap sn = plsnap_get();
    if (sn.playlist_id != g_clip.src_pl || sn.any_media != g_clip.media)
        return 0;
    if (!own_q(tw + 0x2a8, &ring) || !ring)
        return 0;
    slot = ring + (uintptr_t) (row % 0x1b) * 0x78;
    if (!own_q(slot, &v) || (int) (v & 0xffffffff) != row)
        return 0;
    if (!own_q(slot + 0x70, &v) || (v & 0xff))
        return 0;
    if (!own_q(slot + 0x28, &tp) || tp < 0x10000 || !own_str(tp, tt, sizeof tt))
        return 0;
    size_t cl = strlen(g_clip.name); /* the clipboard keeps a capped copy: a full one is a prefix */
    if (!cl ||
        (cl >= sizeof g_clip.name - 1 ? strncmp(tt, g_clip.name, cl) : strcmp(tt, g_clip.name)))
        return 0;
    __atomic_store_n(&g_cg_tw, (unsigned long) tw, __ATOMIC_RELEASE);
    return 1;
}
/* Message thread, 20 Hz while Playlist Editing is on: the hierarchy list that is on screen
 * and holds the highlighted playlist/folder row, and its JUCE selection, copied for the bus
 * thread. One list or none, and only on the PLAYLIST category (artists etc. are the same
 * widget class and are never grabbed). Not necessarily the last list EP147 built a cell for:
 * with a folder highlighted, the right pane previews its children in a second
 * HierarchyListWidget with no selection. So every showing candidate is asked, and exactly
 * one must have its selected row == plsnap's row. */
static void plist_tick(void) {
    static long long next;
    long long now = drag_now_ms();
    if (now < next)
        return;
    next = now + 50;
    uintptr_t w = 0, lb = 0;
    struct plsnap sn = plsnap_get();
    int ok = 0;
    if (sn.sel_pl && sn.any_media) {
        uintptr_t cand[SM_HL + 1];
        int nc = 0, hits = 0;
        cand[nc++] = __atomic_load_n(&g_hl_seen, __ATOMIC_RELAXED);
        for (int i = 0; i < SM_HL; i++) {
            uintptr_t cw = __atomic_load_n(&g_hl[i].w, __ATOMIC_RELAXED);
            int dup = !cw;
            for (int j = 0; j < nc && !dup; j++)
                dup = cand[j] == cw;
            if (!dup)
                cand[nc++] = cw;
        }
        for (int i = 0; i < nc; i++) {
            uintptr_t l = 0;
            if (!plist_widget_ok(cand[i], &l) || !lb_showing(l))
                continue;
            int s = ((int (*)(void *, int)) LISTBOX_GETSELECTED)((void *) l, 0);
            if (s >= 0 && (unsigned) s == sn.sel_row) {
                w = cand[i];
                lb = l;
                hits++;
            }
        }
        ok = hits == 1;
    }
    __atomic_store_n(&g_hl_rows, ok ? plist_rows(w) : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hl_sel,
                     ok ? ((int (*)(void *, int)) LISTBOX_GETSELECTED)((void *) lb, 0) : -1,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_hl_widget, ok ? (unsigned long) w : 0, __ATOMIC_RELEASE);
    mk_tick();
    cg_tick();
}
/* Every displayed name, in display order, from the widget's own row cache ([w+0x248,
 * w+0x250), stride 0x30, juce::String at +0x18). The library thread requires the DB's
 * sibling order to match this list before it writes: that proves "display row r =
 * sequenceNo r+1". */
static int plist_snapshot_names(uintptr_t w, int rows, int must) {
    uintptr_t vb = 0, ve = 0, np = 0;
    g_plo_n = 0;
    if (!pe_hier_live(w)) /* a live playlist list, message thread */
        return 0;
    if (AZ_PROBE_ON("/tmp/azmod-plodump") && own_q(w + 0x248, &vb) && own_q(w + 0x250, &ve) &&
        vb && ve > vb) {
        FILE *f =
            azlog_open("/tmp/azmod-plodump.log", "w"); /* probe: the raw cache, before any check */
        if (f)
            fprintf(f, "rows=%d cache=%ld\n", rows, (long) ((ve - vb) / 0x30));
        for (uintptr_t r = 0; f && r < (ve - vb) / 0x30 && r < 64; r++) {
            char nm[64] = "";
            uintptr_t q = 0;
            if (own_q(vb + r * 0x30 + 0x18, &q) && q >= 0x10000)
                own_str(q, nm, sizeof nm);
            fprintf(f, "%2lu %-24s", (unsigned long) r, nm);
            for (int k = 0; k < 0x30; k += 8) {
                q = 0;
                own_q(vb + r * 0x30 + k, &q);
                fprintf(f, " %016lx", (unsigned long) q);
            }
            fputc('\n', f);
        }
        if (f)
            fclose(f);
    }
    if (rows < 2 || rows > PLO_MAXROWS || !own_q(w + 0x248, &vb) || !own_q(w + 0x250, &ve) ||
        !vb || ve <= vb || (ve - vb) % 0x30 || (int) ((ve - vb) / 0x30) > rows)
        return 0;
    /* The cache is a prefix that grows as EP147 loads rows (a long folder may hold only its
     * first rows at the grab). Rows past it are unknown, like an unloaded name; each
     * record's u32 +0x00 is its row index, checked. */
    int nc = (int) ((ve - vb) / 0x30);
    for (int r = 0; r < rows; r++) {
        g_plo_names[r][0] = 0;
        g_plo_have[r] = 0;
        g_plo_folder[r] = 0;
        uintptr_t iq = 0;
        if (r >= nc || !own_q(vb + (uintptr_t) r * 0x30, &iq) ||
            (unsigned) (iq & 0xffffffffu) != (unsigned) r)
            continue;
        if (!own_q(vb + (uintptr_t) r * 0x30 + 0x18, &np) || np < 0x10000 ||
            !own_str(np, g_plo_names[r], sizeof g_plo_names[r]) || !g_plo_names[r][0])
            continue;     /* not loaded yet */
        uintptr_t kq = 0; /* +0x10 u32: 1 folder, 0x0f list */
        g_plo_folder[r] =
            own_q(vb + (uintptr_t) r * 0x30 + 0x10, &kq) && (unsigned) (kq & 0xffffffffu) == 1;
        g_plo_have[r] = 1;
    }
    /* The grabbed row's name need not be loaded. Its identity is proven where it matters: the
     * grab re-checks EP147's selection (sel == the hierarchy's row), and the commit writes
     * only if the DB's id at that position is the grabbed playlist (mv_plorder:
     * ids[from-1] == g_mv.pl); every known name is still compared there. */
    if (must < 0 || must >= rows)
        return 0;
    if (!g_plo_have[must])
        drag_log("GRAB playlist row %d: name not loaded yet (cache %d of %d); identity by id at "
                 "commit\n",
                 must, nc, rows);
    __atomic_store_n(&g_drag_into, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_drag_into_disp, -1, __ATOMIC_RELAXED);
    g_plo_n = rows;
    if (AZ_PROBE_ON("/tmp/azmod-plodump")) { /* probe: the raw row records, to learn their fields */
        FILE *f = azlog_open("/tmp/azmod-plodump.log", "w");
        for (int r = 0; f && r < rows; r++) {
            fprintf(f, "%2d %-24s", r, g_plo_names[r]);
            for (int k = 0; k < 0x30; k += 8) {
                uintptr_t q = 0;
                own_q(vb + (uintptr_t) r * 0x30 + k, &q);
                fprintf(f, " %016lx", (unsigned long) q);
            }
            fputc('\n', f);
        }
        if (f)
            fclose(f);
    }
    return 1;
}
/* While dragging (message thread): pick up names EP147 has loaded since the grab.
 * A known row whose name changed = the list changed under us: 0, the caller cancels. */
static int plist_refresh_names(uintptr_t w) {
    uintptr_t vb = 0, ve = 0, np = 0;
    if (!pe_hier_live(w) || !own_q(w + 0x248, &vb) || !own_q(w + 0x250, &ve) || !vb || ve <= vb || (ve - vb) % 0x30 ||
        (int) ((ve - vb) / 0x30) > g_plo_n)
        return 0;
    int nc = (int) ((ve - vb) / 0x30); /* a growing prefix, as at the grab */
    for (int r = 0; r < nc; r++) {
        char nm[PL_TGT_NAME_MAX] = "";
        uintptr_t iq = 0;
        if (!own_q(vb + (uintptr_t) r * 0x30, &iq) ||
            (unsigned) (iq & 0xffffffffu) != (unsigned) r)
            continue;
        if (!own_q(vb + (uintptr_t) r * 0x30 + 0x18, &np) || np < 0x10000 ||
            !own_str(np, nm, sizeof nm) || !nm[0])
            continue;
        if (g_plo_have[r]) {
            if (strcmp(nm, g_plo_names[r]))
                return 0;
            continue;
        }
        uintptr_t kq = 0;
        snprintf(g_plo_names[r], sizeof g_plo_names[r], "%s", nm);
        g_plo_folder[r] =
            own_q(vb + (uintptr_t) r * 0x30 + 0x10, &kq) && (unsigned) (kq & 0xffffffffu) == 1;
        __atomic_store_n(&g_plo_have[r], 1, __ATOMIC_RELEASE);
    }
    return 1;
}
static void (*g_orig_hlabel_paint)(void *, void *);
/* The label's paint, on the message thread: `self` (the label) and its parents are live for the
 * paint; a registered widget is read only while it is in the live set (pe_hier_live). */
static void hlabel_paint_hook(void *self, void *g) {
    /* A track title that gets a stems mark has its text clipped short of the mark, so a long
     * title ends before it instead of running under it. The text is a gui::ScrollLabel
     * child (x 45 in the label) painted after this, so the clip is handed to that child's
     * paint (sltext_paint_hook) for this one frame. */
    struct sm_tprep tp = {0};
    int tm = sm_shown() ? sm_title_prep(self, &tp) : 0;
    g_tclip_label = tm == 2 ? (uintptr_t) self : 0;
    g_tclip_x = tp.x0 - 6;
    g_orig_hlabel_paint(self, g);
    /* Landing flash on the playlist list: each row's label paints its own opaque background
     * over the row background rowpaint_hook drew, so the glow goes here, over the label,
     * fading out over FL_MS. */
    {
        unsigned long fw = __atomic_load_n(&g_fl.w, __ATOMIC_ACQUIRE);
        if (fw && g_fl.plist && g_orig_setcolour) {
            uintptr_t w0 = 0;
            int r0 = -1;
            for (int i = 0; i < SM_HL; i++)
                if (__atomic_load_n(&g_hl[i].comp, __ATOMIC_ACQUIRE) ==
                    (unsigned long) (uintptr_t) self) {
                    w0 = __atomic_load_n(&g_hl[i].w, __ATOMIC_RELAXED);
                    r0 = __atomic_load_n(&g_hl[i].row, __ATOMIC_RELAXED);
                    break;
                }
            long long e = drag_now_ms() - g_fl.t0;
            uintptr_t lb = 0, p = (uintptr_t) self, bb = 0;
            int own = 0;
            if (w0 == fw &&
                r0 == (drag_is_widget(w0) ? drag_remap(g_fl.row)
                                          : g_fl.row) /* g_hl rows are model rows */
                && e >= 0 && e < FL_MS && pe_hier_live(w0) && own_q(w0 + 0x48, &lb) && lb)
                for (int d = 0; d < 6 && p; d++) {
                    if (!own_q(p + 0x18, &p))
                        break;
                    if (p == lb) {
                        own = 1;
                        break;
                    }
                }
            if (own && own_q((uintptr_t) self + 0x28, &bb)) {
                int lw = (int) (bb & 0xffffffff), lh = (int) (bb >> 32);
                if (lw > 0 && lh > 0 && lw < 4000 && lh < 400) {
                    unsigned char c[8] = {ROW_GRAB_B, ROW_GRAB_G, ROW_GRAB_R, fl_alpha(140, e),
                                          0, 0, 0, 0};
                    g_orig_setcolour(g, c);
                    g_juce_fillrect(g, 0, 0, lw, lh);
                }
            }
        }
    }
    { /* Selected playlist's brackets, drawn over the label too: labels EP147 rebuilds after a
       * navigation cover the row painter's edges. Same geometry as the row painter's, in
       * the label's own bounds. */
        unsigned long mw = __atomic_load_n(&g_mk_w, __ATOMIC_ACQUIRE);
        if (mw && g_orig_setcolour) {
            uintptr_t w0 = 0, lb = 0, p = (uintptr_t) self, bb = 0;
            int r0 = -1, own = 0;
            for (int i = 0; i < SM_HL; i++)
                if (__atomic_load_n(&g_hl[i].comp, __ATOMIC_ACQUIRE) ==
                    (unsigned long) (uintptr_t) self) {
                    w0 = __atomic_load_n(&g_hl[i].w, __ATOMIC_RELAXED);
                    r0 = __atomic_load_n(&g_hl[i].row, __ATOMIC_RELAXED);
                    break;
                }
            if (w0 == mw && r0 == __atomic_load_n(&g_mk_row, __ATOMIC_RELAXED) &&
                !drag_is_widget(w0) && pe_hier_live(w0) && own_q(w0 + 0x48, &lb) && lb)
                for (int d = 0; d < 6 && p; d++) {
                    if (!own_q(p + 0x18, &p))
                        break;
                    if (p == lb) {
                        own = 1;
                        break;
                    }
                }
            if (own && own_q((uintptr_t) self + 0x28, &bb)) {
                int lw = (int) (bb & 0xffffffff), lh = (int) (bb >> 32);
                if (lw > 40 && lh > 16 && lw < 4000 && lh < 400) {
                    unsigned char c[8] = {ROW_GRAB_B, ROW_GRAB_G, ROW_GRAB_R, 0xff, 0, 0, 0, 0};
                    g_orig_setcolour(g, c);
                    int t = 4, arm = 14, in = 4, y0 = 4, y1 = lh - 5;
                    g_juce_fillrect(g, in, y0, t, y1 - y0); /* [ */
                    g_juce_fillrect(g, in, y0, arm, t);
                    g_juce_fillrect(g, in, y1 - t, arm, t);
                    g_juce_fillrect(g, lw - in - t, y0, t, y1 - y0); /* ] */
                    g_juce_fillrect(g, lw - in - arm, y0, arm, t);
                    g_juce_fillrect(g, lw - in - arm, y1 - t, arm, t);
                }
            }
        }
    }
    { /* cut ghost on the playlist list: over the label, like the flash */
        unsigned long cw = __atomic_load_n(&g_cg_w, __ATOMIC_ACQUIRE);
        if (cw && g_orig_setcolour) {
            uintptr_t w0 = 0, lb = 0, p = (uintptr_t) self, bb = 0;
            int r0 = -1, own = 0;
            for (int i = 0; i < SM_HL; i++)
                if (__atomic_load_n(&g_hl[i].comp, __ATOMIC_ACQUIRE) ==
                    (unsigned long) (uintptr_t) self) {
                    w0 = __atomic_load_n(&g_hl[i].w, __ATOMIC_RELAXED);
                    r0 = __atomic_load_n(&g_hl[i].row, __ATOMIC_RELAXED);
                    break;
                }
            if (w0 == cw && r0 == __atomic_load_n(&g_cg_row, __ATOMIC_RELAXED) &&
                pe_hier_live(w0) && own_q(w0 + 0x48, &lb) && lb)
                for (int d = 0; d < 6 && p; d++) {
                    if (!own_q(p + 0x18, &p))
                        break;
                    if (p == lb) {
                        own = 1;
                        break;
                    }
                }
            if (own && own_q((uintptr_t) self + 0x28, &bb)) {
                int lw = (int) (bb & 0xffffffff), lh = (int) (bb >> 32);
                if (lw > 40 && lh > 8 && lw < 4000 && lh < 400)
                    cg_outline(g, lw, lh);
            }
        }
    }
    if (tm) { /* a track title (the two-pane track pane) */
        if (tm == 2)
            sm_mark_row(tp.e, tp.w, g, tp.row, tp.x0, tp.y0);
        return;
    }
    if (!sm_shown())
        return;
    static int logs = 60;
    int why = 0;
#define HL_OUT(n)  \
    do {           \
        why = (n); \
        goto out;  \
    } while (0)
    uintptr_t w = 0;
    int row = -1;
    for (int i = 0; i < SM_HL; i++)
        if (__atomic_load_n(&g_hl[i].comp, __ATOMIC_ACQUIRE) == (unsigned long) (uintptr_t) self) {
            w = __atomic_load_n(&g_hl[i].w, __ATOMIC_RELAXED);
            row = __atomic_load_n(&g_hl[i].row, __ATOMIC_RELAXED);
            break;
        }
    uintptr_t vt = 0, vb = 0, ve = 0, np = 0, b1 = 0;
    char name[160] = "";
    unsigned kind = 0, id = 0;
    int st = 0;
    if (!w || row < 0)
        HL_OUT(1); /* not a registered playlist-list label */
    if (!pe_hier_live(w) || !own_q(w, &vt) || vt != HIER_WIDGET_VT)
        HL_OUT(2);
    { /* Ownership, not only class: a registered label can be freed and a new label of the same
       * class (a track title in the other pane) allocated at the same address, passing any
       * vtable check. The label must still sit under this widget's ListBox (juce
       * parentComponent at +0x18, walked up a few levels). */
        uintptr_t lb = 0, p = (uintptr_t) self;
        int ok = 0;
        if (own_q(w + 0x48, &lb) && lb)
            for (int d = 0; d < 6 && p; d++) {
                if (!own_q(p + 0x18, &p))
                    break;
                if (p == lb) {
                    ok = 1;
                    break;
                }
            }
        if (!ok)
            HL_OUT(11);
        hlw_live_note(w); /* stem_marks.c: alive now, it is painting this label */
    }
    if (!own_q(w + 0x248, &vb) || !own_q(w + 0x250, &ve) || !vb || ve <= vb || (ve - vb) % 0x30 ||
        (unsigned) row >= (ve - vb) / 0x30)
        HL_OUT(3);
    if (!own_q(vb + (uintptr_t) row * 0x30 + 0x18, &np) || np < 0x10000 ||
        !own_str(np, name, sizeof name) || !name[0])
        HL_OUT(4);
    if (!sm_lookup_pl(row, name, &id))
        HL_OUT(5);
    kind = 8;
    if (kind != 8 || !id || (id & SM_PLF))
        HL_OUT(6); /* a PLAYLIST row, name-checked */
    unsigned media =
        plsnap_get().any_media; /* the stick being browsed, from EP147's own hierarchy */
    if (!media)
        HL_OUT(10);
    int await = 0, busy = 0;
    st = sm_status_mb(id | SM_PLF, name, media, &await, &busy);
    if (!__atomic_load_n(&g_rs_on, __ATOMIC_RELAXED))
        busy = 0; /* Remote Stems off: no animation (the track rows' gate) */
    {
        uintptr_t lb = 0, lbvt = 0; /* the widget is alive: it is painting this row */
        if (own_q(w + 0x48, &lb) && lb && !(lb & 7) && own_q(lb, &lbvt) && lbvt)
            __atomic_store_n(&g_hl_lb_vt, (unsigned long) lbvt, __ATOMIC_RELEASE);
    }
    if (await) { /* repaint this row when its answer lands (ListBox::repaintRow; repainting the
                  * label itself re-runs nothing) */
        int have = 0, freei = -1;
        for (int i = 0; i < SM_PROWS; i++) {
            unsigned long x = __atomic_load_n(&g_sm_lpend[i].w, __ATOMIC_RELAXED);
            if (x == w && __atomic_load_n(&g_sm_lpend[i].row, __ATOMIC_RELAXED) == row) {
                have = 1;
                break;
            }
            if (!x && freei < 0)
                freei = i;
        }
        if (!have && freei >= 0) {
            __atomic_store_n(&g_sm_lpend[freei].row, row, __ATOMIC_RELAXED);
            __atomic_store_n(&g_sm_lpend[freei].w, (unsigned long) w, __ATOMIC_RELEASE);
        }
    }
    if (!busy && ((st != SM_ST_STEMS && st != SM_ST_PART) || !sm_marks()))
        HL_OUT(7); /* marks off: animation only */
    if (!own_q((uintptr_t) self + 0x28, &b1))
        HL_OUT(8);
    int lw = (int) (b1 & 0xffffffff), lh = (int) (b1 >> 32);
    if (lw < 60 || lh < 30 || lw > 4000 || lh > 400)
        HL_OUT(9);
    /* the track rows' mark, at the right end of the row: the full mark when any track in
     * the playlist has stems (no separate partial mark) */
    if (busy) { /* a track in it is being worked on (2): the stemming animation; only waiting (1):
                   dim, still */
        long long t = drag_now_ms();
        smk_draw(g, smk_anim(busy >= 2 ? SM_PH_ST : SM_PH_WT, t), lw - SMK_INSET_PLAYLIST - SMK_W,
                 (lh - SMK_H) / 2);
        __atomic_store_n(&g_sm_anim_ms, t, __ATOMIC_RELAXED);
        int have = 0, freei = -1; /* sm_tick repaints it while it keeps painting busy */
        for (int i = 0; i < SM_PROWS; i++) {
            unsigned long x = __atomic_load_n(&g_sm_lanim[i].w, __ATOMIC_RELAXED);
            if (x == w && __atomic_load_n(&g_sm_lanim[i].row, __ATOMIC_RELAXED) == row) {
                have = 1;
                break;
            }
            if (!x && freei < 0)
                freei = i;
        }
        if (!have && freei >= 0) {
            __atomic_store_n(&g_sm_lanim[freei].row, row, __ATOMIC_RELAXED);
            __atomic_store_n(&g_sm_lanim[freei].w, (unsigned long) w, __ATOMIC_RELEASE);
        }
        why = 101;
        goto out;
    }
    smk_draw(g, SMK_I_FULL, lw - SMK_INSET_PLAYLIST - SMK_W, (lh - SMK_H) / 2);
    why = 100; /* drawn */
out:
    if (logs > 0 && why != 1) {
        FILE *f = azlog_open("/tmp/azmod-stemsmark.log", "a");
        if (f) {
            logs--;
            fprintf(f, "LABEL %lx row=%d why=%d kind=%u id=%u st=%d \"%s\"\n",
                    (unsigned long) (uintptr_t) self, row, why, kind, id, st, name);
            fclose(f);
        }
    }
#undef HL_OUT
}
/* The playlist list's refreshComponentForCell slot serves the stems mark and the playlist
 * drag; it is patched once, by whichever asks first, and each feature acts behind its own
 * gate. 1 = in. */
static int install_hier_refresh_slot(void) {
    static int rc = 0;
    pe_wref_init(); /* the live set the refresh hook fills */
    if (rc == 0)
        rc = azm_patch_vtable(HIER_REFRESH_SLOT, (void *) hier_refresh_hook,
                              (void **) &g_orig_hier_refresh) == 1
                 ? 1
                 : -1;
    return rc;
}
/* The playlist label's paint slot serves Playlist Editing (landing flash, selection
 * brackets, cut ghost) and the stems mark. Patched once, by whichever feature asks first (a
 * second patch would save the hook itself as the original), and each feature draws behind
 * its own gate. Only over the expected function, and only with the refresh slot in: it
 * registers the labels the paint looks up. 1 = in. */
static int install_hlabel_paint_slot(void) {
    static int rc = 0;
    if (rc == 0) {
        uintptr_t cur = 0;
        pe_wref_init();
        rc = img_q(HLABEL_PAINT_SLOT, &cur) && cur == HLABEL_PAINT_ORIG &&
                     install_hier_refresh_slot() == 1 &&
                     azm_patch_vtable(HLABEL_PAINT_SLOT, (void *) hlabel_paint_hook,
                                      (void **) &g_orig_hlabel_paint) == 1
                 ? 1
                 : -1;
    }
    return rc;
}
