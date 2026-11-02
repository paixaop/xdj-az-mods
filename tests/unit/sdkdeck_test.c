/* SPDX-License-Identifier: GPL-3.0-only */
/* deck_info (sd_deck_info and sd_snapshot, extracted out of guest/azmod/sdk_deck.c by run.sh): a
 * mod hands the core its own az_deck_info with .size set, possibly from an older or newer SDK,
 * and the core fills it from azstem's per-deck snapshot:
 *   - exactly .size bytes are written, never more, for every size from the smallest accepted
 *     to larger than this core's struct; .size itself is left as the caller set it;
 *   - a struct smaller than the first field, a NULL one, or a deck outside 0..3: -1, nothing
 *     written;
 *   - without azstem every field it would give is unknown (-1, ""), never zero mistaken for a
 *     value; the length and key, which no per-deck source gives yet, are unknown (-1, "");
 *   - with azstem, each field comes from that deck's snapshot (another deck's never). */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "azmod-mod.h"
#include "../common/azdeckinfo.h"
#include "sdk_logic.h"

#define AZMOD_STEM_DECKS 4
#define AZMOD_PAD_DECKS 2
static int dg_slip_enum = 2;
static double g_cm_bpm[AZMOD_STEM_DECKS] = {120.0, 124.5, 0.0, 99.0};
static int bpm_available = 1;
static double cm_deck_bpm(int deck) { return bpm_available ? g_cm_bpm[deck] : 0.0; }
static int pad_mode_fresh(int pad_deck, int deck) {
    (void) pad_deck;
    return deck == 1 ? 4 : 0; /* deck 2: BEAT LOOP's first page; the others not known */
}
#include SDKDECK_INC

static int g_snap_calls, g_snap_deck = -9;
static int fake_snap(int deck, struct azstem_deck_snap *s) {
    g_snap_calls++;
    g_snap_deck = deck;
    if (s->size < (int) sizeof *s)
        return -1;
    s->loaded = 1;
    s->content_id = 1000u + (unsigned) deck;
    s->playhead = 44100;
    s->pos_s = 1.5 + deck;
    s->has_stems = deck == 0;
    s->stems_on = 0;
    snprintf(s->title, sizeof s->title, "Title %d", deck);
    memset(s->artist, 'A', sizeof s->artist - 1); /* a full-length field */
    s->artist[sizeof s->artist - 1] = 0;
    return 0;
}

/* a caller's buffer: the struct at the front, guard bytes after it */
static unsigned char g_buf[sizeof(az_deck_info) + 256];
static az_deck_info *prep(int size) {
    memset(g_buf, 0xA5, sizeof g_buf);
    az_deck_info *d = (az_deck_info *) g_buf;
    d->size = size;
    return d;
}
/* bytes from `from` on that are still the guard pattern */
static int untouched_from(size_t from) {
    for (size_t i = from; i < sizeof g_buf; i++)
        if (g_buf[i] != 0xA5)
            return 0;
    return 1;
}

int main(void) {
    ut_suite = "sdk: deck_info";
    const int min = (int) (offsetof(az_deck_info, loaded) + sizeof(int));
    az_deck_info *d;

    /* no azstem: unknown, not zero */
    d = prep((int) sizeof *d);
    CHECK("no azstem: filled (0)", sd_deck_info(0, d) == 0);
    CHECK("no azstem: loaded, has_stems, stems_on unknown (-1)", d->loaded == -1 && d->has_stems == -1 && d->stems_on == -1);
    CHECK("no azstem: position and length unknown (-1), no title", d->pos_s == -1.0 && d->len_s == -1.0 && !d->title[0] &&
                                                                       !d->artist[0] && !d->key[0]);
    CHECK("no azstem: the BPM feed still answers, an unknown pad mode is -1", d->bpm == 120.0 && d->pad_mode == -1);

    atomic_store(&g_sd_snap, fake_snap);
    atomic_store(&g_sd_playing[1], 1);
    d = prep((int) sizeof *d);
    CHECK("deck 2: filled (0)", sd_deck_info(1, d) == 0);
    CHECK("... from deck 2's own snapshot", g_snap_deck == 1 && d->content_id == 1001u && d->pos_s == 2.5 &&
                                                !strcmp(d->title, "Title 1") && d->loaded == 1 && d->has_stems == 0);
    CHECK("... playing, BPM and pad mode of deck 2", d->playing == 1 && d->bpm == 124.5 && d->pad_mode == 1);
    CHECK("... a full-length artist copied whole and terminated", strlen(d->artist) == sizeof d->artist - 1);
    CHECK("... the length and key unknown (no per-deck source): -1 and \"\"", d->len_s == -1.0 && !d->key[0]);
    CHECK("... .size left as the caller set it", d->size == (int) sizeof *d);
    CHECK("... nothing written past the struct", untouched_from(sizeof *d));
    bpm_available = 0;
    for (int deck = 0; deck < AZMOD_STEM_DECKS; ++deck) {
        d = prep((int) sizeof *d);
        CHECK("unavailable BPM receipt is unknown for every deck", sd_deck_info(deck, d) == 0 && d->bpm == 0.0);
    }
    bpm_available = 1;

    /* every size a caller can have: exactly that many bytes */
    int bad = 0, nsz = 0;
    for (int size = min; size <= (int) sizeof *d + 200; size++) {
        d = prep(size);
        if (sd_deck_info(3, d) != 0 || d->size != size || !untouched_from((size_t) size))
            bad++;
        nsz++;
    }
    CHECK("every struct size from the first field to beyond this core's: exactly .size bytes written", nsz > 200 && bad == 0);
    d = prep(min);
    sd_deck_info(0, d);
    CHECK("the smallest struct (size + loaded) gets loaded", d->loaded == 1 && untouched_from((size_t) min));

    d = prep(min - 1);
    CHECK("a struct without room for loaded: -1, nothing written", sd_deck_info(0, d) == -1 && untouched_from(sizeof(int)));
    d = prep(-5);
    CHECK("a negative size: -1", sd_deck_info(0, d) == -1 && untouched_from(sizeof(int)));
    d = prep((int) sizeof *d);
    CHECK("deck -1 and deck 4: -1, nothing written", sd_deck_info(-1, d) == -1 && sd_deck_info(4, d) == -1 &&
                                                         untouched_from(sizeof(int)));
    CHECK("NULL: -1", sd_deck_info(0, NULL) == -1);
    return ut_summary();
}
