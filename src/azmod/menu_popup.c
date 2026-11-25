/* SPDX-License-Identifier: GPL-3.0-only */
/* ==== menu_popup.c: playlist editing in EP147's own MENU panel ================
 * #included by azmod.c after the playlist-edit code.
 *
 * The panel is gui::BrowseMenuWidget: two fixed buttons (B1 [w+0xc8], B2 [w+0xd0]).
 * Each holds label sets {normal, down, disabled juce::Image} at [btn+0x1e8] (alloc
 * +0x1f0, count +0x1f8) and shows set [btn+0x1e0]. gui::BrowseMenuWidgetController
 * picks the sets per menu type [ctrl+0x18]: 0 history, 1 playlist list, 2 track row
 * (PLAYED | UNPLAYED), 3 tag list, -1 no stock menu.
 *
 * We add native buttons of our own and lay the row out in six fixed 210 px slots:
 *   playlist list row  COPY | PASTE | [SELECT] | NEW > | EDIT > | MAKE STEMS
 *   track row          COPY | PASTE | ADD TO | PLAYED/UNPLAYED or REMOVE | MAKE STEMS
 *   tag list           COPY <n> TAGGED | CREATE PLAYLIST | REMOVE ALL TRACKS | MAKE STEMS
 * plus a small UNDO where the stock X was. NEW > and EDIT > slide in a submenu row.
 * A track row with no stock menu gets our synthesized type MP_TYPE_ADD (MAKE STEMS).
 *
 * Labels: FreeType text in EP147's font over the native button face (menulabels.h,
 * rendered at build time), loaded through EP147's juce::ImageCache::getFromMemory.
 * Names and counts (ADD TO target, paste source, panel title) are rendered here at
 * runtime with the same parameters.
 *
 * Gates: the hooks install at every start; Playlist Editing's buttons are gated at
 * use by mp_enabled(), MAKE STEMS by mp_stems_on(). Every object (controller, widget,
 * buttons, model) is vtable-checked at use; any mismatch = stock behavior, untouched. */
#include "menulabels.h"
#include "menu_popup_gate.h"

#define MP_CTRL_VT EP_2b44be8   /* gui::BrowseMenuWidgetController */
#define MP_WIDGET_VT EP_2b37908 /* gui::BrowseMenuWidget */
#define MP_BTN_VT EP_29674c8    /* the panel's multi-label button */
#define MP_MODEL_VT EP_29c5028  /* gui::disp_data::BrowseMenu */
#define MP_REBUILD EP_1b59b98   /* ctrl: sets both buttons from the type */
#define MP_PRODUCER EP_1f40998  /* builds the model (on the MENU press and on navigation) */
#define MP_PRODUCER_MENU_LR \
    EP_1ec495c /* return address for a MENU press; navigation returns to 0x1ec32a0 */
#define MP_CLICK_SLOT (MP_CTRL_VT + 0x10) /* buttonClicked 0x1b5bb90 */
#define MP_CLICK_ORIG EP_1b5bb90
#define MP_RESULT_SLOT EP_2b44c50 /* dialog-result thunk 0x1b5bab0 (this = ctrl+0x10) */
#define MP_RESULT_ORIG EP_1b5bab0
#define MP_OPEN_DIALOG EP_1b5bab8 /* (ctrl, String* title, String* body) */
#define MP_REPAINT EP_2488570     /* juce::Component::repaint() */
#define MP_SETENABLED EP_2465c70  /* juce::Component::setEnabled(bool) */
#define MP_KIND_REMOVE 6          /* our pending kind; stock uses 1..5 */
#define MP_KIND_FOLDERDEL 7       /* REMOVE on a folder (ours; stock disables it) */
#define MP_TYPE_ADD 8             /* our synthesized menu type; stock uses 0..3 */
#define MP_TYPE_FOLDER 9 /* ours: a folder or file in the Browse FOLDER view (COPY | PASTE) */
#define MP_SET_UNPLAYED 3         /* B1 extra set */
#define MP_SET_ADD 4              /* B1 and B2 extra set: "ADD TO / PLAYLIST" (no target) */
#define MP_SET_ADDNAME 5          /* B1 and B2 extra set: "ADD TO / <target>" (swapped per name) */
#define MP_SET_REMOVE 3           /* B2 extra set */
#define MP_SET_RELABEL 6 /* B1 "REMOVE ALL" / B2 "REMOVE": stock ALL DELETE / DELETE, renamed */

/* The same test pl_request_add_selected() makes: JUCE's selection == the hierarchy's
 * row. The add/target API itself lives in playlist_actions.c. */
static int pl_selected_is_track(void) {
    struct plsnap sn = plsnap_get();
    int sel = tracklist_selected_row();
    return sn.any_cid && sel >= 0 && (unsigned) sel == sn.any_row;
}

static int g_mp_installed;
static void (*g_mp_orig_rebuild)(void *);
static void (*g_mp_orig_producer)(void *);
static void (*g_mp_orig_click)(void *, void *);
static void (*g_mp_orig_result)(void *, int);
/* Label images [key][normal, down, disabled] (ImagePixelData*), keys in mp_load_images'
 * order, at 246 px and the 210 px twins. */
#define MP_NIMG 26
#define MP_IMG_STEMSX 25 /* CANCEL STEMS: MAKE STEMS while the row is in the Remote Stems queue */
#define MP_IMG_STEMSN 22
#define MP_IMG_PASTEN 23
#define MP_IMG_DELF 24
#define MP_IMG_EDIT 19
#define MP_IMG_PLAYED 20
#define MP_IMG_UNPLAYED 21
#define MP_IMG_NEWMENU 17
#define MP_IMG_RENAME 18
#define MP_IMG_REMALL 15
#define MP_IMG_CREATEPL 16
#define MP_IMG_CUT 12
#define MP_IMG_SELECT 13
#define MP_IMG_SELECTED 14
static uintptr_t g_mp_img[MP_NIMG][3], g_mp_img210[MP_NIMG][3];
static uintptr_t g_mp_dyn[3]; /* "ADD TO / <target name>" (one ref ours), 0 = none yet */
static int g_mp_b1_unplayed;  /* B1 is showing the UNPLAYED set (stock B2's action) */
static int g_mp_b2_action; /* 0 stock, 1 REMOVE from playlist, 3 REMOVE folder, 4 REMOVE playlist
                              (3 and 4 are ours: journaled) */
static struct bn_holder g_mp_title, g_mp_body;
static uintptr_t g_mp_title_s, g_mp_body_s;

static void mp_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-menupopup.log", "a");
    if (!f)
        return; /* one open/close per line */
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
static int mp_enabled(void) { /* Playlist Editing's buttons */
    return g_mp_installed && g_reorder_hooks && az_on(AZF_PLAYLIST_EDITING);
}
static int mp_stems_on(void) { /* MAKE STEMS (remote stems): its own flag, not Playlist Editing's */
    return g_mp_installed && __atomic_load_n(&g_rs_on, __ATOMIC_RELAXED);
}

/* juce::ImageCache::getFromMemory(const void *data, int size) -> juce::Image (sret
 * x8). The returned Image holds one reference, which we hand to a button.
 * The target is passed in from C (EP_23ed7c0, the per-firmware table), never built
 * from movz/movk here: ep147-addrgen cannot see immediates, so a hard-coded address
 * lands mid-function on another firmware. addrgen refuses an EP147-range movz/movk. */
__asm__(".text\n.align 2\n.globl mp_ifm_call\n.hidden mp_ifm_call\nmp_ifm_call:\n"
        "mov x16, x3\nmov x8, x0\nmov x0, x1\nmov w1, w2\nbr x16\n");
void mp_ifm_call(uintptr_t *out, const void *data, int size, uintptr_t target);
static void mp_image_from_memory(uintptr_t *out, const void *data, int size) {
    uintptr_t t = EP_23ed7c0; /* juce::ImageCache::getFromMemory */
    *out = 0;
    if (t)
        mp_ifm_call(out, data, size, t);
}

static void mp_show(uintptr_t btn, int set, int enabled, int visible);
/* Reading EP147's objects here (ep_mem.h): everything in this file runs on the message thread,
 * and every pointer it reads through is one of
 *   - the controller / argument a hook was handed, or an object reached from it through
 *     EP147's own fields during that call (the widget, B1/B2, the menu model);
 *   - one of our own buttons (mp_make_btn allocates them; juce::Component's destructor only
 *     un-parents children, so EP147 never frees them) or an image we hold a reference to;
 *   - a controller cached across calls, used only after mp_ctrl_live() proves it alive.
 * So the reads are plain loads (own_q); EP147's code and vtables are read with img_q. */
static int mp_valid(uintptr_t p, uintptr_t want) {
    uintptr_t vt = 0;
    return p && !(p & 7) && own_q(p, &vt) && vt == want;
}
/* A cached controller's lifetime. gui::BrowseMenuWidgetController is not a component, but
 * it is never on its own: it is a member of the same parent object as its BrowseMenuWidget
 * (parent+0x370 and parent+0x18; the parent's destructor 0x19a9670 on 1.30, 0x1b0d670 on 2.00,
 * destroys the controller, then the widget), and [ctrl+0xa0] is that widget. So while the
 * widget is alive, so is the controller. The widget is a component (juce::Component at +0):
 * its pointer sits in a wref slot, which juce::Component's destructor clears. A controller
 * is live when one of these slots holds exactly ctrl - MP_CTRL_IN_PARENT. */
#define MP_CTRL_IN_PARENT 0x358 /* ctrl - widget: 0x370 - 0x18, the same on 1.30 and 2.00 */
#define MP_LIVE_N 4
static _Atomic uintptr_t g_mp_live_w[MP_LIVE_N];
static unsigned g_mp_live_next;
/* A hook was handed `c` (alive for this call): remember its widget, if it has the layout above. */
static void mp_note_ctrl(uintptr_t c) {
    uintptr_t w = 0;
    if (!on_msg_thread() || !mp_valid(c, MP_CTRL_VT) || !own_q(c + 0xa0, &w) ||
        w + MP_CTRL_IN_PARENT != c || !mp_valid(w, MP_WIDGET_VT))
        return; /* not the embedded layout: such a controller is never used after its call */
    if (!wref_track(g_mp_live_w, MP_LIVE_N, 0))
        return;
    for (int i = 0; i < MP_LIVE_N; i++)
        if (wref_get(&g_mp_live_w[i]) == w)
            return;
    wref_set(&g_mp_live_w[g_mp_live_next++ % MP_LIVE_N], w);
}
/* 1 when a controller cached across calls is still alive (see above); message thread only. */
static int mp_ctrl_live(uintptr_t c) {
    if (!c || !on_msg_thread())
        return 0;
    for (int i = 0; i < MP_LIVE_N; i++) {
        uintptr_t w = wref_get(&g_mp_live_w[i]);
        if (w && w + MP_CTRL_IN_PARENT == c)
            return mp_valid(c, MP_CTRL_VT);
    }
    return 0;
}
/* ctrl -> widget -> B1/B2, each vtable-checked. */
static int mp_parts(uintptr_t ctrl, uintptr_t *b1, uintptr_t *b2) {
    uintptr_t w = 0;
    if (!mp_valid(ctrl, MP_CTRL_VT) || !own_q(ctrl + 0xa0, &w) || !mp_valid(w, MP_WIDGET_VT))
        return 0;
    if (!own_q(w + 0xc8, b1) || !mp_valid(*b1, MP_BTN_VT))
        return 0;
    if (!own_q(w + 0xd0, b2) || !mp_valid(*b2, MP_BTN_VT))
        return 0;
    return 1;
}
static void mp_incref(uintptr_t img) {
    if (img)
        __atomic_fetch_add((int *) (img + 8), 1, __ATOMIC_RELAXED);
}

static int mp_load_images(void) {
    static int done;
    if (done)
        return done > 0;
    /* every key at both widths (menulabels.h: 246 and _W210) */
    static const unsigned char *const src[2][MP_NIMG][3] = {
        {{ML_ADD_normal, ML_ADD_down, ML_ADD_disabled},
         {ML_REMOVE_normal, ML_REMOVE_down, ML_REMOVE_disabled},
         {ML_TARGET_normal, ML_TARGET_down, ML_TARGET_disabled},
         {ML_DEL_normal, ML_DEL_down, ML_DEL_disabled},
         {ML_ALLDEL_normal, ML_ALLDEL_down, ML_ALLDEL_disabled},
         {ML_STEMS_normal, ML_STEMS_down, ML_STEMS_disabled},
         {ML_NEWPL_normal, ML_NEWPL_down, ML_NEWPL_disabled},
         {ML_NEWFOLDER_normal, ML_NEWFOLDER_down, ML_NEWFOLDER_disabled},
         {ML_TARGETF_normal, ML_TARGETF_down, ML_TARGETF_disabled},
         {ML_MOVE_normal, ML_MOVE_down, ML_MOVE_disabled},
         {ML_COPY_normal, ML_COPY_down, ML_COPY_disabled},
         {ML_PASTE_normal, ML_PASTE_down, ML_PASTE_disabled},
         {ML_CUT_normal, ML_CUT_down, ML_CUT_disabled},
         {ML_SELECT_normal, ML_SELECT_down, ML_SELECT_disabled},
         {ML_SELECTED_normal, ML_SELECTED_down, ML_SELECTED_disabled},
         {ML_REMALL_normal, ML_REMALL_down, ML_REMALL_disabled},
         {ML_CREATEPL_normal, ML_CREATEPL_down, ML_CREATEPL_disabled},
         {ML_NEWMENU_normal, ML_NEWMENU_down, ML_NEWMENU_disabled},
         {ML_RENAME_normal, ML_RENAME_down, ML_RENAME_disabled},
         {ML_EDIT_normal, ML_EDIT_down, ML_EDIT_disabled},
         {ML_PLAYED_normal, ML_PLAYED_down, ML_PLAYED_disabled},
         {ML_UNPLAYED_normal, ML_UNPLAYED_down, ML_UNPLAYED_disabled},
         {ML_STEMSN_normal, ML_STEMSN_down, ML_STEMSN_disabled},
         {ML_PASTEN_normal, ML_PASTEN_down, ML_PASTEN_disabled},
         {ML_DELF_normal, ML_DELF_down, ML_DELF_disabled},
         {ML_STEMSX_normal, ML_STEMSX_down, ML_STEMSX_disabled}},
        {{ML_ADD_W210_normal, ML_ADD_W210_down, ML_ADD_W210_disabled},
         {ML_REMOVE_W210_normal, ML_REMOVE_W210_down, ML_REMOVE_W210_disabled},
         {ML_TARGET_W210_normal, ML_TARGET_W210_down, ML_TARGET_W210_disabled},
         {ML_DEL_W210_normal, ML_DEL_W210_down, ML_DEL_W210_disabled},
         {ML_ALLDEL_W210_normal, ML_ALLDEL_W210_down, ML_ALLDEL_W210_disabled},
         {ML_STEMS_W210_normal, ML_STEMS_W210_down, ML_STEMS_W210_disabled},
         {ML_NEWPL_W210_normal, ML_NEWPL_W210_down, ML_NEWPL_W210_disabled},
         {ML_NEWFOLDER_W210_normal, ML_NEWFOLDER_W210_down, ML_NEWFOLDER_W210_disabled},
         {ML_TARGETF_W210_normal, ML_TARGETF_W210_down, ML_TARGETF_W210_disabled},
         {ML_MOVE_W210_normal, ML_MOVE_W210_down, ML_MOVE_W210_disabled},
         {ML_COPY_W210_normal, ML_COPY_W210_down, ML_COPY_W210_disabled},
         {ML_PASTE_W210_normal, ML_PASTE_W210_down, ML_PASTE_W210_disabled},
         {ML_CUT_W210_normal, ML_CUT_W210_down, ML_CUT_W210_disabled},
         {ML_SELECT_W210_normal, ML_SELECT_W210_down, ML_SELECT_W210_disabled},
         {ML_SELECTED_W210_normal, ML_SELECTED_W210_down, ML_SELECTED_W210_disabled},
         {ML_REMALL_W210_normal, ML_REMALL_W210_down, ML_REMALL_W210_disabled},
         {ML_CREATEPL_W210_normal, ML_CREATEPL_W210_down, ML_CREATEPL_W210_disabled},
         {ML_NEWMENU_W210_normal, ML_NEWMENU_W210_down, ML_NEWMENU_W210_disabled},
         {ML_RENAME_W210_normal, ML_RENAME_W210_down, ML_RENAME_W210_disabled},
         {ML_EDIT_W210_normal, ML_EDIT_W210_down, ML_EDIT_W210_disabled},
         {ML_PLAYED_W210_normal, ML_PLAYED_W210_down, ML_PLAYED_W210_disabled},
         {ML_UNPLAYED_W210_normal, ML_UNPLAYED_W210_down, ML_UNPLAYED_W210_disabled},
         {ML_STEMSN_W210_normal, ML_STEMSN_W210_down, ML_STEMSN_W210_disabled},
         {ML_PASTEN_W210_normal, ML_PASTEN_W210_down, ML_PASTEN_W210_disabled},
         {ML_DELF_W210_normal, ML_DELF_W210_down, ML_DELF_W210_disabled},
         {ML_STEMSX_W210_normal, ML_STEMSX_W210_down, ML_STEMSX_W210_disabled}}};
    static const int len[2][MP_NIMG][3] = {
        {{sizeof ML_ADD_normal, sizeof ML_ADD_down, sizeof ML_ADD_disabled},
         {sizeof ML_REMOVE_normal, sizeof ML_REMOVE_down, sizeof ML_REMOVE_disabled},
         {sizeof ML_TARGET_normal, sizeof ML_TARGET_down, sizeof ML_TARGET_disabled},
         {sizeof ML_DEL_normal, sizeof ML_DEL_down, sizeof ML_DEL_disabled},
         {sizeof ML_ALLDEL_normal, sizeof ML_ALLDEL_down, sizeof ML_ALLDEL_disabled},
         {sizeof ML_STEMS_normal, sizeof ML_STEMS_down, sizeof ML_STEMS_disabled},
         {sizeof ML_NEWPL_normal, sizeof ML_NEWPL_down, sizeof ML_NEWPL_disabled},
         {sizeof ML_NEWFOLDER_normal, sizeof ML_NEWFOLDER_down, sizeof ML_NEWFOLDER_disabled},
         {sizeof ML_TARGETF_normal, sizeof ML_TARGETF_down, sizeof ML_TARGETF_disabled},
         {sizeof ML_MOVE_normal, sizeof ML_MOVE_down, sizeof ML_MOVE_disabled},
         {sizeof ML_COPY_normal, sizeof ML_COPY_down, sizeof ML_COPY_disabled},
         {sizeof ML_PASTE_normal, sizeof ML_PASTE_down, sizeof ML_PASTE_disabled},
         {sizeof ML_CUT_normal, sizeof ML_CUT_down, sizeof ML_CUT_disabled},
         {sizeof ML_SELECT_normal, sizeof ML_SELECT_down, sizeof ML_SELECT_disabled},
         {sizeof ML_SELECTED_normal, sizeof ML_SELECTED_down, sizeof ML_SELECTED_disabled},
         {sizeof ML_REMALL_normal, sizeof ML_REMALL_down, sizeof ML_REMALL_disabled},
         {sizeof ML_CREATEPL_normal, sizeof ML_CREATEPL_down, sizeof ML_CREATEPL_disabled},
         {sizeof ML_NEWMENU_normal, sizeof ML_NEWMENU_down, sizeof ML_NEWMENU_disabled},
         {sizeof ML_RENAME_normal, sizeof ML_RENAME_down, sizeof ML_RENAME_disabled},
         {sizeof ML_EDIT_normal, sizeof ML_EDIT_down, sizeof ML_EDIT_disabled},
         {sizeof ML_PLAYED_normal, sizeof ML_PLAYED_down, sizeof ML_PLAYED_disabled},
         {sizeof ML_UNPLAYED_normal, sizeof ML_UNPLAYED_down, sizeof ML_UNPLAYED_disabled},
         {sizeof ML_STEMSN_normal, sizeof ML_STEMSN_down, sizeof ML_STEMSN_disabled},
         {sizeof ML_PASTEN_normal, sizeof ML_PASTEN_down, sizeof ML_PASTEN_disabled},
         {sizeof ML_DELF_normal, sizeof ML_DELF_down, sizeof ML_DELF_disabled},
         {sizeof ML_STEMSX_normal, sizeof ML_STEMSX_down, sizeof ML_STEMSX_disabled}},
        {{sizeof ML_ADD_W210_normal, sizeof ML_ADD_W210_down, sizeof ML_ADD_W210_disabled},
         {sizeof ML_REMOVE_W210_normal, sizeof ML_REMOVE_W210_down, sizeof ML_REMOVE_W210_disabled},
         {sizeof ML_TARGET_W210_normal, sizeof ML_TARGET_W210_down, sizeof ML_TARGET_W210_disabled},
         {sizeof ML_DEL_W210_normal, sizeof ML_DEL_W210_down, sizeof ML_DEL_W210_disabled},
         {sizeof ML_ALLDEL_W210_normal, sizeof ML_ALLDEL_W210_down, sizeof ML_ALLDEL_W210_disabled},
         {sizeof ML_STEMS_W210_normal, sizeof ML_STEMS_W210_down, sizeof ML_STEMS_W210_disabled},
         {sizeof ML_NEWPL_W210_normal, sizeof ML_NEWPL_W210_down, sizeof ML_NEWPL_W210_disabled},
         {sizeof ML_NEWFOLDER_W210_normal, sizeof ML_NEWFOLDER_W210_down,
          sizeof ML_NEWFOLDER_W210_disabled},
         {sizeof ML_TARGETF_W210_normal, sizeof ML_TARGETF_W210_down,
          sizeof ML_TARGETF_W210_disabled},
         {sizeof ML_MOVE_W210_normal, sizeof ML_MOVE_W210_down, sizeof ML_MOVE_W210_disabled},
         {sizeof ML_COPY_W210_normal, sizeof ML_COPY_W210_down, sizeof ML_COPY_W210_disabled},
         {sizeof ML_PASTE_W210_normal, sizeof ML_PASTE_W210_down, sizeof ML_PASTE_W210_disabled},
         {sizeof ML_CUT_W210_normal, sizeof ML_CUT_W210_down, sizeof ML_CUT_W210_disabled},
         {sizeof ML_SELECT_W210_normal, sizeof ML_SELECT_W210_down, sizeof ML_SELECT_W210_disabled},
         {sizeof ML_SELECTED_W210_normal, sizeof ML_SELECTED_W210_down,
          sizeof ML_SELECTED_W210_disabled},
         {sizeof ML_REMALL_W210_normal, sizeof ML_REMALL_W210_down, sizeof ML_REMALL_W210_disabled},
         {sizeof ML_CREATEPL_W210_normal, sizeof ML_CREATEPL_W210_down,
          sizeof ML_CREATEPL_W210_disabled},
         {sizeof ML_NEWMENU_W210_normal, sizeof ML_NEWMENU_W210_down,
          sizeof ML_NEWMENU_W210_disabled},
         {sizeof ML_RENAME_W210_normal, sizeof ML_RENAME_W210_down, sizeof ML_RENAME_W210_disabled},
         {sizeof ML_EDIT_W210_normal, sizeof ML_EDIT_W210_down, sizeof ML_EDIT_W210_disabled},
         {sizeof ML_PLAYED_W210_normal, sizeof ML_PLAYED_W210_down, sizeof ML_PLAYED_W210_disabled},
         {sizeof ML_UNPLAYED_W210_normal, sizeof ML_UNPLAYED_W210_down,
          sizeof ML_UNPLAYED_W210_disabled},
         {sizeof ML_STEMSN_W210_normal, sizeof ML_STEMSN_W210_down, sizeof ML_STEMSN_W210_disabled},
         {sizeof ML_PASTEN_W210_normal, sizeof ML_PASTEN_W210_down, sizeof ML_PASTEN_W210_disabled},
         {sizeof ML_DELF_W210_normal, sizeof ML_DELF_W210_down, sizeof ML_DELF_W210_disabled},
         {sizeof ML_STEMSX_W210_normal, sizeof ML_STEMSX_W210_down,
          sizeof ML_STEMSX_W210_disabled}}};
    for (int wv = 0; wv < 2; wv++)
        for (int k = 0; k < MP_NIMG; k++)
            for (int s = 0; s < 3; s++) {
                uintptr_t img = 0;
                unsigned want = wv ? 210 : 246;
                mp_image_from_memory(&img, src[wv][k][s], len[wv][k][s]);
                uint32_t wh[2] = {0, 0};
                uintptr_t q = 0;
                if (img && own_q(img + 0x10, &q)) {
                    wh[0] = (uint32_t) q;
                    wh[1] = (uint32_t) (q >> 32);
                }
                if (!img || wh[0] != want || wh[1] != 80) {
                    mp_log("LABEL load failed w=%u k=%d s=%d img=%lx %ux%u\n", want, k, s,
                           (unsigned long) img, wh[0], wh[1]);
                    done = -1;
                    return 0;
                }
                (wv ? g_mp_img210 : g_mp_img)[k][s] = img;
            }
    done = 1;
    mp_log("labels loaded (EP147 ImageCache)\n");
    return 1;
}

/* ==== the runtime label: "ADD TO" / <target playlist name> ======================
 * juce::ImageCache::getFromMemory caches by data pointer, so it cannot make a
 * second image from the same bytes. It inlines ImageFileFormat::loadFrom(data,
 * size), which does not cache (0x23ed970..0x23ed9a4), and we make the same four
 * calls: MemoryInputStream ctor 0x230d790(s, data, size, keepCopy=false) (0x28 B:
 * vt, data, size, pos, copy), findImageFormatForStream 0x23b4be0(s), the format's
 * decodeImage (vt +0x28, juce::Image sret), MemoryInputStream dtor 0x22fb7d0 (frees
 * the copy; none). Every call makes a fresh ImagePixelData from ML_ADDTO_* (the
 * face with line 1 "ADD TO" already on it, build.py), and we draw the name onto
 * line 2 before anything else holds a reference: nobody has drawn it yet, so no
 * cache anywhere has seen the old pixels. Old images go back through EP147's own
 * release 0x23a2e60 (atomic --refcount, deleting dtor at 0).
 *
 * The text is the same FreeType render as the build-time label generator (face, size,
 * thinning, placement: MP_FT_* in menulabels.h), so a runtime label and a build-time
 * label are the same pixels. Composite once: (bg*(255-a) + ink*a + 127) / 255,
 * only on opaque face pixels (premultiplied == straight there). Message thread only.
 * Re-rendered only when the target name changes. */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#define MP_STREAM_CTOR EP_230d790
#define MP_STREAM_DTOR EP_22fb7d0
#define MP_FIND_FORMAT EP_23b4be0
#define MP_IMG_RELEASE EP_23a2e60
/* fn(a0, a1) with the juce::Image sret in x8 */
__asm__(".text\n.align 2\n.globl mp_sret2\n.hidden mp_sret2\nmp_sret2:\n"
        "mov x8, x0\nmov x16, x1\nmov x0, x2\nmov x1, x3\nbr x16\n");
void mp_sret2(uintptr_t *out, uintptr_t fn, uintptr_t a0, uintptr_t a1);
static void mp_release(uintptr_t img) {
    if (img)
        ((void (*)(uintptr_t)) MP_IMG_RELEASE)(img);
}
static uintptr_t mp_image_uncached(const unsigned char *data, int size) {
    uint64_t st[8] __attribute__((aligned(16))) = {0}; /* the stream: 0x28 B used */
    ((void (*)(void *, const void *, size_t, int)) MP_STREAM_CTOR)(st, data, (size_t) size, 0);
    uintptr_t fmt = ((uintptr_t (*)(void *)) MP_FIND_FORMAT)(st), img = 0, vt = 0, fn = 0;
    if (fmt && own_q(fmt, &vt) && own_q(vt + 0x28, &fn) && fn)
        mp_sret2(&img, fn, fmt, (uintptr_t) st);
    ((void (*)(void *)) MP_STREAM_DTOR)(st);
    return img;
}
/* SoftwarePixelData: +0x0c format (2 = ARGB), +0x10 w, +0x14 h, +0x48 pixels,
 * +0x50 pixel stride, +0x54 line stride. Anything else: not ours to write. */
static uint8_t *mp_pixels_w(uintptr_t img, int *ls, int W) {
    uintptr_t fm = 0, wh = 0, px = 0, st = 0;
    if (!img || !own_q(img + 0x08, &fm) || !own_q(img + 0x10, &wh) || !own_q(img + 0x48, &px) ||
        !own_q(img + 0x50, &st))
        return 0;
    if ((int) (uint32_t) wh != W || (uint32_t) (wh >> 32) != 80 || (uint32_t) (fm >> 32) != 2 ||
        (uint32_t) st != 4 || (int) (st >> 32) < W * 4 || !px) {
        mp_log("NAME label: image %lx not ARGB Wx80 (fmt %u %ux%u stride %u/%u) -- static label "
               "kept\n",
               (unsigned long) img, (unsigned) (fm >> 32), (unsigned) wh, (unsigned) (wh >> 32),
               (unsigned) st, (unsigned) (st >> 32));
        return 0;
    }
    *ls = (int) (st >> 32);
    return (uint8_t *) px;
}
static FT_Library g_mp_ftlib;
static FT_Face g_mp_face;
static int g_mp_ft; /* 0 untried, 1 ready, -1 no */
static FT_GlyphSlot mp_glyph_f(FT_Face f, int bold64, uint32_t cp) {
    FT_UInt gi = FT_Get_Char_Index(f, cp);
    if (!gi || FT_Load_Glyph(f, gi, FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT))
        return 0;
    if (bold64)
        FT_Outline_Embolden(&f->glyph->outline, bold64);
    return FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL) ? 0 : f->glyph;
}
#define mp_glyph(cp) mp_glyph_f(g_mp_face, MP_FT_BOLD64, (cp)) /* the button labels' face */
#define MP_NAME_MAX 100
struct mp_gl {
    uint32_t cp;
    int x, l, r;
}; /* x = pen px; ink cols x+l..x+r (r < l: none) */
/* Pen positions and ink extents, exactly as ftlabel.line_cov: pen += advance + track,
 * glyph at floor(pen + .5) + bitmap_left, ink = coverage >= 8/255 (> 0.03). */
static int mp_lay_f(FT_Face f, int bold64, const uint32_t *cp, int n, struct mp_gl *g,
                    double *pen_end) {
    double pen = 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        FT_GlyphSlot gs = mp_glyph_f(f, bold64, cp[i]);
        if (!gs)
            continue;
        struct mp_gl *o = &g[k++];
        o->cp = cp[i];
        o->x = (int) floor(pen + 0.5);
        o->l = 1;
        o->r = 0;
        for (unsigned c = 0; c < gs->bitmap.width; c++)
            for (unsigned r = 0; r < gs->bitmap.rows; r++)
                if (gs->bitmap.buffer[r * (unsigned) gs->bitmap.pitch + c] >= 8) {
                    int col = gs->bitmap_left + (int) c;
                    if (o->r < o->l) {
                        o->l = o->r = col;
                    } else {
                        if (col < o->l)
                            o->l = col;
                        if (col > o->r)
                            o->r = col;
                    }
                    break;
                }
        pen += gs->advance.x / 64.0 + MP_FT_TRACK;
    }
    if (pen_end)
        *pen_end = pen;
    return k;
}
static int mp_lay(const uint32_t *cp, int n, struct mp_gl *g, double *pen_end) {
    return mp_lay_f(g_mp_face, MP_FT_BOLD64, cp, n, g, pen_end);
}
static int mp_ink(const struct mp_gl *g, int n, int *lo, int *hi) {
    int any = 0;
    for (int i = 0; i < n; i++)
        if (g[i].r >= g[i].l) {
            if (!any || g[i].x + g[i].l < *lo)
                *lo = g[i].x + g[i].l;
            if (!any || g[i].x + g[i].r > *hi)
                *hi = g[i].x + g[i].r;
            any = 1;
        }
    return any;
}
/* UTF-8 -> code points, ASCII upper-cased (the native labels are all capitals), then
 * ftlabel.fit_name's truncation: the whole name if it fits MP_FT_NAME_MAXW, else the
 * longest prefix (trailing spaces dropped) + "..." that does. */
static int mp_fit_name_w(const char *name, uint32_t *out, int maxw) {
    uint32_t cp[MP_NAME_MAX];
    int n = 0;
    for (const unsigned char *p = (const unsigned char *) name; *p && n < MP_NAME_MAX;) {
        uint32_t c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2
                               : c >= 0xc0   ? 1
                                             : 0;
        if (more)
            c &= 0x3f >> more;
        while (more-- && (*p & 0xc0) == 0x80)
            c = (c << 6) | (*p++ & 0x3f);
        cp[n++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    struct mp_gl g[MP_NAME_MAX + 3];
    int lo, hi;
    int k = mp_lay(cp, n, g, 0);
    if (!mp_ink(g, k, &lo, &hi) || hi - lo + 1 <= maxw) {
        memcpy(out, cp, n * sizeof *cp);
        return n;
    }
    for (int m = n; m >= 0; m--) {
        int t = m;
        while (t > 0 && (cp[t - 1] == ' ' || cp[t - 1] == '\t'))
            t--;
        memcpy(out, cp, t * sizeof *cp);
        out[t] = out[t + 1] = out[t + 2] = '.';
        k = mp_lay(out, t + 3, g, 0);
        if (m == 0 || (mp_ink(g, k, &lo, &hi) && hi - lo + 1 <= maxw))
            return t + 3;
    }
    return 0;
}
/* The three state images for `name`, one reference each, or 0 (then the static
 * "ADD TO / PLAYLIST" label is used). */
static int mp_ft_ready(void) { /* FreeType + the label face, once */
    if (!g_mp_ft) {
        g_mp_ft = -1;
        if (!FT_Init_FreeType(&g_mp_ftlib) && !FT_New_Face(g_mp_ftlib, MP_FT_FACE, 0, &g_mp_face) &&
            !FT_Set_Char_Size(g_mp_face, (FT_F26Dot6) (MP_FT_PX * 64 + 0.5), 0, 72, 72))
            g_mp_ft = 1;
        mp_log("NAME label: FreeType %s (%s)\n", g_mp_ft > 0 ? "ready" : "FAILED", MP_FT_FACE);
    }
    return g_mp_ft > 0;
}
/* Line 2 of a two-line label, drawn at runtime onto a fresh decode of `src` (line 1
 * baked by build.py): the ADD TO [name] label, and COPY / "<n> TAGGED". W = 246 or
 * 210 (the matching art and center). */
static int mp_render_line2(const char *name, const unsigned char *const src[3], const int len[3],
                           int W, double cx, int maxw, uintptr_t out[3], int gray) {
    if (!mp_ft_ready())
        return 0;
    static uint8_t cov[80][246];
    uint32_t cp[MP_NAME_MAX + 3];
    int n = mp_fit_name_w(name, cp, maxw);
    struct mp_gl g[MP_NAME_MAX + 3];
    int lo = 0, hi = 0, k = mp_lay(cp, n, g, 0);
    memset(cov, 0, sizeof cov);
    if (mp_ink(g, k, &lo, &hi)) {
        int X = (int) floor(cx - (lo + hi) / 2.0 + 0.5);
        for (int i = 0; i < k; i++) {
            FT_GlyphSlot gs = mp_glyph(g[i].cp);
            if (!gs)
                continue;
            for (unsigned r = 0; r < gs->bitmap.rows; r++) {
                int y = MP_FT_BASE2 + 1 - gs->bitmap_top + (int) r;
                if (y < 0 || y >= 80)
                    continue;
                for (unsigned c = 0; c < gs->bitmap.width; c++) {
                    int x = X + g[i].x + gs->bitmap_left + (int) c;
                    uint8_t a = gs->bitmap.buffer[r * (unsigned) gs->bitmap.pitch + c];
                    if (x >= 0 && x < W && a > cov[y][x])
                        cov[y][x] = a;
                }
            }
        }
    }
    static const int ink[3] = MP_FT_INK;
    for (int s = 0; s < 3; s++) {
        int ls = 0;
        out[s] = mp_image_uncached(src[s], len[s]);
        uint8_t *px = mp_pixels_w(out[s], &ls, W);
        if (!px) {
            for (int j = 0; j <= s; j++) {
                mp_release(out[j]);
                out[j] = 0;
            }
            return 0;
        }
        for (int y = 0; y < 80; y++)
            for (int x = 0; x < W; x++) {
                unsigned a = gray ? cov[y][x] * MP_FT_GREY256 / 256
                                  : cov[y][x];    /* gray subtext: part of the ink */
                uint8_t *p = px + y * ls + x * 4; /* B G R A */
                if (!a || p[3] != 255)
                    continue;
                for (int c = 0; c < 3; c++)
                    p[c] = (uint8_t) ((p[c] * (255 - a) + ink[s] * a + 127) / 255);
            }
#ifdef AZMOD_HAVE_DAYMODE
        dm_record_own_art((void **) &out[s]); /* themed like EP147's own decoded art (Day: light face, dark ink) */
#endif
    }
    mp_log("NAME label: \"%s\" (%d px) -> %d glyphs, ink %d..%d, images %lx %lx %lx\n", name, W, k,
           lo, hi, (unsigned long) out[0], (unsigned long) out[1], (unsigned long) out[2]);
    return 1;
}
/* "[NAME]" (the brackets mark the selected playlist), the name cut
 * with "..." inside the brackets so the closing one always shows. */
static int mp_text_w(const char *t) {
    uint32_t cp[MP_NAME_MAX + 3];
    int n = 0;
    for (const unsigned char *p = (const unsigned char *) t; *p && n < MP_NAME_MAX;) {
        uint32_t c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2
                               : c >= 0xc0   ? 1
                                             : 0;
        if (more)
            c &= 0x3f >> more;
        while (more-- && (*p & 0xc0) == 0x80)
            c = (c << 6) | (*p++ & 0x3f);
        cp[n++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    struct mp_gl g[MP_NAME_MAX + 3];
    int lo = 0, hi = 0, k = mp_lay(cp, n, g, 0);
    return mp_ink(g, k, &lo, &hi) ? hi - lo + 1 : 0;
}
static void mp_bracket_fit(const char *name, int maxw, char *out, size_t osz) {
    size_t n = strlen(name);
    snprintf(out, osz, "[%s]", name);
    if (!mp_ft_ready() || mp_text_w(out) <= maxw)
        return;
    while (n > 0) {
        n--;
        while (n > 0 && (name[n] & 0xc0) == 0x80)
            n--; /* whole UTF-8 characters */
        size_t t = n;
        while (t > 0 && name[t - 1] == ' ')
            t--;
        snprintf(out, osz, "[%.*s...]", (int) t, name);
        if (mp_text_w(out) <= maxw)
            return;
    }
    snprintf(out, osz, "[...]");
}
static int mp_render_name_w(const char *name, int W, uintptr_t out[3]) {
    static const unsigned char *const src[3] = {ML_ADDTO_normal, ML_ADDTO_down, ML_ADDTO_disabled};
    static const int len[3] = {sizeof ML_ADDTO_normal, sizeof ML_ADDTO_down,
                               sizeof ML_ADDTO_disabled};
    static const unsigned char *const src2[3] = {ML_ADDTO_W210_normal, ML_ADDTO_W210_down,
                                                 ML_ADDTO_W210_disabled};
    static const int len2[3] = {sizeof ML_ADDTO_W210_normal, sizeof ML_ADDTO_W210_down,
                                sizeof ML_ADDTO_W210_disabled};
    int maxw = W == 210 ? MP_FT_NAME_MAXW_W210 : MP_FT_NAME_MAXW;
    char t[PL_TGT_NAME_MAX + 8];
    if (!mp_ft_ready())
        return 0;
    mp_bracket_fit(name, maxw, t, sizeof t);
    return W == 210 ? mp_render_line2(t, src2, len2, 210, MP_FT_CENTER_X_W210, maxw, out, 0)
                    : mp_render_line2(t, src, len, 246, MP_FT_CENTER_X, maxw, out, 0);
}
static int mp_render_name(const char *name, uintptr_t out[3]) {
    return mp_render_name_w(name, 246, out);
}
/* COPY / "<n> TAGGED" (TAG LIST menu), re-rendered when n changes. */
static uintptr_t g_mp_tagdyn[3];
static const uintptr_t *mp_tag_images(int n) {
    static int last = -1;
    if (g_mp_tagdyn[0] && last == n)
        return g_mp_tagdyn;
    static const unsigned char *const src[3] = {ML_COPYN_W210_normal, ML_COPYN_W210_down,
                                                ML_COPYN_W210_disabled};
    static const int len[3] = {sizeof ML_COPYN_W210_normal, sizeof ML_COPYN_W210_down,
                               sizeof ML_COPYN_W210_disabled};
    char t[32];
    snprintf(t, sizeof t, "%d TAGGED", n);
    uintptr_t nw[3];
    if (!mp_render_line2(t, src, len, 210, MP_FT_CENTER_X_W210, MP_FT_NAME_MAXW_W210, nw, 1))
        return 0; /* "<n> TAGGED" in gray */
    for (int s = 0; s < 3; s++) {
        mp_release(g_mp_tagdyn[s]);
        g_mp_tagdyn[s] = nw[s];
    }
    last = n;
    return g_mp_tagdyn;
}
/* Gray subtext at runtime: line 1 baked (PASTEN / STEMSN at 210), line 2 drawn
 * gray; re-rendered only when the text changes. */
struct mp_dyn2 {
    uintptr_t img[3];
    char text[96];
};
static const uintptr_t *mp_dyn2_images(struct mp_dyn2 *d, const char *text, int key) {
    if (d->img[0] && !strcmp(d->text, text))
        return d->img;
    static const unsigned char *const stems[3] = {ML_STEMSN_W210_normal, ML_STEMSN_W210_down,
                                                  ML_STEMSN_W210_disabled};
    static const int stemsl[3] = {sizeof ML_STEMSN_W210_normal, sizeof ML_STEMSN_W210_down,
                                  sizeof ML_STEMSN_W210_disabled};
    static const unsigned char *const paste[3] = {ML_PASTEN_W210_normal, ML_PASTEN_W210_down,
                                                  ML_PASTEN_W210_disabled};
    static const int pastel[3] = {sizeof ML_PASTEN_W210_normal, sizeof ML_PASTEN_W210_down,
                                  sizeof ML_PASTEN_W210_disabled};
    uintptr_t nw[3];
    if (!mp_render_line2(text, key == MP_IMG_STEMSN ? stems : paste,
                         key == MP_IMG_STEMSN ? stemsl : pastel, 210, MP_FT_CENTER_X_W210,
                         MP_FT_NAME_MAXW_W210, nw, 1))
        return 0;
    for (int s = 0; s < 3; s++) {
        mp_release(d->img[s]);
        d->img[s] = nw[s];
    }
    snprintf(d->text, sizeof d->text, "%s", text);
    return d->img;
}
static struct mp_dyn2 g_mp_pastedyn, g_mp_stemsdyn;
/* The panel title: the selected row's name where EP147 draws its
 * baked "MENU" image. BrowseMenuWidget::paint 0x1b57640 = fillAll(0xff232323), then
 * Graphics::drawImage 0x23de290(g, [w+0xe0], 557, 13, 167, 18, 0, 0, sw, sh, false):
 * the word is an image, not text. So: stock paint first, then our image over that
 * box: an opaque canvas in the panel fill (build.py ML_TITLE_BG) with the name
 * composited once in white, EP147's face at the size fitted to "MENU". Rendered on
 * the message thread when the name changes; paint only draws it. */
static FT_Face g_mpt_face;
static uintptr_t g_mpt_img;    /* one ref, or 0 = stock "MENU" */
static uintptr_t g_mpt_widget; /* the widget it is for; compared, never dereferenced */
static char g_mpt_text[96];
static int mp_title_render(const char *name) {
    if (!g_mpt_face &&
        (!mp_ft_ready() || FT_New_Face(g_mp_ftlib, MP_FT_FACE, 0, &g_mpt_face) ||
         FT_Set_Char_Size(g_mpt_face, (FT_F26Dot6) (MP_TITLE_PX * 64 + 0.5), 0, 72, 72))) {
        g_mpt_face = 0;
        return 0;
    }
    uint32_t cp[MP_NAME_MAX + 3];
    int n = 0;
    for (const unsigned char *p = (const unsigned char *) name; *p && n < MP_NAME_MAX;) {
        uint32_t c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2
                               : c >= 0xc0   ? 1
                                             : 0;
        if (more)
            c &= 0x3f >> more;
        while (more-- && (*p & 0xc0) == 0x80)
            c = (c << 6) | (*p++ & 0x3f);
        cp[n++] = c;
    }
    struct mp_gl g[MP_NAME_MAX + 3];
    int lo = 0, hi = 0, k = mp_lay_f(g_mpt_face, MP_TITLE_BOLD64, cp, n, g, 0);
    while (n > 0 && mp_ink(g, k, &lo, &hi) && hi - lo + 1 > MP_TITLE_W - 8) { /* too wide: "..." */
        int t = --n;
        while (t > 0 && cp[t - 1] == ' ')
            t--;
        cp[t] = cp[t + 1] = cp[t + 2] = '.';
        k = mp_lay_f(g_mpt_face, MP_TITLE_BOLD64, cp, t + 3, g, 0);
    }
    if (!mp_ink(g, k, &lo, &hi))
        return 0;
    uintptr_t img = mp_image_uncached(ML_TITLE_BG, sizeof ML_TITLE_BG), fm = 0, wh = 0, px = 0,
              st = 0;
    if (!img || !own_q(img + 0x08, &fm) || !own_q(img + 0x10, &wh) || !own_q(img + 0x48, &px) ||
        !own_q(img + 0x50, &st) || (uint32_t) wh != MP_TITLE_W ||
        (uint32_t) (wh >> 32) != MP_TITLE_H || (uint32_t) (fm >> 32) != 2 || (uint32_t) st != 4 ||
        (int) (st >> 32) < MP_TITLE_W * 4 || !px) {
        mp_release(img);
        return 0;
    }
    int ls = (int) (st >> 32), X = (int) floor(MP_TITLE_W / 2.0 - (lo + hi) / 2.0 + 0.5);
    static uint8_t cov[MP_TITLE_H][MP_TITLE_W];
    memset(cov, 0, sizeof cov);
    for (int i = 0; i < k; i++) {
        FT_GlyphSlot gs = mp_glyph_f(g_mpt_face, MP_TITLE_BOLD64, g[i].cp);
        if (!gs)
            continue;
        for (unsigned r = 0; r < gs->bitmap.rows; r++) {
            int y = MP_TITLE_BASE + 1 - gs->bitmap_top + (int) r;
            if (y < 0 || y >= MP_TITLE_H)
                continue;
            for (unsigned c = 0; c < gs->bitmap.width; c++) {
                int x = X + g[i].x + gs->bitmap_left + (int) c;
                uint8_t a = gs->bitmap.buffer[r * (unsigned) gs->bitmap.pitch + c];
                if (x >= 0 && x < MP_TITLE_W && a > cov[y][x])
                    cov[y][x] = a;
            }
        }
    }
    for (int y = 0; y < MP_TITLE_H; y++)
        for (int x = 0; x < MP_TITLE_W; x++) { /* once: bg*(255-a) + white*a */
            unsigned a = cov[y][x];
            if (!a)
                continue;
            uint8_t *q = (uint8_t *) px + y * ls + x * 4;
            for (int c = 0; c < 3; c++)
                q[c] = (uint8_t) ((q[c] * (255 - a) + 255 * a + 127) / 255);
        }
#ifdef AZMOD_HAVE_DAYMODE
    dm_record_own_art((void **) &img); /* the canvas is the panel fill: themed with the panel */
#endif
    mp_release(g_mpt_img);
    g_mpt_img = img;
    mp_log("TITLE \"%s\" -> %d glyphs, ink %d..%d\n", name, k, lo, hi);
    return 1;
}
/* Rebuild: which name, if any. Titles only where the row is the subject of the
 * menu (playlist/history lists, track rows, our ADD menu); elsewhere stock "MENU". */
static void mp_title_update(uintptr_t c, int type) {
    struct plsnap sn = plsnap_get();
    uintptr_t w = 0;
    own_q(c + 0xa0, &w);
    /* not type 3 (TAG LIST): that menu acts on the whole list, so the highlighted
     * track's name there would be misleading; stock "MENU" stays */
    const char *t =
        (type == 0 || type == 1 || type == 2 || type == MP_TYPE_ADD || type == MP_TYPE_FOLDER)
            ? sn.sel_title
            : "";
    g_mpt_widget = w;
    if (!*t) {
        mp_release(g_mpt_img);
        g_mpt_img = 0;
        g_mpt_text[0] = 0;
        return;
    }
    if (g_mpt_img && !strcmp(t, g_mpt_text))
        return;
    if (mp_title_render(t))
        snprintf(g_mpt_text, sizeof g_mpt_text, "%s", t);
    else {
        mp_release(g_mpt_img);
        g_mpt_img = 0;
        g_mpt_text[0] = 0;
    }
}
#define MP_WIDGET_PAINT_SLOT (EP_2b37908 + 0xd0) /* BrowseMenuWidget vt: paint */
#define MP_WIDGET_PAINT_ORIG EP_1b57640
#define MP_DRAW_IMAGE EP_23de290 /* Graphics::drawImage(img, dx,dy,dw,dh, sx,sy,sw,sh, fillAlpha)  \
                                  */
static void (*g_mp_orig_paint)(void *, void *);
static void mp_paint_hook(void *w, void *gr) {
    g_mp_orig_paint(w, gr);
    if (!g_mpt_img || (uintptr_t) w != g_mpt_widget)
        return;
    uintptr_t wh = 0;
    int ww =
        own_q((uintptr_t) w + 0x28, &wh) ? (int) (uint32_t) wh : 1280; /* Component bounds w */
    uintptr_t holder = g_mpt_img; /* a juce::Image is its pixel-data pointer */
    ((void (*)(void *, uintptr_t *, int, int, int, int, int, int, int, int, int)) MP_DRAW_IMAGE)(
        gr, &holder, (ww - MP_TITLE_W) / 2, 12, MP_TITLE_W, MP_TITLE_H, 0, 0, MP_TITLE_W,
        MP_TITLE_H, 0);
}
/* The current target's images, rendered on a name change; 0 if none can be made. */
static const uintptr_t *mp_target_images(void) {
    static char last[PL_TGT_NAME_MAX];
    if (!pl_add_target_set())
        return 0;
    const char *nm = pl_add_target_name();
    if (g_mp_dyn[0] && !strcmp(last, nm))
        return g_mp_dyn;
    uintptr_t nw[3];
    if (!mp_render_name(nm, nw))
        return 0;
    for (int s = 0; s < 3; s++) {
        mp_release(g_mp_dyn[s]);
        g_mp_dyn[s] = nw[s];
    } /* buttons keep their own refs */
    snprintf(last, sizeof last, "%s", nm);
    return g_mp_dyn;
}
static uintptr_t g_mp_dyn210[3];
static const uintptr_t *mp_target_images210(void) {
    static char last[PL_TGT_NAME_MAX];
    if (!pl_add_target_set())
        return 0;
    const char *nm = pl_add_target_name();
    if (g_mp_dyn210[0] && !strcmp(last, nm))
        return g_mp_dyn210;
    uintptr_t nw[3];
    if (!mp_render_name_w(nm, 210, nw))
        return 0;
    for (int s = 0; s < 3; s++) {
        mp_release(g_mp_dyn210[s]);
        g_mp_dyn210[s] = nw[s];
    }
    snprintf(last, sizeof last, "%s", nm);
    return g_mp_dyn210;
}
/* Point an existing set slot at `set` (a ref each), releasing what it held. */
static int mp_swap_set(uintptr_t btn, int idx, const uintptr_t set[3]) {
    uintptr_t arr = 0, us = 0;
    if (!own_q(btn + 0x1e8, &arr) || !own_q(btn + 0x1f8, &us) || !arr ||
        (int) (uint32_t) us <= idx)
        return 0;
    for (int s = 0; s < 3; s++) {
        uintptr_t *slot = (uintptr_t *) (arr + (uintptr_t) idx * 0x18 + (uintptr_t) s * 8),
                  old = *slot;
        if (old == set[s])
            continue;
        mp_incref(set[s]);
        *slot = set[s];
        mp_release(old);
    }
    return 1;
}
/* Show the ADD label on `btn`: the target's name if one is set and renders, else the
 * static "ADD TO / PLAYLIST". `name_idx` holds the name set, `static_idx` the other. */
static int mp_swap_set(uintptr_t btn, int idx, const uintptr_t set[3]);
static uintptr_t g_mp_ba; /* ADD TO's own button (defined with it, below) */
static void mp_show_add(uintptr_t btn, int static_idx, int name_idx) {
    const uintptr_t *dyn = mp_target_images(), *d2 = btn == g_mp_ba ? mp_target_images210() : 0;
    if (d2)
        mp_swap_set(btn, name_idx + 2, d2); /* ADD TO's own button: its 210 twin slot too */
    if (dyn && mp_swap_set(btn, name_idx, dyn))
        mp_show(btn, name_idx, 1, 1);
    else
        mp_show(btn, static_idx, 1, 1);
}

/* Append a label set to a button, once per button instance. Returns 1 if the
 * button now has set `want_idx` holding `set`, 0 on any doubt (then untouched). */
static int mp_ensure_set(uintptr_t btn, int want_idx, const uintptr_t set[3], int take_extra_ref) {
    uintptr_t arr = 0, al = 0, us = 0;
    if (!own_q(btn + 0x1e8, &arr) || !own_q(btn + 0x1f0, &al) || !own_q(btn + 0x1f8, &us) ||
        !arr)
        return 0;
    int alloc = (int) (uint32_t) al, used = (int) (uint32_t) us;
    if (used > want_idx) { /* already there? must be ours */
        uintptr_t cur = 0;
        own_q(arr + (uintptr_t) want_idx * 0x18, &cur);
        return cur == set[0];
    }
    if (used != want_idx)
        return 0; /* stock count changed: do not guess */
    if (used >= alloc) {
        void *n = realloc((void *) arr, (size_t) (used + 2) * 0x18); /* JUCE frees with free() */
        if (!n)
            return 0;
        arr = (uintptr_t) n;
        *(uintptr_t *) (btn + 0x1e8) = arr;
        *(int32_t *) (btn + 0x1f0) = used + 2;
    }
    for (int s = 0; s < 3; s++) {
        if (take_extra_ref)
            mp_incref(set[s]);
        *(uintptr_t *) (arr + (uintptr_t) used * 0x18 + (uintptr_t) s * 8) = set[s];
    }
    *(int32_t *) (btn + 0x1f8) = used + 1;
    return 1;
}
/* The name slot is swapped per target, so "already there" cannot be tested by
 * pointer: it is ours if it exists at all (we are the only ones who append). */
static int mp_valid_slot(uintptr_t btn, int idx) {
    uintptr_t us = 0;
    return own_q(btn + 0x1f8, &us) && (int) (uint32_t) us > idx;
}
/* B1: [0..2] stock, [3] UNPLAYED (B2's own set), [4] ADD, [5] ADD <name>, [6] REMOVE ALL.
 * B2: [0..2] stock, [3] REMOVE, [4] ADD, [5] ADD <name>, [6] REMOVE. */
static int mp_prepare_buttons(uintptr_t b1, uintptr_t b2) {
    if (!mp_load_images())
        return 0;
    uintptr_t unplayed[3] = {0, 0, 0}, arr2 = 0;
    if (!own_q(b2 + 0x1e8, &arr2) || !arr2)
        return 0;
    for (int s = 0; s < 3; s++)
        own_q(arr2 + (uintptr_t) s * 8, &unplayed[s]); /* B2 set 0 = UNPLAYED */
    int ok =
        mp_ensure_set(b1, MP_SET_UNPLAYED, unplayed, 1) &&
        mp_ensure_set(b1, MP_SET_ADD, g_mp_img[0], 1) &&
        (mp_ensure_set(b1, MP_SET_ADDNAME, g_mp_img[0], 1) || mp_valid_slot(b1, MP_SET_ADDNAME)) &&
        mp_ensure_set(b2, MP_SET_REMOVE, g_mp_img[1], 1) &&
        mp_ensure_set(b2, MP_SET_ADD, g_mp_img[0], 1) &&
        (mp_ensure_set(b2, MP_SET_ADDNAME, g_mp_img[0], 1) || mp_valid_slot(b2, MP_SET_ADDNAME)) &&
        mp_ensure_set(b1, MP_SET_RELABEL, g_mp_img[4], 1) &&
        mp_ensure_set(b2, MP_SET_RELABEL, g_mp_img[3], 1) &&
        mp_ensure_set(b2, MP_SET_RELABEL + 1, g_mp_img210[3], 1) /* REMOVE (playlist row) at 210 */
        &&
        mp_ensure_set(b2, MP_SET_RELABEL + 2, g_mp_img210[1], 1) /* REMOVE FROM PLAYLIST at 210 */
        && mp_ensure_set(b1, MP_SET_RELABEL + 1, g_mp_img210[MP_IMG_REMALL],
                         1) /* TAG LIST: REMOVE ALL TRACKS */
        && mp_ensure_set(b2, MP_SET_RELABEL + 3, g_mp_img210[MP_IMG_CREATEPL],
                         1) /* TAG LIST: CREATE PLAYLIST */
        && mp_ensure_set(b1, MP_SET_RELABEL + 2, g_mp_img210[MP_IMG_PLAYED],
                         1) /* other tabs: PLAYED at 210 (B1's next free set) */
        && mp_ensure_set(b2, MP_SET_RELABEL + 4, g_mp_img210[MP_IMG_UNPLAYED],
                         1) /* ...UNPLAYED at 210 */
        && mp_ensure_set(b2, MP_SET_RELABEL + 5, g_mp_img210[MP_IMG_DELF],
                         1); /* a folder's REMOVE: gray (FOLDER) */
    if (!ok)
        mp_log("prepare refused: b1=%lx b2=%lx (label array not as expected)\n", (unsigned long) b1,
               (unsigned long) b2);
    return ok;
}
static void mp_show(uintptr_t btn, int set, int enabled, int visible) {
    *(int32_t *) (btn + 0x1e0) = set;
    ((void (*)(uintptr_t)) MP_REPAINT)(btn);
    ((void (*)(uintptr_t, int)) MP_SETENABLED)(btn, enabled);
    uintptr_t vt = 0, fn = 0;
    if (own_q(btn, &vt) && own_q(vt + 0x60, &fn) && fn)
        ((void (*)(uintptr_t, int)) fn)(btn, visible); /* setVisible, as stock */
}
/* Item flags from the controller's copy of the model: {u8 id, u8 enabled}[]. */
static int mp_item_enabled(uintptr_t ctrl, int id) {
    uintptr_t items = 0, cnt = 0;
    if (!own_q(ctrl + 0x20, &items) || !own_q(ctrl + 0x30, &cnt) || !items)
        return 0;
    for (int i = 0; i < (int) (uint32_t) cnt && i < 16; i++) {
        uintptr_t w = 0;
        uintptr_t a = items + (uintptr_t) i * 2;
        if (!own_q(a & ~7UL, &w))
            return 0;
        unsigned v = (unsigned) (w >> ((a & 7) * 8)) & 0xffff;
        if ((int) (signed char) (v & 0xff) == id)
            return (v >> 8) & 0xff;
    }
    return 0;
}
/* The same eligibility pl_request_remove() applies (so the button never offers
 * what the request will refuse): JUCE's selected row == the hierarchy's row. */
static int mp_remove_eligible(void) {
    struct plsnap sn = plsnap_get();
    int sel = tracklist_selected_row();
    return sn.playlist_id && sel >= 0 && (unsigned) sel == sn.row;
}

/* ==== Native buttons of our own (B3 opt-out: azmod-menub3.off) =================
 * gui::ToggleImagesButton has no out-of-line ctor (every site inlines it), so
 * this replicates the widget ctor's own inline sequence (0x1b57ec0..0x1b57fdc):
 *   new(0x220); juce::Button base ctor 0x24924f0(p, &name); juce::Timer ctor
 *   0x2398900(p+0x1b8); vtables +0 0x29674c8, +0xc8 0x2967678, +0x1b8 0x29676a8;
 *   name String copy 0x230e3e0(p+0x1d8, &name); label sets {+0x1e8 ptr, +0x1f0
 *   alloc, +0x1f8 count}, set index +0x1e0; +0x200 u16 0, +0x202 u8 0,
 *   +0x204 int 500; its listener list {+0x208 ptr, +0x210 alloc, +0x218 count}
 *   (on B1 it holds the controller); 0x243d1d0(p, &name).
 * then addAndMakeVisible 0x249d6b0(widget, p, -1), setBounds 0x249efe0.
 * Lifetime: the widget's dtor deletes only B1/B2 and un-parents other children,
 * so B3 is never double-freed; it is re-made only if the widget is, and checked
 * (vtable + parent) at every use. Believed touch-only (not knob-reachable). */
#define MP_B3_OFF_FLAG "/home/root/settings/azmod-menub3.off" /* escape hatch: no B3 (COPY/PASTE) */
#define MP_B3_SET_COPY 0  /* "COPY"  (menulabels.h ML_COPY_*) */
#define MP_B3_SET_PASTE 1 /* "PASTE" (menulabels.h ML_PASTE_*) */
#define MP_B3_COPY 5      /* g_mp_b3_action */
#define MP_B3_PASTE 6
#define MP_B3_TAGCOPY 7  /* TAG LIST: COPY / <n> TAGGED */
#define MP_B3_SET_TAGN 4 /* after COPY, PASTE and their 210 twins: the runtime COPY / <n> TAGGED   \
                          */
static int g_mp_tag_rows;
#define MP_BA_SET_ADD 0 /* ADD TO: "ADD TO / PLAYLIST" (static), then the name slot */
#define MP_BA_SET_NAME 1
#define MP_BUTTON_CTOR EP_24924f0
#define MP_TIMER_CTOR EP_2398900
#define MP_STRING_COPY EP_230e3e0
#define MP_BTN_SETNAME EP_243d1d0
#define MP_ADD_CHILD EP_249d6b0  /* juce::Component::addAndMakeVisible(child, zOrder) */
#define MP_SET_BOUNDS EP_249efe0 /* juce::Component::setBounds(x, y, w, h) */
#define MP_TIB_VT0 EP_29674c8
#define MP_TIB_VT1 EP_2967678
#define MP_TIB_VT2 EP_29676a8
static int g_mp_b3_on;
static int
    g_mp_b3_action; /* 0 hidden, MP_B3_COPY, MP_B3_PASTE or MP_B3_TAGCOPY */
static uintptr_t g_mp_b3, g_mp_b3_widget;
static struct bn_holder g_mp_b3_name;
static uintptr_t g_mp_b3_name_s;
/* PASTE (button named AZ_MOVE): pastes the clipboard here. */
static uintptr_t g_mp_bm, g_mp_bm_widget;
static int g_mp_bm_on;
static struct bn_holder g_mp_bm_name;
static uintptr_t g_mp_bm_name_s;
/* ADD TO <target>: adds the selected track to the target set by [SELECT] or the last
 * PASTE, so a batch add needs no re-selecting. */
static uintptr_t g_mp_ba, g_mp_ba_widget;
static int g_mp_ba_on;
static struct bn_holder g_mp_ba_name;
static uintptr_t g_mp_ba_name_s;
/* One of our native buttons on the panel. `name_s` is its juce::String name (also what a
 * click identifies it by), `sets` the g_mp_img label sets it holds, in order. */
static const uintptr_t
    *g_mp_mk_single; /* nsets == 0: this one label set instead (the small UNDO) */
static uintptr_t mp_make_btn(uintptr_t ctrl, uintptr_t widget, uintptr_t b1, uintptr_t *name_s,
                             const int *sets, int nsets) {
    if (!name_s || !*name_s) {
        mp_log("button: no name string -- refusing\n");
        return 0;
    } /* an unset name crashes EP147 */
    /* where juce keeps parentComponent: find it on B1, whose parent is the widget */
    static int parent_off = -1;
    if (parent_off < 0) {
        for (int o = 0x8; o < 0x100; o += 8) {
            uintptr_t v = 0;
            if (own_q(b1 + (uintptr_t) o, &v) && v == widget) {
                parent_off = o;
                break;
            }
        }
        if (parent_off < 0) {
            mp_log("B3: no parent pointer found on B1 -- refusing\n");
            parent_off = 0;
        }
    }
    if (!parent_off)
        return 0;
    uintptr_t p = (uintptr_t) calloc(1, 0x220); /* operator new == malloc on this libstdc++ */
    if (!p)
        return 0;
    ((void (*)(uintptr_t, uintptr_t *)) MP_BUTTON_CTOR)(p, name_s);
    ((void (*)(uintptr_t)) MP_TIMER_CTOR)(p + 0x1b8);
    *(uintptr_t *) (p + 0x000) = MP_TIB_VT0;
    *(uintptr_t *) (p + 0x0c8) = MP_TIB_VT1;
    *(uintptr_t *) (p + 0x1b8) = MP_TIB_VT2;
    *(uintptr_t *) (p + 0x1cc) = 0;
    ((void (*)(uintptr_t, uintptr_t *)) MP_STRING_COPY)(p + 0x1d8, name_s);
    *(int32_t *) (p + 0x1e0) = 0;
    *(uintptr_t *) (p + 0x1e8) = 0;
    *(int32_t *) (p + 0x1f0) = 0;
    *(int32_t *) (p + 0x1f8) = 0;
    *(uint16_t *) (p + 0x200) = 0;
    *(uint8_t *) (p + 0x202) = 0;
    *(int32_t *) (p + 0x204) = 500;
    *(int32_t *) (p + 0x208 + 0) = 0;
    uintptr_t *ls = (uintptr_t *) malloc(8 * sizeof(uintptr_t));
    if (!ls)
        return 0;
    ls[0] = ctrl;
    *(uintptr_t *) (p + 0x208) = (uintptr_t) ls;
    *(int32_t *) (p + 0x210) = 8;
    *(int32_t *) (p + 0x218) = 1;
    ((void (*)(uintptr_t, uintptr_t *)) MP_BTN_SETNAME)(p, name_s);
    /* label sets: an empty array grows through the same helper (want_idx 0) */
    uintptr_t *arr = (uintptr_t *) malloc(3 * 0x18);
    if (!arr)
        return 0;
    *(uintptr_t *) (p + 0x1e8) = (uintptr_t) arr;
    *(int32_t *) (p + 0x1f0) = 3;
    *(int32_t *) (p + 0x1f8) = 0;
    if (!mp_load_images())
        return 0;
    if (nsets == 0 && g_mp_mk_single && !mp_ensure_set(p, 0, g_mp_mk_single, 1)) {
        mp_log("button: single set failed\n");
        return 0;
    }
    for (int k = 0; k < nsets; k++) /* sets 0..n-1 at 246 px, then their 210 px twins */
        if (!mp_ensure_set(p, k, g_mp_img[sets[k]], 1)) {
            mp_log("button: label set failed\n");
            return 0;
        }
    for (int k = 0; k < nsets; k++)
        if (!mp_ensure_set(p, nsets + k, g_mp_img210[sets[k]], 1)) {
            mp_log("button: 210 set failed\n");
            return 0;
        }
    ((void (*)(uintptr_t, uintptr_t, int)) MP_ADD_CHILD)(widget, p, -1);
    uintptr_t par = 0;
    own_q(p + (uintptr_t) parent_off, &par);
    mp_log("B3 made: %lx (parent %lx, want widget %lx; parent at +%#x)\n", (unsigned long) p,
           (unsigned long) par, (unsigned long) widget, parent_off);
    if (par != widget)
        return 0;
    return p;
}
static uintptr_t mp_b3(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (!g_mp_b3_on)
        return 0;
    if (g_mp_b3 && g_mp_b3_widget == widget && mp_valid(g_mp_b3, MP_TIB_VT0))
        return g_mp_b3;
    static const int sets[2] = {10, 11}; /* MP_B3_SET_COPY, _PASTE */
    g_mp_b3 = mp_make_btn(ctrl, widget, b1, &g_mp_b3_name_s, sets, 2);
    g_mp_b3_widget = g_mp_b3 ? widget : 0;
    return g_mp_b3;
}
/* MAKE STEMS: its own button (one label set), so it never takes a slot Playlist
 * Editing uses (the panel lays out whichever buttons are showing). */
static uintptr_t g_mp_bs, g_mp_bs_widget;
static int g_mp_bs_on; /* shown for the current menu */
static int g_mp_bs_pl; /* ...on a PLAYLIST row: the whole playlist (folder: everything under it) */
static struct bn_holder g_mp_bs_name;
static uintptr_t g_mp_bs_name_s;
/* NEW PLAYLIST (Playlist Editing): on a playlist-list row; names it with EP147's own
 * search field + keyboard (playlist_editor.c nl_begin), created beside the selected row. */
static uintptr_t g_mp_bn, g_mp_bn_widget;
static int g_mp_bn_on;
static struct bn_holder g_mp_bn_name;
static uintptr_t g_mp_bn_name_s;
static uintptr_t mp_make_btn(uintptr_t ctrl, uintptr_t widget, uintptr_t b1, uintptr_t *name_s,
                             const int *sets, int nsets);
static uintptr_t mp_bn(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (g_mp_bn && g_mp_bn_widget == widget && mp_valid(g_mp_bn, MP_TIB_VT0))
        return g_mp_bn;
    static const int sets[1] = {6}; /* NEWPL */
    g_mp_bn = mp_make_btn(ctrl, widget, b1, &g_mp_bn_name_s, sets, 1);
    g_mp_bn_widget = g_mp_bn ? widget : 0;
    return g_mp_bn;
}
static uintptr_t mp_bm(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (g_mp_bm && g_mp_bm_widget == widget && mp_valid(g_mp_bm, MP_TIB_VT0))
        return g_mp_bm;
    static const int sets[2] = {
        11 /* PASTE */,
        MP_IMG_PASTEN}; /* PASTE (nothing copied, grayed); PASTE + what is copied,
                         * gray, drawn into set 3 (the PASTEN 210 twin) */
    g_mp_bm = mp_make_btn(ctrl, widget, b1, &g_mp_bm_name_s, sets, 2);
    g_mp_bm_widget = g_mp_bm ? widget : 0;
    return g_mp_bm;
}
/* PASTE, shown: with something copied that can land, its gray second line says what
 * (a track's title, a playlist's name, "<n> TAGGED"); else plain PASTE, grayed. */
static void mp_show_paste(uintptr_t bm, int can) {
    char t[96] = "";
    if (can) {
        if (g_clip.kind == CLIP_TAGGED)
            snprintf(t, sizeof t, "%d TAGGED", g_tag_n);
        else
            snprintf(t, sizeof t, "%s", g_clip.name);
    }
    const uintptr_t *d = t[0] ? mp_dyn2_images(&g_mp_pastedyn, t, MP_IMG_PASTEN) : 0;
    if (d && mp_swap_set(bm, 3, d))
        mp_show(bm, 3, 1, 1);
    else
        mp_show(bm, 0, can, 1);
}
static uintptr_t mp_ba(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (g_mp_ba && g_mp_ba_widget == widget && mp_valid(g_mp_ba, MP_TIB_VT0))
        return g_mp_ba;
    static const int sets[2] = {0, 0}; /* "ADD TO / PLAYLIST", then the name slot */
    g_mp_ba = mp_make_btn(ctrl, widget, b1, &g_mp_ba_name_s, sets, 2);
    g_mp_ba_widget = g_mp_ba ? widget : 0;
    return g_mp_ba;
}
/* SELECT PLAYLIST: makes the row's
 * playlist the ADD TO target, so tracks can be added from any list without going
 * back to it. A PASTE also moves the target (the last paste spot); both feed one. */
static uintptr_t g_mp_bt, g_mp_bt_widget;
static int g_mp_bt_on, g_mp_bt_sel;
static struct bn_holder g_mp_bt_name;
static uintptr_t g_mp_bt_name_s;
static uintptr_t mp_bsel(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (g_mp_bt && g_mp_bt_widget == widget && mp_valid(g_mp_bt, MP_TIB_VT0))
        return g_mp_bt;
    static const int sets[2] = {MP_IMG_SELECT, MP_IMG_SELECTED}; /* [SELECT] / [SELECTED] */
    g_mp_bt = mp_make_btn(ctrl, widget, b1, &g_mp_bt_name_s, sets, 2);
    g_mp_bt_widget = g_mp_bt ? widget : 0;
    return g_mp_bt;
}
/* NEW (opens NEW PLAYLIST / NEW FOLDER in place) and RENAME (the
 * two NEW buttons are merged to make room for RENAME). */
static uintptr_t g_mp_bnm, g_mp_bnm_widget, g_mp_brn, g_mp_brn_widget;
static int g_mp_bnm_on, g_mp_brn_on, g_mp_newmode;
static struct bn_holder g_mp_bnm_name, g_mp_brn_name;
static uintptr_t g_mp_bnm_name_s, g_mp_brn_name_s;
static uintptr_t mp_make_btn(uintptr_t ctrl, uintptr_t widget, uintptr_t b1, uintptr_t *name_s,
                             const int *sets, int nsets);
static uintptr_t mp_bnm(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (g_mp_bnm && g_mp_bnm_widget == widget && mp_valid(g_mp_bnm, MP_TIB_VT0))
        return g_mp_bnm;
    static const int sets[1] = {MP_IMG_NEWMENU};
    g_mp_bnm = mp_make_btn(ctrl, widget, b1, &g_mp_bnm_name_s, sets, 1);
    g_mp_bnm_widget = g_mp_bnm ? widget : 0;
    return g_mp_bnm;
}
/* EDIT (groups the edit actions to declutter the row): opens RENAME / REMOVE in place,
 * like NEW. */
static uintptr_t g_mp_bed, g_mp_bed_widget;
static int g_mp_bed_on, g_mp_editmode;
static struct bn_holder g_mp_bed_name;
static uintptr_t g_mp_bed_name_s;
static uintptr_t mp_bed(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (g_mp_bed && g_mp_bed_widget == widget && mp_valid(g_mp_bed, MP_TIB_VT0))
        return g_mp_bed;
    static const int sets[1] = {MP_IMG_EDIT};
    g_mp_bed = mp_make_btn(ctrl, widget, b1, &g_mp_bed_name_s, sets, 1);
    g_mp_bed_widget = g_mp_bed ? widget : 0;
    return g_mp_bed;
}
static uintptr_t mp_brn(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (g_mp_brn && g_mp_brn_widget == widget && mp_valid(g_mp_brn, MP_TIB_VT0))
        return g_mp_brn;
    static const int sets[1] = {MP_IMG_RENAME};
    g_mp_brn = mp_make_btn(ctrl, widget, b1, &g_mp_brn_name_s, sets, 1);
    g_mp_brn_widget = g_mp_brn ? widget : 0;
    return g_mp_brn;
}
/* NEW FOLDER: NEW PLAYLIST's flow with the folder flag set. */
static uintptr_t g_mp_bf, g_mp_bf_widget;
static int g_mp_bf_on;
static struct bn_holder g_mp_bf_name;
static uintptr_t g_mp_bf_name_s;
static uintptr_t mp_bf(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (g_mp_bf && g_mp_bf_widget == widget && mp_valid(g_mp_bf, MP_TIB_VT0))
        return g_mp_bf;
    static const int sets[1] = {7}; /* NEWFOLDER */
    g_mp_bf = mp_make_btn(ctrl, widget, b1, &g_mp_bf_name_s, sets, 1);
    g_mp_bf_widget = g_mp_bf ? widget : 0;
    return g_mp_bf;
}
static uintptr_t mp_bs(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (g_mp_bs && g_mp_bs_widget == widget && mp_valid(g_mp_bs, MP_TIB_VT0))
        return g_mp_bs;
    static const int sets[1] = {5}; /* STEMS */
    g_mp_bs = mp_make_btn(ctrl, widget, b1, &g_mp_bs_name_s, sets, 1);
    g_mp_bs_widget = g_mp_bs ? widget : 0;
    return g_mp_bs;
}
/* A six-button row: show each button's 210 px twin (the art is drawn at native size
 * and clipped, so 246 art in a 210 button loses its right edge). Our buttons hold
 * their n static sets at 0..n-1 and the twins at n..2n-1; B2's relabels have twins
 * at MP_SET_RELABEL+1 (REMOVE) and +2 (REMOVE FROM PLAYLIST). Stock art (PLAYED)
 * has none and stays as it is. */
static uintptr_t g_mp_b3, g_mp_bm, g_mp_bt, g_mp_bn, g_mp_bf, g_mp_bs, g_mp_bnm, g_mp_brn;
static int mp_twin_of(uintptr_t b, uintptr_t b2);
static void mp_narrow(uintptr_t b, uintptr_t b2) {
    int to = mp_twin_of(b, b2);
    if (to < 0)
        return;
    uintptr_t us = 0;
    if (!own_q(b + 0x1f8, &us) || (int) (uint32_t) us <= to)
        return;
    *(int32_t *) (b + 0x1e0) = to;
    ((void (*)(uintptr_t)) MP_REPAINT)(b);
}
static int mp_twin_of(uintptr_t b, uintptr_t b2) {
    int n = b == g_mp_b3 || b == g_mp_ba || b == g_mp_bt || b == g_mp_bm ? 2
            : b == g_mp_bn || b == g_mp_bf || b == g_mp_bs || b == g_mp_bnm || b == g_mp_brn ||
                    b == g_mp_bed
                ? 1
                : 0;
    int cur = *(int32_t *) (b + 0x1e0);
    if (n && cur >= 0 && cur < n)
        return cur + n; /* a 246 set: its twin */
    if (n && cur >= n && cur < 2 * n)
        return cur; /* already the 210 twin */
    if (b == g_mp_b3 && cur == MP_B3_SET_TAGN)
        return cur; /* COPY / <n> TAGGED: rendered at 210 */
    if (b == b2 && cur == MP_SET_RELABEL)
        return MP_SET_RELABEL + 1;
    if (b == b2 && cur == MP_SET_REMOVE)
        return MP_SET_RELABEL + 2;
    if (b == b2 && cur >= MP_SET_RELABEL + 1 && cur <= MP_SET_RELABEL + 5)
        return cur; /* B2's 210 sets */
    if (b != b2 && cur == MP_SET_RELABEL + 2)
        return cur; /* B1: PLAYED replica at 210 */
    if (b != b2 && cur == MP_SET_RELABEL + 1)
        return cur; /* B1: REMOVE ALL TRACKS replica */
    return -1;
}
static int mp_has_twin(uintptr_t b, uintptr_t b2) {
    return mp_twin_of(b, b2) >= 0;
}
/* UNDO: a small button in the panel's top-right corner, where the stock X was. Shown
 * on every menu but FOLDER (with nothing to undo, pressing it says so); the undo itself
 * re-proves the rows it restores. */
static uintptr_t g_mp_bu, g_mp_bu_widget;
static int g_mp_bu_on;
static struct bn_holder g_mp_bu_name;
static uintptr_t g_mp_bu_name_s;
static uintptr_t g_mp_undo_img[3];
static uintptr_t mp_bu(uintptr_t ctrl, uintptr_t widget, uintptr_t b1) {
    if (g_mp_bu && g_mp_bu_widget == widget && mp_valid(g_mp_bu, MP_TIB_VT0))
        return g_mp_bu;
    if (!g_mp_undo_img[0]) {
        static const unsigned char *const src[3] = {ML_UNDO_SMALL_normal, ML_UNDO_SMALL_down,
                                                    ML_UNDO_SMALL_disabled};
        static const int len[3] = {sizeof ML_UNDO_SMALL_normal, sizeof ML_UNDO_SMALL_down,
                                   sizeof ML_UNDO_SMALL_disabled};
        uintptr_t im[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++) {
            uintptr_t q = 0;
            mp_image_from_memory(&im[k], src[k], len[k]);
            if (!im[k] || !own_q(im[k] + 0x10, &q) || (uint32_t) q != MP_UNDO_W ||
                (uint32_t) (q >> 32) != MP_UNDO_H) {
                mp_log("UNDO label load failed k=%d\n", k);
                return 0;
            }
        }
        memcpy(g_mp_undo_img, im, sizeof im);
    }
    g_mp_mk_single = g_mp_undo_img;
    g_mp_bu = mp_make_btn(ctrl, widget, b1, &g_mp_bu_name_s, 0, 0);
    g_mp_bu_widget = g_mp_bu ? widget : 0;
    g_mp_mk_single = 0;
    return g_mp_bu;
}
/* The stock X (close) in the panel's top-right corner. It is redundant
 * (BACK closes the menu), so it is hidden and the small UNDO takes its corner.
 * Found as a child of the widget by its bounds (a <= 40 px square right of x 1150);
 * the child list is the juce::Array {data, alloc, used} whose data holds B1. Any
 * doubt = left alone. Returns its bounds for UNDO to align to. */
static int mp_hide_x(uintptr_t w, uintptr_t b1, int *xr, int *yc) {
    static int arr_off = -1;
    if (arr_off < 0) {
        for (int o = 0x20; o < 0x140 && arr_off < 0; o += 8) {
            uintptr_t
                d = 0,
                cu = 0; /* juce ArrayBase: elements, int numAllocated (+pad), int numUsed at +16 */
            if (!own_q(w + (uintptr_t) o, &d) || d < 0x10000 || (d & 7) ||
                !own_q(w + (uintptr_t) o + 16, &cu))
                continue;
            int used = (int) (uint32_t) cu;
            if (used < 2 || used > 32)
                continue;
            for (int i = 0; i < used; i++) {
                uintptr_t ch = 0;
                if (own_q(d + (uintptr_t) i * 8, &ch) && ch == b1) {
                    arr_off = o;
                    break;
                }
            }
        }
        mp_log("X: child list at %d\n", arr_off);
        if (arr_off < 0) {
            arr_off = 0;
            return 0;
        }
    }
    if (!arr_off)
        return 0;
    uintptr_t d = 0, cu = 0;
    if (!own_q(w + (uintptr_t) arr_off, &d) || !own_q(w + (uintptr_t) arr_off + 16, &cu) || !d)
        return 0;
    int used = (int) (uint32_t) cu, found = 0;
    for (int i = 0; i < used && i < 32; i++) {
        uintptr_t ch = 0, xy = 0, wh = 0, vt = 0, fn = 0;
        if (!own_q(d + (uintptr_t) i * 8, &ch) || !ch || (ch & 7) || !own_q(ch + 0x20, &xy) ||
            !own_q(ch + 0x28, &wh))
            continue;
        int x = (int) (uint32_t) xy, y = (int) (uint32_t) (xy >> 32), cw = (int) (uint32_t) wh,
            chh = (int) (uint32_t) (wh >> 32);
        if (x < 1150 || cw < 12 || cw > 40 || chh < 12 || chh > 40 || y > 40)
            continue;
        static int logged;
        if (!logged++) {
            own_q(ch, &vt);
            mp_log("X: child %lx vt %lx at %d,%d %dx%d -> hidden\n", (unsigned long) ch,
                   (unsigned long) vt, x, y, cw, chh);
        }
        if (own_q(ch, &vt) && own_q(vt + 0x60, &fn) && fn)
            ((void (*)(uintptr_t, int)) fn)(ch, 0); /* setVisible(false) */
        *xr = x + cw;
        *yc = y + chh / 2;
        found = 1;
    }
    return found;
}
#define MP_SLOT_X(i) \
    ((1280 - (5 * 212 + 210)) / 2 + (i) * 212) /* the six fixed slots */
static void mp_bounds_w(uintptr_t c, int x, int w) {
    ((void (*)(uintptr_t, int, int, int, int)) MP_SET_BOUNDS)(c, x, 36, w, 80);
}
static void mp_bounds(uintptr_t c, int x) {
    mp_bounds_w(c, x, 246);
}

static void mp_rebuild_pledit(void *ctrl);
static void mp_place_stems(uintptr_t c);
static void mp_rebuild_hook(void *ctrl) {
    g_mp_orig_rebuild(ctrl); /* stock first, always */
    mp_note_ctrl((uintptr_t) ctrl);
    g_mp_b1_unplayed = 0;
    g_mp_b2_action = 0;
    g_mp_b3_action = 0;
    g_mp_bs_on = 0;
    g_mp_bn_on = 0;
    g_mp_bf_on = 0;
    g_mp_bm_on = 0;
    g_mp_ba_on = 0;
    g_mp_bt_on = 0;
    g_mp_bnm_on = 0;
    g_mp_brn_on = 0;
    g_mp_newmode = 0;
    g_mp_bed_on = 0;
    g_mp_editmode = 0;
    __atomic_store_n(&g_mp_submode, 0, __ATOMIC_RELEASE);
    if (g_mp_bed && mp_valid(g_mp_bed, MP_TIB_VT0))
        mp_show(g_mp_bed, 0, 0, 0);
    if (g_mp_bnm && mp_valid(g_mp_bnm, MP_TIB_VT0))
        mp_show(g_mp_bnm, 0, 0, 0);
    if (g_mp_brn && mp_valid(g_mp_brn, MP_TIB_VT0))
        mp_show(g_mp_brn, 0, 0, 0);
    if (g_mp_bm && mp_valid(g_mp_bm, MP_TIB_VT0))
        mp_show(g_mp_bm, 0, 0, 0); /* default: hidden */
    if (g_mp_ba && mp_valid(g_mp_ba, MP_TIB_VT0))
        mp_show(g_mp_ba, 0, 0, 0);
    if (g_mp_bt && mp_valid(g_mp_bt, MP_TIB_VT0))
        mp_show(g_mp_bt, 0, 0, 0);
    if (mp_enabled())
        mp_rebuild_pledit(ctrl);      /* Playlist Editing's buttons, on its own gate */
    mp_place_stems((uintptr_t) ctrl); /* MAKE STEMS, on its own gate */
    g_mp_bu_on = 0;
    if (g_mp_bu && mp_valid(g_mp_bu, MP_TIB_VT0))
        mp_show(g_mp_bu, 0, 0, 0);
    if (mp_enabled()) {
        uintptr_t c = (uintptr_t) ctrl, b1 = 0, b2 = 0, ty = 0, w = 0;
        int type =
            mp_parts(c, &b1, &b2) && own_q(c + 0x18, &ty) ? (int) (signed char) (ty & 0xff) : -1;
        int xr = MP_UNDO_X_CORNER + MP_UNDO_W, yc = MP_UNDO_Y + MP_UNDO_H / 2;
        if (type >= 0 && own_q(c + 0xa0, &w) && mp_hide_x(w, b1, &xr, &yc)) {
            xr -= 0;
        }        /* every menu: BACK closes it */
        /* every menu but FOLDER (a file copy is not in the undo journal); with nothing to undo
         * it says "Nothing to undo" */
        if (w && type != MP_TYPE_FOLDER) {
            uintptr_t bu = mp_bu(c, w, b1); /* the small UNDO, in the X's corner */
            if (bu) {
                ((void (*)(uintptr_t, int, int, int, int)) MP_SET_BOUNDS)(
                    bu, xr - MP_UNDO_W, yc - MP_UNDO_H / 2, MP_UNDO_W, MP_UNDO_H);
                mp_show(bu, 0, 1, 1);
                g_mp_bu_on = 1;
            }
        }
    }
    if (mp_enabled()) {
        uintptr_t ty = 0;
        if (own_q((uintptr_t) ctrl + 0x18, &ty))
            mp_title_update((uintptr_t) ctrl, (int) (signed char) (ty & 0xff));
    }
}
static void mp_rebuild_pledit(void *ctrl) {
    uintptr_t c = (uintptr_t) ctrl, b1 = 0, b2 = 0, ty = 0;
    if (!mp_parts(c, &b1, &b2) || !own_q(c + 0x18, &ty))
        return;
    {
        uintptr_t w = 0;
        own_q(c + 0xa0, &w); /* default: stock 2-button layout, B3 hidden */
        if (g_mp_b3 && g_mp_b3_widget == w && mp_valid(g_mp_b3, MP_TIB_VT0)) {
            mp_bounds(b1, 358);
            mp_bounds(b2, 676);
            mp_show(g_mp_b3, 0, 0, 0);
        }
    }
    int type = (int) (signed char) (ty & 0xff);
    if (type == 3) { /* TAG LIST: stock REMOVE ALL TRACKS | CREATE PLAYLIST, + COPY <n> TAGGED */
        int rows = tracklist_showing() ? drag_num_rows() : 0;
        uintptr_t w = 0;
        own_q(c + 0xa0, &w);
        if (mp_prepare_buttons(b1,
                               b2)) { /* our 210 replicas of the stock labels: one look per row */
            *(int32_t *) (b1 + 0x1e0) = MP_SET_RELABEL + 1;
            ((void (*)(uintptr_t)) MP_REPAINT)(b1);
            *(int32_t *) (b2 + 0x1e0) = MP_SET_RELABEL + 3;
            ((void (*)(uintptr_t)) MP_REPAINT)(b2);
        }
        uintptr_t b3 = rows > 0 ? mp_b3(c, w, b1) : 0;
        if (!b3) {
            mp_log("REBUILD type 3: stock kept (%d rows)\n", rows);
            return;
        }
        const uintptr_t *img = mp_tag_images(rows);
        int ok =
            img && (mp_valid_slot(b3, MP_B3_SET_TAGN) ? mp_swap_set(b3, MP_B3_SET_TAGN, img)
                                                      : mp_ensure_set(b3, MP_B3_SET_TAGN, img, 1));
        mp_show(b3, ok ? MP_B3_SET_TAGN : MP_B3_SET_COPY, 1, 1);
        g_mp_b3_action = MP_B3_TAGCOPY;
        g_mp_tag_rows = rows;
        mp_log("REBUILD type 3: COPY %d TAGGED (%s label)\n", rows, ok ? "runtime" : "static");
        return;
    }
    if (type == 1) { /* stock ALL DELETE | DELETE, on the PLAYLIST list only; HISTORY stays stock */
        /* Say REMOVE, not DELETE. Label only: stock's enables,
         * visibility, click and confirm dialog are untouched. */
        if (mp_prepare_buttons(b1, b2)) {
            *(int32_t *) (b1 + 0x1e0) = MP_SET_RELABEL;
            ((void (*)(uintptr_t)) MP_REPAINT)(b1);
            *(int32_t *) (b2 + 0x1e0) = MP_SET_RELABEL;
            ((void (*)(uintptr_t)) MP_REPAINT)(b2);
        }
        /* REMOVE ALL is not offered. Stock ALL DELETE wipes every playlist
         * and folder on the USB whatever level is browsed, behind a
         * box that only says "ALL DELETE". Hidden + disabled here, and its click is
         * refused in mp_click_hook in case anything still reaches it. */
        mp_show(b1, MP_SET_RELABEL, 0, 0);
        mp_bounds(b2, 517); /* REMOVE alone, centered */
        struct plsnap sn = plsnap_get();
        uintptr_t w = 0;
        own_q(c + 0xa0, &w);
        if (!sn.sel_pl || !sn.any_media || type != 1) {
            mp_log("REBUILD type %d: stock kept (no playlist row / history)\n", type);
            return;
        }
        /* REMOVE is ours for folders and playlists: stock's DELETE writes
         * outside the undo journal, so UNDO would have nothing to restore. Same count -> box ->
         * one-transaction delete, journaled. */
        g_mp_b2_action = sn.sel_folder ? 3 : 4;
        mp_show(b2, MP_SET_RELABEL, 1, 0); /* REMOVE: in EDIT's row (mp_edit_row), not this one */
        /* Row: COPY | PASTE | [SELECT] | NEW > | EDIT > | MAKE STEMS, fixed slots
         * (mp_place_stems). PASTE is grayed until something that can land here is copied:
         * any playlist/folder, or tracks onto a PLAYLIST row (a folder holds no tracks). */
        uintptr_t b3 = mp_b3(c, w, b1);
        if (b3) {
            mp_show(b3, MP_B3_SET_COPY, 1, 1);
            g_mp_b3_action = MP_B3_COPY;
        } /* folders copy too */
        uintptr_t bm = mp_bm(c, w, b1);
        int can = g_clip.kind &&
                  !(sn.sel_folder && (g_clip.kind == CLIP_TRACK || g_clip.kind == CLIP_TAGGED));
        if (bm) {
            mp_show_paste(bm, can);
            g_mp_bm_on = 1;
        }
        uintptr_t bt = sn.sel_folder ? 0 : mp_bsel(c, w, b1);     /* [SELECT]: playlists only */
        uintptr_t bnm = mp_bnm(c, w, b1), bed = mp_bed(c, w, b1); /* NEW >, EDIT > */
        if (bnm) {
            mp_show(bnm, 0, 1, 1);
            g_mp_bnm_on = 1;
        }
        if (bed) {
            mp_show(bed, 0, 1, 1);
            g_mp_bed_on = 1;
        }
        if (bt) {
            struct pl_addtgt *tg = pl_target_for(sn.any_media);
            g_mp_bt_sel = tg && tg->pl == sn.sel_pl &&
                          tg->attr == PL_ATTR_LIST; /* this row is the selection */
            mp_show(bt, g_mp_bt_sel, 1, 1);
            g_mp_bt_on = 1;
        }
        mp_log("REBUILD type 1: COPY | PASTE(%s) | %s | NEW | EDIT (%s %u)\n",
               can ? "on" : "grayed", bt ? "SELECT" : "-", sn.sel_folder ? "folder" : "playlist",
               sn.sel_pl);
        return;
    }
    if (type == MP_TYPE_FOLDER) { /* the FOLDER view: COPY | PASTE, files only */
        struct plsnap sn = plsnap_get();
        if (!sn.fsel_media || !sn.fsel_kind || !mp_prepare_buttons(b1, b2))
            return;
        mp_show(b1, 0, 0, 0);
        mp_show(b2, 0, 0, 0);
        uintptr_t w = 0;
        own_q(c + 0xa0, &w);
        uintptr_t b3 = mp_b3(c, w, b1);
        if (b3) {
            mp_show(b3, MP_B3_SET_COPY, 1, 1);
            g_mp_b3_action = MP_B3_COPY;
        }
        /* PASTE: files copied in the FOLDER view of the OTHER stick */
        int can = g_clip.kind == CLIP_FILES && g_clip.media != sn.fsel_media;
        uintptr_t bm = mp_bm(c, w, b1);
        if (bm) {
            mp_show_paste(bm, can);
            g_mp_bm_on = 1;
        }
        mp_log("REBUILD FOLDER: COPY | PASTE(%s) on \"%s\"\n", can ? "on" : "grayed",
               sn.fsel_path);
        return;
    }
    if (type == 2 || type == MP_TYPE_ADD) { /* a track row */
        /* Row: COPY | PASTE | ADD TO | (PLAYED) | REMOVE | MAKE STEMS, fixed slots.
         * COPY from any browse list (TRACK, ALBUM, KEY, folders, search...); PASTE only where
         * it can land: a playlist's own track list (open_pl). */
        int inpl = plsnap_get().open_pl != 0;
        int rem = inpl && type == 2 && mp_remove_eligible();
        int trk = rem || pl_selected_is_track();
        if (!trk) {
            struct plsnap sn = plsnap_get();
            mp_log("REBUILD type %d: stock kept (no track row: sel=%d snap pl=%u row=%u "
                   "sort_block=%u)\n",
                   type, tracklist_selected_row(), sn.playlist_id, sn.row, sn.sort_block);
            return;
        }
        if (!mp_prepare_buttons(b1, b2))
            return;
        uintptr_t w = 0;
        own_q(c + 0xa0, &w);
        int played = 0; /* the stock PLAYED / UNPLAYED: one toggle, the one that applies */
        if (type == 2 && !inpl) {
            int e4 = mp_item_enabled(c, 4), e5 = mp_item_enabled(c, 5);
            mp_log("REBUILD type 2 (not a playlist): stock items 4=%d 5=%d\n", e4, e5);
            if (e4 && !e5) {
                mp_show(b1, MP_SET_RELABEL + 2, 1, 1);
                mp_show(b2, 0, 0, 0);
                played = 1;
            } else if (e5 && !e4) {
                mp_show(b2, MP_SET_RELABEL + 4, 1, 1);
                mp_show(b1, 0, 0, 0);
                played = 2;
            } else {
                mp_show(b1, MP_SET_RELABEL + 2, e4, 1);
                mp_show(b2, MP_SET_RELABEL + 4, e5, 1);
                played = 3;
            }
        } else if (type == 2) {
            mp_show(b1, 0, 0,
                    0); /* PLAYED / UNPLAYED: not in a playlist's menu */
            if (rem) {
                mp_show(b2, MP_SET_REMOVE, 1, 1);
                g_mp_b2_action = 1;
            } else
                mp_show(b2, 0, 0, 0);
        } else {
            mp_show(b1, 0, 0, 0);
            mp_show(b2, 0, 0, 0);
        }
        uintptr_t b3 = mp_b3(c, w, b1);
        if (b3) {
            mp_show(b3, MP_B3_SET_COPY, 1, 1);
            g_mp_b3_action = MP_B3_COPY;
        }
        int paste = inpl && type == 2 && (g_clip.kind == CLIP_TRACK || g_clip.kind == CLIP_TAGGED);
        uintptr_t bm =
            (inpl && type == 2) ? mp_bm(c, w, b1) : 0; /* PASTE: a playlist's tracks only */
        if (bm) {
            mp_show_paste(bm, paste);
            g_mp_bm_on = 1;
        }
        uintptr_t ba =
            pl_add_target_set() ? mp_ba(c, w, b1) : 0; /* ADD TO <the selected playlist> */
        if (ba) {
            mp_show_add(ba, MP_BA_SET_ADD, MP_BA_SET_NAME);
            g_mp_ba_on = 1;
        }
        g_mp_b1_unplayed = 0;
        mp_log("REBUILD type %d: COPY%s%s%s%s\n", type,
               bm ? (paste ? " | PASTE" : " | PASTE(grayed)") : "", ba ? " | ADD TO" : "",
               played ? " | PLAYED/UNPLAYED" : "", rem ? " | REMOVE" : "");
    }
}

/* Visibility is juce's own flag (Component +0xc0 bit 1, as tracklist_showing reads it). */
static int mp_visible(uintptr_t b) {
    uintptr_t f = 0;
    return b && own_q(b + 0xc0, &f) && (f & 2);
}
/* MAKE STEMS: a TRACK row says (REMOTE); a playlist / folder / the Tag List says how
 * many tracks it queues, in gray (the Tag List's rows, a playlist's
 * count from the stems mark's own lookup (sm_pl_count)); not known yet: (REMOTE). */
static int g_mp_bs_cancel; /* the button says CANCEL STEMS (the row is in the queue) */
static void mp_show_stems(uintptr_t bs, const struct plsnap *sn) {
    /* CANCEL STEMS: a track in the Remote Stems queue, or a playlist
     * holding one. Sets 0 and 1 (its 210 px twin) carry this button's static art, so the
     * label is swapped there; folders and the Tag List keep MAKE STEMS. */
    int cancel = g_mp_bs_pl == 0 ? sm_queued_cid(sn->any_cid)
                 : g_mp_bs_pl == 1 && !sn->sel_folder
                     ? sm_pl_busy(sn->sel_pl, sn->sel_title, sn->any_media) > 0
                     : 0;
    int key = cancel ? MP_IMG_STEMSX : 5;
    if (mp_swap_set(bs, 0, g_mp_img[key]) && mp_swap_set(bs, 1, g_mp_img210[key]))
        g_mp_bs_cancel = cancel;
    else
        g_mp_bs_cancel = 0;
    if (g_mp_bs_cancel) {
        mp_show(bs, 0, 1, 1);
        return;
    }
    int n = g_mp_bs_pl == 2 ? g_mp_tag_rows
            : g_mp_bs_pl == 1 && !sn->sel_folder
                ? sm_pl_count(sn->sel_pl, sn->sel_title, sn->any_media)
                : -1;
    const uintptr_t *d = 0;
    if (n > 0) {
        char t[32];
        snprintf(t, sizeof t, n == 1 ? "1 TRACK" : "%d TRACKS", n);
        d = mp_dyn2_images(&g_mp_stemsdyn, t, MP_IMG_STEMSN);
    }
    if (d && (mp_valid_slot(bs, 2) ? mp_swap_set(bs, 2, d) : mp_ensure_set(bs, 2, d, 1)))
        mp_show(bs, 2, 1, 1);
    else
        mp_show(bs, 0, 1, 1);
}
/* Lays out our buttons beside the stock ones. With Playlist Editing on: six fixed
 * 210 px slots. Otherwise centered, 246 px on stock's 318 px pitch (2 buttons: x
 * 358/676; 3: 199/517/835; 4: 40/358/676/994), tightened so five still fit in 1280. */
static void mp_place_stems(uintptr_t c) {
    if (g_mp_bs && mp_valid(g_mp_bs, MP_TIB_VT0))
        mp_show(g_mp_bs, 0, 0, 0); /* default: hidden */
    if (g_mp_bn && mp_valid(g_mp_bn, MP_TIB_VT0))
        mp_show(g_mp_bn, 0, 0, 0);
    if (g_mp_bf && mp_valid(g_mp_bf, MP_TIB_VT0))
        mp_show(g_mp_bf, 0, 0, 0);
    uintptr_t b1 = 0, b2 = 0, ty = 0, w = 0;
    if (!mp_parts(c, &b1, &b2) || !own_q(c + 0x18, &ty) || !own_q(c + 0xa0, &w))
        return;
    int type = (int) (signed char) (ty & 0xff);
    uintptr_t bs = 0, bn = 0, bf = 0;
    struct plsnap sn = plsnap_get();
    (void) bn;
    (void) bf; /* NEW PLAYLIST / NEW FOLDER: only behind NEW (mp_new_row) */
    if (mp_stems_on()) {
        if (type == 0 || type == 1) {
            if (sn.sel_pl && sn.any_media) {
                g_mp_bs_pl = 1;
                bs = mp_bs(c, w, b1);
            }                   /* a folder: every track inside it */
        } else if (type == 3) { /* TAG LIST: every tagged track */
            g_mp_tag_rows = tracklist_showing() ? drag_num_rows()
                                                : 0; /* counted here: Playlist Editing may be off */
            if (g_mp_tag_rows > 0 && sn.any_media) {
                g_mp_bs_pl = 2;
                bs = mp_bs(c, w, b1);
            }
        } else if (type == 2 || type == MP_TYPE_ADD) {
            if (pl_selected_is_track()) {
                g_mp_bs_pl = 0;
                bs = mp_bs(c, w, b1);
            }
            if (type == MP_TYPE_ADD) {
                mp_show(b1, 0, 0, 0);
                mp_show(b2, 0, 0, 0);
            } /* ours: no stock items */
        }
    }
    int b3v = g_mp_b3 && g_mp_b3_widget == w && mp_valid(g_mp_b3, MP_TIB_VT0) && g_mp_b3_action &&
              mp_visible(g_mp_b3);
    int bmv = g_mp_bm_on && g_mp_bm && g_mp_bm_widget == w && mp_valid(g_mp_bm, MP_TIB_VT0) &&
              mp_visible(g_mp_bm);
    int bav = g_mp_ba_on && g_mp_ba && g_mp_ba_widget == w && mp_valid(g_mp_ba, MP_TIB_VT0) &&
              mp_visible(g_mp_ba);
    int btv = g_mp_bt_on && g_mp_bt && g_mp_bt_widget == w && mp_valid(g_mp_bt, MP_TIB_VT0) &&
              mp_visible(g_mp_bt);
    int bnmv = g_mp_bnm_on && g_mp_bnm && g_mp_bnm_widget == w && mp_valid(g_mp_bnm, MP_TIB_VT0) &&
               mp_visible(g_mp_bnm);
    int brnv = g_mp_brn_on && g_mp_brn && g_mp_brn_widget == w && mp_valid(g_mp_brn, MP_TIB_VT0) &&
               mp_visible(g_mp_brn);
    int bedv = g_mp_bed_on && g_mp_bed && g_mp_bed_widget == w && mp_valid(g_mp_bed, MP_TIB_VT0) &&
               mp_visible(g_mp_bed);
    if (type == MP_TYPE_FOLDER) { /* ours: no stock items */
        mp_show(b1, 0, 0, 0);
        mp_show(b2, 0, 0, 0);
    }
    if (mp_enabled() && (type == 0 || type == 1 || type == 2 || type == 3 || type == MP_TYPE_ADD ||
                         type == MP_TYPE_FOLDER)) {
        /* Fixed slots: every button 210 wide, six slots 212 apart, so a
         * button is always in the same place: COPY, PASTE, SELECT / ADD TO on the left,
         * MAKE STEMS always on the right; a slot with nothing for this row stays empty. */
        if (bs) {
            mp_show_stems(bs, &sn);
            g_mp_bs_on = 1;
        }
        uintptr_t slot[6] = {0, 0, 0, 0, 0, 0};
        if (b3v)
            slot[0] = g_mp_b3;
        if (bmv)
            slot[1] = g_mp_bm;
        if (btv)
            slot[2] = g_mp_bt;
        else if (bav)
            slot[2] = g_mp_ba;
        if (bnmv)
            slot[3] = g_mp_bnm;
        if (bedv)
            slot[4] = g_mp_bed;
        if (type == 3) {
            if (mp_visible(b2))
                slot[3] = b2;
            if (mp_visible(b1))
                slot[4] = b1;
        } /* CREATE PLAYLIST, REMOVE ALL TRACKS */
        else if (type == 2 || type == MP_TYPE_ADD) {
            if (mp_visible(b2) && g_mp_b2_action == 1)
                slot[4] = b2; /* REMOVE FROM PLAYLIST */
            else {
                if (mp_visible(b1))
                    slot[3] = b1;
                if (mp_visible(b2))
                    slot[slot[3] ? 4 : 3] = b2;
            } /* PLAYED / UNPLAYED */
        }
        if (bs)
            slot[5] = bs;
        int n = 0;
        for (int i = 0; i < 6; i++)
            if (slot[i]) {
                mp_bounds_w(slot[i], MP_SLOT_X(i), 210);
                mp_narrow(slot[i], b2);
                n++;
            }
        mp_log("REBUILD type %d: slots %s %s %s %s %s %s\n", type, slot[0] ? "COPY" : "-",
               slot[1] ? "PASTE" : "-", slot[2] ? (slot[2] == g_mp_ba ? "ADDTO" : "SELECT") : "-",
               slot[3] ? (slot[3] == g_mp_bnm ? "NEW" : "stock") : "-",
               slot[4] ? (slot[4] == g_mp_bed ? "EDIT" : "stock") : "-", slot[5] ? "STEMS" : "-");
        return;
    }
    if (!bs && !bn && !bf && !b3v && !bmv && !bav && !btv && !bnmv && !brnv)
        return;
    /* Order: COPY, PASTE, [SELECT], ADD TO, NEW, RENAME, NEW PLAYLIST, NEW FOLDER,
     * MAKE STEMS, then the stock buttons (PLAYED, REMOVE), the destructive one last. */
    if (bs && (b3v + bmv + bav + btv + bnmv + brnv + (bn != 0) + (bf != 0) + mp_visible(b1) +
               mp_visible(b2)) +
                      1 >
                  6) {
        mp_show(bs, 0, 0, 0);
        bs = 0;
        mp_log("REBUILD: MAKE STEMS left out (6 buttons do not fit)\n");
    }
    if (bn) {
        mp_show(bn, 0, 1, 1);
        g_mp_bn_on = 1;
    }
    if (bf) {
        mp_show(bf, 0, 1, 1);
        g_mp_bf_on = 1;
    }
    if (bs) {
        mp_show(bs, 0, 1, 1);
        g_mp_bs_on = 1;
    }
    uintptr_t row[9];
    int n = 0;
    if (b3v)
        row[n++] = g_mp_b3;
    if (bmv)
        row[n++] = g_mp_bm;
    if (btv)
        row[n++] = g_mp_bt;
    if (bav)
        row[n++] = g_mp_ba;
    if (bnmv)
        row[n++] = g_mp_bnm;
    if (brnv)
        row[n++] = g_mp_brn;
    if (bn)
        row[n++] = bn;
    if (bf)
        row[n++] = bf;
    if (bs)
        row[n++] = bs;
    if (mp_visible(b2))
        row[n++] = b2;
    if (mp_visible(b1))
        row[n++] = b1; /* REMOVE ALL TRACKS: the most destructive, last */
    /* One width for the whole row: 210 whenever every button has 210 art (uniform
     * across menus); else 246 up to five, which then fit. */
    int all210 = 1;
    for (int i = 0; i < n; i++)
        all210 &= mp_has_twin(row[i], b2);
    int bw = (all210 || n > 5) ? 210 : 246, pitch = bw == 210 ? 212 : 318;
    if (bw == 246) {
        int span = 1280 - 2 * 16 - 246;
        if (n > 1 && (n - 1) * pitch > span)
            pitch = span / (n - 1);
    }
    int x0 = (1280 - ((n - 1) * pitch + bw)) / 2;
    for (int i = 0; i < n; i++)
        mp_bounds_w(row[i], x0 + i * pitch, bw);
    if (bw < 246)
        for (int i = 0; i < n; i++)
            mp_narrow(row[i], b2); /* after every mp_show: it resets the set */
    mp_log(
        "REBUILD type %d: %d buttons (new playlist %d, new folder %d, make stems %d), pitch %d\n",
        type, n, bn != 0, bf != 0, bs != 0, pitch);
}

/* The model is built on the MENU press. On a screen with no stock menu (type
 * -1), a track row and a target set: open the panel as our ADD menu. */
static void mp_producer_hook(void *ctx) {
    uintptr_t lr = (uintptr_t) __builtin_return_address(0);
    g_mp_orig_producer(ctx);
    if (!mp_enabled() && !mp_stems_on())
        return;
    /* Only on a MENU press. The producer also runs on navigation, when a stock
     * menu is empty; synthesizing there would open the panel by itself. */
    if (lr != MP_PRODUCER_MENU_LR)
        return;
    uintptr_t dd = 0, m = 0, ty = 0, gen = 0;
    if (!own_q((uintptr_t) ctx + 0x10, &dd) || !dd || !own_q(dd + 0xa0, &m) ||
        !mp_valid(m, MP_MODEL_VT))
        return;
    if (!own_q(m + 0x18, &ty) || !own_q(dd + 0xb0, &gen))
        return;
    int type = (int) (signed char) (ty & 0xff);
    /* MENU never changes the ADD TO target: retargeting to the open playlist would
     * steal the one the DJ chose with [SELECT] and make copying A -> B impossible. */
    if (type != -1)
        return;
    if (mp_enabled()) { /* the FOLDER view: stock has no menu there; ours copies files */
        struct plsnap fs = plsnap_get();
        if (fs.fsel_media && fs.fsel_kind) {
            *(uint8_t *) (m + 0x08) = 1;
            *(uintptr_t *) (m + 0x10) = gen;
            *(int8_t *) (m + 0x18) = MP_TYPE_FOLDER;
            *(uintptr_t *) (m + 0x20) = gen;
            mp_log("PRODUCER: synthesised the FOLDER menu (\"%s\", gen %lx)\n", fs.fsel_path,
                   (unsigned long) gen);
            return;
        }
    }
    if (!mp_stems_on())
        return;                    /* no stock menu = not a playlist: only MAKE STEMS may add one */
    if (!pl_selected_is_track()) { /* with or without a target: see mp_add */
        mp_log("PRODUCER (MENU press, no stock menu): no ADD menu (no track row selected)\n");
        return;
    }
    *(uint8_t *) (m + 0x08) = 1;     /* visible */
    *(uintptr_t *) (m + 0x10) = gen; /* the revisions the producer itself stamps */
    *(int8_t *) (m + 0x18) = MP_TYPE_ADD;
    *(uintptr_t *) (m + 0x20) = gen;
    mp_log("PRODUCER: synthesised the ADD menu (gen %lx)\n", (unsigned long) gen);
}

/* Close the panel the way stock does. Stock's OK-confirmed actions (ALL DELETE,
 * DELETE, CREATE PLAYLIST, REMOVE ALL TRACKS: callbacks 0x1f14358 / 0x1eee670 /
 * 0x1f17630) end in 0x1ec3248, the function the panel's X button reaches through
 * [ctrl+0x80] vt+0x18 = 0x1f05520; PLAYED/UNPLAYED (0x1f15c28) and CANCEL leave
 * it open. So every action of ours that completes (and posts a box) closes it, through the
 * X button's own call. */
#define MP_CLOSER_VT EP_2dc1820
#define MP_CLOSER_FN EP_1f05520
static void mp_close(uintptr_t ctrl) {
    uintptr_t ob = 0, vt = 0, fn = 0;
    if (!mp_valid(ctrl, MP_CTRL_VT) || !own_q(ctrl + 0x80, &ob) || !mp_valid(ob, MP_CLOSER_VT) ||
        !img_q(MP_CLOSER_VT + 0x18, &fn) || fn != MP_CLOSER_FN) {
        (void) vt;
        mp_log("CLOSE refused (closer %lx not as expected)\n", (unsigned long) ob);
        return;
    }
    ((void (*)(uintptr_t)) fn)(ob);
}
/* The ADD button. No target yet: tell the DJ how to set one, in EP147's top strip. */
#define MP_HINT_L2 "MENU on a playlist > [SELECT] first"
static void mp_add(uintptr_t ctrl, const char *who) {
    if (!pl_add_target_set()) {
        int b = az_banner_strip(
            MP_HINT_L2,
            PL_MSG_READ); /* one line in the top strip, like every playlist-edit message */
        mp_log("CLICK %s, no target -> how-to strip (banner=%d)\n", who, b);
        mp_close(ctrl);
        return;
    }
    int rc = pl_request_add_selected();
    mp_log("CLICK %s -> pl_request_add_selected rc=%d\n", who, rc);
    mp_close(ctrl);
}
/* COPY (move=0) / MOVE (move=1): the highlighted row onto the clipboard (g_clip,
 * azmod_internal.h). A track row: its content_id; a MOVE also records where it comes from
 * (its playlist and position, which REMOVE re-proves), so it needs a playlist
 * sorted by "#". A playlist/folder row: its id (folders are moved, not copied). */
static int mp_clip_set(int move) {
    struct plsnap sn = plsnap_get();
    if (sn.fsel_media && sn.fsel_kind && !move) { /* the FOLDER view: the entry's path */
        memset(&g_clip, 0, sizeof g_clip);
        g_clip.kind = CLIP_FILES;
        g_clip.media = sn.fsel_media;
        snprintf(g_clip.path, sizeof g_clip.path, "%s", sn.fsel_path);
        const char *base = strrchr(sn.fsel_path, '/');
        utf8_copy(g_clip.name, sizeof g_clip.name, base ? base + 1 : sn.fsel_path);
        char b[160];
        snprintf(b, sizeof b, "Copied %s", pl_name_short(g_clip.name));
        az_banner_strip(b, 2000);
        drag_log("CLIP COPY files \"%s\" (media %04x)\n", g_clip.path, g_clip.media);
        return 0;
    }
    if (pl_selected_is_track()) {
        if (move && !mp_remove_eligible()) {
            az_banner_strip("Sort by # to cut tracks", PL_MSG_READ);
            return -64;
        }
        memset(&g_clip, 0, sizeof g_clip);
        g_clip.kind = CLIP_TRACK;
        g_clip.move = move;
        g_clip.media = sn.any_media;
        g_clip.id = sn.any_cid;
        if (move) {
            g_clip.src_pl = sn.playlist_id;
            g_clip.src_seq = sn.row + 1;
        }
    } else if (sn.sel_pl && sn.any_media) {
        memset(&g_clip, 0, sizeof g_clip);
        g_clip.kind = sn.sel_folder ? CLIP_FOLDER : CLIP_LIST;
        g_clip.move = move;
        g_clip.media = sn.any_media;
        g_clip.id = sn.sel_pl;
    } else
        return -3;
    utf8_copy(g_clip.name, sizeof g_clip.name, sn.sel_title);
    {
        char b[160];
        snprintf(b, sizeof b, "%s %s", move ? "Cut" : "Copied", pl_name_short(g_clip.name));
        az_banner_strip(b, 2000);
    }
    drag_log("CLIP %s %s %u \"%s\" (media %04x, from pl %u #%u)\n", move ? "MOVE" : "COPY",
             g_clip.kind == CLIP_TRACK  ? "track"
             : g_clip.kind == CLIP_LIST ? "playlist"
                                        : "folder",
             g_clip.id, g_clip.name, g_clip.media, g_clip.src_pl, g_clip.src_seq);
    cg_capture(); /* a CUT playlist row: its ghost's fingerprint */
    return 0;
}
/* Slide: a submenu row rises in from the bottom while the row it
 * replaces drops out (BACK reverses it), MP_ANIM_MS with an ease-out: EP147's own buttons,
 * moved frame by frame on the message thread (msg_tick runs in JUCE's timer
 * dispatch). Bounds only; the outgoing buttons are hidden when it ends. Every
 * frame re-proves the panel and each button (a menu closed mid-slide stops it). */
#define MP_ANIM_MS 180
#define MP_ANIM_N 12
static struct {
    int on, frames;
    long long t0;
    uintptr_t ctrl, b1, b2;
    int n;
    uintptr_t b[MP_ANIM_N];
    int x[MP_ANIM_N], from[MP_ANIM_N], to[MP_ANIM_N];
    char hide[MP_ANIM_N];
} g_mp_anim;
#define MP_ANIM_DROP 100 /* px below the row: past the panel's bottom, so clipped away */
static void mp_bounds_xy(uintptr_t c, int x, int y, int w) {
    ((void (*)(uintptr_t, int, int, int, int)) MP_SET_BOUNDS)(c, x, y, w, 80);
}
static int mp_x_of(uintptr_t b) {
    uintptr_t xy = 0;
    return own_q(b + 0x20, &xy) ? (int) (uint32_t) xy : 0;
}
static void mp_set_visible(uintptr_t b, int v) {
    uintptr_t vt = 0, fn = 0;
    if (own_q(b, &vt) && own_q(vt + 0x60, &fn) && fn)
        ((void (*)(uintptr_t, int)) fn)(b, v);
}
/* One of our own buttons (never freed by EP147, see mp_valid). */
static int mp_is_ours(uintptr_t b) {
    return b && (b == g_mp_b3 || b == g_mp_bm || b == g_mp_ba || b == g_mp_bt || b == g_mp_bn ||
                 b == g_mp_bf || b == g_mp_bs || b == g_mp_bnm || b == g_mp_brn || b == g_mp_bed ||
                 b == g_mp_bu);
}
/* An animated button: B1/B2 as read from the live widget in this call (g_mp_anim.b1/b2, set by
 * mp_anim_begin from a hook, or refreshed by mp_anim_tick / mp_sub_back), or one of ours. A
 * cached EP147 button that is neither is never touched. */
static int mp_anim_ok(uintptr_t b) {
    if (!b)
        return 0;
    if (b == g_mp_anim.b1 || b == g_mp_anim.b2)
        return mp_valid(b, MP_BTN_VT);
    return mp_is_ours(b) && mp_valid(b, MP_TIB_VT0);
}
static void mp_anim_begin(uintptr_t c, uintptr_t b1, uintptr_t b2) {
    memset(&g_mp_anim, 0, sizeof g_mp_anim);
    g_mp_anim.ctrl = c;
    g_mp_anim.b1 = b1;
    g_mp_anim.b2 = b2;
}
/* one button moving vertically at column x: from y `from` to y `to` (the row's y is 36) */
static void mp_anim_add(uintptr_t b, int x, int from, int to, int hide) {
    if (!b || g_mp_anim.n >= MP_ANIM_N)
        return;
    int i = g_mp_anim.n++;
    g_mp_anim.b[i] = b;
    g_mp_anim.x[i] = x;
    g_mp_anim.from[i] = from;
    g_mp_anim.to[i] = to;
    g_mp_anim.hide[i] = (char) hide;
    mp_bounds_xy(b, x, from, 210);
}
static void mp_anim_go(void) {
    g_mp_anim.t0 = drag_now_ms();
    g_mp_anim.on = 1;
}
static void mp_rebuild_hook(void *ctrl);
static void mp_anim_in(uintptr_t b);
/* BACK on a submenu row (bus thread swallowed it): rebuild the parent row and slide it
 * back up while the submenu drops away. Message thread. */
static void mp_sub_back(void) {
    uintptr_t c = g_mp_anim.ctrl, b1 = 0, b2 = 0;
    if (!mp_ctrl_live(c) || !mp_parts(c, &b1, &b2))
        return;
    g_mp_anim.b1 = b1; /* this call's B1/B2, read from the live widget */
    g_mp_anim.b2 = b2;
    uintptr_t sub[] = {g_mp_bn, g_mp_bf, g_mp_brn, b2};
    int vis[4], xs[4];
    for (int i = 0; i < 4; i++) {
        vis[i] = sub[i] &&
                 (sub[i] == b2 ? mp_valid(b2, MP_BTN_VT) : mp_valid(sub[i], MP_TIB_VT0)) &&
                 mp_visible(sub[i]);
        xs[i] = vis[i] ? mp_x_of(sub[i]) : 0;
    }
    mp_rebuild_hook((void *) c); /* the parent row, laid out as on MENU */
    mp_anim_begin(c, b1, b2);
    for (int i = 0; i < 4; i++)
        if (vis[i] && !(sub[i] == b2 && mp_visible(b2))) {
            mp_set_visible(sub[i], 1);
            mp_anim_add(sub[i], xs[i], 36, 36 + MP_ANIM_DROP, 1);
        }
    uintptr_t row[] = {g_mp_b3, g_mp_bm, g_mp_bt, g_mp_ba, g_mp_bnm, g_mp_bed, g_mp_bs, b1, b2};
    for (unsigned i = 0; i < sizeof row / sizeof *row; i++)
        mp_anim_in(row[i]);
    mp_anim_go();
    mp_log("BACK -> the parent row\n");
}
static void mp_anim_tick(void) {
    if (__atomic_load_n(&g_mp_submode,
                        __ATOMIC_ACQUIRE)) { /* BACK stays captured only while the panel shows */
        uintptr_t w = 0, c = g_mp_anim.ctrl;
        if (!mp_ctrl_live(c) || !own_q(c + 0xa0, &w) || !w || !mp_visible(w)) {
            __atomic_store_n(&g_mp_submode, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&g_mp_subback, 0, __ATOMIC_RELEASE);
        } else if (__atomic_exchange_n(&g_mp_subback, 0, __ATOMIC_ACQ_REL)) {
            __atomic_store_n(&g_mp_submode, 0, __ATOMIC_RELEASE);
            mp_sub_back();
        }
    }
    if (!g_mp_anim.on)
        return;
    uintptr_t ab1 = 0, ab2 = 0;
    if (!mp_ctrl_live(g_mp_anim.ctrl) || !mp_parts(g_mp_anim.ctrl, &ab1, &ab2)) {
        g_mp_anim.on = 0;
        return;
    }
    g_mp_anim.b1 = ab1; /* B1/B2 as the live widget holds them now */
    g_mp_anim.b2 = ab2;
    long long e = drag_now_ms() - g_mp_anim.t0;
    int done = e >= MP_ANIM_MS;
    double p = done ? 1.0 : (double) e / MP_ANIM_MS,
           q = 1.0 - (1.0 - p) * (1.0 - p) * (1.0 - p); /* ease-out cubic */
    for (int i = 0; i < g_mp_anim.n; i++) {
        uintptr_t b = g_mp_anim.b[i];
        if (!mp_anim_ok(b))
            continue;
        mp_bounds_xy(b, g_mp_anim.x[i],
                     g_mp_anim.from[i] +
                         (int) ((g_mp_anim.to[i] - g_mp_anim.from[i]) * q + (q < 1 ? 0.5 : 0)),
                     210);
        if (done && g_mp_anim.hide[i]) {
            mp_set_visible(b, 0);
            mp_bounds_xy(b, g_mp_anim.x[i], 36, 210);
        }
    }
    g_mp_anim.frames++;
    if (done) {
        g_mp_anim.on = 0;
        mp_log("SLIDE %d buttons: %d frames in %lld ms\n", g_mp_anim.n, g_mp_anim.frames, e);
    }
}
/* the row now showing drops out (hidden at the end); call before the new row is shown */
static void mp_anim_out(uintptr_t *bs, int n) {
    for (int i = 0; i < n; i++)
        if (mp_anim_ok(bs[i]) && mp_visible(bs[i]))
            mp_anim_add(bs[i], mp_x_of(bs[i]), 36, 36 + MP_ANIM_DROP, 1);
}
/* the row just shown rises in from below */
static void mp_anim_in(uintptr_t b) {
    if (mp_anim_ok(b) && mp_visible(b))
        mp_anim_add(b, mp_x_of(b), 36 + MP_ANIM_DROP, 36, 0);
}
/* A stock DELETE armed on the playlist list, until its dialog answers. */
static struct {
    unsigned media, pl, folder;
    int armed;
} g_mp_del;
/* NEW tapped: the same panel now offers NEW PLAYLIST / NEW FOLDER (BACK returns to
 * the parent row). */
static void mp_new_row(uintptr_t c) {
    uintptr_t b1 = 0, b2 = 0, w = 0;
    if (!mp_parts(c, &b1, &b2) || !own_q(c + 0xa0, &w))
        return;
    uintptr_t hide[] = {b1, b2, g_mp_b3, g_mp_bm, g_mp_bt,
                        g_mp_ba, g_mp_bnm, g_mp_brn, g_mp_bs, g_mp_bed};
    mp_anim_begin(c, b1, b2);
    mp_anim_out(hide, sizeof hide / sizeof *hide); /* slides out; hidden when the slide ends */
    g_mp_b3_action = 0;
    g_mp_bm_on = g_mp_bt_on = g_mp_ba_on = g_mp_bnm_on = g_mp_brn_on = g_mp_bs_on = g_mp_bed_on = 0;
    uintptr_t bn = mp_bn(c, w, b1), bf = mp_bf(c, w, b1);
    if (!bn || !bf) {
        mp_close(c);
        return;
    }
    mp_show(bn, 0, 1, 1);
    mp_show(bf, 0, 1, 1);
    g_mp_bn_on = g_mp_bf_on = 1;
    g_mp_newmode = 1;
    mp_narrow(bn, b2);
    mp_narrow(bf, b2);
    mp_bounds_w(bn, MP_SLOT_X(2), 210);
    mp_bounds_w(bf, MP_SLOT_X(3), 210);
    mp_anim_in(bn);
    mp_anim_in(bf);
    mp_anim_go();
    __atomic_store_n(&g_mp_submode, 1, __ATOMIC_RELEASE);
    mp_log("CLICK NEW -> NEW PLAYLIST | NEW FOLDER\n");
}
/* EDIT tapped: the same panel now offers RENAME / REMOVE (BACK returns to the parent
 * row). REMOVE is the stock DELETE button relabeled; its click runs our journaled
 * remove (g_mp_b2_action 3 folder / 4 playlist). */
static void mp_edit_row(uintptr_t c) {
    uintptr_t b1 = 0, b2 = 0, w = 0;
    if (!mp_parts(c, &b1, &b2) || !own_q(c + 0xa0, &w))
        return;
    uintptr_t hide[] = {b1, g_mp_b3, g_mp_bm, g_mp_bt, g_mp_ba, g_mp_bnm, g_mp_bs, g_mp_bed};
    mp_anim_begin(c, b1, b2);
    mp_anim_out(hide, sizeof hide / sizeof *hide); /* slides out; hidden when the slide ends */
    g_mp_b3_action = 0;
    g_mp_bm_on = g_mp_bt_on = g_mp_ba_on = g_mp_bnm_on = g_mp_bs_on = g_mp_bed_on = 0;
    uintptr_t brn = mp_brn(c, w, b1);
    if (!brn) {
        mp_close(c);
        return;
    }
    mp_show(brn, 0, 1, 1);
    g_mp_brn_on = 1;
    mp_show(b2, g_mp_b2_action == 3 ? MP_SET_RELABEL + 5 : MP_SET_RELABEL, 1, 1);
    g_mp_editmode = 1; /* a folder: REMOVE (FOLDER) */
    mp_narrow(brn, b2);
    mp_narrow(b2, b2);
    mp_bounds_w(brn, MP_SLOT_X(2), 210);
    mp_bounds_w(b2, MP_SLOT_X(3), 210);
    mp_anim_in(brn);
    mp_anim_in(b2);
    mp_anim_go();
    __atomic_store_n(&g_mp_submode, 1, __ATOMIC_RELEASE);
    mp_log("CLICK EDIT -> RENAME | REMOVE\n");
}
static void mp_click_hook(void *ctrl, void *arg) {
    uintptr_t c = (uintptr_t) ctrl, b1 = 0, b2 = 0, ty = 0, clicked = 0, k1 = 0, k2 = 0, ks = 0;
    mp_note_ctrl(c);
    if (g_mp_bs_on && mp_stems_on() && mp_valid(g_mp_bs, MP_TIB_VT0) &&
        own_q((uintptr_t) arg, &clicked) && own_q(g_mp_bs + 0x1d8, &ks) &&
        clicked == ks) { /* MAKE STEMS */
        struct plsnap sn = plsnap_get();
        int rc =
            g_mp_bs_cancel
                ? (g_mp_bs_pl
                       ? (sn.sel_pl ? rs_cancel_request(sn.any_media, 0, sn.sel_pl) : -3)
                       : (pl_selected_is_track() ? rs_cancel_request(sn.any_media, sn.any_cid, 0)
                                                 : -3))
            : g_mp_bs_pl == 2 ? tag_request_stems(g_mp_tag_rows)
            : g_mp_bs_pl      ? (sn.sel_pl ? rs_request_playlist(sn.any_media, sn.sel_pl) : -3)
                              : (pl_selected_is_track() ? rs_request(sn.any_media, sn.any_cid) : -3);
        mp_log("CLICK %s STEMS %s=%u media %04x -> rc=%d\n", g_mp_bs_cancel ? "CANCEL" : "MAKE",
               g_mp_bs_pl ? "playlist" : "cid", g_mp_bs_pl ? sn.sel_pl : sn.any_cid, sn.any_media,
               rc);
        if (rc < 0)
            pl_edit_notify(MV_OP_RSREAD, rc);
        mp_close(c);
        return;
    }
    {
        uintptr_t kx = 0, ck = 0;
        if (g_mp_bnm_on && mp_enabled() && mp_valid(g_mp_bnm, MP_TIB_VT0) &&
            own_q((uintptr_t) arg, &ck) && own_q(g_mp_bnm + 0x1d8, &kx) &&
            ck == kx) { /* NEW: this row becomes NEW PLAYLIST / NEW FOLDER */
            mp_new_row(c);
            return;
        }
        if (g_mp_bed_on && mp_enabled() && mp_valid(g_mp_bed, MP_TIB_VT0) &&
            own_q((uintptr_t) arg, &ck) && own_q(g_mp_bed + 0x1d8, &kx) &&
            ck == kx) { /* EDIT: this row becomes RENAME / REMOVE */
            mp_edit_row(c);
            return;
        }
        if (g_mp_brn_on && mp_enabled() && mp_valid(g_mp_brn, MP_TIB_VT0) &&
            own_q((uintptr_t) arg, &ck) && own_q(g_mp_brn + 0x1d8, &kx) &&
            ck == kx) { /* RENAME */
            struct plsnap sn = plsnap_get();
            int rc = nl_rename_begin(sn.any_media, sn.sel_pl, sn.sel_title, sn.sel_folder);
            mp_log("CLICK RENAME %u \"%s\" -> rc=%d\n", sn.sel_pl, sn.sel_title, rc);
            if (rc < 0)
                az_banner_strip(rc == -5 ? "The keyboard could not be opened here"
                                         : "Could not start renaming. Try again",
                                PL_MSG_READ);
            mp_close(c);
            return;
        }
    }
    {
        uintptr_t kn = 0, ck = 0;
        if (g_mp_bn_on && mp_enabled() && mp_valid(g_mp_bn, MP_TIB_VT0) &&
            own_q((uintptr_t) arg, &ck) && own_q(g_mp_bn + 0x1d8, &kn) &&
            ck == kn) { /* NEW PLAYLIST */
            struct plsnap sn = plsnap_get();
            int rc = nl_begin(sn.any_media, sn.sel_pl, 0);
            mp_log("CLICK NEW PLAYLIST beside %u media %04x -> rc=%d\n", sn.sel_pl, sn.any_media,
                   rc);
            if (rc < 0)
                az_banner_strip(rc == -5 ? "The keyboard could not be opened here"
                                         : "Could not start naming. Try again",
                                PL_MSG_READ);
            mp_close(c);
            return;
        }
        if (g_mp_bf_on && mp_enabled() && mp_valid(g_mp_bf, MP_TIB_VT0) &&
            own_q((uintptr_t) arg, &ck) && own_q(g_mp_bf + 0x1d8, &kn) &&
            ck == kn) { /* NEW FOLDER */
            struct plsnap sn = plsnap_get();
            int rc = nl_begin(sn.any_media, sn.sel_pl, 1);
            mp_log("CLICK NEW FOLDER beside %u media %04x -> rc=%d\n", sn.sel_pl, sn.any_media, rc);
            if (rc < 0)
                az_banner_strip(rc == -5 ? "The keyboard could not be opened here"
                                         : "Could not start naming. Try again",
                                PL_MSG_READ);
            mp_close(c);
            return;
        }
    }
    clicked = 0;
    if (!mp_enabled()) { /* stems only: never hand our menu type to stock */
        if (own_q(c + 0x18, &ty) && (int) (signed char) (ty & 0xff) == MP_TYPE_ADD)
            return;
        g_mp_orig_click(ctrl, arg);
        return;
    }
    if (!mp_enabled() || !mp_parts(c, &b1, &b2) || !own_q(c + 0x18, &ty) ||
        !own_q((uintptr_t) arg, &clicked) || !own_q(b1 + 0x1d8, &k1) ||
        !own_q(b2 + 0x1d8, &k2)) {
        g_mp_orig_click(ctrl, arg);
        return;
    }
    int type = (int) (signed char) (ty & 0xff);
    if (type == 2 && clicked == k1 && g_mp_b1_unplayed) { /* B1 shows UNPLAYED: stock's B2 action */
        uintptr_t as_b2 = k2;
        mp_log("CLICK B1(UNPLAYED) -> stock B2\n");
        g_mp_orig_click(ctrl, &as_b2);
        return;
    }
    if (type == 2 && clicked == k2 && g_mp_b2_action == 1) {
        *(int32_t *) (c + 0x38) = MP_KIND_REMOVE;
        mp_log("CLICK REMOVE -> confirm\n");
        ((void (*)(uintptr_t, uintptr_t *, uintptr_t *)) MP_OPEN_DIALOG)(c, &g_mp_title_s,
                                                                         &g_mp_body_s);
        return;
    }
    if (type == 0 || type == 1) { /* playlist list (1) / history (0) */
        if (clicked == k1 && mp_b1_refused(type)) { /* HISTORY's ALL DELETE stays stock */
            mp_log("CLICK REMOVE ALL refused (hidden: it wipes every playlist on the USB)\n");
            return;
        }
        if (clicked == k2 && type == 1 &&
            (g_mp_b2_action == 3 ||
             g_mp_b2_action == 4)) { /* REMOVE folder / playlist: count, then our box */
            struct plsnap sn = plsnap_get();
            int rc = fd_request_count(sn.any_media, sn.sel_pl, sn.sel_title, c);
            mp_log("CLICK REMOVE %s %u \"%s\" -> count rc=%d\n",
                   g_mp_b2_action == 3 ? "folder" : "playlist", sn.sel_pl, sn.sel_title, rc);
            if (rc < 0)
                pl_edit_notify(MV_OP_FDEL, rc);
            return; /* the panel stays open for the box */
        }
        if (clicked == k2 && type == 1) { /* stock DELETE -> its dialog; see mp_result_hook */
            struct plsnap sn = plsnap_get();
            g_mp_del.media = sn.any_media;
            g_mp_del.pl = sn.sel_pl;
            g_mp_del.folder = sn.sel_folder;
            g_mp_del.armed = 1;
        }
    }
    uintptr_t k3 = 0, km = 0, ka = 0, kt = 0, ku = 0;
    if (g_mp_bu_on && g_mp_bu && mp_valid(g_mp_bu, MP_TIB_VT0) && own_q(g_mp_bu + 0x1d8, &ku) &&
        clicked == ku) {
        int rc = pl_request_undo();
        mp_log("CLICK UNDO -> rc=%d\n", rc);
        if (rc < 0)
            pl_edit_notify(MV_OP_UNDO, rc);
        mp_close(c);
        return;
    }
    if (g_mp_bt_on && g_mp_bt && mp_valid(g_mp_bt, MP_TIB_VT0) && own_q(g_mp_bt + 0x1d8, &kt) &&
        clicked == kt) {
        if (g_mp_bt_sel) { /* [SELECTED]: tap again to clear */
            struct plsnap sn = plsnap_get();
            pl_target_clear(sn.any_media);
            mp_log("CLICK [SELECTED] -> cleared\n");
            az_banner_strip("Selection cleared", PL_MSG_BRIEF);
            mp_close(c);
            return;
        }
        int rc = pl_request_set_target(); /* [SELECT]: the ADD TO target */
        mp_log("CLICK [SELECT] -> rc=%d\n", rc);
        if (rc < 0)
            pl_edit_notify(MV_OP_TARGET, rc);
        mp_close(c);
        return;
    }
    if (g_mp_bm_on && g_mp_bm && mp_valid(g_mp_bm, MP_TIB_VT0) && own_q(g_mp_bm + 0x1d8, &km) &&
        clicked == km) {
        int rc = pl_request_paste(); /* PASTE (grayed, not clickable, when nothing can land) */
        mp_log("CLICK PASTE -> rc=%d\n", rc);
        if (rc < 0)
            pl_edit_notify(MV_OP_PASTE, rc);
        mp_close(c);
        return;
    }
    if (g_mp_ba_on && g_mp_ba && mp_valid(g_mp_ba, MP_TIB_VT0) && own_q(g_mp_ba + 0x1d8, &ka) &&
        clicked == ka) {
        mp_add(c, "ADD TO");
        return; /* the last paste spot */
    }
    if (g_mp_b3_action && g_mp_b3 && mp_valid(g_mp_b3, MP_TIB_VT0) &&
        own_q(g_mp_b3 + 0x1d8, &k3) && clicked == k3) {
        if (g_mp_b3_action == MP_B3_TAGCOPY) {
            int rc = tag_request_copy(g_mp_tag_rows);
            mp_log("CLICK COPY TAGGED (%d rows) -> rc=%d\n", g_mp_tag_rows, rc);
            if (rc < 0)
                pl_edit_notify(MV_OP_TAGREAD, rc);
        } else if (g_mp_b3_action == MP_B3_PASTE) {
            int rc = pl_request_paste();
            mp_log("CLICK PASTE -> rc=%d\n", rc);
            if (rc < 0)
                pl_edit_notify(MV_OP_PASTE, rc);
        } else {
            int rc = mp_clip_set(0);
            mp_log("CLICK COPY -> clip rc=%d\n", rc);
        }
        mp_close(c);
        return;
    }
    if (type == MP_TYPE_ADD)
        return; /* never hand our type to stock */
    g_mp_orig_click(ctrl, arg);
}
static struct bn_holder g_mp_fd_title, g_mp_fd_body;
static uintptr_t g_mp_fd_title_s, g_mp_fd_body_s;
/* the folder's contents are counted: EP147's own OK / CANCEL box, on the same panel */
static void mp_fd_confirm(uintptr_t c, const char *name, int nlists, int nfolders) {
    if (!mp_ctrl_live(c) || !mp_enabled()) { /* c was cached when the folder was picked */
        mp_log("FOLDER confirm: menu gone\n");
        return;
    }
    /* b fits the dialog's body (bn_holder.text); the name is bounded so a long one is
     * shortened and the counts after it always show */
    char b[160];
    int pl = nlists < 0; /* nlists -1: the root is a playlist, not a folder */
    if (pl)
        snprintf(b, sizeof b, "\"%.96s\"", name);
    else if (nfolders)
        snprintf(b, sizeof b, "\"%.96s\": %d playlist%s, %d folder%s inside", name, nlists,
                 nlists == 1 ? "" : "s", nfolders, nfolders == 1 ? "" : "s");
    else
        snprintf(b, sizeof b, "\"%.96s\": %d playlist%s inside", name, nlists,
                 nlists == 1 ? "" : "s");
    if (pl)
        bn_set(&g_mp_fd_title, "REMOVE PLAYLIST", 15);
    else
        bn_set(&g_mp_fd_title, "REMOVE FOLDER", 13);
    g_mp_fd_title_s = (uintptr_t) g_mp_fd_title.text;
    bn_set(&g_mp_fd_body, b, strlen(b));
    g_mp_fd_body_s = (uintptr_t) g_mp_fd_body.text;
    *(int32_t *) (c + 0x38) = MP_KIND_FOLDERDEL;
    mp_log("FOLDER confirm: %s\n", b);
    ((void (*)(uintptr_t, uintptr_t *, uintptr_t *)) MP_OPEN_DIALOG)(c, &g_mp_fd_title_s,
                                                                     &g_mp_fd_body_s);
}
static void mp_result_hook(void *iface, int canceled) {
    uintptr_t c = (uintptr_t) iface - 0x10, kind = 0;
    mp_note_ctrl(c);
    if (mp_valid(c, MP_CTRL_VT) && own_q(c + 0x38, &kind) && (int32_t) kind == MP_KIND_FOLDERDEL) {
        *(int32_t *) (c + 0x38) = -1;
        if (!canceled && mp_enabled()) {
            int rc = fd_request_delete();
            mp_log("FOLDER CONFIRM OK -> delete rc=%d\n", rc);
            if (rc < 0)
                pl_edit_notify(MV_OP_FDEL, rc);
            mp_close(c);
        }
        g_mp_orig_result(iface, 1); /* as CANCEL: stock closes the dialog, runs nothing */
        return;
    }
    if (mp_valid(c, MP_CTRL_VT) && own_q(c + 0x38, &kind) && (int32_t) kind == MP_KIND_REMOVE) {
        *(int32_t *) (c + 0x38) = -1;
        if (!canceled && mp_enabled()) {
            int rc = pl_request_remove();
            mp_log("CONFIRM OK -> pl_request_remove rc=%d\n", rc);
            if (rc < 0)
                pl_edit_notify(MV_OP_REMOVE, rc);
            mp_close(c); /* as stock's OK: close, then notify */
        }
        g_mp_orig_result(iface, 1); /* as CANCEL: stock closes the dialog, runs nothing */
        return;
    }
    if (g_mp_del.armed) { /* stock DELETE on the playlist list */
        g_mp_del.armed = 0;
        if (!canceled)
            pl_target_forget_deleted(g_mp_del.media, g_mp_del.pl, g_mp_del.folder);
    }
    g_mp_orig_result(iface, canceled);
}

#include "menu_plan_set_art.c"
#include "menu_plan_set.c"

#include "menu_popup_install.c"
