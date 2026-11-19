/* SPDX-License-Identifier: GPL-3.0-only */
#define main community_baseline_main
#include "../../src/azmod/test/community/harness.c"
#undef main
#include <assert.h>

/* The harness excludes sdk_proc and thus its adjacent button adapters. The
 * runner includes those exact production definitions with source provenance. */
enum {
    EPB_V200 = 2
};
static int g_ep147_build;
static unsigned panel_presses, panel_polls;
static cm_mod *expected_mod;
static int panel_submit(cm_mod *owner, uint32_t button, uint32_t hold_ms, uint64_t *token) {
    assert(owner == expected_mod && button == 71 && hold_ms == 80 && token);
    ++panel_presses;
    *token = 90;
    return AZ_OK;
}
static int panel_poll(cm_mod *owner, uint64_t token) {
    assert(owner == expected_mod && token == 90);
    ++panel_polls;
    return AZ_OK;
}
#include "community_buttons.inc"

static void test_code_segments(void) {
    ElfW(Phdr) segments[] = {
        {.p_type = PT_NOTE, .p_flags = PF_X, .p_vaddr = 0x100, .p_memsz = 0x100},
        {.p_type = PT_LOAD, .p_flags = PF_R, .p_vaddr = 0x200, .p_memsz = 0x100},
        {.p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_vaddr = 0x400, .p_memsz = 0x100}};
    cm_mod owner = {.lo = 0x1000, .hi = 0x2000, .image_base = 0x1000, .segments = segments, .nsegments = 3};
    assert(!cm_command_code(NULL, 0x1400));
    assert(!cm_command_code(&owner, 0xfff) && !cm_command_code(&owner, 0x2000));
    assert(!cm_command_code(&owner, 0x1100) && !cm_command_code(&owner, 0x1200));
    assert(!cm_command_code(&owner, 0x13ff) && !cm_command_code(&owner, 0x1500));
    assert(cm_command_code(&owner, 0x1400) && cm_command_code(&owner, 0x14ff));
}
static void expect_tempo(int deck, int available, double expected, uint64_t expected_generation, uint64_t expected_revision, uint64_t expected_observed) {
    double bpm = -1;
    uint64_t generation = 99, revision = 99, observed = 99;
    assert(cm_tempo_read(deck, &bpm, &generation, &revision, &observed) == available);
    assert(bpm == expected && generation == expected_generation && revision == expected_revision);
    assert(compare_timestamps(observed, expected_observed, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
}
static void test_tempo_receipts(void) {
    expect_tempo(-1, 0, 0, 0, 0, 0);
    expect_tempo(AZMOD_STEM_DECKS, 0, 0, 0, 0, 0);
    expect_tempo(0, 0, 0, 0, 0, 0);
    fake_bpm_generation[0] = 12;
    cm_tempo_publish(0, 1, 123.5, 12, 100);
    expect_tempo(0, 1, 123.5, 12, 1, 100);
    fake_bpm_generation[0] = 13;
    expect_tempo(0, 0, 0, 0, 0, 0);
    fake_bpm_generation[0] = 12;
    pthread_mutex_lock(&g_cm_bpm_mu);
    expect_tempo(0, 0, 0, 0, 0, 0);
    cm_tempo_publish(0, 1, 125, 12, 200); /* event changes even if publishing contends */
    pthread_mutex_unlock(&g_cm_bpm_mu);
    expect_tempo(0, 0, 0, 0, 0, 0); /* stale receipt cannot survive a dropped observation */
    cm_tempo_publish(0, 0, 125, 12, 300);
    expect_tempo(0, 0, 0, 12, 3, 300);
    cm_tempo_publish(0, 1, 126, 12, 400);
    expect_tempo(0, 1, 126, 12, 4, 400);
    atomic_store(&g_cm_tempo_event[0], UINT64_MAX);
    expect_tempo(0, 0, 0, 0, 0, 0);
}
static void test_buttons(void) {
    expected_mod = &g_cm[0];
    *expected_mod = (cm_mod) {.id = "fixture", .state = AZCM_LOADED, .lo = 1, .hi = UINTPTR_MAX};
    g_cm_n = 1;
    uint64_t token = 0;
    assert(cm_button_press("missing", 71, 80, &token) == AZ_INVALID);
    assert(cm_button_poll("missing", 90) == AZ_INVALID);
    expected_mod->state = AZCM_NEW;
    assert(cm_button_press("fixture", 71, 80, &token) == AZ_INVALID);
    assert(cm_button_poll("fixture", 90) == AZ_INVALID);
    expected_mod->state = AZCM_LOADED;
    assert(cm_button_press("fixture", 71, 80, &token) == AZ_UNSUPPORTED);
    assert(cm_button_poll("fixture", 90) == AZ_UNSUPPORTED);
    g_ep147_build = EPB_V200;
    g_cm_in_frame = 1;
    assert(cm_button_press("fixture", 71, 80, &token) == AZ_INVALID);
    g_cm_in_frame = 0;
    assert(!panel_presses && !panel_polls && !token);
    assert(cm_button_press("fixture", 71, 80, &token) == AZ_OK && token == 90);
    assert(cm_button_poll("fixture", token) == AZ_OK);
    assert(panel_presses == 1 && panel_polls == 1);
}
int main(void) {
    test_code_segments();
    test_tempo_receipts();
    test_buttons();
    puts("PASS community bindings: executable segment admission, coherent tempo receipts, stale/contended clocks, caller/state/profile button admission");
    return 0;
}
