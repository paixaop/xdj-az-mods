/* SPDX-License-Identifier: GPL-3.0-only */
#include "internal.h"
struct ta_allocation {
    struct ta_allocation *next;
    size_t size;
    max_align_t alignment;
};
_Noreturn void ta_fail(ta_context *c, int status, const char *message) {
    c->status = status;
    c->error = message;
    longjmp(c->failure, 1);
}
void ta_check(ta_context *c) {
    if (c->request && c->request->cancelled && c->request->cancelled(c->request->user))
        ta_fail(c, TA_CANCELLED, "analysis cancelled");
}
void *ta_alloc(ta_context *c, size_t count, size_t width) {
    ta_check(c);
    if (!width || count > (TA_MEMORY_LIMIT - sizeof(ta_allocation)) / width)
        ta_fail(c, TA_LIMIT, "analysis allocation limit");
    size_t size = count * width + sizeof(ta_allocation);
    if (size > TA_MEMORY_LIMIT - c->memory)
        ta_fail(c, TA_LIMIT, "analysis memory limit");
    if (c->budget) {
        size_t used = atomic_load_explicit(&c->budget->used, memory_order_relaxed);
        do {
            if (size > TA_MEMORY_LIMIT - used)
                ta_fail(c, TA_LIMIT, "analysis memory limit");
        } while (!atomic_compare_exchange_weak_explicit(&c->budget->used, &used, used + size,
                                                        memory_order_relaxed, memory_order_relaxed));
    }
    ta_allocation *p = ta_calloc(1, size);
    if (!p) {
        if (c->budget) atomic_fetch_sub_explicit(&c->budget->used, size, memory_order_relaxed);
        ta_fail(c, TA_LIMIT, "analysis allocation failed");
    }
    p->next = c->allocations;
    p->size = size;
    c->allocations = p;
    c->memory += size;
    return p + 1;
}
void ta_release(ta_context *c, void *pointer) {
    if (!pointer)
        return;
    ta_allocation **link = &c->allocations;
    while (*link && (void *)(*link + 1) != pointer)
        link = &(*link)->next;
    if (!*link)
        ta_fail(c, TA_FAILED, "invalid analysis allocation");
    ta_allocation *p = *link;
    *link = p->next;
    c->memory -= p->size;
    if (c->budget) atomic_fetch_sub_explicit(&c->budget->used, p->size, memory_order_relaxed);
    ta_free(p);
}
void ta_cleanup(ta_context *c) {
    if (c->decoder) {
        ta_decode_close(c->decoder);
        c->decoder = NULL;
    }
    while (c->allocations) {
        ta_allocation *p = c->allocations;
        c->allocations = p->next;
        ta_free(p);
    }
    if (c->budget) atomic_fetch_sub_explicit(&c->budget->used, c->memory, memory_order_relaxed);
    c->memory = 0;
}
void ta_share_budget(ta_context *c) {
    if (!c->budget) {
        atomic_init(&c->owned_budget.used, c->memory);
        c->budget = &c->owned_budget;
    }
}
/* Called only after joining. No allocation or recovery frame crosses threads. */
void ta_adopt(ta_context *parent, ta_context *child) {
    ta_allocation **tail = &child->allocations;
    while (*tail) tail = &(*tail)->next;
    *tail = parent->allocations;
    parent->allocations = child->allocations;
    parent->memory += child->memory;
    child->allocations = NULL;
    child->memory = 0;
}
