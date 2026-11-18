/* SPDX-License-Identifier: GPL-3.0-only */
#include "azexit.h"
#include "aztimestamp.h"
#ifdef AZSDK_CUE_STAGE_TESTING
#include "azcuerecallbridge.h"
#include "azcuerecall.h"
#endif
#include "azloadobserver.h"
#include "azmod-mod.h"
#include <stdatomic.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "native_abi_fixture.h"
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>

#include <float.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#define store(object, offset, value)                                                       \
    do {                                                                                   \
        __auto_type native_value = (value);                                                \
        memcpy((unsigned char *) (object) + (offset), &native_value, sizeof native_value); \
    } while (0)
#define put(raw, offset, value)                                             \
    do {                                                                    \
        __auto_type native_value = (value);                                 \
        assert((offset) + sizeof native_value <= sizeof(raw).bytes);        \
        memcpy((raw).bytes + (offset), &native_value, sizeof native_value); \
    } while (0)
static void pause_ms(unsigned ms) {
    struct timespec duration = {ms / 1000, (long) (ms % 1000) * 1000000};
    while (nanosleep(&duration, &duration)) assert(errno == EINTR);
}



typedef struct az_command_request az_command_request;
typedef struct az_command_result az_command_result;
typedef struct azexit_cue_request azexit_cue_request;
typedef struct azexit_cue_stage_sample azexit_cue_stage_sample;
typedef struct azexit_cue_target azexit_cue_target;
typedef struct azexit_native_color azexit_native_color;
typedef struct azexit_native_entry azexit_native_entry;
typedef struct azexit_profile azexit_profile;
typedef struct azexit_request azexit_request;
typedef struct azexit_result azexit_result;
typedef struct azexit_service azexit_service;
typedef struct azsdk_load_sample azsdk_load_sample;

void sdk_exit_public_bind(const azexit_service *);
uint64_t sdk_exit_public_capabilities(void);
int sdk_exit_public_submit(const char *, const az_command_request *, az_command_result *);
int sdk_exit_public_poll(const char *, uint64_t, az_command_result *);
int sdk_exit_public_cancel(const char *, uint64_t, az_command_result *);
int sdk_exit_public_release(const char *, uint64_t);
void sdk_exit_public_auth(void);
void sdk_exit_public_retire(int owner);
void sdk_exit_public_clock(uint64_t time);

typedef struct {
    _Alignas(8) unsigned char bytes[0x10];
} Raw_10;
typedef struct {
    _Alignas(8) unsigned char bytes[0x20];
} Raw_20;
typedef struct {
    _Alignas(8) unsigned char bytes[0x28];
} Raw_28;
typedef struct {
    _Alignas(8) unsigned char bytes[0x30];
} Raw_30;
typedef struct {
    _Alignas(8) unsigned char bytes[0x40];
} Raw_40;
typedef struct {
    _Alignas(8) unsigned char bytes[0x80];
} Raw_80;
typedef struct {
    _Alignas(8) unsigned char bytes[0xa0];
} Raw_a0;
typedef struct {
    _Alignas(8) unsigned char bytes[0xb0];
} Raw_b0;
typedef struct {
    _Alignas(8) unsigned char bytes[0x100];
} Raw_100;
typedef struct {
    _Alignas(8) unsigned char bytes[0x200];
} Raw_200;
typedef struct {
    _Alignas(8) unsigned char bytes[0x250];
} Raw_250;
typedef struct {
    _Alignas(8) unsigned char bytes[0x300];
} Raw_300;
typedef struct {
    _Alignas(8) unsigned char bytes[0x400];
} Raw_400;
static int32_t get_int32_t(const void *object, unsigned offset) {
    int32_t value;
    memcpy(&value, (const unsigned char *) object + offset, sizeof value);
    return value;
}
static uint32_t get_uint32_t(const void *object, unsigned offset) {
    uint32_t value;
    memcpy(&value, (const unsigned char *) object + offset, sizeof value);
    return value;
}
static uint64_t get_uint64_t(const void *object, unsigned offset) {
    uint64_t value;
    memcpy(&value, (const unsigned char *) object + offset, sizeof value);
    return value;
}
static uintptr_t get_uintptr_t(const void *object, unsigned offset) {
    uintptr_t value;
    memcpy(&value, (const unsigned char *) object + offset, sizeof value);
    return value;
}
static bool sample_at(uint64_t sample, uint64_t expected, uint32_t rate) {
    const int order = compare_timestamps(sample, expected, rate, rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
typedef struct Native Native;
struct Native {
    Raw_200 unit;
    Raw_300 core;
    Raw_80 bundle, functions;
    Raw_40 basis;
    Raw_20 jump;
    Raw_250 facade;
    Raw_200 beat;
    Raw_100 quantize, slip;
    Raw_400 context;
    Raw_100 retained_property;
    Raw_80 play_pause;
    Raw_b0 cue_functions;
    Raw_100 hot_properties[8];
    unsigned cue_fault, cue_seeks, cue_jump_calls, cue_seek_calls;
    bool cue_seek_noop;
    uint64_t cue_seek_window;
    Raw_30 manager;
    Raw_10 merger;
    Raw_10 available;
    Raw_a0 commands[8];
    Raw_80 chains[4];
    Raw_10 source_holder;
    bool deferred, executing, pause_engine, reloop_effect, retained;
    bool wrong_beat, normalize_beat;
    bool maintained_loop, cycle_future, replace_future;
    bool invalidate_after_pre, expire_after_pre;
    bool maintenance_play;
    bool preparatory_loop, making_setup, omit_future;
    unsigned setup_fault, setup_cancels;
    bool pending_wrapper;
    unsigned wrapper_fault, wrapper_executes;
    unsigned maintenance_mode;
    unsigned maintenance_fault, schedules, maintenance_executes;
    unsigned pending_operation, pending_argument;
    double pending_ms;
    unsigned effects, command_executes, destroyed, jumps, cancels;
};
uintptr_t set_table[18], cue_table[2], cue_base_table[2], play_pause_table[2];
uintptr_t unit_table[4], core_table[4], facade_table[9], beat_table[17], quantize_table[2], slip_table[2], box_table[6];
uintptr_t command_tables[AZEXIT_COMMAND_TYPES][6], chain_table[2], jump_table[8];
uintptr_t merger_table[3], interval_table[2], interval_source_table[2];
static void native_merge() {}
Native natives[4] = {[0 ... 3] = {.retained = true, .maintenance_play = true}};
_Atomic(uint64_t) loads[4];
_Atomic(unsigned) calls = 0;
_Atomic(bool) hold = false, inside = false, retired = false;
bool wrong_thread, process_result = true, process_throw, exit_throw, exit_noop, replace_load;
typedef void (*Process)(void *, const void *);
typedef bool (*Preprocess)(void *, const void *);
typedef void (*ManagerSubmit)(void *, const void *);
typedef int (*QuantizeSubmit)(void *, int, const void *, const void *, const void *, double);
typedef void (*QuantizeCancel)(void *);
Process hook;
Process core_jump_hook;
void (*hot_countdown_hooks[2])(void *);
unsigned hot_countdown_route;
Preprocess pre_hook;
ManagerSubmit manager_hook;
QuantizeSubmit quantize_hook;
QuantizeCancel quantize_cancel_hook;
_Thread_local bool owner_frame;
unsigned forwards, pre_forwards;
unsigned command_throw_role;
int frame;
enum {
    STOP_BACKGROUND = 5,
    SLIP_STATE = 6,
    QUANTIZE_PENDING = AZEXIT_QUANTIZE_PENDING
};
static void active(unsigned d);
static void apply_set(unsigned d);
static void native_cue_action(void *facade, unsigned operation, unsigned kind);
static void maintain_loop(unsigned d);
static void advance_load(unsigned deck);
static void dispose_pending(unsigned d);
static unsigned deck_for(const void *object, unsigned member) {
    for (unsigned d = 0; d < 4; ++d) {
        const void *candidate = member == 0 ? ((void *) (natives[d].core.bytes)) : member == 1 ? ((void *) (natives[d].facade.bytes))
                                                                               : member == 2   ? ((void *) (natives[d].manager.bytes))
                                                                               : member == 3   ? ((void *) (natives[d].quantize.bytes))
                                                                               : member == 5   ? ((void *) (natives[d].beat.bytes))
                                                                                               : ((void *) (natives[d].jump.bytes));
        if (candidate == object) return d;
    }
    assert(false);
    return 4;
}
static uintptr_t object_word(const void *object, unsigned offset) { return get_uintptr_t(object, offset); }
static void retain(uintptr_t object) {
    if (object) store(((void *) (object)), 8, get_uint32_t(((void *) (object)), 8) + 1);
}
static void release(uintptr_t object) {
    if (!object) return;
    __auto_type value = ((void *) (object));
    uint32_t refs = get_uint32_t(value, 8);
    assert(refs);
    store(value, 8, refs - 1);
    if (refs == 1) ((void (*)(void *))(object_word(((void *) (object_word(value, 0))), 0)))(value);
}
static bool run_command(uintptr_t object) {
    __auto_type value = ((void *) (object));
    uintptr_t table = object_word(value, 0);
    return ((bool (*)(void *))(object_word(((void *) (table)), 16)))(value);
}
static uintptr_t make_command(unsigned d, unsigned index, unsigned type, unsigned role) {
    if (get_uint32_t(natives[d].commands[index].bytes, 8)) {
        index = 0;
        while (index < 8 && get_uint32_t(natives[d].commands[index].bytes, 8)) ++index;
        assert(index < 8);
    }
    __auto_type command = &(natives[d].commands[index]);
    assert(get_uint32_t(command->bytes, 8) == 0);
    memset(command->bytes, 0, sizeof command->bytes);
    put((*command), 0, ((uintptr_t) (command_tables[type])));
    put((*command), 8, (uint32_t) (1));
    put((*command), 0x20, ((uintptr_t) natives[d].quantize.bytes));
    put((*command), 0x90, (uint32_t) (d));
    put((*command), 0x94, (uint32_t) (role ? role : type + 1));
    return ((uintptr_t) command->bytes);
}
static uintptr_t make_chain(unsigned d, uintptr_t head) {
    {
        for (unsigned range_index_1 = 0; range_index_1 < sizeof(natives[d].chains) / sizeof(natives[d].chains)[0]; ++range_index_1) {
            __auto_type chain = &(natives[d].chains[range_index_1]);

            if (get_uint32_t(chain->bytes, 8)) continue;
            memset(chain->bytes, 0, sizeof chain->bytes);
            put((*chain), 0, ((uintptr_t) (chain_table)));
            put((*chain), 8, (uint32_t) (1));
            put((*chain), 0x18, head);
            retain(head);
            return ((uintptr_t) chain->bytes);
        }
    }
    assert(false);
    return 0;
}
static void command_destroy(void *self) {
    unsigned d = get_uint32_t(self, 0x90);
    assert(d < 4 && get_uint32_t(self, 8) == 0);
    ++natives[d].destroyed;
    uintptr_t next = object_word(self, 0x18);
    store(self, 0x18, (uintptr_t) (0));
    release(next);
}
static void command_delete(void *self) { command_destroy(self); }
static void chain_destroy(void *self) {
    assert(get_uint32_t(self, 8) == 0);
    uintptr_t head = object_word(self, 0x18);
    store(self, 0x18, (uintptr_t) (0));
    release(head);
}
static void native_manager_submit(void *self, const void *reference) {
    unsigned d = deck_for(self, 2);
    __auto_type n = &(natives[d]);
    uintptr_t chain = object_word(reference, 0);
    unsigned offset = n->executing ? 8 : 0;
    uintptr_t old = object_word(self, offset);
    retain(chain);
    store(self, offset, chain);
    release(old);
}
static void submit_chain(unsigned d, uintptr_t head) {
    uintptr_t chain = make_chain(d, head);
    manager_hook(natives[d].manager.bytes, &chain);
    release(chain);
}
static void native_jump(void *self, const void *property, int mode, const void *reference, bool option, double timing) {
    unsigned d = deck_for(self, 4);
    assert(property == natives[d].context.bytes + 0x160 && mode == 0 && option && timing == 2.5);
    uintptr_t continuation = object_word(reference, 0);
    __auto_type n = &(natives[d]);
    uintptr_t jump = make_command(d, 2, 0, n->making_setup ? 2004 : 0);
    if (n->making_setup) {
        store(((void *) (jump)), 0x20, ((uintptr_t) n->core.bytes));
        store(((void *) (jump)), 0x28, ((uintptr_t) (interval_source_table)));
        memcpy(((void *) (jump + 0x30)), n->context.bytes + 0x190, 12);
        uint64_t in = get_uint64_t(n->context.bytes + 0x160, 0x40);
        store(((void *) (jump)), 0x40, in + 123); /* preparation range differs from loop IN */
        store(((void *) (jump)), 0x48, in + 456);
        store(((void *) (jump)), 0x50, get_uintptr_t(n->context.bytes + 0x160, 0x58));
    }
    store(((void *) (jump)), 0x18, continuation);
    retain(continuation);
    if (n->making_setup) {
        uintptr_t chain = make_chain(d, jump);
        memcpy(((void *) (chain + 0x20)), ((void *) (continuation + 0x68)), 0x28);
        store(((void *) (chain)), 0x48, (uint8_t) (n->maintenance_mode));
        store(((void *) (chain)), 0x50, DBL_MAX);
        manager_hook(n->manager.bytes, &chain);
        release(chain);
    } else submit_chain(d, jump);
    release(jump);
}
static uint32_t native_jump_cancel(void *self) {
    unsigned d = deck_for(self, 4);
    __auto_type n = &(natives[d]);
    ++n->setup_cancels;
    uint32_t removed = 0;
    {
        unsigned range_values_2[] = {0u, 8u};
        for (unsigned range_index_2 = 0; range_index_2 < sizeof(range_values_2) / sizeof(range_values_2)[0]; ++range_index_2) {
            unsigned offset = range_values_2[range_index_2];

            uintptr_t chain = object_word(n->manager.bytes, offset);
            if (!chain) continue;
            uintptr_t head = object_word(((void *) (chain)), 0x18);
            if (!head || object_word(((void *) (head)), 0) != ((uintptr_t) (command_tables[0]))) continue;
            put(n->manager, offset, (uintptr_t) (0));
            release(chain); /* stock cancelByType releases the whole matching chain */
            ++removed;
        }
    }
    return removed;
}
typedef void (*Schedule)(void *, const void *, const void *, int, const void *, bool, double);
static void native_schedule(void *self, const void *forward, const void *reverse, int mode,
                            const void *reference, bool option, double timing) {
    unsigned d = deck_for(self, 4);
    __auto_type n = &(natives[d]);
    assert(owner_frame && option && timing == DBL_MAX);
    ++n->schedules;
    uintptr_t callback = object_word(reference, 0);
    uintptr_t jump = make_command(d, 2, 0, 2001);
    store(((void *) (jump)), 0x20, ((uintptr_t) n->core.bytes));
    memcpy(((void *) (jump + 0x28)), forward, 0x30);
    store(((void *) (jump)), 0x18, callback);
    retain(callback);
    uintptr_t chain = make_chain(d, jump);
    memcpy(((void *) (chain + 0x20)), reverse, 0x28);
    store(((void *) (chain)), 0x48, (uint8_t) (mode == 0 ? n->maintenance_mode : (unsigned) (mode)));
    store(((void *) (chain)), 0x49, (uint8_t) (0));
    store(((void *) (chain)), 0x50, timing);
    manager_hook(n->manager.bytes, &chain);
    release(chain);
    release(jump);
}
static void maintain_loop(unsigned d) {
    __auto_type n = &(natives[d]);
    __auto_type property = n->context.bytes + 0x160;
    Raw_30 forward = {0};
    Raw_28 reverse = {0};
    put(forward, 0, ((uintptr_t) (interval_source_table)));
    memcpy(forward.bytes + 8, property + 0x30, 12);
    put(forward, 0x18, get_uint64_t(property, 0x40));
    put(forward, 0x20, get_uint64_t(property, 0x98));
    put(forward, 0x28, get_uintptr_t(property, 0x58));
    put(reverse, 0, ((uintptr_t) (interval_table)));
    memcpy(reverse.bytes + 8, property + 0x30, 12);
    put(reverse, 0x18, get_uint64_t(property, 0x98));
    put(reverse, 0x20, get_uint64_t(property, 0x40));
    if (n->maintenance_fault == 2 || (n->making_setup && n->setup_fault == 1)) forward.bytes[8] ^= 2;
    if (n->maintenance_fault == 3) put(reverse, 0x20, get_uint64_t(property, 0x40) + 1);
    uintptr_t loop = make_command(d, 0, 3, 2000);
    store(((void *) (loop)), 0x20, n->making_setup && n->setup_fault == 2 ? ((uintptr_t) natives[(d + 1) % 4].basis.bytes) : ((uintptr_t) n->basis.bytes));
    store(((void *) (loop)), 0x28, ((uintptr_t) (property)) + (n->maintenance_fault == 1 ? 8 : 0));
    memcpy(((void *) (loop + 0x38)), forward.bytes, sizeof forward.bytes);
    memcpy(((void *) (loop + 0x68)), reverse.bytes, sizeof reverse.bytes);
    bool wrapped = n->pending_wrapper && !n->making_setup;
    uintptr_t wrapper = 0, continuation = loop;
    if (wrapped) {
        if (n->wrapper_fault == 3)
            store(((void *) (loop)), 0x20, ((uintptr_t) natives[(d + 1) % 4].basis.bytes));
        if (n->wrapper_fault == 4)
            store(((void *) (loop)), 0x40, get_uint64_t(((void *) (loop)), 0x40) ^ 2);
        uintptr_t old = object_word(n->quantize.bytes, 0x30);
        retain(loop);
        put(n->quantize, 0x30, loop);
        put(n->quantize, 0x28, (uint32_t) (n->wrapper_fault == 5 ? 3 : 2));
        release(old);
        wrapper = make_command(d, 3, 13, 2005);
        store(((void *) (wrapper)), 0x20,
              n->wrapper_fault == 1 ? ((uintptr_t) natives[(d + 1) % 4].quantize.bytes) + 0x20 : ((uintptr_t) n->quantize.bytes) + 0x20);
        store(((void *) (wrapper)), 0x28, (uint32_t) (n->wrapper_fault == 2 || n->wrapper_fault == 5 ? 3 : 2));
        if (n->wrapper_fault == 6) {
            uintptr_t extra = make_command(d, 7, 2, 2003);
            put(n->quantize, 0xe0, extra);
        }
        continuation = wrapper;
    }
    uintptr_t play = 0;
    if (n->maintenance_play || n->maintenance_fault == 5) {
        play = make_command(d, 1, 1, 2002);
        store(((void *) (play)), 0x20, ((uintptr_t) n->core.bytes));
        store(((void *) (continuation)), 0x18, play);
        retain(play);
        if (n->maintenance_fault == 5) {
            uintptr_t extra = make_command(d, 7, 2, 2003);
            store(((void *) (play)), 0x18, extra);
            retain(extra);
            release(extra);
        }
    }
    if (n->making_setup) {
        __auto_type initial = n->setup_fault == 3 ? native_jump : ((void (*)(void *, const void *, int, const void *, bool, double))(jump_table[2]));
        initial(n->jump.bytes, property, 0, &loop, true, 2.5);
    } else {
        __auto_type schedule = ((Schedule) (jump_table[4]));
        schedule(n->jump.bytes, forward.bytes, reverse.bytes, n->maintenance_fault == 4 ? 1 : 0, &continuation, true, DBL_MAX);
    }
    release(play);
    release(wrapper);
    release(loop);
}
static void native_core_jump(void *, const void *);
static bool command_execute(void *self) {
    unsigned d = get_uint32_t(self, 0x90);
    assert(d < 4 && owner_frame);
    __auto_type n = &(natives[d]);
    ++n->command_executes;
    unsigned role = get_uint32_t(self, 0x94);
    if (role == command_throw_role) fixture_raise();
    if (role == 2005) {
        assert(object_word(self, 0x20) == ((uintptr_t) n->quantize.bytes) + 0x20);
        assert(get_uint32_t(self, 0x28) == get_uint32_t(n->quantize.bytes, 0x28));
        ++n->wrapper_executes;
        uintptr_t callback = object_word(n->quantize.bytes, 0x30);
        put(n->quantize, 0x30, (uintptr_t) (0));
        put(n->quantize, 0x28, (uint32_t) (-1));
        assert(callback);
        (void) run_command(callback); /* native owner transfers callback to execution */
        release(callback);
    } else if (role == 2000) {
        ++n->maintenance_executes;
        maintain_loop(d); /* stock LoopCommand schedules the next wrap forever */
    } else if (role == 2001) {
        ++n->jumps;
    } else if (role == 2002 || role == 2003) {
        ++n->effects;
    } else if (role == 1001) {
        Raw_30 position = {0};
        memcpy(position.bytes + 8, ((const unsigned char *) (self)) + 0x30, 12);
        put(position, 0x18, get_uint64_t(self, 0x40) + (uint64_t) (n->cue_fault == 13));
        if (n->cue_fault == 5) native_core_jump(n->core.bytes, position.bytes);
        else core_jump_hook(n->core.bytes, position.bytes);
    } else if (role == 1000) {
        apply_set(d);
    } else if (role == QUANTIZE_PENDING + 1) {
        assert(object_word(self, 0x20) == ((uintptr_t) n->quantize.bytes));
        uintptr_t callback = object_word(n->quantize.bytes, 0xf8);
        if (callback) (void) run_command(callback);
        put(n->quantize, 0xf8, (uintptr_t) (0));
        put(n->quantize, 0x98, (int32_t) (-1));
        release(callback);
    } else if (role == STOP_BACKGROUND + 1) {
        uintptr_t continuation = make_command(d, 1, SLIP_STATE, 0);
        __auto_type jump = ((void (*)(void *, const void *, int, const void *, bool, double))(jump_table[2]));
        jump(n->jump.bytes, n->context.bytes + 0x160, 0, &continuation, true, 2.5);
        release(continuation);
    } else if (role == SLIP_STATE + 1) {
        ++n->effects;
        if (n->reloop_effect) active(d);
        else put(n->context, 0x190, (uint8_t) (0));
        put(n->slip, 0x68, (uint8_t) (0));
        put(n->beat, 0xbc, (uint8_t) (0));
        if (n->reloop_effect && n->maintained_loop) maintain_loop(d);
    } else if (role == 1) ++n->jumps;
    else ++n->effects; /* independent native command used by incarnation tests */
    return role != SLIP_STATE + 1 && !(role == 1000 && n->pending_operation == AZ_CMD_MAIN_RECALL); /* native false is a valid continuation result */
}
static int native_quantize_submit(void *self, int selector, const void *reference, const void *options, const void *property, double timing) {
    unsigned d = deck_for(self, 3);
    __auto_type n = &(natives[d]);
    assert(selector == 7 && options == &frame && property == n->context.bytes + 0x160 && timing == 1.25);
    uintptr_t callback = object_word(reference, 0);
    retain(callback);
    uintptr_t old = object_word(self, 0xf8);
    put(n->quantize, 0xf8, callback);
    put(n->quantize, 0x98, (int32_t) (7));
    release(old);
    /* An already pending native Quantize node processes EXIT's changed state. */
    bool already_pending = false;
    {
        unsigned range_values_3[] = {0u, 8u};
        for (unsigned range_index_3 = 0; range_index_3 < sizeof(range_values_3) / sizeof(range_values_3)[0]; ++range_index_3) {
            unsigned offset = range_values_3[range_index_3];

            uintptr_t chain = object_word(n->manager.bytes, offset);
            if (!chain) continue;
            uintptr_t head = object_word(((void *) (chain)), 0x18);
            already_pending |= head && object_word(((void *) (head)), 0) == ((uintptr_t) (command_tables[QUANTIZE_PENDING])) &&
                               object_word(((void *) (head)), 0x20) == ((uintptr_t) n->quantize.bytes);
        }
    }
    if (!already_pending) {
        uintptr_t pending = make_command(d, 0, QUANTIZE_PENDING, 0);
        submit_chain(d, pending);
        release(pending);
    }
    return 0;
}
static void native_quantize_cancel(void *self) {
    unsigned d = deck_for(self, 3);
    ++natives[d].cancels;
    if (get_int32_t(self, 0x98) != -1) store(self, 0x98, (int32_t) (11));
}
static bool native_preprocess(void *core, const void *frames) {
    (void) deck_for(core, 0);
    assert(frames == &frame);
    ++pre_forwards;
    if (process_throw) fixture_raise();
    owner_frame = true;
    return process_result;
}
struct Execution {
    Native *native;
    uintptr_t chain;
};
static void execution_drop(struct Execution *scope) {
    scope->native->executing = false;
    uint8_t inactive = 0;
    memcpy((void *) (scope->chain + 0x49), &inactive, sizeof inactive);
}
static void engine(unsigned d, uintptr_t original_chain) {
    __auto_type n = &(natives[d]);
    if (!original_chain || object_word(n->manager.bytes, 0) != original_chain) return;
    uintptr_t future = object_word(((void *) (original_chain)), 0x18);
    if (future && get_uint32_t(((void *) (future)), 0x94) == 2001 && !n->cycle_future) return;
    n->executing = true;
    store(((void *) (original_chain)), 0x49, (uint8_t) (1));
    struct Execution execution __attribute__((cleanup(execution_drop))) = {n, original_chain};
    uintptr_t node = object_word(((void *) (original_chain)), 0x18);
    while (node) {
        (void) run_command(node);
        node = object_word(((void *) (node)), 0x18);
    }
    store(((void *) (original_chain)), 0x49, (uint8_t) (0));
    n->executing = false;
    put(n->manager, 0, object_word(n->manager.bytes, 8));
    put(n->manager, 8, (uintptr_t) (0));
    release(original_chain);
}
static void advance_load(unsigned deck) {
    atomic_fetch_add_explicit(&(loads[deck]), 1, memory_order_seq_cst);
    azsdk_exit_load_changed(deck);
}
static void native_hot_countdown(void *beat) {
    __auto_type n = &(natives[deck_for(beat, 5)]);
    uint32_t count = get_uint32_t(beat, 0x104);
    if (count >= 1 && count <= 3) {
        store(beat, 0x104, count - 1);
        if (count == 1) put(n->play_pause, 8, (uint8_t) (n->cue_fault != 4));
    }
}
static void native_hot_countdown_task(void *task) {
    native_hot_countdown(((void *) (object_word(task, 0x10))));
}
static void native_process(void *core, const void *frames) {
    unsigned d = deck_for(core, 0);
    ++forwards;
    uintptr_t chain_at_entry = object_word(natives[d].manager.bytes, 0);
    uint32_t cue_countdown_at_entry = get_uint32_t(natives[d].beat.bytes, 0x104);
    if (!pre_hook(core, frames)) return;
    /* Keep destruction blocked after preProcess and its EXIT callback returned. */
    atomic_store_explicit(&(inside), true, memory_order_release);
    while (atomic_load_explicit(&(hold), memory_order_acquire)) sched_yield();
    if (natives[d].invalidate_after_pre) advance_load(d);
    if (natives[d].expire_after_pre) pause_ms(120);
    if (!natives[d].pause_engine) engine(d, chain_at_entry);
    if (cue_countdown_at_entry >= 1 && cue_countdown_at_entry <= 3 &&
        get_uint32_t(natives[d].beat.bytes, 0x104) == cue_countdown_at_entry) {
        if (!hot_countdown_hooks[0]) native_hot_countdown(natives[d].beat.bytes);
        else if (hot_countdown_route) {
            Raw_20 task = {0};
            put(task, 0x10, ((uintptr_t) natives[d].beat.bytes));
            hot_countdown_hooks[1](task.bytes);
        } else hot_countdown_hooks[0](natives[d].beat.bytes);
    }
    owner_frame = false; /* native task affinity ends before outer-frame settlement */
}
static bool affinity(void *unused) {
    (void) unused;
    return wrong_thread || !owner_frame;
}
static void native_action(void *facade, bool reloop) {
    assert(owner_frame && !wrong_thread);
    unsigned d = deck_for(facade, 1);
    __auto_type n = &(natives[d]);
    atomic_fetch_add_explicit(&(calls), 1, memory_order_seq_cst);
    n->reloop_effect = reloop;
    if (exit_throw) fixture_raise();
    if (!exit_noop && (!reloop || n->retained)) {
        if (!reloop && get_int32_t(n->quantize.bytes, 0x98) != -1) quantize_cancel_hook(n->quantize.bytes);
        /* Stock EXIT owns cancellation of its existing future wrap queue. */
        if (!reloop && n->maintained_loop) dispose_pending(d);
        if (n->deferred) {
            uintptr_t stop = make_command(d, 3, STOP_BACKGROUND, 0);
            assert(quantize_hook(n->quantize.bytes, 7, &stop, &frame, n->context.bytes + 0x160, 1.25) == 0);
            release(stop);
        } else if (reloop) {
            active(d);
            if (n->maintained_loop) maintain_loop(d);
        } else put(n->context, 0x190, (uint8_t) (0));
    }
    if (replace_load) advance_load(d);
}
static void native_exit(void *facade) { native_action(facade, false); }
static void native_control_jump(void *facade, int kind, int behavior, int slip) {
    assert(behavior == 0 && slip == 0);
    if (kind) native_cue_action(facade, AZ_CMD_HOT_TRIGGER, (unsigned) (kind));
    else native_action(facade, true);
}
static void native_backcue(void *facade, int kind) {
    assert(kind == 0);
    native_cue_action(facade, AZ_CMD_MAIN_RECALL, 0);
}
static void native_reloop(void *facade, int slip) { native_control_jump(facade, 0, 0, slip); }
/* Native-shaped Set facade: receives the borrowed subobject, not composite X0. */
static unsigned set_deck(void *receiver) {
    for (unsigned d = 0; d < 4; ++d)
        if (receiver == natives[d].facade.bytes + 0xc8) return d;
    assert(false);
    return 4;
}
static void point(void *property, unsigned offset, uint64_t samples) {
    __auto_type bytes = ((unsigned char *) (property));
    store(bytes, offset + 0x18, (uint8_t) (1));
    store(bytes, offset + 0x1c, (uint8_t) (1));
    store(bytes, offset + 0x1e, (uint8_t) (1));
    store(bytes, offset + 0x20, (uint32_t) (1));
    store(bytes, offset + 0x28, samples);
    store(bytes, offset + 0x38, (uint64_t) (1));
    store(bytes, offset + 0x48, (uint32_t) (48000));
}
static void loop_points(unsigned d, uint64_t in, uint64_t out) {
    __auto_type n = &(natives[d]);
    {
        void *range_values_4[] = {((void *) (n->context.bytes + 0x160)), ((void *) (n->retained_property.bytes))};
        for (unsigned range_index_4 = 0; range_index_4 < sizeof(range_values_4) / sizeof(range_values_4)[0]; ++range_index_4) {
            void *property = range_values_4[range_index_4];

            point(property, 0x18, in);
            point(property, 0x70, out);
            store(property, 0xc8, (uint32_t) (0));
            store(property, 0xd8, (uint32_t) (0));
            store(property, 0x58, ((uintptr_t) n->source_holder.bytes));
        }
    }
}
static void set_call(void *receiver) {
    assert(owner_frame && !wrong_thread);
    (void) set_deck(receiver);
    atomic_fetch_add_explicit(&(calls), 1, memory_order_seq_cst);
    if (exit_throw) fixture_raise();
}
static bool defer_set(unsigned d, unsigned operation, double ms, unsigned argument) {
    __auto_type n = &(natives[d]);
    if (!n->deferred || n->executing) return false;
    n->pending_operation = operation;
    n->pending_ms = ms;
    n->pending_argument = argument;
    uintptr_t callback = make_command(d, 3, operation == AZ_CMD_MAIN_RECALL ? 7 : 3, 1000);
    assert(quantize_hook(n->quantize.bytes, 7, &callback, &frame, n->context.bytes + 0x160, 1.25) == 0);
    release(callback);
    return true;
}
static void native_set_point(void *receiver, int kind, int quantize) {
    set_call(receiver);
    assert(kind == 0 && quantize == 1);
    if (exit_noop || defer_set(set_deck(receiver), AZ_CMD_LOOP_IN, 0, 0)) return;
    __auto_type n = &(natives[set_deck(receiver)]);
    point(n->retained_property.bytes, 0x18, 96000);
    if (n->cue_fault == 2) n->retained_property.bytes[0x30] ^= 2;
}
static void native_clear(void *receiver, int kind) {
    set_call(receiver);
    assert(kind >= 0 && kind <= 8);
    unsigned d = set_deck(receiver);
    if (!exit_noop && !defer_set(d, kind ? AZ_CMD_HOT_CLEAR : AZ_CMD_LOOP_CLEAR, 0, (unsigned) (kind))) {
        void *property = kind ? natives[d].hot_properties[kind - 1].bytes : natives[d].retained_property.bytes;
        memset(((unsigned char *) (property)) + 8, 0, 0xf8);
        if (kind && natives[d].cue_fault == 8) natives[d].play_pause.bytes[9] ^= 1;
    }
}
typedef azexit_native_color NativeColor;
static void native_set_hot(void *receiver, int kind, NativeColor color, int quantize) {
    set_call(receiver);
    assert(kind >= 1 && kind <= 8 && quantize == 1);
    unsigned d = set_deck(receiver);
    unsigned rgb = (unsigned) (color.r) << 16 | (unsigned) (color.g) << 8 | color.b;
    if (exit_noop || defer_set(d, AZ_CMD_HOT_SET, (double) (rgb), (unsigned) (kind))) return;
    __auto_type n = &(natives[d]);
    __auto_type property = &(n->hot_properties[kind - 1]);
    if (n->context.bytes[0x190]) {
        uintptr_t table = object_word(property->bytes, 0);
        memcpy(property->bytes, n->context.bytes + 0x160, sizeof property->bytes);
        put((*property), 0, table);
    } else {
        memset(property->bytes + 8, 0, sizeof property->bytes - 8);
        point(property->bytes, 0x18, get_uint64_t(n->core.bytes, 0x28));
        memcpy(property->bytes + 0x30, n->core.bytes + 0x18, 12);
    }
    put((*property), 0x10, color.r);
    put((*property), 0x11, color.g);
    put((*property), 0x12, color.b);
    if (n->cue_fault == 2) property->bytes[0x30] ^= 2;
    if (n->cue_fault == 6) property->bytes[0x11] ^= 2;
    if (n->cue_fault == 7) put((*property), 0x98, get_uint64_t(property->bytes, 0x98) + 1);
    if (n->cue_fault == 8) n->play_pause.bytes[9] ^= 1;
    if (n->cue_fault == 9) point(property->bytes, 0x70, 192000);
}
static void native_loop_out(void *receiver, int kind, int slip) {
    set_call(receiver);
    assert(kind == 0 && slip == 0);
    if (exit_noop || defer_set(set_deck(receiver), AZ_CMD_LOOP_OUT, 0, 0)) return;
    unsigned d = set_deck(receiver);
    point(natives[d].retained_property.bytes, 0x70, 192000);
    memcpy(natives[d].context.bytes + 0x160, natives[d].retained_property.bytes, 0xf8);
    active(d);
    if (natives[d].maintained_loop) {
        __auto_type n = &(natives[d]);
        if (n->preparatory_loop) {
            __auto_type clear = ((uint32_t (*)(void *))(jump_table[5]));
            assert(clear(n->jump.bytes) == 0);
            n->making_setup = true;
            maintain_loop(d);
            n->making_setup = false;
            assert(clear(n->jump.bytes) == 1);
            if (n->omit_future) return;
        }
        maintain_loop(d);
        if (natives[d].maintenance_fault == 6) {
            uintptr_t one_shot = make_command(d, 7, 0, 1);
            submit_chain(d, one_shot);
            release(one_shot);
            maintain_loop(d); /* ordinary one-shot Jump is replaced unexecuted */
        }
        if (natives[d].replace_future) maintain_loop(d);
    }
}
static void native_adjust_in(void *receiver, double ms, int minimum) {
    set_call(receiver);
    assert(minimum == 0);
    if (exit_noop || defer_set(set_deck(receiver), AZ_CMD_LOOP_ADJUST_IN, ms, 0)) return;
    __auto_type n = &(natives[set_deck(receiver)]);
    void *property = n->context.bytes + 0x160;
    __auto_type in = get_uint64_t(property, 0x40);
    point(property, 0x18, (uint64_t) ((int64_t) (in) + (int64_t) ((ms * 0.001) * 48000.0)));
    memcpy(n->retained_property.bytes + 8, n->context.bytes + 0x168, 0xf0);
    if (n->maintained_loop) maintain_loop(set_deck(receiver));
}
static void native_adjust_out(void *receiver, double ms, int type, int minimum) {
    set_call(receiver);
    assert(type == 1 && minimum == 0);
    if (exit_noop || defer_set(set_deck(receiver), AZ_CMD_LOOP_ADJUST_OUT, ms, 0)) return;
    __auto_type n = &(natives[set_deck(receiver)]);
    void *property = n->context.bytes + 0x160;
    __auto_type out = get_uint64_t(property, 0x98);
    point(property, 0x70, (uint64_t) ((int64_t) (out) + (int64_t) ((ms * 0.001) * 48000.0)));
    if (n->maintained_loop) maintain_loop(set_deck(receiver));
}
static void native_length_beat(void *receiver, uint32_t beat, int type, int slip) {
    set_call(receiver);
    assert(type == 1 && slip == 0 && (beat >> 16));
    if (exit_noop || defer_set(set_deck(receiver), AZ_CMD_LOOP_LENGTH, 0, beat)) return;
    __auto_type n = &(natives[set_deck(receiver)]);
    void *property = n->context.bytes + 0x160;
    __auto_type in = get_uint64_t(property, 0x40);
    point(property, 0x70, in + (uint64_t) (24000.0 * (beat & 0xffff) / (beat >> 16)));
    store(property, 0xc8, (uint32_t) (1));
    store(property, 0xd8, n->normalize_beat ? ((beat & 0xffff) * 2) | ((beat >> 16) * 2 << 16) : beat);
    /* Native beat-length application can synchronize the temporary kind0 cue
     * even though LoopType1 skips the wrapper's selected-cue copy branch. */
    memcpy(n->retained_property.bytes + 8, n->context.bytes + 0x168, 0xf0);
    if (n->maintained_loop) maintain_loop(set_deck(receiver));
}
static void native_length_square(void *receiver, int exponent, int type, int slip) {
    set_call(receiver);
    assert(type == 1 && slip == 0 && (exponent == -1 || exponent == 1));
    if (exit_noop || defer_set(set_deck(receiver), exponent == 1 ? AZ_CMD_LOOP_DOUBLE : AZ_CMD_LOOP_HALVE, 0, 0)) return;
    __auto_type n = &(natives[set_deck(receiver)]);
    void *property = n->context.bytes + 0x160;
    __auto_type in = get_uint64_t(property, 0x40);
    __auto_type out = get_uint64_t(property, 0x98);
    if (get_uint32_t(property, 0xc8) == 1) {
        uint32_t beat = get_uint32_t(property, 0xd8);
        uint32_t numerator = beat & 0xffff, denominator = beat >> 16;
        if (!n->wrong_beat)
            store(property, 0xd8, exponent == 1 ? (numerator * 2) | (denominator << 16) : numerator | ((denominator * 2) << 16));
        /* A tempo change makes doubled beats differ from doubled seconds. */
        point(property, 0x70, in + (exponent == 1 ? 3 * (out - in) / 2 : 3 * (out - in) / 4));
        memcpy(n->retained_property.bytes + 8, n->context.bytes + 0x168, 0xf0);
    } else point(property, 0x70, in + (exponent == 1 ? 2 * (out - in) : (out - in) / 2));
    if (n->maintained_loop) maintain_loop(set_deck(receiver));
}
static void native_position_effect(void *core, const void *position) {
    assert(owner_frame && !wrong_thread);
    __auto_type n = &(natives[deck_for(core, 0)]);
    ++n->cue_seeks;
    memcpy(n->core.bytes + 0x18, ((const unsigned char *) (position)) + 8, 12);
    put(n->core, 0x28, get_uint64_t(position, 0x18));
}
static void native_core_jump(void *core, const void *position) {
    ++natives[deck_for(core, 0)].cue_jump_calls;
    native_position_effect(core, position);
}
static void native_cue_action(void *facade, unsigned operation, unsigned kind) {
    assert(owner_frame && !wrong_thread && kind <= 8);
    unsigned d = deck_for(facade, 1);
    __auto_type n = &(natives[d]);
    atomic_fetch_add_explicit(&(calls), 1, memory_order_seq_cst);
    if (exit_noop || defer_set(d, operation, 0, kind)) return;
    void *property = kind ? ((void *) (n->hot_properties[kind - 1].bytes)) : n->retained_property.bytes;
    Raw_30 position = {0};
    memcpy(position.bytes + 8, ((unsigned char *) (property)) + 0x30, 12);
    put(position, 0x18, get_uint64_t(property, 0x40));
    if (n->cue_fault == 2) position.bytes[8] ^= 2;
    if (n->cue_fault == 3) put(position, 0x18, get_uint64_t(property, 0x40) + 1);
    if (operation == AZ_CMD_MAIN_RECALL) ++n->cue_seek_calls; /* native PlayPause's seek helper */
    if (n->cue_fault != 1 && !(operation == AZ_CMD_MAIN_RECALL && n->cue_seek_noop)) {
        if (operation == AZ_CMD_HOT_TRIGGER) {
            uintptr_t jump = make_command(d, 2, 0, 1001);
            store(((void *) (jump)), 0x20, ((uintptr_t) n->core.bytes));
            store(((void *) (jump)), 0x28, ((uintptr_t) (interval_source_table)));
            memcpy(((void *) (jump + 0x30)), position.bytes + 8, 12);
            uint64_t target = get_uint64_t(position.bytes, 0x18);
            store(((void *) (jump)), 0x40, target + n->cue_seek_window);
            store(((void *) (jump)), 0x48, target - n->cue_seek_window);
            (void) run_command(jump);
            release(jump);
        } else if (n->cue_fault == 5) native_core_jump(n->core.bytes, position.bytes); /* no causal seek receipt */
        else core_jump_hook(n->core.bytes, position.bytes);
    }
    if (operation == AZ_CMD_HOT_TRIGGER) put(n->beat, 0x104, (uint32_t) (n->cue_fault == 12 ? 4 : 3));
    else {
        put(n->play_pause, 8, (uint8_t) (n->cue_fault == 4));
        put(n->play_pause, 9, (uint8_t) (n->cue_fault == 11));
    }
    /* CDJ pause clears the logical flag without stopping the base audio core. */
    if (operation == AZ_CMD_HOT_TRIGGER) put(n->core, 9, (uint8_t) (1));
}
static void apply_set(unsigned d) {
    __auto_type n = &(natives[d]);
    void *receiver = n->facade.bytes + 0xc8;
    switch (n->pending_operation) {
    case AZ_CMD_LOOP_IN:
    case AZ_CMD_MAIN_SET: native_set_point(receiver, 0, 1); break;
    case AZ_CMD_MAIN_RECALL:
    case AZ_CMD_HOT_TRIGGER: native_cue_action(n->facade.bytes, n->pending_operation, n->pending_argument); break;
    case AZ_CMD_LOOP_OUT: native_loop_out(receiver, 0, 0); break;
    case AZ_CMD_HOT_SET: {
        unsigned rgb = (unsigned) (n->pending_ms);
        NativeColor color = {((unsigned char) (rgb >> 16)), ((unsigned char) (rgb >> 8)), ((unsigned char) (rgb))};
        native_set_hot(receiver, (int) (n->pending_argument), color, 1);
        break;
    }
    case AZ_CMD_HOT_CLEAR: native_clear(receiver, (int) (n->pending_argument)); break;
    case AZ_CMD_LOOP_CLEAR: native_clear(receiver, 0); break;
    case AZ_CMD_LOOP_DOUBLE: native_length_square(receiver, 1, 1, 0); break;
    case AZ_CMD_LOOP_HALVE: native_length_square(receiver, -1, 1, 0); break;
    case AZ_CMD_LOOP_LENGTH: native_length_beat(receiver, n->pending_argument, 1, 0); break;
    case AZ_CMD_LOOP_ADJUST_IN: native_adjust_in(receiver, n->pending_ms, 0); break;
    case AZ_CMD_LOOP_ADJUST_OUT: native_adjust_out(receiver, n->pending_ms, 1, 0); break;
    default: assert(false);
    }
}
static void *install(uintptr_t entry, void *replacement, void **original) {
    if (entry == ((uintptr_t) (native_process))) {
        *original = ((void *) (native_process));
        hook = ((Process) (replacement));
    } else if (entry == ((uintptr_t) (native_preprocess))) {
        *original = ((void *) (native_preprocess));
        pre_hook = ((Preprocess) (replacement));
    } else if (entry == ((uintptr_t) (native_manager_submit))) {
        *original = ((void *) (native_manager_submit));
        manager_hook = ((ManagerSubmit) (replacement));
    } else if (entry == ((uintptr_t) (native_quantize_submit))) {
        *original = ((void *) (native_quantize_submit));
        quantize_hook = ((QuantizeSubmit) (replacement));
    } else if (entry == ((uintptr_t) (native_hot_countdown))) {
        *original = ((void *) (native_hot_countdown));
        hot_countdown_hooks[0] = ((void (*)(void *))(replacement));
    } else if (entry == ((uintptr_t) (native_hot_countdown_task))) {
        *original = ((void *) (native_hot_countdown_task));
        hot_countdown_hooks[1] = ((void (*)(void *))(replacement));
    } else if (entry == ((uintptr_t) (native_core_jump))) {
        *original = ((void *) (native_core_jump));
        core_jump_hook = ((Process) (replacement));
    } else {
        assert(entry == ((uintptr_t) (native_quantize_cancel)));
        *original = ((void *) (native_quantize_cancel));
        quantize_cancel_hook = ((QuantizeCancel) (replacement));
    }
    return *original;
}
int azsdk_load_read(int deck, azsdk_load_sample *out) {
    uint64_t value = atomic_load_explicit(&(loads[deck]), memory_order_seq_cst);
    if (!value) return 0;
    *out = (azsdk_load_sample) {100u + (unsigned) (deck), value, value, 1, 1, 0};
    return 1;
}
static void entry(azexit_native_entry *out, uintptr_t address) {
    out->address = address;
    memcpy(out->prologue, ((void *) (address)), 16);
}
static void setup(int build, unsigned enabled) {
    quantize_cancel_hook = native_quantize_cancel;
    core_table[3] = ((uintptr_t) (native_process));
    facade_table[3] = ((uintptr_t) (native_exit));
    facade_table[4] = ((uintptr_t) (native_reloop));
    facade_table[6] = ((uintptr_t) (native_control_jump));
    facade_table[8] = ((uintptr_t) (native_backcue));
    box_table[5] = ((uintptr_t) (affinity));
    beat_table[15] = beat_table[16] = ((uintptr_t) (native_exit));
    jump_table[2] = ((uintptr_t) (native_jump));
    jump_table[4] = ((uintptr_t) (native_schedule));
    jump_table[5] = ((uintptr_t) (native_jump_cancel));
    chain_table[0] = chain_table[1] = ((uintptr_t) (chain_destroy));
    azexit_profile p = {0};
    p.unit = ((uintptr_t) (unit_table));
    p.core = ((uintptr_t) (core_table));
    p.facade = ((uintptr_t) (facade_table));
    p.beat = ((uintptr_t) (beat_table));
    p.quantize = ((uintptr_t) (quantize_table));
    p.slip = ((uintptr_t) (slip_table));
    p.box = ((uintptr_t) (box_table));
    p.process = ((uintptr_t) (native_process));
    p.preprocess = ((uintptr_t) (native_preprocess));
    p.exit = ((uintptr_t) (native_exit));
    p.affinity = ((uintptr_t) (affinity));
    p.reloop = facade_table[4];
    p.control_jump = facade_table[6];
    set_table[2] = p.set_point = ((uintptr_t) (native_set_point));
    set_table[5] = p.clear_cue = ((uintptr_t) (native_clear));
    set_table[9] = p.loop_out = ((uintptr_t) (native_loop_out));
    set_table[14] = p.adjust_in = ((uintptr_t) (native_adjust_in));
    set_table[15] = p.adjust_out = ((uintptr_t) (native_adjust_out));
    set_table[16] = p.length_beat = ((uintptr_t) (native_length_beat));
    set_table[17] = p.length_square = ((uintptr_t) (native_length_square));
    set_table[4] = p.set_hot = ((uintptr_t) (native_set_hot));
    p.set_table = ((uintptr_t) (set_table));
    p.cue_table = ((uintptr_t) (cue_table));
    p.cue_base_table = ((uintptr_t) (cue_base_table));
    p.control_backcue = facade_table[8];
    entry(&(p.core_jump), ((uintptr_t) (native_core_jump)));
    entry(&(p.hot_countdown[0]), ((uintptr_t) (native_hot_countdown)));
    entry(&(p.hot_countdown[1]), ((uintptr_t) (native_hot_countdown_task)));
    hot_countdown_route = (unsigned) (build - 1);
    p.play_pause_table = ((uintptr_t) (play_pause_table));
    merger_table[2] = p.merger_request = ((uintptr_t) (native_merge));
    p.merger_table = ((uintptr_t) (merger_table));
    memcpy(p.prologue, ((void *) (native_preprocess)), 16);
    memcpy(p.process_prologue, ((void *) (native_process)), 16);
    entry(&(p.manager_submit), ((uintptr_t) (native_manager_submit)));
    entry(&(p.quantize_submit), ((uintptr_t) (native_quantize_submit)));
    entry(&(p.quantize_cancel), ((uintptr_t) (native_quantize_cancel)));
    p.chain_table = ((uintptr_t) (chain_table));
    p.chain_destroy = p.chain_deleting = chain_table[0];
    p.jump_table = ((uintptr_t) (jump_table));
    p.jump_request = jump_table[2];
    p.jump_schedule = jump_table[4];
    p.jump_cancel = jump_table[5];
    p.interval_table = ((uintptr_t) (interval_table));
    p.interval_source_table = ((uintptr_t) (interval_source_table));
    for (unsigned i = 0; i < AZEXIT_COMMAND_TYPES; ++i) {
        command_tables[i][0] = ((uintptr_t) (command_destroy));
        command_tables[i][1] = ((uintptr_t) (command_delete));
        command_tables[i][2] = ((uintptr_t) (command_execute));
        p.commands[i] = (struct azexit_command_profile) {((uintptr_t) (command_tables[i])), command_tables[i][0], command_tables[i][1], command_tables[i][2]};
    }
    p.prologue[0] ^= 1;
    azsdk_exit_test_profile(&p);
    assert(!azsdk_exit_configure(1, build, install));
    assert(!azstem_exit_service_v1()->mask());
    p.prologue[0] ^= 1;
    uintptr_t process_entry = p.process;
    p.process = 0;
    azsdk_exit_test_profile(&p);
    assert(!azsdk_exit_configure(1, build, install));
    p.process = process_entry;
    uintptr_t reloop_entry = p.reloop;
    p.reloop = p.exit;
    azsdk_exit_test_profile(&p);
    assert(!azsdk_exit_configure((int) (enabled), build, install));
    p.reloop = reloop_entry;
    azsdk_exit_test_profile(&p);
    assert(!azsdk_exit_configure(1, 0, install));
    assert(!azsdk_exit_configure(16, build, install));
    assert(azsdk_exit_configure((int) (enabled), build, install));
    for (unsigned i = 0; i < AZEXIT_COMMAND_TYPES; ++i) {
        assert(command_tables[i][0] != p.commands[i].destroy);
        assert(command_tables[i][1] != p.commands[i].deleting);
        assert(command_tables[i][2] != p.commands[i].execute);
    }
    assert(jump_table[2] != p.jump_request);
    for (unsigned d = 0; d < 4; d++) {
        __auto_type n = &(natives[d]);
        put(n->unit, 0, p.unit);
        put(n->unit, 0x128, ((uintptr_t) n->core.bytes));
        put(n->unit, 0x180, ((uintptr_t) n->basis.bytes));
        put(n->basis, 0x30, ((uintptr_t) n->core.bytes));
        put(n->basis, 0x38, ((uintptr_t) n->jump.bytes));
        put(n->jump, 0, p.jump_table);
        put(n->jump, 0x10, ((uintptr_t) n->core.bytes));
        put(n->quantize, 0x20, ((uintptr_t) n->jump.bytes));
        put(n->quantize, 0x40, ((uintptr_t) n->quantize.bytes));
        put(n->beat, 0x38, ((uintptr_t) n->basis.bytes));
        put(n->beat, 0x48, ((uintptr_t) n->core.bytes));
        put(n->beat, 0x70, ((uintptr_t) n->context.bytes));
        put(n->beat, 0x80, ((uintptr_t) n->quantize.bytes));
        put(n->unit, 0x188, ((uintptr_t) n->functions.bytes));
        put(n->unit, 0x190, ((uintptr_t) n->bundle.bytes));
        put(n->core, 0, p.core);
        put(n->core, 0x110, p.box);
        put(n->core, 0xa0, ((uintptr_t) n->manager.bytes));
        put(n->merger, 0, p.merger_table);
        put(n->manager, 0x10, ((uintptr_t) n->merger.bytes));
        put(n->bundle, 0x40, ((uintptr_t) n->facade.bytes));
        put(n->functions, 0x20, ((uintptr_t) n->quantize.bytes));
        put(n->functions, 0x30, ((uintptr_t) n->slip.bytes));
        put(n->facade, 0, p.facade);
        put(n->facade, 0xc8, p.set_table);
        put(n->facade, 0xc8 + 0x10, ((uintptr_t) n->core.bytes) + 0x110);
        put(n->facade, 0xc8 + 0x20, ((uintptr_t) n->available.bytes));
        put(n->facade, 0xc8 + 0x48, ((uintptr_t) n->quantize.bytes));
        put(n->facade, 0xc8 + 0x60, ((uintptr_t) n->context.bytes));
        put(n->context, 0x18, ((uintptr_t) n->retained_property.bytes));
        put(n->retained_property, 0, p.cue_table);
        for (unsigned kind = 1; kind <= 8; ++kind) {
            put(n->context, 0x18 + kind * 8, ((uintptr_t) n->hot_properties[kind - 1].bytes));
            put(n->hot_properties[kind - 1], 0, p.cue_base_table);
            point(n->hot_properties[kind - 1].bytes, 0x18, 48000 * (kind + 1));
        }
        loop_points(d, 48000, 144000);
        put(n->context, 0x190, (uint8_t) (0));
        put(n->facade, 0x18, ((uintptr_t) n->core.bytes) + 0x110);
        put(n->facade, 0x40, ((uintptr_t) n->core.bytes));
        put(n->facade, 0x28, ((uintptr_t) n->available.bytes));
        put(n->available, 8, (uint8_t) (1));
        put(n->facade, 0x50, ((uintptr_t) n->quantize.bytes));
        put(n->facade, 0xa8, ((uintptr_t) n->slip.bytes));
        put(n->facade, 0x70, ((uintptr_t) n->beat.bytes));
        put(n->facade, 0x68, ((uintptr_t) n->context.bytes));
        put(n->facade, 0x58, ((uintptr_t) n->play_pause.bytes));
        put(n->facade, 0x78, ((uintptr_t) n->cue_functions.bytes));
        put(n->play_pause, 0, p.play_pause_table);
        put(n->play_pause, 0x58, ((uintptr_t) n->core.bytes));
        put(n->cue_functions, 0, ((uintptr_t) n->core.bytes));
        put(n->cue_functions, 8, ((uintptr_t) n->context.bytes));
        put(n->cue_functions, 0xa8, ((uintptr_t) n->play_pause.bytes));
        put(n->beat, 0, p.beat);
        put(n->quantize, 0, p.quantize);
        put(n->slip, 0, p.slip);
        put(n->slip, 0x78, ((uintptr_t) n->quantize.bytes));
        put(n->quantize, 0x98, (int32_t) (-1));
        atomic_store_explicit(&(loads[d]), 200 + d, memory_order_seq_cst);
        assert(azsdk_exit_register((int) (d), n->unit.bytes));
        assert(!azsdk_exit_register((int) (d), n->unit.bytes));
    }
    assert(!azsdk_exit_register(4, natives[0].unit.bytes));
    sdk_exit_public_bind(azstem_exit_service_v1());
    uint64_t expected_commands = (enabled & AZEXIT_ENABLE_EXIT ? UINT64_C(1) << AZ_CMD_LOOP_EXIT : 0) |
                                 (enabled & AZEXIT_ENABLE_RELOOP ? UINT64_C(1) << AZ_CMD_RELOOP : 0);
    if (enabled & AZEXIT_ENABLE_LOOP)
        expected_commands |= ((UINT64_C(1) << (AZ_CMD_LOOP_ADJUST_OUT + 1)) - 2) &
                             ~((UINT64_C(1) << AZ_CMD_LOOP_EXIT) | (UINT64_C(1) << AZ_CMD_RELOOP));
    if (enabled & AZEXIT_ENABLE_CUE) expected_commands |= (UINT64_C(1) << AZ_CMD_MAIN_SET) |
                                                          (UINT64_C(1) << AZ_CMD_MAIN_RECALL) | (UINT64_C(1) << AZ_CMD_HOT_TRIGGER);
    assert(sdk_exit_public_capabilities() == expected_commands);
}
static void active(unsigned d) {
    __auto_type n = &(natives[d]);
    put(n->context, 0x190, (uint8_t) (1));
    put(n->context, 0x194, (uint8_t) (1));
    put(n->context, 0x196, (uint8_t) (1));
    put(n->context, 0x198, (uint32_t) (1));
}
static void process(unsigned d) {
    owner_frame = false;
    unsigned before = forwards, pre_before = pre_forwards;
    hook(natives[d].core.bytes, &frame);
    assert(forwards == before + 1 && pre_forwards == pre_before + 1);
    owner_frame = false;
}
static uint64_t next_id;
static az_command_request request(unsigned deck) {
    az_command_request r = {0};
    r.size = sizeof r;
    r.version = 1;
    r.operation = AZ_CMD_LOOP_EXIT;
    r.deck = deck;
    r.request_id = ++next_id;
    r.expected_load_generation = atomic_load_explicit(&(loads[deck]), memory_order_seq_cst);
    r.deadline_ns = UINT64_MAX;
    r.flags = AZ_COMMAND_NATIVE_LOAD;
    return r;
}
static az_command_result result() {
    az_command_result r = {0};
    r.size = sizeof r;
    return r;
}
static void terminal(const az_command_request r, unsigned outcome, int reason) {
    __auto_type out = result();
    int rc = sdk_exit_public_poll("one", r.request_id, &out);
    if (rc != AZ_OK || out.outcome != outcome || out.reason != reason)
        fprintf(stderr, "operation %u deck %u: got status %d outcome %u reason %d; expected %u/%d\n",
                r.operation, r.deck, rc, out.outcome, out.reason, outcome, reason);
    assert(rc == AZ_OK && out.outcome == outcome && out.reason == reason);
    assert((out.revision != 0) == (outcome == AZ_COMMAND_APPLIED));
    __auto_type saved = out;
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && !memcmp(&saved, &out, sizeof out));
    if (outcome == AZ_COMMAND_APPLIED)
        assert(sdk_exit_public_cancel("one", r.request_id, &out) == AZ_STALE && !memcmp(&saved, &out, sizeof out));
    int released = sdk_exit_public_release("one", r.request_id);
    if (released != AZ_OK)
        fprintf(stderr, "release status %d for operation %u, deck %u, terminal %u/%d\n",
                released, r.operation, r.deck, outcome, reason);
    assert(released == AZ_OK);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_STALE);
}
static void fenced(const az_command_request r, int reason) {
    __auto_type out = result();
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK &&
           out.outcome == AZ_COMMAND_UNCERTAIN && out.reason == reason);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_BUSY);
}
static void pending(const az_command_request r) {
    __auto_type out = result();
    int status = sdk_exit_public_poll("one", r.request_id, &out);
    assert(status == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED);
}
static void process_completion(void *opaque) { process(*(unsigned *) opaque); }
static void complete(az_command_request r, unsigned outcome, int reason) {
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED);
    if (exit_throw) assert(!fixture_native_try(process_completion, &r.deck));
    process(r.deck);
    terminal(r, outcome, reason);
}
static void happy() {
    sdk_exit_public_auth();
    for (unsigned d = 0; d < 4; d++) {
        active(d);
        complete(request(d), AZ_COMMAND_APPLIED, AZ_OK);
        assert(natives[d].context.bytes[0x190] == 0);
    }
    complete(request(0), AZ_COMMAND_APPLIED, AZ_OK);
    __auto_type r = request(0);
    __auto_type out = result();
    r.flags = 0;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_UNSUPPORTED);
    r.flags = AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_PERSIST;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID);
    r.flags = AZ_COMMAND_NATIVE_LOAD;
    r.quantization = AZ_QUANTIZE_ON;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID);
    r.quantization = 0;
    r.expected_revision = 1;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID);
    r.expected_revision = 0;
    r.operation = AZ_CMD_LOOP_ENTER;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_UNSUPPORTED);
    r.operation = AZ_CMD_LOOP_EXIT;
    r.flags = 4;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID);
}
static void asynchronous(bool adopt_existing) {
    for (unsigned d = 0; d < 4; ++d) {
        __auto_type n = &(natives[d]);
        active(d);
        n->deferred = true;
        put(n->beat, 0xbc, (uint8_t) (1));
        put(n->slip, 0x68, (uint8_t) (1));
        if (adopt_existing) {
            put(n->quantize, 0x98, (int32_t) (2));
            uintptr_t old_pending = make_command(d, 0, QUANTIZE_PENDING, 0);
            submit_chain(d, old_pending); /* native work predates SDK request */
            release(old_pending);
        }
        __auto_type r = request(d);
        __auto_type out = result();
        unsigned effects = n->effects, jumps = n->jumps;
        assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED);
        pending(r);
        process(d);
        pending(r);
        if (!adopt_existing) {
            assert(n->effects == effects && n->context.bytes[0x190] == 1);
            process(d);
            pending(r);
        }
        assert(n->effects == effects && n->context.bytes[0x190] == 1);
        assert(object_word(n->manager.bytes, 0));
        process(d);
        assert(n->effects == effects + 1 && n->jumps == jumps + 1 && n->context.bytes[0x190] == 0);
        terminal(r, AZ_COMMAND_APPLIED, AZ_OK);
        assert(!object_word(n->manager.bytes, 0) && !object_word(n->manager.bytes, 8) && get_int32_t(n->quantize.bytes, 0x98) == -1);
        n->deferred = false;
    }
}
static void retained_after_execution() {
    __auto_type n = &(natives[0]);
    active(0);
    n->deferred = true;
    __auto_type r = request(0);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    process(0);
    uintptr_t chain = object_word(n->manager.bytes, 0);
    uintptr_t retained_command = object_word(((void *) (chain)), 0x18);
    retain(retained_command); /* a native callback holds the executed allocation */
    process(0);
    assert(!n->context.bytes[0x190]);
    assert(!object_word(n->manager.bytes, 0) && !object_word(n->manager.bytes, 8) &&
           get_int32_t(n->quantize.bytes, 0x98) == -1 && !object_word(n->quantize.bytes, 0xf8));
    pending(r); /* execute is insufficient while native references still exist */
    release(retained_command);
    process(0);
    terminal(r, AZ_COMMAND_APPLIED, AZ_OK);
    n->deferred = false;
}
static void dispose_pending(unsigned d) {
    __auto_type n = &(natives[d]);
    uintptr_t current = object_word(n->manager.bytes, 0), later = object_word(n->manager.bytes, 8), callback = object_word(n->quantize.bytes, 0xf8);
    uintptr_t hidden = object_word(n->quantize.bytes, 0x30), extra = object_word(n->quantize.bytes, 0xe0);
    put(n->manager, 0, (uintptr_t) (0));
    put(n->manager, 8, (uintptr_t) (0));
    put(n->quantize, 0xf8, (uintptr_t) (0));
    put(n->quantize, 0x98, (int32_t) (-1));
    put(n->quantize, 0x30, (uintptr_t) (0));
    put(n->quantize, 0x28, (uint32_t) (-1));
    put(n->quantize, 0xe0, (uintptr_t) (0));
    release(hidden);
    release(extra);
    release(current);
    release(later);
    release(callback);
}
static uintptr_t existing_quantize_pending(unsigned d) {
    __auto_type n = &(natives[d]);
    put(n->quantize, 0x98, (int32_t) (2));
    uintptr_t pending = make_command(d, 0, QUANTIZE_PENDING, 0);
    submit_chain(d, pending);
    release(pending);
    return pending;
}
static void borrowed_pending_repurposed() {
    __auto_type n = &(natives[0]);
    active(0);
    n->deferred = true;
    n->pause_engine = true;
    uintptr_t infrastructure = existing_quantize_pending(0);
    __auto_type r = request(0);
    __auto_type out = result();
    unsigned cancels = n->cancels;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    pending(r);
    assert(n->cancels == cancels + 1);
    uintptr_t old_callback = object_word(n->quantize.bytes, 0xf8);
    retain(old_callback); /* a native owner keeps the old EXIT callback alive */
    advance_load(0);
    process(0);
    fenced(r, AZ_STALE);
    assert(object_word(((void *) (object_word(n->manager.bytes, 0))), 0x18) == infrastructure);
    unsigned effects = n->effects, executes = n->command_executes;
    uintptr_t replacement = make_command(0, 3, STOP_BACKGROUND, 0);
    owner_frame = true;
    assert(quantize_hook(n->quantize.bytes, 7, &replacement, &frame, n->context.bytes + 0x160, 1.25) == 0);
    owner_frame = false;
    release(replacement);
    assert(object_word(((void *) (object_word(n->manager.bytes, 0))), 0x18) == infrastructure);
    n->pause_engine = false;
    process(0); /* repurposed native infrastructure must run without the old SDK token */
    assert(n->command_executes == executes + 2 && n->effects == effects);
    process(0);
    assert(n->command_executes == executes + 4 && n->effects == effects + 1 && n->context.bytes[0x190] == 0);
    owner_frame = true;
    assert(!run_command(old_callback)); /* the old effect callback keeps its tombstone */
    owner_frame = false;
    assert(n->command_executes == executes + 4 && n->effects == effects + 1);
    release(old_callback);
    process(0);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_STALE);
    n->deferred = false;
}
static void borrowed_pending_replaced() {
    __auto_type n = &(natives[0]);
    active(0);
    n->pause_engine = true;
    (void) existing_quantize_pending(0);
    uintptr_t old_native_callback = make_command(0, 7, 4, 100);
    put(n->quantize, 0xf8, old_native_callback); /* native Quantize already retains work */
    __auto_type r = request(0);
    __auto_type out = result();
    unsigned cancels = n->cancels, executed = n->command_executes;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    pending(r);
    assert(n->cancels == cancels + 1 && get_int32_t(n->quantize.bytes, 0x98) == 11);
    assert(n->context.bytes[0x190] == 0 && n->command_executes == executed);
    dispose_pending(0); /* cancel adopted this infrastructure; losing it matters */
    n->pause_engine = false;
    process(0);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
}
static void noop_preserves_borrowed_work() {
    __auto_type n = &(natives[0]);
    active(0);
    n->pause_engine = true;
    (void) existing_quantize_pending(0);
    exit_noop = true;
    __auto_type r = request(0);
    __auto_type out = result();
    unsigned cancels = n->cancels, executed = n->command_executes;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    pending(r);
    assert(n->cancels == cancels && n->command_executes == executed);
    exit_noop = false;
    n->pause_engine = false;
    process(0);
    assert(n->cancels == cancels && n->command_executes == executed + 1 && n->context.bytes[0x190] == 1);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
}
static void replacement_and_incarnation() {
    __auto_type n = &(natives[0]);
    active(0);
    n->deferred = true;
    __auto_type r = request(0);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    dispose_pending(0); /* merger discards unexecuted request chain */
    put(n->context, 0x190, (uint8_t) (0)); /* unrelated state must not manufacture APPLIED */
    process(0);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
    active(0);
    r = request(0);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    advance_load(0);
    unsigned effects = n->effects, executed = n->command_executes;
    process(0); /* old QPending must not touch the replacement track */
    assert(n->effects == effects && n->command_executes == executed && n->context.bytes[0x190] == 1);
    fenced(r, AZ_STALE);
    /* A separately held old callback stays suppressed after public result release. */
    uintptr_t old_callback = object_word(n->quantize.bytes, 0xf8);
    owner_frame = true;
    assert(!run_command(old_callback));
    owner_frame = false;
    assert(n->effects == effects && n->command_executes == executed);
    dispose_pending(0);
    process(0);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_STALE);
    uintptr_t same_address = make_command(0, 3, 4, 100);
    assert(same_address == old_callback);
    owner_frame = true;
    assert(run_command(same_address));
    owner_frame = false;
    assert(n->effects == effects + 1 && n->command_executes == executed + 1);
    release(same_address);
    n->deferred = false;
}
static void process_zero(void *unused) {
    (void) unused;
    process(0);
}
static void *audio_worker(void *unused) {
    (void) unused;
    process(0);
    return NULL;
}
static void *retirement_worker(void *unused) {
    (void) unused;
    azsdk_exit_retire(0);
    atomic_store_explicit(&retired, true, memory_order_release);
    return NULL;
}
static void child_exception() {
    __auto_type n = &(natives[0]);
    active(0);
    n->deferred = true;
    __auto_type r = request(0);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    process(0);
    command_throw_role = SLIP_STATE + 1;
    assert(!fixture_native_try(process_zero, NULL));
    command_throw_role = 0;
    owner_frame = false;
    assert(n->context.bytes[0x190] == 1);
    pending(r);
    dispose_pending(0);
    process(0);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
    n->deferred = false;
}
static void delayed_deadline() {
    __auto_type n = &(natives[0]);
    active(0);
    n->deferred = true;
    struct timespec clock;
    assert(!clock_gettime(CLOCK_MONOTONIC, &clock));
    uint64_t time = (uint64_t) (clock.tv_sec) * UINT64_C(1000000000) + (uint64_t) (clock.tv_nsec);
    __auto_type r = request(0);
    r.deadline_ns = time + UINT64_C(50000000);
    __auto_type out = result();
    unsigned executed = n->command_executes, effects = n->effects;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    pending(r);
    uintptr_t old_callback = object_word(n->quantize.bytes, 0xf8);
    pause_ms(75);
    process(0);
    assert(n->command_executes == executed && n->effects == effects && n->context.bytes[0x190] == 1);
    fenced(r, AZ_STALE);
    owner_frame = true;
    assert(!run_command(old_callback));
    owner_frame = false;
    assert(n->command_executes == executed && n->effects == effects);
    dispose_pending(0);
    process(0);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_STALE);
    n->deferred = false;
}
static void loss_interference() {
    __auto_type n = &(natives[0]);
    active(0);
    n->deferred = true;
    __auto_type older = request(0);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &older, &out) == AZ_OK);
    process(0);
    uintptr_t old_chain = object_word(n->manager.bytes, 0), old_callback = object_word(n->quantize.bytes, 0xf8);
    /* Transfer native owned references to a dormant holder; keep both command
     * incarnations alive across the old SDK result and the next request. */
    put(n->manager, 0, (uintptr_t) (0));
    put(n->quantize, 0xf8, (uintptr_t) (0));
    put(n->quantize, 0x98, (int32_t) (-1));
    advance_load(0);
    process(0);
    fenced(older, AZ_STALE);
    __auto_type current = request(0);
    assert(sdk_exit_public_submit("one", &current, &out) == AZ_OK);
    process(0);
    pending(current);
    release(old_chain);
    release(old_callback);
    process(0);
    terminal(older, AZ_COMMAND_UNCERTAIN, AZ_STALE);
    process(0);
    dispose_pending(0); /* the new token's actual chain is discarded independently */
    put(n->context, 0x190, (uint8_t) (0));
    process(0);
    terminal(current, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
    n->deferred = false;
}
static void unrelated_command_passthrough() {
    __auto_type n = &(natives[0]);
    active(0);
    n->deferred = true;
    __auto_type r = request(0);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    unsigned executed = n->command_executes, effects = n->effects, destroyed = n->destroyed;
    for (unsigned type = 0; type < AZEXIT_COMMAND_TYPES; ++type) {
        uintptr_t independent = make_command(0, 7, type, 100);
        owner_frame = true;
        assert(run_command(independent));
        owner_frame = false;
        if (type % 2) {
            store(((void *) (independent)), 8, (uint32_t) (0));
            ((void (*)(void *))(command_tables[type][1]))(((void *) (independent)));
        } else release(independent);
    }
    assert(n->command_executes == executed + AZEXIT_COMMAND_TYPES && n->effects == effects + AZEXIT_COMMAND_TYPES &&
           n->destroyed == destroyed + AZEXIT_COMMAND_TYPES && n->context.bytes[0x190] == 1);
    pending(r);
    process(0);
    pending(r);
    process(0);
    terminal(r, AZ_COMMAND_APPLIED, AZ_OK);
    n->deferred = false;
}
static void fifo_owners() {
    __auto_type n = &(natives[0]);
    active(0);
    n->deferred = true;
    __auto_type first = request(0);
    __auto_type second = request(0);
    __auto_type out = result();
    __auto_type queued = result();
    unsigned exits = atomic_load_explicit(&(calls), memory_order_seq_cst);
    assert(sdk_exit_public_submit("one", &first, &out) == AZ_OK);
    assert(sdk_exit_public_submit("two", &second, &queued) == AZ_OK && queued.outcome == AZ_COMMAND_ACCEPTED);
    process(0);
    pending(first);
    assert(sdk_exit_public_poll("two", second.request_id, &queued) == AZ_OK && queued.outcome == AZ_COMMAND_ACCEPTED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == exits + 1);
    process(0);
    pending(first);
    process(0);
    assert(sdk_exit_public_poll("one", first.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED);
    assert(sdk_exit_public_poll("two", second.request_id, &queued) == AZ_OK && queued.outcome == AZ_COMMAND_ACCEPTED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == exits + 1);
    active(0);
    process(0);
    process(0);
    assert(sdk_exit_public_poll("two", second.request_id, &queued) == AZ_OK && queued.outcome == AZ_COMMAND_ACCEPTED);
    process(0);
    assert(sdk_exit_public_poll("two", second.request_id, &queued) == AZ_OK && queued.outcome == AZ_COMMAND_APPLIED && queued.revision > out.revision);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == exits + 2);
    assert(sdk_exit_public_release("one", first.request_id) == AZ_OK);
    assert(sdk_exit_public_release("two", second.request_id) == AZ_OK);
    n->deferred = false;
}
static void guards() {
    const azexit_service *api = azstem_exit_service_v1();
    azexit_request expired = {UINT64_MAX, atomic_load_explicit(&(loads[0]), memory_order_seq_cst), 1, 0, 0};
    unsigned no_call = atomic_load_explicit(&(calls), memory_order_seq_cst);
    assert(api->submit(&expired) == AZ_PENDING);
    process(0);
    azexit_result expired_result = {0};
    assert(api->poll(0, &expired_result) == AZ_OK && expired_result.outcome == AZ_COMMAND_CANCELLED && expired_result.reason == AZ_STALE);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == no_call && api->release(0, UINT64_MAX) == AZ_OK);
    for (unsigned d = 0; d < 4; d++) {
        unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
        __auto_type n = &(natives[d]);
        wrong_thread = true;
        complete(request(d), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        wrong_thread = false;
        put(n->beat, 0x80, ((uintptr_t) natives[(d + 1) % 4].quantize.bytes));
        complete(request(d), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        put(n->beat, 0x80, ((uintptr_t) n->quantize.bytes));
        put(n->beat, 0x70, ((uintptr_t) natives[(d + 1) % 4].context.bytes));
        complete(request(d), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        put(n->beat, 0x70, ((uintptr_t) n->context.bytes));
        put(n->beat, 0x38, ((uintptr_t) natives[(d + 1) % 4].basis.bytes));
        complete(request(d), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        put(n->beat, 0x38, ((uintptr_t) n->basis.bytes));
        put(n->beat, 0x48, ((uintptr_t) natives[(d + 1) % 4].core.bytes));
        complete(request(d), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        put(n->beat, 0x48, ((uintptr_t) n->core.bytes));
        put(n->quantize, 0x20, ((uintptr_t) natives[(d + 1) % 4].jump.bytes));
        complete(request(d), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        put(n->quantize, 0x20, ((uintptr_t) n->jump.bytes));
        put(n->manager, 0x10, (uintptr_t) (1));
        complete(request(d), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        put(n->manager, 0x10, (uintptr_t) (0));
        assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
        complete(request(d), AZ_COMMAND_APPLIED, AZ_OK);
        put(n->manager, 0x10, ((uintptr_t) n->merger.bytes));
        uintptr_t canonical = merger_table[2];
        merger_table[2] = 0;
        complete(request(d), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        merger_table[2] = canonical;
        put(n->merger, 0, ((uintptr_t) (jump_table)));
        complete(request(d), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        put(n->merger, 0, ((uintptr_t) (merger_table)));
        complete(request(d), AZ_COMMAND_APPLIED, AZ_OK);
    }
    active(0);
    exit_noop = true;
    complete(request(0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
    exit_noop = false;
    exit_throw = true;
    complete(request(0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
    exit_throw = false;
    replace_load = true;
    complete(request(0), AZ_COMMAND_UNCERTAIN, AZ_STALE);
    replace_load = false;
}
static void dormant_retirement() {
    __auto_type n = &(natives[0]);
    active(0);
    n->deferred = true;
    __auto_type r = request(0);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    uintptr_t old_callback = object_word(n->quantize.bytes, 0xf8);
    unsigned executed = n->command_executes;
    azsdk_exit_retire(0); /* retirement suppresses work but does not settle a live allocation */
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_UNCERTAIN);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_BUSY);
    owner_frame = true;
    assert(!run_command(old_callback));
    owner_frame = false;
    assert(n->command_executes == executed && n->context.bytes[0x190] == 1);
    dispose_pending(0);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_STALE);
    assert(azsdk_exit_register(0, n->unit.bytes));
    n->deferred = false;
}
static void lifecycle() {
    /* Slow public polling cannot invalidate a receipt already confirmed native. */
    struct timespec clock;
    assert(!clock_gettime(CLOCK_MONOTONIC, &clock));
    uint64_t time = (uint64_t) (clock.tv_sec) * UINT64_C(1000000000) + (uint64_t) (clock.tv_nsec);
    __auto_type late = request(0);
    late.deadline_ns = time + UINT64_C(5000000000);
    sdk_exit_public_clock(time);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &late, &out) == AZ_OK);
    process(0);
    sdk_exit_public_clock(late.deadline_ns + 1);
    assert(sdk_exit_public_poll("one", late.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED);
    sdk_exit_public_clock(0);
    assert(sdk_exit_public_release("one", late.request_id) == AZ_OK);
    __auto_type r = request(0);
    unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    assert(sdk_exit_public_cancel("two", r.request_id, &out) == AZ_STALE);
    assert(sdk_exit_public_cancel("one", r.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_CANCELLED);
    process(0);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    r = request(0);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    advance_load(0);
    process(0);
    terminal(r, AZ_COMMAND_REJECTED, AZ_STALE);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    r = request(0);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process_result = false;
    process(0);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    process_result = true;
    process_throw = true;
    assert(!fixture_native_try(process_zero, NULL));
    process_throw = false;
    assert(sdk_exit_public_cancel("one", r.request_id, &out) == AZ_OK);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
    r = request(0);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    azsdk_exit_retire(0);
    process(0);
    terminal(r, AZ_COMMAND_REJECTED, AZ_STALE);
    assert(azsdk_exit_register(0, natives[0].unit.bytes));
    r = request(0);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    sdk_exit_public_retire(0);
    process(0);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    (void) sdk_exit_public_poll("one", r.request_id, &out);
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_STALE);
    active(0);
    r = request(0);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    hold = true;
    inside = false;
    retired = false;
    pthread_t audio;
    assert(!pthread_create(&audio, NULL, audio_worker, NULL));
    while (!atomic_load_explicit(&(inside), memory_order_acquire)) sched_yield();
    unsigned exits_before_retirement = atomic_load_explicit(&(calls), memory_order_seq_cst);
    pthread_t destroy;
    assert(!pthread_create(&destroy, NULL, retirement_worker, NULL));
    while (azstem_exit_service_v1()->mask() & 1) sched_yield();
    assert(!atomic_load_explicit(&(retired), memory_order_acquire) && atomic_load_explicit(&(calls), memory_order_seq_cst) == exits_before_retirement);
    hold = false;
    assert(!pthread_join(audio, NULL));
    assert(!pthread_join(destroy, NULL));
    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK);
    assert(sdk_exit_public_release("one", r.request_id) == AZ_OK);
}
static az_command_request reloop_request(unsigned deck) {
    __auto_type r = request(deck);
    r.operation = AZ_CMD_RELOOP;
    return r;
}
static void reloop_happy() {
    for (unsigned d = 0; d < 4; ++d) {
        __auto_type n = &(natives[d]);
        put(n->context, 0x190, (uint8_t) (0));
        __auto_type r = reloop_request(d);
        __auto_type out = result();
        unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
        assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
        assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED);
        process(d);
        assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before + 1 && n->context.bytes[0x190]);
        terminal(r, AZ_COMMAND_APPLIED, AZ_OK);
        exit_noop = true; /* An already engaged loop does not prove a RELOOP seek. */
        complete(reloop_request(d), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
        exit_noop = false;
        /* Even a fully disposed native chain cannot prove an active-loop seek. */
        n->deferred = true;
        __auto_type active_request = reloop_request(d);
        assert(sdk_exit_public_submit("one", &active_request, &out) == AZ_OK);
        for (unsigned frame_index = 0; frame_index < 3; ++frame_index) process(d);
        terminal(active_request, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
        n->deferred = false;
        put(n->context, 0x190, (uint8_t) (0));
        n->retained = false;
        complete(reloop_request(d), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
        n->retained = true;
    }
    __auto_type r = reloop_request(0);
    __auto_type out = result();
    __auto_type saved = out;
    r.beats = 4;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID && !memcmp(&out, &saved, sizeof out));
    r.beats = 0;
    r.expected_revision = 1;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID);
    r.expected_revision = 0;
    r.flags |= AZ_COMMAND_PERSIST;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID);
    r.flags = AZ_COMMAND_NATIVE_LOAD;
    r.quantization = AZ_QUANTIZE_OFF;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID);
    r.quantization = AZ_QUANTIZE_NATIVE;
    r.flags = 0;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_UNSUPPORTED);
}
static void reloop_async(bool existing) {
    for (unsigned d = 0; d < 4; ++d) {
        __auto_type n = &(natives[d]);
        put(n->context, 0x190, (uint8_t) (0));
        n->deferred = true;
        if (existing) (void) existing_quantize_pending(d);
        unsigned effects = n->effects, cancels = n->cancels;
        __auto_type r = reloop_request(d);
        __auto_type out = result();
        assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
        process(d);
        pending(r);
        assert(!n->context.bytes[0x190] && n->effects == effects && n->cancels == cancels);
        process(d);
        if (!existing) pending(r);
        process(d);
        assert(n->context.bytes[0x190] && n->effects == effects + 1);
        terminal(r, AZ_COMMAND_APPLIED, AZ_OK);
        n->deferred = false;
    }
}
static void reloop_stale_pending() {
    __auto_type n = &(natives[0]);
    put(n->context, 0x190, (uint8_t) (0));
    n->deferred = n->pause_engine = true;
    (void) existing_quantize_pending(0);
    __auto_type r = reloop_request(0);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    pending(r);
    unsigned executes = n->command_executes, effects = n->effects;
    advance_load(0);
    process(0);
    fenced(r, AZ_STALE);
    n->pause_engine = false;
    process(0);
    assert(n->command_executes == executes && n->effects == effects && !n->context.bytes[0x190]);
    dispose_pending(0);
    process(0);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_STALE);
    n->deferred = false;
    r = reloop_request(0);
    unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    assert(sdk_exit_public_cancel("one", r.request_id, &out) == AZ_OK);
    process(0);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    terminal(r, AZ_COMMAND_CANCELLED, AZ_CANCELLED);
    r = reloop_request(0);
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    advance_load(0);
    process(0);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    terminal(r, AZ_COMMAND_REJECTED, AZ_STALE);
}
static void reloop_pending_replaced() {
    __auto_type n = &(natives[0]);
    put(n->context, 0x190, (uint8_t) (0));
    n->deferred = n->pause_engine = true;
    (void) existing_quantize_pending(0);
    __auto_type r = reloop_request(0);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    pending(r);
    uintptr_t callback = object_word(n->quantize.bytes, 0xf8);
    owner_frame = true;
    /* A callback runs, but replaces the adopted pending node before it executes. */
    assert(run_command(callback));
    owner_frame = false;
    put(n->quantize, 0xf8, (uintptr_t) (0));
    put(n->quantize, 0x98, (int32_t) (-1));
    release(callback);
    n->pause_engine = false;
    process(0);
    assert(n->context.bytes[0x190]);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
    n->deferred = false;
}
static void mixed_fifo() {
    active(0);
    __auto_type first = request(0);
    __auto_type second = reloop_request(0);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &first, &out) == AZ_OK);
    assert(sdk_exit_public_submit("one", &second, &out) == AZ_OK);
    process(0);
    assert(!natives[0].context.bytes[0x190]);
    assert(sdk_exit_public_poll("one", first.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED);
    uint64_t exit_revision = out.revision;
    pending(second);
    process(0);
    assert(natives[0].context.bytes[0x190]);
    assert(sdk_exit_public_poll("one", second.request_id, &out) == AZ_OK && out.outcome == AZ_COMMAND_APPLIED && out.revision > exit_revision);
    terminal(first, AZ_COMMAND_APPLIED, AZ_OK);
    terminal(second, AZ_COMMAND_APPLIED, AZ_OK);
}
static void legacy_service() {
    __auto_type api = azstem_exit_service_v1();
    struct Legacy {
        uint32_t size, version;
        unsigned (*mask)(void);
        int (*target)(unsigned, uint64_t *);
        int (*submit)(const azexit_request *);
        int (*cancel)(unsigned, uint64_t);
        int (*poll)(unsigned, azexit_result *);
        int (*release)(unsigned, uint64_t);
    } legacy = {AZEXIT_SERVICE_V1_SIZE, 1, api->mask, api->target, api->submit, api->cancel, api->poll, api->release};
    _Static_assert(sizeof legacy == AZEXIT_SERVICE_V1_SIZE, "legacy bridge prefix changed");
    sdk_exit_public_bind(((const azexit_service *) (&legacy)));
    assert(!sdk_exit_public_capabilities());
    __auto_type r = reloop_request(0);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_UNSUPPORTED);
    r.operation = AZ_CMD_LOOP_EXIT;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_UNSUPPORTED);
    sdk_exit_public_bind(api);
    active(0);
    complete(request(0), AZ_COMMAND_APPLIED, AZ_OK);
}
static void capability_gating(unsigned enabled) {
    __auto_type api = azstem_exit_service_v1();
    assert(api->mask() == (enabled & AZEXIT_ENABLE_EXIT ? 15u : 0u));
    __auto_type disabled = enabled & AZEXIT_ENABLE_EXIT ? reloop_request(0) : request(0);
    __auto_type out = result();
    unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
    assert(sdk_exit_public_submit("one", &disabled, &out) == AZ_UNSUPPORTED);
    process(0);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    if (enabled & AZEXIT_ENABLE_EXIT) {
        active(0);
        complete(request(0), AZ_COMMAND_APPLIED, AZ_OK);
    } else {
        azexit_request old = {999, atomic_load_explicit(&(loads[0]), memory_order_seq_cst), UINT64_MAX, 0, 0};
        assert(api->submit(&old) == AZ_UNSUPPORTED);
        complete(reloop_request(0), AZ_COMMAND_APPLIED, AZ_OK);
    }
}
static az_command_request loop_request(unsigned operation, unsigned deck) {
    __auto_type r = request(deck);
    r.operation = operation;
    if (operation == AZ_CMD_LOOP_LENGTH) r.beats = 3.0;
    if (operation == AZ_CMD_LOOP_MOVE || operation == AZ_CMD_LOOP_ADJUST_IN) r.position_s = 0.25;
    if (operation == AZ_CMD_LOOP_ADJUST_OUT) r.end_s = 0.25;
    return r;
}
static void loop_complete(az_command_request r, unsigned outcome, int reason) {
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED);
    for (unsigned i = 0; i < 8; ++i) {
        process(r.deck);
        assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK);
        if (out.outcome != AZ_COMMAND_ACCEPTED) {
            terminal(r, outcome, reason);
            return;
        }
    }
    fprintf(stderr, "loop request stuck: operation=%u deck=%u schedules=%u fault=%u chain=%lx mode=%u\n",
            r.operation, r.deck, natives[r.deck].schedules, natives[r.deck].maintenance_fault,
            object_word(natives[r.deck].manager.bytes, 0), natives[r.deck].maintenance_mode);
    assert(false); /* bounded fixture must reach an explicit receipt */
}
static bool same_loop_property(const unsigned char *first, const unsigned char *second) {
    /* Preserve exact metadata/source identity; compare sample and cached-usec
     * timestamps in the actual native source's one-sample window. */
    for (unsigned byte = 0; byte < 0x100; ++byte) {
        bool timestamp = (byte >= 0x40 && byte < 0x48) || (byte >= 0x68 && byte < 0x70) ||
                         (byte >= 0x98 && byte < 0xa0) || (byte >= 0xc0 && byte < 0xc8);
        if (!timestamp && first[byte] != second[byte]) return false;
    }
    {
        unsigned range_values_5[] = {0x30u, 0x88u};
        for (unsigned range_index_5 = 0; range_index_5 < sizeof(range_values_5) / sizeof(range_values_5)[0]; ++range_index_5) {
            unsigned point_offset = range_values_5[range_index_5];

            __auto_type rate = get_uint32_t(first, point_offset + 0x30);
            const int samples = compare_timestamps(get_uint64_t(first, point_offset + 0x10), get_uint64_t(second, point_offset + 0x10), rate, rate, 1);
            const int micros = compare_timestamps(get_uint64_t(first, point_offset + 0x38), get_uint64_t(second, point_offset + 0x38), 1000000, rate, 1);
            if (samples == AZ_TIME_INVALID || samples != AZ_TIME_EQUAL || micros == AZ_TIME_INVALID || micros != AZ_TIME_EQUAL) return false;
        }
    }
    return true;
}
static void loop_commands() {
    sdk_exit_public_auth();
    for (unsigned d = 0; d < 4; ++d) {
        put(natives[d].retained_property, 0, ((uintptr_t) (set_table)));
        loop_complete(loop_request(AZ_CMD_LOOP_IN, d), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        put(natives[d].retained_property, 0, ((uintptr_t) (cue_table)));
    }
    const unsigned operations[] = {AZ_CMD_LOOP_IN, AZ_CMD_LOOP_OUT, AZ_CMD_LOOP_ENTER,
                                   AZ_CMD_LOOP_CLEAR, AZ_CMD_LOOP_DOUBLE, AZ_CMD_LOOP_HALVE, AZ_CMD_LOOP_LENGTH,
                                   AZ_CMD_LOOP_MOVE, AZ_CMD_LOOP_ADJUST_IN, AZ_CMD_LOOP_ADJUST_OUT};
    for (unsigned d = 0; d < 4; ++d) {
        {
            for (unsigned range_index_6 = 0; range_index_6 < sizeof(operations) / sizeof(operations)[0]; ++range_index_6) {
                unsigned operation = operations[range_index_6];

                loop_points(d, 48000, 144000);
                active(d);
                if (operation == AZ_CMD_LOOP_ENTER || operation == AZ_CMD_LOOP_OUT) put(natives[d].context, 0x190, (uint8_t) (0));
                unsigned char retained_before[0x100];
                memcpy(retained_before, natives[d].retained_property.bytes, sizeof retained_before);
                loop_complete(loop_request(operation, d), AZ_COMMAND_APPLIED, AZ_OK);
                if (operation == AZ_CMD_LOOP_LENGTH) {
                    __auto_type working = natives[d].context.bytes + 0x160;
                    __auto_type retained = natives[d].retained_property.bytes;
                    __auto_type rate = get_uint32_t(working, 0x60);
                    assert(rate && compare_timestamps(get_uint64_t(retained, 0x40), get_uint64_t(working, 0x40), rate, rate, 1) == AZ_TIME_EQUAL);
                    assert(compare_timestamps(get_uint64_t(retained, 0x98), get_uint64_t(working, 0x98), rate, rate, 1) == AZ_TIME_EQUAL);
                    assert(get_uint32_t(retained, 0xc8) == 1 && get_uint32_t(retained, 0xd8) == get_uint32_t(working, 0xd8));
                } else if (operation >= AZ_CMD_LOOP_DOUBLE && operation != AZ_CMD_LOOP_ADJUST_IN && operation != AZ_CMD_LOOP_MOVE)
                    assert(same_loop_property(retained_before, natives[d].retained_property.bytes));
            }
        }
    }
    for (unsigned d = 0; d < 4; ++d) {
        {
            for (unsigned range_index_7 = 0; range_index_7 < sizeof(operations) / sizeof(operations)[0]; ++range_index_7) {
                unsigned operation = operations[range_index_7];

                loop_points(d, 48000, 144000);
                active(d);
                if (operation == AZ_CMD_LOOP_ENTER || operation == AZ_CMD_LOOP_OUT) put(natives[d].context, 0x190, (uint8_t) (0));
                natives[d].deferred = true;
                __auto_type r = loop_request(operation, d);
                __auto_type out = result();
                assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
                process(d);
                pending(r); /* posting/first stage is not application */
                for (unsigned i = 0; i < 8; ++i) {
                    process(d);
                    assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK);
                    if (out.outcome != AZ_COMMAND_ACCEPTED) break;
                }
                if (out.outcome == AZ_COMMAND_ACCEPTED)
                    fprintf(stderr, "deferred operation %u deck %u stuck: calls %u executes %u manager %lx/%lx quant %d callback %lx pendingop%u\n",
                            operation, d, atomic_load_explicit(&(calls), memory_order_seq_cst), natives[d].command_executes,
                            object_word(natives[d].manager.bytes, 0), object_word(natives[d].manager.bytes, 8),
                            get_int32_t(natives[d].quantize.bytes, 0x98), object_word(natives[d].quantize.bytes, 0xf8), natives[d].pending_operation);
                terminal(r, AZ_COMMAND_APPLIED, AZ_OK);
                natives[d].deferred = false;
            }
        }
    }
    {
        for (unsigned range_index_8 = 0; range_index_8 < sizeof(operations) / sizeof(operations)[0]; ++range_index_8) {
            unsigned operation = operations[range_index_8];

            __auto_type r = loop_request(operation, 0);
            __auto_type out = result();
            r.flags |= AZ_COMMAND_PERSIST;
            assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID);
            r.flags = AZ_COMMAND_NATIVE_LOAD;
            r.expected_revision = 1;
            assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID);
            r.expected_revision = 0;
            r.quantization = AZ_QUANTIZE_ON;
            assert(sdk_exit_public_submit("one", &r, &out) == AZ_INVALID);
        }
    }
    /* Negative shifts preserve length; each boundary adjustment preserves its partner. */
    {
        unsigned range_values_9[] = {AZ_CMD_LOOP_MOVE, AZ_CMD_LOOP_ADJUST_IN, AZ_CMD_LOOP_ADJUST_OUT};
        for (unsigned range_index_9 = 0; range_index_9 < sizeof(range_values_9) / sizeof(range_values_9)[0]; ++range_index_9) {
            unsigned operation = range_values_9[range_index_9];

            loop_points(0, 48000, 144000);
            active(0);
            __auto_type r = loop_request(operation, 0);
            if (operation == AZ_CMD_LOOP_ADJUST_OUT) r.end_s = -0.25;
            else r.position_s = -0.25;
            loop_complete(r, AZ_COMMAND_APPLIED, AZ_OK);
            __auto_type in = get_uint64_t(natives[0].context.bytes + 0x160, 0x40);
            __auto_type end = get_uint64_t(natives[0].context.bytes + 0x160, 0x98);
            assert(sample_at(in, operation == AZ_CMD_LOOP_ADJUST_OUT ? 48000u : 36000u, get_uint32_t(natives[0].context.bytes + 0x160, 0x60)));
            assert(sample_at(end, operation == AZ_CMD_LOOP_ADJUST_IN ? 144000u : 132000u, get_uint32_t(natives[0].context.bytes + 0x160, 0xb8)));
        }
    }
    {
        bool range_values_10[] = {false, true};
        for (unsigned range_index_10 = 0; range_index_10 < sizeof(range_values_10) / sizeof(range_values_10)[0]; ++range_index_10) {
            bool deferred = range_values_10[range_index_10];

            {
                unsigned range_values_11[] = {AZ_CMD_LOOP_DOUBLE, AZ_CMD_LOOP_HALVE};
                for (unsigned range_index_11 = 0; range_index_11 < sizeof(range_values_11) / sizeof(range_values_11)[0]; ++range_index_11) {
                    unsigned operation = range_values_11[range_index_11];

                    loop_points(0, 48000, 144000);
                    active(0);
                    __auto_type n = &(natives[0]);
                    put(n->context, 0x160 + 0xc8, (uint32_t) (1));
                    put(n->context, 0x160 + 0xd8, (uint32_t) (0x00010002));
                    n->deferred = deferred;
                    loop_complete(loop_request(operation, 0), AZ_COMMAND_APPLIED, AZ_OK);
                    assert(sample_at(get_uint64_t(n->context.bytes + 0x160, 0x40), 48000, get_uint32_t(n->context.bytes + 0x160, 0x60)));
                    assert(sample_at(get_uint64_t(n->context.bytes + 0x160, 0x98),
                                     operation == AZ_CMD_LOOP_DOUBLE ? 192000u : 120000u, get_uint32_t(n->context.bytes + 0x160, 0xb8)));
                    n->deferred = false;
                    loop_points(0, 48000, 144000);
                    active(0);
                    put(n->context, 0x160 + 0xc8, (uint32_t) (1));
                    put(n->context, 0x160 + 0xd8, (uint32_t) (0x00010002));
                    n->wrong_beat = true;
                    loop_complete(loop_request(operation, 0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
                    n->wrong_beat = false;
                }
            }
        }
    }
    loop_points(0, 48000, 144000);
    active(0);
    __auto_type normalized = loop_request(AZ_CMD_LOOP_LENGTH, 0);
    normalized.beats = 1.5;
    natives[0].normalize_beat = true;
    loop_complete(normalized, AZ_COMMAND_APPLIED, AZ_OK);
    assert(get_uint32_t(natives[0].context.bytes + 0x160, 0xd8) == 0x00040006);
    natives[0].normalize_beat = false;
    uint32_t packed = 0;
    assert(azexit_pack_beat(1.5, &packed) && packed == 0x00020003);
    assert(azexit_pack_beat(1.0 / 3.0, &packed) && packed == 0x00030001);
    assert(azexit_pack_beat(65535.0, &packed) && packed == 0x0001ffff);
    assert(!azexit_pack_beat(0, &packed) && !azexit_pack_beat(-1, &packed));
    assert(!azexit_pack_beat(65536.0, &packed));
    __auto_type length = loop_request(AZ_CMD_LOOP_LENGTH, 0);
    __auto_type out = result();
    length.beats = 0;
    assert(sdk_exit_public_submit("one", &length, &out) == AZ_INVALID);
    length.beats = 65536;
    assert(sdk_exit_public_submit("one", &length, &out) == AZ_INVALID);
    {
        unsigned range_values_12[] = {AZ_CMD_LOOP_IN, AZ_CMD_LOOP_OUT, AZ_CMD_LOOP_DOUBLE,
                                      AZ_CMD_LOOP_HALVE, AZ_CMD_LOOP_LENGTH, AZ_CMD_LOOP_ADJUST_IN,
                                      AZ_CMD_LOOP_ADJUST_OUT};
        for (unsigned range_index_12 = 0; range_index_12 < sizeof(range_values_12) / sizeof(range_values_12)[0]; ++range_index_12) {
            unsigned operation = range_values_12[range_index_12];

            loop_points(0, 48000, 144000);
            active(0);
            if (operation == AZ_CMD_LOOP_OUT) put(natives[0].context, 0x190, (uint8_t) (0));
            exit_noop = true;
            loop_complete(loop_request(operation, 0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
            exit_noop = false;
        }
    }
    {
        unsigned range_values_13[] = {AZ_CMD_LOOP_ADJUST_IN, AZ_CMD_LOOP_MOVE};
        for (unsigned range_index_13 = 0; range_index_13 < sizeof(range_values_13) / sizeof(range_values_13)[0]; ++range_index_13) {
            unsigned operation = range_values_13[range_index_13];

            {
                unsigned range_values_14[] = {0x14u, 0x380u};
                for (unsigned range_index_14 = 0; range_index_14 < sizeof(range_values_14) / sizeof(range_values_14)[0]; ++range_index_14) {
                    unsigned offset = range_values_14[range_index_14];

                    loop_points(0, 48000, 144000);
                    active(0);
                    if (offset == 0x14) put(natives[0].context, offset, (uint32_t) (1));
                    else put(natives[0].context, offset, (uint8_t) (1));
                    unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
                    loop_complete(loop_request(operation, 0), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
                    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
                    if (offset == 0x14) put(natives[0].context, offset, (uint32_t) (0));
                    else put(natives[0].context, offset, (uint8_t) (0));
                }
            }
        }
    }
    loop_points(0, 48000, 144000);
    active(0);
    put(natives[0].context, 0x160 + 0xa8, (uint64_t) (0x10100));
    loop_complete(loop_request(AZ_CMD_LOOP_DOUBLE, 0), AZ_COMMAND_APPLIED, AZ_OK);
    loop_points(0, 48000, 144000);
    active(0);
    natives[0].context.bytes[0x160 + 0x88] ^= 2;
    unsigned source_rejected_calls = atomic_load_explicit(&(calls), memory_order_seq_cst);
    loop_complete(loop_request(AZ_CMD_LOOP_DOUBLE, 0), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == source_rejected_calls);
    loop_points(0, 48000, 144000);
    active(0);
    put(natives[0].context, 0x160 + 0xa8, (uint64_t) (0));
    unsigned rejected_calls = atomic_load_explicit(&(calls), memory_order_seq_cst);
    loop_complete(loop_request(AZ_CMD_LOOP_DOUBLE, 0), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == rejected_calls);
    put(natives[0].facade, 0xc8 + 0x10, ((uintptr_t) natives[1].core.bytes) + 0x110);
    loop_complete(loop_request(AZ_CMD_LOOP_DOUBLE, 0), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == rejected_calls);
    put(natives[0].facade, 0xc8 + 0x10, ((uintptr_t) natives[0].core.bytes) + 0x110);
    /* MOVE cannot finish on another load or hide a second-stage no-op. */
    {
        bool range_values_15[] = {false, true};
        for (unsigned range_index_15 = 0; range_index_15 < sizeof(range_values_15) / sizeof(range_values_15)[0]; ++range_index_15) {
            bool stale_load = range_values_15[range_index_15];

            loop_points(0, 48000, 144000);
            active(0);
            natives[0].deferred = true;
            __auto_type r = loop_request(AZ_CMD_LOOP_MOVE, 0);
            __auto_type move_out = result();
            assert(sdk_exit_public_submit("one", &r, &move_out) == AZ_OK);
            process(0); /* first stage is posted, awaiting native execution */
            pending(r);
            process(0); /* first stage applies; second stage posts separately */
            pending(r);
            if (stale_load) advance_load(0);
            else exit_noop = true;
            for (unsigned i = 0; i < 8; ++i) {
                process(0);
                assert(sdk_exit_public_poll("one", r.request_id, &move_out) == AZ_OK);
                if (move_out.outcome != AZ_COMMAND_ACCEPTED) break;
            }
            assert(move_out.outcome == AZ_COMMAND_UNCERTAIN && move_out.reason == (stale_load ? AZ_STALE : AZ_FAILED));
            assert(sample_at(get_uint64_t(natives[0].context.bytes + 0x160, 0x40), 48000, get_uint32_t(natives[0].context.bytes + 0x160, 0x60)));
            dispose_pending(0); /* native load replacement owns holder disposal */
            process(0);
            terminal(r, AZ_COMMAND_UNCERTAIN, stale_load ? AZ_STALE : AZ_FAILED);
            exit_noop = false;
            natives[0].deferred = false;
        }
    }
    /* Cancellation before the processing frame prevents native mutation. */
    loop_points(0, 48000, 144000);
    active(0);
    __auto_type cancelled = loop_request(AZ_CMD_LOOP_DOUBLE, 0);
    unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
    assert(sdk_exit_public_submit("one", &cancelled, &out) == AZ_OK);
    assert(sdk_exit_public_cancel("one", cancelled.request_id, &out) == AZ_OK);
    process(0);
    terminal(cancelled, AZ_COMMAND_CANCELLED, AZ_CANCELLED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    /* A queued request cannot act on a replacement load. */
    __auto_type stale = loop_request(AZ_CMD_LOOP_HALVE, 0);
    assert(sdk_exit_public_submit("one", &stale, &out) == AZ_OK);
    advance_load(0);
    process(0);
    terminal(stale, AZ_COMMAND_REJECTED, AZ_STALE);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    /* Same-deck ordering spans the old EXIT route and new parameterized route. */
    loop_points(0, 48000, 144000);
    active(0);
    __auto_type first = loop_request(AZ_CMD_LOOP_DOUBLE, 0);
    __auto_type second = request(0);
    assert(sdk_exit_public_submit("one", &first, &out) == AZ_OK);
    assert(sdk_exit_public_submit("one", &second, &out) == AZ_OK);
    process(0);
    terminal(first, AZ_COMMAND_APPLIED, AZ_OK);
    process(0);
    terminal(second, AZ_COMMAND_APPLIED, AZ_OK);
}
static uint64_t native_now() {
    struct timespec value;
    assert(!clock_gettime(CLOCK_MONOTONIC, &value));
    return (uint64_t) (value.tv_sec) * UINT64_C(1000000000) + (uint64_t) (value.tv_nsec);
}
static void native_future_owned_once(unsigned d) {
    uintptr_t chain = object_word(natives[d].manager.bytes, 0);
    assert(chain && get_uint32_t(((void *) (chain)), 8) == 1);
    uintptr_t node = object_word(((void *) (chain)), 0x18);
    unsigned count = 0;
    while (node) {
        assert(get_uint32_t(((void *) (node)), 8) == 1);
        node = object_word(((void *) (node)), 0x18);
        assert(++count <= 3);
    }
    assert(count == (natives[d].maintenance_play ? 3u : 2u));
    if (natives[d].pending_wrapper) {
        uintptr_t hidden = object_word(natives[d].quantize.bytes, 0x30);
        assert(hidden && get_uint32_t(((void *) (hidden)), 8) == 1);
        assert(!object_word(((void *) (hidden)), 0x18));
    }
}
static void recurring_loop_commands() {
    for (unsigned d = 0; d < 4; ++d) {
        __auto_type n = &(natives[d]);
        loop_points(d, 48000, 144000);
        put(n->context, 0x190, (uint8_t) (0));
        n->maintained_loop = n->replace_future = true;
        n->maintenance_mode = d & 1 ? 3 : 0;
        n->maintenance_play = d < 2;
        __auto_type r = loop_request(AZ_CMD_LOOP_OUT, d);
        r.deadline_ns = native_now() + UINT64_C(100000000);
        loop_complete(r, AZ_COMMAND_APPLIED, AZ_OK);
        assert(object_word(n->manager.bytes, 0)); /* future work survives APPLIED */
        native_future_owned_once(d);
        assert(n->schedules >= 2 && n->maintenance_executes == 0);
        n->replace_future = false;
        pause_ms(120);
        unsigned before = n->maintenance_executes;
        n->cycle_future = true;
        for (unsigned cycle = 0; cycle < 5; ++cycle) process(d);
        assert(n->maintenance_executes == before + 5 && object_word(n->manager.bytes, 0));
        n->cycle_future = false;
        /* A later SDK edit must take over existing native cycling without
         * retaining the old request's deadline or requiring an empty manager. */
        {
            unsigned range_values_16[] = {AZ_CMD_LOOP_DOUBLE, AZ_CMD_LOOP_HALVE, AZ_CMD_LOOP_LENGTH,
                                          AZ_CMD_LOOP_ADJUST_IN, AZ_CMD_LOOP_ADJUST_OUT, AZ_CMD_LOOP_MOVE};
            for (unsigned range_index_16 = 0; range_index_16 < sizeof(range_values_16) / sizeof(range_values_16)[0]; ++range_index_16) {
                unsigned operation = range_values_16[range_index_16];

                loop_complete(loop_request(operation, d), AZ_COMMAND_APPLIED, AZ_OK);
                assert(object_word(n->manager.bytes, 0));
                native_future_owned_once(d);
                before = n->maintenance_executes;
                n->cycle_future = true;
                process(d);
                assert(n->maintenance_executes == before + 1);
                n->cycle_future = false;
            }
        }
        {
            bool range_values_17[] = {false, true};
            for (unsigned range_index_17 = 0; range_index_17 < sizeof(range_values_17) / sizeof(range_values_17)[0]; ++range_index_17) {
                bool deferred = range_values_17[range_index_17];

                n->deferred = deferred;
                if (deferred) {
                    loop_points(d, 48000, 144000);
                    put(n->context, 0x190, (uint8_t) (0));
                    loop_complete(loop_request(AZ_CMD_LOOP_OUT, d), AZ_COMMAND_APPLIED, AZ_OK);
                }
                loop_complete(request(d), AZ_COMMAND_APPLIED, AZ_OK);
                assert(!n->context.bytes[0x190] && !object_word(n->manager.bytes, 0) && !object_word(n->manager.bytes, 8));
                {
                    unsigned range_values_18[] = {AZ_CMD_RELOOP, AZ_CMD_LOOP_ENTER};
                    for (unsigned range_index_18 = 0; range_index_18 < sizeof(range_values_18) / sizeof(range_values_18)[0]; ++range_index_18) {
                        unsigned operation = range_values_18[range_index_18];

                        __auto_type enter = request(d);
                        enter.operation = operation;
                        loop_complete(enter, AZ_COMMAND_APPLIED, AZ_OK);
                        assert(n->context.bytes[0x190]);
                        native_future_owned_once(d);
                        before = n->maintenance_executes;
                        n->cycle_future = true;
                        for (unsigned cycle = 0; cycle < 3; ++cycle) process(d);
                        assert(n->maintenance_executes == before + 3);
                        n->cycle_future = false;
                        loop_complete(request(d), AZ_COMMAND_APPLIED, AZ_OK);
                        assert(!n->context.bytes[0x190] && !object_word(n->manager.bytes, 0) && !object_word(n->manager.bytes, 8));
                        before = n->maintenance_executes;
                        n->cycle_future = true;
                        process(d);
                        process(d);
                        assert(n->maintenance_executes == before); /* no future wraps after EXIT */
                        n->cycle_future = false;
                    }
                }
                loop_complete(loop_request(AZ_CMD_LOOP_CLEAR, d), AZ_COMMAND_APPLIED, AZ_OK);
                assert(!n->retained_property.bytes[0x30] && !n->retained_property.bytes[0x88]);
                assert(!n->context.bytes[0x190] && !object_word(n->manager.bytes, 0));
            }
        }
        n->deferred = n->maintained_loop = false;
        dispose_pending(d);
    }
    __auto_type n = &(natives[0]);
    for (unsigned fault = 1; fault <= 6; ++fault) {
        loop_points(0, 48000, 144000);
        put(n->context, 0x190, (uint8_t) (0));
        n->maintained_loop = true;
        n->maintenance_fault = fault;
        __auto_type r = loop_request(AZ_CMD_LOOP_OUT, 0);
        __auto_type out = result();
        assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
        process(0);
        for (unsigned i = 0; i < 3; ++i) {
            assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK);
            assert(out.outcome != AZ_COMMAND_APPLIED);
            process(0);
        }
        n->maintained_loop = false;
        dispose_pending(0);
        process(0);
        terminal(r, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
        n->maintenance_fault = 0;
    }
    {
        bool range_values_19[] = {false, true};
        for (unsigned range_index_19 = 0; range_index_19 < sizeof(range_values_19) / sizeof(range_values_19)[0]; ++range_index_19) {
            bool deadline = range_values_19[range_index_19];

            loop_points(0, 48000, 144000);
            put(n->context, 0x190, (uint8_t) (0));
            n->maintained_loop = true;
            n->invalidate_after_pre = !deadline;
            n->expire_after_pre = deadline;
            __auto_type r = loop_request(AZ_CMD_LOOP_OUT, 0);
            if (deadline) r.deadline_ns = native_now() + UINT64_C(100000000);
            __auto_type out = result();
            assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
            process(0);
            fenced(r, AZ_STALE);
            n->invalidate_after_pre = n->expire_after_pre = false;
            unsigned executes = n->command_executes;
            n->cycle_future = true;
            process(0); /* failed handoff retains suppression through native disposal */
            assert(n->command_executes == executes);
            n->cycle_future = n->maintained_loop = false;
            dispose_pending(0);
            process(0);
            terminal(r, AZ_COMMAND_UNCERTAIN, AZ_STALE);
        }
    }
}
static void preparatory_loop_commands() {
    for (unsigned d = 0; d < 4; ++d) {
        __auto_type n = &(natives[d]);
        n->maintained_loop = n->preparatory_loop = true;
        {
            bool range_values_20[] = {false, true};
            for (unsigned range_index_20 = 0; range_index_20 < sizeof(range_values_20) / sizeof(range_values_20)[0]; ++range_index_20) {
                bool deferred = range_values_20[range_index_20];

                loop_points(d, 48000, 144000);
                put(n->context, 0x190, (uint8_t) (0));
                n->deferred = deferred;
                unsigned clears = n->setup_cancels;
                loop_complete(loop_request(AZ_CMD_LOOP_OUT, d), AZ_COMMAND_APPLIED, AZ_OK);
                assert(n->setup_cancels == clears + 2);
                native_future_owned_once(d);
                unsigned before = n->maintenance_executes;
                n->cycle_future = true;
                for (unsigned i = 0; i < 3; ++i) process(d);
                assert(n->maintenance_executes == before + 3);
                n->cycle_future = false;
                loop_complete(request(d), AZ_COMMAND_APPLIED, AZ_OK);
                assert(!object_word(n->manager.bytes, 0));
            }
        }
        n->deferred = n->maintained_loop = n->preparatory_loop = false;
    }
    __auto_type n = &(natives[0]);
    n->maintained_loop = n->preparatory_loop = true;
    for (unsigned fault = 1; fault <= 3; ++fault) {
        loop_points(0, 48000, 144000);
        put(n->context, 0x190, (uint8_t) (0));
        n->setup_fault = fault;
        loop_complete(loop_request(AZ_CMD_LOOP_OUT, 0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
        dispose_pending(0);
    }
    n->setup_fault = 0;
    n->omit_future = true;
    loop_points(0, 48000, 144000);
    put(n->context, 0x190, (uint8_t) (0));
    __auto_type r = loop_request(AZ_CMD_LOOP_OUT, 0);
    r.deadline_ns = native_now() + UINT64_C(100000000);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    pending(r);
    process(0);
    pending(r); /* engaged state plus an empty queue is not replacement proof */
    pause_ms(120);
    fenced(r, AZ_STALE);
    dispose_pending(0);
    process(0);
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_STALE);
    n->omit_future = n->maintained_loop = n->preparatory_loop = false;
}
static void quantized_loop_commands() {
    for (unsigned d = 0; d < 4; ++d) {
        __auto_type n = &(natives[d]);
        loop_points(d, 48000, 144000);
        active(d);
        n->maintained_loop = n->pending_wrapper = true;
        __auto_type r = loop_request(AZ_CMD_LOOP_LENGTH, d);
        r.deadline_ns = native_now() + UINT64_C(100000000);
        loop_complete(r, AZ_COMMAND_APPLIED, AZ_OK);
        assert(object_word(n->quantize.bytes, 0x30) && get_uint32_t(n->quantize.bytes, 0x28) == 2);
        native_future_owned_once(d);
        unsigned before = n->wrapper_executes;
        pause_ms(120);
        n->cycle_future = true;
        for (unsigned cycle = 0; cycle < 5; ++cycle) process(d);
        assert(n->wrapper_executes == before + 5);
        native_future_owned_once(d); /* hidden callback and outer graph stay native-owned */
        n->cycle_future = false;
        __auto_type replacement = loop_request(AZ_CMD_LOOP_LENGTH, d);
        replacement.beats = 2;
        loop_complete(replacement, AZ_COMMAND_APPLIED, AZ_OK);
        native_future_owned_once(d);
        loop_complete(request(d), AZ_COMMAND_APPLIED, AZ_OK);
        assert(!object_word(n->manager.bytes, 0) && !object_word(n->quantize.bytes, 0x30));
        before = n->wrapper_executes;
        n->cycle_future = true;
        process(d);
        process(d);
        assert(n->wrapper_executes == before); /* stock EXIT cancels pending Quantize future */
        n->cycle_future = n->maintained_loop = n->pending_wrapper = false;
    }
    __auto_type n = &(natives[0]);
    for (unsigned fault = 1; fault <= 6; ++fault) {
        loop_points(0, 48000, 144000);
        active(0);
        n->maintained_loop = n->pending_wrapper = true;
        n->wrapper_fault = fault;
        __auto_type r = loop_request(AZ_CMD_LOOP_LENGTH, 0);
        if (fault == 6) r.deadline_ns = native_now() + UINT64_C(100000000);
        __auto_type out = result();
        assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
        process(0);
        for (unsigned i = 0; i < 3; ++i) {
            assert(sdk_exit_public_poll("one", r.request_id, &out) == AZ_OK);
            assert(out.outcome != AZ_COMMAND_APPLIED);
            process(0);
        }
        n->maintained_loop = n->pending_wrapper = false;
        if (fault == 6) {
            pause_ms(120);
            process(0);
            fenced(r, AZ_STALE);
            dispose_pending(0);
            process(0);
            terminal(r, AZ_COMMAND_UNCERTAIN, AZ_STALE);
        } else {
            dispose_pending(0);
            process(0);
            terminal(r, AZ_COMMAND_UNCERTAIN, AZ_FAILED);
        }
        n->wrapper_fault = 0;
    }
}
static void cue_reset(unsigned d) {
    __auto_type n = &(natives[d]);
    assert(!object_word(n->manager.bytes, 0) && !object_word(n->manager.bytes, 8));
    n->deferred = false;
    n->cue_fault = 0;
    n->cue_seek_noop = false;
    n->cue_seek_window = 0;
    loop_points(d, 48000, 144000);
    memcpy(n->core.bytes + 0x18, n->retained_property.bytes + 0x30, 12);
    /* This synthetic loaded source owns both the core head and retained
     * cue clock; cue-only runs must initialize the core clock explicitly. */
    put(n->core, 0x68, get_uint32_t(n->retained_property.bytes, 0x60));
    put(n->core, 0x28, (uint64_t) (24000));
    put(n->core, 9, (uint8_t) (1));
    put(n->play_pause, 8, (uint8_t) (1));
    put(n->play_pause, 9, (uint8_t) (0));
    put(n->beat, 0x104, (uint32_t) (0));
    for (unsigned kind = 1; kind <= 8; ++kind) {
        __auto_type property = &(n->hot_properties[kind - 1]);
        point(property->bytes, 0x18, 48000 * (kind + 1));
        memcpy(property->bytes + 0x30, n->core.bytes + 0x18, 12);
    }
}
static az_command_request cue_request(unsigned operation, unsigned d, unsigned kind) {
    __auto_type r = request(d);
    r.operation = operation;
    r.classification = operation == AZ_CMD_HOT_TRIGGER ? AZ_CUE_HOT : AZ_CUE_MAIN;
    r.entry_id = kind;
    return r;
}
static void cue_commands() {
    __auto_type api = azstem_exit_service_v1();
    assert(api->size >= AZEXIT_SERVICE_CUE_SIZE && api->submit_cue && api->target_cue);
    for (unsigned d = 0; d < 4; ++d) {
        azexit_cue_target target = {0};
        assert(api->target_cue(d, &target) == AZ_OK && !target.revision_valid);
        __auto_type n = &(natives[d]);
        {
            bool range_values_21[] = {false, true};
            for (unsigned range_index_21 = 0; range_index_21 < sizeof(range_values_21) / sizeof(range_values_21)[0]; ++range_index_21) {
                bool deferred = range_values_21[range_index_21];

                {
                    unsigned range_values_22[] = {0u, 1u};
                    for (unsigned range_index_22 = 0; range_index_22 < sizeof(range_values_22) / sizeof(range_values_22)[0]; ++range_index_22) {
                        unsigned playing = range_values_22[range_index_22];

                        cue_reset(d);
                        n->deferred = deferred;
                        put(n->core, 9, (uint8_t) (playing));
                        put(n->play_pause, 8, (uint8_t) (playing));
                        loop_complete(cue_request(AZ_CMD_MAIN_SET, d, 0), AZ_COMMAND_APPLIED, AZ_OK);
                        assert(sample_at(get_uint64_t(n->retained_property.bytes, 0x40), 96000, get_uint32_t(n->retained_property.bytes, 0x60)));
                        assert(n->core.bytes[9] == playing && n->play_pause.bytes[8] == playing); /* setting a cue preserves transport */
                    }
                }
                cue_reset(d);
                n->deferred = deferred;
                unsigned seeks = n->cue_seeks;
                unsigned native_seeks = n->cue_seek_calls, native_jumps = n->cue_jump_calls;
                loop_complete(cue_request(AZ_CMD_MAIN_RECALL, d, 0), AZ_COMMAND_APPLIED, AZ_OK);
                assert(n->cue_seeks == seeks + 1 && !n->play_pause.bytes[8] && n->core.bytes[9] == 1);
                assert(n->cue_seek_calls == native_seeks + 1 && n->cue_jump_calls == native_jumps + 1);
                assert(sample_at(get_uint64_t(n->core.bytes, 0x28), 48000, get_uint32_t(n->core.bytes, 0x68)));
                loop_complete(cue_request(AZ_CMD_MAIN_RECALL, d, 0), AZ_COMMAND_APPLIED, AZ_OK);
                assert(n->cue_seeks == seeks + 2 && !n->play_pause.bytes[8] && n->core.bytes[9] == 1); /* recall while already paused still seeks */
                for (unsigned kind = 1; kind <= 8; ++kind) {
                    cue_reset(d);
                    n->deferred = deferred;
                    put(n->core, 9, (uint8_t) (0));
                    put(n->play_pause, 8, (uint8_t) (0));
                    seeks = n->cue_seeks;
                    native_seeks = n->cue_seek_calls;
                    native_jumps = n->cue_jump_calls;
                    loop_complete(cue_request(AZ_CMD_HOT_TRIGGER, d, kind), AZ_COMMAND_APPLIED, AZ_OK);
                    assert(n->cue_seeks == seeks + 1 && n->play_pause.bytes[8] && n->core.bytes[9]);
                    assert(n->cue_seek_calls == native_seeks && n->cue_jump_calls == native_jumps + 1);
                    assert(sample_at(get_uint64_t(n->core.bytes, 0x28), 48000 * (kind + 1), get_uint32_t(n->core.bytes, 0x68)));
                    loop_complete(cue_request(AZ_CMD_HOT_TRIGGER, d, kind), AZ_COMMAND_APPLIED, AZ_OK);
                    assert(n->cue_seeks == seeks + 2 && n->play_pause.bytes[8] && n->core.bytes[9]); /* retrigger while playing needs a fresh receipt */
                }
            }
        }
    }
    for (unsigned deck = 0; deck < 4; ++deck) {
        {
            uint32_t range_values_23[] = {44100u, 48000u, 96000u};
            for (unsigned range_index_23 = 0; range_index_23 < sizeof(range_values_23) / sizeof(range_values_23)[0]; ++range_index_23) {
                uint32_t rate = range_values_23[range_index_23];

                {
                    bool range_values_24[] = {false, true};
                    for (unsigned range_index_24 = 0; range_index_24 < sizeof(range_values_24) / sizeof(range_values_24)[0]; ++range_index_24) {
                        bool deferred = range_values_24[range_index_24];

                        cue_reset(deck);
                        __auto_type native = &(natives[deck]);
                        native->deferred = deferred;
                        native->cue_seek_window = 109; /* Captured native latency window, not a fixed epsilon. */
                        put(native->hot_properties[0], 0x60, rate);
                        loop_complete(cue_request(AZ_CMD_HOT_TRIGGER, deck, 1), AZ_COMMAND_APPLIED, AZ_OK);
                        assert(compare_timestamps(get_uint64_t(native->core.bytes, 0x28),
                                                  get_uint64_t(native->hot_properties[0].bytes, 0x40), rate, rate, 219) == AZ_TIME_EQUAL);
                        cue_reset(deck);
                        native->deferred = deferred;
                        native->cue_seek_window = 109;
                        native->cue_fault = 13; /* One sample beyond the native seek window. */
                        loop_complete(cue_request(AZ_CMD_HOT_TRIGGER, deck, 1), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
                    }
                }
            }
        }
    }
    __auto_type n = &(natives[0]);
    {
        bool range_values_25[] = {false, true};
        for (unsigned range_index_25 = 0; range_index_25 < sizeof(range_values_25) / sizeof(range_values_25)[0]; ++range_index_25) {
            bool deferred = range_values_25[range_index_25];

            cue_reset(0);
            n->deferred = deferred;
            point(n->retained_property.bytes, 0x18, 152423);
            loop_complete(cue_request(AZ_CMD_MAIN_RECALL, 0, 0), AZ_COMMAND_APPLIED, AZ_OK);
            assert(sample_at(get_uint64_t(n->core.bytes, 0x28), 152423, get_uint32_t(n->core.bytes, 0x68)) && n->core.bytes[9] == 1 && n->play_pause.bytes[8] == 0);
        /* Real CDJ pause keeps the base stream playing while logical playback
         * and the visible cue position are paused. Setting a cue preserves it. */
            loop_complete(cue_request(AZ_CMD_MAIN_SET, 0, 0), AZ_COMMAND_APPLIED, AZ_OK);
            assert(n->core.bytes[9] == 1 && n->play_pause.bytes[8] == 0);
        }
    }
    for (unsigned fault = 0; fault < 6; ++fault) {
        cue_reset(0);
        unsigned char *object = fault < 2 ? n->play_pause.bytes : fault == 2 ? n->facade.bytes
                                                                             : n->cue_functions.bytes;
        unsigned offset = fault == 0 ? 0 : fault == 1 ? 0x58
                                       : fault == 2   ? 0x58
                                       : fault == 3   ? 0
                                       : fault == 4   ? 8
                                                      : 0xa8;
        uintptr_t original = object_word(object, offset);
        store(object, offset, (uintptr_t) (0));
        unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
        loop_complete(cue_request(AZ_CMD_MAIN_RECALL, 0, 0), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
        assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
        store(object, offset, original);
    }
    cue_reset(0);
    put(n->play_pause, 8, (uint8_t) (2));
    unsigned before_invalid_flag = atomic_load_explicit(&(calls), memory_order_seq_cst);
    loop_complete(cue_request(AZ_CMD_MAIN_RECALL, 0, 0), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before_invalid_flag);
    cue_reset(0);
    put(n->play_pause, 9, (uint8_t) (2));
    before_invalid_flag = atomic_load_explicit(&(calls), memory_order_seq_cst);
    loop_complete(cue_request(AZ_CMD_MAIN_RECALL, 0, 0), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before_invalid_flag);
    {
        unsigned range_values_26[] = {0u, 1u};
        for (unsigned range_index_26 = 0; range_index_26 < sizeof(range_values_26) / sizeof(range_values_26)[0]; ++range_index_26) {
            unsigned playing = range_values_26[range_index_26];
            {
                unsigned range_values_27[] = {0u, 1u};
                for (unsigned range_index_27 = 0; range_index_27 < sizeof(range_values_27) / sizeof(range_values_27)[0]; ++range_index_27) {
                    unsigned auxiliary = range_values_27[range_index_27];

                    cue_reset(0);
                    put(n->play_pause, 8, (uint8_t) (playing));
                    put(n->play_pause, 9, (uint8_t) (auxiliary));
                    loop_complete(cue_request(AZ_CMD_MAIN_SET, 0, 0), AZ_COMMAND_APPLIED, AZ_OK);
                    assert(n->play_pause.bytes[8] == playing && n->play_pause.bytes[9] == auxiliary);
                }
            }
        }
    }
    cue_reset(0);
    n->cue_fault = 11;
    loop_complete(cue_request(AZ_CMD_MAIN_RECALL, 0, 0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
    {
        unsigned range_values_28[] = {1u, 2u, 3u, 4u};
        for (unsigned range_index_28 = 0; range_index_28 < sizeof(range_values_28) / sizeof(range_values_28)[0]; ++range_index_28) {
            unsigned count = range_values_28[range_index_28];

            cue_reset(0);
            put(n->beat, 0x104, (uint32_t) (count));
            unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
            loop_complete(cue_request(AZ_CMD_HOT_TRIGGER, 0, 1), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
            assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
        }
    }
    {
        bool range_values_29[] = {false, true};
        for (unsigned range_index_29 = 0; range_index_29 < sizeof(range_values_29) / sizeof(range_values_29)[0]; ++range_index_29) {
            bool deferred = range_values_29[range_index_29];

            cue_reset(0);
            n->deferred = deferred;
            n->cue_seek_noop = true;
            unsigned entries = n->cue_seek_calls, effects = n->cue_seeks;
            loop_complete(cue_request(AZ_CMD_MAIN_RECALL, 0, 0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
            assert(n->cue_seek_calls == entries + 1 && n->cue_seeks == effects); /* returning without seeking is not an effect receipt */
        }
    }
    {
        unsigned range_values_30[] = {(unsigned) (AZ_CMD_MAIN_SET), (unsigned) (AZ_CMD_MAIN_RECALL), (unsigned) (AZ_CMD_HOT_TRIGGER)};
        for (unsigned range_index_30 = 0; range_index_30 < sizeof(range_values_30) / sizeof(range_values_30)[0]; ++range_index_30) {
            unsigned operation = range_values_30[range_index_30];

            cue_reset(0);
            exit_noop = true;
            loop_complete(cue_request(operation, 0, operation == AZ_CMD_HOT_TRIGGER ? 1 : 0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
            exit_noop = false;
        }
    }
    cue_reset(0);
    n->cue_fault = 2;
    loop_complete(cue_request(AZ_CMD_MAIN_SET, 0, 0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
    {
        unsigned range_values_31[] = {(unsigned) (AZ_CMD_MAIN_RECALL), (unsigned) (AZ_CMD_HOT_TRIGGER)};
        for (unsigned range_index_31 = 0; range_index_31 < sizeof(range_values_31) / sizeof(range_values_31)[0]; ++range_index_31) {
            unsigned operation = range_values_31[range_index_31];

            unsigned kind = operation == AZ_CMD_HOT_TRIGGER ? 1 : 0;
            {
                unsigned range_values_32[] = {1u, 2u, 3u, 4u, 5u};
                for (unsigned range_index_32 = 0; range_index_32 < sizeof(range_values_32) / sizeof(range_values_32)[0]; ++range_index_32) {
                    unsigned fault = range_values_32[range_index_32];

                    cue_reset(0);
                    n->cue_fault = fault;
                    if (fault == 1) { /* already at cue cannot substitute for an actual seek receipt */
                        void *property = kind ? n->hot_properties[kind - 1].bytes : n->retained_property.bytes;
                        put(n->core, 0x28, get_uint64_t(property, 0x40));
                        put(n->core, 9, (uint8_t) (1));
                        put(n->play_pause, 8, (uint8_t) (kind != 0));
                    }
                    loop_complete(cue_request(operation, 0, kind), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
                }
            }
            cue_reset(0);
            void *property = kind ? n->hot_properties[kind - 1].bytes : n->retained_property.bytes;
            ((unsigned char *) (property))[0x30] ^= 2;
            unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
            loop_complete(cue_request(operation, 0, kind), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
            assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before); /* foreign cue is rejected before entering firmware */
        }
    }
    cue_reset(0);
    put(n->play_pause, 8, (uint8_t) (0));
    __auto_type counted = cue_request(AZ_CMD_HOT_TRIGGER, 0, 1);
    __auto_type counted_out = result();
    assert(sdk_exit_public_submit("one", &counted, &counted_out) == AZ_OK);
    process(0);
    pending(counted);
    assert(n->cue_seeks && !n->play_pause.bytes[8] && get_uint32_t(n->beat.bytes, 0x104) == 3);
    for (unsigned count = 3; count > 0; --count) {
        process(0);
        assert(get_uint32_t(n->beat.bytes, 0x104) == count - 1);
        if (count > 1) {
            pending(counted);
            assert(!n->play_pause.bytes[8]);
        }
    }
    terminal(counted, AZ_COMMAND_APPLIED, AZ_OK);
    assert(n->play_pause.bytes[8] == 1 && n->play_pause.bytes[9] == 0);
    cue_reset(0);
    put(n->play_pause, 8, (uint8_t) (0));
    counted = cue_request(AZ_CMD_HOT_TRIGGER, 0, 1);
    counted_out = result();
    assert(sdk_exit_public_submit("one", &counted, &counted_out) == AZ_OK);
    process(0);
    pending(counted);
    advance_load(0);
    process(0);
    fenced(counted, AZ_STALE);
    process(0);
    process(0);
    terminal(counted, AZ_COMMAND_UNCERTAIN, AZ_STALE);
    cue_reset(0);
    n->deferred = true;
    __auto_type r = cue_request(AZ_CMD_HOT_TRIGGER, 0, 8);
    __auto_type out = result();
    unsigned seeks = n->cue_seeks;
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    pending(r);
    assert(n->cue_seeks == seeks); /* queued native work alone is not a receipt */
    process(0);
    pending(r);
    for (unsigned count = 3; count > 0; --count) {
        assert(get_uint32_t(n->beat.bytes, 0x104) == count);
        process(0);
        if (count > 1) pending(r);
    }
    terminal(r, AZ_COMMAND_APPLIED, AZ_OK);
    cue_reset(0);
    {
        __typeof__(cue_request(AZ_CMD_HOT_TRIGGER, 0, 0)) range_values_33[] = {cue_request(AZ_CMD_HOT_TRIGGER, 0, 0), cue_request(AZ_CMD_HOT_TRIGGER, 0, 9),
                                                                               cue_request(AZ_CMD_MAIN_RECALL, 0, 1)};
        for (unsigned range_index_33 = 0; range_index_33 < sizeof(range_values_33) / sizeof(range_values_33)[0]; ++range_index_33) {
            __auto_type invalid = range_values_33[range_index_33];

            out = result();
            assert(sdk_exit_public_submit("one", &invalid, &out) == AZ_OK);
            terminal(invalid, AZ_COMMAND_REJECTED, AZ_INVALID);
        }
    }
    cue_reset(3);
    put(natives[3].play_pause, 8, (uint8_t) (0));
    natives[3].cue_fault = 12;
    __auto_type unknown = cue_request(AZ_CMD_HOT_TRIGGER, 3, 1);
    __auto_type unknown_out = result();
    assert(sdk_exit_public_submit("one", &unknown, &unknown_out) == AZ_OK);
    for (unsigned i = 0; i < 8; ++i) {
        process(3);
        pending(unknown);
    }
    assert(get_uint32_t(natives[3].beat.bytes, 0x104) == 4 && !natives[3].play_pause.bytes[8]);
    /* Unknown native continuation stays fenced until process retirement. */
}
static void hot_countdown_retirement() {
    __auto_type n = &(natives[0]);
    cue_reset(0);
    put(n->play_pause, 8, (uint8_t) (0));
    __auto_type r = cue_request(AZ_CMD_HOT_TRIGGER, 0, 1);
    __auto_type out = result();
    assert(sdk_exit_public_submit("one", &r, &out) == AZ_OK);
    process(0);
    pending(r);
    azsdk_exit_retire(0);
    fenced(r, AZ_STALE);
    azsdk_exit_retire_complete(0, ((uintptr_t) natives[1].unit.bytes));
    fenced(r, AZ_STALE);
    azsdk_exit_retire_complete(0, ((uintptr_t) n->unit.bytes));
    terminal(r, AZ_COMMAND_UNCERTAIN, AZ_STALE);
}
#ifdef AZSDK_CUE_STAGE_TESTING
static bool hydrating_hot;
static unsigned hydrate_deck, hydrate_fault;
const void *azsdk_cue_recall_property(unsigned deck, uint64_t token) {
    assert(deck < 4 && token);
    return natives[deck].hot_properties[7].bytes;
}
int azsdk_cue_recall_dispatch(unsigned deck, uint64_t token, void *core, void *facade) {
    assert(deck < 4 && token && core == natives[deck].core.bytes && facade == natives[deck].facade.bytes);
    __auto_type n = &(natives[deck]);
    if (hydrating_hot) {
        assert(deck == hydrate_deck);
        memcpy(n->hot_properties[0].bytes + 8, n->hot_properties[7].bytes + 8, 0xf0);
        if (hydrate_fault == 1) point(n->retained_property.bytes, 0x18, 999999);
        if (hydrate_fault == 2) point(n->context.bytes + 0x160, 0x18, 999999);
        if (hydrate_fault == 3) put(n->play_pause, 8, (uint8_t) (!n->play_pause.bytes[8]));
        return AZ_OK;
    }
    // A persisted Active Loop can establish native setup before the point
    // recall cancels it. No replacement wrap should follow a Memory point.
    n->making_setup = true;
    maintain_loop(deck);
    n->making_setup = false;
    __auto_type cancel = ((uint32_t (*)(void *))(jump_table[5]));
    assert(cancel(n->jump.bytes) == 1);
    memcpy(n->retained_property.bytes + 8, n->hot_properties[7].bytes + 8, 0xf0);
    if (n->cue_fault != 14) {
        put(n->context, 0x190, (uint8_t) (0));
        put(n->context, 0x1e8, (uint8_t) (0));
    }
    native_cue_action(facade, AZ_CMD_MAIN_RECALL, 0);
    return AZ_OK;
}
static azexit_cue_request hot_stage_request(unsigned operation, unsigned d, unsigned kind, unsigned rgb) {
    __auto_type public_request = request(d);
    azexit_cue_request r = {0};
    r.size = sizeof r;
    r.operation = operation;
    r.identity = (azexit_request) {public_request.request_id, atomic_load_explicit(&(loads[d]), memory_order_seq_cst), UINT64_MAX, d, 0};
    r.flags = AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_PERSIST;
    r.classification = AZ_CUE_HOT;
    r.entry_id = kind;
    r.rgb = rgb;
    return r;
}
static void hot_stage_terminal(const azexit_cue_request r, unsigned outcome, int reason) {
    azexit_result out = {0};
    assert(azsdk_exit_recall_poll(r.identity.deck, &out) == AZ_OK);
    if (out.outcome != outcome || out.reason != reason)
        fprintf(stderr, "hot stage op=%u slot=%u got %u/%d expected %u/%d\n", r.operation, r.entry_id, out.outcome, out.reason, outcome, reason);
    assert(out.token == r.identity.token && out.outcome == outcome && out.reason == reason);
    assert(azsdk_exit_recall_quiescent(r.identity.deck, r.identity.token));
    assert(azsdk_exit_recall_release(r.identity.deck, r.identity.token) == AZ_OK);
}
static void hot_stage_complete(const azexit_cue_request r, unsigned outcome, int reason) {
    assert(azsdk_exit_cue_stage_submit(&r) == AZ_PENDING);
    assert(!azsdk_exit_recall_quiescent(r.identity.deck, r.identity.token));
    for (unsigned i = 0; i < 8; ++i) {
        process(r.identity.deck);
        azexit_result out = {0};
        if (azsdk_exit_recall_poll(r.identity.deck, &out) == AZ_OK) {
            hot_stage_terminal(r, outcome, reason);
            return;
        }
    }
    assert(false);
}
static void hot_stage_reset(unsigned d, bool loop, bool defined) {
    cue_reset(d);
    __auto_type n = &(natives[d]);
    put(n->core, 0x28, (uint64_t) (96000));
    if (!loop) put(n->context, 0x190, (uint8_t) (0));
    {
        for (unsigned range_index_34 = 0; range_index_34 < sizeof(n->hot_properties) / sizeof(n->hot_properties)[0]; ++range_index_34) {
            __auto_type property = &(n->hot_properties[range_index_34]);
            if (!defined) memset(property->bytes + 8, 0, sizeof property->bytes - 8);
        }
    }
}
static void memory_point_recall_commands() {
    for (unsigned d = 0; d < 4; ++d) {
        {
            unsigned range_values_35[] = {0u, 1u, 2u, 3u, 4u, 5u, 11u, 14u};
            for (unsigned range_index_35 = 0; range_index_35 < sizeof(range_values_35) / sizeof(range_values_35)[0]; ++range_index_35) {
                unsigned fault = range_values_35[range_index_35];

                hot_stage_reset(d, true, false);
                __auto_type n = &(natives[d]);
                __auto_type target = &(n->hot_properties[7]);
                point(target->bytes, 0x18, 96000);
                memcpy(target->bytes + 0x30, n->core.bytes + 0x18, 12);
                n->cue_fault = fault;
                unsigned cancelled = n->setup_cancels, seeks = n->cue_seeks;
                __auto_type r = hot_stage_request(AZ_CMD_MEMORY_RECALL, d, 1, 0);
                r.classification = AZ_CUE_MEMORY;
                r.flags = AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_NATIVE_CUE_IDS;
                r.expected_revision = 1;
                hot_stage_complete(r, fault ? AZ_COMMAND_UNCERTAIN : AZ_COMMAND_APPLIED, fault ? AZ_FAILED : AZ_OK);
                assert(n->setup_cancels == cancelled + 1 && !object_word(n->manager.bytes, 0) && !object_word(n->manager.bytes, 8));
                if (!fault) {
                    assert(n->cue_seeks == seeks + 1 && !n->play_pause.bytes[8] && !n->play_pause.bytes[9]);
                    unsigned rate = get_uint32_t(target->bytes, 0x60);
                    assert(compare_timestamps(get_uint64_t(n->core.bytes, 0x28), get_uint64_t(target->bytes, 0x40), rate, rate, 1) == AZ_TIME_EQUAL);
                }
            }
        }
    }
}
static void hot_stage_commands() {
    for (unsigned d = 0; d < 4; ++d) {
        __auto_type n = &(natives[d]);
        {
            bool range_values_36[] = {false, true};
            for (unsigned range_index_36 = 0; range_index_36 < sizeof(range_values_36) / sizeof(range_values_36)[0]; ++range_index_36) {
                bool deferred = range_values_36[range_index_36];
                {
                    bool range_values_37[] = {false, true};
                    for (unsigned range_index_37 = 0; range_index_37 < sizeof(range_values_37) / sizeof(range_values_37)[0]; ++range_index_37) {
                        bool loop = range_values_37[range_index_37];
                        {
                            unsigned range_values_38[] = {0u, 1u};
                            for (unsigned range_index_38 = 0; range_index_38 < sizeof(range_values_38) / sizeof(range_values_38)[0]; ++range_index_38) {
                                unsigned playing = range_values_38[range_index_38];

                                hot_stage_reset(d, loop, false);
                                n->deferred = deferred;
                                put(n->core, 9, (uint8_t) (playing));
                                put(n->play_pause, 8, (uint8_t) (playing));
                                unsigned kind = 1 + (d + (unsigned) (loop) * 3 + playing) % 8;
                                const unsigned rgb = 0x12a5e7;
                                __auto_type r = hot_stage_request(AZ_CMD_HOT_SET, d, kind, rgb);
                                hot_stage_complete(r, AZ_COMMAND_APPLIED, AZ_OK);
                                __auto_type p = &(n->hot_properties[kind - 1]);
                                assert(sample_at(get_uint64_t(p->bytes, 0x40), loop ? 48000u : 96000u, get_uint32_t(p->bytes, 0x60)));
                                assert((p->bytes[0x88] != 0) == loop);
                                if (loop) assert(sample_at(get_uint64_t(p->bytes, 0x98), 144000u, get_uint32_t(p->bytes, 0xb8)));
                                assert(p->bytes[0x10] == 0x12 && p->bytes[0x11] == 0xa5 && p->bytes[0x12] == 0xe7);
                                assert(n->core.bytes[9] == playing && n->play_pause.bytes[8] == playing && sample_at(get_uint64_t(n->core.bytes, 0x28), 96000u, get_uint32_t(n->core.bytes, 0x68)));
                                r = hot_stage_request(AZ_CMD_HOT_CLEAR, d, kind, 0);
                                hot_stage_complete(r, AZ_COMMAND_APPLIED, AZ_OK);
                                assert(!p->bytes[0x30] && !p->bytes[0x88] && n->core.bytes[9] == playing && n->play_pause.bytes[8] == playing);
                                for (unsigned other = 1; other <= 8; ++other)
                                    if (other != kind) assert(!n->hot_properties[other - 1].bytes[0x30]);
                            }
                        }
                    }
                }
            }
        }
    }
    __auto_type n = &(natives[0]);
    {
        bool range_values_39[] = {false, true};
        for (unsigned range_index_39 = 0; range_index_39 < sizeof(range_values_39) / sizeof(range_values_39)[0]; ++range_index_39) {
            bool deferred = range_values_39[range_index_39];
            {
                bool range_values_40[] = {false, true};
                for (unsigned range_index_40 = 0; range_index_40 < sizeof(range_values_40) / sizeof(range_values_40)[0]; ++range_index_40) {
                    bool loop = range_values_40[range_index_40];

                    hot_stage_reset(0, loop, false);
                    __auto_type existing = &(n->hot_properties[0]);
                    if (loop) {
                        uintptr_t table = object_word(existing->bytes, 0);
                        memcpy(existing->bytes, n->context.bytes + 0x160, sizeof existing->bytes);
                        put((*existing), 0, table);
                    } else {
                        point(existing->bytes, 0x18, 96000);
                        memcpy(existing->bytes + 0x30, n->core.bytes + 0x18, 12);
                    }
                    put((*existing), 0x10, (uint8_t) (0x11));
                    put((*existing), 0x11, (uint8_t) (0x22));
                    put((*existing), 0x12, (uint8_t) (0x33));
                    uint64_t in_before = get_uint64_t(existing->bytes, 0x40);
                    n->deferred = deferred;
                    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_SET, 0, 1, 0xaabbcc), AZ_COMMAND_APPLIED, AZ_OK);
                    assert(sample_at(get_uint64_t(existing->bytes, 0x40), in_before, get_uint32_t(existing->bytes, 0x60)) && existing->bytes[0x10] == 0xaa && existing->bytes[0x11] == 0xbb && existing->bytes[0x12] == 0xcc);
                    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_SET, 0, 1, 0xaabbcc), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
                }
            }
        }
    }
    hot_stage_reset(0, false, false);
    put(n->play_pause, 8, (uint8_t) (0));
    put(n->play_pause, 9, (uint8_t) (1));
    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_SET, 0, 1, 0x112233), AZ_COMMAND_APPLIED, AZ_OK);
    assert(n->play_pause.bytes[8] == 0 && n->play_pause.bytes[9] == 1);
    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_CLEAR, 0, 1, 0), AZ_COMMAND_APPLIED, AZ_OK);
    assert(n->play_pause.bytes[8] == 0 && n->play_pause.bytes[9] == 1);
    hot_stage_reset(0, false, false);
    __auto_type r = hot_stage_request(AZ_CMD_HOT_SET, 0, 8, 0xabcdef);
    assert(azsdk_exit_cue_stage_submit(&r) == AZ_PENDING);
    __auto_type original = r;
    r.entry_id = 2;
    r.rgb = 0x010203;
    r.operation = AZ_CMD_HOT_CLEAR;
    process(0);
    hot_stage_terminal(original, AZ_COMMAND_APPLIED, AZ_OK);
    assert(n->hot_properties[7].bytes[0x10] == 0xab && n->hot_properties[7].bytes[0x11] == 0xcd && n->hot_properties[7].bytes[0x12] == 0xef);
    assert(!n->hot_properties[1].bytes[0x30]); /* scalar request was copied before returning */
    {
        unsigned range_values_41[] = {(unsigned) (AZ_CMD_HOT_SET), (unsigned) (AZ_CMD_HOT_CLEAR)};
        for (unsigned range_index_41 = 0; range_index_41 < sizeof(range_values_41) / sizeof(range_values_41)[0]; ++range_index_41) {
            unsigned operation = range_values_41[range_index_41];

            hot_stage_reset(0, false, true);
            exit_noop = true;
            hot_stage_complete(hot_stage_request(operation, 0, 2, operation == AZ_CMD_HOT_SET ? 0x112233 : 0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
            exit_noop = false;
        }
    }
    {
        bool range_values_42[] = {false, true};
        for (unsigned range_index_42 = 0; range_index_42 < sizeof(range_values_42) / sizeof(range_values_42)[0]; ++range_index_42) {
            bool deferred = range_values_42[range_index_42];

            {
                unsigned range_values_43[] = {2u, 6u, 7u, 8u, 9u};
                for (unsigned range_index_43 = 0; range_index_43 < sizeof(range_values_43) / sizeof(range_values_43)[0]; ++range_index_43) {
                    unsigned fault = range_values_43[range_index_43];

                    hot_stage_reset(0, fault == 7, false);
                    n->cue_fault = fault;
                    n->deferred = deferred;
                    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_SET, 0, 3, 0xabcdef), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
                }
            }
            hot_stage_reset(0, false, true);
            n->cue_fault = 8;
            n->deferred = deferred;
            hot_stage_complete(hot_stage_request(AZ_CMD_HOT_CLEAR, 0, 3, 0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
        }
    }
    hot_stage_reset(0, true, false);
    put(n->context, 0x160 + 0x88, (uint8_t) (0)); /* engaged but incomplete native loop cannot be copied */
    unsigned before_incomplete = atomic_load_explicit(&(calls), memory_order_seq_cst);
    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_SET, 0, 3, 0xabcdef), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before_incomplete);
    hot_stage_reset(0, false, true);
    n->hot_properties[0].bytes[0x30] ^= 2;
    unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_CLEAR, 0, 1, 0), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    hot_stage_reset(0, false, false);
    uintptr_t binding = object_word(n->facade.bytes, 0xc8 + 0x10);
    put(n->facade, 0xc8 + 0x10, (uintptr_t) (0));
    before = atomic_load_explicit(&(calls), memory_order_seq_cst);
    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_SET, 0, 1, 0xabcdef), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    put(n->facade, 0xc8 + 0x10, binding);
    hot_stage_reset(0, false, false);
    r = hot_stage_request(AZ_CMD_HOT_SET, 0, 1, 0xabcdef);
    assert(azsdk_exit_cue_stage_submit(&r) == AZ_PENDING);
    assert(azsdk_exit_recall_cancel(0, r.identity.token) == AZ_OK);
    hot_stage_terminal(r, AZ_COMMAND_CANCELLED, AZ_CANCELLED);
    process(0);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
    hot_stage_reset(0, false, false);
    n->deferred = true;
    r = hot_stage_request(AZ_CMD_HOT_SET, 0, 4, 0x123456);
    assert(azsdk_exit_cue_stage_submit(&r) == AZ_PENDING);
    process(0);
    azexit_result out = {0};
    assert(azsdk_exit_recall_poll(0, &out) == AZ_PENDING);
    assert(!azsdk_exit_recall_quiescent(0, r.identity.token));
    assert(azsdk_exit_recall_cancel(0, r.identity.token) == AZ_PENDING);
    assert(azsdk_exit_recall_release(0, r.identity.token) == AZ_BUSY);
    process(0);
    hot_stage_terminal(r, AZ_COMMAND_APPLIED, AZ_OK);
    n->deferred = false;
    hot_stage_reset(0, false, false);
    r = hot_stage_request(AZ_CMD_HOT_SET, 0, 1, 0xabcdef);
    before = atomic_load_explicit(&(calls), memory_order_seq_cst);
    assert(azsdk_exit_cue_stage_submit(&r) == AZ_PENDING);
    advance_load(0);
    process(0);
    hot_stage_terminal(r, AZ_COMMAND_REJECTED, AZ_STALE);
    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
}
static void hot_clear_commands() {
    for (unsigned d = 0; d < 4; ++d) {
        bool range_values_44[] = {false, true};
        for (unsigned range_index_44 = 0; range_index_44 < sizeof(range_values_44) / sizeof(range_values_44)[0]; ++range_index_44) {
            bool deferred = range_values_44[range_index_44];
            {
                unsigned range_values_45[] = {0u, 1u};
                for (unsigned range_index_45 = 0; range_index_45 < sizeof(range_values_45) / sizeof(range_values_45)[0]; ++range_index_45) {
                    unsigned playing = range_values_45[range_index_45];

                    hot_stage_reset(d, false, true);
                    __auto_type n = &(natives[d]);
                    n->deferred = deferred;
                    put(n->core, 9, (uint8_t) (playing));
                    put(n->play_pause, 8, (uint8_t) (playing));
                    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_CLEAR, d, 8, 0), AZ_COMMAND_APPLIED, AZ_OK);
                    assert(!n->hot_properties[7].bytes[0x30] && n->play_pause.bytes[8] == playing);
        /* A stored Hot H can exist while the live slot is the native empty
         * constructor value. Clear still reaches the native facade; persistence
         * separately owns the stored property and proves its deletion. */
                    {
                        unsigned range_values_46[] = {0x30u, 0x88u};
                        for (unsigned range_index_46 = 0; range_index_46 < sizeof(range_values_46) / sizeof(range_values_46)[0]; ++range_index_46) {
                            unsigned offset = range_values_46[range_index_46];

                            __auto_type property = n->hot_properties[7].bytes;
                            memset(property + offset, 0, 0x58);
                            store(property, offset + 0x10, UINT64_C(0x7fffffffffffffff));
                            store(property, offset + 0x30, UINT32_C(0x7fffffff));
                            store(property, offset + 0x38, UINT64_C(0x7fffffffffffffff));
                        }
                    }
                    unsigned before = atomic_load_explicit(&(calls), memory_order_seq_cst);
                    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_CLEAR, d, 8, 0), AZ_COMMAND_APPLIED, AZ_OK);
                    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) > before && n->play_pause.bytes[8] == playing);
                    hot_stage_reset(d, false, true);
                    n->cue_fault = 8;
                    n->deferred = deferred;
                    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_CLEAR, d, 8, 0), AZ_COMMAND_UNCERTAIN, AZ_FAILED);
                    hot_stage_reset(d, false, true);
                    n->hot_properties[7].bytes[0x30] ^= 2;
                    before = atomic_load_explicit(&(calls), memory_order_seq_cst);
                    hot_stage_complete(hot_stage_request(AZ_CMD_HOT_CLEAR, d, 8, 0), AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
                    assert(atomic_load_explicit(&(calls), memory_order_seq_cst) == before);
                }
            }
        }
    }
}
static void arbitrary_hot_hydration() {
    for (unsigned d = 0; d < 4; ++d) {
        bool range_values_47[] = {false, true};
        for (unsigned range_index_47 = 0; range_index_47 < sizeof(range_values_47) / sizeof(range_values_47)[0]; ++range_index_47) {
            bool loop = range_values_47[range_index_47];
            {
                unsigned range_values_48[] = {0u, 1u, 2u, 3u};
                for (unsigned range_index_48 = 0; range_index_48 < sizeof(range_values_48) / sizeof(range_values_48)[0]; ++range_index_48) {
                    unsigned fault = range_values_48[range_index_48];

                    hot_stage_reset(d, loop, false);
                    __auto_type n = &(natives[d]);
                    n->deferred = false;
                    point(n->hot_properties[7].bytes, 0x18, 48000 * 20);
                    if (loop) point(n->hot_properties[7].bytes, 0x70, 48000 * 24);
                    n->hot_properties[7].bytes[0x10] = 0x12;
                    n->hot_properties[7].bytes[0x11] = 0x34;
                    n->hot_properties[7].bytes[0x12] = 0x56;
                    uint64_t head = get_uint64_t(n->core.bytes, 0x28);
                    __auto_type r = hot_stage_request(loop ? AZ_CMD_CUE_LOOP_SET : AZ_CMD_CUE_POINT_SET, d, 1, 0x123456);
                    hydrating_hot = true;
                    hydrate_deck = d;
                    hydrate_fault = fault;
                    hot_stage_complete(r, fault ? AZ_COMMAND_UNCERTAIN : AZ_COMMAND_APPLIED, fault ? AZ_FAILED : AZ_OK);
                    assert(compare_timestamps(get_uint64_t(n->core.bytes, 0x28), head, 48000, 48000, 1) == AZ_TIME_EQUAL);
                    hydrating_hot = false;
                }
            }
        }
    }
    for (unsigned d = 0; d < 4; ++d) {
        bool range_values_49[] = {false, true};
        for (unsigned range_index_49 = 0; range_index_49 < sizeof(range_values_49) / sizeof(range_values_49)[0]; ++range_index_49) {
            bool stale = range_values_49[range_index_49];

            hot_stage_reset(d, false, false);
            __auto_type n = &(natives[d]);
            n->deferred = false;
            point(n->hot_properties[7].bytes, 0x18, 48000 * 20);
            __auto_type r = hot_stage_request(AZ_CMD_CUE_POINT_SET, d, 1, 0x123456);
            unsigned char before[0xf8];
            memcpy(before, n->hot_properties[0].bytes, sizeof before);
            hydrating_hot = true;
            hydrate_deck = d;
            hydrate_fault = 0;
            if (stale) {
                assert(azsdk_exit_cue_stage_submit(&r) == AZ_PENDING);
                advance_load(d);
                process(d);
                hot_stage_terminal(r, AZ_COMMAND_REJECTED, AZ_STALE);
            } else {
                n->hot_properties[7].bytes[0x30] ^= 2;
                hot_stage_complete(r, AZ_COMMAND_REJECTED, AZ_UNSUPPORTED);
            }
            assert(!memcmp(before, n->hot_properties[0].bytes, sizeof before));
            hydrating_hot = false;
        }
    }
}
struct CaptureReader {
    _Atomic(bool) *new_ready, *stop;
    _Atomic(unsigned) *reads;
    azexit_cue_request old;
    azexit_cue_stage_sample first;
};
static void *capture_reader(void *opaque) {
    struct CaptureReader *ctx = opaque;
    while (!atomic_load_explicit(ctx->stop, memory_order_acquire)) {
        bool newer = atomic_load_explicit(ctx->new_ready, memory_order_acquire);
        azexit_cue_stage_sample copied = {0};
        int rc = azsdk_exit_cue_stage_capture(0, ctx->old.identity.token, &copied);
        if (newer) {
            assert(rc != AZ_OK);
            atomic_fetch_add_explicit(ctx->reads, 1, memory_order_release);
        } else if (rc == AZ_OK) {
            assert(copied.sample_rate == ctx->first.sample_rate && !memcmp(copied.source_id, ctx->first.source_id, 12));
            assert(compare_timestamps(copied.in_sample, ctx->first.in_sample, ctx->first.sample_rate, ctx->first.sample_rate, 1) == AZ_TIME_EQUAL);
        }
    }
    return NULL;
}
static void hot_set_capture_commands() {
    for (unsigned d = 0; d < 4; ++d) {
        bool range_values_50[] = {false, true};
        for (unsigned range_index_50 = 0; range_index_50 < sizeof(range_values_50) / sizeof(range_values_50)[0]; ++range_index_50) {
            bool deferred = range_values_50[range_index_50];
            {
                bool range_values_51[] = {false, true};
                for (unsigned range_index_51 = 0; range_index_51 < sizeof(range_values_51) / sizeof(range_values_51)[0]; ++range_index_51) {
                    bool loop = range_values_51[range_index_51];
                    {
                        unsigned range_values_52[] = {0u, 1u};
                        for (unsigned range_index_52 = 0; range_index_52 < sizeof(range_values_52) / sizeof(range_values_52)[0]; ++range_index_52) {
                            unsigned playing = range_values_52[range_index_52];

                            hot_stage_reset(d, loop, false);
                            __auto_type n = &(natives[d]);
                            n->deferred = deferred;
                            put(n->core, 9, (uint8_t) (playing));
                            put(n->play_pause, 8, (uint8_t) (playing));
                            __auto_type r = hot_stage_request(AZ_CMD_HOT_SET, d, 8, 0x12a5e7);
                            azexit_cue_stage_sample captured = {0};
                            assert(azsdk_exit_cue_stage_submit(&r) == AZ_PENDING);
                            assert(azsdk_exit_cue_stage_capture(d, r.identity.token, &captured) == AZ_PENDING);
                            azexit_result out = {0};
                            for (unsigned i = 0; i < 8 && azsdk_exit_recall_poll(d, &out) != AZ_OK; ++i) process(d);
                            assert(azsdk_exit_cue_stage_capture(d, r.identity.token + 1, &captured) == AZ_STALE);
                            assert(azsdk_exit_cue_stage_capture(d, r.identity.token, &captured) == AZ_OK);
                            __auto_type property = &(n->hot_properties[7]);
                            unsigned rate = get_uint32_t(property->bytes, 0x60);
                            uint64_t in = get_uint64_t(property->bytes, 0x40);
                            assert(captured.sample_rate == rate && captured.out_valid == (unsigned) (loop));
                            assert(!memcmp(captured.source_id, property->bytes + 0x30, 12));
                            assert(compare_timestamps(captured.in_sample, in, rate, rate, 1) == AZ_TIME_EQUAL);
                            if (loop) assert(compare_timestamps(captured.out_sample, get_uint64_t(property->bytes, 0x98), rate, rate, 1) == AZ_TIME_EQUAL);
        // Reading the receipt copies the value captured by its owner frame,
        // even if the live slot has since changed.
                            put((*property), 0x40, in + 128);
                            azexit_cue_stage_sample again = {0};
                            assert(azsdk_exit_cue_stage_capture(d, r.identity.token, &again) == AZ_OK);
                            assert(compare_timestamps(again.in_sample, in, rate, rate, 1) == AZ_TIME_EQUAL);
                            put((*property), 0x40, in);
                            hot_stage_terminal(r, AZ_COMMAND_APPLIED, AZ_OK);
                            assert(azsdk_exit_cue_stage_capture(d, r.identity.token, &captured) == AZ_PENDING);
                            assert(n->play_pause.bytes[8] == playing);
                        }
                    }
                }
            }
        }
    }
    hot_stage_reset(0, false, false);
    natives[0].deferred = false;
    __auto_type old = hot_stage_request(AZ_CMD_HOT_SET, 0, 8, 0x112233);
    assert(azsdk_exit_cue_stage_submit(&old) == AZ_PENDING);
    process(0);
    azexit_cue_stage_sample first = {0};
    assert(azsdk_exit_cue_stage_capture(0, old.identity.token, &first) == AZ_OK);
    _Atomic(bool) new_ready = false, stop = false;
    _Atomic(unsigned) reads = 0;
    struct CaptureReader reader_state = {&new_ready, &stop, &reads, old, first};
    pthread_t reader;
    assert(!pthread_create(&reader, NULL, capture_reader, &reader_state));
    int released;
    do {
        released = azsdk_exit_recall_release(0, old.identity.token);
        sched_yield();
    } while (released == AZ_BUSY);
    assert(released == AZ_OK);
    __auto_type next = hot_stage_request(AZ_CMD_HOT_SET, 0, 8, 0x445566);
    assert(azsdk_exit_cue_stage_submit(&next) == AZ_PENDING);
    process(0);
    atomic_store_explicit(&(new_ready), true, memory_order_release);
    while (!atomic_load_explicit(&(reads), memory_order_acquire)) sched_yield();
    atomic_store_explicit(&(stop), true, memory_order_release);
    assert(!pthread_join(reader, NULL));
    hot_stage_terminal(next, AZ_COMMAND_APPLIED, AZ_OK);
}
static bool capture_persistent_route;
static unsigned persistent_route_calls;
static azexit_cue_request persistent_route_request;
int azsdk_exit_test_persistent_cue_submit(const azexit_cue_request *r) {
    if (!capture_persistent_route) return AZ_INVALID;
    persistent_route_request = *r;
    ++persistent_route_calls;
    return AZ_PENDING; // Selection proof only; no native effect is simulated.
}
static void arbitrary_cue_public_routing() {
    __auto_type api = azstem_exit_service_v1();
    capture_persistent_route = true;
    for (unsigned deck = 0; deck < 4; ++deck) {
        unsigned range_values_53[] = {AZ_CUE_HOT, AZ_CUE_MEMORY};
        for (unsigned range_index_53 = 0; range_index_53 < sizeof(range_values_53) / sizeof(range_values_53)[0]; ++range_index_53) {
            unsigned classification = range_values_53[range_index_53];
            {
                unsigned range_values_54[] = {AZ_CMD_CUE_POINT_SET, AZ_CMD_CUE_LOOP_SET};
                for (unsigned range_index_54 = 0; range_index_54 < sizeof(range_values_54) / sizeof(range_values_54)[0]; ++range_index_54) {
                    unsigned operation = range_values_54[range_index_54];

                    hot_stage_reset(deck, false, false);
                    __auto_type r = hot_stage_request(operation, deck, classification == AZ_CUE_HOT ? 2 : 0, 0x00c3ff);
                    r.flags = AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_NATIVE_CUE_IDS;
                    r.expected_revision = 19;
                    r.classification = classification;
                    r.position_s = operation == AZ_CMD_CUE_POINT_SET ? 0 : 12.25;
                    r.end_s = operation == AZ_CMD_CUE_LOOP_SET ? 15.5 : 0;
                    strcpy(r.comment, "Create UTF8 \342\234\223");
                    const unsigned before = persistent_route_calls;
                    assert(api->submit_cue(&r) == AZ_PENDING && persistent_route_calls == before + 1);
                    __auto_type forwarded = &(persistent_route_request);
                    assert(forwarded->operation == r.operation && forwarded->classification == r.classification &&
                           forwarded->flags == r.flags && forwarded->expected_revision == r.expected_revision &&
                           forwarded->entry_id == r.entry_id && !strcmp(forwarded->comment, r.comment));
                    __auto_type rate = get_uint32_t(natives[deck].core.bytes, 0x68);
                    assert(compare_timestamps(forwarded->position_s, r.position_s, 1, rate, 1) == AZ_TIME_EQUAL &&
                           compare_timestamps(forwarded->end_s, r.end_s, 1, rate, 1) == AZ_TIME_EQUAL);
                }
            }
        }
    }
    capture_persistent_route = false;
}
#endif
static void loop_only_gating() {
    __auto_type api = azstem_exit_service_v1();
    assert(api->mask() == 0);
    __auto_type out = result();
    {
        __typeof__(request(0)) range_values_55[] = {request(0), reloop_request(0)};
        for (unsigned range_index_55 = 0; range_index_55 < sizeof(range_values_55) / sizeof(range_values_55)[0]; ++range_index_55) {
            __auto_type r = range_values_55[range_index_55];
            assert(sdk_exit_public_submit("one", &r, &out) == AZ_UNSUPPORTED);
        }
    }
    azexit_request legacy = {999, atomic_load_explicit(&(loads[0]), memory_order_seq_cst), UINT64_MAX, 0, 0};
    assert(api->submit(&legacy) == AZ_UNSUPPORTED);
    assert(api->submit_command(&legacy, AZ_CMD_RELOOP) == AZ_UNSUPPORTED);
    loop_points(0, 48000, 144000);
    active(0);
    loop_complete(loop_request(AZ_CMD_LOOP_DOUBLE, 0), AZ_COMMAND_APPLIED, AZ_OK);
}
#define NATIVE_CASE(test)                               \
    do {                                                \
        fprintf(stderr, "native fixture: %s\n", #test); \
        test;                                           \
    } while (0)
int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    unsigned enabled = argc == 3 ? (unsigned) (strtoul(argv[2], NULL, 10)) : AZEXIT_ENABLE_EXIT | AZEXIT_ENABLE_RELOOP;
    setup(argv[1][0] == '1' ? 1 : 2, enabled);
#ifdef AZSDK_CUE_STAGE_TESTING
#ifdef AZSDK_MEMORY_RECALL_ONLY
    memory_point_recall_commands();
#elif defined(AZSDK_HOT_CLEAR_ONLY)
    hot_clear_commands();
#elif defined(AZSDK_HOT_SET_ONLY)
    hot_set_capture_commands();
#else
    arbitrary_cue_public_routing();
    arbitrary_hot_hydration();
    memory_point_recall_commands();
    hot_stage_commands();
    hot_set_capture_commands();
#endif
    for (int d = 0; d < 4; d++) azsdk_exit_retire(d);
#ifdef AZSDK_MEMORY_RECALL_ONLY
    puts("PASS native DSP Memory point recall: cancelled Active Loop setup, correlated seek/pause and failure controls");
#elif defined(AZSDK_HOT_CLEAR_ONLY)
    puts("PASS native DSP HOT_CLEAR: occupied/empty live slots, transport, deferred work and wrong-source refusal");
#elif defined(AZSDK_HOT_SET_ONLY)
    puts("PASS native DSP HOT_SET: copied source-bound receipt, token fence, transport and loop/point capture");
#else
    puts("PASS native DSP hot-cue stage: copied requests, loop/point capture, deferred graph and cancellation");
#endif
    return 0;
#endif
    if (argc == 3 && (enabled & AZEXIT_ENABLE_CUE)) {
        NATIVE_CASE(cue_commands());
        NATIVE_CASE(hot_countdown_retirement());
        for (int d = 0; d < 4; d++) azsdk_exit_retire(d);
        assert(!sdk_exit_public_capabilities());
        puts("PASS native async main and hot cue controls");
        return 0;
    }
    if (argc == 3 && enabled == 4) {
        loop_only_gating();
        for (int d = 0; d < 4; d++) azsdk_exit_retire(d);
        assert(!sdk_exit_public_capabilities());
        puts("PASS native async loop-only capability gating");
        return 0;
    }
    if (argc == 3 && enabled == 7) {
        NATIVE_CASE(loop_commands());
        NATIVE_CASE(recurring_loop_commands());
        NATIVE_CASE(preparatory_loop_commands());
        NATIVE_CASE(quantized_loop_commands());
        for (int d = 0; d < 4; d++) azsdk_exit_retire(d);
        assert(!sdk_exit_public_capabilities());
        puts("PASS native async loop control and adjustments");
        return 0;
    }
    if (argc == 3) {
        capability_gating(enabled);
        for (int d = 0; d < 4; d++) azsdk_exit_retire(d);
        assert(!sdk_exit_public_capabilities());
        puts("PASS native async command capability gating");
        return 0;
    }
    NATIVE_CASE(happy());
    NATIVE_CASE(asynchronous(false));
    NATIVE_CASE(asynchronous(true));
    NATIVE_CASE(retained_after_execution());
    NATIVE_CASE(borrowed_pending_repurposed());
    NATIVE_CASE(borrowed_pending_replaced());
    NATIVE_CASE(noop_preserves_borrowed_work());
    NATIVE_CASE(replacement_and_incarnation());
    NATIVE_CASE(child_exception());
    NATIVE_CASE(delayed_deadline());
    NATIVE_CASE(loss_interference());
    NATIVE_CASE(unrelated_command_passthrough());
    NATIVE_CASE(fifo_owners());
    NATIVE_CASE(guards());
    NATIVE_CASE(dormant_retirement());
    NATIVE_CASE(reloop_happy());
    NATIVE_CASE(reloop_async(false));
    NATIVE_CASE(reloop_async(true));
    NATIVE_CASE(reloop_stale_pending());
    NATIVE_CASE(reloop_pending_replaced());
    NATIVE_CASE(mixed_fifo());
    NATIVE_CASE(legacy_service());
    NATIVE_CASE(lifecycle());
    for (int d = 0; d < 4; d++) azsdk_exit_retire(d);
    assert(!sdk_exit_public_capabilities());
    puts("PASS native async EXIT/RELOOP: deferred chains, existing Quantize, disposal, mixed FIFO, capability gates and legacy bridge");
}
