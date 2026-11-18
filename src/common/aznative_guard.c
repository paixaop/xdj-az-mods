/* SPDX-License-Identifier: GPL-3.0-only */
/* C error boundary for firmware calls. The C unwinder runs every intervening
 * cleanup before this frame translates a native failure into a zero status.
 * Only this library's C frames bind to the hidden personality below. Other C
 * frames retain libgcc's cleanup behavior, and forced unwinds are never caught.
 * The firmware still owns its exception objects: its C ABI catch functions
 * maintain the runtime's exception bookkeeping and destroy the payload. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <unwind.h>
#ifndef __EXCEPTIONS
#error "The native C boundary requires -fexceptions for compiler-generated cleanup landing pads"
#endif
int az_native_try(void (*fn)(void *), void *argument);
struct frame {
    jmp_buf jump;
    struct frame *previous;
    volatile int handling;
    struct _Unwind_Exception *volatile error;
};
static _Thread_local struct frame *active;
typedef _Unwind_Reason_Code (*personality_fn)(int, _Unwind_Action, _Unwind_Exception_Class, struct _Unwind_Exception *, struct _Unwind_Context *);
static _Thread_local personality_fn real_personality;
static personality_fn resolve_personality(void) {
    int saved = errno;
    personality_fn fn = (personality_fn) dlsym(RTLD_NEXT, "__gcc_personality_v0");
    errno = saved;
    if (!fn) abort();
    return fn;
}
extern void *__cxa_begin_catch(void *);
extern void __cxa_end_catch(void);
__attribute__((visibility("hidden")))
_Unwind_Reason_Code
__gcc_personality_v0(int version, _Unwind_Action actions, _Unwind_Exception_Class kind, struct _Unwind_Exception *error, struct _Unwind_Context *context) {
    if (version != 1) return _URC_FATAL_PHASE1_ERROR;
    struct frame *f = active;
    if (f && !(actions & _UA_FORCE_UNWIND) && (uintptr_t) az_native_try == _Unwind_GetRegionStart(context)) {
        /* Keep this guard active until payload destruction finishes. A failure
         * inside that runtime cleanup must terminate, like the native handler. */
        if (f->handling) return _URC_FATAL_PHASE1_ERROR;
        if (actions & _UA_SEARCH_PHASE) return _URC_HANDLER_FOUND;
        if (actions & _UA_HANDLER_FRAME) f->error = error;
    }
    personality_fn fn = real_personality;
    if (!fn) fn = resolve_personality();
    return fn(version, actions, kind, error, context);
}
/* The jump happens only after phase-two unwinding reaches this cleanup.
 * Jumping at the throw site would skip native destructors and C cleanups. */
static void release(struct frame *f) {
    if (f->handling) abort();
    if (f->error) {
        f->handling = 1;
        int saved = errno;
        __cxa_begin_catch(f->error);
        __cxa_end_catch();
        errno = saved;
        f->error = NULL;
        f->handling = 0;
        active = f->previous;
        longjmp(f->jump, 1);
    }
    active = f->previous;
}
__attribute__((noinline, visibility("hidden"))) int az_native_try(void (*fn)(void *), void *argument) {
    if (!real_personality) real_personality = resolve_personality();
    struct frame f __attribute__((cleanup(release))) = {.previous = active};
    if (setjmp(f.jump)) {
        f.error = NULL;
        return 0;
    }
    active = &f;
    fn(argument);
    return 1;
}

/* A C aggregate larger than two words uses the native indirect-result register
 * (x8 on AAPCS64; the first argument on SysV x86-64). The firmware writes only
 * its documented one- or two-word prefix. Never read the unused carrier bytes.
 * These are caller adapters, not native entry points: returning this carrier
 * to an eight-byte firmware destination would overwrite that destination. */
struct native_result_storage {
    uintptr_t first;
    uintptr_t second;
    unsigned char unused[8];
};
_Static_assert(sizeof(struct native_result_storage) == 24, "indirect native result ABI");
__attribute__((visibility("hidden"))) void az_native_result_word(void *function, void *result, const void *argument) {
    struct native_result_storage value = ((struct native_result_storage (*)(const void *)) function)(argument);
    __builtin_memcpy(result, &value.first, sizeof value.first);
}
__attribute__((visibility("hidden"))) void az_native_result_pair(void *function, void *result, const void *argument) {
    struct native_result_storage value = ((struct native_result_storage (*)(const void *)) function)(argument);
    __builtin_memcpy(result, &value, 2 * sizeof(uintptr_t));
}
