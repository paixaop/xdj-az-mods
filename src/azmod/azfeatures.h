/* SPDX-License-Identifier: GPL-3.0-only */
/* azfeatures.h: the Mod Menu's rows as one table (features.def), and their values.
 *
 * Every row has an item id (an int):
 *   [0, AZF_COUNT)                 the official features of features.def (AZF_*);
 *   [AZI_REG0, AZI_REG0+AZ_REG_MAX) community mods' rows, appended by azf_register at load;
 *   [AZI_ACT0, AZI_COUNT)          the utilities (AZI_* = AZI_ACT0 + AZA_*).
 * Item ids live only in memory (menu selection, last action). Nothing persists or transmits
 * them: azmod.conf uses the row's key, azmod.cfg (dual write) the row's legacy slot.
 *
 * Values: one int per feature and registered row (az_val), read with one relaxed atomic load
 * from any thread (the bus and audio threads included): no lock, no syscall. TOGGLE = 0/1,
 * CHOICE = the index. Written only by the overlay (UI) thread: the menu, the settings load.
 *
 * Included by menu.h after azmod-mod.h (AZ_ROW_*, az_screen_v1) and azmod_mode. */
#ifndef AZMOD_AZFEATURES_H
#define AZMOD_AZFEATURES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* azmod's own translation units only: none of this lands in EP147's symbol namespace */
#pragma GCC visibility push(hidden)

/* The core-side row descriptor: official rows (features.def) and registered rows alike go
 * through it, so both take one code path. Field for field the SDK's az_row_v1 (azmod-mod.h,
 * azf_from_row_v1 copies one into the other); future fields are appended (size-versioned). */
typedef struct az_feature_v2 {
    int size;                    /* sizeof; future fields append */
    const char *key;             /* [a-z0-9-] 1..24, unique in the mod; saved as "<id>:<key>" (built-ins: bare key) */
    const char *label;           /* <= 31 bytes */
    int kind;                    /* AZ_ROW_TOGGLE | AZ_ROW_GROUP | AZ_ROW_ACTION | AZ_ROW_SCREEN | AZ_ROW_CHOICE */
    unsigned flags;              /* AZ_ROW_DEFAULT_ON (built-ins may also use the AZ_ROW_ flags below) */
    const char *parent;          /* NULL = main list, or one of this mod's own earlier GROUP/TOGGLE keys */
    const char *help;            /* right-pane description, may be NULL */
    const unsigned char *icon;   /* optional, NULL = generic */
    const char *const *choices;
    int n_choices;
    int default_choice;   /* CHOICE only */
    const struct az_screen_v1 *screen;                               /* SCREEN only */
    int (*available)(void *user);            /* UI thread; 0 = grayed, help says why */
    void (*on_change)(int value, void *user); /* UI thread: TOGGLE 0/1, CHOICE index (below) */
    void (*on_activate)(void *user);          /* ACTION (and a GROUP's left checkbox) */
    void *user;
} az_feature_v2;

/* Built-in-only flags (never accepted from a community mod: azf_register keeps only
 * AZ_ROW_DEFAULT_ON). */
#define AZ_ROW_HIDDEN 0x100u      /* persisted state, never a row (the 60 FPS memory) */
#define AZ_ROW_FORCE_ON 0x200u    /* on after every load and default, whatever was saved */
#define AZ_ROW_SHOW_GATED 0x400u  /* hidden until az_row_show(item, 1) */
#define AZ_ROW_NO_CHEVRON 0x800u  /* a screen row the compact list draws without a ">" (Updates) */
#define AZ_ROW_CHILD_ICONS 0x1000u /* a group whose members draw their icons in its pane (Extras) */
/* Beat Sync: a release build has no row for it and holds it on; a development build shows it */
#define AZF_RELEASE_LOCKED_ON (AZ_ROW_HIDDEN | AZ_ROW_FORCE_ON)
/* `.d.choices = AZ_CHOICES("a", "b")` in features.def: the labels and their count */
#define AZ_CHOICES(...)                  \
    (const char *const[]) {__VA_ARGS__}, \
        .d.n_choices = (int) (sizeof((const char *const[]) {__VA_ARGS__}) / sizeof(const char *))

enum az_feature_id {
#define AZ_FEATURE(E, ...) AZF_##E,
#define AZ_ACTION(...)
#define AZ_REGISTERED_ROWS()
#define AZ_RETIRED(n)
#include "features.def"
#undef AZ_FEATURE
#undef AZ_ACTION
#undef AZ_REGISTERED_ROWS
#undef AZ_RETIRED
    AZF_COUNT
};
#define AZF_NONE (-1)
enum az_action {
#define AZ_FEATURE(...)
#define AZ_ACTION(E, ...) AZA_##E,
#define AZ_REGISTERED_ROWS()
#define AZ_RETIRED(n)
#include "features.def"
#undef AZ_FEATURE
#undef AZ_ACTION
#undef AZ_REGISTERED_ROWS
#undef AZ_RETIRED
    AZA_COUNT
};

/* Registered rows: 16 per mod, 128 in all (plus the official ones). */
#define AZ_REG_PER_MOD 16
#define AZ_REG_MAX 128
#define AZ_VAL_COUNT (AZF_COUNT + AZ_REG_MAX) /* rows that can hold a value */
#define AZI_REG0 AZF_COUNT
#define AZI_ACT0 AZ_VAL_COUNT
#define AZI_COUNT (AZI_ACT0 + AZA_COUNT) /* every item id */
enum {
#define AZ_FEATURE(...)
#define AZ_ACTION(E, ...) AZI_##E = AZI_ACT0 + AZA_##E,
#define AZ_REGISTERED_ROWS()
#define AZ_RETIRED(n)
#include "features.def"
#undef AZ_FEATURE
#undef AZ_ACTION
#undef AZ_REGISTERED_ROWS
#undef AZ_RETIRED
    AZI_ACTION_END_
};

/* azmod.cfg line 1 as 1.2.2 wrote it (dual write): one char per legacy slot */
#define AZ_CFG_WIDTH 39
#define AZMOD_CFG_MIN_WIDTH 13 /* the narrowest line a shipped build wrote (13 mods) */

/* One row. `d` is the descriptor; the rest is resolved from it (or, for an official row, given
 * in features.def). */
struct az_row {
    az_feature_v2 d;
    int parent;             /* item id, AZF_NONE = the main list */
    int cfg;                /* azmod.cfg slot, -1 = none */
    int radio_n;            /* CHOICE kept one-hot over cfg..cfg+radio_n-1 (the drum banks) */
    int screen;             /* the azmod_mode the row opens; 0 (AZ_CLOSED) or -1 = none */
    const char *pane_label; /* the right pane's label, NULL = d.label */
};

/* ---- values: any thread, one atomic load ---------------------------------------------- */
extern int az_val[AZ_VAL_COUNT]; /* raw: TOGGLE 0/1, CHOICE index */
extern int az_eff[AZ_VAL_COUNT]; /* row_value: TOGGLE = own and every TOGGLE ancestor */
static inline bool az_on(int id) {
    return __atomic_load_n(&az_val[id], __ATOMIC_RELAXED) != 0;
}
static inline int az_get(int id) {
    return __atomic_load_n(&az_val[id], __ATOMIC_RELAXED);
}
static inline int az_value(int id) {
    return __atomic_load_n(&az_eff[id], __ATOMIC_RELAXED);
}

/* ---- the table (UI thread unless said otherwise) ------------------------------------- */
const struct az_row *az_row(int item); /* NULL: no such row (or an empty registered slot) */
const char *az_label(int item);        /* the list label; NULL out of range */
const char *az_pane_label(int item);
int az_kind(int item); /* AZ_ROW_*, -1 = none */
int az_parent(int item);
int az_depth(int item);
int az_screen(int item);     /* the azmod_mode it opens, -1 */
bool az_has_value(int item); /* a TOGGLE or CHOICE feature / registered row */
bool az_hidden(int item);    /* not a row in the menu (any list) */
bool az_active(int item);    /* every TOGGLE ancestor is on (gray and inert otherwise) */
bool az_descends(int item, int top);
int az_n_choices(int item);
const char *az_choice_label(int item, int k);
void az_row_show(int item, bool shown); /* AZ_ROW_SHOW_GATED rows */
/* Every item in display order, hidden ones included (callers skip az_hidden): official rows,
 * then the registered rows, then the utilities. Returns the count (<= AZI_COUNT). */
int az_display_order(int *out);

/* ---- changes (UI thread) -------------------------------------------------------------- */
void az_set(int id, int v);  /* store and republish; no callbacks */
void az_toggle(int id);      /* the DJ's flip: TOGGLE 0<->1, CHOICE next; runs on_change */
void az_defaults(void);      /* every value at its default, forced rows applied */
void az_apply_forced(void);  /* AZ_ROW_FORCE_ON after a load */
void az_after_load(void);    /* after the values are stored: forced rows, normalized memories */
void az_fire_changes(void);  /* after a save: registered rows' on_change (UI thread) */

/* ---- pins (the dev-only pinned SOURCE rows): a bitset over every item ---------------- */
#define AZ_PIN_WORDS ((AZI_COUNT + 63) / 64)
bool az_pinned(int item);    /* any thread */
bool az_pinnable(int item);  /* a TOGGLE or CHOICE row */
void az_pin_toggle(int item);
void az_pins_clear(void);
unsigned long long az_pins_legacy(void);              /* azmod.cfg line 3 (bit = slot) */
void az_pins_from_legacy(unsigned long long mask);
int az_pinned_items(int *out, int max);               /* in display order */

/* ---- azmod.cfg (1.2.2's file, dual-written) ------------------------------------------- */
int az_item_of_cfg(int slot);           /* the feature at a slot (a radio's first), -1 */
int az_cfg_char(int slot);              /* '0'/'1' for a save */
bool az_cfg_retired(int slot);

/* ---- registration: the SDK's register_row, row_value, row_set, row_lookup (sdk_rows.c) --- */
int azf_register(const char *mod_id, const az_feature_v2 *f); /* item id, or -1 (logged) */
void azf_reset_registered(void);         /* forget them (their saved keys stay) */
int azf_retire_owner(const char *mod_id); /* a mod that is not running: its rows hidden, callbacks dropped */
struct az_row_v1;
int azf_from_row_v1(const struct az_row_v1 *r, az_feature_v2 *f); /* the SDK's row -> the core's; -1 = too small */
int azf_row_value(int handle);           /* row_value: one atomic load, any thread; -1 = bad */
int azf_row_set(int handle, int value);  /* row_set: registered rows only, UI thread; 0 / -1 */
int azf_row_lookup(const char *key);     /* row_lookup: an official row by key, read-only */
struct az_official_setting_v1;
int azf_official_setting(unsigned index, struct az_official_setting_v1 *out);
int azf_official_setting_write(const char *key, int expected, int desired);
const char *azf_owner(int item);         /* the registering mod's id, "" for an official row */
/* A registered SCREEN row with nothing to open (no screen of its own, its mod has no page or is
 * not running) is grayed and inert. Set by the row services (sdk_rows.c); NULL = always one. */
extern int (*azf_screen_target)(int item);
int azf_conf_key(int item, char *buf, int n); /* "key" or "<mod>:<key>"; 0 = has none */
extern void (*azf_log)(const char *fmt, ...); /* NULL = silent */

/* features.def's named callbacks (azfeatures.c) */
void azf_fps60_bypass(void *user);
void azf_fps60_member_changed(int value, void *user);
void azf_analysis_changed(int value, void *user);
/* Core startup binds these before accepting menu input. No mod identity is used. */
uint32_t azf_analysis_components(void);
extern void (*azmod_analysis_configure)(uint32_t components);
extern int (*azmod_analysis_status)(unsigned deck, az_analysis_status_v1 *out);
#pragma GCC visibility pop
#endif
