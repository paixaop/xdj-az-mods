/* SPDX-License-Identifier: GPL-3.0-only */
/* Reuse the ordinary native ABI/owning Unit fixture; this test continues through
 * the real observer, C export and sdk_deck_tick to a public on_deck_event caller. */
#include SDKCUE_NATIVE_FIXTURE
extern "C" {
void cue_public_init(void);
void cue_public_tick(void);
unsigned cue_public_count(unsigned, unsigned);
void cue_public_switch_owner(void);
void cue_public_revision(unsigned, int);
void cue_public_capability(int);
}
int main(int argc, char **argv) {
    CHECK(argc == 2 && (argv[1][0] == '1' || argv[1][0] == '2'), "profile argument");
    configure(1, argv[1][0] - '0');
    Fixture contexts[4]; Lease held[4];
    cue_public_init(); cue_public_tick();
    cue_public_capability(0);
    for (unsigned d = 0; d < 4; ++d) {
        CHECK(submit(d, contexts[d]), "native cue listener submitted");
        cue_public_capability(1); // submitted support exists before a receipt
        held[d] = contexts[d].lease();
        emit(held[d], record(d), contexts[d].routing_tag());
        cue_public_revision(d,0); // pending callback is not a GUI collection receipt
        cue_public_tick();
        cue_public_revision(d,1);
        CHECK(cue_public_count(0, d) == 1, "actual callback reaches public cue event");
        CHECK(cue_public_count(1, d) == 0, "unloaded mod receives no event");
        for (unsigned untouched = d + 1; untouched < 4; ++untouched)
            CHECK(cue_public_count(0, untouched) == 0, "public callback remains isolated by owned deck");
        emit(held[d], record(0), contexts[d].routing_tag());
        emit(held[d], record(8), contexts[d].routing_tag());
        cue_public_tick(); cue_public_tick();
        CHECK(cue_public_count(0, d) == 2, "GUI tick coalesces native pending changes exactly once");
    }
    cue_public_switch_owner();
    for (unsigned d = 0; d < 4; ++d) {
        emit(held[d], record(2), contexts[d].routing_tag()); cue_public_tick();
        CHECK(cue_public_count(0, d) == 2 && cue_public_count(1, d) == 1,
              "native callback is delivered only to current loaded public subscriber");
        retire(d); cue_public_tick();
        cue_public_revision(d,0);
        CHECK(cue_public_count(1, d) == 2, "native retirement invalidates public cue state once");
        emit(held[d], record(3), contexts[d].routing_tag()); cue_public_tick();
        CHECK(cue_public_count(1, d) == 2, "held retired native listener cannot produce public events");
        CHECK(submit(d, contexts[d]), "replacement native cue listener");
        auto replacement = contexts[d].lease();
        emit(replacement, record(4), contexts[d].routing_tag()); cue_public_tick();
        cue_public_revision(d,1);
        CHECK(cue_public_count(1, d) == 3, "replacement unit resumes public cue events");
    }
    configure(0, argv[1][0] - '0');
    cue_public_capability(0);
    for(unsigned d=0;d<4;++d)cue_public_revision(d,0);
    puts("PASS native cue callback -> versioned C export -> public on_deck_event: four decks and retirement");
}
