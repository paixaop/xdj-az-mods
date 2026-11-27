/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TA_INTERNAL_H
#define TA_INTERNAL_H
#include "PortableDSP.h"
#include "allocator.h"
#include "analysis.h"
#include "model-format.h"
#include <float.h>
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TA_MEMORY_LIMIT ((size_t)512 * 1024 * 1024)
typedef struct ta_allocation ta_allocation;
typedef struct { atomic_size_t used; } ta_budget;
typedef struct ta_context {
    jmp_buf failure;
    ta_allocation *allocations;
    size_t memory;
    const ta_request *request;
    ta_decoder *decoder;
    int status;
    const char *error;
    ta_execution execution;
    ta_budget owned_budget, *budget;
    struct timespec profile_previous;
    bool phrase_profile;
    bool windows_model;
} ta_context;
/* Every allocation belongs to one analysis and is released on success, failure,
 * or cancellation. The recovery boundary is on this same C thread. */
_Noreturn void ta_fail(ta_context *c, int status, const char *message);
void ta_check(ta_context *c);
void *ta_alloc(ta_context *c, size_t count, size_t width);
void ta_release(ta_context *c, void *pointer);
void ta_cleanup(ta_context *c);
void ta_share_budget(ta_context *c);
void ta_adopt(ta_context *parent, ta_context *child);
typedef void (*ta_parallel_fn)(ta_context *c, void *job, size_t index, unsigned worker);
void ta_parallel(ta_context *c, size_t count, unsigned threads, ta_parallel_fn body, void *job);
void ta_pair(ta_context *c, ta_parallel_fn body, void *job);
void ta_profile_begin(ta_context *c);
void ta_profile_mark(ta_context *c, const char *stage);
static inline unsigned ta_phrase_threads(const ta_context *c) {
    return c->execution.phrase_threads ? c->execution.phrase_threads : 1;
}
typedef struct {
    double *v;
    int n;
} ta_doubles;
typedef struct {
    float *v;
    int n;
} ta_floats;
typedef struct {
    int *v;
    int n;
} ta_ints;
typedef struct {
    uint8_t *v;
    int n;
} ta_bytes;
static inline int ta_min(int a, int b) { return a < b ? a : b; }
static inline int ta_max(int a, int b) { return a > b ? a : b; }
static inline double ta_dmin(double a, double b) { return b < a ? b : a; }
static inline double ta_dmax(double a, double b) { return a < b ? b : a; }
static inline ta_doubles ta_new_doubles(ta_context *c, int n) {
    if (n < 0)
        ta_fail(c, TA_FAILED, "negative array extent");
    return (ta_doubles){ta_alloc(c, (size_t)n, sizeof(double)), n};
}
static inline ta_ints ta_new_ints(ta_context *c, int n) {
    if (n < 0)
        ta_fail(c, TA_FAILED, "negative array extent");
    return (ta_ints){ta_alloc(c, (size_t)n, sizeof(int)), n};
}
static inline ta_bytes ta_new_bytes(ta_context *c, int n) {
    if (n < 0)
        ta_fail(c, TA_FAILED, "negative array extent");
    return (ta_bytes){ta_alloc(c, (size_t)n, 1), n};
}
static inline uint32_t ta_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint32_t ta_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline unsigned ta_be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static inline void ta_put32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
static inline void ta_put16(uint8_t *p, unsigned n) {
    p[0] = n >> 8;
    p[1] = n;
}
uint8_t *ta_model_open(ta_context *c);
void ta_model_phrase_parameters(ta_context *c, double parameters[TA_PHRASE_PARAMETER_COUNT]);
az_analysis_vocal_map_v1 ta_vocals(ta_context *c);
#endif
