/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/common/aznative_c.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unwind.h>

static _Thread_local unsigned cleaned, disposed;
static void *int_type;
extern void *__cxa_allocate_exception(size_t);
extern void __cxa_throw(void *, void *, void (*)(void *)) __attribute__((noreturn));
/* The native runtime exposes its thread's exception bookkeeping through a C ABI. */
extern void *__cxa_get_globals(void);
struct native_exception_globals {
    void *caught;
    unsigned uncaught;
};
static unsigned uncaught(void) {
    struct native_exception_globals *state = __cxa_get_globals();
    return state->uncaught;
}
static void cleanup(unsigned *count) { cleaned += *count; }
static void destroy_native(void *payload) {
    assert(*(int *) payload == 42);
    assert(uncaught() == 0);
    ++disposed;
    errno = ENOMEM;
}
static void destroy_foreign(_Unwind_Reason_Code reason, struct _Unwind_Exception *payload) {
    (void) reason;
    ++disposed;
    free(payload);
    errno = ENOMEM;
}
static void raise_native(void *argument) {
    (void) argument;
    unsigned count __attribute__((cleanup(cleanup))) = 1;
    int *payload = __cxa_allocate_exception(sizeof *payload);
    assert(payload);
    *payload = 42;
    errno = EIO;
    __cxa_throw(payload, int_type, destroy_native);
}
static void destroy_reentrant(void *payload) {
    destroy_native(payload);
    assert(!az_native_try(raise_native, NULL));
}
static void raise_reentrant(void *argument) {
    (void) argument;
    int *payload = __cxa_allocate_exception(sizeof *payload);
    assert(payload);
    *payload = 42;
    errno = EIO;
    __cxa_throw(payload, int_type, destroy_reentrant);
}
static void raise_foreign(void *argument) {
    (void) argument;
    unsigned count __attribute__((cleanup(cleanup))) = 1;
    struct _Unwind_Exception *payload = calloc(1, sizeof *payload);
    assert(payload);
    payload->exception_class = UINT64_C(0x415a544553544300);
    payload->exception_cleanup = destroy_foreign;
    errno = EIO;
    (void) _Unwind_RaiseException(payload);
    abort();
}
static void nested(void *argument) {
    unsigned count __attribute__((cleanup(cleanup))) = 1;
    unsigned before = cleaned, deleted = disposed;
    assert(!az_native_try(raise_native, argument));
    assert(cleaned == before + 1 && disposed == deleted + 1);
    assert(uncaught() == 0 && errno == EIO);
    raise_foreign(argument);
}
static void normal(void *argument) {
    unsigned count __attribute__((cleanup(cleanup))) = 1;
    assert(argument == &cleaned);
    assert(errno == ERANGE);
}
static void *worker(void *unused) {
    (void) unused;
    for (unsigned i = 0; i < 100; ++i) {
        unsigned before = cleaned, deleted = disposed;
        errno = ERANGE;
        assert(az_native_try(normal, &cleaned));
        assert(errno == ERANGE && cleaned == before + 1);
        assert(!az_native_try(nested, NULL));
        assert(cleaned == before + 4 && disposed == deleted + 2);
        assert(errno == EIO && uncaught() == 0);
    }
    unsigned before = cleaned, deleted = disposed;
    assert(!az_native_try(raise_reentrant, NULL));
    assert(cleaned == before + 1 && disposed == deleted + 2);
    assert(errno == EIO && uncaught() == 0);
    return NULL;
}
static unsigned exit_cleaned;
static void exit_cleanup(unsigned *value) { __atomic_fetch_add(&exit_cleaned, *value, __ATOMIC_RELAXED); }
static void exit_inside_guard(void *unused) {
    (void) unused;
    unsigned count __attribute__((cleanup(exit_cleanup))) = 1;
    pthread_exit((void *) (uintptr_t) 37);
}
static void *exit_worker(void *unused) {
    az_native_try(exit_inside_guard, unused);
    abort(); /* Forced unwind must reach pthread_exit, never become a zero status. */
}
int main(void) {
    int_type = dlsym(RTLD_DEFAULT, "_ZTIi");
    assert(int_type);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
#ifdef __linux__
    /* glibc uses a forced native unwind for pthread_exit. */
    void *result = NULL;
    assert(!pthread_create(&threads[0], NULL, exit_worker, NULL));
    assert(!pthread_join(threads[0], &result));
    assert(result == (void *) (uintptr_t) 37 && exit_cleaned == 1);
#else
    (void) exit_worker;
#endif
    puts("PASS C error status: native/foreign payloads, nested/reentrant cleanup, errno and threads");
}
