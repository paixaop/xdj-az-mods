/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
float ta_musl_expf(float);
float ta_musl_expm1f(float);
float ta_musl_tanhf(float);
static const uint32_t reference[][4] = {
#include "fixtures/musl-activation-bits.inc"
};
int main(void) {
    float (*functions[])(float) = {ta_musl_expf, ta_musl_expm1f, ta_musl_tanhf};
    for (unsigned i = 0; i < sizeof reference / sizeof *reference; i++) {
        union { uint32_t bits; float value; } input = {.bits = reference[i][0]};
        for (unsigned fn = 0; fn < 3; fn++) {
            union { float value; uint32_t bits; } actual = {functions[fn](input.value)};
            if (actual.bits != reference[i][fn + 1])
                fprintf(stderr, "activation %u case %u input=%08x got=%08x expected=%08x\n",
                        fn, i, input.bits, actual.bits, reference[i][fn + 1]);
            assert(actual.bits == reference[i][fn + 1]);
        }
    }
    puts("12,288 activation values match the retained ARM64 runtime bit for bit");
}
