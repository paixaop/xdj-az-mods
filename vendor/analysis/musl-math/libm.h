/* SPDX-License-Identifier: MIT
 * Minimal private support for the retained musl 1.2.5 activation functions.
 * Helpers follow musl src/internal/libm.h; see COPYRIGHT. */
#ifndef TA_MUSL_LIBM_H
#define TA_MUSL_LIBM_H
#include <math.h>
#include <stdint.h>
#define hidden __attribute__((visibility("hidden")))
#define TOINT_INTRINSICS 0
#define predict_false(x) __builtin_expect(!!(x), 0)
#define asuint(f) ((union { float f_; uint32_t i_; }){.f_ = (f)}).i_
#define asuint64(f) ((union { double f_; uint64_t i_; }){.f_ = (f)}).i_
#define asdouble(i) ((union { uint64_t i_; double f_; }){.i_ = (i)}).f_
static inline float eval_as_float(float x) { return x; }
static inline double eval_as_double(double x) { return x; }
static inline float fp_barrierf(float x) { volatile float y = x; return y; }
#define FORCE_EVAL(x) do { volatile float y_ = (x); (void)y_; } while (0)
#define expf ta_musl_expf
#define expm1f ta_musl_expm1f
#define tanhf ta_musl_tanhf
#define __exp2f_data ta_musl_exp2f_data
#define __math_xflowf ta_musl_xflowf
#define __math_oflowf ta_musl_oflowf
#define __math_uflowf ta_musl_uflowf
hidden float expf(float);
hidden float expm1f(float);
hidden float tanhf(float);
hidden float __math_xflowf(uint32_t, float);
hidden float __math_oflowf(uint32_t);
hidden float __math_uflowf(uint32_t);
#endif
