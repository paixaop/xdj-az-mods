/* SPDX-License-Identifier: GPL-3.0-only */
#include "sdk_command_target.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static az_deck_snapshot live;
static az_native_cue_snapshot bank;
static az_data_capabilities caps;
static unsigned deck_reads, cue_reads, cap_reads;
static int fail_deck, fail_cues, fail_caps, change;
static int deck_read(int deck, az_deck_snapshot *out) {
    assert(deck == 2);
    ++deck_reads;
    *out = live;
    if (deck_reads == 2) {
        if (change == 1) out->native_load_generation++;
        if (change == 2) out->native_load_unit_generation++;
        if (change == 3) out->native_load_invalidation_epoch++;
        if (change == 7) return AZ_BUSY;
    }
    return fail_deck;
}
static int cue_read(int deck, az_native_cue_snapshot *out) {
    assert(deck == 2);
    ++cue_reads;
    *out = bank;
    if (cue_reads == 2) {
        if (change == 4) out->revision++;
        if (change == 5) out->source_rate_hz = 44100;
    }
    return fail_cues;
}
static int cap_read(az_data_capabilities *out) {
    ++cap_reads;
    *out = caps;
    if (cap_reads == 2 && change == 6) out->commands = 0;
    if (cap_reads == 2 && change == 8) return AZ_FAILED;
    return fail_caps;
}
static const struct az_command_target_source source = {deck_read, cue_read, cap_read};
static void setup(void) {
    live = (az_deck_snapshot){.size=sizeof live, .version=AZ_DATA_VERSION, .deck=2,
        .valid=AZ_LIVE_LOADED|AZ_LIVE_NATIVE_LOAD, .loaded=1, .native_load_generation=40,
        .native_load_unit_generation=9, .native_load_invalidation_epoch=12, .native_load_observed_ns=100};
    bank = (az_native_cue_snapshot){.size=sizeof bank, .version=AZ_DATA_VERSION, .deck=2,
        .native_load_generation=40, .revision=8, .source_rate_hz=48000, .count=1};
    bank.entries[0]=(az_cue_entry){.valid=AZ_CUE_START, .start_s=1.0};
    caps = (az_data_capabilities){.size=sizeof caps, .version=AZ_DATA_VERSION, .commands=1ull<<AZ_CMD_LOOP_EXIT};
    deck_reads=cue_reads=cap_reads=0;
    fail_deck=fail_cues=fail_caps=AZ_OK;
    change=0;
}
static void refuses(int expected) {
    az_command_target_v1 out;
    memset(&out, 0x5a, sizeof out); out.size=sizeof out;
    az_command_target_v1 before=out;
    assert(az_command_target_read(&source,2,40,200,&out)==expected);
    assert(!memcmp(&out,&before,sizeof out));
}
static void malformed_records(void) {
    setup();live.size=4;refuses(AZ_PENDING);
    setup();live.version=99;refuses(AZ_PENDING);
    setup();live.deck=1;refuses(AZ_PENDING);
    setup();live.loaded=0;refuses(AZ_PENDING);
    setup();live.valid=0;refuses(AZ_PENDING);
    setup();live.valid=AZ_LIVE_LOADED;refuses(AZ_STALE);
    setup();live.native_load_unit_generation=0;refuses(AZ_STALE);
    setup();live.native_load_generation=0;refuses(AZ_STALE);
    setup();live.native_load_observed_ns=0;refuses(AZ_STALE);
    setup();live.native_load_observed_ns=201;refuses(AZ_STALE);
    setup();bank.size=4;refuses(AZ_STALE);
    setup();bank.version=99;refuses(AZ_STALE);
    setup();bank.deck=1;refuses(AZ_STALE);
    setup();bank.revision=0;refuses(AZ_STALE);
    setup();bank.count=AZ_NATIVE_CUE_LIMIT+1;refuses(AZ_STALE);
    setup();bank.native_load_generation=41;refuses(AZ_STALE);
    setup();bank.source_rate_hz=0;refuses(AZ_STALE);
    setup();bank.source_rate_hz=0x7fffffff;refuses(AZ_STALE);
    setup();caps.size=4;refuses(AZ_FAILED);
    setup();caps.version=99;refuses(AZ_FAILED);
    setup();caps.commands=0;refuses(AZ_UNSUPPORTED);
}
static void provider_failures(void) {
    setup();fail_deck=AZ_UNSUPPORTED;refuses(AZ_UNSUPPORTED);
    setup();fail_cues=AZ_BUSY;refuses(AZ_BUSY);
    setup();fail_caps=AZ_FAILED;refuses(AZ_FAILED);
    for(int mode=1;mode<=6;++mode){setup();change=mode;refuses(AZ_STALE);}
    setup();change=7;refuses(AZ_BUSY);
    setup();change=8;refuses(AZ_FAILED);
}
static void invalid_calls(void) {
    setup();az_command_target_v1 out={.size=sizeof out};
    assert(az_command_target_read(&source,2,40,200,NULL)==AZ_INVALID);
    out.size=4;assert(az_command_target_read(&source,2,40,200,&out)==AZ_INVALID);out.size=sizeof out;
    assert(az_command_target_read(&source,-1,40,200,&out)==AZ_INVALID);
    assert(az_command_target_read(&source,4,40,200,&out)==AZ_INVALID);
    assert(az_command_target_read(&source,2,0,200,&out)==AZ_INVALID);
    assert(az_command_target_read(&source,2,40,0,&out)==AZ_INVALID);
    assert(az_command_target_read(NULL,2,40,200,&out)==AZ_INVALID);
    struct az_command_target_source missing=source;
    missing.deck_snapshot=NULL;assert(az_command_target_read(&missing,2,40,200,&out)==AZ_INVALID);
    missing=source;missing.native_cues=NULL;assert(az_command_target_read(&missing,2,40,200,&out)==AZ_INVALID);
    missing=source;missing.capabilities=NULL;assert(az_command_target_read(&missing,2,40,200,&out)==AZ_INVALID);
}
int main(void) {
    setup();az_command_target_v1 out={.size=sizeof out};
    assert(az_command_target_read(&source,2,40,200,&out)==AZ_OK);
    assert(out.version==AZ_DATA_VERSION && out.commands==caps.commands);
    assert(out.deck.native_load_generation==40 && out.cues.revision==8 && out.cues.source_rate_hz==48000);
    assert(out.cues.entries[0].valid==AZ_CUE_START);
    setup();bank.source_rate_hz=44100;
    assert(az_command_target_read(&source,2,40,200,&out)==AZ_OK && out.cues.source_rate_hz==44100);
    malformed_records();provider_failures();invalid_calls();
    puts("PASS coherent target: copies, clocks, replaced loads, cue revisions, capability loss and error preservation");
    return 0;
}
