/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define AZMOD_STEM_DECKS 4
#define AZF_BEAT_JUMP_2_FIRST 27
#define BPMCTRL_VT 0x12340000u
/* Real ARM64 calls make the production caller capture observable. The call sites are
 * separated beyond the native screen window, so unrelated callers cannot match. */
#define NATIVE_CALL(name)                                                                                                            \
    __asm__(".text\n.align 2\n.globl " #name "\n.type " #name ",%function\n" #name ":\nstp x29,x30,[sp,#-16]!\nmov x29,sp\nblr x1\n" \
            ".globl " #name "_return\n" #name "_return:\nldp x29,x30,[sp],#16\nret\n"                                                \
            ".size " #name ",.-" #name "\n.space 0x300\n");                                                                          \
    extern int name(long, int (*)(long));                                                                                            \
    extern char name##_return[]
NATIVE_CALL(mode_wave_call);
NATIVE_CALL(mode_title_call);
NATIVE_CALL(mode_jump_call);
NATIVE_CALL(mode_other_call);
#define WAVE_SWITCH ((uintptr_t) mode_wave_call_return)
#define TITLE_UPDATE ((uintptr_t) mode_title_call_return)
#define TITLE_UPDATE_END (TITLE_UPDATE + 4)
#define BJ_TILE_RA ((uintptr_t) mode_jump_call_return)
static int g_true_mode, g_true_mode_deck[4], g_wave_forcestems, g_force_truemode;
static unsigned g_mode_seq, g_tm_seq[4];
static struct {
    int stems_available[4];
} g_state;
static int native_mode, native_page, selected_deck = 1, jump_swap, logging;
static unsigned read_calls, fresh_calls, marks, log_calls;
static int read_mode(long object) {
    assert(object == 777);
    read_calls++;
    ++g_mode_seq;
    return native_mode;
}
static int (*g_orig_modeget)(long) = read_mode;
static void ds_pad_fresh(uintptr_t object) {
    assert(object == 777 && read_calls == fresh_calls + 1);
    fresh_calls++;
}
static int deck_for_modeobj(long object) {
    assert(object == 777);
    return selected_deck;
}
static int stems_bl_mode(void) { return 8; }
static int bl_on_native_vpage(int deck) {
    assert(deck == selected_deck);
    return native_page;
}
static void wave_mark(void) { marks++; }
static int az_on(int setting) {
    assert(setting == AZF_BEAT_JUMP_2_FIRST);
    return jump_swap;
}
static FILE *azlog_open(const char *path, const char *mode) {
    assert(!strcmp(mode, "a"));
    assert(!strcmp(path, "/tmp/azmod-drumtile.log") || !strcmp(path, "/tmp/azmod-drumbpm.log"));
    log_calls++;
    return logging ? tmpfile() : NULL;
}
static unsigned g_bpmstate_generation[4] = {11, 12, 13, 14};
static int g_bpmstate_n, g_bpmstate_pins, g_bpmstate_refused, capture_ok = 1;
static unsigned birth_deck, birth_generation;
static void native_ctor(long self, long deck, long a3, long a4) {
    assert(a3 == 3 && a4 == 4);
    *(int *) self = (int) (deck & 255);
}
static void (*g_orig_bpmctor)(long, long, long, long) = native_ctor;
static int bpmstate_capture(unsigned long self, int deck, uintptr_t vt) {
    assert(*(int *) self == deck && vt == BPMCTRL_VT);
    return capture_ok && deck >= 0 && deck < 4;
}
static void ds_tempo_birth(unsigned deck, unsigned generation) {
    birth_deck = deck;
    birth_generation = generation;
}
#include ROUTING_IMPL_INC
static void modes(void) {
    native_mode = 8;
    g_mode_seq = 40;
    assert(mode_other_call(777, modeget_hook) == 8);
    assert(g_true_mode == 8 && g_true_mode_deck[1] == 9 && g_tm_seq[1] == 40 && !marks);
    g_wave_forcestems = 1;
    g_state.stems_available[1] = 1;
    assert(mode_wave_call(777, modeget_hook) == 11 && marks == 1);
    assert(mode_title_call(777, modeget_hook) == 11 && marks == 2);
    assert(mode_other_call(777, modeget_hook) == 8 && marks == 2);
    g_force_truemode = 1;
    assert(mode_wave_call(777, modeget_hook) == 8);
    g_force_truemode = 0;
    native_page = 1;
    assert(mode_wave_call(777, modeget_hook) == 8);
    native_page = 0;
    g_state.stems_available[1] = 0;
    assert(mode_wave_call(777, modeget_hook) == 8);
    g_state.stems_available[1] = 1;
    selected_deck = -1;
    assert(mode_wave_call(777, modeget_hook) == 8);
    selected_deck = 4;
    modeget_publish(777, 6, 300);
    assert(g_true_mode == 6 && g_tm_seq[1] != 300);
    selected_deck = 1;
    native_mode = 5;
    assert(mode_jump_call(777, modeget_hook) == 5);
    jump_swap = 1;
    assert(mode_jump_call(777, modeget_hook) == 6 && g_true_mode_deck[1] == 6);
    native_mode = 6;
    assert(mode_jump_call(777, modeget_hook) == 5 && g_true_mode_deck[1] == 7);
    assert(mode_other_call(777, modeget_hook) == 6);
    native_mode = 7;
    assert(mode_jump_call(777, modeget_hook) == 7);
    assert(read_calls == fresh_calls);
    unsigned before = log_calls;
    logging = 1;
    for (int i = 0; i < 40; i++) modeget_log(100 + i);
    assert(log_calls > before && log_calls <= 30);
    before = log_calls;
    modeget_log(139);
    modeget_log(500);
    assert(log_calls == before); /* repeated modes and exhausted budget do not open logs */
}
static void births(void) {
    int object = -1;
    bpmctor_hook((long) &object, 0x102, 3, 4);
    assert(object == 2 && birth_deck == 2 && birth_generation == 13 && g_bpmstate_n == 1);
    capture_ok = 0;
    logging = 0;
    bpmctor_hook((long) &object, 3, 3, 4);
    assert(birth_deck == 3 && !birth_generation && g_bpmstate_n == 2);
    bpmctor_hook((long) &object, 255, 3, 4);
    assert(birth_deck == 255 && !birth_generation && g_bpmstate_n == 3);
}
int main(void) {
    modes();
    births();
    puts("PASS native mode caller routing, true-mode publication, logging bound and post-constructor birth");
    return 0;
}
